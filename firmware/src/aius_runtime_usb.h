/* Bounded USB (CDC) runtime service.
 *
 * One concern: staging bounded CDC input and driving the portable
 * aius_usb_control runtime transaction against the normalized CDC adapter.
 * Construction touches memory only.
 *
 * USB_RX is the explicit exception to the one-adapter-event-per-service rule.
 * While connected it performs at most the lesser of the budget's call/byte quota
 * and the free space in the retained 64-byte staging buffer, one byte per
 * primitive call, and does not call the portable step at all. A terminal or
 * malformed primitive result stops the drain, marks the connection unavailable
 * for the epoch and clears staged input.
 *
 * USB_TX calls aius_usb_runtime_step() exactly once and handles at most one
 * returned action: one synchronous application dispatch, one nonblocking write,
 * or one bootloader entry.
 */

#ifndef AIUS_RUNTIME_USB_H
#define AIUS_RUNTIME_USB_H

#include "aius_cdc_teensy.h"
#include "aius_io.h"
#include "aius_optical_app.h"
#include "aius_runtime.h"
#include "aius_usb_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest canonical verb the control protocol accepts ("BOOTLOADER"), plus a
 * terminator. */
#define AIUS_RUNTIME_USB_VERB_STORAGE 11u

#ifdef __cplusplus
/* The staging buffer is exactly the portable RX quota, and the payload copy is
 * exactly what the dispatcher can produce. Enforced, not commented. */
static_assert(AIUS_RUNTIME_USB_VERB_STORAGE >= sizeof("BOOTLOADER"),
              "USB verb storage must hold the longest canonical verb");
static_assert(AIUS_OPTICAL_APP_USB_PAYLOAD_STORAGE ==
	              AIUS_OPTICAL_APP_USB_PAYLOAD_MAX + 1u,
              "USB payload copy must hold the dispatcher's maximum payload");
#endif

typedef struct {
	AiusUsbRuntime runtime;
	AiusCdcTeensy adapter;
	char usb_response[AIUS_USB_RUNTIME_MAX_RESPONSE];
	/* Service-owned copies of the borrowed application verb and payload, so
	 * the retained response stays valid independently of the dispatcher's
	 * next dispatch. Distinct members from `runtime` and `usb_response`, so
	 * the portable step's span-disjointness precondition holds by layout. */
	char usb_verb[AIUS_RUNTIME_USB_VERB_STORAGE];
	char usb_payload[AIUS_OPTICAL_APP_USB_PAYLOAD_STORAGE];
	AiusCdcTeensyOps ops;
	uint8_t usb_rx[AIUS_USB_RUNTIME_RX_QUOTA];
	size_t usb_rx_length;
	AiusIoEvent usb_write_event;
	AiusUsbApplicationResponse application_response;
	bool write_event_present;
	bool application_response_present;
	/* USB_RX saw CLOSED, FAULT or a malformed primitive result this epoch,
	 * so USB_TX treats the connection as unavailable regardless of a later
	 * connected() sample. */
	bool rx_terminal;
	bool bootloader_entered;
} AiusRuntimeUsb;

/* Memory only. Constructs the CDC adapter from `primitives`, caches its
 * normalized ops table and initializes the portable runtime against the
 * embedded response buffer. No primitive is called. */
void aius_runtime_usb_construct(
	AiusRuntimeUsb *service,
	const AiusCdcTeensyPrimitives *primitives);

/* One USB_RX checkpoint: bounded one-byte reads into the staging buffer.
 * Returns false only for an invalid call (null service or budget, or a budget
 * that is not the USB_RX descriptor). */
bool aius_runtime_usb_rx_service(
	AiusRuntimeUsb *service,
	const AiusRuntimeBudget *budget);

/* One USB_TX checkpoint: exactly one portable step plus at most one resulting
 * action. Returns false only for an invalid call (null service or budget, or a
 * budget that is not the USB_TX descriptor). */
bool aius_runtime_usb_tx_service(
	AiusRuntimeUsb *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms,
	AiusOpticalAppDispatcher *app);

#ifdef __cplusplus
}
#endif

#endif
