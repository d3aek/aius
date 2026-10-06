#include "aius_clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read-only: the Phase 3 sensor and its shipped opening rules (called, never
 * modelled), and the governor's decision rule. */
#include "aius_tx_fill_sensor.h"
#include "aius_tx_governor.h"

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define CHECK(condition) check((condition), #condition, __LINE__)

static const char *current_key = "startup";

static void check(int condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "clock oracle mismatch [%s] line %d: %s\n",
	        current_key, line, expression);
	exit(1);
}

static AiusRateTrackerResult step(
	AiusRateTracker *tracker,
	bool locked,
	uint32_t rate_millihz,
	AiusClockSourceState source,
	uint32_t now_ms)
{
	AiusRateTrackerSample sample = {
		.carrier_locked = locked,
		.carrier_rate_millihz = rate_millihz,
		.source = source,
		.now_ms = now_ms,
	};
	return aius_rate_tracker_step(tracker, &sample);
}

static AiusRateTrackerResult accept(
	AiusRateTracker *tracker,
	uint32_t rate_millihz,
	uint32_t start_ms)
{
	AiusRateTrackerResult result;
	uint32_t i;

	for (i = 0u; i < AIUS_CLOCK_ACCEPT_SAMPLES; ++i)
		result = step(tracker, true, rate_millihz,
		              AIUS_CLOCK_SOURCE_LIVE, start_ms + i * 150u);
	return result;
}

static void test_snap_boundaries(void)
{
	static const struct {
		uint32_t nominal;
		uint32_t lower;
		uint32_t upper;
	} cases[] = {
		{32000u, 31360000u, 32640000u},
		{44100u, 43218000u, 44982000u},
		{48000u, 47040000u, 48960000u},
	};
	size_t i;

	current_key = "tracker.snap-boundaries";
	for (i = 0u; i < ARRAY_COUNT(cases); ++i) {
		CHECK(aius_clock_snap_nominal_rate(cases[i].lower) ==
		      cases[i].nominal);
		CHECK(aius_clock_snap_nominal_rate(cases[i].upper) ==
		      cases[i].nominal);
		CHECK(aius_clock_snap_nominal_rate(cases[i].lower - 1u) == 0u);
		CHECK(aius_clock_snap_nominal_rate(cases[i].upper + 1u) == 0u);
	}
}

static void test_tracker_accepts_all_rates(void)
{
	static const uint32_t rates[] = {32000000u, 44100000u, 48000000u};
	size_t i;

	for (i = 0u; i < ARRAY_COUNT(rates); ++i) {
		AiusRateTracker tracker;
		AiusRateTrackerResult result;
		uint32_t sample;

		current_key = "tracker.accept-rates";
		aius_rate_tracker_init(&tracker);
		for (sample = 0u; sample < AIUS_CLOCK_ACCEPT_SAMPLES - 1u;
		     ++sample) {
			result = step(&tracker, true, rates[i],
			              AIUS_CLOCK_SOURCE_LIVE, sample * 150u);
			CHECK(!result.carrier_ready);
			CHECK(!result.nominal_accepted);
		}
		result = step(&tracker, true, rates[i],
		              AIUS_CLOCK_SOURCE_LIVE, 1050u);
		CHECK(result.carrier_ready);
		CHECK(result.nominal_accepted);
		CHECK(result.nominal_changed);
		CHECK(result.nominal_rate_hz == rates[i] / 1000u);
		CHECK(result.target_rate_millihz == rates[i]);
	}
}

static void test_tracker_requires_consecutive_locked_samples(void)
{
	AiusRateTracker tracker;
	AiusRateTrackerResult result;
	uint32_t i;

	current_key = "tracker.lock-flap";
	aius_rate_tracker_init(&tracker);
	for (i = 0u; i < AIUS_CLOCK_ACCEPT_SAMPLES - 1u; ++i)
		result = step(&tracker, true, 48000000u,
		              AIUS_CLOCK_SOURCE_LIVE, i * 150u);
	CHECK(!result.carrier_ready);
	result = step(&tracker, false, 48000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 1000u);
	CHECK(!result.carrier_ready);
	for (i = 0u; i < AIUS_CLOCK_ACCEPT_SAMPLES - 1u; ++i)
		result = step(&tracker, true, 48000000u,
		              AIUS_CLOCK_SOURCE_LIVE, 1100u + i * 150u);
	CHECK(!result.carrier_ready);
	result = step(&tracker, true, 48000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 2150u);
	CHECK(result.carrier_ready);
	CHECK(result.nominal_accepted);
}

static void test_tracker_rejects_settling_physical_rate(void)
{
	AiusRateTracker tracker;
	AiusRateTrackerResult result;
	uint32_t i;

	current_key = "tracker.settling-rate";
	aius_rate_tracker_init(&tracker);
	/* Reproduce the CM6206 transition: the receiver is locked and every sample
	 * already snaps to 44.1 kHz, but the DPLL estimate walks down by hundreds
	 * of hertz. The old eight-sample debounce applied that transient target and
	 * drained 640 TX frames before the ten-second trim. */
	for (i = 0u; i < 80u; ++i) {
		result = step(&tracker, true, 44800000u - i * 8750u,
		              AIUS_CLOCK_SOURCE_LIVE, i * 100u);
		CHECK(!result.carrier_ready);
		CHECK(!result.nominal_accepted);
	}
	for (i = 0u; i <= 10u; ++i) {
		result = step(&tracker, true, 44105350u,
		              AIUS_CLOCK_SOURCE_LIVE, 8000u + i * 100u);
		if (i < 10u) {
			CHECK(!result.carrier_ready);
			CHECK(!result.nominal_accepted);
		}
	}
	CHECK(result.carrier_ready);
	CHECK(result.nominal_accepted);
	CHECK(result.target_rate_millihz == 44105350u);
}

static void test_tracker_quiesces_on_first_changed_sample(void)
{
	AiusRateTracker tracker;
	AiusRateTrackerResult result;
	uint32_t i;

	current_key = "tracker.first-sample-quiesce";
	aius_rate_tracker_init(&tracker);
	result = accept(&tracker, 48000000u, 0u);
	CHECK(result.carrier_ready);

	result = step(&tracker, true, 44100000u,
	              AIUS_CLOCK_SOURCE_LIVE, 2000u);
	CHECK(!result.carrier_ready);
	CHECK(result.quiesce_requested);
	CHECK(!result.nominal_accepted);
	for (i = 1u; i < AIUS_CLOCK_ACCEPT_SAMPLES - 1u; ++i) {
		result = step(&tracker, true, 44100000u,
		              AIUS_CLOCK_SOURCE_LIVE, 2000u + i * 150u);
		CHECK(!result.carrier_ready);
	}
	result = step(&tracker, true, 44100000u,
	              AIUS_CLOCK_SOURCE_LIVE, 3050u);
	CHECK(result.carrier_ready);
	CHECK(result.nominal_accepted);
	CHECK(result.nominal_changed);

	result = step(&tracker, true, 60000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 4000u);
	CHECK(!result.carrier_ready);
	CHECK(result.quiesce_requested);
	CHECK(!result.unsupported_accepted);
	for (i = 1u; i < AIUS_CLOCK_ACCEPT_SAMPLES; ++i)
		result = step(&tracker, true, 60000000u,
		              AIUS_CLOCK_SOURCE_LIVE, 4000u + i);
	CHECK(result.unsupported_accepted);
	CHECK(!result.carrier_ready);
	result = step(&tracker, true, 60000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 4008u);
	CHECK(!result.unsupported_accepted);
}

static void test_tracker_lock_source_and_rate_limit(void)
{
	AiusRateTracker tracker;
	AiusRateTrackerResult result;
	uint32_t i;

	current_key = "tracker.lock-source-rate-limit";
	aius_rate_tracker_init(&tracker);
	result = accept(&tracker, 48000000u, UINT32_MAX - 500u);
	CHECK(result.carrier_ready);
	result = step(&tracker, false, 48000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 600u);
	CHECK(result.quiesce_requested);
	CHECK(!result.carrier_ready);
	result = step(&tracker, true, 48000000u,
	              AIUS_CLOCK_SOURCE_REPLAY, 601u);
	CHECK(!result.carrier_ready);

	for (i = 0u; i < AIUS_CLOCK_ACCEPT_SAMPLES - 1u; ++i)
		result = step(&tracker, true, 44100000u,
		              AIUS_CLOCK_SOURCE_LIVE, 700u + i * 150u);
	CHECK(!result.carrier_ready);
	CHECK(!result.nominal_accepted);
	result = step(&tracker, true, 44100000u,
	              AIUS_CLOCK_SOURCE_LIVE, 1750u);
	CHECK(result.carrier_ready);
	CHECK(result.nominal_accepted);
	CHECK(result.nominal_rate_hz == 44100u);
}

static void run_window(
	AiusRateTracker *tracker,
	uint32_t rate_a,
	uint32_t rate_b,
	uint32_t start_ms,
	AiusRateTrackerResult *result)
{
	uint32_t i;

	for (i = 0u; i <= 10u; ++i)
		*result = step(tracker, true, (i & 1u) ? rate_b : rate_a,
		               AIUS_CLOCK_SOURCE_LIVE, start_ms + i * 100u);
}

static void test_tracker_average_hysteresis_and_cadence(void)
{
	AiusRateTracker tracker;
	AiusRateTrackerResult result;
	uint32_t second;

	current_key = "tracker.average-hysteresis-cadence";
	aius_rate_tracker_init(&tracker);
	result = accept(&tracker, 48000000u, 0u);
	CHECK(aius_rate_tracker_note_target_applied(
		&tracker, 48000000u, 1050u));
	CHECK(!aius_rate_tracker_note_target_applied(
		&tracker, 44100000u, 1050u));

	for (second = 2u; second < 11u; ++second) {
		run_window(&tracker, 48000100u, 47999900u,
		           second * 1000u, &result);
		CHECK(!result.target_update);
	}
	/* Deadband edge, pinned to the 500 mHz hysteresis: below 500 mHz of
	 * departure from the applied target the tracker holds, at 500 mHz it
	 * requests. Raised from 50 mHz because the rate estimate quantizes to
	 * ~140 mHz bins and the old edge trimmed on one-bin hops. */
	run_window(&tracker, 48000499u, 48000499u, 11000u, &result);
	CHECK(!result.target_update);
	run_window(&tracker, 48000500u, 48000500u, 12001u, &result);
	CHECK(result.target_update);
	CHECK(result.target_rate_millihz == 48000500u);
	CHECK(aius_rate_tracker_note_target_applied(
		&tracker, result.target_rate_millihz, 13001u));
	run_window(&tracker, 48001100u, 48001100u, 13002u, &result);
	CHECK(!result.target_update);
	run_window(&tracker, 48001100u, 48001100u, 23002u, &result);
	CHECK(result.target_update);
	/* A one-bin (~140 mHz) hop of the window mean is quantization noise and
	 * must not request a trim even when the 10 s cadence is eligible; a
	 * 600 mHz step is real drift and must. */
	CHECK(aius_rate_tracker_note_target_applied(
		&tracker, 48001100u, 24003u));
	run_window(&tracker, 48001240u, 48001240u, 34004u, &result);
	CHECK(!result.target_update);
	run_window(&tracker, 48001700u, 48001700u, 35005u, &result);
	CHECK(result.target_update);
	CHECK(result.target_rate_millihz == 48001700u);
}

static void check_plan(
	uint32_t nominal,
	uint32_t target,
	uint8_t multiplier,
	uint8_t tx_divider,
	uint32_t channel_bits)
{
	AiusSpdifClockPlan plan;
	uint32_t reconstructed;
	uint64_t pll_millihz;
	uint32_t error;

	CHECK(aius_spdif_clock_plan(nominal, target, &plan));
	CHECK(plan.nominal_rate_hz == nominal);
	CHECK(plan.target_rate_millihz == target);
	CHECK(plan.pll_denominator == AIUS_CLOCK_PLL_DENOMINATOR);
	CHECK(plan.pll_numerator < plan.pll_denominator);
	CHECK(plan.root_predivider == 8u);
	CHECK(plan.root_postdivider == 1u);
	CHECK(plan.tx_clock_divider == tx_divider);
	CHECK(plan.channel_status_rate_bits == channel_bits);
	CHECK(aius_spdif_clock_plan_reconstruct_rate(&plan, &reconstructed));
	error = reconstructed >= target ? reconstructed - target :
	                                  target - reconstructed;
	CHECK(error <= 1u);
	pll_millihz = (uint64_t)reconstructed * 1024u * multiplier;
	CHECK(pll_millihz >= UINT64_C(648000000000));
	CHECK(pll_millihz <= UINT64_C(1296000000000));
}

