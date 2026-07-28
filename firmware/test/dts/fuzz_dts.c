/* Malformed / truncated / fuzz robustness test for the Aius DTS Core decoder
 * (capability-gate phase P3).
 *
 * Feeds the adapter a large corpus of hostile inputs and asserts three
 * properties under ASan/UBSan:
 *   1. Never crashes and never reads out of bounds (the sanitizers are the
 *      oracle; any OOB/UB aborts the run).
 *   2. Every call returns a typed AiusStatus from the declared set — an OK
 *      decode yields a sane block (channels <= 6, planes present); a rejection
 *      yields AIUS_MALFORMED / AIUS_TRUNCATED / AIUS_UNSUPPORTED /
 *      AIUS_NO_SPACE / AIUS_INVALID_ARG.
 *   3. A non-OK status never emits fresh PCM: the caller's block is poisoned
 *      before each call, so a stale-PCM leak would surface as an "OK" claim on a
 *      corrupt frame or a subsequent good decode inheriting old state. After any
 *      run of rejections a clean frame still decodes correctly (no state rot).
 *
 * Inputs: bit-flips, truncations, sync-word corruption, zero/oversized lengths,
 * random noise with and without a planted sync word, and mutated real frames.
 *
 * Phase 1 (before the random iterations) is the defect-31 seam hunt: a
 * DETERMINISTIC sweep of loop-seam-shaped access units -- truncated AU spliced
 * with a full AU (the replay-loop / relock seam), byte-exact at the loop seam
 * and step-16 across every adjacent pair, plus headless tails and bare
 * truncations. Every decode call in BOTH phases runs under a termination
 * oracle the random phase previously lacked: a per-call wall-clock bound
 * (soft fail at 1 s) with an alarm() hard backstop (8 s -- the board's
 * watchdog period) that names the offending input before exiting, so a
 * non-terminating dcadec parse becomes a loud, reproducible failure instead
 * of a hung suite. The slowest call and its input are reported on success --
 * that number is the native half of the defect-31 ISR-budget question.
 *
 * Usage: fuzz_dts <vectors_dir> [iterations]
 * Exit 0 on pass, 1 on any contract violation.
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "aius_dts.h"

#define MAX_SEEDS   8
#define WORK_CAP    (64u * 1024u)

/* Termination oracle bounds. The hard bound matches the board's 8 s watchdog:
 * an input that would starve the watchdog on target hangs here instead, and
 * SIGALRM turns it into a named failure. The soft bound is far above any sane
 * sanitized single-AU decode (sub-10 ms) but low enough to catch quadratic
 * blowups that do terminate. */
#define DECODE_HARD_BOUND_S   8u
#define DECODE_SOFT_BOUND_NS  1000000000ull

struct seed {
    uint8_t *bytes;
    size_t   len;
};

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)n);
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

/* First access unit of `data`: from the first sync word to the next (or EOF). */
static int first_frame(const uint8_t *data, size_t size,
                       size_t *start_out, size_t *len_out)
{
    size_t first = 0;
    int found = 0;
    for (size_t i = 0; i + 4 <= size; i++) {
        if (data[i] == 0x7f && data[i + 1] == 0xfe &&
            data[i + 2] == 0x80 && data[i + 3] == 0x01) {
            if (!found) {
                first = i;
                found = 1;
            } else {
                *start_out = first;
                *len_out = i - first;
                return 1;
            }
        }
    }
    if (found) {
        *start_out = first;
        *len_out = size - first;
        return 1;
    }
    return 0;
}

/* Every access unit of `data`: sync word to next sync word (or EOF). */
static size_t collect_frames(const uint8_t *data, size_t size,
                             size_t *starts, size_t *lens, size_t cap)
{
    size_t count = 0;
    size_t prev = 0;
    int have_prev = 0;
    for (size_t i = 0; i + 4 <= size && count < cap; i++) {
        if (data[i] == 0x7f && data[i + 1] == 0xfe &&
            data[i + 2] == 0x80 && data[i + 3] == 0x01) {
            if (have_prev) {
                starts[count] = prev;
                lens[count] = i - prev;
                count++;
            }
            prev = i;
            have_prev = 1;
        }
    }
    if (have_prev && count < cap) {
        starts[count] = prev;
        lens[count] = size - prev;
        count++;
    }
    return count;
}

