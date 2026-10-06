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
#ifndef AIUS_TX_FILL_SENSOR_COLD
#define AIUS_TX_FILL_SENSOR_COLD FLASHMEM
#endif
#include "aius_tx_fill_sensor.h"
#endif
/* The receive staging is pure and host-built (the clock-target suite pins
 * it); only its hook below is target-only. */
#include "aius_spdif_rx.h"

#if defined(__IMXRT1062__) || defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)

volatile AiusAudioClockSharedState g_aius_audio_clock_state;

/* DTCM .bss in the target build (see the witness note below for why not
 * DMAMEM); defined in the FAKE build too, so clock-target reads the stamps
 * the boundary writes. Declared extern "C" in aius_audio_clock_internal.h. */
volatile uint32_t g_aius_tx_trim_applied_at_ms;
volatile uint32_t g_aius_tx_fill_applied_at_ms;

#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
volatile uint32_t g_aius_audio_clock_fake_now_ms;
#endif

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

/* DTCM .bss, NOT DMAMEM, and that is a link constraint rather than a
 * preference: the OCRAM heap floor assert (link/aius_teensy41.ld,
 * `_heap_end - _heap_start >= 0x1FFE0`) stands at exactly zero margin, so a
 * byte added to the ring above is a link error. Zero-initialised is the correct
 * reset state for all of them (a real half address is never 0, and 0 reads as
 * "never happened" on every stamp, the two applied-at stamps above included).
 * Contracts: aius_spdif_tx24.h. */
