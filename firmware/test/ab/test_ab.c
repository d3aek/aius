/* Host suite for lib/aius_ab: the A/B flash map arithmetic, the boot-record
 * and trial-ledger codecs, the ping-pong pick, the chooser's decision rule
 * and the slot IVT sanity rule (aius-ota-resilience.md Phase 3).
 *
 * The load-bearing scenarios simulate whole boot SEQUENCES: each simulated
 * boot decodes the retained bytes the previous one wrote, exactly as the
 * chooser does across real WDOG resets, so the trial budget is proven as a
 * property of the sequence rather than of one call. */

#include "aius_ab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) check((condition), #condition, __LINE__)

static const char *current_key = "startup";

static void check(int condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "ab oracle mismatch [%s] line %d: %s\n", current_key,
	        line, expression);
	exit(1);
}

static AiusAbRecord committed(uint32_t sequence, uint8_t active)
{
	AiusAbRecord record;

	memset(&record, 0, sizeof(record));
	record.sequence = sequence;
	record.active_slot = active;
	record.pending_slot = AIUS_AB_SLOT_NONE;
	record.trial_budget = 0u;
	return record;
}

static AiusAbRecord trial(uint32_t sequence, uint8_t active, uint8_t pending,
	                  uint8_t budget)
{
	AiusAbRecord record;

	memset(&record, 0, sizeof(record));
	record.sequence = sequence;
	record.active_slot = active;
	record.pending_slot = pending;
	record.trial_budget = budget;
	return record;
}

static void test_layout_arithmetic(void)
{
	current_key = "layout";
	CHECK(AIUS_AB_SLOT_A_BASE == 0x60010000u);
	CHECK(AIUS_AB_SLOT_B_BASE == 0x603E8000u);
	CHECK(AIUS_AB_SLOT_BYTES == 0x3D8000u);
	CHECK(AIUS_AB_SLOT_B_BASE + AIUS_AB_SLOT_BYTES == 0x607C0000u);
	/* The canonical link window is exactly slot A's window: one binary links
	 * here and the chooser remaps it onto whichever physical slot runs. */
	CHECK(AIUS_AB_CANONICAL_BASE == AIUS_AB_SLOT_A_BASE);
	CHECK(AIUS_AB_CANONICAL_BYTES == AIUS_AB_SLOT_BYTES);
	CHECK(AIUS_AB_RECORD_SECTOR_0 == 0x6000E000u);
	CHECK(AIUS_AB_RECORD_SECTOR_1 == 0x6000F000u);
	CHECK(aius_ab_slot_base(AIUS_AB_SLOT_A) == AIUS_AB_SLOT_A_BASE);
	CHECK(aius_ab_slot_base(AIUS_AB_SLOT_B) == AIUS_AB_SLOT_B_BASE);
	CHECK(aius_ab_slot_base(AIUS_AB_SLOT_NONE) == 0u);
	CHECK(aius_ab_other_slot(AIUS_AB_SLOT_A) == AIUS_AB_SLOT_B);
	CHECK(aius_ab_other_slot(AIUS_AB_SLOT_B) == AIUS_AB_SLOT_A);
	CHECK(aius_ab_other_slot(AIUS_AB_SLOT_NONE) == AIUS_AB_SLOT_NONE);
}

