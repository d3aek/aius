#ifndef AIUS_HTTP_H
#define AIUS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AIUS_HTTP_MAX_REQUEST/AIUS_HTTP_MAX_BODY must fit the largest non-streamed
 * body accepted by any route: PUT /api/dsp/config, bounded to 12288 bytes
 * (AIUS_DSP_MODEL_MAX_JSON_BYTES in aius_dsp_config.h). 1024 bytes of headroom
 * covers the request line + headers (X-Aius-SD-Auth, Content-Length, etc.)
 * above the body itself. */
#define AIUS_HTTP_MAX_REQUEST 13312u
#define AIUS_HTTP_MAX_HEADERS 512u
#define AIUS_HTTP_MAX_SD_AUTH 16u /* fixed-format SD auth token width */
#define AIUS_HTTP_MAX_REPLAY_ID 48u
#define AIUS_HTTP_MAX_BODY 12288u
#define AIUS_HTTP_MAX_RESPONSE 8192u
#define AIUS_HTTP_CONTENT_TYPE_STORAGE 64u

typedef enum {
	AIUS_HTTP_GET = 1,
	AIUS_HTTP_POST = 2,
	AIUS_HTTP_DELETE = 3,
	AIUS_HTTP_PUT = 4
} AiusHttpMethod;

typedef enum {
	AIUS_HTTP_ROUTE_UNKNOWN = 0,
	AIUS_HTTP_ROUTE_HOME,
	AIUS_HTTP_ROUTE_STATUS,
	AIUS_HTTP_ROUTE_EVENTS,
	AIUS_HTTP_ROUTE_START,
	AIUS_HTTP_ROUTE_STOP,
	AIUS_HTTP_ROUTE_CLEAR,
	AIUS_HTTP_ROUTE_REBOOT,
	AIUS_HTTP_ROUTE_CONFIG_PUBLIC,
	AIUS_HTTP_ROUTE_SD_STATUS,
	AIUS_HTTP_ROUTE_SD_FILE,
	AIUS_HTTP_ROUTE_DSP_CONFIG,
	AIUS_HTTP_ROUTE_METERS,
	AIUS_HTTP_ROUTE_REPLAYS,
	AIUS_HTTP_ROUTE_REPLAYS_STATUS,
	AIUS_HTTP_ROUTE_REPLAYS_START,
	AIUS_HTTP_ROUTE_REPLAYS_STOP,
	/* Deleting a replay is DELETE /api/replays, i.e. AIUS_HTTP_ROUTE_REPLAYS
	 * with AIUS_HTTP_DELETE -- there is no separate delete path to parse. */
	AIUS_HTTP_ROUTE_SD_REPLAY,
	/* The measurement loopback (item 3): GET /api/measure is the public read
	 * (state machine + figures), POST /api/measure/start the auth-gated arm. */
	AIUS_HTTP_ROUTE_MEASURE,
	AIUS_HTTP_ROUTE_MEASURE_START,
	/* The PSRAM flight recorder. GET /api/recorder reads status + the
	 * frozen header; POST freeze/thaw move the state machine; PUT config
	 * installs the RAM-only trigger config; GET /api/recorder/data pulls
	 * one bounded frozen chunk (offset/length/generation, fixed order, all
	 * required). Every recorder route is token-gated at dispatch. */
	AIUS_HTTP_ROUTE_RECORDER,
	AIUS_HTTP_ROUTE_RECORDER_FREEZE,
	AIUS_HTTP_ROUTE_RECORDER_THAW,
	AIUS_HTTP_ROUTE_RECORDER_CONFIG,
	AIUS_HTTP_ROUTE_RECORDER_DATA,
	/* OTA staged firmware upgrade (docs/plans/aius-ota-upgrade.md, slice 3).
	 * GET /api/ota is the open read; PUT /api/ota/image the resumable chunk
	 * (APPEND_PART-shaped, Content-Range, buffered); POST /api/ota/{arm,verify,
	 * abort} the auth-gated verbs. These ids and their route_for()/method rules
	 * are UNCONDITIONAL -- an enum value costs no image bytes, and the small
	 * matching stubs keep one AiusHttpRoute the optical dispatcher and every host
	 * suite agree on (the `http` suite compiles aius_http.c without the OTA build
	 * option, so gating the matching would leave it target-only and untested).
	 * The OTA feature's real cost -- the session, key, callbacks, ota_diag and
	 * TweetNaCl -- lives behind #if AIUS_OTA in the composition root, not here.
	 * The dispatcher owns the absent-then-token gate for these routes, so none of
	 * them belongs in aius_optical_app.cpp's route_is_control(). */
	AIUS_HTTP_ROUTE_OTA,
	AIUS_HTTP_ROUTE_OTA_IMAGE,
	AIUS_HTTP_ROUTE_OTA_ARM,
	AIUS_HTTP_ROUTE_OTA_VERIFY,
	AIUS_HTTP_ROUTE_OTA_ABORT,
	/* The activation-wiring slice's three POST verbs (all auth-gated at
	 * dispatch, same as the four verbs above). POST /api/ota/activate runs the
	 * flash programmer; POST /api/ota/enable-activation flips the runtime latch
	 * that keeps activation refused (409) until the operator has proven the
	 * scratch-sector recovery check on the bench; POST /api/ota/self-test-flash
	 * runs the real erase/program/read-back proof against one scratch sector.
	 * Like the five ids above these are UNCONDITIONAL here -- the enum value and
	 * its route_for()/method stub cost no image bytes; the composition root's
	 * #if AIUS_OTA owns the real cost. enable-activation and self-test are
	 * deliberately absent from the OpenAPI contract (bench/operator surfaces);
	 * only /api/ota/activate is documented. */
	AIUS_HTTP_ROUTE_OTA_ACTIVATE,
	AIUS_HTTP_ROUTE_OTA_ENABLE_ACTIVATION,
	AIUS_HTTP_ROUTE_OTA_SELF_TEST_FLASH,
	/* The 2026-08-01 incident's re-proof verbs (bench/operator surface, same
	 * class as enable-activation/self-test: auth-gated at dispatch, absent
	 * from the OpenAPI contract). POST /api/ota/rehearse runs the FULL
	 * activation sequence -- verify, quiesce with every interrupt source
	 * parked, erase-head-first/program-head-last, byte-compare per chunk --
	 * against a scratch flash window far above the image, with the reset
	 * EMULATED (a verdict instead of SYSRESETREQ) and interrupts restored
	 * after. POST /api/ota/rehearse-fault is the negative arm: an injected
	 * readback fault on the scratch head asserts the head-compare halt on
	 * REAL primitives. Unconditional ids, like every OTA id above. */
	AIUS_HTTP_ROUTE_OTA_REHEARSE,
	AIUS_HTTP_ROUTE_OTA_REHEARSE_FAULT
} AiusHttpRoute;

