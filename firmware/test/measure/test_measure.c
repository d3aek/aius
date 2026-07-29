/* Known-answer tests for the measurement loopback's portable core: the
 * capture state machine (every edge, including the dual-terminal cancel
 * interleavings) and the sliced analysis (IEEE 1057 3-parameter sine fit +
 * Hann-windowed Goertzel).
 *
 * Built and run by scripts/test.sh --suite measure, with -ffp-contract=off
 * like the dsp suites. Every expectation is analytic.
 *
 * THE PURE-SINE CASE IS THE CANARY: the leakage-bin estimator the prebuild
 * gate rejected would have read ~-40 dB THD+N on a pure sine under Hann.
 * Its tolerance is NEVER widened to pass. */

#include "aius_measure.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define BLOCK 512u

static int close_to(double a, double b, double tol)
{
    double d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

/* Deterministic uniform PRNG for the noise vectors (xorshift32; any fixed
 * sequence does -- the test only needs a known noise POWER). */
static uint32_t prng_state = 0x41555331u;
static double prng_uniform(void) /* uniform in [-1, 1) */
{
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return ((double)(int32_t)prng_state) / 2147483648.0;
}

static int32_t quantize(double x)
{
    double r = x < 0.0 ? x - 0.5 : x + 0.5;
    if (r > (double)AIUS_DSP_SAMPLE_MAX)
        r = (double)AIUS_DSP_SAMPLE_MAX;
    if (r < (double)AIUS_DSP_SAMPLE_MIN)
        r = (double)AIUS_DSP_SAMPLE_MIN;
    return (int32_t)r;
}

/* Synthesize amp*sin(2 pi f/fs n + phase) + harmonics into int32 samples. */
static void synth(int32_t *out, uint32_t n, double amp, double f,
                  double amp3, double amp2, double noise_amp)
{
    for (uint32_t i = 0; i < n; i++) {
        double ph = 2.0 * M_PI * f * (double)i / FS;
        double x = amp * sin(ph) + amp2 * sin(2.0 * ph) + amp3 * sin(3.0 * ph);
        if (noise_amp > 0.0)
            x += noise_amp * prng_uniform();
        out[i] = quantize(x);
    }
}

/* --- capture state machine ------------------------------------------------- */

enum { CAP = 2048 }; /* 4 blocks: enough to walk every edge quickly */
static int32_t cap_l[CAP], cap_r[CAP];

static AiusMeasureCapture fresh_armed(uint32_t gen, uint32_t ovr)
{
    AiusMeasureCapture c;
    assert(aius_measure_capture_init(&c, cap_l, cap_r, CAP) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_IDLE);
    assert(aius_measure_arm(&c, gen, ovr) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_ARMED);
    return c;
}

static void feed_blocks(AiusMeasureCapture *c, uint32_t blocks,
                        uint32_t gen, uint32_t ovr, int32_t seed)
{
    int32_t l[BLOCK], r[BLOCK];
    for (uint32_t b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < BLOCK; i++) {
            l[i] = seed + (int32_t)(b * BLOCK + i);
            r[i] = -(seed + (int32_t)(b * BLOCK + i));
        }
        aius_measure_producer_block(c, l, r, BLOCK, gen, ovr);
    }
}

static void test_capture_happy_path(void)
{
    AiusMeasureCapture c = fresh_armed(7, 3);

    feed_blocks(&c, 1, 7, 3, 1000);
    assert(c.state == AIUS_MEASURE_CAPTURING);
    assert(c.cursor == BLOCK);

    feed_blocks(&c, 3, 7, 3, 1000);
    assert(c.state == AIUS_MEASURE_CAPTURED); /* exactly full, on a boundary */
    assert(c.cursor == CAP);

    /* The copied planes are the committed frames, verbatim. The second
     * feed_blocks call restarts its per-call counter, so the last frame is
     * that call's block 2, sample 511. */
    assert(cap_l[0] == 1000 && cap_r[0] == -1000);
    assert(cap_l[CAP - 1] == 1000 + (int32_t)(3 * BLOCK) - 1);

    /* A stale block after the producer terminal is a no-op. */
    feed_blocks(&c, 1, 7, 3, 9);
    assert(c.state == AIUS_MEASURE_CAPTURED && c.cursor == CAP);
    assert(cap_l[0] == 1000);

    assert(aius_measure_begin_analysis(&c) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_ANALYSING);
    /* The producer ignores analysing too: the frozen buffer stays frozen. */
    feed_blocks(&c, 1, 7, 3, 9);
    assert(cap_l[0] == 1000);
    assert(aius_measure_finish_analysis(&c) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_DONE);
    assert(aius_measure_release(&c) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_IDLE);
    printf("ok capture happy path (armed->capturing->captured->analysing->done->idle)\n");
}

