#include "aius_producer_yield.h"

#include "aius_boot.h"
#include "aius_producer_yield_platform.h"

/* Placement. The producer path (wait, point, guard, the nested pass, visit
 * begin/end) carries no attribute: on the Teensy that is ITCM, as it must be
 * for code the producer runs. Everything else is FLASHMEM. The attribute sits
 * on each DEFINITION (GCC does not merge a section attribute from a forward
 * declaration). Enforced post-link by test/itcm-placement/verify_target.py. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

AiusProducerYieldState g_aius_producer_yield;

FLASHMEM void aius_producer_yield_configure(uint32_t cycles_per_us,
                                            uint32_t late_us)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	*s = (AiusProducerYieldState){0};
	s->cycles_per_us = cycles_per_us != 0u ? cycles_per_us : 1u;
	s->late_cycles = late_us * s->cycles_per_us;
	/* D-S7: the firmware configures before AudioMemory, so audio is not
	 * ready yet and the first pass (which needs it ready) disarms whatever
	 * the boot left stamped. */
	s->audio_seen = aius_producer_yield_plat_audio_ready() ? 1u : 0u;
}

/* Close the open non-yield segment at `now`. */
static void segment_close(AiusProducerYieldState *s, uint32_t now)
{
	uint32_t seg = now - s->seg_start;
	if (seg > s->nonyield_max_cycles)
		s->nonyield_max_cycles = seg;
	if (seg > s->window_recent_max_cycles)
		s->window_recent_max_cycles = seg;
}

/* The dispatch meter's stretch ends at a yield opportunity (F5). */
static void meter_close(AiusProducerYieldState *s)
{
	if (s->meter_active != 0u) {
		uint32_t seg = aius_producer_yield_plat_cycles() - s->meter_seg_start;
		if (seg > s->meter_max_cycles)
			s->meter_max_cycles = seg;
	}
}

static void meter_open(AiusProducerYieldState *s)
{
	if (s->meter_active != 0u)
		s->meter_seg_start = aius_producer_yield_plat_cycles();
}

FLASHMEM void aius_producer_yield_window_open(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	if (s->window_depth++ == 0u)
		s->seg_start = aius_producer_yield_plat_cycles();
}

FLASHMEM void aius_producer_yield_window_close(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	if (s->window_depth == 0u)
		return;
	if (--s->window_depth == 0u)
		segment_close(s, aius_producer_yield_plat_cycles());
}

bool aius_producer_yield_guard(bool window_or_point)
{
	const AiusProducerYieldState *s = &g_aius_producer_yield;
	/* DTCM terms first, platform probes only when they can still matter. */
	return window_or_point && s->in_producer == 0u &&
	       aius_producer_yield_plat_ipsr() == 0u &&
	       aius_producer_yield_plat_primask() == 0u &&
	       aius_producer_yield_plat_irq_software_on() &&
	       aius_producer_yield_plat_audio_ready() &&
	       !aius_producer_yield_plat_recovery_safe() &&
	       aius_producer_yield_plat_work_pending();
}

/* Re-stamp at producer exit when ingress is still non-empty, so a commit that
 * landed mid-pass or a budget-limited pass is still timed (S3). The flag is
 * cleared first: an ISR commit after that stamps itself and wins. */
static void producer_leave(AiusProducerYieldState *s)
{
	s->in_producer = 0u;
	if (s->pend_armed == 0u && aius_producer_yield_plat_work_pending()) {
		s->pend_stamp = aius_producer_yield_plat_cycles();
		s->pend_armed = 1u;
	}
}

/* Pass start: time the oldest unserved commit into the watermark, then disarm.
 * Shared by nested passes and regular visits (both are "the next pass"). */
static void pend_serve(AiusProducerYieldState *s)
{
	if (s->audio_seen == 0u) {
		/* D-S7: audio just turned ready; a stamp from before is a boot
		 * artefact, not a producer wait. */
		s->audio_seen = 1u;
		s->pend_armed = 0u;
		return;
	}
	if (s->pend_armed != 0u) {
		uint32_t lat = aius_producer_yield_plat_cycles() - s->pend_stamp;
		s->pend_armed = 0u;
		if (lat > s->pend_latency_max_cycles)
			s->pend_latency_max_cycles = lat;
		if (lat > s->late_cycles)
			s->late_passes++;
	}
}

void aius_producer_yield_visit_begin(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	s->in_producer = 1u;
	pend_serve(s);
}

void aius_producer_yield_visit_end(void)
{
	producer_leave(&g_aius_producer_yield);
}

/* One nested pass (E5): AUDIO is recorded and committed before the pass, the
 * saved checkpoint is recorded and committed after, on the one exit path, so a
 * wedge inside reads pv=AUDIO from the prev-boot capture. */
static void nested_pass(AiusProducerYieldState *s)
{
	uint16_t saved = aius_producer_yield_plat_ledger_checkpoint();

	s->in_producer = 1u;
	pend_serve(s);
	aius_producer_yield_plat_ledger_record(AIUS_BOOT_CHECKPOINT_AUDIO);
	aius_producer_yield_plat_run_pass();
	aius_producer_yield_plat_ledger_record(saved);
	producer_leave(s);
}

bool aius_producer_yield_wait(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	bool in_window = s->window_depth != 0u;
	bool ran = false;

	s->hook_calls++;
	if (in_window) {
		segment_close(s, aius_producer_yield_plat_cycles());
		meter_close(s);
	}
	if (aius_producer_yield_guard(in_window)) {
		s->wait_passes++;
		nested_pass(s);
		ran = true;
	}
	/* The next segment starts at hook return, after any pass. */
	if (in_window) {
		s->seg_start = aius_producer_yield_plat_cycles();
		meter_open(s);
	}
	return ran;
}

bool aius_producer_yield_point(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	bool ran = false;
	meter_close(s);
	if (aius_producer_yield_guard(true)) {
		s->point_passes++;
		nested_pass(s);
		ran = true;
	}
	meter_open(s);
	return ran;
}

FLASHMEM void aius_producer_yield_meter_begin(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	s->meter_max_cycles = 0u;
	s->meter_active = 1u;
	s->meter_seg_start = aius_producer_yield_plat_cycles();
}

FLASHMEM uint32_t aius_producer_yield_meter_end_us(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	uint32_t cpu = s->cycles_per_us != 0u ? s->cycles_per_us : 1u;
	meter_close(s);
	s->meter_active = 0u;
	return s->meter_max_cycles / cpu;
}

FLASHMEM uint32_t aius_producer_yield_window_take_max_us(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	uint32_t cpu = s->cycles_per_us != 0u ? s->cycles_per_us : 1u;
	uint32_t us = s->window_recent_max_cycles / cpu;
	s->window_recent_max_cycles = 0u;
	return us;
}

FLASHMEM void aius_producer_yield_get_diag(AiusProducerYieldDiag *out)
{
	const AiusProducerYieldState *s = &g_aius_producer_yield;
	uint32_t cpu = s->cycles_per_us != 0u ? s->cycles_per_us : 1u;
	out->wait_passes = s->wait_passes;
	out->point_passes = s->point_passes;
	out->pend_latency_max_us = s->pend_latency_max_cycles / cpu;
	out->late_passes = s->late_passes;
	out->window_nonyield_max_us = s->nonyield_max_cycles / cpu;
	out->hook_calls = s->hook_calls;
}

FLASHMEM void aius_producer_yield_clear_diag(void)
{
	AiusProducerYieldState *s = &g_aius_producer_yield;
	s->pend_latency_max_cycles = 0u;
	s->nonyield_max_cycles = 0u;
	s->late_passes = 0u;
}
