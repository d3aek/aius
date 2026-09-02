/* Bounded network + HTTP runtime service -- see aius_runtime_network_http.h.
 *
 * Every definition is FLASHMEM, on the definition rather than a prototype (GCC
 * does not inherit a `section` attribute from a forward declaration). Nothing
 * here runs per audio block; it runs at the NETWORK and HTTP checkpoints.
 */

#include "aius_runtime_network_http.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

FLASHMEM void http_init(AiusRuntimeNetworkHttp *service, uint32_t now_ms)
{
	/* Discard BEFORE init, or a response's completion hook is zeroed without
	 * ever firing. This function is the teardown path's only terminal: the
	 * socket is about to be released, so no send completes, nothing recycles
	 * and no close arrives -- the three places settle_sd would otherwise run.
	 * A recorder read lease armed on an in-flight 200 would then stay held
	 * for the rest of the boot and eventually stall recording when the bank's
	 * round-robin reached the leaked slot. Refuses a not-yet-valid
	 * transaction, so the construction-time call is a no-op. */
	aius_http_transaction_discard(&service->http);
	aius_http_transaction_init(&service->http, service->http_request,
	                           sizeof(service->http_request),
	                           service->http_response,
	                           sizeof(service->http_response), now_ms);
	memset(&service->pending_http_event, 0,
	       sizeof(service->pending_http_event));
	memset(&service->pending_application_response, 0,
	       sizeof(service->pending_application_response));
	service->http_event_present = false;
	service->application_response_present = false;
}

/* Retains one correlated transport result for the next HTTP step. Transport
 * losses are counted here, where every transport result is visible exactly
 * once -- the transaction's own last_fault records only a connection's first
 * fault, which would undercount repeated accept faults on a broken listener.
 * A CLOSED is a loss only mid-read or mid-write: accept never produces it,
 * and on CLOSE it means "already closed", a completed shutdown. */
FLASHMEM void retain_io(AiusRuntimeNetworkHttp *service,
                        const AiusHttpAction *action, AiusIoEvent io)
{
	if (io.result == AIUS_IO_FAULT ||
	    (io.result == AIUS_IO_CLOSED &&
	     (action->kind == AIUS_HTTP_ACTION_READ ||
	      action->kind == AIUS_HTTP_ACTION_WRITE)))
		service->http_client_drops++;
	memset(&service->pending_http_event, 0,
	       sizeof(service->pending_http_event));
	service->pending_http_event.kind = AIUS_HTTP_EVENT_IO_RESULT;
	service->pending_http_event.action_id = action->action_id;
	service->pending_http_event.completed_action = action->kind;
	service->pending_http_event.io = io;
	service->http_event_present = true;
}

FLASHMEM void retain_application_response(
	AiusRuntimeNetworkHttp *service, const AiusHttpAction *action,
	const AiusHttpApplicationResponse *response)
{
	service->pending_application_response = *response;
	service->application_response_present = true;
	memset(&service->pending_http_event, 0,
	       sizeof(service->pending_http_event));
	service->pending_http_event.kind =
		AIUS_HTTP_EVENT_APPLICATION_RESPONSE;
	service->pending_http_event.action_id = action->action_id;
	service->pending_http_event.completed_action = action->kind;
	service->pending_http_event.application_response =
		&service->pending_application_response;
	service->http_event_present = true;
}

FLASHMEM bool adapter_busy(const AiusRuntimeNetworkHttp *service)
{
	return service->target.active_action.kind != AIUS_NET_ACTION_NONE;
}

} // namespace

