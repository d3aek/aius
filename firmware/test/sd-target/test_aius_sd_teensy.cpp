#include "aius_sd_teensy.h"
#include "aius_sd_free_scan.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

enum Primitive {
	PRIMITIVE_BEGIN,
	PRIMITIVE_MEDIA_PRESENT,
	PRIMITIVE_MKDIR,
	PRIMITIVE_OPEN,
	PRIMITIVE_TRUNCATE,
	PRIMITIVE_SEEK,
	PRIMITIVE_READ,
	PRIMITIVE_WRITE,
	PRIMITIVE_SIZE,
	PRIMITIVE_EXISTS,
	PRIMITIVE_RENAME,
	PRIMITIVE_REMOVE,
	PRIMITIVE_CLOSE,
	PRIMITIVE_TOTAL_SIZE,
	PRIMITIVE_USED_SIZE,
	PRIMITIVE_CACHE_CLEAR,
	PRIMITIVE_COUNT
};

/* Which partition the fake volume mounted, for the parked CACHE_DROP cases. */
enum MountedPartition {
	PARTITION_NONE,
	PARTITION_FAT,
	PARTITION_EXFAT
};

struct Fake {
	Primitive calls[64];
	size_t call_count;
	size_t clock_calls;
	uint32_t clocks[2];
	bool succeeds[PRIMITIVE_COUNT];
	bool media_present;
	/* The driver's own FAT-type numbering, as SdFat reports it. 0 is an
	 * unmounted volume, which is what every case that does not care about
	 * the volume type gets. */
	uint8_t fat_type;
	unsigned fat_type_reads;
	bool exists;
	bool open_valid;
	size_t read_result;
	size_t write_result;
	uint64_t size_result;
	uint64_t total_size;
	uint64_t used_size;
	uint8_t chip_select;
	uint8_t open_mode;
	const char *path;
	const char *path_to;
	uint8_t *mutable_buffer;
	const uint8_t *const_buffer;
	size_t length;
	uint64_t offset;
	/* The vendored driver's error state. A failing primitive leaves
	 * fail_code in error_code (0 models the card answering and refusing, a
	 * not-found OPEN); clearError() zeroes it, so a stale code a test
	 * pre-sets is gone before the primitive runs. */
	uint8_t error_code;
	uint8_t fail_code;
	bool card_detected;
	uint32_t busy_timeouts;
	unsigned clear_error_calls;
	bool begin_dma;
	/* DAT3 re-mux and read (NS3). Not card primitives, so not recorded in
	 * calls[]; each stamps a shared sequence so order is checkable. */
	unsigned sequence;
	unsigned pin_mode_calls;
	uint8_t pin;
	uint8_t pin_mode;
	unsigned pin_mode_at;
	unsigned delay_calls;
	uint32_t delay_us;
	unsigned delay_at;
	unsigned card_detected_reads;
	unsigned card_detected_at;
	/* Parked CACHE_DROP cases (owner decision pending). */
	MountedPartition mounted_partition;
	unsigned fat_cache_clears;
	unsigned exfat_cache_clears;
};

Fake g_fake;
AiusSdFreeScanResult g_scan_result;
uint64_t g_scan_cursor, g_scan_so_far;
bool g_scan_mounted;
uint8_t g_buffer[16];
const char kPath[] = "/path";
const char kPathTo[] = "/path-to";

void record(Primitive primitive)
{
	assert(g_fake.call_count <
	       sizeof(g_fake.calls) / sizeof(g_fake.calls[0]));
	g_fake.calls[g_fake.call_count++] = primitive;
}

bool outcome(bool ok)
{
	if (!ok && g_fake.fail_code != 0)
		g_fake.error_code = g_fake.fail_code;
	return ok;
}

void reset_fake()
{
	memset(&g_fake, 0, sizeof(g_fake));
	for (size_t i = 0; i < PRIMITIVE_COUNT; ++i)
		g_fake.succeeds[i] = true;
	g_fake.media_present = true;
	g_fake.fail_code = 5;
	g_fake.card_detected = true;
	g_fake.exists = true;
	g_fake.open_valid = true;
	g_fake.read_result = 3;
	g_fake.write_result = 3;
	g_fake.size_result = UINT64_C(123);
	g_fake.total_size = UINT64_C(1000);
	g_fake.used_size = UINT64_C(400);
	g_scan_result.status = AIUS_SD_FREE_SCAN_DONE;
	g_scan_result.chunk_free = 0;
	g_scan_result.used_bytes = g_fake.used_size;
	g_scan_cursor = g_scan_so_far = UINT64_MAX;
	g_scan_mounted = false;
	g_fake.clocks[0] = 100;
	g_fake.clocks[1] = 107;
}

void reset_adapter(AiusSdTeensy *adapter)
{
	adapter->file_open = false;
	adapter->last_duration_us = UINT32_C(0xa5a5a5a5);
	adapter->longest_duration_us = UINT32_C(0xa5a5a5a5);
	adapter->protocol_faults = UINT32_C(0xa5a5a5a5);
	adapter->last_fault = UINT32_C(0xa5a5a5a5);
	aius_sd_teensy_init(adapter);
	assert(!adapter->file_open);
	assert(adapter->last_duration_us == 0);
	assert(adapter->longest_duration_us == 0);
	assert(adapter->protocol_faults == 0);
	assert(adapter->last_fault == 0);
	assert(g_fake.call_count == 0);
	assert(g_fake.clock_calls == 0);
}

bool is_file_action(AiusSdActionKind kind)
{
	return kind == AIUS_SD_ACTION_TRUNCATE ||
	       kind == AIUS_SD_ACTION_SEEK ||
	       kind == AIUS_SD_ACTION_READ ||
	       kind == AIUS_SD_ACTION_WRITE ||
	       kind == AIUS_SD_ACTION_FILE_SIZE ||
	       kind == AIUS_SD_ACTION_CLOSE;
}

AiusSdAction make_action(AiusSdActionKind kind)
{
	AiusSdAction action = {};
	action.kind = kind;
	action.action_id = UINT64_C(47);
	switch (kind) {
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_OPEN_READ:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
	case AIUS_SD_ACTION_EXISTS:
	case AIUS_SD_ACTION_REMOVE:
		action.path = kPath;
		break;
	case AIUS_SD_ACTION_SEEK:
		action.offset = 9;
		break;
	case AIUS_SD_ACTION_READ:
	case AIUS_SD_ACTION_WRITE:
		action.buffer = g_buffer;
		action.length = 3;
		action.offset = 9;
		break;
	case AIUS_SD_ACTION_RENAME:
		action.path = kPath;
		action.path_to = kPathTo;
		break;
	case AIUS_SD_ACTION_NONE:
	case AIUS_SD_ACTION_BEGIN:
	case AIUS_SD_ACTION_MEDIA_PRESENT:
	case AIUS_SD_ACTION_TRUNCATE:
	case AIUS_SD_ACTION_NEED_INPUT:
	case AIUS_SD_ACTION_OUTPUT:
	case AIUS_SD_ACTION_FILE_SIZE:
	case AIUS_SD_ACTION_CLOSE:
	case AIUS_SD_ACTION_TOTAL_SIZE:
	case AIUS_SD_ACTION_USED_SIZE:
	case AIUS_SD_ACTION_PUBLISH_CONFIG:
	case AIUS_SD_ACTION_COMPLETE:
	case AIUS_SD_ACTION_FAIL:
	case AIUS_SD_ACTION_CACHE_DROP:
		break;
	}
	return action;
}

void grant_file_authority(AiusSdTeensy *adapter)
{
	adapter->file = File(true);
	adapter->file_open = true;
}

void prepare_authority(AiusSdTeensy *adapter, AiusSdActionKind kind)
{
	if (is_file_action(kind))
		grant_file_authority(adapter);
}

void assert_none_event(const AiusSdEvent &event)
{
	assert(event.kind == AIUS_SD_EVENT_NONE);
	assert(event.action_id == 0);
	assert(event.bytes == 0);
	assert(event.size == 0);
	assert(!event.value);
	assert(event.fault == 0);
	assert(event.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN);
}

/* Every assertion below the volume-type test uses an unmounted fake volume, so
 * the expected filesystem is UNKNOWN throughout and only that test states one. */
void assert_event(const AiusSdEvent &event, AiusSdEventKind kind,
	              uint64_t action_id, size_t bytes, uint64_t size,
	              bool value, uint32_t fault)
{
	assert(event.kind == kind);
	assert(event.action_id == action_id);
	assert(event.bytes == bytes);
	assert(event.size == size);
	assert(event.value == value);
	assert(event.fault == fault);
	assert(event.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN);
}

void assert_protocol_event(const AiusSdEvent &event, uint64_t action_id)
{
	assert_event(event, AIUS_SD_EVENT_FAULT, action_id, 0, 0, false,
	             AIUS_SD_TEENSY_PROTOCOL_FAULT);
}

void assert_driver_event(const AiusSdEvent &event, uint64_t action_id)
{
	assert_event(event, AIUS_SD_EVENT_FAULT, action_id, 0, 0, false,
	             AIUS_SD_TEENSY_DRIVER_FAULT);
}

struct CatalogRow {
	const char *key;
	AiusSdActionKind kind;
	bool eligible;
	Primitive primitive;
	AiusSdEventKind event;
};