static void test_capture_wrong_state_calls(void)
{
    AiusMeasureCapture c;
    assert(aius_measure_capture_init(&c, cap_l, cap_r, CAP) == AIUS_OK);

    /* Idle admits only arm. */
    assert(aius_measure_cancel(&c) == AIUS_INVALID_ARG);
    assert(aius_measure_reclaim(&c) == AIUS_INVALID_ARG);
    assert(aius_measure_begin_analysis(&c) == AIUS_INVALID_ARG);
    assert(aius_measure_finish_analysis(&c) == AIUS_INVALID_ARG);
    assert(aius_measure_release(&c) == AIUS_INVALID_ARG);
    feed_blocks(&c, 1, 0, 0, 5); /* producer in idle: no-op */
    assert(c.state == AIUS_MEASURE_IDLE && c.cursor == 0);

    /* Double-arm refused; arm latches its parameters. */
    assert(aius_measure_arm(&c, 41, 17) == AIUS_OK);
    assert(aius_measure_arm(&c, 42, 18) == AIUS_INVALID_ARG);
    assert(c.armed_generation == 41 && c.armed_overruns == 17);

    /* Armed admits neither analysis nor release. */
    assert(aius_measure_begin_analysis(&c) == AIUS_INVALID_ARG);
    assert(aius_measure_release(&c) == AIUS_INVALID_ARG);

    /* Init argument checks. */
    assert(aius_measure_capture_init(NULL, cap_l, cap_r, CAP) == AIUS_INVALID_ARG);
    assert(aius_measure_capture_init(&c, NULL, cap_r, CAP) == AIUS_INVALID_ARG);
    assert(aius_measure_capture_init(&c, cap_l, cap_r, 0) == AIUS_INVALID_ARG);
    printf("ok wrong-state calls are refused\n");
}

static void test_capture_latch_failures(void)
{
    /* Generation moves on the FIRST block: armed -> failed, nothing copied. */
    AiusMeasureCapture c = fresh_armed(7, 3);
    memset(cap_l, 0, sizeof cap_l);
    feed_blocks(&c, 1, 8, 3, 555);
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(c.fail_reason == AIUS_MEASURE_FAIL_GENERATION);
    assert(c.cursor == 0 && cap_l[0] == 0); /* the failing block is not copied */

    /* Overrun moves mid-capture: capturing -> failed at the block boundary. */
    c = fresh_armed(7, 3);
    feed_blocks(&c, 2, 7, 3, 555);
    assert(c.state == AIUS_MEASURE_CAPTURING);
    feed_blocks(&c, 1, 7, 4, 555);
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(c.fail_reason == AIUS_MEASURE_FAIL_OVERRUN);
    assert(c.cursor == 2 * BLOCK);

    /* failed -> idle, and the machine is reusable. */
    assert(aius_measure_release(&c) == AIUS_OK);
    assert(aius_measure_arm(&c, 9, 9) == AIUS_OK);
    feed_blocks(&c, 4, 9, 9, 1);
    assert(c.state == AIUS_MEASURE_CAPTURED);
    printf("ok arm-time latches retire the capture (generation, overrun)\n");
}

static void test_capture_cancel_isr_ack(void)
{
    /* Cancel from armed, producer acknowledges. */
    AiusMeasureCapture c = fresh_armed(1, 1);
    assert(aius_measure_cancel(&c) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_CANCELLING);
    feed_blocks(&c, 1, 1, 1, 77);
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(c.fail_reason == AIUS_MEASURE_FAIL_CANCELLED);
    assert(c.cursor == 0); /* the acknowledging block is not copied */

    /* Cancel from capturing likewise. */
    c = fresh_armed(1, 1);
    feed_blocks(&c, 1, 1, 1, 77);
    assert(c.state == AIUS_MEASURE_CAPTURING);
    assert(aius_measure_cancel(&c) == AIUS_OK);
    feed_blocks(&c, 1, 1, 1, 77);
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(c.fail_reason == AIUS_MEASURE_FAIL_CANCELLED);
    assert(c.cursor == BLOCK);
    printf("ok cancel: producer acknowledges the token at its next block\n");
}

