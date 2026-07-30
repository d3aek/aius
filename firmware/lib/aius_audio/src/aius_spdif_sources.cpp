/* Build only the PJRC Teensy 4 S/PDIF source units required by Aius.
 * The retained source snapshots are the upstream implementation boundary. */
#include "aius_audio_clock_internal.h"

#if !defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
#include "../../../../evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE/output_spdif3.cpp"
#include "../../../../evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp"
#endif

/* ...and, BELOW those includes and in the same translation unit, the 24-bit
 * transmit path that replaces the snapshot's 16-bit one without editing it.
 *
 * THE INCLUDE ABOVE IS THE SEAM. `SPDIF_tx_buffer` is a file static of
 * output_spdif3.cpp (line 44), so it is addressable here and nowhere else in
 * the image; every other piece of transmit state the filler needs -- `dma`,
 * `update_responsibility`, `AudioStream::update_all()` -- is `protected static`
 * and reached through a private-inheritance shim, exactly as
 * AiusSpdifOutputClock reaches `update_responsibility` in optical_main.cpp.
 * Nothing in evidence/source-snapshots/ changes. The full rationale, the memory
 * decision and the concurrency contract are in aius_spdif_tx24.h. */
#if !defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
#include "aius_spdif_tx24.h"
#endif

#if defined(__IMXRT1062__) || defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)

volatile AiusAudioClockSharedState g_aius_audio_clock_state;

#if !defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
/* DMAMEM, not DTCM: 16 KiB is the one large buffer in this path and OCRAM is
 * where large buffers live.
 *
 * THE CURSORS ARE NOT ZERO AT RESET, and this comment used to claim they were --
 * "uninitialised at reset, which is correct, both cursors start at zero". Wrong
 * twice: `.bss.dma` is NOLOAD so startup clears DTCM and not OCRAM, and the boot
 * ROM actively writes scratch into three fixed OCRAM runs, one of which contains
 * the top of this structure. `aius_spdif_tx24_begin()` now resets them
 * explicitly; see aius_spdif_tx24_reset() for the measured ranges and the
 * audible consequence.
 *
 * DMAMEM here does NOT imply cache maintenance: no DMA reads this buffer. The
 * producer and the consumer are both CPU accesses through the same write-back
 * D-cache, so they are coherent. SPDIF_tx_buffer is the buffer the DMA reads,
 * and that one is flushed below. */
DMAMEM AiusSpdifTx24Ring g_aius_spdif_tx24;
#endif

