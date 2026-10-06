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
	/* Fill-referenced trim (aius-sample-rate-following Phase 4). The centre
	 * IS applied_target_millihz; the setpoint is input only. */
	uint32_t pending_centre_millihz;   /* 0 = no deferred retarget */
	uint32_t pending_centre_windows;   /* from the episode's first proposal */
	int32_t fill_integral_microhz;     /* clamped to +/-900000 */
	int32_t fill_offset_millihz;       /* clamp(P + I), +/-900 */
	int32_t fill_proportional_millihz; /* last window's P (bumpless retarget) */
	uint32_t fill_last_seq;
	uint32_t fill_spent_seq;
	bool fill_have_seq;
	bool fill_spent;
	bool fill_saturated;
	bool centre_flag;                  /* until a CENTRE write is accepted */
} AiusRateTracker;

/* Fill loop (aius-sample-rate-following Phase 4 §Mechanism). */
#define AIUS_CLOCK_FILL_EXCL_UNDERRUN    UINT8_C(0x01)
#define AIUS_CLOCK_FILL_EXCL_STALL       UINT8_C(0x02)
#define AIUS_CLOCK_FILL_EXCL_MISSED_FILL UINT8_C(0x04)
#define AIUS_CLOCK_FILL_EXCL_NO_SAMPLE   UINT8_C(0x08)
#define AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS 120u
/* Gains and bounds (entry gate items 4 to 6): P = 13 * e_q4 / 16 mHz,
 * I += 345 * e_q4 / 16 uHz per window, both rounded symmetrically. */
#define AIUS_CLOCK_FILL_KP_MILLIHZ_PER_FRAME 13
#define AIUS_CLOCK_FILL_KI_MICROHZ_PER_FRAME 345
#define AIUS_CLOCK_FILL_INTEGRAL_MAX_MICROHZ 900000
#define AIUS_CLOCK_FILL_OFFSET_MAX_MILLIHZ 900
#define AIUS_CLOCK_FILL_EMIT_THRESHOLD_MILLIHZ 20u
/* An invalid window with an underrun and no stall reads this occupancy. */
#define AIUS_CLOCK_FILL_UNDERRUN_LEVEL_FRAMES 128u

typedef enum {
	AIUS_RATE_TRACKER_TRIM_NONE = 0,
	AIUS_RATE_TRACKER_TRIM_CENTRE = 1,
	AIUS_RATE_TRACKER_TRIM_FILL = 2
} AiusRateTrackerTrimKind;

typedef struct {            /* copied from AiusTxFillSnapshot by the parent */
	uint32_t window_seq;    /* closed windows, valid or not; wraps */
	int32_t level_q4;       /* read iff level_valid */
	bool level_valid;
	uint8_t exclusions;     /* AIUS_CLOCK_FILL_EXCL_*, last closed window */
} AiusRateTrackerFillWindow;

typedef struct {
	bool emit_allowed;                /* live, g_clock_plan_applied, gate open */
	bool have_applied;
	uint32_t emission_readback_millihz; /* applied plan readback = emission state;
	                                       never written into the tracker's centre */
	uint32_t setpoint_frames;         /* this lock's; 0 = none yet */
	AiusRateTrackerFillWindow window;
} AiusRateTrackerFillInput;

typedef struct {
	AiusRateTrackerTrimKind emit_kind;  /* NONE: write nothing this pass;
	                                       field below is the centre, never the readback */
	uint32_t composed_target_millihz;   /* clamped to band and centre's cell */
	uint32_t centre_millihz;            /* g_clock_desired_plan target */
	int32_t fill_offset_millihz;        /* composed minus centre */
	int32_t fill_integral_microhz;      /* clamped to +/-900000 */
	bool centre_committed;              /* this pass: trim_updates++, event, desired plan */
	bool retarget_pending;
	uint8_t retarget_pending_windows;   /* 0..120 */
	bool saturated;
} AiusRateTrackerFillResult;

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

/* _step: a proposal goes to the pending centre (target_update is still
 * reported); fill_step commits it. accept_candidate sets
 * applied_target_millihz to the accept target and zeroes I. Losing the carrier
 * or the nominal discards a pending centre. */

/* Idempotent. The parent calls it once, when a dark transition that
 * clock_apply_desired_plan started ends; a FILL readback never notes. */
bool aius_rate_tracker_note_target_applied(
	AiusRateTracker *tracker,
	uint32_t target_rate_millihz,
	uint32_t now_ms);

/* Every pass, after _step. Integrates each newly closed window, commits or
 * defers a pending centre, and returns one composed request. */
AiusRateTrackerFillResult aius_rate_tracker_fill_step(
	AiusRateTracker *tracker,
	const AiusRateTrackerFillInput *in);

/* After a request: true spends the window's write and clears the CENTRE flag;
 * false (a refusal) spends nothing and the next pass retries. */
void aius_rate_tracker_note_emit(AiusRateTracker *tracker, bool accepted);

/* Zero I, clear the CENTRE flag; true if a pending centre committed (into
 * applied_target_millihz). committed_centre_millihz (optional) receives the
 * centre after the reset either way. On true the parent, before
 * clock_apply_desired_plan, counts trim_updates, records
 * AIUS_EVENT_CLOCK_TRIM and rebuilds g_clock_desired_plan from the committed
 * centre; if that plan is invalid it keeps the old desired plan and faults as
 * the centre path does today. */
bool aius_rate_tracker_fill_reset(
	AiusRateTracker *tracker,
	bool discard_pending,
	uint32_t *committed_centre_millihz);

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
