/* Composed Teensy runtime -- see aius_runtime_teensy.h.
 *
 * Every definition is FLASHMEM, on the definition rather than a prototype (GCC
 * does not inherit a `section` attribute from a forward declaration). Nothing
 * here runs per audio block; it runs once per runtime epoch.
 */

#include "aius_runtime_teensy.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

FLASHMEM bool platform_complete(const AiusRuntimeTeensyPlatformOps *platform)
{
	return platform != nullptr && platform->reset_capture != nullptr &&
	       platform->metrics != nullptr && platform->udp != nullptr &&
	       platform->service != nullptr && platform->audio != nullptr &&
	       platform->checkpoint != nullptr &&
	       platform->feed_watchdog != nullptr;
}

FLASHMEM bool service_reset_capture(AiusRuntimeTeensy *runtime,
                                    const AiusRuntimeBudget *budget)
{
	AiusRuntimeTeensyBootStatus status;

	memset(&status, 0, sizeof(status));
	if (!runtime->platform.reset_capture(runtime->platform.context, budget,
	                                    runtime->epoch_now_ms, &status))
		return false;
	/* Publish only a complete capture: a failed callback must not leave a
	 * half-written boot status behind. */
	runtime->boot_status = status;
	return true;
}

FLASHMEM bool service_network(AiusRuntimeTeensy *runtime,
                              const AiusRuntimeBudget *budget)
{
	const bool ok = aius_runtime_network_service(&runtime->network_http,
	                                            budget,
	                                            runtime->epoch_now_ms);

	if (runtime->network_http.http_discarded) {
		/* Teardown discarded the transaction. Release the storage
		 * service's now-ownerless HTTP authority so a cancelled job
		 * drains instead of holding an open handle, and drop the
		 * in-epoch bridge slot. */
		runtime->network_http.http_discarded = false;
		aius_runtime_storage_release_http(&runtime->storage);
		memset(&runtime->storage_action, 0,
		       sizeof(runtime->storage_action));
		runtime->storage_action_present = false;
	}
	return ok;
}

FLASHMEM bool service_http(AiusRuntimeTeensy *runtime,
                           const AiusRuntimeBudget *budget)
{
	const AiusHttpEvent *storage_event =
		runtime->storage.http_event_present ?
			&runtime->storage.pending_http_event :
			nullptr;
	AiusHttpAction action;
	bool ok;

	memset(&action, 0, sizeof(action));
	ok = aius_runtime_http_service(&runtime->network_http, budget,
	                               runtime->epoch_now_ms, &runtime->app,
	                               storage_event, &action);
	if (storage_event != nullptr && runtime->network_http.http_stepped) {
		runtime->storage.http_event_present = false;
		memset(&runtime->storage.pending_http_event, 0,
		       sizeof(runtime->storage.pending_http_event));
	}
	if (ok && action.kind != AIUS_HTTP_ACTION_NONE) {
		runtime->storage_action = action;
		runtime->storage_action_present = true;
	}
	return ok;
}

FLASHMEM bool service_storage(AiusRuntimeTeensy *runtime,
                              const AiusRuntimeBudget *budget)
{
	AiusHttpEvent produced;
	const bool ok = aius_runtime_storage_service(
		&runtime->storage, budget, runtime->epoch_now_ms,
		runtime->storage_action_present ? &runtime->storage_action :
		                                 nullptr,
		&produced);

	memset(&runtime->storage_action, 0, sizeof(runtime->storage_action));
	runtime->storage_action_present = false;
	return ok;
}

FLASHMEM bool runtime_service(void *context, const AiusRuntimeBudget *budget)
{
	AiusRuntimeTeensy *runtime = static_cast<AiusRuntimeTeensy *>(context);

	if (runtime == nullptr || budget == nullptr)
		return false;

	switch (budget->checkpoint) {
	case AIUS_BOOT_CHECKPOINT_RESET_CAPTURE:
		return service_reset_capture(runtime, budget);
	case AIUS_BOOT_CHECKPOINT_USB_RX:
		return aius_runtime_usb_rx_service(&runtime->usb, budget);
	case AIUS_BOOT_CHECKPOINT_USB_TX:
		return aius_runtime_usb_tx_service(&runtime->usb, budget,
		                                   runtime->epoch_now_ms,
		                                   &runtime->app);
	case AIUS_BOOT_CHECKPOINT_METRICS:
		return runtime->platform.metrics(runtime->platform.context,
		                                 budget,
		                                 runtime->epoch_now_ms);
	case AIUS_BOOT_CHECKPOINT_NETWORK:
		return service_network(runtime, budget);
	case AIUS_BOOT_CHECKPOINT_HTTP:
		return service_http(runtime, budget);
	case AIUS_BOOT_CHECKPOINT_UDP:
		return runtime->platform.udp(runtime->platform.context, budget,
		                             runtime->epoch_now_ms);
	case AIUS_BOOT_CHECKPOINT_SERVICE:
		return runtime->platform.service(runtime->platform.context,
		                                budget,
		                                runtime->epoch_now_ms);
	case AIUS_BOOT_CHECKPOINT_AUDIO:
		return runtime->platform.audio(runtime->platform.context,
		                               budget,
		                               runtime->epoch_now_ms);
	case AIUS_BOOT_CHECKPOINT_SD:
		return service_storage(runtime, budget);
	default:
		return false;
	}
}

