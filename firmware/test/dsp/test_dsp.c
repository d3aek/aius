/* Known-answer tests for the Aius output DSP (downmix + biquad EQ).
 *
 * Built and run by scripts/test.sh --suite dsp. Deterministic double-precision
 * arithmetic: every expectation is computed analytically. Runs under
 * ASan/UBSan via the shared native cflags.
 */

#include "aius_dsp.h"

/* The 24-bit S/PDIF transmit ring, from the output path that consumes this
 * module's result. Header-only and Arduino-free by construction, so this suite
 * compiles it with no link-line change; the include is relative because
 * lib/aius_audio is not on the dsp suite's -I set (and lib/aius_audio/include
 * holds the retained PJRC snapshot's headers, not ours). */
#include "../../lib/aius_audio/src/aius_spdif_tx24.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0

/* Evaluate a normalized biquad's real-axis transfer function H(z) at z (=+1 for
 * DC, -1 for Nyquist). */
static double h_at(const AiusBiquad *q, double z)
{
    double zn1 = 1.0 / z;
    double zn2 = zn1 * zn1;
    double num = q->b0 + q->b1 * zn1 + q->b2 * zn2;
    double den = 1.0 + q->a1 * zn1 + q->a2 * zn2;
    return num / den;
}

static int close(double a, double b, double tol)
{
    double d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

static int32_t peak_abs_i(const int32_t *s, uint32_t n)
{
    int32_t pk = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t a = s[i] < 0 ? -s[i] : s[i];
        if (a > pk) pk = a;
    }
    return pk;
}

/* --- downmix known answers ------------------------------------------------ */

static void test_stereo_passthrough(void)
{
    int32_t l[4] = {100, -200, 8388607, -8388608};
    int32_t r[4] = {-1, 2, -3, 4};
    int32_t ol[4], or_[4];
    AiusPcmBlock in = {{l, r, 0, 0, 0, 0}, 2, 4, 48000,
                       AIUS_CH_FL | AIUS_CH_FR};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;

    /* Subject is ROUTING, so headroom is pinned to unity explicitly rather than
     * taken from the default (which is 0.5 -- see test_shipped_defaults). A
     * routing test that silently depended on the default level would fail for the
     * wrong reason the next time the level moved. */
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0;
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    assert(out.channels == 2);
    assert(out.channel_mask == (AIUS_CH_FL | AIUS_CH_FR));
    assert(out.frames == 4);
    assert(out.sample_rate_hz == 48000);
    for (int i = 0; i < 4; i++) {
        assert(ol[i] == l[i]); /* exact passthrough */
        assert(or_[i] == r[i]);
    }
    printf("ok stereo passthrough\n");
}

static void test_mono_to_both(void)
{
    int32_t m[3] = {1234, -5678, 42};
    int32_t ol[3], or_[3];
    AiusPcmBlock in = {{m, 0, 0, 0, 0, 0}, 1, 3, 48000, AIUS_CH_FC};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;

    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0; /* routing test: see test_stereo_passthrough */
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    for (int i = 0; i < 3; i++) {
        assert(ol[i] == m[i]); /* unity replicate */
        assert(or_[i] == m[i]);
    }
    printf("ok mono to both\n");
}

/* FC maps equally to L and R at the documented center gain. */
static void test_fc_equal_lr(void)
{
    int32_t fl[1] = {0}, fr[1] = {0}, fc[1] = {1000000};
    int32_t lfe[1] = {0}, sl[1] = {0}, sr[1] = {0};
    int32_t ol[1], or_[1];
    AiusPcmBlock in = {{fl, fr, fc, lfe, sl, sr}, 6, 1, 48000,
                       AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE |
                           AIUS_CH_SL | AIUS_CH_SR};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;

    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0; /* routing test: see test_stereo_passthrough */
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    assert(ol[0] == or_[0]); /* equal into both */
    double expect = (double)fc[0] * AIUS_DSP_DOWNMIX_CENTER_DEFAULT;
    assert(fabs((double)ol[0] - expect) <= 1.0); /* at the -3 dB gain */
    printf("ok FC equal L/R at center gain (%d == %d, ~%.1f)\n",
           ol[0], or_[0], expect);
}

/* SL routes to L only; SR routes to R only. */
static void test_surround_routing(void)
{
    int32_t fl[1] = {0}, fr[1] = {0}, sl[1] = {2000000}, sr[1] = {0};
    int32_t ol[1], or_[1];
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_SL | AIUS_CH_SR;

    AiusPcmBlock in = {{fl, fr, sl, sr, 0, 0}, 4, 1, 48000, mask};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0; /* routing test: see test_stereo_passthrough */

    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    double expect = (double)sl[0] * AIUS_DSP_DOWNMIX_SURROUND_DEFAULT;
    assert(fabs((double)ol[0] - expect) <= 1.0);
    assert(or_[0] == 0); /* SL never leaks to R */

    /* Now SR only. */
    sl[0] = 0;
    sr[0] = 2000000;
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    assert(ol[0] == 0); /* SR never leaks to L */
    assert(fabs((double)or_[0] - expect) <= 1.0);
    printf("ok surround routing SL->L, SR->R\n");
}

/* Full-scale correlated input with unity headroom saturates, never wraps. */
static void test_clip_headroom(void)
{
    int32_t fl[1] = {AIUS_DSP_SAMPLE_MAX}, fr[1] = {AIUS_DSP_SAMPLE_MAX};
    int32_t fc[1] = {AIUS_DSP_SAMPLE_MAX}, lfe[1] = {AIUS_DSP_SAMPLE_MAX};
    int32_t sl[1] = {AIUS_DSP_SAMPLE_MAX}, sr[1] = {AIUS_DSP_SAMPLE_MAX};
    int32_t ol[1], or_[1];
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE |
                    AIUS_CH_SL | AIUS_CH_SR;
    AiusPcmBlock in = {{fl, fr, fc, lfe, sl, sr}, 6, 1, 48000, mask};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;

    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0; /* the case this test is named for */
    /* Worst case, with the LFE genuinely folded in: the shipped -10 dB gain
     * through the shipped 120 Hz LR4, so the sixth matrix term is present rather
     * than silently failing closed. */
    AiusLfeFold fold;
    memset(&fold, 0, sizeof fold);
    cfg.lfe_fold = &fold;
    assert(cfg.lfe == AIUS_DSP_LFE_GAIN_MINUS10DB);
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    assert(ol[0] == AIUS_DSP_SAMPLE_MAX); /* saturated, not overflowed */
    assert(or_[0] == AIUS_DSP_SAMPLE_MAX);

    /* Negative full scale saturates to MIN. */
    fl[0] = fr[0] = fc[0] = lfe[0] = sl[0] = sr[0] = AIUS_DSP_SAMPLE_MIN;
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    assert(ol[0] == AIUS_DSP_SAMPLE_MIN);
    assert(or_[0] == AIUS_DSP_SAMPLE_MIN);
    printf("ok clipping headroom holds at full scale\n");
}

/* The clip-free headroom maps correlated full scale to exactly full scale. */
static void test_clipfree_headroom(void)
{
    int32_t fl[1] = {AIUS_DSP_SAMPLE_MAX}, fr[1] = {AIUS_DSP_SAMPLE_MAX};
    int32_t fc[1] = {AIUS_DSP_SAMPLE_MAX};
    int32_t sl[1] = {AIUS_DSP_SAMPLE_MAX}, sr[1] = {AIUS_DSP_SAMPLE_MAX};
    int32_t ol[1], or_[1];
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_SL |
                    AIUS_CH_SR;
    AiusPcmBlock in = {{fl, fr, fc, sl, sr, 0}, 5, 1, 48000, mask};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;

    /* The block is 5.0 -- no LFE plane -- so the config's LFE term is pinned to
     * excluded to match. Leaving the shipped fold gain in place would put a
     * contributor the input cannot supply into the clip-free denominator and make
     * this test assert a level 1.08 dB below the one it is named for. */
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.lfe = AIUS_DSP_LFE_GAIN_EXCLUDED;
    cfg.headroom = aius_downmix_clipfree_headroom(&cfg);
    assert(cfg.headroom > 0.0 && cfg.headroom < 1.0);
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    /* L = FS*(1 + center + surround)*headroom = FS, within rounding, never over. */
    assert(ol[0] <= AIUS_DSP_SAMPLE_MAX);
    assert(ol[0] >= AIUS_DSP_SAMPLE_MAX - 1);
    printf("ok clip-free headroom maps full scale to full scale\n");
}

/* --- LFE fold-in crossover (roadmap item 2) -------------------------------- */

/* |H(f)| of a normalized biquad at `f` Hz, evaluated on the unit circle. An
 * INDEPENDENT reference computation: it reads only the public coefficients, so it
 * checks the designed filter rather than re-running the module's own evaluator. */
