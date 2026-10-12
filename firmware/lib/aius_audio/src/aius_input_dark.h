#ifndef AIUS_INPUT_DARK_H
#define AIUS_INPUT_DARK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The dark-interval record and the gate-closed watermark, pure and portable
 * (aius-input-recovery Phase 1). Observe-only: nothing here recommends or
 * actuates, and the liveness rule beside it (aius_input_liveness.h) stays
 * uncalled.
 *
 * WHY A SECOND INSTRUMENT. The production-gap watermark in optical_main.cpp
 * measures micros() between blocks ACCEPTED at enqueue with the transmit gate
 * open, and resets its sentinel whenever the gate is closed so a source or rate
 * switch never spans a bogus gap. Every stuck state the plan inventories lives
 * inside the region that reset discards, and two of them never reach enqueue
 * at all (a block refused at the plan or rate guard, or a class latched silent
 * with no block decoded). So this instrument is driven from the epoch, once per
 * SERVICE checkpoint, from state the epoch already latches, and asks two
 * questions the production gap cannot:
 *
 *   PHASE   which guard is the transmit path waiting behind right now? The
 *           gate's ladder, read from the clock latches in the order the
 *           firmware itself tests them: carrier lock, then the rate the
 *           tracker refused, then the tracker's acceptance, then the applied
 *           plan, then the first landed block (`g_clock_live_ready`), then the
 *           armed open the producer has not yet executed (Phase F), then open.
 *           One phase per epoch; a contiguous run of one phase with the gate
 *           closed is timed and latched into that phase's watermark when the
 *           run ends. Max, over-count, last over-threshold duration and its
 *           time, the production gap's own shape, per phase. Hours with the
 *           entertainment stack off land in NO_CARRIER's bucket and nowhere
 *           else, which is what keeps every locked-carrier bucket honest.
 *
 *   RECORD  what did each dark interval look like when it began and when it
 *           ended? A dark interval runs from the last epoch that committed an
 *           output block to the next one that does, the same boundary the
 *           liveness rule uses, so production stopping under an OPEN gate (the
 *           class-latched-silent shape, invisible to a gate-edge instrument) is
 *           an interval like any other. Its entry and exit snapshots carry the
 *           phase, the pipeline class, the tracker's acceptance and the
 *           physical gate. Intervals shorter than AIUS_INPUT_DARK_RECORD_MIN_MS
 *           are not recorded: an AC-3 sync frame is ~32 ms apart and the worst
 *           legitimate E-AC-3 outcome gap is ~256 ms, so 500 ms is the floor
 *           above which an interval says something about the box rather than
 *           the codec. The record is a fixed ring of the newest eight; the
 *           lifetime count says how many it has overwritten.
 *
 * THRESHOLDS. AIUS_INPUT_DARK_ALERT_MS is the plan's proposed steady-phase
 * dark-with-carrier bound, 3 s: a legitimate acquisition needs the tracker's
 * AIUS_CLOCK_ACCEPT_SAMPLES over AIUS_CLOCK_ACCEPT_STABLE_MS (1 s) plus the
 * PCM confirmation squelch or a compressed starvation window (~170 ms each),
 * honestly about 1.5 s, so a run past 3 s is over-counted. Both thresholds are
 * served so a reader never has to know the build.
 *
 * COST. Field writes and compares only; no allocation, no loop over anything
 * but the fixed ring when a reader serializes it. Counters and millis() are
 * lifetime and wrap; unsigned subtraction is wrap-correct for any true delta
 * below 2^32. */

#ifndef AIUS_INPUT_DARK_ALERT_MS
#define AIUS_INPUT_DARK_ALERT_MS 3000u
#endif

#ifndef AIUS_INPUT_DARK_RECORD_MIN_MS
#define AIUS_INPUT_DARK_RECORD_MIN_MS 500u
#endif

#define AIUS_INPUT_DARK_RECORD_DEPTH 8u

typedef enum {
	/* Receiver PLL unlocked. The stack-off hours; never a fault here. */
	AIUS_INPUT_DARK_NO_CARRIER = 0,
	/* Locked at a measured rate the tracker refused (stuck state 3). */
	AIUS_INPUT_DARK_RATE_UNSUPPORTED = 1,
	/* Locked, rate supported, tracker has not accepted a nominal yet. */
	AIUS_INPUT_DARK_CLOCK_ACQUIRING = 2,
	/* Nominal accepted, output plan not yet applied. */
	AIUS_INPUT_DARK_PLAN_PENDING = 3,
	/* Plan applied, no decoded block has cleared enqueue's guards
	 * (stuck states 1 and 2 both park here). */
	AIUS_INPUT_DARK_AWAITING_BLOCK = 4,
	/* A block landed and armed the open; the producer has not executed it. */
	AIUS_INPUT_DARK_GATE_ARMED = 5,
	/* The physical gate is open. Not a gate-closed phase. */
	AIUS_INPUT_DARK_GATE_OPEN = 6,
	AIUS_INPUT_DARK_PHASE_COUNT = 7
} AiusInputDarkPhase;

