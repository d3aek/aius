#ifndef AIUS_INPUT_LIVENESS_H
#define AIUS_INPUT_LIVENESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why nothing is reaching the transmit sink, and for how long. Pure and
 * portable (aius-input-recovery Phase 1).
 *
 * WHAT IT MEASURES, AND WHY IT IS NOT THE PRODUCTION GAP. The production-gap
 * watermark in optical_main.cpp measures micros() between blocks ACCEPTED at
 * enqueue, and deliberately resets its sentinel whenever the transmit gate is
 * closed, so that a source or rate switch never spans a bogus gap. Every way
 * the box can sit dark under a healthy carrier lives inside the region that
 * reset discards: a gate that never opens because `g_clock_live_ready` is only
 * ever armed by a block that has already cleared enqueue's guards, a class
 * latched silent on framed-but-unsupported bursts, and an unsupported-rate
 * latch with no timeout. This rule measures exactly that region. The two are
 * complementary and neither subsumes the other.
 *
 * HOW IT DECIDES. The caller feeds lifetime counters once per epoch and the
 * rule diffs them itself, so the caller keeps no history and no sample can be
 * double-counted. A DARK INTERVAL runs from the last epoch that committed an
 * output block to the next one that does. Across that whole interval the rule
 * accumulates three independent pieces of evidence, ORed rather than sampled,
 * because an epoch is far shorter than the burst period and any single epoch
 * legitimately shows no movement at all:
 *
 *   sink attempts   blocks were produced and offered to the sink. If none of
 *                   them committed, the blocker is downstream of the pipeline:
 *                   the gate, the clock plan or the rate match.
 *   burst outcomes  the parser completed bursts, so framing is healthy. With
 *                   no decoded output behind it this is a source that is
 *                   deliberately quiet: pause bursts and null bursts complete
 *                   by design (see aius_pipeline.h) and must never be treated
 *                   as a fault.
 *   input words     the receiver delivered words that were consumed. With no
 *                   framing evidence behind it, the box is being handed
 *                   material it cannot make sense of.
 *
 * The class is the STRONGEST evidence seen in the interval, in that order, so
 * an interval can only ever escalate as it is better characterised. Escalation
 * is not a new interval and must not restart the clock, or a slow
 * characterisation would keep resetting the very duration being measured.
 *
 * WHAT ENDS AN INTERVAL. A committed block ends it, which is the healthy exit.
 * A change in carrier lock also ends it, and that boundary is load-bearing:
 * the owner's real pattern is hours with the entertainment stack off, and
 * folding a legitimate multi-hour carrier-dark window into the same watermark
 * as a three-second sink-blocked window would destroy the only signal this
 * rule exists to produce. NO_CARRIER is therefore its own class with its own
 * watermark, never a silence to be recovered from.
 *
 * ACCEPTED RESIDUE (do not "fix" without reopening the contract): a committed
 * block is production whatever produced it, so any source committing blocks
 * while the input carrier is dark reads as PRODUCING and opens no NO_CARRIER
 * interval. That is the honest answer to the question this rule asks -- audio
 * IS reaching the sink. The transmit gate makes the same judgement for the
 * same reason (aius_tx_gate.h).
 *
 * PHASE 1 IS OBSERVE-ONLY. The rule classifies and times; it recommends
 * nothing and no caller acts on it yet. It returns the closed interval's class
 * and duration on the step that closes one so the caller can latch per-class
 * watermarks, which is what turns the open question in aius-input-recovery.md
 * (does the tracker-never-accepted case actually occur in the field?) into a
 * reading rather than an argument.
 *
 * Counters are lifetime and wrap; unsigned subtraction is wrap-correct for any
 * true delta below 2^32, which no epoch can exceed. */

typedef enum {
	/* An output block committed this step. Not a dark interval at all. */
	AIUS_INPUT_LIVENESS_PRODUCING = 0,
	/* The receiver PLL is unlocked. Expected, and never a fault here. */
	AIUS_INPUT_LIVENESS_NO_CARRIER,
	/* Locked, but not one word was consumed across the whole interval. */
	AIUS_INPUT_LIVENESS_NO_INPUT,
	/* Words consumed, no burst ever completed: unintelligible material. */
	AIUS_INPUT_LIVENESS_STARVED,
	/* Bursts completing with nothing decoded behind them: source intent. */
	AIUS_INPUT_LIVENESS_SOURCE_SILENT,
	/* Blocks produced and offered, none committed: the blocker is the gate,
	 * the clock plan or the rate match, not the pipeline. */
	AIUS_INPUT_LIVENESS_SINK_BLOCKED,
	AIUS_INPUT_LIVENESS_CLASS_COUNT
} AiusInputLivenessClass;

typedef struct {
	uint32_t committed_blocks; /* audio_diag.committed_output_blocks */
	uint32_t sink_attempts;    /* audio_diag.sink_attempts */
	uint32_t bursts_ok;        /* pipeline_diag.bursts_ok */
	uint32_t input_words;      /* audio_diag.input_words_consumed */
	uint32_t now_ms;
	int carrier_locked;
} AiusInputLivenessSample;

