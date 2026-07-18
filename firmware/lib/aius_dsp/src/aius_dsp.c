/* Aius output DSP: 5.1-to-stereo downmix + Direct Form I biquad EQ.
 *
 * See lib/aius_dsp/include/aius_dsp.h for the contract, the standards followed
 * (ITU-R BS.775 / ATSC A/52 downmix, RBJ cookbook biquads), and the
 * double-precision determinism rationale. This file writes no fused
 * multiply-add and keeps all state in double so host and target stay
 * bit-identical under -ffp-contract=off.
 */

#include "aius_dsp.h"

#include <math.h>
#include <string.h>

/* --- helpers -------------------------------------------------------------- */

/* popcount over the low channel-mask bits (mask is at most 6 bits). */
static int aius_popcount(uint32_t v)
{
    int n = 0;
    while (v) {
        v &= v - 1u;
        n++;
    }
    return n;
}

/* Plane index of channel `bit` within a tightly-packed mask, or -1 if absent.
 * Channels are packed in ascending AIUS_CH_* bit order, so the index is the
 * count of present channels below `bit`. */
static int aius_plane_index(uint32_t mask, uint32_t bit)
{
    if (!(mask & bit)) {
        return -1;
    }
    return aius_popcount(mask & (bit - 1u));
}

/* Round to nearest (ties away from zero) and saturate into the 24-bit range.
 * Deterministic: no library rounding-mode dependence. */
