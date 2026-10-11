#ifndef AIUS_WEB_CACHE_H
#define AIUS_WEB_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define AIUS_WEB_CACHE_STATIC_ASSERT static_assert
#else
#define AIUS_WEB_CACHE_STATIC_ASSERT _Static_assert
#endif

/* The one PSRAM store (aius-sample-rate-following.md Phase 4g, G2 to G4, G7,
 * G9, G10; Phase 4h, B1, H19): every /aius/www file, the presets listing and
 * 4h's pending writes (dsp.json, config.json, presets, sync uploads), held in
 * a pad-2 carve as 512 B chunks, each with a CRC-32 checked against the CHIP
 * (its dcache lines flush-deleted first) at fill and put, before the headers
 * and on the copy each chunk is sent or flushed from.
 *
 * Portable C: no Arduino, no register access. The two hardware effects come
 * in as hooks at init: `flush_delete` is arm_dcache_flush_delete on target,
 * and `chip_crc` is the one path through which this library reads chunk bytes
 * for a CRC (NULL selects aius_web_cache_crc32; a host suite installs a
 * counting reader there). Nothing here allocates or blocks; every function is
 * bounded by one chunk, one manifest scan or, for _verify and _put, one entry
 * (with a yield callback every AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS chunks for
 * _verify).
 *
 * KEYS are relative to /aius/, no leading '/': "www/index.html",
 * "www/presets/<name>.json", "dsp.json", "config.json" (4h NS4). Every path
 * compare folds ASCII case, as FAT and exFAT names do (N1); an entry keeps
 * its latest put's spelling.
 *
 * CARVE LAYOUT (offsets from the carve base, all inside the carve):
 *   [0, AIUS_WEB_CACHE_MANIFEST_BYTES)   the manifest: AIUS_WEB_CACHE_ENTRIES
 *                                        80 B entries (AiusWebCacheEntry),
 *                                        each with its own CRC
 *   [MANIFEST_BYTES, data_offset)        one uint32 CRC-32 per data chunk
 *   [data_offset, carve end)             data_chunks 512 B chunks
 * An entry owns one contiguous run of chunks; runs are first-fit. The control
 * block (AiusWebCache, the only DTCM state) holds the carve pointer, the
 * hooks, the one in-flight fill, the one in-flight streamed put and the
 * commit_seq counter. No per-entry state lives in DTCM or OCRAM.
 *
 * ENTRY STATES (AiusWebCacheState, the state byte, outside entry_crc):
 *   FREE       unused.
 *   FILLING    between _fill_begin and _publish; invisible to _lookup; a
 *              cancelled fill stays FILLING and resumes.
 *   VALID      clean: the card holds the same bytes; served.
 *   STALE      retired while leased: not served, chunks kept until the last
 *              _release.
 *   DIRTY      committed by _put / _put_commit, not yet on the card; served.
 *   FLUSHING   DIRTY pinned by _flush_begin for the job writing it; served
 *              until a newer version of the path commits.
 *   TOMBSTONE  committed DELETE not yet on the card; the path misses.
 *   PUTTING    reserved by _put_begin, body streaming, not committed:
 *              _lookup misses it, _invalidate leaves it, only _put_commit or
 *              _put_abort ends it (NS5).
 * DIRTY, FLUSHING and TOMBSTONE are PENDING: _invalidate never touches them,
 * so a card loss or remount drops clean entries only (H11). A lease pins an
 * entry's chunks across a retire, so a send already under way finishes from
 * the old bytes while the replacement serves the next request.
 *
 * NEWEST: among one path's DIRTY, FLUSHING, TOMBSTONE and VALID entries, the
 * one with the highest commit_seq, compared in serial-number arithmetic so a
 * wrap past 2^32 still orders. _lookup and the listings serve the newest only,
 * so a newer TOMBSTONE hides every older version while an older FLUSHING or
 * leased one finishes underneath (F1).
 *
 * SAFE COPY (E1, FR-B1): one bit per settings PATH, held in the flags byte
 * of that path's newest pending entry and nowhere else, never on a PUTTING
 * entry. A put moves it from the path's previous newest pending entry to the
 * new one; a first put starts it at TARGET, a restore-created one at BACKUP.
 *
 * CONTEXT: single-threaded; the fill and flush (SD checkpoint), the put sink
 * and the HTTP dispatch run in the same serial runtime, never from an ISR. */

