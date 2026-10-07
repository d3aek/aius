#ifndef AIUS_PRODUCER_YIELD_H
#define AIUS_PRODUCER_YIELD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Producer yield (aius-sample-rate-following.md Phase 4e). While the main loop
 * waits on the SD card between commands, the SdFat hook runs one bounded
 * producer pass so ingress never waits a whole SD stretch. The hook is a
 * thread-mode call, never an interrupt.
 *
 * Placement: aius_producer_yield_wait, _point, _visit_begin/_end and the inline
 * _note_pend run in the producer path (ITCM, no attribute); configure, window
 * open/close, get_diag and clear_diag are FLASHMEM. */

/* D in 4e (us): a nested pass whose pend latency exceeds it counts late. */
#define AIUS_PRODUCER_YIELD_LATE_US_DEFAULT 2500u

typedef struct {
	/* DTCM state, exposed only so note_pend can inline into the ISR. */
	volatile uint8_t in_producer;
	volatile uint8_t pend_armed;
	uint8_t window_depth;
	/* D-S7: 0 until the first pass after a configure that saw audio not
	 * ready; that pass disarms the boot-time stamp instead of timing it. */
	uint8_t audio_seen;
	volatile uint32_t pend_stamp;
	uint32_t seg_start;
	uint32_t pend_latency_max_cycles;
	uint32_t nonyield_max_cycles;
	uint32_t late_cycles;
	uint32_t cycles_per_us;
	uint32_t wait_passes;
	uint32_t point_passes;
	uint32_t late_passes;
	uint32_t hook_calls;
	/* Phase 4f: the longest window segment since the last take (the SD
	 * start cost sample) and the dispatch meter (F5). */
	uint32_t window_recent_max_cycles;
	uint32_t meter_seg_start;
	uint32_t meter_max_cycles;
	uint8_t meter_active;
} AiusProducerYieldState;

extern AiusProducerYieldState g_aius_producer_yield;

typedef struct {
	uint32_t wait_passes;                /* producer_wait_passes */
	uint32_t point_passes;               /* producer_point_passes */
	uint32_t pend_latency_max_us;        /* producer_pend_latency_max_us */
	uint32_t late_passes;                /* producer_late_passes */
	uint32_t window_nonyield_max_us;     /* sd_window_nonyield_max_us */
	uint32_t hook_calls;                 /* hook_calls */
} AiusProducerYieldDiag;

/* Boot: reset all state; cycles_per_us converts the cycle clock (600 on the
 * Teensy 4.1), late_us is D. */
void aius_producer_yield_configure(uint32_t cycles_per_us, uint32_t late_us);

/* ISR (AiusAudioProcessor::update): call after an ingress commit with the
 * cycle counter. Stamps only while the producer is idle and nothing is already
 * stamped, so the stamp is the oldest unserved commit (round 1 B1). */
static inline void aius_producer_yield_note_pend(uint32_t now_cycles)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	if (s->in_producer == 0u && s->pend_armed == 0u) {
		s->pend_stamp = now_cycles;
		s->pend_armed = 1u;
	}
}

/* SD window around the storage execute and the raw scans (nesting allowed).
 * Opening starts the non-yield stretch clock; closing ends it. */
void aius_producer_yield_window_open(void);
void aius_producer_yield_window_close(void);

/* The strong aius_sdio_wait_hook body. Counts a hook call; inside a window and
 * with the guard true runs one nested pass. Returns true if a pass ran. */
bool aius_producer_yield_wait(void);

/* Explicit yield point: the same guard without the window flag. */
bool aius_producer_yield_point(void);

/* The guard alone (window_or_point is the first term). */
bool aius_producer_yield_guard(bool window_or_point);

/* The regular AUDIO visits in the main loop: set the in-producer flag around
 * the producer so a checkpoint inside it never nests. Begin records the
 * latency of a pend it serves (watermark and late count, like a nested pass)
 * and disarms the stamp; end re-stamps when ingress is still non-empty. */
void aius_producer_yield_visit_begin(void);
void aius_producer_yield_visit_end(void);

/* Phase 4f F5, the dispatch meter. Between begin and end every yield
 * opportunity (an explicit point, or a hook call inside an SD window) closes
 * the running stretch and opens the next one after any nested pass returns,
 * so end reports the longest stretch the producer could have waited behind,
 * nested producer time excluded. FLASHMEM. */
void aius_producer_yield_meter_begin(void);
uint32_t aius_producer_yield_meter_end_us(void);

/* Phase 4f F2: the longest SD window segment (4e's sd_window_nonyield
 * definition) closed since the previous take, in us, and reset to 0. 0 when
 * no window segment closed. FLASHMEM. */
uint32_t aius_producer_yield_window_take_max_us(void);

void aius_producer_yield_get_diag(AiusProducerYieldDiag *out);
/* Resets the watermarks (pend latency, non-yield stretch) and late_passes so a
 * run measures only itself; pass counters and hook_calls are lifetime. */
void aius_producer_yield_clear_diag(void);

#ifdef __cplusplus
}
#endif

#endif
