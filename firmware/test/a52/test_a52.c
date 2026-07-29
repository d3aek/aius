/* AC-3 adapter differential + identity test (aius-ac3-support.md phase 1).
 *
 * Drives aius_a52 through its public interface over the committed
 * tools/ac3_vectors corpus and compares against FFmpeg's own decode
 * (<name>.ref.s32). liba52 float vs FFmpeg float is NOT bit-exact, so the
 * differential asserts a stated tolerance; channel identity is asserted
 * EXACTLY via per-plane Goertzel on surround51_ident (defect 30's lesson: the
 * channel map is convicted by tones). Frames are walked by a52_syncinfo
 * length — the 0x0B77 sync is 2 bytes and scanning would false-split on
 * payload bytes.
 *
 * Tolerance provenance: liba52 vs FFmpeg is dominated by AC-3 DITHER — the
 * spec reconstructs zero-bit mantissas with decoder-specific pseudo-random
 * noise, so two correct decoders legitimately differ at low level; float
 * rounding is orders below that. Observed worst across the corpus (gcc 13 /
 * glibc x86-64, 2026-07-28): max diff 1906 LSB, SNR 69.3 dB
 * (surround51_ident). Bounds carry ~4x / ~14 dB margin: they still convict
 * a channel permutation (SNR collapses), a gain error (0.5 dB -> ~25 dB
 * SNR), or a block misalignment, which is this differential's actual job —
 * bit-level truth lives with the DTS fixed-point goldens, not here.
 *
 * Usage: test_a52 <vectors_dir>
 * Exit 0 on pass, 1 on failure, 3 when the corpus is absent.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a52.h" /* a52_syncinfo only: corpus AU walking, not decode */
#include "aius_a52.h"
#include "aius_arena.h"

/* Max abs per-sample diff vs FFmpeg, in 24-bit LSBs, and minimum SNR of the
 * adapter's output against the reference. */
#define A52_DIFF_TOL_LSB 8192
#define A52_MIN_SNR_DB 55.0

#define MAX_FRAMES_PER_VECTOR 16
#define FNV1A64_BASIS UINT64_C(0xcbf29ce484222325)
#define FNV1A64_PRIME UINT64_C(0x100000001b3)

struct vector {
    const char *name;
    uint16_t channels;      /* expected decoded channels */
    uint32_t channel_mask;  /* expected AIUS mask */
    uint64_t decoded_hash;  /* FNV-1a over AU-order decoded planes */
};

static const struct vector k_vectors[] = {
    { "stereo_tones",       2, AIUS_CH_FL | AIUS_CH_FR,
      UINT64_C(0x7d7d27ae1ae01351) },
    { "stereo_impulse",     2, AIUS_CH_FL | AIUS_CH_FR,
      UINT64_C(0xe5ace9204e8da8bb) },
    { "surround51_impulse", 6, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
                               AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR,
      UINT64_C(0x27ca745607a8b834) },
    { "surround51_ident",   6, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
                               AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR,
      UINT64_C(0xd549a217bb66b3c6) },
};

static uint64_t fnv1a_sample_le(uint64_t hash, int32_t sample)
{
    uint32_t bits = (uint32_t)sample;
    for (uint32_t byte = 0; byte < 4; byte++) {
        hash ^= (uint8_t)(bits >> (8 * byte));
        hash *= FNV1A64_PRIME;
    }
    return hash;
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
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)n;
    return buf;
}

/* Split a raw AC-3 elementary stream into AUs by header-declared length. */
static size_t collect_aus(uint8_t *data, size_t size,
                          size_t *starts, size_t *lens, size_t cap)
{
    size_t count = 0, off = 0;
    while (off + 7 <= size && count < cap) {
        int flags, sample_rate, bit_rate;
        int len = a52_syncinfo(data + off, &flags, &sample_rate, &bit_rate);
        if (len <= 0) {
            fprintf(stderr, "FAIL: no sync at offset %zu\n", off);
            return 0;
        }
        if (off + (size_t)len > size)
            break; /* trailing partial frame: not expected in this corpus */
        starts[count] = off;
        lens[count] = (size_t)len;
        count++;
        off += (size_t)len;
    }
    return count;
}

