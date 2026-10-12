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

#include <stddef.h>
#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif
#ifndef PROGMEM
#define PROGMEM
#endif

/* Phase 4g: the held-DISPATCH flag took the padding byte before the counters,
 * so the OCRAM-resident service did not grow. */
static_assert(offsetof(AiusRuntimeNetworkHttp, http_requests) ==
	              offsetof(AiusRuntimeNetworkHttp, deferred_dispatch_present) + 1u,
              "deferred_dispatch_present must sit in the padding before the counters");

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
	service->deferred_start_present = false;
	service->deferred_dispatch_present = false;
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
	/* A peer close before any request byte is a browser closing its spare
	 * socket: counted nowhere (aius-sample-rate-following Phase 4n, owner
	 * 2026-10-12). */
	const bool idle_peer_close =
		io.result == AIUS_IO_CLOSED &&
		action->kind == AIUS_HTTP_ACTION_READ &&
		service->http.state == AIUS_HTTP_TX_RECEIVE &&
		service->http.request_length == 0u;

	if (io.result == AIUS_IO_FAULT ||
	    (io.result == AIUS_IO_CLOSED && !idle_peer_close &&
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

/* Transport fault code for a body source that failed or returned a wrong
 * length; the transaction closes the connection exactly as for any write
 * fault. */
const uint32_t kBodyChunkFault = 0xB0D1u;

/* One chunk-sourced body write. The 512 B chunk buffer lives only in this
 * frame (about 512 B plus ~40 B of saved registers and locals) and noinline
 * keeps it off every other path through the service step. The transport copies
 * the bytes during write_once, so the buffer need not outlive the call. */
__attribute__((noinline)) FLASHMEM AiusIoEvent write_body_chunk(
	AiusRuntimeNetworkHttp *service, const AiusHttpAction *action)
{
	uint8_t chunk[AIUS_HTTP_TX_QUOTA];
	uint32_t bytes = 0u;
	AiusIoEvent fault;

	if (!action->body_chunk(action->body_chunk_ctx, action->chunk_index, chunk,
	                        &bytes) ||
	    bytes != action->chunk_length ||
	    action->chunk_offset >= bytes ||
	    action->length > bytes - action->chunk_offset) {
		fault.result = AIUS_IO_FAULT;
		fault.bytes = 0u;
		fault.fault = kBodyChunkFault;
		return fault;
	}
	return aius_network_teensy_write_once(&service->target,
	                                      chunk + action->chunk_offset,
	                                      action->length);
}

/* Flash-resident through PROGMEM: the Teensy link script copies plain const
 * .rodata into DTCM .data, so `const` alone does not keep these in flash.
 * Shed route dispatches answer this without reaching the application;
 * Retry-After comes from aius_http_write_headers (503). */
const uint8_t kShedBody[] PROGMEM = "{\"ok\":false,\"error\":\"audio_busy\"}";

/* Flash-resident through PROGMEM. A DISPATCH refused by the SD-writes freeze
 * (BUSY, Phase 4h S8) answers this; it is not audio shedding, so no shed is
 * noted. */
const uint8_t kSdBusyBody[] PROGMEM = "{\"ok\":false,\"error\":\"sd_busy\"}";

/* Phase 4h S8: a www PUT into the PSRAM store reads and writes no card, so its
 * SD_START skips the admission hook (4g's fill cancel, 4f's F2 shed and F9
 * defer); the route's own admission already ran at DISPATCH. */
FLASHMEM bool sd_start_reads_no_card(const AiusSdJobRequest *sd_job)
{
	return sd_job != nullptr && sd_job->kind == AIUS_SD_JOB_UPLOAD &&
	       sd_job->install_mode == AIUS_SD_INSTALL_STORE;
}

FLASHMEM AiusHttpAdmitVerdict consult(const AiusRuntimeNetworkHttp *service,
                                      AiusHttpAdmitKind kind,
                                      const AiusHttpRequest *request,
                                      const AiusSdJobRequest *sd_job)
{
	AiusHttpAdmitQuery query;

	(void)service;
	memset(&query, 0, sizeof(query));
	query.kind = kind;
	if (request != nullptr) {
		query.route = request->route;
		query.method = request->method;
	}
	query.sd_job = sd_job;
	query.request = request;
	return aius_runtime_http_admit(&query);
}

/* Fails an outstanding SD_START with a 503: the correlated SD_FAILED result
 * the storage service would have produced. A SHED carries AUDIO_BUSY (503
 * audio_busy) and is noted; a BUSY (the SD-writes freeze) carries SD_BUSY
 * (503 sd_busy) and is not, so http_shed counts audio shedding only. BUSY
 * also comes from the BEGIN gate's WAIT. An UNAVAILABLE (the BEGIN gate's
 * NO_CARD) carries SD_UNAVAILABLE (503 sd_unavailable) and is not noted. */
FLASHMEM void refuse_sd_start(AiusRuntimeNetworkHttp *service,
                              const AiusHttpAction *action,
                              AiusHttpAdmitVerdict verdict)
{
	const bool shed = verdict == AIUS_HTTP_VERDICT_SHED;

	memset(&service->pending_http_event, 0,
	       sizeof(service->pending_http_event));
	service->pending_http_event.kind = AIUS_HTTP_EVENT_SD_RESULT;
	service->pending_http_event.action_id = action->action_id;
	service->pending_http_event.completed_action = AIUS_HTTP_ACTION_SD_START;
	service->pending_http_event.io.result = AIUS_IO_PROGRESS;
	service->pending_http_event.sd_kind = AIUS_HTTP_SD_FAILED;
	service->pending_http_event.sd_fault = AIUS_SD_FAULT_MEDIA;
	switch (verdict) {
	case AIUS_HTTP_VERDICT_SHED:
		service->pending_http_event.sd_driver_fault =
			AIUS_HTTP_DRIVER_FAULT_AUDIO_BUSY;
		break;
	case AIUS_HTTP_VERDICT_UNAVAILABLE:
		service->pending_http_event.sd_driver_fault =
			AIUS_HTTP_DRIVER_FAULT_SD_UNAVAILABLE;
		break;
	case AIUS_HTTP_VERDICT_BUSY:
		service->pending_http_event.sd_driver_fault =
			AIUS_HTTP_DRIVER_FAULT_SD_BUSY;
		break;
	case AIUS_HTTP_VERDICT_ADMIT:
	case AIUS_HTTP_VERDICT_DEFER:
		break; /* never refused */
	}
	service->http_event_present = true;
	if (shed)
		aius_runtime_http_note_shed();
}

/* One SD_START verdict applied, fresh or held. DEFER is the caller's. */
FLASHMEM void apply_sd_start(AiusRuntimeNetworkHttp *service,
                             const AiusHttpAction *action,
                             AiusHttpAdmitVerdict verdict,
                             AiusHttpAction *storage_action)
{
	switch (verdict) {
	case AIUS_HTTP_VERDICT_SHED:
	case AIUS_HTTP_VERDICT_BUSY:
	case AIUS_HTTP_VERDICT_UNAVAILABLE:
		refuse_sd_start(service, action, verdict);
		break;
	case AIUS_HTTP_VERDICT_ADMIT:
	case AIUS_HTTP_VERDICT_DEFER:
		*storage_action = *action;
		break;
	}
}

FLASHMEM bool adapter_busy(const AiusRuntimeNetworkHttp *service)
{
	return service->target.active_action.kind != AIUS_NET_ACTION_NONE;
}

/* One DISPATCH, fresh or held: a SHED answers the audio_busy 503 and a BUSY
 * the sd_busy 503, both without reaching the application, and only a SHED is
 * noted (BUSY also comes from the BEGIN gate's WAIT); an UNAVAILABLE is an
 * SD_START verdict and a ROUTE answers it as BUSY; a DEFER holds the dispatch (the transaction's DISPATCH action stays
 * outstanding and the next HTTP epoch asks again, without a step); an ADMIT
 * hands the request to the application. */
FLASHMEM void dispatch_route(AiusRuntimeNetworkHttp *service,
                             AiusOpticalAppDispatcher *app,
                             const AiusHttpAction *action)
{
	const AiusHttpApplicationResponse *response;
	const AiusHttpAdmitVerdict verdict =
		consult(service, AIUS_HTTP_ADMIT_ROUTE, action->request, nullptr);

	service->deferred_dispatch_present = verdict == AIUS_HTTP_VERDICT_DEFER;
	if (verdict == AIUS_HTTP_VERDICT_DEFER)
		return;
	switch (verdict) {
	case AIUS_HTTP_VERDICT_DEFER:
		return;
	case AIUS_HTTP_VERDICT_SHED:
	case AIUS_HTTP_VERDICT_BUSY:
	case AIUS_HTTP_VERDICT_UNAVAILABLE: {
		/* UNAVAILABLE is an SD_START verdict; a ROUTE reads it as BUSY. */
		const bool shed = verdict == AIUS_HTTP_VERDICT_SHED;
		AiusHttpApplicationResponse refusal;

		memset(&refusal, 0, sizeof(refusal));
		refusal.kind = AIUS_HTTP_APP_IMMEDIATE;
		refusal.status = 503u;
		refusal.content_type = "application/json";
		refusal.body = shed ? kShedBody : kSdBusyBody;
		refusal.body_length = shed ? sizeof(kShedBody) - 1u
		                           : sizeof(kSdBusyBody) - 1u;
		if (shed)
			aius_runtime_http_note_shed();
		retain_application_response(service, action, &refusal);
		return;
	}
	case AIUS_HTTP_VERDICT_ADMIT:
		break;
	}
	response = aius_optical_app_http_dispatch(app, action->request);
	/* A null return means an invalid dispatcher or request, which cannot be
	 * expressed as an application response. Retaining no event makes the
	 * next step's NONE a protocol fault, i.e. the deterministic fixed 400 the
	 * transaction owns. */
	if (response != nullptr)
		retain_application_response(service, action, response);
}

} // namespace

extern "C" {

/* Weak defaults; the platform's strong definitions win at link time. */
__attribute__((weak)) FLASHMEM AiusHttpAdmitVerdict aius_runtime_http_admit(
	const AiusHttpAdmitQuery *query)
{
	(void)query;
	return AIUS_HTTP_VERDICT_ADMIT;
}

__attribute__((weak)) FLASHMEM void aius_runtime_http_note_shed(void)
{
}

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

	/* A held SD start is re-consulted without stepping the transaction (its
	 * SD_START action is still outstanding). */
	if (service->deferred_start_present) {
		/* Re-issue the still-outstanding SD_START from the transaction. */
		AiusHttpAction held;
		AiusHttpAdmitVerdict verdict;

		memset(&held, 0, sizeof(held));
		held.kind = AIUS_HTTP_ACTION_SD_START;
		held.action_id = service->http.outstanding_action_id;
		held.sd_request = &service->http.sd_request;
		verdict = consult(service, AIUS_HTTP_ADMIT_SD_START, nullptr,
		                  held.sd_request);

		if (verdict == AIUS_HTTP_VERDICT_DEFER)
			return true;
		service->deferred_start_present = false;
		service->http_stepped = true;
		apply_sd_start(service, &held, verdict, storage_action);
		return true;
	}

	/* Likewise a held DISPATCH (counted once, when first issued): the
	 * transaction still waits on it, so it is re-issued from its state and
	 * nothing steps until the application has answered. */
	if (service->deferred_dispatch_present) {
		AiusHttpAction held;

		memset(&held, 0, sizeof(held));
		held.kind = AIUS_HTTP_ACTION_DISPATCH;
		held.action_id = service->http.outstanding_action_id;
		held.request = &service->http.parsed_request;
		dispatch_route(service, app, &held);
		return true;
	}

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
	 * are rejections; expired deadlines after a request byte (or in a later
	 * phase) are the modern spelling of the legacy stall client drop. An
	 * idle request deadline (no byte) is IDLE_CLOSE and counts nowhere.
	 * TRANSPORT is deliberately absent --
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
		case AIUS_HTTP_FAULT_IDLE_CLOSE:
			/* Normal browser spare socket: counted nowhere. */
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
		if (action.body_chunk != nullptr) {
			retain_io(service, &action, write_body_chunk(service, &action));
			break;
		}
		retain_io(service, &action,
		          aius_network_teensy_write_once(&service->target,
		                                         action.data,
		                                         action.length));
		break;
	case AIUS_HTTP_ACTION_CLOSE:
		retain_io(service, &action,
		          aius_network_teensy_close_once(&service->target));
		break;
	case AIUS_HTTP_ACTION_DISPATCH:
		service->http_requests++;
		dispatch_route(service, app, &action);
		break;
	case AIUS_HTTP_ACTION_SD_START: {
		const AiusHttpAdmitVerdict verdict =
			sd_start_reads_no_card(action.sd_request)
				? AIUS_HTTP_VERDICT_ADMIT
				: consult(service, AIUS_HTTP_ADMIT_SD_START, nullptr,
				          action.sd_request);

		if (verdict == AIUS_HTTP_VERDICT_DEFER)
			service->deferred_start_present = true;
		else
			apply_sd_start(service, &action, verdict, storage_action);
		break;
	}
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