const CatalogRow kCatalog[] = {
	{ "none", AIUS_SD_ACTION_NONE, false, PRIMITIVE_COUNT,
	  AIUS_SD_EVENT_NONE },
	{ "begin", AIUS_SD_ACTION_BEGIN, true, PRIMITIVE_BEGIN,
	  AIUS_SD_EVENT_OK },
	{ "media-present", AIUS_SD_ACTION_MEDIA_PRESENT, true,
	  PRIMITIVE_MEDIA_PRESENT, AIUS_SD_EVENT_BOOL },
	{ "mkdir", AIUS_SD_ACTION_MKDIR, true, PRIMITIVE_MKDIR,
	  AIUS_SD_EVENT_OK },
	{ "open-read", AIUS_SD_ACTION_OPEN_READ, true, PRIMITIVE_OPEN,
	  AIUS_SD_EVENT_OK },
	{ "open-write-begin", AIUS_SD_ACTION_OPEN_WRITE_BEGIN, true,
	  PRIMITIVE_OPEN, AIUS_SD_EVENT_OK },
	{ "open-append", AIUS_SD_ACTION_OPEN_APPEND, true, PRIMITIVE_OPEN,
	  AIUS_SD_EVENT_OK },
	{ "truncate", AIUS_SD_ACTION_TRUNCATE, true, PRIMITIVE_TRUNCATE,
	  AIUS_SD_EVENT_OK },
	{ "seek", AIUS_SD_ACTION_SEEK, true, PRIMITIVE_SEEK,
	  AIUS_SD_EVENT_OK },
	{ "need-input", AIUS_SD_ACTION_NEED_INPUT, false, PRIMITIVE_COUNT,
	  AIUS_SD_EVENT_NONE },
	{ "read", AIUS_SD_ACTION_READ, true, PRIMITIVE_READ,
	  AIUS_SD_EVENT_BYTES },
	{ "write", AIUS_SD_ACTION_WRITE, true, PRIMITIVE_WRITE,
	  AIUS_SD_EVENT_BYTES },
	{ "output", AIUS_SD_ACTION_OUTPUT, false, PRIMITIVE_COUNT,
	  AIUS_SD_EVENT_NONE },
	{ "file-size", AIUS_SD_ACTION_FILE_SIZE, true, PRIMITIVE_SIZE,
	  AIUS_SD_EVENT_SIZE },
	{ "exists", AIUS_SD_ACTION_EXISTS, true, PRIMITIVE_EXISTS,
	  AIUS_SD_EVENT_BOOL },
	{ "rename", AIUS_SD_ACTION_RENAME, true, PRIMITIVE_RENAME,
	  AIUS_SD_EVENT_OK },
	{ "remove", AIUS_SD_ACTION_REMOVE, true, PRIMITIVE_REMOVE,
	  AIUS_SD_EVENT_OK },
	{ "close", AIUS_SD_ACTION_CLOSE, true, PRIMITIVE_CLOSE,
	  AIUS_SD_EVENT_OK },
	{ "total-size", AIUS_SD_ACTION_TOTAL_SIZE, true,
	  PRIMITIVE_TOTAL_SIZE, AIUS_SD_EVENT_SIZE },
	{ "used-size", AIUS_SD_ACTION_USED_SIZE, true,
	  PRIMITIVE_USED_SIZE, AIUS_SD_EVENT_SIZE },
	{ "publish-config", AIUS_SD_ACTION_PUBLISH_CONFIG, false,
	  PRIMITIVE_COUNT, AIUS_SD_EVENT_NONE },
	{ "complete", AIUS_SD_ACTION_COMPLETE, false, PRIMITIVE_COUNT,
	  AIUS_SD_EVENT_NONE },
	{ "fail", AIUS_SD_ACTION_FAIL, false, PRIMITIVE_COUNT,
	  AIUS_SD_EVENT_NONE },
};

void assert_success_event(const CatalogRow &row, const AiusSdEvent &event)
{
	if (!row.eligible) {
		assert_none_event(event);
		return;
	}
	switch (row.event) {
	case AIUS_SD_EVENT_OK:
		assert_event(event, row.event, 47, 0, 0, false, 0);
		break;
	case AIUS_SD_EVENT_BOOL:
		assert_event(event, row.event, 47, 0, 0, true, 0);
		break;
	case AIUS_SD_EVENT_BYTES:
		assert_event(event, row.event, 47, 3, 0, false, 0);
		break;
	case AIUS_SD_EVENT_SIZE: {
		uint64_t expected = g_fake.size_result;
		if (row.kind == AIUS_SD_ACTION_TOTAL_SIZE)
			expected = g_fake.total_size;
		else if (row.kind == AIUS_SD_ACTION_USED_SIZE)
			expected = g_fake.used_size;
		assert_event(event, row.event, 47, 0, expected, false, 0);
		break;
	}
	case AIUS_SD_EVENT_NONE:
	case AIUS_SD_EVENT_FAULT:
		assert(0);
		break;
	}
}

void assert_primitive_payload(AiusSdActionKind kind)
{
	switch (kind) {
	case AIUS_SD_ACTION_BEGIN:
		assert(g_fake.chip_select == BUILTIN_SDCARD);
		break;
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_OPEN_READ:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
	case AIUS_SD_ACTION_EXISTS:
	case AIUS_SD_ACTION_REMOVE:
		assert(g_fake.path == kPath);
		break;
	case AIUS_SD_ACTION_TRUNCATE:
		assert(g_fake.offset == 0);
		break;
	case AIUS_SD_ACTION_SEEK:
		assert(g_fake.offset == 9);
		break;
	case AIUS_SD_ACTION_READ:
		assert(g_fake.mutable_buffer == g_buffer);
		assert(g_fake.length == 3);
		break;
	case AIUS_SD_ACTION_WRITE:
		assert(g_fake.const_buffer == g_buffer);
		assert(g_fake.length == 3);
		break;
	case AIUS_SD_ACTION_RENAME:
		assert(g_fake.path == kPath);
		assert(g_fake.path_to == kPathTo);
		break;
	case AIUS_SD_ACTION_MEDIA_PRESENT:
	case AIUS_SD_ACTION_FILE_SIZE:
	case AIUS_SD_ACTION_CLOSE:
	case AIUS_SD_ACTION_TOTAL_SIZE:
	case AIUS_SD_ACTION_USED_SIZE:
	case AIUS_SD_ACTION_CACHE_DROP:
		break;
	case AIUS_SD_ACTION_NONE:
	case AIUS_SD_ACTION_NEED_INPUT:
	case AIUS_SD_ACTION_OUTPUT:
	case AIUS_SD_ACTION_PUBLISH_CONFIG:
	case AIUS_SD_ACTION_COMPLETE:
	case AIUS_SD_ACTION_FAIL:
		assert(0);
		break;
	}
	if (kind == AIUS_SD_ACTION_OPEN_READ)
		assert(g_fake.open_mode == FILE_READ);
	else if (kind == AIUS_SD_ACTION_OPEN_WRITE_BEGIN)
		assert(g_fake.open_mode == FILE_WRITE_BEGIN);
	else if (kind == AIUS_SD_ACTION_OPEN_APPEND)
		assert(g_fake.open_mode == FILE_WRITE);
}

void test_init_and_fixed_catalog()
{
	assert(sizeof(kCatalog) / sizeof(kCatalog[0]) == 23);
	size_t eligible = 0;
	size_t inert = 0;
	for (size_t i = 0; i < sizeof(kCatalog) / sizeof(kCatalog[0]); ++i) {
		assert((unsigned)kCatalog[i].kind == i);
		assert(kCatalog[i].key[0] != '\0');
		for (size_t j = 0; j < i; ++j)
			assert(strcmp(kCatalog[i].key, kCatalog[j].key) != 0);
		reset_fake();
		AiusSdTeensy adapter;
		reset_adapter(&adapter);
		prepare_authority(&adapter, kCatalog[i].kind);
		AiusSdAction action = make_action(kCatalog[i].kind);
		AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
		assert_success_event(kCatalog[i], event);
		if (kCatalog[i].eligible) {
			assert(g_fake.call_count == 1);
			assert(g_fake.calls[0] == kCatalog[i].primitive);
			assert(g_fake.clock_calls == 2);
			assert(adapter.last_duration_us == 7);
			assert(adapter.longest_duration_us == 7);
			assert(adapter.last_fault == 0);
			assert_primitive_payload(kCatalog[i].kind);
			eligible++;
		} else {
			assert(g_fake.call_count == 0);
			assert(g_fake.clock_calls == 0);
			assert(adapter.last_duration_us == 0);
			assert(adapter.longest_duration_us == 0);
			assert(adapter.protocol_faults == 0);
			inert++;
		}
		if (kCatalog[i].kind == AIUS_SD_ACTION_OPEN_READ ||
		    kCatalog[i].kind == AIUS_SD_ACTION_OPEN_WRITE_BEGIN ||
		    kCatalog[i].kind == AIUS_SD_ACTION_OPEN_APPEND)
			assert(adapter.file_open);
		else if (kCatalog[i].kind == AIUS_SD_ACTION_CLOSE)
			assert(!adapter.file_open);
	}
	assert(eligible == 17);
	assert(inert == 6);
}

