#include <aius_audio_clock.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../lib/aius_audio/src/aius_audio_clock_internal.h"
#include "../../lib/aius_audio/src/aius_spdif_tx24.h"

namespace {

constexpr size_t REGISTER_COUNT =
	static_cast<size_t>(
		AIUS_AUDIO_CLOCK_REG_SPDIF_RATE_MEASUREMENT) + 1u;
constexpr size_t TRACE_CAPACITY = 128u;
constexpr uint32_t PLL_BYPASS = UINT32_C(1) << 16;
constexpr uint32_t PLL_ENABLE = UINT32_C(1) << 13;
constexpr uint32_t PLL_POWERDOWN = UINT32_C(1) << 12;
constexpr uint32_t PLL_POST_DIVIDE_4 = UINT32_C(2) << 19;
constexpr uint32_t PLL_MISC_DIV_MASK =
	(UINT32_C(1) << 23) | (UINT32_C(1) << 15);
constexpr uint32_t SPDIF_ROOT_MASK =
	(UINT32_C(7) << 25) |
	(UINT32_C(7) << 22) |
	(UINT32_C(3) << 20);
constexpr uint32_t SPDIF_TX_CLOCK_MASK =
	(UINT32_C(7) << 8) | UINT32_C(0x7f);
constexpr uint32_t CHANNEL_RATE_MASK = UINT32_C(3) << 22;
constexpr uint32_t PHASE_LOCK = UINT32_C(1) << 6;

struct TraceEntry {
	AiusAudioClockRegister reg;
	uint32_t value;
};

struct FakeTarget {
	uint32_t registers[REGISTER_COUNT];
	TraceEntry trace[TRACE_CAPACITY];
	size_t trace_count;
	uint32_t cycle;
	uint32_t reported_cycle;
	uint32_t initial_cycle;
	uint32_t lock_after_cycles;
	bool lock_enabled;
};

FakeTarget *g_isr_target;

static size_t register_index(AiusAudioClockRegister reg)
{
	const size_t index = static_cast<size_t>(reg);
	assert(index < REGISTER_COUNT);
	return index;
}

static void fake_reset(FakeTarget *target)
{
	memset(target, 0, sizeof(*target));
	target->registers[register_index(
		AIUS_AUDIO_CLOCK_REG_PLL_MISC2)] = UINT32_C(0xf0f0f0f0);
	target->registers[register_index(
		AIUS_AUDIO_CLOCK_REG_CLOCK_GATE_5)] = UINT32_C(0x5a5aa5a5);
	target->registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT)] = UINT32_C(0xffffffff);
	target->registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL)] = UINT32_C(0x00000400);
	target->registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK)] = UINT32_C(0x00000080);
	target->registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW)] =
		UINT32_C(0x15345678);
	target->lock_enabled = true;
	target->initial_cycle = target->cycle;
	target->reported_cycle = target->cycle;
	g_isr_target = target;
	aius_audio_clock_initialize();
}

static uint32_t fake_read(
	void *context,
	AiusAudioClockRegister reg)
{
	FakeTarget *const target = static_cast<FakeTarget *>(context);
	uint32_t value = target->registers[register_index(reg)];
	if (reg == AIUS_AUDIO_CLOCK_REG_PLL_CONTROL &&
	    target->lock_enabled &&
	    static_cast<uint32_t>(
		    target->reported_cycle - target->initial_cycle) >=
		    target->lock_after_cycles)
		value |= AIUS_AUDIO_CLOCK_PLL_LOCK_BIT;
	return value;
}

static void fake_write(
	void *context,
	AiusAudioClockRegister reg,
	uint32_t value)
{
	FakeTarget *const target = static_cast<FakeTarget *>(context);
	assert(target->trace_count < TRACE_CAPACITY);
	target->trace[target->trace_count++] = {reg, value};
	target->registers[register_index(reg)] = value;
}

