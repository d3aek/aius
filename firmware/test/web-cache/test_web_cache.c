/* Host suite for firmware/lib/aius_web_cache (aius-sample-rate-following.md
 * Phase 4g, "host: web-cache (new)"): placement per fitment and its
 * disjointness from the recorder ring and every OTA staging placement; fill,
 * read-back and publish with the flush-delete and read-back order pinned per
 * chunk; a read-back mismatch failing the fill; a 175,913 B publish that reads
 * no chunk; verify's flush-before-CRC order and yields; a flipped bit in any
 * chunk failing verify and dropping the entry; a lease across invalidate;
 * capacity-full fallback; the listing from the manifest; a cancelled fill
 * resuming at its next chunk, and every invalidation dropping the partial. */
#include "aius_web_cache.h"
#include "aius_ota_staging.h"
#include "aius_recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		g_checks++;                                                  \
		if (!(cond)) {                                               \
			g_failures++;                                        \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
			        __LINE__, #cond);                            \
		}                                                            \
	} while (0)

#define MIB UINT32_C(0x00100000)
#define RECORDER_STAGING UINT32_C(32768) /* optical_main.cpp's static_assert */
#define BIG_FILE 175913u                 /* dsp-editor.js, G10 */

static _Alignas(64) uint8_t g_carve[AIUS_WEB_CACHE_CARVE_BYTES];
static AiusWebCache g_cache;

/* ---- fake hardware: an event log, a counting chip reader, a bad chip ---- */

enum { EV_FLUSH = 1, EV_CRC = 2 };

#define EVENTS_MAX 4096u
static struct {
	int kind;
	const uint8_t *addr;
} g_events[EVENTS_MAX];
static size_t g_event_count;
static unsigned g_chip_reads;     /* chip_crc calls over a data chunk */
static unsigned g_flushes;
static uint8_t *g_bad_chip;       /* the chip stores this byte wrong */
static unsigned g_yields;

static bool in_data(const uint8_t *p)
{
	return p >= g_carve + g_cache.data_offset &&
	       p < g_carve + sizeof(g_carve);
}

static void log_event(int kind, const uint8_t *addr)
{
	if (g_event_count < EVENTS_MAX) {
		g_events[g_event_count].kind = kind;
		g_events[g_event_count].addr = addr;
	}
	g_event_count++;
}

static void fake_flush_delete(void *addr, uint32_t bytes)
{
	(void)bytes;
	g_flushes++;
	log_event(EV_FLUSH, addr);
	/* A marginal chip: the written line comes back with bit 2 flipped. */
	if (g_bad_chip != NULL && (uint8_t *)addr <= g_bad_chip &&
	    g_bad_chip < (uint8_t *)addr + bytes)
		*g_bad_chip ^= 0x04u;
}

static uint32_t counting_chip_crc(uint32_t crc, const uint8_t *chip,
                                  uint32_t bytes)
{
	if (in_data(chip))
		g_chip_reads++;
	log_event(EV_CRC, chip);
	return aius_web_cache_crc32(crc, chip, bytes);
}

static void count_yield(void *context)
{
	(void)context;
	g_yields++;
}

static void reset_log(void)
{
	g_event_count = 0u;
	g_chip_reads = 0u;
	g_flushes = 0u;
	g_yields = 0u;
}

