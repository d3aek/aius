#ifndef AIUS_SD_OVERLAY_H
#define AIUS_SD_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_web_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SD write-behind overlay (aius-sample-rate-following.md Phase 4h, H19):
 * the policy over aius_web_cache's one PSRAM store. A write (DSP Apply,
 * public config, preset save, import, delete, card-sync upload) is accepted
 * into the store and served from there at once; a background flush, one file
 * at a time, writes it to the card and reads it back.
 *
 * Portable C: no Arduino, no card access. The platform runs the card jobs
 * this library describes (AiusSdOverlayJob) and reports their progress back;
 * the Q2 re-put serialiser and the event log come in through
 * AiusSdOverlayPlatform. Single-threaded, never from an ISR.
 *
 * KEYS are relative to /aius/, no leading '/' ("dsp.json", "config.json",
 * "www/presets/<name>.json", "www/index.html"); every compare folds ASCII
 * case, as FAT and exFAT names do (N1), so a case-variant put replaces the
 * pending entry rather than adding a second one for the same file.
 *
 * CLASSES: settings ("dsp.json", "config.json" and "www/presets/<name>.json"
 * with no further '/', matched case-folded) flush before sync (every other
 * path, other names under "www/presets/" included, N5), oldest accept first
 * within a class, after a 500 ms debounce.
 * A settings entry flushes as one copy-before-write job with its fixed backup
 * (H4); a sync entry as today's REPLACE; a settings tombstone as a DELETE of
 * the backup then one of the target (or the target's alone when no backup
 * path can be formed, FR-S9); a sync tombstone as one DELETE. Every DELETE
 * carries require_existing false (S2).
 *
 * HELD (H3): a card found gone (a card-gone fault, the probe's no-card
 * verdict, an SD mode change dropping the mount) sets held; no flush starts
 * while held, a failure while held counts no try, and the next mount clears
 * it. Pending entries survive it (the store's invalidate drops clean ones
 * only, H11). A board booted without a card starts held (Q1). */

#define AIUS_SD_OVERLAY_DEBOUNCE_MS 500u
#define AIUS_SD_OVERLAY_BACKOFF_MIN_MS 1000u  /* S5: first retry delay */
#define AIUS_SD_OVERLAY_BACKOFF_MAX_MS 60000u /* S5: doubling cap */
/* Absolute card paths the jobs carry; equals lib/aius_sd's
 * AIUS_SD_PATH_STORAGE (the platform static-asserts it, since this library
 * does not link aius_sd). */
#define AIUS_SD_OVERLAY_PATH_STORAGE 128u
#define AIUS_SD_OVERLAY_ROOT "/aius/"
/* Fixed backups (H4), absolute. A preset's is
 * AIUS_SD_OVERLAY_BACKUP_PRESET_PREFIX "<name>.json". */
#define AIUS_SD_OVERLAY_BACKUP_DSP "/aius/.bak-dsp.json"
#define AIUS_SD_OVERLAY_BACKUP_CONFIG "/aius/.bak-config.json"
#define AIUS_SD_OVERLAY_BACKUP_PRESET_PREFIX "/aius/.bak-preset-"

typedef enum {
	AIUS_SD_OVERLAY_CLASS_SYNC = 0,
	AIUS_SD_OVERLAY_CLASS_SETTINGS = 1
} AiusSdOverlayClass;

/* The two settings files the running state is authoritative for (Q2). */
typedef enum {
	AIUS_SD_OVERLAY_FILE_DSP = 0,    /* dsp.json */
	AIUS_SD_OVERLAY_FILE_CONFIG = 1  /* config.json */
} AiusSdOverlaySettingsFile;

typedef enum {
	AIUS_SD_OVERLAY_PUT_OK = 0,
	AIUS_SD_OVERLAY_PUT_BUSY,    /* store full: 503 sd_busy + Retry-After, nothing evicted (Q4) */
	AIUS_SD_OVERLAY_PUT_INVALID, /* key not storable, or a preset whose backup path cannot fit: refused at accept */
	AIUS_SD_OVERLAY_PUT_ABSENT,  /* no carve (8 MiB fitment): keep today's synchronous card write (Q4) */
	AIUS_SD_OVERLAY_PUT_FAULT,   /* a chunk's chip read-back failed; nothing committed */
	AIUS_SD_OVERLAY_PUT_LIMIT    /* a new preset name at the 16-preset cap: 409 preset_limit */
} AiusSdOverlayPutResult;

