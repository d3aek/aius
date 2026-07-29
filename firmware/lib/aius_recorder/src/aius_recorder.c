#include "aius_recorder.h"

#include <math.h>
#include <string.h>

#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

/* The flight recorder's portable core. Context rules, the transition table
 * and the gap policy are the header's contract; this file keeps every
 * function bounded, allocation-free, and single-writer per transition.
 *
 * KNOWN, ACCEPTED interleaving (the measure machine's cancel-token shape):
 * `freeze_token` is set by the controller and cleared by the block producer;
 * `suppressed_triggers` is incremented from both contexts. Both are
 * diagnostic-bearing, not state-bearing: a collision costs one suppressed
 * count or one redundant token store, never a corrupt transition, because
 * the state word itself always has exactly one writer per edge. */

static int power_of_two(uint32_t value)
{
	return value != 0u && (value & (value - 1u)) == 0u;
}

FLASHMEM uint32_t
aius_recorder_ring_bytes_after_staging(uint32_t storage_bytes,
	                                   uint32_t staging_bytes)
{
	if (storage_bytes <= staging_bytes)
		return 0u;
	const uint32_t available = storage_bytes - staging_bytes;
	if (available < AIUS_RECORDER_FRAME_BYTES)
		return 0u;
	uint32_t ring_bytes = AIUS_RECORDER_FRAME_BYTES;
	while (ring_bytes <= available / 2u &&
	       ring_bytes <= UINT32_MAX / 2u)
		ring_bytes *= 2u;
	return ring_bytes;
}

static void reset_capture(AiusRecorder *r)
{
	r->slot_write = 0u;
	r->slot_read = 0u;
	r->slot_read_offset = 0u;
	r->frames_staged = 0u;
	r->frames_drained = 0u;
	r->blocks_staged = 0u;
	r->wrapped = 0u;
	r->gap_count = 0u;
	r->last_gap_frame = 0u;
	r->suppressed_triggers = 0u;
	r->postroll_remaining = 0u;
	r->postroll_captured = 0u;
	r->end_frame = 0u;
	r->baseline_sync = 0u;
	r->gr_hold_count = 0u;
	r->trigger = AIUS_RECORDER_TRIGGER_NONE;
	r->trigger_at_frame = 0u;
	r->trigger_at_block = 0u;
	r->trigger_at_ms = 0u;
	r->freeze_token = 0u;
	r->reclaim_blocks = 0u;
	r->reclaim_count = 0u;
}

AiusStatus aius_recorder_init(AiusRecorder *r, uint8_t *ring,
                              uint32_t ring_bytes, uint32_t fitted_mb,
                              int32_t *staging, uint32_t *staging_frames,
                              uint32_t staging_slots, uint32_t slot_frames)
{
	if (r == NULL)
		return AIUS_INVALID_ARG;
	memset(r, 0, sizeof(*r));
	r->state = AIUS_RECORDER_ABSENT;
	/* Config defaults are installed even on an absent board so the status
	 * surface reports them honestly. */
	r->trigger_mask = AIUS_RECORDER_MASK_DEFAULT;
	r->gr_threshold_gain = 0.0f;
	r->gr_threshold_db = 0.0f;
	r->gr_hold_blocks = 1u;
	r->postroll_blocks = AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS;
	if (ring_bytes == 0u)
		return AIUS_OK; /* the bare board: absent is a first-class state */
	if (ring == NULL || staging == NULL || staging_frames == NULL ||
	    ring_bytes % AIUS_RECORDER_FRAME_BYTES != 0u ||
	    !power_of_two(ring_bytes / AIUS_RECORDER_FRAME_BYTES) ||
	    !power_of_two(staging_slots) || slot_frames == 0u)
		return AIUS_INVALID_ARG;
	r->ring = ring;
	r->ring_frames = ring_bytes / AIUS_RECORDER_FRAME_BYTES;
	r->ring_mask = r->ring_frames - 1u;
	r->fitted_mb = fitted_mb;
	r->staging = staging;
	r->staging_frames = staging_frames;
	r->staging_slots = staging_slots;
	r->slot_frames = slot_frames;
	reset_capture(r);
	/* Every field above is stored before the state store publishes them. */
	r->state = AIUS_RECORDER_RECORDING;
	return AIUS_OK;
}

