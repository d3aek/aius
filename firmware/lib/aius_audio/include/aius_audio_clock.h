#ifndef AIUS_AUDIO_CLOCK_H
#define AIUS_AUDIO_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include <aius_clock.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_AUDIO_CLOCK_LOCK_TIMEOUT_US UINT32_C(1000)
#define AIUS_AUDIO_CLOCK_INVALID_PERIODS UINT32_C(4)
#define AIUS_AUDIO_CLOCK_DPLL_GAIN UINT32_C(8)

/* Values shared with the fake target. The target build statically verifies
 * them against the Teensy core's register definitions. */
#define AIUS_AUDIO_CLOCK_PLL_LOCK_BIT UINT32_C(0x80000000)
#define AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT UINT32_C(0x00000020)

typedef enum {
	AIUS_AUDIO_CLOCK_REG_PLL_CONTROL = 0,
	AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR,
	AIUS_AUDIO_CLOCK_REG_PLL_DENOMINATOR,
	AIUS_AUDIO_CLOCK_REG_PLL_MISC2,
	AIUS_AUDIO_CLOCK_REG_CLOCK_GATE_5,
	AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT,
	AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
	AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK,
	AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW,
	AIUS_AUDIO_CLOCK_REG_SPDIF_PHASE_CONFIG,
	AIUS_AUDIO_CLOCK_REG_SPDIF_RATE_MEASUREMENT
} AiusAudioClockRegister;

/* Full-plan target work is driven through this small register/cycle boundary.
 * Production supplies the Teensy registers. The native target suite supplies a
 * trace, including a wrap-safe fake DWT cycle count. */
typedef struct {
	void *context;
	uint32_t (*read_register)(
		void *context,
		AiusAudioClockRegister reg);
	void (*write_register)(
		void *context,
		AiusAudioClockRegister reg,
		uint32_t value);
	uint32_t (*read_cycle_count)(void *context);
	uint32_t cycles_per_microsecond;
	uint32_t bus_frequency_hz;
} AiusAudioClockBackend;

typedef enum {
	AIUS_CLOCK_APPLY_OK = 0,
	AIUS_CLOCK_APPLY_INVALID_PLAN = 1,
	AIUS_CLOCK_APPLY_LOCK_TIMEOUT = 2
} AiusClockApplyResult;

typedef struct {
	bool request_pending;
	bool invalid_active;
	bool transition_complete;
	uint32_t periods_remaining;
} AiusAudioClockBoundaryStatus;

/* Initialise the boundary handshake after installing the TX DMA ISR. */
void aius_audio_clock_initialize(void);

/* Start a nominal/structural dark transition at checkpoint time. The caller
 * closes the physical pad first; this operation then makes IEC validity
 * invalid and cancels any older clearable hold before a ring flush/reset or
 * clock write can occur. */
void aius_audio_clock_begin_dark_transition(void);

/* Apply every structural clock field. Validity is made invalid before the
 * first PLL write. Success publishes the plan and queues a hold-only request
 * for the next TX boundary; timeout leaves PLL4 bypassed and invalid. */
AiusClockApplyResult aius_audio_clock_apply(
	const AiusSpdifClockPlan *plan);

AiusClockApplyResult aius_audio_clock_apply_with_backend(
	const AiusSpdifClockPlan *plan,
	const AiusAudioClockBackend *backend);

/* Queue a live numerator update. The request is accepted only when a complete
 * applied plan exists, its nominal and structural fields match, and no prior
 * boundary request/hold is active. The ISR makes validity invalid before the
 * NUM write and publishes the new applied target at that same boundary. */
bool aius_audio_clock_request_trim(
	const AiusSpdifClockPlan *plan);

/* Race-safe checkpoint snapshots. An applied plan is reported after a full
 * apply succeeds or after the ISR consumes a queued NUM trim. */
bool aius_audio_clock_get_applied_plan(
	AiusSpdifClockPlan *plan);

bool aius_audio_clock_get_boundary_status(
	AiusAudioClockBoundaryStatus *status);

/* Read the locked receiver's physical rate directly from SRFM in millihertz.
 * This preserves sub-hertz information that AudioInputSPDIF3::sampleRate()
 * rounds away. */
bool aius_spdif_input_rate_millihz(uint32_t *rate_millihz);

bool aius_spdif_input_rate_millihz_with_backend(
	const AiusAudioClockBackend *backend,
	uint32_t *rate_millihz);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_AUDIO_CLOCK_H */