static uint32_t ref_crc32(const uint8_t *data, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		c ^= data[i];
		for (int b = 0; b < 8; b++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

static void fill_pattern(uint8_t *dst, size_t n, uint32_t seed)
{
	uint32_t x = seed * 2654435761u + 1u;

	for (size_t i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		dst[i] = (uint8_t)x;
	}
}

static void fresh(void)
{
	g_bad_chip = NULL;
	CHECK(aius_web_cache_init(&g_cache, g_carve, sizeof(g_carve),
	                          fake_flush_delete, counting_chip_crc));
	reset_log();
}

/* Fills and publishes `path` from `data`; returns the publish result. */
static bool put_file(const char *path, const uint8_t *data, uint32_t length)
{
	if (!aius_web_cache_fill_begin(&g_cache, path, strlen(path), length))
		return false;
	for (uint32_t at = 0; at < length; at += AIUS_WEB_CACHE_CHUNK_BYTES) {
		uint32_t n = length - at < AIUS_WEB_CACHE_CHUNK_BYTES
			? length - at
			: AIUS_WEB_CACHE_CHUNK_BYTES;
		if (!aius_web_cache_fill_chunk(&g_cache, data + at, n))
			return false;
	}
	return aius_web_cache_publish(&g_cache);
}

static bool lookup(const char *path, AiusWebCacheHit *hit)
{
	return aius_web_cache_lookup(&g_cache, path, strlen(path), hit, NULL);
}

/* Every byte of a leased/valid entry, through the send-path copy. */
static bool entry_equals(uint8_t entry, const uint8_t *data, uint32_t length)
{
	uint8_t buf[AIUS_WEB_CACHE_CHUNK_BYTES];
	uint32_t at = 0u;

	for (uint16_t c = 0; at < length; c++) {
		uint32_t n = 0u;

		if (!aius_web_cache_copy_chunk(&g_cache, entry, c, buf, &n) ||
		    memcmp(buf, data + at, n) != 0)
			return false;
		at += n;
	}
	return at == length;
}

/* ---------------------------------------------------------------- tests -- */

static void test_crc32(void)
{
	uint8_t buf[1500];

	CHECK(aius_web_cache_crc32(0u, (const uint8_t *)"123456789", 9u) ==
	      0xCBF43926u);
	fill_pattern(buf, sizeof(buf), 7u);
	CHECK(aius_web_cache_crc32(0u, buf, sizeof(buf)) ==
	      ref_crc32(buf, sizeof(buf)));
	CHECK(aius_web_cache_crc32(aius_web_cache_crc32(0u, buf, 700u),
	                           buf + 700, 800u) ==
	      ref_crc32(buf, sizeof(buf)));
	CHECK(aius_web_cache_crc32(0u, buf, 0u) == 0u);
}

static bool disjoint(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return a + alen <= b || b + blen <= a;
}

static void test_placement(void)
{
	const uint32_t mapped[] = {0u, 8u * MIB, 16u * MIB};
	const uint32_t pad2_only[] = {0u, 8u * MIB};
	AiusWebCachePlacement p;
	uint32_t ring16 = aius_recorder_ring_bytes_after_staging(
		16u * MIB, RECORDER_STAGING);
	uint32_t ring8 = aius_recorder_ring_bytes_after_staging(
		8u * MIB, RECORDER_STAGING);

	/* 16 MiB: the 2 MiB tail of pad 2 from 0x70808000. */
	CHECK(ring16 == 8u * MIB);
	CHECK(aius_web_cache_place(16u * MIB, RECORDER_STAGING + ring16, &p));
	CHECK(UINT32_C(0x70000000) + p.offset == UINT32_C(0x70808000));
	CHECK(p.capacity == 2u * MIB);
	CHECK(p.offset >= AIUS_WEB_CACHE_PAD_BYTES);
	CHECK(p.offset + p.capacity <= 16u * MIB);
	CHECK(disjoint(p.offset, p.capacity, RECORDER_STAGING, ring16));
	CHECK(disjoint(p.offset, p.capacity, 0u, RECORDER_STAGING));

	/* 8 MiB, bare and pad-2-only (core maps nothing): absent. */
	CHECK(!aius_web_cache_place(8u * MIB, RECORDER_STAGING + ring8, &p));
	CHECK(p.capacity == 0u);
	CHECK(!aius_web_cache_place(0u, 0u, &p));
	CHECK(!aius_web_cache_place(16u * MIB, 15u * MIB, &p)); /* no room */
	CHECK(!aius_web_cache_place(16u * MIB, 0u, NULL));

	/* Disjoint from every aius_ota_staging_place result on any fitment
	 * where the cache exists. */
	for (size_t m = 0; m < sizeof(mapped) / sizeof(mapped[0]); m++) {
		uint32_t ring = aius_recorder_ring_bytes_after_staging(
			mapped[m], RECORDER_STAGING);
		AiusWebCachePlacement wc;
		bool have = aius_web_cache_place(
			mapped[m], ring == 0u ? 0u : RECORDER_STAGING + ring, &wc);

		CHECK(have == (mapped[m] == 16u * MIB));
		if (!have)
			continue;
		for (size_t q = 0; q < sizeof(pad2_only) / sizeof(pad2_only[0]);
		     q++) {
			AiusOtaStagingPlacement s;

			if (!aius_ota_staging_place(mapped[m], pad2_only[q],
			                            RECORDER_STAGING, &s))
				continue;
			CHECK(disjoint(wc.offset, wc.capacity, s.offset,
			               s.capacity));
		}
	}
}

static void test_init_and_absent(void)
{
	AiusWebCache c;
	AiusWebCacheHit hit;

	CHECK(sizeof(AiusWebCache) < 64u);
	CHECK(!aius_web_cache_init(&c, NULL, 0u, fake_flush_delete, NULL));
	CHECK(!aius_web_cache_lookup(&c, "index.html", 10u, &hit, NULL));
	CHECK(!aius_web_cache_fill_begin(&c, "index.html", 10u, 10u));
	CHECK(!aius_web_cache_init(&c, g_carve, AIUS_WEB_CACHE_MANIFEST_BYTES,
	                           fake_flush_delete, NULL));
	CHECK(!aius_web_cache_init(&c, g_carve, sizeof(g_carve), NULL, NULL));

	fresh();
	/* Layout: manifest, chunk CRC table, data, all inside the carve. */
	CHECK(g_cache.data_offset % AIUS_WEB_CACHE_CHUNK_BYTES == 0u);
	CHECK(g_cache.data_offset >=
	      AIUS_WEB_CACHE_MANIFEST_BYTES + 4u * g_cache.data_chunks);
	CHECK(g_cache.data_offset +
	              (uint32_t)g_cache.data_chunks * AIUS_WEB_CACHE_CHUNK_BYTES <=
	      sizeof(g_carve));
	CHECK(g_cache.data_chunks == 4056u);
}

static void test_fill_readback_publish(void)
{
	static uint8_t file[BIG_FILE];
	AiusWebCacheHit hit;
	char etag[AIUS_WEB_CACHE_ETAG_MAX];
	char want[AIUS_WEB_CACHE_ETAG_MAX];
	bool order_ok = true;

	fresh();
	fill_pattern(file, sizeof(file), 1u);
	CHECK(aius_web_cache_fill_begin(&g_cache, "dsp-editor.js", 13u,
	                                BIG_FILE));
	/* Mid-fill the entry is invisible. */
	CHECK(!lookup("dsp-editor.js", &hit));
	for (uint32_t at = 0; at < BIG_FILE; at += AIUS_WEB_CACHE_CHUNK_BYTES) {
		uint32_t n = BIG_FILE - at < 512u ? BIG_FILE - at : 512u;
		const uint8_t *chip;

		reset_log();
		CHECK(aius_web_cache_fill_chunk(&g_cache, file + at, n));
		/* Inside the call: flush-delete the chunk, then CRC it from the
		 * chip, exactly once each, in that order. */
		chip = g_carve + g_cache.data_offset +
		       (at / 512u) * AIUS_WEB_CACHE_CHUNK_BYTES;
		order_ok = order_ok && g_event_count == 2u &&
		           g_events[0].kind == EV_FLUSH &&
		           g_events[0].addr == chip &&
		           g_events[1].kind == EV_CRC &&
		           g_events[1].addr == chip && g_chip_reads == 1u;
	}
	CHECK(order_ok);
	CHECK(!lookup("dsp-editor.js", &hit));

	/* G10, CR-S6: publish reads no chunk and flushes nothing. */
	reset_log();
	CHECK(aius_web_cache_publish(&g_cache));
	CHECK(g_chip_reads == 0u);
	CHECK(g_flushes == 0u);

	CHECK(lookup("dsp-editor.js", &hit));
	CHECK(hit.length == BIG_FILE);
	CHECK(hit.chunks == (BIG_FILE + 511u) / 512u);
	CHECK(hit.file_crc == ref_crc32(file, sizeof(file)));
	CHECK(entry_equals(hit.entry, file, BIG_FILE));
	snprintf(want, sizeof(want), "\"%08x-%u\"", (unsigned)hit.file_crc,
	         (unsigned)BIG_FILE);
	CHECK(aius_web_cache_etag(&hit, etag, sizeof(etag)) == strlen(want));
	CHECK(strcmp(etag, want) == 0);
	CHECK(aius_web_cache_etag(&hit, etag, strlen(want)) == 0u);
	CHECK(g_cache.crc_faults == 0u);

	/* A second publish with no fill in flight is refused. */
	CHECK(!aius_web_cache_publish(&g_cache));
}

static void test_fill_faults(void)
{
	uint8_t file[2000];
	AiusWebCacheHit hit;

	fill_pattern(file, sizeof(file), 2u);

	/* A chip that returns chunk 2 with a bit flipped fails the fill in
	 * that chunk's own primitive. */
	fresh();
	CHECK(aius_web_cache_fill_begin(&g_cache, "status.js", 9u, 2000u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file + 512, 512u));
	g_bad_chip = g_carve + g_cache.data_offset + 2u * 512u + 100u;
	CHECK(!aius_web_cache_fill_chunk(&g_cache, file + 1024, 512u));
	g_bad_chip = NULL;
	CHECK(g_cache.crc_faults == 1u);
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);
	CHECK(!aius_web_cache_fill_chunk(&g_cache, file + 1536, 464u));
	CHECK(!aius_web_cache_publish(&g_cache));
	CHECK(!lookup("status.js", &hit));

	/* Wrong chunk sizes and an early publish drop the fill. */
	fresh();
	CHECK(aius_web_cache_fill_begin(&g_cache, "status.js", 9u, 2000u));
	CHECK(!aius_web_cache_fill_chunk(&g_cache, file, 511u));
	CHECK(!aius_web_cache_publish(&g_cache));
	CHECK(aius_web_cache_fill_begin(&g_cache, "status.js", 9u, 2000u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(!aius_web_cache_publish(&g_cache));
	CHECK(!lookup("status.js", &hit));
	CHECK(aius_web_cache_fill_begin(&g_cache, "status.js", 9u, 2000u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file + 512, 512u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file + 1024, 512u));
	CHECK(!aius_web_cache_fill_chunk(&g_cache, file + 1536, 512u));
	CHECK(!lookup("status.js", &hit));

	/* The G9 cancel: abort drops the unpublished entry; a re-queued fill
	 * then succeeds. */
	CHECK(aius_web_cache_fill_begin(&g_cache, "status.js", 9u, 2000u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	aius_web_cache_fill_abort(&g_cache);
	CHECK(!aius_web_cache_publish(&g_cache));
	CHECK(!lookup("status.js", &hit));
	CHECK(put_file("status.js", file, 2000u));
	CHECK(lookup("status.js", &hit));

	/* Invalidating the path of the in-flight fill drops it too. */
	CHECK(aius_web_cache_fill_begin(&g_cache, "status.js", 9u, 2000u));
	aius_web_cache_invalidate(&g_cache, "status.js", 9u);
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);
	CHECK(!lookup("status.js", &hit));

	/* Paths: too long or empty are never cached. */
	{
		char longp[AIUS_WEB_CACHE_PATH_MAX + 2u];

		memset(longp, 'a', sizeof(longp) - 1u);
		longp[sizeof(longp) - 1u] = '\0';
		CHECK(!aius_web_cache_fill_begin(&g_cache, longp,
		                                 strlen(longp), 10u));
		CHECK(aius_web_cache_fill_begin(&g_cache, longp,
		                                strlen(longp) - 1u, 10u));
		aius_web_cache_fill_abort(&g_cache);
		CHECK(!aius_web_cache_fill_begin(&g_cache, "", 0u, 10u));
	}

	/* A zero-length file publishes with no chunk. */
	CHECK(put_file("empty.txt", file, 0u));
	CHECK(lookup("empty.txt", &hit));
	CHECK(hit.length == 0u && hit.chunks == 0u && hit.file_crc == 0u);
	CHECK(aius_web_cache_verify(&g_cache, hit.entry, NULL, NULL));
	{
		char etag[AIUS_WEB_CACHE_ETAG_MAX];

		CHECK(aius_web_cache_etag(&hit, etag, sizeof(etag)) == 12u);
		CHECK(strcmp(etag, "\"00000000-0\"") == 0);
	}
}

static void test_verify_order_and_yields(void)
{
	static uint8_t file[BIG_FILE];
	AiusWebCacheHit hit;
	bool order_ok = true;
	const uint32_t chunks = (BIG_FILE + 511u) / 512u;

	fresh();
	fill_pattern(file, sizeof(file), 3u);
	CHECK(put_file("dsp-editor.js", file, BIG_FILE));
	CHECK(lookup("dsp-editor.js", &hit));
	reset_log();
	CHECK(aius_web_cache_verify(&g_cache, hit.entry, count_yield, NULL));
	CHECK(g_event_count == 2u * chunks);
	for (uint32_t i = 0; i < chunks && 2u * i + 1u < EVENTS_MAX; i++) {
		const uint8_t *chip = g_carve + g_cache.data_offset +
		                      (size_t)i * AIUS_WEB_CACHE_CHUNK_BYTES;
		order_ok = order_ok && g_events[2u * i].kind == EV_FLUSH &&
		           g_events[2u * i].addr == chip &&
		           g_events[2u * i + 1u].kind == EV_CRC &&
		           g_events[2u * i + 1u].addr == chip;
	}
	CHECK(order_ok);
	CHECK(g_chip_reads == chunks);
	/* A yield after every 8 chunks, none after the last: 344 -> 42. */
	CHECK(g_yields == (chunks - 1u) / AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS);
	CHECK(aius_web_cache_verify(&g_cache, hit.entry, NULL, NULL));
	CHECK(!aius_web_cache_verify(&g_cache, AIUS_WEB_CACHE_ENTRIES, NULL,
	                             NULL));
}

/* Flip one bit in chunk `chunk` of the published entry and verify. */
static void flip_and_verify(const char *path, const uint8_t *file,
                            uint32_t length, uint32_t chunk, uint32_t byte,
                            unsigned bit, bool *all_ok)
{
	AiusWebCacheHit hit;
	uint32_t faults;
	bool ok = true;

	ok = ok && put_file(path, file, length);
	ok = ok && lookup(path, &hit);
	ok = ok && aius_web_cache_verify(&g_cache, hit.entry, NULL, NULL);
	g_carve[g_cache.data_offset + chunk * 512u + byte] ^=
		(uint8_t)(1u << bit);
	faults = g_cache.crc_faults;
	ok = ok && !aius_web_cache_verify(&g_cache, hit.entry, NULL, NULL);
	ok = ok && g_cache.crc_faults == faults + 1u;
	ok = ok && !lookup(path, &hit); /* dropped: next request is SD */
	*all_ok = *all_ok && ok;
}

static void test_flipped_bit_any_chunk(void)
{
	static uint8_t big[BIG_FILE];
	uint8_t small[2700]; /* five whole chunks and a 140 B tail */
	const uint32_t small_chunks = (sizeof(small) + 511u) / 512u;
	const uint32_t big_chunks = (BIG_FILE + 511u) / 512u;
	bool all_ok = true;

	fill_pattern(small, sizeof(small), 4u);
	for (uint32_t c = 0; c < small_chunks; c++) {
		uint32_t n = c + 1u < small_chunks ? 512u
		                                   : sizeof(small) - c * 512u;
		for (unsigned bit = 0; bit < 8u; bit++) {
			fresh();
			flip_and_verify("replays.css", small, sizeof(small), c,
			                (bit * 61u) % n, bit, &all_ok);
		}
	}
	CHECK(all_ok);

	fill_pattern(big, sizeof(big), 5u);
	for (uint32_t c = 0; c < big_chunks; c++) {
		fresh();
		flip_and_verify("dsp-editor.js", big, BIG_FILE, c,
		                c + 1u < big_chunks ? (c * 7u) % 512u : 0u,
		                c % 8u, &all_ok);
	}
	CHECK(all_ok);

	/* The send path catches a flip after the headers too. */
	{
		AiusWebCacheHit hit;
		uint8_t buf[512];
		uint32_t n = 0u;

		fresh();
		CHECK(put_file("replays.css", small, sizeof(small)));
		CHECK(lookup("replays.css", &hit));
		CHECK(aius_web_cache_lease(&g_cache, hit.entry));
		CHECK(aius_web_cache_copy_chunk(&g_cache, hit.entry, 0u, buf,
		                                &n) &&
		      n == 512u);
		g_carve[g_cache.data_offset + 1u * 512u + 9u] ^= 0x80u;
		CHECK(!aius_web_cache_copy_chunk(&g_cache, hit.entry, 1u, buf,
		                                 &n));
		CHECK(g_cache.crc_faults == 1u);
		CHECK(!lookup("replays.css", &hit));
		aius_web_cache_release(&g_cache, hit.entry);
	}

	/* A corrupt manifest entry misses and counts. */
	{
		AiusWebCacheHit hit;

		fresh();
		CHECK(put_file("replays.css", small, sizeof(small)));
		CHECK(lookup("replays.css", &hit));
		g_carve[(size_t)hit.entry * AIUS_WEB_CACHE_ENTRY_BYTES + 52u] ^=
			0x01u; /* length */
		CHECK(!lookup("replays.css", &hit));
		CHECK(g_cache.crc_faults == 1u);
	}
}

static size_t list(const char *dir);
static int row_index(const char *name, bool dir);

/* Finding 5: a manifest-entry CRC fault is reported apart from a plain miss
 * (so the dispatcher refills), the entry count follows the drop, and the
 * listing omits the row while counting the fault the caller watches. */
static void test_entry_fault_reported(void)
{
	uint8_t small[700];
	AiusWebCacheHit hit;
	bool fault = true;

	fresh();
	fill_pattern(small, sizeof(small), 40u);
	CHECK(aius_web_cache_entry_count(&g_cache) == 0u);
	CHECK(!aius_web_cache_lookup(&g_cache, "a.css", 5u, &hit, &fault));
	CHECK(!fault); /* a plain miss */
	CHECK(put_file("a.css", small, sizeof(small)));
	CHECK(put_file("presets/p.json", small, 100u));
	CHECK(put_file("presets/q.json", small, 200u));
	CHECK(aius_web_cache_entry_count(&g_cache) == 3u);
	fault = true;
	CHECK(aius_web_cache_lookup(&g_cache, "a.css", 5u, &hit, &fault));
	CHECK(!fault);
	g_carve[(size_t)hit.entry * AIUS_WEB_CACHE_ENTRY_BYTES + 52u] ^= 0x01u;
	CHECK(!aius_web_cache_lookup(&g_cache, "a.css", 5u, &hit, &fault));
	CHECK(fault);
	CHECK(g_cache.crc_faults == 1u);
	CHECK(aius_web_cache_entry_count(&g_cache) == 2u);
	/* Dropped, so the next lookup is a plain miss. */
	CHECK(!aius_web_cache_lookup(&g_cache, "a.css", 5u, &hit, &fault));
	CHECK(!fault);

	/* The listing: the faulted row is omitted and crc_faults moves. */
	CHECK(lookup("presets/q.json", &hit));
	g_carve[(size_t)hit.entry * AIUS_WEB_CACHE_ENTRY_BYTES + 52u] ^= 0x01u;
	CHECK(list("presets/") == 1u && row_index("p.json", false) >= 0);
	CHECK(g_cache.crc_faults == 2u);
	CHECK(aius_web_cache_entry_count(&g_cache) == 1u);
	CHECK(aius_web_cache_entry_count(NULL) == 0u);
}

/* Finding 9: an entry invalidated while leased, then CRC-faulted on the
 * lease's send copy, leaves the VALID count once, not twice. */
static void test_entry_count_single_drop(void)
{
	uint8_t data[1500];
	uint8_t buf[AIUS_WEB_CACHE_CHUNK_BYTES];
	AiusWebCacheHit hit;
	uint32_t n = 0u;

	fresh();
	fill_pattern(data, sizeof(data), 41u);
	CHECK(put_file("a.js", data, sizeof(data)));
	CHECK(put_file("b.js", data, sizeof(data)));
	CHECK(aius_web_cache_entry_count(&g_cache) == 2u);
	CHECK(lookup("a.js", &hit));
	CHECK(aius_web_cache_lease(&g_cache, hit.entry));
	aius_web_cache_invalidate(&g_cache, "a.js", 4u);
	CHECK(aius_web_cache_entry_count(&g_cache) == 1u);
	g_carve[g_cache.data_offset + 5u] ^= 0x01u; /* a.js starts at chunk 0 */
	CHECK(!aius_web_cache_copy_chunk(&g_cache, hit.entry, 0u, buf, &n));
	CHECK(g_cache.crc_faults == 1u);
	CHECK(aius_web_cache_entry_count(&g_cache) == 1u);
	CHECK(!aius_web_cache_verify(&g_cache, hit.entry, NULL, NULL));
	CHECK(g_cache.crc_faults == 2u);
	CHECK(aius_web_cache_entry_count(&g_cache) == 1u);
	aius_web_cache_release(&g_cache, hit.entry);
	CHECK(aius_web_cache_entry_count(&g_cache) == 1u);
	CHECK(lookup("b.js", &hit));

	/* A non-CRC copy failure (chunk out of range) drops nothing. */
	CHECK(!aius_web_cache_copy_chunk(&g_cache, hit.entry, 99u, buf, &n));
	CHECK(aius_web_cache_entry_count(&g_cache) == 1u);
}

static void test_lease_across_invalidate(void)
{
	uint8_t v1[3000];
	uint8_t v2[3500];
	AiusWebCacheHit old_hit;
	AiusWebCacheHit new_hit;
	AiusWebCacheHit hit;

	fresh();
	fill_pattern(v1, sizeof(v1), 10u);
	fill_pattern(v2, sizeof(v2), 11u);
	CHECK(put_file("index.html", v1, sizeof(v1)));
	CHECK(lookup("index.html", &old_hit));
	CHECK(aius_web_cache_lease(&g_cache, old_hit.entry));

	/* A PUT lands: the path stops being served, the lease keeps the bytes. */
	aius_web_cache_invalidate(&g_cache, "index.html", 10u);
	CHECK(!lookup("index.html", &hit));
	CHECK(!aius_web_cache_lease(&g_cache, old_hit.entry));
	CHECK(entry_equals(old_hit.entry, v1, sizeof(v1)));

	/* The replacement fills beside it and serves the next request. */
	CHECK(put_file("index.html", v2, sizeof(v2)));
	CHECK(lookup("index.html", &new_hit));
	CHECK(new_hit.entry != old_hit.entry);
	CHECK(new_hit.file_crc == ref_crc32(v2, sizeof(v2)));
	CHECK(entry_equals(new_hit.entry, v2, sizeof(v2)));
	CHECK(entry_equals(old_hit.entry, v1, sizeof(v1)));
	CHECK(aius_web_cache_verify(&g_cache, old_hit.entry, NULL, NULL));

	/* The last release frees the old chunks. */
	aius_web_cache_release(&g_cache, old_hit.entry);
	CHECK(!entry_equals(old_hit.entry, v1, sizeof(v1)));
	CHECK(lookup("index.html", &hit) && hit.entry == new_hit.entry);

	/* A publish over a leased VALID entry retires it the same way. */
	CHECK(aius_web_cache_lease(&g_cache, new_hit.entry));
	CHECK(put_file("index.html", v1, sizeof(v1)));
	CHECK(lookup("index.html", &hit));
	CHECK(hit.entry != new_hit.entry &&
	      hit.file_crc == ref_crc32(v1, sizeof(v1)));
	CHECK(entry_equals(new_hit.entry, v2, sizeof(v2)));
	aius_web_cache_release(&g_cache, new_hit.entry);
	CHECK(!entry_equals(new_hit.entry, v2, sizeof(v2)));

	/* Without a lease, invalidate frees at once; NULL clears everything. */
	aius_web_cache_invalidate(&g_cache, NULL, 0u);
	CHECK(!lookup("index.html", &hit));
}

static void test_capacity_full(void)
{
	static uint8_t big[400u * 1024u];
	uint8_t tiny[10];
	AiusWebCacheHit hit;
	char path[24];
	int published = 0;

	fresh();
	fill_pattern(big, sizeof(big), 20u);
	/* 800 chunks each against 4,056: five fit, the sixth does not. */
	for (int i = 0; i < 6; i++) {
		snprintf(path, sizeof(path), "f%d.bin", i);
		if (put_file(path, big, sizeof(big)))
			published++;
	}
	CHECK(published == 5);
	CHECK(!lookup("f5.bin", &hit)); /* falls back to SD */
	CHECK(lookup("f0.bin", &hit) && lookup("f4.bin", &hit));
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);

	/* Freeing one makes room again; a leased one does not until release. */
	CHECK(lookup("f1.bin", &hit));
	CHECK(aius_web_cache_lease(&g_cache, hit.entry));
	aius_web_cache_invalidate(&g_cache, "f1.bin", 6u);
	CHECK(!put_file("f5.bin", big, sizeof(big)));
	aius_web_cache_release(&g_cache, hit.entry);
	CHECK(put_file("f5.bin", big, sizeof(big)));
	CHECK(lookup("f5.bin", &hit));
	CHECK(aius_web_cache_verify(&g_cache, hit.entry, NULL, NULL));
	CHECK(entry_equals(hit.entry, big, sizeof(big)));

	/* Manifest slots: 64 entries, the 65th falls back. */
	fresh();
	memset(tiny, 0x5A, sizeof(tiny));
	published = 0;
	for (int i = 0; i < (int)AIUS_WEB_CACHE_ENTRIES + 1; i++) {
		snprintf(path, sizeof(path), "t%d", i);
		if (put_file(path, tiny, sizeof(tiny)))
			published++;
	}
	CHECK(published == (int)AIUS_WEB_CACHE_ENTRIES);
	CHECK(!lookup("t64", &hit));
	CHECK(lookup("t63", &hit));

	/* Bigger than the whole carve. */
	CHECK(!aius_web_cache_fill_begin(&g_cache, "huge", 4u,
	                                 AIUS_WEB_CACHE_CARVE_BYTES));
}

#define ROWS_MAX 16
static struct {
	char name[AIUS_WEB_CACHE_PATH_MAX + 1u];
	uint32_t size;
	bool dir;
} g_rows[ROWS_MAX];
static size_t g_row_count;

static bool collect_row(void *context, const char *name, size_t name_length,
                        uint32_t size_bytes, bool is_directory)
{
	(void)context;
	if (g_row_count < ROWS_MAX) {
		memcpy(g_rows[g_row_count].name, name, name_length);
		g_rows[g_row_count].name[name_length] = '\0';
		g_rows[g_row_count].size = size_bytes;
		g_rows[g_row_count].dir = is_directory;
	}
	g_row_count++;
	return true;
}

static bool stop_after_one(void *context, const char *name,
                           size_t name_length, uint32_t size_bytes,
                           bool is_directory)
{
	(void)context;
	(void)name;
	(void)name_length;
	(void)size_bytes;
	(void)is_directory;
	return false;
}

static int row_index(const char *name, bool dir)
{
	for (size_t i = 0; i < g_row_count && i < ROWS_MAX; i++)
		if (strcmp(g_rows[i].name, name) == 0 && g_rows[i].dir == dir)
			return (int)i;
	return -1;
}

static size_t list(const char *dir)
{
	g_row_count = 0u;
	return aius_web_cache_listing(&g_cache, dir, strlen(dir), collect_row,
	                              NULL);
}

static void test_listing(void)
{
	uint8_t data[1200];
	int i;

	fresh();
	fill_pattern(data, sizeof(data), 30u);
	CHECK(put_file("index.html", data, 1200u));
	CHECK(put_file("presets/a.json", data, 100u));
	CHECK(put_file("presets/b.json", data, 700u));
	CHECK(put_file("presets/old/c.json", data, 50u));
	CHECK(put_file("presets/old/d.json", data, 60u));
	CHECK(put_file("fonts/x.woff2", data, 1000u));
	CHECK(aius_web_cache_fill_begin(&g_cache, "presets/unpublished.json",
	                                24u, 10u)); /* FILLING: not listed */

	CHECK(list("presets/") == 3u && g_row_count == 3u);
	i = row_index("a.json", false);
	CHECK(i >= 0 && g_rows[i].size == 100u);
	i = row_index("b.json", false);
	CHECK(i >= 0 && g_rows[i].size == 700u);
	CHECK(row_index("old", true) >= 0);

	CHECK(list("") == 3u);
	CHECK(row_index("index.html", false) >= 0);
	CHECK(row_index("presets", true) >= 0);
	CHECK(row_index("fonts", true) >= 0);

	CHECK(list("presets/old/") == 2u);
	CHECK(list("nothing/") == 0u);
	CHECK(aius_web_cache_listing(&g_cache, "presets/", 8u, stop_after_one,
	                             NULL) == 1u);

	/* The listing follows the manifest: a DELETE, then a directory. */
	aius_web_cache_invalidate(&g_cache, "presets/a.json", 14u);
	CHECK(list("presets/") == 2u && row_index("a.json", false) < 0);
	aius_web_cache_invalidate(&g_cache, "presets/", 8u);
	CHECK(list("presets/") == 0u);
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);
	CHECK(list("") == 2u);
}

/* Owner (clean-room finding 1): a cancelled fill file resumes at its next
 * chunk. openapi.json's 262,071 B (512 chunks, a 439 B tail) filled with a
 * "cancel" every `every` chunks, each restart continuing through
 * _fill_resume_offset / _fill_resume, publishes the entry a straight-through
 * fill publishes. */
#define OPENAPI_FILE 262071u

static bool resumed_fill(const char *path, const uint8_t *data,
                         uint32_t length, uint32_t every, unsigned *restarts)
{
	const size_t n = strlen(path);

	*restarts = 0u;
	for (;;) {
		const uint32_t offset =
			aius_web_cache_fill_resume_offset(&g_cache, path, n);
		uint32_t at = offset;

		if (offset == 0u &&
		    !aius_web_cache_fill_begin(&g_cache, path, n, length))
			return false;
		if (offset != 0u &&
		    !aius_web_cache_fill_resume(&g_cache, path, n, offset, length))
			return false;
		CHECK(offset % AIUS_WEB_CACHE_CHUNK_BYTES == 0u || offset == length);
		for (uint32_t k = 0; k < every && at < length; k++) {
			const uint32_t c = length - at < AIUS_WEB_CACHE_CHUNK_BYTES
				? length - at
				: AIUS_WEB_CACHE_CHUNK_BYTES;
			if (!aius_web_cache_fill_chunk(&g_cache, data + at, c))
				return false;
			at += c;
		}
		if (at == length)
			return aius_web_cache_publish(&g_cache);
		/* The cancel: nothing is called; the partial stays FILLING. */
		CHECK(aius_web_cache_entry_count(&g_cache) == 0u);
		CHECK(aius_web_cache_fill_resume_offset(&g_cache, path, n) == at);
		(*restarts)++;
	}
}

static void test_fill_resume(void)
{
	static uint8_t file[OPENAPI_FILE];
	static const char path[] = "openapi.json";
	const size_t n = sizeof(path) - 1u;
	AiusWebCacheHit straight;
	AiusWebCacheHit resumed;
	unsigned restarts = 0u;

	fill_pattern(file, sizeof(file), 0x0A11u);

	/* Straight through: the reference entry. */
	fresh();
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, path, n) == 0u);
	CHECK(put_file(path, file, OPENAPI_FILE));
	CHECK(lookup(path, &straight));
	CHECK(straight.file_crc == ref_crc32(file, OPENAPI_FILE));
	CHECK(straight.chunks == 512u && straight.length == OPENAPI_FILE);

	/* A cancel every chunk, every 3 chunks, and every 7: each completes,
	 * each restart re-reads nothing, and the entry equals the reference. */
	for (uint32_t every = 1u; every <= 7u; every += 3u) {
		fresh();
		CHECK(resumed_fill(path, file, OPENAPI_FILE, every, &restarts));
		CHECK(restarts == (512u + every - 1u) / every - 1u);
		CHECK(lookup(path, &resumed));
		CHECK(resumed.file_crc == straight.file_crc);
		CHECK(resumed.length == straight.length &&
		      resumed.chunks == straight.chunks);
		CHECK(entry_equals(resumed.entry, file, OPENAPI_FILE));
		CHECK(aius_web_cache_fill_resume_offset(&g_cache, path, n) == 0u);
	}

	/* A size change between the cancel and the resume restarts from 0:
	 * the partial is dropped and the next start is a fresh begin. */
	fresh();
	CHECK(aius_web_cache_fill_begin(&g_cache, path, n, OPENAPI_FILE));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file + 512, 512u));
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, path, n) == 1024u);
	CHECK(!aius_web_cache_fill_resume(&g_cache, path, n, 1024u,
	                                  OPENAPI_FILE + 1u));
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, path, n) == 0u);
	/* A wrong offset, and a different path, also drop it. */
	CHECK(aius_web_cache_fill_begin(&g_cache, path, n, OPENAPI_FILE));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(!aius_web_cache_fill_resume(&g_cache, path, n, 1024u,
	                                  OPENAPI_FILE));
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);
	CHECK(aius_web_cache_fill_begin(&g_cache, path, n, OPENAPI_FILE));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, "index.html", 10u) ==
	      0u);
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, path, n) == 512u);
	CHECK(!aius_web_cache_fill_resume(&g_cache, "index.html", 10u, 512u,
	                                  OPENAPI_FILE));
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);

	/* Cancelled after its last chunk: the resume at the length publishes. */
	fresh();
	CHECK(aius_web_cache_fill_begin(&g_cache, "a.css", 5u, 700u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file + 512, 188u));
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, "a.css", 5u) == 700u);
	CHECK(aius_web_cache_fill_resume(&g_cache, "a.css", 5u, 700u, 700u));
	CHECK(aius_web_cache_publish(&g_cache));
	CHECK(lookup("a.css", &resumed) &&
	      resumed.file_crc == ref_crc32(file, 700u));
}