static uint32_t fake_cycle(void *context)
{
	FakeTarget *const target = static_cast<FakeTarget *>(context);
	target->reported_cycle = target->cycle;
	target->cycle++;
	return target->reported_cycle;
}

static AiusAudioClockBackend fake_backend(FakeTarget *target)
{
	AiusAudioClockBackend backend = {};
	backend.context = target;
	backend.read_register = fake_read;
	backend.write_register = fake_write;
	backend.read_cycle_count = fake_cycle;
	backend.cycles_per_microsecond = 1u;
	backend.bus_frequency_hz = UINT32_C(150000000);
	return backend;
}

static AiusSpdifClockPlan exact_plan(uint32_t nominal_rate_hz)
{
	AiusSpdifClockPlan plan = {};
	plan.nominal_rate_hz = nominal_rate_hz;
	plan.target_rate_millihz = nominal_rate_hz * UINT32_C(1000);
	plan.pll_div_select = 28u;
	plan.pll_denominator = AIUS_CLOCK_PLL_DENOMINATOR;
	plan.root_predivider = 8u;
	plan.root_postdivider = 1u;
	switch (nominal_rate_hz) {
	case 32000u:
		plan.pll_numerator = 672000u;
		plan.tx_clock_divider = 42u;
		plan.channel_status_rate_bits =
			AIUS_CLOCK_STCSCL_RATE_32000;
		break;
	case 44100u:
		plan.pll_numerator = 224000u;
		plan.tx_clock_divider = 30u;
		plan.channel_status_rate_bits =
			AIUS_CLOCK_STCSCL_RATE_44100;
		break;
	case 48000u:
		plan.pll_numerator = 672000u;
		plan.tx_clock_divider = 28u;
		plan.channel_status_rate_bits =
			AIUS_CLOCK_STCSCL_RATE_48000;
		break;
	default:
		assert(false);
	}
	return plan;
}

static void assert_plan_equal(
	const AiusSpdifClockPlan &actual,
	const AiusSpdifClockPlan &expected)
{
	assert(actual.nominal_rate_hz == expected.nominal_rate_hz);
	assert(
		actual.target_rate_millihz ==
		expected.target_rate_millihz);
	assert(actual.pll_div_select == expected.pll_div_select);
	assert(actual.pll_numerator == expected.pll_numerator);
	assert(actual.pll_denominator == expected.pll_denominator);
	assert(actual.root_predivider == expected.root_predivider);
	assert(actual.root_postdivider == expected.root_postdivider);
	assert(actual.tx_clock_divider == expected.tx_clock_divider);
	assert(
		actual.channel_status_rate_bits ==
		expected.channel_status_rate_bits);
}

static void assert_write(
	const FakeTarget &target,
	size_t index,
	AiusAudioClockRegister reg,
	uint32_t value)
{
	assert(index < target.trace_count);
	assert(target.trace[index].reg == reg);
	if (target.trace[index].value != value)
		fprintf(
			stderr,
			"trace[%zu] value 0x%08x, expected 0x%08x\n",
			index,
			target.trace[index].value,
			value);
	assert(target.trace[index].value == value);
}

static void clear_trace(FakeTarget *target)
{
	target->trace_count = 0u;
}

static AiusAudioClockBoundaryStatus boundary_status(void)
{
	AiusAudioClockBoundaryStatus status = {};
	assert(aius_audio_clock_get_boundary_status(&status));
	return status;
}

