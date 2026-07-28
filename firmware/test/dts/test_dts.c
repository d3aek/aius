/* Host differential + channel-identity test for the Aius DTS Core decoder (P3).
 *
 * Two independent questions about the same decode, because the first cannot
 * answer the second.
 *
 * DIFFERENTIAL. For each committed vector, decode the raw DTS Core access units
 * with the AiusDtsDecoder (vendored dcadec fixed-point core) and compare the
 * planar PCM against FFmpeg's own decode of the same bitstream. FFmpeg's dca
 * decoder and dcadec share lineage, so the two agree to well within a fixed
 * tolerance once a constant integer decoder-delay offset and a constant
 * amplitude scale are aligned out. We require, per channel: exact channel count,
 * matching sample count (after the shared delay), Pearson correlation > 0.9999,
 * and residual error below -80 dBFS of full scale.
 *
 * CHANNEL IDENTITY. The differential check compares OUR plane c against
 * FFMPEG'S channel c, so it is blind by construction to anything both decoders
 * do identically -- including a shared channel permutation, and including a
 * permutation baked into the vector before either decoder saw it. Defect 30 was
 * exactly that blind spot: a hardware capture of the 5.1 tone programme showed
 * the left output carrying FR's tone, which reads as a decoder permutation, and
 * the corpus README claimed distinct tones "verify correct channel ordering"
 * when nothing checked the claim. So `surround51_ident` carries a known tone per
 * named channel and check_channel_identity() Goertzels every decoded plane
 * against that assignment: the tone the channel owns must be present at the
 * encoder's level and every other channel's tone must be absent.
 *
 * Usage: test_dts <vectors_dir>
 * The vectors_dir holds <name>.dts and <name>.ref.s32 produced by
 * tools/dts_vectors/generate.sh. Exit 0 on all-pass, 1 on any failure.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_dts.h"

#define REF_FULL_SCALE   2147483648.0 /* 2^31: FFmpeg s32 full scale */

/* Full-band channels: the vendored fixed-point core and FFmpeg's DCA decoder
 * share lineage and agree bit-exactly (measured corr ~1.0, err < -125 dBFS). */
#define CORR_THRESHOLD   0.9999
#define ERR_DBFS_MAX     (-80.0)

/* LFE channel: DTS transmits the LFE decimated 64x (~750 Hz) and each decoder
 * reconstructs it with its OWN 64x interpolation FIR. FFmpeg's LFE FIR differs
 * from dcadec's fixed LFE FIR, so their magnitude spectra diverge (verified:
 * different harmonic-image ratios, not a delay or gain). The LFE is therefore
 * correctly reconstructed but not bit-exact to FFmpeg; hold it to a looser
 * bound that still proves right routing, band and amplitude. Matching the LFE
 * bit-exactly needs a dcadec-lineage LFE reference and is a P3 follow-up. */
#define LFE_CORR_THRESHOLD 0.95
#define LFE_ERR_DBFS_MAX   (-40.0)
/* The DTS core QMF synthesis delay is bounded well under this; a small window
 * also keeps a periodic (click-train) signal from aliasing onto a far lag.
 * Main channels align at 0; the LFE decimation/interpolation path carries a
 * different, small constant group delay that we align out per channel. */
#define MAX_LAG          256
#define MAX_CH           AIUS_MAX_CHANNELS

struct pcm {
    double *ch[MAX_CH];
    uint16_t channels;
    uint32_t frames;      /* per-channel sample count */
    uint32_t sample_rate;
    uint32_t channel_mask;
};

