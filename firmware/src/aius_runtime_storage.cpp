/* Bounded SD storage runtime service -- see aius_runtime_storage.h.
 *
 * Every definition is FLASHMEM, on the definition rather than a prototype (GCC
 * does not inherit a `section` attribute from a forward declaration). Nothing
 * here runs per audio block; it runs at the SD checkpoint.
 */

#include "aius_runtime_storage.h"

#include "aius_producer_yield.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif
#ifndef PROGMEM
#define PROGMEM
#endif

namespace {

/* Latches one failed (or refused) job into the diagnostic record. Stores only,
 * on a path that has already failed, so it costs nothing an epoch will notice.
 *
 * `refused` distinguishes the two shapes that reach the client as the same 500:
 * a job that ran and faulted, and an action refused before a job existed
 * because a root job owned the volume. In the refused case every job-shaped
 * field describes the job that was RUNNING, which is exactly what a reader
 * needs to know. */
FLASHMEM void latch_failure(AiusRuntimeStorage *service,
                            AiusRuntimeStorageOwner owner, bool refused,
                            uint32_t now_ms)
{
	AiusRuntimeStorageFailure *record = &service->diag.last_failure;

	if (record->sequence != UINT32_MAX)
		record->sequence++;
	record->at_ms = now_ms;
	record->owner = (uint8_t)owner;
	record->job_kind = (uint8_t)service->job.kind;
	record->fault = (uint8_t)service->job.last_fault;
	record->fault_phase = (uint8_t)service->job.fault_phase;
	record->job_state = (uint8_t)service->job.state;
	record->root_state = (uint8_t)service->root_state;
	record->draining = service->draining;
	record->refused = refused;
	record->driver_fault = service->job.last_driver_fault;
	record->adapter_fault = service->target.last_fault;
	record->mounts_in_job =
		service->target.mounts - service->job_mounts_at_start;
	record->longest_primitive_us = service->target.longest_duration_us;
	/* A USAGE_REFRESH's transferred is a FAT sector cursor, not bytes. */
	record->transferred =
		service->job.kind == AIUS_SD_JOB_USAGE_REFRESH
			? 0u
			: service->job.transferred;
	if (refused) {
		/* No job of the refused owner's ever existed, so the fault
		 * fields would describe the running root job's history rather
		 * than this refusal. Report the refusal itself. */
		record->fault = (uint8_t)AIUS_SD_FAULT_PROTOCOL;
		record->fault_phase = (uint8_t)AIUS_SD_PHASE_NONE;
		record->driver_fault = 0u;
	}
}

/* Job-start bookkeeping shared by both owners: the counters and the mount
 * baseline a later failure reports its delta against. */
FLASHMEM void note_job_start(AiusRuntimeStorage *service,
                             AiusRuntimeStorageOwner owner)
{
	service->job_owner = (uint8_t)owner;
	service->job_mounts_at_start = service->target.mounts;
	if (owner == AIUS_RUNTIME_STORAGE_OWNER_ROOT) {
		if (service->diag.root_jobs != UINT32_MAX)
			service->diag.root_jobs++;
		return;
	}
	if (service->diag.http_jobs != UINT32_MAX)
		service->diag.http_jobs++;
}

/* Appends one formatted fragment, same contract as the status serializer's own
 * helper: it advances `*length` only on a complete write, so a body that runs
 * out of capacity is refused rather than truncated mid-object.
 *
 * Phase 4f: aius_runtime_storage_diag_json is a section of the status render,
 * so like optical_main.cpp's status_append each append ends at a yield point
 * and the render's admission cost stays one append (F5). noinline: it is a
 * render-append yield-root row (producer-isolation W5 (3), stack-bound's
 * render chain). */
FLASHMEM __attribute__((noinline, format(printf, 4, 5))) bool diag_append(
	char *output, size_t capacity, size_t *length, const char *format, ...)
{
	va_list arguments;
	int written;

	if (*length >= capacity)
		return false;
	va_start(arguments, format);
	written = vsnprintf(output + *length, capacity - *length, format,
	                    arguments);
	va_end(arguments);
	(void)aius_producer_yield_point();
	if (written < 0 || (size_t)written >= capacity - *length)
		return false;
	*length += (size_t)written;
	return true;
}

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
FLASHMEM void publish_cancelled(AiusRuntimeStorage *service, uint32_t now_ms)
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
	if (service->diag.cancellations != UINT32_MAX)
		service->diag.cancellations++;
	/* A cancellation that carries a real fault is a failure the client sees
	 * as one, so it is attributed like any other; a clean cancel is not. */
	if (fault != AIUS_SD_FAULT_NONE)
		latch_failure(service,
		              service->draining ?
			              AIUS_RUNTIME_STORAGE_OWNER_DRAIN :
			              AIUS_RUNTIME_STORAGE_OWNER_HTTP,
		              false, now_ms);
}

FLASHMEM void publish_failed(AiusRuntimeStorage *service, uint32_t now_ms)
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
	if (service->diag.http_failures != UINT32_MAX)
		service->diag.http_failures++;
	latch_failure(service, AIUS_RUNTIME_STORAGE_OWNER_HTTP, false, now_ms);
}

