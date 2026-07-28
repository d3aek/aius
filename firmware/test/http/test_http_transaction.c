#include "aius_http_transaction.h"
#include "aius_config.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define RUN_TEST(test) do { \
	fprintf(stderr, "http oracle: %s\n", #test); \
	test(); \
} while (0)

typedef struct {
	AiusHttpTransaction transaction;
	uint8_t request[AIUS_HTTP_MAX_REQUEST];
	uint8_t response[AIUS_HTTP_MAX_RESPONSE];
	uint32_t now;
	AiusHttpAction action;
	unsigned dispatches;
	unsigned closes;
	unsigned cancels;
} Fixture;

static bool contains_bytes(const uint8_t *data, size_t data_length,
	                       const char *needle)
{
	size_t needle_length = strlen(needle);
	if (needle_length > data_length)
		return false;
	for (size_t i = 0; i <= data_length - needle_length; ++i) {
		if (memcmp(data + i, needle, needle_length) == 0)
			return true;
	}
	return false;
}

static AiusHttpEvent no_event(void)
{
	AiusHttpEvent event;
	memset(&event, 0, sizeof(event));
	event.kind = AIUS_HTTP_EVENT_NONE;
	return event;
}

static void assert_none_action(const AiusHttpAction *action)
{
	if (action->kind != AIUS_HTTP_ACTION_NONE)
		fprintf(stderr, "expected NONE action, actual kind=%d id=%llu\n",
		        (int)action->kind, (unsigned long long)action->action_id);
	assert(action->kind == AIUS_HTTP_ACTION_NONE);
	assert(action->action_id == 0);
	assert(action->read_buffer == NULL);
	assert(action->data == NULL);
	assert(action->length == 0);
	assert(action->request == NULL);
	assert(action->sd_request == NULL);
	assert(action->sd_operation == AIUS_HTTP_SD_OP_NONE);
	assert(action->fault == 0);
}

static void assert_action_shape(const AiusHttpAction *action)
{
	if (action->kind == AIUS_HTTP_ACTION_NONE) {
		assert_none_action(action);
		return;
	}
	assert(action->action_id != 0);
	switch (action->kind) {
	case AIUS_HTTP_ACTION_READ:
		assert(action->read_buffer != NULL);
		assert(action->data == NULL);
		assert(action->length > 0);
		assert(action->length <= AIUS_HTTP_RX_QUOTA);
		assert(action->request == NULL);
		assert(action->sd_request == NULL);
		assert(action->sd_operation == AIUS_HTTP_SD_OP_NONE);
		assert(action->fault == 0);
		break;
	case AIUS_HTTP_ACTION_WRITE:
		assert(action->read_buffer == NULL);
		assert(action->data != NULL);
		assert(action->length > 0);
		assert(action->length <= AIUS_HTTP_TX_QUOTA);
		assert(action->request == NULL);
		assert(action->sd_request == NULL);
		assert(action->sd_operation == AIUS_HTTP_SD_OP_NONE);
		assert(action->fault == 0);
		break;
	case AIUS_HTTP_ACTION_DISPATCH:
		assert(action->read_buffer == NULL);
		assert(action->data == NULL);
		assert(action->length == 0);
		assert(action->request != NULL);
		assert(action->sd_request == NULL);
		assert(action->sd_operation == AIUS_HTTP_SD_OP_NONE);
		assert(action->fault == 0);
		break;
	case AIUS_HTTP_ACTION_SD_START:
		assert(action->read_buffer == NULL);
		assert(action->data == NULL);
		assert(action->length == 0);
		assert(action->request == NULL);
		assert(action->sd_request != NULL);
		assert(action->sd_operation == AIUS_HTTP_SD_OP_NONE);
		assert(action->fault == 0);
		break;
	case AIUS_HTTP_ACTION_SD_STEP:
		assert(action->read_buffer == NULL);
		assert(action->request == NULL);
		assert(action->sd_request == NULL);
		assert(action->sd_operation != AIUS_HTTP_SD_OP_NONE);
		if (action->sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD) {
			assert(action->data != NULL);
			assert(action->length > 0);
			assert(action->fault == 0);
		} else {
			assert(action->data == NULL);
			assert(action->length == 0);
			if (action->sd_operation == AIUS_HTTP_SD_OP_FAIL_DOWNLOAD)
				assert(action->fault == AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT);
			else
				assert(action->fault == 0);
		}
		break;
	case AIUS_HTTP_ACTION_ACCEPT:
	case AIUS_HTTP_ACTION_SD_CANCEL:
	case AIUS_HTTP_ACTION_CLOSE:
		assert(action->read_buffer == NULL);
		assert(action->data == NULL);
		assert(action->length == 0);
		assert(action->request == NULL);
		assert(action->sd_request == NULL);
		assert(action->sd_operation == AIUS_HTTP_SD_OP_NONE);
		assert(action->fault == 0);
		break;
	case AIUS_HTTP_ACTION_NONE:
		assert(0);
	}
}

static AiusHttpAction call_step(Fixture *fixture, uint32_t now, AiusHttpEvent event)
{
	AiusHttpAction action;
	memset(&action, 0xa5, sizeof(action));
	aius_http_transaction_step(&fixture->transaction, now, event, &action);
	assert_action_shape(&action);
	if (action.kind != AIUS_HTTP_ACTION_NONE) {
		assert(action.action_id > fixture->action.action_id ||
		       fixture->action.kind == AIUS_HTTP_ACTION_NONE);
	}
	if (action.kind == AIUS_HTTP_ACTION_DISPATCH)
		fixture->dispatches++;
	if (action.kind == AIUS_HTTP_ACTION_CLOSE)
		fixture->closes++;
	if (action.kind == AIUS_HTTP_ACTION_SD_CANCEL)
		fixture->cancels++;
	fixture->now = now;
	fixture->action = action;
	return action;
}

static AiusHttpEvent io_result(const AiusHttpAction *action,
	                           AiusIoResult result, size_t bytes, uint32_t fault)
{
	AiusHttpEvent event = no_event();
	event.kind = AIUS_HTTP_EVENT_IO_RESULT;
	event.action_id = action->action_id;
	event.completed_action = action->kind;
	event.io.result = result;
	event.io.bytes = bytes;
	event.io.fault = fault;
	return event;
}

static AiusHttpEvent application_result(
	const AiusHttpAction *action, const AiusHttpApplicationResponse *response)
{
	AiusHttpEvent event = no_event();
	event.kind = AIUS_HTTP_EVENT_APPLICATION_RESPONSE;
	event.action_id = action->action_id;
	event.completed_action = AIUS_HTTP_ACTION_DISPATCH;
	event.application_response = response;
	return event;
}

static AiusHttpEvent sd_result(const AiusHttpAction *action,
	                           AiusHttpSdEventKind kind)
{
	AiusHttpEvent event = no_event();
	event.kind = AIUS_HTTP_EVENT_SD_RESULT;
	event.action_id = action->action_id;
	event.completed_action = action->kind;
	event.sd_kind = kind;
	return event;
}

static AiusHttpEvent peer_fault(uint32_t fault)
{
	AiusHttpEvent event = no_event();
	event.kind = AIUS_HTTP_EVENT_PEER_FAULT;
	event.peer_fault = fault;
	return event;
}

static void init_fixture_at(Fixture *fixture, size_t request_capacity,
	                        size_t response_capacity, uint32_t now)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->now = now;
	aius_http_transaction_init(&fixture->transaction, fixture->request,
	                           request_capacity, fixture->response,
	                           response_capacity, now);
	assert(fixture->transaction.valid);
	assert(fixture->transaction.state == AIUS_HTTP_TX_IDLE);
}

static void init_fixture(Fixture *fixture)
{
	init_fixture_at(fixture, sizeof(fixture->request), sizeof(fixture->response), 0);
}

static AiusHttpAction accept_client(Fixture *fixture)
{
	AiusHttpAction action = call_step(fixture, fixture->now, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_ACCEPT);
	action = call_step(fixture, fixture->now,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_READ);
	assert(fixture->transaction.client_active);
	return action;
}

static AiusHttpAction feed_request(Fixture *fixture, const char *raw)
{
	AiusHttpAction action = accept_client(fixture);
	size_t offset = 0;
	size_t length = strlen(raw);
	while (offset < length) {
		assert(action.kind == AIUS_HTTP_ACTION_READ);
		size_t count = length - offset;
		if (count > action.length)
			count = action.length;
		memcpy(action.read_buffer, raw + offset, count);
		offset += count;
		action = call_step(fixture, fixture->now,
		                   io_result(&action, AIUS_IO_PROGRESS, count, 0));
	}
	return action;
}

static AiusHttpApplicationResponse immediate(unsigned status,
	                                         const char *content_type,
	                                         const uint8_t *body,
	                                         size_t body_length)
{
	AiusHttpApplicationResponse response;
	memset(&response, 0, sizeof(response));
	response.kind = AIUS_HTTP_APP_IMMEDIATE;
	response.status = status;
	response.content_type = content_type;
	response.body = body;
	response.body_length = body_length;
	return response;
}

static AiusHttpApplicationResponse sd_job(AiusSdJobKind kind,
	                                      unsigned status,
	                                      const char *response_type,
	                                      const uint8_t *body,
	                                      size_t body_length,
	                                      uint64_t total)
{
	AiusHttpApplicationResponse response;
	memset(&response, 0, sizeof(response));
	response.kind = AIUS_HTTP_APP_SD_JOB;
	response.status = status;
	response.content_type = response_type;
	response.body = body;
	response.body_length = body_length;
	response.sd_job.kind = kind;
	response.sd_job.install_mode = AIUS_SD_INSTALL_REPLACE;
	response.sd_job.target_path = "/target";
	if (kind == AIUS_SD_JOB_UPLOAD) {
		response.sd_job.temp_path = "/temp";
		response.sd_job.backup_path = "/backup";
	}
	response.sd_job.content_type =
		kind == AIUS_SD_JOB_DOWNLOAD ? response_type : NULL;
	response.sd_job.total = total;
	return response;
}

static AiusHttpAction start_upload_read(Fixture *fixture);
static AiusHttpAction start_other_advance(Fixture *fixture);

static AiusHttpAction finish_all_writes(Fixture *fixture, AiusHttpAction action)
{
	while (action.kind == AIUS_HTTP_ACTION_WRITE)
		action = call_step(fixture, fixture->now,
		                   io_result(&action, AIUS_IO_PROGRESS, action.length, 0));
	return action;
}

static bool points_into_response(const Fixture *fixture, const uint8_t *data)
{
	return data >= fixture->response &&
	       data < fixture->response + fixture->transaction.response_length;
}

static AiusHttpAction finish_response_headers(Fixture *fixture,
	                                          AiusHttpAction action)
{
	while (action.kind == AIUS_HTTP_ACTION_WRITE &&
	       points_into_response(fixture, action.data)) {
		action = call_step(fixture, fixture->now,
		                   io_result(&action, AIUS_IO_PROGRESS, action.length, 0));
	}
	return action;
}

static void finish_close(Fixture *fixture, AiusHttpAction action)
{
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	action = call_step(fixture, fixture->now,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_NONE);
	assert(fixture->transaction.state == AIUS_HTTP_TX_IDLE);
}

static void test_invalid_initialization_and_null_action(void)
{
	AiusHttpTransaction transaction;
	uint8_t request[32];
	uint8_t response[32];
	AiusHttpAction action;
	AiusHttpEvent event = no_event();

	memset(&transaction, 0xa5, sizeof(transaction));
	aius_http_transaction_init(&transaction, NULL, sizeof(request),
	                           response, sizeof(response), 7);
	assert(!transaction.valid);
	assert(transaction.last_fault == AIUS_HTTP_FAULT_INVALID_INIT);
	memset(&action, 0xa5, sizeof(action));
	aius_http_transaction_step(&transaction, 8, event, &action);
	assert_none_action(&action);

	aius_http_transaction_init(&transaction, request, 0,
	                           response, sizeof(response), 7);
	assert(!transaction.valid);
	aius_http_transaction_init(&transaction, request, AIUS_HTTP_MAX_REQUEST + 1u,
	                           response, sizeof(response), 7);
	assert(!transaction.valid);
	aius_http_transaction_init(&transaction, request, sizeof(request),
	                           response, 0, 7);
	assert(!transaction.valid);
	aius_http_transaction_init(&transaction, response, 16,
	                           response + 8, 16, 7);
	assert(!transaction.valid);

	aius_http_transaction_init(NULL, request, sizeof(request),
	                           response, sizeof(response), 7);
	aius_http_transaction_step(NULL, 8, event, &action);

	aius_http_transaction_init(&transaction, request, sizeof(request),
	                           response, sizeof(response), 7);
	AiusHttpTransaction snapshot = transaction;
	event.kind = AIUS_HTTP_EVENT_PEER_FAULT;
	event.peer_fault = 99;
	aius_http_transaction_step(&transaction, UINT32_MAX, event, NULL);
	assert(memcmp(&transaction, &snapshot, sizeof(transaction)) == 0);
}

static void test_accept_validation_and_restart(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action = call_step(&fixture, 0, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_ACCEPT);
	uint64_t first_id = action.action_id;

	AiusHttpEvent malformed = io_result(&action, AIUS_IO_CLOSED, 0, 0);
	action = call_step(&fixture, 0, malformed);
	assert_none_action(&action);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
	assert(fixture.transaction.last_status == 0);
	assert(!fixture.transaction.client_active);
	assert(fixture.closes == 0);

	action = call_step(&fixture, 0, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_ACCEPT);
	assert(action.action_id > first_id);
	action = call_step(&fixture, 0,
	                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert_none_action(&action);
	action = call_step(&fixture, 0, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_ACCEPT);
	action = call_step(&fixture, 0,
	                   io_result(&action, AIUS_IO_FAULT, 0, 0x44));
	assert_none_action(&action);
	if (fixture.transaction.last_fault != AIUS_HTTP_FAULT_TRANSPORT ||
	    fixture.transaction.last_transport_fault != 0x44) {
		fprintf(stderr,
		        "ACCEPT/FAULT retention: expected fault=%d transport=0x44, "
		        "actual fault=%d transport=0x%x\n",
		        (int)AIUS_HTTP_FAULT_TRANSPORT,
		        (int)fixture.transaction.last_fault,
		        fixture.transaction.last_transport_fault);
	}
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_TRANSPORT);
	assert(fixture.transaction.last_transport_fault == 0x44);
	assert(fixture.closes == 0);
}

static void test_request_quota_timeout_and_wrap(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action = accept_client(&fixture);
	assert(action.length == AIUS_HTTP_RX_QUOTA);
	uint8_t *first_buffer = action.read_buffer;
	action = call_step(&fixture, 149,
	                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_READ);
	assert(action.read_buffer == first_buffer);
	assert(action.length == AIUS_HTTP_RX_QUOTA);
	action = call_step(&fixture, 150,
	                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_REQUEST_TIMEOUT);
	assert(fixture.transaction.last_status == 408);
	assert(contains_bytes(action.data, action.length, "request_timeout"));
	action = finish_all_writes(&fixture, action);
	finish_close(&fixture, action);

	init_fixture_at(&fixture, sizeof(fixture.request), sizeof(fixture.response),
	                UINT32_MAX - 50u);
	action = accept_client(&fixture);
	assert(fixture.transaction.deadline_ms == 99u);
	action = call_step(&fixture, 98,
	                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_READ);
	action = call_step(&fixture, 99, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_REQUEST_TIMEOUT);
}

static void test_dispatch_response_partial_write_close_restart(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action =
		feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
	assert(action.kind == AIUS_HTTP_ACTION_DISPATCH);
	assert(action.request->route == AIUS_HTTP_ROUTE_STATUS);
	assert(action.request->body_complete);
	assert(fixture.dispatches == 1);

	static const uint8_t body[700] = { 1 };
	AiusHttpApplicationResponse response =
		immediate(200, "application/octet-stream", body, sizeof(body));
	action = call_step(&fixture, 20, application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(action.length == AIUS_HTTP_TX_QUOTA);
	const uint8_t *first_data = action.data;
	size_t first_length = action.length;

	action = call_step(&fixture, 30,
	                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(action.data == first_data);
	assert(action.length == first_length);

	size_t accepted = first_length - 1;
	action = call_step(&fixture, 40,
	                   io_result(&action, AIUS_IO_PROGRESS, accepted, 0));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(action.data == first_data + accepted);
	size_t expected_remaining =
		fixture.transaction.response_length - fixture.transaction.response_offset;
	assert(action.length ==
	       (expected_remaining < AIUS_HTTP_TX_QUOTA
		        ? expected_remaining
		        : AIUS_HTTP_TX_QUOTA));
	action = finish_all_writes(&fixture, action);
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	assert(fixture.dispatches == 1);
	assert(fixture.closes == 1);

	AiusHttpEvent duplicate = application_result(&action, &response);
	duplicate.action_id = action.action_id;
	duplicate.completed_action = AIUS_HTTP_ACTION_DISPATCH;
	action = call_step(&fixture, 40, duplicate);
	assert(action.kind == AIUS_HTTP_ACTION_NONE);
	assert(fixture.closes == 1);
	assert(fixture.transaction.state == AIUS_HTTP_TX_IDLE);

	action = call_step(&fixture, 40, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_ACCEPT);
	action = call_step(&fixture, 40,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_READ);
	assert(!fixture.transaction.dispatch_emitted);
	assert(!fixture.transaction.close_emitted);
	assert(fixture.transaction.last_status == 0);
}

/* The epoch ordering the runtime's gated reset depends on, and the ordering that
 * made the ungated one wrong (aius-network-hardening.md open defect 12).
 *
 * POST /api/reboot answers with an ordinary AIUS_HTTP_APP_IMMEDIATE body, so
 * this walks that response one step at a time and asserts, at each step, what a
 * reset gated on "the epoch that dispatched has finished" would have seen. At the
 * dispatch step response_length is still 0: format_response() runs inside
 * accept_application(), which only executes when the NEXT step consumes the
 * retained application event. So a reset there loses the answer entirely, which
 * is exactly what the board did -- boot_generation advanced and the client timed
 * out.
 *
 * AIUS_HTTP_TX_IDLE is asserted absent at every earlier step and present only
 * after the last write was accepted AND the close completed. That is what makes
 * it a drained terminal state rather than a buffered one, and therefore the right
 * signal for the reset to wait on. A test that only checked the end state would
 * pass against the defect; the load-bearing assertions are the ones before the
 * writes. */
static void test_immediate_response_reaches_idle_only_after_write_and_close(void)
{
	Fixture fixture;
	static const uint8_t body[] = "{\"ok\":true,\"rebooting\":true}";
	init_fixture(&fixture);
	AiusHttpAction action =
		feed_request(&fixture,
		             "POST /api/reboot HTTP/1.0\r\nContent-Length: 0\r\n\r\n");
	assert(action.kind == AIUS_HTTP_ACTION_DISPATCH);
	assert(action.request->route == AIUS_HTTP_ROUTE_REBOOT);
	assert(fixture.transaction.state == AIUS_HTTP_TX_DISPATCH);
	/* The dispatching step. The application has been asked and has answered;
	 * nothing is formatted, nothing is buffered, nothing is written. */
	assert(fixture.transaction.state != AIUS_HTTP_TX_IDLE);
	assert(fixture.transaction.response_length == 0u);
	assert(fixture.transaction.response_offset == 0u);
	assert(!fixture.transaction.response_started);

	AiusHttpApplicationResponse response =
		immediate(200, "application/json", body, sizeof(body) - 1u);
	action = call_step(&fixture, fixture.now,
	                   application_result(&action, &response));
	/* One step later: formatting has happened and a first WRITE now exists.
	 * response_started is still false, because it is set by the write's RESULT,
	 * not by its emission -- "buffered" is not "transmitted". */
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.state == AIUS_HTTP_TX_SEND);
	assert(fixture.transaction.state != AIUS_HTTP_TX_IDLE);
	assert(fixture.transaction.response_length != 0u);
	assert(!fixture.transaction.response_started);

	action = finish_all_writes(&fixture, action);
	assert(fixture.transaction.response_started);
	assert(contains_bytes(fixture.response, fixture.transaction.response_length,
	                      "{\"ok\":true,\"rebooting\":true}"));
	/* Every byte is out and the transaction is STILL not drained: the close has
	 * not completed, so the socket has not been released. */
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	assert(fixture.transaction.state == AIUS_HTTP_TX_CLOSE);
	assert(fixture.transaction.state != AIUS_HTTP_TX_IDLE);

	action = call_step(&fixture, fixture.now,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_NONE);
	assert(fixture.transaction.state == AIUS_HTTP_TX_IDLE);
	assert(fixture.transaction.outstanding_action == AIUS_HTTP_ACTION_NONE);
	assert(fixture.dispatches == 1);
	assert(fixture.closes == 1);
}

static void test_response_timeout_progress_precedence(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action =
		feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
	static const uint8_t body[700] = { 2 };
	AiusHttpApplicationResponse response =
		immediate(200, "application/octet-stream", body, sizeof(body));
	action = call_step(&fixture, 10, application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.deadline_ms == 110);

	action = call_step(&fixture, 110,
	                   io_result(&action, AIUS_IO_PROGRESS, 1, 0));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.deadline_ms == 210);
	action = call_step(&fixture, 210,
	                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_RESPONSE_TIMEOUT);
	assert(fixture.transaction.last_status == 200);
}

