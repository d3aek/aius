#include "aius_clock.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

	for (i = 0u; i < 8u; ++i)
		result = step(tracker, true, rate_millihz,
		              AIUS_CLOCK_SOURCE_LIVE, start_ms + i);
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
		for (sample = 0u; sample < 7u; ++sample) {
			result = step(&tracker, true, rates[i],
			              AIUS_CLOCK_SOURCE_LIVE, sample);
			CHECK(!result.carrier_ready);
			CHECK(!result.nominal_accepted);
		}
		result = step(&tracker, true, rates[i],
		              AIUS_CLOCK_SOURCE_LIVE, 7u);
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
	for (i = 0u; i < 7u; ++i)
		result = step(&tracker, true, 48000000u,
		              AIUS_CLOCK_SOURCE_LIVE, i);
	CHECK(!result.carrier_ready);
	result = step(&tracker, false, 48000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 7u);
	CHECK(!result.carrier_ready);
	for (i = 0u; i < 7u; ++i)
		result = step(&tracker, true, 48000000u,
		              AIUS_CLOCK_SOURCE_LIVE, 8u + i);
	CHECK(!result.carrier_ready);
	result = step(&tracker, true, 48000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 15u);
	CHECK(result.carrier_ready);
	CHECK(result.nominal_accepted);
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
	              AIUS_CLOCK_SOURCE_LIVE, 1000u);
	CHECK(!result.carrier_ready);
	CHECK(result.quiesce_requested);
	CHECK(!result.nominal_accepted);
	for (i = 1u; i < 7u; ++i) {
		result = step(&tracker, true, 44100000u,
		              AIUS_CLOCK_SOURCE_LIVE, 1000u + i);
		CHECK(!result.carrier_ready);
	}
	result = step(&tracker, true, 44100000u,
	              AIUS_CLOCK_SOURCE_LIVE, 1007u);
	CHECK(result.carrier_ready);
	CHECK(result.nominal_accepted);
	CHECK(result.nominal_changed);

	result = step(&tracker, true, 60000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 2000u);
	CHECK(!result.carrier_ready);
	CHECK(result.quiesce_requested);
	CHECK(!result.unsupported_accepted);
	for (i = 1u; i < 8u; ++i)
		result = step(&tracker, true, 60000000u,
		              AIUS_CLOCK_SOURCE_LIVE, 2000u + i);
	CHECK(result.unsupported_accepted);
	CHECK(!result.carrier_ready);
	result = step(&tracker, true, 60000000u,
	              AIUS_CLOCK_SOURCE_LIVE, 2008u);
	CHECK(!result.unsupported_accepted);
}

static void test_tracker_lock_source_and_rate_limit(void)
{
	AiusRateTracker tracker;
	AiusRateTrackerResult result;
	uint32_t i;

	current_key = "tracker.lock-source-rate-limit";
	aius_rate_tracker_init(&tracker);
	result = accept(&tracker, 48000000u, UINT32_MAX - 10u);
	CHECK(result.carrier_ready);
	result = step(&tracker, false, 48000000u,
	              AIUS_CLOCK_SOURCE_LIVE, UINT32_MAX - 2u);
	CHECK(result.quiesce_requested);
	CHECK(!result.carrier_ready);
	result = step(&tracker, true, 48000000u,
	              AIUS_CLOCK_SOURCE_REPLAY, UINT32_MAX - 1u);
	CHECK(!result.carrier_ready);

	for (i = 0u; i < 8u; ++i)
		result = step(&tracker, true, 44100000u,
		              AIUS_CLOCK_SOURCE_LIVE, 100u + i);
	CHECK(!result.carrier_ready);
	CHECK(!result.nominal_accepted);
	result = step(&tracker, true, 44100000u,
	              AIUS_CLOCK_SOURCE_LIVE, 996u);
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
		&tracker, 48000000u, 7u));
	CHECK(!aius_rate_tracker_note_target_applied(
		&tracker, 44100000u, 7u));

	for (second = 1u; second < 10u; ++second) {
		run_window(&tracker, 48000100u, 47999900u,
		           second * 1000u, &result);
		CHECK(!result.target_update);
	}
	run_window(&tracker, 48000049u, 48000049u, 10000u, &result);
	CHECK(!result.target_update);
	run_window(&tracker, 48000050u, 48000050u, 11001u, &result);
	CHECK(result.target_update);
	CHECK(result.target_rate_millihz == 48000050u);
	CHECK(aius_rate_tracker_note_target_applied(
		&tracker, result.target_rate_millihz, 12001u));
	run_window(&tracker, 48000150u, 48000150u, 12002u, &result);
	CHECK(!result.target_update);
	run_window(&tracker, 48000150u, 48000150u, 21002u, &result);
	CHECK(result.target_update);
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

int main(void)
{
	test_snap_boundaries();
	test_tracker_accepts_all_rates();
	test_tracker_requires_consecutive_locked_samples();
	test_tracker_quiesces_on_first_changed_sample();
	test_tracker_lock_source_and_rate_limit();
	test_tracker_average_hysteresis_and_cadence();
	test_plan_rates_and_edges();
	test_plan_integer_crossings_and_equality();
	test_plan_rejects_invalid_input();
	puts("clock golden tests passed");
	return 0;
}
