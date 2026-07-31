/* Composed decode -> downmix channel-identity test, for both resident codecs.
 *
 * DEFECT 30 IS WHY THIS EXISTS, and specifically the half of its chase that no
 * shipped suite covered. A hardware DFT capture of the 5.1 tone programme showed
 * the LEFT output carrying FR's 600 Hz and the RIGHT carrying FC's 900 Hz, which
 * reads as a permutation between the decoder's plane order and the downmix's
 * slot order. The prescribed discriminator was "a per-channel tone test through
 * the native pipeline". What shipped from it was a per-plane identity check in
 * the `dts` and `a52` suites, which convicts the DECODE side only, and
 * plane-isolation routing checks in `dsp-vectors`, which convict the DOWNMIX
 * side only and do it with synthetic single-channel blocks. Neither composes,
 * and the capture was a statement about the composition -- one real encoded 5.1
 * programme, through the real decoder, through the real matrix, read at the two
 * stereo outputs. This suite makes exactly that statement.
 *
 * The defect turned out NOT to be a firmware fault: the bench material had been
 * rotated by an ffmpeg `join` without `map=` before it was ever encoded, and
 * `aius_dts.c`'s `k_spkr_map` agreed with the downmix's mask-driven plane lookup
 * all along (see aius-network-hardening.md open defect 30). This test is the
 * standing guard that the composition stays true, and it is the run that would
 * have answered the question in one command.
 *
 * WHAT IT ASSERTS, per codec, over `surround51_ident` (one tone per source
 * channel: FL 300, FR 600, FC 900, LFE 60, SL 1200, SR 1500 Hz):
 *
 *   - every tone reaches the stereo slot(s) its channel's route names, at that
 *     route's own coefficient. The expectation is measured against the tone's
 *     level in its OWN decoded plane, so this is a statement about the route and
 *     nothing else -- the decoder's absolute output level, the DTS encoder's LFE
 *     attenuation and AC-3's applied dynamic-range scaling are all pinned
 *     elsewhere and cannot move it.
 *   - every tone whose route does NOT reach a slot arrives there at least
 *     `reject_db` below the slot it does reach.
 *   - the two symmetric routes (FC, and the folded LFE) arrive equally in both.
 *   - all of it holds through the SHIPPED default config as well as at unity,
 *     with the layout pad and the LFE crossover accounted for exactly.
 *
 * THE NEGATIVE CONTROL rotates the decoded front three planes -- the exact shape
 * defect 30 was suspected of, and the exact shape the rotated material actually
 * had -- and requires the check to REJECT it, for both codecs. A guard that has
 * quietly stopped discriminating reports green, which is how a corpus README came
 * to claim distinct tones "verify correct channel ordering" while nothing
 * checked the claim.
 *
 * Usage: test_downmix_identity <dts_vectors_dir> <ac3_vectors_dir>
 * Exit 0 on pass, 1 on failure, 3 when a corpus is absent.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a52.h" /* a52_syncinfo only: AC-3 access-unit walking, not decode */
#include "aius_a52.h"
#include "aius_dsp.h"
#include "aius_dts.h"

/* Analysis window over the decoded planes and the stereo outputs: samples
 * [IDENT_SKIP, IDENT_SKIP + IDENT_N). IDENT_N = 4000 at 48 kHz puts the bin
 * spacing at exactly 12 Hz, and every tone in the identity programme (60, 300,
 * 600, 900, 1200, 1500 Hz) is a whole multiple of it -- so each lands on its own
 * bin, a rectangular window leaks nothing, and the measured level is the tone's
 * own rather than a scalloped estimate. IDENT_SKIP clears codec start-up (the
 * DTS QMF and its 64x LFE interpolation, the AC-3 MDCT overlap) and the LR4
 * crossover's settling. The DTS vector is 5120 frames, so the window is its
 * tail; the AC-3 vector is 6144. */
#define IDENT_SKIP 1120u
#define IDENT_N    4000u

/* Full scale for both the decoded planes and the matrix output: the AIUS sample
 * scale (signed, left-aligned to AIUS_PCM_SAMPLE_BITS). The matrix does not
 * quantize, but it does not rescale either, so both sides read in these units. */
