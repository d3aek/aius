/* Host suite for firmware/lib/aius_sd_overlay over the real store
 * (aius-sample-rate-following.md Phase 4h, "host: sd-overlay"): latest wins,
 * tombstones, the version pin, class order, read-back retry, the card-loss
 * hold, S5 backoff, Q2 re-put and drop, safe-copy order (FR-B1, FR2-S1, E1),
 * G1, S2, FR-S9, Q4, case folding, shadows, drain, the burst refresh, the
 * job requests and the restore decision; the FR-S7 job-request mapping; and
 * the FR-S6 power-cut model: lib/aius_sd driving fake_card.c, judged by the
 * real dsp.json and config.json parsers. */
#include "aius_sd_overlay.h"
#include "aius_sd_overlay_request.h"

#include "aius_config.h"
#include "aius_dsp_config.h"
#include "fake_card.h"

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
static AiusSdOverlay g_ov;
static uint8_t g_buf[16384];
static uint8_t g_reput_bytes[2][600];
static unsigned g_reputs;
static bool g_reput_busy;
static unsigned g_events;
/* The power-cut model's running settings, when set: a real version. */
static const uint8_t *g_reput_src[2];
static uint32_t g_reput_src_length[2];

/* Armed: the next data chunk flushed to the chip (a put's write) reads back
 * with one bit flipped. */
static bool g_write_fault;

static void flush_delete(void *addr, uint32_t bytes)
{
	uint8_t *p = (uint8_t *)addr;

	if (g_write_fault && bytes == AIUS_WEB_CACHE_CHUNK_BYTES &&
	    p >= g_carve + g_cache.data_offset) {
		g_write_fault = false;
		p[0] ^= 0x01u;
	}
}

static bool reput(void *ctx, AiusSdOverlaySettingsFile file,
                  const uint8_t **bytes, uint32_t *length)
{
	(void)ctx;
	if (g_reput_busy)
		return false;
	g_reputs++;
	*bytes = g_reput_src[file] != NULL ? g_reput_src[file]
	                                   : g_reput_bytes[file];
	*length = g_reput_src[file] != NULL ? g_reput_src_length[file]
	                                    : sizeof(g_reput_bytes[file]);
	return true;
}

static void event(void *ctx, AiusSdOverlayEvent ev, const char *path)
{
	(void)ctx;
	(void)path;
	if (ev == AIUS_SD_OVERLAY_EVENT_CRC_DROP)
		g_events++;
}

static const AiusSdOverlayPlatform k_platform = {&g_cache, reput, event, NULL};
static const AiusSdOverlayPlatform k_absent = {NULL, reput, event, NULL};

static void fresh(bool held)
{
	CHECK(aius_web_cache_init(&g_cache, g_carve, sizeof(g_carve),
	                          flush_delete, NULL));
	aius_sd_overlay_init(&g_ov, &k_platform, held);
	g_reputs = 0u;
	g_reput_busy = false;
	g_events = 0u;
	g_reput_src[0] = g_reput_src[1] = NULL;
	memset(g_reput_bytes[0], 'D', sizeof(g_reput_bytes[0]));
	memset(g_reput_bytes[1], 'C', sizeof(g_reput_bytes[1]));
}

static void pattern(uint8_t *dst, size_t n, uint32_t seed)
{
	for (size_t i = 0; i < n; i++)
		dst[i] = (uint8_t)(seed * 31u + i * 7u);
}

static AiusSdOverlayPutResult put(const char *path, uint32_t length,
                                  uint32_t seed, uint32_t now)
{
	pattern(g_buf, length, seed);
	return aius_sd_overlay_put(&g_ov, path, strlen(path), g_buf, length, now);
}

static AiusSdOverlayPutResult del(const char *path, uint32_t now)
{
	return aius_sd_overlay_tombstone(&g_ov, path, strlen(path), now);
}

static AiusWebCacheEntry *E(uint8_t slot)
{
	return (AiusWebCacheEntry *)(void *)(g_carve +
	                                     slot * AIUS_WEB_CACHE_ENTRY_BYTES);
}

static bool hit(const char *path, AiusWebCacheHit *h)
{
	AiusWebCacheHit tmp;

	return aius_web_cache_lookup(&g_cache, path, strlen(path),
	                             h != NULL ? h : &tmp, NULL);
}

/* The pinned bytes the job reads equal pattern(seed). */
static bool input_is(uint32_t length, uint32_t seed)
{
	static uint8_t got[16384];
	uint32_t n = 0u;

	pattern(g_buf, length, seed);
	return aius_sd_overlay_job_input(&g_ov, 0u, got, sizeof(got), &n) &&
	       n == length && memcmp(got, g_buf, length) == 0;
}

static unsigned pending(void)
{
	AiusSdOverlayStatus s;

	aius_sd_overlay_status(&g_ov, 0u, &s);
	return s.dirty + s.tombstones;
}

/* ---------------------------------------------------------------- tests -- */

