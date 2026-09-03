/* Host test for the Aius S/PDIF input pipeline (classify + parser -> decoder).
 *
 * Proves, against the committed IEC 61937 corpus (tools/iec_corpus/vectors):
 *
 *   1. End-to-end DTS equivalence. Feeding an IEC-framed real DTS vector through
 *      the pipeline (classify -> de-frame -> decode) yields byte-exact the same
 *      planar PCM as feeding the vector's raw access unit straight to
 *      AiusDtsDecoder. Checked for the stereo (dts_be16) and 5.1 (dts51_be16)
 *      vectors, and across a one-word-at-a-time chunking.
 *   2. Classification. The DTS vector types as AIUS_PIPELINE_DTS; a synthetic
 *      linear-PCM word stream (no preamble) types as AIUS_PIPELINE_PCM. The
 *      stateless classifier is exercised on both plus window/undecided edges.
 *   3. PCM pass-through fidelity AND sample scale. Linear-PCM words emerge as
 *      2ch/48k blocks whose samples are the input words left-aligned to the
 *      AiusPcmBlock sample scale, in order. The scale half is asserted in the
 *      *contract's* terms (a full-scale wire word reaches full scale) rather than
 *      against a copy of the producer's own expression -- which is how this test
 *      previously held a 48.16 dB level defect green for the whole life of the
 *      pass-through. See aius_codec.h §SAMPLE SCALE. Every pass-through stream
 *      is measured FROM the confirmation squelch's boundary (run word
 *      AIUS_PIPELINE_PCM_CONFIRM_WORDS): nothing is emitted before it, and the
 *      first emitted frame is that boundary word pair, L-first.
 *   4. No-heap steady state. Two back-to-back DTS bursts decode with a non-zero
 *      warmup allocation then zero allocations on the steady-state frame (via the
 *      --wrap alloc shim + decoder metrics).
 *
 * Usage: test_pipeline <iec_corpus_dir>
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <math.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_a52.h"
#include "aius_dts.h"
#include "aius_eac3.h"
#include "aius_pipeline.h"

static int g_fail;

#define CHECK(cond, ...)                                                        \
	do {                                                                       \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL: ");                                          \
			fprintf(stderr, __VA_ARGS__);                                       \
			fprintf(stderr, "\n");                                              \
			g_fail++;                                                           \
		}                                                                      \
	} while (0)

/* ---- file / corpus loading ----------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *buf = malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	if (got != (size_t)n) {
		free(buf);
		return NULL;
	}
	*len = got;
	return buf;
}

/* Load a .iec wire file (16-bit words serialized big-endian on disk). */
static uint16_t *load_wire(const char *dir, const char *name, size_t *count)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.iec", dir, name);
	size_t bytes = 0;
	uint8_t *raw = read_file(path, &bytes);
	if (!raw) {
		fprintf(stderr, "cannot read %s\n", path);
		return NULL;
	}
	size_t n = bytes / 2;
	uint16_t *w = malloc((n ? n : 1) * sizeof(uint16_t));
	for (size_t i = 0; i < n; i++)
		w[i] = (uint16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);
	free(raw);
	*count = n;
	return w;
}

/* Load a .pay raw access-unit file (the parser's expected de-framed payload). */
static uint8_t *load_pay(const char *dir, const char *name, size_t *len)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.pay", dir, name);
	return read_file(path, len);
}

/* ---- captured-block sink -------------------------------------------------- */

struct block_copy {
	int32_t *planes[AIUS_MAX_CHANNELS];
	uint16_t channels;
	uint32_t frames;
	uint32_t sample_rate_hz;
	uint32_t channel_mask;
};

struct capture {
	struct block_copy blocks[64];
	int n;
	int overflow;
};

static AiusStatus capture_consume(void *ctx, const AiusPcmBlock *b)
{
	struct capture *c = ctx;
	if (c->n >= (int)(sizeof(c->blocks) / sizeof(c->blocks[0]))) {
		c->overflow = 1;
		return AIUS_NO_SPACE;
	}
	struct block_copy *bc = &c->blocks[c->n++];
	memset(bc, 0, sizeof(*bc));
	bc->channels = b->channels;
	bc->frames = b->frames;
	bc->sample_rate_hz = b->sample_rate_hz;
	bc->channel_mask = b->channel_mask;
	for (int ch = 0; ch < b->channels && ch < AIUS_MAX_CHANNELS; ch++) {
		bc->planes[ch] = malloc((size_t)b->frames * sizeof(int32_t));
		memcpy(bc->planes[ch], b->planes[ch], (size_t)b->frames * sizeof(int32_t));
	}
	return AIUS_OK;
}

static void capture_free(struct capture *c)
{
	for (int i = 0; i < c->n; i++)
		for (int ch = 0; ch < AIUS_MAX_CHANNELS; ch++)
			free(c->blocks[i].planes[ch]);
	memset(c, 0, sizeof(*c));
}

/* ---- direct decode of a raw access unit (reference) ---------------------- */

/* Decode one raw DTS Core access unit straight through AiusDtsDecoder and copy
 * the resulting block. Returns 0 on success. */
static int direct_decode(const uint8_t *au, size_t au_len, struct block_copy *out)
{
	AiusDtsDecoder *dec = NULL;
	if (aius_dts_decoder_create(&dec) != AIUS_OK)
		return -1;
	AiusPcmBlock blk;
	AiusStatus st = aius_dts_decode_access_unit(dec, au, au_len, &blk);
	if (st != AIUS_OK) {
		aius_dts_decoder_destroy(dec);
		return -1;
	}
	memset(out, 0, sizeof(*out));
	out->channels = blk.channels;
	out->frames = blk.frames;
	out->sample_rate_hz = blk.sample_rate_hz;
	out->channel_mask = blk.channel_mask;
	for (int ch = 0; ch < blk.channels && ch < AIUS_MAX_CHANNELS; ch++) {
		out->planes[ch] = malloc((size_t)blk.frames * sizeof(int32_t));
		memcpy(out->planes[ch], blk.planes[ch], (size_t)blk.frames * sizeof(int32_t));
	}
	aius_dts_decoder_destroy(dec);
	return 0;
}

/* Byte-exact planar-PCM comparison of two blocks. */
static int blocks_equal(const struct block_copy *a, const struct block_copy *b)
{
	if (a->channels != b->channels || a->frames != b->frames ||
	    a->sample_rate_hz != b->sample_rate_hz ||
	    a->channel_mask != b->channel_mask)
		return 0;
	for (int ch = 0; ch < a->channels; ch++) {
		if (!a->planes[ch] || !b->planes[ch])
			return 0;
		if (memcmp(a->planes[ch], b->planes[ch],
		           (size_t)a->frames * sizeof(int32_t)) != 0)
			return 0;
	}
	return 1;
}

static AiusPipelineClass run_pipeline(const uint16_t *w, size_t n,
                                      size_t chunk, struct capture *cap);
static void push_chunked(AiusPipeline *p, const uint16_t *w, size_t n,
                         size_t chunk, const char *label);

static double goertzel_blocks(const struct capture *cap, int channel,
                              double frequency_hz)
{
	const double omega = 2.0 * 3.14159265358979323846 *
	                     frequency_hz / 48000.0;
	const double coeff = 2.0 * cos(omega);
	double s1 = 0.0;
	double s2 = 0.0;
	for (int b = 0; b < cap->n; b++) {
		if (channel >= cap->blocks[b].channels)
			continue;
		for (uint32_t n = 0; n < cap->blocks[b].frames; n++) {
			double s0 = (double)cap->blocks[b].planes[channel][n] +
			            coeff * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
	}
	return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static void test_ac3_dispatch_and_identity(const char *dir)
{
	size_t n = 0u;
	uint16_t *wire = load_wire(dir, "ac3_surround51_ident", &n);
	CHECK(wire != NULL, "ac3: corpus load");
	if (!wire)
		return;

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPipelineClass klass = run_pipeline(wire, n, 256u, &cap);
	const uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
	                      AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR;
	CHECK(klass == AIUS_PIPELINE_DTS, "ac3: class %d != bitstream", klass);
	CHECK(cap.n == 12, "ac3: %d blocks != 12 (two six-block AUs)", cap.n);
	for (int b = 0; b < cap.n; b++)
		CHECK(cap.blocks[b].channels == 6u &&
		          cap.blocks[b].frames == AIUS_A52_BLOCK_SAMPLES &&
		          cap.blocks[b].sample_rate_hz == 48000u &&
		          cap.blocks[b].channel_mask == mask,
		      "ac3: block %d descriptor (%u,%u,%u,%08x)", b,
		      cap.blocks[b].channels, cap.blocks[b].frames,
		      cap.blocks[b].sample_rate_hz, cap.blocks[b].channel_mask);

	if (cap.n == 12) {
		static const double tones[6] = { 300, 600, 900, 60, 1200, 1500 };
		for (int channel = 0; channel < 6; channel++) {
			double own = goertzel_blocks(&cap, channel, tones[channel]);
			double next = 0.0;
			for (int tone = 0; tone < 6; tone++) {
				if (tone == channel)
					continue;
				double magnitude =
					goertzel_blocks(&cap, channel, tones[tone]);
				if (magnitude > next)
					next = magnitude;
			}
			CHECK(own > 3.0 * next,
			      "ac3: channel %d identity own %.0f next %.0f",
			      channel, own, next);
		}
	}
	printf("  ac3-dispatch   2 AUs -> 12 x 256, 3072 frames/channel identity\n");
	capture_free(&cap);
	free(wire);
}

/* ---- test 1: end-to-end DTS equivalence ---------------------------------- */

/* Feed `w`/`n` through a fresh pipeline in `chunk`-sized pushes, capturing
 * blocks. Returns the pipeline's final classification. */
static AiusPipelineClass run_pipeline(const uint16_t *w, size_t n, size_t chunk,
                                      struct capture *cap)
{
	AiusPcmSink sink = { .ctx = cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	if (aius_pipeline_create(&p, NULL, &sink) != AIUS_OK) {
		CHECK(0, "pipeline create failed");
		return AIUS_PIPELINE_UNKNOWN;
	}
	for (size_t off = 0; off < n; off += chunk) {
		size_t take = n - off;
		if (take > chunk)
			take = chunk;
		AiusStatus st = aius_pipeline_push(p, w + off, take);
		CHECK(st == AIUS_OK, "push status %d", st);
	}
	CHECK(aius_pipeline_finish(p) == AIUS_OK, "finish status");
	AiusPipelineClass klass = aius_pipeline_classification(p);
	aius_pipeline_destroy(p);
	return klass;
}

static void test_dts_equivalence(const char *dir, const char *name)
{
	size_t n = 0, au_len = 0;
	uint16_t *w = load_wire(dir, name, &n);
	uint8_t *au = load_pay(dir, name, &au_len);
	if (!w || !au) {
		CHECK(0, "%s: corpus load failed", name);
		free(w);
		free(au);
		return;
	}

	struct block_copy ref;
	CHECK(direct_decode(au, au_len, &ref) == 0, "%s: direct decode failed", name);

	/* Whole-buffer and one-word-at-a-time chunkings both reassemble the burst. */
	const size_t chunks[] = { n ? n : 1, 1 };
	for (size_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		AiusPipelineClass klass = run_pipeline(w, n, chunks[ci], &cap);
		CHECK(klass == AIUS_PIPELINE_DTS, "%s chunk=%zu: class %d != DTS", name,
		      chunks[ci], klass);
		CHECK(cap.n == 1, "%s chunk=%zu: emitted %d blocks != 1", name,
		      chunks[ci], cap.n);
		if (cap.n == 1)
			CHECK(blocks_equal(&cap.blocks[0], &ref),
			      "%s chunk=%zu: pipeline PCM != direct-decode PCM", name,
			      chunks[ci]);
		capture_free(&cap);
	}
	printf("  %-14s DTS end-to-end == direct decode (ch=%u frames=%u, 2 chunkings)\n",
	       name, ref.channels, ref.frames);

	for (int ch = 0; ch < AIUS_MAX_CHANNELS; ch++)
		free(ref.planes[ch]);
	free(w);
	free(au);
}

/* ---- test 2: PCM pass-through fidelity + classification ------------------ */

/* The pass-through's documented rule, restated for the ordering/framing check
 * below. Deliberately NOT reused by test 2b: a scale assertion written against
 * this expression proves only that the test and the producer agree, which is the
 * trap the old test fell into. */
static int32_t pcm_expected_sample(uint16_t word)
{
	const int32_t wire = (int32_t)(int16_t)word;
	return (int32_t)((uint32_t)wire << (AIUS_PCM_SAMPLE_BITS - 16));
}

/* Reassemble a capture's planes and compare them sample for sample against the
 * stream from word `from` onward; returns the recovered frame count and clears
 * *ok on the first mismatch. Every pass-through assertion in the suite is
 * measured this way -- from the confirmation boundary, not from word 0 -- so
 * `from` is what pins the emission's offset into the stream. */
static uint32_t passthrough_frames_from(const uint16_t *w, size_t from,
                                        const struct capture *cap, int *ok)
{
	uint32_t got = 0;
	for (int b = 0; b < cap->n && *ok; b++) {
		const struct block_copy *bc = &cap->blocks[b];
		for (uint32_t f = 0; f < bc->frames && *ok; f++) {
			if (bc->planes[0][f] !=
			            pcm_expected_sample(w[from + 2 * (got + f)]) ||
			    bc->planes[1][f] !=
			            pcm_expected_sample(w[from + 2 * (got + f) + 1]))
				*ok = 0;
		}
		got += bc->frames;
	}
	return got;
}

static void test_pcm_passthrough(void)
{
	/* Synthetic interleaved L,R 16-bit PCM, no IEC preamble, long enough to
	 * clear the confirmation squelch and emit four full blocks past it. The
	 * generator's stride has full period over 16 bits, so every word of the
	 * stream is distinct and the sample-for-sample comparison below pins the
	 * emission's OFFSET into the stream, not merely its content. */
	enum {
		CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS,
		FRAMES = 6144,
		WORDS = FRAMES * 2,
		EMIT_FRAMES = (WORDS - CONFIRM) / 2
	};
	uint16_t *w = malloc(WORDS * sizeof(uint16_t));
	for (int i = 0; i < WORDS; i++)
		w[i] = (uint16_t)(int16_t)((i * 131 + 7) & 0xFFFF);

	/* Whole-buffer and one-word-at-a-time. The 1-word chunking is the
	 * squelch-boundary alignment case: the confirm boundary is a per-RUN word
	 * offset and never a push offset, so the first emitted frame must still be
	 * the boundary word pair, L-first, when every push is a single word. */
	const size_t chunks[] = { WORDS, 1 };
	for (size_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		AiusPipelineClass klass = run_pipeline(w, WORDS, chunks[ci], &cap);
		CHECK(klass == AIUS_PIPELINE_PCM, "pcm chunk=%zu: class %d != PCM",
		      chunks[ci], klass);
		CHECK(cap.n > 0, "pcm chunk=%zu: no blocks emitted", chunks[ci]);
		if (cap.n > 0)
			CHECK(cap.blocks[0].planes[0][0] == pcm_expected_sample(w[CONFIRM]) &&
			      cap.blocks[0].planes[1][0] ==
			              pcm_expected_sample(w[CONFIRM + 1]),
			      "pcm chunk=%zu: first emitted frame is not the confirm "
			      "boundary word pair (run word %d, L-first)",
			      chunks[ci], CONFIRM);

		/* Block shape, then the samples themselves measured FROM the boundary
		 * word: everything before it was dropped. */
		int shape = 1;
		for (int b = 0; b < cap.n; b++) {
			struct block_copy *bc = &cap.blocks[b];
			if (bc->channels != 2 || bc->sample_rate_hz != 48000 ||
			    bc->channel_mask != (uint32_t)(AIUS_CH_FL | AIUS_CH_FR))
				shape = 0;
		}
		CHECK(shape, "pcm chunk=%zu: emitted block is not 2ch/48k/FL|FR",
		      chunks[ci]);
		int ok = 1;
		uint32_t got = passthrough_frames_from(w, CONFIRM, &cap, &ok);
		CHECK(ok, "pcm chunk=%zu: pass-through samples differ from input",
		      chunks[ci]);
		CHECK(got == EMIT_FRAMES,
		      "pcm chunk=%zu: recovered %u frames != %u -- the squelch must "
		      "drop exactly the first %d run words and no more",
		      chunks[ci], got, (unsigned)EMIT_FRAMES, CONFIRM);
		capture_free(&cap);
	}
	printf("  pcm            PCM pass-through 2ch/48k from run word %d, "
	       "%d frames byte-exact (2 chunkings)\n",
	       CONFIRM, EMIT_FRAMES);
	free(w);
}

static void test_pcm_runtime_rates(void)
{
	enum {
		WORDS = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS +
		        2 * (int)AIUS_PIPELINE_PCM_BLOCK_FRAMES
	};
	uint16_t *w = malloc(WORDS * sizeof(uint16_t));
	for (int i = 0; i < WORDS; i++)
		w[i] = (uint16_t)(int16_t)((i * 131 + 7) & 0xFFFF);

	const uint32_t rates[] = { 32000u, 44100u, 48000u };
	for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "pcm-rate %u: create", rates[r]);
		if (!p)
			continue;
		CHECK(aius_pipeline_set_pcm_sample_rate(p, rates[r]) == AIUS_OK,
		      "pcm-rate %u: setter rejected nominal", rates[r]);
		/* Runtime configuration survives stream-state resets. */
		aius_pipeline_reset(p);
		CHECK(aius_pipeline_push(p, w, WORDS) == AIUS_OK,
		      "pcm-rate %u: push", rates[r]);
		CHECK(aius_pipeline_finish(p) == AIUS_OK,
		      "pcm-rate %u: finish", rates[r]);
		CHECK(cap.n == 1 && cap.blocks[0].sample_rate_hz == rates[r],
		      "pcm-rate %u: descriptor count=%d rate=%u", rates[r], cap.n,
		      cap.n == 1 ? cap.blocks[0].sample_rate_hz : 0u);
		capture_free(&cap);
		aius_pipeline_destroy(p);
	}

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "pcm-rate invalid: create");
	if (p) {
		CHECK(aius_pipeline_set_pcm_sample_rate(p, 44100u) == AIUS_OK &&
		          aius_pipeline_set_pcm_sample_rate(p, 96000u) ==
		              AIUS_INVALID_ARG &&
		          aius_pipeline_set_pcm_sample_rate(NULL, 48000u) ==
		              AIUS_INVALID_ARG,
		      "pcm-rate invalid: acceptance");
		CHECK(aius_pipeline_push(p, w, WORDS) == AIUS_OK &&
		          aius_pipeline_finish(p) == AIUS_OK,
		      "pcm-rate invalid: push/finish");
		CHECK(cap.n == 1 && cap.blocks[0].sample_rate_hz == 44100u,
		      "pcm-rate invalid: rejected value changed configured nominal");
		aius_pipeline_destroy(p);
	}
	capture_free(&cap);
	free(w);
	printf("  pcm-rate       runtime 32/44.1/48 kHz stamps survive reset\n");
}