void configure_failure(AiusSdActionKind kind)
{
	switch (kind) {
	case AIUS_SD_ACTION_BEGIN:
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_TRUNCATE:
	case AIUS_SD_ACTION_SEEK:
	case AIUS_SD_ACTION_RENAME:
	case AIUS_SD_ACTION_REMOVE:
		g_fake.succeeds[
			kind == AIUS_SD_ACTION_BEGIN ? PRIMITIVE_BEGIN :
			kind == AIUS_SD_ACTION_MKDIR ? PRIMITIVE_MKDIR :
			kind == AIUS_SD_ACTION_TRUNCATE ? PRIMITIVE_TRUNCATE :
			kind == AIUS_SD_ACTION_SEEK ? PRIMITIVE_SEEK :
			kind == AIUS_SD_ACTION_RENAME ? PRIMITIVE_RENAME :
			PRIMITIVE_REMOVE] = false;
		break;
	case AIUS_SD_ACTION_OPEN_READ:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
		g_fake.open_valid = false;
		break;
	case AIUS_SD_ACTION_READ:
		g_fake.read_result = 4;
		break;
	case AIUS_SD_ACTION_WRITE:
		g_fake.write_result = 4;
		break;
	default:
		assert(0);
	}
}

void test_storage_failures_and_authority()
{
	static const AiusSdActionKind fallible[] = {
		AIUS_SD_ACTION_BEGIN,
		AIUS_SD_ACTION_MKDIR,
		AIUS_SD_ACTION_OPEN_READ,
		AIUS_SD_ACTION_OPEN_WRITE_BEGIN,
		AIUS_SD_ACTION_OPEN_APPEND,
		AIUS_SD_ACTION_TRUNCATE,
		AIUS_SD_ACTION_SEEK,
		AIUS_SD_ACTION_READ,
		AIUS_SD_ACTION_WRITE,
		AIUS_SD_ACTION_RENAME,
		AIUS_SD_ACTION_REMOVE,
	};
	for (size_t i = 0; i < sizeof(fallible) / sizeof(fallible[0]); ++i) {
		reset_fake();
		AiusSdTeensy adapter;
		reset_adapter(&adapter);
		prepare_authority(&adapter, fallible[i]);
		bool had_authority = adapter.file_open;
		configure_failure(fallible[i]);
		AiusSdAction action = make_action(fallible[i]);
		AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
		assert_driver_event(event, 47);
		assert(adapter.last_fault == AIUS_SD_TEENSY_DRIVER_FAULT);
		assert(adapter.protocol_faults == 0);
		assert(adapter.last_duration_us == 7);
		assert(adapter.longest_duration_us == 7);
		assert(g_fake.call_count == 1);
		assert(g_fake.clock_calls == 2);
		if (fallible[i] == AIUS_SD_ACTION_OPEN_READ ||
		    fallible[i] == AIUS_SD_ACTION_OPEN_WRITE_BEGIN ||
		    fallible[i] == AIUS_SD_ACTION_OPEN_APPEND)
			assert(!adapter.file_open);
		else
			assert(adapter.file_open == had_authority);
	}
}

void test_read_write_result_boundaries()
{
	static const AiusSdActionKind kinds[] = {
		AIUS_SD_ACTION_READ, AIUS_SD_ACTION_WRITE
	};
	static const size_t results[] = { 0, 3, 4, SIZE_MAX };
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
		for (size_t j = 0; j < sizeof(results) / sizeof(results[0]); ++j) {
			reset_fake();
			AiusSdTeensy adapter;
			reset_adapter(&adapter);
			grant_file_authority(&adapter);
			if (kinds[i] == AIUS_SD_ACTION_READ)
				g_fake.read_result = results[j];
			else
				g_fake.write_result = results[j];
			AiusSdAction action = make_action(kinds[i]);
			AiusSdEvent event =
				aius_sd_teensy_execute(&adapter, &action);
			if (results[j] <= action.length)
				assert_event(event, AIUS_SD_EVENT_BYTES, 47,
				             results[j], 0, false, 0);
			else
				assert_driver_event(event, 47);
			assert(adapter.file_open);
			assert(g_fake.call_count == 1);
			assert(g_fake.clock_calls == 2);
		}
	}
}

void test_boolean_and_integer_boundaries()
{
	static const AiusSdActionKind boolean_kinds[] = {
		AIUS_SD_ACTION_MEDIA_PRESENT, AIUS_SD_ACTION_EXISTS
	};
	for (size_t i = 0;
	     i < sizeof(boolean_kinds) / sizeof(boolean_kinds[0]); ++i) {
		reset_fake();
		AiusSdTeensy adapter;
		reset_adapter(&adapter);
		g_fake.media_present = false;
		g_fake.exists = false;
		AiusSdAction action = make_action(boolean_kinds[i]);
		action.action_id = UINT64_MAX;
		AiusSdEvent event =
			aius_sd_teensy_execute(&adapter, &action);
		assert_event(event, AIUS_SD_EVENT_BOOL, UINT64_MAX,
		             0, 0, false, 0);
		assert(g_fake.call_count == 1);
		assert(g_fake.clock_calls == 2);
		assert(adapter.last_fault == 0);
	}

	static const AiusSdActionKind offset_kinds[] = {
		AIUS_SD_ACTION_SEEK,
		AIUS_SD_ACTION_READ,
		AIUS_SD_ACTION_WRITE,
	};
	for (size_t i = 0;
	     i < sizeof(offset_kinds) / sizeof(offset_kinds[0]); ++i) {
		reset_fake();
		AiusSdTeensy adapter;
		reset_adapter(&adapter);
		grant_file_authority(&adapter);
		AiusSdAction action = make_action(offset_kinds[i]);
		action.offset = (uint64_t)INT64_MAX;
		AiusSdEvent event =
			aius_sd_teensy_execute(&adapter, &action);
		if (offset_kinds[i] == AIUS_SD_ACTION_SEEK)
			assert_event(event, AIUS_SD_EVENT_OK, 47,
			             0, 0, false, 0);
		else
			assert_event(event, AIUS_SD_EVENT_BYTES, 47,
			             3, 0, false, 0);
		if (offset_kinds[i] == AIUS_SD_ACTION_SEEK)
			assert(g_fake.offset == (uint64_t)INT64_MAX);
		assert(g_fake.call_count == 1);
		assert(g_fake.clock_calls == 2);
	}
}

enum ActionField {
	FIELD_PATH,
	FIELD_PATH_TO,
	FIELD_BUFFER,
	FIELD_LENGTH,
	FIELD_OFFSET,
	FIELD_FAULT,
	FIELD_COUNT
};

#define FIELD_BIT(field) (UINT32_C(1) << (field))

uint32_t named_fields(AiusSdActionKind kind)
{
	switch (kind) {
	case AIUS_SD_ACTION_MKDIR:
	case AIUS_SD_ACTION_OPEN_READ:
	case AIUS_SD_ACTION_OPEN_WRITE_BEGIN:
	case AIUS_SD_ACTION_OPEN_APPEND:
	case AIUS_SD_ACTION_EXISTS:
	case AIUS_SD_ACTION_REMOVE:
		return FIELD_BIT(FIELD_PATH);
	case AIUS_SD_ACTION_TRUNCATE:
	case AIUS_SD_ACTION_SEEK:
		return FIELD_BIT(FIELD_OFFSET);
	case AIUS_SD_ACTION_READ:
	case AIUS_SD_ACTION_WRITE:
		return FIELD_BIT(FIELD_BUFFER) |
		       FIELD_BIT(FIELD_LENGTH) |
		       FIELD_BIT(FIELD_OFFSET);
	case AIUS_SD_ACTION_RENAME:
		return FIELD_BIT(FIELD_PATH) | FIELD_BIT(FIELD_PATH_TO);
	case AIUS_SD_ACTION_USED_SIZE:
		return FIELD_BIT(FIELD_LENGTH) | FIELD_BIT(FIELD_OFFSET);
	default:
		return 0;
	}
}

void mutate_field(AiusSdAction *action, ActionField field)
{
	switch (field) {
	case FIELD_PATH: action->path = kPath; break;
	case FIELD_PATH_TO: action->path_to = kPathTo; break;
	case FIELD_BUFFER: action->buffer = g_buffer; break;
	case FIELD_LENGTH: action->length = 1; break;
	case FIELD_OFFSET: action->offset = 1; break;
	case FIELD_FAULT: action->fault = 1; break;
	case FIELD_COUNT: assert(0); break;
	}
}

void expect_protocol_rejection(AiusSdAction action, bool with_authority)
{
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	if (with_authority)
		grant_file_authority(&adapter);
	adapter.longest_duration_us = 91;
	AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
	assert_protocol_event(event, action.action_id);
	assert(adapter.protocol_faults == 1);
	assert(adapter.last_fault == AIUS_SD_TEENSY_PROTOCOL_FAULT);
	assert(adapter.last_duration_us == 0);
	assert(adapter.longest_duration_us == 91);
	assert(g_fake.call_count == 0);
	assert(g_fake.clock_calls == 0);
}

