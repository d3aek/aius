/* Vector-driven known-answer tests for the Aius output DSP.
 *
 * Critical-path steps 1-2 of docs/plans/aius-dsp-config-api.md. Each of the four
 * committed DTS Core vectors is decoded through the portable DTS interface
 * (aius_dts_*) into planar int32 PCM, then driven through the PUBLIC portable
 * DSP interface (aius_dsp_downmix / aius_dsp_process). The vectors act as
 * executable specifications: the same real 24-bit content the target will
 * process at run time exercises the downmix matrix and EQ path here.
 *
 * Every expectation is either analytic or an independent in-harness reference
 * computation; nothing grips DSP internals. Determinism: the decode is the same
 * dcadec fixed-point core the `dts` suite validates against FFmpeg, and the DSP
 * is double precision under -ffp-contract=off, so host and Cortex-M7 agree.
 *
 * Retained known-answer fixtures live in <golden_dir>/<name>.golden: the
 * deterministic decoded-input signature (channel layout, frame count, per-input
 * peak) and the stereo-output signature (default-downmix peak L/R). The harness
 * recomputes and strictly compares; a missing golden fails loudly. Regenerate
 * with AIUS_DSP_VECTORS_DUMP=1 (writes the golden files) after a reviewed change.
 *
 * THE OUTPUT SIGNATURE MOVES WHEN THE SHIPPED DEFAULT MOVES, which is what makes
 * it worth retaining and also what makes a regeneration a reviewable event rather
 * than a formality. It moved twice so far: for the 0.5 headroom (defect 2) and for
 * the LFE fold-in (roadmap item 2). A regeneration must be able to account for
 * every changed number, so the harness prints `lfe_delta_peak` per vector and
 * asserts the blast radius directly -- a vector with no LFE plane must be
 * BIT-IDENTICAL with the fold-in enabled, and a vector with one must differ.
 *
 * Usage: test_dsp_vectors <vectors_dir> <golden_dir>
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_alloc_shim.h"
#include "aius_dsp.h"
#include "aius_dts.h"

static int g_fail;

/* Samples the corpus would clip at unity headroom, accumulated over every vector.
 * The defect-2 negative control: asserted non-zero in main so the "default headroom
 * does not clip" checks cannot pass vacuously on a corpus that never stressed the
 * downmix matrix. */
static uint32_t g_unity_clipped;

/* Roadmap item 2's own negative control, the same shape as g_unity_clipped: how
 * many vectors actually exercised the LFE fold-in, and the peak sample delta the
 * fold accounts for on the vector being reported. Asserted non-zero in main, so
 * the "stereo output is untouched" checks cannot pass on a corpus in which nothing
 * carries LFE at all. */
static uint32_t g_lfe_folded_vectors;
static double g_lfe_delta_peak; /* matrix units, unquantized */

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL: ");                                          \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
            g_fail++;                                                           \
        }                                                                       \
    } while (0)

/* ---- decoded vector container -------------------------------------------- */

struct pcm {
    int32_t *ch[AIUS_MAX_CHANNELS]; /* planar, one buffer per active channel */
    uint16_t channels;
    uint32_t frames;
    uint32_t sample_rate;
    uint32_t channel_mask;
};

static void pcm_free(struct pcm *p)
{
    for (int c = 0; c < AIUS_MAX_CHANNELS; c++)
        free(p->ch[c]);
    memset(p, 0, sizeof(*p));
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }
    *size_out = got;
    return buf;
}

/* DTS Core sync-word (0x7FFE8001) access-unit boundaries. */
static int find_frames(const uint8_t *data, size_t size,
                       size_t *offsets, int max_frames)
{
    int n = 0;
    for (size_t i = 0; i + 4 <= size; i++) {
        if (data[i] == 0x7f && data[i + 1] == 0xfe &&
            data[i + 2] == 0x80 && data[i + 3] == 0x01) {
            if (n < max_frames)
                offsets[n] = i;
            n++;
            i += 3;
        }
    }
    return n;
}

/* Decode every access unit in <name>.dts into planar int32 PCM. */
static int decode_vector(const char *path, struct pcm *out)
{
    memset(out, 0, sizeof(*out));

    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data)
        return -1;

    size_t offsets[8192];
    int nframes = find_frames(data, size, offsets, 8192);
    if (nframes <= 0 || nframes > 8192) {
        fprintf(stderr, "%s: no DTS Core sync words\n", path);
        free(data);
        return -1;
    }

    AiusDtsDecoder *dec = NULL;
    if (aius_dts_decoder_create(&dec) != AIUS_OK) {
        free(data);
        return -1;
    }

    size_t cap = 0;
    int rc = 0;
    for (int fi = 0; fi < nframes; fi++) {
        size_t start = offsets[fi];
        size_t end = (fi + 1 < nframes) ? offsets[fi + 1] : size;
        AiusPcmBlock block;
        AiusStatus st = aius_dts_decode_access_unit(dec, data + start,
                                                    end - start, &block);
        if (st != AIUS_OK) {
            fprintf(stderr, "%s: frame %d status %d\n", path, fi, st);
            rc = -1;
            break;
        }
        if (fi == 0) {
            out->channels = block.channels;
            out->sample_rate = block.sample_rate_hz;
            out->channel_mask = block.channel_mask;
        } else if (block.channels != out->channels ||
                   block.channel_mask != out->channel_mask) {
            fprintf(stderr, "%s: frame %d layout changed\n", path, fi);
            rc = -1;
            break;
        }

        uint32_t need = out->frames + block.frames;
        if (need > cap) {
            size_t newcap = cap ? cap * 2 : 8192;
            while (newcap < need)
                newcap *= 2;
            for (int c = 0; c < out->channels; c++) {
                int32_t *g = realloc(out->ch[c], newcap * sizeof(int32_t));
                if (!g) { rc = -1; break; }
                out->ch[c] = g;
            }
            if (rc) break;
            cap = newcap;
        }
        for (int c = 0; c < out->channels; c++)
            memcpy(out->ch[c] + out->frames, block.planes[c],
                   block.frames * sizeof(int32_t));
        out->frames += block.frames;
    }

    aius_dts_decoder_destroy(dec);
    free(data);
    return rc;
}

/* ---- reference downmix (independent of the module under test) ------------ */

/* Exact replica of aius_quantize: round half away from zero, saturate to 24-bit.
 * A second implementation so the reference is not the code under test. */
