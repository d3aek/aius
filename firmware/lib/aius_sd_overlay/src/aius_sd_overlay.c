#include "aius_sd_overlay.h"

#include <string.h>

/* Serial runtime code -> FLASH, never ITCM (see aius_web_cache.c). */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

#define NO_ENTRY AIUS_WEB_CACHE_NO_ENTRY
#define ROOT_LENGTH 6u /* "/aius/" */

/* AiusSdOverlay.state bits. */
#define ST_HELD 0x01u
#define ST_REFRESH_DUE 0x02u /* one USAGE_REFRESH owed */
#define ST_FLUSHED 0x04u     /* a flush completed since the last refresh */
#define ST_SYNC_BURST 0x08u  /* a sync burst is draining */
/* A settings file's safe copy is backup, owed to its next put: the entry that
 * carried it was dropped, or its path failed its CRC (CR2-S1). */
#define ST_OWED_BACKUP_DSP 0x10u
#define ST_OWED_BACKUP_CONFIG 0x20u
/* A Q2 re-put of the settings file is owed, retried each step (D6). */
#define ST_REPUT_DSP 0x40u
#define ST_REPUT_CONFIG 0x80u

/* AiusSdOverlay.pass: NO_ENTRY, or PASS_BACKUP_RUNNING|slot while a settings
 * tombstone's backup DELETE runs, or PASS_BACKUP_DONE|slot once it completed
 * and its target DELETE is owed (honoured only while owed_target finds that
 * same commit in the slot, N1), or PASS_JOB|file (|PASS_JOB_NOTED once a
 * pass note arrived) while dsp.json's or config.json's copy-before-write
 * job runs: the job's file, captured while its entry was sound, since a
 * later entry fault leaves its path untrusted (CR2-S1). Slots are below 64,
 * so PASS_JOB (both high bits) never reads as a slot pass. */
#define PASS_BACKUP_RUNNING 0x80u
#define PASS_BACKUP_DONE 0x40u
#define PASS_SLOT 0x3Fu
#define PASS_JOB 0xC0u
#define PASS_JOB_NOTED 0x02u
#define PASS_JOB_FILE 0x01u

/* The Teensy link script copies every .rodata (string literals included) into
 * DTCM .data; naming `.progmem` keeps these in flash (the aius_web_cache CRC
 * table shape). Flash is memory-mapped, so reads need no accessor. Every
 * literal this file hands out is one of these arrays, never an inline string.
 * Target-only: the host has no such section. */
#if defined(__arm__)
#define AIUS_SD_OVERLAY_FLASHRO __attribute__((section(".progmem")))
#else
#define AIUS_SD_OVERLAY_FLASHRO
#endif

static const char k_dsp[] AIUS_SD_OVERLAY_FLASHRO = "dsp.json";
static const char k_config[] AIUS_SD_OVERLAY_FLASHRO = "config.json";
static const char k_presets[] AIUS_SD_OVERLAY_FLASHRO = "www/presets/";
static const char k_json[] AIUS_SD_OVERLAY_FLASHRO = ".json";
static const char k_root[] AIUS_SD_OVERLAY_FLASHRO = AIUS_SD_OVERLAY_ROOT;
static const char k_bak_dsp[] AIUS_SD_OVERLAY_FLASHRO =
	AIUS_SD_OVERLAY_BACKUP_DSP;
static const char k_bak_config[] AIUS_SD_OVERLAY_FLASHRO =
	AIUS_SD_OVERLAY_BACKUP_CONFIG;
static const char k_bak_preset[] AIUS_SD_OVERLAY_FLASHRO =
	AIUS_SD_OVERLAY_BACKUP_PRESET_PREFIX;