void test_exact_payload_validation()
{
	size_t unnamed_trials = 0;
	size_t id_trials = 0;
	for (size_t i = 0; i < sizeof(kCatalog) / sizeof(kCatalog[0]); ++i) {
		if (!kCatalog[i].eligible)
			continue;
		AiusSdAction base = make_action(kCatalog[i].kind);
		for (unsigned field = 0; field < FIELD_COUNT; ++field) {
			if ((named_fields(base.kind) & FIELD_BIT(field)) != 0)
				continue;
			AiusSdAction malformed = base;
			mutate_field(&malformed, (ActionField)field);
			expect_protocol_rejection(
				malformed, is_file_action(malformed.kind));
			unnamed_trials++;
		}
		base.action_id = 0;
		expect_protocol_rejection(base, is_file_action(base.kind));
		id_trials++;
	}
	assert(id_trials == 17);
	assert(unnamed_trials == 84);

	static const AiusSdActionKind path_actions[] = {
		AIUS_SD_ACTION_MKDIR,
		AIUS_SD_ACTION_OPEN_READ,
		AIUS_SD_ACTION_OPEN_WRITE_BEGIN,
		AIUS_SD_ACTION_OPEN_APPEND,
		AIUS_SD_ACTION_EXISTS,
		AIUS_SD_ACTION_RENAME,
		AIUS_SD_ACTION_REMOVE,
	};
	for (size_t i = 0; i < sizeof(path_actions) / sizeof(path_actions[0]); ++i) {
		AiusSdAction malformed = make_action(path_actions[i]);
		malformed.path = nullptr;
		expect_protocol_rejection(malformed, false);
	}
	AiusSdAction rename = make_action(AIUS_SD_ACTION_RENAME);
	rename.path_to = nullptr;
	expect_protocol_rejection(rename, false);

	static const AiusSdActionKind transfer[] = {
		AIUS_SD_ACTION_READ, AIUS_SD_ACTION_WRITE
	};
	for (size_t i = 0; i < sizeof(transfer) / sizeof(transfer[0]); ++i) {
		AiusSdAction malformed = make_action(transfer[i]);
		malformed.buffer = nullptr;
		expect_protocol_rejection(malformed, true);
		malformed = make_action(transfer[i]);
		malformed.length = 0;
		expect_protocol_rejection(malformed, true);
		malformed = make_action(transfer[i]);
		malformed.length = SIZE_MAX;
		expect_protocol_rejection(malformed, true);
		malformed = make_action(transfer[i]);
		malformed.offset = (uint64_t)INT64_MAX + UINT64_C(1);
		expect_protocol_rejection(malformed, true);
	}
	/* TRUNCATE now carries a length (FR-S1), bounded as SEEK's is. */
	AiusSdAction truncate = make_action(AIUS_SD_ACTION_TRUNCATE);
	truncate.offset = (uint64_t)INT64_MAX + UINT64_C(1);
	expect_protocol_rejection(truncate, true);
	AiusSdAction seek = make_action(AIUS_SD_ACTION_SEEK);
	seek.offset = (uint64_t)INT64_MAX + UINT64_C(1);
	expect_protocol_rejection(seek, true);

	static const AiusSdActionKind opens[] = {
		AIUS_SD_ACTION_BEGIN,
		AIUS_SD_ACTION_OPEN_READ,
		AIUS_SD_ACTION_OPEN_WRITE_BEGIN,
		AIUS_SD_ACTION_OPEN_APPEND,
	};
	for (size_t i = 0; i < sizeof(opens) / sizeof(opens[0]); ++i)
		expect_protocol_rejection(make_action(opens[i]), true);
	static const AiusSdActionKind file_actions[] = {
		AIUS_SD_ACTION_TRUNCATE,
		AIUS_SD_ACTION_SEEK,
		AIUS_SD_ACTION_READ,
		AIUS_SD_ACTION_WRITE,
		AIUS_SD_ACTION_FILE_SIZE,
		AIUS_SD_ACTION_CLOSE,
	};
	for (size_t i = 0;
	     i < sizeof(file_actions) / sizeof(file_actions[0]); ++i)
		expect_protocol_rejection(make_action(file_actions[i]), false);
}

/* Phase 4zd: USED_SIZE takes the scan-cursor payload (offset = sector cursor,
 * length = free clusters so far) and still refuses the old non-empty shapes. */
void test_used_size_cursor_payload()
{
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	adapter.mounted = true;
	AiusSdAction action = make_action(AIUS_SD_ACTION_USED_SIZE);
	action.offset = 64;
	action.length = 1234;
	g_scan_result.status = AIUS_SD_FREE_SCAN_MORE;
	g_scan_result.chunk_free = 0;
	AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
	assert_event(event, AIUS_SD_EVENT_BYTES, 47, 0, 0, false, 0);
	assert(g_scan_cursor == 64 && g_scan_so_far == 1234 && g_scan_mounted);
	assert(g_fake.call_count == 1 && g_fake.calls[0] == PRIMITIVE_USED_SIZE);
	assert(adapter.last_fault == 0);

	g_scan_result.chunk_free = 4096;
	g_fake.clock_calls = 0;
	event = aius_sd_teensy_execute(&adapter, &action);
	assert_event(event, AIUS_SD_EVENT_BYTES, 47, 4096, 0, false, 0);

	g_scan_result.status = AIUS_SD_FREE_SCAN_DONE;
	g_scan_result.used_bytes = UINT64_C(3955871744);
	g_fake.clock_calls = 0;
	event = aius_sd_teensy_execute(&adapter, &action);
	assert_event(event, AIUS_SD_EVENT_SIZE, 47, 0, UINT64_C(3955871744),
	             false, 0);

	g_scan_result.status = AIUS_SD_FREE_SCAN_PROTOCOL;
	g_fake.clock_calls = 0;
	event = aius_sd_teensy_execute(&adapter, &action);
	assert_protocol_event(event, 47);
	assert(adapter.mounted);

	g_scan_result.status = AIUS_SD_FREE_SCAN_DRIVER;
	g_fake.clock_calls = 0;
	event = aius_sd_teensy_execute(&adapter, &action);
	assert_driver_event(event, 47);
	assert(!adapter.mounted);

	/* The old non-empty shapes and over-wide cursors stay refused. */
	for (unsigned shape = 0; shape < 4; ++shape) {
		AiusSdAction bad = make_action(AIUS_SD_ACTION_USED_SIZE);
		switch (shape) {
		case 0: bad.path = kPath; break;
		case 1: bad.path_to = kPathTo; break;
		case 2: bad.buffer = g_buffer; break;
		case 3: bad.fault = 1; break;
		}
		expect_protocol_rejection(bad, false);
	}
	/* Any offset and length reach the scan step, which owns the bound. */
	g_scan_result.status = AIUS_SD_FREE_SCAN_MORE;
	g_scan_result.chunk_free = 0;
	AiusSdAction wide_action = make_action(AIUS_SD_ACTION_USED_SIZE);
	wide_action.offset = UINT64_MAX;
	wide_action.length = SIZE_MAX;
	g_fake.clock_calls = 0;
	event = aius_sd_teensy_execute(&adapter, &wide_action);
	assert_event(event, AIUS_SD_EVENT_BYTES, 47, 0, 0, false, 0);
	assert(g_scan_cursor == UINT64_MAX && g_scan_so_far == SIZE_MAX);
	AiusSdAction no_id = make_action(AIUS_SD_ACTION_USED_SIZE);
	no_id.action_id = 0;
	expect_protocol_rejection(no_id, false);
}

void test_inert_null_unknown_and_duplicate()
{
	for (size_t i = 0; i < sizeof(kCatalog) / sizeof(kCatalog[0]); ++i) {
		if (kCatalog[i].eligible)
			continue;
		reset_fake();
		AiusSdTeensy adapter;
		reset_adapter(&adapter);
		adapter.longest_duration_us = 33;
		AiusSdAction action = make_action(kCatalog[i].kind);
		action.action_id = 99;
		action.path = kPath;
		action.path_to = kPathTo;
		action.buffer = g_buffer;
		action.length = 3;
		action.offset = 4;
		action.fault = 5;
		AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
		assert_none_event(event);
		assert(adapter.protocol_faults == 0);
		assert(adapter.last_duration_us == 0);
		assert(adapter.longest_duration_us == 33);
		assert(g_fake.call_count == 0);
		assert(g_fake.clock_calls == 0);
	}

	reset_fake();
	AiusSdAction valid = make_action(AIUS_SD_ACTION_MEDIA_PRESENT);
	assert_none_event(aius_sd_teensy_execute(nullptr, &valid));
	assert(g_fake.call_count == 0);
	assert(g_fake.clock_calls == 0);

	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	adapter.last_duration_us = 8;
	adapter.longest_duration_us = 9;
	assert_none_event(aius_sd_teensy_execute(&adapter, nullptr));
	assert(adapter.last_duration_us == 0);
	assert(adapter.longest_duration_us == 9);
	assert(g_fake.call_count == 0);
	assert(g_fake.clock_calls == 0);

	AiusSdAction unknown = {};
	/* 23 is CACHE_DROP since Phase 4h; the first unassigned kind is 24. */
	unknown.kind = (AiusSdActionKind)24;
	unknown.action_id = 71;
	expect_protocol_rejection(unknown, false);

	reset_fake();
	reset_adapter(&adapter);
	AiusSdAction duplicate = make_action(AIUS_SD_ACTION_MEDIA_PRESENT);
	duplicate.action_id = UINT64_C(0x1122334455667788);
	AiusSdEvent first = aius_sd_teensy_execute(&adapter, &duplicate);
	AiusSdEvent second = aius_sd_teensy_execute(&adapter, &duplicate);
	assert_event(first, AIUS_SD_EVENT_BOOL, duplicate.action_id,
	             0, 0, true, 0);
	assert_event(second, AIUS_SD_EVENT_BOOL, duplicate.action_id,
	             0, 0, true, 0);
	assert(g_fake.call_count == 2);
	assert(g_fake.clock_calls == 4);
}

