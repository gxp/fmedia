/** fmedia track.
Copyright (c) 2016 Simon Zolin */

#include <core.h>

#include <FF/data/conf.h>
#include <FF/data/utf8.h>
#include <FF/crc.h>
#include <FF/path.h>
#include <FF/time.h>
#include <FF/sys/filemap.h>
#include <FFOS/error.h>
#include <FFOS/process.h>
#include <FFOS/timer.h>


typedef struct fmed_f {
	fflist_item sib;
	void *ctx;
	struct {
		size_t datalen;
		const char *data;

		size_t outlen;
		const char *out;
	} d;
	const char *name;
	const fmed_filter *filt;
	fftime clk;
	unsigned opened :1
		, noskip :1;
} fmed_f;

typedef struct dict_ent {
	ffrbt_node nod;
	const char *name;
	union {
		int64 val;
		void *pval;
	};
	uint acq :1;
} dict_ent;

enum TRK_ST {
	TRK_ST_STOPPED,
	TRK_ST_ACTIVE,
	TRK_ST_PAUSED,
	TRK_ST_ERR,
};

typedef struct fm_trk {
	fflist_item sib;
	fmed_trk props;
	ffchain filt_chain;
	struct {FFARR(fmed_f)} filters;
	fflist_cursor cur;
	ffrbtree dict;
	ffrbtree meta;
	fftime tstart;

	ffstr id;
	char sid[FFSLEN("*") + FFINT_MAXCHARS];

	uint state; //enum TRK_ST
} fm_trk;


static int trk_setout(fm_trk *t);
static int trk_opened(fm_trk *t);
static fmed_f* addfilter(fm_trk *t, const char *modname);
static fmed_f* addfilter1(fm_trk *t, const fmed_modinfo *mod);
static int trk_open(fm_trk *t, const char *fn);
static void trk_open_capt(fm_trk *t);
static void trk_free(fm_trk *t);
static void trk_process(void *udata);
static void trk_stop(fm_trk *t, uint flags);
static fmed_f* trk_modbyext(fm_trk *t, const ffstr3 *map, const ffstr *ext);
static void trk_printtime(fm_trk *t);

static dict_ent* dict_add(fm_trk *t, const char *name, uint *f);
static void dict_ent_free(dict_ent *e);

// TRACK
static void* trk_create(uint cmd, const char *url);
static fmed_trk* trk_conf(void *trk);
static void trk_copy_info(fmed_trk *dst, const fmed_trk *src);
static int trk_cmd(void *trk, uint cmd);
static void trk_loginfo(void *trk, const ffstr **id, const char **module);
static int64 trk_popval(void *trk, const char *name);
static int64 trk_getval(void *trk, const char *name);
static const char* trk_getvalstr(void *trk, const char *name);
static int trk_setval(void *trk, const char *name, int64 val);
static int trk_setvalstr(void *trk, const char *name, const char *val);
static int64 trk_setval4(void *trk, const char *name, int64 val, uint flags);
static char* trk_setvalstr4(void *trk, const char *name, const char *val, uint flags);
static char* trk_getvalstr3(void *trk, const void *name, uint flags);
const fmed_track _fmed_track = {
	&trk_create, &trk_conf, &trk_copy_info, &trk_cmd,
	&trk_popval, &trk_getval, &trk_getvalstr, &trk_setval, &trk_setvalstr, &trk_setval4, &trk_setvalstr4, &trk_getvalstr3,
	&trk_loginfo,
};


static fmed_f* _addfilter1(fm_trk *t)
{
	if (NULL == ffarr_grow(&t->filters, 1, 4))
		return NULL;
	fmed_f *f = ffarr_push(&t->filters, fmed_f);
	ffmem_tzero(f);
	return f;
}

static fmed_f* addfilter1(fm_trk *t, const fmed_modinfo *mod)
{
	fmed_f *f = _addfilter1(t);
	f->name = mod->name;
	f->filt = mod->f;
	return f;
}