static void test_protocol_payload_validation(void)
{
	for (unsigned mutation = 0; mutation < 9; ++mutation) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action = accept_client(&fixture);
		AiusHttpEvent event = io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0);
		switch (mutation) {
		case 0: event.action_id++; break;
		case 1: event.completed_action = AIUS_HTTP_ACTION_WRITE; break;
		case 2: event.io.bytes = 1; break;
		case 3: event.io.fault = 1; break;
		case 4: event.application_response = (const void *)(uintptr_t)1; break;
		case 5: event.sd_kind = AIUS_HTTP_SD_PROGRESS; break;
		case 6: event.sd_capacity = 1; break;
		case 7: event.sd_fault = AIUS_SD_FAULT_IO; break;
		case 8: event.peer_fault = 1; break;
		}
		action = call_step(&fixture, 0, event);
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.last_status == 400);
	}
}

static void test_application_validation_and_overflow(void)
{
	static const uint8_t body[] = "{}";
	for (unsigned invalid = 0; invalid < 6; ++invalid) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/json", body, sizeof(body) - 1u);
		switch (invalid) {
		case 0: response.status = 418; break;
		case 1: response.content_type = ""; break;
		case 2: response.body = NULL; break;
		case 3: response.sd_job.total = 1; break;
		case 4:
			response.kind = AIUS_HTTP_APP_SD_JOB;
			response.sd_job.kind = AIUS_SD_JOB_REPLAY_REFILL;
			response.sd_job.target_path = "/x";
			break;
		case 5: response.sd_job.require_existing = true; break;
		}
		action = call_step(&fixture, 0, application_result(&action, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.last_status == 400);
	}

	/* An SD-job acknowledgement body is copied into the response buffer or
	 * it is nothing: only AIUS_HTTP_APP_IMMEDIATE streams from the
	 * application's buffer, so this stays the deterministic 500. */
	Fixture fixture;
	init_fixture_at(&fixture, sizeof(fixture.request), 192, 0);
	AiusHttpAction action =
		feed_request(&fixture, "DELETE /sd/x HTTP/1.0\r\n\r\n");
	uint8_t huge[128];
	memset(huge, 'x', sizeof(huge));
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_DELETE, 200, "application/json", huge,
		       sizeof(huge), 0);
	action = call_step(&fixture, 0, application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_RESPONSE_CAPACITY);
	assert(fixture.transaction.last_status == 500);
	assert(contains_bytes(action.data, action.length, "response_too_large"));
	assert(!fixture.transaction.sd_started);
	assert(!fixture.transaction.response_streams_body);
}

static AiusHttpApplicationResponse upload_application(
	AiusSdInstallMode mode, uint64_t total, uint64_t initial_offset,
	bool publish)
{
	static const uint8_t body[] = "{}";
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_UPLOAD, 201, "application/json",
		       body, sizeof(body) - 1u, total);
	response.sd_job.install_mode = mode;
	response.sd_job.initial_offset = initial_offset;
	response.sd_job.publish = publish;
	response.sd_job.publish_capacity =
		publish ? AIUS_CONFIG_MAX_JSON_BYTES : 0u;
	response.sd_job.content_type =
		publish ? "application/json" : NULL;
	if (mode == AIUS_SD_INSTALL_APPEND_PART) {
		response.sd_job.temp_path = NULL;
		response.sd_job.backup_path = NULL;
	}
	return response;
}

static AiusHttpAction dispatch_upload_length(
	Fixture *fixture, uint64_t content_length)
{
	char request[160];
	int length = snprintf(
		request, sizeof(request),
		"PUT /sd/x HTTP/1.0\r\nContent-Length: %llu\r\n\r\n",
		(unsigned long long)content_length);
	assert(length > 0);
	assert((size_t)length < sizeof(request));
	AiusHttpAction action = feed_request(fixture, request);
	assert(action.kind == AIUS_HTTP_ACTION_DISPATCH);
	assert(action.request->content_length == content_length);
	return action;
}

static void test_upload_application_modes_and_publication_sizes(void)
{
	struct ModeCase {
		AiusSdInstallMode mode;
		uint64_t initial_offset;
	};
	static const struct ModeCase modes[] = {
		{ AIUS_SD_INSTALL_REPLACE, 0 },
		{ AIUS_SD_INSTALL_APPEND_PART, 7 },
		{ AIUS_SD_INSTALL_FINALIZE_PART, 7 },
	};
	size_t mode_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(modes); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch = dispatch_upload_length(&fixture, 1);
		AiusHttpApplicationResponse response =
			upload_application(modes[i].mode, 1,
			                   modes[i].initial_offset, false);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_SD_START);
		assert(action.sd_request->kind == AIUS_SD_JOB_UPLOAD);
		assert(action.sd_request->install_mode == modes[i].mode);
		assert(action.sd_request->total == 1);
		assert(action.sd_request->initial_offset ==
		       modes[i].initial_offset);
		assert(!action.sd_request->publish);
		assert(action.sd_request->content_type == NULL);
		if (modes[i].mode == AIUS_SD_INSTALL_APPEND_PART) {
			assert(action.sd_request->temp_path == NULL);
			assert(action.sd_request->backup_path == NULL);
		} else {
			assert(strcmp(action.sd_request->temp_path, "/temp") == 0);
			assert(strcmp(action.sd_request->backup_path, "/backup") == 0);
		}
		mode_trials++;
	}
	assert(mode_trials == 3);

	struct PublicationCase {
		AiusSdInstallMode mode;
		uint64_t initial_offset;
		uint64_t total;
		bool valid;
	};
	static const struct PublicationCase publication[] = {
		{ AIUS_SD_INSTALL_REPLACE, 0, 0, false },
		{ AIUS_SD_INSTALL_REPLACE, 0, 1, true },
		{ AIUS_SD_INSTALL_REPLACE, 0,
		  AIUS_CONFIG_MAX_JSON_BYTES, true },
		{ AIUS_SD_INSTALL_REPLACE, 0,
		  AIUS_CONFIG_MAX_JSON_BYTES + 1u, false },
		{ AIUS_SD_INSTALL_FINALIZE_PART, 0, 0, false },
		{ AIUS_SD_INSTALL_FINALIZE_PART, 0, 1, true },
		{ AIUS_SD_INSTALL_FINALIZE_PART,
		  AIUS_CONFIG_MAX_JSON_BYTES - 1u, 1, true },
		{ AIUS_SD_INSTALL_FINALIZE_PART,
		  AIUS_CONFIG_MAX_JSON_BYTES, 1, false },
		{ AIUS_SD_INSTALL_FINALIZE_PART, INT64_MAX, 1, false },
	};
	size_t publication_trials = 0;
	size_t publication_valid = 0;
	size_t publication_invalid = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(publication); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			dispatch_upload_length(&fixture, publication[i].total);
		AiusHttpApplicationResponse response = upload_application(
			publication[i].mode, publication[i].total,
			publication[i].initial_offset, true);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		if (publication[i].valid) {
			assert(action.kind == AIUS_HTTP_ACTION_SD_START);
			assert(action.sd_request->publish);
			assert(strcmp(action.sd_request->content_type,
			              "application/json") == 0);
			publication_valid++;
		} else {
			assert(action.kind == AIUS_HTTP_ACTION_WRITE);
			assert(fixture.transaction.last_fault ==
			       AIUS_HTTP_FAULT_PROTOCOL);
			assert(fixture.transaction.last_status == 400);
			publication_invalid++;
		}
		publication_trials++;
	}
	assert(publication_trials == 9);
	assert(publication_valid == 4);
	assert(publication_invalid == 5);

	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch = dispatch_upload_length(&fixture, 1);
		AiusHttpApplicationResponse response =
			upload_application(AIUS_SD_INSTALL_APPEND_PART, 1, 0, true);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
	}
}

static AiusHttpApplicationResponse control_application(AiusSdJobKind kind)
{
	static const uint8_t body[] = "{}";
	AiusHttpApplicationResponse response =
		sd_job(kind, 200, "application/json",
		       body, sizeof(body) - 1u, 0);
	response.sd_job.target_path = NULL;
	response.sd_job.temp_path = NULL;
	response.sd_job.backup_path = NULL;
	response.sd_job.content_type = NULL;
	if (kind == AIUS_SD_JOB_CONFIG_LOAD) {
		response.sd_job.target_path = "/config";
		response.sd_job.content_type = "application/json";
		response.sd_job.publish = true;
		response.sd_job.publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES;
	} else if (kind == AIUS_SD_JOB_DELETE) {
		response.sd_job.target_path = "/delete";
	}
	return response;
}

static void test_control_job_application_validation(void)
{
	static const AiusSdJobKind kinds[] = {
		AIUS_SD_JOB_PROBE,
		AIUS_SD_JOB_CONFIG_LOAD,
		AIUS_SD_JOB_USAGE_REFRESH,
		AIUS_SD_JOB_DELETE,
	};
	size_t valid_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(kinds); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture,
			             "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			control_application(kinds[i]);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_SD_START);
		assert(action.sd_request->kind == kinds[i]);
		assert(action.sd_request->install_mode ==
		       AIUS_SD_INSTALL_REPLACE);
		assert(action.sd_request->total == 0);
		assert(action.sd_request->initial_offset == 0);
		assert(action.sd_request->temp_path == NULL);
		assert(action.sd_request->backup_path == NULL);
		if (kinds[i] == AIUS_SD_JOB_CONFIG_LOAD) {
			assert(strcmp(action.sd_request->target_path,
			              "/config") == 0);
			assert(action.sd_request->publish);
			assert(strcmp(action.sd_request->content_type,
			              "application/json") == 0);
		} else if (kinds[i] == AIUS_SD_JOB_DELETE) {
			assert(strcmp(action.sd_request->target_path,
			              "/delete") == 0);
			assert(!action.sd_request->publish);
			assert(action.sd_request->content_type == NULL);
		} else {
			assert(action.sd_request->target_path == NULL);
			assert(!action.sd_request->publish);
			assert(action.sd_request->content_type == NULL);
		}
		valid_trials++;
	}
	assert(valid_trials == 4);

	enum InvalidControlField {
		INVALID_CONTROL_TOTAL,
		INVALID_CONTROL_INITIAL_OFFSET,
		INVALID_CONTROL_INSTALL_MODE,
		INVALID_CONTROL_TARGET,
		INVALID_CONTROL_TEMP,
		INVALID_CONTROL_BACKUP,
		INVALID_CONTROL_PUBLISH,
		INVALID_CONTROL_CONTENT_TYPE,
		INVALID_CONTROL_FIELD_COUNT
	};
	size_t invalid_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(kinds); ++i) {
		for (unsigned field = 0;
		     field < INVALID_CONTROL_FIELD_COUNT; ++field) {
			Fixture fixture;
			init_fixture(&fixture);
			AiusHttpAction dispatch =
				feed_request(&fixture,
				             "GET /api/status HTTP/1.0\r\n\r\n");
			AiusHttpApplicationResponse response =
				control_application(kinds[i]);
			switch ((enum InvalidControlField)field) {
			case INVALID_CONTROL_TOTAL:
				response.sd_job.total = 1;
				break;
			case INVALID_CONTROL_INITIAL_OFFSET:
				response.sd_job.initial_offset = 1;
				break;
			case INVALID_CONTROL_INSTALL_MODE:
				response.sd_job.install_mode =
					AIUS_SD_INSTALL_APPEND_PART;
				break;
			case INVALID_CONTROL_TARGET:
				response.sd_job.target_path =
					kinds[i] == AIUS_SD_JOB_CONFIG_LOAD ||
					kinds[i] == AIUS_SD_JOB_DELETE
						? NULL
						: "/forbidden";
				break;
			case INVALID_CONTROL_TEMP:
				response.sd_job.temp_path = "/forbidden";
				break;
			case INVALID_CONTROL_BACKUP:
				response.sd_job.backup_path = "/forbidden";
				break;
			case INVALID_CONTROL_PUBLISH:
				/* Keep publish/publish_capacity paired so the
				 * kind-specific rule is what rejects, not the
				 * generic pairing check. */
				response.sd_job.publish =
					kinds[i] != AIUS_SD_JOB_CONFIG_LOAD;
				response.sd_job.publish_capacity =
					response.sd_job.publish
						? AIUS_CONFIG_MAX_JSON_BYTES
						: 0u;
				break;
			case INVALID_CONTROL_CONTENT_TYPE:
				response.sd_job.content_type =
					kinds[i] == AIUS_SD_JOB_CONFIG_LOAD
						? NULL
						: "application/json";
				break;
			case INVALID_CONTROL_FIELD_COUNT:
				assert(0);
				break;
			}
			AiusHttpAction action = call_step(
				&fixture, 0,
				application_result(&dispatch, &response));
			if (action.kind != AIUS_HTTP_ACTION_WRITE ||
			    fixture.transaction.last_fault !=
				    AIUS_HTTP_FAULT_PROTOCOL) {
				fprintf(stderr,
				        "control validation mismatch: kind=%d "
				        "field=%u action=%d fault=%d status=%u\n",
				        (int)kinds[i], field, (int)action.kind,
				        (int)fixture.transaction.last_fault,
				        fixture.transaction.last_status);
			}
			assert(action.kind == AIUS_HTTP_ACTION_WRITE);
			assert(fixture.transaction.last_fault ==
			       AIUS_HTTP_FAULT_PROTOCOL);
			assert(fixture.transaction.last_status == 400);
			invalid_trials++;
		}
	}
	assert(INVALID_CONTROL_FIELD_COUNT == 8);
	assert(invalid_trials == 32);
}

static void test_require_existing_application_validation(void)
{
	struct NonDeleteCase {
		AiusSdJobKind kind;
		AiusSdInstallMode mode;
	};
	static const struct NonDeleteCase non_delete[] = {
		{ AIUS_SD_JOB_PROBE, AIUS_SD_INSTALL_REPLACE },
		{ AIUS_SD_JOB_CONFIG_LOAD, AIUS_SD_INSTALL_REPLACE },
		{ AIUS_SD_JOB_USAGE_REFRESH, AIUS_SD_INSTALL_REPLACE },
		{ AIUS_SD_JOB_UPLOAD, AIUS_SD_INSTALL_REPLACE },
		{ AIUS_SD_JOB_UPLOAD, AIUS_SD_INSTALL_APPEND_PART },
		{ AIUS_SD_JOB_UPLOAD, AIUS_SD_INSTALL_FINALIZE_PART },
		{ AIUS_SD_JOB_DOWNLOAD, AIUS_SD_INSTALL_REPLACE },
		{ AIUS_SD_JOB_REPLAY_REFILL, AIUS_SD_INSTALL_REPLACE },
	};
	static const uint8_t body[] = "{}";
	size_t rejection_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(non_delete); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch;
		AiusHttpApplicationResponse response;
		if (non_delete[i].kind == AIUS_SD_JOB_UPLOAD) {
			dispatch = dispatch_upload_length(&fixture, 1);
			response = upload_application(
				non_delete[i].mode, 1,
				non_delete[i].mode == AIUS_SD_INSTALL_REPLACE
					? 0
					: 7,
				false);
		} else if (non_delete[i].kind == AIUS_SD_JOB_DOWNLOAD) {
			dispatch =
				feed_request(&fixture,
				             "GET /sd/x HTTP/1.0\r\n\r\n");
			response = sd_job(
				AIUS_SD_JOB_DOWNLOAD, 200,
				"application/octet-stream", NULL, 0, 0);
		} else if (non_delete[i].kind ==
		           AIUS_SD_JOB_REPLAY_REFILL) {
			dispatch =
				feed_request(&fixture,
				             "GET /api/status HTTP/1.0\r\n\r\n");
			response = sd_job(
				AIUS_SD_JOB_REPLAY_REFILL, 200,
				"application/json", body, sizeof(body) - 1u, 1);
		} else {
			dispatch =
				feed_request(&fixture,
				             "GET /api/status HTTP/1.0\r\n\r\n");
			response = control_application(non_delete[i].kind);
		}
		response.sd_job.require_existing = true;
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.last_status == 400);
		assert(!fixture.transaction.sd_started);
		rejection_trials++;
	}
	assert(rejection_trials == 8);

	size_t delete_trials = 0;
	for (unsigned require_existing = 0;
	     require_existing < 2; ++require_existing) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture,
			             "DELETE /sd/x HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			control_application(AIUS_SD_JOB_DELETE);
		response.sd_job.require_existing = require_existing != 0;
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_SD_START);
		assert(action.sd_request->kind == AIUS_SD_JOB_DELETE);
		assert(action.sd_request->require_existing ==
		       (require_existing != 0));
		assert(strcmp(action.sd_request->target_path, "/delete") == 0);
		delete_trials++;
	}
	assert(delete_trials == 2);
}

static void test_request_capacity_and_strict_dispatch(void)
{
	Fixture fixture;
	init_fixture_at(&fixture, 32, sizeof(fixture.response), 0);
	AiusHttpAction action = accept_client(&fixture);
	assert(action.length == 32);
	memset(action.read_buffer, 'x', action.length);
	action = call_step(&fixture, 0,
	                   io_result(&action, AIUS_IO_PROGRESS, action.length, 0));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_REQUEST_CAPACITY);
	assert(fixture.transaction.last_status == 413);
	assert(fixture.dispatches == 0);
}

static void test_upload_prefix_staging_and_deadline(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	const char *raw =
		"PUT /sd/file.bin HTTP/1.0\r\nContent-Length: 5\r\n\r\nab";
	AiusHttpAction action = feed_request(&fixture, raw);
	assert(action.kind == AIUS_HTTP_ACTION_DISPATCH);
	assert(action.request->body_length == 2);
	assert(!action.request->body_complete);

	static const uint8_t ok[] = "{}";
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_UPLOAD, 201, "application/json",
		       ok, sizeof(ok) - 1u, 5);
	action = call_step(&fixture, 10, application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_SD_START);
	assert(action.sd_request->total == 5);
	assert(strcmp(action.sd_request->target_path, "/target") == 0);

	AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
	started.sd_capacity = 4;
	action = call_step(&fixture, 20, started);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	assert(fixture.transaction.deadline_ms == 2020);

	AiusHttpEvent prefix_need = sd_result(&action, AIUS_HTTP_SD_NEED_UPLOAD);
	prefix_need.sd_length = 4;
	action = call_step(&fixture, 20, prefix_need);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
	assert(action.length == 2);
	assert(memcmp(action.data, "ab", 2) == 0);
	assert(fixture.transaction.deadline_ms == 2020);

	const uint8_t *prefix = action.data;
	action = call_step(&fixture, 25, sd_result(&action, AIUS_HTTP_SD_PROGRESS));
	assert(memcmp(prefix, "ab", 2) == 0);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);

	AiusHttpEvent need = sd_result(&action, AIUS_HTTP_SD_NEED_UPLOAD);
	need.sd_length = 4;
	action = call_step(&fixture, 30, need);
	assert(action.kind == AIUS_HTTP_ACTION_READ);
	assert(action.length == 3);
	memcpy(action.read_buffer, "cde", 3);
	action = call_step(&fixture, 31,
	                   io_result(&action, AIUS_IO_PROGRESS, 3, 0));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
	assert(action.length == 3);
	assert(memcmp(action.data, "cde", 3) == 0);
	const uint8_t *staged = action.data;
	AiusHttpAction supply = action;

	action = call_step(&fixture, 32, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_NONE);
	assert(memcmp(staged, "cde", 3) == 0);
	assert(fixture.transaction.outstanding_action == AIUS_HTTP_ACTION_SD_STEP);
	action = call_step(&fixture, 2031, sd_result(&supply, AIUS_HTTP_SD_PROGRESS));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	assert(fixture.transaction.deadline_ms == 4031);

	AiusHttpEvent complete = sd_result(&action, AIUS_HTTP_SD_COMPLETE);
	action = call_step(&fixture, 4031, complete);
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture.transaction.last_status == 201);
	action = finish_all_writes(&fixture, action);
	finish_close(&fixture, action);
	assert(fixture.dispatches == 1);
	assert(fixture.closes == 1);
	assert(fixture.cancels == 0);
}

