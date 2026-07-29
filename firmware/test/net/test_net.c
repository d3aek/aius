#include "aius_net.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "aius_boot.h"

enum {
	LEGACY_STATUS_LEN = 71,
	RECOVERY_STATUS_LEN = 99
};

static void load_status_fixture(const char *name, uint8_t *data, size_t size)
{
	static const char source_suffix[] = "test/net/test_net.c";
	static const char fixture_dir[] = "test/fixtures/status/";
	const char *source = __FILE__;
	size_t source_len = strlen(source);
	size_t suffix_len = strlen(source_suffix);
	char path[1024];

	assert(source_len >= suffix_len);
	assert(strcmp(source + source_len - suffix_len, source_suffix) == 0);
	size_t prefix_len = source_len - suffix_len;
	assert(prefix_len + sizeof(fixture_dir) - 1u + strlen(name) + 1u <=
	       sizeof(path));
	memcpy(path, source, prefix_len);
	memcpy(path + prefix_len, fixture_dir, sizeof(fixture_dir) - 1u);
	strcpy(path + prefix_len + sizeof(fixture_dir) - 1u, name);

	FILE *fixture = fopen(path, "rb");
	assert(fixture != NULL);
	assert(fread(data, 1, size, fixture) == size);
	assert(fgetc(fixture) == EOF);
	assert(fclose(fixture) == 0);
}

static AiusRecoveryStatus canonical_recovery_status(void)
{
	AiusRecoveryStatus recovery = {
		.network_state = AIUS_NET_STATE_RECOVERY_SAFE,
		.recovery_safe = true,
		.network_last_fault = AIUS_NET_FAULT_DRIVER_PROTOCOL,
		.last_checkpoint = AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE,
		.network_generation = UINT32_C(0xc1c2c3c4),
		.boot_generation = UINT32_C(0xd1d2d3d4),
		.reset_source = UINT32_C(0xe1e2e3e4),
		.watchdog_early_resets = 254,
		.loop_epoch = UINT32_C(0xf1f2f3f4),
		.network_restarts = UINT32_C(0x01020305),
	};
	return recovery;
}

static void assert_status_rejected(
	const uint8_t *legacy,
	const AiusRecoveryStatus *recovery,
	size_t capacity)
{
	uint8_t output[RECOVERY_STATUS_LEN + 3];
	uint8_t before[sizeof(output)];

	for (size_t index = 0; index < sizeof(output); index++)
		output[index] = (uint8_t)(0x5au ^ (uint8_t)index);
	memcpy(before, output, sizeof(output));
	assert(aius_net_status_encode(output, capacity, legacy, recovery) == 0);
	assert(memcmp(output, before, sizeof(output)) == 0);
}

static void test_status_canonical_fixture(void)
{
	uint8_t legacy[LEGACY_STATUS_LEN];
	uint8_t expected[RECOVERY_STATUS_LEN];
	uint8_t output[RECOVERY_STATUS_LEN + 1];
	AiusRecoveryStatus recovery = canonical_recovery_status();

	load_status_fixture("status-v1-71.bin", legacy, sizeof(legacy));
	load_status_fixture("status-v1-recovery-99.bin", expected,
	                    sizeof(expected));
	assert(memcmp(expected, legacy, sizeof(legacy)) == 0);

	memset(output, 0xa5, sizeof(output));
	assert(aius_net_status_encode(output, RECOVERY_STATUS_LEN, legacy,
	                              &recovery) == RECOVERY_STATUS_LEN);
	assert(memcmp(output, expected, RECOVERY_STATUS_LEN) == 0);
	assert(memcmp(output, legacy, LEGACY_STATUS_LEN) == 0);
	assert(output[RECOVERY_STATUS_LEN] == 0xa5);
}

