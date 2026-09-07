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
 * OCRAM; keeping it in DTCM would have spent the whole free-for-locals zone as
 * it stood at this decision -- 17,152 B, what the int16 ring had just freed --
 * and moving it to OCRAM instead returned another 8,224 B, taking the zone to
 * its current 25,376. The consumer reads it from a DMA
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
 * runtime AUDIO checkpoint) can be PREEMPTED mid-write by the consumer (the
 * transmit DMA ISR, a higher NVIC priority).
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
     * ran dry WHILE THE TX GATE WAS OPEN. The vendor path substituted too --
     * it fell back to `block_silent` -- but counted nothing, so an underflow
     * was inaudible AND invisible. Idle substitution into a dark pad is
     * deliberately NOT counted (open-work register 2026-08-15, option b): the
     * free-running form advanced at ~48 k/s through every no-carrier hour,
     * wrapped after ~24.9 h, and buried real gate-open glitches in idle noise. */
    volatile uint32_t underruns;
    /* Consumer-owned: fill() invocations, one per transmit DMA interrupt --
     * each is AUDIO_BLOCK_SAMPLES frames on the wire, so against wall time
     * this is the TX bit-clock's rate measured from OUTSIDE the S/PDIF
     * clock domain (boot-lottery discriminator: a clock tree that latched
     * wrong at boot self-reports 48 kHz on every in-domain surface; this
     * counter cannot be fooled by it). */
    volatile uint32_t fills;
    /* Producer-owned, consumer-read: nonzero while the TX gate is open. The
     * runtime writers are tx_gate_actuate() at gate edges (the open edge
     * inside the same masked section as its trim) and boot-only reset()
     * below; fill() reads it once per underrun event (not per frame), so the
     * ISR cost is one load on the already-cold substitution path. */
    volatile uint32_t gate_open;
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
    ring->fills = 0u;
    ring->gate_open = 0u;
}

/* Empty the transition ring without erasing its lifetime underrun diagnostic.
 * The checkpoint caller masks the TX DMA interrupt across this one cursor
 * rebase, so fill() cannot observe a half-transitioned pair. Boot-only reset()
 * above remains the only operation that clears every counter and the gate
 * flag together. */
static inline void aius_spdif_tx24_flush_preserve_diag(
    AiusSpdifTx24Ring *ring)
{
    ring->consumed = ring->written;
}

/* Trim occupancy DOWN to `target_frames` by advancing the consumer cursor,
 * returning the frames dropped (0 when already at or under target). Same
 * caller contract as flush_preserve_diag: the caller masks interrupts across
 * this one cursor rebase so fill() cannot observe a half-transitioned pair.
 *
 * Two legitimate callers:
 *
 * - The TX gate's closed→open edge (e2e-latency Phase 3 R1): while the gate
 *   is closed the output is physically dark, so every dropped frame was
 *   silence nobody could hear, and the frames that REMAIN are the newest —
 *   the freshest audio starts the session.
 * - The latency governor's ratchet trim (aius-output-jitter Phase E, decision
 *   rule in aius_tx_governor.h): with the gate OPEN, after a stall backlog
 *   has parked the standing floor a sustained window above the gate-open
 *   target. This one IS an audible splice — one skip forward — accepted by
 *   contract in preference to a standing multi-tens-of-ms lag for the rest
 *   of the session. No other gate-open caller is legitimate. */
static inline uint32_t aius_spdif_tx24_trim_to(AiusSpdifTx24Ring *ring,
                                               uint32_t target_frames)
{
    const uint32_t level = ring->written - ring->consumed;
    if (level <= target_frames)
        return 0u;
    const uint32_t drop = level - target_frames;
    ring->consumed += drop;
    return drop;
}

/* Set occupancy to EXACTLY `target_frames`: trim down by advancing the
 * consumer cursor (as trim_to), or pad UP by committing digital silence when
 * the ring is short. Same masked-section caller contract as trim_to. The one
 * legitimate caller is the TX gate's closed->open edge, executed at the
 * producer's pre-commit instant (aius-output-jitter Phase F): with the pad
 * dark, dropped frames were never heard and padded frames are silence nobody
 * hears either, and setting the level at that instant is what makes the
 * standing pre-commit floor `target_frames` by construction rather than the
 * phase luck of the producer's first burst against the transmit ISR. The
 * buffer is never cleared (see reset()), so padding must write the zeros.
 * `target_frames` must not exceed AIUS_SPDIF_TX24_RING_FRAMES. */