static double biquad_mag_hz(const AiusBiquad *q, double fs, double f)
{
    double w = 2.0 * M_PI * f / fs;
    double nr = q->b0 + q->b1 * cos(w) + q->b2 * cos(2.0 * w);
    double ni = -(q->b1 * sin(w) + q->b2 * sin(2.0 * w));
    double dr = 1.0 + q->a1 * cos(w) + q->a2 * cos(2.0 * w);
    double di = -(q->a1 * sin(w) + q->a2 * sin(2.0 * w));
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* |H(f)| of the whole cascade. */
static double fold_mag_hz(const AiusLfeFold *f, double fs, double hz)
{
    double m = 1.0;
    for (int k = 0; k < AIUS_DSP_LFE_CROSSOVER_SECTIONS; k++)
        m *= biquad_mag_hz(&f->lp[k], fs, hz);
    return m;
}

/* Downmix an LFE-only block (FL/FR silent) and return L. The isolated fold-in
 * contribution, which is what every level assertion below is about. */
static void fold_lfe_only(const AiusDownmixConfig *cfg, const int32_t *lfe,
                          uint32_t n, int32_t *outL, int32_t *outR)
{
    int32_t *zero = calloc(n, sizeof(int32_t));
    assert(zero != NULL);
    AiusPcmBlock in = {{zero, zero, (int32_t *)lfe, 0, 0, 0}, 3, n, 48000,
                       AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = outL;
    out.planes[1] = outR;
    assert(aius_dsp_downmix(cfg, &in, &out) == AIUS_OK);
    free(zero);
}

static double rms_tail(const int32_t *s, uint32_t n, uint32_t skip)
{
    double acc = 0.0;
    for (uint32_t i = skip; i < n; i++)
        acc += (double)s[i] * (double)s[i];
    return sqrt(acc / (double)(n - skip));
}

/* The designed LR4 has two exact known answers at any rate, and they are the
 * whole reason Q = 1/sqrt(2) is the right per-section value:
 *
 *   |H(DC)| == 1 exactly   -- the RBJ low-pass numerator and denominator both sum
 *                             to 2(1-cos w0)/a0 at z = 1.
 *   |H(f0)| == Q exactly   -- at z = e^jw0 the RBJ low-pass reduces to
 *                             sin^2(w0) / (2*alpha*sin(w0)) = Q. Two sections at
 *                             Q = 1/sqrt(2) therefore give exactly 0.5, i.e.
 *                             -6.02 dB at the corner, which is the LR4 alignment.
 *
 * Both are asserted as identities rather than tolerances, so a change to the
 * coefficient math or to the Q cannot pass on a loose epsilon. */
static void test_lfe_fold_design(void)
{
    AiusLfeFold f;
    assert(aius_lfe_fold_init(&f, FS, 120.0) == AIUS_OK);
    assert(f.designed_hz == 120.0);
    assert(f.designed_fs == FS);

    /* Both sections identical: the cascade is one Butterworth squared. */
    assert(f.lp[0].b0 == f.lp[1].b0 && f.lp[0].b1 == f.lp[1].b1 &&
           f.lp[0].b2 == f.lp[1].b2 && f.lp[0].a1 == f.lp[1].a1 &&
           f.lp[0].a2 == f.lp[1].a2);

    /* Unity at DC, -6.02 dB at the corner. */
    assert(close(fold_mag_hz(&f, FS, 0.0), 1.0, 1e-12));
    assert(close(biquad_mag_hz(&f.lp[0], FS, 120.0), AIUS_DSP_LFE_CROSSOVER_Q,
                 1e-12));
    assert(close(fold_mag_hz(&f, FS, 120.0), 0.5, 1e-12));

    /* Never above unity anywhere, and monotonically decreasing -- the property
     * aius_downmix_clipfree_headroom's frequency-domain bound relies on. */
    double prev = 1.0 + 1e-15;
    for (double hz = 0.0; hz < FS / 2.0; hz += 25.0) {
        double m = fold_mag_hz(&f, FS, hz);
        assert(m <= 1.0 + 1e-12);
        assert(m <= prev + 1e-12);
        prev = m;
    }

    /* 24 dB/octave in the stopband: 240 -> 480 Hz must drop ~24 dB. */
    double slope = 20.0 * log10(fold_mag_hz(&f, FS, 480.0) /
                                fold_mag_hz(&f, FS, 240.0));
    assert(slope < -23.0 && slope > -25.0);

    /* The order claim, stated as a comparison rather than an assertion about one
     * number: 2nd order would leave far more of the band the filter exists to
     * remove. 375 Hz is the DTS 64x-decimated LFE reconstruction Nyquist at
     * 48 kHz, so this is the frequency the choice was actually made against. */
    double lr4 = 20.0 * log10(fold_mag_hz(&f, FS, 375.0));
    double lr2 = 20.0 * log10(biquad_mag_hz(&f.lp[0], FS, 375.0));
    assert(lr4 < -35.0);
    assert(lr2 > -22.0);
    printf("ok LFE fold LR4 design (corner -%.2f dB, 375 Hz: LR4 %.1f dB vs "
           "one section %.1f dB, slope %.1f dB/oct)\n",
           -20.0 * log10(fold_mag_hz(&f, FS, 120.0)), lr4, lr2, slope);
}

/* The corner is the same ANALOG frequency at every rate, which is what "take fs
 * as a parameter" has to mean in practice. Both identities hold at each rate. */
static void test_lfe_fold_rate_independent(void)
{
    static const double rates[] = {32000.0, 44100.0, 48000.0, 96000.0, 192000.0};
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        AiusLfeFold f;
        assert(aius_lfe_fold_init(&f, rates[i], 120.0) == AIUS_OK);
        /* 1e-9, not the 1e-12 used at 48 kHz above, and the slack is in the
         * REFERENCE rather than in the filter. As fs/f0 grows the low-pass poles
         * crowd z = 1 (at 192 kHz, a1 = -1.99444 and a2 = +0.99445), so evaluating
         * 1 + a1 + a2 in this test's own reference cancels to ~1.5e-5 out of
         * operands near 1 and carries ~1e-11 of relative error. The designed
         * filter is exact; the textbook way to evaluate it here is not. */
        assert(close(fold_mag_hz(&f, rates[i], 0.0), 1.0, 1e-9));
        assert(close(fold_mag_hz(&f, rates[i], 120.0), 0.5, 1e-9));
        /* And the shape follows the corner, not the rate: one octave up is the
         * same attenuation at every rate. */
        assert(close(20.0 * log10(fold_mag_hz(&f, rates[i], 240.0)), -24.6, 0.3));
    }
    printf("ok LFE fold corner is rate-independent (32k..192k)\n");
}

/* An unusable design leaves the fold RESET AND UNDESIGNED, which is what the
 * downmix reads as "no usable crossover" and therefore fails closed on. */
static void test_lfe_fold_init_rejects(void)
{
    assert(aius_lfe_fold_init(NULL, FS, 120.0) == AIUS_INVALID_ARG);

    AiusLfeFold f;
    assert(aius_lfe_fold_init(&f, FS, 120.0) == AIUS_OK);
    assert(f.designed_hz == 120.0);
    /* Each rejection must also CLEAR the previously good design, so a rate change
     * to something unusable cannot leave stale coefficients running. */
    assert(aius_lfe_fold_init(&f, 0.0, 120.0) == AIUS_INVALID_ARG);
    assert(f.designed_hz == 0.0 && f.designed_fs == 0.0);

    assert(aius_lfe_fold_init(&f, FS, 0.0) == AIUS_INVALID_ARG);
    assert(aius_lfe_fold_init(&f, FS, -10.0) == AIUS_INVALID_ARG);
    assert(aius_lfe_fold_init(&f, FS, FS / 2.0) == AIUS_INVALID_ARG); /* Nyquist */
    assert(aius_lfe_fold_init(&f, FS, FS) == AIUS_INVALID_ARG);
    /* A 500 Hz corner (the schema maximum) is fine at 8 kHz but not at 800 Hz. */
    assert(aius_lfe_fold_init(&f, 8000.0, AIUS_DSP_LFE_CROSSOVER_MAX_HZ) == AIUS_OK);
    assert(aius_lfe_fold_init(&f, 800.0, AIUS_DSP_LFE_CROSSOVER_MAX_HZ) ==
           AIUS_INVALID_ARG);
    printf("ok LFE fold init rejects unusable designs and clears them\n");
}

/* NEGATIVE CONTROL 1, and the reason the feature is genuinely opt-in: lfe == 0.0
 * reproduces the pre-fold-in output BIT-EXACTLY, in every combination of the two
 * crossover fields. Without this the fold-in could be silently changing output
 * that is supposed to be untouched. */
static void test_lfe_fold_zero_gain_bit_exact(void)
{
    enum { N = 512 };
    int32_t fl[N], fr[N], fc[N], lfe[N], sl[N], sr[N];
    uint32_t seed = 0x5EEDu;
    for (int i = 0; i < N; i++) {
        int32_t *pl[6] = {fl, fr, fc, lfe, sl, sr};
        for (int c = 0; c < 6; c++) {
            seed = seed * 1103515245u + 12345u;
            pl[c][i] = (int32_t)((seed >> 9) % 4000000u) - 2000000;
        }
    }
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE |
                    AIUS_CH_SL | AIUS_CH_SR;
    AiusPcmBlock in = {{fl, fr, fc, lfe, sl, sr}, 6, N, 48000, mask};

    /* The reference: what this matrix produced before the fold-in existed --
     * computed here, from the documented formula, with no LFE term at all. */
    int32_t refL[N], refR[N];
    AiusDownmixConfig d = aius_downmix_config_default();
    for (int i = 0; i < N; i++) {
        double l = (double)fl[i] + (double)fc[i] * d.center +
                   (double)sl[i] * d.surround;
        double r = (double)fr[i] + (double)fc[i] * d.center +
                   (double)sr[i] * d.surround;
        refL[i] = (int32_t)(l * d.headroom >= 0.0 ? floor(l * d.headroom + 0.5)
                                                  : ceil(l * d.headroom - 0.5));
        refR[i] = (int32_t)(r * d.headroom >= 0.0 ? floor(r * d.headroom + 0.5)
                                                  : ceil(r * d.headroom - 0.5));
    }

    /* Four shapes of "LFE excluded", all of which must be that reference. */
    AiusLfeFold fold;
    struct { double xover; int with_state; const char *what; } cases[] = {
        {AIUS_DSP_LFE_CROSSOVER_DEFAULT_HZ, 1, "default corner + state"},
        {AIUS_DSP_LFE_CROSSOVER_DEFAULT_HZ, 0, "default corner, no state"},
        {0.0, 0, "no corner"},
        {0.0, 1, "no corner, state present"},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        int32_t ol[N], orr[N];
        AiusPcmBlock out;
        memset(&out, 0, sizeof out);
        out.planes[0] = ol;
        out.planes[1] = orr;
        AiusDownmixConfig cfg = aius_downmix_config_default();
        cfg.lfe = AIUS_DSP_LFE_GAIN_EXCLUDED;
        cfg.lfe_crossover_hz = cases[c].xover;
        memset(&fold, 0, sizeof fold);
        cfg.lfe_fold = cases[c].with_state ? &fold : NULL;
        assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
        for (int i = 0; i < N; i++) {
            assert(ol[i] == refL[i]);
            assert(orr[i] == refR[i]);
        }
        /* And nothing touched the crossover state: an excluded LFE does not even
         * advance the filter. */
        if (cases[c].with_state)
            assert(fold.state[0].x1 == 0.0 && fold.state[1].y1 == 0.0);
    }

    /* Not vacuous: the SHIPPED default (LFE folded) must differ from that
     * reference on this block, or "bit-exact at zero gain" proves nothing. */
    {
        int32_t ol[N], orr[N];
        AiusPcmBlock out;
        memset(&out, 0, sizeof out);
        out.planes[0] = ol;
        out.planes[1] = orr;
        AiusDownmixConfig cfg = aius_downmix_config_default();
        memset(&fold, 0, sizeof fold);
        cfg.lfe_fold = &fold;
        assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
        int differs = 0;
        for (int i = 0; i < N; i++)
            if (ol[i] != refL[i] || orr[i] != refR[i]) differs++;
        assert(differs > N / 4);
    }
    printf("ok LFE gain 0.0 is bit-exactly the pre-fold-in output (4 shapes)\n");
}

/* NEGATIVE CONTROL 2, and the one that matters most: the low-pass ACTUALLY
 * ATTENUATES. A fold-in with a broken (or absent, or bypassed) filter still
 * "works" -- it produces plausible audio -- and sounds worse, so the only way to
 * know the filter is doing its job is to put content above the corner in and
 * measure that it is gone.
 *
 * Content: a 1 kHz sine on the LFE plane, three octaves above the corner, at the
 * same amplitude as a 40 Hz sine in the passband. Measured against the
 * INDEPENDENTLY computed |H(f)| of the designed sections. */
static void test_lfe_fold_attenuates_above_corner(void)
{
    enum { N = 8192, SKIP = 2048 }; /* SKIP discards the settling transient */
    const double amp = 4000000.0;
    int32_t *lo = malloc(N * sizeof(int32_t));
    int32_t *hi = malloc(N * sizeof(int32_t));
    int32_t *ol = malloc(N * sizeof(int32_t));
    int32_t *orr = malloc(N * sizeof(int32_t));
    assert(lo && hi && ol && orr);
    for (int i = 0; i < N; i++) {
        lo[i] = (int32_t)floor(amp * sin(2.0 * M_PI * 40.0 * i / FS) + 0.5);
        hi[i] = (int32_t)floor(amp * sin(2.0 * M_PI * 1000.0 * i / FS) + 0.5);
    }

    AiusLfeFold fold;
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0; /* the subject is the FILTER, not the level */
    cfg.lfe = 1.0;      /* ditto: unity fold isolates |H(f)| */
    cfg.lfe_crossover_hz = AIUS_DSP_LFE_CROSSOVER_DEFAULT_HZ;
    memset(&fold, 0, sizeof fold);
    cfg.lfe_fold = &fold;

    const double expect_rms = amp / sqrt(2.0);

    /* 40 Hz: essentially unaltered (-0.107 dB by design). */
    memset(&fold, 0, sizeof fold);
    fold_lfe_only(&cfg, lo, N, ol, orr);
    double pass_rms = rms_tail(ol, N, SKIP);
    double pass_gain = pass_rms / expect_rms;
    assert(close(20.0 * log10(pass_gain),
                 20.0 * log10(fold_mag_hz(&fold, FS, 40.0)), 0.05));
    assert(pass_gain > 0.95); /* the fold-in's own band survives the filter */

    /* 1 kHz: gone. */
    memset(&fold, 0, sizeof fold);
    fold_lfe_only(&cfg, hi, N, ol, orr);
    double stop_rms = rms_tail(ol, N, SKIP);
    double stop_gain = stop_rms / expect_rms;
    double measured_db = 20.0 * log10(stop_gain);
    double designed_db = 20.0 * log10(fold_mag_hz(&fold, FS, 1000.0));
    assert(close(measured_db, designed_db, 0.5));
    assert(measured_db < -60.0); /* the blunt version of the same claim */

    /* The contrast, which is the actual negative control: the SAME signal folded
     * with the crossover bypassed comes through at full level, so the 60+ dB above
     * is attributable to the filter and not to the signal being quiet. */
    AiusDownmixConfig flat = cfg;
    flat.lfe_crossover_hz = 0.0;
    flat.lfe_fold = NULL;
    fold_lfe_only(&flat, hi, N, ol, orr);
    double unfiltered_gain = rms_tail(ol, N, SKIP) / expect_rms;
    assert(close(20.0 * log10(unfiltered_gain), 0.0, 0.05));
    assert(20.0 * log10(unfiltered_gain / stop_gain) > 60.0);

    printf("ok LFE fold attenuates above the corner (40 Hz %+.2f dB, 1 kHz "
           "%+.1f dB vs designed %+.1f dB, unfiltered %+.2f dB)\n",
           20.0 * log10(pass_gain), measured_db, designed_db,
           20.0 * log10(unfiltered_gain));
    free(lo);
    free(hi);
    free(ol);
    free(orr);
}

/* A zero corner is an explicit UNFILTERED fold: exactly the flat multiply the
 * matrix did before this change, sample for sample. Kept reachable through the
 * API (the JSON schema forbids it) so the filter's effect is measurable against
 * it -- which is what the test above does. */
static void test_lfe_fold_unfiltered_is_flat_multiply(void)
{
    enum { N = 64 };
    int32_t lfe[N];
    for (int i = 0; i < N; i++)
        lfe[i] = (i % 2 ? -1 : 1) * (int32_t)(i * 100000);
    int32_t ol[N], orr[N];

    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0;
    cfg.lfe = AIUS_DSP_LFE_GAIN_MINUS10DB;
    cfg.lfe_crossover_hz = 0.0;
    cfg.lfe_fold = NULL;
    fold_lfe_only(&cfg, lfe, N, ol, orr);
    for (int i = 0; i < N; i++) {
        double e = (double)lfe[i] * AIUS_DSP_LFE_GAIN_MINUS10DB;
        int32_t q = (int32_t)(e >= 0.0 ? floor(e + 0.5) : ceil(e - 0.5));
        assert(ol[i] == q);
        assert(orr[i] == q); /* symmetric: one mono channel, no image */
    }
    printf("ok unfiltered fold is an exact flat multiply into both channels\n");
}

/* THE MISCONFIGURATION, and it must FAIL CLOSED. A configured corner with no
 * state to hold it drops the LFE -- BS.775's own default -- rather than folding it
 * unfiltered. An unfiltered fallback would make the muddy fold reachable by
 * omission, and an error return would take the audio path down over a cold
 * config helper's missing pointer. */
static void test_lfe_fold_missing_state_fails_closed(void)
{
    enum { N = 64 };
    int32_t lfe[N];
    for (int i = 0; i < N; i++)
        lfe[i] = 3000000 + i * 1000;
    int32_t ol[N], orr[N], zl[N], zr[N], fl_[N], fr_[N];

    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0;

    /* (a) corner configured, no state -> the LFE is skipped, no error. */
    cfg.lfe_fold = NULL;
    fold_lfe_only(&cfg, lfe, N, ol, orr);

    /* (b) what "LFE excluded" looks like on the same input. */
    AiusDownmixConfig excluded = cfg;
    excluded.lfe = AIUS_DSP_LFE_GAIN_EXCLUDED;
    fold_lfe_only(&excluded, lfe, N, zl, zr);

    /* (c) what an unfiltered fallback WOULD have produced -- the wrong answer. */
    AiusDownmixConfig flat = cfg;
    flat.lfe_crossover_hz = 0.0;
    fold_lfe_only(&flat, lfe, N, fl_, fr_);

    int matches_excluded = 1, matches_flat = 1;
    for (int i = 0; i < N; i++) {
        if (ol[i] != zl[i] || orr[i] != zr[i]) matches_excluded = 0;
        if (ol[i] == fl_[i] && orr[i] == fr_[i]) continue;
        matches_flat = 0;
    }
    assert(matches_excluded); /* failed closed */
    assert(!matches_flat);    /* and not to the unfiltered fold */

    /* Same rule when the corner is unusable at the rate rather than unheld: a
     * 500 Hz corner at an 800 Hz rate cannot be designed, so the LFE is skipped
     * instead of being folded through whatever was there before. */
    AiusLfeFold fold;
    memset(&fold, 0, sizeof fold);
    AiusPcmBlock in = {{zl, zr, lfe, 0, 0, 0}, 3, N, 800,
                       AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE};
    memset(zl, 0, sizeof zl);
    memset(zr, 0, sizeof zr);
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = orr;
    AiusDownmixConfig bad = aius_downmix_config_default();
    bad.headroom = 1.0;
    bad.lfe_crossover_hz = AIUS_DSP_LFE_CROSSOVER_MAX_HZ;
    bad.lfe_fold = &fold;
    assert(aius_dsp_downmix(&bad, &in, &out) == AIUS_OK);
    for (int i = 0; i < N; i++)
        assert(ol[i] == 0 && orr[i] == 0);
    printf("ok missing/unusable crossover fails closed to LFE-excluded\n");
}

/* The fold gain is applied AFTER the filter, so the crossover state is in raw
 * sample units and doubling the gain exactly doubles the contribution. That is
 * what makes a gain change not need a state reset. */
static void test_lfe_fold_gain_applied_after_filter(void)
{
    enum { N = 2048 };
    int32_t *lfe = malloc(N * sizeof(int32_t));
    int32_t *a = malloc(N * sizeof(int32_t));
    int32_t *b = malloc(N * sizeof(int32_t));
    int32_t *tmp = malloc(N * sizeof(int32_t));
    assert(lfe && a && b && tmp);
    for (int i = 0; i < N; i++)
        lfe[i] = (int32_t)floor(3000000.0 * sin(2.0 * M_PI * 55.0 * i / FS) + 0.5);

    AiusLfeFold fold;
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0;
    cfg.lfe_crossover_hz = AIUS_DSP_LFE_CROSSOVER_DEFAULT_HZ;
    cfg.lfe_fold = &fold;

    cfg.lfe = 0.25;
    memset(&fold, 0, sizeof fold);
    fold_lfe_only(&cfg, lfe, N, a, tmp);
    cfg.lfe = 0.5;
    memset(&fold, 0, sizeof fold);
    fold_lfe_only(&cfg, lfe, N, b, tmp);

    /* 2x within one quantization step per sample (each side rounds once). */
    for (int i = 0; i < N; i++)
        assert(fabs((double)b[i] - 2.0 * (double)a[i]) <= 1.5);
    printf("ok fold gain applies after the filter (2x gain -> 2x output)\n");
    free(lfe);
    free(a);
    free(b);
    free(tmp);
}

/* Block boundaries are seamless: the crossover carries its delay memory across
 * calls, so splitting a block is bit-identical to processing it whole. A 4th-order
 * IIR makes this non-trivial -- a dropped state would show immediately. */
static void test_lfe_fold_block_boundary_seamless(void)
{
    enum { N = 1024, H = 400 };
    int32_t *lfe = malloc(N * sizeof(int32_t));
    int32_t *whole_l = malloc(N * sizeof(int32_t));
    int32_t *whole_r = malloc(N * sizeof(int32_t));
    int32_t *split_l = malloc(N * sizeof(int32_t));
    int32_t *split_r = malloc(N * sizeof(int32_t));
    int32_t *zero = calloc(N, sizeof(int32_t));
    assert(lfe && whole_l && whole_r && split_l && split_r && zero);
    for (int i = 0; i < N; i++)
        lfe[i] = (int32_t)floor(2500000.0 * sin(2.0 * M_PI * 85.0 * i / FS) +
                                900000.0 * sin(2.0 * M_PI * 700.0 * i / FS) + 0.5);

    AiusLfeFold fold;
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0;
    cfg.lfe_fold = &fold;

    memset(&fold, 0, sizeof fold);
    fold_lfe_only(&cfg, lfe, N, whole_l, whole_r);

    memset(&fold, 0, sizeof fold);
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE;
    AiusPcmBlock ha = {{zero, zero, lfe, 0, 0, 0}, 3, H, 48000, mask};
    AiusPcmBlock hb = {{zero, zero, lfe + H, 0, 0, 0}, 3, N - H, 48000, mask};
    AiusPcmBlock oa;
    memset(&oa, 0, sizeof oa);
    oa.planes[0] = split_l;
    oa.planes[1] = split_r;
    AiusPcmBlock ob;
    memset(&ob, 0, sizeof ob);
    ob.planes[0] = split_l + H;
    ob.planes[1] = split_r + H;
    assert(aius_dsp_downmix(&cfg, &ha, &oa) == AIUS_OK);
    assert(aius_dsp_downmix(&cfg, &hb, &ob) == AIUS_OK);
    for (int i = 0; i < N; i++) {
        assert(split_l[i] == whole_l[i]);
        assert(split_r[i] == whole_r[i]);
    }
    /* Not vacuous: the second half must actually carry signal that depends on the
     * first half's state, so a zeroed-state second call would differ. */
    int nonzero = 0;
    for (int i = H; i < N; i++)
        if (whole_l[i] != 0) nonzero++;
    assert(nonzero > (N - H) / 2);
    printf("ok LFE fold is seamless across a block boundary\n");
    free(lfe);
    free(whole_l);
    free(whole_r);
    free(split_l);
    free(split_r);
    free(zero);
}

/* The corner and the rate are re-derived from the config and the block, not
 * remembered from whatever they were first: a change to either redesigns the
 * sections and resets the memory. That is the contract the runtime-rate work
 * depends on. */
static void test_lfe_fold_redesign_on_change(void)
{
    enum { N = 256 };
    int32_t lfe[N], ol[N], orr[N];
    for (int i = 0; i < N; i++)
        lfe[i] = (int32_t)floor(2000000.0 * sin(2.0 * M_PI * 200.0 * i / FS) + 0.5);

    AiusLfeFold fold;
    memset(&fold, 0, sizeof fold);
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0;
    cfg.lfe = 1.0;
    cfg.lfe_fold = &fold;

    /* First block designs at (120, 48000). */
    fold_lfe_only(&cfg, lfe, N, ol, orr);
    assert(fold.designed_hz == 120.0 && fold.designed_fs == 48000.0);
    double coeff_120 = fold.lp[0].a1;

    /* A corner change redesigns and resets. */
    cfg.lfe_crossover_hz = 80.0;
    fold_lfe_only(&cfg, lfe, N, ol, orr);
    assert(fold.designed_hz == 80.0);
    assert(fold.lp[0].a1 != coeff_120);
    assert(close(fold_mag_hz(&fold, FS, 80.0), 0.5, 1e-12));

    /* A RATE change redesigns too, from the block's own sample_rate_hz, so the
     * corner stays the same analog frequency. fold_lfe_only pins 48 kHz, so drive
     * a 96 kHz block directly. */
    int32_t *zero = calloc(N, sizeof(int32_t));
    assert(zero != NULL);
    AiusPcmBlock in96 = {{zero, zero, lfe, 0, 0, 0}, 3, N, 96000,
                         AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = orr;
    assert(aius_dsp_downmix(&cfg, &in96, &out) == AIUS_OK);
    assert(fold.designed_fs == 96000.0 && fold.designed_hz == 80.0);
    /* 1e-9 for the reference's own cancellation at fs/f0 = 1200; see
     * test_lfe_fold_rate_independent. */
    assert(close(fold_mag_hz(&fold, 96000.0, 80.0), 0.5, 1e-9));
    free(zero);
    printf("ok LFE fold redesigns on a corner or rate change\n");
}

/* reset() clears the memory and keeps the design, so a reset fold reproduces a
 * freshly designed one on the same input. */
static void test_lfe_fold_reset(void)
{
    enum { N = 512 };
    int32_t lfe[N], a_l[N], a_r[N], b_l[N], b_r[N];
    for (int i = 0; i < N; i++)
        lfe[i] = (int32_t)floor(2000000.0 * sin(2.0 * M_PI * 60.0 * i / FS) + 0.5);

    AiusLfeFold fold;
    memset(&fold, 0, sizeof fold);
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.headroom = 1.0;
    cfg.lfe_fold = &fold;

    fold_lfe_only(&cfg, lfe, N, a_l, a_r); /* designs, then runs */
    double a1 = fold.lp[0].a1;
    assert(fold.state[0].y1 != 0.0); /* memory is populated */
    aius_lfe_fold_reset(&fold);
    assert(fold.state[0].x1 == 0.0 && fold.state[0].y1 == 0.0);
    assert(fold.state[1].x1 == 0.0 && fold.state[1].y1 == 0.0);
    assert(fold.lp[0].a1 == a1);                                  /* design kept */
    assert(fold.designed_hz == 120.0 && fold.designed_fs == FS);   /* and its key */

    fold_lfe_only(&cfg, lfe, N, b_l, b_r);
    for (int i = 0; i < N; i++) {
        assert(b_l[i] == a_l[i]);
        assert(b_r[i] == a_r[i]);
    }
    aius_lfe_fold_reset(NULL); /* no-op, must not fault */
    printf("ok LFE fold reset clears memory and keeps the design\n");
}

/* --- biquad known answers ------------------------------------------------- */

static void test_biquad_gain_bypass(void)
{
    int32_t s[4] = {1000, 0, 0, 0}; /* impulse */
    AiusBiquad bp = aius_biquad_bypass();
    AiusBiquadState st;
    memset(&st, 0, sizeof st);
    aius_dsp_eq_run(&bp, 1, &st, s, 4);
    assert(s[0] == 1000 && s[1] == 0 && s[2] == 0 && s[3] == 0);
    printf("ok bypass biquad passes impulse\n");
}

/* Hand-computed Direct Form I recurrence: y[n] = x[n] + 0.5*y[n-1]. */
static void test_biquad_feedback_recurrence(void)
{
    AiusBiquad q = {1.0, 0.0, 0.0, -0.5, 0.0};
    int32_t s[4] = {1000, 0, 0, 0};
    AiusBiquadState st;
    memset(&st, 0, sizeof st);
    aius_dsp_eq_run(&q, 1, &st, s, 4);
    assert(s[0] == 1000 && s[1] == 500 && s[2] == 250 && s[3] == 125);
    printf("ok DF-I feedback recurrence 1000,500,250,125\n");
}

/* A pure-gain section: y = 0.5*x, saturating + rounding checked. */
static void test_biquad_gain_scale(void)
{
    AiusBiquad q = {0.5, 0.0, 0.0, 0.0, 0.0};
    int32_t s[3] = {100, 201, -201};
    AiusBiquadState st;
    memset(&st, 0, sizeof st);
    aius_dsp_eq_run(&q, 1, &st, s, 3);
    assert(s[0] == 50);   /* 50.0 */
    assert(s[1] == 101);  /* 100.5 -> 101 (ties away from zero) */
    assert(s[2] == -101); /* -100.5 -> -101 */
    printf("ok gain section rounds half away from zero\n");
}

/* RBJ cookbook DC/Nyquist analytic gains. */
static void test_biquad_coeff_math(void)
{
    AiusBiquad lp = aius_biquad_lowpass(FS, 1000.0, 0.7071);
    assert(close(h_at(&lp, 1.0), 1.0, 1e-9));  /* DC gain 1 */
    assert(close(h_at(&lp, -1.0), 0.0, 1e-9)); /* Nyquist gain 0 */

    AiusBiquad hp = aius_biquad_highpass(FS, 1000.0, 0.7071);
    assert(close(h_at(&hp, 1.0), 0.0, 1e-9));  /* DC gain 0 */
    assert(close(h_at(&hp, -1.0), 1.0, 1e-9)); /* Nyquist gain 1 */

    AiusBiquad pk = aius_biquad_peaking(FS, 1000.0, 1.0, 6.0);
    assert(close(h_at(&pk, 1.0), 1.0, 1e-9));  /* unity away from f0 */
    assert(close(h_at(&pk, -1.0), 1.0, 1e-9));

    /* Low shelf DC gain = A^2 = 10^(gain_db/20); Nyquist gain = 1. */
    double gain_db = 6.0;
    AiusBiquad ls = aius_biquad_lowshelf(FS, 1000.0, 0.7071, gain_db);
    assert(close(h_at(&ls, 1.0), pow(10.0, gain_db / 20.0), 1e-9));
    assert(close(h_at(&ls, -1.0), 1.0, 1e-9));

    /* High shelf: DC gain = 1; Nyquist gain = 10^(gain_db/20). */
    AiusBiquad hs = aius_biquad_highshelf(FS, 1000.0, 0.7071, gain_db);
    assert(close(h_at(&hs, 1.0), 1.0, 1e-9));
    assert(close(h_at(&hs, -1.0), pow(10.0, gain_db / 20.0), 1e-9));
    printf("ok RBJ biquad DC/Nyquist gains\n");
}

/* A lowpass settles to unity DC gain on a constant input (steady-state check
 * through the run loop). */
static void test_biquad_dc_settle(void)
{
    AiusBiquad lp = aius_biquad_lowpass(FS, 2000.0, 0.7071);
    enum { N = 4096 };
    static int32_t s[N];
    for (int i = 0; i < N; i++) {
        s[i] = 1000000;
    }
    AiusBiquadState st;
    memset(&st, 0, sizeof st);
    aius_dsp_eq_run(&lp, 1, &st, s, N);
    assert(abs(s[N - 1] - 1000000) <= 1); /* DC passes at unity */
    printf("ok lowpass settles to unity DC (%d)\n", s[N - 1]);
}

/* --- process + arena ------------------------------------------------------ */

/* process with a bypass EQ equals bare downmix. */
static void test_process_bypass_equals_downmix(void)
{
    int32_t fl[2] = {111, -222}, fr[2] = {333, -444}, fc[2] = {555, 666};
    int32_t ol[2], or_[2], dl[2], dr[2];
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC;
    AiusPcmBlock in = {{fl, fr, fc, 0, 0, 0}, 3, 2, 48000, mask};

    AiusPcmBlock dm;
    memset(&dm, 0, sizeof dm);
    dm.planes[0] = dl;
    dm.planes[1] = dr;
    AiusDownmixConfig dcfg = aius_downmix_config_default();
    assert(aius_dsp_downmix(&dcfg, &in, &dm) == AIUS_OK);

    AiusBiquad eq[1];
    AiusBiquadState state[2];
    memset(state, 0, sizeof state);
    size_t nsec = aius_eq_default(eq, 1);
    assert(nsec == 1);

    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;
    AiusDspConfig cfg = {aius_downmix_config_default(), eq, nsec, state, NULL,
                         NULL};
    assert(aius_dsp_process(&cfg, &in, &out) == AIUS_OK);
    for (int i = 0; i < 2; i++) {
        assert(ol[i] == dl[i]);
        assert(or_[i] == dr[i]);
    }
    printf("ok process(bypass EQ) == downmix\n");
}

/* --- input stage: trim + EQ ----------------------------------------------- */

static void test_db_to_linear(void)
{
    assert(close(aius_db_to_linear(0.0), 1.0, 1e-12));
    assert(close(aius_db_to_linear(-60.0), 0.001, 1e-12));          /* 10^-3 */
    assert(close(aius_db_to_linear(12.0), pow(10.0, 0.6), 1e-12));  /* ~3.981 */
    assert(close(aius_db_to_linear(6.0), pow(10.0, 0.3), 1e-12));
    printf("ok db_to_linear endpoints\n");
}

/* Trim gain across the [-60, 12] dB endpoints, single quantize, saturation. */
static void test_input_trim_gain(void)
{
    /* Unity: exact passthrough including full scale. */
    {
        int32_t s[3] = {1000, -2000, AIUS_DSP_SAMPLE_MAX};
        AiusInputChannelConfig ch = aius_input_channel_default();
        aius_dsp_input_run(&ch, s, 3);
        assert(s[0] == 1000 && s[1] == -2000 && s[2] == AIUS_DSP_SAMPLE_MAX);
    }
    /* -60 dB (0.001): 1_000_000 -> 1000. */
    {
        int32_t s[1] = {1000000};
        AiusInputChannelConfig ch = aius_input_channel_default();
        ch.gain_linear = aius_db_to_linear(-60.0);
        aius_dsp_input_run(&ch, s, 1);
        assert(s[0] == 1000);
    }
    /* +12 dB (~3.981): 1000 -> 3981 (round half away from zero). */
    {
        int32_t s[1] = {1000};
        AiusInputChannelConfig ch = aius_input_channel_default();
        ch.gain_linear = aius_db_to_linear(12.0);
        aius_dsp_input_run(&ch, s, 1);
        int32_t expect = (int32_t)floor(1000.0 * aius_db_to_linear(12.0) + 0.5);
        assert(s[0] == expect);
    }
    /* +12 dB saturates a near-full-scale sample instead of wrapping. */
    {
        int32_t s[1] = {AIUS_DSP_SAMPLE_MAX};
        AiusInputChannelConfig ch = aius_input_channel_default();
        ch.gain_linear = aius_db_to_linear(12.0);
        aius_dsp_input_run(&ch, s, 1);
        assert(s[0] == AIUS_DSP_SAMPLE_MAX);
    }
    printf("ok input trim gain endpoints + saturation\n");
}

/* Trim folds into the biquad input: a unity net cascade is a passthrough with a
 * single quantize (no intermediate double-rounding). */
static void test_input_trim_folds_into_eq(void)
{
    /* gain 4.0 into a 0.25 pure-gain section -> net unity. */
    int32_t s[3] = {123, -457, 900001};
    AiusBiquad sec[1] = {{0.25, 0.0, 0.0, 0.0, 0.0}};
    AiusBiquadState st[1];
    memset(st, 0, sizeof st);
    AiusInputChannelConfig ch = aius_input_channel_default();
    ch.gain_linear = 4.0;
    ch.eq = sec;
    ch.eq_sections = 1;
    ch.eq_state = st;
    aius_dsp_input_run(&ch, s, 3);
    assert(s[0] == 123 && s[1] == -457 && s[2] == 900001);
    printf("ok input trim folds into EQ (single quantize)\n");
}

/* Two-section input EQ runs as a cascade; two unity-net sections pass through. */
static void test_input_two_section_eq(void)
{
    int32_t s[4] = {1000, 2000, -3000, 4000};
    AiusBiquad sec[2] = {{2.0, 0.0, 0.0, 0.0, 0.0}, {0.5, 0.0, 0.0, 0.0, 0.0}};
    AiusBiquadState st[2];
    memset(st, 0, sizeof st);
    AiusInputChannelConfig ch = aius_input_channel_default();
    ch.eq = sec;
    ch.eq_sections = 2;
    ch.eq_state = st;
    aius_dsp_input_run(&ch, s, 4);
    assert(s[0] == 1000 && s[1] == 2000 && s[2] == -3000 && s[3] == 4000);

    /* A real lowpass section still settles to unity DC through the input path. */
    AiusBiquad lp[1] = {aius_biquad_lowpass(FS, 2000.0, 0.7071)};
    AiusBiquadState lst[1];
    memset(lst, 0, sizeof lst);
    enum { N = 4096 };
    static int32_t d[N];
    for (int i = 0; i < N; i++) d[i] = 1000000;
    AiusInputChannelConfig lc = aius_input_channel_default();
    lc.eq = lp;
    lc.eq_sections = 1;
    lc.eq_state = lst;
    aius_dsp_input_run(&lc, d, N);
    assert(abs(d[N - 1] - 1000000) <= 1);
    printf("ok input two-section EQ cascade + DC settle\n");
}

/* An identity input stage in process leaves the downmix bit-identical. */
static void test_process_input_identity_equals_downmix(void)
{
    int32_t fl[3] = {111, -222, 333}, fr[3] = {444, -555, 666},
            fc[3] = {777, -888, 999};
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC;
    AiusPcmBlock in = {{fl, fr, fc, 0, 0, 0}, 3, 3, 48000, mask};

    int32_t dl[3], dr[3];
    AiusPcmBlock dm;
    memset(&dm, 0, sizeof dm);
    dm.planes[0] = dl;
    dm.planes[1] = dr;
    AiusDownmixConfig dcfg = aius_downmix_config_default();
    assert(aius_dsp_downmix(&dcfg, &in, &dm) == AIUS_OK);

    int32_t s0[3], s1[3], s2[3];
    AiusInputStage stage = aius_input_stage_default();
    stage.scratch[0] = s0;
    stage.scratch[1] = s1;
    stage.scratch[2] = s2;

    int32_t ol[3], or_[3];
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;
    AiusDspConfig cfg = {aius_downmix_config_default(), NULL, 0, NULL, &stage,
                         NULL};
    assert(aius_dsp_process(&cfg, &in, &out) == AIUS_OK);
    for (int i = 0; i < 3; i++) {
        assert(ol[i] == dl[i]);
        assert(or_[i] == dr[i]);
    }
    /* The decoded input planes (const) were not mutated by the input stage. */
    assert(fl[0] == 111 && fl[1] == -222 && fl[2] == 333);
    assert(fc[0] == 777 && fc[1] == -888 && fc[2] == 999);
    printf("ok process(identity input stage) == downmix\n");
}

/* An input trim on FC halves its downmix contribution, matching a pre-scaled
 * downmix exactly (input stage = gain + quantize before the matrix). */
static void test_process_input_trim_scales(void)
{
    int32_t fl[2] = {100000, -100000}, fr[2] = {50000, -50000},
            fc[2] = {800000, -400000};
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC;
    AiusPcmBlock in = {{fl, fr, fc, 0, 0, 0}, 3, 2, 48000, mask};

    /* Reference: FC plane pre-scaled by 0.5 (quantized), then bare downmix. */
    int32_t fcs[2];
    for (int i = 0; i < 2; i++)
        fcs[i] = (int32_t)floor((double)fc[i] * 0.5 + 0.5);
    AiusPcmBlock refin = {{fl, fr, fcs, 0, 0, 0}, 3, 2, 48000, mask};
    int32_t rl[2], rr[2];
    AiusPcmBlock ref;
    memset(&ref, 0, sizeof ref);
    ref.planes[0] = rl;
    ref.planes[1] = rr;
    AiusDownmixConfig dcfg = aius_downmix_config_default();
    assert(aius_dsp_downmix(&dcfg, &refin, &ref) == AIUS_OK);

    /* Input stage: FC (bit ordinal 2) trimmed to 0.5. */
    int32_t s0[2], s1[2], s2[2];
    AiusInputStage stage = aius_input_stage_default();
    stage.channels[2].gain_linear = 0.5;
    stage.scratch[0] = s0;
    stage.scratch[1] = s1;
    stage.scratch[2] = s2;

    int32_t ol[2], or_[2];
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;
    AiusDspConfig cfg = {aius_downmix_config_default(), NULL, 0, NULL, &stage,
                         NULL};
    assert(aius_dsp_process(&cfg, &in, &out) == AIUS_OK);
    for (int i = 0; i < 2; i++) {
        assert(ol[i] == rl[i]);
        assert(or_[i] == rr[i]);
    }
    printf("ok process(FC trim -6 dB) halves FC contribution\n");
}

/* --- true-peak safety limiter --------------------------------------------- */

/* An EXPLICITLY disabled limiter is a pure bypass: no gain change, no delay, and
 * no true-peak FIR. (The default is now enabled -- see test_shipped_defaults --
 * so this constructs the disabled case rather than relying on it.) */
static void test_limiter_disabled_is_bypass(void)
{
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 0;
    int32_t l[4] = {10, AIUS_DSP_SAMPLE_MAX, -30, 40};
    int32_t r[4] = {50, -60, AIUS_DSP_SAMPLE_MIN, 80};
    aius_dsp_limiter_run(&lim, 48000.0, l, r, 4);
    assert(l[0] == 10 && l[1] == AIUS_DSP_SAMPLE_MAX && l[2] == -30 && l[3] == 40);
    assert(r[0] == 50 && r[1] == -60 && r[2] == AIUS_DSP_SAMPLE_MIN && r[3] == 80);
    assert(close(aius_dsp_limiter_gain_reduction_db(&lim), 0.0, 1e-12));
    printf("ok limiter explicitly disabled is a pure bypass\n");
}

/* The shipped defaults, pinned. This is the regression guard for defects 2 and 3
 * as PRODUCT decisions: headroom is modest-not-unity, and the limiter that covers
 * the residual is ON with a lookahead that actually leads its attack.
 *
 * The lookahead > attack relation is the one that matters and the one that was
 * broken (lookahead 0 against a 1 ms attack, so the reduction trailed the peak by
 * about a millisecond). Asserting the RELATION rather than only the two numbers
 * means retuning either one cannot quietly reintroduce the defect. */
static void test_shipped_defaults(void)
{
    AiusDownmixConfig d = aius_downmix_config_default();
    assert(d.headroom == 0.5); /* exact: a power of two, so no tolerance needed */
    assert(d.headroom < 1.0);  /* the defect: unity headroom clipped the corpus */
    assert(d.center == AIUS_DSP_DOWNMIX_CENTER_DEFAULT);
    assert(d.surround == AIUS_DSP_DOWNMIX_SURROUND_DEFAULT);
    /* The LFE is now FOLDED IN, not discarded. Roadmap item 2: 0.0 threw away the
     * bottom octave of film content on a device whose only output is a stereo
     * pair. -10 dB is 20 dB below A/52's +10 dB reference fold-in gain, for the
     * reasons recorded in aius_dsp.h. */
    assert(d.lfe == AIUS_DSP_LFE_GAIN_MINUS10DB);
    assert(d.lfe > 0.0);
    assert(d.lfe < 1.0); /* the reference gain is > 1; this device attenuates */
    /* And the fold is FILTERED by default: a corner is configured, so a caller
     * that supplies state gets the LR4 and a caller that does not gets no LFE.
     * Neither gets an unfiltered fold. */
    assert(d.lfe_crossover_hz == AIUS_DSP_LFE_CROSSOVER_DEFAULT_HZ);
    assert(d.lfe_crossover_hz == 120.0);
    assert(d.lfe_fold == NULL); /* caller-owned state; a value cannot carry it */
    /* Deliberately NOT the clip-free value: that costs 7.66 dB to cover a case
     * the limiter is there for. */
    assert(d.headroom > aius_downmix_clipfree_headroom(&d));
    /* The fold makes the clip-free answer stricter, and the shipped headroom is
     * still above it: 1/(1 + 0.7071 + 0.7071 + 0.316228) = 0.36626. */
    assert(close(aius_downmix_clipfree_headroom(&d),
                 1.0 / (1.0 + AIUS_DSP_DOWNMIX_CENTER_DEFAULT +
                        AIUS_DSP_DOWNMIX_SURROUND_DEFAULT +
                        AIUS_DSP_LFE_GAIN_DEFAULT), 1e-15));

    AiusLimiterConfig c = aius_limiter_config_default();
    assert(c.enabled == 1); /* modest headroom REQUIRES an active limiter */
    assert(c.threshold_dbfs == -1.0);
    assert(c.threshold_dbfs < 0.0); /* margin for the bounded attack overshoot */
    assert(c.attack_ms == 0.5);
    assert(c.release_ms == 100.0);
    assert(c.lookahead_ms == 2.0);
    /* The defect-3 relation: the reduction must LEAD the peak, by several attack
     * time constants, not trail it. */
    assert(c.lookahead_ms > 0.0);
    assert(c.lookahead_ms >= 4.0 * c.attack_ms);
    printf("ok shipped defaults (headroom %.3f, limiter on @ %.1f dBFS, "
           "lookahead %.1f ms = %.0f x attack)\n",
           d.headroom, c.threshold_dbfs, c.lookahead_ms,
           c.lookahead_ms / c.attack_ms);
}

/* Enabled, lookahead 0, all samples below threshold: exact passthrough. */
static void test_limiter_transparent_below_threshold(void)
{
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 1;
    lim.cfg.threshold_dbfs = 0.0; /* T = full scale */
    lim.cfg.lookahead_ms = 0.0;
    int32_t l[5] = {100, -200, 300000, -400000, 12345};
    int32_t r[5] = {-100, 200, -300000, 400000, -6789};
    int32_t el[5], er[5];
    memcpy(el, l, sizeof l);
    memcpy(er, r, sizeof r);
    aius_dsp_limiter_run(&lim, 48000.0, l, r, 5);
    for (int i = 0; i < 5; i++) {
        assert(l[i] == el[i]);
        assert(r[i] == er[i]);
    }
    assert(close(aius_dsp_limiter_gain_reduction_db(&lim), 0.0, 1e-12));
    printf("ok limiter transparent below threshold\n");
}

/* Constant input above threshold: gain converges so the output TRUE peak settles
 * to the threshold, and the reported gain reduction matches T/true-peak.
 *
 * This is where sample-peak and true-peak limiting visibly differ. The BS.1770-4
 * interpolation filter has a small passband ripple (about +0.012 dB), so a
 * constant full-scale input has a true peak slightly ABOVE full scale, and a
 * limiter that settles the true peak to T therefore leaves the SAMPLES a shade
 * below T. The ripple factor is measured here through aius_dsp_true_peak rather
 * than hardcoded, so the assertion describes the semantics instead of restating
 * the coefficient table. */
static void test_limiter_threshold_steady_state(void)
{
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 1;
    lim.cfg.threshold_dbfs = -6.0;
    lim.cfg.attack_ms = 1.0;
    lim.cfg.release_ms = 100.0;
    lim.cfg.lookahead_ms = 0.0;

    double T = pow(10.0, -6.0 / 20.0) * (double)AIUS_DSP_SAMPLE_MAX;
    /* 32768 samples is 683 ms, about seven release time constants. 8192 was not
     * enough: the silence-to-full-scale STEP at sample 0 has a genuine
     * inter-sample overshoot of about +0.64 dB (a step is not band-limited), the
     * detector correctly sees it, and with a 100 ms release that transient was
     * still 0.11 dB from settled at 8192 -- which is a property of the release
     * time, not of the steady state this test is about. */
    enum { N = 32768 };
    static int32_t l[N], r[N];
    for (int i = 0; i < N; i++) {
        l[i] = AIUS_DSP_SAMPLE_MAX;
        r[i] = AIUS_DSP_SAMPLE_MAX;
    }
    aius_dsp_limiter_run(&lim, 48000.0, l, r, N);

    /* Settled. A true-peak limiter pulls the SAMPLES to just under T, because the
     * detector reads the interpolated peak and that sits a hundredth of a dB above
     * the sample value for a constant. Assert the band, not the exact ripple: the
     * ripple belongs to the BS.1770-4 table, and
     * test_limiter_catches_intersample_peak is where the interpolation semantics
     * are pinned. Under a sample-peak follower these would settle exactly AT T. */
    double floor_db = T * pow(10.0, -0.05 / 20.0); /* no more than 0.05 dB under */
    assert((double)l[N - 1] <= T);
    assert((double)l[N - 1] >= floor_db);
    assert((double)r[N - 1] <= T);
    assert((double)r[N - 1] >= floor_db);
    /* The reported reduction agrees with the settled output it produced. */
    double g = pow(10.0, aius_dsp_limiter_gain_reduction_db(&lim) / 20.0);
    assert(fabs((double)l[N - 1] - (double)AIUS_DSP_SAMPLE_MAX * g) <= 2.0);
    printf("ok limiter converges output true peak to threshold "
           "(samples %d, T %.0f, %.4f dB under)\n",
           l[N - 1], T, 20.0 * log10((double)l[N - 1] / T));
}

/* Lookahead delays the signal by exactly `delay` samples (transparent content).
 */
static void test_limiter_lookahead_delay(void)
{
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 1;
    lim.cfg.threshold_dbfs = 0.0; /* below threshold -> unity gain */
    /* 5 samples at 48 kHz: 5 * 1000 / 48000 ms. */
    lim.cfg.lookahead_ms = 5.0 * 1000.0 / 48000.0;

    enum { N = 32, D = 5 };
    int32_t l[N], r[N], el[N], er[N];
    for (int i = 0; i < N; i++) {
        l[i] = (i + 1) * 1000;
        r[i] = -(i + 1) * 1000;
    }
    memcpy(el, l, sizeof l);
    memcpy(er, r, sizeof r);
    aius_dsp_limiter_run(&lim, 48000.0, l, r, N);
    for (int i = 0; i < D; i++) {
        assert(l[i] == 0 && r[i] == 0); /* primed with silence */
    }
    for (int i = D; i < N; i++) {
        assert(l[i] == el[i - D]);
        assert(r[i] == er[i - D]);
    }
    printf("ok limiter lookahead delays by exactly %d samples\n", D);
}

/* Attack pulls the gain down monotonically; release recovers it monotonically.
 *
 * MONOTONICITY IS ASSERTED TO 1e-6 dB, not exactly. A true-peak detector reads an
 * interpolated peak, and on a signal whose curvature is changing (the first few
 * samples of a fade) that reading can creep up by a relative 1e-9 as the filter
 * window fills with the new shape -- so the envelope wobbles by ~1e-8 dB. That is
 * below the resolution at which "monotone" is a meaningful claim about a gain
 * envelope, and it is orders of magnitude below the multi-dB motion this test is
 * actually about (which is still pinned exactly, by the converged-value and
 * recovered-value assertions below). A sample-peak follower was exactly monotone
 * here only because it could not see between the samples. */
#define ENV_MONOTONE_TOL_DB 1e-6

static void test_limiter_attack_release_envelope(void)
{
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 1;
    lim.cfg.threshold_dbfs = -6.0;
    lim.cfg.attack_ms = 2.0;
    lim.cfg.release_ms = 50.0;
    lim.cfg.lookahead_ms = 0.0;

    /* Attack: above-threshold input faded in, then held. Gain must be
     * non-increasing throughout, and the run is long enough to converge to the
     * steady-state reduction (~ -6 dB).
     *
     * The fade-in is what a true-peak detector requires of this test and a
     * sample-peak one did not. Switching from silence to full scale in one sample
     * is a STEP, which is not band-limited and has a real inter-sample overshoot of
     * about +0.64 dB; the detector correctly reports it, the envelope's instant
     * attack latches it, and it then releases -- so the reduction would dip and
     * recover, and "monotone attack" would be measuring the step artefact rather
     * than the attack. A 20 ms raised-cosine onset is band-limited enough that the
     * interpolated peak tracks the envelope and the assertion means what it says. */
    enum { FADE = 960 }; /* 20 ms at 48 kHz, derived from FS, not a magic count */
    double prev = 0.0;   /* gain reduction dB, starts at 0 */
    int attacked = 0;
    for (int i = 0; i < 4000; i++) {
        double env = i < FADE
                         ? 0.5 * (1.0 - cos(M_PI * (double)i / (double)FADE))
                         : 1.0;
        int32_t v = (int32_t)(env * (double)AIUS_DSP_SAMPLE_MAX);
        int32_t l = v, r = v;
        aius_dsp_limiter_run(&lim, 48000.0, &l, &r, 1);
        double db = aius_dsp_limiter_gain_reduction_db(&lim);
        assert(db <= prev + ENV_MONOTONE_TOL_DB); /* monotone non-increasing */
        if (db < -0.5) attacked = 1;
        prev = db;
    }
    assert(attacked); /* gain reduction actually engaged */
    double after_attack = prev;
    assert(after_attack < -5.5 && after_attack > -6.5); /* converged near -6 dB */

    /* Release: fade to silence, then hold it. Gain reduction must recover
     * (non-decreasing) toward 0. Faded for the same reason the onset is: cutting
     * to zero in one sample is a falling step, and the detector correctly sees the
     * ringing that trailing edge puts BETWEEN the samples, which would latch a
     * fresh reduction part-way through the release. */
    double rprev = after_attack;
    for (int i = 0; i < 20000; i++) {
        double env = i < FADE
                         ? 0.5 * (1.0 + cos(M_PI * (double)i / (double)FADE))
                         : 0.0;
        int32_t v = (int32_t)(env * (double)AIUS_DSP_SAMPLE_MAX);
        int32_t l = v, r = v;
        aius_dsp_limiter_run(&lim, 48000.0, &l, &r, 1);
        double db = aius_dsp_limiter_gain_reduction_db(&lim);
        assert(db >= rprev - ENV_MONOTONE_TOL_DB); /* monotone non-decreasing */
        rprev = db;
    }
    assert(rprev > -0.1); /* recovered essentially to unity */
    printf("ok limiter attack pulls down, release recovers\n");
}

/* A full-scale burst (as the downmix would clamp for surround51_impulse) is
 * pulled below full scale when the limiter is enabled: no output saturation. */
static void test_limiter_prevents_saturation(void)
{
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 1;
    lim.cfg.threshold_dbfs = -3.0;
    lim.cfg.attack_ms = 1.0;
    lim.cfg.release_ms = 100.0;
    lim.cfg.lookahead_ms = 5.0; /* 240 samples */

    enum { N = 1024 };
    static int32_t l[N], r[N];
    for (int i = 0; i < N; i++) {
        l[i] = 1000;
        r[i] = -1000;
    }
    /* Clamped burst, as the downmix would emit for a correlated impulse. */
    for (int k = 300; k < 303; k++) {
        l[k] = AIUS_DSP_SAMPLE_MAX;
        r[k] = AIUS_DSP_SAMPLE_MIN;
    }
    aius_dsp_limiter_run(&lim, 48000.0, l, r, N);

    int32_t pk = 0;
    for (int i = 0; i < N; i++) {
        int32_t a = l[i] < 0 ? -l[i] : l[i];
        int32_t b = r[i] < 0 ? -r[i] : r[i];
        if (a > pk) pk = a;
        if (b > pk) pk = b;
        assert(l[i] != AIUS_DSP_SAMPLE_MAX && l[i] != AIUS_DSP_SAMPLE_MIN);
        assert(r[i] != AIUS_DSP_SAMPLE_MAX && r[i] != AIUS_DSP_SAMPLE_MIN);
    }
    assert(pk < AIUS_DSP_SAMPLE_MAX);
    assert(pk < (int32_t)(0.9 * AIUS_DSP_SAMPLE_MAX)); /* pulled toward threshold */
    printf("ok limiter prevents full-scale saturation (peak %d)\n", pk);
}

/* Bounded + finite: every output stays in the 24-bit range and gain in (0, 1]. */
static void test_limiter_bounded_finite(void)
{
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 1;
    lim.cfg.threshold_dbfs = -12.0;
    lim.cfg.attack_ms = 0.5;
    lim.cfg.release_ms = 200.0;
    lim.cfg.lookahead_ms = 2.0;

    enum { N = 4096 };
    static int32_t l[N], r[N];
    uint32_t seed = 0x1234567u;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        l[i] = (int32_t)(seed >> 8) % (AIUS_DSP_SAMPLE_MAX + 1);
        seed = seed * 1103515245u + 12345u;
        r[i] = -(int32_t)((seed >> 8) % (AIUS_DSP_SAMPLE_MAX + 1));
    }
    aius_dsp_limiter_run(&lim, 48000.0, l, r, N);
    for (int i = 0; i < N; i++) {
        assert(l[i] >= AIUS_DSP_SAMPLE_MIN && l[i] <= AIUS_DSP_SAMPLE_MAX);
        assert(r[i] >= AIUS_DSP_SAMPLE_MIN && r[i] <= AIUS_DSP_SAMPLE_MAX);
    }
    double gr = aius_dsp_limiter_gain_reduction_db(&lim);
    assert(gr <= 1e-9); /* never a boost */
    printf("ok limiter output bounded + finite (gr %.2f dB)\n", gr);
}

/* Seamless across block boundaries: a split run with carried state equals the
 * whole-block run (lookahead + envelope state persist). */
static void test_limiter_block_boundary(void)
{
    enum { N = 2048 };
    static int32_t whole_l[N], whole_r[N], split_l[N], split_r[N], base_l[N],
        base_r[N];
    for (int i = 0; i < N; i++) {
        double s = sin(i * 0.05);
        base_l[i] = (int32_t)(s * 7000000.0);
        base_r[i] = (int32_t)(-s * 7000000.0);
    }
    memcpy(whole_l, base_l, sizeof base_l);
    memcpy(whole_r, base_r, sizeof base_r);
    memcpy(split_l, base_l, sizeof base_l);
    memcpy(split_r, base_r, sizeof base_r);

    AiusLimiterConfig c = aius_limiter_config_default();
    c.enabled = 1;
    c.threshold_dbfs = -6.0;
    c.attack_ms = 1.0;
    c.release_ms = 80.0;
    c.lookahead_ms = 3.0;

    AiusLimiter w;
    memset(&w, 0, sizeof w);
    w.cfg = c;
    aius_dsp_limiter_run(&w, 48000.0, whole_l, whole_r, N);

    AiusLimiter s;
    memset(&s, 0, sizeof s);
    s.cfg = c;
    uint32_t h = N / 2;
    aius_dsp_limiter_run(&s, 48000.0, split_l, split_r, h);
    aius_dsp_limiter_run(&s, 48000.0, split_l + h, split_r + h, N - h);

    for (int i = 0; i < N; i++) {
        assert(split_l[i] == whole_l[i]);
        assert(split_r[i] == whole_r[i]);
    }
    printf("ok limiter seamless across block boundary\n");
}

/* Full path: identity input stage + downmix + bypass EQ + limiter; frame count
 * preserved, output bounded. */
static void test_process_full_path(void)
{
    int32_t fl[4] = {6000000, -6000000, 100, -200};
    int32_t fr[4] = {6000000, -6000000, 300, -400};
    int32_t fc[4] = {6000000, -6000000, 500, -600};
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC;
    AiusPcmBlock in = {{fl, fr, fc, 0, 0, 0}, 3, 4, 48000, mask};

    int32_t s0[4], s1[4], s2[4];
    AiusInputStage stage = aius_input_stage_default();
    stage.scratch[0] = s0;
    stage.scratch[1] = s1;
    stage.scratch[2] = s2;

    AiusBiquad eq[1];
    AiusBiquadState est[2];
    memset(est, 0, sizeof est);
    size_t ns = aius_eq_default(eq, 1);

    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    lim.cfg.enabled = 1;
    lim.cfg.threshold_dbfs = -3.0;
    lim.cfg.attack_ms = 1.0;
    lim.cfg.release_ms = 50.0;
    lim.cfg.lookahead_ms = 0.0;

    int32_t ol[4], or_[4];
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = ol;
    out.planes[1] = or_;
    AiusDspConfig cfg = {aius_downmix_config_default(), eq, ns, est, &stage,
                         &lim};
    assert(aius_dsp_process(&cfg, &in, &out) == AIUS_OK);
    assert(out.frames == 4);
    assert(out.channels == 2);
    for (int i = 0; i < 4; i++) {
        assert(ol[i] >= AIUS_DSP_SAMPLE_MIN && ol[i] <= AIUS_DSP_SAMPLE_MAX);
        assert(or_[i] >= AIUS_DSP_SAMPLE_MIN && or_[i] <= AIUS_DSP_SAMPLE_MAX);
    }
    printf("ok process full path (input+downmix+eq+limiter)\n");
}

/* --- true-peak detection: the discriminating tests ------------------------ */

/* THE test for defect 3. A signal whose INTER-SAMPLE peak is over threshold while
 * every one of its samples sits comfortably under it -- so a sample-peak follower
 * provably does nothing, and a true-peak follower must act.
 *
 * The construction is the canonical worst case: a sinusoid at fs/4 offset 45
 * degrees, so every sample lands at +-A/sqrt(2) and the waveform peaks at A
 * exactly halfway between samples. That is +3.01 dB of inter-sample peak, all of
 * it invisible to a sample-peak detector and all of it real to the DAC's
 * reconstruction filter -- which is where the old "true-peak safety limiter"
 * name was a lie and this is the assertion that would fail if it became one
 * again. Nothing here hardcodes fs: the frequency is expressed as a fraction of
 * the rate passed to the limiter. */
static void test_truepeak_catches_intersample_peak(void)
{
    const double T = pow(10.0, AIUS_DSP_LIMITER_THRESHOLD_DEFAULT_DBFS / 20.0) *
                     (double)AIUS_DSP_SAMPLE_MAX;
    /* Samples 1.94 dB BELOW threshold; continuous peak 1.12 dB ABOVE it. */
    const double S = 0.80 * T;
    const double A = S * sqrt(2.0);

    enum { N = 4096, SETTLE = 960 /* 20 ms: past the fresh-state onset */ };
    static int32_t l[N], r[N];
    for (int n = 0; n < N; n++) {
        double v = A * cos(2.0 * M_PI * (double)n / 4.0 + M_PI / 4.0);
        l[n] = (int32_t)(v >= 0.0 ? floor(v + 0.5) : ceil(v - 0.5));
        r[n] = l[n];
    }

    /* 1. A sample-peak detector would see nothing: every sample is under T. */
    int32_t sample_pk = peak_abs_i(l, N);
    assert((double)sample_pk < T);

    /* 2. The true-peak detector sees it, and it is over threshold AND over full
     *    scale -- this is the peak that would have clipped downstream. */
    double true_pk = aius_dsp_true_peak(l, N);
    assert(true_pk > T);
    assert(true_pk > (double)AIUS_DSP_SAMPLE_MAX);
    /* The inter-sample excess is the theoretical +3.01 dB, within the BS.1770-4
     * filter's ~0.2 dB accuracy. */
    double excess_db = 20.0 * log10(true_pk / (double)sample_pk);
    assert(excess_db > 2.8 && excess_db < 3.3);

    /* 3. The limiter, at its shipped defaults, acts on it. */
    static int32_t ol[N], or_[N];
    memcpy(ol, l, sizeof l);
    memcpy(or_, r, sizeof r);
    AiusLimiter lim;
    memset(&lim, 0, sizeof lim);
    lim.cfg = aius_limiter_config_default();
    assert(lim.cfg.enabled == 1); /* on by default, or this catches nothing */
    aius_dsp_limiter_run(&lim, 48000.0, ol, or_, N);
    double gr = aius_dsp_limiter_gain_reduction_db(&lim);
    assert(gr < -0.5); /* it engaged; a sample-peak limiter would report 0.0 */

    /* 4. And the settled output true peak is held at the threshold, back inside
     *    full scale. Measured past the onset: the limiter starts from a fresh
     *    state (gain 1, envelope 0) so the first few ms pass unattenuated. */
    double out_tp = aius_dsp_true_peak(ol + SETTLE, N - SETTLE);
    assert(out_tp <= T * pow(10.0, 0.05 / 20.0)); /* at most 0.05 dB over T */
    assert(out_tp < (double)AIUS_DSP_SAMPLE_MAX); /* no longer over full scale */
    printf("ok true-peak catches inter-sample peak (samples %+.2f dB, "
           "true peak %+.2f dB vs T; gr %.2f dB -> settled %+.4f dB vs T)\n",
           20.0 * log10((double)sample_pk / T), 20.0 * log10(true_pk / T), gr,
           20.0 * log10(out_tp / T));
}

/* The FIR skip gate must not be able to miss an over. It is sound only if
 * 33142/16384 really is an upper bound on every phase's l1 norm, so measure the
 * worst phase's gain through the public interface: driving a phase with the sign
 * pattern of its own coefficients produces exactly its l1 norm times the
 * amplitude. The sign pattern is restated here independently of the table in
 * aius_dsp.c, so a table edit that changed the norm would fail here rather than
 * silently widening the window in which the gate skips. */
static void test_truepeak_gate_bound_holds(void)
{
    /* Phase 1 of BS.1770-4 Table 3, signs only, newest sample first. */
    static const int sign[AIUS_DSP_TRUEPEAK_TAPS] = {
        -1, +1, -1, +1, -1, +1, +1, -1, +1, -1, +1, -1,
    };
    const double A = 1000000.0;
    /* hist[k] is x[n-k], so the sample that lands at tap k is at index n-k. Place
     * the aligned window at the end of the buffer. */
    enum { N = 64 };
    int32_t x[N];
    memset(x, 0, sizeof x);
    for (int k = 0; k < AIUS_DSP_TRUEPEAK_TAPS; k++)
        x[N - 1 - k] = (int32_t)(sign[k] * A);

    double measured = aius_dsp_true_peak(x, N) / A;
    double bound = AIUS_DSP_TRUEPEAK_L1_NUM / AIUS_DSP_TRUEPEAK_L1_DEN;
    /* The gate's constant is a true upper bound... */
    assert(measured <= bound + 1e-9);
    /* ...and a TIGHT one: it is the worst phase, not a loose guess, so the gate
     * skips as much work as it soundly can. */
    assert(measured >= bound - 1e-9);
    printf("ok true-peak gate bound is exact (l1 %.10f == %.10f)\n",
           measured, bound);
}

/* --- 24 -> 16 bit output stage -------------------------------------------- */

/* Defect 1, first half: the conversion ROUNDS, where the old code shifted. Every
 * row is a case where `x >> 8` and round-to-nearest disagree, including the one
 * that names the bug: -1 >> 8 is -1, a whole 16-bit LSB of error on a sample that
 * should map to zero. Dither is off so the rounding rule is visible alone. */
static void test_out16_rounds_not_truncates(void)
{
    struct { int32_t in; int32_t want; int32_t shifted; } k[] = {
        {     0,      0,      0 },
        {   127,      0,      0 }, /*  0.496 -> 0 */
        {   128,      1,      0 }, /*  0.5   -> 1, ties away from zero */
        {   255,      1,      0 }, /*  0.996 -> 1; the shift throws it away */
        {   256,      1,      1 }, /*  exact */
        {    -1,      0,     -1 }, /* THE defect: a shift rounds to -1 LSB */
        {  -127,      0,     -1 },
        {  -128,     -1,     -1 }, /* -0.5   -> -1, ties away from zero */
        {  -255,     -1,     -1 },
        {  -256,     -1,     -1 }, /*  exact */
        {  -384,     -2,     -2 }, /* -1.5   -> -2, ties away from zero */
        {   384,      2,      1 }, /*  1.5   -> 2 (shift gives 1) */
    };
    int differ = 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        assert(aius_out16_sample(NULL, 0, k[i].in) == k[i].want);
        assert((k[i].in >> 8) == k[i].shifted); /* the old behaviour, for contrast */
        if (k[i].shifted != k[i].want) differ++;
    }
    /* If these two rules ever agreed everywhere, this test would be vacuous. */
    assert(differ >= 5);

    /* Saturation, not wrap: 24-bit full scale is 32767.996 in 16-bit units and
     * must clamp to 32767, never round up into -32768. */
    assert(aius_out16_sample(NULL, 0, AIUS_DSP_SAMPLE_MAX) == AIUS_DSP_OUT16_MAX);
    assert(aius_out16_sample(NULL, 0, AIUS_DSP_SAMPLE_MIN) == AIUS_DSP_OUT16_MIN);

    /* The DC offset the shift introduced, measured. Over a symmetric sweep the
     * rounded conversion has zero mean error; the shift has -0.5 LSB. */
    double err_round = 0.0, err_shift = 0.0;
    int n = 0;
    for (int32_t v = -100000; v <= 100000; v += 7) {
        double ideal = (double)v / AIUS_DSP_OUT16_LSB;
        err_round += (double)aius_out16_sample(NULL, 0, v) - ideal;
        err_shift += (double)(v >> 8) - ideal;
        n++;
    }
    err_round /= (double)n;
    err_shift /= (double)n;
    assert(fabs(err_round) < 0.01); /* unbiased */
    assert(err_shift < -0.4);       /* the -0.5 LSB the shift always cost */
    printf("ok out16 rounds not truncates (mean error %.4f LSB vs shift %.4f LSB)\n",
           err_round, err_shift);
}

