/** fmedia terminal startup.
Copyright (c) 2015 Simon Zolin */

#include <core-cmd.h>

#include <FF/audio/pcm.h>
#include <FF/data/psarg.h>
#include <FF/data/conf.h>
#include <FF/data/utf8.h>
#include <FF/sys/dir.h>
#include <FF/path.h>
#include <FF/time.h>
#include <FFOS/sig.h>
#include <FFOS/error.h>
#include <FFOS/process.h>


#define FMED_CMDHELP_FILE  "help.txt"

struct gctx {
	ffsignal sigs_task;
	fmed_cmd *cmd;

	ffdl core_dl;
	fmed_core* (*core_init)(fmed_cmd **ptr, char **argv, char **env);
	void (*core_free)(void);
};
static struct gctx *g;
static fmed_core *core;

static int fmed_cmdline(int argc, char **argv, uint main_only);
static int fmed_arg_usage(void);
static int fmed_arg_skip(ffparser_schem *p, void *obj, const ffstr *val);
static int fmed_arg_infile(ffparser_schem *p, void *obj, const ffstr *val);
static int fmed_arg_listdev(void);
static int fmed_arg_seek(ffparser_schem *p, void *obj, const ffstr *val);
static int fmed_arg_install(ffparser_schem *p, void *obj, const ffstr *val);
static int fmed_arg_channels(ffparser_schem *p, void *obj, ffstr *val);
static int fmed_arg_format(ffparser_schem *p, void *obj, ffstr *val);
static int fmed_arg_out_copy(ffparser_schem *p, void *obj, const ffstr *val);
static int fmed_arg_input_chk(ffparser_schem *p, void *obj, const ffstr *val);
static int fmed_arg_out_chk(ffparser_schem *p, void *obj, const ffstr *val);

static void open_input(void *udata);
static void fmed_onsig(void *udata);
static void rec_lpback_new_track(fmed_cmd *cmd);

//LOG
static void std_log(uint flags, fmed_logdata *ld);
static const fmed_log std_logger = {
	&std_log
};


#define OFF(member)  FFPARS_DSTOFF(fmed_cmd, member)

static const ffpars_arg fmed_cmdline_args[] = {
	{ "",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY | FFPARS_FSTRZ | FFPARS_FMULTI,  FFPARS_DST(&fmed_arg_infile) },

	//QUEUE
	{ "repeat-all",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(repeat_all) },
	{ "track",	FFPARS_TCHARPTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY | FFPARS_FSTRZ,  OFF(trackno) },

	//AUDIO DEVICES
	{ "list-dev",	FFPARS_TBOOL | FFPARS_FALONE,  FFPARS_DST(&fmed_arg_listdev) },
	{ "dev",	FFPARS_TINT,  OFF(playdev_name) },
	{ "dev-capture",	FFPARS_TINT,  OFF(captdev_name) },
	{ "dev-loopback",	FFPARS_TINT,  OFF(lbdev_name) },

	//AUDIO FORMAT
	{ "format",	FFPARS_TSTR | FFPARS_FNOTEMPTY,  FFPARS_DST(&fmed_arg_format) },
	{ "rate",	FFPARS_TINT,  OFF(out_rate) },
	{ "channels",	FFPARS_TSTR | FFPARS_FNOTEMPTY,  FFPARS_DST(&fmed_arg_channels) },

	//INPUT
	{ "record",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(rec) },
	{ "mix",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(mix) },
	{ "seek",	FFPARS_TSTR | FFPARS_FNOTEMPTY,  FFPARS_DST(&fmed_arg_seek) },
	{ "until",	FFPARS_TSTR | FFPARS_FNOTEMPTY,  FFPARS_DST(&fmed_arg_seek) },
	{ "fseek",	FFPARS_TINT | FFPARS_F64BIT,  OFF(fseek) },
	{ "info",	FFPARS_SETVAL('i') | FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(info) },
	{ "tags",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(tags) },
	{ "meta",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FSTRZ,  OFF(meta) },

	//FILTERS
	{ "volume",	FFPARS_TINT8,  OFF(volume) },
	{ "gain",	FFPARS_TFLOAT | FFPARS_FSIGN,  OFF(gain) },
	{ "dynanorm",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(dynanorm) },
	{ "pcm-peaks",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(pcm_peaks) },
	{ "pcm-crc",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(pcm_crc) },

	//ENCODING
	{ "vorbis.quality",	FFPARS_TFLOAT | FFPARS_FSIGN,  OFF(vorbis_qual) },
	{ "opus.bitrate",	FFPARS_TINT,  OFF(opus_brate) },
	{ "mpeg-quality",	FFPARS_TINT | FFPARS_F16BIT,  OFF(mpeg_qual) },
	{ "aac-quality",	FFPARS_TINT,  OFF(aac_qual) },
	{ "aac-profile",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY,  OFF(aac_profile) },
	{ "flac-compression",	FFPARS_TINT8,  OFF(flac_complevel) },
	{ "stream-copy",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(stream_copy) },

	//OUTPUT
	{ "out",	FFPARS_SETVAL('o') | FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY | FFPARS_FSTRZ,  OFF(outfn) },
	{ "overwrite",	FFPARS_SETVAL('y') | FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(overwrite) },
	{ "out-copy",	FFPARS_TSTR | FFPARS_FALONE,  FFPARS_DST(&fmed_arg_out_copy) },
	{ "preserve-date",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(preserve_date) },

	//OTHER OPTIONS
	{ "background",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(bground) },
#ifdef FF_WIN
	{ "background-child",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(bgchild) },
#endif
	{ "globcmd",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY,  OFF(globcmd) },
	{ "globcmd.pipe-name",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY,  OFF(globcmd_pipename) },
	{ "conf",	FFPARS_TSTR,  OFF(dummy) },
	{ "notui",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(notui) },
	{ "gui",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(gui) },
	{ "print-time",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(print_time) },
	{ "debug",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(debug) },
	{ "help",	FFPARS_SETVAL('h') | FFPARS_TBOOL | FFPARS_FALONE,  FFPARS_DST(&fmed_arg_usage) },
	{ "cue-gaps",	FFPARS_TINT8,  OFF(cue_gaps) },

	//INSTALL
	{ "install",	FFPARS_TBOOL | FFPARS_FALONE,  FFPARS_DST(&fmed_arg_install) },
	{ "uninstall",	FFPARS_TBOOL | FFPARS_FALONE,  FFPARS_DST(&fmed_arg_install) },
};

