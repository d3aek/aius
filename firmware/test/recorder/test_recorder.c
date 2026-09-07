/* The flight recorder's portable-core suite: the transition table (including
 * token acknowledge vs reclaim), the trigger edge/resync/qualification cases,
 * ring/chunk arithmetic at absent/8/16 MB geometry, gap accounting, and the
 * drain's interleave + budget behaviour. Every expectation cites the contract
 * (aius-optical-prototype.md §PSRAM flight recorder, prebuild-gated
 * 2026-07-27). */

#include "aius_recorder.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Small geometry keeps wrap and staging-full reachable in a few blocks; the
 * arithmetic is the same power-of-two masking the 8/16 MB probe produces. */
#define RING_FRAMES 1024u
#define RING_BYTES (RING_FRAMES * AIUS_RECORDER_FRAME_BYTES)
#define SLOTS 4u
#define SLOT_FRAMES 8u
#define AUDIO_BURST_SLOTS 8u
#define AUDIO_BURST_SLOT_FRAMES 512u
#define AUDIO_BURST_RING_FRAMES \
	(AUDIO_BURST_SLOTS * AUDIO_BURST_SLOT_FRAMES)
#define AUDIO_BURST_RING_BYTES \
	(AUDIO_BURST_RING_FRAMES * AIUS_RECORDER_FRAME_BYTES)
#define MIB (1024u * 1024u)
/* The caller's configured nominal, and the alternate the accepted live rate
 * may actually be -- the pair the rate stamp exists to tell apart. */
#define DEFAULT_RATE_HZ 48000u
#define ALT_RATE_HZ 44100u

typedef struct {
	AiusRecorder r;
	uint8_t ring[RING_BYTES];
	int32_t staging[SLOTS * SLOT_FRAMES * 2u];
	uint32_t staging_frames[SLOTS];
} Fixture;

static void fixture_init(Fixture *f)
{
	memset(f, 0, sizeof(*f));
	assert(aius_recorder_init(&f->r, f->ring, RING_BYTES, 16u, f->staging,
	                          f->staging_frames, SLOTS, SLOT_FRAMES,
	                          DEFAULT_RATE_HZ) == AIUS_OK);
	assert(f->r.state == AIUS_RECORDER_RECORDING);
}

/* One committed block at an explicit rate, whose samples encode (base + index)
 * per channel so the drain's interleave is checkable sample-exactly. */
static void feed_block_at(Fixture *f, uint32_t frames, int32_t base,
                          uint32_t sample_rate_hz, uint32_t overruns,
                          uint32_t underruns)
{
	int32_t l[SLOT_FRAMES];
	int32_t r[SLOT_FRAMES];
	assert(frames <= SLOT_FRAMES);
	for (uint32_t i = 0; i < frames; i++) {
		l[i] = base + (int32_t)i;
		r[i] = -(base + (int32_t)i);
	}
	aius_recorder_producer_block(&f->r, l, r, frames, sample_rate_hz,
	                        overruns, underruns, false, 0.0f, 1000u);
}

/* The steady case every non-rate test wants: a block at the configured
 * nominal, so the rate stamp is a no-op and those tests read as before. */
static void feed_block(Fixture *f, uint32_t frames, int32_t base,
                       uint32_t overruns, uint32_t underruns)
{
	feed_block_at(f, frames, base, DEFAULT_RATE_HZ, overruns, underruns);
}

static void drain_all(Fixture *f)
{
	while (aius_recorder_drain(&f->r, 65536u) != 0u)
		;
}

