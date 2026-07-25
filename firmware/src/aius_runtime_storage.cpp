/* Bounded SD storage runtime service -- see aius_runtime_storage.h.
 *
 * Every definition is FLASHMEM, on the definition rather than a prototype (GCC
 * does not inherit a `section` attribute from a forward declaration). Nothing
 * here runs per audio block; it runs at the SD checkpoint.
 */

#include "aius_runtime_storage.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

FLASHMEM AiusSdEvent sd_event(AiusSdEventKind kind, uint64_t action_id,
                              uint32_t fault)
{
	AiusSdEvent event;
	memset(&event, 0, sizeof(event));
	event.kind = kind;
	event.action_id = action_id;
	event.fault = fault;
	return event;
}

FLASHMEM void clear_http_result(AiusRuntimeStorage *service)
{
	memset(&service->pending_http_event, 0,
	       sizeof(service->pending_http_event));
	service->http_event_present = false;
}

/* Fills the retained correlated result for the outstanding HTTP action. */
FLASHMEM void publish_http(AiusRuntimeStorage *service, AiusHttpSdEventKind kind)
{
	memset(&service->pending_http_event, 0,
	       sizeof(service->pending_http_event));
	service->pending_http_event.kind = AIUS_HTTP_EVENT_SD_RESULT;
	service->pending_http_event.action_id = service->http_action_id;
	service->pending_http_event.completed_action = service->http_action;
	service->pending_http_event.sd_kind = kind;
	service->http_event_present = true;
	service->http_authority = false;
	/* A terminal result ends the job, not just the action, so HTTP stops
	 * owning the service and a root-initiated start becomes admissible. */
	if (kind == AIUS_HTTP_SD_COMPLETE || kind == AIUS_HTTP_SD_FAILED ||
	    kind == AIUS_HTTP_SD_CANCELLED)
		service->http_owned = false;
}

/* Terminal cancellation result. The synthetic pair the transaction itself
 * injected to drain an unacknowledged download chunk is filtered out; every
 * other pre-existing fault, including a rollback or cleanup fault that replaced
 * CANCELLED, stays authoritative. */
FLASHMEM void publish_cancelled(AiusRuntimeStorage *service)
{
	const AiusSdResult *result = aius_sd_job_result(&service->job);
	AiusSdFault fault = AIUS_SD_FAULT_NONE;
	uint32_t driver_fault = 0u;

	if (result != nullptr) {
		fault = result->fault;
		driver_fault = result->driver_fault;
	}
	if (fault == AIUS_SD_FAULT_IO &&
	    driver_fault == AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT) {
		fault = AIUS_SD_FAULT_NONE;
		driver_fault = 0u;
	}
	if (fault == AIUS_SD_FAULT_NONE)
		driver_fault = 0u;
	publish_http(service, AIUS_HTTP_SD_CANCELLED);
	service->pending_http_event.sd_fault = fault;
	service->pending_http_event.sd_driver_fault = driver_fault;
}

FLASHMEM void publish_failed(AiusRuntimeStorage *service)
{
	const AiusSdResult *result = aius_sd_job_result(&service->job);
	AiusSdFault fault = AIUS_SD_FAULT_NONE;
	uint32_t driver_fault = 0u;

	if (result != nullptr) {
		fault = result->fault;
		driver_fault = result->driver_fault;
	}
	/* FAILED requires a nonzero typed fault. A terminal FAIL without one is
	 * a portable-engine contract break, reported as PROTOCOL rather than as
	 * a malformed event the transaction would fault on. */
	if (fault == AIUS_SD_FAULT_NONE)
		fault = AIUS_SD_FAULT_PROTOCOL;
	publish_http(service, AIUS_HTTP_SD_FAILED);
	service->pending_http_event.sd_fault = fault;
	service->pending_http_event.sd_driver_fault = driver_fault;
}

/* Correlated refusal of an HTTP action that arrived while a root-initiated job
 * owns the service. The running job is not read or touched, so no fault it has
 * recorded so far can leak into the refusal. */
FLASHMEM void publish_busy(AiusRuntimeStorage *service)
{
	publish_http(service, AIUS_HTTP_SD_FAILED);
	service->pending_http_event.sd_fault = AIUS_SD_FAULT_PROTOCOL;
	service->pending_http_event.sd_driver_fault = 0u;
}

/* Settles a root-initiated job at its terminal action. A job with an HTTP owner
 * is left alone: its result travels through the correlated event instead. */