static inline void aius_spdif_tx24_set_level(AiusSpdifTx24Ring *ring,
                                             uint32_t target_frames,
                                             uint32_t *dropped,
                                             uint32_t *padded)
{
    const uint32_t level = ring->written - ring->consumed;
    *dropped = 0u;
    *padded = 0u;
    if (level > target_frames) {
        *dropped = level - target_frames;
        ring->consumed += *dropped;
        return;
    }
    const uint32_t deficit = target_frames - level;
    uint32_t index = ring->written & AIUS_SPDIF_TX24_RING_MASK;
    for (uint32_t i = 0; i < deficit; i++) {
        ring->left[index] = 0;
        ring->right[index] = 0;
        index = (index + 1u) & AIUS_SPDIF_TX24_RING_MASK;
    }
    ring->written = ring->written + deficit;
    *padded = deficit;
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
 * A short ring is not an error: the remainder is digital silence, and when the
 * TX gate is open `underruns` advances by the number of frames substituted --
 * a closed gate substitutes uncounted idle silence into a dark pad. Bounded,
 * branchless per sample, and allocation-free -- this runs in the transmit DMA
 * ISR. */
static inline void aius_spdif_tx24_fill(AiusSpdifTx24Ring *ring,
                                        int32_t *dest, uint32_t frames)
{
    const uint32_t consumed = ring->consumed;
    const uint32_t available = ring->written - consumed;
    const uint32_t take = available < frames ? available : frames;
    uint32_t index = consumed & AIUS_SPDIF_TX24_RING_MASK;

    ring->fills = ring->fills + 1u;

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
        if (ring->gate_open != 0u) {
            ring->underruns = ring->underruns + (frames - take);
        }
    }
}

/* THE MISSED-FILL WITNESS: the one transmit fault this ring's own counters
 * cannot see.
 *
 * The transmit DMA is CIRCULAR (INTHALF | INTMAJOR): it never stops, and the
 * ISR's job is to refill the half the DMA is not currently reading. If the ISR
 * is serviced more than one AUDIO_BLOCK_SAMPLES period late -- a FlexSPI/PSRAM
 * bus hold, a long masked section -- the DMA has already re-read the half it
 * read last time and TRANSMITTED IT AGAIN. That is an audible repeat of 2.67 ms
 * of audio, and it advances NOTHING: fill() was never late for its own math, so
 * `underruns` does not move, and `fills` counts the interrupt that did arrive.
 * The only trace it leaves is the ring level ratcheting up 128 frames per
 * missed period, which reads as latency, not as a fault.
 *
 * The witness is the half address itself: under healthy service the ISR
 * alternates halves, so filling the SAME half twice in a row means an odd
 * number of periods went unserviced. An EVEN number is invisible here (parity
 * is restored) and stays the level instruments' business -- this detects the
 * odd case, which is the common one, rather than pretending to detect all of
 * them.
 *
 * Deliberately NOT a field of the ring: the ring lives in DMAMEM and the OCRAM
 * heap floor (link/aius_teensy41.ld, ASSERT _heap_end - _heap_start >= 0x1FFE0)
 * has zero margin, so a byte added there is a link error. These live in DTCM
 * .bss beside the ISR instead. */
typedef struct {
	/* Consumer-private: the half filled by the previous interrupt. Zero at
	 * reset can never false-positive -- a real half address is never 0. */
	uint32_t last_half;
	/* Consumer-owned, thread-read: interrupts that refilled the half they
	 * had just filled, millis() at the newest of them, and the frames the
	 * corrective rebase below dropped in total. */
	volatile uint32_t missed_fills;
	volatile uint32_t missed_fill_at_ms;
	volatile uint32_t missed_fill_frames;
	/* Consumer-written, thread-cleared: nonzero while a bus-state snapshot
	 * is held unpublished. A miss takes the snapshot only when this is zero,
	 * so a BURST keeps the FIRST one -- taken while the hold that caused it
	 * may still be visible -- and the thread releases it after publishing.
	 * Zero at reset is therefore "the next miss snapshots", which is what a
	 * boot wants without a cold-path initialiser. */
	volatile uint32_t snapshot_held;
} AiusTxFillWitness;