static void test_absent_is_first_class(void)
{
	AiusRecorder r;
	/* ring_bytes 0 is the bare board: OK, ABSENT, every op refuses. */
	assert(aius_recorder_init(&r, NULL, 0u, 0u, NULL, NULL, 0u, 0u,
	                          DEFAULT_RATE_HZ) == AIUS_OK);
	assert(r.state == AIUS_RECORDER_ABSENT);
	assert(aius_recorder_freeze(&r) == AIUS_INVALID_ARG);
	assert(aius_recorder_thaw(&r) == AIUS_INVALID_ARG);
	const uint8_t *p;
	uint32_t n;
	assert(aius_recorder_read(&r, 0u, 8u, 0u, &p, &n) == AIUS_INVALID_ARG);
	assert(aius_recorder_drain(&r, 65536u) == 0u);
	int32_t plane[4] = { 0, 0, 0, 0 };
	aius_recorder_producer_block(&r, plane, plane, 4u, ALT_RATE_HZ, 0u, 0u,
	                        false, 0.0f, 0u);
	assert(r.frames_staged == 0u);
	/* An absent board stages nothing, so the refused block's rate never
	 * reaches the report: the configured nominal stands. */
	assert(r.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(r.rate_changes == 0u);
	/* Config is reportable and settable on an absent board (the status
	 * surface echoes it; there is just nothing to arm). */
	assert(aius_recorder_set_config(&r, AIUS_RECORDER_MASK_ALL, -6.0f, 2u,
	                                10u) == AIUS_OK);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&r, &s);
	assert(s.state == AIUS_RECORDER_ABSENT);
	assert(s.fitted_mb == 0u);
	assert(s.ring_frames == 0u);
	assert(s.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(s.rate_changes == 0u);
}

/* Park (aius-ota-resilience.md Phase 2): one-way surrender of the storage so
 * the OTA activation may stage over the ring. From every state, park lands in
 * ABSENT and every later touch is the absent board's cheap refusal. */
static void test_park_is_one_way_from_every_state(void)
{
	Fixture f;

	assert(aius_recorder_park(NULL) == AIUS_INVALID_ARG);

	/* RECORDING -> parked: the next producer block is a no-op, so the ring
	 * and staging are ownerless the moment park returns. */
	fixture_init(&f);
	assert(aius_recorder_park(&f.r) == AIUS_OK);
	assert(f.r.state == AIUS_RECORDER_ABSENT);
	feed_block(&f, 4u, 100, 0u, 0u);
	assert(f.r.frames_staged == 0u);
	assert(aius_recorder_drain(&f.r, 65536u) == 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_INVALID_ARG);
	assert(aius_recorder_thaw(&f.r) == AIUS_INVALID_ARG);

	/* Mid-capture states park too: an activation request must not wait on
	 * a post-roll or a drain it is about to overwrite anyway. */
	fixture_init(&f);
	feed_block(&f, 4u, 100, 0u, 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block(&f, 4u, 200, 0u, 0u); /* token ack -> POSTROLL */
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(aius_recorder_park(&f.r) == AIUS_OK);
	assert(f.r.state == AIUS_RECORDER_ABSENT);

	/* FROZEN -> parked: a held capture is expendable by the same rule the
	 * ring is -- the activation was acknowledged. Two-block post-roll, the
	 * operator-freeze suite's recipe. */
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 2u) == AIUS_OK);
	feed_block(&f, 8u, 100, 0u, 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block(&f, 8u, 200, 0u, 0u); /* token ack -> POSTROLL */
	feed_block(&f, 8u, 300, 0u, 0u); /* post-roll ends -> DRAINING */
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	assert(aius_recorder_park(&f.r) == AIUS_OK);
	assert(f.r.state == AIUS_RECORDER_ABSENT);

	/* ABSENT parks idempotently -- the bare board and the double park. */
	assert(aius_recorder_park(&f.r) == AIUS_OK);
	assert(f.r.state == AIUS_RECORDER_ABSENT);
}

static void test_init_geometry(void)
{
	AiusRecorder r;
	static uint8_t ring[RING_BYTES];
	static int32_t staging[SLOTS * SLOT_FRAMES * 2u];
	static uint32_t staging_frames[SLOTS];
	/* Non-power-of-two ring frames refuse (the probe never produces one;
	 * a refusal beats silently wrong masking). */
	assert(aius_recorder_init(&r, ring, 24u, 16u, staging, staging_frames,
	                          SLOTS, SLOT_FRAMES,
	                          DEFAULT_RATE_HZ) == AIUS_INVALID_ARG);
	assert(r.state == AIUS_RECORDER_ABSENT);
	/* Unaligned byte size refuses. */
	assert(aius_recorder_init(&r, ring, RING_BYTES - 1u, 16u, staging,
	                          staging_frames, SLOTS, SLOT_FRAMES,
	                          DEFAULT_RATE_HZ) == AIUS_INVALID_ARG);
	/* Contract formula: ring_frames = psram_bytes / 8. */
	assert(aius_recorder_init(&r, ring, RING_BYTES, 16u, staging,
	                          staging_frames, SLOTS, SLOT_FRAMES,
	                          DEFAULT_RATE_HZ) == AIUS_OK);
	assert(r.ring_frames == RING_FRAMES);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&r, &s);
	assert(s.valid_frames == 0u);
	assert(s.oldest_frame == 0u);
	assert(s.trigger_mask == AIUS_RECORDER_MASK_DEFAULT);
	assert(s.postroll_blocks == AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS);
	/* Before any frame exists the report is the caller's configured
	 * nominal, and the init parameter is the ONLY source of it -- a second
	 * init at a different rate reports that one. */
	assert(s.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(s.rate_changes == 0u);
	assert(aius_recorder_init(&r, ring, RING_BYTES, 16u, staging,
	                          staging_frames, SLOTS, SLOT_FRAMES,
	                          ALT_RATE_HZ) == AIUS_OK);
	aius_recorder_snapshot(&r, &s);
	assert(s.sample_rate_hz == ALT_RATE_HZ);
}

static void test_target_psram_geometry(void)
{
	const uint32_t staging_bytes = 32u * 1024u;
	assert(aius_recorder_ring_bytes_after_staging(0u, staging_bytes) == 0u);
	assert(aius_recorder_ring_bytes_after_staging(staging_bytes,
	                                              staging_bytes) == 0u);
	assert(aius_recorder_ring_bytes_after_staging(8u * MIB,
	                                              staging_bytes) ==
	       4u * MIB);
	assert(aius_recorder_ring_bytes_after_staging(16u * MIB,
	                                              staging_bytes) ==
	       8u * MIB);
	assert(aius_recorder_ring_bytes_after_staging(UINT32_MAX, 0u) ==
	       0x80000000u);
}

static void test_eight_step_audio_burst_is_gap_free(void)
{
	AiusRecorder r;
	static uint8_t ring[AUDIO_BURST_RING_BYTES];
	static int32_t
		staging[AUDIO_BURST_SLOTS * AUDIO_BURST_SLOT_FRAMES * 2u];
	static uint32_t staging_frames[AUDIO_BURST_SLOTS];
	static int32_t left[AUDIO_BURST_SLOT_FRAMES];
	static int32_t right[AUDIO_BURST_SLOT_FRAMES];

	memset(&r, 0, sizeof(r));
	memset(ring, 0, sizeof(ring));
	assert(aius_recorder_init(&r, ring, sizeof(ring), 8u, staging,
	                          staging_frames, AUDIO_BURST_SLOTS,
	                          AUDIO_BURST_SLOT_FRAMES,
	                          DEFAULT_RATE_HZ) == AIUS_OK);
	for (uint32_t frame = 0; frame < AUDIO_BURST_SLOT_FRAMES; frame++) {
		left[frame] = (int32_t)frame;
		right[frame] = -(int32_t)frame;
	}
	for (uint32_t step = 0; step < AUDIO_BURST_SLOTS; step++)
		aius_recorder_producer_block(
			&r, left, right, AUDIO_BURST_SLOT_FRAMES,
			DEFAULT_RATE_HZ, 0u, 0u, false, 0.0f,
			1000u + step);
	assert(r.slot_write == AUDIO_BURST_SLOTS);
	assert(r.frames_staged == AUDIO_BURST_RING_FRAMES);
	assert(r.gap_count == 0u);
	aius_recorder_producer_block(
		&r, left, right, AUDIO_BURST_SLOT_FRAMES, DEFAULT_RATE_HZ, 0u,
		0u, false, 0.0f, 2000u);
	assert(r.gap_count == 1u);
	assert(r.frames_staged == AUDIO_BURST_RING_FRAMES);
	assert(aius_recorder_drain(&r, AUDIO_BURST_RING_BYTES) ==
	       AUDIO_BURST_RING_BYTES);
	assert(r.slot_read == AUDIO_BURST_SLOTS);
	assert(r.gap_count == 1u);
}

static void test_stage_drain_interleave(void)
{
	Fixture f;
	fixture_init(&f);
	feed_block(&f, 8u, 100, 0u, 0u);
	feed_block(&f, 8u, 200, 0u, 0u);
	assert(f.r.frames_staged == 16u);
	/* Budget cap: 8 frames' worth drains exactly one slot's frames. */
	assert(aius_recorder_drain(&f.r, 8u * AIUS_RECORDER_FRAME_BYTES) ==
	       8u * AIUS_RECORDER_FRAME_BYTES);
	assert(f.r.frames_drained == 8u);
	drain_all(&f);
	assert(f.r.frames_drained == 16u);
	/* Interleaved LR at the ring head, sample-exact. */
	const int32_t *words = (const int32_t *)f.ring;
	for (uint32_t i = 0; i < 8u; i++) {
		assert(words[2u * i] == 100 + (int32_t)i);
		assert(words[2u * i + 1u] == -(100 + (int32_t)i));
	}
	assert(words[2u * 8u] == 200);
	/* A partial-budget drain resumes mid-slot: 3 frames then the rest. */
	feed_block(&f, 8u, 300, 0u, 0u);
	assert(aius_recorder_drain(&f.r, 3u * AIUS_RECORDER_FRAME_BYTES) ==
	       3u * AIUS_RECORDER_FRAME_BYTES);
	assert(f.r.slot_read_offset == 3u);
	drain_all(&f);
	assert(f.r.slot_read_offset == 0u);
	assert(words[2u * 16u + 4u] == 300 + 2);
}

static void test_gap_on_staging_full(void)
{
	Fixture f;
	fixture_init(&f);
	/* Fill every slot without draining, then one more: dropped + stamped,
	 * never overwriting unconsumed staging. */
	for (uint32_t b = 0; b < SLOTS; b++)
		feed_block(&f, 8u, (int32_t)(b * 1000u), 0u, 0u);
	assert(f.r.frames_staged == SLOTS * 8u);
	feed_block(&f, 8u, 9000, 0u, 0u);
	assert(f.r.gap_count == 1u);
	assert(f.r.last_gap_frame == SLOTS * 8u);
	assert(f.r.frames_staged == SLOTS * 8u);
	drain_all(&f);
	const int32_t *words = (const int32_t *)f.ring;
	assert(words[0] == 0);
	assert(words[2u * 8u] == 1000);
	/* The blocks counter still advanced (the stream moved on). */
	assert(f.r.blocks_staged == SLOTS + 1u);
}

static void test_ring_wrap_and_extent(void)
{
	Fixture f;
	fixture_init(&f);
	/* 1024-frame ring, 8-frame blocks: 128 blocks fill it exactly; more
	 * wraps. Drain as we go so staging never gaps. */
	for (uint32_t b = 0; b < 130u; b++) {
		feed_block(&f, 8u, (int32_t)b, 0u, 0u);
		drain_all(&f);
	}
	assert(f.r.wrapped == 1u);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&f.r, &s);
	assert(s.valid_frames == RING_FRAMES);
	/* 130*8 = 1040; oldest = 1040 % 1024 = 16. */
	assert(s.oldest_frame == 16u);
	/* The wrapped head holds the newest frames, not the oldest. */
	const int32_t *words = (const int32_t *)f.ring;
	assert(words[0] == 128);
}

