/* AiusPipeline: S/PDIF classify + IEC 61937 de-frame -> DTS decode / PCM passthrough.
 *
 * Implements aius_pipeline.h by chaining the P2 AiusIec61937Parser into the P3
 * AiusDtsDecoder behind a single streaming push, with a preamble-based classifier
 * routing linear PCM around the decoder. Classification words are staged in a
 * fixed instance buffer until the stream is typed; thereafter each push feeds the
 * parser (DTS) or the interleaver (PCM) directly. No allocation on the push path
 * beyond the decoder's own arena/heap. See lib/aius_pipeline/include.
 */

#include "aius_pipeline.h"

#include "aius_a52.h"

/* The E-AC-3 route is compile-optional (OTA-precedent flag): the vendored
 * FFmpeg fork is ~120 KB of text+rodata the gated target image must not gain
 * before phase 4's FLASH-cold placement, and the optical build names this TU
 * directly. Off (default), a completed type-21 burst is refused at dispatch
 * (decode_rejects, AIUS_UNSUPPORTED) and the class stays dark. */
#ifndef AIUS_PIPELINE_EAC3
#define AIUS_PIPELINE_EAC3 0
#endif
#if AIUS_PIPELINE_EAC3
#include "aius_eac3.h"
#endif

#include <stdlib.h>
#include <string.h>

/* IEC 61937 preamble, big-endian wire (Pa,Pb) and its byte-swapped little-endian
 * form. Classification only needs the pair; the parser re-detects order per burst. */
#define IEC_PA_BE 0xF872u
#define IEC_PB_BE 0x4E1Fu
#define IEC_PA_LE 0x72F8u
#define IEC_PB_LE 0x1F4Eu
#define PCM_DEFAULT_SAMPLE_RATE_HZ 48000u

typedef enum {
	PENDING_NONE = 0,
	PENDING_PCM,
	PENDING_DTS_AU,
	PENDING_DTS_OUTPUT,
	PENDING_A52_AU,
	PENDING_A52_NEXT,
	PENDING_A52_OUTPUT,
	/* E-AC-3 mirrors the DTS single-shot flow (one AU in, one block out,
	 * sliced on output). Only ever set when AIUS_PIPELINE_EAC3 compiled the
	 * route in: dispatch refuses family 21 before staging otherwise. */
	PENDING_EAC3_AU,
	PENDING_EAC3_OUTPUT,
	/* A CRC-rejected access unit's cadence, emitted as zeros. Deliberately
	 * NOT one of the *_OUTPUT states: it owns no decoder, slices no
	 * decoder-owned block, and must not be reachable by any recovery path
	 * that assumes a resident decode is in flight. */
	PENDING_SILENCE
} PendingKind;

struct AiusPipeline {
	AiusIec61937Parser *parser;
	AiusDtsDecoder *dts_decoder;   /* resident iff resident_family == 11 */
	AiusA52Decoder *a52_decoder;   /* resident iff resident_family == 1 */
#if AIUS_PIPELINE_EAC3
	AiusEac3Decoder *eac3_decoder; /* resident iff resident_family == 21 */
#endif
	AiusPcmSink sink;

	AiusPipelineClass klass;
	size_t window;             /* classification window in words (<= MAX) */
	int arena_backed;          /* instance/decoder from an arena: do not free() */
	AiusStatus last_status;    /* last parse/decode result observed */

	/* The codec slot: exactly one decoder exists at a time, created lazily at
	 * its Pc family's first completed burst and swapped on a family change
	 * (see ensure_codec_resident). On the arena path the slot is a fixed
	 * AIUS_PIPELINE_CODEC_SLOT_BYTES sub-arena carved at create, reset (its
	 * high-water retained) at every swap; on the heap path decoders are plain
	 * libc create/destroy and codec_slot stays zeroed -- arming a per-instance
	 * malloc'd slot would poison the alloc shim's single owned address range
	 * (see aius_alloc_shim.h). */
	AiusArena codec_slot;
	uint16_t resident_family;  /* 0 none; else canonical Pc: 1 AC-3, 11 DTS,
	                            * 21 E-AC-3 */
	AiusCycleClock cycle_clock; /* applied to each lazily created decoder */

	/* Classification staging: raw words held until the stream is typed. */
	uint16_t clsbuf[AIUS_PIPELINE_CLASSIFY_MAX_WORDS];
	size_t cls_n;
	size_t cls_off;

	/* PCM pass-through staging (interleaved L,R -> planar). */
	int32_t pcm_l[AIUS_PIPELINE_PCM_BLOCK_FRAMES];
	int32_t pcm_r[AIUS_PIPELINE_PCM_BLOCK_FRAMES];
	uint32_t pcm_frames;       /* complete L/R frames staged */
	int32_t pcm_pending_l;     /* a lone L sample awaiting its R */
	int pcm_have_half;
	uint32_t pcm_sample_rate_hz;

	/* Confirmation squelch: words consumed by the PCM route since the class
	 * was entered, SATURATING at AIUS_PIPELINE_PCM_CONFIRM_WORDS. Per-run and
	 * push-independent, which is what fixes the boundary at run word CONFIRM
	 * under any push sizing; saturating rather than free-running because a
	 * wrapping uint32 would re-enter the unconfirmed window every ~12.4 h of
	 * continuous PCM and squelch a confirmed stream for no reason. */
	uint32_t pcm_run_words;

	/* PCM->DTS re-type detector: continuous across pushes, active only while
	 * the class is PCM. Positions are a free-running uint32 word counter;
	 * spacing math uses unsigned deltas, and candidate expiry (push-boundary,
	 * age > MAX spacing) removes the wrap-aliasing hazard of an hours-stale
	 * candidate. */
	uint16_t rt_prev_word;     /* last word seen by the PCM route */
	uint8_t rt_have_prev;
	uint8_t rt_cand_valid;
	uint8_t rt_cand_form;      /* candidate pair byte order: 1 BE, 2 LE */
	uint32_t rt_pos;           /* stream position of the NEXT word */
	uint32_t rt_cand_pos;      /* candidate pair's Pa position */

	/* Defect 34: the word after a Pa/Pb pair is the burst_info Pc, and a Pc
	 * naming a data type outside {1,11,12,13} is a SINGLE-PAIR exit from
	 * PCM (see single_pair_exit_header). The pair may end a push, so the
	 * look-ahead is carried in the instance like the prev-word carry is. */
	uint16_t rt_sync_pa;
	uint16_t rt_sync_pb;
	uint8_t rt_sync_form;
	uint8_t rt_await_pc;

	/* DTS starvation: words consumed by the DTS route since the last parser
	 * burst outcome (completed or rejected). Zeroed on EVERY entry to the DTS
	 * class and when starvation fires -- stale residue across a class
	 * transition would immediately re-starve a valid re-type. */
	uint32_t dts_starve_words;

	/* Lifetime diagnostics: survive aius_pipeline_reset(), cleared at create. */
	uint32_t retype_count;
	uint32_t reclassify_count;
	uint32_t squelched_frames;

	/* CRC substitution geometry: the SHAPE of the last access unit the named
	 * family decoded successfully on this link. A CRC-rejected unit is
	 * replaced by that many frames of zeros in that many blocks, so the
	 * transmit ring keeps receiving audio at the cadence the decode would
	 * have produced and nothing downstream sees a gap, a rate change or a
	 * channel-count change.
	 *
	 * The shape has to be REMEMBERED rather than read from the rejected
	 * frame's own header: crc1 covering the header is exactly what a
	 * corrupt frame may have failed, so its fscod/acmod/numblkscod are the
	 * one thing that must not be trusted here. `sub_family` 0 means no unit
	 * has decoded yet, and then a reject is simply dropped -- there is no
	 * established cadence to hold, and nothing has reached the ring to
	 * drain. */
	uint16_t sub_family;       /* 0 none; else canonical Pc, 1 or 21 */
	uint16_t sub_channels;
	uint32_t sub_mask;
	uint32_t sub_rate_hz;
	uint32_t sub_au_frames;    /* frames one access unit of that family is */
	uint32_t sub_block_frames; /* frames per substituted block */
	uint32_t sub_frames_left;  /* frames of the rejected AU still to emit */

	PendingKind pending;
	AiusPcmBlock retained_block;
	AiusPcmBlock sink_block;
	uint32_t retained_offset;
	uint8_t retained_retry;
	uint8_t a52_blocks_decoded;
	uint8_t a52_inflight;
	uint16_t pending_pc;
	uint16_t codec_pc;
#ifdef AIUS_PIPELINE_TEST_HOOKS
	uint8_t test_fail_a52_block;
#endif
	AiusPipelineServiceDiag service_diag;
};

/* A re-type inside finish()'s PCM-promotion flush is impossible only while the
 * minimum pair spacing spans the whole classify window; keep the coupling. */
_Static_assert(AIUS_PIPELINE_RETYPE_MIN_SPACING_WORDS >=
               AIUS_PIPELINE_CLASSIFY_MAX_WORDS,
               "retype MIN spacing must cover the classify window");

/* The squelch is only a guarantee while the unconfirmed window is wide enough
 * for the worst re-type phase to complete inside it: at the maximum pair
 * spacing the validating Pb lands at run word CONFIRM at the latest, which the
 * detector consumes before staging. A narrower CONFIRM would let a mis-typed
 * stream emit burst data from a classification window. */
_Static_assert(AIUS_PIPELINE_PCM_CONFIRM_WORDS >=
               AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS,
               "confirm window must cover the widest re-type pair spacing");

/* Boundary word parity is what makes L-first alignment knowable: with an even
 * boundary the first staged word after the squelch is always an L sample, under
 * any push sizing. An odd CONFIRM would swap the channels of every squelched
 * PCM run. */
_Static_assert(AIUS_PIPELINE_PCM_CONFIRM_WORDS % 2u == 0u,
               "confirm boundary must be even so staging resumes L-first");

