#include "aius_usb_control.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

static const char *current_key = "startup";

#define CHECK(condition) check((condition), #condition, __LINE__)

static void check(int condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "usb-control oracle mismatch [%s] line %d: %s\n",
		current_key, line, expression);
	exit(1);
}

typedef struct {
	AiusUsbRuntime runtime;
	char response[AIUS_USB_RUNTIME_MAX_RESPONSE];
} RuntimeFixture;

typedef struct {
	AiusUsbRuntime runtime;
	char response[AIUS_USB_RUNTIME_MAX_RESPONSE];
	uint8_t rx[AIUS_USB_RUNTIME_RX_QUOTA + 1u];
	AiusUsbApplicationResponse application_response;
	char verb[11];
	char payload[257];
	AiusUsbRuntimeAction action;
} DisjointLayout;

_Static_assert(offsetof(DisjointLayout, response) >= sizeof(AiusUsbRuntime),
	"runtime and response storage overlap");
_Static_assert(offsetof(DisjointLayout, rx) >=
		offsetof(DisjointLayout, response) + AIUS_USB_RUNTIME_MAX_RESPONSE,
	"response and RX storage overlap");
_Static_assert(offsetof(DisjointLayout, application_response) >=
		offsetof(DisjointLayout, rx) + AIUS_USB_RUNTIME_RX_QUOTA + 1u,
	"RX and application response storage overlap");
_Static_assert(offsetof(DisjointLayout, verb) >=
		offsetof(DisjointLayout, application_response) +
			sizeof(AiusUsbApplicationResponse),
	"application response and verb storage overlap");
_Static_assert(offsetof(DisjointLayout, payload) >=
		offsetof(DisjointLayout, verb) + 11u,
	"verb and payload storage overlap");
_Static_assert(offsetof(DisjointLayout, action) >=
		offsetof(DisjointLayout, payload) + 257u,
	"payload and action storage overlap");

static AiusUsbRuntimeInput input_connected(void)
{
	AiusUsbRuntimeInput input;
	memset(&input, 0, sizeof(input));
	input.connected = true;
	return input;
}

static AiusIoEvent io_event(AiusIoResult result, size_t bytes, uint32_t fault)
{
	AiusIoEvent event;
	event.result = result;
	event.bytes = bytes;
	event.fault = fault;
	return event;
}

static void fixture_init_with_capacity(RuntimeFixture *fixture, size_t capacity)
{
	memset(fixture, 0xa5, sizeof(*fixture));
	aius_usb_runtime_init(&fixture->runtime, fixture->response, capacity);
}

static void fixture_init(RuntimeFixture *fixture)
{
	fixture_init_with_capacity(fixture, sizeof(fixture->response));
	CHECK(fixture->runtime.valid);
	CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_IDLE);
}

static void expect_none(const AiusUsbRuntimeAction *action)
{
	CHECK(action->kind == AIUS_USB_ACTION_NONE);
	CHECK(action->request == NULL);
	CHECK(action->tx == NULL);
	CHECK(action->tx_length == 0u);
}

static AiusUsbRuntimeAction runtime_step(
	RuntimeFixture *fixture,
	uint32_t now_ms,
	AiusUsbRuntimeInput input)
{
	AiusUsbRuntimeAction action;
	memset(&action, 0xa5, sizeof(action));
	aius_usb_runtime_step(&fixture->runtime, now_ms, input, &action);
	return action;
}

static AiusUsbRuntimeAction dispatch_line(
	RuntimeFixture *fixture,
	uint32_t now_ms,
	const char *line,
	AiusUsbCommandKind kind,
	uint32_t request_id)
{
	AiusUsbRuntimeInput input = input_connected();
	AiusUsbRuntimeAction action;
	input.rx = (const uint8_t *)line;
	input.rx_length = strlen(line);
	action = runtime_step(fixture, now_ms, input);
	CHECK(action.kind == AIUS_USB_ACTION_DISPATCH);
	CHECK(action.request == &fixture->runtime.pending_request);
	CHECK(action.request->command.kind == kind);
	CHECK(action.request->command.request_id == request_id);
	CHECK(action.rx_consumed == strlen(line));
	CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_DISPATCH);
	CHECK(fixture->runtime.dispatch_emitted);
	return action;
}

static AiusUsbApplicationResponse success_response(
	const char *verb,
	size_t verb_length,
	const char *payload,
	size_t payload_length)
{
	AiusUsbApplicationResponse response;
	response.error = AIUS_USB_ERROR_NONE;
	response.verb = verb;
	response.verb_length = verb_length;
	response.payload = payload;
	response.payload_length = payload_length;
	return response;
}

static AiusUsbApplicationResponse error_response(AiusUsbError error)
{
	AiusUsbApplicationResponse response;
	memset(&response, 0, sizeof(response));
	response.error = error;
	return response;
}

static AiusUsbRuntimeAction supply_response(
	RuntimeFixture *fixture,
	uint32_t now_ms,
	const AiusUsbApplicationResponse *response,
	size_t tx_capacity)
{
	AiusUsbRuntimeInput input = input_connected();
	input.application_response = response;
	input.tx_capacity = tx_capacity;
	return runtime_step(fixture, now_ms, input);
}

static void expect_response(const RuntimeFixture *fixture, const char *expected)
{
	size_t length = strlen(expected);
	CHECK(fixture->runtime.response_length == length);
	CHECK(memcmp(fixture->response, expected, length) == 0);
}

static void begin_short_normal_tx(RuntimeFixture *fixture, size_t tx_capacity)
{
	AiusUsbApplicationResponse response =
		success_response("ID", 2u, "x", 1u);
	AiusUsbRuntimeAction action;
	fixture_init(fixture);
	(void)dispatch_line(
		fixture, 100u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
	action = supply_response(fixture, 100u, &response, tx_capacity);
	if (tx_capacity == 0u) {
		expect_none(&action);
		CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_TX);
	} else {
		CHECK(action.kind == AIUS_USB_ACTION_WRITE);
	}
}

static const char long_payload[] =
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void begin_long_normal_tx(RuntimeFixture *fixture, size_t tx_capacity)
{
	AiusUsbApplicationResponse response =
		success_response("AUTH", 4u, long_payload, sizeof(long_payload) - 1u);
	AiusUsbRuntimeAction action;
	fixture_init(fixture);
	(void)dispatch_line(
		fixture, 100u, "AIUS AUTH 7\n", AIUS_USB_COMMAND_AUTH, 7u);
	action = supply_response(fixture, 100u, &response, tx_capacity);
	if (tx_capacity == 0u)
		expect_none(&action);
	else
		CHECK(action.kind == AIUS_USB_ACTION_WRITE);
}

static void begin_boot_tx(
	RuntimeFixture *fixture,
	uint32_t now_ms,
	size_t tx_capacity,
	AiusUsbRuntimeAction *action)
{
	AiusUsbApplicationResponse response =
		success_response("BOOTLOADER", 10u, NULL, 0u);
	fixture_init(fixture);
	(void)dispatch_line(
		fixture, now_ms, "AIUS BOOTLOADER 9\n",
		AIUS_USB_COMMAND_BOOTLOADER, 9u);
	*action = supply_response(fixture, now_ms, &response, tx_capacity);
	CHECK(fixture->runtime.bootloader_authorized);
	CHECK(fixture->runtime.bootloader_deadline_ms ==
		now_ms + AIUS_USB_BOOTLOADER_DELAY_MS);
}

static void emit_write(RuntimeFixture *fixture, size_t capacity,
	AiusUsbRuntimeAction *action)
{
	AiusUsbRuntimeInput input = input_connected();
	input.tx_capacity = capacity;
	*action = runtime_step(fixture, 101u, input);
	CHECK(action->kind == AIUS_USB_ACTION_WRITE);
	CHECK(action->tx == (const uint8_t *)fixture->response +
		fixture->runtime.response_offset);
	CHECK(action->tx_length <= AIUS_USB_RUNTIME_TX_QUOTA);
	CHECK(action->tx_length <= capacity);
	CHECK(fixture->runtime.write_outstanding);
	CHECK(fixture->runtime.outstanding_length == action->tx_length);
}

static AiusUsbRuntimeAction complete_write(
	RuntimeFixture *fixture,
	uint32_t now_ms,
	AiusIoResult result,
	size_t bytes,
	uint32_t fault,
	size_t replacement_capacity)
{
	AiusUsbRuntimeInput input = input_connected();
	input.tx_event_present = true;
	input.tx_event = io_event(result, bytes, fault);
	input.tx_capacity = replacement_capacity;
	return runtime_step(fixture, now_ms, input);
}

static void enter_bootloader(RuntimeFixture *fixture)
{
	AiusUsbRuntimeAction write;
	AiusUsbRuntimeAction action;
	begin_boot_tx(fixture, 100u, AIUS_USB_RUNTIME_TX_QUOTA, &write);
	CHECK(write.kind == AIUS_USB_ACTION_WRITE);
	CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_ACK_PENDING);
	action = complete_write(
		fixture, 101u, AIUS_IO_PROGRESS, write.tx_length, 0u, 0u);
	CHECK(action.kind == AIUS_USB_ACTION_ENTER_BOOTLOADER);
	CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_ENTER_BOOTLOADER);
	CHECK(fixture->runtime.enter_emitted);
}