static int32_t aius_quantize(double x)
{
    double r = (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
    if (r > (double)AIUS_DSP_SAMPLE_MAX) {
        return AIUS_DSP_SAMPLE_MAX;
    }
    if (r < (double)AIUS_DSP_SAMPLE_MIN) {
        return AIUS_DSP_SAMPLE_MIN;
    }
    return (int32_t)r;
}

/* --- downmix -------------------------------------------------------------- */

AiusDownmixConfig aius_downmix_config_default(void)
{
    AiusDownmixConfig c;
    c.center = AIUS_DSP_DOWNMIX_CENTER_DEFAULT;
    c.surround = AIUS_DSP_DOWNMIX_SURROUND_DEFAULT;
    c.lfe = AIUS_DSP_LFE_GAIN_DEFAULT;
    c.headroom = AIUS_DSP_HEADROOM_DEFAULT;
    return c;
}

double aius_downmix_clipfree_headroom(const AiusDownmixConfig *cfg)
{
    if (cfg == NULL) {
        return AIUS_DSP_HEADROOM_DEFAULT;
    }
    double lfe = cfg->lfe < 0.0 ? -cfg->lfe : cfg->lfe;
    double denom = 1.0 + cfg->center + cfg->surround + lfe;
    if (denom <= 1.0) {
        return 1.0;
    }
    return 1.0 / denom;
}

AiusStatus aius_dsp_downmix(const AiusDownmixConfig *cfg,
                            const AiusPcmBlock *in,
                            AiusPcmBlock *out)
{
    if (cfg == NULL || in == NULL || out == NULL) {
        return AIUS_INVALID_ARG;
    }
    if (in->channels < 1 || in->channels > AIUS_MAX_CHANNELS) {
        return AIUS_INVALID_ARG;
    }
    if ((int)in->channels != aius_popcount(in->channel_mask)) {
        return AIUS_INVALID_ARG;
    }
    if (out->planes[0] == NULL || out->planes[1] == NULL) {
        return AIUS_INVALID_ARG;
    }

    int32_t *outL = out->planes[0];
    int32_t *outR = out->planes[1];
    double headroom = cfg->headroom;

    /* Mono: replicate the single plane to both channels at unity gain. */
    if (in->channels == 1) {
        const int32_t *m = in->planes[0];
        if (m == NULL) {
            return AIUS_INVALID_ARG;
        }
        for (uint32_t i = 0; i < in->frames; i++) {
            double v = (double)m[i] * headroom;
            outL[i] = aius_quantize(v);
            outR[i] = aius_quantize(v);
        }
    } else {
        /* Resolve plane pointers for every possible 5.1 channel (NULL if
         * absent). */
        int iFL = aius_plane_index(in->channel_mask, AIUS_CH_FL);
        int iFR = aius_plane_index(in->channel_mask, AIUS_CH_FR);
        int iFC = aius_plane_index(in->channel_mask, AIUS_CH_FC);
        int iLFE = aius_plane_index(in->channel_mask, AIUS_CH_LFE);
        int iSL = aius_plane_index(in->channel_mask, AIUS_CH_SL);
        int iSR = aius_plane_index(in->channel_mask, AIUS_CH_SR);

        const int32_t *pFL = iFL >= 0 ? in->planes[iFL] : NULL;
        const int32_t *pFR = iFR >= 0 ? in->planes[iFR] : NULL;
        const int32_t *pFC = iFC >= 0 ? in->planes[iFC] : NULL;
        const int32_t *pLFE = iLFE >= 0 ? in->planes[iLFE] : NULL;
        const int32_t *pSL = iSL >= 0 ? in->planes[iSL] : NULL;
        const int32_t *pSR = iSR >= 0 ? in->planes[iSR] : NULL;

        for (uint32_t i = 0; i < in->frames; i++) {
            double l = 0.0;
            double r = 0.0;
            if (pFL) {
                l += (double)pFL[i];
            }
            if (pFR) {
                r += (double)pFR[i];
            }
            if (pFC) {
                double c = (double)pFC[i] * cfg->center;
                l += c;
                r += c;
            }
            if (pLFE) {
                double e = (double)pLFE[i] * cfg->lfe;
                l += e;
                r += e;
            }
            if (pSL) {
                l += (double)pSL[i] * cfg->surround;
            }
            if (pSR) {
                r += (double)pSR[i] * cfg->surround;
            }
            outL[i] = aius_quantize(l * headroom);
            outR[i] = aius_quantize(r * headroom);
        }
    }

    out->channels = 2;
    out->frames = in->frames;
    out->sample_rate_hz = in->sample_rate_hz;
    out->channel_mask = AIUS_CH_FL | AIUS_CH_FR;
    return AIUS_OK;
}

/* --- biquad coefficient math (RBJ audio EQ cookbook) ---------------------- */

AiusBiquad aius_biquad_bypass(void)
{
    AiusBiquad q;
    q.b0 = 1.0;
    q.b1 = 0.0;
    q.b2 = 0.0;
    q.a1 = 0.0;
    q.a2 = 0.0;
    return q;
}

/* Normalize by a0 and store into an AiusBiquad (negating the feedback taps so
 * the run loop subtracts them). */
static AiusBiquad aius_biquad_norm(double b0, double b1, double b2,
                                   double a0, double a1, double a2)
{
    AiusBiquad q;
    q.b0 = b0 / a0;
    q.b1 = b1 / a0;
    q.b2 = b2 / a0;
    q.a1 = a1 / a0;
    q.a2 = a2 / a0;
    return q;
}

AiusBiquad aius_biquad_lowpass(double fs, double f0, double q)
{
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double b1 = 1.0 - cw;
    double b0 = b1 / 2.0;
    double b2 = b0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cw;
    double a2 = 1.0 - alpha;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_highpass(double fs, double f0, double q)
{
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double b0 = (1.0 + cw) / 2.0;
    double b1 = -(1.0 + cw);
    double b2 = b0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cw;
    double a2 = 1.0 - alpha;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_peaking(double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cw;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cw;
    double a2 = 1.0 - alpha / A;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_lowshelf(double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double tsa = 2.0 * sqrt(A) * alpha;
    double b0 = A * ((A + 1.0) - (A - 1.0) * cw + tsa);
    double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
    double b2 = A * ((A + 1.0) - (A - 1.0) * cw - tsa);
    double a0 = (A + 1.0) + (A - 1.0) * cw + tsa;
    double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
    double a2 = (A + 1.0) + (A - 1.0) * cw - tsa;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_highshelf(double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double tsa = 2.0 * sqrt(A) * alpha;
    double b0 = A * ((A + 1.0) + (A - 1.0) * cw + tsa);
    double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
    double b2 = A * ((A + 1.0) + (A - 1.0) * cw - tsa);
    double a0 = (A + 1.0) - (A - 1.0) * cw + tsa;
    double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
    double a2 = (A + 1.0) - (A - 1.0) * cw - tsa;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

size_t aius_eq_default(AiusBiquad *sections, size_t cap)
{
    if (sections == NULL || cap == 0) {
        return 0;
    }
    sections[0] = aius_biquad_bypass();
    return 1;
}

/* --- EQ run --------------------------------------------------------------- */

void aius_dsp_eq_run(const AiusBiquad *sections, size_t n,
                     AiusBiquadState *state,
                     int32_t *samples, uint32_t frames)
{
    if (sections == NULL || state == NULL || samples == NULL || n == 0) {
        return;
    }
    for (uint32_t i = 0; i < frames; i++) {
        double s = (double)samples[i];
        for (size_t k = 0; k < n; k++) {
            const AiusBiquad *q = &sections[k];
            AiusBiquadState *st = &state[k];
            double y = q->b0 * s + q->b1 * st->x1 + q->b2 * st->x2
                       - q->a1 * st->y1 - q->a2 * st->y2;
            st->x2 = st->x1;
            st->x1 = s;
            st->y2 = st->y1;
            st->y1 = y;
            s = y;
        }
        samples[i] = aius_quantize(s);
    }
}

/* --- full process --------------------------------------------------------- */

AiusStatus aius_dsp_process(const AiusDspConfig *cfg,
                            const AiusPcmBlock *in,
                            AiusPcmBlock *out)
{
    if (cfg == NULL) {
        return AIUS_INVALID_ARG;
    }
    if (cfg->eq_sections > 0 && (cfg->eq == NULL || cfg->eq_state == NULL)) {
        return AIUS_INVALID_ARG;
    }

    AiusStatus st = aius_dsp_downmix(&cfg->downmix, in, out);
    if (st != AIUS_OK) {
        return st;
    }

    if (cfg->eq_sections > 0) {
        aius_dsp_eq_run(cfg->eq, cfg->eq_sections, cfg->eq_state,
                        out->planes[0], out->frames);
        aius_dsp_eq_run(cfg->eq, cfg->eq_sections,
                        cfg->eq_state + cfg->eq_sections,
                        out->planes[1], out->frames);
    }
    return AIUS_OK;
}

/* --- arena output allocation ---------------------------------------------- */

AiusStatus aius_dsp_alloc_stereo(AiusArena *arena, uint32_t frames,
                                 uint32_t sample_rate_hz, AiusPcmBlock *out)
{
    if (arena == NULL || out == NULL) {
        return AIUS_INVALID_ARG;
    }
    size_t bytes = (size_t)frames * sizeof(int32_t);
    int32_t *l = (int32_t *)aius_arena_alloc(arena, bytes, sizeof(int32_t));
    int32_t *r = (int32_t *)aius_arena_alloc(arena, bytes, sizeof(int32_t));
    if ((frames > 0 && (l == NULL || r == NULL))) {
        return AIUS_NO_SPACE;
    }
    memset(out, 0, sizeof *out);
    out->planes[0] = l;
    out->planes[1] = r;
    out->channels = 2;
    out->frames = frames;
    out->sample_rate_hz = sample_rate_hz;
    out->channel_mask = AIUS_CH_FL | AIUS_CH_FR;
    return AIUS_OK;
}