static void test_plan_rates_and_edges(void)
{
	current_key = "plan.rates-and-edges";
	check_plan(32000u, 31360000u, 21u, 42u,
	           AIUS_CLOCK_STCSCL_RATE_32000);
	check_plan(32000u, 32000000u, 21u, 42u,
	           AIUS_CLOCK_STCSCL_RATE_32000);
	check_plan(32000u, 32640000u, 21u, 42u,
	           AIUS_CLOCK_STCSCL_RATE_32000);
	check_plan(44100u, 43218000u, 15u, 30u,
	           AIUS_CLOCK_STCSCL_RATE_44100);
	check_plan(44100u, 44100000u, 15u, 30u,
	           AIUS_CLOCK_STCSCL_RATE_44100);
	check_plan(44100u, 44982000u, 15u, 30u,
	           AIUS_CLOCK_STCSCL_RATE_44100);
	check_plan(48000u, 47040000u, 14u, 28u,
	           AIUS_CLOCK_STCSCL_RATE_48000);
	check_plan(48000u, 48000000u, 14u, 28u,
	           AIUS_CLOCK_STCSCL_RATE_48000);
	check_plan(48000u, 48960000u, 14u, 28u,
	           AIUS_CLOCK_STCSCL_RATE_48000);
}

static void check_crossing(
	uint32_t nominal,
	uint32_t below,
	uint32_t above)
{
	AiusSpdifClockPlan left;
	AiusSpdifClockPlan right;

	CHECK(aius_spdif_clock_plan(nominal, below, &left));
	CHECK(aius_spdif_clock_plan(nominal, above, &right));
	CHECK(!aius_spdif_clock_plan_structurally_equal(&left, &right));
	CHECK(left.pll_div_select + 1u == right.pll_div_select);
	check_plan(nominal, below,
	           nominal == 32000u ? 21u : nominal == 44100u ? 15u : 14u,
	           nominal == 32000u ? 42u : nominal == 44100u ? 30u : 28u,
	           nominal == 32000u ? AIUS_CLOCK_STCSCL_RATE_32000 :
	           nominal == 44100u ? AIUS_CLOCK_STCSCL_RATE_44100 :
	                               AIUS_CLOCK_STCSCL_RATE_48000);
	check_plan(nominal, above,
	           nominal == 32000u ? 21u : nominal == 44100u ? 15u : 14u,
	           nominal == 32000u ? 42u : nominal == 44100u ? 30u : 28u,
	           nominal == 32000u ? AIUS_CLOCK_STCSCL_RATE_32000 :
	           nominal == 44100u ? AIUS_CLOCK_STCSCL_RATE_44100 :
	                               AIUS_CLOCK_STCSCL_RATE_48000);
}

static void test_plan_integer_crossings_and_equality(void)
{
	AiusSpdifClockPlan first;
	AiusSpdifClockPlan second;

	current_key = "plan.integer-crossings";
	check_crossing(44100u, 43749999u, 43750000u);
	check_crossing(48000u, 48549106u, 48549107u);
	check_crossing(32000u, 32366070u, 32366071u);

	CHECK(aius_spdif_clock_plan(48000u, 48000000u, &first));
	CHECK(aius_spdif_clock_plan(48000u, 48000100u, &second));
	CHECK(aius_spdif_clock_plan_structurally_equal(&first, &second));
	second.pll_denominator = 10000u;
	CHECK(!aius_spdif_clock_plan_structurally_equal(&first, &second));
	CHECK(!aius_spdif_clock_plan_structurally_equal(NULL, &second));
}

static void test_plan_rejects_invalid_input(void)
{
	AiusSpdifClockPlan plan;
	uint32_t reconstructed;

	current_key = "plan.invalid";
	memset(&plan, 0x5a, sizeof(plan));
	CHECK(!aius_spdif_clock_plan(0u, 48000000u, &plan));
	CHECK(!aius_spdif_clock_plan(48000u, 47039999u, &plan));
	CHECK(!aius_spdif_clock_plan(48000u, 48960001u, &plan));
	CHECK(!aius_spdif_clock_plan(48000u, 48000000u, NULL));
	CHECK(!aius_spdif_clock_plan_reconstruct_rate(NULL, &reconstructed));
	CHECK(!aius_spdif_clock_plan_reconstruct_rate(&plan, NULL));
}

/* ======================================================================
 * Fill-referenced rate trim (aius-sample-rate-following Phase 4).
 * Part 1: the tracker API driven directly.
 * ====================================================================== */

#define FILL_SP 320u
#define Q4(frames) ((int32_t)((frames) * 16))

static uint32_t g_fill_seq;
static char g_key[160];

static uint32_t absolute_difference(uint32_t left, uint32_t right)
{
	return left >= right ? left - right : right - left;
}

static AiusRateTrackerFillResult fill_call(
	AiusRateTracker *tracker,
	int32_t level_q4,
	bool valid,
	uint8_t exclusions,
	uint32_t readback)
{
	AiusRateTrackerFillInput in;

	memset(&in, 0, sizeof(in));
	in.emit_allowed = true;
	in.have_applied = true;
	in.emission_readback_millihz = readback;
	in.setpoint_frames = FILL_SP;
	in.window.window_seq = g_fill_seq;
	in.window.level_q4 = level_q4;
	in.window.level_valid = valid;
	in.window.exclusions = exclusions;
	return aius_rate_tracker_fill_step(tracker, &in);
}

/* A newly closed valid window with error e_q4 against the setpoint. */
static AiusRateTrackerFillResult fill_window(
	AiusRateTracker *tracker,
	int32_t e_q4,
	uint32_t readback)
{
	g_fill_seq++;
	return fill_call(tracker, Q4(FILL_SP) + e_q4, true, 0u, readback);
}

/* Another pass inside the same window. */
static AiusRateTrackerFillResult fill_again(
	AiusRateTracker *tracker,
	uint32_t readback)
{
	return fill_call(tracker, 0, false, 0u, readback);
}

static void fill_accept(AiusRateTracker *tracker, uint32_t rate)
{
	aius_rate_tracker_init(tracker);
	(void)accept(tracker, rate, 0u);
	CHECK(tracker->applied_target_millihz == rate);
	CHECK(tracker->fill_integral_microhz == 0);
	g_fill_seq = 0u;
	(void)fill_again(tracker, rate); /* sequence baseline */
}

static bool fill_propose(AiusRateTracker *tracker, uint32_t rate,
                         uint32_t start_ms)
{
	AiusRateTrackerResult result;

	run_window(tracker, rate, rate, start_ms, &result);
	return result.target_update;
}

static void test_fill_fixed_point(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;

	current_key = "fill.fixed-point";
	fill_accept(&t, 48000000u);
	(void)fill_window(&t, -8, 48000000u);
	CHECK(t.fill_proportional_millihz == -7);  /* -6.5 away from zero */
	CHECK(t.fill_integral_microhz == -173);    /* -172.5 */
	(void)fill_window(&t, 8, 48000000u);
	CHECK(t.fill_proportional_millihz == 7);
	CHECK(t.fill_integral_microhz == 0);
	(void)fill_window(&t, -1, 48000000u);
	CHECK(t.fill_proportional_millihz == -1);  /* -0.8125 */
	CHECK(t.fill_integral_microhz == -22);     /* -21.5625 */
	(void)fill_window(&t, 1, 48000000u);
	CHECK(t.fill_integral_microhz == 0);
	/* Extremes saturate with I held and no overflow (UBSan). */
	g_fill_seq++;
	r = fill_call(&t, INT32_MAX, true, 0u, 48000000u);
	CHECK(r.saturated && r.fill_offset_millihz == 900);
	CHECK(r.fill_integral_microhz == 0);
	g_fill_seq++;
	r = fill_call(&t, INT32_MIN, true, 0u, 48000000u);
	CHECK(r.saturated && r.fill_offset_millihz == -900);
	CHECK(r.fill_integral_microhz == 0);
	/* A full-band retarget (1.92e9 uHz) in int64, then re-clamped. */
	fill_accept(&t, 47040000u);
	CHECK(fill_propose(&t, 48960000u, 11000u));
	r = fill_again(&t, 47040000u);
	CHECK(r.centre_committed && r.centre_millihz == 48960000u);
	CHECK(r.fill_integral_microhz == -900000);
	CHECK(r.composed_target_millihz == 48959100u);
	CHECK(fill_propose(&t, 47040000u, 22000u));
	r = fill_again(&t, 48959100u);
	CHECK(r.centre_committed && r.fill_integral_microhz == 900000);
	CHECK(r.composed_target_millihz == 47040900u);
}

