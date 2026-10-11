/* Optical application dispatch.
 *
 * One bounded decision per HTTP route and per USB command. The dispatcher is
 * pure: it parses nothing from the wire, opens no socket, touches no SD
 * primitive, feeds no watchdog and never delays. Every fact it needs about the
 * running system arrives through the AiusOpticalApp callback table, and every
 * effect it requests leaves as either an AiusHttpApplicationResponse (immediate
 * body, or a portable AiusSdJobRequest for the HTTP transaction to run) or an
 * AiusUsbApplicationResponse.
 *
 * Phase 11 lands the module and its `optical-app` suite only. Nothing in
 * production constructs it yet; Phases 12 and 14 wire it into the Teensy
 * runtime and delete the legacy handlers in optical_main.cpp.
 */

#ifndef AIUS_OPTICAL_APP_H
#define AIUS_OPTICAL_APP_H

#include "aius_http.h"
#include "aius_http_transaction.h"
#include "aius_sd.h"
#include "aius_usb_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest USB payload the control runtime accepts (aius_usb_control.c
 * runtime_success_valid). One authority: the control library's own bound. */
#define AIUS_OPTICAL_APP_USB_PAYLOAD_MAX AIUS_USB_RUNTIME_MAX_PAYLOAD
#define AIUS_OPTICAL_APP_USB_PAYLOAD_STORAGE (AIUS_OPTICAL_APP_USB_PAYLOAD_MAX + 1u)
#define AIUS_OPTICAL_APP_REPLAY_ID_STORAGE (AIUS_HTTP_MAX_REPLAY_ID + 1u)

/* Probe-bin cap for POST /api/measure/start. The dispatcher deliberately does
 * not include aius_measure.h (the dispatch seam has no DSP dependency), so
 * optical_main.cpp static_asserts this equals AIUS_MEASURE_PROBES_MAX. */
#define AIUS_OPTICAL_APP_MEASURE_PROBES_MAX 8u

/* Recorder trigger-mask bits for PUT /api/recorder/config. The dispatcher
 * deliberately does not include aius_recorder.h (same rule as the measure
 * mirror above); optical_main.cpp static_asserts these equal the
 * AIUS_RECORDER_MASK_* values. */
#define AIUS_OPTICAL_APP_RECORDER_MASK_OVERRUN (1u << 0)
#define AIUS_OPTICAL_APP_RECORDER_MASK_UNDERRUN (1u << 1)
#define AIUS_OPTICAL_APP_RECORDER_MASK_CARRIER_LOSS (1u << 2)
#define AIUS_OPTICAL_APP_RECORDER_MASK_LIMITER_GR (1u << 3)
#define AIUS_OPTICAL_APP_RECORDER_MASK_GOVERNOR_TRIM (1u << 4)
#define AIUS_OPTICAL_APP_RECORDER_MASK_MISSED_FILL (1u << 5)
#define AIUS_OPTICAL_APP_RECORDER_MASK_CRC_REJECT (1u << 6)

/* Three-way result for the recorder data read: the dispatcher must be able to
 * tell "your request is malformed" (400: misaligned or out-of-extent offset,
 * zero length) from "the ring is not readable" (409: not frozen, or a stale
 * freeze_generation after a thaw). */
typedef enum {
	AIUS_OPTICAL_APP_RECORDER_READ_OK,
	AIUS_OPTICAL_APP_RECORDER_READ_INVALID,
	AIUS_OPTICAL_APP_RECORDER_READ_REFUSED
} AiusOpticalAppRecorderReadResult;

/* Directory index -- GET /sd/<dir>/ ---------------------------------------
 *
 * Longest entry name the index lists. The /sd/ download route accepts a path
 * component of at most 32 bytes drawn from [A-Za-z0-9._-], so a longer or
 * differently spelled name is one the link a listing would draw could not
 * fetch. The index therefore lists exactly the set /sd/ can serve and reports
 * everything else as hidden rather than drawing a dead link -- which also means
 * no byte that could close an HTML attribute ever reaches the body. */
#define AIUS_OPTICAL_APP_INDEX_NAME_MAX 32u
#define AIUS_OPTICAL_APP_INDEX_NAME_STORAGE (AIUS_OPTICAL_APP_INDEX_NAME_MAX + 1u)

/* Hard cap on one listing, and the whole bounding argument for a route that is
 * otherwise unbounded by nature.
 *
 * 64 entries * the 109-byte worst-case row (a 32-byte name twice over plus a
 * 20-digit size) + 216 bytes of framing is 7,192 bytes, under
 * AIUS_HTTP_MAX_RESPONSE (8,192), so even a full listing frames as one ordinary
 * response and never needs the streaming path. It also bounds the
 * epoch: the application performs at most this many entry reads plus the scan
 * bound below, instead of walking a directory of unknown size. Past the cap the
 * listing stops and says so in its own footer -- see AiusOpticalAppDirBatch. */
#define AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES 64u

/* Bound on the raw directory reads one read_directory call may perform. The
 * application stops at this many reads even when it has filled fewer than
 * `capacity` entries (a directory whose first 128 names are all unservable, for
 * instance) and reports `more`. One epoch's enumeration is bounded by this
 * number, not by the directory's size. */
#define AIUS_OPTICAL_APP_INDEX_SCAN_MAX 128u

/* Smallest response-body buffer a dispatcher accepts. Every dynamically
 * formatted body the dispatcher writes itself fits inside this except the OTA
 * status document, whose worst case (two 32-byte identity strings, a 32-byte
 * fingerprint and a 20-digit build_utc) is ~370 bytes; on a dispatcher this
 * small that document becomes the same deterministic 500 response_too_large any
 * other over-long body does, and the composed runtime hands over
 * AIUS_HTTP_MAX_RESPONSE. The serializer callbacks receive whatever the caller
 * supplied. */
#define AIUS_OPTICAL_APP_MIN_BODY 256u

/* Result of handing a request body to the application for storage. Acceptance
 * and the response are separate facts: ACCEPTED says the application validated
 * the body and staged it in RAM but has written nothing, so the dispatcher owns
 * persisting the raw bytes and the client's 200 is the SD job's -- withheld
 * until the write commits, with a failed write surfacing as 500 sd_commit /
 * 507 sd_write. OK says the application handled the body completely and nothing
 * needs writing, so the 200 is immediate. BUSY means a storage transaction is
 * already in flight, STALE that the body's base_generation no longer matches
 * the running configuration, and INVALID that the body did not parse; all three
 * answer immediately and reach the card not at all. */
typedef enum {
	AIUS_OPTICAL_APP_STORE_OK,
	AIUS_OPTICAL_APP_STORE_ACCEPTED,
	AIUS_OPTICAL_APP_STORE_INVALID,
	AIUS_OPTICAL_APP_STORE_STALE,
	AIUS_OPTICAL_APP_STORE_BUSY
} AiusOpticalAppStoreResult;

/* CARRIER_PRESENT is a start-only refusal and deliberately its own value: a live
 * S/PDIF carrier always owns the audio path, so a replay cannot start against
 * one. It is distinct from FAILED because a client must be able to tell "that id
 * is unusable" from "the input is live" -- the first is retried with a different
 * id, the second only once the carrier goes away. Appended so no existing value
 * moves. */
typedef enum {
	AIUS_OPTICAL_APP_REPLAY_OK,
	AIUS_OPTICAL_APP_REPLAY_NOT_FOUND,
	AIUS_OPTICAL_APP_REPLAY_ACTIVE,
	AIUS_OPTICAL_APP_REPLAY_FAILED,
	AIUS_OPTICAL_APP_REPLAY_CARRIER_PRESENT,
	/* Phase 4g (owner, audit finding 2): a replay start while the web-cache
	 * fill walk is incomplete; the route answers 503 fill_busy. */
	AIUS_OPTICAL_APP_REPLAY_FILL_BUSY
} AiusOpticalAppReplayResult;

/* One directory entry, as the application read it off the card. `name` is the
 * base name only -- no directory part, NUL-terminated, and truncated by the
 * dispatcher at AIUS_OPTICAL_APP_INDEX_NAME_MAX before it is read. */
typedef struct {
	uint64_t size_bytes;
	char name[AIUS_OPTICAL_APP_INDEX_NAME_STORAGE];
	bool is_directory;
} AiusOpticalAppDirEntry;

/* One bounded batch of directory entries. The dispatcher owns `entries` and
 * sets `entries`/`capacity` before the call; the application fills `count`
 * entries and reports what it left behind. There is exactly one batch per
 * request: this is a hard cap, not a pagination cursor, so `more` is the honest
 * statement that the listing stopped early rather than an offset to resume
 * from.
 *
 * `hidden` counts entries the application skipped because it could not
 * represent them -- a name longer than AIUS_OPTICAL_APP_INDEX_NAME_MAX, most
 * commonly. The dispatcher adds its own rejections to that count, so a name it
 * would not draw a link for is reported rather than silently dropped. `.` and
 * `..` are not entries in this sense and are dropped without counting.
 *
 * `more` is set when the card holds further entries beyond `count`, whether the
 * call stopped at `capacity` or at AIUS_OPTICAL_APP_INDEX_SCAN_MAX reads. */
typedef struct {
	AiusOpticalAppDirEntry *entries;
	size_t capacity;
	size_t count;
	size_t hidden;
	bool more;
} AiusOpticalAppDirBatch;

typedef enum {
	AIUS_OPTICAL_APP_DIR_OK,
	/* Nothing exists at the path. */
	AIUS_OPTICAL_APP_DIR_NOT_FOUND,
	/* Something exists, but it is a file. */
	AIUS_OPTICAL_APP_DIR_NOT_DIRECTORY,
	/* The card refused the enumeration. */
	AIUS_OPTICAL_APP_DIR_FAILED
} AiusOpticalAppDirResult;

/* The application seam. Every entry is required except `read_directory`;
 * aius_optical_app_init() rejects an otherwise incomplete table so a missing
 * callback can never surface as a null call at dispatch time. Serializers
 * return the written length, or 0 for "does not fit" -- the dispatcher turns
 * that into a deterministic 500. */
