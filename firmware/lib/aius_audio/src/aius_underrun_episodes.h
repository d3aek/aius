#ifndef AIUS_UNDERRUN_EPISODES_H
#define AIUS_UNDERRUN_EPISODES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The gate-open transmit-underrun EPISODE record, pure and portable.
 * Observe-only: nothing here recommends or actuates.
 *
 * WHY A SECOND INSTRUMENT. `g_aius_spdif_tx24.underruns` (aius_spdif_tx24.h)
 * already counts every frame of silence the transmit ISR had to invent under an
 * open gate -- the honest audible-glitch metric -- but it is a boot-lifetime
 * total with no timestamp. Two polls apart say a glitch happened somewhere in
 * between; nothing says WHEN inside that window, how long the run lasted, or
 * whether one poll interval held one burst or thirty. The owner's report is
 * "occasional sub-second glitches during otherwise stable playback", which is
 * precisely the question a lifetime total cannot answer. This groups the
 * counter's advances into episodes, stamps each with the millisecond it began,
 * and keeps the newest eight.
 *
 * WHAT AN EPISODE IS. A run of consecutive observations that carried an
 * advance. It OPENS at the first observation whose delta is nonzero and CLOSES
 * at the first one after it whose delta is zero (or at a closed gate, below).
 * Grouping is therefore a property of the OBSERVATION CADENCE, and the caller
 * picks it deliberately: the firmware steps this once per ACCEPTED producer
 * block, beside the latency governor's tap, which is ~512 frames (~10.7 ms at
 * 48 kHz) apart. That is the finest cadence available outside the transmit DMA
 * ISR itself, and it is coarser than the ISR's own ~2.67 ms fill period, so a
 * stall that starves several consecutive fills coalesces into ONE episode
 * instead of fragmenting into one per fill. Stepping from the epoch loop
 * instead (~6,500 passes/s measured) would do exactly that fragmenting, which
 * is why the cadence is the block and not the epoch.
 *
 * WHAT THE TWO TIME FIELDS MEAN, because they are not the same thing.
 * `start_ms` is the caller's clock at the observation that OPENED the episode:
 * the underrun itself happened somewhere in the service interval preceding it,
 * so the stamp is accurate to one block, not to the frame. `ms` is the span
 * from that observation to the last one that still carried an advance -- the
 * OBSERVED extent, which is 0 for a burst contained inside a single service
 * pass. The audible cost is `frames`, always: frames / sample_rate is the
 * silence a listener heard, and it is exact whatever `ms` says. A 128-frame
 * episode reading `ms` 0 and `frames` 128 is one 2.7 ms glitch reported
 * honestly, not a lost measurement.
 *
 * WHY A CLOSED GATE ENDS AN EPISODE AND DISCARDS ITS DELTA. The ring counts
 * substitution only while the gate is open, so an advance observed at a
 * gate-CLOSED step is the tail of the drain a stop or relock just caused -- the
 * same transient the flight recorder deliberately refuses to trigger on
 * (aius_recorder.c, the replay-stop row). Counting it would put a fake glitch
 * at the end of every session. The episode it closes is recorded with
 * `gate_open` 0, which is the reader's flag that its extent was TRUNCATED by
 * the close rather than ended by the stream recovering.
 *
 * COST. One unsigned subtract, a compare and a few field writes per accepted
 * block; no allocation, no loop except over the fixed ring when a reader
 * serializes it. Counters and millis() are lifetime and wrap; unsigned
 * subtraction is wrap-correct for any true delta below 2^31 (see the resync
 * rule in step()). */

#define AIUS_UNDERRUN_EPISODE_DEPTH 8u

typedef struct {
	uint32_t start_ms; /* the observation that opened it */
	uint32_t ms;       /* to the last observation that carried an advance */
	uint32_t frames;   /* substituted frames the episode accumulated */
	uint8_t gate_open; /* gate state at the observation that ended it */
} AiusUnderrunEpisode;

typedef struct {
	AiusUnderrunEpisode episodes[AIUS_UNDERRUN_EPISODE_DEPTH];
	uint32_t count; /* lifetime episodes ENDED; slot = count % depth */
	uint32_t last_counter;
	uint32_t open_start_ms;
	uint32_t open_last_ms;
	uint32_t open_frames;
	uint8_t open;
	uint8_t seeded;
} AiusUnderrunEpisodes;

