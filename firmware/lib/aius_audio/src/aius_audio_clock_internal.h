#ifndef AIUS_AUDIO_CLOCK_INTERNAL_H
#define AIUS_AUDIO_CLOCK_INTERNAL_H

#include <stdint.h>

#include <aius_audio_clock.h>

typedef enum {
	AIUS_AUDIO_CLOCK_BOUNDARY_NONE = 0,
	AIUS_AUDIO_CLOCK_BOUNDARY_HOLD = 1,
	AIUS_AUDIO_CLOCK_BOUNDARY_TRIM = 2
} AiusAudioClockBoundaryRequest;

/* This state is defined beside the TX ISR. Every field shared between the
 * checkpoint and ISR is volatile; the sequence words make multi-field
 * checkpoint snapshots coherent without delaying the hot path. */
typedef struct {
	volatile uint32_t boundary_sequence;
	volatile uint32_t request;
	volatile uint32_t invalid_periods_remaining;
	volatile uint32_t invalid_active;
	volatile uint32_t applied_sequence;
	volatile uint32_t has_applied_plan;
	volatile AiusSpdifClockPlan applied_plan;
	volatile AiusSpdifClockPlan queued_plan;
	/* AiusAudioClockTrimKind of the queued TRIM; picks only which applied-at
	 * stamp the boundary writes. Written by request_trim after its pending
	 * check and before `request`, read by the ISR only for a TRIM. */
	volatile uint32_t queued_kind;
} AiusAudioClockSharedState;

extern volatile AiusAudioClockSharedState g_aius_audio_clock_state;

/* millis() at the TX boundary that consumed a CENTRE (trim) or FILL (fill)
 * trim; 0 = none this boot. Defined in aius_spdif_sources.cpp, in the FAKE
 * build too, so clock-target observes the stamp. Contracts: aius_spdif_tx24.h. */
#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint32_t g_aius_tx_trim_applied_at_ms;
extern volatile uint32_t g_aius_tx_fill_applied_at_ms;
#ifdef __cplusplus
}
#endif

/* The boundary ISR's clock. static inline so the ITCM ISR never calls into
 * flash: millis() is the core's inline read of systick_millis_count on
 * target, and a test-set variable under the fake target. */
#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
extern volatile uint32_t g_aius_audio_clock_fake_now_ms;
static inline uint32_t aius_audio_clock_now_ms(void)
{
	return g_aius_audio_clock_fake_now_ms;
}
#elif defined(__IMXRT1052__) || defined(__IMXRT1062__)
#include <Arduino.h>
static inline uint32_t aius_audio_clock_now_ms(void)
{
	return millis();
}
#endif

#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
uint32_t aius_audio_clock_fake_register_read(AiusAudioClockRegister reg);
void aius_audio_clock_fake_register_write(
	AiusAudioClockRegister reg,
	uint32_t value);
void aius_audio_clock_test_tx_boundary(void);
void aius_audio_clock_fake_set_now_ms(uint32_t now_ms);
#endif

#endif /* AIUS_AUDIO_CLOCK_INTERNAL_H */
