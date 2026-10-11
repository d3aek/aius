/* The flight recorder's rolling multi-slot bank
 * (docs/plans/aius-recorder-rolling-buffer.md, Phase B): auto-advance on
 * freeze, recycle counting, the read-lease deferral, park() reaching every
 * slot, and the config clamp. The underlying per-ring state machine (27
 * rows in test_recorder.c) is unchanged and not re-proven here -- this
 * suite proves only what the bank wrapper itself adds: which slot is
 * active, and when that changes. */

#include "aius_recorder_bank.h"

#include <assert.h>
#include <string.h>

#define STAGING_SLOTS 4u
#define STAGING_SLOT_FRAMES 8u
#define BANK_RING_FRAMES 1024u
#define BANK_RING_BYTES (BANK_RING_FRAMES * AIUS_RECORDER_FRAME_BYTES)
#define BANK_SLOT_FRAMES (BANK_RING_FRAMES / AIUS_RECORDER_BANK_SLOTS)
#define MIB (1024u * 1024u)
#define DEFAULT_RATE_HZ 48000u

typedef struct {
	AiusRecorderBank bank;
	uint8_t ring[BANK_RING_BYTES];
	int32_t staging[STAGING_SLOTS * STAGING_SLOT_FRAMES * 2u];
	uint32_t staging_frames[STAGING_SLOTS];
} BankFixture;

static void bank_fixture_init(BankFixture *f)
{
	memset(f, 0, sizeof(*f));
	assert(aius_recorder_bank_init(&f->bank, f->ring, BANK_RING_BYTES, 16u,
	                               f->staging, f->staging_frames,
	                               STAGING_SLOTS, STAGING_SLOT_FRAMES,
	                               DEFAULT_RATE_HZ) == AIUS_OK);
	assert(aius_recorder_bank_present(&f->bank));
	assert(f->bank.active == 0u);
}

static void bank_feed_block(BankFixture *f, uint32_t frames, int32_t base,
                            uint32_t overruns, uint32_t underruns)
{
	int32_t l[STAGING_SLOT_FRAMES];
	int32_t r[STAGING_SLOT_FRAMES];
	assert(frames <= STAGING_SLOT_FRAMES);
	for (uint32_t i = 0; i < frames; i++) {
		l[i] = base + (int32_t)i;
		r[i] = -(base + (int32_t)i);
	}
	aius_recorder_bank_producer_block(&f->bank, l, r, frames,
	                             DEFAULT_RATE_HZ, overruns, underruns,
	                             false, 0.0f, 1000u);
}

static void bank_drain_all(BankFixture *f)
{
	while (aius_recorder_bank_drain(&f->bank, 65536u) != 0u)
		;
}

/* Operator freeze -> post-roll -> drain to FROZEN, on the active slot,
 * through the bank API. Postroll is configured to 0 blocks by the caller
 * (via bank_set_config) so one producer block after the freeze ends the
 * capture immediately -- keeps each test's slot-cycling compact. */
static void bank_trigger_and_freeze_active(BankFixture *f)
{
	/* The slot must hold audio: a trigger on an empty slot is dropped. */
	bank_feed_block(f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f->bank) == AIUS_OK);
	bank_feed_block(f, 8u, 0, 0u, 0u); /* acknowledges the token */
	bank_drain_all(f);
}

static void test_bank_absent_is_first_class(void)
{
	AiusRecorderBank bank;
	memset(&bank, 0xAA, sizeof(bank));
	assert(aius_recorder_bank_init(&bank, NULL, 0u, 0u, NULL, NULL, 0u, 0u,
	                               DEFAULT_RATE_HZ) == AIUS_OK);
	assert(!aius_recorder_bank_present(&bank));
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++)
		assert(bank.slots[i].state == AIUS_RECORDER_ABSENT);
	/* Every entry point stays a cheap no-op / clean refusal on a bare
	 * board, same contract as the single-ring core. */
	int32_t plane[8] = { 0 };
	aius_recorder_bank_producer_block(&bank, plane, plane, 8u,
	                             DEFAULT_RATE_HZ, 0u, 0u, false, 0.0f, 0u);
	aius_recorder_bank_epoch(&bank, false, false, 0u, 0u);
	assert(aius_recorder_bank_drain(&bank, 4096u) == 0u);
	assert(aius_recorder_bank_freeze(&bank) == AIUS_INVALID_ARG);
	const uint8_t *out;
	uint32_t out_length;
	assert(aius_recorder_bank_read(&bank, 0u, 0u, 8u, 0u, &out,
	                               &out_length) == AIUS_INVALID_ARG);
}