namespace {

#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
static inline uint32_t clock_register_read(AiusAudioClockRegister reg)
{
	return aius_audio_clock_fake_register_read(reg);
}

static inline void clock_register_write(
	AiusAudioClockRegister reg,
	uint32_t value)
{
	aius_audio_clock_fake_register_write(reg, value);
}
#else
static inline uint32_t clock_register_read(AiusAudioClockRegister reg)
{
	switch (reg) {
	case AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR:
		return CCM_ANALOG_PLL_AUDIO_NUM;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL:
		return SPDIF_SCR;
	default:
		return 0u;
	}
}

static inline void clock_register_write(
	AiusAudioClockRegister reg,
	uint32_t value)
{
	switch (reg) {
	case AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR:
		CCM_ANALOG_PLL_AUDIO_NUM = value;
		break;
	case AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL:
		SPDIF_SCR = value;
		break;
	default:
		break;
	}
}
#endif

static inline void clock_memory_barrier(void)
{
	__asm__ volatile("" ::: "memory");
}

/* The exact TX-period boundary contract. This remains in the mixed source
 * object and deliberately stays hot:
 *
 *  1. acknowledge a period completed by an existing hold;
 *  2. consume a newly queued request;
 *  3. make validity invalid before a queued NUM write; and
 *  4. arm four periods without decrementing at that same boundary.
 *
 * There are no checkpoint helpers in this translation unit. */
static inline void aius_audio_clock_tx_boundary(void)
{
	if (g_aius_audio_clock_state.invalid_periods_remaining == 0u &&
	    g_aius_audio_clock_state.request ==
		    AIUS_AUDIO_CLOCK_BOUNDARY_NONE)
		return;

	g_aius_audio_clock_state.boundary_sequence++;
	clock_memory_barrier();

	if (g_aius_audio_clock_state.invalid_periods_remaining != 0u) {
		g_aius_audio_clock_state.invalid_periods_remaining--;
		if (g_aius_audio_clock_state.invalid_periods_remaining == 0u) {
			const uint32_t control = clock_register_read(
				AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
			clock_register_write(
				AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
				control &
					~AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT);
			g_aius_audio_clock_state.invalid_active = 0u;
		}
	}

	const uint32_t request = g_aius_audio_clock_state.request;
	if (request != AIUS_AUDIO_CLOCK_BOUNDARY_NONE) {
		const uint32_t control = clock_register_read(
			AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
		clock_register_write(
			AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
			control | AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT);
		g_aius_audio_clock_state.invalid_active = 1u;

		if (request == AIUS_AUDIO_CLOCK_BOUNDARY_TRIM) {
			/* Validity is already invalid. Only then may NUM change. */
			clock_register_write(
				AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR,
				g_aius_audio_clock_state
					.queued_plan.pll_numerator);

			g_aius_audio_clock_state.applied_sequence++;
			clock_memory_barrier();
			g_aius_audio_clock_state
				.applied_plan.target_rate_millihz =
				g_aius_audio_clock_state
					.queued_plan.target_rate_millihz;
			g_aius_audio_clock_state
				.applied_plan.pll_numerator =
				g_aius_audio_clock_state
					.queued_plan.pll_numerator;
			g_aius_audio_clock_state.has_applied_plan = 1u;
			clock_memory_barrier();
			g_aius_audio_clock_state.applied_sequence++;
		}

		g_aius_audio_clock_state.request =
			AIUS_AUDIO_CLOCK_BOUNDARY_NONE;
		g_aius_audio_clock_state.invalid_periods_remaining =
			AIUS_AUDIO_CLOCK_INVALID_PERIODS;
	}

	clock_memory_barrier();
	g_aius_audio_clock_state.boundary_sequence++;
}

#if !defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
/* An ACCESS SHIM, never an audio node: private inheritance and a deleted
 * constructor, because constructing one would run AudioOutputSPDIF3::begin() a
 * second time and re-run config_spdif3(). [class.protected]'s "through an
 * object of the derived type" restriction applies to non-static members only,
 * so a derived CLASS is what reaches these. */
class AiusSpdifTx24Hook : private AudioOutputSPDIF3 {
public:
	AiusSpdifTx24Hook(void) = delete;
	static void install(void);
	static void isr(void);
};

/* THE TRANSMIT DMA INTERRUPT, once per AUDIO_BLOCK_SAMPLES frames (375 Hz at
 * 48 kHz), replacing AudioOutputSPDIF3::isr(). It is shorter than the ISR it
 * replaces: no audio_block_t to dereference, none to release, no two-deep queue
 * to rotate, and no per-sample shift.
 *
 * Deliberately NOT FLASHMEM: this is the definition of code that runs per audio
 * block, so ITCM is where it belongs (§Deciding principle). It sits in
 * lib/aius_audio/src/aius_spdif_sources.cpp.o, which is neither a declared cold
 * object nor the pinned hot object, so test/itcm-placement/verify_target.py
 * does not police it -- and the audio-block set of optical_main.cpp stays
 * exactly the three sections it pins. */
void AiusSpdifTx24Hook::isr(void)
{
	/* This is the period boundary, before any work on the inactive half. */
	aius_audio_clock_tx_boundary();

	/* Which half to fill: the one the DMA is NOT reading. Identical test to
	 * the snapshot's (output_spdif3.cpp:110), against the same buffer. */
	const uint32_t saddr = (uint32_t)(dma.TCD->SADDR);
	dma.clearInterrupt();
	int32_t *const dest =
		(saddr < (uint32_t)SPDIF_tx_buffer + sizeof(SPDIF_tx_buffer) / 2)
			? SPDIF_tx_buffer + AUDIO_BLOCK_SAMPLES * 2
			: SPDIF_tx_buffer;

	aius_spdif_tx24_fill(&g_aius_spdif_tx24, dest, AUDIO_BLOCK_SAMPLES);

	/* One flush of the whole half, rather than the snapshot's flush-every-32-
	 * bytes-as-you-go. Same number of DCCMVAC operations, one call instead of
	 * 32, and provably early enough: this half is the one the DMA is not
	 * reading, so it has a full AUDIO_BLOCK_SAMPLES period (2.67 ms) before
	 * the transfer reaches it. The range is 32-byte aligned and a multiple of
	 * 32 bytes, because SPDIF_tx_buffer is aligned(32) and each half is
	 * AUDIO_BLOCK_SAMPLES * 2 * 4 = 1,024 bytes. */
#if IMXRT_CACHE_ENABLED >= 2
	arm_dcache_flush(dest, AUDIO_BLOCK_SAMPLES * 2 * sizeof *dest);
#endif

	/* Unchanged responsibility contract: this peripheral drives the graph only
	 * while replay_clock_service() has handed it the clock. update_all() only
	 * pends the ingress producer; decode, DSP and the ring producer run later
	 * from the AUDIO checkpoint. Both can be preempted by this handler, which is
	 * why the ring is a single-producer/single-consumer structure rather than a
	 * plain count. */
	if (update_responsibility)
		update_all();
}

FLASHMEM void AiusSpdifTx24Hook::install(void)
{
	/* attachInterrupt() only stores the vector and enables the IRQ, so this
	 * swaps handlers in one 32-bit store. Under a critical section anyway: an
	 * interrupt taken mid-swap would run one handler or the other, both of
	 * which are individually correct, but the pair of stores inside
	 * attachInterrupt() should not be observed split. */
	/* Cursors first, and INSIDE the critical section with the handler swap: the
	 * ring must read as empty before this ISR can run even once, because
	 * `.bss.dma` is NOLOAD and the boot ROM leaves scratch in this exact range
	 * (see aius_spdif_tx24_reset). Resetting after the attach would leave a
	 * window of one interrupt period in which fill() runs against ROM values. */
	__disable_irq();
	aius_spdif_tx24_reset(&g_aius_spdif_tx24);
	dma.attachInterrupt(AiusSpdifTx24Hook::isr);
	__enable_irq();
}
#endif

} /* namespace */

#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
void aius_audio_clock_test_tx_boundary(void)
{
	aius_audio_clock_tx_boundary();
}
#else
FLASHMEM void aius_spdif_tx24_begin(void)
{
	AiusSpdifTx24Hook::install();
	/* The snapshot never initializes the consumer channel-status registers,
	 * leaving their reset-zero message at 44.1 kHz with copyright asserted
	 * while config_spdif3() generates a 48 kHz carrier. Permissive DACs ignore
	 * that contradiction; the CM6206 rejects it and substitutes silence.
	 *
	 * i.MX RT packs chronological status bits from register bit 23 downward:
	 * STCSCH bit 21 is IEC 60958 consumer byte 0 bit 2 (copyright not
	 * asserted), and STCSCL bit 22 is byte 3 bit 1 (48 kHz code 0x02).
	 * These exact words were checked against the CM6206 input's 48 kHz status
	 * and then proven over the physical output-to-input fibre loop. */
	SPDIF_STCSCH = UINT32_C(0x00200000);
	SPDIF_STCSCL = UINT32_C(0x00400000);
}
#endif

#endif /* __IMXRT1062__ || AIUS_AUDIO_CLOCK_FAKE_TARGET */
