#include "aius_sd.h"

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
	case AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE:
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
		request.publish_config = true;
		break;
	case AIUS_SD_JOB_DOWNLOAD:
		request.target_path = "/audio/file.bin";
		request.content_type = "application/octet-stream";
		break;
	case AIUS_SD_JOB_DELETE:
	case AIUS_SD_JOB_REPLAY_REFILL:
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
	                     bool_event(action.action_id, false),
	                     AIUS_SD_ACTION_COMPLETE);
	CHECK(action.kind == AIUS_SD_ACTION_COMPLETE, "probe did not complete");
	expect_result(&job, AIUS_SD_RESULT_PROBE, AIUS_SD_FAULT_NONE, 0u);
	CHECK(!job.result.media_present, "absent media became present");
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

	scenario = "delete";
	request = request_for(AIUS_SD_JOB_DELETE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	CHECK(job.phase == AIUS_SD_PHASE_DELETE_REMOVE, "delete phase mismatch");
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

static AiusSdAction accept_mkdirs(
	AiusSdJob *job,
	AiusSdAction action,
	const char *const *paths,
	size_t count,
	AiusSdActionKind next)
{
	for (size_t index = 0; index < count; ++index) {
		CHECK(action.kind == AIUS_SD_ACTION_MKDIR, "missing mkdir");
		CHECK(strcmp(action.path, paths[index]) == 0, "mkdir path/order mismatch");
		action = acknowledge_ok(
			job,
			&action,
			index + 1u == count ? next : AIUS_SD_ACTION_MKDIR);
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
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_MKDIR);
	if (request->install_mode == AIUS_SD_INSTALL_APPEND_PART)
		return accept_mkdirs(
			job, action, append_dirs, ARRAY_COUNT(append_dirs), next);
	return accept_mkdirs(
		job, action, replace_dirs, ARRAY_COUNT(replace_dirs), next);
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
	action = step_action(
		job, 0u, bool_event(action.action_id, false), AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, job->temp_path) == 0, "install source mismatch");
	CHECK(strcmp(action.path_to, job->target_path) == 0,
	      "install destination mismatch");
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	CHECK(strcmp(action.path, request.temp_path) == 0, "replace open path mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
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
	request.publish_config = true;
	request.content_type = "application/json";

	scenario = "replace existing publish";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, request.backup_path) == 0,
	      "old backup remove mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, request.target_path) == 0 &&
		      strcmp(action.path_to, request.backup_path) == 0,
	      "backup rename mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, request.temp_path) == 0 &&
		      strcmp(action.path_to, request.target_path) == 0,
	      "install rename mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	CHECK(job.installed, "install commit not recorded");
	CHECK(strcmp(action.path, request.backup_path) == 0,
	      "post-commit backup remove mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
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
	request.publish_config = false;
	request.content_type = NULL;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 43u), AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 44u), AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_ROLLBACK, "rollback failure fault mismatch");
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
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
	struct InvalidRequestCase invalid[12];
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
	request.publish_config = false;
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
	request.publish_config = true;
	request.total = 0u;
	request.content_type = "application/json";
	invalid[count++].request = request;
	request = upload_request(AIUS_SD_INSTALL_APPEND_PART);
	request.publish_config = true;
	request.content_type = "application/json";
	invalid[count++].request = request;
	request = request_for(AIUS_SD_JOB_DELETE);
	request.content_type = "unexpected";
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	CHECK(!aius_sd_job_supply(&job, data, 1u),
	      "out-of-state supply succeeded");
	step_action(&job, 0u, none_event(), AIUS_SD_ACTION_NONE);
	CHECK(job.outstanding_action_id == action.action_id,
	      "supply cleared open authority");
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, request.temp_path) == 0,
	      "replace temp cleanup missing");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "outstanding supply fault mismatch");

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
	case AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE:
	case AIUS_SD_ACTION_OPEN_APPEND:
		return AIUS_SD_FAULT_OPEN;
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
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
	request.publish_config = true;
	request.content_type = "application/json";

	scenario = "post-commit cleanup plus publish fault";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
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
	request.publish_config = false;
	request.content_type = NULL;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	aius_sd_job_cancel(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);
}