typedef struct {
	void *context;

	bool (*sd_ready)(void *context);
	bool (*sd_busy)(void *context);
	bool (*auth_matches)(void *context, const uint8_t *token, size_t length);

	size_t (*write_home)(void *context, uint8_t *body, size_t capacity);
	size_t (*write_status)(void *context, uint8_t *body, size_t capacity);
	size_t (*write_events)(void *context, uint32_t cursor, uint8_t *body,
	                       size_t capacity);
	size_t (*write_dsp_config)(void *context, uint8_t *body, size_t capacity);
	size_t (*write_meters)(void *context, uint8_t *body, size_t capacity);
	size_t (*write_replay_status)(void *context, uint8_t *body, size_t capacity);
	size_t (*write_replay_list)(void *context, uint8_t *body, size_t capacity);

	/* Validate and stage one complete request body; write nothing. On
	 * ACCEPTED the dispatcher persists those exact bytes to /aius/config.json
	 * and /aius/dsp.json respectively, so the stored file re-parses through the
	 * same parser at the next boot. */
	AiusOpticalAppStoreResult (*store_public_config)(void *context,
	                                                 const uint8_t *body,
	                                                 size_t length);
	AiusOpticalAppStoreResult (*store_dsp_config)(void *context,
	                                              const uint8_t *body,
	                                              size_t length);

	AiusOpticalAppReplayResult (*replay_start)(void *context, const char *id,
	                                           bool loop);
	AiusOpticalAppReplayResult (*replay_delete)(void *context, const char *id);
	void (*replay_stop)(void *context);

	void (*set_test_running)(void *context, bool running);
	void (*clear_counters)(void *context);
	/* Records the reboot request; the dispatcher itself never resets or delays.
	 * The runtime performs the reset once the HTTP transaction has drained to
	 * AIUS_HTTP_TX_IDLE -- i.e. the response was written onto the socket and the
	 * connection closed -- under a bounded ceiling. Recording is NOT "the
	 * response has been written": at the moment this callback returns the
	 * response has not even been formatted. */
	void (*request_reboot)(void *context);

	/* ID/AUTH payload for one USB command, NUL-terminated into `payload`.
	 * Returns the length written, or 0 when it does not fit. */
	size_t (*write_usb_payload)(void *context, AiusUsbCommandKind kind,
	                            char *payload, size_t capacity);
	/* Per-upload entropy for temporary/backup file names. */
	uint32_t (*upload_id)(void *context);

	/* Reads one bounded batch of directory entries from `path` (an absolute
	 * card path with no trailing slash, e.g. "/aius/www" or "/aius/www/sub").
	 *
	 * This is the ONE optional entry in the table, and deliberately so: the
	 * dispatcher owns the route and its bound, while the enumeration itself is
	 * card access only the target adapter can perform. A null callback leaves
	 * GET /sd/<dir>/ answering a deterministic 503 sd_index_unavailable instead
	 * of refusing the whole table at init -- so a runtime that has not wired it
	 * keeps every other route working, and wiring it is purely additive.
	 *
	 * It is LAST for the same reason. optical_main.cpp's g_aius_optical_app is
	 * a positional aggregate initializer, so a new member anywhere else shifts
	 * every entry below it; appending leaves that table correct and value-
	 * initializes this slot to null. A new initializer belongs at the end of
	 * that list, never in the middle.
	 *
	 * The dispatcher calls this at most once per request, and only after
	 * sd_ready() and !sd_busy(), so the enumeration can never run while an SD
	 * job holds the volume. The implementation must not scan more than
	 * AIUS_OPTICAL_APP_INDEX_SCAN_MAX raw entries, must not retain the
	 * directory handle beyond the call, and must leave `batch->entries` and
	 * `batch->capacity` alone. */
	AiusOpticalAppDirResult (*read_directory)(void *context, const char *path,
	                                          AiusOpticalAppDirBatch *batch);

	/* The measurement loopback (item 3). Appended AFTER read_directory per the
	 * append-only rule above, but both are REQUIRED entries -- app_complete()
	 * refuses a table without them, so a positional initializer that stops at
	 * read_directory fails init rather than dispatching into a null call.
	 *
	 * write_measure serializes the session (state machine, figures, floors,
	 * vector id) for the public GET. measure_start arms a capture against the
	 * named replay vector with the per-channel fit frequencies and up to
	 * AIUS_OPTICAL_APP_MEASURE_PROBES_MAX frequency-response probe bins; false
	 * means refused (a live capture, or parameters the analysis rejected) and
	 * the dispatcher answers 409. */
	size_t (*write_measure)(void *context, uint8_t *body, size_t capacity);
	bool (*measure_start)(void *context, const char *vector_id,
	                      uint32_t f0_l_hz, uint32_t f0_r_hz,
	                      const uint32_t *probes_hz, size_t probe_count);

	/* The PSRAM flight recorder, appended per the append-only rule; all
	 * six are REQUIRED (app_complete refuses a table without them).
	 * write_recorder serializes status + the frozen header.
	 * recorder_freeze / recorder_thaw / recorder_config return false when
	 * the state machine refuses (absent board, wrong state, capture in
	 * flight) -- the dispatcher answers 409; body-parse failures never
	 * reach recorder_config. recorder_read resolves one bounded frozen
	 * chunk into a pointer whose storage (the PSRAM ring) outlives the
	 * response borrow: the ring is only readable FROZEN, and a thaw is
	 * necessarily a later transaction under the single-transaction HTTP
	 * model.
	 *
	 * recorder_read addresses one bank slot: has_slot true reads
	 * slots[slot] (out-of-range slot -> READ_INVALID -> 400); has_slot
	 * false reads the NEWEST FROZEN slot, and answers READ_REFUSED (409)
	 * when no slot is frozen. It is deliberately NOT the active slot: the
	 * bank advances `active` in the same drain that freezes a slot, so an
	 * "active slot" default would refuse every fresh capture the moment it
	 * became readable. On READ_OK it writes the resolved slot index to
	 * *resolved_slot AND takes that slot's read lease
	 * (aius_recorder_bank_read_acquire), so the FROZEN slot cannot be
	 * recycled out from under the borrowed pointer while the response
	 * streams. The lease is dropped by recorder_read_release, which the
	 * dispatcher calls from the response's own completion hook (see the
	 * dispatcher struct's borrow note). resolved_slot / the acquire are set
	 * only on READ_OK; the other two verdicts borrow nothing and take no
	 * lease. */
	size_t (*write_recorder)(void *context, uint8_t *body, size_t capacity);
	bool (*recorder_freeze)(void *context);
	bool (*recorder_thaw)(void *context);
	bool (*recorder_config)(void *context, uint32_t mask,
	                        float gr_threshold_db, uint32_t gr_hold_blocks,
	                        uint32_t postroll_blocks);
	AiusOpticalAppRecorderReadResult (*recorder_read)(
		void *context, bool has_slot, uint32_t slot,
		uint32_t offset_bytes, uint32_t length_bytes,
		uint32_t generation, const uint8_t **data, uint32_t *length,
		uint32_t *resolved_slot);
	void (*recorder_read_release)(void *context, uint32_t slot);
	/* POST /api/sd/mode (aius-sample-rate-following.md Phase 4e, E3): selects
	 * DMA (true) or FIFO (false) SDIO for the next mount. Returns false when a
	 * file is open, answered 503 sd_busy with the mode unchanged. Optional and
	 * last in the table: a table without it answers 404 on the route. */
	bool (*sd_set_mode)(void *context, bool dma);
	/* GET /api/status/brief (Phase 4f F3): exactly the keys the editor's flow
	 * ends read, under the full body's names and nesting. Optional: a table
	 * without it answers 404 on the route. */
	size_t (*write_status_brief)(void *context, uint8_t *body,
	                             size_t capacity);
	/* Phase 4f F5 (optional pair): called around every HTTP route dispatch,
	 * so the composition meters each dispatch's longest stretch between
	 * yield points and charges it to the route's admission class. */
	void (*dispatch_begin)(void *context);
	void (*dispatch_end)(void *context, AiusHttpRoute route,
	                     AiusHttpMethod method);
} AiusOpticalApp;

/* --------------------------------------------------------------------------
 * Admission (aius-sample-rate-following.md Phase 4f, F1, F2, F8, F9, F11, F12)
 *
 * Portable decision half of the HTTP admission hook. The composition root
 * supplies one AiusOpticalAdmitSample per decision (live_owns_ring() and the
 * ring level, the SD mode and millis()) and maps the verdicts onto the
 * runtime seam's AiusHttpAdmitVerdict. Nothing here reads a clock or a ring.
 * ------------------------------------------------------------------------ */

/* Route classes (F1) then the per-mode SD-start windows (F2). Every other
 * route is admitted whatever the margin (F13). */
typedef enum {
	AIUS_OPTICAL_ADMIT_STATUS = 0,     /* GET /api/status */
	AIUS_OPTICAL_ADMIT_BRIEF,          /* GET /api/status/brief */
	AIUS_OPTICAL_ADMIT_METERS,         /* GET /api/meters */
	AIUS_OPTICAL_ADMIT_EVENTS,         /* GET /api/events */
	AIUS_OPTICAL_ADMIT_SETTINGS,       /* PUT /api/dsp/config, PUT /api/config */
	AIUS_OPTICAL_ADMIT_DIRECTORY,      /* GET /api/replays */
	AIUS_OPTICAL_ADMIT_SD_START_DMA,
	AIUS_OPTICAL_ADMIT_SD_START_FIFO,
	AIUS_OPTICAL_ADMIT_CLASSES,
	AIUS_OPTICAL_ADMIT_NONE = AIUS_OPTICAL_ADMIT_CLASSES
} AiusOpticalAdmitClass;

#define AIUS_OPTICAL_ADMIT_ROUTE_CLASSES 6u
#define AIUS_OPTICAL_ADMIT_SLOTS 5u          /* four completed buckets + current */
#define AIUS_OPTICAL_ADMIT_BUCKET 16u        /* dispatches per bucket */
#define AIUS_OPTICAL_ADMIT_UNDERRUN_FRAMES 128
#define AIUS_OPTICAL_ADMIT_GUARD_FRAMES 48
#define AIUS_OPTICAL_ADMIT_FRAMES_PER_MS 48u
#define AIUS_OPTICAL_ADMIT_SD_DEFER_MS 500u  /* F9 */
#define AIUS_OPTICAL_ADMIT_BUCKET_MS 2000u   /* F12: a bucket's wall-time span */

typedef enum {
	AIUS_OPTICAL_ADMIT_VERDICT_ADMIT = 0,
	AIUS_OPTICAL_ADMIT_VERDICT_SHED,
	AIUS_OPTICAL_ADMIT_VERDICT_DEFER
} AiusOpticalAdmitVerdict;

/* F12: a bucketed maximum, 16 B. slot[0..3] are the maxima of the last four
 * completed buckets, slot[4] the current bucket's; the cost is the largest
 * slot. A bucket closes after 16 samples or 2,000 ms of millis() since it
 * opened, whichever first (F-B1): a full bucket is retired lazily by the next
 * sample, and every whole 2,000 ms that passes retires the current bucket,
 * empty or not, at the class's next sample or consult. A completed bucket
 * replaces the oldest. So an outlier stays the cost for at least 64 and at
 * most 79 further samples (gone at the 80th) or about 8 to 10 s of wall time,
 * whichever comes first, whether the class is admitted or shed (a SHED adds
 * no sample); a class's first measurement, which replaces the seed in all
 * five slots, is the same as an outlier at a bucket's first position (gone
 * at the 80th). A window whose buckets all age out empty returns to its seed
 * (F8, R4-S1: never judged against 0). While `seeded` is set all five slots
 * hold the class seed, the window does not age, and the first sample
 * overwrites all five (F8). */
typedef struct {
	uint16_t slot[AIUS_OPTICAL_ADMIT_SLOTS];
	uint8_t count;   /* bits 0-4: samples in the current bucket (0..16);
	                  * bits 5-6: the oldest completed slot */
	uint8_t seeded;
	uint32_t opened_ms; /* millis() when the current bucket opened */
} AiusOpticalCostWindow;