static void test_operator_freeze_ack_postroll(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 2u) == AIUS_OK);
	feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_OPERATOR);
	/* A second freeze while the token is pending is idempotent. */
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	/* The producer acknowledges at its next block: POSTROLL, token cleared,
	 * metadata latched at the pre-block boundary. */
	feed_block(&f, 8u, 1, 0u, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.freeze_token == 0u);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_OPERATOR);
	assert(f.r.trigger_at_frame == 8u);
	/* Post-roll counts the trigger block inclusive: one more block ends
	 * it. */
	feed_block(&f, 8u, 2, 0u, 0u);
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	assert(f.r.end_frame == 24u);
	assert(f.r.postroll_captured == 2u);
	/* Not readable until the drain flushes staging. */
	const uint8_t *p;
	uint32_t n;
	assert(aius_recorder_read(&f.r, 0u, 8u, 0u, &p, &n) ==
	       AIUS_INVALID_ARG);
	/* A block landing in DRAINING is ignored (stale-block rule). */
	feed_block(&f, 8u, 3, 0u, 0u);
	assert(f.r.frames_staged == 24u);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	assert(aius_recorder_read(&f.r, 0u, 24u * 8u, 0u, &p, &n) == AIUS_OK);
	assert(n == 24u * AIUS_RECORDER_FRAME_BYTES);
}

