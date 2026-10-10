#include "aius_sd.h"

/* aius_sd.c is no longer coupled to the feature-flag config (Phase 10a): the
 * publish byte limit is a request field. This suite keeps exercising the
 * feature-flag config's own limit, so it owns the include the module dropped. */
#include "aius_config.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LIMIT 256u
#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

static const char *scenario;

static void fail_at(int line, const char *message)
{
	fprintf(stderr, "sd oracle failure [%s] line %d: %s\n", scenario, line, message);
	exit(1);
}

#define CHECK(condition, message) \
	do { \
		if (!(condition)) \
			fail_at(__LINE__, (message)); \
	} while (0)

static AiusSdEvent none_event(void)
{
	AiusSdEvent event = {0};
	return event;
}

static AiusSdEvent ok_event(uint64_t id)
{
	AiusSdEvent event = {0};
	event.kind = AIUS_SD_EVENT_OK;
	event.action_id = id;
	return event;
}

static AiusSdEvent bool_event(uint64_t id, bool value)
{
	AiusSdEvent event = {0};
	event.kind = AIUS_SD_EVENT_BOOL;
	event.action_id = id;
	event.value = value;
	return event;
}

/* A media-present answer that also carries the volume type, which is the only
 * event the driver may attach one to. */
static AiusSdEvent media_event(uint64_t id, bool value,
	                       AiusSdFilesystem filesystem)
{
	AiusSdEvent event = bool_event(id, value);
	event.filesystem = filesystem;
	return event;
}

static AiusSdEvent size_event(uint64_t id, uint64_t size)
{
	AiusSdEvent event = {0};
	event.kind = AIUS_SD_EVENT_SIZE;
	event.action_id = id;
	event.size = size;
	return event;
}

static AiusSdEvent bytes_event(uint64_t id, size_t bytes)
{
	AiusSdEvent event = {0};
	event.kind = AIUS_SD_EVENT_BYTES;
	event.action_id = id;
	event.bytes = bytes;
	return event;
}

static AiusSdEvent fault_event(uint64_t id, uint32_t fault)
{
	AiusSdEvent event = {0};
	event.kind = AIUS_SD_EVENT_FAULT;
	event.action_id = id;
	event.fault = fault;
	return event;
}

static bool correlated(AiusSdActionKind kind)
{
	return kind != AIUS_SD_ACTION_NONE && kind != AIUS_SD_ACTION_NEED_INPUT &&
	       kind != AIUS_SD_ACTION_COMPLETE && kind != AIUS_SD_ACTION_FAIL;
}

static void validate_action(const AiusSdAction *action)
{
	if (correlated(action->kind))
		CHECK(action->action_id != 0u, "correlated action id is zero");
	else
		CHECK(action->action_id == 0u, "uncorrelated action id is nonzero");

	switch (action->kind) {
	case AIUS_SD_ACTION_NONE:
	case AIUS_SD_ACTION_BEGIN:
	case AIUS_SD_ACTION_MEDIA_PRESENT:
	case AIUS_SD_ACTION_CLOSE:
	case AIUS_SD_ACTION_TOTAL_SIZE:
	case AIUS_SD_ACTION_USED_SIZE:
	case AIUS_SD_ACTION_COMPLETE:
		CHECK(action->path == NULL, "unexpected path");
		CHECK(action->path_to == NULL, "unexpected destination path");
		CHECK(action->buffer == NULL, "unexpected buffer");
		CHECK(action->length == 0u, "unexpected length");
		CHECK(action->offset == 0u, "unexpected offset");
		CHECK(action->fault == 0u, "unexpected fault");
		break;
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_OPEN_READ:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
	case AIUS_SD_ACTION_EXISTS:
	case AIUS_SD_ACTION_REMOVE:
		CHECK(action->path != NULL, "missing path");
		CHECK(action->path_to == NULL, "unexpected destination path");
		CHECK(action->buffer == NULL, "unexpected buffer");
		CHECK(action->length == 0u, "unexpected length");
		CHECK(action->offset == 0u, "unexpected offset");
		CHECK(action->fault == 0u, "unexpected fault");
		break;
	case AIUS_SD_ACTION_TRUNCATE:
		CHECK(action->path == NULL && action->path_to == NULL,
		      "truncate carried path");
		CHECK(action->buffer == NULL && action->length == 0u,
		      "truncate carried buffer");
		CHECK(action->offset == 0u && action->fault == 0u,
		      "truncate carried payload");
		break;
	case AIUS_SD_ACTION_SEEK:
		CHECK(action->path == NULL && action->path_to == NULL,
		      "seek carried path");
		CHECK(action->buffer == NULL && action->length == 0u,
		      "seek carried buffer");
		CHECK(action->fault == 0u, "seek carried fault");
		break;
	case AIUS_SD_ACTION_NEED_INPUT:
		CHECK(action->path == NULL && action->path_to == NULL,
		      "need-input carried path");
		CHECK(action->buffer == NULL, "need-input exposed buffer");
		CHECK(action->length != 0u, "need-input length is zero");
		CHECK(action->fault == 0u, "need-input carried fault");
		break;
	case AIUS_SD_ACTION_READ:
	case AIUS_SD_ACTION_WRITE:
	case AIUS_SD_ACTION_OUTPUT:
	case AIUS_SD_ACTION_PUBLISH_CONFIG:
		CHECK(action->path == NULL && action->path_to == NULL,
		      "buffer action carried path");
		CHECK(action->buffer != NULL, "buffer action has null buffer");
		CHECK(action->length != 0u, "buffer action length is zero");
		CHECK(action->fault == 0u, "buffer action carried fault");
		if (action->kind == AIUS_SD_ACTION_PUBLISH_CONFIG)
			CHECK(action->offset == 0u, "publish carried offset");
		break;
	case AIUS_SD_ACTION_FILE_SIZE:
		CHECK(action->path == NULL && action->path_to == NULL,
		      "file-size carried path");
		CHECK(action->buffer == NULL && action->length == 0u,
		      "file-size carried buffer");
		CHECK(action->offset == 0u && action->fault == 0u,
		      "file-size carried payload");
		break;
	case AIUS_SD_ACTION_RENAME:
		CHECK(action->path != NULL && action->path_to != NULL,
		      "rename paths missing");
		CHECK(action->buffer == NULL && action->length == 0u,
		      "rename carried buffer");
		CHECK(action->offset == 0u && action->fault == 0u,
		      "rename carried payload");
		break;
	case AIUS_SD_ACTION_FAIL:
		CHECK(action->path == NULL && action->path_to == NULL,
		      "fail carried path");
		CHECK(action->buffer == NULL && action->length == 0u,
		      "fail carried buffer");
		CHECK(action->offset == 0u, "fail carried offset");
		CHECK(action->fault != 0u, "fail fault is zero");
		break;
	default:
		fail_at(__LINE__, "unknown action kind");
	}
}

static AiusSdAction step_action(
	AiusSdJob *job,
	uint32_t now_ms,
	AiusSdEvent event,
	AiusSdActionKind expected_kind)
{
	AiusSdAction action;
	memset(&action, 0xa5, sizeof(action));
	aius_sd_job_step(job, now_ms, event, &action);
	if (action.kind != expected_kind) {
		fprintf(stderr,
		        "sd oracle sequence [%s]: expected action %d, actual %d; "
		        "state=%d phase=%d result=%d fault=%d driver=%" PRIu32 "\n",
		        scenario,
		        (int)expected_kind,
		        (int)action.kind,
		        (int)job->state,
		        (int)job->phase,
		        (int)job->result.kind,
		        (int)job->result.fault,
		        job->result.driver_fault);
		exit(1);
	}
	validate_action(&action);
	return action;
}

static AiusSdAction step_none(AiusSdJob *job, AiusSdActionKind expected_kind)
{
	return step_action(job, 0u, none_event(), expected_kind);
}

static AiusSdAction acknowledge_ok(
	AiusSdJob *job,
	const AiusSdAction *previous,
	AiusSdActionKind expected_kind)
{
	return step_action(job, 0u, ok_event(previous->action_id), expected_kind);
}

static AiusSdJobRequest request_for(AiusSdJobKind kind)
{
	AiusSdJobRequest request = {0};
	request.kind = kind;
	request.install_mode = AIUS_SD_INSTALL_REPLACE;
	switch (kind) {
	case AIUS_SD_JOB_CONFIG_LOAD:
		request.target_path = "/config.json";
		request.content_type = "application/json";
		request.publish = true;
		request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
		break;
	case AIUS_SD_JOB_DOWNLOAD:
		request.target_path = "/audio/file.bin";
		request.content_type = "application/octet-stream";
		break;
	case AIUS_SD_JOB_DELETE:
	case AIUS_SD_JOB_REPLAY_REFILL:
	case AIUS_SD_JOB_CACHE_FILL:
		request.target_path = "/audio/file.bin";
		break;
	default:
		break;
	}
	if (kind == AIUS_SD_JOB_REPLAY_REFILL)
		request.total = 1u;
	return request;
}

static void start_valid(
	AiusSdJob *job,
	uint8_t *buffer,
	size_t capacity,
	const AiusSdJobRequest *request)
{
	aius_sd_job_init(job, buffer, capacity);
	aius_sd_job_start(job, request, 0u, 0u);
}

static void expect_result(
	const AiusSdJob *job,
	AiusSdResultKind kind,
	AiusSdFault fault,
	uint32_t driver_fault)
{
	const AiusSdResult *result = aius_sd_job_result(job);
	CHECK(result != NULL, "result unexpectedly null");
	CHECK(result->kind == kind, "result kind mismatch");
	CHECK(result->fault == fault, "result fault mismatch");
	CHECK(result->driver_fault == driver_fault, "driver fault mismatch");
}

static void expect_terminal(
	AiusSdJob *job,
	AiusSdActionKind terminal,
	AiusSdResultKind result_kind,
	AiusSdFault fault,
	uint32_t driver_fault)
{
	AiusSdAction action = step_none(job, terminal);
	if (terminal == AIUS_SD_ACTION_FAIL)
		CHECK(action.fault == (uint32_t)fault, "terminal action fault mismatch");
	expect_result(job, result_kind, fault, driver_fault);
	step_none(job, AIUS_SD_ACTION_NONE);
	step_none(job, AIUS_SD_ACTION_NONE);
}

static AiusSdAction begin_job(AiusSdJob *job)
{
	AiusSdAction action = step_none(job, AIUS_SD_ACTION_BEGIN);
	CHECK(job->phase == AIUS_SD_PHASE_BEGIN, "BEGIN phase mismatch");
	return action;
}

static void test_pointer_and_start_contract(void)
{
	uint8_t buffer[512];
	AiusSdJob job;
	AiusSdAction action;

	scenario = "pointer behavior";
	aius_sd_job_init(NULL, buffer, sizeof(buffer));
	aius_sd_job_start(NULL, NULL, 0u, 0u);
	aius_sd_job_cancel(NULL);
	CHECK(!aius_sd_job_supply(NULL, buffer, 1u), "supply(NULL) succeeded");
	CHECK(aius_sd_job_result(NULL) == NULL, "result(NULL) was non-null");
	memset(&action, 0xa5, sizeof(action));
	aius_sd_job_step(NULL, 0u, none_event(), &action);
	CHECK(action.kind == AIUS_SD_ACTION_NONE, "step(NULL) did not clear action");
	validate_action(&action);

	scenario = "null request";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	aius_sd_job_start(&job, NULL, 0u, 0u);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_INVALID_REQUEST,
	                0u);

	scenario = "null action";
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_PROBE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	aius_sd_job_step(&job, 0u, none_event(), NULL);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_PROTOCOL,
	                0u);
}

static void test_probe_usage_delete(void)
{
	uint8_t buffer[8];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "probe absent";
	request = request_for(AIUS_SD_JOB_PROBE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	CHECK(job.phase == AIUS_SD_PHASE_PROBE_MEDIA, "probe phase mismatch");
	action = step_action(&job,
	                     0u,
	                     media_event(action.action_id, false,
	                                 AIUS_SD_FILESYSTEM_FAT32),
	                     AIUS_SD_ACTION_COMPLETE);
	CHECK(action.kind == AIUS_SD_ACTION_COMPLETE, "probe did not complete");
	expect_result(&job, AIUS_SD_RESULT_PROBE, AIUS_SD_FAULT_NONE, 0u);
	CHECK(!job.result.media_present, "absent media became present");
	/* A driver that answers "no card" but still names a type is reporting a
	 * volume nobody can read; the result keeps UNKNOWN. */
	CHECK(job.result.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN,
	      "absent media reported a filesystem");
	step_none(&job, AIUS_SD_ACTION_NONE);

	scenario = "probe present";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	step_action(&job,
	            0u,
	            bool_event(action.action_id, true),
	            AIUS_SD_ACTION_COMPLETE);
	CHECK(job.result.media_present, "present media became absent");
	CHECK(job.result.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN,
	      "unnamed volume type became known");

	scenario = "probe reports the volume type";
	{
		static const AiusSdFilesystem kFilesystems[] = {
			AIUS_SD_FILESYSTEM_UNKNOWN,
			AIUS_SD_FILESYSTEM_FAT12,
			AIUS_SD_FILESYSTEM_FAT16,
			AIUS_SD_FILESYSTEM_FAT32,
		};
		size_t index;

		for (index = 0u; index < ARRAY_COUNT(kFilesystems); index++) {
			start_valid(&job, buffer, sizeof(buffer), &request);
			action = begin_job(&job);
			action = acknowledge_ok(&job, &action,
			                        AIUS_SD_ACTION_MEDIA_PRESENT);
			step_action(&job,
			            0u,
			            media_event(action.action_id, true,
			                        kFilesystems[index]),
			            AIUS_SD_ACTION_COMPLETE);
			expect_result(&job, AIUS_SD_RESULT_PROBE,
			              AIUS_SD_FAULT_NONE, 0u);
			CHECK(job.result.filesystem == kFilesystems[index],
			      "probe filesystem mismatch");
		}
	}

	/* A malformed event is refused where it arrives: the action stays
	 * outstanding and the fault surfaces once the driver answers it
	 * properly, which is how every other payload rejection here behaves. */
	scenario = "probe rejects an out-of-domain volume type";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	step_action(&job,
	            0u,
	            media_event(action.action_id, true,
	                        (AiusSdFilesystem)(AIUS_SD_FILESYSTEM_FAT32 + 1)),
	            AIUS_SD_ACTION_NONE);
	step_action(&job,
	            0u,
	            bool_event(action.action_id, true),
	            AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PROTOCOL, 0u);

	scenario = "only the media check may carry a volume type";
	{
		/* An EXISTS answer is the other BOOL event, and a DELETE is the
		 * shortest job that asks for one. A volume type on it is a
		 * driver contract break, not a value to ignore. */
		AiusSdJobRequest delete_request =
			request_for(AIUS_SD_JOB_DELETE);

		start_valid(&job, buffer, sizeof(buffer), &delete_request);
		action = begin_job(&job);
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
		step_action(&job,
		            0u,
		            media_event(action.action_id, true,
		                        AIUS_SD_FILESYSTEM_FAT32),
		            AIUS_SD_ACTION_NONE);
		step_action(&job,
		            0u,
		            bool_event(action.action_id, true),
		            AIUS_SD_ACTION_FAIL);
		expect_result(&job, AIUS_SD_RESULT_FAULT,
		              AIUS_SD_FAULT_PROTOCOL, 0u);

		/* Same for a non-BOOL answer: BEGIN's OK carries nothing. */
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = begin_job(&job);
		{
			AiusSdEvent event = ok_event(action.action_id);

			event.filesystem = AIUS_SD_FILESYSTEM_FAT32;
			step_action(&job, 0u, event, AIUS_SD_ACTION_NONE);
		}
		step_action(&job,
		            0u,
		            ok_event(action.action_id),
		            AIUS_SD_ACTION_FAIL);
		expect_result(&job, AIUS_SD_RESULT_FAULT,
		              AIUS_SD_FAULT_PROTOCOL, 0u);
	}

	scenario = "usage";
	request = request_for(AIUS_SD_JOB_USAGE_REFRESH);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_TOTAL_SIZE);
	CHECK(job.phase == AIUS_SD_PHASE_USAGE_TOTAL, "total-size phase mismatch");
	action = step_action(
		&job, 0u, size_event(action.action_id, 1000u), AIUS_SD_ACTION_USED_SIZE);
	CHECK(job.phase == AIUS_SD_PHASE_USAGE_USED, "used-size phase mismatch");
	step_action(&job,
	            0u,
	            size_event(action.action_id, 333u),
	            AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_USAGE, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.total_bytes == 1000u, "total size mismatch");
	CHECK(job.result.used_bytes == 333u, "used size mismatch");

	scenario = "usage used exceeds total";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_TOTAL_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 4u), AIUS_SD_ACTION_USED_SIZE);
	step_action(&job,
	            0u,
	            size_event(action.action_id, 5u),
	            AIUS_SD_ACTION_FAIL);
	expect_result(&job,
	              AIUS_SD_RESULT_FAULT,
	              AIUS_SD_FAULT_PROTOCOL,
	              0u);

	scenario = "delete optional missing defaults to success";
	request = request_for(AIUS_SD_JOB_DELETE);
	CHECK(!request.require_existing,
	      "DELETE request default is not optional");
	start_valid(&job, buffer, sizeof(buffer), &request);
	CHECK(!job.require_existing,
	      "optional DELETE requirement copy mismatch");
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(job.phase == AIUS_SD_PHASE_PATH_EXISTS,
	      "delete existence phase mismatch");
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_DELETE,
	      "delete existence purpose mismatch");
	CHECK(strcmp(action.path, request.target_path) == 0,
	      "delete existence path mismatch");
	const uint64_t after_exists_id = job.next_action_id;
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, false),
	                     AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_COMPLETE, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.next_action_id == after_exists_id,
	      "optional missing DELETE emitted REMOVE");

	scenario = "delete required missing fails without mutation";
	request.require_existing = true;
	start_valid(&job, buffer, sizeof(buffer), &request);
	CHECK(job.require_existing,
	      "required DELETE requirement copy mismatch");
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_DELETE,
	      "required DELETE existence purpose mismatch");
	CHECK(strcmp(action.path, request.target_path) == 0,
	      "required DELETE existence path mismatch");
	const uint64_t required_after_exists_id = job.next_action_id;
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, false),
	                     AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_NOT_FOUND,
	      "required missing DELETE fault mismatch");
	expect_result(
		&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_NOT_FOUND, 0u);
	CHECK(job.next_action_id == required_after_exists_id,
	      "required missing DELETE emitted REMOVE");
	CHECK(job.outstanding_action == AIUS_SD_ACTION_NONE,
	      "required missing DELETE retained authority");

	scenario = "delete required present removes and completes";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(job.phase == AIUS_SD_PHASE_DELETE_REMOVE, "delete phase mismatch");
	CHECK(strcmp(action.path, request.target_path) == 0,
	      "delete remove path mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_COMPLETE, AIUS_SD_FAULT_NONE, 0u);
}

static void test_config_load(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_CONFIG_LOAD);

	scenario = "config partial read and publish ownership";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	CHECK(strcmp(action.path, "/config.json") == 0, "config open path mismatch");
	CHECK(job.phase == AIUS_SD_PHASE_CONFIG_OPEN, "config open phase mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 5u), AIUS_SD_ACTION_READ);
	CHECK(action.buffer == buffer && action.length == 5u && action.offset == 0u,
	      "first config read slice mismatch");
	memcpy(action.buffer, "abc", 3u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 3u), AIUS_SD_ACTION_READ);
	CHECK(action.buffer == buffer + 3 && action.length == 2u &&
		      action.offset == 3u,
	      "second config read slice mismatch");
	memcpy(action.buffer, "de", 2u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 2u), AIUS_SD_ACTION_CLOSE);
	CHECK(job.phase == AIUS_SD_PHASE_CONFIG_CLOSE, "config close phase mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	CHECK(job.phase == AIUS_SD_PHASE_CONFIG_PUBLISH,
	      "config publish phase mismatch");
	CHECK(action.buffer == buffer && action.length == 5u,
	      "config publish buffer mismatch");
	CHECK(memcmp(action.buffer, "abcde", 5u) == 0,
	      "config publish bytes changed");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.transferred_bytes == 5u, "config byte result mismatch");

	scenario = "config zero read closes";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 1u), AIUS_SD_ACTION_READ);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_SHORT_IO, "zero config read fault mismatch");

	const uint64_t invalid_sizes[] = {0u, CONFIG_LIMIT + 1u, (uint64_t)INT64_MAX + 1u};
	for (size_t index = 0; index < ARRAY_COUNT(invalid_sizes); ++index) {
		scenario = "config invalid declared size";
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = begin_job(&job);
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
		action = step_action(&job,
		                     0u,
		                     size_event(action.action_id, invalid_sizes[index]),
		                     AIUS_SD_ACTION_CLOSE);
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
		CHECK(action.fault ==
			      (invalid_sizes[index] > (uint64_t)INT64_MAX
				       ? AIUS_SD_FAULT_OVERFLOW
				       : AIUS_SD_FAULT_CAPACITY),
		      "invalid config size fault mismatch");
	}
}

/* ------------------------------------------------------------------ */
/* the publish-only buffer                                             */
/* ------------------------------------------------------------------ */

