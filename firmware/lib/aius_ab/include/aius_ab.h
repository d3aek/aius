#ifndef AIUS_AB_H
#define AIUS_AB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_ab_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The A/B boot state: the flash boot record, the retained trial ledger, and
 * the chooser's decision rule — PURE LOGIC shared verbatim by three
 * builds that must never disagree about it: the first-stage chooser (a
 * freestanding image with no libc), the slot application, and the host
 * suite. Everything here is explicit byte codecs and arithmetic; no
 * memcpy/memset, no globals, nothing target-shaped, so the chooser can
 * compile this file with -ffreestanding and the host can pin every rule.
 *
 * WHO WRITES WHAT — the discipline that keeps the chooser too simple to be
 * wrong:
 *
 *   flash boot record   application only (ping-pong sector pair; the
 *                       activation writes pending, the healthy boot writes
 *                       committed, the fallback boot clears the trial).
 *   retained ledger     chooser only (trial boot counting; the application
 *                       READS it to learn a fallback happened). Retained
 *                       OCRAM survives exactly the WDOG reset a crashing
 *                       trial dies by; a cold power cycle clears it, which
 *                       fail-closes to a fresh trial count.
 *
 * FAIL-CLOSED EVERYWHERE: an invalid record means "boot slot A" (the state
 * of a fresh board or a HalfKay recovery flash, whose combined image
 * carries slot A); an invalid ledger means "first trial boot". */

/* Record wire layout, 32 bytes, in the ping-pong sectors
 * (AIUS_AB_RECORD_SECTOR_0/1). Written by one image and read by another
 * (and by the chooser), so it is offsets-and-CRC, never a shared struct.
 *
 *    0   4  magic "AIAB"
 *    4   2  version = 1
 *    6   2  record_length = 32
 *    8   4  sequence (monotonic; the higher valid record wins)
 *   12   1  active_slot  (A or B: the committed, known-good slot)
 *   13   1  pending_slot (NONE, or the freshly-programmed slot on trial)
 *   14   1  trial_budget (boots a trial may take; 0 iff pending is NONE)
 *   15   1  reserved0 = 0
 *   16  12  reserved = 0
 *   28   4  CRC-32 over bytes [0, 28)
 */
#define AIUS_AB_RECORD_MAGIC UINT32_C(0x42414941) /* "AIAB" */
#define AIUS_AB_RECORD_VERSION 1u
#define AIUS_AB_RECORD_BYTES 32u
#define AIUS_AB_RECORD_OFFSET_MAGIC 0u
#define AIUS_AB_RECORD_OFFSET_VERSION 4u
#define AIUS_AB_RECORD_OFFSET_LENGTH 6u
#define AIUS_AB_RECORD_OFFSET_SEQUENCE 8u
#define AIUS_AB_RECORD_OFFSET_ACTIVE 12u
#define AIUS_AB_RECORD_OFFSET_PENDING 13u
#define AIUS_AB_RECORD_OFFSET_BUDGET 14u
#define AIUS_AB_RECORD_OFFSET_CRC32 28u

/* Two boots, matching the health machinery's default: the same number of
 * chances the recovery boundary has always given an image. */
#define AIUS_AB_DEFAULT_TRIAL_BOOTS 2u
#define AIUS_AB_MAX_TRIAL_BOOTS 8u

typedef struct {
	uint32_t sequence;
	uint8_t active_slot;
	uint8_t pending_slot; /* AIUS_AB_SLOT_NONE when no trial is open */
	uint8_t trial_budget;
} AiusAbRecord;

/* Ledger wire layout, 32 bytes, at AIUS_AB_LEDGER_ADDRESS in retained OCRAM.
 *
 *    0   4  magic "AIAT"
 *    4   4  record_sequence (the flash record this ledger counts for)
 *    8   4  boots_attempted (trial boots the chooser has launched)
 *   12   4  fell_back (0/1: the chooser exhausted the budget and booted
 *              the active slot; the application persists the rollback)
 *   16   1  booted_slot (AIUS_AB_SLOT_*; the PHYSICAL slot the chooser
 *              actually booted -- the app's only way to learn it under a
 *              single canonical build whose address space cannot reveal it.
 *              A pre-slot-agnostic ledger reads 0 = NONE here.)
 *   17  11  reserved = 0
 *   28   4  CRC-32 over bytes [0, 28)
 */
#define AIUS_AB_LEDGER_MAGIC UINT32_C(0x54414941) /* "AIAT" */
#define AIUS_AB_LEDGER_OFFSET_MAGIC 0u
#define AIUS_AB_LEDGER_OFFSET_SEQUENCE 4u
#define AIUS_AB_LEDGER_OFFSET_BOOTS 8u
#define AIUS_AB_LEDGER_OFFSET_FELL_BACK 12u
#define AIUS_AB_LEDGER_OFFSET_BOOTED_SLOT 16u
#define AIUS_AB_LEDGER_OFFSET_CRC32 28u

typedef struct {
	uint32_t record_sequence;
	uint32_t boots_attempted;
	uint32_t fell_back;
	uint8_t booted_slot; /* AIUS_AB_SLOT_*; the physical slot booted */
} AiusAbLedger;

/* Why the boot happened, for the application's status surface (and for the
 * host suite's oracle). Values are wire-visible on /api/status: append only. */
