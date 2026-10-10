#include "aius_web_cache.h"

#include <string.h>

/* Cold-ish serial runtime code -> FLASH, never ITCM: Phase 4g's ITCM budget is
 * 1,000 B and none of this is on the audio path. FLASHMEM is Teensyduino's
 * __attribute__((section(".flashmem"))) from <avr/pgmspace.h>; the host build
 * falls through to the no-op. It sits on each DEFINITION (GCC does not merge a
 * section attribute from a prototype). Enforced post-link by
 * test/itcm-placement/verify_target.py. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

/* The Teensy link script copies every .rodata into DTCM .data; naming
 * `.progmem` keeps the 64 B CRC table in flash (the aius_pipeline AC-3 table
 * shape). Target-only: the host has no such section. */
#if defined(__arm__)
#define AIUS_WEB_CACHE_FLASHRO __attribute__((section(".progmem")))
#else
#define AIUS_WEB_CACHE_FLASHRO
#endif

enum {
	ENTRY_FREE = 0,
	ENTRY_FILLING = 1,
	ENTRY_VALID = 2,
	ENTRY_STALE = 3,
};

#define PATH_STORAGE (AIUS_WEB_CACHE_PATH_MAX + 1u)

/* One manifest entry, resident in the carve. `entry_crc` covers `path` and
 * start..file_crc; `state` and `leases` are the mutable bookkeeping outside
 * it. `path` is zero-padded so the CRC is a function of the name alone. */
typedef struct {
	char path[PATH_STORAGE];
	uint8_t state;
	uint8_t leases;
	uint16_t start;
	uint16_t chunks;
	uint32_t length;
	uint32_t file_crc;
	uint32_t entry_crc;
} WebCacheEntry;

_Static_assert(sizeof(WebCacheEntry) == AIUS_WEB_CACHE_ENTRY_BYTES,
               "a manifest entry is one 64 B slot");
_Static_assert(sizeof(AiusWebCache) <= 64u,
               "G3: the DTCM control block stays under 64 B");
_Static_assert(AIUS_WEB_CACHE_ENTRIES < AIUS_WEB_CACHE_NO_ENTRY,
               "entry indices fit uint8_t beside the idle marker");

