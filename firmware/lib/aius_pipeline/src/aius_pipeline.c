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
	PENDING_A52_OUTPUT
} PendingKind;

struct AiusPipeline {
	AiusIec61937Parser *parser;
	AiusDtsDecoder *dts_decoder;
	AiusA52Decoder *a52_decoder;
	AiusPcmSink sink;

	AiusPipelineClass klass;
	size_t window;             /* classification window in words (<= MAX) */
	int arena_backed;          /* instance/decoder from an arena: do not free() */
	AiusStatus last_status;    /* last parse/decode result observed */

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

	/* DTS starvation: words consumed by the DTS route since the last parser
	 * burst outcome (completed or rejected). Zeroed on EVERY entry to the DTS
	 * class and when starvation fires -- stale residue across a class
	 * transition would immediately re-starve a valid re-type. */
	uint32_t dts_starve_words;

	/* Lifetime diagnostics: survive aius_pipeline_reset(), cleared at create. */
	uint32_t retype_count;
	uint32_t reclassify_count;
	uint32_t squelched_frames;

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

/* Complete a validated PCM->DTS re-type (defect 28): drop the staged
 * pass-through tail (at a genuine format switch it is burst data misread as
 * PCM -- its Pa is in there too), start the parser clean from the validated
 * pair, and route the remainder of the caller's chunk after it. */
static AiusStatus retype_to_dts(AiusPipeline *p, uint16_t pa, uint16_t pb,
	                            const uint16_t *rest, size_t n,
	                            size_t *consumed)
{
	p->pcm_frames = 0;
	p->pcm_have_half = 0;
	p->pcm_pending_l = 0;
	p->rt_have_prev = 0;
	p->rt_cand_valid = 0;
	aius_iec61937_parser_reset(p->parser);
	p->klass = AIUS_PIPELINE_DTS;
	p->codec_pc = 0u;
	p->dts_starve_words = 0;
	p->retype_count++;

	const uint16_t pair[2] = { pa, pb };
	size_t pair_consumed = 0u;
	AiusStatus st = dts_ingest(p, pair, 2u, &pair_consumed);
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
}

static AiusStatus pcm_ingest(AiusPipeline *p, const uint16_t *w, size_t n,
	                         size_t *consumed)
{
	*consumed = 0u;
	for (size_t i = 0; i < n; i++) {
		const uint16_t word = w[i];

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
						size_t rest_consumed = 0u;
						AiusStatus st =
							retype_to_dts(p, p->rt_prev_word, word,
							              w + i + 1, n - i - 1,
							              &rest_consumed);
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

/* ---- DTS de-frame -> decode ---------------------------------------------- */

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
				if (b.data_type == 1u)
					p->pending = PENDING_A52_AU;
				else if (b.data_type == 11u || b.data_type == 12u ||
				         b.data_type == 13u)
					p->pending = PENDING_DTS_AU;
				else
					continue; /* parser owns the unsupported-code policy */
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
	if (p->a52_inflight)
		(void)aius_a52_abandon(p->a52_decoder);
	p->a52_inflight = 0u;
	p->a52_blocks_decoded = 0u;
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

static void begin_ingest_call(AiusPipeline *p)
{
	if (p->klass == AIUS_PIPELINE_DTS &&
	    p->dts_starve_words > AIUS_PIPELINE_DTS_STARVE_WORDS) {
		aius_iec61937_parser_reset(p->parser);
		cleanup_pending(p);
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

	if (p->pending == PENDING_DTS_AU || p->pending == PENDING_A52_AU) {
		AiusBurst b;
		if (aius_iec61937_current_burst(p->parser, &b) != AIUS_OK) {
			p->service_diag.au_abandons++;
			aius_pipeline_reset(p);
			p->last_status = AIUS_MALFORMED;
			*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
			return AIUS_MALFORMED;
		}
		p->service_diag.decode_units++;
		AiusStatus ds;
		if (p->pending == PENDING_DTS_AU) {
			ds = aius_dts_decode_access_unit(p->dts_decoder, b.payload,
			                                 b.length, &p->retained_block);
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
				aius_pipeline_reset(p);
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
		} else {
			p->pending = PENDING_A52_OUTPUT;
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
			aius_pipeline_reset(p);
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
	else if ((p->pending == PENDING_DTS_OUTPUT ||
	          p->pending == PENDING_A52_OUTPUT) &&
	         !p->retained_retry) {
		AiusStatus slice_status =
			aius_pipeline_slice_pcm_block(&p->retained_block,
			                              p->retained_offset,
			                              &p->sink_block);
		if (slice_status != AIUS_OK) {
			p->service_diag.decode_rejects++;
			p->service_diag.last_failure = slice_status;
			p->service_diag.au_abandons++;
			aius_pipeline_reset(p);
			p->last_status = slice_status;
			*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
			return AIUS_OK;
		}
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
	if (st != AIUS_OK) {
		p->service_diag.output_drops++;
		if (p->pending == PENDING_DTS_OUTPUT ||
		    p->pending == PENDING_A52_OUTPUT)
			p->service_diag.au_abandons++;
		p->service_diag.last_failure = st;
		*result = AIUS_PIPELINE_SERVICE_PROGRESSED;
		aius_pipeline_reset(p);
		p->last_status = st;
		return st;
	}

	p->service_diag.committed_blocks++;
	if (p->pending == PENDING_PCM) {
		p->pcm_frames = 0u;
		p->pending = PENDING_NONE;
	} else if (p->pending == PENDING_DTS_OUTPUT) {
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
					    p->pending == PENDING_A52_OUTPUT)
						p->service_diag.au_abandons++;
					aius_pipeline_reset(p);
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
	aius_iec61937_parser_reset(p->parser);
	p->klass = AIUS_PIPELINE_UNKNOWN;
	p->codec_pc = 0u;
	p->cls_n = 0;
	p->cls_off = 0;
	cleanup_pending(p);
	p->pcm_run_words = 0; /* the next PCM run re-earns its confirmation */
	p->last_status = AIUS_OK;
	/* Detector/starvation STATE clears; the two lifetime counters do not --
	 * they are the live proof that a re-type (not a reboot) happened. */
	p->rt_have_prev = 0;
	p->rt_cand_valid = 0;
	p->rt_pos = 0;
	p->dts_starve_words = 0;
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

void aius_pipeline_clear_service_diag(AiusPipeline *p)
{
	if (!p)
		return;
	memset(&p->service_diag, 0, sizeof(p->service_diag));
	p->service_diag.last_failure = AIUS_OK;
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
	AiusMetrics dts;
	AiusMetrics a52;
	AiusStatus ds = aius_dts_get_metrics(p->dts_decoder, &dts);
	AiusStatus as = aius_a52_get_metrics(p->a52_decoder, &a52);
	if (ds != AIUS_OK || as != AIUS_OK)
		return ds != AIUS_OK ? ds : as;
	if (p->codec_pc != 0u) {
		AiusStatus st =
			aius_pipeline_get_codec_metrics(p, p->codec_pc, out);
		if (st != AIUS_OK)
			return st;
	} else {
		memset(out, 0, sizeof(*out));
	}
	if (a52.arena_high_water > dts.arena_high_water)
		out->arena_high_water = a52.arena_high_water;
	else
		out->arena_high_water = dts.arena_high_water;
	out->last_status = p->last_status;
	return AIUS_OK;
}

uint16_t aius_pipeline_codec_pc(const AiusPipeline *p)
{
	return p ? p->codec_pc : 0u;
}

AiusStatus aius_pipeline_get_codec_metrics(const AiusPipeline *p,
                                           uint16_t codec_pc,
                                           AiusMetrics *out)
{
	if (!p || !out)
		return AIUS_INVALID_ARG;
	if (codec_pc == 1u)
		return aius_a52_get_metrics(p->a52_decoder, out);
	if (codec_pc == 11u || codec_pc == 12u || codec_pc == 13u)
		return aius_dts_get_metrics(p->dts_decoder, out);
	return AIUS_INVALID_ARG;
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
	st = aius_dts_decoder_create(&p->dts_decoder);
	if (st != AIUS_OK) {
		aius_iec61937_parser_destroy(p->parser);
		free(p);
		return st;
	}
	st = aius_a52_decoder_create(&p->a52_decoder);
	if (st != AIUS_OK) {
		aius_dts_decoder_destroy(p->dts_decoder);
		aius_iec61937_parser_destroy(p->parser);
		free(p);
		return st;
	}
	AiusCycleClock cycle_clock = {0};
	if (cfg)
		cycle_clock = cfg->cycle_clock;
	st = aius_dts_set_cycle_clock(p->dts_decoder, &cycle_clock);
	if (st == AIUS_OK)
		st = aius_a52_set_cycle_clock(p->a52_decoder, &cycle_clock);
	if (st != AIUS_OK) {
		aius_a52_decoder_destroy(p->a52_decoder);
		aius_dts_decoder_destroy(p->dts_decoder);
		aius_iec61937_parser_destroy(p->parser);
		free(p);
		return st;
	}
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
	st = aius_dts_decoder_create_arena(&p->dts_decoder, arena);
	if (st != AIUS_OK) {
		aius_iec61937_parser_destroy(p->parser);
		return st;
	}
	st = aius_a52_decoder_create_arena(&p->a52_decoder, arena);
	if (st != AIUS_OK) {
		aius_dts_decoder_destroy(p->dts_decoder);
		aius_iec61937_parser_destroy(p->parser);
		return st;
	}
	AiusCycleClock cycle_clock = {0};
	if (cfg)
		cycle_clock = cfg->cycle_clock;
	st = aius_dts_set_cycle_clock(p->dts_decoder, &cycle_clock);
	if (st == AIUS_OK)
		st = aius_a52_set_cycle_clock(p->a52_decoder, &cycle_clock);
	if (st != AIUS_OK) {
		aius_a52_decoder_destroy(p->a52_decoder);
		aius_dts_decoder_destroy(p->dts_decoder);
		aius_iec61937_parser_destroy(p->parser);
		return st;
	}
	*out = p;
	return AIUS_OK;
}

void aius_pipeline_destroy(AiusPipeline *p)
{
	if (!p)
		return;
	cleanup_pending(p);
	aius_iec61937_parser_destroy(p->parser);
	aius_a52_decoder_destroy(p->a52_decoder);
	aius_dts_decoder_destroy(p->dts_decoder);
	if (!p->arena_backed)
		free(p);
}