#if !defined(__arm__)
/* Host proof only (no target DTCM cost): entry reads of the last preset-cap
 * scan, at most AIUS_WEB_CACHE_ENTRIES. */
extern unsigned aius_sd_overlay_preset_scan_reads;
#endif

/* The saved-preset cap (Phase 4l). */
#define AIUS_SD_OVERLAY_PRESET_LIMIT 16u

/* The card job the overlay hands the platform (one at a time). */
typedef enum {
	AIUS_SD_OVERLAY_JOB_NONE = 0,
	/* Settings save: target and backup each overwritten in place, `order`
	 * first, then CACHE_DROP and the target's read-back (lib/aius_sd's
	 * copy-before-write install mode). */
	AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE,
	/* Sync save: today's AIUS_SD_INSTALL_REPLACE (owner scope 2026-10-07). */
	AIUS_SD_OVERLAY_JOB_REPLACE,
	/* Tombstone: DELETE of `target_path` with require_existing false. */
	AIUS_SD_OVERLAY_JOB_DELETE
} AiusSdOverlayJobKind;

typedef enum {
	AIUS_SD_OVERLAY_ORDER_BACKUP_FIRST = 0, /* safe copy is the target */
	AIUS_SD_OVERLAY_ORDER_TARGET_FIRST = 1  /* safe copy is the backup */
} AiusSdOverlayOrder;

/* Which file a DELETE job removes; a settings tombstone runs BACKUP then
 * TARGET as two jobs. */
typedef enum {
	AIUS_SD_OVERLAY_DELETE_TARGET = 0,
	AIUS_SD_OVERLAY_DELETE_BACKUP = 1
} AiusSdOverlayDeleteStep;

typedef struct {
	AiusSdOverlayJobKind kind;
	AiusSdOverlayOrder order;           /* COPY_BEFORE_WRITE only */
	AiusSdOverlayDeleteStep delete_step; /* DELETE only */
	bool skip_backup;      /* COPY_BEFORE_WRITE: restore-created, unchanged version: target only */
	bool require_existing; /* always false: a missing file is the state a DELETE asks for (S2) */
	uint8_t slot;          /* the store entry pinned by _flush_begin */
	uint32_t commit_seq;   /* the pinned version (FR-S4) */
	uint32_t length;       /* bytes the passes write (0 for DELETE) */
	uint32_t file_crc;     /* accept-time CRC the read-back is compared against (S3) */
	char target_path[AIUS_SD_OVERLAY_PATH_STORAGE]; /* absolute, "/aius/..." */
	char backup_path[AIUS_SD_OVERLAY_PATH_STORAGE]; /* absolute, "" when none */
} AiusSdOverlayJob;

/* How a job ended, as the platform reports it. */
typedef enum {
	AIUS_SD_OVERLAY_JOB_OK = 0,
	AIUS_SD_OVERLAY_JOB_FAILED,     /* card or read-back failure: counts a try, S5 backoff */
	AIUS_SD_OVERLAY_JOB_CARD_GONE,  /* card-gone fault: sets held, no try counted */
	AIUS_SD_OVERLAY_JOB_CHUNK_CRC   /* PSRAM chunk fault on the input: no card failure, Q2's path */
} AiusSdOverlayJobOutcome;

/* What the mount restore does (H10, FR-S5). */
typedef enum {
	AIUS_SD_OVERLAY_RESTORE_KEEP = 0, /* target valid: no backup read */
	AIUS_SD_OVERLAY_RESTORE_RESTORE,  /* backup parsed: apply and put it, restore-created */
	AIUS_SD_OVERLAY_RESTORE_FAULT,    /* backup bytes reached the parser and were refused */
	AIUS_SD_OVERLAY_RESTORE_NOTHING   /* backup load failed before its parser */
} AiusSdOverlayRestoreDecision;