/* Reflected 0xEDB88320, one nibble at a time: 64 B of flash instead of 1 KiB. */
static const uint32_t k_crc_nibble[16] AIUS_WEB_CACHE_FLASHRO = {
	0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
	0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
	0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
	0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

FLASHMEM uint32_t aius_web_cache_crc32(uint32_t crc, const uint8_t *data,
                                       uint32_t bytes)
{
	uint32_t c = ~crc;

	if (data == NULL)
		return crc;
	for (uint32_t i = 0; i < bytes; i++) {
		c ^= data[i];
		c = (c >> 4) ^ k_crc_nibble[c & 15u];
		c = (c >> 4) ^ k_crc_nibble[c & 15u];
	}
	return ~c;
}

FLASHMEM bool aius_web_cache_place(uint32_t mapped_psram_bytes,
                                   uint32_t recorder_end_offset,
                                   AiusWebCachePlacement *placement)
{
	uint32_t offset;

	if (placement == NULL)
		return false;
	placement->offset = 0u;
	placement->capacity = 0u;
	/* Pad 2 must be core-mapped: 8 MiB boards have no pad 2, and the
	 * pad-2-only bench fitment maps nothing (OTA staging owns that chip). */
	if (mapped_psram_bytes <= AIUS_WEB_CACHE_PAD_BYTES)
		return false;
	offset = recorder_end_offset > AIUS_WEB_CACHE_PAD_BYTES
		? recorder_end_offset
		: AIUS_WEB_CACHE_PAD_BYTES;
	/* Cache-line and chunk alignment for the flush-delete ranges. */
	offset = (offset + 31u) & ~UINT32_C(31);
	if (offset > mapped_psram_bytes ||
	    mapped_psram_bytes - offset < AIUS_WEB_CACHE_CARVE_BYTES)
		return false;
	placement->offset = offset;
	placement->capacity = AIUS_WEB_CACHE_CARVE_BYTES;
	return true;
}

static FLASHMEM WebCacheEntry *entry_at(const AiusWebCache *cache, uint32_t i)
{
	return (WebCacheEntry *)(void *)(cache->base +
	                                 i * AIUS_WEB_CACHE_ENTRY_BYTES);
}

static FLASHMEM uint8_t *chunk_at(const AiusWebCache *cache, uint32_t chunk)
{
	return cache->base + cache->data_offset +
	       chunk * AIUS_WEB_CACHE_CHUNK_BYTES;
}

static FLASHMEM uint32_t table_get(const AiusWebCache *cache, uint32_t chunk)
{
	uint32_t v;

	memcpy(&v, cache->base + AIUS_WEB_CACHE_MANIFEST_BYTES + chunk * 4u,
	       sizeof(v));
	return v;
}

static FLASHMEM void table_set(AiusWebCache *cache, uint32_t chunk, uint32_t v)
{
	memcpy(cache->base + AIUS_WEB_CACHE_MANIFEST_BYTES + chunk * 4u, &v,
	       sizeof(v));
}

static FLASHMEM uint32_t chip_crc(const AiusWebCache *cache,
                                  const uint8_t *chip, uint32_t bytes)
{
	if (cache->chip_crc != NULL)
		return cache->chip_crc(0u, chip, bytes);
	return aius_web_cache_crc32(0u, chip, bytes);
}

static FLASHMEM uint32_t chunk_bytes(uint32_t length, uint32_t chunk)
{
	const uint32_t at = chunk * AIUS_WEB_CACHE_CHUNK_BYTES;
	const uint32_t left = length - at;

	return left < AIUS_WEB_CACHE_CHUNK_BYTES ? left
	                                         : AIUS_WEB_CACHE_CHUNK_BYTES;
}

static FLASHMEM uint32_t chunks_for(uint32_t length)
{
	return (length + AIUS_WEB_CACHE_CHUNK_BYTES - 1u) /
	       AIUS_WEB_CACHE_CHUNK_BYTES;
}

static FLASHMEM uint32_t entry_crc_of(const WebCacheEntry *e)
{
	uint32_t crc = aius_web_cache_crc32(0u, (const uint8_t *)e->path,
	                                    PATH_STORAGE);
	return aius_web_cache_crc32(crc, (const uint8_t *)&e->start,
	                            (uint32_t)(offsetof(WebCacheEntry, entry_crc) -
	                                       offsetof(WebCacheEntry, start)));
}

/* Not served any more: STALE while a lease pins it, FREE otherwise. */
static FLASHMEM void retire(WebCacheEntry *e)
{
	e->state = e->leases != 0u ? (uint8_t)ENTRY_STALE : (uint8_t)ENTRY_FREE;
}

static FLASHMEM bool path_ok(const char *path, size_t path_length)
{
	return path != NULL && path_length != 0u &&
	       path_length <= AIUS_WEB_CACHE_PATH_MAX &&
	       memchr(path, '\0', path_length) == NULL;
}

static FLASHMEM bool path_equals(const WebCacheEntry *e, const char *path,
                                 size_t path_length)
{
	return memcmp(e->path, path, path_length) == 0 &&
	       e->path[path_length] == '\0';
}

/* The entry as the CHIP holds it: lines dropped first, then its own CRC and
 * its geometry against the carve. A bad entry is dropped and counted. */
static FLASHMEM bool entry_sound(AiusWebCache *cache, WebCacheEntry *e)
{
	cache->flush_delete(e, AIUS_WEB_CACHE_ENTRY_BYTES);
	if (e->path[AIUS_WEB_CACHE_PATH_MAX] == '\0' &&
	    e->entry_crc == entry_crc_of(e) &&
	    e->chunks == chunks_for(e->length) &&
	    (uint32_t)e->start + e->chunks <= cache->data_chunks)
		return true;
	cache->crc_faults++;
	retire(e);
	return false;
}

FLASHMEM bool aius_web_cache_init(AiusWebCache *cache, uint8_t *base,
                                  uint32_t bytes,
                                  AiusWebCacheFlushDelete flush_delete,
                                  AiusWebCacheChipCrc chip_crc_hook)
{
	uint32_t n;
	uint32_t data_offset = 0u;

	if (cache == NULL)
		return false;
	memset(cache, 0, sizeof(*cache));
	cache->fill_entry = AIUS_WEB_CACHE_NO_ENTRY;
	if (base == NULL || flush_delete == NULL ||
	    bytes <= AIUS_WEB_CACHE_MANIFEST_BYTES)
		return false;
	n = (bytes - AIUS_WEB_CACHE_MANIFEST_BYTES) /
	    (AIUS_WEB_CACHE_CHUNK_BYTES + 4u);
	if (n > UINT16_MAX)
		n = UINT16_MAX;
	while (n != 0u) {
		data_offset = (AIUS_WEB_CACHE_MANIFEST_BYTES + n * 4u +
		               AIUS_WEB_CACHE_CHUNK_BYTES - 1u) &
		              ~(AIUS_WEB_CACHE_CHUNK_BYTES - 1u);
		if (data_offset + n * AIUS_WEB_CACHE_CHUNK_BYTES <= bytes)
			break;
		n--;
	}
	if (n == 0u)
		return false;
	memset(base, 0, AIUS_WEB_CACHE_MANIFEST_BYTES);
	cache->base = base;
	cache->flush_delete = flush_delete;
	cache->chip_crc = chip_crc_hook;
	cache->data_offset = data_offset;
	cache->data_chunks = (uint16_t)n;
	return true;
}

FLASHMEM bool aius_web_cache_lookup(AiusWebCache *cache, const char *path,
                                    size_t path_length, AiusWebCacheHit *hit,
                                    bool *fault)
{
	if (fault != NULL)
		*fault = false;
	if (cache == NULL || cache->base == NULL || hit == NULL ||
	    !path_ok(path, path_length))
		return false;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *e = entry_at(cache, i);

		if (e->state != ENTRY_VALID || !path_equals(e, path, path_length))
			continue;
		if (!entry_sound(cache, e)) {
			if (fault != NULL)
				*fault = true;
			return false;
		}
		hit->entry = (uint8_t)i;
		hit->chunks = e->chunks;
		hit->length = e->length;
		hit->file_crc = e->file_crc;
		return true;
	}
	return false;
}