/* Producer: latch trigger metadata, then the POSTROLL state store publishes it
 * (stage/commit). postroll_blocks == 0 skips straight to DRAINING: the
 * post-roll counts staged blocks from the trigger block inclusive, and zero
 * of them means the capture ends at the trigger boundary. */
static void producer_latch_trigger(AiusRecorder *r, uint32_t which,
                              uint32_t now_ms)
{
	uint32_t postroll = r->postroll_blocks;
	r->trigger = which;
	r->trigger_at_frame = r->frames_staged;
	r->trigger_at_block = r->blocks_staged;
	r->trigger_at_ms = now_ms;
	r->postroll_captured = 0u;
	if (postroll == 0u) {
		r->end_frame = r->frames_staged;
		r->state = AIUS_RECORDER_DRAINING;
		return;
	}
	r->postroll_remaining = postroll;
	r->state = AIUS_RECORDER_POSTROLL;
}

void aius_recorder_producer_block(AiusRecorder *r, const int32_t *l,
                             const int32_t *right, uint32_t frames,
                             uint32_t overruns, uint32_t underruns,
                             bool limiter_gain_valid, float limiter_gain,
                             uint32_t now_ms)
{
	uint32_t state = r->state;
	if (state != AIUS_RECORDER_RECORDING && state != AIUS_RECORDER_POSTROLL)
		return;

	/* Baselines first, and always -- tracking them while a trigger is
	 * masked keeps a later unmask from firing on history, and adopting
	 * them on the first block after init/thaw keeps FROZEN-era counter
	 * growth from reading as an edge. Resync on decrease: an operator
	 * counter clear moves the counters backwards and costs one missed
	 * edge, never a spurious freeze. */
	if (!r->baseline_sync) {
		r->overrun_base = overruns;
		r->underrun_base = underruns;
		r->baseline_sync = 1u;
	}
	if (overruns < r->overrun_base)
		r->overrun_base = overruns;
	if (underruns < r->underrun_base)
		r->underrun_base = underruns;

	uint32_t mask = r->trigger_mask;
	uint32_t fired = AIUS_RECORDER_TRIGGER_NONE;
	uint32_t token = r->freeze_token;
	if (token != 0u) {
		fired = token;
		r->freeze_token = 0u;
	} else {
		if ((mask & AIUS_RECORDER_MASK_OVERRUN) != 0u &&
		    overruns > r->overrun_base)
			fired = AIUS_RECORDER_TRIGGER_OVERRUN;
		else if ((mask & AIUS_RECORDER_MASK_UNDERRUN) != 0u &&
		         r->gate_open && underruns > r->underrun_base)
			fired = AIUS_RECORDER_TRIGGER_UNDERRUN;
		else if ((mask & AIUS_RECORDER_MASK_LIMITER_GR) != 0u &&
		         limiter_gain_valid && r->gr_threshold_gain > 0.0f) {
			if (limiter_gain < r->gr_threshold_gain) {
				r->gr_hold_count++;
				if (r->gr_hold_count >= r->gr_hold_blocks)
					fired = AIUS_RECORDER_TRIGGER_LIMITER_GR;
			} else {
				r->gr_hold_count = 0u;
			}
		}
	}
	/* The fired edge consumed the baseline delta either way. */
	r->overrun_base = overruns;
	r->underrun_base = underruns;

	if (fired != AIUS_RECORDER_TRIGGER_NONE) {
		if (state == AIUS_RECORDER_RECORDING) {
				producer_latch_trigger(r, fired, now_ms);
			state = r->state;
			if (state == AIUS_RECORDER_DRAINING)
				return; /* zero post-roll: this block is out */
		} else {
			/* Second trigger during the post-roll: first wins. */
			r->suppressed_triggers = r->suppressed_triggers + 1u;
		}
	}

	/* Stage the block, or stamp a gap. Staging full means an epoch stall
	 * beyond the ring's ~170 ms design point: the block is DROPPED --
	 * never block, never overwrite unconsumed staging. */
	if (frames == 0u)
		return;
	if (frames > r->slot_frames ||
	    r->slot_write - r->slot_read >= r->staging_slots) {
		r->gap_count = r->gap_count + 1u;
		r->last_gap_frame = r->frames_staged;
	} else {
		uint32_t slot = r->slot_write & (r->staging_slots - 1u);
		int32_t *dst = r->staging + (size_t)slot * r->slot_frames * 2u;
		memcpy(dst, l, frames * sizeof(int32_t));
		memcpy(dst + r->slot_frames, right, frames * sizeof(int32_t));
		r->staging_frames[slot] = frames;
		/* The slot_write store publishes the slot (stage/commit). */
		r->slot_write = r->slot_write + 1u;
		r->frames_staged = r->frames_staged + frames;
		if (!r->wrapped && r->frames_staged >= r->ring_frames)
			r->wrapped = 1u;
	}
	r->blocks_staged = r->blocks_staged + 1u;

	if (state == AIUS_RECORDER_POSTROLL) {
		r->postroll_captured = r->postroll_captured + 1u;
		r->postroll_remaining = r->postroll_remaining - 1u;
		if (r->postroll_remaining == 0u) {
			r->end_frame = r->frames_staged;
			r->state = AIUS_RECORDER_DRAINING;
		}
	}
}