typedef enum {
	AIUS_SD_OVERLAY_BACKUP_NOT_READ = 0, /* target valid, or no backup load ran */
	AIUS_SD_OVERLAY_BACKUP_PARSED,       /* bytes reached the parser and passed */
	AIUS_SD_OVERLAY_BACKUP_REFUSED,      /* bytes reached the parser and were refused */
	AIUS_SD_OVERLAY_BACKUP_LOAD_FAILED   /* missing, empty, oversized or read fault */
} AiusSdOverlayBackupResult;

typedef enum {
	AIUS_SD_OVERLAY_EVENT_CRC_DROP = 0 /* Q2: a faulted preset or www entry dropped */
} AiusSdOverlayEvent;

/* Q2 re-put callback: serialise the running settings for `file` into a
 * platform buffer and return it through `bytes` / `length`. Returns false to
 * retry at the next service step (the dispatcher body is borrowed only while
 * the HTTP transaction is idle, D1). The buffer need stay valid only until
 * the callback's caller returns. */
typedef bool (*AiusSdOverlayReputFn)(void *context,
                                     AiusSdOverlaySettingsFile file,
                                     const uint8_t **bytes, uint32_t *length);
/* Event log hook (`path` relative to /aius/, NUL-terminated). */
typedef void (*AiusSdOverlayEventFn)(void *context, AiusSdOverlayEvent event,
                                     const char *path);

/* The platform binding, normally a const in flash. */
typedef struct {
	AiusWebCache *cache;          /* the one store */
	AiusSdOverlayReputFn reput;   /* required */
	AiusSdOverlayEventFn event;   /* may be NULL */
	void *context;
} AiusSdOverlayPlatform;

/* The overlay's DTCM state (H19's sd_flush counters, scheduler and pass
 * tracking). Treat as opaque; read through _status. */
typedef struct {
	const AiusSdOverlayPlatform *platform;
	uint32_t flushed;
	uint32_t failures;
	uint32_t crc_drops;
	uint32_t max_settings_latency_ms;
	uint32_t sync_drain_ms;
	uint32_t sync_burst_start_ms;
	uint8_t last_fault;    /* AiusSdOverlayJobOutcome of the last failure */
	uint8_t flushing_slot; /* AIUS_WEB_CACHE_NO_ENTRY when no job */
	uint8_t state;         /* held, re-put pending (dsp, config), usage refresh due, burst */
	uint8_t pass;          /* current job's pass tracking */
} AiusSdOverlay;

/* The sd_flush status object (also in /api/status/brief). */
typedef struct {
	uint32_t dirty;      /* DIRTY entries */
	uint32_t flushing;   /* FLUSHING entries and running tombstones */
	uint32_t tombstones; /* TOMBSTONE entries */
	uint32_t oldest_dirty_age_ms;
	uint32_t flushed;
	uint32_t failures;
	uint32_t crc_drops;
	uint32_t max_settings_latency_ms;
	uint32_t sync_drain_ms;
	uint8_t last_fault;
	bool held;
	bool drained;
} AiusSdOverlayStatus;

/* Binds `platform` and zeroes the counters; `held` true for a board booted
 * without a card (Q1). */
void aius_sd_overlay_init(AiusSdOverlay *overlay,
                          const AiusSdOverlayPlatform *platform, bool held);

/* SETTINGS for "dsp.json", "config.json" and "www/presets/<name>.json" (a
 * non-empty name, no further '/', all case-folded, N1), else SYNC: any other
 * name under "www/presets/" flushes as today's REPLACE (N5). */
AiusSdOverlayClass aius_sd_overlay_classify(const char *path,
                                            size_t path_length);

/* Writes `path`'s absolute fixed backup path (H4) into `out` with its NUL.
 * False for a sync path, or a settings path whose backup cannot be formed
 * (a preset not named "<name>.json", or one that would not fit `capacity`,
 * FR-S9). */
bool aius_sd_overlay_backup_path(const char *path, size_t path_length,
                                 char *out, size_t capacity);

/* One-shot accept (dsp.json, config.json, the restore, the Q2 re-put): a put
 * on `path`, latest wins (a put on an existing path keeps its safe copy,
 * FR2-S1). A settings preset whose backup path cannot be formed is INVALID. */
