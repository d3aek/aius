/* Device-side capability handler (capability-gate phase P5). Portable C.
 *
 * Parses one decoded command, drives the arena-backed DTS Core decoder, and
 * streams response frames through the caller's emit sink. */

#include "aius_cap_device.h"

#include <string.h>

AiusStatus aius_cap_device_init(AiusCapDevice *dev, AiusArena *arena)
{
	memset(dev, 0, sizeof(*dev));
	dev->arena = arena;
	dev->cum_last_status = (uint8_t)AIUS_OK;
	dev->create_status = aius_dts_decoder_create_arena(&dev->dec, arena);
	dev->dec_ready = (dev->create_status == AIUS_OK);
	return dev->create_status;
}

void aius_cap_device_destroy(AiusCapDevice *dev)
{
	if (dev->dec != NULL) {
		aius_dts_decoder_destroy(dev->dec);
		dev->dec = NULL;
	}
	dev->dec_ready = 0;
}

/* Encode `payload` as a response of `type` echoing `seq`, and emit it. */
static void emit_response(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                          uint8_t type, uint16_t seq, const uint8_t *payload,
                          uint32_t payload_len)
{
	size_t n = aius_cap_encode(dev->out, sizeof(dev->out), type, seq, payload,
	                           payload_len);
	if (n != 0)
		emit(emit_ctx, dev->out, n);
}

static void emit_ack(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx, uint16_t seq)
{
	emit_response(dev, emit, emit_ctx, AIUS_CAP_RSP_ACK, seq, NULL, 0);
}

static void emit_nack(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                      uint16_t seq, AiusCapError err)
{
	uint8_t p[AIUS_CAP_NACK_LEN];
	AiusCapNack n = { .error_code = (uint8_t)err, .offending_sequence = seq };
	size_t len = aius_cap_put_nack(p, sizeof(p), &n);
	emit_response(dev, emit, emit_ctx, AIUS_CAP_RSP_NACK, seq, p, (uint32_t)len);
}

static void emit_diagnostic(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                            uint16_t seq, AiusCapError err, AiusStatus detail,
                            uint32_t context_index, const char *message)
{
	uint8_t p[AIUS_CAP_DIAGNOSTIC_HEADER_LEN + 128];
	AiusCapDiagnostic d = {
		.error_code = (uint8_t)err,
		.detail_status = (uint8_t)detail,
		.context_index = context_index,
	};
	uint16_t mlen = 0;
	if (message != NULL) {
		size_t sl = strlen(message);
		if (sl > 120)
			sl = 120;
		mlen = (uint16_t)sl;
	}
	size_t len = aius_cap_put_diagnostic(p, sizeof(p), &d, message, mlen);
	emit_response(dev, emit, emit_ctx, AIUS_CAP_RSP_DIAGNOSTIC, seq, p,
	              (uint32_t)len);
}

/* Cache a decoded block into the device planar PCM store. */
static void cache_pcm(AiusCapDevice *dev, const AiusPcmBlock *blk)
{
	uint32_t frames = blk->frames;
	if (frames > AIUS_CAP_DEV_MAX_PCM_FRAMES)
		frames = AIUS_CAP_DEV_MAX_PCM_FRAMES;
	dev->pcm_channels = blk->channels;
	dev->pcm_frames = frames;
	dev->pcm_sample_rate = blk->sample_rate_hz;
	dev->pcm_mask = blk->channel_mask;
	for (uint16_t c = 0; c < blk->channels && c < AIUS_MAX_CHANNELS; c++) {
		int32_t *dst = dev->pcm + (size_t)c * AIUS_CAP_DEV_MAX_PCM_FRAMES;
		if (blk->planes[c] != NULL)
			memcpy(dst, blk->planes[c], (size_t)frames * sizeof(int32_t));
		else
			memset(dst, 0, (size_t)frames * sizeof(int32_t));
	}
	dev->have_pcm = 1;
}

/* Decode one access unit, updating cumulative metrics and (optionally) the PCM
 * cache. `*m_out` receives the per-decode metrics. Returns the decode status. */