/* Controller: reclaim after the producer block counter remains unchanged for
 * the bounded window. */
static void epoch_reclaim(AiusRecorder *r, uint32_t now_ms)
{
	uint32_t state = r->state;
	if (state == AIUS_RECORDER_RECORDING) {
		uint32_t token = r->freeze_token;
		r->trigger = token != 0u ? token
		                         : (uint32_t)AIUS_RECORDER_TRIGGER_OPERATOR;
		r->trigger_at_frame = r->frames_staged;
		r->trigger_at_block = r->blocks_staged;
		r->trigger_at_ms = now_ms;
		r->postroll_captured = 0u;
	}
	r->freeze_token = 0u;
	r->end_frame = r->frames_staged;
	r->state = AIUS_RECORDER_DRAINING;
}

void aius_recorder_epoch(AiusRecorder *r, bool carrier_locked, bool gate_open,
                         uint32_t underruns, uint32_t now_ms)
{
	uint32_t state = r->state;
	if (state == AIUS_RECORDER_ABSENT)
		return;
	/* Latch the gate for the producer's underrun qualification. The underrun
	 * counter FREE-RUNS as silence substitution while the gate is dark, so
	 * a closed -> open transition re-adopts every baseline (controller and producer
	 * alike) rather than reading the dark-era growth as an edge. Costs at
	 * most one missed real edge per transition, never a spurious freeze. */
	uint32_t prev_gate = r->gate_open;
	r->gate_open = gate_open ? 1u : 0u;
	if (gate_open && !prev_gate)
		r->baseline_sync = 0u;

	/* Controller underrun tracking, resync on decrease as the producer does. */
	if (underruns < r->underrun_base_epoch)
		r->underrun_base_epoch = underruns;

	if (state == AIUS_RECORDER_RECORDING && r->freeze_token == 0u) {
		uint32_t mask = r->trigger_mask;
		/* Carrier loss: the falling edge of the per-epoch lock sample.
		 * A level would freeze instantly on the bench norm (no source)
		 * and always during replay. */
		if ((mask & AIUS_RECORDER_MASK_CARRIER_LOSS) != 0u &&
		    r->carrier_prev && !carrier_locked)
			r->freeze_token = AIUS_RECORDER_TRIGGER_CARRIER_LOSS;
		/* Underrun fallback: the flagship producer-stall case advances
		 * the counter while the producer commits no further block. Qualified by the
		 * gate at BOTH ends of the delta: this epoch's (a stop closes
		 * the gate in the same evaluation, suppressing the drain
		 * transient) and the previous epoch's (the baseline must have
		 * been taken under an open gate to mean anything). */
		else if ((mask & AIUS_RECORDER_MASK_UNDERRUN) != 0u &&
		         gate_open && prev_gate &&
		         underruns > r->underrun_base_epoch)
			r->freeze_token = AIUS_RECORDER_TRIGGER_UNDERRUN;
	}
	r->carrier_prev = carrier_locked ? 1u : 0u;
	r->underrun_base_epoch = underruns;

	/* Reclaim: a pending token or an open post-roll with the block counter
	 * unchanged for the bounded window means the producer stopped. */
	state = r->state;
	if ((state == AIUS_RECORDER_RECORDING && r->freeze_token != 0u) ||
	    state == AIUS_RECORDER_POSTROLL) {
		uint32_t blocks = r->blocks_staged;
		if (blocks == r->reclaim_blocks) {
			r->reclaim_count++;
			if (r->reclaim_count >= AIUS_RECORDER_RECLAIM_EPOCHS)
				epoch_reclaim(r, now_ms);
		} else {
			r->reclaim_blocks = blocks;
			r->reclaim_count = 0u;
		}
	} else {
		r->reclaim_blocks = r->blocks_staged;
		r->reclaim_count = 0u;
	}
}

