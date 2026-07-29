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
	                          f->staging_frames, SLOTS,
	                          SLOT_FRAMES) == AIUS_OK);
	assert(f->r.state == AIUS_RECORDER_RECORDING);
}

/* One committed block whose samples encode (base + index) per channel so the
 * drain's interleave is checkable sample-exactly. */
static void feed_block(Fixture *f, uint32_t frames, int32_t base,
                       uint32_t overruns, uint32_t underruns)
{
	int32_t l[SLOT_FRAMES];
	int32_t r[SLOT_FRAMES];
	assert(frames <= SLOT_FRAMES);
	for (uint32_t i = 0; i < frames; i++) {
		l[i] = base + (int32_t)i;
		r[i] = -(base + (int32_t)i);
	}
	aius_recorder_producer_block(&f->r, l, r, frames, overruns, underruns,
	                        false, 0.0f, 1000u);
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
	assert(aius_recorder_init(&r, NULL, 0u, 0u, NULL, NULL, 0u, 0u) ==
	       AIUS_OK);
	assert(r.state == AIUS_RECORDER_ABSENT);
	assert(aius_recorder_freeze(&r) == AIUS_INVALID_ARG);
	assert(aius_recorder_thaw(&r) == AIUS_INVALID_ARG);
	const uint8_t *p;
	uint32_t n;
	assert(aius_recorder_read(&r, 0u, 8u, 0u, &p, &n) == AIUS_INVALID_ARG);
	assert(aius_recorder_drain(&r, 65536u) == 0u);
	int32_t plane[4] = { 0, 0, 0, 0 };
	aius_recorder_producer_block(&r, plane, plane, 4u, 0u, 0u, false, 0.0f, 0u);
	assert(r.frames_staged == 0u);
	/* Config is reportable and settable on an absent board (the status
	 * surface echoes it; there is just nothing to arm). */
	assert(aius_recorder_set_config(&r, AIUS_RECORDER_MASK_ALL, -6.0f, 2u,
	                                10u) == AIUS_OK);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&r, &s);
	assert(s.state == AIUS_RECORDER_ABSENT);
	assert(s.fitted_mb == 0u);
	assert(s.ring_frames == 0u);
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
	                          SLOTS, SLOT_FRAMES) == AIUS_INVALID_ARG);
	assert(r.state == AIUS_RECORDER_ABSENT);
	/* Unaligned byte size refuses. */
	assert(aius_recorder_init(&r, ring, RING_BYTES - 1u, 16u, staging,
	                          staging_frames, SLOTS,
	                          SLOT_FRAMES) == AIUS_INVALID_ARG);
	/* Contract formula: ring_frames = psram_bytes / 8. */
	assert(aius_recorder_init(&r, ring, RING_BYTES, 16u, staging,
	                          staging_frames, SLOTS,
	                          SLOT_FRAMES) == AIUS_OK);
	assert(r.ring_frames == RING_FRAMES);
	AiusRecorderSnapshot s;
	aius_recorder_snapshot(&r, &s);
	assert(s.valid_frames == 0u);
	assert(s.oldest_frame == 0u);
	assert(s.trigger_mask == AIUS_RECORDER_MASK_DEFAULT);
	assert(s.postroll_blocks == AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS);
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
	                          AUDIO_BURST_SLOT_FRAMES) == AIUS_OK);
	for (uint32_t frame = 0; frame < AUDIO_BURST_SLOT_FRAMES; frame++) {
		left[frame] = (int32_t)frame;
		right[frame] = -(int32_t)frame;
	}
	for (uint32_t step = 0; step < AUDIO_BURST_SLOTS; step++)
		aius_recorder_producer_block(
			&r, left, right, AUDIO_BURST_SLOT_FRAMES,
			0u, 0u, false, 0.0f,
			1000u + step);
	assert(r.slot_write == AUDIO_BURST_SLOTS);
	assert(r.frames_staged == AUDIO_BURST_RING_FRAMES);
	assert(r.gap_count == 0u);
	aius_recorder_producer_block(
		&r, left, right, AUDIO_BURST_SLOT_FRAMES, 0u, 0u,
		false, 0.0f, 2000u);
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
	/* Gate opens: the transition re-adopts baselines, so the dark-era
	 * growth (0 -> 620) is NOT an edge... */
	aius_recorder_epoch(&f.r, false, true, 500u, 0u);
	feed_block(&f, 8u, 0, 0u, 620u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* ...and growth under the open gate is. */
	feed_block(&f, 8u, 0, 0u, 740u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_UNDERRUN);
}