static void test_upload_read_would_block_and_progress(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction read = start_upload_read(&fixture);
	assert(fixture.transaction.state == AIUS_HTTP_TX_SD_TRANSFER);
	assert(fixture.transaction.sd_phase ==
	       AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT);
	assert(fixture.transaction.sd_request.kind == AIUS_SD_JOB_UPLOAD);
	assert(fixture.transaction.outstanding_action ==
	       AIUS_HTTP_ACTION_READ);
	assert(fixture.transaction.outstanding_length == 3);
	assert(fixture.transaction.sd_capacity == 4);
	assert(fixture.transaction.body_expected == 5);
	assert(fixture.transaction.upload_supplied == 0);
	assert(fixture.transaction.deadline_ms == 2000);
	uint8_t *buffer = read.read_buffer;

	AiusHttpAction reissued = call_step(
		&fixture, 100,
		io_result(&read, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert(reissued.kind == AIUS_HTTP_ACTION_READ);
	assert(reissued.action_id != read.action_id);
	assert(reissued.read_buffer == buffer);
	assert(reissued.length == 3);
	assert(fixture.transaction.sd_phase ==
	       AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT);
	assert(fixture.transaction.outstanding_action ==
	       AIUS_HTTP_ACTION_READ);
	assert(fixture.transaction.outstanding_length == 3);
	assert(fixture.transaction.sd_capacity == 4);
	assert(fixture.transaction.body_expected == 5);
	assert(fixture.transaction.upload_supplied == 0);
	assert(fixture.transaction.deadline_ms == 2000);

	memcpy(reissued.read_buffer, "abc", 3);
	AiusHttpAction supply = call_step(
		&fixture, 101,
		io_result(&reissued, AIUS_IO_PROGRESS, 3, 0));
	assert(supply.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(supply.sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
	assert(supply.length == 3);
	assert(memcmp(supply.data, "abc", 3) == 0);
	assert(fixture.transaction.sd_phase ==
	       AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY);
	assert(fixture.transaction.deadline_ms == 2101);

	AiusHttpAction advance = call_step(
		&fixture, 102,
		sd_result(&supply, AIUS_HTTP_SD_PROGRESS));
	assert(advance.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(advance.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	assert(fixture.transaction.sd_phase ==
	       AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT);
	assert(fixture.transaction.upload_supplied == 3);
	assert(fixture.transaction.deadline_ms == 2102);
}

/* The upload-progress number `sd_diag.upload_supplied` publishes, driven across
 * every chunk of a body rather than the single chunk the case above stops at.
 *
 * This field became observable in prerequisite 2 of
 * docs/plans/aius-ota-upgrade.md: it was live but serialized nowhere, so an
 * upload in flight was indistinguishable from an idle board. What a reader has
 * to be able to rely on is asserted here rather than at the serializer, because
 * the serializer only copies it:
 *
 *   - it counts BODY bytes accepted by the job, not bytes read off the socket,
 *     so it advances on the SD PROGRESS result and not on the READ result;
 *   - it is monotonic across chunks of unequal size;
 *   - it reaches exactly body_expected, so `supplied == Content-Length` is a
 *     sound completion test for a client watching /api/status.
 *
 * The first assertion is the load-bearing one. A count incremented on the read
 * would run AHEAD of the job by up to one chunk, and the whole point of
 * publishing it beside `job_transferred` is that the difference between them is
 * the in-flight slice. */
static void test_upload_supplied_tracks_the_supplied_body(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction read = start_upload_read(&fixture);
	assert(fixture.transaction.body_expected == 5);
	assert(fixture.transaction.upload_supplied == 0);

	/* First chunk: three bytes read, and STILL zero supplied -- the read has
	 * completed but the job has not accepted them yet. */
	memcpy(read.read_buffer, "abc", 3);
	AiusHttpAction supply = call_step(
		&fixture, 100, io_result(&read, AIUS_IO_PROGRESS, 3, 0));
	assert(supply.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(supply.sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
	assert(supply.length == 3);
	assert(fixture.transaction.upload_supplied == 0);

	AiusHttpAction advance = call_step(
		&fixture, 101, sd_result(&supply, AIUS_HTTP_SD_PROGRESS));
	assert(advance.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(advance.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	assert(fixture.transaction.upload_supplied == 3);

	/* Second chunk, deliberately a different size: the remaining two bytes.
	 * The job asks for four and the transaction offers what the body has
	 * left, so a fixed-chunk assumption would show up here. */
	AiusHttpEvent need = sd_result(&advance, AIUS_HTTP_SD_NEED_UPLOAD);
	need.sd_length = 4;
	AiusHttpAction read2 = call_step(&fixture, 102, need);
	assert(read2.kind == AIUS_HTTP_ACTION_READ);
	assert(read2.length == 2);
	assert(fixture.transaction.upload_supplied == 3);

	memcpy(read2.read_buffer, "de", 2);
	AiusHttpAction supply2 = call_step(
		&fixture, 103, io_result(&read2, AIUS_IO_PROGRESS, 2, 0));
	assert(supply2.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(supply2.sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
	assert(supply2.length == 2);
	assert(memcmp(supply2.data, "de", 2) == 0);
	assert(fixture.transaction.upload_supplied == 3);

	AiusHttpAction after = call_step(
		&fixture, 104, sd_result(&supply2, AIUS_HTTP_SD_PROGRESS));
	assert(after.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(fixture.transaction.upload_supplied == 5);
	assert(fixture.transaction.upload_supplied ==
	       fixture.transaction.body_expected);
}

static void drain_upload_after_transport(
	Fixture *fixture, AiusHttpAction close)
{
	assert(close.kind == AIUS_HTTP_ACTION_CLOSE);
	assert(fixture->transaction.outstanding_action ==
	       AIUS_HTTP_ACTION_CLOSE);
	AiusHttpAction cancel = call_step(
		fixture, fixture->now,
		io_result(&close, AIUS_IO_PROGRESS, 0, 0));
	assert(cancel.kind == AIUS_HTTP_ACTION_SD_CANCEL);
	assert(fixture->cancels == 1);
	AiusHttpAction advance = call_step(
		fixture, fixture->now,
		sd_result(&cancel, AIUS_HTTP_SD_PROGRESS));
	assert(advance.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(advance.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	AiusHttpAction done = call_step(
		fixture, fixture->now,
		sd_result(&advance, AIUS_HTTP_SD_CANCELLED));
	assert_none_action(&done);
	assert(fixture->transaction.state == AIUS_HTTP_TX_IDLE);
	assert(fixture->closes == 1);
	assert(fixture->cancels == 1);
}

static void test_upload_read_transport_terminal_precedence(void)
{
	struct Case {
		AiusIoResult result;
		uint32_t now;
		uint32_t driver_fault;
	};
	static const struct Case cases[] = {
		{ AIUS_IO_CLOSED, 1999, 0 },
		{ AIUS_IO_CLOSED, 2000, 0 },
		{ AIUS_IO_FAULT, 1999, 0x51 },
		{ AIUS_IO_FAULT, 2000, 0x52 },
	};
	size_t trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction read = start_upload_read(&fixture);
		assert(fixture.transaction.deadline_ms == 2000);
		AiusHttpAction close = call_step(
			&fixture, cases[i].now,
			io_result(&read, cases[i].result, 0,
			          cases[i].driver_fault));
		assert(close.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(close.sd_operation == AIUS_HTTP_SD_OP_NONE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_TRANSPORT);
		assert(fixture.transaction.last_transport_fault ==
		       cases[i].driver_fault);
		assert(fixture.transaction.outstanding_action ==
		       AIUS_HTTP_ACTION_CLOSE);
		assert(fixture.transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT);
		drain_upload_after_transport(&fixture, close);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_TRANSPORT);
		assert(fixture.transaction.last_transport_fault ==
		       cases[i].driver_fault);
		trials++;
	}
	assert(trials == 4);
}

static void test_upload_invalid_completion_cancels(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action = feed_request(
		&fixture, "PUT /sd/x HTTP/1.0\r\nContent-Length: 2\r\n\r\n");
	static const uint8_t ok[] = "{}";
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_UPLOAD, 201, "application/json",
		       ok, sizeof(ok) - 1u, 2);
	action = call_step(&fixture, 0, application_result(&action, &response));
	AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
	started.sd_capacity = 2;
	action = call_step(&fixture, 0, started);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);

	AiusHttpAction retained = action;
	action = call_step(&fixture, 0, sd_result(&retained, AIUS_HTTP_SD_COMPLETE));
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
	assert_none_action(&action);
	assert(fixture.transaction.outstanding_action_id == retained.action_id);
	action = call_step(&fixture, 0, sd_result(&retained, AIUS_HTTP_SD_PROGRESS));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	action = finish_all_writes(&fixture, action);
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	action = call_step(&fixture, 0,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_SD_CANCEL);
	assert(fixture.cancels == 1);
	action = call_step(&fixture, 0, sd_result(&action, AIUS_HTTP_SD_PROGRESS));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	action = call_step(&fixture, 0, sd_result(&action, AIUS_HTTP_SD_CANCELLED));
	assert_none_action(&action);
	assert(fixture.transaction.state == AIUS_HTTP_TX_IDLE);
	assert(fixture.cancels == 1);
}

/* Buffered PUT: the parser only reports OK once the whole body has arrived, so
 * the request buffer holds it in full by dispatch time. */
static AiusHttpAction dispatch_buffered_put(Fixture *fixture,
	                                        const uint8_t *body,
	                                        size_t body_length)
{
	static char raw[AIUS_HTTP_MAX_REQUEST];
	int header = snprintf(
		raw, sizeof(raw),
		"PUT /api/dsp/config HTTP/1.0\r\nContent-Length: %zu\r\n\r\n",
		body_length);
	assert(header > 0);
	assert((size_t)header + body_length + 1u < sizeof(raw));
	memcpy(raw + header, body, body_length);
	raw[(size_t)header + body_length] = '\0';
	AiusHttpAction action = feed_request(fixture, raw);
	assert(action.kind == AIUS_HTTP_ACTION_DISPATCH);
	assert(action.request->body_complete);
	assert(action.request->header_length == (size_t)header);
	assert(action.request->body_length == body_length);
	return action;
}

static AiusHttpApplicationResponse buffered_upload_application(uint64_t total)
{
	static const uint8_t ack[] = "{\"ok\":true}";
	return sd_job(AIUS_SD_JOB_UPLOAD, 201, "application/json", ack,
	              sizeof(ack) - 1u, total);
}

/* Drives a complete buffered upload to SD_COMPLETE, asserting every supplied
 * chunk comes straight out of the request buffer and that no further READ is
 * ever needed. Returns the number of supply steps. */
static size_t supply_buffered_upload(Fixture *fixture, AiusHttpAction action,
	                                 const uint8_t *body, size_t body_length,
	                                 size_t header_length, size_t capacity)
{
	size_t supplied = 0;
	size_t supplies = 0;
	while (supplied < body_length) {
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
		AiusHttpEvent need = sd_result(&action, AIUS_HTTP_SD_NEED_UPLOAD);
		need.sd_length = capacity;
		action = call_step(fixture, fixture->now, need);
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		assert(action.sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
		assert(action.data ==
		       fixture->request + header_length + supplied);
		assert(action.length == (body_length - supplied < capacity
						 ? body_length - supplied
						 : capacity));
		assert(memcmp(action.data, body + supplied, action.length) == 0);
		supplied += action.length;
		supplies++;
		action = call_step(fixture, fixture->now,
		                   sd_result(&action, AIUS_HTTP_SD_PROGRESS));
	}
	assert(supplied == body_length);
	assert(fixture->transaction.upload_supplied == body_length);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	action = call_step(fixture, fixture->now,
	                   sd_result(&action, AIUS_HTTP_SD_COMPLETE));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(fixture->transaction.last_status == 201);
	action = finish_all_writes(fixture, action);
	finish_close(fixture, action);
	return supplies;
}

static void test_buffered_upload_admission(void)
{
	static const uint8_t small[] = "{\"gain\":1}";
	const size_t small_length = sizeof(small) - 1u;
	static uint8_t wide[600];
	for (size_t i = 0; i < sizeof(wide); ++i)
		wide[i] = (uint8_t)('a' + (i % 26u));

	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			dispatch_buffered_put(&fixture, small, small_length);
		size_t header_length = dispatch.request->header_length;
		AiusHttpApplicationResponse response =
			buffered_upload_application(small_length);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_SD_START);
		assert(action.sd_request->kind == AIUS_SD_JOB_UPLOAD);
		assert(action.sd_request->total == small_length);
		assert(fixture.transaction.body_expected == small_length);
		assert(fixture.transaction.upload_prefix_offset == header_length);
		assert(fixture.transaction.upload_prefix_length == small_length);
		assert(fixture.transaction.parsed_request.body == NULL);
		AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
		started.sd_capacity = 4;
		action = call_step(&fixture, 0, started);
		size_t supplies = supply_buffered_upload(
			&fixture, action, small, small_length, header_length, 4);
		assert(supplies == 3);
		assert(fixture.dispatches == 1);
		assert(fixture.closes == 1);
		assert(fixture.cancels == 0);
	}

	/* Spans several receive reads, so the prefix must survive reassembly. */
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			dispatch_buffered_put(&fixture, wide, sizeof(wide));
		size_t header_length = dispatch.request->header_length;
		AiusHttpApplicationResponse response =
			buffered_upload_application(sizeof(wide));
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_SD_START);
		AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
		started.sd_capacity = AIUS_HTTP_SD_CAPACITY_MAX;
		action = call_step(&fixture, 0, started);
		size_t supplies = supply_buffered_upload(
			&fixture, action, wide, sizeof(wide), header_length,
			AIUS_HTTP_SD_CAPACITY_MAX);
		assert(supplies == 2);
		assert(fixture.cancels == 0);
	}

	/* total must still agree with the framed body length, either way. */
	static const int64_t deltas[] = { -1, 1 };
	size_t mismatch_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(deltas); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			dispatch_buffered_put(&fixture, small, small_length);
		AiusHttpApplicationResponse response =
			buffered_upload_application(
				(uint64_t)((int64_t)small_length + deltas[i]));
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.last_status == 400);
		assert(!fixture.transaction.sd_started);
		assert(fixture.transaction.upload_prefix_length == 0);
		mismatch_trials++;
	}
	assert(mismatch_trials == 2);

	/* A streamed request still rejects a mismatched total ... */
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch = dispatch_upload_length(&fixture, 4);
		AiusHttpApplicationResponse response =
			upload_application(AIUS_SD_INSTALL_REPLACE, 5, 0, false);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
		assert(!fixture.transaction.sd_started);
	}

	/* ... and every non-upload job on a streamed request. */
	static const AiusSdJobKind streamed_rejects[] = {
		AIUS_SD_JOB_PROBE,
		AIUS_SD_JOB_CONFIG_LOAD,
		AIUS_SD_JOB_USAGE_REFRESH,
		AIUS_SD_JOB_DELETE,
	};
	size_t streamed_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(streamed_rejects); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch = dispatch_upload_length(&fixture, 2);
		AiusHttpApplicationResponse response =
			control_application(streamed_rejects[i]);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.last_status == 400);
		assert(!fixture.transaction.sd_started);
		streamed_trials++;
	}
	assert(streamed_trials == 4);
}

static size_t drain_writes_to_close(Fixture *fixture, AiusHttpAction action)
{
	size_t total = 0;
	while (action.kind == AIUS_HTTP_ACTION_WRITE) {
		total += action.length;
		action = call_step(fixture, fixture->now,
		                   io_result(&action, AIUS_IO_PROGRESS,
		                             action.length, 0));
	}
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	finish_close(fixture, action);
	return total;
}

static void test_immediate_stream_body(void)
{
	static uint8_t model[AIUS_HTTP_MAX_BODY];
	static uint8_t sizing[AIUS_HTTP_MAX_RESPONSE];
	for (size_t i = 0; i < sizeof(model); ++i)
		model[i] = (uint8_t)('0' + (i % 10u));
	assert(sizeof(model) > AIUS_HTTP_MAX_RESPONSE);

	{
		Fixture fixture;
		init_fixture(&fixture);
		assert(fixture.transaction.response_capacity ==
		       AIUS_HTTP_MAX_RESPONSE);
		AiusHttpAction dispatch = feed_request(
			&fixture, "GET /api/dsp/config HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/json", model, sizeof(model));
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_status == 200);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
		assert(fixture.transaction.response_streams_body);
		assert(fixture.transaction.stream_body == model);
		assert(fixture.transaction.stream_body_length == sizeof(model));
		/* The download arm must stay untouched: it would acknowledge an
		 * SD job this response does not have. */
		assert(!fixture.transaction.response_is_download);
		assert(fixture.transaction.download_data == NULL);
		size_t header_length = fixture.transaction.response_length;
		assert(header_length ==
		       aius_http_write_headers(sizing, sizeof(sizing), 200,
		                               "application/json",
		                               sizeof(model), 0u));
		assert(header_length < AIUS_HTTP_TX_QUOTA);
		assert(action.data == fixture.response);
		assert(action.length == header_length);
		assert(contains_bytes(action.data, action.length,
		                      "Content-Length: 12288"));
		assert(fixture.transaction.deadline_active);
		assert(fixture.transaction.deadline_ms == 100);

		action = call_step(&fixture, 0,
		                   io_result(&action, AIUS_IO_PROGRESS,
		                             action.length, 0));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(action.data == model);
		const uint8_t *offered = action.data;
		size_t offered_length = action.length;
		action = call_step(&fixture, 0,
		                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(action.data == offered);
		assert(action.length == offered_length);

		size_t sent = 0;
		size_t writes = 0;
		while (action.kind == AIUS_HTTP_ACTION_WRITE) {
			assert(action.data == model + sent);
			assert(action.length ==
			       (sizeof(model) - sent < AIUS_HTTP_TX_QUOTA
					? sizeof(model) - sent
					: AIUS_HTTP_TX_QUOTA));
			assert(action.sd_operation == AIUS_HTTP_SD_OP_NONE);
			sent += action.length;
			writes++;
			action = call_step(&fixture, 0,
			                   io_result(&action, AIUS_IO_PROGRESS,
			                             action.length, 0));
		}
		assert(sent == sizeof(model));
		assert(writes == sizeof(model) / AIUS_HTTP_TX_QUOTA);
		assert(writes == 24);
		assert(fixture.transaction.stream_body_offset == sizeof(model));
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(!fixture.transaction.sd_started);
		assert(fixture.cancels == 0);
		finish_close(&fixture, action);
		assert(fixture.dispatches == 1);
		assert(fixture.closes == 1);
		assert(!fixture.transaction.response_streams_body);
		assert(fixture.transaction.stream_body == NULL);
	}

	/* Copy and stream put identical bytes on the wire; only the source of
	 * the body differs, and the split is exactly "does the copy fit". */
	size_t boundary = 0;
	while (boundary < sizeof(model) &&
	       aius_http_write_response(sizing, sizeof(sizing), 200,
	                                "application/json", model,
	                                boundary + 1u, 0u) != 0u)
		boundary++;
	assert(boundary != 0);
	assert(boundary + 1u < AIUS_HTTP_MAX_RESPONSE);
	const size_t lengths[] = { 1, 4096, boundary - 1u, boundary,
		                       boundary + 1u, AIUS_HTTP_MAX_RESPONSE,
		                       AIUS_HTTP_MAX_BODY };
	size_t copied = 0;
	size_t streamed = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(lengths); ++i) {
		bool copy_fits = aius_http_write_response(
					 sizing, sizeof(sizing), 200,
					 "application/json", model,
					 lengths[i], 0u) != 0u;
		size_t header_length = aius_http_write_headers(
			sizing, sizeof(sizing), 200, "application/json",
			lengths[i], 0u);
		assert(header_length != 0);
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch = feed_request(
			&fixture, "GET /api/dsp/config HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/json", model, lengths[i]);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_status == 200);
		assert(fixture.transaction.response_streams_body == !copy_fits);
		assert(drain_writes_to_close(&fixture, action) ==
		       header_length + lengths[i]);
		if (copy_fits)
			copied++;
		else
			streamed++;
	}
	assert(ARRAY_LENGTH(lengths) == 7);
	assert(copied == 4);
	assert(streamed == 3);

	/* Peer close mid-body closes out without any SD operation. */
	static const size_t body_writes_before_close[] = { 0, 2 };
	size_t close_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(body_writes_before_close); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch = feed_request(
			&fixture, "GET /api/dsp/config HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/json", model, sizeof(model));
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		for (size_t write = 0;
		     write <= body_writes_before_close[i]; ++write) {
			action = call_step(&fixture, 0,
			                   io_result(&action, AIUS_IO_PROGRESS,
			                             action.length, 0));
			assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		}
		AiusHttpEvent closed = no_event();
		closed.kind = AIUS_HTTP_EVENT_PEER_CLOSED;
		action = call_step(&fixture, 0, closed);
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_TRANSPORT);
		assert(fixture.transaction.response_length == 0);
		assert(fixture.transaction.download_data == NULL);
		assert(!fixture.transaction.response_is_download);
		finish_close(&fixture, action);
		assert(!fixture.transaction.sd_started);
		assert(fixture.cancels == 0);
		assert(fixture.closes == 1);
		close_trials++;
	}
	assert(close_trials == 2);
}

static AiusHttpAction start_download(Fixture *fixture, uint32_t now)
{
	AiusHttpAction action =
		feed_request(fixture, "GET /sd/file.bin HTTP/1.0\r\n\r\n");
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_DOWNLOAD, 200, "application/octet-stream",
		       NULL, 0, 0);
	action = call_step(fixture, now, application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_SD_START);
	AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
	started.sd_capacity = 8;
	action = call_step(fixture, now, started);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	return action;
}

static void test_download_stable_chunks_and_ack(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action = start_download(&fixture, 10);
	static const uint8_t chunk[] = "abcde";
	AiusHttpEvent data = sd_result(&action, AIUS_HTTP_SD_DOWNLOAD_DATA);
	data.sd_data = chunk;
	data.sd_length = sizeof(chunk) - 1u;
	data.sd_size = sizeof(chunk) - 1u;
	action = call_step(&fixture, 20, data);
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(contains_bytes(action.data, action.length, "Content-Length: 5"));
	if (fixture.transaction.deadline_ms != 120)
		fprintf(stderr,
		        "DOWNLOAD_DATA send deadline: expected 120, actual %u\n",
		        fixture.transaction.deadline_ms);
	assert(fixture.transaction.deadline_ms == 120);

	action = finish_response_headers(&fixture, action);
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(action.data == chunk);
	assert(action.length == sizeof(chunk) - 1u);
	const uint8_t *offered = action.data;
	action = call_step(&fixture, 30,
	                   io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(action.data == offered);
	action = call_step(&fixture, 40,
	                   io_result(&action, AIUS_IO_PROGRESS, 2, 0));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(action.data == offered + 2);
	assert(action.length == 3);
	action = call_step(&fixture, 50,
	                   io_result(&action, AIUS_IO_PROGRESS, 3, 0));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ACK_DOWNLOAD);
	assert(fixture.transaction.download_sent == 5);
	assert(fixture.transaction.download_data == chunk);

	action = call_step(&fixture, 60, sd_result(&action, AIUS_HTTP_SD_COMPLETE));
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	finish_close(&fixture, action);
	assert(fixture.closes == 1);
	assert(fixture.cancels == 0);
}

