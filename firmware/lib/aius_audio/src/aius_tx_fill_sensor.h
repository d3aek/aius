#ifndef AIUS_TX_FILL_SENSOR_H
#define AIUS_TX_FILL_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Phase-corrected transmit-ring fill statistic (aius-sample-rate-following
 * Phase 3), pure and portable, observed only.
 *
 * OCCUPANCY. The ring level only moves in 128-frame TX periods, so it is
 * corrected by the DMA read position: fill() hands each DMA half one period
 * ahead, so the unsent frames are the ring level, 128 in the next half and
 * (1 - phi) * 128 still being read from the current one:
 *     occupancy = level + 256 - phi * 128,  phi * 128 = DMA offset in frames.
 * An empty ring reads 256 - phi * 128 (128 to 256), never wrapped; the
 * arithmetic is signed as a defence only.
 *
 * STATISTIC. A 192,000-frame window of committed frames with its own
 * boundaries (not the governor's), split into 16 sub-windows of 12,000
 * frames. A sub-window's value is the minimum occupancy over its pre-commit
 * samples; the window statistic is the mean of the non-excluded sub-window
 * minima, rounded to 1/16 frame. Fewer than 8 valid sub-windows: invalid.
 * A sub-window is excluded (counted) by an underrun, a missed fill, a
 * production stall, or having no sample. An invalid window, like a reset,
 * serves null (spread 0) until the next valid window; `fill_windows` counts
 * valid windows only.
 *
 * FLASHMEM: this header is compiled into cold (flash) objects only; define
 * AIUS_TX_FILL_SENSOR_COLD as FLASHMEM at the including object if wanted. */

#ifndef AIUS_TX_FILL_SENSOR_COLD
#define AIUS_TX_FILL_SENSOR_COLD
#endif

#define AIUS_TX_FILL_SUBWINDOW_FRAMES 12000u
#define AIUS_TX_FILL_SUBWINDOWS 16u
#define AIUS_TX_FILL_MIN_VALID_SUBWINDOWS 8u
#define AIUS_TX_FILL_STALL_ALLOWANCE_US 10000u
#define AIUS_TX_FILL_HALF_FRAMES 128
#define AIUS_TX_FILL_MAX_ATTEMPTS 3u

/* Exclusion reasons (equal to AIUS_CLOCK_FILL_EXCL_*). NO_SAMPLE is set when a
 * sub-window closes with cur_min INT32_MAX. */
#define AIUS_TX_FILL_EXCL_UNDERRUN 0x01u
#define AIUS_TX_FILL_EXCL_STALL 0x02u
#define AIUS_TX_FILL_EXCL_MISSED_FILL 0x04u
#define AIUS_TX_FILL_EXCL_NO_SAMPLE 0x08u

/* Reader callbacks for the torn-read retry; the host suite injects tears.
 * `fills` is the ring's fills counter, `level` its frame level,
 * `dma_offset_frames` the TX DMA read offset into the current half (frames,
 * 0..127 from the SADDR mapping; the occupancy arithmetic itself clamps a
 * larger value to 128 as a defence). */
typedef struct {
	void *ctx;
	uint32_t (*fills)(void *ctx);
	uint32_t (*level)(void *ctx);
	uint32_t (*dma_offset_frames)(void *ctx);
} AiusTxFillReader;

/* Observability fields, exactly as served on `clock`. */
typedef struct {
	int32_t fill_level_q4;                 /* 1/16 frame; meaningful iff valid */
	int fill_level_valid;                  /* 0 serves null */
	uint32_t fill_subwindow_spread_frames; /* max minus min of the minima */
	uint32_t fill_windows;                 /* valid windows published */
	uint32_t fill_subwindows_excluded;     /* lifetime */
	uint32_t fill_torn_reads;              /* samples dropped after 3 tears */
	uint32_t fill_window_seq;              /* every window close, valid or not; lifetime */
	uint32_t fill_window_exclusions;       /* OR of reason bits over the last window */
} AiusTxFillSnapshot;

typedef struct {
	/* window state (emptied by reset) */
	int32_t sub_min_q4[AIUS_TX_FILL_SUBWINDOWS]; /* INT32_MAX = excluded */
	uint32_t subs_closed;
	int32_t cur_min_q4;      /* INT32_MAX = no sample yet */
	uint32_t cur_frames;
	int cur_excluded;
	uint32_t cur_reasons;    /* reason bits, current sub-window */
	uint32_t win_reasons;    /* OR of closed sub-windows' bits, current window */
	int have_last_block;
	uint32_t last_commit_us;
	uint32_t last_unit_frames;
	int latch_valid;
	uint32_t underruns_latch;
	uint32_t missed_fills_latch;
	/* published */
	AiusTxFillSnapshot snap;
} AiusTxFillSensor;

AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_clear_window(AiusTxFillSensor *s)
{
	s->subs_closed = 0u;
	s->cur_min_q4 = INT32_MAX;
	s->cur_frames = 0u;
	s->cur_excluded = 0;
	s->cur_reasons = 0u;
	s->win_reasons = 0u;
	s->have_last_block = 0;
	for (uint32_t i = 0u; i < AIUS_TX_FILL_SUBWINDOWS; i++)
		s->sub_min_q4[i] = INT32_MAX;
}

AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_init(AiusTxFillSensor *s)
{
	aius_tx_fill_sensor_clear_window(s);
	s->latch_valid = 0;
	s->snap.fill_level_q4 = 0;
	s->snap.fill_level_valid = 0;
	s->snap.fill_subwindow_spread_frames = 0u;
	s->snap.fill_windows = 0u;
	s->snap.fill_subwindows_excluded = 0u;
	s->snap.fill_torn_reads = 0u;
	s->snap.fill_window_seq = 0u;
	s->snap.fill_window_exclusions = 0u;
}

/* Gate close, governor trim, carrier loss, replay block: empty the window and
 * serve null until the next valid window. Lifetime counters survive. */
AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_reset(AiusTxFillSensor *s)
{
	aius_tx_fill_sensor_clear_window(s);
	s->latch_valid = 0;
	s->snap.fill_level_valid = 0;
	s->snap.fill_level_q4 = 0;
	s->snap.fill_subwindow_spread_frames = 0u;
	s->snap.fill_window_exclusions = 0u;
}

/* Governor trim: empties the window like reset but KEEPS the fault latches.
 * observe_faults already latched this block's counters before the trim, so
 * clearing them would swallow a fault between this block and the next. */
AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_reset_keep_latches(AiusTxFillSensor *s)
{
	aius_tx_fill_sensor_clear_window(s);
	s->snap.fill_level_valid = 0;
	s->snap.fill_level_q4 = 0;
	s->snap.fill_subwindow_spread_frames = 0u;
	s->snap.fill_window_exclusions = 0u;
}

/* Convenience for the tap: resets and returns 1 when the sensor must not
 * sample this block (gate closed, carrier lost, replay block). */
AIUS_TX_FILL_SENSOR_COLD static inline int
aius_tx_fill_sensor_hold(AiusTxFillSensor *s, int gate_open,
                         int carrier_present, int replay_block)
{
	if (gate_open && carrier_present && !replay_block)
		return 0;
	aius_tx_fill_sensor_reset(s);
	return 1;
}

/* TX DMA buffer geometry: 2,048 bytes, two halves of 1,024, 8 bytes a frame. */
#define AIUS_TX_FILL_BUFFER_BYTES 2048u
#define AIUS_TX_FILL_FRAME_BYTES 8u

/* TX DMA SADDR (byte address) to frame offset within the half being read.
 * Same half test as the ISR: saddr below base + half selects the first half.
 * Result 0..127; an address outside the buffer (never produced by the DMA)
 * clamps to 127 as a defence. */
AIUS_TX_FILL_SENSOR_COLD static inline uint32_t
aius_tx_fill_saddr_offset_frames(uint32_t saddr, uint32_t base)
{
	const uint32_t half = AIUS_TX_FILL_BUFFER_BYTES / 2u;
	uint32_t bytes = saddr - base;
	if (saddr >= base + half)
		bytes -= half;
	const uint32_t frames = bytes / AIUS_TX_FILL_FRAME_BYTES;
	return frames < (uint32_t)AIUS_TX_FILL_HALF_FRAMES
		? frames : (uint32_t)AIUS_TX_FILL_HALF_FRAMES - 1u;
}

/* Production unit (frames) the stall test measures a gap against, by IEC
 * 61937 repetition period of the burst's Pc: AC-3 (1) 1,536; DTS types I/II/III
 * (11/12/13) 512/1,024/2,048; E-AC-3 (21) 6,144. PCM is the block itself; an
 * unknown compressed Pc is 2,048.
 * Known limit: E-AC-3 6,144 and unknown-compressed 2,048 make the stall
 * threshold lenient (up to 138 ms) for sources sending shorter units; it errs
 * toward inclusion. */
AIUS_TX_FILL_SENSOR_COLD static inline uint32_t
aius_tx_fill_unit_frames(uint32_t pc, uint32_t block_frames, int is_pcm)
{
	if (is_pcm)
		return block_frames;
	switch (pc) {
	case 1u: return 1536u;
	case 11u: return 512u;
	case 12u: return 1024u;
	case 13u: return 2048u;
	case 21u: return 6144u;
	default: return 2048u;
	}
}

/* Opening rules (Phase 4), pure. Always-inline and not COLD: the callers are
 * a FASTRUN accessor and the masked gate open, which must not run from flash. */
#define AIUS_TX_FILL_INLINE static inline __attribute__((always_inline))

/* A period boundary is pending when SADDR lies in the half the ISR last filled
 * (the DMA already reads it; the half it just played is stale). An address
 * outside the ring is never pending. */
AIUS_TX_FILL_INLINE bool
aius_tx_fill_boundary_pending(uint32_t saddr, uint32_t ring_base,
                              uint32_t last_half_base)
{
	const uint32_t half = AIUS_TX_FILL_BUFFER_BYTES / 2u;
	if (saddr < ring_base || saddr - ring_base >= AIUS_TX_FILL_BUFFER_BYTES)
		return false;
	return saddr >= last_half_base && saddr - last_half_base < half;
}

/* Setpoint at the open: level + 256 - offset, minus 128 if a boundary is
 * pending (384 - phi*128, or 256 - phi'*128 at level 128). */
AIUS_TX_FILL_INLINE uint32_t
aius_tx_fill_open_setpoint_frames(uint32_t level_frames, uint32_t offset_frames,
                                  bool boundary_pending)
{
	const uint32_t off = offset_frames > (uint32_t)AIUS_TX_FILL_HALF_FRAMES
		? (uint32_t)AIUS_TX_FILL_HALF_FRAMES : offset_frames;
	uint32_t sp = level_frames + 2u * (uint32_t)AIUS_TX_FILL_HALF_FRAMES - off;
	if (boundary_pending)
		sp -= (uint32_t)AIUS_TX_FILL_HALF_FRAMES;
	return sp;
}

typedef enum {
	AIUS_TX_FILL_OPEN_WAIT = 0,
	AIUS_TX_FILL_OPEN_NOW = 1
} AiusTxFillOpenDecision;

/* NOW at a unit start, or once the committed frames since the arm reach one
 * unit (the cap); otherwise WAIT. */
AIUS_TX_FILL_INLINE AiusTxFillOpenDecision
aius_tx_fill_open_decide(bool first_in_unit, uint32_t committed_since_arm_frames,
                         uint32_t unit_frames)
{
	return (first_in_unit || committed_since_arm_frames >= unit_frames)
		? AIUS_TX_FILL_OPEN_NOW : AIUS_TX_FILL_OPEN_WAIT;
}

/* Occupancy in 1/16 frame, signed: level + 256 - dma_offset_frames. */
AIUS_TX_FILL_SENSOR_COLD static inline int32_t
aius_tx_fill_occupancy_q4(uint32_t level_frames, uint32_t dma_offset_frames)
{
	int32_t off = dma_offset_frames > (uint32_t)AIUS_TX_FILL_HALF_FRAMES
		? AIUS_TX_FILL_HALF_FRAMES : (int32_t)dma_offset_frames;
	int64_t occ = (int64_t)level_frames + 2 * AIUS_TX_FILL_HALF_FRAMES - off;
	return (int32_t)(occ * 16);
}

AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_exclude(AiusTxFillSensor *s, uint32_t reason)
{
	s->cur_excluded = 1;
	s->cur_reasons |= reason;
}

/* An underrun or a missed fill voids the current sub-window. */
AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_note_underrun(AiusTxFillSensor *s)
{
	aius_tx_fill_sensor_exclude(s, AIUS_TX_FILL_EXCL_UNDERRUN);
}

AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_note_missed_fill(AiusTxFillSensor *s)
{
	aius_tx_fill_sensor_exclude(s, AIUS_TX_FILL_EXCL_MISSED_FILL);
}

/* Latch-based form of the two above for the tap: pass the ring's lifetime
 * underrun counter and the TX missed-fill counter on every block. The first
 * call after a reset only latches. */
AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_observe_faults(AiusTxFillSensor *s, uint32_t underruns,
                                   uint32_t missed_fills)
{
	if (s->latch_valid) {
		if (underruns != s->underruns_latch)
			aius_tx_fill_sensor_note_underrun(s);
		if (missed_fills != s->missed_fills_latch)
			aius_tx_fill_sensor_note_missed_fill(s);
	}
	s->underruns_latch = underruns;
	s->missed_fills_latch = missed_fills;
	s->latch_valid = 1;
}

/* Pre-commit sample: read fills, level, DMA offset, fills again; retry if
 * fills moved, three attempts in all. Three tears drop the sample and count
 * `fill_torn_reads`. Returns 1 if a sample was recorded. */
AIUS_TX_FILL_SENSOR_COLD static inline int
aius_tx_fill_sensor_sample(AiusTxFillSensor *s, const AiusTxFillReader *r)
{
	for (uint32_t attempt = 0u; attempt < AIUS_TX_FILL_MAX_ATTEMPTS;
	     attempt++) {
		const uint32_t before = r->fills(r->ctx);
		const uint32_t level = r->level(r->ctx);
		const uint32_t offset = r->dma_offset_frames(r->ctx);
		const uint32_t after = r->fills(r->ctx);
		if (before != after)
			continue;
		const int32_t occ = aius_tx_fill_occupancy_q4(level, offset);
		if (occ < s->cur_min_q4)
			s->cur_min_q4 = occ;
		return 1;
	}
	s->snap.fill_torn_reads++;
	return 0;
}

AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_close_window(AiusTxFillSensor *s)
{
	int64_t sum = 0;
	int32_t lo = INT32_MAX, hi = INT32_MIN;
	uint32_t valid = 0u;
	for (uint32_t i = 0u; i < AIUS_TX_FILL_SUBWINDOWS; i++) {
		const int32_t v = s->sub_min_q4[i];
		if (v == INT32_MAX)
			continue;
		sum += v;
		valid++;
		if (v < lo)
			lo = v;
		if (v > hi)
			hi = v;
	}
	if (valid >= AIUS_TX_FILL_MIN_VALID_SUBWINDOWS) {
		const int64_t n = (int64_t)valid;
		const int64_t q = sum >= 0 ? (sum + n / 2) / n
		                           : -((-sum + n / 2) / n);
		s->snap.fill_level_q4 = (int32_t)q;
		s->snap.fill_level_valid = 1;
		s->snap.fill_subwindow_spread_frames =
			(uint32_t)(((int64_t)hi - lo + 8) / 16);
		s->snap.fill_windows++;
	} else {
		s->snap.fill_level_valid = 0;
		s->snap.fill_level_q4 = 0;
		s->snap.fill_subwindow_spread_frames = 0u;
	}
	s->snap.fill_window_exclusions = s->win_reasons;
	s->snap.fill_window_seq++;
	s->win_reasons = 0u;
	s->subs_closed = 0u;
	for (uint32_t i = 0u; i < AIUS_TX_FILL_SUBWINDOWS; i++)
		s->sub_min_q4[i] = INT32_MAX;
}

/* One call per ACCEPTED producer block after it is committed.
 * `frames`: the block's frames (window accounting). `unit_frames`: the
 * production unit the stall test measures against (PCM's block, an AC-3
 * burst's 1,536, the pipeline unit for E-AC-3 and DTS); a burst's blocks
 * arrive back to back, so the unit, not the block, sets the expected gap.
 * `now_us`: wrapping microsecond clock. `rate_hz`: stream rate. A gap since
 * the previous accepted block longer than that unit's duration plus 10 ms is
 * a production stall and excludes the current sub-window. */
AIUS_TX_FILL_SENSOR_COLD static inline void
aius_tx_fill_sensor_commit(AiusTxFillSensor *s, uint32_t frames,
                           uint32_t unit_frames, uint32_t now_us,
                           uint32_t rate_hz)
{
	if (s->have_last_block && rate_hz != 0u) {
		const uint32_t gap_us = now_us - s->last_commit_us;
		const uint64_t unit_us =
			(uint64_t)s->last_unit_frames * 1000000u / rate_hz;
		if ((uint64_t)gap_us >
		    unit_us + AIUS_TX_FILL_STALL_ALLOWANCE_US)
			aius_tx_fill_sensor_exclude(s, AIUS_TX_FILL_EXCL_STALL);
	}
	s->have_last_block = 1;
	s->last_commit_us = now_us;
	s->last_unit_frames = unit_frames;

	s->cur_frames += frames;
	while (s->cur_frames >= AIUS_TX_FILL_SUBWINDOW_FRAMES) {
		s->cur_frames -= AIUS_TX_FILL_SUBWINDOW_FRAMES;
		if (s->cur_min_q4 == INT32_MAX)
			s->cur_reasons |= AIUS_TX_FILL_EXCL_NO_SAMPLE;
		s->win_reasons |= s->cur_reasons;
		s->cur_reasons = 0u;
		if (s->cur_excluded || s->cur_min_q4 == INT32_MAX) {
			s->snap.fill_subwindows_excluded++;
			s->sub_min_q4[s->subs_closed] = INT32_MAX;
		} else {
			s->sub_min_q4[s->subs_closed] = s->cur_min_q4;
		}
		s->cur_min_q4 = INT32_MAX;
		s->cur_excluded = 0;
		if (++s->subs_closed >= AIUS_TX_FILL_SUBWINDOWS)
			aius_tx_fill_sensor_close_window(s);
	}
}

AIUS_TX_FILL_SENSOR_COLD static inline AiusTxFillSnapshot
aius_tx_fill_sensor_snapshot(const AiusTxFillSensor *s)
{
	return s->snap;
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_TX_FILL_SENSOR_H */
