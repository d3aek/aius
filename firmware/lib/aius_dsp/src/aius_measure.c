#include "aius_measure.h"

#include <math.h>
#include <string.h>

/* Cold code -> FLASH, hot code -> ITCM, the same split aius_replay.c documents.
 *
 * The ONLY function here that runs per audio block is aius_measure_isr_block
 * (the tap), so it alone carries no attribute and stays in ITCM. Everything
 * else -- arm/cancel/retire transitions and the whole sliced analysis -- runs
 * in epoch or HTTP context and carries FLASHMEM on its definition, leaving
 * ITCM to the code that earns it (docs/plans/aius-network-hardening.md,
 * "Deciding principle"). The attribute must sit on each DEFINITION: GCC does
 * not merge a `section` attribute from a forward declaration. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

/* --- capture state machine -------------------------------------------------- */

FLASHMEM AiusStatus aius_measure_capture_init(AiusMeasureCapture *c,
                                              int32_t *l, int32_t *r,
                                              uint32_t frames_cap)
{
    if (!c || !l || !r || frames_cap == 0)
        return AIUS_INVALID_ARG;
    memset(c, 0, sizeof *c);
    c->l = l;
    c->r = r;
    c->frames_cap = frames_cap;
    c->state = AIUS_MEASURE_IDLE;
    return AIUS_OK;
}

FLASHMEM AiusStatus aius_measure_arm(AiusMeasureCapture *c,
                                     uint32_t config_generation,
                                     uint32_t overruns)
{
    if (!c || !c->l || !c->r)
        return AIUS_INVALID_ARG;
    if (c->state != AIUS_MEASURE_IDLE)
        return AIUS_INVALID_ARG;
    /* Stage/commit: every parameter is stored before the state store, so an
     * ISR that observes `armed` observes complete arm parameters. The target
     * adapter issues the barrier between the two, as it does for the transmit
     * ring's commit. */
    c->fail_reason = AIUS_MEASURE_FAIL_NONE;
    c->cursor = 0;
    c->armed_generation = config_generation;
    c->armed_overruns = overruns;
    c->state = AIUS_MEASURE_ARMED;
    return AIUS_OK;
}

/* Hot: the tap, reached from AiusAudioProcessor::enqueue() once per committed
 * block while a capture is in flight. Bounded: one latch compare pair and at
 * most one block's memcpy per plane. */
void aius_measure_isr_block(AiusMeasureCapture *c,
                            const int32_t *l, const int32_t *r,
                            uint32_t frames,
                            uint32_t config_generation,
                            uint32_t overruns)
{
    if (!c)
        return;
    uint32_t state = c->state;

    if (state == AIUS_MEASURE_CANCELLING) {
        /* Acknowledge the token; copy nothing. One side of the dual-terminal
         * cancel edge (the epoch's reclaim is the other): both store the same
         * terminal state value, so losing the race is benign. */
        c->fail_reason = AIUS_MEASURE_FAIL_CANCELLED;
        c->state = AIUS_MEASURE_FAILED;
        return;
    }
    if (state != AIUS_MEASURE_ARMED && state != AIUS_MEASURE_CAPTURING)
        return; /* idle/captured/analysing/done/failed: a stale block is a no-op */

    /* The arm-time latch check, at every block boundary: a config-generation
     * splice or an overrun's 512 dropped frames inside the window is exactly
     * the seam artifact the coherent stimulus paid to remove, so the capture
     * retires instead of recording it. The failing block is NOT copied. */
    if (config_generation != c->armed_generation) {
        c->fail_reason = AIUS_MEASURE_FAIL_GENERATION;
        c->state = AIUS_MEASURE_FAILED;
        return;
    }
    if (overruns != c->armed_overruns) {
        c->fail_reason = AIUS_MEASURE_FAIL_OVERRUN;
        c->state = AIUS_MEASURE_FAILED;
        return;
    }

    if (state == AIUS_MEASURE_ARMED)
        c->state = AIUS_MEASURE_CAPTURING;

    if (!l || !r || frames == 0)
        return;
    uint32_t cursor = c->cursor;
    uint32_t room = c->frames_cap - cursor;
    uint32_t take = frames < room ? frames : room;
    memcpy(c->l + cursor, l, (size_t)take * sizeof(int32_t));
    memcpy(c->r + cursor, r, (size_t)take * sizeof(int32_t));
    cursor += take;
    c->cursor = cursor;
    if (cursor == c->frames_cap)
        c->state = AIUS_MEASURE_CAPTURED; /* the ISR's terminal: buffer-complete */
}