/* Correlated refusal of an HTTP action that arrived while a root-initiated job
 * owns the service. The running job is not read or touched, so no fault it has
 * recorded so far can leak into the refusal. */
FLASHMEM void publish_busy(AiusRuntimeStorage *service, uint32_t now_ms)
{
	publish_http(service, AIUS_HTTP_SD_FAILED);
	service->pending_http_event.sd_fault = AIUS_SD_FAULT_PROTOCOL;
	service->pending_http_event.sd_driver_fault = 0u;
	/* The one counter that separates the two hypotheses this record exists
	 * to separate: the client's 500 came from the contention rule, not from
	 * the card. Latched before root_state can move on. */
	if (service->diag.busy_refusals != UINT32_MAX)
		service->diag.busy_refusals++;
	latch_failure(service, AIUS_RUNTIME_STORAGE_OWNER_HTTP, true, now_ms);
}

/* Settles a root-initiated job at its terminal action. A job with an HTTP owner
 * is left alone: its result travels through the correlated event instead. */
FLASHMEM void root_terminal(AiusRuntimeStorage *service, bool failed,
                            uint32_t now_ms)
{
	if (service->root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING)
		return;
	service->root_state = failed ? AIUS_RUNTIME_STORAGE_ROOT_FAILED :
	                               AIUS_RUNTIME_STORAGE_ROOT_COMPLETE;
	if (service->root_cancelling) {
		/* Phase 4g: a requested cancel is not a failure. */
		service->root_cancelling = false;
		if (failed && service->diag.cancellations != UINT32_MAX)
			service->diag.cancellations++;
		return;
	}
	if (!failed)
		return;
	/* Phase 4g: a CACHE_FILL the sink refused (capacity full, read-back
	 * mismatch) is a cache event, counted by the sink as fill_refusals: it
	 * must not replace the record of a real card fault. A CLOSE fault after
	 * it replaced the code (CLEANUP) and falls through. */
	if (service->job.kind == AIUS_SD_JOB_CACHE_FILL &&
	    service->job.last_fault == AIUS_SD_FAULT_IO &&
	    service->job.last_driver_fault == AIUS_RUNTIME_STORAGE_FILL_SINK_FAULT)
		return;
	/* Phase 4h FR-S3, S3: a chunk the input source refused (a PSRAM chunk
	 * CRC fault) is the store's fault, not the card's: the caller takes
	 * Q2's path and no root failure is counted. A CLOSE that then faults
	 * replaced the code (CLEANUP) and falls through. */
	if ((service->flags_4h & AIUS_RUNTIME_STORAGE_FLAG_ROOT_INPUT_FAULT) &&
	    (service->job.last_fault == AIUS_SD_FAULT_PROTOCOL ||
	     service->job.last_fault == AIUS_SD_FAULT_SHORT_IO))
		return;
	if (service->diag.root_failures != UINT32_MAX)
		service->diag.root_failures++;
	latch_failure(service, AIUS_RUNTIME_STORAGE_OWNER_ROOT, false, now_ms);
}

/* Phase 4h FR-S3: answers a root job's NEED_INPUT from the registered source
 * into the publication buffer, which no input-taking job publishes through
 * (copy-before-write forbids publish; a publishing upload is refused here).
 * Memory only. Any refusal reaches the engine as a failed supply, which
 * faults the job and routes it to its CLOSE. */
FLASHMEM void supply_root_input(AiusRuntimeStorage *service,
                                const AiusSdAction *action)
{
	uint8_t *scratch =
		service->sd_publish + AIUS_RUNTIME_STORAGE_INPUT_SCRATCH_OFFSET;
	size_t bytes = 0u;

	if (service->job.publish || action->length == 0u ||
	    action->length > AIUS_RUNTIME_STORAGE_TRANSFER) {
		(void)aius_sd_job_supply(&service->job, nullptr, 0u);
		return;
	}
	if (!aius_runtime_storage_root_input(&service->job, action->offset,
	                                     scratch, action->length,
	                                     &bytes)) {
		service->flags_4h |= AIUS_RUNTIME_STORAGE_FLAG_ROOT_INPUT_FAULT;
		(void)aius_sd_job_supply(&service->job, nullptr, 0u);
		return;
	}
	if (bytes != action->length) {
		(void)aius_sd_job_supply(&service->job, nullptr, 0u);
		return;
	}
	(void)aius_sd_job_supply(&service->job, scratch, bytes);
}

/* True when the action produced a correlated HTTP result (or resolved the
 * epoch's work); false when nothing observable happened and HTTP authority must
 * stay outstanding for a later epoch. */
