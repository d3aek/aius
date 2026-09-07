/* The missed-fill witness and its ring correction (glitch-timestamping
 * 2026-09-07).
 *
 * The transmit DMA is circular, so an interrupt that arrives a whole
 * AUDIO_BLOCK_SAMPLES period late finds the DMA has already re-read -- and
 * re-transmitted -- the half it filled last time. Nothing in the ring's own
 * counters moves: fill() was never late for its own arithmetic, so `underruns`
 * stays put and the only trace is the standing level ratcheting up one period's
 * worth per miss. These rows pin both halves of the answer: the half-parity
 * detector, and the cursor rebase that stops the miss from becoming permanent
 * latency (which the governor would otherwise cut with a second audible skip).
 *
 * The ring here is the production structure, driven through its own producer
 * and consumer entry points; nothing simulates it. */

#include "aius_spdif_tx24.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The engine block: one transmit interrupt's worth of frames, and therefore
 * exactly what one missed period costs the ring. */
#define BLOCK_FRAMES 128u

/* Two fake DMA half addresses. Only their inequality matters to the witness,
 * and neither may be 0 -- that is the value a zeroed witness holds, and the
 * reason the first call after a reset cannot report a miss. */
#define HALF_A UINT32_C(0x20200000)
#define HALF_B UINT32_C(0x20200400)

static AiusSpdifTx24Ring g_ring;

/* Stage and commit `frames` of silence, the way the producer does. */
static void commit_frames(AiusSpdifTx24Ring *ring, uint32_t frames)
{
	uint32_t staged = 0u;

	while (staged < frames) {
		int32_t *left = NULL;
		int32_t *right = NULL;
		uint32_t run = aius_spdif_tx24_stage(ring, staged, &left, &right);
		if (run > frames - staged)
			run = frames - staged;
		for (uint32_t index = 0u; index < run; index++) {
			left[index] = (int32_t)(staged + index);
			right[index] = -(int32_t)(staged + index);
		}
		staged += run;
	}
	aius_spdif_tx24_commit(ring, frames);
}