typedef struct {
	AiusOpticalCostWindow window[AIUS_OPTICAL_ADMIT_CLASSES];
	uint32_t shed[AIUS_OPTICAL_ADMIT_CLASSES]; /* SHED verdicts per class */
	uint32_t dispatch_max_us;   /* every route's longest stretch (F13) */
	/* Held decisions (F9): each DEFER of an HTTP SD start and each hold of a
	 * root job's start. Not an epoch count: one epoch can add two (an HTTP
	 * DEFER and a root hold in the same epoch). */
	uint32_t sd_fifo_defers;
	uint32_t defer_started_ms;  /* F9's millis() timer */
	uint8_t defer_timing;
	/* F-B2: once-per-boot latch, set when bring-up's first LOAD_DSP step
	 * ends (any outcome). Root holds wait for it; init clears it, clear
	 * keeps it. Sits in the struct's tail padding. */
	uint8_t bringup_loaded;
} AiusOpticalAdmission;

typedef struct {
	bool live_owns_ring;   /* the ring's gate flag set with the carrier locked */
	bool fifo_mode;        /* the mounted SD mode is FIFO */
	uint32_t level_frames; /* transmit ring level */
	uint32_t now_ms;
} AiusOpticalAdmitSample;

/* Boot: every window holds its declared seed (in flash), counters zero. */
void aius_optical_admission_init(AiusOpticalAdmission *admission);
/* POST /api/clear: shed counts, dispatch_max_us and sd_fifo_defers reset; the
 * cost windows and a running F9 timer are kept (F8, R4-S1). */
void aius_optical_admission_clear(AiusOpticalAdmission *admission);
AiusOpticalAdmitClass aius_optical_admit_route_class(AiusHttpRoute route,
                                                     AiusHttpMethod method);
/* One sample (us, saturating at 65,535) into a class window at millis()
 * now_ms; the window first ages to now_ms. */
void aius_optical_admission_record(AiusOpticalAdmission *admission,
                                   AiusOpticalAdmitClass klass, uint32_t us,
                                   uint32_t now_ms);
/* After a route dispatch: its class window and dispatch_max_us. */
void aius_optical_admission_note_dispatch(AiusOpticalAdmission *admission,
                                          AiusHttpRoute route,
                                          AiusHttpMethod method, uint32_t us,
                                          uint32_t now_ms);
/* F-B1: retire every bucket whose 2,000 ms have passed, in every window. The
 * consults below age their own class; a reader of cost_us() for display (the
 * status body) calls this first. */
void aius_optical_admission_age(AiusOpticalAdmission *admission,
                                uint32_t now_ms);
uint32_t aius_optical_admission_cost_us(const AiusOpticalAdmission *admission,
                                        AiusOpticalAdmitClass klass);
/* F1: true while live input owns the ring and level - 128 < cost + 48 frames
 * (48 frames per ms); always false outside ownership (F11). Reads the window
 * as it stands: the consults below age it first. */
bool aius_optical_admission_short(const AiusOpticalAdmission *admission,
                                  AiusOpticalAdmitClass klass,
                                  const AiusOpticalAdmitSample *sample);
/* ROUTE: SHED for a short class (counted), else ADMIT. */
AiusOpticalAdmitVerdict aius_optical_admission_route(
	AiusOpticalAdmission *admission, AiusHttpRoute route,
	AiusHttpMethod method, const AiusOpticalAdmitSample *sample);
/* F2: in FIFO mode, true while the FIFO SD-start window is short; counts each
 * true as one hold in sd_fifo_defers. F-B2: always false until
 * aius_optical_admission_note_bringup_loaded(), so bring-up's PROBE,
 * LOAD_CONFIG and LOAD_DSP root jobs start while live input owns the ring
 * (FIFO is the boot mode; the 2026-08-16 starvation). HTTP SD starts
 * (aius_optical_admission_sd_start) do not read the latch. */
bool aius_optical_admission_sd_fifo_defer(AiusOpticalAdmission *admission,
                                          const AiusOpticalAdmitSample *sample);
/* F-B2: bring-up's first LOAD_DSP step has ended, found or not. */
void aius_optical_admission_note_bringup_loaded(AiusOpticalAdmission *admission);
/* Round 4: whether the HTTP transaction can no longer start or finish the SD
 * write an upload staging (PUT /api/dsp/config, PUT /api/config) was armed
 * for. False while the transaction is in DISPATCH (the arming epoch, its SD
 * job not yet emitted), SD_START or SD_TRANSFER, while the seam holds a
 * deferred start (`start_held`) or an admitted start waits for the storage
 * checkpoint (`start_queued`); true otherwise (a teardown re-inited the
 * transaction, or it moved on to another request). Reads state only. */
bool aius_optical_app_sd_staging_orphaned(const AiusHttpTransaction *http,
                                          bool start_held, bool start_queued);
/* SD_START: OTA staging always ADMIT (F9); FIFO: DEFER while
 * sd_fifo_defer() holds, SHED once 500 ms have passed since the first
 * deferral; DMA: SHED when short. ADMIT and SHED stop the timer.
 * `reconsult` is true only when this consult re-issues the start the last
 * DEFER held; a first consult (a new start, or one after a network teardown
 * dropped the held start) restarts the timer (F-S2). */
AiusOpticalAdmitVerdict aius_optical_admission_sd_start(
	AiusOpticalAdmission *admission, const AiusSdJobRequest *job,
	const AiusOpticalAdmitSample *sample, bool reconsult);
/* An OTA staging chunk job: an UPLOAD whose target is the staged image. */
bool aius_optical_app_sd_job_is_ota_staging(const AiusSdJobRequest *job);

/* --------------------------------------------------------------------------
 * OTA -- staged firmware upgrade (docs/plans/aius-ota-upgrade.md, slice 3)
 * ------------------------------------------------------------------------ */

/* A SECOND, SEPARATE TABLE, and that is the design rather than an oversight.
 *
 * Two facts force it. First, AiusOpticalApp is a POSITIONAL AGGREGATE
 * INITIALIZER in optical_main.cpp, so appending a member there is not additive
 * the way the entries above claim: -Wextra makes -Wmissing-field-initializers
 * an error in every build that composes that table, so a runtime which has not
 * wired OTA would stop compiling rather than value-initialize the new slots to
 * null. Second, OTA is a COMPILE-TIME option that defaults OFF (the plan's
 * §Design constraints from the owner), so "this build has no OTA" must be a
 * state the dispatcher can HOLD, not a table it cannot construct.
 *
 * One optional sub-table, installed after init, answers both: a dispatcher
 * nobody wired is absent by construction (aius_optical_app_init zeroes it), and
 * wiring it costs the composition root one aius_optical_app_set_ota() call
 * inside its own #if. The table is all-or-nothing -- a partially filled one is
 * refused rather than dispatched into a null call.
 *
 * The dispatcher holds NO OTA state. It bounds and shapes each request, hands
 * the decision to this table and publishes the typed verdict; the session, the
 * staging medium, the digest and the key all live behind it. */

/* Container header bytes, mirrored from AIUS_OTA_HEADER_BYTES rather than
 * included: the dispatch seam has no OTA-core dependency, exactly as it has no
 * DSP or recorder dependency (see AIUS_OPTICAL_APP_MEASURE_PROBES_MAX above).
 * The composition slice static_asserts the two are equal.
 *
 * It is a ROUTE-LEVEL constant here because of the header-first rule: the first
 * chunk of an image carries the fixed-size container header, and a first chunk
 * shorter than one whole header is refused before the application is asked. */
#define AIUS_OPTICAL_APP_OTA_HEADER_BYTES 256u

/* Per-request chunk cap, mirrored from AIUS_OTA_MAX_CHUNK_BYTES (one flash
 * sector, ~80 us of SHA-256 on this part against the audio block's 10,667 us).
 * It is what keeps PUT /api/ota/image an ORDINARY BUFFERED ROUTE: 4096 is well
 * under AIUS_HTTP_MAX_BODY, so the transport needs no streaming carve-out and
 * every chunk arrives whole, self-contained and resumable -- the APPEND_PART
 * shape the replay upload already answers with next_offset. */
#define AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES 4096u

/* Width of every text field the status document publishes: target_id and
 * build_revision (AIUS_OTA_TARGET_ID_BYTES and AIUS_OTA_BUILD_REVISION_BYTES
 * are both 32) and the compiled-in key's fingerprint. */
#define AIUS_OPTICAL_APP_OTA_TEXT_MAX 32u
#define AIUS_OPTICAL_APP_OTA_TEXT_STORAGE (AIUS_OPTICAL_APP_OTA_TEXT_MAX + 1u)

/* The on-card staging file, under the /aius/ root the SD engine installs to. The
 * chunk route durably stages the whole container here (header + payload) at the
 * container file offset; the composition's activation reads it back, skipping
 * the 256-byte header. A fixed name because staging is single-image: a new offer
 * overwrites it. */
#define AIUS_OPTICAL_APP_OTA_STAGED_NAME "ota-staged.img"

/* The five verbs, in the dispatcher's own vocabulary.
 *
 * Route identity normally arrives as an AiusHttpRoute, but lib/aius_http does
 * not allocate the five /api/ota paths yet and this slice does not own that
 * library. So the verb is named here and passed in explicitly; the composition
 * slice adds AIUS_HTTP_ROUTE_OTA_* and five case labels that forward, and this
 * enum stays the dispatcher-side name either way.
 *
 * The activation-wiring slice adds three POST verbs. activate runs the flash
 * programmer; enable-activation flips the runtime latch that keeps activation
 * refused until the operator has proven the scratch-sector recovery check on
 * the bench; self-test-flash runs that proof. activation_available in the
 * status document (driven by the latch) is how a page knows whether to offer
 * the activate step. */
typedef enum {
	AIUS_OPTICAL_APP_OTA_ROUTE_NONE = 0, /* not an OTA request */
	AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,   /* GET  /api/ota          (open read) */
	AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE,    /* PUT  /api/ota/image    (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_ARM,      /* POST /api/ota/arm      (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY,   /* POST /api/ota/verify   (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_ABORT,    /* POST /api/ota/abort    (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,          /* POST /api/ota/activate          (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_ENABLE_ACTIVATION, /* POST /api/ota/enable-activation (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST,         /* POST /api/ota/self-test-flash   (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE,          /* POST /api/ota/rehearse          (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT     /* POST /api/ota/rehearse-fault    (auth) */
} AiusOpticalAppOtaRoute;

/* The upgrade lifecycle, value-for-value the AiusOtaState the core publishes.
 * Mirrored for the same reason the constants above are: the seam names the
 * states it prints without depending on the library that owns them. */
typedef enum {
	AIUS_OPTICAL_APP_OTA_STATE_IDLE = 0,
	AIUS_OPTICAL_APP_OTA_STATE_OFFERED = 1,
	AIUS_OPTICAL_APP_OTA_STATE_STAGING = 2,
	AIUS_OPTICAL_APP_OTA_STATE_STAGED = 3,
	AIUS_OPTICAL_APP_OTA_STATE_VERIFIED = 4,
	AIUS_OPTICAL_APP_OTA_STATE_ARMED = 5,
	AIUS_OPTICAL_APP_OTA_STATE_ACTIVATION_REQUESTED = 6,
	AIUS_OPTICAL_APP_OTA_STATE_FAILED = 7
} AiusOpticalAppOtaState;

