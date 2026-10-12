/* The input-liveness rule (aius-input-recovery Phase 1).
 *
 * The scenarios are the owner's stated use pattern, decomposed: hours with the
 * entertainment stack off (NO_CARRIER), the OSD-navigation burst where the box
 * is handed heterogeneous material (STARVED, SOURCE_SILENT, SINK_BLOCKED), and
 * long stable decode (PRODUCING). The discriminator rows are the load-bearing
 * ones: a source that is deliberately quiet completes bursts and MUST NOT read
 * as a fault, and a carrier-dark window MUST NOT share a duration with a
 * locked-but-dark one, or the watermark this rule exists to produce is
 * meaningless.
 *
 * COUNTERS ONLY EVER GO UP HERE, and that is enforced by shape rather than by
 * memory: every row advances a cursor by a delta and no row can name an
 * absolute value. The first draft of this suite used a helper returning fixed
 * baselines, so a row following one that advanced a counter silently drove it
 * BACKWARDS; the rule's wrap-correct unsigned subtraction then read that as a
 * ~4-billion delta and the de-escalation row passed without exercising
 * anything. A deliberate mutation of the rule (allowing de-escalation) still
 * passed the suite, which is how the flaw was caught. The absolute-valued rows
 * are confined to the two wrap scenarios, where wrapping is the subject. */

#include "aius_input_liveness.h"

#include <assert.h>

/* Advance the cursor. Deltas only, so monotonicity cannot be forgotten. */
static void advance(AiusInputLivenessSample *cursor, uint32_t now_ms,
                    int carrier_locked, uint32_t committed, uint32_t sink,
                    uint32_t bursts, uint32_t words)
{
	cursor->now_ms = now_ms;
	cursor->carrier_locked = carrier_locked;
	cursor->committed_blocks += committed;
	cursor->sink_attempts += sink;
	cursor->bursts_ok += bursts;
	cursor->input_words += words;
}

/* A cursor at arbitrary non-zero lifetime values, so no row can pass by
 * accident of a counter that happened to start at zero. */
static AiusInputLivenessSample cursor_at(uint32_t now_ms, int carrier_locked)
{
	AiusInputLivenessSample cursor;
	cursor.committed_blocks = 1000u;
	cursor.sink_attempts = 2000u;
	cursor.bursts_ok = 3000u;
	cursor.input_words = 4000u;
	cursor.now_ms = now_ms;
	cursor.carrier_locked = carrier_locked;
	return cursor;
}

/* Seed the rule from the cursor. The seed step observes no delta, so it must
 * claim nothing. */
static AiusInputLiveness seeded_from(const AiusInputLivenessSample *cursor)
{
	AiusInputLiveness state;
	aius_input_liveness_reset(&state);
	const AiusInputLivenessResult result =
		aius_input_liveness_step(&state, cursor);
	assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
	assert(result.dark_ms == 0u);
	assert(result.interval_closed == 0);
	return state;
}