int main(void)
{
	AiusTxFillWitness witness;
	int32_t destination[BLOCK_FRAMES * 2u];

	/* Healthy service alternates halves, and the very first interrupt after
	 * a zeroed witness reports nothing: a real half address is never 0. */
	{
		memset(&witness, 0, sizeof(witness));
		assert(aius_tx_fill_witness_step(&witness, HALF_A) == 0);
		assert(witness.missed_fills == 0u);
		for (uint32_t index = 0u; index < 100u; index++) {
			const uint32_t half = (index & 1u) ? HALF_A : HALF_B;
			assert(aius_tx_fill_witness_step(&witness, half) == 0);
		}
		assert(witness.missed_fills == 0u);
		assert(witness.last_half == HALF_A);
	}

	/* The same half twice IS the fault, and it is counted once. */
	{
		memset(&witness, 0, sizeof(witness));
		assert(aius_tx_fill_witness_step(&witness, HALF_A) == 0);
		assert(aius_tx_fill_witness_step(&witness, HALF_A) == 1);
		assert(witness.missed_fills == 1u);
		/* Alternation resumes: no second report from the same fault. */
		assert(aius_tx_fill_witness_step(&witness, HALF_B) == 0);
		assert(aius_tx_fill_witness_step(&witness, HALF_A) == 0);
		assert(witness.missed_fills == 1u);
	}

	/* A burst of misses is a burst of counts -- the thread-side publisher
	 * collapses them into one event carrying the delta, which it can only do
	 * if every one of them is counted here. */
	{
		memset(&witness, 0, sizeof(witness));
		(void)aius_tx_fill_witness_step(&witness, HALF_B);
		for (uint32_t index = 0u; index < 5u; index++)
			assert(aius_tx_fill_witness_step(&witness, HALF_B) == 1);
		assert(witness.missed_fills == 5u);
	}

	/* An EVEN number of missed periods restores the parity and is invisible
	 * here by construction. Pinned so the limit is a stated contract rather
	 * than a surprise in the field. */
	{
		memset(&witness, 0, sizeof(witness));
		(void)aius_tx_fill_witness_step(&witness, HALF_A);
		/* Two periods lost: the next fill lands on the other half, as a
		 * healthy one would. */
		assert(aius_tx_fill_witness_step(&witness, HALF_B) == 0);
		assert(witness.missed_fills == 0u);
	}

	/* The correction: dropping the oldest frames returns the standing level
	 * to what it was before the missed period, which is the whole point --
	 * the repeat is already on the wire, the latency need not be. */
	{
		aius_spdif_tx24_reset(&g_ring);
		commit_frames(&g_ring, 512u);
		aius_spdif_tx24_fill(&g_ring, destination, BLOCK_FRAMES);
		const uint32_t floor_frames = aius_spdif_tx24_level(&g_ring);
		assert(floor_frames == 384u);

		/* One missed period: no fill ran, so the level stands 128 above
		 * the floor once the producer's next block lands. */
		commit_frames(&g_ring, BLOCK_FRAMES);
		assert(aius_spdif_tx24_level(&g_ring) ==
		       floor_frames + BLOCK_FRAMES);
		assert(aius_spdif_tx24_drop_stale(&g_ring, BLOCK_FRAMES) ==
		       BLOCK_FRAMES);
		assert(aius_spdif_tx24_level(&g_ring) == floor_frames);
		/* And nothing else moved: this is the consumer's own cursor. */
		assert(g_ring.underruns == 0u);
	}

	/* The drop takes the OLDEST frames -- the newest audio is what survives,
	 * exactly as the gate-open and governor trims do. */
	{
		aius_spdif_tx24_reset(&g_ring);
		commit_frames(&g_ring, 256u);
		assert(aius_spdif_tx24_drop_stale(&g_ring, 128u) == 128u);
		aius_spdif_tx24_fill(&g_ring, destination, BLOCK_FRAMES);
		/* Frame 128 is the first survivor, and its sample value is the
		 * index the producer wrote. */
		assert(destination[0] == 128);
		assert(destination[1] == -128);
		assert(g_ring.underruns == 0u);
	}

	/* Bounded by the level: an empty ring drops nothing (there is no audio
	 * to skip past, and the consumer must never overtake the producer), and
	 * a short one drops only what it holds. */
	{
		aius_spdif_tx24_reset(&g_ring);
		assert(aius_spdif_tx24_drop_stale(&g_ring, BLOCK_FRAMES) == 0u);
		assert(aius_spdif_tx24_level(&g_ring) == 0u);
		commit_frames(&g_ring, 32u);
		assert(aius_spdif_tx24_drop_stale(&g_ring, BLOCK_FRAMES) == 32u);
		assert(aius_spdif_tx24_level(&g_ring) == 0u);
		/* A zero-frame correction is a no-op, not a rebase. */
		commit_frames(&g_ring, 64u);
		assert(aius_spdif_tx24_drop_stale(&g_ring, 0u) == 0u);
		assert(aius_spdif_tx24_level(&g_ring) == 64u);
	}

	/* The cursors are free-running and wrap; the level is their unsigned
	 * difference, so a correction across the wrap is ordinary arithmetic. */
	{
		aius_spdif_tx24_reset(&g_ring);
		g_ring.written = UINT32_MAX - 64u;
		g_ring.consumed = UINT32_MAX - 64u;
		commit_frames(&g_ring, 256u);
		assert(aius_spdif_tx24_level(&g_ring) == 256u);
		assert(aius_spdif_tx24_drop_stale(&g_ring, BLOCK_FRAMES) ==
		       BLOCK_FRAMES);
		assert(aius_spdif_tx24_level(&g_ring) == 128u);
	}

	puts("Aius transmit missed-fill witness (parity, counts, ring rebase)");
	puts("RESULT: all checks passed");
	return 0;
}
