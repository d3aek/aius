/* No-heap arena path + allocation-metrics test for the Aius DTS Core decoder
 * (capability-gate phase P3).
 *
 * Proves the target-shaped allocation contract on the host:
 *   1. An arena-backed decoder serves EVERY allocation — its own handle/scratch
 *      and dcadec's `ta` tree — from a caller-owned AiusArena. The allocation
 *      shim is linked with -Wl,--wrap=malloc,...,--wrap=free so any stray libc
 *      allocation on the decode path would be visible; while the arena is armed
 *      the shim never calls libc, and it counts every arena allocation.
 *   2. Warmup (create + first frame) allocates; every SUBSEQUENT frame decode
 *      allocates exactly zero times. This is the steady-state no-heap guarantee.
 *   3. aius_dts_get_metrics reports the arena high-water and per-decode count.
 *   4. A too-small arena fails create with AIUS_NO_SPACE rather than crashing.
 *
 * Usage: test_dts_arena <vectors_dir>
 * Exit 0 on pass, 1 on any failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_alloc_shim.h"
#include "aius_arena.h"
#include "aius_dts.h"

/* Arena reserved for the decoder. Sized from the measured 5.1 high-water
 * (60864 bytes for a 512-sample-per-frame 5.1 core decode) rounded up to 96 KiB
 * for headroom; the test asserts the real high-water fits inside. This figure
 * feeds the P4 memory budget. */
#define ARENA_BYTES (96u * 1024u)

static uint8_t g_arena_mem[ARENA_BYTES] __attribute__((aligned(16)));

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

/* Decode surround51_tones under an arena, asserting the no-heap steady state. */
static int run_arena_decode(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/surround51_tones.dts", dir);

    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "FAIL: cannot read %s\n", path);
        return 1;
    }

    size_t offsets[8192];
    int nframes = find_frames(data, size, offsets, 8192);
    if (nframes < 3) {
        fprintf(stderr, "FAIL: need >=3 frames to prove steady state, got %d\n",
                nframes);
        free(data);
        return 1;
    }

    AiusArena arena;
    if (aius_arena_init(&arena, g_arena_mem, sizeof(g_arena_mem)) != 0) {
        fprintf(stderr, "FAIL: arena init\n");
        free(data);
        return 1;
    }

    AiusDtsDecoder *dec = NULL;
    AiusStatus st = aius_dts_decoder_create_arena(&dec, &arena);
    if (st != AIUS_OK) {
        fprintf(stderr, "FAIL: create_arena status %d\n", st);
        free(data);
        return 1;
    }

    int fail = 0;
    AiusMetrics m;

    for (int fi = 0; fi < nframes; fi++) {
        size_t start = offsets[fi];
        size_t end = (fi + 1 < nframes) ? offsets[fi + 1] : size;
        AiusPcmBlock block;
        st = aius_dts_decode_access_unit(dec, data + start, end - start, &block);
        if (st != AIUS_OK) {
            fprintf(stderr, "FAIL: frame %d decode status %d\n", fi, st);
            fail = 1;
            break;
        }
        if (aius_dts_get_metrics(dec, &m) != AIUS_OK) {
            fprintf(stderr, "FAIL: get_metrics\n");
            fail = 1;
            break;
        }
        if (m.last_status != AIUS_OK) {
            fprintf(stderr, "FAIL: metrics.last_status %d\n", m.last_status);
            fail = 1;
            break;
        }
        if (block.channels != 6 ||
            block.channel_mask != (AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
                                   AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR)) {
            fprintf(stderr, "FAIL: frame %d layout ch=%u mask=0x%x\n",
                    fi, block.channels, block.channel_mask);
            fail = 1;
            break;
        }

        if (fi == 0) {
            /* Warmup frame: allocation is expected and must be non-zero. */
            printf("  warmup frame: alloc_count=%u arena_high_water=%u bytes\n",
                   m.alloc_count, m.arena_high_water);
            if (m.alloc_count == 0) {
                fprintf(stderr, "FAIL: warmup frame allocated nothing (shim not wired?)\n");
                fail = 1;
                break;
            }
        } else {
            /* Steady state: zero allocations, forever. */
            if (m.alloc_count != 0) {
                fprintf(stderr,
                        "FAIL: frame %d allocated %u time(s); steady state must be 0\n",
                        fi, m.alloc_count);
                fail = 1;
                break;
            }
        }
    }

    if (!fail) {
        aius_dts_get_metrics(dec, &m);
        printf("  steady-state alloc_count=0 across %d frames\n", nframes - 1);
        printf("  5.1 decode arena high-water: %u bytes (arena %u bytes)\n",
               m.arena_high_water, (unsigned)sizeof(g_arena_mem));
        if (m.arena_high_water >= sizeof(g_arena_mem)) {
            fprintf(stderr, "FAIL: high-water >= arena capacity\n");
            fail = 1;
        }
    }

    aius_dts_decoder_destroy(dec);
    /* The shim must be disarmed once the decoder is torn down. */
    if (aius_alloc_shim_arena() != NULL) {
        fprintf(stderr, "FAIL: shim left armed after destroy\n");
        fail = 1;
    }
    free(data);
    return fail;
}

/* A too-small arena must fail create cleanly (AIUS_NO_SPACE), not crash. */
static int run_tiny_arena(void)
{
    static uint8_t tiny[64] __attribute__((aligned(16)));
    AiusArena arena;
    aius_arena_init(&arena, tiny, sizeof(tiny));
    AiusDtsDecoder *dec = NULL;
    AiusStatus st = aius_dts_decoder_create_arena(&dec, &arena);
    if (st != AIUS_NO_SPACE) {
        fprintf(stderr, "FAIL: tiny arena create returned %d, want AIUS_NO_SPACE\n", st);
        aius_dts_decoder_destroy(dec);
        return 1;
    }
    /* Shim must be disarmed after a failed create. */
    if (aius_alloc_shim_arena() != NULL) {
        fprintf(stderr, "FAIL: shim left armed after failed create\n");
        return 1;
    }
    printf("  tiny arena -> AIUS_NO_SPACE (no crash)\n");
    return 0;
}

/* Invalid arguments are rejected without touching the shim. */
static int run_invalid_args(void)
{
    AiusArena arena;
    aius_arena_init(&arena, g_arena_mem, sizeof(g_arena_mem));
    if (aius_dts_decoder_create_arena(NULL, &arena) != AIUS_INVALID_ARG) {
        fprintf(stderr, "FAIL: NULL out not rejected\n");
        return 1;
    }
    AiusDtsDecoder *dec = NULL;
    if (aius_dts_decoder_create_arena(&dec, NULL) != AIUS_INVALID_ARG) {
        fprintf(stderr, "FAIL: NULL arena not rejected\n");
        return 1;
    }
    AiusMetrics m;
    if (aius_dts_get_metrics(NULL, &m) != AIUS_INVALID_ARG ||
        aius_dts_get_metrics(dec, NULL) != AIUS_INVALID_ARG) {
        fprintf(stderr, "FAIL: get_metrics NULL not rejected\n");
        return 1;
    }
    printf("  invalid args -> AIUS_INVALID_ARG\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vectors_dir>\n", argv[0]);
        return 2;
    }
    printf("DTS Core no-heap arena + metrics test\n");

    int failures = 0;
    failures += run_arena_decode(argv[1]);
    failures += run_tiny_arena();
    failures += run_invalid_args();

    if (failures) {
        printf("RESULT: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("RESULT: all checks passed\n");
    return 0;
}