static fmed_f* addfilter(fm_trk *t, const char *modname)
{
	fmed_f *f = _addfilter1(t);
	f->name = modname;
	f->filt = core->getmod(modname);
	return f;
}

static fmed_f* trk_modbyext(fm_trk *t, const ffstr3 *map, const ffstr *ext)
{
	const fmed_modinfo *mi = core_modbyext(map, ext);
	if (mi == NULL)
		return NULL;
	return addfilter1(t, mi);
}

static int trk_open(fm_trk *t, const char *fn)
{
	ffstr ext;
	fffileinfo fi;

	trk_setvalstr(t, "input", fn);
	addfilter(t, "#queue.track");

	if (0 == fffile_infofn(fn, &fi) && fffile_isdir(fffile_infoattr(&fi))) {
		addfilter(t, "plist.dir");
		return 0;
	}

	if (ffs_match(fn, ffsz_len(fn), "http", 4)) {
		addfilter(t, "net.icy");
		ffstr_setz(&ext, "mp3");
		if (NULL == trk_modbyext(t, &fmed->conf.inmap, &ext))
			return 1;
	} else {
		addfilter(t, "#file.in");

		ffpath_splitname(fn, ffsz_len(fn), NULL, &ext);
		if (NULL == trk_modbyext(t, &fmed->conf.inmap, &ext))
			return 1;
	}

	addfilter(t, "#soundmod.until");
	return 0;
}

static void trk_open_capt(fm_trk *t)
{
	ffpcm_fmtcopy(&t->props.audio.fmt, &fmed->conf.inp_pcm);

	if (fmed->conf.inp_pcm.channels & ~FFPCM_CHMASK) {
		trk_setval(t, "conv_channels", fmed->conf.inp_pcm.channels);
		t->props.audio.fmt.channels = fmed->conf.inp_pcm.channels & FFPCM_CHMASK;
	}

	addfilter1(t, fmed->conf.input);

	addfilter(t, "#soundmod.until");
	addfilter(t, "#soundmod.rtpeak");
}

static int trk_setout(fm_trk *t)
{
	const char *s, *fn;

	if (t->props.type == FMED_TRK_TYPE_NETIN) {
		ffstr ext;
		const char *input = trk_getvalstr(t, "input");
		ffpath_splitname(input, ffsz_len(input), NULL, &ext);
		if (NULL == trk_modbyext(t, &fmed->conf.inmap, &ext))
			return -1;

	} else if (t->props.type != FMED_TRK_TYPE_MIXIN) {
		if (fmed->cmd.gui)
			addfilter(t, "gui.gui");
		else if (!fmed->cmd.notui)
			addfilter(t, "#tui.tui");
	}

	if (t->props.type != FMED_TRK_TYPE_MIXOUT) {
		addfilter(t, "#soundmod.gain");
	}

	addfilter(t, "#soundmod.conv");
	addfilter(t, "#soundmod.conv-soxr");

	if (t->props.type == FMED_TRK_TYPE_MIXIN) {
		addfilter(t, "mixer.in");

	} else if (fmed->cmd.pcm_peaks) {
		addfilter(t, "#soundmod.peaks");

	} else if (FMED_PNULL != (s = trk_getvalstr(t, "output"))) {
		ffstr name, ext;
		ffpath_splitname(s, ffsz_len(s), &name, &ext);
		if (NULL == trk_modbyext(t, &fmed->conf.outmap, &ext))
			return -1;
		addfilter(t, "#file.out");

	} else if (fmed->cmd.outfn.len != 0 && !fmed->cmd.rec) {
		ffstr name, ext;
		ffs_rsplit2by(fmed->cmd.outfn.ptr, fmed->cmd.outfn.len, '.', &name, &ext);

		if (name.len != 0)
			trk_setvalstr(t, "output", fmed->cmd.outfn.ptr);

		else if (FMED_PNULL != (fn = trk_getvalstr(t, "input"))) {

			ffstr fname;
			ffstr3 outfn = {0};

			ffpath_split2(fn, ffsz_len(fn), NULL, &fname);
			ffpath_splitname(fname.ptr, fname.len, &name, NULL);

			if (0 == ffstr_catfmt(&outfn, "%S/%S.%S%Z"
				, &fmed->cmd.outdir, &name, &ext))
				return -1;
			trk_setvalstr4(t, "output", outfn.ptr, FMED_TRK_FACQUIRE);

		} else {
			errlog(core, t, "core", "--out must be set");
			return -1;
		}

		if (fmed->cmd.out_copy) {
			trk_setval(t, "out-copy", 1);
			if (fmed->conf.output != NULL)
				addfilter1(t, fmed->conf.output);
			return 0;
		}

		if (fmed->cmd.stream_copy)
			trk_setval(t, "stream_copy", 1);

		if (NULL == trk_modbyext(t, &fmed->conf.outmap, &ext))
			return -1;

		addfilter(t, "#file.out");

	} else if (fmed->conf.output != NULL) {
		addfilter1(t, fmed->conf.output);
	}

	return 0;
}