/* The probe reports the mounted volume's type, and reading it is not a second
 * card primitive. Both halves matter: the status surface went blind when this
 * information had no producer, and the one-primitive-per-epoch rule is what
 * bounds the watchdog's worst case. */
void test_media_present_reports_the_volume_type()
{
	struct Row {
		uint8_t fat_type;
		AiusSdFilesystem filesystem;
	};
	/* 64 is exFAT and 255 is nothing at all; both are UNKNOWN because the
	 * status wire vocabulary names FAT12, FAT16 and FAT32 only. */
	static const Row kRows[] = {
		{ 0u, AIUS_SD_FILESYSTEM_UNKNOWN },
		{ 12u, AIUS_SD_FILESYSTEM_FAT12 },
		{ 16u, AIUS_SD_FILESYSTEM_FAT16 },
		{ 32u, AIUS_SD_FILESYSTEM_FAT32 },
		{ 64u, AIUS_SD_FILESYSTEM_UNKNOWN },
		{ 255u, AIUS_SD_FILESYSTEM_UNKNOWN },
	};
	for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i) {
		reset_fake();
		AiusSdTeensy adapter;
		reset_adapter(&adapter);
		g_fake.fat_type = kRows[i].fat_type;
		AiusSdAction action =
			make_action(AIUS_SD_ACTION_MEDIA_PRESENT);
		AiusSdEvent event =
			aius_sd_teensy_execute(&adapter, &action);
		assert(event.kind == AIUS_SD_EVENT_BOOL);
		assert(event.action_id == 47);
		assert(event.value);
		assert(event.fault == 0);
		assert(event.filesystem == kRows[i].filesystem);
		assert(g_fake.call_count == 1);
		assert(g_fake.calls[0] == PRIMITIVE_MEDIA_PRESENT);
		assert(g_fake.fat_type_reads == 1);
		assert(g_fake.clock_calls == 2);
		assert(adapter.last_fault == 0);
	}

	/* No card, no volume to ask: the type is UNKNOWN and unread. */
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	g_fake.media_present = false;
	g_fake.fat_type = 32u;
	AiusSdAction action = make_action(AIUS_SD_ACTION_MEDIA_PRESENT);
	AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
	assert(event.kind == AIUS_SD_EVENT_BOOL);
	assert(!event.value);
	assert(event.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN);
	assert(g_fake.fat_type_reads == 0);
	assert(g_fake.call_count == 1);

	/* EXISTS is the other action answering with a BOOL, and never carries a
	 * volume type -- the portable engine faults a job whose event does. */
	reset_fake();
	reset_adapter(&adapter);
	g_fake.fat_type = 32u;
	action = make_action(AIUS_SD_ACTION_EXISTS);
	event = aius_sd_teensy_execute(&adapter, &action);
	assert(event.kind == AIUS_SD_EVENT_BOOL);
	assert(event.filesystem == AIUS_SD_FILESYSTEM_UNKNOWN);
	assert(g_fake.fat_type_reads == 0);
}

void test_durations_and_counter_saturation()
{
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	AiusSdAction action = make_action(AIUS_SD_ACTION_MEDIA_PRESENT);
	g_fake.clocks[0] = 45;
	g_fake.clocks[1] = 45;
	AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
	assert_event(event, AIUS_SD_EVENT_BOOL, 47, 0, 0, true, 0);
	assert(adapter.last_duration_us == 0);
	assert(adapter.longest_duration_us == 0);

	reset_fake();
	reset_adapter(&adapter);
	g_fake.clocks[0] = UINT32_MAX - 2u;
	g_fake.clocks[1] = 2;
	event = aius_sd_teensy_execute(&adapter, &action);
	assert_event(event, AIUS_SD_EVENT_BOOL, 47, 0, 0, true, 0);
	assert(adapter.last_duration_us == 5);
	assert(adapter.longest_duration_us == 5);

	reset_fake();
	reset_adapter(&adapter);
	adapter.protocol_faults = UINT32_MAX;
	adapter.longest_duration_us = 17;
	action.action_id = 0;
	event = aius_sd_teensy_execute(&adapter, &action);
	assert_protocol_event(event, 0);
	assert(adapter.protocol_faults == UINT32_MAX);
	assert(adapter.last_duration_us == 0);
	assert(adapter.longest_duration_us == 17);
}

/* The diagnostic counters, which exist to attribute a failure that reaches a
 * client as a bare 500.
 *
 * `mounts` is the load-bearing one: it counts SD.begin() calls that actually
 * reached the card, so a job can be asked afterwards whether the volume was
 * remounted while it held it. A BEGIN the file_open guard refused never touched
 * the card and must not be counted -- that refusal is a protocol fault, and
 * conflating the two would make a collision look like a mount. */
void test_diagnostic_counters()
{
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	assert(adapter.mounts == 0);
	assert(adapter.driver_faults == 0);
	assert(adapter.last_protocol_action == AIUS_SD_ACTION_NONE);

	/* A mount that reached the card counts, succeed or fail. */
	AiusSdAction begin = make_action(AIUS_SD_ACTION_BEGIN);
	AiusSdEvent event = aius_sd_teensy_execute(&adapter, &begin);
	assert(event.kind == AIUS_SD_EVENT_OK);
	assert(adapter.mounts == 1);
	assert(adapter.driver_faults == 0);

	reset_fake();
	/* The mount above is cached, so a second BEGIN would be answered without
	 * reaching the card. Invalidate first, or this case would be measuring
	 * the cache instead of the counter. */
	aius_sd_teensy_invalidate_mount(&adapter);
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(event.kind == AIUS_SD_EVENT_FAULT);
	assert(event.fault == AIUS_SD_TEENSY_DRIVER_FAULT);
	assert(adapter.mounts == 2);
	assert(adapter.driver_faults == 1);
	assert(adapter.last_protocol_action == AIUS_SD_ACTION_NONE);

	/* A BEGIN refused because a file is open never reaches the card. This is
	 * the cross-owner collision case: the volume is not remounted, the
	 * asking job fails, and last_protocol_action names what was refused. */
	reset_fake();
	adapter.file_open = true;
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(event.kind == AIUS_SD_EVENT_FAULT);
	assert(event.fault == AIUS_SD_TEENSY_PROTOCOL_FAULT);
	assert(g_fake.call_count == 0);
	assert(adapter.mounts == 2);
	assert(adapter.protocol_faults == 1);
	assert(adapter.last_protocol_action == AIUS_SD_ACTION_BEGIN);

	/* A refused open names itself too, so a second owner colliding on the
	 * one shared handle is attributable to the action it tried. */
	reset_fake();
	AiusSdAction open_read = make_action(AIUS_SD_ACTION_OPEN_READ);
	event = aius_sd_teensy_execute(&adapter, &open_read);
	assert(event.fault == AIUS_SD_TEENSY_PROTOCOL_FAULT);
	assert(adapter.protocol_faults == 2);
	assert(adapter.last_protocol_action == AIUS_SD_ACTION_OPEN_READ);
	assert(adapter.mounts == 2);

	/* Both counters saturate rather than wrap. */
	reset_fake();
	reset_adapter(&adapter);
	adapter.mounts = UINT32_MAX;
	adapter.driver_faults = UINT32_MAX;
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(event.fault == AIUS_SD_TEENSY_DRIVER_FAULT);
	assert(adapter.mounts == UINT32_MAX);
	assert(adapter.driver_faults == UINT32_MAX);
}

/* Performs a BEGIN and reports whether it reached the card. */
bool begin_touched_the_card(AiusSdTeensy *adapter)
{
	reset_fake();
	AiusSdAction begin = make_action(AIUS_SD_ACTION_BEGIN);
	AiusSdEvent event = aius_sd_teensy_execute(adapter, &begin);
	assert(event.kind == AIUS_SD_EVENT_OK ||
	       event.kind == AIUS_SD_EVENT_FAULT);
	return g_fake.call_count != 0;
}

/* THE MOUNT CACHE, and every path that must invalidate it.
 *
 * The portable machine opens every job with a BEGIN, and SDClass::begin() has no
 * idempotence of its own: it re-runs the whole SDIO card identification and
 * volume mount each time. So the adapter mounted once per job -- which the first
 * live run of a working replay measured as 224 mounts for 224 jobs, 20 of them
 * blocking a full second before failing. That behaviour was plainly visible in
 * this file for months and nothing here objected; test_diagnostic_counters even
 * depended on it. Hence this test.
 *
 * The bargain is that the cache is REVALIDATED rather than assumed, so each
 * invalidation path is asserted individually rather than trusted as a set. */
