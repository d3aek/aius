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

/* ------------------------------------------------------------------ */
/* Phase 4y: per-slot revisions in bytes 16-27                         */
/* ------------------------------------------------------------------ */

/* THE FROZEN VERSION-1 ORACLE. The host aius_ab.c is the chooser's own
 * source, so its decode accepting a new record proves nothing about the
 * chooser on the board; this is the version-1 acceptance rule written out
 * independently (own CRC, own offsets), and it must accept every record the
 * revision-aware encoder writes. */
static uint32_t v1_crc32(const uint8_t *bytes, unsigned length)
{
	uint32_t crc = 0xffffffffu;
	unsigned index;
	unsigned bit;

	for (index = 0u; index < length; index++) {
		crc ^= bytes[index];
		for (bit = 0u; bit < 8u; bit++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
	}
	return ~crc;
}

static int v1_oracle_accepts(const uint8_t bytes[32])
{
	const uint32_t magic = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	                       ((uint32_t)bytes[2] << 16) |
	                       ((uint32_t)bytes[3] << 24);
	const unsigned version = (unsigned)bytes[4] | ((unsigned)bytes[5] << 8);
	const unsigned length = (unsigned)bytes[6] | ((unsigned)bytes[7] << 8);
	const uint32_t crc = (uint32_t)bytes[28] | ((uint32_t)bytes[29] << 8) |
	                     ((uint32_t)bytes[30] << 16) |
	                     ((uint32_t)bytes[31] << 24);
	const uint8_t active = bytes[12];
	const uint8_t pending = bytes[13];
	const uint8_t budget = bytes[14];

	if (magic != 0x42414941u || version != 1u || length != 32u)
		return 0;
	if (crc != v1_crc32(bytes, 28u))
		return 0;
	if (active != 1u && active != 2u)
		return 0;
	if (pending != 0u && ((pending != 1u && pending != 2u) ||
	                      pending == active))
		return 0;
	if (pending == 0u)
		return budget == 0u;
	return budget >= 1u && budget <= 8u;
}

/* Bytes the PRE-4y encoder wrote for {sequence 0x01020304, active A,
 * pending B, budget 2}, checked in: the revision-aware encoder with no
 * revisions must reproduce them exactly. */
static const uint8_t kPre4yRecord[32] = {
	0x41, 0x49, 0x41, 0x42, 0x01, 0x00, 0x20, 0x00,
	0x04, 0x03, 0x02, 0x01, 0x01, 0x02, 0x02, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x03, 0x09, 0x68, 0x6b,
};

static AiusAbRevisions revisions_of(const char *a, const char *b)
{
	AiusAbRevisions revisions;

	memset(&revisions, 0, sizeof(revisions));
	(void)aius_ab_revision_parse(a, revisions.slot[0]);
	(void)aius_ab_revision_parse(b, revisions.slot[1]);
	return revisions;
}

static int revision_is(const AiusAbRevisions *revisions, uint8_t slot,
	               const char *expected)
{
	char text[AIUS_AB_REVISION_TEXT_STORAGE];

	if (expected == NULL)
		return !aius_ab_revision_format(
			       aius_ab_revision_of(revisions, slot), text,
			       sizeof(text)) &&
		       text[0] == '\0';
	return aius_ab_revision_format(aius_ab_revision_of(revisions, slot),
	                               text, sizeof(text)) &&
	       strcmp(text, expected) == 0;
}

static void test_revision_text(void)
{
	uint8_t revision[AIUS_AB_REVISION_BYTES];
	char text[AIUS_AB_REVISION_TEXT_STORAGE];
	static const char *const refused[] = {
		NULL, "", "92aae16", "92aae16abcd", "92aae16abcdef",
		"92aae16abcde+aius-ab", "92aae16abcdg", "92aae16abcd ",
		" 92aae16abcd", "unknown", "000000000000",
	};
	unsigned index;

	current_key = "revision.parse-folds-case";
	CHECK(aius_ab_revision_parse("92AAE16abcDE", revision));
	CHECK(revision[0] == 0x92u && revision[1] == 0xaau &&
	      revision[2] == 0xe1u && revision[3] == 0x6au &&
	      revision[4] == 0xbcu && revision[5] == 0xdeu);
	CHECK(aius_ab_revision_format(revision, text, sizeof(text)));
	CHECK(strcmp(text, "92aae16abcde") == 0);

	current_key = "revision.non-12-hex-stores-zero";
	for (index = 0u; index < sizeof(refused) / sizeof(refused[0]);
	     index++) {
		memset(revision, 0x5a, sizeof(revision));
		CHECK(!aius_ab_revision_parse(refused[index], revision));
		CHECK(revision[0] == 0u && revision[1] == 0u &&
		      revision[2] == 0u && revision[3] == 0u &&
		      revision[4] == 0u && revision[5] == 0u);
	}

	current_key = "revision.format-unknown";
	memset(revision, 0, sizeof(revision));
	memset(text, 'x', sizeof(text));
	CHECK(!aius_ab_revision_format(revision, text, sizeof(text)));
	CHECK(text[0] == '\0');
	CHECK(aius_ab_revision_parse("0123456789ab", revision));
	CHECK(!aius_ab_revision_format(revision, text, sizeof(text) - 1u));
	CHECK(aius_ab_revision_of(NULL, AIUS_AB_SLOT_A) == NULL);
}

static void test_record_revisions(void)
{
	AiusAbRecord record;
	AiusAbRecord decoded;
	AiusAbRevisions revisions;
	AiusAbRevisions read;
	uint8_t bytes[AIUS_AB_RECORD_BYTES];
	uint8_t zeroed[AIUS_AB_RECORD_BYTES];
	unsigned index;

	current_key = "revisions.pre-4y-bytes";
	record = trial(0x01020304u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 2u);
	CHECK(aius_ab_record_encode_revisions(&record, NULL, bytes,
	                                      sizeof(bytes)));
	CHECK(memcmp(bytes, kPre4yRecord, sizeof(bytes)) == 0);
	CHECK(aius_ab_record_encode(&record, bytes, sizeof(bytes)));
	CHECK(memcmp(bytes, kPre4yRecord, sizeof(bytes)) == 0);
	CHECK(v1_oracle_accepts(kPre4yRecord));

	current_key = "revisions.round-trip";
	revisions = revisions_of("0123456789ab", "fedcba987654");
	CHECK(aius_ab_record_encode_revisions(&record, &revisions, bytes,
	                                      sizeof(bytes)));
	CHECK(bytes[16] == 0x01u && bytes[21] == 0xabu);
	CHECK(bytes[22] == 0xfeu && bytes[27] == 0x54u);
	CHECK(aius_ab_record_decode(&decoded, bytes, sizeof(bytes)));
	CHECK(decoded.sequence == 0x01020304u &&
	      decoded.active_slot == AIUS_AB_SLOT_A &&
	      decoded.pending_slot == AIUS_AB_SLOT_B &&
	      decoded.trial_budget == 2u);
	CHECK(aius_ab_record_revisions(bytes, sizeof(bytes), &read));
	CHECK(revision_is(&read, AIUS_AB_SLOT_A, "0123456789ab"));
	CHECK(revision_is(&read, AIUS_AB_SLOT_B, "fedcba987654"));

	current_key = "revisions.frozen-v1-oracle";
	/* Every slot/budget shape the app writes, with revisions in every
	 * byte, passes the frozen version-1 rule and the pick. */
	{
		static const struct {
			uint8_t active, pending, budget;
		} shapes[] = {
			{ AIUS_AB_SLOT_A, AIUS_AB_SLOT_NONE, 0u },
			{ AIUS_AB_SLOT_B, AIUS_AB_SLOT_NONE, 0u },
			{ AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 2u },
			{ AIUS_AB_SLOT_B, AIUS_AB_SLOT_A, 8u },
		};
		for (index = 0u; index < sizeof(shapes) / sizeof(shapes[0]);
		     index++) {
			AiusAbRecord picked;
			record = trial(index + 9u, shapes[index].active,
			               shapes[index].pending, shapes[index].budget);
			revisions = revisions_of("ffffffffffff", "a1b2c3d4e5f6");
			CHECK(aius_ab_record_encode_revisions(
				&record, &revisions, bytes, sizeof(bytes)));
			CHECK(v1_oracle_accepts(bytes));
			CHECK(aius_ab_record_pick(bytes, NULL, sizeof(bytes),
			                          &picked) == 0);
		}
	}

	current_key = "revisions.zero-bytes-unknown";
	record = committed(3u, AIUS_AB_SLOT_B);
	CHECK(aius_ab_record_encode(&record, zeroed, sizeof(zeroed)));
	CHECK(aius_ab_record_revisions(zeroed, sizeof(zeroed), &read));
	CHECK(revision_is(&read, AIUS_AB_SLOT_A, NULL));
	CHECK(revision_is(&read, AIUS_AB_SLOT_B, NULL));

	current_key = "revisions.invalid-record";
	memset(&read, 0x77, sizeof(read));
	bytes[3] ^= 0x01u;
	CHECK(!aius_ab_record_revisions(bytes, sizeof(bytes), &read));
	CHECK(revision_is(&read, AIUS_AB_SLOT_A, NULL));
	CHECK(revision_is(&read, AIUS_AB_SLOT_B, NULL));
}

static void test_plan_healthy(void)
{
	AiusAbRecord record;
	AiusAbRecord next;
	AiusAbRevisions revisions;
	AiusAbRevisions next_revisions;

	current_key = "healthy.first-record";
	CHECK(aius_ab_plan_healthy(NULL, NULL, AIUS_AB_SLOT_B, "0123456789ab",
	                           &next, &next_revisions));
	CHECK(next.sequence == 1u && next.active_slot == AIUS_AB_SLOT_B &&
	      next.pending_slot == AIUS_AB_SLOT_NONE && next.trial_budget == 0u);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_B, "0123456789ab"));
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, NULL));

	current_key = "healthy.writes-once";
	/* A record with no revision (the first 4y boot): one write, then the
	 * same boot's inputs on the next boot ask for none. */
	record = committed(5u, AIUS_AB_SLOT_A);
	memset(&revisions, 0, sizeof(revisions));
	CHECK(aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_A,
	                           "0123456789AB", &next, &next_revisions));
	CHECK(next.sequence == 6u && next.active_slot == AIUS_AB_SLOT_A &&
	      next.pending_slot == AIUS_AB_SLOT_NONE);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, "0123456789ab"));
	record = next;
	revisions = next_revisions;
	CHECK(!aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_A,
	                            "0123456789ab", &next, &next_revisions));

	current_key = "healthy.rewrites-a-different-revision";
	/* HalfKay recovery, a bad-IVT fallback, a rollback into a slot whose
	 * stored revision is stale: the running image's own wins. */
	CHECK(aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_A,
	                           "fedcba987654", &next, &next_revisions));
	CHECK(next.sequence == 7u);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, "fedcba987654"));

	current_key = "healthy.unknown-build-clears";
	CHECK(aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_A,
	                           "unknown", &next, &next_revisions));
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, NULL));
	memset(&revisions, 0, sizeof(revisions));
	CHECK(!aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_A,
	                            "0123456789ab+aius-ab", &next,
	                            &next_revisions));

	current_key = "healthy.trial-commit-folds-revision";
	record = trial(9u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 2u);
	revisions = revisions_of("111111111111", "222222222222");
	CHECK(aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_B,
	                           "333333333333", &next, &next_revisions));
	CHECK(next.sequence == 10u && next.active_slot == AIUS_AB_SLOT_B &&
	      next.pending_slot == AIUS_AB_SLOT_NONE && next.trial_budget == 0u);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, "111111111111"));
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_B, "333333333333"));

	current_key = "healthy.other-slot-trial-untouched";
	/* N1: a trial pending for the OTHER slot (a failed rollback write)
	 * never earns a write, even with a stale revision. */
	record = trial(9u, AIUS_AB_SLOT_A, AIUS_AB_SLOT_B, 2u);
	CHECK(!aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_A,
	                            "333333333333", &next, &next_revisions));
	CHECK(!aius_ab_plan_healthy(&record, &revisions, AIUS_AB_SLOT_NONE,
	                            "333333333333", &next, &next_revisions));
}

