/* Host known-answer test for the Aius IEC 61937 parser (capability-gate P2).
 *
 * Drives the parser over the committed corpus (tools/iec_corpus) and asserts,
 * for every vector, the exact typed result and — for completed bursts — the
 * data type and canonical big-endian payload bytes. Each de-framing vector runs
 * under three chunkings (whole buffer, one word at a time, and an odd 3-word
 * stride) to prove arbitrary word-boundary reassembly. Also covers the argument
 * contract, recovery, truncation, and multi-burst draining in code.
 *
 * Usage: test_iec61937 <corpus_dir>
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_iec61937.h"

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

/* ---- corpus loading ------------------------------------------------------ */

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

/* Load a .pay expected-payload file (may be zero length). Returns NULL only on a
 * missing file; a zero-length file yields a valid non-NULL pointer, *len == 0. */
static uint8_t *load_pay(const char *dir, const char *name, size_t *len)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.pay", dir, name);
	size_t bytes = 0;
	uint8_t *raw = read_file(path, &bytes);
	if (!raw)
		return NULL;
	*len = bytes;
	return raw;
}

/* Push words[pos..] in `chunk`-sized pieces until a non-NEED_MORE result or the
 * input is exhausted. Advances *pos by the words consumed. */
static AiusStatus pump(AiusIec61937Parser *p, const uint16_t *words, size_t n,
                       size_t chunk, size_t *pos)
{
	while (*pos < n) {
		size_t give = n - *pos;
		if (give > chunk)
			give = chunk;
		size_t consumed = 0;
		AiusStatus st = aius_iec61937_push(p, words + *pos, give, &consumed);
		*pos += consumed;
		if (st != AIUS_NEED_MORE)
			return st;
	}
	return AIUS_NEED_MORE;
}

static const char *status_name(AiusStatus s)
{
	switch (s) {
	case AIUS_OK: return "OK";
	case AIUS_NEED_MORE: return "NEED_MORE";
	case AIUS_UNSUPPORTED: return "UNSUPPORTED";
	case AIUS_MALFORMED: return "MALFORMED";
	case AIUS_TRUNCATED: return "TRUNCATED";
	case AIUS_NO_SPACE: return "NO_SPACE";
	case AIUS_INVALID_ARG: return "INVALID_ARG";
	default: return "?";
	}
}

/* ---- de-framing vectors (one burst, payload compared) -------------------- */

struct kav {
	const char *name;
	AiusIec61937Packing packing;
	AiusStatus expect;   /* AIUS_OK or AIUS_UNSUPPORTED */
	uint16_t data_type;
};

static const struct kav k_burst[] = {
	{ "dts_be16", AIUS_IEC61937_PACK_16, AIUS_OK, 13 },
	{ "dts_le16", AIUS_IEC61937_PACK_16, AIUS_OK, 13 },
	{ "dts_14bit", AIUS_IEC61937_PACK_14, AIUS_OK, 13 },
	{ "dts51_be16", AIUS_IEC61937_PACK_16, AIUS_OK, 13 },
	{ "type11", AIUS_IEC61937_PACK_16, AIUS_OK, 11 },
	{ "type12", AIUS_IEC61937_PACK_16, AIUS_OK, 12 },
	{ "unsupported_ac3", AIUS_IEC61937_PACK_16, AIUS_UNSUPPORTED, 1 },
	{ "null_type", AIUS_IEC61937_PACK_16, AIUS_UNSUPPORTED, 0 },
	{ "padded", AIUS_IEC61937_PACK_16, AIUS_OK, 13 },
	{ "recover", AIUS_IEC61937_PACK_16, AIUS_OK, 13 },
};