FLASHMEM bool route_action(AiusRuntimeStorage *service, const AiusSdAction *action,
                           bool authoritative, uint32_t now_ms)
{
	switch (action->kind) {
	case AIUS_SD_ACTION_NONE:
		return false;

	case AIUS_SD_ACTION_NEED_INPUT:
		if (!authoritative) {
			/* Phase 4h FR-S3: a root job's input comes from the
			 * registered source, supplied now; advance() steps the
			 * job once more this epoch for the WRITE. Any other
			 * ownerless job (a drain) has no source and waits as
			 * before. */
			if (service->root_state ==
			    AIUS_RUNTIME_STORAGE_ROOT_RUNNING)
				supply_root_input(service, action);
			return true;
		}
		publish_http(service, AIUS_HTTP_SD_NEED_UPLOAD);
		service->pending_http_event.sd_length = action->length;
		return true;

	case AIUS_SD_ACTION_OUTPUT:
		if (!authoritative) {
			/* Phase 4g: a root CACHE_FILL's chunk goes to the sink;
			 * OK lets advance() take the next READ this epoch. The
			 * size is the whole file's (end_offset, total 0), also
			 * for a resumed fill's nonzero initial_offset. */
			const bool fill =
				service->root_state ==
					AIUS_RUNTIME_STORAGE_ROOT_RUNNING &&
				service->job.kind == AIUS_SD_JOB_CACHE_FILL;
			if (fill &&
			    aius_runtime_storage_root_output(
				    &service->job, action->buffer, action->length,
				    action->offset, service->job.end_offset)) {
				service->pending_event = sd_event(
					AIUS_SD_EVENT_OK, action->action_id, 0u);
				service->event_present = true;
				return true;
			}
			/* No HTTP owner: refuse the chunk so the job drains. A
			 * fill's sink refusal carries its own code, settled by
			 * root_terminal() as a cache event. */
			service->pending_event = sd_event(
				AIUS_SD_EVENT_FAULT, action->action_id,
				fill ? AIUS_RUNTIME_STORAGE_FILL_SINK_FAULT
				     : AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT);
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
		root_terminal(service, false, now_ms);
		if (!authoritative)
			return true;
		if (service->cancelling) {
			publish_cancelled(service, now_ms);
			return true;
		}
		publish_http(service, AIUS_HTTP_SD_COMPLETE);
		return true;

	case AIUS_SD_ACTION_FAIL:
		service->terminal_seen = true;
		/* Read before the flag is cleared: an ownerless failure is a
		 * drain's, and nothing else publishes it. */
		if (!authoritative &&
		    service->root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING &&
		    service->draining) {
			if (service->diag.drains != UINT32_MAX)
				service->diag.drains++;
			latch_failure(service,
			              AIUS_RUNTIME_STORAGE_OWNER_DRAIN, false,
			              now_ms);
		}
		service->draining = false;
		root_terminal(service, true, now_ms);
		if (!authoritative)
			return true;
		if (service->cancelling)
			publish_cancelled(service, now_ms);
		else
			publish_failed(service, now_ms);
		return true;

	case AIUS_SD_ACTION_CACHE_DROP:
		/* Phase 4h H1, CR4-B2: the copy-before-write read-back's cache
		 * drop is an adapter primitive like any other, run inside the
		 * SD window below; never SD. or the card directly. */
		break;

	default:
		break;
	}

	/* One driver primitive, its result retained for the next epoch. The SD
	 * window lets every card wait inside it (mounts, probes and OTA staging
	 * chunks included) run a pending producer pass through the SdFat hook
	 * (aius-sample-rate-following.md Phase 4e). */
	aius_producer_yield_window_open();
	service->pending_event =
		aius_sd_teensy_execute(&service->target, action);
	aius_producer_yield_window_close();
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
	/* One route_action call site, so it stays inlined here: `advance` is the
	 * storage execute's W2 windowed row (producer-isolation), and a second
	 * call site outlines it (Phase 4g link). */
	for (bool memory_hop_done = false;;) {
		/* Phase 4g: a fill chunk's OUTPUT is memory only (the sink copies
		 * and checks it), so its OK is delivered now and the job's next
		 * action, the next READ or the CLOSE, is this epoch's one
		 * primitive: one 512 B card read per epoch. Phase 4h FR-S3: a
		 * root job's NEED_INPUT is memory only too (the source supplies
		 * it), so the job's WRITE, or the CLOSE of a refused supply, is
		 * this epoch's one primitive. At most one hop per advance. */
		const bool root_running =
			!memory_hop_done && !authoritative &&
			service->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING;
		const bool fill_output =
			root_running && action.kind == AIUS_SD_ACTION_OUTPUT &&
			service->job.kind == AIUS_SD_JOB_CACHE_FILL;
		const bool root_input =
			root_running && action.kind == AIUS_SD_ACTION_NEED_INPUT;
		const bool routed =
			route_action(service, &action, authoritative, now_ms);
		if (!fill_output && !root_input)
			return routed;
		memory_hop_done = true;
		if (fill_output) {
			if (!service->event_present ||
			    service->pending_event.kind != AIUS_SD_EVENT_OK)
				return true;
			event = service->pending_event;
			service->event_present = false;
			memset(&service->pending_event, 0,
			       sizeof(service->pending_event));
		} else {
			event = sd_event(AIUS_SD_EVENT_NONE, 0u, 0u);
		}
		memset(&action, 0, sizeof(action));
		aius_sd_job_step(&service->job, now_ms, event, &action);
	}
}

/* Advances the ownerless job, a drain or a root-initiated job, by at most one
 * primitive. Phase 4f F2/F9: never deferred here. sd_fifo_defer() holds a
 * root job only at its start gate (optical_main.cpp); a started job steps
 * every epoch to its end, as a started HTTP job does, so it never parks as
 * ROOT_RUNNING with every SD route on sd_busy. Phase 4g G8 (owner,
 * 2026-10-10) is the one exception: an uncancelled CACHE_FILL root job's step
 * waits while the platform's fill gate holds; an HTTP SD request cancels a
 * paused fill, and a cancelled fill always steps. */
FLASHMEM void advance_ownerless(AiusRuntimeStorage *service, uint32_t now_ms)
{
	if ((!service->draining &&
	     service->root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING) ||
	    service->terminal_seen)
		return;
	const bool fill_paused =
		!service->draining &&
		service->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING &&
		service->job.kind == AIUS_SD_JOB_CACHE_FILL &&
		!service->root_cancelling &&
		aius_runtime_storage_fill_step_defer(&service->job);
	if (!fill_paused)
		(void)advance(service, now_ms, false);
}

/* ---- Phase 4h S8: the STORE upload sink ---- */

FLASHMEM bool flag_4h(const AiusRuntimeStorage *service, uint8_t bit)
{
	return (service->flags_4h & bit) != 0u;
}

FLASHMEM void set_flag_4h(AiusRuntimeStorage *service, uint8_t bit, bool on)
{
	if (on)
		service->flags_4h = (uint8_t)(service->flags_4h | bit);
	else
		service->flags_4h = (uint8_t)(service->flags_4h & ~bit);
}

FLASHMEM uint32_t store_total(const AiusRuntimeStorage *service)
{
	return (uint32_t)(service->store_progress >> 32);
}

FLASHMEM uint32_t store_supplied(const AiusRuntimeStorage *service)
{
	return (uint32_t)service->store_progress;
}

/* Bytes of the current chunk held in the accumulator: whole chunks are
 * pushed as they complete, so it is the supplied count's 512 B remainder. */
FLASHMEM uint32_t store_fill(const AiusRuntimeStorage *service)
{
	return store_supplied(service) % AIUS_RUNTIME_STORAGE_TRANSFER;
}

FLASHMEM uint8_t *store_chunk(AiusRuntimeStorage *service)
{
	return service->sd_publish + AIUS_RUNTIME_STORAGE_STORE_CHUNK_OFFSET;
}

/* A publishing job owns sd_publish until its terminal; the STORE accumulator
 * shares it, so a STORE waits rather than writing under it. */
FLASHMEM bool publish_buffer_busy(const AiusRuntimeStorage *service)
{
	return service->job.publish && !service->terminal_seen &&
	       (service->draining ||
	        service->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING);
}

FLASHMEM void store_abort(AiusRuntimeStorage *service)
{
	if (flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE))
		aius_runtime_storage_store_abort();
	set_flag_4h(service,
	            AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE |
	                    AIUS_RUNTIME_STORAGE_FLAG_STORE_FAILED,
	            false);
	service->store_progress = 0u;
}