/* Geometry. 16 MiB fitment: pad 1 is [0x70000000, 0x70800000), the recorder
 * staging slots and ring end at 0x70808000 (32 KiB + 8 MiB), so the carve is
 * the next 2 MiB of pad 2. */
#define AIUS_WEB_CACHE_PAD_BYTES UINT32_C(0x00800000)   /* 8 MiB per pad */
#define AIUS_WEB_CACHE_CARVE_BYTES UINT32_C(0x00200000) /* 2 MiB */
#define AIUS_WEB_CACHE_CHUNK_BYTES 512u
#define AIUS_WEB_CACHE_ENTRIES 64u /* owner Q-A: a documented cap, no growth */
#define AIUS_WEB_CACHE_ENTRY_BYTES 80u
#define AIUS_WEB_CACHE_MANIFEST_BYTES \
	(AIUS_WEB_CACHE_ENTRIES * AIUS_WEB_CACHE_ENTRY_BYTES) /* 5,120 B */
/* Longest storable key, relative to /aius/, without its NUL
 * ("www/presets/" plus a 32 B name is 44). A longer www file is never cached
 * and so is always served from the card; a longer put is refused. */
#define AIUS_WEB_CACHE_PATH_MAX 45u
#define AIUS_WEB_CACHE_PATH_STORAGE (AIUS_WEB_CACHE_PATH_MAX + 1u)
/* _verify calls its yield callback after every this many chunks. */
#define AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS 8u
/* `"<8 hex digits>-<up to 10 decimal digits>"` plus NUL. */
#define AIUS_WEB_CACHE_ETAG_MAX 22u
#define AIUS_WEB_CACHE_NO_ENTRY 0xFFu

typedef enum {
	AIUS_WEB_CACHE_STATE_FREE = 0,
	AIUS_WEB_CACHE_STATE_FILLING = 1,
	AIUS_WEB_CACHE_STATE_VALID = 2,
	AIUS_WEB_CACHE_STATE_STALE = 3,
	AIUS_WEB_CACHE_STATE_DIRTY = 4,
	AIUS_WEB_CACHE_STATE_FLUSHING = 5,
	AIUS_WEB_CACHE_STATE_TOMBSTONE = 6,
	AIUS_WEB_CACHE_STATE_PUTTING = 7
} AiusWebCacheState;

/* The entry's flags byte (mutable, outside entry_crc). */
#define AIUS_WEB_CACHE_FLAG_SETTINGS 0x01u        /* settings class */
#define AIUS_WEB_CACHE_FLAG_SAFE_BACKUP 0x02u     /* safe copy: 1 backup, 0 target */
#define AIUS_WEB_CACHE_FLAG_RESTORE_CREATED 0x04u /* H10 restore accept */
#define AIUS_WEB_CACHE_FLAG_CRC_FAULTED 0x08u     /* Q2: unflushable, skip */
#define AIUS_WEB_CACHE_FLAG_DELETE_RUNNING 0x10u  /* TOMBSTONE's DELETE job in flight (G1) */
/* Set with CRC_FAULTED when the entry itself failed (entry_crc or geometry),
 * not just a chunk: its path and fields are untrusted (CR-N1). */
#define AIUS_WEB_CACHE_FLAG_ENTRY_FAULTED 0x20u
/* The flags a caller may set at _put / _put_begin; the rest are the
 * library's. */
#define AIUS_WEB_CACHE_PUT_FLAGS \
	(AIUS_WEB_CACHE_FLAG_SETTINGS | AIUS_WEB_CACHE_FLAG_RESTORE_CREATED)

typedef enum {
	AIUS_WEB_CACHE_SAFE_TARGET = 0,
	AIUS_WEB_CACHE_SAFE_BACKUP = 1
} AiusWebCacheSafeCopy;

/* One manifest entry, resident in the carve (80 B, no padding). The first
 * 64 B are 4g's layout unchanged; the 16 B 4h adds follow in sixth closure
 * N2's order. `entry_crc` covers `path`, start..file_crc and `commit_seq`
 * (fixed once committed, N3); `state`, `leases`, `accept_ms`, `next_try_ms`,
 * `flags`, `tries` are mutable bookkeeping outside it. `path` is zero-padded
 * so the CRC is a function of the name alone. Owned by the library: callers
 * read entries only through the calls below. */
