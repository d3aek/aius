#include "aius_clock.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

#define AIUS_CLOCK_NOMINAL_INTERVAL_MS UINT32_C(1000)
#define AIUS_CLOCK_WINDOW_MS UINT32_C(1000)
#define AIUS_CLOCK_TARGET_INTERVAL_MS UINT32_C(10000)
/* The receiver's physical-rate estimate is quantized to roughly 140 mHz at
 * 48 kHz, so a settled carrier still hops between adjacent bins: a deadband
 * under about two bins retrims on quantization noise alone (48 kHz gate 1
 * measured ~53 post-settle trims in 10 minutes at 50 mHz, one per 10.8 s,
 * each costing a 512-frame invalid window). 500 mHz is about 3.5 bins, so a
 * one-bin hop can no longer request a target. The residual offset inside the
 * deadband (plus the estimate's own error) drains or fills the ring without
 * bound; the fill loop below carries it (aius-sample-rate-following Phase 4,
 * fill-referenced rate trim), and the deadband stays inside the 1000 mHz
 * follow-error bound with the estimate's own spread on top. The
 * 2026-09-04 re-run at 500 mHz read 19 post-settle trims (from 53): the
 * settled estimate still spreads ~1.2 Hz over ten minutes, so the deadband
 * cannot grow past the follow-error bound. A wider band cannot help, and
 * averaging the estimate over the cadence interval is the only firmware lever
 * left -- deliberately not taken. Gate 1 is judged on the settled follow
 * error, slope and harmful-counter deltas, which this image passes, not on the
 * trim count the source's ~140 mHz estimate quantum makes unreachable (D4). */
#define AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ UINT32_C(500)
#define AIUS_CLOCK_PLL_REFERENCE_MILLIHZ UINT64_C(24000000000)
#define AIUS_CLOCK_PLL_MIN_MILLIHZ UINT64_C(648000000000)
#define AIUS_CLOCK_PLL_MAX_MILLIHZ UINT64_C(1296000000000)

typedef struct {
	uint32_t nominal_hz;
	uint32_t lower_millihz;
	uint32_t upper_millihz;
	uint8_t multiplier;
	uint8_t tx_divider;
	uint32_t channel_status_rate_bits;
} AiusRateDefinition;

static const AiusRateDefinition rate_definitions[] = {
	{32000u, 31360000u, 32640000u, 21u, 42u,
	 AIUS_CLOCK_STCSCL_RATE_32000},
	{44100u, 43218000u, 44982000u, 15u, 30u,
	 AIUS_CLOCK_STCSCL_RATE_44100},
	{48000u, 47040000u, 48960000u, 14u, 28u,
	 AIUS_CLOCK_STCSCL_RATE_48000},
};

static FLASHMEM const AiusRateDefinition *definition_for_nominal(
	uint32_t nominal_hz)
{
	size_t i;

	for (i = 0u; i < sizeof(rate_definitions) / sizeof(rate_definitions[0]);
	     ++i) {
		if (rate_definitions[i].nominal_hz == nominal_hz)
			return &rate_definitions[i];
	}
	return NULL;
}

static FLASHMEM uint32_t rounded_average(uint64_t sum, uint32_t count)
{
	if (count == 0u)
		return 0u;
	return (uint32_t)((sum + ((uint64_t)count / 2u)) / count);
}

static FLASHMEM uint32_t absolute_difference(uint32_t left, uint32_t right)
{
	return left >= right ? left - right : right - left;
}

static FLASHMEM bool elapsed_at_least(
	uint32_t now_ms,
	uint32_t then_ms,
	uint32_t interval_ms)
{
	return (uint32_t)(now_ms - then_ms) >= interval_ms;
}

static FLASHMEM void reset_candidate(AiusRateTracker *tracker)
{
	tracker->candidate_nominal_hz = 0u;
	tracker->candidate_count = 0u;
	tracker->candidate_sum_millihz = 0u;
	tracker->candidate_start_ms = 0u;
	tracker->candidate_min_millihz = 0u;
	tracker->candidate_max_millihz = 0u;
	tracker->unsupported_candidate = false;
}

static FLASHMEM void reset_window(AiusRateTracker *tracker)
{
	tracker->window_start_ms = 0u;
	tracker->window_count = 0u;
	tracker->window_sum_millihz = 0u;
	tracker->has_window = false;
}

