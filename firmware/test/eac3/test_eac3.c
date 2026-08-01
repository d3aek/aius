/* E-AC-3 adapter differential + regression test (aius-eac3-support.md
 * phase 2).
 *
 * Drives aius_eac3 through its public interface over the committed
 * tools/eac3_vectors corpus and compares against FFmpeg's own decode
 * (<name>.ref.s32). The adapter IS FFmpeg's C decode path, so the
 * differential tolerance covers only SIMD-vs-C float rounding in the
 * reference build -- far tighter than the a52 suite's dither-dominated
 * bound -- while still convicting a channel permutation, gain error, or
 * block misalignment. The FNV hash pins the adapter's own output exactly
 * (regression oracle); the arena pass proves the no-heap steady state and
 * records the slot-sizing high-water; the malformed family proves
 * fail-closed behaviour under ASan/UBSan.
 *
 * Usage: test_eac3 <vectors_dir>
 * Exit 0 on pass, 1 on failure, 3 when the corpus is absent.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_arena.h"
#include "aius_eac3.h"

/* Max abs per-sample diff vs FFmpeg (24-bit LSBs) and minimum SNR. Same C
 * algorithm on both sides; the gap is the reference build's SIMD float
 * ordering plus the s32 reference's own rounding. Observed worst at
 * authoring: 1 LSB / >100 dB. */
#define EAC3_DIFF_TOL_LSB 256
#define EAC3_MIN_SNR_DB 80.0

#define MAX_FRAMES_PER_VECTOR 16
#define FNV1A64_BASIS UINT64_C(0xcbf29ce484222325)
#define FNV1A64_PRIME UINT64_C(0x100000001b3)

/* The suite's arena mirrors the codec-slot shape: the printed high-water is
 * the Phase-4 slot-sizing evidence, and the capacity assert keeps it from
 * silently outgrowing what the plan records. */
#define EAC3_TEST_ARENA_BYTES (256u * 1024u)

struct vector {
    const char *name;
    uint16_t channels;
    uint32_t channel_mask;
    uint32_t sample_rate_hz;
    uint64_t decoded_hash; /* FNV-1a over AU -> channel -> frame order */
};

static const struct vector k_vectors[] = {
    { "mono_tone",          1, AIUS_CH_FC,
      48000u, UINT64_C(0x9851ad07438b7194) },
    { "stereo_tones",       2, AIUS_CH_FL | AIUS_CH_FR,
      48000u, UINT64_C(0x04b35276c4f236bd) },
    { "stereo_tones_44100", 2, AIUS_CH_FL | AIUS_CH_FR,
      44100u, UINT64_C(0x8bf6dd7e141877f8) },
    { "stereo_tones_32000", 2, AIUS_CH_FL | AIUS_CH_FR,
      32000u, UINT64_C(0x8dfdc6459a9e0cc3) },
    { "surround51_impulse", 6, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
                               AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR,
      48000u, UINT64_C(0x6f87b9872c9fd415) },
    { "surround51_ident",   6, AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
                               AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR,
      48000u, UINT64_C(0xa5906f20eeebf49c) },
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

/* Split a raw E-AC-3 elementary stream into syncframes by the header's
 * 11-bit frmsiz ((frmsiz + 1) * 2 bytes). */
static size_t collect_aus(const uint8_t *data, size_t size,
                          size_t *starts, size_t *lens, size_t cap)
{
    size_t count = 0, off = 0;
    while (off + 6 <= size && count < cap) {
        if (data[off] != 0x0B || data[off + 1] != 0x77) {
            fprintf(stderr, "FAIL: no sync at offset %zu\n", off);
            return 0;
        }
        size_t len =
            ((size_t)(((data[off + 2] & 0x07u) << 8) | data[off + 3]) + 1u) *
            2u;
        if (off + len > size)
            break;
        starts[count] = off;
        lens[count] = len;
        count++;
        off += len;
    }
    return count;
}

static int g_fail;

#define CHECK(cond, ...)                        \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: ");          \
            fprintf(stderr, __VA_ARGS__);       \
            fprintf(stderr, "\n");              \
            g_fail++;                           \
        }                                       \
    } while (0)

