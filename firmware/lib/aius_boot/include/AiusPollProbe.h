/* Retained probe around fnet_poll(), the non-reentrant FNET service entry
 * this fork historically ran from BOTH a 1 kHz IntervalTimer ISR and a
 * main-loop server spin (NativeEthernetServer.cpp) with no-op FNET mutexes.
 * The probe survives a watchdog reset (DMAMEM/OCRAM is not touched by the
 * Teensy 4 startup), so the first boot after a wedge can name the context
 * and DHCP-client state the previous boot died in. Every update flushes its
 * cache line: a watchdog reset does not write back dirty dcache.
 *
 * Registered in the aius open-work register under the periodic-WDOG entry
 * (~31 min 45 s cadence, DHCP T1-anchored). */

#ifndef AIUS_POLL_PROBE_H
#define AIUS_POLL_PROBE_H

#include <stdint.h>

#define AIUS_POLL_PROBE_MAGIC 0x50504E46u /* 'FNPP' */

/* Poll caller contexts. */
#define AIUS_POLL_CTX_TIMER_ISR 1u
#define AIUS_POLL_CTX_SERVER_SPIN 2u
#define AIUS_POLL_CTX_NETWORK_JOB 3u

typedef struct __attribute__((aligned(32))) AiusPollProbe {
	uint32_t magic;
	/* Live (reset each boot by the decode in beginNoWait). */
	volatile uint32_t depth;      /* non-zero at reset = died inside poll */
	volatile uint32_t max_depth;  /* 2+ = reentry observed */
	volatile uint32_t reentries;  /* count of depth>1 entries */
	volatile uint32_t enters;
	volatile uint32_t exits;
	volatile uint8_t last_context;
	volatile uint8_t dhcp_state_at_enter;
	volatile uint8_t pad0;
	volatile uint8_t pad1;
	volatile uint32_t last_enter_cyccnt;
	/* Sticky wedge record decoded at boot from the previous boot's live
	 * fields; survives until the next wedge overwrites it. */
	volatile uint32_t wedge_seen;
	volatile uint8_t wedge_context;
	volatile uint8_t wedge_dhcp_state;
	volatile uint8_t wedge_pad0;
	volatile uint8_t wedge_pad1;
	volatile uint32_t wedge_depth;
	volatile uint32_t prev_max_depth;
	volatile uint32_t prev_reentries;
} AiusPollProbe;

extern AiusPollProbe aius_poll_probe;

#endif /* AIUS_POLL_PROBE_H */