void test_mount_is_cached_and_revalidated()
{
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	assert(!adapter.mounted);
	assert(adapter.mount_reuses == 0);

	/* The first mount reaches the card. */
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounted);
	assert(adapter.mounts == 1);
	assert(adapter.mount_reuses == 0);

	/* The second does not -- no card transaction at all, which is also why
	 * this cannot weaken the one-blocking-primitive-per-epoch bound: it
	 * removes a primitive, it never adds one. */
	assert(!begin_touched_the_card(&adapter));
	assert(adapter.mounts == 1);
	assert(adapter.mount_reuses == 1);
	assert(adapter.last_duration_us == 0);

	/* Invalidation path 1: the caller says so. */
	aius_sd_teensy_invalidate_mount(&adapter);
	assert(!adapter.mounted);
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounts == 2);

	/* Invalidation path 2: the probe finds the card gone. A media check that
	 * still reports the card present must NOT invalidate, or the cache would
	 * be dropped every second and buy nothing. */
	reset_fake();
	AiusSdAction media = make_action(AIUS_SD_ACTION_MEDIA_PRESENT);
	AiusSdEvent event = aius_sd_teensy_execute(&adapter, &media);
	assert(event.kind == AIUS_SD_EVENT_BOOL && event.value);
	assert(adapter.mounted);
	assert(!begin_touched_the_card(&adapter));
	assert(adapter.mounts == 2);

	reset_fake();
	g_fake.media_present = false;
	event = aius_sd_teensy_execute(&adapter, &media);
	assert(event.kind == AIUS_SD_EVENT_BOOL && !event.value);
	assert(!adapter.mounted);
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounts == 3);

	/* Invalidation path 3: a failed mount is not a mount. */
	reset_fake();
	aius_sd_teensy_invalidate_mount(&adapter);
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	AiusSdAction begin = make_action(AIUS_SD_ACTION_BEGIN);
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(event.kind == AIUS_SD_EVENT_FAULT);
	assert(!adapter.mounted);
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounts == 5);

	/* Invalidation path 4: the card refused an unrelated primitive. A driver
	 * fault anywhere means the volume is no longer trusted. */
	reset_fake();
	assert(adapter.mounted);
	g_fake.succeeds[PRIMITIVE_REMOVE] = false;
	AiusSdAction remove = make_action(AIUS_SD_ACTION_REMOVE);
	event = aius_sd_teensy_execute(&adapter, &remove);
	assert(event.kind == AIUS_SD_EVENT_FAULT);
	assert(!adapter.mounted);
	assert(begin_touched_the_card(&adapter));

	/* Invalidation path 5: init. A reused adapter never inherits a mount. */
	reset_fake();
	assert(adapter.mounted);
	aius_sd_teensy_init(&adapter);
	assert(!adapter.mounted);
	assert(adapter.mount_reuses == 0);
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounts == 1);

	/* A protocol-refused BEGIN neither mounts nor counts as a reuse: it never
	 * reached the decision at all. */
	reset_fake();
	adapter.file_open = true;
	const uint32_t reuses = adapter.mount_reuses;
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(event.fault == AIUS_SD_TEENSY_PROTOCOL_FAULT);
	assert(g_fake.call_count == 0);
	assert(adapter.mount_reuses == reuses);
	adapter.file_open = false;

	/* The reuse counter saturates rather than wrapping, like its siblings. */
	reset_fake();
	adapter.mount_reuses = UINT32_MAX;
	assert(adapter.mounted);
	assert(!begin_touched_the_card(&adapter));
	assert(adapter.mount_reuses == UINT32_MAX);

	/* Null-safe, like every other adapter entry point. */
	aius_sd_teensy_invalidate_mount(nullptr);
}

/* THE FAULT SPLIT (Phase 4e). The driver's error code, cleared before every
 * primitive, decides whether a failure is the card failing or a plain miss. */
void test_fault_split_and_absence()
{
	AiusSdTeensy adapter;
	AiusSdAction open_read = make_action(AIUS_SD_ACTION_OPEN_READ);
	AiusSdAction begin = make_action(AIUS_SD_ACTION_BEGIN);
	AiusSdEvent event;

	/* A not-found OPEN (code 0) counts open_misses, keeps the mount, and is
	 * not a driver fault; the job still sees a FAULT event. */
	reset_fake();
	reset_adapter(&adapter);
	assert(begin_touched_the_card(&adapter) && adapter.mounted);
	reset_fake();
	g_fake.open_valid = false;
	g_fake.fail_code = 0;
	event = aius_sd_teensy_execute(&adapter, &open_read);
	assert(event.kind == AIUS_SD_EVENT_FAULT);
	assert(event.fault == AIUS_SD_DRIVER_FAULT_MISS);
	assert(adapter.open_misses == 1);
	assert(adapter.driver_faults == 0);
	assert(adapter.mounted);
	assert(!adapter.file_open);

	/* A nonzero code is the card failing: counted, mount invalidated. */
	reset_fake();
	g_fake.open_valid = false;
	g_fake.fail_code = 7;
	event = aius_sd_teensy_execute(&adapter, &open_read);
	assert(event.kind == AIUS_SD_EVENT_FAULT);
	assert(event.fault == AIUS_SD_TEENSY_DRIVER_FAULT);
	assert(adapter.open_misses == 1);
	assert(adapter.driver_faults == 1);
	assert(!adapter.mounted);

	/* A stale nonzero code from an earlier primitive does not turn a later
	 * not-found OPEN into a driver fault: clearError() runs first. */
	reset_fake();
	reset_adapter(&adapter);
	assert(begin_touched_the_card(&adapter));
	reset_fake();
	g_fake.error_code = 9;
	g_fake.open_valid = false;
	g_fake.fail_code = 0;
	event = aius_sd_teensy_execute(&adapter, &open_read);
	assert(g_fake.clear_error_calls == 1);
	assert(adapter.open_misses == 1);
	assert(adapter.driver_faults == 0);
	assert(adapter.mounted);

	/* clearError() precedes every card primitive, one per execute. */
	reset_fake();
	AiusSdAction exists = make_action(AIUS_SD_ACTION_EXISTS);
	exists.path = kPath;
	aius_sd_teensy_execute(&adapter, &exists);
	assert(g_fake.clear_error_calls == 1);
	reset_fake();
	AiusSdAction media = make_action(AIUS_SD_ACTION_MEDIA_PRESENT);
	aius_sd_teensy_execute(&adapter, &media);
	assert(g_fake.clear_error_calls == 1);

	/* A failed BEGIN with no card inserted counts absent_mounts, leaves
	 * driver_faults alone and leaves the mount unset. */
	reset_fake();
	reset_adapter(&adapter);
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	g_fake.card_detected = false;
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(event.kind == AIUS_SD_EVENT_FAULT);
	assert(adapter.absent_mounts == 1);
	assert(adapter.driver_faults == 0);
	assert(adapter.mounts == 1);
	assert(!adapter.mounted);

	/* With the card present the same failure is a driver fault. */
	reset_fake();
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(adapter.absent_mounts == 1);
	assert(adapter.driver_faults == 1);

	/* A failed BEGIN whose driver code is zero is still a mount failure,
	 * not a miss, when the card is present. */
	reset_fake();
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	g_fake.fail_code = 0;
	event = aius_sd_teensy_execute(&adapter, &begin);
	assert(adapter.driver_faults == 2);
	assert(adapter.open_misses == 0);

	/* The counters saturate. */
	reset_fake();
	reset_adapter(&adapter);
	adapter.absent_mounts = UINT32_MAX;
	adapter.open_misses = UINT32_MAX;
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	g_fake.card_detected = false;
	aius_sd_teensy_execute(&adapter, &begin);
	assert(adapter.absent_mounts == UINT32_MAX);
	reset_fake();
	assert(begin_touched_the_card(&adapter));
	reset_fake();
	g_fake.open_valid = false;
	g_fake.fail_code = 0;
	aius_sd_teensy_execute(&adapter, &open_read);
	assert(adapter.open_misses == UINT32_MAX);
}

/* busy_timeouts is mirrored from the driver after each primitive. */
void test_busy_timeouts_surface()
{
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	assert(adapter.busy_timeouts == 0);
	g_fake.busy_timeouts = 3;
	AiusSdAction exists = make_action(AIUS_SD_ACTION_EXISTS);
	exists.path = kPath;
	aius_sd_teensy_execute(&adapter, &exists);
	assert(adapter.busy_timeouts == 3);
}

/* Mode-aware mount: one begin() call carrying the mode; a mode change
 * invalidates the mount only with no file open. */
void test_mode_and_remount()
{
	reset_fake();
	AiusSdTeensy adapter;
	reset_adapter(&adapter);
	assert(!adapter.dma);
	assert(begin_touched_the_card(&adapter));
	assert(!g_fake.begin_dma);
	assert(g_fake.call_count == 1);

	/* Same mode: the mount stays. */
	aius_sd_teensy_set_mode(&adapter, false);
	assert(adapter.mounted);

	/* A change with no file open forces a remount in the new mode. */
	aius_sd_teensy_set_mode(&adapter, true);
	assert(adapter.dma);
	assert(!adapter.mounted);
	assert(begin_touched_the_card(&adapter));
	assert(g_fake.begin_dma);
	assert(g_fake.call_count == 1);
	assert(adapter.mounted);

	/* A change with a file open records the mode but keeps the mount. */
	adapter.file_open = true;
	aius_sd_teensy_set_mode(&adapter, false);
	assert(!adapter.dma);
	assert(adapter.mounted);
	adapter.file_open = false;

	/* init returns to FIFO; null is safe. */
	aius_sd_teensy_set_mode(&adapter, true);
	aius_sd_teensy_init(&adapter);
	assert(!adapter.dma);
	aius_sd_teensy_set_mode(nullptr, true);
}

