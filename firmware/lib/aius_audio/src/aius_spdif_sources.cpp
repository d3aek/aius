/* Build only the PJRC Teensy 4 S/PDIF source units required by Aius.
 * The retained source snapshots are the upstream implementation boundary. */
#include "../../../../evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE/output_spdif3.cpp"
#include "../../../../evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp"

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
#include "aius_spdif_tx24.h"

#if defined(__IMXRT1062__)

/* DMAMEM, not DTCM: 16 KiB is the one large buffer in this path and OCRAM is
 * where large buffers live. Uninitialised at reset, which is correct -- both
 * cursors start at zero, so the ring reads as empty and the first interrupts
 * transmit silence until the first decoded block lands.
 *
 * DMAMEM here does NOT imply cache maintenance: no DMA reads this buffer. The
 * producer and the consumer are both CPU accesses through the same write-back
 * D-cache, so they are coherent. SPDIF_tx_buffer is the buffer the DMA reads,
 * and that one is flushed below. */
DMAMEM AiusSpdifTx24Ring g_aius_spdif_tx24;

namespace {

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
	 * pends the software interrupt, so the DSP runs at a LOWER priority than
	 * this handler -- which is why the ring is a single-producer/single-consumer
	 * structure rather than a plain count. */
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
	__disable_irq();
	dma.attachInterrupt(AiusSpdifTx24Hook::isr);
	__enable_irq();
}

} /* namespace */

FLASHMEM void aius_spdif_tx24_begin(void)
{
	AiusSpdifTx24Hook::install();
}

#endif /* __IMXRT1062__ */