static const ffpars_arg fmed_cmdline_main_args[] = {
	{ "",	FFPARS_TSTR | FFPARS_FMULTI,  FFPARS_DST(&fmed_arg_input_chk) },
	{ "*",	FFPARS_TSTR | FFPARS_FMULTI,  FFPARS_DST(&fmed_arg_skip) },
	{ "out",	FFPARS_SETVAL('o') | FFPARS_TSTR,  FFPARS_DST(&fmed_arg_out_chk) },
	{ "conf",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY,  OFF(conf_fn) },
	{ "notui",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(notui) },
	{ "gui",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(gui) },
	{ "debug",	FFPARS_TBOOL8 | FFPARS_FALONE,  OFF(debug) },
	{ "help",	FFPARS_SETVAL('h') | FFPARS_TBOOL | FFPARS_FALONE,  FFPARS_DST(&fmed_arg_usage) },
};

#undef OFF

static int fmed_arg_usage(void)
{
	ffarr buf = {0};
	ssize_t n;
	char *fn = NULL;
	fffd f = FF_BADFD;
	int r = FFPARS_ESYS;

	if (NULL == (fn = core->getpath(FFSTR(FMED_CMDHELP_FILE))))
		goto done;

	if (FF_BADFD == (f = fffile_open(fn, O_RDONLY | O_NOATIME)))
		goto done;

	if (NULL == ffarr_alloc(&buf, fffile_size(f)))
		goto done;

	n = fffile_read(f, buf.ptr, buf.cap);
	if (n <= 0)
		goto done;

	fffile_write(ffstdout, buf.ptr, n);
	r = FFPARS_ELAST;

done:
	if (r != FFPARS_ELAST)
		syserrlog(core, NULL, "core", "trying to read help.txt");
	ffmem_safefree(fn);
	FF_SAFECLOSE(f, FF_BADFD, fffile_close);
	ffarr_free(&buf);
	return r;
}

static int fmed_arg_infile(ffparser_schem *p, void *obj, const ffstr *val)
{
	fmed_cmd *cmd = obj;
	char **fn;
	if (NULL == (fn = ffarr_pushgrowT(&cmd->in_files, 4, char*)))
		return FFPARS_ESYS;
	*fn = val->ptr;
	return 0;
}