static void test_zero_download(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action = start_download(&fixture, 0);
	AiusHttpEvent complete = sd_result(&action, AIUS_HTTP_SD_COMPLETE);
	complete.sd_size = 0;
	action = call_step(&fixture, 0, complete);
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(contains_bytes(action.data, action.length, "Content-Length: 0"));
	action = finish_all_writes(&fixture, action);
	finish_close(&fixture, action);
}

static void test_download_cache_control(void)
{
	/* An accepted download response carrying cache_max_age_s emits the
	 * Cache-Control line in BOTH deferred header writes (data-led and the
	 * zero-length COMPLETE), an immediate response carries it inline, and
	 * an SD-failure substitution on the same transaction never does --
	 * error responses are uncacheable whatever the route asked for. */
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action = feed_request(
			&fixture, "GET /sd/www/app.css HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			sd_job(AIUS_SD_JOB_DOWNLOAD, 200, "text/css", NULL, 0, 0);
		response.cache_max_age_s = 86400u;
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		assert(action.kind == AIUS_HTTP_ACTION_SD_START);
		assert(fixture.transaction.cache_max_age_s == 86400u);
		AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
		started.sd_capacity = 8;
		action = call_step(&fixture, 0, started);
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		static const uint8_t chunk[] = "body{}";
		AiusHttpEvent data =
			sd_result(&action, AIUS_HTTP_SD_DOWNLOAD_DATA);
		data.sd_data = chunk;
		data.sd_length = sizeof(chunk) - 1u;
		data.sd_size = sizeof(chunk) - 1u;
		action = call_step(&fixture, 0, data);
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(contains_bytes(action.data, action.length,
		                      "Cache-Control: max-age=86400"));
		assert(contains_bytes(action.data, action.length,
		                      "Connection: close"));
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action = feed_request(
			&fixture, "GET /sd/www/app.css HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			sd_job(AIUS_SD_JOB_DOWNLOAD, 200, "text/css", NULL, 0, 0);
		response.cache_max_age_s = 86400u;
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
		started.sd_capacity = 8;
		action = call_step(&fixture, 0, started);
		AiusHttpEvent complete =
			sd_result(&action, AIUS_HTTP_SD_COMPLETE);
		complete.sd_size = 0;
		action = call_step(&fixture, 0, complete);
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(contains_bytes(action.data, action.length,
		                      "Content-Length: 0"));
		assert(contains_bytes(action.data, action.length,
		                      "Cache-Control: max-age=86400"));
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action = feed_request(
			&fixture, "GET /sd/www/app.css HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			sd_job(AIUS_SD_JOB_DOWNLOAD, 200, "text/css", NULL, 0, 0);
		response.cache_max_age_s = 86400u;
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
		started.sd_capacity = 8;
		action = call_step(&fixture, 0, started);
		AiusHttpEvent failed = sd_result(&action, AIUS_HTTP_SD_FAILED);
		failed.sd_fault = AIUS_SD_FAULT_NOT_FOUND;
		action = call_step(&fixture, 0, failed);
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(contains_bytes(action.data, action.length, "404"));
		assert(!contains_bytes(action.data, action.length,
		                       "Cache-Control"));
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		static const uint8_t body[] = "{}";
		AiusHttpAction action = feed_request(
			&fixture, "GET /api/dsp/config HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/json", body,
			          sizeof(body) - 1u);
		response.cache_max_age_s = 5u;
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(contains_bytes(action.data, action.length,
		                      "Cache-Control: max-age=5"));
	}
}

static void test_download_peer_fault_release_close_cancel_drain(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action = start_download(&fixture, 0);
	static const uint8_t chunk[] = "abc";
	AiusHttpEvent data = sd_result(&action, AIUS_HTTP_SD_DOWNLOAD_DATA);
	data.sd_data = chunk;
	data.sd_length = sizeof(chunk) - 1u;
	data.sd_size = 6;
	action = call_step(&fixture, 0, data);
	action = finish_response_headers(&fixture, action);
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);

	action = call_step(&fixture, 0, peer_fault(0x99));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_FAIL_DOWNLOAD);
	assert(action.fault == AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_TRANSPORT);
	assert(fixture.transaction.last_transport_fault == 0x99);

	action = call_step(&fixture, 0, sd_result(&action, AIUS_HTTP_SD_PROGRESS));
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	action = call_step(&fixture, 0,
	                   io_result(&action, AIUS_IO_CLOSED, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_SD_CANCEL);
	assert(fixture.cancels == 1);
	action = call_step(&fixture, 0, sd_result(&action, AIUS_HTTP_SD_PROGRESS));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	AiusHttpEvent cancelled = sd_result(&action, AIUS_HTTP_SD_CANCELLED);
	cancelled.sd_fault = AIUS_SD_FAULT_ROLLBACK;
	cancelled.sd_driver_fault = 0x55;
	action = call_step(&fixture, 0, cancelled);
	assert_none_action(&action);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_TRANSPORT);
	assert(fixture.transaction.last_sd_fault == AIUS_SD_FAULT_ROLLBACK);
	assert(fixture.transaction.last_driver_fault == 0x55);
	assert(fixture.closes == 1);
	assert(fixture.cancels == 1);
}

static void test_sd_failure_mapping(void)
{
	struct Mapping {
		AiusSdFault fault;
		unsigned status;
		const char *body;
		bool download;
	};
	static const struct Mapping mappings[] = {
		{ AIUS_SD_FAULT_OPEN, 404, "sd_not_found", true },
		{ AIUS_SD_FAULT_OPEN, 500, "sd_failure", false },
		{ AIUS_SD_FAULT_NOT_FOUND, 404, "sd_not_found", false },
		{ AIUS_SD_FAULT_NOT_FOUND, 404, "sd_not_found", true },
		{ AIUS_SD_FAULT_RANGE, 416, "sd_range", true },
		{ AIUS_SD_FAULT_MEDIA, 503, "sd_unavailable", true },
		{ AIUS_SD_FAULT_CAPACITY, 507, "sd_capacity", true },
		{ AIUS_SD_FAULT_OVERFLOW, 507, "sd_capacity", true },
		{ AIUS_SD_FAULT_IO, 500, "sd_failure", true },
	};
	assert(AIUS_SD_FAULT_NOT_FOUND == 18);
	for (size_t i = 0; i < ARRAY_LENGTH(mappings); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action;
		AiusHttpApplicationResponse response;
		if (mappings[i].download) {
			action = feed_request(&fixture, "GET /sd/x HTTP/1.0\r\n\r\n");
			response = sd_job(AIUS_SD_JOB_DOWNLOAD, 200,
			                  "application/octet-stream", NULL, 0, 0);
		} else {
			action = feed_request(&fixture, "DELETE /sd/x HTTP/1.0\r\n\r\n");
			static const uint8_t ok[] = "{}";
			response = sd_job(AIUS_SD_JOB_DELETE, 204,
			                  "application/json", ok, sizeof(ok) - 1u, 0);
			if (mappings[i].fault == AIUS_SD_FAULT_NOT_FOUND)
				response.sd_job.require_existing = true;
		}
		action = call_step(&fixture, 0, application_result(&action, &response));
		AiusHttpEvent failed = sd_result(&action, AIUS_HTTP_SD_FAILED);
		failed.sd_fault = mappings[i].fault;
		failed.sd_driver_fault = 7;
		action = call_step(&fixture, 0, failed);
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_SD);
		assert(fixture.transaction.last_sd_fault == mappings[i].fault);
		assert(fixture.transaction.last_driver_fault == 7);
		assert(fixture.transaction.last_status == mappings[i].status);
		assert(contains_bytes(action.data, action.length, mappings[i].body));
		if (mappings[i].fault == AIUS_SD_FAULT_NOT_FOUND) {
			static const uint8_t not_found_body[] =
				"{\"ok\":false,\"error\":\"sd_not_found\"}";
			uint8_t expected[256];
			size_t expected_length = aius_http_write_response(
				expected, sizeof(expected), 404,
				"application/json", not_found_body,
				sizeof(not_found_body) - 1u, 0u);
			assert(expected_length > 0);
			assert(fixture.transaction.response_length ==
			       expected_length);
			assert(memcmp(fixture.response, expected,
			              expected_length) == 0);
		}
		action = finish_all_writes(&fixture, action);
		finish_close(&fixture, action);
	}

	Fixture fixture;
	init_fixture(&fixture);
	AiusHttpAction action = start_other_advance(&fixture);
	uint64_t retained_action_id = action.action_id;
	AiusHttpEvent invalid = sd_result(&action, AIUS_HTTP_SD_FAILED);
	invalid.sd_fault = (AiusSdFault)(AIUS_SD_FAULT_NOT_FOUND + 1);
	invalid.sd_driver_fault = 8;
	action = call_step(&fixture, 0, invalid);
	assert_none_action(&action);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
	assert(fixture.transaction.outstanding_action ==
	       AIUS_HTTP_ACTION_SD_STEP);
	assert(fixture.transaction.outstanding_action_id ==
	       retained_action_id);
}

static void test_sd_result_matrix_rejects_wrong_kind_and_retains_authority(void)
{
	struct Case {
		AiusSdJobKind job;
		AiusHttpSdOperation operation;
		AiusHttpSdEventKind invalid;
	};
	static const struct Case cases[] = {
		{ AIUS_SD_JOB_UPLOAD, AIUS_HTTP_SD_OP_ADVANCE, AIUS_HTTP_SD_DOWNLOAD_DATA },
		{ AIUS_SD_JOB_DOWNLOAD, AIUS_HTTP_SD_OP_ADVANCE, AIUS_HTTP_SD_NEED_UPLOAD },
		{ AIUS_SD_JOB_DELETE, AIUS_HTTP_SD_OP_ADVANCE, AIUS_HTTP_SD_NEED_UPLOAD },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action;
		AiusHttpApplicationResponse response;
		if (cases[i].job == AIUS_SD_JOB_UPLOAD) {
			action = feed_request(
				&fixture, "PUT /sd/x HTTP/1.0\r\nContent-Length: 1\r\n\r\n");
			static const uint8_t ok[] = "{}";
			response = sd_job(cases[i].job, 201, "application/json",
			                  ok, sizeof(ok) - 1u, 1);
		} else if (cases[i].job == AIUS_SD_JOB_DOWNLOAD) {
			action = feed_request(&fixture, "GET /sd/x HTTP/1.0\r\n\r\n");
			response = sd_job(cases[i].job, 200,
			                  "application/octet-stream", NULL, 0, 0);
		} else {
			action = feed_request(&fixture, "DELETE /sd/x HTTP/1.0\r\n\r\n");
			static const uint8_t ok[] = "{}";
			response = sd_job(cases[i].job, 204, "application/json",
			                  ok, sizeof(ok) - 1u, 0);
		}
		action = call_step(&fixture, 0, application_result(&action, &response));
		AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
		started.sd_capacity = 8;
		action = call_step(&fixture, 0, started);
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		assert(action.sd_operation == cases[i].operation);
		uint64_t retained_id = action.action_id;
		AiusHttpEvent invalid = sd_result(&action, cases[i].invalid);
		if (cases[i].invalid == AIUS_HTTP_SD_DOWNLOAD_DATA) {
			static const uint8_t byte = 1;
			invalid.sd_data = &byte;
			invalid.sd_length = 1;
			invalid.sd_size = 1;
		} else {
			invalid.sd_length = 1;
		}
		action = call_step(&fixture, 0, invalid);
		assert_none_action(&action);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.outstanding_action == AIUS_HTTP_ACTION_SD_STEP);
		assert(fixture.transaction.outstanding_action_id == retained_id);
		action = call_step(&fixture, 0,
		                   sd_result(&(AiusHttpAction){
			                   .kind = AIUS_HTTP_ACTION_SD_STEP,
			                   .action_id = retained_id
		                   }, AIUS_HTTP_SD_PROGRESS));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	}
}

static void test_sd_deadlines(void)
{
	struct DeadlineCase {
		AiusSdJobKind kind;
		AiusHttpFault fault;
		uint32_t interval;
		const char *request;
	};
	static const struct DeadlineCase cases[] = {
		{ AIUS_SD_JOB_UPLOAD, AIUS_HTTP_FAULT_UPLOAD_TIMEOUT, 2000,
		  "PUT /sd/x HTTP/1.0\r\nContent-Length: 1\r\n\r\n" },
		{ AIUS_SD_JOB_DOWNLOAD, AIUS_HTTP_FAULT_DOWNLOAD_TIMEOUT, 10000,
		  "GET /sd/x HTTP/1.0\r\n\r\n" },
		{ AIUS_SD_JOB_DELETE, AIUS_HTTP_FAULT_SD_TIMEOUT, 2000,
		  "DELETE /sd/x HTTP/1.0\r\n\r\n" },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		Fixture fixture;
		init_fixture_at(&fixture, sizeof(fixture.request),
		                sizeof(fixture.response), UINT32_MAX - 10u);
		AiusHttpAction action = feed_request(&fixture, cases[i].request);
		static const uint8_t ok[] = "{}";
		AiusHttpApplicationResponse response =
			cases[i].kind == AIUS_SD_JOB_DOWNLOAD
				? sd_job(cases[i].kind, 200, "application/octet-stream",
				         NULL, 0, 0)
				: sd_job(cases[i].kind, 204, "application/json",
				         ok, sizeof(ok) - 1u,
				         cases[i].kind == AIUS_SD_JOB_UPLOAD ? 1 : 0);
		action = call_step(&fixture, UINT32_MAX - 10u,
		                   application_result(&action, &response));
		AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
		started.sd_capacity = 8;
		action = call_step(&fixture, UINT32_MAX - 10u, started);
		uint32_t deadline = (UINT32_MAX - 10u) + cases[i].interval;
		assert(fixture.transaction.deadline_ms == deadline);
		action = call_step(&fixture, deadline, no_event());
		assert_none_action(&action);
		assert(fixture.transaction.last_fault == cases[i].fault);
		assert(fixture.transaction.outstanding_action == AIUS_HTTP_ACTION_SD_STEP);
		uint64_t retained_id = fixture.transaction.outstanding_action_id;
		action = call_step(&fixture, deadline,
		                   sd_result(&(AiusHttpAction){
			                   .kind = AIUS_HTTP_ACTION_SD_STEP,
			                   .action_id = retained_id
		                   }, AIUS_HTTP_SD_PROGRESS));
		if (cases[i].kind == AIUS_SD_JOB_UPLOAD)
			assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		else
			assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	}
}

static void test_action_id_exhaustion(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	fixture.transaction.next_action_id = UINT64_MAX;
	AiusHttpAction action = call_step(&fixture, 0, no_event());
	assert(action.kind == AIUS_HTTP_ACTION_ACCEPT);
	assert(action.action_id == UINT64_MAX);
	action = call_step(&fixture, 0,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert_none_action(&action);
	assert(!fixture.transaction.valid);
	assert(fixture.transaction.action_ids_exhausted);
	assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_ACTION_ID_EXHAUSTED);
	AiusHttpTransaction snapshot = fixture.transaction;
	action = call_step(&fixture, 1, no_event());
	assert_none_action(&action);
	assert(memcmp(&fixture.transaction, &snapshot,
	              sizeof(fixture.transaction)) == 0);
}

enum EventField {
	EVENT_FIELD_ACTION_ID,
	EVENT_FIELD_COMPLETED_ACTION,
	EVENT_FIELD_IO_RESULT,
	EVENT_FIELD_IO_BYTES,
	EVENT_FIELD_IO_FAULT,
	EVENT_FIELD_APPLICATION_RESPONSE,
	EVENT_FIELD_SD_KIND,
	EVENT_FIELD_SD_DATA,
	EVENT_FIELD_SD_CAPACITY,
	EVENT_FIELD_SD_LENGTH,
	EVENT_FIELD_SD_SIZE,
	EVENT_FIELD_SD_FAULT,
	EVENT_FIELD_SD_DRIVER_FAULT,
	EVENT_FIELD_PEER_FAULT,
	EVENT_FIELD_COUNT
};

#define EVENT_FIELD_BIT(field) (UINT32_C(1) << (field))

static void mutate_event_field(AiusHttpEvent *event, enum EventField field)
{
	static const uint8_t byte = 0x5a;
	switch (field) {
	case EVENT_FIELD_ACTION_ID: event->action_id = UINT64_C(99); break;
	case EVENT_FIELD_COMPLETED_ACTION:
		event->completed_action = AIUS_HTTP_ACTION_WRITE;
		break;
	case EVENT_FIELD_IO_RESULT: event->io.result = AIUS_IO_WOULD_BLOCK; break;
	case EVENT_FIELD_IO_BYTES: event->io.bytes = 1; break;
	case EVENT_FIELD_IO_FAULT: event->io.fault = 1; break;
	case EVENT_FIELD_APPLICATION_RESPONSE:
		event->application_response =
			(const AiusHttpApplicationResponse *)(uintptr_t)1;
		break;
	case EVENT_FIELD_SD_KIND: event->sd_kind = AIUS_HTTP_SD_PROGRESS; break;
	case EVENT_FIELD_SD_DATA: event->sd_data = &byte; break;
	case EVENT_FIELD_SD_CAPACITY: event->sd_capacity = 1; break;
	case EVENT_FIELD_SD_LENGTH: event->sd_length = 1; break;
	case EVENT_FIELD_SD_SIZE: event->sd_size = 1; break;
	case EVENT_FIELD_SD_FAULT: event->sd_fault = AIUS_SD_FAULT_IO; break;
	case EVENT_FIELD_SD_DRIVER_FAULT: event->sd_driver_fault = 1; break;
	case EVENT_FIELD_PEER_FAULT: event->peer_fault = 1; break;
	case EVENT_FIELD_COUNT: assert(0); break;
	}
}

static void test_event_schema_equivalence_classes(void)
{
	struct EventSchema {
		AiusHttpEventKind kind;
		uint32_t named_fields;
	};
	static const struct EventSchema generic_schemas[] = {
		{ AIUS_HTTP_EVENT_NONE, 0 },
		{ AIUS_HTTP_EVENT_IO_RESULT,
		  EVENT_FIELD_BIT(EVENT_FIELD_ACTION_ID) |
		  EVENT_FIELD_BIT(EVENT_FIELD_COMPLETED_ACTION) |
		  EVENT_FIELD_BIT(EVENT_FIELD_IO_RESULT) |
		  EVENT_FIELD_BIT(EVENT_FIELD_IO_BYTES) |
		  EVENT_FIELD_BIT(EVENT_FIELD_IO_FAULT) },
		{ AIUS_HTTP_EVENT_PEER_CLOSED, 0 },
		{ AIUS_HTTP_EVENT_PEER_FAULT,
		  EVENT_FIELD_BIT(EVENT_FIELD_PEER_FAULT) },
	};
	assert(ARRAY_LENGTH(generic_schemas) == 4);
	assert(EVENT_FIELD_COUNT == 14);

	size_t mutation_trials = 0;
	for (size_t schema_index = 0;
	     schema_index < ARRAY_LENGTH(generic_schemas); ++schema_index) {
		for (unsigned field = 0; field < EVENT_FIELD_COUNT; ++field) {
			if ((generic_schemas[schema_index].named_fields &
			     EVENT_FIELD_BIT(field)) != 0)
				continue;
			Fixture fixture;
			init_fixture(&fixture);
			AiusHttpAction read = accept_client(&fixture);
			AiusHttpEvent event = no_event();
			event.kind = generic_schemas[schema_index].kind;
			if (event.kind == AIUS_HTTP_EVENT_IO_RESULT) {
				event.action_id = read.action_id;
				event.completed_action = AIUS_HTTP_ACTION_READ;
				event.io.result = AIUS_IO_WOULD_BLOCK;
			} else if (event.kind == AIUS_HTTP_EVENT_PEER_FAULT) {
				event.peer_fault = 0x77;
			}
			mutate_event_field(&event, (enum EventField)field);
			AiusHttpAction action = call_step(&fixture, 0, event);
			assert(action.kind == AIUS_HTTP_ACTION_WRITE);
			assert(fixture.transaction.last_fault ==
			       AIUS_HTTP_FAULT_PROTOCOL);
			assert(fixture.transaction.last_status == 400);
			mutation_trials++;
		}
	}

	static const uint8_t body[] = "{}";
	const uint32_t application_named =
		EVENT_FIELD_BIT(EVENT_FIELD_ACTION_ID) |
		EVENT_FIELD_BIT(EVENT_FIELD_COMPLETED_ACTION) |
		EVENT_FIELD_BIT(EVENT_FIELD_APPLICATION_RESPONSE);
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/json", body, sizeof(body) - 1u);
		AiusHttpAction action = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
	}
	for (unsigned field = 0; field < EVENT_FIELD_COUNT; ++field) {
		if ((application_named & EVENT_FIELD_BIT(field)) != 0)
			continue;
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/json", body, sizeof(body) - 1u);
		AiusHttpEvent event = application_result(&dispatch, &response);
		mutate_event_field(&event, (enum EventField)field);
		AiusHttpAction action = call_step(&fixture, 0, event);
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.last_status == 400);
		mutation_trials++;
	}

	const uint32_t sd_started_named =
		EVENT_FIELD_BIT(EVENT_FIELD_ACTION_ID) |
		EVENT_FIELD_BIT(EVENT_FIELD_COMPLETED_ACTION) |
		EVENT_FIELD_BIT(EVENT_FIELD_SD_KIND) |
		EVENT_FIELD_BIT(EVENT_FIELD_SD_CAPACITY);
	for (unsigned baseline = 0; baseline < 2; ++baseline) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture, "DELETE /sd/x HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			sd_job(AIUS_SD_JOB_DELETE, 204, "application/json",
			       body, sizeof(body) - 1u, 0);
		AiusHttpAction start = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		assert(start.kind == AIUS_HTTP_ACTION_SD_START);
		AiusHttpEvent event = sd_result(&start, AIUS_HTTP_SD_STARTED);
		event.sd_capacity = 8;
		if (baseline == 0) {
			AiusHttpAction action = call_step(&fixture, 0, event);
			assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
			assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
			assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
			continue;
		}
	}
	for (unsigned field = 0; field < EVENT_FIELD_COUNT; ++field) {
		if ((sd_started_named & EVENT_FIELD_BIT(field)) != 0)
			continue;
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture, "DELETE /sd/x HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			sd_job(AIUS_SD_JOB_DELETE, 204, "application/json",
			       body, sizeof(body) - 1u, 0);
		AiusHttpAction start = call_step(
			&fixture, 0, application_result(&dispatch, &response));
		AiusHttpEvent event = sd_result(&start, AIUS_HTTP_SD_STARTED);
		event.sd_capacity = 8;
		mutate_event_field(&event, (enum EventField)field);
		AiusHttpAction action = call_step(&fixture, 0, event);
		assert_none_action(&action);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_PROTOCOL);
		assert(fixture.transaction.outstanding_action ==
		       AIUS_HTTP_ACTION_SD_START);
		assert(fixture.transaction.outstanding_action_id ==
		       start.action_id);
		mutation_trials++;
	}
	assert(mutation_trials == 71);
}

