/* The gate-open underrun EPISODE record (glitch-timestamping, 2026-09-07).
 *
 * The rows are the facts the instrument exists to make true: consecutive
 * observations carrying an advance are ONE episode with one start stamp and the
 * summed frames, the episode ends on the first static observation after it, the
 * ring keeps the newest eight of them oldest-first, a closed gate ends an
 * episode and discards its own delta, and neither the millisecond clock's wrap
 * nor an operator counter clear can manufacture a phantom. Every step is one
 * accepted producer block, which is the cadence the firmware taps
 * (optical_main.cpp, beside the latency governor). */

#include "aius_underrun_episodes.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

/* One accepted 512-frame block at 48 kHz, the firmware's own observation
 * cadence. Time and the counter are the caller's, so the tests move both
 * explicitly. */
#define BLOCK_MS 11u

/* Step with the gate open, asserting nothing ended. */
static void step_open(AiusUnderrunEpisodes *state, uint32_t now_ms,
                      uint32_t underruns)
{
	assert(aius_underrun_episodes_step(state, now_ms, underruns, 1) == NULL);
}

int main(void)
{
	static AiusUnderrunEpisodes state;

	/* A reset ring holds nothing and answers NULL past its end. */
	{
		aius_underrun_episodes_reset(&state);
		assert(state.count == 0u);
		assert(state.open == 0u);
		assert(aius_underrun_episode(&state, 0u) == NULL);
	}

	/* The seed row: the first step adopts the counter baseline and claims
	 * nothing, so a boot (or a re-arm) that inherits a nonzero lifetime
	 * counter does not read as one enormous episode. */
	{
		aius_underrun_episodes_reset(&state);
		assert(aius_underrun_episodes_step(&state, 1000u, 4096u, 1) ==
		       NULL);
		assert(state.count == 0u);
		assert(state.open == 0u);
		/* Static from there: still nothing. */
		step_open(&state, 1000u + BLOCK_MS, 4096u);
		assert(state.count == 0u);
	}

	/* One burst inside a single service pass: the episode opens and closes
	 * with `ms` 0 -- the observed extent is genuinely sub-cadence -- while
	 * `frames` carries the audible cost exactly. */
	{
		const AiusUnderrunEpisode *ended;
		aius_underrun_episodes_reset(&state);
		step_open(&state, 1000u, 0u);
		step_open(&state, 1011u, 128u); /* opens */
		assert(state.open == 1u);
		ended = aius_underrun_episodes_step(&state, 1022u, 128u, 1);
		assert(ended != NULL);
		assert(ended->start_ms == 1011u);
		assert(ended->ms == 0u);
		assert(ended->frames == 128u);
		assert(ended->gate_open == 1u);
		assert(state.count == 1u);
		assert(state.open == 0u);
		/* Recorded, and the newest is the one that just ended. */
		assert(aius_underrun_episode(&state, 0u) == ended);
		assert(aius_underrun_episode(&state, 1u) == NULL);
		/* The detail a type-14 event carries. */
		assert(aius_underrun_episode_detail(ended) == 128u);
	}

	/* A stall spanning three observations is ONE episode: the frames sum,
	 * the start stamp is the first advance, and the span runs to the LAST
	 * advance rather than to the static observation that closed it. */
	{
		const AiusUnderrunEpisode *ended;
		aius_underrun_episodes_reset(&state);
		step_open(&state, 2000u, 1000u);
		step_open(&state, 2011u, 1128u); /* opens: +128 */
		step_open(&state, 2022u, 1384u); /* +256 */
		step_open(&state, 2033u, 1512u); /* +128 */
		assert(state.count == 0u);       /* still one open episode */
		ended = aius_underrun_episodes_step(&state, 2044u, 1512u, 1);
		assert(ended != NULL);
		assert(ended->start_ms == 2011u);
		assert(ended->ms == 22u); /* 2033 - 2011 */
		assert(ended->frames == 512u);
		assert(ended->gate_open == 1u);
		assert(state.count == 1u);
	}

	/* Two separated episodes are two records, newest first through the
	 * accessor, each with its own stamp and frames. */
	{
		const AiusUnderrunEpisode *first;
		const AiusUnderrunEpisode *second;
		aius_underrun_episodes_reset(&state);
		step_open(&state, 3000u, 0u);
		step_open(&state, 3011u, 64u);
		assert(aius_underrun_episodes_step(&state, 3022u, 64u, 1) !=
		       NULL);
		/* A long clean run in between changes nothing. */
		for (uint32_t at = 3033u; at < 3500u; at += BLOCK_MS)
			step_open(&state, at, 64u);
		step_open(&state, 3500u, 96u);
		assert(aius_underrun_episodes_step(&state, 3511u, 96u, 1) !=
		       NULL);
		assert(state.count == 2u);
		second = aius_underrun_episode(&state, 0u);
		first = aius_underrun_episode(&state, 1u);
		assert(second != NULL && first != NULL);
		assert(first->start_ms == 3011u && first->frames == 64u);
		assert(second->start_ms == 3500u && second->frames == 32u);
		assert(aius_underrun_episode(&state, 2u) == NULL);
	}

	/* The ring overwrites at the ninth: the lifetime count keeps counting,
	 * the reader holds the newest eight, and the oldest survivor is episode
	 * 2 rather than episode 1. */
	{
		uint32_t index;
		aius_underrun_episodes_reset(&state);
		step_open(&state, 0u, 0u);
		for (index = 1u; index <= 9u; index++) {
			const uint32_t at = index * 1000u;
			step_open(&state, at, index * 100u);
			assert(aius_underrun_episodes_step(&state, at + BLOCK_MS,
			                                   index * 100u, 1) != NULL);
		}
		assert(state.count == 9u);
		/* Newest is episode 9; oldest retained is episode 2. */
		assert(aius_underrun_episode(&state, 0u)->start_ms == 9000u);
		assert(aius_underrun_episode(&state, 7u)->start_ms == 2000u);
		assert(aius_underrun_episode(&state, 8u) == NULL);
		/* Every retained record is distinct and 100 frames each. */
		for (index = 0u; index < AIUS_UNDERRUN_EPISODE_DEPTH; index++) {
			const AiusUnderrunEpisode *episode =
				aius_underrun_episode(&state, index);
			assert(episode != NULL);
			assert(episode->frames == 100u);
			assert(episode->start_ms == (9u - index) * 1000u);
		}
	}

	/* A closed gate ends the open episode with gate_open 0 -- the truncation
	 * flag -- and its own delta is discarded rather than counted: the drain
	 * transient a stop or relock causes is not a listening glitch. */
	{
		const AiusUnderrunEpisode *ended;
		aius_underrun_episodes_reset(&state);
		step_open(&state, 4000u, 0u);
		step_open(&state, 4011u, 256u);
		ended = aius_underrun_episodes_step(&state, 4022u, 900u, 0);
		assert(ended != NULL);
		assert(ended->start_ms == 4011u);
		assert(ended->frames == 256u); /* the 644 at the close is not in */
		assert(ended->gate_open == 0u);
		assert(state.count == 1u);
	}

	/* An advance observed entirely under a closed gate opens nothing at all,
	 * and the baseline still tracks it, so the reopen does not then read the
	 * dark-era growth as an episode. */
	{
		aius_underrun_episodes_reset(&state);
		assert(aius_underrun_episodes_step(&state, 5000u, 0u, 0) == NULL);
		assert(aius_underrun_episodes_step(&state, 5011u, 500u, 0) ==
		       NULL);
		assert(aius_underrun_episodes_step(&state, 5022u, 900u, 0) ==
		       NULL);
		assert(state.count == 0u);
		assert(state.open == 0u);
		/* Gate opens with the counter where the dark era left it. */
		step_open(&state, 5033u, 900u);
		assert(state.count == 0u);
		assert(state.open == 0u);
	}

	/* millis() wrap: an episode that opens just before UINT32_MAX and ends
	 * after the wrap reports its true span, because the difference is
	 * unsigned. */
	{
		const AiusUnderrunEpisode *ended;
		const uint32_t before = UINT32_MAX - 10u;
		aius_underrun_episodes_reset(&state);
		step_open(&state, before, 0u);
		step_open(&state, before + 5u, 128u);   /* opens */
		step_open(&state, before + 25u, 256u);  /* wrapped past 0 */
		ended = aius_underrun_episodes_step(&state, before + 40u, 256u, 1);
		assert(ended != NULL);
		assert(ended->start_ms == before + 5u);
		assert(ended->ms == 20u);
		assert(ended->frames == 256u);
	}

	/* Counter wrap: the ring's own u32 underrun counter wrapping is an
	 * ordinary advance, because the delta is the unsigned difference. */
	{
		const AiusUnderrunEpisode *ended;
		aius_underrun_episodes_reset(&state);
		step_open(&state, 6000u, UINT32_MAX - 63u);
		step_open(&state, 6011u, 63u); /* wrapped: a true +127 */
		ended = aius_underrun_episodes_step(&state, 6022u, 63u, 1);
		assert(ended != NULL);
		assert(ended->frames == 127u);
	}

	/* An operator counter clear moves it BACKWARDS. That resyncs and costs
	 * at most one missed episode; it never manufactures a four-billion-frame
	 * phantom, and it never opens one. */
	{
		aius_underrun_episodes_reset(&state);
		step_open(&state, 7000u, 90000u);
		step_open(&state, 7011u, 0u); /* POST /api/clear */
		assert(state.open == 0u);
		assert(state.count == 0u);
		/* And the next real advance is measured from the cleared value. */
		step_open(&state, 7022u, 64u);
		assert(state.open == 1u);
		assert(state.open_frames == 64u);
	}

	/* Saturation: `detail` is 16-bit, and an episode past that reports the
	 * ceiling rather than a truncated (and possibly zero) count. */
	{
		AiusUnderrunEpisode episode = { 0u, 0u, 0u, 0u };
		episode.frames = 65535u;
		assert(aius_underrun_episode_detail(&episode) == 65535u);
		episode.frames = 65536u;
		assert(aius_underrun_episode_detail(&episode) == 65535u);
		episode.frames = 4000000u;
		assert(aius_underrun_episode_detail(&episode) == 65535u);
	}

	puts("Aius underrun-episode record (boundaries, ring, gate, wrap)");
	puts("RESULT: all checks passed");
	return 0;
}