static void test_overrun_edge_and_resync(void)
{
	Fixture f;
	fixture_init(&f);
	/* First block adopts baselines: a nonzero starting counter is not an
	 * edge. */
	feed_block(&f, 8u, 0, 41u, 7u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* Unchanged: no edge. */
	feed_block(&f, 8u, 0, 41u, 7u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* A decrease is a counter clear: resync, never a freeze. */
	feed_block(&f, 8u, 0, 0u, 7u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* The next increase is the edge. */
	feed_block(&f, 8u, 0, 1u, 7u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_OVERRUN);
}

static void test_masked_trigger_tracks_baseline(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r,
	                                AIUS_RECORDER_MASK_DEFAULT &
	                                        ~AIUS_RECORDER_MASK_OVERRUN,
	                                0.0f, 1u, 2u) == AIUS_OK);
	feed_block(&f, 8u, 0, 0u, 0u);
	/* Overruns advance while masked: no freeze, baseline follows. */
	feed_block(&f, 8u, 0, 5u, 0u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* Re-enable: history must NOT fire; only a new edge does. */
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 2u) == AIUS_OK);
	feed_block(&f, 8u, 0, 5u, 0u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	feed_block(&f, 8u, 0, 6u, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
}

static void test_underrun_gate_qualification(void)
{
	Fixture f;
	fixture_init(&f);
	/* Gate closed (idle silence substitution free-runs the counter):
	 * underrun edges never fire. */
	aius_recorder_epoch(&f.r, false, false, 0u, 0u);
	feed_block(&f, 8u, 0, 0u, 0u);
	feed_block(&f, 8u, 0, 0u, 500u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* Gate opens: the transition re-adopts baselines and resets the settle
	 * counter, so the dark-era growth (0 -> 620) is NOT an edge... */
	aius_recorder_epoch(&f.r, false, true, 500u, 0u);
	feed_block(&f, 8u, 0, 0u, 620u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* ...and growth under the open gate would be, but the trigger stays
	 * suppressed until AIUS_RECORDER_SETTLE_BLOCKS clean blocks have
	 * staged since the edge (one already has, above). Drained inside the
	 * loop: the fixture's 4-slot staging would otherwise gap, and a gapped
	 * block is not a clean one. */
	for (uint32_t i = 1u; i < AIUS_RECORDER_SETTLE_BLOCKS; i++) {
		drain_all(&f);
		feed_block(&f, 8u, 0, 0u, 620u);
	}
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* Now settled: the next underrun growth fires. */
	feed_block(&f, 8u, 0, 0u, 740u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_UNDERRUN);
}

/* The debounce this settle counter exists for: field evidence shows the
 * spurious boot bring-up trigger firing at block 1, well after the gate's
 * own closed->open edge (a replay session opens the gate almost
 * immediately; an unrelated stall lands a couple of seconds later). Prove
 * a large underrun delta right after the edge does NOT fire, on both the
 * block-evaluable path and the epoch fallback -- an edge-only latch would
 * have missed this case entirely. */
static void test_underrun_suppressed_before_settled(void)
{
	Fixture f;
	fixture_init(&f);
	aius_recorder_epoch(&f.r, false, true, 0u, 0u);
	feed_block(&f, 8u, 0, 0u, 0u);
	/* One clean block staged (settled_blocks == 1); a large delta on the
	 * very next block must not fire despite the gate having been open
	 * since the first block. */
	feed_block(&f, 8u, 0, 0u, 5000u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
}

/* "Cleanly-staged" is the header's word and the code has to mean it: a block
 * that carried an underrun delta of its own, or that gapped, is not evidence
 * the stream has settled. Counting those as clean let a stream that was
 * stalling the entire time arm the underrun trigger anyway -- the exact
 * failure the window exists to prevent. The window is N CONSECUTIVE clean
 * blocks; a dirty one restarts it. */
static void test_settle_requires_consecutive_clean_blocks(void)
{
	Fixture f;
	fixture_init(&f);
	aius_recorder_epoch(&f.r, false, true, 0u, 0u);
	/* One clean block (it adopts the baselines, so it carries no delta). */
	feed_block(&f, 8u, 0, 0u, 0u);
	drain_all(&f);
	assert(f.r.settled_blocks == 1u);
	/* More than a full window of underrun-bearing blocks: never settles,
	 * so the trigger never arms, however long the stall runs. */
	uint32_t underruns = 0u;
	for (uint32_t i = 0; i < AIUS_RECORDER_SETTLE_BLOCKS + 1u; i++) {
		underruns += 10u;
		feed_block(&f, 8u, 0, 0u, underruns);
		drain_all(&f);
		assert(f.r.settled_blocks == 0u);
		assert(f.r.state == AIUS_RECORDER_RECORDING);
	}
	/* A gapped block is dirty too: fill staging, overflow one block, and
	 * the window restarts even though no underrun moved. */
	feed_block(&f, 8u, 0, 0u, underruns);
	assert(f.r.settled_blocks == 1u);
	for (uint32_t b = 1u; b < SLOTS; b++)
		feed_block(&f, 8u, 0, 0u, underruns);
	feed_block(&f, 8u, 0, 0u, underruns); /* staging full: dropped */
	assert(f.r.gap_count == 1u);
	assert(f.r.settled_blocks == 0u);
	drain_all(&f);
	/* A full window of genuinely clean blocks does settle... */
	for (uint32_t i = 0; i < AIUS_RECORDER_SETTLE_BLOCKS; i++) {
		feed_block(&f, 8u, 0, 0u, underruns);
		drain_all(&f);
	}
	assert(f.r.settled_blocks == AIUS_RECORDER_SETTLE_BLOCKS);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* ...and the same delta that was ignored throughout now fires. */
	feed_block(&f, 8u, 0, 0u, underruns + 10u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_UNDERRUN);
}

static void test_underrun_epoch_fallback_suppressed_before_settled(void)
{
	Fixture f;
	fixture_init(&f);
	aius_recorder_epoch(&f.r, false, true, 0u, 0u);
	feed_block(&f, 8u, 0, 0u, 0u);
	/* Gate has been open across two epochs (both ends of the fallback's
	 * qualification are satisfied) but only one block has staged. */
	aius_recorder_epoch(&f.r, false, true, 0u, 1u);
	aius_recorder_epoch(&f.r, false, true, 5000u, 2u);
	assert(f.r.freeze_token == 0u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
}

static void test_underrun_epoch_fallback_and_reclaim(void)
{
	Fixture f;
	fixture_init(&f);
	feed_block(&f, 8u, 0, 0u, 0u);
	/* Gate opens; settle with clean blocks before the fallback may arm
	 * (the field-observed boot-bring-up trigger fired well before any
	 * session had staged this many). */
	aius_recorder_epoch(&f.r, false, true, 0u, 100u);
	/* Drained after each block: the fixture's 4-slot staging would
	 * otherwise gap most of the settle run, which the settle counter
	 * itself doesn't care about (it counts evaluated blocks, not staged
	 * ones) but end_frame below does. */
	for (uint32_t i = 0; i < AIUS_RECORDER_SETTLE_BLOCKS; i++) {
		feed_block(&f, 8u, 0, 0u, 0u);
		drain_all(&f);
	}
	/* Producer stall: no more blocks, underruns advance, gate open.
	 * The epoch sets the token... */
	aius_recorder_epoch(&f.r, false, true, 0u, 150u);
	aius_recorder_epoch(&f.r, false, true, 4096u, 151u);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_UNDERRUN);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* ...and with the block counter unchanged, reclaims once
	 * AIUS_RECORDER_RECLAIM_MS has elapsed since the epoch that last saw
	 * the counter (150 ms above) -- not after any number of epochs. Sixty
	 * epochs in the next 50 ms leave it pending; the 280 ms mark takes it.
	 * The transition happens epoch-side with the token's identity. */
	for (uint32_t i = 0; i < 60u; i++)
		aius_recorder_epoch(&f.r, false, true, 4096u, 152u + i / 2u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&f.r, false, true, 4096u,
	                    150u + AIUS_RECORDER_RECLAIM_MS - 1u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&f.r, false, true, 4096u,
	                    150u + AIUS_RECORDER_RECLAIM_MS);
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_UNDERRUN);
	assert(f.r.end_frame == 8u * (1u + AIUS_RECORDER_SETTLE_BLOCKS));
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
}

/* The runtime's cadences, in microseconds, for the rows that drive the
 * reclaim window the way the board does: a 512-frame block every 10.67 ms at
 * 48 kHz, and an epoch every ~154 us (the ~6,500 epoch/s measured 2026-09-03,
 * against which the old 32-EPOCH window elapsed in ~4.9 ms, inside one block
 * period). now_ms is the truncated millisecond of the microsecond clock. */
#define RUNTIME_BLOCK_US 10667u
#define RUNTIME_EPOCH_US 154u

/* Run epochs at the runtime cadence until the clock reaches until_us. The
 * gate is open and the carrier never locks, so no epoch-side trigger can
 * fire: the only epoch-side transition on offer is the reclaim. */
static void run_epochs_until(Fixture *f, uint32_t *t_us, uint32_t until_us)
{
	while (*t_us < until_us) {
		aius_recorder_epoch(&f->r, false, true, 0u, *t_us / 1000u);
		*t_us += RUNTIME_EPOCH_US;
	}
}

/* THE BENCH DEFECT (2026-09-03, image f28a347d0a68): every operator freeze
 * read postroll_captured 0 against postroll_blocks 188, because ~69 epochs
 * run between two consecutive blocks and the reclaim counted 32 of them as
 * "the producer stopped". Drive the runtime's exact cadence and prove the
 * full post-roll is captured: the token is acknowledged by the producer
 * (not reclaimed by the epochs before it), POSTROLL survives every
 * inter-block gap, and the capture ends by the producer's count with
 * postroll_captured == postroll_blocks. */
static void test_postroll_survives_runtime_epoch_cadence(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS)
	       == AIUS_OK);
	uint32_t t_us = 0u;
	/* Steady recording for a few blocks first. */
	for (uint32_t b = 0; b < 4u; b++) {
		run_epochs_until(&f, &t_us, (b + 1u) * RUNTIME_BLOCK_US);
		feed_block(&f, 8u, 0, 0u, 0u);
		drain_all(&f);
	}
	uint32_t next_block_us = 5u * RUNTIME_BLOCK_US;
	/* Operator freeze lands mid-period; a full inter-block run of epochs
	 * follows before the producer's next block. */
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	run_epochs_until(&f, &t_us, next_block_us);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_OPERATOR);
	feed_block(&f, 8u, 0, 0u, 0u); /* acknowledges the token */
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_OPERATOR);
	assert(f.r.trigger_at_block == 4u);
	assert(f.r.postroll_captured == 1u); /* the acknowledging block counts */
	/* Every further post-roll block arrives after a full inter-block run of
	 * epochs, and none of those runs reclaims. */
	for (uint32_t b = 2u; b < AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS; b++) {
		next_block_us += RUNTIME_BLOCK_US;
		run_epochs_until(&f, &t_us, next_block_us);
		assert(f.r.state == AIUS_RECORDER_POSTROLL);
		feed_block(&f, 8u, 0, 0u, 0u);
		drain_all(&f);
		assert(f.r.postroll_captured == b);
	}
	next_block_us += RUNTIME_BLOCK_US;
	run_epochs_until(&f, &t_us, next_block_us);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	feed_block(&f, 8u, 0, 0u, 0u); /* the 188th: producer closes it */
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	assert(f.r.postroll_captured == AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS);
	assert(f.r.end_frame == f.r.frames_staged);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&f.r, &s);
	assert(s.postroll_captured == s.postroll_blocks);
}