#define IDENT_FULL_SCALE ((double)AIUS_PCM_SAMPLE_MAX + 1.0)

#define IDENT_PI 3.14159265358979323846

/* Decoded PCM cap: the DTS ident vector is 5120 frames and the AC-3 one 6144, so
 * one fixed carve covers both and the harness never reallocates. */
#define PCM_CAPACITY_FRAMES 8192u

#define MAX_ACCESS_UNITS 64u

#define IDENT_MASK_51                                                          \
    (AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL |         \
     AIUS_CH_SR)

/* Per-codec measurement budget. The routing arithmetic is exact -- one double
 * multiply per sample -- so both numbers are properties of the CODEC's own
 * inter-channel floor, not of the matrix: at a tone's bin the output also
 * carries whatever the other five planes have there, summing in an unknown
 * phase. Every run prints the margins it achieved (the `err` and `leak`
 * columns), so a bound that has drifted toward the measurement is visible.
 *
 *   gain_tol_db  how far the measured route gain may sit from the coefficient.
 *                Fixed-point DTS is deterministic and quiet between channels, so
 *                its whole budget is the LFE plane's 300 Hz residue (-45.7 dB,
 *                the 64x decimation/interpolation pair) perturbing a main route;
 *                measured worst 0.011 dB against a 0.05 bound. AC-3 is lossy
 *                with dynamic-range scaling applied and its per-channel noise
 *                reaches -34 dB at a neighbour's bin; measured worst 0.073 dB
 *                (SL) against a 0.35 bound, the wider margin because that noise
 *                is float decode rather than fixed point and may move a little
 *                with the toolchain. Both stay far below what they exist to
 *                convict: the smallest wrong coefficient here is a 3 dB event.
 *
 *   reject_db    how far below its own slot a tone must sit in the slot its
 *                route does not reach. A channel in the WRONG slot is a 0 dB
 *                event, so any bound under the codec's inter-channel floor
 *                discriminates it. Measured worst on the committed corpora:
 *                55.7 dB (DTS) and 33.9 dB (AC-3). */
struct codec_bounds {
    double gain_tol_db;
    double reject_db;
};

static const struct codec_bounds k_dts_bounds = { 0.05, 40.0 };
static const struct codec_bounds k_ac3_bounds = { 0.35, 25.0 };

/* One source channel's route through the matrix, in AIUS_CH_* bit order -- which
 * is plane order, because both decoders emit tightly packed planes in that order
 * and `aius_dsp_downmix` resolves them from the mask the same way.
 *
 * THE COEFFICIENTS ARE THE HEADER'S OWN, not transcribed numbers: the shipped
 * VALUES are pinned by the `dsp` suite (exact identities on the matrix and the
 * crossover), and what this table pins is which SLOT each source channel reaches
 * and that the coefficient applied there is the one its route names. A zero gain
 * means "must not arrive here" and is asserted as rejection, not as a level. */
struct route {
    unsigned int bit;
    const char  *name;
    double       tone_hz;
    double       gain_l;
    double       gain_r;
};

static const struct route k_routes[AIUS_MAX_CHANNELS] = {
    { AIUS_CH_FL,  "FL",   300.0, 1.0, 0.0 },
    { AIUS_CH_FR,  "FR",   600.0, 0.0, 1.0 },
    { AIUS_CH_FC,  "FC",   900.0, AIUS_DSP_DOWNMIX_CENTER_DEFAULT,
      AIUS_DSP_DOWNMIX_CENTER_DEFAULT },
    { AIUS_CH_LFE, "LFE",   60.0, AIUS_DSP_LFE_GAIN_MINUS10DB,
      AIUS_DSP_LFE_GAIN_MINUS10DB },
    { AIUS_CH_SL,  "SL",  1200.0, AIUS_DSP_DOWNMIX_SURROUND_DEFAULT, 0.0 },
    { AIUS_CH_SR,  "SR",  1500.0, 0.0, AIUS_DSP_DOWNMIX_SURROUND_DEFAULT },
};

#define ROUTE_LFE 3 /* index of the LFE row above, i.e. its plane index */

/* ---- decoded vector container -------------------------------------------- */