typedef struct {
	char path[AIUS_WEB_CACHE_PATH_STORAGE];
	uint8_t state;        /* AiusWebCacheState */
	uint8_t leases;
	uint16_t start;
	uint16_t chunks;
	uint32_t length;
	uint32_t file_crc;
	uint32_t entry_crc;
	uint32_t accept_ms;   /* debounce, oldest-dirty age, settings latency */
	uint32_t next_try_ms; /* S5 backoff: no flush before this */
	uint32_t commit_seq;  /* newest = highest, serial-number compare */
	uint8_t flags;        /* AIUS_WEB_CACHE_FLAG_* */
	uint8_t tries;        /* consecutive failed flushes */
	uint8_t reserved[2];
} AiusWebCacheEntry;

AIUS_WEB_CACHE_STATIC_ASSERT(sizeof(AiusWebCacheEntry) ==
                                 AIUS_WEB_CACHE_ENTRY_BYTES,
                             "a manifest entry is one 80 B slot");
AIUS_WEB_CACHE_STATIC_ASSERT(offsetof(AiusWebCacheEntry, state) == 46u &&
                                 offsetof(AiusWebCacheEntry, start) == 48u &&
                                 offsetof(AiusWebCacheEntry, length) == 52u &&
                                 offsetof(AiusWebCacheEntry, entry_crc) == 60u,
                             "4g's first 64 B are unchanged");
AIUS_WEB_CACHE_STATIC_ASSERT(offsetof(AiusWebCacheEntry, accept_ms) == 64u &&
                                 offsetof(AiusWebCacheEntry, next_try_ms) == 68u &&
                                 offsetof(AiusWebCacheEntry, commit_seq) == 72u &&
                                 offsetof(AiusWebCacheEntry, flags) == 76u &&
                                 offsetof(AiusWebCacheEntry, tries) == 77u &&
                                 offsetof(AiusWebCacheEntry, reserved) == 78u,
                             "N2: accept_ms, next_try_ms, commit_seq, flags, "
                             "tries, 2 B reserved");
AIUS_WEB_CACHE_STATIC_ASSERT(AIUS_WEB_CACHE_MANIFEST_BYTES == 5120u,
                             "the manifest is 5,120 B of carve");

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

/* The DTCM control block: 52 B on target (32-bit pointers), static-asserted
 * below and at most 64 B on any host. Treat as opaque; `crc_faults` is the
 * www_cache.crc_faults status key. */
typedef struct {
	uint8_t *base; /* carve base; NULL = absent, every call misses */
	AiusWebCacheFlushDelete flush_delete;
	AiusWebCacheChipCrc chip_crc;
	uint32_t data_offset;  /* from base */
	uint32_t fill_length;  /* bytes the in-flight fill expects */
	uint32_t fill_bytes;   /* bytes copied and read back so far */
	uint32_t fill_crc;     /* running CRC-32 of the card bytes */
	uint32_t crc_faults;   /* fill/put read-back, verify, send, entry faults */
	uint16_t data_chunks;
	uint8_t fill_entry;    /* AIUS_WEB_CACHE_NO_ENTRY when idle */
	uint8_t put_entry;     /* the streamed put's PUTTING entry, or NO_ENTRY */
	uint32_t put_length;   /* bytes the streamed put expects */
	uint32_t put_bytes;    /* bytes copied and read back so far */
	uint32_t put_crc;      /* running CRC-32 of the received bytes */
	uint32_t commit_seq;   /* last commit_seq issued */
} AiusWebCache;

AIUS_WEB_CACHE_STATIC_ASSERT(sizeof(AiusWebCache) <= 64u,
                             "G3: the DTCM control block stays under 64 B");
AIUS_WEB_CACHE_STATIC_ASSERT(sizeof(void *) != 4u || sizeof(AiusWebCache) == 52u,
                             "N1: the control block is 52 B on target");
AIUS_WEB_CACHE_STATIC_ASSERT(AIUS_WEB_CACHE_ENTRIES < AIUS_WEB_CACHE_NO_ENTRY,
                             "entry indices fit uint8_t beside the idle marker");