/* The reclaim is still there for the case it exists for: the producer stops
 * mid-post-roll (the underrun and carrier-loss triggers fire exactly when
 * blocks may stop). It fires at AIUS_RECORDER_RECLAIM_MS after the epoch
 * that last saw the counter move -- not one millisecond before, however many
 * epochs run in between -- and keeps the blocks already captured. */
static void test_producer_stop_mid_postroll_reclaims_at_window(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS)
	       == AIUS_OK);
	feed_block(&f, 8u, 0, 0u, 0u);
	aius_recorder_epoch(&f.r, false, true, 0u, 1000u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block(&f, 8u, 0, 0u, 0u); /* acknowledges the token */
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	for (uint32_t b = 0; b < 10u; b++) {
		aius_recorder_epoch(&f.r, false, true, 0u, 1010u + b * 11u);
		feed_block(&f, 8u, 0, 0u, 0u);
		drain_all(&f);
	}
	assert(f.r.postroll_captured == 11u);
	/* The producer stops. This epoch is the last to see the counter move,
	 * so the window is measured from its now_ms. */
	aius_recorder_epoch(&f.r, false, true, 0u, 2000u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	/* Hundreds of epochs inside the window: still POSTROLL. */
	for (uint32_t ms = 2001u; ms < 2000u + AIUS_RECORDER_RECLAIM_MS; ms++) {
		aius_recorder_epoch(&f.r, false, true, 0u, ms);
		aius_recorder_epoch(&f.r, false, true, 0u, ms);
		assert(f.r.state == AIUS_RECORDER_POSTROLL);
	}
	aius_recorder_epoch(&f.r, false, true, 0u, 2000u + AIUS_RECORDER_RECLAIM_MS);
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	/* The reclaim closes the capture with what it has: the trigger and
	 * the captured count are the producer's, the end position is now. */
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_OPERATOR);
	assert(f.r.postroll_captured == 11u);
	assert(f.r.end_frame == f.r.frames_staged);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
}

/* A token with no producer at all (the carrier-loss and operator-with-no-
 * stream cases): reclaimed at the window, with the token's identity and an
 * empty post-roll, and not a millisecond earlier whatever the epoch count. */
static void test_pending_token_without_blocks_reclaims_at_window(void)
{
	Fixture f;
	fixture_init(&f);
	feed_block(&f, 8u, 0, 0u, 0u);
	drain_all(&f);
	/* Unarmed epoch: observes the counter at 1 and stamps the window. */
	aius_recorder_epoch(&f.r, false, true, 0u, 1000u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_OPERATOR);
	/* 100 epochs across the first 100 ms -- more than three of the old
	 * 32-epoch windows -- and the token is still pending. */
	for (uint32_t i = 0; i < 100u; i++) {
		aius_recorder_epoch(&f.r, false, true, 0u, 1001u + i);
		assert(f.r.state == AIUS_RECORDER_RECORDING);
	}
	aius_recorder_epoch(&f.r, false, true, 0u,
	                    1000u + AIUS_RECORDER_RECLAIM_MS - 1u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&f.r, false, true, 0u,
	                    1000u + AIUS_RECORDER_RECLAIM_MS);
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	assert(f.r.freeze_token == 0u);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_OPERATOR);
	assert(f.r.trigger_at_block == 1u);
	assert(f.r.trigger_at_ms == 1000u + AIUS_RECORDER_RECLAIM_MS);
	assert(f.r.postroll_captured == 0u);
	assert(f.r.end_frame == 8u);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
}

/* now_ms is a u32 millisecond counter and wraps every ~49.7 days; the
 * window is an unsigned difference, so a window that straddles 0xFFFFFFFF
 * is the same 280 ms as any other -- neither an instant reclaim (a signed
 * or saturating compare would see "now" far behind "since") nor a stall. */
static void test_reclaim_window_across_ms_wrap(void)
{
	Fixture f;
	fixture_init(&f);
	const uint32_t since = 0xFFFFFFFFu - 99u; /* 100 ms before the wrap */
	feed_block(&f, 8u, 0, 0u, 0u);
	aius_recorder_epoch(&f.r, false, true, 0u, since);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	aius_recorder_epoch(&f.r, false, true, 0u, 0xFFFFFFFFu);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&f.r, false, true, 0u, 0u); /* wrapped */
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* 279 ms elapsed, 179 of them past the wrap: still pending. */
	aius_recorder_epoch(&f.r, false, true, 0u,
	                    since + AIUS_RECORDER_RECLAIM_MS - 1u);
	assert(since + AIUS_RECORDER_RECLAIM_MS - 1u == 179u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&f.r, false, true, 0u,
	                    since + AIUS_RECORDER_RECLAIM_MS);
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	assert(f.r.trigger_at_ms == 180u);
	/* The mirror: a window that STARTS just after the wrap is not read as
	 * ~49.7 days old by an epoch a few ms later. */
	Fixture g;
	fixture_init(&g);
	feed_block(&g, 8u, 0, 0u, 0u);
	aius_recorder_epoch(&g.r, false, true, 0u, 5u);
	assert(aius_recorder_freeze(&g.r) == AIUS_OK);
	aius_recorder_epoch(&g.r, false, true, 0u, 6u);
	assert(g.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&g.r, false, true, 0u, 5u + AIUS_RECORDER_RECLAIM_MS);
	assert(g.r.state == AIUS_RECORDER_DRAINING);
}

/* A token set on the very first epoch after a thaw (a carrier edge the
 * inherited history still detects) must get a full window, not an instant
 * reclaim against a zero stamp: reset_capture leaves "nothing observed" and
 * the first epoch starts the window at its own now_ms. */
static void test_reclaim_window_starts_fresh_after_thaw(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 1u) == AIUS_OK);
	aius_recorder_epoch(&f.r, true, true, 0u, 50000u);
	feed_block(&f, 8u, 0, 0u, 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block(&f, 8u, 0, 0u, 0u);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	assert(aius_recorder_thaw(&f.r) == AIUS_OK);
	/* Carrier falls on the first post-thaw epoch: token set, window starts
	 * at 60000, not at 0. */
	aius_recorder_epoch(&f.r, false, true, 0u, 60000u);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&f.r, false, true, 0u,
	                    60000u + AIUS_RECORDER_RECLAIM_MS - 1u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	aius_recorder_epoch(&f.r, false, true, 0u,
	                    60000u + AIUS_RECORDER_RECLAIM_MS);
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
}