static void test_fill_invalid_windows(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	int32_t held_i;
	int32_t held_offset;

	current_key = "fill.invalid-windows";
	/* Underrun and no stall reads 128: e = -192, P = -2496, -900 clamp. */
	fill_accept(&t, 48000000u);
	g_fill_seq++;
	r = fill_call(&t, 0, false, AIUS_CLOCK_FILL_EXCL_UNDERRUN, 48000000u);
	CHECK(t.fill_proportional_millihz == -2496);
	CHECK(r.saturated && r.fill_offset_millihz == -900);
	CHECK(r.fill_integral_microhz == 0);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	fill_accept(&t, 48000000u);
	g_fill_seq++;
	r = fill_call(&t, 0, false,
	              AIUS_CLOCK_FILL_EXCL_UNDERRUN |
	                      AIUS_CLOCK_FILL_EXCL_MISSED_FILL,
	              48000000u);
	CHECK(r.fill_offset_millihz == -900);
	/* A stall, or no underrun: I held, nothing emitted all window. */
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(10), 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	aius_rate_tracker_note_emit(&t, true);
	held_i = t.fill_integral_microhz;
	held_offset = t.fill_offset_millihz;
	g_fill_seq++;
	r = fill_call(&t, 0, false,
	              AIUS_CLOCK_FILL_EXCL_UNDERRUN | AIUS_CLOCK_FILL_EXCL_STALL,
	              48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	CHECK(r.fill_integral_microhz == held_i);
	CHECK(r.fill_offset_millihz == held_offset);
	r = fill_again(&t, 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	g_fill_seq++;
	r = fill_call(&t, 0, false, AIUS_CLOCK_FILL_EXCL_MISSED_FILL, 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	CHECK(r.fill_integral_microhz == held_i);
	/* q4 0 with level_valid 0 and no bits is never read as a level. */
	g_fill_seq++;
	r = fill_call(&t, 0, false, 0u, 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	CHECK(r.fill_integral_microhz == held_i);
	CHECK(r.fill_offset_millihz == held_offset);
	/* The next valid window may write again. */
	r = fill_window(&t, Q4(10), 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
}

static void test_fill_readback_never_centre(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	uint32_t request_ms;
	uint32_t i;

	current_key = "fill.readback-never-centre";
	fill_accept(&t, 48000000u);
	CHECK(aius_rate_tracker_note_target_applied(&t, 48000000u, 1100u));
	request_ms = t.last_target_request_ms;
	r = fill_window(&t, Q4(20), 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	CHECK(r.composed_target_millihz == 48000267u);
	aius_rate_tracker_note_emit(&t, true);
	for (i = 0u; i < 50u; ++i) {
		r = fill_again(&t, 48000267u);
		CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
		CHECK(r.centre_millihz == 48000000u);
		CHECK(t.applied_target_millihz == 48000000u);
		CHECK(t.last_target_request_ms == request_ms);
	}
}

static void test_fill_centre_kind(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	uint32_t rb;

	current_key = "fill.centre-kind-refused-then-spent";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(4), 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	aius_rate_tracker_note_emit(&t, true); /* the window is spent */
	rb = r.composed_target_millihz;
	CHECK(rb == 48000053u);
	CHECK(fill_propose(&t, 48001000u, 11000u));
	r = fill_again(&t, rb);
	/* I = 1380 - 1e6 re-clamps to -900000: the commit needs a write. */
	CHECK(r.centre_committed);
	CHECK(r.composed_target_millihz == 48000152u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_CENTRE);
	aius_rate_tracker_note_emit(&t, false); /* refused */
	r = fill_again(&t, rb);
	CHECK(!r.centre_committed);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_CENTRE); /* spent window */
	aius_rate_tracker_note_emit(&t, true);
	r = fill_again(&t, rb); /* the readback lags a true return */
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	r = fill_window(&t, Q4(30), 48000152u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);

	current_key = "fill.centre-flag-leak";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(4), 48000000u);
	aius_rate_tracker_note_emit(&t, true);
	CHECK(fill_propose(&t, 48001000u, 11000u));
	r = fill_again(&t, 48000053u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_CENTRE);
	aius_rate_tracker_note_emit(&t, false);
	r = fill_again(&t, r.composed_target_millihz); /* no write needed */
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	CHECK(!t.centre_flag);
	r = fill_window(&t, Q4(30), 48000152u); /* a later noise write */
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	aius_rate_tracker_note_emit(&t, true);
	/* A full apply while the flag is up clears it. */
	CHECK(fill_propose(&t, 48002000u, 22000u));
	r = fill_again(&t, 48000000u);
	CHECK(r.centre_committed);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_CENTRE);
	aius_rate_tracker_note_emit(&t, false);
	CHECK(!aius_rate_tracker_fill_reset(&t, false, NULL));
	CHECK(!t.centre_flag && t.fill_integral_microhz == 0);
	r = fill_window(&t, Q4(30), 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
}

static void test_fill_stale_pending(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;

	current_key = "fill.stale-pending-drop";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(200), 48000000u);
	CHECK(r.saturated && r.fill_offset_millihz == 900);
	CHECK(fill_propose(&t, 47999400u, 11000u));
	r = fill_again(&t, 48000900u);
	CHECK(!r.centre_committed && r.retarget_pending);
	r = fill_window(&t, Q4(200), 48000900u);
	CHECK(r.retarget_pending && r.retarget_pending_windows == 1u);
	CHECK(!fill_propose(&t, 48000100u, 16000u)); /* back inside 500 */
	r = fill_again(&t, 48000900u);
	CHECK(!r.retarget_pending);
	r = fill_window(&t, 0, 48000900u); /* saturation ends */
	CHECK(!r.saturated && !r.centre_committed);
	CHECK(r.centre_millihz == 48000000u);
}

static void test_fill_saturated_retarget(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	uint32_t old;

	/* Same sign: commits at once, toward the loop's demand, never past. */
	current_key = "fill.saturated-retarget-same-sign";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(200), 48000000u); /* raw 2600 */
	old = r.composed_target_millihz;
	CHECK(old == 48000900u);
	CHECK(fill_propose(&t, 48000600u, 11000u));
	r = fill_again(&t, old);
	CHECK(r.centre_committed);
	CHECK(r.composed_target_millihz == 48001500u);
	CHECK(r.composed_target_millihz <= 48002600u);
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(-200), 48000000u);
	old = r.composed_target_millihz;
	CHECK(old == 47999100u);
	CHECK(fill_propose(&t, 47999400u, 11000u));
	r = fill_again(&t, old);
	CHECK(r.centre_committed);
	CHECK(r.composed_target_millihz == 47998500u);
	CHECK(r.composed_target_millihz >= 47997400u);

	/* Opposite sign: deferred, composed unchanged; commits bumpless when
	 * the saturation resolves. */
	current_key = "fill.saturated-retarget-opposite-sign";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(200), 48000000u);
	CHECK(fill_propose(&t, 47999400u, 11000u));
	r = fill_again(&t, 48000900u);
	CHECK(!r.centre_committed && r.retarget_pending);
	CHECK(r.composed_target_millihz == 48000900u);
	r = fill_window(&t, 0, 48000900u);
	CHECK(r.centre_committed && !r.saturated);
	CHECK(r.centre_millihz == 47999400u);
	CHECK(r.composed_target_millihz == 48000000u); /* centre_old + raw 0 */
}

static void test_fill_defer_cap(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	uint32_t now = 11000u;
	uint32_t latest = 47999400u;
	uint32_t refreshes = 0u;
	uint32_t commit_w = 0u;
	uint32_t w;

	current_key = "fill.defer-cap-reproposals";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(200), 48000000u);
	CHECK(fill_propose(&t, latest, now));
	r = fill_again(&t, 48000900u);
	CHECK(r.retarget_pending && r.retarget_pending_windows == 0u);
	for (w = 1u; w <= 130u && commit_w == 0u; ++w) {
		const uint32_t rate = 47999400u - (w % 7u) * 10u;

		now += 4000u;
		if (fill_propose(&t, rate, now)) {
			latest = rate;
			refreshes++;
		}
		r = fill_window(&t, Q4(200), 48000900u);
		if (r.centre_committed)
			commit_w = w;
		else
			CHECK(r.retarget_pending &&
			      r.retarget_pending_windows == w);
	}
	CHECK(commit_w == AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS);
	CHECK(refreshes >= 30u);
	CHECK(r.centre_millihz == latest);
	CHECK(!r.retarget_pending);
}

static void check_cell(uint32_t centre, uint32_t composed)
{
	AiusSpdifClockPlan a;
	AiusSpdifClockPlan b;

	CHECK(aius_spdif_clock_plan(48000u, centre, &a));
	CHECK(aius_spdif_clock_plan(48000u, composed, &b));
	CHECK(aius_spdif_clock_plan_structurally_equal(&a, &b));
}

static void test_fill_band_and_cell(void)
{
	static const uint32_t nominals[] = {32000u, 44100u, 48000u};
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	size_t i;

	current_key = "fill.band-and-cell-clamp";
	fill_accept(&t, 48549000u);
	r = fill_window(&t, Q4(200), 48549000u);
	CHECK(r.composed_target_millihz == 48549106u);
	CHECK(r.fill_offset_millihz == 106);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	check_cell(48549000u, r.composed_target_millihz);
	fill_accept(&t, 48549500u);
	r = fill_window(&t, Q4(-200), 48549500u);
	CHECK(r.composed_target_millihz == 48549107u);
	check_cell(48549500u, r.composed_target_millihz);
	fill_accept(&t, 48959800u);
	r = fill_window(&t, Q4(200), 48959800u);
	CHECK(r.composed_target_millihz == 48960000u);
	fill_accept(&t, 47040300u);
	r = fill_window(&t, Q4(-200), 47040300u);
	CHECK(r.composed_target_millihz == 47040000u);
	/* Every in-band centre has a valid plan, so the invalid-plan fault on
	 * a committed centre is unreachable from the tracker. */
	for (i = 0u; i < ARRAY_COUNT(nominals); ++i) {
		AiusSpdifClockPlan plan;
		const uint32_t lo = nominals[i] * 1000u - nominals[i] * 20u;
		const uint32_t hi = nominals[i] * 1000u + nominals[i] * 20u;
		uint32_t rate;

		for (rate = lo; rate <= hi; rate += (hi - lo) / 1000u)
			CHECK(aius_spdif_clock_plan(nominals[i], rate, &plan));
	}
}

static void test_fill_refusal_and_subthreshold(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	int32_t integral;

	current_key = "fill.refusal-retry-subthreshold";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(10), 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	integral = r.fill_integral_microhz;
	CHECK(integral == 3450);
	aius_rate_tracker_note_emit(&t, false);
	r = fill_again(&t, 48000000u); /* retried, controller not re-run */
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL);
	CHECK(r.fill_integral_microhz == integral);
	aius_rate_tracker_note_emit(&t, true);
	r = fill_again(&t, 48000000u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	r = fill_window(&t, Q4(10), 48000133u); /* +3 mHz: sub-threshold */
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	CHECK(r.fill_integral_microhz == 2 * integral);
}

static void test_fill_bumpless(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	uint32_t rb = 48000000u;
	uint32_t before;
	int32_t integral;
	uint32_t k;

	current_key = "fill.bumpless-nonzero-I";
	fill_accept(&t, 48000000u);
	for (k = 0u; k < 3u; ++k) {
		r = fill_window(&t, Q4(10), rb);
		aius_rate_tracker_note_emit(&t,
		                            r.emit_kind !=
		                                    AIUS_RATE_TRACKER_TRIM_NONE);
		rb = r.composed_target_millihz;
	}
	r = fill_again(&t, rb);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	before = r.composed_target_millihz;
	integral = r.fill_integral_microhz;
	CHECK(integral == 10350);
	CHECK(fill_propose(&t, 48000600u, 11000u));
	r = fill_again(&t, before);
	CHECK(r.centre_committed);
	CHECK(r.fill_integral_microhz == integral - 600000);
	CHECK(absolute_difference(r.composed_target_millihz, before) <= 1u);
	CHECK(r.emit_kind == AIUS_RATE_TRACKER_TRIM_NONE);
	CHECK(!t.centre_flag);

	current_key = "fill.bumpless-committed-once";
	for (k = 0u; k < 30u; ++k) {
		CHECK(!fill_propose(&t, 48000600u, 12000u + k * 1000u));
		r = fill_again(&t, before);
		CHECK(!r.centre_committed);
		CHECK(r.centre_millihz == 48000600u);
	}
}

static void test_fill_reset_and_accept(void)
{
	AiusRateTracker t;
	AiusRateTrackerFillResult r;
	uint32_t centre = 0u;

	current_key = "fill.reset-commit-and-discard";
	fill_accept(&t, 48000000u);
	r = fill_window(&t, Q4(200), 48000000u);
	CHECK(fill_propose(&t, 47999400u, 11000u));
	r = fill_again(&t, 48000900u);
	CHECK(r.retarget_pending);
	CHECK(aius_rate_tracker_fill_reset(&t, false, &centre));
	CHECK(centre == 47999400u && t.applied_target_millihz == centre);
	CHECK(t.fill_integral_microhz == 0 && t.fill_offset_millihz == 0);
	CHECK(t.pending_centre_millihz == 0u && !t.centre_flag);

	r = fill_window(&t, Q4(200), 47999400u);
	CHECK(fill_propose(&t, 47998800u, 22000u));
	r = fill_again(&t, 48000300u);
	CHECK(r.retarget_pending);
	CHECK(!aius_rate_tracker_fill_reset(&t, true, &centre));
	CHECK(centre == 47999400u && t.applied_target_millihz == 47999400u);
	CHECK(t.pending_centre_millihz == 0u);

	current_key = "fill.carrier-loss-and-accept";
	r = fill_window(&t, Q4(200), 47999400u);
	CHECK(fill_propose(&t, 47998800u, 33000u));
	r = fill_again(&t, 48000300u);
	CHECK(r.retarget_pending);
	(void)step(&t, false, 48000000u, AIUS_CLOCK_SOURCE_LIVE, 34100u);
	CHECK(t.pending_centre_millihz == 0u);
	(void)accept(&t, 48000300u, 35000u);
	CHECK(t.carrier_ready && t.applied_target_millihz == 48000300u);
	CHECK(t.fill_integral_microhz == 0 && !t.fill_saturated);
}

/* ======================================================================
 * Part 2: the plant model. TX consumes 128-frame periods at the applied
 * rate; the producer commits PCM blocks or burst units at the source rate
 * with trough-timing jitter; the sensor is the Phase 3 header; the carrier
 * estimate is quantised to 140 mHz with 1.2 Hz spread and 280 mHz wander.
 * The parent-side rules (dark-transition note with the apply-started flag,
 * the gate's arm counter, fill_reset sites, desired plan, request_trim) are
 * modelled here, in the shape optical_main must take.
 * ====================================================================== */

#define PLANT_RING_BASE UINT32_C(0x20200000)
#define PLANT_HALF_BYTES UINT32_C(1024)
#define PLANT_PASS_S 0.1
#define PLANT_DARK_S 0.05
#define PLANT_BLOCK_GAP_S 0.0002
#define PLANT_MAX_WINDOWS 6000u
#define PLANT_GOV_TARGET 128u
#define PLANT_GOV_DEADBAND 256u
#define PLANT_GOV_WINDOW 192000u

typedef enum {
	CLS_PCM = 0,
	CLS_AC3,
	CLS_EAC3,
	CLS_DTS2048,
	CLS_NOMARK
} PlantClass;

typedef struct {
	uint32_t blocks;
	uint32_t block_frames;
	uint32_t pc;
	int is_pcm;
	bool marks;
} ClassShape;