/* TRUNCATE truncates to the action's offset: a copy-before-write pass's new
 * length, and REPLACE's 0 unchanged (FR-S1). One primitive each. */
void test_truncate_carries_offset()
{
	static const uint64_t lengths[] = { 0u, 200u, 12288u };
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		reset_fake();
		AiusSdTeensy adapter;
		reset_adapter(&adapter);
		grant_file_authority(&adapter);
		g_fake.offset = UINT64_C(0xa5a5);
		AiusSdAction action = make_action(AIUS_SD_ACTION_TRUNCATE);
		action.offset = lengths[i];
		AiusSdEvent event = aius_sd_teensy_execute(&adapter, &action);
		assert_event(event, AIUS_SD_EVENT_OK, 47, 0, 0, false, 0);
		assert(g_fake.call_count == 1);
		assert(g_fake.calls[0] == PRIMITIVE_TRUNCATE);
		assert(g_fake.offset == lengths[i]);
		assert(adapter.file_open);
	}
}

/* D-S1: a hard driver fault (a write or read error) drops the mount but is
 * no card-gone signal by itself: the platform re-muxes DAT3
 * (aius_sd_teensy_invalidate_mount) and reads it, and only an absent card is
 * card-gone (aius_optical_sd_card_gone, optical-app). Both ways. */
void test_driver_fault_then_card_presence()
{
	for (int pulled = 0; pulled < 2; ++pulled) {
		AiusSdTeensy adapter;
		reset_fake();
		reset_adapter(&adapter);
		assert(begin_touched_the_card(&adapter));
		assert(adapter.mounted && !adapter.dat3_remuxed);

		reset_fake();
		g_fake.succeeds[PRIMITIVE_MKDIR] = false; /* fail_code 5: nonzero */
		g_fake.card_detected = pulled == 0;
		AiusSdAction mkdir = make_action(AIUS_SD_ACTION_MKDIR);
		AiusSdEvent event = aius_sd_teensy_execute(&adapter, &mkdir);
		assert(event.kind == AIUS_SD_EVENT_FAULT);
		assert(event.fault == AIUS_SD_TEENSY_DRIVER_FAULT);
		assert(adapter.driver_faults == 1);
		/* The stale-descriptor rule stands: the mount is dropped, DAT3 is
		 * still the bus's, and the fault alone reads present. */
		assert(!adapter.mounted && !adapter.dat3_remuxed);
		assert(aius_sd_teensy_card_detected(&adapter));
		assert(g_fake.card_detected_reads == 0);
		/* The platform's presence read after the lost mount. */
		aius_sd_teensy_invalidate_mount(&adapter);
		assert(adapter.dat3_remuxed);
		assert(aius_sd_teensy_card_detected(&adapter) == (pulled == 0));
		assert(g_fake.card_detected_reads == 1);
		/* A present card remounts at the next BEGIN (the S5 retry). */
		if (pulled == 0) {
			reset_fake();
			assert(begin_touched_the_card(&adapter));
			assert(adapter.mounted && !adapter.dat3_remuxed);
		}
	}
}

/* aius_sd_teensy_card_detected() and the DAT3 re-mux (S1, N7, NS3, D3, E3). */
void test_card_detected_and_dat3()
{
	AiusSdTeensy adapter;

	/* A fresh boot: no mount and DAT3 never re-muxed, so the accessor
	 * reports present without reading DAT3, and the first probe's BEGIN
	 * runs as today (D3). */
	reset_fake();
	reset_adapter(&adapter);
	assert(!adapter.dat3_remuxed);
	g_fake.card_detected = false;
	assert(aius_sd_teensy_card_detected(&adapter));
	assert(g_fake.card_detected_reads == 0);
	assert(adapter.absent_mounts == 0);
	assert(g_fake.call_count == 0);
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounted && !adapter.dat3_remuxed);

	/* Mounted: present with no read, and BEGIN takes the reused mount. */
	reset_fake();
	g_fake.card_detected = false;
	assert(aius_sd_teensy_card_detected(&adapter));
	assert(g_fake.card_detected_reads == 0);
	assert(!begin_touched_the_card(&adapter));
	assert(adapter.mount_reuses == 1);

	/* A card-gone invalidation with no file open re-muxes pin 46 to
	 * INPUT_PULLDOWN and waits 5 us before the accessor's first read, with
	 * no card primitive; a pulled card then reads absent and counts. */
	reset_fake();
	aius_sd_teensy_invalidate_mount(&adapter);
	assert(!adapter.mounted && adapter.dat3_remuxed);
	assert(g_fake.pin_mode_calls == 1);
	assert(g_fake.pin == 46u && g_fake.pin_mode == INPUT_PULLDOWN);
	assert(g_fake.delay_calls == 1 && g_fake.delay_us == 5u);
	assert(g_fake.pin_mode_at < g_fake.delay_at);
	assert(g_fake.call_count == 0 && g_fake.clock_calls == 0);
	g_fake.card_detected = false;
	assert(!aius_sd_teensy_card_detected(&adapter));
	assert(g_fake.card_detected_reads == 1);
	assert(g_fake.delay_at < g_fake.card_detected_at);
	assert(adapter.absent_mounts == 1);
	assert(g_fake.call_count == 0);
	g_fake.card_detected = true;
	assert(aius_sd_teensy_card_detected(&adapter));
	assert(adapter.absent_mounts == 1);

	/* Already re-muxed: a second invalidation re-muxes nothing. */
	aius_sd_teensy_invalidate_mount(&adapter);
	assert(g_fake.pin_mode_calls == 1 && g_fake.delay_calls == 1);

	/* A successful begin clears dat3_remuxed; a failed one sets it. */
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounted && !adapter.dat3_remuxed);
	reset_fake();
	aius_sd_teensy_invalidate_mount(&adapter);
	assert(adapter.dat3_remuxed);
	adapter.dat3_remuxed = false;
	reset_fake();
	g_fake.succeeds[PRIMITIVE_BEGIN] = false;
	AiusSdAction begin = make_action(AIUS_SD_ACTION_BEGIN);
	aius_sd_teensy_execute(&adapter, &begin);
	assert(!adapter.mounted && adapter.dat3_remuxed);
	assert(g_fake.pin_mode_calls == 0);

	/* With a file open, no re-mux: the handle still owns the bus. */
	reset_fake();
	reset_adapter(&adapter);
	assert(begin_touched_the_card(&adapter));
	reset_fake();
	grant_file_authority(&adapter);
	aius_sd_teensy_invalidate_mount(&adapter);
	assert(!adapter.mounted);
	assert(!adapter.dat3_remuxed);
	assert(g_fake.pin_mode_calls == 0 && g_fake.delay_calls == 0);
	g_fake.card_detected = false;
	assert(aius_sd_teensy_card_detected(&adapter));
	assert(g_fake.card_detected_reads == 0);
	adapter.file_open = false;

	/* A mode change drops the mount, re-muxes nothing and leaves
	 * dat3_remuxed clear, so the accessor reads nothing and the next BEGIN
	 * remounts (E3). */
	reset_fake();
	reset_adapter(&adapter);
	assert(begin_touched_the_card(&adapter));
	reset_fake();
	aius_sd_teensy_set_mode(&adapter, true);
	assert(!adapter.mounted && !adapter.dat3_remuxed);
	assert(g_fake.pin_mode_calls == 0 && g_fake.delay_calls == 0);
	g_fake.card_detected = false;
	assert(aius_sd_teensy_card_detected(&adapter));
	assert(g_fake.card_detected_reads == 0);
	assert(begin_touched_the_card(&adapter));
	assert(adapter.mounted && g_fake.begin_dma);

	/* The counter saturates; null is safe. */
	reset_fake();
	reset_adapter(&adapter);
	adapter.dat3_remuxed = true;
	adapter.absent_mounts = UINT32_MAX;
	g_fake.card_detected = false;
	assert(!aius_sd_teensy_card_detected(&adapter));
	assert(adapter.absent_mounts == UINT32_MAX);
	assert(!aius_sd_teensy_card_detected(nullptr));
}

/* CACHE_DROP (H1, CR4-B2): one cacheClear of the mounted partition through the
 * FsVolume vendor forwarder, on FAT and exFAT, and nothing else. */