FLASHMEM void store_publish_failed(AiusRuntimeStorage *service,
                                   AiusSdFault fault, uint32_t driver_fault,
                                   uint32_t now_ms)
{
	store_abort(service);
	publish_http(service, AIUS_HTTP_SD_FAILED);
	service->pending_http_event.sd_fault = fault;
	service->pending_http_event.sd_driver_fault = driver_fault;
	if (service->diag.http_failures != UINT32_MAX)
		service->diag.http_failures++;
	(void)now_ms;
}

/* Hands the accumulated chunk (a whole 512 B one, or the tail) to the sink. */
FLASHMEM bool store_push_chunk(AiusRuntimeStorage *service, uint32_t length)
{
	if (length == 0u)
		return true;
	return aius_runtime_storage_store_chunk(store_chunk(service), length);
}

/* One correlated HTTP action of a STORE upload. Memory only: no engine call,
 * no card call, no http_owned, no root_state. Always publishes a result. */
FLASHMEM void store_action(AiusRuntimeStorage *service,
                           const AiusHttpAction *http_action, uint32_t now_ms)
{
	switch (http_action->kind) {
	case AIUS_HTTP_ACTION_SD_START: {
		const AiusSdJobRequest *request = http_action->sd_request;

		/* A stale upload (a STORE START with one still in flight) is
		 * the transaction's defect; the old put is abandoned. */
		store_abort(service);
		set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_HTTP, true);
		service->output_outstanding = false;
		if (service->diag.http_jobs != UINT32_MAX)
			service->diag.http_jobs++;
		/* Nothing reaches the engine either way; a malformed request
		 * frees the app's reserved entry, a refused open has none. */
		set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE,
		            true);
		if (request->kind != AIUS_SD_JOB_UPLOAD || request->total == 0u ||
		    request->total > UINT32_MAX || request->publish) {
			store_publish_failed(service,
			                     AIUS_SD_FAULT_INVALID_REQUEST, 0u,
			                     now_ms);
			return;
		}
		if (!aius_runtime_storage_store_open(request)) {
			set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE,
			            false);
			store_publish_failed(service,
			                     AIUS_SD_FAULT_INVALID_REQUEST, 0u,
			                     now_ms);
			return;
		}
		service->store_progress = request->total << 32;
		publish_http(service, AIUS_HTTP_SD_STARTED);
		service->pending_http_event.sd_capacity =
			AIUS_RUNTIME_STORAGE_TRANSFER;
		return;
	}

	case AIUS_HTTP_ACTION_SD_CANCEL:
		store_abort(service);
		publish_http(service, AIUS_HTTP_SD_CANCELLED);
		service->pending_http_event.sd_fault = AIUS_SD_FAULT_NONE;
		service->pending_http_event.sd_driver_fault = 0u;
		if (service->diag.cancellations != UINT32_MAX)
			service->diag.cancellations++;
		return;

	case AIUS_HTTP_ACTION_SD_STEP:
	default:
		break;
	}

	if (!flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE)) {
		store_publish_failed(service, AIUS_SD_FAULT_PROTOCOL, 0u, now_ms);
		return;
	}
	switch (http_action->sd_operation) {
	case AIUS_HTTP_SD_OP_ADVANCE: {
		const uint32_t taken = store_supplied(service);
		const uint32_t total = store_total(service);
		if (flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_FAILED)) {
			store_publish_failed(
				service, AIUS_SD_FAULT_IO,
				AIUS_RUNTIME_STORAGE_STORE_SINK_FAULT, now_ms);
			return;
		}
		if (taken < total) {
			uint32_t want =
				AIUS_RUNTIME_STORAGE_TRANSFER - store_fill(service);
			/* A publishing job that started before this upload
			 * owns sd_publish; no byte is held (no publishing
			 * job starts while a STORE is active), so wait. */
			if (publish_buffer_busy(service)) {
				publish_http(service, AIUS_HTTP_SD_PROGRESS);
				return;
			}
			if (want > total - taken)
				want = total - taken;
			publish_http(service, AIUS_HTTP_SD_NEED_UPLOAD);
			service->pending_http_event.sd_length = want;
			return;
		}
		if (!store_push_chunk(service, store_fill(service)) ||
		    !aius_runtime_storage_store_commit(now_ms)) {
			store_publish_failed(
				service, AIUS_SD_FAULT_IO,
				AIUS_RUNTIME_STORAGE_STORE_SINK_FAULT, now_ms);
			return;
		}
		set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE,
		            false);
		service->store_progress = 0u;
		publish_http(service, AIUS_HTTP_SD_COMPLETE);
		return;
	}

	case AIUS_HTTP_SD_OP_SUPPLY_UPLOAD: {
		const uint32_t taken = store_supplied(service);
		const uint32_t fill = store_fill(service);
		const size_t length = http_action->length;
		if (flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_FAILED) ||
		    http_action->data == nullptr || length == 0u ||
		    length > AIUS_RUNTIME_STORAGE_TRANSFER - fill ||
		    length > store_total(service) - taken ||
		    publish_buffer_busy(service)) {
			set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_FAILED,
			            true);
		} else {
			memcpy(store_chunk(service) + fill, http_action->data,
			       length);
			service->store_progress += length;
			if (fill + length == AIUS_RUNTIME_STORAGE_TRANSFER &&
			    !store_push_chunk(service,
			                      AIUS_RUNTIME_STORAGE_TRANSFER))
				set_flag_4h(service,
				            AIUS_RUNTIME_STORAGE_FLAG_STORE_FAILED,
				            true);
		}
		/* The supply phase admits PROGRESS only; a refused chunk
		 * answers FAILED at the next ADVANCE. */
		publish_http(service, AIUS_HTTP_SD_PROGRESS);
		return;
	}

	default:
		store_publish_failed(service, AIUS_SD_FAULT_PROTOCOL, 0u, now_ms);
		return;
	}
}

} // namespace

