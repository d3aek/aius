#include <aius_audio_clock.h>

#include <limits.h>
#include <stddef.h>

#include "aius_audio_clock_internal.h"

#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
#include <Arduino.h>
#include <utility/imxrt_hw.h>
#elif !defined(FLASHMEM)
#define FLASHMEM
#endif

namespace {

constexpr uint32_t PLL_BYPASS = UINT32_C(1) << 16;
constexpr uint32_t PLL_ENABLE = UINT32_C(1) << 13;
constexpr uint32_t PLL_POWERDOWN = UINT32_C(1) << 12;
constexpr uint32_t PLL_POST_DIVIDE_4 = UINT32_C(2) << 19;
constexpr uint32_t PLL_DIV_SELECT_MASK = UINT32_C(0x3f);
constexpr uint32_t PLL_FRACTION_MASK = UINT32_C(0x1fffffff);
constexpr uint32_t PLL_MISC_DIV_MASK =
	(UINT32_C(1) << 23) | (UINT32_C(1) << 15);
constexpr uint32_t SPDIF_ROOT_PRED_MASK = UINT32_C(7) << 25;
constexpr uint32_t SPDIF_ROOT_PODF_MASK = UINT32_C(7) << 22;
constexpr uint32_t SPDIF_ROOT_SELECT_MASK = UINT32_C(3) << 20;
constexpr uint32_t SPDIF_TX_CLOCK_SOURCE_MASK = UINT32_C(7) << 8;
constexpr uint32_t SPDIF_TX_CLOCK_SOURCE_ROOT = UINT32_C(1) << 8;
constexpr uint32_t SPDIF_TX_CLOCK_DIVIDER_MASK = UINT32_C(0x7f);
constexpr uint32_t SPDIF_CHANNEL_STATUS_RATE_MASK = UINT32_C(3) << 22;
constexpr uint32_t SPDIF_PHASE_LOCK_BIT = UINT32_C(1) << 6;

#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
static_assert(
	CCM_ANALOG_PLL_AUDIO_LOCK == AIUS_AUDIO_CLOCK_PLL_LOCK_BIT,
	"fake-target PLL lock bit drifted from the Teensy core");
static_assert(
	SPDIF_SCR_VALCTRL == AIUS_AUDIO_CLOCK_VALCTRL_VALID_BIT,
	"fake-target validity bit drifted from the Teensy core");
static_assert(
	CCM_ANALOG_PLL_AUDIO_BYPASS == PLL_BYPASS &&
	CCM_ANALOG_PLL_AUDIO_ENABLE == PLL_ENABLE &&
	CCM_ANALOG_PLL_AUDIO_POWERDOWN == PLL_POWERDOWN,
	"PLL control encoding drifted from the Teensy core");
static_assert(
	CCM_ANALOG_PLL_AUDIO_POST_DIV_SELECT(2) == PLL_POST_DIVIDE_4 &&
	CCM_ANALOG_PLL_AUDIO_DIV_SELECT(0x3f) == PLL_DIV_SELECT_MASK,
	"PLL divider encoding drifted from the Teensy core");
static_assert(
	CCM_ANALOG_PLL_AUDIO_NUM_MASK == PLL_FRACTION_MASK &&
	CCM_ANALOG_PLL_AUDIO_DENOM_MASK == PLL_FRACTION_MASK,
	"PLL fraction mask drifted from the Teensy core");
static_assert(
	(CCM_ANALOG_MISC2_DIV_MSB | CCM_ANALOG_MISC2_DIV_LSB) ==
		PLL_MISC_DIV_MASK,
	"PLL post-divider encoding drifted from the Teensy core");
static_assert(
	(CCM_CDCDR_SPDIF0_CLK_PRED_MASK |
	 CCM_CDCDR_SPDIF0_CLK_PODF_MASK |
	 CCM_CDCDR_SPDIF0_CLK_SEL_MASK) ==
		(SPDIF_ROOT_PRED_MASK |
		 SPDIF_ROOT_PODF_MASK |
		 SPDIF_ROOT_SELECT_MASK),
	"S/PDIF root encoding drifted from the Teensy core");
static_assert(
	SPDIF_STC_TXCLK_SOURCE(7) == SPDIF_TX_CLOCK_SOURCE_MASK &&
	SPDIF_STC_TXCLK_SOURCE(1) == SPDIF_TX_CLOCK_SOURCE_ROOT &&
	SPDIF_STC_TXCLK_DF(0x7f) == SPDIF_TX_CLOCK_DIVIDER_MASK,
	"S/PDIF TX clock encoding drifted from the Teensy core");
static_assert(
	SPDIF_SRPC_LOCK == SPDIF_PHASE_LOCK_BIT,
	"S/PDIF DPLL lock encoding drifted from the Teensy core");
#endif

static FLASHMEM void memory_barrier(void)
{
	__asm__ volatile("" ::: "memory");
}

static FLASHMEM uint32_t enter_critical(void)
{
#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
	uint32_t saved;
	__asm__ volatile("MRS %0, PRIMASK" : "=r"(saved) :: "memory");
	__disable_irq();
	return saved;
#else
	return 0u;
#endif
}

static FLASHMEM void leave_critical(uint32_t saved)
{
#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
	if (saved == 0u)
		__enable_irq();
#else
	(void)saved;
#endif
}

static FLASHMEM bool plan_is_valid(const AiusSpdifClockPlan *plan)
{
	if (plan == NULL ||
	    plan->pll_div_select < 27u ||
	    plan->pll_div_select > 54u ||
	    plan->pll_numerator >= plan->pll_denominator ||
	    plan->pll_denominator != AIUS_CLOCK_PLL_DENOMINATOR ||
	    plan->pll_numerator > PLL_FRACTION_MASK ||
	    plan->pll_denominator > PLL_FRACTION_MASK ||
	    plan->root_predivider != 8u ||
	    plan->root_postdivider != 1u ||
	    plan->tx_clock_divider == 0u ||
	    plan->tx_clock_divider > 128u ||
	    (plan->channel_status_rate_bits &
	     ~SPDIF_CHANNEL_STATUS_RATE_MASK) != 0u)
		return false;

	switch (plan->nominal_rate_hz) {
	case 32000u:
		return plan->tx_clock_divider == 42u &&
		       plan->channel_status_rate_bits ==
		       AIUS_CLOCK_STCSCL_RATE_32000;
	case 44100u:
		return plan->tx_clock_divider == 30u &&
		       plan->channel_status_rate_bits ==
		       AIUS_CLOCK_STCSCL_RATE_44100;
	case 48000u:
		return plan->tx_clock_divider == 28u &&
		       plan->channel_status_rate_bits ==
		       AIUS_CLOCK_STCSCL_RATE_48000;
	default:
		return false;
	}
}

static FLASHMEM bool backend_is_valid(const AiusAudioClockBackend *backend)
{
	return backend != NULL &&
	       backend->read_register != NULL &&
	       backend->write_register != NULL &&
	       backend->read_cycle_count != NULL &&
	       backend->cycles_per_microsecond != 0u &&
	       backend->cycles_per_microsecond <=
		       UINT32_MAX / AIUS_AUDIO_CLOCK_LOCK_TIMEOUT_US;
}

static FLASHMEM uint32_t backend_read(
	const AiusAudioClockBackend *backend,
	AiusAudioClockRegister reg)
{
	return backend->read_register(backend->context, reg);
}

static FLASHMEM void backend_write(
	const AiusAudioClockBackend *backend,
	AiusAudioClockRegister reg,
	uint32_t value)
{
	backend->write_register(backend->context, reg, value);
}

static FLASHMEM void copy_plan_to_volatile(
	volatile AiusSpdifClockPlan *destination,
	const AiusSpdifClockPlan *source)
{
	destination->nominal_rate_hz = source->nominal_rate_hz;
	destination->target_rate_millihz = source->target_rate_millihz;
	destination->pll_div_select = source->pll_div_select;
	destination->pll_numerator = source->pll_numerator;
	destination->pll_denominator = source->pll_denominator;
	destination->root_predivider = source->root_predivider;
	destination->root_postdivider = source->root_postdivider;
	destination->tx_clock_divider = source->tx_clock_divider;
	destination->channel_status_rate_bits =
		source->channel_status_rate_bits;
}

static FLASHMEM void copy_plan_from_volatile(
	AiusSpdifClockPlan *destination,
	const volatile AiusSpdifClockPlan *source)
{
	destination->nominal_rate_hz = source->nominal_rate_hz;
	destination->target_rate_millihz = source->target_rate_millihz;
	destination->pll_div_select = source->pll_div_select;
	destination->pll_numerator = source->pll_numerator;
	destination->pll_denominator = source->pll_denominator;
	destination->root_predivider = source->root_predivider;
	destination->root_postdivider = source->root_postdivider;
	destination->tx_clock_divider = source->tx_clock_divider;
	destination->channel_status_rate_bits =
		source->channel_status_rate_bits;
}

static FLASHMEM bool plans_structurally_equal(
	const AiusSpdifClockPlan *left,
	const AiusSpdifClockPlan *right)
{
	return left->pll_div_select == right->pll_div_select &&
	       left->pll_denominator == right->pll_denominator &&
	       left->root_predivider == right->root_predivider &&
	       left->root_postdivider == right->root_postdivider &&
	       left->tx_clock_divider == right->tx_clock_divider;
}

static FLASHMEM void begin_state_update(void)
{
	g_aius_audio_clock_state.boundary_sequence++;
	memory_barrier();
}

static FLASHMEM void end_state_update(void)
{
	memory_barrier();
	g_aius_audio_clock_state.boundary_sequence++;
}

static FLASHMEM void clear_applied_plan(void)
{
	g_aius_audio_clock_state.applied_sequence++;
	memory_barrier();
	g_aius_audio_clock_state.has_applied_plan = 0u;
	memory_barrier();
	g_aius_audio_clock_state.applied_sequence++;
}

static FLASHMEM void publish_applied_plan(
	const AiusSpdifClockPlan *plan)
{
	g_aius_audio_clock_state.applied_sequence++;
	memory_barrier();
	copy_plan_to_volatile(&g_aius_audio_clock_state.applied_plan, plan);
	g_aius_audio_clock_state.has_applied_plan = 1u;
	memory_barrier();
	g_aius_audio_clock_state.applied_sequence++;
}

static FLASHMEM void latch_dark_transition(void)
{
	const uint32_t saved = enter_critical();
	begin_state_update();
	g_aius_audio_clock_state.request = AIUS_AUDIO_CLOCK_BOUNDARY_NONE;
	g_aius_audio_clock_state.invalid_periods_remaining = 0u;
	g_aius_audio_clock_state.invalid_active = 1u;
	end_state_update();
	leave_critical(saved);
}

static FLASHMEM void queue_hold_request(void)
{
	const uint32_t saved = enter_critical();
	begin_state_update();
	g_aius_audio_clock_state.request = AIUS_AUDIO_CLOCK_BOUNDARY_HOLD;
	g_aius_audio_clock_state.invalid_periods_remaining = 0u;
	g_aius_audio_clock_state.invalid_active = 1u;
	end_state_update();
	leave_critical(saved);
}

static FLASHMEM void begin_dark_transition_with_backend(
	const AiusAudioClockBackend *backend)
{
	latch_dark_transition();
	const uint32_t control = backend_read(
		backend, AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
	/* CLEARING ValCtrl is what marks the outgoing stream invalid (V=1); the
	 * bit's polarity and the inherited inversion this line used to carry are
	 * at AIUS_AUDIO_CLOCK_VALCTRL_VALID_BIT. */
	backend_write(
		backend,
		AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
		control & ~AIUS_AUDIO_CLOCK_VALCTRL_VALID_BIT);
}

#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
static FLASHMEM uint32_t target_register_read(
	void *,
	AiusAudioClockRegister reg)
{
	switch (reg) {
	case AIUS_AUDIO_CLOCK_REG_PLL_CONTROL:
		return CCM_ANALOG_PLL_AUDIO;
	case AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR:
		return CCM_ANALOG_PLL_AUDIO_NUM;
	case AIUS_AUDIO_CLOCK_REG_PLL_DENOMINATOR:
		return CCM_ANALOG_PLL_AUDIO_DENOM;
	case AIUS_AUDIO_CLOCK_REG_PLL_MISC2:
		return CCM_ANALOG_MISC2;
	case AIUS_AUDIO_CLOCK_REG_CLOCK_GATE_5:
		return CCM_CCGR5;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT:
		return CCM_CDCDR;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL:
		return SPDIF_SCR;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK:
		return SPDIF_STC;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW:
		return SPDIF_STCSCL;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_PHASE_CONFIG:
		return SPDIF_SRPC;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_RATE_MEASUREMENT:
		return SPDIF_SRFM;
	}
	return 0u;
}

static FLASHMEM void target_register_write(
	void *,
	AiusAudioClockRegister reg,
	uint32_t value)
{
	switch (reg) {
	case AIUS_AUDIO_CLOCK_REG_PLL_CONTROL:
		CCM_ANALOG_PLL_AUDIO = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR:
		CCM_ANALOG_PLL_AUDIO_NUM = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_PLL_DENOMINATOR:
		CCM_ANALOG_PLL_AUDIO_DENOM = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_PLL_MISC2:
		CCM_ANALOG_MISC2 = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_CLOCK_GATE_5:
		CCM_CCGR5 = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT:
		CCM_CDCDR = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL:
		SPDIF_SCR = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK:
		SPDIF_STC = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW:
		SPDIF_STCSCL = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_PHASE_CONFIG:
		SPDIF_SRPC = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_RATE_MEASUREMENT:
		SPDIF_SRFM = value;
		break;
	}
}

static FLASHMEM uint32_t target_cycle_count(void *)
{
	return ARM_DWT_CYCCNT;
}

static FLASHMEM AiusAudioClockBackend target_backend(void)
{
	AiusAudioClockBackend backend = {};
	backend.read_register = target_register_read;
	backend.write_register = target_register_write;
	backend.read_cycle_count = target_cycle_count;
	backend.cycles_per_microsecond =
		static_cast<uint32_t>(F_CPU_ACTUAL / UINT32_C(1000000));
	backend.bus_frequency_hz = F_BUS_ACTUAL;
	return backend;
}
#elif defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
static FLASHMEM uint32_t fake_target_read(
	void *,
	AiusAudioClockRegister reg)
{
	return aius_audio_clock_fake_register_read(reg);
}

static FLASHMEM void fake_target_write(
	void *,
	AiusAudioClockRegister reg,
	uint32_t value)
{
	aius_audio_clock_fake_register_write(reg, value);
}

static FLASHMEM uint32_t fake_target_cycle(void *)
{
	/* The production wrappers are not used for bounded-lock tests. This keeps
	 * begin_dark_transition() available to the fake target without inventing a
	 * second public installation API. */
	static uint32_t cycle;
	return cycle++;
}

static FLASHMEM AiusAudioClockBackend target_backend(void)
{
	AiusAudioClockBackend backend = {};
	backend.read_register = fake_target_read;
	backend.write_register = fake_target_write;
	backend.read_cycle_count = fake_target_cycle;
	backend.cycles_per_microsecond = 1u;
	backend.bus_frequency_hz = UINT32_C(150000000);
	return backend;
}
#endif

} /* namespace */

extern "C" {

FLASHMEM void aius_audio_clock_initialize(void)
{
	const uint32_t saved = enter_critical();
	g_aius_audio_clock_state.boundary_sequence = 0u;
	g_aius_audio_clock_state.request = AIUS_AUDIO_CLOCK_BOUNDARY_NONE;
	g_aius_audio_clock_state.invalid_periods_remaining = 0u;
	g_aius_audio_clock_state.invalid_active = 0u;
	g_aius_audio_clock_state.applied_sequence = 0u;
	g_aius_audio_clock_state.has_applied_plan = 0u;
	memory_barrier();
	leave_critical(saved);
}

FLASHMEM void aius_audio_clock_begin_dark_transition(void)
{
#if defined(__IMXRT1052__) || defined(__IMXRT1062__) || \
	defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
	AiusAudioClockBackend backend = target_backend();
	begin_dark_transition_with_backend(&backend);
#else
	latch_dark_transition();
#endif
}

FLASHMEM AiusClockApplyResult aius_audio_clock_apply_with_backend(
	const AiusSpdifClockPlan *plan,
	const AiusAudioClockBackend *backend)
{
	uint32_t pll_control;
	uint32_t start_cycles;
	uint32_t timeout_cycles;
	uint32_t root;
	uint32_t tx_clock;
	uint32_t channel_status;

	if (!plan_is_valid(plan) || !backend_is_valid(backend))
		return AIUS_CLOCK_APPLY_INVALID_PLAN;

	begin_dark_transition_with_backend(backend);
	clear_applied_plan();

	pll_control = PLL_BYPASS |
	              PLL_ENABLE |
	              PLL_POWERDOWN |
	              PLL_POST_DIVIDE_4 |
	              (plan->pll_div_select & PLL_DIV_SELECT_MASK);
	backend_write(
		backend, AIUS_AUDIO_CLOCK_REG_PLL_CONTROL, pll_control);
	backend_write(
		backend,
		AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR,
		plan->pll_numerator & PLL_FRACTION_MASK);
	backend_write(
		backend,
		AIUS_AUDIO_CLOCK_REG_PLL_DENOMINATOR,
		plan->pll_denominator & PLL_FRACTION_MASK);
	pll_control &= ~PLL_POWERDOWN;
	backend_write(
		backend, AIUS_AUDIO_CLOCK_REG_PLL_CONTROL, pll_control);

	timeout_cycles = backend->cycles_per_microsecond *
	                 AIUS_AUDIO_CLOCK_LOCK_TIMEOUT_US;
	start_cycles = backend->read_cycle_count(backend->context);
	for (;;) {
		if ((uint32_t)(
			    backend->read_cycle_count(backend->context) -
			    start_cycles) > timeout_cycles)
			return AIUS_CLOCK_APPLY_LOCK_TIMEOUT;
		if ((backend_read(
			     backend, AIUS_AUDIO_CLOCK_REG_PLL_CONTROL) &
		     AIUS_AUDIO_CLOCK_PLL_LOCK_BIT) != 0u)
			break;
	}

	backend_write(
		backend,
		AIUS_AUDIO_CLOCK_REG_PLL_MISC2,
		backend_read(
			backend, AIUS_AUDIO_CLOCK_REG_PLL_MISC2) &
			~PLL_MISC_DIV_MASK);
	backend_write(
		backend,
		AIUS_AUDIO_CLOCK_REG_PLL_CONTROL,
		pll_control & ~PLL_BYPASS);

	/* Do not gate CCM_CCGR5_SPDIF here. That gate stops the receiver and its
	 * live DMA as well as the transmitter. Boot-time config_spdif3() may gate
	 * it before RX DMA is enabled; a runtime output replan must instead keep
	 * RX continuous while the physical TX pad and IEC validity are dark. */
	root = backend_read(backend, AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT);
	root &= ~(SPDIF_ROOT_PRED_MASK |
	          SPDIF_ROOT_PODF_MASK |
	          SPDIF_ROOT_SELECT_MASK);
	root |= (static_cast<uint32_t>(plan->root_predivider - 1u) << 25) |
	        (static_cast<uint32_t>(plan->root_postdivider - 1u) << 22);
	backend_write(
		backend, AIUS_AUDIO_CLOCK_REG_SPDIF_ROOT, root);

	tx_clock = backend_read(
		backend, AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK);
	tx_clock &= ~(SPDIF_TX_CLOCK_SOURCE_MASK |
	              SPDIF_TX_CLOCK_DIVIDER_MASK);
	tx_clock |= SPDIF_TX_CLOCK_SOURCE_ROOT |
	            static_cast<uint32_t>(plan->tx_clock_divider - 1u);
	backend_write(
		backend, AIUS_AUDIO_CLOCK_REG_SPDIF_TX_CLOCK, tx_clock);

	channel_status = backend_read(
		backend,
		AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW);
	channel_status &= ~SPDIF_CHANNEL_STATUS_RATE_MASK;
	channel_status |= plan->channel_status_rate_bits;
	backend_write(
		backend,
		AIUS_AUDIO_CLOCK_REG_SPDIF_CHANNEL_STATUS_LOW,
		channel_status);

	publish_applied_plan(plan);
	queue_hold_request();
	return AIUS_CLOCK_APPLY_OK;
}

FLASHMEM AiusClockApplyResult aius_audio_clock_apply(
	const AiusSpdifClockPlan *plan)
{
#if defined(__IMXRT1052__) || defined(__IMXRT1062__) || \
	defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
	AiusAudioClockBackend backend = target_backend();
	return aius_audio_clock_apply_with_backend(plan, &backend);
#else
	(void)plan;
	return AIUS_CLOCK_APPLY_INVALID_PLAN;
#endif
}

FLASHMEM bool aius_audio_clock_get_applied_plan(
	AiusSpdifClockPlan *plan)
{
	uint32_t before;
	uint32_t after;
	uint32_t present;

	if (plan == NULL)
		return false;
	for (;;) {
		before = g_aius_audio_clock_state.applied_sequence;
		if ((before & 1u) != 0u)
			continue;
		memory_barrier();
		present = g_aius_audio_clock_state.has_applied_plan;
		copy_plan_from_volatile(
			plan, &g_aius_audio_clock_state.applied_plan);
		memory_barrier();
		after = g_aius_audio_clock_state.applied_sequence;
		if (before == after && (after & 1u) == 0u)
			break;
	}
	return present != 0u;
}

FLASHMEM bool aius_audio_clock_request_trim(
	const AiusSpdifClockPlan *plan)
{
	AiusSpdifClockPlan applied;
	uint32_t saved;

	if (!plan_is_valid(plan) ||
	    !aius_audio_clock_get_applied_plan(&applied) ||
	    plan->nominal_rate_hz != applied.nominal_rate_hz ||
	    plan->channel_status_rate_bits !=
		    applied.channel_status_rate_bits ||
	    !plans_structurally_equal(plan, &applied))
		return false;

	saved = enter_critical();
	begin_state_update();
	if (g_aius_audio_clock_state.request !=
		    AIUS_AUDIO_CLOCK_BOUNDARY_NONE ||
	    g_aius_audio_clock_state.invalid_periods_remaining != 0u ||
	    g_aius_audio_clock_state.invalid_active != 0u) {
		end_state_update();
		leave_critical(saved);
		return false;
	}
	copy_plan_to_volatile(
		&g_aius_audio_clock_state.queued_plan, plan);
	memory_barrier();
	g_aius_audio_clock_state.request =
		AIUS_AUDIO_CLOCK_BOUNDARY_TRIM;
	end_state_update();
	leave_critical(saved);
	return true;
}

FLASHMEM bool aius_audio_clock_get_boundary_status(
	AiusAudioClockBoundaryStatus *status)
{
	uint32_t before;
	uint32_t after;
	uint32_t request;
	uint32_t invalid;
	uint32_t periods;

	if (status == NULL)
		return false;
	for (;;) {
		before = g_aius_audio_clock_state.boundary_sequence;
		if ((before & 1u) != 0u)
			continue;
		memory_barrier();
		request = g_aius_audio_clock_state.request;
		invalid = g_aius_audio_clock_state.invalid_active;
		periods =
			g_aius_audio_clock_state.invalid_periods_remaining;
		memory_barrier();
		after = g_aius_audio_clock_state.boundary_sequence;
		if (before == after && (after & 1u) == 0u)
			break;
	}

	status->request_pending =
		request != AIUS_AUDIO_CLOCK_BOUNDARY_NONE;
	status->invalid_active = invalid != 0u;
	status->periods_remaining = periods;
	status->transition_complete =
		request == AIUS_AUDIO_CLOCK_BOUNDARY_NONE &&
		invalid == 0u &&
		periods == 0u;
	return true;
}

FLASHMEM bool aius_spdif_input_rate_millihz_with_backend(
	const AiusAudioClockBackend *backend,
	uint32_t *rate_millihz)
{
	uint32_t measurement;
	uint64_t scaled;
	const uint64_t denominator =
		(UINT64_C(1) << 27) * AIUS_AUDIO_CLOCK_DPLL_GAIN;
	uint64_t rounded;

	if (backend == NULL ||
	    backend->read_register == NULL ||
	    rate_millihz == NULL ||
	    backend->bus_frequency_hz == 0u ||
	    (backend_read(
		     backend, AIUS_AUDIO_CLOCK_REG_SPDIF_PHASE_CONFIG) &
	     SPDIF_PHASE_LOCK_BIT) == 0u)
		return false;

	measurement = backend_read(
		backend,
		AIUS_AUDIO_CLOCK_REG_SPDIF_RATE_MEASUREMENT);
	if (measurement != 0u &&
	    backend->bus_frequency_hz >
		    UINT64_MAX / measurement / UINT64_C(1000))
		return false;
	scaled = static_cast<uint64_t>(backend->bus_frequency_hz) *
	         measurement * UINT64_C(1000);
	rounded = (scaled + denominator / 2u) / denominator;
	if (rounded > UINT32_MAX)
		return false;
	*rate_millihz = static_cast<uint32_t>(rounded);
	return true;
}

FLASHMEM bool aius_spdif_input_rate_millihz(
	uint32_t *rate_millihz)
{
#if defined(__IMXRT1052__) || defined(__IMXRT1062__) || \
	defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
	AiusAudioClockBackend backend = target_backend();
	return aius_spdif_input_rate_millihz_with_backend(
		&backend, rate_millihz);
#else
	(void)rate_millihz;
	return false;
#endif
}

} /* extern "C" */

#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
/* Retained PJRC boot helper. Runtime target changes use the bounded adapter
 * above; this helper remains only for config_spdif3()'s boot-time setup. */
FLASHMEM void set_audioClock(
	int nfact,
	int32_t nmult,
	uint32_t ndiv,
	bool force)
{
	if (!force &&
	    (CCM_ANALOG_PLL_AUDIO & CCM_ANALOG_PLL_AUDIO_ENABLE) != 0u)
		return;
	CCM_ANALOG_PLL_AUDIO =
		CCM_ANALOG_PLL_AUDIO_BYPASS |
		CCM_ANALOG_PLL_AUDIO_ENABLE |
		CCM_ANALOG_PLL_AUDIO_POST_DIV_SELECT(2) |
		CCM_ANALOG_PLL_AUDIO_DIV_SELECT(nfact);
	CCM_ANALOG_PLL_AUDIO_NUM =
		static_cast<uint32_t>(nmult) &
		CCM_ANALOG_PLL_AUDIO_NUM_MASK;
	CCM_ANALOG_PLL_AUDIO_DENOM =
		ndiv & CCM_ANALOG_PLL_AUDIO_DENOM_MASK;
	CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_POWERDOWN;
	while ((CCM_ANALOG_PLL_AUDIO &
	        CCM_ANALOG_PLL_AUDIO_LOCK) == 0u) {}
	CCM_ANALOG_MISC2 &=
		~(CCM_ANALOG_MISC2_DIV_MSB |
		  CCM_ANALOG_MISC2_DIV_LSB);
	CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_BYPASS;
}
#endif
