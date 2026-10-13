#include "aius_ab.h"

/* Freestanding-safe by construction: the first-stage chooser compiles this
 * file with -ffreestanding and no libc, so everything is explicit loops and
 * arithmetic -- no memcpy, no memset, no tables (the bitwise CRC is the dark
 * stamp's convention, and the chooser has no .rodata story to lean on). The
 * FLASHMEM macro is the application build's placement; the chooser and the
 * host both compile it away. */
#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

_Static_assert(AIUS_AB_SLOT_A_BASE == UINT32_C(0x60010000),
               "slot A sits immediately after the 64 KiB head");
_Static_assert(AIUS_AB_SLOT_B_BASE == UINT32_C(0x603E8000),
               "slot B sits immediately after slot A");
_Static_assert(AIUS_AB_SLOT_B_BASE + AIUS_AB_SLOT_BYTES ==
	               UINT32_C(0x607C0000),
               "slot B ends exactly at the app-addressable region's end");
_Static_assert(AIUS_AB_RECORD_SECTOR_1 + AIUS_AB_SECTOR_BYTES ==
	               AIUS_AB_SLOT_A_BASE,
               "the record pair is the head region's last two sectors");
_Static_assert(AIUS_AB_RECORD_OFFSET_CRC32 + 4u == AIUS_AB_RECORD_BYTES,
               "the record CRC is the last field");
_Static_assert(AIUS_AB_LEDGER_OFFSET_CRC32 + 4u == AIUS_AB_LEDGER_BYTES,
               "the ledger CRC is the last field");

