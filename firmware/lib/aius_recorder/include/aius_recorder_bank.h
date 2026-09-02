#ifndef AIUS_RECORDER_BANK_H
#define AIUS_RECORDER_BANK_H

#include <stdbool.h>
#include <stdint.h>

#include "aius_codec.h"
#include "aius_recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The flight recorder's rolling multi-slot bank
 * (docs/plans/aius-recorder-rolling-buffer.md, Phase B).
 *
 * A round-robin of AIUS_RECORDER_BANK_SLOTS independent AiusRecorder rings,
 * each a full copy of the existing single-writer-per-transition state
 * machine, unmodified. Only ONE slot is ever RECORDING/POSTROLL/DRAINING at a
 * time -- the bank itself is the second writer this adds, and its only job
 * is choosing which slot that is:
 *
 *   - producer/epoch calls forward to the active slot verbatim (no new
 *     per-block logic beyond the forward).
 *   - drain() forwards to the active slot, then -- if that slot just reached
 *     FROZEN -- advances to the next slot in the ring, UNLESS that slot has
 *     an outstanding HTTP read lease, in which case the advance defers one
 *     epoch at a time (retried on the next drain() call) rather than
 *     recycling a slot a response is still streaming from. The deferral is
 *     BOUNDED: after AIUS_RECORDER_RECLAIM_EPOCHS deferred drains the advance
 *     skips past the leased slot to the first candidate that is not leased
 *     (counted in lease_skips), leaving the leased capture intact for its
 *     reader; only when every other slot is leased too does it keep
 *     deferring. Deferred drains are counted in deferred_epochs.
 *   - a slot that was already FROZEN when the ring wraps back to it is
 *     recycled (thawed back to RECORDING) and counted in slots_recycled --
 *     an honest count of wraps, not a claim about whether the operator
 *     pulled it first (nothing marks a generation as read; see the plan's
 *     Design section for why that distinction is out of scope here).
 *   - the incoming slot INHERITS the outgoing slot's epoch-side edge history
 *     (aius_recorder_inherit_epoch_state), because the bank forwards epochs
 *     to the active slot only: without it a recycled slot edge-detects the
 *     carrier against whatever the stream looked like when it was last
 *     active, seconds ago, and fires a spurious CARRIER_LOSS (or misses a
 *     real one on a virgin slot).
 *
 * THE UNDERRUN TRIGGER IS BLIND FOR THE SETTLE WINDOW AFTER EVERY ADVANCE,
 * BY DESIGN. A slot re-arms with settled_blocks == 0 (reset_capture), so the
 * incoming capture cannot arm its underrun trigger until it has staged
 * AIUS_RECORDER_SETTLE_BLOCKS consecutive clean blocks -- about 85 ms at the
 * production 512-frame block, and longer if the stream is not clean. This is
 * the same debounce a boot or a manual thaw gets, applied for the same reason:
 * an underrun observed in the first blocks after a capture re-arms says more
 * about the transition than about the stream. The cost is real and named
 * here rather than discovered: a second underrun burst arriving inside that
 * window after a first one auto-advanced the bank is NOT captured by the
 * incoming slot (the outgoing slot's capture already holds the first burst).
 * Pinned by test_bank_underrun_blind_window_after_advance.
 *
 * Total ring bytes across all N slots is always exactly the single-ring
 * byte extent aius_recorder_ring_bytes_after_staging() would have produced
 * -- the OTA pad-1 capacity contract does not change shape. All N slots
 * share the caller's single staging carve (never concurrently touched,
 * since only the active slot's producer/drain ever run), so init binds the
 * same staging pointers into every slot.
 */

/* Compile-time slot count -- not runtime-configurable (see the plan's
 * Residual risks). Must be a power of two so ring_bytes / N stays a
 * power-of-two per-slot extent, matching what aius_recorder_init's own
 * geometry check requires. */
#define AIUS_RECORDER_BANK_SLOTS 4u

/* The production committed-block frame size the postroll-vs-slot-capacity
 * clamp in aius_recorder_bank_set_config() assumes (firmware always commits
 * 512-frame blocks; see aius_recorder.h's postroll comment). The portable
 * AiusRecorder core itself is block-size-agnostic -- this constant exists
 * only so the bank can keep an operator from configuring a postroll longer
 * than a slot can physically hold; a native test driving a different block
 * size should not treat this clamp as meaningful for its own arithmetic. */
#define AIUS_RECORDER_BANK_BLOCK_FRAMES 512u

typedef struct {
	AiusRecorder slots[AIUS_RECORDER_BANK_SLOTS];
	/* Index of the slot producer/epoch/drain forward to. Single epoch-side
	 * writer (advanced only from aius_recorder_bank_drain(), never from a
	 * producer context) -- the same "ONE WRITER PER TRANSITION" discipline
	 * aius_recorder.c documents for the single-ring state word, one level
	 * up. */
	uint32_t active;
	/* Per-slot outstanding HTTP read count. Incremented by
	 * aius_recorder_bank_read_acquire() when a chunk response borrows a
	 * pointer into that slot's ring, decremented by
	 * aius_recorder_bank_read_release() when the response completes. A
	 * slot with a nonzero count cannot be recycled -- see the module
	 * comment above. */
	uint32_t reads_in_flight[AIUS_RECORDER_BANK_SLOTS];
	/* Wraps of the round-robin: incremented each time drain() recycles a
	 * slot that was already FROZEN (as opposed to a virgin never-triggered
	 * slot from before the ring has wrapped once). Honest about what it
	 * measures -- see the module comment above. */
	uint32_t slots_recycled;
	/* Read-lease deferral accounting, for the status document.
	 * deferred_epochs is cumulative over the bank's life (every drain() that
	 * found the advance blocked by a lease); lease_skips counts the advances
	 * that ran out of deferral budget and skipped past a leased slot.
	 * defer_held is the CURRENT run only -- the consecutive deferred drains
	 * behind the present block, zeroed by every completed advance, and the
	 * value compared against AIUS_RECORDER_RECLAIM_EPOCHS. */
	uint32_t deferred_epochs;
	uint32_t lease_skips;
	uint32_t defer_held;
	/* Newest freeze, tracked at the moment drain() observes it so the
	 * status/HTTP layer has a "read this one" default that is not the
	 * active slot (the advance happens in the same drain that freezes, so
	 * the active slot is always the RECORDING one). Valid only while that
	 * slot is still FROZEN; aius_recorder_bank_newest_frozen() falls back to
	 * a generation scan once it isn't. */
	uint32_t newest_frozen;
	bool newest_frozen_valid;
	/* Config a slot refused because it was mid-capture, replayed onto that
	 * slot when it next thaws or is recycled. Without this a slot that was
	 * in POSTROLL/DRAINING when the operator reconfigured came back with the
	 * PREVIOUS config and nothing said so. One flag per slot: a slot that
	 * accepts a later config clears its own. */
	uint32_t config_pending_slots[AIUS_RECORDER_BANK_SLOTS];
	uint32_t pending_mask;
	float pending_gr_threshold_db;
	uint32_t pending_gr_hold_blocks;
	uint32_t pending_postroll_blocks;
} AiusRecorderBank;

/* Bind storage and arm every slot, or refuse the whole bank into ABSENT
 * (ring_bytes == 0 is the bare-board case, not an error, matching
 * aius_recorder_init). ring_bytes must be evenly divisible by
 * AIUS_RECORDER_BANK_SLOTS into a per-slot extent aius_recorder_init itself
 * accepts (a power-of-two multiple of AIUS_RECORDER_FRAME_BYTES) --
 * everything else is delegated verbatim to the per-slot init, N times, at
 * ascending base offsets into `ring`. staging/staging_frames/staging_slots/
 * slot_frames are the SAME shared carve passed to all N slots (safe: only
 * the active slot's producer/drain ever touch it). Call once, cold, before
 * the transmit interrupt arms.
 *
 * The per-slot default post-roll gets the same slot-capacity clamp
 * aius_recorder_bank_set_config() applies, so a bank whose slots are shorter
 * than AIUS_RECORDER_POSTROLL_DEFAULT_BLOCKS never arms with a post-roll it
 * cannot hold -- the default is as capable of overrunning a short slot as an
 * operator's value is. */
AiusStatus aius_recorder_bank_init(AiusRecorderBank *bank, uint8_t *ring,
                                   uint32_t ring_bytes, uint32_t fitted_mb,
                                   int32_t *staging, uint32_t *staging_frames,
                                   uint32_t staging_slots,
                                   uint32_t slot_frames,
                                   uint32_t default_sample_rate_hz);

/* True when the bank was bound to real storage (any slot is representative
 * -- all N transition ABSENT<->bound together, atomically from every
 * caller's perspective: park() and init() are both called from the single
 * epoch/controller context with no interleaving producer). */
bool aius_recorder_bank_present(const AiusRecorderBank *bank);

/* Producer, once per committed block: forwards verbatim to the active
 * slot. No new hot-path cost beyond the array index. */
void aius_recorder_bank_producer_block(AiusRecorderBank *bank,
                                       const int32_t *l, const int32_t *right,
                                       uint32_t frames,
                                       uint32_t sample_rate_hz,
                                       uint32_t overruns, uint32_t underruns,
                                       bool limiter_gain_valid,
                                       float limiter_gain, uint32_t now_ms);

/* Epoch, once per epoch before the drain: forwards verbatim to the active
 * slot. */
void aius_recorder_bank_epoch(AiusRecorderBank *bank, bool carrier_locked,
                              bool gate_open, uint32_t underruns,
                              uint32_t now_ms);

/* Epoch: drains the active slot (see aius_recorder_drain), then advances
 * the ring to the next slot if the active slot just reached FROZEN and the
 * next slot's read-lease count is zero. Returns the bytes drained from the
 * (pre-advance) active slot. This is the ONLY function that advances
 * `active`. */
uint32_t aius_recorder_bank_drain(AiusRecorderBank *bank, uint32_t max_bytes);

/* Epoch (operator): request a freeze of the active slot. Same semantics as
 * aius_recorder_freeze. */
AiusStatus aius_recorder_bank_freeze(AiusRecorderBank *bank);

/* Epoch (operator): thaw the active slot IN PLACE (no round-robin advance)
 * -- the same slot re-arms rather than the next one taking over. Preserves
 * the pre-bank `POST /api/recorder/thaw` contract for the case auto-advance
 * doesn't cover: forcing a FROZEN active slot back to RECORDING immediately
 * rather than waiting for the next slot's read lease to clear (or simply an
 * operator who wants the current slot back, not the next one). AIUS_OK only
 * if the active slot was FROZEN, same as aius_recorder_thaw.
 *
 * AIUS_REFUSED when the active slot has an outstanding read lease: thawing it
 * would recycle the ring bytes a response is still streaming from, which is
 * exactly what drain()'s deferral exists to prevent -- an operator thaw is no
 * more entitled to do that than the round-robin is. The caller retries once
 * the pull completes (or lets auto-advance take over). Distinct from the
 * AIUS_INVALID_ARG a non-FROZEN slot returns, so the caller can tell "not
 * frozen" from "frozen but busy". */
AiusStatus aius_recorder_bank_thaw(AiusRecorderBank *bank);

/* Cold, ONE-WAY: park every slot (any state -> ABSENT), same one-way
 * contract as aius_recorder_park, applied N times. Refuses only a null
 * argument. */
AiusStatus aius_recorder_bank_park(AiusRecorderBank *bank);

/* The most recently frozen slot that is STILL frozen, for a reader that wants
 * "the capture that just happened" -- true plus its index, or false when no
 * slot is frozen. This is the honest default for the data route: `active`
 * advances inside the same drain() that freezes, so reading the active slot
 * always hits a RECORDING one.
 *
 * Tracked at the freeze, so it is exact while that slot holds. Once it leaves
 * FROZEN (thaw, recycle, park) the answer falls back to the frozen slot with
 * the highest freeze_generation -- the one thawed, and therefore re-frozen,
 * most recently -- with ties broken in ring order backwards from the active
 * slot, which is newest-first. */
bool aius_recorder_bank_newest_frozen(const AiusRecorderBank *bank,
                                      uint32_t *slot);

/* Read-lease deferral counters and the pending-config flag, for the status
 * document: how many drains the round-robin has spent blocked behind a lease,
 * how many advances gave up waiting and skipped a leased slot, and whether any
 * slot is still carrying a config it refused while mid-capture. */
uint32_t aius_recorder_bank_deferred_epochs(const AiusRecorderBank *bank);
uint32_t aius_recorder_bank_lease_skips(const AiusRecorderBank *bank);
bool aius_recorder_bank_config_pending(const AiusRecorderBank *bank);

/* Epoch, cold: install trigger config on every slot (so a slot not yet
 * activated already carries it). postroll_blocks is additionally clamped to
 * the slot's own capacity in AIUS_RECORDER_BANK_BLOCK_FRAMES-sized blocks,
 * on top of AIUS_RECORDER_SET_CONFIG's existing AIUS_RECORDER_POSTROLL_MAX_BLOCKS
 * clamp -- a postroll longer than a slot can hold is nonsensical once slots
 * shrink from one ~21.8s ring to N shorter ones. A slot mid-capture (the
 * active one, if it happens to be in POSTROLL/DRAINING) keeps its own
 * config, matching aius_recorder_set_config's per-slot contract -- this is
 * expected, not an error, and is not surfaced as one: the config still
 * lands on every slot that can accept it. Returns AIUS_INVALID_ARG only for
 * a bad argument shape (mask bits, gr_threshold_db sign/finiteness), never
 * for an individual slot's state refusal.
 *
 * The refused slot does NOT keep the old config forever: the bank remembers
 * the config it refused and applies it the moment that slot thaws or is
 * recycled, so a reconfigure never silently survives one capture and then
 * comes back as the previous settings. aius_recorder_bank_config_pending()
 * reports the in-between state for the status document. */
AiusStatus aius_recorder_bank_set_config(AiusRecorderBank *bank, uint32_t mask,
                                         float gr_threshold_db,
                                         uint32_t gr_hold_blocks,
                                         uint32_t postroll_blocks);

/* Epoch: resolve one bounded chunk of a specific FROZEN slot's ring.
 * Delegates to aius_recorder_read on slots[slot]; AIUS_INVALID_ARG for
 * slot >= AIUS_RECORDER_BANK_SLOTS in addition to that function's own
 * refusals. Does NOT acquire the read lease -- callers hold it for the
 * whole response lifetime via acquire/release below, wrapping possibly
 * several chunk reads. */
AiusStatus aius_recorder_bank_read(const AiusRecorderBank *bank,
                                   uint32_t slot, uint32_t offset_bytes,
                                   uint32_t length_bytes, uint32_t generation,
                                   const uint8_t **out, uint32_t *out_length);

/* HTTP-layer read lease: acquire before borrowing a pointer into a slot's
 * ring for a response, release once that response completes. A slot with
 * an outstanding lease cannot be recycled by drain() -- see the module
 * comment. Both are cheap increments/decrements; out-of-range slot indices
 * are the caller's responsibility (mirroring aius_recorder_bank_read's own
 * bounds check happening at the same call site in practice). */
void aius_recorder_bank_read_acquire(AiusRecorderBank *bank, uint32_t slot);
void aius_recorder_bank_read_release(AiusRecorderBank *bank, uint32_t slot);

/* Epoch: coherent status snapshot for one slot (see aius_recorder_snapshot).
 * AIUS_INVALID_ARG for slot >= AIUS_RECORDER_BANK_SLOTS. */
AiusStatus aius_recorder_bank_snapshot_slot(const AiusRecorderBank *bank,
                                            uint32_t slot,
                                            AiusRecorderSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_RECORDER_BANK_H */
