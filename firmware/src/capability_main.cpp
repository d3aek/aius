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
/* DMAMEM is not cleared by the normal C runtime startup. Retain the last
 * breadcrumb long enough for the next boot to report a target fault. */
DMAMEM static volatile uint32_t g_stage;
static bool g_previous_crash;
static bool g_crash_reported;

static void stage(uint32_t value)
{
    g_stage = value;
    CrashReport.breadcrumb(1, value);
}

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
        stage(event == AIUS_CAP_FRAME_ERROR ? 13u : 14u);
        if (event == AIUS_CAP_FRAME_ERROR)
            aius_cap_device_handle_error(&g_device, &frame, emit_hid, NULL);
        else
            aius_cap_device_handle(&g_device, &frame, emit_hid, NULL);
    }
}

static void emit_crash_report(void)
{
    uint8_t payload[AIUS_CAP_DIAGNOSTIC_HEADER_LEN + 64];
    const char message[] = "[DEBUG-P6-CRASH] previous target reset";
    AiusCapDiagnostic diagnostic = {
        .error_code = (uint8_t)AIUS_CAP_ERR_INTERNAL,
        .detail_status = (uint8_t)AIUS_INVALID_ARG,
        .context_index = g_stage,
    };
    size_t payload_len = aius_cap_put_diagnostic(
        payload, sizeof(payload), &diagnostic, message, sizeof(message) - 1u);
    uint8_t frame[AIUS_CAP_HEADER_LEN + AIUS_CAP_DIAGNOSTIC_HEADER_LEN + 64u +
                  AIUS_CAP_CRC_LEN];
    size_t frame_len = aius_cap_encode(frame, sizeof(frame),
                                       AIUS_CAP_RSP_DIAGNOSTIC, 0, payload,
                                       (uint32_t)payload_len);
    emit_hid(NULL, frame, frame_len);
}

void setup(void)
{
    g_previous_crash = (bool)CrashReport;
    CrashReport.clear();
    stage(1u);
    (void)aius_arena_init(&g_arena, g_arena_mem, sizeof(g_arena_mem));
    stage(2u);
    (void)aius_cap_device_init(&g_device, &g_arena);
    stage(3u);
    aius_cap_framer_init(&g_framer);
    stage(4u);
}

void loop(void)
{
    stage(10u);
    int received = RawHID.recv(g_hid_in, 0);
    if (received > 0) {
        stage(11u);
        (void)aius_cap_framer_feed(&g_framer, g_hid_in,
                                   (size_t)received > sizeof(g_hid_in)
                                       ? sizeof(g_hid_in)
                                       : (size_t)received);
        stage(12u);
        if (g_previous_crash && !g_crash_reported) {
            g_crash_reported = true;
            emit_crash_report();
        }
        drain_frames();
    }
}
