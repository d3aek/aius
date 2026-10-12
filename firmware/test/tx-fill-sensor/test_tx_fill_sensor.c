/* The phase-corrected transmit-ring fill statistic (aius-sample-rate-following
 * Phase 3): torn-read retry, DMA phase correction, empty-ring reading, class
 * floors and trough-count bias, stall exclusion, validity threshold and the
 * reset paths. Pure header, no implementation linked. */

#include "aius_tx_fill_sensor.h"

#include <assert.h>
#include <stdint.h>

#define RATE 48000u
#define WINDOW_FRAMES (AIUS_TX_FILL_SUBWINDOW_FRAMES * AIUS_TX_FILL_SUBWINDOWS)

/* ---- injectable reader ------------------------------------------------ */
typedef struct {
	uint32_t fills;
	uint32_t level;
	uint32_t offset;
	uint32_t tears_left; /* number of upcoming attempts whose 2nd fills read moves */
	uint32_t fills_reads;
} Fake;

static uint32_t fake_fills(void *c)
{
	Fake *f = c;
	f->fills_reads++;
	/* Odd reads are the "before" read, even the "after" read. */
	if ((f->fills_reads & 1u) == 0u && f->tears_left != 0u) {
		f->tears_left--;
		f->fills++;
	}
	return f->fills;
}
static uint32_t fake_level(void *c) { return ((Fake *)c)->level; }
static uint32_t fake_offset(void *c) { return ((Fake *)c)->offset; }

static AiusTxFillReader reader_for(Fake *f)
{
	AiusTxFillReader r = { f, fake_fills, fake_level, fake_offset };
	return r;
}

/* ---- deterministic jitter --------------------------------------------- */
static uint32_t rng_state = 12345u;
static uint32_t rnd(uint32_t n)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (rng_state >> 8) % n;
}

/* Feed one block: pre-commit sample of `occ` frames (offset chosen so the
 * level stays unsigned), then the commit. */
static void feed(AiusTxFillSensor *s, uint32_t occ, uint32_t frames,
                 uint32_t unit, uint32_t now_us)
{
	Fake f = { 0u, 0u, 0u, 0u, 0u };
	const uint32_t off = rnd(128u);
	f.level = occ + off - 256u;
	f.offset = off;
	AiusTxFillReader r = reader_for(&f);
	assert(aius_tx_fill_sensor_sample(s, &r) == 1);
	aius_tx_fill_sensor_commit(s, frames, unit, now_us, RATE);
}

/* Drive `windows` full windows of a class. Returns the mean fill_level_q4
 * over the windows (as double, in frames). */
typedef enum { PCM, AC3, DTS2048, EAC3_LONG } Class;

static uint32_t g_now;
static double run_class(AiusTxFillSensor *s, Class c, uint32_t floor_frames,
                        uint32_t jitter, uint32_t windows)
{
	double sum = 0.0;
	uint32_t done = 0u, committed = 0u;
	const uint32_t target = windows * WINDOW_FRAMES;
	uint32_t last_windows = s->snap.fill_windows;
	while (committed < target) {
		uint32_t occ = floor_frames + rnd(jitter);
		switch (c) {
		case PCM:
			feed(s, occ, 512u, 512u, g_now);
			g_now += 512u * 1000000u / RATE;
			committed += 512u;
			break;
		case AC3: /* six 256 blocks back to back, one burst per 32 ms */
			for (uint32_t k = 0u; k < 6u; k++) {
				feed(s, occ + k * 256u, 256u,
				     aius_tx_fill_unit_frames(1u, 256u, 0), g_now);
				g_now += 100u;
			}
			g_now += 32000u - 600u;
			committed += 1536u;
			break;
		case DTS2048:
			feed(s, occ, 2048u, aius_tx_fill_unit_frames(13u, 2048u, 0),
			     g_now);
			g_now += 2048u * 1000000u / RATE + (uint32_t)rnd(5000u);
			committed += 2048u;
			break;
		case EAC3_LONG:
			feed(s, occ, 6144u, aius_tx_fill_unit_frames(21u, 6144u, 0),
			     g_now);
			g_now += 6144u * 1000000u / RATE + (uint32_t)rnd(5000u);
			committed += 6144u;
			break;
		}
		if (s->snap.fill_windows != last_windows) {
			last_windows = s->snap.fill_windows;
			sum += s->snap.fill_level_q4 / 16.0;
			done++;
		}
	}
	assert(done >= windows - 1u);
	return sum / done;
}