FLASHMEM uint32_t aius_web_cache_entry_count(const AiusWebCache *cache)
{
	uint32_t count = 0u;

	if (cache == NULL || cache->base == NULL)
		return 0u;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++)
		if (entry_at(cache, i)->state == ENTRY_VALID)
			count++;
	return count;
}

/* An entry whose chunks may be read: VALID, or STALE under a lease. */
static FLASHMEM WebCacheEntry *readable(const AiusWebCache *cache,
                                        uint8_t entry)
{
	WebCacheEntry *e;

	if (cache == NULL || cache->base == NULL ||
	    entry >= AIUS_WEB_CACHE_ENTRIES)
		return NULL;
	e = entry_at(cache, entry);
	return e->state == ENTRY_VALID || e->state == ENTRY_STALE ? e : NULL;
}

FLASHMEM bool aius_web_cache_lease(AiusWebCache *cache, uint8_t entry)
{
	WebCacheEntry *e = readable(cache, entry);

	if (e == NULL || e->state != ENTRY_VALID || e->leases == UINT8_MAX)
		return false;
	e->leases++;
	return true;
}

FLASHMEM void aius_web_cache_release(AiusWebCache *cache, uint8_t entry)
{
	WebCacheEntry *e = readable(cache, entry);

	if (e == NULL || e->leases == 0u)
		return;
	e->leases--;
	if (e->leases == 0u && e->state == ENTRY_STALE)
		e->state = ENTRY_FREE;
}

