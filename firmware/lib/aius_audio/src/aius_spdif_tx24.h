#ifndef AIUS_SPDIF_TX24_H
#define AIUS_SPDIF_TX24_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE 24-BIT OUTPUT PATH: our own route from the playback ring into the S/PDIF
 * transmit DMA buffer, alongside the retained AudioOutputSPDIF3 rather than
 * through it.
 *
 * WHY IT EXISTS. `audio_block_t` is `int16_t data[AUDIO_BLOCK_SAMPLES]`
 * (cores/teensy4/AudioStream.h:76), so every sample that reaches the wire
 * through the AudioStream engine has been reduced to 16 bits -- a ~-93 dBFS
 * floor on a chain that is double-precision, dithered and true-peak limited
 * throughout. The hardware is not the limit: S/PDIF carries 24 bits, SPDIF_STL
 * and SPDIF_STR are 24-bit registers, and the transmit DMA buffer is
 * `int32_t`. The proof is one line of the retained snapshot's ISR,
 *
 *     *dest++ = (*src_left++) << 8;   (output_spdif3.cpp:130)
 *
 * which shifts 16-bit data back up into the 24-bit slot it was just reduced
 * out of. The old chain therefore spent two stages -- reduce to 16, shift back
 * to 24 -- to arrive at a strictly worse word than the DSP already had.
 *
 * The DSP's output is 24-bit signed integer by contract
 * (AIUS_DSP_SAMPLE_MAX = 2^23-1), so THIS PATH IS LOSSLESS FROM THE DSP TO THE
 * WIRE AND NEEDS NO DITHER AT ALL. Dither is demoted from a correctness fix to
 * a refinement that only a 16-bit output selection needs; aius_out16_* stays
 * reachable (aius_out_stage_run's 16-bit arm) and is bypassed by default.
 *
 * NO VENDOR PATCH. The snapshot is untouched. Three things make that possible:
 *
 *   1. `SPDIF_tx_buffer` is a FILE STATIC of output_spdif3.cpp, and that file
 *      is #included into aius_spdif_sources.cpp -- a file we own -- so code
 *      appended to that translation unit addresses the transmit buffer
 *      directly.
 *   2. `dma`, `update_responsibility` and `AudioStream::update_all()` are
 *      `protected static`, so a private-inheritance shim reaches them. That is
 *      the trick AiusSpdifOutputClock already ships in optical_main.cpp.
 *   3. `DMAChannel::attachInterrupt()` only stores a vector, so pointing the
 *      transmit DMA interrupt at our filler after begin() has run replaces the
 *      vendor ISR without editing it.
 *
 * WHAT DISAPPEARS WITH IT. `emit_block()`, the two `audio_block_t` allocations
 * per engine block, the AudioConnection pair into AudioOutputSPDIF3, and the
 * vendor two-deep block queue with its `block_silent` substitution. Transmit
 * queue occupancy IS this ring's level and an underflow IS this ring's
 * counter, which is what docs/plans/aius-sample-rate-following.md
 * §Interaction with the 24-bit output path asked for.
 *
 * MEMORY. The ring is 2 x 2048 int32 = 16 KiB and lives in DMAMEM (OCRAM), not
 * DTCM. It is the one large buffer here and the standing rule puts those in
 * OCRAM; keeping it in DTCM would spend the whole 17,152-byte free-for-locals
 * zone that the int16 ring had just freed. The consumer reads it from a DMA
 * ISR, which is the reason to think twice -- and the reason it is still fine:
 * the read is strictly sequential, 256 words per interrupt, so the L1 D-cache
 * amortises each OCRAM line fill over eight words. See
 * docs/reference/firmware-io.md §S/PDIF output for the arithmetic.
 *
 * NO CACHE MAINTENANCE ON THE RING, deliberately. DMAMEM normally implies a
 * flush, but nothing DMAs this buffer: both ends are CPU accesses through the
 * same write-back D-cache, so they are coherent by construction. Only
 * SPDIF_tx_buffer needs flushing, and that is the DMA's actual source.
 *
 * CONCURRENCY: single producer, single consumer, no critical section.
 * `written` and `consumed` are free-running 32-bit counters, each written by
 * exactly one side, and the level is their unsigned difference -- which is
 * correct across the counters' own wrap. This matters because the two sides are
 * no longer the same context: the producer (enqueue(), reached from the
 * software ISR that AudioStream::update_all() pends) can be PREEMPTED
 * mid-write by the consumer (the transmit DMA ISR, a higher NVIC priority).
 * The old int16 ring had both ends inside update(), so a plain `count` was
 * safe there and would be a race here. Staged frames become visible only at
 * aius_spdif_tx24_commit(), so a preempting consumer never sees a half-written
 * block; 32-bit aligned loads and stores are single-copy atomic on the
 * Cortex-M7, so neither counter can be observed torn.
 */

/* Frames of stereo elasticity between the decoder's 512-frame bursts and the
 * transmitter's 128-frame drain. Unchanged from the int16 ring it replaces, so
 * the AIUS_NO_SPACE threshold and the meter's `queue_frames` keep their
 * meaning. Must be a power of two: the cursors reduce modulo it on the audio
 * path, and a non-power-of-two would be a hardware divide per interrupt. */
#define AIUS_SPDIF_TX24_RING_FRAMES 2048u

#if (AIUS_SPDIF_TX24_RING_FRAMES & (AIUS_SPDIF_TX24_RING_FRAMES - 1u)) != 0u
#error "AIUS_SPDIF_TX24_RING_FRAMES must be a power of two"
#endif

#define AIUS_SPDIF_TX24_RING_MASK (AIUS_SPDIF_TX24_RING_FRAMES - 1u)

/* Samples are 24-bit signed values in [AIUS_DSP_SAMPLE_MIN,
 * AIUS_DSP_SAMPLE_MAX], stored one per int32 rather than packed into three
 * bytes. Packing would save 4 KiB of OCRAM -- of 310 KiB free -- and would cost
 * three byte loads plus two shifts per sample in the transmit ISR, against a
 * bank that is 41 % used. The aligned word is also what SPDIF_STL takes, so an
 * unpacked ring makes the fill a straight copy. */
typedef struct {
    int32_t left[AIUS_SPDIF_TX24_RING_FRAMES];
    int32_t right[AIUS_SPDIF_TX24_RING_FRAMES];
    /* Producer-owned: frames ever committed. */
    volatile uint32_t written;
    /* Consumer-owned: frames ever transmitted. */
    volatile uint32_t consumed;
    /* Consumer-owned: frames of digital silence substituted for a ring that
     * ran dry. The vendor path did this too -- it fell back to `block_silent`
     * -- but counted nothing, so an underflow was inaudible AND invisible. */
    volatile uint32_t underruns;
} AiusSpdifTx24Ring;

/* Put the ring in the empty state. MUST be called before the transmit interrupt
 * is armed; it is not optional and the cursors are not zero at reset.
 *
 * WHY THIS EXISTS, because the original code asserted the opposite. This ring
 * lives in `.bss.dma`, which is `NOLOAD` -- the Teensy startup path clears DTCM
 * and does not clear OCRAM -- and worse, the i.MX RT1062 **boot ROM overwrites
 * three fixed runs of OCRAM on every reset** (measured; the ranges and the
 * method are in aius-network-hardening.md, and they are what displaced the
 * retained boot ledger). The three cursors sit at the top of this structure,
 * inside the largest of those runs, so after a reset they held ROM scratch.
 *
 * The consequence was audible rather than theoretical: `aius_spdif_tx24_fill()`
 * computes `written - consumed`, so arbitrary cursors make the ring look
 * arbitrarily full and the first interrupts transmit whatever the buffer
 * happens to contain. Bounded -- the index is masked, so there is no
 * out-of-bounds access and no crash -- which is exactly why it survived: a
 * silent burst of garbage at every boot, and nothing failing.
 *
 * The buffer itself is deliberately NOT cleared. 16 KiB of stores before the
 * first block would be wasted work: with both cursors at zero the ring reads as
 * empty, `fill()` substitutes silence, and no sample is ever read before a
 * producer writes it. Only the cursors carry the invariant. */
static inline void aius_spdif_tx24_reset(AiusSpdifTx24Ring *ring)
{
    ring->written = 0u;
    ring->consumed = 0u;
    ring->underruns = 0u;
}

/* Committed frames not yet transmitted. This is the transmit queue occupancy
 * the meter reports; there is no other queue behind it. */
static inline uint32_t aius_spdif_tx24_level(const AiusSpdifTx24Ring *ring)
{
    return ring->written - ring->consumed;
}

/* Frames the producer may stage before it would overwrite untransmitted
 * audio. */
static inline uint32_t aius_spdif_tx24_free(const AiusSpdifTx24Ring *ring)
{
    return AIUS_SPDIF_TX24_RING_FRAMES - (ring->written - ring->consumed);
}

/* PRODUCER, step 1. Point `*left` / `*right` at the destination planes for the
 * frame `offset` frames past the write cursor, and return how many frames fit
 * there CONTIGUOUSLY (i.e. before the ring wraps). The caller writes at most
 * that many, then calls again at the new offset for any remainder -- exactly
 * the two-call shape the int16 ring used, but with the index arithmetic here
 * instead of at the call site.
 *
 * Staging is invisible to the consumer: nothing is transmitted until
 * aius_spdif_tx24_commit(). The caller is responsible for checking
 * aius_spdif_tx24_free() first; staging beyond it silently overwrites frames
 * the transmitter has not yet sent. */
static inline uint32_t aius_spdif_tx24_stage(AiusSpdifTx24Ring *ring,
                                             uint32_t offset,
                                             int32_t **left, int32_t **right)
{
    const uint32_t index = (ring->written + offset) & AIUS_SPDIF_TX24_RING_MASK;
    *left = &ring->left[index];
    *right = &ring->right[index];
    return AIUS_SPDIF_TX24_RING_FRAMES - index;
}

/* PRODUCER, step 2. Publish `frames` staged frames. The single store to
 * `written` is what makes them visible, so it must follow every sample store
 * -- which it does: `written` is volatile and the sample writes are through
 * pointers derived from it. */
static inline void aius_spdif_tx24_commit(AiusSpdifTx24Ring *ring,
                                          uint32_t frames)
{
    ring->written = ring->written + frames;
}

/* CONSUMER. Write `frames` stereo frames into `dest` as the interleaved 24-bit
 * transmit words the S/PDIF DMA moves to SPDIF_STL / SPDIF_STR: left at even
 * indices, right at odd, 2 * `frames` words in all. `dest` is one half of
 * SPDIF_tx_buffer.
 *
 * A short ring is not an error: the remainder is digital silence and
 * `underruns` advances by the number of frames substituted. Bounded, branchless
 * per sample, and allocation-free -- this runs in the transmit DMA ISR. */
static inline void aius_spdif_tx24_fill(AiusSpdifTx24Ring *ring,
                                        int32_t *dest, uint32_t frames)
{
    const uint32_t consumed = ring->consumed;
    const uint32_t available = ring->written - consumed;
    const uint32_t take = available < frames ? available : frames;
    uint32_t index = consumed & AIUS_SPDIF_TX24_RING_MASK;

    for (uint32_t i = 0; i < take; i++) {
        /* The whole point of the path: the DSP's 24-bit word, unmodified, into
         * a 24-bit transmit register. No dither, no reduction, no shift. */
        *dest++ = ring->left[index];
        *dest++ = ring->right[index];
        index = (index + 1u) & AIUS_SPDIF_TX24_RING_MASK;
    }
    ring->consumed = consumed + take;

    if (take < frames) {
        for (uint32_t i = take; i < frames; i++) {
            *dest++ = 0;
            *dest++ = 0;
        }
        ring->underruns = ring->underruns + (frames - take);
    }
}

/* The one ring. Defined in DMAMEM by aius_spdif_sources.cpp, beside the
 * transmit ISR that drains it; the producer is AiusAudioProcessor::enqueue()
 * in optical_main.cpp. */
extern AiusSpdifTx24Ring g_aius_spdif_tx24;

/* Point the S/PDIF transmit DMA interrupt at the 24-bit filler. Call once,
 * after AudioOutputSPDIF3 has been constructed (its begin() allocates the DMA
 * channel and installs the vendor ISR) and before the first decoded block. Cold
 * path, FLASHMEM at the definition. Until it is called the vendor ISR runs and
 * transmits `block_silent`, which is the same silence the board idles in. */
void aius_spdif_tx24_begin(void);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_SPDIF_TX24_H */