static void test_rx_tx_quota_boundaries(void)
{
	static const size_t boundaries[] = { 0, 1, 255, 256, 257, 511, 512, 513 };
	assert(ARRAY_LENGTH(boundaries) == 8);

	size_t rx_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(boundaries); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action = accept_client(&fixture);
		size_t supplied = 0;
		if (boundaries[i] == 0) {
			const uint8_t *offered = action.read_buffer;
			action = call_step(
				&fixture, 0,
				io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
			assert(action.kind == AIUS_HTTP_ACTION_READ);
			assert(action.read_buffer == offered);
		} else {
			while (supplied < boundaries[i]) {
				assert(action.kind == AIUS_HTTP_ACTION_READ);
				assert(action.length <= AIUS_HTTP_RX_QUOTA);
				size_t count = boundaries[i] - supplied;
				if (count > action.length)
					count = action.length;
				memset(action.read_buffer, 'x', count);
				supplied += count;
				action = call_step(
					&fixture, 0,
					io_result(&action, AIUS_IO_PROGRESS, count, 0));
			}
			assert(action.kind == AIUS_HTTP_ACTION_READ);
			assert(fixture.transaction.request_length == boundaries[i]);
		}
		rx_trials++;
	}
	assert(rx_trials == 8);

	size_t tx_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(boundaries); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		static uint8_t body[700];
		AiusHttpApplicationResponse response =
			immediate(200, "application/octet-stream", body, sizeof(body));
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		size_t accepted = 0;
		if (boundaries[i] == 0) {
			const uint8_t *offered = action.data;
			size_t offered_length = action.length;
			action = call_step(
				&fixture, 0,
				io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
			assert(action.data == offered);
			assert(action.length == offered_length);
		} else {
			while (accepted < boundaries[i]) {
				assert(action.kind == AIUS_HTTP_ACTION_WRITE);
				assert(action.length <= AIUS_HTTP_TX_QUOTA);
				size_t count = boundaries[i] - accepted;
				if (count > action.length)
					count = action.length;
				accepted += count;
				action = call_step(
					&fixture, 0,
					io_result(&action, AIUS_IO_PROGRESS, count, 0));
			}
			assert(fixture.transaction.response_offset == boundaries[i]);
			assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		}
		tx_trials++;
	}
	assert(tx_trials == 8);
}

static void test_all_application_statuses_and_field_edges(void)
{
	static const unsigned statuses[] = {
		200, 201, 204, 400, 401, 403, 404, 405,
		408, 409, 413, 416, 500, 503, 507
	};
	assert(ARRAY_LENGTH(statuses) == 15);
	static const uint8_t body[] = "{}";
	size_t status_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(statuses); ++i) {
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(statuses[i], "a", body, sizeof(body) - 1u);
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_status == statuses[i]);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
		status_trials++;
	}
	assert(status_trials == 15);

	char type_63[AIUS_HTTP_CONTENT_TYPE_STORAGE];
	memset(type_63, 't', sizeof(type_63));
	type_63[sizeof(type_63) - 1u] = '\0';
	char type_64[AIUS_HTTP_CONTENT_TYPE_STORAGE + 1u];
	memset(type_64, 't', sizeof(type_64));
	type_64[sizeof(type_64) - 1u] = '\0';
	struct FieldEdge {
		const char *content_type;
		const uint8_t *body;
		size_t body_length;
		bool valid;
	};
	static const struct FieldEdge fixed_edges[] = {
		{ "a", NULL, 0, true },
		{ "", NULL, 0, false },
		{ NULL, NULL, 0, false },
		{ "a", NULL, 1, false },
	};
	size_t field_trials = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(fixed_edges) + 2u; ++i) {
		struct FieldEdge edge;
		if (i < ARRAY_LENGTH(fixed_edges))
			edge = fixed_edges[i];
		else if (i == ARRAY_LENGTH(fixed_edges))
			edge = (struct FieldEdge){ type_63, NULL, 0, true };
		else
			edge = (struct FieldEdge){ type_64, NULL, 0, false };
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction action =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, edge.content_type, edge.body, edge.body_length);
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		if (edge.valid) {
			assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
			assert(fixture.transaction.last_status == 200);
		} else {
			assert(fixture.transaction.last_fault ==
			       AIUS_HTTP_FAULT_PROTOCOL);
			assert(fixture.transaction.last_status == 400);
		}
		field_trials++;
	}
	assert(field_trials == 6);

	static const uint8_t fallback_body[] =
		"{\"ok\":false,\"error\":\"response_too_large\"}";
	uint8_t sizing[AIUS_HTTP_MAX_RESPONSE];
	uint8_t large[256];
	size_t fallback_length =
		aius_http_write_response(sizing, sizeof(sizing), 500,
		                         "application/json", fallback_body,
		                         sizeof(fallback_body) - 1u, 0u);
	size_t primary_length =
		aius_http_write_response(sizing, sizeof(sizing), 200,
		                         "application/octet-stream", large,
		                         sizeof(large), 0u);
	size_t header_length =
		aius_http_write_headers(sizing, sizeof(sizing), 200,
		                        "application/octet-stream",
		                        sizeof(large), 0u);
	assert(fallback_length > 1);
	assert(primary_length > fallback_length + 1u);
	assert(header_length > 1);
	assert(header_length < fallback_length);
	/* An immediate body that does not fit is streamed from the
	 * application's buffer, so only a capacity below the headers alone
	 * still exhausts the response. */
	size_t capacities[] = {
		1, header_length - 1u, header_length, fallback_length,
		primary_length - 1u, primary_length
	};
	assert(ARRAY_LENGTH(capacities) == 6);
	size_t capacity_trials = 0;
	size_t copied = 0;
	size_t streamed = 0;
	size_t exhausted = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(capacities); ++i) {
		Fixture fixture;
		init_fixture_at(&fixture, sizeof(fixture.request), capacities[i], 0);
		AiusHttpAction action =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			immediate(200, "application/octet-stream", large, sizeof(large));
		action = call_step(&fixture, 0,
		                   application_result(&action, &response));
		if (capacities[i] >= primary_length) {
			assert(action.kind == AIUS_HTTP_ACTION_WRITE);
			assert(fixture.transaction.last_status == 200);
			assert(!fixture.transaction.response_streams_body);
			assert(fixture.transaction.response_length ==
			       primary_length);
			assert(action.data == fixture.response);
			copied++;
		} else if (capacities[i] >= header_length) {
			assert(action.kind == AIUS_HTTP_ACTION_WRITE);
			assert(fixture.transaction.last_status == 200);
			assert(fixture.transaction.last_fault ==
			       AIUS_HTTP_FAULT_NONE);
			assert(fixture.transaction.response_streams_body);
			assert(fixture.transaction.response_length ==
			       header_length);
			assert(fixture.transaction.stream_body == large);
			assert(fixture.transaction.stream_body_length ==
			       sizeof(large));
			assert(!fixture.transaction.response_is_download);
			assert(fixture.transaction.download_data == NULL);
			streamed++;
		} else {
			assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
			assert(fixture.transaction.last_fault ==
			       AIUS_HTTP_FAULT_RESPONSE_CAPACITY);
			assert(!fixture.transaction.response_streams_body);
			assert(fixture.transaction.response_length == 0);
			exhausted++;
		}
		capacity_trials++;
	}
	assert(capacity_trials == 6);
	assert(copied == 1);
	assert(streamed == 3);
	assert(exhausted == 2);
}

enum SdMatrixContext {
	SD_MATRIX_START,
	SD_MATRIX_ADVANCE_UPLOAD,
	SD_MATRIX_ADVANCE_DOWNLOAD,
	SD_MATRIX_ADVANCE_OTHER,
	SD_MATRIX_SUPPLY_UPLOAD,
	SD_MATRIX_ACK_DOWNLOAD,
	SD_MATRIX_FAIL_DOWNLOAD,
	SD_MATRIX_CANCEL,
	SD_MATRIX_ADVANCE_CANCEL,
	SD_MATRIX_CONTEXT_COUNT
};

static uint32_t sd_valid_mask(enum SdMatrixContext context)
{
	switch (context) {
	case SD_MATRIX_START:
		return EVENT_FIELD_BIT(AIUS_HTTP_SD_STARTED - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_FAILED - 1);
	case SD_MATRIX_ADVANCE_UPLOAD:
		return EVENT_FIELD_BIT(AIUS_HTTP_SD_PROGRESS - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_NEED_UPLOAD - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_COMPLETE - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_FAILED - 1);
	case SD_MATRIX_ADVANCE_DOWNLOAD:
		return EVENT_FIELD_BIT(AIUS_HTTP_SD_PROGRESS - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_DOWNLOAD_DATA - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_COMPLETE - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_FAILED - 1);
	case SD_MATRIX_ADVANCE_OTHER:
		return EVENT_FIELD_BIT(AIUS_HTTP_SD_PROGRESS - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_COMPLETE - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_FAILED - 1);
	case SD_MATRIX_SUPPLY_UPLOAD:
		return EVENT_FIELD_BIT(AIUS_HTTP_SD_PROGRESS - 1);
	case SD_MATRIX_ACK_DOWNLOAD:
		return EVENT_FIELD_BIT(AIUS_HTTP_SD_PROGRESS - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_DOWNLOAD_DATA - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_COMPLETE - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_FAILED - 1);
	case SD_MATRIX_FAIL_DOWNLOAD:
	case SD_MATRIX_CANCEL:
	case SD_MATRIX_ADVANCE_CANCEL:
		return EVENT_FIELD_BIT(AIUS_HTTP_SD_PROGRESS - 1) |
		       EVENT_FIELD_BIT(AIUS_HTTP_SD_CANCELLED - 1);
	case SD_MATRIX_CONTEXT_COUNT: return 0;
	}
	return 0;
}

static AiusHttpAction start_delete(Fixture *fixture)
{
	static const uint8_t body[] = "{}";
	AiusHttpAction action =
		feed_request(fixture, "DELETE /sd/x HTTP/1.0\r\n\r\n");
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_DELETE, 204, "application/json",
		       body, sizeof(body) - 1u, 0);
	action = call_step(fixture, fixture->now,
	                   application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_SD_START);
	return action;
}

static AiusHttpAction start_upload_advance(Fixture *fixture,
	                                       bool complete_prefix)
{
	const char *request = complete_prefix
		? "PUT /sd/x HTTP/1.0\r\nContent-Length: 1\r\n\r\nx"
		: "PUT /sd/x HTTP/1.0\r\nContent-Length: 1\r\n\r\n";
	static const uint8_t body[] = "{}";
	AiusHttpAction action = feed_request(fixture, request);
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_UPLOAD, 201, "application/json",
		       body, sizeof(body) - 1u, 1);
	action = call_step(fixture, fixture->now,
	                   application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_SD_START);
	AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
	started.sd_capacity = 8;
	action = call_step(fixture, fixture->now, started);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	return action;
}

static AiusHttpAction start_other_advance(Fixture *fixture)
{
	AiusHttpAction action = start_delete(fixture);
	AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
	started.sd_capacity = 8;
	action = call_step(fixture, fixture->now, started);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	return action;
}

static AiusHttpAction start_upload_supply(Fixture *fixture)
{
	AiusHttpAction action = start_upload_advance(fixture, true);
	AiusHttpEvent need = sd_result(&action, AIUS_HTTP_SD_NEED_UPLOAD);
	need.sd_length = 1;
	action = call_step(fixture, fixture->now, need);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
	assert(action.length == 1);
	assert(action.data[0] == 'x');
	return action;
}

static AiusHttpAction start_upload_read(Fixture *fixture)
{
	static const uint8_t body[] = "{}";
	AiusHttpAction action = feed_request(
		fixture,
		"PUT /sd/x HTTP/1.0\r\nContent-Length: 5\r\n\r\n");
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_UPLOAD, 201, "application/json",
		       body, sizeof(body) - 1u, 5);
	action = call_step(fixture, fixture->now,
	                   application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_SD_START);
	AiusHttpEvent started = sd_result(&action, AIUS_HTTP_SD_STARTED);
	started.sd_capacity = 4;
	action = call_step(fixture, fixture->now, started);
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
	AiusHttpEvent need = sd_result(&action, AIUS_HTTP_SD_NEED_UPLOAD);
	need.sd_length = 3;
	action = call_step(fixture, fixture->now, need);
	assert(action.kind == AIUS_HTTP_ACTION_READ);
	assert(action.length == 3);
	return action;
}

static AiusHttpAction start_download_ack(Fixture *fixture, uint64_t total)
{
	static const uint8_t chunk[] = { 0x33 };
	AiusHttpAction action = start_download(fixture, fixture->now);
	AiusHttpEvent data = sd_result(&action, AIUS_HTTP_SD_DOWNLOAD_DATA);
	data.sd_data = chunk;
	data.sd_length = sizeof(chunk);
	data.sd_size = total;
	action = call_step(fixture, fixture->now, data);
	action = finish_response_headers(fixture, action);
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	assert(action.data == chunk);
	action = call_step(fixture, fixture->now,
	                   io_result(&action, AIUS_IO_PROGRESS,
	                             action.length, 0));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_ACK_DOWNLOAD);
	return action;
}

static AiusHttpAction start_download_fail(Fixture *fixture)
{
	AiusHttpAction action = start_download(fixture, fixture->now);
	static const uint8_t chunk[] = { 0x44 };
	AiusHttpEvent data = sd_result(&action, AIUS_HTTP_SD_DOWNLOAD_DATA);
	data.sd_data = chunk;
	data.sd_length = sizeof(chunk);
	data.sd_size = 2;
	action = call_step(fixture, fixture->now, data);
	action = finish_response_headers(fixture, action);
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	action = call_step(fixture, fixture->now, peer_fault(0x31));
	assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
	assert(action.sd_operation == AIUS_HTTP_SD_OP_FAIL_DOWNLOAD);
	return action;
}

static AiusHttpAction start_sd_cancel(Fixture *fixture)
{
	AiusHttpAction advance = start_upload_advance(fixture, false);
	AiusHttpAction action =
		call_step(fixture, fixture->now, peer_fault(0x32));
	assert_none_action(&action);
	assert(fixture->transaction.outstanding_action_id == advance.action_id);
	action = call_step(fixture, fixture->now,
	                   sd_result(&advance, AIUS_HTTP_SD_PROGRESS));
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	action = call_step(fixture, fixture->now,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_SD_CANCEL);
	return action;
}

static AiusHttpAction start_protocol_sd_cancel(Fixture *fixture)
{
	AiusHttpAction advance = start_upload_advance(fixture, false);
	AiusHttpEvent invalid =
		sd_result(&advance, AIUS_HTTP_SD_STARTED);
	invalid.sd_capacity = 8;
	AiusHttpAction action =
		call_step(fixture, fixture->now, invalid);
	assert_none_action(&action);
	assert(fixture->transaction.last_fault ==
	       AIUS_HTTP_FAULT_PROTOCOL);
	assert(fixture->transaction.outstanding_action_id ==
	       advance.action_id);
	action = call_step(fixture, fixture->now,
	                   sd_result(&advance, AIUS_HTTP_SD_PROGRESS));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	action = finish_all_writes(fixture, action);
	assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
	action = call_step(fixture, fixture->now,
	                   io_result(&action, AIUS_IO_PROGRESS, 0, 0));
	assert(action.kind == AIUS_HTTP_ACTION_SD_CANCEL);
	return action;
}

enum SdSchemaContext {
	SD_SCHEMA_PROGRESS,
	SD_SCHEMA_NEED_UPLOAD,
	SD_SCHEMA_DOWNLOAD_DATA,
	SD_SCHEMA_COMPLETE_ZERO_DOWNLOAD,
	SD_SCHEMA_COMPLETE_OTHER,
	SD_SCHEMA_FAILED,
	SD_SCHEMA_CANCELLED,
	SD_SCHEMA_CONTEXT_COUNT
};

static AiusHttpAction reach_sd_schema_context(
	Fixture *fixture, enum SdSchemaContext context)
{
	init_fixture(fixture);
	switch (context) {
	case SD_SCHEMA_PROGRESS:
		return start_other_advance(fixture);
	case SD_SCHEMA_NEED_UPLOAD:
		return start_upload_advance(fixture, false);
	case SD_SCHEMA_DOWNLOAD_DATA:
	case SD_SCHEMA_COMPLETE_ZERO_DOWNLOAD:
		return start_download(fixture, fixture->now);
	case SD_SCHEMA_COMPLETE_OTHER:
		return start_other_advance(fixture);
	case SD_SCHEMA_FAILED:
		return start_other_advance(fixture);
	case SD_SCHEMA_CANCELLED:
		return start_protocol_sd_cancel(fixture);
	case SD_SCHEMA_CONTEXT_COUNT:
		break;
	}
	assert(0);
	return (AiusHttpAction){ 0 };
}

static AiusHttpEvent canonical_sd_schema_event(
	const AiusHttpAction *action, enum SdSchemaContext context)
{
	static const uint8_t byte = 0x6d;
	AiusHttpEvent event;
	switch (context) {
	case SD_SCHEMA_PROGRESS:
		event = sd_result(action, AIUS_HTTP_SD_PROGRESS);
		break;
	case SD_SCHEMA_NEED_UPLOAD:
		event = sd_result(action, AIUS_HTTP_SD_NEED_UPLOAD);
		event.sd_length = 1;
		break;
	case SD_SCHEMA_DOWNLOAD_DATA:
		event = sd_result(action, AIUS_HTTP_SD_DOWNLOAD_DATA);
		event.sd_data = &byte;
		event.sd_length = 1;
		event.sd_size = 1;
		break;
	case SD_SCHEMA_COMPLETE_ZERO_DOWNLOAD:
	case SD_SCHEMA_COMPLETE_OTHER:
		event = sd_result(action, AIUS_HTTP_SD_COMPLETE);
		if (context == SD_SCHEMA_COMPLETE_ZERO_DOWNLOAD)
			event.sd_size = 0;
		break;
	case SD_SCHEMA_FAILED:
		event = sd_result(action, AIUS_HTTP_SD_FAILED);
		event.sd_fault = AIUS_SD_FAULT_IO;
		event.sd_driver_fault = 1;
		break;
	case SD_SCHEMA_CANCELLED:
		event = sd_result(action, AIUS_HTTP_SD_CANCELLED);
		event.sd_fault = AIUS_SD_FAULT_ROLLBACK;
		event.sd_driver_fault = 2;
		break;
	case SD_SCHEMA_CONTEXT_COUNT:
		assert(0);
		event = no_event();
		break;
	}
	return event;
}