static void test_bank_init_geometry_every_fitment(void)
{
	const uint32_t staging_bytes = 32u * 1024u;
	/* Contract: total ring bytes across the bank is exactly what the
	 * single-ring geometry formula would have produced -- the OTA pad-1
	 * capacity contract does not change shape. Geometry-only: init never
	 * dereferences `ring`, so a non-null placeholder pointer is sound
	 * here (no producer_block/drain call follows). */
	uint8_t *placeholder = (uint8_t *)(uintptr_t)0x1000;
	int32_t *staging_placeholder = (int32_t *)(uintptr_t)0x2000;
	uint32_t *staging_frames_placeholder = (uint32_t *)(uintptr_t)0x3000;
	AiusRecorderBank bank;

	uint32_t ring_16mb = aius_recorder_ring_bytes_after_staging(
		16u * MIB, staging_bytes);
	assert(ring_16mb == 8u * MIB);
	assert(aius_recorder_bank_init(&bank, placeholder, ring_16mb, 16u,
	                               staging_placeholder,
	                               staging_frames_placeholder,
	                               STAGING_SLOTS, STAGING_SLOT_FRAMES,
	                               DEFAULT_RATE_HZ) == AIUS_OK);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++)
		assert(bank.slots[i].ring_frames ==
		       ring_16mb / AIUS_RECORDER_FRAME_BYTES /
		               AIUS_RECORDER_BANK_SLOTS);

	uint32_t ring_8mb = aius_recorder_ring_bytes_after_staging(
		8u * MIB, staging_bytes);
	assert(ring_8mb == 4u * MIB);
	assert(aius_recorder_bank_init(&bank, placeholder, ring_8mb, 8u,
	                               staging_placeholder,
	                               staging_frames_placeholder,
	                               STAGING_SLOTS, STAGING_SLOT_FRAMES,
	                               DEFAULT_RATE_HZ) == AIUS_OK);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++)
		assert(bank.slots[i].ring_frames ==
		       ring_8mb / AIUS_RECORDER_FRAME_BYTES /
		               AIUS_RECORDER_BANK_SLOTS);

	uint32_t ring_0mb =
		aius_recorder_ring_bytes_after_staging(0u, staging_bytes);
	assert(ring_0mb == 0u);
	assert(aius_recorder_bank_init(&bank, placeholder, ring_0mb, 0u,
	                               staging_placeholder,
	                               staging_frames_placeholder,
	                               STAGING_SLOTS, STAGING_SLOT_FRAMES,
	                               DEFAULT_RATE_HZ) == AIUS_OK);
	assert(!aius_recorder_bank_present(&bank));
}

static void test_bank_forwards_full_lifecycle(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 2u) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f.bank) == AIUS_OK);
	bank_feed_block(&f, 8u, 1, 0u, 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_POSTROLL);
	bank_feed_block(&f, 8u, 2, 0u, 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_DRAINING);
	bank_drain_all(&f);
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	const uint8_t *out;
	uint32_t out_length;
	assert(aius_recorder_bank_read(&f.bank, 0u, 0u, 24u * 8u, 0u, &out,
	                               &out_length) == AIUS_OK);
	assert(out_length == 24u * AIUS_RECORDER_FRAME_BYTES);
}

/* A producer-posted note reaches the ACTIVE slot only, exactly as the producer
 * block and the epoch do: the slot being recorded is the one the splice belongs
 * to, and an idle slot must not latch a trigger it never saw a block for. */