/* The phases that have a watermark: everything below GATE_OPEN. */
#define AIUS_INPUT_DARK_CLOSED_PHASES 6u

typedef struct {
	uint32_t now_ms;
	uint32_t committed_blocks; /* pipeline service diag, lifetime */
	uint8_t carrier_locked;
	uint8_t gate_open;         /* the physical transmit gate */
	uint8_t carrier_ready;     /* tracker accepted a nominal */
	uint8_t plan_applied;
	uint8_t live_ready;        /* a block has landed at the accepted rate */
	uint8_t rate_unsupported;  /* tracker refused the measured rate */
	uint8_t pipeline_class;    /* AiusPipelineClass, carried opaquely */
} AiusInputDarkSample;

typedef struct {
	uint8_t phase;
	uint8_t klass;
	uint8_t carrier_ready;
	uint8_t gate;
} AiusInputDarkSnapshot;

typedef struct {
	uint32_t max_ms;
	uint32_t over_count;
	uint32_t last_ms;    /* the most recent over-threshold run */
	uint32_t last_at_ms; /* now_ms when it ended */
} AiusInputDarkWatermark;

typedef struct {
	uint32_t start_ms;    /* the last epoch that committed before it */
	uint32_t duration_ms;
	AiusInputDarkSnapshot entry;
	AiusInputDarkSnapshot exit;
} AiusInputDarkInterval;

typedef struct {
	AiusInputDarkWatermark phases[AIUS_INPUT_DARK_CLOSED_PHASES];
	AiusInputDarkInterval intervals[AIUS_INPUT_DARK_RECORD_DEPTH];
	uint32_t interval_count;    /* lifetime recorded; slot = count % depth */
	uint32_t last_committed;
	uint32_t last_commit_ms;    /* the epoch that last observed a commit */
	uint32_t phase_start_ms;
	AiusInputDarkSnapshot interval_entry;
	uint8_t phase;
	uint8_t interval_open;
	uint8_t seeded;
} AiusInputDark;

static inline void aius_input_dark_reset(AiusInputDark *state)
{
	uint32_t index;

	for (index = 0u; index < AIUS_INPUT_DARK_CLOSED_PHASES; index++) {
		state->phases[index].max_ms = 0u;
		state->phases[index].over_count = 0u;
		state->phases[index].last_ms = 0u;
		state->phases[index].last_at_ms = 0u;
	}
	for (index = 0u; index < AIUS_INPUT_DARK_RECORD_DEPTH; index++) {
		state->intervals[index].start_ms = 0u;
		state->intervals[index].duration_ms = 0u;
		state->intervals[index].entry.phase = 0u;
		state->intervals[index].entry.klass = 0u;
		state->intervals[index].entry.carrier_ready = 0u;
		state->intervals[index].entry.gate = 0u;
		state->intervals[index].exit = state->intervals[index].entry;
	}
	state->interval_count = 0u;
	state->last_committed = 0u;
	state->last_commit_ms = 0u;
	state->phase_start_ms = 0u;
	state->interval_entry.phase = 0u;
	state->interval_entry.klass = 0u;
	state->interval_entry.carrier_ready = 0u;
	state->interval_entry.gate = 0u;
	state->phase = (uint8_t)AIUS_INPUT_DARK_NO_CARRIER;
	state->interval_open = 0u;
	state->seeded = 0u;
}

/* The ladder position this sample describes, tested in the firmware's own
 * guard order. */
static inline AiusInputDarkPhase
aius_input_dark_phase(const AiusInputDarkSample *sample)
{
	if (sample->gate_open)
		return AIUS_INPUT_DARK_GATE_OPEN;
	if (!sample->carrier_locked)
		return AIUS_INPUT_DARK_NO_CARRIER;
	if (sample->rate_unsupported)
		return AIUS_INPUT_DARK_RATE_UNSUPPORTED;
	if (!sample->carrier_ready)
		return AIUS_INPUT_DARK_CLOCK_ACQUIRING;
	if (!sample->plan_applied)
		return AIUS_INPUT_DARK_PLAN_PENDING;
	if (!sample->live_ready)
		return AIUS_INPUT_DARK_AWAITING_BLOCK;
	return AIUS_INPUT_DARK_GATE_ARMED;
}