static void test_record_codec_fail_closed(void)
{
	AiusAbRecord record;
	AiusAbRecord decoded;
	uint8_t bytes[AIUS_AB_RECORD_BYTES];
	uint8_t mutated[AIUS_AB_RECORD_BYTES];
	unsigned index;

	current_key = "record.roundtrip";
	record = trial(7u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 2u);
	CHECK(aius_ab_record_encode(&record, bytes, sizeof(bytes)));
	CHECK(aius_ab_record_decode(&decoded, bytes, sizeof(bytes)));
	CHECK(decoded.sequence == 7u);
	CHECK(decoded.active_slot == AIUS_AB_SLOT_A);
	CHECK(decoded.pending_slot == AIUS_AB_SLOT_B);
	CHECK(decoded.trial_budget == 2u);

	current_key = "record.arguments";
	CHECK(!aius_ab_record_encode(NULL, bytes, sizeof(bytes)));
	CHECK(!aius_ab_record_encode(&record, NULL, sizeof(bytes)));
	CHECK(!aius_ab_record_encode(&record, bytes, sizeof(bytes) - 1u));
	CHECK(!aius_ab_record_decode(NULL, bytes, sizeof(bytes)));
	CHECK(!aius_ab_record_decode(&decoded, NULL, sizeof(bytes)));
	CHECK(!aius_ab_record_decode(&decoded, bytes, sizeof(bytes) + 1u));

	current_key = "record.fail-closed";
	/* Any single-bit corruption refuses, and refusal zeroes the struct. */
	for (index = 0u; index < AIUS_AB_RECORD_BYTES; index++) {
		memcpy(mutated, bytes, sizeof(mutated));
		mutated[index] ^= 0x01u;
		CHECK(!aius_ab_record_decode(&decoded, mutated,
		                             sizeof(mutated)));
		CHECK(decoded.active_slot == AIUS_AB_SLOT_NONE);
	}
	/* An erased sector -- all 0xFF, the post-migration state -- refuses. */
	memset(mutated, 0xff, sizeof(mutated));
	CHECK(!aius_ab_record_decode(&decoded, mutated, sizeof(mutated)));

	current_key = "record.domains";
	/* Domain rules the CRC alone cannot express: re-encoded records with
	 * illegal fields must refuse at decode. */
	record = committed(1u, AIUS_AB_SLOT_A);
	record.trial_budget = 1u; /* budget without a trial */
	CHECK(aius_ab_record_encode(&record, bytes, sizeof(bytes)));
	CHECK(!aius_ab_record_decode(&decoded, bytes, sizeof(bytes)));
	record = trial(1u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_A, 2u); /* pending==active */
	CHECK(aius_ab_record_encode(&record, bytes, sizeof(bytes)));
	CHECK(!aius_ab_record_decode(&decoded, bytes, sizeof(bytes)));
	record = trial(1u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 0u); /* trial, no boots */
	CHECK(aius_ab_record_encode(&record, bytes, sizeof(bytes)));
	CHECK(!aius_ab_record_decode(&decoded, bytes, sizeof(bytes)));
	record = trial(1u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B,
	               AIUS_AB_MAX_TRIAL_BOOTS + 1u);
	CHECK(aius_ab_record_encode(&record, bytes, sizeof(bytes)));
	CHECK(!aius_ab_record_decode(&decoded, bytes, sizeof(bytes)));
}

static void test_ledger_codec_fail_closed(void)
{
	AiusAbLedger ledger;
	AiusAbLedger decoded;
	uint8_t bytes[AIUS_AB_LEDGER_BYTES];
	uint8_t mutated[AIUS_AB_LEDGER_BYTES];
	unsigned index;

	current_key = "ledger.roundtrip";
	ledger.record_sequence = 9u;
	ledger.boots_attempted = 2u;
	ledger.fell_back = 1u;
	ledger.booted_slot = AIUS_AB_SLOT_B;
	CHECK(aius_ab_ledger_encode(&ledger, bytes, sizeof(bytes)));
	CHECK(aius_ab_ledger_decode(&decoded, bytes, sizeof(bytes)));
	CHECK(decoded.record_sequence == 9u);
	CHECK(decoded.boots_attempted == 2u);
	CHECK(decoded.fell_back == 1u);
	CHECK(decoded.booted_slot == AIUS_AB_SLOT_B);

	current_key = "ledger.legacy-booted-slot";
	/* A pre-slot-agnostic ledger has byte 16 = 0: it decodes (valid CRC over
	 * the whole record) with booted_slot NONE -- the app then falls back to
	 * the record's active slot. Encode carries the field, so a NONE value
	 * round-trips honestly. */
	ledger.booted_slot = AIUS_AB_SLOT_NONE;
	CHECK(aius_ab_ledger_encode(&ledger, bytes, sizeof(bytes)));
	CHECK(aius_ab_ledger_decode(&decoded, bytes, sizeof(bytes)));
	CHECK(decoded.booted_slot == AIUS_AB_SLOT_NONE);

	current_key = "ledger.booted-slot-domain";
	/* A garbage booted_slot (valid CRC, out-of-domain value) fails closed:
	 * encode writes the byte raw, so the value is CRC-valid but rejected. */
	ledger.booted_slot = 3u;
	CHECK(aius_ab_ledger_encode(&ledger, bytes, sizeof(bytes)));
	CHECK(!aius_ab_ledger_decode(&decoded, bytes, sizeof(bytes)));
	CHECK(decoded.booted_slot == AIUS_AB_SLOT_NONE);

	current_key = "ledger.fail-closed";
	ledger.booted_slot = AIUS_AB_SLOT_B;
	CHECK(aius_ab_ledger_encode(&ledger, bytes, sizeof(bytes)));
	for (index = 0u; index < AIUS_AB_LEDGER_BYTES; index++) {
		memcpy(mutated, bytes, sizeof(mutated));
		mutated[index] ^= 0x01u;
		CHECK(!aius_ab_ledger_decode(&decoded, mutated,
		                             sizeof(mutated)));
		CHECK(decoded.boots_attempted == 0u);
	}
	/* Cold power cycle: OCRAM garbage / zeros refuse. */
	memset(mutated, 0, sizeof(mutated));
	CHECK(!aius_ab_ledger_decode(&decoded, mutated, sizeof(mutated)));
}

