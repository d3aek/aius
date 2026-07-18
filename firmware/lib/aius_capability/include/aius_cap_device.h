#ifndef AIUS_CAP_DEVICE_H
#define AIUS_CAP_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"
#include "aius_capability.h"
#include "aius_codec.h"
#include "aius_dts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device-side capability handler (capability-gate phase P5).
 *
 * Portable C: parses one decoded command frame, drives an arena-backed
 * AiusDtsDecoder, and emits response frame(s) through a byte sink. The identical
 * code runs behind the in-process simulated device (host tests) and, at P6,
 * behind the real Teensy USB-Serial transport. No Arduino, no libc heap on the
 * decode path — every decoder allocation comes from the caller-owned arena.
 */

/* Cached-PCM capacity: AIUS_MAX_CHANNELS planes of up to this many samples. A
 * DTS Core access unit emits at most ~1024 samples/channel; 2048 is headroom. */
#define AIUS_CAP_DEV_MAX_PCM_FRAMES 2048u
#define AIUS_CAP_DEV_PCM_CAP (AIUS_MAX_CHANNELS * AIUS_CAP_DEV_MAX_PCM_FRAMES)

/* Sink for an encoded response frame. `frame`/`len` are valid only for the
 * duration of the call. Multiple calls stream a multi-part response. */
typedef void (*AiusCapEmit)(void *ctx, const uint8_t *frame, size_t len);

typedef struct {
	AiusArena     *arena;
	AiusDtsDecoder *dec;
	AiusStatus     create_status; /* status of the arena-backed create */
	int            dec_ready;
	int            warmed;         /* first decode (warmup) has run */

	/* Duplicate-sequence rejection: the immediately-previous accepted seq. */
	uint16_t last_sequence;
	int      have_last_seq;

	/* Last successfully decoded block, cached channel-major planar. */
	int      have_pcm;
	uint16_t pcm_channels;
	uint32_t pcm_frames;
	uint32_t pcm_sample_rate;
	uint32_t pcm_mask;
	int32_t  pcm[AIUS_CAP_DEV_PCM_CAP];

	/* Cumulative metrics since init / last reset-metrics. */
	uint32_t cum_submitted;
	uint32_t cum_ok;
	uint32_t cum_failed;
	uint64_t cum_total_samples;
	uint32_t cum_peak_high_water;
	uint32_t cum_alloc_violations;
	uint8_t  cum_last_status;

	/* Scratch for building one outbound frame. */
	uint8_t  out[AIUS_CAP_MAX_FRAME];
} AiusCapDevice;

/* Provision the device: bind the decoder to `arena`. Returns the decoder create
 * status (AIUS_OK on success, AIUS_NO_SPACE / AIUS_INVALID_ARG otherwise). Even
 * on a create failure the device is usable — commands that need the decoder
 * respond with a typed diagnostic rather than crashing. */
AiusStatus aius_cap_device_init(AiusCapDevice *dev, AiusArena *arena);

/* Release the decoder. The arena's storage is reclaimed by the caller. */
void aius_cap_device_destroy(AiusCapDevice *dev);

/* Handle one decoded command frame, emitting response frame(s) via `emit`. */
void aius_cap_device_handle(AiusCapDevice *dev, const AiusCapFrame *cmd,
                            AiusCapEmit emit, void *emit_ctx);

/* Emit a NACK for a framer-level error event (bad CRC, bad version, ...). */
void aius_cap_device_handle_error(AiusCapDevice *dev, const AiusCapFrame *err,
                                  AiusCapEmit emit, void *emit_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_CAP_DEVICE_H */
