#include "aius_sd.h"

#include <limits.h>
#include <string.h>

/* Cold code -> FLASH. Nothing here runs on the audio block path (this is the
 * per-epoch SD job state machine), so every definition below carries FLASHMEM
 * and lands in FLASH instead of the shared FlexRAM/ITCM pool the audio pipeline
 * needs. FLASHMEM is Teensyduino's __attribute__((section(".flashmem"))) from
 * <avr/pgmspace.h>; TEENSYDUINO is a project-wide compiler -D, so the native
 * host suites fall through to the no-op and this file keeps its portable,
 * Arduino-free compile. The attribute must sit on each DEFINITION: GCC does not
 * merge a `section` attribute from a forward declaration, so a prototype-only
 * FLASHMEM silently leaves the code in ITCM. Enforced post-link by
 * test/itcm-placement/verify_target.py. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

#define AIUS_SD_MAX_TRANSFER 512u
/* Ids held back for the post-commit publication tail, which must be able to
 * finish after a committed install however close to exhaustion the lifetime is.
 * Sized against that tail's worst case at the feature-flag publish limit (256
 * one-byte reads plus open, size, close, publish and the two backup-cleanup
 * actions). Only AIUS_SD_PHASE_INSTALL_PUBLISH_* actions draw on it; a
 * CONFIG_LOAD emits unreserved actions throughout, so a publish buffer larger
 * than the transfer slice does not enlarge the tail this constant covers. */
#define AIUS_SD_RESERVED_ACTION_IDS UINT64_C(262)

typedef enum {
	AIUS_SD_EVENT_WAITING,
	AIUS_SD_EVENT_CONSUMED,
	AIUS_SD_EVENT_REJECTED
} AiusSdEventDisposition;

static void route_abort(AiusSdJob *job);
static void emit_for_phase(AiusSdJob *job, AiusSdAction *action);

static FLASHMEM void clear_action(AiusSdAction *action)
{
	if (action != NULL)
		memset(action, 0, sizeof(*action));
}

static FLASHMEM void clear_result(AiusSdResult *result)
{
	memset(result, 0, sizeof(*result));
}

static FLASHMEM bool job_is_quiescent(const AiusSdJob *job)
{
	if (job->state == AIUS_SD_IDLE)
		return true;
	return (job->state == AIUS_SD_DONE ||
	        job->state == AIUS_SD_FAILED) &&
	       job->terminal_emitted;
}

/* Where a publication accumulates and how much of it there is. With no publish
 * buffer registered both fall back to the job's own transfer buffer, which is
 * the delivered behaviour and what every caller that never registers one keeps.
 * Neither answer touches the transfer slice: `capacity` still bounds each READ
 * action, so the per-epoch SD primitive is the same size either way. */
static FLASHMEM uint8_t *publication_buffer(const AiusSdJob *job)
{
	return job->publish_buffer != NULL ? job->publish_buffer : job->buffer;
}

static FLASHMEM size_t publication_capacity(const AiusSdJob *job)
{
	return job->publish_buffer != NULL ? job->publish_buffer_capacity
	                                   : job->capacity;
}

/* Bytes still to read into the publication, clamped to one transfer slice. */
static FLASHMEM size_t publication_read_length(const AiusSdJob *job)
{
	uint64_t remaining =
		job->captured_size - (uint64_t)job->config_bytes;

	return remaining < (uint64_t)job->capacity ? (size_t)remaining
	                                           : job->capacity;
}

static FLASHMEM void set_phase(AiusSdJob *job, AiusSdPhase phase, AiusSdState state)
{
	job->phase = phase;
	job->state = state;
}

static FLASHMEM void record_fault(AiusSdJob *job,
	                 AiusSdFault fault,
	                 uint32_t driver_fault)
{
	bool replace = false;

	if (fault == AIUS_SD_FAULT_ROLLBACK) {
		replace = true;
	} else if (fault == AIUS_SD_FAULT_CLEANUP) {
		replace = job->last_fault != AIUS_SD_FAULT_ROLLBACK &&
		          job->last_fault != AIUS_SD_FAULT_CLEANUP;
	} else if (job->last_fault == AIUS_SD_FAULT_NONE) {
		replace = true;
	}

	if (replace) {
		job->last_fault = fault;
		job->last_driver_fault = driver_fault;
	}
	job->abort_requested = true;
}

static FLASHMEM void finish_failure(AiusSdJob *job)
{
	if (job->last_fault == AIUS_SD_FAULT_NONE)
		job->last_fault = AIUS_SD_FAULT_PROTOCOL;

	clear_result(&job->result);
	job->result.kind = AIUS_SD_RESULT_FAULT;
	job->result.transferred_bytes =
		job->kind == AIUS_SD_JOB_CONFIG_LOAD
			? (uint64_t)job->config_bytes
			: job->transferred;
	job->result.fault = job->last_fault;
	job->result.driver_fault = job->last_driver_fault;
	set_phase(job, AIUS_SD_PHASE_TERMINAL, AIUS_SD_FAILED);
}

static FLASHMEM void finish_success(AiusSdJob *job)
{
	clear_result(&job->result);

	switch (job->kind) {
	case AIUS_SD_JOB_PROBE:
		job->result.kind = AIUS_SD_RESULT_PROBE;
		break;
	case AIUS_SD_JOB_USAGE_REFRESH:
		job->result.kind = AIUS_SD_RESULT_USAGE;
		break;
	case AIUS_SD_JOB_CONFIG_LOAD:
	case AIUS_SD_JOB_UPLOAD:
	case AIUS_SD_JOB_DOWNLOAD:
	case AIUS_SD_JOB_REPLAY_REFILL:
		job->result.kind = AIUS_SD_RESULT_BYTES;
		job->result.transferred_bytes =
			job->kind == AIUS_SD_JOB_CONFIG_LOAD
				? (uint64_t)job->config_bytes
				: job->transferred;
		break;
	case AIUS_SD_JOB_DELETE:
		job->result.kind = AIUS_SD_RESULT_COMPLETE;
		break;
	default:
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		finish_failure(job);
		return;
	}

	set_phase(job, AIUS_SD_PHASE_TERMINAL, AIUS_SD_DONE);
}

static FLASHMEM void reset_for_start(AiusSdJob *job)
{
	uint8_t *buffer = job->buffer;
	size_t capacity = job->capacity;
	/* Registered once at construction, like the transfer buffer, so it must
	 * survive a restart the same way. */
	uint8_t *publish_buffer = job->publish_buffer;
	size_t publish_buffer_capacity = job->publish_buffer_capacity;
	uint64_t next_action_id = job->next_action_id;
	bool action_ids_exhausted = job->action_ids_exhausted;

	memset(job, 0, sizeof(*job));
	job->buffer = buffer;
	job->capacity = capacity;
	job->publish_buffer = publish_buffer;
	job->publish_buffer_capacity = publish_buffer_capacity;
	job->next_action_id = next_action_id;
	job->action_ids_exhausted = action_ids_exhausted;
	job->state = AIUS_SD_IDLE;
}

static FLASHMEM void reject_start(AiusSdJob *job, AiusSdFault fault)
{
	reset_for_start(job);
	job->last_fault = fault;
	job->abort_requested = true;
	finish_failure(job);
}

static FLASHMEM bool copy_canonical_path(const char *source,
	                        char destination[AIUS_SD_PATH_STORAGE])
{
	size_t length = 0u;

	if (source == NULL || source[0] != '/')
		return false;

	while (length < AIUS_SD_PATH_STORAGE && source[length] != '\0')
		length++;
	if (length == 0u || length >= AIUS_SD_PATH_STORAGE ||
	    length == 1u || source[length - 1u] == '/')
		return false;

	size_t segment_start = 1u;
	for (size_t index = 0u; index <= length; index++) {
		unsigned char byte = (unsigned char)source[index];

		if (index < length) {
			if (byte < 0x20u || byte == 0x7fu || byte == '\\')
				return false;
			if (byte != '/')
				continue;
		}

		if (index == segment_start)
			return false;
		size_t segment_length = index - segment_start;
		if ((segment_length == 1u &&
		     source[segment_start] == '.') ||
		    (segment_length == 2u &&
		     source[segment_start] == '.' &&
		     source[segment_start + 1u] == '.'))
			return false;
		segment_start = index + 1u;
	}

	for (size_t index = 0u; index <= length; index++)
		destination[index] = source[index];
	return true;
}

static FLASHMEM bool paths_are_distinct(const char *first,
	                       const char *second,
	                       const char *third)
{
	if (first != NULL && second != NULL && strcmp(first, second) == 0)
		return false;
	if (first != NULL && third != NULL && strcmp(first, third) == 0)
		return false;
	if (second != NULL && third != NULL && strcmp(second, third) == 0)
		return false;
	return true;
}