/* What _lookup reports for the path's newest servable entry. */
typedef struct {
	uint8_t entry;
	uint16_t chunks;
	uint32_t length;
	uint32_t file_crc; /* CRC-32 of the whole file, the ETag's first half */
} AiusWebCacheHit;

/* A copy of one entry's fields for the overlay's scheduler and status
 * (the entry's own CRC is checked on the chip first). */
typedef struct {
	char path[AIUS_WEB_CACHE_PATH_STORAGE];
	uint8_t state; /* AiusWebCacheState */
	uint8_t flags;
	uint8_t tries;
	uint8_t leases;
	uint32_t length;
	uint32_t file_crc;
	uint32_t commit_seq;
	uint32_t accept_ms;
	uint32_t next_try_ms;
} AiusWebCacheEntryInfo;

/* Outcome of a put or tombstone. */
typedef enum {
	AIUS_WEB_CACHE_PUT_OK = 0,
	AIUS_WEB_CACHE_PUT_FULL,    /* no free entry or run: 503 sd_busy, nothing evicted (Q4) */
	AIUS_WEB_CACHE_PUT_INVALID, /* bad path or length, absent cache, wrong call order */
	AIUS_WEB_CACHE_PUT_FAULT,   /* a chunk's chip read-back differed (counted) */
	/* FAULT in a one-shot put that reused the path's own pending entry in
	 * place (CR4-S1): the pending version it held is lost too. _LOST: its
	 * safe copy was the target; _LOST_BACKUP: the backup (H4). The caller
	 * owes the path's Q2 re-put, or counts a non-settings drop. */
	AIUS_WEB_CACHE_PUT_FAULT_LOST,
	AIUS_WEB_CACHE_PUT_FAULT_LOST_BACKUP
} AiusWebCachePutResult;

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

/* Phase 4h: the producer yield the one-shot _put calls after each chunk it
 * copies and reads back (12 KB of dsp.json is 24 chunks in one call; the
 * streamed _put_chunk and _copy_chunk move one chunk per call and take
 * none). One binding per image, outside the control block so it stays
 * 52 B (N1); NULL (the default) disables it. Survives _init. */
void aius_web_cache_set_put_yield(AiusWebCacheYield yield, void *context);

/* zlib-compatible CRC-32 (crc 0 to start); nibble table in flash. */
uint32_t aius_web_cache_crc32(uint32_t crc, const uint8_t *data,
                              uint32_t bytes);

/* Finds the path's newest entry among DIRTY, FLUSHING, TOMBSTONE and VALID
 * (key relative to /aius/, case-folded). A TOMBSTONE newest, a PUTTING or
 * FILLING entry, or no entry is a miss. The entry's own CRC is checked on the
 * chip first; a mismatch counts a fault, sets `*fault` (may be NULL; false
 * on every other outcome) and misses: a clean entry is dropped so the caller
 * can refill, a pending one is kept and flagged CRC_FAULTED for the overlay's
 * service step (Q2). A miss on a clean path means serve from the card. */
bool aius_web_cache_lookup(AiusWebCache *cache, const char *path,
                           size_t path_length, AiusWebCacheHit *hit,
                           bool *fault);

/* The VALID manifest entries (STALE, FILLING and pending excluded): the
 * www_cache.entries status key. One manifest scan, derived from the entry
 * states, so an entry dropped twice (invalidated while leased, then a CRC
 * fault on the lease) leaves the count once. 0 for an absent cache. */
uint32_t aius_web_cache_entry_count(const AiusWebCache *cache);

/* Pins a servable (VALID, DIRTY or FLUSHING) entry's chunks; false
 * otherwise. Every true lease needs exactly one _release. */
bool aius_web_cache_lease(AiusWebCache *cache, uint8_t entry);
/* Unpins; a STALE entry with no lease left frees its chunks. */
void aius_web_cache_release(AiusWebCache *cache, uint8_t entry);

/* Pre-header check of every chunk: flush-delete, then chip CRC against the
 * accept-time chunk CRC, `yield` (may be NULL) after every
 * AIUS_WEB_CACHE_VERIFY_YIELD_CHUNKS chunks. A mismatch counts a fault and
 * returns false: a clean entry is dropped (STALE while leased; serve from
 * the card and refill), a pending one is flagged CRC_FAULTED (Q2). */