static FLASHMEM void fill_zero_controller(AiusRateTracker *tracker)
{
	tracker->fill_integral_microhz = 0;
	tracker->fill_offset_millihz = 0;
	tracker->fill_proportional_millihz = 0;
	tracker->fill_saturated = false;
}

static FLASHMEM void fill_drop_pending(AiusRateTracker *tracker)
{
	tracker->pending_centre_millihz = 0u;
	tracker->pending_centre_windows = 0u;
}

static FLASHMEM AiusRateTrackerResult tracker_result(
	const AiusRateTracker *tracker)
{
	AiusRateTrackerResult result;

	memset(&result, 0, sizeof(result));
	result.carrier_ready = tracker->carrier_ready;
	result.nominal_rate_hz = tracker->accepted_nominal_hz;
	return result;
}

FLASHMEM void aius_rate_tracker_init(AiusRateTracker *tracker)
{
	if (tracker != NULL)
		memset(tracker, 0, sizeof(*tracker));
}

FLASHMEM uint32_t aius_clock_snap_nominal_rate(
	uint32_t carrier_rate_millihz)
{
	size_t i;

	for (i = 0u; i < sizeof(rate_definitions) / sizeof(rate_definitions[0]);
	     ++i) {
		if (carrier_rate_millihz >= rate_definitions[i].lower_millihz &&
		    carrier_rate_millihz <= rate_definitions[i].upper_millihz)
			return rate_definitions[i].nominal_hz;
	}
	return 0u;
}

static FLASHMEM void observe_candidate(
	AiusRateTracker *tracker,
	uint32_t nominal_hz,
	uint32_t carrier_rate_millihz,
	uint32_t now_ms)
{
	bool unsupported = nominal_hz == 0u;
	uint32_t candidate_min;
	uint32_t candidate_max;

	if (tracker->candidate_count == 0u ||
	    tracker->candidate_nominal_hz != nominal_hz ||
	    tracker->unsupported_candidate != unsupported) {
		reset_candidate(tracker);
		tracker->candidate_nominal_hz = nominal_hz;
		tracker->unsupported_candidate = unsupported;
	}
	if (tracker->candidate_count != 0u && !unsupported) {
		candidate_min =
			carrier_rate_millihz < tracker->candidate_min_millihz
				? carrier_rate_millihz
				: tracker->candidate_min_millihz;
		candidate_max =
			carrier_rate_millihz > tracker->candidate_max_millihz
				? carrier_rate_millihz
				: tracker->candidate_max_millihz;
		/* The receiver DPLL can report the right nominal band while its
		 * physical-rate estimate is still settling by hundreds of hertz.
		 * Applying that transient estimate drains the small TX ring before
		 * the ten-second trim cadence can correct it. Restart the supported
		 * candidate at the newest sample until a complete stable interval is
		 * available. Unsupported detection retains its count-only debounce. */
		if (candidate_max - candidate_min >
		    AIUS_CLOCK_ACCEPT_SPAN_MILLIHZ) {
			reset_candidate(tracker);
			tracker->candidate_nominal_hz = nominal_hz;
		}
	}
	if (tracker->candidate_count == 0u) {
		tracker->candidate_start_ms = now_ms;
		tracker->candidate_min_millihz = carrier_rate_millihz;
		tracker->candidate_max_millihz = carrier_rate_millihz;
	} else {
		if (carrier_rate_millihz < tracker->candidate_min_millihz)
			tracker->candidate_min_millihz = carrier_rate_millihz;
		if (carrier_rate_millihz > tracker->candidate_max_millihz)
			tracker->candidate_max_millihz = carrier_rate_millihz;
	}
	if (tracker->candidate_count != UINT32_MAX)
		tracker->candidate_count++;
	if (UINT64_MAX - tracker->candidate_sum_millihz >=
	    carrier_rate_millihz)
		tracker->candidate_sum_millihz += carrier_rate_millihz;
}

static FLASHMEM bool nominal_accept_is_rate_limited(
	const AiusRateTracker *tracker,
	uint32_t nominal_hz,
	uint32_t now_ms)
{
	return tracker->has_nominal_accept &&
	       nominal_hz != tracker->accepted_nominal_hz &&
	       !elapsed_at_least(now_ms, tracker->last_nominal_accept_ms,
	                         AIUS_CLOCK_NOMINAL_INTERVAL_MS);
}

