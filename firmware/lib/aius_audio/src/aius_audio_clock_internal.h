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
} AiusAudioClockSharedState;

extern volatile AiusAudioClockSharedState g_aius_audio_clock_state;

#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
uint32_t aius_audio_clock_fake_register_read(AiusAudioClockRegister reg);
void aius_audio_clock_fake_register_write(
	AiusAudioClockRegister reg,
	uint32_t value);
void aius_audio_clock_test_tx_boundary(void);
#endif

#endif /* AIUS_AUDIO_CLOCK_INTERNAL_H */
