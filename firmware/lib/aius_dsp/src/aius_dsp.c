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

/* Shared cascade core: multiply each sample by `pregain`, run the `n`-section
 * Direct Form I cascade, and quantize once. n == 0 applies pregain + quantize
 * only. Callers guarantee `state` holds `n` sections when n > 0. */
static void run_cascade(double pregain, const AiusBiquad *sections, size_t n,
                        AiusBiquadState *state, int32_t *samples,
                        uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        double s = (double)samples[i] * pregain;
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

void aius_dsp_eq_run(const AiusBiquad *sections, size_t n,
                     AiusBiquadState *state,
                     int32_t *samples, uint32_t frames)
{
    if (sections == NULL || state == NULL || samples == NULL || n == 0) {
        return;
    }
    run_cascade(1.0, sections, n, state, samples, frames);
}

/* --- input stage: per-channel trim + EQ (pre-downmix) --------------------- */

double aius_db_to_linear(double db)
{
    return pow(10.0, db / 20.0);
}

AiusInputChannelConfig aius_input_channel_default(void)
{
    AiusInputChannelConfig ch;
    ch.gain_linear = 1.0;
    ch.eq = NULL;
    ch.eq_sections = 0;
    ch.eq_state = NULL;
    return ch;
}

void aius_dsp_input_run(const AiusInputChannelConfig *ch,
                        int32_t *samples, uint32_t frames)
{
    if (ch == NULL || samples == NULL) {
        return;
    }
    const AiusBiquad *sec = ch->eq;
    AiusBiquadState *st = ch->eq_state;
    size_t n = ch->eq_sections;
    if (n > 0 && (sec == NULL || st == NULL)) {
        n = 0; /* trim still applies; EQ skipped when misconfigured */
    }
    run_cascade(ch->gain_linear, sec, n, st, samples, frames);
}

AiusInputStage aius_input_stage_default(void)
{
    AiusInputStage stage;
    memset(&stage, 0, sizeof stage);
    for (int c = 0; c < AIUS_DSP_INPUT_CHANNELS; c++) {
        stage.channels[c] = aius_input_channel_default();
    }
    return stage;
}

/* Stage every decoded plane through its input channel's trim + EQ into the
 * stage's scratch planes, and fill `staged` to mirror `in` with those scratch
 * planes. Channels are packed in ascending AIUS_CH_* bit order, so the k-th
 * present bit is plane index k and its per-channel config is channels[bit
 * ordinal]. Returns AIUS_INVALID_ARG on a null/absent plane or scratch. */
static AiusStatus input_stage_apply(const AiusInputStage *stage,
                                    const AiusPcmBlock *in, AiusPcmBlock *staged)
{
    static const uint32_t order[AIUS_DSP_INPUT_CHANNELS] = {
        AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_FC, AIUS_CH_LFE, AIUS_CH_SL, AIUS_CH_SR,
    };
    if (in == NULL || staged == NULL) {
        return AIUS_INVALID_ARG;
    }
    if (in->channels < 1 || in->channels > AIUS_MAX_CHANNELS) {
        return AIUS_INVALID_ARG;
    }
    if ((int)in->channels != aius_popcount(in->channel_mask)) {
        return AIUS_INVALID_ARG;
    }

    *staged = *in;
    int p = 0;
    for (int b = 0; b < AIUS_DSP_INPUT_CHANNELS; b++) {
        if (!(in->channel_mask & order[b])) {
            continue;
        }
        const int32_t *src = in->planes[p];
        int32_t *dst = stage->scratch[p];
        if (src == NULL || dst == NULL) {
            return AIUS_INVALID_ARG;
        }
        memcpy(dst, src, (size_t)in->frames * sizeof(int32_t));
        aius_dsp_input_run(&stage->channels[b], dst, in->frames);
        staged->planes[p] = dst;
        p++;
    }
    return AIUS_OK;
}

/* --- true-peak safety limiter --------------------------------------------- */

AiusLimiterConfig aius_limiter_config_default(void)
{
    AiusLimiterConfig c;
    c.enabled = 0;
    c.threshold_dbfs = 0.0;
    c.attack_ms = 1.0;
    c.release_ms = 100.0;
    c.lookahead_ms = 0.0;
    return c;
}

/* One-pole smoothing coefficient for a time constant `ms` at `fs` Hz:
 * exp(-1 / (ms * fs / 1000)). Larger ms -> coefficient nearer 1 (slower). */
static double limiter_coeff(double ms, double fs)
{
    double samples = ms * fs / 1000.0;
    if (samples < 1e-9) {
        samples = 1e-9;
    }
    return exp(-1.0 / samples);
}

void aius_dsp_limiter_run(AiusLimiter *lim, double fs,
                          int32_t *l, int32_t *r, uint32_t frames)
{
    if (lim == NULL || l == NULL || r == NULL) {
        return;
    }
    if (!lim->cfg.enabled || fs <= 0.0) {
        return; /* pure bypass: no delay, no gain change */
    }

    /* Derived, off the per-sample path. */
    double thr_lin =
        pow(10.0, lim->cfg.threshold_dbfs / 20.0) * (double)AIUS_DSP_SAMPLE_MAX;
    double atk = limiter_coeff(lim->cfg.attack_ms, fs);
    double rel = limiter_coeff(lim->cfg.release_ms, fs);
    double look = lim->cfg.lookahead_ms * fs / 1000.0;
    uint32_t delay = (uint32_t)(look + 0.5);
    if (delay > AIUS_DSP_LIMITER_MAX_LOOKAHEAD) {
        delay = AIUS_DSP_LIMITER_MAX_LOOKAHEAD;
    }

    AiusLimiterState *s = &lim->state;
    if (!s->primed || s->delay != delay) {
        s->gain = 1.0;
        s->env = 0.0;
        s->delay = delay;
        s->pos = 0;
        memset(s->buf_l, 0, sizeof s->buf_l);
        memset(s->buf_r, 0, sizeof s->buf_r);
        s->primed = 1;
    }

    for (uint32_t i = 0; i < frames; i++) {
        int32_t cur_l = l[i];
        int32_t cur_r = r[i];

        /* Peak follower on the incoming (lookahead-leading) sample: instant
         * attack, release-timed decay. */
        double al = cur_l < 0 ? -(double)cur_l : (double)cur_l;
        double ar = cur_r < 0 ? -(double)cur_r : (double)cur_r;
        double m = al > ar ? al : ar;
        if (m > s->env) {
            s->env = m; /* instant attack: never miss a peak */
        } else {
            s->env = m + (s->env - m) * rel; /* release-timed decay toward level */
        }

        /* Desired stereo-linked gain, then smooth it for a click-free ramp. */
        double desired = 1.0;
        if (s->env > thr_lin) {
            desired = thr_lin / s->env;
        }
        double coeff = (desired < s->gain) ? atk : rel;
        s->gain = desired + (s->gain - desired) * coeff;

        /* Apply the gain to the delayed sample so the reduction leads the peak. */
        int32_t del_l = cur_l;
        int32_t del_r = cur_r;
        if (delay > 0) {
            del_l = s->buf_l[s->pos];
            del_r = s->buf_r[s->pos];
            s->buf_l[s->pos] = cur_l;
            s->buf_r[s->pos] = cur_r;
            s->pos++;
            if (s->pos >= delay) {
                s->pos = 0;
            }
        }
        l[i] = aius_quantize((double)del_l * s->gain);
        r[i] = aius_quantize((double)del_r * s->gain);
    }
}

double aius_dsp_limiter_gain_reduction_db(const AiusLimiter *lim)
{
    if (lim == NULL) {
        return 0.0;
    }
    double g = lim->state.primed ? lim->state.gain : 1.0;
    if (g <= 0.0) {
        return -1000.0; /* fully closed; defensive, gain stays > 0 in practice */
    }
    if (g > 1.0) {
        g = 1.0;
    }
    return 20.0 * log10(g);
}

/* --- coherent meter snapshot ---------------------------------------------- */

/* AIUS_CH_* bit order for the six metered input channels, matching the input
 * stage's channel indexing. */
static const uint32_t k_meter_order[AIUS_METER_INPUT_CHANNELS] = {
    AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_FC, AIUS_CH_LFE, AIUS_CH_SL, AIUS_CH_SR,
};

void aius_meter_init(AiusMeter *m, uint32_t sample_rate_hz)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof *m);
    m->sample_rate_hz = sample_rate_hz;
}