/* A job may publish more bytes than one transfer slice by accumulating into a
 * caller-owned buffer registered with aius_sd_job_set_publish_buffer(). The
 * point of the design is that the buffer grows and the per-epoch SD primitive
 * does not, so these cases assert the read slice as hard as the payload.
 *
 * PUBLISH_LIMIT mirrors the application's AIUS_DSP_MODEL_MAX_JSON_BYTES and
 * TRANSFER_LIMIT its SD transfer slice; the engine knows neither constant, so
 * the suite pins the numbers the caller actually uses. Buffers are file-scope
 * to keep a 12 KB payload out of the test's stack frame. */
#define TRANSFER_LIMIT 512u
#define PUBLISH_LIMIT 12288u

static uint8_t publish_transfer[TRANSFER_LIMIT];
static uint8_t publish_area[PUBLISH_LIMIT];
static uint8_t publish_expected[PUBLISH_LIMIT];

static bool all_zero(const uint8_t *bytes, size_t length)
{
	for (size_t index = 0u; index < length; ++index)
		if (bytes[index] != 0u)
			return false;
	return true;
}

/* Initializes a job over the 512-byte transfer slice and registers (or, with a
 * null buffer, clears) the publication buffer, asserting that init itself
 * leaves no registration behind. */
static void arm_publish_job(AiusSdJob *job, uint8_t *buffer, size_t capacity)
{
	aius_sd_job_init(job, publish_transfer, sizeof(publish_transfer));
	CHECK(job->publish_buffer == NULL, "init left a publish buffer");
	CHECK(job->publish_buffer_capacity == 0u, "init left a publish capacity");
	aius_sd_job_set_publish_buffer(job, buffer, capacity);
	CHECK(job->capacity == TRANSFER_LIMIT,
	      "publish registration moved the transfer slice");
}

/* Drives an armed job's CONFIG_LOAD to the action following its size event. */
static AiusSdAction start_publish_config_load(
	AiusSdJob *job,
	size_t publish_capacity,
	uint64_t size,
	AiusSdActionKind after_size)
{
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	AiusSdAction action;

	request.publish_capacity = publish_capacity;
	aius_sd_job_start(job, &request, 0u, 0u);
	action = begin_job(job);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_FILE_SIZE);
	return step_action(
		job, 0u, size_event(action.action_id, size), after_size);
}

static void test_publish_buffer(void)
{
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;
	unsigned reads;
	size_t offset;

	for (size_t index = 0u; index < sizeof(publish_expected); ++index)
		publish_expected[index] = (uint8_t)((index * 31u + 7u) & 0xffu);

	scenario = "full-capacity publication in one-slice primitives";
	memset(publish_area, 0, sizeof(publish_area));
	memset(publish_transfer, 0, sizeof(publish_transfer));
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	CHECK(job.publish_buffer == publish_area,
	      "publish buffer was not registered");
	CHECK(job.publish_buffer_capacity == PUBLISH_LIMIT,
	      "publish capacity was not registered");
	action = start_publish_config_load(
		&job, PUBLISH_LIMIT, PUBLISH_LIMIT, AIUS_SD_ACTION_READ);
	reads = 0u;
	offset = 0u;
	while (offset < PUBLISH_LIMIT) {
		CHECK(action.kind == AIUS_SD_ACTION_READ,
		      "publication left the read phase early");
		CHECK(action.length == TRANSFER_LIMIT,
		      "publication read exceeded one transfer slice");
		CHECK(action.buffer == publish_area + offset,
		      "publication read missed the publish buffer");
		CHECK(action.offset == (uint64_t)offset,
		      "publication read offset mismatch");
		memcpy(action.buffer, publish_expected + offset, TRANSFER_LIMIT);
		offset += TRANSFER_LIMIT;
		reads++;
		action = step_action(&job,
		                     0u,
		                     bytes_event(action.action_id, TRANSFER_LIMIT),
		                     offset == PUBLISH_LIMIT
			                     ? AIUS_SD_ACTION_CLOSE
			                     : AIUS_SD_ACTION_READ);
	}
	CHECK(reads == PUBLISH_LIMIT / TRANSFER_LIMIT,
	      "publication read count is not one per slice");
	CHECK(reads == 24u, "a 12 KB publication is not 24 bounded reads");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	CHECK(job.phase == AIUS_SD_PHASE_CONFIG_PUBLISH,
	      "publish phase mismatch");
	CHECK(action.buffer == publish_area,
	      "publish did not carry the publish buffer");
	CHECK(action.length == PUBLISH_LIMIT,
	      "publish length is not the whole file");
	CHECK(memcmp(action.buffer, publish_expected, PUBLISH_LIMIT) == 0,
	      "published bytes differ from the file");
	CHECK(all_zero(publish_transfer, sizeof(publish_transfer)),
	      "publication passed through the transfer buffer");
	/* Single-shot: the publish is followed by the terminal action, never by
	 * a second publish. */
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.transferred_bytes == PUBLISH_LIMIT,
	      "publication transferred count mismatch");

	scenario = "short read re-slices without growing the primitive";
	memset(publish_area, 0, sizeof(publish_area));
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	action = start_publish_config_load(
		&job, PUBLISH_LIMIT, 1000u, AIUS_SD_ACTION_READ);
	CHECK(action.length == TRANSFER_LIMIT && action.offset == 0u,
	      "first slice of a 1000-byte publication mismatch");
	memcpy(action.buffer, publish_expected, 200u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 200u), AIUS_SD_ACTION_READ);
	/* A short read moves the accumulation offset, not the slice size. */
	CHECK(action.length == TRANSFER_LIMIT,
	      "short read shrank the next slice");
	CHECK(action.offset == 200u, "short read did not advance the offset");
	CHECK(action.buffer == publish_area + 200u,
	      "short read landed off the publish buffer");
	memcpy(action.buffer, publish_expected + 200u, TRANSFER_LIMIT);
	action = step_action(&job,
	                     0u,
	                     bytes_event(action.action_id, TRANSFER_LIMIT),
	                     AIUS_SD_ACTION_READ);
	/* The tail is the remainder, which is below one slice. */
	CHECK(action.length == 1000u - 712u, "tail slice is not the remainder");
	CHECK(action.offset == 712u, "tail slice offset mismatch");
	CHECK(action.buffer == publish_area + 712u, "tail slice buffer mismatch");
	memcpy(action.buffer, publish_expected + 712u, 1000u - 712u);
	action = step_action(&job,
	                     0u,
	                     bytes_event(action.action_id, 1000u - 712u),
	                     AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	CHECK(action.buffer == publish_area && action.length == 1000u,
	      "short-read publication mismatch");
	CHECK(memcmp(action.buffer, publish_expected, 1000u) == 0,
	      "short-read published bytes differ from the file");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	CHECK(job.result.transferred_bytes == 1000u,
	      "short-read transferred count mismatch");

	scenario = "read event above one slice is a protocol fault";
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	action = start_publish_config_load(
		&job, PUBLISH_LIMIT, PUBLISH_LIMIT, AIUS_SD_ACTION_READ);
	CHECK(action.length == TRANSFER_LIMIT, "slice under test mismatch");
	{
		/* The whole file would once have been a legal answer here; with the
		 * slice bound it is not, and the event is refused rather than
		 * overrunning the emitted window. Authority stays with the read
		 * until a well-formed event releases it. */
		const uint64_t read_id = action.action_id;
		step_action(&job,
		            0u,
		            bytes_event(read_id, TRANSFER_LIMIT + 1u),
		            AIUS_SD_ACTION_NONE);
		CHECK(job.config_bytes == 0u,
		      "refused read event still accumulated");
		action = step_action(&job,
		                     0u,
		                     bytes_event(read_id, TRANSFER_LIMIT),
		                     AIUS_SD_ACTION_CLOSE);
	}
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "oversized read event was accepted");

	scenario = "null registration falls back to the transfer buffer";
	memset(publish_area, 0, sizeof(publish_area));
	arm_publish_job(&job, NULL, 0u);
	CHECK(job.publish_buffer == NULL,
	      "null registration retained a buffer");
	CHECK(job.publish_buffer_capacity == 0u,
	      "null registration retained a capacity");
	action = start_publish_config_load(
		&job, AIUS_CONFIG_MAX_JSON_BYTES, 5u, AIUS_SD_ACTION_READ);
	CHECK(action.buffer == publish_transfer && action.length == 5u &&
		      action.offset == 0u,
	      "fallback read did not use the transfer buffer");
	memcpy(action.buffer, "abcde", 5u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 5u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	CHECK(action.buffer == publish_transfer && action.length == 5u,
	      "fallback publish did not use the transfer buffer");
	CHECK(memcmp(action.buffer, "abcde", 5u) == 0,
	      "fallback published bytes changed");
	CHECK(all_zero(publish_area, sizeof(publish_area)),
	      "fallback wrote into the publish buffer");
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);

	scenario = "zero capacity clears the registration";
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	aius_sd_job_set_publish_buffer(&job, publish_area, 0u);
	CHECK(job.publish_buffer == NULL && job.publish_buffer_capacity == 0u,
	      "zero capacity kept the registration");
	/* Cleared, the transfer slice bounds the publication again. */
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	request.publish_capacity = PUBLISH_LIMIT;
	aius_sd_job_start(&job, &request, 0u, 0u);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_CAPACITY,
	                0u);
	aius_sd_job_set_publish_buffer(NULL, publish_area, PUBLISH_LIMIT);

	scenario = "publish capacity above the registered buffer is rejected";
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	request.publish_capacity = PUBLISH_LIMIT + 1u;
	aius_sd_job_start(&job, &request, 0u, 0u);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_CAPACITY,
	                0u);

	scenario = "file above the publish buffer is rejected at the size event";
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	action = start_publish_config_load(
		&job, PUBLISH_LIMIT, PUBLISH_LIMIT + 1u, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CAPACITY,
	      "oversized file was accepted");

	scenario = "the declared publish capacity still bounds below the buffer";
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	action = start_publish_config_load(
		&job, 1024u, 1025u, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CAPACITY,
	      "file above the declared publish capacity was accepted");

	scenario = "registration is construction-time and survives a restart";
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	action = start_publish_config_load(
		&job, PUBLISH_LIMIT, 600u, AIUS_SD_ACTION_READ);
	aius_sd_job_set_publish_buffer(
		&job, publish_transfer, sizeof(publish_transfer));
	CHECK(job.publish_buffer == publish_area,
	      "in-flight registration swapped the buffer");
	CHECK(job.publish_buffer_capacity == PUBLISH_LIMIT,
	      "in-flight registration swapped the capacity");
	action = step_action(
		&job, 0u, fault_event(action.action_id, 5u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(job.publish_buffer == publish_area,
	      "a terminal job lost the registration");
	action = start_publish_config_load(
		&job, PUBLISH_LIMIT, 600u, AIUS_SD_ACTION_READ);
	CHECK(action.buffer == publish_area && action.length == TRANSFER_LIMIT,
	      "restart lost the publish buffer");
}

static void open_sized_seek(
	AiusSdJob *job,
	uint64_t file_size,
	AiusSdAction *action)
{
	*action = begin_job(job);
	*action = acknowledge_ok(job, action, AIUS_SD_ACTION_OPEN_READ);
	*action = acknowledge_ok(job, action, AIUS_SD_ACTION_FILE_SIZE);
	*action = step_action(
		job, 0u, size_event(action->action_id, file_size), AIUS_SD_ACTION_SEEK);
}

static void test_download_and_refill(void)
{
	uint8_t buffer[4];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_DOWNLOAD);

	scenario = "download partial read output ownership";
	request.initial_offset = 2u;
	request.total = 5u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 10u, &action);
	CHECK(action.offset == 2u, "download seek offset mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	CHECK(action.buffer == buffer && action.length == 4u && action.offset == 2u,
	      "download first read mismatch");
	memcpy(action.buffer, "abc", 3u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 3u), AIUS_SD_ACTION_OUTPUT);
	CHECK(action.buffer == buffer && action.length == 3u && action.offset == 2u,
	      "download first output mismatch");
	CHECK(memcmp(action.buffer, "abc", 3u) == 0,
	      "download output bytes changed");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	CHECK(action.length == 2u && action.offset == 5u,
	      "download second read mismatch");
	memcpy(action.buffer, "de", 2u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 2u), AIUS_SD_ACTION_OUTPUT);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.transferred_bytes == 5u, "download result bytes mismatch");

	scenario = "download through eof";
	request.initial_offset = 5u;
	request.total = 0u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 5u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	CHECK(job.result.transferred_bytes == 0u, "EOF download was nonempty");

	scenario = "download range";
	request.initial_offset = 11u;
	request.total = 0u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 10u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_RANGE, "download range fault mismatch");

	scenario = "replay refill capped";
	request = request_for(AIUS_SD_JOB_REPLAY_REFILL);
	request.initial_offset = 7u;
	request.total = 9u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 10u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	CHECK(action.length == 3u && action.offset == 7u,
	      "refill cap mismatch");
	memcpy(action.buffer, "xyz", 3u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 3u), AIUS_SD_ACTION_OUTPUT);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	CHECK(job.result.transferred_bytes == 3u, "refill result mismatch");

	scenario = "replay refill at eof";
	request.initial_offset = 10u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 10u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);

	scenario = "download zero read closes";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 1u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_SHORT_IO, "download zero read mismatch");
}

#define CF_CHUNK 512u
#define CF_FILE (2u * CF_CHUNK + 76u)

static unsigned cf_writes;

static AiusSdAction cf_step(AiusSdJob *job, AiusSdEvent event,
	                    AiusSdActionKind expected)
{
	AiusSdAction action = step_action(job, 0u, event, expected);
	switch (action.kind) {
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
	case AIUS_SD_ACTION_TRUNCATE:
	case AIUS_SD_ACTION_WRITE:
	case AIUS_SD_ACTION_RENAME:
	case AIUS_SD_ACTION_REMOVE:
		cf_writes++;
		break;
	default:
		break;
	}
	return action;
}

/* Drives a CACHE_FILL up to the OUTPUT of chunk `chunks`, checking the chunk
 * bytes, offset and length at the sink each time; returns the OUTPUT action
 * of the last chunk driven (or the SEEK action for chunks == 0). */
static AiusSdAction cf_drive(AiusSdJob *job, uint8_t *buffer, unsigned chunks)
{
	AiusSdAction action = cf_step(job, none_event(), AIUS_SD_ACTION_BEGIN);
	action = cf_step(job, ok_event(action.action_id), AIUS_SD_ACTION_OPEN_READ);
	action = cf_step(job, ok_event(action.action_id), AIUS_SD_ACTION_FILE_SIZE);
	action = cf_step(job, size_event(action.action_id, CF_FILE),
	                 AIUS_SD_ACTION_SEEK);
	for (unsigned index = 0u; index < chunks; index++) {
		size_t length = index < 2u ? CF_CHUNK : CF_FILE - 2u * CF_CHUNK;

		action = cf_step(job, ok_event(action.action_id), AIUS_SD_ACTION_READ);
		CHECK(action.buffer == buffer && action.length == length &&
		      action.offset == (uint64_t)index * CF_CHUNK,
		      "cache fill read geometry mismatch");
		memset(buffer, (int)(0x30u + index), length);
		action = cf_step(job, bytes_event(action.action_id, length),
		                 AIUS_SD_ACTION_OUTPUT);
		CHECK(action.buffer == buffer && action.length == length &&
		      action.offset == (uint64_t)index * CF_CHUNK &&
		      buffer[0] == 0x30u + index,
		      "cache fill sink chunk mismatch");
	}
	return action;
}

static void test_cache_fill(void)
{
	uint8_t buffer[CF_CHUNK];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_CACHE_FILL);

	scenario = "cache fill request shape";
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	expect_terminal(&job, AIUS_SD_ACTION_FAIL, AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_INVALID_REQUEST, 0u);
	request = request_for(AIUS_SD_JOB_CACHE_FILL);
	request.content_type = "text/plain";
	start_valid(&job, buffer, sizeof(buffer), &request);
	expect_terminal(&job, AIUS_SD_ACTION_FAIL, AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_INVALID_REQUEST, 0u);

	scenario = "cache fill reads a file chunk by chunk";
	request = request_for(AIUS_SD_JOB_CACHE_FILL);
	cf_writes = 0u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = cf_drive(&job, buffer, 3u);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.transferred_bytes == CF_FILE, "cache fill byte count");
	CHECK(cf_writes == 0u, "cache fill issued a card write");

	scenario = "cache fill sink failure fails the job";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = cf_drive(&job, buffer, 1u);
	action = cf_step(&job, fault_event(action.action_id, 77u),
	                 AIUS_SD_ACTION_CLOSE);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(job.result.kind == AIUS_SD_RESULT_FAULT && job.result.driver_fault == 77u,
	      "cache fill sink failure result");
	CHECK(!job.file_open, "cache fill sink failure left the file open");

	/* boundary 0: before the first read; 1: after chunk 1; 3: after the last */
	static const unsigned cancel_after[] = {0u, 1u, 3u};
	for (size_t row = 0u; row < ARRAY_COUNT(cancel_after); row++) {
		scenario = "cache fill cancel at a primitive boundary";
		cf_writes = 0u;
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = cf_drive(&job, buffer, cancel_after[row]);
		CHECK(job.file_open, "cache fill file not open before cancel");
		aius_sd_job_cancel(&job);
		action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
		action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == (uint32_t)AIUS_SD_FAULT_CANCELLED,
		      "cache fill cancel fault mismatch");
		CHECK(!job.file_open, "cache fill cancel left the file open");
		CHECK(cf_writes == 0u, "cache fill cancel issued a card write");
		cf_step(&job, none_event(), AIUS_SD_ACTION_NONE);
		cf_step(&job, none_event(), AIUS_SD_ACTION_NONE);
		CHECK(job.state == AIUS_SD_FAILED, "cache fill cancel not terminal");
		expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CANCELLED, 0u);

		/* the job object restarts cleanly after the cancel */
		start_valid(&job, buffer, sizeof(buffer), &request);
		cf_step(&job, none_event(), AIUS_SD_ACTION_BEGIN);
	}

	/* The other boundaries a cancel can land on: with OPEN_READ, FILE_SIZE
	 * or a READ outstanding. The READ row is the state production cancels
	 * from: advance() runs the OUTPUT inline, so between epochs a fill sits
	 * with its next READ issued. Each closes, writes nothing and never hands
	 * the sink another chunk. */
	for (unsigned row = 0u; row < 3u; row++) {
		scenario = row == 0u ? "cache fill cancel with OPEN_READ pending"
		         : row == 1u ? "cache fill cancel with FILE_SIZE pending"
		                     : "cache fill cancel with a READ pending";
		cf_writes = 0u;
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = cf_step(&job, none_event(), AIUS_SD_ACTION_BEGIN);
		action = cf_step(&job, ok_event(action.action_id),
		                 AIUS_SD_ACTION_OPEN_READ);
		if (row == 0u) {
			aius_sd_job_cancel(&job);
			action = cf_step(&job, ok_event(action.action_id),
			                 AIUS_SD_ACTION_CLOSE);
		} else if (row == 1u) {
			action = cf_step(&job, ok_event(action.action_id),
			                 AIUS_SD_ACTION_FILE_SIZE);
			aius_sd_job_cancel(&job);
			action = cf_step(&job, size_event(action.action_id, CF_FILE),
			                 AIUS_SD_ACTION_CLOSE);
		} else {
			action = cf_step(&job, ok_event(action.action_id),
			                 AIUS_SD_ACTION_FILE_SIZE);
			action = cf_step(&job, size_event(action.action_id, CF_FILE),
			                 AIUS_SD_ACTION_SEEK);
			action = cf_step(&job, ok_event(action.action_id),
			                 AIUS_SD_ACTION_READ);
			memset(buffer, 0x41, CF_CHUNK);
			action = cf_step(&job, bytes_event(action.action_id, CF_CHUNK),
			                 AIUS_SD_ACTION_OUTPUT);
			action = cf_step(&job, ok_event(action.action_id),
			                 AIUS_SD_ACTION_READ);
			CHECK(job.file_open, "cache fill file not open before cancel");
			aius_sd_job_cancel(&job);
			/* The pending READ's bytes arrive: CLOSE, not an OUTPUT. */
			action = cf_step(&job, bytes_event(action.action_id, CF_CHUNK),
			                 AIUS_SD_ACTION_CLOSE);
		}
		action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == (uint32_t)AIUS_SD_FAULT_CANCELLED,
		      "cache fill cancel fault mismatch");
		CHECK(!job.file_open, "cache fill cancel left the file open");
		CHECK(cf_writes == 0u, "cache fill cancel issued a card write");
		cf_step(&job, none_event(), AIUS_SD_ACTION_NONE);
		CHECK(job.state == AIUS_SD_FAILED, "cache fill cancel not terminal");
		expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CANCELLED, 0u);
		start_valid(&job, buffer, sizeof(buffer), &request);
		cf_step(&job, none_event(), AIUS_SD_ACTION_BEGIN);
	}

	/* Phase 4g (owner, clean-room finding 1): a cancelled fill resumes at
	 * its next chunk. A chunk-aligned initial_offset seeks there and reads
	 * on to the end, offsets absolute; an unaligned one is refused. */
	scenario = "cache fill resume refuses an unaligned offset";
	static const uint64_t unaligned[] = {1u, 511u, 513u, 1000u};
	for (size_t row = 0u; row < ARRAY_COUNT(unaligned); row++) {
		request = request_for(AIUS_SD_JOB_CACHE_FILL);
		request.initial_offset = unaligned[row];
		start_valid(&job, buffer, sizeof(buffer), &request);
		expect_terminal(&job, AIUS_SD_ACTION_FAIL, AIUS_SD_RESULT_FAULT,
		                AIUS_SD_FAULT_INVALID_REQUEST, 0u);
	}

	scenario = "cache fill resume reads from an aligned offset";
	request = request_for(AIUS_SD_JOB_CACHE_FILL);
	request.initial_offset = CF_CHUNK;
	cf_writes = 0u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = cf_step(&job, none_event(), AIUS_SD_ACTION_BEGIN);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_OPEN_READ);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_FILE_SIZE);
	action = cf_step(&job, size_event(action.action_id, CF_FILE),
	                 AIUS_SD_ACTION_SEEK);
	CHECK(action.offset == CF_CHUNK, "cache fill resume seek offset");
	for (unsigned index = 1u; index < 3u; index++) {
		size_t length = index < 2u ? CF_CHUNK : CF_FILE - 2u * CF_CHUNK;

		action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_READ);
		CHECK(action.length == length &&
		      action.offset == (uint64_t)index * CF_CHUNK,
		      "cache fill resume read geometry");
		memset(buffer, (int)(0x30u + index), length);
		action = cf_step(&job, bytes_event(action.action_id, length),
		                 AIUS_SD_ACTION_OUTPUT);
		CHECK(action.length == length &&
		      action.offset == (uint64_t)index * CF_CHUNK,
		      "cache fill resume sink offset is absolute");
	}
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.transferred_bytes == CF_FILE - CF_CHUNK,
	      "cache fill resume byte count");
	CHECK(job.end_offset == CF_FILE, "cache fill resume end is the file size");
	CHECK(cf_writes == 0u, "cache fill resume issued a card write");

	scenario = "cache fill resume past a shrunk file is a range fault";
	request.initial_offset = 4u * CF_CHUNK;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = cf_step(&job, none_event(), AIUS_SD_ACTION_BEGIN);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_OPEN_READ);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_FILE_SIZE);
	action = cf_step(&job, size_event(action.action_id, CF_FILE),
	                 AIUS_SD_ACTION_CLOSE);
	action = cf_step(&job, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == (uint32_t)AIUS_SD_FAULT_RANGE,
	      "cache fill resume past the end fault");
	CHECK(!job.file_open, "cache fill resume range fault left the file open");
}