static void test_bank_forwards_governor_trim_to_active_only(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 2u) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u); /* an empty slot drops the note */
	aius_recorder_bank_note_producer_event(
		&f.bank, AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	assert(f.bank.slots[f.bank.active].freeze_token ==
	       AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	for (uint32_t slot = 0; slot < AIUS_RECORDER_BANK_SLOTS; slot++) {
		if (slot == f.bank.active)
			continue;
		assert(f.bank.slots[slot].freeze_token == 0u);
	}
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.bank.slots[f.bank.active].state == AIUS_RECORDER_POSTROLL);
	assert(f.bank.slots[f.bank.active].trigger ==
	       AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	/* A null bank is a no-op, like every other bank entry point. */
	aius_recorder_bank_note_producer_event(
		NULL, AIUS_RECORDER_TRIGGER_MISSED_FILL);
}

static void test_bank_auto_advance_on_freeze(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	/* Auto-advanced with no thaw call -- the virgin next slot needed no
	 * reset. */
	assert(f.bank.active == 1u);
	assert(f.bank.slots[1].state == AIUS_RECORDER_RECORDING);
	assert(f.bank.slots_recycled == 0u);
}

static void test_bank_full_wrap_recycles_and_counts(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS - 1u; i++) {
		bank_trigger_and_freeze_active(&f);
		assert(f.bank.slots_recycled == 0u);
	}
	assert(f.bank.active == AIUS_RECORDER_BANK_SLOTS - 1u);
	uint32_t slot0_generation_before = f.bank.slots[0].freeze_generation;
	/* The Nth freeze wraps onto slot 0, still FROZEN from the first
	 * capture: a genuine recycle. */
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.slots_recycled == 1u);
	assert(f.bank.active == 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_RECORDING);
	assert(f.bank.slots[0].freeze_generation == slot0_generation_before + 1u);
}

static void test_bank_thaw_in_place_does_not_advance(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	/* Hold a lease on slot 1 so auto-advance defers, leaving slot 0
	 * genuinely FROZEN (not already moved on) for the operator thaw to
	 * act on. */
	aius_recorder_bank_read_acquire(&f.bank, 1u);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f.bank) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	bank_drain_all(&f);
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	assert(f.bank.active == 0u);
	uint32_t generation_before = f.bank.slots[0].freeze_generation;
	/* Operator thaw: the SAME slot re-arms, not the next one -- unlike
	 * auto-advance, this never moves `active`. */
	assert(aius_recorder_bank_thaw(&f.bank) == AIUS_OK);
	assert(f.bank.active == 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_RECORDING);
	assert(f.bank.slots[0].freeze_generation == generation_before + 1u);
	/* Not FROZEN (already thawed): a second call refuses, same contract
	 * as the single-ring core. */
	assert(aius_recorder_bank_thaw(&f.bank) == AIUS_INVALID_ARG);
	aius_recorder_bank_read_release(&f.bank, 1u);
}

static void test_bank_recycle_deferred_while_read_leased(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	/* Hold a lease on slot 1 -- the slot the very first freeze would
	 * advance into. */
	aius_recorder_bank_read_acquire(&f.bank, 1u);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f.bank) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_DRAINING);
	bank_drain_all(&f);
	/* Drained to FROZEN, but the advance is deferred: the lease on the
	 * target slot is still held. Nothing records while deferred -- the
	 * accepted correctness-over-liveness trade the plan documents. */
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	assert(f.bank.active == 0u);
	assert(f.bank.slots[1].state == AIUS_RECORDER_RECORDING);
	/* Release the lease: the next drain() call completes the advance. */
	aius_recorder_bank_read_release(&f.bank, 1u);
	assert(aius_recorder_bank_drain(&f.bank, 4096u) == 0u);
	assert(f.bank.active == 1u);
}

/* Epoch history follows the STREAM, not the slot. The bank forwards epochs to
 * the active slot only, so a slot re-armed mid-session would otherwise
 * edge-detect the carrier against its own stale past: a slot last active while
 * LOCKED keeps carrier_prev == 1 and fires a spurious CARRIER_LOSS on its
 * first epoch under a carrier that has been gone for seconds. */
