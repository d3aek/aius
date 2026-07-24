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

/* Smallest response-body buffer a dispatcher accepts. Every dynamically
 * formatted body the dispatcher writes itself fits well inside this; the
 * serializer callbacks receive whatever the caller supplied. */
#define AIUS_OPTICAL_APP_MIN_BODY 256u

/* Result of handing a validated request body to the application for storage.
 * BUSY means a storage transaction is already in flight, STALE that the body's
 * base_generation no longer matches the running configuration. */
typedef enum {
	AIUS_OPTICAL_APP_STORE_OK,
	AIUS_OPTICAL_APP_STORE_INVALID,
	AIUS_OPTICAL_APP_STORE_STALE,
	AIUS_OPTICAL_APP_STORE_BUSY
} AiusOpticalAppStoreResult;

typedef enum {
	AIUS_OPTICAL_APP_REPLAY_OK,
	AIUS_OPTICAL_APP_REPLAY_NOT_FOUND,
	AIUS_OPTICAL_APP_REPLAY_ACTIVE,
	AIUS_OPTICAL_APP_REPLAY_FAILED
} AiusOpticalAppReplayResult;

/* The application seam. Every entry is required; aius_optical_app_init()
 * rejects an incomplete table so a missing callback can never surface as a
 * null call at dispatch time. Serializers return the written length, or 0 for
 * "does not fit" -- the dispatcher turns that into a deterministic 500. */
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
	/* Records the reboot request. The runtime performs it after the response
	 * has been written; the dispatcher itself never resets or delays. */
	void (*request_reboot)(void *context);

	/* ID/AUTH payload for one USB command, NUL-terminated into `payload`.
	 * Returns the length written, or 0 when it does not fit. */
	size_t (*write_usb_payload)(void *context, AiusUsbCommandKind kind,
	                            char *payload, size_t capacity);
	/* Per-upload entropy for temporary/backup file names. */
	uint32_t (*upload_id)(void *context);
} AiusOpticalApp;

/* Caller-owned dispatcher state. The returned response borrows this object's
 * storage until the next dispatch call on the same dispatcher. */
typedef struct {
	AiusOpticalApp app;
	uint8_t *body;
	size_t body_capacity;
	char target_path[AIUS_SD_PATH_STORAGE];
	char temp_path[AIUS_SD_PATH_STORAGE];
	char backup_path[AIUS_SD_PATH_STORAGE];
	char replay_id[AIUS_OPTICAL_APP_REPLAY_ID_STORAGE];
	char usb_payload[AIUS_OPTICAL_APP_USB_PAYLOAD_STORAGE];
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

/* One bounded decision for one parsed request. Returns NULL only for an
 * invalid dispatcher or a null request -- every reachable route, method and
 * failure produces a response. */
const AiusHttpApplicationResponse *aius_optical_app_http_dispatch(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request);

/* One bounded decision for one parsed USB command. Returns NULL only for an
 * invalid dispatcher or a null request. */
const AiusUsbApplicationResponse *aius_optical_app_usb_dispatch(
	AiusOpticalAppDispatcher *dispatcher,
	const AiusUsbApplicationRequest *request);

#ifdef __cplusplus
}
#endif

#endif
