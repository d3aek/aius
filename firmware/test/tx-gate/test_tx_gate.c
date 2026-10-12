/* The transmit-gate predicate's truth table.
 *
 * Live readiness is authoritative with a carrier, and no carrier means a dark
 * output: live input is the only audio source since replay retired (Phase 4i).
 * Every input combination is covered, so a future edit cannot open the gate
 * without both a lock and the live clock proof. */

#include "aius_tx_gate.h"

#include <assert.h>

int main(void)
{
	/* Carrier locked: live readiness decides. */
	assert(aius_tx_gate_permitted(true, true));
	assert(!aius_tx_gate_permitted(true, false));

	/* No carrier: the output is dark whatever live readiness claims. */
	assert(!aius_tx_gate_permitted(false, true));
	assert(!aius_tx_gate_permitted(false, false));

	return 0;
}