static void test_bank_recycled_slot_inherits_carrier_state(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	/* Slot 0 records while the carrier is locked, then freezes. */
	aius_recorder_bank_epoch(&f.bank, true, true, 0u, 0u);
	assert(f.bank.slots[0].carrier_prev == 1u);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 1u);
	/* The carrier goes away while slot 1 is active: ONE genuine falling
	 * edge, taken by the slot that was there for it (holding audio: an
	 * empty slot would drop it). */
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	aius_recorder_bank_epoch(&f.bank, false, true, 0u, 1u);
	assert(f.bank.slots[1].freeze_token ==
	       AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
	bank_feed_block(&f, 8u, 0, 0u, 0u); /* acknowledges the token */
	bank_drain_all(&f);
	assert(f.bank.active == 2u);
	/* Slots 2 and 3 see the unlocked level, never an edge. */
	aius_recorder_bank_epoch(&f.bank, false, true, 0u, 2u);
	assert(f.bank.slots[2].freeze_token == 0u);
	bank_trigger_and_freeze_active(&f);
	aius_recorder_bank_epoch(&f.bank, false, true, 0u, 3u);
	assert(f.bank.slots[3].freeze_token == 0u);
	/* The wrap lands back on slot 0, whose own history still says "locked"
	 * from the top of this test. It inherits the stream's answer instead. */
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 0u);
	assert(f.bank.slots_recycled == 1u);
	assert(f.bank.slots[0].carrier_prev == 0u);
	aius_recorder_bank_epoch(&f.bank, false, true, 0u, 4u);
	assert(f.bank.slots[0].freeze_token == 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_RECORDING);
}

/* The mirror of the same defect: a VIRGIN slot's carrier_prev is 0, so the
 * first epoch after an advance would read a real falling edge as a level and
 * drop it -- the recorder blind exactly at the handover. */
static void test_bank_advance_captures_first_epoch_carrier_edge(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	aius_recorder_bank_epoch(&f.bank, true, true, 0u, 0u);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 1u);
	assert(f.bank.slots[1].carrier_prev == 1u);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	aius_recorder_bank_epoch(&f.bank, false, true, 0u, 1u);
	assert(f.bank.slots[1].freeze_token ==
	       AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
}

/* Phase E: switching the source off flaps the lock on the fresh, empty slot
 * the bank advanced to. Each edge is dropped and counted; the earlier frozen
 * capture is never recycled. */
static void test_bank_empty_carrier_loss_never_advances(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	assert(aius_recorder_bank_empty_triggers_dropped(&f.bank) == 0u);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 1u);
	uint32_t generation = f.bank.slots[0].freeze_generation;

	/* Three lock flaps (lock, then loss) on the empty slot 1. */
	for (uint32_t i = 0; i < 3u; i++) {
		aius_recorder_bank_epoch(&f.bank, true, true, 0u, 10u * i);
		aius_recorder_bank_epoch(&f.bank, false, true, 0u, 10u * i + 1u);
		assert(f.bank.slots[1].freeze_token == 0u);
		bank_drain_all(&f);
		assert(f.bank.active == 1u);
		assert(aius_recorder_bank_empty_triggers_dropped(&f.bank) ==
		       i + 1u);
	}
	assert(f.bank.slots[1].state == AIUS_RECORDER_RECORDING);
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	assert(f.bank.slots[0].freeze_generation == generation);
	assert(f.bank.slots_recycled == 0u);

	/* A carrier loss on a slot holding audio still freezes, as before. */
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	aius_recorder_bank_epoch(&f.bank, true, true, 0u, 100u);
	aius_recorder_bank_epoch(&f.bank, false, true, 0u, 101u);
	assert(f.bank.slots[1].freeze_token ==
	       AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	bank_drain_all(&f);
	assert(f.bank.slots[1].state == AIUS_RECORDER_FROZEN);
	assert(f.bank.active == 2u);
	assert(aius_recorder_bank_empty_triggers_dropped(&f.bank) == 3u);
}

/* An operator thaw recycles the same ring bytes drain()'s deferral protects,
 * so it owes the read lease the same respect. */
