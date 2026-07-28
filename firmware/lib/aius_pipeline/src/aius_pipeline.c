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

#include <stdlib.h>
#include <string.h>

/* IEC 61937 preamble, big-endian wire (Pa,Pb) and its byte-swapped little-endian
 * form. Classification only needs the pair; the parser re-detects order per burst. */
#define IEC_PA_BE 0xF872u
#define IEC_PB_BE 0x4E1Fu
#define IEC_PA_LE 0x72F8u
#define IEC_PB_LE 0x1F4Eu

struct AiusPipeline {
	AiusIec61937Parser *parser;
	AiusDtsDecoder *decoder;
	AiusPcmSink sink;

	AiusPipelineClass klass;
	size_t window;             /* classification window in words (<= MAX) */
	int arena_backed;          /* instance/decoder from an arena: do not free() */
	AiusStatus last_status;    /* last parse/decode result observed */

	/* Classification staging: raw words held until the stream is typed. */
	uint16_t clsbuf[AIUS_PIPELINE_CLASSIFY_MAX_WORDS];
	size_t cls_n;

	/* PCM pass-through staging (interleaved L,R -> planar). */
	int32_t pcm_l[AIUS_PIPELINE_PCM_BLOCK_FRAMES];
	int32_t pcm_r[AIUS_PIPELINE_PCM_BLOCK_FRAMES];
	uint32_t pcm_frames;       /* complete L/R frames staged */
	int32_t pcm_pending_l;     /* a lone L sample awaiting its R */
	int pcm_have_half;

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
};

/* A re-type inside finish()'s PCM-promotion flush is impossible only while the
 * minimum pair spacing spans the whole classify window; keep the coupling. */
_Static_assert(AIUS_PIPELINE_RETYPE_MIN_SPACING_WORDS >=
               AIUS_PIPELINE_CLASSIFY_MAX_WORDS,
               "retype MIN spacing must cover the classify window");

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

static AiusStatus dts_feed(AiusPipeline *p, const uint16_t *w, size_t n);

static AiusStatus pcm_emit_block(AiusPipeline *p)
{
	if (p->pcm_frames == 0)
		return AIUS_OK;

	AiusPcmBlock blk;
	memset(&blk, 0, sizeof(blk));
	blk.planes[0] = p->pcm_l;
	blk.planes[1] = p->pcm_r;
	blk.channels = 2;
	blk.frames = p->pcm_frames;
	blk.sample_rate_hz = AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ;
	blk.channel_mask = AIUS_CH_FL | AIUS_CH_FR;

	AiusStatus st = p->sink.consume(p->sink.ctx, &blk);
	p->pcm_frames = 0;
	return st;
}

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
                                const uint16_t *rest, size_t n)
{
	p->pcm_frames = 0;
	p->pcm_have_half = 0;
	p->pcm_pending_l = 0;
	p->rt_have_prev = 0;
	p->rt_cand_valid = 0;
	aius_iec61937_parser_reset(p->parser);
	p->klass = AIUS_PIPELINE_DTS;
	p->dts_starve_words = 0;
	p->retype_count++;

	const uint16_t pair[2] = { pa, pb };
	AiusStatus st = dts_feed(p, pair, 2);
	if (st != AIUS_OK)
		return st;
	return dts_feed(p, rest, n);
}

static AiusStatus pcm_feed(AiusPipeline *p, const uint16_t *w, size_t n)
{
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
					    form == (int)p->rt_cand_form)
						return retype_to_dts(p, p->rt_prev_word, word,
						                     w + i + 1, n - i - 1);
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
			AiusStatus st = pcm_emit_block(p);
			if (st != AIUS_OK)
				return st;
		}
	}
	return AIUS_OK;
}

/* ---- DTS de-frame -> decode ---------------------------------------------- */

static AiusStatus dts_feed(AiusPipeline *p, const uint16_t *w, size_t n)
{
	/* Starvation accounting (defect 28): presume the push sterile, then any
	 * burst outcome below -- completed OR rejected, either is framing
	 * evidence -- zeroes the counter. Granularity is one push; words after
	 * the last outcome inside the same push go uncounted, a bounded
	 * undercount the threshold's 4x margin absorbs. */
	p->dts_starve_words += (uint32_t)n;

	size_t pos = 0;
	while (pos < n) {
		size_t consumed = 0;
		AiusStatus st = aius_iec61937_push(p->parser, w + pos, n - pos, &consumed);
		pos += consumed;

		if (st == AIUS_NEED_MORE)
			break;
		if (st == AIUS_INVALID_ARG)
			return AIUS_INVALID_ARG;

		p->dts_starve_words = 0;

		if (st == AIUS_OK) {
			/* A supported (DTS Core) burst completed: decode its access unit. */
			AiusBurst b;
			if (aius_iec61937_current_burst(p->parser, &b) == AIUS_OK) {
				AiusPcmBlock blk;
				AiusStatus ds = aius_dts_decode_access_unit(p->decoder, b.payload,
				                                            b.length, &blk);
				p->last_status = ds;
				if (ds == AIUS_OK) {
					AiusStatus cs = p->sink.consume(p->sink.ctx, &blk);
					if (cs != AIUS_OK)
						return cs;
				}
				/* A decode fault is non-fatal: skip this burst, keep streaming. */
			}
		} else {
			/* UNSUPPORTED / MALFORMED / NO_SPACE: skip; the parser re-locks. */
			p->last_status = st;
		}

		if (consumed == 0)
			break; /* defensive: never spin on a non-advancing parser */
	}
	return AIUS_OK;
}