/* The typed verdict every mutating OTA callback returns, and the ONE place a
 * refusal's HTTP status and wire token are allocated -- the dispatcher maps
 * each value to exactly one (status, token) pair, and the status document
 * reports a latched fault through the same map, so a verdict cannot answer one
 * name on a refusal and a different one in the document.
 *
 * The vocabulary is the OTA core's AiusOtaStatus, collapsed to what a client
 * can act on: INVALID_ARG and NO_SPACE are programming errors the seam cannot
 * receive, and CORRUPT is a persisted-record fault the application resolves
 * before it answers. RANGE keeps its own value because it is the one refusal
 * that carries a resume instruction rather than an apology. */
typedef enum {
	AIUS_OPTICAL_APP_OTA_OK = 0,
	/* The feature latch is off; nothing was mutated. 409. */
	AIUS_OPTICAL_APP_OTA_DISARMED,
	/* A legal call, illegal from the current state. 409. */
	AIUS_OPTICAL_APP_OTA_STATE_REFUSED,
	/* The chunk's offset is not the next expected offset. 409, and the body
	 * carries next_offset -- the client resumes from there, exactly as the
	 * APPEND_PART transport already teaches it to. */
	AIUS_OPTICAL_APP_OTA_RANGE,
	/* A container field, or the chunk itself, failed a validation rule. 400:
	 * resending the same bytes can never succeed. */
	AIUS_OPTICAL_APP_OTA_MALFORMED,
	/* Well-formed, but not for this build or this board (target_id mismatch,
	 * unknown signature scheme, an image this part cannot hold). 400. */
	AIUS_OPTICAL_APP_OTA_UNSUPPORTED,
	/* Staged payload digest != the header's digest. 409. */
	AIUS_OPTICAL_APP_OTA_DIGEST,
	/* The signature verifier refused the image. 409. */
	AIUS_OPTICAL_APP_OTA_SIGNATURE,
	/* The staging medium refused: busy, full, or a failed write. 507, the
	 * same answer a card that will not take an upload already produces. */
	AIUS_OPTICAL_APP_OTA_STORAGE,
	/* Activation is not enabled on this device: the runtime latch is off. 409.
	 * The operator flips it (POST /api/ota/enable-activation) only after the
	 * scratch-sector recovery check passes on the bench, so activation cannot
	 * run before that proof and cannot be reached casually. */
	AIUS_OPTICAL_APP_OTA_ACTIVATION_DISABLED,
	/* The scratch-sector self-test ran and the flash read-back did not match
	 * the programmed pattern: the driver's LUT use is not safe on this board.
	 * 500 -- the device could not complete the operation it was asked to prove. */
	AIUS_OPTICAL_APP_OTA_SELF_TEST_FAILED,
	/* The authenticated payload's image vector table is not an image linked
	 * for this device's flash window (tag, entry pointer or IVT self pointer
	 * out of bounds; the 2026-08-16 Intel-hex-as-payload shape). 409: the
	 * container is intact and correctly signed, it is simply not a bootable
	 * image for this hole, and only a different image can succeed. Appended,
	 * because kOtaVerdicts is indexed by this value. */
	AIUS_OPTICAL_APP_OTA_IVT
} AiusOpticalAppOtaResult;

/* Everything GET /api/ota publishes, filled by the application in one call.
 *
 * The DISPATCHER serializes this, rather than taking a ready-made body from a
 * write_*() serializer like the other GET documents, because two of its fields
 * are the route layer's own knowledge and not the application's: whether an
 * activate verb exists at all, and what a latched fault is called on the wire.
 * The application answers what the upgrade IS; the route answers what this
 * firmware can DO with it.
 *
 * The three text fields are NUL-terminated and are drawn straight into JSON, so
 * the document lists only what it can draw: a field carrying a byte outside
 * printable ASCII, a quote or a backslash is published EMPTY rather than
 * escaped or truncated -- the /sd/ index's list-only-what-you-can-link rule,
 * applied to a scalar. Half an identity with a quote deleted from the middle is
 * a different identity. */
typedef struct {
	AiusOpticalAppOtaState state;
	/* The FEATURE LATCH, not the ARMED state: "may this device do OTA at
	 * all" against "how far has this upgrade got". A reboot always comes up
	 * disarmed. */
	bool latch_armed;
	uint32_t staged_bytes;
	uint32_t total_bytes;
	/* The offset the next chunk must carry -- the same number an accepted
	 * chunk answers with, so a resuming client can read it here instead of
	 * replaying a chunk to discover it. */
	uint32_t next_offset;
	char target_id[AIUS_OPTICAL_APP_OTA_TEXT_STORAGE];
	char build_revision[AIUS_OPTICAL_APP_OTA_TEXT_STORAGE];
	uint64_t build_utc;
	/* The compiled-in public key's fingerprint, and whether it is the
	 * PUBLISHED dev keypair. The plan (owner decision 2026-07-31) ships the
	 * test-only key as the device key for now, and requires that state to
	 * stay visible: the flag is what lets a page banner TEST KEY. */
	char key_fingerprint[AIUS_OPTICAL_APP_OTA_TEXT_STORAGE];
	bool test_key;
	/* The runtime activation latch: whether POST /api/ota/activate will run the
	 * flash programmer rather than answer 409. It starts false at every boot and
	 * is flipped only by the deliberate operator act (POST
	 * /api/ota/enable-activation), which the operator performs after proving the
	 * scratch-sector recovery check on the bench. A page reads this to decide
	 * whether to offer the activate step. */
	bool activation_available;
	/* The verdict that failed, AIUS_OPTICAL_APP_OTA_OK when none, plus the
	 * injected verifier's own return value (a live diagnostic: it says WHY
	 * the verifier said no, and is not persisted). */
	AiusOpticalAppOtaResult fault;
	int verifier_fault;
} AiusOpticalAppOtaStatus;

/* The OTA seam. Every entry is required as a SET: aius_optical_app_set_ota()
 * takes all eleven or none, so a half-wired table is refused at registration
 * rather than discovered as a null call at dispatch.
 *
 * `context` is the OTA adapter's own, not AiusOpticalApp's: the session, its
 * staging backend and its key are this feature's state, and a build that
 * compiles OTA out has no reason to hold a pointer to them. */
typedef struct {
	void *context;

	/* The absent gate. False compiles out the whole surface at runtime --
	 * every verb, including a verb added later, answers 404 ota_absent
	 * before any other decision runs. A table that was never installed is
	 * absent by the same gate, so "compiled out" and "present but
	 * unavailable" reach the client identically. */
	bool (*present)(void *context);

	/* Fills the status document. False answers 503 ota_status; the struct
	 * is zeroed before the call, so a partial fill publishes zeros rather
	 * than stack. */
	bool (*status)(void *context, AiusOpticalAppOtaStatus *status);

	/* One self-contained chunk of the staged image at `offset`, where
	 * offset 0 begins with the container header. `data`/`length` borrow the
	 * request body for the duration of the call only -- the application
	 * must consume them synchronously.
	 *
	 * `next_offset` is written on OK and on RANGE (on RANGE it is the
	 * resume instruction the refusal carries), and ignored otherwise; it is
	 * zeroed before the call so a callback that leaves it alone still
	 * produces a defined body.
	 *
	 * The declared TOTAL is deliberately not passed: the container header
	 * states the image length and the application has already parsed it, so
	 * handing the transport's number down would create a second source of
	 * truth for the one fact the signature covers. */
	AiusOpticalAppOtaResult (*chunk)(void *context, uint32_t offset,
	                                 const uint8_t *data, size_t length,
	                                 uint32_t *next_offset);

	/* PURE preview of `chunk`: the exact refusal the same call would earn,
	 * with the session untouched (aius-ota-resilience.md Phase 4a). The
	 * route answers refusals from THIS, promises the acceptance, and runs
	 * the mutating `chunk` only when the durable SD write commits -- so a
	 * chunk whose write faults leaves the session exactly where it was and
	 * the 500/503 is honestly resumable instead of a poisoned digest.
	 * Writes `next_offset` under the same rules as `chunk`. */
	AiusOpticalAppOtaResult (*chunk_check)(void *context, uint32_t offset,
	                                       const uint8_t *data, size_t length,
	                                       uint32_t *next_offset);

	/* The deliberate act: arm the FEATURE LATCH. Arming does not start an
	 * upgrade, it makes one possible. */
	AiusOpticalAppOtaResult (*arm)(void *context);
	/* Digest, then signature, over what was staged. */
	AiusOpticalAppOtaResult (*verify)(void *context);
	/* Back to IDLE, discarding the staged image. */
	AiusOpticalAppOtaResult (*abort)(void *context);

	/* Runs the staged image's flash activation. Gated on the runtime
	 * activation latch (ACTIVATION_DISABLED when off) and on ARMED state
	 * (STATE_REFUSED otherwise). On acceptance the device commits to quiesce +
	 * program + SYSRESETREQ, so the 200 the dispatcher returns here is the LAST
	 * thing the client sees before the board resets into the new image -- the
	 * reset is deferred past the response drain, exactly as POST /api/reboot. */
	AiusOpticalAppOtaResult (*activate)(void *context);
	/* Flips the runtime activation latch true: the deliberate operator act that
	 * makes activate and the self-test reachable. Nothing else toggles it and a
	 * reboot always comes up with it false. */
	AiusOpticalAppOtaResult (*enable_activation)(void *context);
	/* Runs the item-5 scratch-sector recovery proof with the REAL flash
	 * primitives against one sector far above the image (erase + program a
	 * known pattern + read-back verify), so the board must keep serving HTTP
	 * after it. Gated behind the activation latch so it cannot be hit casually.
	 * OK == pass, SELF_TEST_FAILED == the read-back mismatched,
	 * ACTIVATION_DISABLED == the latch is off. */
	AiusOpticalAppOtaResult (*self_test_flash)(void *context);

	/* Requests the 2026-08-01 incident's re-proof: the FULL activation
	 * sequence (verify, quiesce with every interrupt source parked, head
	 * erased first / programmed last, byte-compare per chunk) against a
	 * scratch flash window, with the reset EMULATED and interrupts restored
	 * -- the windowed primitives make an address below the scratch base
	 * structurally unreachable, so the boot region cannot be touched.
	 * `inject_fault` true is the negative arm: a readback fault injected on
	 * the scratch head, asserting the head-compare halt on real primitives.
	 * Deferred like activate -- the 200 drains, then loop() runs the
	 * rehearsal (the board goes quiet for its duration and resumes after);
	 * the verdict lands in ota_diag. Gated behind the activation latch like
	 * the self-test, and refused while a real activation or another
	 * rehearsal is pending. OK == the rehearsal was accepted and deferred. */
	AiusOpticalAppOtaResult (*rehearse)(void *context, bool inject_fault);
} AiusOpticalAppOta;

/* Caller-owned dispatcher state.
 *
 * Borrow invariant: the returned response borrows this object's storage --
 * `body`, `target_path`, `temp_path`, `backup_path`, `replay_id` -- until the
 * next HTTP dispatch on the same dispatcher, and that is strictly later than
 * the caller returning from the dispatch call. A body larger than
 * AIUS_HTTP_MAX_RESPONSE is sent straight out of `body` over roughly 25 socket
 * writes (aius_http_transaction.c format_stream_response), and an SD job reads
 * the three path buffers for the whole job, so nothing here may be rewritten
 * before the transaction reports the request finished.
 *
 * Two mechanisms make that exclusive today, and any storage added here -- the
 * Phase 14b write_meters buffer included -- must hold both. First, the
 * transaction emits AIUS_HTTP_ACTION_DISPATCH only from state
 * AIUS_HTTP_TX_DISPATCH and only once per request (`dispatch_emitted`,
 * aius_http_transaction.c:1611), so no second HTTP dispatch can run while a
 * response drains in AIUS_HTTP_TX_SEND or an SD job runs in
 * AIUS_HTTP_TX_SD_TRANSFER. Second, USB dispatch shares one dispatcher with
 * HTTP every runtime epoch but writes only `usb_payload` / `usb_response`, so
 * an interleaved USB command cannot disturb an in-flight HTTP response. */
