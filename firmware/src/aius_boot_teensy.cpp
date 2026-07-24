#include "aius_boot_teensy.h"

#include <Arduino.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

constexpr uint8_t kNoActiveSlot = UINT8_MAX;
constexpr size_t kSlotCrcBytes = sizeof(uint32_t) + sizeof(AiusBootLedger);
constexpr size_t kCacheLineBytes = 32u;

bool primitives_complete(const AiusBootTeensyPrimitives *primitives)
{
	return primitives != nullptr &&
	       primitives->read_slot != nullptr &&
	       primitives->write_slot_bytes != nullptr &&
	       primitives->flush_slot_range != nullptr &&
	       primitives->barrier != nullptr &&
	       primitives->read_reset_source != nullptr &&
	       primitives->clear_reset_source != nullptr &&
	       primitives->enable_watchdog_clock != nullptr &&
	       primitives->write_watchdog_wmcr != nullptr &&
	       primitives->write_watchdog_wcr != nullptr &&
	       primitives->write_watchdog_wsr != nullptr;
}

void put_u16(uint8_t *destination, uint16_t value)
{
	destination[0] = static_cast<uint8_t>(value);
	destination[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t *destination, uint32_t value)
{
	destination[0] = static_cast<uint8_t>(value);
	destination[1] = static_cast<uint8_t>(value >> 8);
	destination[2] = static_cast<uint8_t>(value >> 16);
	destination[3] = static_cast<uint8_t>(value >> 24);
}

void serialize_ledger(uint8_t *destination, const AiusBootLedger &ledger)
{
	put_u32(&destination[0], ledger.magic);
	put_u16(&destination[4], ledger.version);
	put_u16(&destination[6], ledger.last_checkpoint);
	put_u32(&destination[8], ledger.build_id);
	put_u32(&destination[12], ledger.boot_generation);
	put_u32(&destination[16], ledger.reset_source);
	destination[20] = ledger.early_watchdog_resets;
	destination[21] = ledger.mode;
	put_u16(&destination[22], ledger.reserved);
	put_u32(&destination[24], ledger.crc32);
}

uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
	crc ^= byte;
	for (unsigned int bit = 0; bit < 8; ++bit) {
		const uint32_t mask = 0u - (crc & 1u);
		crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
	}
	return crc;
}

uint32_t slot_crc32(uint32_t sequence, const AiusBootLedger &ledger)
{
	uint8_t bytes[kSlotCrcBytes];
	put_u32(bytes, sequence);
	serialize_ledger(&bytes[sizeof(sequence)], ledger);

	uint32_t crc = UINT32_MAX;
	for (const uint8_t byte : bytes)
		crc = crc32_update(crc, byte);
	return crc ^ UINT32_MAX;
}

bool slot_valid(const AiusBootSlot &slot)
{
	return slot.committed == AIUS_BOOT_SLOT_COMMITTED &&
	       slot.slot_crc32 == slot_crc32(slot.sequence, slot.ledger) &&
	       aius_boot_is_valid(&slot.ledger);
}

uint8_t newest_slot(const AiusBootSlot (&slots)[2],
                    const bool (&valid)[2])
{
	if (valid[0] && !valid[1])
		return 0u;
	if (!valid[0] && valid[1])
		return 1u;

	const uint32_t delta = slots[0].sequence - slots[1].sequence;
	if (delta == 0u || delta == UINT32_C(0x80000000))
		return 0u;
	return delta < UINT32_C(0x80000000) ? 0u : 1u;
}

bool write_bytes(const AiusBootStorageTeensy &storage,
                 uint8_t slot,
                 size_t offset,
                 const uint8_t *bytes,
                 size_t length)
{
	return storage.primitives.write_slot_bytes(
		storage.primitives.context, slot, offset, bytes, length);
}

bool hardware_read_slot(void *, uint8_t slot, AiusBootSlot *out)
{
	if (slot >= 2u || out == nullptr)
		return false;
	arm_dcache_delete(&g_aius_boot_slots[slot], sizeof(AiusBootSlot));
	memcpy(out, &g_aius_boot_slots[slot], sizeof(*out));
	return true;
}

bool hardware_write_slot_bytes(void *,
                               uint8_t slot,
                               size_t offset,
                               const uint8_t *data,
                               size_t length)
{
	if (slot >= 2u || offset > sizeof(AiusBootSlot) ||
	    length > sizeof(AiusBootSlot) - offset ||
	    (length != 0u && data == nullptr))
		return false;

	uint8_t *destination =
		reinterpret_cast<uint8_t *>(&g_aius_boot_slots[slot]);
	memcpy(destination + offset, data, length);
	return true;
}

