#ifndef AIUS_IEC61937_H
#define AIUS_IEC61937_H

#include <stddef.h>
#include <stdint.h>

#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IEC 61937 data-burst parser (capability-gate phase P2).
 *
 * Consumes the 16-bit S/PDIF (IEC 60958) subframe payload words carrying an
 * IEC 61937 data stream and emits complete, de-framed data bursts. Each burst
 * begins with the four-word preamble Pa=0xF872, Pb=0x4E1F, Pc (burst_info) and
 * Pd (length-code), followed by the burst payload words; the parser normalizes
 * the payload to canonical big-endian bytes regardless of the wire's 16/14-bit
 * packing or byte order, and hands back an AiusBurst descriptor.
 *
 * The steady-state push path performs no allocation: the de-framed payload lands
 * in a fixed instance buffer (AIUS_IEC61937_MAX_PAYLOAD bytes). Only create
 * allocates (one instance), mirroring the P3 AiusDtsDecoder adapter.
 *
 * Wire byte order is auto-detected per burst from the Pa preamble (0xF872 big-
 * endian, 0x72F8 byte-swapped little-endian); the detected order then applies to
 * Pb/Pc/Pd and the payload words. Payload packing (16- or 14-bit) is a create-
 * time configuration, not auto-detected. The classifier that routes PCM vs
 * non-linear bitstreams and picks the packing, and the end-to-end decoder
 * dispatch, are separate concerns; this parser only de-frames bursts and types
 * them.
 *
 * AC-3 data type 1 and DTS Core data types 11/12/13 (0x0B/0x0C/0x0D, burst
 * types I/II/III at 48 kHz) are in scope and complete with AIUS_OK; the payload
 * bytes are the raw codec access unit ready for decoder dispatch. Every other
 * well-formed type completes with AIUS_UNSUPPORTED (still de-framed, never
 * AIUS_MALFORMED). Pd is a bit count for AC-3 and DTS Core. E-AC-3 type 21,
 * though unsupported, is de-framed using its specified byte-count Pd.
 */

/* Fixed de-framed-payload capacity. Covers AC-3 at 48 kHz and the in-scope IEC
 * 61937 DTS Core burst repetition periods (through type III, the 2048-frame
 * period, ~4 KiB of 16-bit payload). A length-code demanding more than this
 * completes with AIUS_NO_SPACE (never a buffer overrun) so the caller can
 * provision more; the 16-bit Pd can express lengths beyond this buffer. */
#define AIUS_IEC61937_MAX_PAYLOAD 4096u

/* How the burst payload words encode payload bytes on the wire. */
typedef enum {
	AIUS_IEC61937_PACK_16 = 0, /* two payload bytes per 16-bit wire word */
	AIUS_IEC61937_PACK_14      /* 14 significant (low) bits per wire word */
} AiusIec61937Packing;

/* Parser configuration. A zeroed struct selects the defaults (16-bit packing). */
typedef struct {
	AiusIec61937Packing packing;
} AiusIec61937Config;

/* Create a parser. On success *out holds a heap-owned handle; destroy with
 * aius_iec61937_parser_destroy. `cfg` may be NULL for defaults. Returns AIUS_OK,
 * AIUS_NO_SPACE, or AIUS_INVALID_ARG. */
AiusStatus aius_iec61937_parser_create(AiusIec61937Parser **out,
                                       const AiusIec61937Config *cfg);

/* Drop any in-progress burst state and return to searching for a preamble. The
 * most-recently completed burst descriptor is invalidated. */
void aius_iec61937_parser_reset(AiusIec61937Parser *p);

/* Release the parser. NULL is a no-op. */
void aius_iec61937_parser_destroy(AiusIec61937Parser *p);

/* Push `count` subframe payload words. `*consumed` receives how many words were
 * taken from `words`; the caller re-presents the remainder on the next call. A
 * single call emits at most one burst, so a coalesced read carrying several
 * bursts is drained by repeated calls. Return values:
 *
 *   AIUS_OK          a supported AC-3 or DTS Core burst completed; its
 *                    descriptor is available via
 *                    aius_iec61937_current_burst. `*consumed` stops just past
 *                    the burst's final payload word.
 *   AIUS_UNSUPPORTED a well-formed but out-of-scope burst completed; the
 *                    descriptor (type + de-framed payload) is still available.
 *   AIUS_NEED_MORE   all input consumed, no burst yet; call again with more.
 *   AIUS_MALFORMED   a corrupt burst (error flag, or a zero/invalid length on an
 *                    in-scope type) was rejected; the parser resumes searching
 *                    for the next preamble from `*consumed`.
 *   AIUS_NO_SPACE    the length-code exceeds AIUS_IEC61937_MAX_PAYLOAD; rejected
 *                    and searching resumes from `*consumed`.
 *   AIUS_INVALID_ARG p, words (with count>0), or consumed is NULL.
 *
 * AIUS_UNSUPPORTED intentionally publishes its freshly de-framed descriptor.
 * Rejected and argument-error statuses leave no stale payload readable as a
 * fresh burst. */
AiusStatus aius_iec61937_push(AiusIec61937Parser *p, const uint16_t *words,
                              size_t count, size_t *consumed);

/* Signal end of input. Returns AIUS_TRUNCATED if a burst was mid-parse (past its
 * preamble), else AIUS_OK. Either way the parser is reset to search for a fresh
 * preamble. */
AiusStatus aius_iec61937_finish(AiusIec61937Parser *p);

/* Fill `out` with the most-recently completed burst (valid after AIUS_OK or
 * AIUS_UNSUPPORTED). `out->payload` points into the parser and stays valid until
 * the next push overwrites it. Returns AIUS_OK, or AIUS_INVALID_ARG if no burst
 * is current or an argument is NULL. */
AiusStatus aius_iec61937_current_burst(const AiusIec61937Parser *p,
                                       AiusBurst *out);

/* Cumulative reject/accept tallies plus the last rejected burst header, for
 * after-the-fact diagnosis of a stream the parser refuses (defect 29: a live
 * carrier whose every status was swallowed). `last_reject_pc`/`_pd` hold the
 * NORMALIZED header words of the most recent MALFORMED or NO_SPACE burst --
 * reading them answers byte order, bit slips, or genuine garbage in one look. */
typedef struct {
	uint32_t bursts_ok;
	uint32_t rejects_no_space;
	uint32_t rejects_malformed;
	uint32_t rejects_unsupported;
	uint32_t resyncs; /* returns to Pa-search from a non-terminal state */
	uint16_t last_reject_pc;
	uint16_t last_reject_pd;
} AiusIecDiag;

/* Copy the diagnostic tallies. Returns AIUS_OK or AIUS_INVALID_ARG. */
AiusStatus aius_iec61937_get_diag(const AiusIec61937Parser *p,
                                  AiusIecDiag *out);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_IEC61937_H */
