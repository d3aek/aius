#ifndef AIUS_USB_CONTROL_H
#define AIUS_USB_CONTROL_H

#include "aius_io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_USB_CONTROL_MAX_LINE 127u
#define AIUS_USB_CONTROL_LINE_STORAGE 128u
#define AIUS_USB_RUNTIME_MIN_RESPONSE 48u
#define AIUS_USB_RUNTIME_MAX_RESPONSE 320u
#define AIUS_USB_RUNTIME_RX_QUOTA 64u
#define AIUS_USB_RUNTIME_TX_QUOTA 64u
/* How long an authorized BOOTLOADER transaction keeps trying to place its
 * response on the wire before it enters the loader regardless. The runtime
 * spends it waiting for the write to complete; the CDC adapter spends the same
 * budget waiting for the host to retire the transfer. */
#define AIUS_USB_BOOTLOADER_DELAY_MS 500u

typedef enum { AIUS_USB_EVENT_NONE = 0, AIUS_USB_EVENT_COMMAND, AIUS_USB_EVENT_ERROR } AiusUsbEventType;
typedef enum { AIUS_USB_COMMAND_NONE = 0, AIUS_USB_COMMAND_PROTOCOL, AIUS_USB_COMMAND_ID, AIUS_USB_COMMAND_AUTH, AIUS_USB_COMMAND_BOOTLOADER } AiusUsbCommandKind;
typedef enum { AIUS_USB_ERROR_NONE = 0, AIUS_USB_ERROR_BAD_VERSION, AIUS_USB_ERROR_BAD_COMMAND, AIUS_USB_ERROR_BAD_ARGUMENT, AIUS_USB_ERROR_LINE_TOO_LONG, AIUS_USB_ERROR_BUSY, AIUS_USB_ERROR_INTERNAL } AiusUsbError;

typedef struct { AiusUsbCommandKind kind; uint32_t request_id; } AiusUsbCommand;
typedef struct { AiusUsbEventType type; AiusUsbCommand command; AiusUsbError error; } AiusUsbEvent;
typedef struct { char line[AIUS_USB_CONTROL_LINE_STORAGE]; uint8_t length; uint8_t discarding; uint8_t invalid; uint8_t suppress_lf; } AiusUsbParser;

typedef enum {
	AIUS_USB_RUNTIME_IDLE,
	AIUS_USB_RUNTIME_DISPATCH,
	AIUS_USB_RUNTIME_TX,
	AIUS_USB_RUNTIME_ACK_PENDING,
	AIUS_USB_RUNTIME_ENTER_BOOTLOADER
} AiusUsbRuntimeState;

typedef struct {
	AiusUsbCommand command;
} AiusUsbApplicationRequest;

typedef struct {
	AiusUsbError error;
	const char *verb;
	size_t verb_length;
	const char *payload;
	size_t payload_length;
} AiusUsbApplicationResponse;

typedef struct {
	const uint8_t *rx;
	size_t rx_length;
	size_t tx_capacity;
	bool connected;
	bool tx_event_present;
	AiusIoEvent tx_event;
	const AiusUsbApplicationResponse *application_response;
} AiusUsbRuntimeInput;

typedef enum {
	AIUS_USB_ACTION_NONE,
	AIUS_USB_ACTION_DISPATCH,
	AIUS_USB_ACTION_WRITE,
	AIUS_USB_ACTION_ENTER_BOOTLOADER
} AiusUsbRuntimeActionKind;

typedef struct {
	AiusUsbRuntimeActionKind kind;
	const AiusUsbApplicationRequest *request;
	const uint8_t *tx;
	size_t tx_length;
	size_t rx_consumed;
} AiusUsbRuntimeAction;

typedef struct {
	AiusUsbParser parser;
	AiusUsbRuntimeState state;
	char *response;
	size_t response_capacity;
	size_t response_length;
	size_t response_offset;
	uint32_t bootloader_deadline_ms;
	uint32_t protocol_faults;
	/* ENTER_BOOTLOADER actions whose composition returned instead of handing
	 * the board to HalfKay. Each one resets the transaction to IDLE so the
	 * next AIUS BOOTLOADER is served normally. Saturating. */
	uint32_t bootloader_entry_failures;
	size_t outstanding_length;
	AiusUsbApplicationRequest pending_request;
	bool valid;
	bool write_outstanding;
	bool dispatch_emitted;
	bool bootloader_authorized;
	bool enter_emitted;
} AiusUsbRuntime;

void aius_usb_control_init(AiusUsbParser *parser);
int aius_usb_control_feed(AiusUsbParser *parser, uint8_t byte, AiusUsbEvent *event);
size_t aius_usb_control_write_ok(char *output, size_t capacity, uint32_t request_id, const char *verb, const char *payload);
size_t aius_usb_control_write_error(char *output, size_t capacity, uint32_t request_id, AiusUsbError error);
void aius_usb_runtime_init(
	AiusUsbRuntime *runtime,
	char *response,
	size_t response_capacity);
void aius_usb_runtime_step(
	AiusUsbRuntime *runtime,
	uint32_t now_ms,
	AiusUsbRuntimeInput input,
	AiusUsbRuntimeAction *action);

#ifdef __cplusplus
}
#endif

#endif