static AiusStatus decode_one(AiusCapDevice *dev, const uint8_t *au, uint32_t len,
                             AiusMetrics *m_out, int cache, int *alloc_violation)
{
	*alloc_violation = 0;
	memset(m_out, 0, sizeof(*m_out));
	if (!dev->dec_ready)
		return dev->create_status == AIUS_OK ? AIUS_INVALID_ARG
		                                     : dev->create_status;

	AiusPcmBlock blk;
	AiusStatus st = aius_dts_decode_access_unit(dev->dec, au, len, &blk);
	dev->cum_submitted++;
	aius_dts_get_metrics(dev->dec, m_out);

	if (dev->warmed) {
		if (m_out->alloc_count != 0)
			*alloc_violation = 1;
	} else {
		dev->warmed = 1; /* first decode is warmup; its allocation is expected */
	}

	if (m_out->arena_high_water > dev->cum_peak_high_water)
		dev->cum_peak_high_water = m_out->arena_high_water;
	dev->cum_last_status = (uint8_t)st;

	if (st == AIUS_OK) {
		dev->cum_ok++;
		dev->cum_total_samples += (uint64_t)blk.frames * blk.channels;
		if (cache)
			cache_pcm(dev, &blk);
	} else {
		dev->cum_failed++;
	}
	if (*alloc_violation)
		dev->cum_alloc_violations++;
	return st;
}

static void handle_identify(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                            uint16_t seq)
{
	AiusCapCapabilities c = {
		.abi_version = (uint8_t)AIUS_CODEC_ABI_VERSION,
		.proto_version = (uint8_t)AIUS_CAP_VERSION,
		.max_channels = (uint16_t)AIUS_MAX_CHANNELS,
		.max_payload = AIUS_CAP_MAX_PAYLOAD,
		.capability_flags =
			AIUS_CAP_FLAG_DTS_CORE | AIUS_CAP_FLAG_ARENA | AIUS_CAP_FLAG_SOAK,
	};
	memset(c.name, 0, sizeof(c.name));
	memcpy(c.name, "AIUS-DTS", 8);
	uint8_t p[AIUS_CAP_CAPABILITIES_LEN];
	size_t len = aius_cap_put_capabilities(p, sizeof(p), &c);
	emit_response(dev, emit, emit_ctx, AIUS_CAP_RSP_CAPABILITIES, seq, p,
	              (uint32_t)len);
}

static void reset_metrics(AiusCapDevice *dev)
{
	dev->cum_submitted = 0;
	dev->cum_ok = 0;
	dev->cum_failed = 0;
	dev->cum_total_samples = 0;
	dev->cum_peak_high_water = 0;
	dev->cum_alloc_violations = 0;
	dev->cum_last_status = (uint8_t)AIUS_OK;
}

static void emit_frame_result(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                              uint16_t seq, AiusStatus st, const AiusMetrics *m)
{
	AiusCapFrameResult r = {
		.status = (uint8_t)st,
		.channels = (st == AIUS_OK) ? dev->pcm_channels : 0,
		.frames = (st == AIUS_OK) ? dev->pcm_frames : 0,
		.sample_rate_hz = (st == AIUS_OK) ? dev->pcm_sample_rate : 0,
		.channel_mask = (st == AIUS_OK) ? dev->pcm_mask : 0,
		.arena_high_water = m->arena_high_water,
		.alloc_count = m->alloc_count,
	};
	uint8_t p[AIUS_CAP_FRAME_RESULT_LEN];
	size_t len = aius_cap_put_frame_result(p, sizeof(p), &r);
	emit_response(dev, emit, emit_ctx, AIUS_CAP_RSP_FRAME_RESULT, seq, p,
	              (uint32_t)len);
}

static void handle_submit_frame(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                                uint16_t seq, const uint8_t *au, uint32_t len)
{
	AiusMetrics m;
	int viol = 0;
	AiusStatus st = decode_one(dev, au, len, &m, /*cache=*/1, &viol);
	emit_frame_result(dev, emit, emit_ctx, seq, st, &m);
}