static uint32_t sd_schema_named_fields(enum SdSchemaContext context)
{
	uint32_t fields =
		EVENT_FIELD_BIT(EVENT_FIELD_ACTION_ID) |
		EVENT_FIELD_BIT(EVENT_FIELD_COMPLETED_ACTION) |
		EVENT_FIELD_BIT(EVENT_FIELD_SD_KIND);
	switch (context) {
	case SD_SCHEMA_PROGRESS:
		break;
	case SD_SCHEMA_NEED_UPLOAD:
		fields |= EVENT_FIELD_BIT(EVENT_FIELD_SD_LENGTH);
		break;
	case SD_SCHEMA_DOWNLOAD_DATA:
		fields |= EVENT_FIELD_BIT(EVENT_FIELD_SD_DATA) |
		          EVENT_FIELD_BIT(EVENT_FIELD_SD_LENGTH) |
		          EVENT_FIELD_BIT(EVENT_FIELD_SD_SIZE);
		break;
	case SD_SCHEMA_COMPLETE_ZERO_DOWNLOAD:
		fields |= EVENT_FIELD_BIT(EVENT_FIELD_SD_SIZE);
		break;
	case SD_SCHEMA_COMPLETE_OTHER:
		break;
	case SD_SCHEMA_FAILED:
	case SD_SCHEMA_CANCELLED:
		fields |= EVENT_FIELD_BIT(EVENT_FIELD_SD_FAULT) |
		          EVENT_FIELD_BIT(EVENT_FIELD_SD_DRIVER_FAULT);
		break;
	case SD_SCHEMA_CONTEXT_COUNT:
		assert(0);
		break;
	}
	return fields;
}

static void test_sd_result_schema_unnamed_fields(void)
{
	static const unsigned expected_trials[] = {
		11, 10, 8, 10, 11, 9, 9
	};
	assert(SD_SCHEMA_CONTEXT_COUNT == 7);
	assert(ARRAY_LENGTH(expected_trials) == SD_SCHEMA_CONTEXT_COUNT);
	size_t valid_baselines = 0;
	size_t mutation_trials = 0;
	for (unsigned context = 0;
	     context < SD_SCHEMA_CONTEXT_COUNT; ++context) {
		{
			Fixture fixture;
			AiusHttpAction outstanding = reach_sd_schema_context(
				&fixture, (enum SdSchemaContext)context);
			AiusHttpAction action = call_step(
				&fixture, fixture.now,
				canonical_sd_schema_event(
					&outstanding,
					(enum SdSchemaContext)context));
			assert(fixture.transaction.outstanding_action_id !=
			       outstanding.action_id);
			if (context != SD_SCHEMA_CANCELLED)
				assert(fixture.transaction.last_fault !=
				       AIUS_HTTP_FAULT_PROTOCOL);
			else
				assert(fixture.transaction.last_fault ==
				       AIUS_HTTP_FAULT_PROTOCOL);
			(void)action;
			valid_baselines++;
		}
		size_t context_trials = 0;
		uint32_t named = sd_schema_named_fields(
			(enum SdSchemaContext)context);
		for (unsigned field = 0; field < EVENT_FIELD_COUNT; ++field) {
			if ((named & EVENT_FIELD_BIT(field)) != 0)
				continue;
			Fixture fixture;
			AiusHttpAction outstanding = reach_sd_schema_context(
				&fixture, (enum SdSchemaContext)context);
			AiusHttpEvent event = canonical_sd_schema_event(
				&outstanding, (enum SdSchemaContext)context);
			mutate_event_field(&event, (enum EventField)field);
			AiusHttpAction action =
				call_step(&fixture, fixture.now, event);
			assert_none_action(&action);
			assert(fixture.transaction.last_fault ==
			       AIUS_HTTP_FAULT_PROTOCOL);
			assert(fixture.transaction.outstanding_action ==
			       outstanding.kind);
			assert(fixture.transaction.outstanding_action_id ==
			       outstanding.action_id);
			context_trials++;
			mutation_trials++;
		}
		assert(context_trials == expected_trials[context]);
	}
	assert(valid_baselines == 7);
	assert(mutation_trials == 68);
}

enum FixedResponseCause {
	FIXED_BAD_REQUEST,
	FIXED_METHOD_NOT_ALLOWED,
	FIXED_NOT_FOUND,
	FIXED_REQUEST_TIMEOUT,
	FIXED_REQUEST_TOO_LARGE,
	FIXED_PROTOCOL,
	FIXED_RESPONSE_TOO_LARGE,
	FIXED_SD_NOT_FOUND,
	FIXED_SD_RANGE,
	FIXED_SD_UNAVAILABLE,
	FIXED_SD_CAPACITY,
	FIXED_SD_OVERFLOW,
	FIXED_SD_FAILURE,
	FIXED_RESPONSE_CAUSE_COUNT
};

struct FixedResponseCase {
	const char *key;
	unsigned status;
	const char *body;
};

static const struct FixedResponseCase fixed_response_cases[] = {
	{ "bad_request", 400, "{\"ok\":false,\"error\":\"bad_request\"}" },
	{ "method_not_allowed", 405,
	  "{\"ok\":false,\"error\":\"method_not_allowed\"}" },
	{ "not_found", 404, "{\"ok\":false,\"error\":\"not_found\"}" },
	{ "request_timeout", 408,
	  "{\"ok\":false,\"error\":\"request_timeout\"}" },
	{ "request_too_large", 413,
	  "{\"ok\":false,\"error\":\"request_too_large\"}" },
	{ "protocol", 400, "{\"ok\":false,\"error\":\"protocol\"}" },
	{ "response_too_large", 500,
	  "{\"ok\":false,\"error\":\"response_too_large\"}" },
	{ "sd_not_found", 404,
	  "{\"ok\":false,\"error\":\"sd_not_found\"}" },
	{ "sd_range", 416, "{\"ok\":false,\"error\":\"sd_range\"}" },
	{ "sd_unavailable", 503,
	  "{\"ok\":false,\"error\":\"sd_unavailable\"}" },
	{ "sd_capacity.capacity", 507,
	  "{\"ok\":false,\"error\":\"sd_capacity\"}" },
	{ "sd_capacity.overflow", 507,
	  "{\"ok\":false,\"error\":\"sd_capacity\"}" },
	{ "sd_failure", 500, "{\"ok\":false,\"error\":\"sd_failure\"}" },
};

static AiusHttpAction select_sd_fixed_response(
	Fixture *fixture, AiusSdFault fault)
{
	AiusHttpAction action =
		feed_request(fixture, "GET /sd/x HTTP/1.0\r\n\r\n");
	AiusHttpApplicationResponse response =
		sd_job(AIUS_SD_JOB_DOWNLOAD, 200,
		       "application/octet-stream", NULL, 0, 0);
	action = call_step(fixture, fixture->now,
	                   application_result(&action, &response));
	assert(action.kind == AIUS_HTTP_ACTION_SD_START);
	AiusHttpEvent failed = sd_result(&action, AIUS_HTTP_SD_FAILED);
	failed.sd_fault = fault;
	failed.sd_driver_fault = 0x72;
	return call_step(fixture, fixture->now, failed);
}

static AiusHttpAction select_fixed_response(
	Fixture *fixture, enum FixedResponseCause cause)
{
	switch (cause) {
	case FIXED_BAD_REQUEST:
		return feed_request(fixture, "BROKEN\r\n\r\n");
	case FIXED_METHOD_NOT_ALLOWED:
		return feed_request(
			fixture, "DELETE /api/status HTTP/1.0\r\n\r\n");
	case FIXED_NOT_FOUND:
		return feed_request(
			fixture, "GET /missing HTTP/1.0\r\n\r\n");
	case FIXED_REQUEST_TIMEOUT: {
		AiusHttpAction read = accept_client(fixture);
		(void)read;
		return call_step(fixture, 150, no_event());
	}
	case FIXED_REQUEST_TOO_LARGE: {
		AiusHttpAction read = accept_client(fixture);
		assert(read.length == 32);
		memset(read.read_buffer, 'x', read.length);
		return call_step(
			fixture, fixture->now,
			io_result(&read, AIUS_IO_PROGRESS, read.length, 0));
	}
	case FIXED_PROTOCOL: {
		AiusHttpAction read = accept_client(fixture);
		return call_step(
			fixture, fixture->now,
			io_result(&read, AIUS_IO_PROGRESS, 0, 0));
	}
	case FIXED_RESPONSE_TOO_LARGE: {
		/* Driven through an SD-job acknowledgement: an immediate body
		 * that overflows the response buffer now streams instead. */
		static uint8_t body[256];
		AiusHttpAction dispatch =
			feed_request(fixture,
			             "DELETE /sd/x HTTP/1.0\r\n\r\n");
		AiusHttpApplicationResponse response =
			sd_job(AIUS_SD_JOB_DELETE, 200, "application/json",
			       body, sizeof(body), 0);
		return call_step(
			fixture, fixture->now,
			application_result(&dispatch, &response));
	}
	case FIXED_SD_NOT_FOUND:
		return select_sd_fixed_response(fixture, AIUS_SD_FAULT_OPEN);
	case FIXED_SD_RANGE:
		return select_sd_fixed_response(fixture, AIUS_SD_FAULT_RANGE);
	case FIXED_SD_UNAVAILABLE:
		return select_sd_fixed_response(fixture, AIUS_SD_FAULT_MEDIA);
	case FIXED_SD_CAPACITY:
		return select_sd_fixed_response(
			fixture, AIUS_SD_FAULT_CAPACITY);
	case FIXED_SD_OVERFLOW:
		return select_sd_fixed_response(
			fixture, AIUS_SD_FAULT_OVERFLOW);
	case FIXED_SD_FAILURE:
		return select_sd_fixed_response(fixture, AIUS_SD_FAULT_IO);
	case FIXED_RESPONSE_CAUSE_COUNT:
		break;
	}
	assert(0);
	return (AiusHttpAction){ 0 };
}

static void test_fixed_response_capacity_registry(void)
{
	assert(FIXED_RESPONSE_CAUSE_COUNT == 13);
	assert(ARRAY_LENGTH(fixed_response_cases) ==
	       FIXED_RESPONSE_CAUSE_COUNT);
	static const int capacity_delta[] = { -1, 0, 1 };
	static const char *const capacity_keys[] = {
		"fit-1", "fit", "fit+1"
	};
	assert(ARRAY_LENGTH(capacity_keys) ==
	       ARRAY_LENGTH(capacity_delta));
	for (size_t i = 0; i < ARRAY_LENGTH(capacity_keys); ++i) {
		assert(capacity_keys[i][0] != '\0');
		for (size_t j = 0; j < i; ++j)
			assert(strcmp(capacity_keys[i],
			              capacity_keys[j]) != 0);
	}
	size_t rows = 0;
	size_t zero_emission = 0;
	size_t complete_emission = 0;
	for (unsigned cause = 0;
	     cause < FIXED_RESPONSE_CAUSE_COUNT; ++cause) {
		for (unsigned prior = 0; prior < cause; ++prior)
			assert(strcmp(fixed_response_cases[cause].key,
			              fixed_response_cases[prior].key) != 0);
		uint8_t expected[256];
		const struct FixedResponseCase *test_case =
			&fixed_response_cases[cause];
		size_t body_length = strlen(test_case->body);
		size_t fit = aius_http_write_response(
			expected, sizeof(expected), test_case->status,
			"application/json",
			(const uint8_t *)test_case->body, body_length, 0u);
		assert(fit > 1);
		assert(fit < sizeof(expected));
		for (size_t variant = 0;
		     variant < ARRAY_LENGTH(capacity_delta); ++variant) {
			size_t capacity =
				(size_t)((int64_t)fit +
				         capacity_delta[variant]);
			Fixture fixture;
			size_t request_capacity =
				cause == FIXED_REQUEST_TOO_LARGE
					? 32
					: sizeof(fixture.request);
			init_fixture_at(
				&fixture, request_capacity, capacity, 0);
			AiusHttpAction action = select_fixed_response(
				&fixture, (enum FixedResponseCause)cause);
			if (capacity < fit) {
				if (action.kind != AIUS_HTTP_ACTION_CLOSE ||
				    fixture.transaction.response_length != 0) {
					fprintf(stderr,
					        "fixed response mismatch: key=%s "
					        "variant=fit-1 action=%d length=%zu "
					        "fault=%d status=%u\n",
					        test_case->key, (int)action.kind,
					        fixture.transaction.response_length,
					        (int)fixture.transaction.last_fault,
					        fixture.transaction.last_status);
				}
				assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
				assert(fixture.transaction.response_length == 0);
				assert(fixture.transaction.response_offset == 0);
				finish_close(&fixture, action);
				zero_emission++;
			} else {
				if (action.kind != AIUS_HTTP_ACTION_WRITE ||
				    fixture.transaction.response_length != fit ||
				    memcmp(fixture.response, expected, fit) != 0) {
					fprintf(stderr,
					        "fixed response mismatch: key=%s "
					        "variant=%s action=%d length=%zu/%zu "
					        "fault=%d status=%u\n",
					        test_case->key,
					        capacity == fit ? "fit" : "fit+1",
					        (int)action.kind,
					        fixture.transaction.response_length,
					        fit,
					        (int)fixture.transaction.last_fault,
					        fixture.transaction.last_status);
				}
				assert(action.kind == AIUS_HTTP_ACTION_WRITE);
				assert(fixture.transaction.response_length == fit);
				assert(memcmp(fixture.response, expected, fit) == 0);
				assert(action.data == fixture.response);
				assert(action.length == fit);
				action = finish_all_writes(&fixture, action);
				finish_close(&fixture, action);
				complete_emission++;
			}
			rows++;
		}
	}
	assert(ARRAY_LENGTH(capacity_delta) == 3);
	assert(rows == 39);
	assert(zero_emission == 13);
	assert(complete_emission == 26);
}

static AiusHttpAction reach_sd_matrix_context(
	Fixture *fixture, enum SdMatrixContext context,
	AiusHttpSdEventKind upcoming_kind)
{
	init_fixture(fixture);
	switch (context) {
	case SD_MATRIX_START:
		return start_delete(fixture);
	case SD_MATRIX_ADVANCE_UPLOAD: {
		AiusHttpAction action = upcoming_kind == AIUS_HTTP_SD_COMPLETE
			? start_upload_supply(fixture)
			: start_upload_advance(fixture, false);
		if (upcoming_kind == AIUS_HTTP_SD_COMPLETE) {
			action = call_step(fixture, fixture->now,
			                   sd_result(&action, AIUS_HTTP_SD_PROGRESS));
			assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
			assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
		}
		return action;
	}
	case SD_MATRIX_ADVANCE_DOWNLOAD:
		return start_download(fixture, fixture->now);
	case SD_MATRIX_ADVANCE_OTHER:
		return start_other_advance(fixture);
	case SD_MATRIX_SUPPLY_UPLOAD:
		return start_upload_supply(fixture);
	case SD_MATRIX_ACK_DOWNLOAD:
		return start_download_ack(
			fixture, upcoming_kind == AIUS_HTTP_SD_COMPLETE ? 1 : 2);
	case SD_MATRIX_FAIL_DOWNLOAD:
		return start_download_fail(fixture);
	case SD_MATRIX_CANCEL:
		return start_sd_cancel(fixture);
	case SD_MATRIX_ADVANCE_CANCEL: {
		AiusHttpAction action = start_sd_cancel(fixture);
		action = call_step(fixture, fixture->now,
		                   sd_result(&action, AIUS_HTTP_SD_PROGRESS));
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
		return action;
	}
	case SD_MATRIX_CONTEXT_COUNT:
		break;
	}
	assert(0);
	return (AiusHttpAction){ 0 };
}

static AiusHttpEvent matrix_sd_event(const AiusHttpAction *action,
	                                 enum SdMatrixContext context,
	                                 AiusHttpSdEventKind kind)
{
	static const uint8_t byte = 0x7b;
	AiusHttpEvent event = no_event();
	event.kind = AIUS_HTTP_EVENT_SD_RESULT;
	event.action_id = action->action_id;
	event.completed_action = action->kind;
	event.sd_kind = kind;
	switch (kind) {
	case AIUS_HTTP_SD_STARTED:
		event.sd_capacity = 8;
		break;
	case AIUS_HTTP_SD_NEED_UPLOAD:
		event.sd_length = 1;
		break;
	case AIUS_HTTP_SD_DOWNLOAD_DATA:
		event.sd_data = &byte;
		event.sd_length = 1;
		event.sd_size = context == SD_MATRIX_ACK_DOWNLOAD ? 2 : 1;
		break;
	case AIUS_HTTP_SD_FAILED:
		event.sd_fault = AIUS_SD_FAULT_IO;
		event.sd_driver_fault = 1;
		break;
	case AIUS_HTTP_SD_EVENT_NONE:
	case AIUS_HTTP_SD_PROGRESS:
	case AIUS_HTTP_SD_COMPLETE:
	case AIUS_HTTP_SD_CANCELLED:
		break;
	}
	return event;
}

static void test_executable_sd_result_matrix(void)
{
	static const unsigned expected_valid_counts[] =
		{ 2, 4, 4, 3, 1, 4, 2, 2, 2 };
	assert(SD_MATRIX_CONTEXT_COUNT == 9);
	assert(ARRAY_LENGTH(expected_valid_counts) == SD_MATRIX_CONTEXT_COUNT);
	size_t invalid_trials = 0;
	size_t valid_trials = 0;
	for (unsigned context = 0; context < SD_MATRIX_CONTEXT_COUNT; ++context) {
		unsigned context_valid = 0;
		for (unsigned kind = AIUS_HTTP_SD_STARTED;
		     kind <= AIUS_HTTP_SD_CANCELLED; ++kind) {
			Fixture fixture;
			bool valid =
				(sd_valid_mask((enum SdMatrixContext)context) &
				 EVENT_FIELD_BIT(kind - 1u)) != 0;
			AiusHttpAction outstanding = reach_sd_matrix_context(
				&fixture, (enum SdMatrixContext)context,
				(AiusHttpSdEventKind)kind);
			assert(fixture.transaction.state ==
			       (context == SD_MATRIX_START
				        ? AIUS_HTTP_TX_SD_START
				        : AIUS_HTTP_TX_SD_TRANSFER));
			assert(fixture.transaction.outstanding_action ==
			       outstanding.kind);
			assert(fixture.transaction.outstanding_action_id ==
			       outstanding.action_id);
			AiusHttpAction action = call_step(
				&fixture, fixture.now,
				matrix_sd_event(&outstanding,
				                (enum SdMatrixContext)context,
				                (AiusHttpSdEventKind)kind));
			if (valid) {
				assert(fixture.transaction.last_fault !=
				       AIUS_HTTP_FAULT_PROTOCOL);
				assert(fixture.transaction.outstanding_action_id !=
				       outstanding.action_id);
				context_valid++;
				valid_trials++;
			} else {
				AiusHttpFault expected_fault =
					context >= SD_MATRIX_FAIL_DOWNLOAD
						? AIUS_HTTP_FAULT_TRANSPORT
						: AIUS_HTTP_FAULT_PROTOCOL;
				if (fixture.transaction.last_fault != expected_fault ||
				    action.kind != AIUS_HTTP_ACTION_NONE ||
				    fixture.transaction.outstanding_action_id !=
					    outstanding.action_id) {
					fprintf(stderr,
					        "SD matrix invalid mismatch: context=%u "
					        "kind=%u fault=%d action=%d outstanding=%d "
					        "id=%llu expected-id=%llu\n",
					        context, kind,
					        (int)fixture.transaction.last_fault,
					        (int)action.kind,
					        (int)fixture.transaction.outstanding_action,
					        (unsigned long long)
						        fixture.transaction.outstanding_action_id,
					        (unsigned long long)outstanding.action_id);
				}
				assert_none_action(&action);
				assert(fixture.transaction.last_fault ==
				       expected_fault);
				assert(fixture.transaction.outstanding_action ==
				       outstanding.kind);
				assert(fixture.transaction.outstanding_action_id ==
				       outstanding.action_id);
				invalid_trials++;
			}
		}
		assert(context_valid == expected_valid_counts[context]);
	}
	assert(valid_trials == 24);
	assert(invalid_trials == 39);
	assert(valid_trials + invalid_trials == 63);
}

static void test_ack_download_deadline_cases(void)
{
	size_t trials = 0;
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction ack = start_download_ack(&fixture, 1);
		assert(fixture.transaction.deadline_ms == 10000);
		AiusHttpAction action = call_step(&fixture, 100, no_event());
		assert_none_action(&action);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
		assert(fixture.transaction.outstanding_action_id == ack.action_id);
		trials++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction ack = start_download_ack(&fixture, 1);
		assert(fixture.transaction.deadline_ms == 10000);
		AiusHttpAction action = call_step(&fixture, 10000, no_event());
		assert_none_action(&action);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_DOWNLOAD_TIMEOUT);
		assert(fixture.transaction.outstanding_action_id == ack.action_id);
		trials++;
	}
	{
		Fixture fixture;
		init_fixture_at(&fixture, sizeof(fixture.request),
		                sizeof(fixture.response), UINT32_MAX - 50u);
		AiusHttpAction ack = start_download_ack(&fixture, 1);
		assert(fixture.transaction.deadline_ms == UINT32_C(9949));
		AiusHttpAction before = call_step(&fixture, UINT32_C(9948), no_event());
		assert_none_action(&before);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
		AiusHttpAction action =
			call_step(&fixture, UINT32_C(9949), no_event());
		assert_none_action(&action);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_DOWNLOAD_TIMEOUT);
		assert(fixture.transaction.outstanding_action_id == ack.action_id);
		trials++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction ack = start_download_ack(&fixture, 1);
		AiusHttpAction action = call_step(
			&fixture, 10000,
			matrix_sd_event(&ack, SD_MATRIX_ACK_DOWNLOAD,
			                AIUS_HTTP_SD_COMPLETE));
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
		assert(fixture.transaction.outstanding_action_id != ack.action_id);
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		trials++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction ack = start_download_ack(&fixture, 2);
		AiusHttpAction action = call_step(
			&fixture, 10000,
			matrix_sd_event(&ack, SD_MATRIX_ACK_DOWNLOAD,
			                AIUS_HTTP_SD_PROGRESS));
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
		assert(fixture.transaction.outstanding_action_id != ack.action_id);
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		assert(fixture.transaction.deadline_ms == 20000);
		trials++;
	}
	assert(trials == 5);
}

