#include "aius_recorder_bank.h"

#include <math.h>
#include <string.h>

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

	uint32_t next = (bank->active + 1u) % AIUS_RECORDER_BANK_SLOTS;
	if (bank->reads_in_flight[next] != 0u)
		return drained; /* deferred: retried on the next drain() call */

	if (bank->slots[next].state == AIUS_RECORDER_FROZEN) {
		/* The ring has wrapped onto a slot an operator may not have
		 * pulled yet -- recycle it and count the wrap honestly. */
		bank->slots_recycled++;
		(void)aius_recorder_thaw(&bank->slots[next]);
	}
	/* A slot that has never triggered is still in its virgin RECORDING
	 * state from init and needs no reset. */
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
	return aius_recorder_thaw(&bank->slots[bank->active]);
}

AiusStatus aius_recorder_bank_park(AiusRecorderBank *bank)
{
	if (bank == NULL)
		return AIUS_INVALID_ARG;
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++)
		(void)aius_recorder_park(&bank->slots[i]);
	return AIUS_OK;
}

AiusStatus aius_recorder_bank_set_config(AiusRecorderBank *bank, uint32_t mask,
                                         float gr_threshold_db,
                                         uint32_t gr_hold_blocks,
                                         uint32_t postroll_blocks)
{
	if (bank == NULL || (mask & ~AIUS_RECORDER_MASK_ALL) != 0u ||
	    gr_threshold_db > 0.0f || !isfinite(gr_threshold_db))
		return AIUS_INVALID_ARG;
	/* A postroll longer than a slot can physically hold is nonsensical
	 * once slots shrink from one long ring to N shorter ones -- clamp
	 * against slot capacity in the production block size, on top of
	 * aius_recorder_set_config's own AIUS_RECORDER_POSTROLL_MAX_BLOCKS
	 * clamp. Every slot has the same ring_frames (bank_init partitions
	 * the total evenly), so slot 0 is representative. */
	uint32_t slot_capacity_blocks =
		bank->slots[0].ring_frames / AIUS_RECORDER_BANK_BLOCK_FRAMES;
	if (slot_capacity_blocks != 0u && postroll_blocks > slot_capacity_blocks)
		postroll_blocks = slot_capacity_blocks;
	for (uint32_t i = 0; i < AIUS_RECORDER_BANK_SLOTS; i++) {
		/* A slot mid-capture (only ever the active one) refuses --
		 * expected, matching aius_recorder_set_config's own "a
		 * capture in flight keeps its config" contract. Every other
		 * slot accepts it, so a not-yet-activated slot already
		 * carries the new config when its turn comes. */
		(void)aius_recorder_set_config(&bank->slots[i], mask,
		                               gr_threshold_db, gr_hold_blocks,
		                               postroll_blocks);
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