static void handle_request_pcm(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                               uint16_t seq)
{
	if (!dev->have_pcm) {
		emit_nack(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_NO_PCM);
		return;
	}
	AiusCapPcmHeader h = {
		.channels = dev->pcm_channels,
		.frames = dev->pcm_frames,
		.sample_rate_hz = dev->pcm_sample_rate,
		.channel_mask = dev->pcm_mask,
	};
	uint32_t sample_bytes =
		(uint32_t)dev->pcm_channels * dev->pcm_frames * (uint32_t)sizeof(int32_t);
	uint32_t payload_len = AIUS_CAP_PCM_HEADER_LEN + sample_bytes;
	if (payload_len > AIUS_CAP_MAX_PAYLOAD) {
		emit_nack(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_INTERNAL);
		return;
	}
	/* Build the payload in place in the frame scratch (after the 12-byte header
	 * slot), then frame it manually to avoid an overlapping in-place memcpy. */
	uint8_t *o = dev->out;
	uint8_t *pl = o + AIUS_CAP_HEADER_LEN;
	aius_cap_put_pcm_header(pl, AIUS_CAP_PCM_HEADER_LEN, &h);
	uint8_t *sp = pl + AIUS_CAP_PCM_HEADER_LEN;
	for (uint16_t c = 0; c < dev->pcm_channels; c++) {
		const int32_t *src = dev->pcm + (size_t)c * AIUS_CAP_DEV_MAX_PCM_FRAMES;
		for (uint32_t n = 0; n < dev->pcm_frames; n++) {
			aius_cap_put_u32(sp, (uint32_t)src[n]);
			sp += 4;
		}
	}
	o[0] = AIUS_CAP_MAGIC0;
	o[1] = AIUS_CAP_MAGIC1;
	o[2] = AIUS_CAP_MAGIC2;
	o[3] = AIUS_CAP_MAGIC3;
	o[4] = AIUS_CAP_VERSION;
	o[5] = AIUS_CAP_RSP_PCM_DATA;
	aius_cap_put_u16(o + 6, seq);
	aius_cap_put_u32(o + 8, payload_len);
	uint32_t crc = aius_cap_crc32(o, (size_t)AIUS_CAP_HEADER_LEN + payload_len);
	aius_cap_put_u32(o + AIUS_CAP_HEADER_LEN + payload_len, crc);
	emit(emit_ctx, o, (size_t)AIUS_CAP_HEADER_LEN + payload_len + AIUS_CAP_CRC_LEN);
}

/* Parse and decode a [u32 len][au]... batch; stream per-frame results (when
 * `stream` is set) and accumulate a summary. Returns 0 on a clean parse, -1 on
 * a malformed batch (a length field overruns the payload). */
static int run_batch(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                     uint16_t seq, const uint8_t *p, uint32_t len, int stream,
                     AiusCapSummary *sum)
{
	uint32_t pos = 0;
	while (pos < len) {
		if (pos + 4 > len)
			return -1;
		uint32_t au_len = aius_cap_get_u32(p + pos);
		pos += 4;
		if (pos + au_len > len)
			return -1;
		const uint8_t *au = p + pos;
		pos += au_len;

		AiusMetrics m;
		int viol = 0;
		AiusStatus st = decode_one(dev, au, au_len, &m, /*cache=*/1, &viol);
		sum->frames_submitted++;
		if (st == AIUS_OK) {
			sum->frames_ok++;
			sum->total_samples += (uint64_t)dev->pcm_frames * dev->pcm_channels;
		} else {
			sum->frames_failed++;
		}
		if (viol)
			sum->alloc_violations++;
		if (m.arena_high_water > sum->peak_arena_high_water)
			sum->peak_arena_high_water = m.arena_high_water;
		sum->last_status = (uint8_t)st;

		if (stream)
			emit_frame_result(dev, emit, emit_ctx, seq, st, &m);
	}
	return 0;
}