/* ---- test 2b: the pass-through's sample scale, in the contract's terms ------
 *
 * THE DISCRIMINATING TEST, and the reason it is separate from test 2. Test 2
 * compares the block against pcm_expected_sample(), which is the producer's rule
 * restated -- so it proves ordering, framing and channel split, and it CANNOT
 * catch a wrong scale, because both sides would move together. That is exactly
 * what happened: for the whole life of the pass-through, test 2 was green while
 * every PCM stream played 48.16 dB below every DTS stream.
 *
 * This test asserts the property a consumer actually depends on and that no
 * producer expression appears in: **a full-scale wire word reaches full scale.**
 * Nothing here refers to 16 bits, to a shift count, or to pcm_expected_sample. */
static void test_pcm_sample_scale(void)
{
	/* Both wire extremes and the two values that bracket zero, interleaved so
	 * each lands on a known plane. Long enough to classify as PCM unaided AND
	 * to clear the confirmation squelch with two full blocks to spare -- the
	 * plane split is itself a boundary-parity check, since an odd boundary
	 * would put the positive extreme on plane 0 and leave neg_peak at 0. */
	enum { FRAMES = 5120, WORDS = FRAMES * 2 };
	uint16_t *w = malloc(WORDS * sizeof(uint16_t));
	for (int f = 0; f < FRAMES; f++) {
		w[2 * f] = 0x8000u;     /* -32768: 16-bit negative full scale */
		w[2 * f + 1] = 0x7FFFu; /* +32767: 16-bit positive full scale */
	}

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPipelineClass klass = run_pipeline(w, WORDS, WORDS, &cap);
	CHECK(klass == AIUS_PIPELINE_PCM, "scale: class %d != PCM", klass);
	CHECK(cap.n > 0, "scale: no blocks emitted");

	int32_t neg_peak = 0, pos_peak = 0;
	for (int b = 0; b < cap.n; b++) {
		struct block_copy *bc = &cap.blocks[b];
		for (uint32_t f = 0; f < bc->frames; f++) {
			if (bc->planes[0][f] < neg_peak)
				neg_peak = bc->planes[0][f];
			if (bc->planes[1][f] > pos_peak)
				pos_peak = bc->planes[1][f];
		}
	}

	/* Negative full scale is EXACT. Two's complement's asymmetry works in our
	 * favour here: the most negative wire word left-aligns onto the most negative
	 * sample with no rounding at all. An == is therefore the right assertion, not
	 * a tolerance. */
	CHECK(neg_peak == AIUS_PCM_SAMPLE_MIN,
	      "scale: negative full-scale wire word reached %ld, not AIUS_PCM_SAMPLE_MIN "
	      "(%ld) -- pass-through PCM is %.2f dB off every other producer",
	      (long)neg_peak, (long)AIUS_PCM_SAMPLE_MIN,
	      20.0 * log10((double)-neg_peak / (double)-AIUS_PCM_SAMPLE_MIN));

	/* Positive full scale lands 255 LSBs short (0x7FFF00 against 0x7FFFFF), and
	 * that is CORRECT rather than tolerated: left-aligning zero-fills the new low
	 * bits, which is bit-exact and reversible. Scaling by MAX/32767 instead would
	 * hit the constant exactly and would introduce rounding error on every other
	 * sample in exchange -- a worse trade for a 0.00026 dB difference. So the
	 * bound is one wire LSB, tight enough to fail a missing or wrong-width
	 * alignment (the old behaviour was 255 dB^-1... 48.16 dB down, i.e. off by 2^8)
	 * and loose enough to permit the zero-fill. */
	const int32_t one_wire_lsb = (AIUS_PCM_SAMPLE_MAX + 1) / 32768;
	CHECK(pos_peak > AIUS_PCM_SAMPLE_MAX - one_wire_lsb,
	      "scale: positive full-scale wire word reached %ld, not within one wire LSB "
	      "(%ld) of AIUS_PCM_SAMPLE_MAX (%ld)",
	      (long)pos_peak, (long)one_wire_lsb, (long)AIUS_PCM_SAMPLE_MAX);

	printf("  pcm-scale      full-scale wire word -> full scale (%ld / %ld)\n",
	       (long)neg_peak, (long)pos_peak);

	capture_free(&cap);
	free(w);
}

/* ---- test 3: stateless classifier edges ---------------------------------- */

static void test_classifier(const char *dir)
{
	size_t n = 0;
	uint16_t *w = load_wire(dir, "dts_be16", &n);
	if (!w) {
		CHECK(0, "classifier: load dts_be16 failed");
		return;
	}
	CHECK(aius_pipeline_classify(w, n, 0) == AIUS_PIPELINE_DTS,
	      "classify(dts) != DTS");

	/* Minimal preamble pair, big-endian and byte-swapped little-endian. */
	const uint16_t be[] = { 0xF872u, 0x4E1Fu };
	const uint16_t le[] = { 0x72F8u, 0x1F4Eu };
	CHECK(aius_pipeline_classify(be, 2, 0) == AIUS_PIPELINE_DTS, "classify(BE) != DTS");
	CHECK(aius_pipeline_classify(le, 2, 0) == AIUS_PIPELINE_DTS, "classify(LE) != DTS");

	/* Linear-PCM ramp, longer than the window -> PCM; a short prefix -> UNKNOWN. */
	uint16_t pcm[600];
	for (int i = 0; i < 600; i++)
		pcm[i] = (uint16_t)(int16_t)(i * 137 + 3);
	CHECK(aius_pipeline_classify(pcm, 600, 512) == AIUS_PIPELINE_PCM,
	      "classify(pcm,600,512) != PCM");
	CHECK(aius_pipeline_classify(pcm, 100, 512) == AIUS_PIPELINE_UNKNOWN,
	      "classify(pcm,100,512) != UNKNOWN");
	/* A lone Pa with no following Pb is not a preamble pair. */
	const uint16_t lone[] = { 0xF872u, 0x0000u };
	CHECK(aius_pipeline_classify(lone, 2, 4) == AIUS_PIPELINE_UNKNOWN,
	      "classify(lone Pa) != UNKNOWN");
	printf("  classifier     DTS/PCM/UNKNOWN + BE/LE preamble edges\n");
	free(w);
}

/* ---- test 4: no-heap steady state via the arena-backed pipeline ---------- */

struct increment_cycle_clock {
	uint32_t now;
	uint32_t step;
	uint32_t calls;
};

static uint32_t increment_cycle_read(void *ctx)
{
	struct increment_cycle_clock *clock = ctx;
	uint32_t value = clock->now;
	clock->now += clock->step;
	clock->calls++;
	return value;
}

/* Arena dedicated to the pipeline instance + DTS decoder. 5.1 core decode
 * high-water measures well under 256 KiB; the instance adds a few KiB. */
#define PIPE_ARENA_BYTES (256u * 1024u)
static uint8_t g_pipe_arena[PIPE_ARENA_BYTES] __attribute__((aligned(16)));

static void test_no_heap_steady_state(const char *dir)
{
	size_t n = 0;
	uint16_t *w = load_wire(dir, "dts_be16", &n);
	size_t ac3_n = 0u;
	uint16_t *ac3 = load_wire(dir, "ac3_surround51_ident", &ac3_n);
	if (!w || !ac3) {
		CHECK(0, "steady-state: corpus load failed");
		free(ac3);
		free(w);
		return;
	}

	/* One arena-backed pipeline decodes two identical bursts in sequence. On the
	 * arena path alloc_count reflects real allocations: the first (warmup) burst
	 * allocates, the second (steady state) allocates zero. */
	AiusArena arena;
	CHECK(aius_arena_init(&arena, g_pipe_arena, sizeof(g_pipe_arena)) == 0,
	      "ss: arena init");

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	struct increment_cycle_clock fake_clock = {
		.now = UINT32_MAX - 20u,
		.step = 19u,
	};
	AiusPipelineConfig config = {
		.packing = AIUS_IEC61937_PACK_16,
		.cycle_clock = {
			.ctx = &fake_clock,
			.read = increment_cycle_read,
		},
	};
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create_arena(&p, &config, &sink, &arena) == AIUS_OK,
	      "ss: create_arena");
	if (!p) {
		free(w);
		return;
	}

	AiusMetrics m;
	AiusMetrics dts_m;
	AiusMetrics ac3_m;

	CHECK(aius_pipeline_codec_pc(p) == 0u, "ss: initial codec Pc is nonzero");
	CHECK(aius_pipeline_resident_codec(p) == 0u,
	      "ss: a decoder is resident before any burst");
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK &&
	          m.decode_cycles == 0u && m.stack_high_water == 0u &&
	          m.alloc_count == 0u && m.arena_high_water == 0u &&
	          m.last_status == AIUS_OK,
	      "ss: metrics before any residency are not all-zero");
	CHECK(aius_pipeline_get_codec_metrics(p, 22u, &m) == AIUS_INVALID_ARG,
	      "ss: unsupported codec metrics Pc accepted");
	CHECK(aius_pipeline_get_codec_metrics(p, 21u, &m) == AIUS_OK &&
	          m.decode_cycles == 0u && m.alloc_count == 0u &&
	          m.arena_high_water == 0u,
	      "ss: non-resident E-AC-3 metrics are not all-zero");

	/* Warmup burst: allocates (proves the arena + alloc shim are wired). */
	CHECK(aius_pipeline_push(p, w, n) == AIUS_OK, "ss: warmup push");
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK, "ss: warmup metrics");
	CHECK(m.alloc_count > 0, "ss: warmup allocated 0 (arena/shim not wired?)");
	CHECK(m.decode_cycles == fake_clock.step,
	      "ss: DTS warmup cycles=%" PRIu64 " expected=%u",
	      m.decode_cycles, fake_clock.step);
	printf("  steady-state   warmup alloc_count=%u arena_high_water=%u bytes\n",
	       m.alloc_count, m.arena_high_water);

	/* Steady-state burst: zero allocations. */
	CHECK(aius_pipeline_push(p, w, n) == AIUS_OK, "ss: steady push");
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK, "ss: steady metrics");
	CHECK(m.alloc_count == 0,
	      "ss: steady-state decode allocated %u (must be 0)", m.alloc_count);
	CHECK(cap.n == 2, "ss: emitted %d blocks != 2", cap.n);
	CHECK(aius_pipeline_codec_pc(p) == 13u ||
	          aius_pipeline_codec_pc(p) == 12u ||
	          aius_pipeline_codec_pc(p) == 11u,
	      "ss: DTS codec Pc %u", aius_pipeline_codec_pc(p));
	CHECK(aius_pipeline_get_codec_metrics(p, aius_pipeline_codec_pc(p),
	                                      &dts_m) == AIUS_OK &&
	          dts_m.alloc_count == 0u &&
	          dts_m.decode_cycles == fake_clock.step,
	      "ss: separately-addressed DTS metrics");
	CHECK(aius_pipeline_resident_codec(p) == 11u,
	      "ss: DTS residency %u != 11", aius_pipeline_resident_codec(p));

	CHECK(aius_pipeline_push(p, ac3, ac3_n) == AIUS_OK, "ss: AC-3 push");
	CHECK(aius_pipeline_codec_pc(p) == 1u, "ss: AC-3 codec Pc not selected");
	CHECK(aius_pipeline_resident_codec(p) == 1u,
	      "ss: AC-3 residency %u != 1", aius_pipeline_resident_codec(p));
	{
		/* The family change swapped the slot: one swap counted, and the
		 * departed DTS decoder's metrics read zero (its state left with
		 * the slot). */
		AiusPipelineServiceDiag sd;
		AiusMetrics gone;
		aius_pipeline_get_service_diag(p, &sd);
		CHECK(sd.codec_swaps == 1u, "ss: codec_swaps %u != 1",
		      sd.codec_swaps);
		CHECK(aius_pipeline_get_codec_metrics(p, 13u, &gone) == AIUS_OK &&
		          gone.decode_cycles == 0u && gone.alloc_count == 0u &&
		          gone.arena_high_water == 0u,
		      "ss: departed DTS metrics are not zero");
	}
	CHECK(aius_pipeline_get_codec_metrics(p, 1u, &ac3_m) == AIUS_OK &&
	          ac3_m.alloc_count == 0u &&
	          ac3_m.decode_cycles == 7u * (uint64_t)fake_clock.step,
	      "ss: AC-3 steady-state decode allocated");
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK &&
	          m.alloc_count == ac3_m.alloc_count &&
	          m.decode_cycles == ac3_m.decode_cycles &&
	          m.arena_high_water >= dts_m.arena_high_water &&
	          m.arena_high_water >= ac3_m.arena_high_water,
	      "ss: compatibility/per-codec metric projection");
	CHECK(dts_m.decode_cycles == fake_clock.step &&
	          ac3_m.decode_cycles == 7u * (uint64_t)fake_clock.step &&
	          fake_clock.calls == 32u,
	      "ss: per-codec timing projections/calls dts=%" PRIu64
	      " ac3=%" PRIu64 " calls=%u",
	      dts_m.decode_cycles, ac3_m.decode_cycles, fake_clock.calls);
	printf("  steady-state   2nd burst alloc_count=0 (no-heap steady state)\n");

	aius_pipeline_destroy(p);
	capture_free(&cap);
	free(ac3);
	free(w);
}

/* ---- codec-slot swap oracles ----------------------------------------------
 *
 * The slot discipline's whole risk is state bleeding across a swap: a decoder
 * created into storage another decoder just vacated must decode EXACTLY like
 * one created into fresh memory, every time, under any family sequence. Two
 * phases over one arena-backed pipeline: a deterministic 8-cycle DTS/AC-3
 * alternation, then a seeded-xorshift fuzz over the family sequence (repeats
 * included, which exercise the no-swap path between swaps). Every push's
 * emitted blocks must be bit-identical to a fresh arena pipeline's decode of
 * the same vector, and the slot's cumulative high-water must respect
 * AIUS_PIPELINE_CODEC_SLOT_BYTES -- the capacity oracle that makes
 * create-into-slot infallible on the target. */

static int captures_equal(const struct capture *got, const struct capture *ref)
{
	if (got->n != ref->n || got->overflow)
		return 0;
	for (int i = 0; i < got->n; i++)
		if (!blocks_equal(&got->blocks[i], &ref->blocks[i]))
			return 0;
	return 1;
}

/* Decode one wire vector through a fresh arena-backed pipeline into *ref.
 * Returns 0 on success. Reuses g_pipe_arena; callers sequence pipelines. */
static int slot_reference_decode(const uint16_t *w, size_t n, struct capture *ref)
{
	AiusArena arena;
	if (aius_arena_init(&arena, g_pipe_arena, sizeof(g_pipe_arena)) != 0)
		return -1;
	memset(ref, 0, sizeof(*ref));
	AiusPcmSink sink = { .ctx = ref, .consume = capture_consume };
	AiusPipeline *p = NULL;
	if (aius_pipeline_create_arena(&p, NULL, &sink, &arena) != AIUS_OK)
		return -1;
	AiusStatus st = aius_pipeline_push(p, w, n);
	aius_pipeline_destroy(p);
	return (st == AIUS_OK && ref->n > 0) ? 0 : -1;
}

