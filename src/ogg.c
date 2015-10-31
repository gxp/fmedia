/** OGG Vorbis input/output.
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>

#include <FF/audio/ogg.h>
#include <FF/audio/pcm.h>
#include <FF/array.h>
#include <FFOS/error.h>
#include <FFOS/random.h>
#include <FFOS/timer.h>


static const fmed_core *core;

typedef struct fmed_ogg {
	ffogg og;
	uint state;
} fmed_ogg;

typedef struct ogg_out {
	ffogg_enc og;
	uint state;
} ogg_out;

static struct ogg_in_conf_t {
	byte seekable;
} ogg_in_conf;

static struct ogg_out_conf_t {
	uint qual;
} ogg_out_conf;

static const char *const metanames[] = {
	"meta_album"
	, "meta_artist"
	, "meta_comment"
	, "meta_date"
	, "meta_genre"
	, "meta_title"
	, "meta_tracknumber"
	, "meta_tracktotal"
};


//FMEDIA MODULE
static const void* ogg_iface(const char *name);
static int ogg_sig(uint signo);
static void ogg_destroy(void);
static const fmed_mod fmed_ogg_mod = {
	&ogg_iface, &ogg_sig, &ogg_destroy
};

//DECODE
static void* ogg_open(fmed_filt *d);
static void ogg_close(void *ctx);
static int ogg_decode(void *ctx, fmed_filt *d);
static int ogg_conf(ffpars_ctx *ctx);
static const fmed_filter fmed_ogg_input = {
	&ogg_open, &ogg_decode, &ogg_close, &ogg_conf
};

static const ffpars_arg ogg_in_conf_args[] = {
	{ "seekable",  FFPARS_TBOOL | FFPARS_F8BIT,  FFPARS_DSTOFF(struct ogg_in_conf_t, seekable) }
};

//ENCODE
static void* ogg_out_open(fmed_filt *d);
static void ogg_out_close(void *ctx);
static int ogg_out_encode(void *ctx, fmed_filt *d);
static int ogg_out_config(ffpars_ctx *ctx);
static const fmed_filter fmed_ogg_output = {
	&ogg_out_open, &ogg_out_encode, &ogg_out_close, &ogg_out_config
};

static const ffpars_arg ogg_out_conf_args[] = {
	{ "quality",  FFPARS_TINT,  FFPARS_DSTOFF(struct ogg_out_conf_t, qual) }
};


FF_EXP const fmed_mod* fmed_getmod(const fmed_core *_core)
{
	fftime t;
	fftime_now(&t);
	ffrnd_seed(t.s);

	ffmem_init();
	core = _core;
	return &fmed_ogg_mod;
}


static const void* ogg_iface(const char *name)
{
	if (!ffsz_cmp(name, "decode")) {
		ogg_in_conf.seekable = 1;
		return &fmed_ogg_input;

	} else if (!ffsz_cmp(name, "encode")) {
		ogg_out_conf.qual = 50;
		return &fmed_ogg_output;
	}
	return NULL;
}

static int ogg_sig(uint signo)
{
	return 0;
}

static void ogg_destroy(void)
{
}


static int ogg_conf(ffpars_ctx *ctx)
{
	ffpars_setargs(ctx, &ogg_in_conf, ogg_in_conf_args, FFCNT(ogg_in_conf_args));
	return 0;
}

static void* ogg_open(fmed_filt *d)
{
	int64 input_size;
	fmed_ogg *o = ffmem_tcalloc1(fmed_ogg);
	if (o == NULL)
		return NULL;
	ffogg_init(&o->og);

	if (FMED_NULL != (input_size = fmed_getval("total_size")))
		o->og.total_size = input_size;

	if (ogg_in_conf.seekable)
		o->og.seekable = 1;
	return o;
}

static void ogg_close(void *ctx)
{
	fmed_ogg *o = ctx;
	ffogg_close(&o->og);
	ffmem_free(o);
}

static int ogg_decode(void *ctx, fmed_filt *d)
{
	enum { I_HDR, I_DATA };
	fmed_ogg *o = ctx;
	int r;
	uint tag;
	int64 seek_time;

	if (d->flags & FMED_FSTOP) {
		d->outlen = 0;
		return FMED_RLASTOUT;
	}

	o->og.data = d->data;
	o->og.datalen = d->datalen;

again:
	switch (o->state) {
	case I_HDR:
		break;

	case I_DATA:
		if (FMED_NULL != (seek_time = fmed_popval("seek_time")))
			ffogg_seek(&o->og, ffpcm_samples(seek_time, ffogg_rate(&o->og)));
		break;
	}

	for (;;) {
		r = ffogg_decode(&o->og);
		switch (r) {
		case FFOGG_RMORE:

			if (o->state == I_HDR) {
				int64 off = fmed_getval("input_off");
				if (off + d->datalen == o->og.total_size
					&& 0 == ffogg_nodata(&o->og))
					break;
			}

			if (d->flags & FMED_FLAST) {
				dbglog(core, d->trk, "ogg", "no eos page");
				d->outlen = 0;
				return FMED_RDONE;
			}
			return FMED_RMORE;

		case FFOGG_RDATA:
			goto data;

		case FFOGG_RDONE:
			d->outlen = 0;
			return FMED_RDONE;

		case FFOGG_RHDR:
			d->track->setvalstr(d->trk, "pcm_decoder", "Vorbis");
			fmed_setval("pcm_format", FFPCM_FLOAT);
			fmed_setval("pcm_channels", ffogg_channels(&o->og));
			fmed_setval("pcm_sample_rate", ffogg_rate(&o->og));
			fmed_setval("pcm_ileaved", 0);
			break;

		case FFOGG_RTAG:
			dbglog(core, d->trk, "ogg", "%S: %S", &o->og.tagname, &o->og.tagval);

			tag = ffogg_tag(o->og.tagname.ptr, o->og.tagname.len);
			if (tag < FFCNT(metanames) && o->og.tagval.len != 0)
				d->track->setvalstr(d->trk, metanames[tag], o->og.tagval.ptr);
			break;

		case FFOGG_RHDRFIN:
			dbglog(core, d->trk, "ogg", "vendor: %s", o->og.vcmt.vendor);
			if (!ogg_in_conf.seekable)
				o->state = I_DATA;
			break;

		case FFOGG_RINFO:
			fmed_setval("total_samples", o->og.total_samples);
			fmed_setval("bitrate", ffogg_bitrate(&o->og));
			o->state = I_DATA;
			goto again;

		case FFOGG_RSEEK:
			fmed_setval("input_seek", o->og.off);
			return FMED_RMORE;

		case FFOGG_RWARN:
			errlog(core, d->trk, "ogg", "warning: near sample %u: ffogg_decode(): %s"
				, ffogg_cursample(&o->og), ffogg_errstr(o->og.err));
			break;

		default:
			errlog(core, d->trk, "ogg", "ffogg_decode(): %s", ffogg_errstr(o->og.err));
			return FMED_RERR;
		}
	}

data:
	dbglog(core, d->trk, "ogg", "decoded %u PCM samples, page: %u, granule pos: %U"
		, o->og.nsamples, ffogg_pageno(&o->og), ffogg_granulepos(&o->og));
	d->track->setval(d->trk, "current_position", ffogg_cursample(&o->og));

	d->data = o->og.data;
	d->datalen = o->og.datalen;
	d->outni = (void**)o->og.pcm;
	d->outlen = o->og.pcmlen;
	return FMED_ROK;
}


static int ogg_out_config(ffpars_ctx *ctx)
{
	ffpars_setargs(ctx, &ogg_out_conf, ogg_out_conf_args, FFCNT(ogg_out_conf_args));
	return 0;
}

static void* ogg_out_open(fmed_filt *d)
{
	ogg_out *o = ffmem_tcalloc1(ogg_out);
	if (o == NULL)
		return NULL;

	ffogg_enc_init(&o->og);
	return o;
}

static void ogg_out_close(void *ctx)
{
	ogg_out *o = ctx;
	ffogg_enc_close(&o->og);
	ffmem_free(o);
}

static int ogg_out_addmeta(ogg_out *o, fmed_filt *d)
{
	uint i;
	const char *val;
	for (i = 0;  i < FFCNT(metanames);  i++) {
		val = d->track->getvalstr(d->trk, metanames[i]);
		if (val != FMED_PNULL) {
			ffogg_iaddtag(&o->og, i, val);
		}
	}
	return 0;
}

static int ogg_out_encode(void *ctx, fmed_filt *d)
{
	enum { I_CONF, I_CREAT, I_INPUT, I_ENCODE, I_DATA };
	ogg_out *o = ctx;
	int r, qual, il;
	ffpcm fmt;

	switch (o->state) {
	case I_CONF:
		fmt.format = (int)fmed_getval("pcm_format");
		if (fmt.format != FFPCM_FLOAT)
			fmed_setval("conv_pcm_format", FFPCM_FLOAT);

		il = (int)fmed_getval("pcm_ileaved");
		if (il != 0)
			fmed_setval("conv_pcm_ileaved", 0);

		if (fmt.format != FFPCM_FLOAT || il != 0) {
			o->state = I_CREAT;
			return FMED_RMORE;
		}
		//break;

	case I_CREAT:
		fmt.format = (int)fmed_getval("pcm_format");
		il = (int)fmed_getval("pcm_ileaved");
		if (fmt.format != FFPCM_FLOAT || il != 0) {
			errlog(core, d->trk, "ogg", "input format must be PCM-float non-interleaved");
			return FMED_RERR;
		}

		fmt.sample_rate = (int)fmed_getval("pcm_sample_rate");
		fmt.channels = (int)fmed_getval("pcm_channels");

		ogg_out_addmeta(o, d);

		qual = (int)fmed_getval("ogg-quality");
		if (qual == FMED_NULL)
			qual = ogg_out_conf.qual;

		if (0 != (r = ffogg_create(&o->og, &fmt, qual))) {
			errlog(core, d->trk, "ogg", "ffogg_create() failed: %s", ffogg_errstr(r));
			return FMED_RERR;
		}
		//o->state = I_INPUT;
		//break;

	case I_INPUT:
		o->og.pcm = (const float**)d->data;
		o->og.pcmlen = d->datalen;
		if (d->flags & FMED_FLAST)
			o->og.fin = 1;
		o->state = I_ENCODE;
		//break;

	case I_ENCODE:
		break;

	case I_DATA:
		d->out = o->og.data;
		d->outlen = o->og.datalen;
		o->state = I_ENCODE;
		return FMED_ROK;
	}

	for (;;) {
		r = ffogg_encode(&o->og);
		switch (r) {

		case FFOGG_RDATA:
			goto data;

		case FFOGG_RDONE:
			d->outlen = 0;
			return FMED_RDONE;

		case FFOGG_RMORE:
			o->state = I_INPUT;
			return FMED_RMORE;

		default:
			errlog(core, d->trk, "ogg", "ffogg_encode() failed: %s", ffogg_errstr(o->og.err));
			return FMED_RERR;
		}
	}

data:
	d->out = ffogg_pagehdr(&o->og, &d->outlen);
	o->state = I_DATA;

	dbglog(core, d->trk, "ogg", "output: %L+%L bytes, page: %u, granule pos: %U"
		, (size_t)d->outlen, o->og.datalen
		, ffogg_pageno(&o->og), ffogg_granulepos(&o->og));

	return FMED_ROK;
}