/* Defect 1, second half: the conversion is DITHERED, and the dither does the one
 * thing dither is for -- it carries information that sits below the quantiser's
 * own step. An input of 100 (0.39 of a 16-bit LSB) rounds to exactly zero forever
 * without dither: the signal is simply gone. With TPDF dither the individual
 * samples still only take the values 0 and 1, but their MEAN reproduces the input
 * exactly, which is what makes the error noise instead of distortion. */
static void test_out16_dither_carries_sub_lsb(void)
{
    enum { N = 200000 };
    const int32_t level = 100; /* 100/256 = 0.390625 LSB */
    const double ideal = (double)level / AIUS_DSP_OUT16_LSB;

    /* Undithered: the sub-LSB signal vanishes completely. */
    AiusOut16Stage flat;
    aius_out16_init(&flat, AIUS_DSP_DITHER_SEED_DEFAULT, 0);
    assert(flat.dither == 0);
    for (int i = 0; i < 64; i++)
        assert(aius_out16_sample(&flat.stream[0], flat.dither, level) == 0);

    /* Dithered: mean preserved, and the output actually varies. */
    AiusOut16Stage st = aius_out16_stage_default();
    assert(st.dither == 1); /* dither is ON by default -- that is the defect fix */
    /* q = 0.390625 + d with d triangular on (-1, 1), so the reachable codes are
     * exactly {-1, 0, 1} -- and all three must actually appear, or the dither is
     * not spanning its stated amplitude. */
    double sum = 0.0;
    int saw[3] = {0, 0, 0};
    int out_of_range = 0;
    for (int i = 0; i < N; i++) {
        int32_t v = aius_out16_sample(&st.stream[0], st.dither, level);
        if (v >= -1 && v <= 1) saw[v + 1] = 1;
        else out_of_range++;
        sum += (double)v;
    }
    assert(saw[0] && saw[1] && saw[2]); /* -1, 0 and 1 all occur */
    assert(out_of_range == 0);          /* +-1 LSB TPDF reaches no further */
    double mean = sum / (double)N;
    assert(fabs(mean - ideal) < 0.01); /* ~9 sigma for this N */

    /* And the dither is bounded: a mid-scale input never moves by more than one
     * 16-bit LSB from the undithered answer. */
    const int32_t mid = 1000 * 256 + 40;
    int32_t base = aius_out16_sample(NULL, 0, mid);
    for (int i = 0; i < 20000; i++) {
        int32_t v = aius_out16_sample(&st.stream[1], 1, mid);
        assert(v >= base - 1 && v <= base + 1);
    }
    printf("ok out16 TPDF dither carries sub-LSB detail (mean %.6f ~ %.6f LSB)\n",
           mean, ideal);
}

