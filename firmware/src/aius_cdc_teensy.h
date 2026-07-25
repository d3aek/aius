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
 */

#ifndef AIUS_CDC_TEENSY_H
#define AIUS_CDC_TEENSY_H

#include "aius_io.h"

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

/* Normalized seam. `context` is the AiusCdcTeensy the ops table was built from.
 * read_byte() stores the received byte only for PROGRESS(bytes=1). */
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
 * accepted prefix length. */
typedef struct {
	void *context;
	bool (*connected)(void *context);
	int (*read)(void *context);
	int (*available_for_write)(void *context);
	size_t (*write)(void *context, const uint8_t *data, size_t length);
	void (*enter_bootloader)(void *context);
} AiusCdcTeensyPrimitives;

typedef struct {
	AiusCdcTeensyPrimitives primitives;
	uint32_t protocol_faults;
	uint32_t last_fault;
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
