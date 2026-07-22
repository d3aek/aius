#include "aius_usb_control.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static AiusUsbEvent feed(AiusUsbParser *parser, const char *text)
{
	AiusUsbEvent event = {0};
	for (size_t i = 0; text[i] != '\0'; i++)
		if (aius_usb_control_feed(parser, (uint8_t)text[i], &event))
			return event;
	return event;
}

static void test_commands(void)
{
	AiusUsbParser parser;
	AiusUsbEvent event;
	aius_usb_control_init(&parser);
	event = feed(&parser, "AIUS PROTOCOL 1\n");
	assert(event.command.kind == AIUS_USB_COMMAND_PROTOCOL);
	event = feed(&parser, "AIUS ID 7\r\n");
	assert(event.command.kind == AIUS_USB_COMMAND_ID && event.command.request_id == 7);
	event = feed(&parser, "AIUS AUTH\n");
	assert(event.command.kind == AIUS_USB_COMMAND_AUTH && event.command.request_id == 0);
	event = feed(&parser, "AIUS BOOTLOADER 4294967295\r");
	assert(event.command.kind == AIUS_USB_COMMAND_BOOTLOADER && event.command.request_id == UINT32_MAX);
}

static void test_errors(void)
{
	AiusUsbParser parser;
	AiusUsbEvent event;
	char long_line[AIUS_USB_CONTROL_MAX_LINE + 3u];
	aius_usb_control_init(&parser);
	event = feed(&parser, "\n");
	assert(event.error == AIUS_USB_ERROR_BAD_COMMAND);
	event = feed(&parser, "AIUS NOPE 1\n");
	assert(event.error == AIUS_USB_ERROR_BAD_COMMAND);
	event = feed(&parser, "AIUS PROTOCOL 2\n");
	assert(event.error == AIUS_USB_ERROR_BAD_VERSION);
	event = feed(&parser, "AIUS ID 0\n");
	assert(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	event = feed(&parser, "AIUS ID 1 extra\n");
	assert(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	memset(long_line, 'x', sizeof(long_line) - 2u);
	long_line[sizeof(long_line) - 2u] = '\n';
	long_line[sizeof(long_line) - 1u] = '\0';
	event = feed(&parser, long_line);
	assert(event.error == AIUS_USB_ERROR_LINE_TOO_LONG);
	event = feed(&parser, "AIUS ID 2\n");
	assert(event.command.request_id == 2);
	{
		const uint8_t invalid[] = {'A', 'I', 'U', 'S', ' ', 'I', 'D', ' ', '3', 0x01, '\n'};
		for (size_t i = 0; i < sizeof(invalid); i++)
			if (aius_usb_control_feed(&parser, invalid[i], &event))
				break;
		assert(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	}
	{
		const uint8_t invalid[] = {'A', 'I', 'U', 'S', ' ', 'I', 'D', ' ', '3', 0x80, '\n'};
		for (size_t i = 0; i < sizeof(invalid); i++)
			if (aius_usb_control_feed(&parser, invalid[i], &event))
				break;
		assert(event.error == AIUS_USB_ERROR_BAD_ARGUMENT);
	}
}

static void test_formatters(void)
{
	char output[96];
	size_t length = aius_usb_control_write_ok(output, sizeof(output), 7, "ID", "x");
	assert(length != 0 && strcmp(output, "AIUS/1 OK 7 ID x\n") == 0);
	length = aius_usb_control_write_error(output, sizeof(output), 7, AIUS_USB_ERROR_BAD_ARGUMENT);
	assert(length != 0 && strstr(output, "AIUS/1 ERR 7 BAD_ARGUMENT") == output);
	assert(aius_usb_control_write_ok(output, 8, 1, "ID", "long") == 0);
}

int main(void)
{
	test_commands();
	test_errors();
	test_formatters();
	puts("ok usb control");
	return 0;
}