/* Upload or delete of the path, a directory prefix, a remount or card loss
 * (invalidate all) and another file's begin each drop a cancelled partial. */
static void test_fill_resume_invalidated(void)
{
	static uint8_t file[2048];
	struct {
		const char *path;
		const char *drop; /* NULL: invalidate everything */
	} cases[] = {
		{"openapi.json", "openapi.json"},
		{"presets/a.json", "presets/"},
		{"openapi.json", NULL},
	};

	fill_pattern(file, sizeof(file), 7u);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const size_t n = strlen(cases[i].path);

		fresh();
		CHECK(aius_web_cache_fill_begin(&g_cache, cases[i].path, n,
		                                sizeof(file)));
		CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
		CHECK(aius_web_cache_fill_resume_offset(&g_cache, cases[i].path,
		                                        n) == 512u);
		/* Another path's invalidate keeps it. */
		aius_web_cache_invalidate(&g_cache, "index.html", 10u);
		CHECK(aius_web_cache_fill_resume_offset(&g_cache, cases[i].path,
		                                        n) == 512u);
		aius_web_cache_invalidate(&g_cache, cases[i].drop,
		                          cases[i].drop != NULL
		                                  ? strlen(cases[i].drop)
		                                  : 0u);
		CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);
		CHECK(aius_web_cache_fill_resume_offset(&g_cache, cases[i].path,
		                                        n) == 0u);
		CHECK(!aius_web_cache_fill_resume(&g_cache, cases[i].path, n,
		                                  512u, sizeof(file)));
	}
	/* The walk moving to another file drops the partial too. */
	fresh();
	CHECK(aius_web_cache_fill_begin(&g_cache, "openapi.json", 12u,
	                                sizeof(file)));
	CHECK(aius_web_cache_fill_chunk(&g_cache, file, 512u));
	CHECK(aius_web_cache_fill_begin(&g_cache, "index.html", 10u, 600u));
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, "openapi.json", 12u) ==
	      0u);
}

int main(void)
{
	test_fill_resume();
	test_fill_resume_invalidated();
	test_crc32();
	test_placement();
	test_init_and_absent();
	test_fill_readback_publish();
	test_fill_faults();
	test_verify_order_and_yields();
	test_flipped_bit_any_chunk();
	test_entry_fault_reported();
	test_entry_count_single_drop();
	test_lease_across_invalidate();
	test_capacity_full();
	test_listing();
	if (g_failures != 0) {
		fprintf(stderr, "web-cache: %d of %d checks failed\n",
		        g_failures, g_checks);
		return 1;
	}
	printf("web-cache: %d checks passed\n", g_checks);
	return 0;
}