uint32_t aius_recorder_drain(AiusRecorder *r, uint32_t max_bytes)
{
	uint32_t state = r->state;
	if (state == AIUS_RECORDER_ABSENT || state == AIUS_RECORDER_FROZEN)
		return 0u;
	uint32_t budget_frames = max_bytes / AIUS_RECORDER_FRAME_BYTES;
	uint32_t drained_frames = 0u;
	while (budget_frames != 0u && r->slot_read != r->slot_write) {
		uint32_t slot = r->slot_read & (r->staging_slots - 1u);
		uint32_t slot_total = r->staging_frames[slot];
		uint32_t offset = r->slot_read_offset;
		uint32_t remaining = slot_total - offset;
		uint32_t take = remaining < budget_frames ? remaining
		                                          : budget_frames;
		const int32_t *src_l =
			r->staging + (size_t)slot * r->slot_frames * 2u + offset;
		const int32_t *src_r = src_l + r->slot_frames;
		for (uint32_t i = 0; i < take; i++) {
			uint32_t frame =
				(r->frames_drained + i) & r->ring_mask;
			int32_t *dst = (int32_t *)(r->ring +
				(size_t)frame * AIUS_RECORDER_FRAME_BYTES);
			dst[0] = src_l[i];
			dst[1] = src_r[i];
		}
		r->frames_drained += take;
		drained_frames += take;
		budget_frames -= take;
		if (offset + take == slot_total) {
			r->slot_read_offset = 0u;
			r->slot_read = r->slot_read + 1u;
		} else {
			r->slot_read_offset = offset + take;
		}
	}
	if (r->state == AIUS_RECORDER_DRAINING &&
	    r->slot_read == r->slot_write)
		r->state = AIUS_RECORDER_FROZEN;
	return drained_frames * AIUS_RECORDER_FRAME_BYTES;
}

AiusStatus aius_recorder_freeze(AiusRecorder *r)
{
	if (r == NULL)
		return AIUS_INVALID_ARG;
	uint32_t state = r->state;
	if (state == AIUS_RECORDER_RECORDING) {
		if (r->freeze_token == 0u)
			r->freeze_token = AIUS_RECORDER_TRIGGER_OPERATOR;
		return AIUS_OK;
	}
	if (state == AIUS_RECORDER_POSTROLL) {
		/* First wins; the operator's late request is counted, and the
		 * capture already in flight satisfies it. */
		r->suppressed_triggers = r->suppressed_triggers + 1u;
		return AIUS_OK;
	}
	return AIUS_INVALID_ARG;
}

AiusStatus aius_recorder_thaw(AiusRecorder *r)
{
	if (r == NULL || r->state != AIUS_RECORDER_FROZEN)
		return AIUS_INVALID_ARG;
	/* FROZEN means the producer is a no-op, so the controller may reset the
	 * producer-owned fields; the RECORDING store last re-arms it. */
	reset_capture(r);
	r->freeze_generation++;
	r->state = AIUS_RECORDER_RECORDING;
	return AIUS_OK;
}