static void test_capture_cancel_dual_terminal(void)
{
    /* Interleaving 1 -- the producer acknowledgement lands between the
     * epoch's observation of `cancelling` and its reclaim store: reclaim
     * arrives on an already-failed capture, same terminal state, first
     * reason kept, AIUS_OK. */
    AiusMeasureCapture c = fresh_armed(1, 1);
    assert(aius_measure_cancel(&c) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_CANCELLING); /* the epoch's check ... */
    feed_blocks(&c, 1, 1, 1, 0);                /* ... producer runs ... */
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(aius_measure_reclaim(&c) == AIUS_OK); /* ... the epoch's store */
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(c.fail_reason == AIUS_MEASURE_FAIL_CANCELLED);

    /* Interleaving 2 -- the controller reclaims first after a producer stall,
     * and a stale producer block lands afterwards: ignored in failed. */
    c = fresh_armed(1, 1);
    assert(aius_measure_cancel(&c) == AIUS_OK);
    assert(aius_measure_reclaim(&c) == AIUS_OK);
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(c.fail_reason == AIUS_MEASURE_FAIL_RECLAIMED);
    feed_blocks(&c, 1, 1, 1, 0);
    assert(c.state == AIUS_MEASURE_FAILED);
    assert(c.fail_reason == AIUS_MEASURE_FAIL_RECLAIMED);

    /* And once the epoch releases, the retired capture is gone: a straggler
     * block cannot act on idle either. */
    assert(aius_measure_release(&c) == AIUS_OK);
    feed_blocks(&c, 1, 1, 1, 0);
    assert(c.state == AIUS_MEASURE_IDLE && c.cursor == 0);
    printf("ok dual-terminal cancel edge: both interleavings are benign\n");
}

/* --- analysis known answers ------------------------------------------------- */

enum { WIN = 8192 };
static int32_t window_buf[WIN];

/* Feed the window in the target's declared slice size; the figures must be
 * identical whether sliced or fed whole (pinned by test_slicing_invariance). */
static void analyse(AiusMeasureFigures *fig, const int32_t *s, double f0,
                    const double *probe_hz, uint32_t probes)
{
    AiusMeasureAnalysis a;
    assert(aius_measure_analysis_init(&a, FS, f0, WIN, probe_hz, probes) ==
           AIUS_OK);
    for (uint32_t off = 0; off < WIN; off += AIUS_MEASURE_SLICE_FRAMES)
        assert(aius_measure_analysis_slice(&a, s + off,
                                           AIUS_MEASURE_SLICE_FRAMES) == AIUS_OK);
    assert(aius_measure_analysis_finish(&a, fig) == AIUS_OK);
}

/* THE CANARY. A pure 24-bit sine's THD+N is its quantization floor, about
 * -140 dB re a -6 dBFS fundamental. The rejected leakage-bin method reads
 * ~-40 dB here (Hann leakage), and a sign of any windowing error, oscillator
 * drift, or fit bias is this number rising. NEVER widen the -130 bound. */
static void test_pure_sine_canary(void)
{
    double amp = 0.5 * (double)AIUS_DSP_SAMPLE_MAX; /* -6.02 dBFS */
    synth(window_buf, WIN, amp, 440.0, 0.0, 0.0, 0.0);
    AiusMeasureFigures fig;
    analyse(&fig, window_buf, 440.0, NULL, 0);

    assert(close_to(fig.fundamental_dbfs, -6.02, 0.01));
    assert(fig.thd_n_db < -130.0);
    assert(fig.thd_n_db > -160.0); /* it is a real floor, not a zero underflow */
    assert(fig.snr_db > 130.0);
    assert(fig.thd_db < -110.0);
    assert(fig.harmonics == AIUS_MEASURE_HARMONICS); /* 440*10 < 24 kHz */
    assert(fig.dc_dbfs < -120.0);
    assert(fig.frames == WIN);
    printf("ok pure-sine canary (THD+N %.1f dB, SNR %.1f dB)\n",
           fig.thd_n_db, fig.snr_db);
}