static inline const char *aius_input_dark_phase_name(uint32_t phase)
{
	static const char *const names[AIUS_INPUT_DARK_PHASE_COUNT] = {
		"no_carrier",       "rate_unsupported", "clock_acquiring",
		"plan_pending",     "awaiting_block",   "gate_armed",
		"gate_open",
	};

	return phase < AIUS_INPUT_DARK_PHASE_COUNT ? names[phase] : "invalid";
}

static inline void aius_input_dark_latch(AiusInputDarkWatermark *watermark,
                                         uint32_t run_ms, uint32_t now_ms)
{
	if (run_ms > watermark->max_ms)
		watermark->max_ms = run_ms;
	if (run_ms > AIUS_INPUT_DARK_ALERT_MS) {
		watermark->over_count++;
		watermark->last_ms = run_ms;
		watermark->last_at_ms = now_ms;
	}
}

/* One call per epoch. The first call after a reset seeds the counter baseline
 * and the phase clock; it can observe no delta, so it claims nothing. */
static inline void aius_input_dark_step(AiusInputDark *state,
                                        const AiusInputDarkSample *sample)
{
	const AiusInputDarkPhase phase = aius_input_dark_phase(sample);
	AiusInputDarkSnapshot snapshot;
	uint32_t committed_delta;

	snapshot.phase = (uint8_t)phase;
	snapshot.klass = sample->pipeline_class;
	snapshot.carrier_ready = sample->carrier_ready ? 1u : 0u;
	snapshot.gate = sample->gate_open ? 1u : 0u;

	if (!state->seeded) {
		state->last_committed = sample->committed_blocks;
		state->last_commit_ms = sample->now_ms;
		state->phase = (uint8_t)phase;
		state->phase_start_ms = sample->now_ms;
		state->interval_open = 0u;
		state->seeded = 1u;
		return;
	}

	committed_delta = sample->committed_blocks - state->last_committed;
	state->last_committed = sample->committed_blocks;

	/* The gate-closed watermark: a phase run ends when the phase moves.
	 * GATE_OPEN runs are the production gap's region and are not latched. */
	if ((uint8_t)phase != state->phase) {
		if (state->phase < AIUS_INPUT_DARK_CLOSED_PHASES)
			aius_input_dark_latch(&state->phases[state->phase],
			                      sample->now_ms - state->phase_start_ms,
			                      sample->now_ms);
		state->phase = (uint8_t)phase;
		state->phase_start_ms = sample->now_ms;
	}

	/* The record: a commit closes the open interval; the first epoch without
	 * one opens it, timed from the last commit. */
	if (committed_delta != 0u) {
		if (state->interval_open) {
			const uint32_t duration_ms =
				sample->now_ms - state->last_commit_ms;
			if (duration_ms >= AIUS_INPUT_DARK_RECORD_MIN_MS) {
				AiusInputDarkInterval *slot =
					&state->intervals[state->interval_count %
					                  AIUS_INPUT_DARK_RECORD_DEPTH];
				slot->start_ms = state->last_commit_ms;
				slot->duration_ms = duration_ms;
				slot->entry = state->interval_entry;
				slot->exit = snapshot;
				state->interval_count++;
			}
			state->interval_open = 0u;
		}
		state->last_commit_ms = sample->now_ms;
		return;
	}
	if (!state->interval_open) {
		state->interval_open = 1u;
		state->interval_entry = snapshot;
	}
}

/* How long the current phase run has lasted. */
static inline uint32_t aius_input_dark_phase_ms(const AiusInputDark *state,
                                                uint32_t now_ms)
{
	return state->seeded ? now_ms - state->phase_start_ms : 0u;
}

/* How long the open dark interval has lasted; 0 when none is open. */
static inline uint32_t aius_input_dark_open_ms(const AiusInputDark *state,
                                               uint32_t now_ms)
{
	return state->interval_open ? now_ms - state->last_commit_ms : 0u;
}

/* The recorded interval `index` places back from the newest (0 = newest);
 * NULL past what the ring holds. */
static inline const AiusInputDarkInterval *
aius_input_dark_interval(const AiusInputDark *state, uint32_t index)
{
	const uint32_t held = state->interval_count < AIUS_INPUT_DARK_RECORD_DEPTH
		? state->interval_count
		: AIUS_INPUT_DARK_RECORD_DEPTH;

	if (index >= held)
		return NULL;
	return &state->intervals[(state->interval_count - 1u - index) %
	                         AIUS_INPUT_DARK_RECORD_DEPTH];
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_INPUT_DARK_H */