enum AuthorityContext {
	AUTHORITY_FREE_INITIAL,
	AUTHORITY_FREE_ACCEPT_RETRY,
	AUTHORITY_FREE_RESTART,
	AUTHORITY_ACCEPT,
	AUTHORITY_READ_ORDINARY,
	AUTHORITY_READ_UPLOAD,
	AUTHORITY_DISPATCH,
	AUTHORITY_WRITE,
	AUTHORITY_SD_START,
	AUTHORITY_SD_ADVANCE_UPLOAD,
	AUTHORITY_SD_ADVANCE_DOWNLOAD,
	AUTHORITY_SD_ADVANCE_OTHER,
	AUTHORITY_SD_SUPPLY_UPLOAD,
	AUTHORITY_SD_ACK_DOWNLOAD,
	AUTHORITY_SD_FAIL_DOWNLOAD,
	AUTHORITY_SD_CANCEL,
	AUTHORITY_SD_ADVANCE_CANCEL,
	AUTHORITY_CLOSE,
	AUTHORITY_CONTEXT_COUNT
};

static AiusHttpAction start_immediate_write(Fixture *fixture);

struct AuthorityExpectation {
	AiusHttpFault fault;
	AiusHttpActionKind action;
	bool retain;
};

static AiusHttpAction authority_free_action(void)
{
	AiusHttpAction action;
	memset(&action, 0, sizeof(action));
	return action;
}

static AiusHttpAction reach_authority_context(
	Fixture *fixture, enum AuthorityContext context)
{
	init_fixture(fixture);
	switch (context) {
	case AUTHORITY_FREE_INITIAL:
		return authority_free_action();
	case AUTHORITY_FREE_ACCEPT_RETRY: {
		AiusHttpAction action = call_step(fixture, 0, no_event());
		assert(action.kind == AIUS_HTTP_ACTION_ACCEPT);
		action = call_step(
			fixture, 0,
			io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
		assert_none_action(&action);
		return action;
	}
	case AUTHORITY_FREE_RESTART: {
		AiusHttpAction action = start_immediate_write(fixture);
		action = finish_all_writes(fixture, action);
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		finish_close(fixture, action);
		return authority_free_action();
	}
	case AUTHORITY_ACCEPT:
		return call_step(fixture, 0, no_event());
	case AUTHORITY_READ_ORDINARY:
		return accept_client(fixture);
	case AUTHORITY_READ_UPLOAD:
		return start_upload_read(fixture);
	case AUTHORITY_DISPATCH:
		return feed_request(
			fixture, "GET /api/status HTTP/1.0\r\n\r\n");
	case AUTHORITY_WRITE:
		return start_immediate_write(fixture);
	case AUTHORITY_SD_START:
		return start_delete(fixture);
	case AUTHORITY_SD_ADVANCE_UPLOAD:
		return start_upload_advance(fixture, false);
	case AUTHORITY_SD_ADVANCE_DOWNLOAD:
		return start_download(fixture, fixture->now);
	case AUTHORITY_SD_ADVANCE_OTHER:
		return start_other_advance(fixture);
	case AUTHORITY_SD_SUPPLY_UPLOAD:
		return start_upload_supply(fixture);
	case AUTHORITY_SD_ACK_DOWNLOAD:
		return start_download_ack(fixture, 2);
	case AUTHORITY_SD_FAIL_DOWNLOAD:
		return start_download_fail(fixture);
	case AUTHORITY_SD_CANCEL:
		return start_sd_cancel(fixture);
	case AUTHORITY_SD_ADVANCE_CANCEL: {
		AiusHttpAction action = start_sd_cancel(fixture);
		action = call_step(fixture, fixture->now,
		                   sd_result(&action, AIUS_HTTP_SD_PROGRESS));
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		assert(action.sd_operation == AIUS_HTTP_SD_OP_ADVANCE);
		return action;
	}
	case AUTHORITY_CLOSE: {
		AiusHttpAction action = start_immediate_write(fixture);
		action = finish_all_writes(fixture, action);
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		return action;
	}
	case AUTHORITY_CONTEXT_COUNT:
		break;
	}
	assert(0);
	return authority_free_action();
}

static void assert_reached_authority(
	const Fixture *fixture, enum AuthorityContext context,
	const AiusHttpAction *outstanding)
{
	AiusHttpTransactionState expected_state;
	AiusHttpActionKind expected_action;
	switch (context) {
	case AUTHORITY_FREE_INITIAL:
	case AUTHORITY_FREE_ACCEPT_RETRY:
	case AUTHORITY_FREE_RESTART:
		expected_state = AIUS_HTTP_TX_IDLE;
		expected_action = AIUS_HTTP_ACTION_NONE;
		break;
	case AUTHORITY_ACCEPT:
		expected_state = AIUS_HTTP_TX_IDLE;
		expected_action = AIUS_HTTP_ACTION_ACCEPT;
		break;
	case AUTHORITY_READ_ORDINARY:
		expected_state = AIUS_HTTP_TX_RECEIVE;
		expected_action = AIUS_HTTP_ACTION_READ;
		break;
	case AUTHORITY_READ_UPLOAD:
		expected_state = AIUS_HTTP_TX_SD_TRANSFER;
		expected_action = AIUS_HTTP_ACTION_READ;
		break;
	case AUTHORITY_DISPATCH:
		expected_state = AIUS_HTTP_TX_DISPATCH;
		expected_action = AIUS_HTTP_ACTION_DISPATCH;
		break;
	case AUTHORITY_WRITE:
		expected_state = AIUS_HTTP_TX_SEND;
		expected_action = AIUS_HTTP_ACTION_WRITE;
		break;
	case AUTHORITY_SD_START:
		expected_state = AIUS_HTTP_TX_SD_START;
		expected_action = AIUS_HTTP_ACTION_SD_START;
		break;
	case AUTHORITY_SD_ADVANCE_UPLOAD:
	case AUTHORITY_SD_ADVANCE_DOWNLOAD:
	case AUTHORITY_SD_ADVANCE_OTHER:
	case AUTHORITY_SD_SUPPLY_UPLOAD:
	case AUTHORITY_SD_ACK_DOWNLOAD:
	case AUTHORITY_SD_FAIL_DOWNLOAD:
	case AUTHORITY_SD_ADVANCE_CANCEL:
		expected_state = AIUS_HTTP_TX_SD_TRANSFER;
		expected_action = AIUS_HTTP_ACTION_SD_STEP;
		break;
	case AUTHORITY_SD_CANCEL:
		expected_state = AIUS_HTTP_TX_SD_TRANSFER;
		expected_action = AIUS_HTTP_ACTION_SD_CANCEL;
		break;
	case AUTHORITY_CLOSE:
		expected_state = AIUS_HTTP_TX_CLOSE;
		expected_action = AIUS_HTTP_ACTION_CLOSE;
		break;
	case AUTHORITY_CONTEXT_COUNT:
		assert(0);
		return;
	}
	assert(fixture->transaction.state == expected_state);
	assert(fixture->transaction.outstanding_action == expected_action);
	assert(outstanding->kind == expected_action);
	assert(fixture->transaction.outstanding_action_id ==
	       outstanding->action_id);
	switch (context) {
	case AUTHORITY_SD_START:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_WAIT_START);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_DELETE);
		break;
	case AUTHORITY_SD_ADVANCE_UPLOAD:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_UPLOAD);
		assert(outstanding->sd_operation ==
		       AIUS_HTTP_SD_OP_ADVANCE);
		break;
	case AUTHORITY_SD_ADVANCE_DOWNLOAD:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_DOWNLOAD_ADVANCE);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_DOWNLOAD);
		assert(outstanding->sd_operation ==
		       AIUS_HTTP_SD_OP_ADVANCE);
		break;
	case AUTHORITY_SD_ADVANCE_OTHER:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_WAIT_TERMINAL);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_DELETE);
		assert(outstanding->sd_operation ==
		       AIUS_HTTP_SD_OP_ADVANCE);
		break;
	case AUTHORITY_SD_SUPPLY_UPLOAD:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_UPLOAD);
		assert(outstanding->sd_operation ==
		       AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
		break;
	case AUTHORITY_SD_ACK_DOWNLOAD:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_DOWNLOAD_ACK);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_DOWNLOAD);
		assert(outstanding->sd_operation ==
		       AIUS_HTTP_SD_OP_ACK_DOWNLOAD);
		break;
	case AUTHORITY_SD_FAIL_DOWNLOAD:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_CANCEL);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_DOWNLOAD);
		assert(outstanding->sd_operation ==
		       AIUS_HTTP_SD_OP_FAIL_DOWNLOAD);
		break;
	case AUTHORITY_SD_CANCEL:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_CANCEL);
		break;
	case AUTHORITY_SD_ADVANCE_CANCEL:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_CANCEL);
		assert(outstanding->sd_operation ==
		       AIUS_HTTP_SD_OP_ADVANCE);
		break;
	case AUTHORITY_FREE_INITIAL:
	case AUTHORITY_FREE_ACCEPT_RETRY:
	case AUTHORITY_FREE_RESTART:
	case AUTHORITY_ACCEPT:
	case AUTHORITY_READ_ORDINARY:
	case AUTHORITY_DISPATCH:
	case AUTHORITY_WRITE:
	case AUTHORITY_CLOSE:
	case AUTHORITY_CONTEXT_COUNT:
		break;
	case AUTHORITY_READ_UPLOAD:
		assert(fixture->transaction.sd_phase ==
		       AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT);
		assert(fixture->transaction.sd_request.kind ==
		       AIUS_SD_JOB_UPLOAD);
		assert(fixture->transaction.outstanding_length == 3);
		assert(fixture->transaction.sd_capacity == 4);
		assert(fixture->transaction.body_expected == 5);
		assert(fixture->transaction.upload_supplied == 0);
		assert(fixture->transaction.deadline_ms == 2000);
		break;
	}
}

static bool is_sd_context(enum AuthorityContext context);

static AiusHttpEvent authority_event(
	enum AuthorityContext context, AiusHttpEventKind kind,
	const AiusHttpAction *outstanding)
{
	static const uint8_t body[] = "{}";
	static const AiusHttpApplicationResponse response = {
		.kind = AIUS_HTTP_APP_IMMEDIATE,
		.status = 200,
		.content_type = "application/json",
		.body = body,
		.body_length = sizeof(body) - 1u
	};
	AiusHttpEvent event = no_event();
	event.kind = kind;
	switch (kind) {
	case AIUS_HTTP_EVENT_NONE:
		break;
	case AIUS_HTTP_EVENT_IO_RESULT:
		event.action_id =
			outstanding->action_id != 0 ? outstanding->action_id : 1;
		if (outstanding->kind == AIUS_HTTP_ACTION_ACCEPT ||
		    outstanding->kind == AIUS_HTTP_ACTION_READ ||
		    outstanding->kind == AIUS_HTTP_ACTION_WRITE ||
		    outstanding->kind == AIUS_HTTP_ACTION_CLOSE) {
			event.completed_action = outstanding->kind;
			event.io.result =
				outstanding->kind == AIUS_HTTP_ACTION_ACCEPT ||
				outstanding->kind == AIUS_HTTP_ACTION_CLOSE
					? AIUS_IO_PROGRESS
					: AIUS_IO_WOULD_BLOCK;
		} else {
			event.completed_action = AIUS_HTTP_ACTION_READ;
			event.io.result = AIUS_IO_WOULD_BLOCK;
		}
		break;
	case AIUS_HTTP_EVENT_APPLICATION_RESPONSE:
		event.action_id =
			outstanding->action_id != 0 ? outstanding->action_id : 1;
		event.completed_action = AIUS_HTTP_ACTION_DISPATCH;
		event.application_response = &response;
		break;
	case AIUS_HTTP_EVENT_SD_RESULT:
		event.action_id =
			outstanding->action_id != 0 ? outstanding->action_id : 1;
		event.completed_action =
			is_sd_context(context)
				? outstanding->kind
				: AIUS_HTTP_ACTION_SD_STEP;
		event.sd_kind = outstanding->kind == AIUS_HTTP_ACTION_SD_START
			? AIUS_HTTP_SD_STARTED
			: AIUS_HTTP_SD_PROGRESS;
		if (event.sd_kind == AIUS_HTTP_SD_STARTED)
			event.sd_capacity = 8;
		break;
	case AIUS_HTTP_EVENT_PEER_CLOSED:
		break;
	case AIUS_HTTP_EVENT_PEER_FAULT:
		event.peer_fault = 0x71;
		break;
	}
	return event;
}

static bool is_free_context(enum AuthorityContext context)
{
	return context <= AUTHORITY_FREE_RESTART;
}

static bool is_sd_context(enum AuthorityContext context)
{
	return context >= AUTHORITY_SD_START &&
	       context <= AUTHORITY_SD_ADVANCE_CANCEL;
}

static bool is_cleanup_sd_context(enum AuthorityContext context)
{
	return context >= AUTHORITY_SD_FAIL_DOWNLOAD &&
	       context <= AUTHORITY_SD_ADVANCE_CANCEL;
}

static struct AuthorityExpectation authority_expectation(
	enum AuthorityContext context, AiusHttpEventKind event)
{
	struct AuthorityExpectation expected = {
		.fault = is_cleanup_sd_context(context)
			? AIUS_HTTP_FAULT_TRANSPORT
			: AIUS_HTTP_FAULT_PROTOCOL,
		.action = AIUS_HTTP_ACTION_NONE,
		.retain = false
	};
	if (is_free_context(context)) {
		if (event == AIUS_HTTP_EVENT_NONE) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.action = AIUS_HTTP_ACTION_ACCEPT;
		}
		return expected;
	}
	if (context == AUTHORITY_ACCEPT) {
		if (event == AIUS_HTTP_EVENT_NONE) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.retain = true;
		} else if (event == AIUS_HTTP_EVENT_IO_RESULT) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.action = AIUS_HTTP_ACTION_READ;
		}
		return expected;
	}
	if (context == AUTHORITY_READ_ORDINARY ||
	    context == AUTHORITY_READ_UPLOAD) {
		if (event == AIUS_HTTP_EVENT_NONE) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.retain = true;
		} else if (event == AIUS_HTTP_EVENT_IO_RESULT) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.action = AIUS_HTTP_ACTION_READ;
		} else if (event == AIUS_HTTP_EVENT_PEER_CLOSED ||
		           event == AIUS_HTTP_EVENT_PEER_FAULT) {
			expected.fault = AIUS_HTTP_FAULT_TRANSPORT;
			expected.action = AIUS_HTTP_ACTION_CLOSE;
		} else {
			expected.action = AIUS_HTTP_ACTION_WRITE;
		}
		return expected;
	}
	if (context == AUTHORITY_DISPATCH) {
		if (event == AIUS_HTTP_EVENT_APPLICATION_RESPONSE) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.action = AIUS_HTTP_ACTION_WRITE;
		} else if (event == AIUS_HTTP_EVENT_PEER_CLOSED ||
		           event == AIUS_HTTP_EVENT_PEER_FAULT) {
			expected.fault = AIUS_HTTP_FAULT_TRANSPORT;
			expected.retain = true;
		} else {
			expected.action = AIUS_HTTP_ACTION_WRITE;
		}
		return expected;
	}
	if (context == AUTHORITY_WRITE) {
		if (event == AIUS_HTTP_EVENT_NONE) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.retain = true;
		} else if (event == AIUS_HTTP_EVENT_IO_RESULT) {
			expected.fault = AIUS_HTTP_FAULT_NONE;
			expected.action = AIUS_HTTP_ACTION_WRITE;
		} else if (event == AIUS_HTTP_EVENT_PEER_CLOSED ||
		           event == AIUS_HTTP_EVENT_PEER_FAULT) {
			expected.fault = AIUS_HTTP_FAULT_TRANSPORT;
			expected.action = AIUS_HTTP_ACTION_CLOSE;
		} else {
			expected.action = AIUS_HTTP_ACTION_WRITE;
		}
		return expected;
	}
	if (is_sd_context(context)) {
		if (event == AIUS_HTTP_EVENT_SD_RESULT) {
			expected.action =
				context == AUTHORITY_SD_FAIL_DOWNLOAD
					? AIUS_HTTP_ACTION_CLOSE
					: AIUS_HTTP_ACTION_SD_STEP;
			expected.fault = is_cleanup_sd_context(context)
				? AIUS_HTTP_FAULT_TRANSPORT
				: AIUS_HTTP_FAULT_NONE;
		} else {
			expected.retain = true;
			if (event == AIUS_HTTP_EVENT_NONE &&
			    context != AUTHORITY_SD_START)
				expected.fault = is_cleanup_sd_context(context)
					? AIUS_HTTP_FAULT_TRANSPORT
					: AIUS_HTTP_FAULT_NONE;
			if (event == AIUS_HTTP_EVENT_PEER_CLOSED ||
			    event == AIUS_HTTP_EVENT_PEER_FAULT)
				expected.fault = AIUS_HTTP_FAULT_TRANSPORT;
		}
		return expected;
	}
	assert(context == AUTHORITY_CLOSE);
	if (event == AIUS_HTTP_EVENT_NONE) {
		expected.fault = AIUS_HTTP_FAULT_NONE;
		expected.retain = true;
	} else if (event == AIUS_HTTP_EVENT_IO_RESULT) {
		expected.fault = AIUS_HTTP_FAULT_NONE;
	} else if (event == AIUS_HTTP_EVENT_PEER_CLOSED ||
	           event == AIUS_HTTP_EVENT_PEER_FAULT) {
		expected.fault = AIUS_HTTP_FAULT_TRANSPORT;
	}
	return expected;
}

static void test_authority_event_equivalence_registry(void)
{
	size_t rows = 0;
	size_t retained = 0;
	size_t consumed = 0;
	size_t clean = 0;
	size_t protocol = 0;
	size_t transport = 0;
	for (unsigned context = 0; context < AUTHORITY_CONTEXT_COUNT; ++context) {
		for (unsigned kind = AIUS_HTTP_EVENT_NONE;
		     kind <= AIUS_HTTP_EVENT_PEER_FAULT; ++kind) {
			if (context == AUTHORITY_READ_UPLOAD &&
			    kind == AIUS_HTTP_EVENT_IO_RESULT)
				continue;
			Fixture fixture;
			AiusHttpAction outstanding = reach_authority_context(
				&fixture, (enum AuthorityContext)context);
			assert_reached_authority(
				&fixture, (enum AuthorityContext)context,
				&outstanding);
			struct AuthorityExpectation expected =
				authority_expectation(
					(enum AuthorityContext)context,
					(AiusHttpEventKind)kind);
			AiusHttpEvent event = authority_event(
				(enum AuthorityContext)context,
				(AiusHttpEventKind)kind, &outstanding);
			AiusHttpAction action =
				call_step(&fixture, fixture.now, event);
			if (fixture.transaction.last_fault != expected.fault ||
			    action.kind != expected.action ||
			    (expected.retain &&
			     fixture.transaction.outstanding_action_id !=
				     outstanding.action_id) ||
			    (!expected.retain && outstanding.action_id != 0 &&
			     fixture.transaction.outstanding_action_id ==
				     outstanding.action_id)) {
				fprintf(stderr,
				        "authority registry mismatch: context=%u "
				        "event=%u fault=%d/%d action=%d/%d "
				        "outstanding=%llu prior=%llu retain=%d\n",
				        context, kind,
				        (int)fixture.transaction.last_fault,
				        (int)expected.fault, (int)action.kind,
				        (int)expected.action,
				        (unsigned long long)
					        fixture.transaction.outstanding_action_id,
				        (unsigned long long)outstanding.action_id,
				        expected.retain);
			}
			assert(fixture.transaction.last_fault == expected.fault);
			assert(action.kind == expected.action);
			if (expected.retain) {
				assert(fixture.transaction.outstanding_action ==
				       outstanding.kind);
				assert(fixture.transaction.outstanding_action_id ==
				       outstanding.action_id);
				retained++;
			} else {
				assert(outstanding.action_id == 0 ||
				       fixture.transaction.outstanding_action_id !=
					       outstanding.action_id);
				consumed++;
			}
			switch (expected.fault) {
			case AIUS_HTTP_FAULT_NONE: clean++; break;
			case AIUS_HTTP_FAULT_PROTOCOL: protocol++; break;
			case AIUS_HTTP_FAULT_TRANSPORT: transport++; break;
			default: assert(0); break;
			}
			rows++;
		}
	}
	struct UploadReadIoVariant {
		const char *key;
		AiusIoResult result;
		size_t bytes;
		uint32_t driver_fault;
		AiusHttpFault fault;
		AiusHttpActionKind action;
	};
	static const struct UploadReadIoVariant upload_read_io[] = {
		{ "would-block", AIUS_IO_WOULD_BLOCK, 0, 0,
		  AIUS_HTTP_FAULT_NONE, AIUS_HTTP_ACTION_READ },
		{ "progress", AIUS_IO_PROGRESS, 3, 0,
		  AIUS_HTTP_FAULT_NONE, AIUS_HTTP_ACTION_SD_STEP },
		{ "closed", AIUS_IO_CLOSED, 0, 0,
		  AIUS_HTTP_FAULT_TRANSPORT, AIUS_HTTP_ACTION_CLOSE },
		{ "fault", AIUS_IO_FAULT, 0, 0x61,
		  AIUS_HTTP_FAULT_TRANSPORT, AIUS_HTTP_ACTION_CLOSE },
	};
	size_t upload_read_rows = 0;
	for (size_t i = 0; i < ARRAY_LENGTH(upload_read_io); ++i) {
		for (size_t j = 0; j < i; ++j)
			assert(strcmp(upload_read_io[i].key,
			              upload_read_io[j].key) != 0);
		Fixture fixture;
		AiusHttpAction read =
			reach_authority_context(&fixture, AUTHORITY_READ_UPLOAD);
		assert_reached_authority(
			&fixture, AUTHORITY_READ_UPLOAD, &read);
		if (upload_read_io[i].result == AIUS_IO_PROGRESS)
			memcpy(read.read_buffer, "abc", 3);
		AiusHttpAction action = call_step(
			&fixture, fixture.now,
			io_result(&read, upload_read_io[i].result,
			          upload_read_io[i].bytes,
			          upload_read_io[i].driver_fault));
		assert(fixture.transaction.last_fault ==
		       upload_read_io[i].fault);
		assert(action.kind == upload_read_io[i].action);
		assert(fixture.transaction.outstanding_action_id !=
		       read.action_id);
		if (upload_read_io[i].result == AIUS_IO_WOULD_BLOCK) {
			assert(action.read_buffer == read.read_buffer);
			assert(action.length == read.length);
		} else if (upload_read_io[i].result == AIUS_IO_PROGRESS) {
			assert(action.sd_operation ==
			       AIUS_HTTP_SD_OP_SUPPLY_UPLOAD);
			assert(action.length == 3);
			assert(memcmp(action.data, "abc", 3) == 0);
		}
		consumed++;
		if (upload_read_io[i].fault == AIUS_HTTP_FAULT_NONE)
			clean++;
		else
			transport++;
		rows++;
		upload_read_rows++;
	}
	assert(AUTHORITY_CONTEXT_COUNT == 18);
	assert(upload_read_rows == 4);
	assert(rows == 111);
	assert(retained == 52);
	assert(consumed == 59);
	assert(clean == 26);
	assert(protocol == 43);
	assert(transport == 42);
}