extern "C" {

FLASHMEM void aius_runtime_storage_construct(AiusRuntimeStorage *service)
{
	if (service == nullptr)
		return;
	memset(&service->sd_transfer, 0, sizeof(service->sd_transfer));
	memset(&service->sd_publish, 0, sizeof(service->sd_publish));
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
	memset(&service->diag, 0, sizeof(service->diag));
	service->job_mounts_at_start = 0u;
	service->job_owner = (uint8_t)AIUS_RUNTIME_STORAGE_OWNER_NONE;
	service->root_cancelling = false;
	service->flags_4h = 0u;
	aius_sd_teensy_init(&service->target);
	aius_sd_job_init(&service->job, service->sd_transfer,
	                 sizeof(service->sd_transfer));
	/* Order matters: init clears the registration, so the publication buffer
	 * is wired after it. The transfer slice stays 512 bytes -- this only
	 * changes where a publishing job accumulates, never how much a single SD
	 * primitive moves. */
	aius_sd_job_set_publish_buffer(&service->job, service->sd_publish,
	                               sizeof(service->sd_publish));
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

/* Phase 4h weak defaults: no input source (true with 0 bytes, which fails the
 * job as a counted protocol fault) and no STORE sink (the open refuses). */
__attribute__((weak)) FLASHMEM bool aius_runtime_storage_root_input(
	const AiusSdJob *job, uint64_t offset, uint8_t *dst, size_t capacity,
	size_t *bytes)
{
	(void)job;
	(void)offset;
	(void)dst;
	(void)capacity;
	*bytes = 0u;
	return true;
}

__attribute__((weak)) FLASHMEM bool aius_runtime_storage_store_open(
	const AiusSdJobRequest *request)
{
	(void)request;
	return false;
}

__attribute__((weak)) FLASHMEM bool aius_runtime_storage_store_chunk(
	const uint8_t *data, size_t length)
{
	(void)data;
	(void)length;
	return false;
}

__attribute__((weak)) FLASHMEM bool aius_runtime_storage_store_commit(
	uint32_t now_ms)
{
	(void)now_ms;
	return false;
}

__attribute__((weak)) FLASHMEM void aius_runtime_storage_store_abort(void)
{
}

/* No restore runs in a build without optical_main.cpp's H10 steps. */
__attribute__((weak)) FLASHMEM void aius_runtime_storage_settings_restores(
	uint32_t *restores, uint32_t *faults)
{
	*restores = 0u;
	*faults = 0u;
}

FLASHMEM bool aius_runtime_storage_root_input_faulted(
	const AiusRuntimeStorage *service)
{
	return service != nullptr &&
	       service->root_state == AIUS_RUNTIME_STORAGE_ROOT_FAILED &&
	       flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_ROOT_INPUT_FAULT);
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
	if (service->http_owned ||
	    /* Phase 4h S8: a STORE upload's correlation owns no volume. */
	    (!flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_HTTP) &&
	     (service->http_authority || service->http_event_present)) ||
	    /* A publishing job's buffer holds the STORE's chunk. */
	    (request->publish &&
	     flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE)) ||
	    service->draining ||
	    service->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING)
		return false;

	service->cancelling = false;
	service->root_cancelling = false;
	set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_ROOT_INPUT_FAULT, false);
	service->terminal_seen = false;
	service->output_outstanding = false;
	/* The slot is a STORE's progress while one runs beside this job. */
	if (!flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_ACTIVE))
		service->output_action_id = 0u;
	service->event_present = false;
	memset(&service->pending_event, 0, sizeof(service->pending_event));
	service->root_state = AIUS_RUNTIME_STORAGE_ROOT_RUNNING;
	note_job_start(service, AIUS_RUNTIME_STORAGE_OWNER_ROOT);
	aius_sd_job_start(&service->job, request, now_ms, 0u);
	if (service->job.state == AIUS_SD_FAILED) {
		/* A rejected start still owes one FAIL notification; draining it
		 * here settles root_state and costs no primitive. */
		(void)advance(service, now_ms, false);
		return false;
	}
	return true;
}