static void test_init_and_prologues(void)
{
	RuntimeFixture fixture;
	RuntimeFixture before;
	AiusUsbRuntimeAction action;
	AiusUsbRuntimeInput input = input_connected();
	char response[AIUS_USB_RUNTIME_MAX_RESPONSE];
	char before_response[AIUS_USB_RUNTIME_MAX_RESPONSE];
	DisjointLayout layout;

	current_key = "prologue.init.null-runtime";
	memset(response, 0x5a, sizeof(response));
	memcpy(before_response, response, sizeof(response));
	aius_usb_runtime_init(NULL, response, sizeof(response));
	CHECK(memcmp(response, before_response, sizeof(response)) == 0);

	current_key = "prologue.init.full-reset";
	memset(&fixture, 0xa5, sizeof(fixture));
	memcpy(before_response, fixture.response, sizeof(before_response));
	aius_usb_runtime_init(
		&fixture.runtime, fixture.response, sizeof(fixture.response));
	CHECK(memcmp(fixture.response, before_response, sizeof(before_response)) == 0);
	CHECK(fixture.runtime.parser.length == 0u);
	CHECK(fixture.runtime.parser.discarding == 0u);
	CHECK(fixture.runtime.parser.invalid == 0u);
	CHECK(fixture.runtime.parser.suppress_lf == 0u);
	CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
	CHECK(fixture.runtime.response == fixture.response);
	CHECK(fixture.runtime.response_capacity == sizeof(fixture.response));
	CHECK(fixture.runtime.response_length == 0u);
	CHECK(fixture.runtime.response_offset == 0u);
	CHECK(fixture.runtime.bootloader_deadline_ms == 0u);
	CHECK(fixture.runtime.protocol_faults == 0u);
	CHECK(fixture.runtime.bootloader_entry_failures == 0u);
	CHECK(fixture.runtime.outstanding_length == 0u);
	CHECK(fixture.runtime.pending_request.command.kind == AIUS_USB_COMMAND_NONE);
	CHECK(fixture.runtime.pending_request.command.request_id == 0u);
	CHECK(fixture.runtime.valid);
	CHECK(!fixture.runtime.write_outstanding);
	CHECK(!fixture.runtime.dispatch_emitted);
	CHECK(!fixture.runtime.bootloader_authorized);
	CHECK(!fixture.runtime.enter_emitted);

	current_key = "prologue.init.null-response";
	fixture_init_with_capacity(&fixture, AIUS_USB_RUNTIME_MIN_RESPONSE);
	memcpy(before_response, fixture.response, sizeof(before_response));
	aius_usb_runtime_init(
		&fixture.runtime, NULL, AIUS_USB_RUNTIME_MIN_RESPONSE);
	CHECK(!fixture.runtime.valid);
	CHECK(memcmp(fixture.response, before_response, sizeof(before_response)) == 0);

	current_key = "prologue.step.null-action";
	fixture_init(&fixture);
	fixture.runtime.protocol_faults = 9u;
	before = fixture;
	input.rx = NULL;
	input.rx_length = 1u;
	aius_usb_runtime_step(&fixture.runtime, 55u, input, NULL);
	CHECK(memcmp(&fixture, &before, sizeof(fixture)) == 0);

	current_key = "prologue.step.null-runtime";
	memset(&action, 0xa5, sizeof(action));
	aius_usb_runtime_step(NULL, 55u, input, &action);
	expect_none(&action);
	CHECK(action.rx_consumed == 0u);

	current_key = "prologue.step.invalid-runtime";
	fixture_init_with_capacity(&fixture, AIUS_USB_RUNTIME_MIN_RESPONSE - 1u);
	CHECK(!fixture.runtime.valid);
	fixture.runtime.protocol_faults = 9u;
	memset(&action, 0xa5, sizeof(action));
	aius_usb_runtime_step(&fixture.runtime, 55u, input, &action);
	expect_none(&action);
	CHECK(action.rx_consumed == 0u);
	CHECK(fixture.runtime.protocol_faults == 9u);

	current_key = "layout.normal-disjoint";
	memset(&layout, 0, sizeof(layout));
	CHECK((uintptr_t)&layout.runtime + sizeof(layout.runtime) <=
		(uintptr_t)layout.response);
	CHECK((uintptr_t)layout.response + sizeof(layout.response) <=
		(uintptr_t)layout.rx);
	CHECK((uintptr_t)layout.rx + sizeof(layout.rx) <=
		(uintptr_t)&layout.application_response);
	CHECK((uintptr_t)&layout.application_response +
			sizeof(layout.application_response) <=
		(uintptr_t)layout.verb);
	CHECK((uintptr_t)layout.verb + sizeof(layout.verb) <=
		(uintptr_t)layout.payload);
	CHECK((uintptr_t)layout.payload + sizeof(layout.payload) <=
		(uintptr_t)&layout.action);

	current_key = "prologue.rx-null-nonzero";
	fixture_init(&fixture);
	input = input_connected();
	input.rx_length = 1u;
	action = runtime_step(&fixture, 1u, input);
	expect_none(&action);
	CHECK(action.rx_consumed == 0u);
	CHECK(fixture.runtime.protocol_faults == 1u);

	current_key = "prologue.rx-nonnull-zero";
	fixture_init(&fixture);
	input = input_connected();
	input.rx = (const uint8_t *)"x";
	action = runtime_step(&fixture, 1u, input);
	expect_none(&action);
	CHECK(fixture.runtime.protocol_faults == 1u);

	current_key = "prologue.rx-overlap-response";
	fixture_init(&fixture);
	input = input_connected();
	input.rx = (const uint8_t *)fixture.response;
	input.rx_length = 1u;
	action = runtime_step(&fixture, 1u, input);
	expect_none(&action);
	CHECK(action.rx_consumed == 0u);
	CHECK(fixture.runtime.protocol_faults == 1u);
}

static AiusUsbEvent legacy_feed(AiusUsbParser *parser, const uint8_t *bytes,
	size_t length)
{
	AiusUsbEvent event;
	memset(&event, 0, sizeof(event));
	for (size_t i = 0; i < length; ++i)
		if (aius_usb_control_feed(parser, bytes[i], &event))
			return event;
	return event;
}

static AiusUsbEvent legacy_feed_text(AiusUsbParser *parser, const char *text)
{
	return legacy_feed(parser, (const uint8_t *)text, strlen(text));
}

