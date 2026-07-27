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
 *   1. The ISR side (aius_recorder_isr_block): copies the committed planes
 *      into a caller-owned DMAMEM staging ring in the same interrupt that owns
 *      them, evaluates the block-evaluable triggers (overrun edge, underrun
 *      edge, limiter-GR hold), acknowledges epoch freeze tokens, and counts
 *      the post-roll. EXTMEM is NEVER touched from this context.
 *
 *   2. The epoch side: sets freeze tokens for the epoch-detected triggers
 *      (carrier falling edge, operator, the underrun fallback), reclaims when
 *      the ISR provably stopped, and DRAINS staging into the PSRAM ring in
 *      bounded byte slices from thread context, where a FlexSPI2 stall costs
 *      nothing audible. The drain interleaves (planar staging -> interleaved
 *      LR ring) because the epoch loop is free and the ISR copy is not.
 *
 * RECORDING IS GAP-HONEST, NOT GAPLESS. Staging rides out the measured normal
 * worst case (~170 ms); a mount-class (~1 s) or watchdog-class epoch stall
 * fills it, and the ISR then DROPS the block and stamps a gap -- it never
 * blocks and never overwrites unconsumed staging. Gap count and last-gap
 * position are status and frozen-header fields.
 *
 * ONE WRITER PER TRANSITION (the measure machine's discipline):
 *
 *   RECORDING -> POSTROLL   ISR   (block-evaluable trigger at a block
 *                                  boundary, or token acknowledgement;
 *                                  metadata stored before the state store)
 *   POSTROLL  -> DRAINING   ISR   (post-roll block count reached; end
 *                                  position latched first)
 *   RECORDING/POSTROLL
 *             -> DRAINING   epoch (reclaim: the block counter unchanged for
 *                                  AIUS_RECORDER_RECLAIM_EPOCHS epochs while
 *                                  a token is pending or post-roll is open --
 *                                  the underrun and carrier-loss triggers
 *                                  fire precisely when blocks may stop)
 *   DRAINING  -> FROZEN     epoch (drain flushed the staging remnant; the
 *                                  capture is not readable before this)
 *   FROZEN    -> RECORDING  epoch (thaw; increments freeze_generation)
 *
 * The ISR stages only in RECORDING and POSTROLL and ignores every other
 * state, so a stale block after an epoch-side transition is a no-op. ABSENT
 * (no fitted PSRAM) is terminal from init: every entry point but init is a
 * cheap refusal, and the caller's presence latch reduces the per-block cost
 * to one state load.
 *
 * PSRAM GEOMETRY. The ring is raw storage handed to init -- base 0x70000000
 * sized by external_psram_size on target, a malloc'd buffer in the native
 * suite. ring_frames = bytes / 8 (interleaved stereo s32le, 24-bit
 * LSB-aligned) and must be a power of two, which the 8/16 MB probe results
 * are. PSRAM is never zeroed, so the valid extent (frames staged since
 * arm, saturating at the ring) bounds what a reader may serve: an unwrapped
 * ring never serves power-up garbage. */

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

/* Epochs with the ISR block counter unchanged before the epoch reclaims a
 * pending token or an open post-roll. Playback epochs run ~115/s, so this is
 * ~280 ms -- an ISR that silent for that long has provably stopped (the
 * stream stops in whole-block units). */
#define AIUS_RECORDER_RECLAIM_EPOCHS 32u

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
	AIUS_RECORDER_TRIGGER_OPERATOR
} AiusRecorderTrigger;

/* Automatic-trigger enable bits (the operator is never masked). Defaults at
 * boot: overrun, underrun and carrier enabled; GR disabled until configured
 * (a threshold has no universal default). */
#define AIUS_RECORDER_MASK_OVERRUN (1u << 0)
#define AIUS_RECORDER_MASK_UNDERRUN (1u << 1)
#define AIUS_RECORDER_MASK_CARRIER_LOSS (1u << 2)
#define AIUS_RECORDER_MASK_LIMITER_GR (1u << 3)
#define AIUS_RECORDER_MASK_DEFAULT \
	(AIUS_RECORDER_MASK_OVERRUN | AIUS_RECORDER_MASK_UNDERRUN | \
	 AIUS_RECORDER_MASK_CARRIER_LOSS)
#define AIUS_RECORDER_MASK_ALL \
	(AIUS_RECORDER_MASK_DEFAULT | AIUS_RECORDER_MASK_LIMITER_GR)

/* Trigger metadata, latched before the state store that makes it visible. */
typedef struct {
	uint32_t trigger;       /* AiusRecorderTrigger */
	uint32_t at_frame;      /* frames staged when the trigger latched */
	uint32_t at_block;      /* ISR blocks staged when it latched */
	uint32_t at_ms;         /* caller's now_ms when it latched */
} AiusRecorderTriggerInfo;