FLASHMEM bool aius_web_cache_verify(AiusWebCache *cache, uint8_t entry,
                                    AiusWebCacheYield yield,
                                    void *yield_context)
{
	WebCacheEntry *e = readable(cache, entry);
	uint32_t start;
	uint32_t chunks;
	uint32_t length;

	if (e == NULL)
		return false;
	start = e->start;
	chunks = e->chunks;
	length = e->length;
	for (uint32_t i = 0; i < chunks; i++) {
		uint8_t *chip = chunk_at(cache, start + i);

		cache->flush_delete(chip, AIUS_WEB_CACHE_CHUNK_BYTES);
		if (chip_crc(cache, chip, chunk_bytes(length, i)) !=
		    table_get(cache, start + i)) {
			cache->crc_faults++;
			retire(e);
			return false;
		}
		if (yield != NULL &&
		    (i + 1u) % AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS == 0u &&
		    i + 1u < chunks)
			yield(yield_context);
	}
	return true;
}

FLASHMEM bool aius_web_cache_copy_chunk(AiusWebCache *cache, uint8_t entry,
                                        uint16_t chunk, uint8_t *dst,
                                        uint32_t *bytes)
{
	WebCacheEntry *e = readable(cache, entry);
	uint8_t *chip;
	uint32_t n;

	if (e == NULL || dst == NULL || bytes == NULL || chunk >= e->chunks)
		return false;
	n = chunk_bytes(e->length, chunk);
	chip = chunk_at(cache, (uint32_t)e->start + chunk);
	cache->flush_delete(chip, AIUS_WEB_CACHE_CHUNK_BYTES);
	memcpy(dst, chip, n);
	if (aius_web_cache_crc32(0u, dst, n) !=
	    table_get(cache, (uint32_t)e->start + chunk)) {
		cache->crc_faults++;
		retire(e);
		return false;
	}
	*bytes = n;
	return true;
}

FLASHMEM void aius_web_cache_fill_abort(AiusWebCache *cache)
{
	if (cache == NULL || cache->base == NULL ||
	    cache->fill_entry == AIUS_WEB_CACHE_NO_ENTRY)
		return;
	entry_at(cache, cache->fill_entry)->state = ENTRY_FREE;
	cache->fill_entry = AIUS_WEB_CACHE_NO_ENTRY;
	cache->fill_length = 0u;
	cache->fill_bytes = 0u;
	cache->fill_crc = 0u;
}

/* First fit over the live runs: bounded by ENTRIES restarts of an
 * ENTRIES-long scan, once per file. False when no run of `need` is free. */
static FLASHMEM bool find_run(const AiusWebCache *cache, uint32_t need,
                              uint32_t *start)
{
	uint32_t cand = 0u;
	bool moved = true;

	while (moved) {
		moved = false;
		if (cand + need > cache->data_chunks)
			return false;
		if (need == 0u)
			break;
		for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
			const WebCacheEntry *e = entry_at(cache, i);
			const uint32_t s = e->start;
			const uint32_t end = s + e->chunks;

			if (e->state == ENTRY_FREE || e->chunks == 0u)
				continue;
			if (s < cand + need && cand < end) {
				cand = end;
				moved = true;
				break;
			}
		}
	}
	*start = cand;
	return true;
}

FLASHMEM bool aius_web_cache_fill_begin(AiusWebCache *cache, const char *path,
                                        size_t path_length, uint32_t length)
{
	WebCacheEntry *e = NULL;
	uint32_t slot = 0u;
	uint32_t start = 0u;
	uint32_t need;

	if (cache == NULL || cache->base == NULL)
		return false;
	aius_web_cache_fill_abort(cache);
	if (!path_ok(path, path_length))
		return false;
	need = chunks_for(length);
	if (length > UINT32_MAX - AIUS_WEB_CACHE_CHUNK_BYTES ||
	    need > cache->data_chunks)
		return false;
	for (; slot < AIUS_WEB_CACHE_ENTRIES; slot++) {
		if (entry_at(cache, slot)->state == ENTRY_FREE) {
			e = entry_at(cache, slot);
			break;
		}
	}
	if (e == NULL || !find_run(cache, need, &start))
		return false;
	memset(e, 0, sizeof(*e));
	memcpy(e->path, path, path_length);
	e->start = (uint16_t)start;
	e->chunks = (uint16_t)need;
	e->length = length;
	e->state = ENTRY_FILLING;
	cache->fill_entry = (uint8_t)slot;
	cache->fill_length = length;
	cache->fill_bytes = 0u;
	cache->fill_crc = 0u;
	return true;
}

