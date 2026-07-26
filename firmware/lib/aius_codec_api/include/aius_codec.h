#ifndef AIUS_CODEC_H
#define AIUS_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Portable C ABI for the Aius IEC 61937 parser and DTS Core decoder.
 *
 * These declarations are framework-neutral so the identical code compiles for
 * host tests and the Teensy target. Implementations land per capability-gate
 * phase (parser P2, decoder P3); this header is the stable contract they fill. */

#define AIUS_CODEC_ABI_VERSION 1u

/* 5.1 is the widest layout in scope. */
#define AIUS_MAX_CHANNELS 6

/* Typed result for every parser/decoder operation. A non-OK status never leaves
 * stale PCM in the caller's sink. */
typedef enum {
	AIUS_OK = 0,
	AIUS_NEED_MORE,   /* input exhausted mid-unit; caller supplies more */
	AIUS_UNSUPPORTED, /* well-formed but out of scope (e.g. DTS-HD, DTS Express) */
	AIUS_MALFORMED,   /* sync, length, or CRC violation */
	AIUS_TRUNCATED,   /* stream ended inside a unit */
	AIUS_NO_SPACE,    /* arena or output capacity exhausted */
	AIUS_INVALID_ARG
} AiusStatus;

/* Canonical channel bits for AiusPcmBlock.channel_mask. popcount(mask) == channels. */
enum {
	AIUS_CH_FL = 1u << 0,  /* front left */
	AIUS_CH_FR = 1u << 1,  /* front right */
	AIUS_CH_FC = 1u << 2,  /* front centre */
	AIUS_CH_LFE = 1u << 3, /* low-frequency effects */
	AIUS_CH_SL = 1u << 4,  /* surround left */
	AIUS_CH_SR = 1u << 5   /* surround right */
};

/* SAMPLE SCALE — the contract every AiusPcmBlock producer owes its consumer.
 *
 * Planes are int32_t, but the *container* width is not the *sample* width, and
 * leaving that unsaid cost 48 dB. This header used to say only "signed 32-bit",
 * so each producer picked its own full scale: the DTS decoder reconstructs to 24
 * significant bits, while the pipeline's linear-PCM pass-through sign-extended
 * the 16-bit wire word and emitted it "verbatim". Both satisfied "signed 32-bit"
 * and they differed by exactly 2^8, so switching a live source from DTS to PCM
 * stepped the level by 48.16 dB. Neither was wrong against the contract, because
 * there was no contract -- and the pass-through's own test asserted the block
 * against a copy of the producer's expression, so it pinned the defect green.
 *
 * So: **a sample is signed, left-aligned to AIUS_PCM_SAMPLE_BITS, sign-extended
 * into int32_t.** A producer whose native width is narrower scales UP to this
 * range; it does not hand over its own units and leave the scale to the reader.
 * Full scale is AIUS_PCM_SAMPLE_MAX/MIN, and dBFS is always relative to those.
 *
 * 24 because it is the widest source width in scope (the DTS Core's
 * reconstruction) and the S/PDIF transmit word carries it whole, so the shipped
 * path neither narrows nor widens. Retarget all three constants together; the DSP
 * derives AIUS_DSP_SAMPLE_MAX/MIN from them rather than restating them, and both
 * aius_dsp.c and aius_pipeline.c bind to them with a _Static_assert. */
#define AIUS_PCM_SAMPLE_BITS 24
#define AIUS_PCM_SAMPLE_MAX ((int32_t)8388607)  /*  2^23 - 1 */
#define AIUS_PCM_SAMPLE_MIN ((int32_t)-8388608) /* -2^23     */

/* One block of decoded audio: `channels` planar buffers of `frames` samples
 * each, signed 32-bit at the sample scale above, at `sample_rate_hz`. Planes are
 * arena- or caller-owned. */
typedef struct {
	int32_t *planes[AIUS_MAX_CHANNELS];
	uint16_t channels;
	uint32_t frames;
	uint32_t sample_rate_hz;
	uint32_t channel_mask; /* OR of AIUS_CH_*; popcount == channels */
} AiusPcmBlock;

/* One IEC 61937 data burst emitted by the parser, normalized to big-endian
 * payload bytes regardless of the wire's 16/14-bit endian representation. */
typedef struct {
	uint16_t data_type;    /* Pc data-type field */
	const uint8_t *payload; /* burst payload */
	uint32_t length;        /* payload length in bytes */
} AiusBurst;

/* Per-operation measurements, read after a decode. On the host, target-only
 * fields (cycles, stack) are 0. */
typedef struct {
	uint64_t decode_cycles;    /* DWT cycle count on target, 0 on host */
	uint32_t arena_high_water; /* bytes */
	uint32_t stack_high_water; /* bytes, target-only */
	uint32_t alloc_count;      /* allocations on the last decode path (0 expected) */
	AiusStatus last_status;
} AiusMetrics;

/* Opaque codec instances, sized and placed by the caller via the arena. */
typedef struct AiusIec61937Parser AiusIec61937Parser;
typedef struct AiusDtsDecoder AiusDtsDecoder;

/* Destination for decoded blocks, decoupled from Arduino audio objects. */
typedef struct {
	void *ctx;
	AiusStatus (*consume)(void *ctx, const AiusPcmBlock *block);
} AiusPcmSink;

#ifdef __cplusplus
}
#endif

#endif /* AIUS_CODEC_H */