/* ---- arena pass: no-heap steady state + slot-sizing high-water ---------- */

static uint8_t g_eac3_arena[EAC3_TEST_ARENA_BYTES]
    __attribute__((aligned(16)));

static int test_arena_steady_state(const char *dir)
{
    size_t es_size = 0;
    uint8_t *es = read_file(dir, "surround51_ident", "eac3", &es_size);
    if (!es) {
        fprintf(stderr, "corpus vector surround51_ident missing\n");
        return 3;
    }
    size_t starts[MAX_FRAMES_PER_VECTOR], lens[MAX_FRAMES_PER_VECTOR];
    size_t naus = collect_aus(es, es_size, starts, lens,
                              MAX_FRAMES_PER_VECTOR);
    CHECK(naus >= 2, "arena: need >= 2 AUs, got %zu", naus);
    if (naus < 2) {
        free(es);
        return 1;
    }

    AiusArena arena;
    CHECK(aius_arena_init(&arena, g_eac3_arena, sizeof(g_eac3_arena)) == 0,
          "arena: init");

    AiusEac3Decoder *dec = NULL;
    CHECK(aius_eac3_decoder_create_arena(&dec, &arena) == AIUS_OK,
          "arena: create");
    if (!dec) {
        free(es);
        return 1;
    }

    AiusPcmBlock block;
    AiusMetrics m;
    /* Warmup AU: the fork's remaining lazy allocations (downmix scratch et
     * al) land here. */
    CHECK(aius_eac3_decode_access_unit(dec, es + starts[0], lens[0],
                                       &block) == AIUS_OK,
          "arena: warmup decode");
    CHECK(aius_eac3_get_metrics(dec, &m) == AIUS_OK, "arena: warmup metrics");
    const uint32_t warmup_allocs = m.alloc_count;

    /* Steady state: zero allocations from here on. */
    for (size_t a = 1; a < naus; a++) {
        CHECK(aius_eac3_decode_access_unit(dec, es + starts[a], lens[a],
                                           &block) == AIUS_OK,
              "arena: AU %zu decode", a);
        CHECK(aius_eac3_get_metrics(dec, &m) == AIUS_OK,
              "arena: AU %zu metrics", a);
        CHECK(m.alloc_count == 0u,
              "arena: AU %zu allocated %u (steady state must be 0)", a,
              m.alloc_count);
    }
    CHECK(m.arena_high_water > 0u &&
              m.arena_high_water <= EAC3_TEST_ARENA_BYTES,
          "arena: high-water %u vs capacity %u", m.arena_high_water,
          EAC3_TEST_ARENA_BYTES);
    printf("ok arena: warmup allocs %u, steady 0, high-water %u/%u bytes\n",
           warmup_allocs, m.arena_high_water, EAC3_TEST_ARENA_BYTES);

    aius_eac3_decoder_destroy(dec);
    free(es);
    return 0;
}

/* ---- malformed family: fail closed under ASan/UBSan --------------------- */