struct pcm {
    int32_t *ch[AIUS_MAX_CHANNELS];
    double  *dch[AIUS_MAX_CHANNELS]; /* the planes as the matrix takes them */
    uint16_t channels;
    uint32_t frames;
    uint32_t sample_rate;
    uint32_t channel_mask;
};

static void pcm_free(struct pcm *p)
{
    for (int c = 0; c < AIUS_MAX_CHANNELS; c++) {
        free(p->ch[c]);
        free(p->dch[c]);
    }
    memset(p, 0, sizeof(*p));
}

static int pcm_alloc(struct pcm *p, uint16_t channels)
{
    for (uint16_t c = 0; c < channels; c++) {
        p->ch[c] = calloc(PCM_CAPACITY_FRAMES, sizeof(int32_t));
        p->dch[c] = calloc(PCM_CAPACITY_FRAMES, sizeof(double));
        if (!p->ch[c] || !p->dch[c])
            return -1;
    }
    p->channels = channels;
    return 0;
}

/* The one int32 -> double conversion, done once per vector: `aius_dsp_process`
 * owns this boundary on the target, and a test driving the matrix directly owns
 * it here. */
static void pcm_stage_doubles(struct pcm *p)
{
    for (uint16_t c = 0; c < p->channels; c++)
        for (uint32_t n = 0; n < p->frames; n++)
            p->dch[c][n] = (double)p->ch[c][n];
}

static uint8_t *read_file(const char *dir, const char *name, const char *ext,
                          size_t *size_out)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.%s", dir, name, ext);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)n;
    return buf;
}

/* Append one decoded block's planes, stopping at the harness cap. */
static void pcm_append(struct pcm *p, const AiusPcmBlock *block)
{
    uint32_t take = block->frames;
    if (p->frames + take > PCM_CAPACITY_FRAMES)
        take = PCM_CAPACITY_FRAMES - p->frames;
    for (uint16_t c = 0; c < p->channels; c++)
        memcpy(p->ch[c] + p->frames, block->planes[c], take * sizeof(int32_t));
    p->frames += take;
}

/* ---- DTS: the real adapter over the committed corpus ---------------------- */

/* DTS Core sync-word (0x7FFE8001) access-unit boundaries. */
static size_t dts_find_access_units(const uint8_t *data, size_t size,
                                    size_t *offsets, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i + 4 <= size && n < cap; i++) {
        if (data[i] == 0x7f && data[i + 1] == 0xfe && data[i + 2] == 0x80 &&
            data[i + 3] == 0x01) {
            offsets[n++] = i;
            i += 3;
        }
    }
    return n;
}

static int decode_dts(const char *dir, const char *name, struct pcm *out)
{
    memset(out, 0, sizeof(*out));
    size_t size = 0;
    uint8_t *data = read_file(dir, name, "dts", &size);
    if (!data)
        return 3;

    size_t offsets[MAX_ACCESS_UNITS];
    size_t naus = dts_find_access_units(data, size, offsets, MAX_ACCESS_UNITS);
    AiusDtsDecoder *dec = NULL;
    if (naus == 0 || aius_dts_decoder_create(&dec) != AIUS_OK) {
        free(data);
        return 1;
    }

    int rc = 0;
    for (size_t a = 0; a < naus && out->frames < PCM_CAPACITY_FRAMES; a++) {
        size_t start = offsets[a];
        size_t end = (a + 1 < naus) ? offsets[a + 1] : size;
        AiusPcmBlock block;
        if (aius_dts_decode_access_unit(dec, data + start, end - start,
                                        &block) != AIUS_OK) {
            fprintf(stderr, "FAIL %s: DTS AU %zu did not decode\n", name, a);
            rc = 1;
            break;
        }
        if (a == 0) {
            out->sample_rate = block.sample_rate_hz;
            out->channel_mask = block.channel_mask;
            if (pcm_alloc(out, block.channels) != 0) {
                rc = 1;
                break;
            }
        } else if (block.channels != out->channels ||
                   block.channel_mask != out->channel_mask) {
            fprintf(stderr, "FAIL %s: DTS AU %zu changed layout\n", name, a);
            rc = 1;
            break;
        }
        pcm_append(out, &block);
    }

    aius_dts_decoder_destroy(dec);
    free(data);
    if (rc == 0)
        pcm_stage_doubles(out);
    return rc;
}