static void test_plan_writes(void)
{
	AiusAbRecord record;
	AiusAbRecord next;
	AiusAbRevisions revisions;
	AiusAbRevisions next_revisions;

	current_key = "pending.sets-target-revision";
	record = committed(4u, AIUS_AB_SLOT_A);
	revisions = revisions_of("111111111111", "222222222222");
	CHECK(aius_ab_plan_pending(&record, &revisions, AIUS_AB_SLOT_A,
	                           AIUS_AB_DEFAULT_TRIAL_BOOTS, "abcdefabcdef",
	                           &next, &next_revisions));
	CHECK(next.sequence == 5u && next.active_slot == AIUS_AB_SLOT_A &&
	      next.pending_slot == AIUS_AB_SLOT_B &&
	      next.trial_budget == AIUS_AB_DEFAULT_TRIAL_BOOTS);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, "111111111111"));
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_B, "abcdefabcdef"));
	CHECK(aius_ab_plan_pending(NULL, NULL, AIUS_AB_SLOT_B, 2u,
	                           "not-a-revision", &next, &next_revisions));
	CHECK(next.sequence == 1u && next.active_slot == AIUS_AB_SLOT_B &&
	      next.pending_slot == AIUS_AB_SLOT_A);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, NULL));

	current_key = "clear.other-slot-only";
	CHECK(aius_ab_plan_clear_other(&record, &revisions, AIUS_AB_SLOT_A,
	                               &next, &next_revisions));
	CHECK(next.sequence == 5u && next.active_slot == AIUS_AB_SLOT_A &&
	      next.pending_slot == AIUS_AB_SLOT_NONE);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, "111111111111"));
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_B, NULL));
	/* Already unknown, or no record: nothing to write. */
	CHECK(!aius_ab_plan_clear_other(&next, &next_revisions, AIUS_AB_SLOT_A,
	                                &record, &revisions));
	CHECK(!aius_ab_plan_clear_other(NULL, NULL, AIUS_AB_SLOT_A, &next,
	                                &next_revisions));

	current_key = "clear.closes-a-stale-other-trial";
	record = trial(8u, AIUS_AB_SLOT_B, AIUS_AB_SLOT_A, 2u);
	revisions = revisions_of("111111111111", "222222222222");
	CHECK(aius_ab_plan_clear_other(&record, &revisions, AIUS_AB_SLOT_B,
	                               &next, &next_revisions));
	CHECK(next.sequence == 9u && next.active_slot == AIUS_AB_SLOT_B &&
	      next.pending_slot == AIUS_AB_SLOT_NONE && next.trial_budget == 0u);
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_A, NULL));
	CHECK(revision_is(&next_revisions, AIUS_AB_SLOT_B, "222222222222"));

	current_key = "switch.pending-other-with-budget";
	record = committed(12u, AIUS_AB_SLOT_B);
	CHECK(aius_ab_plan_switch(&record, AIUS_AB_SLOT_B,
	                          AIUS_AB_DEFAULT_TRIAL_BOOTS, &next));
	CHECK(next.sequence == 13u && next.active_slot == AIUS_AB_SLOT_B &&
	      next.pending_slot == AIUS_AB_SLOT_A &&
	      next.trial_budget == AIUS_AB_DEFAULT_TRIAL_BOOTS);
	{
		uint8_t bytes[AIUS_AB_RECORD_BYTES];
		AiusAbChoice choice;
		AiusAbRecord decoded;
		CHECK(aius_ab_record_encode_revisions(&next, &revisions, bytes,
		                                      sizeof(bytes)));
		CHECK(v1_oracle_accepts(bytes));
		CHECK(aius_ab_record_decode(&decoded, bytes, sizeof(bytes)));
		aius_ab_choose(&decoded, NULL, &choice);
		CHECK(choice.boot_slot == AIUS_AB_SLOT_A &&
		      choice.reason == AIUS_AB_REASON_TRIAL);
	}

	current_key = "switch.refusals";
	CHECK(!aius_ab_plan_switch(NULL, AIUS_AB_SLOT_A, 2u, &next));
	CHECK(!aius_ab_plan_switch(&record, AIUS_AB_SLOT_A, 2u, &next));
	CHECK(!aius_ab_plan_switch(&record, AIUS_AB_SLOT_B, 0u, &next));
	CHECK(!aius_ab_plan_switch(&record, AIUS_AB_SLOT_B,
	                           AIUS_AB_MAX_TRIAL_BOOTS + 1u, &next));
	record = trial(12u, AIUS_AB_SLOT_B, AIUS_AB_SLOT_A, 2u);
	CHECK(!aius_ab_plan_switch(&record, AIUS_AB_SLOT_B, 2u, &next));
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
	test_revision_text();
	test_record_revisions();
	test_plan_healthy();
	test_plan_writes();
	puts("ab tests passed");
	return 0;
}
