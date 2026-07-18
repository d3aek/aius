/* Convert N mono WAV files into one interleaved signed-32-bit-LE PCM stream.
 *
 * The dcadec CLI, run with `-c -b -m` (core-only, fixed-point bit-exact, mono
 * native-layout output), writes one mono WAV per DTS channel named by the
 * dcadec-native speaker name. This helper reads those mono WAVs in the order
 * given on the command line (the caller passes them in AIUS canonical channel
 * order: FL, FR, FC, LFE, SL, SR, restricted to the channels present) and
 * interleaves their samples into a single `<name>.dcadec.s32` reference the
 * host dts-lfe test compares against sample-for-sample.
 *
 * Each mono WAV carries the DTS Core's native 24-bit range (24-bit PCM,
 * right-aligned); every sample is sign-extended into int32 with no scaling, so
 * the emitted reference is bit-identical to AiusDtsDecoder's planar output.
 *
 * Usage: wav_to_s32 <out.s32> <in0.wav> [<in1.wav> ...]
 * Exit: 0 on success, non-zero on any parse/IO error.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mono {
    int32_t *samples;
    uint32_t nsamples;
};

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "wav_to_s32: cannot open %s\n", path);
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

/* Parse one mono 24-bit PCM WAV into sign-extended int32 samples. */
static int load_mono_wav(const char *path, struct mono *out)
{
    out->samples = NULL;
    out->nsamples = 0;

    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data)
        return -1;

    if (size < 12 || memcmp(data, "RIFF", 4) != 0 ||
        memcmp(data + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "wav_to_s32: %s: not a RIFF/WAVE file\n", path);
        free(data);
        return -1;
    }

    uint16_t channels = 0, bits = 0;
    const uint8_t *pcm = NULL;
    uint32_t pcm_bytes = 0;
    int have_fmt = 0;

    size_t off = 12;
    while (off + 8 <= size) {
        const uint8_t *id = data + off;
        uint32_t clen = rd_u32le(data + off + 4);
        size_t body = off + 8;
        if (body + clen > size)
            clen = (uint32_t)(size - body); /* tolerate a short trailing chunk */
        if (memcmp(id, "fmt ", 4) == 0 && clen >= 16) {
            channels = rd_u16le(data + body + 2);
            bits = rd_u16le(data + body + 14);
            have_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            pcm = data + body;
            pcm_bytes = clen;
        }
        off = body + clen + (clen & 1u); /* chunks are word-aligned */
    }

    if (!have_fmt || !pcm) {
        fprintf(stderr, "wav_to_s32: %s: missing fmt/data chunk\n", path);
        free(data);
        return -1;
    }
    if (channels != 1) {
        fprintf(stderr, "wav_to_s32: %s: expected mono, got %u channels\n",
                path, channels);
        free(data);
        return -1;
    }
    if (bits != 24) {
        fprintf(stderr, "wav_to_s32: %s: expected 24-bit PCM, got %u-bit\n",
                path, bits);
        free(data);
        return -1;
    }

    uint32_t nsamples = pcm_bytes / 3u;
    int32_t *samples = malloc((size_t)nsamples * sizeof(int32_t));
    if (!samples) { free(data); return -1; }
    for (uint32_t i = 0; i < nsamples; i++) {
        const uint8_t *s = pcm + (size_t)i * 3u;
        int32_t v = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                              ((uint32_t)s[2] << 16));
        if (v & 0x00800000)
            v |= (int32_t)0xFF000000u; /* sign-extend 24 -> 32 */
        samples[i] = v;
    }

    out->samples = samples;
    out->nsamples = nsamples;
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.s32> <in0.wav> [<in1.wav> ...]\n",
                argv[0]);
        return 2;
    }
    const char *out_path = argv[1];
    int nch = argc - 2;
    if (nch > 6) {
        fprintf(stderr, "wav_to_s32: too many channels (%d)\n", nch);
        return 2;
    }

    struct mono ch[6];
    int rc = 0;
    for (int c = 0; c < nch; c++) {
        if (load_mono_wav(argv[2 + c], &ch[c]) != 0) {
            rc = 1;
            nch = c; /* only free what loaded */
            goto cleanup;
        }
        if (c > 0 && ch[c].nsamples != ch[0].nsamples) {
            fprintf(stderr,
                    "wav_to_s32: channel %d has %u samples, channel 0 has %u\n",
                    c, ch[c].nsamples, ch[0].nsamples);
            rc = 1;
            nch = c + 1;
            goto cleanup;
        }
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "wav_to_s32: cannot write %s\n", out_path);
        rc = 1;
        goto cleanup;
    }
    for (uint32_t n = 0; n < ch[0].nsamples; n++) {
        for (int c = 0; c < nch; c++) {
            int32_t v = ch[c].samples[n];
            uint8_t b[4] = {
                (uint8_t)(v & 0xff),
                (uint8_t)((v >> 8) & 0xff),
                (uint8_t)((v >> 16) & 0xff),
                (uint8_t)((v >> 24) & 0xff),
            };
            if (fwrite(b, 1, 4, out) != 4) {
                fprintf(stderr, "wav_to_s32: write error on %s\n", out_path);
                rc = 1;
                break;
            }
        }
        if (rc)
            break;
    }
    fclose(out);

cleanup:
    for (int c = 0; c < nch; c++)
        free(ch[c].samples);
    return rc;
}