static ClassShape class_shape(PlantClass cls)
{
	ClassShape s = {1u, 512u, 0u, 1, true};

	switch (cls) {
	case CLS_AC3: s = (ClassShape){6u, 256u, 1u, 0, true}; break;
	case CLS_EAC3: s = (ClassShape){6u, 256u, 21u, 0, true}; break;
	case CLS_DTS2048: s = (ClassShape){4u, 512u, 13u, 0, true}; break;
	case CLS_NOMARK: s = (ClassShape){6u, 256u, 99u, 0, false}; break;
	case CLS_PCM: break;
	}
	return s;
}

typedef struct {
	uint32_t nominal_hz;
	uint32_t base_mhz;       /* 0: nominal * 1000 */
	PlantClass cls;
	int32_t offset_mhz;      /* true source minus centre (estimate bias) */
	double open_phase;       /* < 0: natural producer phase at the open */
	int32_t arm_block;       /* < 0: arm at once */
	double isr_latency_s;
	bool trough_noise;
	bool estimate_noise;
	double jitter_sigma_frames;
	double dip_p;
	double dip_frames;
	double ramp_mhz_per_min;
	uint64_t seed;
} PlantCfg;

typedef struct {
	double t;
	bool valid;
	double e;
	double composed;
	int32_t offset;
	int32_t p;
	int32_t integral;
	uint32_t fill_writes;
	bool saturated;
} PlantWindow;

typedef struct {
	PlantCfg cfg;
	uint64_t rng;
	double t;
	PlantClass cls;
	PlantClass next_cls;
	double src_extra_mhz;
	double est_extra_mhz;
	bool carrier;
	bool replay;
	bool replay_applied;
	/* TX */
	double half_start;
	double period;
	uint32_t half;
	uint32_t last_half;
	bool other_fresh;
	uint32_t level;
	uint32_t fills;
	uint32_t underruns;
	bool isr_pending;
	double isr_due;
	bool force_underrun;
	double last_underrun_t;
	double min_underrun_gap_s;
	/* clock */
	AiusSpdifClockPlan applied;
	AiusSpdifClockPlan desired;
	AiusSpdifClockPlan queued;
	AiusRateTrackerTrimKind queued_kind;
	bool plan_applied;
	bool trim_queued;
	uint32_t refuse_next;
	/* producer */
	double unit_due;
	double next_block_t;
	uint32_t block_in_unit;
	double dip_until;
	double rebase_s;
	/* gate */
	bool gate_open;
	bool armed;
	bool arm_wanted;
	int32_t arm_block;
	int32_t next_arm_block;
	uint32_t committed_since_arm;
	uint32_t setpoint;
	uint32_t opens;
	uint32_t open_block;
	uint32_t open_committed;
	bool open_pending;
	uint32_t open_offset;
	double open_true_occ;
	double open_t;
	/* sensor, governor */
	AiusTxFillSensor sensor;
	AiusTxGovernor gov;
	uint32_t governor_trims;
	double last_gov_t;
	/* tracker and parent */
	AiusRateTracker tr;
	bool dark;
	bool apply_started;
	double dark_until;
	uint32_t notes;
	uint32_t accepts;
	uint32_t full_applies;
	uint32_t structural_applies;
	uint32_t faults;
	uint32_t trim_updates;
	uint32_t proposals;
	uint32_t last_proposal_mhz;
	uint32_t fill_writes;
	uint32_t centre_writes;
	uint32_t fill_stamps;
	uint32_t centre_stamps;
	uint32_t refusals;
	uint32_t writes_during_replay;
	bool replay_plan_exact;
	bool first_fill_seen;
	uint32_t first_fill_centre;
	uint32_t first_fill_composed;
	bool pending_seen;
	uint32_t pending_first_seq;
	uint32_t commit_after_pending_windows;
	double next_pass_t;
	uint32_t seq_seen;
	bool have_seq;
	uint32_t nwin;
	PlantWindow win[PLANT_MAX_WINDOWS];
} Plant;

static Plant g_plant;