AiusStatus aius_recorder_set_config(AiusRecorder *r, uint32_t mask,
                                    float gr_threshold_db,
                                    uint32_t gr_hold_blocks,
                                    uint32_t postroll_blocks)
{
	if (r == NULL || (mask & ~AIUS_RECORDER_MASK_ALL) != 0u ||
	    gr_threshold_db > 0.0f || !isfinite(gr_threshold_db))
		return AIUS_INVALID_ARG;
	uint32_t state = r->state;
	if (state != AIUS_RECORDER_RECORDING && state != AIUS_RECORDER_FROZEN &&
	    state != AIUS_RECORDER_ABSENT)
		return AIUS_INVALID_ARG; /* a capture in flight keeps its config */
	if (postroll_blocks > AIUS_RECORDER_POSTROLL_MAX_BLOCKS)
		postroll_blocks = AIUS_RECORDER_POSTROLL_MAX_BLOCKS;
	if (gr_hold_blocks == 0u)
		gr_hold_blocks = 1u;
	r->trigger_mask = mask;
	r->gr_threshold_db = gr_threshold_db;
	/* The one transcendental, evaluated cold; 0 dB disables (a threshold
	 * of unity gain would hold-count every block the limiter runs). */
	r->gr_threshold_gain = gr_threshold_db < 0.0f
		? powf(10.0f, gr_threshold_db / 20.0f)
		: 0.0f;
	r->gr_hold_blocks = gr_hold_blocks;
	r->postroll_blocks = postroll_blocks;
	return AIUS_OK;
}

AiusStatus aius_recorder_read(const AiusRecorder *r, uint32_t offset_bytes,
                              uint32_t length_bytes, uint32_t generation,
                              const uint8_t **out, uint32_t *out_length)
{
	if (r == NULL || out == NULL || out_length == NULL)
		return AIUS_INVALID_ARG;
	if (r->state != AIUS_RECORDER_FROZEN ||
	    generation != r->freeze_generation)
		return AIUS_INVALID_ARG;
	if (offset_bytes % AIUS_RECORDER_FRAME_BYTES != 0u)
		return AIUS_INVALID_ARG;
	uint32_t valid_frames = r->wrapped ? r->ring_frames : r->frames_drained;
	uint32_t valid_bytes = valid_frames * AIUS_RECORDER_FRAME_BYTES;
	if (length_bytes == 0u || offset_bytes >= valid_bytes)
		return AIUS_INVALID_ARG;
	uint32_t available = valid_bytes - offset_bytes;
	uint32_t take = length_bytes;
	if (take > AIUS_RECORDER_READ_MAX_BYTES)
		take = AIUS_RECORDER_READ_MAX_BYTES;
	if (take > available)
		take = available;
	*out = r->ring + offset_bytes;
	*out_length = take;
	return AIUS_OK;
}

void aius_recorder_snapshot(const AiusRecorder *r, AiusRecorderSnapshot *out)
{
	memset(out, 0, sizeof(*out));
	out->state = r->state;
	out->fitted_mb = r->fitted_mb;
	out->ring_frames = r->ring_frames;
	out->valid_frames = r->wrapped ? r->ring_frames : r->frames_drained;
	out->oldest_frame = r->wrapped ? (r->frames_drained & r->ring_mask) : 0u;
	out->freeze_generation = r->freeze_generation;
	out->gap_count = r->gap_count;
	out->last_gap_frame = r->last_gap_frame;
	out->postroll_blocks = r->postroll_blocks;
	out->postroll_captured = r->postroll_captured;
	out->trigger_mask = r->trigger_mask;
	out->gr_threshold_db = r->gr_threshold_db;
	out->gr_hold_blocks = r->gr_hold_blocks;
	out->suppressed_triggers = r->suppressed_triggers;
	out->trigger.trigger = r->trigger;
	out->trigger.at_frame = r->trigger_at_frame;
	out->trigger.at_block = r->trigger_at_block;
	out->trigger.at_ms = r->trigger_at_ms;
}
