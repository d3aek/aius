/* Malformed / truncated / fuzz robustness test for the Aius AC-3 adapter
 * (aius-ac3-support.md phase 1) — the dts-fuzz shape ported to liba52's
 * threat model.
 *
 * The fork validates both AC-3 CRC regions before parsing and has a fail-closed
 * byte end with no safety pad. Mutated real AUs have both CRCs repaired so the
 * fuzz stream still reaches mantissa/exponent/bit-allocation logic; random
 * core reach and every load the reader bound suppresses are counted. ASan
 * watches the one-frame scratch allocation on every feed.
 * Properties:
 *   1. Never crashes and never reads out of bounds (sanitizers are the
 *      oracle).
 *   2. Every call returns a typed AiusStatus; an OK decode yields a sane
 *      block (channels <= 6, 1536 frames, planes present).
 *   3. A non-OK status never emits fresh PCM (caller's block poisoned before
 *      each call); after any abuse a clean AU still decodes (no state rot).
 *   4. TERMINATION: every decode call runs under a wall-clock bound (1 s
 *      soft) and an alarm() hard backstop (8 s, the board's watchdog) that
 *      names the offending input — the defect-31 oracle shape.
 *   5. COVERAGE: the shipped random stream keeps at least 16,000 CRC-valid
 *      feeds reaching a52_frame/a52_block.
 *
 * Phase 1 is a deterministic seam hunt (truncated AU spliced with the next
 * AU byte-exact at the loop seam, step-16 across adjacent pairs, headless
 * tails, bare truncations); phase 2 is random corruption with a planted
 * 2-byte 0x0B77 sync. Seed AUs are walked by a52_syncinfo length — a 2-byte
 * sync scan would false-split on payload bytes.
 *
 * Usage: fuzz_a52 <vectors_dir> [iterations]
 * Exit 0 on pass, 1 on any contract violation.
 */

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "a52.h" /* a52_syncinfo only: seed AU walking */
#include "a52_crc_test.h"
#include "aius_a52.h"

#ifndef AIUS_A52_BOUND_PROBE
#error "fuzz_a52 requires AIUS_A52_BOUND_PROBE"
#endif

#define MAX_SEEDS 4
#define MAX_AUS 16
#define WORK_CAP (16u * 1024u)

#define DECODE_HARD_BOUND_S 8u
#define DECODE_SOFT_BOUND_NS 1000000000ull
#define SHIPPED_FUZZ_ITERATIONS 50000L
#define SHIPPED_OK_COUNT 5616L
#define SHIPPED_REJECT_COUNT 60184L
#define SHIPPED_FILL_COUNT UINT32_C(3889955)
#define SHIPPED_SUPPRESSED_FILL_COUNT UINT32_C(54909)
#define SHIPPED_MIN_CORE_REACH 16000L

struct seed_file {
    const char *name;
    uint8_t *data;
    size_t size;
    size_t starts[MAX_AUS];
    size_t lens[MAX_AUS];
    size_t naus;
};

static uint8_t *read_file(const char *path, size_t *size_out)
{
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

static size_t collect_aus(uint8_t *data, size_t size,
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

/* ---- termination oracle (the defect-31 shape from fuzz_dts.c) ---------- */

static char g_decode_ctx[192];

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "\nFAIL: decode exceeded the hard bound (hung): ";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    r = write(STDERR_FILENO, g_decode_ctx, strlen(g_decode_ctx));
    r = write(STDERR_FILENO, "\n", 1);
    (void)r;
    _exit(1);
}

static uint64_t g_max_decode_ns;
static char g_max_decode_ctx[sizeof(g_decode_ctx)];

static AiusStatus timed_decode(AiusA52Decoder *dec, const uint8_t *buf,
                               size_t len, AiusPcmBlock *block,
                               uint64_t *elapsed_ns_out)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    alarm(DECODE_HARD_BOUND_S);
    AiusStatus st = aius_a52_decode_access_unit(dec, buf, len, block);
    alarm(0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                  (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
    if (ns > g_max_decode_ns) {
        g_max_decode_ns = ns;
        memcpy(g_max_decode_ctx, g_decode_ctx, sizeof(g_max_decode_ctx));
    }
    *elapsed_ns_out = ns;
    return st;
}

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
    }
    return 0;
}

static int block_is_sane(const AiusPcmBlock *b)
{
    if (b->channels == 0 || b->channels > AIUS_MAX_CHANNELS)
        return 0;
    if (b->frames != AIUS_A52_FRAME_SAMPLES)
        return 0;
    for (uint16_t c = 0; c < b->channels; c++)
        if (b->planes[c] == NULL)
            return 0;
    return 1;
}

static void poison_block(AiusPcmBlock *b)
{
    memset(b, 0xA5, sizeof(*b));
}