static int trk_opened(fm_trk *t)
{
	fmed_f *f;
	FFARR_WALK(&t->filters, f) {
		ffchain_add(&t->filt_chain, &f->sib);
	}

	fflist_ins(&fmed->trks, &t->sib);
	t->state = TRK_ST_ACTIVE;
	return 0;
}

/*
Example of a typical chain:
 #queue.track
 -> INPUT
 -> DECODER -> (#soundmod.until) -> UI -> #soundmod.gain -> (#soundmod.conv/conv-soxr) -> (ENCODER)
 -> OUTPUT
*/
static void* trk_create(uint cmd, const char *fn)
{
	fm_trk *t = ffmem_tcalloc1(fm_trk);
	if (t == NULL)
		return NULL;
	ffchain_init(&t->filt_chain);
	ffrbt_init(&t->dict);
	ffrbt_init(&t->meta);

	trk_copy_info(&t->props, NULL);
	t->props.track = &_fmed_track;
	t->props.handler = &trk_process;
	t->props.trk = t;

	t->id.len = ffs_fmt(t->sid, t->sid + sizeof(t->sid), "*%u", ++fmed->trkid);
	t->id.ptr = t->sid;

	switch (cmd) {
	case FMED_TRACK_OPEN:
		if (0 != trk_open(t, fn)) {
			trk_free(t);
			return FMED_TRK_EFMT;
		}
		break;

	case FMED_TRACK_REC:
		trk_open_capt(t);
		t->props.type = FMED_TRK_TYPE_REC;
		break;

	case FMED_TRACK_MIX:
		addfilter(t, "#queue.track");
		addfilter(t, "mixer.out");
		t->props.type = FMED_TRK_TYPE_MIXOUT;
		break;

	case FMED_TRACK_NET:
		addfilter(t, "net.in");
		t->props.type = FMED_TRK_TYPE_NETIN;
		break;
	}

	if (fmed->cmd.meta.len != 0)
		trk_setvalstr(t, "meta", fmed->cmd.meta.ptr);
	return t;
}

static fmed_trk* trk_conf(void *trk)
{
	fm_trk *t = trk;
	return &t->props;
}

static void trk_copy_info(fmed_trk *dst, const fmed_trk *src)
{
	if (src == NULL) {
		ffmem_tzero(dst);
		memset(&dst->audio, 0xff, sizeof(dst->audio)); //FMED_NULL
		memset(&dst->input, 0xff, sizeof(dst->input));
		memset(&dst->output, 0xff, sizeof(dst->output));
		return;
	}
	ffmemcpy(&dst->audio, &src->audio, sizeof(dst->audio));
	ffmemcpy(&dst->input, &src->input, sizeof(dst->input));
	ffmemcpy(&dst->output, &src->output, sizeof(dst->output));
	dst->bits = src->bits;
}

static void trk_stop(fm_trk *t, uint flags)
{
	trk_setval(t, "stopped", flags);
	t->props.flags |= FMED_FSTOP;
	if (t->state != TRK_ST_ACTIVE)
		trk_free(t);
}