/* ---- AC-3: the real adapter over the committed corpus --------------------- */

/* Split a raw AC-3 elementary stream by header-declared length. Scanning for the
 * two-byte 0x0B77 sync would false-split on payload bytes (see test_a52.c). */
static size_t ac3_collect_access_units(uint8_t *data, size_t size,
                                       size_t *starts, size_t *lens, size_t cap)
{
    size_t count = 0, off = 0;
    while (off + 7 <= size && count < cap) {
        int flags, sample_rate, bit_rate;
        int len = a52_syncinfo(data + off, &flags, &sample_rate, &bit_rate);
        if (len <= 0 || off + (size_t)len > size)
            break;
        starts[count] = off;
        lens[count] = (size_t)len;
        count++;
        off += (size_t)len;
    }
    return count;
}

static int decode_ac3(const char *dir, const char *name, struct pcm *out)
{
    memset(out, 0, sizeof(*out));
    size_t size = 0;
    uint8_t *data = read_file(dir, name, "ac3", &size);
    if (!data)
        return 3;

    size_t starts[MAX_ACCESS_UNITS], lens[MAX_ACCESS_UNITS];
    size_t naus =
        ac3_collect_access_units(data, size, starts, lens, MAX_ACCESS_UNITS);
    AiusA52Decoder *dec = NULL;
    if (naus == 0 || aius_a52_decoder_create(&dec) != AIUS_OK) {
        free(data);
        return 1;
    }

    int rc = 0;
    for (size_t a = 0; a < naus && out->frames < PCM_CAPACITY_FRAMES; a++) {
        if (aius_a52_begin_access_unit(dec, data + starts[a], lens[a]) !=
            AIUS_OK) {
            fprintf(stderr, "FAIL %s: AC-3 AU %zu did not begin\n", name, a);
            rc = 1;
            break;
        }
        for (uint32_t b = 0; b < AIUS_A52_BLOCKS_PER_ACCESS_UNIT; b++) {
            AiusPcmBlock block;
            if (aius_a52_decode_next_block(dec, &block) != AIUS_OK) {
                fprintf(stderr, "FAIL %s: AC-3 AU %zu block %u\n", name, a, b);
                rc = 1;
                break;
            }
            if (a == 0 && b == 0) {
                out->sample_rate = block.sample_rate_hz;
                out->channel_mask = block.channel_mask;
                if (pcm_alloc(out, block.channels) != 0) {
                    rc = 1;
                    break;
                }
            } else if (block.channels != out->channels ||
                       block.channel_mask != out->channel_mask) {
                fprintf(stderr,
                        "FAIL %s: AC-3 AU %zu block %u changed layout\n", name,
                        a, b);
                rc = 1;
                break;
            }
            pcm_append(out, &block);
        }
        if (rc)
            break;
    }

    aius_a52_decoder_destroy(dec);
    free(data);
    if (rc == 0)
        pcm_stage_doubles(out);
    return rc;
}

/* ---- measurement ---------------------------------------------------------- */

/* Level of `freq` in `x[0..n)` as dBFS of the AIUS sample scale. Single-bin
 * Goertzel; `freq` is expected to land on a bin (see IDENT_N).
 *
 * IT IS A RATIO INSTRUMENT HERE, which is what makes the check immune to every
 * level property that is not the matrix's: the matrix is linear and
 * time-invariant, so whatever a plane's tone bin holds -- including AC-3's
 * applied dynamic-range scaling and the DTS encoder's fixed LFE attenuation --
 * appears in the output bin multiplied by exactly the route's coefficient. */
static double goertzel_dbfs(const double *x, uint32_t n, double freq,
                            uint32_t rate)
{
    double k = (double)(long)(0.5 + (double)n * freq / (double)rate);
    double coeff = 2.0 * cos(2.0 * IDENT_PI * k / (double)n);
    double s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    double amp = sqrt(power > 0.0 ? power : 0.0) * 2.0 / (double)n;
    return 20.0 * log10(amp / IDENT_FULL_SCALE + 1e-30);
}

