#include "aius_usb_control.h"

#include <stdio.h>
#include <string.h>

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