static void test_record_pick(void)
{
	uint8_t sector0[AIUS_AB_RECORD_BYTES];
	uint8_t sector1[AIUS_AB_RECORD_BYTES];
	AiusAbRecord record;
	AiusAbRecord picked;

	current_key = "pick.higher-sequence";
	record = committed(5u, AIUS_AB_SLOT_A);
	CHECK(aius_ab_record_encode(&record, sector0, sizeof(sector0)));
	record = committed(6u, AIUS_AB_SLOT_B);
	CHECK(aius_ab_record_encode(&record, sector1, sizeof(sector1)));
	CHECK(aius_ab_record_pick(sector0, sector1, AIUS_AB_RECORD_BYTES,
	                          &picked) == 1);
	CHECK(picked.sequence == 6u);
	CHECK(picked.active_slot == AIUS_AB_SLOT_B);

	current_key = "pick.wrapped-sequence";
	/* Modular ordering: 0 succeeds UINT32_MAX across the wrap. */
	record = committed(UINT32_MAX, AIUS_AB_SLOT_A);
	CHECK(aius_ab_record_encode(&record, sector0, sizeof(sector0)));
	record = committed(0u, AIUS_AB_SLOT_B);
	CHECK(aius_ab_record_encode(&record, sector1, sizeof(sector1)));
	CHECK(aius_ab_record_pick(sector0, sector1, AIUS_AB_RECORD_BYTES,
	                          &picked) == 1);
	CHECK(picked.active_slot == AIUS_AB_SLOT_B);

	current_key = "pick.torn-write";
	/* A torn write -- one sector corrupt -- picks the survivor: the whole
	 * point of the ping-pong pair. */
	sector1[3] ^= 0x40u;
	CHECK(aius_ab_record_pick(sector0, sector1, AIUS_AB_RECORD_BYTES,
	                          &picked) == 0);
	CHECK(picked.active_slot == AIUS_AB_SLOT_A);

	current_key = "pick.none";
	sector0[3] ^= 0x40u;
	CHECK(aius_ab_record_pick(sector0, sector1, AIUS_AB_RECORD_BYTES,
	                          &picked) == -1);
	CHECK(picked.active_slot == AIUS_AB_SLOT_NONE);
}

/* One simulated boot: run the chooser over the record + the retained bytes,
 * persist the ledger exactly as the chooser would, and return the choice. */
static AiusAbChoice boot_once(const AiusAbRecord *record,
	                      uint8_t retained[AIUS_AB_LEDGER_BYTES])
{
	AiusAbLedger ledger;
	AiusAbLedger *ledger_ref = NULL;
	AiusAbChoice choice;

	if (aius_ab_ledger_decode(&ledger, retained, AIUS_AB_LEDGER_BYTES))
		ledger_ref = &ledger;
	aius_ab_choose(record, ledger_ref, &choice);
	if (choice.ledger_write)
		CHECK(aius_ab_ledger_encode(&choice.ledger, retained,
		                            AIUS_AB_LEDGER_BYTES));
	return choice;
}