static void trk_printtime(fm_trk *t)
{
	fmed_f *pf;
	ffstr3 s = {0};
	fftime all = {0};

	FFARR_WALK(&t->filters, pf) {
		fftime_add(&all, &pf->clk);
	}
	if (all.s == 0 && all.mcs == 0)
		return;
	ffstr_catfmt(&s, "time: %u.%06u.  ", all.s, all.mcs);

	FFARR_WALK(&t->filters, pf) {
		ffstr_catfmt(&s, "%s: %u.%06u (%u%%), "
			, pf->name, pf->clk.s, pf->clk.mcs, fftime_mcs(&pf->clk) * 100 / fftime_mcs(&all));
	}
	if (s.len > FFSLEN(", "))
		s.len -= FFSLEN(", ");

	dbglog(core, t, "core", "%S", &s);
	ffarr_free(&s);
}

static void dict_ent_free(dict_ent *e)
{
	if (e->acq)
		ffmem_free(e->pval);
	ffmem_free(e);
}

static void trk_free(fm_trk *t)
{
	fmed_f *pf;
	dict_ent *e;
	fftree_node *node, *next;
	int type = t->props.type;

	if (fmed->cmd.print_time) {
		fftime t2;
		ffclk_get(&t2);
		ffclk_diff(&t->tstart, &t2);
		core->log(FMED_LOG_INFO, t, "core", "track processing time: %u.%06u", t2.s, t2.mcs);
	}

	dbglog(core, t, "core", "media: closing...");
	FFARR_WALK(&t->filters, pf) {
		if (pf->ctx != NULL) {
			t->cur = &pf->sib;
			pf->filt->close(pf->ctx);
		}
	}

	if (core->loglev == FMED_LOG_DEBUG)
		trk_printtime(t);

	FFTREE_WALKSAFE(&t->dict, node, next) {
		e = FF_GETPTR(dict_ent, nod, node);
		ffrbt_rm(&t->dict, &e->nod);
		dict_ent_free(e);
	}

	FFTREE_WALKSAFE(&t->meta, node, next) {
		e = FF_GETPTR(dict_ent, nod, node);
		ffrbt_rm(&t->meta, &e->nod);
		dict_ent_free(e);
	}

	if (fflist_exists(&fmed->trks, &t->sib))
		fflist_rm(&fmed->trks, &t->sib);

	dbglog(core, t, "core", "media: closed");
	ffmem_free(t);

	if (type == FMED_TRK_TYPE_REC && !fmed->cmd.gui)
		core->sig(FMED_STOP);
}

#ifdef _DEBUG
// enum FMED_R
static const char *const fmed_retstr[] = {
	"err", "ok", "data", "more", "async", "done", "done-prev", "last-out", "fin", "syserr",
};
#endif

