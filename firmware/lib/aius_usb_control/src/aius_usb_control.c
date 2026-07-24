#include "aius_usb_control.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	char *output;
	size_t length;
} AiusUsbResponseWriter;

static void clear_event(AiusUsbEvent *event)
{
	event->type = AIUS_USB_EVENT_NONE;
	event->command.kind = AIUS_USB_COMMAND_NONE;
	event->command.request_id = 0;
	event->error = AIUS_USB_ERROR_NONE;
}

static int token(const char **cursor, const char *end, const char **start, size_t *length)
{
	while (*cursor < end && **cursor == ' ')
		(*cursor)++;
	if (*cursor == end)
		return 0;
	*start = *cursor;
	while (*cursor < end && **cursor != ' ')
		(*cursor)++;
	*length = (size_t)(*cursor - *start);
	return 1;
}

static int token_is(const char *start, size_t length, const char *value)
{
	size_t value_length = strlen(value);
	return length == value_length && memcmp(start, value, length) == 0;
}

static int parse_id(const char *start, size_t length, uint32_t *result)
{
	uint32_t value = 0;
	if (length == 0 || length > 10)
		return 0;
	for (size_t i = 0; i < length; i++) {
		if (start[i] < '0' || start[i] > '9')
			return 0;
		uint32_t digit = (uint32_t)(start[i] - '0');
		if (value > (UINT32_MAX - digit) / 10u)
			return 0;
		value = value * 10u + digit;
	}
	if (value == 0)
		return 0;
	*result = value;
	return 1;
}

static AiusUsbError parse_line(const char *line, size_t length, AiusUsbCommand *command)
{
	const char *cursor = line;
	const char *end = line + length;
	const char *first;
	const char *second;
	const char *third = NULL;
	size_t first_length;
	size_t second_length;
	size_t third_length = 0;
	uint32_t request_id;
	if (!token(&cursor, end, &first, &first_length) || !token(&cursor, end, &second, &second_length))
		return AIUS_USB_ERROR_BAD_COMMAND;
	if (cursor != end && !token(&cursor, end, &third, &third_length))
		return AIUS_USB_ERROR_BAD_ARGUMENT;
	if (cursor != end)
		return AIUS_USB_ERROR_BAD_ARGUMENT;
	if (!token_is(first, first_length, "AIUS"))
		return AIUS_USB_ERROR_BAD_COMMAND;
	if (token_is(second, second_length, "PROTOCOL")) {
		if (third == NULL || third_length != 1 || third[0] != '1')
			return AIUS_USB_ERROR_BAD_VERSION;
		command->kind = AIUS_USB_COMMAND_PROTOCOL;
		command->request_id = 0;
		return AIUS_USB_ERROR_NONE;
	}
	if (token_is(second, second_length, "AUTH") && third == NULL) {
		command->kind = AIUS_USB_COMMAND_AUTH;
		command->request_id = 0;
		return AIUS_USB_ERROR_NONE;
	}
	if (third == NULL || !parse_id(third, third_length, &request_id))
		return AIUS_USB_ERROR_BAD_ARGUMENT;
	if (token_is(second, second_length, "ID"))
		command->kind = AIUS_USB_COMMAND_ID;
	else if (token_is(second, second_length, "AUTH"))
		command->kind = AIUS_USB_COMMAND_AUTH;
	else if (token_is(second, second_length, "BOOTLOADER"))
		command->kind = AIUS_USB_COMMAND_BOOTLOADER;
	else
		return AIUS_USB_ERROR_BAD_COMMAND;
	command->request_id = request_id;
	return AIUS_USB_ERROR_NONE;
}