/* A substituted silence block borrows the PCM pass-through's staging plane
 * rather than carrying its own (see fill_silence_descriptor): the instance
 * bound below is the scarce DTCM this pipeline is allowed, and a dedicated
 * 512-frame zero plane would cost a whole kilobyte of it for a path that runs
 * only on a corrupt frame. Both cadences the substitution emits -- AC-3's
 * 256-frame block and the 512-frame output slice E-AC-3 is cut into -- must
 * therefore fit inside that plane. */
_Static_assert(AIUS_A52_BLOCK_SAMPLES <= AIUS_PIPELINE_PCM_BLOCK_FRAMES,
               "AC-3 block outgrew the PCM staging plane the substitution borrows");
_Static_assert(AIUS_PIPELINE_MAX_OUTPUT_FRAMES <= AIUS_PIPELINE_PCM_BLOCK_FRAMES,
               "output slice outgrew the PCM staging plane the substitution borrows");

/* The published instance bound is what lets arena owners size instance +
 * carve + slot at compile time (optical_main's ARENA_BYTES assert). The
 * 64-bit host instance bounds the 32-bit target's. */
_Static_assert(sizeof(struct AiusPipeline) <= AIUS_PIPELINE_INSTANCE_MAX_BYTES,
               "AiusPipeline outgrew AIUS_PIPELINE_INSTANCE_MAX_BYTES; "
               "re-measure and raise the published bound");

/* ---- classifier ---------------------------------------------------------- */

/* 0 = not a pair; 1 = big-endian wire form; 2 = byte-swapped form. The form
 * matters to the re-type detector: validation means two pairs of the SAME
 * form -- a mixed-form sequence is not a real IEC stream, and requiring the
 * match halves the random-false-pair rate for free. */
static int pair_form(uint16_t a, uint16_t b)
{
	if (a == IEC_PA_BE && b == IEC_PB_BE)
		return 1;
	if (a == IEC_PA_LE && b == IEC_PB_LE)
		return 2;
	return 0;
}

static int is_preamble_pair(uint16_t a, uint16_t b)
{
	return pair_form(a, b) != 0;
}

/* Defect 34: does `pc_word`, read in the pair's own wire byte order, complete a
 * well-formed IEC 61937 burst header for a data type the two-pair rule cannot
 * be trusted to reach?
 *
 * WHY THIS EXISTS. E-AC-3 (type 21) repeats every 12,288 words at its worst
 * (6-block) burst shape, which is wider than
 * AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS (8,192): the first candidate pair
 * always expires before the second arrives, so the two-pair rule can NEVER
 * re-type such a link. ~96% of entry phases therefore typed PCM, cleared the
 * confirmation squelch and played compressed burst data as full-scale noise --
 * the exact harm the squelch exists to prevent. Raising the spacing was the
 * other candidate and was rejected: PCM_CONFIRM_WORDS must stay >= MAX, so it
 * would lengthen the confirmation mute of every genuine PCM source.
 *
 * WHY ONE PAIR IS ENOUGH HERE, when two are needed for types 1/11/12/13. A
 * bare pair is 32 bits of evidence and the two-pair rule buys the rest. A pair
 * FOLLOWED BY a burst_info that clears the error flag and names a type in this
 * set is stronger evidence AND acting on it fails safe. Since E-AC-3 became
 * decodable (phase 3) the set is no longer "types no decoder claims": type 21
 * stays in it because the single header is its ONLY viable PCM exit (period
 * wider than MAX), and acting now yields decode where the route is compiled
 * in, or a dark dispatch refusal where it is not. Either way a false positive
 * on genuine PCM costs the ordinary starvation recovery -- silence, never
 * noise -- whereas failing to act plays the burst. The rule is still NOT
 * extended to 1/11/12/13: their periods fit inside MAX, so the two-pair rule
 * reaches them, and for a type it can reach, acting on one pair would trade a
 * provably-rare false flip for no coverage gain.
 *
 * A header with the ERROR FLAG set stays excluded -- the parser rejects it, so
 * it is not a sync. The documented residual: a link asserting the error flag
 * on every burst still confirms PCM (defect 34's own exclusion, unchanged).
 *
 * Types 1/11/12/13 return 0 and fall through to the unchanged two-pair rule. */
static int single_pair_exit_header(int form, uint16_t pc_word)
{
	const uint16_t pc =
		(form == 2) ? (uint16_t)((pc_word >> 8) | (pc_word << 8)) : pc_word;
	if ((pc & AIUS_IEC61937_PC_ERROR_FLAG) != 0u)
		return 0; /* the parser rejects this header; it is not a sync */
	const uint16_t data_type =
		(uint16_t)(pc & AIUS_IEC61937_PC_DATA_TYPE_MASK);
	return !(data_type == 1u || data_type == 11u || data_type == 12u ||
	         data_type == 13u);
}

AiusPipelineClass aius_pipeline_classify(const uint16_t *words, size_t count,
                                         size_t window)
{
	if (window == 0 || window > AIUS_PIPELINE_CLASSIFY_MAX_WORDS)
		window = AIUS_PIPELINE_CLASSIFY_MAX_WORDS;
	if (!words)
		return count >= window ? AIUS_PIPELINE_PCM : AIUS_PIPELINE_UNKNOWN;

	/* A preamble whose Pa lies within the first `window` words types the stream
	 * as DTS. Both words of the pair must be present to confirm it. */
	for (size_t i = 0; i + 1 < count && i < window; i++) {
		if (is_preamble_pair(words[i], words[i + 1]))
			return AIUS_PIPELINE_DTS;
	}
	if (count >= window)
		return AIUS_PIPELINE_PCM;
	return AIUS_PIPELINE_UNKNOWN;
}

/* ---- PCM pass-through ----------------------------------------------------- */

static AiusStatus dts_ingest(AiusPipeline *p, const uint16_t *w, size_t n,
	                         size_t *consumed);

/* Wire word (16-bit, IEC 60958 subframe payload) -> AiusPcmBlock sample scale.
 *
 * The left-align is the whole point and it is not cosmetic: a bare sign-extend
 * put pass-through PCM 48.16 dB below the DTS decoder's 24-bit output, so
 * switching source material stepped the level by 8 bits. See aius_codec.h
 * §SAMPLE SCALE.
 *
 * Scaled through uint32_t rather than shifting the signed value: left shift of a
 * negative int is undefined behaviour (C17 6.5.7p4), and the host suites build
 * with UBSan, so it is reported rather than merely latent. The two's-complement
 * result is identical, which is what makes the unsigned round-trip correct here
 * rather than just quieter. */
#define PCM_WIRE_BITS 16
#define PCM_WIRE_SHIFT (AIUS_PCM_SAMPLE_BITS - PCM_WIRE_BITS)

_Static_assert(PCM_WIRE_SHIFT >= 0,
               "AiusPcmBlock sample scale is narrower than the 16-bit wire word; "
               "pcm_feed would have to dither and round, not left-align");

static inline int32_t pcm_wire_to_sample(uint16_t word)
{
	const int32_t wire = (int32_t)(int16_t)word; /* sign-extend 16 -> 32 */
	return (int32_t)((uint32_t)wire << PCM_WIRE_SHIFT);
}

/* Leave PCM for the bitstream (dark) class: drop the staged pass-through tail
 * (at a genuine format switch it is burst data misread as PCM -- its Pa is in
 * there too), start the parser clean from `lead` (the sync words the detector
 * consumed and must not lose), and route the remainder of the caller's chunk
 * after it.
 *
 * Two callers, one transition, one counter: the defect-28 two-pair re-type
 * (lead = Pa,Pb) and the defect-34 unsupported-burst-header exit (lead =
 * Pa,Pb,Pc). Both are PCM->bitstream re-types, so both count into
 * retype_count. */
static AiusStatus enter_bitstream(AiusPipeline *p, const uint16_t *lead,
	                              size_t lead_n, const uint16_t *rest,
	                              size_t n, size_t *consumed)
{
	p->pcm_frames = 0;
	p->pcm_have_half = 0;
	p->pcm_pending_l = 0;
	p->rt_have_prev = 0;
	p->rt_cand_valid = 0;
	p->rt_await_pc = 0;
	aius_iec61937_parser_reset(p->parser);
	p->klass = AIUS_PIPELINE_DTS;
	p->codec_pc = 0u;
	p->dts_starve_words = 0;
	p->retype_count++;

	size_t lead_consumed = 0u;
	AiusStatus st = dts_ingest(p, lead, lead_n, &lead_consumed);
	if (st != AIUS_OK)
		return st;
	if (p->pending != PENDING_NONE) {
		*consumed = 0u;
		return AIUS_OK;
	}
	return dts_ingest(p, rest, n, consumed);
}

/* Enter the PCM class: the confirmation window is counted from here, so the run
 * counter restarts and the staging phase starts clean (the boundary word must
 * land L-first). Every entry point funnels through this -- classify-window
 * typing, which also covers starvation re-classification and a post-reset
 * re-classify, and finish()'s promotion of an undecided tail. */
static void pcm_enter(AiusPipeline *p)
{
	p->klass = AIUS_PIPELINE_PCM;
	p->pcm_run_words = 0;
	p->pcm_frames = 0;
	p->pcm_have_half = 0;
	p->pcm_pending_l = 0;
	/* A fresh run has no pair awaiting its Pc; a stale look-ahead would test
	 * an unrelated word against a long-gone sync. */
	p->rt_await_pc = 0;
}