static FLASHMEM void accept_candidate(
	AiusRateTracker *tracker,
	uint32_t now_ms,
	AiusRateTrackerResult *result)
{
	uint32_t old_nominal = tracker->accepted_nominal_hz;
	uint32_t target = rounded_average(tracker->candidate_sum_millihz,
	                                  tracker->candidate_count);

	tracker->accepted_nominal_hz = tracker->candidate_nominal_hz;
	tracker->carrier_ready = true;
	tracker->has_nominal_accept = true;
	tracker->last_nominal_accept_ms = now_ms;
	tracker->last_target_request_ms = now_ms;
	/* The centre and the hysteresis reference are one field: the accept
	 * target, so the first FILL write after a relock composes on it. */
	tracker->applied_target_millihz = target;
	fill_zero_controller(tracker);
	fill_drop_pending(tracker);
	tracker->centre_flag = false;
	tracker->unsupported_episode_reported = false;
	reset_window(tracker);

	result->carrier_ready = true;
	result->nominal_accepted = true;
	result->nominal_changed = old_nominal != tracker->accepted_nominal_hz;
	result->nominal_rate_hz = tracker->accepted_nominal_hz;
	result->target_rate_millihz = target;
	reset_candidate(tracker);
}

static FLASHMEM void observe_measurement_window(
	AiusRateTracker *tracker,
	uint32_t carrier_rate_millihz,
	uint32_t now_ms,
	AiusRateTrackerResult *result)
{
	uint32_t mean;

	if (!tracker->has_window) {
		tracker->has_window = true;
		tracker->window_start_ms = now_ms;
	}
	if (tracker->window_count != UINT32_MAX)
		tracker->window_count++;
	if (UINT64_MAX - tracker->window_sum_millihz >=
	    carrier_rate_millihz)
		tracker->window_sum_millihz += carrier_rate_millihz;

	if (!elapsed_at_least(now_ms, tracker->window_start_ms,
	                      AIUS_CLOCK_WINDOW_MS))
		return;

	mean = rounded_average(tracker->window_sum_millihz,
	                       tracker->window_count);
	reset_window(tracker);
	if (tracker->applied_target_millihz == 0u)
		return;
	if (absolute_difference(mean, tracker->applied_target_millihz) <
	    AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ) {
		/* Back inside the reference's hysteresis: a deferred centre is
		 * stale and must never commit at the cap or end of saturation. */
		fill_drop_pending(tracker);
		return;
	}
	if (!elapsed_at_least(now_ms, tracker->last_target_request_ms,
	                      AIUS_CLOCK_TARGET_INTERVAL_MS))
		return;

	tracker->last_target_request_ms = now_ms;
	result->target_update = true;
	result->target_rate_millihz = mean;
	/* A re-proposal refreshes the value only; the deferral cap counts from
	 * the episode's first proposal. */
	if (tracker->pending_centre_millihz == 0u)
		tracker->pending_centre_windows = 0u;
	tracker->pending_centre_millihz = mean;
}

FLASHMEM AiusRateTrackerResult aius_rate_tracker_step(
	AiusRateTracker *tracker,
	const AiusRateTrackerSample *sample)
{
	AiusRateTrackerResult result;
	uint32_t nominal_hz;
	bool agrees_with_ready;

	if (tracker == NULL || sample == NULL) {
		memset(&result, 0, sizeof(result));
		return result;
	}
	result = tracker_result(tracker);
	if (sample->source != AIUS_CLOCK_SOURCE_LIVE ||
	    !sample->carrier_locked) {
		result.quiesce_requested = tracker->carrier_ready;
		tracker->carrier_ready = false;
		result.carrier_ready = false;
		reset_candidate(tracker);
		reset_window(tracker);
		fill_drop_pending(tracker);
		tracker->unsupported_episode_reported = false;
		return result;
	}

	nominal_hz = aius_clock_snap_nominal_rate(
		sample->carrier_rate_millihz);
	agrees_with_ready = tracker->carrier_ready && nominal_hz != 0u &&
	                     nominal_hz == tracker->accepted_nominal_hz;
	if (!agrees_with_ready) {
		result.quiesce_requested = tracker->carrier_ready;
		tracker->carrier_ready = false;
		result.carrier_ready = false;
		reset_window(tracker);
		fill_drop_pending(tracker);
		if (nominal_hz != 0u)
			tracker->unsupported_episode_reported = false;
		observe_candidate(tracker, nominal_hz,
		                  sample->carrier_rate_millihz,
		                  sample->now_ms);
		if (tracker->candidate_count < AIUS_CLOCK_ACCEPT_SAMPLES)
			return result;
		if (nominal_hz == 0u) {
			if (!tracker->unsupported_episode_reported) {
				result.unsupported_accepted = true;
				tracker->unsupported_episode_reported = true;
			}
			return result;
		}
		if (!elapsed_at_least(sample->now_ms,
		                      tracker->candidate_start_ms,
		                      AIUS_CLOCK_ACCEPT_STABLE_MS))
			return result;
		if (nominal_accept_is_rate_limited(tracker, nominal_hz,
		                                  sample->now_ms))
			return result;
		accept_candidate(tracker, sample->now_ms, &result);
		return result;
	}

	reset_candidate(tracker);
	tracker->unsupported_episode_reported = false;
	observe_measurement_window(tracker, sample->carrier_rate_millihz,
	                           sample->now_ms, &result);
	return result;
}

