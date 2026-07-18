/* Fuzz entry for the Aius IEC 61937 parser (capability-gate P2).
 *
 * Feeds random and adversarial word streams — pushed in random chunk sizes, with
 * random packing, interleaved reset/finish/current-burst calls — and asserts the
 * parser only ever returns a declared AiusStatus and never reports a burst longer
 * than its buffer. Correctness of the framing is the known-answer test's job;
 * this one exists so ASan/UBSan can prove no out-of-bounds access, no crash, and
 * no undefined behaviour for any input, however malformed. Deterministic: a fixed
 * seed reproduces the run.
 *
 * Usage: fuzz_iec61937 [iterations] [seed]
 * Exit 0 if every iteration held its invariants (a violation aborts the process).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "aius_iec61937.h"

/* xorshift64: small deterministic PRNG, no libc RNG dependency. */
static uint64_t g_rng;
static uint32_t rnd(void)
{
	uint64_t x = g_rng;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	g_rng = x;
	return (uint32_t)(x >> 11);
}
static uint32_t rnd_range(uint32_t n) { return n ? rnd() % n : 0; }

static int valid_status(AiusStatus s)
{
	switch (s) {
	case AIUS_OK:
	case AIUS_NEED_MORE:
	case AIUS_UNSUPPORTED:
	case AIUS_MALFORMED:
	case AIUS_TRUNCATED:
	case AIUS_NO_SPACE:
	case AIUS_INVALID_ARG:
		return 1;
	}
	return 0;
}

#define MAX_WORDS 512

/* Build one adversarial word buffer. Biased to reach deep parser states by
 * occasionally emitting real preambles, small/large length-codes, and pauses. */
static size_t make_stream(uint16_t *w, size_t cap)
{
	size_t n = rnd_range((uint32_t)cap);
	size_t i = 0;
	while (i < n) {
		uint32_t roll = rnd_range(100);
		if (roll < 25 && i + 4 <= n) {
			/* A preamble, sometimes byte-swapped, then a header + payload. */
			int swap = rnd_range(2);
			uint16_t pa = swap ? 0x72F8u : 0xF872u;
			uint16_t pb = swap ? 0x1F4Eu : 0x4E1Fu;
			uint16_t pc = (uint16_t)rnd();
			uint16_t pd = (uint16_t)(rnd_range(3) == 0 ? rnd() : rnd_range(400));
			w[i++] = pa;
			if (i < n) w[i++] = pb;
			if (i < n) w[i++] = pc;
			if (i < n) w[i++] = pd;
		} else if (roll < 40) {
			w[i++] = 0x0000; /* pause / stuffing */
		} else {
			w[i++] = (uint16_t)rnd();
		}
	}
	return n;
}

int main(int argc, char **argv)
{
	unsigned long iters = (argc > 1) ? strtoul(argv[1], NULL, 10) : 200000ul;
	g_rng = (argc > 2) ? strtoull(argv[2], NULL, 10) : 0x9E3779B97F4A7C15ull;

	uint16_t buf[MAX_WORDS];
	unsigned long emitted = 0;

	for (unsigned long it = 0; it < iters; it++) {
		AiusIec61937Config cfg = {
			.packing = rnd_range(2) ? AIUS_IEC61937_PACK_14 : AIUS_IEC61937_PACK_16
		};
		AiusIec61937Parser *p = NULL;
		if (aius_iec61937_parser_create(&p, &cfg) != AIUS_OK) {
			fprintf(stderr, "create failed\n");
			return 1;
		}

		size_t n = make_stream(buf, MAX_WORDS);
		size_t pos = 0;
		int guard = 0;
		while (pos <= n) {
			size_t remain = n - pos;
			size_t chunk = remain ? (rnd_range((uint32_t)remain) + 1) : 0;
			size_t consumed = 12345;
			AiusStatus st = aius_iec61937_push(p, buf + pos, chunk, &consumed);
			if (!valid_status(st)) {
				fprintf(stderr, "invalid status %d\n", st);
				return 1;
			}
			if (consumed > chunk) {
				fprintf(stderr, "over-consumed %zu > %zu\n", consumed, chunk);
				return 1;
			}
			pos += consumed;

			if (st == AIUS_OK || st == AIUS_UNSUPPORTED) {
				AiusBurst b;
				if (aius_iec61937_current_burst(p, &b) == AIUS_OK) {
					if (b.length > AIUS_IEC61937_MAX_PAYLOAD) {
						fprintf(stderr, "burst len %u > cap\n", b.length);
						return 1;
					}
					emitted++;
				}
			}
			/* Occasionally perturb: finish or reset mid-stream. */
			if (rnd_range(50) == 0)
				(void)aius_iec61937_finish(p);
			if (rnd_range(80) == 0)
				aius_iec61937_parser_reset(p);

			if (chunk == 0) {
				if (++guard > 2)
					break; /* no more input; avoid spinning */
			}
		}

		if (!valid_status(aius_iec61937_finish(p))) {
			fprintf(stderr, "finish invalid status\n");
			return 1;
		}
		aius_iec61937_parser_destroy(p);
	}

	printf("fuzz_iec61937: %lu iterations ok, %lu bursts emitted\n", iters,
	       emitted);
	return 0;
}