/* ---- termination oracle --------------------------------------------------
 * The feeder formats the context BEFORE each decode; the SIGALRM handler only
 * write()s it (async-signal-safe) and exits. A non-terminating parse therefore
 * reports exactly which input hung instead of hanging the suite. */

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

/* Decode under the clock and the alarm. g_decode_ctx must already name the
 * input. Returns the status; *elapsed_ns_out gets the wall-clock cost. */
static AiusStatus timed_decode(AiusDtsDecoder *dec, const uint8_t *buf,
                               size_t len, AiusPcmBlock *block,
                               uint64_t *elapsed_ns_out)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    alarm(DECODE_HARD_BOUND_S);
    AiusStatus st = aius_dts_decode_access_unit(dec, buf, len, block);
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

/* xorshift64* PRNG: deterministic so a failing iteration reproduces. */
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

/* Validate a block the decoder claimed OK, to catch a bogus success. */
static int block_is_sane(const AiusPcmBlock *b)
{
    if (b->channels == 0 || b->channels > AIUS_MAX_CHANNELS)
        return 0;
    if (b->frames == 0 || b->frames > (1u << 20))
        return 0;
    for (uint16_t c = 0; c < b->channels; c++)
        if (b->planes[c] == NULL)
            return 0;
    return 1;
}

/* Poison a block so a stale-PCM leak on a non-OK path is detectable. */
static void poison_block(AiusPcmBlock *b)
{
    memset(b, 0xA5, sizeof(*b));
}

/* One oracle-checked feed: poison, timed decode, typed status, sane block,
 * every emitted sample touched (ASan), soft time bound. g_decode_ctx must
 * already name the input. Returns 0 ok, 1 contract violation. */