void hardware_flush_slot_range(void *,
                               uint8_t slot,
                               size_t offset,
                               size_t length)
{
	if (slot >= 2u || offset > sizeof(AiusBootSlot) ||
	    length > sizeof(AiusBootSlot) - offset)
		return;
	uint8_t *address =
		reinterpret_cast<uint8_t *>(&g_aius_boot_slots[slot]) + offset;
	arm_dcache_flush(address, static_cast<uint32_t>(length));
}

void hardware_barrier(void *)
{
#if defined(__IMXRT1062__)
	asm volatile("dsb sy" ::: "memory");
#else
	__DSB();
#endif
}

uint32_t hardware_read_reset_source(void *)
{
	return SRC_SRSR;
}

void hardware_clear_reset_source(void *, uint32_t captured)
{
	SRC_SRSR = captured;
}

void hardware_enable_watchdog_clock(void *)
{
	CCM_CCGR3 |= CCM_CCGR3_WDOG1(3);
}

void hardware_write_watchdog_wmcr(void *, uint16_t value)
{
	WDOG1_WMCR = value;
}

void hardware_write_watchdog_wcr(void *, uint16_t value)
{
	WDOG1_WCR = value;
}

void hardware_write_watchdog_wsr(void *, uint16_t value)
{
	WDOG1_WSR = value;
}

} // namespace

DMAMEM AiusBootSlot g_aius_boot_slots[2] __attribute__((used));

extern "C" void aius_boot_storage_teensy_init(
	AiusBootStorageTeensy *storage,
	const AiusBootTeensyPrimitives *primitives)
{
	if (storage == nullptr)
		return;

	memset(storage, 0, sizeof(*storage));
	storage->active_slot = kNoActiveSlot;
	if (!primitives_complete(primitives))
		return;

	storage->primitives = *primitives;
	storage->valid = true;
}

extern "C" bool aius_boot_storage_teensy_load(
	AiusBootStorageTeensy *storage,
	AiusBootLedger *ledger)
{
	if (ledger != nullptr)
		memset(ledger, 0, sizeof(*ledger));
	if (storage == nullptr || ledger == nullptr || !storage->valid)
		return false;

	AiusBootSlot slots[2] = {};
	bool valid[2] = {};
	for (uint8_t slot = 0u; slot < 2u; ++slot) {
		if (storage->primitives.read_slot(
			    storage->primitives.context, slot, &slots[slot]))
			valid[slot] = slot_valid(slots[slot]);
	}

	if (!valid[0] && !valid[1]) {
		storage->active_slot = kNoActiveSlot;
		storage->sequence = 0u;
		return false;
	}

	const uint8_t selected = newest_slot(slots, valid);
	storage->active_slot = selected;
	storage->sequence = slots[selected].sequence;
	*ledger = slots[selected].ledger;
	return true;
}

extern "C" bool aius_boot_storage_teensy_commit(
	AiusBootStorageTeensy *storage,
	const AiusBootLedger *ledger)
{
	if (storage == nullptr || ledger == nullptr || !storage->valid ||
	    !aius_boot_is_valid(ledger))
		return false;

	const uint8_t target =
		storage->active_slot == kNoActiveSlot
			? 0u
			: static_cast<uint8_t>(storage->active_slot ^ 1u);
	const uint32_t sequence =
		storage->active_slot == kNoActiveSlot
			? 1u
			: storage->sequence + 1u;

	uint8_t u32_bytes[sizeof(uint32_t)];
	put_u32(u32_bytes, 0u);
	if (!write_bytes(*storage,
	                 target,
	                 offsetof(AiusBootSlot, committed),
	                 u32_bytes,
	                 sizeof(u32_bytes)))
		return false;

	put_u32(u32_bytes, sequence);
	if (!write_bytes(*storage,
	                 target,
	                 offsetof(AiusBootSlot, sequence),
	                 u32_bytes,
	                 sizeof(u32_bytes)))
		return false;

	uint8_t ledger_bytes[sizeof(AiusBootLedger)];
	serialize_ledger(ledger_bytes, *ledger);
	if (!write_bytes(*storage,
	                 target,
	                 offsetof(AiusBootSlot, ledger),
	                 ledger_bytes,
	                 sizeof(ledger_bytes)))
		return false;

	const uint8_t zero_padding[sizeof(AiusBootSlot::padding)] = {};
	if (!write_bytes(*storage,
	                 target,
	                 offsetof(AiusBootSlot, padding),
	                 zero_padding,
	                 sizeof(zero_padding)))
		return false;

	put_u32(u32_bytes, slot_crc32(sequence, *ledger));
	if (!write_bytes(*storage,
	                 target,
	                 offsetof(AiusBootSlot, slot_crc32),
	                 u32_bytes,
	                 sizeof(u32_bytes)))
		return false;

	storage->primitives.flush_slot_range(
		storage->primitives.context, target, 0u, sizeof(AiusBootSlot));
	storage->primitives.barrier(storage->primitives.context);

	put_u32(u32_bytes, AIUS_BOOT_SLOT_COMMITTED);
	if (!write_bytes(*storage,
	                 target,
	                 offsetof(AiusBootSlot, committed),
	                 u32_bytes,
	                 sizeof(u32_bytes)))
		return false;

	storage->primitives.flush_slot_range(
		storage->primitives.context, target, 0u, kCacheLineBytes);
	storage->primitives.barrier(storage->primitives.context);

	storage->active_slot = target;
	storage->sequence = sequence;
	return true;
}