typedef enum {
	AIUS_AB_REASON_NO_RECORD = 0,   /* fresh board / recovery flash: slot A */
	AIUS_AB_REASON_ACTIVE = 1,      /* no trial open: the committed slot */
	AIUS_AB_REASON_TRIAL = 2,       /* trial boot, budget remaining */
	AIUS_AB_REASON_FELL_BACK = 3,   /* budget exhausted: the committed slot */
	AIUS_AB_REASON_INSANE_OTHER = 4 /* chosen slot failed the IVT sanity
	                                 * check; the other one was taken */
} AiusAbReason;

typedef struct {
	uint8_t boot_slot;      /* AIUS_AB_SLOT_A or _B */
	uint8_t reason;         /* AiusAbReason */
	bool ledger_write;      /* the chooser must persist `ledger` */
	AiusAbLedger ledger;    /* the updated ledger when ledger_write */
} AiusAbChoice;

/* Byte codecs. Encode returns false only on null/short buffers; decode is
 * fail-closed (magic, version, length, CRC, slot domains, the
 * pending!=active rule and the budget domain) and leaves the struct zeroed
 * on refusal. */
bool aius_ab_record_encode(const AiusAbRecord *record, uint8_t *bytes,
	                   size_t length);
bool aius_ab_record_decode(AiusAbRecord *record, const uint8_t *bytes,
	                   size_t length);
bool aius_ab_ledger_encode(const AiusAbLedger *ledger, uint8_t *bytes,
	                   size_t length);
bool aius_ab_ledger_decode(AiusAbLedger *ledger, const uint8_t *bytes,
	                   size_t length);

/* The ping-pong pick: decodes both sectors' bytes and selects the valid
 * record with the higher sequence. Returns the winning sector index (0 or
 * 1), or -1 when neither is valid (`record` is then zeroed). Equal
 * sequences prefer sector 0 -- two valid equal-sequence records cannot be
 * written by the single-writer application, so the tie rule only decides a
 * pathological state deterministically. */
int aius_ab_record_pick(const uint8_t *sector0, const uint8_t *sector1,
	                size_t length, AiusAbRecord *record);

/* THE CHOOSER'S DECISION, pure. `record` NULL means no valid record
 * (fail-closed slot A); `ledger` NULL means no valid ledger (a fresh trial
 * count). The IVT sanity check is the CALLER's -- it needs flash reads --
 * and a caller that finds the chosen slot insane takes
 * aius_ab_other_slot() and reports AIUS_AB_REASON_INSANE_OTHER itself. */
void aius_ab_choose(const AiusAbRecord *record, const AiusAbLedger *ledger,
	            AiusAbChoice *choice);

uint8_t aius_ab_other_slot(uint8_t slot);

uint32_t aius_ab_slot_base(uint8_t slot); /* 0 for an invalid slot */

/* The FlexSPI AHB remap inputs (aius-ota-slot-agnostic.md) that make the
 * CANONICAL link window resolve to the chosen PHYSICAL slot: window
 * [AIUS_AB_CANONICAL_BASE, +AIUS_AB_CANONICAL_BYTES) and an offset added to an
 * in-window address to reach the slot's physical flash -- 0 for slot A
 * (identity), +AIUS_AB_SLOT_BYTES for slot B. The chooser writes these to
 * IOMUXC_GPR_GPR30/31/32 before jumping to the canonical entry. Pure and
 * target-agnostic so the host suite pins the arithmetic; the register poke is a
 * tiny target-only shim over this. All fields are 4 KiB-aligned (the remap's
 * granularity). Returns false for an invalid slot (out zeroed). */
typedef struct {
	uint32_t start;  /* GPR30: window start, canonical base */
	uint32_t end;    /* GPR31: window end, EXCLUSIVE */
	uint32_t offset; /* GPR32: added to an in-window address */
} AiusAbRemap;

bool aius_ab_remap_for_slot(uint8_t slot, AiusAbRemap *out);

/* THE IVT SANITY RULE, window form: the two words at an image's
 * +AIUS_AB_SLOT_IVT_OFFSET -- word0 the IVT header (tag 0xD1 in its low byte
 * on this part's byte order), word1 the entry pointer -- judged against the
 * flash window the image must have been LINKED for: [window_base,
 * window_base + window_bytes). The entry must lie strictly inside it (an
 * entry exactly at the base is the FCB sector, never code). An erased sector
 * (0xFFFFFFFF), a torn program, ASCII text or an image linked for another
 * window all fail here. A zero-length window refuses everything: "no window
 * known" fails closed rather than guessing. Pure, so the host suite pins it
 * and both the first-stage chooser (slot form below) and the OTA verify path
 * (lib/aius_ota, against the window the composition configured) share the
 * one rule rather than two copies of the bounds. */
bool aius_ab_ivt_sane_in_window(uint32_t ivt_word0, uint32_t entry_word,
	                        uint32_t window_base, uint32_t window_bytes);

/* The chooser's slot form of the rule over the two words it fetches from a
 * slot's PHYSICAL base + AIUS_AB_SLOT_IVT_OFFSET. Under the slot-agnostic
 * scheme every image is linked at the CANONICAL window, so the entry must lie
 * inside that window regardless of which physical slot the caller read it
 * from (the chooser's remap makes the canonical window resolve to that
 * physical slot). False for an invalid slot id. */
bool aius_ab_slot_ivt_sane(uint8_t slot, uint32_t ivt_word0,
	                   uint32_t entry_word);

#ifdef __cplusplus
}
#endif

#endif
