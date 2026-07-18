/* Host bit-exact test: Aius DTS Core decoder vs a full dcadec build (P3 LFE).
 *
 * The `dts` differential suite proves the Aius/dcadec core matches FFmpeg on the
 * full-band channels but holds the LFE to a loose bound, because FFmpeg
 * reconstructs the 64x-decimated LFE with a DIFFERENT interpolation FIR than
 * dcadec's fixed-point LFE FIR. That looseness was a caveat, not a defect: since
 * we vendored dcadec's OWN core, our LFE must be bit-exact to a full dcadec
 * build, which runs the identical interpolate_lfe_fixed_fir under
 * DCADEC_FLAG_CORE_ONLY | DCADEC_FLAG_CORE_BIT_EXACT.
 *
 * This suite closes the caveat. For each committed vector it decodes with the
 * AiusDtsDecoder and compares against dcadec's own core decode of the same
 * bitstream (tools/dcadec_ref/<name>.dcadec.s32, produced by `dcadec -c -b -m`).
 * The reference carries the native 24-bit sample range dcadec's core emits -
 * the SAME representation AiusDtsDecoder emits - so NO scale or lag alignment is
 * applied: EVERY channel, LFE included, must match sample-for-sample and
 * channel-for-channel exactly. Any single differing sample fails and is
 * reported with its channel, index, and both values.
 *
 * Usage: test_dts_lfe <vectors_dir> <ref_dir>
 *   vectors_dir holds <name>.dts (tools/dts_vectors/vectors)
 *   ref_dir     holds <name>.dcadec.s32 (tools/dcadec_ref)
 * Exit 0 on all-pass, 1 on any mismatch.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_dts.h"

#define MAX_CH AIUS_MAX_CHANNELS

struct pcm {
    int32_t *ch[MAX_CH];
    uint16_t channels;
    uint32_t frames;
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
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *size_out = got;
    return buf;
}

/* DTS Core sync words (0x7FFE8001) mark access-unit boundaries. */
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

/* Decode every access unit in the .dts file into planar int32 PCM (the decoder's
 * native 24-bit range, sign-extended). No scaling: values are copied verbatim. */
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
                int32_t *g = realloc(out->ch[c], newcap * sizeof(int32_t));
                if (!g) { rc = -1; break; }
                out->ch[c] = g;
            }
            if (rc) break;
            cap = newcap;
        }
        for (int c = 0; c < out->channels; c++)
            for (uint32_t n = 0; n < block.frames; n++)
                out->ch[c][out->frames + n] = block.planes[c][n];
        out->frames += block.frames;
    }

    aius_dts_decoder_destroy(dec);
    free(data);
    return rc;
}

/* Load the interleaved s32le dcadec reference into planar int32 PCM. */
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
        out->ch[c] = malloc((size_t)frames * sizeof(int32_t));
        if (!out->ch[c]) { free(data); return -1; }
    }
    for (uint32_t n = 0; n < frames; n++) {
        for (int c = 0; c < channels; c++) {
            size_t off = ((size_t)n * channels + c) * 4;
            int32_t v = (int32_t)((uint32_t)data[off] |
                                  ((uint32_t)data[off + 1] << 8) |
                                  ((uint32_t)data[off + 2] << 16) |
                                  ((uint32_t)data[off + 3] << 24));
            out->ch[c][n] = v;
        }
    }
    free(data);
    return 0;
}

/* AIUS_CH_* bit carried by the c-th tightly-packed plane, given the mask. */
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

struct vector { const char *name; int channels; uint32_t expect_mask; };

static const struct vector k_vectors[] = {
    { "stereo_tones", 2,
      AIUS_CH_FL | AIUS_CH_FR },
    { "surround51_tones", 6,
      AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR },
    { "stereo_impulse", 2,
      AIUS_CH_FL | AIUS_CH_FR },
    { "surround51_impulse", 6,
      AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR },
};

static int run_vector(const char *vdir, const char *rdir, const struct vector *v)
{
    char dts_path[1024], ref_path[1024];
    snprintf(dts_path, sizeof(dts_path), "%s/%s.dts", vdir, v->name);
    snprintf(ref_path, sizeof(ref_path), "%s/%s.dcadec.s32", rdir, v->name);

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
            uint32_t mismatches = 0;
            uint32_t first_idx = 0;
            int32_t first_dec = 0, first_ref = 0;
            int32_t peak = 0; /* |ref| peak: proves the channel carries signal */
            for (uint32_t n = 0; n < dec.frames; n++) {
                int32_t rv = ref.ch[c][n];
                int32_t a = rv < 0 ? -rv : rv;
                if (a > peak)
                    peak = a;
                if (dec.ch[c][n] != rv) {
                    if (mismatches == 0) {
                        first_idx = n;
                        first_dec = dec.ch[c][n];
                        first_ref = rv;
                    }
                    mismatches++;
                }
            }
            printf("    ch%d%s exact=%s (%u/%u samples match, peak=%d)\n",
                   c, is_lfe ? " [LFE]" : "     ",
                   mismatches == 0 ? "yes" : "NO",
                   dec.frames - mismatches, dec.frames, peak);
            if (mismatches) {
                fprintf(stderr,
                        "FAIL %s ch%d%s: %u/%u samples differ; first at n=%u dec=%d ref=%d\n",
                        v->name, c, is_lfe ? " [LFE]" : "", mismatches,
                        dec.frames, first_idx, first_dec, first_ref);
                fail = 1;
            }
        }
    }

    pcm_free(&dec);
    pcm_free(&ref);
    return fail;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <vectors_dir> <ref_dir>\n", argv[0]);
        return 2;
    }
    const char *vdir = argv[1];
    const char *rdir = argv[2];

    int failures = 0;
    printf("DTS Core LFE bit-exact test (AiusDtsDecoder vs dcadec core)\n");
    for (size_t i = 0; i < sizeof(k_vectors) / sizeof(k_vectors[0]); i++)
        failures += run_vector(vdir, rdir, &k_vectors[i]);

    if (failures) {
        printf("RESULT: %d vector(s) FAILED\n", failures);
        return 1;
    }
    printf("RESULT: all channels bit-exact to dcadec (LFE included)\n");
    return 0;
}