static AiusStatus pcm_ingest(AiusPipeline *p, const uint16_t *w, size_t n,
	                         size_t *consumed)
{
	*consumed = 0u;
	for (size_t i = 0; i < n; i++) {
		const uint16_t word = w[i];

		/* Defect 34, ahead of everything: this word is the burst_info Pc of
		 * the pair the previous iteration saw. A Pc that completes a
		 * well-formed header for a type outside {1,11,12,13} leaves PCM on
		 * this ONE pair -- the widest supported re-type spacing cannot reach
		 * a 12,288-word E-AC-3 period, so waiting for a second pair means
		 * waiting forever. The sync words go to the parser with the rest. */
		if (p->rt_await_pc) {
			p->rt_await_pc = 0u;
			if (single_pair_exit_header((int)p->rt_sync_form, word)) {
				const uint16_t lead[3] = { p->rt_sync_pa, p->rt_sync_pb,
				                           word };
				size_t rest_consumed = 0u;
				AiusStatus st =
					enter_bitstream(p, lead, 3u, w + i + 1, n - i - 1,
					                &rest_consumed);
				*consumed = i + 1u + rest_consumed;
				return st;
			}
		}

		/* Re-type detector, ahead of staging: on a validated second pair the
		 * staged tail (which holds its Pa) drops and Pb never stages. The
		 * prev-word carry is what detects a pair split across pushes. */
		if (p->rt_have_prev) {
			const int form = pair_form(p->rt_prev_word, word);
			if (form != 0) {
				const uint32_t pa_pos = p->rt_pos - 1;
				if (p->rt_cand_valid) {
					const uint32_t delta = pa_pos - p->rt_cand_pos;
					if (delta >= AIUS_PIPELINE_RETYPE_MIN_SPACING_WORDS &&
					    delta <= AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS &&
					    form == (int)p->rt_cand_form) {
						const uint16_t lead[2] = { p->rt_prev_word,
						                           word };
						size_t rest_consumed = 0u;
						AiusStatus st =
							enter_bitstream(p, lead, 2u, w + i + 1,
							                n - i - 1, &rest_consumed);
						*consumed = i + 1u + rest_consumed;
						return st;
					}
					if (delta > AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS ||
					    form != (int)p->rt_cand_form) {
						p->rt_cand_pos = pa_pos;
						p->rt_cand_form = (uint8_t)form;
					}
					/* Sub-MIN same-form pairs keep the OLDER candidate:
					 * pause/null-burst idles repeat pairs at sub-MIN
					 * spacing, and only the accumulated distance from the
					 * older pair lets such a stream ever re-type. */
				} else {
					p->rt_cand_valid = 1;
					p->rt_cand_pos = pa_pos;
					p->rt_cand_form = (uint8_t)form;
				}
				/* Arm the Pc look-ahead whatever the two-pair rule made of
				 * this pair: the candidate machinery and the defect-34
				 * header test are independent readings of the same sync. */
				p->rt_sync_pa = p->rt_prev_word;
				p->rt_sync_pb = word;
				p->rt_sync_form = (uint8_t)form;
				p->rt_await_pc = 1u;
			}
		}
		p->rt_prev_word = word;
		p->rt_have_prev = 1;
		p->rt_pos++;

		/* Confirmation squelch: until the run has held PCM for CONFIRM words,
		 * the word is DROPPED -- not staged, not delayed, and pcm_have_half
		 * stays clear across the whole window so staging resumes L-first at
		 * the boundary word. The detector above ran first and unchanged: it
		 * must still re-type a mis-typed stream from inside the window, which
		 * is the entire point of squelching. */
		if (p->pcm_run_words < AIUS_PIPELINE_PCM_CONFIRM_WORDS) {
			/* Frames, not words: the odd run word completes the dropped pair,
			 * so a trailing half-word is never counted. */
			if ((p->pcm_run_words & 1u) != 0u)
				p->squelched_frames++;
			p->pcm_run_words++;
			continue;
		}

		int32_t sample = pcm_wire_to_sample(word);
		if (!p->pcm_have_half) {
			p->pcm_pending_l = sample;
			p->pcm_have_half = 1;
			continue;
		}
		p->pcm_l[p->pcm_frames] = p->pcm_pending_l;
		p->pcm_r[p->pcm_frames] = sample;
		p->pcm_have_half = 0;
		p->pcm_frames++;
		if (p->pcm_frames == AIUS_PIPELINE_PCM_BLOCK_FRAMES) {
			p->pending = PENDING_PCM;
			*consumed = i + 1u;
			return AIUS_OK;
		}
	}
	*consumed = n;
	return AIUS_OK;
}

/* ---- AC-3 / E-AC-3 syncframe CRC ------------------------------------------ */

/* The 512-byte CRC-16/ANSI table, pinned to FLASH and never DTCM.
 *
 * The Teensy framework's link script copies EVERY .rodata into DTCM .data
 * (link/aius_teensy41.ld's `.data` line), and DTCM is the scarce region here --
 * the ldscript asserts a 24,352 B floor under the stack/locals zone, and the
 * decoder arena is funded out of exactly this pressure. `.progmem` is claimed
 * to FLASH earlier in the same script, so naming that section keeps the table
 * in XIP where it belongs: it is read once per access unit, sequentially,
 * inside the cache, against a 32 ms budget. The attribute is target-only --
 * a host build has no such section and does not need one.
 *
 * Generated at compile time would need a constexpr the C11 build does not
 * have; 256 entries of a fixed standard polynomial are written out instead,
 * and test/pipeline/test_ac3_crc.c re-derives every one of them bitwise and
 * compares, so a typo here is a failing suite rather than a silent
 * accept-everything CRC. */
#if defined(__arm__)
#define AIUS_AC3_CRC_FLASHRO __attribute__((section(".progmem")))
#else
#define AIUS_AC3_CRC_FLASHRO
#endif

static const uint16_t k_ac3_crc16_table[256] AIUS_AC3_CRC_FLASHRO = {
	0x0000u, 0x8005u, 0x800Fu, 0x000Au, 0x801Bu, 0x001Eu, 0x0014u, 0x8011u,
	0x8033u, 0x0036u, 0x003Cu, 0x8039u, 0x0028u, 0x802Du, 0x8027u, 0x0022u,
	0x8063u, 0x0066u, 0x006Cu, 0x8069u, 0x0078u, 0x807Du, 0x8077u, 0x0072u,
	0x0050u, 0x8055u, 0x805Fu, 0x005Au, 0x804Bu, 0x004Eu, 0x0044u, 0x8041u,
	0x80C3u, 0x00C6u, 0x00CCu, 0x80C9u, 0x00D8u, 0x80DDu, 0x80D7u, 0x00D2u,
	0x00F0u, 0x80F5u, 0x80FFu, 0x00FAu, 0x80EBu, 0x00EEu, 0x00E4u, 0x80E1u,
	0x00A0u, 0x80A5u, 0x80AFu, 0x00AAu, 0x80BBu, 0x00BEu, 0x00B4u, 0x80B1u,
	0x8093u, 0x0096u, 0x009Cu, 0x8099u, 0x0088u, 0x808Du, 0x8087u, 0x0082u,
	0x8183u, 0x0186u, 0x018Cu, 0x8189u, 0x0198u, 0x819Du, 0x8197u, 0x0192u,
	0x01B0u, 0x81B5u, 0x81BFu, 0x01BAu, 0x81ABu, 0x01AEu, 0x01A4u, 0x81A1u,
	0x01E0u, 0x81E5u, 0x81EFu, 0x01EAu, 0x81FBu, 0x01FEu, 0x01F4u, 0x81F1u,
	0x81D3u, 0x01D6u, 0x01DCu, 0x81D9u, 0x01C8u, 0x81CDu, 0x81C7u, 0x01C2u,
	0x0140u, 0x8145u, 0x814Fu, 0x014Au, 0x815Bu, 0x015Eu, 0x0154u, 0x8151u,
	0x8173u, 0x0176u, 0x017Cu, 0x8179u, 0x0168u, 0x816Du, 0x8167u, 0x0162u,
	0x8123u, 0x0126u, 0x012Cu, 0x8129u, 0x0138u, 0x813Du, 0x8137u, 0x0132u,
	0x0110u, 0x8115u, 0x811Fu, 0x011Au, 0x810Bu, 0x010Eu, 0x0104u, 0x8101u,
	0x8303u, 0x0306u, 0x030Cu, 0x8309u, 0x0318u, 0x831Du, 0x8317u, 0x0312u,
	0x0330u, 0x8335u, 0x833Fu, 0x033Au, 0x832Bu, 0x032Eu, 0x0324u, 0x8321u,
	0x0360u, 0x8365u, 0x836Fu, 0x036Au, 0x837Bu, 0x037Eu, 0x0374u, 0x8371u,
	0x8353u, 0x0356u, 0x035Cu, 0x8359u, 0x0348u, 0x834Du, 0x8347u, 0x0342u,
	0x03C0u, 0x83C5u, 0x83CFu, 0x03CAu, 0x83DBu, 0x03DEu, 0x03D4u, 0x83D1u,
	0x83F3u, 0x03F6u, 0x03FCu, 0x83F9u, 0x03E8u, 0x83EDu, 0x83E7u, 0x03E2u,
	0x83A3u, 0x03A6u, 0x03ACu, 0x83A9u, 0x03B8u, 0x83BDu, 0x83B7u, 0x03B2u,
	0x0390u, 0x8395u, 0x839Fu, 0x039Au, 0x838Bu, 0x038Eu, 0x0384u, 0x8381u,
	0x0280u, 0x8285u, 0x828Fu, 0x028Au, 0x829Bu, 0x029Eu, 0x0294u, 0x8291u,
	0x82B3u, 0x02B6u, 0x02BCu, 0x82B9u, 0x02A8u, 0x82ADu, 0x82A7u, 0x02A2u,
	0x82E3u, 0x02E6u, 0x02ECu, 0x82E9u, 0x02F8u, 0x82FDu, 0x82F7u, 0x02F2u,
	0x02D0u, 0x82D5u, 0x82DFu, 0x02DAu, 0x82CBu, 0x02CEu, 0x02C4u, 0x82C1u,
	0x8243u, 0x0246u, 0x024Cu, 0x8249u, 0x0258u, 0x825Du, 0x8257u, 0x0252u,
	0x0270u, 0x8275u, 0x827Fu, 0x027Au, 0x826Bu, 0x026Eu, 0x0264u, 0x8261u,
	0x0220u, 0x8225u, 0x822Fu, 0x022Au, 0x823Bu, 0x023Eu, 0x0234u, 0x8231u,
	0x8213u, 0x0216u, 0x021Cu, 0x8219u, 0x0208u, 0x820Du, 0x8207u, 0x0202u
};