static void test_choose_rules(void)
{
	AiusAbRecord record;
	AiusAbChoice choice;
	uint8_t retained[AIUS_AB_LEDGER_BYTES];

	current_key = "choose.no-record";
	memset(retained, 0xa5, sizeof(retained));
	choice = boot_once(NULL, retained);
	CHECK(choice.boot_slot == AIUS_AB_SLOT_A);
	CHECK(choice.reason == AIUS_AB_REASON_NO_RECORD);
	CHECK(!choice.ledger_write);

	current_key = "choose.no-record.booted-slot";
	/* booted_slot always tracks the chosen slot: the app's channel to its
	 * physical slot under a single canonical build. */
	CHECK(choice.ledger.booted_slot == AIUS_AB_SLOT_A);

	current_key = "choose.active";
	record = committed(3u, AIUS_AB_SLOT_B);
	choice = boot_once(&record, retained);
	CHECK(choice.boot_slot == AIUS_AB_SLOT_B);
	CHECK(choice.reason == AIUS_AB_REASON_ACTIVE);
	CHECK(!choice.ledger_write);
	CHECK(choice.ledger.booted_slot == AIUS_AB_SLOT_B);

	current_key = "choose.trial-budget-sequence";
	/* THE ROLLBACK SEQUENCE, boot by boot: a 2-boot trial of slot B gets
	 * exactly two launches, and the third boot falls back to A with the
	 * verdict latched for the application. */
	record = trial(4u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 2u);
	memset(retained, 0xa5, sizeof(retained)); /* no valid ledger yet */
	choice = boot_once(&record, retained);
	CHECK(choice.boot_slot == AIUS_AB_SLOT_B);
	CHECK(choice.reason == AIUS_AB_REASON_TRIAL);
	CHECK(choice.ledger.boots_attempted == 1u);
	CHECK(choice.ledger.booted_slot == AIUS_AB_SLOT_B); /* the trial slot */
	choice = boot_once(&record, retained); /* trial crashed: WDOG reset */
	CHECK(choice.boot_slot == AIUS_AB_SLOT_B);
	CHECK(choice.ledger.boots_attempted == 2u);
	choice = boot_once(&record, retained); /* crashed again */
	CHECK(choice.boot_slot == AIUS_AB_SLOT_A);
	CHECK(choice.reason == AIUS_AB_REASON_FELL_BACK);
	CHECK(choice.ledger.fell_back == 1u);
	CHECK(choice.ledger.booted_slot == AIUS_AB_SLOT_A); /* fell back to active */
	/* The fallback booting again (the app has not yet persisted the
	 * rollback): still the active slot, verdict still latched. */
	choice = boot_once(&record, retained);
	CHECK(choice.boot_slot == AIUS_AB_SLOT_A);
	CHECK(choice.reason == AIUS_AB_REASON_FELL_BACK);
	CHECK(choice.ledger.fell_back == 1u);

	current_key = "choose.stale-ledger-resets";
	/* A NEW record sequence starts a fresh count even against a stale
	 * retained ledger from the previous trial. */
	record = trial(5u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 2u);
	choice = boot_once(&record, retained);
	CHECK(choice.boot_slot == AIUS_AB_SLOT_B);
	CHECK(choice.ledger.boots_attempted == 1u);
	CHECK(choice.ledger.record_sequence == 5u);

	current_key = "choose.cold-power-fresh-count";
	/* A cold power cycle clears retained OCRAM: the trial honestly gets
	 * its budget again (only the WDOG crash loop the budget exists for
	 * preserves the count). */
	memset(retained, 0, sizeof(retained));
	choice = boot_once(&record, retained);
	CHECK(choice.boot_slot == AIUS_AB_SLOT_B);
	CHECK(choice.ledger.boots_attempted == 1u);
}