static uint64_t rng_next(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static double rng_unit(uint64_t *s)
{
	return (double)(rng_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

static double rng_gauss(uint64_t *s)
{
	double sum = 0.0;
	int i;

	for (i = 0; i < 12; ++i)
		sum += rng_unit(s);
	return sum - 6.0;
}

static double dabs(double v)
{
	return v < 0.0 ? -v : v;
}

static double dsqrt(double v)
{
	double x = v > 1.0 ? v : 1.0;
	int i;

	if (v <= 0.0)
		return 0.0;
	for (i = 0; i < 60; ++i)
		x = 0.5 * (x + v / x);
	return x;
}

static PlantCfg plant_cfg(uint32_t nominal, PlantClass cls, int32_t offset)
{
	PlantCfg c;

	memset(&c, 0, sizeof(c));
	c.nominal_hz = nominal;
	c.cls = cls;
	c.offset_mhz = offset;
	c.open_phase = -1.0;
	c.arm_block = -1;
	c.trough_noise = true;
	c.estimate_noise = true;
	c.jitter_sigma_frames = 1.5;
	c.dip_p = 0.04;
	c.dip_frames = 20.0;
	c.seed = UINT64_C(0x9e3779b97f4a7c15) ^ ((uint64_t)nominal << 20) ^
	         (uint64_t)(uint32_t)offset ^ ((uint64_t)cls << 40);
	return c;
}

static double plant_true_mhz(const Plant *p)
{
	if (p->replay)
		return 48000000.0;
	return (double)(p->cfg.base_mhz ? p->cfg.base_mhz
	                                : p->cfg.nominal_hz * 1000u) +
	       (double)p->cfg.offset_mhz +
	       p->cfg.ramp_mhz_per_min * p->t / 60.0 + p->src_extra_mhz;
}

static double plant_triangle(double t)
{
	double x = t / 960.0;

	x -= (double)(int64_t)x;
	return (x < 0.5 ? 4.0 * x - 1.0 : 3.0 - 4.0 * x) * 140.0;
}

static uint32_t plant_estimate(Plant *p)
{
	double v = plant_true_mhz(p) - (double)p->cfg.offset_mhz +
	           p->est_extra_mhz;

	if (p->cfg.estimate_noise && !p->replay) {
		v += (rng_unit(&p->rng) - 0.5) * 1200.0 + plant_triangle(p->t);
		v = (double)(int64_t)(v / 140.0 + 0.5) * 140.0;
	}
	return (uint32_t)(v + 0.5);
}

static uint32_t plant_readback(const Plant *p)
{
	uint32_t rate = 0u;

	CHECK(aius_spdif_clock_plan_reconstruct_rate(&p->applied, &rate));
	return rate;
}

static void plant_set_out_rate(Plant *p)
{
	p->period = 128.0 / ((double)plant_readback(p) / 1000.0);
}

static uint32_t plant_dma_frames(const Plant *p)
{
	const double frac = (p->t - p->half_start) / p->period;
	uint32_t off = frac <= 0.0 ? 0u : (uint32_t)(frac * 128.0);

	return off > 127u ? 127u : off;
}

static uint32_t plant_saddr(const Plant *p)
{
	return PLANT_RING_BASE + p->half * PLANT_HALF_BYTES +
	       plant_dma_frames(p) * AIUS_TX_FILL_FRAME_BYTES;
}

static uint32_t reader_fills(void *ctx)
{
	return ((Plant *)ctx)->fills;
}

static uint32_t reader_level(void *ctx)
{
	return ((Plant *)ctx)->level;
}

static uint32_t reader_offset(void *ctx)
{
	return aius_tx_fill_saddr_offset_frames(plant_saddr((Plant *)ctx),
	                                        PLANT_RING_BASE);
}

static void plant_gate_close(Plant *p)
{
	p->gate_open = false;
	p->armed = false;
	p->arm_wanted = false;
	p->level = 0u;
	aius_tx_fill_sensor_reset(&p->sensor);
}

/* clock_apply_desired_plan: dark, so the gate closes; the parent called
 * fill_reset before it at every site. */
static void plant_full_apply(Plant *p)
{
	p->applied = p->desired;
	p->plan_applied = true;
	p->trim_queued = false;
	plant_set_out_rate(p);
	p->dark = true;
	p->apply_started = true;
	p->dark_until = p->t + PLANT_DARK_S;
	p->full_applies++;
	plant_gate_close(p);
}

/* clock_record_fault: raises the dark flag with no apply. */
static void plant_fault(Plant *p)
{
	p->dark = true;
	p->apply_started = false;
	p->dark_until = p->t + 0.2;
	p->faults++;
	plant_gate_close(p);
}

/* Fault retry (:8767): fill_reset(false) commits a pending centre into the
 * desired plan, then the full apply. */
static void plant_fault_retry(Plant *p)
{
	uint32_t centre = 0u;

	if (aius_rate_tracker_fill_reset(&p->tr, false, &centre)) {
		AiusSpdifClockPlan plan;

		p->trim_updates++;
		if (aius_spdif_clock_plan(p->tr.accepted_nominal_hz, centre, &plan))
			p->desired = plan;
		else
			p->faults++;
	}
	plant_full_apply(p);
}

static void plant_carrier_loss(Plant *p)
{
	p->carrier = false;
	(void)aius_rate_tracker_fill_reset(&p->tr, true, NULL);
	plant_gate_close(p);
	p->setpoint = 0u;
}

static void plant_rephase(Plant *p)
{
	const double frac = (0.25 + p->cfg.open_phase * 127.5) / 128.0;

	p->unit_due = p->half_start + p->period * (2.0 + frac);
	p->next_block_t = p->unit_due;
	p->block_in_unit = 0u;
}

static void plant_arm(Plant *p, int32_t block)
{
	p->arm_wanted = true;
	p->arm_block = block;
	if (block < 0) {
		p->armed = true;
		p->committed_since_arm = 0u;
		if (p->cfg.open_phase >= 0.0)
			plant_rephase(p);
	}
}

static void plant_isr(Plant *p)
{
	p->isr_pending = false;
	if (p->trim_queued) {
		p->applied = p->queued;
		p->trim_queued = false;
		if (p->queued_kind == AIUS_RATE_TRACKER_TRIM_CENTRE)
			p->centre_stamps++;
		else
			p->fill_stamps++;
		plant_set_out_rate(p);
	}
	if (p->gate_open) {
		if (!p->force_underrun && p->level >= 128u) {
			p->level -= 128u;
		} else {
			p->force_underrun = false;
			p->underruns++;
			if (p->last_underrun_t > 0.0 &&
			    p->t - p->last_underrun_t > 1.0 &&
			    (p->min_underrun_gap_s == 0.0 ||
			     p->t - p->last_underrun_t < p->min_underrun_gap_s))
				p->min_underrun_gap_s = p->t - p->last_underrun_t;
			p->last_underrun_t = p->t;
			aius_tx_fill_sensor_note_underrun(&p->sensor);
		}
	}
	p->other_fresh = true;
	p->last_half = p->half ^ 1u;
	p->fills++;
}

static void plant_boundary(Plant *p)
{
	p->half_start += p->period;
	p->half ^= 1u;
	p->other_fresh = false; /* the half just played is stale */
	/* The masked gate open delays the boundary ISR: modelled only while
	 * the gate is closed, so the open can see a pending boundary. */
	if (p->cfg.isr_latency_s > 0.0 && !p->gate_open) {
		p->isr_pending = true;
		p->isr_due = p->half_start + p->cfg.isr_latency_s;
	} else {
		plant_isr(p);
	}
}

/* tx_gate_execute_open, inside its masked section: ONE SADDR read; the
 * shipped predicate and setpoint rule; the level-128 pad. */
static void plant_open(Plant *p)
{
	const uint32_t saddr = plant_saddr(p);
	const bool pending = aius_tx_fill_boundary_pending(
		saddr, PLANT_RING_BASE,
		PLANT_RING_BASE + p->last_half * PLANT_HALF_BYTES);
	const uint32_t offset =
		aius_tx_fill_saddr_offset_frames(saddr, PLANT_RING_BASE);
	const double frac = (p->t - p->half_start) / p->period;

	p->level = 128u;
	p->setpoint = aius_tx_fill_open_setpoint_frames(128u, offset, pending);
	p->open_true_occ = (double)p->level + (p->other_fresh ? 128.0 : 0.0) +
	                   128.0 * (1.0 - frac);
	p->open_pending = pending;
	p->open_offset = offset;
	p->open_block = p->block_in_unit;
	p->open_committed = p->committed_since_arm;
	p->open_t = p->t;
	p->opens++;
	p->gate_open = true;
	p->armed = false;
	p->arm_wanted = false;
	aius_tx_fill_sensor_reset(&p->sensor);
	aius_tx_governor_reset(&p->gov, p->underruns);
}

static double plant_jitter_s(Plant *p, double src_hz)
{
	double frames;

	if (!p->cfg.trough_noise)
		return 0.0;
	frames = dabs(rng_gauss(&p->rng)) * p->cfg.jitter_sigma_frames;
	if (rng_unit(&p->rng) < p->cfg.dip_p)
		frames += rng_unit(&p->rng) * p->cfg.dip_frames;
	return frames / src_hz;
}

static void plant_block(Plant *p)
{
	const ClassShape sh = class_shape(p->cls);
	const bool marked = sh.marks && p->block_in_unit == 0u;
	const uint32_t unit_frames =
		aius_tx_fill_unit_frames(sh.pc, sh.block_frames, sh.is_pcm);
	const double src_hz = plant_true_mhz(p) / 1000.0;
	double arrival;

	if (!p->carrier)
		goto schedule;
	if (p->arm_wanted && !p->armed && p->arm_block >= 0 &&
	    p->block_in_unit == (uint32_t)p->arm_block) {
		p->armed = true;
		p->committed_since_arm = 0u;
	}
	/* The parent's arm counter: frames produced since the arm. */
	if (!p->gate_open && p->armed) {
		if (aius_tx_fill_open_decide(marked, p->committed_since_arm,
		                             unit_frames) ==
		    AIUS_TX_FILL_OPEN_NOW)
			plant_open(p);
		else
			p->committed_since_arm += sh.block_frames;
	}
	if (p->gate_open) {
		if (p->replay) {
			(void)aius_tx_fill_sensor_hold(&p->sensor, 1, 1, 1);
		} else {
			const AiusTxFillReader reader = {
				p, reader_fills, reader_level, reader_offset};
			uint32_t excess;

			(void)aius_tx_fill_sensor_sample(&p->sensor, &reader);
			excess = aius_tx_governor_step(
				&p->gov, PLANT_GOV_TARGET, PLANT_GOV_DEADBAND,
				PLANT_GOV_WINDOW, p->level, sh.block_frames,
				p->underruns, 1);
			if (excess != 0u) {
				p->level -= excess < p->level ? excess : p->level;
				p->governor_trims++;
				p->last_gov_t = p->t;
				aius_tx_fill_sensor_reset_keep_latches(&p->sensor);
			}
		}
		p->level += sh.block_frames;
		if (!p->replay)
			aius_tx_fill_sensor_commit(
				&p->sensor, sh.block_frames, unit_frames,
				(uint32_t)(uint64_t)(p->t * 1e6), p->cfg.nominal_hz);
	}
schedule:
	p->block_in_unit++;
	if (p->block_in_unit >= sh.blocks) {
		ClassShape next;

		p->block_in_unit = 0u;
		/* An in-lock class switch: the new class's first unit lands where
		 * the old class's next unit would have (floor kept), then the
		 * injected re-base. */
		if (p->next_cls != p->cls) {
			p->cls = p->next_cls;
			next = sh;
		} else {
			next = class_shape(p->cls);
		}
		p->unit_due += (double)(next.blocks * next.block_frames) / src_hz +
		               p->rebase_s;
		p->rebase_s = 0.0;
		arrival = p->unit_due + plant_jitter_s(p, src_hz);
	} else {
		arrival = p->t + PLANT_BLOCK_GAP_S;
	}
	if (arrival < p->dip_until)
		arrival = p->dip_until;
	if (arrival < p->t)
		arrival = p->t;
	p->next_block_t = arrival;
}

static bool plant_request_trim(Plant *p, const AiusSpdifClockPlan *plan,
                               AiusRateTrackerTrimKind kind)
{
	if (!p->plan_applied || p->trim_queued ||
	    plan->nominal_rate_hz != p->applied.nominal_rate_hz ||
	    !aius_spdif_clock_plan_structurally_equal(plan, &p->applied)) {
		p->refusals++;
		return false;
	}
	if (p->refuse_next != 0u) {
		p->refuse_next--;
		p->refusals++;
		return false;
	}
	p->queued = *plan;
	p->queued_kind = kind;
	p->trim_queued = true;
	return true;
}

static void plant_record_window(Plant *p,
                                const AiusTxFillSnapshot *snap,
                                const AiusRateTrackerFillResult *fr)
{
	PlantWindow *w;

	if (p->nwin >= PLANT_MAX_WINDOWS)
		return;
	w = &p->win[p->nwin++];
	w->t = p->t;
	w->valid = snap->fill_level_valid != 0 && p->gate_open;
	w->e = (double)snap->fill_level_q4 / 16.0 - (double)p->setpoint;
	w->composed = (double)fr->composed_target_millihz;
	w->offset = fr->fill_offset_millihz;
	w->p = p->tr.fill_proportional_millihz;
	w->integral = fr->fill_integral_microhz;
	w->fill_writes = 0u;
	w->saturated = fr->saturated;
}

static void plant_pass(Plant *p)
{
	const uint32_t now_ms = (uint32_t)(uint64_t)(p->t * 1000.0);
	AiusRateTrackerSample sample;
	AiusRateTrackerResult r;
	AiusRateTrackerFillInput in;
	AiusRateTrackerFillResult fr;
	AiusTxFillSnapshot snap;
	bool applied_now = false;

	memset(&sample, 0, sizeof(sample));
	sample.carrier_locked = p->carrier;
	sample.carrier_rate_millihz = plant_estimate(p);
	sample.source = p->replay ? AIUS_CLOCK_SOURCE_REPLAY
	                          : AIUS_CLOCK_SOURCE_LIVE;
	sample.now_ms = now_ms;
	r = aius_rate_tracker_step(&p->tr, &sample);
	if (r.target_update) {
		p->proposals++;
		p->last_proposal_mhz = r.target_rate_millihz;
	}
	if (r.quiesce_requested) {
		(void)aius_rate_tracker_fill_reset(&p->tr, true, NULL);
		plant_gate_close(p);
		p->setpoint = 0u;
	}
	if (p->replay && !p->replay_applied) {
		(void)aius_rate_tracker_fill_reset(&p->tr, true, NULL);
		CHECK(aius_spdif_clock_plan(48000u, 48000000u, &p->desired));
		plant_full_apply(p);
		p->replay_applied = true;
		applied_now = true;
	}
	if (r.nominal_accepted) {
		(void)aius_rate_tracker_fill_reset(&p->tr, true, NULL);
		CHECK(aius_spdif_clock_plan(r.nominal_rate_hz,
		                            r.target_rate_millihz, &p->desired));
		plant_full_apply(p);
		p->accepts++;
		applied_now = true;
	}
	if (p->dark && p->t >= p->dark_until) {
		p->dark = false;
		/* Note once, on the edge, only for a transition the apply
		 * started, never for a replay, at the accepted nominal. */
		if (p->apply_started && !p->replay &&
		    p->applied.nominal_rate_hz == p->tr.accepted_nominal_hz) {
			CHECK(aius_rate_tracker_note_target_applied(
				&p->tr, plant_readback(p), now_ms));
			p->notes++;
		}
		p->apply_started = false;
		if (p->replay) {
			p->gate_open = true;
			p->level = 128u;
		} else if (p->tr.carrier_ready && !p->gate_open) {
			plant_arm(p, p->next_arm_block);
		}
	}

	snap = aius_tx_fill_sensor_snapshot(&p->sensor);
	memset(&in, 0, sizeof(in));
	in.emit_allowed = !p->replay && p->tr.carrier_ready &&
	                  p->plan_applied && p->gate_open && !p->dark;
	in.have_applied = p->plan_applied;
	in.emission_readback_millihz = p->plan_applied ? plant_readback(p) : 0u;
	in.setpoint_frames = p->setpoint;
	in.window.window_seq = snap.fill_window_seq;
	in.window.level_q4 = snap.fill_level_q4;
	in.window.level_valid = snap.fill_level_valid != 0;
	in.window.exclusions = (uint8_t)snap.fill_window_exclusions;
	fr = aius_rate_tracker_fill_step(&p->tr, &in);

	if (fr.retarget_pending && !p->pending_seen) {
		p->pending_seen = true;
		p->pending_first_seq = snap.fill_window_seq;
	}
	if (fr.centre_committed) {
		AiusSpdifClockPlan plan;

		if (p->pending_seen) {
			p->commit_after_pending_windows =
				snap.fill_window_seq - p->pending_first_seq;
			p->pending_seen = false;
		}
		p->trim_updates++;
		if (!aius_spdif_clock_plan(p->tr.accepted_nominal_hz,
		                           fr.centre_millihz, &plan)) {
			plant_fault(p);
		} else if (p->plan_applied &&
		           !aius_spdif_clock_plan_structurally_equal(
				   &plan, &p->applied)) {
			/* Structural full apply (:8903), I zeroed. */
			(void)aius_rate_tracker_fill_reset(&p->tr, false, NULL);
			p->desired = plan;
			plant_full_apply(p);
			p->structural_applies++;
			applied_now = true;
		} else {
			p->desired = plan;
		}
	} else if (!fr.retarget_pending) {
		p->pending_seen = false; /* dropped or discarded */
	}
	if (!p->have_seq || snap.fill_window_seq != p->seq_seen) {
		if (p->have_seq)
			plant_record_window(p, &snap, &fr);
		p->have_seq = true;
		p->seq_seen = snap.fill_window_seq;
	}
	if (fr.emit_kind != AIUS_RATE_TRACKER_TRIM_NONE && !applied_now) {
		AiusSpdifClockPlan plan;
		const bool ok = aius_spdif_clock_plan(p->tr.accepted_nominal_hz,
		                                      fr.composed_target_millihz,
		                                      &plan) &&
		                plant_request_trim(p, &plan, fr.emit_kind);

		aius_rate_tracker_note_emit(&p->tr, ok);
		if (ok && p->replay)
			p->writes_during_replay++;
		if (ok && fr.emit_kind == AIUS_RATE_TRACKER_TRIM_FILL) {
			p->fill_writes++;
			if (p->nwin != 0u)
				p->win[p->nwin - 1u].fill_writes++;
			if (!p->first_fill_seen) {
				p->first_fill_seen = true;
				p->first_fill_centre = fr.centre_millihz;
				p->first_fill_composed = fr.composed_target_millihz;
			}
		} else if (ok) {
			p->centre_writes++;
		}
	}
	if (p->replay && p->replay_applied && !p->dark &&
	    p->applied.target_rate_millihz != 48000000u)
		p->replay_plan_exact = false;
}

static void plant_init(Plant *p, const PlantCfg *cfg)
{
	memset(p, 0, sizeof(*p));
	p->cfg = *cfg;
	p->rng = cfg->seed != 0u ? cfg->seed : UINT64_C(88172645463325252);
	p->cls = cfg->cls;
	p->next_cls = cfg->cls;
	p->carrier = true;
	p->replay_plan_exact = true;
	aius_rate_tracker_init(&p->tr);
	aius_tx_fill_sensor_init(&p->sensor);
	aius_tx_governor_reset(&p->gov, 0u);
	p->period = 128.0 / (double)cfg->nominal_hz;
	p->half = 0u;
	p->last_half = 1u;
	p->other_fresh = true;
	p->unit_due = 0.0123;
	p->next_block_t = p->unit_due;
	p->next_pass_t = 0.05;
	p->next_arm_block = cfg->arm_block;
}

static void plant_run(Plant *p, double until)
{
	for (;;) {
		const double t_tx = p->half_start + p->period;
		const double t_isr = p->isr_pending ? p->isr_due : 1e300;
		const double t_blk = p->next_block_t;
		const double t_pass = p->next_pass_t;
		double tn = t_tx;
		int which = 0;

		if (t_isr < tn) {
			tn = t_isr;
			which = 1;
		}
		if (t_blk < tn) {
			tn = t_blk;
			which = 2;
		}
		if (t_pass < tn) {
			tn = t_pass;
			which = 3;
		}
		if (tn > until) {
			p->t = until;
			return;
		}
		p->t = tn;
		if (which == 0)
			plant_boundary(p);
		else if (which == 1)
			plant_isr(p);
		else if (which == 2)
			plant_block(p);
		else {
			plant_pass(p);
			p->next_pass_t += PLANT_PASS_S;
		}
	}
}

typedef struct {
	uint32_t n;
	double max_abs_e;
	double mean_e;
	double sd_e;
	double min_e;
	double rms_mod;
	double max_mod;
	double mean_p;
	uint32_t writes;
	uint32_t max_writes;
	uint32_t saturated;
} PlantStats;

static PlantStats plant_stats(const Plant *p, double from, double to)
{
	PlantStats s;
	double sum_e = 0.0;
	double sum_c = 0.0;
	double sum_p = 0.0;
	double sq_e = 0.0;
	double sq_c = 0.0;
	uint32_t i;

	memset(&s, 0, sizeof(s));
	s.min_e = 1e9;
	for (i = 0u; i < p->nwin; ++i) {
		const PlantWindow *w = &p->win[i];

		if (w->t < from || w->t > to || !w->valid)
			continue;
		s.n++;
		sum_e += w->e;
		sum_c += w->composed;
		sum_p += w->p;
		if (dabs(w->e) > s.max_abs_e)
			s.max_abs_e = dabs(w->e);
		if (w->e < s.min_e)
			s.min_e = w->e;
		s.writes += w->fill_writes;
		if (w->fill_writes > s.max_writes)
			s.max_writes = w->fill_writes;
		if (w->saturated)
			s.saturated++;
	}
	if (s.n == 0u)
		return s;
	s.mean_e = sum_e / s.n;
	s.mean_p = sum_p / s.n;
	for (i = 0u; i < p->nwin; ++i) {
		const PlantWindow *w = &p->win[i];
		double dc;

		if (w->t < from || w->t > to || !w->valid)
			continue;
		dc = w->composed - sum_c / s.n;
		sq_e += (w->e - s.mean_e) * (w->e - s.mean_e);
		sq_c += dc * dc;
		if (dabs(dc) > s.max_mod)
			s.max_mod = dabs(dc);
	}
	s.sd_e = dsqrt(sq_e / s.n);
	s.rms_mod = dsqrt(sq_c / s.n);
	return s;
}

/* Last window time in [from, to] with |e| above the bound; from if none. */
static double plant_last_outside(const Plant *p, double from, double to,
                                 double bound)
{
	double last = from;
	uint32_t i;

	for (i = 0u; i < p->nwin; ++i) {
		const PlantWindow *w = &p->win[i];

		if (w->t >= from && w->t <= to && w->valid && dabs(w->e) > bound)
			last = w->t;
	}
	return last;
}

/* First window time after from with |e| back within the bound, once it has
 * left it; from if it never left. */
static double plant_first_inside(const Plant *p, double from, double bound)
{
	bool left = false;
	uint32_t i;

	for (i = 0u; i < p->nwin; ++i) {
		const PlantWindow *w = &p->win[i];

		if (w->t < from || !w->valid)
			continue;
		if (dabs(w->e) > bound)
			left = true;
		else if (left)
			return w->t;
	}
	return from;
}

static bool within(double measured, double expected, double fraction)
{
	return dabs(measured - expected) <= dabs(expected) * fraction;
}

#define SESSION_S 7200.0

/* The estimate's own noise (1.2 Hz sample spread, 140 mHz bins, 280 mHz
 * wander) retargets the centre about once per five minutes, each bumpless;
 * offsets kept inside the authority budget (hysteresis 500 + estimate error
 * 170). */
static void test_plant_estimate_noise(void)
{
	static const int32_t offsets[] = {0, 225, -225, 480, -480};
	double worst_mod = 0.0;
	uint32_t updates = 0u;
	size_t o;

	for (o = 0u; o < ARRAY_COUNT(offsets); ++o) {
		Plant *p = &g_plant;
		PlantCfg cfg = plant_cfg(48000u, CLS_AC3, offsets[o]);
		PlantStats all;
		PlantStats late;

		snprintf(g_key, sizeof(g_key), "plant.estimate-noise offset=%d",
		         (int)offsets[o]);
		current_key = g_key;
		plant_init(p, &cfg);
		plant_run(p, SESSION_S);
		all = plant_stats(p, p->open_t, SESSION_S);
		late = plant_stats(p, SESSION_S - 3600.0, SESSION_S);
		CHECK(p->underruns == 0u && p->governor_trims == 0u);
		CHECK(all.max_abs_e <= 64.0);
		CHECK(all.max_writes <= 1u);
		CHECK(late.rms_mod <= 25.0);
		if (late.rms_mod > worst_mod)
			worst_mod = late.rms_mod;
		updates += p->trim_updates;
	}
	printf("plant.estimate-noise: offsets 0/+-225/+-480, %u bumpless centre "
	       "retargets in 10 h, worst modulation rms %.1f mHz, 0 underruns "
	       "PASS\n",
	       (unsigned)updates, worst_mod);
}

/* Offsets x open phases x rates; also the opening kick at offset 0. */
static void test_plant_offsets(void)
{
	static const int32_t offsets[] = {0, 225, -225, 480, -480, 550, -550,
	                                  650, -650, 800, -800};
	static const double phases[] = {0.0, 0.47, 1.0};
	static const uint32_t rates[] = {48000u, 44100u, 32000u};
	size_t r, o, ph;

	for (r = 0u; r < ARRAY_COUNT(rates); ++r) {
		double worst_e = 0.0;
		double worst_mod = 0.0;
		double worst_p = 0.0;
		double wf_lo = 1.0;
		double wf_hi = 0.0;

		for (o = 0u; o < ARRAY_COUNT(offsets); ++o) {
			for (ph = 0u; ph < ARRAY_COUNT(phases); ++ph) {
				Plant *p = &g_plant;
				PlantCfg cfg = plant_cfg(rates[r], CLS_AC3, offsets[o]);
				PlantStats all;
				PlantStats late;
				double wf;
				uint32_t i;
				uint32_t kicks = 0u;

				cfg.open_phase = phases[ph];
				/* Offsets are against the centre: hold it fixed. */
				cfg.estimate_noise = false;
				cfg.seed ^= (uint64_t)(ph + 1u) << 50;
				snprintf(g_key, sizeof(g_key),
				         "plant.offset rate=%u offset=%d phi=%.2f",
				         (unsigned)rates[r], (int)offsets[o],
				         phases[ph]);
				current_key = g_key;
				plant_init(p, &cfg);
				plant_run(p, SESSION_S);
				CHECK(p->opens == 1u);
				CHECK(p->underruns == 0u);
				CHECK(p->governor_trims == 0u);
				CHECK(dabs((double)p->setpoint - p->open_true_occ) <= 1.0);
				all = plant_stats(p, p->open_t, SESSION_S);
				late = plant_stats(p, SESSION_S - 3600.0, SESSION_S);
				CHECK(all.max_abs_e <= 64.0);
				CHECK(late.rms_mod <= 25.0);
				CHECK(dabs(late.mean_p) <= 15.0);
				CHECK(all.max_writes <= 1u);
				wf = (double)all.writes / (double)all.n;
				if (all.max_abs_e > worst_e)
					worst_e = all.max_abs_e;
				if (late.rms_mod > worst_mod)
					worst_mod = late.rms_mod;
				if (dabs(late.mean_p) > worst_p)
					worst_p = dabs(late.mean_p);
				if (wf < wf_lo)
					wf_lo = wf;
				if (wf > wf_hi)
					wf_hi = wf;
				if (offsets[o] != 0)
					continue;
				/* The opening kick. */
				for (i = 0u; i < p->nwin && kicks < 3u; ++i) {
					const PlantWindow *w = &p->win[i];

					if (!w->valid || w->t < p->open_t)
						continue;
					kicks++;
					CHECK(dabs(w->e) <= 16.0);
					CHECK(w->offset <= 200 && w->offset >= -200);
				}
				CHECK(kicks == 3u);
			}
		}
		printf("plant.offsets rate=%u: 33 sessions PASS; worst |e| %.1f "
		       "frames, worst modulation rms %.1f mHz, worst |mean P| "
		       "%.1f mHz, write fraction %.2f..%.2f\n",
		       (unsigned)rates[r], worst_e, worst_mod, worst_p, wf_lo,
		       wf_hi);
	}
}

static void test_plant_pending_boundary(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 225);
	uint32_t non_pending_setpoint;

	current_key = "plant.pending-boundary-open";
	cfg.open_phase = 2.0 / 127.5;
	cfg.isr_latency_s = 150e-6;
	plant_init(p, &cfg);
	plant_run(p, 3600.0 + 5.0);
	CHECK(p->opens == 1u && p->open_pending);
	CHECK(p->setpoint == 256u - p->open_offset);
	CHECK(dabs((double)p->setpoint - p->open_true_occ) <= 1.0);
	CHECK(p->underruns == 0u && p->governor_trims == 0u);
	printf("plant.pending-boundary: offset %u, setpoint %u (true %.1f), "
	       "0 underruns, 0 governor trims over 1 h PASS\n",
	       (unsigned)p->open_offset, (unsigned)p->setpoint,
	       p->open_true_occ);
	cfg.isr_latency_s = 0.0;
	plant_init(p, &cfg);
	plant_run(p, 60.0);
	CHECK(p->opens == 1u && !p->open_pending);
	non_pending_setpoint = p->setpoint;
	CHECK(non_pending_setpoint == 384u - p->open_offset);
}

static void test_plant_unit_deferral(void)
{
	static const struct {
		PlantClass cls;
		const char *name;
		uint32_t k_max;
	} classes[] = {
		{CLS_AC3, "AC-3", 5u},
		{CLS_EAC3, "E-AC-3", 5u},
		{CLS_DTS2048, "DTS-2048", 3u},
	};
	size_t c;
	uint32_t k;

	for (c = 0u; c < ARRAY_COUNT(classes); ++c) {
		const ClassShape sh = class_shape(classes[c].cls);

		for (k = 1u; k <= classes[c].k_max; ++k) {
			Plant *p = &g_plant;
			PlantCfg cfg = plant_cfg(48000u, classes[c].cls, 225);

			snprintf(g_key, sizeof(g_key), "plant.unit-deferral %s k=%u",
			         classes[c].name, (unsigned)k);
			current_key = g_key;
			cfg.arm_block = (int32_t)k;
			plant_init(p, &cfg);
			plant_run(p, 600.0);
			CHECK(p->opens == 1u);
			CHECK(p->open_block == 0u);
			CHECK(p->open_committed ==
			      (sh.blocks - k) * sh.block_frames);
			CHECK(p->underruns == 0u);
		}
	}
	{
		Plant *p = &g_plant;
		PlantCfg cfg = plant_cfg(48000u, CLS_NOMARK, 225);

		current_key = "plant.unit-deferral never-marks";
		cfg.arm_block = 2;
		plant_init(p, &cfg);
		plant_run(p, 600.0);
		CHECK(p->opens == 1u);
		CHECK(p->open_committed ==
		      aius_tx_fill_unit_frames(99u, 256u, 0));
		printf("plant.unit-deferral: AC-3 k=1..5, E-AC-3 k=1..5, DTS-2048 "
		       "k=1..3 open at the next unit start, 0 underruns PASS; "
		       "never-marking class opens at the %u-frame cap (%u "
		       "underruns, mid-unit open)\n",
		       (unsigned)p->open_committed, (unsigned)p->underruns);
	}
}

/* Closed-form figures (entry gate items 4 to 6), noise-free plant. */
static void test_plant_closed_form(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_PCM, 0);
	PlantStats s;
	double t_step;
	double peak = 0.0;
	double t16;
	double t_settle;
	uint32_t i;

	current_key = "plant.closed-form-step";
	cfg.trough_noise = false;
	cfg.estimate_noise = false;
	plant_init(p, &cfg);
	plant_run(p, 900.0);
	t_step = p->t;
	p->src_extra_mhz = 700.0;
	p->est_extra_mhz = -700.0;
	plant_run(p, t_step + 2400.0);
	for (i = 0u; i < p->nwin; ++i)
		if (p->win[i].valid && p->win[i].t > t_step &&
		    dabs(p->win[i].e) > peak)
			peak = dabs(p->win[i].e);
	{
		/* The closed form: continuous PI, 4 s window, plant u/1000
		 * frames/s. de/dt = d - (Kp e + I)/1000, dI/dt = Ki/4 e. The 2%
		 * settle is against the response envelope d/w_d (the classic
		 * 4/(zeta w_n), 10.3 min); 2% of the peak is below the sensor's
		 * one-frame resolution. */
		const double kp = 13.0;
		const double ki = 0.345 / 4.0;
		const double wn2 = ki / 1000.0;
		const double wd = dsqrt(wn2 - (kp / 2000.0) * (kp / 2000.0));
		const double settle_bound = 0.02 * 0.7 / wd;
		double e = 0.0;
		double integral = 0.0;
		double cf_peak = 0.0;
		double cf_t16 = 0.0;
		double cf_settle = 0.0;
		double tt;

		for (tt = 0.0; tt < 2400.0; tt += 0.01) {
			const double de = 0.7 - (kp * e + integral) / 1000.0;

			integral += ki * e * 0.01;
			e += de * 0.01;
			if (e > cf_peak)
				cf_peak = e;
			if (dabs(e) > 16.0)
				cf_t16 = tt;
			if (dabs(e) > settle_bound)
				cf_settle = tt;
		}
		t16 = plant_last_outside(p, t_step, p->t, 16.0) - t_step;
		t_settle = plant_last_outside(p, t_step, p->t, settle_bound) -
		           t_step;
		printf("plant.closed-form step 700 mHz: peak %.1f frames "
		       "(closed form %.1f, plan 34.5); inside 16 frames at %.0f s "
		       "(closed form %.0f, plan text ~210); settled (2%% of "
		       "d/w_d = %.2f frames) at %.0f s (closed form %.0f, plan "
		       "618)\n",
		       peak, cf_peak, t16, cf_t16, settle_bound, t_settle,
		       cf_settle);
		CHECK(within(peak, 34.5, 0.2) && within(peak, cf_peak, 0.2));
		CHECK(within(t16, cf_t16, 0.2));
		/* 618 s is the envelope's bound; the response settles inside it. */
		CHECK(within(t_settle, cf_settle, 0.2) &&
		      t_settle <= 618.0 * 1.2);
	}
	CHECK(p->underruns == 0u);

	current_key = "plant.closed-form-write-fraction";
	cfg = plant_cfg(48000u, CLS_AC3, 225);
	plant_init(p, &cfg);
	plant_run(p, SESSION_S);
	s = plant_stats(p, 1800.0, SESSION_S); /* settled */
	printf("plant.closed-form write fraction at 20 mHz: %.2f (plan 0.35 to "
	       "0.45), statistic sd %.2f frames (Phase 3 AC-3 1.17), "
	       "modulation rms %.1f mHz (plan 15 to 18.5)\n",
	       (double)s.writes / s.n, s.sd_e, s.rms_mod);
	CHECK(within((double)s.writes / s.n, 0.40, 0.2));
	CHECK(within(s.sd_e, 1.17, 0.35));
}

/* One underrun at the setpoint with I at -670, 0, +670 mHz. */
static void test_plant_underrun_recovery(void)
{
	static const int32_t integrals[] = {-670, 0, 670};
	static const double expected_s[] = {99.0, 144.0, 491.0};
	static const double expected_under[] = {26.0, 15.0, 4.0};
	static const uint32_t rates[] = {48000u, 44100u, 32000u};
	size_t i;
	size_t r;

	for (i = 0u; i < ARRAY_COUNT(integrals); ++i) {
		Plant *p = &g_plant;
		PlantCfg cfg = plant_cfg(48000u, CLS_AC3, integrals[i]);
		double t0;
		double rec;
		PlantStats after;
		int32_t integral_before;

		snprintf(g_key, sizeof(g_key), "plant.underrun-recovery I=%d",
		         (int)integrals[i]);
		current_key = g_key;
		cfg.trough_noise = false;
		cfg.estimate_noise = false;
		plant_init(p, &cfg);
		plant_run(p, 2700.0);
		integral_before = p->tr.fill_integral_microhz;
		CHECK(within(integral_before / 1000.0, integrals[i], 0.05) ||
		      (integrals[i] == 0 && integral_before > -20000 &&
		       integral_before < 20000));
		t0 = p->t;
		p->force_underrun = true;
		plant_run(p, t0 + 1800.0);
		rec = plant_first_inside(p, t0, 16.0) - t0;
		after = plant_stats(p, t0 + rec, p->t);
		printf("plant.underrun-recovery I=%+d mHz: back within 16 frames "
		       "at %.0f s (plan %.0f), undershoot %.1f frames (plan %.0f), "
		       "last outside 16 at %.0f s, underruns %u, governor %u\n",
		       (int)integrals[i], rec, expected_s[i], -after.min_e,
		       expected_under[i],
		       plant_last_outside(p, t0, p->t, 16.0) - t0,
		       (unsigned)p->underruns, (unsigned)p->governor_trims);
		CHECK(p->underruns == 1u && p->governor_trims == 0u);
		CHECK(within(rec, expected_s[i], 0.2));
	}
	/* With noise, at each rate: no second underrun, no governor trim. */
	for (r = 0u; r < ARRAY_COUNT(rates); ++r) {
		for (i = 0u; i < ARRAY_COUNT(integrals); ++i) {
			Plant *p = &g_plant;
			PlantCfg cfg = plant_cfg(rates[r], CLS_AC3, integrals[i]);
			double t0;

			snprintf(g_key, sizeof(g_key),
			         "plant.underrun-noise rate=%u I=%d",
			         (unsigned)rates[r], (int)integrals[i]);
			current_key = g_key;
			plant_init(p, &cfg);
			plant_run(p, 2700.0);
			t0 = p->t;
			p->force_underrun = true;
			plant_run(p, t0 + 1800.0);
			CHECK(p->underruns == 1u && p->governor_trims == 0u);
		}
	}
	printf("plant.underrun-noise: 48/44.1/32 kHz x I -670/0/+670: one "
	       "underrun, no governor trim PASS\n");
}

/* Saturation retargets in the plant: drift past +900 during an underrun
 * recovery (same sign, at once) and an opposite-sign estimate step. */
static void test_plant_saturation_retargets(void)
{
	Plant *p = &g_plant;
	/* I at +670 inside the authority budget: a 170 mHz estimate error
	 * plus 499 mHz of drift inside the hysteresis. */
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 170);
	double t0;
	uint32_t updates;
	double s;

	current_key = "plant.drift-past-clamp-same-sign";
	cfg.trough_noise = false;
	cfg.estimate_noise = false;
	plant_init(p, &cfg);
	plant_run(p, 60.0);
	p->src_extra_mhz = 499.0; /* after the accept, inside the hysteresis */
	plant_run(p, 2700.0);
	CHECK(p->tr.fill_integral_microhz > 640000);
	t0 = p->t;
	p->force_underrun = true;
	plant_run(p, t0 + 30.0);
	CHECK(p->tr.fill_saturated && p->tr.fill_offset_millihz == 900);
	updates = p->trim_updates;
	for (s = 0.0; s < 60.0; s += 1.0) {
		p->src_extra_mhz = 499.0 + 1500.0 * (s + 1.0) / 60.0;
		plant_run(p, t0 + 30.0 + s + 1.0);
		CHECK(p->tr.pending_centre_windows <= 1u);
	}
	plant_run(p, t0 + 1800.0);
	CHECK(p->trim_updates > updates);
	CHECK(!p->tr.fill_saturated);
	CHECK(p->underruns == 1u && p->governor_trims == 0u);
	printf("plant.drift-past-clamp: %u same-sign commits, none deferred "
	       "past one window, desaturated, 1 underrun, 0 governor PASS\n",
	       (unsigned)(p->trim_updates - updates));

	/* A real drift of -600 (source and estimate) while saturated at +900:
	 * that saturation resolves itself, so the retarget defers. */
	current_key = "plant.opposite-sign-defers";
	cfg = plant_cfg(48000u, CLS_AC3, 670);
	cfg.trough_noise = false;
	cfg.estimate_noise = false;
	plant_init(p, &cfg);
	plant_run(p, 2700.0);
	t0 = p->t;
	p->force_underrun = true;
	plant_run(p, t0 + 20.0);
	CHECK(p->tr.fill_saturated && p->tr.fill_offset_millihz > 0);
	updates = p->trim_updates;
	p->src_extra_mhz = -600.0;
	plant_run(p, t0 + 32.0);
	CHECK(p->tr.pending_centre_millihz != 0u);
	CHECK(p->trim_updates == updates);
	plant_run(p, t0 + 1800.0);
	CHECK(p->trim_updates == updates + 1u);
	CHECK(p->commit_after_pending_windows <=
	      AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS);
	CHECK(p->underruns == 1u && p->governor_trims == 0u);
	printf("plant.opposite-sign: deferred, committed after %u windows "
	       "(cap 120), 1 underrun PASS\n",
	       (unsigned)p->commit_after_pending_windows);
}