static FLASHMEM AiusSdFault validate_request(const AiusSdJob *job,
	                            const AiusSdJobRequest *request,
	                            uint32_t now_ms,
	                            uint32_t deadline_ms,
	                            char target[AIUS_SD_PATH_STORAGE],
	                            char temp[AIUS_SD_PATH_STORAGE],
	                            char backup[AIUS_SD_PATH_STORAGE])
{
	if (request == NULL)
		return AIUS_SD_FAULT_INVALID_REQUEST;
	if (request->kind < AIUS_SD_JOB_PROBE ||
	    request->kind > AIUS_SD_JOB_REPLAY_REFILL ||
	    request->install_mode < AIUS_SD_INSTALL_REPLACE ||
	    request->install_mode > AIUS_SD_INSTALL_FINALIZE_PART)
		return AIUS_SD_FAULT_INVALID_REQUEST;
	if (job->buffer == NULL || job->capacity == 0u ||
	    job->capacity > AIUS_SD_MAX_TRANSFER)
		return AIUS_SD_FAULT_CAPACITY;
	if (deadline_ms != 0u &&
	    (deadline_ms - now_ms) > (uint32_t)INT32_MAX)
		return AIUS_SD_FAULT_INVALID_REQUEST;
	if (request->total > (uint64_t)INT64_MAX ||
	    request->initial_offset > (uint64_t)INT64_MAX)
		return AIUS_SD_FAULT_OVERFLOW;
	if (request->total >
	    (uint64_t)INT64_MAX - request->initial_offset)
		return AIUS_SD_FAULT_OVERFLOW;

	target[0] = '\0';
	temp[0] = '\0';
	backup[0] = '\0';

	if (request->kind != AIUS_SD_JOB_UPLOAD &&
	    request->install_mode != AIUS_SD_INSTALL_REPLACE)
		return AIUS_SD_FAULT_INVALID_REQUEST;
	if (request->kind != AIUS_SD_JOB_DELETE &&
	    request->require_existing)
		return AIUS_SD_FAULT_INVALID_REQUEST;
	if (request->publish && request->publish_capacity == 0u)
		return AIUS_SD_FAULT_INVALID_REQUEST;
	if (!request->publish && request->publish_capacity != 0u)
		return AIUS_SD_FAULT_INVALID_REQUEST;

	switch (request->kind) {
	case AIUS_SD_JOB_PROBE:
	case AIUS_SD_JOB_USAGE_REFRESH:
		if (request->total != 0u || request->initial_offset != 0u ||
		    request->publish || request->content_type != NULL ||
		    request->target_path != NULL || request->temp_path != NULL ||
		    request->backup_path != NULL)
			return AIUS_SD_FAULT_INVALID_REQUEST;
		break;

	case AIUS_SD_JOB_CONFIG_LOAD:
		if (request->total != 0u || request->initial_offset != 0u ||
		    !request->publish || request->content_type == NULL ||
		    request->temp_path != NULL || request->backup_path != NULL)
			return AIUS_SD_FAULT_INVALID_REQUEST;
		/* The publication capacity, not the transfer slice: a registered
		 * publish buffer is what the accumulated bytes have to fit. */
		if (publication_capacity(job) < request->publish_capacity)
			return AIUS_SD_FAULT_CAPACITY;
		if (!copy_canonical_path(request->target_path, target))
			return AIUS_SD_FAULT_PATH;
		break;

	case AIUS_SD_JOB_DOWNLOAD:
		if (request->publish || request->content_type == NULL ||
		    request->temp_path != NULL || request->backup_path != NULL)
			return AIUS_SD_FAULT_INVALID_REQUEST;
		if (!copy_canonical_path(request->target_path, target))
			return AIUS_SD_FAULT_PATH;
		break;

	case AIUS_SD_JOB_DELETE:
		if (request->total != 0u || request->initial_offset != 0u ||
		    request->publish || request->content_type != NULL ||
		    request->temp_path != NULL || request->backup_path != NULL)
			return AIUS_SD_FAULT_INVALID_REQUEST;
		if (!copy_canonical_path(request->target_path, target))
			return AIUS_SD_FAULT_PATH;
		break;

	case AIUS_SD_JOB_REPLAY_REFILL:
		if (request->total == 0u || request->publish ||
		    request->content_type != NULL || request->temp_path != NULL ||
		    request->backup_path != NULL)
			return AIUS_SD_FAULT_INVALID_REQUEST;
		if (!copy_canonical_path(request->target_path, target))
			return AIUS_SD_FAULT_PATH;
		break;

	case AIUS_SD_JOB_UPLOAD:
		switch (request->install_mode) {
		case AIUS_SD_INSTALL_REPLACE:
			if (request->initial_offset != 0u ||
			    !copy_canonical_path(request->target_path, target) ||
			    !copy_canonical_path(request->temp_path, temp) ||
			    !copy_canonical_path(request->backup_path, backup))
				return request->initial_offset != 0u
					? AIUS_SD_FAULT_INVALID_REQUEST
					: AIUS_SD_FAULT_PATH;
			if (!paths_are_distinct(target, temp, backup))
				return AIUS_SD_FAULT_PATH;
			if (request->publish) {
				if (request->content_type == NULL ||
				    request->total == 0u ||
				    request->total > request->publish_capacity)
					return AIUS_SD_FAULT_INVALID_REQUEST;
				if (publication_capacity(job) <
				    request->publish_capacity)
					return AIUS_SD_FAULT_CAPACITY;
			} else if (request->content_type != NULL) {
				return AIUS_SD_FAULT_INVALID_REQUEST;
			}
			break;

		case AIUS_SD_INSTALL_APPEND_PART:
			if (request->publish ||
			    request->content_type != NULL ||
			    request->temp_path != NULL ||
			    request->backup_path != NULL)
				return AIUS_SD_FAULT_INVALID_REQUEST;
			if (!copy_canonical_path(request->target_path, target))
				return AIUS_SD_FAULT_PATH;
			break;

		case AIUS_SD_INSTALL_FINALIZE_PART:
			if (!copy_canonical_path(request->target_path, target) ||
			    !copy_canonical_path(request->temp_path, temp) ||
			    !copy_canonical_path(request->backup_path, backup))
				return AIUS_SD_FAULT_PATH;
			if (!paths_are_distinct(target, temp, backup))
				return AIUS_SD_FAULT_PATH;
			if (request->publish) {
				uint64_t final_size =
					request->initial_offset + request->total;
				if (request->content_type == NULL ||
				    final_size == 0u ||
				    final_size > request->publish_capacity)
					return AIUS_SD_FAULT_INVALID_REQUEST;
				if (publication_capacity(job) <
				    request->publish_capacity)
					return AIUS_SD_FAULT_CAPACITY;
			} else if (request->content_type != NULL) {
				return AIUS_SD_FAULT_INVALID_REQUEST;
			}
			break;
		}
		break;
	}

	return AIUS_SD_FAULT_NONE;
}

FLASHMEM void aius_sd_job_init(AiusSdJob *job, uint8_t *buffer, size_t capacity)
{
	if (job == NULL)
		return;
	memset(job, 0, sizeof(*job));
	job->state = AIUS_SD_IDLE;
	job->buffer = buffer;
	job->capacity = capacity;
	job->next_action_id = 1u;
}

FLASHMEM void aius_sd_job_set_publish_buffer(AiusSdJob *job,
	                           uint8_t *buffer,
	                           size_t capacity)
{
	if (job == NULL || !job_is_quiescent(job))
		return;
	if (buffer == NULL || capacity == 0u) {
		job->publish_buffer = NULL;
		job->publish_buffer_capacity = 0u;
		return;
	}
	job->publish_buffer = buffer;
	job->publish_buffer_capacity = capacity;
}