static void finish(AiusUsbParser *parser, AiusUsbEvent *event)
{
	AiusUsbError error;
	clear_event(event);
	if (parser->invalid) {
		event->type = AIUS_USB_EVENT_ERROR;
		event->error = AIUS_USB_ERROR_BAD_ARGUMENT;
	} else if (parser->discarding) {
		event->type = AIUS_USB_EVENT_ERROR;
		event->error = AIUS_USB_ERROR_LINE_TOO_LONG;
	} else if (parser->length == 0) {
		event->type = AIUS_USB_EVENT_ERROR;
		event->error = AIUS_USB_ERROR_BAD_COMMAND;
	} else {
		parser->line[parser->length] = '\0';
		error = parse_line(parser->line, parser->length, &event->command);
		if (error == AIUS_USB_ERROR_NONE)
			event->type = AIUS_USB_EVENT_COMMAND;
		else {
			event->type = AIUS_USB_EVENT_ERROR;
			event->error = error;
		}
	}
	parser->length = 0;
	parser->discarding = 0;
	parser->invalid = 0;
}

void aius_usb_control_init(AiusUsbParser *parser)
{
	memset(parser, 0, sizeof(*parser));
}

int aius_usb_control_feed(AiusUsbParser *parser, uint8_t byte, AiusUsbEvent *event)
{
	clear_event(event);
	if (byte == '\n') {
		if (parser->suppress_lf) {
			parser->suppress_lf = 0;
			return 0;
		}
		finish(parser, event);
		return 1;
	}
	if (byte == '\r') {
		finish(parser, event);
		parser->suppress_lf = 1;
		return 1;
	}
	parser->suppress_lf = 0;
	if (byte < 0x20u || byte > 0x7eu) {
		if (!parser->discarding)
			parser->length = 0;
		parser->discarding = 1;
		parser->invalid = 1;
		return 0;
	}
	if (parser->discarding)
		return 0;
	if (parser->length >= AIUS_USB_CONTROL_MAX_LINE) {
		parser->discarding = 1;
		return 0;
	}
	parser->line[parser->length++] = (char)byte;
	return 0;
}

size_t aius_usb_control_write_ok(char *output, size_t capacity, uint32_t request_id, const char *verb, const char *payload)
{
	int length = snprintf(output, capacity, "AIUS/1 OK %lu %s%s%s\n", (unsigned long)request_id, verb, payload != NULL && payload[0] != '\0' ? " " : "", payload != NULL ? payload : "");
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}

size_t aius_usb_control_write_error(char *output, size_t capacity, uint32_t request_id, AiusUsbError error)
{
	static const char *const names[] = {"NONE", "BAD_VERSION", "BAD_COMMAND", "BAD_ARGUMENT", "LINE_TOO_LONG", "BUSY", "INTERNAL"};
	const char *name = error <= AIUS_USB_ERROR_INTERNAL ? names[error] : "INTERNAL";
	int length = snprintf(output, capacity, "AIUS/1 ERR %lu %s error\n", (unsigned long)request_id, name);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}

static void runtime_action_clear(AiusUsbRuntimeAction *action)
{
	action->kind = AIUS_USB_ACTION_NONE;
	action->request = NULL;
	action->tx = NULL;
	action->tx_length = 0;
	action->rx_consumed = 0;
}

static void runtime_fault(AiusUsbRuntime *runtime)
{
	if (runtime->protocol_faults != UINT32_MAX)
		runtime->protocol_faults++;
}

static size_t decimal_length(uint32_t value)
{
	size_t length = 1;
	while (value >= 10u) {
		value /= 10u;
		length++;
	}
	return length;
}

static void writer_append(
	AiusUsbResponseWriter *writer,
	const char *value,
	size_t length)
{
	memcpy(writer->output + writer->length, value, length);
	writer->length += length;
}

static void writer_append_id(AiusUsbResponseWriter *writer, uint32_t value)
{
	char digits[10];
	size_t length = 0;
	do {
		digits[length++] = (char)('0' + (value % 10u));
		value /= 10u;
	} while (value != 0u);
	while (length != 0u) {
		length--;
		writer->output[writer->length++] = digits[length];
	}
}