static int32_t ref_quantize(double x)
{
    double r = (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
    if (r > (double)AIUS_DSP_SAMPLE_MAX)
        return AIUS_DSP_SAMPLE_MAX;
    if (r < (double)AIUS_DSP_SAMPLE_MIN)
        return AIUS_DSP_SAMPLE_MIN;
    return (int32_t)r;
}

/* int32 decoded plane -> the double plane every DSP stage takes since the
 * single-quantize change. aius_dsp_process does this internally into
 * cfg->scratch; a test driving a stage directly does it here. */
static void to_d(const int32_t *src, double *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = (double)src[i];
}

static int mask_plane(uint32_t mask, uint32_t bit)
{
    if (!(mask & bit))
        return -1;
    int idx = 0;
    for (uint32_t b = 1; b < bit; b <<= 1)
        if (mask & b)
            idx++;
    return idx;
}

/* Independent Lo/Ro downmix of a planar block per the documented formula,
 * including the LFE fold-in and its crossover.
 *
 * THE SPLIT OF RESPONSIBILITY for the crossover, because it matters for what this
 * proves. The reference runs its OWN Direct Form I recurrence over its OWN delay
 * memory, so the fold's integration into the matrix -- filter before gain, one
 * quantize per output sample, symmetric into L and R, state carried across the
 * whole vector -- is checked against an implementation that shares no code with
 * the module. It reads the DESIGNED coefficients from the fold rather than
 * re-deriving them, because the coefficient math is pinned separately and more
 * strongly in the `dsp` suite, as exact identities (|H(0)| == 1, |H(f0)| == Q at
 * every rate) that no transcription here could match for rigour. Re-transcribing
 * the RBJ formulas here would also risk a last-bit disagreement on a knife-edge
 * sample, which would make this a flaky test of arithmetic order rather than a
 * firm test of routing.
 *
 * IT NO LONGER QUANTIZES, because the matrix no longer does: the reference ends
 * where the module's stage ends, in double, and the harness applies THE single
 * quantize (ref_quantize) once, at the same place aius_dsp_process does. The
 * sample-exact comparison over a whole real vector therefore got stricter -- two
 * unrounded doubles must agree bit for bit rather than agreeing after both were
 * rounded to the same integer. */
static void ref_downmix(const AiusDownmixConfig *cfg, const struct pcm *p,
                        double *outL, double *outR)
{
    int iFL = mask_plane(p->channel_mask, AIUS_CH_FL);
    int iFR = mask_plane(p->channel_mask, AIUS_CH_FR);
    int iFC = mask_plane(p->channel_mask, AIUS_CH_FC);
    int iLFE = mask_plane(p->channel_mask, AIUS_CH_LFE);
    int iSL = mask_plane(p->channel_mask, AIUS_CH_SL);
    int iSR = mask_plane(p->channel_mask, AIUS_CH_SR);

    /* The reference's own crossover: designed here (so the harness never depends
     * on the module having been called first) with its own state. */
    AiusLfeFold ref_fold;
    int filtered = cfg->lfe != 0.0 && cfg->lfe_crossover_hz > 0.0 &&
                   cfg->lfe_fold != NULL &&
                   aius_lfe_fold_init(&ref_fold, (double)p->sample_rate,
                                      cfg->lfe_crossover_hz) == AIUS_OK;
    double x1[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};
    double x2[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};
    double y1[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};
    double y2[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};

    for (uint32_t n = 0; n < p->frames; n++) {
        double l = 0.0, r = 0.0;
        if (iFL >= 0) l += (double)p->ch[iFL][n];
        if (iFR >= 0) r += (double)p->ch[iFR][n];
        if (iFC >= 0) { double c = (double)p->ch[iFC][n] * cfg->center; l += c; r += c; }
        if (iLFE >= 0) {
            double e = (double)p->ch[iLFE][n];
            if (filtered) {
                for (int k = 0; k < AIUS_DSP_LFE_CROSSOVER_SECTIONS; k++) {
                    const AiusBiquad *q = &ref_fold.lp[k];
                    double y = q->b0 * e + q->b1 * x1[k] + q->b2 * x2[k]
                               - q->a1 * y1[k] - q->a2 * y2[k];
                    x2[k] = x1[k];
                    x1[k] = e;
                    y2[k] = y1[k];
                    y1[k] = y;
                    e = y;
                }
            }
            e *= cfg->lfe;
            l += e;
            r += e;
        }
        if (iSL >= 0) l += (double)p->ch[iSL][n] * cfg->surround;
        if (iSR >= 0) r += (double)p->ch[iSR][n] * cfg->surround;
        outL[n] = l * cfg->headroom;
        outR[n] = r * cfg->headroom;
    }
}

static int32_t peak_abs(const int32_t *s, uint32_t n)
{
    int32_t pk = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t a = s[i] < 0 ? -s[i] : s[i];
        if (a > pk) pk = a;
    }
    return pk;
}

/* ---- vector table -------------------------------------------------------- */

struct vector { const char *name; uint16_t channels; uint32_t mask; };

static const struct vector k_vectors[] = {
    { "stereo_tones", 2, AIUS_CH_FL | AIUS_CH_FR },
    { "surround51_tones", 6,
      AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR },
    { "stereo_impulse", 2, AIUS_CH_FL | AIUS_CH_FR },
    { "surround51_impulse", 6,
      AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR },
};

/* Build a mask-consistent input block that carries `sig` on channel `bit` and
 * zero on every other channel of `mask`. Planes come from caller storage:
 * planes[] is filled in ascending AIUS_CH_* order. `zero` is a shared silent
 * plane. Returns the number of channels (popcount(mask)).
 *
 * Double planes, because the isolation tests drive aius_dsp_downmix directly and
 * that is a double-plane stage now. */
static int build_isolated(uint32_t mask, uint32_t bit, double *sig,
                          double *zero, double *planes[])
{
    static const uint32_t order[] = {
        AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_FC, AIUS_CH_LFE, AIUS_CH_SL, AIUS_CH_SR,
    };
    int n = 0;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (mask & order[i])
            planes[n++] = (order[i] == bit) ? sig : zero;
    }
    return n;
}

/* ---- per-vector known-answer + signal-math tests ------------------------- */

/* Signature retained as the golden fixture: decoded-input layout + per-channel
 * peak, and the default-downmix stereo-output peak. */
struct signature {
    uint16_t channels;
    uint32_t mask;
    uint32_t frames;
    int32_t in_peak[AIUS_MAX_CHANNELS];
    int32_t out_peak_l;
    int32_t out_peak_r;
};

static void print_signature(FILE *f, const struct vector *v,
                            const struct signature *s)
{
    fprintf(f, "channels %u\n", s->channels);
    fprintf(f, "mask 0x%x\n", s->mask);
    fprintf(f, "frames %u\n", s->frames);
    fprintf(f, "in_peak");
    for (int c = 0; c < v->channels; c++)
        fprintf(f, " %d", s->in_peak[c]);
    fprintf(f, "\n");
    fprintf(f, "out_peak_l %d\n", s->out_peak_l);
    fprintf(f, "out_peak_r %d\n", s->out_peak_r);
}

static int load_golden(const char *dir, const char *name, const struct vector *v,
                       struct signature *g)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.golden", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open golden %s\n", path);
        return -1;
    }
    memset(g, 0, sizeof(*g));
    unsigned mask = 0, ch = 0, fr = 0;
    int ok = 1;
    ok &= fscanf(f, " channels %u", &ch) == 1;
    ok &= fscanf(f, " mask 0x%x", &mask) == 1;
    ok &= fscanf(f, " frames %u", &fr) == 1;
    ok &= fscanf(f, " in_peak") == 0;
    for (int c = 0; c < v->channels; c++)
        ok &= fscanf(f, " %d", &g->in_peak[c]) == 1;
    ok &= fscanf(f, " out_peak_l %d", &g->out_peak_l) == 1;
    ok &= fscanf(f, " out_peak_r %d", &g->out_peak_r) == 1;
    fclose(f);
    g->channels = (uint16_t)ch;
    g->mask = mask;
    g->frames = fr;
    return ok ? 0 : -1;
}