static AiusSdJobRequest upload_request(AiusSdInstallMode mode)
{
	AiusSdJobRequest request = {0};
	request.kind = AIUS_SD_JOB_UPLOAD;
	request.install_mode = mode;
	request.target_path = "/final/dir/file";
	if (mode != AIUS_SD_INSTALL_APPEND_PART) {
		request.temp_path = "/temp/dir/file.part";
		request.backup_path = "/backup/dir/file.old";
	}
	return request;
}

static AiusSdAction accept_parent_paths(
	AiusSdJob *job,
	AiusSdAction action,
	const char *const *paths,
	size_t count,
	AiusSdActionKind next)
{
	for (size_t index = 0; index < count; ++index) {
		CHECK(action.kind == AIUS_SD_ACTION_EXISTS,
		      "missing parent existence observation");
		CHECK(job->phase == AIUS_SD_PHASE_PATH_EXISTS,
		      "parent existence phase mismatch");
		CHECK(job->existence_purpose == AIUS_SD_EXISTS_MKDIR,
		      "parent existence purpose mismatch");
		CHECK(strcmp(action.path, paths[index]) == 0,
		      "parent existence path/order mismatch");
		action = step_action(job,
		                     0u,
		                     bool_event(action.action_id, false),
		                     AIUS_SD_ACTION_MKDIR);
		CHECK(job->phase == AIUS_SD_PHASE_MKDIR, "mkdir phase mismatch");
		CHECK(strcmp(action.path, paths[index]) == 0, "mkdir path/order mismatch");
		action = acknowledge_ok(
			job,
			&action,
			index + 1u == count ? next : AIUS_SD_ACTION_EXISTS);
	}
	return action;
}

static AiusSdAction begin_upload(
	AiusSdJob *job,
	const AiusSdJobRequest *request,
	uint8_t *buffer,
	size_t capacity,
	AiusSdActionKind next)
{
	static const char *const replace_dirs[] = {
		"/final", "/final/dir", "/temp", "/temp/dir", "/backup", "/backup/dir"};
	static const char *const append_dirs[] = {"/final", "/final/dir"};
	AiusSdAction action;
	start_valid(job, buffer, capacity, request);
	action = begin_job(job);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	if (request->install_mode == AIUS_SD_INSTALL_APPEND_PART)
		return accept_parent_paths(
			job, action, append_dirs, ARRAY_COUNT(append_dirs), next);
	return accept_parent_paths(
		job, action, replace_dirs, ARRAY_COUNT(replace_dirs), next);
}

static AiusSdAction acknowledge_write_begin(
	AiusSdJob *job,
	const AiusSdAction *open,
	AiusSdActionKind after_truncate)
{
	CHECK(open->kind == AIUS_SD_ACTION_OPEN_WRITE_BEGIN,
	      "expected write-begin open");
	CHECK(job->phase == AIUS_SD_PHASE_UPLOAD_OPEN,
	      "write-begin phase mismatch");
	AiusSdAction action =
		acknowledge_ok(job, open, AIUS_SD_ACTION_TRUNCATE);
	CHECK(job->file_open, "write-begin did not establish file authority");
	CHECK(job->phase == AIUS_SD_PHASE_UPLOAD_TRUNCATE,
	      "truncate phase mismatch");
	return acknowledge_ok(job, &action, after_truncate);
}

static AiusSdAction supply_and_write(
	AiusSdJob *job,
	AiusSdAction action,
	const uint8_t *data,
	size_t length,
	size_t first_ack,
	AiusSdActionKind after_write)
{
	CHECK(action.kind == AIUS_SD_ACTION_NEED_INPUT, "expected need-input");
	CHECK(action.length >= length, "need-input window too small");
	CHECK(aius_sd_job_supply(job, data, length), "valid supply rejected");
	action = step_none(job, AIUS_SD_ACTION_WRITE);
	CHECK(action.length == length, "write length mismatch");
	CHECK(memcmp(action.buffer, data, length) == 0, "write buffer mismatch");
	if (first_ack < length) {
		uint64_t offset = action.offset;
		action = step_action(
			job, 0u, bytes_event(action.action_id, first_ack), AIUS_SD_ACTION_WRITE);
		CHECK(action.length == length - first_ack, "write suffix length mismatch");
		CHECK(action.offset == offset + first_ack, "write suffix offset mismatch");
		CHECK(memcmp(action.buffer, data + first_ack, length - first_ack) == 0,
		      "write suffix pointer mismatch");
		action = step_action(job,
		                     0u,
		                     bytes_event(action.action_id, length - first_ack),
		                     after_write);
	} else {
		action = step_action(
			job, 0u, bytes_event(action.action_id, length), after_write);
	}
	return action;
}

static AiusSdAction install_no_target(AiusSdJob *job, AiusSdAction action)
{
	CHECK(action.kind == AIUS_SD_ACTION_EXISTS, "install existence missing");
	CHECK(job->phase == AIUS_SD_PHASE_PATH_EXISTS,
	      "install-target existence phase mismatch");
	CHECK(job->existence_purpose == AIUS_SD_EXISTS_INSTALL_TARGET,
	      "install-target existence purpose mismatch");
	CHECK(strcmp(action.path, job->target_path) == 0,
	      "install-target existence path mismatch");
	action = step_action(
		job, 0u, bool_event(action.action_id, false), AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, job->temp_path) == 0, "install source mismatch");
	CHECK(strcmp(action.path_to, job->target_path) == 0,
	      "install destination mismatch");
	return action;
}

static AiusSdAction install_existing_target(
	AiusSdJob *job,
	AiusSdAction action,
	bool backup_exists)
{
	CHECK(action.kind == AIUS_SD_ACTION_EXISTS,
	      "install-target existence missing");
	CHECK(job->existence_purpose == AIUS_SD_EXISTS_INSTALL_TARGET,
	      "install-target purpose mismatch");
	CHECK(strcmp(action.path, job->target_path) == 0,
	      "install-target path mismatch");
	action = step_action(job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_EXISTS);
	CHECK(job->existence_purpose == AIUS_SD_EXISTS_REMOVE_OLD_BACKUP,
	      "old-backup purpose mismatch");
	CHECK(strcmp(action.path, job->backup_path) == 0,
	      "old-backup existence path mismatch");
	action = step_action(
		job,
		0u,
		bool_event(action.action_id, backup_exists),
		backup_exists ? AIUS_SD_ACTION_REMOVE : AIUS_SD_ACTION_RENAME);
	if (backup_exists) {
		CHECK(strcmp(action.path, job->backup_path) == 0,
		      "old-backup removal path mismatch");
		action = acknowledge_ok(job, &action, AIUS_SD_ACTION_RENAME);
	}
	CHECK(strcmp(action.path, job->target_path) == 0 &&
		      strcmp(action.path_to, job->backup_path) == 0,
	      "backup rename mismatch");
	return action;
}

static AiusSdAction finish_committed_backup(
	AiusSdJob *job,
	AiusSdAction action,
	bool backup_exists,
	AiusSdActionKind next)
{
	CHECK(action.kind == AIUS_SD_ACTION_EXISTS,
	      "committed-backup existence missing");
	CHECK(job->existence_purpose == AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
	      "committed-backup purpose mismatch");
	CHECK(strcmp(action.path, job->backup_path) == 0,
	      "committed-backup existence path mismatch");
	action = step_action(
		job,
		0u,
		bool_event(action.action_id, backup_exists),
		backup_exists ? AIUS_SD_ACTION_REMOVE : next);
	if (backup_exists) {
		CHECK(strcmp(action.path, job->backup_path) == 0,
		      "committed-backup removal path mismatch");
		action = acknowledge_ok(job, &action, next);
	}
	return action;
}

static AiusSdAction remove_disposable_temp(
	AiusSdJob *job,
	AiusSdAction action,
	bool temp_exists,
	AiusSdActionKind next)
{
	CHECK(action.kind == AIUS_SD_ACTION_EXISTS,
	      "cleanup-temp existence missing");
	CHECK(job->existence_purpose == AIUS_SD_EXISTS_CLEANUP_TEMP,
	      "cleanup-temp purpose mismatch");
	CHECK(strcmp(action.path, job->temp_path) == 0,
	      "cleanup-temp existence path mismatch");
	action = step_action(
		job,
		0u,
		bool_event(action.action_id, temp_exists),
		temp_exists ? AIUS_SD_ACTION_REMOVE : next);
	if (temp_exists) {
		CHECK(strcmp(action.path, job->temp_path) == 0,
		      "cleanup-temp removal path mismatch");
		action = acknowledge_ok(job, &action, next);
	}
	return action;
}

static void test_upload_modes(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t abc[] = {'a', 'b', 'c'};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "upload replace partial write";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = sizeof(abc);
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	CHECK(strcmp(action.path, request.temp_path) == 0, "replace open path mismatch");
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(action.offset == 0u && action.length == sizeof(abc),
	      "replace input request mismatch");
	action = supply_and_write(
		&job, action, abc, sizeof(abc), 1u, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_no_target(&job, action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.transferred_bytes == sizeof(abc),
	      "replace transferred mismatch");

	scenario = "upload empty replace";
	request.total = 0u;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(&job, &action, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_no_target(&job, action);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	CHECK(job.result.transferred_bytes == 0u, "empty replace result mismatch");

	scenario = "upload append-part";
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.initial_offset = 4u;
	request.total = sizeof(abc);
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	CHECK(strcmp(action.path, request.target_path) == 0, "part-size open mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 4u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	CHECK(strcmp(action.path, request.target_path) == 0, "append open path mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(action.offset == 4u, "append input offset mismatch");
	action = supply_and_write(
		&job, action, abc, sizeof(abc), sizeof(abc), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);

	scenario = "upload append-part fresh (.part absent, offset 0)";
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.initial_offset = 0u;
	request.total = sizeof(abc);
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	/* The .part does not exist, so the size read-open faults -- which at a
	 * zero claimed offset means "nothing to resume", not a failure: the job
	 * proceeds to the append open that creates the file and records no
	 * fault. Found on hardware, where every FRESH PUT /sd/replays/<id>
	 * answered 500 sd_failure at this phase; only resumes of an existing
	 * .part ever worked. */
	action = step_action(&job, 0u, fault_event(action.action_id, 77u),
	                     AIUS_SD_ACTION_OPEN_APPEND);
	CHECK(strcmp(action.path, request.target_path) == 0,
	      "fresh append open path mismatch");
	CHECK(job.last_fault == AIUS_SD_FAULT_NONE,
	      "fresh append recorded a fault");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(action.offset == 0u, "fresh append input offset mismatch");
	action = supply_and_write(
		&job, action, abc, sizeof(abc), sizeof(abc), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);

	scenario = "upload append-part resume against a missing .part still faults";
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.initial_offset = 4u;
	request.total = sizeof(abc);
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	/* A non-zero claimed offset with no .part cannot be verified, so the
	 * open's failure stays a fault. */
	action = step_action(&job, 0u, fault_event(action.action_id, 78u),
	                     AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_OPEN, 78u);

	scenario = "upload finalize-part";
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.initial_offset = 4u;
	request.total = sizeof(abc);
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	CHECK(strcmp(action.path, request.temp_path) == 0, "finalize size path mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 4u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, abc, sizeof(abc), sizeof(abc), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 7u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_no_target(&job, action);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
}

static void test_install_publication_and_authority(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {'j', 's', 'o', 'n'};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = sizeof(data);
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	request.content_type = "application/json";

	scenario = "replace existing publish";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, request.temp_path) == 0 &&
		      strcmp(action.path_to, request.target_path) == 0,
	      "install rename mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(job.installed, "install commit not recorded");
	action = finish_committed_backup(
		&job, action, true, AIUS_SD_ACTION_OPEN_READ);
	CHECK(strcmp(action.path, request.target_path) == 0,
	      "publish reread path mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(&job,
	                     0u,
	                     size_event(action.action_id, sizeof(data)),
	                     AIUS_SD_ACTION_READ);
	memcpy(action.buffer, data, 2u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 2u), AIUS_SD_ACTION_READ);
	memcpy(action.buffer, data + 2u, 2u);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 2u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	CHECK(action.buffer == buffer && action.length == sizeof(data),
	      "post-commit publish buffer mismatch");
	CHECK(memcmp(action.buffer, data, sizeof(data)) == 0,
	      "post-commit publish data mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);

	scenario = "install failure without backup preserves temp";
	request.publish = false;
	request.publish_capacity = 0u;
	request.content_type = NULL;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_no_target(&job, action);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 41u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_INSTALL, "install fault mismatch");
	CHECK(job.install_failed, "install_failed authority missing");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_INSTALL, 41u);

	scenario = "install failure rollback success";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 42u), AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, request.backup_path) == 0 &&
		      strcmp(action.path_to, request.target_path) == 0,
	      "rollback rename mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_INSTALL, "rollback success fault mismatch");
	CHECK(job.install_failed, "rollback path lost install_failed");

	scenario = "install failure rollback failure";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 43u), AIUS_SD_ACTION_RENAME);
	CHECK(job.install_failed,
	      "temp-to-target failure did not preserve temp authority");
	CHECK(job.backup_moved,
	      "temp-to-target failure lost backup recovery authority");
	CHECK(!job.installed,
	      "failed temp-to-target rename claimed install commit");
	const uint64_t after_rollback_id = job.next_action_id;
	action = step_action(
		&job, 0u, fault_event(action.action_id, 44u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_ROLLBACK, "rollback failure fault mismatch");
	CHECK(job.install_failed,
	      "rollback failure lost disposable-temp recovery authority");
	CHECK(job.backup_moved,
	      "rollback failure lost backup recovery authority");
	CHECK(!job.installed,
	      "rollback failure claimed install commit");
	CHECK(job.next_action_id == after_rollback_id,
	      "rollback failure emitted recovery-file cleanup");
	CHECK(job.outstanding_action == AIUS_SD_ACTION_NONE,
	      "rollback failure retained correlated authority");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_ROLLBACK, 44u);
}

static void test_request_boundaries(void)
{
	uint8_t buffer[512];
	AiusSdJob job;
	AiusSdJobRequest request;

	struct CapacityCase {
		uint8_t *buffer;
		size_t capacity;
		AiusSdJobKind kind;
		AiusSdFault fault;
	};
	const struct CapacityCase capacities[] = {
		{NULL, 1u, AIUS_SD_JOB_PROBE, AIUS_SD_FAULT_CAPACITY},
		{buffer, 0u, AIUS_SD_JOB_PROBE, AIUS_SD_FAULT_CAPACITY},
		{buffer, 513u, AIUS_SD_JOB_PROBE, AIUS_SD_FAULT_CAPACITY},
		{buffer, 255u, AIUS_SD_JOB_CONFIG_LOAD, AIUS_SD_FAULT_CAPACITY},
	};
	for (size_t index = 0; index < ARRAY_COUNT(capacities); ++index) {
		scenario = "capacity boundary";
		request = request_for(capacities[index].kind);
		start_valid(&job,
		            capacities[index].buffer,
		            capacities[index].capacity,
		            &request);
		expect_terminal(&job,
		                AIUS_SD_ACTION_FAIL,
		                AIUS_SD_RESULT_FAULT,
		                capacities[index].fault,
		                0u);
	}

	const char *const invalid_paths[] = {
		"", "relative", "/", "/tail/", "/a//b", "/a/./b", "/a/../b",
		"/a\\b", "/line\nfeed"};
	for (size_t index = 0; index < ARRAY_COUNT(invalid_paths); ++index) {
		scenario = "path syntax boundary";
		request = request_for(AIUS_SD_JOB_DELETE);
		request.target_path = invalid_paths[index];
		start_valid(&job, buffer, sizeof(buffer), &request);
		expect_terminal(&job,
		                AIUS_SD_ACTION_FAIL,
		                AIUS_SD_RESULT_FAULT,
		                AIUS_SD_FAULT_PATH,
		                0u);
	}

	char path127[AIUS_SD_PATH_STORAGE];
	char path128[AIUS_SD_PATH_STORAGE + 1u];
	path127[0] = '/';
	memset(path127 + 1, 'a', sizeof(path127) - 2u);
	path127[sizeof(path127) - 1u] = '\0';
	path128[0] = '/';
	memset(path128 + 1, 'b', sizeof(path128) - 2u);
	path128[sizeof(path128) - 1u] = '\0';

	scenario = "path 127 bytes accepted";
	request = request_for(AIUS_SD_JOB_DELETE);
	request.target_path = path127;
	start_valid(&job, buffer, sizeof(buffer), &request);
	AiusSdAction action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_DELETE,
	      "127-byte delete existence purpose mismatch");
	CHECK(strcmp(action.path, path127) == 0, "127-byte path copy mismatch");

	scenario = "path 128 bytes rejected";
	request.target_path = path128;
	start_valid(&job, buffer, sizeof(buffer), &request);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_PATH,
	                0u);

	scenario = "duplicate upload paths";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.temp_path = request.target_path;
	start_valid(&job, buffer, sizeof(buffer), &request);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_PATH,
	                0u);

	scenario = "request arithmetic overflow";
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.initial_offset = (uint64_t)INT64_MAX;
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_OVERFLOW,
	                0u);

	scenario = "download arithmetic overflow";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.initial_offset = (uint64_t)INT64_MAX;
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_OVERFLOW,
	                0u);

	struct InvalidRequestCase {
		AiusSdJobRequest request;
	};
	struct InvalidRequestCase invalid[14];
	size_t count = 0u;
	invalid[count++].request = (AiusSdJobRequest){
		.kind = (AiusSdJobKind)99, .install_mode = AIUS_SD_INSTALL_REPLACE};
	request = request_for(AIUS_SD_JOB_PROBE);
	request.install_mode = AIUS_SD_INSTALL_APPEND_PART;
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_PROBE);
	request.total = 1u;
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_PROBE);
	request.target_path = "/x";
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	request.publish = false;
	request.publish_capacity = 0u;
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.content_type = NULL;
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_REPLAY_REFILL);
	request.total = 0u;
	invalid[count++].request = request;
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.temp_path = "/temp";
	invalid[count++].request = request;
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.initial_offset = 1u;
	invalid[count++].request = request;
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	request.total = 0u;
	request.content_type = "application/json";
	invalid[count++].request = request;
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	request.content_type = "application/json";
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_DELETE);
	request.content_type = "unexpected";
	invalid[count++].request = request;
	/* publish and publish_capacity are paired, never implicit: neither half
	 * is a valid request on its own. */
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.publish = true;
	request.publish_capacity = 0u;
	request.total = 1u;
	request.content_type = "application/json";
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_PROBE);
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	invalid[count++].request = request;
	for (size_t index = 0; index < count; ++index) {
		scenario = "invalid request field matrix";
		start_valid(&job, buffer, sizeof(buffer), &invalid[index].request);
		expect_terminal(&job,
		                AIUS_SD_ACTION_FAIL,
		                AIUS_SD_RESULT_FAULT,
		                AIUS_SD_FAULT_INVALID_REQUEST,
		                0u);
	}
}

