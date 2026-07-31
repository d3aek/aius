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
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK &&
	          m.decode_cycles == 0u && m.stack_high_water == 0u &&
	          m.alloc_count == 0u && m.arena_high_water > 0u &&
	          m.last_status == AIUS_OK,
	      "ss: unknown compatibility metrics are not zero-operation");
	CHECK(aius_pipeline_get_codec_metrics(p, 21u, &m) == AIUS_INVALID_ARG,
	      "ss: unsupported codec metrics Pc accepted");

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

	CHECK(aius_pipeline_push(p, ac3, ac3_n) == AIUS_OK, "ss: AC-3 push");
	CHECK(aius_pipeline_codec_pc(p) == 1u, "ss: AC-3 codec Pc not selected");
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
		w[PAIR] = RAMP_PA;
		w[PAIR + 1] = RAMP_PB;
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
		w[PAIR] = RAMP_PA;
		w[PAIR + 1] = RAMP_PB;
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
		w[1000] = RAMP_PA;
		w[1001] = RAMP_PB;
		w[1100] = RAMP_PA;
		w[1101] = RAMP_PB;
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
		w[600] = RAMP_PA;
		w[601] = RAMP_PB;
		w[16600] = RAMP_PA;
		w[16601] = RAMP_PB;
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
		w[1000] = RAMP_PA;
		w[1001] = RAMP_PB;
		w[2000] = 0x72F8u; /* LE Pa */
		w[2001] = 0x1F4Eu; /* LE Pb */
		w[2600] = 0x72F8u;
		w[2601] = 0x1F4Eu;
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
	w[FIRST_PA] = RAMP_PA;
	w[FIRST_PA + 1] = RAMP_PB;
	w[FIRST_PA + SPACING] = RAMP_PA;
	w[FIRST_PA + SPACING + 1] = RAMP_PB;

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

/* T6: framed-but-unsupported never starves. Repeated E-AC-3 bursts are framing
 * evidence, so the class must hold bitstream in silence rather than decay into
 * PCM noise. Fails if the starvation counter resets on AIUS_OK only. */
static void test_unsupported_never_starves(const char *dir)
{
	size_t n = 0, sn = 0;
	uint16_t *v = load_wire(dir, "unsupported_eac3", &n);
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
	printf("  unsupported    E-AC-3 bursts hold bitstream class, no starve\n");
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
	r1[550] = RAMP_PA;
	r1[551] = RAMP_PB;
	push_chunked(p, r1, 700, 256, "reset-sem-b1");
	aius_pipeline_reset(p);
	uint16_t *r2 = make_ramp(2000);
	r2[1550] = RAMP_PA;
	r2[1551] = RAMP_PB;
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
	test_pcm_passthrough();
	test_pcm_runtime_rates();
	test_pcm_sample_scale();
	test_classifier(dir);
	test_no_heap_steady_state(dir);
	test_retype_mid_burst_entry(dir);
	test_retype_mid_stream_switch(dir);
	test_retype_false_positive_immunity();
	test_worst_phase_confirm_bound();
	test_retype_boundary_pair(dir);
	test_starve_and_recover(dir);
	test_unsupported_never_starves(dir);
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