/* The crossover, run over a copy of the LFE plane by an implementation that
 * shares no code with the module: its own Direct Form I recurrence over its own
 * delay memory, starting from zero.
 *
 * WHY THE REFERENCE FILTERS RATHER THAN CORRECTING FOR |H(60 Hz)|. A single-bin
 * reading of a filtered plane is not the plane's reading times |H| at the bin
 * centre: the decoded LFE carries a little content either side of 60 Hz (the
 * encoder's 64x decimation pair, and AC-3's block-rate scaling), the LR4's gain
 * and phase move steeply through that neighbourhood, and the rectangular window
 * sums the lot. Measured, that discrepancy is 0.56 dB on the DTS vector -- ten
 * times the tolerance a flat route is held to. Filtering the reference the same
 * way removes the term entirely and keeps the assertion exact. It reads the
 * DESIGNED coefficients rather than re-deriving them, because the coefficient
 * math is pinned harder in the `dsp` suite as exact identities. */
static void reference_filter(const AiusLfeFold *fold, const double *in,
                             double *out, uint32_t n)
{
    double x1[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};
    double x2[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};
    double y1[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};
    double y2[AIUS_DSP_LFE_CROSSOVER_SECTIONS] = {0};
    for (uint32_t i = 0; i < n; i++) {
        double v = in[i];
        for (int k = 0; k < AIUS_DSP_LFE_CROSSOVER_SECTIONS; k++) {
            const AiusBiquad *q = &fold->lp[k];
            double y = q->b0 * v + q->b1 * x1[k] + q->b2 * x2[k] -
                       q->a1 * y1[k] - q->a2 * y2[k];
            x2[k] = x1[k];
            x1[k] = v;
            y2[k] = y1[k];
            y1[k] = y;
            v = y;
        }
        out[i] = v;
    }
}

/* The LAYOUT-AWARE HEADROOM PREDICATE, implemented from the contract and never
 * asked of the module -- a reference that asked it would agree with it about a
 * broken rule. Reads the mask and the channel count, the two facts the contract
 * names, and nothing else. */
static double effective_headroom(const AiusDownmixConfig *cfg,
                                 const struct pcm *p)
{
    const uint32_t summing =
        AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR;
    if (!cfg->layout_aware)
        return cfg->headroom;
    if (p->channels > 1 && (p->channel_mask & summing) != 0u)
        return cfg->headroom;
    return 1.0;
}

/* ---- the check ------------------------------------------------------------ */

/* Downmix `planes` and prove every tone lands where its route says, at the gain
 * its route names.
 *
 * `planes` is the plane vector handed to the matrix -- normally the decoded
 * planes in AIUS order, and for the negative control a permuted view of them.
 * `ref_fold` is the designed crossover when the config folds the LFE through one
 * (the reference filters its own copy of the LFE plane with it) and NULL for the
 * unfiltered fold. `report` off silences the table and the diagnosis for the
 * negative control, which expects to fail; the verdict is the return value
 * either way. Returns 0 pass, 1 fail. */