static int fmed_arg_listdev(void)
{
	fmed_adev_ent *ents = NULL;
	const fmed_modinfo *mod;
	const fmed_adev *adev = NULL;
	uint i, ndev;
	ffarr buf = {0};

	if (NULL == ffarr_alloc(&buf, 1024))
		goto end;

	if (NULL == (mod = core->getmod2(FMED_MOD_INFO_ADEV_OUT, NULL, 0))
		|| NULL == (adev = mod->m->iface("adev")))
		goto end;

	ndev = adev->list(&ents, FMED_ADEV_PLAYBACK);
	if ((int)ndev < 0)
		goto end;

	ffstr_catfmt(&buf, "Playback/Loopback:\n");
	for (i = 0;  i != ndev;  i++) {
		ffstr_catfmt(&buf, "device #%u: %s\n", i + 1, ents[i].name);
	}

	if (NULL == (mod = core->getmod2(FMED_MOD_INFO_ADEV_IN, NULL, 0))
		|| NULL == (adev = mod->m->iface("adev")))
		goto end;

	ndev = adev->list(&ents, FMED_ADEV_CAPTURE);
	if ((int)ndev < 0)
		goto end;

	ffstr_catfmt(&buf, "\nCapture:\n");
	for (i = 0;  i != ndev;  i++) {
		ffstr_catfmt(&buf, "device #%u: %s\n", i + 1, ents[i].name);
	}

end:
	if (buf.len != 0)
		fffile_write(ffstdout, buf.ptr, buf.len);
	ffarr_free(&buf);
	if (ents != NULL)
		adev->listfree(ents);
	return FFPARS_ELAST;
}

static int fmed_arg_format(ffparser_schem *p, void *obj, ffstr *val)
{
	fmed_cmd *conf = obj;
	int r;
	if (0 > (r = ffpcm_fmt(val->ptr, val->len)))
		return FFPARS_EBADVAL;
	conf->out_format = r;
	return 0;
}

static int fmed_arg_channels(ffparser_schem *p, void *obj, ffstr *val)
{
	fmed_cmd *conf = obj;
	int r;
	if (0 > (r = ffpcm_channels(val->ptr, val->len)))
		return FFPARS_EBADVAL;
	conf->out_channels = r;
	return 0;
}

static const char* const outcp_str[] = { "all", "cmd" };

static int fmed_arg_out_copy(ffparser_schem *p, void *obj, const ffstr *val)
{
	fmed_cmd *cmd = obj;
	int r = FMED_OUTCP_ALL;
	if (val != NULL) {
		r = ffszarr_findsorted(outcp_str, FFCNT(outcp_str), val->ptr, val->len);
		if (r < 0)
			return FFPARS_EVALUNSUPP;
		r++;
	}
	cmd->out_copy = r;
	return 0;
}

static int fmed_arg_seek(ffparser_schem *p, void *obj, const ffstr *val)
{
	fmed_cmd *cmd = obj;
	uint i;
	ffdtm dt;
	fftime t;
	if (val->len != fftime_fromstr(&dt, val->ptr, val->len, FFTIME_HMS_MSEC_VAR))
		return FFPARS_EBADVAL;

	fftime_join(&t, &dt, FFTIME_TZNODATE);
	i = fftime_ms(&t);

	if (!ffsz_cmp(p->curarg->name, "seek"))
		cmd->seek_time = i;
	else
		cmd->until_time = i;
	return 0;
}

static int fmed_arg_install(ffparser_schem *p, void *obj, const ffstr *val)
{
#ifdef FF_WIN
	const fmed_modinfo *mi = core->insmod("gui.gui", NULL);
	if (mi != NULL)
		mi->m->sig(!ffsz_cmp(p->curarg->name, "install") ? FMED_SIG_INSTALL : FMED_SIG_UNINSTALL);
#endif
	return FFPARS_ELAST;
}

static int fmed_arg_skip(ffparser_schem *p, void *obj, const ffstr *val)
{
	return 0;
}

static int fmed_arg_input_chk(ffparser_schem *p, void *obj, const ffstr *val)
{
	ffstr fname, name;
	if (NULL == ffpath_split2(val->ptr, val->len, NULL, &fname)) {
		ffpath_splitname(fname.ptr, fname.len, &name, NULL);
		if (ffstr_eqcz(&name, "@stdin"))
			core->props->stdin_busy = 1;
	}
	return 0;
}