FLASHMEM void aius_sd_job_start(AiusSdJob *job,
	               const AiusSdJobRequest *request,
	               uint32_t now_ms,
	               uint32_t deadline_ms)
{
	char target[AIUS_SD_PATH_STORAGE];
	char temp[AIUS_SD_PATH_STORAGE];
	char backup[AIUS_SD_PATH_STORAGE];

	if (job == NULL)
		return;
	if (!job_is_quiescent(job)) {
		record_fault(job, AIUS_SD_FAULT_INVALID_REQUEST, 0u);
		if (job->outstanding_action == AIUS_SD_ACTION_NONE)
			route_abort(job);
		return;
	}
	if (job->action_ids_exhausted) {
		reject_start(job, AIUS_SD_FAULT_OVERFLOW);
		return;
	}

	AiusSdFault fault = validate_request(job, request, now_ms, deadline_ms,
	                                    target, temp, backup);
	if (fault != AIUS_SD_FAULT_NONE) {
		reject_start(job, fault);
		return;
	}
	if (job->next_action_id == 0u ||
	    job->next_action_id >
		    UINT64_MAX - AIUS_SD_RESERVED_ACTION_IDS) {
		reject_start(job, AIUS_SD_FAULT_OVERFLOW);
		return;
	}

	AiusSdJobRequest request_copy = *request;
	reset_for_start(job);
	job->kind = request_copy.kind;
	job->install_mode = request_copy.install_mode;
	memcpy(job->target_path, target, sizeof(target));
	memcpy(job->temp_path, temp, sizeof(temp));
	memcpy(job->backup_path, backup, sizeof(backup));
	job->content_type = request_copy.content_type;
	job->initial_offset = request_copy.initial_offset;
	job->total = request_copy.total;
	job->end_offset = request_copy.initial_offset + request_copy.total;
	job->deadline_ms = deadline_ms;
	job->publish = request_copy.publish;
	job->publish_capacity = request_copy.publish_capacity;
	job->require_existing =
		request_copy.kind == AIUS_SD_JOB_DELETE &&
		request_copy.require_existing;
	set_phase(job, AIUS_SD_PHASE_BEGIN, AIUS_SD_PREPARE);
}

FLASHMEM void aius_sd_job_cancel(AiusSdJob *job)
{
	if (job == NULL || job->state == AIUS_SD_IDLE ||
	    job->state == AIUS_SD_DONE || job->state == AIUS_SD_FAILED)
		return;
	job->cancel_requested = true;
}

static FLASHMEM bool is_replace_temp_disposable(const AiusSdJob *job)
{
	return job->kind == AIUS_SD_JOB_UPLOAD &&
	       job->install_mode == AIUS_SD_INSTALL_REPLACE &&
	       !job->installed &&
	       !job->install_failed;
}

static FLASHMEM void select_existence(AiusSdJob *job,
	                     AiusSdExistencePurpose purpose,
	                     AiusSdState state)
{
	job->existence_purpose = purpose;
	set_phase(job, AIUS_SD_PHASE_PATH_EXISTS, state);
}

static FLASHMEM void continue_after_committed_backup(AiusSdJob *job)
{
	job->backup_moved = false;
	if (job->publish) {
		job->config_bytes = 0u;
		set_phase(job, AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN,
		          AIUS_SD_PUBLISH);
	} else if (job->abort_requested) {
		finish_failure(job);
	} else {
		finish_success(job);
	}
}

static FLASHMEM void route_abort(AiusSdJob *job)
{
	if (!job->abort_requested ||
	    job->outstanding_action != AIUS_SD_ACTION_NONE ||
	    job->state == AIUS_SD_DONE ||
	    job->state == AIUS_SD_FAILED)
		return;

	if (job->installed) {
		if ((job->phase == AIUS_SD_PHASE_PATH_EXISTS &&
		     job->existence_purpose ==
			     AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP) ||
		    job->phase == AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP)
			return;
		if (job->file_open) {
			set_phase(job, AIUS_SD_PHASE_CLEANUP_CLOSE,
			          AIUS_SD_CANCEL_CLOSE);
			return;
		}
		if (job->backup_moved) {
			select_existence(
				job,
				AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
				AIUS_SD_CLEANUP);
			return;
		}
		finish_failure(job);
		return;
	}

	if (job->backup_moved) {
		set_phase(job, AIUS_SD_PHASE_ROLLBACK, AIUS_SD_ROLLBACK);
		return;
	}
	if (job->file_open) {
		set_phase(job, AIUS_SD_PHASE_CLEANUP_CLOSE,
		          AIUS_SD_CANCEL_CLOSE);
		return;
	}
	if (job->phase == AIUS_SD_PHASE_CLEANUP_TEMP ||
	    (job->phase == AIUS_SD_PHASE_PATH_EXISTS &&
	     job->existence_purpose == AIUS_SD_EXISTS_CLEANUP_TEMP))
		return;
	if (is_replace_temp_disposable(job)) {
		select_existence(job, AIUS_SD_EXISTS_CLEANUP_TEMP,
		                 AIUS_SD_CANCEL_CLEANUP);
		return;
	}
	finish_failure(job);
}

FLASHMEM bool aius_sd_job_supply(AiusSdJob *job,
	                const uint8_t *data,
	                size_t length)
{
	if (job == NULL)
		return false;
	if (job_is_quiescent(job))
		return false;

	bool correct_state =
		job->state == AIUS_SD_NEED_INPUT &&
		job->phase == AIUS_SD_PHASE_UPLOAD_INPUT &&
		job->outstanding_action == AIUS_SD_ACTION_NONE &&
		job->requested != 0u;
	if (!correct_state) {
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		route_abort(job);
		return false;
	}

	uint64_t remaining = job->total - job->transferred;
	if (length == 0u) {
		record_fault(job, AIUS_SD_FAULT_SHORT_IO, 0u);
		route_abort(job);
		return false;
	}
	if (data == NULL || length > job->requested ||
	    (uint64_t)length > remaining) {
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		route_abort(job);
		return false;
	}

	memcpy(job->buffer, data, length);
	job->buffer_offset = 0u;
	job->pending = length;
	job->requested = 0u;
	set_phase(job, AIUS_SD_PHASE_UPLOAD_WRITE, AIUS_SD_WRITE);
	return true;
}

FLASHMEM const AiusSdResult *aius_sd_job_result(const AiusSdJob *job)
{
	return job == NULL ? NULL : &job->result;
}

static FLASHMEM bool emit_correlated(AiusSdJob *job,
	                    AiusSdAction *action,
	                    AiusSdActionKind kind,
	                    bool reserved)
{
	if (job->action_ids_exhausted || job->next_action_id == 0u) {
		record_fault(job, AIUS_SD_FAULT_OVERFLOW, 0u);
		if (job->installed && job->file_open)
			set_phase(job, AIUS_SD_PHASE_CLEANUP_CLOSE,
			          AIUS_SD_CANCEL_CLOSE);
		else
			route_abort(job);
		return false;
	}
	if (!reserved &&
	    job->next_action_id >
		    UINT64_MAX - AIUS_SD_RESERVED_ACTION_IDS) {
		record_fault(job, AIUS_SD_FAULT_OVERFLOW, 0u);
		bool cleanup_publication =
			job->installed &&
			job->last_fault == AIUS_SD_FAULT_CLEANUP &&
			job->phase >= AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN &&
			job->phase <= AIUS_SD_PHASE_INSTALL_PUBLISH;
		if (cleanup_publication) {
			if (job->file_open)
				set_phase(job, AIUS_SD_PHASE_CLEANUP_CLOSE,
				          AIUS_SD_CANCEL_CLOSE);
			else
				finish_failure(job);
		} else {
			route_abort(job);
		}
		return false;
	}

	uint64_t action_id = job->next_action_id;
	if (action_id == UINT64_MAX) {
		job->action_ids_exhausted = true;
	} else {
		job->next_action_id = action_id + 1u;
	}

	job->outstanding_action = kind;
	job->outstanding_action_id = action_id;
	action->kind = kind;
	action->action_id = action_id;
	return true;
}

static FLASHMEM void emit_uncorrelated(AiusSdAction *action,
	                      AiusSdActionKind kind)
{
	action->kind = kind;
}

static FLASHMEM const char *upload_file_path(const AiusSdJob *job)
{
	return job->install_mode == AIUS_SD_INSTALL_APPEND_PART
		       ? job->target_path
		       : job->temp_path;
}

static FLASHMEM const char *mkdir_source_path(const AiusSdJob *job, uint8_t index)
{
	if (index == 0u)
		return job->target_path;
	if (index == 1u)
		return job->temp_path;
	return job->backup_path;
}

static FLASHMEM bool prepare_next_directory(AiusSdJob *job)
{
	uint8_t path_count =
		job->install_mode == AIUS_SD_INSTALL_APPEND_PART ? 1u : 3u;

	while (job->mkdir_path_index < path_count) {
		const char *path =
			mkdir_source_path(job, job->mkdir_path_index);
		size_t length = strlen(path);

		for (size_t index = job->mkdir_offset;
		     index < length;
		     index++) {
			if (path[index] != '/')
				continue;
			for (size_t copy = 0u; copy < index; copy++)
				job->directory_path[copy] = path[copy];
			job->directory_path[index] = '\0';
			job->mkdir_offset = (uint8_t)(index + 1u);
			return true;
		}

		job->mkdir_path_index++;
		job->mkdir_offset = 1u;
	}

	job->directory_path[0] = '\0';
	return false;
}