/* The 5/8 boundary in bytes, exactly as an AC-3 encoder computes it: quarter
 * plus sixteenth of the frame's byte count, doubled. That is floor-to-even and
 * NOT the exact fraction -- see the header's 44.1 kHz worked example. */
static size_t ac3_crc1_end(size_t size)
{
	return ((size >> 2) + (size >> 4)) << 1;
}

AiusAc3CrcVerdict aius_ac3_crc_check(const uint8_t *frame, size_t size)
{
	/* frame[5]'s top five bits are bsid, which selects the arm. A buffer
	 * too short to hold it is not a syncframe of either family. */
	if (!frame || size < 7u)
		return AIUS_AC3_CRC_TOO_SHORT;

	const unsigned bsid = (unsigned)(frame[5] >> 3);
	/* bsid <= 10 is AC-3 (legacy + the annex-D reduced-rate variants) and
	 * carries crc1 at bytes 2-3. bsid 11..16 is E-AC-3, whose bytes 2-3 are
	 * strmtyp/frmsiz and which has no interior CRC at all. */
	const size_t crc1_end = bsid <= 10u ? ac3_crc1_end(size) : 0u;

	uint16_t crc = 0u;
	size_t index = 2u; /* the syncword itself is outside both spans */
	if (crc1_end > index && crc1_end <= size) {
		for (; index < crc1_end; index++)
			crc = (uint16_t)((crc << 8) ^
			                 k_ac3_crc16_table[(crc >> 8) ^ frame[index]]);
		if (crc != 0u)
			return AIUS_AC3_CRC_BAD_CRC1;
	}
	/* Same register, carried on: crc1's span is a prefix of crc2's, so the
	 * accepted prefix costs nothing twice. */
	for (; index < size; index++)
		crc = (uint16_t)((crc << 8) ^
		                 k_ac3_crc16_table[(crc >> 8) ^ frame[index]]);
	return crc == 0u ? AIUS_AC3_CRC_OK : AIUS_AC3_CRC_BAD_CRC2;
}

/* ---- codec slot ----------------------------------------------------------- */

/* Canonical Pc of the decoder family that handles `pc`: the three DTS
 * subtypes share one decoder (and one residency). */
static uint16_t pc_family(uint16_t pc)
{
	return (pc == 11u || pc == 12u || pc == 13u) ? 11u : pc;
}

/* Can `family` ever become resident in THIS pipeline? AIUS_OK, or the refusal
 * to record: AIUS_UNSUPPORTED when the decoder is not compiled in,
 * AIUS_NO_SPACE when the arena slot cannot hold it. Checked BEFORE the
 * resident is touched, so an impossible residency can never evict a healthy
 * decoder or dirty the slot's cumulative high-water -- the metric that sizes
 * the slot. Since phase 4 the slot constant IS the E-AC-3 need, so the
 * NO_SPACE arm folds to constant-false; it stays because the comparison is
 * the contract (a future slot shrink refuses here instead of evicting into
 * a doomed create). */
static AiusStatus residency_possible(const AiusPipeline *p, uint16_t family)
{
	if (family == 1u || family == 11u)
		return AIUS_OK;
#if AIUS_PIPELINE_EAC3
	if (family == 21u)
		return (!p->arena_backed ||
		        AIUS_PIPELINE_CODEC_SLOT_BYTES >=
		            AIUS_PIPELINE_EAC3_SLOT_BYTES)
			       ? AIUS_OK
			       : AIUS_NO_SPACE;
#else
	(void)p;
#endif
	return AIUS_UNSUPPORTED;
}

/* Destroy whatever decoder holds the slot and reclaim its storage. The
 * decoder destroys are NULL-safe; the a52 abandon is defensive -- ingest
 * never swaps while an AU is in flight (it does not run with a work item
 * pending), but a defensive path that skipped the abandon would leak
 * liba52's discontinuity state into the next residency. */
static void destroy_resident(AiusPipeline *p)
{
	if (p->a52_inflight && p->a52_decoder)
		(void)aius_a52_abandon(p->a52_decoder);
	p->a52_inflight = 0u;
	if (p->dts_decoder) {
		aius_dts_decoder_destroy(p->dts_decoder);
		p->dts_decoder = NULL;
	}
	if (p->a52_decoder) {
		aius_a52_decoder_destroy(p->a52_decoder);
		p->a52_decoder = NULL;
	}
#if AIUS_PIPELINE_EAC3
	if (p->eac3_decoder) {
		aius_eac3_decoder_destroy(p->eac3_decoder);
		p->eac3_decoder = NULL;
	}
#endif
	p->resident_family = 0u;
	if (p->arena_backed)
		aius_arena_reset(&p->codec_slot);
}

/* Create `family`'s decoder into the (already reclaimed) slot and apply the
 * pipeline's cycle clock. On the arena path a failure here means the slot
 * capacity is wrong -- a sizing bug the suite's capacity oracle exists to
 * catch, not a runtime condition to recover; the caller refuses the burst. */
static AiusStatus create_resident(AiusPipeline *p, uint16_t family)
{
	AiusStatus st;
	if (family == 1u) {
		st = p->arena_backed
			? aius_a52_decoder_create_arena(&p->a52_decoder,
			                                &p->codec_slot)
			: aius_a52_decoder_create(&p->a52_decoder);
		if (st == AIUS_OK)
			st = aius_a52_set_cycle_clock(p->a52_decoder,
			                              &p->cycle_clock);
	} else if (family == 11u) {
		st = p->arena_backed
			? aius_dts_decoder_create_arena(&p->dts_decoder,
			                                &p->codec_slot)
			: aius_dts_decoder_create(&p->dts_decoder);
		if (st == AIUS_OK)
			st = aius_dts_set_cycle_clock(p->dts_decoder,
			                              &p->cycle_clock);
#if AIUS_PIPELINE_EAC3
	} else if (family == 21u) {
		st = p->arena_backed
			? aius_eac3_decoder_create_arena(&p->eac3_decoder,
			                                 &p->codec_slot)
			: aius_eac3_decoder_create(&p->eac3_decoder);
		if (st == AIUS_OK)
			st = aius_eac3_set_cycle_clock(p->eac3_decoder,
			                               &p->cycle_clock);
#endif
	} else {
		return AIUS_UNSUPPORTED;
	}
	if (st != AIUS_OK) {
		destroy_resident(p);
		return st;
	}
	p->resident_family = family;
	return AIUS_OK;
}

/* Make `family`'s decoder the slot's resident, swapping out the other
 * family's if it holds the slot. Same-family calls are free. An impossible
 * residency (not compiled in / cannot fit the slot) refuses BEFORE the
 * current resident is touched. */
static AiusStatus ensure_codec_resident(AiusPipeline *p, uint16_t family)
{
	if (p->resident_family == family)
		return AIUS_OK;
	AiusStatus st = residency_possible(p, family);
	if (st != AIUS_OK)
		return st;
	const int swapping = p->resident_family != 0u;
	destroy_resident(p);
	st = create_resident(p, family);
	if (st == AIUS_OK && swapping)
		p->service_diag.codec_swaps++;
	return st;
}

/* ---- DTS de-frame -> decode ---------------------------------------------- */

/* Remember the shape a family's decode produces, so a later CRC reject of that
 * family can be substituted at the same cadence. Called on every SUCCESSFUL
 * decode, which is what keeps the shape current across a rate or channel-count
 * change in the source. `au_frames` is what one whole access unit of the family
 * is worth (AC-3's fixed 1,536; E-AC-3's decoded block), and the per-block
 * cadence is derived from the block the sink actually sees -- AC-3 emits one
 * 256-frame block per call, E-AC-3's single block is cut into output slices, so
 * the slice cap is the honest per-block figure for it. */
static void note_substitution_shape(AiusPipeline *p, uint16_t family,
	                                const AiusPcmBlock *block,
	                                uint32_t au_frames)
{
	p->sub_family = family;
	/* Clamped so the descriptor can never claim more channels than the
	 * substitution actually populates planes for. A decoded block is always
	 * inside the bound; the clamp is what keeps that a fact rather than an
	 * assumption held two functions away. */
	p->sub_channels = block->channels < AIUS_MAX_CHANNELS
	                          ? block->channels
	                          : (uint16_t)AIUS_MAX_CHANNELS;
	p->sub_mask = block->channel_mask;
	p->sub_rate_hz = block->sample_rate_hz;
	p->sub_au_frames = au_frames;
	p->sub_block_frames = block->frames < AIUS_PIPELINE_MAX_OUTPUT_FRAMES
	                              ? block->frames
	                              : AIUS_PIPELINE_MAX_OUTPUT_FRAMES;
}

/* Describe the next substituted block directly into sink_block.
 *
 * EVERY channel points at the SAME zeroed plane. The sink contract is
 * read-only (aius_codec.h) and the target's consumer processes out of place
 * into its own staging, so one plane of zeros is a complete and correct
 * multi-channel silent block -- and it is what makes the substitution cost no
 * DTCM beyond the plane the PCM route already owns. That plane is provably
 * idle here: it is written only by the PCM route, which cannot run while the
 * class is DTS, and any transition back to PCM goes through a reset that
 * clears `pending` and so ends the substitution first.
 *
 * Re-filled per block rather than zeroed once per unit, so a transmit-blocked
 * retry re-describes an identical block with no state to keep in step. */
