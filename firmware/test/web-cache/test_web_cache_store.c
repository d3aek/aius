/* Host suite for firmware/lib/aius_web_cache as 4h's one store
 * (aius-sample-rate-following.md Phase 4h, "host: web-cache", B1, N1, S6,
 * NS1, NS5, N2, N3, Q-A, D2, D6, E2, F1, G1): pending states, puts, the
 * tombstone, the flush pin, newest by commit_seq, the safe-copy bit, case
 * folding, the entry cap and fragmentation. */
#include "aius_web_cache.h"

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

static _Alignas(64) uint8_t g_carve[AIUS_WEB_CACHE_CARVE_BYTES];
static AiusWebCache g_cache;
static uint8_t *g_bad_chip;
static uint8_t g_buf[AIUS_WEB_CACHE_CARVE_BYTES];

static void fake_flush_delete(void *addr, uint32_t bytes)
{
	if (g_bad_chip != NULL && (uint8_t *)addr <= g_bad_chip &&
	    g_bad_chip < (uint8_t *)addr + bytes)
		*g_bad_chip ^= 0x04u;
}

static void fresh(void)
{
	g_bad_chip = NULL;
	CHECK(aius_web_cache_init(&g_cache, g_carve, sizeof(g_carve),
	                          fake_flush_delete, NULL));
}

static void pattern(uint8_t *dst, size_t n, uint32_t seed)
{
	uint32_t x = seed * 2654435761u + 1u;

	for (size_t i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		dst[i] = (uint8_t)x;
	}
}

static AiusWebCacheEntry *E(uint8_t slot)
{
	return (AiusWebCacheEntry *)(void *)(g_carve +
	                                     slot * AIUS_WEB_CACHE_ENTRY_BYTES);
}

static bool fill(const char *path, uint32_t length, uint32_t seed)
{
	pattern(g_buf, length, seed);
	if (!aius_web_cache_fill_begin(&g_cache, path, strlen(path), length))
		return false;
	for (uint32_t at = 0; at < length; at += 512u) {
		uint32_t n = length - at < 512u ? length - at : 512u;

		if (!aius_web_cache_fill_chunk(&g_cache, g_buf + at, n))
			return false;
	}
	return aius_web_cache_publish(&g_cache);
}

static AiusWebCachePutResult put(const char *path, uint32_t length,
                                 uint32_t seed, uint8_t flags)
{
	pattern(g_buf, length, seed);
	return aius_web_cache_put(&g_cache, path, strlen(path), g_buf, length,
	                          flags, 1000u);
}

static AiusWebCachePutResult stream(const char *path, uint32_t length,
                                    uint32_t seed)
{
	AiusWebCachePutResult r;

	pattern(g_buf, length, seed);
	r = aius_web_cache_put_begin(&g_cache, path, strlen(path), length, 0u);
	for (uint32_t at = 0; r == AIUS_WEB_CACHE_PUT_OK && at < length;
	     at += 512u)
		r = aius_web_cache_put_chunk(&g_cache, g_buf + at,
		                             length - at < 512u ? length - at
		                                                : 512u);
	return r == AIUS_WEB_CACHE_PUT_OK ? aius_web_cache_put_commit(&g_cache,
	                                                              1000u)
	                                  : r;
}

static bool hit(const char *path, AiusWebCacheHit *h)
{
	AiusWebCacheHit tmp;

	return aius_web_cache_lookup(&g_cache, path, strlen(path),
	                             h != NULL ? h : &tmp, NULL);
}

static uint8_t slot_of(const char *path)
{
	AiusWebCacheHit h;

	return hit(path, &h) ? h.entry : AIUS_WEB_CACHE_NO_ENTRY;
}

static unsigned count(uint8_t state)
{
	unsigned n = 0u;

	for (uint8_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++)
		n += E(i)->state == state;
	return n;
}

/* The bytes an entry serves equal pattern(seed). */
static bool serves(uint8_t slot, uint32_t length, uint32_t seed)
{
	uint8_t chunk[512];

	pattern(g_buf, length, seed);
	for (uint32_t at = 0; at < length; at += 512u) {
		uint32_t n = 0u;

		if (!aius_web_cache_copy_chunk(&g_cache, slot,
		                               (uint16_t)(at / 512u), chunk, &n) ||
		    memcmp(chunk, g_buf + at, n) != 0)
			return false;
	}
	return true;
}

static bool safe_backup(uint8_t slot)
{
	return (E(slot)->flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) != 0u;
}

static unsigned g_rows;
static char g_row_names[64][48];

static bool collect(void *ctx, const char *name, size_t n, uint32_t size,
                    bool dir)
{
	(void)ctx;
	(void)size;
	(void)dir;
	if (g_rows < 64u) {
		memcpy(g_row_names[g_rows], name, n);
		g_row_names[g_rows][n] = '\0';
	}
	g_rows++;
	return true;
}

static unsigned list(const char *dir, bool pending_only)
{
	g_rows = 0u;
	if (pending_only)
		aius_web_cache_pending_listing(&g_cache, dir, strlen(dir), collect,
		                               NULL);
	else
		aius_web_cache_listing(&g_cache, dir, strlen(dir), collect, NULL);
	return g_rows;
}

