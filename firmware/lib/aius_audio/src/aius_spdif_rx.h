#ifndef AIUS_SPDIF_RX_H
#define AIUS_SPDIF_RX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE RECEIVE STAGING: one S/PDIF receive DMA half into one AudioStream block
 * pair, replacing the retained snapshot's AudioInputSPDIF3::isr() copy loop.
 *
 * WHY IT EXISTS. The snapshot's ISR (input_spdif3.cpp:105-131) guards its copy
 * with `offset <= AUDIO_BLOCK_SAMPLES*2`, writes AUDIO_BLOCK_SAMPLES samples at
 * `data[offset]`, then advances offset by 2*AUDIO_BLOCK_SAMPLES. At offset ==
 * 2*AUDIO_BLOCK_SAMPLES the guard still passes and the copy lands at
 * data[256..383] of a 128-sample block: 512 bytes past the pair, across the
 * next pool blocks' headers. Under the snapshot's own design that is latent --
 * the same ISR calls update_all() first, update() swaps the pair, and offset is
 * 0 at every copy. Aius breaks that assumption on purpose: replay hands
 * update_all() to the transmit ISR (replay_clock_service), and with the DPLL
 * unlocked the receive DMA storms at ~6.5x the block rate (2,465 update calls
 * per second measured on 2026-09-03), so several receive interrupts land
 * between two update() calls, the second writes out of bounds, and the
 * corrupted memory_pool_index values leak blocks on release(). The 2026-09-03
 * gate-4 replay run left the pool with two free blocks: every third update()
 * found no block, every miss reset the ingress generation, and the pipeline
 * never typed the stream again (open-work register, RX pool corruption under
 * replay).
 *
 * THE RULE. A DMA half is staged only into an EMPTY pair (offset 0) and only
 * whole, AUDIO_BLOCK_SAMPLES frames; anything else is dropped and counted. A
 * pair the graph has not consumed is never touched until update() swaps it.
 * With the input driving update_all() this is the snapshot's behaviour to the
 * sample; with the output driving it, surplus halves are dropped rather than
 * written past the block.
 *
 * NO VENDOR PATCH, by the aius_spdif_tx24.h route: the snapshot's DMA buffer is
 * a file static of the translation unit that includes it, `dma` is protected,
 * and DMAChannel::attachInterrupt() re-points the vector after begin(). The
 * three block statics are protected in the repo's input_spdif3.h for the hook.
 *
 * The staging is pure so the host clock-target suite pins it; cache
 * maintenance on the DMA half is the interrupt handler's job, before the call. */

/* Stage `frames` interleaved left/right words (24-bit samples, taken to 16
 * bits by the snapshot's >> 8) into `left`/`right`. Returns 1 when the pair was
 * empty and `frames` fits within `block_samples`, else 0 with nothing written.
 * `block_offset` is the snapshot's AudioInputSPDIF3::block_offset: 0 is empty,
 * and a staged pair reads `frames` so update()'s `>= AUDIO_BLOCK_SAMPLES` test
 * fires. */
static inline int aius_spdif_rx_stage(const int32_t *src, uint32_t frames,
                                      int16_t *left, int16_t *right,
                                      uint16_t *block_offset,
                                      uint32_t block_samples)
{
	if (!src || !left || !right || !block_offset)
		return 0;
	if (*block_offset != 0u || frames == 0u || frames > block_samples)
		return 0;
	for (uint32_t frame = 0u; frame < frames; frame++) {
		left[frame] = (int16_t)(src[2u * frame] >> 8);
		right[frame] = (int16_t)(src[2u * frame + 1u] >> 8);
	}
	*block_offset = (uint16_t)frames;
	return 1;
}

/* Target only. Install after the static AudioInputSPDIF3 has run begin(); the
 * counter is the lifetime number of receive DMA halves dropped because the pair
 * was still unconsumed or unallocated -- nonzero in practice only while the
 * output drives update_all() with the receiver storming. */
void aius_spdif_rx_begin(void);
uint32_t aius_spdif_rx_dropped_halves(void);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_SPDIF_RX_H */