static FLASHMEM const char *existence_path(const AiusSdJob *job)
{
	switch (job->existence_purpose) {
	case AIUS_SD_EXISTS_MKDIR:
		if (job->state != AIUS_SD_MKDIR)
			return NULL;
		return job->directory_path[0] == '\0'
			       ? NULL
			       : job->directory_path;
	case AIUS_SD_EXISTS_DELETE:
		if (job->state != AIUS_SD_PREPARE)
			return NULL;
		return job->target_path[0] == '\0' ? NULL : job->target_path;
	case AIUS_SD_EXISTS_INSTALL_TARGET:
		if (job->state != AIUS_SD_CHECK_TARGET)
			return NULL;
		return job->target_path[0] == '\0' ? NULL : job->target_path;
	case AIUS_SD_EXISTS_REMOVE_OLD_BACKUP:
		if (job->state != AIUS_SD_BACKUP)
			return NULL;
		return job->backup_path[0] == '\0' ? NULL : job->backup_path;
	case AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP:
		if (job->state != AIUS_SD_CLEANUP)
			return NULL;
		return job->backup_path[0] == '\0' ? NULL : job->backup_path;
	case AIUS_SD_EXISTS_CLEANUP_TEMP:
		if (job->state != AIUS_SD_CANCEL_CLEANUP)
			return NULL;
		return job->temp_path[0] == '\0' ? NULL : job->temp_path;
	case AIUS_SD_EXISTS_NONE:
	default:
		return NULL;
	}
}

static FLASHMEM bool existence_uses_reserved_id(const AiusSdJob *job)
{
	return job->existence_purpose ==
		       AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP ||
	       job->existence_purpose == AIUS_SD_EXISTS_CLEANUP_TEMP;
}

static FLASHMEM void emit_existence(AiusSdJob *job, AiusSdAction *action)
{
	const char *path = existence_path(job);

	if (job->phase != AIUS_SD_PHASE_PATH_EXISTS || path == NULL) {
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		job->existence_purpose = AIUS_SD_EXISTS_NONE;
		route_abort(job);
		return;
	}
	if (emit_correlated(job, action, AIUS_SD_ACTION_EXISTS,
	                    existence_uses_reserved_id(job)))
		action->path = path;
}

static FLASHMEM void emit_upload_open(AiusSdJob *job, AiusSdAction *action)
{
	set_phase(job, AIUS_SD_PHASE_UPLOAD_OPEN, AIUS_SD_OPEN);
	AiusSdActionKind kind =
		job->install_mode == AIUS_SD_INSTALL_REPLACE
			? AIUS_SD_ACTION_OPEN_WRITE_BEGIN
			: AIUS_SD_ACTION_OPEN_APPEND;
	if (emit_correlated(job, action, kind, false))
		action->path = upload_file_path(job);
}

static FLASHMEM void emit_mkdir_or_next(AiusSdJob *job, AiusSdAction *action)
{
	if (job->directory_path[0] != '\0') {
		if (emit_correlated(job, action, AIUS_SD_ACTION_MKDIR, false))
			action->path = job->directory_path;
		return;
	}

	if (prepare_next_directory(job)) {
		select_existence(job, AIUS_SD_EXISTS_MKDIR,
		                 AIUS_SD_MKDIR);
		emit_existence(job, action);
		return;
	}

	if (job->install_mode == AIUS_SD_INSTALL_REPLACE) {
		emit_upload_open(job, action);
		return;
	}

	set_phase(job, AIUS_SD_PHASE_PART_SIZE_OPEN, AIUS_SD_OPEN);
	if (emit_correlated(job, action, AIUS_SD_ACTION_OPEN_READ, false))
		action->path = job->install_mode ==
				             AIUS_SD_INSTALL_APPEND_PART
			               ? job->target_path
			               : job->temp_path;
}

static FLASHMEM void emit_read(AiusSdJob *job,
	              AiusSdAction *action,
	              bool config_read,
	              bool reserved)
{
	size_t length;
	uint64_t offset;
	uint8_t *buffer;

	if (config_read) {
		/* config_bytes is the accumulation offset here: how much of the
		 * publication is already in the buffer, hence both where the next
		 * slice lands and what file offset it comes from. The slice is
		 * one transfer at most, so a publication larger than `capacity`
		 * costs more read actions rather than a larger primitive. */
		length = publication_read_length(job);
		offset = (uint64_t)job->config_bytes;
		buffer = publication_buffer(job) + job->config_bytes;
	} else {
		uint64_t absolute = job->initial_offset + job->transferred;
		uint64_t remaining = job->end_offset - absolute;
		length = remaining < job->capacity
			         ? (size_t)remaining
			         : job->capacity;
		offset = absolute;
		buffer = job->buffer;
	}

	if (emit_correlated(job, action, AIUS_SD_ACTION_READ, reserved)) {
		action->buffer = buffer;
		action->length = length;
		action->offset = offset;
	}
}