/* The determinism half of the dither contract: random-looking, but a pure function
 * of (seed, sample index), so a bit-exact host/target path and reproducible golden
 * fixtures both survive. This is the assertion that would fail if anyone reached
 * for rand(), a clock, or the hardware RNG. */
static void test_out16_dither_deterministic(void)
{
    enum { N = 512 };
    static int32_t x24[N];
    for (int i = 0; i < N; i++)
        x24[i] = (int32_t)((i * 37) % 65536) - 32768;

    static int16_t a_l[N], a_r[N], b_l[N], b_r[N], c_l[N], c_r[N];

    AiusOut16Stage a = aius_out16_stage_default();
    aius_out16_run(&a, x24, x24, a_l, a_r, N);

    /* Same seed again -- i.e. exactly what a RESET does. Bit-identical. */
    AiusOut16Stage b = aius_out16_stage_default();
    aius_out16_run(&b, x24, x24, b_l, b_r, N);
    assert(memcmp(a_l, b_l, sizeof a_l) == 0);
    assert(memcmp(a_r, b_r, sizeof a_r) == 0);

    /* A different seed gives a different sequence, or the seed does nothing. */
    AiusOut16Stage c;
    aius_out16_init(&c, AIUS_DSP_DITHER_SEED_DEFAULT ^ 0xA5A5A5A5u, 1);
    aius_out16_run(&c, x24, x24, c_l, c_r, N);
    assert(memcmp(a_l, c_l, sizeof a_l) != 0);

    /* L and R are INDEPENDENT streams: identical input on both channels must not
     * produce identical dither, or the noise would image dead centre. */
    assert(memcmp(a_l, a_r, sizeof a_l) != 0);
    /* ...but both must still track the signal: never more than 1 LSB apart. */
    for (int i = 0; i < N; i++) {
        int d = (int)a_l[i] - (int)a_r[i];
        assert(d >= -2 && d <= 2);
    }

    /* Block form and per-sample form agree, so a test that pins one pins both. */
    AiusOut16Stage d = aius_out16_stage_default();
    for (int i = 0; i < N; i++) {
        int32_t sl = aius_out16_sample(&d.stream[0], d.dither, x24[i]);
        int32_t sr = aius_out16_sample(&d.stream[1], d.dither, x24[i]);
        assert(sl == a_l[i]);
        assert(sr == a_r[i]);
    }

    /* Undithered conversion is trivially deterministic AND stream-independent. */
    AiusOut16Stage e;
    aius_out16_init(&e, 12345u, 0);
    static int16_t e_l[N], e_r[N];
    aius_out16_run(&e, x24, x24, e_l, e_r, N);
    assert(memcmp(e_l, e_r, sizeof e_l) == 0);
    printf("ok out16 dither is deterministic (reset reproduces, L/R independent)\n");
}