static void test_bank_thaw_refused_while_active_slot_leased(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	/* Lease slot 1 so the freeze leaves slot 0 FROZEN and still active. */
	aius_recorder_bank_read_acquire(&f.bank, 1u);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	/* A pull is streaming from slot 0 itself: refused, and distinctly from
	 * the not-frozen refusal. */
	aius_recorder_bank_read_acquire(&f.bank, 0u);
	assert(aius_recorder_bank_thaw(&f.bank) == AIUS_REFUSED);
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	/* Released: the same call now succeeds. */
	aius_recorder_bank_read_release(&f.bank, 0u);
	assert(aius_recorder_bank_thaw(&f.bank) == AIUS_OK);
	assert(f.bank.slots[0].state == AIUS_RECORDER_RECORDING);
	aius_recorder_bank_read_release(&f.bank, 1u);
}

/* The deferral is bounded, not indefinite: a stuck reader must not stop the
 * recorder for the rest of the session. After the reclaim-sized budget the
 * advance skips PAST the leased slot, leaving that capture intact. */
static void test_bank_lease_deferral_is_bounded(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	aius_recorder_bank_read_acquire(&f.bank, 1u);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 0u);
	assert(aius_recorder_bank_deferred_epochs(&f.bank) >= 1u);
	assert(aius_recorder_bank_lease_skips(&f.bank) == 0u);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_DEFER_DRAINS; i++)
		(void)aius_recorder_bank_drain(&f.bank, 4096u);
	assert(f.bank.active == 2u);
	assert(aius_recorder_bank_lease_skips(&f.bank) == 1u);
	assert(aius_recorder_bank_deferred_epochs(&f.bank) >=
	       AIUS_RECORDER_BANK_DEFER_DRAINS);
	/* The leased slot was left alone -- skipped, not recycled. */
	assert(f.bank.slots[1].state == AIUS_RECORDER_RECORDING);
	assert(f.bank.reads_in_flight[1] == 1u);
	aius_recorder_bank_read_release(&f.bank, 1u);
}

/* With every candidate leased there is nowhere to skip to: the bank keeps
 * deferring and keeps counting, and takes the first slot that frees up. */
static void test_bank_lease_deferral_when_every_slot_leased(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	for (uint32_t slot = 1u; slot < AIUS_RECORDER_BANK_SLOTS; slot++)
		aius_recorder_bank_read_acquire(&f.bank, slot);
	bank_trigger_and_freeze_active(&f);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_DEFER_DRAINS + 2u; i++)
		(void)aius_recorder_bank_drain(&f.bank, 4096u);
	assert(f.bank.active == 0u);
	assert(aius_recorder_bank_lease_skips(&f.bank) == 0u);
	assert(aius_recorder_bank_deferred_epochs(&f.bank) >
	       AIUS_RECORDER_BANK_DEFER_DRAINS);
	/* Slot 2 frees up while slots 1 and 3 stay held: the next drain skips
	 * to it. */
	aius_recorder_bank_read_release(&f.bank, 2u);
	(void)aius_recorder_bank_drain(&f.bank, 4096u);
	assert(f.bank.active == 2u);
	assert(aius_recorder_bank_lease_skips(&f.bank) == 1u);
	aius_recorder_bank_read_release(&f.bank, 1u);
	aius_recorder_bank_read_release(&f.bank, 3u);
}

/* A slot mid-capture refuses a reconfigure; the bank replays it when that
 * slot re-arms, so an operator's change is not silently undone by whichever
 * capture happened to be in flight. */
static void test_bank_pending_config_applies_on_recycle(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 2u) == AIUS_OK);
	assert(!aius_recorder_bank_config_pending(&f.bank));
	/* Slot 0 into POSTROLL, then reconfigure: it keeps its own config. */
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f.bank) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_POSTROLL);
	assert(aius_recorder_bank_set_config(&f.bank,
	                                     AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                     1u, 0u) == AIUS_OK);
	assert(aius_recorder_bank_config_pending(&f.bank));
	AiusRecorderSnapshot s;
	assert(aius_recorder_bank_snapshot_slot(&f.bank, 0u, &s) == AIUS_OK);
	assert(s.postroll_blocks == 2u);
	assert(s.trigger_mask == AIUS_RECORDER_MASK_ALL);
	assert(aius_recorder_bank_snapshot_slot(&f.bank, 1u, &s) == AIUS_OK);
	assert(s.postroll_blocks == 0u);
	/* Finish that capture and cycle the ring back onto slot 0. */
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	bank_drain_all(&f);
	assert(f.bank.active == 1u);
	assert(aius_recorder_bank_config_pending(&f.bank));
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS - 1u; i++)
		bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 0u);
	assert(!aius_recorder_bank_config_pending(&f.bank));
	assert(aius_recorder_bank_snapshot_slot(&f.bank, 0u, &s) == AIUS_OK);
	assert(s.postroll_blocks == 0u);
	assert(s.trigger_mask == AIUS_RECORDER_MASK_DEFAULT);
}