static int check_downmix_identity(const char *label, const struct pcm *p,
                                  double *const *planes,
                                  const AiusDownmixConfig *cfg,
                                  const struct codec_bounds *bounds,
                                  const AiusLfeFold *ref_fold, int report)
{
    if (p->channels != AIUS_MAX_CHANNELS || p->channel_mask != IDENT_MASK_51) {
        fprintf(stderr,
                "FAIL %s: mask 0x%x / %u channels; this check covers full 5.1 "
                "only\n",
                label, p->channel_mask, p->channels);
        return 1;
    }
    if (p->sample_rate != 48000u || p->frames < IDENT_SKIP + IDENT_N) {
        fprintf(stderr,
                "FAIL %s: %u frames at %u Hz; the window needs %u at 48000\n",
                label, p->frames, p->sample_rate, IDENT_SKIP + IDENT_N);
        return 1;
    }

    AiusPcmBlockD in;
    memset(&in, 0, sizeof in);
    for (uint16_t c = 0; c < p->channels; c++)
        in.planes[c] = planes[c];
    in.channels = p->channels;
    in.frames = p->frames;
    in.sample_rate_hz = p->sample_rate;
    in.channel_mask = p->channel_mask;

    int fail = 0;
    double *out_l = calloc(p->frames, sizeof(double));
    double *out_r = calloc(p->frames, sizeof(double));
    double *lfe_l = calloc(p->frames, sizeof(double));
    double *lfe_r = calloc(p->frames, sizeof(double));
    double *ref_lfe = calloc(p->frames, sizeof(double));
    if (!out_l || !out_r || !lfe_l || !lfe_r || !ref_lfe) {
        fprintf(stderr, "FAIL %s: OOM\n", label);
        fail = 1;
        goto done;
    }
    if (aius_dsp_downmix(cfg, &in, out_l, out_r) != AIUS_OK) {
        fprintf(stderr, "FAIL %s: downmix refused the block\n", label);
        fail = 1;
        goto done;
    }

    /* THE LFE'S CONTRIBUTION, ISOLATED WITHOUT LEAVING THE COMPOSITION. The same
     * block through the same matrix with the fold gain at
     * AIUS_DSP_LFE_GAIN_EXCLUDED, subtracted: every other channel cancels
     * exactly, and what remains is the LFE's own arrival in each slot. The pad is
     * resolved FROM THE MASK, so both runs take the same one and the subtraction
     * does not smuggle a level change in with it (aius_dsp.h §aius_dsp_downmix).
     *
     * The mains do not need this and are measured directly, but the LFE does: it
     * is the quietest route in the matrix (10 dB below a plane that is itself the
     * most coarsely coded band), so the other five planes' own content at 60 Hz
     * sets a floor no main route ever meets -- 0.76 dB of it on AC-3, measured,
     * which is a statement about the codec's noise and not about the route. */
    AiusDownmixConfig excluded = *cfg;
    excluded.lfe = AIUS_DSP_LFE_GAIN_EXCLUDED;
    if (aius_dsp_downmix(&excluded, &in, lfe_l, lfe_r) != AIUS_OK) {
        fprintf(stderr, "FAIL %s: LFE-excluded downmix refused the block\n",
                label);
        fail = 1;
        goto done;
    }
    for (uint32_t n = 0; n < p->frames; n++) {
        lfe_l[n] = out_l[n] - lfe_l[n];
        lfe_r[n] = out_r[n] - lfe_r[n];
    }

    if (ref_fold != NULL)
        reference_filter(ref_fold, planes[ROUTE_LFE], ref_lfe, p->frames);

    const double pad_db = 20.0 * log10(effective_headroom(cfg, p));

    if (report)
        printf("      %-3s %6s %10s %10s %10s %8s %8s %8s\n", "ch", "Hz",
               "source", "L", "R", "err L", "err R", "leak");

    for (uint16_t c = 0; c < p->channels; c++) {
        const struct route *r = &k_routes[c];
        const double tone = r->tone_hz;

        /* The source the matrix actually read for this slot: the plane itself,
         * or -- for a folded LFE -- the plane through the reference crossover.
         * The negative control's own levels therefore come from its permuted
         * view too, exactly as the matrix saw it.
         *
         * And the slots it is read in: the composed output for every main route,
         * the LFE-excluded DIFFERENCE for the LFE, per the isolation note above. */
        const double *src = planes[c];
        const double *win_l = out_l + IDENT_SKIP;
        const double *win_r = out_r + IDENT_SKIP;
        if (c == ROUTE_LFE) {
            if (ref_fold != NULL)
                src = ref_lfe;
            win_l = lfe_l + IDENT_SKIP;
            win_r = lfe_r + IDENT_SKIP;
        }
        double own = goertzel_dbfs(src + IDENT_SKIP, IDENT_N, tone,
                                   p->sample_rate);
        double got_l = goertzel_dbfs(win_l, IDENT_N, tone, p->sample_rate);
        double got_r = goertzel_dbfs(win_r, IDENT_N, tone, p->sample_rate);

        double err_l = 0.0, err_r = 0.0;
        int routed_l = r->gain_l > 0.0, routed_r = r->gain_r > 0.0;
        if (routed_l)
            err_l = got_l - (own + 20.0 * log10(r->gain_l) + pad_db);
        if (routed_r)
            err_r = got_r - (own + 20.0 * log10(r->gain_r) + pad_db);

        /* Rejection is measured against the slot the tone DOES reach, so a
         * channel that legitimately sits lower (the LFE) is held to the same
         * standard as the rest and a swapped slot reads as a ~0 dB leak. A route
         * that reaches both slots has nothing to reject. */
        int one_sided = routed_l != routed_r;
        double leak = 0.0;
        if (one_sided)
            leak = routed_l ? got_r - got_l : got_l - got_r;

        if (report) {
            char cl[16], cr[16], lk[16];
            if (routed_l)
                snprintf(cl, sizeof cl, "%+8.3f", err_l);
            else
                snprintf(cl, sizeof cl, "%8s", "-");
            if (routed_r)
                snprintf(cr, sizeof cr, "%+8.3f", err_r);
            else
                snprintf(cr, sizeof cr, "%8s", "-");
            if (one_sided)
                snprintf(lk, sizeof lk, "%8.1f", leak);
            else
                snprintf(lk, sizeof lk, "%8s", "both");
            printf("      %-3s %6.0f %10.3f %10.3f %10.3f %s %s %s\n", r->name,
                   tone, own, got_l, got_r, cl, cr, lk);
        }

        if (routed_l && fabs(err_l) > bounds->gain_tol_db) {
            if (report)
                fprintf(stderr,
                        "FAIL %s: %s's %.0f Hz reaches L %.3f dB off its route "
                        "(%.3f dBFS measured,\n"
                        "     source %.3f %+.3f dB route %+.3f dB pad, bound "
                        "%.2f). A wrong coefficient,\n"
                        "     or the plane order and the slot order disagree.\n",
                        label, r->name, tone, err_l, got_l, own,
                        20.0 * log10(r->gain_l), pad_db, bounds->gain_tol_db);
            fail = 1;
        }
        if (routed_r && fabs(err_r) > bounds->gain_tol_db) {
            if (report)
                fprintf(stderr,
                        "FAIL %s: %s's %.0f Hz reaches R %.3f dB off its route "
                        "(%.3f dBFS measured,\n"
                        "     source %.3f %+.3f dB route %+.3f dB pad, bound "
                        "%.2f). A wrong coefficient,\n"
                        "     or the plane order and the slot order disagree.\n",
                        label, r->name, tone, err_r, got_r, own,
                        20.0 * log10(r->gain_r), pad_db, bounds->gain_tol_db);
            fail = 1;
        }
        if (one_sided && leak > -bounds->reject_db) {
            if (report)
                fprintf(stderr,
                        "FAIL %s: %s's %.0f Hz is only %.1f dB down in the slot "
                        "its route does not\n"
                        "     reach (bound %.1f). That is a channel arriving in "
                        "the wrong output.\n",
                        label, r->name, tone, -leak, bounds->reject_db);
            fail = 1;
        }

        /* FC and the folded LFE are the two symmetric routes: they land on both
         * outputs and must land equally. Asserted directly rather than inferred
         * from two independent level checks, because an asymmetric matrix is its
         * own failure. */
        if (routed_l && routed_r &&
            fabs(got_l - got_r) > 2.0 * bounds->gain_tol_db) {
            if (report)
                fprintf(stderr,
                        "FAIL %s: %s's %.0f Hz is not symmetric: L %.3f R %.3f "
                        "(bound %.2f)\n",
                        label, r->name, tone, got_l, got_r,
                        2.0 * bounds->gain_tol_db);
            fail = 1;
        }
    }

done:
    free(out_l);
    free(out_r);
    free(lfe_l);
    free(lfe_r);
    free(ref_lfe);
    return fail;
}