static void test_replay_stop_transient_suppressed(void)
{
	Fixture f;
	fixture_init(&f);
	feed_block(&f, 8u, 0, 0u, 0u);
	aius_recorder_epoch(&f.r, false, true, 0u, 0u);
	/* The stop closes the gate in the same evaluation that would see the
	 * drained-ring underruns: no token. */
	aius_recorder_epoch(&f.r, false, false, 900u, 1u);
	assert(f.r.freeze_token == 0u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
}

static void test_carrier_falling_edge(void)
{
	Fixture f;
	fixture_init(&f);
	feed_block(&f, 8u, 0, 0u, 0u);
	/* Bench norm: never locked, no edge, no freeze -- a level would fire
	 * here instantly. */
	for (uint32_t i = 0; i < 100u; i++)
		aius_recorder_epoch(&f.r, false, false, 0u, i);
	assert(f.r.freeze_token == 0u);
	/* Locked then lost: the falling edge sets the token; the producer
	 * acknowledges. */
	aius_recorder_epoch(&f.r, true, true, 0u, 200u);
	aius_recorder_epoch(&f.r, false, false, 0u, 201u);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
	feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
}

/* The governor-trim trigger (glitch-timestamping 2026-09-07): an audible splice
 * freezes a capture exactly as an underrun does, and the block that carries the
 * splice is the one the capture ends on. */
static void test_governor_trim_trigger(void)
{
	Fixture f;
	fixture_init(&f);
	/* Enabled by default: a board nobody has configured still catches the
	 * splice, which is the whole point of a diagnostic default. */
	assert((AIUS_RECORDER_MASK_DEFAULT &
	        AIUS_RECORDER_MASK_GOVERNOR_TRIM) != 0u);
	assert(f.r.trigger_mask == AIUS_RECORDER_MASK_DEFAULT);
	/* No settle window and no gate qualification: unlike an underrun, the
	 * caller has already decided this happened -- the governor's own
	 * multi-second window IS the settling -- so the very first block after
	 * a trim carries it. */
	aius_recorder_note_producer_event(&f.r,
	                                  AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	assert(f.r.freeze_token == 0u);
	/* Named on the wire; the values are appended, so no other trigger
	 * moved. */
	assert(AIUS_RECORDER_TRIGGER_OPERATOR == 5u);
	assert(AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM == 6u);
	assert(AIUS_RECORDER_TRIGGER_MISSED_FILL == 7u);
}

/* The missed-fill trigger rides the same producer-posted plumbing, and the one
 * entry point refuses to arm anything whose evaluation lives elsewhere. */
static void test_missed_fill_trigger(void)
{
	Fixture f;
	fixture_init(&f);
	assert((AIUS_RECORDER_MASK_DEFAULT &
	        AIUS_RECORDER_MASK_MISSED_FILL) != 0u);
	aius_recorder_note_producer_event(&f.r,
	                                  AIUS_RECORDER_TRIGGER_MISSED_FILL);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_MISSED_FILL);
	feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_MISSED_FILL);

	/* A trigger this entry point does not post is a no-op: the underrun and
	 * carrier edges have their own qualification and must not be armable by
	 * a caller that has evaluated nothing. */
	Fixture g;
	fixture_init(&g);
	aius_recorder_note_producer_event(&g.r,
	                                  AIUS_RECORDER_TRIGGER_UNDERRUN);
	aius_recorder_note_producer_event(&g.r,
	                                  AIUS_RECORDER_TRIGGER_CARRIER_LOSS);
	aius_recorder_note_producer_event(&g.r, AIUS_RECORDER_TRIGGER_OPERATOR);
	aius_recorder_note_producer_event(&g.r, AIUS_RECORDER_TRIGGER_NONE);
	assert(g.r.freeze_token == 0u);
	feed_block(&g, 8u, 0, 0u, 0u);
	assert(g.r.state == AIUS_RECORDER_RECORDING);
	/* A null recorder is a no-op like every other entry point. */
	aius_recorder_note_producer_event(NULL,
	                                  AIUS_RECORDER_TRIGGER_MISSED_FILL);
}

/* Masked off, the note is inert -- and a second trim during a post-roll never
 * displaces the capture already in flight. */
static void test_governor_trim_masked_and_first_wins(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r,
	                                AIUS_RECORDER_MASK_DEFAULT &
	                                        ~AIUS_RECORDER_MASK_GOVERNOR_TRIM,
	                                0.0f, 1u, 2u) == AIUS_OK);
	aius_recorder_note_producer_event(&f.r,
	                                  AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	assert(f.r.freeze_token == 0u);
	feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);

	/* Re-enabled: the first trim takes the capture, the second is counted
	 * against it rather than restarting it. */
	Fixture g;
	fixture_init(&g);
	assert(aius_recorder_set_config(&g.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 4u) == AIUS_OK);
	aius_recorder_note_producer_event(&g.r,
	                                  AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	feed_block(&g, 8u, 0, 0u, 0u);
	assert(g.r.state == AIUS_RECORDER_POSTROLL);
	/* POSTROLL: no token */
	aius_recorder_note_producer_event(&g.r,
	                                  AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	assert(g.r.freeze_token == 0u);
	feed_block(&g, 8u, 0, 0u, 0u);
	assert(g.r.trigger == AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM);
	assert(g.r.state == AIUS_RECORDER_POSTROLL);
}

static void test_gr_hold(void)
{
	Fixture f;
	fixture_init(&f);
	/* -6.02 dB is a linear gain of ~0.5; hold 3 blocks. */
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_ALL, -6.02f,
	                                3u, 2u) == AIUS_OK);
	int32_t plane[8] = { 0 };
	/* Two held blocks then a recovery: the hold count resets. */
	aius_recorder_producer_block(&f.r, plane, plane, 8u, DEFAULT_RATE_HZ, 0u,
	                        0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, DEFAULT_RATE_HZ, 0u,
	                        0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, DEFAULT_RATE_HZ, 0u,
	                        0u, true, 0.9f, 0u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* Three consecutive held blocks fire. */
	aius_recorder_producer_block(&f.r, plane, plane, 8u, DEFAULT_RATE_HZ, 0u,
	                        0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, DEFAULT_RATE_HZ, 0u,
	                        0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, DEFAULT_RATE_HZ, 0u,
	                        0u, true, 0.4f, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_LIMITER_GR);
	/* GR disabled (0 dB) never fires whatever the mask. */
	Fixture g;
	fixture_init(&g);
	assert(aius_recorder_set_config(&g.r, AIUS_RECORDER_MASK_ALL, 0.0f, 1u,
	                                2u) == AIUS_OK);
	for (uint32_t i = 0; i < 10u; i++)
		aius_recorder_producer_block(&g.r, plane, plane, 8u,
		                        DEFAULT_RATE_HZ, 0u, 0u, true, 0.01f,
		                        0u);
	assert(g.r.state == AIUS_RECORDER_RECORDING);
}