typedef struct {
	AiusOpticalApp app;
	/* Zeroed by init, so a dispatcher nobody wired refuses OTA rather than
	 * dispatching into a null call. It adds nothing to the borrow set above:
	 * every OTA response is either a string literal or formatted into
	 * `body`, and the status document is assembled in call-local storage. */
	AiusOpticalAppOta ota;
	uint8_t *body;
	size_t body_capacity;
	char target_path[AIUS_SD_PATH_STORAGE];
	char temp_path[AIUS_SD_PATH_STORAGE];
	char backup_path[AIUS_SD_PATH_STORAGE];
	char replay_id[AIUS_OPTICAL_APP_REPLAY_ID_STORAGE];
	char usb_payload[AIUS_OPTICAL_APP_USB_PAYLOAD_STORAGE];
	/* The flag word (Phase 4f): ota_pending, recorder_lease_active, valid
	 * and the recorder lease's slot (a recorder bank index, under 256) share
	 * one word placed here, in what was the alignment hole before the
	 * 8 B aligned index_entries, so the OCRAM instance does not grow. Each
	 * is documented with its mechanism below. */
	bool ota_pending;
	bool recorder_lease_active;
	bool valid;
	uint8_t recorder_lease_slot;
	/* Phase 4g: the web-cache lease a cache-served body holds (the entry
	 * index and its guard), released by that response's settle hook
	 * (web_cache_settle) exactly as the recorder lease above. Both sit in
	 * the 2 B alignment hole before index_entries (ARM offsets 910, 911),
	 * so the OCRAM instance does not grow. */
	uint8_t web_lease_entry;
	bool web_lease_active;
	/* Scratch for one directory listing, ~3 KB. Written and read entirely
	 * inside a single dispatch call -- the response borrows `body`, never this
	 * -- so it does not extend the borrow invariant above. It lives here rather
	 * than on the stack because the composed runtime instance is DMAMEM
	 * (OCRAM), and the DTCM free-for-locals zone is the scarce bank. */
	AiusOpticalAppDirEntry index_entries[AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES];
	AiusHttpApplicationResponse http_response;
	AiusUsbApplicationResponse usb_response;
	uint32_t upload_sequence;
	uint32_t http_dispatches;
	uint32_t usb_dispatches;
	/* The pending OTA absorb (aius-ota-resilience.md Phase 4a): the chunk
	 * the route promised but has not yet absorbed, applied by the
	 * transaction's sd_settle hook at the write's COMMITTED terminal and
	 * cleared at any other outcome. `data` borrows the job-owning
	 * transaction's request body, which outlives the job (the settle fires
	 * before that transaction recycles); the drain arm in the composition
	 * clears a pending left by a released connection. While armed, the
	 * chunk route answers 503 sd_busy -- a one-epoch window. ota_pending
	 * itself sits in the flag word above index_entries. */
	uint32_t ota_pending_offset;
	const uint8_t *ota_pending_data;
	size_t ota_pending_length;
	/* Read lease outstanding on a recorder-data response. Set on a READ_OK
	 * from recorder_read, whose 200 borrows a pointer into slots[slot]'s
	 * PSRAM ring for the streamed body's lifetime. The response carries a
	 * completion hook (sd_settle = recorder_read_settle, ctx = this
	 * dispatcher) that the transaction fires EXACTLY ONCE when the borrow
	 * ends -- the response has drained and the connection recycled, or a
	 * mid-stream abort tore it down -- at which point recorder_read_release
	 * drops the lease. `recorder_lease_slot` carries the borrowed slot to
	 * that hook (only one read is ever in flight -- the transaction is
	 * single-shot per request); `recorder_lease_active` guards it so the
	 * hook releases at most once and never when no borrow is live.
	 *
	 * `recorder_lease_leaks` counts the times a data read found a lease
	 * STILL active and had to drop it itself. That must not happen: the
	 * previous response's hook fires at its own terminal, teardown
	 * included (aius_http_transaction_discard). A nonzero count therefore
	 * names a path that bypassed the hook -- diagnostic, not a normal
	 * mode -- while the drop itself keeps the leaked slot from pinning the
	 * bank's round-robin for the rest of the boot. recorder_lease_slot and
	 * recorder_lease_active sit in the flag word above index_entries. */
	uint32_t recorder_lease_leaks;
} AiusOpticalAppDispatcher;

/* Copies `app` and adopts the caller-owned response body buffer. Returns false
 * (leaving an invalid dispatcher that refuses every dispatch) for a null
 * dispatcher/app, an incomplete callback table, or a body buffer smaller than
 * AIUS_OPTICAL_APP_MIN_BODY. */
bool aius_optical_app_init(AiusOpticalAppDispatcher *dispatcher,
                           const AiusOpticalApp *app,
                           uint8_t *body, size_t body_capacity);

/* Installs the OTA seam on an initialized dispatcher, or removes it when `ota`
 * is NULL. Call it AFTER aius_optical_app_init(), which zeroes the slot.
 *
 * Returns false -- leaving OTA ABSENT rather than half-wired -- for a null or
 * invalid dispatcher and for a table with some but not all six callbacks. A
 * build that compiles OTA out simply never calls this. */
bool aius_optical_app_set_ota(AiusOpticalAppDispatcher *dispatcher,
                              const AiusOpticalAppOta *ota);

/* ------------------------------------------------------------------ */
/* Phase 4g: the PSRAM web cache seam (aius-sample-rate-following.md G1 to
 * G3, G7). The cache itself (lib/aius_web_cache) is the composition's; the
 * dispatcher reaches it only through this table, so the portable dispatcher
 * stays free of the cache's carve and hooks.
 *
 * `path` arguments are relative to /sd/www with no leading '/'. `lookup`
 * finds a VALID entry and writes its quoted strong ETag, NUL-terminated, into
 * `etag`, and on a miss sets `*fault` when the miss is a manifest entry that
 * failed its own CRC and was dropped (the dispatcher then calls `refill`);
 * `lease` pins it (one `release` per true lease); `verify` is the
 * pre-header chunk check with its producer yield point every 8 chunks;
 * `copy_chunk` is the send-path copy of one 512 B chunk with its CRC on the
 * copy; `refill` schedules the fill walk to refetch what a CRC fault
 * dropped; `listing` fills `batch` from the manifest only when the fill has
 * published every card file of directory `dir` ("presets/"), else returns
 * false with `batch` untouched and the card is read. The seam is borrowed:
 * it must outlive its installation. */
#define AIUS_OPTICAL_APP_ETAG_STORAGE 24u
/* The body source yields to the producer after every this many chunks. */
#define AIUS_OPTICAL_APP_WEB_YIELD_CHUNKS 8u

typedef struct {
	uint8_t entry;
	uint32_t length;
} AiusOpticalAppWebHit;

typedef struct {
	void *context;
	bool (*lookup)(void *context, const char *path, size_t length,
	               AiusOpticalAppWebHit *hit, char *etag,
	               size_t etag_capacity, bool *fault);
	bool (*lease)(void *context, uint8_t entry);
	void (*release)(void *context, uint8_t entry);
	bool (*verify)(void *context, uint8_t entry);
	bool (*copy_chunk)(void *context, uint8_t entry, uint16_t chunk,
	                   uint8_t *dst, uint32_t *bytes);
	void (*refill)(void *context);
	bool (*listing)(void *context, const char *dir, size_t dir_length,
	                AiusOpticalAppDirBatch *batch);
} AiusOpticalAppWebCache;

/* Installs the web cache seam (every member but `context` required), or
 * removes it with NULL; call it after aius_optical_app_init(), which clears
 * it. One seam per process: it is held in DTCM, not in the dispatcher, whose
 * OCRAM instance has no room for a pointer. False for an invalid dispatcher
 * or a half-wired table (the cache then stays absent: every GET reads the
 * card). */
bool aius_optical_app_set_web_cache(AiusOpticalAppDispatcher *dispatcher,
                                    const AiusOpticalAppWebCache *web_cache);

/* Phase 4h: the SD write-behind overlay seam (lib/aius_sd_overlay, owned by
 * the composition). While `present` reads true (the PSRAM carve is fitted),
 * settings and www writes are accepted into the store and answered 200 at
 * accept, card or no card (Q1, B2); with it false (8 MiB) every route keeps
 * its synchronous card write (Q4). Keys are relative to /aius/.
 *   put_begin  a www PUT: reserves the streamed put for `length` bytes; OK,
 *              BUSY (store full: 503 sd_busy) or INVALID (507 sd_upload).
 *   put_abort  frees that reservation when no STORE request follows.
 *   tombstone  a www DELETE: OK whether or not the card holds the file (S2),
 *              BUSY when no entry is free (N3).
 *   drained    nothing dirty, flushing or tombstoned (FR-S8): OTA activate,
 *              OTA rehearse and POST /api/reboot answer 409 sd_flush_pending
 *              otherwise.
 *   merge_listing  a card-read listing of directory `dir` ("www/" or
 *              "www/presets/", relative to /aius/) gets the store's pending
 *              rows (a pending size replaces the card's; a new name is
 *              appended while capacity lasts, else `more`) and loses its
 *              tombstoned names (S6). */
typedef struct {
	void *context;
	bool (*present)(void *context);
	AiusOpticalAppStoreResult (*put_begin)(void *context, const char *rel,
	                                       size_t rel_length, uint32_t length);
	void (*put_abort)(void *context);
	AiusOpticalAppStoreResult (*tombstone)(void *context, const char *rel,
	                                       size_t rel_length);
	bool (*drained)(void *context);
	void (*merge_listing)(void *context, const char *dir, size_t dir_length,
	                      AiusOpticalAppDirBatch *batch);
} AiusOpticalAppSdOverlay;

/* Installs the overlay seam (every member but `context` required) or removes
 * it with NULL; held in DTCM like the web cache seam and cleared by
 * aius_optical_app_init(). False for an invalid dispatcher or a half-wired
 * table. */
bool aius_optical_app_set_sd_overlay(AiusOpticalAppDispatcher *dispatcher,
                                     const AiusOpticalAppSdOverlay *overlay);

/* Phase 4h H10: the mount load and restore of the two settings files, as
 * portable policy. The composition's SD step machine runs the CONFIG_LOAD
 * jobs (the target, then on a failure its fixed backup) and reports each
 * publication and each job end here; the platform callbacks own the parsers,
 * the running state and the store.
 *
 *   carve_present  the PSRAM store is fitted (no carve: no restore, N1)
 *   shadowed       the store shadows the target path (Q1: acknowledge, do not
 *                  apply, count nothing, restore nothing)
 *   load           the target's parser over `data` (a backup uses its
 *                  target's parser) into a local; true when it passes, and
 *                  then, when `apply`, applies it as the target's load
 *                  would (config: g_config; dsp: staged with its origin and
 *                  the apply flag)
 *   target_failed  a failed target load's fallback, called exactly once per
 *                  failed target (config.json: defaults and one config error,
 *                  FR-S11; dsp.json: nothing, the active model stays)
 *   put_restore    the backup's bytes as a restore-created put of the target
 *   decide         aius_sd_overlay_restore_decision (H10, FR-S5, FR-S10)
 */