static void fill_silence_descriptor(AiusPipeline *p)
{
	uint32_t frames = p->sub_block_frames;
	if (frames > p->sub_frames_left)
		frames = p->sub_frames_left;
	memset(p->pcm_l, 0, (size_t)frames * sizeof(p->pcm_l[0]));
	memset(&p->sink_block, 0, sizeof(p->sink_block));
	for (uint16_t ch = 0u;
	     ch < p->sub_channels && ch < AIUS_MAX_CHANNELS; ch++)
		p->sink_block.planes[ch] = p->pcm_l;
	p->sink_block.channels = p->sub_channels;
	p->sink_block.frames = frames;
	p->sink_block.sample_rate_hz = p->sub_rate_hz;
	p->sink_block.channel_mask = p->sub_mask;
	/* One mark per unit: the remaining frames sit on a unit boundary exactly
	 * when this block's first frame starts a unit of the recorded shape.
	 * Derived from state a retry does not advance. */
	p->sink_block.first_in_unit =
		(uint8_t)((p->sub_frames_left % p->sub_au_frames) == 0u);
}

/* A completed burst failed its own CRC. Refuse the decode and, when this
 * family has an established shape, substitute the unit's cadence with silence.
 *
 * NOT the decode-reject path and emphatically not the abandon path: those
 * either drop the unit outright (the ring drains by one access unit for a
 * fault the source, not the board, committed) or reset the whole stream state
 * (a class reset, a re-classification and an audio generation change -- three
 * epochs of consequence for one 32 ms unit that arrived intact and merely
 * wrong). The class, the parser lock, the resident decoder and the audio
 * generation all survive here by construction: nothing below touches them.
 *
 * HARD ZERO, no fade, deliberately. A fade-OUT would need the tail of the last
 * decoded block, which is already committed to the transmit ring and whose
 * plane the decoder has since reused -- retaining it means a per-block copy on
 * the healthy path to serve the corrupt one. The fade-IN at the other end of
 * the unit is not implementable at this seam at all: the next unit's first
 * sample arrives already decoded and cannot be ramped without delaying the
 * whole stream by a block. So a ramp removes one of the two steps for real
 * hot-path cost, and the downstream DSP does not soften either of them (the
 * true-peak limiter reduces gain on peaks; it has no floor behaviour, so a
 * step to zero passes through it unchanged). One bounded click that is the
 * audible signature of a counted, timestamped event is the accepted trade
 * against 32 ms of full-scale garbage that nothing recorded at all. */
static void reject_burst_crc(AiusPipeline *p, uint16_t family, size_t length)
{
	p->service_diag.crc_rejects++;
	p->service_diag.crc_reject_last_bytes = (uint32_t)length;
	p->service_diag.last_failure = AIUS_MALFORMED;
	p->last_status = AIUS_MALFORMED;
	p->pending_pc = 0u;
	if (p->sub_family != family || p->sub_channels == 0u ||
	    p->sub_block_frames == 0u || p->sub_au_frames == 0u) {
		/* No decode of this family has succeeded yet, so there is no
		 * cadence to hold and nothing in the ring to protect. */
		p->pending = PENDING_NONE;
		return;
	}
	p->sub_frames_left = p->sub_au_frames;
	p->pending = PENDING_SILENCE;
}

static AiusStatus dts_ingest(AiusPipeline *p, const uint16_t *w, size_t n,
	                         size_t *consumed_out)
{
	size_t pos = 0;
	while (pos < n) {
		size_t consumed = 0;
		AiusStatus st = aius_iec61937_push(p->parser, w + pos, n - pos, &consumed);
		pos += consumed;
		p->dts_starve_words += (uint32_t)consumed;

		if (st == AIUS_NEED_MORE)
			break;
		if (st == AIUS_INVALID_ARG) {
			*consumed_out = pos;
			return AIUS_INVALID_ARG;
		}

		p->dts_starve_words = 0;

		if (st == AIUS_OK) {
			AiusBurst b;
			if (aius_iec61937_current_burst(p->parser, &b) == AIUS_OK) {
				uint16_t family;
				if (b.data_type == 1u || b.data_type == 11u ||
				    b.data_type == 12u || b.data_type == 13u ||
				    b.data_type == 21u)
					family = pc_family(b.data_type);
				else
					continue; /* parser owns the unsupported-code policy */
				AiusStatus cs = ensure_codec_resident(p, family);
				if (cs != AIUS_OK) {
					/* Residency-impossible / slot-capacity / create
					 * failure: refuse this burst and re-lock, the
					 * decode-reject shape. Type 21 lands here
					 * whenever the E-AC-3 route is not compiled in
					 * (AIUS_UNSUPPORTED) or the arena slot cannot
					 * hold the decoder (AIUS_NO_SPACE, until phase 4
					 * grows it) -- the link stays dark. */
					p->last_status = cs;
					p->service_diag.decode_rejects++;
					p->service_diag.last_failure = cs;
					continue;
				}
				/* CRC BEFORE THE UNIT IS EVER STAGED FOR
				 * DECODE, for the two families that carry one.
				 *
				 * A burst can arrive complete and framed and
				 * still be wrong inside: an S/PDIF DPLL
				 * micro-flap shorter than three epochs keeps the
				 * classification, so the parser hands over a
				 * full-length access unit with bit errors in it.
				 * Neither liba52 nor the vendored E-AC-3 fork
				 * validates the frame's own CRC, so that unit
				 * used to decode and PLAY: 32 ms of full-scale
				 * crackle that no counter recorded and no
				 * timestamp placed. One pass over at most 3,840
				 * bytes here is what turns it into a counted,
				 * stamped, silent event. DTS carries no
				 * equivalent whole-frame CRC and is not checked.
				 *
				 * Refused here rather than in service_one so a
				 * corrupt unit never becomes a pending DECODE at
				 * all -- there is no decoder call to unwind, no
				 * a52_inflight to abandon, and therefore no path
				 * by which this reaches the class-resetting
				 * recovery the register calls too heavy. */
				if ((family == 1u || family == 21u) &&
				    aius_ac3_crc_check(b.payload, b.length) !=
				        AIUS_AC3_CRC_OK) {
					reject_burst_crc(p, family, b.length);
					if (p->pending == PENDING_NONE)
						continue;
					*consumed_out = pos;
					return AIUS_OK;
				}
				p->pending = (family == 1u)    ? PENDING_A52_AU
				             : (family == 21u) ? PENDING_EAC3_AU
				                               : PENDING_DTS_AU;
				p->pending_pc = b.data_type;
				*consumed_out = pos;
				return AIUS_OK;
			}
		} else {
			/* UNSUPPORTED / MALFORMED / NO_SPACE: skip; the parser re-locks. */
			p->last_status = st;
			p->service_diag.parse_rejects++;
			p->service_diag.last_failure = st;
		}

		if (consumed == 0)
			break; /* defensive: never spin on a non-advancing parser */
	}
	*consumed_out = pos;
	return AIUS_OK;
}

/* ---- routing ------------------------------------------------------------- */

static AiusStatus route_ingest(AiusPipeline *p, const uint16_t *w, size_t n,
	                           size_t *consumed)
{
	if (p->klass == AIUS_PIPELINE_DTS)
		return dts_ingest(p, w, n, consumed);
	if (p->klass == AIUS_PIPELINE_PCM)
		return pcm_ingest(p, w, n, consumed);
	*consumed = 0u;
	return AIUS_OK;
}

static void cleanup_pending(AiusPipeline *p)
{
	if (p->a52_inflight && p->a52_decoder)
		(void)aius_a52_abandon(p->a52_decoder);
	p->a52_inflight = 0u;
	p->a52_blocks_decoded = 0u;
	/* An interrupted substitution does not resume: whatever cut it short
	 * (discontinuity, sink failure, operator reset) has already moved the
	 * stream past the unit the zeros were standing in for. */
	p->sub_frames_left = 0u;
	p->pending_pc = 0u;
	p->pending = PENDING_NONE;
	p->retained_retry = 0u;
	p->retained_offset = 0u;
	memset(&p->retained_block, 0, sizeof(p->retained_block));
	memset(&p->sink_block, 0, sizeof(p->sink_block));
	p->pcm_frames = 0u;
	p->pcm_have_half = 0;
	p->pcm_pending_l = 0;
}

/* Defect 35: a source discontinuity kills staged partial compressed input
 * BEFORE anything decodes it.
 *
 * An encoded source terminated mid-burst leaves the parser inside a payload it
 * will never be given the rest of. Left alone, the next words -- which belong to
 * whatever the source became, PCM included -- complete that burst to its
 * DECLARED length, and a dead access unit is then handed to a decoder: one
 * rejected DTS decode, or (liba52 validates no payload CRC) a whole six-block
 * AC-3 grind whose every block is full-scale garbage on the wire.
 *
 * So every discontinuity funnels through here: the partial burst is dropped
 * ONCE, and the drop is COUNTED. `au_abandons` therefore now also covers staged
 * compressed input that never reached a decoder -- which is the honest reading,
 * and the reason the live event could not be attributed: it read zero abandons
 * from end to end while a whole access unit was thrown away. `last_failure`
 * carries the observation across the reset that follows, as AIUS_TRUNCATED:
 * MALFORMED asserts the source emitted corrupt data, and a cut burst is not
 * corrupt, it is truncated -- the word aius_pipeline_finish already uses for a
 * burst the input ended inside. So a genuinely corrupt burst (the parser's own
 * MALFORMED) and a burst a carrier return or replay stop cut short stay
 * distinguishable on the wire, and the cut no longer reads as a board fault.
 * `last_status` is left to aius_pipeline_reset's documented clear, so the
 * operator-owned latch keeps its existing semantics exactly.
 *
 * A completed-but-unserviced AU counts too (it is staged compressed work with no
 * decode attempt). An AU already emitting blocks does NOT: aius_pipeline_push's
 * output-drop path and the service failure paths already count that abandon,
 * and counting it here would double it. */
static void drop_staged_compressed_input(AiusPipeline *p)
{
	const int staged = aius_iec61937_partial_burst(p->parser) ||
	                   p->pending == PENDING_DTS_AU ||
	                   p->pending == PENDING_A52_AU ||
	                   p->pending == PENDING_EAC3_AU;
	aius_iec61937_parser_reset(p->parser);
	cleanup_pending(p);
	if (staged) {
		p->service_diag.au_abandons++;
		p->service_diag.last_failure = AIUS_TRUNCATED;
	}
}