static FLASHMEM char fold(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static FLASHMEM bool fold_equal(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (fold(a[i]) != fold(b[i]))
			return false;
	return true;
}

static FLASHMEM bool is_key(const char *path, size_t length, const char *key)
{
	const size_t n = strlen(key);

	return length == n && fold_equal(path, key, n);
}

/* "www/presets/<name>.json" with a non-empty name and no further '/', all
 * case-folded (N1, N5): the only preset key with a backup (FR-S9). Any other
 * name under www/presets/ is sync class. */
static FLASHMEM bool is_preset(const char *path, size_t length)
{
	const size_t pn = sizeof(k_presets) - 1u;
	const size_t jn = sizeof(k_json) - 1u;
	const char *name;
	size_t name_length;

	if (path == NULL || length <= pn || !fold_equal(path, k_presets, pn))
		return false;
	name = path + pn;
	name_length = length - pn;
	return name_length > jn &&
	       fold_equal(name + name_length - jn, k_json, jn) &&
	       memchr(name, '/', name_length) == NULL &&
	       memchr(name, '\0', name_length) == NULL;
}

static FLASHMEM AiusWebCache *store(const AiusSdOverlay *overlay)
{
	if (overlay == NULL || overlay->platform == NULL ||
	    overlay->platform->cache == NULL ||
	    overlay->platform->cache->base == NULL)
		return NULL;
	return overlay->platform->cache;
}

/* Hint read of an entry's state and flags straight from the manifest (the
 * CPU is the only writer, so its dcache is coherent with itself); the chosen
 * entry is then read through aius_web_cache_entry_info, which checks its CRC
 * on the chip. */
static FLASHMEM const AiusWebCacheEntry *peek(const AiusWebCache *cache,
                                              uint32_t i)
{
	return (const AiusWebCacheEntry *)(const void *)(cache->base +
	                                                 i * AIUS_WEB_CACHE_ENTRY_BYTES);
}

/* millis() stamps (CR-B1). Every stamp lies at or before `now_ms` except a
 * backoff's, which lies at most AIUS_SD_OVERLAY_BACKOFF_MAX_MS ahead, so
 * ages are unsigned differences, right for any lapse under 2^32 ms; never a
 * signed difference, which reads a lapse past 2^31 ms (24.8 days, e.g. a Q1
 * write and a card inserted weeks later) as the future. */
static FLASHMEM uint32_t age(uint32_t now_ms, uint32_t stamp_ms)
{
	return now_ms - stamp_ms;
}

/* Debounced: at least the debounce since the accept. A lapse of a whole
 * multiple of 2^32 ms reads a 500 ms window early; nothing reads late. */
static FLASHMEM bool debounced(uint32_t now_ms, uint32_t accept_ms)
{
	return age(now_ms, accept_ms) >= AIUS_SD_OVERLAY_DEBOUNCE_MS;
}

/* The backoff has passed unless `now_ms` lies within the backoff window
 * before `next_try_ms`: a stamp further "ahead" than any backoff is one long
 * passed (a lapse past 2^31 ms), so it reads due. */
static FLASHMEM bool backoff_passed(uint32_t now_ms, uint32_t next_try_ms)
{
	const uint32_t ahead = next_try_ms - now_ms;

	return next_try_ms == 0u || ahead == 0u ||
	       ahead > AIUS_SD_OVERLAY_BACKOFF_MAX_MS;
}

FLASHMEM void aius_sd_overlay_init(AiusSdOverlay *overlay,
                                   const AiusSdOverlayPlatform *platform,
                                   bool held)
{
	if (overlay == NULL)
		return;
	memset(overlay, 0, sizeof(*overlay));
	overlay->platform = platform;
	overlay->flushing_slot = NO_ENTRY;
	overlay->pass = NO_ENTRY;
	if (held)
		overlay->state |= ST_HELD;
}

FLASHMEM AiusSdOverlayClass aius_sd_overlay_classify(const char *path,
                                                     size_t path_length)
{
	if (path == NULL)
		return AIUS_SD_OVERLAY_CLASS_SYNC;
	if (is_key(path, path_length, k_dsp) ||
	    is_key(path, path_length, k_config) || is_preset(path, path_length))
		return AIUS_SD_OVERLAY_CLASS_SETTINGS;
	return AIUS_SD_OVERLAY_CLASS_SYNC;
}

static FLASHMEM bool copy_out(char *out, size_t capacity, const char *a,
                              size_t alen, const char *b, size_t blen)
{
	if (out == NULL || alen + blen + 1u > capacity)
		return false;
	memcpy(out, a, alen);
	memcpy(out + alen, b, blen);
	out[alen + blen] = '\0';
	return true;
}

FLASHMEM bool aius_sd_overlay_backup_path(const char *path, size_t path_length,
                                          char *out, size_t capacity)
{
	const size_t pn = sizeof(k_presets) - 1u;

	if (path == NULL)
		return false;
	if (is_key(path, path_length, k_dsp))
		return copy_out(out, capacity, k_bak_dsp, sizeof(k_bak_dsp) - 1u,
		                k_bak_dsp, 0u);
	if (is_key(path, path_length, k_config))
		return copy_out(out, capacity, k_bak_config,
		                sizeof(k_bak_config) - 1u, k_bak_config, 0u);
	if (!is_preset(path, path_length))
		return false;
	return copy_out(out, capacity, k_bak_preset, sizeof(k_bak_preset) - 1u,
	                path + pn, path_length - pn);
}

static FLASHMEM AiusSdOverlayPutResult map(AiusWebCachePutResult r)
{
	switch (r) {
	case AIUS_WEB_CACHE_PUT_OK:
		return AIUS_SD_OVERLAY_PUT_OK;
	case AIUS_WEB_CACHE_PUT_FULL:
		return AIUS_SD_OVERLAY_PUT_BUSY;
	case AIUS_WEB_CACHE_PUT_FAULT:
	case AIUS_WEB_CACHE_PUT_FAULT_LOST:
	case AIUS_WEB_CACHE_PUT_FAULT_LOST_BACKUP:
		return AIUS_SD_OVERLAY_PUT_FAULT;
	case AIUS_WEB_CACHE_PUT_INVALID:
	default:
		return AIUS_SD_OVERLAY_PUT_INVALID;
	}
}

/* The accept-time checks common to every put: the store exists, the key
 * fits, and a settings key's backup path can be formed. */
static FLASHMEM AiusSdOverlayPutResult admit(const AiusSdOverlay *overlay,
                                             const char *path,
                                             size_t path_length,
                                             uint8_t *flags)
{
	char backup[AIUS_SD_OVERLAY_PATH_STORAGE];

	if (store(overlay) == NULL)
		return AIUS_SD_OVERLAY_PUT_ABSENT;
	if (path == NULL || path_length == 0u ||
	    path_length > AIUS_WEB_CACHE_PATH_MAX ||
	    ROOT_LENGTH + path_length + 1u > AIUS_SD_OVERLAY_PATH_STORAGE)
		return AIUS_SD_OVERLAY_PUT_INVALID;
	*flags = 0u;
	if (aius_sd_overlay_classify(path, path_length) ==
	    AIUS_SD_OVERLAY_CLASS_SETTINGS) {
		if (!aius_sd_overlay_backup_path(path, path_length, backup,
		                                 sizeof(backup)))
			return AIUS_SD_OVERLAY_PUT_INVALID;
		*flags = AIUS_WEB_CACHE_FLAG_SETTINGS;
	}
	return AIUS_SD_OVERLAY_PUT_OK;
}

/* `path` names dsp.json or config.json: which, through `file`. */
static FLASHMEM bool settings_file(const char *path, size_t length,
                                   AiusSdOverlaySettingsFile *file)
{
	if (is_key(path, length, k_dsp))
		*file = AIUS_SD_OVERLAY_FILE_DSP;
	else if (is_key(path, length, k_config))
		*file = AIUS_SD_OVERLAY_FILE_CONFIG;
	else
		return false;
	return true;
}

static FLASHMEM const char *key_of(AiusSdOverlaySettingsFile file)
{
	return file == AIUS_SD_OVERLAY_FILE_DSP ? k_dsp : k_config;
}

static FLASHMEM uint8_t owed_bit(AiusSdOverlaySettingsFile file)
{
	return file == AIUS_SD_OVERLAY_FILE_DSP ? ST_OWED_BACKUP_DSP
	                                        : ST_OWED_BACKUP_CONFIG;
}

static FLASHMEM uint8_t reput_bit(AiusSdOverlaySettingsFile file)
{
	return file == AIUS_SD_OVERLAY_FILE_DSP ? ST_REPUT_DSP : ST_REPUT_CONFIG;
}

/* The running job is `file`'s copy-before-write. */
static FLASHMEM bool job_file(const AiusSdOverlay *overlay,
                              AiusSdOverlaySettingsFile *file)
{
	if ((overlay->pass & (uint8_t)~(PASS_JOB_NOTED | PASS_JOB_FILE)) !=
	    PASS_JOB)
		return false;
	*file = (overlay->pass & PASS_JOB_FILE) != 0u ? AIUS_SD_OVERLAY_FILE_CONFIG
	                                              : AIUS_SD_OVERLAY_FILE_DSP;
	return true;
}

static FLASHMEM void owe(AiusSdOverlay *overlay, AiusSdOverlaySettingsFile file,
                         AiusWebCacheSafeCopy which)
{
	if (which == AIUS_WEB_CACHE_SAFE_BACKUP)
		overlay->state |= owed_bit(file);
	else
		overlay->state &= (uint8_t)~owed_bit(file);
}

/* After an accepted put or tombstone on `path`: a safe copy owed to that
 * settings file lands on its newest pending entry (the one just committed).
 * A restore's own verdict, read from the card, supersedes it. */
static FLASHMEM void settle_owed(AiusSdOverlay *overlay, const char *path,
                                 size_t length, bool restore)
{
	AiusSdOverlaySettingsFile file;
	const char *key;

	if (!settings_file(path, length, &file) ||
	    (overlay->state & owed_bit(file)) == 0u)
		return;
	overlay->state &= (uint8_t)~owed_bit(file);
	key = key_of(file);
	if (!restore)
		(void)aius_web_cache_set_safe_copy(store(overlay), key, strlen(key),
		                                   AIUS_WEB_CACHE_SAFE_BACKUP);
}

/* A one-shot put that reused `path`'s pending entry in place faulted and
 * lost that version (CR4-S1). dsp.json / config.json: the running settings
 * are authoritative, so the key's Q2 re-put is owed (drained false until it
 * lands, D6, H9), carrying the lost entry's safe copy (H4). Any other path:
 * the version is dropped, counted and logged as Q2 drops one. */
static FLASHMEM void note_lost(AiusSdOverlay *overlay, const char *path,
                               size_t length, AiusWebCachePutResult r)
{
	const AiusSdOverlayPlatform *p = overlay->platform;
	AiusSdOverlaySettingsFile file;
	char name[AIUS_WEB_CACHE_PATH_STORAGE];

	if (r != AIUS_WEB_CACHE_PUT_FAULT_LOST &&
	    r != AIUS_WEB_CACHE_PUT_FAULT_LOST_BACKUP)
		return;
	if (settings_file(path, length, &file)) {
		owe(overlay, file,
		    r == AIUS_WEB_CACHE_PUT_FAULT_LOST_BACKUP
		            ? AIUS_WEB_CACHE_SAFE_BACKUP
		            : AIUS_WEB_CACHE_SAFE_TARGET);
		overlay->state |= reput_bit(file);
		return;
	}
	overlay->crc_drops++;
	if (p->event == NULL || length >= sizeof(name))
		return;
	memcpy(name, path, length);
	name[length] = '\0';
	p->event(p->context, AIUS_SD_OVERLAY_EVENT_CRC_DROP, name);
}

static FLASHMEM void note_accept(AiusSdOverlay *overlay, uint8_t flags,
                                 uint32_t now_ms)
{
	if ((flags & AIUS_WEB_CACHE_FLAG_SETTINGS) == 0u &&
	    (overlay->state & ST_SYNC_BURST) == 0u) {
		overlay->state |= ST_SYNC_BURST;
		overlay->sync_burst_start_ms = now_ms;
	}
}

static FLASHMEM AiusSdOverlayPutResult put_flags(AiusSdOverlay *overlay,
                                                 const char *path,
                                                 size_t path_length,
                                                 const uint8_t *bytes,
                                                 uint32_t length,
                                                 uint8_t extra,
                                                 uint32_t now_ms)
{
	uint8_t flags = 0u;
	AiusSdOverlayPutResult r = admit(overlay, path, path_length, &flags);

	if (r != AIUS_SD_OVERLAY_PUT_OK)
		return r;
	flags |= extra;
	{
		const AiusWebCachePutResult w =
			aius_web_cache_put(store(overlay), path, path_length,
			                   bytes, length, flags, now_ms);

		note_lost(overlay, path, path_length, w);
		r = map(w);
	}
	if (r == AIUS_SD_OVERLAY_PUT_OK) {
		settle_owed(overlay, path, path_length,
		            (extra & AIUS_WEB_CACHE_FLAG_RESTORE_CREATED) != 0u);
		note_accept(overlay, flags, now_ms);
	}
	return r;
}

FLASHMEM AiusSdOverlayPutResult aius_sd_overlay_put(AiusSdOverlay *overlay,
                                                    const char *path,
                                                    size_t path_length,
                                                    const uint8_t *bytes,
                                                    uint32_t length,
                                                    uint32_t now_ms)
{
	return put_flags(overlay, path, path_length, bytes, length, 0u, now_ms);
}

FLASHMEM AiusSdOverlayPutResult aius_sd_overlay_put_restore(
	AiusSdOverlay *overlay, const char *path, size_t path_length,
	const uint8_t *bytes, uint32_t length, uint32_t now_ms)
{
	return put_flags(overlay, path, path_length, bytes, length,
	                 AIUS_WEB_CACHE_FLAG_RESTORE_CREATED, now_ms);
}

FLASHMEM AiusSdOverlayPutResult aius_sd_overlay_put_begin(
	AiusSdOverlay *overlay, const char *path, size_t path_length,
	uint32_t length)
{
	uint8_t flags = 0u;
	AiusSdOverlayPutResult r = admit(overlay, path, path_length, &flags);

	if (r != AIUS_SD_OVERLAY_PUT_OK)
		return r;
	return map(aius_web_cache_put_begin(store(overlay), path, path_length,
	                                    length, flags));
}

FLASHMEM AiusSdOverlayPutResult aius_sd_overlay_put_chunk(
	AiusSdOverlay *overlay, const uint8_t *bytes, uint32_t length)
{
	if (store(overlay) == NULL)
		return AIUS_SD_OVERLAY_PUT_ABSENT;
	return map(aius_web_cache_put_chunk(store(overlay), bytes, length));
}

FLASHMEM AiusSdOverlayPutResult aius_sd_overlay_put_commit(
	AiusSdOverlay *overlay, uint32_t now_ms)
{
	AiusWebCache *cache = store(overlay);
	AiusWebCacheEntryInfo info;
	uint8_t flags = 0u;
	bool have_info = false;
	AiusSdOverlayPutResult r;

	if (cache == NULL)
		return AIUS_SD_OVERLAY_PUT_ABSENT;
	if (cache->put_entry != NO_ENTRY &&
	    aius_web_cache_entry_info(cache, cache->put_entry, &info)) {
		flags = info.flags;
		have_info = true;
	}
	r = map(aius_web_cache_put_commit(cache, now_ms));
	if (r == AIUS_SD_OVERLAY_PUT_OK) {
		if (have_info)
			settle_owed(overlay, info.path, strlen(info.path), false);
		note_accept(overlay, flags, now_ms);
	}
	return r;
}

FLASHMEM void aius_sd_overlay_put_abort(AiusSdOverlay *overlay)
{
	if (store(overlay) != NULL)
		aius_web_cache_put_abort(store(overlay));
}

FLASHMEM AiusSdOverlayPutResult aius_sd_overlay_tombstone(
	AiusSdOverlay *overlay, const char *path, size_t path_length,
	uint32_t now_ms)
{
	uint8_t flags = 0u;
	AiusSdOverlayPutResult r;

	if (store(overlay) == NULL)
		return AIUS_SD_OVERLAY_PUT_ABSENT;
	if (path == NULL || path_length == 0u ||
	    path_length > AIUS_WEB_CACHE_PATH_MAX)
		return AIUS_SD_OVERLAY_PUT_INVALID;
	/* A settings tombstone needs no backup path: without one it deletes
	 * the target alone (FR-S9). */
	if (aius_sd_overlay_classify(path, path_length) ==
	    AIUS_SD_OVERLAY_CLASS_SETTINGS)
		flags = AIUS_WEB_CACHE_FLAG_SETTINGS;
	r = map(aius_web_cache_tombstone(store(overlay), path, path_length,
	                                 flags, now_ms));
	if (r == AIUS_SD_OVERLAY_PUT_OK) {
		settle_owed(overlay, path, path_length, false);
		note_accept(overlay, flags, now_ms);
	}
	return r;
}

FLASHMEM bool aius_sd_overlay_shadows(const AiusSdOverlay *overlay,
                                      const char *path, size_t path_length)
{
	const AiusWebCacheState s =
		aius_web_cache_pending_state(store(overlay), path, path_length);

	return s == AIUS_WEB_CACHE_STATE_DIRTY ||
	       s == AIUS_WEB_CACHE_STATE_FLUSHING ||
	       s == AIUS_WEB_CACHE_STATE_TOMBSTONE;
}

static FLASHMEM bool hint_pending(uint8_t state)
{
	return state == AIUS_WEB_CACHE_STATE_DIRTY ||
	       state == AIUS_WEB_CACHE_STATE_FLUSHING ||
	       state == AIUS_WEB_CACHE_STATE_TOMBSTONE;
}

FLASHMEM bool aius_sd_overlay_drained(const AiusSdOverlay *overlay)
{
	const AiusWebCache *cache = store(overlay);

	if (cache == NULL)
		return true;
	/* An owed Q2 re-put is a pending settings write: it holds the OTA and
	 * reboot gate shut until it lands (D6, H9). It always can once its
	 * file's own entry is unpinned, or an entry frees (CR3-B1). */
	if ((overlay->state & (ST_REPUT_DSP | ST_REPUT_CONFIG)) != 0u)
		return false;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++)
		if (hint_pending(peek(cache, i)->state))
			return false;
	return true;
}

