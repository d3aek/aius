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
 *      pass-through. See aius_codec.h §SAMPLE SCALE.
 *   4. No-heap steady state. Two back-to-back DTS bursts decode with a non-zero
 *      warmup allocation then zero allocations on the steady-state frame (via the
 *      --wrap alloc shim + decoder metrics).
 *
 * Usage: test_pipeline <iec_corpus_dir>
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void test_pcm_passthrough(void)
{
	/* 1024 frames of synthetic interleaved L,R 16-bit PCM, no IEC preamble. */
	enum { FRAMES = 1024, WORDS = FRAMES * 2 };
	uint16_t *w = malloc(WORDS * sizeof(uint16_t));
	for (int i = 0; i < WORDS; i++)
		w[i] = (uint16_t)(int16_t)((i * 131 + 7) & 0xFFFF);

	struct capture cap;
	memset(&cap, 0, sizeof(cap));
	AiusPipelineClass klass = run_pipeline(w, WORDS, WORDS, &cap);
	CHECK(klass == AIUS_PIPELINE_PCM, "pcm: class %d != PCM", klass);

	/* Reassemble captured planes and compare sample-for-sample. */
	uint32_t got = 0;
	int ok = 1;
	for (int b = 0; b < cap.n && ok; b++) {
		struct block_copy *bc = &cap.blocks[b];
		if (bc->channels != 2 || bc->sample_rate_hz != 48000 ||
		    bc->channel_mask != (uint32_t)(AIUS_CH_FL | AIUS_CH_FR)) {
			ok = 0;
			break;
		}
		for (uint32_t f = 0; f < bc->frames; f++) {
			int32_t expl = pcm_expected_sample(w[2 * (got + f)]);
			int32_t expr = pcm_expected_sample(w[2 * (got + f) + 1]);
			if (bc->planes[0][f] != expl || bc->planes[1][f] != expr) {
				ok = 0;
				break;
			}
		}
		got += bc->frames;
	}
	CHECK(ok, "pcm: pass-through samples differ from input");
	CHECK(got == FRAMES, "pcm: recovered %u frames != %u", got, (unsigned)FRAMES);
	printf("  pcm            PCM pass-through 2ch/48k, %u frames byte-exact\n", got);

	capture_free(&cap);
	free(w);
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
	 * each lands on a known plane. Long enough to classify as PCM unaided. */
	enum { FRAMES = 512, WORDS = FRAMES * 2 };
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

/* Arena dedicated to the pipeline instance + DTS decoder. 5.1 core decode
 * high-water measures well under 256 KiB; the instance adds a few KiB. */
#define PIPE_ARENA_BYTES (256u * 1024u)
static uint8_t g_pipe_arena[PIPE_ARENA_BYTES] __attribute__((aligned(16)));

static void test_no_heap_steady_state(const char *dir)
{
	size_t n = 0;
	uint16_t *w = load_wire(dir, "dts_be16", &n);
	if (!w) {
		CHECK(0, "steady-state: corpus load failed");
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
	AiusPipeline *p = NULL;
	CHECK(aius_pipeline_create_arena(&p, NULL, &sink, &arena) == AIUS_OK,
	      "ss: create_arena");
	if (!p) {
		free(w);
		return;
	}

	AiusMetrics m;

	/* Warmup burst: allocates (proves the arena + alloc shim are wired). */
	CHECK(aius_pipeline_push(p, w, n) == AIUS_OK, "ss: warmup push");
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK, "ss: warmup metrics");
	CHECK(m.alloc_count > 0, "ss: warmup allocated 0 (arena/shim not wired?)");
	printf("  steady-state   warmup alloc_count=%u arena_high_water=%u bytes\n",
	       m.alloc_count, m.arena_high_water);

	/* Steady-state burst: zero allocations. */
	CHECK(aius_pipeline_push(p, w, n) == AIUS_OK, "ss: steady push");
	CHECK(aius_pipeline_get_metrics(p, &m) == AIUS_OK, "ss: steady metrics");
	CHECK(m.alloc_count == 0,
	      "ss: steady-state decode allocated %u (must be 0)", m.alloc_count);
	CHECK(cap.n == 2, "ss: emitted %d blocks != 2", cap.n);
	printf("  steady-state   2nd burst alloc_count=0 (no-heap steady state)\n");

	aius_pipeline_destroy(p);
	capture_free(&cap);
	free(w);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <iec_corpus_dir>\n", argv[0]);
		return 2;
	}
	const char *dir = argv[1];

	printf("Aius S/PDIF pipeline test (classify + IEC 61937 -> DTS)\n");
	test_dts_equivalence(dir, "dts_be16");
	test_dts_equivalence(dir, "dts51_be16");
	test_pcm_passthrough();
	test_pcm_sample_scale();
	test_classifier(dir);
	test_no_heap_steady_state(dir);

	if (g_fail) {
		printf("RESULT: %d check(s) FAILED\n", g_fail);
		return 1;
	}
	printf("RESULT: all checks passed\n");
	return 0;
}
