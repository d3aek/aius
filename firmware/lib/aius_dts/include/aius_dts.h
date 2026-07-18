#ifndef AIUS_DTS_H
#define AIUS_DTS_H

#include <stddef.h>
#include <stdint.h>

#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DTS Core decoder adapter (capability-gate phase P3).
 *
 * Wraps the vendored dcadec fixed-point DTS Core decoder behind the portable
 * AiusDtsDecoder handle. Feeds one raw DTS Core access unit at a time (16-bit
 * big-endian, starting at the 0x7FFE8001 sync word) and emits planar PCM in the
 * AIUS_CH_* channel convention.
 *
 * Scope note (P3, first correctness milestone): allocation currently routes
 * through dcadec's own heap allocator; the no-heap arena path is a P3 follow-up.
 * Malformed/truncated/fuzz corpora and IEC 61937 unwrapping are other phases.
 * Emitted PCM carries the DTS Core's native 24-bit sample range (the decoder
 * clips reconstruction to 24 significant bits), sign-extended into int32.
 */

/* Create a decoder. On success *out holds a heap-owned handle; destroy with
 * aius_dts_decoder_destroy. Returns AIUS_OK or AIUS_NO_SPACE / AIUS_INVALID_ARG. */
AiusStatus aius_dts_decoder_create(AiusDtsDecoder **out);

/* Decode one DTS Core access unit. `data`/`size` need not be aligned or padded;
 * the decoder copies into an internally aligned, padded scratch buffer. On
 * AIUS_OK, `block` describes decoder-owned planar PCM valid until the next
 * decode or destroy. `block->planes` are populated for the active channels in
 * AIUS_CH_* order (FL, FR, FC, LFE, SL, SR), tightly packed to `channels`. */
AiusStatus aius_dts_decode_access_unit(AiusDtsDecoder *dec,
                                       const uint8_t *data, size_t size,
                                       AiusPcmBlock *block);

/* Release a decoder and all of its owned memory. NULL is a no-op. */
void aius_dts_decoder_destroy(AiusDtsDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_DTS_H */