static void test_legacy_parser_and_formatters(void)
{
	AiusUsbParser parser;
	AiusUsbEvent event;
	char output[96];
	char long_line[AIUS_USB_CONTROL_MAX_LINE + 2u];
	const uint8_t invalid_control[] = {
		'A', 'I', 'U', 'S', ' ', 'I', 'D', ' ', '3', 0x01, '\n'
	};
	const uint8_t invalid_high[] = {
		'A', 'I', 'U', 'S', ' ', 'I', 'D', ' ', '3', 0x80, '\n'
	};

	current_key = "legacy.parser.commands";
	aius_usb_control_init(&parser);
	event = legacy_feed_text(&parser, "AIUS PROTOCOL 1\n");
	CHECK(event.type == AIUS_USB_EVENT_COMMAND);
	CHECK(event.command.kind == AIUS_USB_COMMAND_PROTOCOL);
	event = legacy_feed_text(&parser, "AIUS ID 7\r\n");
	CHECK(event.command.kind == AIUS_USB_COMMAND_ID);
	CHECK(event.command.request_id == 7u);
	event = legacy_feed_text(&parser, "AIUS AUTH\n");
	CHECK(event.command.kind == AIUS_USB_COMMAND_AUTH);
	CHECK(event.command.request_id == 0u);
	event = legacy_feed_text(&parser, "AIUS BOOTLOADER 4294967295\r");
	CHECK(event.command.kind == AIUS_USB_COMMAND_BOOTLOADER);
	CHECK(event.command.request_id == UINT32_MAX);

	current_key = "legacy.parser.errors";
	aius_usb_control_init(&parser);
	event = legacy_feed_text(&parser, "\n");
	CHECK(event.error == AIUS_USB_ERROR_BAD_COMMAND);
	event = legacy_feed_text(&parser, "AIUS NOPE 1\n");
	CHECK(event.error == AIUS_USB_ERROR_BAD_COMMAND);
	event = legacy_feed_text(&parser, "AIUS PROTOCOL 2\n");
	CHECK(event.error == AIUS_USB_ERROR_BAD_VERSION);
	event = legacy_feed_text(&parser, "AIUS ID 0\n");
	CHECK(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	event = legacy_feed_text(&parser, "AIUS ID 1 extra\n");
	CHECK(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	event = legacy_feed_text(&parser, "AIUS ID 4294967296\n");
	CHECK(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	event = legacy_feed(&parser, invalid_control, sizeof(invalid_control));
	CHECK(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	event = legacy_feed(&parser, invalid_high, sizeof(invalid_high));
	CHECK(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);

	current_key = "legacy.parser.overflow-recovery";
	memset(long_line, 'x', sizeof(long_line));
	long_line[sizeof(long_line) - 1u] = '\n';
	event = legacy_feed(&parser, (const uint8_t *)long_line, sizeof(long_line));
	CHECK(event.error == AIUS_USB_ERROR_LINE_TOO_LONG);
	event = legacy_feed_text(&parser, "AIUS ID 2\n");
	CHECK(event.command.kind == AIUS_USB_COMMAND_ID);
	CHECK(event.command.request_id == 2u);

	current_key = "legacy.formatters";
	CHECK(aius_usb_control_write_ok(
		output, sizeof(output), 7u, "ID", "x") ==
		strlen("AIUS/1 OK 7 ID x\n"));
	CHECK(strcmp(output, "AIUS/1 OK 7 ID x\n") == 0);
	CHECK(aius_usb_control_write_ok(
		output, sizeof(output), 0u, "PROTOCOL", "") ==
		strlen("AIUS/1 OK 0 PROTOCOL\n"));
	CHECK(strcmp(output, "AIUS/1 OK 0 PROTOCOL\n") == 0);
	CHECK(aius_usb_control_write_error(
		output, sizeof(output), 7u, AIUS_USB_ERROR_BAD_ARGUMENT) ==
		strlen("AIUS/1 ERR 7 BAD_ARGUMENT error\n"));
	CHECK(strcmp(output, "AIUS/1 ERR 7 BAD_ARGUMENT error\n") == 0);
	CHECK(aius_usb_control_write_ok(output, 8u, 1u, "ID", "long") == 0u);
	CHECK(aius_usb_control_write_error(
		output, 8u, 1u, AIUS_USB_ERROR_INTERNAL) == 0u);
}

typedef struct {
	const char *family;
	const char *key;
	unsigned variant;
} TransitionCase;

static const char *const transition_families[] = {
	"T01", "T02", "T03", "T04", "T05", "T06", "T07", "T08",
	"T09", "T10", "T11", "T12", "T13", "T14", "T15", "T16",
	"T17", "T18", "T19", "T20", "T21", "T22", "T23", "T24"
};

static const TransitionCase transition_cases[] = {
	{"T01", "T01.disconnected", 0u},
	{"T02", "T02.no-rx", 0u},
	{"T03", "T03.incomplete-rx", 0u},
	{"T04", "T04.complete-command", 0u},
	{"T05", "T05.parser-error", 0u},
	{"T06", "T06.normal-success", 0u},
	{"T07", "T07.application-error", 0u},
	{"T08", "T08.bootloader-success", 0u},
	{"T09", "T09.invalid-response", 0u},
	{"T10", "T10.missing-response", 0u},
	{"T11", "T11.unsolicited-response", 0u},
	{"T12", "T12.zero-capacity", 0u},
	{"T13", "T13.nonfinal-write", 0u},
	{"T14", "T14.final-normal-write", 0u},
	{"T15", "T15.final-boot-write", 0u},
	{"T16", "T16.normal", 0u},
	{"T16", "T16.boot", 1u},
	{"T17", "T17.normal", 0u},
	{"T17", "T17.boot", 1u},
	{"T18", "T18.final-normal-progress", 0u},
	{"T19", "T19.final-boot-progress", 0u},
	{"T20", "T20.normal", 0u},
	{"T20", "T20.boot", 1u},
	{"T21", "T21.normal.closed", 0u},
	{"T21", "T21.normal.fault", 1u},
	{"T21", "T21.boot.closed", 2u},
	{"T21", "T21.boot.fault", 3u},
	{"T22", "T22.missing", 0u},
	{"T22", "T22.malformed", 1u},
	{"T23", "T23.missing", 0u},
	{"T23", "T23.malformed", 1u},
	{"T24", "T24.none", 0u},
	{"T24", "T24.rx", 1u},
	{"T24", "T24.application", 2u},
	{"T24", "T24.tx", 3u},
	{"T24", "T24.disconnect", 4u}
};

static int string_in_list(
	const char *candidate,
	const char *const *values,
	size_t count)
{
	for (size_t i = 0; i < count; ++i)
		if (strcmp(candidate, values[i]) == 0)
			return 1;
	return 0;
}

static void validate_transition_registry(void)
{
	current_key = "registry.transitions";
	CHECK(ARRAY_COUNT(transition_families) == 24u);
	CHECK(ARRAY_COUNT(transition_cases) == 36u);
	for (size_t i = 0; i < ARRAY_COUNT(transition_families); ++i) {
		for (size_t j = i + 1u; j < ARRAY_COUNT(transition_families); ++j)
			CHECK(strcmp(transition_families[i], transition_families[j]) != 0);
	}
	for (size_t i = 0; i < ARRAY_COUNT(transition_cases); ++i) {
		CHECK(string_in_list(
			transition_cases[i].family,
			transition_families,
			ARRAY_COUNT(transition_families)));
		for (size_t j = i + 1u; j < ARRAY_COUNT(transition_cases); ++j)
			CHECK(strcmp(transition_cases[i].key,
				transition_cases[j].key) != 0);
	}
}

static void test_transition(const TransitionCase *test)
{
	RuntimeFixture fixture;
	AiusUsbRuntimeAction action;
	AiusUsbRuntimeAction write;
	AiusUsbRuntimeInput input;
	AiusUsbApplicationResponse response;
	uint32_t before_faults;
	size_t before_offset;

	current_key = test->key;
	if (strcmp(test->family, "T01") == 0) {
		fixture_init(&fixture);
		input = input_connected();
		input.rx = (const uint8_t *)"AIUS";
		input.rx_length = 4u;
		action = runtime_step(&fixture, 1u, input);
		expect_none(&action);
		CHECK(fixture.runtime.parser.length == 4u);
		memset(&input, 0, sizeof(input));
		action = runtime_step(&fixture, 2u, input);
		expect_none(&action);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
		CHECK(fixture.runtime.parser.length == 0u);
		CHECK(action.rx_consumed == 0u);
	} else if (strcmp(test->family, "T02") == 0) {
		fixture_init(&fixture);
		input = input_connected();
		action = runtime_step(&fixture, 1u, input);
		expect_none(&action);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
	} else if (strcmp(test->family, "T03") == 0) {
		fixture_init(&fixture);
		input = input_connected();
		input.rx = (const uint8_t *)"AIUS";
		input.rx_length = 4u;
		action = runtime_step(&fixture, 1u, input);
		expect_none(&action);
		CHECK(action.rx_consumed == 4u);
		CHECK(fixture.runtime.parser.length == 4u);
	} else if (strcmp(test->family, "T04") == 0) {
		fixture_init(&fixture);
		(void)dispatch_line(
			&fixture, 1u, "AIUS AUTH 8\n", AIUS_USB_COMMAND_AUTH, 8u);
	} else if (strcmp(test->family, "T05") == 0) {
		fixture_init(&fixture);
		input = input_connected();
		input.rx = (const uint8_t *)"\n";
		input.rx_length = 1u;
		action = runtime_step(&fixture, 1u, input);
		expect_none(&action);
		CHECK(action.rx_consumed == 1u);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
		expect_response(&fixture, "AIUS/1 ERR 0 BAD_COMMAND error\n");
	} else if (strcmp(test->family, "T06") == 0) {
		begin_short_normal_tx(&fixture, 0u);
		expect_response(&fixture, "AIUS/1 OK 7 ID x\n");
		CHECK(fixture.runtime.protocol_faults == 0u);
	} else if (strcmp(test->family, "T07") == 0) {
		fixture_init(&fixture);
		(void)dispatch_line(
			&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
		response = error_response(AIUS_USB_ERROR_BUSY);
		action = supply_response(&fixture, 1u, &response, 0u);
		expect_none(&action);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
		expect_response(&fixture, "AIUS/1 ERR 7 BUSY error\n");
	} else if (strcmp(test->family, "T08") == 0) {
		begin_boot_tx(&fixture, 100u, 0u, &action);
		expect_none(&action);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
		expect_response(
			&fixture, "AIUS/1 OK 9 BOOTLOADER entering\n");
	} else if (strcmp(test->family, "T09") == 0) {
		fixture_init(&fixture);
		(void)dispatch_line(
			&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
		response = success_response("AUTH", 4u, "x", 1u);
		action = supply_response(&fixture, 1u, &response, 0u);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == 1u);
		expect_response(&fixture, "AIUS/1 ERR 7 INTERNAL error\n");
	} else if (strcmp(test->family, "T10") == 0) {
		fixture_init(&fixture);
		(void)dispatch_line(
			&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
		input = input_connected();
		action = runtime_step(&fixture, 1u, input);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == 1u);
		expect_response(&fixture, "AIUS/1 ERR 7 INTERNAL error\n");
	} else if (strcmp(test->family, "T11") == 0) {
		fixture_init(&fixture);
		response = success_response("ID", 2u, "x", 1u);
		input = input_connected();
		input.application_response = &response;
		action = runtime_step(&fixture, 1u, input);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == 1u);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
	} else if (strcmp(test->family, "T12") == 0) {
		begin_short_normal_tx(&fixture, 0u);
		before_offset = fixture.runtime.response_offset;
		input = input_connected();
		action = runtime_step(&fixture, 2u, input);
		expect_none(&action);
		CHECK(fixture.runtime.response_offset == before_offset);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
	} else if (strcmp(test->family, "T13") == 0) {
		begin_long_normal_tx(&fixture, 0u);
		emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &action);
		CHECK(action.tx_length == AIUS_USB_RUNTIME_TX_QUOTA);
		CHECK(action.tx_length < fixture.runtime.response_length);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
	} else if (strcmp(test->family, "T14") == 0) {
		begin_short_normal_tx(&fixture, 0u);
		emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &action);
		CHECK(action.tx_length == fixture.runtime.response_length);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
	} else if (strcmp(test->family, "T15") == 0) {
		begin_boot_tx(&fixture, 100u, 0u, &action);
		emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &action);
		CHECK(action.tx_length == fixture.runtime.response_length);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_ACK_PENDING);
	} else if (strcmp(test->family, "T16") == 0) {
		if (test->variant == 0u)
			begin_long_normal_tx(&fixture, 8u);
		else
			begin_boot_tx(&fixture, 100u, 8u, &write);
		CHECK(fixture.runtime.outstanding_length == 8u);
		action = complete_write(
			&fixture, 101u, AIUS_IO_PROGRESS, 3u, 0u, 5u);
		CHECK(action.kind == AIUS_USB_ACTION_WRITE);
		CHECK(action.tx == (const uint8_t *)fixture.response + 3u);
		CHECK(action.tx_length == 5u);
		CHECK(fixture.runtime.response_offset == 3u);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
		CHECK(fixture.runtime.write_outstanding);
		CHECK(fixture.runtime.outstanding_length == 5u);
		CHECK(fixture.runtime.protocol_faults == 0u);
		CHECK(fixture.runtime.bootloader_authorized == (test->variant != 0u));
		if (test->variant != 0u)
			CHECK(fixture.runtime.bootloader_deadline_ms == 600u);
	} else if (strcmp(test->family, "T17") == 0) {
		if (test->variant == 0u)
			begin_long_normal_tx(&fixture, 8u);
		else
			begin_boot_tx(&fixture, 100u, 8u, &write);
		action = complete_write(
			&fixture, 101u, AIUS_IO_PROGRESS, 8u, 0u, 7u);
		CHECK(action.kind == AIUS_USB_ACTION_WRITE);
		CHECK(action.tx == (const uint8_t *)fixture.response + 8u);
		CHECK(action.tx_length == 7u);
		CHECK(fixture.runtime.response_offset == 8u);
		CHECK(fixture.runtime.response_offset <
			fixture.runtime.response_length);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
		CHECK(fixture.runtime.write_outstanding);
		CHECK(fixture.runtime.outstanding_length == 7u);
		CHECK(fixture.runtime.protocol_faults == 0u);
		CHECK(fixture.runtime.bootloader_authorized == (test->variant != 0u));
	} else if (strcmp(test->family, "T18") == 0) {
		begin_short_normal_tx(&fixture, 0u);
		emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &write);
		action = complete_write(
			&fixture, 101u, AIUS_IO_PROGRESS, write.tx_length, 0u, 0u);
		expect_none(&action);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
		CHECK(fixture.runtime.response_length == 0u);
		CHECK(!fixture.runtime.write_outstanding);
	} else if (strcmp(test->family, "T19") == 0) {
		begin_boot_tx(
			&fixture, 100u, AIUS_USB_RUNTIME_TX_QUOTA, &write);
		action = complete_write(
			&fixture, 101u, AIUS_IO_PROGRESS, write.tx_length, 0u, 0u);
		CHECK(action.kind == AIUS_USB_ACTION_ENTER_BOOTLOADER);
		CHECK(fixture.runtime.state ==
			AIUS_USB_RUNTIME_ENTER_BOOTLOADER);
		CHECK(fixture.runtime.enter_emitted);
	} else if (strcmp(test->family, "T20") == 0) {
		if (test->variant == 0u)
			begin_long_normal_tx(&fixture, 8u);
		else
			begin_boot_tx(&fixture, 100u, 8u, &write);
		before_offset = fixture.runtime.response_offset;
		action = complete_write(
			&fixture, 101u, AIUS_IO_WOULD_BLOCK, 0u, 0u, 6u);
		CHECK(action.kind == AIUS_USB_ACTION_WRITE);
		CHECK(action.tx ==
			(const uint8_t *)fixture.response + before_offset);
		CHECK(action.tx_length == 6u);
		CHECK(fixture.runtime.response_offset == before_offset);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
		CHECK(fixture.runtime.write_outstanding);
		CHECK(fixture.runtime.outstanding_length == 6u);
		CHECK(fixture.runtime.protocol_faults == 0u);
		CHECK(fixture.runtime.bootloader_authorized == (test->variant != 0u));
		if (test->variant != 0u)
			CHECK(fixture.runtime.bootloader_deadline_ms == 600u);
	} else if (strcmp(test->family, "T21") == 0) {
		AiusIoResult result =
			(test->variant == 0u || test->variant == 2u) ?
				AIUS_IO_CLOSED : AIUS_IO_FAULT;
		uint32_t fault = result == AIUS_IO_FAULT ? 17u : 0u;
		if (test->variant < 2u) {
			begin_short_normal_tx(&fixture, 0u);
			emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &write);
		} else {
			begin_boot_tx(
				&fixture, 100u, AIUS_USB_RUNTIME_TX_QUOTA, &write);
		}
		action = complete_write(
			&fixture, 101u, result, 0u, fault, 0u);
		if (test->variant < 2u) {
			expect_none(&action);
			CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
		} else {
			CHECK(action.kind == AIUS_USB_ACTION_ENTER_BOOTLOADER);
			CHECK(fixture.runtime.state ==
				AIUS_USB_RUNTIME_ENTER_BOOTLOADER);
		}
	} else if (strcmp(test->family, "T22") == 0) {
		begin_short_normal_tx(&fixture, 0u);
		emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &write);
		before_faults = fixture.runtime.protocol_faults;
		input = input_connected();
		if (test->variant != 0u) {
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 0u, 0u);
		} else {
			input.tx_event =
				io_event((AiusIoResult)99, SIZE_MAX, UINT32_MAX);
		}
		action = runtime_step(&fixture, 101u, input);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == before_faults + 1u);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
	} else if (strcmp(test->family, "T23") == 0) {
		begin_boot_tx(
			&fixture, 100u, AIUS_USB_RUNTIME_TX_QUOTA, &write);
		before_faults = fixture.runtime.protocol_faults;
		input = input_connected();
		input.tx_capacity = 9u;
		if (test->variant != 0u) {
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 0u, 0u);
		} else {
			input.tx_event =
				io_event((AiusIoResult)99, SIZE_MAX, UINT32_MAX);
		}
		action = runtime_step(&fixture, 101u, input);
		CHECK(action.kind == AIUS_USB_ACTION_WRITE);
		CHECK(action.tx == (const uint8_t *)fixture.response);
		CHECK(action.tx_length == 9u);
		CHECK(fixture.runtime.protocol_faults == before_faults + 1u);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
		CHECK(fixture.runtime.bootloader_authorized);
		CHECK(fixture.runtime.response_offset == 0u);
		CHECK(fixture.runtime.write_outstanding);
		CHECK(fixture.runtime.outstanding_length == 9u);
	} else {
		/* T24 is the step after an ENTER_BOOTLOADER action whose
		 * composition returned: HalfKay was not reached, so the terminal
		 * state is released instead of latched and this step behaves as a
		 * fresh IDLE one. */
		enter_bootloader(&fixture);
		before_faults = fixture.runtime.protocol_faults;
		CHECK(fixture.runtime.bootloader_entry_failures == 0u);
		input = input_connected();
		response = success_response("ID", 2u, "x", 1u);
		if (test->variant == 1u) {
			input.rx = (const uint8_t *)"AIUS ID 4\n";
			input.rx_length = strlen((const char *)input.rx);
		} else if (test->variant == 2u) {
			input.application_response = &response;
		} else if (test->variant == 3u) {
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 1u, 0u);
		} else if (test->variant == 4u) {
			input.connected = false;
		}
		action = runtime_step(&fixture, 1000u, input);
		CHECK(fixture.runtime.bootloader_entry_failures == 1u);
		CHECK(!fixture.runtime.enter_emitted);
		CHECK(!fixture.runtime.bootloader_authorized);
		CHECK(fixture.runtime.bootloader_deadline_ms == 0u);
		CHECK(fixture.runtime.response_length == 0u);
		if (test->variant == 1u) {
			CHECK(action.kind == AIUS_USB_ACTION_DISPATCH);
			CHECK(action.rx_consumed == strlen("AIUS ID 4\n"));
			CHECK(action.request->command.kind ==
				AIUS_USB_COMMAND_ID);
			CHECK(action.request->command.request_id == 4u);
			CHECK(fixture.runtime.state ==
				AIUS_USB_RUNTIME_DISPATCH);
		} else {
			expect_none(&action);
			CHECK(action.rx_consumed == 0u);
			CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
		}
		if (test->variant == 2u || test->variant == 3u)
			CHECK(fixture.runtime.protocol_faults == before_faults + 1u);
		else
			CHECK(fixture.runtime.protocol_faults == before_faults);
	}
}