typedef struct {
	uint32_t last_committed;
	uint32_t last_sink_attempts;
	uint32_t last_bursts_ok;
	uint32_t last_input_words;
	uint32_t interval_start_ms;
	/* The strongest evidence seen since interval_start_ms. Held as the class
	 * itself rather than three flags so the ordering lives in one place. */
	uint8_t interval_class;
	uint8_t carrier_locked;
	uint8_t seeded;
} AiusInputLiveness;

typedef struct {
	/* The open interval's class this step, or PRODUCING. */
	AiusInputLivenessClass klass;
	/* How long the open interval has run. 0 when PRODUCING. */
	uint32_t dark_ms;
	/* 1 on the step that ends a dark interval; the two fields below are
	 * meaningful only then. */
	int interval_closed;
	AiusInputLivenessClass closed_class;
	uint32_t closed_ms;
} AiusInputLivenessResult;

static inline void aius_input_liveness_reset(AiusInputLiveness *state)
{
	state->last_committed = 0u;
	state->last_sink_attempts = 0u;
	state->last_bursts_ok = 0u;
	state->last_input_words = 0u;
	state->interval_start_ms = 0u;
	state->interval_class = (uint8_t)AIUS_INPUT_LIVENESS_PRODUCING;
	state->carrier_locked = 0u;
	state->seeded = 0u;
}

/* One call per epoch. The first call after a reset only seeds the counter
 * baseline: it can observe no delta, so it claims nothing. */
static inline AiusInputLivenessResult aius_input_liveness_step(
	AiusInputLiveness *state, const AiusInputLivenessSample *sample)
{
	AiusInputLivenessResult result;
	result.klass = AIUS_INPUT_LIVENESS_PRODUCING;
	result.dark_ms = 0u;
	result.interval_closed = 0;
	result.closed_class = AIUS_INPUT_LIVENESS_PRODUCING;
	result.closed_ms = 0u;

	const uint8_t carrier = sample->carrier_locked ? 1u : 0u;
	if (!state->seeded) {
		state->last_committed = sample->committed_blocks;
		state->last_sink_attempts = sample->sink_attempts;
		state->last_bursts_ok = sample->bursts_ok;
		state->last_input_words = sample->input_words;
		state->interval_start_ms = sample->now_ms;
		state->interval_class = (uint8_t)AIUS_INPUT_LIVENESS_PRODUCING;
		state->carrier_locked = carrier;
		state->seeded = 1u;
		return result;
	}

	const uint32_t committed_delta =
		sample->committed_blocks - state->last_committed;
	const uint32_t sink_delta =
		sample->sink_attempts - state->last_sink_attempts;
	const uint32_t bursts_delta = sample->bursts_ok - state->last_bursts_ok;
	const uint32_t words_delta =
		sample->input_words - state->last_input_words;
	state->last_committed = sample->committed_blocks;
	state->last_sink_attempts = sample->sink_attempts;
	state->last_bursts_ok = sample->bursts_ok;
	state->last_input_words = sample->input_words;

	/* Either boundary closes the open interval before a new one starts, so a
	 * carrier-dark window and a sink-blocked window can never share a
	 * duration. A step that both relocks and commits closes the old interval
	 * and reports PRODUCING: the commit is the new state's evidence, not the
	 * old one's. */
	if (carrier != state->carrier_locked || committed_delta != 0u) {
		if (state->interval_class != AIUS_INPUT_LIVENESS_PRODUCING) {
			result.interval_closed = 1;
			result.closed_class = (AiusInputLivenessClass)
				state->interval_class;
			result.closed_ms =
				sample->now_ms - state->interval_start_ms;
		}
		state->interval_start_ms = sample->now_ms;
		state->interval_class =
			(uint8_t)AIUS_INPUT_LIVENESS_PRODUCING;
		state->carrier_locked = carrier;
	}
	if (committed_delta != 0u)
		return result;

	AiusInputLivenessClass observed;
	if (!carrier) {
		observed = AIUS_INPUT_LIVENESS_NO_CARRIER;
	} else if (sink_delta != 0u) {
		observed = AIUS_INPUT_LIVENESS_SINK_BLOCKED;
	} else if (bursts_delta != 0u) {
		observed = AIUS_INPUT_LIVENESS_SOURCE_SILENT;
	} else if (words_delta != 0u) {
		observed = AIUS_INPUT_LIVENESS_STARVED;
	} else {
		observed = AIUS_INPUT_LIVENESS_NO_INPUT;
	}

	/* Strongest evidence wins, so an interval only ever escalates as it is
	 * better characterised. NO_CARRIER never competes with the locked
	 * classes: the boundary above guarantees an interval is wholly one or
	 * wholly the other. */
	if (state->interval_class == AIUS_INPUT_LIVENESS_PRODUCING ||
	    (uint8_t)observed > state->interval_class)
		state->interval_class = (uint8_t)observed;

	result.klass = (AiusInputLivenessClass)state->interval_class;
	result.dark_ms = sample->now_ms - state->interval_start_ms;
	return result;
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_INPUT_LIVENESS_H */