static void test_bank_pending_config_applies_on_thaw(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 2u) == AIUS_OK);
	/* Lease slot 1 so the freeze leaves slot 0 FROZEN and active, with the
	 * operator thaw as its only way back. */
	aius_recorder_bank_read_acquire(&f.bank, 1u);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f.bank) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_set_config(&f.bank,
	                                     AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                     1u, 0u) == AIUS_OK);
	assert(aius_recorder_bank_config_pending(&f.bank));
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	bank_drain_all(&f);
	assert(f.bank.active == 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	assert(aius_recorder_bank_thaw(&f.bank) == AIUS_OK);
	assert(!aius_recorder_bank_config_pending(&f.bank));
	AiusRecorderSnapshot s;
	assert(aius_recorder_bank_snapshot_slot(&f.bank, 0u, &s) == AIUS_OK);
	assert(s.postroll_blocks == 0u);
	assert(s.trigger_mask == AIUS_RECORDER_MASK_DEFAULT);
	aius_recorder_bank_read_release(&f.bank, 1u);
}

/* The blindness window the bank's header states: the incoming slot re-arms
 * with an empty settle window, so the underrun trigger cannot fire for
 * AIUS_RECORDER_SETTLE_BLOCKS clean blocks after every advance. Pinned so it
 * stays a stated contract rather than an accident. */
static void test_bank_underrun_blind_window_after_advance(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	aius_recorder_bank_epoch(&f.bank, false, true, 0u, 0u);
	for (uint32_t i = 0; i < AIUS_RECORDER_SETTLE_BLOCKS; i++) {
		bank_feed_block(&f, 8u, 0, 0u, 0u);
		bank_drain_all(&f);
	}
	assert(f.bank.slots[0].settled_blocks == AIUS_RECORDER_SETTLE_BLOCKS);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 1u);
	/* The incoming slot starts its own window at zero -- and inherits the
	 * open gate, so nothing else is holding the trigger off. */
	assert(f.bank.slots[1].settled_blocks == 0u);
	assert(f.bank.slots[1].gate_open == 1u);
	bank_feed_block(&f, 8u, 0, 0u, 0u); /* adopts the baselines */
	bank_drain_all(&f);
	bank_feed_block(&f, 8u, 0, 0u, 5000u); /* a real burst, not captured */
	bank_drain_all(&f);
	assert(f.bank.slots[1].state == AIUS_RECORDER_RECORDING);
	/* It is a window, not a disabled trigger: once it closes the same delta
	 * fires. */
	for (uint32_t i = 0; i < AIUS_RECORDER_SETTLE_BLOCKS; i++) {
		bank_feed_block(&f, 8u, 0, 0u, 5000u);
		bank_drain_all(&f);
	}
	bank_feed_block(&f, 8u, 0, 0u, 6000u);
	assert(f.bank.slots[1].trigger == AIUS_RECORDER_TRIGGER_UNDERRUN);
}

/* "Read the active slot" always hits a RECORDING one, because the advance
 * happens inside the same drain() that freezes. The bank tracks the newest
 * freeze so a reader has an honest default. */