FLASHMEM void root_terminal(AiusRuntimeStorage *service, bool failed)
{
	if (service->root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING)
		return;
	service->root_state = failed ? AIUS_RUNTIME_STORAGE_ROOT_FAILED :
	                               AIUS_RUNTIME_STORAGE_ROOT_COMPLETE;
}

/* True when the action produced a correlated HTTP result (or resolved the
 * epoch's work); false when nothing observable happened and HTTP authority must
 * stay outstanding for a later epoch. */
FLASHMEM bool route_action(AiusRuntimeStorage *service, const AiusSdAction *action,
                           bool authoritative)
{
	switch (action->kind) {
	case AIUS_SD_ACTION_NONE:
		return false;

	case AIUS_SD_ACTION_NEED_INPUT:
		if (!authoritative)
			return true;
		publish_http(service, AIUS_HTTP_SD_NEED_UPLOAD);
		service->pending_http_event.sd_length = action->length;
		return true;

	case AIUS_SD_ACTION_OUTPUT:
		if (!authoritative) {
			/* No HTTP owner: refuse the chunk so the job drains. */
			service->pending_event = sd_event(
				AIUS_SD_EVENT_FAULT, action->action_id,
				AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT);
			service->event_present = true;
			return true;
		}
		service->output_action_id = action->action_id;
		service->output_outstanding = true;
		publish_http(service, AIUS_HTTP_SD_DOWNLOAD_DATA);
		service->pending_http_event.sd_data = action->buffer;
		service->pending_http_event.sd_length = action->length;
		service->pending_http_event.sd_size =
			service->job.end_offset - service->job.initial_offset;
		return true;

	case AIUS_SD_ACTION_PUBLISH_CONFIG:
		/* Correlated application action. With a consumer registered the
		 * complete payload is handed over here and acknowledged; a
		 * refusal faults the job so the caller sees FAULT_PUBLISH rather
		 * than a success that published nothing. With no consumer the
		 * bytes are acknowledged and dropped, and the count makes the
		 * drop observable rather than silent. */
		if (service->publish.deliver != nullptr) {
			if (service->publish.deliver(service->publish.context,
			                             action->buffer,
			                             action->length)) {
				service->pending_event = sd_event(
					AIUS_SD_EVENT_OK, action->action_id, 0u);
			} else {
				service->pending_event = sd_event(
					AIUS_SD_EVENT_FAULT, action->action_id,
					AIUS_RUNTIME_STORAGE_PUBLISH_REJECT_FAULT);
			}
		} else {
			if (service->publish_dropped != UINT32_MAX)
				service->publish_dropped++;
			service->pending_event = sd_event(
				AIUS_SD_EVENT_OK, action->action_id, 0u);
		}
		service->event_present = true;
		if (authoritative)
			publish_http(service, AIUS_HTTP_SD_PROGRESS);
		return true;

	case AIUS_SD_ACTION_COMPLETE:
		service->terminal_seen = true;
		service->draining = false;
		root_terminal(service, false);
		if (!authoritative)
			return true;
		if (service->cancelling) {
			publish_cancelled(service);
			return true;
		}
		publish_http(service, AIUS_HTTP_SD_COMPLETE);
		return true;

	case AIUS_SD_ACTION_FAIL:
		service->terminal_seen = true;
		service->draining = false;
		root_terminal(service, true);
		if (!authoritative)
			return true;
		if (service->cancelling)
			publish_cancelled(service);
		else
			publish_failed(service);
		return true;

	default:
		break;
	}

	/* One driver primitive, its result retained for the next epoch. */
	service->pending_event =
		aius_sd_teensy_execute(&service->target, action);
	service->event_present = true;
	if (authoritative)
		publish_http(service, AIUS_HTTP_SD_PROGRESS);
	return true;
}

/* One bounded job advance: deliver at most one retained adapter result, take at
 * most one emitted action, and spend at most one SD primitive. */
FLASHMEM bool advance(AiusRuntimeStorage *service, uint32_t now_ms,
                      bool authoritative)
{
	AiusSdAction action;
	AiusSdEvent event;

	if (service->event_present) {
		event = service->pending_event;
		service->event_present = false;
		memset(&service->pending_event, 0,
		       sizeof(service->pending_event));
	} else {
		event = sd_event(AIUS_SD_EVENT_NONE, 0u, 0u);
	}

	memset(&action, 0, sizeof(action));
	aius_sd_job_step(&service->job, now_ms, event, &action);
	return route_action(service, &action, authoritative);
}

} // namespace