FLASHMEM AiusStatus aius_measure_cancel(AiusMeasureCapture *c)
{
    if (!c)
        return AIUS_INVALID_ARG;
    if (c->state != AIUS_MEASURE_ARMED && c->state != AIUS_MEASURE_CAPTURING)
        return AIUS_INVALID_ARG;
    c->state = AIUS_MEASURE_CANCELLING; /* the token; the ISR acknowledges it */
    return AIUS_OK;
}

FLASHMEM AiusStatus aius_measure_reclaim(AiusMeasureCapture *c)
{
    if (!c)
        return AIUS_INVALID_ARG;
    /* The ISR's acknowledgement may land between the epoch's observation of
     * `cancelling` and this call -- the interleaving the native suite drives.
     * Same terminal state either way; the first reason is kept. */
    if (c->state == AIUS_MEASURE_FAILED)
        return AIUS_OK;
    if (c->state != AIUS_MEASURE_CANCELLING)
        return AIUS_INVALID_ARG;
    c->fail_reason = AIUS_MEASURE_FAIL_RECLAIMED;
    c->state = AIUS_MEASURE_FAILED;
    return AIUS_OK;
}

FLASHMEM AiusStatus aius_measure_begin_analysis(AiusMeasureCapture *c)
{
    if (!c || c->state != AIUS_MEASURE_CAPTURED)
        return AIUS_INVALID_ARG;
    c->state = AIUS_MEASURE_ANALYSING;
    return AIUS_OK;
}

FLASHMEM AiusStatus aius_measure_finish_analysis(AiusMeasureCapture *c)
{
    if (!c || c->state != AIUS_MEASURE_ANALYSING)
        return AIUS_INVALID_ARG;
    c->state = AIUS_MEASURE_DONE;
    return AIUS_OK;
}

FLASHMEM AiusStatus aius_measure_release(AiusMeasureCapture *c)
{
    if (!c)
        return AIUS_INVALID_ARG;
    if (c->state != AIUS_MEASURE_DONE && c->state != AIUS_MEASURE_FAILED)
        return AIUS_INVALID_ARG;
    c->cursor = 0;
    c->state = AIUS_MEASURE_IDLE;
    return AIUS_OK;
}

/* --- sliced analysis -------------------------------------------------------- */

FLASHMEM AiusStatus aius_measure_analysis_init(AiusMeasureAnalysis *a,
                                               double fs_hz, double f0_hz,
                                               uint32_t window_frames,
                                               const double *probe_hz,
                                               uint32_t probes)
{
    if (!a || !(fs_hz > 0.0) || window_frames == 0)
        return AIUS_INVALID_ARG;
    if (!(f0_hz > 0.0) || !(f0_hz < fs_hz / 2.0))
        return AIUS_INVALID_ARG;
    if (probes > AIUS_MEASURE_PROBES_MAX || (probes > 0 && !probe_hz))
        return AIUS_INVALID_ARG;

    memset(a, 0, sizeof *a);
    a->fs_hz = fs_hz;
    a->f0_hz = f0_hz;
    a->n_total = window_frames;

    /* Init-time transcendentals seed the per-sample recurrences; the slice
     * path itself is pure +,-,* (see the header's determinism note). */
    double w0 = 2.0 * M_PI * f0_hz / fs_hz;
    a->rot_c = cos(w0);
    a->rot_s = sin(w0);
    a->bc = 1.0; /* cos(0), sin(0) */
    a->bs = 0.0;

    double ww = 2.0 * M_PI / (double)window_frames;
    a->wrot_c = cos(ww);
    a->wrot_s = sin(ww);
    a->wc = 1.0;
    a->ws = 0.0;

    /* Harmonic bins 2..10 of f0, dropped at Nyquist and above. */
    uint32_t bins = 0;
    for (uint32_t k = 2; k <= AIUS_MEASURE_HARMONICS + 1; k++) {
        double f = f0_hz * (double)k;
        if (!(f < fs_hz / 2.0))
            break;
        a->bin_cosw[bins] = cos(2.0 * M_PI * f / fs_hz);
        a->bin_sinw[bins] = sin(2.0 * M_PI * f / fs_hz);
        bins++;
    }
    a->harmonics = bins;
    for (uint32_t p = 0; p < probes; p++) {
        double f = probe_hz[p];
        if (!(f > 0.0) || !(f < fs_hz / 2.0))
            return AIUS_INVALID_ARG;
        a->bin_cosw[bins] = cos(2.0 * M_PI * f / fs_hz);
        a->bin_sinw[bins] = sin(2.0 * M_PI * f / fs_hz);
        bins++;
    }
    a->probes = probes;
    a->bins = bins;
    return AIUS_OK;
}