void aius_meter_reset_accumulators(AiusMeter *m)
{
    if (m == NULL) {
        return;
    }
    memset(m->in_peak, 0, sizeof m->in_peak);
    memset(m->in_sqsum, 0, sizeof m->in_sqsum);
    memset(m->out_peak, 0, sizeof m->out_peak);
    memset(m->out_sqsum, 0, sizeof m->out_sqsum);
    memset(m->wave, 0, sizeof m->wave);
    m->wave_count = 0;
    m->in_frames = 0;
    m->out_frames = 0;
}

void aius_meter_accumulate_input(AiusMeter *m, const AiusPcmBlock *block)
{
    if (m == NULL || block == NULL || block->frames == 0) {
        return;
    }
    int p = 0;
    for (int c = 0; c < AIUS_METER_INPUT_CHANNELS; c++) {
        if (!(block->channel_mask & k_meter_order[c])) {
            continue;
        }
        const int32_t *plane = block->planes[p];
        p++;
        if (plane == NULL) {
            continue; /* misconfigured plane: leave this channel at zero */
        }
        double peak = m->in_peak[c];
        double sqsum = m->in_sqsum[c];
        for (uint32_t i = 0; i < block->frames; i++) {
            double v = (double)plane[i];
            double a = v < 0.0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
            sqsum += v * v;
        }
        m->in_peak[c] = peak;
        m->in_sqsum[c] = sqsum;
    }
    m->in_frames += block->frames;
}

