#include "aius_recorder_bank.h"

#include <math.h>
#include <string.h>

/* A post-roll longer than a slot can physically hold is nonsensical once
 * slots shrink from one long ring to N shorter ones -- clamp against slot
 * capacity in the production block size, on top of aius_recorder_set_config's
 * own AIUS_RECORDER_POSTROLL_MAX_BLOCKS clamp. Every slot has the same
 * ring_frames (bank_init partitions the total evenly), so slot 0 is
 * representative. A slot shorter than one production block floors the divide
 * to 0, where the clamp deliberately declines to act rather than clamp to a
 * nonsensical zero post-roll (native fixtures live here; the target does
 * not). */
static uint32_t bank_clamp_postroll(const AiusRecorderBank *bank,
                                    uint32_t postroll_blocks)
{
	uint32_t slot_capacity_blocks =
		bank->slots[0].ring_frames / AIUS_RECORDER_BANK_BLOCK_FRAMES;
	if (slot_capacity_blocks != 0u && postroll_blocks > slot_capacity_blocks)
		return slot_capacity_blocks;
	return postroll_blocks;
}

/* Replay a config a slot refused while it was mid-capture, at the moment it
 * re-arms. Silent no-op for a slot with nothing pending, which is the normal
 * case on every advance. */
static void bank_apply_pending_config(AiusRecorderBank *bank, uint32_t slot)
{
	if (bank->config_pending_slots[slot] == 0u)
		return;
	if (aius_recorder_set_config(&bank->slots[slot], bank->pending_mask,
	                             bank->pending_gr_threshold_db,
	                             bank->pending_gr_hold_blocks,
	                             bank->pending_postroll_blocks) == AIUS_OK)
		bank->config_pending_slots[slot] = 0u;
}

AiusStatus aius_recorder_bank_init(AiusRecorderBank *bank, uint8_t *ring,
                                   uint32_t ring_bytes, uint32_t fitted_mb,
                                   int32_t *staging, uint32_t *staging_frames,
                                   uint32_t staging_slots,
                                   uint32_t slot_frames,
                                   uint32_t default_sample_rate_hz)
{
	if (bank == NULL)
		return AIUS_INVALID_ARG;
	memset(bank, 0, sizeof(*bank));
	if (ring_bytes == 0u) {
		/* The bare-board case: every slot reports absent, matching
		 * aius_recorder_init's own "not an error" contract. */
		for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
			AiusStatus status = aius_recorder_init(
				&bank->slots[i], NULL, 0u, 0u, NULL, NULL, 0u,
				0u, default_sample_rate_hz);
			if (status != AIUS_OK)
				return status;
		}
		return AIUS_OK;
	}
	if (ring == NULL || ring_bytes % AIUS_RECORDER_BANK_SLOTS != 0u)
		return AIUS_INVALID_ARG;
	uint32_t slot_bytes = ring_bytes / AIUS_RECORDER_BANK_SLOTS;
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
		/* Per-slot init enforces the power-of-two frame-count geometry
		 * itself; a non-power-of-two slot_bytes refuses here rather
		 * than needing a duplicate check in this function. */
		AiusStatus status = aius_recorder_init(
			&bank->slots[i], ring + (size_t)i * slot_bytes,
			slot_bytes, fitted_mb, staging, staging_frames,
			staging_slots, slot_frames, default_sample_rate_hz);
		if (status != AIUS_OK)
			return status;
	}
	/* The DEFAULT post-roll needs the slot-capacity clamp as much as an
	 * operator's does -- a bank of short slots would otherwise arm with a
	 * post-roll longer than a slot can hold and only get clamped if someone
	 * happened to call set_config. Re-installing each slot's own freshly
	 * installed defaults keeps this free of a second copy of what those
	 * defaults are. */
	uint32_t postroll = bank_clamp_postroll(bank,
	                                        bank->slots[0].postroll_blocks);
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++)
		(void)aius_recorder_set_config(&bank->slots[i],
		                               bank->slots[i].trigger_mask,
		                               bank->slots[i].gr_threshold_db,
		                               bank->slots[i].gr_hold_blocks,
		                               postroll);
	return AIUS_OK;
}

bool aius_recorder_bank_present(const AiusRecorderBank *bank)
{
	return bank->slots[0].state != AIUS_RECORDER_ABSENT;
}

void aius_recorder_bank_producer_block(AiusRecorderBank *bank,
                                       const int32_t *l, const int32_t *right,
                                       uint32_t frames,
                                       uint32_t sample_rate_hz,
                                       uint32_t overruns, uint32_t underruns,
                                       bool limiter_gain_valid,
                                       float limiter_gain, uint32_t now_ms)
{
	aius_recorder_producer_block(&bank->slots[bank->active], l, right,
	                        frames, sample_rate_hz, overruns, underruns,
	                        limiter_gain_valid, limiter_gain, now_ms);
}