static void test_second_trigger_first_wins(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 4u) == AIUS_OK);
	/* Open the gate first and settle so the later underrun edge is
	 * qualified without a transition re-adoption or the settle window
	 * eating it. Drained inside the loop so no block gaps: a gapped block
	 * is not a clean one and would restart the window. */
	aius_recorder_epoch(&f.r, false, true, 0u, 0u);
	for (uint32_t i = 0; i < AIUS_RECORDER_SETTLE_BLOCKS; i++) {
		feed_block(&f, 8u, 0, 0u, 0u);
		drain_all(&f);
	}
	feed_block(&f, 8u, 0, 1u, 0u); /* overrun edge -> POSTROLL */
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_OVERRUN);
	/* An underrun edge and an operator freeze during the post-roll are
	 * counted, not taken. */
	feed_block(&f, 8u, 0, 1u, 50u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_OVERRUN);
	assert(f.r.suppressed_triggers == 2u);
}

static void test_thaw_and_generation(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 1u) == AIUS_OK);
	feed_block(&f, 8u, 7, 0u, 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block(&f, 8u, 8, 0u, 0u);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&f.r, &s);
	assert(s.valid_frames == 16u);
	assert(s.freeze_generation == 0u);
	/* Thaw: re-armed, generation bumped, capture state reset, config
	 * kept. */
	assert(aius_recorder_thaw(&f.r) == AIUS_OK);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	assert(f.r.freeze_generation == 1u);
	assert(f.r.frames_staged == 0u);
	assert(f.r.postroll_blocks == 1u);
	/* Counters kept advancing while FROZEN: the first block after thaw
	 * re-adopts baselines instead of edge-firing on the growth. */
	feed_block(&f, 8u, 0, 900u, 900u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	assert(aius_recorder_thaw(&f.r) == AIUS_INVALID_ARG);
}

static void test_read_bounds(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 1u) == AIUS_OK);
	feed_block(&f, 8u, 11, 0u, 0u);
	feed_block(&f, 8u, 12, 0u, 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block(&f, 8u, 13, 0u, 0u);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	const uint8_t *p;
	uint32_t n;
	/* 24 valid frames = 192 bytes. Whole-extent read clamps to it. */
	assert(aius_recorder_read(&f.r, 0u, 1u << 20, 0u, &p, &n) == AIUS_OK);
	assert(p == f.ring);
	assert(n == 24u * AIUS_RECORDER_FRAME_BYTES);
	/* Offset walks; the borrow is the ring itself. */
	assert(aius_recorder_read(&f.r, 8u * AIUS_RECORDER_FRAME_BYTES, 64u,
	                          0u, &p, &n) == AIUS_OK);
	assert(p == f.ring + 8u * AIUS_RECORDER_FRAME_BYTES);
	assert(n == 64u);
	/* Misaligned offset, stale generation, out-of-extent offset, zero
	 * length: refused. */
	assert(aius_recorder_read(&f.r, 4u, 8u, 0u, &p, &n) ==
	       AIUS_INVALID_ARG);
	assert(aius_recorder_read(&f.r, 0u, 8u, 1u, &p, &n) ==
	       AIUS_INVALID_ARG);
	assert(aius_recorder_read(&f.r, 24u * AIUS_RECORDER_FRAME_BYTES, 8u,
	                          0u, &p, &n) == AIUS_INVALID_ARG);
	assert(aius_recorder_read(&f.r, 0u, 0u, 0u, &p, &n) ==
	       AIUS_INVALID_ARG);
	/* The chunk cap clamps a larger ask. */
	Fixture g;
	fixture_init(&g);
	assert(aius_recorder_set_config(&g.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 0u) == AIUS_OK);
	for (uint32_t b = 0; b < RING_FRAMES / 8u; b++) {
		feed_block(&g, 8u, (int32_t)b, 0u, 0u);
		drain_all(&g);
	}
	assert(aius_recorder_freeze(&g.r) == AIUS_OK);
	feed_block(&g, 8u, 0, 0u, 0u); /* zero post-roll: token ack freezes */
	drain_all(&g);
	assert(g.r.state == AIUS_RECORDER_FROZEN);
	assert(aius_recorder_read(&g.r, 0u, RING_BYTES, 0u, &p, &n) ==
	       AIUS_OK);
	assert(n == AIUS_RECORDER_READ_MAX_BYTES ||
	       n == RING_BYTES); /* small fixture ring is under the cap */
	assert(n == RING_BYTES);
}

/* The rate the ring actually carries, which is the whole point: the status
 * route used to serve a compile-time nominal, so an accepted 44.1 kHz stream
 * produced a WAV header saying 48000 and a capture that played ~8.8% fast. */
static void test_rate_stamp_and_changes(void)
{
	Fixture f;
	fixture_init(&f);
	AiusRecorderSnapshot s;

	/* Before any frame: the configured default, and nothing has changed. */
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(s.rate_changes == 0u);

	/* The FIRST staged block adopts its own rate. Adopting the accepted
	 * live rate is not a change -- the default was a placeholder, not a
	 * predecessor -- so a single-rate 44.1 kHz capture stays rejection-free
	 * for a downloader that keys on rate_changes. */
	feed_block_at(&f, 8u, 100, ALT_RATE_HZ, 0u, 0u);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == ALT_RATE_HZ);
	assert(s.rate_changes == 0u);

	/* Same rate again: no change, whatever the block count. */
	feed_block_at(&f, 8u, 200, ALT_RATE_HZ, 0u, 0u);
	feed_block_at(&f, 8u, 300, ALT_RATE_HZ, 0u, 0u);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == ALT_RATE_HZ);
	assert(s.rate_changes == 0u);

	/* A real transition mid-capture: the ring is now mixed. The reported
	 * rate follows the newest frames and the counter says so. */
	drain_all(&f);
	feed_block_at(&f, 8u, 400, DEFAULT_RATE_HZ, 0u, 0u);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(s.rate_changes == 1u);
	/* Back again counts a second time: the count is transitions, not
	 * distinct rates. */
	feed_block_at(&f, 8u, 500, ALT_RATE_HZ, 0u, 0u);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == ALT_RATE_HZ);
	assert(s.rate_changes == 2u);
}

/* Only frames that reach staging may move the reported rate: a dropped block
 * contributes nothing to the ring, so it cannot relabel it. */