FLASHMEM bool aius_rate_tracker_note_target_applied(
	AiusRateTracker *tracker,
	uint32_t target_rate_millihz,
	uint32_t now_ms)
{
	if (tracker == NULL || tracker->accepted_nominal_hz == 0u ||
	    aius_clock_snap_nominal_rate(target_rate_millihz) !=
		    tracker->accepted_nominal_hz)
		return false;
	tracker->applied_target_millihz = target_rate_millihz;
	tracker->last_target_request_ms = now_ms;
	return true;
}

static FLASHMEM bool multiply_u64(
	uint64_t left,
	uint64_t right,
	uint64_t *product)
{
	if (product == NULL || (right != 0u && left > UINT64_MAX / right))
		return false;
	*product = left * right;
	return true;
}

static FLASHMEM bool divide_nearest_u64(
	uint64_t numerator,
	uint64_t denominator,
	uint64_t *quotient)
{
	uint64_t half;

	if (denominator == 0u || quotient == NULL)
		return false;
	half = denominator / 2u;
	if (numerator > UINT64_MAX - half)
		return false;
	*quotient = (numerator + half) / denominator;
	return true;
}

FLASHMEM bool aius_spdif_clock_plan(
	uint32_t nominal_rate_hz,
	uint32_t target_rate_millihz,
	AiusSpdifClockPlan *plan)
{
	const AiusRateDefinition *definition;
	AiusSpdifClockPlan next;
	uint64_t pll_millihz;
	uint64_t remainder;
	uint64_t scaled_remainder;
	uint64_t numerator;

	if (plan == NULL)
		return false;
	definition = definition_for_nominal(nominal_rate_hz);
	if (definition == NULL ||
	    target_rate_millihz < definition->lower_millihz ||
	    target_rate_millihz > definition->upper_millihz ||
	    !multiply_u64(target_rate_millihz,
	                  UINT64_C(1024) * definition->multiplier,
	                  &pll_millihz) ||
	    pll_millihz < AIUS_CLOCK_PLL_MIN_MILLIHZ ||
	    pll_millihz > AIUS_CLOCK_PLL_MAX_MILLIHZ)
		return false;

	memset(&next, 0, sizeof(next));
	next.nominal_rate_hz = nominal_rate_hz;
	next.target_rate_millihz = target_rate_millihz;
	next.pll_div_select =
		(uint32_t)(pll_millihz / AIUS_CLOCK_PLL_REFERENCE_MILLIHZ);
	remainder = pll_millihz % AIUS_CLOCK_PLL_REFERENCE_MILLIHZ;
	if (!multiply_u64(remainder, AIUS_CLOCK_PLL_DENOMINATOR,
	                  &scaled_remainder) ||
	    !divide_nearest_u64(scaled_remainder,
	                        AIUS_CLOCK_PLL_REFERENCE_MILLIHZ,
	                        &numerator))
		return false;
	if (numerator == AIUS_CLOCK_PLL_DENOMINATOR) {
		next.pll_div_select++;
		numerator = 0u;
	}
	if (next.pll_div_select < 27u || next.pll_div_select > 54u ||
	    numerator >= AIUS_CLOCK_PLL_DENOMINATOR)
		return false;
	next.pll_numerator = (uint32_t)numerator;
	next.pll_denominator = AIUS_CLOCK_PLL_DENOMINATOR;
	next.root_predivider = 8u;
	next.root_postdivider = 1u;
	next.tx_clock_divider = definition->tx_divider;
	next.channel_status_rate_bits =
		definition->channel_status_rate_bits;
	*plan = next;
	return true;
}