static void test_bank_newest_frozen_tracks_and_falls_back(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	uint32_t slot = 99u;
	assert(!aius_recorder_bank_newest_frozen(&f.bank, &slot));
	bank_trigger_and_freeze_active(&f);
	assert(aius_recorder_bank_newest_frozen(&f.bank, &slot));
	assert(slot == 0u);
	assert(f.bank.active == 1u);
	/* Lease slot 2 so the second freeze stays put, leaving two frozen
	 * slots and the newest one thawable in place. */
	aius_recorder_bank_read_acquire(&f.bank, 2u);
	bank_trigger_and_freeze_active(&f);
	assert(f.bank.active == 1u);
	assert(aius_recorder_bank_newest_frozen(&f.bank, &slot));
	assert(slot == 1u);
	/* The tracked slot leaves FROZEN: the answer falls back to the frozen
	 * slot with the highest freeze generation. */
	assert(aius_recorder_bank_thaw(&f.bank) == AIUS_OK);
	assert(aius_recorder_bank_newest_frozen(&f.bank, &slot));
	assert(slot == 0u);
	/* Nothing frozen at all (a parked bank is the extreme case): no answer,
	 * and the caller's slot is left alone. */
	assert(aius_recorder_bank_park(&f.bank) == AIUS_OK);
	slot = 99u;
	assert(!aius_recorder_bank_newest_frozen(&f.bank, &slot));
	assert(slot == 99u);
	aius_recorder_bank_read_release(&f.bank, 2u);
}

static void test_bank_park_reaches_every_slot(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	bank_trigger_and_freeze_active(&f); /* slot 0 -> FROZEN, active -> 1 */
	assert(f.bank.slots[0].state == AIUS_RECORDER_FROZEN);
	assert(f.bank.active == 1u);
	/* Reconfigure with a real post-roll so the next capture parks
	 * mid-POSTROLL, not FROZEN -- park() must reach both a frozen slot
	 * and a not-yet-drained one, not just the active slot. */
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 2u) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f.bank) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u); /* slot 1 -> POSTROLL (1 of 2) */
	assert(f.bank.slots[1].state == AIUS_RECORDER_POSTROLL);
	assert(aius_recorder_bank_park(&f.bank) == AIUS_OK);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++)
		assert(f.bank.slots[i].state == AIUS_RECORDER_ABSENT);
}

static void test_bank_park_mid_draining(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 0u) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_bank_freeze(&f.bank) == AIUS_OK);
	bank_feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.bank.slots[0].state == AIUS_RECORDER_DRAINING);
	assert(aius_recorder_bank_park(&f.bank) == AIUS_OK);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++)
		assert(f.bank.slots[i].state == AIUS_RECORDER_ABSENT);
}

static void test_bank_set_config_reaches_every_slot_and_clamps(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	/* BANK_SLOT_FRAMES (256) is below one production block
	 * (AIUS_RECORDER_BANK_BLOCK_FRAMES, 512), so the capacity clamp's
	 * divide-by-block-size floors to 0 -- the clamp deliberately declines
	 * to act rather than clamp to a nonsensical 0 (see the header). A
	 * postroll under AIUS_RECORDER_POSTROLL_MAX_BLOCKS still reaches
	 * every slot unclamped by the capacity guard. */
	assert(aius_recorder_bank_set_config(&f.bank, AIUS_RECORDER_MASK_ALL,
	                                     -6.0f, 3u, 100u) == AIUS_OK);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
		AiusRecorderSnapshot s;
		assert(aius_recorder_bank_snapshot_slot(&f.bank, i, &s) ==
		       AIUS_OK);
		assert(s.trigger_mask == AIUS_RECORDER_MASK_ALL);
		assert(s.postroll_blocks == 100u);
		assert(s.gr_hold_blocks == 3u);
	}
}

static void test_bank_set_config_clamps_to_slot_capacity(void)
{
	/* A dedicated larger fixture: BANK_SLOT_FRAMES here (4096) exceeds
	 * AIUS_RECORDER_BANK_BLOCK_FRAMES (512), so the capacity clamp has
	 * something real to clamp against -- 8 blocks per slot. */
	enum { CAP_SLOTS_TOTAL_FRAMES = 4096u * AIUS_RECORDER_BANK_SLOTS };
	static uint8_t ring[CAP_SLOTS_TOTAL_FRAMES * AIUS_RECORDER_FRAME_BYTES];
	static int32_t staging[STAGING_SLOTS * STAGING_SLOT_FRAMES * 2u];
	static uint32_t staging_frames[STAGING_SLOTS];
	AiusRecorderBank bank;
	assert(aius_recorder_bank_init(
		       &bank, ring, sizeof(ring), 16u, staging, staging_frames,
		       STAGING_SLOTS, STAGING_SLOT_FRAMES,
		       DEFAULT_RATE_HZ) == AIUS_OK);
	assert(bank.slots[0].ring_frames == 4096u);
	assert(aius_recorder_bank_set_config(&bank, AIUS_RECORDER_MASK_ALL,
	                                     0.0f, 1u, 500u) == AIUS_OK);
	AiusRecorderSnapshot s;
	assert(aius_recorder_bank_snapshot_slot(&bank, 0u, &s) == AIUS_OK);
	assert(s.postroll_blocks ==
	       4096u / AIUS_RECORDER_BANK_BLOCK_FRAMES); /* clamped to 8 */
}