static void test_rate_stamp_ignores_dropped_blocks(void)
{
	Fixture f;
	fixture_init(&f);
	feed_block_at(&f, 8u, 0, DEFAULT_RATE_HZ, 0u, 0u);
	/* Fill the remaining slots without draining, then overflow at the other
	 * rate: the overflowing block is dropped and stamped as a gap. */
	for (uint32_t b = 1u; b < SLOTS; b++)
		feed_block_at(&f, 8u, (int32_t)(b * 1000u), DEFAULT_RATE_HZ, 0u,
		              0u);
	feed_block_at(&f, 8u, 9000, ALT_RATE_HZ, 0u, 0u);
	assert(f.r.gap_count == 1u);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(s.rate_changes == 0u);
	/* Drained, the same rate now stages and is adopted as a change against
	 * the frames already in the ring. */
	drain_all(&f);
	feed_block_at(&f, 8u, 9100, ALT_RATE_HZ, 0u, 0u);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == ALT_RATE_HZ);
	assert(s.rate_changes == 1u);

	/* A zero-frame block is not a commit either. */
	feed_block_at(&f, 0u, 0, DEFAULT_RATE_HZ, 0u, 0u);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == ALT_RATE_HZ);
	assert(s.rate_changes == 1u);
}

/* A thawed recorder is a fresh capture: it has staged no frame, so it reports
 * the configured default again and re-adopts without counting a change. */
static void test_rate_resets_on_thaw(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 1u) == AIUS_OK);
	feed_block_at(&f, 8u, 0, ALT_RATE_HZ, 0u, 0u);
	feed_block_at(&f, 8u, 1, DEFAULT_RATE_HZ, 0u, 0u);
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block_at(&f, 8u, 2, DEFAULT_RATE_HZ, 0u, 0u);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(s.rate_changes == 1u);
	/* The frozen capture keeps its verdict until it is thawed. */
	assert(aius_recorder_thaw(&f.r) == AIUS_OK);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == DEFAULT_RATE_HZ);
	assert(s.rate_changes == 0u);
	feed_block_at(&f, 8u, 3, ALT_RATE_HZ, 0u, 0u);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.sample_rate_hz == ALT_RATE_HZ);
	assert(s.rate_changes == 0u);
}

/* WHERE the rate changed, not just that it did: with the fiducials a host can
 * say how many old-rate frames a switch left in the ring (and cut the WAV
 * there); with the count alone it can only reject the whole capture. */
static void test_rate_change_fiducials(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 0u) == AIUS_OK);
	AiusRecorderSnapshot s;

	/* The first staged block adopts its rate without a change, so it leaves
	 * no fiducial -- the same rule rate_changes follows. */
	feed_block_at(&f, 8u, 0, DEFAULT_RATE_HZ, 0u, 0u);
	drain_all(&f);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.rate_change_count == 0u);
	assert(s.rate_change_frames[0] == 0u);

	/* Two changes at known offsets: 8-frame blocks, so the switch at the
	 * second block starts at stream frame 8 and the switch back at 24. */
	feed_block_at(&f, 8u, 0, ALT_RATE_HZ, 0u, 0u);
	drain_all(&f);
	feed_block_at(&f, 8u, 0, ALT_RATE_HZ, 0u, 0u); /* same rate: no change */
	drain_all(&f);
	feed_block_at(&f, 8u, 0, DEFAULT_RATE_HZ, 0u, 0u);
	drain_all(&f);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.rate_change_count == 2u);
	assert(s.rate_changes == s.rate_change_count);
	assert(s.rate_change_frames[0] == 8u);
	assert(s.rate_change_frames[1] == 24u);

	/* Fill the array (frames 32 and 40), then overflow it: the fifth change
	 * counts and nothing beyond the array is written. */
	feed_block_at(&f, 8u, 0, ALT_RATE_HZ, 0u, 0u);
	drain_all(&f);
	feed_block_at(&f, 8u, 0, DEFAULT_RATE_HZ, 0u, 0u);
	drain_all(&f);
	feed_block_at(&f, 8u, 0, ALT_RATE_HZ, 0u, 0u); /* the fifth, at 48 */
	drain_all(&f);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.rate_change_count == 5u);
	assert(s.rate_change_frames[2] == 32u);
	assert(s.rate_change_frames[3] == 40u);
	for (uint32_t i = 0; i < AIUS_RECORDER_RATE_CHANGE_MAX; i++)
		assert(s.rate_change_frames[i] != 48u);

	/* A thawed capture is a fresh one: the fiducials go with the rest of
	 * the extent, never carried into the next capture's index space. */
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block_at(&f, 8u, 0, ALT_RATE_HZ, 0u, 0u); /* zero post-roll */
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
	assert(aius_recorder_thaw(&f.r) == AIUS_OK);
	aius_recorder_snapshot(&f.r, &s);
	assert(s.rate_change_count == 0u);
	for (uint32_t i = 0; i < AIUS_RECORDER_RATE_CHANGE_MAX; i++)
		assert(s.rate_change_frames[i] == 0u);
}

static void test_config_validation(void)
{
	Fixture f;
	fixture_init(&f);
	/* Unknown mask bits, positive dB: refused. */
	assert(aius_recorder_set_config(&f.r, 1u << 7, 0.0f, 1u, 1u) ==
	       AIUS_INVALID_ARG);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_ALL, 3.0f, 1u,
	                                1u) == AIUS_INVALID_ARG);
	/* Post-roll clamps to the bound; hold clamps to >= 1. */
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_ALL, -1.0f,
	                                0u, 100000u) == AIUS_OK);
	assert(f.r.postroll_blocks == AIUS_RECORDER_POSTROLL_MAX_BLOCKS);
	assert(f.r.gr_hold_blocks == 1u);
	/* Config refuses while a capture is in flight. */
	assert(aius_recorder_freeze(&f.r) == AIUS_OK);
	feed_block(&f, 8u, 0, 0u, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 1u) == AIUS_INVALID_ARG);
}

int main(void)
{
	test_absent_is_first_class();
	test_park_is_one_way_from_every_state();
	test_init_geometry();
	test_target_psram_geometry();
	test_eight_step_audio_burst_is_gap_free();
	test_stage_drain_interleave();
	test_gap_on_staging_full();
	test_ring_wrap_and_extent();
	test_operator_freeze_ack_postroll();
	test_overrun_edge_and_resync();
	test_masked_trigger_tracks_baseline();
	test_underrun_gate_qualification();
	test_underrun_suppressed_before_settled();
	test_settle_requires_consecutive_clean_blocks();
	test_underrun_epoch_fallback_suppressed_before_settled();
	test_underrun_epoch_fallback_and_reclaim();
	test_postroll_survives_runtime_epoch_cadence();
	test_producer_stop_mid_postroll_reclaims_at_window();
	test_pending_token_without_blocks_reclaims_at_window();
	test_reclaim_window_across_ms_wrap();
	test_reclaim_window_starts_fresh_after_thaw();
	test_replay_stop_transient_suppressed();
	test_carrier_falling_edge();
	test_governor_trim_trigger();
	test_missed_fill_trigger();
	test_governor_trim_masked_and_first_wins();
	test_gr_hold();
	test_second_trigger_first_wins();
	test_thaw_and_generation();
	test_read_bounds();
	test_rate_stamp_and_changes();
	test_rate_stamp_ignores_dropped_blocks();
	test_rate_resets_on_thaw();
	test_rate_change_fiducials();
	test_config_validation();
	return 0;
}
