/* Bounded USB (CDC) runtime service -- see aius_runtime_usb.h.
 *
 * Every definition is FLASHMEM, on the definition rather than a prototype (GCC
 * does not inherit a `section` attribute from a forward declaration). Nothing
 * here runs per audio block; it runs at the USB_RX and USB_TX checkpoints.
 */

#include "aius_runtime_usb.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

FLASHMEM bool io_shape(const AiusIoEvent *event, AiusIoResult result,
                       size_t bytes, bool fault)
{
	return event->result == result && event->bytes == bytes &&
	       (fault ? event->fault != 0u : event->fault == 0u);
}

/* A terminal or malformed primitive result: stop, mark the connection
 * unavailable for the epoch, and discard the partial line. */
FLASHMEM void rx_stop(AiusRuntimeUsb *service, uint32_t fault)
{
	service->rx_terminal = true;
	service->usb_rx_length = 0u;
	service->adapter.last_fault = fault;
}

FLASHMEM void copy_application_response(
	AiusRuntimeUsb *service, const AiusUsbApplicationResponse *response)
{
	AiusUsbApplicationResponse copy;

	memset(&copy, 0, sizeof(copy));
	memset(service->usb_verb, 0, sizeof(service->usb_verb));
	memset(service->usb_payload, 0, sizeof(service->usb_payload));

	if (response->error != AIUS_USB_ERROR_NONE) {
		copy.error = response->error;
		service->application_response = copy;
		service->application_response_present = true;
		return;
	}

	/* Defensive bound: the delivered dispatcher never exceeds a 10-byte verb
	 * or a 256-byte payload, so this substitution is unreachable through it.
	 * Substituting the application's own INTERNAL error keeps the emitted
	 * bytes identical to the runtime's invalid-field response rather than
	 * copying out of bounds. */
	if (response->verb == nullptr ||
	    response->verb_length == 0u ||
	    response->verb_length >= sizeof(service->usb_verb) ||
	    response->payload_length >= sizeof(service->usb_payload) ||
	    (response->payload == nullptr) != (response->payload_length == 0u)) {
		copy.error = AIUS_USB_ERROR_INTERNAL;
		service->application_response = copy;
		service->application_response_present = true;
		return;
	}

	memcpy(service->usb_verb, response->verb, response->verb_length);
	copy.verb = service->usb_verb;
	copy.verb_length = response->verb_length;
	if (response->payload_length != 0u) {
		memcpy(service->usb_payload, response->payload,
		       response->payload_length);
		copy.payload = service->usb_payload;
		copy.payload_length = response->payload_length;
	}
	service->application_response = copy;
	service->application_response_present = true;
}

} // namespace