__attribute__((weak)) FLASHMEM bool aius_runtime_storage_root_output(
	const AiusSdJob *job, const uint8_t *data, size_t length,
	uint64_t offset, uint64_t size)
{
	(void)job;
	(void)data;
	(void)length;
	(void)offset;
	(void)size;
	return false;
}

__attribute__((weak)) FLASHMEM bool aius_runtime_storage_fill_step_defer(
	const AiusSdJob *job)
{
	(void)job;
	return false;
}

FLASHMEM bool aius_runtime_storage_cancel_root(AiusRuntimeStorage *service)
{
	if (service == nullptr ||
	    service->root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING ||
	    service->terminal_seen)
		return false;
	service->root_cancelling = true;
	aius_sd_job_cancel(&service->job);
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

		/* Phase 4h S8: a STORE upload is diverted to the PSRAM sink
		 * before the engine and the contention rule: it reads and
		 * writes no card, so it runs beside a root job or a drain,
		 * which still takes this epoch's one primitive. */
		if ((http_action->kind == AIUS_HTTP_ACTION_SD_START &&
		     http_action->sd_request->install_mode ==
			     AIUS_SD_INSTALL_STORE) ||
		    (http_action->kind != AIUS_HTTP_ACTION_SD_START &&
		     flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_HTTP))) {
			store_action(service, http_action, now_ms);
			advance_ownerless(service, now_ms);
			*http_event = service->pending_http_event;
			return true;
		}
		if (http_action->kind == AIUS_HTTP_ACTION_SD_START) {
			/* A card request ends any STORE correlation (and
			 * abandons a stale put, a transaction defect). */
			store_abort(service);
			set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_HTTP,
			            false);
		}

		if (service->root_state ==
		    AIUS_RUNTIME_STORAGE_ROOT_RUNNING) {
			/* The other direction of the contention rule: refuse the
			 * action in-band instead of restarting or stepping the
			 * root job underneath its owner. */
			publish_busy(service, now_ms);
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
			note_job_start(service,
			               AIUS_RUNTIME_STORAGE_OWNER_HTTP);
			aius_sd_job_start(&service->job, http_action->sd_request,
			                  now_ms, 0u);
			if (service->job.state == AIUS_SD_FAILED) {
				/* A rejected start still owes one FAIL
				 * notification; draining it here keeps the job
				 * restartable and costs no primitive. */
				(void)advance(service, now_ms, false);
				publish_failed(service, now_ms);
				break;
			}
			publish_http(service, AIUS_HTTP_SD_STARTED);
			service->pending_http_event.sd_capacity =
				sizeof(service->sd_transfer);
			break;

		case AIUS_HTTP_ACTION_SD_CANCEL:
			service->cancelling = true;
			if (service->terminal_seen) {
				publish_cancelled(service, now_ms);
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
	} else {
		/* No HTTP action this epoch: advance the ownerless job -- a
		 * drain or a root-initiated job -- by one primitive. */
		advance_ownerless(service, now_ms);
	}

	if (service->http_event_present)
		*http_event = service->pending_http_event;
	return true;
}