static FLASHMEM void emit_for_phase(AiusSdJob *job, AiusSdAction *action)
{
	switch (job->phase) {
	case AIUS_SD_PHASE_NONE:
		break;

	case AIUS_SD_PHASE_BEGIN:
		(void)emit_correlated(job, action, AIUS_SD_ACTION_BEGIN, false);
		break;

	case AIUS_SD_PHASE_PROBE_MEDIA:
		(void)emit_correlated(job, action,
		                      AIUS_SD_ACTION_MEDIA_PRESENT, false);
		break;

	case AIUS_SD_PHASE_CONFIG_OPEN:
		if (emit_correlated(job, action, AIUS_SD_ACTION_OPEN_READ,
		                    false))
			action->path = job->target_path;
		break;
	case AIUS_SD_PHASE_CONFIG_SIZE:
	case AIUS_SD_PHASE_PART_SIZE:
	case AIUS_SD_PHASE_FINAL_SIZE:
	case AIUS_SD_PHASE_DOWNLOAD_SIZE:
		(void)emit_correlated(job, action,
		                      AIUS_SD_ACTION_FILE_SIZE, false);
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE:
		(void)emit_correlated(job, action,
		                      AIUS_SD_ACTION_FILE_SIZE, true);
		break;
	case AIUS_SD_PHASE_CONFIG_READ:
		emit_read(job, action, true, false);
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH_READ:
		emit_read(job, action, true, true);
		break;
	case AIUS_SD_PHASE_CONFIG_CLOSE:
	case AIUS_SD_PHASE_PART_SIZE_CLOSE:
	case AIUS_SD_PHASE_UPLOAD_CLOSE:
	case AIUS_SD_PHASE_FINAL_SIZE_CLOSE:
	case AIUS_SD_PHASE_DOWNLOAD_CLOSE:
		(void)emit_correlated(job, action, AIUS_SD_ACTION_CLOSE, false);
		break;
	/* Single-shot: config_bytes has stopped being an offset and is now the
	 * complete published length, delivered in one action. */
	case AIUS_SD_PHASE_CONFIG_PUBLISH:
		if (emit_correlated(job, action,
		                    AIUS_SD_ACTION_PUBLISH_CONFIG, false)) {
			action->buffer = publication_buffer(job);
			action->length = job->config_bytes;
		}
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH:
		if (emit_correlated(job, action,
		                    AIUS_SD_ACTION_PUBLISH_CONFIG, true)) {
			action->buffer = publication_buffer(job);
			action->length = job->config_bytes;
		}
		break;

	case AIUS_SD_PHASE_USAGE_TOTAL:
		(void)emit_correlated(job, action,
		                      AIUS_SD_ACTION_TOTAL_SIZE, false);
		break;
	case AIUS_SD_PHASE_USAGE_USED:
		(void)emit_correlated(job, action,
		                      AIUS_SD_ACTION_USED_SIZE, false);
		break;

	case AIUS_SD_PHASE_PATH_EXISTS:
		emit_existence(job, action);
		break;
	case AIUS_SD_PHASE_MKDIR:
		emit_mkdir_or_next(job, action);
		break;

	case AIUS_SD_PHASE_PART_SIZE_OPEN:
		if (emit_correlated(job, action, AIUS_SD_ACTION_OPEN_READ,
		                    false))
			action->path =
				job->install_mode ==
						AIUS_SD_INSTALL_APPEND_PART
					? job->target_path
					: job->temp_path;
		break;

	case AIUS_SD_PHASE_UPLOAD_OPEN: {
		AiusSdActionKind kind =
			job->install_mode == AIUS_SD_INSTALL_REPLACE
				? AIUS_SD_ACTION_OPEN_WRITE_BEGIN
				: AIUS_SD_ACTION_OPEN_APPEND;
		if (emit_correlated(job, action, kind, false))
			action->path = upload_file_path(job);
		break;
	}
	case AIUS_SD_PHASE_UPLOAD_TRUNCATE:
		(void)emit_correlated(job, action,
		                      AIUS_SD_ACTION_TRUNCATE, false);
		break;
	case AIUS_SD_PHASE_UPLOAD_INPUT: {
		if (job->requested != 0u)
			break;
		uint64_t remaining = job->total - job->transferred;
		job->requested = remaining < job->capacity
			                 ? (size_t)remaining
			                 : job->capacity;
		emit_uncorrelated(action, AIUS_SD_ACTION_NEED_INPUT);
		action->length = job->requested;
		action->offset = job->initial_offset + job->transferred;
		break;
	}
	case AIUS_SD_PHASE_UPLOAD_WRITE:
		if (emit_correlated(job, action, AIUS_SD_ACTION_WRITE, false)) {
			action->buffer = job->buffer + job->buffer_offset;
			action->length = job->pending;
			action->offset =
				job->initial_offset + job->transferred;
		}
		break;

	case AIUS_SD_PHASE_FINAL_SIZE_OPEN:
		if (emit_correlated(job, action, AIUS_SD_ACTION_OPEN_READ,
		                    false))
			action->path = job->temp_path;
		break;

	case AIUS_SD_PHASE_DOWNLOAD_OPEN:
		if (emit_correlated(job, action, AIUS_SD_ACTION_OPEN_READ,
		                    false))
			action->path = job->target_path;
		break;
	case AIUS_SD_PHASE_DOWNLOAD_SEEK:
		if (emit_correlated(job, action, AIUS_SD_ACTION_SEEK, false))
			action->offset = job->initial_offset;
		break;
	case AIUS_SD_PHASE_DOWNLOAD_READ:
		emit_read(job, action, false, false);
		break;
	case AIUS_SD_PHASE_DOWNLOAD_OUTPUT:
		if (emit_correlated(job, action, AIUS_SD_ACTION_OUTPUT,
		                    false)) {
			action->buffer = job->buffer;
			action->length = job->pending;
			action->offset =
				job->initial_offset + job->transferred;
		}
		break;

	case AIUS_SD_PHASE_DELETE_REMOVE:
		if (emit_correlated(job, action, AIUS_SD_ACTION_REMOVE,
		                    false))
			action->path = job->target_path;
		break;

	case AIUS_SD_PHASE_INSTALL_REMOVE_OLD_BACKUP:
		if (emit_correlated(job, action, AIUS_SD_ACTION_REMOVE,
		                    false))
			action->path = job->backup_path;
		break;
	case AIUS_SD_PHASE_INSTALL_BACKUP:
		if (emit_correlated(job, action, AIUS_SD_ACTION_RENAME,
		                    false)) {
			action->path = job->target_path;
			action->path_to = job->backup_path;
		}
		break;
	case AIUS_SD_PHASE_INSTALL_RENAME:
		if (emit_correlated(job, action, AIUS_SD_ACTION_RENAME,
		                    false)) {
			action->path = job->temp_path;
			action->path_to = job->target_path;
		}
		break;
	case AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP:
		if (emit_correlated(job, action, AIUS_SD_ACTION_REMOVE, true))
			action->path = job->backup_path;
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN:
		if (emit_correlated(job, action, AIUS_SD_ACTION_OPEN_READ,
		                    true))
			action->path = job->target_path;
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE:
		(void)emit_correlated(job, action, AIUS_SD_ACTION_CLOSE, true);
		break;

	case AIUS_SD_PHASE_ROLLBACK:
		if (emit_correlated(job, action, AIUS_SD_ACTION_RENAME, true)) {
			action->path = job->backup_path;
			action->path_to = job->target_path;
		}
		break;
	case AIUS_SD_PHASE_CLEANUP_CLOSE:
		if (emit_correlated(job, action, AIUS_SD_ACTION_CLOSE, true))
			job->close_emitted = true;
		break;
	case AIUS_SD_PHASE_CLEANUP_TEMP:
		if (emit_correlated(job, action, AIUS_SD_ACTION_REMOVE, true))
			action->path = job->temp_path;
		break;

	case AIUS_SD_PHASE_TERMINAL:
		if (job->terminal_emitted)
			break;
		emit_uncorrelated(action,
		                  job->state == AIUS_SD_DONE
			                  ? AIUS_SD_ACTION_COMPLETE
			                  : AIUS_SD_ACTION_FAIL);
		if (action->kind == AIUS_SD_ACTION_FAIL)
			action->fault = (uint32_t)job->last_fault;
		job->terminal_emitted = true;
		break;
	}
}

static FLASHMEM void select_after_begin(AiusSdJob *job)
{
	switch (job->kind) {
	case AIUS_SD_JOB_PROBE:
		set_phase(job, AIUS_SD_PHASE_PROBE_MEDIA, AIUS_SD_PREPARE);
		break;
	case AIUS_SD_JOB_CONFIG_LOAD:
		set_phase(job, AIUS_SD_PHASE_CONFIG_OPEN, AIUS_SD_OPEN);
		break;
	case AIUS_SD_JOB_USAGE_REFRESH:
		set_phase(job, AIUS_SD_PHASE_USAGE_TOTAL, AIUS_SD_PREPARE);
		break;
	case AIUS_SD_JOB_UPLOAD:
		job->mkdir_path_index = 0u;
		job->mkdir_offset = 1u;
		job->directory_path[0] = '\0';
		set_phase(job, AIUS_SD_PHASE_MKDIR, AIUS_SD_MKDIR);
		break;
	case AIUS_SD_JOB_DOWNLOAD:
	case AIUS_SD_JOB_REPLAY_REFILL:
		set_phase(job, AIUS_SD_PHASE_DOWNLOAD_OPEN, AIUS_SD_OPEN);
		break;
	case AIUS_SD_JOB_DELETE:
		select_existence(job, AIUS_SD_EXISTS_DELETE,
		                 AIUS_SD_PREPARE);
		break;
	default:
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		break;
	}
}

static FLASHMEM void select_after_upload_close(AiusSdJob *job)
{
	if (job->install_mode == AIUS_SD_INSTALL_APPEND_PART) {
		finish_success(job);
	} else if (job->install_mode == AIUS_SD_INSTALL_FINALIZE_PART) {
		set_phase(job, AIUS_SD_PHASE_FINAL_SIZE_OPEN, AIUS_SD_OPEN);
	} else {
		select_existence(job, AIUS_SD_EXISTS_INSTALL_TARGET,
		                 AIUS_SD_CHECK_TARGET);
	}
}

static FLASHMEM void select_post_commit(AiusSdJob *job)
{
	if (job->backup_moved) {
		select_existence(
			job, AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
			AIUS_SD_CLEANUP);
	} else if (job->publish) {
		job->config_bytes = 0u;
		set_phase(job, AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN,
		          AIUS_SD_PUBLISH);
	} else if (job->abort_requested) {
		finish_failure(job);
	} else {
		finish_success(job);
	}
}

static FLASHMEM void clear_won_abort_at_commit(AiusSdJob *job)
{
	if (job->last_fault == AIUS_SD_FAULT_CANCELLED ||
	    job->last_fault == AIUS_SD_FAULT_TIMEOUT) {
		job->last_fault = AIUS_SD_FAULT_NONE;
		job->last_driver_fault = 0u;
		job->abort_requested = false;
	}
	job->cancel_requested = false;
}

