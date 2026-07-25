/* Teensy CDC primitive normalization -- see aius_cdc_teensy.h.
 *
 * Every definition in this file is FLASHMEM. The attribute must sit on the
 * definition: GCC does not inherit a `section` attribute from a forward
 * declaration, so a prototype-only FLASHMEM silently leaves the code in ITCM,
 * and the teensy41-optical image has ~1.4 kB of ITCM left. Nothing here runs
 * per audio block -- the whole file runs at the USB_RX/USB_TX checkpoints.
 */

#include "aius_cdc_teensy.h"

#include <Arduino.h>

#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

FLASHMEM AiusIoEvent io_event(AiusIoResult result, size_t bytes, uint32_t fault)
{
	AiusIoEvent event;
	event.result = result;
	event.bytes = bytes;
	event.fault = fault;
	return event;
}

/* One diagnostic update per raw-protocol violation, saturating. The returned
 * FAULT carries the same sentinel so the caller never has to re-count it. */
FLASHMEM AiusIoEvent protocol_fault(AiusCdcTeensy *adapter)
{
	if (adapter != nullptr) {
		if (adapter->protocol_faults != UINT32_MAX)
			adapter->protocol_faults++;
		adapter->last_fault = AIUS_CDC_ADAPTER_PROTOCOL_FAULT;
	}
	return io_event(AIUS_IO_FAULT, 0u, AIUS_CDC_ADAPTER_PROTOCOL_FAULT);
}

FLASHMEM bool ops_connected(void *context)
{
	AiusCdcTeensy *adapter = static_cast<AiusCdcTeensy *>(context);
	if (adapter == nullptr || adapter->primitives.connected == nullptr)
		return false;
	return adapter->primitives.connected(adapter->primitives.context);
}

FLASHMEM size_t ops_write_capacity(void *context)
{
	AiusCdcTeensy *adapter = static_cast<AiusCdcTeensy *>(context);
	if (adapter == nullptr ||
	    adapter->primitives.available_for_write == nullptr)
		return 0u;
	const int available = adapter->primitives.available_for_write(
		adapter->primitives.context);
	if (available <= 0)
		return 0u;
	if (static_cast<unsigned>(available) > AIUS_CDC_TEENSY_MAX_WRITE)
		return AIUS_CDC_TEENSY_MAX_WRITE;
	return static_cast<size_t>(available);
}

FLASHMEM AiusIoEvent ops_read_byte(void *context, uint8_t *byte)
{
	AiusCdcTeensy *adapter = static_cast<AiusCdcTeensy *>(context);
	if (adapter == nullptr || byte == nullptr ||
	    adapter->primitives.read == nullptr)
		return protocol_fault(adapter);
	if (!ops_connected(context))
		return io_event(AIUS_IO_CLOSED, 0u, 0u);
	const int value = adapter->primitives.read(adapter->primitives.context);
	if (value == -1)
		return io_event(AIUS_IO_WOULD_BLOCK, 0u, 0u);
	if (value < 0 || value > 255)
		return protocol_fault(adapter);
	*byte = static_cast<uint8_t>(value);
	return io_event(AIUS_IO_PROGRESS, 1u, 0u);
}

FLASHMEM AiusIoEvent ops_write(void *context, const uint8_t *data, size_t length)
{
	AiusCdcTeensy *adapter = static_cast<AiusCdcTeensy *>(context);
	if (adapter == nullptr || adapter->primitives.write == nullptr)
		return protocol_fault(adapter);
	if (data == nullptr || length == 0u ||
	    length > AIUS_CDC_TEENSY_MAX_WRITE)
		return protocol_fault(adapter);
	if (!ops_connected(context))
		return io_event(AIUS_IO_CLOSED, 0u, 0u);
	const size_t accepted =
		adapter->primitives.write(adapter->primitives.context, data,
		                          length);
	if (accepted == 0u)
		return io_event(AIUS_IO_WOULD_BLOCK, 0u, 0u);
	if (accepted > length)
		return protocol_fault(adapter);
	return io_event(AIUS_IO_PROGRESS, accepted, 0u);
}

FLASHMEM void ops_enter_bootloader(void *context)
{
	AiusCdcTeensy *adapter = static_cast<AiusCdcTeensy *>(context);
	if (adapter == nullptr ||
	    adapter->primitives.enter_bootloader == nullptr)
		return;
	adapter->primitives.enter_bootloader(adapter->primitives.context);
}

FLASHMEM bool serial_connected(void *context)
{
	(void)context;
	return static_cast<bool>(Serial);
}

FLASHMEM int serial_read(void *context)
{
	(void)context;
	return Serial.read();
}

FLASHMEM int serial_available_for_write(void *context)
{
	(void)context;
	return Serial.availableForWrite();
}

FLASHMEM size_t serial_write(void *context, const uint8_t *data, size_t length)
{
	(void)context;
	return Serial.write(data, length);
}

FLASHMEM void serial_enter_bootloader(void *context)
{
	(void)context;
	_reboot_Teensyduino_();
}

} // namespace

extern "C" {

FLASHMEM bool aius_cdc_teensy_primitives_complete(
	const AiusCdcTeensyPrimitives *primitives)
{
	return primitives != nullptr && primitives->connected != nullptr &&
	       primitives->read != nullptr &&
	       primitives->available_for_write != nullptr &&
	       primitives->write != nullptr &&
	       primitives->enter_bootloader != nullptr;
}

FLASHMEM void aius_cdc_teensy_construct(
	AiusCdcTeensy *adapter,
	const AiusCdcTeensyPrimitives *primitives)
{
	if (adapter == nullptr)
		return;
	memset(adapter, 0, sizeof(*adapter));
	if (primitives != nullptr)
		adapter->primitives = *primitives;
}

FLASHMEM AiusCdcTeensyOps aius_cdc_teensy_ops(AiusCdcTeensy *adapter)
{
	AiusCdcTeensyOps ops;
	memset(&ops, 0, sizeof(ops));
	if (adapter == nullptr)
		return ops;
	ops.context = adapter;
	ops.connected = ops_connected;
	ops.write_capacity = ops_write_capacity;
	ops.read_byte = ops_read_byte;
	ops.write = ops_write;
	ops.enter_bootloader = ops_enter_bootloader;
	return ops;
}

FLASHMEM AiusCdcTeensyPrimitives aius_cdc_teensy_serial_primitives(void)
{
	AiusCdcTeensyPrimitives primitives;
	memset(&primitives, 0, sizeof(primitives));
	primitives.context = nullptr;
	primitives.connected = serial_connected;
	primitives.read = serial_read;
	primitives.available_for_write = serial_available_for_write;
	primitives.write = serial_write;
	primitives.enter_bootloader = serial_enter_bootloader;
	return primitives;
}

} // extern "C"