static void pcm_free(struct pcm *p)
{
    for (int c = 0; c < MAX_CH; c++)
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

/* Find DTS Core sync words (0x7FFE8001, 16-bit big-endian) that mark access
 * unit boundaries. Fills offsets[] with start offsets; returns their count. */
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

/* Decode every access unit in the .dts file into planar double PCM. */
static int decode_dts(const char *path, struct pcm *out)
{
    memset(out, 0, sizeof(*out));

    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data)
        return -1;

    size_t offsets[8192];
    int nframes = find_frames(data, size, offsets, 8192);
    if (nframes <= 0 || nframes > 8192) {
        fprintf(stderr, "%s: no DTS Core sync words found\n", path);
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
            fprintf(stderr, "%s: frame %d decode status %d\n", path, fi, st);
            rc = -1;
            break;
        }
        if (fi == 0) {
            out->channels = block.channels;
            out->sample_rate = block.sample_rate_hz;
            out->channel_mask = block.channel_mask;
        } else if (block.channels != out->channels ||
                   block.channel_mask != out->channel_mask) {
            fprintf(stderr, "%s: frame %d channel layout changed\n", path, fi);
            rc = -1;
            break;
        }

        uint32_t need = out->frames + block.frames;
        if (need > cap) {
            size_t newcap = cap ? cap * 2 : 8192;
            while (newcap < need)
                newcap *= 2;
            for (int c = 0; c < out->channels; c++) {
                double *g = realloc(out->ch[c], newcap * sizeof(double));
                if (!g) { rc = -1; break; }
                out->ch[c] = g;
            }
            if (rc) break;
            cap = newcap;
        }
        for (int c = 0; c < out->channels; c++)
            for (uint32_t n = 0; n < block.frames; n++)
                out->ch[c][out->frames + n] = (double)block.planes[c][n];
        out->frames += block.frames;
    }

    aius_dts_decoder_destroy(dec);
    free(data);
    return rc;
}

/* Load an interleaved s32le reference into planar double PCM. */
static int load_ref(const char *path, int channels, struct pcm *out)
{
    memset(out, 0, sizeof(*out));

    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data)
        return -1;

    uint32_t frames = (uint32_t)(size / (4u * (unsigned)channels));
    out->channels = (uint16_t)channels;
    out->frames = frames;
    for (int c = 0; c < channels; c++) {
        out->ch[c] = malloc((size_t)frames * sizeof(double));
        if (!out->ch[c]) { free(data); return -1; }
    }
    for (uint32_t n = 0; n < frames; n++) {
        for (int c = 0; c < channels; c++) {
            size_t off = ((size_t)n * channels + c) * 4;
            int32_t v = (int32_t)((uint32_t)data[off] |
                                  ((uint32_t)data[off + 1] << 8) |
                                  ((uint32_t)data[off + 2] << 16) |
                                  ((uint32_t)data[off + 3] << 24));
            out->ch[c][n] = (double)v;
        }
    }
    free(data);
    return 0;
}

/* Best integer lag applied to `dec` so dec[n+lag] aligns to ref[n] on channel
 * `ch`, chosen to maximize normalized cross-correlation. Main and LFE channels
 * carry different constant group delays, so each channel is aligned on its own.
 * A near-silent channel (no variance) reports lag 0. */
static int find_lag_ch(const struct pcm *dec, const struct pcm *ref, int ch)
{
    int best_lag = 0;
    double best_score = -1.0;
    for (int lag = -MAX_LAG; lag <= MAX_LAG; lag++) {
        double num = 0.0, de = 0.0, dr = 0.0;
        for (uint32_t n = 0; n < ref->frames; n++) {
            long j = (long)n + lag;
            if (j < 0 || (uint32_t)j >= dec->frames)
                continue;
            double a = dec->ch[ch][j];
            double b = ref->ch[ch][n];
            num += a * b;
            de += a * a;
            dr += b * b;
        }
        if (de <= 0.0 || dr <= 0.0)
            continue;
        double score = num / sqrt(de * dr);
        if (score > best_score) { best_score = score; best_lag = lag; }
    }
    return best_lag;
}

struct metric { double corr; double err_dbfs; double scale; uint32_t n; };

/* Compare one channel over the lag-aligned overlap. */
static struct metric compare_ch(const double *dec, uint32_t dec_n,
                                const double *ref, uint32_t ref_n, int lag)
{
    struct metric m = { 0 };
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    uint32_t cnt = 0;
    for (uint32_t n = 0; n < ref_n; n++) {
        long j = (long)n + lag;
        if (j < 0 || (uint32_t)j >= dec_n)
            continue;
        double a = dec[j];   /* decoder */
        double b = ref[n];   /* reference */
        sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
        cnt++;
    }
    m.n = cnt;
    if (cnt == 0)
        return m;

    /* Pearson correlation (scale/offset invariant). */
    double cov = sab - sa * sb / cnt;
    double va = saa - sa * sa / cnt;
    double vb = sbb - sb * sb / cnt;
    m.corr = (va > 0 && vb > 0) ? cov / sqrt(va * vb) : 1.0;

    /* Least-squares amplitude scale mapping decoder -> reference. */
    double scale = (saa > 0) ? sab / saa : 0.0;
    m.scale = scale;

    /* Residual RMS relative to reference full scale. */
    double sse = 0.0;
    for (uint32_t n = 0; n < ref_n; n++) {
        long j = (long)n + lag;
        if (j < 0 || (uint32_t)j >= dec_n)
            continue;
        double r = ref[n] - scale * dec[j];
        sse += r * r;
    }
    double rms = sqrt(sse / cnt);
    m.err_dbfs = 20.0 * log10((rms + 1e-12) / REF_FULL_SCALE);
    return m;
}

