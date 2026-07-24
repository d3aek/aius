#include "aius_boot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
	do {                                                                     \
		if (!(condition)) {                                               \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,     \
			        __LINE__, #condition);                              \
			exit(1);                                                   \
		}                                                                \
	} while (0)

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
	crc ^= byte;
	for (unsigned int bit = 0; bit < 8; bit++) {
		uint32_t mask = 0u - (crc & 1u);
		crc = (crc >> 1) ^ (0xedb88320u & mask);
	}
	return crc;
}

static uint32_t crc32(const uint8_t *bytes, size_t length)
{
	uint32_t crc = UINT32_MAX;
	for (size_t index = 0; index < length; index++)
		crc = crc32_update(crc, bytes[index]);
	return crc ^ UINT32_MAX;
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

static void resign(AiusBootLedger *ledger)
{
	uint8_t bytes[24];
	put_u32(&bytes[0], ledger->magic);
	put_u16(&bytes[4], ledger->version);
	put_u16(&bytes[6], ledger->last_checkpoint);
	put_u32(&bytes[8], ledger->build_id);
	put_u32(&bytes[12], ledger->boot_generation);
	put_u32(&bytes[16], ledger->reset_source);
	bytes[20] = ledger->early_watchdog_resets;
	bytes[21] = ledger->mode;
	put_u16(&bytes[22], ledger->reserved);
	ledger->crc32 = crc32(bytes, sizeof(bytes));
}

static AiusBootInput input(uint32_t build_id, uint32_t reset_source,
	                   AiusResetKind kind)
{
	AiusBootInput result = {
		.build_id = build_id,
		.raw_reset_source = reset_source,
		.reset_kind = kind,
	};
	return result;
}

static AiusBootLedger fresh_ledger(uint32_t build_id)
{
	AiusBootLedger ledger;
	aius_boot_init(&ledger, NULL,
	               input(build_id, 0x10203040u, AIUS_RESET_COLD));
	CHECK(aius_boot_is_valid(&ledger));
	return ledger;
}

static void check_fresh(const AiusBootLedger *ledger, uint32_t build_id,
	                uint32_t reset_source)
{
	CHECK(ledger->magic == AIUS_BOOT_MAGIC);
	CHECK(ledger->version == AIUS_BOOT_VERSION);
	CHECK(ledger->last_checkpoint ==
	      AIUS_BOOT_CHECKPOINT_RESET_CAPTURE);
	CHECK(ledger->build_id == build_id);
	CHECK(ledger->boot_generation == 1u);
	CHECK(ledger->reset_source == reset_source);
	CHECK(ledger->early_watchdog_resets == 0u);
	CHECK(ledger->mode == AIUS_BOOT_MODE_NORMAL);
	CHECK(ledger->reserved == 0u);
	CHECK(aius_boot_is_valid(ledger));
}

static void check_invalid_mutations_are_no_op(AiusBootLedger ledger)
{
	AiusBootLedger before = ledger;
	aius_boot_record_checkpoint(&ledger, AIUS_BOOT_CHECKPOINT_HTTP);
	CHECK(memcmp(&ledger, &before, sizeof(ledger)) == 0);
	aius_boot_mark_healthy(&ledger);
	CHECK(memcmp(&ledger, &before, sizeof(ledger)) == 0);
}

static void test_fixed_ledger_crc_vector(void)
{
	static const uint8_t abi_bytes[24] = {
		0x42, 0x55, 0x49, 0x41, 0x01, 0x00, 0x09, 0x00,
		0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
		0xdd, 0xcc, 0xbb, 0xaa, 0x02, 0x01, 0x00, 0x00,
	};
	CHECK(crc32(abi_bytes, sizeof(abi_bytes)) == 0x11df68f2u);

	const AiusBootLedger ledger = {
		.magic = 0x41495542u,
		.version = 1u,
		.last_checkpoint = 9u,
		.build_id = 0x11223344u,
		.boot_generation = 0x55667788u,
		.reset_source = 0xaabbccddu,
		.early_watchdog_resets = 2u,
		.mode = 1u,
		.reserved = 0u,
		.crc32 = 0x11df68f2u,
	};
	CHECK(aius_boot_is_valid(&ledger));
}

static void test_integrity_and_domain_validation(void)
{
	CHECK(sizeof(AiusBootLedger) == 28u);
	CHECK(AIUS_BOOT_MAGIC == 0x41495542u);
	CHECK(crc32((const uint8_t *)"123456789", 9u) == 0xcbf43926u);
	CHECK(!aius_boot_is_valid(NULL));
	aius_boot_record_checkpoint(NULL, AIUS_BOOT_CHECKPOINT_HTTP);
	aius_boot_mark_healthy(NULL);

	AiusBootLedger output;
	aius_boot_init(&output, NULL,
	               input(0x11223344u, 0xaabbccddu,
	                     AIUS_RESET_WATCHDOG));
	check_fresh(&output, 0x11223344u, 0xaabbccddu);
	AiusBootLedger before = output;
	aius_boot_init(NULL, &output,
	               input(0x55667788u, 0x99aabbccu,
	                     AIUS_RESET_WATCHDOG));
	CHECK(memcmp(&output, &before, sizeof(output)) == 0);

	AiusBootLedger invalid = fresh_ledger(7u);
	invalid.magic ^= 1u;
	resign(&invalid);
	CHECK(!aius_boot_is_valid(&invalid));
	check_invalid_mutations_are_no_op(invalid);
	aius_boot_init(&output, &invalid,
	               input(8u, 11u, AIUS_RESET_COLD));
	check_fresh(&output, 8u, 11u);

	invalid = fresh_ledger(7u);
	invalid.version++;
	resign(&invalid);
	CHECK(!aius_boot_is_valid(&invalid));
	check_invalid_mutations_are_no_op(invalid);
	aius_boot_init(&output, &invalid,
	               input(9u, 12u, AIUS_RESET_SOFTWARE));
	check_fresh(&output, 9u, 12u);

	invalid = fresh_ledger(7u);
	invalid.crc32 ^= 0x80000000u;
	CHECK(!aius_boot_is_valid(&invalid));
	check_invalid_mutations_are_no_op(invalid);
	aius_boot_init(&output, &invalid,
	               input(10u, 13u, AIUS_RESET_OTHER));
	check_fresh(&output, 10u, 13u);

	invalid = fresh_ledger(7u);
	invalid.last_checkpoint =
		AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE + 1u;
	resign(&invalid);
	CHECK(!aius_boot_is_valid(&invalid));
	check_invalid_mutations_are_no_op(invalid);
	aius_boot_init(&output, &invalid,
	               input(11u, 14u, AIUS_RESET_WATCHDOG));
	check_fresh(&output, 11u, 14u);

	invalid = fresh_ledger(7u);
	invalid.mode = AIUS_BOOT_MODE_RECOVERY_SAFE + 1u;
	resign(&invalid);
	CHECK(!aius_boot_is_valid(&invalid));
	check_invalid_mutations_are_no_op(invalid);
	aius_boot_init(&output, &invalid,
	               input(12u, 15u, AIUS_RESET_COLD));
	check_fresh(&output, 12u, 15u);

	invalid = fresh_ledger(7u);
	invalid.reserved = 1u;
	resign(&invalid);
	CHECK(!aius_boot_is_valid(&invalid));
	check_invalid_mutations_are_no_op(invalid);
	aius_boot_init(&output, &invalid,
	               input(13u, 16u, AIUS_RESET_SOFTWARE));
	check_fresh(&output, 13u, 16u);
}

static void test_every_reset_kind(void)
{
	const AiusResetKind non_watchdog_kinds[] = {
		AIUS_RESET_COLD,
		AIUS_RESET_SOFTWARE,
		AIUS_RESET_OTHER,
	};

	for (size_t index = 0;
	     index < sizeof(non_watchdog_kinds) /
		             sizeof(non_watchdog_kinds[0]);
	     index++) {
		AiusBootLedger retained = fresh_ledger(100u);
		retained.last_checkpoint = AIUS_BOOT_CHECKPOINT_SD;
		retained.boot_generation = 41u;
		retained.early_watchdog_resets = 1u;
		resign(&retained);

		AiusBootLedger output;
		uint32_t raw = 0x80000000u | (uint32_t)index;
		aius_boot_init(&output, &retained,
		               input(100u, raw,
		                     non_watchdog_kinds[index]));
		CHECK(output.boot_generation == 42u);
		CHECK(output.reset_source == raw);
		CHECK(output.early_watchdog_resets == 1u);
		CHECK(output.mode == AIUS_BOOT_MODE_NORMAL);
		CHECK(output.last_checkpoint == AIUS_BOOT_CHECKPOINT_SD);
		CHECK(aius_boot_is_valid(&output));
	}

	AiusBootLedger retained = fresh_ledger(100u);
	retained.last_checkpoint = AIUS_BOOT_CHECKPOINT_NETWORK;
	resign(&retained);

	AiusBootLedger output;
	aius_boot_init(&output, &retained,
	               input(100u, 0x55u, AIUS_RESET_WATCHDOG));
	CHECK(output.early_watchdog_resets == 1u);
	CHECK(output.mode == AIUS_BOOT_MODE_NORMAL);
	CHECK(output.reset_source == 0x55u);
	CHECK(aius_boot_is_valid(&output));

	retained = output;
	aius_boot_init(&output, &retained,
	               input(100u, 0x56u, AIUS_RESET_WATCHDOG));
	CHECK(output.early_watchdog_resets == 2u);
	CHECK(output.mode == AIUS_BOOT_MODE_RECOVERY_SAFE);
	CHECK(output.reset_source == 0x56u);
	CHECK(aius_boot_is_valid(&output));

	retained = fresh_ledger(100u);
	retained.last_checkpoint = AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE;
	retained.early_watchdog_resets = 1u;
	resign(&retained);
	aius_boot_init(&output, &retained,
	               input(100u, 0x57u, AIUS_RESET_WATCHDOG));
	CHECK(output.early_watchdog_resets == 1u);
	CHECK(output.mode == AIUS_BOOT_MODE_NORMAL);
	CHECK(aius_boot_is_valid(&output));
}

static void test_build_change_clears_recovery_state(void)
{
	AiusBootLedger retained = fresh_ledger(100u);
	retained.last_checkpoint = AIUS_BOOT_CHECKPOINT_SD;
	retained.boot_generation = 8u;
	retained.early_watchdog_resets = 9u;
	retained.mode = AIUS_BOOT_MODE_RECOVERY_SAFE;
	resign(&retained);

	AiusBootLedger output;
	aius_boot_init(&output, &retained,
	               input(101u, 0xabcdef01u,
	                     AIUS_RESET_WATCHDOG));
	CHECK(output.build_id == 101u);
	CHECK(output.boot_generation == 9u);
	CHECK(output.reset_source == 0xabcdef01u);
	CHECK(output.last_checkpoint == AIUS_BOOT_CHECKPOINT_SD);
	CHECK(output.early_watchdog_resets == 0u);
	CHECK(output.mode == AIUS_BOOT_MODE_NORMAL);
	CHECK(aius_boot_is_valid(&output));
}

static void test_saturating_counters_and_monotonic_generation(void)
{
	const uint32_t generations[] = {0u, 1u, UINT32_MAX - 1u,
	                                UINT32_MAX};
	const uint32_t expected[] = {1u, 2u, UINT32_MAX, UINT32_MAX};

	for (size_t index = 0;
	     index < sizeof(generations) / sizeof(generations[0]); index++) {
		AiusBootLedger retained = fresh_ledger(1u);
		retained.boot_generation = generations[index];
		resign(&retained);

		AiusBootLedger output;
		aius_boot_init(&output, &retained,
		               input(1u, 0u, AIUS_RESET_OTHER));
		CHECK(output.boot_generation == expected[index]);
		CHECK(output.boot_generation >= retained.boot_generation);
		CHECK(aius_boot_is_valid(&output));
	}

	AiusBootLedger retained = fresh_ledger(1u);
	retained.last_checkpoint = AIUS_BOOT_CHECKPOINT_USB_RX;
	retained.early_watchdog_resets = UINT8_MAX - 1u;
	resign(&retained);

	AiusBootLedger output;
	aius_boot_init(&output, &retained,
	               input(1u, 0u, AIUS_RESET_WATCHDOG));
	CHECK(output.early_watchdog_resets == UINT8_MAX);
	CHECK(output.mode == AIUS_BOOT_MODE_RECOVERY_SAFE);

	retained = output;
	aius_boot_init(&output, &retained,
	               input(1u, 0u, AIUS_RESET_WATCHDOG));
	CHECK(output.early_watchdog_resets == UINT8_MAX);
	CHECK(output.mode == AIUS_BOOT_MODE_RECOVERY_SAFE);
	CHECK(aius_boot_is_valid(&output));
}

static void test_healthy_clear_cannot_exit_recovery(void)
{
	AiusBootLedger ledger = fresh_ledger(77u);
	ledger.last_checkpoint = AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE;
	ledger.early_watchdog_resets = 1u;
	resign(&ledger);

	uint32_t generation = ledger.boot_generation;
	uint32_t reset_source = ledger.reset_source;
	aius_boot_mark_healthy(&ledger);
	CHECK(ledger.early_watchdog_resets == 0u);
	CHECK(ledger.mode == AIUS_BOOT_MODE_NORMAL);
	CHECK(ledger.boot_generation == generation);
	CHECK(ledger.reset_source == reset_source);
	CHECK(ledger.last_checkpoint ==
	      AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	CHECK(aius_boot_is_valid(&ledger));

	ledger.early_watchdog_resets = 2u;
	ledger.mode = AIUS_BOOT_MODE_RECOVERY_SAFE;
	resign(&ledger);
	aius_boot_mark_healthy(&ledger);
	CHECK(ledger.early_watchdog_resets == 2u);
	CHECK(ledger.mode == AIUS_BOOT_MODE_RECOVERY_SAFE);
	CHECK(aius_boot_is_valid(&ledger));
}

static void test_checkpoint_persistence(void)
{
	const uint16_t checkpoints[] = {
		AIUS_BOOT_CHECKPOINT_RESET_CAPTURE,
		AIUS_BOOT_CHECKPOINT_USB_RX,
		AIUS_BOOT_CHECKPOINT_USB_TX,
		AIUS_BOOT_CHECKPOINT_METRICS,
		AIUS_BOOT_CHECKPOINT_NETWORK,
		AIUS_BOOT_CHECKPOINT_HTTP,
		AIUS_BOOT_CHECKPOINT_UDP,
		AIUS_BOOT_CHECKPOINT_REPLAY,
		AIUS_BOOT_CHECKPOINT_SD,
		AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE,
	};

	AiusBootLedger ledger = fresh_ledger(19u);
	for (size_t index = 0;
	     index < sizeof(checkpoints) / sizeof(checkpoints[0]); index++) {
		uint32_t generation = ledger.boot_generation;
		uint32_t reset_source = ledger.reset_source;
		uint8_t early = ledger.early_watchdog_resets;
		uint8_t mode = ledger.mode;
		aius_boot_record_checkpoint(&ledger, checkpoints[index]);
		CHECK(ledger.last_checkpoint == checkpoints[index]);
		CHECK(ledger.boot_generation == generation);
		CHECK(ledger.reset_source == reset_source);
		CHECK(ledger.early_watchdog_resets == early);
		CHECK(ledger.mode == mode);
		CHECK(aius_boot_is_valid(&ledger));
	}

	AiusBootLedger before = ledger;
	aius_boot_record_checkpoint(
		&ledger, AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE + 1u);
	CHECK(memcmp(&ledger, &before, sizeof(ledger)) == 0);
	aius_boot_record_checkpoint(&ledger, UINT16_MAX);
	CHECK(memcmp(&ledger, &before, sizeof(ledger)) == 0);

	aius_boot_record_checkpoint(&ledger, AIUS_BOOT_CHECKPOINT_SD);
	AiusBootLedger output;
	aius_boot_init(&output, &ledger,
	               input(19u, 22u, AIUS_RESET_SOFTWARE));
	CHECK(output.last_checkpoint == AIUS_BOOT_CHECKPOINT_SD);
	CHECK(aius_boot_is_valid(&output));
}

static void test_recovery_mode_persists_for_same_build(void)
{
	const AiusResetKind kinds[] = {
		AIUS_RESET_COLD,
		AIUS_RESET_SOFTWARE,
		AIUS_RESET_WATCHDOG,
		AIUS_RESET_OTHER,
	};
	for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]);
	     index++) {
		AiusBootLedger retained = fresh_ledger(5u);
		retained.last_checkpoint =
			AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE;
		retained.early_watchdog_resets = 2u;
		retained.mode = AIUS_BOOT_MODE_RECOVERY_SAFE;
		resign(&retained);

		AiusBootLedger output;
		aius_boot_init(&output, &retained,
		               input(5u, (uint32_t)index, kinds[index]));
		CHECK(output.early_watchdog_resets == 2u);
		CHECK(output.mode == AIUS_BOOT_MODE_RECOVERY_SAFE);
		CHECK(aius_boot_is_valid(&output));
	}
}

int main(void)
{
	test_fixed_ledger_crc_vector();
	test_integrity_and_domain_validation();
	test_every_reset_kind();
	test_build_change_clears_recovery_state();
	test_saturating_counters_and_monotonic_generation();
	test_healthy_clear_cannot_exit_recovery();
	test_checkpoint_persistence();
	test_recovery_mode_persists_for_same_build();
	puts("boot ledger tests passed");
	return 0;
}
