/* Teensy CDC primitive normalization -- see aius_cdc_teensy.h.
 *
 * Every definition in this file is FLASHMEM. The attribute must sit on the
 * definition: GCC does not inherit a `section` attribute from a forward
 * declaration, so a prototype-only FLASHMEM silently leaves the code in ITCM,
 * and the teensy41-optical image has ~1.4 kB of ITCM left. Nothing here runs
 * per audio block -- the whole file runs at the USB_RX/USB_TX checkpoints.
 *
 * ops_enter_bootloader() is the one op that blocks, for a bounded interval, on
 * a terminal path; see aius_cdc_teensy.h §Bootloader drain.
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

/* The raw free space, folded into the idle high-water mark on the way past.
 * False when the adapter cannot sample at all. */
FLASHMEM bool sample_tx_free(AiusCdcTeensy *adapter, uint32_t *free_bytes)
{
	if (adapter == nullptr ||
	    adapter->primitives.available_for_write == nullptr)
		return false;
	const int available = adapter->primitives.available_for_write(
		adapter->primitives.context);
	*free_bytes = available <= 0 ? 0u : static_cast<uint32_t>(available);
	if (*free_bytes > adapter->tx_free_high_water)
		adapter->tx_free_high_water = *free_bytes;
	return true;
}

FLASHMEM size_t ops_write_capacity(void *context)
{
	AiusCdcTeensy *adapter = static_cast<AiusCdcTeensy *>(context);
	uint32_t free_bytes = 0u;
	if (!sample_tx_free(adapter, &free_bytes) || free_bytes == 0u)
		return 0u;
	if (free_bytes > AIUS_CDC_TEENSY_MAX_WRITE)
		return AIUS_CDC_TEENSY_MAX_WRITE;
	return static_cast<size_t>(free_bytes);
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

/* True once the transmit ring is back at its idle free space, i.e. the host has
 * retired every queued transfer. An adapter that cannot sample reports drained,
 * so a missing primitive degrades to the old immediate hand-over rather than to
 * a stall. */
FLASHMEM bool tx_drained(AiusCdcTeensy *adapter)
{
	uint32_t free_bytes = 0u;
	if (!sample_tx_free(adapter, &free_bytes))
		return true;
	return free_bytes >= adapter->tx_free_high_water;
}

/* Push the buffered response out and wait for the host to take it -- see
 * aius_cdc_teensy.h §Bootloader drain for why this blocks and what bounds it.
 * Returns after at most AIUS_CDC_TEENSY_DRAIN_BOUND_MS whatever the host does,
 * and immediately when the adapter has no clock to bound a wait with. */
FLASHMEM void drain_before_bootloader(AiusCdcTeensy *adapter)
{
	uint32_t start;
	uint32_t elapsed = 0u;
	uint32_t free_bytes = 0u;

	adapter->bootloader_drain_ms = 0u;
	adapter->bootloader_drained = false;

	/* Sampled before the flush so the ring's idle capacity is known even if
	 * this epoch is the first to reach the adapter: the response still sits
	 * in the head buffer, which available_for_write() does not count. */
	(void)sample_tx_free(adapter, &free_bytes);

	if (adapter->primitives.flush != nullptr)
		adapter->primitives.flush(adapter->primitives.context);

	if (adapter->primitives.millis == nullptr) {
		adapter->bootloader_drained = tx_drained(adapter);
		return;
	}

	start = adapter->primitives.millis(adapter->primitives.context);
	for (;;) {
		elapsed = adapter->primitives.millis(
			          adapter->primitives.context) -
		          start;
		if (tx_drained(adapter)) {
			adapter->bootloader_drained = true;
			break;
		}
		if (elapsed >= AIUS_CDC_TEENSY_DRAIN_BOUND_MS) {
			adapter->bootloader_drained = false;
			break;
		}
	}
	adapter->bootloader_drain_ms = elapsed;
}

FLASHMEM void ops_enter_bootloader(void *context)
{
	AiusCdcTeensy *adapter = static_cast<AiusCdcTeensy *>(context);
	if (adapter == nullptr ||
	    adapter->primitives.enter_bootloader == nullptr)
		return;
	drain_before_bootloader(adapter);
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

/* usb_serial_flush_output(): submits the partially filled head buffer to the
 * CDC IN endpoint instead of leaving it for the 75 us auto-flush timer. It does
 * not wait for the host -- drain_before_bootloader() does that. */
FLASHMEM void serial_flush(void *context)
{
	(void)context;
	Serial.flush();
}

FLASHMEM uint32_t serial_millis(void *context)
{
	(void)context;
	return millis();
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
	       primitives->flush != nullptr &&
	       primitives->millis != nullptr &&
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
	primitives.flush = serial_flush;
	primitives.millis = serial_millis;
	primitives.enter_bootloader = serial_enter_bootloader;
	return primitives;
}

} // extern "C"