/* AIUS_CH_* bit carried by the c-th tightly-packed plane, given the block's
 * channel mask. Planes are emitted in canonical AIUS order. */
static unsigned int plane_aius_bit(uint32_t mask, int plane)
{
    static const unsigned int order[] = {
        AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_FC, AIUS_CH_LFE, AIUS_CH_SL, AIUS_CH_SR,
    };
    int seen = 0;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (mask & order[i]) {
            if (seen == plane)
                return order[i];
            seen++;
        }
    }
    return 0;
}

/* --- channel identity ----------------------------------------------------- */

/* Analysis window over the decoded planes: samples [IDENT_SKIP, IDENT_SKIP +
 * IDENT_N). IDENT_N = 4000 at 48 kHz makes the bin spacing exactly 12 Hz, and
 * every tone in the identity vector (60, 300, 600, 900, 1200, 1500 Hz) is a
 * whole multiple of it -- so each lands on its own bin, a rectangular window
 * leaks nothing, and the measured level is the tone's own rather than a
 * scalloped estimate. IDENT_SKIP clears the QMF start-up and the much longer
 * 64x LFE interpolation start-up; the vector is 5120 samples, so the window is
 * the tail of it. */
#define IDENT_SKIP 1120u
#define IDENT_N    4000u

/* Full scale for the decoded planes: they carry the AIUS sample scale (signed,
 * left-aligned to AIUS_PCM_SAMPLE_BITS), not FFmpeg's s32 range. */
#define IDENT_FULL_SCALE ((double)AIUS_PCM_SAMPLE_MAX + 1.0)

#define IDENT_PI 3.14159265358979323846

/* A foreign channel's tone must arrive in this plane at least this far below
 * the level it reaches in its OWN plane. Measured on the committed vector, the
 * five full-band planes reject everything by 90-108 dB and the LFE plane -- the
 * only interesting one, since its 64x decimation/interpolation pair passes a
 * little of the 300 Hz neighbour -- by 45.7 dB. The bound sits below all of
 * them deliberately: the failure it catches is a whole channel in the wrong
 * plane, a 0 dB event, and tightening it would only convert encoder stopband
 * behaviour into a false alarm. */
#define IDENT_REJECT_DB 40.0

/* Every full-band plane reproduces its own tone at the level the encoder was
 * fed. The window is tight because the whole path is deterministic fixed point
 * over committed bytes: the spread across the five full-band planes is under
 * 0.01 dB. */
#define IDENT_MAIN_DBFS     (-22.50)
#define IDENT_MAIN_DBFS_TOL   0.20

/* THE LFE PLANE IS NOT AT UNITY, and the deficit is a property of the bitstream
 * rather than of this decoder. Fed the same 60 Hz level as the full-band
 * channels, the decoded LFE plane comes back this far below them.
 *
 * It is a fixed attenuation of the ENCODED signal, not a mapping fault and not
 * quantization. Measured through FFmpeg's own decoder on purpose-built
 * material: sweeping the LFE input amplitude over 24 dB (0.45 down to 0.0225)
 * moves the decoded level by the same 24 dB, holding the offset at 9.83 dB at
 * every step; sweeping the LFE tone at fixed amplitude adds the residual
 * low-pass of the encoder's 64x decimation FIR on top of it (7.25 dB at 10 Hz,
 * 7.48 at 20, 8.36 at 40, 9.83 at 60, 14.66 at 100, 24.41 at 150). FFmpeg's
 * decoder and ours both reproduce it, and the `dts-lfe` suite pins ours
 * bit-exact against an upstream dcadec build, so it is upstream of every
 * decoder involved.
 *
 * The pinned value is OURS, not FFmpeg's, and the two differ: FFmpeg reads
 * -9.70 dB on this same vector because it reconstructs the 64x-decimated LFE
 * with a different interpolation FIR (the same divergence the LFE_* thresholds
 * above accommodate). Ours is the number the device actually hands the DSP.
 *
 * It matters because it is the second half of defect 30. The hardware capture
 * put the LFE 19 dB below the dominant output tone against an expectation of
 * about -10.5 dB from the fold gain and the crossover alone, and that gap read
 * as part of a mapping fault. Fold (-10 dB) plus the LR4 at 60 Hz (-0.53 dB)
 * plus this -8.32 dB predicts -18.85 dB, which is the capture. Pinned tightly
 * so a genuine decode-side LFE regression -- a lost scale factor, the wrong
 * interpolation FIR -- fails here rather than being waved through as more of
 * the same encoder property. */