void aius_meter_accumulate_output(AiusMeter *m, const int32_t *l, const int32_t *r,
                                  uint32_t frames)
{
    if (m == NULL || l == NULL || r == NULL || frames == 0) {
        return;
    }
    double peak_l = m->out_peak[0];
    double peak_r = m->out_peak[1];
    double sq_l = m->out_sqsum[0];
    double sq_r = m->out_sqsum[1];
    for (uint32_t i = 0; i < frames; i++) {
        double vl = (double)l[i];
        double vr = (double)r[i];
        double al = vl < 0.0 ? -vl : vl;
        double ar = vr < 0.0 ? -vr : vr;
        if (al > peak_l) {
            peak_l = al;
        }
        if (ar > peak_r) {
            peak_r = ar;
        }
        sq_l += vl * vl;
        sq_r += vr * vr;
    }
    m->out_peak[0] = peak_l;
    m->out_peak[1] = peak_r;
    m->out_sqsum[0] = sq_l;
    m->out_sqsum[1] = sq_r;
    m->out_frames += frames;
    m->wave_count = aius_meter_decimate(l, r, frames, m->wave, AIUS_METER_WAVEFORM_MAX);
}

void aius_meter_set_limiter_gr(AiusMeter *m, double gain_reduction_db)
{
    if (m == NULL) {
        return;
    }
    m->limiter_gr_db = gain_reduction_db;
}

void aius_meter_set_telemetry(AiusMeter *m, uint32_t queue_frames,
                              uint32_t estimated_latency_us,
                              uint32_t dsp_cycles_last, uint32_t dsp_cycles_max)
{
    if (m == NULL) {
        return;
    }
    m->queue_frames = queue_frames;
    m->estimated_latency_us = estimated_latency_us;
    m->dsp_cycles_last = dsp_cycles_last;
    m->dsp_cycles_max = dsp_cycles_max;
}