static void acknowledge_hold(void)
{
	AiusAudioClockBoundaryStatus status;

	/* Arm boundary: it is not one of the subsequently completed periods. */
	aius_audio_clock_test_tx_boundary();
	status = boundary_status();
	assert(!status.request_pending);
	assert(status.invalid_active);
	assert(!status.transition_complete);
	assert(
		status.periods_remaining ==
		AIUS_AUDIO_CLOCK_INVALID_PERIODS);

	for (uint32_t remaining = 3u; remaining != 0u; remaining--) {
		aius_audio_clock_test_tx_boundary();
		status = boundary_status();
		assert(status.invalid_active);
		assert(!status.transition_complete);
		assert(status.periods_remaining == remaining);
	}

	aius_audio_clock_test_tx_boundary();
	status = boundary_status();
	assert(!status.request_pending);
	assert(!status.invalid_active);
	assert(status.transition_complete);
	assert(status.periods_remaining == 0u);
}

static void test_immediate_lock_and_register_order(void)
{
	FakeTarget target;
	fake_reset(&target);
	AiusAudioClockBackend backend = fake_backend(&target);
	const AiusSpdifClockPlan plan = exact_plan(48000u);
	const uint32_t initial_misc =
		target.registers[register_index(
			AIUS_AUDIO_CLOCK_REG_PLL_MISC2)];
	const uint32_t initial_gate =
		target.registers[register_index(
			AIUS_AUDIO_CLOCK_REG_CLOCK_GATE_5)];
	const uint32_t initial_root =
		target.registers[register_index(
			AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT)];
	const uint32_t initial_tx =
		target.registers[register_index(
			AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK)];
	const uint32_t initial_status =
		target.registers[register_index(
			AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW)];
	const uint32_t pll_dark =
		PLL_BYPASS |
		PLL_ENABLE |
		PLL_POWERDOWN |
		PLL_POST_DIVIDE_4 |
		plan.pll_div_select;
	const uint32_t pll_waiting = pll_dark & ~PLL_POWERDOWN;

	assert(
		aius_audio_clock_apply_with_backend(&plan, &backend) ==
		AIUS_CLOCK_APPLY_OK);
	assert(target.trace_count == 10u);
	assert_write(
		target,
		0u,
		AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
		UINT32_C(0x00000400) |
			AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT);
	assert_write(
		target, 1u, AIUS_AUDIO_CLOCK_REG_PLL_CONTROL, pll_dark);
	assert_write(
		target,
		2u,
		AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR,
		plan.pll_numerator);
	assert_write(
		target,
		3u,
		AIUS_AUDIO_CLOCK_REG_PLL_DENOMINATOR,
		AIUS_CLOCK_PLL_DENOMINATOR);
	assert_write(
		target,
		4u,
		AIUS_AUDIO_CLOCK_REG_PLL_CONTROL,
		pll_waiting);
	assert_write(
		target,
		5u,
		AIUS_AUDIO_CLOCK_REG_PLL_MISC2,
		initial_misc & ~PLL_MISC_DIV_MASK);
	assert_write(
		target,
		6u,
		AIUS_AUDIO_CLOCK_REG_PLL_CONTROL,
		pll_waiting & ~PLL_BYPASS);
	assert_write(
		target,
		7u,
		AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT,
		(initial_root & ~SPDIF_ROOT_MASK) |
			(UINT32_C(7) << 25));
	assert_write(
		target,
		8u,
		AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK,
		(initial_tx & ~SPDIF_TX_CLOCK_MASK) |
			(UINT32_C(1) << 8) |
			(plan.tx_clock_divider - 1u));
	assert_write(
		target,
		9u,
		AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW,
		(initial_status & ~CHANNEL_RATE_MASK) |
			plan.channel_status_rate_bits);
	assert(
		target.registers[register_index(
			AIUS_AUDIO_CLOCK_REG_CLOCK_GATE_5)] == initial_gate);
	for (size_t index = 0; index < target.trace_count; index++)
		assert(
			target.trace[index].reg !=
			AIUS_AUDIO_CLOCK_REG_CLOCK_GATE_5);

	AiusSpdifClockPlan applied = {};
	assert(aius_audio_clock_get_applied_plan(&applied));
	assert_plan_equal(applied, plan);
	const AiusAudioClockBoundaryStatus status = boundary_status();
	assert(status.request_pending);
	assert(status.invalid_active);
	assert(!status.transition_complete);
	assert(status.periods_remaining == 0u);
}