static void test_codec_slot_swap(const char *dir)
{
	size_t wire_n[3] = { 0u, 0u, 0u };
	uint16_t *wire[3];
	wire[0] = load_wire(dir, "dts51_be16", &wire_n[0]);
	wire[1] = load_wire(dir, "ac3_surround51_ident", &wire_n[1]);
	wire[2] = load_wire(dir, "eac3_surround51_ident", &wire_n[2]);
	if (!wire[0] || !wire[1] || !wire[2]) {
		CHECK(0, "slot-swap: corpus load failed");
		for (int f = 0; f < 3; f++)
			free(wire[f]);
		return;
	}
	static const uint16_t FAMILY[3] = { 11u, 1u, 21u };

	/* References: the tallest committed vector of each family, decoded by a
	 * fresh arena pipeline (first residency, virgin slot). */
	struct capture ref[3];
	for (int f = 0; f < 3; f++)
		CHECK(slot_reference_decode(wire[f], wire_n[f], &ref[f]) == 0,
		      "slot-swap: family %u reference decode", FAMILY[f]);

	AiusArena arena;
	CHECK(aius_arena_init(&arena, g_pipe_arena, sizeof(g_pipe_arena)) == 0,
	      "slot-swap: churn arena init");
	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create_arena(&p, NULL, &sink, &arena) == AIUS_OK,
	      "slot-swap: churn create");
	if (!p) {
		for (int f = 0; f < 3; f++) {
			capture_free(&ref[f]);
			free(wire[f]);
		}
		return;
	}

	uint32_t expected_swaps = 0u;
	uint16_t resident = 0u;
	int identity_ok = 1;

	/* Phase 1: strict three-way rotation -- every push but the first swaps,
	 * and every residency-opening decode must be bit-identical to a fresh
	 * arena pipeline's. */
	for (int cycle = 0; cycle < 18 && identity_ok; cycle++) {
		const int f = cycle % 3;
		if (resident != 0u && resident != FAMILY[f])
			expected_swaps++;
		resident = FAMILY[f];
		identity_ok =
			aius_pipeline_push(p, wire[f], wire_n[f]) == AIUS_OK &&
			aius_pipeline_resident_codec(p) == FAMILY[f] &&
			captures_equal(&cap, &ref[f]);
		capture_free(&cap);
	}
	CHECK(identity_ok, "slot-swap: rotation identity broke");

	/* Phase 2: fuzz the family sequence (deterministic xorshift32). Repeats
	 * hit the same-family fast path between swaps. Fresh-decode identity is
	 * only the contract for the push that BEGINS a residency: within one, the
	 * decoder's synthesis/overlap history legitimately carries across AUs, so
	 * a repeat push checks structure (block count/shape), not sample bits. */
	uint32_t rng = 0x1dbabe5u;
	for (int i = 0; i < 48 && identity_ok; i++) {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		const int f = (int)(rng % 3u);
		const int fresh = resident != FAMILY[f];
		if (resident != 0u && fresh)
			expected_swaps++;
		resident = FAMILY[f];
		identity_ok =
			aius_pipeline_push(p, wire[f], wire_n[f]) == AIUS_OK &&
			aius_pipeline_resident_codec(p) == FAMILY[f] &&
			(fresh ? captures_equal(&cap, &ref[f])
			       : cap.n == ref[f].n && !cap.overflow);
		capture_free(&cap);
	}
	CHECK(identity_ok, "slot-swap: fuzz identity broke");

	AiusPipelineServiceDiag sd;
	aius_pipeline_get_service_diag(p, &sd);
	CHECK(sd.codec_swaps == expected_swaps, "slot-swap: swaps %u != %u",
	      sd.codec_swaps, expected_swaps);
	CHECK(sd.decode_rejects == 0u,
	      "slot-swap: %u decode rejects (slot create failed?)",
	      sd.decode_rejects);

	/* Capacity oracle: the cumulative slot high-water across every residency
	 * of all THREE families' tallest vectors fits the committed slot size
	 * (E-AC-3 is the tallest: 179,232 measured against the 180,224 slot --
	 * a 992 B margin, deterministic because the slot is reset before every
	 * create). */
	AiusMetrics m;
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK &&
	          m.arena_high_water > 0u &&
	          m.arena_high_water <= AIUS_PIPELINE_CODEC_SLOT_BYTES,
	      "slot-swap: slot high-water %u vs capacity %u",
	      m.arena_high_water, AIUS_PIPELINE_CODEC_SLOT_BYTES);
	printf("  slot-swap      66 pushes over 3 families, %u swaps, "
	       "bit-identical; slot high-water %u/%u (margin %u)\n",
	       sd.codec_swaps, m.arena_high_water, AIUS_PIPELINE_CODEC_SLOT_BYTES,
	       AIUS_PIPELINE_CODEC_SLOT_BYTES - m.arena_high_water);

	aius_pipeline_destroy(p);
	capture_free(&cap);
	for (int f = 0; f < 3; f++) {
		capture_free(&ref[f]);
		free(wire[f]);
	}
}


/* ---- E-AC-3 dispatch identity + arena-slot refusal (phase 3) -------------- */

/* Wire -> pipeline -> sink must be bit-identical to the SAME two access units
 * decoded through one adapter-owned decoder in wire order (overlap-add
 * history carries across AUs, so AU 2's reference depends on AU 1 having
 * decoded first). The reference decoder is destroyed before the pipeline's
 * decoder is created: the fork's framework shim re-arms its process-global
 * output provider per decode, so serialized decoders are safe and sequenced
 * ones doubly so. Heap path -- the slot bound is the next test's subject. */
static void test_eac3_dispatch_and_identity(const char *dir)
{
	size_t n = 0u, plen = 0u;
	uint16_t *wire = load_wire(dir, "eac3_surround51_ident", &n);
	uint8_t *pay = load_pay(dir, "eac3_surround51_ident", &plen);
	CHECK(wire && pay, "eac3: corpus load");
	if (!wire || !pay) {
		free(wire);
		free(pay);
		return;
	}

	int32_t *ref[2][AIUS_MAX_CHANNELS];
	memset(ref, 0, sizeof(ref));
	uint32_t ref_frames = 0u;
	{
		AiusEac3Decoder *dec = NULL;
		CHECK(aius_eac3_decoder_create(&dec) == AIUS_OK, "eac3: ref create");
		for (int au = 0; au < 2 && dec; au++) {
			AiusPcmBlock blk;
			CHECK(aius_eac3_decode_access_unit(dec, pay, plen, &blk) ==
			              AIUS_OK,
			      "eac3: reference decode AU %d", au);
			ref_frames = blk.frames;
			for (int ch = 0;
			     ch < blk.channels && ch < AIUS_MAX_CHANNELS; ch++) {
				ref[au][ch] =
					malloc((size_t)blk.frames * sizeof(int32_t));
				memcpy(ref[au][ch], blk.planes[ch],
				       (size_t)blk.frames * sizeof(int32_t));
			}
		}
		aius_eac3_decoder_destroy(dec);
	}
	CHECK(ref_frames == 1536u, "eac3: reference AU frames %u != 1536",
	      ref_frames);

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK, "eac3: create");
	push_chunked(p, wire, n, 256, "eac3");
	CHECK(aius_pipeline_finish(p) == AIUS_OK, "eac3: finish");
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_DTS,
	      "eac3: class != bitstream");
	CHECK(aius_pipeline_codec_pc(p) == 21u &&
	              aius_pipeline_resident_codec(p) == 21u,
	      "eac3: codec pc %u resident %u", aius_pipeline_codec_pc(p),
	      aius_pipeline_resident_codec(p));
	AiusPipelineServiceDiag sd;
	aius_pipeline_get_service_diag(p, &sd);
	CHECK(sd.decode_units == 2u && sd.decode_rejects == 0u &&
	              sd.codec_swaps == 0u && sd.au_abandons == 0u,
	      "eac3: service diag (%u units, %u rejects, %u swaps, %u abandons)",
	      sd.decode_units, sd.decode_rejects, sd.codec_swaps,
	      sd.au_abandons);
	aius_pipeline_destroy(p);

	const uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
	                      AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR;
	CHECK(cap.n == 6, "eac3: %d blocks != 6 (two AUs x 3 slices)", cap.n);
	int identical = cap.n == 6;
	for (int b = 0; b < cap.n && identical; b++) {
		const struct block_copy *bc = &cap.blocks[b];
		if (bc->channels != 6u || bc->frames != 512u ||
		    bc->sample_rate_hz != 48000u || bc->channel_mask != mask) {
			CHECK(0, "eac3: block %d descriptor (%u,%u,%u,%08x)", b,
			      bc->channels, bc->frames, bc->sample_rate_hz,
			      bc->channel_mask);
			identical = 0;
			break;
		}
		const int au = b / 3;
		const uint32_t off = (uint32_t)(b % 3) * 512u;
		for (int ch = 0; ch < 6; ch++)
			if (!ref[au][ch] ||
			    memcmp(bc->planes[ch], ref[au][ch] + off,
			           512u * sizeof(int32_t)) != 0)
				identical = 0;
	}
	CHECK(identical, "eac3: pipeline output != direct adapter decode");
	printf("  eac3-dispatch  2 AUs -> 6 x 512, bit-identical to the direct "
	       "decode\n");

	capture_free(&cap);
	for (int au = 0; au < 2; au++)
		for (int ch = 0; ch < AIUS_MAX_CHANNELS; ch++)
			free(ref[au][ch]);
	free(wire);
	free(pay);
}

/* The arena/slot half, since the phase-4 slot growth: E-AC-3 is a full
 * slot citizen. A type-21 burst on a live DTS link SWAPS the slot (one
 * counted swap; the DTS state legitimately leaves with it), decodes on the
 * arena path bit-identically to a fresh arena pipeline, and the cumulative
 * slot high-water -- now covering the tallest of the three families --
 * respects the committed slot size. */
static void test_eac3_arena_residency(const char *dir)
{
	size_t dts_n = 0u, eac3_n = 0u;
	uint16_t *dts = load_wire(dir, "dts51_be16", &dts_n);
	uint16_t *eac3 = load_wire(dir, "eac3_surround51_ident", &eac3_n);
	CHECK(dts && eac3, "eac3-slot: corpus load");
	if (!dts || !eac3) {
		free(dts);
		free(eac3);
		return;
	}

	/* Reference: the eac3 wire through a fresh arena pipeline (first
	 * residency, virgin slot). */
	struct capture ref;
	CHECK(slot_reference_decode(eac3, eac3_n, &ref) == 0,
	      "eac3-slot: reference decode");

	AiusArena arena;
	CHECK(aius_arena_init(&arena, g_pipe_arena, sizeof(g_pipe_arena)) == 0,
	      "eac3-slot: arena init");
	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create_arena(&p, NULL, &sink, &arena) == AIUS_OK,
	      "eac3-slot: create");
	if (!p) {
		capture_free(&ref);
		free(dts);
		free(eac3);
		return;
	}

	CHECK(aius_pipeline_push(p, dts, dts_n) == AIUS_OK,
	      "eac3-slot: DTS warmup");
	CHECK(aius_pipeline_resident_codec(p) == 11u,
	      "eac3-slot: DTS not resident");
	capture_free(&cap);

	CHECK(aius_pipeline_push(p, eac3, eac3_n) == AIUS_OK,
	      "eac3-slot: type-21 push");
	AiusPipelineServiceDiag sd;
	aius_pipeline_get_service_diag(p, &sd);
	CHECK(sd.codec_swaps == 1u, "eac3-slot: swaps %u != 1", sd.codec_swaps);
	CHECK(sd.decode_rejects == 0u, "eac3-slot: %u decode rejects",
	      sd.decode_rejects);
	CHECK(aius_pipeline_resident_codec(p) == 21u &&
	              aius_pipeline_codec_pc(p) == 21u,
	      "eac3-slot: residency %u codec_pc %u after the swap",
	      aius_pipeline_resident_codec(p), aius_pipeline_codec_pc(p));
	CHECK(captures_equal(&cap, &ref),
	      "eac3-slot: swap-in decode differs from a fresh arena pipeline's");

	/* Capacity oracle across the residencies so far -- E-AC-3 is the
	 * tallest, and the committed slot must hold its cumulative
	 * high-water. */
	AiusMetrics m;
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK &&
	              m.arena_high_water > 0u &&
	              m.arena_high_water <= AIUS_PIPELINE_CODEC_SLOT_BYTES,
	      "eac3-slot: slot high-water %u vs capacity %u",
	      m.arena_high_water, AIUS_PIPELINE_CODEC_SLOT_BYTES);

	/* And back: DTS re-creates into the reclaimed slot. A fresh warmup
	 * (non-zero alloc_count) is the contract here -- its state left with
	 * the swap -- so only the swap count and the decode are asserted. */
	capture_free(&cap);
	CHECK(aius_pipeline_push(p, dts, dts_n) == AIUS_OK,
	      "eac3-slot: DTS resumes");
	aius_pipeline_get_service_diag(p, &sd);
	CHECK(sd.codec_swaps == 2u && cap.n > 0,
	      "eac3-slot: swap-back (%u swaps, %d blocks)", sd.codec_swaps,
	      cap.n);
	printf("  eac3-slot      E-AC-3 resident on the arena slot: swap-in "
	       "bit-identical, high-water %u/%u (margin %u), swap-back clean\n",
	       m.arena_high_water, AIUS_PIPELINE_CODEC_SLOT_BYTES,
	       AIUS_PIPELINE_CODEC_SLOT_BYTES - m.arena_high_water);

	aius_pipeline_destroy(p);
	capture_free(&cap);
	capture_free(&ref);
	free(dts);
	free(eac3);
}

/* ---- defect-28: mid-stream re-classification ------------------------------
 *
 * The live-lottery repro (a carrier locked at random burst phase) and the
 * mid-stream format-switch family. Streams are built by repeating committed
 * corpus vectors: dts_be16/dts51_be16 are 946 words, Pa at word 0, zero tail,
 * so N-fold repeats put preamble pairs at exact 946-word spacing -- inside
 * the plausible re-type range. All long streams push in 256-word chunks (the
 * target's push size): starvation is evaluated at push boundaries, so
 * whole-buffer pushes would legitimately defer it past the stream's end. */

static const uint16_t RAMP_PA = 0xF872u;
static const uint16_t RAMP_PB = 0x4E1Fu;

static uint16_t *repeat_wire(const uint16_t *v, size_t n, size_t reps,
                             size_t *out_n)
{
	uint16_t *s = malloc(n * reps * sizeof(uint16_t));
	for (size_t r = 0; r < reps; r++)
		memcpy(s + r * n, v, n * sizeof(uint16_t));
	*out_n = n * reps;
	return s;
}

/* Linear ramp that cannot form a preamble pair by construction: consecutive
 * words always differ by 131, and neither wire form's Pb - Pa equals 131. */
static uint16_t *make_ramp(size_t n)
{
	uint16_t *w = malloc(n * sizeof(uint16_t));
	for (size_t i = 0; i < n; i++)
		w[i] = (uint16_t)((i * 131 + 7) & 0xFFFF);
	return w;
}

/* Plant a four-word preamble at `at` -- Pa, Pb, and a burst_info naming a
 * DECODABLE data type (DTS type III), which the defect-34 single-header rule
 * deliberately ignores.
 *
 * The word after Pb stopped being inert when a well-formed out-of-set burst
 * header became a single-pair exit from PCM. Every leg below plants its pairs
 * into a ramp and used to inherit whatever ramp value followed; two of those
 * inherited values (a type-21 header at fp-far-pairs, a type-31 one at
 * reset-sem) happen to be valid single-pair-exit headers, so the legs would
 * have been testing that rule instead of the two-pair rule they are named for.
 * Their assertions are unchanged -- only the stimulus is now fully stated. That
 * two arbitrary ramp words landed on valid headers is itself worth knowing: it
 * is why the single-header rule is confined to the types the two-pair rule
 * cannot reach, where acting wrongly costs silence rather than a flipped live
 * stream. */
static void plant_pair(uint16_t *w, size_t at, int le)
{
	if (le) {
		w[at] = 0x72F8u;     /* Pa, byte-swapped wire form */
		w[at + 1] = 0x1F4Eu; /* Pb */
		w[at + 2] = 0x0D00u; /* Pc: data type 13, error flag clear */
		w[at + 3] = 0x0010u; /* Pd: 4096 bits */
	} else {
		w[at] = RAMP_PA;
		w[at + 1] = RAMP_PB;
		w[at + 2] = 0x000Du;
		w[at + 3] = 0x1000u;
	}
}

static void push_chunked(AiusPipeline *p, const uint16_t *w, size_t n,
                         size_t chunk, const char *label)
{
	for (size_t off = 0; off < n; off += chunk) {
		size_t take = n - off;
		if (take > chunk)
			take = chunk;
		AiusStatus st = aius_pipeline_push(p, w + off, take);
		CHECK(st == AIUS_OK, "%s: push status %d at word %zu", label, st, off);
	}
}

/* Everything the re-classification tests read back off a pipeline before it is
 * destroyed: the two lifetime re-class counters, plus the confirmation
 * squelch's lifetime dropped-frame count and its live gate state. */
struct reclass_diag {
	uint32_t retype;
	uint32_t reclassify;
	uint32_t squelched_frames;
	int pcm_confirmed;
};

/* Full create/push/finish/counter-read runner for the re-classification
 * tests. `d` receives the diagnostics read BEFORE destroy. Returns the final
 * classification. */
