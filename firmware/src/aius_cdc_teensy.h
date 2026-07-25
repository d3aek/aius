/* Teensy CDC primitive normalization.
 *
 * The only production `Serial` mapping in the new target runtime. The adapter
 * turns the pinned Teensyduino primitives (an int-returning `read()`, an
 * int-returning `availableForWrite()`, a size_t-returning `write()`) into the
 * portable AiusIoEvent vocabulary the bounded CDC runtime consumes, and records
 * one diagnostic when a primitive returns a value outside its documented range.
 *
 * Two tables, deliberately: AiusCdcTeensyPrimitives is the raw driver seam that
 * tests replace, AiusCdcTeensyOps is the normalized seam the USB service calls.
 * Only aius_cdc_teensy_serial_primitives() mentions `Serial`.
 *
 * Bootloader entry is the one op that blocks -- see §Bootloader drain below.
 */

#ifndef AIUS_CDC_TEENSY_H
#define AIUS_CDC_TEENSY_H

#include "aius_io.h"
#include "aius_usb_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest write the bounded CDC runtime ever offers, and therefore the clamp
 * aius_cdc_teensy_ops()->write_capacity() applies. */
#define AIUS_CDC_TEENSY_MAX_WRITE 64u

/* Recorded in AiusCdcTeensy::last_fault and returned as the AiusIoEvent fault
 * whenever a primitive returns a value its API cannot legally produce (a read
 * sentinel below -1, a write result above the offered length, a missing
 * primitive). ASCII "USBP". */
#define AIUS_CDC_ADAPTER_PROTOCOL_FAULT UINT32_C(0x55534250)

/* The longest the bootloader drain blocks before it hands over regardless. The
 * portable runtime's pre-write bound and this post-write bound are the same
 * budget, so a wedged host costs at most AIUS_USB_BOOTLOADER_DELAY_MS on either
 * side of the final write. */
#define AIUS_CDC_TEENSY_DRAIN_BOUND_MS AIUS_USB_BOOTLOADER_DELAY_MS

/* Normalized seam. `context` is the AiusCdcTeensy the ops table was built from.
 * read_byte() stores the received byte only for PROGRESS(bytes=1).
 *
 * §Bootloader drain. enter_bootloader() is the sole blocking op, and it blocks
 * deliberately: Teensyduino's `Serial.write()` reports bytes *buffered*, not
 * bytes *transmitted*, and `_reboot_Teensyduino_()` halts the core or clears
 * USB1_USBCMD, so entering the loader straight off a full-length write races
 * the in-flight transfer and usually loses. The op therefore pushes the partial
 * head buffer out with flush(), then polls available_for_write() until the
 * transmit ring is back at its idle free space -- a real host-side retirement
 * signal, not a sleep -- and only then calls enter_bootloader(). The poll is
 * bounded by AIUS_CDC_TEENSY_DRAIN_BOUND_MS; on expiry the op enters the loader
 * anyway, so a host that never drains costs a bounded stall and never wedges
 * the board. This is a terminal action, so a bounded block in this one path is
 * acceptable where it would not be at any other checkpoint; the 8-second
 * watchdog window comfortably covers it. */
typedef struct {
	void *context;
	bool (*connected)(void *context);
	size_t (*write_capacity)(void *context);
	AiusIoEvent (*read_byte)(void *context, uint8_t *byte);
	AiusIoEvent (*write)(
		void *context,
		const uint8_t *data,
		size_t length);
	void (*enter_bootloader)(void *context);
} AiusCdcTeensyOps;

/* Raw driver seam. `read` returns -1 when empty and 0..255 otherwise;
 * `available_for_write` may return a negative value; `write` returns the
 * accepted prefix length; `flush` submits the buffered partial packet;
 * `millis` is the free-running millisecond clock the drain bound is measured
 * against. */
typedef struct {
	void *context;
	bool (*connected)(void *context);
	int (*read)(void *context);
	int (*available_for_write)(void *context);
	size_t (*write)(void *context, const uint8_t *data, size_t length);
	void (*flush)(void *context);
	uint32_t (*millis)(void *context);
	void (*enter_bootloader)(void *context);
} AiusCdcTeensyPrimitives;

typedef struct {
	AiusCdcTeensyPrimitives primitives;
	uint32_t protocol_faults;
	uint32_t last_fault;
	/* Largest free space available_for_write() has ever reported, which is
	 * the transmit ring's idle capacity: the driver excludes the buffer it
	 * is currently filling, so this saturates on the first idle sample and
	 * "free space back at the high-water mark" means every queued transfer
	 * has been retired by the host. */
	uint32_t tx_free_high_water;
	/* Last bootloader drain: how long it polled, and whether it observed a
	 * genuine drain or gave up at AIUS_CDC_TEENSY_DRAIN_BOUND_MS. */
	uint32_t bootloader_drain_ms;
	bool bootloader_drained;
} AiusCdcTeensy;

/* True when every primitive the ops table forwards to is present. `context` may
 * legally be null. Composition uses this to reject an incomplete table before
 * an epoch can call through it. */
bool aius_cdc_teensy_primitives_complete(
	const AiusCdcTeensyPrimitives *primitives);

/* Copies `primitives` (a null table zeroes the adapter) and clears the
 * diagnostics. Memory only: no primitive is called. */
void aius_cdc_teensy_construct(
	AiusCdcTeensy *adapter,
	const AiusCdcTeensyPrimitives *primitives);

/* The normalized table bound to `adapter`. A null adapter yields an all-null
 * table. */
AiusCdcTeensyOps aius_cdc_teensy_ops(AiusCdcTeensy *adapter);

/* The production `Serial` table. The sole function in the new target runtime
 * that names `Serial`. */
AiusCdcTeensyPrimitives aius_cdc_teensy_serial_primitives(void);

#ifdef __cplusplus
}
#endif

#endif