static void test_classes_and_paths(void)
{
	char b[AIUS_SD_OVERLAY_PATH_STORAGE];

	CHECK(aius_sd_overlay_classify("dsp.json", 8u) ==
	      AIUS_SD_OVERLAY_CLASS_SETTINGS);
	CHECK(aius_sd_overlay_classify("CONFIG.json", 11u) ==
	      AIUS_SD_OVERLAY_CLASS_SETTINGS);
	CHECK(aius_sd_overlay_classify("www/presets/x.json", 18u) ==
	      AIUS_SD_OVERLAY_CLASS_SETTINGS);
	CHECK(aius_sd_overlay_classify("WWW/Presets/x.json", 18u) ==
	      AIUS_SD_OVERLAY_CLASS_SETTINGS); /* N1 */
	CHECK(aius_sd_overlay_classify("www/index.html", 14u) ==
	      AIUS_SD_OVERLAY_CLASS_SYNC);
	CHECK(aius_sd_overlay_backup_path("dsp.json", 8u, b, sizeof(b)) &&
	      strcmp(b, "/aius/.bak-dsp.json") == 0);
	CHECK(aius_sd_overlay_backup_path("config.json", 11u, b, sizeof(b)) &&
	      strcmp(b, "/aius/.bak-config.json") == 0);
	CHECK(aius_sd_overlay_backup_path("www/presets/Warm.json", 21u, b,
	                                  sizeof(b)) &&
	      strcmp(b, "/aius/.bak-preset-Warm.json") == 0);
	CHECK(!aius_sd_overlay_backup_path("www/presets/old", 15u, b, sizeof(b)));
	CHECK(!aius_sd_overlay_backup_path("www/presets/Warm.json", 21u, b, 20u));
	CHECK(!aius_sd_overlay_backup_path("www/a.js", 8u, b, sizeof(b)));

	/* No carve: the caller keeps today's synchronous write (Q4). */
	aius_sd_overlay_init(&g_ov, &k_absent, false);
	CHECK(put("dsp.json", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_ABSENT);
}

/* N5: only www/presets/<name>.json (no further '/', case-folded) is settings
 * class; any other name under www/presets/ is sync class and accepted as
 * today's REPLACE, never refused at accept. */
static void test_n5_preset_classes(void)
{
	static const char *const sync[] = {
		"www/presets/old", "www/presets/notes.txt", "www/presets/a/b.json",
		"www/presets/.json", "www/presets/x.jso", "www/presets/sub/"};
	AiusSdOverlayJob j;

	CHECK(aius_sd_overlay_classify("WWW/PRESETS/X.JSON", 18u) ==
	      AIUS_SD_OVERLAY_CLASS_SETTINGS);
	CHECK(aius_sd_overlay_classify("www/presets/a.json", 18u) ==
	      AIUS_SD_OVERLAY_CLASS_SETTINGS);
	for (unsigned i = 0; i < sizeof(sync) / sizeof(sync[0]); i++)
		CHECK(aius_sd_overlay_classify(sync[i], strlen(sync[i])) ==
		      AIUS_SD_OVERLAY_CLASS_SYNC);

	fresh(false);
	CHECK(put("www/presets/old", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/presets/a/b.json", 20u, 2u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/presets/p.json", 30u, 3u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	/* Settings first: the preset's copy-before-write. */
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE);
	CHECK(strcmp(j.target_path, "/aius/www/presets/p.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 600u);
	for (unsigned k = 0; k < 2u; k++) {
		CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
		CHECK(j.kind == AIUS_SD_OVERLAY_JOB_REPLACE);
		CHECK(j.backup_path[0] == '\0');
		CHECK(strcmp(j.target_path, "/aius/www/presets/old") == 0 ||
		      strcmp(j.target_path, "/aius/www/presets/a/b.json") == 0);
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 600u);
	}
	CHECK(aius_sd_overlay_drained(&g_ov));
	/* A sync-class tombstone under presets/ is one DELETE. */
	CHECK(del("www/presets/a/b.json", 1000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 1500u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_DELETE &&
	      j.delete_step == AIUS_SD_OVERLAY_DELETE_TARGET);
	CHECK(strcmp(j.target_path, "/aius/www/presets/a/b.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1500u);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

static void test_jobs_and_latest_wins(void)
{
	AiusSdOverlayJob j;
	AiusWebCacheHit h;

	fresh(false);
	CHECK(put("www/presets/a.json", 900u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/presets/A.json", 700u, 2u, 100u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(pending() == 1u); /* a case variant replaces the entry */
	CHECK(!aius_sd_overlay_next_job(&g_ov, 500u, &j)); /* debounce */
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE);
	CHECK(j.order == AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST && !j.skip_backup);
	CHECK(strcmp(j.target_path, "/aius/www/presets/A.json") == 0);
	CHECK(strcmp(j.backup_path, "/aius/.bak-preset-A.json") == 0);
	CHECK(j.length == 700u && input_is(700u, 2u));
	CHECK(!j.require_existing);
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_BACKUP);
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_TARGET);
	aius_sd_overlay_note_readback(&g_ov, true);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 650u);
	CHECK(pending() == 0u && aius_sd_overlay_drained(&g_ov));
	CHECK(hit("www/presets/a.json", &h) && h.length == 700u);
	CHECK(!aius_sd_overlay_next_job(&g_ov, 5000u, &j)); /* one flush */

	/* A sync entry is today's REPLACE. */
	CHECK(put("www/app.js", 1500u, 3u, 1000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 1500u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_REPLACE && j.backup_path[0] == '\0');
	CHECK(strcmp(j.target_path, "/aius/www/app.js") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1600u);

	/* Settings before an older sync entry. */
	CHECK(put("www/b.js", 100u, 4u, 2000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("dsp.json", 100u, 5u, 2100u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 2700u, &j));
	CHECK(strcmp(j.target_path, "/aius/dsp.json") == 0);
	CHECK(strcmp(j.backup_path, "/aius/.bak-dsp.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2800u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 2800u, &j));
	CHECK(strcmp(j.target_path, "/aius/www/b.js") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2900u);
}

static void test_version_pin(void)
{
	AiusSdOverlayJob j;
	AiusWebCacheHit h;

	fresh(false);
	CHECK(put("dsp.json", 3000u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	CHECK(aius_sd_overlay_shadows(&g_ov, "dsp.json", 8u)); /* FLUSHING */
	/* N+1 lands mid-pass: both passes and the CRC stay on N. */
	CHECK(put("dsp.json", 2000u, 2u, 700u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(input_is(3000u, 1u) && j.length == 3000u);
	CHECK(hit("dsp.json", &h) && h.length == 2000u);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 800u);
	CHECK(pending() == 1u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 1300u, &j));
	CHECK(j.length == 2000u && input_is(2000u, 2u));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1400u);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

static void test_readback_and_backoff(void)
{
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	uint32_t t = 600u;
	const uint32_t expect[] = {1000u, 2000u, 4000u, 8000u, 16000u,
	                           32000u, 60000u, 60000u};

	fresh(false);
	CHECK(put("config.json", 400u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, t, &j));
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_BACKUP);
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_TARGET);
	aius_sd_overlay_note_readback(&g_ov, false);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, t);
	aius_sd_overlay_status(&g_ov, t, &s);
	CHECK(s.failures == 1u && s.dirty == 1u);
	/* The failed read-back makes the backup safe: retry writes target first. */
	for (unsigned k = 0; k < sizeof(expect) / sizeof(expect[0]); k++) {
		const uint32_t at = t + expect[k];

		CHECK(!aius_sd_overlay_next_job(&g_ov, at - 1u, &j));
		CHECK(aius_sd_overlay_next_job(&g_ov, at, &j));
		if (k == 0u)
			CHECK(j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST);
		t = at;
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, t);
	}
	/* A success resets the backoff. */
	t += 60000u;
	CHECK(aius_sd_overlay_next_job(&g_ov, t, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, t);
	CHECK(put("config.json", 400u, 2u, t) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, t + 500u, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, t + 500u);
	CHECK(aius_sd_overlay_next_job(&g_ov, t + 1500u, &j));
	/* A failure while held counts no try. */
	aius_sd_overlay_note_card_lost(&g_ov);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, t + 1600u);
	aius_sd_overlay_note_mounted(&g_ov, t + 1700u);
	CHECK(aius_sd_overlay_next_job(&g_ov, t + 1700u, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, t + 1700u);
	/* tries is 2 again (not 3): next try 2 s out. */
	CHECK(!aius_sd_overlay_next_job(&g_ov, t + 3699u, &j));
	CHECK(aius_sd_overlay_next_job(&g_ov, t + 3700u, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, t + 3700u);
}

static void test_held(void)
{
	AiusSdOverlayJob j;
	AiusWebCacheHit h;

	/* Booted without a card: accepted, no flush until a mount (Q1). */
	fresh(true);
	CHECK(put("dsp.json", 500u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/presets/p.json", 500u, 2u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(!aius_sd_overlay_next_job(&g_ov, 10000u, &j));
	CHECK(!aius_sd_overlay_drained(&g_ov));
	aius_sd_overlay_note_mounted(&g_ov, 10000u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 10000u, &j));
	/* Card gone mid-flush: dirty with its safe copy, held. */
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_CARD_GONE, 10100u);
	CHECK(aius_sd_overlay_held(&g_ov));
	CHECK(!aius_sd_overlay_next_job(&g_ov, 20000u, &j));
	/* The card loss drops clean entries only; a refill never replaces. */
	aius_web_cache_invalidate(&g_cache, NULL, 0u);
	CHECK(hit("dsp.json", &h) && hit("www/presets/p.json", NULL));
	CHECK(!aius_web_cache_fill_begin(&g_cache, "dsp.json", 8u, 100u));
	CHECK(aius_sd_overlay_shadows(&g_ov, "dsp.json", 8u));
	/* The same card returns: target first, since the backup closed. */
	aius_sd_overlay_note_mounted(&g_ov, 20000u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 20000u, &j));
	CHECK(strcmp(j.target_path, "/aius/dsp.json") == 0);
	CHECK(j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 20100u);
	/* A mode change sets held. */
	aius_sd_overlay_note_card_lost(&g_ov);
	CHECK(aius_sd_overlay_held(&g_ov));
	CHECK(!aius_sd_overlay_next_job(&g_ov, 30000u, &j));
	aius_sd_overlay_note_mounted(&g_ov, 30000u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 30000u, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 30100u);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

static void corrupt_chunk(const char *path)
{
	AiusWebCacheHit h;

	CHECK(hit(path, &h));
	g_carve[g_cache.data_offset + E(h.entry)->start * 512u + 3u] ^= 0x10u;
}

static void test_q2(void)
{
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	AiusWebCacheHit h;
	uint8_t got[600];
	uint32_t n = 0u;

	fresh(false);
	CHECK(put("dsp.json", 1200u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("config.json", 300u, 2u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/presets/p.json", 300u, 3u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	/* NS6: dsp.json's backup pass CLOSEs, its target pass hits a chunk
	 * fault: the re-put keeps the safe copy (backup). */
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	CHECK(strcmp(j.target_path, "/aius/config.json") == 0 ||
	      strcmp(j.target_path, "/aius/dsp.json") == 0 ||
	      strcmp(j.target_path, "/aius/www/presets/p.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 600u);
	while (aius_sd_overlay_next_job(&g_ov, 700u, &j))
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 700u);
	CHECK(aius_sd_overlay_drained(&g_ov));

	CHECK(put("dsp.json", 1200u, 4u, 1000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 1500u, &j));
	CHECK(j.order == AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST);
	/* Production's settle order (aius_optical_sd_flush_settle): the input
	 * fault, then the pass notes, then _job_end (CR2-S1). */
	corrupt_chunk("dsp.json");
	CHECK(!aius_sd_overlay_job_input(&g_ov, 0u, g_buf, sizeof(g_buf), &n));
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_CHUNK_CRC, 1500u);
	aius_sd_overlay_status(&g_ov, 1500u, &s);
	CHECK(s.failures == 0u); /* not a card failure */
	CHECK(!aius_sd_overlay_next_job(&g_ov, 99999u, &j)); /* skipped */
	g_reput_busy = true; /* HTTP busy: retried next step (D1) */
	aius_sd_overlay_service(&g_ov, 1600u);
	CHECK(g_reputs == 0u && !aius_sd_overlay_drained(&g_ov));
	g_reput_busy = false;
	aius_sd_overlay_service(&g_ov, 1700u);
	CHECK(g_reputs == 1u);
	CHECK(hit("dsp.json", &h) && h.length == 600u &&
	      aius_web_cache_copy_chunk(&g_cache, h.entry, 0u, got, &n) &&
	      got[0] == 'D');
	CHECK(aius_sd_overlay_next_job(&g_ov, 2200u, &j));
	CHECK(j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST); /* NS6 */
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2200u);

	/* config.json: re-put from the running config. */
	CHECK(put("config.json", 300u, 5u, 3000u) == AIUS_SD_OVERLAY_PUT_OK);
	corrupt_chunk("config.json");
	CHECK(aius_sd_overlay_next_job(&g_ov, 3500u, &j));
	CHECK(!aius_sd_overlay_job_input(&g_ov, 0u, g_buf, sizeof(g_buf), &n));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_CHUNK_CRC, 3500u);
	aius_sd_overlay_service(&g_ov, 3600u);
	CHECK(g_reputs == 2u);
	CHECK(hit("config.json", &h) && h.length == 600u);

	/* A preset is dropped, counted and logged (N6: the card's file
	 * returns at the next refill). */
	CHECK(put("www/presets/p.json", 300u, 6u, 4000u) == AIUS_SD_OVERLAY_PUT_OK);
	corrupt_chunk("www/presets/p.json");
	CHECK(!hit("www/presets/p.json", NULL) || true);
	{
		bool fault = false;
		uint8_t e = 0xFF;

		for (uint8_t i = 0; i < 64u; i++)
			if (E(i)->state == AIUS_WEB_CACHE_STATE_DIRTY)
				e = i;
		CHECK(e != 0xFF && !aius_web_cache_verify(&g_cache, e, NULL, NULL));
		(void)fault;
	}
	aius_sd_overlay_service(&g_ov, 4100u);
	aius_sd_overlay_status(&g_ov, 4100u, &s);
	CHECK(s.crc_drops == 1u && g_events == 1u);
	CHECK(!aius_sd_overlay_shadows(&g_ov, "www/presets/p.json", 18u));
	CHECK(!hit("www/presets/p.json", NULL));
}

static void test_tombstones(void)
{
	AiusSdOverlayJob j;
	AiusWebCacheHit h;

	/* A settings tombstone: backup DELETE then target DELETE, each with
	 * require_existing false (S2: a pre-4h preset has no backup). */
	fresh(false);
	CHECK(put("www/presets/x.json", 300u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(del("www/presets/x.json", 100u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(!hit("www/presets/x.json", NULL));
	CHECK(aius_sd_overlay_shadows(&g_ov, "www/presets/x.json", 18u));
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_DELETE && !j.require_existing);
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
	CHECK(strcmp(j.target_path, "/aius/.bak-preset-x.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 600u);
	CHECK(!aius_sd_overlay_drained(&g_ov)); /* FR-S8 */
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_TARGET);
	CHECK(strcmp(j.target_path, "/aius/www/presets/x.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 700u);
	CHECK(aius_sd_overlay_drained(&g_ov));
	/* A path the card never held, sync class (N5): one DELETE. */
	CHECK(del("www/presets/legacy", 1000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 1500u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_TARGET);
	CHECK(strcmp(j.target_path, "/aius/www/presets/legacy") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1500u);
	CHECK(aius_sd_overlay_drained(&g_ov));

	/* G1: a backed-off tombstone is retired by a re-PUT; no DELETE runs
	 * after it, and the card ends with the new save. */
	CHECK(put("www/presets/y.json", 300u, 2u, 2000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 2500u, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2500u);
	CHECK(del("www/presets/y.json", 3000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 3500u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_DELETE);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, 3500u);
	CHECK(put("www/presets/y.json", 400u, 3u, 3600u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 4100u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE && j.length == 400u);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 4100u);
	CHECK(!aius_sd_overlay_next_job(&g_ov, 100000u, &j));
	CHECK(hit("www/presets/y.json", &h) && h.length == 400u);

	/* G1, backup DELETE completed first: the safe copy moved from T to
	 * N+2 reads target, so N+2 writes the backup first. */
	CHECK(del("www/presets/y.json", 200000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 200500u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 200500u);
	CHECK(put("www/presets/y.json", 500u, 4u, 200600u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 201100u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE);
	CHECK(j.order == AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 201100u);
	CHECK(aius_sd_overlay_drained(&g_ov));

	/* A re-PUT while T's DELETE runs leaves T pinned; its end retires it. */
	CHECK(del("www/presets/y.json", 300000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 300500u, &j));
	CHECK(put("www/presets/y.json", 600u, 5u, 300600u) == AIUS_SD_OVERLAY_PUT_OK);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, 300700u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 301200u, &j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE && j.length == 600u);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 301200u);
	CHECK(!aius_sd_overlay_next_job(&g_ov, 999999u, &j));
}

static void test_safe_copy_order(void)
{
	AiusSdOverlayJob j;

	/* A restore-created entry flushes target-only; a put before its flush
	 * keeps the safe copy (backup), so that flush writes the target first. */
	fresh(false);
	pattern(g_buf, 400u, 1u);
	CHECK(aius_sd_overlay_put_restore(&g_ov, "dsp.json", 8u, g_buf, 400u,
	                                  0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 500u, &j));
	CHECK(j.skip_backup && j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST);
	CHECK(j.backup_path[0] != '\0');
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, 500u);
	CHECK(put("dsp.json", 400u, 2u, 600u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 2000u, &j));
	CHECK(!j.skip_backup && j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2000u);

	/* E1: N backup first, N+1 put mid backup pass, the backup pass CLOSEs
	 * (bit on N+1), a card-gone fault in the target pass: N retired, N+1
	 * the only DIRTY entry with backup safe, flushed target first. */
	CHECK(put("dsp.json", 12288u, 3u, 3000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 3500u, &j));
	CHECK(j.order == AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST);
	CHECK(put("dsp.json", 12288u, 4u, 3600u) == AIUS_SD_OVERLAY_PUT_OK);
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_CARD_GONE, 3700u);
	CHECK(pending() == 1u);
	aius_sd_overlay_note_mounted(&g_ov, 4000u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 4200u, &j));
	CHECK(j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST);
	CHECK(input_is(12288u, 4u));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 4200u);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

static void test_store_full_shadows_refresh(void)
{
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	unsigned refreshes = 0u;

	fresh(false);
	for (unsigned i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/f%u", i);
		CHECK(put(p, 10u, i, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	}
	CHECK(put("dsp.json", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_BUSY); /* Q4 */
	CHECK(aius_sd_overlay_shadows(&g_ov, "www/f3", 6u));
	CHECK(!aius_sd_overlay_shadows(&g_ov, "www/none", 8u));
	/* One USAGE_REFRESH per burst. */
	while (aius_sd_overlay_next_job(&g_ov, 1000u, &j)) {
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1500u);
		refreshes += aius_sd_overlay_take_usage_refresh(&g_ov);
	}
	CHECK(refreshes == 1u);
	CHECK(!aius_sd_overlay_shadows(&g_ov, "www/f3", 6u)); /* clean */
	aius_sd_overlay_status(&g_ov, 1500u, &s);
	CHECK(s.flushed == 64u && s.drained && s.sync_drain_ms == 1500u);
	/* PUTTING is not shadowed (D2). */
	CHECK(aius_sd_overlay_put_begin(&g_ov, "www/new.js", 10u, 10u, true) ==
	      AIUS_SD_OVERLAY_PUT_BUSY);
	aius_web_cache_invalidate(&g_cache, NULL, 0u);
	CHECK(aius_sd_overlay_put_begin(&g_ov, "www/new.js", 10u, 10u, true) ==
	      AIUS_SD_OVERLAY_PUT_OK);
	CHECK(!aius_sd_overlay_shadows(&g_ov, "www/new.js", 10u));
	CHECK(aius_sd_overlay_put_chunk(&g_ov, (const uint8_t *)"0123456789",
	                                10u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_put_commit(&g_ov, 2000u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_shadows(&g_ov, "www/new.js", 10u));

	/* POST /api/clear resets the two maxima. */
	CHECK(put("dsp.json", 10u, 1u, 3000u) == AIUS_SD_OVERLAY_PUT_OK);
	while (aius_sd_overlay_next_job(&g_ov, 4000u, &j))
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 4000u);
	aius_sd_overlay_status(&g_ov, 4000u, &s);
	CHECK(s.max_settings_latency_ms == 1000u);
	aius_sd_overlay_clear_stats(&g_ov);
	aius_sd_overlay_status(&g_ov, 4000u, &s);
	CHECK(s.max_settings_latency_ms == 0u && s.sync_drain_ms == 0u);
}

/* ---- Phase 4l: at most 16 saved presets ---- */

static void preset_path(char *out, size_t cap, const char *name)
{
	snprintf(out, cap, "www/presets/%s.json", name);
}

/* put_begin on a preset name; an accepted reservation is aborted at once so
 * the probe leaves nothing behind. */
static AiusSdOverlayPutResult probe_preset(const char *name, bool complete)
{
	char p[48];
	AiusSdOverlayPutResult r;

	preset_path(p, sizeof(p), name);
	r = aius_sd_overlay_put_begin(&g_ov, p, strlen(p), 10u, complete);
	if (r == AIUS_SD_OVERLAY_PUT_OK)
		aius_sd_overlay_put_abort(&g_ov);
	return r;
}

static AiusSdOverlayPutResult save_preset(const char *name)
{
	char p[48];

	preset_path(p, sizeof(p), name);
	return put(p, 10u, 3u, 0u);
}

static void save_presets(unsigned first, unsigned count)
{
	for (unsigned i = first; i < first + count; i++) {
		char n[16];

		snprintf(n, sizeof(n), "p%u", i);
		CHECK(save_preset(n) == AIUS_SD_OVERLAY_PUT_OK);
	}
}

static void delete_preset(const char *name)
{
	char p[48];

	preset_path(p, sizeof(p), name);
	CHECK(del(p, 0u) == AIUS_SD_OVERLAY_PUT_OK);
}

static void test_preset_limit_count_rule(void)
{
	fresh(false);
	save_presets(0u, 16u);
	CHECK(probe_preset("new", true) == AIUS_SD_OVERLAY_PUT_LIMIT);
	CHECK(probe_preset("P3", true) == AIUS_SD_OVERLAY_PUT_OK); /* overwrite, folded */

	/* Foo.json and foo.json count once. */
	fresh(false);
	save_presets(0u, 14u);
	CHECK(save_preset("Foo") == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(save_preset("foo") == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("a", true) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(save_preset("a") == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("b", true) == AIUS_SD_OVERLAY_PUT_LIMIT);

	/* A tombstone for a name never on the card frees no slot. */
	fresh(false);
	save_presets(0u, 16u);
	delete_preset("ghost");
	CHECK(probe_preset("new", true) == AIUS_SD_OVERLAY_PUT_LIMIT);

	/* A running (FLUSHING) tombstone under a newer DIRTY entry of one name
	 * counts it once. */
	{
		AiusSdOverlayJob j;

		fresh(false);
		save_presets(0u, 14u);
		CHECK(save_preset("x") == AIUS_SD_OVERLAY_PUT_OK);
		delete_preset("x");
		CHECK(aius_sd_overlay_next_job(&g_ov, 10000u, &j));
		CHECK(save_preset("x") == AIUS_SD_OVERLAY_PUT_OK);
		CHECK(probe_preset("a", true) == AIUS_SD_OVERLAY_PUT_OK);
		CHECK(save_preset("a") == AIUS_SD_OVERLAY_PUT_OK);
		CHECK(probe_preset("b", true) == AIUS_SD_OVERLAY_PUT_LIMIT);
	}

	/* A newest TOMBSTONE over a counted name frees its slot. */
	fresh(false);
	save_presets(0u, 16u);
	delete_preset("p5");
	CHECK(probe_preset("a", true) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(save_preset("a") == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("b", true) == AIUS_SD_OVERLAY_PUT_LIMIT);

	/* Paths outside the settings class are neither counted nor refused. */
	fresh(false);
	save_presets(0u, 15u);
	CHECK(put("www/presets/sub/x.json", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/presets/notes.txt", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_put_begin(&g_ov, "www/presets/sub/x.json", 22u, 10u,
	                                true) == AIUS_SD_OVERLAY_PUT_OK);
	aius_sd_overlay_put_abort(&g_ov);
	CHECK(aius_sd_overlay_put_begin(&g_ov, "www/presets/notes.txt", 21u, 10u,
	                                true) == AIUS_SD_OVERLAY_PUT_OK);
	aius_sd_overlay_put_abort(&g_ov);
	CHECK(probe_preset("a", true) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(save_preset("a") == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("b", true) == AIUS_SD_OVERLAY_PUT_LIMIT);
}

static void test_preset_limit_state(void)
{
	AiusWebCacheHit h;

	/* B1, S1: the walk is incomplete. */
	fresh(false);
	save_presets(0u, 15u);
	CHECK(probe_preset("p2", false) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("new", false) == AIUS_SD_OVERLAY_PUT_BUSY);
	CHECK(probe_preset("new", true) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(save_preset("new") == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("next", true) == AIUS_SD_OVERLAY_PUT_LIMIT);

	/* Overfull: 16 counted mid-walk is LIMIT, not BUSY. */
	fresh(false);
	save_presets(0u, 16u);
	CHECK(probe_preset("new", false) == AIUS_SD_OVERLAY_PUT_LIMIT);
	CHECK(probe_preset("p9", false) == AIUS_SD_OVERLAY_PUT_OK);

	/* A card with 20 presets keeps all 20 served. */
	fresh(false);
	save_presets(0u, 20u);
	for (unsigned i = 0; i < 20u; i++) {
		char p[48];

		snprintf(p, sizeof(p), "www/presets/p%u.json", i);
		CHECK(hit(p, &h));
	}
	CHECK(probe_preset("p19", true) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("new", true) == AIUS_SD_OVERLAY_PUT_LIMIT);
}

static void test_preset_limit_headroom(void)
{
	AiusSdOverlayJob j;

	fresh(false);
	save_presets(0u, 16u);
	CHECK(put("dsp.json", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("config.json", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	for (unsigned i = 0; i < 27u; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/f%u", i);
		CHECK(put(p, 10u, i, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	}
	/* A job in flight, then a full sync replay, an Apply and an overwrite. */
	CHECK(aius_sd_overlay_next_job(&g_ov, 10000u, &j));
	for (unsigned i = 0; i < 27u; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/f%u", i);
		CHECK(put(p, 10u, i + 100u, 1u) != AIUS_SD_OVERLAY_PUT_BUSY);
	}
	CHECK(put("dsp.json", 10u, 2u, 1u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("p4", true) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(save_preset("p4") == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(probe_preset("new", true) == AIUS_SD_OVERLAY_PUT_LIMIT);
	/* S1: one preset put_begin reads each manifest slot at most once. */
	CHECK(aius_sd_overlay_preset_scan_reads > 0u &&
	      aius_sd_overlay_preset_scan_reads <= AIUS_WEB_CACHE_ENTRIES);
}

static uint8_t tombstone_slot(void)
{
	for (uint8_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++)
		if (E(i)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
			return i;
	return 0xFF;
}

/* D-B1: a tombstone whose path takes a bit flip fails its entry CRC; it is
 * dropped (crc_drops, event), never re-committed or re-put, and no DELETE or
 * other job is issued for the original path or the flipped one, before or
 * after its backup DELETE ran. A faulted dirty dsp.json re-puts on the key's
 * own path, not the faulted bytes. */
static void test_b1_faulted_tombstone_path_flip(void)
{
	static const struct {
		const char *path;
		const char *flipped;
		size_t at;
		uint8_t bit;
	} k_cases[] = {
		{"www/presets/a.json", "www/presets/c.json", 12u, 0x02u},
		{"dsp.json", "Dsp.json", 0u, 0x20u}, /* still a settings key */
	};
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	AiusWebCacheHit h;

	for (unsigned c = 0; c < 2u; c++)
		for (unsigned backup_done = 0; backup_done < 2u; backup_done++) {
			const char *path = k_cases[c].path;
			const char *flipped = k_cases[c].flipped;
			uint8_t t;
			unsigned named = 0u;

			fresh(false);
			CHECK(put(path, 300u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
			while (aius_sd_overlay_next_job(&g_ov, 600u, &j))
				aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK,
				                        600u);
			CHECK(del(path, 1000u) == AIUS_SD_OVERLAY_PUT_OK);
			if (backup_done != 0u) {
				CHECK(aius_sd_overlay_next_job(&g_ov, 1500u, &j));
				CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
				aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK,
				                        1500u);
			}
			t = tombstone_slot();
			CHECK(t != 0xFF);
			if (t == 0xFF)
				continue;
			E(t)->path[k_cases[c].at] ^= k_cases[c].bit;
			CHECK(strcmp(E(t)->path, flipped) == 0);
			CHECK(!aius_sd_overlay_job_ready(&g_ov, 99999u));
			CHECK(!aius_sd_overlay_next_job(&g_ov, 99999u, &j));
			CHECK((E(t)->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u);
			aius_sd_overlay_service(&g_ov, 100000u);
			aius_sd_overlay_status(&g_ov, 100000u, &s);
			CHECK(s.crc_drops == 1u && g_events == 1u);
			CHECK(g_reputs == 0u); /* a delete never becomes a write */
			CHECK(s.tombstones == 0u && s.dirty == 0u && s.drained);
			CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_FREE);
			CHECK(!aius_sd_overlay_shadows(&g_ov, path, strlen(path)));
			CHECK(!aius_sd_overlay_shadows(&g_ov, flipped, strlen(flipped)));
			/* Nothing reaches the card for either path, mount or not. */
			aius_sd_overlay_note_card_lost(&g_ov);
			aius_sd_overlay_note_mounted(&g_ov, 200000u);
			for (uint32_t now = 100000u; now <= 400000u; now += 50000u) {
				CHECK(!aius_sd_overlay_job_ready(&g_ov, now));
				while (aius_sd_overlay_next_job(&g_ov, now, &j)) {
					named++;
					aius_sd_overlay_job_end(
						&g_ov, AIUS_SD_OVERLAY_JOB_OK, now);
				}
				aius_sd_overlay_service(&g_ov, now);
			}
			CHECK(named == 0u);
			aius_sd_overlay_status(&g_ov, 400000u, &s);
			CHECK(s.flushed == 1u && s.crc_drops == 1u);
		}

	/* A faulted dirty dsp.json (case bit in its path): the Q2 re-puts land
	 * on "dsp.json" and "config.json" themselves (CR-N1), the dsp.json one
	 * retiring the faulted entry. */
	fresh(false);
	CHECK(put("dsp.json", 300u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(hit("dsp.json", &h));
	E(h.entry)->path[0] ^= 0x20u;
	CHECK(!aius_sd_overlay_next_job(&g_ov, 600u, &j));
	aius_sd_overlay_service(&g_ov, 700u);
	CHECK(g_reputs == 2u);
	/* The faulted entry is gone (its slot may hold a re-put now). */
	CHECK((E(h.entry)->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) == 0u &&
	      strcmp(E(h.entry)->path, "Dsp.json") != 0);
	CHECK(pending() == 2u);
	CHECK(hit("dsp.json", &h) && strcmp(E(h.entry)->path, "dsp.json") == 0);
	CHECK(aius_sd_overlay_next_job(&g_ov, 1200u, &j));
	CHECK(strcmp(j.target_path, "/aius/dsp.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1200u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 1200u, &j));
	CHECK(strcmp(j.target_path, "/aius/config.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1200u);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

/* CR-B1: millis lapses past 2^31 ms and across 2^32. */
static void test_cr_b1_long_lapse(void)
{
	const uint32_t lapse = UINT32_C(0x80000100);
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	uint32_t t;

	/* Q1: accepted with no card, a card inserted 24.8 days later. */
	fresh(true);
	CHECK(put("dsp.json", 100u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	aius_sd_overlay_note_mounted(&g_ov, lapse);
	CHECK(aius_sd_overlay_job_ready(&g_ov, lapse));
	CHECK(aius_sd_overlay_next_job(&g_ov, lapse, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, lapse);
	aius_sd_overlay_status(&g_ov, lapse, &s);
	CHECK(s.max_settings_latency_ms == lapse && s.drained);

	/* Debounce and backoff across 2^32. */
	fresh(false);
	t = UINT32_C(0xFFFFFF00);
	CHECK(put("www/a.js", 100u, 1u, t) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(!aius_sd_overlay_job_ready(&g_ov, t + 499u));
	CHECK(!aius_sd_overlay_next_job(&g_ov, t + 499u, &j));
	t += 500u; /* wraps to 0xF4 */
	CHECK(t == 0xF4u && aius_sd_overlay_next_job(&g_ov, t, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, t);
	CHECK(!aius_sd_overlay_job_ready(&g_ov, t + 999u));
	CHECK(aius_sd_overlay_next_job(&g_ov, t + 1000u, &j));
	t += 1000u;
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, t);
	CHECK(!aius_sd_overlay_job_ready(&g_ov, t + 1999u));
	/* A backoff stamp lapsed past 2^31 ms reads passed, not future. */
	aius_sd_overlay_note_card_lost(&g_ov);
	aius_sd_overlay_note_mounted(&g_ov, t + 2000u + lapse);
	CHECK(aius_sd_overlay_job_ready(&g_ov, t + 2000u + lapse));
	CHECK(aius_sd_overlay_next_job(&g_ov, t + 2000u + lapse, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, t + 2000u + lapse);
	CHECK(aius_sd_overlay_drained(&g_ov));

	/* Oldest first by age, and its age reported, across 2^31. */
	fresh(true);
	CHECK(put("www/presets/a.json", 100u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/presets/b.json", 100u, 2u, UINT32_C(0x90000000)) ==
	      AIUS_SD_OVERLAY_PUT_OK);
	aius_sd_overlay_status(&g_ov, UINT32_C(0x90001000), &s);
	CHECK(s.oldest_dirty_age_ms == UINT32_C(0x90001000));
	aius_sd_overlay_note_mounted(&g_ov, UINT32_C(0x90001000));
	CHECK(aius_sd_overlay_next_job(&g_ov, UINT32_C(0x90001000), &j));
	CHECK(strcmp(j.target_path, "/aius/www/presets/a.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, UINT32_C(0x90001000));

	/* An owed target DELETE after a long hold. */
	fresh(false);
	CHECK(del("www/presets/x.json", 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 500u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 500u);
	aius_sd_overlay_note_card_lost(&g_ov);
	aius_sd_overlay_note_mounted(&g_ov, 500u + lapse);
	CHECK(aius_sd_overlay_next_job(&g_ov, 500u + lapse, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_TARGET);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 500u + lapse);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

/* CR-S1: an entry-field flip (start onto another preset's chunks, whose
 * table CRCs match; or length) on a FLUSHING entry fails the job at its
 * first input read, before any WRITE; the entry takes Q2's path and no job
 * ever names its path again. A DIRTY entry with a flipped field never
 * starts a flush. */
static void test_cr_s1_entry_field_flip(void)
{
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	AiusWebCacheHit h;
	uint8_t got[1024];
	uint32_t n = 99u;

	for (unsigned field = 0; field < 3u; field++) {
		uint8_t a;
		unsigned named = 0u;

		fresh(false);
		CHECK(put("www/presets/a.json", 700u, 1u, 0u) ==
		      AIUS_SD_OVERLAY_PUT_OK);
		CHECK(put("www/presets/b.json", 700u, 2u, 0u) ==
		      AIUS_SD_OVERLAY_PUT_OK);
		CHECK(hit("www/presets/a.json", &h));
		a = h.entry;
		CHECK(hit("www/presets/b.json", &h));
		if (field < 2u) {
			CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
			CHECK(strcmp(j.target_path, "/aius/www/presets/a.json") == 0);
			CHECK(j.slot == a);
		}
		if (field != 1u)
			E(a)->start = E(h.entry)->start; /* b's chunks */
		else
			E(a)->length ^= 1u; /* 701: same chunk count */
		if (field < 2u) {
			n = 99u;
			CHECK(!aius_sd_overlay_job_input(&g_ov, 0u, got, sizeof(got),
			                                 &n));
			CHECK(n == 0u);
			CHECK(!aius_web_cache_copy_chunk(&g_cache, a, 0u, got, &n));
			aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_CHUNK_CRC,
			                        600u);
		}
		CHECK((E(a)->flags & AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED) != 0u ||
		      field == 2u);
		while (aius_sd_overlay_next_job(&g_ov, 700u, &j)) {
			named += strcmp(j.target_path,
			                "/aius/www/presets/a.json") == 0;
			aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 700u);
		}
		CHECK((E(a)->flags & AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED) != 0u);
		aius_sd_overlay_service(&g_ov, 800u);
		aius_sd_overlay_status(&g_ov, 800u, &s);
		CHECK(s.crc_drops == 1u && g_reputs == 2u); /* CR-N1 */
		while (aius_sd_overlay_next_job(&g_ov, 1400u, &j)) {
			named += strcmp(j.target_path,
			                "/aius/www/presets/a.json") == 0;
			aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1400u);
		}
		CHECK(named == 0u && aius_sd_overlay_drained(&g_ov));
	}
}

/* CR-N1: a flip in dirty dsp.json's path ("fsp.json") reads like a www
 * entry; the path is untrusted, so both settings files re-put from the
 * running settings rather than dsp.json being dropped as a preset. */
static void test_cr_n1_untrusted_settings_path(void)
{
	AiusSdOverlayStatus s;
	AiusSdOverlayJob j;
	AiusWebCacheHit h;
	uint8_t d;

	fresh(false);
	CHECK(put("dsp.json", 300u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(hit("dsp.json", &h));
	d = h.entry;
	E(d)->path[0] ^= 0x02u; /* 'd' -> 'f' */
	CHECK(strcmp(E(d)->path, "fsp.json") == 0);
	CHECK(!aius_sd_overlay_next_job(&g_ov, 600u, &j));
	g_reput_busy = true; /* dropped first; the re-puts retried (D1, D6) */
	aius_sd_overlay_service(&g_ov, 700u);
	aius_sd_overlay_status(&g_ov, 700u, &s);
	CHECK(g_reputs == 0u && s.crc_drops == 1u &&
	      E(d)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(!aius_sd_overlay_drained(&g_ov) && !s.drained); /* re-puts owed */
	g_reput_busy = false;
	aius_sd_overlay_service(&g_ov, 800u);
	aius_sd_overlay_status(&g_ov, 800u, &s);
	CHECK(g_reputs == 2u && s.crc_drops == 1u);
	aius_sd_overlay_service(&g_ov, 900u);
	CHECK(g_reputs == 2u); /* landed: no more */
	CHECK(aius_sd_overlay_shadows(&g_ov, "dsp.json", 8u));
	CHECK(aius_sd_overlay_shadows(&g_ov, "config.json", 11u));
	CHECK(!aius_sd_overlay_shadows(&g_ov, "fsp.json", 8u));
	CHECK(hit("dsp.json", &h) && h.length == 600u);
}

/* D-N1: a settings tombstone always DELETEs its backup before its target,
 * even in a slot whose previous tombstone's backup DELETE completed. */
static void fill_sync(unsigned n, uint32_t now)
{
	for (unsigned i = 0; i < n; i++) {
		char p[32];

		snprintf(p, sizeof(p), "www/f%u", i);
		CHECK(put(p, 10u, i, now) == AIUS_SD_OVERLAY_PUT_OK);
	}
}

static void flush_all(uint32_t now)
{
	AiusSdOverlayJob j;

	while (aius_sd_overlay_next_job(&g_ov, now, &j))
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, now);
}

/* CR2-N1 / CR3-B1: an entry-faulted preset in a full manifest is dropped
 * first; the owed re-puts hold drained false until both land, and both land
 * in place over their files' own clean entries even after another put took
 * the freed entry. */
static void test_cr2_n1_full_manifest(void)
{
	AiusWebCacheHit h;
	AiusSdOverlayStatus s;
	unsigned reputs;

	fresh(false);
	CHECK(put("dsp.json", 10u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("config.json", 10u, 2u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	flush_all(600u); /* both clean, cached */
	fill_sync(AIUS_WEB_CACHE_ENTRIES - 3u, 1000u);
	CHECK(put("www/presets/p.json", 10u, 1u, 1000u) ==
	      AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/x.js", 10u, 1u, 1000u) == AIUS_SD_OVERLAY_PUT_BUSY);
	CHECK(hit("www/presets/p.json", &h));
	E(h.entry)->path[12] ^= 0x02u;
	/* The settings pick reads it first and flags it. */
	CHECK(aius_sd_overlay_job_ready(&g_ov, 1500u));
	CHECK((E(h.entry)->flags & AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED) != 0u);
	g_reput_busy = true;
	aius_sd_overlay_service(&g_ov, 1000u);
	aius_sd_overlay_status(&g_ov, 1000u, &s);
	CHECK(s.crc_drops == 1u && g_reputs == 0u);
	CHECK(put("www/late.js", 10u, 1u, 1100u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/x.js", 10u, 1u, 1100u) == AIUS_SD_OVERLAY_PUT_BUSY);
	flush_all(5000u);
	CHECK(!aius_sd_overlay_drained(&g_ov)); /* the re-puts are owed */
	aius_sd_overlay_status(&g_ov, 5000u, &s);
	CHECK(!s.drained);
	g_reput_busy = false;
	aius_sd_overlay_service(&g_ov, 6000u);
	CHECK(g_reputs == 2u);
	CHECK(aius_sd_overlay_shadows(&g_ov, "dsp.json", 8u));
	CHECK(aius_sd_overlay_shadows(&g_ov, "config.json", 11u));
	CHECK(hit("dsp.json", &h) && h.length == 600u);
	CHECK(hit("config.json", &h) && h.length == 600u);
	CHECK(!aius_sd_overlay_drained(&g_ov));
	flush_all(7000u);
	CHECK(aius_sd_overlay_drained(&g_ov));
	reputs = g_reputs;
	aius_sd_overlay_service(&g_ov, 8000u);
	CHECK(g_reputs == reputs);
}

/* CR3-B1, the reviewer's case: a dirty dsp.json takes a chunk fault after
 * its backup pass CLOSEd, its re-put is owed (serialiser busy), another put
 * takes the last free entry: drained stays false, and the re-put lands in
 * place over the faulted entry, keeping its safe copy (NS6). */
static void test_cr3_reput_in_place(void)
{
	AiusSdOverlayJob j;
	AiusWebCacheHit h;
	uint32_t n = 0u;
	uint8_t d;

	fresh(false);
	CHECK(put("dsp.json", 1200u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(hit("dsp.json", &h));
	d = h.entry;
	fill_sync(AIUS_WEB_CACHE_ENTRIES - 2u, 0u); /* one entry left free */
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	CHECK(j.slot == d && j.order == AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST);
	corrupt_chunk("dsp.json");
	CHECK(!aius_sd_overlay_job_input(&g_ov, 0u, g_buf, sizeof(g_buf), &n));
	aius_sd_overlay_note_pass_closed(&g_ov, AIUS_WEB_CACHE_SAFE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_CHUNK_CRC, 600u);
	g_reput_busy = true;
	aius_sd_overlay_service(&g_ov, 700u);
	CHECK(g_reputs == 0u && !aius_sd_overlay_drained(&g_ov));
	CHECK(put("www/late.js", 10u, 1u, 700u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(put("www/x.js", 10u, 1u, 700u) == AIUS_SD_OVERLAY_PUT_BUSY);
	/* Every sync entry flushed: only the faulted dsp.json and the owed
	 * re-put remain, and the gate stays shut. */
	while (aius_sd_overlay_next_job(&g_ov, 2000u, &j))
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2000u);
	CHECK(!aius_sd_overlay_drained(&g_ov));
	g_reput_busy = false;
	aius_sd_overlay_service(&g_ov, 2100u);
	CHECK(g_reputs == 1u);
	CHECK(hit("dsp.json", &h) && h.entry == d && h.length == 600u);
	CHECK((E(d)->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) == 0u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 2600u, &j));
	CHECK(j.slot == d && j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2600u);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

/* CR4-S1: a one-shot put reusing its path's pending entry in place (store
 * full) faults while writing: that pending version is lost. dsp.json /
 * config.json: the re-put is owed, drained false until it lands, and it
 * lands with the lost entry's safe copy. A preset: dropped and counted. */
static void test_cr4_lost_in_place(void)
{
	static const char *const keys[] = {"dsp.json", "config.json"};
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	AiusWebCacheHit h;

	for (unsigned k = 0; k < 2u; k++)
		for (unsigned backup = 0; backup < 2u; backup++) {
			const char *key = keys[k];

			fresh(false);
			CHECK(put(key, 600u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
			if (backup != 0u)
				CHECK(aius_web_cache_set_safe_copy(
					&g_cache, key, strlen(key),
					AIUS_WEB_CACHE_SAFE_BACKUP));
			fill_sync(AIUS_WEB_CACHE_ENTRIES - 1u, 0u);
			g_write_fault = true;
			CHECK(put(key, 600u, 2u, 100u) == AIUS_SD_OVERLAY_PUT_FAULT);
			CHECK(!g_write_fault);
			CHECK(!aius_sd_overlay_shadows(&g_ov, key, strlen(key)));
			CHECK(!aius_sd_overlay_drained(&g_ov)); /* re-put owed */
			flush_all(5000u);
			CHECK(!aius_sd_overlay_drained(&g_ov));
			aius_sd_overlay_service(&g_ov, 5100u);
			CHECK(g_reputs == 1u);
			CHECK(hit(key, &h) && h.length == 600u);
			CHECK(((E(h.entry)->flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) !=
			       0u) == (backup != 0u));
			CHECK(aius_sd_overlay_next_job(&g_ov, 5600u, &j));
			CHECK((j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST) ==
			      (backup != 0u));
			aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 5600u);
			CHECK(aius_sd_overlay_drained(&g_ov));
			aius_sd_overlay_status(&g_ov, 5600u, &s);
			CHECK(s.crc_drops == 0u);
		}

	/* A preset's lost pending version: dropped, counted, logged. */
	fresh(false);
	CHECK(put("www/presets/p.json", 600u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	fill_sync(AIUS_WEB_CACHE_ENTRIES - 1u, 0u);
	g_write_fault = true;
	CHECK(put("www/presets/p.json", 600u, 2u, 100u) ==
	      AIUS_SD_OVERLAY_PUT_FAULT);
	aius_sd_overlay_status(&g_ov, 100u, &s);
	CHECK(s.crc_drops == 1u && g_events == 1u && g_reputs == 0u);
	CHECK(!aius_sd_overlay_shadows(&g_ov, "www/presets/p.json", 18u));
	flush_all(5000u);
	aius_sd_overlay_service(&g_ov, 5100u);
	CHECK(g_reputs == 0u && aius_sd_overlay_drained(&g_ov));

	/* A put that reuses a clean entry and faults loses nothing. */
	fresh(false);
	CHECK(put("dsp.json", 600u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	flush_all(600u);
	fill_sync(AIUS_WEB_CACHE_ENTRIES - 1u, 1000u);
	g_write_fault = true;
	CHECK(put("dsp.json", 600u, 2u, 1100u) == AIUS_SD_OVERLAY_PUT_FAULT);
	flush_all(5000u);
	aius_sd_overlay_service(&g_ov, 5100u);
	CHECK(g_reputs == 0u && aius_sd_overlay_drained(&g_ov));
}

static void test_n1_backup_delete_after_slot_reuse(void)
{
	AiusSdOverlayJob j;
	uint8_t t;
	bool z_backup = false;
	bool z_target = false;
	bool ordered = true;

	fresh(false);
	CHECK(put("www/presets/x.json", 100u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(aius_sd_overlay_next_job(&g_ov, 600u, &j));
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 600u);
	CHECK(del("www/presets/x.json", 1000u) == AIUS_SD_OVERLAY_PUT_OK);
	t = tombstone_slot();
	CHECK(aius_sd_overlay_next_job(&g_ov, 1500u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 1500u);
	/* A re-PUT retires T; z's tombstone lands in T's slot before any
	 * scheduler pass. */
	CHECK(put("www/presets/x.json", 200u, 2u, 1600u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(E(t)->state == AIUS_WEB_CACHE_STATE_FREE);
	CHECK(del("www/presets/z.json", 1600u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(tombstone_slot() == t);
	CHECK(!aius_sd_overlay_job_ready(&g_ov, 2099u)); /* no owed shortcut */
	while (aius_sd_overlay_next_job(&g_ov, 2100u, &j)) {
		if (strcmp(j.target_path, "/aius/.bak-preset-z.json") == 0)
			z_backup = true;
		if (strcmp(j.target_path, "/aius/www/presets/z.json") == 0) {
			ordered = ordered && z_backup;
			z_target = true;
		}
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 2100u);
	}
	CHECK(z_backup && z_target && ordered);
	CHECK(aius_sd_overlay_drained(&g_ov));

	/* The same path re-tombstoned in place after its backup DELETE: a new
	 * commit, so its backup DELETE runs again first. */
	CHECK(del("www/presets/x.json", 3000u) == AIUS_SD_OVERLAY_PUT_OK);
	t = tombstone_slot();
	CHECK(aius_sd_overlay_next_job(&g_ov, 3500u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 3500u);
	CHECK(del("www/presets/X.json", 3600u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(tombstone_slot() == t);
	CHECK(!aius_sd_overlay_next_job(&g_ov, 4099u, &j)); /* debounced again */
	CHECK(aius_sd_overlay_next_job(&g_ov, 4100u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 4100u);
	CHECK(aius_sd_overlay_next_job(&g_ov, 4100u, &j));
	CHECK(j.delete_step == AIUS_SD_OVERLAY_DELETE_TARGET);
	CHECK(strcmp(j.target_path, "/aius/www/presets/X.json") == 0);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, 4100u);
	CHECK(aius_sd_overlay_drained(&g_ov));
}

static uint32_t g_lcg;

static uint32_t rnd(uint32_t n)
{
	g_lcg = g_lcg * 1664525u + 1013904223u;
	return (g_lcg >> 8) % n;
}

/* D-S2: _job_ready is true exactly when _next_job returns a job, through
 * debounce, S5 backoff, held, a running job, CRC faults and owed target
 * DELETEs; status-derived "due" (every tombstone, any dirty entry older than
 * the debounce) is not. */
static void test_s2_job_ready(void)
{
	static const char *const paths[] = {
		"dsp.json", "config.json", "www/presets/a.json",
		"www/presets/b.json", "www/app.js", "www/presets/n.txt"};
	AiusSdOverlayJob j;
	AiusSdOverlayStatus s;
	uint32_t now = 0u;
	bool running = false;
	unsigned mismatches = 0u;
	unsigned ready = 0u;
	unsigned idle = 0u;
	unsigned old_due_not_ready = 0u;

	/* One backed-off entry: the status says due, no job is ready. */
	fresh(false);
	CHECK(put("config.json", 100u, 1u, 0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(!aius_sd_overlay_job_ready(&g_ov, 499u));
	CHECK(aius_sd_overlay_job_ready(&g_ov, 500u));
	CHECK(aius_sd_overlay_next_job(&g_ov, 500u, &j));
	CHECK(!aius_sd_overlay_job_ready(&g_ov, 500u)); /* a job runs */
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_FAILED, 500u);
	aius_sd_overlay_status(&g_ov, 1499u, &s);
	CHECK(s.dirty == 1u && s.oldest_dirty_age_ms >= 500u);
	CHECK(!aius_sd_overlay_job_ready(&g_ov, 1499u));
	CHECK(aius_sd_overlay_job_ready(&g_ov, 1500u));
	aius_sd_overlay_note_card_lost(&g_ov);
	CHECK(!aius_sd_overlay_job_ready(&g_ov, 1500u)); /* held */
	aius_sd_overlay_note_mounted(&g_ov, 1500u);
	CHECK(aius_sd_overlay_job_ready(&g_ov, 1500u));
	CHECK(!aius_sd_overlay_job_ready(NULL, 1500u));

	/* Random walk: the predicate and the scheduler agree at every step. */
	fresh(false);
	g_lcg = 12345u;
	for (unsigned step = 0; step < 40000u; step++) {
		const char *p = paths[rnd(6u)];
		const uint32_t op = rnd(24u);

		now += rnd(400u);
		if (op < 6u) {
			(void)put(p, 1u + rnd(1500u), step, now);
		} else if (op < 8u) {
			(void)del(p, now);
		} else if (op < 13u && running) {
			const uint32_t o = rnd(10u);

			aius_sd_overlay_job_end(
				&g_ov,
				o < 6u ? AIUS_SD_OVERLAY_JOB_OK
				: o < 9u ? AIUS_SD_OVERLAY_JOB_FAILED
				         : AIUS_SD_OVERLAY_JOB_CARD_GONE,
				now);
			running = false;
		} else if (op == 13u) {
			aius_sd_overlay_note_card_lost(&g_ov);
		} else if (op < 16u) {
			aius_sd_overlay_note_mounted(&g_ov, now);
		} else if (op == 16u) {
			const uint8_t i = (uint8_t)rnd(AIUS_WEB_CACHE_ENTRIES);

			if (i != g_ov.flushing_slot &&
			    (E(i)->state == AIUS_WEB_CACHE_STATE_DIRTY ||
			     E(i)->state == AIUS_WEB_CACHE_STATE_TOMBSTONE))
				E(i)->path[0] ^= 0x02u;
		} else if (op == 17u) {
			aius_sd_overlay_service(&g_ov, now);
		}
		if (running) {
			mismatches += aius_sd_overlay_job_ready(&g_ov, now);
			continue;
		}
		{
			const bool r = aius_sd_overlay_job_ready(&g_ov, now);
			bool old_due;

			aius_sd_overlay_status(&g_ov, now, &s);
			old_due = s.tombstones != 0u ||
			          (s.dirty != 0u &&
			           s.oldest_dirty_age_ms >=
			                   AIUS_SD_OVERLAY_DEBOUNCE_MS);
			running = aius_sd_overlay_next_job(&g_ov, now, &j);
			mismatches += r != running;
			ready += running;
			idle += !running;
			old_due_not_ready += old_due && !r && !s.held;
		}
	}
	CHECK(mismatches == 0u);
	CHECK(ready > 1000u && idle > 1000u);
	CHECK(old_due_not_ready > 100u);
}

static void test_restore_decision(void)
{
	CHECK(aius_sd_overlay_restore_decision(true, AIUS_SD_OVERLAY_BACKUP_NOT_READ) ==
	      AIUS_SD_OVERLAY_RESTORE_KEEP);
	CHECK(aius_sd_overlay_restore_decision(true, AIUS_SD_OVERLAY_BACKUP_REFUSED) ==
	      AIUS_SD_OVERLAY_RESTORE_KEEP);
	CHECK(aius_sd_overlay_restore_decision(false, AIUS_SD_OVERLAY_BACKUP_PARSED) ==
	      AIUS_SD_OVERLAY_RESTORE_RESTORE);
	CHECK(aius_sd_overlay_restore_decision(false, AIUS_SD_OVERLAY_BACKUP_REFUSED) ==
	      AIUS_SD_OVERLAY_RESTORE_FAULT);
	CHECK(aius_sd_overlay_restore_decision(
		      false, AIUS_SD_OVERLAY_BACKUP_LOAD_FAILED) ==
	      AIUS_SD_OVERLAY_RESTORE_NOTHING);
}

/* ------------------------------------------- FR-S6 / FR-S7 card model -- */

#define VERSIONS 3

typedef struct {
	uint8_t bytes[2048];
	uint32_t length;
} Version;

/* One settings file: its key, its pair of card paths, three complete saves
 * (0 the card's old one, 1 the new save, 2 E1's N+1) and the parser. */
typedef struct {
	const char *name;
	const char *key;
	const char *target;
	const char *backup;
	uint32_t max_bytes;
	Version v[VERSIONS];
	bool multi_chunk;
} SettingsFile;

static SettingsFile g_dsp_file = {"dsp", "dsp.json", "/aius/dsp.json",
                                  AIUS_SD_OVERLAY_BACKUP_DSP,
                                  AIUS_DSP_MODEL_MAX_JSON_BYTES, {{{0}, 0}}, true};
static SettingsFile g_cfg_file = {"config", "config.json", "/aius/config.json",
                                  AIUS_SD_OVERLAY_BACKUP_CONFIG,
                                  AIUS_CONFIG_MAX_JSON_BYTES, {{{0}, 0}}, false};

static FakeCard g_card;
static AiusSdJob g_job;
static uint8_t g_job_buf[512];
static uint8_t g_input[512];
static uint32_t g_now;
static unsigned g_cuts;
static unsigned g_torn_targets; /* cuts leaving the target failing */
static unsigned g_restores;     /* mounts that restored from the backup */

static uint32_t zlib_crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0u; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0u; bit < 8u; bit++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
	}
	return ~crc;
}

/* Pads a JSON object to `length` with spaces before its closing brace. */
static void pad_version(Version *v, const char *head, uint32_t length)
{
	size_t n = strlen(head);

	CHECK(n + 1u <= length && length <= sizeof(v->bytes));
	memcpy(v->bytes, head, n);
	memset(v->bytes + n, ' ', length - n - 1u);
	v->bytes[length - 1u] = '}';
	v->length = length;
}

static void make_versions(void)
{
	static const uint32_t dsp_lengths[VERSIONS] = {1100u, 1700u, 1300u};
	static const uint32_t cfg_lengths[VERSIONS] = {150u, 230u, 100u};
	static const char *const cfg_flags[VERSIONS] = {
		"true,\"remote_control\":false,\"telemetry\":false",
		"false,\"remote_control\":true,\"telemetry\":false",
		"true,\"remote_control\":true,\"telemetry\":true"};
	char head[1200];

	for (unsigned i = 0u; i < VERSIONS; i++) {
		snprintf(head, sizeof(head),
		         "{\"source\":\"sd\",\"config_generation\":%u,"
		         "\"sample_rate_hz\":48000,"
		         "\"features\":{\"input_eq\":true,\"matrix\":true,"
		         "\"output_eq\":false,\"limiter\":true,\"meters\":false},"
		         "\"input_channels\":{"
		         "\"fl\":{\"gain_db\":-3.5,\"eq\":[{\"type\":\"peaking\","
		         "\"frequency_hz\":1000,\"q\":1.5,\"gain_db\":4}]},"
		         "\"fr\":{\"gain_db\":0,\"eq\":[]},"
		         "\"fc\":{\"gain_db\":12,\"eq\":[]},"
		         "\"lfe\":{\"gain_db\":-60,\"eq\":[]},"
		         "\"sl\":{\"gain_db\":0,\"eq\":[]},"
		         "\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		         "\"output_eq\":[],"
		         "\"matrix\":{\"center_gain_linear\":0.7,"
		         "\"surround_gain_linear\":0.5,\"lfe_gain_linear\":2,"
		         "\"lfe_crossover_hz\":80,\"headroom_linear\":0.9,"
		         "\"clipfree\":true},"
		         "\"limiter\":{\"enabled\":true,\"threshold_dbfs\":-3,"
		         "\"attack_ms\":5,\"release_ms\":200,\"lookahead_ms\":2},"
		         "\"base_generation\":%u",
		         i + 1u, i + 1u);
		pad_version(&g_dsp_file.v[i], head, dsp_lengths[i]);
		snprintf(head, sizeof(head), "{\"features\":{\"sd_web\":%s}",
		         cfg_flags[i]);
		pad_version(&g_cfg_file.v[i], head, cfg_lengths[i]);
	}
}

/* The file's validator: the version index it holds when its parser accepts
 * it, else -1. A file the parser accepts must equal one complete saved
 * version byte for byte (FR-S1: never a mix of two). */
static int validate(const SettingsFile *f, const uint8_t *bytes,
                    uint32_t length)
{
	bool ok;

	if (bytes == NULL || length == 0u || length > f->max_bytes)
		return -1;
	if (f->multi_chunk) {
		AiusDspModel model;

		ok = aius_dsp_config_parse_put(bytes, length, &model) ==
		     AIUS_DSP_CONFIG_OK;
	} else {
		AiusConfig config;

		ok = aius_config_parse(bytes, length, &config) == AIUS_CONFIG_OK;
	}
	if (!ok)
		return -1;
	for (int i = 0; i < VERSIONS; i++)
		if (f->v[i].length == length &&
		    memcmp(f->v[i].bytes, bytes, length) == 0)
			return i;
	CHECK(!"a file the parser accepts holds a mix of saves");
	return -1;
}

static int validate_card(const SettingsFile *f, const char *path)
{
	uint32_t length = 0u;
	const uint8_t *bytes = fake_card_file(&g_card, path, &length);

	return validate(f, bytes, length);
}

typedef enum { RUN_DONE, RUN_FAILED, RUN_CUT } RunEnd;

/* A put of `put` lands before the `put_occurrence`th primitive of
 * `put_kind` (E1's put mid-pass). */
/* A PSRAM fault the hook lands once the target pass has opened (CR2-S1). */
typedef enum {
	INPUT_FAULT_NONE = 0,
	INPUT_FAULT_CHUNK,       /* a data chunk: path still trusted */
	INPUT_FAULT_ENTRY_START, /* the entry's start: entry CRC fails */
	INPUT_FAULT_ENTRY_PATH   /* the entry's path: entry CRC fails */
} InputFault;

typedef struct {
	AiusSdActionKind put_kind;
	uint32_t put_occurrence;
	const SettingsFile *file;
	const Version *put;
	InputFault input_fault;
} Hook;

static void land_input_fault(InputFault fault, uint8_t slot)
{
	AiusWebCacheEntry *e = E(slot);

	if (fault == INPUT_FAULT_CHUNK)
		g_carve[g_cache.data_offset + (size_t)e->start * 512u + 3u] ^= 0x10u;
	else if (fault == INPUT_FAULT_ENTRY_START)
		e->start ^= 1u;
	else if (fault == INPUT_FAULT_ENTRY_PATH)
		e->path[0] ^= 0x02u;
}

/* Runs one overlay job on the card as the platform would: the request
 * through aius_sd_overlay_job_request, the input through _job_input, then,
 * as aius_optical_sd_flush_settle does once the card job has ended, each
 * pass CLOSE reported with _note_pass_closed, the read-back with
 * _note_readback, then _job_end. A failed input read (a PSRAM fault) closes
 * the open file and fails the job CHUNK_CRC. A cut abandons the job (PSRAM
 * is gone with the power). */
static RunEnd run_job(const AiusSdOverlayJob *oj, const Hook *hook)
{
	AiusSdJobRequest request;
	AiusSdEvent event;
	uint32_t counts[FAKE_CARD_KINDS] = {0};
	bool write_open = false;
	bool open_target = false;
	bool input_faulted = false;
	AiusWebCacheSafeCopy notes[2];
	unsigned noted = 0u;
	RunEnd end = RUN_FAILED;

	CHECK(aius_sd_overlay_job_request(oj, NULL, NULL, &request));
	aius_sd_job_init(&g_job, g_job_buf, sizeof(g_job_buf));
	aius_sd_job_start(&g_job, &request, 0u, 0u);
	memset(&event, 0, sizeof(event));
	for (unsigned guard = 0u; guard < 4096u; guard++) {
		AiusSdAction action;
		uint32_t n = 0u;

		memset(&action, 0, sizeof(action));
		aius_sd_job_step(&g_job, 0u, event, &action);
		memset(&event, 0, sizeof(event));
		if (action.kind == AIUS_SD_ACTION_NONE) {
			CHECK(!"job stalled");
			break;
		}
		if (action.kind == AIUS_SD_ACTION_NEED_INPUT) {
			CHECK(action.length <= sizeof(g_input));
			if (!aius_sd_overlay_job_input(&g_ov, (uint32_t)action.offset,
			                               g_input, (uint32_t)action.length,
			                               &n)) {
				AiusSdAction close;
				AiusSdEvent closed;

				CHECK(hook != NULL &&
				      hook->input_fault != INPUT_FAULT_NONE);
				input_faulted = true;
				memset(&close, 0, sizeof(close));
				memset(&closed, 0, sizeof(closed));
				close.kind = AIUS_SD_ACTION_CLOSE;
				if (write_open &&
				    !fake_card_execute(&g_card, &close, &closed))
					return RUN_CUT;
				break;
			}
			CHECK(n == action.length);
			CHECK(aius_sd_job_supply(&g_job, g_input, n));
			continue;
		}
		if (action.kind == AIUS_SD_ACTION_COMPLETE) {
			end = RUN_DONE;
			break;
		}
		if (action.kind == AIUS_SD_ACTION_FAIL)
			break;
		CHECK(action.kind != AIUS_SD_ACTION_RENAME &&
		      action.kind != AIUS_SD_ACTION_REMOVE);
		counts[action.kind]++;
		if (hook != NULL && hook->put != NULL &&
		    action.kind == hook->put_kind &&
		    counts[action.kind] == hook->put_occurrence)
			CHECK(aius_sd_overlay_put(&g_ov, hook->file->key,
			                          strlen(hook->file->key),
			                          hook->put->bytes, hook->put->length,
			                          g_now) == AIUS_SD_OVERLAY_PUT_OK);
		bool pass_close = action.kind == AIUS_SD_ACTION_CLOSE &&
		                  g_job.phase == AIUS_SD_PHASE_CBW_CLOSE &&
		                  write_open;
		if (!fake_card_execute(&g_card, &action, &event))
			return RUN_CUT;
		if (action.kind == AIUS_SD_ACTION_OPEN_WRITE_BEGIN &&
		    event.kind == AIUS_SD_EVENT_OK) {
			write_open = true;
			open_target = strcmp(action.path, oj->target_path) == 0;
			if (open_target && hook != NULL)
				land_input_fault(hook->input_fault, oj->slot);
		}
		if (action.kind == AIUS_SD_ACTION_CLOSE) {
			if (pass_close && event.kind == AIUS_SD_EVENT_OK &&
			    noted < 2u)
				notes[noted++] = open_target
					? AIUS_WEB_CACHE_SAFE_TARGET
					: AIUS_WEB_CACHE_SAFE_BACKUP;
			write_open = false;
		}
	}
	const AiusSdResult *result = aius_sd_job_result(&g_job);

	if (!input_faulted)
		CHECK(result->passes_closed == noted);
	for (unsigned k = 0u; k < noted; k++)
		aius_sd_overlay_note_pass_closed(&g_ov, notes[k]);
	if (input_faulted) {
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_CHUNK_CRC, g_now);
	} else if (end == RUN_DONE) {
		aius_sd_overlay_note_readback(&g_ov, true);
		aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, g_now);
	} else {
		if (result->readback_mismatch)
			aius_sd_overlay_note_readback(&g_ov, false);
		aius_sd_overlay_job_end(&g_ov,
		                        g_card.gone ? AIUS_SD_OVERLAY_JOB_CARD_GONE
		                                    : AIUS_SD_OVERLAY_JOB_FAILED,
		                        g_now);
	}
	return end;
}

static void tick(void)
{
	g_now += 2u * AIUS_SD_OVERLAY_BACKOFF_MAX_MS;
}

static bool next_job(AiusSdOverlayJob *j)
{
	tick();
	return aius_sd_overlay_next_job(&g_ov, g_now, j);
}

/* The mount load and restore (H10, FR-S5) after the power returns: PSRAM
 * empty, the target judged by its parser, the backup read only when the
 * target fails, a RESTORE put restore-created and flushed target-only to
 * completion. Returns the version the target holds afterwards, or -1. */
static int mount_restore(const SettingsFile *f,
                         AiusSdOverlayRestoreDecision *decision)
{
	AiusSdOverlayBackupResult backup = AIUS_SD_OVERLAY_BACKUP_NOT_READ;
	int target = validate_card(f, f->target);
	int restored = -1;

	if (target < 0) {
		uint32_t length = 0u;
		const uint8_t *bytes = fake_card_file(&g_card, f->backup, &length);

		if (bytes == NULL || length == 0u || length > f->max_bytes)
			backup = AIUS_SD_OVERLAY_BACKUP_LOAD_FAILED;
		else if ((restored = validate(f, bytes, length)) >= 0)
			backup = AIUS_SD_OVERLAY_BACKUP_PARSED;
		else
			backup = AIUS_SD_OVERLAY_BACKUP_REFUSED;
	}
	*decision = aius_sd_overlay_restore_decision(target >= 0, backup);
	if (*decision == AIUS_SD_OVERLAY_RESTORE_KEEP)
		return target;
	if (*decision != AIUS_SD_OVERLAY_RESTORE_RESTORE)
		return -1;

	AiusSdOverlayJob j;
	const Version *v = &f->v[restored];

	CHECK(aius_sd_overlay_put_restore(&g_ov, f->key, strlen(f->key),
	                                  v->bytes, v->length,
	                                  g_now) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(next_job(&j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE && j.skip_backup);
	CHECK(run_job(&j, NULL) == RUN_DONE);
	CHECK(aius_sd_overlay_drained(&g_ov));
	return validate_card(f, f->target);
}

typedef enum { ST_VALID, ST_MISSING, ST_TORN } StartState;
typedef enum {
	SC_SAVE,         /* a save of v1 */
	SC_RESTORE,      /* the restore's target-only flush (from torn) */
	SC_RETRY_FIRST,  /* v1's job fails in its first pass; the retry */
	SC_RETRY_SECOND, /* v1's job fails in its second pass; the retry */
	SC_E1,           /* E1: v1 backup first, v2 put mid backup pass, card
	                  * gone in the target pass; v2's flush */
	SC_NS6_CHUNK,    /* NS6/CR2-S1: v1's backup pass CLOSEs, a PSRAM fault
	                  * fails its target pass after the zeroed sector 0; */
	SC_NS6_START,    /* the Q2 re-put (running settings, v1) flushes: a  */
	SC_NS6_PATH      /* chunk, entry start or entry path fault */
} Scenario;

static const char *const k_state_names[] = {"valid", "missing", "torn"};
static const char *const k_scenario_names[] = {
	"save", "restore", "retry-1", "retry-2", "e1",
	"ns6-chunk", "ns6-start", "ns6-path"};

static void start_card(const SettingsFile *f, StartState st)
{
	fake_card_init(&g_card);
	if (st == ST_MISSING)
		return;
	fake_card_store(&g_card, f->backup, f->v[0].bytes, f->v[0].length);
	if (st == ST_VALID) {
		fake_card_store(&g_card, f->target, f->v[0].bytes, f->v[0].length);
	} else {
		/* An interrupted save of v2: sector 0 zeroed (FR-S1). */
		fake_card_store(&g_card, f->target, f->v[2].bytes, f->v[2].length);
		fake_card_zero_sector(&g_card, f->target, 0u);
	}
}

static void fail_next(AiusSdActionKind kind, uint32_t occurrence, bool gone)
{
	g_card.fault_kind = kind;
	g_card.fault_occurrence = g_card.kind_counts[kind] + occurrence;
	g_card.fault_card_gone = gone;
}

static void report(const SettingsFile *f, StartState st, Scenario sc,
                   uint32_t cut, bool mid, const char *what)
{
	fprintf(stderr, "  power-cut %s from %s, %s, cut after %u%s: %s\n",
	        f->name, k_state_names[st], k_scenario_names[sc], cut,
	        mid ? " (inside its sync)" : "", what);
}

/* One case: the start state, the scenario's setup, then its last job cut
 * after `cut` primitives (inside the next one's sync with `mid`), then the
 * checks and the mount restore. Returns true when the cut fired before the
 * job ended. */
static bool power_cut_case(const SettingsFile *f, StartState st, Scenario sc,
                           uint32_t cut, bool mid)
{
	AiusSdOverlayJob j;
	AiusSdOverlayRestoreDecision decision;
	bool expect_target_first = false;
	bool expect_skip = false;
	int failures = g_failures;
	int newest = 1;

	g_now = 0u;
	start_card(f, st);
	fresh(false);
	/* The mount before the save: a torn target restores from the backup. */
	if (st == ST_TORN) {
		AiusSdOverlayBackupResult backup = AIUS_SD_OVERLAY_BACKUP_PARSED;

		CHECK(validate_card(f, f->target) < 0 &&
		      validate_card(f, f->backup) == 0);
		CHECK(aius_sd_overlay_restore_decision(false, backup) ==
		      AIUS_SD_OVERLAY_RESTORE_RESTORE);
		CHECK(aius_sd_overlay_put_restore(&g_ov, f->key, strlen(f->key),
		                                  f->v[0].bytes, f->v[0].length,
		                                  g_now) == AIUS_SD_OVERLAY_PUT_OK);
		expect_target_first = true;
		expect_skip = sc == SC_RESTORE;
	}
	if (sc == SC_RESTORE)
		newest = 0;
	else
		CHECK(aius_sd_overlay_put(&g_ov, f->key, strlen(f->key),
		                          f->v[1].bytes, f->v[1].length,
		                          g_now) == AIUS_SD_OVERLAY_PUT_OK);

	if (sc == SC_RETRY_FIRST || sc == SC_RETRY_SECOND) {
		CHECK(next_job(&j));
		CHECK((j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST) ==
		      expect_target_first);
		fail_next(AIUS_SD_ACTION_TRUNCATE, sc == SC_RETRY_FIRST ? 1u : 2u,
		          false);
		CHECK(run_job(&j, NULL) == RUN_FAILED);
		g_card.fault_occurrence = 0u;
		/* FR-B1: a failure after the first pass's CLOSE flips the order. */
		if (sc == SC_RETRY_SECOND)
			expect_target_first = !expect_target_first;
	} else if (sc == SC_E1) {
		Hook hook = {AIUS_SD_ACTION_WRITE, 2u, f, &f->v[2],
		             INPUT_FAULT_NONE};
		uint32_t chunks = (f->v[1].length + 511u) / 512u;

		CHECK(next_job(&j));
		CHECK(j.order == AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST);
		/* Each pass WRITEs a zero chunk 0, chunks 1 to N - 1 and the real
		 * chunk 0; the card goes after the target's zero chunk 0. */
		fail_next(AIUS_SD_ACTION_WRITE, chunks + 1u + 2u, true);
		CHECK(run_job(&j, &hook) == RUN_FAILED);
		g_card.fault_occurrence = 0u;
		CHECK(aius_sd_overlay_held(&g_ov));
		CHECK(validate_card(f, f->target) < 0 &&
		      validate_card(f, f->backup) == 1);
		fake_card_return(&g_card);
		aius_sd_overlay_note_mounted(&g_ov, g_now);
		expect_target_first = true;
		newest = 2;
	} else if (sc >= SC_NS6_CHUNK) {
		const AiusSdOverlaySettingsFile file =
			f == &g_dsp_file ? AIUS_SD_OVERLAY_FILE_DSP
			                 : AIUS_SD_OVERLAY_FILE_CONFIG;
		Hook hook;

		memset(&hook, 0, sizeof(hook));
		hook.input_fault = sc == SC_NS6_CHUNK   ? INPUT_FAULT_CHUNK
		                   : sc == SC_NS6_START ? INPUT_FAULT_ENTRY_START
		                                        : INPUT_FAULT_ENTRY_PATH;
		g_reput_src[file] = f->v[1].bytes; /* the running settings */
		g_reput_src_length[file] = f->v[1].length;
		CHECK(next_job(&j));
		CHECK(j.order == AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST);
		CHECK(run_job(&j, &hook) == RUN_FAILED);
		/* The backup holds v1 and is the safe copy; a multi-chunk
		 * target is torn (its zero chunk 0 landed before the faulted
		 * read), a one-chunk one reads its input first and keeps v0. */
		CHECK(validate_card(f, f->backup) == 1);
		CHECK(validate_card(f, f->target) == (f->multi_chunk ? -1 : 0) ||
		      (f->multi_chunk && validate_card(f, f->target) < 0));
		aius_sd_overlay_service(&g_ov, g_now);
		CHECK(g_reputs == (sc == SC_NS6_CHUNK ? 1u : 2u));
		/* An entry fault also re-puts the other file; dsp.json's
		 * (committed first) flushes ahead of config.json's. */
		if (sc != SC_NS6_CHUNK && f != &g_dsp_file) {
			CHECK(next_job(&j) &&
			      strcmp(j.target_path, g_dsp_file.target) == 0);
			CHECK(run_job(&j, NULL) == RUN_DONE);
		}
		expect_target_first = true;
	}

	CHECK(next_job(&j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE);
	CHECK((j.order == AIUS_SD_OVERLAY_ORDER_TARGET_FIRST) ==
	      expect_target_first);
	CHECK(j.skip_backup == expect_skip);
	CHECK(j.length == f->v[newest].length &&
	      j.file_crc == zlib_crc32(f->v[newest].bytes, f->v[newest].length));
	g_card.cut_after = g_card.primitives + cut;
	g_card.cut_mid_sync = mid;
	RunEnd end = run_job(&j, NULL);
	bool fired = end == RUN_CUT;

	if (!fired) {
		CHECK(end == RUN_DONE);
		if (sc != SC_NS6_CHUNK && sc >= SC_NS6_START && f == &g_dsp_file) {
			AiusSdOverlayJob other;

			g_card.cut_after = FAKE_CARD_NEVER;
			CHECK(next_job(&other) &&
			      strcmp(other.target_path, g_cfg_file.target) == 0);
			CHECK(run_job(&other, NULL) == RUN_DONE);
		}
		CHECK(aius_sd_overlay_drained(&g_ov));
		CHECK(validate_card(f, f->target) == newest);
		if (!expect_skip)
			CHECK(validate_card(f, f->backup) == newest);
	}
	fake_card_power_cut(&g_card);
	fake_card_return(&g_card);
	g_cuts++;

	/* After every cut: the target or the backup passes its validator (from
	 * a missing pair, nothing need pass yet), and validate() has checked
	 * every passing file holds one complete save. */
	int target = validate_card(f, f->target);
	int backup = validate_card(f, f->backup);

	if (st != ST_MISSING)
		CHECK(target >= 0 || backup >= 0);
	else
		CHECK(target < 0 || target == 1);

	/* The mount load and restore. */
	fresh(false);
	int after = mount_restore(f, &decision);

	if (target < 0 && st != ST_MISSING)
		g_torn_targets++;
	if (decision == AIUS_SD_OVERLAY_RESTORE_RESTORE) {
		g_restores++;
		/* The newest complete save on the card: the backup's, the target
		 * having failed. */
		CHECK(after == backup && backup >= 0);
		CHECK(validate_card(f, f->backup) == backup);
	} else if (decision == AIUS_SD_OVERLAY_RESTORE_KEEP) {
		CHECK(after == target && target >= 0);
	} else {
		CHECK(st == ST_MISSING && target < 0 && backup < 0);
	}
	CHECK(g_card.renames == 0u && g_card.removes == 0u);
	if (g_failures != failures)
		report(f, st, sc, cut, mid, "failed");
	return fired;
}

static AiusSdEvent card_do(AiusSdActionKind kind, const char *path,
                           uint8_t *buffer, size_t length, uint64_t offset)
{
	AiusSdAction a;
	AiusSdEvent e;

	memset(&a, 0, sizeof(a));
	memset(&e, 0, sizeof(e));
	a.kind = kind;
	a.action_id = 7u;
	a.path = path;
	a.buffer = buffer;
	a.length = length;
	a.offset = offset;
	if (!fake_card_execute(&g_card, &a, &e))
		e.kind = AIUS_SD_EVENT_NONE;
	return e;
}

static uint32_t card_size(const char *path, const uint8_t **bytes)
{
	uint32_t length = 0u;

	*bytes = fake_card_file(&g_card, path, &length);
	return length;
}

/* The fake card's own landing rules, so the sweep's verdicts are not
 * vacuous. */
static void test_fake_card_rules(void)
{
	static uint8_t old[1100];
	static uint8_t sector[512];
	const uint8_t *got;

	memset(old, 'o', sizeof(old));
	memset(sector, 'n', sizeof(sector));

	/* A full sector inside the landed allocation lands at once; a partial
	 * one does not. */
	fake_card_init(&g_card);
	fake_card_store(&g_card, "/aius/f", old, sizeof(old));
	CHECK(card_do(AIUS_SD_ACTION_OPEN_WRITE_BEGIN, "/aius/f", NULL, 0u, 0u)
	              .kind == AIUS_SD_EVENT_OK);
	CHECK(card_do(AIUS_SD_ACTION_WRITE, NULL, sector, 512u, 512u).bytes ==
	      512u);
	CHECK(card_do(AIUS_SD_ACTION_WRITE, NULL, sector, 10u, 0u).bytes == 10u);
	fake_card_power_cut(&g_card);
	CHECK(card_size("/aius/f", &got) == 1100u && got[0] == 'o' &&
	      got[512] == 'n' && got[1023] == 'n' && got[1024] == 'o');
	fake_card_return(&g_card);

	/* A new file, even its full sectors, is gone without a sync. */
	CHECK(card_do(AIUS_SD_ACTION_OPEN_WRITE_BEGIN, "/aius/g", NULL, 0u, 0u)
	              .kind == AIUS_SD_EVENT_OK);
	CHECK(card_do(AIUS_SD_ACTION_WRITE, NULL, sector, 512u, 0u).bytes == 512u);
	CHECK(card_do(AIUS_SD_ACTION_EXISTS, "/aius/g", NULL, 0u, 0u).value);
	fake_card_power_cut(&g_card);
	CHECK(fake_card_file(&g_card, "/aius/g", &(uint32_t){0}) == NULL);
	fake_card_return(&g_card);

	/* TRUNCATE syncs: the partial sector and the size land. */
	CHECK(card_do(AIUS_SD_ACTION_OPEN_WRITE_BEGIN, "/aius/f", NULL, 0u, 0u)
	              .kind == AIUS_SD_EVENT_OK);
	CHECK(card_do(AIUS_SD_ACTION_WRITE, NULL, sector, 100u, 512u).bytes ==
	      100u);
	CHECK(card_do(AIUS_SD_ACTION_TRUNCATE, NULL, NULL, 0u, 612u).kind ==
	      AIUS_SD_EVENT_OK);
	fake_card_power_cut(&g_card);
	CHECK(card_size("/aius/f", &got) == 612u && got[611] == 'n');
	fake_card_return(&g_card);

	/* A cut inside a sync: the data has landed, the directory entry not. */
	fake_card_store(&g_card, "/aius/f", old, sizeof(old));
	CHECK(card_do(AIUS_SD_ACTION_OPEN_WRITE_BEGIN, "/aius/f", NULL, 0u, 0u)
	              .kind == AIUS_SD_EVENT_OK);
	CHECK(card_do(AIUS_SD_ACTION_WRITE, NULL, sector, 100u, 0u).bytes == 100u);
	g_card.cut_after = g_card.primitives;
	g_card.cut_mid_sync = true;
	CHECK(card_do(AIUS_SD_ACTION_TRUNCATE, NULL, NULL, 0u, 100u).kind ==
	      AIUS_SD_EVENT_NONE);
	CHECK(card_size("/aius/f", &got) == 1100u && got[99] == 'n' &&
	      got[100] == 'o');
	fake_card_return(&g_card);

	/* A card-gone fault loses the cache and faults what follows. */
	fake_card_store(&g_card, "/aius/f", old, sizeof(old));
	fail_next(AIUS_SD_ACTION_TRUNCATE, 1u, true);
	CHECK(card_do(AIUS_SD_ACTION_OPEN_WRITE_BEGIN, "/aius/f", NULL, 0u, 0u)
	              .kind == AIUS_SD_EVENT_OK);
	CHECK(card_do(AIUS_SD_ACTION_WRITE, NULL, sector, 100u, 0u).bytes == 100u);
	CHECK(card_do(AIUS_SD_ACTION_TRUNCATE, NULL, NULL, 0u, 100u).kind ==
	      AIUS_SD_EVENT_FAULT);
	CHECK(card_do(AIUS_SD_ACTION_CLOSE, NULL, NULL, 0u, 0u).kind ==
	      AIUS_SD_EVENT_FAULT);
	fake_card_return(&g_card);
	CHECK(card_size("/aius/f", &got) == 1100u && got[0] == 'o');
	CHECK(card_do(AIUS_SD_ACTION_RENAME, "/aius/f", NULL, 0u, 0u).kind ==
	      AIUS_SD_EVENT_FAULT && g_card.renames == 1u);
}

static void power_cut_sweep(const SettingsFile *f, StartState st, Scenario sc)
{
	for (uint32_t cut = 0u; cut < 256u; cut++) {
		bool fired = power_cut_case(f, st, sc, cut, false);

		(void)power_cut_case(f, st, sc, cut, true);
		if (!fired)
			return;
	}
	CHECK(!"power-cut sweep never reached the job's end");
}

static void test_power_cut_model(void)
{
	SettingsFile *const files[] = {&g_dsp_file, &g_cfg_file};

	make_versions();
	for (unsigned fi = 0u; fi < 2u; fi++) {
		const SettingsFile *f = files[fi];

		for (int i = 0; i < VERSIONS; i++)
			CHECK(validate(f, f->v[i].bytes, f->v[i].length) == i);
		CHECK((f->v[1].length > 512u) == f->multi_chunk);
		power_cut_sweep(f, ST_VALID, SC_SAVE);
		power_cut_sweep(f, ST_VALID, SC_NS6_CHUNK);
		power_cut_sweep(f, ST_VALID, SC_NS6_START);
		power_cut_sweep(f, ST_VALID, SC_NS6_PATH);
		power_cut_sweep(f, ST_VALID, SC_RETRY_FIRST);
		power_cut_sweep(f, ST_VALID, SC_RETRY_SECOND);
		power_cut_sweep(f, ST_MISSING, SC_SAVE);
		power_cut_sweep(f, ST_MISSING, SC_RETRY_FIRST);
		power_cut_sweep(f, ST_MISSING, SC_RETRY_SECOND);
		power_cut_sweep(f, ST_TORN, SC_SAVE);
		power_cut_sweep(f, ST_TORN, SC_RESTORE);
		power_cut_sweep(f, ST_TORN, SC_RETRY_FIRST);
		power_cut_sweep(f, ST_TORN, SC_RETRY_SECOND);
		if (f->multi_chunk)
			power_cut_sweep(f, ST_VALID, SC_E1);
	}
	/* The sweep reached torn targets and restores, not only clean cuts. */
	CHECK(g_torn_targets > 0u && g_restores >= g_torn_targets);
}

/* FR-S7: the overlay job's card request. */
static void test_job_requests(void)
{
	AiusSdOverlayJob j;
	AiusSdJobRequest r;
	const Version *v = &g_dsp_file.v[1];

	make_versions();
	fresh(false);
	g_now = 0u;
	CHECK(aius_sd_overlay_put(&g_ov, "dsp.json", 8u, v->bytes, v->length,
	                          0u) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(next_job(&j));
	CHECK(aius_sd_overlay_job_request(&j, NULL, NULL, &r));
	CHECK(r.kind == AIUS_SD_JOB_UPLOAD &&
	      r.install_mode == AIUS_SD_INSTALL_COPY_BEFORE_WRITE);
	CHECK(r.target_path == j.target_path &&
	      strcmp(r.target_path, "/aius/dsp.json") == 0);
	CHECK(r.backup_path == j.backup_path &&
	      strcmp(r.backup_path, AIUS_SD_OVERLAY_BACKUP_DSP) == 0);
	CHECK(r.temp_path == NULL && r.content_type == NULL && !r.publish &&
	      r.publish_capacity == 0u && !r.require_existing &&
	      r.initial_offset == 0u);
	CHECK(r.total == v->length && !r.target_first && !r.skip_backup);
	CHECK(r.expected_crc32 == zlib_crc32(v->bytes, v->length));
	/* The request runs to COMPLETE on the card: the read-back CRC agrees. */
	fake_card_init(&g_card);
	CHECK(run_job(&j, NULL) == RUN_DONE);
	CHECK(validate_card(&g_dsp_file, "/aius/dsp.json") == 1);
	CHECK(validate_card(&g_dsp_file, AIUS_SD_OVERLAY_BACKUP_DSP) == 1);

	/* Target first; restore-created target only (backup_path kept). */
	j.order = AIUS_SD_OVERLAY_ORDER_TARGET_FIRST;
	CHECK(aius_sd_overlay_job_request(&j, NULL, NULL, &r) && r.target_first &&
	      !r.skip_backup);
	j.skip_backup = true;
	CHECK(aius_sd_overlay_job_request(&j, NULL, NULL, &r) && r.skip_backup &&
	      r.backup_path == j.backup_path);
	j.backup_path[0] = '\0';
	CHECK(!aius_sd_overlay_job_request(&j, NULL, NULL, &r) &&
	      r.kind == AIUS_SD_JOB_PROBE && r.target_path == NULL);

	/* A sync entry: today's REPLACE with the platform's temp and backup. */
	memset(&j, 0, sizeof(j));
	j.kind = AIUS_SD_OVERLAY_JOB_REPLACE;
	j.length = 1500u;
	strcpy(j.target_path, "/aius/www/app.js");
	CHECK(!aius_sd_overlay_job_request(&j, NULL, "/aius/b.tmp", &r));
	CHECK(!aius_sd_overlay_job_request(&j, "/aius/t.tmp", NULL, &r));
	CHECK(aius_sd_overlay_job_request(&j, "/aius/t.tmp", "/aius/b.tmp", &r));
	CHECK(r.kind == AIUS_SD_JOB_UPLOAD &&
	      r.install_mode == AIUS_SD_INSTALL_REPLACE && r.total == 1500u &&
	      strcmp(r.temp_path, "/aius/t.tmp") == 0 &&
	      strcmp(r.backup_path, "/aius/b.tmp") == 0 && !r.target_first);

	/* A settings tombstone's two DELETE jobs, require_existing false (S2). */
	fresh(false);
	CHECK(del("dsp.json", g_now) == AIUS_SD_OVERLAY_PUT_OK);
	CHECK(next_job(&j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_DELETE &&
	      j.delete_step == AIUS_SD_OVERLAY_DELETE_BACKUP);
	CHECK(aius_sd_overlay_job_request(&j, NULL, NULL, &r));
	CHECK(r.kind == AIUS_SD_JOB_DELETE && !r.require_existing &&
	      strcmp(r.target_path, AIUS_SD_OVERLAY_BACKUP_DSP) == 0 &&
	      r.backup_path == NULL && r.total == 0u);
	aius_sd_overlay_job_end(&g_ov, AIUS_SD_OVERLAY_JOB_OK, g_now);
	CHECK(next_job(&j));
	CHECK(j.kind == AIUS_SD_OVERLAY_JOB_DELETE &&
	      j.delete_step == AIUS_SD_OVERLAY_DELETE_TARGET);
	CHECK(aius_sd_overlay_job_request(&j, NULL, NULL, &r));
	CHECK(r.kind == AIUS_SD_JOB_DELETE && !r.require_existing &&
	      strcmp(r.target_path, "/aius/dsp.json") == 0);

	memset(&j, 0, sizeof(j));
	CHECK(!aius_sd_overlay_job_request(&j, NULL, NULL, &r));
	CHECK(!aius_sd_overlay_job_request(NULL, NULL, NULL, &r));
}

int main(void)
{
	test_classes_and_paths();
	test_jobs_and_latest_wins();
	test_version_pin();
	test_readback_and_backoff();
	test_held();
	test_q2();
	test_tombstones();
	test_safe_copy_order();
	test_store_full_shadows_refresh();
	test_n5_preset_classes();
	test_preset_limit_count_rule();
	test_preset_limit_state();
	test_preset_limit_headroom();
	test_b1_faulted_tombstone_path_flip();
	test_n1_backup_delete_after_slot_reuse();
	test_s2_job_ready();
	test_cr_b1_long_lapse();
	test_cr_s1_entry_field_flip();
	test_cr_n1_untrusted_settings_path();
	test_cr2_n1_full_manifest();
	test_cr3_reput_in_place();
	test_cr4_lost_in_place();
	test_restore_decision();
	test_job_requests();
	test_fake_card_rules();
	test_power_cut_model();
	if (g_failures != 0) {
		fprintf(stderr, "sd-overlay: %d of %d checks failed\n", g_failures,
		        g_checks);
		return 1;
	}
	printf("sd-overlay: %d checks passed (%u power cuts)\n", g_checks,
	       g_cuts);
	return 0;
}