static FLASHMEM void handle_ok_event(AiusSdJob *job)
{
	switch (job->phase) {
	case AIUS_SD_PHASE_BEGIN:
		select_after_begin(job);
		break;
	case AIUS_SD_PHASE_MKDIR:
		job->directory_path[0] = '\0';
		break;
	case AIUS_SD_PHASE_CONFIG_OPEN:
		job->file_open = true;
		set_phase(job, AIUS_SD_PHASE_CONFIG_SIZE, AIUS_SD_READ);
		break;
	case AIUS_SD_PHASE_CONFIG_CLOSE:
		job->file_open = false;
		set_phase(job, AIUS_SD_PHASE_CONFIG_PUBLISH,
		          AIUS_SD_PUBLISH);
		break;
	case AIUS_SD_PHASE_CONFIG_PUBLISH:
		finish_success(job);
		break;
	case AIUS_SD_PHASE_PART_SIZE_OPEN:
		job->file_open = true;
		set_phase(job, AIUS_SD_PHASE_PART_SIZE, AIUS_SD_READ);
		break;
	case AIUS_SD_PHASE_PART_SIZE_CLOSE:
		job->file_open = false;
		set_phase(job, AIUS_SD_PHASE_UPLOAD_OPEN, AIUS_SD_OPEN);
		break;
	case AIUS_SD_PHASE_UPLOAD_OPEN:
		job->file_open = true;
		if (job->install_mode == AIUS_SD_INSTALL_REPLACE) {
			set_phase(job, AIUS_SD_PHASE_UPLOAD_TRUNCATE,
			          AIUS_SD_OPEN);
			break;
		}
		if (job->total == job->transferred)
			set_phase(job, AIUS_SD_PHASE_UPLOAD_CLOSE,
			          AIUS_SD_CLEANUP);
		else
			set_phase(job, AIUS_SD_PHASE_UPLOAD_INPUT,
			          AIUS_SD_NEED_INPUT);
		break;
	case AIUS_SD_PHASE_UPLOAD_TRUNCATE:
		if (job->total == job->transferred)
			set_phase(job, AIUS_SD_PHASE_UPLOAD_CLOSE,
			          AIUS_SD_CLEANUP);
		else
			set_phase(job, AIUS_SD_PHASE_UPLOAD_INPUT,
			          AIUS_SD_NEED_INPUT);
		break;
	case AIUS_SD_PHASE_UPLOAD_CLOSE:
		job->file_open = false;
		select_after_upload_close(job);
		break;
	case AIUS_SD_PHASE_FINAL_SIZE_OPEN:
		job->file_open = true;
		set_phase(job, AIUS_SD_PHASE_FINAL_SIZE, AIUS_SD_READ);
		break;
	case AIUS_SD_PHASE_FINAL_SIZE_CLOSE:
		job->file_open = false;
		select_existence(job, AIUS_SD_EXISTS_INSTALL_TARGET,
		                 AIUS_SD_CHECK_TARGET);
		break;
	case AIUS_SD_PHASE_DOWNLOAD_OPEN:
		job->file_open = true;
		set_phase(job, AIUS_SD_PHASE_DOWNLOAD_SIZE, AIUS_SD_READ);
		break;
	case AIUS_SD_PHASE_DOWNLOAD_SEEK:
		if (job->end_offset == job->initial_offset)
			set_phase(job, AIUS_SD_PHASE_DOWNLOAD_CLOSE,
			          AIUS_SD_CLEANUP);
		else
			set_phase(job, AIUS_SD_PHASE_DOWNLOAD_READ,
			          AIUS_SD_READ);
		break;
	case AIUS_SD_PHASE_DOWNLOAD_OUTPUT:
		job->transferred += (uint64_t)job->pending;
		job->pending = 0u;
		job->buffer_offset = 0u;
		if (job->initial_offset + job->transferred ==
		    job->end_offset)
			set_phase(job, AIUS_SD_PHASE_DOWNLOAD_CLOSE,
			          AIUS_SD_CLEANUP);
		else
			set_phase(job, AIUS_SD_PHASE_DOWNLOAD_READ,
			          AIUS_SD_READ);
		break;
	case AIUS_SD_PHASE_DOWNLOAD_CLOSE:
		job->file_open = false;
		finish_success(job);
		break;
	case AIUS_SD_PHASE_DELETE_REMOVE:
		finish_success(job);
		break;
	case AIUS_SD_PHASE_INSTALL_REMOVE_OLD_BACKUP:
		set_phase(job, AIUS_SD_PHASE_INSTALL_BACKUP,
		          AIUS_SD_BACKUP);
		break;
	case AIUS_SD_PHASE_INSTALL_BACKUP:
		job->backup_moved = true;
		set_phase(job, AIUS_SD_PHASE_INSTALL_RENAME,
		          AIUS_SD_INSTALL);
		break;
	case AIUS_SD_PHASE_INSTALL_RENAME:
		job->installed = true;
		clear_won_abort_at_commit(job);
		select_post_commit(job);
		break;
	case AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP:
		continue_after_committed_backup(job);
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN:
		job->file_open = true;
		set_phase(job, AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE,
		          AIUS_SD_PUBLISH);
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE:
		job->file_open = false;
		set_phase(job, AIUS_SD_PHASE_INSTALL_PUBLISH,
		          AIUS_SD_PUBLISH);
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH:
		if (job->abort_requested)
			finish_failure(job);
		else
			finish_success(job);
		break;
	case AIUS_SD_PHASE_ROLLBACK:
		job->backup_moved = false;
		route_abort(job);
		break;
	case AIUS_SD_PHASE_CLEANUP_CLOSE:
		job->file_open = false;
		route_abort(job);
		break;
	case AIUS_SD_PHASE_CLEANUP_TEMP:
		finish_failure(job);
		break;
	default:
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		break;
	}
}

static FLASHMEM void handle_bool_event(AiusSdJob *job, bool value,
	                               AiusSdFilesystem filesystem)
{
	if (job->phase == AIUS_SD_PHASE_PROBE_MEDIA) {
		finish_success(job);
		job->result.media_present = value;
		/* The volume type belongs to a card that is there. An absent
		 * card reports UNKNOWN however the driver answered. */
		job->result.filesystem =
			value ? filesystem : AIUS_SD_FILESYSTEM_UNKNOWN;
		return;
	}
	if (job->phase != AIUS_SD_PHASE_PATH_EXISTS ||
	    job->existence_purpose == AIUS_SD_EXISTS_NONE ||
	    existence_path(job) == NULL) {
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		return;
	}

	AiusSdExistencePurpose purpose = job->existence_purpose;
	job->existence_purpose = AIUS_SD_EXISTS_NONE;

	switch (purpose) {
	case AIUS_SD_EXISTS_MKDIR:
		if (value)
			job->directory_path[0] = '\0';
		set_phase(job, AIUS_SD_PHASE_MKDIR, AIUS_SD_MKDIR);
		break;
	case AIUS_SD_EXISTS_DELETE:
		if (value)
			set_phase(job, AIUS_SD_PHASE_DELETE_REMOVE,
			          AIUS_SD_PREPARE);
		else if (!job->require_existing)
			finish_success(job);
		else {
			record_fault(job, AIUS_SD_FAULT_NOT_FOUND, 0u);
			finish_failure(job);
		}
		break;
	case AIUS_SD_EXISTS_INSTALL_TARGET:
		job->target_existed = value;
		if (value)
			select_existence(job,
			                 AIUS_SD_EXISTS_REMOVE_OLD_BACKUP,
			                 AIUS_SD_BACKUP);
		else
			set_phase(job, AIUS_SD_PHASE_INSTALL_RENAME,
			          AIUS_SD_INSTALL);
		break;
	case AIUS_SD_EXISTS_REMOVE_OLD_BACKUP:
		if (value)
			set_phase(job,
			          AIUS_SD_PHASE_INSTALL_REMOVE_OLD_BACKUP,
			          AIUS_SD_BACKUP);
		else
			set_phase(job, AIUS_SD_PHASE_INSTALL_BACKUP,
			          AIUS_SD_BACKUP);
		break;
	case AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP:
		if (value)
			set_phase(job, AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP,
			          AIUS_SD_CLEANUP);
		else
			continue_after_committed_backup(job);
		break;
	case AIUS_SD_EXISTS_CLEANUP_TEMP:
		if (value)
			set_phase(job, AIUS_SD_PHASE_CLEANUP_TEMP,
			          AIUS_SD_CANCEL_CLEANUP);
		else
			finish_failure(job);
		break;
	case AIUS_SD_EXISTS_NONE:
	default:
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		break;
	}
}