FLASHMEM void runtime_checkpoint(void *context, uint16_t checkpoint)
{
	AiusRuntimeTeensy *runtime = static_cast<AiusRuntimeTeensy *>(context);

	if (runtime == nullptr)
		return;
	runtime->last_checkpoint = checkpoint;
	runtime->platform.checkpoint(runtime->platform.context, checkpoint);
}

FLASHMEM void runtime_feed_watchdog(void *context)
{
	AiusRuntimeTeensy *runtime = static_cast<AiusRuntimeTeensy *>(context);

	if (runtime == nullptr)
		return;
	runtime->platform.feed_watchdog(runtime->platform.context);
}

} // namespace

extern "C" {

FLASHMEM void aius_runtime_teensy_construct(
	AiusRuntimeTeensy *runtime,
	const AiusOpticalApp *app_ops,
	const AiusCdcTeensyPrimitives *cdc_primitives,
	const AiusRuntimeTeensyPlatformOps *platform_ops,
	uint32_t now_ms,
	bool recovery_safe)
{
	bool app_valid;

	if (runtime == nullptr)
		return;

	memset(&runtime->app_body, 0, sizeof(runtime->app_body));
	memset(&runtime->platform, 0, sizeof(runtime->platform));
	memset(&runtime->boot_status, 0, sizeof(runtime->boot_status));
	memset(&runtime->storage_action, 0, sizeof(runtime->storage_action));
	runtime->epoch_now_ms = now_ms;
	runtime->last_checkpoint = AIUS_BOOT_CHECKPOINT_RESET_CAPTURE;
	runtime->recovery_safe = recovery_safe;
	runtime->storage_action_present = false;
	runtime->completed_epochs = 0u;
	runtime->valid = false;

	aius_runtime_network_http_construct(&runtime->network_http, now_ms,
	                                   recovery_safe);
	aius_runtime_storage_construct(&runtime->storage);
	aius_runtime_usb_construct(&runtime->usb, cdc_primitives);

	app_valid = aius_optical_app_init(&runtime->app, app_ops,
	                                 runtime->app_body,
	                                 sizeof(runtime->app_body));
	if (platform_ops != nullptr)
		runtime->platform = *platform_ops;
	if (runtime->platform.publish_config != nullptr) {
		AiusRuntimeStoragePublish publish;

		memset(&publish, 0, sizeof(publish));
		publish.context = runtime->platform.context;
		publish.deliver = runtime->platform.publish_config;
		aius_runtime_storage_set_publish(&runtime->storage, &publish);
	}

	runtime->valid = app_valid &&
	                 aius_cdc_teensy_primitives_complete(cdc_primitives) &&
	                 platform_complete(platform_ops);
}

FLASHMEM bool aius_runtime_teensy_epoch(
	AiusRuntimeTeensy *runtime,
	uint32_t now_ms)
{
	AiusRuntimeOps ops;
	bool completed;

	if (runtime == nullptr || !runtime->valid)
		return false;

	runtime->epoch_now_ms = now_ms;
	memset(&runtime->storage_action, 0, sizeof(runtime->storage_action));
	runtime->storage_action_present = false;

	memset(&ops, 0, sizeof(ops));
	ops.context = runtime;
	ops.service = runtime_service;
	ops.checkpoint = runtime_checkpoint;
	ops.feed_watchdog = runtime_feed_watchdog;

	completed = aius_runtime_epoch(&ops, runtime->recovery_safe);
	if (completed && runtime->completed_epochs != UINT32_MAX)
		runtime->completed_epochs++;
	return completed;
}

FLASHMEM bool aius_runtime_teensy_start_storage_job(
	AiusRuntimeTeensy *runtime,
	const AiusSdJobRequest *request)
{
	if (runtime == nullptr || !runtime->valid)
		return false;
	/* The root's latched clock sample, so a job started inside an epoch and
	 * one started between epochs agree with the services on `now`. */
	return aius_runtime_storage_start_root_job(&runtime->storage, request,
	                                           runtime->epoch_now_ms);
}

FLASHMEM void aius_runtime_teensy_status(
	const AiusRuntimeTeensy *runtime,
	AiusRecoveryStatus *status)
{
	if (status == nullptr)
		return;
	memset(status, 0, sizeof(*status));
	if (runtime == nullptr)
		return;
	status->network_state = runtime->network_http.supervisor.state;
	status->recovery_safe = runtime->recovery_safe;
	status->network_last_fault =
		runtime->network_http.supervisor.last_fault;
	status->last_checkpoint = runtime->last_checkpoint;
	status->network_generation =
		runtime->network_http.supervisor.generation;
	status->boot_generation = runtime->boot_status.boot_generation;
	status->reset_source = runtime->boot_status.reset_source;
	status->watchdog_early_resets =
		runtime->boot_status.watchdog_early_resets;
	status->loop_epoch = runtime->completed_epochs;
	status->network_restarts = runtime->network_http.supervisor.restarts;
}

} // extern "C"