static void test_require_existing_request_matrix(void)
{
	uint8_t buffer[512];
	AiusSdJob job;
	struct RequireExistingCase {
		const char *name;
		AiusSdJobRequest request;
	};
	struct RequireExistingCase cases[8];
	size_t count = 0u;

	cases[count++] = (struct RequireExistingCase){
		"PROBE require_existing rejected",
		request_for(AIUS_SD_JOB_PROBE)};
	cases[count++] = (struct RequireExistingCase){
		"CONFIG_LOAD require_existing rejected",
		request_for(AIUS_SD_JOB_CONFIG_LOAD)};
	cases[count++] = (struct RequireExistingCase){
		"USAGE_REFRESH require_existing rejected",
		request_for(AIUS_SD_JOB_USAGE_REFRESH)};
	cases[count++] = (struct RequireExistingCase){
		"DOWNLOAD require_existing rejected",
		request_for(AIUS_SD_JOB_DOWNLOAD)};
	cases[count++] = (struct RequireExistingCase){
		"REPLAY_REFILL require_existing rejected",
		request_for(AIUS_SD_JOB_REPLAY_REFILL)};
	cases[count++] = (struct RequireExistingCase){
		"UPLOAD REPLACE require_existing rejected",
		upload_request(AIUS_SD_INSTALL_REPLACE)};
	cases[count++] = (struct RequireExistingCase){
		"UPLOAD APPEND_PART require_existing rejected",
		upload_request(AIUS_SD_INSTALL_APPEND_PART)};
	cases[count++] = (struct RequireExistingCase){
		"UPLOAD FINALIZE_PART require_existing rejected",
		upload_request(AIUS_SD_INSTALL_FINALIZE_PART)};
	CHECK(count == ARRAY_COUNT(cases),
	      "require_existing rejection registry incomplete");

	for (size_t index = 0u; index < count; ++index) {
		scenario = cases[index].name;
		cases[index].request.require_existing = true;
		start_valid(
			&job, buffer, sizeof(buffer), &cases[index].request);
		expect_result(&job,
		              AIUS_SD_RESULT_FAULT,
		              AIUS_SD_FAULT_INVALID_REQUEST,
		              0u);
		CHECK(job.next_action_id == 1u,
		      "invalid require_existing consumed an action id");
		CHECK(job.outstanding_action == AIUS_SD_ACTION_NONE &&
			      job.outstanding_action_id == 0u,
		      "invalid require_existing retained authority");
		AiusSdAction action =
			step_none(&job, AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == AIUS_SD_FAULT_INVALID_REQUEST,
		      "invalid require_existing terminal mismatch");
		CHECK(job.next_action_id == 1u,
		      "invalid require_existing emitted BEGIN");
		step_none(&job, AIUS_SD_ACTION_NONE);
	}
}

static void test_protocol_mismatches_and_supply(void)
{
	uint8_t buffer[8];
	const uint8_t data[] = {1u, 2u};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_PROBE);

	scenario = "payload-bearing NONE while outstanding";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	AiusSdEvent malformed = none_event();
	malformed.bytes = 1u;
	step_action(&job, 0u, malformed, AIUS_SD_ACTION_NONE);
	CHECK(job.outstanding_action == AIUS_SD_ACTION_BEGIN,
	      "malformed event cleared authority");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL, "protocol abort fault mismatch");

	scenario = "wrong id retains outstanding";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	CHECK(job.outstanding_action_id == action.action_id,
	      "wrong id cleared outstanding action");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL, "wrong-id fault mismatch");

	scenario = "duplicate prior result retains current outstanding";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	uint64_t completed_id = action.action_id;
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	uint64_t current_id = action.action_id;
	step_action(
		&job, 0u, ok_event(completed_id), AIUS_SD_ACTION_NONE);
	CHECK(job.outstanding_action == AIUS_SD_ACTION_MEDIA_PRESENT &&
		      job.outstanding_action_id == current_id,
	      "duplicate prior result cleared current authority");
	action = step_action(
		&job, 0u, bool_event(current_id, true), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "duplicate prior result fault mismatch");

	scenario = "wrong kind retains outstanding";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL, "wrong-kind fault mismatch");

	scenario = "malformed OK retains outstanding";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	malformed = ok_event(action.action_id);
	malformed.size = 1u;
	step_action(&job, 0u, malformed, AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL, "malformed OK fault mismatch");

	scenario = "event without outstanding";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	step_action(&job,
	            0u,
	            bool_event(action.action_id, false),
	            AIUS_SD_ACTION_COMPLETE);
	step_action(&job, 0u, ok_event(1u), AIUS_SD_ACTION_NONE);
	CHECK(job.result.kind == AIUS_SD_RESULT_PROBE,
	      "post-terminal duplicate mutated result");

	scenario = "invalid supply matrix";
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.total = 2u;
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(!aius_sd_job_supply(&job, data, 0u), "zero supply succeeded");
	action = step_none(&job, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_SHORT_IO, "zero supply fault mismatch");

	scenario = "null positive supply";
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(!aius_sd_job_supply(&job, NULL, 1u), "null supply succeeded");
	action = step_none(&job, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL, "null supply fault mismatch");

	scenario = "oversized supply";
	action = begin_upload(
		&job, &request, buffer, 1u, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(action.length == 1u, "supply window boundary mismatch");
	CHECK(!aius_sd_job_supply(&job, data, 2u), "oversized supply succeeded");
	action = step_none(&job, AIUS_SD_ACTION_CLOSE);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);

	scenario = "supply while action outstanding retains authority";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = 1u;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	CHECK(!aius_sd_job_supply(&job, data, 1u),
	      "out-of-state supply succeeded");
	step_action(&job, 0u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(job.outstanding_action_id == action.action_id,
	      "supply cleared open authority");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "outstanding supply fault mismatch");

	scenario = "supply before TRUNCATE acknowledgement";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_TRUNCATE);
	CHECK(job.file_open, "write-begin did not establish truncate authority");
	CHECK(!aius_sd_job_supply(&job, data, 1u),
	      "pre-truncate supply succeeded");
	step_action(&job, 0u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(job.outstanding_action == AIUS_SD_ACTION_TRUNCATE &&
		      job.outstanding_action_id == action.action_id,
	      "pre-truncate supply cleared truncate authority");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == 0u,
	      "pre-truncate supply counted upload bytes");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "pre-truncate supply fault mismatch");

	scenario = "idle and terminal supply inert";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	CHECK(!aius_sd_job_supply(&job, data, 1u), "idle supply succeeded");
	CHECK(job.state == AIUS_SD_IDLE, "idle supply mutated state");
}

static void test_deadline_cancel_and_restart(void)
{
	uint8_t buffer[8];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_PROBE);

	scenario = "deadline equality after completion event";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	aius_sd_job_start(&job, &request, 100u, 105u);
	action = step_action(&job, 100u, none_event(), AIUS_SD_ACTION_BEGIN);
	action = step_action(
		&job, 101u, ok_event(action.action_id), AIUS_SD_ACTION_MEDIA_PRESENT);
	step_action(&job,
	            105u,
	            bool_event(action.action_id, true),
	            AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_PROBE, AIUS_SD_FAULT_NONE, 0u);

	scenario = "deadline equality outstanding";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	aius_sd_job_start(&job, &request, 100u, 105u);
	action = step_action(&job, 100u, none_event(), AIUS_SD_ACTION_BEGIN);
	step_action(&job, 105u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(job.abort_requested, "deadline did not record abort");
	action = step_action(
		&job, 106u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_TIMEOUT, "timeout fault mismatch");

	scenario = "wrapping deadline";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	aius_sd_job_start(&job, &request, UINT32_MAX - 2u, 2u);
	action = step_action(
		&job, UINT32_MAX - 2u, none_event(), AIUS_SD_ACTION_BEGIN);
	step_action(&job, 1u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(!job.abort_requested, "deadline expired before wrap target");
	step_action(&job, 2u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(job.abort_requested, "deadline failed at wrap equality");
	action = step_action(
		&job, 2u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_TIMEOUT, "wrap timeout fault mismatch");

	scenario = "invalid deadline range";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	aius_sd_job_start(&job, &request, 0u, UINT32_C(0x80000000));
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_INVALID_REQUEST,
	                0u);

	scenario = "repeated cancel unresolved begin";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	aius_sd_job_cancel(&job);
	aius_sd_job_cancel(&job);
	step_none(&job, AIUS_SD_ACTION_NONE);
	CHECK(job.outstanding_action_id == action.action_id,
	      "cancel cleared unresolved begin");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "cancel fault mismatch");

	scenario = "one-shot terminal restart and lifetime ids";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	uint64_t first_id = action.action_id;
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	uint64_t second_id = action.action_id;
	step_action(&job,
	            0u,
	            bool_event(action.action_id, true),
	            AIUS_SD_ACTION_COMPLETE);
	step_none(&job, AIUS_SD_ACTION_NONE);
	aius_sd_job_start(&job, &request, 0u, 0u);
	action = begin_job(&job);
	CHECK(action.action_id > second_id && second_id > first_id,
	      "action ids reused across restart");
	aius_sd_job_start(&job, &request, 0u, 0u);
	CHECK(job.outstanding_action_id == action.action_id,
	      "active restart disturbed outstanding action");
}

static AiusSdFault mapped_fault(AiusSdActionKind action)
{
	switch (action) {
	case AIUS_SD_ACTION_MEDIA_PRESENT:
		return AIUS_SD_FAULT_MEDIA;
	case AIUS_SD_ACTION_OPEN_READ:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
		return AIUS_SD_FAULT_OPEN;
	case AIUS_SD_ACTION_TRUNCATE:
	case AIUS_SD_ACTION_SEEK:
		return AIUS_SD_FAULT_SEEK;
	case AIUS_SD_ACTION_PUBLISH_CONFIG:
		return AIUS_SD_FAULT_PUBLISH;
	default:
		return AIUS_SD_FAULT_IO;
	}
}

static void test_basic_action_faults(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "BEGIN fault";
	request = request_for(AIUS_SD_JOB_PROBE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 20u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == mapped_fault(AIUS_SD_ACTION_BEGIN), "BEGIN fault map");
	CHECK(job.next_action_id == 2u,
	      "BEGIN fault emitted a cleanup correlation");
	CHECK(job.outstanding_action == AIUS_SD_ACTION_NONE &&
		      job.existence_purpose == AIUS_SD_EXISTS_NONE,
	      "BEGIN fault retained cleanup authority");
	CHECK(!job.file_open && !job.backup_moved && !job.installed,
	      "BEGIN fault acquired storage authority");
	step_none(&job, AIUS_SD_ACTION_NONE);

	scenario = "MEDIA_PRESENT fault";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 21u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == mapped_fault(AIUS_SD_ACTION_MEDIA_PRESENT),
	      "media fault map");

	scenario = "ordinary REMOVE fault";
	request = request_for(AIUS_SD_JOB_DELETE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 22u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_IO, "remove fault map");

	scenario = "OPEN fault";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 23u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_OPEN, "open fault map");

	scenario = "SEEK fault closes";
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 1u, &action);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 24u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_SEEK, "seek fault map");

	scenario = "READ fault closes";
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 1u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 25u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_IO, "read fault map");

	scenario = "OUTPUT fault closes";
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 1u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	action.buffer[0] = 1u;
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_OUTPUT);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 26u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_IO, "output fault map");

	scenario = "SIZE overflow closes";
	request.total = 0u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(&job,
	                     0u,
	                     size_event(action.action_id, (uint64_t)INT64_MAX + 1u),
	                     AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_OVERFLOW, "driver size overflow mismatch");
}

static void test_post_commit_fault_precedence(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {1u};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = 1u;
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	request.content_type = "application/json";

	scenario = "post-commit cleanup plus publish fault";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 51u), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 1u), AIUS_SD_ACTION_READ);
	action.buffer[0] = 1u;
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 52u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CLEANUP,
	      "cleanup did not win combined post-commit faults");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CLEANUP, 51u);

	scenario = "cancel after install commit ignored";
	request.publish = false;
	request.publish_capacity = 0u;
	request.content_type = NULL;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	aius_sd_job_cancel(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = finish_committed_backup(
		&job, action, true, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
}

static void test_action_id_reserve(void)
{
	uint8_t buffer[8];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_PROBE);
	const uint64_t reserve_start = UINT64_MAX - UINT64_C(262);

	scenario = "action id above reserve rejected authority-free";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	job.next_action_id = reserve_start + 1u;
	aius_sd_job_start(&job, &request, 0u, 0u);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_OVERFLOW,
	                0u);
	CHECK(job.outstanding_action == AIUS_SD_ACTION_NONE,
	      "above-reserve start emitted correlated work");
	CHECK(job.next_action_id == reserve_start + 1u,
	      "above-reserve rejection consumed an id");

	scenario = "action id reserve equality accepts BEGIN only";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	job.next_action_id = reserve_start;
	aius_sd_job_start(&job, &request, 0u, 0u);
	action = begin_job(&job);
	CHECK(action.action_id == reserve_start,
	      "reserve-equality BEGIN id mismatch");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_OVERFLOW,
	      "reserve-equality ordinary emission did not overflow");
	CHECK(job.outstanding_action == AIUS_SD_ACTION_NONE,
	      "reserve-equality overflow retained authority");
	CHECK(job.next_action_id == reserve_start + 1u,
	      "reserve-equality overflow consumed extra id");

	scenario = "action id just below reserve completes probe";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	job.next_action_id = reserve_start - 1u;
	aius_sd_job_start(&job, &request, 0u, 0u);
	action = begin_job(&job);
	CHECK(action.action_id == reserve_start - 1u,
	      "just-below BEGIN id mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	CHECK(action.action_id == reserve_start,
	      "just-below second id mismatch");
	step_action(&job,
	            0u,
	            bool_event(action.action_id, true),
	            AIUS_SD_ACTION_COMPLETE);

	scenario = "upload reserve equality overflows before path mutation";
	AiusSdJobRequest upload = upload_request(AIUS_SD_INSTALL_REPLACE);
	upload.total = 0u;
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	job.next_action_id = reserve_start;
	aius_sd_job_start(&job, &upload, 0u, 0u);
	action = begin_job(&job);
	CHECK(action.action_id == reserve_start,
	      "upload reserve-equality BEGIN id mismatch");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_NONE);
	action = step_none(&job, AIUS_SD_ACTION_EXISTS);
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_CLEANUP_TEMP,
	      "reserve-equality overflow attempted normal path work");
	CHECK(strcmp(action.path, job.temp_path) == 0,
	      "reserve-equality cleanup path mismatch");
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, false),
	                     AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_OVERFLOW,
	      "reserve-equality upload fault mismatch");
	CHECK(!job.file_open && !job.backup_moved && !job.installed,
	      "reserve-equality overflow acquired file authority");

	scenario = "upload just below reserve aborts before mutation";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	job.next_action_id = reserve_start - 1u;
	aius_sd_job_start(&job, &upload, 0u, 0u);
	action = begin_job(&job);
	CHECK(action.action_id == reserve_start - 1u,
	      "just-below upload BEGIN id mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(action.action_id == reserve_start,
	      "just-below parent existence id mismatch");
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_MKDIR,
	      "just-below parent existence purpose mismatch");
	CHECK(strcmp(action.path, "/final") == 0,
	      "just-below parent existence path mismatch");
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_NONE);
	action = step_none(&job, AIUS_SD_ACTION_EXISTS);
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_CLEANUP_TEMP,
	      "just-below overflow attempted next path mutation");
	CHECK(strcmp(action.path, job.temp_path) == 0,
	      "just-below cleanup path mismatch");
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, false),
	                     AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_OVERFLOW,
	      "just-below upload fault mismatch");
	CHECK(!job.file_open && !job.backup_moved && !job.installed,
	      "just-below overflow acquired file authority");
}

static AiusSdAction reach_replace_open(
	AiusSdJob *job,
	uint8_t *buffer,
	size_t capacity,
	uint64_t total)
{
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = total;
	return begin_upload(job,
	                    &request,
	                    buffer,
	                    capacity,
	                    AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
}

static AiusSdAction reach_existing_install(
	AiusSdJob *job,
	uint8_t *buffer,
	const uint8_t *data,
	size_t length)
{
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	AiusSdAction action;
	request.total = length;
	action = begin_upload(job,
	                      &request,
	                      buffer,
	                      CONFIG_LIMIT,
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		job, action, data, length, length, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(job, action, true);
	return acknowledge_ok(job, &action, AIUS_SD_ACTION_RENAME);
}

static void finish_protocol_after_close(AiusSdJob *job, AiusSdAction action)
{
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "protocol cleanup terminal mismatch");
}

static void test_malformed_event_table(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t byte = 0x5au;
	AiusSdJob job;
	AiusSdAction action;
	AiusSdEvent malformed;
	AiusSdJobRequest request;

	const AiusSdEvent none_payloads[] = {
		{.kind = AIUS_SD_EVENT_NONE, .action_id = 1u},
		{.kind = AIUS_SD_EVENT_NONE, .bytes = 1u},
		{.kind = AIUS_SD_EVENT_NONE, .size = 1u},
		{.kind = AIUS_SD_EVENT_NONE, .value = true},
		{.kind = AIUS_SD_EVENT_NONE, .fault = 1u},
	};
	for (size_t index = 0; index < ARRAY_COUNT(none_payloads); ++index) {
		scenario = "payload-bearing NONE table";
		request = request_for(AIUS_SD_JOB_PROBE);
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = begin_job(&job);
		step_action(&job, 0u, none_payloads[index], AIUS_SD_ACTION_NONE);
		action = step_action(
			&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
		      "payload-bearing NONE fault mismatch");
	}

	const AiusSdEvent ok_payloads[] = {
		{.kind = AIUS_SD_EVENT_OK, .action_id = 1u, .bytes = 1u},
		{.kind = AIUS_SD_EVENT_OK, .action_id = 1u, .size = 1u},
		{.kind = AIUS_SD_EVENT_OK, .action_id = 1u, .value = true},
		{.kind = AIUS_SD_EVENT_OK, .action_id = 1u, .fault = 1u},
	};
	for (size_t index = 0; index < ARRAY_COUNT(ok_payloads); ++index) {
		scenario = "malformed OK payload table";
		request = request_for(AIUS_SD_JOB_PROBE);
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = begin_job(&job);
		malformed = ok_payloads[index];
		malformed.action_id = action.action_id;
		step_action(&job, 0u, malformed, AIUS_SD_ACTION_NONE);
		action = step_action(
			&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
		      "malformed OK payload fault mismatch");
	}

	struct BoolMalformed {
		const char *name;
		AiusSdEvent event;
	};
	const struct BoolMalformed bool_cases[] = {
		{"BOOL zero id", {.kind = AIUS_SD_EVENT_BOOL, .value = true}},
		{"BOOL wrong id", {.kind = AIUS_SD_EVENT_BOOL, .action_id = 99u, .value = true}},
		{"BOOL bytes payload",
		 {.kind = AIUS_SD_EVENT_BOOL, .action_id = 1u, .bytes = 1u, .value = true}},
		{"BOOL size payload",
		 {.kind = AIUS_SD_EVENT_BOOL, .action_id = 1u, .size = 1u, .value = true}},
		{"BOOL fault payload",
		 {.kind = AIUS_SD_EVENT_BOOL, .action_id = 1u, .value = true, .fault = 1u}},
	};
	for (size_t index = 0; index < ARRAY_COUNT(bool_cases); ++index) {
		scenario = bool_cases[index].name;
		request = request_for(AIUS_SD_JOB_PROBE);
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = begin_job(&job);
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
		malformed = bool_cases[index].event;
		if (malformed.action_id == 1u)
			malformed.action_id = action.action_id;
		step_action(&job, 0u, malformed, AIUS_SD_ACTION_NONE);
		CHECK(job.outstanding_action_id == action.action_id,
		      "malformed BOOL cleared authority");
		action = step_action(&job,
		                     0u,
		                     bool_event(action.action_id, true),
		                     AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
		      "malformed BOOL fault mismatch");
	}

	struct SizeMalformed {
		const char *name;
		AiusSdEvent event;
	};
	const struct SizeMalformed size_cases[] = {
		{"SIZE zero id", {.kind = AIUS_SD_EVENT_SIZE, .size = 4u}},
		{"SIZE wrong id", {.kind = AIUS_SD_EVENT_SIZE, .action_id = 99u, .size = 4u}},
		{"SIZE bytes payload",
		 {.kind = AIUS_SD_EVENT_SIZE, .action_id = 1u, .bytes = 1u, .size = 4u}},
		{"SIZE bool payload",
		 {.kind = AIUS_SD_EVENT_SIZE, .action_id = 1u, .size = 4u, .value = true}},
		{"SIZE fault payload",
		 {.kind = AIUS_SD_EVENT_SIZE, .action_id = 1u, .size = 4u, .fault = 1u}},
	};
	for (size_t index = 0; index < ARRAY_COUNT(size_cases); ++index) {
		scenario = size_cases[index].name;
		request = request_for(AIUS_SD_JOB_USAGE_REFRESH);
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = begin_job(&job);
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_TOTAL_SIZE);
		malformed = size_cases[index].event;
		if (malformed.action_id == 1u)
			malformed.action_id = action.action_id;
		step_action(&job, 0u, malformed, AIUS_SD_ACTION_NONE);
		CHECK(job.outstanding_action_id == action.action_id,
		      "malformed SIZE cleared authority");
		action = step_action(&job,
		                     0u,
		                     size_event(action.action_id, 4u),
		                     AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
		      "malformed SIZE fault mismatch");
	}

	struct BytesMalformed {
		const char *name;
		AiusSdEvent event;
	};
	const struct BytesMalformed bytes_cases[] = {
		{"BYTES zero id", {.kind = AIUS_SD_EVENT_BYTES, .bytes = 1u}},
		{"BYTES wrong id", {.kind = AIUS_SD_EVENT_BYTES, .action_id = 99u, .bytes = 1u}},
		{"BYTES size payload",
		 {.kind = AIUS_SD_EVENT_BYTES, .action_id = 1u, .bytes = 1u, .size = 1u}},
		{"BYTES bool payload",
		 {.kind = AIUS_SD_EVENT_BYTES, .action_id = 1u, .bytes = 1u, .value = true}},
		{"BYTES fault payload",
		 {.kind = AIUS_SD_EVENT_BYTES, .action_id = 1u, .bytes = 1u, .fault = 1u}},
		{"BYTES oversized",
		 {.kind = AIUS_SD_EVENT_BYTES, .action_id = 1u, .bytes = 2u}},
	};
	for (size_t index = 0; index < ARRAY_COUNT(bytes_cases); ++index) {
		scenario = bytes_cases[index].name;
		request = request_for(AIUS_SD_JOB_DOWNLOAD);
		request.total = 1u;
		start_valid(&job, buffer, sizeof(buffer), &request);
		open_sized_seek(&job, 1u, &action);
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
		malformed = bytes_cases[index].event;
		if (malformed.action_id == 1u)
			malformed.action_id = action.action_id;
		step_action(&job, 0u, malformed, AIUS_SD_ACTION_NONE);
		CHECK(job.outstanding_action_id == action.action_id,
		      "malformed BYTES cleared authority");
		action.buffer[0] = byte;
		action = step_action(&job,
		                     0u,
		                     bytes_event(action.action_id, 1u),
		                     AIUS_SD_ACTION_CLOSE);
		finish_protocol_after_close(&job, action);
	}

	struct FaultMalformed {
		const char *name;
		AiusSdEvent event;
	};
	const struct FaultMalformed fault_cases[] = {
		{"FAULT zero id", {.kind = AIUS_SD_EVENT_FAULT, .fault = 7u}},
		{"FAULT wrong id", {.kind = AIUS_SD_EVENT_FAULT, .action_id = 99u, .fault = 7u}},
		{"FAULT zero driver", {.kind = AIUS_SD_EVENT_FAULT, .action_id = 1u}},
		{"FAULT bytes payload",
		 {.kind = AIUS_SD_EVENT_FAULT, .action_id = 1u, .bytes = 1u, .fault = 7u}},
		{"FAULT size payload",
		 {.kind = AIUS_SD_EVENT_FAULT, .action_id = 1u, .size = 1u, .fault = 7u}},
		{"FAULT bool payload",
		 {.kind = AIUS_SD_EVENT_FAULT, .action_id = 1u, .value = true, .fault = 7u}},
	};
	for (size_t index = 0; index < ARRAY_COUNT(fault_cases); ++index) {
		scenario = fault_cases[index].name;
		request = request_for(AIUS_SD_JOB_PROBE);
		start_valid(&job, buffer, sizeof(buffer), &request);
		action = begin_job(&job);
		malformed = fault_cases[index].event;
		if (malformed.action_id == 1u)
			malformed.action_id = action.action_id;
		step_action(&job, 0u, malformed, AIUS_SD_ACTION_NONE);
		CHECK(job.outstanding_action_id == action.action_id,
		      "malformed FAULT cleared authority");
		action = step_action(
			&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
		      "malformed FAULT fault mismatch");
	}
}

