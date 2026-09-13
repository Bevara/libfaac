/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / libfaac AAC encoder filter - talks directly
 *  to libfaac's native API (faac_encoder_open/faac_encoder_encode/
 *  faac_encoder_close, the faac_* surface of libfaac 2.x; the legacy
 *  faacEnc* API was removed upstream).
 *
 *  Produces raw AAC-LC (MPEG-4) frames with an out-of-band
 *  AudioSpecificConfig, which is what an mp4 muxer (and MSE) expects: mp4mx
 *  accepts the pid directly, no ADTS reframing involved. This is the native
 *  AAC route for the progressive/MSE path, where the source audio (Vorbis,
 *  MP2...) cannot be remuxed as-is - the alternative being the WebCodecs
 *  encoder, measured ~20 s slower on a 33 s clip.
 *
 *  libfaac has no thread, no mandatory SIMD (the SSE2 quantizer is optional
 *  and left out of the wasm build) and no setjmp.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <faac.h>

typedef struct
{
	/* opts */
	u32 bitrate;

	GF_FilterPid *ipid, *opid;
	u32 sample_rate, num_channels;
	u32 frame_samples, max_output_bytes;
	u64 frames_out;

	faac_encoder *enc;
	u8 *outbuf;
} GF_FaacEncCtx;

static void faacenc_close(GF_FaacEncCtx *ctx)
{
	if (ctx->enc)
		faac_encoder_close(&ctx->enc);
	ctx->enc = NULL;
	if (ctx->outbuf)
		gf_free(ctx->outbuf);
	ctx->outbuf = NULL;
}

static GF_Err faacenc_setup(GF_FaacEncCtx *ctx)
{
	faac_params p;
	faac_encoder_info info;
	faac_status st;
	const uint8_t *asc = NULL;
	uint32_t asc_len = 0;
	u8 *dsi;

	faacenc_close(ctx);

	st = faac_params_init(&p, sizeof(p));
	if (st < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FaacEnc] params init failed: %s\n", faac_strerror(st)));
		return GF_IO_ERR;
	}
	p.sample_rate = ctx->sample_rate;
	p.num_channels = ctx->num_channels;
	p.mpeg_version = FAAC_MPEG4;
	p.object_type = FAAC_OBJ_LOW;
	/* raw frames + out-of-band ASC: what mp4 wants. ADTS would need a
	 * reframer in front of the muxer */
	p.output_format = FAAC_STREAM_RAW;
	p.input_format = FAAC_INPUT_16BIT;
	/* libfaac's bit_rate is per channel; the option is the usual total kbps */
	p.bit_rate = (ctx->bitrate * 1000) / ctx->num_channels;

	st = faac_encoder_open(&p, &ctx->enc);
	if (st < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FaacEnc] Failed to open encoder (%u Hz, %u ch): %s\n", ctx->sample_rate, ctx->num_channels, faac_strerror(st)));
		ctx->enc = NULL;
		return GF_NOT_SUPPORTED;
	}

	memset(&info, 0, sizeof(info));
	info.struct_size = sizeof(info);
	st = faac_encoder_get_info(ctx->enc, &info);
	if (st < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FaacEnc] get_info failed: %s\n", faac_strerror(st)));
		faacenc_close(ctx);
		return GF_IO_ERR;
	}
	ctx->frame_samples = info.frame_samples;
	ctx->max_output_bytes = info.max_output_bytes;
	ctx->outbuf = gf_malloc(ctx->max_output_bytes);
	if (!ctx->outbuf)
	{
		faacenc_close(ctx);
		return GF_OUT_OF_MEM;
	}
	ctx->frames_out = 0;

	st = faac_encoder_asc(ctx->enc, &asc, &asc_len);
	if ((st < 0) || !asc || !asc_len)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FaacEnc] No AudioSpecificConfig: %s\n", faac_strerror(st)));
		faacenc_close(ctx);
		return GF_NOT_SUPPORTED;
	}
	/* the buffer is owned by the encoder and dies with it: copy it */
	dsi = gf_malloc(asc_len);
	if (!dsi)
	{
		faacenc_close(ctx);
		return GF_OUT_OF_MEM;
	}
	memcpy(dsi, asc, asc_len);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA_NO_COPY(dsi, asc_len));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->sample_rate));
	/* priming samples the decoder must skip; negative delay = skip, which
	 * mp4mx turns into an edit list */
	if (info.encoder_delay)
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DELAY, &PROP_LONGSINT(-(s64)info.encoder_delay));

	GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[FaacEnc] AAC-LC %u Hz %u ch, %u kbps, frame %u samples, delay %u\n", ctx->sample_rate, ctx->num_channels, ctx->bitrate, ctx->frame_samples, info.encoder_delay));
	return GF_OK;
}

