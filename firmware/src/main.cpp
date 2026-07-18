/* Aius capability firmware — DTS Core decode footprint harness (gate phase P4).
 *
 * At startup this provisions a statically-reserved arena, creates the arena-backed
 * AiusDtsDecoder (the no-heap path), decodes one compiled-in real 5.1 DTS Core
 * access unit, reads AiusMetrics back, and prints a one-line result plus the
 * metrics over USB Serial. Its purpose is to force the linker to pull in the ENTIRE
 * real decode pipeline — the AiusDtsDecoder adapter, the vendored dcadec fixed-point
 * core, the arena, and the allocation shim — so the P4 per-bank memory report
 * reflects the true firmware footprint.
 *
 * This phase cross-compiles + links + sizes only; the on-Teensy run and the USB
 * capability protocol are gate phases P5/P6. No hardware is required to build it.
 */

#include <Arduino.h>

extern "C" {
#include "aius_arena.h"
#include "aius_codec.h"
#include "aius_dts.h"
}

#include "dts_frame_surround51.h"

/* Static arena for the decoder. Sized from the measured 5.1 host high-water
 * (60864 bytes for a 512-sample-per-frame 5.1 core decode) rounded up to 96 KiB
 * for headroom, matching the dts-arena host suite. Uninitialised static storage,
 * so on the Teensy 4.1 it lands in DTCM (RAM1); see tools/memory for the bank
 * accounting. 16-byte aligned to satisfy the shim's allocation alignment. */
#define AIUS_ARENA_BYTES (96u * 1024u)
static uint8_t g_arena_mem[AIUS_ARENA_BYTES] __attribute__((aligned(16)));

static AiusArena g_arena;
static AiusDtsDecoder *g_dec;
static AiusPcmBlock g_block;
static AiusMetrics g_metrics;
static AiusStatus g_status = AIUS_INVALID_ARG;
static bool g_ran;

static const char *status_name(AiusStatus s)
{
    switch (s) {
    case AIUS_OK:          return "AIUS_OK";
    case AIUS_NEED_MORE:   return "AIUS_NEED_MORE";
    case AIUS_UNSUPPORTED: return "AIUS_UNSUPPORTED";
    case AIUS_MALFORMED:   return "AIUS_MALFORMED";
    case AIUS_TRUNCATED:   return "AIUS_TRUNCATED";
    case AIUS_NO_SPACE:    return "AIUS_NO_SPACE";
    case AIUS_INVALID_ARG: return "AIUS_INVALID_ARG";
    default:               return "AIUS_?";
    }
}

/* Run the full pipeline once and capture status, block, and metrics. */
static void run_decode(void)
{
    if (aius_arena_init(&g_arena, g_arena_mem, sizeof(g_arena_mem)) != 0) {
        g_status = AIUS_NO_SPACE;
        g_ran = true;
        return;
    }
    g_status = aius_dts_decoder_create_arena(&g_dec, &g_arena);
    if (g_status != AIUS_OK) {
        g_ran = true;
        return;
    }
    g_status = aius_dts_decode_access_unit(g_dec, aius_dts_frame_surround51,
                                           aius_dts_frame_surround51_len, &g_block);
    aius_dts_get_metrics(g_dec, &g_metrics);
    g_ran = true;
}

void setup(void)
{
    Serial.begin(115200);
    /* Bounded wait so an un-hosted board still proceeds (never flashed this phase). */
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000u) {
        /* spin */
    }
    run_decode();
}

void loop(void)
{
    if (g_ran) {
        Serial.print("AIUS DTS P4: status=");
        Serial.print(status_name(g_status));
        if (g_status == AIUS_OK) {
            Serial.print(" channels=");
            Serial.print(g_block.channels);
            Serial.print(" frames=");
            Serial.print(g_block.frames);
            Serial.print(" rate=");
            Serial.print(g_block.sample_rate_hz);
            Serial.print(" mask=0x");
            Serial.print(g_block.channel_mask, HEX);
        }
        Serial.print(" | arena_high_water=");
        Serial.print(g_metrics.arena_high_water);
        Serial.print("B alloc_count=");
        Serial.print(g_metrics.alloc_count);
        Serial.print(" arena_cap=");
        Serial.print((uint32_t)sizeof(g_arena_mem));
        Serial.println("B");
    }
    delay(2000);
}