FLASHMEM AiusStatus aius_measure_analysis_slice(AiusMeasureAnalysis *a,
                                                const int32_t *samples,
                                                uint32_t frames)
{
    if (!a || (!samples && frames > 0))
        return AIUS_INVALID_ARG;
    uint32_t left = a->n_total - a->n;
    if (frames > left)
        frames = left;

    /* Hoist the accumulator set into locals so the loop runs on registers;
     * the struct is written back once per slice. The write-back preserves the
     * exact operation sequence, so slicing cannot change the figures. */
    double bc = a->bc, bs = a->bs;
    const double rc = a->rot_c, rs = a->rot_s;
    double wc = a->wc, ws = a->ws;
    const double wrc = a->wrot_c, wrs = a->wrot_s;
    double scc = a->scc, sss = a->sss, scs = a->scs, sc = a->sc, ss = a->ss;
    double sxc = a->sxc, sxs = a->sxs, sx = a->sx, sxx = a->sxx;
    double sxc_e = a->sxc_e, sxs_e = a->sxs_e, sx_e = a->sx_e, sxx_e = a->sxx_e;
    double wsum = a->wsum;

/* Neumaier-compensated add into sum/err locals: the finish-time residual
 * identity exposes a value ~14 orders below these sums, so their naive
 * accumulated rounding would read as signal (see the header). Requires the
 * suite's -ffp-contract=off: a contracted fused multiply-add would break the
 * error-term algebra. */
#define ACC(sum, err, term)                                                    \
    do {                                                                       \
        double acc_t_ = (sum) + (term);                                        \
        if (fabs(sum) >= fabs(term))                                           \
            (err) += ((sum)-acc_t_) + (term);                                  \
        else                                                                   \
            (err) += ((term)-acc_t_) + (sum);                                  \
        (sum) = acc_t_;                                                        \
    } while (0)

    for (uint32_t i = 0; i < frames; i++) {
        double x = (double)samples[i];

        /* 3-parameter fit sums, unwindowed: the fit needs no window and no
         * coherence -- that is exactly why it replaced the leakage-bin idea.
         * The Gram sums are O(n) with O(1) terms and need no compensation. */
        scc += bc * bc;
        sss += bs * bs;
        scs += bc * bs;
        sc += bc;
        ss += bs;
        ACC(sxc, sxc_e, x * bc);
        ACC(sxs, sxs_e, x * bs);
        ACC(sx, sx_e, x);
        ACC(sxx, sxx_e, x * x);

        /* Hann window value from its own oscillator: w = 0.5 - 0.5*cos(ww n). */
        double w = 0.5 - 0.5 * wc;
        wsum += w;
        double xw = x * w;

        /* Windowed Goertzel bins (harmonics then probes). */
        for (uint32_t b = 0; b < a->bins; b++) {
            double s0 = xw + 2.0 * a->bin_cosw[b] * a->bin_s1[b] - a->bin_s2[b];
            a->bin_s2[b] = a->bin_s1[b];
            a->bin_s1[b] = s0;
        }

        /* Advance both rotations. */
        double nbc = bc * rc - bs * rs;
        double nbs = bs * rc + bc * rs;
        bc = nbc;
        bs = nbs;
        double nwc = wc * wrc - ws * wrs;
        double nws = ws * wrc + wc * wrs;
        wc = nwc;
        ws = nws;
    }

#undef ACC

    a->bc = bc;
    a->bs = bs;
    a->wc = wc;
    a->ws = ws;
    a->scc = scc;
    a->sss = sss;
    a->scs = scs;
    a->sc = sc;
    a->ss = ss;
    a->sxc = sxc;
    a->sxs = sxs;
    a->sx = sx;
    a->sxx = sxx;
    a->sxc_e = sxc_e;
    a->sxs_e = sxs_e;
    a->sx_e = sx_e;
    a->sxx_e = sxx_e;
    a->wsum = wsum;
    a->n += frames;
    return AIUS_OK;
}

/* Power ratio to dB, clamped to [DB_FLOOR, -DB_FLOOR] so every figure is
 * finite and deterministic; a zero numerator reports the floor. */
static FLASHMEM double power_db(double num, double den)
{
    if (!(num > 0.0) || !(den > 0.0))
        return AIUS_MEASURE_DB_FLOOR;
    double db = 10.0 * log10(num / den);
    if (db < AIUS_MEASURE_DB_FLOOR)
        return AIUS_MEASURE_DB_FLOOR;
    if (db > -AIUS_MEASURE_DB_FLOOR)
        return -AIUS_MEASURE_DB_FLOOR;
    return db;
}

static FLASHMEM double amplitude_dbfs(double amp)
{
    if (!(amp > 0.0))
        return AIUS_MEASURE_DB_FLOOR;
    double db = 20.0 * log10(amp / (double)AIUS_DSP_SAMPLE_MAX);
    return db < AIUS_MEASURE_DB_FLOOR ? AIUS_MEASURE_DB_FLOOR : db;
}