static void test_status_invalid_inputs_do_not_mutate_output(void)
{
	uint8_t legacy[LEGACY_STATUS_LEN];
	AiusRecoveryStatus recovery = canonical_recovery_status();

	load_status_fixture("status-v1-71.bin", legacy, sizeof(legacy));

	assert(aius_net_status_encode(NULL, RECOVERY_STATUS_LEN, legacy,
	                              &recovery) == 0);
	assert_status_rejected(NULL, &recovery, RECOVERY_STATUS_LEN);
	assert_status_rejected(legacy, NULL, RECOVERY_STATUS_LEN);

	for (size_t capacity = 0; capacity < RECOVERY_STATUS_LEN; capacity++)
		assert_status_rejected(legacy, &recovery, capacity);

	size_t invalid_versions = 0;
	for (unsigned version = 0; version <= UINT8_MAX; version++) {
		if (version == AIUS_NET_VERSION)
			continue;
		legacy[0] = (uint8_t)version;
		assert_status_rejected(legacy, &recovery, RECOVERY_STATUS_LEN);
		invalid_versions++;
	}
	assert(invalid_versions == 255);
	legacy[0] = AIUS_NET_VERSION;

	size_t invalid_modes = 0;
	for (unsigned mode = 0; mode <= UINT8_MAX; mode++) {
		if (mode <= 1)
			continue;
		legacy[1] = (uint8_t)mode;
		assert_status_rejected(legacy, &recovery, RECOVERY_STATUS_LEN);
		invalid_modes++;
	}
	assert(invalid_modes == 254);
	legacy[1] = 1;

	size_t invalid_wire_states = 0;
	for (unsigned state = 6; state <= UINT8_MAX; state++) {
		recovery.network_state = (AiusNetState)state;
		assert_status_rejected(legacy, &recovery, RECOVERY_STATUS_LEN);
		invalid_wire_states++;
	}
	assert(invalid_wire_states == UINT8_MAX - 5u);

	static const int invalid_states[] = {INT_MIN, -1, 256, INT_MAX};
	for (size_t index = 0;
	     index < sizeof(invalid_states) / sizeof(invalid_states[0]);
	     index++) {
		recovery.network_state = (AiusNetState)invalid_states[index];
		assert_status_rejected(legacy, &recovery, RECOVERY_STATUS_LEN);
	}
	recovery = canonical_recovery_status();

	size_t invalid_wire_faults = 0;
	for (unsigned fault = 11; fault <= UINT8_MAX; fault++) {
		recovery.network_last_fault = (AiusNetFault)fault;
		assert_status_rejected(legacy, &recovery, RECOVERY_STATUS_LEN);
		invalid_wire_faults++;
	}
	assert(invalid_wire_faults == UINT8_MAX - 10u);

	static const int invalid_faults[] = {INT_MIN, -1, 256, INT_MAX};
	for (size_t index = 0;
	     index < sizeof(invalid_faults) / sizeof(invalid_faults[0]);
	     index++) {
		recovery.network_last_fault =
			(AiusNetFault)invalid_faults[index];
		assert_status_rejected(legacy, &recovery, RECOVERY_STATUS_LEN);
	}
	recovery = canonical_recovery_status();

	size_t invalid_checkpoints = 0;
	for (uint32_t checkpoint = AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE + 1u;
	     checkpoint <= UINT16_MAX;
	     checkpoint++) {
		recovery.last_checkpoint = (uint16_t)checkpoint;
		assert_status_rejected(legacy, &recovery, RECOVERY_STATUS_LEN);
		invalid_checkpoints++;
	}
	assert(invalid_checkpoints ==
	       UINT16_MAX - AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
}

static void test_status_valid_endpoints(void)
{
	uint8_t legacy[LEGACY_STATUS_LEN];
	uint8_t output[RECOVERY_STATUS_LEN];
	AiusRecoveryStatus recovery = canonical_recovery_status();

	load_status_fixture("status-v1-71.bin", legacy, sizeof(legacy));

	for (unsigned mode = 0; mode <= 1; mode++) {
		legacy[1] = (uint8_t)mode;
		assert(aius_net_status_encode(output, sizeof(output), legacy,
		                              &recovery) == sizeof(output));
		assert(memcmp(output, legacy, sizeof(legacy)) == 0);
	}

	for (unsigned state = 0; state <= 5; state++) {
		recovery.network_state = (AiusNetState)state;
		assert(aius_net_status_encode(output, sizeof(output), legacy,
		                              &recovery) == sizeof(output));
		assert(output[71] == state);
	}

	recovery = canonical_recovery_status();
	for (unsigned fault = 0; fault <= 10; fault++) {
		recovery.network_last_fault = (AiusNetFault)fault;
		assert(aius_net_status_encode(output, sizeof(output), legacy,
		                              &recovery) == sizeof(output));
		assert(output[73] == fault);
	}

	recovery = canonical_recovery_status();
	for (unsigned checkpoint = 0;
	     checkpoint <= AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE;
	     checkpoint++) {
		recovery.last_checkpoint = (uint16_t)checkpoint;
		assert(aius_net_status_encode(output, sizeof(output), legacy,
		                              &recovery) == sizeof(output));
		assert(output[74] == checkpoint);
	}

	recovery.network_state = AIUS_NET_STATE_STACK_START;
	recovery.recovery_safe = false;
	recovery.network_last_fault = AIUS_NET_FAULT_NONE;
	recovery.last_checkpoint = 0;
	assert(aius_net_status_encode(output, sizeof(output), legacy,
	                              &recovery) == sizeof(output));
	assert(output[71] == 0);
	assert(output[72] == 0);
	assert(output[73] == 0);
	assert(output[74] == 0);

	recovery = canonical_recovery_status();
	assert(aius_net_status_encode(output, SIZE_MAX, legacy, &recovery) ==
	       sizeof(output));
	assert(output[71] == 5);
	assert(output[72] == 1);
	assert(output[73] == 10);
	assert(output[74] == AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
}

static void test_status_uint32_max_serialization(void)
{
	uint8_t legacy[LEGACY_STATUS_LEN];
	uint8_t output[RECOVERY_STATUS_LEN];
	AiusRecoveryStatus recovery = canonical_recovery_status();

	load_status_fixture("status-v1-71.bin", legacy, sizeof(legacy));
	recovery.network_generation = UINT32_MAX;
	recovery.boot_generation = UINT32_MAX;
	recovery.reset_source = UINT32_MAX;
	recovery.watchdog_early_resets = UINT8_MAX;
	recovery.loop_epoch = UINT32_MAX;
	recovery.network_restarts = UINT32_MAX;
	assert(aius_net_status_encode(output, sizeof(output), legacy,
	                              &recovery) == sizeof(output));
	for (size_t index = 75; index < 88; index++)
		assert(output[index] == UINT8_MAX);
	assert(output[88] == 0);
	assert(output[89] == 0);
	assert(output[90] == 0);
	for (size_t index = 91; index < sizeof(output); index++)
		assert(output[index] == UINT8_MAX);
}

static void test_round_trip(void)
{
	uint8_t frame[AIUS_NET_MAX_FRAME];
	const uint8_t payload[] = {0x00, 0x12, 0xa5, 0xff};
	size_t len = aius_net_encode(frame, sizeof(frame), AIUS_NET_CMD_GET_STATUS,
	                             0x1234, 0xdeadbeefu, payload, sizeof(payload));
	assert(len == AIUS_NET_HEADER_LEN + sizeof(payload) + AIUS_NET_CRC_LEN);
	AiusNetFrame out;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_OK);
	assert(out.type == AIUS_NET_CMD_GET_STATUS);
	assert(out.flags == 0x1234);
	assert(out.request_id == 0xdeadbeefu);
	assert(out.payload_len == sizeof(payload));
	assert(memcmp(out.payload, payload, sizeof(payload)) == 0);
}

static void test_rejects(void)
{
	uint8_t frame[AIUS_NET_MAX_FRAME];
	size_t len = aius_net_encode(frame, sizeof(frame), AIUS_NET_CMD_DISCOVER,
	                             0, 7, NULL, 0);
	AiusNetFrame out;
	assert(aius_net_decode(frame, len - 1, &out) == AIUS_NET_ERR_SHORT);
	frame[0] ^= 1;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_MAGIC);
	frame[0] ^= 1;
	frame[4]++;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_VERSION);
	frame[4]--;
	frame[14] = 1;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_RESERVED);
	frame[14] = 0;
	frame[len - 1] ^= 0x80;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_CRC);
	assert(aius_net_encode(frame, sizeof(frame), 1, 0, 0, NULL, 1) == 0);
}

int main(void)
{
	test_round_trip();
	test_rejects();
	test_status_canonical_fixture();
	test_status_invalid_inputs_do_not_mutate_output();
	test_status_valid_endpoints();
	test_status_uint32_max_serialization();
	puts("ok net protocol");
	return 0;
}
