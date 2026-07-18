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
};

/* ---- classifier ---------------------------------------------------------- */

static int is_preamble_pair(uint16_t a, uint16_t b)
{
	return (a == IEC_PA_BE && b == IEC_PB_BE) ||
	       (a == IEC_PA_LE && b == IEC_PB_LE);
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

static AiusStatus pcm_feed(AiusPipeline *p, const uint16_t *w, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int32_t sample = (int32_t)(int16_t)w[i]; /* sign-extend 16 -> 32 */
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
	size_t pos = 0;
	while (pos < n) {
		size_t consumed = 0;
		AiusStatus st = aius_iec61937_push(p->parser, w + pos, n - pos, &consumed);
		pos += consumed;

		if (st == AIUS_NEED_MORE)
			break;
		if (st == AIUS_INVALID_ARG)
			return AIUS_INVALID_ARG;

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
}

AiusPipelineClass aius_pipeline_classification(const AiusPipeline *p)
{
	return p ? p->klass : AIUS_PIPELINE_UNKNOWN;
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