/* CONSUMER, once per transmit interrupt, with the address of the half about to
 * be filled. Returns nonzero when this is the same half as last time -- the
 * caller then stamps the time and takes the correction, which keeps every
 * clock and register read off the healthy path. One load, one compare and one
 * store when nothing is wrong. */
static inline int aius_tx_fill_witness_step(AiusTxFillWitness *witness,
                                            uint32_t half)
{
	const int repeated = witness->last_half == half;

	witness->last_half = half;
	if (repeated)
		witness->missed_fills = witness->missed_fills + 1u;
	return repeated;
}

/* CONSUMER, on a detected missed fill: drop `frames` of the OLDEST queued audio
 * by advancing the consumer cursor, bounded by the current level. Returns the
 * frames actually dropped.
 *
 * WHY THE RING MUST BE REBASED. A missed period is one fill() that never ran,
 * so the ring kept frames the transmitter would have taken: the standing level
 * -- which IS the output latency -- ratchets up one AUDIO_BLOCK_SAMPLES per
 * miss and never comes back down. Left alone, the latency governor eventually
 * notices the elevated floor and cuts it with a SECOND audible skip seconds
 * later (aius_tx_governor.h). Correcting here spends the same frames at the
 * instant the damage was done, when a splice is masked by the stale repeat the
 * DMA has just transmitted anyway, and leaves the governor's floor untouched.
 *
 * No masked section, unlike trim_to's callers: this is the CONSUMER advancing
 * its own cursor, which is the one cursor it owns outright (see the SPSC
 * contract at the top of this file). It is also why the correction lives in the
 * ISR rather than being reported to the epoch: only the consumer may move it. */
static inline uint32_t aius_spdif_tx24_drop_stale(AiusSpdifTx24Ring *ring,
                                                  uint32_t frames)
{
	const uint32_t level = ring->written - ring->consumed;
	const uint32_t drop = level < frames ? level : frames;

	ring->consumed = ring->consumed + drop;
	return drop;
}

/* The one ring. Defined in DMAMEM by aius_spdif_sources.cpp, beside the
 * transmit ISR that drains it; the producer is AiusAudioProcessor::enqueue()
 * in optical_main.cpp. */
extern AiusSpdifTx24Ring g_aius_spdif_tx24;

/* ISR-owned transmit diagnostics that are not ring state, defined in DTCM .bss
 * by aius_spdif_sources.cpp (see the witness's placement note above).
 *
 * g_aius_tx_trim_applied_at_ms is millis() at the TX period boundary that
 * consumed a same-nominal trim request, i.e. the instant CCM_ANALOG_PLL_AUDIO_NUM
 * actually changed. Paired with clock.trim_last_at_ms (stamped at the request,
 * thread context) it brackets the queueing delay -- up to one 128-frame period
 * -- between asking for a trim and the wire rate moving. 0 = no trim has been
 * applied this boot.
 *
 * g_aius_tx_missed_fill_flexspi holds FLEXSPI_INTR, FLEXSPI2_INTR and
 * FLEXSPI2_STS1 sampled at the first miss of a burst. A missed transmit
 * interrupt is a latency fault, not an audio fault, and the prime suspect on
 * this board is a FlexSPI bus hold (program flash is XIP on FlexSPI1, the
 * recorder's PSRAM on FlexSPI2) -- the same reasoning, and the same raw-word
 * discipline, as the stall witness's NVIC_IABR snapshot in optical_main.cpp. */
extern AiusTxFillWitness g_aius_tx_fill_witness;
extern volatile uint32_t g_aius_tx_trim_applied_at_ms;
extern volatile uint32_t g_aius_tx_missed_fill_flexspi[3];

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
