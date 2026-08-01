/* E-AC-3 adapter robustness fuzz (aius-eac3-support.md phase 2).
 *
 * Phase 1 is a deterministic seam hunt over every committed seed AU
 * (header-byte sweeps, truncations at every prefix seam, sync-in-payload
 * splices, concatenations -- the fuzz_dts/fuzz_a52 seam shapes adapted to
 * the single-shot contract). Phase 2 is random corruption with a planted
 * xorshift64* seed so a failing iteration reproduces. Every decode runs
 * under the defect-31 termination oracle (hard alarm bound), must return a
 * TYPED status (AIUS_REFUSED must never escape a decoder), and an AIUS_OK
 * block must be shape-sane. ASan/UBSan convict memory/UB violations.
 *
 * Usage: fuzz_eac3 <vectors_dir> [iterations]
 * Exit 0 on pass, 1 on violation, 3 when the corpus is absent.
 */

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "aius_eac3.h"

#define DECODE_HARD_BOUND_S 10
#define MAX_SEEDS 8
#define MAX_AUS_PER_SEED 16
#define WORK_CAP (AIUS_EAC3_MAX_BURST + 64u)

struct seed_file {
    const char *name;
    uint8_t *data;
    size_t size;
    size_t starts[MAX_AUS_PER_SEED];
    size_t lens[MAX_AUS_PER_SEED];
    size_t naus;
};

static const char *const k_seed_names[] = {
    "mono_tone",          "stereo_tones",       "stereo_tones_44100",
    "stereo_tones_32000", "surround51_impulse", "surround51_ident",
};

static uint8_t *read_file(const char *dir, const char *name,
                          size_t *size_out)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.eac3", dir, name);
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