static void test_transition_registry(void)
{
	validate_transition_registry();
	for (size_t i = 0; i < ARRAY_COUNT(transition_cases); ++i)
		test_transition(&transition_cases[i]);
}

typedef struct {
	const char *key;
	AiusIoResult result;
	size_t bytes;
	uint32_t fault;
	int present;
	int idle;
} MalformedTxCase;

static const MalformedTxCase malformed_tx_cases[] = {
	{"tx.invalid-result-low", (AiusIoResult)-1, 0u, 0u, 1, 0},
	{"tx.invalid-result-high", (AiusIoResult)4, 0u, 0u, 1, 0},
	{"tx.progress.zero", AIUS_IO_PROGRESS, 0u, 0u, 1, 0},
	{"tx.progress.overflow", AIUS_IO_PROGRESS, 65u, 0u, 1, 0},
	{"tx.progress.fault", AIUS_IO_PROGRESS, 1u, 9u, 1, 0},
	{"tx.would-block.bytes", AIUS_IO_WOULD_BLOCK, 1u, 0u, 1, 0},
	{"tx.would-block.fault", AIUS_IO_WOULD_BLOCK, 0u, 9u, 1, 0},
	{"tx.closed.bytes", AIUS_IO_CLOSED, 1u, 0u, 1, 0},
	{"tx.closed.fault", AIUS_IO_CLOSED, 0u, 9u, 1, 0},
	{"tx.fault.bytes", AIUS_IO_FAULT, 1u, 9u, 1, 0},
	{"tx.fault.zero-code", AIUS_IO_FAULT, 0u, 0u, 1, 0},
	{"tx.unnamed-payload", AIUS_IO_PROGRESS, 1u, 0u, 0, 1}
};