/* +/-950 past the clamp with the estimate biased so the centre stays off;
 * then the deferral cap with 10 s re-proposals. */
static void test_plant_past_clamp(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg;
	PlantStats s;
	uint32_t updates;
	uint32_t proposals;
	uint32_t k;
	double t0;

	current_key = "plant.past-clamp +950";
	cfg = plant_cfg(48000u, CLS_AC3, 950);
	plant_init(p, &cfg);
	plant_run(p, SESSION_S);
	s = plant_stats(p, 1800.0, SESSION_S);
	printf("plant.past-clamp +950: %u governor trims, %u underruns, "
	       "saturated %u of %u windows (today: a splice every ~4.5 min)\n",
	       (unsigned)p->governor_trims, (unsigned)p->underruns,
	       (unsigned)s.saturated, (unsigned)s.n);
	CHECK(p->underruns == 0u);
	CHECK(p->governor_trims >= 1u && p->governor_trims <= 2u);
	CHECK(s.saturated * 10u >= s.n * 9u);

	current_key = "plant.defer-cap-reproposals";
	t0 = p->t;
	updates = p->trim_updates;
	proposals = p->proposals;
	p->pending_seen = false;
	p->cfg.estimate_noise = false;
	for (k = 0u; k < 53u; ++k) {
		/* An estimate error the loop cannot see; refreshed values. */
		p->est_extra_mhz = -600.0 - (double)(k % 5u) * 20.0;
		plant_run(p, t0 + 10.0 * (k + 1u));
	}
	CHECK(p->trim_updates == updates + 1u);
	CHECK(p->commit_after_pending_windows ==
	      AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS);
	CHECK(p->proposals - proposals >= 40u);
	CHECK(p->tr.applied_target_millihz == p->last_proposal_mhz);
	printf("plant.defer-cap: committed at window %u after %u re-proposals, "
	       "latest value PASS\n",
	       (unsigned)p->commit_after_pending_windows,
	       (unsigned)(p->proposals - proposals));

	current_key = "plant.past-clamp -950";
	cfg = plant_cfg(48000u, CLS_AC3, -950);
	plant_init(p, &cfg);
	plant_run(p, SESSION_S);
	printf("plant.past-clamp -950: %u underruns (min gap %.0f s), %u "
	       "governor trims (today: an underrun every ~2.2 min)\n",
	       (unsigned)p->underruns, p->min_underrun_gap_s,
	       (unsigned)p->governor_trims);
	CHECK(p->governor_trims == 0u);
	CHECK(p->underruns >= 1u && p->underruns <= 4u);
	CHECK(p->underruns == 1u || p->min_underrun_gap_s >= 1200.0);
}

