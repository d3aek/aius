#ifndef AIUS_USB_CONTROL_H
#define AIUS_USB_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_USB_CONTROL_MAX_LINE 127u
#define AIUS_USB_CONTROL_LINE_STORAGE 128u

typedef enum { AIUS_USB_EVENT_NONE = 0, AIUS_USB_EVENT_COMMAND, AIUS_USB_EVENT_ERROR } AiusUsbEventType;
typedef enum { AIUS_USB_COMMAND_NONE = 0, AIUS_USB_COMMAND_PROTOCOL, AIUS_USB_COMMAND_ID, AIUS_USB_COMMAND_AUTH, AIUS_USB_COMMAND_BOOTLOADER } AiusUsbCommandKind;
typedef enum { AIUS_USB_ERROR_NONE = 0, AIUS_USB_ERROR_BAD_VERSION, AIUS_USB_ERROR_BAD_COMMAND, AIUS_USB_ERROR_BAD_ARGUMENT, AIUS_USB_ERROR_LINE_TOO_LONG, AIUS_USB_ERROR_BUSY, AIUS_USB_ERROR_INTERNAL } AiusUsbError;

typedef struct { AiusUsbCommandKind kind; uint32_t request_id; } AiusUsbCommand;
typedef struct { AiusUsbEventType type; AiusUsbCommand command; AiusUsbError error; } AiusUsbEvent;
typedef struct { char line[AIUS_USB_CONTROL_LINE_STORAGE]; uint8_t length; uint8_t discarding; uint8_t invalid; uint8_t suppress_lf; } AiusUsbParser;

void aius_usb_control_init(AiusUsbParser *parser);
int aius_usb_control_feed(AiusUsbParser *parser, uint8_t byte, AiusUsbEvent *event);
size_t aius_usb_control_write_ok(char *output, size_t capacity, uint32_t request_id, const char *verb, const char *payload);
size_t aius_usb_control_write_error(char *output, size_t capacity, uint32_t request_id, AiusUsbError error);

#ifdef __cplusplus
}
#endif

#endif