static void validate_unique_keys(
	const char *registry_key,
	const char *const *keys,
	size_t count,
	size_t expected)
{
	current_key = registry_key;
	CHECK(count == expected);
	for (size_t i = 0; i < count; ++i)
		for (size_t j = i + 1u; j < count; ++j)
			CHECK(strcmp(keys[i], keys[j]) != 0);
}

static void test_malformed_tx_registry(void)
{
	const char *keys[ARRAY_COUNT(malformed_tx_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(malformed_tx_cases); ++i)
		keys[i] = malformed_tx_cases[i].key;
	validate_unique_keys(
		"registry.malformed-tx", keys,
		ARRAY_COUNT(malformed_tx_cases), 12u);

	for (size_t i = 0; i < ARRAY_COUNT(malformed_tx_cases); ++i) {
		const MalformedTxCase *test = &malformed_tx_cases[i];
		RuntimeFixture fixture;
		AiusUsbRuntimeInput input = input_connected();
		AiusUsbRuntimeAction action;
		AiusUsbRuntimeAction write;
		uint32_t before_faults;
		current_key = test->key;
		if (test->idle) {
			fixture_init(&fixture);
			input.tx_event = io_event(test->result, test->bytes, test->fault);
			action = runtime_step(&fixture, 1u, input);
			expect_none(&action);
			CHECK(fixture.runtime.protocol_faults == 0u);
			continue;
		}
		begin_short_normal_tx(&fixture, 0u);
		emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &write);
		before_faults = fixture.runtime.protocol_faults;
		input.tx_event_present = test->present != 0;
		input.tx_event = io_event(test->result, test->bytes, test->fault);
		action = runtime_step(&fixture, 101u, input);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == before_faults + 1u);
		CHECK(fixture.runtime.response_offset == 0u);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
	}
}

typedef struct {
	const char *key;
	unsigned mode;
} KeyedCase;

static const KeyedCase unsolicited_cases[] = {
	{"unsolicited.application.idle", 0u},
	{"unsolicited.application.tx", 1u},
	{"unsolicited.application.ack-pending", 2u},
	{"unsolicited.application.terminal", 3u},
	{"duplicate.application.after-response", 4u},
	{"unsolicited.tx.idle", 5u},
	{"unsolicited.tx.dispatch", 6u},
	{"unsolicited.tx.tx-no-write", 7u},
	{"unsolicited.tx.terminal", 8u},
	{"duplicate.tx.after-progress", 9u},
	{"secondary.tx-with-application", 10u},
	{"secondary.application-with-tx", 11u}
};

static void test_unsolicited_registry(void)
{
	const char *keys[ARRAY_COUNT(unsolicited_cases)];
	AiusUsbApplicationResponse response =
		success_response("ID", 2u, "x", 1u);
	for (size_t i = 0; i < ARRAY_COUNT(unsolicited_cases); ++i)
		keys[i] = unsolicited_cases[i].key;
	validate_unique_keys(
		"registry.unsolicited", keys,
		ARRAY_COUNT(unsolicited_cases), 12u);

	for (size_t i = 0; i < ARRAY_COUNT(unsolicited_cases); ++i) {
		const KeyedCase *test = &unsolicited_cases[i];
		RuntimeFixture fixture;
		AiusUsbRuntimeInput input = input_connected();
		AiusUsbRuntimeAction action;
		AiusUsbRuntimeAction write;
		uint32_t before_faults;
		current_key = test->key;

		if (test->mode == 0u) {
			fixture_init(&fixture);
			input.application_response = &response;
		} else if (test->mode == 1u || test->mode == 4u) {
			begin_short_normal_tx(&fixture, 0u);
			input.application_response = &response;
		} else if (test->mode == 2u) {
			begin_boot_tx(
				&fixture, 100u, AIUS_USB_RUNTIME_TX_QUOTA, &write);
			input.application_response = &response;
		} else if (test->mode == 3u) {
			enter_bootloader(&fixture);
			input.application_response = &response;
		} else if (test->mode == 5u) {
			fixture_init(&fixture);
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 1u, 0u);
		} else if (test->mode == 6u) {
			fixture_init(&fixture);
			(void)dispatch_line(
				&fixture, 1u, "AIUS ID 7\n",
				AIUS_USB_COMMAND_ID, 7u);
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 1u, 0u);
		} else if (test->mode == 7u) {
			begin_short_normal_tx(&fixture, 0u);
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 1u, 0u);
		} else if (test->mode == 8u) {
			enter_bootloader(&fixture);
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 1u, 0u);
		} else if (test->mode == 9u) {
			begin_long_normal_tx(&fixture, 8u);
			action = complete_write(
				&fixture, 101u, AIUS_IO_PROGRESS, 8u, 0u, 0u);
			expect_none(&action);
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 1u, 0u);
		} else if (test->mode == 10u) {
			fixture_init(&fixture);
			(void)dispatch_line(
				&fixture, 1u, "AIUS ID 7\n",
				AIUS_USB_COMMAND_ID, 7u);
			input.application_response = &response;
			input.tx_event_present = true;
			input.tx_event = io_event(AIUS_IO_PROGRESS, 1u, 0u);
		} else {
			begin_short_normal_tx(&fixture, 0u);
			emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &write);
			input.application_response = &response;
			input.tx_event_present = true;
			input.tx_event =
				io_event(AIUS_IO_PROGRESS, write.tx_length, 0u);
		}
		before_faults = fixture.runtime.protocol_faults;
		action = runtime_step(&fixture, 102u, input);
		CHECK(fixture.runtime.protocol_faults ==
			before_faults +
				(test->mode == 2u || test->mode == 6u ? 2u : 1u));
		if (test->mode == 10u) {
			expect_none(&action);
			expect_response(&fixture, "AIUS/1 OK 7 ID x\n");
		} else if (test->mode == 11u) {
			expect_none(&action);
			CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
		} else {
			expect_none(&action);
		}
	}
}

static const KeyedCase rx_cases[] = {
	{"rx.length-0", 0u},
	{"rx.length-1", 1u},
	{"rx.terminal-at-63", 63u},
	{"rx.terminal-at-64", 64u},
	{"rx.terminal-at-65-retained", 65u},
	{"rx.terminal-cr-retained-suffix", 100u},
	{"rx.suppressed-lf", 101u},
	{"rx.terminal-lf-retained-suffix", 102u}
};

static void test_rx_registry(void)
{
	const char *keys[ARRAY_COUNT(rx_cases)];
	uint8_t bytes[65];
	for (size_t i = 0; i < ARRAY_COUNT(rx_cases); ++i)
		keys[i] = rx_cases[i].key;
	validate_unique_keys(
		"registry.rx", keys, ARRAY_COUNT(rx_cases), 8u);

	for (size_t i = 0; i < ARRAY_COUNT(rx_cases); ++i) {
		const KeyedCase *test = &rx_cases[i];
		RuntimeFixture fixture;
		AiusUsbRuntimeInput input = input_connected();
		AiusUsbRuntimeAction action;
		current_key = test->key;
		fixture_init(&fixture);
		memset(bytes, 'A', sizeof(bytes));

		if (test->mode <= 65u) {
			if (test->mode != 0u)
				input.rx = bytes;
			input.rx_length = test->mode;
			if (test->mode >= 63u)
				bytes[test->mode - 1u] = '\n';
			action = runtime_step(&fixture, 1u, input);
			if (test->mode < 63u) {
				expect_none(&action);
				CHECK(action.rx_consumed == test->mode);
				CHECK(fixture.runtime.parser.length == test->mode);
			} else if (test->mode <= AIUS_USB_RUNTIME_RX_QUOTA) {
				expect_none(&action);
				CHECK(action.rx_consumed == test->mode);
				CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
				CHECK(fixture.runtime.parser.length == 0u);
			} else {
				expect_none(&action);
				CHECK(action.rx_consumed ==
					AIUS_USB_RUNTIME_RX_QUOTA);
				CHECK(fixture.runtime.state ==
					AIUS_USB_RUNTIME_IDLE);
				CHECK(fixture.runtime.parser.length ==
					AIUS_USB_RUNTIME_RX_QUOTA);
				input.rx = bytes + AIUS_USB_RUNTIME_RX_QUOTA;
				input.rx_length = 1u;
				action = runtime_step(&fixture, 2u, input);
				expect_none(&action);
				CHECK(action.rx_consumed == 1u);
				CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
				CHECK(fixture.runtime.parser.length == 0u);
			}
		} else if (test->mode == 100u || test->mode == 102u) {
			const char *line = test->mode == 100u ?
				"AIUS ID 7\rTAIL" : "AIUS ID 7\nTAIL";
			input.rx = (const uint8_t *)line;
			input.rx_length = strlen(line);
			action = runtime_step(&fixture, 1u, input);
			CHECK(action.kind == AIUS_USB_ACTION_DISPATCH);
			CHECK(action.rx_consumed == strlen("AIUS ID 7\n"));
			CHECK(action.rx_consumed < input.rx_length);
			CHECK(action.request->command.request_id == 7u);
		} else {
			AiusUsbApplicationResponse response =
				success_response("ID", 2u, "x", 1u);
			AiusUsbRuntimeAction write;
			input.rx = (const uint8_t *)"AIUS ID 7\r";
			input.rx_length = strlen((const char *)input.rx);
			action = runtime_step(&fixture, 1u, input);
			CHECK(action.kind == AIUS_USB_ACTION_DISPATCH);
			action = supply_response(&fixture, 1u, &response, 0u);
			expect_none(&action);
			emit_write(&fixture, AIUS_USB_RUNTIME_TX_QUOTA, &write);
			action = complete_write(
				&fixture, 2u, AIUS_IO_PROGRESS,
				write.tx_length, 0u, 0u);
			expect_none(&action);
			input = input_connected();
			input.rx = (const uint8_t *)"\n";
			input.rx_length = 1u;
			action = runtime_step(&fixture, 3u, input);
			expect_none(&action);
			CHECK(action.rx_consumed == 1u);
			CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
			CHECK(fixture.runtime.parser.length == 0u);
			CHECK(fixture.runtime.parser.suppress_lf == 0u);
		}
	}
}