/* --- the 24-bit output path, and the negative control for it -------------- */

/* Distinct values in `v`. O(n^2), and n is 256 here: the point is to say
 * "how much of the signal survived" in a form an assertion can read. */
static int distinct_count(const int32_t *v, int n)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (v[j] == v[i]) { seen = 1; break; }
        if (!seen) count++;
    }
    return count;
}

/* THE NEGATIVE CONTROL FOR THE 24-BIT PATH.
 *
 * A test that asserts the ring is `int32_t` proves nothing -- the old path was
 * int32 at the transmit register too, which is exactly what made the defect
 * invisible. So this drives ONE signal through BOTH paths and shows the old one
 * destroys it while the new one carries it bit for bit.
 *
 * The signal is chosen so that ALL of its information lives in the low 8 bits:
 * every sample is strictly inside one 16-bit LSB (256 in 24-bit units), i.e.
 * below -90 dBFS. That is not a contrived corner -- it is the tail of every fade
 * and every reverb, which is where a decoder is judged.
 *
 * The old path is modelled exactly, not approximated: aius_out16_run() into
 * int16 planes, then `(*src++) << 8` into the 24-bit transmit word, which is
 * output_spdif3.cpp:130 verbatim. */
static void test_tx24_carries_low_bits_the_old_path_discarded(void)
{
    enum { FRAMES = 128 }; /* one engine block: exactly what one transmit interrupt fills */
    static int32_t l24[FRAMES], r24[FRAMES];
    for (int i = 0; i < FRAMES; i++) {
        l24[i] = i + 1;      /*    1 ..  128, all inside one 16-bit LSB */
        r24[i] = -(i + 1);   /*   -1 .. -128 */
    }

    /* --- the old path ---------------------------------------------------- */
    static int16_t o16_l[FRAMES], o16_r[FRAMES];
    static int32_t old_tx[FRAMES * 2];
    AiusOut16Stage st16 = aius_out16_stage_default();
    aius_out16_run(&st16, l24, r24, o16_l, o16_r, FRAMES);
    for (int i = 0; i < FRAMES; i++) {
        /* The snapshot's `<< 8` verbatim, but reached through uint32_t: shifting
         * a negative signed value is undefined, and UBSan says so on every run.
         * The two's-complement result is identical, which is the point -- this
         * models the old wire word exactly. */
        old_tx[2 * i] = (int32_t)((uint32_t)(int32_t)o16_l[i] << 8);
        old_tx[2 * i + 1] = (int32_t)((uint32_t)(int32_t)o16_r[i] << 8);
    }
    /* The defect as an assertion: every word the old path put on the wire is a
     * multiple of 256, so the low 8 bits are ALWAYS zero and this signal is
     * unrepresentable no matter how good the DSP above it was. */
    for (int i = 0; i < FRAMES * 2; i++)
        assert((old_tx[i] & 0xFF) == 0);
    /* And it is loss of SIGNAL, not just of precision: 128 distinct input values
     * collapse into the handful of codes the dither can reach around zero. */
    const int distinct_old = distinct_count(old_tx, FRAMES * 2);
    assert(distinct_old <= 5);

    /* --- the new path: DSP word -> stage at 24 bits -> ring -> transmit --- */
    static AiusSpdifTx24Ring ring;
    memset(&ring, 0, sizeof ring);

    int32_t *stage_l = NULL;
    int32_t *stage_r = NULL;
    uint32_t span = aius_spdif_tx24_stage(&ring, 0, &stage_l, &stage_r);
    assert(span >= (uint32_t)FRAMES);
    /* st == NULL is deliberate and is itself an assertion: the 24-bit arm cannot
     * touch a dither stream, because it is handed none. */
    aius_out_stage_run(NULL, AIUS_DSP_OUT_WIDTH_24, l24, r24,
                       stage_l, stage_r, FRAMES);
    /* Nothing is transmittable until the commit -- the property that lets the
     * higher-priority transmit ISR preempt a mid-block producer safely. */
    assert(aius_spdif_tx24_level(&ring) == 0);
    aius_spdif_tx24_commit(&ring, FRAMES);
    assert(aius_spdif_tx24_level(&ring) == FRAMES);
    assert(aius_spdif_tx24_free(&ring) ==
           AIUS_SPDIF_TX24_RING_FRAMES - (uint32_t)FRAMES);

    /* `tx` is laid out exactly as one half of SPDIF_tx_buffer: 2 * FRAMES int32,
     * left at even indices, right at odd, which is the interleave the transmit
     * DMA moves to SPDIF_STL / SPDIF_STR. */
    static int32_t tx[FRAMES * 2];
    aius_spdif_tx24_fill(&ring, tx, FRAMES);
    assert(aius_spdif_tx24_level(&ring) == 0);
    assert(ring.underruns == 0);

    /* THE ASSERTION THE WHOLE ITEM EXISTS FOR: bit for bit, low 8 bits included. */
    for (int i = 0; i < FRAMES; i++) {
        assert(tx[2 * i] == l24[i]);
        assert(tx[2 * i + 1] == r24[i]);
    }
    /* Stated the other way round, so a future memcpy-shaped regression that
     * happened to preserve the first sample cannot pass: every distinct input
     * value is still distinguishable on the wire. */
    assert(distinct_count(tx, FRAMES * 2) == 2 * FRAMES);
    /* If the two paths ever agreed on this signal, this control would be
     * vacuous. */
    assert(memcmp(tx, old_tx, sizeof tx) != 0);

    printf("ok 24-bit transmit path is lossless (%d of %d codes survive; the "
           "16-bit path left %d)\n",
           distinct_count(tx, FRAMES * 2), 2 * FRAMES, distinct_old);
}

