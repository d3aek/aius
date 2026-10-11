/* The dark-interval record and the gate-closed watermark
 * (aius-input-recovery Phase 1).
 *
 * The rows are the owner's use pattern decomposed against the transmit gate's
 * ladder: hours unlocked (a NO_CARRIER run that must land in its own bucket and
 * nowhere else), the OSD burst walking the ladder rung by rung with the gate
 * closed (each phase's watermark gets exactly its own run), the long stable
 * decode with the gate open (no watermark moves, no interval opens), and the
 * three stuck shapes the plan inventories, each parked in the phase that names
 * its guard. The record rows pin the commit-bounded boundary: production
 * stopping under an OPEN gate is an interval, an AC-3 inter-frame gap is not.
 *
 * Counters only ever go up here, enforced by shape: every row advances a cursor
 * by a delta (the input-liveness suite's lesson). */

#include "aius_input_dark.h"
#include "aius_input_rx.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The gate ladder's states as the epoch would latch them. */
static AiusInputDarkSample cursor_at(uint32_t now_ms)
{
	AiusInputDarkSample cursor;
	memset(&cursor, 0, sizeof(cursor));
	cursor.now_ms = now_ms;
	cursor.committed_blocks = 5000u;
	cursor.pipeline_class = 0u; /* AIUS_PIPELINE_UNKNOWN */
	return cursor;
}

static void unlocked(AiusInputDarkSample *cursor)
{
	cursor->carrier_locked = 0u;
	cursor->gate_open = 0u;
	cursor->carrier_ready = 0u;
	cursor->plan_applied = 0u;
	cursor->live_ready = 0u;
	cursor->rate_unsupported = 0u;
}

static void locked_unsupported(AiusInputDarkSample *cursor)
{
	unlocked(cursor);
	cursor->carrier_locked = 1u;
	cursor->rate_unsupported = 1u;
}

static void locked_acquiring(AiusInputDarkSample *cursor)
{
	unlocked(cursor);
	cursor->carrier_locked = 1u;
}

static void locked_plan_pending(AiusInputDarkSample *cursor)
{
	locked_acquiring(cursor);
	cursor->carrier_ready = 1u;
}

static void locked_awaiting_block(AiusInputDarkSample *cursor)
{
	locked_plan_pending(cursor);
	cursor->plan_applied = 1u;
}

static void locked_gate_armed(AiusInputDarkSample *cursor)
{
	locked_awaiting_block(cursor);
	cursor->live_ready = 1u;
}

static void locked_gate_open(AiusInputDarkSample *cursor)
{
	locked_gate_armed(cursor);
	cursor->gate_open = 1u;
}

/* One one-second window of n >= 2 epochs starting at `start`: the last lands
 * exactly at start + 1000 and closes it; the first `trues` read LOCK true. */
static void rx_window(AiusInputRx *rx, uint32_t start, uint32_t n,
                      uint32_t trues)
{
	uint32_t i;
	for (i = 0u; i < n; i++)
		aius_input_rx_step(rx, start + (i * 1000u) / (n - 1u), 0, 0, 0,
		                   (uint8_t)(i < trues));
}

/* Advance the cursor: time and committed blocks by delta only. */
static void advance(AiusInputDarkSample *cursor, uint32_t now_ms,
                    uint32_t committed)
{
	cursor->now_ms = now_ms;
	cursor->committed_blocks += committed;
}

static AiusInputDark seeded_from(const AiusInputDarkSample *cursor)
{
	AiusInputDark state;
	aius_input_dark_reset(&state);
	aius_input_dark_step(&state, cursor);
	assert(state.seeded == 1u);
	assert(state.interval_open == 0u);
	assert(state.interval_count == 0u);
	return state;
}

static void assert_untouched(const AiusInputDarkWatermark *watermark)
{
	assert(watermark->max_ms == 0u);
	assert(watermark->over_count == 0u);
	assert(watermark->last_ms == 0u);
	assert(watermark->last_at_ms == 0u);
}