#define IDENT_LFE_OFFSET_DB   (-8.32)
#define IDENT_LFE_OFFSET_TOL    0.10

/* Tone carried by each AIUS channel of the identity vector, in AIUS_CH_* bit
 * order (FL, FR, FC, LFE, SL, SR). Mirrors the hardware bench tone programme. */
static const double k_ident_tones[AIUS_MAX_CHANNELS] = {
    300.0, 600.0, 900.0, 60.0, 1200.0, 1500.0,
};

/* What the identity check is expected to say about a vector.
 *
 * IDENT_REJECTS is a negative control, and it is not decoration. A guard that
 * has quietly stopped discriminating reports green, which is exactly how the
 * corpus README came to claim distinct tones "verify correct channel ordering"
 * while nothing checked it. `surround51_tones` is the KNOWN-ROTATED vector (see
 * tools/dts_vectors/README.md §The `join` channel trap), so the check must
 * reject it on every run or it is not a check. */
enum ident_mode {
    IDENT_NONE = 0,   /* not an identity-bearing vector */
    IDENT_PASSES,     /* tones on the channels they name */
    IDENT_REJECTS     /* known permuted: the check MUST fail it */
};

struct vector {
    const char *name;
    int channels;
    uint32_t expect_mask;
    enum ident_mode identity;
};

static const struct vector k_vectors[] = {
    { "stereo_tones", 2,
      AIUS_CH_FL | AIUS_CH_FR, IDENT_NONE },
    { "surround51_tones", 6,
      AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR,
      IDENT_REJECTS },
    { "stereo_impulse", 2,
      AIUS_CH_FL | AIUS_CH_FR, IDENT_NONE },
    { "surround51_impulse", 6,
      AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR,
      IDENT_NONE },
    { "surround51_ident", 6,
      AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR,
      IDENT_PASSES },
};

/* Level of `freq` in `x[0..n)` as dBFS of the AIUS sample scale. Single-bin
 * Goertzel; `freq` is expected to land on a bin (see IDENT_N). */
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

static const char *aius_bit_name(unsigned int bit)
{
    switch (bit) {
    case AIUS_CH_FL:  return "FL";
    case AIUS_CH_FR:  return "FR";
    case AIUS_CH_FC:  return "FC";
    case AIUS_CH_LFE: return "LFE";
    case AIUS_CH_SL:  return "SL";
    case AIUS_CH_SR:  return "SR";
    default:          return "??";
    }
}

/* Every decoded plane must carry the tone its channel owns and none of the
 * others. Returns 0 on pass, 1 on any failure. `report` off silences the
 * measurements and the diagnosis, for the negative control that expects to
 * fail; the verdict is the return value either way. */
