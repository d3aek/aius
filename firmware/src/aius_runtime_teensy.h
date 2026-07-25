/* Composed Teensy runtime.
 *
 * The construction-only, epoch-driven composition root. It embeds the three
 * bounded services, the optical application dispatcher and the platform callback
 * table, and owns exactly three things: the fixed scheduler callback mapping,
 * the retained scheduler mode, and the cross-service HTTP/storage bridge. It
 * performs no transport, SD, watchdog or delay call of its own -- every effect
 * leaves through a service or a platform callback.
 *
 * Nothing in this header holds file-static lifecycle state; the whole runtime is
 * one caller-owned object. It is large (roughly 35 kB, dominated by the HTTP
 * request/response buffers and the DSP-model-sized application body buffer), so
 * the final composition MUST place its instance in OCRAM:
 *
 *     DMAMEM static AiusRuntimeTeensy g_runtime;
 *
 * DTCM has ~22 kB free for locals on this image and shares FlexRAM with ITCM;
 * a DTCM instance would not fit. optical_main.cpp already keeps the HTTP
 * buffers in DMAMEM for the same reason.
 *
 * Phase 12 lands the composition and its `runtime-target` suite only. Nothing in
 * production constructs it yet; Phase 14 replaces setup()/loop() with it.
 */

#ifndef AIUS_RUNTIME_TEENSY_H
#define AIUS_RUNTIME_TEENSY_H

#ifndef __cplusplus
#error "aius_runtime_teensy.h requires C++ (AiusSdTeensy embeds an SD File)"
#endif

#include "aius_cdc_teensy.h"
#include "aius_dsp_config.h"
#include "aius_net.h"
#include "aius_optical_app.h"
#include "aius_runtime.h"
#include "aius_runtime_network_http.h"
#include "aius_runtime_storage.h"
#include "aius_runtime_usb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Application response-body buffer. Sized for the largest body any route
 * serializes -- GET /api/dsp/config's effective model, which optical_main.cpp
 * today serves from an equally sized dedicated DMAMEM buffer. */
#define AIUS_RUNTIME_TEENSY_APP_BODY AIUS_DSP_MODEL_MAX_JSON_BYTES

extern "C" {

/* Retained boot facts the RESET_CAPTURE platform callback publishes once per
 * epoch. Kept separate from the boot ledger so the composition never depends on
 * the retained-storage adapter's types. */
typedef struct {
	uint32_t boot_generation;
	uint32_t reset_source;
	uint8_t watchdog_early_resets;
} AiusRuntimeTeensyBootStatus;

/* The platform seam: the four checkpoints the composition does not own, plus
 * checkpoint recording and the sole watchdog feed. Every entry is required. */
typedef struct {
	void *context;
	bool (*reset_capture)(
		void *context,
		const AiusRuntimeBudget *budget,
		uint32_t now_ms,
		AiusRuntimeTeensyBootStatus *status);
	bool (*metrics)(
		void *context,
		const AiusRuntimeBudget *budget,
		uint32_t now_ms);
	bool (*udp)(
		void *context,
		const AiusRuntimeBudget *budget,
		uint32_t now_ms);
	bool (*replay)(
		void *context,
		const AiusRuntimeBudget *budget,
		uint32_t now_ms);
	void (*checkpoint)(void *context, uint16_t checkpoint);
	void (*feed_watchdog)(void *context);
} AiusRuntimeTeensyPlatformOps;

typedef struct {
	AiusRuntimeNetworkHttp network_http;
	AiusRuntimeStorage storage;
	AiusRuntimeUsb usb;
	AiusOpticalAppDispatcher app;
	uint8_t app_body[AIUS_RUNTIME_TEENSY_APP_BODY];
	AiusRuntimeTeensyPlatformOps platform;
	AiusRuntimeTeensyBootStatus boot_status;
	/* The HTTP/storage bridge. The HTTP checkpoint (5) produces at most one
	 * SD-backed action; the SD checkpoint (8) consumes it in the same epoch,
	 * so it is carried here rather than inside either service. */
	AiusHttpAction storage_action;
	/* The epoch's clock sample. The portable scheduler's service callback
	 * carries only a budget, so the root latches `now_ms` for the services
	 * and platform callbacks that need it. */
	uint32_t epoch_now_ms;
	uint16_t last_checkpoint;
	bool recovery_safe;
	bool storage_action_present;
	bool valid;
	uint32_t completed_epochs;
} AiusRuntimeTeensy;

/* Memory only: constructs each service, copies the platform callbacks, adopts
 * `recovery_safe` as the scheduler mode for every epoch and initializes the
 * application dispatcher against the embedded body buffer. Performs no target
 * call. A null runtime/app table/CDC primitive table/platform table, an
 * incomplete CDC or platform table, or an application table the dispatcher
 * rejects leaves an invalid composition whose epoch returns false without a
 * checkpoint or feed. */
void aius_runtime_teensy_construct(
	AiusRuntimeTeensy *runtime,
	const AiusOpticalApp *app_ops,
	const AiusCdcTeensyPrimitives *cdc_primitives,
	const AiusRuntimeTeensyPlatformOps *platform_ops,
	uint32_t now_ms,
	bool recovery_safe);

/* One runtime epoch through the portable scheduler. Returns the scheduler
 * result: true only when the complete checkpoint trace ran and the sole feed
 * callback was invoked. `completed_epochs` increments, saturating, only then. */
bool aius_runtime_teensy_epoch(
	AiusRuntimeTeensy *runtime,
	uint32_t now_ms);

/* Pure snapshot of the retained boot facts, last checkpoint and network
 * counters. Performs no platform call. */
void aius_runtime_teensy_status(
	const AiusRuntimeTeensy *runtime,
	AiusRecoveryStatus *status);

} // extern "C"

#endif