static AiusPipelineClass run_reclass(const uint16_t *w, size_t n, size_t chunk,
                                     struct capture *cap, struct reclass_diag *d,
                                     const char *label)
{
	AiusPcmSink sink = { .ctx = cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	memset(d, 0, sizeof(*d));
	if (aius_pipeline_create(&p, NULL, &sink) != AIUS_OK) {
		CHECK(0, "%s: pipeline create failed", label);
		return AIUS_PIPELINE_UNKNOWN;
	}
	push_chunked(p, w, n, chunk, label);
	CHECK(aius_pipeline_finish(p) == AIUS_OK, "%s: finish status", label);
	CHECK(aius_pipeline_get_reclass_counters(p, &d->retype, &d->reclassify) ==
	              AIUS_OK,
	      "%s: counter read", label);
	CHECK(aius_pipeline_get_squelch_diag(p, &d->squelched_frames,
	                                     &d->pcm_confirmed) == AIUS_OK,
	      "%s: squelch diag read", label);
	AiusPipelineClass klass = aius_pipeline_classification(p);
	aius_pipeline_destroy(p);
	return klass;
}

/* T1: the live lottery. Entry at word offset 4 puts the first visible pair at
 * pushed position 942 -- outside the 512-word classify window -- so today the
 * stream types PCM and stays there. The fix must re-type on the second pair
 * (spacing 946) and decode every burst from there on. */
static void test_retype_mid_burst_entry(const char *dir)
{
	size_t n = 0, sn = 0;
	uint16_t *v = load_wire(dir, "dts_be16", &n);
	if (!v) {
		CHECK(0, "retype-entry: corpus load failed");
		return;
	}
	CHECK(n == 946, "retype-entry: dts_be16 is %zu words, expected 946", n);
	uint16_t *s = repeat_wire(v, n, 8, &sn);

	/* 256-word chunks (the target shape) and 1-word chunks (worst-case
	 * boundary carry for the pair detector). */
	const size_t chunks[] = { 256, 1 };
	for (size_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipelineClass klass = run_reclass(s + 4, sn - 4, chunks[ci], &cap,
		                                      &d, "retype-entry");
		CHECK(klass == AIUS_PIPELINE_DTS,
		      "retype-entry chunk=%zu: class %d != DTS (the defect-28 lottery)",
		      chunks[ci], klass);
		CHECK(d.retype == 1, "retype-entry chunk=%zu: retype_count %u != 1",
		      chunks[ci], d.retype);
		CHECK(d.reclassify == 0, "retype-entry chunk=%zu: reclassify_count %u != 0",
		      chunks[ci], d.reclassify);
		/* NO PCM block: the whole mis-typed run (words 0..1888, 1889 words)
		 * sits inside the unconfirmed window, so it is dropped rather than
		 * played -- this is the periodic-buzz seam the squelch exists for.
		 * Bursts 3..8 decoded = 6 blocks. */
		CHECK(cap.n == 6, "retype-entry chunk=%zu: %d blocks != 6", chunks[ci],
		      cap.n);
		/* 1889 dropped words -> 944 frames; the trailing half-word (word 1888
		 * with no partner) is not counted. */
		CHECK(d.squelched_frames == 944,
		      "retype-entry chunk=%zu: squelched_frames %u != 944", chunks[ci],
		      d.squelched_frames);
		capture_free(&cap);
	}
	printf("  retype-entry   mid-burst entry re-types to DTS (2 chunkings)\n");
	free(s);
	free(v);
}

/* T2: a genuine mid-stream PCM->DTS switch, with the geometry chosen so both
 * halves of the claim are observable. The ramp is 8192 + 3 x 1024 words: the
 * confirmation window, then three whole blocks, so the confirmed PCM segment
 * demonstrably PLAYS (byte-exact from the boundary word) and the switch lands
 * at staged phase 0. Then 4 x dts51_be16: pair spacing is 946 words, under one
 * block, so the burst tail staged as PCM never reaches a block boundary before
 * the re-type drops it -- no burst data reaches the sink -- and bursts 2..4
 * decode as 6-channel blocks.
 *
 * CAVEAT, deliberate: this pins drop-of-the-staged-tail at a BLOCK-ALIGNED
 * switch, not a universal no-buzz property. A switch landing at staged phase
 * 511 can still emit one mixed block out of a CONFIRMED PCM class -- by design
 * and accepted ("burst noise is more acceptable than extra latency"). What the
 * squelch closes is the other, common seam: burst data reaching the sink from a
 * CLASSIFICATION window. */
static void test_retype_mid_stream_switch(const char *dir)
{
	size_t n = 0, bn = 0;
	uint16_t *v = load_wire(dir, "dts51_be16", &n);
	if (!v) {
		CHECK(0, "retype-switch: corpus load failed");
		return;
	}
	uint16_t *bursts = repeat_wire(v, n, 4, &bn);
	enum {
		CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS,
		BLOCK_WORDS = 2 * (int)AIUS_PIPELINE_PCM_BLOCK_FRAMES,
		RAMP_WORDS = CONFIRM + 3 * BLOCK_WORDS
	};
	size_t sn = RAMP_WORDS + bn;
	uint16_t *s = malloc(sn * sizeof(uint16_t));
	uint16_t *ramp = make_ramp(RAMP_WORDS);
	memcpy(s, ramp, RAMP_WORDS * sizeof(uint16_t));
	memcpy(s + RAMP_WORDS, bursts, bn * sizeof(uint16_t));

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	struct reclass_diag d;
	AiusPipelineClass klass = run_reclass(s, sn, 256, &cap, &d, "retype-switch");
	CHECK(klass == AIUS_PIPELINE_DTS, "retype-switch: class %d != DTS", klass);
	CHECK(d.retype == 1, "retype-switch: retype_count %u != 1", d.retype);
	int pcm_blocks = 0, dts_blocks = 0, first_dts = -1;
	for (int b = 0; b < cap.n; b++) {
		if (cap.blocks[b].channels == 6) {
			if (first_dts < 0)
				first_dts = b;
			dts_blocks++;
		} else if (cap.blocks[b].channels == 2) {
			pcm_blocks++;
		}
	}
	CHECK(pcm_blocks == 3,
	      "retype-switch: %d PCM blocks != 3 (the confirmed ramp segment, and "
	      "nothing at all from the burst tail)", pcm_blocks);
	CHECK(first_dts == 3,
	      "retype-switch: first decoded block at index %d != 3 -- a PCM block "
	      "reached the sink after the switch", first_dts);
	CHECK(dts_blocks >= 3, "retype-switch: %d decoded 5.1 blocks < 3",
	      dts_blocks);
	CHECK(cap.n > 0 &&
	              cap.blocks[0].planes[0][0] == pcm_expected_sample(s[CONFIRM]),
	      "retype-switch: the first PCM frame is not the confirm boundary word");
	CHECK(d.squelched_frames == (uint32_t)CONFIRM / 2u,
	      "retype-switch: squelched_frames %u != %u", d.squelched_frames,
	      (unsigned)(CONFIRM / 2));
	printf("  retype-switch  confirmed PCM plays 3 blocks, the switch drops the "
	       "staged burst tail, 3 x 5.1 decoded\n");
	capture_free(&cap);
	free(ramp);
	free(bursts);
	free(s);
	free(v);
}

/* T3: false-positive immunity. All embedded pairs sit AFTER the classify
 * window so the stream types PCM first and the in-flight detector (not the
 * classifier) is what is exercised. */
static void test_retype_false_positive_immunity(void)
{
	/* (a) one pair only, AFTER the confirmation boundary: stays PCM and the
	 * pass-through is byte-exact from the boundary word, the pair words
	 * included. The candidate arms and later expires (the stream runs more than
	 * one MAX spacing past it) with no effect on emission at all. The length is
	 * deliberately not block-aligned -- 4904 emitted frames is 9 blocks plus
	 * 296 -- so finish()'s partial flush runs from a CONFIRMED class, which is
	 * this suite's only coverage of that flush. */
	{
		enum {
			CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS,
			WORDS = 18000,
			PAIR = 9000,
			EMIT_FRAMES = (WORDS - CONFIRM) / 2
		};
		uint16_t *w = make_ramp(WORDS);
		plant_pair(w, PAIR, 0);
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipelineClass klass = run_reclass(w, WORDS, 256, &cap, &d,
		                                      "fp-one-pair");
		CHECK(klass == AIUS_PIPELINE_PCM, "fp-one-pair: class %d != PCM", klass);
		CHECK(d.retype == 0, "fp-one-pair: retype_count %u != 0", d.retype);
		CHECK(d.pcm_confirmed == 1,
		      "fp-one-pair: pcm_confirmed 0 well past the confirm boundary");
		int ok = 1;
		uint32_t got = passthrough_frames_from(w, CONFIRM, &cap, &ok);
		CHECK(ok && got == EMIT_FRAMES,
		      "fp-one-pair: pass-through not byte-exact from the boundary "
		      "(%u frames, want %d)", got, EMIT_FRAMES);
		capture_free(&cap);
		free(w);
	}
	/* (a2) the same single pair placed INSIDE the unconfirmed window of a
	 * stream that goes on to confirm. An armed candidate must not delay, blank
	 * or shift the boundary: the first emitted frame is still exactly the
	 * boundary word pair, L-first, and the stream is byte-exact from there.
	 * Without this leg nothing in the suite couples detector candidate state to
	 * the confirm boundary. */
	{
		enum {
			CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS,
			WORDS = 12288,
			PAIR = 1000,
			EMIT_FRAMES = (WORDS - CONFIRM) / 2
		};
		uint16_t *w = make_ramp(WORDS);
		plant_pair(w, PAIR, 0);
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipelineClass klass = run_reclass(w, WORDS, 256, &cap, &d,
		                                      "fp-in-window-pair");
		CHECK(klass == AIUS_PIPELINE_PCM, "fp-in-window-pair: class %d != PCM",
		      klass);
		CHECK(d.retype == 0, "fp-in-window-pair: retype_count %u != 0", d.retype);
		CHECK(cap.n > 0 &&
		              cap.blocks[0].planes[0][0] ==
		                      pcm_expected_sample(w[CONFIRM]) &&
		              cap.blocks[0].planes[1][0] ==
		                      pcm_expected_sample(w[CONFIRM + 1]),
		      "fp-in-window-pair: first emitted frame is not run word %d L-first",
		      CONFIRM);
		int ok = 1;
		uint32_t got = passthrough_frames_from(w, CONFIRM, &cap, &ok);
		CHECK(ok && got == EMIT_FRAMES,
		      "fp-in-window-pair: pass-through not byte-exact from the boundary "
		      "(%u frames, want %d)", got, EMIT_FRAMES);
		CHECK(d.squelched_frames == (uint32_t)CONFIRM / 2u,
		      "fp-in-window-pair: squelched_frames %u != %u",
		      d.squelched_frames, (unsigned)(CONFIRM / 2));
		capture_free(&cap);
		free(w);
	}
	/* (b) two pairs 100 words apart (sub-MIN, same form): candidate persists,
	 * no re-type. */
	{
		enum { WORDS = 4000 };
		uint16_t *w = make_ramp(WORDS);
		plant_pair(w, 1000, 0);
		plant_pair(w, 1100, 0);
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipelineClass klass = run_reclass(w, WORDS, 256, &cap, &d,
		                                      "fp-close-pairs");
		CHECK(klass == AIUS_PIPELINE_PCM && d.retype == 0,
		      "fp-close-pairs: class %d retype %u (want PCM, 0)", klass,
		      d.retype);
		capture_free(&cap);
		free(w);
	}
	/* (c) two pairs 16000 words apart (over MAX): replacement, no re-type. */
	{
		enum { WORDS = 17000 };
		uint16_t *w = make_ramp(WORDS);
		plant_pair(w, 600, 0);
		plant_pair(w, 16600, 0);
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipelineClass klass = run_reclass(w, WORDS, 256, &cap, &d,
		                                      "fp-far-pairs");
		CHECK(klass == AIUS_PIPELINE_PCM && d.retype == 0,
		      "fp-far-pairs: class %d retype %u (want PCM, 0)", klass, d.retype);
		capture_free(&cap);
		free(w);
	}
	/* (d) form mismatch never validates, and DOES replace the candidate: a
	 * BE pair, then an in-range LE pair (mismatch -> no re-type, candidate
	 * replaced), then a second LE pair in range OF THE REPLACEMENT. Re-type
	 * fires iff the replacement installed: had the BE candidate survived,
	 * the third pair would mismatch it too and nothing would fire. */
	{
		enum { WORDS = 4000 };
		uint16_t *w = make_ramp(WORDS);
		plant_pair(w, 1000, 0);
		plant_pair(w, 2000, 1); /* LE */
		plant_pair(w, 2600, 1);
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipelineClass klass = run_reclass(w, WORDS, 256, &cap, &d,
		                                      "fp-form-mismatch");
		CHECK(klass == AIUS_PIPELINE_DTS && d.retype == 1,
		      "fp-form-mismatch: class %d retype %u (want DTS, 1 -- mismatch "
		      "must replace, same-form must validate)", klass, d.retype);
		capture_free(&cap);
		free(w);
	}
	printf("  fp-immunity    one pair (post- and in-window) / sub-MIN / "
	       "over-MAX / form-mismatch\n");
}

/* T3b: the worst re-type phase against the confirm bound. Synthetic pairs at
 * EXACTLY the maximum supported burst period (4096 words) with the first Pa at
 * run word 4095, so the validating Pb lands on run word 8192 -- the confirm
 * boundary word itself, the first word that would ever be staged.
 *
 * Zero blocks reach the sink, which is the honest statement of the bound: the
 * worst phase validates AT the boundary, one word past the window rather than
 * inside it, and still emits nothing because (a) the detector runs BEFORE
 * staging in pcm_feed, so the validating Pb is consumed by the re-type and
 * never staged, and (b) emission is quantized to a whole block, so even a
 * staged boundary word would need 1024 more words to reach the sink. Narrow
 * CONFIRM below the MAX pair spacing and this test emits burst data. */
static void test_worst_phase_confirm_bound(void)
{
	enum {
		CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS,
		SPACING = 4096, /* the widest supported burst period, in words */
		/* so the SECOND Pa is the last squelched word and its Pb is the
		 * boundary word */
		FIRST_PA = CONFIRM - SPACING - 1,
		WORDS = 10240
	};
	uint16_t *w = make_ramp(WORDS);
	plant_pair(w, FIRST_PA, 0);
	plant_pair(w, FIRST_PA + SPACING, 0);

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	struct reclass_diag d;
	AiusPipelineClass klass = run_reclass(w, WORDS, 256, &cap, &d,
	                                      "worst-phase");
	CHECK(klass == AIUS_PIPELINE_DTS,
	      "worst-phase: class %d != DTS (the pair at the boundary must re-type)",
	      klass);
	CHECK(d.retype == 1, "worst-phase: retype_count %u != 1", d.retype);
	CHECK(cap.n == 0,
	      "worst-phase: %d blocks reached the sink -- the worst-phase re-type "
	      "must emit nothing at all", cap.n);
	/* Words 0..8191 dropped: the validating Pb at 8192 is consumed by the
	 * re-type, so it is neither staged nor counted as squelched. */
	CHECK(d.squelched_frames == (uint32_t)CONFIRM / 2u,
	      "worst-phase: squelched_frames %u != %u", d.squelched_frames,
	      (unsigned)(CONFIRM / 2));
	CHECK(d.pcm_confirmed == 0, "worst-phase: pcm_confirmed set after re-type");
	printf("  worst-phase    4096-spaced pairs validate AT run word %d, "
	       "0 blocks emitted\n", CONFIRM);
	capture_free(&cap);
	free(w);
}

/* T4: the validated pair split exactly across a push boundary (Pa last word
 * of one push, Pb first of the next): the prev-word carry must bridge it. */
static void test_retype_boundary_pair(const char *dir)
{
	size_t n = 0, sn = 0;
	uint16_t *v = load_wire(dir, "dts_be16", &n);
	if (!v) {
		CHECK(0, "boundary-pair: corpus load failed");
		return;
	}
	uint16_t *s = repeat_wire(v, n, 8, &sn);

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "boundary-pair: create");
	/* Entry at offset 4: second pair at pushed 1888/1889. Split there. */
	CHECK(aius_pipeline_push(p, s + 4, 1889) == AIUS_OK, "boundary-pair: push A");
	CHECK(aius_pipeline_push(p, s + 4 + 1889, sn - 4 - 1889) == AIUS_OK,
	      "boundary-pair: push B");
	CHECK(aius_pipeline_finish(p) == AIUS_OK, "boundary-pair: finish");
	uint32_t rt = 0, rc = 0;
	CHECK(aius_pipeline_get_reclass_counters(p, &rt, &rc) == AIUS_OK,
	      "boundary-pair: counters");
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_DTS && rt == 1,
	      "boundary-pair: class %d retype %u (want DTS, 1)",
	      aius_pipeline_classification(p), rt);
	aius_pipeline_destroy(p);
	printf("  boundary-pair  Pa|Pb split across pushes still re-types\n");
	capture_free(&cap);
	free(s);
	free(v);
}

/* T5: DTS -> starve -> PCM -> re-type -> DTS, in one stream. The resumed-DTS
 * leg is the starvation-counter-residue regression (gate finding F1): stale
 * residue across the class transition re-fires starvation instantly and the
 * final class never holds DTS.
 *
 * The re-stage point is deterministic for this corpus and this chunking: the
 * burst-2 outcome lands in the 1792-2047 push, and the starvation check first
 * fires 64 pushes later, at word 18432 -- exact consumed-prefix starvation
 * accounting includes the sterile suffix after the preceding burst outcome.
 * The ramp is sized from that so the confirmed run has exactly three
 * whole blocks of post-confirm words before the DTS tail arrives: the recovered
 * PCM demonstrably plays, and the switch lands at staged phase 0, so the burst
 * tail staged as PCM is dropped whole. Same phase-511 caveat as T2 -- this pins
 * the block-aligned case, not a universal no-buzz property. */
static void test_starve_and_recover(const char *dir)
{
	size_t n = 0, head_n = 0, tail_n = 0;
	uint16_t *v = load_wire(dir, "dts_be16", &n);
	if (!v) {
		CHECK(0, "starve: corpus load failed");
		return;
	}
	uint16_t *head = repeat_wire(v, n, 2, &head_n);
	uint16_t *tail = repeat_wire(v, n, 4, &tail_n);
	const size_t block_words = 2u * AIUS_PIPELINE_PCM_BLOCK_FRAMES;
	const size_t restage_word = 18432u;
	const size_t confirm_word = restage_word + AIUS_PIPELINE_PCM_CONFIRM_WORDS;
	const size_t ramp_words = confirm_word + 3u * block_words - head_n;
	uint16_t *ramp = make_ramp(ramp_words);
	size_t sn = head_n + ramp_words + tail_n;
	uint16_t *s = malloc(sn * sizeof(uint16_t));
	memcpy(s, head, head_n * sizeof(uint16_t));
	memcpy(s + head_n, ramp, ramp_words * sizeof(uint16_t));
	memcpy(s + head_n + ramp_words, tail, tail_n * sizeof(uint16_t));

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	struct reclass_diag d;
	AiusPipelineClass klass = run_reclass(s, sn, 256, &cap, &d, "starve");
	CHECK(klass == AIUS_PIPELINE_DTS, "starve: final class %d != DTS", klass);
	CHECK(d.reclassify == 1, "starve: reclassify_count %u != 1", d.reclassify);
	CHECK(d.retype == 1, "starve: retype_count %u != 1", d.retype);
	/* 2 decoded (head) + 3 PCM (the confirmed ramp segment, and NOTHING from
	 * the burst tail) + 3 decoded (resumed tail bursts 2..4) = 8. A 9th block
	 * would be burst data played as PCM. */
	CHECK(cap.n == 8, "starve: %d blocks != 8", cap.n);
	/* The three PCM blocks are the ramp from the confirm boundary on, in
	 * order: this is what pins the re-stage point and the boundary together
	 * (dts_be16 decodes to 2ch, so block shape cannot tell them apart). */
	for (int i = 0; i < 3; i++)
		CHECK(cap.n > 2 + i && cap.blocks[2 + i].frames ==
		                               AIUS_PIPELINE_PCM_BLOCK_FRAMES &&
		              cap.blocks[2 + i].planes[0][0] ==
		                      pcm_expected_sample(
		                              s[confirm_word + (size_t)i * block_words]),
		      "starve: capture block %d is not confirmed ramp block %d", 2 + i,
		      i);
	/* One squelched run: 8192 words from the re-stage point. */
	CHECK(d.squelched_frames == AIUS_PIPELINE_PCM_CONFIRM_WORDS / 2u,
	      "starve: squelched_frames %u != %u", d.squelched_frames,
	      (unsigned)(AIUS_PIPELINE_PCM_CONFIRM_WORDS / 2u));
	printf("  starve-recover DTS starves to PCM, confirms, re-types back, "
	       "8 blocks\n");
	capture_free(&cap);
	free(s);
	free(ramp);
	free(head);
	free(tail);
	free(v);
}