extern "C" uint32_t aius_boot_teensy_reset_source(
	const AiusBootTeensyPrimitives *primitives)
{
	if (primitives == nullptr ||
	    primitives->read_reset_source == nullptr ||
	    primitives->clear_reset_source == nullptr)
		return 0u;

	const uint32_t captured =
		primitives->read_reset_source(primitives->context);
	primitives->clear_reset_source(primitives->context, captured);
	return captured;
}

extern "C" AiusResetKind aius_boot_teensy_reset_kind(
	uint32_t raw_reset_source)
{
	if ((raw_reset_source & SRC_SRSR_WDOG_RST_B) != 0u)
		return AIUS_RESET_WATCHDOG;
	if ((raw_reset_source &
	     (SRC_SRSR_IPP_RESET_B | SRC_SRSR_CSU_RESET_B)) != 0u)
		return AIUS_RESET_COLD;
	if ((raw_reset_source &
	     (SRC_SRSR_JTAG_SW_RST | SRC_SRSR_LOCKUP_SYSRESETREQ)) != 0u)
		return AIUS_RESET_SOFTWARE;
	return AIUS_RESET_OTHER;
}

extern "C" bool aius_boot_teensy_watchdog_arm(
	const AiusBootTeensyPrimitives *primitives)
{
	if (primitives == nullptr ||
	    primitives->enable_watchdog_clock == nullptr ||
	    primitives->write_watchdog_wmcr == nullptr ||
	    primitives->write_watchdog_wcr == nullptr)
		return false;

	primitives->enable_watchdog_clock(primitives->context);
	primitives->write_watchdog_wmcr(primitives->context, 0u);
	primitives->write_watchdog_wcr(
		primitives->context,
		static_cast<uint16_t>(
			WDOG_WCR_SRS | WDOG_WCR_WT(15) | WDOG_WCR_WDE |
			WDOG_WCR_WDT | WDOG_WCR_SRE));
	return true;
}

extern "C" bool aius_boot_teensy_watchdog_feed(
	const AiusBootTeensyPrimitives *primitives)
{
	if (primitives == nullptr ||
	    primitives->write_watchdog_wsr == nullptr)
		return false;

	primitives->write_watchdog_wsr(primitives->context, UINT16_C(0x5555));
	primitives->write_watchdog_wsr(primitives->context, UINT16_C(0xaaaa));
	return true;
}

extern "C" AiusBootTeensyPrimitives
aius_boot_teensy_hardware_primitives(void)
{
	AiusBootTeensyPrimitives primitives = {};
	primitives.read_slot = hardware_read_slot;
	primitives.write_slot_bytes = hardware_write_slot_bytes;
	primitives.flush_slot_range = hardware_flush_slot_range;
	primitives.barrier = hardware_barrier;
	primitives.read_reset_source = hardware_read_reset_source;
	primitives.clear_reset_source = hardware_clear_reset_source;
	primitives.enable_watchdog_clock = hardware_enable_watchdog_clock;
	primitives.write_watchdog_wmcr = hardware_write_watchdog_wmcr;
	primitives.write_watchdog_wcr = hardware_write_watchdog_wcr;
	primitives.write_watchdog_wsr = hardware_write_watchdog_wsr;
	return primitives;
}