int main(void)
{
	/* Long stable decode: committed blocks advancing keeps the rule in
	 * PRODUCING and never opens an interval. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		for (uint32_t tick = 1u; tick <= 100u; tick++) {
			advance(&cursor, tick * 10u, 1, 1u, 1u, 0u, 256u);
			const AiusInputLivenessResult result =
				aius_input_liveness_step(&state, &cursor);
			assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
			assert(result.dark_ms == 0u);
			assert(result.interval_closed == 0);
		}
	}

	/* Stack off: an unlocked carrier is its own class, however long it runs,
	 * and is never a silence to recover from. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 10u, 0, 0u, 0u, 0u, 0u);
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_NO_CARRIER);
		assert(result.dark_ms == 0u);
		advance(&cursor, 3600000u, 0, 0u, 0u, 0u, 0u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_NO_CARRIER);
		assert(result.dark_ms == 3600000u - 10u);
	}

	/* Deliberate quiet: bursts completing with nothing decoded behind them is
	 * source intent. Pause and null bursts complete by design, so this row is
	 * what stops the rule fighting a source that is simply silent. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		for (uint32_t tick = 1u; tick <= 10u; tick++) {
			advance(&cursor, tick * 100u, 1, 0u, 0u, 1u, 1536u);
			const AiusInputLivenessResult result =
				aius_input_liveness_step(&state, &cursor);
			assert(result.klass ==
			       AIUS_INPUT_LIVENESS_SOURCE_SILENT);
			assert(result.dark_ms == tick * 100u);
		}
	}

	/* Unintelligible material: words consumed, no burst ever completed. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 500u, 1, 0u, 0u, 0u, 8192u);
		const AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_STARVED);
		assert(result.dark_ms == 500u);
	}

	/* Locked but not one word delivered. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 500u, 1, 0u, 0u, 0u, 0u);
		const AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_NO_INPUT);
		assert(result.dark_ms == 500u);
	}

	/* The gate/clock case: blocks produced and offered, none committed. It
	 * outranks framing evidence, because a source that is genuinely quiet
	 * produces no blocks to offer in the first place. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 250u, 1, 0u, 8u, 8u, 12288u);
		const AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SINK_BLOCKED);
		assert(result.dark_ms == 250u);
	}

	/* Escalation inside one interval does not restart the clock: a slow
	 * characterisation must not keep resetting the duration being measured.
	 * Each row advances ONLY the evidence it is about, so the class it
	 * asserts is the class that evidence produces. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);

		advance(&cursor, 100u, 1, 0u, 0u, 0u, 512u);
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_STARVED);
		assert(result.dark_ms == 100u);

		advance(&cursor, 200u, 1, 0u, 0u, 1u, 512u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SOURCE_SILENT);
		assert(result.dark_ms == 200u);

		advance(&cursor, 300u, 1, 0u, 1u, 1u, 512u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SINK_BLOCKED);
		assert(result.dark_ms == 300u);

		/* ...and it never de-escalates. This row advances ONLY words, so
		 * the observed class this step is STARVED; the interval must stay
		 * SINK_BLOCKED because that is the strongest evidence it has ever
		 * carried. A rule that assigned the observed class unconditionally
		 * fails exactly here. */
		advance(&cursor, 400u, 1, 0u, 0u, 0u, 512u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SINK_BLOCKED);
		assert(result.dark_ms == 400u);

		/* Same, one rung down: framing evidence alone must not pull a
		 * SINK_BLOCKED interval back to SOURCE_SILENT. */
		advance(&cursor, 500u, 1, 0u, 0u, 1u, 512u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SINK_BLOCKED);
		assert(result.dark_ms == 500u);
	}

	/* A quiet source that later goes unintelligible keeps the stronger class,
	 * so SOURCE_SILENT cannot be walked back to STARVED either. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 100u, 1, 0u, 0u, 1u, 1536u);
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SOURCE_SILENT);
		advance(&cursor, 200u, 1, 0u, 0u, 0u, 1536u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SOURCE_SILENT);
	}

	/* A commit closes the interval and reports its class and duration once. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 700u, 1, 0u, 0u, 20u, 30720u);
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SOURCE_SILENT);
		assert(result.interval_closed == 0);

		advance(&cursor, 900u, 1, 1u, 1u, 1u, 1536u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
		assert(result.interval_closed == 1);
		assert(result.closed_class ==
		       AIUS_INPUT_LIVENESS_SOURCE_SILENT);
		assert(result.closed_ms == 900u);

		/* Reported once, not on every subsequent producing step. */
		advance(&cursor, 1000u, 1, 1u, 1u, 1u, 1536u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
		assert(result.interval_closed == 0);
	}

	/* The boundary that matters most: a carrier drop closes the locked-dark
	 * interval and starts a separate NO_CARRIER one, so a multi-hour
	 * stack-off window can never inflate a locked-dark watermark. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 400u, 1, 0u, 4u, 4u, 6144u);
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SINK_BLOCKED);

		advance(&cursor, 500u, 0, 0u, 0u, 0u, 0u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.interval_closed == 1);
		assert(result.closed_class == AIUS_INPUT_LIVENESS_SINK_BLOCKED);
		assert(result.closed_ms == 500u);
		assert(result.klass == AIUS_INPUT_LIVENESS_NO_CARRIER);
		assert(result.dark_ms == 0u);

		/* The new interval times from the carrier edge, not from the start
		 * of the interval it displaced. */
		advance(&cursor, 5000u, 0, 0u, 0u, 0u, 0u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_NO_CARRIER);
		assert(result.dark_ms == 4500u);

		/* Relock with nothing yet committed opens a fresh locked interval,
		 * timed from the relock. */
		advance(&cursor, 6000u, 1, 0u, 0u, 0u, 0u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.interval_closed == 1);
		assert(result.closed_class == AIUS_INPUT_LIVENESS_NO_CARRIER);
		assert(result.closed_ms == 5500u);
		assert(result.klass == AIUS_INPUT_LIVENESS_NO_INPUT);
		assert(result.dark_ms == 0u);
	}

	/* A step that both relocks and commits belongs to the new state: the old
	 * interval closes and the step reports PRODUCING, not a one-step dark
	 * interval invented from the commit. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 100u, 0, 0u, 0u, 0u, 0u);
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_NO_CARRIER);

		advance(&cursor, 300u, 1, 1u, 1u, 1u, 1536u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.interval_closed == 1);
		assert(result.closed_class == AIUS_INPUT_LIVENESS_NO_CARRIER);
		assert(result.closed_ms == 200u);
		assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
		assert(result.dark_ms == 0u);
	}

	/* Accepted residue: blocks are committed with the input carrier dark.
	 * Production is production whatever produced it, so the rule reports
	 * PRODUCING and opens no NO_CARRIER interval while they last. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 0);
		AiusInputLiveness state = seeded_from(&cursor);
		for (uint32_t tick = 1u; tick <= 20u; tick++) {
			advance(&cursor, tick * 10u, 0, 1u, 1u, 0u, 0u);
			const AiusInputLivenessResult result =
				aius_input_liveness_step(&state, &cursor);
			assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
			assert(result.interval_closed == 0);
			assert(result.dark_ms == 0u);
		}
	}

	/* Lifetime counters wrap. Unsigned subtraction is wrap-correct, so a
	 * counter crossing 2^32 mid-interval reads as the delta it is and not as
	 * a ~4-billion jump that would mask every other class. These are the only
	 * rows allowed to name absolute counter values. */
	{
		AiusInputLiveness state;
		aius_input_liveness_reset(&state);
		AiusInputLivenessSample cursor;
		cursor.committed_blocks = 0xFFFFFFFFu;
		cursor.sink_attempts = 0xFFFFFFFFu;
		cursor.bursts_ok = 0xFFFFFFFFu;
		cursor.input_words = 0xFFFFFFFFu;
		cursor.now_ms = 0u;
		cursor.carrier_locked = 1;
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);

		/* Only the word counter wraps: STARVED, not SINK_BLOCKED. */
		advance(&cursor, 100u, 1, 0u, 0u, 0u, 3u);
		assert(cursor.input_words == 2u); /* wrapped past zero */
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_STARVED);

		/* A wrapping commit counter still reads as production. */
		advance(&cursor, 200u, 1, 2u, 0u, 0u, 0u);
		assert(cursor.committed_blocks == 1u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
		assert(result.interval_closed == 1);
		assert(result.closed_class == AIUS_INPUT_LIVENESS_STARVED);
	}

	/* The millisecond clock wraps too, and a dark interval spanning the wrap
	 * must report its true duration rather than a ~49-day one. */
	{
		AiusInputLivenessSample cursor = cursor_at(0xFFFFFF00u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 0x000000A0u, 1, 0u, 0u, 1u, 1536u);
		const AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SOURCE_SILENT);
		assert(result.dark_ms == 0xA0u + 0x100u);
	}

	/* A reset re-seeds: the first step after it observes no delta and cannot
	 * manufacture an interval out of the counter jump. */
	{
		AiusInputLivenessSample cursor = cursor_at(0u, 1);
		AiusInputLiveness state = seeded_from(&cursor);
		advance(&cursor, 100u, 1, 0u, 1u, 0u, 512u);
		AiusInputLivenessResult result =
			aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_SINK_BLOCKED);

		aius_input_liveness_reset(&state);
		advance(&cursor, 200u, 1, 500000u, 900000u, 0u, 0u);
		result = aius_input_liveness_step(&state, &cursor);
		assert(result.klass == AIUS_INPUT_LIVENESS_PRODUCING);
		assert(result.interval_closed == 0);
		assert(result.dark_ms == 0u);
	}

	return 0;
}