/* Coherent epoch-context snapshot for the status serializer. */
typedef struct {
	uint32_t state;              /* AiusRecorderState */
	uint32_t fitted_mb;          /* as probed; 0 when absent */
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

/* Caller-owned state. On target this struct lives in DTCM (never DMAMEM: the
 * boot ROM scribbles OCRAM at reset); the staging planes it points at live in
 * DMAMEM and the ring in EXTMEM. Fields crossing the ISR/epoch boundary are
 * volatile; 32-bit aligned loads and stores are single-copy atomic on the
 * Cortex-M7, which is why the GR threshold is stored as a 32-bit float and
 * every counter is u32. */
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

	/* ISR-owned. */
	volatile uint32_t slot_write;    /* free-running */
	volatile uint32_t frames_staged; /* free-running; ring pos = % ring */
	volatile uint32_t blocks_staged;
	volatile uint32_t wrapped;       /* sticky: frames_staged reached ring */
	volatile uint32_t gap_count;
	volatile uint32_t last_gap_frame;
	volatile uint32_t suppressed_triggers;
	volatile uint32_t postroll_remaining;
	volatile uint32_t postroll_captured;
	volatile uint32_t end_frame;     /* frames_staged at POSTROLL exit */
	uint32_t overrun_base;
	uint32_t underrun_base;
	/* 0 after init/thaw: the ISR adopts the current counter values as
	 * baselines on its first block instead of edge-detecting against stale
	 * ones (counters keep advancing while FROZEN). */
	volatile uint32_t baseline_sync;
	uint32_t gr_hold_count;
	/* Trigger metadata: stored before the POSTROLL state store. */
	volatile uint32_t trigger;
	volatile uint32_t trigger_at_frame;
	volatile uint32_t trigger_at_block;
	volatile uint32_t trigger_at_ms;

	/* Epoch <-> ISR. */
	volatile uint32_t freeze_token;  /* AiusRecorderTrigger; 0 = none */
	volatile uint32_t gate_open;     /* epoch-latched tx-gate decision */

	/* Epoch-owned (slot_read is volatile because the ISR reads it for the
	 * staging-full check). */
	volatile uint32_t slot_read;     /* free-running */
	uint32_t slot_read_offset;       /* frames drained of the current slot */
	uint32_t frames_drained;         /* free-running */
	uint32_t freeze_generation;
	uint32_t carrier_prev;
	uint32_t underrun_base_epoch;
	uint32_t reclaim_blocks;
	uint32_t reclaim_count;

	/* Config: written cold by set_config/init, read per block by the ISR. */
	volatile uint32_t trigger_mask;
	volatile float gr_threshold_gain; /* linear; 0 = disabled */
	float gr_threshold_db;            /* as configured, for echo only */
	volatile uint32_t gr_hold_blocks;
	volatile uint32_t postroll_blocks;
} AiusRecorder;

/* Bind the storage and arm, or refuse into ABSENT. ring_bytes == 0 is the
 * bare-board case and is NOT an error: the recorder reports absent and every
 * other entry point refuses cleanly. ring_bytes must otherwise be a
 * power-of-two multiple of AIUS_RECORDER_FRAME_BYTES (the 8/16 MB probe
 * satisfies this); staging_slots a power of two; slot_frames nonzero.
 * Installs the default trigger config. Call once, cold, before the transmit
 * interrupt arms. AIUS_INVALID_ARG refuses into ABSENT with no storage
 * bound. */
AiusStatus aius_recorder_init(AiusRecorder *r, uint8_t *ring,
                              uint32_t ring_bytes, uint32_t fitted_mb,
                              int32_t *staging, uint32_t *staging_frames,
                              uint32_t staging_slots, uint32_t slot_frames);

/* ISR, once per committed block, after the planes are committed to the
 * transmit ring. In RECORDING/POSTROLL: evaluates the block-evaluable
 * triggers (overrun/underrun edges against resync-on-decrease baselines, the
 * underrun edge qualified by the epoch-latched gate; GR hold when
 * limiter_gain_valid), acknowledges a pending freeze token, copies the planes
 * into staging (or stamps a gap when staging is full), and counts down an
 * open post-roll. In every other state: one state load, then a no-op.
 * Bounded, allocation-free, EXTMEM-free. */
void aius_recorder_isr_block(AiusRecorder *r, const int32_t *l,
                             const int32_t *right, uint32_t frames,
                             uint32_t overruns, uint32_t underruns,
                             bool limiter_gain_valid, float limiter_gain,
                             uint32_t now_ms);

/* Epoch, once per epoch BEFORE the drain: latches the tx-gate decision for
 * the ISR's underrun qualification, edge-detects the epoch triggers (carrier
 * falling edge under the gate qualification; the underrun fallback under the
 * same latched gate, for the producer-stall case where the ISR never runs
 * again), sets the freeze token for them, and reclaims a pending token or
 * open post-roll after AIUS_RECORDER_RECLAIM_EPOCHS epochs with the block
 * counter unchanged. */
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

/* Epoch: FROZEN -> RECORDING. Increments freeze_generation, resets the
 * capture (extent, gaps, trigger, post-roll) and re-arms. Config persists.
 * Any other state: AIUS_INVALID_ARG. */
AiusStatus aius_recorder_thaw(AiusRecorder *r);

/* Epoch, cold: install trigger config. gr_threshold_db <= 0 (0 disables GR
 * whatever the mask says; the linear threshold is derived here, never in the
 * ISR). Allowed in RECORDING and FROZEN only -- a capture in flight keeps
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

/* Epoch: coherent status snapshot for the serializer. */
void aius_recorder_snapshot(const AiusRecorder *r, AiusRecorderSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_RECORDER_H */
