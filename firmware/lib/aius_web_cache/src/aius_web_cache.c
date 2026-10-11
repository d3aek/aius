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
	ENTRY_FREE = AIUS_WEB_CACHE_STATE_FREE,
	ENTRY_FILLING = AIUS_WEB_CACHE_STATE_FILLING,
	ENTRY_VALID = AIUS_WEB_CACHE_STATE_VALID,
	ENTRY_STALE = AIUS_WEB_CACHE_STATE_STALE,
	ENTRY_DIRTY = AIUS_WEB_CACHE_STATE_DIRTY,
	ENTRY_FLUSHING = AIUS_WEB_CACHE_STATE_FLUSHING,
	ENTRY_TOMBSTONE = AIUS_WEB_CACHE_STATE_TOMBSTONE,
	ENTRY_PUTTING = AIUS_WEB_CACHE_STATE_PUTTING,
};

#define PATH_STORAGE AIUS_WEB_CACHE_PATH_STORAGE
#define NO_ENTRY AIUS_WEB_CACHE_NO_ENTRY

/* One manifest entry, resident in the carve: the public 80 B layout, whose
 * size and field order the header static-asserts (4h N2). */
typedef AiusWebCacheEntry WebCacheEntry;

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

/* Covers path, start..file_crc and commit_seq (fixed once committed, 4h N3),
 * so a bit flip in commit_seq fails the entry rather than reordering it. */
static FLASHMEM uint32_t entry_crc_of(const WebCacheEntry *e)
{
	uint32_t crc = aius_web_cache_crc32(0u, (const uint8_t *)e->path,
	                                    PATH_STORAGE);
	crc = aius_web_cache_crc32(crc, (const uint8_t *)&e->start,
	                           (uint32_t)(offsetof(WebCacheEntry, entry_crc) -
	                                      offsetof(WebCacheEntry, start)));
	return aius_web_cache_crc32(crc, (const uint8_t *)&e->commit_seq,
	                            (uint32_t)sizeof(e->commit_seq));
}

static FLASHMEM bool is_pending(uint8_t state)
{
	return state == ENTRY_DIRTY || state == ENTRY_FLUSHING ||
	       state == ENTRY_TOMBSTONE;
}

/* A candidate for a path's newest: committed and not retired. */
static FLASHMEM bool is_live(uint8_t state)
{
	return state == ENTRY_VALID || is_pending(state);
}

/* Not served any more: STALE while a lease pins it, FREE otherwise. */
static FLASHMEM void retire(WebCacheEntry *e)
{
	e->state = e->leases != 0u ? (uint8_t)ENTRY_STALE : (uint8_t)ENTRY_FREE;
	e->flags = 0u;
}

static FLASHMEM bool path_ok(const char *path, size_t path_length)
{
	return path != NULL && path_length != 0u &&
	       path_length <= AIUS_WEB_CACHE_PATH_MAX &&
	       memchr(path, '\0', path_length) == NULL;
}

static FLASHMEM char fold(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* ASCII case-folded compare of n bytes (N1: FAT and exFAT names fold). */
static FLASHMEM bool fold_equal(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (fold(a[i]) != fold(b[i]))
			return false;
	return true;
}

static FLASHMEM bool path_equals(const WebCacheEntry *e, const char *path,
                                 size_t path_length)
{
	return path_length <= AIUS_WEB_CACHE_PATH_MAX &&
	       fold_equal(e->path, path, path_length) &&
	       e->path[path_length] == '\0';
}

/* Serial-number compare: a newer than b (wraps past 2^32 order correctly). */
static FLASHMEM bool seq_newer(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

/* The entry as the CHIP holds it: lines dropped first, then its own CRC and
 * its geometry against the carve. A bad clean entry is dropped; a bad pending
 * one is kept and flagged CRC_FAULTED for the overlay (Q2). Counted. */
static FLASHMEM bool entry_sound(AiusWebCache *cache, WebCacheEntry *e)
{
	cache->flush_delete(e, AIUS_WEB_CACHE_ENTRY_BYTES);
	if (e->path[AIUS_WEB_CACHE_PATH_MAX] == '\0' &&
	    e->entry_crc == entry_crc_of(e) &&
	    e->chunks == chunks_for(e->length) &&
	    (uint32_t)e->start + e->chunks <= cache->data_chunks)
		return true;
	cache->crc_faults++;
	if (is_pending(e->state))
		e->flags |= AIUS_WEB_CACHE_FLAG_CRC_FAULTED |
		            AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED;
	else
		retire(e);
	return false;
}

/* A chunk fault on entry `e`: drop a clean one, flag a pending one. */
static FLASHMEM void chunk_fault(AiusWebCache *cache, WebCacheEntry *e)
{
	cache->crc_faults++;
	if (is_pending(e->state))
		e->flags |= AIUS_WEB_CACHE_FLAG_CRC_FAULTED;
	else
		retire(e);
}

/* The path's newest live entry (highest commit_seq), else NO_ENTRY. */
static FLASHMEM uint8_t newest_of(const AiusWebCache *cache, const char *path,
                                  size_t path_length)
{
	uint8_t best = NO_ENTRY;
	uint32_t best_seq = 0u;

	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const WebCacheEntry *e = entry_at(cache, i);

		if (!is_live(e->state) || !path_equals(e, path, path_length))
			continue;
		if (best == NO_ENTRY || seq_newer(e->commit_seq, best_seq)) {
			best = (uint8_t)i;
			best_seq = e->commit_seq;
		}
	}
	return best;
}

/* The path's newest pending entry (DIRTY, FLUSHING, TOMBSTONE). */
static FLASHMEM WebCacheEntry *newest_pending(const AiusWebCache *cache,
                                              const char *path,
                                              size_t path_length)
{
	WebCacheEntry *best = NULL;

	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *e = entry_at(cache, i);

		if (!is_pending(e->state) || !path_equals(e, path, path_length))
			continue;
		if (best == NULL || seq_newer(e->commit_seq, best->commit_seq))
			best = e;
	}
	return best;
}

/* Pending in the fill's sense: DIRTY, FLUSHING, TOMBSTONE or PUTTING (D2). */
static FLASHMEM bool path_pending_any(const AiusWebCache *cache,
                                      const char *path, size_t path_length)
{
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const WebCacheEntry *e = entry_at(cache, i);

		if ((is_pending(e->state) || e->state == ENTRY_PUTTING) &&
		    path_equals(e, path, path_length))
			return true;
	}
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
	cache->fill_entry = NO_ENTRY;
	cache->put_entry = NO_ENTRY;
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
	uint8_t best = NO_ENTRY;
	uint32_t best_seq = 0u;
	WebCacheEntry *e;

	if (fault != NULL)
		*fault = false;
	if (cache == NULL || cache->base == NULL || hit == NULL ||
	    !path_ok(path, path_length))
		return false;
	/* Every candidate's CRC is checked, so a flipped commit_seq can never
	 * promote an older version (N3). */
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *c = entry_at(cache, i);

		if (!is_live(c->state) || !path_equals(c, path, path_length))
			continue;
		if (!entry_sound(cache, c)) {
			if (fault != NULL)
				*fault = true;
			if (is_pending(c->state))
				return false; /* an unreadable pending version */
			continue;
		}
		if (best == NO_ENTRY || seq_newer(c->commit_seq, best_seq)) {
			best = (uint8_t)i;
			best_seq = c->commit_seq;
		}
	}
	if (best == NO_ENTRY)
		return false;
	e = entry_at(cache, best);
	if (e->state == ENTRY_TOMBSTONE ||
	    (e->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u)
		return false;
	hit->entry = best;
	hit->chunks = e->chunks;
	hit->length = e->length;
	hit->file_crc = e->file_crc;
	return true;
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

/* An entry whose chunks may be read: VALID, DIRTY, FLUSHING, or STALE under
 * a lease. */
static FLASHMEM WebCacheEntry *readable(const AiusWebCache *cache,
                                        uint8_t entry)
{
	WebCacheEntry *e;

	if (cache == NULL || cache->base == NULL ||
	    entry >= AIUS_WEB_CACHE_ENTRIES)
		return NULL;
	e = entry_at(cache, entry);
	return e->state == ENTRY_VALID || e->state == ENTRY_STALE ||
	               e->state == ENTRY_DIRTY || e->state == ENTRY_FLUSHING
		? e
		: NULL;
}

FLASHMEM bool aius_web_cache_lease(AiusWebCache *cache, uint8_t entry)
{
	WebCacheEntry *e = readable(cache, entry);

	if (e == NULL || e->state == ENTRY_STALE || e->leases == UINT8_MAX)
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
			chunk_fault(cache, e);
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

	if (e == NULL || dst == NULL || bytes == NULL)
		return false;
	/* The entry's own CRC first: a flipped start or length would read
	 * another file's chunks, whose table CRCs match (CR-S1). A failing
	 * pending entry is flagged; a flagged one never serves. */
	if ((e->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u ||
	    !entry_sound(cache, e) || chunk >= e->chunks)
		return false;
	n = chunk_bytes(e->length, chunk);
	chip = chunk_at(cache, (uint32_t)e->start + chunk);
	cache->flush_delete(chip, AIUS_WEB_CACHE_CHUNK_BYTES);
	memcpy(dst, chip, n);
	if (aius_web_cache_crc32(0u, dst, n) !=
	    table_get(cache, (uint32_t)e->start + chunk)) {
		chunk_fault(cache, e);
		return false;
	}
	*bytes = n;
	return true;
}

FLASHMEM void aius_web_cache_fill_abort(AiusWebCache *cache)
{
	if (cache == NULL || cache->base == NULL ||
	    cache->fill_entry == NO_ENTRY)
		return;
	entry_at(cache, cache->fill_entry)->state = ENTRY_FREE;
	cache->fill_entry = NO_ENTRY;
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

static FLASHMEM uint8_t free_slot(const AiusWebCache *cache)
{
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++)
		if (entry_at(cache, i)->state == ENTRY_FREE)
			return (uint8_t)i;
	return NO_ENTRY;
}

FLASHMEM bool aius_web_cache_fill_begin(AiusWebCache *cache, const char *path,
                                        size_t path_length, uint32_t length)
{
	WebCacheEntry *e;
	uint8_t slot;
	uint32_t start = 0u;
	uint32_t need;

	if (cache == NULL || cache->base == NULL)
		return false;
	aius_web_cache_fill_abort(cache);
	if (!path_ok(path, path_length) ||
	    path_pending_any(cache, path, path_length))
		return false;
	need = chunks_for(length);
	if (length > UINT32_MAX - AIUS_WEB_CACHE_CHUNK_BYTES ||
	    need > cache->data_chunks)
		return false;
	slot = free_slot(cache);
	if (slot == NO_ENTRY || !find_run(cache, need, &start))
		return false;
	e = entry_at(cache, slot);
	memset(e, 0, sizeof(*e));
	memcpy(e->path, path, path_length);
	e->start = (uint16_t)start;
	e->chunks = (uint16_t)need;
	e->length = length;
	e->state = ENTRY_FILLING;
	cache->fill_entry = slot;
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
	    cache->fill_entry == NO_ENTRY || !path_ok(path, path_length))
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
	    !path_pending_any(cache, path, path_length) &&
	    cache->fill_bytes == offset && cache->fill_length == length)
		return true;
	aius_web_cache_fill_abort(cache);
	return false;
}

/* Copies `bytes` into chunk `chunk`, flush-deletes it and checks the chip's
 * read-back against the source (G7, G10); records the chunk CRC. */
static FLASHMEM bool write_chunk(AiusWebCache *cache, uint32_t chunk,
                                 const uint8_t *src, uint32_t bytes,
                                 uint32_t *crc_out)
{
	uint8_t *chip = chunk_at(cache, chunk);
	const uint32_t src_crc = aius_web_cache_crc32(0u, src, bytes);

	memcpy(chip, src, bytes);
	/* G7: the write-allocated lines go to the chip and are dropped, so the
	 * read-back below is the chip's, not the cache's. */
	cache->flush_delete(chip, AIUS_WEB_CACHE_CHUNK_BYTES);
	if (chip_crc(cache, chip, bytes) != src_crc) {
		cache->crc_faults++;
		return false;
	}
	table_set(cache, chunk, src_crc);
	*crc_out = src_crc;
	return true;
}

FLASHMEM bool aius_web_cache_fill_chunk(AiusWebCache *cache,
                                        const uint8_t *card, uint32_t bytes)
{
	const WebCacheEntry *e;
	uint32_t chunk;
	uint32_t c;

	if (cache == NULL || cache->base == NULL ||
	    cache->fill_entry == NO_ENTRY)
		return false;
	if (card == NULL || cache->fill_bytes >= cache->fill_length ||
	    bytes != chunk_bytes(cache->fill_length,
	                         cache->fill_bytes / AIUS_WEB_CACHE_CHUNK_BYTES)) {
		aius_web_cache_fill_abort(cache);
		return false;
	}
	e = entry_at(cache, cache->fill_entry);
	chunk = (uint32_t)e->start + cache->fill_bytes / AIUS_WEB_CACHE_CHUNK_BYTES;
	if (!write_chunk(cache, chunk, card, bytes, &c)) {
		aius_web_cache_fill_abort(cache);
		return false;
	}
	cache->fill_crc = aius_web_cache_crc32(cache->fill_crc, card, bytes);
	cache->fill_bytes += bytes;
	return true;
}

FLASHMEM bool aius_web_cache_publish(AiusWebCache *cache)
{
	WebCacheEntry staged;
	WebCacheEntry *e;
	uint8_t slot;
	size_t len;

	if (cache == NULL || cache->base == NULL ||
	    cache->fill_entry == NO_ENTRY)
		return false;
	slot = cache->fill_entry;
	e = entry_at(cache, slot);
	len = strlen(e->path);
	/* G10: fill_bytes only advances past a chunk whose read-back matched,
	 * in order, so equality with the length is "every chunk checked". A
	 * card file never replaces a pending entry (H11). */
	if (cache->fill_bytes != cache->fill_length ||
	    path_pending_any(cache, e->path, len)) {
		aius_web_cache_fill_abort(cache);
		return false;
	}
	memcpy(&staged, e, sizeof(staged));
	staged.file_crc = cache->fill_crc;
	staged.leases = 0u;
	staged.flags = 0u;
	staged.commit_seq = ++cache->commit_seq;
	staged.entry_crc = entry_crc_of(&staged);
	staged.state = ENTRY_VALID;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *old = entry_at(cache, i);

		if (i != slot && old->state == ENTRY_VALID &&
		    path_equals(old, staged.path, len))
			retire(old);
	}
	memcpy(e, &staged, sizeof(staged));
	cache->fill_entry = NO_ENTRY;
	cache->fill_length = 0u;
	cache->fill_bytes = 0u;
	cache->fill_crc = 0u;
	return true;
}

static FLASHMEM bool prefix_equals(const WebCacheEntry *e, const char *prefix,
                                   size_t prefix_length)
{
	return prefix_length <= AIUS_WEB_CACHE_PATH_MAX &&
	       fold_equal(e->path, prefix, prefix_length) &&
	       memchr(e->path, '\0', prefix_length) == NULL;
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

		/* Clean entries only: pending and PUTTING are never touched
		 * (H11, NS5). */
		if (e->state != ENTRY_VALID && e->state != ENTRY_FILLING)
			continue;
		if (all)
			match = true;
		else if (prefix)
			match = prefix_equals(e, path, path_length);
		else
			match = path_equals(e, path, path_length);
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

/* Bit i set when entry i is its path's newest, sound and listed: VALID,
 * DIRTY or FLUSHING (TOMBSTONE omitted, S6); `pending_only` keeps DIRTY and
 * FLUSHING alone. */
static FLASHMEM uint64_t listable(AiusWebCache *cache, const char *dir,
                                  size_t dir_length, bool pending_only)
{
	uint64_t mask = 0u;

	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *e = entry_at(cache, i);
		const uint8_t s = e->state;

		if (s != ENTRY_DIRTY && s != ENTRY_FLUSHING &&
		    (pending_only || s != ENTRY_VALID))
			continue;
		if ((dir_length != 0u && !prefix_equals(e, dir, dir_length)) ||
		    e->path[dir_length] == '\0')
			continue;
		if (newest_of(cache, e->path, strlen(e->path)) != i)
			continue;
		if ((e->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u ||
		    !entry_sound(cache, e))
			continue;
		mask |= UINT64_C(1) << i;
	}
	return mask;
}

static FLASHMEM size_t listing_rows(AiusWebCache *cache, size_t dir_length,
                                    uint64_t mask, bool files_only,
                                    AiusWebCacheListingRow row, void *context)
{
	size_t rows = 0u;

	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const WebCacheEntry *e = entry_at(cache, i);
		const char *rest;
		size_t name_length;
		bool is_directory;
		bool seen = false;

		if ((mask & (UINT64_C(1) << i)) == 0u)
			continue;
		rest = e->path + dir_length;
		name_length = segment(rest, &is_directory);
		if (name_length == 0u || (files_only && is_directory))
			continue;
		/* A subdirectory is one row however many files sit under it. */
		for (uint32_t j = 0; is_directory && j < i && !seen; j++) {
			const WebCacheEntry *p = entry_at(cache, j);

			seen = (mask & (UINT64_C(1) << j)) != 0u &&
			       fold_equal(p->path, e->path,
			                  dir_length + name_length + 1u);
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

FLASHMEM size_t aius_web_cache_listing(AiusWebCache *cache, const char *dir,
                                       size_t dir_length,
                                       AiusWebCacheListingRow row,
                                       void *context)
{
	if (cache == NULL || cache->base == NULL || row == NULL ||
	    (dir == NULL && dir_length != 0u) ||
	    dir_length > AIUS_WEB_CACHE_PATH_MAX)
		return 0u;
	return listing_rows(cache, dir_length,
	                    listable(cache, dir, dir_length, false), false, row,
	                    context);
}

FLASHMEM size_t aius_web_cache_pending_listing(AiusWebCache *cache,
                                               const char *dir,
                                               size_t dir_length,
                                               AiusWebCacheListingRow row,
                                               void *context)
{
	if (cache == NULL || cache->base == NULL || row == NULL ||
	    (dir == NULL && dir_length != 0u) ||
	    dir_length > AIUS_WEB_CACHE_PATH_MAX)
		return 0u;
	return listing_rows(cache, dir_length,
	                    listable(cache, dir, dir_length, true), true, row,
	                    context);
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

/* ---- 4h write-behind ---- */

/* Reserves a FREE entry as PUTTING with a run for `length` bytes. */
static FLASHMEM AiusWebCachePutResult reserve(AiusWebCache *cache,
                                              const char *path,
                                              size_t path_length,
                                              uint32_t length, uint8_t flags,
                                              uint8_t *slot_out)
{
	WebCacheEntry *e;
	uint8_t slot;
	uint32_t start = 0u;
	uint32_t need;

	if (cache == NULL || cache->base == NULL || !path_ok(path, path_length) ||
	    length > UINT32_MAX - AIUS_WEB_CACHE_CHUNK_BYTES)
		return AIUS_WEB_CACHE_PUT_INVALID;
	need = chunks_for(length);
	if (need > cache->data_chunks)
		return AIUS_WEB_CACHE_PUT_FULL;
	slot = free_slot(cache);
	if (slot == NO_ENTRY || !find_run(cache, need, &start))
		return AIUS_WEB_CACHE_PUT_FULL;
	e = entry_at(cache, slot);
	memset(e, 0, sizeof(*e));
	memcpy(e->path, path, path_length);
	e->start = (uint16_t)start;
	e->chunks = (uint16_t)need;
	e->length = length;
	e->flags = (uint8_t)(flags & AIUS_WEB_CACHE_PUT_FLAGS);
	e->state = ENTRY_PUTTING;
	*slot_out = slot;
	return AIUS_WEB_CACHE_PUT_OK;
}

/* The one-shot _put with no free entry or run (CR3-B1): reserves the path's
 * own newest entry in place when it is not pinned (unleased, not FLUSHING,
 * not FILLING, not a running DELETE), as _tombstone reuses one, its chunk
 * run counted free. So a settings put (dsp.json, config.json, the restore,
 * the Q2 re-put) always lands once its own entry is unpinned, whatever the
 * manifest holds. A chunk fault while writing frees the entry, and with it
 * a pending version it held: _put reports that as FAULT_LOST(_BACKUP).
 * `was_pending`: the reused entry was pending; `was_backup`: with the
 * safe-copy bit. */
static FLASHMEM AiusWebCachePutResult reserve_in_place(AiusWebCache *cache,
                                                       const char *path,
                                                       size_t path_length,
                                                       uint32_t length,
                                                       uint8_t flags,
                                                       uint8_t *slot_out,
                                                       bool *was_pending,
                                                       bool *was_backup)
{
	uint8_t slot = newest_of(cache, path, path_length);
	WebCacheEntry *e;
	uint8_t state;
	uint32_t start = 0u;
	uint32_t need = chunks_for(length);

	if (slot == NO_ENTRY)
		return AIUS_WEB_CACHE_PUT_FULL;
	e = entry_at(cache, slot);
	if (e->leases != 0u || e->state == ENTRY_FLUSHING ||
	    (e->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) != 0u)
		return AIUS_WEB_CACHE_PUT_FULL;
	state = e->state;
	e->state = ENTRY_FREE; /* its run counts free for find_run */
	if (!find_run(cache, need, &start)) {
		e->state = state;
		return AIUS_WEB_CACHE_PUT_FULL;
	}
	*was_pending = is_pending(state);
	*was_backup = *was_pending &&
	              (e->flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) != 0u;
	memset(e, 0, sizeof(*e));
	memcpy(e->path, path, path_length);
	e->start = (uint16_t)start;
	e->chunks = (uint16_t)need;
	e->length = length;
	e->flags = (uint8_t)(flags & AIUS_WEB_CACHE_PUT_FLAGS);
	e->state = ENTRY_PUTTING;
	*slot_out = slot;
	return AIUS_WEB_CACHE_PUT_OK;
}

/* Commits entry `slot` as `state` (DIRTY or TOMBSTONE): takes the safe-copy
 * bit from the path's newest pending entry, retires the path's VALID, DIRTY
 * and non-running TOMBSTONE entries and its FILLING partial, leaves FLUSHING
 * and a running TOMBSTONE pinned (FR-S4, G1, F1). */
static FLASHMEM void commit_entry(AiusWebCache *cache, uint8_t slot,
                                  uint8_t state, uint32_t file_crc,
                                  uint32_t now_ms)
{
	WebCacheEntry *e = entry_at(cache, slot);
	const size_t len = strlen(e->path);
	WebCacheEntry *prev = newest_pending(cache, e->path, len);
	bool backup;

	if (prev != NULL)
		backup = (prev->flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) != 0u;
	else
		backup = (e->flags & AIUS_WEB_CACHE_FLAG_RESTORE_CREATED) != 0u;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		WebCacheEntry *o = entry_at(cache, i);

		if (i == slot || !path_equals(o, e->path, len))
			continue;
		if (is_pending(o->state))
			o->flags &= (uint8_t)~AIUS_WEB_CACHE_FLAG_SAFE_BACKUP;
		if (o->state == ENTRY_VALID || o->state == ENTRY_DIRTY ||
		    (o->state == ENTRY_TOMBSTONE &&
		     (o->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) == 0u))
			retire(o);
		else if (o->state == ENTRY_FILLING && cache->fill_entry == i)
			aius_web_cache_fill_abort(cache);
	}
	e->flags &= (uint8_t)AIUS_WEB_CACHE_PUT_FLAGS;
	if (backup)
		e->flags |= AIUS_WEB_CACHE_FLAG_SAFE_BACKUP;
	e->file_crc = file_crc;
	e->leases = 0u;
	e->accept_ms = now_ms;
	e->next_try_ms = 0u;
	e->tries = 0u;
	e->commit_seq = ++cache->commit_seq;
	e->entry_crc = entry_crc_of(e);
	e->state = state;
}

FLASHMEM AiusWebCachePutResult aius_web_cache_put_begin(AiusWebCache *cache,
                                                        const char *path,
                                                        size_t path_length,
                                                        uint32_t length,
                                                        uint8_t flags)
{
	AiusWebCachePutResult r;
	uint8_t slot = NO_ENTRY;

	if (cache == NULL || cache->base == NULL || cache->put_entry != NO_ENTRY)
		return AIUS_WEB_CACHE_PUT_INVALID;
	r = reserve(cache, path, path_length, length, flags, &slot);
	if (r != AIUS_WEB_CACHE_PUT_OK)
		return r;
	cache->put_entry = slot;
	cache->put_length = length;
	cache->put_bytes = 0u;
	cache->put_crc = 0u;
	return AIUS_WEB_CACHE_PUT_OK;
}

FLASHMEM void aius_web_cache_put_abort(AiusWebCache *cache)
{
	if (cache == NULL || cache->base == NULL ||
	    cache->put_entry == NO_ENTRY)
		return;
	entry_at(cache, cache->put_entry)->state = ENTRY_FREE;
	cache->put_entry = NO_ENTRY;
	cache->put_length = 0u;
	cache->put_bytes = 0u;
	cache->put_crc = 0u;
}

FLASHMEM bool aius_web_cache_put_active(const AiusWebCache *cache)
{
	return cache != NULL && cache->base != NULL &&
	       cache->put_entry != NO_ENTRY;
}

FLASHMEM AiusWebCachePutResult aius_web_cache_put_chunk(AiusWebCache *cache,
                                                        const uint8_t *bytes,
                                                        uint32_t length)
{
	const WebCacheEntry *e;
	uint32_t c;

	if (cache == NULL || cache->base == NULL ||
	    cache->put_entry == NO_ENTRY)
		return AIUS_WEB_CACHE_PUT_INVALID;
	if (bytes == NULL || cache->put_bytes >= cache->put_length ||
	    length != chunk_bytes(cache->put_length,
	                          cache->put_bytes / AIUS_WEB_CACHE_CHUNK_BYTES)) {
		aius_web_cache_put_abort(cache);
		return AIUS_WEB_CACHE_PUT_INVALID;
	}
	e = entry_at(cache, cache->put_entry);
	if (!write_chunk(cache,
	                 (uint32_t)e->start +
	                         cache->put_bytes / AIUS_WEB_CACHE_CHUNK_BYTES,
	                 bytes, length, &c)) {
		aius_web_cache_put_abort(cache);
		return AIUS_WEB_CACHE_PUT_FAULT;
	}
	cache->put_crc = aius_web_cache_crc32(cache->put_crc, bytes, length);
	cache->put_bytes += length;
	return AIUS_WEB_CACHE_PUT_OK;
}

FLASHMEM AiusWebCachePutResult aius_web_cache_put_commit(AiusWebCache *cache,
                                                         uint32_t now_ms)
{
	uint8_t slot;

	if (cache == NULL || cache->base == NULL ||
	    cache->put_entry == NO_ENTRY)
		return AIUS_WEB_CACHE_PUT_INVALID;
	if (cache->put_bytes != cache->put_length) {
		aius_web_cache_put_abort(cache);
		return AIUS_WEB_CACHE_PUT_INVALID;
	}
	slot = cache->put_entry;
	commit_entry(cache, slot, ENTRY_DIRTY, cache->put_crc, now_ms);
	cache->put_entry = NO_ENTRY;
	cache->put_length = 0u;
	cache->put_bytes = 0u;
	cache->put_crc = 0u;
	return AIUS_WEB_CACHE_PUT_OK;
}

/* The one-shot _put's per-chunk producer yield (header). */
static AiusWebCacheYield s_put_yield;
static void *s_put_yield_context;

FLASHMEM void aius_web_cache_set_put_yield(AiusWebCacheYield yield,
                                           void *context)
{
	s_put_yield = yield;
	s_put_yield_context = context;
}

FLASHMEM AiusWebCachePutResult aius_web_cache_put(AiusWebCache *cache,
                                                  const char *path,
                                                  size_t path_length,
                                                  const uint8_t *bytes,
                                                  uint32_t length,
                                                  uint8_t flags,
                                                  uint32_t now_ms)
{
	AiusWebCachePutResult r;
	uint8_t slot = NO_ENTRY;
	uint32_t file_crc = 0u;
	WebCacheEntry *e;

	bool was_pending = false;
	bool was_backup = false;

	if (bytes == NULL && length != 0u)
		return AIUS_WEB_CACHE_PUT_INVALID;
	r = reserve(cache, path, path_length, length, flags, &slot);
	if (r == AIUS_WEB_CACHE_PUT_FULL)
		r = reserve_in_place(cache, path, path_length, length, flags,
		                     &slot, &was_pending, &was_backup);
	if (r != AIUS_WEB_CACHE_PUT_OK)
		return r;
	e = entry_at(cache, slot);
	for (uint32_t at = 0u; at < length; at += AIUS_WEB_CACHE_CHUNK_BYTES) {
		const uint32_t i = at / AIUS_WEB_CACHE_CHUNK_BYTES;
		const uint32_t n = chunk_bytes(length, i);
		uint32_t c;

		if (!write_chunk(cache, (uint32_t)e->start + i, bytes + at, n,
		                 &c)) {
			e->state = ENTRY_FREE;
			/* A reused pending version is gone with it: say so, with
			 * its safe copy, so the caller can owe it (CR4-S1). */
			if (!was_pending)
				return AIUS_WEB_CACHE_PUT_FAULT;
			return was_backup ? AIUS_WEB_CACHE_PUT_FAULT_LOST_BACKUP
			                  : AIUS_WEB_CACHE_PUT_FAULT_LOST;
		}
		file_crc = aius_web_cache_crc32(file_crc, bytes + at, n);
		/* The entry is still PUTTING here (lookup misses it, invalidate
		 * leaves it), so a nested pass sees no half-written version. */
		if (s_put_yield != NULL)
			s_put_yield(s_put_yield_context);
	}
	commit_entry(cache, slot, ENTRY_DIRTY, file_crc, now_ms);
	/* A reused pending entry was its own previous holder of the path's
	 * safe-copy bit (as _tombstone's reuse). */
	if (was_backup && newest_pending(cache, e->path, strlen(e->path)) == e)
		e->flags |= AIUS_WEB_CACHE_FLAG_SAFE_BACKUP;
	return AIUS_WEB_CACHE_PUT_OK;
}

FLASHMEM AiusWebCachePutResult aius_web_cache_tombstone(AiusWebCache *cache,
                                                        const char *path,
                                                        size_t path_length,
                                                        uint8_t flags,
                                                        uint32_t now_ms)
{
	uint8_t slot;
	WebCacheEntry *e;

	if (cache == NULL || cache->base == NULL || !path_ok(path, path_length))
		return AIUS_WEB_CACHE_PUT_INVALID;
	slot = newest_of(cache, path, path_length);
	if (slot != NO_ENTRY) {
		e = entry_at(cache, slot);
		/* Reuse in place: unleased, not FLUSHING, not a running DELETE
		 * (N3). */
		if (e->leases != 0u || e->state == ENTRY_FLUSHING ||
		    (e->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) != 0u)
			slot = NO_ENTRY;
	}
	if (slot == NO_ENTRY) {
		slot = free_slot(cache);
		if (slot == NO_ENTRY)
			return AIUS_WEB_CACHE_PUT_FULL;
		e = entry_at(cache, slot);
		memset(e, 0, sizeof(*e));
		memcpy(e->path, path, path_length);
	} else {
		/* The reused entry's safe-copy bit is the path's; keep it for
		 * commit_entry to read through newest_pending. Every byte the
		 * new entry_crc covers comes from the caller (path, empty run,
		 * file_crc, a new commit_seq), so a CRC_FAULTED entry reused
		 * here carries none of its own bytes forward. The caller must
		 * never pass a faulted entry's own path back in (D-B1: the
		 * overlay drops a faulted tombstone instead). */
		e = entry_at(cache, slot);
		if (!is_pending(e->state))
			e->flags = 0u;
		memset(e->path, 0, PATH_STORAGE);
		memcpy(e->path, path, path_length);
	}
	{
		/* commit_entry reads the previous holder before retiring
		 * others; a reused pending entry is its own previous holder. */
		const bool was_backup =
			is_pending(e->state) &&
			(e->flags & AIUS_WEB_CACHE_FLAG_SAFE_BACKUP) != 0u;

		e->state = ENTRY_PUTTING; /* out of the pending set meanwhile */
		e->start = 0u;
		e->chunks = 0u;
		e->length = 0u;
		e->flags = (uint8_t)(flags & AIUS_WEB_CACHE_PUT_FLAGS);
		commit_entry(cache, slot, ENTRY_TOMBSTONE, 0u, now_ms);
		if (was_backup &&
		    newest_pending(cache, e->path, strlen(e->path)) == e)
			e->flags |= AIUS_WEB_CACHE_FLAG_SAFE_BACKUP;
	}
	return AIUS_WEB_CACHE_PUT_OK;
}

FLASHMEM AiusWebCacheState aius_web_cache_pending_state(
	const AiusWebCache *cache, const char *path, size_t path_length)
{
	const WebCacheEntry *p;

	if (cache == NULL || cache->base == NULL || !path_ok(path, path_length))
		return AIUS_WEB_CACHE_STATE_FREE;
	p = newest_pending(cache, path, path_length);
	if (p != NULL)
		return (AiusWebCacheState)p->state;
	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const WebCacheEntry *e = entry_at(cache, i);

		if (e->state == ENTRY_PUTTING && path_equals(e, path, path_length))
			return AIUS_WEB_CACHE_STATE_PUTTING;
	}
	return AIUS_WEB_CACHE_STATE_FREE;
}

static FLASHMEM WebCacheEntry *slot_entry(const AiusWebCache *cache,
                                          uint8_t slot)
{
	if (cache == NULL || cache->base == NULL ||
	    slot >= AIUS_WEB_CACHE_ENTRIES)
		return NULL;
	return entry_at(cache, slot);
}

FLASHMEM bool aius_web_cache_flush_begin(AiusWebCache *cache, uint8_t slot)
{
	WebCacheEntry *e = slot_entry(cache, slot);

	if (e == NULL || (e->flags & AIUS_WEB_CACHE_FLAG_CRC_FAULTED) != 0u ||
	    (e->state != ENTRY_DIRTY && e->state != ENTRY_TOMBSTONE))
		return false;
	/* A flush never starts on an entry whose own CRC fails (CR-S1). */
	if (!entry_sound(cache, e))
		return false;
	if (e->state == ENTRY_DIRTY) {
		e->state = ENTRY_FLUSHING;
		return true;
	}
	if (e->state == ENTRY_TOMBSTONE &&
	    (e->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) == 0u) {
		e->flags |= AIUS_WEB_CACHE_FLAG_DELETE_RUNNING;
		return true;
	}
	return false;
}

/* A DIRTY or TOMBSTONE entry of e's path newer than e. */
static FLASHMEM bool newer_pending(const AiusWebCache *cache,
                                   const WebCacheEntry *e)
{
	const size_t len = strlen(e->path);

	for (uint32_t i = 0; i < AIUS_WEB_CACHE_ENTRIES; i++) {
		const WebCacheEntry *o = entry_at(cache, i);

		if (o != e &&
		    (o->state == ENTRY_DIRTY || o->state == ENTRY_TOMBSTONE) &&
		    path_equals(o, e->path, len) &&
		    seq_newer(o->commit_seq, e->commit_seq))
			return true;
	}
	return false;
}

FLASHMEM void aius_web_cache_flush_end(AiusWebCache *cache, uint8_t slot,
                                       bool ok)
{
	WebCacheEntry *e = slot_entry(cache, slot);

	if (e == NULL)
		return;
	if (e->state == ENTRY_FLUSHING) {
		if (newer_pending(cache, e))
			retire(e);
		else if (ok) {
			e->state = ENTRY_VALID;
			e->flags = 0u;
		} else
			e->state = ENTRY_DIRTY;
		return;
	}
	if (e->state == ENTRY_TOMBSTONE &&
	    (e->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) != 0u) {
		e->flags &= (uint8_t)~AIUS_WEB_CACHE_FLAG_DELETE_RUNNING;
		if (newer_pending(cache, e) || ok)
			retire(e);
	}
}

FLASHMEM bool aius_web_cache_set_safe_copy(AiusWebCache *cache,
                                           const char *path,
                                           size_t path_length,
                                           AiusWebCacheSafeCopy which)
{
	WebCacheEntry *e;

	if (cache == NULL || cache->base == NULL || !path_ok(path, path_length))
		return false;
	e = newest_pending(cache, path, path_length);
	if (e == NULL)
		return false;
	if (which == AIUS_WEB_CACHE_SAFE_BACKUP)
		e->flags |= AIUS_WEB_CACHE_FLAG_SAFE_BACKUP;
	else
		e->flags &= (uint8_t)~AIUS_WEB_CACHE_FLAG_SAFE_BACKUP;
	return true;
}

FLASHMEM bool aius_web_cache_entry_info(AiusWebCache *cache, uint8_t slot,
                                        AiusWebCacheEntryInfo *info)
{
	WebCacheEntry *e = slot_entry(cache, slot);

	if (e == NULL || info == NULL || e->state == ENTRY_FREE)
		return false;
	if ((is_live(e->state) || e->state == ENTRY_STALE) &&
	    !entry_sound(cache, e) && !is_pending(e->state))
		return false;
	memcpy(info->path, e->path, PATH_STORAGE);
	info->path[AIUS_WEB_CACHE_PATH_MAX] = '\0';
	info->state = e->state;
	info->flags = e->flags;
	info->tries = e->tries;
	info->leases = e->leases;
	info->length = e->length;
	info->file_crc = e->file_crc;
	info->commit_seq = e->commit_seq;
	info->accept_ms = e->accept_ms;
	info->next_try_ms = e->next_try_ms;
	return true;
}

FLASHMEM bool aius_web_cache_set_retry(AiusWebCache *cache, uint8_t slot,
                                       uint32_t next_try_ms, uint8_t tries)
{
	WebCacheEntry *e = slot_entry(cache, slot);

	if (e == NULL || !is_pending(e->state))
		return false;
	e->next_try_ms = next_try_ms;
	e->tries = tries;
	return true;
}

FLASHMEM bool aius_web_cache_drop_pending(AiusWebCache *cache, uint8_t slot)
{
	WebCacheEntry *e = slot_entry(cache, slot);

	if (e == NULL ||
	    (e->state != ENTRY_DIRTY && e->state != ENTRY_TOMBSTONE) ||
	    (e->flags & AIUS_WEB_CACHE_FLAG_DELETE_RUNNING) != 0u)
		return false;
	retire(e);
	return true;
}
