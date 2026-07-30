#ifndef AIUS_TX_GATE_H
#define AIUS_TX_GATE_H

#include <stdbool.h>

#include "aius_replay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The optical transmit gate's policy, shared by both actuators.
 *
 * ONE PREDICATE, TWO ACTUATORS. The policy is the final product's contract --
 * no input carrier and no replay means no output carrier -- enforced
 * electronically on the carrier board (TX_ENABLE, pad 39, into the 1G126's
 * active-high OE with a 10k pull-down default) and, until that board exists,
 * in firmware on the bench build by muxing pad 14 between its SPDIF_OUT ALT
 * and GPIO driven low. Keeping the predicate here, portable and pure, is what
 * makes the bench path non-throwaway: the carrier firmware consumes the same
 * boolean through a different register.
 *
 * WHY THESE INPUTS, EXACTLY:
 *
 * - `carrier_locked` is the S/PDIF receiver's PLL lock (SPDIF_SRPC LOCK), the
 *   same single volatile read the status surface serves.
 * - `live_ready` proves the accepted carrier rate, applied output clock and
 *   decoded stream agree. A locked but unaccepted or mismatched carrier stays
 *   physically dark.
 * - `replay_ready` proves the exact 48 kHz replay plan is applied and its
 *   transition hold is complete. A selected replay cannot leak the last live
 *   clock or open after a failed apply.
 * - `SWITCHING_TO_REPLAY` counts as running ON PURPOSE: the prebuffer phase
 *   is the sink's lock lead time. Opening the gate only at SOURCE_REPLAY
 *   would hand the sink audio in the same instant it first sees a carrier,
 *   and every consumer S/PDIF receiver mutes for tens of milliseconds after
 *   lock.
 * - `SOURCE_ERROR` does not count: a replay torn down by the refill fault
 *   limit is not a reason to carry.
 *
 * ACCEPTED RESIDUE (do not "fix" without reopening the contract): nothing
 * auto-stops an EOF'd replay -- its source stays SOURCE_REPLAY until an
 * operator stop or carrier displacement -- so a drained replay keeps a
 * silence carrier up. The state-based alternative would cut the final ~43 ms
 * still draining the tx24 ring. Recorded in aius-optical-prototype.md
 * "Firmware carrier management".
 */
static inline bool aius_tx_gate_permitted(bool carrier_locked,
                                          bool live_ready,
                                          bool replay_ready,
                                          AiusReplaySource source)
{
	if (carrier_locked)
		return live_ready;
	return replay_ready &&
	       (source == AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY ||
	        source == AIUS_REPLAY_SOURCE_REPLAY);
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_TX_GATE_H */