void test_cache_drop()
{
	AiusSdTeensy adapter;
	AiusSdAction drop = make_action(AIUS_SD_ACTION_CACHE_DROP);

	/* No payload, a nonzero id, and no file open (the engine issues it
	 * between the last pass's CLOSE and the read-back's OPEN_READ). */
	AiusSdAction malformed = drop;
	malformed.action_id = 0;
	expect_protocol_rejection(malformed, false);
	malformed = drop;
	malformed.path = kPath;
	expect_protocol_rejection(malformed, false);
	malformed = drop;
	malformed.offset = 1;
	expect_protocol_rejection(malformed, false);
	malformed = drop;
	malformed.buffer = g_buffer;
	malformed.length = 1;
	expect_protocol_rejection(malformed, false);
	expect_protocol_rejection(drop, true);

	/* No volume mounted: the forwarder answers nullptr with no card
	 * transaction, a FAULT event with clearError() first and the duration
	 * recorded, and no driver fault counted. */
	reset_fake();
	reset_adapter(&adapter);
	g_fake.mounted_partition = PARTITION_NONE;
	AiusSdEvent event = aius_sd_teensy_execute(&adapter, &drop);
	assert_driver_event(event, 47);
	assert(g_fake.call_count == 0);
	assert(g_fake.clear_error_calls == 1);
	assert(g_fake.clock_calls == 2);
	assert(adapter.driver_faults == 0);

	static const MountedPartition partitions[] = {
		PARTITION_FAT, PARTITION_EXFAT
	};
	for (size_t i = 0; i < sizeof(partitions) / sizeof(partitions[0]); ++i) {
		/* One cacheClear of the mounted partition and nothing else. */
		reset_fake();
		reset_adapter(&adapter);
		g_fake.mounted_partition = partitions[i];
		event = aius_sd_teensy_execute(&adapter, &drop);
		assert_event(event, AIUS_SD_EVENT_OK, 47, 0, 0, false, 0);
		assert(g_fake.call_count == 1);
		assert(g_fake.calls[0] == PRIMITIVE_CACHE_CLEAR);
		assert(g_fake.fat_cache_clears ==
		       (partitions[i] == PARTITION_FAT ? 1u : 0u));
		assert(g_fake.exfat_cache_clears ==
		       (partitions[i] == PARTITION_EXFAT ? 1u : 0u));
		assert(adapter.driver_faults == 0);

		/* A failed sync counts a driver fault only on a nonzero code. */
		reset_fake();
		reset_adapter(&adapter);
		g_fake.mounted_partition = partitions[i];
		g_fake.succeeds[PRIMITIVE_CACHE_CLEAR] = false;
		g_fake.fail_code = 0;
		event = aius_sd_teensy_execute(&adapter, &drop);
		assert_driver_event(event, 47);
		assert(adapter.driver_faults == 0);
		reset_fake();
		g_fake.mounted_partition = partitions[i];
		g_fake.succeeds[PRIMITIVE_CACHE_CLEAR] = false;
		event = aius_sd_teensy_execute(&adapter, &drop);
		assert_driver_event(event, 47);
		assert(adapter.driver_faults == 1);
	}
}

} // namespace

void pinMode(uint8_t pin, uint8_t mode)
{
	g_fake.pin_mode_calls++;
	g_fake.pin = pin;
	g_fake.pin_mode = mode;
	g_fake.pin_mode_at = ++g_fake.sequence;
}

void delayMicroseconds(uint32_t usec)
{
	g_fake.delay_calls++;
	g_fake.delay_us = usec;
	g_fake.delay_at = ++g_fake.sequence;
}

uint8_t *FatPartition::cacheClear()
{
	record(PRIMITIVE_CACHE_CLEAR);
	g_fake.fat_cache_clears++;
	return outcome(g_fake.succeeds[PRIMITIVE_CACHE_CLEAR]) ? g_buffer
	                                                     : nullptr;
}

uint8_t *ExFatPartition::cacheClear()
{
	record(PRIMITIVE_CACHE_CLEAR);
	g_fake.exfat_cache_clears++;
	return outcome(g_fake.succeeds[PRIMITIVE_CACHE_CLEAR]) ? g_buffer
	                                                     : nullptr;
}

SDClass SD;

File::File() : valid_(false) {}

File::File(bool valid) : valid_(valid) {}

File::operator bool() const
{
	return valid_;
}

bool File::truncate(uint64_t size)
{
	record(PRIMITIVE_TRUNCATE);
	g_fake.offset = size;
	return outcome(g_fake.succeeds[PRIMITIVE_TRUNCATE]);
}

bool File::seek(uint64_t position)
{
	record(PRIMITIVE_SEEK);
	g_fake.offset = position;
	return outcome(g_fake.succeeds[PRIMITIVE_SEEK]);
}

size_t File::read(uint8_t *buffer, size_t length)
{
	record(PRIMITIVE_READ);
	g_fake.mutable_buffer = buffer;
	g_fake.length = length;
	return g_fake.read_result;
}

size_t File::write(const uint8_t *buffer, size_t length)
{
	record(PRIMITIVE_WRITE);
	g_fake.const_buffer = buffer;
	g_fake.length = length;
	return g_fake.write_result;
}

uint64_t File::size()
{
	record(PRIMITIVE_SIZE);
	return g_fake.size_result;
}

void File::close()
{
	record(PRIMITIVE_CLOSE);
	valid_ = false;
}

bool SDClass::begin(uint8_t chip_select, bool dma)
{
	record(PRIMITIVE_BEGIN);
	g_fake.chip_select = chip_select;
	g_fake.begin_dma = dma;
	return outcome(g_fake.succeeds[PRIMITIVE_BEGIN]);
}

void SDClass::clearError()
{
	g_fake.clear_error_calls++;
	g_fake.error_code = 0;
}

uint8_t SDClass::sdErrorCode()
{
	return g_fake.error_code;
}

bool SDClass::cardDetected()
{
	g_fake.card_detected_reads++;
	g_fake.card_detected_at = ++g_fake.sequence;
	return g_fake.card_detected;
}

uint32_t SDClass::busyTimeouts()
{
	return g_fake.busy_timeouts;
}

bool SDClass::mediaPresent()
{
	record(PRIMITIVE_MEDIA_PRESENT);
	return g_fake.media_present;
}

bool SDClass::mkdir(const char *path)
{
	record(PRIMITIVE_MKDIR);
	g_fake.path = path;
	return outcome(g_fake.succeeds[PRIMITIVE_MKDIR]);
}

File SDClass::open(const char *path, uint8_t mode)
{
	record(PRIMITIVE_OPEN);
	g_fake.path = path;
	g_fake.open_mode = mode;
	outcome(g_fake.open_valid);
	return File(g_fake.open_valid);
}

bool SDClass::exists(const char *path)
{
	record(PRIMITIVE_EXISTS);
	g_fake.path = path;
	return g_fake.exists;
}

bool SDClass::rename(const char *path, const char *path_to)
{
	record(PRIMITIVE_RENAME);
	g_fake.path = path;
	g_fake.path_to = path_to;
	return outcome(g_fake.succeeds[PRIMITIVE_RENAME]);
}

bool SDClass::remove(const char *path)
{
	record(PRIMITIVE_REMOVE);
	g_fake.path = path;
	return outcome(g_fake.succeeds[PRIMITIVE_REMOVE]);
}

uint64_t SDClass::totalSize()
{
	record(PRIMITIVE_TOTAL_SIZE);
	return g_fake.total_size;
}

uint64_t SDClass::usedSize()
{
	/* The unbounded walk the chunked scan replaced: never reached. */
	assert(0);
	return g_fake.used_size;
}

/* The free-scan unit is not compiled here (its real FsVolume is SdFat's, this
 * fake's is its own); a programmable step stands in. It is the one primitive
 * USED_SIZE spends. */
AiusSdFreeScanResult aius_sd_free_scan_step(
	FsVolume *, uint64_t cursor, uint64_t free_so_far, bool mounted)
{
	record(PRIMITIVE_USED_SIZE);
	g_scan_cursor = cursor;
	g_scan_so_far = free_so_far;
	g_scan_mounted = mounted;
	return g_scan_result;
}

/* Deliberately not recorded as a primitive: the real fatType() reads a member
 * the mount already filled. Counting the reads separately is what lets the
 * volume-type test assert the probe still spends exactly one card primitive. */
uint8_t FsVolume::fatType() const
{
	g_fake.fat_type_reads++;
	return g_fake.fat_type;
}

/* As the vendor forwarder: the mounted partition's cacheClear(), else
 * nullptr with no card transaction. */
uint8_t *FsVolume::cacheClear()
{
	static FatPartition fat;
	static ExFatPartition exfat;
	switch (g_fake.mounted_partition) {
	case PARTITION_FAT:
		return fat.cacheClear();
	case PARTITION_EXFAT:
		return exfat.cacheClear();
	case PARTITION_NONE:
		break;
	}
	return nullptr;
}

FsVolume *SdFs::vol()
{
	return &volume_;
}

uint32_t micros()
{
	size_t index = g_fake.clock_calls++;
	assert(index < 4);
	return g_fake.clocks[index % 2];
}

int main()
{
	test_init_and_fixed_catalog();
	test_storage_failures_and_authority();
	test_read_write_result_boundaries();
	test_boolean_and_integer_boundaries();
	test_exact_payload_validation();
	test_used_size_cursor_payload();
	test_inert_null_unknown_and_duplicate();
	test_media_present_reports_the_volume_type();
	test_durations_and_counter_saturation();
	test_diagnostic_counters();
	test_mount_is_cached_and_revalidated();
	test_fault_split_and_absence();
	test_busy_timeouts_surface();
	test_mode_and_remount();
	test_truncate_carries_offset();
	test_card_detected_and_dat3();
	test_driver_fault_then_card_presence();
	test_cache_drop();
	return 0;
}