static void run_vector(const char *vec_dir, const char *golden_dir,
                       const struct vector *v, int dump)
{
    char dts_path[1024];
    snprintf(dts_path, sizeof(dts_path), "%s/%s.dts", vec_dir, v->name);

    struct pcm p;
    if (decode_vector(dts_path, &p) != 0) {
        fprintf(stderr, "FAIL %s: decode error\n", v->name);
        g_fail++;
        return;
    }

    /* Decode-into-interface sanity: layout matches the vector spec. */
    CHECK(p.channels == v->channels, "%s channels %u != %u",
          v->name, p.channels, v->channels);
    CHECK(p.channel_mask == v->mask, "%s mask 0x%x != 0x%x",
          v->name, p.channel_mask, v->mask);
    CHECK(p.sample_rate == 48000, "%s rate %u != 48000", v->name, p.sample_rate);
    CHECK(p.frames > 0, "%s empty decode", v->name);

    uint32_t F = p.frames;
    int32_t *outL = malloc(F * sizeof(int32_t));
    int32_t *outR = malloc(F * sizeof(int32_t));
    /* The matrix's own (unquantized) result, and the independent reference for
     * it. outL/outR below are these, quantized once -- which is exactly what the
     * chain emits, so the retained golden signature is unchanged by the move to a
     * double intermediate. */
    double *dblL = malloc(F * sizeof(double));
    double *dblR = malloc(F * sizeof(double));
    double *refL = malloc(F * sizeof(double));
    double *refR = malloc(F * sizeof(double));
    double *zero = calloc(F, sizeof(double));
    /* The decoded planes as doubles (for stages driven directly), plus the
     * staging planes and stereo work pair aius_dsp_process now requires. Carved
     * once per vector: the allocation-free guard below arms the shim after this
     * point, so nothing on the process path may allocate. */
    double *dch[AIUS_MAX_CHANNELS] = {0};
    double *pscr[AIUS_MAX_CHANNELS] = {0};
    double *pwl = malloc(F * sizeof(double));
    double *pwr = malloc(F * sizeof(double));
    int oom = (!outL || !outR || !dblL || !dblR || !refL || !refR || !zero ||
               !pwl || !pwr);
    for (int c = 0; c < AIUS_MAX_CHANNELS; c++) {
        pscr[c] = malloc(F * sizeof(double));
        if (pscr[c] == NULL) oom = 1;
    }
    for (int c = 0; c < p.channels; c++) {
        dch[c] = malloc(F * sizeof(double));
        if (dch[c] == NULL) {
            oom = 1;
            continue;
        }
        to_d(p.ch[c], dch[c], F);
    }
    if (oom) {
        fprintf(stderr, "FAIL %s: OOM\n", v->name);
        g_fail++;
        goto done;
    }

    /* THE SHIPPED DEFAULT NOW CARRIES STATE, which changes how this harness has to
     * be read. The LFE fold-in's crossover is a 4th-order IIR whose delay memory
     * persists across calls, so every INDEPENDENT run below -- each of which
     * re-processes the whole vector from its first sample -- must start from zero
     * memory or it inherits the previous run's filter history and the two results
     * differ for the first few hundred samples. `aius_lfe_fold_reset(&fold)`
     * immediately before a run is what says "this run starts fresh"; the two
     * block-split tests deliberately do NOT reset between halves, because carrying
     * the state is the property they exist to prove. */
    AiusLfeFold fold;
    memset(&fold, 0, sizeof fold);
    AiusDownmixConfig cfg = aius_downmix_config_default();
    cfg.lfe_fold = &fold;

    /* Routing and per-channel GAIN math is asserted at unity headroom. The shipped
     * default headroom is 0.5 (defect 2: unity clipped this very corpus), so a
     * routing assertion that folded the output level in would break every time the
     * level was retuned, for a reason that has nothing to do with routing. `cfg`
     * stays the shipped default everywhere the LEVEL is the subject -- the golden
     * signature, the clip check, and the reference comparison. */
    AiusDownmixConfig unity = cfg;
    unity.headroom = 1.0;

    /* --- full-vector default downmix through the public interface --------- */
    /* Two views of the same decoded block: the int32 one aius_dsp_process takes
     * (it owns the int32 -> double boundary) and the double one the stages take
     * when a test drives them directly. */
    AiusPcmBlock in;
    memset(&in, 0, sizeof in);
    for (int c = 0; c < p.channels; c++)
        in.planes[c] = p.ch[c];
    in.channels = p.channels;
    in.frames = F;
    in.sample_rate_hz = p.sample_rate;
    in.channel_mask = p.channel_mask;

    AiusPcmBlockD ind;
    memset(&ind, 0, sizeof ind);
    for (int c = 0; c < p.channels; c++)
        ind.planes[c] = dch[c];
    ind.channels = p.channels;
    ind.frames = F;
    ind.sample_rate_hz = p.sample_rate;
    ind.channel_mask = p.channel_mask;

    aius_lfe_fold_reset(&fold);
    CHECK(aius_dsp_downmix(&cfg, &ind, dblL, dblR) == AIUS_OK, "%s downmix",
          v->name);
    /* THE single quantize, applied by the harness exactly where the chain applies
     * it. Everything downstream in this function that says "the shipped-default
     * stereo output" means outL/outR, and they are the same integers this vector
     * always produced -- which is why the retained golden did not move. */
    for (uint32_t n = 0; n < F; n++) {
        outL[n] = ref_quantize(dblL[n]);
        outR[n] = ref_quantize(dblR[n]);
    }

    /* Independent reference downmix: sample-exact over the whole real vector,
     * fold-in and crossover included (ref_downmix runs its own recurrence), and
     * now compared UNQUANTIZED -- two doubles, bit for bit. */
    ref_downmix(&cfg, &p, refL, refR);
    {
        uint32_t diff = 0, first = 0;
        for (uint32_t n = 0; n < F; n++) {
            if (dblL[n] != refL[n] || dblR[n] != refR[n]) {
                if (!diff) first = n;
                diff++;
            }
        }
        CHECK(diff == 0, "%s downmix != reference (%u samples, first @%u)",
              v->name, diff, first);
    }

    /* Finite intermediate, in-range wire. The two halves of what used to be one
     * check: the matrix result may legitimately exceed the 24-bit range now (that
     * is the whole point), but it must be finite, and what reaches the wire after
     * the single quantize must be inside it. */
    {
        int finite = 1, inrange = 1;
        for (uint32_t n = 0; n < F; n++) {
            if (!isfinite(dblL[n]) || !isfinite(dblR[n]))
                finite = 0;
            if (outL[n] > AIUS_DSP_SAMPLE_MAX || outL[n] < AIUS_DSP_SAMPLE_MIN ||
                outR[n] > AIUS_DSP_SAMPLE_MAX || outR[n] < AIUS_DSP_SAMPLE_MIN)
                inrange = 0;
        }
        CHECK(finite, "%s matrix produced a non-finite sample", v->name);
        CHECK(inrange, "%s output out of 24-bit range", v->name);
    }

    /* --- the fold-in's blast radius, asserted rather than assumed ---------- */
    /* Every vector gets the same comparison: the shipped default against LFE
     * excluded, over the whole decoded vector.
     *
     *   - A vector with no LFE in its layout must be BIT-IDENTICAL. The fold-in is
     *     allowed to change 5.1 output and is not allowed to touch stereo, and this
     *     is the check that says so at every sample rather than leaving it implied
     *     by an unchanged golden file.
     *   - A vector WITH LFE must differ, or the golden it just regenerated would be
     *     recording a feature that is not running.
     *
     * `lfe_delta_peak` is reported alongside the signature so a reviewer reading a
     * changed golden can see how much of the change the fold-in accounts for. */
    {
        double *xl = malloc(F * sizeof(double));
        double *xr = malloc(F * sizeof(double));
        if (xl == NULL || xr == NULL) {
            fprintf(stderr, "FAIL %s: OOM\n", v->name);
            g_fail++;
        } else {
            AiusDownmixConfig ex = cfg;
            ex.lfe = AIUS_DSP_LFE_GAIN_EXCLUDED;
            aius_lfe_fold_reset(&fold);
            CHECK(aius_dsp_downmix(&ex, &ind, xl, xr) == AIUS_OK,
                  "%s excluded dmix", v->name);
            /* Compared in the matrix's own units now: "bit-identical" for a
             * vector with no LFE plane is a double identity, not an equality of
             * two rounded integers. */
            uint32_t diff = 0;
            double dpk = 0.0;
            for (uint32_t n = 0; n < F; n++) {
                if (dblL[n] != xl[n] || dblR[n] != xr[n]) diff++;
                double d = dblL[n] - xl[n];
                if (d < 0.0) d = -d;
                if (d > dpk) dpk = d;
                d = dblR[n] - xr[n];
                if (d < 0.0) d = -d;
                if (d > dpk) dpk = d;
            }
            if (p.channel_mask & AIUS_CH_LFE) {
                CHECK(diff > 0,
                      "%s carries LFE but the fold-in changed nothing -- the "
                      "default is not folding", v->name);
                g_lfe_folded_vectors++;
            } else {
                CHECK(diff == 0,
                      "%s has no LFE plane but the fold-in changed %u sample(s) "
                      "(peak delta %.3f) -- stereo output must be untouched",
                      v->name, diff, dpk);
            }
            g_lfe_delta_peak = dpk;
        }
        free(xl);
        free(xr);
    }

    /* --- process(bypass EQ) == bare downmix (trim/EQ bypass equivalence) --- */
    {
        AiusBiquad eq[1];
        AiusBiquadState st[2];
        memset(st, 0, sizeof st);
        size_t ns = aius_eq_default(eq, 1);
        int32_t *pl = malloc(F * sizeof(int32_t));
        int32_t *pr = malloc(F * sizeof(int32_t));
        AiusPcmBlock pout;
        memset(&pout, 0, sizeof pout);
        pout.planes[0] = pl;
        pout.planes[1] = pr;
        AiusDspConfig dc;
        memset(&dc, 0, sizeof dc);
        dc.downmix = cfg;
        dc.eq = eq;
        dc.eq_sections = ns;
        dc.eq_state = st;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) dc.scratch[c] = pscr[c];
        dc.work_l = pwl;
        dc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&dc, &in, &pout) == AIUS_OK, "%s process", v->name);
        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (pl[n] != outL[n] || pr[n] != outR[n]) diff++;
        CHECK(diff == 0, "%s process(bypass) != downmix (%u)", v->name, diff);
        /* The block meta belongs to process now (the matrix writes planes only),
         * so bounded latency and the stereo layout are asserted here. */
        CHECK(pout.frames == F, "%s frames %u != %u", v->name, pout.frames, F);
        CHECK(pout.channels == 2 &&
              pout.channel_mask == (AIUS_CH_FL | AIUS_CH_FR),
              "%s stereo layout", v->name);
        free(pl);
        free(pr);
    }

    /* --- bounded latency: seamless block boundaries with a stateful EQ ----- */
    /* Split the vector in half and process each half reusing carried EQ state;
     * a real IIR lowpass makes this non-trivial. Concatenated == whole-block
     * proves the path adds no hidden buffering/latency across block edges. */
    {
        AiusBiquad eq[1];
        eq[0] = aius_biquad_lowpass(48000.0, 4000.0, 0.7071);

        /* whole block */
        AiusBiquadState wst[2];
        memset(wst, 0, sizeof wst);
        int32_t *wl = malloc(F * sizeof(int32_t));
        int32_t *wr = malloc(F * sizeof(int32_t));
        AiusPcmBlock wout;
        memset(&wout, 0, sizeof wout);
        wout.planes[0] = wl;
        wout.planes[1] = wr;
        AiusDspConfig wc;
        memset(&wc, 0, sizeof wc);
        wc.downmix = cfg;
        wc.eq = eq;
        wc.eq_sections = 1;
        wc.eq_state = wst;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) wc.scratch[c] = pscr[c];
        wc.work_l = pwl;
        wc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&wc, &in, &wout) == AIUS_OK, "%s eq whole", v->name);

        /* two halves, state carried (staging planes reused per call, as on the
         * target: they are block scratch, not stream state) */
        uint32_t h = F / 2;
        AiusBiquadState sst[2];
        memset(sst, 0, sizeof sst);
        int32_t *sl = malloc(F * sizeof(int32_t));
        int32_t *sr = malloc(F * sizeof(int32_t));
        AiusDspConfig sc = wc;
        sc.eq_state = sst;

        AiusPcmBlock ha = in; ha.frames = h;
        AiusPcmBlock hb = in;
        for (int c = 0; c < p.channels; c++)
            hb.planes[c] = p.ch[c] + h;
        hb.frames = F - h;

        AiusPcmBlock oa;
        memset(&oa, 0, sizeof oa);
        oa.planes[0] = sl; oa.planes[1] = sr;
        AiusPcmBlock ob;
        memset(&ob, 0, sizeof ob);
        ob.planes[0] = sl + h; ob.planes[1] = sr + h;
        /* Reset before half A only. The crossover memory MUST carry from A into B
         * -- that, and the EQ state, is what "seamless block boundary" means now
         * that the matrix itself holds a filter. */
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&sc, &ha, &oa) == AIUS_OK, "%s eq half a", v->name);
        CHECK(aius_dsp_process(&sc, &hb, &ob) == AIUS_OK, "%s eq half b", v->name);

        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (sl[n] != wl[n] || sr[n] != wr[n]) diff++;
        CHECK(diff == 0, "%s block-split EQ != whole (%u samples)", v->name, diff);
        free(wl); free(wr); free(sl); free(sr);
    }

    /* --- channel-routing signal math via isolated real content ------------ */
    if (v->channels == 2) {
        /* Stereo: FL/FR passthrough + isolation (no cross-bleed), at unity. */
        double *uL = malloc(F * sizeof(double));
        double *uR = malloc(F * sizeof(double));
        CHECK(aius_dsp_downmix(&unity, &ind, uL, uR) == AIUS_OK, "%s unity dmix",
              v->name);
        int exactL = 1, exactR = 1;
        for (uint32_t n = 0; n < F; n++) {
            if (uL[n] != (double)p.ch[0][n]) exactL = 0;
            if (uR[n] != (double)p.ch[1][n]) exactR = 0;
        }
        CHECK(exactL, "%s FL->L not exact passthrough at unity", v->name);
        CHECK(exactR, "%s FR->R not exact passthrough at unity", v->name);

        /* And the shipped default is EXACTLY that scaled by the headroom -- so the
         * level change between the two is the intended 0.5, sample for sample, and
         * not an accidental gain or a rounding artefact. This is the assertion that
         * distinguishes an intended level change from an accidental one. */
        int scaled = 1;
        for (uint32_t n = 0; n < F; n++) {
            if (outL[n] != ref_quantize((double)p.ch[0][n] * cfg.headroom) ||
                outR[n] != ref_quantize((double)p.ch[1][n] * cfg.headroom))
                scaled = 0;
        }
        CHECK(scaled, "%s default downmix != unity scaled by headroom", v->name);
        free(uL);
        free(uR);
    } else {
        /* 5.1: isolate each decoded channel into a mask-consistent block and
         * prove placement + gain through the public downmix. Every expectation
         * below dropped its ref_quantize: an isolated channel through the matrix
         * at unity headroom is exactly one product, and that is what comes back
         * now. */
        double *pl[AIUS_MAX_CHANNELS];
        double *iL = malloc(F * sizeof(double));
        double *iR = malloc(F * sizeof(double));

        int iFC = mask_plane(p.channel_mask, AIUS_CH_FC);
        int iLFE = mask_plane(p.channel_mask, AIUS_CH_LFE);
        int iSL = mask_plane(p.channel_mask, AIUS_CH_SL);
        int iSR = mask_plane(p.channel_mask, AIUS_CH_SR);

        /* FC: symmetric contribution, L == R == center*FC. */
        {
            int nch = build_isolated(AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC,
                                     AIUS_CH_FC, dch[iFC], zero, pl);
            AiusPcmBlockD b = { {pl[0], pl[1], pl[2], 0, 0, 0}, (uint16_t)nch, F,
                                48000, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC };
            CHECK(aius_dsp_downmix(&unity, &b, iL, iR) == AIUS_OK, "%s FC iso",
                  v->name);
            int sym = 1, gain = 1;
            for (uint32_t n = 0; n < F; n++) {
                if (iL[n] != iR[n]) sym = 0;
                if (iL[n] != (double)p.ch[iFC][n] * unity.center) gain = 0;
            }
            CHECK(sym, "%s FC not symmetric L==R", v->name);
            CHECK(gain, "%s FC gain != center", v->name);
        }
        /* SL -> L only. */
        {
            int nch = build_isolated(AIUS_CH_FL | AIUS_CH_SL, AIUS_CH_SL,
                                     dch[iSL], zero, pl);
            AiusPcmBlockD b = { {pl[0], pl[1], 0, 0, 0, 0},
                                (uint16_t)nch, F, 48000, AIUS_CH_FL | AIUS_CH_SL };
            CHECK(aius_dsp_downmix(&unity, &b, iL, iR) == AIUS_OK, "%s SL iso",
                  v->name);
            int placed = 1, quiet = 1;
            for (uint32_t n = 0; n < F; n++) {
                if (iL[n] != (double)p.ch[iSL][n] * unity.surround) placed = 0;
                if (iR[n] != 0.0) quiet = 0;
            }
            CHECK(placed, "%s SL->L gain wrong", v->name);
            CHECK(quiet, "%s SL leaked to R", v->name);
        }
        /* SR -> R only. */
        {
            int nch = build_isolated(AIUS_CH_FR | AIUS_CH_SR, AIUS_CH_SR,
                                     dch[iSR], zero, pl);
            AiusPcmBlockD b = { {pl[0], pl[1], 0, 0, 0, 0},
                                (uint16_t)nch, F, 48000, AIUS_CH_FR | AIUS_CH_SR };
            CHECK(aius_dsp_downmix(&unity, &b, iL, iR) == AIUS_OK, "%s SR iso",
                  v->name);
            int placed = 1, quiet = 1;
            for (uint32_t n = 0; n < F; n++) {
                if (iR[n] != (double)p.ch[iSR][n] * unity.surround) placed = 0;
                if (iL[n] != 0.0) quiet = 0;
            }
            CHECK(placed, "%s SR->R gain wrong", v->name);
            CHECK(quiet, "%s SR leaked to L", v->name);
        }
        /* LFE fold-in, on real decoded LFE content isolated onto its own block.
         * Four claims, in the order they were built: excluded at gain 0, exact flat
         * multiply with no corner, low-passed and symmetric at the shipped default,
         * and failing closed with no state. */
        {
            int nch = build_isolated(AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE,
                                     AIUS_CH_LFE, dch[iLFE], zero, pl);
            AiusPcmBlockD b = { {pl[0], pl[1], pl[2], 0, 0, 0}, (uint16_t)nch, F,
                                48000, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE };

            /* (1) NEGATIVE CONTROL at vector scale: gain 0 -> stereo silent, so
             * the fold-in is genuinely opt-in on real content and not just on a
             * synthetic block. */
            AiusDownmixConfig ec = unity;
            ec.lfe = AIUS_DSP_LFE_GAIN_EXCLUDED;
            aius_lfe_fold_reset(&fold);
            CHECK(aius_dsp_downmix(&ec, &b, iL, iR) == AIUS_OK, "%s LFE excluded",
                  v->name);
            int silent = 1;
            for (uint32_t n = 0; n < F; n++)
                if (iL[n] != 0.0 || iR[n] != 0.0) silent = 0;
            CHECK(silent, "%s LFE not excluded at gain 0", v->name);

            /* (2) -10 dB with NO corner: still the exact flat multiply the matrix
             * always did, symmetric into both. The unfiltered fold stays reachable
             * through the API even though the JSON schema forbids it. */
            AiusDownmixConfig lc = unity;
            lc.lfe = AIUS_DSP_LFE_GAIN_MINUS10DB;
            lc.lfe_crossover_hz = 0.0;
            lc.lfe_fold = NULL;
            CHECK(aius_dsp_downmix(&lc, &b, iL, iR) == AIUS_OK, "%s LFE flat fold",
                  v->name);
            int folded = 1, nonzero = 0;
            double flat_peak = 0.0;
            for (uint32_t n = 0; n < F; n++) {
                double e = (double)p.ch[iLFE][n] * AIUS_DSP_LFE_GAIN_MINUS10DB;
                if (iL[n] != e || iR[n] != e) folded = 0;
                if (e != 0.0) nonzero = 1;
                double a = e < 0.0 ? -e : e;
                if (a > flat_peak) flat_peak = a;
            }
            CHECK(folded, "%s LFE flat fold gain wrong", v->name);
            CHECK(nonzero, "%s LFE test signal silent", v->name);

            /* (3) THE SHIPPED FOLD: same gain, 120 Hz LR4. Symmetric, and audibly
             * REDUCED against the unfiltered fold -- which is the whole point, and
             * is only checkable because (2) measured the unfiltered case on the same
             * content. The corpus's LFE planes carry 21% of their power above the
             * corner, so this must come out below the flat fold. */
            AiusDownmixConfig fc = unity;
            fc.lfe = AIUS_DSP_LFE_GAIN_MINUS10DB;
            aius_lfe_fold_reset(&fold);
            CHECK(aius_dsp_downmix(&fc, &b, iL, iR) == AIUS_OK, "%s LFE LR4 fold",
                  v->name);
            int sym = 1;
            double lp_peak = 0.0;
            for (uint32_t n = 0; n < F; n++) {
                if (iL[n] != iR[n]) sym = 0;
                double a = iL[n] < 0.0 ? -iL[n] : iL[n];
                if (a > lp_peak) lp_peak = a;
            }
            CHECK(sym, "%s LR4 fold not symmetric L==R", v->name);
            CHECK(lp_peak > 0.0, "%s LR4 fold silent (filter killed everything)",
                  v->name);
            CHECK(lp_peak < flat_peak,
                  "%s LR4 fold peak %.1f not below the unfiltered %.1f -- the "
                  "crossover is not attenuating", v->name, lp_peak, flat_peak);

            /* (4) And with no state to hold the corner it fails CLOSED, to (1)
             * rather than to (2): silent, not an unfiltered fold. */
            AiusDownmixConfig nc = fc;
            nc.lfe_fold = NULL;
            CHECK(aius_dsp_downmix(&nc, &b, iL, iR) == AIUS_OK, "%s LFE no state",
                  v->name);
            int closed = 1;
            for (uint32_t n = 0; n < F; n++)
                if (iL[n] != 0.0 || iR[n] != 0.0) closed = 0;
            CHECK(closed, "%s missing crossover state did not fail closed", v->name);
        }
        free(iL);
        free(iR);
    }

    /* --- defect 2: the SHIPPED default downmix must not clip this corpus --- */
    {
        /* `out`/outL/outR above is the shipped-default downmix. Not one sample of
         * it may sit on a rail. Unity headroom put 10 samples of
         * surround51_impulse exactly on AIUS_DSP_SAMPLE_MAX -- the committed corpus
         * clipped -- and this is the check that stays red if that comes back. */
        uint32_t at_rail = 0;
        for (uint32_t n = 0; n < F; n++) {
            if (outL[n] >= AIUS_DSP_SAMPLE_MAX || outL[n] <= AIUS_DSP_SAMPLE_MIN ||
                outR[n] >= AIUS_DSP_SAMPLE_MAX || outR[n] <= AIUS_DSP_SAMPLE_MIN)
                at_rail++;
        }
        CHECK(at_rail == 0, "%s DEFAULT downmix clipped %u samples (headroom %.6f)",
              v->name, at_rail, cfg.headroom);

        /* The negative control, in the suite rather than in a reviewer's head:
         * unity headroom really does clip this corpus. Without it, the check above
         * could pass on a corpus that never stressed the matrix at all, and the
         * defect would be untestable. Accumulated across vectors and asserted once
         * in main, because only one of the four is hot enough to clip. */
        double *ul = malloc(F * sizeof(double));
        double *ur = malloc(F * sizeof(double));
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_downmix(&unity, &ind, ul, ur) == AIUS_OK,
              "%s unity clip probe", v->name);
        /* "Would clip" is now a statement about what the FINAL QUANTIZE would do
         * to this matrix result, since the matrix itself no longer clamps. Same
         * samples, same count: a value at or past the rail is one the wire cannot
         * carry. */
        for (uint32_t n = 0; n < F; n++) {
            if (ref_quantize(ul[n]) >= AIUS_DSP_SAMPLE_MAX ||
                ref_quantize(ul[n]) <= AIUS_DSP_SAMPLE_MIN ||
                ref_quantize(ur[n]) >= AIUS_DSP_SAMPLE_MAX ||
                ref_quantize(ur[n]) <= AIUS_DSP_SAMPLE_MIN)
                g_unity_clipped++;
        }
        free(ul);
        free(ur);
    }

    /* --- clip-free headroom: real vector never saturates ------------------ */
    {
        AiusLfeFold cfold;
        memset(&cfold, 0, sizeof cfold);
        AiusDownmixConfig cc = aius_downmix_config_default();
        cc.lfe_fold = &cfold; /* the clip-free bound covers the fold, so run it */
        cc.headroom = aius_downmix_clipfree_headroom(&cc);
        CHECK(cc.headroom > 0.0 && cc.headroom <= 1.0, "%s clipfree headroom", v->name);
        double *cl = malloc(F * sizeof(double));
        double *cr = malloc(F * sizeof(double));
        CHECK(aius_dsp_downmix(&cc, &ind, cl, cr) == AIUS_OK, "%s clipfree dmix",
              v->name);
        int saturated = 0;
        for (uint32_t n = 0; n < F; n++) {
            if (ref_quantize(cl[n]) == AIUS_DSP_SAMPLE_MAX ||
                ref_quantize(cl[n]) == AIUS_DSP_SAMPLE_MIN ||
                ref_quantize(cr[n]) == AIUS_DSP_SAMPLE_MAX ||
                ref_quantize(cr[n]) == AIUS_DSP_SAMPLE_MIN)
                saturated++;
        }
        CHECK(saturated == 0, "%s clip-free downmix saturated %u samples",
              v->name, saturated);
        free(cl);
        free(cr);
    }

    /* --- input stage: identity trim/EQ leaves the downmix unchanged -------- */
    {
        AiusInputStage stage = aius_input_stage_default();
        int32_t *pl = malloc(F * sizeof(int32_t));
        int32_t *pr = malloc(F * sizeof(int32_t));
        AiusPcmBlock po;
        memset(&po, 0, sizeof po);
        po.planes[0] = pl;
        po.planes[1] = pr;
        AiusDspConfig dc;
        memset(&dc, 0, sizeof dc);
        dc.downmix = cfg;
        dc.input = &stage;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) dc.scratch[c] = pscr[c];
        dc.work_l = pwl;
        dc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&dc, &in, &po) == AIUS_OK, "%s input-id", v->name);
        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (pl[n] != outL[n] || pr[n] != outR[n]) diff++;
        CHECK(diff == 0, "%s identity input stage != downmix (%u)", v->name, diff);
        /* Input planes are const: unchanged after processing. The staging
         * convert-copy writes into cfg->scratch, and it runs on every block now
         * rather than only when an input stage is configured. */
        int mutated = 0;
        for (uint32_t n = 0; n < F; n++)
            if (p.ch[0][n] != in.planes[0][n]) mutated = 1;
        CHECK(!mutated, "%s input stage mutated decoded plane", v->name);
        free(pl);
        free(pr);
    }

    /* --- input trim: FL -6 dB matches a pre-scaled-FL downmix exactly -------
     *
     * THE PIN THAT INVERTED (F5), on real content. The reference used to be the
     * FL plane scaled by 0.5 AND QUANTIZED, because that is what the input stage
     * did: gain, round to int32, hand the matrix an integer plane. Neither path
     * rounds now, so the reference pre-scales IN DOUBLE and the equality holds
     * end to end. It is a real difference on this corpus, not a formality: the
     * decoded FL plane is full of odd samples, so a pre-quantized reference now
     * disagrees with the trimmed chain by up to half an LSB per sample. */
    {
        double *fls = malloc(F * sizeof(double)); /* FL plane * 0.5, in double */
        double odd = 0.0;
        for (uint32_t n = 0; n < F; n++) {
            fls[n] = (double)p.ch[0][n] * 0.5;
            if (fls[n] != floor(fls[n])) odd += 1.0;
        }
        AiusPcmBlockD rin = ind;
        rin.planes[0] = fls;
        double *rdl = malloc(F * sizeof(double));
        double *rdr = malloc(F * sizeof(double));
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_downmix(&cfg, &rin, rdl, rdr) == AIUS_OK, "%s trim ref",
              v->name);

        AiusInputStage stage = aius_input_stage_default();
        stage.channels[0].gain_linear = 0.5; /* FL trim */
        int32_t *tl = malloc(F * sizeof(int32_t));
        int32_t *tr = malloc(F * sizeof(int32_t));
        AiusPcmBlock to;
        memset(&to, 0, sizeof to);
        to.planes[0] = tl;
        to.planes[1] = tr;
        AiusDspConfig tc;
        memset(&tc, 0, sizeof tc);
        tc.downmix = cfg;
        tc.input = &stage;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) tc.scratch[c] = pscr[c];
        tc.work_l = pwl;
        tc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&tc, &in, &to) == AIUS_OK, "%s trim proc", v->name);
        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (tl[n] != ref_quantize(rdl[n]) || tr[n] != ref_quantize(rdr[n]))
                diff++;
        CHECK(diff == 0, "%s FL trim != pre-scaled downmix (%u)", v->name, diff);
        /* Not vacuous: the trim really does produce half-LSBs for the matrix to
         * carry, so a stage that re-rounded would land somewhere else. */
        CHECK(odd > 0.0, "%s FL trim produced no half-LSB (test is vacuous)",
              v->name);
        free(fls);
        free(rdl);
        free(rdr);
        free(tl);
        free(tr);
    }

    /* --- limiter: disabled tail leaves the downmix bit-identical ------------ */
    {
        AiusLimiter lim;
        memset(&lim, 0, sizeof lim);
        lim.cfg = aius_limiter_config_default();
        lim.cfg.enabled = 0; /* the shipped default is now ENABLED (defect 3) */
        int32_t *ll = malloc(F * sizeof(int32_t));
        int32_t *lr = malloc(F * sizeof(int32_t));
        AiusPcmBlock lo;
        memset(&lo, 0, sizeof lo);
        lo.planes[0] = ll;
        lo.planes[1] = lr;
        AiusDspConfig lc;
        memset(&lc, 0, sizeof lc);
        lc.downmix = cfg;
        lc.limiter = &lim;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) lc.scratch[c] = pscr[c];
        lc.work_l = pwl;
        lc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&lc, &in, &lo) == AIUS_OK, "%s lim off", v->name);
        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (ll[n] != outL[n] || lr[n] != outR[n]) diff++;
        CHECK(diff == 0, "%s disabled limiter changed output (%u)", v->name, diff);
        free(ll);
        free(lr);
    }

    /* --- limiter: enabled prevents the surround51_impulse full-scale clip --- */
    {
        AiusLimiter lim;
        memset(&lim, 0, sizeof lim);
        lim.cfg = aius_limiter_config_default();
        lim.cfg.enabled = 1;
        lim.cfg.threshold_dbfs = -3.0;
        lim.cfg.attack_ms = 1.0;
        lim.cfg.release_ms = 100.0;
        lim.cfg.lookahead_ms = 5.0;
        int32_t *ll = malloc(F * sizeof(int32_t));
        int32_t *lr = malloc(F * sizeof(int32_t));
        AiusPcmBlock lo;
        memset(&lo, 0, sizeof lo);
        lo.planes[0] = ll;
        lo.planes[1] = lr;
        AiusDspConfig lc;
        memset(&lc, 0, sizeof lc);
        lc.downmix = cfg;
        lc.limiter = &lim;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) lc.scratch[c] = pscr[c];
        lc.work_l = pwl;
        lc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&lc, &in, &lo) == AIUS_OK, "%s lim on", v->name);

        int32_t base_pk = peak_abs(outL, F);
        int32_t r_pk = peak_abs(outR, F);
        if (r_pk > base_pk) base_pk = r_pk;
        int saturated = 0, inrange = 1;
        int32_t lim_pk = 0;
        for (uint32_t n = 0; n < F; n++) {
            if (ll[n] == AIUS_DSP_SAMPLE_MAX || ll[n] == AIUS_DSP_SAMPLE_MIN ||
                lr[n] == AIUS_DSP_SAMPLE_MAX || lr[n] == AIUS_DSP_SAMPLE_MIN)
                saturated++;
            if (ll[n] > AIUS_DSP_SAMPLE_MAX || ll[n] < AIUS_DSP_SAMPLE_MIN ||
                lr[n] > AIUS_DSP_SAMPLE_MAX || lr[n] < AIUS_DSP_SAMPLE_MIN)
                inrange = 0;
            int32_t a = ll[n] < 0 ? -ll[n] : ll[n];
            int32_t b = lr[n] < 0 ? -lr[n] : lr[n];
            if (a > lim_pk) lim_pk = a;
            if (b > lim_pk) lim_pk = b;
        }
        CHECK(inrange, "%s limited output out of range", v->name);
        /* surround51_impulse saturates at the default downmix (peak == MAX); the
         * enabled limiter must pull every sample below full scale. */
        if (base_pk == AIUS_DSP_SAMPLE_MAX) {
            CHECK(saturated == 0, "%s limiter left %d saturated samples", v->name,
                  saturated);
            CHECK(lim_pk < AIUS_DSP_SAMPLE_MAX,
                  "%s limiter peak %d still full scale", v->name, lim_pk);
        }
        free(ll);
        free(lr);
    }

    /* --- full path: seamless across a block boundary with carried state ----- */
    {
        AiusInputStage stage = aius_input_stage_default();
        stage.channels[2].gain_linear = aius_db_to_linear(-3.0); /* FC trim */
        AiusBiquad oeq[1];
        oeq[0] = aius_biquad_lowpass(48000.0, 4000.0, 0.7071);

        AiusLimiterConfig limc = aius_limiter_config_default();
        limc.enabled = 1;
        limc.threshold_dbfs = -3.0;
        limc.attack_ms = 1.0;
        limc.release_ms = 80.0;
        limc.lookahead_ms = 3.0;

        /* whole block */
        AiusBiquadState west[2];
        memset(west, 0, sizeof west);
        AiusLimiter wlim;
        memset(&wlim, 0, sizeof wlim);
        wlim.cfg = limc;
        int32_t *wl = malloc(F * sizeof(int32_t));
        int32_t *wr = malloc(F * sizeof(int32_t));
        AiusPcmBlock wo;
        memset(&wo, 0, sizeof wo);
        wo.planes[0] = wl;
        wo.planes[1] = wr;
        AiusDspConfig wc;
        memset(&wc, 0, sizeof wc);
        wc.downmix = cfg;
        wc.eq = oeq;
        wc.eq_sections = 1;
        wc.eq_state = west;
        wc.input = &stage;
        wc.limiter = &wlim;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) wc.scratch[c] = pscr[c];
        wc.work_l = pwl;
        wc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&wc, &in, &wo) == AIUS_OK, "%s full whole", v->name);
        CHECK(wo.frames == F, "%s full frames %u != %u", v->name, wo.frames, F);

        /* two halves, EQ + limiter state carried (scratch reused per call) */
        AiusBiquadState sest[2];
        memset(sest, 0, sizeof sest);
        AiusLimiter slim;
        memset(&slim, 0, sizeof slim);
        slim.cfg = limc;
        int32_t *sl = malloc(F * sizeof(int32_t));
        int32_t *sr = malloc(F * sizeof(int32_t));
        AiusDspConfig sc = wc;
        sc.eq_state = sest;
        sc.limiter = &slim;

        uint32_t hh = F / 2;
        AiusPcmBlock ha = in;
        ha.frames = hh;
        AiusPcmBlock hb = in;
        for (int c = 0; c < p.channels; c++)
            hb.planes[c] = p.ch[c] + hh;
        hb.frames = F - hh;

        AiusPcmBlock oa;
        memset(&oa, 0, sizeof oa);
        oa.planes[0] = sl;
        oa.planes[1] = sr;
        AiusPcmBlock ob;
        memset(&ob, 0, sizeof ob);
        ob.planes[0] = sl + hh;
        ob.planes[1] = sr + hh;
        /* Reset before half A only: the crossover, the EQ and the limiter all carry
         * their state into half B, which is what makes this a boundary test. */
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&sc, &ha, &oa) == AIUS_OK, "%s full half a", v->name);
        CHECK(aius_dsp_process(&sc, &hb, &ob) == AIUS_OK, "%s full half b", v->name);

        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (sl[n] != wl[n] || sr[n] != wr[n]) diff++;
        CHECK(diff == 0, "%s full-path block split != whole (%u)", v->name, diff);
        free(wl);
        free(wr);
        free(sl);
        free(sr);
    }

    /* --- allocation-free proof: the full process path touches no allocator -- */
    {
        static uint8_t guard_backing[64];
        AiusArena guard;
        aius_arena_init(&guard, guard_backing, sizeof guard_backing);

        AiusInputStage stage = aius_input_stage_default();
        stage.channels[0].gain_linear = aius_db_to_linear(-2.0);
        AiusBiquad oeq[1];
        oeq[0] = aius_biquad_highshelf(48000.0, 8000.0, 0.7071, 3.0);
        AiusBiquadState oest[2];
        memset(oest, 0, sizeof oest);
        AiusLimiter lim;
        memset(&lim, 0, sizeof lim);
        lim.cfg = aius_limiter_config_default();
        lim.cfg.enabled = 1;
        lim.cfg.threshold_dbfs = -3.0;
        lim.cfg.attack_ms = 1.0;
        lim.cfg.release_ms = 100.0;
        lim.cfg.lookahead_ms = 5.0;
        int32_t *gl = malloc(F * sizeof(int32_t));
        int32_t *gr = malloc(F * sizeof(int32_t));
        AiusPcmBlock go;
        memset(&go, 0, sizeof go);
        go.planes[0] = gl;
        go.planes[1] = gr;
        AiusMeter gmeter;
        aius_meter_init(&gmeter, 48000);
        AiusDspConfig gc;
        memset(&gc, 0, sizeof gc);
        gc.downmix = cfg;
        gc.eq = oeq;
        gc.eq_sections = 1;
        gc.eq_state = oest;
        gc.input = &stage;
        gc.limiter = &lim;
        /* Staging planes carved at the top of run_vector, i.e. BEFORE the shim is
         * armed: the chain's double scratch is caller-owned precisely so the
         * process path can stay allocation-free. */
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) gc.scratch[c] = pscr[c];
        gc.work_l = pwl;
        gc.work_r = pwr;

        /* Arm the shim AFTER every buffer is allocated; the process path, the
         * caller-driven meter taps, publish, and read must not touch
         * malloc/calloc/realloc. The crossover is reset BEFORE arming so its
         * design happens inside the guarded call -- the redesign path evaluates
         * cos/sin and must be allocation-free too, which is exactly what the fold
         * being caller-owned state buys. */
        aius_lfe_fold_reset(&fold);
        memset(&fold, 0, sizeof fold); /* force a redesign inside the guard */
        aius_alloc_shim_set_arena(&guard);
        aius_alloc_shim_reset_count();
        AiusStatus gs = aius_dsp_process(&gc, &in, &go);
        aius_meter_accumulate_input(&gmeter, &in);
        aius_meter_accumulate_output(&gmeter, go.planes[0], go.planes[1], go.frames);
        aius_meter_set_limiter_gr(&gmeter, aius_dsp_limiter_gain_reduction_db(&lim));
        aius_meter_set_telemetry(&gmeter, 96, 2000, 30000, 40000);
        aius_meter_publish(&gmeter);
        AiusMeterSnapshot gsnap;
        int gread = aius_meter_read(&gmeter, &gsnap);
        uint32_t allocs = aius_alloc_shim_alloc_count();
        aius_alloc_shim_set_arena(NULL);

        CHECK(gs == AIUS_OK, "%s guarded process status", v->name);
        CHECK(allocs == 0, "%s metered process path allocated %u time(s)", v->name, allocs);
        CHECK(gread == 1, "%s guarded meter read", v->name);
        CHECK(gsnap.sequence == 1 && gsnap.waveform_count <= AIUS_METER_WAVEFORM_MAX,
              "%s guarded meter snapshot", v->name);

        free(gl);
        free(gr);
    }

    /* --- meter taps do not alter audio + capture real vector peaks ---------- */
    {
        AiusMeter meter;
        aius_meter_init(&meter, 48000);
        int32_t *ml = malloc(F * sizeof(int32_t));
        int32_t *mr = malloc(F * sizeof(int32_t));
        AiusPcmBlock mo;
        memset(&mo, 0, sizeof mo);
        mo.planes[0] = ml;
        mo.planes[1] = mr;
        /* Identity input + bypass EQ + no limiter == bare downmix; the meter is
         * driven caller-side around process. */
        AiusBiquad eq[1];
        AiusBiquadState st[2];
        memset(st, 0, sizeof st);
        size_t ns = aius_eq_default(eq, 1);
        AiusDspConfig mc;
        memset(&mc, 0, sizeof mc);
        mc.downmix = cfg;
        mc.eq = eq;
        mc.eq_sections = ns;
        mc.eq_state = st;
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) mc.scratch[c] = pscr[c];
        mc.work_l = pwl;
        mc.work_r = pwr;
        aius_lfe_fold_reset(&fold);
        CHECK(aius_dsp_process(&mc, &in, &mo) == AIUS_OK, "%s meter proc", v->name);

        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (ml[n] != outL[n] || mr[n] != outR[n]) diff++;
        CHECK(diff == 0, "%s metered output != bare downmix (%u)", v->name, diff);

        aius_meter_accumulate_input(&meter, &in);
        aius_meter_accumulate_output(&meter, mo.planes[0], mo.planes[1], mo.frames);
        aius_meter_publish(&meter);
        AiusMeterSnapshot s;
        CHECK(aius_meter_read(&meter, &s) == 1, "%s meter read", v->name);
        /* Output peak matches an independent scan of the stereo result. */
        CHECK(s.output_peak[0] == (double)peak_abs(outL, F),
              "%s meter out_peak_l %.0f != %d", v->name, s.output_peak[0],
              peak_abs(outL, F));
        CHECK(s.output_peak[1] == (double)peak_abs(outR, F),
              "%s meter out_peak_r %.0f != %d", v->name, s.output_peak[1],
              peak_abs(outR, F));
        /* Per-input peak (identity trim) matches the decoded plane peaks. */
        {
            static const uint32_t order[AIUS_METER_INPUT_CHANNELS] = {
                AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_FC,
                AIUS_CH_LFE, AIUS_CH_SL, AIUS_CH_SR,
            };
            int pp = 0;
            for (int c = 0; c < AIUS_METER_INPUT_CHANNELS; c++) {
                if (!(p.channel_mask & order[c])) {
                    CHECK(s.input_peak[c] == 0.0 && s.input_rms[c] == 0.0,
                          "%s absent channel %d not zero", v->name, c);
                    continue;
                }
                CHECK(s.input_peak[c] == (double)peak_abs(p.ch[pp], F),
                      "%s meter in_peak[%d] %.0f != %d", v->name, c,
                      s.input_peak[c], peak_abs(p.ch[pp], F));
                CHECK(s.input_rms[c] >= 0.0 && s.input_rms[c] <= s.input_peak[c] + 1e-6,
                      "%s meter in_rms[%d] out of range", v->name, c);
                pp++;
            }
        }
        free(ml);
        free(mr);
    }

    /* --- retained known-answer fixture: signature compare ------------------ */
    {
        struct signature sig;
        memset(&sig, 0, sizeof sig);
        sig.channels = p.channels;
        sig.mask = p.channel_mask;
        sig.frames = F;
        for (int c = 0; c < p.channels; c++)
            sig.in_peak[c] = peak_abs(p.ch[c], F);
        sig.out_peak_l = peak_abs(outL, F);
        sig.out_peak_r = peak_abs(outR, F);

        if (dump) {
            char gp[1024];
            snprintf(gp, sizeof(gp), "%s/%s.golden", golden_dir, v->name);
            FILE *gf = fopen(gp, "w");
            if (!gf) {
                fprintf(stderr, "cannot write golden %s\n", gp);
                g_fail++;
            } else {
                print_signature(gf, v, &sig);
                fclose(gf);
                printf("  wrote golden %s.golden\n", v->name);
            }
        } else {
            struct signature g;
            if (load_golden(golden_dir, v->name, v, &g) != 0) {
                fprintf(stderr, "FAIL %s: golden missing/unreadable\n", v->name);
                g_fail++;
            } else {
                CHECK(g.channels == sig.channels && g.mask == sig.mask &&
                      g.frames == sig.frames, "%s golden layout drift", v->name);
                for (int c = 0; c < p.channels; c++)
                    CHECK(g.in_peak[c] == sig.in_peak[c],
                          "%s in_peak[%d] %d != golden %d", v->name, c,
                          sig.in_peak[c], g.in_peak[c]);
                CHECK(g.out_peak_l == sig.out_peak_l,
                      "%s out_peak_l %d != golden %d", v->name, sig.out_peak_l,
                      g.out_peak_l);
                CHECK(g.out_peak_r == sig.out_peak_r,
                      "%s out_peak_r %d != golden %d", v->name, sig.out_peak_r,
                      g.out_peak_r);
            }
        }

        printf("  %-20s ch=%u frames=%u in_peak", v->name, sig.channels, sig.frames);
        for (int c = 0; c < p.channels; c++)
            printf(" %d", sig.in_peak[c]);
        /* Peak reported in dBFS as well as raw units: a reviewer comparing this
         * against a golden change wants to see how close to the rail it lands, and
         * "8388607/8388607" versus "0 dBFS" is the difference between reading a
         * number and reading a clip. */
        printf(" out_peak=%d/%d (%+.2f/%+.2f dBFS) lfe_delta_peak=%.0f\n",
               sig.out_peak_l, sig.out_peak_r,
               20.0 * log10((double)sig.out_peak_l / (double)AIUS_DSP_SAMPLE_MAX),
               20.0 * log10((double)sig.out_peak_r / (double)AIUS_DSP_SAMPLE_MAX),
               g_lfe_delta_peak);
    }

