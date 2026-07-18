#ifndef AIUS_DTS_H
#define AIUS_DTS_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"
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
 * Allocation: two provisioning modes share one create/decode/destroy contract.
 * aius_dts_decoder_create routes allocation through libc (host convenience).
 * aius_dts_decoder_create_arena routes ALL allocation — the adapter's handle and
 * scratch and dcadec's `ta` tree — through a caller-owned AiusArena and never
 * touches the libc heap; this is the target-shaped, no-heap path. Both require
 * the allocation shim (lib/aius_dts/src/aius_alloc_shim.c) linked with
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free. After a warmup
 * decode (init/first frame allocates), steady-state frame decoding allocates
 * nothing; read the per-decode allocation count via aius_dts_get_metrics.
 *
 * Emitted PCM carries the DTS Core's native 24-bit sample range (the decoder
 * clips reconstruction to 24 significant bits), sign-extended into int32.
 */

/* Create a decoder whose allocation routes through libc. On success *out holds a
 * heap-owned handle; destroy with aius_dts_decoder_destroy. Returns AIUS_OK or
 * AIUS_NO_SPACE / AIUS_INVALID_ARG. */
AiusStatus aius_dts_decoder_create(AiusDtsDecoder **out);

/* Create a decoder whose handle, scratch, and every dcadec sub-allocation come
 * from `arena` — no libc heap once provisioned. `arena` must outlive the decoder
 * and be dedicated to it. Size it from the measured high-water of a real decode
 * (see aius_dts_get_metrics.arena_high_water; a 5.1 core frame peaks well under
 * 256 KiB). Returns AIUS_OK, AIUS_NO_SPACE if the arena cannot fit the decoder,
 * or AIUS_INVALID_ARG. */
AiusStatus aius_dts_decoder_create_arena(AiusDtsDecoder **out, AiusArena *arena);

/* Decode one DTS Core access unit. `data`/`size` need not be aligned or padded;
 * the decoder copies into an internally aligned, padded scratch buffer. On
 * AIUS_OK, `block` describes decoder-owned planar PCM valid until the next
 * decode or destroy. `block->planes` are populated for the active channels in
 * AIUS_CH_* order (FL, FR, FC, LFE, SL, SR), tightly packed to `channels`. */
AiusStatus aius_dts_decode_access_unit(AiusDtsDecoder *dec,
                                       const uint8_t *data, size_t size,
                                       AiusPcmBlock *block);

/* Enable (quiet == 0, the default) or suppress (quiet != 0) the decoder's
 * hard-error log sink. Malformed input is always reported per call via the
 * returned AiusStatus; a caller decoding untrusted or fuzzed streams sets quiet
 * to avoid per-frame log spam (and, on the target, per-frame printf). Applies
 * process-wide to every decoder. */
void aius_dts_set_log_quiet(int quiet);

/* Copy the measurements from the most recent decode into *out. `arena_high_water`
 * and `alloc_count` are populated on both provisioning modes (alloc_count is 0
 * for a libc decoder); `decode_cycles` and `stack_high_water` stay 0 on the host.
 * `last_status` is the status the last decode returned. Returns AIUS_OK, or
 * AIUS_INVALID_ARG if either pointer is NULL. */
AiusStatus aius_dts_get_metrics(const AiusDtsDecoder *dec, AiusMetrics *out);

/* Release a decoder and all of its owned memory. NULL is a no-op. An arena-backed
 * decoder's storage is reclaimed by resetting or discarding the caller's arena;
 * this call runs dcadec's teardown but does not reset the arena. */
void aius_dts_decoder_destroy(AiusDtsDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_DTS_H */
