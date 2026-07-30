#ifndef AIUS_CLOCK_H
#define AIUS_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_CLOCK_PLL_DENOMINATOR UINT32_C(1000000)
/* Nominal membership can settle before the receiver's physical-rate estimate.
 * A supported candidate needs both the count and this bounded one-second span
 * before its target is safe to apply to the 512-frame transmit ring. */
#define AIUS_CLOCK_ACCEPT_SAMPLES UINT32_C(8)
#define AIUS_CLOCK_ACCEPT_STABLE_MS UINT32_C(1000)
#define AIUS_CLOCK_ACCEPT_SPAN_MILLIHZ UINT32_C(1000)
#define AIUS_CLOCK_STCSCL_RATE_44100 UINT32_C(0x00000000)
#define AIUS_CLOCK_STCSCL_RATE_48000 UINT32_C(0x00400000)
#define AIUS_CLOCK_STCSCL_RATE_32000 UINT32_C(0x00c00000)

typedef enum {
	AIUS_CLOCK_SOURCE_NONE = 0,
	AIUS_CLOCK_SOURCE_LIVE = 1,
	AIUS_CLOCK_SOURCE_REPLAY = 2
} AiusClockSourceState;

typedef struct {
	bool carrier_locked;
	uint32_t carrier_rate_millihz;
	AiusClockSourceState source;
	uint32_t now_ms;
} AiusRateTrackerSample;

typedef struct {
	bool carrier_ready;
	bool quiesce_requested;
	bool nominal_accepted;
	bool nominal_changed;
	bool target_update;
	bool unsupported_accepted;
	uint32_t nominal_rate_hz;
	uint32_t target_rate_millihz;
} AiusRateTrackerResult;

typedef struct {
	uint32_t accepted_nominal_hz;
	uint32_t applied_target_millihz;
	uint32_t candidate_nominal_hz;
	uint32_t candidate_count;
	uint64_t candidate_sum_millihz;
	uint32_t candidate_start_ms;
	uint32_t candidate_min_millihz;
	uint32_t candidate_max_millihz;
	uint32_t last_nominal_accept_ms;
	uint32_t last_target_request_ms;
	uint32_t window_start_ms;
	uint32_t window_count;
	uint64_t window_sum_millihz;
	bool carrier_ready;
	bool has_nominal_accept;
	bool has_window;
	bool unsupported_candidate;
	bool unsupported_episode_reported;
} AiusRateTracker;

typedef struct {
	uint32_t nominal_rate_hz;
	uint32_t target_rate_millihz;
	uint32_t pll_div_select;
	uint32_t pll_numerator;
	uint32_t pll_denominator;
	uint8_t root_predivider;
	uint8_t root_postdivider;
	uint8_t tx_clock_divider;
	uint32_t channel_status_rate_bits;
} AiusSpdifClockPlan;

void aius_rate_tracker_init(AiusRateTracker *tracker);

AiusRateTrackerResult aius_rate_tracker_step(
	AiusRateTracker *tracker,
	const AiusRateTrackerSample *sample);

bool aius_rate_tracker_note_target_applied(
	AiusRateTracker *tracker,
	uint32_t target_rate_millihz,
	uint32_t now_ms);

uint32_t aius_clock_snap_nominal_rate(uint32_t carrier_rate_millihz);

bool aius_spdif_clock_plan(
	uint32_t nominal_rate_hz,
	uint32_t target_rate_millihz,
	AiusSpdifClockPlan *plan);

bool aius_spdif_clock_plan_structurally_equal(
	const AiusSpdifClockPlan *left,
	const AiusSpdifClockPlan *right);

bool aius_spdif_clock_plan_reconstruct_rate(
	const AiusSpdifClockPlan *plan,
	uint32_t *target_rate_millihz);

#ifdef __cplusplus
}
#endif

#endif