bool aius_web_cache_verify(AiusWebCache *cache, uint8_t entry,
                           AiusWebCacheYield yield, void *yield_context);

/* Copy of chunk `chunk` into `dst` (AIUS_WEB_CACHE_CHUNK_BYTES): flush-delete,
 * copy, CRC the COPY against the accept-time chunk CRC. Serves the send path
 * and the flush's input (S3; a FLUSHING entry and a leased STALE one are
 * readable). Sets `*bytes` (512, or the tail). False on a mismatch (fault
 * counted; clean entry dropped, pending entry flagged CRC_FAULTED) or a bad
 * entry/chunk. */
bool aius_web_cache_copy_chunk(AiusWebCache *cache, uint8_t entry,
                               uint16_t chunk, uint8_t *dst,
                               uint32_t *bytes);

/* Starts filling `path` of `length` bytes. Drops any unpublished fill first.
 * False when the path is too long, the path is pending (DIRTY, FLUSHING,
 * TOMBSTONE or PUTTING: the store's bytes win, D2), or capacity is full (no
 * free entry or no free run of chunks): the file stays uncached and is
 * served from the card. A VALID entry for the same path keeps serving until
 * _publish. */
bool aius_web_cache_fill_begin(AiusWebCache *cache, const char *path,
                               size_t path_length, uint32_t length);

/* One fill primitive: copies `card` (exactly 512 B, or the file's tail) into
 * the next chunk, flush-deletes it and CRCs it read back from the chip before
 * returning; folds the card bytes into the file CRC. False, with the fill
 * dropped, on a wrong size, no fill in flight, or a read-back CRC that
 * differs from the card bytes (a counted fault). */
bool aius_web_cache_fill_chunk(AiusWebCache *cache, const uint8_t *card,
                               uint32_t bytes);

/* Makes the filled entry VALID with the next commit_seq: confirms every
 * chunk was copied and read back, writes the manifest entry and its CRC, and
 * retires any older VALID entry for the path. Reads no chunk (G10). False,
 * with the fill dropped, when the fill is incomplete or the path turned
 * pending meanwhile (a card file never replaces a pending entry, H11). */
bool aius_web_cache_publish(AiusWebCache *cache);

/* Drops the in-flight unpublished fill, if any: a failed fill file, never a
 * cancelled one (which resumes). */
void aius_web_cache_fill_abort(AiusWebCache *cache);

/* RESUME (owner, clean-room finding 1): a cancelled fill file keeps its
 * FILLING entry, its checked chunks and its running file CRC, and the restart
 * continues at its next chunk. Only _fill_begin (another file), _invalidate
 * (remount, card loss), _put_commit / _put / _tombstone of its path,
 * _fill_abort and a failed _fill_chunk or _fill_resume drop it.
 *
 * _fill_resume_offset: where the fill of `path` restarts: the bytes the
 * in-flight fill of that path already checked (a multiple of
 * AIUS_WEB_CACHE_CHUNK_BYTES, or the whole length), else 0 (start afresh with
 * _fill_begin). Reads nothing in the carve but the entry header.
 *
 * _fill_resume: the restarted job's first chunk (or its end, for a job that
 * emitted none) continues that fill at `offset` of a card file now `length`
 * bytes long. True when the in-flight fill is `path`'s, at exactly `offset`,
 * begun at the same `length`, and the path is not pending; the next
 * _fill_chunk then takes the chunk at `offset`. Otherwise the partial fill is
 * dropped (a size change between the cancel and the resume restarts the file
 * from 0) and false. */
uint32_t aius_web_cache_fill_resume_offset(const AiusWebCache *cache,
                                           const char *path,
                                           size_t path_length);
bool aius_web_cache_fill_resume(AiusWebCache *cache, const char *path,
                                size_t path_length, uint32_t offset,
                                uint32_t length);

/* Retires every CLEAN entry for `path` (VALID and FILLING; leased entries go
 * STALE); DIRTY, FLUSHING, TOMBSTONE and PUTTING are never touched (H11,
 * NS5). A path ending in '/' retires everything under that directory; NULL
 * retires everything clean (remount, card loss). Case-folded. */
void aius_web_cache_invalidate(AiusWebCache *cache, const char *path,
                               size_t path_length);