static void test_unresolved_action_authority(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {1u, 2u};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdEvent wrong;
	AiusSdJobRequest request;

	scenario = "unresolved OPEN authority";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	wrong = ok_event(action.action_id + 1u);
	step_action(&job, 0u, wrong, AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "unresolved OPEN terminal mismatch");

	scenario = "unresolved WRITE authority";
	action = reach_replace_open(&job, buffer, sizeof(buffer), sizeof(data));
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)), "write supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	step_action(
		&job, 0u, bytes_event(action.action_id + 1u, 1u), AIUS_SD_ACTION_NONE);
	action = step_action(&job,
	                     0u,
	                     bytes_event(action.action_id, 1u),
	                     AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == 1u, "resolved WRITE progress not retained");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "unresolved WRITE terminal mismatch");

	scenario = "unresolved OUTPUT authority";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 1u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	action.buffer[0] = 1u;
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_OUTPUT);
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == 1u, "resolved OUTPUT progress not retained");
	finish_protocol_after_close(&job, action);

	scenario = "unresolved CLOSE authority";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 0u);
	action = acknowledge_write_begin(&job, &action, AIUS_SD_ACTION_CLOSE);
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "unresolved CLOSE terminal mismatch");

	scenario = "unresolved REMOVE authority";
	request = request_for(AIUS_SD_JOB_DELETE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "unresolved REMOVE terminal mismatch");

	scenario = "unresolved backup RENAME authority";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	CHECK(strcmp(action.path, job.temp_path) == 0,
	      "expected install rename");
	/* Restart the authority edge at the preceding backup rename. */
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = sizeof(data);
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, request.backup_path) == 0,
	      "protocol cleanup did not roll backup back");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "rollback cleanup did not retain protocol fault");

	scenario = "unresolved install RENAME success wins";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_EXISTS);
	CHECK(job.installed, "resolved install success did not commit");
	action = finish_committed_backup(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "install commit did not retain prior protocol abort");
}

static void test_complete_action_fault_matrix(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {1u};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "MKDIR fault and disposable cleanup";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, false),
	                     AIUS_SD_ACTION_MKDIR);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 60u), AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 60u);

	scenario = "OPEN_WRITE_BEGIN fault";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 61u), AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_OPEN, 61u);

	scenario = "TRUNCATE fault closes and cleans disposable temp";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_TRUNCATE);
	CHECK(job.file_open, "truncate fixture lost open authority");
	action = step_action(
		&job, 0u, fault_event(action.action_id, 611u), AIUS_SD_ACTION_CLOSE);
	CHECK(job.file_open, "truncate fault cleared authority before close");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(!job.file_open, "truncate cleanup close retained authority");
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_SEEK, 611u);

	scenario = "OPEN_APPEND fault preserves durable part";
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.total = 1u;
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 62u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_OPEN, 62u);

	scenario = "WRITE fault close and cleanup";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)), "write-fault supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 63u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 63u);

	scenario = "FILE_SIZE fault close";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 64u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 64u);

	scenario = "TOTAL_SIZE fault";
	request = request_for(AIUS_SD_JOB_USAGE_REFRESH);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_TOTAL_SIZE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 66u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 66u);

	scenario = "USED_SIZE fault";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_TOTAL_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 8u), AIUS_SD_ACTION_USED_SIZE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 67u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 67u);

	scenario = "ordinary CLOSE fault";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.total = 0u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 0u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 68u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 68u);

	scenario = "old backup removal fault";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = 1u;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 69u), AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 69u);

	scenario = "cleanup close fault precedence and temp authority";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 71u), AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CLEANUP, 71u);

	scenario = "cleanup temp removal fault";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 72u), AIUS_SD_ACTION_EXISTS);
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 73u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CLEANUP, 73u);
}

static void test_partial_abort_and_commit_precedence(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {1u, 2u};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "cancel after partial upload write";
	action = reach_replace_open(&job, buffer, sizeof(buffer), sizeof(data));
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)), "partial cancel supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_WRITE);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == sizeof(data), "partial cancel lost write progress");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "partial upload cancel fault mismatch");

	scenario = "deadline after partial download output";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.total = 2u;
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	aius_sd_job_start(&job, &request, 1u, 5u);
	action = step_action(&job, 1u, none_event(), AIUS_SD_ACTION_BEGIN);
	action = step_action(
		&job, 1u, ok_event(action.action_id), AIUS_SD_ACTION_OPEN_READ);
	action = step_action(
		&job, 1u, ok_event(action.action_id), AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 1u, size_event(action.action_id, 2u), AIUS_SD_ACTION_SEEK);
	action = step_action(
		&job, 1u, ok_event(action.action_id), AIUS_SD_ACTION_READ);
	action.buffer[0] = 1u;
	action = step_action(
		&job, 1u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_OUTPUT);
	action = step_action(
		&job, 1u, ok_event(action.action_id), AIUS_SD_ACTION_READ);
	step_action(&job, 5u, none_event(), AIUS_SD_ACTION_NONE);
	action.buffer[0] = 2u;
	action = step_action(
		&job, 5u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == 1u, "unacknowledged output counted as transferred");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_TIMEOUT,
	      "partial download timeout fault mismatch");

	scenario = "deadline after partial upload write";
	action = reach_replace_open(&job, buffer, sizeof(buffer), sizeof(data));
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)),
	      "partial deadline supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_WRITE);
	job.deadline_ms = 5u;
	step_action(&job, 5u, none_event(), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 5u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_TIMEOUT,
	      "partial upload deadline fault mismatch");

	scenario = "cancel after partial download output";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.total = 2u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, 2u, &action);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	action.buffer[0] = 1u;
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_OUTPUT);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_READ);
	aius_sd_job_cancel(&job);
	action.buffer[0] = 2u;
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == 1u, "cancel counted unacknowledged output");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "partial download cancel fault mismatch");

	scenario = "cancel in-flight install success commits";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_EXISTS);
	CHECK(job.installed, "in-flight install success did not commit");
	action = finish_committed_backup(
		&job, action, true, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);

	scenario = "deadline in-flight install success commits";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	job.deadline_ms = 5u;
	step_action(&job, 5u, none_event(), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 5u, ok_event(action.action_id), AIUS_SD_ACTION_EXISTS);
	CHECK(job.installed, "deadline install success did not commit");
	action = finish_committed_backup(
		&job, action, true, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
}

static AiusSdAction reach_backup_rename(
	AiusSdJob *job,
	uint8_t *buffer,
	const uint8_t *data,
	size_t length)
{
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	AiusSdAction action;
	request.total = length;
	action = begin_upload(job,
	                      &request,
	                      buffer,
	                      CONFIG_LIMIT,
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		job, action, data, length, length, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	return install_existing_target(job, action, true);
}

static AiusSdAction fail_backup_rename_to_temp_cleanup(
	AiusSdJob *job,
	uint8_t *buffer,
	const uint8_t *data,
	size_t length,
	uint32_t driver_fault)
{
	AiusSdAction action =
		reach_backup_rename(job, buffer, data, length);
	CHECK(job->phase == AIUS_SD_PHASE_INSTALL_BACKUP,
	      "backup-rename fault fixture phase mismatch");
	CHECK(strcmp(action.path, job->target_path) == 0 &&
		      strcmp(action.path_to, job->backup_path) == 0,
	      "backup-rename fault fixture paths mismatch");
	action = step_action(
		job,
		0u,
		fault_event(action.action_id, driver_fault),
		AIUS_SD_ACTION_EXISTS);
	CHECK(job->phase == AIUS_SD_PHASE_PATH_EXISTS,
	      "backup-rename fault skipped path-exists cleanup");
	CHECK(job->existence_purpose == AIUS_SD_EXISTS_CLEANUP_TEMP,
	      "backup-rename fault selected wrong cleanup purpose");
	CHECK(strcmp(action.path, job->temp_path) == 0,
	      "backup-rename fault checked wrong cleanup path");
	CHECK(!job->install_failed,
	      "target-to-backup fault claimed install failure");
	CHECK(!job->backup_moved,
	      "failed target-to-backup rename claimed backup authority");
	return action;
}

static void test_backup_rename_fault_temp_cleanup(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {6u};
	AiusSdJob job;
	AiusSdAction action;

	scenario = "backup rename fault absent temp preserves install fault";
	action = fail_backup_rename_to_temp_cleanup(
		&job, buffer, data, sizeof(data), 70u);
	action = step_action(
		&job,
		0u,
		bool_event(action.action_id, false),
		AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_INSTALL,
	      "absent temp replaced backup-rename fault");
	expect_result(
		&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_INSTALL, 70u);

	scenario = "backup rename fault present temp removes then preserves";
	action = fail_backup_rename_to_temp_cleanup(
		&job, buffer, data, sizeof(data), 71u);
	action = step_action(
		&job,
		0u,
		bool_event(action.action_id, true),
		AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, job.temp_path) == 0,
	      "backup-rename cleanup removed wrong path");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_INSTALL,
	      "successful temp cleanup replaced backup-rename fault");
	expect_result(
		&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_INSTALL, 71u);

	scenario = "backup rename temp existence fault overrides cleanup";
	action = fail_backup_rename_to_temp_cleanup(
		&job, buffer, data, sizeof(data), 72u);
	action = step_action(
		&job,
		0u,
		fault_event(action.action_id, 73u),
		AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CLEANUP,
	      "temp existence fault did not override install fault");
	expect_result(
		&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CLEANUP, 73u);

	scenario = "backup rename temp removal fault overrides cleanup";
	action = fail_backup_rename_to_temp_cleanup(
		&job, buffer, data, sizeof(data), 74u);
	action = step_action(
		&job,
		0u,
		bool_event(action.action_id, true),
		AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, job.temp_path) == 0,
	      "failing cleanup removal targeted wrong path");
	action = step_action(
		&job,
		0u,
		fault_event(action.action_id, 75u),
		AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CLEANUP,
	      "temp removal fault did not override install fault");
	expect_result(
		&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CLEANUP, 75u);
}

static void test_abort_install_failed_authority(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {7u};
	AiusSdJob job;
	AiusSdAction action;

	scenario = "cancel after backup move rolls back and removes temp";
	action = reach_backup_rename(&job, buffer, data, sizeof(data));
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, job.backup_path) == 0,
	      "cancel did not request rollback");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "precommit rollback cancel fault mismatch");
	CHECK(!job.install_failed, "cancel-only rollback set install_failed");

	scenario = "cancel plus install failure preserves temp";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 80u), AIUS_SD_ACTION_RENAME);
	CHECK(job.install_failed, "cancelled install fault not recorded");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "install fault replaced earlier cancellation");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CANCELLED, 0u);

	scenario = "timeout plus install failure preserves temp";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	job.deadline_ms = 5u;
	step_action(&job, 5u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(job.abort_requested, "install deadline did not record timeout");
	action = step_action(
		&job, 5u, fault_event(action.action_id, 81u), AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_TIMEOUT,
	      "install fault replaced earlier timeout");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_TIMEOUT, 0u);

	scenario = "protocol plus install failure preserves temp";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 82u), AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "install fault replaced earlier protocol abort");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PROTOCOL, 0u);

	scenario = "cancel plus install and rollback failure";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 83u), AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 84u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_ROLLBACK,
	      "rollback failure did not override cancellation");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_ROLLBACK, 84u);

	scenario = "timeout plus install and rollback failure";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	job.deadline_ms = 5u;
	step_action(&job, 5u, none_event(), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 5u, fault_event(action.action_id, 85u), AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 5u, fault_event(action.action_id, 86u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_ROLLBACK,
	      "rollback failure did not override timeout");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_ROLLBACK, 86u);

	scenario = "protocol plus install and rollback failure";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 87u), AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 88u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_ROLLBACK,
	      "rollback failure did not override protocol");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_ROLLBACK, 88u);
}

static AiusSdAction reach_post_commit_publish_open(
	AiusSdJob *job,
	uint8_t *buffer,
	const uint8_t *data,
	size_t length)
{
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	AiusSdAction action;
	request.total = length;
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	request.content_type = "application/json";
	action = begin_upload(job,
	                      &request,
	                      buffer,
	                      CONFIG_LIMIT,
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		job, action, data, length, length, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(job, action, true);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	return finish_committed_backup(
		job, action, true, AIUS_SD_ACTION_OPEN_READ);
}

static AiusSdAction reach_post_commit_publish(
	AiusSdJob *job,
	uint8_t *buffer,
	const uint8_t *data,
	size_t length)
{
	AiusSdAction action =
		reach_post_commit_publish_open(job, buffer, data, length);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		job, 0u, size_event(action.action_id, length), AIUS_SD_ACTION_READ);
	memcpy(action.buffer, data, length);
	action = step_action(
		job, 0u, bytes_event(action.action_id, length), AIUS_SD_ACTION_CLOSE);
	return acknowledge_ok(job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
}

/* The symmetric post-commit path: an installed file re-read and published from
 * the registered publish buffer, in the same bounded slices. begin_upload()
 * re-inits the job, so the registration is made after it rather than before. */
static void test_post_commit_publish_buffer(void)
{
	static const char *const replace_dirs[] = {"/final",  "/final/dir",
	                                           "/temp",   "/temp/dir",
	                                           "/backup", "/backup/dir"};
	const size_t total = 1024u;
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	AiusSdJob job;
	AiusSdAction action;
	size_t offset;
	unsigned reads;

	scenario = "post-commit publication above one slice";
	memset(publish_area, 0, sizeof(publish_area));
	request.total = total;
	request.publish = true;
	request.publish_capacity = total;
	request.content_type = "application/json";
	arm_publish_job(&job, publish_area, sizeof(publish_area));
	aius_sd_job_start(&job, &request, 0u, 0u);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = accept_parent_paths(&job,
	                             action,
	                             replace_dirs,
	                             ARRAY_COUNT(replace_dirs),
	                             AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	for (offset = 0u; offset < total; offset += TRANSFER_LIMIT) {
		CHECK(action.length == TRANSFER_LIMIT,
		      "upload input window is not one slice");
		action = supply_and_write(&job,
		                          action,
		                          publish_expected + offset,
		                          TRANSFER_LIMIT,
		                          TRANSFER_LIMIT,
		                          offset + TRANSFER_LIMIT == total
			                          ? AIUS_SD_ACTION_CLOSE
			                          : AIUS_SD_ACTION_NEED_INPUT);
	}
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = finish_committed_backup(
		&job, action, true, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, total), AIUS_SD_ACTION_READ);

	reads = 0u;
	for (offset = 0u; offset < total; offset += TRANSFER_LIMIT) {
		CHECK(action.length == TRANSFER_LIMIT,
		      "post-commit read exceeded one transfer slice");
		CHECK(action.buffer == publish_area + offset,
		      "post-commit read missed the publish buffer");
		CHECK(action.offset == (uint64_t)offset,
		      "post-commit read offset mismatch");
		memcpy(action.buffer, publish_expected + offset, TRANSFER_LIMIT);
		reads++;
		action = step_action(&job,
		                     0u,
		                     bytes_event(action.action_id, TRANSFER_LIMIT),
		                     offset + TRANSFER_LIMIT == total
			                     ? AIUS_SD_ACTION_CLOSE
			                     : AIUS_SD_ACTION_READ);
	}
	CHECK(reads == 2u, "post-commit publication read count drifted");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	CHECK(action.buffer == publish_area && action.length == total,
	      "post-commit publish did not carry the whole publication");
	CHECK(memcmp(action.buffer, publish_expected, total) == 0,
	      "post-commit published bytes differ from the file");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	CHECK(job.result.transferred_bytes == total,
	      "post-commit transferred count mismatch");
}

static void test_post_commit_fault_matrix(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {9u};
	AiusSdJob job;
	AiusSdAction action;

	scenario = "postcommit reread OPEN fault";
	action = reach_post_commit_publish_open(&job, buffer, data, sizeof(data));
	action = step_action(
		&job, 0u, fault_event(action.action_id, 90u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PUBLISH, 90u);
	CHECK(job.installed, "publish-open fault lost installed authority");

	scenario = "postcommit reread SIZE fault";
	action = reach_post_commit_publish_open(&job, buffer, data, sizeof(data));
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 91u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PUBLISH, 91u);

	scenario = "postcommit reread wrong SIZE";
	action = reach_post_commit_publish_open(&job, buffer, data, sizeof(data));
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 2u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PUBLISH, 0u);

	scenario = "postcommit reread READ fault";
	action = reach_post_commit_publish_open(&job, buffer, data, sizeof(data));
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 1u), AIUS_SD_ACTION_READ);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 92u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PUBLISH, 92u);

	scenario = "postcommit reread zero READ";
	action = reach_post_commit_publish_open(&job, buffer, data, sizeof(data));
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 1u), AIUS_SD_ACTION_READ);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PUBLISH, 0u);

	scenario = "postcommit reread CLOSE fault";
	action = reach_post_commit_publish_open(&job, buffer, data, sizeof(data));
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 1u), AIUS_SD_ACTION_READ);
	action.buffer[0] = data[0];
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 93u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CLEANUP, 93u);

	scenario = "standalone PUBLISH_CONFIG fault";
	action = reach_post_commit_publish(&job, buffer, data, sizeof(data));
	action = step_action(
		&job, 0u, fault_event(action.action_id, 94u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_PUBLISH, 94u);
	CHECK(job.installed, "publish fault rolled back installed target");
}