static void test_slot_ivt_sanity(void)
{
	current_key = "ivt.sane";
	/* The Teensy IVT header word carries tag 0xD1 in its low byte. Under the
	 * slot-agnostic scheme BOTH slots' images are linked at the canonical
	 * window, so a sane entry lies there whichever slot the caller read it
	 * from -- the chooser's remap resolves the canonical window to that
	 * physical slot. */
	CHECK(aius_ab_slot_ivt_sane(AIUS_AB_SLOT_A, 0x432000D1u,
	                            AIUS_AB_CANONICAL_BASE + 0x1200u));
	CHECK(aius_ab_slot_ivt_sane(AIUS_AB_SLOT_B, 0x432000D1u,
	                            AIUS_AB_CANONICAL_BASE + 0x1200u));

	current_key = "ivt.insane";
	/* An erased slot (0xFFFFFFFF), a torn header, an entry outside the
	 * canonical window, an entry exactly at the window base (must be >), and
	 * an invalid slot id all refuse. */
	CHECK(!aius_ab_slot_ivt_sane(AIUS_AB_SLOT_A, 0xFFFFFFFFu,
	                             0xFFFFFFFFu));
	CHECK(!aius_ab_slot_ivt_sane(AIUS_AB_SLOT_A, 0x432000D0u,
	                             AIUS_AB_CANONICAL_BASE + 0x1200u));
	CHECK(!aius_ab_slot_ivt_sane(AIUS_AB_SLOT_A, 0x432000D1u,
	                             AIUS_AB_CANONICAL_BASE));
	CHECK(!aius_ab_slot_ivt_sane(AIUS_AB_SLOT_NONE, 0x432000D1u,
	                             AIUS_AB_CANONICAL_BASE + 0x1200u));

	current_key = "ivt.atomic-cutover-guard";
	/* An OLD dual-linked slot-B image -- entry inside slot B's PHYSICAL
	 * window, outside the canonical window -- is refused by the new check.
	 * This is why the chooser, the canonical images and this rule must cut
	 * over together in one HalfKay recovery flash: a new chooser facing an
	 * old B image would (correctly) find it insane. */
	CHECK(!aius_ab_slot_ivt_sane(AIUS_AB_SLOT_B, 0x432000D1u,
	                             AIUS_AB_SLOT_B_BASE + 0x1200u));

	current_key = "ivt.window-form";
	/* The window form the OTA verify path shares with the chooser: the
	 * same words judged against an explicit link window. The plain
	 * single-image posture links at the flash base and is sane against the
	 * boot window, insane against the canonical one; a zero-length window
	 * ("no window known") refuses even a perfect image; a window reaching
	 * the top of the address space does not wrap into accept-everything. */
	CHECK(aius_ab_ivt_sane_in_window(0x432000D1u, AIUS_AB_FLASH_BASE + 0x15f1u,
	                                 AIUS_AB_FLASH_BASE,
	                                 UINT32_C(0x007C0000)));
	CHECK(!aius_ab_ivt_sane_in_window(0x432000D1u,
	                                  AIUS_AB_FLASH_BASE + 0x15f1u,
	                                  AIUS_AB_CANONICAL_BASE,
	                                  AIUS_AB_CANONICAL_BYTES));
	CHECK(aius_ab_ivt_sane_in_window(0x432000D1u,
	                                 AIUS_AB_CANONICAL_BASE + 0x15f1u,
	                                 AIUS_AB_CANONICAL_BASE,
	                                 AIUS_AB_CANONICAL_BYTES));
	CHECK(!aius_ab_ivt_sane_in_window(0x432000D1u,
	                                  AIUS_AB_CANONICAL_BASE + 0x15f1u,
	                                  AIUS_AB_CANONICAL_BASE, 0u));
	CHECK(!aius_ab_ivt_sane_in_window(0x432000D1u, 0xFFFFFFF0u,
	                                  0xFFFFF000u, 0x2000u));
	/* Entry exactly at the window end (exclusive) refuses. */
	CHECK(!aius_ab_ivt_sane_in_window(0x432000D1u,
	                                  AIUS_AB_CANONICAL_BASE +
		                                  AIUS_AB_CANONICAL_BYTES,
	                                  AIUS_AB_CANONICAL_BASE,
	                                  AIUS_AB_CANONICAL_BYTES));
}

static void test_remap_primitive(void)
{
	AiusAbRemap remap;

	current_key = "remap.slot-a-identity";
	/* Slot A: the canonical window IS slot A's, so the offset is zero
	 * (identity) -- HalfKay recovery boots slot A with the remap regs at
	 * their reset value. */
	CHECK(aius_ab_remap_for_slot(AIUS_AB_SLOT_A, &remap));
	CHECK(remap.start == AIUS_AB_CANONICAL_BASE);
	CHECK(remap.end == AIUS_AB_CANONICAL_BASE + AIUS_AB_CANONICAL_BYTES);
	CHECK(remap.offset == 0u);

	current_key = "remap.slot-b-offset";
	/* Slot B: the canonical window is redirected forward by one slot span,
	 * so a canonical fetch lands in slot B's physical flash. */
	CHECK(aius_ab_remap_for_slot(AIUS_AB_SLOT_B, &remap));
	CHECK(remap.start == AIUS_AB_CANONICAL_BASE);
	CHECK(remap.end == AIUS_AB_CANONICAL_BASE + AIUS_AB_CANONICAL_BYTES);
	CHECK(remap.offset == AIUS_AB_SLOT_BYTES);
	/* The end-to-end invariant: a canonical in-window address plus the
	 * offset reaches the slot's physical base. */
	CHECK(remap.start + remap.offset == AIUS_AB_SLOT_B_BASE);
	/* 4 KiB alignment, the remap's hardware granularity, on every field. */
	CHECK((remap.start & 0xFFFu) == 0u);
	CHECK((remap.end & 0xFFFu) == 0u);
	CHECK((remap.offset & 0xFFFu) == 0u);

	current_key = "remap.invalid-slot";
	/* An invalid slot yields no window and a false return: the chooser must
	 * never program a remap for a slot it cannot name. */
	CHECK(!aius_ab_remap_for_slot(AIUS_AB_SLOT_NONE, &remap));
	CHECK(remap.start == 0u && remap.end == 0u && remap.offset == 0u);
	CHECK(!aius_ab_remap_for_slot(AIUS_AB_SLOT_A, NULL));
}

int main(void)
{
	test_layout_arithmetic();
	test_record_codec_fail_closed();
	test_ledger_codec_fail_closed();
	test_record_pick();
	test_choose_rules();
	test_slot_ivt_sanity();
	test_remap_primitive();
	puts("ab tests passed");
	return 0;
}