static int check_channel_identity(const char *name, const struct pcm *dec,
                                  int report)
{
    if (dec->frames < IDENT_SKIP + IDENT_N) {
        if (report)
            fprintf(stderr, "FAIL %s identity: %u frames, need %u\n",
                    name, dec->frames, IDENT_SKIP + IDENT_N);
        return 1;
    }
    if (dec->sample_rate != 48000u) {
        if (report)
            fprintf(stderr,
                    "FAIL %s identity: rate %u, the window assumes 48000\n",
                    name, dec->sample_rate);
        return 1;
    }
    /* Full 5.1 only, so plane index == index into k_ident_tones. A narrower
     * mask would pack the planes differently and silently compare the wrong
     * tone against each plane. */
    if (dec->channels != AIUS_MAX_CHANNELS ||
        dec->channel_mask != (AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
                              AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR)) {
        if (report)
            fprintf(stderr,
                    "FAIL %s identity: mask 0x%x / %u channels; the identity "
                    "check covers full 5.1 only\n",
                    name, dec->channel_mask, dec->channels);
        return 1;
    }

    int fail = 0;
    double own[AIUS_MAX_CHANNELS];

    /* Pass 1: each plane's own tone. `c` indexes k_ident_tones directly -- the
     * full-5.1 guard above is what makes that true. */
    for (int c = 0; c < dec->channels; c++)
        own[c] = goertzel_dbfs(dec->ch[c] + IDENT_SKIP, IDENT_N,
                               k_ident_tones[c], dec->sample_rate);

    /* Pass 2: how much of every OTHER channel reached this plane. Bleed is
     * measured against the tone's level in its OWN plane, not against this
     * plane's level, so a plane that legitimately sits lower (the LFE) is held
     * to the same standard as the rest and a swap reads as bleed near 0 dB. */
    for (int c = 0; c < dec->channels; c++) {
        unsigned int bit = plane_aius_bit(dec->channel_mask, c);
        const double *win = dec->ch[c] + IDENT_SKIP;

        double worst_bleed = -1e9;
        int worst_idx = -1;
        for (int t = 0; t < dec->channels; t++) {
            if (t == c)
                continue;
            double bleed = goertzel_dbfs(win, IDENT_N, k_ident_tones[t],
                                         dec->sample_rate) - own[t];
            if (bleed > worst_bleed) { worst_bleed = bleed; worst_idx = t; }
        }

        if (report)
            printf("    %-3s %6.0f Hz %8.3f dBFS   worst bleed %6.0f Hz "
                   "%7.1f dB\n",
                   aius_bit_name(bit), k_ident_tones[c], own[c],
                   k_ident_tones[worst_idx], worst_bleed);

        if (worst_bleed > -IDENT_REJECT_DB) {
            if (report)
                fprintf(stderr,
                        "FAIL %s identity: plane %d (%s) carries %.0f Hz at "
                        "%.2f dB of that channel's own level (<= -%.1f)\n"
                        "     %.0f Hz belongs to plane %d. The decode is "
                        "permuted, or the vector was\n"
                        "     authored permuted (see "
                        "tools/dts_vectors/README.md).\n",
                        name, c, aius_bit_name(bit), k_ident_tones[worst_idx],
                        worst_bleed, IDENT_REJECT_DB,
                        k_ident_tones[worst_idx], worst_idx);
            fail = 1;
        }

        if (bit != AIUS_CH_LFE &&
            fabs(own[c] - IDENT_MAIN_DBFS) > IDENT_MAIN_DBFS_TOL) {
            if (report)
                fprintf(stderr,
                        "FAIL %s identity: plane %d (%s) tone at %.3f dBFS, "
                        "expected %.2f +/- %.2f\n",
                        name, c, aius_bit_name(bit), own[c], IDENT_MAIN_DBFS,
                        IDENT_MAIN_DBFS_TOL);
            fail = 1;
        }
    }

    /* The LFE plane's level against the full-band planes it was fed alongside.
     * See IDENT_LFE_OFFSET_DB for why the expected value is not 0. */
    int lfe_plane = -1, main_plane = -1;
    for (int c = 0; c < dec->channels; c++) {
        if (plane_aius_bit(dec->channel_mask, c) == AIUS_CH_LFE)
            lfe_plane = c;
        else if (main_plane < 0)
            main_plane = c;
    }
    if (lfe_plane >= 0 && main_plane >= 0) {
        double offset = own[lfe_plane] - own[main_plane];
        if (report)
            printf("    LFE offset vs full-band planes %.3f dB "
                   "(expected %.2f +/- %.2f; encoder property, see source)\n",
                   offset, IDENT_LFE_OFFSET_DB, IDENT_LFE_OFFSET_TOL);
        if (fabs(offset - IDENT_LFE_OFFSET_DB) > IDENT_LFE_OFFSET_TOL) {
            if (report)
                fprintf(stderr,
                        "FAIL %s identity: LFE plane at %.3f dB against the "
                        "full-band planes, expected %.2f +/- %.2f\n",
                        name, offset, IDENT_LFE_OFFSET_DB,
                        IDENT_LFE_OFFSET_TOL);
            fail = 1;
        }
    }

    return fail;
}