static int test_malformed(const char *dir)
{
    size_t es_size = 0;
    uint8_t *es = read_file(dir, "stereo_tones", "eac3", &es_size);
    if (!es) {
        fprintf(stderr, "corpus vector stereo_tones missing\n");
        return 3;
    }
    size_t starts[MAX_FRAMES_PER_VECTOR], lens[MAX_FRAMES_PER_VECTOR];
    size_t naus = collect_aus(es, es_size, starts, lens,
                              MAX_FRAMES_PER_VECTOR);
    CHECK(naus >= 1, "malformed: no AUs");

    AiusEac3Decoder *dec = NULL;
    CHECK(aius_eac3_decoder_create(&dec) == AIUS_OK, "malformed: create");

    AiusPcmBlock block;

    /* Garbage: no sync anywhere. */
    uint8_t junk[512];
    for (size_t i = 0; i < sizeof(junk); i++)
        junk[i] = (uint8_t)(i * 37u + 11u);
    CHECK(aius_eac3_decode_access_unit(dec, junk, sizeof(junk), &block) ==
              AIUS_MALFORMED,
          "malformed: junk accepted");

    /* Truncated: a real frame cut short. */
    CHECK(aius_eac3_decode_access_unit(dec, es + starts[0], lens[0] / 2,
                                       &block) == AIUS_MALFORMED,
          "malformed: truncated frame accepted");

    /* Tiny. */
    CHECK(aius_eac3_decode_access_unit(dec, es, 4, &block) == AIUS_MALFORMED,
          "malformed: 4-byte input accepted");

    /* Corrupt payload: flip bytes mid-frame; the decoder must either refuse
     * (MALFORMED) or decode-with-concealment -- both are in-contract; the
     * point is no OOB/UB under the sanitizers and no crash. */
    uint8_t *bent = malloc(lens[0]);
    memcpy(bent, es + starts[0], lens[0]);
    for (size_t i = lens[0] / 2; i < lens[0] / 2 + 8 && i < lens[0]; i++)
        bent[i] ^= 0x5A;
    AiusStatus st = aius_eac3_decode_access_unit(dec, bent, lens[0], &block);
    CHECK(st == AIUS_MALFORMED || st == AIUS_OK,
          "malformed: bent frame status %d", (int)st);
    free(bent);

    /* A valid decode still works after the failures (no poisoned state). */
    CHECK(aius_eac3_decode_access_unit(dec, es + starts[0], lens[0],
                                       &block) == AIUS_OK,
          "malformed: recovery decode failed");

    printf("ok malformed: junk/truncated/tiny refused, bent survives, "
           "recovery decodes\n");
    aius_eac3_decoder_destroy(dec);
    free(es);
    return 0;
}