static void test_action_id_reserve(void)
{
	uint8_t buffer[8];
	AiusSdJob job;
	AiusSdAction action;
	AiusSdJobRequest request = request_for(AIUS_SD_JOB_PROBE);

	scenario = "action id exhaustion terminal restart";
	aius_sd_job_init(&job, buffer, sizeof(buffer));
	job.next_action_id = UINT64_MAX - 4u;
	aius_sd_job_start(&job, &request, 0u, 0u);
	action = begin_job(&job);
	CHECK(action.action_id == UINT64_MAX - 4u, "near-limit id mismatch");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MEDIA_PRESENT);
	CHECK(action.action_id == UINT64_MAX - 3u, "near-limit second id mismatch");
	step_action(&job,
	            0u,
	            bool_event(action.action_id, true),
	            AIUS_SD_ACTION_COMPLETE);
	step_none(&job, AIUS_SD_ACTION_NONE);
	aius_sd_job_start(&job, &request, 0u, 0u);
	expect_terminal(&job,
	                AIUS_SD_ACTION_FAIL,
	                AIUS_SD_RESULT_FAULT,
	                AIUS_SD_FAULT_OVERFLOW,
	                0u);
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
	                    AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		job, action, data, length, length, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_RENAME);
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	CHECK(action.fault == AIUS_SD_FAULT_PROTOCOL,
	      "unresolved OPEN terminal mismatch");

	scenario = "unresolved WRITE authority";
	action = reach_replace_open(&job, buffer, sizeof(buffer), sizeof(data));
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)), "write supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	step_action(
		&job, 0u, bytes_event(action.action_id + 1u, 1u), AIUS_SD_ACTION_NONE);
	action = step_action(&job,
	                     0u,
	                     bytes_event(action.action_id, 1u),
	                     AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == 1u, "resolved WRITE progress not retained");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	finish_protocol_after_close(&job, action);

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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_REMOVE);
	finish_protocol_after_close(&job, action);

	scenario = "unresolved REMOVE authority";
	request = request_for(AIUS_SD_JOB_DELETE);
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_RENAME);
	CHECK(strcmp(action.path, request.backup_path) == 0,
	      "protocol cleanup did not roll backup back");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, request.temp_path) == 0,
	      "protocol cleanup did not remove temp");
	finish_protocol_after_close(&job, action);

	scenario = "unresolved install RENAME success wins";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	step_action(
		&job, 0u, ok_event(action.action_id + 1u), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_REMOVE);
	CHECK(job.installed, "resolved install success did not commit");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MKDIR);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 60u), AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, request.temp_path) == 0,
	      "MKDIR fault omitted disposable cleanup");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 60u);

	scenario = "OPEN_WRITE_TRUNCATE fault";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 61u), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_OPEN, 61u);

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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)), "write-fault supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 63u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
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

	scenario = "EXISTS fault";
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.total = 0u;
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 65u), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 65u);

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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 69u), AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, request.temp_path) == 0,
	      "old-backup fault omitted temp cleanup");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_IO, 69u);

	scenario = "backup rename fault preserves recovery files";
	action = begin_upload(&job,
	                      &request,
	                      buffer,
	                      sizeof(buffer),
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		&job, action, data, sizeof(data), sizeof(data), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		&job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_RENAME);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 70u), AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_INSTALL, 70u);

	scenario = "cleanup close fault precedence and temp authority";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_CLOSE);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 71u), AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, job.temp_path) == 0,
	      "close fault lost temp cleanup authority");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
	expect_result(&job, AIUS_SD_RESULT_FAULT, AIUS_SD_FAULT_CLEANUP, 71u);

	scenario = "cleanup temp removal fault";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	action = step_action(
		&job, 0u, fault_event(action.action_id, 72u), AIUS_SD_ACTION_REMOVE);
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)), "partial cancel supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_WRITE);
	aius_sd_job_cancel(&job);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	CHECK(job.transferred == sizeof(data), "partial cancel lost write progress");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_NEED_INPUT);
	CHECK(aius_sd_job_supply(&job, data, sizeof(data)),
	      "partial deadline supply failed");
	action = step_none(&job, AIUS_SD_ACTION_WRITE);
	action = step_action(
		&job, 0u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_WRITE);
	job.deadline_ms = 5u;
	step_action(&job, 5u, none_event(), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 5u, bytes_event(action.action_id, 1u), AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
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
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_REMOVE);
	CHECK(job.installed, "in-flight install success did not commit");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
	expect_result(&job, AIUS_SD_RESULT_BYTES, AIUS_SD_FAULT_NONE, 0u);

	scenario = "deadline in-flight install success commits";
	action = reach_existing_install(&job, buffer, data, sizeof(data));
	job.deadline_ms = 5u;
	step_action(&job, 5u, none_event(), AIUS_SD_ACTION_NONE);
	action = step_action(
		&job, 5u, ok_event(action.action_id), AIUS_SD_ACTION_REMOVE);
	CHECK(job.installed, "deadline install success did not commit");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_COMPLETE);
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
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		job, action, data, length, length, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	return acknowledge_ok(job, &action, AIUS_SD_ACTION_RENAME);
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	CHECK(strcmp(action.path, job.temp_path) == 0,
	      "cancel rollback omitted disposable temp");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
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
	request.publish_config = true;
	request.content_type = "application/json";
	action = begin_upload(job,
	                      &request,
	                      buffer,
	                      CONFIG_LIMIT,
	                      AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_NEED_INPUT);
	action = supply_and_write(
		job, action, data, length, length, AIUS_SD_ACTION_CLOSE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_EXISTS);
	action = step_action(
		job, 0u, bool_event(action.action_id, true), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_RENAME);
	action = acknowledge_ok(job, &action, AIUS_SD_ACTION_REMOVE);
	return acknowledge_ok(job, &action, AIUS_SD_ACTION_OPEN_READ);
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
	request.publish_config = true;
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
	request.publish_config = true;
	fields[field_count++] = (struct InvalidFieldCase){"download publish", request};
	request = upload_request(AIUS_SD_INSTALL_REPLACE);
	request.content_type = "x";
	fields[field_count++] = (struct InvalidFieldCase){"replace stray content", request};
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.content_type = "x";
	fields[field_count++] = (struct InvalidFieldCase){"final stray content", request};
	request = upload_request(AIUS_SD_INSTALL_FINALIZE_PART);
	request.publish_config = true;
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
		&job, 0u, ok_event(action.action_id), AIUS_SD_ACTION_REMOVE);
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	CHECK(action.action_id == UINT64_MAX - 1u,
	      "reserved temp-cleanup id mismatch");
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
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_REMOVE);
	CHECK(action.action_id == UINT64_MAX - 1u,
	      "reserved rollback cleanup id mismatch");
	acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);

	scenario = "consume UINT64_MAX and saturate lifetime";
	action = reach_replace_open(&job, buffer, sizeof(buffer), 1u);
	job.next_action_id = UINT64_MAX;
	action = step_action(
		&job, 0u, fault_event(action.action_id, 101u), AIUS_SD_ACTION_REMOVE);
	CHECK(action.action_id == UINT64_MAX, "cleanup did not consume UINT64_MAX");
	CHECK(job.action_ids_exhausted, "UINT64_MAX did not exhaust ids");
	CHECK(job.next_action_id == UINT64_MAX, "next id did not saturate");
	action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_FAIL);
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
	MATRIX_CLEAN_COMMIT_REMOVE_BACKUP,
	MATRIX_CLEAN_EXISTING_INSTALL,
	MATRIX_CLEAN_CANCEL_CLOSE_REMOVE,
	MATRIX_CLEAN_CANCEL_CLOSE_DURABLE,
	MATRIX_CLEAN_EXISTING_OPEN_REMOVE
} MatrixCleanup;

