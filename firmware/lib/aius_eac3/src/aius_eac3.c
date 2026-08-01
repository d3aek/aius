/* AiusEac3Decoder: E-AC-3 (Dolby Digital Plus) adapter over the vendored
 * FFmpeg fork. See ../include/aius_eac3.h for the contract and
 * vendor/ffmpeg-eac3/NOTICE.md for the fork's provenance and edits.
 *
 * The fork's calling convention is the upstream-static decoder driven
 * directly: aius_eac3_core_init/frame/end over a fork-owned AVCodecContext,
 * with output planes supplied through the framework shim's provider hook.
 * The provider hands out this adapter's plane storage; after a successful
 * decode the float samples are quantized IN PLACE to int32 at the
 * AIUS_PCM_SAMPLE_BITS scale (both are 4 bytes; the punning is done with
 * memcpy, which is defined behaviour), so one 1536-frame plane set serves
 * both roles and the slot pays for it once. */

#include "aius_eac3.h"

#include <stdlib.h>
#include <string.h>

#include "aius_alloc_shim.h"

#include "libavcodec/ac3dec.h"
#include "libavutil/error.h"

#include "aius_avshim.h"

#define EAC3_PLANE_BYTES (AIUS_EAC3_MAX_FRAME_SAMPLES * sizeof(int32_t))

/* AV native channel-mask bits for the layouts this adapter carries. The
 * plane order of both conventions is "ascending mask bit", and the six
 * carried positions rank identically on both sides, so plane c maps to
 * plane c; only the mask value is translated. */
#define EAC3_AV_FL (1ull << 0)
#define EAC3_AV_FR (1ull << 1)
#define EAC3_AV_FC (1ull << 2)
#define EAC3_AV_LFE (1ull << 3)
#define EAC3_AV_SL (1ull << 9)
#define EAC3_AV_SR (1ull << 10)
#define EAC3_AV_CARRIED \
	(EAC3_AV_FL | EAC3_AV_FR | EAC3_AV_FC | EAC3_AV_LFE | EAC3_AV_SL | \
	 EAC3_AV_SR)

struct AiusEac3Decoder {
	AVCodecContext avctx;
	AVFrame frame;
	AVPacket pkt;
	AC3DecodeContext *ctx;   /* priv_data; separately allocated (large) */
	uint8_t *planes[AIUS_MAX_CHANNELS];
	AiusArena *arena;        /* NULL on the libc path */
	AiusCycleClock cycle_clock;
	AiusMetrics metrics;
};

static uint32_t translate_mask(uint64_t av_mask)
{
	uint32_t m = 0;
	if (av_mask & EAC3_AV_FL)
		m |= AIUS_CH_FL;
	if (av_mask & EAC3_AV_FR)
		m |= AIUS_CH_FR;
	if (av_mask & EAC3_AV_FC)
		m |= AIUS_CH_FC;
	if (av_mask & EAC3_AV_LFE)
		m |= AIUS_CH_LFE;
	if (av_mask & EAC3_AV_SL)
		m |= AIUS_CH_SL;
	if (av_mask & EAC3_AV_SR)
		m |= AIUS_CH_SR;
	return m;
}

/* Output-plane provider registered with the framework shim (process-global;
 * one live decoder by contract). */
static int provide_planes(void *opaque, AVCodecContext *avctx, AVFrame *frame)
{
	AiusEac3Decoder *dec = opaque;
	(void)avctx;
	const int ch = frame->ch_layout.nb_channels;
	if (ch < 1 || ch > AIUS_MAX_CHANNELS ||
	    frame->nb_samples < 1 ||
	    (unsigned)frame->nb_samples > AIUS_EAC3_MAX_FRAME_SAMPLES)
		return AVERROR(EINVAL);
	for (int c = 0; c < ch; c++)
		frame->data[c] = dec->planes[c];
	return 0;
}