extern "C" {

FLASHMEM void aius_runtime_usb_construct(
	AiusRuntimeUsb *service,
	const AiusCdcTeensyPrimitives *primitives)
{
	if (service == nullptr)
		return;
	memset(service, 0, sizeof(*service));
	aius_cdc_teensy_construct(&service->adapter, primitives);
	service->ops = aius_cdc_teensy_ops(&service->adapter);
	aius_usb_runtime_init(&service->runtime, service->usb_response,
	                      sizeof(service->usb_response));
}

FLASHMEM bool aius_runtime_usb_rx_service(
	AiusRuntimeUsb *service,
	const AiusRuntimeBudget *budget)
{
	size_t quota;
	size_t space;
	size_t attempt;

	if (service == nullptr || budget == nullptr)
		return false;
	if (budget->checkpoint != AIUS_BOOT_CHECKPOINT_USB_RX)
		return false;

	service->rx_terminal = false;
	if (service->ops.connected == nullptr ||
	    service->ops.read_byte == nullptr)
		return false;
	if (!service->ops.connected(service->ops.context)) {
		service->usb_rx_length = 0u;
		return true;
	}

	quota = budget->max_calls;
	if (budget->max_rx_bytes < quota)
		quota = budget->max_rx_bytes;
	space = sizeof(service->usb_rx) - service->usb_rx_length;
	if (space < quota)
		quota = space;

	for (attempt = 0; attempt < quota; ++attempt) {
		uint8_t byte = 0u;
		const AiusIoEvent event =
			service->ops.read_byte(service->ops.context, &byte);

		if (io_shape(&event, AIUS_IO_PROGRESS, 1u, false)) {
			service->usb_rx[service->usb_rx_length++] = byte;
			continue;
		}
		if (io_shape(&event, AIUS_IO_WOULD_BLOCK, 0u, false))
			break;
		if (io_shape(&event, AIUS_IO_CLOSED, 0u, false)) {
			rx_stop(service, 0u);
			break;
		}
		if (io_shape(&event, AIUS_IO_FAULT, 0u, true)) {
			/* Already normalized and, for the adapter's own
			 * sentinel, already counted inside the adapter. */
			rx_stop(service, event.fault);
			break;
		}
		rx_stop(service, AIUS_CDC_ADAPTER_PROTOCOL_FAULT);
		if (service->adapter.protocol_faults != UINT32_MAX)
			service->adapter.protocol_faults++;
		break;
	}
	return true;
}

FLASHMEM bool aius_runtime_usb_tx_service(
	AiusRuntimeUsb *service,
	const AiusRuntimeBudget *budget,
	uint32_t now_ms,
	AiusOpticalAppDispatcher *app)
{
	AiusUsbRuntimeInput input;
	AiusUsbRuntimeAction action;
	bool connected;
	bool had_response;
	bool had_write;
	size_t capacity = 0u;

	if (service == nullptr || budget == nullptr)
		return false;
	if (budget->checkpoint != AIUS_BOOT_CHECKPOINT_USB_TX)
		return false;
	if (service->ops.connected == nullptr ||
	    service->ops.write_capacity == nullptr ||
	    service->ops.write == nullptr ||
	    service->ops.enter_bootloader == nullptr)
		return false;

	/* Move and clear the retained presence flags before the step, so only
	 * newly produced results can set them again. */
	had_response = service->application_response_present;
	had_write = service->write_event_present;
	service->application_response_present = false;
	service->write_event_present = false;

	connected = service->ops.connected(service->ops.context) &&
	            !service->rx_terminal;
	if (connected) {
		capacity = service->ops.write_capacity(service->ops.context);
		if (capacity > budget->max_tx_bytes)
			capacity = budget->max_tx_bytes;
	}

	memset(&input, 0, sizeof(input));
	input.rx = service->usb_rx_length != 0u ? service->usb_rx : nullptr;
	input.rx_length = service->usb_rx_length;
	input.tx_capacity = capacity;
	input.connected = connected;
	input.tx_event_present = had_write;
	if (had_write)
		input.tx_event = service->usb_write_event;
	input.application_response =
		had_response ? &service->application_response : nullptr;

	memset(&action, 0, sizeof(action));
	aius_usb_runtime_step(&service->runtime, now_ms, input, &action);

	if (action.rx_consumed != 0u) {
		const size_t consumed = action.rx_consumed <
			                        service->usb_rx_length ?
			                        action.rx_consumed :
			                        service->usb_rx_length;
		const size_t remaining = service->usb_rx_length - consumed;

		if (remaining != 0u)
			memmove(service->usb_rx, service->usb_rx + consumed,
			        remaining);
		service->usb_rx_length = remaining;
	}

	switch (action.kind) {
	case AIUS_USB_ACTION_DISPATCH: {
		const AiusUsbApplicationResponse *response =
			aius_optical_app_usb_dispatch(app, action.request);
		/* A null return means an invalid dispatcher or request. Retaining
		 * no response makes the next step's missing response the
		 * runtime's own deterministic INTERNAL error. */
		if (response != nullptr)
			copy_application_response(service, response);
		break;
	}
	case AIUS_USB_ACTION_WRITE:
		service->usb_write_event = service->ops.write(
			service->ops.context, action.tx, action.tx_length);
		service->write_event_present = true;
		break;
	case AIUS_USB_ACTION_ENTER_BOOTLOADER:
		service->bootloader_entered = true;
		service->ops.enter_bootloader(service->ops.context);
		break;
	case AIUS_USB_ACTION_NONE:
	default:
		break;
	}

	/* Loss between the two checkpoints still discards the partial line. */
	if (!connected)
		service->usb_rx_length = 0u;
	return true;
}

} // extern "C"