static FLASHMEM void handle_size_event(AiusSdJob *job, uint64_t size)
{
	switch (job->phase) {
	case AIUS_SD_PHASE_CONFIG_SIZE:
		job->captured_size = size;
		/* Both bounds still apply: the request's declared limit, and the
		 * buffer the bytes actually accumulate into. */
		if (size == 0u || size > job->publish_capacity ||
		    size > publication_capacity(job)) {
			record_fault(job, AIUS_SD_FAULT_CAPACITY, 0u);
		} else {
			job->config_bytes = 0u;
			set_phase(job, AIUS_SD_PHASE_CONFIG_READ,
			          AIUS_SD_READ);
		}
		break;
	case AIUS_SD_PHASE_USAGE_TOTAL:
		job->result.total_bytes = size;
		set_phase(job, AIUS_SD_PHASE_USAGE_USED, AIUS_SD_PREPARE);
		break;
	case AIUS_SD_PHASE_USAGE_USED:
		job->result.used_bytes = size;
		if (size > job->result.total_bytes)
			record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		else {
			uint64_t total = job->result.total_bytes;
			uint64_t used = job->result.used_bytes;
			finish_success(job);
			job->result.total_bytes = total;
			job->result.used_bytes = used;
		}
		break;
	case AIUS_SD_PHASE_PART_SIZE:
		job->captured_size = size;
		if (size != job->initial_offset)
			record_fault(job, AIUS_SD_FAULT_RANGE, 0u);
		else
			set_phase(job, AIUS_SD_PHASE_PART_SIZE_CLOSE,
			          AIUS_SD_CLEANUP);
		break;
	case AIUS_SD_PHASE_FINAL_SIZE:
		job->captured_size = size;
		if (size != job->end_offset)
			record_fault(job, AIUS_SD_FAULT_RANGE, 0u);
		else
			set_phase(job, AIUS_SD_PHASE_FINAL_SIZE_CLOSE,
			          AIUS_SD_CLEANUP);
		break;
	case AIUS_SD_PHASE_DOWNLOAD_SIZE:
		job->captured_size = size;
		if (job->initial_offset > size) {
			record_fault(job, AIUS_SD_FAULT_RANGE, 0u);
			break;
		}
		if (job->kind == AIUS_SD_JOB_REPLAY_REFILL) {
			uint64_t available = size - job->initial_offset;
			uint64_t selected =
				job->total < available ? job->total : available;
			job->end_offset = job->initial_offset + selected;
		} else if (job->total == 0u) {
			job->end_offset = size;
		} else if (job->end_offset > size) {
			record_fault(job, AIUS_SD_FAULT_RANGE, 0u);
			break;
		}
		set_phase(job, AIUS_SD_PHASE_DOWNLOAD_SEEK,
		          AIUS_SD_READ);
		break;
	case AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE:
		job->captured_size = size;
		if (size != job->end_offset || size == 0u ||
		    size > job->publish_capacity ||
		    size > publication_capacity(job)) {
			record_fault(job, AIUS_SD_FAULT_PUBLISH, 0u);
			set_phase(job, AIUS_SD_PHASE_CLEANUP_CLOSE,
			          AIUS_SD_CANCEL_CLOSE);
		} else {
			job->config_bytes = 0u;
			set_phase(job, AIUS_SD_PHASE_INSTALL_PUBLISH_READ,
			          AIUS_SD_PUBLISH);
		}
		break;
	default:
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		break;
	}
}

static FLASHMEM void handle_bytes_event(AiusSdJob *job, size_t bytes)
{
	if (bytes == 0u) {
		record_fault(job,
		             job->phase ==
				             AIUS_SD_PHASE_INSTALL_PUBLISH_READ
			             ? AIUS_SD_FAULT_PUBLISH
			             : AIUS_SD_FAULT_SHORT_IO,
		             0u);
		if (job->phase == AIUS_SD_PHASE_INSTALL_PUBLISH_READ)
			set_phase(job, AIUS_SD_PHASE_CLEANUP_CLOSE,
			          AIUS_SD_CANCEL_CLOSE);
		return;
	}

	switch (job->phase) {
	case AIUS_SD_PHASE_CONFIG_READ:
	case AIUS_SD_PHASE_INSTALL_PUBLISH_READ:
		job->config_bytes += bytes;
		if ((uint64_t)job->config_bytes == job->captured_size)
			set_phase(job,
			          job->phase == AIUS_SD_PHASE_CONFIG_READ
				          ? AIUS_SD_PHASE_CONFIG_CLOSE
				          : AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE,
			          job->phase == AIUS_SD_PHASE_CONFIG_READ
				          ? AIUS_SD_CLEANUP
				          : AIUS_SD_PUBLISH);
		break;
	case AIUS_SD_PHASE_UPLOAD_WRITE:
		job->buffer_offset += bytes;
		job->pending -= bytes;
		job->transferred += (uint64_t)bytes;
		if (job->pending != 0u)
			break;
		job->buffer_offset = 0u;
		if (job->transferred == job->total)
			set_phase(job, AIUS_SD_PHASE_UPLOAD_CLOSE,
			          AIUS_SD_CLEANUP);
		else
			set_phase(job, AIUS_SD_PHASE_UPLOAD_INPUT,
			          AIUS_SD_NEED_INPUT);
		break;
	case AIUS_SD_PHASE_DOWNLOAD_READ:
		job->pending = bytes;
		job->buffer_offset = 0u;
		set_phase(job, AIUS_SD_PHASE_DOWNLOAD_OUTPUT,
		          AIUS_SD_READ);
		break;
	default:
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		break;
	}
}

static FLASHMEM bool is_post_commit_publish_phase(AiusSdPhase phase)
{
	return phase >= AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN &&
	       phase <= AIUS_SD_PHASE_INSTALL_PUBLISH;
}

static FLASHMEM AiusSdFault map_driver_fault(const AiusSdJob *job,
	                            AiusSdActionKind action)
{
	if (job->phase == AIUS_SD_PHASE_PATH_EXISTS) {
		switch (job->existence_purpose) {
		case AIUS_SD_EXISTS_INSTALL_TARGET:
		case AIUS_SD_EXISTS_REMOVE_OLD_BACKUP:
			return AIUS_SD_FAULT_INSTALL;
		case AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP:
		case AIUS_SD_EXISTS_CLEANUP_TEMP:
			return AIUS_SD_FAULT_CLEANUP;
		case AIUS_SD_EXISTS_MKDIR:
		case AIUS_SD_EXISTS_DELETE:
		case AIUS_SD_EXISTS_NONE:
		default:
			return AIUS_SD_FAULT_IO;
		}
	}
	if (is_post_commit_publish_phase(job->phase)) {
		return job->phase == AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE
			       ? AIUS_SD_FAULT_CLEANUP
			       : AIUS_SD_FAULT_PUBLISH;
	}
	if (job->phase == AIUS_SD_PHASE_CLEANUP_CLOSE ||
	    job->phase == AIUS_SD_PHASE_CLEANUP_TEMP ||
	    job->phase == AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP)
		return AIUS_SD_FAULT_CLEANUP;
	if (job->phase == AIUS_SD_PHASE_ROLLBACK)
		return AIUS_SD_FAULT_ROLLBACK;
	if (action == AIUS_SD_ACTION_MEDIA_PRESENT)
		return AIUS_SD_FAULT_MEDIA;
	if (action == AIUS_SD_ACTION_OPEN_READ ||
	    action == AIUS_SD_ACTION_OPEN_WRITE_BEGIN ||
	    action == AIUS_SD_ACTION_OPEN_APPEND)
		return AIUS_SD_FAULT_OPEN;
	if (action == AIUS_SD_ACTION_TRUNCATE ||
	    action == AIUS_SD_ACTION_SEEK)
		return AIUS_SD_FAULT_SEEK;
	if (action == AIUS_SD_ACTION_RENAME)
		return AIUS_SD_FAULT_INSTALL;
	if (action == AIUS_SD_ACTION_PUBLISH_CONFIG)
		return AIUS_SD_FAULT_PUBLISH;
	return AIUS_SD_FAULT_IO;
}

static FLASHMEM void handle_fault_event(AiusSdJob *job,
	                       AiusSdActionKind action,
	                       uint32_t driver_fault)
{
	AiusSdPhase phase = job->phase;
	AiusSdExistencePurpose existence_purpose =
		job->existence_purpose;
	AiusSdFault fault = map_driver_fault(job, action);

	if (action == AIUS_SD_ACTION_CLOSE)
		job->file_open = false;
	if (phase == AIUS_SD_PHASE_INSTALL_RENAME)
		job->install_failed = true;

	record_fault(job, fault, driver_fault);

	if (phase == AIUS_SD_PHASE_BEGIN) {
		finish_failure(job);
		return;
	}
	if (phase == AIUS_SD_PHASE_PATH_EXISTS) {
		job->existence_purpose = AIUS_SD_EXISTS_NONE;
		if (existence_purpose ==
		    AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP) {
			continue_after_committed_backup(job);
		} else if (existence_purpose ==
		           AIUS_SD_EXISTS_CLEANUP_TEMP) {
			finish_failure(job);
		}
		return;
	}
	if (phase == AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP) {
		continue_after_committed_backup(job);
		return;
	}
	if (phase == AIUS_SD_PHASE_ROLLBACK ||
	    phase == AIUS_SD_PHASE_CLEANUP_TEMP ||
	    phase == AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN ||
	    phase == AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE ||
	    phase == AIUS_SD_PHASE_INSTALL_PUBLISH) {
		finish_failure(job);
		return;
	}
	if (phase == AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE ||
	    phase == AIUS_SD_PHASE_INSTALL_PUBLISH_READ) {
		set_phase(job, AIUS_SD_PHASE_CLEANUP_CLOSE,
		          AIUS_SD_CANCEL_CLOSE);
	}
}

static FLASHMEM bool event_is_empty_none(AiusSdEvent event)
{
	return event.kind == AIUS_SD_EVENT_NONE &&
	       event.action_id == 0u &&
	       event.bytes == 0u &&
	       event.size == 0u &&
	       !event.value &&
	       event.fault == 0u &&
	       event.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN;
}