static void run_burst_vector(const char *dir, const struct kav *v)
{
	size_t n = 0;
	uint16_t *w = load_wire(dir, v->name, &n);
	size_t plen = 0;
	uint8_t *pay = load_pay(dir, v->name, &plen);
	if (!w || !pay) {
		CHECK(0, "%s: corpus load failed", v->name);
		free(w);
		free(pay);
		return;
	}

	const size_t chunks[] = { n ? n : 1, 1, 3 };
	for (size_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
		AiusIec61937Config cfg = { .packing = v->packing };
		AiusIec61937Parser *p = NULL;
		CHECK(aius_iec61937_parser_create(&p, &cfg) == AIUS_OK,
		      "%s: create", v->name);

		size_t pos = 0;
		AiusStatus st = pump(p, w, n, chunks[ci], &pos);
		CHECK(st == v->expect, "%s chunk=%zu: status %s != %s", v->name,
		      chunks[ci], status_name(st), status_name(v->expect));

		AiusBurst b;
		CHECK(aius_iec61937_current_burst(p, &b) == AIUS_OK,
		      "%s chunk=%zu: no current burst", v->name, chunks[ci]);
		CHECK(b.data_type == v->data_type, "%s chunk=%zu: type %u != %u",
		      v->name, chunks[ci], b.data_type, v->data_type);
		CHECK(b.length == plen, "%s chunk=%zu: len %u != %zu", v->name,
		      chunks[ci], b.length, plen);
		if (b.length == plen && plen)
			CHECK(memcmp(b.payload, pay, plen) == 0,
			      "%s chunk=%zu: payload bytes differ", v->name, chunks[ci]);

		aius_iec61937_parser_destroy(p);
	}
	printf("  %-18s %s type=%u payload=%zu B (3 chunkings)\n", v->name,
	       status_name(v->expect), v->data_type, plen);
	free(w);
	free(pay);
}

/* ---- rejection vectors (typed status, no burst emitted) ------------------ */

struct rejv {
	const char *name;
	AiusStatus expect;
};

static const struct rejv k_reject[] = {
	{ "error_flag", AIUS_MALFORMED },
	{ "zero_len", AIUS_MALFORMED },
	{ "overlong", AIUS_NO_SPACE },
};

static void run_reject_vector(const char *dir, const struct rejv *v)
{
	size_t n = 0;
	uint16_t *w = load_wire(dir, v->name, &n);
	if (!w) {
		CHECK(0, "%s: corpus load failed", v->name);
		return;
	}
	AiusIec61937Parser *p = NULL;
	aius_iec61937_parser_create(&p, NULL);
	size_t pos = 0;
	AiusStatus st = pump(p, w, n, n ? n : 1, &pos);
	CHECK(st == v->expect, "%s: status %s != %s", v->name, status_name(st),
	      status_name(v->expect));
	/* A rejected burst never leaves a fresh readable burst. */
	AiusBurst b;
	CHECK(aius_iec61937_current_burst(p, &b) == AIUS_INVALID_ARG,
	      "%s: stale burst readable after rejection", v->name);
	printf("  %-18s %s (rejected, no burst)\n", v->name, status_name(v->expect));
	aius_iec61937_parser_destroy(p);
	free(w);
}

/* ---- two_bursts: one push drains one burst; repeat for the next ---------- */

static void run_two_bursts(const char *dir)
{
	size_t n = 0;
	uint16_t *w = load_wire(dir, "two_bursts", &n);
	size_t plen = 0;
	uint8_t *pay = load_pay(dir, "two_bursts", &plen);
	if (!w || !pay) {
		CHECK(0, "two_bursts: corpus load failed");
		free(w);
		free(pay);
		return;
	}
	AiusIec61937Parser *p = NULL;
	aius_iec61937_parser_create(&p, NULL);

	int emitted = 0;
	size_t pos = 0;
	while (pos < n) {
		AiusStatus st = pump(p, w, n, n, &pos);
		if (st == AIUS_NEED_MORE)
			break;
		CHECK(st == AIUS_OK, "two_bursts: burst %d status %s", emitted,
		      status_name(st));
		AiusBurst b;
		CHECK(aius_iec61937_current_burst(p, &b) == AIUS_OK,
		      "two_bursts: burst %d missing", emitted);
		CHECK(b.length == plen && memcmp(b.payload, pay, plen) == 0,
		      "two_bursts: burst %d payload differs", emitted);
		emitted++;
	}
	CHECK(emitted == 2, "two_bursts: emitted %d != 2", emitted);
	printf("  %-18s OK,OK (coalesced drain)\n", "two_bursts");
	aius_iec61937_parser_destroy(p);
	free(w);
	free(pay);
}

