#ifndef AIUS_BOOT_TEENSY_H
#define AIUS_BOOT_TEENSY_H

#include "aius_boot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_BOOT_SLOT_COMMITTED 0x41535542u

typedef struct __attribute__((aligned(32))) {
	uint32_t committed;
	uint32_t sequence;
	AiusBootLedger ledger;
	uint32_t slot_crc32;
	uint8_t padding[24];
} AiusBootSlot;

#ifdef __cplusplus
static_assert(sizeof(AiusBootSlot) == 64, "retained slot size");
static_assert(alignof(AiusBootSlot) == 32, "retained slot alignment");
#else
_Static_assert(sizeof(AiusBootSlot) == 64, "retained slot size");
_Static_assert(_Alignof(AiusBootSlot) == 32, "retained slot alignment");
#endif

typedef struct {
	void *context;
	bool (*read_slot)(void *context, uint8_t slot, AiusBootSlot *out);
	bool (*write_slot_bytes)(void *context,
	                         uint8_t slot,
	                         size_t offset,
	                         const uint8_t *data,
	                         size_t length);
	void (*flush_slot_range)(void *context,
	                         uint8_t slot,
	                         size_t offset,
	                         size_t length);
	void (*barrier)(void *context);
	uint32_t (*read_reset_source)(void *context);
	void (*clear_reset_source)(void *context, uint32_t captured);
	void (*enable_watchdog_clock)(void *context);
	void (*write_watchdog_wmcr)(void *context, uint16_t value);
	void (*write_watchdog_wcr)(void *context, uint16_t value);
	void (*write_watchdog_wsr)(void *context, uint16_t value);
} AiusBootTeensyPrimitives;

typedef struct {
	AiusBootTeensyPrimitives primitives;
	uint8_t active_slot;
	uint32_t sequence;
	bool valid;
} AiusBootStorageTeensy;

extern AiusBootSlot g_aius_boot_slots[2];
extern AiusAbortLatch g_aius_abort_latch;

/* Retained-RAM wrappers over the portable abort latch: dcache delete before
 * the boot-time read, flush + barrier after every write, so the value survives
 * the watchdog reset it exists to explain. */
uint8_t aius_boot_teensy_abort_capture(void);
void aius_boot_teensy_abort_note(uint16_t checkpoint);

void aius_boot_storage_teensy_init(
	AiusBootStorageTeensy *storage,
	const AiusBootTeensyPrimitives *primitives);

bool aius_boot_storage_teensy_load(
	AiusBootStorageTeensy *storage,
	AiusBootLedger *ledger);

bool aius_boot_storage_teensy_commit(
	AiusBootStorageTeensy *storage,
	const AiusBootLedger *ledger);

uint32_t aius_boot_teensy_reset_source(
	const AiusBootTeensyPrimitives *primitives);

AiusResetKind aius_boot_teensy_reset_kind(uint32_t raw_reset_source);

bool aius_boot_teensy_watchdog_arm(
	const AiusBootTeensyPrimitives *primitives);

bool aius_boot_teensy_watchdog_feed(
	const AiusBootTeensyPrimitives *primitives);

AiusBootTeensyPrimitives aius_boot_teensy_hardware_primitives(void);

#ifdef __cplusplus
}
#endif

#endif