int main(void)
{
	AiusTxFillSensor s;

	/* --- occupancy arithmetic and phi mapping ------------------------- */
	/* offset 0 = start of half (phi 0): level + 256; offset 128 = end of
	 * half (phi 1): level + 128; mid-half: level + 192. */
	assert(aius_tx_fill_occupancy_q4(100u, 0u) == (100 + 256) * 16);
	assert(aius_tx_fill_occupancy_q4(100u, 128u) == (100 + 128) * 16);
	assert(aius_tx_fill_occupancy_q4(100u, 64u) == (100 + 192) * 16);
	/* Defensive clamp only: the SADDR mapping never produces > 127. */
	assert(aius_tx_fill_occupancy_q4(100u, 9999u) == (100 + 128) * 16);
	/* Empty ring: 256 - phi*128, 128 to 256, never wrapped. */
	assert(aius_tx_fill_occupancy_q4(0u, 0u) == 256 * 16);
	assert(aius_tx_fill_occupancy_q4(0u, 64u) == 192 * 16);
	assert(aius_tx_fill_occupancy_q4(0u, 128u) == 128 * 16);
	assert(aius_tx_fill_occupancy_q4(0u, 127u) > 0);

	/* --- SADDR to frame offset (the firmware's own mapping) ------------ */
	{
		const uint32_t base = 0x20000000u;
		assert(aius_tx_fill_saddr_offset_frames(base, base) == 0u);
		assert(aius_tx_fill_saddr_offset_frames(base + 1016u, base) == 127u);
		assert(aius_tx_fill_saddr_offset_frames(base + 1024u, base) == 0u);
		assert(aius_tx_fill_saddr_offset_frames(base + 2040u, base) == 127u);
		assert(aius_tx_fill_saddr_offset_frames(base + 8u, base) == 1u);
		assert(aius_tx_fill_saddr_offset_frames(base + 1024u + 512u, base) == 64u);
		/* Defensive clamps: addresses the DMA never produces. */
		assert(aius_tx_fill_saddr_offset_frames(base + 2048u, base) == 127u);
		assert(aius_tx_fill_saddr_offset_frames(base - 8u, base) == 127u);
	}

	/* --- stall unit by Pc (IEC 61937 repetition period) ---------------- */
	assert(aius_tx_fill_unit_frames(1u, 256u, 0) == 1536u);
	assert(aius_tx_fill_unit_frames(11u, 256u, 0) == 512u);
	assert(aius_tx_fill_unit_frames(12u, 256u, 0) == 1024u);
	assert(aius_tx_fill_unit_frames(13u, 256u, 0) == 2048u);
	assert(aius_tx_fill_unit_frames(21u, 256u, 0) == 6144u);
	assert(aius_tx_fill_unit_frames(0u, 512u, 1) == 512u);
	assert(aius_tx_fill_unit_frames(21u, 512u, 1) == 512u); /* PCM ignores Pc */

	/* --- torn reads ---------------------------------------------------- */
	aius_tx_fill_sensor_init(&s);
	{
		Fake f = { 5u, 40u, 0u, 1u, 0u };
		AiusTxFillReader r = reader_for(&f);
		assert(aius_tx_fill_sensor_sample(&s, &r) == 1); /* one tear retries */
		assert(s.snap.fill_torn_reads == 0u);
		assert(s.cur_min_q4 == (40 + 256) * 16);

		Fake g = { 5u, 40u, 0u, 2u, 0u };
		r = reader_for(&g);
		assert(aius_tx_fill_sensor_sample(&s, &r) == 1); /* two tears retry */
		assert(s.snap.fill_torn_reads == 0u);

		Fake h = { 5u, 10u, 0u, 3u, 0u };
		r = reader_for(&h);
		assert(aius_tx_fill_sensor_sample(&s, &r) == 0); /* three tears drop */
		assert(s.snap.fill_torn_reads == 1u);
		assert(s.cur_min_q4 == (40 + 256) * 16); /* the dropped one not folded */
		assert(h.fills_reads == 6u);             /* exactly three attempts */
	}

	/* --- class floors, trough-count bias ------------------------------ */
	{
		const uint32_t floor_frames = 300u, jitter = 64u;
		AiusTxFillSensor p, a;
		aius_tx_fill_sensor_init(&p);
		aius_tx_fill_sensor_init(&a);
		g_now = 1000u;
		const double pcm = run_class(&p, PCM, floor_frames, jitter, 20u);
		g_now = 1000u;
		const double ac3 = run_class(&a, AC3, floor_frames, jitter, 20u);
		assert(p.snap.fill_subwindows_excluded == 0u);
		assert(a.snap.fill_subwindows_excluded == 0u);
		/* Both read the floor (not F + 640 for AC-3). */
		assert(pcm >= floor_frames && pcm < floor_frames + 8.0);
		assert(ac3 >= floor_frames && ac3 < floor_frames + 16.0);
		/* Expected minimum of n uniform troughs of range J is J/(n+1):
		 * ~23 troughs for PCM, ~7.8 for AC-3 per sub-window. */
		const double bias = (double)jitter / 8.8 - (double)jitter / 24.4;
		const double diff = ac3 - pcm;
		assert(diff > 0.0);
		assert(diff > bias - 2.5 && diff < bias + 2.5);
		/* A real window carries sub-window spread and a count. */
		assert(p.snap.fill_level_valid == 1);
		assert(p.snap.fill_windows >= 19u);
		assert(p.snap.fill_subwindow_spread_frames <= jitter);
	}

	/* --- cadence that must not exclude; a real stall that must -------- */
	{
		AiusTxFillSensor c;
		aius_tx_fill_sensor_init(&c);
		g_now = 5000u;
		run_class(&c, DTS2048, 300u, 32u, 2u);
		assert(c.snap.fill_subwindows_excluded == 0u);
		aius_tx_fill_sensor_init(&c);
		g_now = 5000u;
		run_class(&c, EAC3_LONG, 300u, 32u, 2u);
		assert(c.snap.fill_subwindows_excluded == 0u);
		aius_tx_fill_sensor_init(&c);
		g_now = 5000u;
		run_class(&c, AC3, 300u, 32u, 2u);
		assert(c.snap.fill_subwindows_excluded == 0u);

		/* Long-burst E-AC-3 (128 ms units, gaps > 40 ms) excludes nothing
		 * over 20 windows; a 300 ms stall in it still excludes. */
		aius_tx_fill_sensor_init(&c);
		g_now = 5000u;
		run_class(&c, EAC3_LONG, 300u, 32u, 20u);
		assert(c.snap.fill_subwindows_excluded == 0u);
		assert(c.snap.fill_windows >= 19u);
		{
			g_now += 300000u;
			const uint32_t b4 = c.snap.fill_subwindows_excluded;
			feed(&c, 300u, 6144u, aius_tx_fill_unit_frames(21u, 6144u, 0), g_now);
			assert(c.cur_excluded == 1 || c.snap.fill_subwindows_excluded > b4);
		}

		/* PCM with one 100 ms stall: that sub-window is excluded. */
		aius_tx_fill_sensor_init(&c);
		g_now = 5000u;
		run_class(&c, PCM, 300u, 32u, 1u);
		assert(c.snap.fill_subwindows_excluded == 0u);
		g_now += 100000u;
		const uint32_t before = c.snap.fill_subwindows_excluded;
		const uint32_t target = c.snap.fill_windows + 1u;
		while (c.snap.fill_windows < target) {
			feed(&c, 300u, 512u, 512u, g_now);
			g_now += 10666u;
		}
		assert(c.snap.fill_subwindows_excluded == before + 1u);
		/* 15 valid sub-windows still >= 8, so the window stays valid. */
		assert(c.snap.fill_level_valid == 1);
		/* A clock wrap inside a normal cadence is not a stall. */
		aius_tx_fill_sensor_init(&c);
		g_now = 0xFFFFFF00u;
		run_class(&c, PCM, 300u, 32u, 1u);
		assert(c.snap.fill_subwindows_excluded == 0u);
	}

	/* --- underrun and missed-fill exclusion; validity threshold ------- */
	{
		AiusTxFillSensor v;
		/* Exclude N of 16 sub-windows via underruns; the rest valid. */
		for (uint32_t excluded = 0u; excluded <= 10u; excluded++) {
			aius_tx_fill_sensor_init(&v);
			g_now = 100u;
			for (uint32_t sub = 0u; sub < 16u; sub++) {
				if (sub < excluded) {
					if (sub & 1u)
						aius_tx_fill_sensor_note_missed_fill(&v);
					else
						aius_tx_fill_sensor_note_underrun(&v);
				}
				/* 12,000 frames = 3 x 4,000-frame blocks. */
				for (uint32_t b = 0u; b < 3u; b++) {
					feed(&v, 300u + sub, 4000u, 4000u, g_now);
					g_now += 4000u * 1000000u / RATE;
				}
			}
			assert(v.snap.fill_subwindows_excluded == excluded);
			if (16u - excluded >= 8u) {
				assert(v.snap.fill_level_valid == 1);
				assert(v.snap.fill_windows == 1u);
			} else {
				assert(v.snap.fill_level_valid == 0);
				assert(v.snap.fill_windows == 0u);
			}
		}
		/* Value: mean of the minima (300..315, rounded to 1/16). */
		aius_tx_fill_sensor_init(&v);
		g_now = 100u;
		for (uint32_t sub = 0u; sub < 16u; sub++)
			for (uint32_t b = 0u; b < 3u; b++) {
				feed(&v, 300u + sub, 4000u, 4000u, g_now);
				g_now += 4000u * 1000000u / RATE;
			}
		assert(v.snap.fill_level_q4 == (int32_t)(307.5 * 16));
		assert(v.snap.fill_subwindow_spread_frames == 15u);

		/* Latch form: counters moving mark the sub-window. */
		aius_tx_fill_sensor_init(&v);
		aius_tx_fill_sensor_observe_faults(&v, 3u, 1u);
		assert(v.cur_excluded == 0);
		aius_tx_fill_sensor_observe_faults(&v, 3u, 1u);
		assert(v.cur_excluded == 0);
		aius_tx_fill_sensor_observe_faults(&v, 4u, 1u);
		assert(v.cur_excluded == 1);
		v.cur_excluded = 0;
		aius_tx_fill_sensor_observe_faults(&v, 4u, 2u);
		assert(v.cur_excluded == 1);
	}

	/* --- an invalid window serves null; fill_windows counts valid only -- */
	{
		AiusTxFillSensor w;
		aius_tx_fill_sensor_init(&w);
		g_now = 100u;
		run_class(&w, PCM, 300u, 16u, 1u);
		assert(w.snap.fill_level_valid == 1 && w.snap.fill_windows == 1u);
		const uint32_t before_windows = w.snap.fill_windows;
		/* Next window: 9 of 16 sub-windows excluded by underruns. */
		for (uint32_t sub = 0u; sub < 16u; sub++) {
			if (sub < 9u)
				aius_tx_fill_sensor_note_underrun(&w);
			for (uint32_t b = 0u; b < 3u; b++) {
				feed(&w, 300u, 4000u, 4000u, g_now);
				g_now += 4000u * 1000000u / RATE;
			}
		}
		assert(w.snap.fill_level_valid == 0);
		assert(w.snap.fill_subwindow_spread_frames == 0u);
		assert(w.snap.fill_windows == before_windows);
		/* A valid window after it serves a value again. */
		for (uint32_t sub = 0u; sub < 16u; sub++)
			for (uint32_t b = 0u; b < 3u; b++) {
				feed(&w, 300u, 4000u, 4000u, g_now);
				g_now += 4000u * 1000000u / RATE;
			}
		assert(w.snap.fill_level_valid == 1);
		assert(w.snap.fill_windows == before_windows + 1u);
	}

	/* --- trim reset keeps the fault latches --------------------------- */
	{
		AiusTxFillSensor t;
		aius_tx_fill_sensor_init(&t);
		g_now = 100u;
		feed(&t, 300u, 512u, 512u, g_now);
		aius_tx_fill_sensor_observe_faults(&t, 3u, 1u);
		aius_tx_fill_sensor_reset_keep_latches(&t);
		assert(t.subs_closed == 0u && t.cur_frames == 0u);
		assert(t.cur_min_q4 == INT32_MAX && t.snap.fill_level_valid == 0);
		assert(t.latch_valid == 1);
		/* A fault between the trim block and the next is seen. */
		aius_tx_fill_sensor_observe_faults(&t, 4u, 1u);
		assert(t.cur_excluded == 1);
		/* The full reset still forgets the latches. */
		aius_tx_fill_sensor_reset(&t);
		assert(t.latch_valid == 0 && t.cur_excluded == 0);
		aius_tx_fill_sensor_observe_faults(&t, 9u, 9u);
		assert(t.cur_excluded == 0);
	}

	/* --- reset paths --------------------------------------------------- */
	{
		AiusTxFillSensor r;
		for (uint32_t path = 0u; path < 4u; path++) {
			aius_tx_fill_sensor_init(&r);
			g_now = 100u;
			run_class(&r, PCM, 300u, 16u, 2u);
			assert(r.snap.fill_level_valid == 1);
			const uint32_t windows = r.snap.fill_windows;
			/* Half a window in flight, then the reset. */
			for (uint32_t i = 0u; i < 200u; i++) {
				feed(&r, 300u, 512u, 512u, g_now);
				g_now += 10666u;
			}
			switch (path) {
			case 0: assert(aius_tx_fill_sensor_hold(&r, 0, 1)); break; /* gate close */
			case 1: assert(aius_tx_fill_sensor_hold(&r, 1, 0)); break; /* carrier loss */
			case 2: aius_tx_fill_sensor_reset(&r); break;               /* governor trim */
			default: assert(!aius_tx_fill_sensor_hold(&r, 1, 1)); break;
			}
			if (path == 3u) {
				assert(r.snap.fill_level_valid == 1);
				continue;
			}
			assert(r.snap.fill_level_valid == 0);
			assert(r.subs_closed == 0u && r.cur_frames == 0u);
			assert(r.snap.fill_windows == windows); /* lifetime kept */
			/* Null until a FULL window completes again. */
			uint32_t committed = 0u;
			while (committed + 512u < WINDOW_FRAMES) {
				feed(&r, 300u, 512u, 512u, g_now);
				g_now += 10666u;
				committed += 512u;
				assert(r.snap.fill_level_valid == 0);
			}
			while (!r.snap.fill_level_valid) {
				feed(&r, 300u, 512u, 512u, g_now);
				g_now += 10666u;
			}
			assert(r.snap.fill_windows == windows + 1u);
		}
	}

	/* --- opening rules ------------------------------------------------- */
	{
		const uint32_t rb = 0x20000000u, h0 = rb, h1 = rb + 1024u;
		/* Pending: SADDR lies in the last-filled half, both halves, edges. */
		assert(aius_tx_fill_boundary_pending(h0, rb, h0));
		assert(aius_tx_fill_boundary_pending(h0 + 1023u, rb, h0));
		assert(!aius_tx_fill_boundary_pending(h1, rb, h0));
		assert(!aius_tx_fill_boundary_pending(h1 + 1023u, rb, h0));
		assert(aius_tx_fill_boundary_pending(h1, rb, h1));
		assert(aius_tx_fill_boundary_pending(h1 + 1023u, rb, h1));
		assert(!aius_tx_fill_boundary_pending(h0, rb, h1));
		assert(!aius_tx_fill_boundary_pending(h1 - 1u, rb, h1));
		assert(!aius_tx_fill_boundary_pending(rb + 2048u, rb, h1));
		/* Open level (F3): the target, plus the late ISR's half when pending. */
		assert(aius_tx_fill_open_level_frames(128u, false) == 128u);
		assert(aius_tx_fill_open_level_frames(128u, true) == 256u);
		assert(aius_tx_fill_open_level_frames(768u, false) == 768u);
		/* Pad split (R3): only frames actually padded above the target. */
		assert(aius_tx_fill_open_pending_pad_frames(256u, true) == 128u); /* from empty */
		assert(aius_tx_fill_open_pending_pad_frames(128u, true) == 128u); /* level 128 */
		assert(aius_tx_fill_open_pending_pad_frames(56u, true) == 56u);   /* level 200 */
		assert(aius_tx_fill_open_pending_pad_frames(0u, true) == 0u);     /* trimmed */
		assert(aius_tx_fill_open_pending_pad_frames(128u, false) == 0u);
		/* Setpoint at the normal open, level 128: 384 - phi*128. */
		assert(aius_tx_fill_open_setpoint_frames(128u, 0u, false) == 384u);
		assert(aius_tx_fill_open_setpoint_frames(128u, 64u, false) == 320u);
		assert(aius_tx_fill_open_setpoint_frames(128u, 127u, false) == 257u);
		/* Pending open, padded to level 256: 384 - phi'*128, as normal. */
		{
			const uint32_t lv = aius_tx_fill_open_level_frames(128u, true);
			assert(aius_tx_fill_open_setpoint_frames(lv, 0u, true) == 384u);
			assert(aius_tx_fill_open_setpoint_frames(lv, 64u, true) == 320u);
			assert(aius_tx_fill_open_setpoint_frames(lv, 127u, true) == 257u);
		}
		/* The rule itself: the late ISR takes one half from the level. */
		assert(aius_tx_fill_open_setpoint_frames(128u, 0u, true) == 256u);
		assert(aius_tx_fill_open_setpoint_frames(256u, 32u, false) == 480u);
		assert(aius_tx_fill_open_setpoint_frames(256u, 32u, true) == 352u);
		/* Open decision: unit start, mid-unit below the cap, at the cap. */
		assert(aius_tx_fill_open_decide(true, 0u, 1536u) == AIUS_TX_FILL_OPEN_NOW);
		assert(aius_tx_fill_open_decide(false, 256u, 1536u) == AIUS_TX_FILL_OPEN_WAIT);
		assert(aius_tx_fill_open_decide(false, 1535u, 1536u) == AIUS_TX_FILL_OPEN_WAIT);
		assert(aius_tx_fill_open_decide(false, 1536u, 1536u) == AIUS_TX_FILL_OPEN_NOW);
		assert(aius_tx_fill_open_decide(false, 4000u, 1536u) == AIUS_TX_FILL_OPEN_NOW);
	}

	/* --- window sequence and exclusion reasons ------------------------- */
	{
		AiusTxFillSensor q;
		aius_tx_fill_sensor_init(&q);
		assert(q.snap.fill_window_seq == 0u && q.snap.fill_window_exclusions == 0u);
		g_now = 100u;
		/* Clean valid window: seq 1, no reasons. */
		for (uint32_t sub = 0u; sub < 16u; sub++)
			for (uint32_t b = 0u; b < 3u; b++) {
				feed(&q, 300u, 4000u, 4000u, g_now);
				g_now += 4000u * 1000000u / RATE;
			}
		assert(q.snap.fill_window_seq == 1u && q.snap.fill_windows == 1u);
		assert(q.snap.fill_window_exclusions == 0u);
		/* Invalid window (9 underruns): seq advances, windows does not. */
		for (uint32_t sub = 0u; sub < 16u; sub++) {
			if (sub < 9u)
				aius_tx_fill_sensor_note_underrun(&q);
			for (uint32_t b = 0u; b < 3u; b++) {
				feed(&q, 300u, 4000u, 4000u, g_now);
				g_now += 4000u * 1000000u / RATE;
			}
		}
		assert(q.snap.fill_level_valid == 0);
		assert(q.snap.fill_window_seq == 2u && q.snap.fill_windows == 1u);
		assert(q.snap.fill_window_exclusions == AIUS_TX_FILL_EXCL_UNDERRUN);
		/* Reset keeps the seq, zeroes the served reasons and the in-flight bits. */
		aius_tx_fill_sensor_note_missed_fill(&q);
		aius_tx_fill_sensor_reset(&q);
		assert(q.snap.fill_window_seq == 2u);
		assert(q.snap.fill_window_exclusions == 0u);
		assert(q.cur_reasons == 0u && q.win_reasons == 0u);
		/* Each reason bit alone, and OR across sub-windows of one window. */
		{
			static const uint32_t want[4] = {
				AIUS_TX_FILL_EXCL_UNDERRUN, AIUS_TX_FILL_EXCL_MISSED_FILL,
				AIUS_TX_FILL_EXCL_STALL, AIUS_TX_FILL_EXCL_NO_SAMPLE };
			for (uint32_t k = 0u; k < 4u; k++) {
				aius_tx_fill_sensor_init(&q);
				g_now = 100u;
				for (uint32_t sub = 0u; sub < 16u; sub++) {
					for (uint32_t b = 0u; b < 3u; b++) {
						if (sub == 3u && k == 0u && b == 0u)
							aius_tx_fill_sensor_note_underrun(&q);
						if (sub == 3u && k == 1u && b == 0u)
							aius_tx_fill_sensor_note_missed_fill(&q);
						if (sub == 3u && k == 2u && b == 1u)
							g_now += 100000u; /* stall */
						if (sub == 3u && k == 3u) {
							/* commit without a sample */
							aius_tx_fill_sensor_commit(&q, 4000u, 4000u,
								g_now, RATE);
						} else {
							feed(&q, 300u, 4000u, 4000u, g_now);
						}
						g_now += 4000u * 1000000u / RATE;
					}
				}
				assert(q.snap.fill_window_seq == 1u);
				assert(q.snap.fill_window_exclusions == want[k]);
				assert(q.snap.fill_subwindows_excluded == 1u);
			}
			/* A window after a faulty one starts clean. */
			for (uint32_t sub = 0u; sub < 16u; sub++)
				for (uint32_t b = 0u; b < 3u; b++) {
					feed(&q, 300u, 4000u, 4000u, g_now);
					g_now += 4000u * 1000000u / RATE;
				}
			assert(q.snap.fill_window_seq == 2u);
			assert(q.snap.fill_window_exclusions == 0u);
		}
	}

	return 0;
}