static int fmed_arg_out_chk(ffparser_schem *p, void *obj, const ffstr *val)
{
	ffstr fname, name;
	if (NULL == ffpath_split2(val->ptr, val->len, NULL, &fname)) {
		ffpath_splitname(fname.ptr, fname.len, &name, NULL);
		if (ffstr_eqcz(&name, "@stdout"))
			core->props->stdout_busy = 1;
	}
	return 0;
}

static int fmed_cmdline(int argc, char **argv, uint main_only)
{
	ffparser_schem ps;
	ffpsarg_parser p;
	ffpars_ctx ctx = {0};
	int r = 0;
	int ret = 1;
	const char *arg;
	ffpsarg a;

	ffpsarg_init(&a, (void*)argv, argc);

	if (main_only)
		ffpars_setargs(&ctx, g->cmd, fmed_cmdline_main_args, FFCNT(fmed_cmdline_main_args));
	else
		ffpars_setargs(&ctx, g->cmd, fmed_cmdline_args, FFCNT(fmed_cmdline_args));

	if (0 != ffpsarg_scheminit(&ps, &p, &ctx)) {
		errlog(core, NULL, "core", "cmd line parser", NULL);
		return 1;
	}

	ffpsarg_next(&a); //skip argv[0]

	arg = ffpsarg_next(&a);
	while (arg != NULL) {
		int n = 0;
		r = ffpsarg_parse(&p, arg, &n);
		if (n != 0)
			arg = ffpsarg_next(&a);

		r = ffpsarg_schemrun(&ps);

		if (r == FFPARS_ELAST)
			goto fail;

		if (ffpars_iserr(r))
			break;
	}

	if (!ffpars_iserr(r))
		r = ffpsarg_schemfin(&ps);

	if (ffpars_iserr(r)) {
		errlog(core, NULL, "core", "cmd line parser: near \"%S\": %s"
			, &p.val, (r == FFPARS_ESYS) ? fferr_strp(fferr_last()) : ffpars_errstr(r));
		goto fail;
	}

	ret = 0;

fail:
	ffpsarg_destroy(&a);
	ffpars_schemfree(&ps);
	ffpsarg_parseclose(&p);
	return ret;
}


static void std_log(uint flags, fmed_logdata *ld)
{
	char buf[4096];
	char *s = buf;
	const char *end = buf + FFCNT(buf) - FFSLEN("\n");

	if (flags != FMED_LOG_USER) {
		s += ffs_fmt(s, end, "%s [%s] %s: ", ld->stime, ld->level, ld->module);

		if (ld->ctx != NULL)
			s += ffs_fmt(s, end, "%S:\t", ld->ctx);
	}

	s += ffs_fmtv(s, end, ld->fmt, ld->va);

	if (flags & FMED_LOG_SYS)
		s += ffs_fmt(s, end, ": %E", fferr_last());

	*s++ = '\n';

	uint lev = flags & _FMED_LOG_LEVMASK;
	fffd fd = (lev > FMED_LOG_WARN && !core->props->stdout_busy) ? ffstdout : ffstderr;
	ffstd_write(fd, buf, s - buf);
}


static const int sigs[] = { SIGINT };
static const int sigs_block[] = { SIGINT, SIGIO };

static void fmed_onsig(void *udata)
{
	const fmed_track *track;
	int sig;
	ffsignal *sg = udata;

	if (-1 == (sig = ffsig_read(sg, NULL)))
		return;

	if (NULL == (track = core->getmod("#core.track")))
		return;
	track->cmd((void*)-1, FMED_TRACK_STOPALL_EXIT);
}

static void qu_setprops(fmed_cmd *fmed, const fmed_queue *qu, fmed_que_entry *qe);

#ifdef FF_WIN
/** Add to queue filenames expanded by wildcard. */
static void* open_input_wcard(const fmed_queue *qu, char *src, const fmed_track *track, const fmed_trk *trkinfo)
{
	ffdirexp de;
	void *first = NULL;

	ffstr s;
	ffstr_setz(&s, src);
	if (ffarr_end(&s) == ffs_findof(s.ptr, s.len, "*?", 2))
		return NULL;

	if (0 != ffdir_expopen(&de, src, 0))
		return NULL;

	const char *fn;
	while (NULL != (fn = ffdir_expread(&de))) {
		fmed_que_entry e, *qe;
		ffmem_tzero(&e);
		ffstr_setz(&e.url, fn);
		qe = qu->add(&e);
		track->copy_info(qe->trk, trkinfo);
		qu_setprops(g->cmd, qu, qe);
		if (first == NULL)
			first = qe;
	}
	ffdir_expclose(&de);
	return first;
}
#endif