typedef enum {
	AIUS_OPTICAL_SETTINGS_CONFIG = 0, /* /aius/config.json */
	AIUS_OPTICAL_SETTINGS_DSP = 1     /* /aius/dsp.json */
} AiusOpticalSettingsFile;

/* Mirrors lib/aius_sd_overlay's AiusSdOverlayBackupResult and
 * AiusSdOverlayRestoreDecision value for value (optical_main.cpp
 * static-asserts it), so this file needs no overlay include. */
typedef enum {
	AIUS_OPTICAL_SETTINGS_BACKUP_NOT_READ = 0,
	AIUS_OPTICAL_SETTINGS_BACKUP_PARSED = 1,
	AIUS_OPTICAL_SETTINGS_BACKUP_REFUSED = 2,
	AIUS_OPTICAL_SETTINGS_BACKUP_LOAD_FAILED = 3
} AiusOpticalSettingsBackup;

typedef enum {
	AIUS_OPTICAL_SETTINGS_KEEP = 0,
	AIUS_OPTICAL_SETTINGS_RESTORE = 1,
	AIUS_OPTICAL_SETTINGS_FAULT = 2,
	AIUS_OPTICAL_SETTINGS_NOTHING = 3
} AiusOpticalSettingsDecision;

typedef struct {
	void *context;
	bool (*carve_present)(void *context);
	bool (*shadowed)(void *context, AiusOpticalSettingsFile file);
	bool (*load)(void *context, AiusOpticalSettingsFile file,
	             const uint8_t *data, size_t length, bool apply);
	void (*target_failed)(void *context, AiusOpticalSettingsFile file);
	void (*put_restore)(void *context, AiusOpticalSettingsFile file,
	                    const uint8_t *data, size_t length);
	AiusOpticalSettingsDecision (*decide)(bool target_valid,
	                                      AiusOpticalSettingsBackup backup);
} AiusOpticalSettingsOps;

/* The per-load verdict and the sd_diag counters (settings_restores,
 * settings_restore_faults). Zero-initialise. */
typedef struct {
	uint32_t restores;
	uint32_t restore_faults;
	uint8_t verdict; /* the running load's publication verdict */
} AiusOpticalSettingsMount;

/* A load (target or backup) is starting: forget the last verdict. */
void aius_optical_settings_load_begin(AiusOpticalSettingsMount *mount);

/* The running load published `data`. `backup` false: the target's load;
 * true: its backup's (the restore step). Always acknowledges (returns true):
 * a refused parse is a property of the file, not a job fault. */
bool aius_optical_settings_publish(AiusOpticalSettingsMount *mount,
                                   const AiusOpticalSettingsOps *ops,
                                   AiusOpticalSettingsFile file, bool backup,
                                   const uint8_t *data, size_t length);

/* The running load's job ended (`job_ok` false: open, size, read or publish
 * fault). For a target: true when its backup's load must run next (the
 * restore step, S7: the single next step). For a backup: counts the
 * decision and returns false. */
bool aius_optical_settings_load_end(AiusOpticalSettingsMount *mount,
                                    const AiusOpticalSettingsOps *ops,
                                    AiusOpticalSettingsFile file, bool backup,
                                    bool job_ok);

/* Phase 4h: the write-behind flush coordinator, as portable policy. One
 * instance per process (optical_main.cpp's), driven at the composition's
 * seams; the platform callbacks own the overlay store, the storage engine,
 * the 4g fill and the card pins. It holds:
 *   - the flush job: one at a time, started only with nothing held, frozen
 *     or card-unready, after any HTTP SD start or card-reading DISPATCH held
 *     behind the fill has run (G6) and after the fill file it stops has
 *     stopped (G9); its end releases the fill hold only if it stopped a fill
 *     (S9, R5-S1). No replay interlock (Q3).
 *   - the card-loss hold's playback remount retry (H3, S1, N5, NS3) and the
 *     NS7 no-card probe skip: the idle step's choice.
 *   - the H9 SD-writes freeze and the OTA/reboot drain gate (R4-B1, FR-S8).
 *   - the one streamed www put (NS2, NS5, D4).
 *
 * Callbacks (every member but `context` required):
 *   present         the PSRAM carve is fitted (false: drained, no flush)
 *   ready           the card is ready (the lifecycle's g_sd_ready)
 *   held            the overlay's card-loss hold
 *   due             a tombstone, or a dirty entry past its debounce
 *   drained         the overlay holds nothing dirty, flushing or tombstoned
 *   http_deferred   an HTTP SD start or card-reading DISPATCH is held (G6)
 *   fill_in_flight  a 4g fill file is in flight
 *   fill_cancel     stops it through the latching 4g cancel (holds the fill)
 *   fill_hold_release  the canceller's release of that latch
 *   storage_ready   no SD job in flight and no FIFO deferral (F2)
 *   start           picks and starts the next flush job; fills `job`
 *   job_end         the overlay's job end (also the refused start's FAILED)
 *   note_pass       a closed pass of a COPY_BEFORE_WRITE (an
 *                   AiusWebCacheSafeCopy value)
 *   note_readback   its read-back verdict
 *   note_mounted    a held-clearing mount
 *   note_card_lost  the probe's no-card verdict
 *   playback        the probe-skip predicate (bring-up done, source on)
 *   card_detected   DAT3 reads a card
 *   settled         no SD job of any owner in flight (the freeze's wait)
 *   fill_rel        the in-flight fill file's /aius/-relative key, or NULL
 *   cancel_root     stops that fill file with no hold latched (a put reads no
 *                   card); true when it stopped one
 *   put_begin       the overlay's streamed put reservation
 *   put_abort       frees it */
#define AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS 2000u     /* H3: first playback retry */
#define AIUS_OPTICAL_SD_FLUSH_REMOUNT_MAX_MS 60000u /* S1: doubling cap */
#define AIUS_OPTICAL_SD_FLUSH_FLAP_MS 5000u        /* N5: a gone card this soon
                                                     after a held-clearing mount
                                                     is a failed retry */
#define AIUS_OPTICAL_SD_FREEZE_MS 2000u            /* H9: the freeze's ceiling */

/* Mirrors lib/aius_sd_overlay's AiusSdOverlayJobOutcome value for value
 * (optical_main.cpp static-asserts it). */
typedef enum {
	AIUS_OPTICAL_SD_FLUSH_OK = 0,
	AIUS_OPTICAL_SD_FLUSH_FAILED = 1,
	AIUS_OPTICAL_SD_FLUSH_CARD_GONE = 2,
	AIUS_OPTICAL_SD_FLUSH_CHUNK_CRC = 3
} AiusOpticalSdFlushOutcome;

typedef enum {
	AIUS_OPTICAL_SD_FLUSH_START_NONE = 0, /* no job due */
	AIUS_OPTICAL_SD_FLUSH_START_STARTED,
	AIUS_OPTICAL_SD_FLUSH_START_REFUSED   /* the engine refused the request */
} AiusOpticalSdFlushStart;

typedef struct {
	bool copy_before_write; /* AIUS_SD_OVERLAY_JOB_COPY_BEFORE_WRITE */
	bool target_first;      /* AIUS_SD_OVERLAY_ORDER_TARGET_FIRST */
	bool skip_backup;
} AiusOpticalSdFlushJob;

/* The terminal of the running flush job, as the storage engine reports it. */
typedef struct {
	bool result;            /* a result record exists */
	bool fault;             /* its kind is FAULT */
	bool input_faulted;     /* the root input refused a chunk (FR-S3) */
	bool mounted;           /* the adapter still trusts its mount */
	bool loss_pending;      /* D-S7: the card-loss verdict is PENDING (a
	                           file still open): the end waits, unsettled */
	bool card_detected;     /* D-S1: DAT3 read after a lost mount (true
	                           while mounted); a lost mount with a card
	                           detected is a read or write error, not a
	                           card-gone fault */
	bool readback_mismatch;
	uint8_t passes_closed;
} AiusOpticalSdFlushEnd;

typedef struct {
	void *context;
	bool (*present)(void *context);
	bool (*ready)(void *context);
	bool (*held)(void *context);
	bool (*due)(void *context, uint32_t now_ms);
	bool (*drained)(void *context);
	bool (*http_deferred)(void *context);
	bool (*fill_in_flight)(void *context);
	void (*fill_cancel)(void *context);
	void (*fill_hold_release)(void *context);
	bool (*storage_ready)(void *context);
	AiusOpticalSdFlushStart (*start)(void *context, uint32_t now_ms,
	                                 AiusOpticalSdFlushJob *job);
	void (*job_end)(void *context, AiusOpticalSdFlushOutcome outcome,
	                uint32_t now_ms);
	void (*note_pass)(void *context, uint8_t safe_copy);
	void (*note_readback)(void *context, bool ok);
	void (*note_mounted)(void *context, uint32_t now_ms);
	void (*note_card_lost)(void *context);
	bool (*playback)(void *context);
	bool (*card_detected)(void *context);
	bool (*settled)(void *context);
	const char *(*fill_rel)(void *context);
	bool (*cancel_root)(void *context);
	AiusOpticalAppStoreResult (*put_begin)(void *context, const char *rel,
	                                       size_t rel_length, uint32_t length);
	void (*put_abort)(void *context);
	/* D-S5: after a driver fault dropped the mount, re-mux DAT3 and read it
	 * (true: a card is in the slot). Called only for a NEW fault, never for
	 * a mode-only drop (E3). */
	bool (*card_present_after_fault)(void *context);
	/* D-S9: the adapter holds a trusted mount (a BEGIN then reuses it, no
	 * card transaction). */
	bool (*mounted)(void *context);
} AiusOpticalSdFlushOps;

/* Zero-initialise. */
typedef struct {
	uint32_t remount_at_ms;
	uint32_t remount_interval_ms; /* 0: AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS */
	uint32_t mounted_at_ms;       /* the last held-clearing mount */
	uint32_t freeze_latched_at_ms;
	bool running;
	bool copy_before_write;
	bool target_first;
	bool skip_backup;
	bool cancelled_fill;   /* this flush stopped a fill file (S9) */
	bool remount_running;  /* the probe in flight is the held retry */
	bool mount_seen;       /* mounted_at_ms is meaningful */
	bool frozen;
	bool freeze_cancelled_fill; /* the freeze stopped a fill (R5-S1, S9) */
	bool put_open;         /* the one streamed put (NS5) */
	bool fault_remount;    /* D-S5: a fault dropped a present card's mount */
	bool remount_armed;    /* CR-B1: remount_at_ms is a pending schedule; a
	                          lapsed or never-set one reads due, and stays
	                          lapsed however long ago (no 2^31 ms revival) */
	uint8_t loss;          /* AiusOpticalSdCardLoss latched for this drop */
	uint8_t fault_backoff; /* D-S8: 0 quiet, n: delay 2 s << (n - 1), cap 60 s */
	uint32_t last_fault_ms; /* D-S8: the last FAULT_PRESENT classification
	                           or fault remount (the quiet window's start) */
	uint16_t direct_faults; /* direct card ops' driver faults (wrapping) */
	uint32_t faults_seen;  /* the fault count at the last observation */
} AiusOpticalSdFlush;

