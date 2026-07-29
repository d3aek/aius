#ifndef AIUS_A52_H
#define AIUS_A52_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"
#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AC-3 (Dolby Digital) decoder adapter (aius-ac3-support.md phase 1).
 *
 * Wraps the identity-only liba52 fork (lib/aius_a52/a52) behind the portable
 * AiusA52Decoder handle, mirroring the AiusDtsDecoder contract. Feeds one raw
 * AC-3 access unit at a time (the IEC 61937 parser's complete burst payload,
 * starting at the 0x0B77 sync word) and emits planar PCM in the AIUS_CH_*
 * channel convention at the AIUS_PCM_SAMPLE_BITS scale. Begin validates and
 * arms one access unit; six next calls emit its 256-frame blocks.
 *
 * Allocation mirrors aius_dts: create routes through libc, create_arena
 * routes ALL allocation (handle, scratch, planes, and liba52's state +
 * sample buffer) through a caller-owned AiusArena via the aius_alloc_shim
 * link-time wrap. liba52 allocates only at init; steady-state decode
 * allocates nothing (aius_a52_get_metrics.alloc_count proves it).
 *
 * SAFETY SHAPE, unlike upstream liba52: the fork's bit reader has a byte-exact,
 * fail-closed end bound. The adapter sets it from the validated 48 kHz frame
 * length before a52_frame and copies into one AIUS_A52_MAX_FRAME scratch.
 * a52_frame validates CRC1, CRC2, and their chained residue before parsing;
 * the reader substitutes zeros at the bound and never reads beyond that owned
 * frame allocation (contract + derivation: lib/aius_a52/a52/NOTICE.md).
 *
 * Output flags always request the stream's OWN coding
 * (syncinfo & (A52_CHANNEL_MASK|A52_LFE)), level 1.0, bias 0. The fork
 * contains no downmix/upmix machinery and rejects a non-identity request.
 * The plane mapping is derived from the SYNCINFO flags (direct core tests
 * pin their agreement with a52_frame, including A52_DOLBY). Dynamic range
 * compression stays at liba52's default (dynrng APPLIED -- the plan records
 * this as a decision).
 *
 * Channel modes carried: MONO (FC), STEREO / DOLBY (FL FR), 3F (FL FC FR),
 * 2F2R (FL FR SL SR), 3F2R (FL FR FC SL SR), each with or without LFE.
 * Mono-surround (2F1R/3F1R) and dual-mono (CHANNEL/CHANNEL1/CHANNEL2)
 * return AIUS_UNSUPPORTED: AIUS has no CS plane, and inventing a fold here
 * would be downmix policy, which belongs to the DSP chain. The box stays
 * exactly as dark for those rare modes as it is for all AC-3 today. */

/* The AC-3 maximum frame size (frmsizecod ceiling: 640 kbps at 32 kHz). */
#define AIUS_A52_MAX_FRAME 3840u

/* AC-3 access-unit shape: six independently emitted 256-frame blocks. */
#define AIUS_A52_BLOCK_SAMPLES 256u
#define AIUS_A52_BLOCKS_PER_ACCESS_UNIT 6u
#define AIUS_A52_FRAME_SAMPLES \
    (AIUS_A52_BLOCK_SAMPLES * AIUS_A52_BLOCKS_PER_ACCESS_UNIT)

typedef struct AiusA52Decoder AiusA52Decoder;

/* Create a decoder whose allocation routes through libc. On success *out holds
 * a heap-owned handle; destroy with aius_a52_decoder_destroy. Returns AIUS_OK
 * or AIUS_NO_SPACE / AIUS_INVALID_ARG. */
AiusStatus aius_a52_decoder_create(AiusA52Decoder **out);

/* Create a decoder whose handle, scratch, planes, and every liba52 allocation
 * come from `arena` — no libc heap once provisioned. `arena` must outlive the
 * decoder and be dedicated to it (size from the measured high-water via
 * aius_a52_get_metrics). Returns AIUS_OK, AIUS_NO_SPACE, or AIUS_INVALID_ARG. */
AiusStatus aius_a52_decoder_create_arena(AiusA52Decoder **out, AiusArena *arena);

/* Begin one AC-3 access unit. `data`/`size` need not be aligned; the adapter
 * copies the complete AU into owned scratch that remains live through all six
 * block calls. Size < 7 and sync/length/bsid/CRC violations return
 * AIUS_MALFORMED; sample rate != 48 kHz and unmappable channel modes return
 * AIUS_UNSUPPORTED. A live iterator is reset before replacement. */
AiusStatus aius_a52_begin_access_unit(AiusA52Decoder *dec,
                                      const uint8_t *data, size_t size);

/* Decode exactly one 256-frame block from the live AU. Six calls succeed; a
 * seventh or a call without a live begin returns AIUS_INVALID_ARG and never
 * invokes the core. A core error resets and disarms the iterator. On AIUS_OK,
 * `block` holds decoder-owned AIUS_CH_* ordered planes valid only until the
 * next next/begin/destroy call. */
AiusStatus aius_a52_decode_next_block(AiusA52Decoder *dec,
                                      AiusPcmBlock *block);

/* Stop a live AU and reset overlap, dither, and reader state. This is required
 * when a caller stops before block six (for example, sink refusal). Valid idle
 * decoders return AIUS_OK without disturbing completed-stream continuity. */
AiusStatus aius_a52_abandon(AiusA52Decoder *dec);

/* Copy the measurements from the most recent iterator call into *out. Mirrors
 * aius_dts_get_metrics: arena_high_water and alloc_count populated on both
 * provisioning modes; decode_cycles / stack_high_water stay 0 on the host. */
AiusStatus aius_a52_get_metrics(const AiusA52Decoder *dec, AiusMetrics *out);

#ifdef AIUS_A52_BOUND_PROBE
/* Test-only positive proof for F3a's reader bound. Counts are cumulative over
 * the decoder lifetime. Every fill must observe the frame-derived end exactly;
 * suppressed_fills must be non-zero over the shipped fuzz corpus. */
AiusStatus aius_a52_get_bound_probe(const AiusA52Decoder *dec,
                                    uint32_t *fills,
                                    uint32_t *suppressed_fills,
                                    uint32_t *end_mismatches);
#endif

/* Release a decoder and all of its owned memory. NULL is a no-op. An
 * arena-backed decoder's storage is reclaimed by resetting or discarding the
 * caller's arena; this call runs a52_free but does not reset the arena. */
void aius_a52_decoder_destroy(AiusA52Decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_A52_H */