/* The in-flight fill when it belongs to `path`, else NULL. */
static FLASHMEM const WebCacheEntry *fill_of(const AiusWebCache *cache,
                                             const char *path,
                                             size_t path_length)
{
	const WebCacheEntry *e;

	if (cache == NULL || cache->base == NULL ||
	    cache->fill_entry == AIUS_WEB_CACHE_NO_ENTRY ||
	    !path_ok(path, path_length))
		return NULL;
	e = entry_at(cache, cache->fill_entry);
	return e->state == ENTRY_FILLING && path_equals(e, path, path_length)
		? e
		: NULL;
}

FLASHMEM uint32_t aius_web_cache_fill_resume_offset(const AiusWebCache *cache,
                                                    const char *path,
                                                    size_t path_length)
{
	return fill_of(cache, path, path_length) != NULL ? cache->fill_bytes
	                                                 : 0u;
}

FLASHMEM bool aius_web_cache_fill_resume(AiusWebCache *cache, const char *path,
                                         size_t path_length, uint32_t offset,
                                         uint32_t length)
{
	if (fill_of(cache, path, path_length) != NULL &&
	    cache->fill_bytes == offset && cache->fill_length == length)
		return true;
	aius_web_cache_fill_abort(cache);
	return false;
}

FLASHMEM bool aius_web_cache_fill_chunk(AiusWebCache *cache,
                                        const uint8_t *card, uint32_t bytes)
{
	const WebCacheEntry *e;
	uint32_t chunk;
	uint32_t card_crc;
	uint8_t *chip;

	if (cache == NULL || cache->base == NULL ||
	    cache->fill_entry == AIUS_WEB_CACHE_NO_ENTRY)
		return false;
	if (card == NULL || cache->fill_bytes >= cache->fill_length ||
	    bytes != chunk_bytes(cache->fill_length,
	                         cache->fill_bytes / AIUS_WEB_CACHE_CHUNK_BYTES)) {
		aius_web_cache_fill_abort(cache);
		return false;
	}
	e = entry_at(cache, cache->fill_entry);
	chunk = (uint32_t)e->start + cache->fill_bytes / AIUS_WEB_CACHE_CHUNK_BYTES;
	chip = chunk_at(cache, chunk);
	card_crc = aius_web_cache_crc32(0u, card, bytes);
	memcpy(chip, card, bytes);
	/* G7: the write-allocated lines go to the chip and are dropped, so the
	 * read-back below is the chip's, not the cache's. */
	cache->flush_delete(chip, AIUS_WEB_CACHE_CHUNK_BYTES);
	if (chip_crc(cache, chip, bytes) != card_crc) {
		cache->crc_faults++;
		aius_web_cache_fill_abort(cache);
		return false;
	}
	table_set(cache, chunk, card_crc);
	cache->fill_crc = aius_web_cache_crc32(cache->fill_crc, card, bytes);
	cache->fill_bytes += bytes;
	return true;
}

FLASHMEM bool aius_web_cache_publish(AiusWebCache *cache)
{
	WebCacheEntry staged;
	WebCacheEntry *e;
	uint8_t slot;

	if (cache == NULL || cache->base == NULL ||
	    cache->fill_entry == AIUS_WEB_CACHE_NO_ENTRY)
		return false;
	/* G10: fill_bytes only advances past a chunk whose read-back matched,
	 * in order, so equality with the length is "every chunk checked". */
	if (cache->fill_bytes != cache->fill_length) {
		aius_web_cache_fill_abort(cache);
		return false;
	}
	slot = cache->fill_entry;
	e = entry_at(cache, slot);
	memcpy(&staged, e, sizeof(staged));
	staged.file_crc = cache->fill_crc;
	staged.leases = 0u;
	staged.entry_crc = entry_crc_of(&staged);
	staged.state = ENTRY_VALID;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *old = entry_at(cache, i);

		if (i != slot && old->state == ENTRY_VALID &&
		    path_equals(old, staged.path, strlen(staged.path)))
			retire(old);
	}
	memcpy(e, &staged, sizeof(staged));
	cache->fill_entry = AIUS_WEB_CACHE_NO_ENTRY;
	cache->fill_length = 0u;
	cache->fill_bytes = 0u;
	cache->fill_crc = 0u;
	return true;
}