AiusTxFillWitness g_aius_tx_fill_witness;
volatile uint32_t g_aius_tx_missed_fill_flexspi[3];
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
 *  1. acknowledge a period completed by an existing hold, and restore validity
 *     at the last of them;
 *  2. consume a newly queued request;
 *  3. a HOLD request marks validity invalid and arms four periods without
 *     decrementing at that same boundary; and
 *  4. a TRIM request writes the queued numerator, touches neither validity
 *     nor the hold, and stamps the applied-at time its queued kind names.
 *
 * There are no checkpoint helpers in this translation unit.
 *
 * WHY A TRIM NO LONGER HOLDS VALIDITY (2026-09-07). The hold exists to tell a
 * receiver not to convert samples that span a discontinuous clock change. A
 * same-nominal NUM step is not one: it is carrier-continuous -- proven at
 * +438 ppm, far beyond the tracker's ~10 ppm corrections
 * (docs/plans/aius-sample-rate-following.md) -- and every sample either side of
 * it is perfectly convertible. Marking them invalid bought nothing and cost at
 * least AIUS_AUDIO_CLOCK_INVALID_PERIODS periods (10.67 ms) of V=1 per trim,
 * which a validity-honouring DAC answers with a mute: a sub-second dropout
 * with no counter behind it, i.e. the exact fault class the instruments added
 * beside this are chasing. The dark paths KEEP the hold -- there the pad is
 * closed anyway and the discontinuity is real.
 *
 * The bit's polarity is inverted against its own name and this file used to
 * read it backwards; see AIUS_AUDIO_CLOCK_VALCTRL_VALID_BIT. */
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
			/* SETTING ValCtrl is what puts V=0 on the wire. */
			clock_register_write(
				AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
				control |
					AIUS_AUDIO_CLOCK_VALCTRL_VALID_BIT);
			g_aius_audio_clock_state.invalid_active = 0u;
		}
	}

	const uint32_t request = g_aius_audio_clock_state.request;
	if (request == AIUS_AUDIO_CLOCK_BOUNDARY_TRIM) {
		/* Carrier-continuous: the numerator, and nothing else. */
		clock_register_write(
			AIUS_AUDIO_CLOCK_REG_PLL_NUMERATOR,
			g_aius_audio_clock_state.queued_plan.pll_numerator);

		g_aius_audio_clock_state.applied_sequence++;
		clock_memory_barrier();
		g_aius_audio_clock_state.applied_plan.target_rate_millihz =
			g_aius_audio_clock_state
				.queued_plan.target_rate_millihz;
		g_aius_audio_clock_state.applied_plan.pll_numerator =
			g_aius_audio_clock_state.queued_plan.pll_numerator;
		g_aius_audio_clock_state.has_applied_plan = 1u;
		clock_memory_barrier();
		g_aius_audio_clock_state.applied_sequence++;
		g_aius_audio_clock_state.request =
			AIUS_AUDIO_CLOCK_BOUNDARY_NONE;
		/* The instant the wire rate actually moved, against the request's
		 * clock.trim_last_at_ms (CENTRE) or clock.fill_trim_last_at_ms
		 * (FILL): each pair brackets the queueing delay, at most one
		 * 128-frame period. The kind picks the stamp and nothing else:
		 * one load, one compare, one store. request_trim refuses any
		 * other kind, so everything not FILL is CENTRE. */
		const uint32_t now_ms = aius_audio_clock_now_ms();
		if (g_aius_audio_clock_state.queued_kind ==
		    static_cast<uint32_t>(AIUS_AUDIO_CLOCK_TRIM_FILL))
			g_aius_tx_fill_applied_at_ms = now_ms;
		else
			g_aius_tx_trim_applied_at_ms = now_ms;
	} else if (request != AIUS_AUDIO_CLOCK_BOUNDARY_NONE) {
		const uint32_t control = clock_register_read(
			AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL);
		clock_register_write(
			AIUS_AUDIO_CLOCK_REG_SPDIF_CONTROL,
			control & ~AIUS_AUDIO_CLOCK_VALCTRL_VALID_BIT);
		g_aius_audio_clock_state.invalid_active = 1u;
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
	/* The TX DMA's read offset into the half it is playing, in frames
	 * 0..AUDIO_BLOCK_SAMPLES-1 (aius-sample-rate-following Phase 3 sensor).
	 * FLASHMEM: called per producer block from the flash-resident enqueue. */
	static uint32_t dma_offset_frames(void);
	/* The gate open's masked read: {half, offset} and boundary-pending from
	 * ONE SADDR read. FASTRUN: its caller holds the interrupt mask. */
	static uint32_t dma_read_position(uint32_t *half,
	                                  uint32_t *offset_frames);
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
 * exactly the five sections it pins (HOT_SECTIONS in verify_target.py). */
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

	/* MISSED-FILL WITNESS + CORRECTION (glitch-timestamping 2026-09-07).
	 * Filling the half we just filled means the DMA has already re-read --
	 * and re-transmitted -- the other one, i.e. this interrupt is a whole
	 * period late. The repeat is already on the wire and nothing can undo
	 * it; what CAN be undone is its permanent cost, the AUDIO_BLOCK_SAMPLES
	 * of ring occupancy no fill() consumed, which otherwise stands as extra
	 * latency until the governor cuts it with a second audible skip. The
	 * whole correction is the consumer moving its own cursor, so no masking
	 * is needed. Healthy path: one compare (the witness step's), and none of
	 * the clock, register or ring reads below. */
	if (aius_tx_fill_witness_step(&g_aius_tx_fill_witness,
	                              (uint32_t)dest)) {
		g_aius_tx_fill_witness.missed_fill_at_ms = millis();
		g_aius_tx_fill_witness.missed_fill_frames =
			g_aius_tx_fill_witness.missed_fill_frames +
			aius_spdif_tx24_drop_stale(&g_aius_spdif_tx24,
			                           AUDIO_BLOCK_SAMPLES);
		if (g_aius_tx_fill_witness.snapshot_held == 0u) {
			/* Raw words, decoded by the host: the firmware is a
			 * camera here, exactly as the stall witness is with
			 * NVIC_IABR. */
			g_aius_tx_missed_fill_flexspi[0] = FLEXSPI_INTR;
			g_aius_tx_missed_fill_flexspi[1] = FLEXSPI2_INTR;
			g_aius_tx_missed_fill_flexspi[2] = FLEXSPI2_STS1;
			g_aius_tx_fill_witness.snapshot_held = 1u;
		}
	}

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

/* The pure header hard-codes the TX buffer geometry; tie it to the real buffer. */
static_assert(sizeof(SPDIF_tx_buffer) == AIUS_TX_FILL_BUFFER_BYTES &&
		      AUDIO_BLOCK_SAMPLES == AIUS_TX_FILL_HALF_FRAMES &&
		      AUDIO_BLOCK_SAMPLES * AIUS_TX_FILL_FRAME_BYTES ==
			      AIUS_TX_FILL_BUFFER_BYTES / 2u,
	      "aius_tx_fill_sensor.h TX buffer geometry diverged from SPDIF_tx_buffer");

/* Same half test as isr() (saddr against the buffer's midpoint), reduced to the
 * offset inside that half by the pure, host-tested mapping in
 * aius_tx_fill_sensor.h. */
FLASHMEM uint32_t AiusSpdifTx24Hook::dma_offset_frames(void)
{
	const uint32_t base = (uint32_t)SPDIF_tx_buffer;
	return aius_tx_fill_saddr_offset_frames((uint32_t)(dma.TCD->SADDR), base);
}

/* C-linkage entry for optical_main.cpp, which cannot name the shim class. */
extern "C" FLASHMEM uint32_t aius_spdif_tx24_dma_offset_frames(void)
{
	return AiusSpdifTx24Hook::dma_offset_frames();
}

/* Runs inside the caller's masked section, so: FASTRUN (never flash), no
 * PRIMASK access (an unconditional interrupts() would reopen the caller's
 * section), ONE SADDR read (a second read, or the DMA interrupt flag, can
 * straddle a crossing), and the offset computed inline rather than through
 * dma_offset_frames(), which is FLASHMEM. Same half test and clamp as
 * aius_tx_fill_saddr_offset_frames. Returns nonzero when a period boundary is
 * pending: SADDR lies in the half the ISR last filled, decided by the shared
 * always-inline predicate aius_tx_fill_boundary_pending (aius_tx_fill_sensor.h;
 * aius-sample-rate-following Phase 4, Read phi inside the masked section). */
FASTRUN uint32_t AiusSpdifTx24Hook::dma_read_position(uint32_t *half,
                                                      uint32_t *offset_frames)
{
	const uint32_t saddr = (uint32_t)(dma.TCD->SADDR);
	const uint32_t base = (uint32_t)SPDIF_tx_buffer;
	const uint32_t half_bytes = AIUS_TX_FILL_BUFFER_BYTES / 2u;
	const uint32_t second = saddr >= base + half_bytes ? 1u : 0u;
	const uint32_t frames =
		(saddr - base - second * half_bytes) / AIUS_TX_FILL_FRAME_BYTES;

	*half = second;
	*offset_frames = frames < (uint32_t)AIUS_TX_FILL_HALF_FRAMES
		? frames : (uint32_t)AIUS_TX_FILL_HALF_FRAMES - 1u;
	return aius_tx_fill_boundary_pending(
		saddr, base, g_aius_tx_fill_witness.last_half) ? 1u : 0u;
}

/* C-linkage entry for the gate open in optical_main.cpp. FASTRUN on this core
 * is section placement only, so noinline keeps the symbol (and the ITCM
 * residency test/itcm-placement/verify_target.py pins) from being folded
 * away. */
extern "C" FASTRUN __attribute__((noinline)) uint32_t
aius_spdif_tx24_dma_read_position(uint32_t *half, uint32_t *offset_frames)
{
	return AiusSpdifTx24Hook::dma_read_position(half, offset_frames);
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

/* The receive-side shim, same shape as AiusSpdifTx24Hook: private inheritance
 * reaches `dma`, `update_responsibility`, `update_all()` and (protected in the
 * repo's input_spdif3.h) the block pair; `spdif_rx_buffer` is the snapshot's
 * file static, addressable because the snapshot is included above. */
class AiusSpdifRxHook : private AudioInputSPDIF3 {
public:
	AiusSpdifRxHook(void) = delete;
	static void install(void);
	static void isr(void);
	static volatile uint32_t dropped_halves;
};

volatile uint32_t AiusSpdifRxHook::dropped_halves;

/* THE RECEIVE DMA INTERRUPT, once per AUDIO_BLOCK_SAMPLES frames while the
 * DPLL is locked (and several times that while it is not), replacing
 * AudioInputSPDIF3::isr(). The snapshot's order is kept -- graph first, so the
 * pair update() swaps in is the one this interrupt fills -- and the only
 * behavioural change is the one aius_spdif_rx.h states: a pair the graph has
 * not consumed is dropped from, never written past.
 *
 * Not FLASHMEM: per-block code belongs in ITCM, as for the transmit hook. */
void AiusSpdifRxHook::isr(void)
{
	dma.clearInterrupt();
	if (update_responsibility)
		update_all();

	/* Which half to drain: the one the DMA is NOT filling. Identical test to
	 * the snapshot's (input_spdif3.cpp:90), against the same buffer. */
	const uint32_t daddr = (uint32_t)(dma.TCD->DADDR);
	int32_t *const src =
		(daddr < (uint32_t)spdif_rx_buffer + sizeof(spdif_rx_buffer) / 2)
			? (int32_t *)&spdif_rx_buffer[AUDIO_BLOCK_SAMPLES * 2]
			: (int32_t *)&spdif_rx_buffer[0];

	audio_block_t *const left = block_left;
	audio_block_t *const right = block_right;
	if (left == nullptr || right == nullptr) {
		/* update() allocates both or neither, so a lone block never exists;
		 * no pair means the pool could not supply one, which is a drop. */
		dropped_halves++;
		return;
	}
	/* The half is 32-byte aligned and a multiple of 32 bytes (the buffer is
	 * aligned(32) and each half is AUDIO_BLOCK_SAMPLES * 2 * 4 = 1,024 bytes),
	 * so the DMA-written lines can be invalidated whole rather than the
	 * snapshot's line-by-line DCIMVAC inside the copy loop. */
#if IMXRT_CACHE_ENABLED >= 1
	arm_dcache_delete(src, AUDIO_BLOCK_SAMPLES * 2 * sizeof *src);
#endif
	if (!aius_spdif_rx_stage(src, AUDIO_BLOCK_SAMPLES, left->data,
	                         right->data, &block_offset, AUDIO_BLOCK_SAMPLES))
		dropped_halves++;
}

FLASHMEM void AiusSpdifRxHook::install(void)
{
	/* One vector store under a critical section, as for the transmit hook;
	 * the pair and offset are the snapshot's own statics and need no reset. */
	__disable_irq();
	dma.attachInterrupt(AiusSpdifRxHook::isr);
	__enable_irq();
}
#endif

} /* namespace */

#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
void aius_audio_clock_test_tx_boundary(void)
{
	aius_audio_clock_tx_boundary();
}

void aius_audio_clock_fake_set_now_ms(uint32_t now_ms)
{
	g_aius_audio_clock_fake_now_ms = now_ms;
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

FLASHMEM void aius_spdif_rx_begin(void)
{
	AiusSpdifRxHook::install();
}

FLASHMEM uint32_t aius_spdif_rx_dropped_halves(void)
{
	return AiusSpdifRxHook::dropped_halves;
}
#endif

#endif /* __IMXRT1062__ || AIUS_AUDIO_CLOCK_FAKE_TARGET */