/* No sync-class entry pending: the sync burst has drained. */
static FLASHMEM bool sync_drained(const AiusWebCache *cache)
{
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const AiusWebCacheEntry *e = peek(cache, i);

		if (hint_pending(e->state) &&
		    (e->flags & AIUS_WEB_CACHE_FLAG_SETTINGS) == 0u)
			return false;
	}
	return true;
}

/* Ranks a over b at `now_ms`: settings class first, then oldest accept (by
 * age, CR-B1), then commit. */
static FLASHMEM bool ranks_before(const AiusWebCacheEntry *a,
                                  const AiusWebCacheEntry *b, uint32_t now_ms)
{
	const bool as = (a->flags & AIUS_WEB_CACHE_FLAG_SETTINGS) != 0u;
	const bool bs = (b->flags & AIUS_WEB_CACHE_FLAG_SETTINGS) != 0u;

	if (as != bs)
		return as;
	if (a->accept_ms != b->accept_ms)
		return age(now_ms, a->accept_ms) > age(now_ms, b->accept_ms);
	return (int32_t)(a->commit_seq - b->commit_seq) < 0;
}

static FLASHMEM bool absolute(const char *rel, char *out)
{
	const size_t n = strlen(rel);

	return copy_out(out, AIUS_SD_OVERLAY_PATH_STORAGE, k_root, ROOT_LENGTH,
	                rel, n);
}