/* D-S5: what a lost mount means. Classified once per driver fault (the
 * adapter's driver_faults plus the direct card ops', noted through
 * aius_optical_sd_note_direct_fault); a mount dropped by a mode change
 * (POST /api/sd/mode) moves no fault count, so DAT3 is never re-muxed for it
 * and the card stays readable on SdFat's volume (E3). The verdict is latched
 * until the mount returns, so every caller in a pass agrees. */
/*
 * D-S7: never classified while an adapter file is open (DAT3 is still the
 * bus's, so the read would report present): the fault stays unconsumed and
 * the verdict is PENDING until the first call with no file open; a flush
 * job's end waits for it (no S5 try, no remount). A card-gone verdict notes
 * the hold itself (note_card_lost, N5's flap note).
 * D-S8: the fault remount is due AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS after the
 * fault; a further FAULT_PRESENT within AIUS_OPTICAL_SD_FAULT_QUIET_MS of the
 * last doubles the delay to AIUS_OPTICAL_SD_FLUSH_REMOUNT_MAX_MS, and a
 * successful mount does not reset it (only a quiet period does).
 * D-N7: with no PSRAM carve a fault drop is card-gone with no re-mux and no
 * self-initiated remount (the 8 MiB fitment's behaviour). */
#define AIUS_OPTICAL_SD_FAULT_QUIET_MS 60000u
typedef enum {
	AIUS_OPTICAL_SD_CARD_KEPT = 0,      /* mounted, or a fault-free drop */
	AIUS_OPTICAL_SD_CARD_GONE,          /* not ready, or DAT3 reads no card */
	AIUS_OPTICAL_SD_CARD_FAULT_PRESENT, /* a fault, card present: remount on
	                                       the D-S8 schedule (the held
	                                       retry's gates, also during
	                                       playback); no hold */
	AIUS_OPTICAL_SD_CARD_PENDING        /* D-S7: a fault with a file open */
} AiusOpticalSdCardLoss;

AiusOpticalSdCardLoss aius_optical_sd_card_loss(AiusOpticalSdFlush *flush,
                                                const AiusOpticalSdFlushOps *ops,
                                                bool card_ready, bool mounted,
                                                bool file_open,
                                                uint32_t driver_faults,
                                                uint32_t now_ms);
/* A direct card op ended with a nonzero driver code. */
void aius_optical_sd_note_direct_fault(AiusOpticalSdFlush *flush);

/* CR-B1: the seam's millis windows lapse explicitly, as 4g's fill hold does:
 * the remount schedule (remount_armed), the N5 flap window (mount_seen), the
 * D-S8 quiet window (fault_backoff) each latch "lapsed" once observed past
 * their bound, so an uptime past 2^31 ms or a schedule older than 2^31 ms
 * never reads as pending again. Called every lifecycle pass
 * (aius_optical_sd_card_loss does), and by every reader. */
void aius_optical_sd_flush_lapse(AiusOpticalSdFlush *flush, uint32_t now_ms);

/* D-S9, the one BEGIN gate: true when a storage job that would BEGIN must
 * not start now. During playback (the probe-skip predicate) with no trusted
 * mount, no BEGIN starts ahead of the D-S8 / H3 schedule (a fault remount
 * or the carve's hold not yet due) or while DAT3 reads no card (which also
 * sets the hold). The scheduled remount itself (remount_running) and any
 * BEGIN that reuses a mount are never blocked. Consulted by every BEGIN
 * path: the lifecycle's root steps, the flush start, the fill start, an
 * HTTP SD job's SD_START admission and the replay job's BEGIN. */
bool aius_optical_sd_begin_blocked(AiusOpticalSdFlush *flush,
                                   const AiusOpticalSdFlushOps *ops,
                                   uint32_t now_ms);
/* D-N9: the lifecycle's root step start. The probe step the remount retry
 * chose (remount_running, held by that one step until aius_optical_sd_idle_
 * step runs again) passes; every other step takes the BEGIN gate. Other
 * BEGIN paths never inherit the remount's slot. */
bool aius_optical_sd_step_begin_blocked(AiusOpticalSdFlush *flush,
                                        const AiusOpticalSdFlushOps *ops,
                                        uint32_t now_ms);
/* D-S10: the replay refill's BEGIN: blocked only while DAT3 reads no card
 * during playback with no mount (a real pull: the replay stops); never by
 * the schedule, so a transient fault remounts at once as before 4h. Replay
 * retires in 4i. */
bool aius_optical_sd_replay_begin_blocked(AiusOpticalSdFlush *flush,
                                          const AiusOpticalSdFlushOps *ops);

/* The lifecycle's choice with no step running (the order is the policy: a
 * due flush, then the held remount retry, then the NS7 no-card skip, then the
 * fill, then the probe). */
typedef enum {
	AIUS_OPTICAL_SD_IDLE_WAIT = 0, /* frozen, or a flush owns this pass */
	AIUS_OPTICAL_SD_IDLE_NO_CARD,  /* probe due, DAT3 reads no card: no BEGIN;
	                                  the caller re-arms its probe timer */
	AIUS_OPTICAL_SD_IDLE_FILL,     /* the next fill file */
	AIUS_OPTICAL_SD_IDLE_PROBE,    /* a probe step (a due probe off playback) */
	AIUS_OPTICAL_SD_IDLE_REMOUNT   /* the held flush's remount retry probe */
} AiusOpticalSdIdleStep;

AiusOpticalSdIdleStep aius_optical_sd_idle_step(AiusOpticalSdFlush *flush,
                                                const AiusOpticalSdFlushOps *ops,
                                                uint32_t now_ms, bool probe_due);

/* Starts the next due flush. True while a flush owns this pass (started, or
 * waiting for the fill file it cancelled to stop): start no fill file. */
bool aius_optical_sd_flush_start(AiusOpticalSdFlush *flush,
                                 const AiusOpticalSdFlushOps *ops,
                                 uint32_t now_ms);
/* The running flush job reached its terminal (the caller checks the engine's
 * root job is no longer running): the pass and read-back notes, then
 * job_end; a card-gone outcome is noted (N5); the fill hold is released only
 * if this flush stopped a fill (S9). No-op with no flush running. */
void aius_optical_sd_flush_settle(AiusOpticalSdFlush *flush,
                                  const AiusOpticalSdFlushOps *ops,
                                  const AiusOpticalSdFlushEnd *end,
                                  uint32_t now_ms);
/* D-S1: the card-gone classification. A lost mount is card-gone only when
 * DAT3 then reads no card (or the lifecycle's card is not ready); a card
 * still detected failed a read or write: the job fails as a counted S5 try
 * with backoff and no hold is set. */
bool aius_optical_sd_card_gone(bool card_ready, bool mounted,
                               bool card_detected);

/* D-N3: the REPLACE flush's scratch name id, from the pinned version's
 * accept CRC and its target path. Boot-independent, so a scratch pair a cut
 * left behind in an earlier boot (the only copy of its file) is never reused
 * by a different file's flush (a 2^-32 collision); the same file with the
 * same bytes reuses it harmlessly (latest wins). */
uint32_t aius_optical_sd_flush_scratch_id(uint32_t file_crc,
                                          const char *target_path);

/* A card-gone fault (a job's or a direct card op's): within
 * AIUS_OPTICAL_SD_FLUSH_FLAP_MS of a held-clearing mount it backs off (N5). */
void aius_optical_sd_flush_note_card_gone(AiusOpticalSdFlush *flush,
                                          uint32_t now_ms);
/* The probe's verdict: a mount while held clears the hold; no card notes the
 * loss, and a held retry that found none backs off (S1). */
void aius_optical_sd_flush_note_probe(AiusOpticalSdFlush *flush,
                                      const AiusOpticalSdFlushOps *ops,
                                      bool present, uint32_t now_ms);
/* FR-S8: nothing dirty, flushing or tombstoned (always true with no carve). */
bool aius_optical_sd_flush_drained(const AiusOpticalSdFlushOps *ops);

/* H9, R4-B1: the OTA and reboot services' gate, after their HTTP drain wait
 * and before any one-way step. The drain re-check precedes the freeze latch;
 * the latched freeze then waits for any in-flight SD job under
 * AIUS_OPTICAL_SD_FREEZE_MS. PASS obliges the caller to query
 * aius_optical_sd_flush_drained() again as the statement immediately before
 * its one-way step. */
typedef enum {
	AIUS_OPTICAL_SD_GATE_WAIT = 0, /* frozen, an SD job still in flight */
	AIUS_OPTICAL_SD_GATE_PASS,     /* frozen and settled */
	AIUS_OPTICAL_SD_GATE_DRAIN,    /* unflushed writes: ota_flush_pending */
	AIUS_OPTICAL_SD_GATE_BUSY      /* the ceiling passed: ota_sd_busy */
} AiusOpticalSdGate;

AiusOpticalSdGate aius_optical_sd_write_gate(AiusOpticalSdFlush *flush,
                                             const AiusOpticalSdFlushOps *ops,
                                             uint32_t now_ms);
/* Unfreezes; releases the fill hold only if the freeze stopped a fill (S9). */
void aius_optical_sd_freeze_release(AiusOpticalSdFlush *flush,
                                    const AiusOpticalSdFlushOps *ops);

/* A www PUT's accept (NS2, B1): a fill file of the same path (case-folded)
 * in flight is stopped with no hold latched, counted in `*fill_cancels`
 * (www_cache.fill_cancels); then the overlay's reservation. BUSY while a put
 * is already open. */
AiusOpticalAppStoreResult aius_optical_sd_flush_put_begin(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	uint32_t *fill_cancels, const char *rel, size_t rel_length,
	uint32_t length);
/* The STORE sink's commit: true when a put was open (now closed). */
bool aius_optical_sd_flush_put_close(AiusOpticalSdFlush *flush);
/* Frees an open put's reservation (no-op with none open). */
void aius_optical_sd_flush_put_abort(AiusOpticalSdFlush *flush,
                                     const AiusOpticalSdFlushOps *ops);
/* NS5, D4: an open put whose transaction can no longer start or finish its
 * STORE sink (aius_optical_app_sd_staging_orphaned) is aborted. */
void aius_optical_sd_flush_sweep_orphan(AiusOpticalSdFlush *flush,
                                        const AiusOpticalSdFlushOps *ops,
                                        const AiusHttpTransaction *http,
                                        bool deferred_start_present,
                                        bool storage_action_present);

/* R4-B1, R5-S2: the OTA activation, OTA rehearsal and reboot services' SD
 * step, once their HTTP drain wait has ended. `*requested` is the service's
 * request flag: an abandon unfreezes and clears it, so the service never
 * re-runs (every later call answers IDLE: no park, no reset) until a new
 * request. `reset_on_busy`: the reboot service resets past the freeze
 * ceiling; the OTA services abandon with ota_sd_busy. */