static void test_last_cycle_lock(void)
{
	FakeTarget target;
	fake_reset(&target);
	target.lock_after_cycles = AIUS_AUDIO_CLOCK_LOCK_TIMEOUT_US;
	AiusAudioClockBackend backend = fake_backend(&target);
	const AiusSpdifClockPlan plan = exact_plan(44100u);

	assert(
		aius_audio_clock_apply_with_backend(&plan, &backend) ==
		AIUS_CLOCK_APPLY_OK);
	assert(
		static_cast<uint32_t>(
			target.reported_cycle - target.initial_cycle) ==
		AIUS_AUDIO_CLOCK_LOCK_TIMEOUT_US);
}

static void test_cycle_wrap_is_bounded(void)
{
	FakeTarget target;
	fake_reset(&target);
	target.cycle = UINT32_MAX - 499u;
	target.initial_cycle = target.cycle;
	target.reported_cycle = target.cycle;
	target.lock_after_cycles = AIUS_AUDIO_CLOCK_LOCK_TIMEOUT_US;
	AiusAudioClockBackend backend = fake_backend(&target);
	const AiusSpdifClockPlan plan = exact_plan(32000u);

	assert(
		aius_audio_clock_apply_with_backend(&plan, &backend) ==
		AIUS_CLOCK_APPLY_OK);
	assert(
		static_cast<uint32_t>(
			target.reported_cycle - target.initial_cycle) ==
		AIUS_AUDIO_CLOCK_LOCK_TIMEOUT_US);
}

static void test_never_lock_latches_failure(void)
{
	FakeTarget target;
	fake_reset(&target);
	target.lock_enabled = false;
	AiusAudioClockBackend backend = fake_backend(&target);
	const AiusSpdifClockPlan plan = exact_plan(32000u);

	assert(
		aius_audio_clock_apply_with_backend(&plan, &backend) ==
		AIUS_CLOCK_APPLY_LOCK_TIMEOUT);
	assert(target.trace_count == 5u);
	assert(
		(target.trace[0].value &
		 AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT) != 0u);
	assert(
		target.trace[1].reg ==
		AIUS_AUDIO_CLOCK_REG_PLL_CONTROL);
	assert(
		target.trace[2].reg ==
		AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR);
	assert(
		target.trace[3].reg ==
		AIUS_AUDIO_CLOCK_REG_PLL_DENOMINATOR);
	assert(
		target.trace[4].reg ==
		AIUS_AUDIO_CLOCK_REG_PLL_CONTROL);
	assert(
		(target.registers[register_index(
			AIUS_AUDIO_CLOCK_REG_PLL_CONTROL)] &
		 PLL_BYPASS) != 0u);

	AiusSpdifClockPlan applied = {};
	assert(!aius_audio_clock_get_applied_plan(&applied));
	AiusAudioClockBoundaryStatus status = boundary_status();
	assert(!status.request_pending);
	assert(status.invalid_active);
	assert(!status.transition_complete);
	assert(status.periods_remaining == 0u);

	clear_trace(&target);
	for (size_t i = 0; i < 8u; i++)
		aius_audio_clock_test_tx_boundary();
	assert(target.trace_count == 0u);
	status = boundary_status();
	assert(status.invalid_active);
	assert(!status.transition_complete);
}