/* ---- per-codec driver ----------------------------------------------------- */

/* Both configurations, then the negative control.
 *
 * THE UNITY RUN is where the coefficients are read with nothing else in the way:
 * no pad, and the unfiltered fold (an API capability the JSON schema
 * deliberately forbids, kept reachable precisely so the filter's effect is
 * measurable against it), so every route is exactly one multiply.
 *
 * THE SHIPPED RUN then repeats the whole check through the configuration the
 * device actually runs -- layout-aware 0.5 pad, LFE through the 120 Hz LR4 -- so
 * this suite cannot pass on a path nobody ships. */
static int run_codec(const char *label, struct pcm *p,
                     const struct codec_bounds *bounds)
{
    int fail = 0;

    AiusLfeFold fold;
    memset(&fold, 0, sizeof fold);

    AiusDownmixConfig shipped = aius_downmix_config_default();
    shipped.lfe_fold = &fold;

    AiusDownmixConfig unity = shipped;
    unity.headroom = 1.0;
    unity.lfe_crossover_hz = 0.0; /* unfiltered fold: one exact multiply */
    unity.lfe_fold = NULL;

    double *planes[AIUS_MAX_CHANNELS];
    for (uint16_t c = 0; c < p->channels; c++)
        planes[c] = p->dch[c];

    printf("  %s: unity headroom, unfiltered LFE fold (tolerance %.2f dB, "
           "rejection %.0f dB)\n",
           label, bounds->gain_tol_db, bounds->reject_db);
    fail |= check_downmix_identity(label, p, planes, &unity, bounds, NULL, 1);

    if (aius_lfe_fold_init(&fold, (double)p->sample_rate,
                           shipped.lfe_crossover_hz) != AIUS_OK) {
        fprintf(stderr, "FAIL %s: crossover would not design\n", label);
        return 1;
    }
    AiusLfeFold designed = fold; /* the reference's copy: coefficients only */
    aius_lfe_fold_reset(&fold);
    printf("  %s: shipped default (pad %.2f dB, LFE through the %.0f Hz LR4)\n",
           label, 20.0 * log10(effective_headroom(&shipped, p)),
           shipped.lfe_crossover_hz);
    fail |= check_downmix_identity(label, p, planes, &shipped, bounds,
                                   &designed, 1);

    /* THE NEGATIVE CONTROL. Rotate the front three planes the way a wrong map
     * table would -- and the way the bench material actually was -- and require
     * the check to reject it. Everything else about the run is identical, so a
     * green here means the check has stopped discriminating and every pass above
     * it is worthless. */
    double *rotated[AIUS_MAX_CHANNELS];
    for (uint16_t c = 0; c < p->channels; c++)
        rotated[c] = planes[c];
    rotated[0] = planes[2];
    rotated[1] = planes[0];
    rotated[2] = planes[1];
    if (check_downmix_identity(label, p, rotated, &unity, bounds, NULL, 0) ==
        0) {
        fprintf(stderr,
                "FAIL %s: the check ACCEPTED a rotated front three -- it is no "
                "longer\n"
                "     discriminating, so every pass above it is vacuous.\n",
                label);
        fail = 1;
    } else {
        printf("  ok %s rejects a rotated front three (negative control)\n",
               label);
    }

    return fail;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <dts_vectors_dir> <ac3_vectors_dir>\n",
                argv[0]);
        return 1;
    }
    const char *dts_dir = argv[1];
    const char *ac3_dir = argv[2];

    struct pcm dts, ac3;
    int rc = decode_dts(dts_dir, "surround51_ident", &dts);
    if (rc == 3) {
        fprintf(stderr, "DTS corpus absent: %s/surround51_ident.dts\n", dts_dir);
        return 3;
    }
    if (rc != 0) {
        pcm_free(&dts);
        return 1;
    }

    rc = decode_ac3(ac3_dir, "surround51_ident", &ac3);
    if (rc == 3) {
        fprintf(stderr, "AC-3 corpus absent: %s/surround51_ident.ac3\n",
                ac3_dir);
        pcm_free(&dts);
        return 3;
    }
    if (rc != 0) {
        pcm_free(&dts);
        pcm_free(&ac3);
        return 1;
    }

    printf("decoded DTS  %u frames, %u ch, mask 0x%x, %u Hz\n", dts.frames,
           dts.channels, dts.channel_mask, dts.sample_rate);
    printf("decoded AC-3 %u frames, %u ch, mask 0x%x, %u Hz\n", ac3.frames,
           ac3.channels, ac3.channel_mask, ac3.sample_rate);

    int fail = 0;
    fail |= run_codec("dts", &dts, &k_dts_bounds);
    fail |= run_codec("ac3", &ac3, &k_ac3_bounds);

    pcm_free(&dts);
    pcm_free(&ac3);

    if (fail) {
        fprintf(stderr, "DOWNMIX IDENTITY FAILED\n");
        return 1;
    }
    printf("ok decode -> downmix channel identity, both codecs\n");
    return 0;
}