static AiusHttpAction start_immediate_write(Fixture *fixture)
{
	static const uint8_t body[] = "{}";
	AiusHttpAction dispatch =
		feed_request(fixture, "GET /api/status HTTP/1.0\r\n\r\n");
	AiusHttpApplicationResponse response =
		immediate(200, "application/json", body, sizeof(body) - 1u);
	AiusHttpAction action = call_step(
		fixture, fixture->now, application_result(&dispatch, &response));
	assert(action.kind == AIUS_HTTP_ACTION_WRITE);
	return action;
}

static void test_engine_driven_precedence_registry(void)
{
	size_t rows = 0;
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction read = accept_client(&fixture);
		AiusHttpEvent malformed =
			io_result(&read, AIUS_IO_PROGRESS, 0, 0);
		AiusHttpAction action = call_step(&fixture, 150, malformed);
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_PROTOCOL);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction read = accept_client(&fixture);
		*read.read_buffer = 'G';
		AiusHttpAction action = call_step(
			&fixture, 150,
			io_result(&read, AIUS_IO_PROGRESS, 1, 0));
		assert(action.kind == AIUS_HTTP_ACTION_READ);
		assert(fixture.transaction.last_fault == AIUS_HTTP_FAULT_NONE);
		assert(fixture.transaction.deadline_ms == 300);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction read = accept_client(&fixture);
		AiusHttpAction action = call_step(
			&fixture, 150,
			io_result(&read, AIUS_IO_CLOSED, 0, 0));
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_TRANSPORT);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction write = start_immediate_write(&fixture);
		AiusHttpAction action = call_step(
			&fixture, fixture.now,
			io_result(&write, AIUS_IO_CLOSED, 0, 0));
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_TRANSPORT);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction write = start_immediate_write(&fixture);
		assert(write.kind == AIUS_HTTP_ACTION_WRITE);
		AiusHttpAction action =
			call_step(&fixture, fixture.now, (AiusHttpEvent){
				.kind = AIUS_HTTP_EVENT_PEER_CLOSED
			});
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_TRANSPORT);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction read = accept_client(&fixture);
		AiusHttpAction close = call_step(
			&fixture, 0,
			io_result(&read, AIUS_IO_FAULT, 0, 0x11));
		assert(close.kind == AIUS_HTTP_ACTION_CLOSE);
		AiusHttpAction action = call_step(
			&fixture, 0,
			io_result(&close, AIUS_IO_FAULT, 0, 0x22));
		assert_none_action(&action);
		assert(fixture.transaction.last_transport_fault == 0x11);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_TRANSPORT);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction write = start_immediate_write(&fixture);
		AiusHttpAction close = finish_all_writes(&fixture, write);
		assert(close.kind == AIUS_HTTP_ACTION_CLOSE);
		AiusHttpAction action = call_step(
			&fixture, 0,
			io_result(&close, AIUS_IO_FAULT, 0, 0x22));
		assert_none_action(&action);
		assert(fixture.transaction.last_transport_fault == 0x22);
		assert(fixture.transaction.last_status == 200);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpAction action =
			call_step(&fixture, 0, peer_fault(0x33));
		assert_none_action(&action);
		assert(fixture.transaction.outstanding_action ==
		       AIUS_HTTP_ACTION_DISPATCH);
		static const uint8_t body[] = "{}";
		AiusHttpApplicationResponse malformed =
			sd_job(AIUS_SD_JOB_REPLAY_REFILL, 200,
			       "application/json", body, sizeof(body) - 1u, 1);
		action = call_step(
			&fixture, 0,
			application_result(&dispatch, &malformed));
		assert(action.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_TRANSPORT);
		assert(fixture.transaction.last_transport_fault == 0x33);
		assert(!fixture.transaction.sd_started);
		rows++;
	}
	assert(rows == 8);
}

static void test_multiple_restart_registry(void)
{
	Fixture fixture;
	init_fixture(&fixture);
	uint64_t prior_id = 0;
	size_t cycles = 0;
	for (unsigned cycle = 0; cycle < 3; ++cycle) {
		AiusHttpAction write = start_immediate_write(&fixture);
		assert(write.action_id > prior_id);
		prior_id = write.action_id;
		AiusHttpAction close = finish_all_writes(&fixture, write);
		assert(close.kind == AIUS_HTTP_ACTION_CLOSE);
		assert(close.action_id > prior_id);
		prior_id = close.action_id;
		finish_close(&fixture, close);
		assert(fixture.transaction.state == AIUS_HTTP_TX_IDLE);
		cycles++;
	}
	assert(cycles == 3);
	assert(fixture.dispatches == 3);
	assert(fixture.closes == 3);
}

static void test_duplicate_authority_registry(void)
{
	size_t rows = 0;
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction dispatch =
			feed_request(&fixture, "GET /api/status HTTP/1.0\r\n\r\n");
		AiusHttpAction action = call_step(&fixture, 0, no_event());
		assert(action.kind == AIUS_HTTP_ACTION_WRITE);
		assert(fixture.dispatches == 1);
		assert(fixture.transaction.last_fault ==
		       AIUS_HTTP_FAULT_PROTOCOL);
		(void)dispatch;
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction close =
			finish_all_writes(&fixture, start_immediate_write(&fixture));
		assert(close.kind == AIUS_HTTP_ACTION_CLOSE);
		AiusHttpAction action = call_step(&fixture, 0, no_event());
		assert_none_action(&action);
		assert(fixture.closes == 1);
		action = call_step(
			&fixture, 0,
			io_result(&close, AIUS_IO_PROGRESS, 0, 0));
		assert_none_action(&action);
		assert(fixture.closes == 1);
		rows++;
	}
	{
		Fixture fixture;
		init_fixture(&fixture);
		AiusHttpAction cancel = start_sd_cancel(&fixture);
		fixture.cancels = 1;
		AiusHttpAction action = call_step(&fixture, 0, no_event());
		assert_none_action(&action);
		assert(fixture.cancels == 1);
		action = call_step(
			&fixture, 0,
			matrix_sd_event(&cancel, SD_MATRIX_CANCEL,
			                AIUS_HTTP_SD_PROGRESS));
		assert(action.kind == AIUS_HTTP_ACTION_SD_STEP);
		assert(fixture.cancels == 1);
		rows++;
	}
	assert(rows == 3);
}

/* Chunk-boundary equivalence.
 *
 * TCP may split a request at any byte, and every mainstream client library
 * (python http.client, requests, Go, .NET, Java) writes headers and body as
 * separate writes; curl's coalesced small POST -- the shape every manual test
 * used -- is the exception, not the rule. So the transaction must accumulate
 * across epochs until the declared body is complete and answer identically
 * wherever the boundaries fall. The feed below reports WOULD_BLOCK for a read
 * that has nothing yet, which is what an empty non-blocking socket means; an
 * adapter that reports it as CLOSED instead makes the transaction discard the
 * response and close silently on every split request. */
typedef struct {
	uint8_t bytes[AIUS_HTTP_MAX_RESPONSE];
	size_t length;
} ObservedResponse;

/* Delivers `raw` as `count` chunks ending at the offsets in `bounds`, one
 * WOULD_BLOCK per boundary, and records every response byte the client would
 * see. */
static void feed_chunked(Fixture *fixture, const char *raw, size_t length,
	                     const char *expect_body, size_t expect_body_length,
	                     const size_t *bounds, size_t count,
	                     ObservedResponse *observed)
{
	static const uint8_t reply_body[] =
		"{\"ok\":true,\"state\":\"prebuffering\"}";
	AiusHttpApplicationResponse reply =
		immediate(200u, "application/json", reply_body,
		          sizeof(reply_body) - 1u);
	AiusHttpAction action = accept_client(fixture);
	size_t offset = 0;
	size_t chunk = 0;
	size_t chunk_end = bounds[0];

	memset(observed, 0, sizeof(*observed));
	while (action.kind == AIUS_HTTP_ACTION_READ) {
		size_t available = chunk_end - offset;
		size_t take = available < action.length ? available : action.length;
		if (take == 0u) {
			/* The transaction may only ask for more once a boundary
			 * is still short of the declared body. */
			assert(chunk + 1u < count);
			chunk_end = bounds[++chunk];
			action = call_step(
				fixture, fixture->now,
				io_result(&action, AIUS_IO_WOULD_BLOCK, 0, 0));
			continue;
		}
		memcpy(action.read_buffer, raw + offset, take);
		offset += take;
		action = call_step(fixture, fixture->now,
		                   io_result(&action, AIUS_IO_PROGRESS, take, 0));
	}
	assert(offset == length);
	assert(action.kind == AIUS_HTTP_ACTION_DISPATCH);
	assert(action.request->body_complete);
	assert(action.request->body_length == expect_body_length);
	assert(memcmp(action.request->body, expect_body, expect_body_length) == 0);

	action = call_step(fixture, fixture->now,
	                   application_result(&action, &reply));
	while (action.kind == AIUS_HTTP_ACTION_WRITE) {
		assert(observed->length + action.length <=
		       sizeof(observed->bytes));
		memcpy(observed->bytes + observed->length, action.data,
		       action.length);
		observed->length += action.length;
		action = call_step(
			fixture, fixture->now,
			io_result(&action, AIUS_IO_PROGRESS, action.length, 0));
	}
	finish_close(fixture, action);
	assert(fixture->dispatches == 1);
}

static void assert_same_response(const char *label,
	                             const ObservedResponse *reference,
	                             const ObservedResponse *observed)
{
	if (reference->length != observed->length ||
	    memcmp(reference->bytes, observed->bytes, reference->length) != 0) {
		fprintf(stderr,
		        "chunk boundary %s: expected %zu bytes \"%.*s\", "
		        "actual %zu bytes \"%.*s\"\n",
		        label, reference->length, (int)reference->length,
		        (const char *)reference->bytes, observed->length,
		        (int)observed->length, (const char *)observed->bytes);
	}
	assert(observed->length == reference->length);
	assert(memcmp(observed->bytes, reference->bytes, reference->length) == 0);
}

static void test_receive_chunk_boundary_equivalence(void)
{
	static const char body[] = "{\"id\":\"channel_id_51e\",\"loop\":true}";
	static const char raw[] =
		"POST /api/replays/start HTTP/1.1\r\n"
		"Host: 192.0.2.66\r\n"
		"X-Aius-SD-Auth: 0123456789abcdef\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: 35\r\n"
		"Connection: close\r\n"
		"\r\n"
		"{\"id\":\"channel_id_51e\",\"loop\":true}";
	const size_t body_length = sizeof(body) - 1u;
	const size_t length = sizeof(raw) - 1u;
	const size_t header_length = length - body_length;
	const size_t name_offset = (size_t)(strstr(raw, "Content-Length") - raw);
	Fixture fixture;
	ObservedResponse reference;
	ObservedResponse observed;
	size_t bounds[sizeof(raw)];
	size_t rows = 0;

	assert(body_length == 35u);
	init_fixture(&fixture);
	bounds[0] = length;
	feed_chunked(&fixture, raw, length, body, body_length, bounds, 1u,
	             &reference);
	assert(contains_bytes(reference.bytes, reference.length, "200 OK"));
	assert(contains_bytes(reference.bytes, reference.length, "prebuffering"));

	{
		/* Every boundary a segment can land on, each fed as two chunks. */
		const struct {
			const char *label;
			size_t split;
		} table[] = {
			{ "mid-request-line", 4u },
			{ "mid-header-name", name_offset + 7u },
			{ "mid-content-length-value", name_offset +
				  sizeof("Content-Length: ") },
			{ "inside-header-terminator-1", header_length - 3u },
			{ "inside-header-terminator-2", header_length - 2u },
			{ "inside-header-terminator-3", header_length - 1u },
			{ "headers-then-body", header_length },
			{ "one-body-byte-with-headers", header_length + 1u },
			{ "last-body-byte-alone", length - 1u },
		};
		for (size_t i = 0; i < ARRAY_LENGTH(table); ++i) {
			assert(table[i].split > 0u && table[i].split < length);
			init_fixture(&fixture);
			bounds[0] = table[i].split;
			bounds[1] = length;
			feed_chunked(&fixture, raw, length, body, body_length,
			             bounds, 2u, &observed);
			assert_same_response(table[i].label, &reference,
			                     &observed);
			rows++;
		}
	}

	init_fixture(&fixture);
	for (size_t i = 0; i < length; ++i)
		bounds[i] = i + 1u;
	feed_chunked(&fixture, raw, length, body, body_length, bounds, length,
	             &observed);
	assert_same_response("byte-at-a-time", &reference, &observed);
	rows++;
	assert(rows == 10);
}

/* The same equivalence for a body that spans many RX epochs: PUT
 * /api/dsp/config carries up to AIUS_HTTP_MAX_BODY bytes, so its body alone is
 * 48 reads at AIUS_HTTP_RX_QUOTA and a boundary can fall anywhere in it. */
static void test_receive_large_body_chunk_boundaries(void)
{
	static const char head[] =
		"PUT /api/dsp/config HTTP/1.1\r\n"
		"Host: 192.0.2.66\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: 12288\r\n"
		"\r\n";
	static char raw[sizeof(head) + AIUS_HTTP_MAX_BODY];
	const size_t header_length = sizeof(head) - 1u;
	const size_t body_length = AIUS_HTTP_MAX_BODY;
	const size_t length = header_length + body_length;
	Fixture fixture;
	ObservedResponse reference;
	ObservedResponse observed;
	size_t bounds[4];
	size_t rows = 0;

	assert(body_length == 12288u);
	memcpy(raw, head, header_length);
	for (size_t i = 0; i < body_length; ++i)
		raw[header_length + i] = (char)('a' + (i % 26u));
	assert(length <= AIUS_HTTP_MAX_REQUEST);

	init_fixture(&fixture);
	bounds[0] = length;
	feed_chunked(&fixture, raw, length, raw + header_length, body_length,
	             bounds, 1u, &reference);
	assert(contains_bytes(reference.bytes, reference.length, "200 OK"));

	{
		const struct {
			const char *label;
			size_t split;
		} table[] = {
			{ "large-headers-then-body", header_length },
			{ "large-mid-body", header_length + (body_length / 2u) },
			{ "large-quota-boundary", header_length +
				  AIUS_HTTP_RX_QUOTA },
			{ "large-last-body-byte-alone", length - 1u },
		};
		for (size_t i = 0; i < ARRAY_LENGTH(table); ++i) {
			init_fixture(&fixture);
			bounds[0] = table[i].split;
			bounds[1] = length;
			feed_chunked(&fixture, raw, length, raw + header_length,
			             body_length, bounds, 2u, &observed);
			assert_same_response(table[i].label, &reference,
			                     &observed);
			rows++;
		}
	}
	assert(rows == 4);
}

int main(void)
{
	struct RegistryRow {
		const char *key;
		void (*run)(void);
	};
	static const struct RegistryRow registry[] = {
		{ "init.invalid-null-exhaustion",
		  test_invalid_initialization_and_null_action },
		{ "accept.validation-restart", test_accept_validation_and_restart },
		{ "receive.quota-timeout-wrap", test_request_quota_timeout_and_wrap },
		{ "dispatch.partial-close-restart",
		  test_dispatch_response_partial_write_close_restart },
		{ "dispatch.idle-only-after-write-and-close",
		  test_immediate_response_reaches_idle_only_after_write_and_close },
		{ "send.timeout-progress-precedence",
		  test_response_timeout_progress_precedence },
		{ "event.protocol-payload", test_protocol_payload_validation },
		{ "application.validation-overflow",
		  test_application_validation_and_overflow },
		{ "application.upload-modes-publication-sizes",
		  test_upload_application_modes_and_publication_sizes },
		{ "application.control-job-validation",
		  test_control_job_application_validation },
		{ "application.require-existing-validation",
		  test_require_existing_application_validation },
		{ "receive.capacity-dispatch",
		  test_request_capacity_and_strict_dispatch },
		{ "upload.prefix-staging-deadline",
		  test_upload_prefix_staging_and_deadline },
		{ "upload.read-would-block-progress",
		  test_upload_read_would_block_and_progress },
		{ "upload.supplied-tracks-body",
		  test_upload_supplied_tracks_the_supplied_body },
		{ "upload.read-transport-terminal-precedence",
		  test_upload_read_transport_terminal_precedence },
		{ "upload.invalid-complete-cancel",
		  test_upload_invalid_completion_cancels },
		{ "upload.buffered-admission", test_buffered_upload_admission },
		{ "response.immediate-stream-body",
		  test_immediate_stream_body },
		{ "download.chunk-stability-ack",
		  test_download_stable_chunks_and_ack },
		{ "download.zero", test_zero_download },
		{ "download.cache-control", test_download_cache_control },
		{ "download.peer-fail-release-drain",
		  test_download_peer_fault_release_close_cancel_drain },
		{ "sd.failure-map", test_sd_failure_mapping },
		{ "sd.invalid-kind-retention",
		  test_sd_result_matrix_rejects_wrong_kind_and_retains_authority },
		{ "sd.deadline-classes", test_sd_deadlines },
		{ "action-id.exhaustion", test_action_id_exhaustion },
		{ "event.schema-equivalence", test_event_schema_equivalence_classes },
		{ "event.sd-schema-unnamed-fields",
		  test_sd_result_schema_unnamed_fields },
		{ "quota.boundaries", test_rx_tx_quota_boundaries },
		{ "application.status-field-capacity",
		  test_all_application_statuses_and_field_edges },
		{ "sd.matrix-executable", test_executable_sd_result_matrix },
		{ "response.fixed-capacity-registry",
		  test_fixed_response_capacity_registry },
		{ "download.ack-deadlines", test_ack_download_deadline_cases },
		{ "authority.event-equivalence",
		  test_authority_event_equivalence_registry },
		{ "precedence.engine", test_engine_driven_precedence_registry },
		{ "lifecycle.multiple-restarts", test_multiple_restart_registry },
		{ "authority.duplicate-dispatch-close-cancel",
		  test_duplicate_authority_registry },
		{ "receive.chunk-boundary-equivalence",
		  test_receive_chunk_boundary_equivalence },
		{ "receive.large-body-chunk-boundaries",
		  test_receive_large_body_chunk_boundaries },
	};
	assert(ARRAY_LENGTH(registry) == 40);
	for (size_t i = 0; i < ARRAY_LENGTH(registry); ++i) {
		assert(registry[i].key[0] != '\0');
		for (size_t j = 0; j < i; ++j)
			assert(strcmp(registry[i].key, registry[j].key) != 0);
		fprintf(stderr, "http oracle: %s\n", registry[i].key);
		registry[i].run();
	}
	puts("ok http transaction oracle");
	return 0;
}