static bool listed(const char *name)
{
	for (unsigned i = 0; i < g_rows && i < 64u; i++)
		if (strcmp(g_row_names[i], name) == 0)
			return true;
	return false;
}

/* ---------------------------------------------------------------- tests -- */

static void test_layout(void)
{
	CHECK(sizeof(AiusWebCacheEntry) == 80u);
	CHECK(AIUS_WEB_CACHE_MANIFEST_BYTES == 5120u);
	CHECK(offsetof(AiusWebCacheEntry, accept_ms) == 64u);
	CHECK(offsetof(AiusWebCacheEntry, next_try_ms) == 68u);
	CHECK(offsetof(AiusWebCacheEntry, commit_seq) == 72u);
	CHECK(offsetof(AiusWebCacheEntry, flags) == 76u);
	CHECK(offsetof(AiusWebCacheEntry, tries) == 77u);
	CHECK(sizeof(void *) != 4u || sizeof(AiusWebCache) == 52u);
	/* Keys relative to /aius/: dsp.json and www/presets/<32 B name>. */
	fresh();
	CHECK(put("dsp.json", 100u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(put("www/presets/abcdefghijklmnopqrstuvwxyz0.json", 100u, 1u,
	          0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(strlen("www/presets/abcdefghijklmnopqrstuvwxyz0.json") == 44u);
	CHECK(put("www/presets/abcdefghijklmnopqrstuvwxyz01.json", 100u, 1u,
	          0u) == AIUS_WEB_CACHE_PUT_OK); /* 45 */
	CHECK(put("www/presets/abcdefghijklmnopqrstuvwxyz012.json", 100u, 1u,
	          0u) == AIUS_WEB_CACHE_PUT_INVALID); /* 46 */
}

static void test_commit_seq_crc(void)
{
	bool fault = false;
	AiusWebCacheHit h;
	uint8_t s;

	fresh();
	CHECK(put("dsp.json", 700u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	s = slot_of("dsp.json");
	CHECK(s != AIUS_WEB_CACHE_NO_ENTRY);
	g_carve[(size_t)s * 80u + 72u] ^= 0x01u; /* commit_seq bit */
	CHECK(!aius_web_cache_lookup(&g_cache, "dsp.json", 8u, &h, &fault));
	CHECK(fault);
	CHECK(E(s)->state == AIUS_WEB_CACHE_STATE_DIRTY); /* kept: Q2 */
	CHECK((E(s)->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u);
	/* A CRC-faulted entry is never picked for a flush. */
	CHECK(!aius_web_cache_flush_begin(&g_cache, s));
}

static void test_putting(void)
{
	uint8_t s;

	fresh();
	CHECK(aius_web_cache_put_begin(&g_cache, "www/a.js", 8u, 1500u, 0u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	s = g_cache.put_entry;
	CHECK(E(s)->state == AIUS_WEB_CACHE_STATE_PUTTING);
	CHECK(aius_web_cache_put_active(&g_cache));
	CHECK(!hit("www/a.js", NULL));
	aius_web_cache_invalidate(&g_cache, NULL, 0u);
	CHECK(E(s)->state == AIUS_WEB_CACHE_STATE_PUTTING);
	/* A second streamed put is refused while the slot is held. */
	CHECK(aius_web_cache_put_begin(&g_cache, "www/b.js", 8u, 10u, 0u) ==
	      AIUS_WEB_CACHE_PUT_INVALID);
	/* Fill, resume and publish refuse a PUTTING path; not shadowed. */
	CHECK(!aius_web_cache_fill_begin(&g_cache, "www/a.js", 8u, 1500u));
	CHECK(aius_web_cache_pending_state(&g_cache, "www/a.js", 8u) ==
	      AIUS_WEB_CACHE_STATE_PUTTING);
	aius_web_cache_put_abort(&g_cache);
	CHECK(E(s)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(!aius_web_cache_put_active(&g_cache));

	/* Chip read-back mismatch aborts the put. */
	CHECK(aius_web_cache_put_begin(&g_cache, "www/a.js", 8u, 1500u, 0u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	s = g_cache.put_entry;
	g_bad_chip = g_carve + g_cache.data_offset + E(s)->start * 512u + 600u;
	pattern(g_buf, 1500u, 3u);
	CHECK(aius_web_cache_put_chunk(&g_cache, g_buf, 512u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(aius_web_cache_put_chunk(&g_cache, g_buf + 512u, 512u) ==
	      AIUS_WEB_CACHE_PUT_FAULT);
	CHECK(E(s)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(!aius_web_cache_put_active(&g_cache));
	g_bad_chip = NULL;

	/* No free run: refused, nothing evicted. */
	CHECK(fill("www/big.bin", 2000000u, 4u));
	CHECK(aius_web_cache_put_begin(&g_cache, "www/c.bin", 9u, 200000u, 0u) ==
	      AIUS_WEB_CACHE_PUT_FULL);
	CHECK(hit("www/big.bin", NULL));
	/* No free entry: refused. */
	fresh();
	for (unsigned i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/f%u", i);
		CHECK(fill(p, 10u, i));
	}
	CHECK(aius_web_cache_put_begin(&g_cache, "www/x", 5u, 10u, 0u) ==
	      AIUS_WEB_CACHE_PUT_FULL);
	CHECK(put("dsp.json", 10u, 1u, 0u) == AIUS_WEB_CACHE_PUT_FULL);
	CHECK(aius_web_cache_entry_count(&g_cache) == 64u);
}

static void test_put_commit_retires(void)
{
	uint8_t v;
	uint8_t d1;
	uint8_t d2;
	AiusWebCacheHit h;

	fresh();
	CHECK(fill("www/x.js", 1200u, 10u));
	v = slot_of("www/x.js");
	CHECK(aius_web_cache_lease(&g_cache, v));
	CHECK(stream("www/x.js", 900u, 11u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(E(v)->state == AIUS_WEB_CACHE_STATE_STALE);
	CHECK(serves(v, 1200u, 10u)); /* leased send finishes from old bytes */
	aius_web_cache_release(&g_cache, v);
	CHECK(E(v)->state == AIUS_WEB_CACHE_STATE_FREE);
	d1 = slot_of("www/x.js");
	CHECK(E(d1)->state == AIUS_WEB_CACHE_STATE_DIRTY);
	CHECK(serves(d1, 900u, 11u));
	/* An older DIRTY is retired by the next put. */
	CHECK(put("www/x.js", 800u, 12u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(E(d1)->state == AIUS_WEB_CACHE_STATE_FREE);
	d2 = slot_of("www/x.js");
	/* A FLUSHING entry stays pinned under a newer put. */
	CHECK(aius_web_cache_flush_begin(&g_cache, d2));
	CHECK(put("www/x.js", 700u, 13u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(E(d2)->state == AIUS_WEB_CACHE_STATE_FLUSHING);
	CHECK(hit("www/x.js", &h) && h.length == 700u); /* newest wins */
	aius_web_cache_flush_end(&g_cache, d2, true);
	CHECK(E(d2)->state == AIUS_WEB_CACHE_STATE_FREE);
	/* The path's FILLING partial is dropped by a put. */
	CHECK(aius_web_cache_fill_begin(&g_cache, "www/y.js", 8u, 2000u) ==
	      true);
	pattern(g_buf, 2000u, 14u);
	CHECK(aius_web_cache_fill_chunk(&g_cache, g_buf, 512u));
	CHECK(put("www/y.js", 50u, 15u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(g_cache.fill_entry == AIUS_WEB_CACHE_NO_ENTRY);
	CHECK(!aius_web_cache_fill_begin(&g_cache, "www/y.js", 8u, 2000u));
	CHECK(aius_web_cache_fill_resume_offset(&g_cache, "www/y.js", 8u) == 0u);
}

static void test_flush_end(void)
{
	uint8_t n;
	uint8_t t;

	fresh();
	CHECK(put("www/a", 600u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	n = slot_of("www/a");
	CHECK(aius_web_cache_flush_begin(&g_cache, n));
	aius_web_cache_flush_end(&g_cache, n, true);
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_VALID);
	CHECK(aius_web_cache_entry_count(&g_cache) == 1u);

	/* Newer DIRTY: retired, FREE unleased, STALE leased then FREE. */
	CHECK(put("www/b", 600u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	n = slot_of("www/b");
	CHECK(aius_web_cache_flush_begin(&g_cache, n));
	CHECK(aius_web_cache_lease(&g_cache, n));
	CHECK(put("www/b", 500u, 3u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	aius_web_cache_flush_end(&g_cache, n, true);
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_STALE);
	aius_web_cache_release(&g_cache, n);
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_FREE);

	/* Failure with nothing newer returns to DIRTY. */
	n = slot_of("www/b");
	CHECK(aius_web_cache_flush_begin(&g_cache, n));
	aius_web_cache_flush_end(&g_cache, n, false);
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_DIRTY);

	/* TOMBSTONE: failed DELETE stays, completed one frees. */
	CHECK(aius_web_cache_tombstone(&g_cache, "www/b", 5u, 0u, 2000u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	t = 0xFF;
	for (uint8_t i = 0; i < 64u; i++)
		if (E(i)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
			t = i;
	CHECK(t == n); /* reused in place */
	CHECK(aius_web_cache_flush_begin(&g_cache, t));
	aius_web_cache_flush_end(&g_cache, t, false);
	CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE);
	CHECK(aius_web_cache_flush_begin(&g_cache, t));
	aius_web_cache_flush_end(&g_cache, t, true);
	CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(!hit("www/b", NULL));
}

static void test_tombstone_cap(void)
{
	fresh();
	for (unsigned i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/presets/p%u.json", i);
		CHECK(fill(p, 300u, i));
	}
	CHECK(aius_web_cache_tombstone(&g_cache, "www/presets/p3.json", 19u, 0u,
	                               1u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(!hit("www/presets/p3.json", NULL));
	CHECK(aius_web_cache_tombstone(&g_cache, "www/presets/zz.json", 19u, 0u,
	                               1u) == AIUS_WEB_CACHE_PUT_FULL);
}

static void test_delete_during_flush(bool ok)
{
	uint8_t n;
	uint8_t t = 0xFF;

	fresh();
	CHECK(put("www/x", 600u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	n = slot_of("www/x");
	CHECK(aius_web_cache_flush_begin(&g_cache, n));
	CHECK(aius_web_cache_tombstone(&g_cache, "www/x", 5u, 0u, 1u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	for (uint8_t i = 0; i < 64u; i++)
		if (E(i)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
			t = i;
	CHECK(t != 0xFF && t != n); /* a new entry */
	CHECK(!hit("www/x", NULL));
	CHECK(list("www/", false) == 0u);
	aius_web_cache_flush_end(&g_cache, n, ok);
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(!hit("www/x", NULL));
	CHECK(aius_web_cache_flush_begin(&g_cache, t));
	aius_web_cache_flush_end(&g_cache, t, true);
	CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(!hit("www/x", NULL));
}

static void test_delete_while_leased(void)
{
	uint8_t v;

	fresh();
	CHECK(fill("www/x", 1500u, 5u));
	v = slot_of("www/x");
	CHECK(aius_web_cache_lease(&g_cache, v));
	CHECK(aius_web_cache_tombstone(&g_cache, "www/x", 5u, 0u, 1u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(E(v)->state == AIUS_WEB_CACHE_STATE_STALE);
	CHECK(serves(v, 1500u, 5u));
	CHECK(!hit("www/x", NULL));
	aius_web_cache_release(&g_cache, v);
	CHECK(E(v)->state == AIUS_WEB_CACHE_STATE_FREE);
}

static void test_safe_copy(void)
{
	uint8_t n;
	uint8_t n1;
	uint8_t p;

	/* Streamed commit takes the bit as it stands at commit (never on
	 * PUTTING). */
	fresh();
	CHECK(put("www/presets/a.json", 600u, 1u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	n = slot_of("www/presets/a.json");
	CHECK(!safe_backup(n));
	CHECK(aius_web_cache_put_begin(&g_cache, "www/presets/a.json", 18u, 100u,
	                               AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	p = g_cache.put_entry;
	CHECK(aius_web_cache_set_safe_copy(&g_cache, "www/presets/a.json", 18u,
	                                   AIUS_WEB_CACHE_SAFE_BACKUP));
	CHECK(safe_backup(n) && !safe_backup(p));
	pattern(g_buf, 100u, 2u);
	CHECK(aius_web_cache_put_chunk(&g_cache, g_buf, 100u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(aius_web_cache_put_commit(&g_cache, 5u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(safe_backup(p));
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_FREE);

	/* E2: N FLUSHING (target), N+1 put: bit on N+1 only; the backup-pass
	 * CLOSE lands on N+1; a failure retires N, N+1 stays backup. */
	fresh();
	CHECK(put("dsp.json", 12288u, 1u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	n = slot_of("dsp.json");
	CHECK(aius_web_cache_flush_begin(&g_cache, n));
	CHECK(put("dsp.json", 12288u, 2u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	n1 = slot_of("dsp.json");
	CHECK(n1 != n && !safe_backup(n) && !safe_backup(n1));
	CHECK(aius_web_cache_set_safe_copy(&g_cache, "dsp.json", 8u,
	                                   AIUS_WEB_CACHE_SAFE_BACKUP));
	CHECK(safe_backup(n1) && !safe_backup(n));
	aius_web_cache_flush_end(&g_cache, n, false);
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(E(n1)->state == AIUS_WEB_CACHE_STATE_DIRTY && safe_backup(n1));
	CHECK(count(AIUS_WEB_CACHE_STATE_DIRTY) == 1u);

	/* No put mid-job: the bit stays on the FLUSHING entry and survives
	 * its return to DIRTY. */
	CHECK(aius_web_cache_flush_begin(&g_cache, n1));
	CHECK(aius_web_cache_set_safe_copy(&g_cache, "dsp.json", 8u,
	                                   AIUS_WEB_CACHE_SAFE_BACKUP));
	aius_web_cache_flush_end(&g_cache, n1, false);
	CHECK(E(n1)->state == AIUS_WEB_CACHE_STATE_DIRTY && safe_backup(n1));

	/* A restore-created first put starts at backup. */
	fresh();
	CHECK(put("config.json", 300u, 1u,
	          AIUS_WEB_CACHE_FLAG_SETTINGS |
	                  AIUS_WEB_CACHE_FLAG_RESTORE_CREATED) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(safe_backup(slot_of("config.json")));
}

static void test_cap_and_presets(void)
{
	unsigned i;
	uint8_t t = 0xFF;

	fresh();
	for (i = 0; i < 27u; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/f%u.js", i);
		CHECK(fill(p, 20000u + i * 1000u, i));
	}
	for (i = 0; i < 40u && aius_web_cache_entry_count(&g_cache) < 64u; i++) {
		char p[40];

		snprintf(p, sizeof(p), "www/presets/preset-%u.json", i);
		CHECK(fill(p, 1400u + i * 13u, 100u + i));
	}
	CHECK(aius_web_cache_entry_count(&g_cache) == 64u);
	/* An Apply's put refuses (the documented 503 sd_busy). */
	CHECK(aius_web_cache_put_begin(&g_cache, "dsp.json", 8u, 12288u, 0u) ==
	      AIUS_WEB_CACHE_PUT_FULL);
	CHECK(put("dsp.json", 12288u, 1u, 0u) == AIUS_WEB_CACHE_PUT_FULL);
	/* A DELETE of a cached preset lands and frees its entry once flushed. */
	CHECK(aius_web_cache_tombstone(&g_cache, "www/presets/preset-0.json",
	                               25u, 0u, 1u) == AIUS_WEB_CACHE_PUT_OK);
	for (uint8_t s = 0; s < 64u; s++)
		if (E(s)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
			t = s;
	CHECK(t != 0xFF);
	CHECK(aius_web_cache_flush_begin(&g_cache, t));
	aius_web_cache_flush_end(&g_cache, t, true);
	CHECK(count(AIUS_WEB_CACHE_STATE_FREE) == 1u);
	CHECK(put("dsp.json", 12288u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
}

/* Free run lengths: the largest one, in chunks. */
static uint32_t largest_free_run(void)
{
	static uint8_t used[8192];
	uint32_t best = 0u;
	uint32_t run = 0u;

	memset(used, 0, sizeof(used));
	for (uint8_t i = 0; i < 64u; i++) {
		const AiusWebCacheEntry *e = E(i);

		if (e->state == AIUS_WEB_CACHE_STATE_FREE)
			continue;
		for (uint32_t c = e->start; c < (uint32_t)e->start + e->chunks; c++)
			used[c] = 1u;
	}
	for (uint32_t c = 0; c < g_cache.data_chunks; c++) {
		run = used[c] ? 0u : run + 1u;
		if (run > best)
			best = run;
	}
	return best;
}

static void test_fragmentation(void)
{
	/* The www set's shape: openapi.json and dsp-editor.js at their real
	 * sizes, 25 more files making up 867,345 B, plus five bundles. */
	static uint32_t sizes[32];
	const unsigned files = 32u;
	uint32_t rest = 867345u - 262071u - 177029u;

	sizes[0] = 262071u;
	sizes[1] = 177029u;
	for (unsigned i = 2; i < 27u; i++) {
		sizes[i] = rest / (27u - i) + (i % 5u) * 997u;
		if (sizes[i] > rest)
			sizes[i] = rest;
		rest -= sizes[i];
	}
	for (unsigned i = 27; i < files; i++)
		sizes[i] = 20000u + i * 777u; /* bundles */
	fresh();
	for (unsigned i = 0; i < files; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/f%u", i);
		CHECK(fill(p, sizes[i], i));
	}
	CHECK(put("dsp.json", 12288u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	for (unsigned sync = 0; sync < 5u; sync++) {
		for (unsigned k = 0; k < files; k++) {
			/* Replay order rotates so runs interleave. */
			const unsigned i = (k + sync * 7u) % files;
			char p[32];
			uint8_t s;

			snprintf(p, sizeof(p), "www/f%u", i);
			CHECK(stream(p, sizes[i] + sync, 1000u + sync) ==
			      AIUS_WEB_CACHE_PUT_OK);
			s = slot_of(p);
			CHECK(aius_web_cache_flush_begin(&g_cache, s));
			aius_web_cache_flush_end(&g_cache, s, true);
		}
		printf("web-cache-store: sync %u largest free run %u chunks\n",
		       sync + 1u, (unsigned)largest_free_run());
		CHECK(largest_free_run() >= (262071u + 511u) / 512u);
	}
}

static void test_newest_and_wrap(void)
{
	AiusWebCacheHit h;
	uint8_t n;

	fresh();
	g_cache.commit_seq = UINT32_C(0xFFFFFFFE);
	CHECK(put("www/a", 100u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK); /* ...FF */
	n = slot_of("www/a");
	CHECK(aius_web_cache_flush_begin(&g_cache, n));
	CHECK(put("www/a", 200u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK); /* 0 */
	CHECK(put("www/a", 300u, 3u, 0u) == AIUS_WEB_CACHE_PUT_OK); /* 1: wraps */
	CHECK(hit("www/a", &h) && h.length == 300u);
	CHECK(count(AIUS_WEB_CACHE_STATE_DIRTY) == 1u);
	CHECK(list("www/", false) == 1u);
	/* A newer TOMBSTONE misses while the FLUSHING one is pinned. */
	CHECK(aius_web_cache_tombstone(&g_cache, "www/a", 5u, 0u, 1u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(!hit("www/a", NULL));
	CHECK(list("www/", false) == 0u);
	CHECK(E(n)->state == AIUS_WEB_CACHE_STATE_FLUSHING);
}

static void test_g1_tombstone(void)
{
	uint8_t t = 0xFF;

	/* Non-running TOMBSTONE retired by a re-put. */
	fresh();
	CHECK(fill("www/x", 100u, 1u));
	CHECK(aius_web_cache_tombstone(&g_cache, "www/x", 5u, 0u, 1u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	for (uint8_t s = 0; s < 64u; s++)
		if (E(s)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
			t = s;
	CHECK(aius_web_cache_flush_begin(&g_cache, t));
	aius_web_cache_flush_end(&g_cache, t, false); /* backs off */
	CHECK(put("www/x", 200u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(count(AIUS_WEB_CACHE_STATE_TOMBSTONE) == 0u);
	CHECK(hit("www/x", NULL));

	/* A running one stays pinned; _flush_end retires it either way. */
	for (int ok = 0; ok < 2; ok++) {
		fresh();
		CHECK(aius_web_cache_tombstone(&g_cache, "www/x", 5u, 0u, 1u) ==
		      AIUS_WEB_CACHE_PUT_OK);
		for (uint8_t s = 0; s < 64u; s++)
			if (E(s)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
				t = s;
		CHECK(aius_web_cache_flush_begin(&g_cache, t));
		CHECK(put("www/x", 200u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK);
		CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE);
		CHECK(hit("www/x", NULL));
		aius_web_cache_flush_end(&g_cache, t, ok != 0);
		CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_FREE);
		CHECK(count(AIUS_WEB_CACHE_STATE_DIRTY) == 1u);
	}
}

/* D-B1: a tombstone whose path takes a bit flip (a.json -> c.json) fails its
 * entry CRC: flagged on the chip read (never re-stamped by it), refused by
 * _flush_begin so no DELETE can start for either path, and _drop_pending
 * leaves nothing pending for either. */
static void test_faulted_tombstone_path_flip(void)
{
	AiusWebCacheEntryInfo info;
	uint8_t t = 0xFF;

	fresh();
	CHECK(fill("www/presets/c.json", 100u, 1u));
	CHECK(aius_web_cache_tombstone(&g_cache, "www/presets/a.json", 18u,
	                               AIUS_WEB_CACHE_FLAG_SETTINGS, 1u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	for (uint8_t s = 0; s < 64u; s++)
		if (E(s)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
			t = s;
	CHECK(t != 0xFF);
	if (t == 0xFF)
		return;
	E(t)->path[12] ^= 0x02u;
	CHECK(strcmp(E(t)->path, "www/presets/c.json") == 0);
	for (int pass = 0; pass < 2; pass++) {
		CHECK(aius_web_cache_entry_info(&g_cache, t, &info));
		CHECK((info.flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u);
		CHECK(info.state == AIUS_WEB_CACHE_STATE_TOMBSTONE);
		CHECK(!aius_web_cache_flush_begin(&g_cache, t));
		CHECK((E(t)->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) == 0u);
	}
	CHECK(aius_web_cache_pending_state(&g_cache, "www/presets/a.json", 18u) ==
	      AIUS_WEB_CACHE_STATE_FREE);
	CHECK(aius_web_cache_drop_pending(&g_cache, t));
	CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(aius_web_cache_pending_state(&g_cache, "www/presets/a.json", 18u) ==
	      AIUS_WEB_CACHE_STATE_FREE);
	CHECK(aius_web_cache_pending_state(&g_cache, "www/presets/c.json", 18u) ==
	      AIUS_WEB_CACHE_STATE_FREE);
	CHECK(count(AIUS_WEB_CACHE_STATE_TOMBSTONE) == 0u &&
	      count(AIUS_WEB_CACHE_STATE_DIRTY) == 0u);
	/* c.json's card copy is still served: no delete reached it. */
	CHECK(hit("www/presets/c.json", NULL));
}

/* CR-S1: a flipped start (onto another entry's chunks, whose table CRCs
 * match) or length never yields bytes: copy_chunk re-checks the entry CRC
 * and flags a pending entry CRC_FAULTED | ENTRY_FAULTED; _flush_begin
 * refuses an unflagged entry whose CRC fails; a clean one is dropped. */
static void test_entry_field_flip(void)
{
	uint8_t chunk[512];
	uint32_t n = 0u;
	uint8_t a;
	uint8_t b;

	fresh();
	CHECK(put("www/presets/a.json", 700u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(put("www/presets/b.json", 700u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	a = slot_of("www/presets/a.json");
	b = slot_of("www/presets/b.json");
	CHECK(aius_web_cache_flush_begin(&g_cache, a)); /* FLUSHING */
	CHECK(aius_web_cache_copy_chunk(&g_cache, a, 0u, chunk, &n) && n == 512u);
	E(a)->start = E(b)->start;
	CHECK(!aius_web_cache_copy_chunk(&g_cache, a, 0u, chunk, &n));
	CHECK(!aius_web_cache_copy_chunk(&g_cache, a, 1u, chunk, &n));
	CHECK(E(a)->state == AIUS_WEB_CACHE_STATE_FLUSHING);
	CHECK((E(a)->flags & (AIUS_WEB_CACHE_FLAG_CRC_FAULTED |
	                      AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED)) ==
	      (AIUS_WEB_CACHE_FLAG_CRC_FAULTED | AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED));
	CHECK(serves(b, 700u, 2u)); /* b untouched */

	/* A DIRTY entry's length flip, not yet flagged: no flush starts. */
	E(b)->length ^= 1u;
	CHECK((E(b)->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) == 0u);
	CHECK(!aius_web_cache_flush_begin(&g_cache, b));
	CHECK(E(b)->state == AIUS_WEB_CACHE_STATE_DIRTY);
	CHECK((E(b)->flags & AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED) != 0u);

	/* A chunk fault alone flags CRC_FAULTED only: the path stays trusted. */
	fresh();
	CHECK(put("dsp.json", 700u, 3u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	a = slot_of("dsp.json");
	g_carve[g_cache.data_offset + (size_t)E(a)->start * 512u] ^= 0x01u;
	CHECK(!aius_web_cache_copy_chunk(&g_cache, a, 0u, chunk, &n));
	CHECK((E(a)->flags & (AIUS_WEB_CACHE_FLAG_CRC_FAULTED |
	                      AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED)) ==
	      AIUS_WEB_CACHE_FLAG_CRC_FAULTED);

	/* A clean entry's start flip: no bytes, the entry dropped. */
	fresh();
	CHECK(fill("www/x.js", 700u, 4u));
	CHECK(fill("www/y.js", 700u, 5u));
	a = slot_of("www/x.js");
	b = slot_of("www/y.js");
	E(a)->start = E(b)->start;
	CHECK(!aius_web_cache_copy_chunk(&g_cache, a, 0u, chunk, &n));
	CHECK(E(a)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(!hit("www/x.js", NULL) && hit("www/y.js", NULL));
}

/* CR3-B1: with no free entry, a one-shot put reuses its own path's
 * unpinned entry in place (clean or pending, its run counted free, a pending
 * one's safe-copy bit kept); a FLUSHING or leased one, or no entry of the
 * path, stays FULL. */
static void test_put_in_place_when_full(void)
{
	char p[32];
	uint8_t d;

	fresh();
	CHECK(fill("dsp.json", 100u, 1u));
	for (unsigned i = 0; i + 1u < AIUS_WEB_CACHE_ENTRIES; i++) {
		snprintf(p, sizeof(p), "www/f%u", i);
		CHECK(put(p, 10u, i, 0u) == AIUS_WEB_CACHE_PUT_OK);
	}
	d = slot_of("dsp.json");
	CHECK(put("config.json", 10u, 1u, 0u) == AIUS_WEB_CACHE_PUT_FULL);
	CHECK(put("dsp.json", 1500u, 2u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(slot_of("dsp.json") == d && serves(d, 1500u, 2u));
	CHECK(E(d)->state == AIUS_WEB_CACHE_STATE_DIRTY);
	CHECK(serves(slot_of("www/f0"), 10u, 0u));
	/* A pending entry reused in place keeps the path's safe copy. */
	CHECK(aius_web_cache_set_safe_copy(&g_cache, "dsp.json", 8u,
	                                   AIUS_WEB_CACHE_SAFE_BACKUP));
	CHECK(put("dsp.json", 700u, 3u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(slot_of("dsp.json") == d && serves(d, 700u, 3u) && safe_backup(d));
	/* Pinned: FLUSHING, or leased. */
	CHECK(aius_web_cache_flush_begin(&g_cache, d));
	CHECK(put("dsp.json", 700u, 4u, 0u) == AIUS_WEB_CACHE_PUT_FULL);
	aius_web_cache_flush_end(&g_cache, d, true);
	CHECK(aius_web_cache_lease(&g_cache, d));
	CHECK(put("dsp.json", 700u, 4u, 0u) == AIUS_WEB_CACHE_PUT_FULL);
	aius_web_cache_release(&g_cache, d);
	CHECK(put("dsp.json", 700u, 4u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(slot_of("dsp.json") == d && serves(d, 700u, 4u));
}

static void test_fill_refuses_pending(void)
{
	fresh();
	CHECK(put("www/a", 100u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(!aius_web_cache_fill_begin(&g_cache, "www/a", 5u, 100u));
	CHECK(aius_web_cache_pending_state(&g_cache, "www/a", 5u) ==
	      AIUS_WEB_CACHE_STATE_DIRTY);
	/* A fill begun before the put cannot publish over it. */
	CHECK(aius_web_cache_fill_begin(&g_cache, "www/b", 5u, 100u));
	pattern(g_buf, 100u, 9u);
	CHECK(aius_web_cache_fill_chunk(&g_cache, g_buf, 100u));
	CHECK(aius_web_cache_put_begin(&g_cache, "www/b", 5u, 10u, 0u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(!aius_web_cache_publish(&g_cache));
	CHECK(!aius_web_cache_fill_resume(&g_cache, "www/b", 5u, 100u, 100u));
	aius_web_cache_put_abort(&g_cache);
	CHECK(aius_web_cache_pending_state(&g_cache, "www/b", 5u) ==
	      AIUS_WEB_CACHE_STATE_FREE);
}

static void test_oneshot_beside_stream(void)
{
	fresh();
	CHECK(aius_web_cache_put_begin(&g_cache, "www/openapi.json", 16u,
	                               262071u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(put("dsp.json", 12288u, 7u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(aius_web_cache_put_active(&g_cache));
	CHECK(serves(slot_of("dsp.json"), 12288u, 7u));
}

static unsigned g_put_yields;
static unsigned g_put_yield_misses;

static void count_put_yield(void *context)
{
	CHECK(context == &g_put_yields);
	g_put_yields++;
	/* Mid-put the entry is PUTTING: a nested pass's lookup misses it. */
	if (!hit("dsp.json", NULL))
		g_put_yield_misses++;
}

/* Phase 4h: the one-shot _put yields to the producer once per chunk; the
 * streamed sink's _put_chunk (one chunk per call) takes none. */
static void test_put_yields_per_chunk(void)
{
	fresh();
	aius_web_cache_set_put_yield(count_put_yield, &g_put_yields);
	g_put_yields = g_put_yield_misses = 0u;
	CHECK(put("dsp.json", 12288u, 7u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(g_put_yields == 24u && g_put_yield_misses == 24u);
	CHECK(serves(slot_of("dsp.json"), 12288u, 7u));
	g_put_yields = 0u;
	CHECK(put("config.json", 513u, 8u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_OK);
	CHECK(g_put_yields == 2u); /* a tail chunk yields too */
	g_put_yields = 0u;
	CHECK(stream("www/a.js", 2048u, 9u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(g_put_yields == 0u);
	/* A chip fault in the third chunk stops the put after two yields. */
	fresh();
	g_put_yields = 0u;
	g_bad_chip = g_carve + g_cache.data_offset + 2u * 512u + 5u;
	CHECK(put("dsp.json", 12288u, 7u, AIUS_WEB_CACHE_FLAG_SETTINGS) ==
	      AIUS_WEB_CACHE_PUT_FAULT);
	CHECK(g_put_yields == 2u);
	aius_web_cache_set_put_yield(NULL, NULL);
	g_put_yields = 0u;
	fresh();
	CHECK(put("dsp.json", 1024u, 7u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(g_put_yields == 0u);
}

static void test_invalidate_leaves_pending(void)
{
	fresh();
	CHECK(put("www/presets/a.json", 100u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(put("www/b.js", 100u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(aius_web_cache_flush_begin(&g_cache, slot_of("www/b.js")));
	CHECK(fill("www/c.js", 100u, 3u));
	CHECK(aius_web_cache_tombstone(&g_cache, "www/d.js", 8u, 0u, 1u) ==
	      AIUS_WEB_CACHE_PUT_OK);
	aius_web_cache_invalidate(&g_cache, NULL, 0u);
	aius_web_cache_invalidate(&g_cache, "www/", 4u);
	aius_web_cache_invalidate(&g_cache, "www/presets/a.json", 18u);
	CHECK(count(AIUS_WEB_CACHE_STATE_DIRTY) == 1u);
	CHECK(count(AIUS_WEB_CACHE_STATE_FLUSHING) == 1u);
	CHECK(count(AIUS_WEB_CACHE_STATE_TOMBSTONE) == 1u);
	CHECK(!hit("www/c.js", NULL));
}

static void test_case_folding_and_listing(void)
{
	AiusWebCacheHit h;

	fresh();
	CHECK(fill("www/index.html", 100u, 1u));
	CHECK(hit("WWW/Index.HTML", NULL));
	aius_web_cache_invalidate(&g_cache, "WWW/", 4u);
	CHECK(!hit("www/index.html", NULL));
	CHECK(fill("www/index.html", 100u, 1u));
	aius_web_cache_invalidate(&g_cache, "www/INDEX.html", 14u);
	CHECK(!hit("www/index.html", NULL));

	/* A case-variant put replaces the entry; the latest spelling stays. */
	CHECK(put("www/presets/a.json", 100u, 1u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(put("www/presets/A.json", 120u, 2u, 0u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(count(AIUS_WEB_CACHE_STATE_DIRTY) == 1u);
	CHECK(hit("www/presets/a.json", &h) && h.length == 120u);
	CHECK(list("WWW/PRESETS/", false) == 1u && listed("A.json"));

	/* Listing: pending rows, tombstones omitted; directory once. */
	CHECK(fill("www/presets/b.json", 100u, 3u));
	CHECK(fill("www/presets/c.json", 100u, 4u));
	CHECK(aius_web_cache_tombstone(&g_cache, "www/presets/c.json", 18u, 0u,
	                               1u) == AIUS_WEB_CACHE_PUT_OK);
	CHECK(list("www/presets/", false) == 2u);
	CHECK(listed("A.json") && listed("b.json") && !listed("c.json"));
	CHECK(list("www/presets/", true) == 1u && listed("A.json"));
	CHECK(list("www/", false) == 1u && listed("presets"));
}

int main(void)
{
	test_layout();
	test_commit_seq_crc();
	test_putting();
	test_put_commit_retires();
	test_flush_end();
	test_tombstone_cap();
	test_delete_during_flush(true);
	test_delete_during_flush(false);
	test_delete_while_leased();
	test_safe_copy();
	test_cap_and_presets();
	test_fragmentation();
	test_newest_and_wrap();
	test_g1_tombstone();
	test_faulted_tombstone_path_flip();
	test_entry_field_flip();
	test_put_in_place_when_full();
	test_fill_refuses_pending();
	test_oneshot_beside_stream();
	test_put_yields_per_chunk();
	test_invalidate_leaves_pending();
	test_case_folding_and_listing();
	if (g_failures != 0) {
		fprintf(stderr, "web-cache-store: %d of %d checks failed\n",
		        g_failures, g_checks);
		return 1;
	}
	printf("web-cache-store: %d checks passed\n", g_checks);
	return 0;
}
