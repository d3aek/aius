#ifndef AIUS_TX_GATE_H
#define AIUS_TX_GATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The optical transmit gate's policy, shared by both actuators.
 *
 * ONE PREDICATE, TWO ACTUATORS. The policy is the final product's contract --
 * no input carrier means no output carrier -- enforced electronically on the
 * carrier board (TX_ENABLE, pad 39, into the 1G126's active-high OE with a
 * 10k pull-down default) and, until that board exists, in firmware on the
 * bench build by muxing pad 14 between its SPDIF_OUT ALT and GPIO driven low.
 * Keeping the predicate here, portable and pure, is what makes the bench path
 * non-throwaway: the carrier firmware consumes the same boolean through a
 * different register.
 *
 * WHY THESE INPUTS, EXACTLY:
 *
 * - `carrier_locked` is the S/PDIF receiver's PLL lock (SPDIF_SRPC LOCK), the
 *   same single volatile read the status surface serves.
 * - `live_ready` proves the accepted carrier rate, applied output clock and
 *   decoded stream agree. A locked but unaccepted or mismatched carrier stays
 *   physically dark.
 *
 * Live input (optical AC-3 and PCM) is the only audio source since replay
 * retired (Phase 4i), so no carrier means no output.
 */
static inline bool aius_tx_gate_permitted(bool carrier_locked,
                                          bool live_ready)
{
	return carrier_locked && live_ready;
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_TX_GATE_H */
