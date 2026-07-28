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

/* ------------------------------------------------------------------ */
/* Boot-configuration fuse 0x6E0 -- READ ONLY, NO EXCEPTIONS           */
/* ------------------------------------------------------------------ */

/* The word served when the read did not happen, i.e. every host build.
 *
 * It is NOT zero, and that is the whole reason it exists: zero is the SHIPPED
 * value of both fields on an unblown part, so a zero sentinel would be
 * indistinguishable from the single most likely genuine answer -- and the
 * question this read exists to settle is precisely "is this fuse blown".
 * 0xFFFFFFFF decodes to a secondary-image offset of 255 x 256 KiB = 63.75 MiB
 * on a part with 8 MiB of flash, so it is structurally impossible as a real
 * reading and cannot be mistaken for one even by a client that ignores `read`.
 */
#define AIUS_BOOT_FUSE_UNREAD UINT32_C(0xFFFFFFFF)

typedef struct {
	/* The complete 32-bit shadow word, undecoded, so a field this firmware
	 * does not know about is still recoverable from the wire. */
	uint32_t word;
	/* [23:16] FLASH_SEC_IMAGE_OFFSET. The secondary (slot B) image base is
	 * 256 KiB x this value, so 0 means the secondary image is at the same
	 * address as the primary and hardware A/B selection is unavailable.
	 * RM p.212 Table 9-10. */
	uint8_t sec_image_offset;
	/* [15:12] secondary image size, same table. */
	uint8_t sec_image_size;
	/* False when `word` is AIUS_BOOT_FUSE_UNREAD because no OCOTP exists to
	 * read -- every host build. Never false on the target. */
	bool read;
} AiusBootFuse;

/* Decodes a fuse word. Pure arithmetic, no register access, `read` left false
 * for the caller to set -- which is what makes the decode testable on the host
 * over fabricated words including the ones no board would ever produce. */
AiusBootFuse aius_boot_teensy_decode_boot_fuse(uint32_t word);

/* Reads and decodes the fuse. On the target this performs ONE 32-bit load from
 * the OCOTP shadow register; on a host build it decodes AIUS_BOOT_FUSE_UNREAD
 * and reports read == false.
 *
 * THIS FUNCTION AND ITS CALLERS NEVER WRITE OCOTP. Not the fuse array, not a
 * shadow register, not OCOTP_CTRL, not READ_CTRL, not the write-unlock key.
 * Blowing a fuse is irreversible and per-board, and the one this reads decides
 * where the boot ROM looks for a secondary image -- a stray write would
 * permanently alter the boot behaviour of the part with no way back. The read
 * path is deliberately incapable of it: it touches a single const-qualified
 * load and nothing else, and test/boot-target/test_source_guard.py rejects any
 * assignment to an OCOTP symbol in this adapter. */
AiusBootFuse aius_boot_teensy_boot_fuse(void);

bool aius_boot_teensy_watchdog_arm(
	const AiusBootTeensyPrimitives *primitives);

bool aius_boot_teensy_watchdog_feed(
	const AiusBootTeensyPrimitives *primitives);

AiusBootTeensyPrimitives aius_boot_teensy_hardware_primitives(void);

#ifdef __cplusplus
}
#endif

#endif