/* T6: framed-but-unsupported never starves. Repeated type-7 (MPEG-2 AAC)
 * bursts are framing evidence, so the class must hold bitstream in silence
 * rather than decay into PCM noise. Fails if the starvation counter resets on
 * AIUS_OK only. (This leg used the E-AC-3-shaped vector until type 21 went in
 * scope in phase 3; type 7 is the genuinely refused type now.) */
static void test_unsupported_never_starves(const char *dir)
{
	size_t n = 0, sn = 0;
	uint16_t *v = load_wire(dir, "unsupported_type7", &n);
	if (!v) {
		CHECK(0, "unsupported: corpus load failed");
		return;
	}
	const size_t repeats = AIUS_PIPELINE_DTS_STARVE_WORDS / n + 2u;
	uint16_t *s = repeat_wire(v, n, repeats, &sn);
	CHECK(sn > AIUS_PIPELINE_DTS_STARVE_WORDS,
	      "unsupported: repeated vector does not span starvation budget");

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	struct reclass_diag d;
	AiusPipelineClass klass = run_reclass(s, sn, 256, &cap, &d, "unsupported");
	CHECK(klass == AIUS_PIPELINE_DTS, "unsupported: class %d != DTS", klass);
	CHECK(d.reclassify == 0, "unsupported: reclassify_count %u != 0 (starved!)",
	      d.reclassify);
	CHECK(cap.n == 0, "unsupported: %d blocks != 0", cap.n);
	printf("  unsupported    type-7 bursts hold bitstream class, no starve\n");
	capture_free(&cap);
	free(s);
	free(v);
}

/* T7: reset semantics. (a) the lifetime counters survive aius_pipeline_reset;
 * (b) the detector's candidate does NOT -- a pre-reset pair may not validate
 * a post-reset pair; (c) squelched_frames is a lifetime counter like (a) while
 * pcm_confirmed is live gate state that goes false again on every PCM
 * re-entry. */
static void test_reset_semantics(const char *dir)
{
	size_t n = 0, sn = 0;
	uint16_t *v = load_wire(dir, "dts_be16", &n);
	if (!v) {
		CHECK(0, "reset-sem: corpus load failed");
		return;
	}
	uint16_t *s = repeat_wire(v, n, 8, &sn);

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK, "reset-sem: create");

	/* (a) drive a re-type, then reset: counters must survive. */
	push_chunked(p, s + 4, sn - 4, 256, "reset-sem");
	uint32_t rt = 0, rc = 0, sq = 0;
	int confirmed = 0;
	CHECK(aius_pipeline_get_reclass_counters(p, &rt, &rc) == AIUS_OK,
	      "reset-sem: counters");
	CHECK(rt == 1, "reset-sem: retype_count %u != 1 before reset", rt);
	/* Same mid-burst entry as T1: 1889 dropped words -> 944 frames. */
	CHECK(aius_pipeline_get_squelch_diag(p, &sq, &confirmed) == AIUS_OK,
	      "reset-sem: squelch diag");
	CHECK(sq == 944 && confirmed == 0,
	      "reset-sem: squelch diag (%u,%d) before reset (want 944,0)", sq,
	      confirmed);
	aius_pipeline_reset(p);
	CHECK(aius_pipeline_get_reclass_counters(p, &rt, &rc) == AIUS_OK,
	      "reset-sem: counters after reset");
	CHECK(rt == 1 && rc == 0,
	      "reset-sem: counters (%u,%u) did not survive reset (want 1,0)", rt, rc);
	CHECK(aius_pipeline_get_squelch_diag(p, &sq, &confirmed) == AIUS_OK,
	      "reset-sem: squelch diag after reset");
	CHECK(sq == 944, "reset-sem: squelched_frames %u did not survive reset "
	      "(want 944)", sq);
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_UNKNOWN,
	      "reset-sem: class not UNKNOWN after reset");

	/* (b) pair before reset + in-range pair after reset must NOT re-type:
	 * ramp types PCM, pair at 550 arms a candidate, reset clears it, second
	 * run's pair at 1550 (delta 1000 from the first if state leaked) becomes
	 * a fresh candidate instead of validating. */
	uint16_t *r1 = make_ramp(700);
	plant_pair(r1, 550, 0);
	push_chunked(p, r1, 700, 256, "reset-sem-b1");
	aius_pipeline_reset(p);
	uint16_t *r2 = make_ramp(2000);
	plant_pair(r2, 1550, 0);
	push_chunked(p, r2, 2000, 256, "reset-sem-b2");
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_PCM,
	      "reset-sem: class %d != PCM after leaked-candidate probe",
	      aius_pipeline_classification(p));
	CHECK(aius_pipeline_get_reclass_counters(p, &rt, &rc) == AIUS_OK,
	      "reset-sem: final counters");
	CHECK(rt == 1, "reset-sem: retype_count %u != 1 (pre-reset pair leaked!)",
	      rt);

	/* (c) squelch counter/gate semantics. Both (b) runs ended well inside
	 * their unconfirmed windows, so every one of their words was dropped:
	 * 944 + 350 (r1) + 1000 (r2) frames, gate still closed. */
	const uint32_t confirm_frames = AIUS_PIPELINE_PCM_CONFIRM_WORDS / 2u;
	CHECK(aius_pipeline_get_squelch_diag(p, &sq, &confirmed) == AIUS_OK,
	      "reset-sem: squelch diag (b)");
	CHECK(sq == 944u + 350u + 1000u && confirmed == 0,
	      "reset-sem: squelch diag (%u,%d) after two in-window runs "
	      "(want %u,0)", sq, confirmed, 944u + 350u + 1000u);
	/* Carry the SAME run past the boundary: the gate opens and this run's
	 * dropped total tops out at CONFIRM/2 frames, not one frame more. */
	uint16_t *r3 = make_ramp(AIUS_PIPELINE_PCM_CONFIRM_WORDS);
	push_chunked(p, r3, AIUS_PIPELINE_PCM_CONFIRM_WORDS, 256, "reset-sem-c1");
	CHECK(aius_pipeline_get_squelch_diag(p, &sq, &confirmed) == AIUS_OK,
	      "reset-sem: squelch diag (c1)");
	CHECK(sq == 944u + 350u + confirm_frames && confirmed == 1,
	      "reset-sem: squelch diag (%u,%d) past the boundary (want %u,1)", sq,
	      confirmed, 944u + 350u + confirm_frames);
	/* Reset: the lifetime counter survives, the gate closes, and the next PCM
	 * entry starts unconfirmed again -- a re-entered class is never trusted
	 * because a previous run of the same class was. */
	aius_pipeline_reset(p);
	CHECK(aius_pipeline_get_squelch_diag(p, &sq, &confirmed) == AIUS_OK,
	      "reset-sem: squelch diag (c2)");
	CHECK(sq == 944u + 350u + confirm_frames && confirmed == 0,
	      "reset-sem: squelch diag (%u,%d) after reset (want %u,0)", sq,
	      confirmed, 944u + 350u + confirm_frames);
	push_chunked(p, r3, 600, 256, "reset-sem-c3");
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_PCM,
	      "reset-sem: class %d != PCM after re-entry",
	      aius_pipeline_classification(p));
	CHECK(aius_pipeline_get_squelch_diag(p, &sq, &confirmed) == AIUS_OK,
	      "reset-sem: squelch diag (c3)");
	CHECK(sq == 944u + 350u + confirm_frames + 300u && confirmed == 0,
	      "reset-sem: squelch diag (%u,%d) on PCM re-entry (want %u,0)", sq,
	      confirmed, 944u + 350u + confirm_frames + 300u);

	aius_pipeline_destroy(p);
	printf("  reset-sem      counters survive reset; detector state and the "
	       "confirm gate do not\n");
	capture_free(&cap);
	free(r1);
	free(r2);
	free(r3);
	free(s);
	free(v);
}

struct deferred_sink {
	uint32_t calls;
	uint32_t commits;
	int block_once;
	int refuse_all;
	uint32_t fail_on_call;
	AiusStatus fail_status;
	const AiusPcmBlock *first;
	const AiusPcmBlock *second;
	AiusPcmBlock first_copy;
	AiusPcmBlock second_copy;
	uint64_t hashes[32];
};

static uint64_t deferred_block_hash(const AiusPcmBlock *block)
{
	uint64_t hash = UINT64_C(0xcbf29ce484222325);
	for (uint16_t channel = 0u; channel < block->channels; channel++) {
		for (uint32_t frame = 0u; frame < block->frames; frame++) {
			uint32_t sample = (uint32_t)block->planes[channel][frame];
			for (uint32_t byte = 0u; byte < 4u; byte++) {
				hash ^= (uint8_t)(sample >> (8u * byte));
				hash *= UINT64_C(0x100000001b3);
			}
		}
	}
	return hash;
}

static AiusStatus deferred_consume(void *ctx, const AiusPcmBlock *block)
{
	struct deferred_sink *sink = ctx;
	sink->calls++;
	if (sink->calls <=
	    (uint32_t)(sizeof(sink->hashes) / sizeof(sink->hashes[0])))
		sink->hashes[sink->calls - 1u] = deferred_block_hash(block);
	if (sink->calls == 1u) {
		sink->first = block;
		sink->first_copy = *block;
	} else if (sink->calls == 2u) {
		sink->second = block;
		sink->second_copy = *block;
	}
	if (sink->block_once) {
		sink->block_once = 0;
		return AIUS_NO_SPACE;
	}
	if (sink->refuse_all)
		return AIUS_REFUSED;
	if (sink->fail_on_call == sink->calls)
		return sink->fail_status;
	sink->commits++;
	return AIUS_OK;
}

static void test_deferred_ac3(const char *dir)
{
	size_t n = 0u;
	uint16_t *wire = load_wire(dir, "ac3_surround51_ident", &n);
	CHECK(wire != NULL, "defer-ac3: corpus load");
	if (!wire)
		return;

	struct deferred_sink sink_state = { .block_once = 1 };
	AiusPcmSink sink = { .ctx = &sink_state, .consume = deferred_consume };
	struct increment_cycle_clock fake_clock = {
		.now = UINT32_MAX - 12u,
		.step = 17u,
	};
	AiusPipelineConfig clock_config = {
		.packing = AIUS_IEC61937_PACK_16,
		.cycle_clock = {
			.ctx = &fake_clock,
			.read = increment_cycle_read,
		},
	};
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, &clock_config, &sink) == AIUS_OK,
	      "defer-ac3: create");
	size_t consumed = 0u;
	CHECK(aius_pipeline_ingest(p, wire, n, &consumed) == AIUS_OK &&
	          consumed > 0u && consumed < n && aius_pipeline_has_pending(p),
	      "defer-ac3: first AU retained at exact prefix");

	AiusPipelineServiceResult result;
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_NO_SPACE &&
	          result == AIUS_PIPELINE_SERVICE_TRANSMIT_BLOCKED,
	      "defer-ac3: first block backpressure");
	CHECK(fake_clock.calls == 4u,
	      "defer-ac3: begin/block timing reads=%u", fake_clock.calls);
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK &&
	          result == AIUS_PIPELINE_SERVICE_PROGRESSED,
	      "defer-ac3: identical retained retry");
	AiusMetrics cycle_metrics;
	CHECK(fake_clock.calls == 4u &&
	          aius_pipeline_get_codec_metrics(p, 1u, &cycle_metrics) ==
	              AIUS_OK &&
	          cycle_metrics.decode_cycles == 0u,
	      "defer-ac3: retry remeasured or partial timing published");
	CHECK(sink_state.first == sink_state.second &&
	          memcmp(&sink_state.first_copy, &sink_state.second_copy,
	                 sizeof(sink_state.first_copy)) == 0 &&
	          sink_state.hashes[0] == sink_state.hashes[1],
	      "defer-ac3: retry descriptor/sample identity");
	CHECK(aius_pipeline_codec_pc(p) == 0u,
	      "defer-ac3: codec became live before all six blocks");
	for (uint32_t block = 1u;
	     block < AIUS_A52_BLOCKS_PER_ACCESS_UNIT; block++)
		CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK &&
		          result == AIUS_PIPELINE_SERVICE_PROGRESSED,
		      "defer-ac3: block %u service", block + 1u);

	AiusPipelineServiceDiag diag;
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.decode_units == 6u && diag.sink_attempts == 7u &&
	          diag.committed_blocks == 6u &&
	          diag.transmit_stalls == 1u &&
	          diag.retained_retries == 1u &&
	          diag.output_drops == 0u && diag.au_abandons == 0u,
	      "defer-ac3: six-unit/retry accounting");
	CHECK(aius_pipeline_codec_pc(p) == 1u,
	      "defer-ac3: codec not live after all six commits");
	CHECK(fake_clock.calls == 14u &&
	          aius_pipeline_get_codec_metrics(p, 1u, &cycle_metrics) ==
	              AIUS_OK &&
	          cycle_metrics.decode_cycles ==
	              7u * (uint64_t)fake_clock.step,
	      "defer-ac3: completed timing aggregate/retry calls");

	/* Explicit reset (also the source/generation-gap entry point) abandons an
	 * armed iterator. A restarted AU must reproduce a clean block zero. */
	consumed = 0u;
	CHECK(aius_pipeline_ingest(p, wire, n, &consumed) == AIUS_OK &&
	          aius_pipeline_has_pending(p),
	      "defer-ac3: arm reset probe");
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK,
	      "defer-ac3: commit pre-reset block zero");
	CHECK(fake_clock.calls == 18u,
	      "defer-ac3: partial AU timing reads=%u", fake_clock.calls);
	aius_pipeline_reset(p);
	CHECK(aius_pipeline_codec_pc(p) == 0u &&
	          !aius_pipeline_has_pending(p),
	      "defer-ac3: reset clears pending codec state");
	CHECK(aius_pipeline_get_codec_metrics(p, 1u, &cycle_metrics) == AIUS_OK &&
	          cycle_metrics.decode_cycles ==
	              7u * (uint64_t)fake_clock.step,
	      "defer-ac3: reset published partial timing");
	CHECK(aius_pipeline_push(p, wire, n) == AIUS_OK,
	      "defer-ac3: clean AU after reset");
	CHECK(sink_state.hashes[8] == sink_state.hashes[0],
	      "defer-ac3: abandon/reset did not reproduce fresh block zero");
	aius_pipeline_destroy(p);

	/* A fatal sink status is a discontinuity and abandons the partial AU. */
	memset(&sink_state, 0, sizeof(sink_state));
	sink_state.fail_on_call = 2u;
	sink_state.fail_status = AIUS_MALFORMED;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "defer-ac3-fatal: create");
	consumed = 0u;
	CHECK(aius_pipeline_ingest(p, wire, n, &consumed) == AIUS_OK,
	      "defer-ac3-fatal: ingest");
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK,
	      "defer-ac3-fatal: first block");
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_MALFORMED,
	      "defer-ac3-fatal: fatal sink status");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.committed_blocks == 1u && diag.output_drops == 1u &&
	          diag.au_abandons == 1u && diag.decode_rejects == 0u &&
	          aius_pipeline_codec_pc(p) == 0u &&
	          !aius_pipeline_has_pending(p),
	      "defer-ac3-fatal: discontinuity accounting/cleanup");
	sink_state.fail_on_call = 0u;
	CHECK(aius_pipeline_push(p, wire, n) == AIUS_OK &&
	          aius_pipeline_codec_pc(p) == 1u,
	      "defer-ac3-fatal: clean restart");
	aius_pipeline_destroy(p);

	/* An intentional sink refusal consumes every decoded block without
	 * committing output, resetting classification, or reporting a failure. */
	memset(&sink_state, 0, sizeof(sink_state));
	sink_state.refuse_all = 1;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "defer-ac3-refused: create");
	CHECK(aius_pipeline_push(p, wire, n) == AIUS_OK,
	      "defer-ac3-refused: consume");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.decode_units == 12u && diag.sink_attempts == 12u &&
	          diag.committed_blocks == 0u &&
	          diag.output_drops == 0u && diag.au_abandons == 0u &&
	          sink_state.commits == 0u &&
	          aius_pipeline_codec_pc(p) == 1u &&
	          aius_pipeline_classification(p) == AIUS_PIPELINE_DTS &&
	          !aius_pipeline_has_pending(p),
	      "defer-ac3-refused: intentional mute accounting/state");
	aius_pipeline_destroy(p);

	/* Legacy push cannot retain its caller-owned suffix: it abandons, then a
	 * later complete push starts clean. */
	memset(&sink_state, 0, sizeof(sink_state));
	sink_state.block_once = 1;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "legacy-ac3-loss: create");
	CHECK(aius_pipeline_push(p, wire, n) == AIUS_NO_SPACE,
	      "legacy-ac3-loss: returns no-space");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.output_drops == 1u && diag.au_abandons == 1u &&
	          diag.committed_blocks == 0u &&
	          aius_pipeline_codec_pc(p) == 0u,
	      "legacy-ac3-loss: exact abandon accounting");
	CHECK(aius_pipeline_push(p, wire, n) == AIUS_OK &&
	          aius_pipeline_codec_pc(p) == 1u,
	      "legacy-ac3-loss: clean next push");
	aius_pipeline_destroy(p);

	/* Controlled core error after three committed blocks: reject/abandon are
	 * each counted once, later blocks are truncated, and the next AU is fresh. */
	memset(&sink_state, 0, sizeof(sink_state));
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "defer-ac3-midfail: create");
	CHECK(aius_pipeline_test_fail_a52_block(p, 4u) == AIUS_OK,
	      "defer-ac3-midfail: inject");
	consumed = 0u;
	CHECK(aius_pipeline_ingest(p, wire, n, &consumed) == AIUS_OK,
	      "defer-ac3-midfail: ingest");
	for (uint32_t unit = 0u; unit < 4u; unit++)
		CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK,
		      "defer-ac3-midfail: unit %u", unit + 1u);
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.decode_units == 4u && diag.committed_blocks == 3u &&
	          diag.decode_rejects == 1u && diag.au_abandons == 1u &&
	          diag.output_drops == 0u && !aius_pipeline_has_pending(p) &&
	          aius_pipeline_codec_pc(p) == 0u,
	      "defer-ac3-midfail: exact reject/abandon accounting");
	CHECK(aius_pipeline_push(p, wire, n) == AIUS_OK &&
	          aius_pipeline_codec_pc(p) == 1u,
	      "defer-ac3-midfail: clean restart");
	aius_pipeline_destroy(p);

	free(wire);
	printf("  deferred-ac3   six units, retry identity, all abandon paths\n");
}