static void trk_process(void *udata)
{
	fm_trk *t = udata;
	fmed_f *nf;
	fmed_f *f;
	int r = FFLIST_CUR_NEXT, e;
	fftime t1, t2;

	if (t->cur == NULL) {
		t->cur = &t->filters.ptr->sib;
		f = FF_GETPTR(fmed_f, sib, t->cur);
		goto next;
	}

	for (;;) {

		if (t->state != TRK_ST_ACTIVE) {
			if (t->state == TRK_ST_ERR)
				goto fin;
			return;
		}

		f = FF_GETPTR(fmed_f, sib, t->cur);

#ifdef _DEBUG
		dbglog(core, t, "core", "%s calling %s, input: %L"
			, (r == FFLIST_CUR_NEXT) ? ">>" : "<<", f->name, f->d.datalen);
#endif
		if (core->loglev == FMED_LOG_DEBUG) {
			ffclk_get(&t1);
		}

		if (t->cur->prev == ffchain_sentl(&t->filt_chain))
			t->props.flags |= FMED_FLAST;
		else
			t->props.flags &= ~FMED_FLAST;

		t->props.data = f->d.data,  t->props.datalen = f->d.datalen;
		t->props.out = f->d.out,  t->props.outlen = f->d.outlen;
		e = f->filt->process(f->ctx, &t->props);
		f->d.data = t->props.data,  f->d.datalen = t->props.datalen;
		f->d.out = t->props.out,  f->d.outlen = t->props.outlen;

		if (core->loglev == FMED_LOG_DEBUG) {
			ffclk_get(&t2);
			ffclk_diff(&t1, &t2);
			fftime_add(&f->clk, &t2);
		}

#ifdef _DEBUG
		dbglog(core, t, "core", "%s returned: %s, output: %L"
			, f->name, ((uint)(e + 1) < FFCNT(fmed_retstr)) ? fmed_retstr[e + 1] : "", f->d.outlen);
#endif

		switch (e) {
		case FMED_RSYSERR:
			syserrlog(core, t, f->name, "%s", "system error");
			// break
		case FMED_RERR:
			t->state = TRK_ST_ERR;
			goto fin;

		case FMED_RASYNC:
			return;

		case FMED_RMORE:
			r = FFLIST_CUR_PREV;
			break;

		case FMED_ROK:
			r = FFLIST_CUR_NEXT;
			break;

		case FMED_RDATA:
			f->noskip = 1;
			r = FFLIST_CUR_NEXT | FFLIST_CUR_SAMEIFBOUNCE;
			break;

		case FMED_RDONE:
			f->d.datalen = 0;
			r = FFLIST_CUR_NEXT | FFLIST_CUR_RM;
			break;

		case FMED_RDONE_PREV:
			f->d.datalen = 0;
			r = FFLIST_CUR_PREV | FFLIST_CUR_RM;
			break;

		case FMED_RLASTOUT:
			f->d.datalen = 0;
			r = FFLIST_CUR_NEXT | FFLIST_CUR_RM | FFLIST_CUR_RMPREV;
			break;

		case FMED_RFIN:
			goto fin;

		default:
			errlog(core, t, "core", "unknown return code from module: %u", e);
			t->state = TRK_ST_ERR;
			goto fin;
		}

		if (f->d.datalen != 0)
			r |= FFLIST_CUR_SAMEIFBOUNCE;

shift:
		r = fflist_curshift(&t->cur, r | FFLIST_CUR_BOUNCE, ffchain_sentl(&t->filt_chain));

		switch (r) {
		case FFLIST_CUR_NONEXT:
			goto fin; //done

		case FFLIST_CUR_NOPREV:
			errlog(core, t, "core", "module %s requires more input data", f->name);
			t->state = TRK_ST_ERR;
			goto fin;

		case FFLIST_CUR_NEXT:
next:
			nf = FF_GETPTR(fmed_f, sib, t->cur);
			nf->d.data = f->d.out;
			nf->d.datalen = f->d.outlen;

			if (!nf->opened) {
				if (t->props.flags & FMED_FSTOP)
					goto fin; // calling the rest of the chain is pointless

				dbglog(core, t, "core", "creating context for %s...", nf->name);
				nf->ctx = nf->filt->open(&t->props);
				if (nf->ctx == NULL) {
					t->state = TRK_ST_ERR;
					goto fin;

				} else if (nf->ctx == FMED_FILT_SKIP) {
					dbglog(core, t, "core", "%s is skipped", nf->name);
					nf->ctx = NULL; //don't call fmed_filter.close()
					nf->d.out = f->d.out;
					nf->d.outlen = f->d.outlen;
					f = nf;
					r = FFLIST_CUR_NEXT | FFLIST_CUR_RM;
					goto shift;
				}

				dbglog(core, t, "core", "context for %s created", nf->name);
				nf->opened = 1;
			}
			break;

		case FFLIST_CUR_SAME:
		case FFLIST_CUR_PREV:
			nf = FF_GETPTR(fmed_f, sib, t->cur);
			if (nf->noskip) {
				nf->noskip = 0;
			} else if (nf->d.datalen == 0 && &nf->sib != ffchain_first(&t->filt_chain)) {
				r = FFLIST_CUR_PREV;
				f = nf;
				goto shift;
			}
			break;

		default:
			FF_ASSERT(0);
		}
	}

	return;

fin:
	if (t->state == TRK_ST_ERR)
		trk_setval(t, "error", 1);

	trk_free(t);
}