FLASHMEM bool aius_spdif_clock_plan_structurally_equal(
	const AiusSpdifClockPlan *left,
	const AiusSpdifClockPlan *right)
{
	if (left == NULL || right == NULL)
		return false;
	return left->pll_div_select == right->pll_div_select &&
	       left->pll_denominator == right->pll_denominator &&
	       left->root_predivider == right->root_predivider &&
	       left->root_postdivider == right->root_postdivider &&
	       left->tx_clock_divider == right->tx_clock_divider;
}

FLASHMEM bool aius_spdif_clock_plan_reconstruct_rate(
	const AiusSpdifClockPlan *plan,
	uint32_t *target_rate_millihz)
{
	const AiusRateDefinition *definition;
	uint64_t integer_pll;
	uint64_t fractional_scaled;
	uint64_t fractional_pll;
	uint64_t pll_millihz;
	uint64_t reconstructed;

	if (plan == NULL || target_rate_millihz == NULL ||
	    plan->pll_denominator == 0u ||
	    plan->pll_numerator >= plan->pll_denominator)
		return false;
	definition = definition_for_nominal(plan->nominal_rate_hz);
	if (definition == NULL ||
	    !multiply_u64(plan->pll_div_select,
	                  AIUS_CLOCK_PLL_REFERENCE_MILLIHZ,
	                  &integer_pll) ||
	    !multiply_u64(AIUS_CLOCK_PLL_REFERENCE_MILLIHZ,
	                  plan->pll_numerator, &fractional_scaled) ||
	    !divide_nearest_u64(fractional_scaled, plan->pll_denominator,
	                        &fractional_pll) ||
	    integer_pll > UINT64_MAX - fractional_pll)
		return false;
	pll_millihz = integer_pll + fractional_pll;
	if (!divide_nearest_u64(
		    pll_millihz,
		    UINT64_C(1024) * definition->multiplier,
		    &reconstructed) ||
	    reconstructed > UINT32_MAX)
		return false;
	*target_rate_millihz = (uint32_t)reconstructed;
	return true;
}

/* ---- Fill-referenced rate trim (aius-sample-rate-following Phase 4) ----
 * The tracker owns the controller state (the centre, which IS
 * applied_target_millihz, and I) and the emission decision; the parent feeds
 * the fill window and the applied-plan readback and writes what it is told. */

static FLASHMEM int64_t div_round_symmetric(int64_t numerator,
                                            int64_t denominator)
{
	return numerator >= 0
		? (numerator + denominator / 2) / denominator
		: -((-numerator + denominator / 2) / denominator);
}

static FLASHMEM int64_t clamp_i64(int64_t value, int64_t bound)
{
	if (value > bound)
		return bound;
	if (value < -bound)
		return -bound;
	return value;
}

/* offset = clamp(P + I); saturated when the clamp bites. */
static FLASHMEM void fill_recompute_offset(AiusRateTracker *tracker)
{
	const int64_t raw = (int64_t)tracker->fill_proportional_millihz +
		div_round_symmetric(tracker->fill_integral_microhz, 1000);
	const int64_t clamped =
		clamp_i64(raw, AIUS_CLOCK_FILL_OFFSET_MAX_MILLIHZ);

	tracker->fill_offset_millihz = (int32_t)clamped;
	tracker->fill_saturated = clamped != raw;
}

/* One closed window's PI update. e = statistic - setpoint; positive (ring
 * filling) speeds the output up. I holds while the clamp is active. */