static void test_codec_lifecycle(const char *dir)
{
	size_t ac3_n = 0u;
	size_t malformed_n = 0u;
	size_t dts_n = 0u;
	uint16_t *ac3 = load_wire(dir, "ac3_surround51_ident", &ac3_n);
	uint16_t *malformed = load_wire(dir, "zero_len_ac3", &malformed_n);
	uint16_t *dts = load_wire(dir, "dts_be16", &dts_n);
	CHECK(ac3 && malformed && dts, "codec-life: corpus load");
	if (!ac3 || !malformed || !dts) {
		free(ac3);
		free(malformed);
		free(dts);
		return;
	}

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "codec-life: create");
	CHECK(aius_pipeline_push(p, ac3, ac3_n) == AIUS_OK &&
	          aius_pipeline_codec_pc(p) == 1u,
	      "codec-life: successful AC-3 selects Pc1");
	CHECK(aius_pipeline_push(p, malformed, malformed_n) == AIUS_OK &&
	          aius_pipeline_codec_pc(p) == 1u,
	      "codec-life: later malformed burst erased successful Pc");
	AiusMetrics metrics;
	CHECK(aius_pipeline_get_metrics(p, &metrics) == AIUS_OK &&
	          metrics.last_status == AIUS_MALFORMED,
	      "codec-life: compatibility status did not preserve parse reject");

	aius_pipeline_reset(p);
	CHECK(aius_pipeline_codec_pc(p) == 0u &&
	          aius_pipeline_get_metrics(p, &metrics) == AIUS_OK &&
	          metrics.decode_cycles == 0u && metrics.alloc_count == 0u &&
	          metrics.stack_high_water == 0u &&
	          metrics.last_status == AIUS_OK,
	      "codec-life: reset did not clear live codec projection");

	CHECK(aius_pipeline_push(p, dts, dts_n) == AIUS_OK &&
	          aius_pipeline_codec_pc(p) >= 11u &&
	          aius_pipeline_codec_pc(p) <= 13u,
	      "codec-life: successful DTS did not select its Pc");
	size_t starve_n = AIUS_PIPELINE_DTS_STARVE_WORDS + 256u;
	uint16_t *starve = calloc(starve_n, sizeof(uint16_t));
	CHECK(starve != NULL, "codec-life: starve allocation");
	if (starve) {
		CHECK(aius_pipeline_push(p, starve, starve_n) == AIUS_OK,
		      "codec-life: starve body");
		uint16_t boundary = 0u;
		CHECK(aius_pipeline_push(p, &boundary, 1u) == AIUS_OK &&
		          aius_pipeline_codec_pc(p) == 0u,
		      "codec-life: starvation did not clear codec Pc");
		free(starve);
	}

	aius_pipeline_destroy(p);
	capture_free(&cap);
	free(ac3);
	free(malformed);
	free(dts);
	printf("  codec-lifecycle preserve reject; clear reset/starvation/reclass\n");
}

/* The operator clear owns the observation latches.
 *
 * `last_status` survives every AUTOMATIC transition on purpose -- a fault the
 * pipeline swallowed to keep the wire streaming must stay visible, which is the
 * whole reason the field exists. The cost was that the documented AC-3-to-PCM
 * diagnostic (pull the source, watch it re-type) left AIUS_MALFORMED sitting in
 * pipeline_diag.last_status with nothing but a reboot able to move it, because
 * the PCM route never writes the field. The clear the operator invokes is the
 * one event entitled to drop it. */
static void test_clear_drops_observation_latches(const char *dir)
{
	size_t ac3_n = 0u;
	size_t malformed_n = 0u;
	uint16_t *ac3 = load_wire(dir, "ac3_surround51_ident", &ac3_n);
	uint16_t *malformed = load_wire(dir, "zero_len_ac3", &malformed_n);
	CHECK(ac3 && malformed, "clear-latch: corpus load");
	if (!ac3 || !malformed) {
		free(ac3);
		free(malformed);
		return;
	}

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "clear-latch: create");

	AiusMetrics metrics;
	AiusPipelineServiceDiag diag;

	/* A clean AC-3 run, then an aborted encoded unit: the reject latches on
	 * BOTH surfaces -- pipeline_diag.last_status and the service diag's
	 * last_failure, which is what audio_diag.pipeline_last_failure serves. */
	CHECK(aius_pipeline_push(p, ac3, ac3_n) == AIUS_OK,
	      "clear-latch: clean AC-3");
	CHECK(aius_pipeline_push(p, malformed, malformed_n) == AIUS_OK,
	      "clear-latch: aborted unit is non-fatal to the push");
	CHECK(aius_pipeline_get_metrics(p, &metrics) == AIUS_OK &&
	          metrics.last_status == AIUS_MALFORMED,
	      "clear-latch: last_status did not latch the reject");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.last_failure == AIUS_MALFORMED && diag.parse_rejects > 0u,
	      "clear-latch: service diag did not record the reject");

	/* The teardown itself: the source goes away, the bitstream class starves
	 * out and the stream re-types to confirmed PCM. None of that moves the
	 * latch -- the PCM route never writes it -- which is why a reboot used to
	 * be the only way out. Long enough to cross the starve bound AND the
	 * confirmation window, so the re-type is complete rather than pending. */
	const size_t ramp_words = AIUS_PIPELINE_DTS_STARVE_WORDS +
	                          AIUS_PIPELINE_PCM_CONFIRM_WORDS +
	                          2u * AIUS_PIPELINE_PCM_BLOCK_FRAMES;
	uint16_t *ramp = make_ramp(ramp_words);
	push_chunked(p, ramp, ramp_words, 256, "clear-latch-pcm");
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_PCM,
	      "clear-latch: teardown did not re-type to PCM");
	CHECK(aius_pipeline_get_metrics(p, &metrics) == AIUS_OK &&
	          metrics.last_status == AIUS_MALFORMED,
	      "clear-latch: an automatic re-type erased the latch");

	/* The operator's clear takes both to zero. AIUS_OK is 0, which is what
	 * the two JSON fields serve. */
	aius_pipeline_clear_service_diag(p);
	CHECK(aius_pipeline_get_metrics(p, &metrics) == AIUS_OK &&
	          metrics.last_status == AIUS_OK && (int)metrics.last_status == 0,
	      "clear-latch: pipeline_diag.last_status survived the clear");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.last_failure == AIUS_OK && (int)diag.last_failure == 0 &&
	          diag.parse_rejects == 0u && diag.decode_rejects == 0u &&
	          diag.au_abandons == 0u && diag.committed_blocks == 0u,
	      "clear-latch: audio_diag.pipeline_last_failure or a counter "
	      "survived the clear");

	/* A clear is not a mute: the next real failure latches again. */
	CHECK(aius_pipeline_push(p, ac3, ac3_n) == AIUS_OK,
	      "clear-latch: clean AC-3 after the clear");
	CHECK(aius_pipeline_push(p, malformed, malformed_n) == AIUS_OK,
	      "clear-latch: second aborted unit");
	CHECK(aius_pipeline_get_metrics(p, &metrics) == AIUS_OK &&
	          metrics.last_status == AIUS_MALFORMED,
	      "clear-latch: a post-clear failure did not re-latch last_status");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.last_failure == AIUS_MALFORMED,
	      "clear-latch: a post-clear failure did not re-latch last_failure");

	aius_pipeline_clear_service_diag(NULL); /* NULL stays a no-op */
	aius_pipeline_destroy(p);
	capture_free(&cap);
	free(ramp);
	free(ac3);
	free(malformed);
	printf("  clear-latch    operator clear drops last_status + last_failure; "
	       "re-types do not\n");
}

static void test_deferred_dts(const char *dir)
{
	size_t n = 0u;
	uint16_t *wire = load_wire(dir, "dts_be16", &n);
	CHECK(wire != NULL, "defer-dts: corpus load");
	if (!wire)
		return;
	struct deferred_sink sink_state = { .block_once = 1 };
	AiusPcmSink sink = { .ctx = &sink_state, .consume = deferred_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "defer-dts: create");

	size_t consumed = 0u;
	CHECK(aius_pipeline_ingest(p, wire, n, &consumed) == AIUS_OK &&
	          consumed == n && aius_pipeline_has_pending(p),
	      "defer-dts: exact complete-AU prefix");
	AiusPipelineServiceDiag diag;
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(sink_state.calls == 0u && diag.decode_units == 0u,
	      "defer-dts: ingest invokes neither decoder nor sink");
	uint16_t suffix[16] = { 0 };
	consumed = 99u;
	CHECK(aius_pipeline_ingest(p, suffix, 16u, &consumed) == AIUS_OK &&
	          consumed == 0u,
	      "defer-dts: parser payload blocks later ingestion");

	AiusPipelineServiceResult result;
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_NO_SPACE &&
	          result == AIUS_PIPELINE_SERVICE_TRANSMIT_BLOCKED,
	      "defer-dts: first output blocked");
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK &&
	          result == AIUS_PIPELINE_SERVICE_PROGRESSED,
	      "defer-dts: retained output retries");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.decode_units == 1u && diag.sink_attempts == 2u &&
	          diag.committed_blocks == 1u && diag.transmit_stalls == 1u &&
	          diag.retained_retries == 1u &&
	          sink_state.first == sink_state.second,
	      "defer-dts: one bounded decode and identical retry");
	consumed = 0u;
	CHECK(aius_pipeline_ingest(p, wire, n, &consumed) == AIUS_OK &&
	          consumed == n && aius_pipeline_has_pending(p),
	      "defer-dts: arm reset cleanup");
	aius_pipeline_reset(p);
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK &&
	          result == AIUS_PIPELINE_SERVICE_IDLE &&
	          sink_state.calls == 2u,
	      "defer-dts: reset abandons parser-owned pending AU");
	aius_pipeline_destroy(p);

	memset(&sink_state, 0, sizeof(sink_state));
	sink_state.block_once = 1;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "legacy-dts-loss: create");
	CHECK(aius_pipeline_push(p, wire, n) == AIUS_NO_SPACE,
	      "legacy-dts-loss: blocked push returns no-space");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.output_drops == 1u && diag.au_abandons == 1u &&
	          diag.committed_blocks == 0u,
	      "legacy-dts-loss: output drop abandons its AU");
	aius_pipeline_destroy(p);

	memset(&sink_state, 0, sizeof(sink_state));
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "defer-finish: create");
	uint16_t staged[AIUS_PIPELINE_CLASSIFY_MAX_WORDS] = { 0 };
	staged[0] = 0xf872u;
	staged[1] = 0x4e1fu;
	staged[2] = 0x000du;
	staged[3] = 0x0010u;
	consumed = 0u;
	CHECK(aius_pipeline_ingest(p, staged,
	                           AIUS_PIPELINE_CLASSIFY_MAX_WORDS,
	                           &consumed) == AIUS_OK &&
	          consumed == AIUS_PIPELINE_CLASSIFY_MAX_WORDS &&
	          aius_pipeline_has_pending(p) &&
	          aius_pipeline_has_buffered_input(p),
	      "defer-finish: accepted typed suffix retained behind AU");
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK &&
	          result == AIUS_PIPELINE_SERVICE_PROGRESSED,
	      "defer-finish: reject malformed staged DTS AU");
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.decode_rejects == 1u && diag.committed_blocks == 0u,
	      "defer-finish: rejected service work is not committed");
	CHECK(aius_pipeline_finish(p) == AIUS_OK &&
	          !aius_pipeline_has_buffered_input(p),
	      "defer-finish: typed retained suffix is routed");
	aius_pipeline_destroy(p);
	free(wire);
	printf("  deferred-dts   exact prefix, retained retry and finish suffix\n");
}

static void test_output_slicing(void)
{
	int32_t left[1536];
	int32_t right[1536];
	AiusPcmBlock source;
	memset(&source, 0, sizeof(source));
	source.planes[0] = left;
	source.planes[1] = right;
	source.channels = 2u;
	source.frames = 1536u;
	source.sample_rate_hz = 48000u;
	source.channel_mask = AIUS_CH_FL | AIUS_CH_FR;
	AiusPcmBlock slice;
	CHECK(aius_pipeline_slice_pcm_block(&source, 0u, &slice) == AIUS_OK &&
	          slice.frames == 512u && slice.planes[0] == left &&
	          slice.planes[1] == right,
	      "slice: first 512-frame view");
	CHECK(aius_pipeline_slice_pcm_block(&source, 512u, &slice) == AIUS_OK &&
	          slice.frames == 512u && slice.planes[0] == left + 512u &&
	          slice.planes[1] == right + 512u,
	      "slice: second 512-frame view");
	CHECK(aius_pipeline_slice_pcm_block(&source, 1024u, &slice) == AIUS_OK &&
	          slice.frames == 512u && slice.planes[0] == left + 1024u &&
	          slice.planes[1] == right + 1024u,
	      "slice: third 512-frame view");
	CHECK(aius_pipeline_slice_pcm_block(&source, 1536u, &slice) ==
	          AIUS_INVALID_ARG,
	      "slice: completed block has no view");
	printf("  output-slice   1536 decoder-owned frames -> 512/512/512 views\n");
}

static void test_deferred_pcm_and_legacy_loss(void)
{
	const size_t block_words = 2u * AIUS_PIPELINE_PCM_BLOCK_FRAMES;
	const size_t total =
		AIUS_PIPELINE_PCM_CONFIRM_WORDS + block_words + 100u;
	uint16_t *words = make_ramp(total);
	struct deferred_sink sink_state = { .block_once = 1 };
	AiusPcmSink sink = { .ctx = &sink_state, .consume = deferred_consume };
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "defer-pcm: create");
	size_t consumed = 0u;
	CHECK(aius_pipeline_ingest(p, words, total, &consumed) == AIUS_OK &&
	          consumed == AIUS_PIPELINE_PCM_CONFIRM_WORDS + block_words &&
	          aius_pipeline_has_pending(p),
	      "defer-pcm: exact pending-block prefix");
	CHECK(sink_state.calls == 0u, "defer-pcm: ingest does not sink");
	AiusPipelineServiceResult result;
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_NO_SPACE &&
	          result == AIUS_PIPELINE_SERVICE_TRANSMIT_BLOCKED,
	      "defer-pcm: block retained");
	CHECK(aius_pipeline_service_one(p, &result) == AIUS_OK &&
	          result == AIUS_PIPELINE_SERVICE_PROGRESSED,
	      "defer-pcm: retry commits");
	size_t suffix_consumed = 0u;
	CHECK(aius_pipeline_ingest(p, words + consumed, total - consumed,
	                           &suffix_consumed) == AIUS_OK &&
	          suffix_consumed == total - consumed,
	      "defer-pcm: caller re-presents exact suffix");
	aius_pipeline_destroy(p);

	memset(&sink_state, 0, sizeof(sink_state));
	sink_state.block_once = 1;
	CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
	      "legacy-loss: create");
	CHECK(aius_pipeline_push(p, words, total) == AIUS_NO_SPACE,
	      "legacy-loss: blocked push returns no-space");
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_UNKNOWN &&
	          !aius_pipeline_has_pending(p),
	      "legacy-loss: blocked push abandons pending state");
	AiusPipelineServiceDiag diag;
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.output_drops == 1u && diag.au_abandons == 0u &&
	          diag.committed_blocks == 0u,
	      "legacy-loss: PCM output drop is not an AU abandon");
	CHECK(aius_pipeline_push(p, words, block_words) == AIUS_OK &&
	          sink_state.calls == 1u,
	      "legacy-loss: suffix was lost and next push re-confirms");
	aius_pipeline_destroy(p);
	free(words);
	printf("  deferred-pcm   retained retry; legacy NO_SPACE drops suffix cleanly\n");
}

/* ---- defect 35: a source teardown mid-burst drops the partial AU ----------
 *
 * Terminating a live encoded source mid-burst leaves the parser inside a
 * payload it will never be given the rest of. Left alone the NEXT words --
 * which belong to whatever the source became -- complete that burst to its
 * declared length and a dead access unit reaches a decoder: one rejected DTS
 * decode, or a whole six-block AC-3 grind (liba52 validates no payload CRC) of
 * which every block is full-scale garbage on the wire.
 *
 * The fix makes the discontinuity kill it first. These legs assert the
 * observable: across the teardown the dead AU gets ZERO decode attempts, the
 * staged input is dropped exactly ONCE with one au_abandon, and PCM then
 * recovers on its own confirmation schedule. */

/* Drive `w` through the DEFERRED target path -- 256-word ingests, one service
 * unit at a time -- which is exactly the shape of the AUDIO checkpoint. */
static void drive_deferred(AiusPipeline *p, const uint16_t *w, size_t n)
{
	size_t off = 0u;
	while (off < n || aius_pipeline_has_pending(p) ||
	       aius_pipeline_has_buffered_input(p)) {
		if (aius_pipeline_has_pending(p)) {
			AiusPipelineServiceResult result;
			(void)aius_pipeline_service_one(p, &result);
			continue;
		}
		size_t take = n - off;
		if (take > 256u)
			take = 256u;
		size_t consumed = 0u;
		(void)aius_pipeline_ingest(p, off < n ? w + off : NULL, take,
		                           &consumed);
		off += consumed;
		if (consumed == 0u && !aius_pipeline_has_pending(p) &&
		    !aius_pipeline_has_buffered_input(p))
			break;
	}
}

/* `cut` truncates the vector inside its first burst's payload; `au_units` is
 * how many decode units one whole vector produces (1 per DTS AU, 6 per AC-3
 * block sequence x the vector's AUs). */