static int checked_feed(AiusA52Decoder *dec, const uint8_t *buf, size_t len,
                        long *ok_count, long *reject_count,
                        long *core_reach_count)
{
    AiusPcmBlock block;
    poison_block(&block);
    uint32_t fills_before = 0;
    if (core_reach_count) {
        uint32_t suppressed = 0, mismatches = 0;
        if (aius_a52_get_bound_probe(dec, &fills_before, &suppressed,
                                     &mismatches) != AIUS_OK)
            return 1;
    }
    uint64_t ns = 0;
    AiusStatus st = timed_decode(dec, buf, len, &block, &ns);
    if (core_reach_count) {
        uint32_t fills_after = 0, suppressed = 0, mismatches = 0;
        if (aius_a52_get_bound_probe(dec, &fills_after, &suppressed,
                                     &mismatches) != AIUS_OK)
            return 1;
        if (fills_after != fills_before)
            (*core_reach_count)++;
    }
    if (ns > DECODE_SOFT_BOUND_NS) {
        fprintf(stderr, "FAIL: decode took %.1f ms (soft bound): %s\n",
                (double)ns / 1e6, g_decode_ctx);
        return 1;
    }
    if (!status_is_typed(st)) {
        fprintf(stderr, "FAIL: untyped status %d: %s\n", (int)st, g_decode_ctx);
        return 1;
    }
    if (st == AIUS_OK) {
        if (!block_is_sane(&block)) {
            fprintf(stderr, "FAIL: OK with insane block: %s\n", g_decode_ctx);
            return 1;
        }
        volatile int32_t sink = 0;
        for (uint16_t c = 0; c < block.channels; c++)
            for (uint32_t n = 0; n < block.frames; n++)
                sink ^= block.planes[c][n];
        (void)sink;
        (*ok_count)++;
    } else {
        (*reject_count)++;
    }
    return 0;
}