/* ---- main: differential + hash over the corpus -------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vectors_dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];

    printf("Aius E-AC-3 adapter test (FFmpeg-fork differential + "
           "regression)\n");

    int rc = test_arena_steady_state(dir);
    if (rc == 3)
        return 3;
    rc = test_malformed(dir);
    if (rc == 3)
        return 3;

    double worst_snr = 1e9;
    int worst_diff = 0;

    for (size_t v = 0; v < sizeof(k_vectors) / sizeof(k_vectors[0]); v++) {
        const struct vector *vec = &k_vectors[v];
        size_t es_size = 0, ref_size = 0;
        uint8_t *es = read_file(dir, vec->name, "eac3", &es_size);
        uint8_t *ref_raw = read_file(dir, vec->name, "ref.s32", &ref_size);
        if (!es || !ref_raw) {
            fprintf(stderr, "corpus vector %s missing under %s\n", vec->name,
                    dir);
            return 3;
        }
        const int32_t *ref = (const int32_t *)(void *)ref_raw;
        const size_t ref_samples =
            ref_size / (sizeof(int32_t) * vec->channels);

        size_t starts[MAX_FRAMES_PER_VECTOR], lens[MAX_FRAMES_PER_VECTOR];
        size_t naus = collect_aus(es, es_size, starts, lens,
                                  MAX_FRAMES_PER_VECTOR);
        if (naus == 0) {
            fprintf(stderr, "FAIL: %s: no complete AUs\n", vec->name);
            return 1;
        }
        size_t parsed = starts[naus - 1] + lens[naus - 1];
        if (parsed != es_size) {
            fprintf(stderr, "FAIL: %s: parsed %zu of %zu bytes\n", vec->name,
                    parsed, es_size);
            return 1;
        }

        /* FRESH decoder per vector: the IMDCT overlap delay would carry the
         * previous vector's tail into this one's first frame. */
        AiusEac3Decoder *dec = NULL;
        if (aius_eac3_decoder_create(&dec) != AIUS_OK) {
            fprintf(stderr, "FAIL: decoder create\n");
            return 1;
        }

        int vec_diff = 0;
        double sig = 0.0, err = 0.0;
        uint64_t decoded_hash = FNV1A64_BASIS;
        size_t frames_total = 0;

        for (size_t a = 0; a < naus; a++) {
            AiusPcmBlock block;
            memset(&block, 0xA5, sizeof(block));
            AiusStatus st = aius_eac3_decode_access_unit(
                dec, es + starts[a], lens[a], &block);
            if (st != AIUS_OK) {
                fprintf(stderr, "FAIL: %s AU %zu status %d\n", vec->name, a,
                        (int)st);
                return 1;
            }
            if (block.channels != vec->channels ||
                block.channel_mask != vec->channel_mask ||
                block.sample_rate_hz != vec->sample_rate_hz ||
                block.frames == 0 ||
                block.frames > AIUS_EAC3_MAX_FRAME_SAMPLES ||
                block.frames % AIUS_EAC3_BLOCK_SAMPLES != 0) {
                fprintf(stderr,
                        "FAIL: %s AU %zu shape ch=%u mask=%x fr=%u rate=%u\n",
                        vec->name, a, block.channels, block.channel_mask,
                        block.frames, block.sample_rate_hz);
                return 1;
            }
            /* AIUS plane order FL,FR,FC,LFE,SL,SR == FFmpeg's interleave
             * order positionally for every carried layout. */
            for (uint16_t c = 0; c < block.channels; c++) {
                for (uint32_t n = 0; n < block.frames; n++) {
                    size_t fr = frames_total + n;
                    int32_t ours = block.planes[c][n];
                    if (fr < ref_samples) {
                        int32_t theirs =
                            ref[fr * vec->channels + c] >> 8;
                        int diff = abs(ours - theirs);
                        if (diff > vec_diff)
                            vec_diff = diff;
                        sig += (double)theirs * (double)theirs;
                        err += (double)(ours - theirs) *
                               (double)(ours - theirs);
                    }
                }
            }
            /* AU -> channel -> frame hash order (the shipped FNV oracle
             * construction). */
            for (uint16_t c = 0; c < block.channels; c++)
                for (uint32_t n = 0; n < block.frames; n++)
                    decoded_hash = fnv1a_sample_le(decoded_hash,
                                                   block.planes[c][n]);
            frames_total += block.frames;
        }

        if (frames_total != ref_samples) {
            fprintf(stderr,
                    "FAIL: %s decoded %zu frames, reference has %zu\n",
                    vec->name, frames_total, ref_samples);
            return 1;
        }

        double snr = 10.0 * log10(sig / (err > 0.0 ? err : 1e-30));
        printf("ok %s: %zu AUs, hash %016" PRIx64
               ", max diff %d LSB, SNR %.1f dB vs FFmpeg\n",
               vec->name, naus, decoded_hash, vec_diff, snr);
        if (vec->decoded_hash != 0 && decoded_hash != vec->decoded_hash) {
            fprintf(stderr,
                    "FAIL: %s decoded hash %016" PRIx64 " != expected %016"
                    PRIx64 "\n",
                    vec->name, decoded_hash, vec->decoded_hash);
            return 1;
        }
        if (vec_diff > EAC3_DIFF_TOL_LSB || snr < EAC3_MIN_SNR_DB) {
            fprintf(stderr, "FAIL: %s outside tolerance (%d LSB / %.1f dB)\n",
                    vec->name, EAC3_DIFF_TOL_LSB, EAC3_MIN_SNR_DB);
            return 1;
        }
        if (vec_diff > worst_diff)
            worst_diff = vec_diff;
        if (snr < worst_snr)
            worst_snr = snr;

        aius_eac3_decoder_destroy(dec);
        free(ref_raw);
        free(es);
    }

    if (g_fail) {
        printf("RESULT: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("ok differential worst: %d LSB, %.1f dB\n", worst_diff, worst_snr);
    printf("RESULT: all checks passed\n");
    return 0;
}