static void qu_setval(const fmed_queue *qu, fmed_que_entry *qe, const char *name, int64 val)
{
	qu->meta_set(qe, name, ffsz_len(name), (void*)&val, sizeof(int64), FMED_QUE_TRKDICT | FMED_QUE_NUM);
}

static void qu_setprops(fmed_cmd *fmed, const fmed_queue *qu, fmed_que_entry *qe)
{
	if (fmed->trackno != NULL) {
		qu->meta_set(qe, FFSTR("input_trackno"), fmed->trackno, ffsz_len(fmed->trackno), FMED_QUE_TRKDICT);
		ffmem_free0(fmed->trackno);
	}

	if (fmed->playdev_name != 0)
		qu_setval(qu, qe, "playdev_name", fmed->playdev_name);

	if (fmed->out_copy) {
		qu_setval(qu, qe, "out-copy", fmed->out_copy);
		if (fmed->stream_copy)
			qu_setval(qu, qe, "out_stream_copy", 1);
		if (fmed->outfn.len != 0)
			qu->meta_set(qe, FFSTR("out_filename"), fmed->outfn.ptr, fmed->outfn.len, FMED_QUE_TRKDICT);

	} else {
		if (fmed->outfn.len != 0 && !fmed->rec)
			qu->meta_set(qe, FFSTR("output"), fmed->outfn.ptr, fmed->outfn.len, FMED_QUE_TRKDICT);
	}

	if (fmed->rec)
		qu_setval(qu, qe, "low_latency", 1);

	if (fmed->meta.len != 0)
		qu->meta_set(qe, FFSTR("meta"), fmed->meta.ptr, fmed->meta.len, FMED_QUE_TRKDICT);
}

static void trk_prep(fmed_cmd *fmed, fmed_trk *trk)
{
	trk->input_info = fmed->info;
	if (fmed->fseek != 0)
		trk->input.seek = fmed->fseek;
	if (fmed->seek_time != 0)
		trk->audio.seek = fmed->seek_time;
	if (fmed->until_time != 0)
		trk->audio.until = fmed->until_time;

	trk->out_overwrite = fmed->overwrite;
	trk->out_preserve_date = fmed->preserve_date;

	if (fmed->out_format != 0)
		trk->audio.convfmt.format = fmed->out_format;
	if (fmed->out_channels != 0)
		trk->audio.convfmt.channels = fmed->out_channels;
	if (fmed->out_rate != 0)
		trk->audio.convfmt.sample_rate = fmed->out_rate;

	trk->pcm_peaks = fmed->pcm_peaks;
	trk->pcm_peaks_crc = fmed->pcm_crc;
	trk->use_dynanorm = fmed->dynanorm;

	if (fmed->volume != 100) {
		double db;
		if (fmed->volume < 100)
			db = ffpcm_vol2db(fmed->volume, 48);
		else
			db = ffpcm_vol2db_inc(fmed->volume - 100, 25, 6);
		trk->audio.gain = db * 100;
	}

	if (fmed->gain != 0)
		trk->audio.gain = fmed->gain * 100;

	if (fmed->aac_qual != (uint)-1)
		trk->aac.quality = fmed->aac_qual;
	if (fmed->aac_profile != NULL)
		ffstr_setz(&trk->aac.profile, fmed->aac_profile);
	if (fmed->vorbis_qual != -255)
		trk->vorbis.quality = (fmed->vorbis_qual + 1.0) * 10;
	if (fmed->opus_brate != 0)
		trk->opus.bitrate = fmed->opus_brate;
	if (fmed->mpeg_qual != 0xffff)
		trk->mpeg.quality = fmed->mpeg_qual;
	if (fmed->flac_complevel != 0xff)
		trk->flac.compression = fmed->flac_complevel;

	if (fmed->stream_copy && !fmed->out_copy)
		trk->stream_copy = 1;
}