/* SNR excludes harmonics 2..10, THD+N does not: on sine + -60 dB third
 * harmonic the two figures differ by ~80 dB. This is the definitional split
 * the design insists on, held by an 80 dB margin. */
static void test_third_harmonic_splits_the_figures(void)
{
    double amp = 0.5 * (double)AIUS_DSP_SAMPLE_MAX;
    synth(window_buf, WIN, amp, 440.0, amp * 1e-3, 0.0, 0.0); /* -60 dBc 3rd */
    AiusMeasureFigures fig;
    analyse(&fig, window_buf, 440.0, NULL, 0);

    assert(close_to(fig.thd_n_db, -60.0, 0.05));
    assert(close_to(fig.thd_db, -60.0, 0.05));
    assert(close_to(fig.harmonic_dbc[1], -60.0, 0.05)); /* [1] = 3rd */
    assert(fig.harmonic_dbc[0] < -110.0);               /* [0] = 2nd, absent */
    /* The harmonic is excluded from SNR, to the accuracy of its own Goertzel
     * estimate: the fundamental's Hann leakage 150 bins away biases the 3rd's
     * amplitude by ~-90 dB of itself, so the subtraction leaves ~1e-9 of the
     * harmonic's power behind and SNR tops out near 100 dB here rather than
     * at the -140 dB quantization floor. The definitional split from THD+N
     * (~40 dB here) is the claim under test, not the exclusion's depth. */
    assert(fig.snr_db > 90.0);

    /* The contrast, which is the actual negative control: the analyser must
     * SEE the -60 dB harmonic -- an estimator that still reported the pure
     * sine's -140 dB here would pass the canary by being blind, not by being
     * right. */
    assert(fig.thd_n_db > -70.0);
    printf("ok -60 dB third harmonic (THD+N %.2f dB, THD %.2f dBc, SNR %.1f dB)\n",
           fig.thd_n_db, fig.thd_db, fig.snr_db);
}

static void test_sine_in_noise_snr(void)
{
    /* Uniform noise at -80 dB re the fundamental power: amplitude A_n with
     * A_n^2/3 = 1e-8 * amp^2/2. */
    double amp = 0.5 * (double)AIUS_DSP_SAMPLE_MAX;
    double noise_amp = sqrt(3.0e-8 / 2.0) * amp;
    prng_state = 0x41555331u;
    synth(window_buf, WIN, amp, 440.0, 0.0, 0.0, noise_amp);
    AiusMeasureFigures fig;
    analyse(&fig, window_buf, 440.0, NULL, 0);

    /* One 8192-sample window estimates white-noise power to ~0.1 dB. */
    assert(close_to(fig.thd_n_db, -80.0, 0.3));
    assert(close_to(fig.snr_db, 80.0, 0.3));
    assert(fig.thd_db < -90.0); /* only the noise landing in 9 Hann bins */
    printf("ok sine in -80 dB noise (THD+N %.2f dB, SNR %.2f dB)\n",
           fig.thd_n_db, fig.snr_db);
}

static void test_multitone_probes(void)
{
    /* Two tones: 1 kHz at -20 dBFS (the fit target) and 10 kHz at -26 dBFS,
     * read back through the probe bins. 1 kHz and 10 kHz are >1500 Hann bins
     * apart at N = 8192, so cross-leakage is irrelevant. */
    double a1 = 0.1 * (double)AIUS_DSP_SAMPLE_MAX;
    double a2 = 0.05 * (double)AIUS_DSP_SAMPLE_MAX;
    for (uint32_t i = 0; i < WIN; i++) {
        double t = (double)i / FS;
        window_buf[i] = quantize(a1 * sin(2.0 * M_PI * 1000.0 * t) +
                                 a2 * sin(2.0 * M_PI * 10000.0 * t));
    }
    double probes[2] = {1000.0, 10000.0};
    AiusMeasureFigures fig;
    analyse(&fig, window_buf, 1000.0, probes, 2);

    assert(fig.probes == 2);
    assert(close_to(fig.probe_dbfs[0], -20.0, 0.05));
    assert(close_to(fig.probe_dbfs[1], -26.02, 0.05));
    assert(close_to(fig.fundamental_dbfs, -20.0, 0.05));
    printf("ok multitone probes (%.2f, %.2f dBFS)\n", fig.probe_dbfs[0],
           fig.probe_dbfs[1]);
}