typedef enum {
	RESPONSE_SUCCESS,
	RESPONSE_ERROR,
	RESPONSE_INVALID,
	RESPONSE_CAPACITY
} ResponseCaseKind;

typedef struct {
	const char *key;
	ResponseCaseKind kind;
	unsigned variant;
} ResponseCase;

static const ResponseCase response_cases[] = {
	{"response.success.protocol", RESPONSE_SUCCESS, 0u},
	{"response.success.id", RESPONSE_SUCCESS, 1u},
	{"response.success.auth", RESPONSE_SUCCESS, 2u},
	{"response.success.bootloader", RESPONSE_SUCCESS, 3u},
	{"response.error.bad-version", RESPONSE_ERROR, AIUS_USB_ERROR_BAD_VERSION},
	{"response.error.bad-command", RESPONSE_ERROR, AIUS_USB_ERROR_BAD_COMMAND},
	{"response.error.bad-argument", RESPONSE_ERROR, AIUS_USB_ERROR_BAD_ARGUMENT},
	{"response.error.line-too-long", RESPONSE_ERROR, AIUS_USB_ERROR_LINE_TOO_LONG},
	{"response.error.busy", RESPONSE_ERROR, AIUS_USB_ERROR_BUSY},
	{"response.error.internal", RESPONSE_ERROR, AIUS_USB_ERROR_INTERNAL},
	{"response.invalid.verb-null", RESPONSE_INVALID, 0u},
	{"response.invalid.verb-zero", RESPONSE_INVALID, 1u},
	{"response.invalid.verb-too-long", RESPONSE_INVALID, 2u},
	{"response.invalid.verb-mapping", RESPONSE_INVALID, 3u},
	{"response.invalid.payload-null", RESPONSE_INVALID, 4u},
	{"response.invalid.payload-domain", RESPONSE_INVALID, 5u},
	{"response.capacity.invalid-47", RESPONSE_CAPACITY, 0u},
	{"response.capacity.minimum-48", RESPONSE_CAPACITY, 1u},
	{"response.capacity.primary-overflow-48", RESPONSE_CAPACITY, 2u},
	{"response.capacity.maximum-320", RESPONSE_CAPACITY, 3u},
	{"response.capacity.invalid-321", RESPONSE_CAPACITY, 4u}
};

static const char *error_code(AiusUsbError error)
{
	switch (error) {
	case AIUS_USB_ERROR_BAD_VERSION:
		return "BAD_VERSION";
	case AIUS_USB_ERROR_BAD_COMMAND:
		return "BAD_COMMAND";
	case AIUS_USB_ERROR_BAD_ARGUMENT:
		return "BAD_ARGUMENT";
	case AIUS_USB_ERROR_LINE_TOO_LONG:
		return "LINE_TOO_LONG";
	case AIUS_USB_ERROR_BUSY:
		return "BUSY";
	case AIUS_USB_ERROR_INTERNAL:
		return "INTERNAL";
	default:
		return "";
	}
}