/* The stream-state half of a reset, shared by the counted (public) and the
 * already-counted (internal) entry points. */
static void pipeline_reset_state(AiusPipeline *p)
{
	p->klass = AIUS_PIPELINE_UNKNOWN;
	p->codec_pc = 0u;
	p->cls_n = 0;
	p->cls_off = 0;
	p->pcm_run_words = 0; /* the next PCM run re-earns its confirmation */
	p->last_status = AIUS_OK;
	/* Detector/starvation STATE clears; the lifetime counters do not --
	 * they are the live proof that a re-type (not a reboot) happened. */
	p->rt_have_prev = 0;
	p->rt_cand_valid = 0;
	p->rt_await_pc = 0;
	p->rt_pos = 0;
	p->dts_starve_words = 0;
	/* The substitution shape is an observation of THIS source's units. A
	 * discontinuity may have put a different programme (or a different rate
	 * or channel count) on the wire, so it is re-learned from the next
	 * successful decode rather than carried across. */
	p->sub_family = 0u;
	p->sub_channels = 0u;
	p->sub_mask = 0u;
	p->sub_rate_hz = 0u;
	p->sub_au_frames = 0u;
	p->sub_block_frames = 0u;
}

/* Discontinuity for the service/push failure paths, which have ALREADY counted
 * the abandon they are recovering from. Counting again here would double it --
 * that is the whole reason this is separate from aius_pipeline_reset. */
static void pipeline_reset_serviced(AiusPipeline *p)
{
	aius_iec61937_parser_reset(p->parser);
	cleanup_pending(p);
	pipeline_reset_state(p);
}

static void begin_ingest_call(AiusPipeline *p)
{
	if (p->klass == AIUS_PIPELINE_DTS &&
	    p->dts_starve_words > AIUS_PIPELINE_DTS_STARVE_WORDS) {
		/* Starvation IS the classification discontinuity: the source
		 * stopped supplying bursts, so anything half-parsed is dead. */
		drop_staged_compressed_input(p);
		p->klass = AIUS_PIPELINE_UNKNOWN;
		p->codec_pc = 0u;
		p->cls_n = 0;
		p->cls_off = 0;
		p->dts_starve_words = 0;
		p->reclassify_count++;
	}
	if (p->klass == AIUS_PIPELINE_PCM && p->rt_cand_valid &&
	    p->rt_pos - p->rt_cand_pos >
	        AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS + 1u)
		p->rt_cand_valid = 0;
}

AiusStatus aius_pipeline_ingest(AiusPipeline *p, const uint16_t *words,
	                            size_t count, size_t *consumed)
{
	if (!p || !consumed || (count > 0u && !words))
		return AIUS_INVALID_ARG;
	*consumed = 0u;
	if (p->pending != PENDING_NONE)
		return AIUS_OK;

	/* Push-boundary re-judgements (defect 28). Starvation: a DTS class that
	 * consumed the starve budget with no parser burst outcome returns to
	 * UNKNOWN, and this push's words re-classify through the normal staging
	 * path below. Candidate expiry: a re-type candidate older than the MAX
	 * spacing can never validate and is dropped (which also removes the
	 * uint32 position-wrap aliasing of an hours-stale candidate). */
	begin_ingest_call(p);
	/* ^ +1: with the candidate's Pa as the LAST word of a push, its age at
	 * the next boundary is delta+1 -- a bare > MAX would expire a candidate
	 * whose validating Pb (delta exactly MAX) is still legal. */

	/* Classification words were accepted before their class was known. Finish
	 * routing that retained prefix before accepting later input. */
	if (p->cls_off < p->cls_n && p->klass != AIUS_PIPELINE_UNKNOWN) {
		size_t routed = 0u;
		AiusStatus st =
			route_ingest(p, p->clsbuf + p->cls_off, p->cls_n - p->cls_off,
			             &routed);
		p->cls_off += routed;
		if (p->cls_off == p->cls_n)
			p->cls_off = p->cls_n = 0u;
		if (st != AIUS_OK || p->pending != PENDING_NONE ||
		    p->cls_off < p->cls_n)
			return st;
	}

	/* Stage words until the stream is typed, then flush the staged run. */
	while (*consumed < count && p->klass == AIUS_PIPELINE_UNKNOWN) {
		size_t room = p->window - p->cls_n;
		size_t take = count - *consumed;
		if (take > room)
			take = room;
		memcpy(p->clsbuf + p->cls_n, words + *consumed,
		       take * sizeof(uint16_t));
		p->cls_n += take;
		*consumed += take;

		p->klass = aius_pipeline_classify(p->clsbuf, p->cls_n, p->window);
		if (p->klass == AIUS_PIPELINE_DTS)
			p->dts_starve_words = 0; /* every DTS entry starts unstarved */
		else if (p->klass == AIUS_PIPELINE_PCM)
			pcm_enter(p); /* every PCM entry starts unconfirmed */
		if (p->klass != AIUS_PIPELINE_UNKNOWN) {
			size_t routed = 0u;
			AiusStatus st =
				route_ingest(p, p->clsbuf, p->cls_n, &routed);
			p->cls_off = routed;
			if (p->cls_off == p->cls_n)
				p->cls_off = p->cls_n = 0u;
			if (st != AIUS_OK || p->pending != PENDING_NONE ||
			    p->cls_off < p->cls_n)
				return st;
		}
		/* Still UNKNOWN only when cls_n < window; the loop takes more input. */
	}

	if (*consumed < count) {
		size_t routed = 0u;
		AiusStatus st =
			route_ingest(p, words + *consumed, count - *consumed, &routed);
		*consumed += routed;
		return st;
	}
	return AIUS_OK;
}

static void fill_pcm_descriptor(AiusPipeline *p)
{
	memset(&p->sink_block, 0, sizeof(p->sink_block));
	p->sink_block.planes[0] = p->pcm_l;
	p->sink_block.planes[1] = p->pcm_r;
	p->sink_block.channels = 2u;
	p->sink_block.frames = p->pcm_frames;
	p->sink_block.sample_rate_hz = p->pcm_sample_rate_hz;
	p->sink_block.channel_mask = AIUS_CH_FL | AIUS_CH_FR;
	p->sink_block.first_in_unit = 1u; /* every PCM block */
}

AiusStatus aius_pipeline_slice_pcm_block(const AiusPcmBlock *source,
	                                     uint32_t frame_offset,
	                                     AiusPcmBlock *slice)
{
	if (!source || !slice || source->channels == 0u ||
	    source->channels > AIUS_MAX_CHANNELS || source->frames == 0u ||
	    frame_offset >= source->frames)
		return AIUS_INVALID_ARG;
	uint32_t frames = source->frames - frame_offset;
	if (frames > AIUS_PIPELINE_MAX_OUTPUT_FRAMES)
		frames = AIUS_PIPELINE_MAX_OUTPUT_FRAMES;
	for (uint16_t ch = 0u; ch < source->channels; ch++)
		if (!source->planes[ch])
			return AIUS_INVALID_ARG;
	*slice = *source;
	slice->frames = frames;
	for (uint16_t ch = 0u; ch < source->channels; ch++)
		slice->planes[ch] = source->planes[ch] + frame_offset;
	return AIUS_OK;
}

