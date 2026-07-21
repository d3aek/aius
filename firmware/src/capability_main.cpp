/* Aius P6 capability firmware over Teensy RawHID. */

#include <Arduino.h>
#include <string.h>

extern "C" {
#include "aius_arena.h"
#include "aius_cap_device.h"
#include "aius_capability.h"
}

/* RawHID on Teensy 4.1 is a pair of 64-byte reports. Keep the protocol
 * transport-neutral: the framer accepts coalesced reports and the emitter
 * fragments responses at the HID report boundary. */
#define AIUS_HID_REPORT_BYTES 64u
#define AIUS_ARENA_BYTES (96u * 1024u)

static uint8_t g_arena_mem[AIUS_ARENA_BYTES] __attribute__((aligned(16)));
static uint8_t g_hid_in[AIUS_HID_REPORT_BYTES];
static uint8_t g_hid_out[AIUS_HID_REPORT_BYTES];
static AiusArena g_arena;
static AiusCapDevice g_device;
static AiusCapFramer g_framer;

static void emit_hid(void *, const uint8_t *frame, size_t len)
{
    while (len != 0) {
        size_t chunk = len < AIUS_HID_REPORT_BYTES ? len : AIUS_HID_REPORT_BYTES;
        memset(g_hid_out, 0, sizeof(g_hid_out));
        memcpy(g_hid_out, frame, chunk);
        (void)RawHID.send(g_hid_out, AIUS_HID_REPORT_BYTES);
        frame += chunk;
        len -= chunk;
    }
}

static void drain_frames(void)
{
    AiusCapFrame frame;
    for (;;) {
        AiusCapFrameEvent event = aius_cap_framer_next(&g_framer, &frame);
        if (event == AIUS_CAP_FRAME_NONE)
            return;
        if (event == AIUS_CAP_FRAME_ERROR)
            aius_cap_device_handle_error(&g_device, &frame, emit_hid, NULL);
        else
            aius_cap_device_handle(&g_device, &frame, emit_hid, NULL);
    }
}

void setup(void)
{
    (void)aius_arena_init(&g_arena, g_arena_mem, sizeof(g_arena_mem));
    (void)aius_cap_device_init(&g_device, &g_arena);
    aius_cap_framer_init(&g_framer);
}

void loop(void)
{
    int received = RawHID.recv(g_hid_in, 0);
    if (received > 0) {
        (void)aius_cap_framer_feed(&g_framer, g_hid_in,
                                   (size_t)received > sizeof(g_hid_in)
                                       ? sizeof(g_hid_in)
                                       : (size_t)received);
        drain_frames();
    }
}
