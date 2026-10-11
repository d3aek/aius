#ifndef AIUS_INPUT_RX_H
#define AIUS_INPUT_RX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The dark-input receiver state instrument (aius-output-jitter Phase L), pure
 * and portable. Observe-only: nothing reads it back to act.
 *
 * The three states the owner saw as counter rates only: SIS SYMERR alone,
 * SYMERR with BITERR, and SYMERR with BITERR and LOCKLOSS. Stepped once per
 * REPLAY epoch by input_fault_service() (about 6,500/s, see aius_recorder.h)
 * from the SIS error subset that epoch latched. Everything is judged over one
 * AIUS_INPUT_RX_WINDOW_MS window, never per epoch:
 *   - rx_state is the class of the OR of the error bits seen in the last
 *     completed window, so sporadic single-epoch errors do not flicker it;
 *   - rx_state_ms (state_since_ms) is stamped with millis() when that
 *     windowed class changes. The first completed window adopts its class
 *     without a stamp (no predecessor is known);
 *   - lock_permille is true SRPC LOCK reads * 1000 / epochs in the window
 *     (0..1000), a ratio, so the epoch rate does not matter: a slower loop
 *     does not read as a flickering lock. A window that closes late (a
 *     stalled epoch) still reports its own ratio.
 *
 * COST. Field writes and compares; no SD, no blocking, no allocation. */

#define AIUS_INPUT_RX_WINDOW_MS 1000u

#define AIUS_INPUT_RX_BIT_SYMERR 1u
#define AIUS_INPUT_RX_BIT_BITERR 2u
#define AIUS_INPUT_RX_BIT_LOCKLOSS 4u

typedef enum {
	AIUS_INPUT_RX_CLEAN = 0,                 /* none of the three latched */
	AIUS_INPUT_RX_SYMERR = 1,                /* SYMERR only */
	AIUS_INPUT_RX_SYMERR_BITERR = 2,         /* SYMERR and BITERR */
	AIUS_INPUT_RX_SYMERR_BITERR_LOCKLOSS = 3, /* all three */
	AIUS_INPUT_RX_OTHER = 4                  /* any other combination */
} AiusInputRxState;

typedef struct {
	uint32_t state_since_ms; /* millis() of the last windowed class change; 0 = none */
	uint32_t window_start_ms;
	uint32_t window_epochs;      /* epochs stepped in the open window */
	uint32_t window_lock_reads;  /* of those, LOCK true */
	uint16_t lock_permille;      /* true LOCK reads per 1000 epochs, last window */
	uint8_t window_errors;       /* OR of AIUS_INPUT_RX_BIT_* in the open window */
	uint8_t rx_state;            /* class of the last completed window */
	uint8_t seeded;              /* 0 none, 1 window open, 2 class adopted */
} AiusInputRx;

static inline void aius_input_rx_reset(AiusInputRx *rx)
{
	rx->state_since_ms = 0u;
	rx->window_start_ms = 0u;
	rx->window_epochs = 0u;
	rx->window_lock_reads = 0u;
	rx->lock_permille = 0u;
	rx->window_errors = 0u;
	rx->rx_state = (uint8_t)AIUS_INPUT_RX_CLEAN;
	rx->seeded = 0u;
}

static inline AiusInputRxState aius_input_rx_classify(uint8_t symerr,
                                                      uint8_t biterr,
                                                      uint8_t lockloss)
{
	if (!symerr && !biterr && !lockloss)
		return AIUS_INPUT_RX_CLEAN;
	if (symerr && !biterr && !lockloss)
		return AIUS_INPUT_RX_SYMERR;
	if (symerr && biterr && !lockloss)
		return AIUS_INPUT_RX_SYMERR_BITERR;
	if (symerr && biterr && lockloss)
		return AIUS_INPUT_RX_SYMERR_BITERR_LOCKLOSS;
	return AIUS_INPUT_RX_OTHER;
}

/* One call per epoch. The first call opens the first window. */
static inline void aius_input_rx_step(AiusInputRx *rx, uint32_t now_ms,
                                      uint8_t symerr, uint8_t biterr,
                                      uint8_t lockloss, uint8_t srpc_lock)
{
	if (!rx->seeded) {
		rx->window_start_ms = now_ms;
		rx->window_epochs = 0u;
		rx->window_lock_reads = 0u;
		rx->window_errors = 0u;
		rx->seeded = 1u;
	}
	/* Counts freeze at 4e6 epochs (~10 min at the epoch rate) so reads * 1000
	 * stays in 32 bits: no 64-bit division in the epoch path. */
	if (rx->window_epochs < 4000000u) {
		rx->window_epochs++;
		if (srpc_lock)
			rx->window_lock_reads++;
	}
	if (symerr)
		rx->window_errors |= AIUS_INPUT_RX_BIT_SYMERR;
	if (biterr)
		rx->window_errors |= AIUS_INPUT_RX_BIT_BITERR;
	if (lockloss)
		rx->window_errors |= AIUS_INPUT_RX_BIT_LOCKLOSS;
	if ((uint32_t)(now_ms - rx->window_start_ms) >= AIUS_INPUT_RX_WINDOW_MS) {
		const uint8_t state = (uint8_t)aius_input_rx_classify(
			(rx->window_errors & AIUS_INPUT_RX_BIT_SYMERR) ? 1u : 0u,
			(rx->window_errors & AIUS_INPUT_RX_BIT_BITERR) ? 1u : 0u,
			(rx->window_errors & AIUS_INPUT_RX_BIT_LOCKLOSS) ? 1u : 0u);
		/* epochs >= 1 here (this call counted itself); reads <= epochs. */
		rx->lock_permille = (uint16_t)(
			(rx->window_lock_reads * 1000u) / rx->window_epochs);
		if (rx->seeded < 2u)
			rx->seeded = 2u;
		else if (state != rx->rx_state)
			rx->state_since_ms = now_ms;
		rx->rx_state = state;
		rx->window_start_ms = now_ms;
		rx->window_epochs = 0u;
		rx->window_lock_reads = 0u;
		rx->window_errors = 0u;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_INPUT_RX_H */