static uint32_t crc32_bitwise(const uint8_t *bytes, uint32_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	uint32_t index;
	unsigned bit;

	for (index = 0u; index < length; index++) {
		crc ^= bytes[index];
		for (bit = 0u; bit < 8u; bit++) {
			if (crc & 1u)
				crc = (crc >> 1) ^ UINT32_C(0xedb88320);
			else
				crc >>= 1;
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value & 0xffu);
	bytes[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xffu);
	bytes[1] = (uint8_t)((value >> 8) & 0xffu);
	bytes[2] = (uint8_t)((value >> 16) & 0xffu);
	bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint16_t get_u16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t get_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool slot_valid(uint8_t slot)
{
	return slot == AIUS_AB_SLOT_A || slot == AIUS_AB_SLOT_B;
}

FLASHMEM bool aius_ab_record_encode(const AiusAbRecord *record,
	                            uint8_t *bytes, size_t length)
{
	uint32_t index;

	if (record == NULL || bytes == NULL ||
	    length != AIUS_AB_RECORD_BYTES)
		return false;
	for (index = 0u; index < AIUS_AB_RECORD_BYTES; index++)
		bytes[index] = 0u;
	put_u32(&bytes[AIUS_AB_RECORD_OFFSET_MAGIC], AIUS_AB_RECORD_MAGIC);
	put_u16(&bytes[AIUS_AB_RECORD_OFFSET_VERSION], AIUS_AB_RECORD_VERSION);
	put_u16(&bytes[AIUS_AB_RECORD_OFFSET_LENGTH], AIUS_AB_RECORD_BYTES);
	put_u32(&bytes[AIUS_AB_RECORD_OFFSET_SEQUENCE], record->sequence);
	bytes[AIUS_AB_RECORD_OFFSET_ACTIVE] = record->active_slot;
	bytes[AIUS_AB_RECORD_OFFSET_PENDING] = record->pending_slot;
	bytes[AIUS_AB_RECORD_OFFSET_BUDGET] = record->trial_budget;
	put_u32(&bytes[AIUS_AB_RECORD_OFFSET_CRC32],
	        crc32_bitwise(bytes, AIUS_AB_RECORD_OFFSET_CRC32));
	return true;
}

FLASHMEM bool aius_ab_record_decode(AiusAbRecord *record,
	                            const uint8_t *bytes, size_t length)
{
	uint8_t active;
	uint8_t pending;
	uint8_t budget;

	if (record == NULL)
		return false;
	record->sequence = 0u;
	record->active_slot = AIUS_AB_SLOT_NONE;
	record->pending_slot = AIUS_AB_SLOT_NONE;
	record->trial_budget = 0u;

	if (bytes == NULL || length != AIUS_AB_RECORD_BYTES)
		return false;
	if (get_u32(&bytes[AIUS_AB_RECORD_OFFSET_MAGIC]) !=
	    AIUS_AB_RECORD_MAGIC)
		return false;
	if (get_u16(&bytes[AIUS_AB_RECORD_OFFSET_VERSION]) !=
	    AIUS_AB_RECORD_VERSION)
		return false;
	if (get_u16(&bytes[AIUS_AB_RECORD_OFFSET_LENGTH]) !=
	    AIUS_AB_RECORD_BYTES)
		return false;
	if (get_u32(&bytes[AIUS_AB_RECORD_OFFSET_CRC32]) !=
	    crc32_bitwise(bytes, AIUS_AB_RECORD_OFFSET_CRC32))
		return false;
	active = bytes[AIUS_AB_RECORD_OFFSET_ACTIVE];
	pending = bytes[AIUS_AB_RECORD_OFFSET_PENDING];
	budget = bytes[AIUS_AB_RECORD_OFFSET_BUDGET];
	if (!slot_valid(active))
		return false;
	if (pending != AIUS_AB_SLOT_NONE &&
	    (!slot_valid(pending) || pending == active))
		return false;
	/* The budget domain rides the trial: zero iff no trial is open, so a
	 * record cannot spell an open trial with no boots to spend. */
	if (pending == AIUS_AB_SLOT_NONE) {
		if (budget != 0u)
			return false;
	} else if (budget == 0u || budget > AIUS_AB_MAX_TRIAL_BOOTS) {
		return false;
	}
	record->sequence = get_u32(&bytes[AIUS_AB_RECORD_OFFSET_SEQUENCE]);
	record->active_slot = active;
	record->pending_slot = pending;
	record->trial_budget = budget;
	return true;
}

FLASHMEM bool aius_ab_ledger_encode(const AiusAbLedger *ledger,
	                            uint8_t *bytes, size_t length)
{
	uint32_t index;

	if (ledger == NULL || bytes == NULL ||
	    length != AIUS_AB_LEDGER_BYTES)
		return false;
	for (index = 0u; index < AIUS_AB_LEDGER_BYTES; index++)
		bytes[index] = 0u;
	put_u32(&bytes[AIUS_AB_LEDGER_OFFSET_MAGIC], AIUS_AB_LEDGER_MAGIC);
	put_u32(&bytes[AIUS_AB_LEDGER_OFFSET_SEQUENCE],
	        ledger->record_sequence);
	put_u32(&bytes[AIUS_AB_LEDGER_OFFSET_BOOTS], ledger->boots_attempted);
	put_u32(&bytes[AIUS_AB_LEDGER_OFFSET_FELL_BACK],
	        ledger->fell_back != 0u ? 1u : 0u);
	bytes[AIUS_AB_LEDGER_OFFSET_BOOTED_SLOT] = ledger->booted_slot;
	put_u32(&bytes[AIUS_AB_LEDGER_OFFSET_CRC32],
	        crc32_bitwise(bytes, AIUS_AB_LEDGER_OFFSET_CRC32));
	return true;
}

FLASHMEM bool aius_ab_ledger_decode(AiusAbLedger *ledger,
	                            const uint8_t *bytes, size_t length)
{
	uint8_t booted_slot;

	if (ledger == NULL)
		return false;
	ledger->record_sequence = 0u;
	ledger->boots_attempted = 0u;
	ledger->fell_back = 0u;
	ledger->booted_slot = AIUS_AB_SLOT_NONE;

	if (bytes == NULL || length != AIUS_AB_LEDGER_BYTES)
		return false;
	if (get_u32(&bytes[AIUS_AB_LEDGER_OFFSET_MAGIC]) !=
	    AIUS_AB_LEDGER_MAGIC)
		return false;
	if (get_u32(&bytes[AIUS_AB_LEDGER_OFFSET_CRC32]) !=
	    crc32_bitwise(bytes, AIUS_AB_LEDGER_OFFSET_CRC32))
		return false;
	if (get_u32(&bytes[AIUS_AB_LEDGER_OFFSET_FELL_BACK]) > 1u)
		return false;
	/* booted_slot domain: NONE (a pre-slot-agnostic ledger) or a real slot;
	 * anything else is a layout drift or garbage and fails closed. */
	booted_slot = bytes[AIUS_AB_LEDGER_OFFSET_BOOTED_SLOT];
	if (booted_slot != AIUS_AB_SLOT_NONE && !slot_valid(booted_slot))
		return false;
	ledger->record_sequence =
		get_u32(&bytes[AIUS_AB_LEDGER_OFFSET_SEQUENCE]);
	ledger->boots_attempted = get_u32(&bytes[AIUS_AB_LEDGER_OFFSET_BOOTS]);
	ledger->fell_back = get_u32(&bytes[AIUS_AB_LEDGER_OFFSET_FELL_BACK]);
	ledger->booted_slot = booted_slot;
	return true;
}

FLASHMEM int aius_ab_record_pick(const uint8_t *sector0,
	                         const uint8_t *sector1, size_t length,
	                         AiusAbRecord *record)
{
	AiusAbRecord first;
	AiusAbRecord second;
	bool first_valid;
	bool second_valid;

	if (record == NULL)
		return -1;
	first_valid = aius_ab_record_decode(&first, sector0, length);
	second_valid = aius_ab_record_decode(&second, sector1, length);
	if (first_valid && second_valid) {
		/* Higher sequence wins; sequence arithmetic is modular so a
		 * wrapped counter still orders adjacent writes correctly. */
		if ((int32_t)(second.sequence - first.sequence) > 0) {
			*record = second;
			return 1;
		}
		*record = first;
		return 0;
	}
	if (first_valid) {
		*record = first;
		return 0;
	}
	if (second_valid) {
		*record = second;
		return 1;
	}
	(void)aius_ab_record_decode(record, NULL, 0u); /* zeroed, honestly */
	return -1;
}

FLASHMEM void aius_ab_choose(const AiusAbRecord *record,
	                     const AiusAbLedger *ledger, AiusAbChoice *choice)
{
	uint32_t boots;

	if (choice == NULL)
		return;
	choice->boot_slot = AIUS_AB_SLOT_A;
	choice->reason = (uint8_t)AIUS_AB_REASON_NO_RECORD;
	choice->ledger_write = false;
	choice->ledger.record_sequence = 0u;
	choice->ledger.boots_attempted = 0u;
	choice->ledger.fell_back = 0u;
	/* booted_slot always tracks the slot actually chosen: the chooser
	 * persists it as the app's only channel to its physical slot under a
	 * single canonical build (aius-ota-slot-agnostic.md). */
	choice->ledger.booted_slot = AIUS_AB_SLOT_A;

	if (record == NULL)
		return; /* fresh board / recovery flash: slot A, fail-closed */

	if (record->pending_slot == AIUS_AB_SLOT_NONE) {
		choice->boot_slot = record->active_slot;
		choice->reason = (uint8_t)AIUS_AB_REASON_ACTIVE;
		choice->ledger.booted_slot = record->active_slot;
		return;
	}

	/* A trial is open. The ledger counts its boots; one that is missing,
	 * invalid or belongs to an OLDER record starts the count fresh --
	 * which is also the honest cold-power-cycle behaviour (retained OCRAM
	 * cleared -> the trial gets its budget again, and only the WDOG-reset
	 * crash loop the budget exists for keeps the count). */
	boots = 1u;
	if (ledger != NULL && ledger->record_sequence == record->sequence)
		boots = ledger->boots_attempted + 1u;

	choice->ledger.record_sequence = record->sequence;
	choice->ledger_write = true;
	if (boots > record->trial_budget) {
		choice->boot_slot = record->active_slot;
		choice->reason = (uint8_t)AIUS_AB_REASON_FELL_BACK;
		choice->ledger.booted_slot = record->active_slot;
		/* The count stops moving once the fall-back is recorded: the
		 * ledger now carries the verdict for the application to
		 * persist, not a live counter. */
		choice->ledger.boots_attempted =
			ledger != NULL ? ledger->boots_attempted : 0u;
		choice->ledger.fell_back = 1u;
		return;
	}
	choice->boot_slot = record->pending_slot;
	choice->reason = (uint8_t)AIUS_AB_REASON_TRIAL;
	choice->ledger.boots_attempted = boots;
	choice->ledger.fell_back = 0u;
	choice->ledger.booted_slot = record->pending_slot;
}

FLASHMEM uint8_t aius_ab_other_slot(uint8_t slot)
{
	if (slot == AIUS_AB_SLOT_A)
		return AIUS_AB_SLOT_B;
	if (slot == AIUS_AB_SLOT_B)
		return AIUS_AB_SLOT_A;
	return AIUS_AB_SLOT_NONE;
}

FLASHMEM uint32_t aius_ab_slot_base(uint8_t slot)
{
	if (slot == AIUS_AB_SLOT_A)
		return AIUS_AB_SLOT_A_BASE;
	if (slot == AIUS_AB_SLOT_B)
		return AIUS_AB_SLOT_B_BASE;
	return 0u;
}

FLASHMEM bool aius_ab_remap_for_slot(uint8_t slot, AiusAbRemap *out)
{
	uint32_t base;

	if (out == NULL)
		return false;
	out->start = 0u;
	out->end = 0u;
	out->offset = 0u;
	base = aius_ab_slot_base(slot);
	if (base == 0u)
		return false;
	/* The window is always the canonical link window; the offset carries an
	 * in-window (canonical) address to the slot's physical flash -- 0 for
	 * slot A (the canonical window IS slot A's), +AIUS_AB_SLOT_BYTES for B. */
	out->start = AIUS_AB_CANONICAL_BASE;
	out->end = AIUS_AB_CANONICAL_BASE + AIUS_AB_CANONICAL_BYTES;
	out->offset = base - AIUS_AB_CANONICAL_BASE;
	return true;
}

FLASHMEM bool aius_ab_ivt_sane_in_window(uint32_t ivt_word0,
	                                 uint32_t entry_word,
	                                 uint32_t window_base,
	                                 uint32_t window_bytes)
{
	/* No window, no verdict: a caller that does not know where the image
	 * must have been linked cannot call it sane. The end is computed in
	 * 64 bits so a window reaching the top of the address space cannot
	 * wrap into a bound that accepts everything. */
	if (window_bytes == 0u ||
	    (uint64_t)window_base + window_bytes > UINT64_C(0x100000000))
		return false;
	/* The i.MX RT IVT header tag is 0xD1 in the word's low byte on this
	 * little-endian part; an erased sector reads 0xFFFFFFFF and a torn
	 * program something else, and both must fail here so the chooser
	 * takes the other slot instead of jumping into garbage. */
	if ((ivt_word0 & 0xffu) != 0xd1u)
		return false;
	/* The entry pointer is a LINKED address: an image linked for another
	 * window (a plain 0x60000000 image offered to a slot, an old
	 * physically-linked slot-B image, text) points outside this one. */
	return entry_word > window_base &&
	       entry_word < window_base + window_bytes;
}

FLASHMEM bool aius_ab_slot_ivt_sane(uint8_t slot, uint32_t ivt_word0,
	                            uint32_t entry_word)
{
	if (aius_ab_slot_base(slot) == 0u)
		return false;
	/* Every slot-agnostic image is LINKED at the canonical window, so its
	 * entry pointer lies there whichever physical slot it occupies -- the
	 * chooser's remap makes the canonical window resolve to that slot. An
	 * entry outside the canonical window is an unlinked/torn/cross-scheme
	 * image the chooser must refuse. */
	return aius_ab_ivt_sane_in_window(ivt_word0, entry_word,
	                                  AIUS_AB_CANONICAL_BASE,
	                                  AIUS_AB_CANONICAL_BYTES);
}

/* ------------------------------------------------------------------ */
/* Per-slot build revisions (Phase 4y)                                 */
/* ------------------------------------------------------------------ */

/* Application and host only. The chooser compiles this file -ffreestanding
 * (__STDC_HOSTED__ == 0) and links without -ffunction-sections, so code here
 * would ride into its image as dead weight; behind this guard its object is
 * exactly what it was, and "no chooser change" holds byte for byte. */
#if __STDC_HOSTED__

static FLASHMEM int revision_slot_index(uint8_t slot)
{
	if (slot == AIUS_AB_SLOT_A)
		return 0;
	if (slot == AIUS_AB_SLOT_B)
		return 1;
	return -1;
}

static FLASHMEM void revision_copy(uint8_t *to, const uint8_t *from)
{
	uint32_t index;

	for (index = 0u; index < AIUS_AB_REVISION_BYTES; index++)
		to[index] = from != NULL ? from[index] : 0u;
}

static FLASHMEM bool revision_equal(const uint8_t *a, const uint8_t *b)
{
	uint32_t index;

	for (index = 0u; index < AIUS_AB_REVISION_BYTES; index++)
		if (a[index] != b[index])
			return false;
	return true;
}

static FLASHMEM bool revision_known(const uint8_t *revision)
{
	uint32_t index;

	for (index = 0u; index < AIUS_AB_REVISION_BYTES; index++)
		if (revision[index] != 0u)
			return true;
	return false;
}

/* `from` NULL clears both. */
static FLASHMEM void revisions_copy(AiusAbRevisions *to,
	                            const AiusAbRevisions *from)
{
	revision_copy(to->slot[0], from != NULL ? from->slot[0] : NULL);
	revision_copy(to->slot[1], from != NULL ? from->slot[1] : NULL);
}

static FLASHMEM void record_copy(AiusAbRecord *to, const AiusAbRecord *from)
{
	to->sequence = from->sequence;
	to->active_slot = from->active_slot;
	to->pending_slot = from->pending_slot;
	to->trial_budget = from->trial_budget;
}

static FLASHMEM int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

FLASHMEM bool aius_ab_record_encode_revisions(const AiusAbRecord *record,
	                                      const AiusAbRevisions *revisions,
	                                      uint8_t *bytes, size_t length)
{
	uint32_t index;

	if (!aius_ab_record_encode(record, bytes, length))
		return false;
	if (revisions == NULL)
		return true;
	for (index = 0u; index < AIUS_AB_REVISION_BYTES; index++) {
		bytes[AIUS_AB_RECORD_OFFSET_REVISIONS + index] =
			revisions->slot[0][index];
		bytes[AIUS_AB_RECORD_OFFSET_REVISIONS + AIUS_AB_REVISION_BYTES +
		      index] = revisions->slot[1][index];
	}
	put_u32(&bytes[AIUS_AB_RECORD_OFFSET_CRC32],
	        crc32_bitwise(bytes, AIUS_AB_RECORD_OFFSET_CRC32));
	return true;
}

FLASHMEM bool aius_ab_record_revisions(const uint8_t *bytes, size_t length,
	                               AiusAbRevisions *revisions)
{
	AiusAbRecord record;

	if (revisions == NULL)
		return false;
	revisions_copy(revisions, NULL);
	if (!aius_ab_record_decode(&record, bytes, length))
		return false;
	revision_copy(revisions->slot[0],
	              &bytes[AIUS_AB_RECORD_OFFSET_REVISIONS]);
	revision_copy(revisions->slot[1],
	              &bytes[AIUS_AB_RECORD_OFFSET_REVISIONS +
	                     AIUS_AB_REVISION_BYTES]);
	return true;
}

FLASHMEM bool aius_ab_revision_parse(const char *text,
	                             uint8_t revision[AIUS_AB_REVISION_BYTES])
{
	uint8_t packed[AIUS_AB_REVISION_BYTES];
	uint32_t index;
	int digit;

	if (revision == NULL)
		return false;
	revision_copy(revision, NULL);
	if (text == NULL)
		return false;
	/* The digit test stops at the NUL, so a short string never reads past
	 * its end. */
	for (index = 0u; index < AIUS_AB_REVISION_HEX_CHARS; index++) {
		digit = hex_digit(text[index]);
		if (digit < 0)
			return false;
		if ((index & 1u) == 0u)
			packed[index / 2u] = (uint8_t)(digit << 4);
		else
			packed[index / 2u] |= (uint8_t)digit;
	}
	if (text[AIUS_AB_REVISION_HEX_CHARS] != '\0')
		return false;
	revision_copy(revision, packed);
	return revision_known(revision);
}

FLASHMEM bool aius_ab_revision_format(
	const uint8_t revision[AIUS_AB_REVISION_BYTES], char *text,
	size_t capacity)
{
	uint32_t index;
	unsigned nibble;

	if (text == NULL || capacity == 0u)
		return false;
	text[0] = '\0';
	if (revision == NULL || capacity < AIUS_AB_REVISION_TEXT_STORAGE ||
	    !revision_known(revision))
		return false;
	for (index = 0u; index < AIUS_AB_REVISION_HEX_CHARS; index++) {
		nibble = (index & 1u) == 0u ? (unsigned)(revision[index / 2u] >> 4)
		                            : (unsigned)(revision[index / 2u] & 0xfu);
		text[index] = (char)(nibble < 10u ? '0' + nibble
		                                  : 'a' + (nibble - 10u));
	}
	text[AIUS_AB_REVISION_HEX_CHARS] = '\0';
	return true;
}

FLASHMEM const uint8_t *aius_ab_revision_of(const AiusAbRevisions *revisions,
	                                    uint8_t slot)
{
	const int index = revision_slot_index(slot);

	if (revisions == NULL || index < 0)
		return NULL;
	return revisions->slot[index];
}

static FLASHMEM void revision_set(AiusAbRevisions *revisions, uint8_t slot,
	                          const uint8_t *revision)
{
	const int index = revision_slot_index(slot);

	if (index >= 0)
		revision_copy(revisions->slot[index], revision);
}

FLASHMEM bool aius_ab_plan_healthy(const AiusAbRecord *record,
	                           const AiusAbRevisions *revisions,
	                           uint8_t running_slot,
	                           const char *running_revision,
	                           AiusAbRecord *next,
	                           AiusAbRevisions *next_revisions)
{
	uint8_t packed[AIUS_AB_REVISION_BYTES];

	if (next == NULL || next_revisions == NULL || !slot_valid(running_slot))
		return false;
	(void)aius_ab_revision_parse(running_revision, packed);
	if (record == NULL) {
		/* The fresh migration's first record, committed to this slot. */
		next->sequence = 1u;
		next->active_slot = running_slot;
		next->pending_slot = AIUS_AB_SLOT_NONE;
		next->trial_budget = 0u;
		revisions_copy(next_revisions, NULL);
		revision_set(next_revisions, running_slot, packed);
		return true;
	}
	record_copy(next, record);
	revisions_copy(next_revisions, revisions);
	if (record->pending_slot == running_slot) {
		/* The trial vouches for itself; the revision folds into the same
		 * write. */
		next->sequence++;
		next->active_slot = running_slot;
		next->pending_slot = AIUS_AB_SLOT_NONE;
		next->trial_budget = 0u;
		revision_set(next_revisions, running_slot, packed);
		return true;
	}
	if (record->pending_slot != AIUS_AB_SLOT_NONE)
		return false; /* a trial for the other slot stays as it is */
	if (revision_equal(aius_ab_revision_of(next_revisions, running_slot),
	                   packed))
		return false;
	next->sequence++;
	revision_set(next_revisions, running_slot, packed);
	return true;
}

FLASHMEM bool aius_ab_plan_pending(const AiusAbRecord *record,
	                           const AiusAbRevisions *revisions,
	                           uint8_t running_slot, uint8_t budget,
	                           const char *target_revision,
	                           AiusAbRecord *next,
	                           AiusAbRevisions *next_revisions)
{
	uint8_t packed[AIUS_AB_REVISION_BYTES];

	if (next == NULL || next_revisions == NULL ||
	    !slot_valid(running_slot) || budget == 0u ||
	    budget > AIUS_AB_MAX_TRIAL_BOOTS)
		return false;
	if (record != NULL) {
		record_copy(next, record);
		revisions_copy(next_revisions, revisions);
	} else {
		next->sequence = 0u;
		revisions_copy(next_revisions, NULL);
	}
	next->sequence++;
	next->active_slot = running_slot;
	next->pending_slot = aius_ab_other_slot(running_slot);
	next->trial_budget = budget;
	(void)aius_ab_revision_parse(target_revision, packed);
	revision_set(next_revisions, next->pending_slot, packed);
	return true;
}

FLASHMEM bool aius_ab_plan_clear_other(const AiusAbRecord *record,
	                               const AiusAbRevisions *revisions,
	                               uint8_t running_slot, AiusAbRecord *next,
	                               AiusAbRevisions *next_revisions)
{
	const uint8_t other = aius_ab_other_slot(running_slot);
	const uint8_t *stored;

	if (record == NULL || revisions == NULL || next == NULL ||
	    next_revisions == NULL || !slot_valid(running_slot))
		return false;
	stored = aius_ab_revision_of(revisions, other);
	if (!revision_known(stored))
		return false;
	record_copy(next, record);
	revisions_copy(next_revisions, revisions);
	next->sequence++;
	if (next->pending_slot == other) {
		next->active_slot = running_slot;
		next->pending_slot = AIUS_AB_SLOT_NONE;
		next->trial_budget = 0u;
	}
	revision_set(next_revisions, other, NULL);
	return true;
}

FLASHMEM bool aius_ab_plan_switch(const AiusAbRecord *record,
	                          uint8_t running_slot, uint8_t budget,
	                          AiusAbRecord *next)
{
	if (record == NULL || next == NULL || !slot_valid(running_slot) ||
	    budget == 0u || budget > AIUS_AB_MAX_TRIAL_BOOTS)
		return false;
	if (record->pending_slot != AIUS_AB_SLOT_NONE ||
	    record->active_slot != running_slot)
		return false;
	record_copy(next, record);
	next->sequence++;
	next->pending_slot = aius_ab_other_slot(running_slot);
	next->trial_budget = budget;
	return true;
}

#endif /* __STDC_HOSTED__ */