AiusSdOverlayPutResult aius_sd_overlay_put(AiusSdOverlay *overlay,
                                           const char *path,
                                           size_t path_length,
                                           const uint8_t *bytes,
                                           uint32_t length, uint32_t now_ms);

/* The H10 restore's accept: as _put, marked restore-created, so with no
 * earlier pending entry its safe copy starts at backup and its flush, while
 * the version is unchanged, writes the target only (FR-B1). */
AiusSdOverlayPutResult aius_sd_overlay_put_restore(AiusSdOverlay *overlay,
                                                   const char *path,
                                                   size_t path_length,
                                                   const uint8_t *bytes,
                                                   uint32_t length,
                                                   uint32_t now_ms);

/* Streamed accept (the AIUS_SD_INSTALL_STORE sink, one at a time): _begin
 * reserves a PUTTING entry for `length` bytes (BUSY when full, Q4), _chunk
 * takes each 512 B (or tail) chunk, _commit makes it DIRTY at `now_ms`,
 * _abort frees it (client drop, chunk CRC fault, orphan sweep, NS5). The
 * platform stops an in-flight fill of the path before _begin (B1, NS2).
 * A www/presets/<name>.json whose case-folded name is not among the counted
 * presets (newest entry VALID, FILLING, DIRTY or FLUSHING) answers LIMIT at 16
 * or more counted, else BUSY while `presets_complete` is false (the presets
 * walk has not finished, so the count may still grow); an overwrite of a
 * counted name proceeds. */
AiusSdOverlayPutResult aius_sd_overlay_put_begin(AiusSdOverlay *overlay,
                                                 const char *path,
                                                 size_t path_length,
                                                 uint32_t length,
                                                 bool presets_complete);
AiusSdOverlayPutResult aius_sd_overlay_put_chunk(AiusSdOverlay *overlay,
                                                 const uint8_t *bytes,
                                                 uint32_t length);
AiusSdOverlayPutResult aius_sd_overlay_put_commit(AiusSdOverlay *overlay,
                                                  uint32_t now_ms);
void aius_sd_overlay_put_abort(AiusSdOverlay *overlay);

/* Accepts a DELETE of `path` as a tombstone: the path misses in GET and
 * listings at once. BUSY when no entry is free and the path holds none to
 * reuse (N3). OK whether or not the card holds the file (S2). */
AiusSdOverlayPutResult aius_sd_overlay_tombstone(AiusSdOverlay *overlay,
                                                 const char *path,
                                                 size_t path_length,
                                                 uint32_t now_ms);

/* True while the store holds `path` DIRTY, FLUSHING or TOMBSTONE (PUTTING
 * is not shadowed, D2): the reload, refill and publish_config skip it. */
bool aius_sd_overlay_shadows(const AiusSdOverlay *overlay, const char *path,
                             size_t path_length);

/* True only when no entry is DIRTY, FLUSHING or TOMBSTONE (FR-S8) and no Q2
 * re-put is owed (D6, H9, CR3-B1): the OTA and reboot gate. Held entries
 * keep it false. */
bool aius_sd_overlay_drained(const AiusSdOverlay *overlay);

/* The scheduler: when no job runs, not held, picks the next due entry
 * (debounced, next_try_ms passed, not CRC_FAULTED; settings before sync,
 * oldest accept first), pins it with _flush_begin and fills `job`. False
 * when nothing is due. The platform starts the job (behind its own fill
 * cancel, FIFO defer and freeze gates) and must end it with _job_end. */
bool aius_sd_overlay_next_job(AiusSdOverlay *overlay, uint32_t now_ms,
                              AiusSdOverlayJob *job);

/* The scheduler-due predicate: true exactly when _next_job would return a
 * job at `now_ms` (same held, running-job, debounce, S5 backoff,
 * CRC_FAULTED and owed-target rules, one shared pick), so the platform
 * cancels an in-flight fill only for a job that will start. Changes no
 * overlay state; the pick's on-chip CRC check may flag a failing entry
 * CRC_FAULTED in the store, as _next_job's would. */
bool aius_sd_overlay_job_ready(const AiusSdOverlay *overlay, uint32_t now_ms);