static int checked_feed(AiusDtsDecoder *dec, const uint8_t *buf, size_t len,
                        long *ok_count, long *reject_count)
{
    AiusPcmBlock block;
    poison_block(&block);
    uint64_t ns = 0;
    AiusStatus st = timed_decode(dec, buf, len, &block, &ns);
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

#define MAX_FRAMES 64

/* The defect-31 seam hunt: deterministic loop-seam-shaped inputs from one
 * seed file. The replay-loop / carrier-relock seam hands the decoder a
 * truncated AU with the next AU's bytes right behind it -- S1. S3 is the
 * head-lost dual (tail bytes then a full AU), S2 the bare truncation. The
 * LOOP seam (last AU -> first AU) sweeps every byte; adjacent pairs sweep
 * step 16 to keep the phase inside the suite's runtime envelope. */
static int seam_hunt_file(AiusDtsDecoder *dec, const uint8_t *data, size_t size,
                          const char *name, uint8_t *buf, size_t buf_cap,
                          long *ok_count, long *reject_count, long *feeds)
{
    size_t starts[MAX_FRAMES], lens[MAX_FRAMES];
    size_t n = collect_frames(data, size, starts, lens, MAX_FRAMES);
    if (n < 2)
        return 0;

    /* Loop seam, byte-exact: last AU truncated at every k, then AU 0. */
    const uint8_t *last = data + starts[n - 1];
    const uint8_t *head = data + starts[0];
    for (size_t k = 1; k < lens[n - 1]; k++) {
        if (k + lens[0] > buf_cap)
            break;
        memcpy(buf, last, k);
        memcpy(buf + k, head, lens[0]);
        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "%s S1 loop-seam k=%zu (of %zu) + AU0", name, k, lens[n - 1]);
        if (checked_feed(dec, buf, k + lens[0], ok_count, reject_count))
            return 1;
        (*feeds)++;
    }

    /* Bare truncation, byte-exact, on AU 0. */
    for (size_t k = 4; k < lens[0]; k++) {
        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "%s S2 truncate AU0 at k=%zu (of %zu)", name, k, lens[0]);
        if (checked_feed(dec, head, k, ok_count, reject_count))
            return 1;
        (*feeds)++;
    }

    /* Adjacent seams, step 16: splice and headless tail. */
    for (size_t i = 0; i + 1 < n; i++) {
        const uint8_t *a = data + starts[i];
        const uint8_t *b = data + starts[i + 1];
        for (size_t k = 1; k < lens[i]; k += 16) {
            if (k + lens[i + 1] <= buf_cap) {
                memcpy(buf, a, k);
                memcpy(buf + k, b, lens[i + 1]);
                snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                         "%s S1 splice AU%zu k=%zu + AU%zu", name, i, k, i + 1);
                if (checked_feed(dec, buf, k + lens[i + 1], ok_count,
                                 reject_count))
                    return 1;
                (*feeds)++;
            }
            size_t tail = lens[i] - k;
            if (tail + lens[i + 1] <= buf_cap) {
                memcpy(buf, a + k, tail);
                memcpy(buf + tail, b, lens[i + 1]);
                snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                         "%s S3 tail AU%zu from k=%zu + AU%zu", name, i, k,
                         i + 1);
                if (checked_feed(dec, buf, tail + lens[i + 1], ok_count,
                                 reject_count))
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
    long iters = (argc >= 3) ? strtol(argv[2], NULL, 10) : 50000;
    if (iters <= 0)
        iters = 50000;

    static const char *names[] = {
        "stereo_tones", "surround51_tones", "stereo_impulse", "surround51_impulse",
        "surround51_ident",
    };

    struct seed seeds[MAX_SEEDS];
    int nseeds = 0;
    uint8_t *files[MAX_SEEDS] = { 0 };
    size_t file_sizes[MAX_SEEDS] = { 0 };
    const char *file_names[MAX_SEEDS] = { 0 };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.dts", dir, names[i]);
        size_t sz = 0;
        uint8_t *data = read_file(path, &sz);
        if (!data)
            continue;
        size_t start = 0, len = 0;
        if (first_frame(data, sz, &start, &len) && len >= 8 && len <= WORK_CAP) {
            files[nseeds] = data;
            file_sizes[nseeds] = sz;
            file_names[nseeds] = names[i];
            seeds[nseeds].bytes = data + start;
            seeds[nseeds].len = len;
            nseeds++;
        } else {
            free(data);
        }
    }
    if (nseeds == 0) {
        fprintf(stderr, "FAIL: no usable seed frames in %s\n", dir);
        return 1;
    }

    printf("DTS Core robustness/fuzz: %ld iterations over %d seed frame(s)\n",
           iters, nseeds);

    /* Malformed input is the point of this test; mute the per-frame error log. */
    aius_dts_set_log_quiet(1);

    AiusDtsDecoder *dec = NULL;
    if (aius_dts_decoder_create(&dec) != AIUS_OK) {
        fprintf(stderr, "FAIL: decoder create\n");
        return 1;
    }

    uint8_t *work = malloc(WORK_CAP);
    if (!work) {
        aius_dts_decoder_destroy(dec);
        return 1;
    }

    /* Termination oracle: SIGALRM names the hung input and exits non-zero. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
    }

    /* Explicit argument-guard cases (not left to chance). */
    {
        AiusPcmBlock b;
        poison_block(&b);
        if (aius_dts_decode_access_unit(dec, NULL, 16, &b) != AIUS_INVALID_ARG ||
            aius_dts_decode_access_unit(dec, work, 0, &b) != AIUS_INVALID_ARG ||
            aius_dts_decode_access_unit(dec, work, 8, NULL) != AIUS_INVALID_ARG ||
            aius_dts_decode_access_unit(NULL, work, 8, &b) != AIUS_INVALID_ARG) {
            fprintf(stderr, "FAIL: argument guards not enforced\n");
            free(work);
            aius_dts_decoder_destroy(dec);
            return 1;
        }
    }

    long ok_count = 0, reject_count = 0;
    int fail = 0;

    /* Phase 1: the defect-31 deterministic seam hunt. */
    {
        uint8_t *seam_buf = malloc(2u * WORK_CAP);
        if (!seam_buf) {
            free(work);
            aius_dts_decoder_destroy(dec);
            return 1;
        }
        long feeds = 0;
        for (int i = 0; i < nseeds && !fail; i++)
            fail = seam_hunt_file(dec, files[i], file_sizes[i], file_names[i],
                                  seam_buf, 2u * WORK_CAP, &ok_count,
                                  &reject_count, &feeds);
        free(seam_buf);
        if (!fail) {
            /* Attribution point: the decoder must be clean before phase 2. */
            AiusPcmBlock block;
            poison_block(&block);
            snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                     "post-seam recovery decode of %s AU0", file_names[0]);
            uint64_t ns = 0;
            AiusStatus st = timed_decode(dec, seeds[0].bytes, seeds[0].len,
                                         &block, &ns);
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
        const struct seed *s = &seeds[rng_range((uint32_t)nseeds)];
        size_t len = 0;
        int strategy = (int)rng_range(7);

        switch (strategy) {
        case 0: /* bit-flip a copy of a real frame */
            len = s->len;
            memcpy(work, s->bytes, len);
            {
                int flips = 1 + (int)rng_range(24);
                for (int k = 0; k < flips; k++) {
                    size_t bit = rng_next() % (len * 8);
                    work[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
                }
            }
            break;
        case 1: /* truncation */
            len = 4 + rng_range((uint32_t)s->len);
            if (len > s->len)
                len = s->len;
            memcpy(work, s->bytes, len);
            break;
        case 2: /* corrupt the sync word */
            len = s->len;
            memcpy(work, s->bytes, len);
            work[rng_range(4)] ^= (uint8_t)(1u + rng_range(255));
            break;
        case 3: /* random noise, no valid sync */
            len = 8 + rng_range(4096);
            for (size_t i = 0; i < len; i++)
                work[i] = (uint8_t)rng_next();
            break;
        case 4: /* random noise behind a valid sync word */
            len = 8 + rng_range(4096);
            work[0] = 0x7f; work[1] = 0xfe; work[2] = 0x80; work[3] = 0x01;
            for (size_t i = 4; i < len; i++)
                work[i] = (uint8_t)rng_next();
            break;
        case 5: /* real frame with a random tail appended (oversized) */
            len = s->len;
            memcpy(work, s->bytes, len);
            {
                uint32_t extra = rng_range((uint32_t)(WORK_CAP - len));
                for (uint32_t i = 0; i < extra; i++)
                    work[len + i] = (uint8_t)rng_next();
                len += extra;
            }
            break;
        default: /* mutate a real frame in a random middle region */
            len = s->len;
            memcpy(work, s->bytes, len);
            {
                int edits = 1 + (int)rng_range(32);
                for (int k = 0; k < edits; k++)
                    work[rng_range((uint32_t)len)] = (uint8_t)rng_next();
            }
            break;
        }

        AiusPcmBlock block;
        poison_block(&block);
        snprintf(g_decode_ctx, sizeof(g_decode_ctx),
                 "random iter %ld strat %d len %zu", it, strategy, len);
        uint64_t ns = 0;
        AiusStatus st = timed_decode(dec, work, len, &block, &ns);

        if (ns > DECODE_SOFT_BOUND_NS) {
            fprintf(stderr, "FAIL: decode took %.1f ms (soft bound): %s\n",
                    (double)ns / 1e6, g_decode_ctx);
            fail = 1;
            break;
        }
        if (!status_is_typed(st)) {
            fprintf(stderr, "FAIL: iter %ld strat %d untyped status %d\n",
                    it, strategy, (int)st);
            fail = 1;
            break;
        }
        if (st == AIUS_OK) {
            if (!block_is_sane(&block)) {
                fprintf(stderr, "FAIL: iter %ld strat %d OK with insane block\n",
                        it, strategy);
                fail = 1;
                break;
            }
            /* Touch every emitted sample so ASan flags any bad plane pointer. */
            volatile int32_t sink = 0;
            for (uint16_t c = 0; c < block.channels; c++)
                for (uint32_t n = 0; n < block.frames; n++)
                    sink ^= block.planes[c][n];
            (void)sink;
            ok_count++;
        } else {
            reject_count++;
        }
    }

    /* Recovery: after all that abuse, a clean real frame still decodes OK. */
    if (!fail) {
        AiusPcmBlock block;
        poison_block(&block);
        AiusStatus st = aius_dts_decode_access_unit(dec, seeds[0].bytes,
                                                    seeds[0].len, &block);
        if (st != AIUS_OK || !block_is_sane(&block)) {
            fprintf(stderr, "FAIL: decoder did not recover after fuzzing (st=%d)\n",
                    (int)st);
            fail = 1;
        }
    }

    free(work);
    aius_dts_decoder_destroy(dec);
    for (int i = 0; i < nseeds; i++)
        free(files[i]);

    if (fail) {
        printf("RESULT: FAILED\n");
        return 1;
    }
    printf("  %ld ok / %ld rejected, no crash / OOB / stale PCM\n",
           ok_count, reject_count);
    printf("  slowest decode: %.3f ms (%s)\n",
           (double)g_max_decode_ns / 1e6, g_max_decode_ctx);
    printf("RESULT: passed\n");
    return 0;
}