static bool runtime_format_ok(
	AiusUsbRuntime *runtime,
	uint32_t request_id,
	const char *verb,
	size_t verb_length,
	const char *payload,
	size_t payload_length)
{
	static const char prefix[] = "AIUS/1 OK ";
	const size_t required =
		(sizeof(prefix) - 1u) + decimal_length(request_id) + 1u +
		verb_length + (payload_length != 0u ? 1u + payload_length : 0u) +
		1u;
	AiusUsbResponseWriter writer;
	if (required > runtime->response_capacity)
		return false;
	writer.output = runtime->response;
	writer.length = 0;
	writer_append(&writer, prefix, sizeof(prefix) - 1u);
	writer_append_id(&writer, request_id);
	writer_append(&writer, " ", 1u);
	writer_append(&writer, verb, verb_length);
	if (payload_length != 0u) {
		writer_append(&writer, " ", 1u);
		writer_append(&writer, payload, payload_length);
	}
	writer_append(&writer, "\n", 1u);
	runtime->response_length = writer.length;
	runtime->response_offset = 0;
	return true;
}

static const char *runtime_error_name(
	AiusUsbError error,
	size_t *length)
{
	switch (error) {
	case AIUS_USB_ERROR_BAD_VERSION:
		*length = sizeof("BAD_VERSION") - 1u;
		return "BAD_VERSION";
	case AIUS_USB_ERROR_BAD_COMMAND:
		*length = sizeof("BAD_COMMAND") - 1u;
		return "BAD_COMMAND";
	case AIUS_USB_ERROR_BAD_ARGUMENT:
		*length = sizeof("BAD_ARGUMENT") - 1u;
		return "BAD_ARGUMENT";
	case AIUS_USB_ERROR_LINE_TOO_LONG:
		*length = sizeof("LINE_TOO_LONG") - 1u;
		return "LINE_TOO_LONG";
	case AIUS_USB_ERROR_BUSY:
		*length = sizeof("BUSY") - 1u;
		return "BUSY";
	case AIUS_USB_ERROR_INTERNAL:
	default:
		*length = sizeof("INTERNAL") - 1u;
		return "INTERNAL";
	}
}

static void runtime_format_error(
	AiusUsbRuntime *runtime,
	uint32_t request_id,
	AiusUsbError error)
{
	static const char prefix[] = "AIUS/1 ERR ";
	static const char suffix[] = " error\n";
	size_t name_length;
	const char *name = runtime_error_name(error, &name_length);
	AiusUsbResponseWriter writer;
	writer.output = runtime->response;
	writer.length = 0;
	writer_append(&writer, prefix, sizeof(prefix) - 1u);
	writer_append_id(&writer, request_id);
	writer_append(&writer, " ", 1u);
	writer_append(&writer, name, name_length);
	writer_append(&writer, suffix, sizeof(suffix) - 1u);
	runtime->response_length = writer.length;
	runtime->response_offset = 0;
}

static bool span_overlaps_response(
	const AiusUsbRuntime *runtime,
	const void *span,
	size_t length)
{
	uintptr_t span_start;
	uintptr_t response_start;
	uintptr_t span_last;
	uintptr_t response_last;
	if (length == 0u)
		return false;
	span_start = (uintptr_t)span;
	response_start = (uintptr_t)runtime->response;
	if (span_start > UINTPTR_MAX - (length - 1u))
		return true;
	span_last = span_start + length - 1u;
	response_last = response_start + runtime->response_capacity - 1u;
	return span_start <= response_last && response_start <= span_last;
}

static bool printable_ascii_chunk(
	const char *payload,
	size_t offset,
	size_t length)
{
	size_t index;
	for (index = 0; index < length; index++) {
		const unsigned char byte = (unsigned char)payload[offset + index];
		if (byte < 0x20u || byte > 0x7eu)
			return false;
	}
	return true;
}

static bool printable_ascii(const char *payload, size_t length)
{
	const size_t first = length < 64u ? length : 64u;
	const size_t second = length - first < 64u ? length - first : 64u;
	const size_t third =
		length - first - second < 64u ? length - first - second : 64u;
	const size_t fourth = length - first - second - third;
	return printable_ascii_chunk(payload, 0u, first) &&
		printable_ascii_chunk(payload, first, second) &&
		printable_ascii_chunk(payload, first + second, third) &&
		printable_ascii_chunk(payload, first + second + third, fourth);
}