static FLASHMEM void fill_integrate(AiusRateTracker *tracker, int32_t level_q4,
                                    uint32_t setpoint_frames)
{
	const int64_t e_q4 = (int64_t)level_q4 - (int64_t)setpoint_frames * 16;
	const int64_t p = clamp_i64(
		div_round_symmetric(AIUS_CLOCK_FILL_KP_MILLIHZ_PER_FRAME * e_q4, 16),
		INT32_MAX);
	const int64_t raw_held = p + div_round_symmetric(
		tracker->fill_integral_microhz, 1000);

	tracker->fill_proportional_millihz = (int32_t)p;
	if (raw_held <= AIUS_CLOCK_FILL_OFFSET_MAX_MILLIHZ &&
	    raw_held >= -AIUS_CLOCK_FILL_OFFSET_MAX_MILLIHZ) {
		const int64_t next = (int64_t)tracker->fill_integral_microhz +
			div_round_symmetric(
				AIUS_CLOCK_FILL_KI_MICROHZ_PER_FRAME * e_q4, 16);
		tracker->fill_integral_microhz = (int32_t)clamp_i64(
			next, AIUS_CLOCK_FILL_INTEGRAL_MAX_MICROHZ);
	}
	fill_recompute_offset(tracker);
}

/* Bumpless: I -= (centre_new - centre_old), int64 uHz (a full band move is
 * 1.92e9 uHz), then re-clamped. Raises the CENTRE flag. */
static FLASHMEM void fill_commit_centre(AiusRateTracker *tracker,
                                        uint32_t centre_millihz)
{
	const int64_t delta_microhz =
		((int64_t)centre_millihz -
		 (int64_t)tracker->applied_target_millihz) * 1000;

	tracker->fill_integral_microhz = (int32_t)clamp_i64(
		(int64_t)tracker->fill_integral_microhz - delta_microhz,
		AIUS_CLOCK_FILL_INTEGRAL_MAX_MICROHZ);
	tracker->applied_target_millihz = centre_millihz;
	fill_drop_pending(tracker);
	fill_recompute_offset(tracker);
	tracker->centre_flag = true;
}

/* centre + offset, clamped into the plan band and into the centre's
 * structural cell (request_trim refuses a structurally different plan
 * forever). False when the centre itself has no valid plan. */
static FLASHMEM bool fill_compose(const AiusRateTracker *tracker,
                                  uint32_t *composed_millihz)
{
	const AiusRateDefinition *definition =
		definition_for_nominal(tracker->accepted_nominal_hz);
	const uint32_t centre = tracker->applied_target_millihz;
	AiusSpdifClockPlan centre_plan;
	AiusSpdifClockPlan probe;
	int64_t target;
	uint32_t good;
	uint32_t bad;

	if (definition == NULL ||
	    !aius_spdif_clock_plan(tracker->accepted_nominal_hz, centre,
	                           &centre_plan))
		return false;
	target = (int64_t)centre + tracker->fill_offset_millihz;
	if (target < definition->lower_millihz)
		target = definition->lower_millihz;
	if (target > definition->upper_millihz)
		target = definition->upper_millihz;
	if (aius_spdif_clock_plan(tracker->accepted_nominal_hz,
	                          (uint32_t)target, &probe) &&
	    aius_spdif_clock_plan_structurally_equal(&centre_plan, &probe)) {
		*composed_millihz = (uint32_t)target;
		return true;
	}
	/* Across a cell edge: the furthest target toward the request that keeps
	 * the centre's structure (a cell is contiguous). */
	good = centre;
	bad = (uint32_t)target;
	while (absolute_difference(good, bad) > 1u) {
		const uint32_t mid = good < bad
			? good + (bad - good) / 2u
			: bad + (good - bad) / 2u;
		if (aius_spdif_clock_plan(tracker->accepted_nominal_hz, mid,
		                          &probe) &&
		    aius_spdif_clock_plan_structurally_equal(&centre_plan, &probe))
			good = mid;
		else
			bad = mid;
	}
	*composed_millihz = good;
	return true;
}