typedef enum {
	AIUS_HTTP_OK = 0,
	AIUS_HTTP_ERR_EMPTY,
	AIUS_HTTP_ERR_MALFORMED,
	AIUS_HTTP_ERR_TIMEOUT,
	AIUS_HTTP_ERR_TOO_LARGE,
	AIUS_HTTP_ERR_METHOD,
	AIUS_HTTP_ERR_ROUTE,
	AIUS_HTTP_ERR_BODY
} AiusHttpResult;

typedef struct {
	AiusHttpMethod method;
	AiusHttpRoute route;
	uint32_t cursor;
	uint8_t has_body;
	size_t header_length;
	bool has_content_length;
	uint64_t content_length;
	const uint8_t *body;
	size_t body_length;
	bool body_complete;
	const uint8_t *resource;
	size_t resource_length;
	/* Replay ID for /sd/replays/<id>, pointing into the parsed request bytes. */
	const uint8_t *replay_id;
	size_t replay_id_length;
	/* Optional exact-header value, pointing into the parsed request bytes. */
	const uint8_t *sd_auth;
	size_t sd_auth_length;
	/* Optional validated Content-Range metadata. */
	uint8_t has_content_range;
	uint32_t content_range_start;
	uint32_t content_range_end;
	uint32_t content_range_total;
	/* GET /api/recorder/data query values, valid for that route only. */
	uint32_t recorder_offset;
	uint32_t recorder_length;
	uint32_t recorder_generation;
	/* Optional trailing &slot=<n> selecting one bank slot to read. Absent
	 * (recorder_has_slot == false) means the handler reads the NEWEST FROZEN
	 * slot, refusing with 409 when none is frozen. The pre-bank single-shot
	 * request shape is preserved byte-for-byte; what it resolves to is the
	 * newest capture, not the active (still-recording) slot. */
	bool recorder_has_slot;
	uint32_t recorder_slot;
} AiusHttpRequest;

AiusHttpResult aius_http_parse(const uint8_t *data, size_t length,
	                               AiusHttpRequest *request);

const char *aius_http_reason(unsigned status);
/* cache_max_age_s nonzero appends "Cache-Control: max-age=<n>"; zero emits
 * no caching header at all (the pre-existing header set, byte-identical). */
size_t aius_http_write_headers(uint8_t *output, size_t capacity,
	                           unsigned status, const char *content_type,
	                           uint64_t content_length,
	                           uint32_t cache_max_age_s);
size_t aius_http_write_response(uint8_t *output, size_t capacity,
	                              unsigned status, const char *content_type,
	                              const uint8_t *body, size_t body_length,
	                              uint32_t cache_max_age_s);

#ifdef __cplusplus
}
#endif

#endif