static const char *runtime_command_verb(
	AiusUsbCommandKind command,
	size_t *length)
{
	switch (command) {
	case AIUS_USB_COMMAND_PROTOCOL:
		*length = sizeof("PROTOCOL") - 1u;
		return "PROTOCOL";
	case AIUS_USB_COMMAND_ID:
		*length = sizeof("ID") - 1u;
		return "ID";
	case AIUS_USB_COMMAND_AUTH:
		*length = sizeof("AUTH") - 1u;
		return "AUTH";
	case AIUS_USB_COMMAND_BOOTLOADER:
		*length = sizeof("BOOTLOADER") - 1u;
		return "BOOTLOADER";
	case AIUS_USB_COMMAND_NONE:
	default:
		*length = 0;
		return NULL;
	}
}

static bool runtime_success_valid(
	const AiusUsbRuntime *runtime,
	const AiusUsbApplicationResponse *response)
{
	size_t expected_length;
	const char *expected =
		runtime_command_verb(runtime->pending_request.command.kind, &expected_length);
	if (expected == NULL || response->verb == NULL ||
		response->verb_length < 1u || response->verb_length > 10u ||
		response->verb_length != expected_length ||
		span_overlaps_response(runtime, response->verb, response->verb_length) ||
		memcmp(response->verb, expected, expected_length) != 0)
		return false;
	if ((response->payload == NULL) != (response->payload_length == 0u) ||
		response->payload_length > 256u)
		return false;
	if (response->payload_length != 0u &&
		(span_overlaps_response(
			 runtime,
			 response->payload,
			 response->payload_length) ||
		 !printable_ascii(response->payload, response->payload_length)))
		return false;
	return runtime->pending_request.command.kind != AIUS_USB_COMMAND_BOOTLOADER ||
		response->payload_length == 0u;
}

static bool runtime_error_valid(
	const AiusUsbApplicationResponse *response)
{
	return response->error >= AIUS_USB_ERROR_BAD_VERSION &&
		response->error <= AIUS_USB_ERROR_INTERNAL &&
		response->verb == NULL && response->verb_length == 0u &&
		response->payload == NULL && response->payload_length == 0u;
}

static void runtime_clear_transaction(
	AiusUsbRuntime *runtime,
	bool clear_parser)
{
	if (clear_parser)
		aius_usb_control_init(&runtime->parser);
	runtime->state = AIUS_USB_RUNTIME_IDLE;
	runtime->response_length = 0;
	runtime->response_offset = 0;
	runtime->bootloader_deadline_ms = 0;
	runtime->outstanding_length = 0;
	runtime->pending_request.command.kind = AIUS_USB_COMMAND_NONE;
	runtime->pending_request.command.request_id = 0;
	runtime->write_outstanding = false;
	runtime->dispatch_emitted = false;
	runtime->bootloader_authorized = false;
	runtime->enter_emitted = false;
}

static void runtime_select_bootloader(
	AiusUsbRuntime *runtime,
	AiusUsbRuntimeAction *action)
{
	runtime->state = AIUS_USB_RUNTIME_ENTER_BOOTLOADER;
	runtime->write_outstanding = false;
	runtime->outstanding_length = 0;
	if (!runtime->enter_emitted) {
		runtime->enter_emitted = true;
		action->kind = AIUS_USB_ACTION_ENTER_BOOTLOADER;
	}
}

static bool tx_event_valid(
	const AiusIoEvent *event,
	size_t outstanding_length)
{
	switch (event->result) {
	case AIUS_IO_PROGRESS:
		return event->bytes >= 1u &&
			event->bytes <= outstanding_length &&
			event->fault == 0u;
	case AIUS_IO_WOULD_BLOCK:
	case AIUS_IO_CLOSED:
		return event->bytes == 0u && event->fault == 0u;
	case AIUS_IO_FAULT:
		return event->bytes == 0u && event->fault != 0u;
	default:
		return false;
	}
}