/* The listing of directory `dir` ("www/" or "www/presets/", with its trailing
 * '/'; "" for the root) from each path's newest entry: VALID, DIRTY and
 * FLUSHING rows, TOMBSTONE omitted (S6); each file directly in it once, and
 * each subdirectory once as a directory row. Case-folded. Returns the rows
 * emitted. Completeness (every card file cached) is the caller's to know: an
 * entry whose own CRC fails is dropped (pending: flagged), counted in
 * crc_faults and its row omitted, so a caller serving a complete listing
 * compares crc_faults across the call and reads the card instead when it
 * moved. */
size_t aius_web_cache_listing(AiusWebCache *cache, const char *dir,
                              size_t dir_length, AiusWebCacheListingRow row,
                              void *context);

/* The pending rows only (DIRTY and FLUSHING newest, files directly in `dir`),
 * for merging into a card-read listing (S6); the caller hides a card row
 * whose _pending_state is TOMBSTONE. Returns the rows emitted. */
size_t aius_web_cache_pending_listing(AiusWebCache *cache, const char *dir,
                                      size_t dir_length,
                                      AiusWebCacheListingRow row,
                                      void *context);

/* Writes the strong ETag `"<crc32 %08x>-<length>"` with its quotes and a
 * NUL; returns its length without the NUL, 0 when `capacity` is short. */
size_t aius_web_cache_etag(const AiusWebCacheHit *hit, char *out,
                           size_t capacity);

/* ---- 4h write-behind (the policy is lib/aius_sd_overlay's) ---- */

/* Streamed put (the www upload sink; the one put the single HTTP transaction
 * carries, NS5). Reserves a FREE entry as PUTTING for `path` of `length`
 * bytes and a chunk run, with `flags` (only AIUS_WEB_CACHE_PUT_FLAGS bits
 * kept). FULL when no entry or no run is free (Q4: refuse, never evict);
 * INVALID when a streamed put is already in flight, the path is bad or the
 * cache is absent. Reads no card. */
AiusWebCachePutResult aius_web_cache_put_begin(AiusWebCache *cache,
                                               const char *path,
                                               size_t path_length,
                                               uint32_t length, uint8_t flags);

/* Copies the next `bytes` (exactly 512, or the tail) into the PUTTING entry,
 * flush-deletes and CRCs the chunk read back from the chip against the
 * received bytes (G10), and folds them into the file CRC. A mismatch counts
 * a fault, frees the PUTTING entry and returns FAULT; a wrong size or no put
 * in flight returns INVALID (and aborts any put). */
AiusWebCachePutResult aius_web_cache_put_chunk(AiusWebCache *cache,
                                               const uint8_t *bytes,
                                               uint32_t length);

/* Commits the streamed put: every chunk checked, else INVALID and aborted.
 * The entry turns DIRTY with the next commit_seq and accept_ms `now_ms`,
 * tries 0 and next_try_ms 0. It retires the path's VALID entries (STALE while
 * leased), any older DIRTY one and any older TOMBSTONE whose DELETE is not
 * running (G1), and drops the path's FILLING partial; a FLUSHING entry, or a
 * TOMBSTONE whose DELETE is running, stays pinned until its _flush_end
 * (FR-S4, G1). The safe-copy bit moves to the new entry from the path's
 * newest DIRTY, FLUSHING or TOMBSTONE entry as it stands at commit; with
 * none it starts at TARGET (BACKUP when RESTORE_CREATED). */
AiusWebCachePutResult aius_web_cache_put_commit(AiusWebCache *cache,
                                                uint32_t now_ms);

/* Frees the streamed put's PUTTING entry and its run; no-op when none. The
 * orphan sweep's and a client drop's call (NS5, D4). */
void aius_web_cache_put_abort(AiusWebCache *cache);

/* True while a streamed put holds the put slot. */
bool aius_web_cache_put_active(const AiusWebCache *cache);

/* One-shot put (D6): reserves, copies and chip-checks each chunk of
 * `bytes[0, length)` and commits in one call, with _put_commit's retirement
 * and safe-copy rules and `flags` as _put_begin's. Takes no put slot, so it
 * runs while a streamed put holds the slot. The store routes (dsp.json,
 * config.json), the H10 restore and the Q2 re-put use it. With no free entry
 * or run it reuses the path's own newest entry in place when unpinned
 * (unleased, not FLUSHING, no DELETE running; CR3-B1), so a settings put
 * lands whatever else the manifest holds. FULL or INVALID leave the store
 * unchanged; FAULT frees the reserved entry; FAULT_LOST / _LOST_BACKUP when
 * that entry was the path's own pending version, now lost (CR4-S1). */
