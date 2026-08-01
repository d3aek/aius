#ifndef AIUS_EAC3_H
#define AIUS_EAC3_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"
#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* E-AC-3 (Dolby Digital Plus) decoder adapter (aius-eac3-support.md phase 2).
 *
 * Wraps the vendored FFmpeg fork (vendor/ffmpeg-eac3) behind the portable
 * AiusEac3Decoder handle. Single-shot like AiusDtsDecoder: one call decodes
 * one complete E-AC-3 access unit (the IEC 61937 type-21 burst payload,
 * starting at the 0x0B77 sync word) into one decoder-owned AiusPcmBlock of
 * num_blocks x 256 frames (256..1536), planar int32 at the
 * AIUS_PCM_SAMPLE_BITS scale, AIUS_CH_* plane order (ascending mask bit).
 * The pipeline slices the block through aius_pipeline_slice_pcm_block
 * exactly as it does DTS output.
 *
 * 5.1-ONLY BY CONSTRUCTION (owner decision 2026-08-01): the fork decodes
 * the independent substream and CONSUMES any dependent substreams in the
 * burst without decoding them (NOTICE.md [eac3-51-only]). Dual-mono and
 * the mono-surround modes (2F1R/3F1R: a back-centre AIUS has no plane for)
 * return AIUS_UNSUPPORTED, mirroring aius_a52; reduced sample rates
 * (E-AC-3 fscod2: 16/22.05/24 kHz) are likewise AIUS_UNSUPPORTED. Carried:
 * MONO (FC), STEREO (FL FR), 3F, 2F2R, 3F2R, each with or without LFE, at
 * 32/44.1/48 kHz.
 *
 * Allocation mirrors aius_dts/aius_a52: create routes through libc,
 * create_arena routes ALL allocation (handle, planes, and every FFmpeg
 * allocation: the decode context, av_tx twiddles, float-dsp, downmix
 * scratch) through a caller-owned AiusArena via the aius_alloc_shim
 * link-time wrap. The fork allocates at init/first-decode warmup only;
 * steady-state decode allocates nothing (get_metrics.alloc_count proves
 * it). Exactly one live AiusEac3Decoder is supported per process: the
 * fork's framework shim (aius_avshim) registers a process-global output
 * provider, which is fine on the box (one codec slot) and in the suites. */

/* E-AC-3 syncframe ceiling: an 11-bit frmsiz counts 16-bit words, so one
 * frame is at most (2048)*2 bytes. A burst may carry more (dependent
 * substreams follow the independent frame); the adapter accepts up to a
 * full type-21 burst payload. */
#define AIUS_EAC3_MAX_FRAME 4096u
#define AIUS_EAC3_MAX_BURST 24576u

/* Access-unit shape: 1, 2, 3, or 6 audio blocks of 256 samples. */
#define AIUS_EAC3_BLOCK_SAMPLES 256u
#define AIUS_EAC3_MAX_BLOCKS 6u
#define AIUS_EAC3_MAX_FRAME_SAMPLES \
	(AIUS_EAC3_BLOCK_SAMPLES * AIUS_EAC3_MAX_BLOCKS)

typedef struct AiusEac3Decoder AiusEac3Decoder;

/* Create a decoder whose allocation routes through libc. On success *out
 * holds a heap-owned handle; destroy with aius_eac3_decoder_destroy.
 * Returns AIUS_OK or AIUS_NO_SPACE / AIUS_INVALID_ARG. */
AiusStatus aius_eac3_decoder_create(AiusEac3Decoder **out);

/* Create a decoder whose handle, planes, and every FFmpeg allocation come
 * from `arena` — no libc heap once provisioned. `arena` must outlive the
 * decoder and be dedicated to it (size from the measured high-water via
 * aius_eac3_get_metrics). Returns AIUS_OK, AIUS_NO_SPACE, or
 * AIUS_INVALID_ARG. */
AiusStatus aius_eac3_decoder_create_arena(AiusEac3Decoder **out,
                                          AiusArena *arena);

/* Copy the cycle-clock callback into the decoder. NULL or a clock with a
 * NULL reader disables timing. */
AiusStatus aius_eac3_set_cycle_clock(AiusEac3Decoder *dec,
                                     const AiusCycleClock *clock);

/* Decode one complete E-AC-3 access unit. `data`/`size` need not be
 * aligned. Sync/length/CRC violations and a burst that yields no frame
 * return AIUS_MALFORMED; out-of-scope shapes (dual mono, back-centre
 * modes, reduced rates, >5.1 output) return AIUS_UNSUPPORTED. On AIUS_OK,
 * `block` holds decoder-owned planes valid until the next decode/destroy
 * call; frames is num_blocks x 256. Dependent substreams in the burst are
 * consumed, never decoded. A failed decode leaves no partial output. */
AiusStatus aius_eac3_decode_access_unit(AiusEac3Decoder *dec,
                                        const uint8_t *data, size_t size,
                                        AiusPcmBlock *block);

/* Copy the measurements into *out. Mirrors aius_dts_get_metrics:
 * arena_high_water and alloc_count are populated on both provisioning
 * modes; decode_cycles covers the last successful access unit and stays 0
 * with no clock. */
AiusStatus aius_eac3_get_metrics(const AiusEac3Decoder *dec,
                                 AiusMetrics *out);

/* Release a decoder and all of its owned memory. NULL is a no-op. An
 * arena-backed decoder's storage is reclaimed by resetting or discarding
 * the caller's arena; this call still runs the fork's teardown. */
void aius_eac3_decoder_destroy(AiusEac3Decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_EAC3_H */
