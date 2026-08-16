/* The latency governor's decision rule (aius-output-jitter Phase E).
 *
 * The scenarios mirror the two regimes the live decomposition measured: a
 * clean session whose floor rides the 128-frame gate-open target (no trim,
 * ever), and a post-stall session whose floor ratcheted a quantum-plus above
 * it (one trim, of exactly the excess over target). The window, deadband,
 * underrun-void and gate-reset edges are each pinned so a future edit cannot
 * quietly turn the governor into a sawtooth-chasing servo. */

#include "aius_tx_governor.h"

#include <assert.h>

#define TARGET 128u
#define DEADBAND 256u
#define WINDOW 4096u /* 8 x 512-frame blocks per decision, for short rows */
#define BLOCK 512u

/* Drive one full window of identical pre-commit levels; return the decision
 * of the final (window-closing) step. All intermediate steps must be 0. */
static uint32_t drive_flat_window(AiusTxGovernor *governor, uint32_t level,
                                  uint32_t underruns)
{
	uint32_t decision = 0u;
	for (uint32_t frames = 0u; frames < WINDOW; frames += BLOCK) {
		decision = aius_tx_governor_step(governor, TARGET, DEADBAND,
		                                 WINDOW, level, BLOCK,
		                                 underruns, 1);
		if (frames + BLOCK < WINDOW)
			assert(decision == 0u);
	}
	return decision;
}

int main(void)
{
	AiusTxGovernor governor;

	/* Clean floor at the target: a full window decides "no trim". */
	aius_tx_governor_reset(&governor, 0u);
	assert(drive_flat_window(&governor, TARGET, 0u) == 0u);

	/* Sub-deadband elevation is tolerated -- the governor is not a servo. */
	aius_tx_governor_reset(&governor, 0u);
	assert(drive_flat_window(&governor, TARGET + DEADBAND, 0u) == 0u);

	/* Ratcheted floor: sustained TARGET+1536 trims exactly 1536 -- back to
	 * the target in one skip, not to target+deadband. */
	aius_tx_governor_reset(&governor, 0u);
	assert(drive_flat_window(&governor, TARGET + 1536u, 0u) == 1536u);

	/* The minimum, not the mean, is the floor estimator: one low sample in
	 * an otherwise-elevated window vetoes the trim. */
	aius_tx_governor_reset(&governor, 0u);
	assert(aius_tx_governor_step(&governor, TARGET, DEADBAND, WINDOW,
	                             TARGET, BLOCK, 0u, 1) == 0u);
	for (uint32_t frames = BLOCK; frames < WINDOW - BLOCK; frames += BLOCK)
		assert(aius_tx_governor_step(&governor, TARGET, DEADBAND,
		                             WINDOW, TARGET + 2048u, BLOCK,
		                             0u, 1) == 0u);
	assert(aius_tx_governor_step(&governor, TARGET, DEADBAND, WINDOW,
	                             TARGET + 2048u, BLOCK, 0u, 1) == 0u);

	/* An underrun mid-window voids the window: the pre-episode elevation
	 * never converts to a trim, and the counter re-latches so the next
	 * clean window measures the post-episode floor fresh. */
	aius_tx_governor_reset(&governor, 0u);
	for (uint32_t frames = 0u; frames < WINDOW / 2u; frames += BLOCK)
		assert(aius_tx_governor_step(&governor, TARGET, DEADBAND,
		                             WINDOW, TARGET + 1024u, BLOCK,
		                             0u, 1) == 0u);
	assert(aius_tx_governor_step(&governor, TARGET, DEADBAND, WINDOW,
	                             TARGET + 1024u, BLOCK, 7u, 1) == 0u);
	assert(governor.window_frames_seen == 0u);
	assert(governor.underruns_latch == 7u);
	assert(drive_flat_window(&governor, TARGET + 1024u, 7u) == 1024u);

	/* A closed gate resets the estimator; the open edge re-bases the ring
	 * itself, so no decision may carry across the closure. */
	aius_tx_governor_reset(&governor, 0u);
	for (uint32_t frames = 0u; frames < WINDOW / 2u; frames += BLOCK)
		assert(aius_tx_governor_step(&governor, TARGET, DEADBAND,
		                             WINDOW, TARGET + 1024u, BLOCK,
		                             0u, 1) == 0u);
	assert(aius_tx_governor_step(&governor, TARGET, DEADBAND, WINDOW,
	                             TARGET + 1024u, BLOCK, 0u, 0) == 0u);
	assert(governor.window_frames_seen == 0u);

	/* After a trim decision the state is reset: the very next step starts
	 * a fresh window rather than re-deciding on stale samples. */
	aius_tx_governor_reset(&governor, 0u);
	(void)drive_flat_window(&governor, TARGET + 1536u, 0u);
	assert(governor.window_min_level == UINT32_MAX);
	assert(governor.window_frames_seen == 0u);

	/* Window arithmetic: an oversize final block still closes the window
	 * (frames_seen >= window, not ==). */
	aius_tx_governor_reset(&governor, 0u);
	assert(aius_tx_governor_step(&governor, TARGET, DEADBAND, WINDOW,
	                             TARGET + 512u, WINDOW - 1u, 0u,
	                             1) == 0u);
	assert(aius_tx_governor_step(&governor, TARGET, DEADBAND, WINDOW,
	                             TARGET + 512u, BLOCK, 0u, 1) == 512u);

	return 0;
}