static void test_harmonics_drop_at_nyquist(void)
{
    /* f0 = 6 kHz: only the 2nd (12 kHz) and 3rd (18 kHz) sit below 24 kHz. */
    AiusMeasureAnalysis a;
    assert(aius_measure_analysis_init(&a, FS, 6000.0, WIN, NULL, 0) == AIUS_OK);
    assert(a.harmonics == 2);

    synth(window_buf, WIN, 0.25 * (double)AIUS_DSP_SAMPLE_MAX, 6000.0,
          0.0, 0.0, 0.0);
    for (uint32_t off = 0; off < WIN; off += AIUS_MEASURE_SLICE_FRAMES)
        assert(aius_measure_analysis_slice(&a, window_buf + off,
                                           AIUS_MEASURE_SLICE_FRAMES) == AIUS_OK);
    AiusMeasureFigures fig;
    assert(aius_measure_analysis_finish(&a, &fig) == AIUS_OK);
    assert(fig.harmonics == 2);
    assert(fig.thd_n_db < -125.0); /* the truncated bin set still reads clean */
    printf("ok harmonics drop at Nyquist (2 of 9 at f0 = 6 kHz)\n");
}

static void test_slicing_invariance(void)
{
    /* One 8192-sample call and 32 x 256-sample calls accumulate the identical
     * operation sequence, so the figures are bit-identical -- the determinism
     * pin that lets host and target disagree about slicing without
     * disagreeing about figures. */
    synth(window_buf, WIN, 0.3 * (double)AIUS_DSP_SAMPLE_MAX, 440.0,
          1000.0, 500.0, 0.0);
    AiusMeasureAnalysis a1, a2;
    AiusMeasureFigures f1, f2;
    assert(aius_measure_analysis_init(&a1, FS, 440.0, WIN, NULL, 0) == AIUS_OK);
    assert(aius_measure_analysis_slice(&a1, window_buf, WIN) == AIUS_OK);
    assert(aius_measure_analysis_finish(&a1, &f1) == AIUS_OK);

    assert(aius_measure_analysis_init(&a2, FS, 440.0, WIN, NULL, 0) == AIUS_OK);
    for (uint32_t off = 0; off < WIN; off += AIUS_MEASURE_SLICE_FRAMES)
        assert(aius_measure_analysis_slice(&a2, window_buf + off,
                                           AIUS_MEASURE_SLICE_FRAMES) == AIUS_OK);
    assert(aius_measure_analysis_finish(&a2, &f2) == AIUS_OK);

    assert(memcmp(&f1, &f2, sizeof f1) == 0);
    printf("ok slicing invariance (whole window == 32 x 256, bit-identical)\n");
}

static void test_analysis_guards(void)
{
    AiusMeasureAnalysis a;
    AiusMeasureFigures fig;
    double bad_probe = -1.0;
    assert(aius_measure_analysis_init(NULL, FS, 440.0, WIN, NULL, 0) ==
           AIUS_INVALID_ARG);
    assert(aius_measure_analysis_init(&a, 0.0, 440.0, WIN, NULL, 0) ==
           AIUS_INVALID_ARG);
    assert(aius_measure_analysis_init(&a, FS, 0.0, WIN, NULL, 0) ==
           AIUS_INVALID_ARG);
    assert(aius_measure_analysis_init(&a, FS, FS / 2.0, WIN, NULL, 0) ==
           AIUS_INVALID_ARG);
    assert(aius_measure_analysis_init(&a, FS, 440.0, 0, NULL, 0) ==
           AIUS_INVALID_ARG);
    assert(aius_measure_analysis_init(&a, FS, 440.0, WIN, NULL, 1) ==
           AIUS_INVALID_ARG);
    assert(aius_measure_analysis_init(&a, FS, 440.0, WIN, &bad_probe, 1) ==
           AIUS_INVALID_ARG);

    /* A short window must refuse to publish: the figures would describe a
     * measurement that did not happen as declared. */
    assert(aius_measure_analysis_init(&a, FS, 440.0, WIN, NULL, 0) == AIUS_OK);
    assert(aius_measure_analysis_slice(&a, window_buf, WIN / 2) == AIUS_OK);
    assert(aius_measure_analysis_finish(&a, &fig) == AIUS_INVALID_ARG);
    /* Overfeeding truncates at the planned window and then publishes. */
    assert(aius_measure_analysis_slice(&a, window_buf + WIN / 2, WIN) == AIUS_OK);
    assert(aius_measure_analysis_finish(&a, &fig) == AIUS_OK);
    assert(fig.frames == WIN);
    printf("ok analysis guards (short window refused, overfeed truncated)\n");
}

