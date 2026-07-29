#include "aius_boot_teensy.h"
#include "Arduino.h"

#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <limits>
#include <vector>

volatile uint32_t SRC_SRSR;
volatile uint32_t CCM_CCGR3;
volatile uint16_t WDOG1_WMCR;
volatile uint16_t WDOG1_WCR;
volatile uint16_t WDOG1_WSR;

namespace {

struct CacheCall {
	bool flush;
	void *address;
	uint32_t length;
};

std::vector<CacheCall> g_cache_calls;
size_t g_dsb_calls;

enum class CallKind {
	Read,
	Write,
	Flush,
	Barrier,
	ReadReset,
	ClearReset,
	EnableClock,
	WriteWmcr,
	WriteWcr,
	WriteWsr
};

struct Call {
	CallKind kind;
	uint8_t slot;
	size_t offset;
	size_t length;
	uint32_t value;
};

struct Fake {
	alignas(32) AiusBootSlot slots[2];
	std::vector<Call> calls;
	bool read_ok[2];
	size_t fail_write_call;
	size_t fail_write_prefix;
	size_t throw_after_call;
	size_t write_calls;
	uint32_t reset_source;
	jmp_buf reset;
	bool reset_armed;
};

Fake g_fake;

void reset_fake()
{
	memset(g_fake.slots, 0, sizeof(g_fake.slots));
	g_fake.calls.clear();
	g_fake.read_ok[0] = true;
	g_fake.read_ok[1] = true;
	g_fake.fail_write_call = std::numeric_limits<size_t>::max();
	g_fake.fail_write_prefix = std::numeric_limits<size_t>::max();
	g_fake.throw_after_call = std::numeric_limits<size_t>::max();
	g_fake.write_calls = 0;
	g_fake.reset_source = 0;
	g_fake.reset_armed = false;
}

void record(CallKind kind, uint8_t slot = 0, size_t offset = 0,
	    size_t length = 0, uint32_t value = 0)
{
	g_fake.calls.push_back({kind, slot, offset, length, value});
	if (g_fake.reset_armed &&
	    g_fake.calls.size() == g_fake.throw_after_call)
		longjmp(g_fake.reset, 1);
}

bool fake_read_slot(void *context, uint8_t slot, AiusBootSlot *out)
{
	auto *fake = static_cast<Fake *>(context);
	assert(fake == &g_fake);
	assert(slot < 2);
	assert(out != nullptr);
	*out = fake->slots[slot];
	record(CallKind::Read, slot);
	return fake->read_ok[slot];
}

bool fake_write_slot_bytes(void *context, uint8_t slot, size_t offset,
			   const uint8_t *data, size_t length)
{
	auto *fake = static_cast<Fake *>(context);
	assert(fake == &g_fake);
	assert(slot < 2);
	assert(data != nullptr);
	assert(offset <= sizeof(AiusBootSlot));
	assert(length <= sizeof(AiusBootSlot) - offset);

	const size_t write_index = fake->write_calls++;
	size_t copied = length;
	bool ok = true;
	if (write_index == fake->fail_write_call) {
		copied = fake->fail_write_prefix;
		assert(copied < length);
		ok = false;
	}
	memcpy(reinterpret_cast<uint8_t *>(&fake->slots[slot]) + offset,
	       data, copied);
	record(CallKind::Write, slot, offset, length);
	return ok;
}

void fake_flush(void *context, uint8_t slot, size_t offset, size_t length)
{
	assert(context == &g_fake);
	record(CallKind::Flush, slot, offset, length);
}

void fake_barrier(void *context)
{
	assert(context == &g_fake);
	record(CallKind::Barrier);
}

uint32_t fake_read_reset(void *context)
{
	assert(context == &g_fake);
	const uint32_t value = g_fake.reset_source;
	record(CallKind::ReadReset, 0, 0, 0, value);
	return value;
}

void fake_clear_reset(void *context, uint32_t captured)
{
	assert(context == &g_fake);
	record(CallKind::ClearReset, 0, 0, 0, captured);
}

void fake_enable_clock(void *context)
{
	assert(context == &g_fake);
	record(CallKind::EnableClock);
}

void fake_write_wmcr(void *context, uint16_t value)
{
	assert(context == &g_fake);
	record(CallKind::WriteWmcr, 0, 0, 0, value);
}

void fake_write_wcr(void *context, uint16_t value)
{
	assert(context == &g_fake);
	record(CallKind::WriteWcr, 0, 0, 0, value);
}

void fake_write_wsr(void *context, uint16_t value)
{
	assert(context == &g_fake);
	record(CallKind::WriteWsr, 0, 0, 0, value);
}

AiusBootTeensyPrimitives complete_primitives()
{
	AiusBootTeensyPrimitives primitives = {};
	primitives.context = &g_fake;
	primitives.read_slot = fake_read_slot;
	primitives.write_slot_bytes = fake_write_slot_bytes;
	primitives.flush_slot_range = fake_flush;
	primitives.barrier = fake_barrier;
	primitives.read_reset_source = fake_read_reset;
	primitives.clear_reset_source = fake_clear_reset;
	primitives.enable_watchdog_clock = fake_enable_clock;
	primitives.write_watchdog_wmcr = fake_write_wmcr;
	primitives.write_watchdog_wcr = fake_write_wcr;
	primitives.write_watchdog_wsr = fake_write_wsr;
	return primitives;
}

uint32_t crc32(const uint8_t *bytes, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	for (size_t i = 0; i < length; ++i) {
		crc ^= bytes[i];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^
			      (UINT32_C(0xedb88320) &
			       static_cast<uint32_t>(0u - (crc & 1u)));
	}
	return ~crc;
}

void put_le32(uint8_t *out, uint32_t value)
{
	out[0] = static_cast<uint8_t>(value);
	out[1] = static_cast<uint8_t>(value >> 8);
	out[2] = static_cast<uint8_t>(value >> 16);
	out[3] = static_cast<uint8_t>(value >> 24);
}

AiusBootLedger make_ledger(uint32_t build_id, uint32_t generation)
{
	AiusBootLedger ledger = {};
	AiusBootInput input = {};
	input.build_id = build_id;
	input.raw_reset_source = UINT32_C(0x12345678);
	input.reset_kind = AIUS_RESET_COLD;
	aius_boot_init(&ledger, nullptr, input);
	for (uint32_t i = 1; i < generation; ++i) {
		AiusBootLedger retained = ledger;
		aius_boot_init(&ledger, &retained, input);
	}
	assert(aius_boot_is_valid(&ledger));
	return ledger;
}

AiusBootSlot make_slot(uint32_t sequence, const AiusBootLedger &ledger)
{
	AiusBootSlot slot = {};
	slot.committed = AIUS_BOOT_SLOT_COMMITTED;
	slot.sequence = sequence;
	slot.ledger = ledger;
	memset(slot.padding, 0, sizeof(slot.padding));
	uint8_t covered[32];
	put_le32(covered, sequence);
	memcpy(covered + 4, &ledger, sizeof(ledger));
	slot.slot_crc32 = crc32(covered, sizeof(covered));
	return slot;
}

void refresh_ledger_crc(AiusBootLedger *ledger)
{
	uint8_t covered[24];
	put_le32(covered, ledger->magic);
	covered[4] = static_cast<uint8_t>(ledger->version);
	covered[5] = static_cast<uint8_t>(ledger->version >> 8);
	covered[6] = static_cast<uint8_t>(ledger->last_checkpoint);
	covered[7] = static_cast<uint8_t>(ledger->last_checkpoint >> 8);
	put_le32(covered + 8, ledger->build_id);
	put_le32(covered + 12, ledger->boot_generation);
	put_le32(covered + 16, ledger->reset_source);
	covered[20] = ledger->early_watchdog_resets;
	covered[21] = ledger->mode;
	covered[22] = static_cast<uint8_t>(ledger->reserved);
	covered[23] = static_cast<uint8_t>(ledger->reserved >> 8);
	ledger->crc32 = crc32(covered, sizeof(covered));
}

void assert_zero_ledger(const AiusBootLedger &ledger)
{
	AiusBootLedger zero = {};
	assert(memcmp(&ledger, &zero, sizeof(ledger)) == 0);
}

void assert_invalid_storage(const AiusBootStorageTeensy &storage)
{
	assert(!storage.valid);
	assert(storage.active_slot == UINT8_MAX);
	assert(storage.sequence == 0);
}

void test_layout()
{
	static_assert(sizeof(AiusBootSlot) == 64, "slot");
	static_assert(alignof(AiusBootSlot) == 32, "slot alignment");
	assert(reinterpret_cast<uintptr_t>(&g_aius_boot_slots[0]) % 32 == 0);
	assert(reinterpret_cast<uintptr_t>(&g_aius_boot_slots[1]) % 32 == 0);
	assert(reinterpret_cast<uint8_t *>(&g_aius_boot_slots[1]) -
	       reinterpret_cast<uint8_t *>(&g_aius_boot_slots[0]) == 64);
	assert(sizeof(g_aius_boot_slots) == 128);
}

void test_init_rejects_null_and_every_incomplete_table()
{
	reset_fake();
	AiusBootStorageTeensy storage = {};
	storage.active_slot = 1;
	storage.sequence = 99;
	storage.valid = true;
	aius_boot_storage_teensy_init(nullptr, nullptr);
	aius_boot_storage_teensy_init(&storage, nullptr);
	assert_invalid_storage(storage);

	const AiusBootTeensyPrimitives complete = complete_primitives();
	using Member = void (*)(AiusBootTeensyPrimitives &);
	const Member clearers[] = {
		[](AiusBootTeensyPrimitives &p) { p.read_slot = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.write_slot_bytes = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.flush_slot_range = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.barrier = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.read_reset_source = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.clear_reset_source = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.enable_watchdog_clock = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.write_watchdog_wmcr = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.write_watchdog_wcr = nullptr; },
		[](AiusBootTeensyPrimitives &p) { p.write_watchdog_wsr = nullptr; },
	};
	for (const Member clear : clearers) {
		AiusBootTeensyPrimitives incomplete = complete;
		clear(incomplete);
		memset(&storage, 0xa5, sizeof(storage));
		aius_boot_storage_teensy_init(&storage, &incomplete);
		assert_invalid_storage(storage);
	}

	AiusBootTeensyPrimitives copied = complete;
	aius_boot_storage_teensy_init(&storage, &copied);
	assert(storage.valid);
	assert(storage.active_slot == UINT8_MAX);
	assert(storage.sequence == 0);
	memset(&copied, 0, sizeof(copied));
	AiusBootLedger out = make_ledger(1, 1);
	assert(!aius_boot_storage_teensy_load(&storage, &out));
	assert_zero_ledger(out);
	assert(g_fake.calls.size() == 2);
}

void init_valid(AiusBootStorageTeensy *storage)
{
	AiusBootTeensyPrimitives primitives = complete_primitives();
	aius_boot_storage_teensy_init(storage, &primitives);
	assert(storage->valid);
}

void test_load_validation_and_zeroing()
{
	reset_fake();
	AiusBootStorageTeensy storage;
	init_valid(&storage);
	AiusBootLedger out = make_ledger(1, 1);

	assert(!aius_boot_storage_teensy_load(nullptr, &out));
	assert_zero_ledger(out);
	assert(!aius_boot_storage_teensy_load(&storage, nullptr));

	AiusBootStorageTeensy invalid = storage;
	invalid.valid = false;
	assert(!aius_boot_storage_teensy_load(&invalid, &out));
	assert_zero_ledger(out);

	out = make_ledger(2, 1);
	assert(!aius_boot_storage_teensy_load(&storage, &out));
	assert_zero_ledger(out);
	assert(storage.active_slot == UINT8_MAX);
	assert(storage.sequence == 0);

	const AiusBootLedger ledger0 = make_ledger(10, 2);
	const AiusBootLedger ledger1 = make_ledger(11, 3);
	g_fake.slots[0] = make_slot(4, ledger0);
	g_fake.slots[1] = make_slot(5, ledger1);
	assert(aius_boot_storage_teensy_load(&storage, &out));
	assert(memcmp(&out, &ledger1, sizeof(out)) == 0);
	assert(storage.active_slot == 1);
	assert(storage.sequence == 5);

	g_fake.slots[1].committed ^= 1;
	assert(aius_boot_storage_teensy_load(&storage, &out));
	assert(memcmp(&out, &ledger0, sizeof(out)) == 0);
	assert(storage.active_slot == 0);

	g_fake.slots[0].slot_crc32 ^= 1;
	assert(!aius_boot_storage_teensy_load(&storage, &out));
	assert_zero_ledger(out);

	g_fake.slots[0] = make_slot(7, ledger0);
	g_fake.slots[0].ledger.crc32 ^= 1;
	g_fake.slots[0].slot_crc32 =
		make_slot(7, g_fake.slots[0].ledger).slot_crc32;
	assert(!aius_boot_storage_teensy_load(&storage, &out));
	assert_zero_ledger(out);

	g_fake.read_ok[0] = false;
	g_fake.slots[1] = make_slot(9, ledger1);
	assert(aius_boot_storage_teensy_load(&storage, &out));
	assert(memcmp(&out, &ledger1, sizeof(out)) == 0);
	g_fake.read_ok[1] = false;
	assert(!aius_boot_storage_teensy_load(&storage, &out));
	assert_zero_ledger(out);
}

void test_every_invalid_ledger_and_slot_crc_byte()
{
	const AiusBootLedger valid = make_ledger(15, 2);
	std::array<AiusBootLedger, 6> invalid = {
		valid, valid, valid, valid, valid, valid
	};
	invalid[0].magic ^= 1;
	refresh_ledger_crc(&invalid[0]);
	invalid[1].version = 2;
	refresh_ledger_crc(&invalid[1]);
	invalid[2].last_checkpoint =
		AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE + 1u;
	refresh_ledger_crc(&invalid[2]);
	invalid[3].mode = 2;
	refresh_ledger_crc(&invalid[3]);
	invalid[4].reserved = 1;
	refresh_ledger_crc(&invalid[4]);
	invalid[5].crc32 ^= 1;

	for (const AiusBootLedger &ledger : invalid) {
		reset_fake();
		g_fake.slots[0] = make_slot(3, ledger);
		AiusBootStorageTeensy storage;
		init_valid(&storage);
		AiusBootLedger out = valid;
		assert(!aius_boot_storage_teensy_load(&storage, &out));
		assert_zero_ledger(out);
		assert(!aius_boot_storage_teensy_commit(&storage, &ledger));
	}

	for (size_t byte = 0; byte < 32; ++byte) {
		reset_fake();
		g_fake.slots[0] = make_slot(3, valid);
		uint8_t *covered =
			reinterpret_cast<uint8_t *>(&g_fake.slots[0].sequence);
		covered[byte] ^= UINT8_C(0x80);
		AiusBootStorageTeensy storage;
		init_valid(&storage);
		AiusBootLedger out;
		assert(!aius_boot_storage_teensy_load(&storage, &out));
		assert_zero_ledger(out);
	}

	reset_fake();
	g_fake.slots[0] = make_slot(3, valid);
	g_fake.slots[0].padding[17] = UINT8_C(0xa5);
	AiusBootStorageTeensy storage;
	init_valid(&storage);
	AiusBootLedger out;
	assert(aius_boot_storage_teensy_load(&storage, &out));
	assert(memcmp(&out, &valid, sizeof(out)) == 0);
}

void assert_selected(uint32_t left, uint32_t right, uint8_t expected)
{
	reset_fake();
	g_fake.slots[0] = make_slot(left, make_ledger(20, 1));
	g_fake.slots[1] = make_slot(right, make_ledger(21, 1));
	AiusBootStorageTeensy storage;
	init_valid(&storage);
	AiusBootLedger out;
	assert(aius_boot_storage_teensy_load(&storage, &out));
	assert(storage.active_slot == expected);
	assert(storage.sequence == (expected == 0 ? left : right));
	assert(out.build_id == (expected == 0 ? 20u : 21u));
}

void test_sequence_order()
{
	assert_selected(7, 7, 0);
	assert_selected(0, UINT32_MAX, 0);
	assert_selected(UINT32_MAX, 0, 1);
	assert_selected(0, UINT32_C(0x80000000), 0);
	assert_selected(UINT32_C(0x80000000), 0, 0);
	assert_selected(UINT32_C(0xfffffffe), UINT32_C(1), 1);
	assert_selected(UINT32_C(1), UINT32_C(0xfffffffe), 0);
}

void assert_call(size_t index, CallKind kind, uint8_t slot, size_t offset,
		 size_t length)
{
	assert(index < g_fake.calls.size());
	const Call &call = g_fake.calls[index];
	assert(call.kind == kind);
	assert(call.slot == slot);
	assert(call.offset == offset);
	assert(call.length == length);
}

void test_commit_contract()
{
	reset_fake();
	AiusBootStorageTeensy storage;
	init_valid(&storage);
	const AiusBootLedger ledger = make_ledger(31, 4);

	assert(!aius_boot_storage_teensy_commit(nullptr, &ledger));
	assert(!aius_boot_storage_teensy_commit(&storage, nullptr));
	AiusBootStorageTeensy invalid = storage;
	invalid.valid = false;
	assert(!aius_boot_storage_teensy_commit(&invalid, &ledger));
	AiusBootLedger corrupt = ledger;
	corrupt.crc32 ^= 1;
	assert(!aius_boot_storage_teensy_commit(&storage, &corrupt));
	assert(g_fake.calls.empty());

	assert(aius_boot_storage_teensy_commit(&storage, &ledger));
	assert(storage.active_slot == 0);
	assert(storage.sequence == 1);
	assert(g_fake.calls.size() == 10);
	assert_call(0, CallKind::Write, 0, 0, 4);
	assert_call(1, CallKind::Write, 0, 4, 4);
	assert_call(2, CallKind::Write, 0, 8, 28);
	assert_call(3, CallKind::Write, 0, 40, 24);
	assert_call(4, CallKind::Write, 0, 36, 4);
	assert_call(5, CallKind::Flush, 0, 0, 64);
	assert_call(6, CallKind::Barrier, 0, 0, 0);
	assert_call(7, CallKind::Write, 0, 0, 4);
	assert_call(8, CallKind::Flush, 0, 0, 32);
	assert_call(9, CallKind::Barrier, 0, 0, 0);
	assert(g_fake.slots[0].committed == AIUS_BOOT_SLOT_COMMITTED);
	assert(g_fake.slots[0].sequence == 1);
	assert(memcmp(&g_fake.slots[0].ledger, &ledger, sizeof(ledger)) == 0);
	for (uint8_t byte : g_fake.slots[0].padding)
		assert(byte == 0);
	assert(g_fake.slots[0].slot_crc32 ==
	       make_slot(1, ledger).slot_crc32);

	reset_fake();
	const AiusBootLedger old = make_ledger(40, 1);
	g_fake.slots[1] = make_slot(UINT32_MAX, old);
	init_valid(&storage);
	AiusBootLedger loaded;
	assert(aius_boot_storage_teensy_load(&storage, &loaded));
	g_fake.calls.clear();
	g_fake.write_calls = 0;
	assert(aius_boot_storage_teensy_commit(&storage, &ledger));
	assert(storage.active_slot == 0);
	assert(storage.sequence == 0);
	assert(g_fake.slots[0].sequence == 0);
}

void load_after_interruption(const AiusBootLedger &old,
			     const AiusBootLedger &replacement,
			     bool replacement_published)
{
	g_fake.calls.clear();
	g_fake.read_ok[0] = true;
	g_fake.read_ok[1] = true;
	AiusBootStorageTeensy rebooted;
	init_valid(&rebooted);
	AiusBootLedger loaded;
	assert(aius_boot_storage_teensy_load(&rebooted, &loaded));
	const AiusBootLedger &expected =
		replacement_published ? replacement : old;
	assert(memcmp(&loaded, &expected, sizeof(loaded)) == 0);
}

void test_every_torn_write_prefix()
{
	const size_t lengths[] = {4, 4, 28, 24, 4, 4};
	const AiusBootLedger old = make_ledger(50, 2);
	const AiusBootLedger replacement = make_ledger(51, 3);
	for (size_t write = 0; write < 6; ++write) {
		for (size_t prefix = 0; prefix < lengths[write]; ++prefix) {
			reset_fake();
			g_fake.slots[0] = make_slot(9, old);
			AiusBootStorageTeensy storage;
			init_valid(&storage);
			AiusBootLedger loaded;
			assert(aius_boot_storage_teensy_load(&storage, &loaded));
			g_fake.calls.clear();
			g_fake.write_calls = 0;
			g_fake.fail_write_call = write;
			g_fake.fail_write_prefix = prefix;
			assert(!aius_boot_storage_teensy_commit(
				&storage, &replacement));
			assert(storage.active_slot == 0);
			assert(storage.sequence == 9);
			load_after_interruption(old, replacement, false);
		}
	}
}

void test_reset_after_every_commit_call()
{
	const AiusBootLedger old = make_ledger(60, 2);
	const AiusBootLedger replacement = make_ledger(61, 3);
	for (size_t boundary = 1; boundary <= 10; ++boundary) {
		reset_fake();
		g_fake.slots[0] = make_slot(100, old);
		AiusBootStorageTeensy storage;
		init_valid(&storage);
		AiusBootLedger loaded;
		assert(aius_boot_storage_teensy_load(&storage, &loaded));
		g_fake.calls.clear();
		g_fake.write_calls = 0;
		g_fake.throw_after_call = boundary;
		g_fake.reset_armed = true;
		if (setjmp(g_fake.reset) == 0)
			(void)aius_boot_storage_teensy_commit(
				&storage, &replacement);
		g_fake.reset_armed = false;
		load_after_interruption(old, replacement, boundary >= 8);
	}
}

AiusResetKind expected_reset_kind(uint32_t raw)
{
	if ((raw & SRC_SRSR_WDOG_RST_B) != 0)
		return AIUS_RESET_WATCHDOG;
	if ((raw & (SRC_SRSR_IPP_RESET_B | SRC_SRSR_CSU_RESET_B)) != 0)
		return AIUS_RESET_COLD;
	if ((raw & (SRC_SRSR_JTAG_SW_RST |
		    SRC_SRSR_LOCKUP_SYSRESETREQ)) != 0)
		return AIUS_RESET_SOFTWARE;
	return AIUS_RESET_OTHER;
}

void test_reset_classification_exhaustive()
{
	for (uint32_t raw = 0; raw < 512; ++raw)
		assert(aius_boot_teensy_reset_kind(raw) ==
		       expected_reset_kind(raw));
	const uint32_t high[] = {
		UINT32_C(0xffffffff), UINT32_C(0xfffffe00),
		UINT32_C(0x80000000), UINT32_C(0x40000100)
	};
	for (uint32_t raw : high)
		assert(aius_boot_teensy_reset_kind(raw) ==
		       expected_reset_kind(raw));
}

void test_reset_capture_full_raw_w1c()
{
	const uint32_t values[] = {
		0, 1, 511, UINT32_C(0x80000000), UINT32_C(0xdeadbeef),
		UINT32_MAX
	};
	for (uint32_t value : values) {
		reset_fake();
		g_fake.reset_source = value;
		const AiusBootTeensyPrimitives primitives =
			complete_primitives();
		assert(aius_boot_teensy_reset_source(&primitives) == value);
		assert(g_fake.calls.size() == 2);
		assert(g_fake.calls[0].kind == CallKind::ReadReset);
		assert(g_fake.calls[0].value == value);
		assert(g_fake.calls[1].kind == CallKind::ClearReset);
		assert(g_fake.calls[1].value == value);
	}

	AiusBootTeensyPrimitives incomplete = complete_primitives();
	incomplete.read_reset_source = nullptr;
	reset_fake();
	assert(aius_boot_teensy_reset_source(nullptr) == 0);
	assert(aius_boot_teensy_reset_source(&incomplete) == 0);
	incomplete = complete_primitives();
	incomplete.clear_reset_source = nullptr;
	assert(aius_boot_teensy_reset_source(&incomplete) == 0);
	assert(g_fake.calls.empty());
}

void test_watchdog_exact_traces()
{
	const uint16_t expected_wcr =
		WDOG_WCR_SRS | WDOG_WCR_WT(15) | WDOG_WCR_WDE |
		WDOG_WCR_WDT | WDOG_WCR_SRE;
	AiusBootTeensyPrimitives primitives = complete_primitives();
	reset_fake();
	assert(aius_boot_teensy_watchdog_arm(&primitives));
	assert(g_fake.calls.size() == 3);
	assert(g_fake.calls[0].kind == CallKind::EnableClock);
	assert(g_fake.calls[1].kind == CallKind::WriteWmcr);
	assert(g_fake.calls[1].value == 0);
	assert(g_fake.calls[2].kind == CallKind::WriteWcr);
	assert(g_fake.calls[2].value == expected_wcr);

	reset_fake();
	assert(aius_boot_teensy_watchdog_feed(&primitives));
	assert(g_fake.calls.size() == 2);
	assert(g_fake.calls[0].kind == CallKind::WriteWsr);
	assert(g_fake.calls[0].value == UINT16_C(0x5555));
	assert(g_fake.calls[1].kind == CallKind::WriteWsr);
	assert(g_fake.calls[1].value == UINT16_C(0xaaaa));

	assert(!aius_boot_teensy_watchdog_arm(nullptr));
	assert(!aius_boot_teensy_watchdog_feed(nullptr));
	using ClearArm = void (*)(AiusBootTeensyPrimitives &);
	const ClearArm arm_clearers[] = {
		[](AiusBootTeensyPrimitives &p) {
			p.enable_watchdog_clock = nullptr;
		},
		[](AiusBootTeensyPrimitives &p) {
			p.write_watchdog_wmcr = nullptr;
		},
		[](AiusBootTeensyPrimitives &p) {
			p.write_watchdog_wcr = nullptr;
		},
	};
	for (const ClearArm clear : arm_clearers) {
		reset_fake();
		AiusBootTeensyPrimitives incomplete = primitives;
		clear(incomplete);
		assert(!aius_boot_teensy_watchdog_arm(&incomplete));
		assert(g_fake.calls.empty());
	}
	reset_fake();
	primitives.write_watchdog_wsr = nullptr;
	assert(!aius_boot_teensy_watchdog_feed(&primitives));
	assert(g_fake.calls.empty());
}

void test_hardware_primitive_factory()
{
	const AiusBootTeensyPrimitives primitives =
		aius_boot_teensy_hardware_primitives();
	assert(primitives.read_slot != nullptr);
	assert(primitives.write_slot_bytes != nullptr);
	assert(primitives.flush_slot_range != nullptr);
	assert(primitives.barrier != nullptr);
	assert(primitives.read_reset_source != nullptr);
	assert(primitives.clear_reset_source != nullptr);
	assert(primitives.enable_watchdog_clock != nullptr);
	assert(primitives.write_watchdog_wmcr != nullptr);
	assert(primitives.write_watchdog_wcr != nullptr);
	assert(primitives.write_watchdog_wsr != nullptr);

	memset(g_aius_boot_slots, 0, sizeof(g_aius_boot_slots));
	g_cache_calls.clear();
	g_dsb_calls = 0;
	const uint8_t bytes[] = {1, 2, 3, 4, 5};
	assert(primitives.write_slot_bytes(primitives.context, 1, 7, bytes,
					   sizeof(bytes)));
	assert(memcmp(reinterpret_cast<uint8_t *>(&g_aius_boot_slots[1]) + 7,
		      bytes, sizeof(bytes)) == 0);
	primitives.flush_slot_range(primitives.context, 1, 0, 64);
	assert(g_cache_calls.size() == 1);
	assert(g_cache_calls[0].flush);
	assert(g_cache_calls[0].address == &g_aius_boot_slots[1]);
	assert(g_cache_calls[0].length == 64);
	primitives.barrier(primitives.context);
	assert(g_dsb_calls == 1);

	AiusBootSlot slot = {};
	g_cache_calls.clear();
	assert(primitives.read_slot(primitives.context, 1, &slot));
	assert(memcmp(&slot, &g_aius_boot_slots[1], sizeof(slot)) == 0);
	assert(g_cache_calls.size() == 1);
	assert(!g_cache_calls[0].flush);
	assert(g_cache_calls[0].address == &g_aius_boot_slots[1]);
	assert(g_cache_calls[0].length == sizeof(AiusBootSlot));
	assert(!primitives.read_slot(primitives.context, 2, &slot));
	assert(!primitives.read_slot(primitives.context, 0, nullptr));
	assert(!primitives.write_slot_bytes(
		primitives.context, 2, 0, bytes, sizeof(bytes)));
	assert(!primitives.write_slot_bytes(
		primitives.context, 0, 63, bytes, sizeof(bytes)));
	assert(!primitives.write_slot_bytes(
		primitives.context, 0, 0, nullptr, sizeof(bytes)));

	SRC_SRSR = UINT32_C(0xfedcba98);
	assert(aius_boot_teensy_reset_source(&primitives) ==
	       UINT32_C(0xfedcba98));
	assert(SRC_SRSR == UINT32_C(0xfedcba98));

	CCM_CCGR3 = UINT32_C(0x55aa0005);
	WDOG1_WMCR = UINT16_C(0xffff);
	WDOG1_WCR = 0;
	assert(aius_boot_teensy_watchdog_arm(&primitives));
	assert(CCM_CCGR3 ==
	       (UINT32_C(0x55aa0005) | CCM_CCGR3_WDOG1(CCM_CCGR_ON)));
	assert(WDOG1_WMCR == 0);
	assert(WDOG1_WCR ==
	       (WDOG_WCR_SRS | WDOG_WCR_WT(15) | WDOG_WCR_WDE |
		WDOG_WCR_WDT | WDOG_WCR_SRE));
	WDOG1_WSR = 0;
	assert(aius_boot_teensy_watchdog_feed(&primitives));
	assert(WDOG1_WSR == UINT16_C(0xaaaa));
}

/* The boot-configuration fuse decode, prerequisite 4 of
 * docs/plans/aius-ota-upgrade.md. The single gating fact for hardware A/B on
 * this part is whether PJRC blows 0x6E0[23:16]; the answer needs a real board,
 * but the decode that will report it must be right BEFORE the board is asked,
 * because a wrong shift silently answers the question wrongly and the answer
 * looks plausible either way.
 *
 * Both fields are exercised over fabricated words, including the neighbouring
 * bits that a one-off shift or a missing mask would pull in. */
void test_boot_fuse_decode()
{
	struct Case {
		uint32_t word;
		uint8_t offset;
		uint8_t size;
	};
	static const Case cases[] = {
		/* The shipped part: both fields zero, so the secondary image
		 * base is 0 -- the same address as the primary, which is what
		 * makes hardware A/B unavailable on an unblown board. */
		{ UINT32_C(0x00000000), 0x00u, 0x0u },
		/* One bit in each field, at its low end. */
		{ UINT32_C(0x00011000), 0x01u, 0x1u },
		/* Each field saturated with every OTHER bit clear, so a decode
		 * that forgot to mask would import the neighbours. */
		{ UINT32_C(0x00FF0000), 0xFFu, 0x0u },
		{ UINT32_C(0x0000F000), 0x00u, 0xFu },
		/* Every neighbouring bit set and both fields clear: [31:24],
		 * [11:0]. A shift that is off by four in either direction, or a
		 * mask a nibble too wide, reads nonzero here. */
		{ UINT32_C(0xFF000FFF), 0x00u, 0x0u },
		/* A realistic blown reading: offset 4 -> 4 x 256 KiB = slot B
		 * at 0x60100000, the base the plan's sizing section names. */
		{ UINT32_C(0x00042000), 0x04u, 0x2u },
		/* All ones -- the host sentinel, decoded like any other word. */
		{ UINT32_C(0xFFFFFFFF), 0xFFu, 0xFu },
	};

	for (const Case &c : cases) {
		const AiusBootFuse fuse =
			aius_boot_teensy_decode_boot_fuse(c.word);
		/* The full word survives undecoded, so a field this firmware
		 * does not know about is still recoverable from the wire. */
		assert(fuse.word == c.word);
		assert(fuse.sec_image_offset == c.offset);
		assert(fuse.sec_image_size == c.size);
		/* The decode never claims a read happened; that is the
		 * reader's to assert. */
		assert(!fuse.read);
	}
}

/* The host build has no OCOTP, and what it serves instead must be impossible to
 * mistake for an answer.
 *
 * Zero would have been the natural sentinel and would have been the worst
 * possible one: zero is the SHIPPED value of both fields, so a host build would
 * have reported exactly the reading a genuine unblown board produces, and the
 * one question this read exists to settle would have been answered "unblown" by
 * a machine that never looked. */
void test_boot_fuse_host_sentinel()
{
	const AiusBootFuse fuse = aius_boot_teensy_boot_fuse();

	assert(!fuse.read);
	assert(fuse.word == AIUS_BOOT_FUSE_UNREAD);
	assert(fuse.word != 0u);
	/* 255 x 256 KiB = 63.75 MiB of secondary-image offset on a part with
	 * 8 MiB of flash: structurally impossible, so even a client that
	 * ignores `read` cannot read it as a board answer. */
	assert(fuse.sec_image_offset == 0xFFu);
	assert(fuse.sec_image_size == 0x0Fu);
	assert(static_cast<uint32_t>(fuse.sec_image_offset) * 256u * 1024u >
	       UINT32_C(8) * 1024u * 1024u);
}

} // namespace

void arm_dcache_delete(void *address, uint32_t size)
{
	g_cache_calls.push_back({false, address, size});
}

void arm_dcache_flush(void *address, uint32_t size)
{
	g_cache_calls.push_back({true, address, size});
}

void aius_test_arm_dsb(void)
{
	++g_dsb_calls;
}

int main()
{
	test_layout();
	test_init_rejects_null_and_every_incomplete_table();
	test_load_validation_and_zeroing();
	test_every_invalid_ledger_and_slot_crc_byte();
	test_sequence_order();
	test_commit_contract();
	test_every_torn_write_prefix();
	test_reset_after_every_commit_call();
	test_reset_classification_exhaustive();
	test_reset_capture_full_raw_w1c();
	test_watchdog_exact_traces();
	test_hardware_primitive_factory();
	test_boot_fuse_decode();
	test_boot_fuse_host_sentinel();
	return 0;
}