/* The DEFAULT post-roll needs the capacity clamp as much as an operator's
 * does: a bank of short slots used to arm with 188 blocks of post-roll it
 * could not hold, and only a set_config call would ever have corrected it. */
static void test_bank_init_clamps_default_postroll(void)
{
	enum { INIT_CLAMP_TOTAL_FRAMES = 4096u * AIUS_RECORDER_BANK_SLOTS };
	static uint8_t ring[INIT_CLAMP_TOTAL_FRAMES *
	                    AIUS_RECORDER_FRAME_BYTES];
	static int32_t staging[STAGING_SLOTS * STAGING_SLOT_FRAMES * 2u];
	static uint32_t staging_frames[STAGING_SLOTS];
	AiusRecorderBank bank;
	assert(AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS >
	       4096u / AIUS_RECORDER_BANK_BLOCK_FRAMES);
	assert(aius_recorder_bank_init(&bank, ring, sizeof(ring), 16u, staging,
	                               staging_frames, STAGING_SLOTS,
	                               STAGING_SLOT_FRAMES,
	                               DEFAULT_RATE_HZ) == AIUS_OK);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
		AiusRecorderSnapshot s;
		assert(aius_recorder_bank_snapshot_slot(&bank, i, &s) ==
		       AIUS_OK);
		assert(s.postroll_blocks ==
		       4096u / AIUS_RECORDER_BANK_BLOCK_FRAMES);
	}
}

static void test_bank_read_bounds(void)
{
	BankFixture f;
	bank_fixture_init(&f);
	const uint8_t *out;
	uint32_t out_length;
	assert(aius_recorder_bank_read(&f.bank, AIUS_RECORDER_BANK_SLOTS, 0u,
	                               8u, 0u, &out,
	                               &out_length) == AIUS_INVALID_ARG);
	/* A RECORDING (not FROZEN) slot refuses, delegated verbatim. */
	assert(aius_recorder_bank_read(&f.bank, 0u, 0u, 8u, 0u, &out,
	                               &out_length) == AIUS_INVALID_ARG);
}

int main(void)
{
	test_bank_absent_is_first_class();
	test_bank_init_geometry_every_fitment();
	test_bank_forwards_full_lifecycle();
	test_bank_forwards_governor_trim_to_active_only();
	test_bank_auto_advance_on_freeze();
	test_bank_full_wrap_recycles_and_counts();
	test_bank_thaw_in_place_does_not_advance();
	test_bank_recycle_deferred_while_read_leased();
	test_bank_recycled_slot_inherits_carrier_state();
	test_bank_advance_captures_first_epoch_carrier_edge();
	test_bank_empty_carrier_loss_never_advances();
	test_bank_thaw_refused_while_active_slot_leased();
	test_bank_lease_deferral_is_bounded();
	test_bank_lease_deferral_when_every_slot_leased();
	test_bank_pending_config_applies_on_recycle();
	test_bank_pending_config_applies_on_thaw();
	test_bank_underrun_blind_window_after_advance();
	test_bank_newest_frozen_tracks_and_falls_back();
	test_bank_park_reaches_every_slot();
	test_bank_park_mid_draining();
	test_bank_set_config_reaches_every_slot_and_clamps();
	test_bank_set_config_clamps_to_slot_capacity();
	test_bank_init_clamps_default_postroll();
	test_bank_read_bounds();
	return 0;
}