static void runtime_handle_tx_event(
	AiusUsbRuntime *runtime,
	bool event_present,
	const AiusIoEvent *event,
	AiusUsbRuntimeAction *action)
{
	const bool authorized = runtime->bootloader_authorized;
	const size_t outstanding_length = runtime->outstanding_length;
	runtime->write_outstanding = false;
	runtime->outstanding_length = 0;
	if (!event_present || !tx_event_valid(event, outstanding_length)) {
		runtime_fault(runtime);
		if (authorized)
			runtime->state = AIUS_USB_RUNTIME_TX;
		else
			runtime_clear_transaction(runtime, false);
		return;
	}
	switch (event->result) {
	case AIUS_IO_PROGRESS:
		runtime->response_offset += event->bytes;
		if (runtime->response_offset == runtime->response_length) {
			if (authorized)
				runtime_select_bootloader(runtime, action);
			else
				runtime_clear_transaction(runtime, false);
		} else {
			runtime->state = AIUS_USB_RUNTIME_TX;
		}
		break;
	case AIUS_IO_WOULD_BLOCK:
		runtime->state = AIUS_USB_RUNTIME_TX;
		break;
	case AIUS_IO_CLOSED:
	case AIUS_IO_FAULT:
		if (authorized)
			runtime_select_bootloader(runtime, action);
		else
			runtime_clear_transaction(runtime, false);
		break;
	default:
		break;
	}
}

static void runtime_handle_application_response(
	AiusUsbRuntime *runtime,
	uint32_t now_ms,
	const AiusUsbApplicationResponse *response)
{
	const uint32_t request_id = runtime->pending_request.command.request_id;
	bool valid = response != NULL;
	if (valid && response->error == AIUS_USB_ERROR_NONE) {
		valid = runtime_success_valid(runtime, response);
		if (valid &&
			runtime->pending_request.command.kind == AIUS_USB_COMMAND_BOOTLOADER) {
			valid = runtime_format_ok(
				runtime,
				request_id,
				"BOOTLOADER",
				sizeof("BOOTLOADER") - 1u,
				"entering",
				sizeof("entering") - 1u);
			if (valid) {
				runtime->bootloader_authorized = true;
				runtime->bootloader_deadline_ms =
					now_ms + AIUS_USB_BOOTLOADER_DELAY_MS;
			}
		} else if (valid) {
			valid = runtime_format_ok(
				runtime,
				request_id,
				response->verb,
				response->verb_length,
				response->payload,
				response->payload_length);
		}
	} else if (valid) {
		valid = runtime_error_valid(response);
		if (valid)
			runtime_format_error(runtime, request_id, response->error);
	}
	if (!valid) {
		runtime_fault(runtime);
		runtime->bootloader_authorized = false;
		runtime->bootloader_deadline_ms = 0;
		runtime_format_error(
			runtime,
			request_id,
			AIUS_USB_ERROR_INTERNAL);
	}
	runtime->state = AIUS_USB_RUNTIME_TX;
	runtime->dispatch_emitted = false;
	runtime->pending_request.command.kind = AIUS_USB_COMMAND_NONE;
	runtime->pending_request.command.request_id = 0;
}

static void runtime_emit_write(
	AiusUsbRuntime *runtime,
	size_t tx_capacity,
	AiusUsbRuntimeAction *action)
{
	size_t length;
	if (action->kind != AIUS_USB_ACTION_NONE ||
		runtime->state != AIUS_USB_RUNTIME_TX ||
		runtime->write_outstanding ||
		runtime->response_offset == runtime->response_length ||
		tx_capacity == 0u)
		return;
	length = runtime->response_length - runtime->response_offset;
	if (length > AIUS_USB_RUNTIME_TX_QUOTA)
		length = AIUS_USB_RUNTIME_TX_QUOTA;
	if (length > tx_capacity)
		length = tx_capacity;
	action->kind = AIUS_USB_ACTION_WRITE;
	action->tx =
		(const uint8_t *)runtime->response + runtime->response_offset;
	action->tx_length = length;
	runtime->write_outstanding = true;
	runtime->outstanding_length = length;
	if (runtime->bootloader_authorized &&
		runtime->response_offset + length == runtime->response_length)
		runtime->state = AIUS_USB_RUNTIME_ACK_PENDING;
}