/* Set up an opposite-sign deferral: I at +670, an underrun (saturated at
 * +900 for minutes), a real -600 mHz drift. */
static void plant_deferral(Plant *p, PlantCfg *cfg)
{
	*cfg = plant_cfg(48000u, CLS_AC3, 670);
	cfg->trough_noise = false;
	cfg->estimate_noise = false;
	plant_init(p, cfg);
	plant_run(p, 2700.0);
	p->force_underrun = true;
	plant_run(p, p->t + 20.0);
	p->src_extra_mhz = -600.0;
	plant_run(p, p->t + 12.0);
	CHECK(p->tr.fill_saturated);
	CHECK(p->tr.pending_centre_millihz == 47999400u);
}

static void test_plant_resets_during_deferral(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg;
	uint32_t pending;
	uint32_t notes;
	uint32_t updates;
	uint32_t writes;

	current_key = "plant.fault-retry-during-deferral";
	plant_deferral(p, &cfg);
	pending = p->tr.pending_centre_millihz;
	notes = p->notes;
	plant_fault_retry(p);
	CHECK(p->desired.target_rate_millihz == pending);
	CHECK(p->tr.applied_target_millihz == pending);
	CHECK(p->tr.fill_integral_microhz == 0);
	plant_run(p, p->t + 5.0);
	CHECK(p->notes == notes + 1u);
	CHECK(p->opens == 2u);
	updates = p->trim_updates;
	p->src_extra_mhz = 0.0; /* drifts back */
	plant_run(p, p->t + 30.0);
	CHECK(p->trim_updates == updates + 1u); /* retargets resume */
	CHECK(p->tr.applied_target_millihz == 48000000u);

	current_key = "plant.replay-during-deferral";
	plant_deferral(p, &cfg);
	writes = p->fill_writes + p->centre_writes;
	p->replay = true;
	plant_run(p, p->t + 60.0);
	CHECK(p->tr.pending_centre_millihz == 0u);
	CHECK(p->applied.target_rate_millihz == 48000000u);
	CHECK(p->replay_plan_exact);
	CHECK(p->gate_open && p->writes_during_replay == 0u);
	CHECK(p->fill_writes + p->centre_writes == writes);
	CHECK(p->tr.fill_integral_microhz == 0);
	p->replay = false;
	p->replay_applied = false;
	plant_run(p, p->t + 30.0);
	CHECK(p->tr.carrier_ready && p->gate_open);
	CHECK(p->tr.fill_integral_microhz != 0 || p->nwin > 0u);

	current_key = "plant.carrier-loss-during-deferral";
	plant_deferral(p, &cfg);
	plant_carrier_loss(p);
	CHECK(p->tr.pending_centre_millihz == 0u);
	plant_run(p, p->t + 0.3);
	p->carrier = true;
	plant_run(p, p->t + 30.0);
	CHECK(p->tr.pending_centre_millihz == 0u && p->gate_open);
	printf("plant.resets-during-deferral: fault retry commits the pending "
	       "centre (I 0, one note, retargets resume); replay and carrier "
	       "loss discard it, replay plan exactly 48000000, no writes PASS\n");
}