FLASHMEM AiusRateTrackerFillResult aius_rate_tracker_fill_step(
	AiusRateTracker *tracker,
	const AiusRateTrackerFillInput *in)
{
	AiusRateTrackerFillResult result;
	bool new_window = false;
	uint32_t composed = 0u;
	bool composed_ok;

	memset(&result, 0, sizeof(result));
	if (tracker == NULL || in == NULL)
		return result;

	if (!tracker->fill_have_seq) {
		tracker->fill_have_seq = true;
		tracker->fill_last_seq = in->window.window_seq;
	} else if (in->window.window_seq != tracker->fill_last_seq) {
		new_window = true;
		tracker->fill_last_seq = in->window.window_seq;
	}

	if (new_window && tracker->pending_centre_millihz != 0u &&
	    tracker->pending_centre_windows <
		    AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS)
		tracker->pending_centre_windows++;

	if (new_window && in->emit_allowed && in->setpoint_frames != 0u &&
	    tracker->applied_target_millihz != 0u) {
		const uint8_t excl = in->window.exclusions;

		if (in->window.level_valid) {
			fill_integrate(tracker, in->window.level_q4,
			               in->setpoint_frames);
		} else if ((excl & AIUS_CLOCK_FILL_EXCL_UNDERRUN) != 0u &&
		           (excl & AIUS_CLOCK_FILL_EXCL_STALL) == 0u) {
			/* The true occupancy at an underrun (level 0 at phi 1). */
			fill_integrate(tracker,
			               (int32_t)(AIUS_CLOCK_FILL_UNDERRUN_LEVEL_FRAMES *
			                         16u),
			               in->setpoint_frames);
		} else {
			/* Stall or no underrun: hold I, emit nothing this window. */
			tracker->fill_spent = true;
			tracker->fill_spent_seq = tracker->fill_last_seq;
		}
	}

	if (tracker->pending_centre_millihz != 0u &&
	    tracker->applied_target_millihz != 0u) {
		const int64_t delta = (int64_t)tracker->pending_centre_millihz -
		                      (int64_t)tracker->applied_target_millihz;
		const bool same_sign =
			(delta > 0 && tracker->fill_offset_millihz > 0) ||
			(delta < 0 && tracker->fill_offset_millihz < 0) ||
			delta == 0;

		/* Saturated, an opposite-sign retarget defers (that saturation
		 * resolves itself) up to the cap; anything else commits now. */
		if (!tracker->fill_saturated || same_sign ||
		    tracker->pending_centre_windows >=
			    AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS) {
			fill_commit_centre(tracker, tracker->pending_centre_millihz);
			result.centre_committed = true;
		}
	}

	composed_ok = tracker->applied_target_millihz != 0u &&
	              fill_compose(tracker, &composed);
	if (!composed_ok)
		composed = tracker->applied_target_millihz;

	if (in->emit_allowed && in->have_applied && composed_ok) {
		if (absolute_difference(composed, in->emission_readback_millihz) >
		    AIUS_CLOCK_FILL_EMIT_THRESHOLD_MILLIHZ) {
			const bool spent = tracker->fill_spent &&
				tracker->fill_spent_seq == tracker->fill_last_seq;

			/* CENTRE persists until accepted and bypasses the one
			 * FILL write per window. */
			if (tracker->centre_flag)
				result.emit_kind = AIUS_RATE_TRACKER_TRIM_CENTRE;
			else if (!spent)
				result.emit_kind = AIUS_RATE_TRACKER_TRIM_FILL;
		} else {
			/* A pass that needs no write clears the flag, so a noise
			 * write later is never labelled CENTRE. */
			tracker->centre_flag = false;
		}
	}

	result.composed_target_millihz = composed;
	result.centre_millihz = tracker->applied_target_millihz;
	result.fill_offset_millihz =
		(int32_t)((int64_t)composed -
		          (int64_t)tracker->applied_target_millihz);
	result.fill_integral_microhz = tracker->fill_integral_microhz;
	result.retarget_pending = tracker->pending_centre_millihz != 0u;
	result.retarget_pending_windows =
		(uint8_t)tracker->pending_centre_windows;
	result.saturated = tracker->fill_saturated;
	return result;
}

FLASHMEM void aius_rate_tracker_note_emit(AiusRateTracker *tracker,
                                          bool accepted)
{
	if (tracker == NULL || !accepted)
		return;
	tracker->fill_spent = true;
	tracker->fill_spent_seq = tracker->fill_last_seq;
	/* A FILL is only returned with the flag down, so this clears it
	 * exactly when a CENTRE write was accepted. */
	tracker->centre_flag = false;
}

FLASHMEM bool aius_rate_tracker_fill_reset(
	AiusRateTracker *tracker,
	bool discard_pending,
	uint32_t *committed_centre_millihz)
{
	bool committed = false;

	if (tracker == NULL)
		return false;
	fill_zero_controller(tracker);
	tracker->centre_flag = false;
	if (!discard_pending && tracker->pending_centre_millihz != 0u) {
		tracker->applied_target_millihz = tracker->pending_centre_millihz;
		committed = true;
	}
	fill_drop_pending(tracker);
	if (committed_centre_millihz != NULL)
		*committed_centre_millihz = tracker->applied_target_millihz;
	return committed;
}