static void test_all_nominal_mappings_and_four_period_hold(void)
{
	const uint32_t rates[] = {32000u, 44100u, 48000u};
	for (uint32_t rate : rates) {
		FakeTarget target;
		fake_reset(&target);
		AiusAudioClockBackend backend = fake_backend(&target);
		const AiusSpdifClockPlan plan = exact_plan(rate);

		assert(
			aius_audio_clock_apply_with_backend(
				&plan, &backend) ==
			AIUS_CLOCK_APPLY_OK);
		assert(
			target.trace[8].value ==
			(UINT32_C(0x80) |
			 (UINT32_C(1) << 8) |
			 (plan.tx_clock_divider - 1u)));
		assert(
			(target.trace[9].value & CHANNEL_RATE_MASK) ==
			plan.channel_status_rate_bits);

		clear_trace(&target);
		acknowledge_hold();
		/* Hold-only arm writes validity, never NUM. The fourth
		 * acknowledgement is the one clear-valid write. */
		assert(target.trace_count == 2u);
		assert(
			target.trace[0].reg ==
			AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
		assert(
			(target.trace[0].value &
			 AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT) != 0u);
		assert(
			target.trace[1].reg ==
			AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
		assert(
			(target.trace[1].value &
			 AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT) == 0u);
	}
}

static void test_trim_order_and_applied_acknowledgement(void)
{
	FakeTarget target;
	fake_reset(&target);
	AiusAudioClockBackend backend = fake_backend(&target);
	const AiusSpdifClockPlan original = exact_plan(48000u);

	assert(
		aius_audio_clock_apply_with_backend(
			&original, &backend) ==
		AIUS_CLOCK_APPLY_OK);
	acknowledge_hold();
	clear_trace(&target);

	AiusSpdifClockPlan trim = original;
	trim.target_rate_millihz = 48021000u;
	trim.pll_numerator = 684544u;
	assert(aius_audio_clock_request_trim(&trim));

	AiusSpdifClockPlan applied = {};
	assert(aius_audio_clock_get_applied_plan(&applied));
	assert_plan_equal(applied, original);
	AiusAudioClockBoundaryStatus status = boundary_status();
	assert(status.request_pending);
	assert(!status.invalid_active);
	assert(!status.transition_complete);
	assert(status.periods_remaining == 0u);

	aius_audio_clock_test_tx_boundary();
	assert(target.trace_count == 2u);
	assert_write(
		target,
		0u,
		AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
		UINT32_C(0x00000400) |
			AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT);
	assert_write(
		target,
		1u,
		AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR,
		trim.pll_numerator);
	assert(aius_audio_clock_get_applied_plan(&applied));
	assert_plan_equal(applied, trim);

	status = boundary_status();
	assert(!status.request_pending);
	assert(status.invalid_active);
	assert(status.periods_remaining == 4u);
	assert(!aius_audio_clock_request_trim(&original));

	for (uint32_t expected = 3u; expected != 0u; expected--) {
		aius_audio_clock_test_tx_boundary();
		status = boundary_status();
		assert(status.periods_remaining == expected);
		assert(status.invalid_active);
	}
	aius_audio_clock_test_tx_boundary();
	status = boundary_status();
	assert(status.transition_complete);
	assert(!status.invalid_active);
	assert(status.periods_remaining == 0u);
	assert(target.trace_count == 3u);
	assert(
		target.trace[2].reg ==
		AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
	assert(
		(target.trace[2].value &
		 AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT) == 0u);
}

static void test_trim_refuses_structural_or_nominal_change(void)
{
	FakeTarget target;
	fake_reset(&target);
	AiusAudioClockBackend backend = fake_backend(&target);
	const AiusSpdifClockPlan original = exact_plan(48000u);
	assert(
		aius_audio_clock_apply_with_backend(
			&original, &backend) ==
		AIUS_CLOCK_APPLY_OK);
	acknowledge_hold();

	AiusSpdifClockPlan changed = original;
	changed.pll_div_select++;
	assert(!aius_audio_clock_request_trim(&changed));
	changed = exact_plan(44100u);
	assert(!aius_audio_clock_request_trim(&changed));
}

static void test_dark_transition_is_immediate_and_cancels_old_hold(void)
{
	FakeTarget target;
	fake_reset(&target);
	g_isr_target = &target;

	aius_audio_clock_begin_dark_transition();
	assert(target.trace_count == 1u);
	assert(
		target.trace[0].reg ==
		AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
	assert(
		(target.trace[0].value &
		 AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT) != 0u);
	const AiusAudioClockBoundaryStatus status = boundary_status();
	assert(!status.request_pending);
	assert(status.invalid_active);
	assert(!status.transition_complete);
	assert(status.periods_remaining == 0u);
}

static void test_invalid_plan_writes_nothing(void)
{
	FakeTarget target;
	fake_reset(&target);
	AiusAudioClockBackend backend = fake_backend(&target);
	AiusSpdifClockPlan plan = exact_plan(48000u);
	plan.pll_denominator = 10000u;
	assert(
		aius_audio_clock_apply_with_backend(&plan, &backend) ==
		AIUS_CLOCK_APPLY_INVALID_PLAN);
	assert(target.trace_count == 0u);
	assert(!aius_audio_clock_request_trim(&plan));
}

static void test_srfm_integer_millihertz(void)
{
	FakeTarget target;
	fake_reset(&target);
	AiusAudioClockBackend backend = fake_backend(&target);
	uint32_t rate = UINT32_C(0xdeadbeef);

	target.registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_PHASE_CONFIG)] = PHASE_LOCK;
	target.registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_RATE_MEASUREMENT)] = 343748u;
	assert(aius_spdif_input_rate_millihz_with_backend(&backend, &rate));
	assert(rate == 48021041u);

	target.registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_PHASE_CONFIG)] = 0u;
	rate = UINT32_C(0xdeadbeef);
	assert(!aius_spdif_input_rate_millihz_with_backend(&backend, &rate));
	assert(rate == UINT32_C(0xdeadbeef));

	target.registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_PHASE_CONFIG)] = PHASE_LOCK;
	target.registers[register_index(
		AIUS_AUDIO_CLOCK_REG_SPDIF_RATE_MEASUREMENT)] = UINT32_MAX;
	backend.bus_frequency_hz = UINT32_MAX;
	assert(!aius_spdif_input_rate_millihz_with_backend(&backend, &rate));
}