/* ---- truncated: mid-burst end-of-stream -> TRUNCATED --------------------- */

static void run_truncated(const char *dir)
{
	size_t n = 0;
	uint16_t *w = load_wire(dir, "truncated", &n);
	if (!w) {
		CHECK(0, "truncated: corpus load failed");
		return;
	}
	AiusIec61937Parser *p = NULL;
	aius_iec61937_parser_create(&p, NULL);
	size_t pos = 0;
	AiusStatus st = pump(p, w, n, n ? n : 1, &pos);
	CHECK(st == AIUS_NEED_MORE, "truncated: status %s != NEED_MORE",
	      status_name(st));
	CHECK(aius_iec61937_finish(p) == AIUS_TRUNCATED,
	      "truncated: finish did not report TRUNCATED");
	/* Finishing an idle parser is OK. */
	CHECK(aius_iec61937_finish(p) == AIUS_OK, "truncated: idle finish not OK");
	printf("  %-18s NEED_MORE -> finish()=TRUNCATED\n", "truncated");
	aius_iec61937_parser_destroy(p);
	free(w);
}

/* ---- argument / lifecycle contract --------------------------------------- */

static void run_contract(void)
{
	AiusIec61937Parser *p = NULL;
	CHECK(aius_iec61937_parser_create(&p, NULL) == AIUS_OK, "create default");

	size_t consumed = 123;
	uint16_t word = 0;
	CHECK(aius_iec61937_push(NULL, &word, 1, &consumed) == AIUS_INVALID_ARG,
	      "push NULL parser");
	CHECK(aius_iec61937_push(p, NULL, 1, &consumed) == AIUS_INVALID_ARG,
	      "push NULL words with count");
	CHECK(aius_iec61937_push(p, &word, 1, NULL) == AIUS_INVALID_ARG,
	      "push NULL consumed");
	/* count == 0 with NULL words is valid and consumes nothing. */
	CHECK(aius_iec61937_push(p, NULL, 0, &consumed) == AIUS_NEED_MORE &&
	          consumed == 0,
	      "push zero count");

	AiusBurst b;
	CHECK(aius_iec61937_current_burst(p, &b) == AIUS_INVALID_ARG,
	      "current burst before any");
	CHECK(aius_iec61937_current_burst(NULL, &b) == AIUS_INVALID_ARG,
	      "current burst NULL parser");
	CHECK(aius_iec61937_current_burst(p, NULL) == AIUS_INVALID_ARG,
	      "current burst NULL out");

	CHECK(aius_iec61937_finish(NULL) == AIUS_INVALID_ARG, "finish NULL");
	CHECK(aius_iec61937_finish(p) == AIUS_OK, "finish idle");
	aius_iec61937_parser_reset(p);
	aius_iec61937_parser_reset(NULL); /* no-op */

	AiusIec61937Config bad = { .packing = (AiusIec61937Packing)99 };
	AiusIec61937Parser *q = NULL;
	CHECK(aius_iec61937_parser_create(&q, &bad) == AIUS_INVALID_ARG,
	      "create bad packing");
	CHECK(aius_iec61937_parser_create(NULL, NULL) == AIUS_INVALID_ARG,
	      "create NULL out");

	aius_iec61937_parser_destroy(p);
	aius_iec61937_parser_destroy(NULL); /* no-op */
	printf("  %-18s argument + lifecycle contract\n", "contract");
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <corpus_dir>\n", argv[0]);
		return 2;
	}
	const char *dir = argv[1];

	printf("IEC 61937 parser known-answer test\n");
	for (size_t i = 0; i < sizeof(k_burst) / sizeof(k_burst[0]); i++)
		run_burst_vector(dir, &k_burst[i]);
	for (size_t i = 0; i < sizeof(k_reject) / sizeof(k_reject[0]); i++)
		run_reject_vector(dir, &k_reject[i]);
	run_two_bursts(dir);
	run_truncated(dir);
	run_contract();

	if (g_fail) {
		printf("RESULT: %d check(s) FAILED\n", g_fail);
		return 1;
	}
	printf("RESULT: all checks passed\n");
	return 0;
}
