/* Known-answer tests for the Aius output DSP (downmix + biquad EQ).
 *
 * Built and run by scripts/test.sh --suite dsp. Deterministic double-precision
 * arithmetic: every expectation is computed analytically. Runs under
 * ASan/UBSan via the shared native cflags.
 */

#include "aius_dsp.h"

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

    AiusDownmixConfig cfg = aius_downmix_config_default();
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
    cfg.lfe = AIUS_DSP_LFE_GAIN_MINUS10DB; /* worst case, LFE folded in */
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

    AiusDownmixConfig cfg = aius_downmix_config_default(); /* lfe = 0 */
    cfg.headroom = aius_downmix_clipfree_headroom(&cfg);
    assert(cfg.headroom > 0.0 && cfg.headroom < 1.0);
    assert(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK);
    /* L = FS*(1 + center + surround)*headroom = FS, within rounding, never over. */
    assert(ol[0] <= AIUS_DSP_SAMPLE_MAX);
    assert(ol[0] >= AIUS_DSP_SAMPLE_MAX - 1);
    printf("ok clip-free headroom maps full scale to full scale\n");
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
    AiusDspConfig cfg = {aius_downmix_config_default(), eq, nsec, state};
    assert(aius_dsp_process(&cfg, &in, &out) == AIUS_OK);
    for (int i = 0; i < 2; i++) {
        assert(ol[i] == dl[i]);
        assert(or_[i] == dr[i]);
    }
    printf("ok process(bypass EQ) == downmix\n");
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
    AiusDspConfig pc = {cfg, NULL, 1, NULL};
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
    test_biquad_gain_bypass();
    test_biquad_feedback_recurrence();
    test_biquad_gain_scale();
    test_biquad_coeff_math();
    test_biquad_dc_settle();
    test_process_bypass_equals_downmix();
    test_arena_alloc();
    test_invalid_args();
    printf("ALL DSP TESTS PASSED\n");
    return 0;
}