static void decoder_call_start(AiusEac3Decoder *dec)
{
	aius_avshim_set_get_buffer(provide_planes, dec);
	if (dec->arena) {
		aius_alloc_shim_set_arena(dec->arena);
		aius_alloc_shim_reset_count();
	}
}

static AiusStatus decoder_call_end(AiusEac3Decoder *dec, AiusStatus st)
{
	if (dec->arena) {
		dec->metrics.alloc_count = aius_alloc_shim_alloc_count();
		dec->metrics.arena_high_water =
			(uint32_t)aius_arena_high_water(dec->arena);
		aius_alloc_shim_set_arena(NULL);
	} else {
		dec->metrics.alloc_count = 0;
	}
	dec->metrics.last_status = st;
	return st;
}

static AiusStatus decoder_create(AiusEac3Decoder **out, AiusArena *arena)
{
	if (!out)
		return AIUS_INVALID_ARG;
	*out = NULL;

	if (arena)
		aius_alloc_shim_set_arena(arena);

	AiusStatus st = AIUS_NO_SPACE;
	AiusEac3Decoder *dec = calloc(1, sizeof(*dec));
	if (!dec)
		goto done;
	dec->arena = arena;

	dec->ctx = calloc(1, sizeof(*dec->ctx));
	if (!dec->ctx)
		goto fail;
	for (int c = 0; c < AIUS_MAX_CHANNELS; c++) {
		dec->planes[c] = malloc(EAC3_PLANE_BYTES);
		if (!dec->planes[c])
			goto fail;
		memset(dec->planes[c], 0, EAC3_PLANE_BYTES);
	}

	dec->avctx.priv_data = dec->ctx;
	/* The AVOption defaults libavcodec's registry used to apply; the only
	 * non-zero one the decode path reads. */
	dec->ctx->drc_scale = 1.0f;

	aius_avshim_set_get_buffer(provide_planes, dec);
	if (aius_eac3_core_init(&dec->avctx) < 0)
		goto fail;

	if (arena)
		dec->metrics.arena_high_water =
			(uint32_t)aius_arena_high_water(arena);
	*out = dec;
	st = AIUS_OK;
	goto done;

fail:
	if (dec) {
		for (int c = 0; c < AIUS_MAX_CHANNELS; c++)
			free(dec->planes[c]);
		free(dec->ctx);
		free(dec);
	}
done:
	if (arena)
		aius_alloc_shim_set_arena(NULL);
	return st;
}

AiusStatus aius_eac3_decoder_create(AiusEac3Decoder **out)
{
	return decoder_create(out, NULL);
}

AiusStatus aius_eac3_decoder_create_arena(AiusEac3Decoder **out,
                                          AiusArena *arena)
{
	if (!arena)
		return AIUS_INVALID_ARG;
	return decoder_create(out, arena);
}

AiusStatus aius_eac3_set_cycle_clock(AiusEac3Decoder *dec,
                                     const AiusCycleClock *clock)
{
	if (!dec)
		return AIUS_INVALID_ARG;
	if (clock)
		dec->cycle_clock = *clock;
	else
		memset(&dec->cycle_clock, 0, sizeof(dec->cycle_clock));
	if (!dec->cycle_clock.read)
		dec->metrics.decode_cycles = 0;
	return AIUS_OK;
}

/* One saturating float->int32 quantize at the AIUS_PCM_SAMPLE_BITS scale
 * (aius_a52's expression). */
static int32_t sample_quantize(float v)
{
	float scaled = v * 8388608.0f; /* 2^23: full scale at 24 bits */
	if (scaled >= (float)AIUS_PCM_SAMPLE_MAX)
		return AIUS_PCM_SAMPLE_MAX;
	if (scaled <= (float)AIUS_PCM_SAMPLE_MIN)
		return AIUS_PCM_SAMPLE_MIN;
	return (int32_t)scaled;
}

static AiusStatus map_core_error(int ret)
{
	if (ret == AVERROR_PATCHWELCOME || ret == AVERROR(ENOSYS))
		return AIUS_UNSUPPORTED;
	if (ret == AVERROR(ENOMEM))
		return AIUS_NO_SPACE;
	return AIUS_MALFORMED;
}