void aius_recorder_bank_epoch(AiusRecorderBank *bank, bool carrier_locked,
                              bool gate_open, uint32_t underruns,
                              uint32_t now_ms)
{
	aius_recorder_epoch(&bank->slots[bank->active], carrier_locked,
	                    gate_open, underruns, now_ms);
}

uint32_t aius_recorder_bank_drain(AiusRecorderBank *bank, uint32_t max_bytes)
{
	AiusRecorder *active = &bank->slots[bank->active];
	uint32_t drained = aius_recorder_drain(active, max_bytes);
	if (active->state != AIUS_RECORDER_FROZEN)
		return drained;

	/* The freeze just observed is the newest one by construction: this is
	 * the only place a slot reaches FROZEN. Re-stating it on a deferred
	 * drain is harmless (same slot, same fact). */
	bank->newest_frozen = bank->active;
	bank->newest_frozen_valid = true;

	uint32_t next = (bank->active + 1u) % AIUS_RECORDER_BANK_SLOTS;
	if (bank->reads_in_flight[next] != 0u) {
		bank->deferred_epochs++;
		if (bank->defer_held < AIUS_RECORDER_RECLAIM_EPOCHS) {
			/* Inside the deferral budget: wait for the reader,
			 * retried on the next drain() call. Nothing records
			 * meanwhile -- correctness over liveness, but bounded
			 * by the same window the reclaim wait uses. */
			bank->defer_held++;
			return drained;
		}
		/* Budget spent. Rather than stall the recorder indefinitely
		 * behind one slow (or stuck) response, skip PAST the leased
		 * slot to the first candidate that is not leased: the leased
		 * capture stays intact for its reader, and recording resumes
		 * one slot further on. The active slot itself is never a
		 * candidate (it holds the capture just frozen). */
		bool found = false;
		for (uint32_t step = 2u; step < AIUS_RECORDER_BANK_SLOTS;
		     step++) {
			uint32_t index = (bank->active + step) %
			                 AIUS_RECORDER_BANK_SLOTS;
			if (bank->reads_in_flight[index] != 0u)
				continue;
			next = index;
			found = true;
			break;
		}
		if (!found)
			return drained; /* every candidate leased: keep waiting */
		bank->lease_skips++;
	}
	bank->defer_held = 0u;

	if (bank->slots[next].state == AIUS_RECORDER_FROZEN) {
		/* The ring has wrapped onto a slot an operator may not have
		 * pulled yet -- recycle it and count the wrap honestly. */
		bank->slots_recycled++;
		(void)aius_recorder_thaw(&bank->slots[next]);
	}
	/* A slot that has never triggered is still in its virgin RECORDING
	 * state from init and needs no reset -- but virgin or recycled, its
	 * epoch-side edge history is its own stale past (or nothing at all),
	 * and it is about to be the only slot seeing epochs. Hand it the
	 * outgoing slot's, which is this epoch's truth. */
	aius_recorder_inherit_epoch_state(&bank->slots[next], active);
	bank_apply_pending_config(bank, next);
	bank->active = next;
	return drained;
}

AiusStatus aius_recorder_bank_freeze(AiusRecorderBank *bank)
{
	if (bank == NULL)
		return AIUS_INVALID_ARG;
	return aius_recorder_freeze(&bank->slots[bank->active]);
}

AiusStatus aius_recorder_bank_thaw(AiusRecorderBank *bank)
{
	if (bank == NULL)
		return AIUS_INVALID_ARG;
	uint32_t active = bank->active;
	/* An operator thaw recycles the same ring bytes drain()'s deferral
	 * protects: refuse while a response is still streaming from them. The
	 * caller retries when the pull completes. */
	if (bank->reads_in_flight[active] != 0u)
		return AIUS_REFUSED;
	AiusStatus status = aius_recorder_thaw(&bank->slots[active]);
	if (status == AIUS_OK)
		bank_apply_pending_config(bank, active);
	return status;
}

AiusStatus aius_recorder_bank_park(AiusRecorderBank *bank)
{
	if (bank == NULL)
		return AIUS_INVALID_ARG;
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
		(void)aius_recorder_park(&bank->slots[i]);
		/* One-way to ABSENT: no slot will ever re-arm to collect a
		 * pending config, so nothing is pending any more. */
		bank->config_pending_slots[i] = 0u;
	}
	return AIUS_OK;
}