void aius_usb_runtime_init(
	AiusUsbRuntime *runtime,
	char *response,
	size_t response_capacity)
{
	if (runtime == NULL)
		return;
	memset(runtime, 0, sizeof(*runtime));
	runtime->response = response;
	runtime->response_capacity = response_capacity;
	runtime->state = AIUS_USB_RUNTIME_IDLE;
	runtime->valid = response != NULL &&
		response_capacity >= AIUS_USB_RUNTIME_MIN_RESPONSE &&
		response_capacity <= AIUS_USB_RUNTIME_MAX_RESPONSE;
}

void aius_usb_runtime_step(
	AiusUsbRuntime *runtime,
	uint32_t now_ms,
	AiusUsbRuntimeInput input,
	AiusUsbRuntimeAction *action)
{
	AiusUsbRuntimeState entry_state;
	bool rx_valid;
	bool expected_write;
	if (action == NULL)
		return;
	runtime_action_clear(action);
	if (runtime == NULL || !runtime->valid)
		return;

	entry_state = runtime->state;
	expected_write = runtime->write_outstanding;
	rx_valid = (input.rx == NULL) == (input.rx_length == 0u);
	if (rx_valid && input.rx_length != 0u)
		rx_valid = !span_overlaps_response(
			runtime,
			input.rx,
			input.rx_length);
	if (!rx_valid)
		runtime_fault(runtime);

	if (entry_state == AIUS_USB_RUNTIME_DISPATCH) {
		runtime_handle_application_response(
			runtime,
			now_ms,
			input.application_response);
	} else if (input.application_response != NULL) {
		runtime_fault(runtime);
	}

	if (expected_write) {
		runtime_handle_tx_event(
			runtime,
			input.tx_event_present,
			&input.tx_event,
			action);
	} else if (input.tx_event_present) {
		runtime_fault(runtime);
	}

	if (!input.connected) {
		if (runtime->bootloader_authorized)
			runtime_select_bootloader(runtime, action);
		else
			runtime_clear_transaction(runtime, true);
		return;
	}

	if (runtime->bootloader_authorized &&
		(runtime->state == AIUS_USB_RUNTIME_TX ||
		 runtime->state == AIUS_USB_RUNTIME_ACK_PENDING) &&
		(int32_t)(now_ms - runtime->bootloader_deadline_ms) >= 0) {
		runtime_select_bootloader(runtime, action);
		return;
	}

	if (runtime->state == AIUS_USB_RUNTIME_ENTER_BOOTLOADER)
		return;

	if (entry_state == AIUS_USB_RUNTIME_IDLE && rx_valid) {
		const size_t quota = input.rx_length < AIUS_USB_RUNTIME_RX_QUOTA ?
			input.rx_length :
			AIUS_USB_RUNTIME_RX_QUOTA;
		size_t index;
		for (index = 0; index < quota; index++) {
			AiusUsbEvent event;
			const int terminal =
				aius_usb_control_feed(&runtime->parser, input.rx[index], &event);
			action->rx_consumed = index + 1u;
			if (terminal != 0) {
				if (event.type == AIUS_USB_EVENT_COMMAND) {
					runtime->pending_request.command = event.command;
					runtime->state = AIUS_USB_RUNTIME_DISPATCH;
					runtime->dispatch_emitted = true;
					action->kind = AIUS_USB_ACTION_DISPATCH;
					action->request = &runtime->pending_request;
				} else {
					runtime_format_error(runtime, 0u, event.error);
					runtime->state = AIUS_USB_RUNTIME_TX;
				}
				break;
			}
		}
	}

	runtime_emit_write(runtime, input.tx_capacity, action);
}