extern "C" {

FLASHMEM void aius_runtime_network_http_construct(
	AiusRuntimeNetworkHttp *service,
	uint32_t now_ms,
	bool recovery_safe)
{
	AiusNetAction initial;

	if (service == nullptr)
		return;
	memset(service, 0, sizeof(*service));
	aius_network_teensy_init(&service->target);
	http_init(service, now_ms);

	memset(&initial, 0, sizeof(initial));
	aius_net_supervisor_init(&service->supervisor, now_ms, recovery_safe,
	                         &initial);
	service->pending_network_action = initial;
	service->network_action_present = initial.kind != AIUS_NET_ACTION_NONE;
}

FLASHMEM bool aius_runtime_network_service(
	AiusRuntimeNetworkHttp *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms)
{
	AiusNetEvent event;

	if (service == nullptr || budget == nullptr)
		return false;
	if (budget->checkpoint != AIUS_BOOT_CHECKPOINT_NETWORK)
		return false;

	if (service->network_action_present || adapter_busy(service)) {
		AiusNetAction action;

		if (service->network_action_present) {
			action = service->pending_network_action;
			service->network_action_present = false;
			/* Teardown invalidates the generation before any
			 * physical cleanup: discard HTTP first so a stale
			 * transaction can never reach a released socket. */
			if (action.kind == AIUS_NET_ACTION_TEARDOWN) {
				http_init(service, now_ms);
				service->http_discarded = true;
			}
		} else {
			memset(&action, 0, sizeof(action));
			action.kind = AIUS_NET_ACTION_NONE;
		}
		memset(&service->pending_network_action, 0,
		       sizeof(service->pending_network_action));

		aius_network_teensy_apply(&service->target, now_ms, action,
		                          &event);
		if (event.kind != AIUS_NET_EVENT_NONE) {
			service->pending_network_event = event;
			service->network_event_present = true;
		}
		return true;
	}

	if (service->network_event_present) {
		AiusNetAction action;

		memset(&action, 0, sizeof(action));
		aius_net_supervisor_step(&service->supervisor, now_ms,
		                         service->pending_network_event,
		                         &action);
		service->network_event_present = false;
		memset(&service->pending_network_event, 0,
		       sizeof(service->pending_network_event));
		if (action.kind != AIUS_NET_ACTION_NONE) {
			service->pending_network_action = action;
			service->network_action_present = true;
		}
		return true;
	}

	aius_network_teensy_poll(&service->target, now_ms, &event);
	if (event.kind != AIUS_NET_EVENT_NONE) {
		service->pending_network_event = event;
		service->network_event_present = true;
		return true;
	}

	/* An idle poll still has to let the clock move the supervisor. The
	 * DHCP deadline and the backoff expiry are the supervisor's only
	 * time-driven transitions, and neither the adapter nor a queued callback
	 * produces an event for them: a lease that never arrives is silence, and
	 * a torn-down stack polls silently forever. Without this step the
	 * supervisor could never leave DHCP_WAIT or BACKOFF, which is exactly
	 * the unbounded recovery the plan exists to close. Still one adapter
	 * operation for the epoch. */
	{
		AiusNetAction action;

		memset(&action, 0, sizeof(action));
		aius_net_supervisor_step(&service->supervisor, now_ms, event,
		                        &action);
		if (action.kind != AIUS_NET_ACTION_NONE) {
			service->pending_network_action = action;
			service->network_action_present = true;
		}
	}
	return true;
}

FLASHMEM bool aius_runtime_http_service(
	AiusRuntimeNetworkHttp *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms,
	AiusOpticalAppDispatcher *app,
	const AiusHttpEvent *storage_event,
	AiusHttpAction *storage_action)
{
	AiusHttpEvent event;
	AiusHttpAction action;
	AiusHttpFault fault_before;

	if (service == nullptr || budget == nullptr || storage_action == nullptr)
		return false;
	if (budget->checkpoint != AIUS_BOOT_CHECKPOINT_HTTP)
		return false;

	memset(storage_action, 0, sizeof(*storage_action));
	storage_action->kind = AIUS_HTTP_ACTION_NONE;
	service->http_stepped = false;

	/* HTTP lives inside a published network generation. Outside one there is
	 * no listener to accept on, so the transaction is left untouched rather
	 * than driven into a protocol fault once per epoch. */
	if (!service->target.online)
		return true;

	memset(&event, 0, sizeof(event));
	if (service->http_event_present) {
		event = service->pending_http_event;
	} else if (storage_event != nullptr &&
	           storage_event->kind != AIUS_HTTP_EVENT_NONE) {
		event = *storage_event;
	}
	service->http_event_present = false;
	service->application_response_present = false;

	service->http_stepped = true;
	memset(&action, 0, sizeof(action));
	fault_before = service->http.last_fault;
	aius_http_transaction_step(&service->http, now_ms, event, &action);

	/* A changed last_fault is a connection's first fault, recorded inside
	 * this step (reset_client() clears it per accepted connection, so each
	 * connection contributes at most one transition). Fixed 4xx refusals
	 * are rejections; expired deadlines are the modern spelling of the
	 * legacy idle/stall client drop. TRANSPORT is deliberately absent --
	 * transport losses are counted per occurrence in retain_io(), and
	 * counting the transition too would double-count the same loss. */
	if (service->http.last_fault != fault_before) {
		switch (service->http.last_fault) {
		case AIUS_HTTP_FAULT_PROTOCOL:
		case AIUS_HTTP_FAULT_REQUEST_CAPACITY:
			service->http_rejections++;
			break;
		case AIUS_HTTP_FAULT_REQUEST_TIMEOUT:
		case AIUS_HTTP_FAULT_RESPONSE_TIMEOUT:
		case AIUS_HTTP_FAULT_UPLOAD_TIMEOUT:
		case AIUS_HTTP_FAULT_DOWNLOAD_TIMEOUT:
			service->http_client_drops++;
			break;
		default:
			break;
		}
	}

	switch (action.kind) {
	case AIUS_HTTP_ACTION_ACCEPT:
		retain_io(service, &action,
		          aius_network_teensy_accept(&service->target));
		break;
	case AIUS_HTTP_ACTION_READ:
		retain_io(service, &action,
		          aius_network_teensy_read_once(&service->target,
		                                        action.read_buffer,
		                                        action.length));
		break;
	case AIUS_HTTP_ACTION_WRITE:
		retain_io(service, &action,
		          aius_network_teensy_write_once(&service->target,
		                                         action.data,
		                                         action.length));
		break;
	case AIUS_HTTP_ACTION_CLOSE:
		retain_io(service, &action,
		          aius_network_teensy_close_once(&service->target));
		break;
	case AIUS_HTTP_ACTION_DISPATCH: {
		const AiusHttpApplicationResponse *response =
			aius_optical_app_http_dispatch(app, action.request);
		service->http_requests++;
		/* A null return means an invalid dispatcher or request, which
		 * cannot be expressed as an application response. Retaining no
		 * event makes the next step's NONE a protocol fault, i.e. the
		 * deterministic fixed 400 the transaction owns. */
		if (response != nullptr)
			retain_application_response(service, &action, response);
		break;
	}
	case AIUS_HTTP_ACTION_SD_START:
	case AIUS_HTTP_ACTION_SD_STEP:
	case AIUS_HTTP_ACTION_SD_CANCEL:
		*storage_action = action;
		break;
	case AIUS_HTTP_ACTION_NONE:
	default:
		break;
	}
	return true;
}

} // extern "C"