uint32_t aius_meter_decimate(const int32_t *l, const int32_t *r, uint32_t frames,
                             int32_t *out, uint32_t cap)
{
    if (out == NULL || cap == 0 || frames == 0 || l == NULL) {
        return 0;
    }
    uint32_t points = frames < cap ? frames : cap;
    for (uint32_t b = 0; b < points; b++) {
        /* Contiguous, near-equal buckets: [start, end). Computed in 64-bit so the
         * product cannot overflow before the divide. */
        uint32_t start = (uint32_t)((uint64_t)b * frames / points);
        uint32_t end = (uint32_t)((uint64_t)(b + 1) * frames / points);
        int32_t pk = 0;
        for (uint32_t i = start; i < end; i++) {
            int32_t al = l[i] < 0 ? -l[i] : l[i];
            if (al > pk) {
                pk = al;
            }
            if (r != NULL) {
                int32_t ar = r[i] < 0 ? -r[i] : r[i];
                if (ar > pk) {
                    pk = ar;
                }
            }
        }
        out[b] = pk;
    }
    return points;
}

void aius_meter_publish(AiusMeter *m)
{
    if (m == NULL) {
        return;
    }
    AiusMeterSnapshot *s = &m->published;

    /* Seqlock write boundary: make seq odd before touching the payload. */
    m->seq++;

    m->publishes++;
    s->sequence = m->publishes;
    s->sample_rate_hz = m->sample_rate_hz;

    for (int c = 0; c < AIUS_METER_INPUT_CHANNELS; c++) {
        s->input_peak[c] = m->in_peak[c];
        s->input_rms[c] = m->in_frames > 0
                              ? sqrt(m->in_sqsum[c] / (double)m->in_frames)
                              : 0.0;
    }
    for (int k = 0; k < 2; k++) {
        s->output_peak[k] = m->out_peak[k];
        s->output_rms[k] = m->out_frames > 0
                               ? sqrt(m->out_sqsum[k] / (double)m->out_frames)
                               : 0.0;
    }
    s->limiter_gain_reduction_db = m->limiter_gr_db;

    s->waveform_count = m->wave_count;
    for (uint32_t i = 0; i < AIUS_METER_WAVEFORM_MAX; i++) {
        s->waveform[i] = i < m->wave_count ? m->wave[i] : 0;
    }

    s->queue_frames = m->queue_frames;
    s->estimated_latency_us = m->estimated_latency_us;
    s->dsp_cycles_last = m->dsp_cycles_last;
    s->dsp_cycles_max = m->dsp_cycles_max;

    /* Seqlock write boundary: make seq even to mark the snapshot complete. */
    m->seq++;

    aius_meter_reset_accumulators(m);
}

int aius_meter_read(const AiusMeter *m, AiusMeterSnapshot *out)
{
    if (m == NULL || out == NULL) {
        return 0;
    }
    for (int tries = 0; tries < AIUS_METER_READ_RETRIES; tries++) {
        uint32_t s1 = m->seq;
        if (s1 & 1u) {
            continue; /* publish in progress: retry */
        }
        *out = m->published;
        uint32_t s2 = m->seq;
        if (s1 == s2) {
            return 1; /* coherent: no publish straddled the copy */
        }
    }
    return 0; /* contended past the retry bound: reject, never return torn */
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

    /* Input stage (trim + EQ) before the downmix, staged into caller scratch so
     * the const decoded input is untouched. */
    const AiusPcmBlock *dmix_in = in;
    AiusPcmBlock staged;
    if (cfg->input != NULL) {
        AiusStatus ist = input_stage_apply(cfg->input, in, &staged);
        if (ist != AIUS_OK) {
            return ist;
        }
        dmix_in = &staged;
    }

    AiusStatus st = aius_dsp_downmix(&cfg->downmix, dmix_in, out);
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

    if (cfg->limiter != NULL) {
        aius_dsp_limiter_run(cfg->limiter, (double)out->sample_rate_hz,
                             out->planes[0], out->planes[1], out->frames);
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
