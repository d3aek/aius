/* Host suite for aius_producer_yield: fake platform over a fake cycle clock
 * (1 cycle == 1 ns at 1000 cycles/us, so 0.2 ms == 200000 cycles). */
#include <stdio.h>
#include <string.h>

#include "aius_boot.h"
#include "aius_producer_yield.h"
#include "aius_producer_yield_platform.h"

#define CPU_PER_US 1000u
#define MS(x) ((uint32_t)((x) * 1000.0 * CPU_PER_US + 0.5))

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct {
	uint32_t now, ipsr, primask;
	bool irq_sw, ready, recovery, pending;
	uint32_t pass_cycles;
	bool pending_after_pass;
	uint16_t ledger;
	uint16_t log[16];
	int log_n;
	int passes;
	bool pass_saw_audio;
} f;

uint32_t aius_producer_yield_plat_cycles(void) { return f.now; }
uint32_t aius_producer_yield_plat_ipsr(void) { return f.ipsr; }
uint32_t aius_producer_yield_plat_primask(void) { return f.primask; }
bool aius_producer_yield_plat_irq_software_on(void) { return f.irq_sw; }
bool aius_producer_yield_plat_audio_ready(void) { return f.ready; }
bool aius_producer_yield_plat_recovery_safe(void) { return f.recovery; }
bool aius_producer_yield_plat_work_pending(void) { return f.pending; }
uint16_t aius_producer_yield_plat_ledger_checkpoint(void) { return f.ledger; }
void aius_producer_yield_plat_ledger_record(uint16_t cp)
{
	f.ledger = cp;
	if (f.log_n < 16)
		f.log[f.log_n++] = cp;
}
void aius_producer_yield_plat_run_pass(void)
{
	f.passes++;
	f.pass_saw_audio = f.ledger == AIUS_BOOT_CHECKPOINT_AUDIO;
	f.now += f.pass_cycles;
	f.pending = f.pending_after_pass;
}

static void reset(void)
{
	memset(&f, 0, sizeof f);
	f.irq_sw = f.ready = f.pending = true;
	f.ledger = AIUS_BOOT_CHECKPOINT_HTTP;
	aius_producer_yield_configure(CPU_PER_US, AIUS_PRODUCER_YIELD_LATE_US_DEFAULT);
}

static void test_truth_table(void)
{
	/* bits: 0 window/point, 1 in_producer, 2 ipsr, 3 primask, 4 !irq_sw,
	 * 5 !ready, 6 recovery, 7 !pending. A pass runs only for 0b00000001. */
	for (unsigned m = 0; m < 256; m++) {
		for (int via_point = 0; via_point < 2; via_point++) {
			bool ran;
			reset();
			if (via_point && !(m & 1u))
				continue; /* a point has no window term */
			if (m & 1u) {
				if (!via_point)
					aius_producer_yield_window_open();
			}
			if (m & 2u)
				aius_producer_yield_visit_begin();
			f.ipsr = (m & 4u) ? 15u : 0u;
			f.primask = (m & 8u) ? 1u : 0u;
			f.irq_sw = !(m & 16u);
			f.ready = !(m & 32u);
			f.recovery = (m & 64u) != 0u;
			f.pending = !(m & 128u);
			ran = via_point ? aius_producer_yield_point()
			                : aius_producer_yield_wait();
			CHECK(ran == (m == 1u));
			CHECK(f.passes == (m == 1u ? 1 : 0));
			CHECK((f.log_n == 0) == (m != 1u));
		}
	}
	/* wait outside any window never passes, point inside none does. */
	reset();
	CHECK(!aius_producer_yield_wait());
	CHECK(aius_producer_yield_point());
	{
		AiusProducerYieldDiag d;
		aius_producer_yield_get_diag(&d);
		CHECK(d.hook_calls == 1u && d.point_passes == 1u && d.wait_passes == 0u);
	}
}

static void test_pend_stamp(void)
{
	AiusProducerYieldDiag d;
	reset();
	/* Idle: first commit stamps; a later commit does not move it. */
	f.now = MS(1);
	aius_producer_yield_note_pend(f.now);
	f.now = MS(3);
	aius_producer_yield_note_pend(f.now);
	f.now = MS(5);
	f.pass_cycles = MS(1);
	CHECK(aius_producer_yield_point());
	aius_producer_yield_get_diag(&d);
	CHECK(d.pend_latency_max_us == 4000u); /* 5 ms - 1 ms */
	CHECK(d.late_passes == 1u);            /* > 2.5 ms */
	/* Pass left ingress empty: no re-stamp, next idle commit stamps. */
	CHECK(g_aius_producer_yield.pend_armed == 0u);

	/* Commit while the producer runs is not stamped; non-empty exit re-stamps. */
	reset();
	aius_producer_yield_visit_begin();
	aius_producer_yield_note_pend(MS(1));
	CHECK(g_aius_producer_yield.pend_armed == 0u);
	f.now = MS(2);
	f.pending = true;
	aius_producer_yield_visit_end();
	CHECK(g_aius_producer_yield.pend_armed == 1u);
	CHECK(g_aius_producer_yield.pend_stamp == MS(2));
	f.now = MS(3);
	f.pass_cycles = 0;
	f.pending_after_pass = true; /* budget-limited pass leaves work */
	CHECK(aius_producer_yield_point());
	CHECK(g_aius_producer_yield.pend_armed == 1u);
	CHECK(g_aius_producer_yield.pend_stamp == MS(3));
	aius_producer_yield_get_diag(&d);
	CHECK(d.pend_latency_max_us == 1000u && d.late_passes == 0u);

	/* D-S3: a pend served by a regular visit is timed and counted late. */
	reset();
	aius_producer_yield_note_pend(MS(1));
	f.now = MS(4);
	f.pending = false;
	aius_producer_yield_visit_begin();
	aius_producer_yield_visit_end();
	aius_producer_yield_get_diag(&d);
	CHECK(d.pend_latency_max_us == 3000u && d.late_passes == 1u);
	CHECK(d.wait_passes == 0u && d.point_passes == 0u);
	CHECK(g_aius_producer_yield.pend_armed == 0u);

	/* Under D: watermark only, not late. A visit with no pend records nothing. */
	reset();
	aius_producer_yield_note_pend(MS(1));
	f.now = MS(2);
	f.pending = false;
	aius_producer_yield_visit_begin();
	aius_producer_yield_visit_end();
	f.now = MS(90);
	aius_producer_yield_visit_begin();
	aius_producer_yield_visit_end();
	aius_producer_yield_get_diag(&d);
	CHECK(d.pend_latency_max_us == 1000u && d.late_passes == 0u);
}