AiusStatus aius_pipeline_service_one(AiusPipeline *p,
	                                 AiusPipelineServiceResult *result)
{
	if (!p || !result)
		return AIUS_INVALID_ARG;
	*result = AIUS_PIPELINE_SERVICE_IDLE;
	if (p->pending == PENDING_NONE)
		return AIUS_OK;

	if (p->pending == PENDING_DTS_AU || p->pending == PENDING_A52_AU ||
	    p->pending == PENDING_EAC3_AU) {
		AiusBurst b;
		if (aius_iec61937_current_burst(p->parser, &b) != AIUS_OK) {
			p->service_diag.au_abandons++;
			pipeline_reset_serviced(p);
			p->last_status = AIUS_MALFORMED;
			*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
			return AIUS_MALFORMED;
		}
		p->service_diag.decode_units++;
		AiusStatus ds;
		if (p->pending == PENDING_DTS_AU) {
			ds = aius_dts_decode_access_unit(p->dts_decoder, b.payload,
			                                 b.length, &p->retained_block);
#if AIUS_PIPELINE_EAC3
		} else if (p->pending == PENDING_EAC3_AU) {
			/* Single-shot like DTS: one AU in, one 256..1536-frame
			 * block out, sliced on output. A refusal (UNSUPPORTED:
			 * dual mono, back-centre modes, reduced rates; MALFORMED:
			 * garbage) is a decode reject, never an abandon -- no
			 * partial output exists. */
			ds = aius_eac3_decode_access_unit(p->eac3_decoder,
			                                  b.payload, b.length,
			                                  &p->retained_block);
#endif
		} else {
			ds = aius_a52_begin_access_unit(p->a52_decoder, b.payload,
			                                b.length);
			if (ds == AIUS_OK) {
				p->a52_inflight = 1u;
#ifdef AIUS_PIPELINE_TEST_HOOKS
				if (p->test_fail_a52_block == 1u) {
					p->test_fail_a52_block = 0u;
					ds = AIUS_MALFORMED;
				} else
#endif
					ds = aius_a52_decode_next_block(p->a52_decoder,
					                                &p->retained_block);
				if (ds == AIUS_OK)
					p->a52_blocks_decoded = 1u;
			}
		}
		p->last_status = ds;
		if (ds != AIUS_OK) {
			p->service_diag.decode_rejects++;
			p->service_diag.last_failure = ds;
			if (p->a52_inflight) {
				p->service_diag.au_abandons++;
				pipeline_reset_serviced(p);
				p->last_status = ds;
			} else {
				p->pending = PENDING_NONE;
				p->pending_pc = 0u;
				memset(&p->retained_block, 0,
				       sizeof(p->retained_block));
			}
			*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
			return AIUS_OK;
		}
		if (p->pending == PENDING_DTS_AU) {
			p->pending = PENDING_DTS_OUTPUT;
			p->codec_pc = p->pending_pc;
		} else if (p->pending == PENDING_EAC3_AU) {
			p->pending = PENDING_EAC3_OUTPUT;
			p->codec_pc = p->pending_pc;
			/* One decoded block IS the whole unit here. */
			note_substitution_shape(p, 21u, &p->retained_block,
			                        p->retained_block.frames);
		} else {
			p->pending = PENDING_A52_OUTPUT;
			/* AC-3's unit is fixed by the format at six 256-frame
			 * blocks, so the first block carries the whole shape --
			 * no need to wait for the sixth to learn it. */
			note_substitution_shape(p, 1u, &p->retained_block,
			                        AIUS_A52_BLOCK_SAMPLES *
			                                AIUS_A52_BLOCKS_PER_ACCESS_UNIT);
		}
		p->retained_offset = 0u;
	}

	if (p->pending == PENDING_A52_NEXT) {
		p->service_diag.decode_units++;
		AiusStatus ds;
#ifdef AIUS_PIPELINE_TEST_HOOKS
		if (p->test_fail_a52_block ==
		    (uint8_t)(p->a52_blocks_decoded + 1u)) {
			p->test_fail_a52_block = 0u;
			ds = AIUS_MALFORMED;
		} else
#endif
			ds = aius_a52_decode_next_block(p->a52_decoder,
			                                &p->retained_block);
		p->last_status = ds;
		if (ds != AIUS_OK) {
			p->service_diag.decode_rejects++;
			p->service_diag.au_abandons++;
			p->service_diag.last_failure = ds;
			pipeline_reset_serviced(p);
			p->last_status = ds;
			*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
			return AIUS_OK;
		}
		p->a52_blocks_decoded++;
		p->pending = PENDING_A52_OUTPUT;
		p->retained_offset = 0u;
	}

	if (p->pending == PENDING_PCM)
		fill_pcm_descriptor(p);
	else if (p->pending == PENDING_SILENCE)
		fill_silence_descriptor(p);
	else if ((p->pending == PENDING_DTS_OUTPUT ||
	          p->pending == PENDING_A52_OUTPUT ||
	          p->pending == PENDING_EAC3_OUTPUT) &&
	         !p->retained_retry) {
		AiusStatus slice_status =
			aius_pipeline_slice_pcm_block(&p->retained_block,
			                              p->retained_offset,
			                              &p->sink_block);
		if (slice_status != AIUS_OK) {
			p->service_diag.decode_rejects++;
			p->service_diag.last_failure = slice_status;
			p->service_diag.au_abandons++;
			pipeline_reset_serviced(p);
			p->last_status = slice_status;
			*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
			return AIUS_OK;
		}
		/* Unit start: AC-3 block 0 of 6; E-AC-3 and DTS the first slice
		 * of each access unit. Set once here, so a retried block (which
		 * skips this branch) keeps it. */
		p->sink_block.first_in_unit =
			(p->pending == PENDING_A52_OUTPUT)
				? (uint8_t)(p->a52_blocks_decoded == 1u)
				: (uint8_t)(p->retained_offset == 0u);
	}

	p->service_diag.sink_attempts++;
	if (p->retained_retry)
		p->service_diag.retained_retries++;
	AiusStatus st = p->sink.consume(p->sink.ctx, &p->sink_block);
	if (st == AIUS_NO_SPACE) {
		p->service_diag.transmit_stalls++;
		p->retained_retry = 1u;
		*result = AIUS_PIPELINE_SERVICE_TRANSMIT_BLOCKED;
		return AIUS_NO_SPACE;
	}
	const int refused = st == AIUS_REFUSED;
	if (st != AIUS_OK && !refused) {
		p->service_diag.output_drops++;
		if (p->pending == PENDING_DTS_OUTPUT ||
		    p->pending == PENDING_A52_OUTPUT ||
		    p->pending == PENDING_EAC3_OUTPUT)
			p->service_diag.au_abandons++;
		p->service_diag.last_failure = st;
		*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
		pipeline_reset_serviced(p);
		p->last_status = st;
		return st;
	}

	if (!refused)
		p->service_diag.committed_blocks++;
	if (p->pending == PENDING_PCM) {
		p->pcm_frames = 0u;
		p->pending = PENDING_NONE;
	} else if (p->pending == PENDING_SILENCE) {
		/* A REFUSED substituted block still advances: the sink declined
		 * the block on its own terms (an unsupported or mismatched rate
		 * closes the gate), and re-offering the same zeros forever would
		 * wedge the pipeline on the one path that exists to keep it
		 * moving. codec_pc is deliberately NOT set: nothing decoded. */
		p->sub_frames_left -= p->sink_block.frames;
		if (p->sub_frames_left == 0u)
			p->pending = PENDING_NONE;
	} else if (p->pending == PENDING_DTS_OUTPUT ||
	           p->pending == PENDING_EAC3_OUTPUT) {
		p->retained_offset += p->sink_block.frames;
		if (p->retained_offset >= p->retained_block.frames) {
			p->pending = PENDING_NONE;
			p->pending_pc = 0u;
		}
	} else {
		if (p->a52_blocks_decoded == AIUS_A52_BLOCKS_PER_ACCESS_UNIT) {
			p->codec_pc = p->pending_pc;
			p->a52_inflight = 0u;
			p->pending = PENDING_NONE;
			p->pending_pc = 0u;
		} else {
			p->pending = PENDING_A52_NEXT;
		}
	}
	p->retained_retry = 0u;
	memset(&p->sink_block, 0, sizeof(p->sink_block));
	if (p->pending == PENDING_NONE) {
		p->retained_offset = 0u;
		memset(&p->retained_block, 0, sizeof(p->retained_block));
	}
	*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
	return AIUS_OK;
}

int aius_pipeline_has_pending(const AiusPipeline *p)
{
	return p && p->pending != PENDING_NONE;
}

int aius_pipeline_has_buffered_input(const AiusPipeline *p)
{
	return p && p->cls_off < p->cls_n &&
	       p->klass != AIUS_PIPELINE_UNKNOWN;
}

AiusStatus aius_pipeline_push(AiusPipeline *p, const uint16_t *words, size_t count)
{
	if (!p || (count > 0u && !words))
		return AIUS_INVALID_ARG;
	size_t off = 0u;
	while (off < count || aius_pipeline_has_pending(p) ||
	       aius_pipeline_has_buffered_input(p)) {
		if (aius_pipeline_has_pending(p)) {
			AiusPipelineServiceResult result;
			AiusStatus st = aius_pipeline_service_one(p, &result);
			if (st != AIUS_OK) {
				if (st == AIUS_NO_SPACE) {
					p->service_diag.output_drops++;
					if (p->pending == PENDING_DTS_OUTPUT ||
					    p->pending == PENDING_A52_OUTPUT ||
					    p->pending == PENDING_EAC3_OUTPUT)
						p->service_diag.au_abandons++;
					pipeline_reset_serviced(p);
				}
				return st;
			}
			continue;
		}
		size_t accepted = 0u;
		AiusStatus st =
			aius_pipeline_ingest(p, off < count ? words + off : NULL,
			                     count - off, &accepted);
		off += accepted;
		if (st != AIUS_OK)
			return st;
		if (accepted == 0u && !aius_pipeline_has_pending(p))
			break;
	}
	return AIUS_OK;
}

AiusStatus aius_pipeline_finish(AiusPipeline *p)
{
	if (!p)
		return AIUS_INVALID_ARG;

	AiusStatus rc = AIUS_OK;
	while (p->pending != PENDING_NONE) {
		AiusPipelineServiceResult result;
		rc = aius_pipeline_service_one(p, &result);
		if (rc != AIUS_OK)
			return rc;
	}

	/* A completed work item can leave a typed suffix in the classification
	 * staging buffer. Route and service that accepted suffix before finalizing
	 * either stream. */
	while (aius_pipeline_has_buffered_input(p)) {
		const size_t before = p->cls_off;
		size_t consumed = 0u;
		rc = aius_pipeline_ingest(p, NULL, 0u, &consumed);
		if (rc != AIUS_OK)
			return rc;
		while (p->pending != PENDING_NONE) {
			AiusPipelineServiceResult result;
			rc = aius_pipeline_service_one(p, &result);
			if (rc != AIUS_OK)
				return rc;
		}
		if (aius_pipeline_has_buffered_input(p) && p->cls_off == before)
			return AIUS_MALFORMED;
	}

	/* No preamble ever arrived: the staged words are linear PCM. They are a
	 * sub-window run, so the squelch drops all of them -- an unconfirmed
	 * promotion is exactly the case that must not reach the sink. */
	if (p->klass == AIUS_PIPELINE_UNKNOWN && p->cls_n > 0) {
		pcm_enter(p);
		size_t routed = 0u;
		rc = route_ingest(p, p->clsbuf, p->cls_n, &routed);
		p->cls_n = p->cls_off = 0u;
	}

	if (rc == AIUS_OK && p->klass == AIUS_PIPELINE_PCM &&
	    p->pcm_frames > 0u) {
		p->pending = PENDING_PCM;
		AiusPipelineServiceResult result;
		rc = aius_pipeline_service_one(p, &result);
		/* A dangling half-frame (lone L) is incomplete and dropped. */
	}

	AiusStatus fs = aius_iec61937_finish(p->parser);
	if (rc == AIUS_OK && fs != AIUS_OK)
		rc = fs;
	return rc;
}

void aius_pipeline_reset(AiusPipeline *p)
{
	if (!p)
		return;
	/* The source-teardown entry point: the audio service calls this on every
	 * ingress source/gap generation change, and the operator's clear path
	 * calls it too. Partial compressed input dies here, once and counted,
	 * before any decode can be attempted on it (defect 35). */
	drop_staged_compressed_input(p);
	pipeline_reset_state(p);
}