/* The bypassed stage is still THERE and still right. aius_out16_* was not
 * deleted when the path stopped narrowing the word -- it is the other arm of
 * aius_out_stage_run(), and this pins it to the block form the dsp suite
 * already tests, times the exact power of two the S/PDIF snapshot's ISR used to
 * apply. So "bypassed" is a selection, not rot. */
static void test_out_stage_16bit_arm_matches_the_old_wire_words(void)
{
    enum { N = 512 };
    static int32_t x24[N], y24[N];
    for (int i = 0; i < N; i++) {
        x24[i] = (int32_t)((i * 7919) % 1000003) - 500000;
        y24[i] = -x24[i];
    }

    static int16_t ref_l[N], ref_r[N];
    AiusOut16Stage ref = aius_out16_stage_default();
    aius_out16_run(&ref, x24, y24, ref_l, ref_r, N);

    static int32_t got_l[N], got_r[N];
    AiusOut16Stage sel = aius_out16_stage_default();
    aius_out_stage_run(&sel, AIUS_DSP_OUT_WIDTH_16, x24, y24, got_l, got_r, N);

    for (int i = 0; i < N; i++) {
        assert(got_l[i] == (int32_t)ref_l[i] * 256);
        assert(got_r[i] == (int32_t)ref_r[i] * 256);
        assert((got_l[i] & 0xFF) == 0); /* a 16-bit selection zeroes the low byte */
    }
    /* Both consumed the same number of draws from each stream, so the stage's
     * determinism contract survives the width parameter. */
    assert(memcmp(&ref, &sel, sizeof ref) == 0);

    /* The 24-bit arm, on the same input, touches neither stream and changes
     * nothing -- the definition of a bypass. */
    AiusOut16Stage untouched = aius_out16_stage_default();
    const AiusOut16Stage before = untouched;
    aius_out_stage_run(&untouched, AIUS_DSP_OUT_WIDTH_24, x24, y24,
                       got_l, got_r, N);
    assert(memcmp(&before, &untouched, sizeof before) == 0);
    for (int i = 0; i < N; i++) {
        assert(got_l[i] == x24[i]);
        assert(got_r[i] == y24[i]);
    }

    /* An unrecognised width writes nothing rather than guessing a rule. */
    for (int i = 0; i < N; i++) got_l[i] = 0x5A5A5A;
    aius_out_stage_run(&sel, 20, x24, y24, got_l, got_r, N);
    for (int i = 0; i < N; i++) assert(got_l[i] == 0x5A5A5A);

    printf("ok out16 stays reachable as the 16-bit arm (and 24 bits bypasses it)\n");
}