static void test_ledger(void)
{
	reset();
	f.ledger = AIUS_BOOT_CHECKPOINT_HTTP;
	CHECK(aius_producer_yield_point());
	CHECK(f.pass_saw_audio);
	CHECK(f.log_n == 2);
	CHECK(f.log[0] == AIUS_BOOT_CHECKPOINT_AUDIO);
	CHECK(f.log[1] == AIUS_BOOT_CHECKPOINT_HTTP);
	CHECK(f.ledger == AIUS_BOOT_CHECKPOINT_HTTP);

	/* Wait path, other saved checkpoint, work left over. */
	reset();
	f.ledger = AIUS_BOOT_CHECKPOINT_SD;
	f.pending_after_pass = true;
	aius_producer_yield_window_open();
	CHECK(aius_producer_yield_wait());
	aius_producer_yield_window_close();
	CHECK(f.pass_saw_audio && f.log_n == 2);
	CHECK(f.log[0] == AIUS_BOOT_CHECKPOINT_AUDIO && f.log[1] == AIUS_BOOT_CHECKPOINT_SD);

	/* Saved == AUDIO still records both. */
	reset();
	f.ledger = AIUS_BOOT_CHECKPOINT_AUDIO;
	CHECK(aius_producer_yield_point());
	CHECK(f.log_n == 2 && f.ledger == AIUS_BOOT_CHECKPOINT_AUDIO);

	/* A refused guard touches no ledger. */
	reset();
	f.pending = false;
	CHECK(!aius_producer_yield_point());
	CHECK(f.log_n == 0);
}

static void test_stretch(void)
{
	AiusProducerYieldDiag d;
	/* CR-S1: 0.2 ms, hook, 7.6 ms pass, 0.3 ms, hook (no pass), 0.1 ms, close. */
	reset();
	f.pass_cycles = MS(7.6);
	f.now = MS(10);
	aius_producer_yield_window_open();
	f.now += MS(0.2);
	CHECK(aius_producer_yield_wait());
	f.now += MS(0.3);
	f.pending = false;
	CHECK(!aius_producer_yield_wait());
	f.now += MS(0.1);
	aius_producer_yield_window_close();
	aius_producer_yield_get_diag(&d);
	CHECK(d.window_nonyield_max_us == 300u);
	CHECK(d.hook_calls == 2u && d.wait_passes == 1u);

	/* Close-to-last-hook segment can be the max; nested windows count once. */
	reset();
	f.pending = false;
	aius_producer_yield_window_open();
	aius_producer_yield_window_open();
	f.now += MS(0.1);
	aius_producer_yield_wait();
	f.now += MS(0.2);
	aius_producer_yield_window_close();
	f.now += MS(5); /* still open (depth 1): counts in the segment */
	aius_producer_yield_window_close();
	aius_producer_yield_get_diag(&d);
	CHECK(d.window_nonyield_max_us == 5200u);

	/* Hooks outside a window leave the clock alone. */
	reset();
	f.pending = false;
	f.now = MS(100);
	aius_producer_yield_wait();
	aius_producer_yield_get_diag(&d);
	CHECK(d.window_nonyield_max_us == 0u && d.hook_calls == 1u);

	/* Cycle counter wrap. */
	reset();
	f.pending = false;
	f.now = 0xFFFFFFFFu - MS(0.1);
	aius_producer_yield_window_open();
	f.now += MS(0.4);
	aius_producer_yield_window_close();
	aius_producer_yield_get_diag(&d);
	CHECK(d.window_nonyield_max_us == 400u);
}

static void test_clear(void)
{
	AiusProducerYieldDiag d;
	reset();
	f.now = MS(0);
	aius_producer_yield_note_pend(f.now);
	f.now = MS(4);
	aius_producer_yield_window_open();
	f.now += MS(2);
	aius_producer_yield_wait();
	aius_producer_yield_window_close();
	aius_producer_yield_get_diag(&d);
	CHECK(d.pend_latency_max_us == 6000u && d.late_passes == 1u);
	CHECK(d.window_nonyield_max_us == 2000u && d.wait_passes == 1u);
	aius_producer_yield_clear_diag();
	aius_producer_yield_get_diag(&d);
	CHECK(d.pend_latency_max_us == 0u && d.late_passes == 0u);
	CHECK(d.window_nonyield_max_us == 0u);
	CHECK(d.wait_passes == 1u && d.hook_calls == 1u); /* lifetime counters */
}

int main(void)
{
	test_truth_table();
	test_pend_stamp();
	test_ledger();
	test_stretch();
	test_clear();
	printf("producer-yield: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