static inline void aius_underrun_episodes_reset(AiusUnderrunEpisodes *state)
{
	uint32_t index;

	for (index = 0u; index < AIUS_UNDERRUN_EPISODE_DEPTH; index++) {
		state->episodes[index].start_ms = 0u;
		state->episodes[index].ms = 0u;
		state->episodes[index].frames = 0u;
		state->episodes[index].gate_open = 0u;
	}
	state->count = 0u;
	state->last_counter = 0u;
	state->open_start_ms = 0u;
	state->open_last_ms = 0u;
	state->open_frames = 0u;
	state->open = 0u;
	state->seeded = 0u;
}

/* End the open episode into the ring and return it; NULL when none is open.
 * The returned pointer is the ring slot, so it is stable only until the
 * AIUS_UNDERRUN_EPISODE_DEPTH-th episode after it -- callers read it in the
 * same call (the event post) rather than retaining it. */
static inline const AiusUnderrunEpisode *
aius_underrun_episodes_close(AiusUnderrunEpisodes *state, uint8_t gate_open)
{
	AiusUnderrunEpisode *slot;

	if (!state->open)
		return NULL;
	slot = &state->episodes[state->count % AIUS_UNDERRUN_EPISODE_DEPTH];
	slot->start_ms = state->open_start_ms;
	slot->ms = state->open_last_ms - state->open_start_ms;
	slot->frames = state->open_frames;
	slot->gate_open = gate_open;
	state->count++;
	state->open = 0u;
	return slot;
}

/* One call per observation, with the ring's lifetime underrun counter and the
 * TX gate state read at the same instant. Returns the episode that ENDED at
 * this call (the caller's cue to post its event), or NULL.
 *
 * The first call after a reset seeds the counter baseline: it can observe no
 * delta, so it claims nothing -- which is also what keeps a boot's or a thaw's
 * pre-existing counter value from reading as one enormous episode. */
static inline const AiusUnderrunEpisode *
aius_underrun_episodes_step(AiusUnderrunEpisodes *state, uint32_t now_ms,
                            uint32_t underruns, int gate_open)
{
	uint32_t delta;

	if (!state->seeded) {
		state->last_counter = underruns;
		state->seeded = 1u;
		return NULL;
	}
	delta = underruns - state->last_counter;
	state->last_counter = underruns;
	/* A counter that moved BACKWARDS is an operator clear, not a 4-billion
	 * frame glitch: resync and cost one missed episode, exactly as the
	 * event-edge collapser and the recorder's baselines do. Any true
	 * advance is orders below this bound, so the counter's own 32-bit wrap
	 * still reads as the plain unsigned difference. */
	if (delta > 0x7FFFFFFFu)
		delta = 0u;
	if (!gate_open)
		return aius_underrun_episodes_close(state, 0u);
	if (delta != 0u) {
		if (!state->open) {
			state->open = 1u;
			state->open_start_ms = now_ms;
			state->open_frames = 0u;
		}
		state->open_frames += delta;
		state->open_last_ms = now_ms;
		return NULL;
	}
	return aius_underrun_episodes_close(state, 1u);
}

/* The event ring's 16-bit `detail` for one episode. Saturating, not
 * truncating: a cast would report a 65,536-frame episode as zero, which is the
 * one value a reader would take for "no frames lost". */
static inline uint16_t
aius_underrun_episode_detail(const AiusUnderrunEpisode *episode)
{
	return episode->frames > 0xFFFFu ? (uint16_t)0xFFFFu
	                                 : (uint16_t)episode->frames;
}

/* The recorded episode `index` places back from the newest (0 = newest);
 * NULL past what the ring holds. */
static inline const AiusUnderrunEpisode *
aius_underrun_episode(const AiusUnderrunEpisodes *state, uint32_t index)
{
	const uint32_t held = state->count < AIUS_UNDERRUN_EPISODE_DEPTH
		? state->count
		: AIUS_UNDERRUN_EPISODE_DEPTH;

	if (index >= held)
		return NULL;
	return &state->episodes[(state->count - 1u - index) %
	                        AIUS_UNDERRUN_EPISODE_DEPTH];
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_UNDERRUN_EPISODES_H */