FLASHMEM void aius_web_cache_invalidate(AiusWebCache *cache, const char *path,
                                        size_t path_length)
{
	const bool all = path == NULL;
	const bool prefix = !all && path_length != 0u &&
	                    path[path_length - 1u] == '/';

	if (cache == NULL || cache->base == NULL || (!all && path_length == 0u))
		return;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *e = entry_at(cache, i);
		bool match;

		if (e->state != ENTRY_VALID && e->state != ENTRY_FILLING)
			continue;
		if (all)
			match = true;
		else if (prefix)
			match = path_length <= AIUS_WEB_CACHE_PATH_MAX &&
			        memcmp(e->path, path, path_length) == 0;
		else
			match = path_length <= AIUS_WEB_CACHE_PATH_MAX &&
			        path_equals(e, path, path_length);
		if (!match)
			continue;
		if (e->state == ENTRY_FILLING)
			aius_web_cache_fill_abort(cache);
		else
			retire(e);
	}
}

/* Length of the first path segment of `rest`, and whether it is a
 * directory (a '/' follows it). */
static FLASHMEM size_t segment(const char *rest, bool *is_directory)
{
	const char *slash = strchr(rest, '/');

	*is_directory = slash != NULL;
	return slash != NULL ? (size_t)(slash - rest) : strlen(rest);
}

FLASHMEM size_t aius_web_cache_listing(AiusWebCache *cache, const char *dir,
                                       size_t dir_length,
                                       AiusWebCacheListingRow row,
                                       void *context)
{
	size_t rows = 0u;

	if (cache == NULL || cache->base == NULL || row == NULL ||
	    (dir == NULL && dir_length != 0u) ||
	    dir_length > AIUS_WEB_CACHE_PATH_MAX)
		return 0u;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *e = entry_at(cache, i);
		const char *rest;
		size_t name_length;
		bool is_directory;
		bool seen = false;

		if (e->state != ENTRY_VALID ||
		    (dir_length != 0u && memcmp(e->path, dir, dir_length) != 0) ||
		    e->path[dir_length] == '\0' || !entry_sound(cache, e))
			continue;
		rest = e->path + dir_length;
		name_length = segment(rest, &is_directory);
		if (name_length == 0u)
			continue;
		/* A subdirectory is one row however many files sit under it. */
		for (uint32_t j = 0; is_directory && j < i && !seen; j++) {
			const WebCacheEntry *p = entry_at(cache, j);

			seen = p->state == ENTRY_VALID &&
			       memcmp(p->path, e->path, dir_length + name_length + 1u) ==
			               0;
		}
		if (seen)
			continue;
		rows++;
		if (!row(context, rest, name_length,
		         is_directory ? 0u : e->length, is_directory))
			break;
	}
	return rows;
}

FLASHMEM size_t aius_web_cache_etag(const AiusWebCacheHit *hit, char *out,
                                    size_t capacity)
{
	static const char hex[] AIUS_WEB_CACHE_FLASHRO = "0123456789abcdef";
	char digits[10];
	size_t n = 0u;
	size_t nd = 0u;
	uint32_t v;

	if (hit == NULL || out == NULL)
		return 0u;
	v = hit->length;
	do {
		digits[nd++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	if (capacity < 1u + 8u + 1u + nd + 1u + 1u)
		return 0u;
	out[n++] = '"';
	for (int shift = 28; shift >= 0; shift -= 4)
		out[n++] = hex[(hit->file_crc >> shift) & 15u];
	out[n++] = '-';
	while (nd != 0u)
		out[n++] = digits[--nd];
	out[n++] = '"';
	out[n] = '\0';
	return n;
}