static void test_underrun_epoch_fallback_and_reclaim(void)
{
	Fixture f;
	fixture_init(&f);
	feed_block(&f, 8u, 0, 0u, 0u);
	/* Producer stall: no more blocks, underruns advance, gate open.
	 * The epoch sets the token... */
	aius_recorder_epoch(&f.r, false, true, 0u, 100u);
	aius_recorder_epoch(&f.r, false, true, 4096u, 101u);
	assert(f.r.freeze_token == AIUS_RECORDER_TRIGGER_UNDERRUN);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* ...and with the block counter unchanged, reclaims after the bound:
	 * the transition happens epoch-side with the token's identity. */
	for (uint32_t i = 0; i < AIUS_RECORDER_RECLAIM_EPOCHS; i++)
		aius_recorder_epoch(&f.r, false, true, 4096u, 102u + i);
	assert(f.r.state == AIUS_RECORDER_DRAINING);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_UNDERRUN);
	assert(f.r.end_frame == 8u);
	drain_all(&f);
	assert(f.r.state == AIUS_RECORDER_FROZEN);
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

static void test_gr_hold(void)
{
	Fixture f;
	fixture_init(&f);
	/* -6.02 dB is a linear gain of ~0.5; hold 3 blocks. */
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_ALL, -6.02f,
	                                3u, 2u) == AIUS_OK);
	int32_t plane[8] = { 0 };
	/* Two held blocks then a recovery: the hold count resets. */
	aius_recorder_producer_block(&f.r, plane, plane, 8u, 0u, 0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, 0u, 0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, 0u, 0u, true, 0.9f, 0u);
	assert(f.r.state == AIUS_RECORDER_RECORDING);
	/* Three consecutive held blocks fire. */
	aius_recorder_producer_block(&f.r, plane, plane, 8u, 0u, 0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, 0u, 0u, true, 0.4f, 0u);
	aius_recorder_producer_block(&f.r, plane, plane, 8u, 0u, 0u, true, 0.4f, 0u);
	assert(f.r.state == AIUS_RECORDER_POSTROLL);
	assert(f.r.trigger == AIUS_RECORDER_TRIGGER_LIMITER_GR);
	/* GR disabled (0 dB) never fires whatever the mask. */
	Fixture g;
	fixture_init(&g);
	assert(aius_recorder_set_config(&g.r, AIUS_RECORDER_MASK_ALL, 0.0f, 1u,
	                                2u) == AIUS_OK);
	for (uint32_t i = 0; i < 10u; i++)
		aius_recorder_producer_block(&g.r, plane, plane, 8u, 0u, 0u, true,
		                        0.01f, 0u);
	assert(g.r.state == AIUS_RECORDER_RECORDING);
}

static void test_second_trigger_first_wins(void)
{
	Fixture f;
	fixture_init(&f);
	assert(aius_recorder_set_config(&f.r, AIUS_RECORDER_MASK_DEFAULT, 0.0f,
	                                1u, 4u) == AIUS_OK);
	/* Open the gate first so the later underrun edge is qualified without
	 * a transition re-adoption eating it. */
	aius_recorder_epoch(&f.r, false, true, 0u, 0u);
	feed_block(&f, 8u, 0, 0u, 0u);
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
	test_underrun_epoch_fallback_and_reclaim();
	test_replay_stop_transient_suppressed();
	test_carrier_falling_edge();
	test_gr_hold();
	test_second_trigger_first_wins();
	test_thaw_and_generation();
	test_read_bounds();
	test_config_validation();
	return 0;
}