/* The job's input (FR-S3): copies the pinned version's bytes at `offset`
 * (a multiple of 512) into `dst`, up to `capacity`, through
 * aius_web_cache_copy_chunk, each chunk checked against its accept-time CRC
 * (S3). Sets `*bytes`. False on a chunk CRC fault: the platform fails the
 * job with AIUS_SD_OVERLAY_JOB_CHUNK_CRC before any WRITE of it. */
bool aius_sd_overlay_job_input(AiusSdOverlay *overlay, uint32_t offset,
                               uint8_t *dst, uint32_t capacity,
                               uint32_t *bytes);

/* A pass of the running copy-before-write job CLOSEd successfully: `which`
 * becomes the path's safe copy, written on its newest pending entry (E1). */
void aius_sd_overlay_note_pass_closed(AiusSdOverlay *overlay,
                                      AiusWebCacheSafeCopy which);

/* The running job's target read-back: pass makes the safe copy target, a
 * mismatch makes it backup (FR-B1). */
void aius_sd_overlay_note_readback(AiusSdOverlay *overlay, bool ok);

/* Ends the running job. OK: the entry turns clean (or is retired behind a
 * newer version), tries reset; a backup DELETE leaves the tombstone for its
 * target DELETE and makes the safe copy target (G1). FAILED: a try counted,
 * next_try_ms 1 s ahead doubling to 60 s (S5), unless held. CARD_GONE: held
 * set, the entry stays pending with its safe copy, no try. CHUNK_CRC: the
 * entry is flagged CRC_FAULTED for the service step (Q2), no card failure.
 * A burst's last flush arms one USAGE_REFRESH. */
void aius_sd_overlay_job_end(AiusSdOverlay *overlay,
                             AiusSdOverlayJobOutcome outcome, uint32_t now_ms);

/* The card is gone (card-gone fault, probe no-card verdict, SD mode change):
 * sets held. The platform separately invalidates the store's clean entries. */
void aius_sd_overlay_note_card_lost(AiusSdOverlay *overlay);

/* A card mounted: clears held. */
void aius_sd_overlay_note_mounted(AiusSdOverlay *overlay, uint32_t now_ms);

bool aius_sd_overlay_held(const AiusSdOverlay *overlay);

/* The service step (once per SD checkpoint): handles CRC_FAULTED entries
 * and owes Q2 re-puts through platform->reput as puts on the key's own path
 * (in place over its own unpinned entry when the manifest is full, CR3-B1),
 * retried each step until they land, drained false meanwhile (D6): a dirty
 * dsp.json / config.json with a chunk fault is kept until its re-put
 * replaces it, carrying its safe copy (NS6); a dirty entry whose own CRC
 * failed (ENTRY_FAULTED, path untrusted) is dropped first (CR2-N1) and
 * re-puts both. A preset or www entry, an entry-faulted one and every
 * tombstone are dropped, count in crc_drops and log an event: a faulted entry's
 * path is never trusted for a card operation, so a dropped tombstone's
 * delete is lost and the card keeps the file. Also the burst bookkeeping.
 * Never inside a lookup. */
void aius_sd_overlay_service(AiusSdOverlay *overlay, uint32_t now_ms);

/* True once per flush burst, after its last flush: the platform queues one
 * USAGE_REFRESH. Consumes the request. */
bool aius_sd_overlay_take_usage_refresh(AiusSdOverlay *overlay);

/* Fills the sd_flush status object. */
void aius_sd_overlay_status(const AiusSdOverlay *overlay, uint32_t now_ms,
                            AiusSdOverlayStatus *status);

/* POST /api/clear: resets max_settings_latency_ms and sync_drain_ms. */
void aius_sd_overlay_clear_stats(AiusSdOverlay *overlay);

/* The mount restore's verdict (H10, FR-S5, FR-S10): target valid gives KEEP
 * whatever `backup`; else PARSED gives RESTORE, REFUSED gives FAULT, and
 * LOAD_FAILED or NOT_READ give NOTHING. Pure. */
AiusSdOverlayRestoreDecision aius_sd_overlay_restore_decision(
	bool target_valid, AiusSdOverlayBackupResult backup);

#ifdef __cplusplus
}
#endif

#endif