/* No job may start: no store, a job running, or held. */
static FLASHMEM bool closed(const AiusSdOverlay *overlay)
{
	return store(overlay) == NULL || overlay->flushing_slot != NO_ENTRY ||
	       (overlay->state & ST_HELD) != 0u;
}

/* The tombstone whose target DELETE is owed: `pass` names it as
 * PASS_BACKUP_DONE, and it is still the very commit whose backup DELETE
 * completed. _job_end marks that commit with tries 0 and next_try_ms set
 * (a failure counts a try; any re-commit in the slot, a reused or a fresh
 * entry, zeroes both), so a tombstone landing in a reused slot never
 * inherits the pass (N1). NO_ENTRY when none. */
static FLASHMEM uint8_t owed_target(const AiusSdOverlay *overlay,
                                    const AiusWebCache *cache)
{
	uint8_t s;
	const AiusWebCacheEntry *e;

	if (overlay->pass == NO_ENTRY ||
	    (overlay->pass & (PASS_BACKUP_RUNNING | PASS_BACKUP_DONE)) !=
	            PASS_BACKUP_DONE)
		return NO_ENTRY;
	s = overlay->pass & PASS_SLOT;
	if (s >= AIUS_WEB_CACHE_ENTRIES)
		return NO_ENTRY;
	e = peek(cache, s);
	if (e->state != AIUS_WEB_CACHE_STATE_TOMBSTONE ||
	    (e->flags & (AIUS_WEB_CACHE_FLAG_CRC_FAULTED |
	                 AIUS_WEB_CACHE_FLAG_DELETE_RUNNING)) != 0u ||
	    e->tries != 0u || e->next_try_ms == 0u)
		return NO_ENTRY;
	return s;
}

/* The scheduler's choice, shared by _next_job and _job_ready so the two
 * cannot diverge: an owed target DELETE first, ahead of the debounce and the
 * queue; else the best due entry (debounced, backoff passed, not
 * CRC_FAULTED, no DELETE running; settings first, oldest accept). The pick is
 * a hint read; the caller checks it on the chip. NO_ENTRY when none. */
static FLASHMEM uint8_t pick(const AiusSdOverlay *overlay,
                             const AiusWebCache *cache, uint32_t now_ms,
                             bool *owed)
{
	const AiusWebCacheEntry *best = NULL;
	uint8_t slot = owed_target(overlay, cache);

	*owed = slot != NO_ENTRY;
	if (*owed)
		return slot;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const AiusWebCacheEntry *e = peek(cache, i);

		if ((e->state != AIUS_WEB_CACHE_STATE_DIRTY &&
		     e->state != AIUS_WEB_CACHE_STATE_TOMBSTONE) ||
		    (e->flags & (AIUS_WEB_CACHE_FLAG_CRC_FAULTED |
		                 AIUS_WEB_CACHE_FLAG_DELETE_RUNNING)) != 0u)
			continue;
		if (!debounced(now_ms, e->accept_ms) ||
		    !backoff_passed(now_ms, e->next_try_ms))
			continue;
		if (best == NULL || ranks_before(e, best, now_ms)) {
			best = e;
			slot = (uint8_t)i;
		}
	}
	return slot;
}

/* The pick, read through its CRC check on the chip. A failing pending entry
 * comes back flagged CRC_FAULTED, so the next pick skips it: each round
 * flags one more entry, bounded by the entry count. NO_ENTRY when none is
 * sound and due. */
static FLASHMEM uint8_t pick_sound(const AiusSdOverlay *overlay,
                                   AiusWebCache *cache, uint32_t now_ms,
                                   bool *owed, AiusWebCacheEntryInfo *info)
{
	for (uint32_t n = 0; n <= AIUS_WEB_CACHE_ENTRIES; n++) {
		const uint8_t slot = pick(overlay, cache, now_ms, owed);

		if (slot == NO_ENTRY)
			return NO_ENTRY;
		if (aius_web_cache_entry_info(cache, slot, info) &&
		    (info->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) == 0u)
			return slot;
	}
	*owed = false;
	return NO_ENTRY;
}

FLASHMEM bool aius_sd_overlay_job_ready(const AiusSdOverlay *overlay,
                                        uint32_t now_ms)
{
	AiusWebCacheEntryInfo info;
	bool owed;

	if (overlay == NULL || closed(overlay))
		return false;
	return pick_sound(overlay, store(overlay), now_ms, &owed, &info) !=
	       NO_ENTRY;
}

FLASHMEM bool aius_sd_overlay_next_job(AiusSdOverlay *overlay, uint32_t now_ms,
                                       AiusSdOverlayJob *job)
{
	AiusWebCache *cache = store(overlay);
	AiusWebCacheEntryInfo info;
	uint8_t slot;
	bool owed;
	bool settings;
	bool have_backup;
	char backup[AIUS_SD_OVERLAY_PATH_STORAGE];

	if (overlay == NULL || job == NULL || closed(overlay))
		return false;
	memset(job, 0, sizeof(*job));
	slot = pick_sound(overlay, cache, now_ms, &owed, &info);
	/* A pass naming anything but this owed tombstone is stale. */
	if (!owed)
		overlay->pass = NO_ENTRY;
	if (slot == NO_ENTRY || !aius_web_cache_flush_begin(cache, slot))
		return false;
	settings = (info.flags & AIUS_WEB_CACHE_FLAG_SETTINGS) != 0u;
	have_backup = settings &&
	              aius_sd_overlay_backup_path(info.path, strlen(info.path),
	                                          backup, sizeof(backup));
	job->slot = slot;
	job->commit_seq = info.commit_seq;
	job->require_existing = false;
	if (!absolute(info.path, job->target_path)) {
		aius_web_cache_flush_end(cache, slot, false);
		return false;
	}
	if (info.state == AIUS_WEB_CACHE_STATE_TOMBSTONE) {
		job->kind = AIUS_SD_OVERLAY_JOB_DELETE;
		if (have_backup && !owed) {
			/* The backup's DELETE first, so a cut between the two
			 * leaves the target and no backup. */
			job->delete_step = AIUS_SD_OVERLAY_DELETE_BACKUP;
			memcpy(job->target_path, backup, sizeof(backup));
			memcpy(job->backup_path, backup, sizeof(backup));
			overlay->pass = (uint8_t)(PASS_BACKUP_RUNNING | slot);
		} else {
			job->delete_step = AIUS_SD_OVERLAY_DELETE_TARGET;
			overlay->pass = NO_ENTRY;
		}
	} else if (have_backup) {
		job->kind = AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE;
		job->length = info.length;
		job->file_crc = info.file_crc;
		memcpy(job->backup_path, backup, sizeof(backup));
		job->order = (info.flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) != 0u
			? AIUS_SD_OVERLAY_ORDER_TARGET_FIRST
			: AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST;
		/* A restore-created version, unchanged: its backup already holds
		 * these bytes (FR-B1). */
		job->skip_backup =
			(info.flags & AIUS_WEB_CACHE_FLAG_RESTORE_CREATED) != 0u;
		if (job->skip_backup)
			job->order = AIUS_SD_OVERLAY_ORDER_TARGET_FIRST;
		{
			AiusSdOverlaySettingsFile file;

			overlay->pass =
				settings_file(info.path, strlen(info.path), &file)
					? (uint8_t)(PASS_JOB |
					            (file == AIUS_SD_OVERLAY_FILE_CONFIG
					                     ? PASS_JOB_FILE
					                     : 0u))
					: NO_ENTRY;
		}
	} else {
		job->kind = AIUS_SD_OVERLAY_JOB_REPLACE;
		job->length = info.length;
		job->file_crc = info.file_crc;
		overlay->pass = NO_ENTRY;
	}
	overlay->flushing_slot = slot;
	return true;
}