static int test_core_identity_contract(const char *dir)
{
    size_t es_size = 0;
    uint8_t *es = read_file(dir, "stereo_tones", "ac3", &es_size);
    if (!es) {
        fprintf(stderr, "corpus missing for core identity contract\n");
        return 3;
    }

    size_t starts[MAX_FRAMES_PER_VECTOR], lens[MAX_FRAMES_PER_VECTOR];
    size_t naus = collect_aus(es, es_size, starts, lens,
                              MAX_FRAMES_PER_VECTOR);
    if (naus == 0 || lens[0] < 7) {
        fprintf(stderr, "FAIL: no stereo AU for core identity contract\n");
        free(es);
        return 1;
    }

    uint8_t *frame = malloc(lens[0]);
    if (!frame) {
        free(es);
        return 1;
    }
    memcpy(frame, es + starts[0], lens[0]);
    free(es);

    /* Gate recipe: stereo_tones is acmod=2, dsurmod=0 (byte 6 == 0x43).
     * Changing only dsurmod to 2 makes the core's negotiated coding
     * A52_DOLBY while preserving the decoded stereo samples. */
    if (frame[6] != 0x43) {
        fprintf(stderr, "FAIL: dsurmod fixture byte 0x%02x != 0x43\n",
                frame[6]);
        free(frame);
        return 1;
    }
    frame[6] = 0x53;

    int flags = 0, sample_rate = 0, bit_rate = 0;
    int frame_size = a52_syncinfo(frame, &flags, &sample_rate, &bit_rate);
    if (frame_size != (int)lens[0] ||
        (flags & A52_CHANNEL_MASK) != A52_DOLBY) {
        fprintf(stderr,
                "FAIL: dsurmod syncinfo size=%d flags=0x%x (want %zu/DOLBY)\n",
                frame_size, flags, lens[0]);
        free(frame);
        return 1;
    }

    a52_state_t *state = a52_init(0);
    if (!state) {
        free(frame);
        return 1;
    }
    sample_t level = 1.0f;
    int frame_flags = flags & (A52_CHANNEL_MASK | A52_LFE);
    if (a52_frame(state, frame, &frame_flags, &level, 0) != 0 ||
        (frame_flags & A52_CHANNEL_MASK) != A52_DOLBY) {
        fprintf(stderr, "FAIL: dsurmod frame flags=0x%x (want DOLBY)\n",
                frame_flags);
        a52_free(state);
        free(frame);
        return 1;
    }
    a52_free(state);

    state = a52_init(0);
    if (!state) {
        free(frame);
        return 1;
    }
    level = 1.0f;
    frame_flags = A52_STEREO;
    if (a52_frame(state, frame, &frame_flags, &level, 0) == 0) {
        fprintf(stderr, "FAIL: core accepted non-identity DOLBY->STEREO\n");
        a52_free(state);
        free(frame);
        return 1;
    }
    a52_free(state);
    free(frame);
    printf("ok core identity-only flags (DOLBY grant, non-identity reject)\n");
    return 0;
}