static void test_teardown_drops_partial_burst(const char *dir, const char *name,
                                              size_t cut, uint32_t au_units,
                                              uint32_t au_blocks)
{
	enum {
		CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS,
		BLOCK_WORDS = 2 * (int)AIUS_PIPELINE_PCM_BLOCK_FRAMES,
		RAMP_WORDS = CONFIRM + 2 * BLOCK_WORDS,
		/* The control and residual legs get NO discontinuity, so their PCM
		 * has to wait out the starvation bound before it can re-classify at
		 * all; the teardown leg's reset re-classifies immediately. */
		LONG_RAMP_WORDS = (int)AIUS_PIPELINE_DTS_STARVE_WORDS + 2048 +
		                  CONFIRM + 4 * BLOCK_WORDS
	};
	size_t n = 0u;
	uint16_t *wire = load_wire(dir, name, &n);
	if (!wire) {
		CHECK(0, "teardown %s: corpus load", name);
		return;
	}
	uint16_t *ramp = make_ramp(RAMP_WORDS);
	uint16_t *long_ramp = make_ramp(LONG_RAMP_WORDS);
	AiusPipelineServiceDiag before;
	AiusPipelineServiceDiag after;
	AiusPipelineServiceDiag final;
	AiusMetrics metrics;
	uint32_t squelched = 0u;
	int confirmed = 0;

	/* (a) CONTROL -- the natural, complete-burst end of the same stream,
	 * followed by the same PCM. Nothing is abandoned, nothing is rejected. */
	{
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "teardown %s: control create", name);
		if (p) {
			drive_deferred(p, wire, n);
			drive_deferred(p, long_ramp, LONG_RAMP_WORDS);
			aius_pipeline_get_service_diag(p, &final);
			CHECK(aius_pipeline_get_squelch_diag(p, &squelched,
			                                     &confirmed) == AIUS_OK &&
			              confirmed == 1,
			      "teardown %s: control did not recover to confirmed PCM",
			      name);
			CHECK(final.decode_units == au_units &&
			              final.decode_rejects == 0u &&
			              final.au_abandons == 0u &&
			              final.parse_rejects == 0u &&
			              final.last_failure == AIUS_OK,
			      "teardown %s: natural EOF is not clean "
			      "(units=%u rejects=%u abandons=%u parse=%u last=%d)",
			      name, final.decode_units, final.decode_rejects,
			      final.au_abandons, final.parse_rejects,
			      (int)final.last_failure);
			CHECK(cap.n > (int)au_blocks,
			      "teardown %s: control emitted %d blocks, not the %u "
			      "decoded ones plus a recovered PCM run", name, cap.n,
			      au_blocks);
			capture_free(&cap);
			aius_pipeline_destroy(p);
		}
	}

	/* (b) THE DEFECT -- the source is cut mid-payload and torn down. */
	{
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "teardown %s: create", name);
		if (!p) {
			free(ramp);
			free(wire);
			return;
		}
		drive_deferred(p, wire, n);
		drive_deferred(p, wire, cut); /* the burst the source cut short */
		aius_pipeline_get_service_diag(p, &before);
		const int blocks_before = cap.n;
		CHECK(before.decode_units == au_units && before.au_abandons == 0u,
		      "teardown %s: the truncated prefix decoded something (units=%u)",
		      name, before.decode_units);

		/* The teardown itself. */
		aius_pipeline_reset(p);
		aius_pipeline_get_service_diag(p, &after);
		CHECK(after.au_abandons == 1u,
		      "teardown %s: staged partial compressed input was dropped "
		      "with %u abandons, not 1", name, after.au_abandons);
		CHECK(after.last_failure == AIUS_MALFORMED,
		      "teardown %s: the drop left no observation (last_failure=%d)",
		      name, (int)after.last_failure);
		CHECK(after.decode_units == before.decode_units,
		      "teardown %s: the discontinuity itself decoded something", name);
		/* Dropped ONCE: a second discontinuity over nothing counts nothing. */
		aius_pipeline_reset(p);
		aius_pipeline_get_service_diag(p, &after);
		CHECK(after.au_abandons == 1u,
		      "teardown %s: a second reset re-counted the same drop (%u)",
		      name, after.au_abandons);

		/* PCM after the teardown, on its ordinary confirmation schedule. */
		drive_deferred(p, ramp, RAMP_WORDS);
		aius_pipeline_get_service_diag(p, &final);
		CHECK(final.decode_units == before.decode_units,
		      "teardown %s: the dead AU was decoded %u times after the "
		      "discontinuity -- it must be decoded none", name,
		      final.decode_units - before.decode_units);
		CHECK(final.decode_rejects == 0u,
		      "teardown %s: %u decode rejects -- a dead AU still reached a "
		      "decoder", name, final.decode_rejects);
		CHECK(final.au_abandons == 1u,
		      "teardown %s: au_abandons %u != 1", name, final.au_abandons);
		CHECK(cap.n == blocks_before + 2,
		      "teardown %s: %d blocks after the teardown != 2 (the PCM "
		      "recovery alone)", name, cap.n - blocks_before);
		if (cap.n == blocks_before + 2)
			CHECK(cap.blocks[blocks_before].channels == 2u &&
			              cap.blocks[blocks_before].planes[0][0] ==
			                      pcm_expected_sample(ramp[CONFIRM]),
			      "teardown %s: the recovered PCM does not start at the "
			      "confirm boundary word", name);
		CHECK(aius_pipeline_get_squelch_diag(p, &squelched, &confirmed) ==
		                      AIUS_OK &&
		              confirmed == 1,
		      "teardown %s: PCM did not recover", name);
		/* The operator-owned latch is untouched by this change:
		 * aius_pipeline_reset clears last_status by contract, and the
		 * observation the drop leaves rides last_failure instead. */
		CHECK(aius_pipeline_get_metrics(p, &metrics) == AIUS_OK &&
		              metrics.last_status == AIUS_OK,
		      "teardown %s: reset stopped clearing the observation latch",
		      name);
		CHECK(final.last_failure == AIUS_MALFORMED,
		      "teardown %s: the drop's observation was erased by recovery",
		      name);
		capture_free(&cap);
		aius_pipeline_destroy(p);
	}

	/* (c) THE RESIDUAL, pinned so it cannot grow. With NO teardown signal the
	 * words after the cut still complete the dead burst to its declared
	 * length, and it reaches a decoder exactly once as an access unit. That
	 * remaining exposure is a target-wiring question (how promptly carrier
	 * loss bumps the ingress source generation), not a pipeline one -- but it
	 * is bounded here so a regression that turns one attempt into a grind
	 * fails. */
	{
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "teardown %s: bare create", name);
		if (p) {
			drive_deferred(p, wire, n);
			drive_deferred(p, wire, cut);
			aius_pipeline_get_service_diag(p, &before);
			drive_deferred(p, long_ramp, LONG_RAMP_WORDS);
			aius_pipeline_get_service_diag(p, &final);
			CHECK(final.decode_units - before.decode_units <= au_blocks,
			      "teardown %s: the dead AU took %u decode units without a "
			      "teardown -- more than one access unit's worth", name,
			      final.decode_units - before.decode_units);
			CHECK(final.au_abandons + final.decode_rejects >= 1u ||
			              final.decode_units == before.decode_units,
			      "teardown %s: the dead AU was neither rejected nor "
			      "abandoned", name);
			capture_free(&cap);
			aius_pipeline_destroy(p);
		}
	}

	printf("  teardown-%-6s mid-burst teardown drops the partial AU once "
	       "(1 abandon, 0 decode attempts)\n",
	       name[0] == 'a' ? "ac3" : "dts");
	free(long_ramp);
	free(ramp);
	free(wire);
}

/* ---- a carrier displacing a COMPRESSED replay counts its cut burst once ----
 *
 * Open-work register, 2026-09-03: `au_abandons` +1 with `last_failure`
 * MALFORMED at every carrier return during a `--with-replay` gate-4 run, filed
 * as a false latch on a "PCM replay". The replay is not PCM: `channel_id_51`
 * resolves to `/aius/replays/channel_id_51.iec`, the DTS Core rendering, so the
 * carrier displaces a real IEC 61937 bitstream at an arbitrary word and the
 * defect-35 accounting above counts the burst it cut. These legs pin the
 * boundary that makes that reading honest rather than noisy: a discontinuity
 * counts exactly one abandon when -- and only when -- the parser is past a
 * confirmed Pa/Pb with header or payload outstanding. It counts nothing over
 * inter-burst stuffing, over a PCM-classified stream carrying a false Pa, over
 * an undecided staging window that ends in a lone Pa, or over a bare Pa. The
 * parser sees no word at all in the PCM and UNKNOWN classes, so those legs also
 * pin that a reset over them cannot latch MALFORMED. */
static void scrub_sync_words(uint16_t *w, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (w[i] == 0xF872u || w[i] == 0x72F8u || w[i] == 0x4E1Fu ||
		    w[i] == 0x1F4Eu)
			w[i] ^= 1u;
	}
}

static void check_reset_counts(AiusPipeline *p, uint32_t expected_abandons,
                               const char *leg)
{
	AiusPipelineServiceDiag diag;
	aius_pipeline_reset(p);
	aius_pipeline_get_service_diag(p, &diag);
	CHECK(diag.au_abandons == expected_abandons,
	      "cut-burst %s: reset counted %u abandons, not %u", leg,
	      diag.au_abandons, expected_abandons);
	CHECK(diag.last_failure ==
	              (expected_abandons ? AIUS_MALFORMED : AIUS_OK),
	      "cut-burst %s: last_failure %d after the reset", leg,
	      (int)diag.last_failure);
	CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_UNKNOWN,
	      "cut-burst %s: reset left the class decided", leg);
}

static void test_discontinuity_counts_only_committed_bursts(const char *dir)
{
	enum {
		CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS,
		BLOCK_WORDS = 2 * (int)AIUS_PIPELINE_PCM_BLOCK_FRAMES,
		RAMP_WORDS = CONFIRM + 2 * BLOCK_WORDS,
		STUFFING_WORDS = 256,
		STAGED_WORDS = 300 /* < AIUS_PIPELINE_CLASSIFY_MAX_WORDS: undecided */
	};
	size_t n = 0u;
	uint16_t *wire = load_wire(dir, "dts_be16", &n);
	if (!wire) {
		CHECK(0, "cut-burst: corpus load");
		return;
	}
	CHECK(n > 4u && wire[0] == 0xF872u && wire[1] == 0x4E1Fu,
	      "cut-burst: dts_be16 does not open with a big-endian Pa/Pb");
	struct capture cap;
	AiusPcmSink sink = { .ctx = &cap, .consume = capture_consume };
	AiusPipelineServiceDiag diag;

	/* (a) COMMITTED: every cut past the confirmed Pa/Pb -- header still
	 * outstanding (2, 3), payload just begun (4), mid-payload, and one word
	 * short of completion -- is one abandon, exactly like the live carrier
	 * return landing anywhere inside the replay's burst. */
	const size_t cuts[] = { 2u, 3u, 4u, n / 2u, n - 1u };
	for (size_t c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
		char leg[32];
		snprintf(leg, sizeof(leg), "cut@%zu", cuts[c]);
		memset(&cap, 0, sizeof(cap));
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "cut-burst %s: create", leg);
		if (!p)
			continue;
		drive_deferred(p, wire, cuts[c]);
		aius_pipeline_get_service_diag(p, &diag);
		CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_DTS &&
		              diag.decode_units == 0u && diag.au_abandons == 0u,
		      "cut-burst %s: the cut prefix was not a staged burst "
		      "(class=%d units=%u abandons=%u)", leg,
		      (int)aius_pipeline_classification(p), diag.decode_units,
		      diag.au_abandons);
		check_reset_counts(p, 1u, leg);
		aius_pipeline_get_service_diag(p, &diag);
		CHECK(diag.decode_units == 0u && diag.decode_rejects == 0u,
		      "cut-burst %s: the discontinuity decoded something", leg);
		capture_free(&cap);
		aius_pipeline_destroy(p);
	}

	/* (b) INTER-BURST: the whole burst decoded, then the source cut inside
	 * the zero stuffing before the next Pa. Nothing is staged; nothing
	 * counts. */
	{
		uint16_t stuffing[STUFFING_WORDS];
		memset(stuffing, 0, sizeof(stuffing));
		memset(&cap, 0, sizeof(cap));
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "cut-burst stuffing: create");
		if (p) {
			drive_deferred(p, wire, n);
			drive_deferred(p, stuffing, STUFFING_WORDS);
			aius_pipeline_get_service_diag(p, &diag);
			CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_DTS &&
			              diag.decode_units == 1u && diag.au_abandons == 0u,
			      "cut-burst stuffing: the whole burst did not decode "
			      "(class=%d units=%u abandons=%u)",
			      (int)aius_pipeline_classification(p), diag.decode_units,
			      diag.au_abandons);
			check_reset_counts(p, 0u, "stuffing");
			capture_free(&cap);
			aius_pipeline_destroy(p);
		}
	}

	/* (c) PCM: a confirmed linear-PCM run carrying a lone false Pa inside
	 * the squelch window and another in the emitted region. The re-type
	 * detector sees no pair, the parser sees no word; the reset counts
	 * nothing and latches nothing. */
	{
		uint16_t *ramp = make_ramp(RAMP_WORDS);
		scrub_sync_words(ramp, RAMP_WORDS);
		ramp[10] = 0xF872u;
		ramp[CONFIRM + 5] = 0xF872u;
		memset(&cap, 0, sizeof(cap));
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "cut-burst pcm: create");
		if (p) {
			drive_deferred(p, ramp, RAMP_WORDS);
			aius_pipeline_get_service_diag(p, &diag);
			CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_PCM &&
			              cap.n == 2 && diag.au_abandons == 0u &&
			              diag.last_failure == AIUS_OK,
			      "cut-burst pcm: the ramp did not play as PCM "
			      "(class=%d blocks=%d abandons=%u last=%d)",
			      (int)aius_pipeline_classification(p), cap.n,
			      diag.au_abandons, (int)diag.last_failure);
			check_reset_counts(p, 0u, "pcm");
			capture_free(&cap);
			aius_pipeline_destroy(p);
		}
		free(ramp);
	}

	/* (d) UNDECIDED: fewer than a classify window of words, the last of them
	 * a Pa with no Pb behind it yet; and (e) a bare Pa. Neither is a
	 * confirmed sync, so neither is staged compressed input. */
	{
		uint16_t *staged = make_ramp(STAGED_WORDS);
		scrub_sync_words(staged, STAGED_WORDS);
		staged[STAGED_WORDS - 1] = 0xF872u;
		const uint16_t bare_pa = 0xF872u;
		memset(&cap, 0, sizeof(cap));
		AiusPipeline *p = NULL;
		CHECK(aius_pipeline_create(&p, NULL, &sink) == AIUS_OK,
		      "cut-burst undecided: create");
		if (p) {
			drive_deferred(p, staged, STAGED_WORDS);
			CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_UNKNOWN,
			      "cut-burst undecided: %d words decided the class",
			      STAGED_WORDS);
			check_reset_counts(p, 0u, "undecided");
			drive_deferred(p, &bare_pa, 1u);
			CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_UNKNOWN,
			      "cut-burst bare-pa: one word decided the class");
			check_reset_counts(p, 0u, "bare-pa");
			capture_free(&cap);
			aius_pipeline_destroy(p);
		}
		free(staged);
	}

	printf("  cut-burst      a discontinuity counts one abandon only past a "
	       "confirmed Pa/Pb (5 cuts); none over stuffing, PCM, undecided\n");
	free(wire);
}

/* ---- defect 34: a type-21 carrier entered at any phase stays dark ----------
 *
 * E-AC-3's IEC 61937 repetition period is 12,288 words at the 6-block burst
 * shape, wider than AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS (8,192): the first
 * candidate pair always expires before the second arrives, so the two-pair
 * rule can NEVER re-type such a link. Every entry phase outside the 512-word
 * classify window -- ~96% of them -- typed PCM, cleared the 8,192-word
 * confirmation squelch, and played compressed burst data as full-scale noise.
 *
 * The fix leaves PCM on the FIRST pair whose Pc completes a well-formed
 * header for a type outside {1,11,12,13}. Since phase 3 type 21 is decodable,
 * so the exit's outcome forks on what the DECODER makes of the burst:
 *
 *   - REFUSED DECODE (this sweep): the carrier is the synthetic
 *     unsupported_eac3 burst -- a valid 48 kHz 5.1 E-AC-3 header over
 *     garbage -- so the arena pipeline creates the E-AC-3 residency (the
 *     phase-4 slot holds it) and then every decode fails deterministically
 *     at frame parse, MALFORMED in the service diag. The dark guarantees
 *     below are exactly defect 34's, unchanged: refused-payload links
 *     stay silent at every entry phase.
 *   - DECODE (the following sweep): a heap-path pipeline decodes the real
 *     carrier from every entry phase instead of going dark.
 *
 * What dark buys, and its exact limit:
 *
 *   - NO BURST PAYLOAD EVER REACHES THE SINK, at any entry phase. That is the
 *     harm, and it is closed outright.
 *   - The stream never ENDS in a confirmed PCM class at any phase.
 *   - For every phase whose first preamble lands at or before the confirmation
 *     boundary -- two thirds of the period -- not one block is emitted.
 *   - The remaining third emits up to four blocks of the carrier's inter-burst
 *     STUFFING (digital silence) before the preamble arrives. That residual is
 *     not removable while genuine PCM must still confirm at run word 8,192
 *     unchanged: over those phases the carrier's first 8,192 words are literal
 *     zeros, so the two streams are indistinguishable at the boundary. Raising
 *     the confirmation would fix it and lengthen every real PCM source's mute,
 *     which is the trade this fix exists to avoid.
 */

#define EAC3_PERIOD_WORDS 12288u

/* One repetition period of a type-21 carrier: the committed unsupported_eac3
 * burst, then IEC 61937 stuffing (zeros) out to the 12,288-word period. */
static uint16_t *make_eac3_carrier(const char *dir, size_t periods,
                                   size_t *out_n, size_t *out_burst_words)
{
	size_t n = 0u;
	uint16_t *burst = load_wire(dir, "unsupported_eac3", &n);
	if (!burst)
		return NULL;
	CHECK(n < EAC3_PERIOD_WORDS,
	      "eac3-entry: burst is %zu words, wider than the 12,288-word period",
	      n);
	const size_t total = EAC3_PERIOD_WORDS * periods;
	uint16_t *s = calloc(total, sizeof(uint16_t));
	for (size_t r = 0; r < periods; r++)
		memcpy(s + r * EAC3_PERIOD_WORDS, burst, n * sizeof(uint16_t));
	free(burst);
	*out_n = total;
	*out_burst_words = n;
	return s;
}