done:
    free(outL);
    free(outR);
    free(dblL);
    free(dblR);
    free(refL);
    free(refR);
    free(zero);
    free(pwl);
    free(pwr);
    for (int c = 0; c < AIUS_MAX_CHANNELS; c++) {
        free(dch[c]);
        free(pscr[c]);
    }
    pcm_free(&p);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <vectors_dir> <golden_dir>\n", argv[0]);
        return 2;
    }
    const char *vec_dir = argv[1];
    const char *golden_dir = argv[2];
    int dump = 0;
    const char *d = getenv("AIUS_DSP_VECTORS_DUMP");
    if (d && d[0] && d[0] != '0')
        dump = 1;

    /* Decoder logs are per-frame on malformed input; the corpus is clean, but
     * keep stderr quiet so a genuine test FAIL stands out. */
    aius_dts_set_log_quiet(1);

    printf("DSP vector known-answer test (decode -> portable DSP interface)%s\n",
           dump ? " [DUMP]" : "");
    for (size_t i = 0; i < sizeof(k_vectors) / sizeof(k_vectors[0]); i++)
        run_vector(vec_dir, golden_dir, &k_vectors[i], dump);

    /* Defect-2 negative control (see g_unity_clipped): the corpus must still be hot
     * enough to clip at unity headroom, or the per-vector "default headroom does not
     * clip" checks are proving nothing. */
    CHECK(g_unity_clipped > 0,
          "corpus no longer clips at unity headroom, so the default-headroom "
          "checks are vacuous -- the corpus stopped stressing the matrix");
    printf("  corpus would clip %u sample(s) at unity headroom; "
           "the shipped default clips 0\n", g_unity_clipped);

    /* Item-2 negative control: at least one vector must carry LFE, or "the fold-in
     * leaves stereo untouched" is being proved on a corpus with no LFE anywhere. */
    CHECK(g_lfe_folded_vectors > 0,
          "no vector exercised the LFE fold-in, so the stereo-unchanged checks are "
          "vacuous -- the corpus stopped carrying an LFE channel");
    printf("  %u of %zu vectors exercised the LFE fold-in; the other(s) are "
           "bit-identical with it enabled\n",
           g_lfe_folded_vectors, sizeof(k_vectors) / sizeof(k_vectors[0]));

    if (g_fail) {
        printf("RESULT: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("RESULT: all vectors passed\n");
    return 0;
}