static GF_Err faacenc_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_FaacEncCtx *ctx = (GF_FaacEncCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		faacenc_close(ctx);
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_AAC_MPEG4));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, NULL);

	gf_filter_set_name(filter, "encfaac:libfaac");

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_SAMPLE_RATE);
	if (!prop)
		return GF_OK;
	ctx->sample_rate = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_NUM_CHANNELS);
	if (!prop)
		return GF_OK;
	ctx->num_channels = prop->value.uint;
	if (!ctx->num_channels)
		return GF_NOT_SUPPORTED;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_AUDIO_FORMAT);
	if (prop && (prop->value.uint != GF_AUDIO_FMT_S16))
	{
		gf_filter_pid_negotiate_property(pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
		return GF_OK;
	}

	return faacenc_setup(ctx);
}

/* one encode call: emits at most one AAC frame */
static GF_Err faacenc_push(GF_FaacEncCtx *ctx, const void *in, u32 in_samples, Bool *got_frame)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	uint32_t written = 0;
	faac_status st;

	*got_frame = GF_FALSE;
	st = faac_encoder_encode(ctx->enc, in, in_samples, ctx->outbuf, ctx->max_output_bytes, &written);
	if (st < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FaacEnc] Encoding failed: %s\n", faac_strerror(st)));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (!written)
		return GF_OK;

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, written, &output);
	if (!dst_pck)
		return GF_OUT_OF_MEM;
	memcpy(output, ctx->outbuf, written);
	gf_filter_pck_set_cts(dst_pck, ctx->frames_out * ctx->frame_samples);
	gf_filter_pck_set_duration(dst_pck, ctx->frame_samples);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
	ctx->frames_out++;
	*got_frame = GF_TRUE;
	return GF_OK;
}

static GF_Err faacenc_process(GF_Filter *filter)
{
	GF_FaacEncCtx *ctx = (GF_FaacEncCtx *)gf_filter_get_udta(filter);
	GF_FilterPacket *pck;
	const u8 *in_data;
	u32 size, total_samples, chunk_max, pos;
	Bool got;
	GF_Err e;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			if (ctx->enc)
			{
				/* drain: in_samples == 0 until nothing more comes out */
				do
				{
					e = faacenc_push(ctx, NULL, 0, &got);
					if (e) break;
				} while (got);
			}
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}

	if (!ctx->enc)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_SERVICE_ERROR;
	}

	in_data = (const u8 *)gf_filter_pck_get_data(pck, &size);
	if (!in_data || !ctx->num_channels)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}

	/* interleaved S16: total sample count across channels. The library
	 * buffers internally but accepts at most one frame's worth per call */
	total_samples = size / 2;
	chunk_max = ctx->frame_samples * ctx->num_channels;
	for (pos = 0; pos < total_samples; )
	{
		u32 n = total_samples - pos;
		if (n > chunk_max) n = chunk_max;
		e = faacenc_push(ctx, in_data + pos * 2, n, &got);
		if (e)
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			return e;
		}
		pos += n;
	}

	gf_filter_pid_drop_packet(ctx->ipid);
	return GF_OK;
}

static void faacenc_finalize(GF_Filter *filter)
{
	GF_FaacEncCtx *ctx = (GF_FaacEncCtx *)gf_filter_get_udta(filter);
	faacenc_close(ctx);
}

static const GF_FilterCapability FaacEncCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_AUDIO_FORMAT, GF_AUDIO_FMT_S16),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_AAC_MPEG4),
};

#define OFFS(_n) #_n, offsetof(GF_FaacEncCtx, _n)
static GF_FilterArgs FaacEncArgs[] =
	{
		{OFFS(bitrate), "target bitrate in kbps, all channels together", GF_PROP_UINT, "128", NULL, GF_FS_ARG_HINT_ADVANCED},
		{0}};

GF_FilterRegister FaacEncRegister = {
	.name = "encfaac",
	GF_FS_SET_DESCRIPTION("AAC-LC audio encoder (native libfaac)")
		GF_FS_SET_HELP("This filter encodes a raw S16 PCM audio PID to MPEG-4 AAC-LC by calling "
					   "libfaac's native API directly. Output is raw AAC frames with an "
					   "out-of-band AudioSpecificConfig, ready for mp4 muxing.")
			.private_size = sizeof(GF_FaacEncCtx),
	.args = FaacEncArgs,
	SETCAPS(FaacEncCaps),
	.configure_pid = faacenc_configure_pid,
	.process = faacenc_process,
	.finalize = faacenc_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE encfaac_register(GF_FilterSession *session)
{
	return &FaacEncRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_encfaac(void) {
    gf_filter_auto_register("encfaac", encfaac_register);
}