/* --- capture + analysis end to end ------------------------------------------ */

static int32_t big_l[AIUS_MEASURE_CAPTURE_FRAMES];
static int32_t big_r[AIUS_MEASURE_CAPTURE_FRAMES];

static void test_end_to_end_stereo_capture(void)
{
    /* The full target-shaped pass: a 16384-frame capture fed in 32 x 512
     * blocks (L 440 Hz at -6 dBFS, R 660 Hz at -12 dBFS -- the two-tone
     * vector's channel split), analysis over the LAST 8192 frames of each
     * plane, per channel at its own f0. */
    static int32_t src_l[AIUS_MEASURE_CAPTURE_FRAMES];
    static int32_t src_r[AIUS_MEASURE_CAPTURE_FRAMES];
    synth(src_l, AIUS_MEASURE_CAPTURE_FRAMES,
          0.5 * (double)AIUS_DSP_SAMPLE_MAX, 440.0, 0.0, 0.0, 0.0);
    synth(src_r, AIUS_MEASURE_CAPTURE_FRAMES,
          0.25 * (double)AIUS_DSP_SAMPLE_MAX, 660.0, 0.0, 0.0, 0.0);

    AiusMeasureCapture c;
    assert(aius_measure_capture_init(&c, big_l, big_r,
                                     AIUS_MEASURE_CAPTURE_FRAMES) == AIUS_OK);
    assert(aius_measure_arm(&c, 12, 0) == AIUS_OK);
    for (uint32_t off = 0; off < AIUS_MEASURE_CAPTURE_FRAMES; off += BLOCK)
        aius_measure_producer_block(&c, src_l + off, src_r + off, BLOCK, 12, 0);
    assert(c.state == AIUS_MEASURE_CAPTURED);

    assert(aius_measure_begin_analysis(&c) == AIUS_OK);
    uint32_t start = AIUS_MEASURE_CAPTURE_FRAMES - AIUS_MEASURE_WINDOW_FRAMES;
    AiusMeasureFigures fl, fr;
    analyse(&fl, big_l + start, 440.0, NULL, 0);
    analyse(&fr, big_r + start, 660.0, NULL, 0);
    assert(aius_measure_finish_analysis(&c) == AIUS_OK);
    assert(aius_measure_release(&c) == AIUS_OK);

    assert(close_to(fl.fundamental_dbfs, -6.02, 0.01));
    assert(close_to(fr.fundamental_dbfs, -12.04, 0.01));
    assert(fl.thd_n_db < -130.0);
    assert(fr.thd_n_db < -125.0); /* quieter tone, same absolute quant floor */
    printf("ok end-to-end stereo capture (L %.1f dB, R %.1f dB THD+N)\n",
           fl.thd_n_db, fr.thd_n_db);
}

int main(void)
{
    test_capture_happy_path();
    test_capture_wrong_state_calls();
    test_capture_latch_failures();
    test_capture_cancel_isr_ack();
    test_capture_cancel_dual_terminal();
    test_pure_sine_canary();
    test_third_harmonic_splits_the_figures();
    test_sine_in_noise_snr();
    test_multitone_probes();
    test_harmonics_drop_at_nyquist();
    test_slicing_invariance();
    test_analysis_guards();
    test_end_to_end_stereo_capture();
    printf("measure: all tests passed\n");
    return 0;
}
