/* The transmit-gate predicate's meaningful truth table.
 *
 * Live readiness is authoritative with a carrier; replay readiness is
 * authoritative without one. Each source is covered at the ready state plus
 * both refusal edges, so a future edit cannot bypass either clock proof. */

#include "aius_tx_gate.h"

#include <assert.h>

int main(void)
{
	/* Carrier locked: the gate is open regardless of the replay source --
	 * passthrough must carry even while a replay is switching or torn down. */
	assert(aius_tx_gate_permitted(true, true, false,
	                              AIUS_REPLAY_SOURCE_OPTICAL));
	assert(aius_tx_gate_permitted(true, true, false,
	                              AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY));
	assert(aius_tx_gate_permitted(true, true, false,
	                              AIUS_REPLAY_SOURCE_REPLAY));
	assert(aius_tx_gate_permitted(true, true, false,
	                              AIUS_REPLAY_SOURCE_SWITCHING_TO_OPTICAL));
	assert(aius_tx_gate_permitted(true, true, false,
	                              AIUS_REPLAY_SOURCE_ERROR));
	assert(!aius_tx_gate_permitted(true, false, true,
	                               AIUS_REPLAY_SOURCE_OPTICAL));

	/* No carrier: only a selected replay opens the gate. Prebuffering counts
	 * (the sink's lock lead time); an error teardown does not; the two
	 * optical-side sources with no lock mean silence, so the output is dark. */
	assert(!aius_tx_gate_permitted(false, false, true,
	                               AIUS_REPLAY_SOURCE_OPTICAL));
	assert(aius_tx_gate_permitted(false, false, true,
	                              AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY));
	assert(aius_tx_gate_permitted(false, false, true,
	                              AIUS_REPLAY_SOURCE_REPLAY));
	assert(!aius_tx_gate_permitted(false, false, true,
	                               AIUS_REPLAY_SOURCE_SWITCHING_TO_OPTICAL));
	assert(!aius_tx_gate_permitted(false, false, true,
	                               AIUS_REPLAY_SOURCE_ERROR));
	assert(!aius_tx_gate_permitted(false, false, false,
	                               AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY));
	assert(!aius_tx_gate_permitted(false, false, false,
	                               AIUS_REPLAY_SOURCE_REPLAY));

	return 0;
}