static void open_input(void *udata)
{
	char **pfn;
	const fmed_track *track;
	const fmed_queue *qu;
	fmed_que_entry e, *qe;
	void *first = NULL;
	fmed_cmd *fmed = udata;

	if (NULL == (qu = core->getmod("#queue.queue")))
		goto end;
	if (NULL == (track = core->getmod("#core.track")))
		goto end;

	fmed_trk trkinfo;
	track->copy_info(&trkinfo, NULL);
	trk_prep(fmed, &trkinfo);

	FFARR_WALKT(&fmed->in_files, pfn, char*) {

#ifdef FF_WIN
		if (NULL != (qe = open_input_wcard(qu, *pfn, track, &trkinfo))) {
			if (first == NULL)
				first = qe;
			continue;
		}
#endif

		ffmem_tzero(&e);
		ffstr_setz(&e.url, *pfn);
		qe = qu->add(&e);
		if (first == NULL)
			first = qe;

		track->copy_info(qe->trk, &trkinfo);
		qu_setprops(fmed, qu, qe);
	}
	FFARR_FREE_ALL_PTR(&fmed->in_files, ffmem_free, char*);

	if (first != NULL) {
		if (!fmed->mix)
			qu->cmd(FMED_QUE_PLAY, first);
		else
			qu->cmd(FMED_QUE_MIX, NULL);
	}

	if (fmed->rec) {
		void *trk;
		if (NULL == (trk = track->create(FMED_TRACK_REC, NULL)))
			goto end;
		fmed_trk *ti = track->conf(trk);
		ffpcmex fmt = ti->audio.fmt;
		track->copy_info(ti, &trkinfo);
		ti->audio.fmt = fmt;

		if (fmed->lbdev_name != (uint)-1) {
			track->setval(trk, "loopback_device", fmed->lbdev_name);
			rec_lpback_new_track(fmed);
		} else if (fmed->captdev_name != 0)
			track->setval(trk, "capture_device", fmed->captdev_name);

		if (fmed->outfn.len != 0)
			track->setvalstr(trk, "output", fmed->outfn.ptr);

		if (fmed->rec)
			track->setval(trk, "low_latency", 1);

		track->cmd(trk, FMED_TRACK_START);
	}

	if (first == NULL && !fmed->rec && !fmed->gui)
		core->sig(FMED_STOP);

	return;

end:
	return;
}

/** Create a track to support recording from WASAPI in loopback mode.
It generates silence and plays it via an audio device,
 so data from WASAPI in looopback mode can be read continuously. */
static void rec_lpback_new_track(fmed_cmd *cmd)
{
	const fmed_track *track;
	void *trk;
	int r = 0;

	if (NULL == (track = core->getmod("#core.track")))
		return;
	if (NULL == (trk = track->create(FMED_TRK_TYPE_NONE, NULL)))
		return;

	fmed_trk *info = track->conf(trk);
	info->audio.fmt.format = FFPCM_16;
	info->audio.fmt.channels = 2;
	info->audio.fmt.sample_rate = 44100;
	if (cmd->lbdev_name != (uint)-1)
		track->setval(trk, "playdev_name", cmd->lbdev_name);

	r |= track->cmd(trk, FMED_TRACK_ADDFILT, "#soundmod.silgen");
	r |= track->cmd(trk, FMED_TRACK_ADDFILT, "wasapi.out");
	if (r != 0) {
		track->cmd(trk, FMED_TRACK_STOP);
		return;
	}
	track->cmd(trk, FMED_TRACK_START);
}

static int gcmd_send(const fmed_globcmd_iface *globcmd)
{
	if (0 != globcmd->write(g->cmd->globcmd.ptr, g->cmd->globcmd.len)) {
		return -1;
	}

	return 0;
}