/* ---- routing ------------------------------------------------------------- */

/* Route already-decided-class words to the matching consumer. */
static AiusStatus route(AiusPipeline *p, const uint16_t *w, size_t n)
{
	if (p->klass == AIUS_PIPELINE_DTS)
		return dts_feed(p, w, n);
	if (p->klass == AIUS_PIPELINE_PCM)
		return pcm_feed(p, w, n);
	return AIUS_OK;
}

AiusStatus aius_pipeline_push(AiusPipeline *p, const uint16_t *words, size_t count)
{
	if (!p)
		return AIUS_INVALID_ARG;
	if (count > 0 && !words)
		return AIUS_INVALID_ARG;
	if (count == 0)
		return AIUS_OK;

	/* Push-boundary re-judgements (defect 28). Starvation: a DTS class that
	 * consumed the starve budget with no parser burst outcome returns to
	 * UNKNOWN, and this push's words re-classify through the normal staging
	 * path below. Candidate expiry: a re-type candidate older than the MAX
	 * spacing can never validate and is dropped (which also removes the
	 * uint32 position-wrap aliasing of an hours-stale candidate). */
	if (p->klass == AIUS_PIPELINE_DTS &&
	    p->dts_starve_words > AIUS_PIPELINE_DTS_STARVE_WORDS) {
		aius_iec61937_parser_reset(p->parser);
		p->klass = AIUS_PIPELINE_UNKNOWN;
		p->cls_n = 0;
		p->dts_starve_words = 0;
		p->reclassify_count++;
	}
	if (p->klass == AIUS_PIPELINE_PCM && p->rt_cand_valid &&
	    p->rt_pos - p->rt_cand_pos >
	        AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS + 1u)
		p->rt_cand_valid = 0;
	/* ^ +1: with the candidate's Pa as the LAST word of a push, its age at
	 * the next boundary is delta+1 -- a bare > MAX would expire a candidate
	 * whose validating Pb (delta exactly MAX) is still legal. */

	size_t off = 0;

	/* Stage words until the stream is typed, then flush the staged run. */
	while (off < count && p->klass == AIUS_PIPELINE_UNKNOWN) {
		size_t room = p->window - p->cls_n;
		size_t take = count - off;
		if (take > room)
			take = room;
		memcpy(p->clsbuf + p->cls_n, words + off, take * sizeof(uint16_t));
		p->cls_n += take;
		off += take;

		p->klass = aius_pipeline_classify(p->clsbuf, p->cls_n, p->window);
		if (p->klass == AIUS_PIPELINE_DTS)
			p->dts_starve_words = 0; /* every DTS entry starts unstarved */
		if (p->klass != AIUS_PIPELINE_UNKNOWN) {
			AiusStatus st = route(p, p->clsbuf, p->cls_n);
			p->cls_n = 0;
			if (st != AIUS_OK)
				return st;
		}
		/* Still UNKNOWN only when cls_n < window; the loop takes more input. */
	}

	if (off < count)
		return route(p, words + off, count - off);
	return AIUS_OK;
}

AiusStatus aius_pipeline_finish(AiusPipeline *p)
{
	if (!p)
		return AIUS_INVALID_ARG;

	AiusStatus rc = AIUS_OK;

	/* No preamble ever arrived: the staged words are linear PCM. */
	if (p->klass == AIUS_PIPELINE_UNKNOWN && p->cls_n > 0) {
		p->klass = AIUS_PIPELINE_PCM;
		rc = route(p, p->clsbuf, p->cls_n);
		p->cls_n = 0;
	}

	if (p->klass == AIUS_PIPELINE_PCM) {
		AiusStatus st = pcm_emit_block(p); /* flush a partial block */
		if (rc == AIUS_OK)
			rc = st;
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
	p->cls_n = 0;
	p->pcm_frames = 0;
	p->pcm_have_half = 0;
	p->pcm_pending_l = 0;
	p->last_status = AIUS_OK;
	/* Detector/starvation STATE clears; the two lifetime counters do not --
	 * they are the live proof that a re-type (not a reboot) happened. */
	p->rt_have_prev = 0;
	p->rt_cand_valid = 0;
	p->rt_pos = 0;
	p->dts_starve_words = 0;
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

AiusStatus aius_pipeline_get_metrics(const AiusPipeline *p, AiusMetrics *out)
{
	if (!p || !out)
		return AIUS_INVALID_ARG;
	AiusStatus st = aius_dts_get_metrics(p->decoder, out);
	if (st == AIUS_OK)
		out->last_status = p->last_status;
	return st;
}

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
	st = aius_dts_decoder_create(&p->decoder);
	if (st != AIUS_OK) {
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
	st = aius_dts_decoder_create_arena(&p->decoder, arena);
	if (st != AIUS_OK) {
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
	aius_iec61937_parser_destroy(p->parser);
	aius_dts_decoder_destroy(p->decoder);
	if (!p->arena_backed)
		free(p);
}