FLASHMEM bool aius_sd_overlay_job_input(AiusSdOverlay *overlay,
                                        uint32_t offset, uint8_t *dst,
                                        uint32_t capacity, uint32_t *bytes)
{
	AiusWebCache *cache = store(overlay);
	AiusWebCacheEntryInfo info;
	uint32_t done = 0u;

	if (bytes != NULL)
		*bytes = 0u;
	if (cache == NULL || dst == NULL || bytes == NULL ||
	    overlay->flushing_slot == NO_ENTRY ||
	    offset % AIUS_WEB_CACHE_CHUNK_BYTES != 0u ||
	    !aius_web_cache_entry_info(cache, overlay->flushing_slot, &info) ||
	    info.state != AIUS_WEB_CACHE_STATE_FLUSHING || offset > info.length)
		return false;
	/* entry_info re-checked the entry's own CRC: a flipped start or
	 * length fails the job before any WRITE of it (CR-S1, Q2's path);
	 * copy_chunk re-checks it per chunk. */
	if ((info.flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u)
		return false;
	while (offset + done < info.length &&
	       capacity - done >= AIUS_WEB_CACHE_CHUNK_BYTES) {
		uint32_t n = 0u;

		if (!aius_web_cache_copy_chunk(
			    cache, overlay->flushing_slot,
			    (uint16_t)((offset + done) / AIUS_WEB_CACHE_CHUNK_BYTES),
			    dst + done, &n))
			return false;
		done += n;
	}
	/* A tail shorter than a chunk fits a smaller capacity. */
	if (offset + done < info.length && capacity - done >= info.length - offset - done) {
		uint8_t chunk[AIUS_WEB_CACHE_CHUNK_BYTES];
		uint32_t n = 0u;

		if (!aius_web_cache_copy_chunk(
			    cache, overlay->flushing_slot,
			    (uint16_t)((offset + done) / AIUS_WEB_CACHE_CHUNK_BYTES),
			    chunk, &n))
			return false;
		memcpy(dst + done, chunk, n);
		done += n;
	}
	*bytes = done;
	return true;
}

static FLASHMEM void set_safe(AiusSdOverlay *overlay,
                              AiusWebCacheSafeCopy which)
{
	AiusWebCache *cache = store(overlay);
	AiusWebCacheEntryInfo info;
	AiusSdOverlaySettingsFile file;
	const char *key;

	if (cache == NULL || overlay->flushing_slot == NO_ENTRY ||
	    !aius_web_cache_entry_info(cache, overlay->flushing_slot, &info))
		return;
	/* A chunk fault leaves the path trusted: the note lands as usual, so
	 * a backup pass that CLOSEd before a faulted target pass moves the
	 * safe copy to the backup for the Q2 re-put (CR2-S1, NS6). */
	if ((info.flags & AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED) == 0u) {
		(void)aius_web_cache_set_safe_copy(cache, info.path,
		                                   strlen(info.path), which);
		return;
	}
	/* The entry failed its own CRC: its path may name another file. A
	 * settings job's file was captured while the entry was sound; the
	 * note goes to that key's newest pending entry, or is owed to its next
	 * put (the re-put) when none is pending. Any other job's note is
	 * dropped: no card copy of a preset or www file is re-put. */
	if (!job_file(overlay, &file))
		return;
	overlay->pass |= PASS_JOB_NOTED;
	key = key_of(file);
	if (!aius_web_cache_set_safe_copy(cache, key, strlen(key), which))
		owe(overlay, file, which);
}

FLASHMEM void aius_sd_overlay_note_pass_closed(AiusSdOverlay *overlay,
                                               AiusWebCacheSafeCopy which)
{
	if (overlay != NULL)
		set_safe(overlay, which);
}

FLASHMEM void aius_sd_overlay_note_readback(AiusSdOverlay *overlay, bool ok)
{
	if (overlay != NULL)
		set_safe(overlay, ok ? AIUS_WEB_CACHE_SAFE_TARGET
		                     : AIUS_WEB_CACHE_SAFE_BACKUP);
}

static FLASHMEM void after_flush(AiusSdOverlay *overlay, uint32_t now_ms)
{
	AiusWebCache *cache = store(overlay);

	overlay->state |= ST_FLUSHED;
	if ((overlay->state & ST_SYNC_BURST) != 0u && sync_drained(cache)) {
		const uint32_t d = now_ms - overlay->sync_burst_start_ms;

		if (d > overlay->sync_drain_ms)
			overlay->sync_drain_ms = d;
		overlay->state &= (uint8_t)~ST_SYNC_BURST;
	}
	if (aius_sd_overlay_drained(overlay)) {
		overlay->state |= ST_REFRESH_DUE;
		overlay->state &= (uint8_t)~ST_FLUSHED;
	}
}

FLASHMEM void aius_sd_overlay_job_end(AiusSdOverlay *overlay,
                                      AiusSdOverlayJobOutcome outcome,
                                      uint32_t now_ms)
{
	AiusWebCache *cache = store(overlay);
	AiusWebCacheEntryInfo info;
	uint8_t slot;
	bool backup_step;

	if (cache == NULL || overlay->flushing_slot == NO_ENTRY)
		return;
	slot = overlay->flushing_slot;
	overlay->flushing_slot = NO_ENTRY;
	backup_step = overlay->pass == (PASS_BACKUP_RUNNING | slot);
	if (!aius_web_cache_entry_info(cache, slot, &info)) {
		overlay->pass = NO_ENTRY;
		return;
	}
	{
		AiusSdOverlaySettingsFile file;

		/* A settings job whose entry failed its own CRC, no pass noted:
		 * the safe copy it started with is still the card's. When the
		 * key finds no pending entry to carry it (the path flipped),
		 * owe it to the key's next put, the Q2 re-put (CR2-S1). */
		if (job_file(overlay, &file) &&
		    (overlay->pass & PASS_JOB_NOTED) == 0u &&
		    (info.flags & AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED) != 0u &&
		    !aius_sd_overlay_shadows(overlay, key_of(file),
		                             strlen(key_of(file))))
			owe(overlay, file,
			    (info.flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) != 0u
			            ? AIUS_WEB_CACHE_SAFE_BACKUP
			            : AIUS_WEB_CACHE_SAFE_TARGET);
	}
	if (outcome == AIUS_SD_OVERLAY_JOB_OK) {
		if (backup_step) {
			/* The backup is gone: the target is the only copy the next
			 * job must write last (G1). The tombstone stays for its
			 * target DELETE unless a newer version retires it. */
			overlay->flushing_slot = slot;
			set_safe(overlay, AIUS_WEB_CACHE_SAFE_TARGET);
			overlay->flushing_slot = NO_ENTRY;
			aius_web_cache_flush_end(cache, slot, false);
			/* tries 0 with next_try_ms set marks this commit's backup
			 * DELETE done (owed_target); next_try_ms is due now. */
			(void)aius_web_cache_set_retry(cache, slot,
			                               now_ms == 0u ? 1u : now_ms, 0u);
			overlay->pass = (uint8_t)(PASS_BACKUP_DONE | slot);
			return;
		}
		if (info.state == AIUS_WEB_CACHE_STATE_FLUSHING &&
		    (info.flags & AIUS_WEB_CACHE_FLAG_SETTINGS) != 0u) {
			const uint32_t latency = now_ms - info.accept_ms;

			if (latency > overlay->max_settings_latency_ms)
				overlay->max_settings_latency_ms = latency;
		}
		aius_web_cache_flush_end(cache, slot, true);
		overlay->flushed++;
		overlay->pass = NO_ENTRY;
		after_flush(overlay, now_ms);
		return;
	}
	overlay->pass = NO_ENTRY;
	overlay->last_fault = (uint8_t)outcome;
	aius_web_cache_flush_end(cache, slot, false);
	switch (outcome) {
	case AIUS_SD_OVERLAY_JOB_CARD_GONE:
		overlay->failures++;
		overlay->state |= ST_HELD;
		break;
	case AIUS_SD_OVERLAY_JOB_CHUNK_CRC:
		/* copy_chunk flagged the entry; the service step takes Q2's
		 * path. Not a card failure. */
		break;
	case AIUS_SD_OVERLAY_JOB_FAILED:
	default:
		overlay->failures++;
		if ((overlay->state & ST_HELD) == 0u) {
			const uint8_t tries =
				info.tries == UINT8_MAX ? UINT8_MAX
				                        : (uint8_t)(info.tries + 1u);
			uint32_t delay = AIUS_SD_OVERLAY_BACKOFF_MIN_MS;
			uint32_t next;

			for (uint8_t t = 1u;
			     t < tries && delay < AIUS_SD_OVERLAY_BACKOFF_MAX_MS; t++)
				delay *= 2u;
			if (delay > AIUS_SD_OVERLAY_BACKOFF_MAX_MS)
				delay = AIUS_SD_OVERLAY_BACKOFF_MAX_MS;
			next = now_ms + delay;
			(void)aius_web_cache_set_retry(cache, slot,
			                               next == 0u ? 1u : next, tries);
		}
		break;
	}
}

FLASHMEM void aius_sd_overlay_note_card_lost(AiusSdOverlay *overlay)
{
	if (overlay != NULL)
		overlay->state |= ST_HELD;
}

FLASHMEM void aius_sd_overlay_note_mounted(AiusSdOverlay *overlay,
                                           uint32_t now_ms)
{
	(void)now_ms;
	if (overlay != NULL)
		overlay->state &= (uint8_t)~ST_HELD;
}

FLASHMEM bool aius_sd_overlay_held(const AiusSdOverlay *overlay)
{
	return overlay != NULL && (overlay->state & ST_HELD) != 0u;
}

/* Q2: the running settings are authoritative, put on the file's own key,
 * never a faulted entry's path bytes. A put on the path keeps its safe copy
 * (FR2-S1). False when the serialiser is busy or the store full: retried
 * next step (D1, D6). */
static FLASHMEM bool reput(AiusSdOverlay *overlay,
                           AiusSdOverlaySettingsFile file, uint32_t now_ms)
{
	const AiusSdOverlayPlatform *p = overlay->platform;
	const char *key = key_of(file);
	const uint8_t *bytes = NULL;
	uint32_t length = 0u;
	AiusWebCachePutResult r;

	if (p->reput == NULL || !p->reput(p->context, file, &bytes, &length))
		return false;
	r = aius_web_cache_put(store(overlay), key, strlen(key), bytes, length,
	                       AIUS_WEB_CACHE_FLAG_SETTINGS, now_ms);
	note_lost(overlay, key, strlen(key), r);
	if (r != AIUS_WEB_CACHE_PUT_OK)
		return false;
	settle_owed(overlay, key, strlen(key), false);
	note_accept(overlay, AIUS_WEB_CACHE_FLAG_SETTINGS, now_ms);
	return true;
}

/* Handles faulted entry `slot`: a trusted dsp.json / config.json one is kept
 * for its re-put to replace; any other is dropped first, so a full manifest
 * frees one (CR2-N1), owing what it carried: the re-put of its settings file (both
 * when its own CRC failed, CR-N1) and, when its path reads as a settings key
 * and no other pending entry of that key carries one, its safe copy. A path
 * that failed its CRC yet still reads as "dsp.json" or "config.json" is
 * taken as that key for the safe copy alone: the alternative, the default
 * target-safe order, overwrites the backup first, which is the only valid
 * copy exactly when the bit was set (NS6). Returns the re-put bits owed. */
static FLASHMEM uint8_t drop_faulted(AiusSdOverlay *overlay, uint8_t slot,
                                     const AiusWebCacheEntryInfo *info)
{
	const AiusSdOverlayPlatform *p = overlay->platform;
	const size_t len = strlen(info->path);
	const bool untrusted =
		(info->flags & AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED) != 0u;
	AiusSdOverlaySettingsFile file;
	const bool keyed = info->state != AIUS_WEB_CACHE_STATE_TOMBSTONE &&
	                   settings_file(info->path, len, &file);

	/* A chunk fault on dsp.json or config.json (path trusted) keeps its
	 * entry: the re-put replaces it, in place when the manifest is full
	 * (CR3-B1), carrying its safe copy (NS6). */
	if (keyed && !untrusted)
		return reput_bit(file);
	if (!aius_web_cache_drop_pending(store(overlay), slot))
		return 0u;
	overlay->crc_drops++;
	if (p->event != NULL)
		p->event(p->context, AIUS_SD_OVERLAY_EVENT_CRC_DROP, info->path);
	if (keyed && !aius_sd_overlay_shadows(overlay, key_of(file),
	                                      strlen(key_of(file))))
		owe(overlay, file,
		    (info->flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) != 0u
		            ? AIUS_WEB_CACHE_SAFE_BACKUP
		            : AIUS_WEB_CACHE_SAFE_TARGET);
	if (info->state == AIUS_WEB_CACHE_STATE_TOMBSTONE)
		return 0u;
	if (untrusted)
		return ST_REPUT_DSP | ST_REPUT_CONFIG;
	return keyed ? reput_bit(file) : 0u;
}

FLASHMEM void aius_sd_overlay_service(AiusSdOverlay *overlay, uint32_t now_ms)
{
	AiusWebCache *cache = store(overlay);

	if (cache == NULL)
		return;
	/* A faulted entry's path never reaches a card operation. A tombstone
	 * is dropped (the card keeps the file, the delete is lost), never
	 * re-committed: re-stamping its CRC would make a flipped path a DELETE
	 * of a file never deleted. */
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const AiusWebCacheEntry *e = peek(cache, i);
		AiusWebCacheEntryInfo info;

		if ((e->state != AIUS_WEB_CACHE_STATE_DIRTY &&
		     e->state != AIUS_WEB_CACHE_STATE_TOMBSTONE) ||
		    (e->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) == 0u ||
		    (uint8_t)i == overlay->flushing_slot ||
		    !aius_web_cache_entry_info(cache, (uint8_t)i, &info))
			continue;
		overlay->state |= drop_faulted(overlay, (uint8_t)i, &info);
	}
	/* Q2 re-puts, each retried every step until it lands (D1, D6). */
	for (uint32_t f = 0; f < 2u; f++) {
		const AiusSdOverlaySettingsFile file =
			f == 0u ? AIUS_SD_OVERLAY_FILE_DSP : AIUS_SD_OVERLAY_FILE_CONFIG;

		if ((overlay->state & reput_bit(file)) != 0u &&
		    reput(overlay, file, now_ms))
			overlay->state &= (uint8_t)~reput_bit(file);
	}
}