static void reject_request(
	AiusSdJob *job,
	uint8_t *buffer,
	size_t capacity,
	const AiusSdJobRequest *request,
	AiusSdFault fault)
{
	start_valid(job, buffer, capacity, request);
	expect_terminal(
		job, AIUS_SD_ACTION_FAIL, AIUS_SD_RESULT_FAULT, fault, 0u);
}

static void test_complete_request_boundary_matrix(void)
{
	uint8_t buffer[512];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	const size_t valid_capacities[] = {1u, 512u};
	for (size_t index = 0; index < ARRAY_COUNT(valid_capacities); ++index) {
		scenario = "valid capacity endpoint";
		request = request_for(AIUS_SD_JOB_PROBE);
		start_valid(&job, buffer, valid_capacities[index], &request);
		action = begin_job(&job);
		aius_sd_job_cancel(&job);
		action = step_action(
			&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
		CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
		      "valid capacity endpoint rejected");
	}

	scenario = "capacity 513 rejected";
	request = request_for(AIUS_SD_JOB_PROBE);
	reject_request(
		&job, buffer, sizeof(buffer) + 1u, &request, AIUS_SD_FAULT_CAPACITY);

	struct ExtraPathCase {
		const char *name;
		AiusSdJobRequest request;
	};
	struct ExtraPathCase paths[20];
	size_t path_count = 0u;
	request = request_for(AIUS_SD_JOB_PROBE);
	request.target_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"probe target", request};
	request = request_for(AIUS_SD_JOB_PROBE);
	request.temp_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"probe temp", request};
	request = request_for(AIUS_SD_JOB_PROBE);
	request.backup_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"probe backup", request};
	request = request_for(AIUS_SD_JOB_USAGE_REFRESH);
	request.target_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"usage target", request};
	request = request_for(AIUS_SD_JOB_USAGE_REFRESH);
	request.temp_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"usage temp", request};
	request = request_for(AIUS_SD_JOB_USAGE_REFRESH);
	request.backup_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"usage backup", request};
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	request.temp_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"config temp", request};
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	request.backup_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"config backup", request};
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.temp_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"download temp", request};
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.backup_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"download backup", request};
	request = request_for(AIUS_SD_JOB_DELETE);
	request.temp_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"delete temp", request};
	request = request_for(AIUS_SD_JOB_DELETE);
	request.backup_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"delete backup", request};
	request = request_for(AIUS_SD_JOB_REPLAY_REFILL);
	request.temp_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"refill temp", request};
	request = request_for(AIUS_SD_JOB_REPLAY_REFILL);
	request.backup_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"refill backup", request};
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.temp_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"append temp", request};
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.backup_path = "/x";
	paths[path_count++] = (struct ExtraPathCase){"append backup", request};
	for (size_t index = 0; index < path_count; ++index) {
		scenario = paths[index].name;
		reject_request(&job,
		               buffer,
		               sizeof(buffer),
		               &paths[index].request,
		               AIUS_SD_FAULT_INVALID_REQUEST);
	}

	scenario = "all path control bytes rejected";
	char control_path[] = "/a?b";
	request = request_for(AIUS_SD_JOB_DELETE);
	request.target_path = control_path;
	for (unsigned value = 1u; value < 32u; ++value) {
		control_path[2] = (char)value;
		reject_request(
			&job, buffer, sizeof(buffer), &request, AIUS_SD_FAULT_PATH);
	}
	control_path[2] = 127;
	reject_request(&job, buffer, sizeof(buffer), &request, AIUS_SD_FAULT_PATH);

	struct DistinctCase {
		const char *name;
		AiusSdJobRequest request;
	};
	struct DistinctCase distinct[6];
	size_t distinct_count = 0u;
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.temp_path = request.target_path;
	distinct[distinct_count++] = (struct DistinctCase){"replace target=temp", request};
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.backup_path = request.target_path;
	distinct[distinct_count++] = (struct DistinctCase){"replace target=backup", request};
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.backup_path = request.temp_path;
	distinct[distinct_count++] = (struct DistinctCase){"replace temp=backup", request};
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.temp_path = request.target_path;
	distinct[distinct_count++] = (struct DistinctCase){"final target=temp", request};
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.backup_path = request.target_path;
	distinct[distinct_count++] = (struct DistinctCase){"final target=backup", request};
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.backup_path = request.temp_path;
	distinct[distinct_count++] = (struct DistinctCase){"final temp=backup", request};
	for (size_t index = 0; index < distinct_count; ++index) {
		scenario = distinct[index].name;
		reject_request(&job,
		               buffer,
		               sizeof(buffer),
		               &distinct[index].request,
		               AIUS_SD_FAULT_PATH);
	}

	struct InvalidFieldCase {
		const char *name;
		AiusSdJobRequest request;
	};
	struct InvalidFieldCase fields[20];
	size_t field_count = 0u;
	request = request_for(AIUS_SD_JOB_USAGE_REFRESH);
	request.initial_offset = 1u;
	fields[field_count++] = (struct InvalidFieldCase){"usage offset", request};
	request = request_for(AIUS_SD_JOB_DELETE);
	request.total = 1u;
	fields[field_count++] = (struct InvalidFieldCase){"delete total", request};
	request = request_for(AIUS_SD_JOB_DELETE);
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	fields[field_count++] = (struct InvalidFieldCase){"delete publish", request};
	request = request_for(AIUS_SD_JOB_REPLAY_REFILL);
	request.content_type = "x";
	fields[field_count++] = (struct InvalidFieldCase){"refill content type", request};
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	request.total = 1u;
	fields[field_count++] = (struct InvalidFieldCase){"config total", request};
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	request.initial_offset = 1u;
	fields[field_count++] = (struct InvalidFieldCase){"config offset", request};
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	fields[field_count++] = (struct InvalidFieldCase){"download publish", request};
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.content_type = "x";
	fields[field_count++] = (struct InvalidFieldCase){"replace stray content", request};
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.content_type = "x";
	fields[field_count++] = (struct InvalidFieldCase){"final stray content", request};
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	request.total = CONFIG_LIMIT + 1u;
	request.content_type = "application/json";
	fields[field_count++] = (struct InvalidFieldCase){"final publish oversize", request};
	for (size_t index = 0; index < field_count; ++index) {
		scenario = fields[index].name;
		reject_request(&job,
		               buffer,
		               sizeof(buffer),
		               &fields[index].request,
		               AIUS_SD_FAULT_INVALID_REQUEST);
	}

	scenario = "valid upload INT64_MAX total";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = (uint64_t)INT64_MAX;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "INT64_MAX upload rejected");

	scenario = "valid checked final INT64_MAX";
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.initial_offset = (uint64_t)INT64_MAX - 1u;
	request.total = 1u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "checked INT64_MAX final rejected");

	scenario = "valid download INT64_MAX offset";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.initial_offset = (uint64_t)INT64_MAX;
	start_valid(&job, buffer, sizeof(buffer), &request);
	open_sized_seek(&job, (uint64_t)INT64_MAX, &action);
	CHECK(action.offset == (uint64_t)INT64_MAX, "INT64_MAX seek truncated");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);

	scenario = "positive download beyond EOF";
	request = request_for(AIUS_SD_JOB_DOWNLOAD);
	request.initial_offset = 1u;
	request.total = 2u;
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 2u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_RANGE,
	      "positive beyond-EOF fault mismatch");

	scenario = "append part size mismatch";
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.initial_offset = 2u;
	request.total = 0u;
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_RANGE,
	      "append size mismatch fault mismatch");

	scenario = "final part post-write size mismatch";
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.initial_offset = 0u;
	request.total = 1u;
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	const uint8_t byte = 1u;
	action = supply_and_write(
		&job, action, &byte, 1u, 1u, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_RANGE,
	      "final size mismatch fault mismatch");
}

static void test_reserved_action_id_properties(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {1u};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "reserved close and temp cleanup ids";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	job.next_action_id = UINT64_MAX - 2u;
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	CHECK(action.action_id == UINT64_MAX - 2u, "reserved close id mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(action.action_id == UINT64_MAX - 1u,
	      "reserved temp-existence id mismatch");
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(action.action_id == UINT64_MAX,
	      "reserved temp-removal id mismatch");
	CHECK(job.action_ids_exhausted,
	      "reserved temp removal did not exhaust ids");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "reserved cleanup terminal mismatch");

	scenario = "reserved rollback id";
	action = reach_backup_rename(&job, buffer, data, sizeof(data));
	job.next_action_id = UINT64_MAX - 2u;
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_RENAME);
	CHECK(action.action_id == UINT64_MAX - 2u, "reserved rollback id mismatch");
	CHECK(strcmp(action.path, job.backup_path) == 0,
	      "reserved rollback source mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(action.action_id == UINT64_MAX - 1u,
	      "reserved rollback cleanup existence id mismatch");
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(action.action_id == UINT64_MAX,
	      "reserved rollback cleanup removal id mismatch");
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);

	scenario = "consume UINT64_MAX and saturate lifetime";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	job.next_action_id = UINT64_MAX;
	action = step_action(
		&job, 0u, fault_event(action.action_id, 101u), AIUS_SD_ACTION_EXISTS);
	CHECK(action.action_id == UINT64_MAX, "cleanup did not consume UINT64_MAX");
	CHECK(job.action_ids_exhausted, "UINT64_MAX did not exhaust ids");
	CHECK(job.next_action_id == UINT64_MAX, "next id did not saturate");
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, false),
	                     AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_OPEN,
	      "exhaustion changed prior terminal cause");
	step_none(&job, AIUS_SD_ACTION_NONE);

	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = 1u;
	aius_sd_job_start(&job, &request, 0u, 0u);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_OVERFLOW,
	                0u);
	CHECK(job.next_action_id == UINT64_MAX, "restart unsaturated lifetime id");

	scenario = "fresh init resets exhausted authority-free lifetime";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	request = request_for(AIUS_SD_JOB_PROBE);
	aius_sd_job_start(&job, &request, 0u, 0u);
	action = begin_job(&job);
	CHECK(action.action_id == 1u, "fresh init did not reset id to one");
}

static void test_exact_262_id_publication_reserve(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	uint8_t data[CONFIG_LIMIT];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	const uint64_t tail_start = UINT64_MAX - UINT64_C(261);
	for (size_t index = 0u; index < sizeof(data); ++index)
		data[index] = (uint8_t)index;
	request.total = sizeof(data);
	request.publish = true;
	request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	request.content_type = "application/json";

	scenario = "exact 262-id worst-case publication tail";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job,
		action,
		data,
		sizeof(data),
		sizeof(data),
		AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_existing_target(&job, action, true);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, job.temp_path) == 0 &&
		      strcmp(action.path_to, job.target_path) == 0,
	      "reserve fixture install rename mismatch");

	job.next_action_id = tail_start;
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_EXISTS);
	CHECK(action.action_id == tail_start,
	      "publication tail backup existence id mismatch");
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
	      "publication tail backup purpose mismatch");
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(action.action_id == tail_start + 1u,
	      "publication tail backup remove id mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	CHECK(action.action_id == tail_start + 2u,
	      "publication tail open id mismatch");
	CHECK(strcmp(action.path, job.target_path) == 0,
	      "publication tail open path mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	CHECK(action.action_id == tail_start + 3u,
	      "publication tail size id mismatch");
	action = step_action(&job,
	                     0u,
	                     size_event(action.action_id, sizeof(data)),
	                     AIUS_SD_ACTION_READ);
	for (size_t index = 0u; index < sizeof(data); ++index) {
		CHECK(action.action_id == tail_start + 4u + index,
		      "publication tail read id mismatch");
		CHECK(action.buffer == buffer + index,
		      "publication tail read buffer mismatch");
		CHECK(action.length == sizeof(data) - index,
		      "publication tail read length mismatch");
		CHECK(action.offset == index,
		      "publication tail read offset mismatch");
		action.buffer[0] = data[index];
		action = step_action(
			&job,
			0u,
			bytes_event(action.action_id, 1u),
			index + 1u == sizeof(data) ? AIUS_SD_ACTION_CLOSE
			                          : AIUS_SD_ACTION_READ);
	}
	CHECK(action.action_id == UINT64_MAX - 1u,
	      "publication tail close id mismatch");
	action = acknowledge_ok(
		&job, &action, AIUS_SD_ACTION_PUBLISH_CONFIG);
	CHECK(action.action_id == UINT64_MAX,
	      "publication tail publish did not consume UINT64_MAX");
	CHECK(action.buffer == buffer && action.length == sizeof(data),
	      "publication tail publish buffer mismatch");
	CHECK(memcmp(action.buffer, data, sizeof(data)) == 0,
	      "publication tail publish data mismatch");
	CHECK(job.action_ids_exhausted,
	      "publication tail did not exhaust object lifetime");
	CHECK(job.next_action_id == UINT64_MAX,
	      "publication tail next id did not saturate");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
	step_none(&job, AIUS_SD_ACTION_NONE);

	aius_sd_job_start(&job, &request, 0u, 0u);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_OVERFLOW,
	                0u);
	CHECK(job.next_action_id == UINT64_MAX,
	      "exhausted publication lifetime reused an id");
}

typedef enum {
	MATRIX_PROBE,
	MATRIX_CONFIG,
	MATRIX_USAGE,
	MATRIX_REPLACE,
	MATRIX_APPEND,
	MATRIX_FINALIZE,
	MATRIX_DOWNLOAD,
	MATRIX_REFILL,
	MATRIX_DELETE,
	MATRIX_REPLACE_PUBLISH
} MatrixProfile;

typedef enum {
	MATRIX_CLEAN_PLAIN,
	MATRIX_CLEAN_CLOSE,
	MATRIX_CLEAN_REMOVE_TEMP,
	MATRIX_CLEAN_CLOSE_REMOVE_TEMP,
	MATRIX_CLEAN_ROLLBACK_REMOVE_TEMP,
	MATRIX_CLEAN_COMMIT_EXISTS,
	MATRIX_CLEAN_COMMIT_REMOVE,
	MATRIX_CLEAN_EXISTING_INSTALL,
	MATRIX_CLEAN_CANCEL_CLOSE_REMOVE,
	MATRIX_CLEAN_CANCEL_CLOSE_DURABLE,
	MATRIX_CLEAN_EXISTING_OPEN_EXISTS,
	MATRIX_CLEAN_EXISTING_OPEN_REMOVE
} MatrixCleanup;

typedef struct {
	const char *name;
	MatrixProfile profile;
	AiusSdPhase phase;
	AiusSdActionKind action;
	MatrixCleanup cleanup;
	bool target_exists;
	AiusSdExistencePurpose purpose;
} MatrixFixture;

typedef struct {
	AiusSdJob job;
	AiusSdAction action;
	uint8_t buffer[CONFIG_LIMIT];
} MatrixState;

static AiusSdJobRequest matrix_request(MatrixProfile profile)
{
	AiusSdJobRequest request;
	switch (profile) {
	case MATRIX_PROBE:
		return request_for(AIUS_SD_JOB_PROBE);
	case MATRIX_CONFIG:
		return request_for(AIUS_SD_JOB_CONFIG_LOAD);
	case MATRIX_USAGE:
		return request_for(AIUS_SD_JOB_USAGE_REFRESH);
	case MATRIX_REPLACE:
		request = upload_request(AIUS_SD_INSTALL_REPLACE);
		request.total = 1u;
		return request;
	case MATRIX_APPEND:
		request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
		request.total = 1u;
		return request;
	case MATRIX_FINALIZE:
		request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
		request.total = 1u;
		return request;
	case MATRIX_DOWNLOAD:
		request = request_for(AIUS_SD_JOB_DOWNLOAD);
		request.total = 1u;
		return request;
	case MATRIX_REFILL:
		request = request_for(AIUS_SD_JOB_REPLAY_REFILL);
		request.total = 1u;
		return request;
	case MATRIX_DELETE:
		return request_for(AIUS_SD_JOB_DELETE);
	case MATRIX_REPLACE_PUBLISH:
		request = upload_request(AIUS_SD_INSTALL_REPLACE);
		request.total = 1u;
		request.publish = true;
		request.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
		request.content_type = "application/json";
		return request;
	}
	fail_at(__LINE__, "unknown matrix profile");
	return request_for(AIUS_SD_JOB_PROBE);
}

static uint64_t matrix_size_for_phase(AiusSdPhase phase)
{
	switch (phase) {
	case AIUS_SD_PHASE_USAGE_TOTAL:
		return 10u;
	case AIUS_SD_PHASE_USAGE_USED:
		return 5u;
	case AIUS_SD_PHASE_PART_SIZE:
		return 0u;
	case AIUS_SD_PHASE_CONFIG_SIZE:
	case AIUS_SD_PHASE_FINAL_SIZE:
	case AIUS_SD_PHASE_DOWNLOAD_SIZE:
	case AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE:
		return 1u;
	default:
		return 1u;
	}
}

static AiusSdEvent matrix_success_event(
	const MatrixFixture *fixture,
	const AiusSdAction *action,
	AiusSdPhase phase)
{
	switch (action->kind) {
	case AIUS_SD_ACTION_MEDIA_PRESENT:
		return bool_event(action->action_id, true);
	case AIUS_SD_ACTION_EXISTS:
		return bool_event(action->action_id, fixture->target_exists);
	case AIUS_SD_ACTION_FILE_SIZE:
	case AIUS_SD_ACTION_TOTAL_SIZE:
	case AIUS_SD_ACTION_USED_SIZE:
		return size_event(action->action_id,
		                  matrix_size_for_phase(phase));
	case AIUS_SD_ACTION_READ:
	case AIUS_SD_ACTION_WRITE:
		return bytes_event(action->action_id, action->length);
	default:
		return ok_event(action->action_id);
	}
}

static void matrix_step_unknown(
	AiusSdJob *job,
	AiusSdEvent event,
	AiusSdAction *action)
{
	memset(action, 0xa5, sizeof(*action));
	aius_sd_job_step(job, 0u, event, action);
	validate_action(action);
}

static void matrix_build_happy(
	MatrixState *state,
	const MatrixFixture *fixture)
{
	AiusSdJobRequest request = matrix_request(fixture->profile);
	AiusSdEvent event = none_event();
	aius_sd_job_init(&state->job, state->buffer, sizeof(state->buffer));
	aius_sd_job_start(&state->job, &request, 0u, 0u);
	for (size_t steps = 0u; steps < 128u; ++steps) {
		matrix_step_unknown(&state->job, event, &state->action);
		CHECK(state->action.kind != AIUS_SD_ACTION_FAIL,
		      "matrix builder reached failure");
		CHECK(state->action.kind != AIUS_SD_ACTION_COMPLETE,
		      "matrix builder missed fixture");
		if (state->job.phase == fixture->phase &&
		    state->action.kind == fixture->action &&
		    (fixture->purpose == AIUS_SD_EXISTS_NONE ||
		     state->job.existence_purpose == fixture->purpose))
			return;
		if (state->action.kind == AIUS_SD_ACTION_NEED_INPUT) {
			const uint8_t byte = 0x3cu;
			CHECK(aius_sd_job_supply(&state->job, &byte, 1u),
			      "matrix builder supply rejected");
			event = none_event();
			continue;
		}
		if (state->action.kind == AIUS_SD_ACTION_READ)
			memset(state->action.buffer, 0x3c, state->action.length);
		event = matrix_success_event(
			fixture, &state->action, state->job.phase);
	}
	fail_at(__LINE__, "matrix builder step bound exceeded");
}

static void matrix_build_fixture(
	MatrixState *state,
	const MatrixFixture *fixture)
{
	MatrixFixture precursor;
	AiusSdEvent event;

	if (fixture->phase == AIUS_SD_PHASE_ROLLBACK) {
		precursor = *fixture;
		precursor.phase = AIUS_SD_PHASE_INSTALL_RENAME;
		precursor.action = AIUS_SD_ACTION_RENAME;
		precursor.target_exists = true;
		matrix_build_happy(state, &precursor);
		event = fault_event(state->action.action_id, 120u);
		state->action = step_action(
			&state->job, 0u, event, AIUS_SD_ACTION_RENAME);
		CHECK(state->job.phase == AIUS_SD_PHASE_ROLLBACK,
		      "matrix rollback fixture phase mismatch");
		return;
	}
	if (fixture->phase == AIUS_SD_PHASE_CLEANUP_CLOSE) {
		precursor = *fixture;
		precursor.phase = AIUS_SD_PHASE_UPLOAD_OPEN;
		precursor.action = fixture->profile == MATRIX_REPLACE
			                   ? AIUS_SD_ACTION_OPEN_WRITE_BEGIN
			                   : AIUS_SD_ACTION_OPEN_APPEND;
		matrix_build_happy(state, &precursor);
		aius_sd_job_cancel(&state->job);
		state->action = step_action(&state->job,
		                            0u,
		                            ok_event(state->action.action_id),
		                            AIUS_SD_ACTION_CLOSE);
		CHECK(state->job.phase == AIUS_SD_PHASE_CLEANUP_CLOSE,
		      "matrix cleanup-close fixture phase mismatch");
		return;
	}
	if (fixture->phase == AIUS_SD_PHASE_CLEANUP_TEMP ||
	    fixture->purpose == AIUS_SD_EXISTS_CLEANUP_TEMP) {
		precursor = *fixture;
		precursor.phase = AIUS_SD_PHASE_UPLOAD_OPEN;
		precursor.action = AIUS_SD_ACTION_OPEN_WRITE_BEGIN;
		precursor.purpose = AIUS_SD_EXISTS_NONE;
		matrix_build_happy(state, &precursor);
		state->action = step_action(
			&state->job,
			0u,
			fault_event(state->action.action_id, 121u),
			AIUS_SD_ACTION_EXISTS);
		if (fixture->purpose == AIUS_SD_EXISTS_CLEANUP_TEMP)
			return;
		state->action = step_action(
			&state->job,
			0u,
			bool_event(state->action.action_id, true),
			AIUS_SD_ACTION_REMOVE);
		CHECK(state->job.phase == AIUS_SD_PHASE_CLEANUP_TEMP,
		      "matrix cleanup-temp fixture phase mismatch");
		return;
	}
	matrix_build_happy(state, fixture);
}