FLASHMEM AiusStatus aius_measure_analysis_finish(const AiusMeasureAnalysis *a,
                                                 AiusMeasureFigures *out)
{
    if (!a || !out || a->n == 0 || a->n != a->n_total)
        return AIUS_INVALID_ARG;

    /* Solve the 3x3 normal equations for x[n] ~= A*cos + B*sin + C by Gaussian
     * elimination with partial pivoting. The Gram matrix of {cos, sin, 1} over
     * thousands of samples at any in-band f0 is far from singular; a pivot at
     * exact zero can only mean a degenerate call and fails closed. */
    double sxc = a->sxc + a->sxc_e; /* fold in the Neumaier compensation */
    double sxs = a->sxs + a->sxs_e;
    double sx = a->sx + a->sx_e;
    double sxx = a->sxx + a->sxx_e;
    double m[3][4] = {
        {a->scc, a->scs, a->sc, sxc},
        {a->scs, a->sss, a->ss, sxs},
        {a->sc, a->ss, (double)a->n, sx},
    };
    for (int col = 0; col < 3; col++) {
        int pivot = col;
        for (int row = col + 1; row < 3; row++)
            if (fabs(m[row][col]) > fabs(m[pivot][col]))
                pivot = row;
        if (m[pivot][col] == 0.0)
            return AIUS_INVALID_ARG;
        if (pivot != col)
            for (int k = 0; k < 4; k++) {
                double t = m[col][k];
                m[col][k] = m[pivot][k];
                m[pivot][k] = t;
            }
        for (int row = 0; row < 3; row++) {
            if (row == col)
                continue;
            double f = m[row][col] / m[col][col];
            for (int k = col; k < 4; k++)
                m[row][k] -= f * m[col][k];
        }
    }
    double fit_a = m[0][3] / m[0][0];
    double fit_b = m[1][3] / m[1][1];
    double fit_c = m[2][3] / m[2][2];

    /* Least-squares identity: residual sum of squares = sxx - coef . rhs. */
    double rss = sxx - (fit_a * sxc + fit_b * sxs + fit_c * sx);
    if (rss < 0.0)
        rss = 0.0;
    double residual_power = rss / (double)a->n;

    double fund_amp = sqrt(fit_a * fit_a + fit_b * fit_b);
    double fund_power = fund_amp * fund_amp / 2.0;

    memset(out, 0, sizeof *out);
    out->f0_hz = a->f0_hz;
    out->frames = a->n;
    out->fundamental_dbfs = amplitude_dbfs(fund_amp);
    out->dc_dbfs = amplitude_dbfs(fabs(fit_c));
    out->thd_n_db = power_db(residual_power, fund_power);

    /* Goertzel finalization in Re/Im form -- X = s1 - e^{-jw} s2 -- rather
     * than the phase-free s1^2 + s2^2 - 2cos(w) s1 s2: on an empty bin the
     * recurrence state is orders larger than |X|, and the phase-free form
     * squares BEFORE it cancels, losing the bin to rounding. Amplitude =
     * 2|X| / sum(w[n]), the exact Hann coherent-gain correction using the
     * window actually accumulated rather than the asymptotic N/2. */
    double harmonic_power = 0.0;
    out->harmonics = a->harmonics;
    for (uint32_t b = 0; b < a->bins; b++) {
        double re = a->bin_s1[b] - a->bin_cosw[b] * a->bin_s2[b];
        double im = a->bin_sinw[b] * a->bin_s2[b];
        double magsq = re * re + im * im;
        double amp = a->wsum > 0.0 ? 2.0 * sqrt(magsq) / a->wsum : 0.0;
        double p = amp * amp / 2.0;
        if (b < a->harmonics) {
            harmonic_power += p;
            out->harmonic_dbc[b] = power_db(p, fund_power);
        } else {
            out->probe_dbfs[b - a->harmonics] = amplitude_dbfs(amp);
        }
    }
    out->probes = a->probes;
    out->thd_db = power_db(harmonic_power, fund_power);

    /* SNR excludes harmonics 2..10 from the residual; THD+N does not -- the
     * two are different numbers by definition. The subtraction can underflow
     * when the harmonic estimates carry the residual's own noise, so it
     * clamps at zero and the dB helper's floor bounds the report. */
    double noise_power = residual_power - harmonic_power;
    if (noise_power > 0.0)
        out->snr_db = power_db(fund_power, noise_power);
    else
        /* Underflowed subtraction with a live fundamental: SNR beyond every
         * resolvable floor, clamped high; no fundamental at all: the floor. */
        out->snr_db = fund_power > 0.0 ? -AIUS_MEASURE_DB_FLOOR
                                       : AIUS_MEASURE_DB_FLOOR;
    return AIUS_OK;
}