typedef struct {
	const char *name;
	MatrixProfile profile;
	AiusSdPhase phase;
	AiusSdActionKind action;
	MatrixCleanup cleanup;
	bool target_exists;
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
		request.publish_config = true;
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
		    state->action.kind == fixture->action)
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
			                   ? AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE
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
	if (fixture->phase == AIUS_SD_PHASE_CLEANUP_TEMP) {
		precursor = *fixture;
		precursor.phase = AIUS_SD_PHASE_UPLOAD_OPEN;
		precursor.action = AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE;
		matrix_build_happy(state, &precursor);
		state->action = step_action(
			&state->job,
			0u,
			fault_event(state->action.action_id, 121u),
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
	"delete-remove",
	"install-exists",
	"install-remove-old-backup",
	"install-backup-rename",
	"install-rename-no-backup",
	"install-rename-with-backup",
	"install-remove-backup",
	"publish-open",
	"publish-size",
	"publish-read",
	"publish-close",
	"publish-config",
	"rollback-rename",
	"cleanup-close-replace",
	"cleanup-close-append",
	"cleanup-temp-replace",
};

#define MATRIX_FIXTURE(name_, profile_, phase_, action_, cleanup_, exists_) \
	{(name_), (profile_), (phase_), (action_), (cleanup_), (exists_)}

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
	               AIUS_SD_ACTION_OPEN_WRITE_TRUNCATE,
	               MATRIX_CLEAN_CLOSE_REMOVE_TEMP, false),
	MATRIX_FIXTURE("upload-open-append", MATRIX_APPEND,
	               AIUS_SD_PHASE_UPLOAD_OPEN, AIUS_SD_ACTION_OPEN_APPEND,
	               MATRIX_CLEAN_CLOSE, false),
	MATRIX_FIXTURE("upload-open-finalize", MATRIX_FINALIZE,
	               AIUS_SD_PHASE_UPLOAD_OPEN, AIUS_SD_ACTION_OPEN_APPEND,
	               MATRIX_CLEAN_CLOSE, false),
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
	MATRIX_FIXTURE("delete-remove", MATRIX_DELETE,
	               AIUS_SD_PHASE_DELETE_REMOVE, AIUS_SD_ACTION_REMOVE,
	               MATRIX_CLEAN_PLAIN, false),
	MATRIX_FIXTURE("install-exists", MATRIX_REPLACE,
	               AIUS_SD_PHASE_INSTALL_EXISTS, AIUS_SD_ACTION_EXISTS,
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
	               MATRIX_CLEAN_COMMIT_REMOVE_BACKUP, true),
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
	MATRIX_FIXTURE("cleanup-temp-replace", MATRIX_REPLACE,
	               AIUS_SD_PHASE_CLEANUP_TEMP, AIUS_SD_ACTION_REMOVE,
	               MATRIX_CLEAN_EXISTING_OPEN_REMOVE, false),
};