static size_t collect_aus(const uint8_t *data, size_t size, size_t *starts,
                          size_t *lens, size_t cap)
{
    size_t count = 0, off = 0;
    while (off + 6 <= size && count < cap) {
        if (data[off] != 0x0B || data[off + 1] != 0x77)
            break;
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

/* ---- termination oracle (the defect-31 shape) --------------------------- */

static char g_decode_ctx[192];

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: decode exceeded the hard bound: ";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    r = write(STDERR_FILENO, g_decode_ctx, strlen(g_decode_ctx));
    r = write(STDERR_FILENO, "\n", 1);
    (void)r;
    _exit(1);
}

static uint64_t g_max_decode_ns;
static char g_max_decode_ctx[sizeof(g_decode_ctx)];

/* xorshift64*: deterministic so a failing iteration reproduces. */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rng_next(void)
{
    uint64_t x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}
static uint32_t rng_range(uint32_t n)
{
    return n ? (uint32_t)(rng_next() % n) : 0;
}

static int status_is_typed(AiusStatus st)
{
    switch (st) {
    case AIUS_OK:
    case AIUS_NEED_MORE:
    case AIUS_UNSUPPORTED:
    case AIUS_MALFORMED:
    case AIUS_TRUNCATED:
    case AIUS_NO_SPACE:
    case AIUS_INVALID_ARG:
        return 1;
    case AIUS_REFUSED:
        return 0;
    }
    return 0;
}

static int block_is_sane(const AiusPcmBlock *b)
{
    if (b->channels == 0 || b->channels > AIUS_MAX_CHANNELS)
        return 0;
    if (b->frames == 0 || b->frames > AIUS_EAC3_MAX_FRAME_SAMPLES ||
        b->frames % AIUS_EAC3_BLOCK_SAMPLES != 0)
        return 0;
    if ((uint32_t)__builtin_popcount(b->channel_mask) != b->channels)
        return 0;
    for (uint16_t c = 0; c < b->channels; c++)
        if (b->planes[c] == NULL)
            return 0;
    return 1;
}

/* One decode under the oracles. Returns non-zero on a contract violation. */
static int exercise(AiusEac3Decoder *dec, const uint8_t *buf, size_t len)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    alarm(DECODE_HARD_BOUND_S);

    AiusPcmBlock block;
    memset(&block, 0xA5, sizeof(block));
    AiusStatus st = aius_eac3_decode_access_unit(dec, buf, len, &block);

    alarm(0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                  (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    if (ns > g_max_decode_ns) {
        g_max_decode_ns = ns;
        memcpy(g_max_decode_ctx, g_decode_ctx, sizeof(g_max_decode_ctx));
    }

    if (!status_is_typed(st)) {
        fprintf(stderr, "FAIL: untyped status %d: %s\n", (int)st,
                g_decode_ctx);
        return 1;
    }
    if (st == AIUS_OK && !block_is_sane(&block)) {
        fprintf(stderr, "FAIL: insane OK block: %s\n", g_decode_ctx);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <vectors_dir> [iterations]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    long iters = argc == 3 ? strtol(argv[2], NULL, 10) : 20000;

    signal(SIGALRM, on_alarm);

    struct seed_file seeds[MAX_SEEDS];
    int nseeds = 0;
    for (size_t i = 0;
         i < sizeof(k_seed_names) / sizeof(k_seed_names[0]) &&
         nseeds < MAX_SEEDS;
         i++) {
        struct seed_file *sf = &seeds[nseeds];
        sf->name = k_seed_names[i];
        sf->data = read_file(dir, sf->name, &sf->size);
        if (!sf->data)
            continue;
        sf->naus = collect_aus(sf->data, sf->size, sf->starts, sf->lens,
                               MAX_AUS_PER_SEED);
        if (sf->naus > 0)
            nseeds++;
        else
            free(sf->data);
    }
    if (nseeds == 0) {
        fprintf(stderr, "no usable seed AUs under %s\n", dir);
        return 3;
    }

    AiusEac3Decoder *dec = NULL;
    if (aius_eac3_decoder_create(&dec) != AIUS_OK) {
        fprintf(stderr, "FAIL: decoder create\n");
        return 1;
    }

    uint8_t *work = malloc(WORK_CAP);
    long exercised = 0;
    int fail = 0;

    printf("E-AC-3 robustness/fuzz: %ld iterations over %d seed file(s)\n",
           iters, nseeds);

    /* Phase 1: deterministic seam hunt over every seed AU. */
    for (int i = 0; i < nseeds && !fail; i++) {
        const struct seed_file *sf = &seeds[i];
        for (size_t a = 0; a < sf->naus && !fail; a++) {
            const uint8_t *au = sf->data + sf->starts[a];
            const size_t len = sf->lens[a];

            /* Every truncation length. */
            for (size_t cut = 0; cut <= len && !fail; cut++) {
                snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                         "%s AU %zu truncate %zu/%zu", sf->name, a, cut,
                         len);
                memcpy(work, au, cut);
                fail = exercise(dec, work, cut);
                exercised++;
            }
            if (fail)
                break;

            /* Header-byte sweep: every value of each of the first 8
             * bytes. */
            for (size_t b = 0; b < 8 && b < len && !fail; b++) {
                memcpy(work, au, len);
                for (int v = 0; v < 256 && !fail; v++) {
                    work[b] = (uint8_t)v;
                    snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                             "%s AU %zu hdr[%zu]=%02x", sf->name, a, b, v);
                    fail = exercise(dec, work, len);
                    exercised++;
                }
            }
            if (fail)
                break;

            /* Sync-in-payload splice: a second sync word planted mid-AU,
             * and the AU self-concatenated (superframe shape). */
            memcpy(work, au, len);
            if (len > 16) {
                work[len / 2] = 0x0B;
                work[len / 2 + 1] = 0x77;
                snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                         "%s AU %zu planted sync", sf->name, a);
                fail = fail || exercise(dec, work, len);
                exercised++;
            }
            if (!fail && 2 * len <= WORK_CAP) {
                memcpy(work, au, len);
                memcpy(work + len, au, len);
                snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                         "%s AU %zu self-concat", sf->name, a);
                fail = exercise(dec, work, 2 * len);
                exercised++;
            }
        }
    }

    /* Phase 2: random corruption. */
    for (long it = 0; it < iters && !fail; it++) {
        const struct seed_file *sf = &seeds[rng_range((uint32_t)nseeds)];
        const size_t a = rng_range((uint32_t)sf->naus);
        const uint8_t *au = sf->data + sf->starts[a];
        size_t len = sf->lens[a];
        memcpy(work, au, len);

        const uint32_t mode = rng_range(4);
        if (mode == 0) {
            /* byte corruption burst */
            uint32_t hits = 1 + rng_range(16);
            for (uint32_t h = 0; h < hits; h++)
                work[rng_range((uint32_t)len)] = (uint8_t)rng_next();
        } else if (mode == 1) {
            /* bit flips */
            uint32_t hits = 1 + rng_range(32);
            for (uint32_t h = 0; h < hits; h++)
                work[rng_range((uint32_t)len)] ^=
                    (uint8_t)(1u << rng_range(8));
        } else if (mode == 2) {
            /* truncate to a random prefix */
            len = rng_range((uint32_t)len + 1);
        } else {
            /* extend with garbage up to the burst cap */
            size_t extra = rng_range(512);
            if (len + extra > WORK_CAP)
                extra = WORK_CAP - len;
            for (size_t e = 0; e < extra; e++)
                work[len + e] = (uint8_t)rng_next();
            len += extra;
        }
        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "iter %ld %s AU %zu mode %u len %zu", it, sf->name, a,
                 mode, len);
        fail = exercise(dec, work, len);
        exercised++;

        /* Periodically prove a clean AU still decodes (no poisoned
         * state). */
        if (!fail && (it & 0x3FF) == 0x3FF) {
            snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                     "recovery after iter %ld", it);
            AiusPcmBlock block;
            AiusStatus st = aius_eac3_decode_access_unit(
                dec, sf->data + sf->starts[0], sf->lens[0], &block);
            if (st != AIUS_OK || !block_is_sane(&block)) {
                fprintf(stderr, "FAIL: recovery decode status %d: %s\n",
                        (int)st, g_decode_ctx);
                fail = 1;
            }
            exercised++;
        }
    }

    for (int i = 0; i < nseeds; i++)
        free(seeds[i].data);
    aius_eac3_decoder_destroy(dec);
    free(work);

    if (fail) {
        printf("RESULT: FAILED after %ld decode(s)\n", exercised);
        return 1;
    }
    printf("ok %ld decodes, worst %.1f ms (%s)\n", exercised,
           (double)g_max_decode_ns / 1e6, g_max_decode_ctx);
    printf("RESULT: all checks passed\n");
    return 0;
}