FLASHMEM bool aius_runtime_storage_diag_json(
	const AiusRuntimeStorage *service,
	const AiusHttpTransaction *http,
	char *output,
	size_t capacity,
	size_t *length)
{
	const AiusRuntimeStorageFailure *failure;

	if (service == nullptr || output == nullptr || length == nullptr)
		return false;
	/* Every format below is a named PROGMEM array, never an inline literal:
	 * the Teensy link script copies .rodata (string literals included) into
	 * DTCM .data, and the canonical slot-a image must keep _VectorsRam at
	 * 0x20007c00 (Phase 4h memory clause). A named array, unlike PSTR(),
	 * keeps diag_append's format(printf) check. */
	static const char format_head[] PROGMEM =
		",\"sd_diag\":{\"http_jobs\":%lu,\"root_jobs\":%lu,"
		"\"http_faults\":%lu,\"root_faults\":%lu,"
		"\"busy_refusals\":%lu,\"cancels\":%lu,\"drains\":%lu,"
		"\"publish_dropped\":%lu,";
	static const char format_restores[] PROGMEM =
		"\"settings_restores\":%lu,"
		"\"settings_restore_faults\":%lu,";
	static const char format_adapter[] PROGMEM =
		"\"mounts\":%lu,\"driver_faults\":%lu,"
		"\"protocol_faults\":%lu,\"last_protocol_action\":%u,"
		"\"last_primitive_us\":%lu,"
		"\"longest_primitive_us\":%lu,";
	static const char format_ownership[] PROGMEM =
		"\"root_state\":%u,\"http_owned\":%s,"
		"\"draining\":%s,\"file_open\":%s,\"job_kind\":%u,"
		"\"job_state\":%u,";
	static const char format_progress[] PROGMEM =
		"\"upload_supplied\":%llu,\"job_transferred\":%llu,"
		"\"job_total\":%llu,\"last_failure\":";
	static const char format_failure_head[] PROGMEM =
		"{\"seq\":%lu,\"at_ms\":%lu,\"owner\":%u,"
		"\"refused\":%s,\"job_kind\":%u,\"job_state\":%u,"
		"\"fault\":%u,\"fault_phase\":%u,";
	static const char format_failure_tail[] PROGMEM =
		"\"driver_fault\":%lu,\"adapter_fault\":%lu,"
		"\"root_state\":%u,\"draining\":%s,"
		"\"mounts_in_job\":%lu,\"longest_primitive_us\":%lu,"
		"\"bytes\":%llu}}";

	failure = &service->diag.last_failure;
	if (!diag_append(output, capacity, length, format_head,
	                 (unsigned long)service->diag.http_jobs,
	                 (unsigned long)service->diag.root_jobs,
	                 (unsigned long)service->diag.http_failures,
	                 (unsigned long)service->diag.root_failures,
	                 (unsigned long)service->diag.busy_refusals,
	                 (unsigned long)service->diag.cancellations,
	                 (unsigned long)service->diag.drains,
	                 (unsigned long)service->publish_dropped))
		return false;
	/* Phase 4h H10: the mount restore's counters, owned by optical_main.cpp
	 * (its RESTORE steps), read through a link-time hook so this object
	 * grows by nothing. */
	{
		uint32_t restores = 0u;
		uint32_t restore_faults = 0u;
		aius_runtime_storage_settings_restores(&restores,
		                                       &restore_faults);
		if (!diag_append(output, capacity, length, format_restores,
		                 (unsigned long)restores,
		                 (unsigned long)restore_faults))
			return false;
	}
	/* The adapter's own totals, beside the service's: `mounts` is every
	 * SD.begin() the firmware has performed, so a probe cadence is visible
	 * as a rate rather than inferred from the clock. */
	if (!diag_append(output, capacity, length, format_adapter,
	                 (unsigned long)service->target.mounts,
	                 (unsigned long)service->target.driver_faults,
	                 (unsigned long)service->target.protocol_faults,
	                 (unsigned)service->target.last_protocol_action,
	                 (unsigned long)service->target.last_duration_us,
	                 (unsigned long)service->target.longest_duration_us))
		return false;
	/* Phase 4e: the transfer mode, the two non-fault failure classes (a
	 * not-found primitive and a mount with no card), the vendored driver's
	 * 1 s busy-timeout expiries, and the SD wait hook's own record (every
	 * hook entry, and the longest stretch inside a window with no hook in
	 * it; POST /api/clear resets that watermark). */
	{
		AiusProducerYieldDiag yield;
		aius_producer_yield_get_diag(&yield);
		/* sd_mode is the MOUNTED volume's mode ("none" while unmounted),
		 * sd_mode_requested the mode the next mount uses. A mounted
		 * volume's mode equals the requested one because the only mode
		 * setter (optical_main.cpp app_sd_set_mode) refuses with a file
		 * open, and with none open aius_sd_teensy_set_mode drops the
		 * mount, so a mode change always remounts before it is served. */
		/* PROGMEM: flash, not DTCM .rodata. On the canonical slot-a
		 * image this string as .rodata pushed the 1 KiB-aligned
		 * _VectorsRam past 0x20008000, costing the stack zone 1,024 B
		 * of padding. */
		static const char format_sd_yield[] PROGMEM =
			"\"sd_mode\":\"%s\",\"sd_mode_requested\":\"%s\","
			"\"open_misses\":%lu,"
			"\"absent_mounts\":%lu,\"busy_timeouts\":%lu,"
			"\"hook_calls\":%lu,"
			"\"sd_window_nonyield_max_us\":%lu,";
		const char *requested = service->target.dma ? "dma" : "fifo";
		if (!diag_append(output, capacity, length, format_sd_yield,
		                 service->target.mounted ? requested : "none",
		                 requested,
		                 (unsigned long)service->target.open_misses,
		                 (unsigned long)service->target.absent_mounts,
		                 (unsigned long)service->target.busy_timeouts,
		                 (unsigned long)yield.hook_calls,
		                 (unsigned long)yield.window_nonyield_max_us))
			return false;
	}
	/* Live ownership, so a status read taken during a stuck job says who
	 * holds the volume rather than only what failed last. */
	if (!diag_append(output, capacity, length, format_ownership,
	                 (unsigned)service->root_state,
	                 service->http_owned ? "true" : "false",
	                 service->draining ? "true" : "false",
	                 service->target.file_open ? "true" : "false",
	                 (unsigned)service->job.kind,
	                 (unsigned)service->job.state))
		return false;
	/* Live PROGRESS. Until this existed the surface could say a job was
	 * running and, on failure, how many bytes it had moved when it died --
	 * and nothing at all about a job that was merely slow. An upload was
	 * therefore auditable only in retrospect and only if it failed, which is
	 * the opposite of what an operator watching a multi-megabyte push needs.
	 * Recorded as the cheapest high-value observability fix in
	 * docs/plans/aius-ota-upgrade.md (§Observability, prerequisite 2).
	 *
	 * Three numbers, deliberately, because two of them answer different
	 * questions and the third is the denominator that turns a count into
	 * progress:
	 *
	 *   upload_supplied  bytes the HTTP transaction has handed DOWN to the
	 *                    job (AiusHttpTransaction::upload_supplied) -- how
	 *                    much of the request body has been consumed off the
	 *                    socket. Zero for every non-upload job kind, and
	 *                    reset per request by the transaction.
	 *   job_transferred  bytes the job has COMMITTED to the card
	 *                    (AiusSdJob::transferred) -- how much actually
	 *                    reached storage, for a download as well as an
	 *                    upload.
	 *   job_total        the job's requested transfer size, so a reader can
	 *                    compute a fraction without knowing Content-Length.
	 *
	 * supplied minus transferred is the in-flight slice, bounded by
	 * AIUS_SD_MAX_TRANSFER; a supplied count that advances while transferred
	 * stands still is a card that has stopped accepting writes, which is
	 * exactly the state the pre-existing counters could not distinguish from
	 * an idle board. All three are uint64 on the wire because both fields are
	 * uint64 in their owners and a multi-megabyte image is the point. */
	if (!diag_append(output, capacity, length, format_progress,
	                 (unsigned long long)(http != nullptr
	                                              ? http->upload_supplied
	                                              : 0u),
	                 (unsigned long long)service->job.transferred,
	                 (unsigned long long)service->job.total))
		return false;
	if (failure->sequence == 0u)
		return diag_append(output, capacity, length, "null}");
	if (!diag_append(output, capacity, length, format_failure_head,
	                 (unsigned long)failure->sequence,
	                 (unsigned long)failure->at_ms,
	                 (unsigned)failure->owner,
	                 failure->refused ? "true" : "false",
	                 (unsigned)failure->job_kind,
	                 (unsigned)failure->job_state,
	                 (unsigned)failure->fault,
	                 (unsigned)failure->fault_phase))
		return false;
	if (!diag_append(output, capacity, length, format_failure_tail,
	                 (unsigned long)failure->driver_fault,
	                 (unsigned long)failure->adapter_fault,
	                 (unsigned)failure->root_state,
	                 failure->draining ? "true" : "false",
	                 (unsigned long)failure->mounts_in_job,
	                 (unsigned long)failure->longest_primitive_us,
	                 (unsigned long long)failure->transferred))
		return false;
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
	/* Phase 4h S8, D4: a client dropped mid-body aborts its STORE put. */
	if (flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_HTTP)) {
		store_abort(service);
		set_flag_4h(service, AIUS_RUNTIME_STORAGE_FLAG_STORE_HTTP, false);
	}
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