/* The ring's two structural cases, both of which the old int16 ring also had and
 * neither of which anything asserted: a block that WRAPS, and a transmit
 * interrupt that arrives with the ring short. */
/* The ring must be empty after a reset even though nothing zeroed its memory.
 *
 * Every other ring test in this file opens with `memset(&ring, 0, sizeof ring)`,
 * so not one of them could detect this: they all assert behaviour from the state
 * the target does NOT come up in. `.bss.dma` is NOLOAD, and the i.MX RT1062 boot
 * ROM writes scratch into three fixed OCRAM runs, one of which contains this
 * structure's cursors. So the test seeds the cursors with the ROM bytes actually
 * observed at that address rather than a chosen pattern -- the point is the
 * device's real reset state, not a hypothetical one. */
static void test_tx24_reset_empties_a_ring_that_was_never_zeroed(void)
{
    static AiusSpdifTx24Ring ring;

    /* Sample data as well, so "reads as empty" cannot be satisfied by the buffer
     * happening to hold zeros -- silence below must come from the cursors. */
    for (uint32_t i = 0; i < AIUS_SPDIF_TX24_RING_FRAMES; i++) {
        ring.left[i] = 0x7FFF00;
        ring.right[i] = -0x7FFF00;
    }
    /* The words measured at this address after a reset on the board. */
    ring.written = 0x00000001u;
    ring.consumed = 0x00001000u;
    ring.underruns = 0x00060008u;

    /* Pre-condition, and the defect in one line: unsigned wrap makes the ring
     * look nearly full, so fill() would transmit buffer contents. */
    assert(aius_spdif_tx24_level(&ring) != 0u);

    aius_spdif_tx24_reset(&ring);

    assert(aius_spdif_tx24_level(&ring) == 0u);
    assert(aius_spdif_tx24_free(&ring) == AIUS_SPDIF_TX24_RING_FRAMES);
    assert(ring.underruns == 0u);

    /* THE AUDIBLE ASSERTION. A fill straight after reset must emit digital
     * silence and count the substitution, not leak the samples still sitting in
     * the buffer -- that burst is what the defect actually produced at every
     * boot, bounded and inaudible to the test suite but not to a listener. */
    enum { N = 64 };
    static int32_t tx[N * 2];
    for (int i = 0; i < N * 2; i++) tx[i] = 0x123456; /* must be overwritten */
    aius_spdif_tx24_fill(&ring, tx, N);
    for (int i = 0; i < N * 2; i++) {
        assert(tx[i] == 0);
    }
    assert(ring.underruns == (uint32_t)N);
    printf("ok tx24 reset empties a ring the boot ROM left dirty (silence, %u underruns)\n",
           ring.underruns);
}

static void test_tx24_ring_wraps_and_counts_underruns(void)
{
    static AiusSpdifTx24Ring ring;
    memset(&ring, 0, sizeof ring);

    /* Park the cursors 100 frames from the end of the buffer, empty. Setting the
     * counters directly is the point: they are free-running frame counts, so any
     * pair with a zero difference is a legal empty ring. */
    ring.written = AIUS_SPDIF_TX24_RING_FRAMES - 100u;
    ring.consumed = ring.written;

    enum { N = 300 }; /* wraps: 100 before the end, 200 after */
    static int32_t l24[N], r24[N];
    for (int i = 0; i < N; i++) {
        l24[i] = 0x400000 + i; /* low bits distinct across the whole block */
        r24[i] = -0x400000 - i;
    }

    /* Exactly the two-call shape AiusAudioProcessor::enqueue() uses. */
    int32_t *sl = NULL;
    int32_t *sr = NULL;
    uint32_t first = aius_spdif_tx24_stage(&ring, 0, &sl, &sr);
    if (first > (uint32_t)N) first = (uint32_t)N;
    assert(first == 100u); /* the wrap is really being exercised */
    aius_out_stage_run(NULL, AIUS_DSP_OUT_WIDTH_24, l24, r24, sl, sr, first);
    (void)aius_spdif_tx24_stage(&ring, first, &sl, &sr);
    aius_out_stage_run(NULL, AIUS_DSP_OUT_WIDTH_24, l24 + first, r24 + first,
                       sl, sr, (uint32_t)N - first);
    aius_spdif_tx24_commit(&ring, N);
    assert(aius_spdif_tx24_level(&ring) == N);

    static int32_t tx[N * 2];
    aius_spdif_tx24_fill(&ring, tx, N);
    for (int i = 0; i < N; i++) {
        assert(tx[2 * i] == l24[i]);
        assert(tx[2 * i + 1] == r24[i]);
    }
    assert(aius_spdif_tx24_level(&ring) == 0);
    assert(ring.underruns == 0);

    /* Underrun: 50 frames committed, a 128-frame interrupt. The first 50 are the
     * audio, the remaining 78 are digital silence, and -- unlike the vendor
     * path's block_silent substitution -- they are COUNTED. */
    memset(&ring, 0, sizeof ring);
    first = aius_spdif_tx24_stage(&ring, 0, &sl, &sr);
    assert(first >= 50u);
    aius_out_stage_run(NULL, AIUS_DSP_OUT_WIDTH_24, l24, r24, sl, sr, 50u);
    aius_spdif_tx24_commit(&ring, 50u);

    static int32_t short_tx[128 * 2];
    for (int i = 0; i < 128 * 2; i++) short_tx[i] = 0x7FFFFF;
    aius_spdif_tx24_fill(&ring, short_tx, 128u);
    for (int i = 0; i < 50; i++) {
        assert(short_tx[2 * i] == l24[i]);
        assert(short_tx[2 * i + 1] == r24[i]);
    }
    for (int i = 50; i < 128; i++) {
        assert(short_tx[2 * i] == 0);
        assert(short_tx[2 * i + 1] == 0);
    }
    assert(ring.underruns == 78u);
    assert(aius_spdif_tx24_level(&ring) == 0);
    /* An empty ring underruns by the whole interrupt and never goes negative --
     * the free-running-counter difference is what makes that safe. */
    aius_spdif_tx24_fill(&ring, short_tx, 128u);
    assert(ring.underruns == 78u + 128u);
    assert(aius_spdif_tx24_level(&ring) == 0);

    printf("ok 24-bit ring wraps mid-block and counts substituted silence\n");
}

/* --- coherent meter snapshot ---------------------------------------------- */

/* Per-input peak/RMS on a 6-channel block where each channel carries a distinct
 * constant: peak == RMS == |constant| for every channel, in AIUS_CH_* order. */
static void test_meter_input_peak_rms(void)
{
    enum { N = 8 };
    int32_t fl[N], fr[N], fc[N], lfe[N], sl[N], sr[N];
    const int32_t val[6] = {1000, -2000, 3000, -4000, 5000, -6000};
    int32_t *pl[6] = {fl, fr, fc, lfe, sl, sr};
    for (int c = 0; c < 6; c++)
        for (int i = 0; i < N; i++)
            pl[c][i] = val[c];
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE |
                    AIUS_CH_SL | AIUS_CH_SR;
    AiusPcmBlock in = {{fl, fr, fc, lfe, sl, sr}, 6, N, 48000, mask};

    AiusMeter m;
    aius_meter_init(&m, 48000);
    aius_meter_accumulate_input(&m, &in);
    aius_meter_publish(&m);

    AiusMeterSnapshot s;
    assert(aius_meter_read(&m, &s) == 1);
    assert(s.sample_rate_hz == 48000);
    for (int c = 0; c < 6; c++) {
        double a = val[c] < 0 ? -(double)val[c] : (double)val[c];
        assert(s.input_peak[c] == a); /* constant DC: peak == |val| */
        assert(s.input_rms[c] == a);  /* RMS of a constant == |val| */
    }
    printf("ok meter input peak/RMS per channel\n");
}

/* Silence -> zero; full-scale DC -> peak/RMS at full scale; stereo output taps. */
static void test_meter_output_and_silence(void)
{
    enum { N = 4 };
    /* Silence. */
    {
        int32_t z[N] = {0, 0, 0, 0};
        AiusMeter m;
        aius_meter_init(&m, 48000);
        aius_meter_accumulate_output(&m, z, z, N);
        aius_meter_publish(&m);
        AiusMeterSnapshot s;
        assert(aius_meter_read(&m, &s) == 1);
        assert(s.output_peak[0] == 0.0 && s.output_peak[1] == 0.0);
        assert(s.output_rms[0] == 0.0 && s.output_rms[1] == 0.0);
    }
    /* Full-scale DC on L, negative full-scale on R. */
    {
        int32_t l[N], r[N];
        for (int i = 0; i < N; i++) {
            l[i] = AIUS_DSP_SAMPLE_MAX;
            r[i] = AIUS_DSP_SAMPLE_MIN;
        }
        AiusMeter m;
        aius_meter_init(&m, 48000);
        aius_meter_accumulate_output(&m, l, r, N);
        aius_meter_publish(&m);
        AiusMeterSnapshot s;
        assert(aius_meter_read(&m, &s) == 1);
        assert(s.output_peak[0] == (double)AIUS_DSP_SAMPLE_MAX);
        assert(s.output_peak[1] == -(double)AIUS_DSP_SAMPLE_MIN); /* |MIN| */
        assert(s.output_rms[0] == (double)AIUS_DSP_SAMPLE_MAX);
        assert(s.output_rms[1] == -(double)AIUS_DSP_SAMPLE_MIN);
    }
    printf("ok meter output peak/RMS + silence\n");
}

/* A tone (and a ramp) RMS matches an independent same-order reference exactly. */
static void test_meter_tone_rms(void)
{
    enum { N = 64 };
    int32_t l[N], r[N];
    for (int i = 0; i < N; i++) {
        l[i] = (int32_t)(sin(i * 0.19) * 5000000.0);
        r[i] = (int32_t)(cos(i * 0.11) * 3000000.0);
    }
    /* Independent reference: same summation order as accumulate_output. */
    double sql = 0.0, sqr = 0.0, pkl = 0.0, pkr = 0.0;
    for (int i = 0; i < N; i++) {
        double vl = (double)l[i], vr = (double)r[i];
        double al = vl < 0 ? -vl : vl, ar = vr < 0 ? -vr : vr;
        if (al > pkl) pkl = al;
        if (ar > pkr) pkr = ar;
        sql += vl * vl;
        sqr += vr * vr;
    }
    double rl = sqrt(sql / (double)N), rr = sqrt(sqr / (double)N);

    AiusMeter m;
    aius_meter_init(&m, 48000);
    aius_meter_accumulate_output(&m, l, r, N);
    aius_meter_publish(&m);
    AiusMeterSnapshot s;
    assert(aius_meter_read(&m, &s) == 1);
    assert(s.output_peak[0] == pkl && s.output_peak[1] == pkr);
    assert(s.output_rms[0] == rl && s.output_rms[1] == rr);
    printf("ok meter tone RMS matches reference (%.1f, %.1f)\n", rl, rr);
}

/* Waveform decimation: exact point count, deterministic bucketing, ramp, bounds,
 * degenerate inputs, and the max(|l|,|r|) envelope. */
static void test_meter_decimate(void)
{
    /* Ramp of 256 into 128 buckets: bucket b = [2b, 2b+2), peak = 2b+1. */
    {
        enum { N = 256, CAP = 128 };
        static int32_t ramp[N];
        for (int i = 0; i < N; i++) ramp[i] = i;
        int32_t out[CAP];
        uint32_t pts = aius_meter_decimate(ramp, NULL, N, out, CAP);
        assert(pts == CAP);
        for (uint32_t b = 0; b < CAP; b++)
            assert(out[b] == (int32_t)(2 * b + 1));
    }
    /* frames < cap: one sample per bucket, envelope == |sample|. */
    {
        enum { CAP = 128 };
        int32_t l[10];
        for (int i = 0; i < 10; i++) l[i] = -(i + 1) * 100;
        int32_t out[CAP];
        uint32_t pts = aius_meter_decimate(l, NULL, 10, out, CAP);
        assert(pts == 10);
        for (int i = 0; i < 10; i++) assert(out[i] == (i + 1) * 100);
    }
    /* Uneven division 130 -> 128: bounded, buckets contiguous + near-equal, each
     * peak equals an independent per-bucket max scan. */
    {
        enum { N = 130, CAP = 128 };
        static int32_t l[N];
        for (int i = 0; i < N; i++) l[i] = (i * 7) % 5000 - 2500;
        int32_t out[CAP];
        uint32_t pts = aius_meter_decimate(l, NULL, N, out, CAP);
        assert(pts == CAP);
        assert(pts <= CAP && pts <= N);
        uint32_t prev_end = 0;
        for (uint32_t b = 0; b < pts; b++) {
            uint32_t start = (uint32_t)((uint64_t)b * N / pts);
            uint32_t end = (uint32_t)((uint64_t)(b + 1) * N / pts);
            assert(start == prev_end);           /* contiguous, no gaps/overlap */
            assert(end - start == 1 || end - start == 2); /* near-equal */
            int32_t pk = 0;
            for (uint32_t i = start; i < end; i++) {
                int32_t a = l[i] < 0 ? -l[i] : l[i];
                if (a > pk) pk = a;
            }
            assert(out[b] == pk);
            prev_end = end;
        }
        assert(prev_end == N); /* full coverage */
    }
    /* max(|l|,|r|) envelope: r dominates in a bucket. */
    {
        int32_t l[4] = {10, 20, 30, 40};
        int32_t r[4] = {-100, 5, -7, 9};
        int32_t out[4];
        uint32_t pts = aius_meter_decimate(l, r, 4, out, 4);
        assert(pts == 4);
        assert(out[0] == 100 && out[1] == 20 && out[2] == 30 && out[3] == 40);
    }
    /* Degenerate: NULL out, zero cap, zero frames, NULL l all yield 0 points. */
    {
        int32_t l[1] = {1}, out[1];
        assert(aius_meter_decimate(l, NULL, 1, NULL, 1) == 0);
        assert(aius_meter_decimate(l, NULL, 1, out, 0) == 0);
        assert(aius_meter_decimate(l, NULL, 0, out, 1) == 0);
        assert(aius_meter_decimate(NULL, NULL, 1, out, 1) == 0);
    }
    printf("ok meter waveform decimation (ramp, bounds, envelope)\n");
}