static void emit_summary(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                         uint16_t seq, const AiusCapSummary *sum)
{
	uint8_t p[AIUS_CAP_SUMMARY_LEN];
	size_t len = aius_cap_put_summary(p, sizeof(p), sum);
	emit_response(dev, emit, emit_ctx, AIUS_CAP_RSP_SUMMARY, seq, p,
	              (uint32_t)len);
}

static void handle_run_corpus(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                              uint16_t seq, const uint8_t *p, uint32_t len)
{
	AiusCapSummary sum;
	memset(&sum, 0, sizeof(sum));
	sum.iterations = 1;
	if (run_batch(dev, emit, emit_ctx, seq, p, len, /*stream=*/1, &sum) != 0) {
		emit_nack(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_MALFORMED_PAYLOAD);
		return;
	}
	emit_summary(dev, emit, emit_ctx, seq, &sum);
}

static void handle_run_soak(AiusCapDevice *dev, AiusCapEmit emit, void *emit_ctx,
                            uint16_t seq, const uint8_t *p, uint32_t len)
{
	if (len < 4) {
		emit_nack(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_MALFORMED_PAYLOAD);
		return;
	}
	uint32_t iters = aius_cap_get_u32(p);
	const uint8_t *batch = p + 4;
	uint32_t batch_len = len - 4;

	AiusCapSummary sum;
	memset(&sum, 0, sizeof(sum));
	sum.iterations = iters;
	int diagnosed = 0;
	for (uint32_t it = 0; it < iters; it++) {
		uint32_t before_viol = sum.alloc_violations;
		if (run_batch(dev, emit, emit_ctx, seq, batch, batch_len, /*stream=*/0,
		              &sum) != 0) {
			emit_nack(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_MALFORMED_PAYLOAD);
			return;
		}
		if (!diagnosed && sum.alloc_violations > before_viol) {
			emit_diagnostic(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_INTERNAL,
			                AIUS_OK, it, "steady-state allocation during soak");
			diagnosed = 1;
		}
		if (!diagnosed && sum.last_status != (uint8_t)AIUS_OK) {
			emit_diagnostic(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_DECODE,
			                (AiusStatus)sum.last_status, it, "soak decode error");
			diagnosed = 1;
		}
	}
	emit_summary(dev, emit, emit_ctx, seq, &sum);
}

void aius_cap_device_handle(AiusCapDevice *dev, const AiusCapFrame *cmd,
                            AiusCapEmit emit, void *emit_ctx)
{
	uint16_t seq = cmd->sequence;

	/* Duplicate-sequence rejection. */
	if (dev->have_last_seq && seq == dev->last_sequence) {
		emit_nack(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_DUP_SEQUENCE);
		return;
	}
	dev->last_sequence = seq;
	dev->have_last_seq = 1;

	switch (cmd->msg_type) {
	case AIUS_CAP_CMD_IDENTIFY:
		handle_identify(dev, emit, emit_ctx, seq);
		break;
	case AIUS_CAP_CMD_RESET_METRICS:
		reset_metrics(dev);
		emit_ack(dev, emit, emit_ctx, seq);
		break;
	case AIUS_CAP_CMD_SUBMIT_FRAME:
		handle_submit_frame(dev, emit, emit_ctx, seq, cmd->payload,
		                    cmd->payload_len);
		break;
	case AIUS_CAP_CMD_REQUEST_PCM:
		handle_request_pcm(dev, emit, emit_ctx, seq);
		break;
	case AIUS_CAP_CMD_RUN_CORPUS:
		handle_run_corpus(dev, emit, emit_ctx, seq, cmd->payload,
		                  cmd->payload_len);
		break;
	case AIUS_CAP_CMD_RUN_SOAK:
		handle_run_soak(dev, emit, emit_ctx, seq, cmd->payload, cmd->payload_len);
		break;
	default:
		emit_nack(dev, emit, emit_ctx, seq, AIUS_CAP_ERR_UNKNOWN_TYPE);
		break;
	}
}

void aius_cap_device_handle_error(AiusCapDevice *dev, const AiusCapFrame *err,
                                  AiusCapEmit emit, void *emit_ctx)
{
	emit_nack(dev, emit, emit_ctx, err->sequence, err->error);
}