static int run_vector(const char *dir, const struct vector *v)
{
    char dts_path[1024], ref_path[1024];
    snprintf(dts_path, sizeof(dts_path), "%s/%s.dts", dir, v->name);
    snprintf(ref_path, sizeof(ref_path), "%s/%s.ref.s32", dir, v->name);

    struct pcm dec, ref;
    if (decode_dts(dts_path, &dec) != 0) {
        fprintf(stderr, "FAIL %s: decode error\n", v->name);
        return 1;
    }
    if (load_ref(ref_path, v->channels, &ref) != 0) {
        fprintf(stderr, "FAIL %s: reference load error\n", v->name);
        pcm_free(&dec);
        return 1;
    }

    int fail = 0;

    if (dec.channels != v->channels) {
        fprintf(stderr, "FAIL %s: channel count %u != %d\n",
                v->name, dec.channels, v->channels);
        fail = 1;
    }
    if (dec.channel_mask != v->expect_mask) {
        fprintf(stderr, "FAIL %s: channel mask 0x%x != 0x%x\n",
                v->name, dec.channel_mask, v->expect_mask);
        fail = 1;
    }

    /* Exact sample-count match: the two decoders emit the same number of
     * per-channel samples for the same bitstream. */
    if (!fail && dec.frames != ref.frames) {
        fprintf(stderr, "FAIL %s: sample count dec=%u != ref=%u\n",
                v->name, dec.frames, ref.frames);
        fail = 1;
    }

    if (!fail) {
        printf("  %-20s ch=%u frames=%u rate=%u\n",
               v->name, dec.channels, dec.frames, dec.sample_rate);
        for (int c = 0; c < v->channels; c++) {
            int is_lfe = plane_aius_bit(dec.channel_mask, c) == AIUS_CH_LFE;
            double corr_min = is_lfe ? LFE_CORR_THRESHOLD : CORR_THRESHOLD;
            double err_max = is_lfe ? LFE_ERR_DBFS_MAX : ERR_DBFS_MAX;
            int lag = find_lag_ch(&dec, &ref, c);
            struct metric m = compare_ch(dec.ch[c], dec.frames,
                                         ref.ch[c], ref.frames, lag);
            printf("    ch%d%s lag=%d corr=%.8f err=%.2f dBFS scale=%.6f n=%u\n",
                   c, is_lfe ? " [LFE]" : "     ", lag, m.corr, m.err_dbfs,
                   m.scale, m.n);
            if (m.n == 0 || m.corr < corr_min || m.err_dbfs > err_max) {
                fprintf(stderr,
                        "FAIL %s ch%d%s: lag=%d corr=%.8f (>%.4f) err=%.2f dBFS (<%.1f)\n",
                        v->name, c, is_lfe ? " [LFE]" : "", lag, m.corr, corr_min,
                        m.err_dbfs, err_max);
                fail = 1;
            }
        }
    }

    /* Runs whatever the differential result: the two answer different
     * questions, and a permuted decode can pass the differential check. */
    if (v->identity == IDENT_PASSES) {
        fail |= check_channel_identity(v->name, &dec, 1);
    } else if (v->identity == IDENT_REJECTS) {
        if (check_channel_identity(v->name, &dec, 0) == 0) {
            fprintf(stderr,
                    "FAIL %s: the identity check PASSED a vector known to be "
                    "channel-rotated.\n"
                    "     The check is not discriminating, so its green result "
                    "on surround51_ident\n"
                    "     means nothing. See tools/dts_vectors/README.md "
                    "§The `join` channel trap.\n",
                    v->name);
            fail = 1;
        } else {
            printf("    identity negative control: rotation detected, as it "
                   "must be\n");
        }
    }

    pcm_free(&dec);
    pcm_free(&ref);
    return fail;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vectors_dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];

    int failures = 0;
    printf("DTS Core differential test (dcadec core vs FFmpeg)\n");
    for (size_t i = 0; i < sizeof(k_vectors) / sizeof(k_vectors[0]); i++)
        failures += run_vector(dir, &k_vectors[i]);

    if (failures) {
        printf("RESULT: %d vector(s) FAILED\n", failures);
        return 1;
    }
    printf("RESULT: all vectors passed\n");
    return 0;
}
