#ifndef AIUS_TX_GOVERNOR_H
#define AIUS_TX_GOVERNOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The transmit-ring latency governor's decision rule, pure and portable
 * (aius-output-jitter Phase E).
 *
 * WHAT IT REPAIRS. The gate-open trim re-bases the ring's standing occupancy
 * exactly once, at the closed-to-open edge. Nothing re-bases it afterwards, so
 * an output stall (SD contention starving the producer) plays silence, then
 * commits its backlog late and parks the ring's floor up to ~2,048 frames
 * (~43 ms) above the 128-frame target for the rest of the session. The
 * decomposition that established this -- structural ~14-16 ms from the
 * 512-frame PCM quantum, episodic ratchet on top with no regulator -- is
 * recorded in aius-output-jitter.md Phase E.
 *
 * HOW IT DECIDES. The caller feeds every accepted producer block's PRE-commit
 * ring level. The governor keeps the WINDOW MINIMUM of that level over
 * `window_frames` committed frames. The minimum is the floor estimator: burst
 * commits and drain phase inflate individual samples, but over a multi-second
 * window some sample always lands just before a commit, where the level IS the
 * standing floor. A mean would chase the sawtooth; the minimum is phase-immune
 * by construction.
 *
 * When a full window's floor stays above `target_frames + deadband_frames`,
 * the step returns the excess over target -- the caller drops exactly that
 * many of the OLDEST queued frames in ONE masked trim (an audible skip
 * forward, accepted by the operator in preference to a standing 40 ms lag;
 * see the reopened caller contract on aius_spdif_tx24_trim_to). Returning the
 * excess rather than trimming here keeps this header free of the ring type
 * and testable on the host without a harness.
 *
 * WHAT VOIDS A WINDOW. A closed gate resets the estimator (the open edge
 * re-bases the ring itself). An underrun mid-window voids the window: the
 * level hit zero, so the floor was demonstrably not elevated, and the
 * post-episode backlog that DOES elevate it is measured by the next clean
 * window. Both resets also cover "measure fresh after a trim". The deadband
 * tolerates drain jitter around the target so the governor never chases
 * sub-perceptual drift; it exists to kill ratchets, not to servo. */

typedef struct {
	uint32_t window_min_level;  /* UINT32_MAX = no sample this window */
	uint32_t window_frames_seen;
	uint32_t underruns_latch;
} AiusTxGovernor;

static inline void aius_tx_governor_reset(AiusTxGovernor *governor,
                                          uint32_t underruns)
{
	governor->window_min_level = UINT32_MAX;
	governor->window_frames_seen = 0u;
	governor->underruns_latch = underruns;
}

/* One call per ACCEPTED producer block, with the ring level read BEFORE that
 * block is staged. Returns the frame count to trim NOW (0 = no action).
 * `underruns` is the ring's lifetime underrun counter; `gate_open` is the TX
 * gate state at the same instant. */
static inline uint32_t aius_tx_governor_step(AiusTxGovernor *governor,
                                             uint32_t target_frames,
                                             uint32_t deadband_frames,
                                             uint32_t window_frames,
                                             uint32_t precommit_level_frames,
                                             uint32_t block_frames,
                                             uint32_t underruns,
                                             int gate_open)
{
	if (!gate_open || underruns != governor->underruns_latch) {
		aius_tx_governor_reset(governor, underruns);
		return 0u;
	}
	if (precommit_level_frames < governor->window_min_level)
		governor->window_min_level = precommit_level_frames;
	governor->window_frames_seen += block_frames;
	if (governor->window_frames_seen < window_frames)
		return 0u;
	const uint32_t floor_frames = governor->window_min_level;
	aius_tx_governor_reset(governor, underruns);
	if (floor_frames <= target_frames + deadband_frames)
		return 0u;
	return floor_frames - target_frames;
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_TX_GOVERNOR_H */