static const char *const matrix_plan_names[] = {
	"begin",
	"probe-media",
	"config-open",
	"config-size",
	"config-read",
	"config-close",
	"config-publish",
	"usage-total",
	"usage-used",
	"path-exists-mkdir",
	"mkdir-replace",
	"mkdir-append",
	"mkdir-finalize",
	"part-open-append",
	"part-open-finalize",
	"part-size-append",
	"part-size-finalize",
	"part-close-append",
	"part-close-finalize",
	"upload-open-replace",
	"upload-open-append",
	"upload-open-finalize",
	"upload-truncate-replace",
	"upload-write-replace",
	"upload-write-append",
	"upload-write-finalize",
	"upload-close-replace",
	"upload-close-append",
	"upload-close-finalize",
	"final-size-open",
	"final-size",
	"final-size-close",
	"download-open",
	"refill-open",
	"download-size",
	"refill-size",
	"download-seek",
	"refill-seek",
	"download-read",
	"refill-read",
	"download-output",
	"refill-output",
	"download-close",
	"refill-close",
	"delete-exists",
	"delete-remove",
	"install-target-exists",
	"remove-old-backup-exists",
	"install-remove-old-backup",
	"install-backup-rename",
	"install-rename-no-backup",
	"install-rename-with-backup",
	"remove-committed-backup-exists",
	"install-remove-backup",
	"publish-open",
	"publish-size",
	"publish-read",
	"publish-close",
	"publish-config",
	"rollback-rename",
	"cleanup-close-replace",
	"cleanup-close-append",
	"cleanup-temp-exists",
	"cleanup-temp-replace",
};

#define MATRIX_FIXTURE(name_, profile_, phase_, action_, cleanup_, exists_) \
	{(name_), (profile_), (phase_), (action_), (cleanup_), (exists_), \
	 AIUS_SD_EXISTS_NONE}

#define MATRIX_EXISTS_FIXTURE( \
	name_, profile_, purpose_, cleanup_, exists_) \
	{(name_), (profile_), AIUS_SD_PHASE_PATH_EXISTS, AIUS_SD_ACTION_EXISTS, \
	 (cleanup_), (exists_), (purpose_)}