static int loadcore(char *argv0)
{
	int rc = -1;
	char buf[FF_MAXPATH];
	const char *path;
	ffdl dl = NULL;
	ffarr a = {0};

	if (NULL == (path = ffps_filename(buf, sizeof(buf), argv0)))
		goto end;
	if (0 == ffstr_catfmt(&a, "%s/../mod/core.%s%Z", path, FFDL_EXT))
		goto end;
	a.len = ffpath_norm(a.ptr, a.cap, a.ptr, a.len - 1, 0);
	a.ptr[a.len] = '\0';

	if (NULL == (dl = ffdl_open(a.ptr, 0))) {
		fffile_fmt(ffstderr, NULL, "can't load %s: %s\n", a.ptr, ffdl_errstr());
		goto end;
	}

	g->core_init = (void*)ffdl_addr(dl, "core_init");
	g->core_free = (void*)ffdl_addr(dl, "core_free");
	if (g->core_init == NULL || g->core_free == NULL) {
		fffile_fmt(ffstderr, NULL, "can't resolve functions from %s: %s\n"
			, a.ptr, ffdl_errstr());
		goto end;
	}

	g->core_dl = dl;
	dl = NULL;
	rc = 0;

end:
	FF_SAFECLOSE(dl, NULL, ffdl_close);
	ffarr_free(&a);
	return rc;
}

#if defined FF_WIN
#define OS_STR  "win"
#elif defined FF_BSD
#define OS_STR  "bsd"
#else
#define OS_STR  "linux"
#endif

#ifdef FF_64
#define CPU_STR  "amd64"
#else
#define CPU_STR  "i686"
#endif

int main(int argc, char **argv, char **env)
{
	int rc = 1;
	fmed_cmd *gcmd;

	ffmem_init();
	if (NULL == (g = ffmem_new(struct gctx)))
		return 1;
	ffsig_init(&g->sigs_task);

	fffile_writecz(ffstderr, "fmedia v" FMED_VER " (" OS_STR "-" CPU_STR ")\n");

	ffsig_mask(SIG_BLOCK, sigs_block, FFCNT(sigs_block));

	if (0 != loadcore(argv[0]))
		goto end;

	if (NULL == (core = g->core_init(&g->cmd, argv, env)))
		goto end;
	g->cmd->log = &std_logger;
	gcmd = g->cmd;

	if (argc == 1) {
		fmed_arg_usage();
		rc = 0;
		goto end;
	}

	if (0 != fmed_cmdline(argc, argv, 1))
		goto end;

	if (gcmd->debug)
		core->loglev = FMED_LOG_DEBUG;

	if (0 != core->cmd(FMED_CONF, gcmd->conf_fn))
		goto end;

	ffmem_safefree(gcmd->conf_fn);

	if (0 != fmed_cmdline(argc, argv, 0))
		goto end;

	if (gcmd->bground) {
		if (gcmd->bgchild)
			ffterm_detach();
		else {
			ffps ps = ffps_createself_bg("--background-child");
			if (ps == FFPS_INV) {
				syserrlog(core, NULL, "core", "failed to spawn background process");
				goto end;

			} else if (ps != 0) {
				core->log(FMED_LOG_INFO, NULL, "core", "spawned background process: PID %u", ffps_id(ps));
				(void)ffps_close(ps);
				rc = 0;
				goto end;
			}
		}
	}

	const fmed_globcmd_iface *globcmd = NULL;
	ffbool gcmd_listen = 0;
	if (gcmd->globcmd.len != 0
		&& NULL != (globcmd = core->getmod("#globcmd.globcmd"))) {

		if (ffstr_eqcz(&gcmd->globcmd, "listen"))
			gcmd_listen = 1;

		else if (0 == globcmd->ctl(FMED_GLOBCMD_OPEN, g->cmd->globcmd_pipename)) {
			gcmd_send(globcmd);
			rc = 0;
			goto end;
		}
	}

	if (0 != core->sig(FMED_OPEN))
		goto end;

	if (gcmd_listen) {
		globcmd->ctl(FMED_GLOBCMD_START, g->cmd->globcmd_pipename);
		ffmem_safefree0(g->cmd->globcmd_pipename);
	}

	g->sigs_task.udata = &g->sigs_task;
	if (0 != ffsig_ctl(&g->sigs_task, core->kq, sigs, FFCNT(sigs), &fmed_onsig)) {
		syserrlog(core, NULL, "core", "%s", "ffsig_ctl()");
		goto end;
	}

	fftask_set(&gcmd->tsk_start, &open_input, g->cmd);
	core->task(&gcmd->tsk_start, FMED_TASK_POST);

	core->sig(FMED_START);
	rc = 0;

end:
	if (core != NULL) {
		ffsig_ctl(&g->sigs_task, core->kq, sigs, FFCNT(sigs), NULL);
		g->core_free();
	}
	FF_SAFECLOSE(g->core_dl, NULL, ffdl_close);
	ffmem_free(g);
	return rc;
}
