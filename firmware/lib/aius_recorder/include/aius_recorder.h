#ifndef AIUS_RECORDER_H
#define AIUS_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The PSRAM flight recorder's portable core
 * (docs/plans/aius-optical-prototype.md §PSRAM flight recorder, prebuild-gated
 * 2026-07-27).
 *
 * A continuous circular recording of the output wire words that FREEZES on the
 * faults it exists to explain, preserving pre-trigger history. Two halves,
 * both host-testable with no Arduino dependency, extending the measure
 * machine's split:
 *
 *   1. The block producer (aius_recorder_producer_block): copies committed
 *      planes into caller-owned staging, evaluates block-evaluable triggers
 *      (overrun edge, underrun
 *      edge, limiter-GR hold), acknowledges epoch freeze tokens, and counts
 *      the post-roll. On target the caller-owned staging is the first 32 KiB
 *      of PSRAM; producer and drain are serialized in the AUDIO/epoch path.
 *
 *   2. The epoch side: sets freeze tokens for the epoch-detected triggers
 *      (carrier falling edge, operator, the underrun fallback), reclaims when
 *      the block producer stopped, and DRAINS staging into the PSRAM ring in
 *      bounded byte slices. The drain interleaves (planar staging ->
 *      interleaved LR ring) outside the producer call.
 *
 * RECORDING IS GAP-HONEST, NOT GAPLESS. Eight target slots hold the maximum
 * eight output steps one AUDIO checkpoint may commit before the next drain.
 * A longer stall fills them, and the producer then DROPS the block and stamps
 * a gap -- it never blocks and never overwrites unconsumed staging. Gap count
 * and last-gap position are status and frozen-header fields.
 *
 * ONE WRITER PER TRANSITION (the measure machine's discipline):
 *
 *   RECORDING -> POSTROLL   producer (block-evaluable trigger at a block
 *                                  boundary, or token acknowledgement;
 *                                  metadata stored before the state store)
 *   POSTROLL  -> DRAINING   producer (post-roll block count reached; end
 *                                  position latched first)
 *   RECORDING/POSTROLL
 *             -> DRAINING   epoch (reclaim: the block counter unchanged for
 *                                  AIUS_RECORDER_RECLAIM_MS of the caller's
 *                                  now_ms while a token is pending or
 *                                  post-roll is open -- the underrun and
 *                                  carrier-loss triggers fire precisely when
 *                                  blocks may stop)
 *   DRAINING  -> FROZEN     epoch (drain flushed the staging remnant; the
 *                                  capture is not readable before this)
 *   FROZEN    -> RECORDING  epoch (thaw; increments freeze_generation)
 *
 * The producer stages only in RECORDING and POSTROLL and ignores every other
 * state, so a stale block after an epoch-side transition is a no-op. ABSENT
 * (no fitted PSRAM) is terminal from init: every entry point but init is a
 * cheap refusal, and the caller's presence latch reduces the per-block cost
 * to one state load.
 *
 * PSRAM GEOMETRY. On target the first 32 KiB is staging and the ring is the
 * largest power-of-two byte extent after that carve: 4 MiB on an 8 MiB board,
 * 8 MiB on a 16 MiB board. Native tests use caller-owned buffers.
 * ring_frames = bytes / 8 (interleaved stereo s32le, 24-bit LSB-aligned).
 * PSRAM is never zeroed, so the valid extent (frames staged since arm,
 * saturating at the ring) bounds what a reader may serve: an unwrapped ring
 * never serves power-up garbage. */

/* Post-roll default and bound, in 512-frame blocks (~10.67 ms each):
 * 188 blocks is ~2.005 s, 938 is ~10.005 s. */
#define AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS 188u
#define AIUS_RECORDER_POSTROLL_MAX_BLOCKS 938u

/* One data chunk per HTTP request is capped here (frame-aligned): ~512
 * connections for a full 16 MB pull, clear of the measured FNET TIME_WAIT
 * cliff. */
#define AIUS_RECORDER_READ_MAX_BYTES 65536u

/* Interleaved stereo s32 frame. */
#define AIUS_RECORDER_FRAME_BYTES 8u

/* Milliseconds of the caller's now_ms with the producer block counter
 * unchanged before the controller reclaims a pending token or an open
 * post-roll. 280 ms is ~26 block periods at the 512-frame / 10.67 ms block: a
 * producer silent for that long has stopped (the stream stops in whole-block
 * units), while one that is merely between two consecutive blocks has not.
 *
 * A TIME, DELIBERATELY NOT AN EPOCH COUNT. The epoch rate is not a constant
 * of this design -- it is whatever the main loop's checkpoint cadence happens
 * to be, and it moved without this library noticing: the window was first
 * written as 32 epochs against a measured ~115 epoch/s (~280 ms), then the
 * runtime came to run ~6,500 epoch/s (2026-09-03, image f28a347d0a68), so 32
 * epochs elapsed in ~4.9 ms, inside one block period, and every freeze was
 * reclaimed between two consecutive blocks with postroll_captured 0. Measured
 * against now_ms the window means what it says whatever the loop does. The
 * comparison is an unsigned subtraction, so it is correct across the u32
 * millisecond wrap. */
#define AIUS_RECORDER_RECLAIM_MS 280u

/* Underrun-trigger settle window, in CONSECUTIVE cleanly-staged 512-frame
 * blocks under an open gate (~85 ms): a boot bring-up or post-thaw session
 * that hasn't demonstrated this many clean blocks yet cannot arm the underrun
 * trigger. Clean means the block fired no trigger, carried no underrun delta
 * of its own, and actually reached staging -- an underrun-bearing or gapped
 * block resets the window to zero rather than counting toward it, so a
 * continuously stalling stream never settles.
 * Field evidence (aius-open-work-register.md) shows the spurious trigger
 * firing at block 1 -- the gate is often already well past its own
 * closed->open edge by the time an unrelated stall (SD mount class) lands,
 * so gating on the edge alone does not suppress it; gating on N clean
 * blocks since the edge does, regardless of how much time separates them. */
#define AIUS_RECORDER_SETTLE_BLOCKS 8u

/* Per-capture rate-change fiducials retained for the host. A capture that
 * changes rate more often than this is already unusable as one WAV, so the
 * array holds the FIRST this many transitions and the count keeps counting. */
#define AIUS_RECORDER_RATE_CHANGE_MAX 4u

typedef enum {
	AIUS_RECORDER_ABSENT = 0,
	AIUS_RECORDER_RECORDING,
	AIUS_RECORDER_POSTROLL,
	AIUS_RECORDER_DRAINING,
	AIUS_RECORDER_FROZEN
} AiusRecorderState;

typedef enum {
	AIUS_RECORDER_TRIGGER_NONE = 0,
	AIUS_RECORDER_TRIGGER_OVERRUN,
	AIUS_RECORDER_TRIGGER_UNDERRUN,
	AIUS_RECORDER_TRIGGER_CARRIER_LOSS,
	AIUS_RECORDER_TRIGGER_LIMITER_GR,
	AIUS_RECORDER_TRIGGER_OPERATOR,
	/* The producer-posted triggers, appended rather than inserted: the
	 * value is a wire fact (GET /api/recorder serves its name) and every
	 * other one keeps its number.
	 *
	 * GOVERNOR_TRIM: the output-latency governor cut the transmit ring back
	 * to its target in one deliberate skip.
	 * MISSED_FILL: a transmit interrupt arrived a whole period late, so the
	 * DMA re-transmitted a stale half -- an audible repeat that advances no
	 * underrun counter.
	 * CRC_REJECT: a compressed access unit failed its own syncframe CRC and
	 * was replaced by silence rather than decoded. The capture is what says
	 * whether the source or the receiver produced the corruption, so it is
	 * worth freezing on: the ring holds the audio either side of the
	 * substituted unit. */
	AIUS_RECORDER_TRIGGER_GOVERNOR_TRIM,
	AIUS_RECORDER_TRIGGER_MISSED_FILL,
	AIUS_RECORDER_TRIGGER_CRC_REJECT
} AiusRecorderTrigger;

/* Automatic-trigger enable bits (the operator is never masked). Defaults at
 * boot: overrun, underrun, carrier, governor-trim, missed-fill and crc-reject
 * enabled; GR disabled until configured (a threshold has no universal
 * default). */
#define AIUS_RECORDER_MASK_OVERRUN (1u << 0)
#define AIUS_RECORDER_MASK_UNDERRUN (1u << 1)
#define AIUS_RECORDER_MASK_CARRIER_LOSS (1u << 2)
#define AIUS_RECORDER_MASK_LIMITER_GR (1u << 3)
#define AIUS_RECORDER_MASK_GOVERNOR_TRIM (1u << 4)
#define AIUS_RECORDER_MASK_MISSED_FILL (1u << 5)
#define AIUS_RECORDER_MASK_CRC_REJECT (1u << 6)
#define AIUS_RECORDER_MASK_DEFAULT \
	(AIUS_RECORDER_MASK_OVERRUN | AIUS_RECORDER_MASK_UNDERRUN | \
	 AIUS_RECORDER_MASK_CARRIER_LOSS | AIUS_RECORDER_MASK_GOVERNOR_TRIM | \
	 AIUS_RECORDER_MASK_MISSED_FILL | AIUS_RECORDER_MASK_CRC_REJECT)
#define AIUS_RECORDER_MASK_ALL \
	(AIUS_RECORDER_MASK_DEFAULT | AIUS_RECORDER_MASK_LIMITER_GR)

/* Trigger metadata, latched before the state store that makes it visible. */
typedef struct {
	uint32_t trigger;       /* AiusRecorderTrigger */
	uint32_t at_frame;      /* frames staged when the trigger latched */
	uint32_t at_block;      /* producer blocks staged when it latched */
	uint32_t at_ms;         /* caller's now_ms when it latched */
} AiusRecorderTriggerInfo;

/* Coherent epoch-context snapshot for the status serializer. */
typedef struct {
	uint32_t state;              /* AiusRecorderState */
	uint32_t fitted_mb;          /* as probed; 0 when absent */
	/* Rate of the most recently STAGED frame, and how many times the staged
	 * rate disagreed with its predecessor since the capture armed. Before any
	 * frame is staged the rate reports the caller's configured default and
	 * rate_changes is 0. A nonzero rate_changes means the ring spans a rate
	 * transition, so sample_rate_hz is true of only its newest frames and a
	 * downloader should reject the capture rather than write one WAV header
	 * over all of it. */
	uint32_t sample_rate_hz;
	uint32_t rate_changes;
	/* Where those changes happened, so a host can count how many old-rate
	 * frames a switch left in the ring instead of guessing. Each entry is the
	 * capture's own free-running stream index (frames staged since arm -- the
	 * same index space as the trigger's at_frame, and the one valid_frames
	 * measures and oldest_frame is the wrapped position of) of the FIRST frame
	 * carrying the new rate. rate_change_count is the total observed and is
	 * the same number rate_changes reports, under the name this array's
	 * contract uses: it may exceed AIUS_RECORDER_RATE_CHANGE_MAX, in which
	 * case the array holds the first that many and the rest are counted
	 * only. */
	uint32_t rate_change_frames[AIUS_RECORDER_RATE_CHANGE_MAX];
	uint32_t rate_change_count;
	uint32_t ring_frames;        /* capacity */
	uint32_t valid_frames;       /* extent a reader may serve */
	uint32_t oldest_frame;       /* physical ring index of the oldest frame */
	uint32_t freeze_generation;
	uint32_t gap_count;
	uint32_t last_gap_frame;
	uint32_t postroll_blocks;    /* configured */
	uint32_t postroll_captured;  /* blocks actually captured after trigger */
	uint32_t trigger_mask;
	float gr_threshold_db;       /* as configured (<= 0; 0 = unset) */
	uint32_t gr_hold_blocks;
	uint32_t suppressed_triggers;
	AiusRecorderTriggerInfo trigger; /* valid in POSTROLL/DRAINING/FROZEN */
} AiusRecorderSnapshot;

/* Caller-owned state. On target this struct lives in DTCM (never OCRAM: the
 * boot ROM scribbles it at reset); the staging planes and ring it points at
 * live in fitted PSRAM. Volatile fields preserve the portable producer /
 * controller handoff contract; every counter is u32 and the GR threshold is a
 * 32-bit float. */
typedef struct {
	volatile uint32_t state; /* AiusRecorderState */

	/* Geometry, written once at init. */
	uint8_t *ring;
	uint32_t ring_frames;
	uint32_t ring_mask;
	uint32_t fitted_mb;
	int32_t *staging;        /* slots * slot_frames * 2 samples, planar */
	uint32_t staging_slots;  /* power of two */
	uint32_t slot_frames;
	uint32_t *staging_frames; /* per-slot frame count, staging_slots long */

	/* Block-producer-owned. */
	volatile uint32_t slot_write;    /* free-running */
	volatile uint32_t frames_staged; /* free-running; ring pos = % ring */
	volatile uint32_t blocks_staged;
	volatile uint32_t gap_count;
	volatile uint32_t last_gap_frame;
	/* Rate truth for the frames actually in staging/ring. Stamped by the
	 * producer on the block it stages, never from a build-time nominal: the
	 * accepted live rate may be 44.1 kHz while the compile-time constant says
	 * 48 kHz, and a WAV header built from the latter plays the capture at the
	 * wrong speed. */
	volatile uint32_t sample_rate_hz;
	volatile uint32_t rate_changes;
	/* Stream index (frames_staged at the stamping block) of the first frame
	 * at each new rate, first AIUS_RECORDER_RATE_CHANGE_MAX only; rate_changes
	 * keeps counting past the array. */
	volatile uint32_t rate_change_frames[AIUS_RECORDER_RATE_CHANGE_MAX];
	volatile uint32_t suppressed_triggers;
	/* Triggers dropped because the slot held zero valid frames. Lifetime of
	 * the slot (zeroed by init only, never by a thaw), so the bank sums it. */
	volatile uint32_t empty_triggers_dropped;
	volatile uint32_t postroll_remaining;
	volatile uint32_t postroll_captured;
	volatile uint32_t end_frame;     /* frames_staged at POSTROLL exit */
	uint32_t overrun_base;
	uint32_t underrun_base;
	/* Underrun-trigger settle counter: CONSECUTIVE cleanly-staged blocks
	 * under an open gate since the last closed->open edge (or init/thaw),
	 * capped at AIUS_RECORDER_SETTLE_BLOCKS. The producer both increments it
	 * on a clean block and zeroes it on a dirty one; the epoch zeroes it on
	 * the gate edge. Two writers by design -- the same accepted shape as
	 * freeze_token/suppressed_triggers, and the collision is asymmetric: a
	 * lost producer increment costs one block's delay in arming, while a lost
	 * epoch reset leaves the pre-edge count standing and can arm up to
	 * AIUS_RECORDER_SETTLE_BLOCKS - 1 blocks EARLY -- a spurious trigger, the
	 * very thing this window exists to prevent, though still never a corrupt
	 * transition. Accepted because on this firmware both writers run from
	 * main-loop checkpoints (the AUDIO tap and the epoch/service checkpoint),
	 * never an interrupt, so the interleaving is theoretical. */
	volatile uint32_t settled_blocks;
	uint32_t gr_hold_count;
	/* Trigger metadata: stored before the POSTROLL state store. */
	volatile uint32_t trigger;
	volatile uint32_t trigger_at_frame;
	volatile uint32_t trigger_at_block;
	volatile uint32_t trigger_at_ms;

	/* Controller <-> block producer. */
	volatile uint32_t freeze_token;  /* AiusRecorderTrigger; 0 = none */

	/* Controller-owned (slot_read is volatile because the producer reads it for the
	 * staging-full check). */
	volatile uint32_t slot_read;     /* free-running */
	uint32_t slot_read_offset;       /* frames drained of the current slot */
	uint32_t frames_drained;         /* free-running */
	uint32_t freeze_generation;
	uint32_t underrun_base_epoch;
	/* Reclaim window: the producer block count last observed by the epoch
	 * and the now_ms at which it was first seen at that value. Reclaim fires
	 * once now_ms - reclaim_since_ms reaches AIUS_RECORDER_RECLAIM_MS with the
	 * count still unchanged. reset_capture leaves reclaim_blocks at UINT32_MAX
	 * ("nothing observed yet") so the first epoch after init/thaw starts the
	 * window at its own now_ms rather than at a stale or zero stamp. */
	uint32_t reclaim_blocks;
	uint32_t reclaim_since_ms;

	/* Config: written cold by set_config/init, read per producer block. */
	uint32_t default_sample_rate_hz;  /* reported until a frame is staged */
	volatile uint32_t trigger_mask;
	volatile float gr_threshold_gain; /* linear; 0 = disabled */
	float gr_threshold_db;            /* as configured, for echo only */
	volatile uint32_t gr_hold_blocks;
	volatile uint32_t postroll_blocks;

	/* 0/1 flags, packed into one word: this struct is instantiated per bank
	 * slot in DTCM, whose _ebss ceiling is committed, so a counter added
	 * (empty_triggers_dropped) is paid for here. Byte stores are independent
	 * on the target, so the producer-written and epoch-written flags do not
	 * share a read-modify-write. */
	volatile uint8_t wrapped;        /* producer; sticky: frames_staged reached ring */
	/* producer; 0 after init/thaw: the producer adopts the current counter
	 * values as baselines on its first block instead of edge-detecting
	 * against stale ones (counters keep advancing while FROZEN). */
	volatile uint8_t baseline_sync;
	volatile uint8_t gate_open;      /* epoch-latched tx-gate decision */
	uint8_t carrier_prev;            /* epoch; last carrier-lock sample */
} AiusRecorder;

/* Bind the storage and arm, or refuse into ABSENT. ring_bytes == 0 is the
 * bare-board case and is NOT an error: the recorder reports absent and every
 * other entry point refuses cleanly. ring_bytes must otherwise be a
 * power-of-two multiple of AIUS_RECORDER_FRAME_BYTES (the 8/16 MB probe
 * satisfies this); staging_slots a power of two; slot_frames nonzero.
 * Installs the default trigger config. Call once, cold, before the transmit
 * interrupt arms. AIUS_INVALID_ARG refuses into ABSENT with no storage
 * bound.
 *
 * default_sample_rate_hz is the caller's configured nominal, reported by the
 * snapshot until the first frame is staged and re-adopted on every thaw. It is
 * a parameter rather than a constant here so the one honest source of that
 * fact stays with the caller that owns it (the firmware's build config), with
 * no second copy in this library to drift from it. Not validated: an absent
 * board still echoes it, and a wrong nominal is a caller defect this library
 * cannot adjudicate. */
AiusStatus aius_recorder_init(AiusRecorder *r, uint8_t *ring,
                              uint32_t ring_bytes, uint32_t fitted_mb,
                              int32_t *staging, uint32_t *staging_frames,
                              uint32_t staging_slots, uint32_t slot_frames,
                              uint32_t default_sample_rate_hz);

/* Largest power-of-two ring byte extent that fits after a caller-owned staging
 * carve. Returns zero when no complete stereo frame fits. */
uint32_t aius_recorder_ring_bytes_after_staging(uint32_t storage_bytes,
                                                uint32_t staging_bytes);

/* Producer, once per committed block, after the planes are committed to the
 * transmit ring. In RECORDING/POSTROLL: evaluates the block-evaluable
 * triggers (overrun/underrun edges against resync-on-decrease baselines, the
 * underrun edge qualified by the epoch-latched gate; GR hold when
 * limiter_gain_valid), acknowledges a pending freeze token, copies the planes
 * into staging (or stamps a gap when staging is full), stamps that block's
 * sample_rate_hz on the capture, and counts down an open post-roll. In every
 * other state: one state load, then a no-op. Bounded and allocation-free.
 *
 * sample_rate_hz is THIS block's accepted rate. Only a block that actually
 * enters staging stamps it -- a dropped (gapped) block contributes no frame,
 * so it may not move the rate the ring reports. Costs one compare per block
 * and two stores on the rare edge. */
void aius_recorder_producer_block(AiusRecorder *r, const int32_t *l,
                             const int32_t *right, uint32_t frames,
                             uint32_t sample_rate_hz,
                             uint32_t overruns, uint32_t underruns,
                             bool limiter_gain_valid, float limiter_gain,
                             uint32_t now_ms);

/* Epoch, once per epoch BEFORE the drain: latches the tx-gate decision for
 * the producer's underrun qualification, edge-detects controller triggers (carrier
 * falling edge under the gate qualification; the underrun fallback under the
 * same latched gate, for the producer-stall case where no block is committed
 * again), sets the freeze token for them, and reclaims a pending token or
 * open post-roll once the block counter has been unchanged for
 * AIUS_RECORDER_RECLAIM_MS of now_ms (wrap-safe unsigned difference). */
void aius_recorder_epoch(AiusRecorder *r, bool carrier_locked, bool gate_open,
                         uint32_t underruns, uint32_t now_ms);

/* Epoch: drain up to max_bytes of staged frames into the PSRAM ring
 * (interleaving planar staging into LR frames). Returns the bytes written.
 * Performs DRAINING -> FROZEN once staging is empty in DRAINING. This is the
 * ONLY function that touches the ring storage. */
uint32_t aius_recorder_drain(AiusRecorder *r, uint32_t max_bytes);

/* Epoch (operator): request a freeze. From RECORDING sets the operator
 * token; from POSTROLL counts a suppressed trigger (first wins); otherwise
 * refuses. */
AiusStatus aius_recorder_freeze(AiusRecorder *r);

/* Producer: something the CALLER detected on the output path just happened --
 * a governor trim, or a missed transmit fill. Sets that trigger's token when it
 * is enabled and a capture is running with nothing already pending: the same
 * masked, first-token-wins, RECORDING-only rule the epoch's carrier-loss edge
 * uses, and deliberately not an evaluation of its own, because the caller owns
 * the decision -- it is the one that made the splice or lost the period.
 *
 * `which` must be one of the producer-posted triggers (GOVERNOR_TRIM,
 * MISSED_FILL, CRC_REJECT); anything else is a no-op, so a trigger with an
 * epoch-side or block-side evaluation can never be armed from here by mistake.
 *
 * Called from the producer immediately BEFORE the block that follows the event,
 * so that block's aius_recorder_producer_block() acknowledges the token and the
 * capture ends at the damaged block with the history before it intact --
 * exactly the shape an underrun freeze has. No status: a masked trigger, a
 * frozen slot and a second event inside one post-roll are all ordinary
 * outcomes, none of which the audio producer could act on. */
void aius_recorder_note_producer_event(AiusRecorder *r,
                                       AiusRecorderTrigger which);

/* Epoch: FROZEN -> RECORDING. Increments freeze_generation, resets the
 * capture (extent, gaps, trigger, post-roll) and re-arms. Config persists, and
 * so does the epoch-side EDGE HISTORY (carrier level, latched gate, epoch
 * underrun baseline): a thaw in place does not move the recorder to a
 * different point in the stream, so re-deriving that history from nothing
 * would manufacture an edge on the next epoch. Any other state:
 * AIUS_INVALID_ARG. */
AiusStatus aius_recorder_thaw(AiusRecorder *r);

/* Epoch: hand a recorder the epoch-side edge history (carrier level, latched
 * gate, epoch underrun baseline) of the one it is taking over from.
 *
 * The single-ring core edge-detects against its OWN last epoch, which is
 * exactly right while one recorder sees every epoch. A bank forwards each
 * epoch to its active slot only, so a slot re-armed mid-stream would otherwise
 * edge-detect against whatever the stream looked like when it was last active
 * -- history that may be seconds old and, in the carrier case, inverted: a
 * slot last active while locked keeps carrier_prev == 1 and fires a spurious
 * CARRIER_LOSS on its first epoch under an already-unlocked carrier, while a
 * virgin slot (carrier_prev == 0) silently misses a real falling edge on that
 * first epoch. Seeding the incoming slot from the outgoing one makes both
 * cases read the stream, not the slot's own past.
 *
 * Not a capture reset: it touches no producer-owned field and no state word,
 * so it is safe on any state and composes with thaw in either order. */
void aius_recorder_inherit_epoch_state(AiusRecorder *r,
                                       const AiusRecorder *from);

/* Epoch, cold: install trigger config. gr_threshold_db <= 0 (0 disables GR
 * whatever the mask says; the linear threshold is derived here, never in the
 * producer). Allowed in RECORDING and FROZEN only -- a capture in flight keeps
 * the config it latched. postroll_blocks is clamped to
 * AIUS_RECORDER_POSTROLL_MAX_BLOCKS. */
AiusStatus aius_recorder_set_config(AiusRecorder *r, uint32_t mask,
                                    float gr_threshold_db,
                                    uint32_t gr_hold_blocks,
                                    uint32_t postroll_blocks);

/* Epoch: resolve one bounded chunk of the frozen ring. FROZEN only;
 * generation must match freeze_generation; offset_bytes must be a multiple
 * of AIUS_RECORDER_FRAME_BYTES and inside the valid extent. *out points at
 * ring storage (PHYSICAL ring order -- the snapshot's oldest_frame tells the
 * host where the stream starts) and *out_length is
 * min(length_bytes, AIUS_RECORDER_READ_MAX_BYTES, extent - offset). The
 * borrow is stable for as long as the state stays FROZEN, which the caller
 * guarantees across its response lifetime (the single-transaction HTTP model:
 * a thaw is necessarily a later request). */
AiusStatus aius_recorder_read(const AiusRecorder *r, uint32_t offset_bytes,
                              uint32_t length_bytes, uint32_t generation,
                              const uint8_t **out, uint32_t *out_length);

/* Cold, ONE-WAY: surrender the storage. Any state -> ABSENT; the producer's
 * next block sees ABSENT and no-ops, every other entry point refuses, and
 * nothing in this library touches the ring or the staging slots again -- a
 * reset is the only way back to RECORDING (init runs again at boot).
 *
 * This exists for the OTA activation session (aius-ota-resilience.md
 * Phase 2): the staged image is placed over the pad-1 region the ring
 * normally owns, which is sound precisely because this call makes the ring
 * ownerless BEFORE the SD->PSRAM copy begins. The one-way shape matches the
 * activation quiesce it precedes -- a real activation ends in a reset, and a
 * rehearsal deliberately leaves audio (and now the recorder) down until the
 * reboot the operator already owes. Refuses only a null argument. */
AiusStatus aius_recorder_park(AiusRecorder *r);

/* Epoch: coherent status snapshot for the serializer. */
void aius_recorder_snapshot(const AiusRecorder *r, AiusRecorderSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_RECORDER_H */