AiusStatus aius_pipeline_set_pcm_sample_rate(AiusPipeline *p,
                                             uint32_t sample_rate_hz)
{
	if (!p || (sample_rate_hz != 32000u && sample_rate_hz != 44100u &&
	           sample_rate_hz != 48000u))
		return AIUS_INVALID_ARG;
	p->pcm_sample_rate_hz = sample_rate_hz;
	return AIUS_OK;
}

void aius_pipeline_get_service_diag(const AiusPipeline *p,
	                                AiusPipelineServiceDiag *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (p)
		*out = p->service_diag;
}

AiusStatus aius_pipeline_get_crc_rejects(const AiusPipeline *p,
                                         uint32_t *rejects,
                                         uint32_t *last_bytes)
{
	if (!p || !rejects || !last_bytes)
		return AIUS_INVALID_ARG;
	*rejects = p->service_diag.crc_rejects;
	*last_bytes = p->service_diag.crc_reject_last_bytes;
	return AIUS_OK;
}

void aius_pipeline_clear_service_diag(AiusPipeline *p)
{
	if (!p)
		return;
	memset(&p->service_diag, 0, sizeof(p->service_diag));
	p->service_diag.last_failure = AIUS_OK;
	/* last_status is an OBSERVATION LATCH, and the operator-invoked clear owns
	 * resetting observation latches; automatic transitions deliberately do not.
	 * A swallowed fault must outlive every re-classification that follows it,
	 * or the surface that exists to make a silent decoder visible would erase
	 * its own evidence -- which is why neither a re-type nor a re-lock touches
	 * this. But a DELIBERATE teardown (pulling the AC-3 source to watch the
	 * PCM re-type, the documented AC-3-to-PCM diagnostic) leaves AIUS_MALFORMED
	 * latched, and without this the operator's clear left it reading as a live
	 * fault in pipeline_diag.last_status until the next reboot. */
	p->last_status = AIUS_OK;
}

AiusStatus aius_pipeline_get_parser_diag(const AiusPipeline *p,
                                         AiusIecDiag *out)
{
	if (!p || !out)
		return AIUS_INVALID_ARG;
	return aius_iec61937_get_diag(p->parser, out);
}

AiusPipelineClass aius_pipeline_classification(const AiusPipeline *p)
{
	return p ? p->klass : AIUS_PIPELINE_UNKNOWN;
}

AiusStatus aius_pipeline_get_reclass_counters(const AiusPipeline *p,
                                              uint32_t *retype,
                                              uint32_t *reclassify)
{
	if (!p || !retype || !reclassify)
		return AIUS_INVALID_ARG;
	*retype = p->retype_count;
	*reclassify = p->reclassify_count;
	return AIUS_OK;
}

AiusStatus aius_pipeline_get_squelch_diag(const AiusPipeline *p,
                                          uint32_t *squelched_frames,
                                          int *pcm_confirmed)
{
	if (!p || !squelched_frames || !pcm_confirmed)
		return AIUS_INVALID_ARG;
	*squelched_frames = p->squelched_frames;
	/* Derived, not stored: the gate IS the class plus the run counter, and a
	 * separate flag could only ever disagree with them. */
	*pcm_confirmed = (p->klass == AIUS_PIPELINE_PCM &&
	                  p->pcm_run_words >= AIUS_PIPELINE_PCM_CONFIRM_WORDS)
	                         ? 1
	                         : 0;
	return AIUS_OK;
}

AiusStatus aius_pipeline_get_metrics(const AiusPipeline *p, AiusMetrics *out)
{
	if (!p || !out)
		return AIUS_INVALID_ARG;
	if (p->codec_pc != 0u) {
		AiusStatus st =
			aius_pipeline_get_codec_metrics(p, p->codec_pc, out);
		if (st != AIUS_OK)
			return st;
	} else {
		memset(out, 0, sizeof(*out));
	}
	/* The slot's cumulative high-water (reset retains it) is the peak decoder
	 * memory across every residency -- the number that sizes the slot. Heap
	 * decoders have no arena and keep the projected codec's 0. */
	if (p->arena_backed)
		out->arena_high_water =
			(uint32_t)aius_arena_high_water(&p->codec_slot);
	out->last_status = p->last_status;
	return AIUS_OK;
}

uint16_t aius_pipeline_codec_pc(const AiusPipeline *p)
{
	return p ? p->codec_pc : 0u;
}

uint16_t aius_pipeline_resident_codec(const AiusPipeline *p)
{
	return p ? p->resident_family : 0u;
}

AiusStatus aius_pipeline_get_codec_metrics(const AiusPipeline *p,
                                           uint16_t codec_pc,
                                           AiusMetrics *out)
{
	if (!p || !out)
		return AIUS_INVALID_ARG;
	if (codec_pc != 1u && codec_pc != 11u && codec_pc != 12u &&
	    codec_pc != 13u
#if AIUS_PIPELINE_EAC3
	    && codec_pc != 21u
#endif
	)
		return AIUS_INVALID_ARG;
	if (pc_family(codec_pc) != p->resident_family) {
		/* Not resident: its state left with the slot. */
		memset(out, 0, sizeof(*out));
		return AIUS_OK;
	}
	if (codec_pc == 1u)
		return aius_a52_get_metrics(p->a52_decoder, out);
#if AIUS_PIPELINE_EAC3
	if (codec_pc == 21u)
		return aius_eac3_get_metrics(p->eac3_decoder, out);
#endif
	return aius_dts_get_metrics(p->dts_decoder, out);
}

#ifdef AIUS_PIPELINE_TEST_HOOKS
AiusStatus aius_pipeline_test_fail_a52_block(AiusPipeline *p,
                                             uint8_t block_number)
{
	if (!p || block_number > AIUS_A52_BLOCKS_PER_ACCESS_UNIT)
		return AIUS_INVALID_ARG;
	p->test_fail_a52_block = block_number;
	return AIUS_OK;
}
#endif

/* ---- lifecycle ----------------------------------------------------------- */

/* Validate config + sink and initialise the non-owning fields of a zeroed
 * instance. Does NOT create the parser/decoder. */
static AiusStatus init_fields(AiusPipeline *p, const AiusPipelineConfig *cfg,
                              const AiusPcmSink *sink)
{
	AiusIec61937Packing packing = AIUS_IEC61937_PACK_16;
	size_t window = AIUS_PIPELINE_CLASSIFY_MAX_WORDS;
	if (cfg) {
		if (cfg->packing != AIUS_IEC61937_PACK_16 &&
		    cfg->packing != AIUS_IEC61937_PACK_14)
			return AIUS_INVALID_ARG;
		packing = cfg->packing;
		if (cfg->classify_window_words != 0 &&
		    cfg->classify_window_words < AIUS_PIPELINE_CLASSIFY_MAX_WORDS)
			window = cfg->classify_window_words;
	}
	if (!sink || !sink->consume)
		return AIUS_INVALID_ARG;

	p->sink = *sink;
	p->window = window;
	p->klass = AIUS_PIPELINE_UNKNOWN;
	p->last_status = AIUS_OK;
	p->pcm_sample_rate_hz = PCM_DEFAULT_SAMPLE_RATE_HZ;
	if (cfg)
		p->cycle_clock = cfg->cycle_clock;

	AiusIec61937Config pcfg = { .packing = packing };
	return aius_iec61937_parser_create(&p->parser, &pcfg);
}

AiusStatus aius_pipeline_create(AiusPipeline **out, const AiusPipelineConfig *cfg,
                                const AiusPcmSink *sink)
{
	if (!out)
		return AIUS_INVALID_ARG;
	*out = NULL;

	AiusPipeline *p = calloc(1, sizeof(*p));
	if (!p)
		return AIUS_NO_SPACE;
	p->arena_backed = 0;

	AiusStatus st = init_fields(p, cfg, sink);
	if (st != AIUS_OK) {
		free(p);
		return st;
	}
	/* No decoder yet: the slot is filled lazily by ensure_codec_resident at
	 * the first completed compressed burst. */
	*out = p;
	return AIUS_OK;
}

AiusStatus aius_pipeline_create_arena(AiusPipeline **out,
                                      const AiusPipelineConfig *cfg,
                                      const AiusPcmSink *sink, AiusArena *arena)
{
	if (!out)
		return AIUS_INVALID_ARG;
	*out = NULL;
	if (!arena)
		return AIUS_INVALID_ARG;

	AiusPipeline *p = aius_arena_alloc(arena, sizeof(*p), 16);
	if (!p)
		return AIUS_NO_SPACE;
	memset(p, 0, sizeof(*p));
	p->arena_backed = 1;

	AiusStatus st = init_fields(p, cfg, sink);
	if (st != AIUS_OK)
		return st; /* arena storage reclaimed by the caller discarding the arena */
	/* Carve the fixed codec slot; the decoder the stream needs is created
	 * into it lazily by ensure_codec_resident. Carving up front keeps the
	 * capacity a create-time guarantee rather than a first-burst surprise. */
	void *slot = aius_arena_alloc(arena, AIUS_PIPELINE_CODEC_SLOT_BYTES, 16);
	if (!slot) {
		aius_iec61937_parser_destroy(p->parser);
		return AIUS_NO_SPACE;
	}
	if (aius_arena_init(&p->codec_slot, slot,
	                    AIUS_PIPELINE_CODEC_SLOT_BYTES) != 0) {
		aius_iec61937_parser_destroy(p->parser);
		return AIUS_INVALID_ARG;
	}
	*out = p;
	return AIUS_OK;
}

void aius_pipeline_destroy(AiusPipeline *p)
{
	if (!p)
		return;
	cleanup_pending(p);
	destroy_resident(p);
	aius_iec61937_parser_destroy(p->parser);
	if (!p->arena_backed)
		free(p);
}