FLASHMEM bool aius_sd_overlay_take_usage_refresh(AiusSdOverlay *overlay)
{
	if (overlay == NULL || (overlay->state & ST_REFRESH_DUE) == 0u)
		return false;
	overlay->state &= (uint8_t)~ST_REFRESH_DUE;
	return true;
}

FLASHMEM void aius_sd_overlay_status(const AiusSdOverlay *overlay,
                                     uint32_t now_ms,
                                     AiusSdOverlayStatus *status)
{
	const AiusWebCache *cache = store(overlay);
	bool have_oldest = false;
	uint32_t oldest = 0u;

	if (status == NULL)
		return;
	memset(status, 0, sizeof(*status));
	if (overlay == NULL)
		return;
	status->flushed = overlay->flushed;
	status->failures = overlay->failures;
	status->crc_drops = overlay->crc_drops;
	status->max_settings_latency_ms = overlay->max_settings_latency_ms;
	status->sync_drain_ms = overlay->sync_drain_ms;
	status->last_fault = overlay->last_fault;
	status->held = (overlay->state & ST_HELD) != 0u;
	status->drained = true;
	if (cache == NULL)
		return;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const AiusWebCacheEntry *e = peek(cache, i);

		switch (e->state) {
		case AIUS_WEB_CACHE_STATE_DIRTY:
			status->dirty++;
			if (!have_oldest ||
			    age(now_ms, e->accept_ms) > age(now_ms, oldest)) {
				oldest = e->accept_ms;
				have_oldest = true;
			}
			break;
		case AIUS_WEB_CACHE_STATE_FLUSHING:
			status->flushing++;
			break;
		case AIUS_WEB_CACHE_STATE_TOMBSTONE:
			status->tombstones++;
			if ((e->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) != 0u)
				status->flushing++;
			break;
		default:
			continue;
		}
		status->drained = false;
	}
	status->drained = status->drained && aius_sd_overlay_drained(overlay);
	if (have_oldest)
		status->oldest_dirty_age_ms = now_ms - oldest;
}

FLASHMEM void aius_sd_overlay_clear_stats(AiusSdOverlay *overlay)
{
	if (overlay == NULL)
		return;
	overlay->max_settings_latency_ms = 0u;
	overlay->sync_drain_ms = 0u;
}

FLASHMEM AiusSdOverlayRestoreDecision aius_sd_overlay_restore_decision(
	bool target_valid, AiusSdOverlayBackupResult backup)
{
	if (target_valid)
		return AIUS_SD_OVERLAY_RESTORE_KEEP;
	switch (backup) {
	case AIUS_SD_OVERLAY_BACKUP_PARSED:
		return AIUS_SD_OVERLAY_RESTORE_RESTORE;
	case AIUS_SD_OVERLAY_BACKUP_REFUSED:
		return AIUS_SD_OVERLAY_RESTORE_FAULT;
	case AIUS_SD_OVERLAY_BACKUP_LOAD_FAILED:
	case AIUS_SD_OVERLAY_BACKUP_NOT_READ:
	default:
		return AIUS_SD_OVERLAY_RESTORE_NOTHING;
	}
}