static void test_flush_preserves_lifetime_diagnostics(void)
{
	AiusSpdifTx24Ring ring = {};
	ring.written = UINT32_MAX - 7u;
	ring.consumed = UINT32_MAX - 107u;
	ring.underruns = 9123u;
	assert(aius_spdif_tx24_level(&ring) == 100u);

	aius_spdif_tx24_flush_preserve_diag(&ring);
	assert(ring.written == UINT32_MAX - 7u);
	assert(ring.consumed == ring.written);
	assert(ring.underruns == 9123u);
	assert(aius_spdif_tx24_level(&ring) == 0u);

	aius_spdif_tx24_reset(&ring);
	assert(ring.written == 0u);
	assert(ring.consumed == 0u);
	assert(ring.underruns == 0u);
}

} /* namespace */

uint32_t aius_audio_clock_fake_register_read(
	AiusAudioClockRegister reg)
{
	assert(g_isr_target != nullptr);
	return fake_read(g_isr_target, reg);
}

void aius_audio_clock_fake_register_write(
	AiusAudioClockRegister reg,
	uint32_t value)
{
	assert(g_isr_target != nullptr);
	fake_write(g_isr_target, reg, value);
}

int main(void)
{
	test_immediate_lock_and_register_order();
	test_last_cycle_lock();
	test_cycle_wrap_is_bounded();
	test_never_lock_latches_failure();
	test_all_nominal_mappings_and_four_period_hold();
	test_trim_order_and_applied_acknowledgement();
	test_trim_refuses_structural_or_nominal_change();
	test_dark_transition_is_immediate_and_cancels_old_hold();
	test_invalid_plan_writes_nothing();
	test_srfm_integer_millihertz();
	test_flush_preserves_lifetime_diagnostics();
	puts("clock-target tests passed");
	return 0;
}