/* Push without finish(): the target never calls finish, and a phase that ends
 * mid-burst would report a truncation that says nothing about entry. */
static AiusPipeline *run_entry(const uint16_t *w, size_t n, struct capture *cap,
                               struct reclass_diag *d, const char *label)
{
	AiusPcmSink sink = { .ctx = cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	memset(d, 0, sizeof(*d));
	if (aius_pipeline_create(&p, NULL, &sink) != AIUS_OK) {
		CHECK(0, "%s: create", label);
		return NULL;
	}
	push_chunked(p, w, n, 256, label);
	CHECK(aius_pipeline_get_reclass_counters(p, &d->retype, &d->reclassify) ==
	              AIUS_OK,
	      "%s: counters", label);
	CHECK(aius_pipeline_get_squelch_diag(p, &d->squelched_frames,
	                                     &d->pcm_confirmed) == AIUS_OK,
	      "%s: squelch diag", label);
	return p;
}

/* The arena-backed variant: the target shape, whose 64-KiB codec slot cannot
 * hold the E-AC-3 decoder -- the deterministic dispatch refusal the dark
 * sweep needs. Reuses g_pipe_arena; callers sequence pipelines. */
static AiusPipeline *run_entry_arena(const uint16_t *w, size_t n,
                                     struct capture *cap,
                                     struct reclass_diag *d, const char *label)
{
	AiusArena arena;
	if (aius_arena_init(&arena, g_pipe_arena, sizeof(g_pipe_arena)) != 0) {
		CHECK(0, "%s: arena init", label);
		return NULL;
	}
	AiusPcmSink sink = { .ctx = cap, .consume = capture_consume };
	AiusPipeline *p = NULL;
	memset(d, 0, sizeof(*d));
	if (aius_pipeline_create_arena(&p, NULL, &sink, &arena) != AIUS_OK) {
		CHECK(0, "%s: create_arena", label);
		return NULL;
	}
	push_chunked(p, w, n, 256, label);
	CHECK(aius_pipeline_get_reclass_counters(p, &d->retype, &d->reclassify) ==
	              AIUS_OK,
	      "%s: counters", label);
	CHECK(aius_pipeline_get_squelch_diag(p, &d->squelched_frames,
	                                     &d->pcm_confirmed) == AIUS_OK,
	      "%s: squelch diag", label);
	return p;
}

static void test_unsupported_entry_phase_sweep(const char *dir)
{
	enum { CONFIRM = (int)AIUS_PIPELINE_PCM_CONFIRM_WORDS, PERIODS = 4 };
	size_t carrier_n = 0u;
	size_t burst_words = 0u;
	uint16_t *carrier = make_eac3_carrier(dir, PERIODS, &carrier_n,
	                                      &burst_words);
	if (!carrier) {
		CHECK(0, "eac3-entry: carrier build");
		return;
	}
	/* A ramp long enough that every swept offset still has the whole
	 * confirmation window plus two blocks behind it. */
	const size_t ramp_n = carrier_n;
	uint16_t *ramp = make_ramp(ramp_n);

	/* The preamble words are the only non-stuffing words the pass-through can
	 * ever stage before the dark exit consumes the Pc; anything else emitted
	 * would be burst payload. */
	const int32_t pa_sample = pcm_expected_sample(0xF872u);
	const int32_t pb_sample = pcm_expected_sample(0x4E1Fu);

	/* Stride across the full period, plus every phase where the geometry
	 * changes: the classify window's edges, the confirmation boundary, and
	 * the period's own ends. */
	static const size_t explicit_phases[] = {
		0, 1, 2, 511, 512, 513, 1024, 3072, 4094, 4095, 4096, 4097,
		8191, 8192, 11775, 11776, 11777, 12286, 12287
	};
	size_t phases[256];
	size_t phase_count = 0u;
	for (size_t i = 0; i < sizeof(explicit_phases) / sizeof(explicit_phases[0]);
	     i++)
		phases[phase_count++] = explicit_phases[i];
	for (size_t phi = 0; phi < EAC3_PERIOD_WORDS; phi += 128u)
		phases[phase_count++] = phi;

	int dark_at_every_phase = 1;
	int payload_reached_sink = 0;
	int silent_entry_phases = 0;
	int worst_blocks = 0;
	int pcm_timing_ok = 1;

	for (size_t k = 0; k < phase_count; k++) {
		const size_t phi = phases[k];
		/* The run word at which this entry phase first meets a preamble. */
		const size_t first_sync =
			phi == 0u ? 0u : (EAC3_PERIOD_WORDS - phi);

		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipeline *p = run_entry_arena(carrier + phi, carrier_n - phi,
		                                  &cap, &d, "eac3-entry");
		if (!p)
			break;
		const AiusPipelineClass klass = aius_pipeline_classification(p);
		AiusIecDiag parser;
		CHECK(aius_pipeline_get_parser_diag(p, &parser) == AIUS_OK,
		      "eac3-entry: parser diag");

		if (klass == AIUS_PIPELINE_PCM || d.pcm_confirmed != 0) {
			CHECK(0,
			      "eac3-entry phase %zu: ended class %d confirmed %d -- a "
			      "type-21 carrier confirmed as PCM", phi, (int)klass,
			      d.pcm_confirmed);
			dark_at_every_phase = 0;
		}
		/* The parser frames the carrier's bursts (in scope since phase 3)
		 * and the DECODER refuses each one -- since the phase-4 slot
		 * growth the arena creates the E-AC-3 residency, and this
		 * carrier's valid-header-over-garbage payload then fails frame
		 * parse deterministically -- which is what holds the class dark
		 * without starving. */
		CHECK(parser.bursts_ok >= (uint32_t)(PERIODS - 2) &&
		              parser.rejects_unsupported == 0u,
		      "eac3-entry phase %zu: parser framed %u ok / %u unsupported "
		      "bursts", phi, parser.bursts_ok, parser.rejects_unsupported);
		{
			AiusPipelineServiceDiag sd;
			aius_pipeline_get_service_diag(p, &sd);
			CHECK(sd.decode_rejects >= (uint32_t)(PERIODS - 2) &&
			              sd.last_failure == AIUS_MALFORMED &&
			              sd.codec_swaps == 0u,
			      "eac3-entry phase %zu: decode refusal diag "
			      "(%u rejects, last %d, %u swaps)", phi,
			      sd.decode_rejects, (int)sd.last_failure,
			      sd.codec_swaps);
			CHECK(aius_pipeline_resident_codec(p) == 21u,
			      "eac3-entry phase %zu: the E-AC-3 residency was not "
			      "created (resident %u)", phi,
			      aius_pipeline_resident_codec(p));
		}
		CHECK(d.reclassify == 0u,
		      "eac3-entry phase %zu: the dark class starved back out (%u)",
		      phi, d.reclassify);

		for (int b = 0; b < cap.n; b++) {
			for (uint32_t f = 0; f < cap.blocks[b].frames; f++) {
				for (int ch = 0; ch < cap.blocks[b].channels; ch++) {
					const int32_t s = cap.blocks[b].planes[ch][f];
					if (s != 0 && s != pa_sample && s != pb_sample)
						payload_reached_sink = 1;
				}
			}
		}
		if (cap.n > worst_blocks)
			worst_blocks = cap.n;
		if (first_sync <= (size_t)CONFIRM) {
			CHECK(cap.n == 0,
			      "eac3-entry phase %zu: %d blocks emitted although the "
			      "first preamble (run word %zu) lands inside the "
			      "confirmation window", phi, cap.n, first_sync);
			if (cap.n == 0)
				silent_entry_phases++;
		} else {
			/* The bound is a FRAME budget (how much stuffing silence may
			 * reach the sink before the preamble re-types the run), not a
			 * block count: at the historical 512-frame PCM block the limit
			 * was 4 blocks = 2048 frames, and shrinking the block
			 * (output-jitter Phase E) slices the same span into more,
			 * smaller blocks without widening the leak. */
			CHECK((uint32_t)cap.n * AIUS_PIPELINE_PCM_BLOCK_FRAMES <=
			              2048u,
			      "eac3-entry phase %zu: %d stuffing blocks (%u frames) "
			      "emitted before the preamble at run word %zu", phi, cap.n,
			      (uint32_t)cap.n * AIUS_PIPELINE_PCM_BLOCK_FRAMES,
			      first_sync);
		}
		capture_free(&cap);
		aius_pipeline_destroy(p);

		/* The control that makes the fix's cost explicit: genuine PCM
		 * entered at the SAME offset still confirms at run word CONFIRM
		 * exactly, first frame the boundary word pair, L-first. */
		struct capture pcm_cap;
		memset(&pcm_cap, 0, sizeof(pcm_cap));
		struct reclass_diag pd;
		const size_t pcm_words =
			(size_t)CONFIRM + 2u * 2u * AIUS_PIPELINE_PCM_BLOCK_FRAMES;
		AiusPipeline *q = run_entry(ramp + phi, pcm_words, &pcm_cap, &pd,
		                            "eac3-entry-pcm");
		if (q) {
			if (aius_pipeline_classification(q) != AIUS_PIPELINE_PCM ||
			    pd.pcm_confirmed != 1 || pcm_cap.n != 2 ||
			    pcm_cap.blocks[0].planes[0][0] !=
			            pcm_expected_sample(ramp[phi + CONFIRM]) ||
			    pcm_cap.blocks[0].planes[1][0] !=
			            pcm_expected_sample(ramp[phi + CONFIRM + 1]) ||
			    pd.squelched_frames != (uint32_t)CONFIRM / 2u) {
				CHECK(0,
				      "eac3-entry phase %zu: genuine PCM entry changed "
				      "(class %d confirmed %d blocks %d squelched %u)",
				      phi, (int)aius_pipeline_classification(q),
				      pd.pcm_confirmed, pcm_cap.n, pd.squelched_frames);
				pcm_timing_ok = 0;
			}
			capture_free(&pcm_cap);
			aius_pipeline_destroy(q);
		}
	}

	CHECK(!payload_reached_sink,
	      "eac3-entry: burst payload reached the sink -- the full-scale noise "
	      "defect 34 describes is still reachable");
	CHECK(dark_at_every_phase,
	      "eac3-entry: at least one entry phase confirmed PCM on a type-21 "
	      "carrier");
	CHECK(pcm_timing_ok,
	      "eac3-entry: genuine PCM confirmation timing changed");
	printf("  eac3-entry     %zu entry phases over the 12,288-word period: no "
	       "burst payload emitted, none confirms PCM (%d fully silent, worst "
	       "%d stuffing blocks)\n",
	       phase_count, silent_entry_phases, worst_blocks);
	free(ramp);
	free(carrier);
}

/* ---- phase 3: the same carrier geometry, now decodable ---------------------
 *
 * The positive half of the defect-34 story: a heap-path pipeline (no slot
 * bound) meets a REAL type-21 carrier -- the committed eac3_surround51_ident
 * burst tiled at its exact 12,288-word period -- at swept entry phases, and
 * must lock and DECODE from every one of them instead of going dark. Phases
 * that type PCM first may emit up to four blocks of confirmed inter-burst
 * stuffing (2-ch digital silence, the sweep above's documented residual);
 * no compressed payload may ever surface, and every decoded block is the
 * 6-channel 512-frame 5.1 shape. */

static uint16_t *make_real_eac3_carrier(const char *dir, size_t periods,
                                        size_t *out_n)
{
	size_t n = 0u, plen = 0u;
	uint16_t *wire = load_wire(dir, "eac3_surround51_ident", &n);
	uint8_t *pay = load_pay(dir, "eac3_surround51_ident", &plen);
	if (!wire || !pay) {
		free(wire);
		free(pay);
		return NULL;
	}
	const size_t burst_words = 4u + (plen + 1u) / 2u;
	CHECK(n >= burst_words && wire[0] == 0xF872u,
	      "eac3-decode: unexpected corpus vector shape");
	const size_t total = EAC3_PERIOD_WORDS * periods;
	uint16_t *s = calloc(total, sizeof(uint16_t));
	for (size_t r = 0; r < periods; r++)
		memcpy(s + r * EAC3_PERIOD_WORDS, wire,
		       burst_words * sizeof(uint16_t));
	free(wire);
	free(pay);
	*out_n = total;
	return s;
}

static void test_eac3_decode_entry_phase_sweep(const char *dir)
{
	enum { PERIODS = 4 };
	size_t carrier_n = 0u;
	uint16_t *carrier = make_real_eac3_carrier(dir, PERIODS, &carrier_n);
	if (!carrier) {
		CHECK(0, "eac3-decode: carrier build");
		return;
	}

	static const size_t explicit_phases[] = {
		0, 1, 2, 511, 512, 513, 771, 772, 773, 4095, 4096, 8191, 8192,
		8193, 11776, 12287
	};
	size_t phases[32];
	size_t phase_count = 0u;
	for (size_t i = 0;
	     i < sizeof(explicit_phases) / sizeof(explicit_phases[0]); i++)
		phases[phase_count++] = explicit_phases[i];
	for (size_t phi = 1024u; phi < EAC3_PERIOD_WORDS; phi += 1024u)
		phases[phase_count++] = phi;

	/* A confirmed-stuffing block boundary can land ON the next burst's
	 * preamble: the Pa/Pb words then stage as that block's final samples
	 * before the Pc-triggered exit drops the rest -- the same membership
	 * the dark sweep allows. Anything else non-zero is payload. */
	const int32_t pa_sample = pcm_expected_sample(0xF872u);
	const int32_t pb_sample = pcm_expected_sample(0x4E1Fu);

	int decoded_everywhere = 1;
	for (size_t k = 0; k < phase_count; k++) {
		const size_t phi = phases[k];
		struct capture cap;
		memset(&cap, 0, sizeof(cap));
		struct reclass_diag d;
		AiusPipeline *p = run_entry(carrier + phi, carrier_n - phi, &cap,
		                            &d, "eac3-decode");
		if (!p)
			break;
		CHECK(aius_pipeline_classification(p) == AIUS_PIPELINE_DTS &&
		              d.pcm_confirmed == 0,
		      "eac3-decode phase %zu: class %d confirmed %d", phi,
		      (int)aius_pipeline_classification(p), d.pcm_confirmed);
		CHECK(d.reclassify == 0u, "eac3-decode phase %zu: starved (%u)",
		      phi, d.reclassify);
		CHECK(aius_pipeline_codec_pc(p) == 21u &&
		              aius_pipeline_resident_codec(p) == 21u,
		      "eac3-decode phase %zu: codec pc %u resident %u", phi,
		      aius_pipeline_codec_pc(p), aius_pipeline_resident_codec(p));

		const uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
		                      AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR;
		int decoded = 0;
		int in_decoded_run = 0;
		int shape_ok = 1;
		for (int b = 0; b < cap.n; b++) {
			if (cap.blocks[b].channels == 2u && !in_decoded_run) {
				/* Leading confirmed stuffing: silence, or the
				 * boundary-straddling preamble words. */
				for (uint32_t f = 0; f < cap.blocks[b].frames; f++) {
					const int32_t l = cap.blocks[b].planes[0][f];
					const int32_t r = cap.blocks[b].planes[1][f];
					if ((l != 0 && l != pa_sample &&
					     l != pb_sample) ||
					    (r != 0 && r != pa_sample &&
					     r != pb_sample))
						shape_ok = 0;
				}
				continue;
			}
			in_decoded_run = 1;
			if (cap.blocks[b].channels != 6u ||
			    cap.blocks[b].frames != 512u ||
			    cap.blocks[b].sample_rate_hz != 48000u ||
			    cap.blocks[b].channel_mask != mask)
				shape_ok = 0;
			decoded++;
		}
		CHECK(shape_ok,
		      "eac3-decode phase %zu: non-silent stuffing or mis-shaped "
		      "decoded block", phi);
		if (decoded < 3 * (PERIODS - 1)) {
			CHECK(0, "eac3-decode phase %zu: %d decoded blocks < %d",
			      phi, decoded, 3 * (PERIODS - 1));
			decoded_everywhere = 0;
		}
		capture_free(&cap);
		aius_pipeline_destroy(p);
	}
	CHECK(decoded_everywhere,
	      "eac3-decode: some entry phase failed to decode");
	printf("  eac3-decode    %zu entry phases over the period: every phase "
	       "locks and decodes 5.1\n", phase_count);
	free(carrier);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <iec_corpus_dir>\n", argv[0]);
		return 2;
	}
	const char *dir = argv[1];

	printf("Aius S/PDIF pipeline test (classify + IEC 61937 -> codecs)\n");
	test_dts_equivalence(dir, "dts_be16");
	test_dts_equivalence(dir, "dts51_be16");
	test_ac3_dispatch_and_identity(dir);
	test_eac3_dispatch_and_identity(dir);
	test_pcm_passthrough();
	test_pcm_runtime_rates();
	test_pcm_sample_scale();
	test_classifier(dir);
	test_no_heap_steady_state(dir);
	test_codec_slot_swap(dir);
	test_eac3_arena_residency(dir);
	test_retype_mid_burst_entry(dir);
	test_retype_mid_stream_switch(dir);
	test_retype_false_positive_immunity();
	test_worst_phase_confirm_bound();
	test_retype_boundary_pair(dir);
	test_starve_and_recover(dir);
	test_unsupported_never_starves(dir);
	test_unsupported_entry_phase_sweep(dir);
	test_eac3_decode_entry_phase_sweep(dir);
	test_teardown_drops_partial_burst(dir, "dts_be16", 500u, 1u, 1u);
	test_teardown_drops_partial_burst(dir, "ac3_surround51_ident", 700u, 12u,
	                                  12u);
	test_discontinuity_counts_only_committed_bursts(dir);
	test_reset_semantics(dir);
	test_deferred_dts(dir);
	test_deferred_ac3(dir);
	test_codec_lifecycle(dir);
	test_clear_drops_observation_latches(dir);
	test_output_slicing();
	test_deferred_pcm_and_legacy_loss();

	if (g_fail) {
		printf("RESULT: %d check(s) FAILED\n", g_fail);
		return 1;
	}
	printf("RESULT: all checks passed\n");
	return 0;
}