/* Build a deterministic published snapshot from a DC output block. */
static void meter_publish_dc(AiusMeter *m, int32_t v, uint32_t frames)
{
    enum { N = 32 };
    int32_t l[N], r[N];
    if (frames > N) frames = N;
    for (uint32_t i = 0; i < frames; i++) {
        l[i] = v;
        r[i] = v;
    }
    aius_meter_accumulate_output(m, l, r, frames);
    aius_meter_publish(m);
}

/* Coherence: monotone sequence, mid-update rejection, and an interleaved write
 * (mirroring publish's seqlock protocol) never surfaces a partial snapshot. */
static void test_meter_coherence(void)
{
    AiusMeter m;
    aius_meter_init(&m, 48000);

    /* Sequence advances monotonically across publishes; each read is coherent. */
    uint32_t prev = 0;
    for (int k = 1; k <= 5; k++) {
        meter_publish_dc(&m, k * 1000, 16);
        assert((m.seq & 1u) == 0); /* stable after publish */
        AiusMeterSnapshot s;
        assert(aius_meter_read(&m, &s) == 1);
        assert(s.sequence == prev + 1);
        assert(s.output_peak[0] == (double)(k * 1000));
        prev = s.sequence;
    }

    /* A read that straddles a write-in-progress (odd seq) is rejected, never
     * returned torn. Scribble an inconsistent payload to prove it is not read. */
    AiusMeterSnapshot good;
    assert(aius_meter_read(&m, &good) == 1);
    uint32_t base = m.seq;
    m.seq = base + 1;                       /* odd: publish "in progress" */
    m.published.output_peak[0] = -12345.0;  /* inconsistent scribble */
    AiusMeterSnapshot torn;
    memset(&torn, 0x5a, sizeof torn);
    assert(aius_meter_read(&m, &torn) == 0); /* rejected across all retries */
    /* Complete the write with a consistent payload; the reader now accepts it. */
    m.published.output_peak[0] = 777.0;
    m.seq = base + 2;                        /* even: complete */
    AiusMeterSnapshot after;
    assert(aius_meter_read(&m, &after) == 1);
    assert(after.output_peak[0] == 777.0);

    /* Interleaved: at every step of a hand-run publish protocol, a reader either
     * fails (mid-write) or gets a complete snapshot -- never a partial mix. */
    aius_meter_init(&m, 48000);
    meter_publish_dc(&m, 4000, 16); /* snapshot A */
    AiusMeterSnapshot probe;
    assert(aius_meter_read(&m, &probe) == 1 && probe.output_peak[0] == 4000.0);
    uint32_t s0 = m.seq;
    m.seq = s0 + 1;                          /* begin B: odd */
    assert(aius_meter_read(&m, &probe) == 0);
    m.published.sequence = probe.sequence + 100; /* partial field writes */
    assert(aius_meter_read(&m, &probe) == 0);
    m.published.output_peak[0] = 9000.0;
    m.published.output_peak[1] = 9000.0;
    assert(aius_meter_read(&m, &probe) == 0); /* still mid-write */
    m.seq = s0 + 2;                          /* commit B: even */
    assert(aius_meter_read(&m, &probe) == 1);
    assert(probe.output_peak[0] == 9000.0 && probe.output_peak[1] == 9000.0);
    printf("ok meter coherence (monotone seq, mid-write rejected, no tearing)\n");
}

/* Every published field is finite and within range on adversarial content. */
static void test_meter_bounds_finite(void)
{
    enum { N = 512 };
    static int32_t l[N], r[N];
    int32_t fl[N], fr[N], fc[N], lfe[N], sl[N], sr[N];
    int32_t *pl[6] = {fl, fr, fc, lfe, sl, sr};
    uint32_t seed = 0xC0FFEEu;
    for (int i = 0; i < N; i++) {
        for (int c = 0; c < 6; c++) {
            seed = seed * 1103515245u + 12345u;
            int32_t v = (int32_t)((seed >> 8) % (AIUS_DSP_SAMPLE_MAX + 1u));
            pl[c][i] = (i & 1) ? -v : v;
        }
        l[i] = pl[0][i];
        r[i] = pl[1][i];
    }
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE |
                    AIUS_CH_SL | AIUS_CH_SR;
    AiusPcmBlock in = {{fl, fr, fc, lfe, sl, sr}, 6, N, 48000, mask};

    AiusMeter m;
    aius_meter_init(&m, 48000);
    aius_meter_accumulate_input(&m, &in);
    aius_meter_accumulate_output(&m, l, r, N);
    aius_meter_set_limiter_gr(&m, -6.5);
    aius_meter_set_telemetry(&m, 128, 3200, 42000, 51000);
    aius_meter_publish(&m);

    AiusMeterSnapshot s;
    assert(aius_meter_read(&m, &s) == 1);
    for (int c = 0; c < AIUS_METER_INPUT_CHANNELS; c++) {
        assert(isfinite(s.input_peak[c]) && isfinite(s.input_rms[c]));
        assert(s.input_peak[c] >= 0.0 && s.input_peak[c] <= (double)AIUS_DSP_SAMPLE_MAX + 1.0);
        assert(s.input_rms[c] >= 0.0 && s.input_rms[c] <= s.input_peak[c] + 1e-6);
    }
    for (int k = 0; k < 2; k++) {
        assert(isfinite(s.output_peak[k]) && isfinite(s.output_rms[k]));
        assert(s.output_rms[k] <= s.output_peak[k] + 1e-6); /* RMS <= peak */
    }
    assert(isfinite(s.limiter_gain_reduction_db) && s.limiter_gain_reduction_db <= 0.0);
    assert(s.waveform_count <= AIUS_METER_WAVEFORM_MAX);
    for (uint32_t i = 0; i < s.waveform_count; i++)
        assert(s.waveform[i] >= 0 && s.waveform[i] <= AIUS_DSP_SAMPLE_MAX);
    /* Target-adapter telemetry carried through verbatim. */
    assert(s.queue_frames == 128 && s.estimated_latency_us == 3200);
    assert(s.dsp_cycles_last == 42000 && s.dsp_cycles_max == 51000);
    printf("ok meter fields finite + in range\n");
}

/* Metering does not alter audio: process with a meter attached is bit-identical
 * to the same config with no meter, and the meter captured the true output peak. */
static void test_meter_process_no_mutation(void)
{
    int32_t fl[4] = {6000000, -6000000, 100, -200};
    int32_t fr[4] = {5000000, -5000000, 300, -400};
    int32_t fc[4] = {4000000, -4000000, 500, -600};
    uint32_t mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC;
    AiusPcmBlock in = {{fl, fr, fc, 0, 0, 0}, 3, 4, 48000, mask};

    AiusBiquad eq[1];
    size_t ns = aius_eq_default(eq, 1);
    AiusLimiterConfig limc = aius_limiter_config_default();
    limc.enabled = 1;
    limc.threshold_dbfs = -3.0;
    limc.attack_ms = 1.0;
    limc.release_ms = 50.0;
    limc.lookahead_ms = 0.0;

    /* Reference: no meter. */
    int32_t s0[4], s1[4], s2[4];
    AiusInputStage stage = aius_input_stage_default();
    stage.channels[2].gain_linear = aius_db_to_linear(-3.0);
    stage.scratch[0] = s0;
    stage.scratch[1] = s1;
    stage.scratch[2] = s2;
    AiusBiquadState est[2];
    memset(est, 0, sizeof est);
    AiusLimiter lim0;
    memset(&lim0, 0, sizeof lim0);
    lim0.cfg = limc;
    int32_t rl[4], rr[4];
    AiusPcmBlock rout;
    memset(&rout, 0, sizeof rout);
    rout.planes[0] = rl;
    rout.planes[1] = rr;
    AiusDspConfig rc = {aius_downmix_config_default(), eq, ns, est, &stage, &lim0};
    assert(aius_dsp_process(&rc, &in, &rout) == AIUS_OK);

    /* Metered: identical (meter-free) config; the meter is driven caller-side. */
    int32_t m0[4], m1[4], m2[4];
    AiusInputStage mstage = aius_input_stage_default();
    mstage.channels[2].gain_linear = aius_db_to_linear(-3.0);
    mstage.scratch[0] = m0;
    mstage.scratch[1] = m1;
    mstage.scratch[2] = m2;
    AiusBiquadState mest[2];
    memset(mest, 0, sizeof mest);
    AiusLimiter lim1;
    memset(&lim1, 0, sizeof lim1);
    lim1.cfg = limc;
    AiusMeter meter;
    aius_meter_init(&meter, 48000);
    int32_t ml[4], mr[4];
    AiusPcmBlock mout;
    memset(&mout, 0, sizeof mout);
    mout.planes[0] = ml;
    mout.planes[1] = mr;
    AiusDspConfig mc = {aius_downmix_config_default(), eq, ns, mest, &mstage,
                        &lim1};
    assert(aius_dsp_process(&mc, &in, &mout) == AIUS_OK);

    /* Caller-driven metering: run the taps around process, not inside it. */
    aius_meter_accumulate_input(&meter, &in);
    aius_meter_accumulate_output(&meter, mout.planes[0], mout.planes[1],
                                 mout.frames);
    aius_meter_set_limiter_gr(&meter, aius_dsp_limiter_gain_reduction_db(&lim1));

    for (int i = 0; i < 4; i++) {
        assert(ml[i] == rl[i]); /* metering did not touch the audio */
        assert(mr[i] == rr[i]);
    }
    /* Decoded input planes remain const. */
    assert(fl[0] == 6000000 && fc[3] == -600);

    aius_meter_publish(&meter);
    AiusMeterSnapshot s;
    assert(aius_meter_read(&meter, &s) == 1);
    int32_t opk_l = peak_abs_i(rl, 4), opk_r = peak_abs_i(rr, 4);
    assert(s.output_peak[0] == (double)opk_l);
    assert(s.output_peak[1] == (double)opk_r);
    /* Limiter GR captured (<= 0). */
    assert(s.limiter_gain_reduction_db <= 0.0);
    printf("ok meter taps do not mutate audio (bit-identical) + capture output\n");
}

static void test_arena_alloc(void)
{
    static uint8_t backing[4096];
    AiusArena a;
    assert(aius_arena_init(&a, backing, sizeof backing) == 0);
    AiusPcmBlock out;
    assert(aius_dsp_alloc_stereo(&a, 256, 48000, &out) == AIUS_OK);
    assert(out.planes[0] != NULL && out.planes[1] != NULL);
    assert(out.planes[0] != out.planes[1]);
    assert(out.channels == 2 && out.frames == 256);
    assert(out.channel_mask == (AIUS_CH_FL | AIUS_CH_FR));

    /* Exhaustion returns AIUS_NO_SPACE. */
    AiusArena tiny;
    static uint8_t small[8];
    assert(aius_arena_init(&tiny, small, sizeof small) == 0);
    AiusPcmBlock out2;
    assert(aius_dsp_alloc_stereo(&tiny, 256, 48000, &out2) == AIUS_NO_SPACE);
    printf("ok arena stereo alloc + exhaustion\n");
}

static void test_invalid_args(void)
{
    AiusDownmixConfig cfg = aius_downmix_config_default();
    int32_t l[1], r[1], p[1] = {0};
    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = l;
    out.planes[1] = r;
    AiusPcmBlock in = {{p, 0, 0, 0, 0, 0}, 1, 1, 48000, AIUS_CH_FC};

    assert(aius_dsp_downmix(NULL, &in, &out) == AIUS_INVALID_ARG);
    assert(aius_dsp_downmix(&cfg, NULL, &out) == AIUS_INVALID_ARG);
    assert(aius_dsp_downmix(&cfg, &in, NULL) == AIUS_INVALID_ARG);

    /* channels != popcount(mask). */
    AiusPcmBlock bad = {{p, 0, 0, 0, 0, 0}, 2, 1, 48000, AIUS_CH_FC};
    assert(aius_dsp_downmix(&cfg, &bad, &out) == AIUS_INVALID_ARG);

    /* out plane NULL. */
    AiusPcmBlock noout;
    memset(&noout, 0, sizeof noout);
    assert(aius_dsp_downmix(&cfg, &in, &noout) == AIUS_INVALID_ARG);

    /* process: eq_sections>0 but null coeffs. */
    AiusDspConfig pc = {cfg, NULL, 1, NULL, NULL, NULL};
    assert(aius_dsp_process(&pc, &in, &out) == AIUS_INVALID_ARG);
    printf("ok invalid-arg guards\n");
}

int main(void)
{
    test_stereo_passthrough();
    test_mono_to_both();
    test_fc_equal_lr();
    test_surround_routing();
    test_clip_headroom();
    test_clipfree_headroom();
    test_lfe_fold_design();
    test_lfe_fold_rate_independent();
    test_lfe_fold_init_rejects();
    test_lfe_fold_zero_gain_bit_exact();
    test_lfe_fold_attenuates_above_corner();
    test_lfe_fold_unfiltered_is_flat_multiply();
    test_lfe_fold_missing_state_fails_closed();
    test_lfe_fold_gain_applied_after_filter();
    test_lfe_fold_block_boundary_seamless();
    test_lfe_fold_redesign_on_change();
    test_lfe_fold_reset();
    test_biquad_gain_bypass();
    test_biquad_feedback_recurrence();
    test_biquad_gain_scale();
    test_biquad_coeff_math();
    test_biquad_dc_settle();
    test_process_bypass_equals_downmix();
    test_db_to_linear();
    test_input_trim_gain();
    test_input_trim_folds_into_eq();
    test_input_two_section_eq();
    test_process_input_identity_equals_downmix();
    test_process_input_trim_scales();
    test_shipped_defaults();
    test_limiter_disabled_is_bypass();
    test_limiter_transparent_below_threshold();
    test_limiter_threshold_steady_state();
    test_limiter_lookahead_delay();
    test_limiter_attack_release_envelope();
    test_limiter_prevents_saturation();
    test_limiter_bounded_finite();
    test_limiter_block_boundary();
    test_process_full_path();
    test_truepeak_catches_intersample_peak();
    test_truepeak_gate_bound_holds();
    test_out16_rounds_not_truncates();
    test_out16_dither_carries_sub_lsb();
    test_out16_dither_deterministic();
    test_tx24_carries_low_bits_the_old_path_discarded();
    test_out_stage_16bit_arm_matches_the_old_wire_words();
    test_tx24_reset_empties_a_ring_that_was_never_zeroed();
    test_tx24_ring_wraps_and_counts_underruns();
    test_meter_input_peak_rms();
    test_meter_output_and_silence();
    test_meter_tone_rms();
    test_meter_decimate();
    test_meter_coherence();
    test_meter_bounds_finite();
    test_meter_process_no_mutation();
    test_arena_alloc();
    test_invalid_args();
    printf("ALL DSP TESTS PASSED\n");
    return 0;
}