static double goertzel(const int32_t *x, uint32_t n, double freq)
{
    double k = floor(0.5 + (double)n * freq / 48000.0);
    double w = 2.0 * M_PI * k / (double)n;
    double c = 2.0 * cos(w);
    double s0 = 0.0, s1 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double next = (double)x[i] + c * s0 - s1;
        s1 = s0;
        s0 = next;
    }
    return sqrt(fabs(s0 * s0 + s1 * s1 - c * s0 * s1)) / (double)n;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vectors_dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    /* Unbuffered stdout: a sanitizer abort on a failure path must not eat
     * the measurement lines the failure is diagnosed from. */
    setvbuf(stdout, NULL, _IONBF, 0);

    int core_contract = test_core_identity_contract(dir);
    if (core_contract != 0)
        return core_contract;

    int32_t *decoded[AIUS_MAX_CHANNELS] = { 0 }; /* whole-vector planes */
    double worst_snr = 1e9;
    int worst_diff = 0;

    for (size_t v = 0; v < sizeof(k_vectors) / sizeof(k_vectors[0]); v++) {
        const struct vector *vec = &k_vectors[v];
        size_t es_size = 0, ref_size = 0;
        uint8_t *es = read_file(dir, vec->name, "ac3", &es_size);
        uint8_t *ref_raw = read_file(dir, vec->name, "ref.s32", &ref_size);
        if (!es || !ref_raw) {
            fprintf(stderr, "corpus vector %s missing under %s\n", vec->name,
                    dir);
            return 3;
        }
        const int32_t *ref = (const int32_t *)(void *)ref_raw;

        size_t starts[MAX_FRAMES_PER_VECTOR], lens[MAX_FRAMES_PER_VECTOR];
        size_t naus = collect_aus(es, es_size, starts, lens,
                                  MAX_FRAMES_PER_VECTOR);
        if (naus == 0) {
            fprintf(stderr, "FAIL: %s: no complete AUs\n", vec->name);
            return 1;
        }
        size_t parsed_size = starts[naus - 1] + lens[naus - 1];
        if (parsed_size != es_size) {
            fprintf(stderr,
                    "FAIL: %s: parsed %zu of %zu bytes (partial AU or cap)\n",
                    vec->name, parsed_size, es_size);
            return 1;
        }
        /* Precondition (gate F9): the reference must be exactly the decoded
         * frames — no priming/trim ambiguity exists for raw elementary
         * streams, and this pins it. */
        size_t ref_samples = ref_size / (sizeof(int32_t) * vec->channels);
        if (ref_samples != naus * AIUS_A52_FRAME_SAMPLES) {
            fprintf(stderr, "FAIL: %s: ref %zu samples != %zu AUs x 1536\n",
                    vec->name, ref_samples, naus);
            return 1;
        }

        /* FRESH decoder per vector: the IMDCT overlap delay would carry the
         * previous vector's tail into this one's first frame (gate F5). */
        AiusA52Decoder *dec = NULL;
        if (aius_a52_decoder_create(&dec) != AIUS_OK) {
            fprintf(stderr, "FAIL: decoder create\n");
            return 1;
        }

        for (int c = 0; c < AIUS_MAX_CHANNELS; c++) {
            free(decoded[c]);
            decoded[c] = malloc(naus * AIUS_A52_FRAME_SAMPLES *
                                sizeof(int32_t));
        }

        int vec_diff = 0;
        double sig = 0.0, err = 0.0;
        uint64_t decoded_hash = FNV1A64_BASIS;
        for (size_t a = 0; a < naus; a++) {
            AiusPcmBlock block;
            memset(&block, 0xA5, sizeof(block));
            AiusStatus st = aius_a52_decode_access_unit(dec, es + starts[a],
                                                        lens[a], &block);
            if (st != AIUS_OK) {
                fprintf(stderr, "FAIL: %s AU %zu status %d\n", vec->name, a,
                        (int)st);
                return 1;
            }
            if (block.channels != vec->channels ||
                block.channel_mask != vec->channel_mask ||
                block.frames != AIUS_A52_FRAME_SAMPLES) {
                fprintf(stderr, "FAIL: %s AU %zu shape ch=%u mask=%x fr=%u\n",
                        vec->name, a, block.channels, block.channel_mask,
                        block.frames);
                return 1;
            }
            /* Packed AIUS order FL,FR,FC,LFE,SL,SR == FFmpeg's 5.1 interleave
             * order positionally, so plane c compares against ref channel c. */
            for (uint16_t c = 0; c < block.channels; c++) {
                for (uint32_t n = 0; n < block.frames; n++) {
                    size_t fr = a * AIUS_A52_FRAME_SAMPLES + n;
                    int32_t ours = block.planes[c][n];
                    int32_t theirs = ref[fr * vec->channels + c] >> 8;
                    int diff = abs(ours - theirs);
                    if (diff > vec_diff)
                        vec_diff = diff;
                    sig += (double)theirs * (double)theirs;
                    err += (double)(ours - theirs) * (double)(ours - theirs);
                    decoded[c][fr] = ours;
                    decoded_hash = fnv1a_sample_le(decoded_hash, ours);
                }
            }
        }
        double snr = 10.0 * log10(sig / (err > 0.0 ? err : 1e-30));
        printf("ok %s: %zu AUs, hash %016" PRIx64
               ", max diff %d LSB, SNR %.1f dB vs FFmpeg\n",
               vec->name, naus, decoded_hash, vec_diff, snr);
        if (decoded_hash != vec->decoded_hash) {
            fprintf(stderr,
                    "FAIL: %s decoded hash %016" PRIx64
                    " != expected %016" PRIx64 "\n",
                    vec->name, decoded_hash, vec->decoded_hash);
            return 1;
        }
        if (vec_diff > A52_DIFF_TOL_LSB || snr < A52_MIN_SNR_DB) {
            fprintf(stderr, "FAIL: %s outside tolerance (%d LSB / %.1f dB)\n",
                    vec->name, A52_DIFF_TOL_LSB, A52_MIN_SNR_DB);
            return 1;
        }
        if (vec_diff > worst_diff)
            worst_diff = vec_diff;
        if (snr < worst_snr)
            worst_snr = snr;

        /* Channel identity, EXACT, on the ident vector's own decode. */
        if (strcmp(vec->name, "surround51_ident") == 0) {
            static const double tones[6] = { 300, 600, 900, 60, 1200, 1500 };
            static const char *names[6] = { "FL", "FR", "FC", "LFE", "SL",
                                            "SR" };
            uint32_t total = (uint32_t)(naus * AIUS_A52_FRAME_SAMPLES);
            for (int c = 0; c < 6; c++) {
                double own = goertzel(decoded[c], total, tones[c]);
                double next = 0.0;
                for (int t = 0; t < 6; t++)
                    if (t != c) {
                        double m = goertzel(decoded[c], total, tones[t]);
                        if (m > next)
                            next = m;
                    }
                if (own < 3.0 * next) {
                    fprintf(stderr,
                            "FAIL: channel identity: %s own %.0f next %.0f\n",
                            names[c], own, next);
                    return 1;
                }
            }
            printf("ok surround51_ident channel identity (Goertzel per plane)\n");
        }

        aius_a52_decoder_destroy(dec);
        free(es);
        free(ref_raw);
    }
    printf("ok differential worst: %d LSB, %.1f dB\n", worst_diff, worst_snr);

    /* Arena provisioning: the whole decoder in caller-owned storage, zero
     * steady-state allocations, high-water reported (phase-4 sizing input). */
    {
        static uint8_t backing[160 * 1024];
        AiusArena arena;
        if (aius_arena_init(&arena, backing, sizeof(backing)) != 0) {
            fprintf(stderr, "FAIL: arena init\n");
            return 1;
        }
        AiusA52Decoder *dec = NULL;
        if (aius_a52_decoder_create_arena(&dec, &arena) != AIUS_OK) {
            fprintf(stderr, "FAIL: arena decoder create\n");
            return 1;
        }
        size_t es_size = 0;
        uint8_t *es = read_file(dir, "surround51_ident", "ac3", &es_size);
        if (!es) {
            fprintf(stderr, "corpus missing for arena pass\n");
            return 3;
        }
        size_t starts[MAX_FRAMES_PER_VECTOR], lens[MAX_FRAMES_PER_VECTOR];
        size_t naus = collect_aus(es, es_size, starts, lens,
                                  MAX_FRAMES_PER_VECTOR);
        AiusMetrics m;
        for (size_t a = 0; a < naus && a < 2; a++) {
            AiusPcmBlock block;
            if (aius_a52_decode_access_unit(dec, es + starts[a], lens[a],
                                            &block) != AIUS_OK) {
                fprintf(stderr, "FAIL: arena decode AU %zu\n", a);
                return 1;
            }
            if (aius_a52_get_metrics(dec, &m) != AIUS_OK) {
                fprintf(stderr, "FAIL: metrics read\n");
                return 1;
            }
            if (m.alloc_count != 0) {
                fprintf(stderr,
                        "FAIL: decode AU %zu allocated %u times (want 0)\n",
                        a, m.alloc_count);
                return 1;
            }
        }
        printf("ok arena: high water %u bytes, 0 allocations per decode\n",
               m.arena_high_water);
        /* Coexistence input for phase 4: dcadec's measured peak is 62,560 B
         * in the current 128 KiB arena (which also carries the 32,768 B DSP
         * staging carve). Report the pair; the phase-4 grow-vs-swap decision
         * consumes these numbers, this test only pins that a52 fits a
         * dedicated arena comfortably. */
        printf("  phase-4 input: a52 %u + dcadec 62560 + staging 32768 vs "
               "arena 131072\n", m.arena_high_water);
        aius_a52_decoder_destroy(dec);
        free(es);
    }

    for (int c = 0; c < AIUS_MAX_CHANNELS; c++)
        free(decoded[c]);
    printf("ALL A52 TESTS PASSED\n");
    return 0;
}