static dict_ent* dict_findstr(fm_trk *t, const ffstr *name)
{
	dict_ent *ent;
	uint crc = ffcrc32_get(name->ptr, name->len);
	ffrbt_node *nod;

	nod = ffrbt_find(&t->dict, crc, NULL);
	if (nod == NULL)
		return NULL;
	ent = (dict_ent*)nod;

	if (!ffstr_eqz(name, ent->name))
		return NULL;

	return ent;
}

static dict_ent* dict_find(fm_trk *t, const char *name)
{
	ffstr s;
	ffstr_setz(&s, name);
	return dict_findstr(t, &s);
}

static dict_ent* dict_add(fm_trk *t, const char *name, uint *f)
{
	dict_ent *ent;
	uint crc = ffcrc32_getz(name, 0);
	ffrbt_node *nod, *parent;
	ffrbtree *tree = (*f & FMED_TRK_META) ? &t->meta : &t->dict;

	nod = ffrbt_find(tree, crc, &parent);
	if (nod != NULL) {
		ent = (dict_ent*)nod;
		if (0 != ffsz_cmp(name, ent->name)) {
			errlog(core, NULL, "core", "setval: CRC collision: %u, key: %s, with key: %s"
				, crc, name, ent->name);
			t->state = TRK_ST_ERR;
			return NULL;
		}
		*f = 1;

	} else {
		ent = ffmem_tcalloc1(dict_ent);
		if (ent == NULL) {
			errlog(core, NULL, "core", "setval: %e", FFERR_BUFALOC);
			t->state = TRK_ST_ERR;
			return NULL;
		}
		ent->nod.key = crc;
		ffrbt_insert(tree, &ent->nod, parent);
		ent->name = name;
		*f = 0;
	}

	return ent;
}

static dict_ent* meta_find(fm_trk *t, const ffstr *name)
{
	dict_ent *ent;
	uint crc = ffcrc32_get(name->ptr, name->len);
	ffrbt_node *nod;

	nod = ffrbt_find(&t->meta, crc, NULL);
	if (nod == NULL)
		return NULL;
	ent = (dict_ent*)nod;

	if (!ffstr_eqz(name, ent->name))
		return NULL;

	return ent;
}


static int trk_cmd(void *trk, uint cmd)
{
	fm_trk *t = trk;
	fflist_item *next;

	dbglog(core, NULL, "track", "received command:%u, trk:%p", cmd, trk);

	switch (cmd) {
	case FMED_TRACK_STOPALL_EXIT:
		if (fmed->trks.len == 0 || fmed->stopped) {
			core->sig(FMED_STOP);
			break;
		}
		fmed->stopped = 1;
		trk = (void*)-1;
		// break

	case FMED_TRACK_STOPALL:
		FFLIST_WALKSAFE(&fmed->trks, t, sib, next) {
			if (t->props.type == FMED_TRK_TYPE_REC && trk == NULL)
				continue;

			trk_stop(t, cmd);
		}
		break;

	case FMED_TRACK_STOP:
		trk_stop(t, FMED_TRACK_STOP);
		break;

	case FMED_TRACK_START:
		if (0 != trk_setout(t)) {
			trk_setval(t, "error", 1);
		}
		if (0 != trk_opened(t)) {
			trk_free(t);
			return -1;
		}

		if (fmed->cmd.print_time)
			ffclk_get(&t->tstart);

		trk_process(t);
		break;

	case FMED_TRACK_PAUSE:
		t->state = TRK_ST_PAUSED;
		break;
	case FMED_TRACK_UNPAUSE:
		t->state = TRK_ST_ACTIVE;
		trk_process(t);
		break;
	}
	return 0;
}

static void trk_loginfo(void *trk, const ffstr **id, const char **module)
{
	fm_trk *t = trk;
	*id = &t->id;
	*module = NULL;
	if (t->cur != NULL) {
		const fmed_f *f = FF_GETPTR(fmed_f, sib, t->cur);
		*module = f->name;
	}
}