AiusWebCachePutResult aius_web_cache_put(AiusWebCache *cache,
                                         const char *path, size_t path_length,
                                         const uint8_t *bytes, uint32_t length,
                                         uint8_t flags, uint32_t now_ms);

/* Commits a DELETE of `path` as a TOMBSTONE with the next commit_seq and
 * accept_ms `now_ms`, carrying `flags` as _put's. Reuses the path's own
 * entry in place when it is unleased and not FLUSHING (a cached preset's
 * delete needs no new entry, N3), else takes a FREE one, else FULL (the
 * DELETE answers 503 sd_busy). Then, as _put_commit: retires the path's other
 * VALID and DIRTY entries (STALE while leased), any older non-running
 * TOMBSTONE and FILLING partial, and moves the safe-copy bit to the tombstone
 * (F1). A tombstone owns no chunks. */
AiusWebCachePutResult aius_web_cache_tombstone(AiusWebCache *cache,
                                               const char *path,
                                               size_t path_length,
                                               uint8_t flags, uint32_t now_ms);

/* The path's pending state for shadows(): the newest committed pending
 * entry's state (DIRTY, FLUSHING or TOMBSTONE); else PUTTING when only a
 * PUTTING entry exists (pending for _fill_begin, _fill_resume, _publish and
 * the walk's skip, but not shadowed, D2); else FREE (none). VALID is never
 * reported. Case-folded. */
AiusWebCacheState aius_web_cache_pending_state(const AiusWebCache *cache,
                                               const char *path,
                                               size_t path_length);

/* Pins entry `slot` for its card job: DIRTY turns FLUSHING; a TOMBSTONE gets
 * DELETE_RUNNING. False for any other state, a CRC_FAULTED entry or a bad
 * slot. The pinned version's chunks stay readable until _flush_end. */
bool aius_web_cache_flush_begin(AiusWebCache *cache, uint8_t slot);

/* Ends entry `slot`'s card job. FLUSHING: when a newer DIRTY or TOMBSTONE of
 * the path exists it is retired (FREE unleased, STALE until its last
 * _release) whatever `ok`; else `ok` turns it VALID and failure returns it to
 * DIRTY with its safe-copy bit. TOMBSTONE (DELETE_RUNNING cleared): when a
 * newer DIRTY entry committed during the job it is retired whatever `ok`
 * (G1); else `ok` frees it and failure leaves it TOMBSTONE (NS1). The
 * caller (the overlay) sets tries and next_try_ms; this call does not. */
void aius_web_cache_flush_end(AiusWebCache *cache, uint8_t slot, bool ok);

/* Writes the safe-copy bit on `path`'s newest pending entry (DIRTY, FLUSHING
 * or TOMBSTONE), which is a newer DIRTY entry when a put landed mid-job
 * (E1). False when the path has no pending entry. */
bool aius_web_cache_set_safe_copy(AiusWebCache *cache, const char *path,
                                  size_t path_length,
                                  AiusWebCacheSafeCopy which);

/* Copies entry `slot`'s fields into `info` after checking its CRC on the
 * chip (a pending entry that fails is flagged CRC_FAULTED and still
 * reported; a clean one is dropped and false). False for FREE or a bad
 * slot. One entry, no chunk read. */
bool aius_web_cache_entry_info(AiusWebCache *cache, uint8_t slot,
                               AiusWebCacheEntryInfo *info);

/* Sets entry `slot`'s S5 backoff bookkeeping. False unless pending. */
bool aius_web_cache_set_retry(AiusWebCache *cache, uint8_t slot,
                              uint32_t next_try_ms, uint8_t tries);

/* Drops a pending entry the overlay gives up on (Q2: a CRC-faulted preset or
 * www entry): retired (STALE while leased), so the card's older file returns
 * at the next refill. False unless DIRTY or TOMBSTONE and not running. */
bool aius_web_cache_drop_pending(AiusWebCache *cache, uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif
