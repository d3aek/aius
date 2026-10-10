#ifndef AIUS_WEB_CACHE_H
#define AIUS_WEB_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The PSRAM web cache (aius-sample-rate-following.md Phase 4g, G2 to G4, G7,
 * G9, G10): every /sd/www file and the presets listing, held in a pad-2 carve
 * as 512 B chunks, each with a CRC-32 checked against the CHIP (its dcache
 * lines flush-deleted first) at fill, before the headers and on the copy each
 * chunk is sent from.
 *
 * Portable C: no Arduino, no register access. The two hardware effects come
 * in as hooks at init: `flush_delete` is arm_dcache_flush_delete on target,
 * and `chip_crc` is the one path through which this library reads chunk bytes
 * for a CRC (NULL selects aius_web_cache_crc32; a host suite installs a
 * counting reader there). Nothing here allocates or blocks; every function is
 * bounded by one chunk, one manifest scan or, for _verify, one entry with a
 * yield callback every AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS chunks.
 *
 * CARVE LAYOUT (offsets from the carve base, all inside the carve):
 *   [0, AIUS_WEB_CACHE_MANIFEST_BYTES)   the manifest: AIUS_WEB_CACHE_ENTRIES
 *                                        64 B entries, each with its own CRC
 *   [MANIFEST_BYTES, data_offset)        one uint32 CRC-32 per data chunk
 *   [data_offset, carve end)             data_chunks 512 B chunks
 * An entry owns one contiguous run of chunks; runs are first-fit. The control
 * block (AiusWebCache, the only DTCM state) holds the carve pointer, the hooks
 * and the one in-flight fill.
 *
 * ENTRY STATES: FREE; FILLING (between _fill_begin and _publish, invisible to
 * _lookup; a cancelled fill file stays FILLING and resumes); VALID (served); STALE (invalidated or replaced while leased: not
 * served, chunks kept until the last _release). A lease pins an entry's chunks
 * across an invalidate, so a send already under way finishes from the old
 * bytes while the replacement serves the next request.
 *
 * CONTEXT: single-threaded; the fill (SD checkpoint) and the HTTP dispatch run
 * in the same serial runtime, never from an ISR. */

/* Geometry. 16 MiB fitment: pad 1 is [0x70000000, 0x70800000), the recorder
 * staging slots and ring end at 0x70808000 (32 KiB + 8 MiB), so the carve is
 * the next 2 MiB of pad 2. */
#define AIUS_WEB_CACHE_PAD_BYTES UINT32_C(0x00800000)   /* 8 MiB per pad */
#define AIUS_WEB_CACHE_CARVE_BYTES UINT32_C(0x00200000) /* 2 MiB */
#define AIUS_WEB_CACHE_CHUNK_BYTES 512u
#define AIUS_WEB_CACHE_ENTRIES 64u
#define AIUS_WEB_CACHE_ENTRY_BYTES 64u
#define AIUS_WEB_CACHE_MANIFEST_BYTES \
	(AIUS_WEB_CACHE_ENTRIES * AIUS_WEB_CACHE_ENTRY_BYTES)
/* Longest cacheable path, relative to /sd/www, without its NUL ("presets/"
 * plus a 32-byte index name is 40). A longer file is never cached and so is
 * always served from the card. */
#define AIUS_WEB_CACHE_PATH_MAX 45u
/* _verify calls its yield callback after every this many chunks. */
#define AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS 8u
/* `"<8 hex digits>-<up to 10 decimal digits>"` plus NUL. */
#define AIUS_WEB_CACHE_ETAG_MAX 22u
#define AIUS_WEB_CACHE_NO_ENTRY 0xFFu

typedef struct {
	uint32_t offset;   /* from the FlexSPI2 base (0x70000000) */
	uint32_t capacity; /* AIUS_WEB_CACHE_CARVE_BYTES */
} AiusWebCachePlacement;

/* arm_dcache_flush_delete(addr, bytes): write back and drop the lines so the
 * next read comes from the chip. */
typedef void (*AiusWebCacheFlushDelete)(void *addr, uint32_t bytes);
/* zlib-style CRC-32 continuation over `bytes` read at `chip`. */
typedef uint32_t (*AiusWebCacheChipCrc)(uint32_t crc, const uint8_t *chip,
                                        uint32_t bytes);
/* Producer yield point inside _verify. */
typedef void (*AiusWebCacheYield)(void *context);
/* One listing row; return false to stop the listing early. */
typedef bool (*AiusWebCacheListingRow)(void *context, const char *name,
                                       size_t name_length,
                                       uint32_t size_bytes,
                                       bool is_directory);

/* The DTCM control block, under 64 B (static-asserted in the source). Treat
 * as opaque; `crc_faults` is the www_cache.crc_faults status key. */
typedef struct {
	uint8_t *base; /* carve base; NULL = absent, every call misses */
	AiusWebCacheFlushDelete flush_delete;
	AiusWebCacheChipCrc chip_crc;
	uint32_t data_offset;  /* from base */
	uint32_t fill_length;  /* bytes the in-flight fill expects */
	uint32_t fill_bytes;   /* bytes copied and read back so far */
	uint32_t fill_crc;     /* running CRC-32 of the card bytes */
	uint32_t crc_faults;   /* fill read-back, verify, send and entry faults */
	uint16_t data_chunks;
	uint8_t fill_entry;    /* AIUS_WEB_CACHE_NO_ENTRY when idle */
} AiusWebCache;

/* What _lookup reports for a VALID entry. */
typedef struct {
	uint8_t entry;
	uint16_t chunks;
	uint32_t length;
	uint32_t file_crc; /* CRC-32 of the whole file, the ETag's first half */
} AiusWebCacheHit;

/* The carve for a fitment: AIUS_WEB_CACHE_CARVE_BYTES on pad 2 from
 * max(recorder_end_offset, pad 1's end), where recorder_end_offset is the
 * recorder's staging bytes plus its ring bytes (0x808000 on 16 MiB).
 * `mapped_psram_bytes` is external_psram_size in bytes. False (no cache) when
 * pad 2 is not mapped (the 8 MiB and pad-2-only fitments) or the carve does
 * not fit. Disjoint by construction from the recorder ring and from every
 * aius_ota_staging_place() result, which stays on pad 1 whenever this
 * succeeds. */
bool aius_web_cache_place(uint32_t mapped_psram_bytes,
                          uint32_t recorder_end_offset,
                          AiusWebCachePlacement *placement);

/* Binds the carve and empties the manifest. `base` NULL or `bytes` too small
 * leaves the cache absent (returns false; every later call misses or fails).
 * `flush_delete` is required for a present cache; `chip_crc` may be NULL. */
bool aius_web_cache_init(AiusWebCache *cache, uint8_t *base, uint32_t bytes,
                         AiusWebCacheFlushDelete flush_delete,
                         AiusWebCacheChipCrc chip_crc);

/* zlib-compatible CRC-32 (crc 0 to start); nibble table in flash. */
uint32_t aius_web_cache_crc32(uint32_t crc, const uint8_t *data,
                              uint32_t bytes);

/* Finds the VALID entry for `path` (relative to /sd/www, no leading '/').
 * The entry's own CRC is checked on the chip first; a mismatch drops the
 * entry, counts a fault, sets `*fault` (may be NULL; false on every other
 * outcome) and misses, so the caller can schedule its refill. A miss means
 * serve from the card. */
bool aius_web_cache_lookup(AiusWebCache *cache, const char *path,
                           size_t path_length, AiusWebCacheHit *hit,
                           bool *fault);

/* The VALID manifest entries (STALE and FILLING excluded): the
 * www_cache.entries status key. One manifest scan, derived from the entry
 * states, so an entry dropped twice (invalidated while leased, then a CRC
 * fault on the lease) leaves the count once. 0 for an absent cache. */
uint32_t aius_web_cache_entry_count(const AiusWebCache *cache);

/* Pins a VALID entry's chunks; false when it is not VALID. Every true lease
 * needs exactly one _release. */
bool aius_web_cache_lease(AiusWebCache *cache, uint8_t entry);
/* Unpins; a STALE entry with no lease left frees its chunks. */
void aius_web_cache_release(AiusWebCache *cache, uint8_t entry);

/* Pre-header check of every chunk: flush-delete, then chip CRC against the
 * fill's card CRC, `yield` (may be NULL) after every
 * AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS chunks. A mismatch drops the entry
 * (STALE while leased), counts a fault and returns false: serve from the card
 * and refill. */
bool aius_web_cache_verify(AiusWebCache *cache, uint8_t entry,
                           AiusWebCacheYield yield, void *yield_context);

/* Send-path copy of chunk `chunk` into `dst` (AIUS_WEB_CACHE_CHUNK_BYTES):
 * flush-delete, copy, CRC the COPY against the fill's card CRC. Sets `*bytes`
 * (512, or the tail). False on a mismatch (entry dropped, fault counted:
 * close the connection and refill) or a bad entry/chunk. */
bool aius_web_cache_copy_chunk(AiusWebCache *cache, uint8_t entry,
                               uint16_t chunk, uint8_t *dst,
                               uint32_t *bytes);

/* Starts filling `path` of `length` bytes. Drops any unpublished fill first.
 * False when the path is too long or capacity is full (no free entry or no
 * free run of chunks): the file stays uncached and is served from the card.
 * A VALID entry for the same path keeps serving until _publish. */
bool aius_web_cache_fill_begin(AiusWebCache *cache, const char *path,
                               size_t path_length, uint32_t length);

/* One fill primitive: copies `card` (exactly 512 B, or the file's tail) into
 * the next chunk, flush-deletes it and CRCs it read back from the chip before
 * returning; folds the card bytes into the file CRC. False, with the fill
 * dropped, on a wrong size, no fill in flight, or a read-back CRC that
 * differs from the card bytes (a counted fault). */
bool aius_web_cache_fill_chunk(AiusWebCache *cache, const uint8_t *card,
                               uint32_t bytes);

/* Makes the filled entry VALID: confirms every chunk was copied and read
 * back, writes the manifest entry and its CRC, and retires any older entry for
 * the path. Reads no chunk (G10). False, with the fill dropped, when the fill
 * is incomplete. */
bool aius_web_cache_publish(AiusWebCache *cache);

/* Drops the in-flight unpublished fill, if any: a failed fill file, never a
 * cancelled one (which resumes). */
void aius_web_cache_fill_abort(AiusWebCache *cache);

/* RESUME (owner, clean-room finding 1): a cancelled fill file keeps its
 * FILLING entry, its checked chunks and its running file CRC, and the restart
 * continues at its next chunk. Only _fill_begin (another file), _invalidate
 * (upload, delete, remount, card loss), _fill_abort and a failed _fill_chunk
 * or _fill_resume drop it.
 *
 * _fill_resume_offset: where the fill of `path` restarts: the bytes the
 * in-flight fill of that path already checked (a multiple of
 * AIUS_WEB_CACHE_CHUNK_BYTES, or the whole length), else 0 (start afresh with
 * _fill_begin). Reads nothing in the carve but the entry header.
 *
 * _fill_resume: the restarted job's first chunk (or its end, for a job that
 * emitted none) continues that fill at `offset` of a card file now `length`
 * bytes long. True when the in-flight fill is `path`'s, at exactly `offset`,
 * begun at the same `length`; the next _fill_chunk then takes the chunk at
 * `offset`. Otherwise the partial fill is dropped (a size change between the
 * cancel and the resume restarts the file from 0) and false. */
uint32_t aius_web_cache_fill_resume_offset(const AiusWebCache *cache,
                                           const char *path,
                                           size_t path_length);
bool aius_web_cache_fill_resume(AiusWebCache *cache, const char *path,
                                size_t path_length, uint32_t offset,
                                uint32_t length);

/* Retires every entry for `path` (VALID and FILLING; leased entries go STALE).
 * A path ending in '/' retires everything under that directory; NULL retires
 * everything (remount, card swap). */
void aius_web_cache_invalidate(AiusWebCache *cache, const char *path,
                               size_t path_length);

/* The listing of directory `dir` ("" for the www root, else "presets/" with
 * its trailing '/') from the VALID manifest entries: each file directly in it
 * once, and each subdirectory once as a directory row. Returns the rows
 * emitted. Completeness (every card file cached) is the caller's to know: an
 * entry whose own CRC fails is dropped, counted in crc_faults and its row
 * omitted, so a caller serving a complete listing compares crc_faults across
 * the call and reads the card instead when it moved. */
size_t aius_web_cache_listing(AiusWebCache *cache, const char *dir,
                              size_t dir_length, AiusWebCacheListingRow row,
                              void *context);

/* Writes the strong ETag `"<crc32 %08x>-<length>"` with its quotes and a
 * NUL; returns its length without the NUL, 0 when `capacity` is short. */
size_t aius_web_cache_etag(const AiusWebCacheHit *hit, char *out,
                           size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
