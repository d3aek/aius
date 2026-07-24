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
 * Usage: test_dsp_vectors <vectors_dir> <golden_dir>
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_dsp.h"
#include "aius_dts.h"

static int g_fail;

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

/* Independent Lo/Ro downmix of a planar block per the documented formula. */
static void ref_downmix(const AiusDownmixConfig *cfg, const struct pcm *p,
                        int32_t *outL, int32_t *outR)
{
    int iFL = mask_plane(p->channel_mask, AIUS_CH_FL);
    int iFR = mask_plane(p->channel_mask, AIUS_CH_FR);
    int iFC = mask_plane(p->channel_mask, AIUS_CH_FC);
    int iLFE = mask_plane(p->channel_mask, AIUS_CH_LFE);
    int iSL = mask_plane(p->channel_mask, AIUS_CH_SL);
    int iSR = mask_plane(p->channel_mask, AIUS_CH_SR);
    for (uint32_t n = 0; n < p->frames; n++) {
        double l = 0.0, r = 0.0;
        if (iFL >= 0) l += (double)p->ch[iFL][n];
        if (iFR >= 0) r += (double)p->ch[iFR][n];
        if (iFC >= 0) { double c = (double)p->ch[iFC][n] * cfg->center; l += c; r += c; }
        if (iLFE >= 0) { double e = (double)p->ch[iLFE][n] * cfg->lfe; l += e; r += e; }
        if (iSL >= 0) l += (double)p->ch[iSL][n] * cfg->surround;
        if (iSR >= 0) r += (double)p->ch[iSR][n] * cfg->surround;
        outL[n] = ref_quantize(l * cfg->headroom);
        outR[n] = ref_quantize(r * cfg->headroom);
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
 * plane. Returns the number of channels (popcount(mask)). */
static int build_isolated(uint32_t mask, uint32_t bit, const int32_t *sig,
                          const int32_t *zero, const int32_t *planes[])
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
    int32_t *refL = malloc(F * sizeof(int32_t));
    int32_t *refR = malloc(F * sizeof(int32_t));
    int32_t *zero = calloc(F, sizeof(int32_t));
    if (!outL || !outR || !refL || !refR || !zero) {
        fprintf(stderr, "FAIL %s: OOM\n", v->name);
        g_fail++;
        goto done;
    }

    AiusDownmixConfig cfg = aius_downmix_config_default();

    /* --- full-vector default downmix through the public interface --------- */
    AiusPcmBlock in;
    memset(&in, 0, sizeof in);
    for (int c = 0; c < p.channels; c++)
        in.planes[c] = p.ch[c];
    in.channels = p.channels;
    in.frames = F;
    in.sample_rate_hz = p.sample_rate;
    in.channel_mask = p.channel_mask;

    AiusPcmBlock out;
    memset(&out, 0, sizeof out);
    out.planes[0] = outL;
    out.planes[1] = outR;
    CHECK(aius_dsp_downmix(&cfg, &in, &out) == AIUS_OK, "%s downmix", v->name);

    /* Bounded latency: no buffering, output frame count == input. */
    CHECK(out.frames == F, "%s frames %u != %u", v->name, out.frames, F);
    CHECK(out.channels == 2 && out.channel_mask == (AIUS_CH_FL | AIUS_CH_FR),
          "%s stereo layout", v->name);

    /* Independent reference downmix: sample-exact over the whole real vector. */
    ref_downmix(&cfg, &p, refL, refR);
    {
        uint32_t diff = 0, first = 0;
        for (uint32_t n = 0; n < F; n++) {
            if (outL[n] != refL[n] || outR[n] != refR[n]) {
                if (!diff) first = n;
                diff++;
            }
        }
        CHECK(diff == 0, "%s downmix != reference (%u samples, first @%u)",
              v->name, diff, first);
    }

    /* Finite / in-range output (no overflow-wrap; int is always finite). */
    {
        int inrange = 1;
        for (uint32_t n = 0; n < F; n++) {
            if (outL[n] > AIUS_DSP_SAMPLE_MAX || outL[n] < AIUS_DSP_SAMPLE_MIN ||
                outR[n] > AIUS_DSP_SAMPLE_MAX || outR[n] < AIUS_DSP_SAMPLE_MIN)
                inrange = 0;
        }
        CHECK(inrange, "%s output out of 24-bit range", v->name);
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
        AiusDspConfig dc = { cfg, eq, ns, st };
        CHECK(aius_dsp_process(&dc, &in, &pout) == AIUS_OK, "%s process", v->name);
        uint32_t diff = 0;
        for (uint32_t n = 0; n < F; n++)
            if (pl[n] != outL[n] || pr[n] != outR[n]) diff++;
        CHECK(diff == 0, "%s process(bypass) != downmix (%u)", v->name, diff);
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
        AiusDspConfig wc = { cfg, eq, 1, wst };
        CHECK(aius_dsp_process(&wc, &in, &wout) == AIUS_OK, "%s eq whole", v->name);

        /* two halves, state carried */
        uint32_t h = F / 2;
        AiusBiquadState sst[2];
        memset(sst, 0, sizeof sst);
        int32_t *sl = malloc(F * sizeof(int32_t));
        int32_t *sr = malloc(F * sizeof(int32_t));
        AiusDspConfig sc = { cfg, eq, 1, sst };

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
        /* Stereo: FL/FR passthrough + isolation (no cross-bleed). */
        int exactL = 1, exactR = 1;
        for (uint32_t n = 0; n < F; n++) {
            if (outL[n] != p.ch[0][n]) exactL = 0;
            if (outR[n] != p.ch[1][n]) exactR = 0;
        }
        CHECK(exactL, "%s FL->L not exact passthrough", v->name);
        CHECK(exactR, "%s FR->R not exact passthrough", v->name);
    } else {
        /* 5.1: isolate each decoded channel into a mask-consistent block and
         * prove placement + gain through the public downmix. */
        const int32_t *pl[AIUS_MAX_CHANNELS];
        int32_t *iL = malloc(F * sizeof(int32_t));
        int32_t *iR = malloc(F * sizeof(int32_t));
        AiusPcmBlock isoOut;
        memset(&isoOut, 0, sizeof isoOut);
        isoOut.planes[0] = iL;
        isoOut.planes[1] = iR;

        int iFC = mask_plane(p.channel_mask, AIUS_CH_FC);
        int iLFE = mask_plane(p.channel_mask, AIUS_CH_LFE);
        int iSL = mask_plane(p.channel_mask, AIUS_CH_SL);
        int iSR = mask_plane(p.channel_mask, AIUS_CH_SR);

        /* FC: symmetric contribution, L == R == center*FC. */
        {
            int nch = build_isolated(AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC,
                                     AIUS_CH_FC, p.ch[iFC], zero, pl);
            AiusPcmBlock b = { {(int32_t *)pl[0], (int32_t *)pl[1],
                                (int32_t *)pl[2], 0, 0, 0}, (uint16_t)nch, F,
                               48000, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC };
            CHECK(aius_dsp_downmix(&cfg, &b, &isoOut) == AIUS_OK, "%s FC iso", v->name);
            int sym = 1, gain = 1;
            for (uint32_t n = 0; n < F; n++) {
                if (iL[n] != iR[n]) sym = 0;
                if (iL[n] != ref_quantize((double)p.ch[iFC][n] * cfg.center)) gain = 0;
            }
            CHECK(sym, "%s FC not symmetric L==R", v->name);
            CHECK(gain, "%s FC gain != center", v->name);
        }
        /* SL -> L only. */
        {
            int nch = build_isolated(AIUS_CH_FL | AIUS_CH_SL, AIUS_CH_SL,
                                     p.ch[iSL], zero, pl);
            AiusPcmBlock b = { {(int32_t *)pl[0], (int32_t *)pl[1], 0, 0, 0, 0},
                               (uint16_t)nch, F, 48000, AIUS_CH_FL | AIUS_CH_SL };
            CHECK(aius_dsp_downmix(&cfg, &b, &isoOut) == AIUS_OK, "%s SL iso", v->name);
            int placed = 1, quiet = 1;
            for (uint32_t n = 0; n < F; n++) {
                if (iL[n] != ref_quantize((double)p.ch[iSL][n] * cfg.surround)) placed = 0;
                if (iR[n] != 0) quiet = 0;
            }
            CHECK(placed, "%s SL->L gain wrong", v->name);
            CHECK(quiet, "%s SL leaked to R", v->name);
        }
        /* SR -> R only. */
        {
            int nch = build_isolated(AIUS_CH_FR | AIUS_CH_SR, AIUS_CH_SR,
                                     p.ch[iSR], zero, pl);
            AiusPcmBlock b = { {(int32_t *)pl[0], (int32_t *)pl[1], 0, 0, 0, 0},
                               (uint16_t)nch, F, 48000, AIUS_CH_FR | AIUS_CH_SR };
            CHECK(aius_dsp_downmix(&cfg, &b, &isoOut) == AIUS_OK, "%s SR iso", v->name);
            int placed = 1, quiet = 1;
            for (uint32_t n = 0; n < F; n++) {
                if (iR[n] != ref_quantize((double)p.ch[iSR][n] * cfg.surround)) placed = 0;
                if (iL[n] != 0) quiet = 0;
            }
            CHECK(placed, "%s SR->R gain wrong", v->name);
            CHECK(quiet, "%s SR leaked to L", v->name);
        }
        /* LFE gain: excluded by default (0), folded at -10 dB when configured. */
        {
            int nch = build_isolated(AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE,
                                     AIUS_CH_LFE, p.ch[iLFE], zero, pl);
            AiusPcmBlock b = { {(int32_t *)pl[0], (int32_t *)pl[1],
                                (int32_t *)pl[2], 0, 0, 0}, (uint16_t)nch, F,
                               48000, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_LFE };
            /* default: LFE excluded -> stereo silent. */
            CHECK(aius_dsp_downmix(&cfg, &b, &isoOut) == AIUS_OK, "%s LFE def", v->name);
            int silent = 1;
            for (uint32_t n = 0; n < F; n++)
                if (iL[n] != 0 || iR[n] != 0) silent = 0;
            CHECK(silent, "%s LFE not excluded by default", v->name);
            /* -10 dB fold: symmetric into both. */
            AiusDownmixConfig lc = cfg;
            lc.lfe = AIUS_DSP_LFE_GAIN_MINUS10DB;
            CHECK(aius_dsp_downmix(&lc, &b, &isoOut) == AIUS_OK, "%s LFE fold", v->name);
            int folded = 1, nonzero = 0;
            for (uint32_t n = 0; n < F; n++) {
                int32_t e = ref_quantize((double)p.ch[iLFE][n] * AIUS_DSP_LFE_GAIN_MINUS10DB);
                if (iL[n] != e || iR[n] != e) folded = 0;
                if (e != 0) nonzero = 1;
            }
            CHECK(folded, "%s LFE fold gain wrong", v->name);
            CHECK(nonzero, "%s LFE test signal silent", v->name);
        }
        free(iL);
        free(iR);
    }

    /* --- clip-free headroom: real vector never saturates ------------------ */
    {
        AiusDownmixConfig cc = aius_downmix_config_default();
        cc.headroom = aius_downmix_clipfree_headroom(&cc);
        CHECK(cc.headroom > 0.0 && cc.headroom <= 1.0, "%s clipfree headroom", v->name);
        AiusPcmBlock co;
        memset(&co, 0, sizeof co);
        int32_t *cl = malloc(F * sizeof(int32_t));
        int32_t *cr = malloc(F * sizeof(int32_t));
        co.planes[0] = cl;
        co.planes[1] = cr;
        CHECK(aius_dsp_downmix(&cc, &in, &co) == AIUS_OK, "%s clipfree dmix", v->name);
        int saturated = 0;
        for (uint32_t n = 0; n < F; n++) {
            if (cl[n] == AIUS_DSP_SAMPLE_MAX || cl[n] == AIUS_DSP_SAMPLE_MIN ||
                cr[n] == AIUS_DSP_SAMPLE_MAX || cr[n] == AIUS_DSP_SAMPLE_MIN)
                saturated++;
        }
        CHECK(saturated == 0, "%s clip-free downmix saturated %u samples",
              v->name, saturated);
        free(cl);
        free(cr);
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
        printf(" out_peak=%d/%d\n", sig.out_peak_l, sig.out_peak_r);
    }

done:
    free(outL);
    free(outR);
    free(refL);
    free(refR);
    free(zero);
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

    if (g_fail) {
        printf("RESULT: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("RESULT: all vectors passed\n");
    return 0;
}