bool aius_recorder_bank_newest_frozen(const AiusRecorderBank *bank,
                                      uint32_t *slot)
{
	if (bank == NULL || slot == NULL)
		return false;
	if (bank->newest_frozen_valid &&
	    bank->slots[bank->newest_frozen].state == AIUS_RECORDER_FROZEN) {
		*slot = bank->newest_frozen;
		return true;
	}
	/* The tracked slot has left FROZEN. Fall back to the frozen slot with
	 * the highest freeze_generation -- the most recently thawed, hence the
	 * most recently re-frozen. Walking backwards from the active slot makes
	 * the scan newest-first in ring order, so a generation tie (the normal
	 * case: a round-robin cycles every slot at the same rate) resolves to
	 * the one frozen most recently. */
	bool found = false;
	uint32_t best = 0u;
	uint32_t best_generation = 0u;
	for (uint32_t back = 1u; back <= AIUS_RECORDER_BANK_SLOTS; back++) {
		uint32_t index = (bank->active + AIUS_RECORDER_BANK_SLOTS -
		                  back) % AIUS_RECORDER_BANK_SLOTS;
		if (bank->slots[index].state != AIUS_RECORDER_FROZEN)
			continue;
		uint32_t generation = bank->slots[index].freeze_generation;
		if (found && generation <= best_generation)
			continue;
		found = true;
		best = index;
		best_generation = generation;
	}
	if (!found)
		return false;
	*slot = best;
	return true;
}

uint32_t aius_recorder_bank_deferred_epochs(const AiusRecorderBank *bank)
{
	return bank->deferred_epochs;
}

uint32_t aius_recorder_bank_lease_skips(const AiusRecorderBank *bank)
{
	return bank->lease_skips;
}

bool aius_recorder_bank_config_pending(const AiusRecorderBank *bank)
{
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
		if (bank->config_pending_slots[i] != 0u)
			return true;
	}
	return false;
}

AiusStatus aius_recorder_bank_set_config(AiusRecorderBank *bank, uint32_t mask,
                                         float gr_threshold_db,
                                         uint32_t gr_hold_blocks,
                                         uint32_t postroll_blocks)
{
	if (bank == NULL || (mask & ~AIUS_RECORDER_MASK_ALL) != 0u ||
	    gr_threshold_db > 0.0f || !isfinite(gr_threshold_db))
		return AIUS_INVALID_ARG;
	postroll_blocks = bank_clamp_postroll(bank, postroll_blocks);
	/* Remembered as the pending config before the loop, so a slot that
	 * refuses below is refusing something the bank can replay verbatim. */
	bank->pending_mask = mask;
	bank->pending_gr_threshold_db = gr_threshold_db;
	bank->pending_gr_hold_blocks = gr_hold_blocks;
	bank->pending_postroll_blocks = postroll_blocks;
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
		/* A slot mid-capture (only ever the active one) refuses --
		 * expected, matching aius_recorder_set_config's own "a
		 * capture in flight keeps its config" contract, and not an
		 * error. It is flagged rather than forgotten: the refused
		 * config is applied when that slot next thaws or is recycled,
		 * so the operator's change survives the capture in flight
		 * instead of silently reverting to the previous settings.
		 * Every other slot accepts it now, so a not-yet-activated
		 * slot already carries the new config when its turn comes --
		 * and clears any older pending flag of its own by doing so. */
		bank->config_pending_slots[i] =
			aius_recorder_set_config(&bank->slots[i], mask,
			                         gr_threshold_db, gr_hold_blocks,
			                         postroll_blocks) == AIUS_OK
				? 0u
				: 1u;
	}
	return AIUS_OK;
}

AiusStatus aius_recorder_bank_read(const AiusRecorderBank *bank,
                                   uint32_t slot, uint32_t offset_bytes,
                                   uint32_t length_bytes, uint32_t generation,
                                   const uint8_t **out, uint32_t *out_length)
{
	if (bank == NULL || slot >= AIUS_RECORDER_BANK_SLOTS)
		return AIUS_INVALID_ARG;
	return aius_recorder_read(&bank->slots[slot], offset_bytes,
	                          length_bytes, generation, out, out_length);
}

void aius_recorder_bank_read_acquire(AiusRecorderBank *bank, uint32_t slot)
{
	bank->reads_in_flight[slot]++;
}

void aius_recorder_bank_read_release(AiusRecorderBank *bank, uint32_t slot)
{
	if (bank->reads_in_flight[slot] > 0u)
		bank->reads_in_flight[slot]--;
}

AiusStatus aius_recorder_bank_snapshot_slot(const AiusRecorderBank *bank,
                                            uint32_t slot,
                                            AiusRecorderSnapshot *out)
{
	if (bank == NULL || slot >= AIUS_RECORDER_BANK_SLOTS || out == NULL)
		return AIUS_INVALID_ARG;
	aius_recorder_snapshot(&bank->slots[slot], out);
	return AIUS_OK;
}