#undef MATRIX_FIXTURE

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
	case MATRIX_CLEAN_COMMIT_REMOVE_BACKUP:
	case MATRIX_CLEAN_CANCEL_CLOSE_REMOVE:
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
		CHECK(action.kind == AIUS_SD_ACTION_REMOVE,
		      "matrix expected temp removal");
		CHECK(strcmp(action.path, state->job.temp_path) == 0,
		      "matrix cleanup removed wrong temp");
		action = acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_CLOSE_REMOVE_TEMP:
		CHECK(action.kind == AIUS_SD_ACTION_CLOSE,
		      "matrix expected close before temp cleanup");
		action =
			acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_REMOVE);
		CHECK(strcmp(action.path, state->job.temp_path) == 0,
		      "matrix close cleanup removed wrong temp");
		action = acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_ROLLBACK_REMOVE_TEMP:
		CHECK(action.kind == AIUS_SD_ACTION_RENAME,
		      "matrix expected rollback rename");
		CHECK(strcmp(action.path, state->job.backup_path) == 0 &&
			      strcmp(action.path_to, state->job.target_path) == 0,
		      "matrix rollback paths mismatch");
		action =
			acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_REMOVE);
		CHECK(strcmp(action.path, state->job.temp_path) == 0,
		      "matrix rollback removed wrong temp");
		action = acknowledge_ok(&state->job, &action, AIUS_SD_ACTION_FAIL);
		break;
	case MATRIX_CLEAN_COMMIT_REMOVE_BACKUP:
		CHECK(action.kind == AIUS_SD_ACTION_REMOVE,
		      "matrix expected committed backup cleanup");
		CHECK(strcmp(action.path, state->job.backup_path) == 0,
		      "matrix committed cleanup removed wrong backup");
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
			false};
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
		AiusSdAction action = step_action(
			&invalid.job, 0u, ok_event(1u), AIUS_SD_ACTION_CLOSE);
		if (profiles[index] == MATRIX_REPLACE) {
			action = acknowledge_ok(
				&invalid.job, &action, AIUS_SD_ACTION_REMOVE);
			CHECK(strcmp(action.path, invalid.job.temp_path) == 0,
			      "replace NEED_INPUT protocol removed wrong temp");
		}
		action =
			acknowledge_ok(&invalid.job, &action, AIUS_SD_ACTION_FAIL);
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
	start_valid(&job, buffer, sizeof(buffer), &request);
	action = begin_job(&job);
	while (true) {
		action = acknowledge_ok(&job, &action, AIUS_SD_ACTION_MKDIR);
		if (strcmp(action.path, "/backup/dir") == 0) {
			action =
				acknowledge_ok(&job, &action, AIUS_SD_ACTION_OPEN_READ);
			break;
		}
	}
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
		true};
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

int main(void)
{
	test_pointer_and_start_contract();
	test_probe_usage_delete();
	test_config_load();
	test_download_and_refill();
	test_upload_modes();
	test_install_publication_and_authority();
	test_request_boundaries();
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
	test_complete_request_boundary_matrix();
	test_reserved_action_id_properties();
	size_t matrix_trials = test_fixture_cross_product();
	size_t lifecycle_cases = test_active_no_outstanding_rows();
	lifecycle_cases += test_durable_upload_cancellation_modes();
	printf("sd oracle: all tests passed (%zu fixtures, %zu matrix trials, "
	       "%zu lifecycle cases)\n",
	       ARRAY_COUNT(matrix_fixtures),
	       matrix_trials,
	       lifecycle_cases);
	return 0;
}