static const MatrixFixture matrix_fixtures[] = {
	MATRIX_FIXTURE("begin", MATRIX_PROBE, AIUS_SD_PHASE_BEGIN,
	               AIUS_SD_ACTION_BEGIN, MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("probe-media", MATRIX_PROBE, AIUS_SD_PHASE_PROBE_MEDIA,
	               AIUS_SD_ACTION_MEDIA_PRESENT, MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("config-open", MATRIX_CONFIG, AIUS_SD_PHASE_CONFIG_OPEN,
	               AIUS_SD_ACTION_OPEN_READ, MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("config-size", MATRIX_CONFIG, AIUS_SD_PHASE_CONFIG_SIZE,
	               AIUS_SD_ACTION_FILE_SIZE, MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("config-read", MATRIX_CONFIG, AIUS_SD_PHASE_CONFIG_READ,
	               AIUS_SD_ACTION_READ, MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("config-close", MATRIX_CONFIG, AIUS_SD_PHASE_CONFIG_CLOSE,
	               AIUS_SD_ACTION_CLOSE, MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("config-publish", MATRIX_CONFIG, AIUS_SD_PHASE_CONFIG_PUBLISH,
	               AIUS_SD_ACTION_PUBLISH_CONFIG, MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("usage-total", MATRIX_USAGE, AIUS_SD_PHASE_USAGE_TOTAL,
	               AIUS_SD_ACTION_TOTAL_SIZE, MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("usage-used", MATRIX_USAGE, AIUS_SD_PHASE_USAGE_USED,
	               AIUS_SD_ACTION_USED_SIZE, MATRIX_CLEAN_PLAIN, false),
	MATRIX_EXISTS_FIXTURE("path-exists-mkdir", MATRIX_REPLACE,
	                      AIUS_SD_EXISTS_MKDIR,
	                      MATRIX_CLEAN_REMOVE_TEMP, false),
	MATRIX_FIXTURE("mkdir-replace", MATRIX_REPLACE, AIUS_SD_PHASE_MKDIR,
	               AIUS_SD_ACTION_MKDIR, MATRIX_CLEAN_REMOVE_TEMP, false),
	MATRIX_FIXTURE("mkdir-append", MATRIX_APPEND, AIUS_SD_PHASE_MKDIR,
	               AIUS_SD_ACTION_MKDIR, MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("mkdir-finalize", MATRIX_FINALIZE, AIUS_SD_PHASE_MKDIR,
	               AIUS_SD_ACTION_MKDIR, MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("part-open-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_PART_SIZE_OPEN, AIUS_SD_ACTION_OPEN_READ,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("part-open-finalize", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_PART_SIZE_OPEN, AIUS_SD_ACTION_OPEN_READ,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("part-size-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_PART_SIZE, AIUS_SD_ACTION_FILE_SIZE,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("part-size-finalize", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_PART_SIZE, AIUS_SD_ACTION_FILE_SIZE,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("part-close-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_PART_SIZE_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("part-close-finalize", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_PART_SIZE_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("upload-open-replace", MATRIX_REPLACE,
	               AIUS_SD_PHASE_UPLOAD_OPEN,
	               AIUS_SD_ACTION_OPEN_WRITE_BEGIN,
	               MATRIX_CLEAN_CLOSE_REMOVE_TEMP, false),
	MATRIX_FIXTURE("upload-open-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_UPLOAD_OPEN, AIUS_SD_ACTION_OPEN_APPEND,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("upload-open-finalize", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_UPLOAD_OPEN, AIUS_SD_ACTION_OPEN_APPEND,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("upload-truncate-replace", MATRIX_REPLACE,
	               AIUS_SD_PHASE_UPLOAD_TRUNCATE,
	               AIUS_SD_ACTION_TRUNCATE,
	               MATRIX_CLEAN_CLOSE_REMOVE_TEMP, false),
	MATRIX_FIXTURE("upload-write-replace", MATRIX_REPLACE,
	               AIUS_SD_PHASE_UPLOAD_WRITE, AIUS_SD_ACTION_WRITE,
	               MATRIX_CLEAN_CLOSE_REMOVE_TEMP, false),
	MATRIX_FIXTURE("upload-write-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_UPLOAD_WRITE, AIUS_SD_ACTION_WRITE,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("upload-write-finalize", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_UPLOAD_WRITE, AIUS_SD_ACTION_WRITE,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("upload-close-replace", MATRIX_REPLACE,
	               AIUS_SD_PHASE_UPLOAD_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_REMOVE_TEMP, false),
	MATRIX_FIXTURE("upload-close-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_UPLOAD_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("upload-close-finalize", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_UPLOAD_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("final-size-open", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_FINAL_SIZE_OPEN, AIUS_SD_ACTION_OPEN_READ,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("final-size", MATRIX_FINALIZE, AIUS_SD_PHASE_FINAL_SIZE,
	               AIUS_SD_ACTION_FILE_SIZE, MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("final-size-close", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_FINAL_SIZE_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("download-open", MATRIX_DOWNLOAD,
	               AIUS_SD_PHASE_DOWNLOAD_OPEN, AIUS_SD_ACTION_OPEN_READ,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("refill-open", MATRIX_REFILL, AIUS_SD_PHASE_DOWNLOAD_OPEN,
	               AIUS_SD_ACTION_OPEN_READ, MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("download-size", MATRIX_DOWNLOAD,
	               AIUS_SD_PHASE_DOWNLOAD_SIZE, AIUS_SD_ACTION_FILE_SIZE,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("refill-size", MATRIX_REFILL, AIUS_SD_PHASE_DOWNLOAD_SIZE,
	               AIUS_SD_ACTION_FILE_SIZE, MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("download-seek", MATRIX_DOWNLOAD,
	               AIUS_SD_PHASE_DOWNLOAD_SEEK, AIUS_SD_ACTION_SEEK,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("refill-seek", MATRIX_REFILL,
	               AIUS_SD_PHASE_DOWNLOAD_SEEK, AIUS_SD_ACTION_SEEK,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("download-read", MATRIX_DOWNLOAD,
	               AIUS_SD_PHASE_DOWNLOAD_READ, AIUS_SD_ACTION_READ,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("refill-read", MATRIX_REFILL,
	               AIUS_SD_PHASE_DOWNLOAD_READ, AIUS_SD_ACTION_READ,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("download-output", MATRIX_DOWNLOAD,
	               AIUS_SD_PHASE_DOWNLOAD_OUTPUT, AIUS_SD_ACTION_OUTPUT,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("refill-output", MATRIX_REFILL,
	               AIUS_SD_PHASE_DOWNLOAD_OUTPUT, AIUS_SD_ACTION_OUTPUT,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("download-close", MATRIX_DOWNLOAD,
	               AIUS_SD_PHASE_DOWNLOAD_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("refill-close", MATRIX_REFILL,
	               AIUS_SD_PHASE_DOWNLOAD_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_EXISTS_FIXTURE("delete-exists", MATRIX_DELETE,
	                      AIUS_SD_EXISTS_DELETE,
	                      MATRIX_CLEAN_PLAIN, true),
	MATRIX_FIXTURE("delete-remove", MATRIX_DELETE,
	               AIUS_SD_PHASE_DELETE_REMOVE, AIUS_SD_ACTION_REMOVE,
	               MATRIX_CLEAN_PLAIN, true),
	MATRIX_EXISTS_FIXTURE("install-target-exists", MATRIX_REPLACE,
	                      AIUS_SD_EXISTS_INSTALL_TARGET,
	                      MATRIX_CLEAN_REMOVE_TEMP, true),
	MATRIX_EXISTS_FIXTURE("remove-old-backup-exists", MATRIX_REPLACE,
	                      AIUS_SD_EXISTS_REMOVE_OLD_BACKUP,
	                      MATRIX_CLEAN_REMOVE_TEMP, true),
	MATRIX_FIXTURE("install-remove-old-backup", MATRIX_REPLACE,
	               AIUS_SD_PHASE_INSTALL_REMOVE_OLD_BACKUP,
	               AIUS_SD_ACTION_REMOVE, MATRIX_CLEAN_REMOVE_TEMP, true),
	MATRIX_FIXTURE("install-backup-rename", MATRIX_REPLACE,
	               AIUS_SD_PHASE_INSTALL_BACKUP, AIUS_SD_ACTION_RENAME,
	               MATRIX_CLEAN_ROLLBACK_REMOVE_TEMP, true),
	MATRIX_FIXTURE("install-rename-no-backup", MATRIX_REPLACE,
	               AIUS_SD_PHASE_INSTALL_RENAME, AIUS_SD_ACTION_RENAME,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("install-rename-with-backup", MATRIX_REPLACE,
	               AIUS_SD_PHASE_INSTALL_RENAME, AIUS_SD_ACTION_RENAME,
	               MATRIX_CLEAN_COMMIT_EXISTS, true),
	MATRIX_EXISTS_FIXTURE("remove-committed-backup-exists", MATRIX_REPLACE,
	                      AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
	                      MATRIX_CLEAN_COMMIT_REMOVE, true),
	MATRIX_FIXTURE("install-remove-backup", MATRIX_REPLACE,
	               AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP, AIUS_SD_ACTION_REMOVE,
	               MATRIX_CLEAN_PLAIN, true),
	MATRIX_FIXTURE("publish-open", MATRIX_REPLACE_PUBLISH,
	               AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN,
	               AIUS_SD_ACTION_OPEN_READ, MATRIX_CLEAN_CLOSE, true),
	MATRIX_FIXTURE("publish-size", MATRIX_REPLACE_PUBLISH,
	               AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE,
	               AIUS_SD_ACTION_FILE_SIZE, MATRIX_CLEAN_CLOSE, true),
	MATRIX_FIXTURE("publish-read", MATRIX_REPLACE_PUBLISH,
	               AIUS_SD_PHASE_INSTALL_PUBLISH_READ, AIUS_SD_ACTION_READ,
	               MATRIX_CLEAN_CLOSE, true),
	MATRIX_FIXTURE("publish-close", MATRIX_REPLACE_PUBLISH,
	               AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_PLAIN, true),
	MATRIX_FIXTURE("publish-config", MATRIX_REPLACE_PUBLISH,
	               AIUS_SD_PHASE_INSTALL_PUBLISH,
	               AIUS_SD_ACTION_PUBLISH_CONFIG, MATRIX_CLEAN_PLAIN, true),
	MATRIX_FIXTURE("rollback-rename", MATRIX_REPLACE,
	               AIUS_SD_PHASE_ROLLBACK, AIUS_SD_ACTION_RENAME,
	               MATRIX_CLEAN_EXISTING_INSTALL, true),
	MATRIX_FIXTURE("cleanup-close-replace", MATRIX_REPLACE,
	               AIUS_SD_PHASE_CLEANUP_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_CANCEL_CLOSE_REMOVE, false),
	MATRIX_FIXTURE("cleanup-close-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_CLEANUP_CLOSE, AIUS_SD_ACTION_CLOSE,
	               MATRIX_CLEAN_CANCEL_CLOSE_DURABLE, false),
	MATRIX_EXISTS_FIXTURE("cleanup-temp-exists", MATRIX_REPLACE,
	                      AIUS_SD_EXISTS_CLEANUP_TEMP,
	                      MATRIX_CLEAN_EXISTING_OPEN_EXISTS, true),
	MATRIX_FIXTURE("cleanup-temp-replace", MATRIX_REPLACE,
	               AIUS_SD_PHASE_CLEANUP_TEMP, AIUS_SD_ACTION_REMOVE,
	               MATRIX_CLEAN_EXISTING_OPEN_REMOVE, false),
};

#undef MATRIX_FIXTURE
#undef MATRIX_EXISTS_FIXTURE

static MatrixState matrix_clone(const MatrixState *base)
{
	MatrixState clone = *base;
	clone.job.buffer = clone.buffer;
	return clone;
}

static AiusSdActionKind matrix_first_cleanup(MatrixCleanup cleanup)
{
	switch (cleanup) {
	case MATRIX_CLEAN_PLAIN:
	case MATRIX_CLEAN_EXISTING_INSTALL:
	case MATRIX_CLEAN_CANCEL_CLOSE_DURABLE:
	case MATRIX_CLEAN_EXISTING_OPEN_REMOVE:
		return AIUS_SD_ACTION_FAIL;
	case MATRIX_CLEAN_CLOSE:
	case MATRIX_CLEAN_CLOSE_REMOVE_TEMP:
		return AIUS_SD_ACTION_CLOSE;
	case MATRIX_CLEAN_REMOVE_TEMP:
	case MATRIX_CLEAN_CANCEL_CLOSE_REMOVE:
	case MATRIX_CLEAN_COMMIT_EXISTS:
		return AIUS_SD_ACTION_EXISTS;
	case MATRIX_CLEAN_COMMIT_REMOVE:
	case MATRIX_CLEAN_EXISTING_OPEN_EXISTS:
		return AIUS_SD_ACTION_REMOVE;
	case MATRIX_CLEAN_ROLLBACK_REMOVE_TEMP:
		return AIUS_SD_ACTION_RENAME;
	}
	fail_at(__LINE__, "unknown matrix cleanup");
	return AIUS_SD_ACTION_NONE;
}

static void matrix_expect_terminal_fault(
	const MatrixFixture *fixture,
	const AiusSdAction *action,
	const AiusSdJob *job)
{
	AiusSdFault fault = AIUS_SD_FAULT_PROTOCOL;
	uint32_t driver_fault = 0u;
	switch (fixture->cleanup) {
	case MATRIX_CLEAN_EXISTING_INSTALL:
		fault = AIUS_SD_FAULT_INSTALL;
		driver_fault = 120u;
		break;
	case MATRIX_CLEAN_CANCEL_CLOSE_REMOVE:
	case MATRIX_CLEAN_CANCEL_CLOSE_DURABLE:
		fault = AIUS_SD_FAULT_CANCELLED;
		break;
	case MATRIX_CLEAN_EXISTING_OPEN_EXISTS:
	case MATRIX_CLEAN_EXISTING_OPEN_REMOVE:
		fault = AIUS_SD_FAULT_OPEN;
		driver_fault = 121u;
		break;
	default:
		break;
	}
	CHECK(action->kind == AIUS_SD_ACTION_FAIL,
	      "matrix cleanup omitted terminal FAIL");
	CHECK(action->fault == (uint32_t)fault,
	      "matrix terminal fault mismatch");
	expect_result(job, AIUS_SD_RESULT_FAULT, fault, driver_fault);
}

static void matrix_drain_cleanup(
	MatrixState *state,
	const MatrixFixture *fixture,
	AiusSdAction action)
{
	switch (fixture->cleanup) {
	case MATRIX_CLEAN_PLAIN:
	case MATRIX_CLEAN_EXISTING_INSTALL:
	case MATRIX_CLEAN_CANCEL_CLOSE_DURABLE:
	case MATRIX_CLEAN_EXISTING_OPEN_REMOVE:
		break;
	case MATRIX_CLEAN_CLOSE:
		CHECK(action.kind == AIUS_SD_ACTION_CLOSE,
		      "matrix expected cleanup close");
		action = acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_REMOVE_TEMP:
	case MATRIX_CLEAN_CANCEL_CLOSE_REMOVE:
		CHECK(action.kind == AIUS_SD_ACTION_EXISTS,
		      "matrix expected temp existence");
		action = remove_disposable_temp(
			&state->job, action, true, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_CLOSE_REMOVE_TEMP:
		CHECK(action.kind == AIUS_SD_ACTION_CLOSE,
		      "matrix expected close before temp cleanup");
		action =
			acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_EXISTS);
		action = remove_disposable_temp(
			&state->job, action, true, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_ROLLBACK_REMOVE_TEMP:
		CHECK(action.kind == AIUS_SD_ACTION_RENAME,
		      "matrix expected rollback rename");
		CHECK(strcmp(action.path, state->job.backup_path) == 0 &&
			      strcmp(action.path_to, state->job.target_path) == 0,
		      "matrix rollback paths mismatch");
		action =
			acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_EXISTS);
		action = remove_disposable_temp(
			&state->job, action, true, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_COMMIT_EXISTS:
		CHECK(action.kind == AIUS_SD_ACTION_EXISTS,
		      "matrix expected committed backup existence");
		action = finish_committed_backup(
			&state->job, action, true, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_COMMIT_REMOVE:
	case MATRIX_CLEAN_EXISTING_OPEN_EXISTS:
		CHECK(action.kind == AIUS_SD_ACTION_REMOVE,
		      "matrix expected existence-routed removal");
		action = acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_FAIL);
		break;
	}
	matrix_expect_terminal_fault(fixture, &action, &state->job);
}

static AiusSdEvent matrix_wrong_kind_event(
	AiusSdEventKind kind,
	uint64_t action_id)
{
	switch (kind) {
	case AIUS_SD_EVENT_OK:
		return ok_event(action_id);
	case AIUS_SD_EVENT_BYTES:
		return bytes_event(action_id, 1u);
	case AIUS_SD_EVENT_BOOL:
		return bool_event(action_id, true);
	case AIUS_SD_EVENT_SIZE:
		return size_event(action_id, 1u);
	default:
		fail_at(__LINE__, "invalid wrong event kind");
		return none_event();
	}
}

static void matrix_malformed_then_resolve(
	const MatrixState *base,
	const MatrixFixture *fixture,
	AiusSdEvent malformed)
{
	MatrixState state = matrix_clone(base);
	AiusSdAction outstanding = state.action;
	AiusSdState prior_state = state.job.state;
	AiusSdPhase prior_phase = state.job.phase;
	AiusSdAction action = step_action(
		&state.job, 0u, malformed, AIUS_SD_ACTION_NONE);
	(void)action;
	CHECK(state.job.outstanding_action_id == outstanding.action_id,
	      "matrix malformed event cleared action id");
	CHECK(state.job.outstanding_action == outstanding.kind,
	      "matrix malformed event cleared action kind");
	CHECK(state.job.state == prior_state && state.job.phase == prior_phase,
	      "matrix malformed event changed authority phase");
	AiusSdEvent valid =
		matrix_success_event(fixture, &outstanding, prior_phase);
	action = step_action(&state.job,
	                     0u,
	                     valid,
	                     matrix_first_cleanup(fixture->cleanup));
	matrix_drain_cleanup(&state, fixture, action);
}

static size_t matrix_run_fixture(const MatrixFixture *fixture)
{
	MatrixState base;
	AiusSdEvent valid;
	AiusSdEvent malformed;
	AiusSdEventKind success_kind;
	size_t cases = 0u;
	matrix_build_fixture(&base, fixture);
	CHECK(base.job.phase == fixture->phase,
	      "matrix fixture phase mismatch");
	CHECK(base.action.kind == fixture->action,
	      "matrix fixture action mismatch");
	if (fixture->purpose != AIUS_SD_EXISTS_NONE)
		CHECK(base.job.existence_purpose == fixture->purpose,
		      "matrix fixture existence purpose mismatch");
	CHECK(base.job.outstanding_action_id == base.action.action_id,
	      "matrix fixture action id mismatch");
	CHECK(base.job.outstanding_action == base.action.kind,
	      "matrix fixture outstanding kind mismatch");

	scenario = fixture->name;
	MatrixState waiting = matrix_clone(&base);
	step_action(&waiting.job, 0u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(waiting.job.outstanding_action_id == base.action.action_id,
	      "matrix empty NONE cleared authority");
	++cases;

	valid = matrix_success_event(fixture, &base.action, base.job.phase);
	success_kind = valid.kind;
	malformed = valid;
	malformed.action_id = 0u;
	matrix_malformed_then_resolve(&base, fixture, malformed);
	++cases;
	malformed = valid;
	malformed.action_id = base.action.action_id + 1u;
	matrix_malformed_then_resolve(&base, fixture, malformed);
	++cases;

	const AiusSdEventKind candidate_kinds[] = {
		AIUS_SD_EVENT_OK,
		AIUS_SD_EVENT_BYTES,
		AIUS_SD_EVENT_BOOL,
		AIUS_SD_EVENT_SIZE,
	};
	for (size_t index = 0; index < ARRAY_COUNT(candidate_kinds); ++index) {
		if (candidate_kinds[index] == success_kind)
			continue;
		malformed = matrix_wrong_kind_event(
			candidate_kinds[index], base.action.action_id);
		matrix_malformed_then_resolve(&base, fixture, malformed);
		++cases;
	}

	if (success_kind != AIUS_SD_EVENT_BYTES) {
		malformed = valid;
		malformed.bytes = 1u;
		matrix_malformed_then_resolve(&base, fixture, malformed);
		++cases;
	}
	if (success_kind != AIUS_SD_EVENT_SIZE) {
		malformed = valid;
		malformed.size = 1u;
		matrix_malformed_then_resolve(&base, fixture, malformed);
		++cases;
	}
	if (success_kind != AIUS_SD_EVENT_BOOL) {
		malformed = valid;
		malformed.value = true;
		matrix_malformed_then_resolve(&base, fixture, malformed);
		++cases;
	}
	malformed = valid;
	malformed.fault = 1u;
	matrix_malformed_then_resolve(&base, fixture, malformed);
	++cases;

	const AiusSdEvent malformed_faults[] = {
		{.kind = AIUS_SD_EVENT_FAULT, .fault = 7u},
		{.kind = AIUS_SD_EVENT_FAULT, .action_id = 1u, .fault = 7u},
		{.kind = AIUS_SD_EVENT_FAULT, .action_id = 2u},
		{.kind = AIUS_SD_EVENT_FAULT,
		 .action_id = 2u,
		 .bytes = 1u,
		 .fault = 7u},
		{.kind = AIUS_SD_EVENT_FAULT,
		 .action_id = 2u,
		 .size = 1u,
		 .fault = 7u},
		{.kind = AIUS_SD_EVENT_FAULT,
		 .action_id = 2u,
		 .value = true,
		 .fault = 7u},
	};
	for (size_t index = 0; index < ARRAY_COUNT(malformed_faults); ++index) {
		malformed = malformed_faults[index];
		if (malformed.action_id == 1u)
			malformed.action_id = base.action.action_id + 1u;
		else if (malformed.action_id == 2u)
			malformed.action_id = base.action.action_id;
		matrix_malformed_then_resolve(&base, fixture, malformed);
		++cases;
	}
	return cases;
}

static size_t test_fixture_cross_product(void)
{
	size_t cases = 0u;
	CHECK(ARRAY_COUNT(matrix_fixtures) == ARRAY_COUNT(matrix_plan_names),
	      "matrix fixture count differs from plan-owned names");
	for (size_t index = 0; index < ARRAY_COUNT(matrix_plan_names); ++index) {
		CHECK(strcmp(matrix_fixtures[index].name, matrix_plan_names[index]) == 0,
		      "matrix fixture ordering/name completeness mismatch");
		cases += matrix_run_fixture(&matrix_fixtures[index]);
	}
	return cases;
}

static void existence_state(
	MatrixState *state,
	const char *name,
	MatrixProfile profile,
	AiusSdExistencePurpose purpose,
	bool target_exists)
{
	MatrixFixture fixture = {
		name,
		profile,
		AIUS_SD_PHASE_PATH_EXISTS,
		AIUS_SD_ACTION_EXISTS,
		MATRIX_CLEAN_PLAIN,
		target_exists,
		purpose};
	matrix_build_fixture(state, &fixture);
	CHECK(state->action.kind == AIUS_SD_ACTION_EXISTS,
	      "existence fixture action mismatch");
	CHECK(state->job.phase == AIUS_SD_PHASE_PATH_EXISTS,
	      "existence fixture phase mismatch");
	CHECK(state->job.existence_purpose == purpose,
	      "existence fixture purpose mismatch");
}

static size_t test_existence_purpose_routes(void)
{
	MatrixState base;
	MatrixState state;
	AiusSdAction action;

	scenario = "existence MKDIR false route";
	existence_state(
		&base, scenario, MATRIX_REPLACE, AIUS_SD_EXISTS_MKDIR, false);
	CHECK(strcmp(base.action.path, "/final") == 0,
	      "MKDIR existence path mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, false),
	                     AIUS_SD_ACTION_MKDIR);
	CHECK(strcmp(action.path, "/final") == 0,
	      "MKDIR false mutation path mismatch");
	state = matrix_clone(&base);
	scenario = "existence MKDIR true route";
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, true),
	                     AIUS_SD_ACTION_EXISTS);
	CHECK(state.job.existence_purpose == AIUS_SD_EXISTS_MKDIR,
	      "MKDIR true did not select next existence");
	CHECK(strcmp(action.path, "/final/dir") == 0,
	      "MKDIR true next-prefix path mismatch");

	scenario = "existence DELETE routes";
	existence_state(
		&base, scenario, MATRIX_DELETE, AIUS_SD_EXISTS_DELETE, true);
	CHECK(strcmp(base.action.path, base.job.target_path) == 0,
	      "DELETE existence path mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, false),
	                     AIUS_SD_ACTION_COMPLETE);
	CHECK(state.job.existence_purpose == AIUS_SD_EXISTS_NONE,
	      "DELETE false retained purpose");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, state.job.target_path) == 0,
	      "DELETE true removal path mismatch");

	scenario = "existence INSTALL_TARGET routes";
	existence_state(
		&base,
		scenario,
		MATRIX_REPLACE,
		AIUS_SD_EXISTS_INSTALL_TARGET,
		true);
	CHECK(strcmp(base.action.path, base.job.target_path) == 0,
	      "INSTALL_TARGET existence path mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, false),
	                     AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, state.job.temp_path) == 0 &&
		      strcmp(action.path_to, state.job.target_path) == 0,
	      "INSTALL_TARGET false rename mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, true),
	                     AIUS_SD_ACTION_EXISTS);
	CHECK(state.job.existence_purpose == AIUS_SD_EXISTS_REMOVE_OLD_BACKUP,
	      "INSTALL_TARGET true purpose mismatch");
	CHECK(strcmp(action.path, state.job.backup_path) == 0,
	      "INSTALL_TARGET true path mismatch");

	scenario = "existence REMOVE_OLD_BACKUP routes";
	existence_state(
		&base,
		scenario,
		MATRIX_REPLACE,
		AIUS_SD_EXISTS_REMOVE_OLD_BACKUP,
		true);
	CHECK(strcmp(base.action.path, base.job.backup_path) == 0,
	      "REMOVE_OLD_BACKUP existence path mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, false),
	                     AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, state.job.target_path) == 0 &&
		      strcmp(action.path_to, state.job.backup_path) == 0,
	      "REMOVE_OLD_BACKUP false rename mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, state.job.backup_path) == 0,
	      "REMOVE_OLD_BACKUP true removal mismatch");

	scenario = "existence REMOVE_COMMITTED_BACKUP routes";
	existence_state(
		&base,
		scenario,
		MATRIX_REPLACE,
		AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
		true);
	CHECK(strcmp(base.action.path, base.job.backup_path) == 0,
	      "REMOVE_COMMITTED_BACKUP existence path mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, false),
	                     AIUS_SD_ACTION_COMPLETE);
	CHECK(state.job.existence_purpose == AIUS_SD_EXISTS_NONE,
	      "REMOVE_COMMITTED_BACKUP false retained purpose");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, state.job.backup_path) == 0,
	      "REMOVE_COMMITTED_BACKUP true removal mismatch");

	scenario = "existence CLEANUP_TEMP routes";
	existence_state(
		&base,
		scenario,
		MATRIX_REPLACE,
		AIUS_SD_EXISTS_CLEANUP_TEMP,
		true);
	CHECK(strcmp(base.action.path, base.job.temp_path) == 0,
	      "CLEANUP_TEMP existence path mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, false),
	                     AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_OPEN,
	      "CLEANUP_TEMP false fault mismatch");
	state = matrix_clone(&base);
	action = step_action(&state.job,
	                     0u,
	                     bool_event(state.action.action_id, true),
	                     AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, state.job.temp_path) == 0,
	      "CLEANUP_TEMP true removal mismatch");
	return 12u;
}

static size_t test_active_no_outstanding_rows(void)
{
	const MatrixProfile profiles[] = {
		MATRIX_REPLACE, MATRIX_APPEND, MATRIX_FINALIZE};
	const char *const names[] = {
		"need-input-replace",
		"need-input-append",
		"need-input-finalize",
	};
	size_t cases = 0u;
	for (size_t index = 0; index < ARRAY_COUNT(profiles); ++index) {
		MatrixFixture fixture = {
			names[index],
			profiles[index],
			AIUS_SD_PHASE_UPLOAD_INPUT,
			AIUS_SD_ACTION_NEED_INPUT,
			MATRIX_CLEAN_PLAIN,
			false,
			AIUS_SD_EXISTS_NONE};
		MatrixState state;
		matrix_build_happy(&state, &fixture);
		scenario = names[index];
		CHECK(state.job.outstanding_action == AIUS_SD_ACTION_NONE,
		      "NEED_INPUT unexpectedly correlated");
		CHECK(state.job.requested == 1u, "NEED_INPUT request window mismatch");
		step_none(&state.job, AIUS_SD_ACTION_NONE);
		step_none(&state.job, AIUS_SD_ACTION_NONE);
		CHECK(state.job.phase == AIUS_SD_PHASE_UPLOAD_INPUT,
		      "empty NONE moved NEED_INPUT row");
		CHECK(state.job.requested == 1u,
		      "empty NONE changed NEED_INPUT request");
		++cases;

		MatrixState invalid = state;
		invalid.job.buffer = invalid.buffer;
		CHECK(invalid.job.next_action_id > 1u,
		      "NEED_INPUT fixture lacks completed correlation");
		const uint64_t duplicate_id = invalid.job.next_action_id - 1u;
		AiusSdAction action = step_action(
			&invalid.job,
			0u,
			ok_event(duplicate_id),
			AIUS_SD_ACTION_CLOSE);
		if (profiles[index] == MATRIX_REPLACE) {
			action = acknowledge_ok(
				&invalid.job, &action, AIUS_SD_ACTION_EXISTS);
			action = remove_disposable_temp(
				&invalid.job, action, true, AIUS_SD_ACTION_FAIL);
		} else {
			action =
				acknowledge_ok(&invalid.job, &action, AIUS_SD_ACTION_FAIL);
		}
		CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
		      "NEED_INPUT no-outstanding event fault mismatch");
		++cases;

		MatrixState supplied = state;
		supplied.job.buffer = supplied.buffer;
		const uint8_t byte = 0x41u;
		CHECK(aius_sd_job_supply(&supplied.job, &byte, 1u),
		      "valid no-outstanding supply rejected");
		CHECK(supplied.job.outstanding_action == AIUS_SD_ACTION_NONE,
		      "supply manufactured outstanding authority");
		action = step_none(&supplied.job, AIUS_SD_ACTION_WRITE);
		CHECK(action.offset == 0u && action.length == 1u,
		      "supplied no-outstanding WRITE mismatch");
		++cases;
	}

	scenario = "idle no-outstanding";
	MatrixState idle;
	aius_sd_job_init(&idle.job, idle.buffer, sizeof(idle.buffer));
	step_none(&idle.job, AIUS_SD_ACTION_NONE);
	CHECK(idle.job.state == AIUS_SD_IDLE,
	      "IDLE no-outstanding step mutated state");
	++cases;
	return cases;
}

static size_t test_durable_upload_cancellation_modes(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t data[] = {1u, 2u};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "append partial cancellation preserves target";
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.total = sizeof(data);
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)),
	      "append cancellation supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_WRITE);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "partial append cancellation fault mismatch");

	scenario = "append zero-byte cancellation preserves target";
	request.total = 0u;
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "zero append cancellation fault mismatch");

	scenario = "finalize cancellation before append preserves part";
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.total = 1u;
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	CHECK(job.phase == AIUS_SD_PHASE_PART_SIZE_OPEN,
	      "finalize pre-append fixture mismatch");
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "finalize pre-append cancellation mismatch");

	scenario = "finalize partial cancellation preserves part";
	request.total = sizeof(data);
	action = begin_upload(
		&job, &request, buffer, sizeof(buffer), AIUS_SD_ACTION_OPEN_READ);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FILE_SIZE);
	action = step_action(
		&job, 0u, size_event(action.action_id, 0u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_APPEND);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)),
	      "finalize cancellation supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_WRITE);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "partial finalize cancellation mismatch");

	scenario = "finalize backup in-flight cancellation rolls back";
	MatrixFixture backup_fixture = {
		"finalize-backup-in-flight",
		MATRIX_FINALIZE,
		AIUS_SD_PHASE_INSTALL_BACKUP,
		AIUS_SD_ACTION_RENAME,
		MATRIX_CLEAN_PLAIN,
		true,
		AIUS_SD_EXISTS_NONE};
	MatrixState state;
	matrix_build_happy(&state, &backup_fixture);
	aius_sd_job_cancel(&state.job);
	action = step_action(&state.job,
	                     0u,
	                     ok_event(state.action.action_id),
	                     AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, state.job.backup_path) == 0 &&
		      strcmp(action.path_to, state.job.target_path) == 0,
	      "finalize cancellation rollback paths mismatch");
	action = acknowledge_ok(&state.job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_CANCELLED,
	      "finalize backup cancellation mismatch");
	return 5u;
}

static void test_install_target_existence_fault_cleanup(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = 0u;

	scenario = "install-target existence fault cleans disposable temp";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(&job, &action, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	CHECK(job.existence_purpose == AIUS_SD_EXISTS_INSTALL_TARGET,
	      "install-target fault fixture purpose mismatch");
	action = step_action(
		&job, 0u, fault_event(action.action_id, 65u), AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(
		&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_INSTALL, 65u);
}

/* fault_phase: where the authoritative fault landed, latched while it is still
 * current.
 *
 * The reason it exists is the last case here. A failed upload reaches its
 * terminal state through the cleanup arm, so by the time anyone reads the
 * result `phase` is TERMINAL for every failure alike -- a card that refused the
 * write and a card that refused the install rename are indistinguishable, and
 * both surface to the client as one 500. fault_phase keeps them apart. */
static void test_fault_phase_attribution(void)
{
	uint8_t buffer[CONFIG_LIMIT];
	const uint8_t abc[] = {'a', 'b', 'c'};
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request;

	scenario = "fault_phase absent until a fault";
	request = request_for(AIUS_SD_JOB_PROBE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	CHECK(job.fault_phase == AIUS_SD_PHASE_NONE,
	      "fresh job carried a fault phase");
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	action = step_action(&job,
	                     0u,
	                     bool_event(action.action_id, true),
	                     AIUS_SD_ACTION_COMPLETE);
	CHECK(job.fault_phase == AIUS_SD_PHASE_NONE,
	      "successful probe recorded a fault phase");

	scenario = "fault_phase BEGIN";
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 20u), AIUS_SD_ACTION_FAIL);
	CHECK(job.fault_phase == AIUS_SD_PHASE_BEGIN,
	      "mount failure phase mismatch");
	CHECK(job.phase == AIUS_SD_PHASE_TERMINAL,
	      "mount failure did not reach terminal");
	CHECK(job.last_driver_fault == 20u,
	      "mount failure lost its driver fault");

	scenario = "fault_phase cleared by restart";
	start_valid(&job, buffer, sizeof(buffer), &request);
	CHECK(job.fault_phase == AIUS_SD_PHASE_NONE,
	      "restart kept the previous job's fault phase");

	scenario = "fault_phase UPLOAD_WRITE survives cleanup";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = sizeof(abc);
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, abc, sizeof(abc)),
	      "write-fault fixture supply rejected");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	/* The card refusing the write, which is the alternative hypothesis for
	 * the intermittent 500 the probe race is also a candidate for. */
	action = step_action(&job,
	                     0u,
	                     fault_event(action.action_id, 31u),
	                     AIUS_SD_ACTION_CLOSE);
	CHECK(job.fault_phase == AIUS_SD_PHASE_UPLOAD_WRITE,
	      "write fault phase mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 31u);
	CHECK(job.phase == AIUS_SD_PHASE_TERMINAL,
	      "write fault did not reach terminal");
	CHECK(job.fault_phase == AIUS_SD_PHASE_UPLOAD_WRITE,
	      "cleanup overwrote the write fault phase");

	scenario = "fault_phase UPLOAD_WRITE for a short write";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(
		&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, abc, sizeof(abc)),
	      "short-write fixture supply rejected");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(&job,
	                     0u,
	                     bytes_event(action.action_id, 0u),
	                     AIUS_SD_ACTION_CLOSE);
	CHECK(job.fault_phase == AIUS_SD_PHASE_UPLOAD_WRITE,
	      "short write fault phase mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = remove_disposable_temp(&job, action, true, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_SHORT_IO, 0u);
	CHECK(job.fault_phase == AIUS_SD_PHASE_UPLOAD_WRITE,
	      "short write fault phase drifted at terminal");

	scenario = "fault_phase INSTALL_RENAME";
	request.total = 0u;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_BEGIN);
	action = acknowledge_write_begin(&job, &action, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = install_no_target(&job, action);
	action = step_action(&job,
	                     0u,
	                     fault_event(action.action_id, 41u),
	                     AIUS_SD_ACTION_FAIL);
	CHECK(job.fault_phase == AIUS_SD_PHASE_INSTALL_RENAME,
	      "install rename fault phase mismatch");
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_INSTALL, 41u);
	CHECK(job.fault_phase == AIUS_SD_PHASE_INSTALL_RENAME,
	      "install rename fault phase drifted at terminal");

	scenario = "fault_phase CONFIG_OPEN";
	request = request_for(AIUS_SD_JOB_CONFIG_LOAD);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 51u), AIUS_SD_ACTION_FAIL);
	CHECK(job.fault_phase == AIUS_SD_PHASE_CONFIG_OPEN,
	      "config open fault phase mismatch");
}

int main(void)
{
	test_pointer_and_start_contract();
	test_probe_usage_delete();
	test_config_load();
	test_publish_buffer();
	test_download_and_refill();
	test_cache_fill();
	test_upload_modes();
	test_install_publication_and_authority();
	test_request_boundaries();
	test_require_existing_request_matrix();
	test_protocol_mismatches_and_supply();
	test_deadline_cancel_and_restart();
	test_basic_action_faults();
	test_post_commit_fault_precedence();
	test_action_id_reserve();
	test_malformed_event_table();
	test_unresolved_action_authority();
	test_complete_action_fault_matrix();
	test_partial_abort_and_commit_precedence();
	test_abort_install_failed_authority();
	test_post_commit_fault_matrix();
	test_post_commit_publish_buffer();
	test_complete_request_boundary_matrix();
	test_reserved_action_id_properties();
	test_exact_262_id_publication_reserve();
	test_backup_rename_fault_temp_cleanup();
	test_fault_phase_attribution();
	size_t matrix_trials = test_fixture_cross_product();
	size_t lifecycle_cases = test_existence_purpose_routes();
	lifecycle_cases += test_active_no_outstanding_rows();
	lifecycle_cases += test_durable_upload_cancellation_modes();
	CHECK(ARRAY_COUNT(matrix_fixtures) == 64u,
	      "phase-10 fixture registry count drifted");
	CHECK(matrix_trials == 1001u,
	      "phase-10 matrix trial count drifted");
	CHECK(lifecycle_cases == 27u,
	      "phase-10 lifecycle case count drifted");
	test_install_target_existence_fault_cleanup();
	printf("sd oracle: all tests passed (%zu fixtures, %zu matrix trials, "
	       "%zu lifecycle cases)\n",
	       ARRAY_COUNT(matrix_fixtures),
	       matrix_trials,
	       lifecycle_cases);
	return 0;
}