/* Deterministic seam hunt over one seed file (the fuzz_dts seam shapes). */
static int seam_hunt_file(AiusA52Decoder *dec, const struct seed_file *sf,
                          uint8_t *buf, size_t buf_cap,
                          long *ok_count, long *reject_count, long *feeds)
{
    if (sf->naus < 2)
        return 0;
    const uint8_t *last = sf->data + sf->starts[sf->naus - 1];
    const uint8_t *head = sf->data + sf->starts[0];
    size_t last_len = sf->lens[sf->naus - 1];
    size_t head_len = sf->lens[0];

    for (size_t k = 1; k < last_len; k++) {
        if (k + head_len > buf_cap)
            break;
        memcpy(buf, last, k);
        memcpy(buf + k, head, head_len);
        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "%s S1 loop-seam k=%zu (of %zu) + AU0", sf->name, k,
                 last_len);
        if (checked_feed(dec, buf, k + head_len, ok_count, reject_count,
                         NULL))
            return 1;
        (*feeds)++;
    }
    for (size_t k = 1; k < head_len; k++) {
        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "%s S2 truncate AU0 at k=%zu (of %zu)", sf->name, k,
                 head_len);
        if (checked_feed(dec, head, k, ok_count, reject_count, NULL))
            return 1;
        (*feeds)++;
    }
    for (size_t i = 0; i + 1 < sf->naus; i++) {
        const uint8_t *a = sf->data + sf->starts[i];
        const uint8_t *b = sf->data + sf->starts[i + 1];
        for (size_t k = 1; k < sf->lens[i]; k += 16) {
            if (k + sf->lens[i + 1] <= buf_cap) {
                memcpy(buf, a, k);
                memcpy(buf + k, b, sf->lens[i + 1]);
                snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                         "%s S1 splice AU%zu k=%zu + AU%zu", sf->name, i, k,
                         i + 1);
                if (checked_feed(dec, buf, k + sf->lens[i + 1], ok_count,
                                 reject_count, NULL))
                    return 1;
                (*feeds)++;
            }
            size_t tail = sf->lens[i] - k;
            if (tail + sf->lens[i + 1] <= buf_cap) {
                memcpy(buf, a + k, tail);
                memcpy(buf + tail, b, sf->lens[i + 1]);
                snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                         "%s S3 tail AU%zu from k=%zu + AU%zu", sf->name, i,
                         k, i + 1);
                if (checked_feed(dec, buf, tail + sf->lens[i + 1], ok_count,
                                 reject_count, NULL))
                    return 1;
                (*feeds)++;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vectors_dir> [iterations]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    long iters = (argc >= 3) ? strtol(argv[2], NULL, 10) :
        SHIPPED_FUZZ_ITERATIONS;
    if (iters <= 0)
        iters = SHIPPED_FUZZ_ITERATIONS;

    static const char *names[] = {
        "stereo_tones", "stereo_impulse", "surround51_impulse",
        "surround51_ident",
    };

    struct seed_file seeds[MAX_SEEDS];
    int nseeds = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.ac3", dir, names[i]);
        size_t sz = 0;
        uint8_t *data = read_file(path, &sz);
        if (!data)
            continue;
        struct seed_file *sf = &seeds[nseeds];
        sf->name = names[i];
        sf->data = data;
        sf->size = sz;
        sf->naus = collect_aus(data, sz, sf->starts, sf->lens, MAX_AUS);
        if (sf->naus >= 1)
            nseeds++;
        else
            free(data);
    }
    if (nseeds == 0) {
        fprintf(stderr, "FAIL: no usable seed AUs in %s\n", dir);
        return 1;
    }

    printf("AC-3 robustness/fuzz: %ld iterations over %d seed file(s)\n",
           iters, nseeds);

    AiusA52Decoder *dec = NULL;
    if (aius_a52_decoder_create(&dec) != AIUS_OK) {
        fprintf(stderr, "FAIL: decoder create\n");
        return 1;
    }

    uint8_t *work = malloc(WORK_CAP);
    if (!work) {
        aius_a52_decoder_destroy(dec);
        return 1;
    }

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
    }

    /* Explicit argument guards. */
    {
        AiusPcmBlock b;
        poison_block(&b);
        if (aius_a52_decode_access_unit(dec, NULL, 16, &b) !=
                AIUS_INVALID_ARG ||
            aius_a52_decode_access_unit(dec, work, 0, &b) !=
                AIUS_INVALID_ARG ||
            aius_a52_decode_access_unit(dec, work, 8, NULL) !=
                AIUS_INVALID_ARG ||
            aius_a52_decode_access_unit(NULL, work, 8, &b) !=
                AIUS_INVALID_ARG) {
            fprintf(stderr, "FAIL: argument guards not enforced\n");
            return 1;
        }
    }

    long ok_count = 0, reject_count = 0, core_reach_count = 0;
    long crc_repaired_count = 0;
    int fail = 0;

    /* Phase 1: deterministic seam hunt. */
    {
        long feeds = 0;
        for (int i = 0; i < nseeds && !fail; i++)
            fail = seam_hunt_file(dec, &seeds[i], work, WORK_CAP, &ok_count,
                                  &reject_count, &feeds);
        if (!fail) {
            AiusPcmBlock block;
            poison_block(&block);
            snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                     "post-seam recovery decode of %s AU0", seeds[0].name);
            uint64_t ns = 0;
            AiusStatus st = timed_decode(dec, seeds[0].data + seeds[0].starts[0],
                                         seeds[0].lens[0], &block, &ns);
            if (st != AIUS_OK || !block_is_sane(&block)) {
                fprintf(stderr,
                        "FAIL: decoder did not recover after the seam hunt "
                        "(st=%d)\n", (int)st);
                fail = 1;
            }
        }
        printf("  seam hunt: %ld seam-shaped feeds, all terminated\n", feeds);
    }

    for (long it = 0; it < iters && !fail; it++) {
        const struct seed_file *sf = &seeds[rng_range((uint32_t)nseeds)];
        size_t au = rng_range((uint32_t)sf->naus);
        const uint8_t *seed = sf->data + sf->starts[au];
        size_t seed_len = sf->lens[au];
        size_t len = 0;
        int strategy = (int)rng_range(7);

        switch (strategy) {
        case 0: /* bit-flip a copy of a real AU */
            len = seed_len;
            memcpy(work, seed, len);
            {
                int flips = 1 + (int)rng_range(24);
                for (int k = 0; k < flips; k++) {
                    size_t bit = rng_next() % (len * 8);
                    work[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
                }
            }
            break;
        case 1: /* truncation */
            len = 1 + rng_range((uint32_t)seed_len);
            if (len > seed_len)
                len = seed_len;
            memcpy(work, seed, len);
            break;
        case 2: /* corrupt the 2-byte sync word / header */
            len = seed_len;
            memcpy(work, seed, len);
            work[rng_range(7)] ^= (uint8_t)(1u + rng_range(255));
            break;
        case 3: /* random noise, no planted sync */
            len = 7 + rng_range(4096);
            for (size_t i = 0; i < len; i++)
                work[i] = (uint8_t)rng_next();
            break;
        case 4: /* random noise behind a valid 0x0B77 sync: the hostile
                 * payload the bounded reader must contain */
            len = 7 + rng_range(4096);
            for (size_t i = 0; i < len; i++)
                work[i] = (uint8_t)rng_next();
            work[0] = 0x0B;
            work[1] = 0x77;
            break;
        case 5: /* real AU with a random tail appended (oversized) */
            len = seed_len;
            memcpy(work, seed, len);
            {
                uint32_t extra = rng_range((uint32_t)(WORK_CAP - len));
                for (uint32_t i = 0; i < extra; i++)
                    work[len + i] = (uint8_t)rng_next();
                len += extra;
            }
            break;
        default: /* mutate a real AU in a random middle region */
            len = seed_len;
            memcpy(work, seed, len);
            {
                int edits = 1 + (int)rng_range(32);
                for (int k = 0; k < edits; k++)
                    work[rng_range((uint32_t)len)] = (uint8_t)rng_next();
            }
            break;
        }

        if (strategy == 0 || strategy == 2 || strategy == 6) {
            int flags = 0, sample_rate = 0, bit_rate = 0;
            int frame_size = a52_syncinfo(work, &flags, &sample_rate,
                                          &bit_rate);
            if (frame_size == (int)len && sample_rate == 48000) {
                if (!a52_test_crc_repair(work, len)) {
                    fprintf(stderr,
                            "FAIL: CRC repair at random iter %ld strat %d "
                            "len %zu\n",
                            it, strategy, len);
                    fail = 1;
                    break;
                }
                crc_repaired_count++;
            }
        }

        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "random iter %ld strat %d len %zu", it, strategy, len);
        if (checked_feed(dec, work, len, &ok_count, &reject_count,
                         &core_reach_count)) {
            fail = 1;
            break;
        }
    }

    /* Recovery: after all that abuse, a clean real AU still decodes OK. */
    if (!fail) {
        AiusPcmBlock block;
        poison_block(&block);
        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "final recovery decode of %s AU0", seeds[0].name);
        uint64_t ns = 0;
        AiusStatus st = timed_decode(dec, seeds[0].data + seeds[0].starts[0],
                                     seeds[0].lens[0], &block, &ns);
        if (st != AIUS_OK || !block_is_sane(&block)) {
            fprintf(stderr, "FAIL: decoder did not recover after fuzzing "
                            "(st=%d)\n", (int)st);
            fail = 1;
        }
    }

    uint32_t bound_fills = 0, suppressed_fills = 0, end_mismatches = 0;
    if (!fail &&
        (aius_a52_get_bound_probe(dec, &bound_fills, &suppressed_fills,
                                  &end_mismatches) != AIUS_OK ||
         bound_fills == 0 || suppressed_fills == 0 || end_mismatches != 0)) {
        fprintf(stderr,
                "FAIL: reader bound fills=%" PRIu32
                " suppressed=%" PRIu32 " end-mismatches=%" PRIu32 "\n",
                bound_fills, suppressed_fills, end_mismatches);
        fail = 1;
    }
    if (!fail && iters == SHIPPED_FUZZ_ITERATIONS &&
        (ok_count != SHIPPED_OK_COUNT ||
         reject_count != SHIPPED_REJECT_COUNT ||
         bound_fills != SHIPPED_FILL_COUNT ||
         suppressed_fills != SHIPPED_SUPPRESSED_FILL_COUNT)) {
        fprintf(stderr,
                "FAIL: shipped fuzz outcome %ld/%ld ok/rejected, "
                "%" PRIu32 "/%" PRIu32 " fills/suppressed; expected "
                "%ld/%ld and %" PRIu32 "/%" PRIu32 "\n",
                ok_count, reject_count, bound_fills, suppressed_fills,
                SHIPPED_OK_COUNT, SHIPPED_REJECT_COUNT,
                SHIPPED_FILL_COUNT, SHIPPED_SUPPRESSED_FILL_COUNT);
        fail = 1;
    }
    if (!fail && iters == SHIPPED_FUZZ_ITERATIONS &&
        core_reach_count < SHIPPED_MIN_CORE_REACH) {
        fprintf(stderr,
                "FAIL: only %ld/%ld random feeds reached the AC-3 core "
                "(floor %ld)\n",
                core_reach_count, iters, SHIPPED_MIN_CORE_REACH);
        fail = 1;
    }

    free(work);
    aius_a52_decoder_destroy(dec);
    for (int i = 0; i < nseeds; i++)
        free(seeds[i].data);

    if (fail) {
        printf("RESULT: FAILED\n");
        return 1;
    }
    printf("  %ld ok / %ld rejected, no crash / OOB / stale PCM\n",
           ok_count, reject_count);
    printf("  reader bound: %" PRIu32 " fills, %" PRIu32
           " suppressed, 0 end mismatches\n",
           bound_fills, suppressed_fills);
    printf("  CRC-valid core reach: %ld / %ld random feeds "
           "(%ld repaired)\n",
           core_reach_count, iters, crc_repaired_count);
    printf("  slowest decode: %.3f ms (%s)\n",
           (double)g_max_decode_ns / 1e6, g_max_decode_ctx);
    printf("RESULT: passed\n");
    return 0;
}
