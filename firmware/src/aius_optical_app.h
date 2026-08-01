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
 * runtime_success_valid). */
#define AIUS_OPTICAL_APP_USB_PAYLOAD_MAX 256u
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
	AIUS_OPTICAL_APP_REPLAY_CARRIER_PRESENT
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
	 * five are REQUIRED (app_complete refuses a table without them).
	 * write_recorder serializes status + the frozen header.
	 * recorder_freeze / recorder_thaw / recorder_config return false when
	 * the state machine refuses (absent board, wrong state, capture in
	 * flight) -- the dispatcher answers 409; body-parse failures never
	 * reach recorder_config. recorder_read resolves one bounded frozen
	 * chunk into a pointer whose storage (the PSRAM ring) outlives the
	 * response borrow: the ring is only readable FROZEN, and a thaw is
	 * necessarily a later transaction under the single-transaction HTTP
	 * model. */
	size_t (*write_recorder)(void *context, uint8_t *body, size_t capacity);
	bool (*recorder_freeze)(void *context);
	bool (*recorder_thaw)(void *context);
	bool (*recorder_config)(void *context, uint32_t mask,
	                        float gr_threshold_db, uint32_t gr_hold_blocks,
	                        uint32_t postroll_blocks);
	AiusOpticalAppRecorderReadResult (*recorder_read)(
		void *context, uint32_t offset_bytes, uint32_t length_bytes,
		uint32_t generation, const uint8_t **data, uint32_t *length);
} AiusOpticalApp;

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

/* The five verbs, in the dispatcher's own vocabulary.
 *
 * Route identity normally arrives as an AiusHttpRoute, but lib/aius_http does
 * not allocate the five /api/ota paths yet and this slice does not own that
 * library. So the verb is named here and passed in explicitly; the composition
 * slice adds AIUS_HTTP_ROUTE_OTA_* and five case labels that forward, and this
 * enum stays the dispatcher-side name either way.
 *
 * There is deliberately NO activate verb. Activation ships with the flash slice
 * (device gate: verdict item 5), and the status document's
 * "activation_available": false is how a page knows not to offer the step. */
typedef enum {
	AIUS_OPTICAL_APP_OTA_ROUTE_NONE = 0, /* not an OTA request */
	AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,   /* GET  /api/ota          (open read) */
	AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE,    /* PUT  /api/ota/image    (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_ARM,      /* POST /api/ota/arm      (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY,   /* POST /api/ota/verify   (auth) */
	AIUS_OPTICAL_APP_OTA_ROUTE_ABORT     /* POST /api/ota/abort    (auth) */
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
	AIUS_OPTICAL_APP_OTA_STORAGE
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
	/* The verdict that failed, AIUS_OPTICAL_APP_OTA_OK when none, plus the
	 * injected verifier's own return value (a live diagnostic: it says WHY
	 * the verifier said no, and is not persisted). */
	AiusOpticalAppOtaResult fault;
	int verifier_fault;
} AiusOpticalAppOtaStatus;

/* The OTA seam. Every entry is required as a SET: aius_optical_app_set_ota()
 * takes all six or none, so a half-wired table is refused at registration
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

	/* The deliberate act: arm the FEATURE LATCH. Arming does not start an
	 * upgrade, it makes one possible. */
	AiusOpticalAppOtaResult (*arm)(void *context);
	/* Digest, then signature, over what was staged. */
	AiusOpticalAppOtaResult (*verify)(void *context);
	/* Back to IDLE, discarding the staged image. */
	AiusOpticalAppOtaResult (*abort)(void *context);
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
	bool valid;
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

#ifdef __cplusplus
}
#endif

#endif