static FLASHMEM AiusSdEventKind expected_event_kind(AiusSdActionKind action)
{
	switch (action) {
	case AIUS_SD_ACTION_MEDIA_PRESENT:
	case AIUS_SD_ACTION_EXISTS:
		return AIUS_SD_EVENT_BOOL;
	case AIUS_SD_ACTION_FILE_SIZE:
	case AIUS_SD_ACTION_TOTAL_SIZE:
	case AIUS_SD_ACTION_USED_SIZE:
		return AIUS_SD_EVENT_SIZE;
	case AIUS_SD_ACTION_READ:
	case AIUS_SD_ACTION_WRITE:
		return AIUS_SD_EVENT_BYTES;
	case AIUS_SD_ACTION_BEGIN:
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_OPEN_READ:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
	case AIUS_SD_ACTION_TRUNCATE:
	case AIUS_SD_ACTION_SEEK:
	case AIUS_SD_ACTION_OUTPUT:
	case AIUS_SD_ACTION_RENAME:
	case AIUS_SD_ACTION_REMOVE:
	case AIUS_SD_ACTION_CLOSE:
	case AIUS_SD_ACTION_PUBLISH_CONFIG:
		return AIUS_SD_EVENT_OK;
	default:
		return AIUS_SD_EVENT_NONE;
	}
}

static FLASHMEM size_t outstanding_byte_length(const AiusSdJob *job)
{
	if (job->outstanding_action == AIUS_SD_ACTION_WRITE)
		return job->pending;
	if (job->phase == AIUS_SD_PHASE_CONFIG_READ ||
	    job->phase == AIUS_SD_PHASE_INSTALL_PUBLISH_READ)
		return publication_read_length(job);
	if (job->phase == AIUS_SD_PHASE_DOWNLOAD_READ) {
		uint64_t absolute = job->initial_offset + job->transferred;
		uint64_t remaining = job->end_offset - absolute;
		return remaining < job->capacity ? (size_t)remaining
		                                 : job->capacity;
	}
	return 0u;
}

/* The volume type is payload of exactly one action -- the probe's media check --
 * and is in domain there. Everywhere else it must be absent, so a driver that
 * attaches it to an unrelated event is a protocol break rather than a value
 * silently ignored. */
static FLASHMEM bool filesystem_payload_is_valid(AiusSdEvent event,
	                                 AiusSdActionKind action)
{
	if (action != AIUS_SD_ACTION_MEDIA_PRESENT)
		return event.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN;
	/* Cast rather than a two-sided compare: the enum's underlying type is
	 * implementation-defined, so `>= 0` is either redundant or a warning. */
	return (unsigned)event.filesystem <=
	       (unsigned)AIUS_SD_FILESYSTEM_FAT32;
}

static FLASHMEM bool success_payload_is_valid(AiusSdEvent event,
	                             AiusSdEventKind expected)
{
	switch (expected) {
	case AIUS_SD_EVENT_OK:
		return event.bytes == 0u && event.size == 0u &&
		       !event.value && event.fault == 0u;
	case AIUS_SD_EVENT_BOOL:
		return event.bytes == 0u && event.size == 0u &&
		       event.fault == 0u;
	case AIUS_SD_EVENT_SIZE:
		return event.bytes == 0u && !event.value &&
		       event.fault == 0u;
	case AIUS_SD_EVENT_BYTES:
		return event.size == 0u && !event.value &&
		       event.fault == 0u;
	default:
		return false;
	}
}

static FLASHMEM void clear_outstanding(AiusSdJob *job)
{
	job->outstanding_action = AIUS_SD_ACTION_NONE;
	job->outstanding_action_id = 0u;
}

static FLASHMEM AiusSdEventDisposition consume_event(AiusSdJob *job,
	                                    AiusSdEvent event)
{
	if (job->outstanding_action == AIUS_SD_ACTION_NONE) {
		if (event_is_empty_none(event))
			return AIUS_SD_EVENT_WAITING;
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		return AIUS_SD_EVENT_REJECTED;
	}

	if (event_is_empty_none(event))
		return AIUS_SD_EVENT_WAITING;

	if (event.action_id == 0u ||
	    event.action_id != job->outstanding_action_id) {
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		return AIUS_SD_EVENT_REJECTED;
	}

	AiusSdActionKind action = job->outstanding_action;
	if (event.kind == AIUS_SD_EVENT_FAULT) {
		if (event.bytes != 0u || event.size != 0u || event.value ||
		    event.fault == 0u ||
		    !filesystem_payload_is_valid(event,
		                                 AIUS_SD_ACTION_NONE)) {
			record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
			return AIUS_SD_EVENT_REJECTED;
		}
		clear_outstanding(job);
		handle_fault_event(job, action, event.fault);
		return AIUS_SD_EVENT_CONSUMED;
	}

	AiusSdEventKind expected = expected_event_kind(action);
	if (event.kind != expected ||
	    !success_payload_is_valid(event, expected) ||
	    !filesystem_payload_is_valid(event, action)) {
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		return AIUS_SD_EVENT_REJECTED;
	}
	if (expected == AIUS_SD_EVENT_BYTES &&
	    event.bytes > outstanding_byte_length(job)) {
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		return AIUS_SD_EVENT_REJECTED;
	}

	clear_outstanding(job);
	if (expected == AIUS_SD_EVENT_SIZE &&
	    event.size > (uint64_t)INT64_MAX) {
		record_fault(job, AIUS_SD_FAULT_OVERFLOW, 0u);
		return AIUS_SD_EVENT_CONSUMED;
	}

	switch (expected) {
	case AIUS_SD_EVENT_OK:
		handle_ok_event(job);
		break;
	case AIUS_SD_EVENT_BOOL:
		handle_bool_event(job, event.value, event.filesystem);
		break;
	case AIUS_SD_EVENT_SIZE:
		handle_size_event(job, event.size);
		break;
	case AIUS_SD_EVENT_BYTES:
		handle_bytes_event(job, event.bytes);
		break;
	default:
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);
		break;
	}
	return AIUS_SD_EVENT_CONSUMED;
}

static FLASHMEM bool deadline_expired(const AiusSdJob *job, uint32_t now_ms)
{
	return job->deadline_ms != 0u &&
	       (int32_t)(now_ms - job->deadline_ms) >= 0;
}

static FLASHMEM bool continue_post_commit_publication(const AiusSdJob *job)
{
	return job->installed &&
	       job->last_fault == AIUS_SD_FAULT_CLEANUP &&
	       is_post_commit_publish_phase(job->phase) &&
	       job->state != AIUS_SD_FAILED;
}

FLASHMEM void aius_sd_job_step(AiusSdJob *job,
	              uint32_t now_ms,
	              AiusSdEvent event,
	              AiusSdAction *action)
{
	clear_action(action);
	if (job == NULL)
		return;
	if ((job->state == AIUS_SD_DONE ||
	     job->state == AIUS_SD_FAILED) &&
	    job->terminal_emitted)
		return;

	if (job->cancel_requested && !job->installed)
		record_fault(job, AIUS_SD_FAULT_CANCELLED, 0u);

	if (action == NULL && !job_is_quiescent(job))
		record_fault(job, AIUS_SD_FAULT_PROTOCOL, 0u);

	AiusSdEventDisposition disposition = consume_event(job, event);
	bool completed_by_event =
		disposition == AIUS_SD_EVENT_CONSUMED &&
		job->state == AIUS_SD_DONE;

	if (job->installed)
		job->cancel_requested = false;

	if (job->abort_requested && job->state == AIUS_SD_DONE)
		finish_failure(job);

	if (job->abort_requested &&
	    job->state != AIUS_SD_DONE &&
	    job->state != AIUS_SD_FAILED &&
	    !continue_post_commit_publication(job))
		route_abort(job);

	bool active = job->state != AIUS_SD_IDLE &&
	              job->state != AIUS_SD_DONE &&
	              job->state != AIUS_SD_FAILED;
	if (!job->abort_requested && active && !job->installed &&
	    !completed_by_event && deadline_expired(job, now_ms)) {
		record_fault(job, AIUS_SD_FAULT_TIMEOUT, 0u);
		route_abort(job);
	}

	if (action == NULL ||
	    job->outstanding_action != AIUS_SD_ACTION_NONE)
		return;
	emit_for_phase(job, action);
	if (action->kind == AIUS_SD_ACTION_NONE &&
	    job->outstanding_action == AIUS_SD_ACTION_NONE &&
	    job->phase == AIUS_SD_PHASE_TERMINAL &&
	    !job->terminal_emitted)
		emit_for_phase(job, action);
}
