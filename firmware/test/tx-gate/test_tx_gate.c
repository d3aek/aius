/* The transmit-gate predicate's whole truth table.
 *
 * Ten rows is the entire input space (2 lock states x 5 sources), so the table
 * IS the test rather than a sample of it: any future edit to the predicate
 * changes at least one row here, and the row's comment says which contract
 * clause the expectation comes from (aius-optical-prototype.md "Firmware
 * carrier management"). */

#include "aius_tx_gate.h"

#include <assert.h>

int main(void)
{
	/* Carrier locked: the gate is open regardless of the replay source --
	 * passthrough must carry even while a replay is switching or torn down. */
	assert(aius_tx_gate_permitted(true, true, AIUS_REPLAY_SOURCE_OPTICAL));
	assert(aius_tx_gate_permitted(true, true,
	                              AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY));
	assert(aius_tx_gate_permitted(true, true, AIUS_REPLAY_SOURCE_REPLAY));
	assert(aius_tx_gate_permitted(true, true,
	                              AIUS_REPLAY_SOURCE_SWITCHING_TO_OPTICAL));
	assert(aius_tx_gate_permitted(true, true, AIUS_REPLAY_SOURCE_ERROR));
	assert(!aius_tx_gate_permitted(true, false, AIUS_REPLAY_SOURCE_OPTICAL));

	/* No carrier: only a selected replay opens the gate. Prebuffering counts
	 * (the sink's lock lead time); an error teardown does not; the two
	 * optical-side sources with no lock mean silence, so the output is dark. */
	assert(!aius_tx_gate_permitted(false, false, AIUS_REPLAY_SOURCE_OPTICAL));
	assert(aius_tx_gate_permitted(false, false,
	                              AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY));
	assert(aius_tx_gate_permitted(false, false, AIUS_REPLAY_SOURCE_REPLAY));
	assert(!aius_tx_gate_permitted(false, false,
	                               AIUS_REPLAY_SOURCE_SWITCHING_TO_OPTICAL));
	assert(!aius_tx_gate_permitted(false, false, AIUS_REPLAY_SOURCE_ERROR));

	return 0;
}