static void test_invalid_response_variant(unsigned variant)
{
	RuntimeFixture fixture;
	AiusUsbApplicationResponse response;
	AiusUsbRuntimeAction action;
	char too_long_verb[11];
	char bad_payload[2] = {'x', '\x1f'};
	char overlong_payload[257];
	uint32_t before_faults;

	fixture_init(&fixture);
	(void)dispatch_line(
		&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
	memset(too_long_verb, 'I', sizeof(too_long_verb));
	memset(overlong_payload, 'p', sizeof(overlong_payload));
	response = success_response("ID", 2u, "x", 1u);
	if (variant == 0u) {
		response.verb = NULL;
	} else if (variant == 1u) {
		response.verb_length = 0u;
	} else if (variant == 2u) {
		response.verb = too_long_verb;
		response.verb_length = sizeof(too_long_verb);
	} else if (variant == 3u) {
		response.verb = "AUTH";
		response.verb_length = 4u;
	} else if (variant == 4u) {
		response.payload = NULL;
	} else {
		response.payload = bad_payload;
		response.payload_length = sizeof(bad_payload);
	}
	before_faults = fixture.runtime.protocol_faults;
	action = supply_response(&fixture, 1u, &response, 0u);
	expect_none(&action);
	CHECK(fixture.runtime.protocol_faults == before_faults + 1u);
	expect_response(&fixture, "AIUS/1 ERR 7 INTERNAL error\n");

	if (variant == 5u) {
		current_key = "response.invalid.payload-domain.overlong";
		fixture_init(&fixture);
		(void)dispatch_line(
			&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
		response = success_response(
			"ID", 2u, overlong_payload, sizeof(overlong_payload));
		action = supply_response(&fixture, 1u, &response, 0u);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == 1u);
		expect_response(&fixture, "AIUS/1 ERR 7 INTERNAL error\n");

		current_key = "response.invalid.payload-domain.nonnull-zero";
		fixture_init(&fixture);
		(void)dispatch_line(
			&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
		response = success_response("ID", 2u, "ignored", 0u);
		action = supply_response(&fixture, 1u, &response, 0u);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == 1u);
		expect_response(&fixture, "AIUS/1 ERR 7 INTERNAL error\n");

		current_key = "response.invalid.payload-domain.overlap";
		fixture_init(&fixture);
		(void)dispatch_line(
			&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
		response = success_response("ID", 2u, fixture.response, 1u);
		action = supply_response(&fixture, 1u, &response, 0u);
		expect_none(&action);
		CHECK(fixture.runtime.protocol_faults == 1u);
		expect_response(&fixture, "AIUS/1 ERR 7 INTERNAL error\n");
	}
}

static void test_response_registry(void)
{
	const char *keys[ARRAY_COUNT(response_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(response_cases); ++i)
		keys[i] = response_cases[i].key;
	validate_unique_keys(
		"registry.responses", keys, ARRAY_COUNT(response_cases), 21u);

	for (size_t i = 0; i < ARRAY_COUNT(response_cases); ++i) {
		const ResponseCase *test = &response_cases[i];
		RuntimeFixture fixture;
		AiusUsbApplicationResponse response;
		AiusUsbRuntimeAction action;
		char expected[96];
		char payload[256];
		current_key = test->key;

		if (test->kind == RESPONSE_SUCCESS) {
			const char *line;
			const char *verb;
			const char *body;
			AiusUsbCommandKind command;
			uint32_t request_id;
			if (test->variant == 0u) {
				line = "AIUS PROTOCOL 1\n";
				verb = "PROTOCOL";
				body = "1";
				command = AIUS_USB_COMMAND_PROTOCOL;
				request_id = 0u;
			} else if (test->variant == 1u) {
				line = "AIUS ID 7\n";
				verb = "ID";
				body = "build=x";
				command = AIUS_USB_COMMAND_ID;
				request_id = 7u;
			} else if (test->variant == 2u) {
				line = "AIUS AUTH 8\n";
				verb = "AUTH";
				body = "token";
				command = AIUS_USB_COMMAND_AUTH;
				request_id = 8u;
			} else {
				line = "AIUS BOOTLOADER 9\n";
				verb = "BOOTLOADER";
				body = NULL;
				command = AIUS_USB_COMMAND_BOOTLOADER;
				request_id = 9u;
			}
			fixture_init(&fixture);
			(void)dispatch_line(
				&fixture, 100u, line, command, request_id);
			response = success_response(
				verb, strlen(verb), body,
				body == NULL ? 0u : strlen(body));
			action = supply_response(&fixture, 100u, &response, 0u);
			expect_none(&action);
			if (test->variant == 0u)
				expect_response(
					&fixture, "AIUS/1 OK 0 PROTOCOL 1\n");
			else if (test->variant == 1u)
				expect_response(
					&fixture, "AIUS/1 OK 7 ID build=x\n");
			else if (test->variant == 2u)
				expect_response(
					&fixture, "AIUS/1 OK 8 AUTH token\n");
			else
				expect_response(
					&fixture,
					"AIUS/1 OK 9 BOOTLOADER entering\n");
			CHECK(fixture.runtime.protocol_faults == 0u);
		} else if (test->kind == RESPONSE_ERROR) {
			AiusUsbError error = (AiusUsbError)test->variant;
			bool bootloader_denied = error == AIUS_USB_ERROR_BUSY;
			uint32_t request_id = bootloader_denied ? 9u : 7u;
			fixture_init(&fixture);
			(void)dispatch_line(
				&fixture, 1u,
				bootloader_denied ?
					"AIUS BOOTLOADER 9\n" : "AIUS ID 7\n",
				bootloader_denied ?
					AIUS_USB_COMMAND_BOOTLOADER :
					AIUS_USB_COMMAND_ID,
				request_id);
			response = error_response(error);
			action = supply_response(&fixture, 1u, &response, 0u);
			expect_none(&action);
			(void)snprintf(expected, sizeof(expected),
				"AIUS/1 ERR %u %s error\n",
				request_id, error_code(error));
			expect_response(&fixture, expected);
			CHECK(fixture.runtime.protocol_faults == 0u);
			CHECK(!fixture.runtime.bootloader_authorized);
			if (bootloader_denied) {
				AiusUsbRuntimeInput disconnected;
				memset(&disconnected, 0, sizeof(disconnected));
				action = runtime_step(
					&fixture, 2u, disconnected);
				expect_none(&action);
				CHECK(fixture.runtime.state ==
					AIUS_USB_RUNTIME_IDLE);
				CHECK(!fixture.runtime.bootloader_authorized);
				CHECK(!fixture.runtime.enter_emitted);
			}
		} else if (test->kind == RESPONSE_INVALID) {
			test_invalid_response_variant(test->variant);
		} else if (test->variant == 0u || test->variant == 4u) {
			size_t capacity = test->variant == 0u ?
				AIUS_USB_RUNTIME_MIN_RESPONSE - 1u :
				AIUS_USB_RUNTIME_MAX_RESPONSE + 1u;
			fixture_init_with_capacity(&fixture, capacity);
			CHECK(!fixture.runtime.valid);
			CHECK(fixture.runtime.response_capacity == capacity);
		} else if (test->variant == 1u) {
			fixture_init_with_capacity(
				&fixture, AIUS_USB_RUNTIME_MIN_RESPONSE);
			CHECK(fixture.runtime.valid);
			(void)dispatch_line(
				&fixture, 1u, "AIUS ID 7\n",
				AIUS_USB_COMMAND_ID, 7u);
			response = success_response("ID", 2u, "x", 1u);
			action = supply_response(&fixture, 1u, &response, 0u);
			expect_none(&action);
			expect_response(&fixture, "AIUS/1 OK 7 ID x\n");
		} else if (test->variant == 2u) {
			fixture_init_with_capacity(
				&fixture, AIUS_USB_RUNTIME_MIN_RESPONSE);
			(void)dispatch_line(
				&fixture, 1u, "AIUS ID 7\n",
				AIUS_USB_COMMAND_ID, 7u);
			response = success_response(
				"ID", 2u, long_payload, sizeof(long_payload) - 1u);
			action = supply_response(&fixture, 1u, &response, 0u);
			expect_none(&action);
			CHECK(fixture.runtime.protocol_faults == 1u);
			expect_response(&fixture, "AIUS/1 ERR 7 INTERNAL error\n");
		} else {
			memset(payload, 'p', sizeof(payload));
			fixture_init_with_capacity(
				&fixture, AIUS_USB_RUNTIME_MAX_RESPONSE);
			(void)dispatch_line(
				&fixture, 1u, "AIUS ID 7\n",
				AIUS_USB_COMMAND_ID, 7u);
			response = success_response(
				"ID", 2u, payload, sizeof(payload));
			action = supply_response(&fixture, 1u, &response, 0u);
			expect_none(&action);
			CHECK(fixture.runtime.protocol_faults == 0u);
			CHECK(fixture.runtime.response_length ==
				strlen("AIUS/1 OK 7 ID ") + sizeof(payload) + 1u);
			CHECK(memcmp(fixture.response, "AIUS/1 OK 7 ID ",
				strlen("AIUS/1 OK 7 ID ")) == 0);
			CHECK(fixture.response[fixture.runtime.response_length - 1u] ==
				'\n');
		}
	}
}

static const KeyedCase deadline_cases[] = {
	{"deadline.nonwrap.before", 0u},
	{"deadline.nonwrap.equal", 1u},
	{"deadline.nonwrap.after", 2u},
	{"deadline.wrap.before", 3u},
	{"deadline.wrap.equal", 4u},
	{"deadline.wrap.after", 5u}
};

static void test_deadline_registry(void)
{
	const char *keys[ARRAY_COUNT(deadline_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(deadline_cases); ++i)
		keys[i] = deadline_cases[i].key;
	validate_unique_keys(
		"registry.deadlines", keys, ARRAY_COUNT(deadline_cases), 6u);

	for (size_t i = 0; i < ARRAY_COUNT(deadline_cases); ++i) {
		const KeyedCase *test = &deadline_cases[i];
		uint32_t start = test->mode < 3u ? 100u : UINT32_MAX - 250u;
		uint32_t deadline = start + AIUS_USB_BOOTLOADER_DELAY_MS;
		uint32_t now;
		current_key = test->key;
		if (test->mode % 3u == 0u)
			now = deadline - 1u;
		else if (test->mode % 3u == 1u)
			now = deadline;
		else
			now = deadline + 1u;

		for (unsigned entry = 0u; entry < 2u; ++entry) {
			RuntimeFixture fixture;
			AiusUsbRuntimeAction action;
			AiusUsbRuntimeInput input = input_connected();
			size_t expected_replacement = 7u;
			begin_boot_tx(
				&fixture, start,
				entry == 0u ? 0u : AIUS_USB_RUNTIME_TX_QUOTA,
				&action);
			if (entry == 0u) {
				expect_none(&action);
				CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
			} else {
				CHECK(action.kind == AIUS_USB_ACTION_WRITE);
				CHECK(fixture.runtime.state ==
					AIUS_USB_RUNTIME_ACK_PENDING);
				input.tx_event_present = true;
				input.tx_event =
					io_event(AIUS_IO_WOULD_BLOCK, 0u, 0u);
				input.tx_capacity = expected_replacement;
			}
			CHECK(fixture.runtime.bootloader_authorized);
			CHECK(fixture.runtime.bootloader_deadline_ms == deadline);

			action = runtime_step(&fixture, now, input);
			if (test->mode % 3u == 0u) {
				if (entry == 0u) {
					expect_none(&action);
				} else {
					CHECK(action.kind == AIUS_USB_ACTION_WRITE);
					CHECK(action.tx ==
						(const uint8_t *)fixture.response);
					CHECK(action.tx_length ==
						expected_replacement);
					CHECK(fixture.runtime.write_outstanding);
					CHECK(fixture.runtime.outstanding_length ==
						expected_replacement);
				}
				CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_TX);
				CHECK(fixture.runtime.bootloader_authorized);
			} else {
				CHECK(action.kind ==
					AIUS_USB_ACTION_ENTER_BOOTLOADER);
				CHECK(fixture.runtime.state ==
					AIUS_USB_RUNTIME_ENTER_BOOTLOADER);
				CHECK(fixture.runtime.enter_emitted);
			}
			CHECK(fixture.runtime.bootloader_deadline_ms == deadline);
			CHECK(fixture.runtime.protocol_faults == 0u);
		}
	}
}

static const KeyedCase reconnect_cases[] = {
	{"reconnect.incomplete-line", 0u},
	{"reconnect.dispatch-before-response", 1u},
	{"reconnect.normal-tx", 2u},
	{"reconnect.boot-before-authorization", 3u}
};

static void assert_reconnect_dispatch(RuntimeFixture *fixture)
{
	AiusUsbRuntimeInput input = input_connected();
	AiusUsbRuntimeAction action;
	input.rx = (const uint8_t *)"AIUS ID 44\n";
	input.rx_length = strlen((const char *)input.rx);
	action = runtime_step(fixture, 50u, input);
	CHECK(action.kind == AIUS_USB_ACTION_DISPATCH);
	CHECK(action.request->command.kind == AIUS_USB_COMMAND_ID);
	CHECK(action.request->command.request_id == 44u);
	CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_DISPATCH);
}

static void test_reconnect_registry(void)
{
	const char *keys[ARRAY_COUNT(reconnect_cases)];
	for (size_t i = 0; i < ARRAY_COUNT(reconnect_cases); ++i)
		keys[i] = reconnect_cases[i].key;
	validate_unique_keys(
		"registry.reconnect", keys, ARRAY_COUNT(reconnect_cases), 4u);

	for (size_t i = 0; i < ARRAY_COUNT(reconnect_cases); ++i) {
		const KeyedCase *test = &reconnect_cases[i];
		RuntimeFixture fixture;
		AiusUsbRuntimeInput input = input_connected();
		AiusUsbRuntimeAction action;
		AiusUsbRuntimeAction write;
		current_key = test->key;
		fixture_init(&fixture);
		if (test->mode == 0u) {
			input.rx = (const uint8_t *)"AIUS ID";
			input.rx_length = strlen((const char *)input.rx);
			action = runtime_step(&fixture, 1u, input);
			expect_none(&action);
		} else if (test->mode == 1u) {
			(void)dispatch_line(
				&fixture, 1u, "AIUS AUTH 4\n",
				AIUS_USB_COMMAND_AUTH, 4u);
		} else if (test->mode == 2u) {
			begin_short_normal_tx(&fixture, 0u);
			emit_write(&fixture, 8u, &write);
		} else {
			(void)dispatch_line(
				&fixture, 1u, "AIUS BOOTLOADER 4\n",
				AIUS_USB_COMMAND_BOOTLOADER, 4u);
		}
		memset(&input, 0, sizeof(input));
		action = runtime_step(&fixture, 2u, input);
		expect_none(&action);
		CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
		CHECK(fixture.runtime.parser.length == 0u);
		CHECK(fixture.runtime.response_length == 0u);
		CHECK(fixture.runtime.response_offset == 0u);
		CHECK(!fixture.runtime.write_outstanding);
		CHECK(!fixture.runtime.bootloader_authorized);
		CHECK(!fixture.runtime.enter_emitted);
		assert_reconnect_dispatch(&fixture);
	}
}

/* One complete authorized BOOTLOADER transaction driven from IDLE: request,
 * application response, final write, write completion, entry action. Unlike
 * enter_bootloader() it does not reinitialize the fixture, so a single runtime
 * can be walked through several attempts in a row. */
static void boot_attempt(
	RuntimeFixture *fixture,
	uint32_t now_ms,
	uint32_t request_id,
	const char *line)
{
	AiusUsbApplicationResponse response =
		success_response("BOOTLOADER", 10u, NULL, 0u);
	AiusUsbRuntimeAction action;

	(void)dispatch_line(
		fixture, now_ms, line, AIUS_USB_COMMAND_BOOTLOADER, request_id);
	action = supply_response(
		fixture, now_ms, &response, AIUS_USB_RUNTIME_TX_QUOTA);
	CHECK(action.kind == AIUS_USB_ACTION_WRITE);
	CHECK(action.tx_length == fixture->runtime.response_length);
	CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_ACK_PENDING);
	action = complete_write(
		fixture, now_ms + 1u, AIUS_IO_PROGRESS, action.tx_length, 0u, 0u);
	CHECK(action.kind == AIUS_USB_ACTION_ENTER_BOOTLOADER);
	CHECK(fixture->runtime.state == AIUS_USB_RUNTIME_ENTER_BOOTLOADER);
	CHECK(fixture->runtime.enter_emitted);
}

static const KeyedCase retry_cases[] = {
	{"retry.second-request-served", 0u},
	{"retry.third-request-served", 1u},
	{"retry.failure-counter-saturates", 2u}
};

/* A software bootloader entry that fails leaves the board running, so every
 * later AIUS BOOTLOADER must be answered and retried rather than swallowed. */
static void test_retry_registry(void)
{
	static const char *const lines[] = {
		"AIUS BOOTLOADER 1\n",
		"AIUS BOOTLOADER 2\n",
		"AIUS BOOTLOADER 3\n"
	};
	static const char *const responses[] = {
		"AIUS/1 OK 1 BOOTLOADER entering\n",
		"AIUS/1 OK 2 BOOTLOADER entering\n",
		"AIUS/1 OK 3 BOOTLOADER entering\n"
	};
	const char *keys[ARRAY_COUNT(retry_cases)];

	for (size_t i = 0; i < ARRAY_COUNT(retry_cases); ++i)
		keys[i] = retry_cases[i].key;
	validate_unique_keys(
		"registry.retry", keys, ARRAY_COUNT(retry_cases), 3u);

	for (size_t i = 0; i < ARRAY_COUNT(retry_cases); ++i) {
		const KeyedCase *test = &retry_cases[i];
		RuntimeFixture fixture;
		AiusUsbRuntimeAction action;
		AiusUsbRuntimeInput input;
		const unsigned attempts = test->mode == 1u ? 3u : 2u;
		current_key = test->key;
		fixture_init(&fixture);

		if (test->mode == 2u) {
			boot_attempt(&fixture, 100u, 1u, lines[0]);
			fixture.runtime.bootloader_entry_failures = UINT32_MAX;
			input = input_connected();
			action = runtime_step(&fixture, 200u, input);
			expect_none(&action);
			CHECK(fixture.runtime.bootloader_entry_failures ==
				UINT32_MAX);
			CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
			continue;
		}

		for (unsigned attempt = 0u; attempt < attempts; ++attempt) {
			const uint32_t now = 100u + (uint32_t)attempt * 10u;
			boot_attempt(&fixture, now, attempt + 1u, lines[attempt]);
			expect_response(&fixture, responses[attempt]);
			/* The entry action returned, so the next step releases
			 * the terminal state and the runtime is servable again. */
			input = input_connected();
			action = runtime_step(&fixture, now + 2u, input);
			expect_none(&action);
			CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
			CHECK(!fixture.runtime.enter_emitted);
			CHECK(!fixture.runtime.bootloader_authorized);
			CHECK(fixture.runtime.bootloader_entry_failures ==
				attempt + 1u);
		}
		CHECK(fixture.runtime.protocol_faults == 0u);
	}
}

static void test_partial_multi_slice_tx(void)
{
	RuntimeFixture fixture;
	AiusUsbRuntimeAction first;
	AiusUsbRuntimeAction second;
	AiusUsbRuntimeAction third;
	AiusUsbRuntimeAction action;
	size_t expected_offset;

	current_key = "tx.partial-multi-slice";
	begin_long_normal_tx(&fixture, 0u);
	emit_write(&fixture, 17u, &first);
	CHECK(first.tx_length == 17u);
	action = complete_write(
		&fixture, 101u, AIUS_IO_PROGRESS, 5u, 0u, 13u);
	CHECK(action.kind == AIUS_USB_ACTION_WRITE);
	second = action;
	CHECK(second.tx == (const uint8_t *)fixture.response + 5u);
	CHECK(second.tx_length == 13u);
	CHECK(fixture.runtime.response_offset == 5u);
	action = complete_write(
		&fixture, 102u, AIUS_IO_PROGRESS, 13u, 0u, 64u);
	CHECK(action.kind == AIUS_USB_ACTION_WRITE);
	third = action;
	CHECK(third.tx == (const uint8_t *)fixture.response + 18u);
	CHECK(third.tx_length == AIUS_USB_RUNTIME_TX_QUOTA);
	CHECK(fixture.runtime.response_offset == 18u);
	expected_offset = 18u;
	while (fixture.runtime.state != AIUS_USB_RUNTIME_IDLE) {
		size_t accepted = fixture.runtime.outstanding_length;
		action = complete_write(
			&fixture, 103u, AIUS_IO_PROGRESS, accepted, 0u,
			AIUS_USB_RUNTIME_TX_QUOTA);
		expected_offset += accepted;
		CHECK(fixture.runtime.response_offset == expected_offset ||
			fixture.runtime.state == AIUS_USB_RUNTIME_IDLE);
		if (fixture.runtime.state == AIUS_USB_RUNTIME_IDLE)
			expect_none(&action);
		else
			CHECK(action.kind == AIUS_USB_ACTION_WRITE);
	}
	CHECK(expected_offset == strlen("AIUS/1 OK 7 AUTH ") +
		sizeof(long_payload) - 1u + 1u);
}

static void test_fault_saturation_and_channel_order(void)
{
	RuntimeFixture fixture;
	AiusUsbRuntimeInput input;
	AiusUsbRuntimeAction action;
	AiusUsbApplicationResponse response;

	current_key = "counter.saturates";
	fixture_init(&fixture);
	fixture.runtime.protocol_faults = UINT32_MAX - 1u;
	input = input_connected();
	input.rx_length = 1u;
	action = runtime_step(&fixture, 1u, input);
	expect_none(&action);
	CHECK(fixture.runtime.protocol_faults == UINT32_MAX);
	action = runtime_step(&fixture, 2u, input);
	expect_none(&action);
	CHECK(fixture.runtime.protocol_faults == UINT32_MAX);

	current_key = "channel-order.response-before-disconnect";
	fixture_init(&fixture);
	(void)dispatch_line(
		&fixture, 100u, "AIUS BOOTLOADER 9\n",
		AIUS_USB_COMMAND_BOOTLOADER, 9u);
	response = success_response("BOOTLOADER", 10u, NULL, 0u);
	memset(&input, 0, sizeof(input));
	input.application_response = &response;
	action = runtime_step(&fixture, 100u, input);
	CHECK(action.kind == AIUS_USB_ACTION_ENTER_BOOTLOADER);
	CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_ENTER_BOOTLOADER);

	current_key = "boot.authorization.connection-loss";
	begin_boot_tx(&fixture, 100u, 0u, &action);
	expect_none(&action);
	memset(&input, 0, sizeof(input));
	action = runtime_step(&fixture, 101u, input);
	CHECK(action.kind == AIUS_USB_ACTION_ENTER_BOOTLOADER);
	CHECK(fixture.runtime.state == AIUS_USB_RUNTIME_ENTER_BOOTLOADER);

	current_key = "channel-order.rx-fault-does-not-preempt-response";
	fixture_init(&fixture);
	(void)dispatch_line(
		&fixture, 1u, "AIUS ID 7\n", AIUS_USB_COMMAND_ID, 7u);
	response = success_response("ID", 2u, "x", 1u);
	input = input_connected();
	input.application_response = &response;
	input.rx_length = 1u;
	action = runtime_step(&fixture, 1u, input);
	expect_none(&action);
	CHECK(fixture.runtime.protocol_faults == 1u);
	expect_response(&fixture, "AIUS/1 OK 7 ID x\n");
}

int main(void)
{
	test_init_and_prologues();
	test_legacy_parser_and_formatters();
	test_transition_registry();
	test_malformed_tx_registry();
	test_unsolicited_registry();
	test_rx_registry();
	test_response_registry();
	test_deadline_registry();
	test_reconnect_registry();
	test_retry_registry();
	test_partial_multi_slice_tx();
	test_fault_saturation_and_channel_order();
	puts("ok usb control: 24 families, 36 variants, 12 malformed tx, "
		"12 unsolicited, 8 rx, 21 responses, 6 deadlines, 4 reconnect, "
		"3 retry");
	return 0;
}
