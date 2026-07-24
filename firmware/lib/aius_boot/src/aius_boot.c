#include "aius_boot.h"

#include <stddef.h>
#include <string.h>

#define AIUS_BOOT_CRC_BYTES 24u

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
	crc ^= byte;
	for (unsigned int bit = 0; bit < 8; bit++) {
		uint32_t mask = 0u - (crc & 1u);
		crc = (crc >> 1) ^ (0xedb88320u & mask);
	}
	return crc;
}

static void put_u16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)(value & 0xffu);
	destination[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)(value & 0xffu);
	destination[1] = (uint8_t)((value >> 8) & 0xffu);
	destination[2] = (uint8_t)((value >> 16) & 0xffu);
	destination[3] = (uint8_t)(value >> 24);
}

static uint32_t ledger_crc32(const AiusBootLedger *ledger)
{
	uint8_t bytes[AIUS_BOOT_CRC_BYTES];
	put_u32(&bytes[0], ledger->magic);
	put_u16(&bytes[4], ledger->version);
	put_u16(&bytes[6], ledger->last_checkpoint);
	put_u32(&bytes[8], ledger->build_id);
	put_u32(&bytes[12], ledger->boot_generation);
	put_u32(&bytes[16], ledger->reset_source);
	bytes[20] = ledger->early_watchdog_resets;
	bytes[21] = ledger->mode;
	put_u16(&bytes[22], ledger->reserved);

	uint32_t crc = UINT32_MAX;
	for (size_t index = 0; index < sizeof(bytes); index++)
		crc = crc32_update(crc, bytes[index]);
	return crc ^ UINT32_MAX;
}

static void update_crc(AiusBootLedger *ledger)
{
	ledger->crc32 = ledger_crc32(ledger);
}

static void saturating_increment_u32(uint32_t *value)
{
	if (*value != UINT32_MAX)
		(*value)++;
}

static void saturating_increment_u8(uint8_t *value)
{
	if (*value != UINT8_MAX)
		(*value)++;
}

bool aius_boot_is_valid(const AiusBootLedger *ledger)
{
	if (ledger == NULL)
		return false;
	if (ledger->magic != AIUS_BOOT_MAGIC)
		return false;
	if (ledger->version != AIUS_BOOT_VERSION)
		return false;
	if (ledger->crc32 != ledger_crc32(ledger))
		return false;
	if (ledger->last_checkpoint >
	    AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE)
		return false;
	if (ledger->mode > AIUS_BOOT_MODE_RECOVERY_SAFE)
		return false;
	return ledger->reserved == 0u;
}

void aius_boot_init(AiusBootLedger *ledger,
	            const AiusBootLedger *retained,
	            AiusBootInput input)
{
	if (ledger == NULL)
		return;

	AiusBootLedger next;
	if (!aius_boot_is_valid(retained)) {
		memset(&next, 0, sizeof(next));
		next.magic = AIUS_BOOT_MAGIC;
		next.version = AIUS_BOOT_VERSION;
		next.build_id = input.build_id;
		next.boot_generation = 1u;
		next.reset_source = input.raw_reset_source;
		next.mode = AIUS_BOOT_MODE_NORMAL;
		update_crc(&next);
		*ledger = next;
		return;
	}

	next = *retained;
	saturating_increment_u32(&next.boot_generation);
	next.reset_source = input.raw_reset_source;

	if (next.build_id != input.build_id) {
		next.build_id = input.build_id;
		next.early_watchdog_resets = 0u;
		next.mode = AIUS_BOOT_MODE_NORMAL;
	} else if (input.reset_kind == AIUS_RESET_WATCHDOG &&
	           next.last_checkpoint !=
		           AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE) {
		saturating_increment_u8(&next.early_watchdog_resets);
		if (next.early_watchdog_resets >= 2u)
			next.mode = AIUS_BOOT_MODE_RECOVERY_SAFE;
	}

	update_crc(&next);
	*ledger = next;
}

void aius_boot_record_checkpoint(AiusBootLedger *ledger,
	                         uint16_t checkpoint)
{
	if (!aius_boot_is_valid(ledger) ||
	    checkpoint > AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE)
		return;
	ledger->last_checkpoint = checkpoint;
	update_crc(ledger);
}

void aius_boot_mark_healthy(AiusBootLedger *ledger)
{
	if (!aius_boot_is_valid(ledger))
		return;
	if (ledger->mode == AIUS_BOOT_MODE_NORMAL)
		ledger->early_watchdog_resets = 0u;
	update_crc(ledger);
}