typedef enum {
	AIUS_OPTICAL_SD_SERVICE_IDLE = 0,      /* no request */
	AIUS_OPTICAL_SD_SERVICE_WAIT,          /* frozen, an SD job in flight */
	AIUS_OPTICAL_SD_SERVICE_RUN,           /* proceed toward the one-way step */
	AIUS_OPTICAL_SD_SERVICE_FLUSH_PENDING, /* abandoned: ota_flush_pending */
	AIUS_OPTICAL_SD_SERVICE_SD_BUSY        /* abandoned: ota_sd_busy */
} AiusOpticalSdService;

AiusOpticalSdService aius_optical_sd_service_gate(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	volatile bool *requested, uint32_t now_ms, bool reset_on_busy);
/* The second drain query, the statement immediately before the one-way step
 * (the recorder park, the reset). False: a write was accepted during the
 * wait; unfrozen and `*requested` cleared (ota_flush_pending). */
bool aius_optical_sd_service_final(AiusOpticalSdFlush *flush,
                                   const AiusOpticalSdFlushOps *ops,
                                   volatile bool *requested);

/* E3: direct card ops (a /sd/ index, a replay listing) run only with the
 * card ready and DAT3 not re-muxed (a card-gone invalidation or a failed
 * begin); a mode-only mount drop (POST /api/sd/mode) leaves DAT3 clear, so
 * they read the card as today and the next storage BEGIN remounts. */
bool aius_optical_app_card_ops_ready(bool card_ready, bool dat3_remuxed);

/* The HTTP transaction is idle with nothing outstanding: the reboot drain
 * predicate, and the Q2 re-put's borrow condition. False for NULL. */
bool aius_optical_app_http_idle(const AiusHttpTransaction *http);

/* D1, Q2: a settings re-put serialises the running settings into the
 * dispatcher's application body, borrowed only while the transaction is idle
 * (a GET /api/status response being sent waits); false retries at the next
 * service step. `writer` returns 0 when the document does not fit. */
typedef size_t (*AiusOpticalSettingsWriter)(uint8_t *body, size_t capacity);
bool aius_optical_settings_reput(const AiusHttpTransaction *http,
                                 uint8_t *body, size_t capacity,
                                 AiusOpticalSettingsWriter writer,
                                 const uint8_t **bytes, uint32_t *length);

/* GET /api/dsp/config and /aius/dsp.json (write_dsp_config): the model
 * serialised with its header. `headroom_linear` is the served pad (with
 * clipfree, the computed one). */
typedef struct {
	const char *source;         /* "sd" or "default" (dsp_source_name) */
	bool config_pending;        /* the model is the staged one */
	bool file_form;             /* /aius/dsp.json (a Q2 re-put): adds the
	                               base_generation key the load requires; the
	                               GET document omits it */
	uint32_t config_generation;
	uint32_t staged_generation;
	double headroom_linear;
} AiusOpticalDspConfigHeader;

#ifdef AIUS_DSP_CONFIG_H
/* NS6: the file form parses with aius_dsp_config_parse_put to the same model
 * (the GET form lacks base_generation, so the parser refuses it). 0 when it does not fit `capacity` or an EQ type is unknown. */
size_t aius_optical_dsp_config_write(const AiusDspModel *model,
                                     const AiusOpticalDspConfigHeader *header,
                                     uint8_t *body, size_t capacity);
#endif

/* Drops a pending OTA absorb WITHOUT applying it -- the drain backstop for a
 * job whose owning connection was released before its terminal (the storage
 * service drains it ownerless, so the transaction's settle hook never fires).
 * The composition's settle arm calls this when it observes such a terminal
 * for the OTA staging target; safe (and a no-op) any other time. The session
 * stays exactly where it was, which is the honest state: nothing was
 * absorbed, and a resuming client re-reads next_offset and continues. */
void aius_optical_app_ota_pending_clear(AiusOpticalAppDispatcher *dispatcher);

/* One bounded decision for one parsed request. Returns NULL only for an
 * invalid dispatcher or a null request -- every reachable route, method and
 * failure produces a response. */
const AiusHttpApplicationResponse *aius_optical_app_http_dispatch(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request);

/* One bounded decision for one OTA request, with the verb supplied explicitly.
 *
 * This is the entry point ONLY because lib/aius_http does not allocate the
 * five /api/ota paths yet: aius_http_parse() refuses an unknown path outright, so
 * no byte sequence reaches aius_optical_app_http_dispatch() as an OTA route
 * today. Everything else about the request is the ordinary parsed one --
 * method, headers, Content-Range and body all arrive through the real parser.
 *
 * WHAT THE COMPOSITION SLICE ADDS, in order: five AIUS_HTTP_ROUTE_OTA_ values
 * plus their path/method rules in lib/aius_http, then five case labels in
 * aius_optical_app_http_dispatch() that call the same internal handler this
 * does. Two things those labels must NOT do. They must not re-enter this
 * function -- `http_dispatches` is counted here, so that would count twice --
 * and they must not join route_is_control()'s pre-switch credential gate: the
 * OTA gate order is ABSENT then TOKEN, so that an OTA-less build answers one
 * 404 for every verb instead of a 401 that says a surface exists.
 *
 * Returns NULL only for an invalid dispatcher or a null request. */
const AiusHttpApplicationResponse *aius_optical_app_ota_dispatch(
	AiusOpticalAppDispatcher *dispatcher, AiusOpticalAppOtaRoute route,
	const AiusHttpRequest *request);

/* One bounded decision for one parsed USB command. Returns NULL only for an
 * invalid dispatcher or a null request. */
const AiusUsbApplicationResponse *aius_optical_app_usb_dispatch(
	AiusOpticalAppDispatcher *dispatcher,
	const AiusUsbApplicationRequest *request);

/* ------------------------------------------------------------------ */
/* Phase 4g G6, G9: the web-cache fill hold (aius-sample-rate-following.md).
 * The decisions are portable and live here; optical_main.cpp holds the one
 * instance, starts and cancels the fill jobs and calls these at their seams.
 *
 * A cancel latches the hold with no expiry (one latch per cancel). The
 * canceller's end calls _hold_release(), which drops its latch and sets the
 * expiry to AIUS_OPTICAL_WEB_FILL_HOLD_MS after that end; every HTTP SD job
 * completion (_note_http_complete) also sets it to that long after the
 * completion; a live hold's expiry only moves later (R5-S1), and a lapsed one
 * stays lapsed however long ago it lapsed (no signed-wrap revival past
 * 2^31 ms), the next completion or release arming it afresh. The scheduler
 * starts no fill file while _held() is true. An HTTP SD start that finds a
 * fill file in flight (_sd_start) cancels it once, latches as its canceller
 * and is DEFERred at the SD_START arm until the fill has stopped; that
 * request's HTTP SD job completion is its release. A route that reads the card
 * directly (aius_optical_app_route_reads_card) does the same at the DISPATCH
 * arm (_route); its admission is its canceller's release and its end
 * (_route_end) extends the hold as a job completion does. */
#define AIUS_OPTICAL_WEB_FILL_HOLD_MS 2000u

typedef struct {
	uint32_t hold_until_ms;
	uint32_t holds;   /* cancels that latched the hold */
	uint32_t cancels; /* cancels that stopped an in-flight fill file */
	uint8_t latches;  /* latched cancels not yet released */
	uint8_t armed;    /* hold_until_ms holds an expiry */
	uint8_t http_canceller; /* the held HTTP SD start owns one latch */
	uint8_t http_defer;     /* the held SD_START or DISPATCH is held by the fill */
	uint8_t card_route;     /* an admitted direct-card route has not ended */
} AiusOpticalWebFill;

void aius_optical_web_fill_init(AiusOpticalWebFill *fill);
/* Latches the hold. True when `fill_in_flight`: the caller cancels it. */
bool aius_optical_web_fill_cancel(AiusOpticalWebFill *fill,
                                  bool fill_in_flight);
void aius_optical_web_fill_hold_release(AiusOpticalWebFill *fill,
                                        uint32_t now_ms);
/* Every HTTP SD job terminal; also the release of an HTTP canceller. */
void aius_optical_web_fill_note_http_complete(AiusOpticalWebFill *fill,
                                              uint32_t now_ms);
/* The HTTP canceller's request ended with no job (shed, torn down): its
 * release, as a completion would have been. */
void aius_optical_web_fill_drop_http_canceller(AiusOpticalWebFill *fill,
                                               uint32_t now_ms);
bool aius_optical_web_fill_held(const AiusOpticalWebFill *fill,
                                uint32_t now_ms);
/* The scheduler's start gate: true (start no fill file) while _held() or
 * while a replay plays (`replay_playing`: REPLAY or SWITCHING_TO_REPLAY;
 * owner, closure nit A), so a walk restarted mid-replay waits for its end. */
bool aius_optical_web_fill_paused(const AiusOpticalWebFill *fill,
                                  uint32_t now_ms, bool replay_playing);
/* The SD_START arm. True: DEFER this start; `*cancel` is set on the one
 * consult that must cancel the in-flight fill file, through the platform's
 * one cancel entry (which latches via aius_optical_web_fill_cancel). False:
 * consult the 4f admission as usual. */
bool aius_optical_web_fill_sd_start(AiusOpticalWebFill *fill,
                                    bool fill_in_flight, bool *cancel);
/* Whether the held SD_START was held only by the fill (so 4f's admission
 * sees a first consult, not a re-consult of its own held start); clears it. */
bool aius_optical_web_fill_take_defer(AiusOpticalWebFill *fill);

/* The routes whose handlers read the card directly rather than through an SD
 * job (Phase 4g, audit finding 3): GET /sd/<dir>/ (the index), GET and DELETE
 * /api/replays. False for NULL. */
bool aius_optical_app_route_reads_card(const AiusHttpRequest *request);
/* The DISPATCH arm for such a route. True: DEFER the dispatch; `*cancel` as
 * for _sd_start. False: run it now; a dispatch the fill held releases its
 * canceller latch here (the hold then runs AIUS_OPTICAL_WEB_FILL_HOLD_MS from
 * now), and the route's end is due (_route_end). */
bool aius_optical_web_fill_route(AiusOpticalWebFill *fill, bool fill_in_flight,
                                 uint32_t now_ms, bool *cancel);
/* Any dispatch's end (or shed): when a direct-card route was admitted, its
 * completion extends the hold like an HTTP SD job completion. */
void aius_optical_web_fill_route_end(AiusOpticalWebFill *fill, uint32_t now_ms);
/* Phase 4g (owner, audit finding 6): the cache follows the card. `ready` is
 * the lifecycle's card-ready, `mounted` the adapter's trusted mount (cleared
 * by every card-gone fault: a failed mount, a nonzero driver code, an absent
 * media check, an explicit invalidation), `mounts` its mount count. True when
 * the cache must be emptied: the card went (ready or mount lost while the walk
 * belonged to a card), or a new mount appeared (the walk restarts on it).
 * `*walk_card` (the walk belongs to a mounted card) and `*mounts_seen` are the
 * caller's state; the walk runs only while `*walk_card`, so after a card-gone
 * fault it restarts only after the next mount. */
bool aius_optical_web_fill_follow_card(bool *walk_card, uint32_t *mounts_seen,
                                       bool ready, bool mounted,
                                       uint32_t mounts);

#ifdef __cplusplus
}
#endif

#endif