extern "C" {

FLASHMEM void aius_runtime_storage_construct(AiusRuntimeStorage *service)
{
	if (service == nullptr)
		return;
	memset(&service->sd_transfer, 0, sizeof(service->sd_transfer));
	memset(&service->pending_event, 0, sizeof(service->pending_event));
	memset(&service->pending_http_event, 0,
	       sizeof(service->pending_http_event));
	service->http_action_id = 0u;
	service->http_action = AIUS_HTTP_ACTION_NONE;
	service->event_present = false;
	service->http_event_present = false;
	service->http_authority = false;
	service->http_owned = false;
	service->output_action_id = 0u;
	service->output_outstanding = false;
	service->cancelling = false;
	service->terminal_seen = false;
	service->draining = false;
	service->publish_dropped = 0u;
	memset(&service->publish, 0, sizeof(service->publish));
	service->root_state = AIUS_RUNTIME_STORAGE_ROOT_IDLE;
	aius_sd_teensy_init(&service->target);
	aius_sd_job_init(&service->job, service->sd_transfer,
	                 sizeof(service->sd_transfer));
}

FLASHMEM void aius_runtime_storage_set_publish(
	AiusRuntimeStorage *service,
	const AiusRuntimeStoragePublish *ops)
{
	if (service == nullptr)
		return;
	if (ops == nullptr || ops->deliver == nullptr) {
		memset(&service->publish, 0, sizeof(service->publish));
		return;
	}
	service->publish = *ops;
}

FLASHMEM bool aius_runtime_storage_start_root_job(
	AiusRuntimeStorage *service,
	const AiusSdJobRequest *request,
	uint32_t now_ms)
{
	if (service == nullptr || request == nullptr)
		return false;
	/* Reject rather than queue or preempt: one job, one buffer, and an HTTP
	 * owner whose correlated authority must never be abandoned. */
	if (service->http_owned || service->http_authority ||
	    service->http_event_present || service->draining ||
	    service->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING)
		return false;

	service->cancelling = false;
	service->terminal_seen = false;
	service->output_outstanding = false;
	service->output_action_id = 0u;
	service->event_present = false;
	memset(&service->pending_event, 0, sizeof(service->pending_event));
	service->root_state = AIUS_RUNTIME_STORAGE_ROOT_RUNNING;
	aius_sd_job_start(&service->job, request, now_ms, 0u);
	if (service->job.state == AIUS_SD_FAILED) {
		/* A rejected start still owes one FAIL notification; draining it
		 * here settles root_state and costs no primitive. */
		(void)advance(service, now_ms, false);
		return false;
	}
	return true;
}

FLASHMEM const AiusSdResult *aius_runtime_storage_root_result(
	const AiusRuntimeStorage *service)
{
	if (service == nullptr ||
	    service->root_state == AIUS_RUNTIME_STORAGE_ROOT_IDLE ||
	    service->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING)
		return nullptr;
	return aius_sd_job_result(&service->job);
}

FLASHMEM bool aius_runtime_storage_service(
	AiusRuntimeStorage *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms,
	const AiusHttpAction *http_action,
	AiusHttpEvent *http_event)
{
	if (service == nullptr || budget == nullptr || http_event == nullptr)
		return false;
	if (budget->checkpoint != AIUS_BOOT_CHECKPOINT_SD)
		return false;

	memset(http_event, 0, sizeof(*http_event));

	if (http_action != nullptr &&
	    http_action->kind != AIUS_HTTP_ACTION_NONE) {
		if (http_action->kind != AIUS_HTTP_ACTION_SD_START &&
		    http_action->kind != AIUS_HTTP_ACTION_SD_STEP &&
		    http_action->kind != AIUS_HTTP_ACTION_SD_CANCEL)
			return false;
		if (service->http_authority || http_action->action_id == 0u)
			return false;
		if (http_action->kind == AIUS_HTTP_ACTION_SD_START &&
		    http_action->sd_request == nullptr)
			return false;
		if (http_action->kind == AIUS_HTTP_ACTION_SD_STEP &&
		    (http_action->sd_operation == AIUS_HTTP_SD_OP_NONE ||
		     (!service->output_outstanding &&
		      (http_action->sd_operation ==
		           AIUS_HTTP_SD_OP_ACK_DOWNLOAD ||
		       http_action->sd_operation ==
		           AIUS_HTTP_SD_OP_FAIL_DOWNLOAD))))
			return false;
		service->http_authority = true;
		service->http_action = http_action->kind;
		service->http_action_id = http_action->action_id;
		clear_http_result(service);

		if (service->root_state ==
		    AIUS_RUNTIME_STORAGE_ROOT_RUNNING) {
			/* The other direction of the contention rule: refuse the
			 * action in-band instead of restarting or stepping the
			 * root job underneath its owner. */
			publish_busy(service);
			*http_event = service->pending_http_event;
			return true;
		}
		service->http_owned = true;

		switch (http_action->kind) {
		case AIUS_HTTP_ACTION_SD_START:
			service->cancelling = false;
			service->terminal_seen = false;
			service->draining = false;
			service->output_outstanding = false;
			service->output_action_id = 0u;
			service->event_present = false;
			memset(&service->pending_event, 0,
			       sizeof(service->pending_event));
			aius_sd_job_start(&service->job, http_action->sd_request,
			                  now_ms, 0u);
			if (service->job.state == AIUS_SD_FAILED) {
				/* A rejected start still owes one FAIL
				 * notification; draining it here keeps the job
				 * restartable and costs no primitive. */
				(void)advance(service, now_ms, false);
				publish_failed(service);
				break;
			}
			publish_http(service, AIUS_HTTP_SD_STARTED);
			service->pending_http_event.sd_capacity =
				sizeof(service->sd_transfer);
			break;

		case AIUS_HTTP_ACTION_SD_CANCEL:
			service->cancelling = true;
			if (service->terminal_seen) {
				publish_cancelled(service);
				break;
			}
			if (service->output_outstanding) {
				/* Resolve the borrowed chunk before cancelling
				 * so its correlated authority is not abandoned. */
				service->pending_event = sd_event(
					AIUS_SD_EVENT_FAULT,
					service->output_action_id,
					AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT);
				service->event_present = true;
				service->output_outstanding = false;
			}
			aius_sd_job_cancel(&service->job);
			publish_http(service, AIUS_HTTP_SD_PROGRESS);
			break;

		case AIUS_HTTP_ACTION_SD_STEP:
		default:
			switch (http_action->sd_operation) {
			case AIUS_HTTP_SD_OP_ADVANCE:
				(void)advance(service, now_ms, true);
				break;
			case AIUS_HTTP_SD_OP_SUPPLY_UPLOAD:
				(void)aius_sd_job_supply(&service->job,
				                         http_action->data,
				                         http_action->length);
				publish_http(service, AIUS_HTTP_SD_PROGRESS);
				break;
			case AIUS_HTTP_SD_OP_ACK_DOWNLOAD:
				service->pending_event = sd_event(
					AIUS_SD_EVENT_OK,
					service->output_action_id, 0u);
				service->event_present = true;
				service->output_outstanding = false;
				(void)advance(service, now_ms, true);
				break;
			case AIUS_HTTP_SD_OP_FAIL_DOWNLOAD:
				service->pending_event = sd_event(
					AIUS_SD_EVENT_FAULT,
					service->output_action_id,
					http_action->fault);
				service->event_present = true;
				service->output_outstanding = false;
				service->cancelling = true;
				(void)advance(service, now_ms, true);
				break;
			case AIUS_HTTP_SD_OP_NONE:
			default:
				return false;
			}
			break;
		}
	} else if ((service->draining ||
	            service->root_state ==
	                    AIUS_RUNTIME_STORAGE_ROOT_RUNNING) &&
	           !service->terminal_seen) {
		/* No HTTP action this epoch: advance the ownerless job -- a
		 * drain or a root-initiated job -- by one primitive. */
		(void)advance(service, now_ms, false);
	}

	if (service->http_event_present)
		*http_event = service->pending_http_event;
	return true;
}

FLASHMEM void aius_runtime_storage_release_http(AiusRuntimeStorage *service)
{
	if (service == nullptr)
		return;
	clear_http_result(service);
	service->http_authority = false;
	service->http_action = AIUS_HTTP_ACTION_NONE;
	service->http_action_id = 0u;
	/* Only an HTTP-owned job loses its owner here. A root-initiated job has
	 * none to lose, so a network teardown must not cancel it. */
	if (!service->http_owned)
		return;
	service->http_owned = false;
	if (service->terminal_seen ||
	    service->job.state == AIUS_SD_IDLE)
		return;
	if (service->output_outstanding) {
		service->pending_event =
			sd_event(AIUS_SD_EVENT_FAULT, service->output_action_id,
			         AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT);
		service->event_present = true;
		service->output_outstanding = false;
	}
	service->cancelling = true;
	service->draining = true;
	aius_sd_job_cancel(&service->job);
}

} // extern "C"