int main(void)
{
	assert(AIUS_INPUT_DARK_RECORD_DEPTH == 8u);
	assert(AIUS_INPUT_DARK_CLOSED_PHASES == 6u);
	assert(AIUS_INPUT_DARK_GATE_OPEN == AIUS_INPUT_DARK_CLOSED_PHASES);
	assert(AIUS_INPUT_DARK_ALERT_MS == 3000u);
	assert(AIUS_INPUT_DARK_RECORD_MIN_MS == 500u);
	assert(sizeof(AiusInputDarkSnapshot) == 4u);
	assert(sizeof(AiusInputDarkInterval) == 16u);

	/* The ladder, one guard at a time, in the firmware's test order. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		unlocked(&cursor);
		assert(aius_input_dark_phase(&cursor) ==
		       AIUS_INPUT_DARK_NO_CARRIER);
		locked_unsupported(&cursor);
		assert(aius_input_dark_phase(&cursor) ==
		       AIUS_INPUT_DARK_RATE_UNSUPPORTED);
		/* Unsupported outranks the tracker's acceptance flag. */
		cursor.carrier_ready = 1u;
		assert(aius_input_dark_phase(&cursor) ==
		       AIUS_INPUT_DARK_RATE_UNSUPPORTED);
		locked_acquiring(&cursor);
		assert(aius_input_dark_phase(&cursor) ==
		       AIUS_INPUT_DARK_CLOCK_ACQUIRING);
		locked_plan_pending(&cursor);
		assert(aius_input_dark_phase(&cursor) ==
		       AIUS_INPUT_DARK_PLAN_PENDING);
		locked_awaiting_block(&cursor);
		assert(aius_input_dark_phase(&cursor) ==
		       AIUS_INPUT_DARK_AWAITING_BLOCK);
		locked_gate_armed(&cursor);
		assert(aius_input_dark_phase(&cursor) ==
		       AIUS_INPUT_DARK_GATE_ARMED);
		locked_gate_open(&cursor);
		assert(aius_input_dark_phase(&cursor) == AIUS_INPUT_DARK_GATE_OPEN);
		/* A replay's open gate under a dark carrier is still open. */
		cursor.carrier_locked = 0u;
		assert(aius_input_dark_phase(&cursor) == AIUS_INPUT_DARK_GATE_OPEN);
		assert(strcmp(aius_input_dark_phase_name(
			       AIUS_INPUT_DARK_AWAITING_BLOCK),
		              "awaiting_block") == 0);
		assert(strcmp(aius_input_dark_phase_name(99u), "invalid") == 0);
	}

	/* Long stable decode, gate open: no watermark moves and no interval
	 * opens, however long it runs. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		cursor.pipeline_class = 1u; /* PCM */
		AiusInputDark state = seeded_from(&cursor);
		uint32_t tick;
		for (tick = 1u; tick <= 1000u; tick++) {
			advance(&cursor, tick * 9u, 3u);
			aius_input_dark_step(&state, &cursor);
			assert(state.interval_open == 0u);
			assert(state.phase == AIUS_INPUT_DARK_GATE_OPEN);
		}
		uint32_t phase;
		for (phase = 0u; phase < AIUS_INPUT_DARK_CLOSED_PHASES; phase++)
			assert_untouched(&state.phases[phase]);
		assert(state.interval_count == 0u);
		assert(aius_input_dark_open_ms(&state, 9000u) == 0u);
		assert(aius_input_dark_phase_ms(&state, 9000u) == 9000u);
	}

	/* Stack off for an hour, then the OSD burst walks the ladder rung by
	 * rung with the gate closed. Each phase's watermark receives exactly
	 * its own run, the hour lands in NO_CARRIER's bucket alone, and only
	 * the runs past 3 s are over-counted. This is the gate-closed region
	 * the production-gap watermark discards, measured. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		unlocked(&cursor);
		AiusInputDark state = seeded_from(&cursor);

		advance(&cursor, 3600000u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phase == AIUS_INPUT_DARK_NO_CARRIER);
		assert(aius_input_dark_phase_ms(&state, 3600000u) == 3600000u);

		locked_unsupported(&cursor);
		advance(&cursor, 3600000u + 10u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_NO_CARRIER].max_ms ==
		       3600000u + 10u);
		assert(state.phases[AIUS_INPUT_DARK_NO_CARRIER].over_count == 1u);
		assert(state.phases[AIUS_INPUT_DARK_NO_CARRIER].last_ms ==
		       3600000u + 10u);
		assert(state.phases[AIUS_INPUT_DARK_NO_CARRIER].last_at_ms ==
		       3600000u + 10u);

		/* 2 s refused, below the alert: max moves, over-count does not. */
		locked_acquiring(&cursor);
		advance(&cursor, 3600000u + 2010u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_RATE_UNSUPPORTED].max_ms ==
		       2000u);
		assert(state.phases[AIUS_INPUT_DARK_RATE_UNSUPPORTED].over_count ==
		       0u);
		assert(state.phases[AIUS_INPUT_DARK_RATE_UNSUPPORTED].last_ms ==
		       0u);

		/* A legitimate 1.5 s acquisition is under the alert. */
		locked_plan_pending(&cursor);
		advance(&cursor, 3600000u + 3510u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_CLOCK_ACQUIRING].max_ms ==
		       1500u);
		assert(state.phases[AIUS_INPUT_DARK_CLOCK_ACQUIRING].over_count ==
		       0u);

		locked_awaiting_block(&cursor);
		advance(&cursor, 3600000u + 3610u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_PLAN_PENDING].max_ms == 100u);

		locked_gate_armed(&cursor);
		advance(&cursor, 3600000u + 4010u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_AWAITING_BLOCK].max_ms == 400u);

		locked_gate_open(&cursor);
		advance(&cursor, 3600000u + 4019u, 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_GATE_ARMED].max_ms == 9u);
		assert(state.phase == AIUS_INPUT_DARK_GATE_OPEN);

		/* Nothing leaked between buckets. */
		assert(state.phases[AIUS_INPUT_DARK_NO_CARRIER].max_ms ==
		       3600000u + 10u);
		assert(state.phases[AIUS_INPUT_DARK_CLOCK_ACQUIRING].max_ms ==
		       1500u);
		uint32_t phase;
		for (phase = 0u; phase < AIUS_INPUT_DARK_CLOSED_PHASES; phase++) {
			if (phase == AIUS_INPUT_DARK_NO_CARRIER)
				continue;
			assert(state.phases[phase].over_count == 0u);
		}

		/* The whole dark span is one recorded interval, timed from the
		 * seed (the last epoch that counted as a commit), entered unlocked
		 * with the gate closed and left open with the tracker accepted. */
		assert(state.interval_count == 1u);
		const AiusInputDarkInterval *interval =
			aius_input_dark_interval(&state, 0u);
		assert(interval != NULL);
		assert(interval->start_ms == 0u);
		assert(interval->duration_ms == 3600000u + 4019u);
		assert(interval->entry.phase == AIUS_INPUT_DARK_NO_CARRIER);
		assert(interval->entry.carrier_ready == 0u);
		assert(interval->entry.gate == 0u);
		assert(interval->exit.phase == AIUS_INPUT_DARK_GATE_OPEN);
		assert(interval->exit.carrier_ready == 1u);
		assert(interval->exit.gate == 1u);
		assert(aius_input_dark_interval(&state, 1u) == NULL);
	}

	/* Stuck state 1, gate never opens: plan applied, tracker accepted, no
	 * block ever lands. The AWAITING_BLOCK run is the live reading while
	 * stuck (phase_ms grows), and the open interval names its entry. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		cursor.pipeline_class = 2u; /* DTS */
		AiusInputDark state = seeded_from(&cursor);
		advance(&cursor, 9u, 1u);
		aius_input_dark_step(&state, &cursor);

		/* Rate change: gate closes, tracker re-acquires, plan lands. */
		locked_acquiring(&cursor);
		cursor.pipeline_class = 0u;
		advance(&cursor, 18u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 1u);
		assert(state.interval_entry.phase ==
		       AIUS_INPUT_DARK_CLOCK_ACQUIRING);
		assert(state.interval_entry.klass == 0u);
		assert(state.interval_entry.gate == 0u);
		locked_awaiting_block(&cursor);
		advance(&cursor, 1218u, 0u);
		aius_input_dark_step(&state, &cursor);

		/* ...and 36 minutes of nothing. */
		advance(&cursor, 1218u + 2160000u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phase == AIUS_INPUT_DARK_AWAITING_BLOCK);
		assert(aius_input_dark_phase_ms(&state, cursor.now_ms) ==
		       2160000u);
		assert(aius_input_dark_open_ms(&state, cursor.now_ms) ==
		       cursor.now_ms - 9u);
		/* Not latched until the run ends: the open run is the reading. */
		assert(state.phases[AIUS_INPUT_DARK_AWAITING_BLOCK].max_ms == 0u);
		assert(state.interval_count == 0u);

		/* The carrier drops: the run ends and is over-counted in the
		 * phase that names the guard, not in NO_CARRIER's. */
		unlocked(&cursor);
		advance(&cursor, cursor.now_ms + 9u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_AWAITING_BLOCK].max_ms ==
		       2160000u + 9u);
		assert(state.phases[AIUS_INPUT_DARK_AWAITING_BLOCK].over_count ==
		       1u);
		assert(state.phases[AIUS_INPUT_DARK_AWAITING_BLOCK].last_at_ms ==
		       cursor.now_ms);
		assert_untouched(&state.phases[AIUS_INPUT_DARK_NO_CARRIER]);
		/* The interval is still open: a carrier edge is not a commit. */
		assert(state.interval_open == 1u);
		assert(state.interval_count == 0u);
	}

	/* Stuck state 2, class latched and silent: production stops under an
	 * OPEN gate. No gate-edge instrument sees this; the commit-bounded
	 * record does, and its entry snapshot says the gate was open. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		cursor.pipeline_class = 2u; /* DTS */
		AiusInputDark state = seeded_from(&cursor);
		advance(&cursor, 9u, 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 0u);

		advance(&cursor, 18u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 1u);
		assert(state.interval_entry.phase == AIUS_INPUT_DARK_GATE_OPEN);
		assert(state.interval_entry.klass == 2u);
		assert(state.interval_entry.gate == 1u);
		assert(state.interval_entry.carrier_ready == 1u);
		/* Timed from the last commit, not from the first dark epoch. */
		advance(&cursor, 20009u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(aius_input_dark_open_ms(&state, cursor.now_ms) == 20000u);
		/* The gate stayed open the whole time, so no closed-phase
		 * watermark moved: this shape is the record's alone. */
		uint32_t phase;
		for (phase = 0u; phase < AIUS_INPUT_DARK_CLOSED_PHASES; phase++)
			assert_untouched(&state.phases[phase]);

		/* Recovery (a commit) closes and records it with the exit state. */
		cursor.pipeline_class = 1u; /* PCM */
		advance(&cursor, 20018u, 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 0u);
		assert(state.interval_count == 1u);
		const AiusInputDarkInterval *interval =
			aius_input_dark_interval(&state, 0u);
		assert(interval->start_ms == 9u);
		assert(interval->duration_ms == 20009u);
		assert(interval->entry.klass == 2u);
		assert(interval->exit.klass == 1u);
		assert(interval->exit.phase == AIUS_INPUT_DARK_GATE_OPEN);
	}

	/* Stuck state 3, rate latched unsupported: the run parks in
	 * RATE_UNSUPPORTED and is over-counted there when it ends. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		AiusInputDark state = seeded_from(&cursor);
		locked_unsupported(&cursor);
		advance(&cursor, 9u, 0u);
		aius_input_dark_step(&state, &cursor);
		advance(&cursor, 9u + 600000u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phase == AIUS_INPUT_DARK_RATE_UNSUPPORTED);
		unlocked(&cursor);
		advance(&cursor, 18u + 600000u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_RATE_UNSUPPORTED].max_ms ==
		       600009u);
		assert(state.phases[AIUS_INPUT_DARK_RATE_UNSUPPORTED].over_count ==
		       1u);
	}

	/* An AC-3 inter-frame gap is a dark interval by the boundary rule but
	 * far below the record floor, so a steady compressed decode records
	 * nothing however many frames it plays. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		AiusInputDark state = seeded_from(&cursor);
		uint32_t frame;
		for (frame = 1u; frame <= 2000u; frame++) {
			/* Three epochs of nothing, then the decoded frame lands. */
			advance(&cursor, frame * 32u - 24u, 0u);
			aius_input_dark_step(&state, &cursor);
			advance(&cursor, frame * 32u - 16u, 0u);
			aius_input_dark_step(&state, &cursor);
			advance(&cursor, frame * 32u - 8u, 0u);
			aius_input_dark_step(&state, &cursor);
			assert(state.interval_open == 1u);
			advance(&cursor, frame * 32u, 2u);
			aius_input_dark_step(&state, &cursor);
			assert(state.interval_open == 0u);
		}
		assert(state.interval_count == 0u);
	}

	/* An interval exactly at the floor is recorded; one epoch under is not. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		AiusInputDark state = seeded_from(&cursor);
		advance(&cursor, 9u, 0u);
		aius_input_dark_step(&state, &cursor);
		advance(&cursor, 499u, 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_count == 0u);
		advance(&cursor, 508u, 0u);
		aius_input_dark_step(&state, &cursor);
		advance(&cursor, 999u, 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_count == 1u);
		assert(aius_input_dark_interval(&state, 0u)->start_ms == 499u);
		assert(aius_input_dark_interval(&state, 0u)->duration_ms == 500u);
	}

	/* The record ring keeps the newest eight; the lifetime count says what
	 * it overwrote, and the newest-first accessor orders them. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		AiusInputDark state = seeded_from(&cursor);
		uint32_t index;
		for (index = 1u; index <= 11u; index++) {
			advance(&cursor, cursor.now_ms + 9u, 0u);
			aius_input_dark_step(&state, &cursor);
			advance(&cursor, cursor.now_ms + 1000u * index, 1u);
			aius_input_dark_step(&state, &cursor);
		}
		assert(state.interval_count == 11u);
		assert(aius_input_dark_interval(&state, 0u)->duration_ms ==
		       11000u + 9u);
		assert(aius_input_dark_interval(&state, 7u)->duration_ms ==
		       4000u + 9u);
		assert(aius_input_dark_interval(&state, 8u) == NULL);
	}

	/* millis() wraps mid-run and mid-interval; both durations stay true. */
	{
		AiusInputDarkSample cursor = cursor_at(0xFFFFFF00u);
		locked_gate_open(&cursor);
		AiusInputDark state = seeded_from(&cursor);
		locked_awaiting_block(&cursor);
		advance(&cursor, 0xFFFFFF80u, 0u);
		aius_input_dark_step(&state, &cursor);
		advance(&cursor, 0x00000080u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(aius_input_dark_phase_ms(&state, 0x00000080u) == 0x100u);
		assert(aius_input_dark_open_ms(&state, 0x00000080u) == 0x180u);
		locked_gate_open(&cursor);
		advance(&cursor, 0x00000200u, 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.phases[AIUS_INPUT_DARK_AWAITING_BLOCK].max_ms ==
		       0x280u);
		assert(state.interval_count == 1u);
		assert(aius_input_dark_interval(&state, 0u)->duration_ms == 0x300u);
	}

	/* The committed counter wraps too. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		cursor.committed_blocks = 0xFFFFFFFFu;
		AiusInputDark state = seeded_from(&cursor);
		advance(&cursor, 9u, 2u);
		assert(cursor.committed_blocks == 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 0u);
	}

	/* A reset re-seeds: the next step claims nothing from the jump. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_awaiting_block(&cursor);
		AiusInputDark state = seeded_from(&cursor);
		advance(&cursor, 5000u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 1u);
		aius_input_dark_reset(&state);
		assert(aius_input_dark_phase_ms(&state, 5000u) == 0u);
		locked_gate_open(&cursor);
		advance(&cursor, 6000u, 400000u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 0u);
		assert(state.interval_count == 0u);
		assert_untouched(&state.phases[AIUS_INPUT_DARK_AWAITING_BLOCK]);
	}

	/* POST /api/clear zeroes the pipeline's committed_blocks. The firmware
	 * rebases last_committed to 0 in the same epoch-serial thread, so the
	 * drop is not a commit: the open interval stays open with its entry and
	 * start intact, and the next real commit closes it over the whole span. */
	{
		AiusInputDarkSample cursor = cursor_at(0u);
		locked_gate_open(&cursor);
		cursor.pipeline_class = 2u; /* DTS */
		AiusInputDark state = seeded_from(&cursor);
		advance(&cursor, 9u, 1u);
		aius_input_dark_step(&state, &cursor);
		locked_awaiting_block(&cursor);
		advance(&cursor, 5000u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 1u);
		assert(state.interval_entry.gate == 0u);

		/* The clear, then the rebase clear_counters() performs. */
		cursor.committed_blocks = 0u;
		state.last_committed = 0u;
		advance(&cursor, 5009u, 0u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 1u);
		assert(state.interval_count == 0u);
		assert(aius_input_dark_open_ms(&state, cursor.now_ms) == 5000u);
		assert(state.phase == AIUS_INPUT_DARK_AWAITING_BLOCK);
		assert(aius_input_dark_phase_ms(&state, cursor.now_ms) == 5009u - 5000u);

		/* Without the rebase the same step would read a ~4-billion delta as
		 * a commit; this is the shape clear_counters() must never produce. */
		AiusInputDark unrebased = state;
		unrebased.last_committed = 4000u;
		advance(&cursor, 5018u, 0u);
		aius_input_dark_step(&unrebased, &cursor);
		assert(unrebased.interval_open == 0u);
		assert(unrebased.interval_count == 1u);

		/* The rebased instrument closes on the next real commit, over the
		 * whole span from the last commit before the clear. */
		locked_gate_open(&cursor);
		advance(&cursor, 6009u, 1u);
		aius_input_dark_step(&state, &cursor);
		assert(state.interval_open == 0u);
		assert(state.interval_count == 1u);
		assert(aius_input_dark_interval(&state, 0u)->start_ms == 9u);
		assert(aius_input_dark_interval(&state, 0u)->duration_ms == 6000u);
		assert(aius_input_dark_interval(&state, 0u)->entry.gate == 0u);
	}

	/* Phase L (aius-output-jitter): the dark-input receiver state classifier,
	 * its change stamp, and the SRPC LOCK true-reads-per-second count. */
	{
		AiusInputRx rx;
		uint32_t t;

		assert(aius_input_rx_classify(0, 0, 0) == AIUS_INPUT_RX_CLEAN);
		assert(aius_input_rx_classify(1, 0, 0) == AIUS_INPUT_RX_SYMERR);
		assert(aius_input_rx_classify(1, 1, 0) == AIUS_INPUT_RX_SYMERR_BITERR);
		assert(aius_input_rx_classify(1, 1, 1) ==
		       AIUS_INPUT_RX_SYMERR_BITERR_LOCKLOSS);
		assert(aius_input_rx_classify(0, 1, 0) == AIUS_INPUT_RX_OTHER);
		assert(aius_input_rx_classify(0, 0, 1) == AIUS_INPUT_RX_OTHER);
		assert(aius_input_rx_classify(1, 0, 1) == AIUS_INPUT_RX_OTHER);
		assert(aius_input_rx_classify(0, 1, 1) == AIUS_INPUT_RX_OTHER);

		/* Nothing publishes until a window closes; the first completed
		 * window adopts its class (OR over the window) without a stamp. */
		aius_input_rx_reset(&rx);
		aius_input_rx_step(&rx, 5000u, 1, 0, 0, 1);
		aius_input_rx_step(&rx, 5500u, 0, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_CLEAN);
		assert(rx.lock_permille == 0u);
		aius_input_rx_step(&rx, 6000u, 0, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_SYMERR);
		assert(rx.state_since_ms == 0u);

		/* Class is the OR over the window: SYMERR in one epoch and BITERR in
		 * another is SYMERR_BITERR, stamped at the window close only. */
		aius_input_rx_step(&rx, 6100u, 1, 0, 0, 1);
		aius_input_rx_step(&rx, 6500u, 0, 1, 0, 1);
		aius_input_rx_step(&rx, 6999u, 0, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_SYMERR);
		assert(rx.state_since_ms == 0u);
		aius_input_rx_step(&rx, 7000u, 0, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_SYMERR_BITERR);
		assert(rx.state_since_ms == 7000u);

		/* Another window of the same class does not move the stamp. */
		aius_input_rx_step(&rx, 7500u, 1, 1, 0, 1);
		aius_input_rx_step(&rx, 8000u, 1, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_SYMERR_BITERR);
		assert(rx.state_since_ms == 7000u);

		/* Each windowed change stamps at its close. */
		aius_input_rx_step(&rx, 8500u, 1, 1, 1, 1);
		aius_input_rx_step(&rx, 9000u, 0, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_SYMERR_BITERR_LOCKLOSS);
		assert(rx.state_since_ms == 9000u);
		aius_input_rx_step(&rx, 9500u, 0, 0, 0, 1);
		aius_input_rx_step(&rx, 10000u, 0, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_CLEAN);
		assert(rx.state_since_ms == 10000u);

		/* A sporadic single-epoch error per window does not flicker the
		 * class: SYMERR every window, so no stamp after adoption. */
		aius_input_rx_reset(&rx);
		for (t = 0u; t <= 5000u; t += 100u)
			aius_input_rx_step(&rx, 20000u + t,
			                   (uint8_t)((t % 1000u) == 300u), 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_SYMERR);
		assert(rx.state_since_ms == 0u);

		/* Permille is a ratio: full, half and zero lock read 1000, 500 and 0
		 * whatever the epoch count of the window (10, 100 and 6500). */
		{
			static const uint32_t counts[] = { 10u, 100u, 6500u };
			uint32_t c;
			for (c = 0u; c < 3u; c++) {
				const uint32_t n = counts[c];
				aius_input_rx_reset(&rx);
				rx_window(&rx, 7000u, n, n);
				assert(rx.lock_permille == 1000u);
				aius_input_rx_reset(&rx);
				rx_window(&rx, 7000u, n, n / 2u);
				assert(rx.lock_permille == 500u);
				aius_input_rx_reset(&rx);
				rx_window(&rx, 7000u, n, 0u);
				assert(rx.lock_permille == 0u);
			}
		}

		/* An open window publishes nothing. */
		aius_input_rx_reset(&rx);
		aius_input_rx_step(&rx, 100u, 0, 0, 0, 1);
		aius_input_rx_step(&rx, 1099u, 0, 0, 0, 1);
		assert(rx.lock_permille == 0u);

		/* A stalled window (2 epochs, 2 s apart) reports its own ratio, not
		 * a rate. */
		aius_input_rx_reset(&rx);
		aius_input_rx_step(&rx, 100u, 0, 0, 0, 1);
		aius_input_rx_step(&rx, 2100u, 0, 0, 0, 0);
		assert(rx.lock_permille == 500u);

		/* Wrap-correct across the 32-bit millis() boundary, window and
		 * stamp both. */
		aius_input_rx_reset(&rx);
		aius_input_rx_step(&rx, 0xFFFFFFF0u, 0, 0, 0, 1);
		aius_input_rx_step(&rx, 0xFFFFFFF0u + 999u, 0, 0, 0, 1);
		assert(rx.lock_permille == 0u);
		aius_input_rx_step(&rx, 0xFFFFFFF0u + 1000u, 0, 0, 0, 1);
		assert(rx.lock_permille == 1000u);
		aius_input_rx_step(&rx, 0xFFFFFFF0u + 1500u, 1, 0, 0, 1);
		aius_input_rx_step(&rx, 0xFFFFFFF0u + 2000u, 0, 0, 0, 1);
		assert(rx.rx_state == AIUS_INPUT_RX_SYMERR);
		assert(rx.state_since_ms == 0xFFFFFFF0u + 2000u);

		/* Lock never read true publishes zero. */
		aius_input_rx_reset(&rx);
		for (t = 0u; t <= 2000u; t += 10u)
			aius_input_rx_step(&rx, t, 1, 0, 0, 0);
		assert(rx.lock_permille == 0u);
	}

	puts("Aius input dark-interval test (phase ladder, stuck states, watermark)");
	puts("RESULT: all checks passed");
	return 0;
}