static void test_plant_structural_apply(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 0);
	uint32_t refusals;

	current_key = "plant.structural-full-apply";
	cfg.base_mhz = 48549000u;
	cfg.estimate_noise = false;
	plant_init(p, &cfg);
	plant_run(p, 600.0);
	p->src_extra_mhz = 600.0;
	plant_run(p, 640.0);
	CHECK(p->structural_applies == 1u);
	/* The note re-asserts the full apply's readback (reconstruction can
	 * differ by 1 mHz). */
	CHECK(absolute_difference(p->tr.applied_target_millihz, 48549600u) <=
	      1u);
	CHECK(p->desired.target_rate_millihz == 48549600u);
	CHECK(aius_spdif_clock_plan_structurally_equal(&p->applied,
	                                               &p->desired));
	refusals = p->refusals;
	plant_run(p, 2400.0);
	CHECK(p->refusals == refusals);
	CHECK(p->underruns == 0u);
	printf("plant.structural-apply: centre across the 48549107 crossing "
	       "takes the full apply, no permanent refusal PASS\n");
}

static void test_plant_relock(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 225);
	uint32_t old_centre;
	uint32_t new_centre;

	current_key = "plant.relock-same-nominal";
	plant_init(p, &cfg);
	plant_run(p, 1200.0);
	old_centre = p->tr.applied_target_millihz;
	plant_carrier_loss(p);
	plant_run(p, p->t + 0.3);
	p->src_extra_mhz = 2000.0;
	p->carrier = true;
	p->first_fill_seen = false;
	plant_run(p, p->t + 120.0);
	new_centre = p->tr.applied_target_millihz;
	CHECK(p->first_fill_seen && p->opens == 2u);
	CHECK(p->first_fill_centre == new_centre);
	CHECK(absolute_difference(p->first_fill_composed, new_centre) <= 900u);
	CHECK(absolute_difference(p->first_fill_composed, old_centre) > 900u);

	current_key = "plant.relock-nominal-change";
	plant_carrier_loss(p);
	plant_run(p, p->t + 0.3);
	p->cfg.nominal_hz = 44100u;
	p->src_extra_mhz = 300.0;
	p->carrier = true;
	p->first_fill_seen = false;
	plant_run(p, p->t + 3.0);
	CHECK(p->tr.accepted_nominal_hz == 44100u);
	CHECK(p->tr.fill_integral_microhz == 0);
	plant_run(p, p->t + 600.0);
	CHECK(p->first_fill_seen);
	CHECK(p->first_fill_centre / 1000u == 44100u);
	CHECK(absolute_difference(p->first_fill_composed,
	                          p->tr.applied_target_millihz) <= 900u);
	printf("plant.relock: first FILL write composed on the new centre "
	       "(same nominal +2000 mHz, and 48 -> 44.1 kHz) PASS\n");
}

static void test_plant_fault_edge(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 225);
	uint32_t notes;
	uint32_t centre;
	uint32_t request_ms;

	current_key = "plant.fault-edge-no-note";
	cfg.estimate_noise = false;
	plant_init(p, &cfg);
	plant_run(p, 600.0);
	notes = p->notes;
	centre = p->tr.applied_target_millihz;
	request_ms = p->tr.last_target_request_ms;
	plant_fault(p);
	plant_run(p, p->t + 2.0);
	CHECK(!p->dark);
	CHECK(p->notes == notes);
	CHECK(p->tr.applied_target_millihz == centre);
	CHECK(p->tr.last_target_request_ms == request_ms);
	printf("plant.fault-edge: no note, centre and cadence unchanged PASS\n");
}

static void test_plant_gate_blip(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 480);
	int32_t integral;
	double composed_before;
	double t0;
	double worst = 0.0;
	uint32_t i;

	current_key = "plant.gate-blip-keeps-I";
	plant_init(p, &cfg);
	plant_run(p, 1800.0);
	integral = p->tr.fill_integral_microhz;
	composed_before = p->win[p->nwin - 1u].composed;
	plant_gate_close(p);
	plant_run(p, p->t + 0.1);
	plant_arm(p, 3);
	t0 = p->t;
	plant_run(p, p->t + 0.2);
	CHECK(p->opens == 2u && p->open_block == 0u);
	CHECK(p->open_committed == 3u * 256u);
	CHECK(p->tr.fill_integral_microhz == integral);
	plant_run(p, t0 + 60.0);
	for (i = 0u; i < p->nwin; ++i)
		if (p->win[i].t > t0 && p->win[i].valid &&
		    dabs(p->win[i].composed - composed_before) > worst)
			worst = dabs(p->win[i].composed - composed_before);
	CHECK(worst <= 200.0);
	CHECK(p->underruns == 0u);
	printf("plant.gate-blip: reopened at the burst start, setpoint %u "
	       "re-read, I kept, worst composed move %.0f mHz over 60 s PASS\n",
	       (unsigned)p->setpoint, worst);
}

static void test_plant_stall_backlog(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 225);
	double dip_end;
	PlantStats s;

	current_key = "plant.stall-backlog-2048";
	plant_init(p, &cfg);
	plant_run(p, 1800.0);
	dip_end = p->t + 2048.0 / 48000.0;
	p->dip_until = dip_end;
	plant_run(p, p->t + 1800.0);
	CHECK(p->underruns > 0u && p->last_underrun_t < dip_end + 0.1);
	CHECK(p->governor_trims == 1u);
	s = plant_stats(p, p->last_gov_t + 10.0, p->t);
	CHECK(s.max_abs_e <= 16.0);
	printf("plant.stall-backlog: %u silent periods in the stall, governor "
	       "trims once, |e| <= %.1f after, no later underrun PASS\n",
	       (unsigned)p->underruns, s.max_abs_e);
}

static void test_plant_ramp(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 0);
	PlantStats s;

	current_key = "plant.warm-up-ramp";
	cfg.trough_noise = false;
	cfg.estimate_noise = false;
	cfg.ramp_mhz_per_min = -14.0;
	plant_init(p, &cfg);
	plant_run(p, 2400.0);
	s = plant_stats(p, 1800.0, 2400.0);
	printf("plant.ramp -14 mHz/min: tracking error %.2f frames (plan "
	       "2.7), %u centre retargets\n",
	       s.mean_e, (unsigned)p->trim_updates);
	CHECK(within(-s.mean_e, 2.7, 0.2));
	CHECK(p->underruns == 0u);
	cfg.trough_noise = true;
	cfg.estimate_noise = true;
	plant_init(p, &cfg);
	plant_run(p, 2400.0);
	CHECK(p->underruns == 0u && p->governor_trims == 0u);
}

static void test_plant_class_switch(void)
{
	static const double rebases[] = {64.0, -64.0};
	size_t i;

	for (i = 0u; i < ARRAY_COUNT(rebases); ++i) {
		Plant *p = &g_plant;
		PlantCfg cfg = plant_cfg(48000u, CLS_PCM, 225);
		PlantStats before;
		PlantStats after;
		double t0;

		snprintf(g_key, sizeof(g_key),
		         "plant.in-lock-class-switch rebase=%+.0f", rebases[i]);
		current_key = g_key;
		plant_init(p, &cfg);
		plant_run(p, 1200.0);
		before = plant_stats(p, 600.0, 1200.0);
		t0 = p->t;
		p->next_cls = CLS_AC3;
		p->rebase_s = rebases[i] / 48000.0;
		plant_run(p, t0 + 2400.0);
		after = plant_stats(p, t0 + 1200.0, t0 + 2400.0);
		printf("plant.class-switch PCM->AC-3 rebase %+.0f frames: floor "
		       "bias %.1f frames, modulation rms %.1f mHz, %u underruns\n",
		       rebases[i], before.mean_e - after.mean_e,
		       after.rms_mod, (unsigned)p->underruns);
		CHECK(p->underruns == 0u && p->governor_trims == 0u);
		CHECK(after.rms_mod <= 25.0);
	}
}

static void test_plant_bumpless_and_refusal(void)
{
	Plant *p = &g_plant;
	PlantCfg cfg = plant_cfg(48000u, CLS_AC3, 225);
	uint32_t updates;
	uint32_t centre_writes;
	uint32_t fills;

	current_key = "plant.bumpless-retarget-writes-nothing";
	cfg.trough_noise = false;
	cfg.estimate_noise = false;
	plant_init(p, &cfg);
	plant_run(p, 1800.0);
	updates = p->trim_updates;
	centre_writes = p->centre_writes;
	p->est_extra_mhz = 600.0;
	plant_run(p, p->t + 120.0);
	CHECK(p->trim_updates == updates + 1u);
	CHECK(p->centre_writes == centre_writes);
	CHECK(p->desired.target_rate_millihz == p->tr.applied_target_millihz);
	CHECK(p->tr.applied_target_millihz == 48000600u);

	current_key = "plant.trim-refusal-retried";
	fills = p->fill_writes;
	p->refuse_next = 3u;
	p->src_extra_mhz = 100.0; /* move the loop */
	plant_run(p, p->t + 600.0);
	CHECK(p->refuse_next == 0u);
	CHECK(p->fill_writes > fills);
	CHECK(p->underruns == 0u);
	printf("plant.bumpless: committed once with no write, desired plan "
	       "follows; refusals retried PASS\n");
}

int main(void)
{
	test_snap_boundaries();
	test_tracker_accepts_all_rates();
	test_tracker_requires_consecutive_locked_samples();
	test_tracker_rejects_settling_physical_rate();
	test_tracker_quiesces_on_first_changed_sample();
	test_tracker_lock_source_and_rate_limit();
	test_tracker_average_hysteresis_and_cadence();
	test_plan_rates_and_edges();
	test_plan_integer_crossings_and_equality();
	test_plan_rejects_invalid_input();
	test_fill_fixed_point();
	test_fill_invalid_windows();
	test_fill_readback_never_centre();
	test_fill_centre_kind();
	test_fill_stale_pending();
	test_fill_saturated_retarget();
	test_fill_defer_cap();
	test_fill_band_and_cell();
	test_fill_refusal_and_subthreshold();
	test_fill_bumpless();
	test_fill_reset_and_accept();
	puts("fill tracker API cases passed");
	test_plant_closed_form();
	test_plant_underrun_recovery();
	test_plant_pending_boundary();
	test_plant_unit_deferral();
	test_plant_saturation_retargets();
	test_plant_past_clamp();
	test_plant_resets_during_deferral();
	test_plant_structural_apply();
	test_plant_relock();
	test_plant_fault_edge();
	test_plant_gate_blip();
	test_plant_stall_backlog();
	test_plant_ramp();
	test_plant_class_switch();
	test_plant_bumpless_and_refusal();
	test_plant_estimate_noise();
	test_plant_offsets();
	puts("clock golden tests passed");
	return 0;
}