static int64 trk_popval(void *trk, const char *name)
{
	fm_trk *t = trk;
	dict_ent *ent = dict_find(t, name);
	if (ent != NULL) {
		int64 val = ent->val;
		ffrbt_rm(&t->dict, &ent->nod);
		dict_ent_free(ent);
		return val;
	}

	return FMED_NULL;
}

static int64 trk_getval(void *trk, const char *name)
{
	fm_trk *t = trk;
	dict_ent *ent = dict_find(t, name);
	if (ent != NULL)
		return ent->val;
	return FMED_NULL;
}

static const char* trk_getvalstr(void *trk, const char *name)
{
	fm_trk *t = trk;
	dict_ent *ent = dict_find(t, name);
	if (ent != NULL)
		return ent->pval;
	return FMED_PNULL;
}

static char* trk_getvalstr3(void *trk, const void *name, uint flags)
{
	fm_trk *t = trk;
	dict_ent *ent;
	ffstr nm;

	if (flags & FMED_TRK_NAMESTR)
		ffstr_set2(&nm, (ffstr*)name);
	else
		ffstr_setz(&nm, (char*)name);

	if (flags & FMED_TRK_META) {
		ent = meta_find(t, &nm);
		if (ent == NULL) {
			void *qent;
			if (NULL == (qent = (void*)trk_getval(t, "queue_item")))
				return FMED_PNULL;
			const fmed_queue *qu = core->getmod("#queue.queue");
			ffstr *val;
			if (NULL == (val = qu->meta_find(qent, nm.ptr, nm.len)))
				return FMED_PNULL;
			return val->ptr;
		}
	} else
		ent = dict_findstr(t, &nm);
	if (ent == NULL)
		return FMED_PNULL;

	return ent->pval;
}

static int trk_setval(void *trk, const char *name, int64 val)
{
	trk_setval4(trk, name, val, 0);
	return 0;
}

static int64 trk_setval4(void *trk, const char *name, int64 val, uint flags)
{
	fm_trk *t = trk;
	uint st = 0;
	dict_ent *ent = dict_add(t, name, &st);
	if (ent == NULL)
		return FMED_NULL;

	if ((flags & FMED_TRK_FNO_OVWRITE) && st == 1)
		return ent->val;

	if (ent->acq) {
		ffmem_free(ent->pval);
		ent->acq = 0;
	}

	ent->val = val;
	dbglog(core, trk, "core", "setval: %s = %D", name, val);
	return val;
}

static char* trk_setvalstr4(void *trk, const char *name, const char *val, uint flags)
{
	fm_trk *t = trk;
	dict_ent *ent;
	uint st = flags;

	if (flags & FMED_TRK_META) {
		ent = dict_add(t, name, &st);
		if (ent == NULL)
			return NULL;

		if (ent->acq)
			ffmem_free(ent->pval);

		if (flags & FMED_TRK_VALSTR) {
			const ffstr *sval = (void*)val;
			ent->pval = ffsz_alcopy(sval->ptr, sval->len);
		} else
			ent->pval = ffsz_alcopyz(val);

		if (ent->pval == NULL)
			return NULL;

		ent->acq = 1;
		dbglog(core, trk, "core", "set meta: %s = %s", name, ent->pval);
		return ent->pval;

	} else
		ent = dict_add(t, name, &st);

	if (ent == NULL
		|| ((flags & FMED_TRK_FNO_OVWRITE) && st == 1)) {

		if (flags & FMED_TRK_FACQUIRE)
			ffmem_free((char*)val);
		return (ent != NULL) ? ent->pval : NULL;
	}

	if (ent->acq)
		ffmem_free(ent->pval);
	ent->acq = (flags & FMED_TRK_FACQUIRE) ? 1 : 0;

	ent->pval = (void*)val;

	dbglog(core, trk, "core", "setval: %s = %s", name, val);
	return ent->pval;
}

static int trk_setvalstr(void *trk, const char *name, const char *val)
{
	trk_setvalstr4(trk, name, val, 0);
	return 0;
}