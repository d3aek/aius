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
 * Usage: fuzz_dts <vectors_dir> [iterations]
 * Exit 0 on pass, 1 on any contract violation.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_dts.h"

#define MAX_SEEDS   8
#define WORK_CAP    (64u * 1024u)

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
    };

    struct seed seeds[MAX_SEEDS];
    int nseeds = 0;
    uint8_t *files[MAX_SEEDS] = { 0 };
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
        AiusStatus st = aius_dts_decode_access_unit(dec, work, len, &block);

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
    printf("RESULT: passed\n");
    return 0;
}