AiusStatus aius_eac3_decode_access_unit(AiusEac3Decoder *dec,
                                        const uint8_t *data, size_t size,
                                        AiusPcmBlock *block)
{
	if (!dec || !block || (!data && size > 0))
		return AIUS_INVALID_ARG;
	memset(block, 0, sizeof(*block));
	if (size < 6 || size > AIUS_EAC3_MAX_BURST)
		return AIUS_MALFORMED;

	decoder_call_start(dec);

	uint32_t t0 = 0;
	const int timed = dec->cycle_clock.read != NULL;
	if (timed)
		t0 = dec->cycle_clock.read(dec->cycle_clock.ctx);

	memset(&dec->frame, 0, sizeof(dec->frame));
	memset(&dec->pkt, 0, sizeof(dec->pkt));
	dec->pkt.data = (uint8_t *)data;
	dec->pkt.size = (int)size;

	int got = 0;
	int ret = aius_eac3_core_frame(&dec->avctx, &dec->frame, &got,
	                               &dec->pkt);

	uint64_t cycles = 0;
	if (timed) {
		uint32_t t1 = dec->cycle_clock.read(dec->cycle_clock.ctx);
		cycles = (uint32_t)(t1 - t0);
	}

	if (ret < 0)
		return decoder_call_end(dec, map_core_error(ret));
	if (!got)
		return decoder_call_end(dec, AIUS_MALFORMED);

	/* Scope gates: rates and layouts the box carries. The fork already
	 * confines output to the independent substream. */
	const uint32_t rate = (uint32_t)dec->avctx.sample_rate;
	if (rate != 32000u && rate != 44100u && rate != 48000u)
		return decoder_call_end(dec, AIUS_UNSUPPORTED);
	if (dec->ctx->channel_mode == AC3_CHMODE_DUALMONO)
		return decoder_call_end(dec, AIUS_UNSUPPORTED);
	const int ch = dec->frame.ch_layout.nb_channels;
	const uint64_t av_mask = dec->frame.ch_layout.u.mask;
	if (ch < 1 || ch > AIUS_MAX_CHANNELS ||
	    (av_mask & ~EAC3_AV_CARRIED) != 0u)
		return decoder_call_end(dec, AIUS_UNSUPPORTED);

	/* Quantize in place: the decoder wrote float; the block hands out
	 * int32 in the same storage. memcpy punning keeps it defined. */
	const uint32_t frames = (uint32_t)dec->frame.nb_samples;
	for (int c = 0; c < ch; c++) {
		uint8_t *p = dec->planes[c];
		for (uint32_t n = 0; n < frames; n++) {
			float f;
			memcpy(&f, p + 4u * n, 4u);
			const int32_t q = sample_quantize(f);
			memcpy(p + 4u * n, &q, 4u);
		}
		block->planes[c] = (int32_t *)(void *)dec->planes[c];
	}
	block->channels = (uint16_t)ch;
	block->frames = frames;
	block->sample_rate_hz = rate;
	block->channel_mask = translate_mask(av_mask);

	dec->metrics.decode_cycles = cycles;
	return decoder_call_end(dec, AIUS_OK);
}

AiusStatus aius_eac3_get_metrics(const AiusEac3Decoder *dec, AiusMetrics *out)
{
	if (!dec || !out)
		return AIUS_INVALID_ARG;
	*out = dec->metrics;
	return AIUS_OK;
}

void aius_eac3_decoder_destroy(AiusEac3Decoder *dec)
{
	if (!dec)
		return;
	/* The shim frees by pointer range, so arena blocks route back to the
	 * arena even while disarmed (the aius_dts/aius_a52 shape). */
	(void)aius_eac3_core_end(&dec->avctx);
	for (int c = 0; c < AIUS_MAX_CHANNELS; c++)
		free(dec->planes[c]);
	free(dec->ctx);
	free(dec);
}
