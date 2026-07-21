#ifndef AIUS_CAPABILITY_H
#define AIUS_CAPABILITY_H

#include <stddef.h>
#include <stdint.h>

#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Aius USB capability protocol (capability-gate phase P5, host side).
 *
 * A small versioned binary protocol carried over USB-Serial between a host
 * runner and the Teensy 4.1 "capability" firmware. Framework-neutral portable
 * C: the identical wire codec compiles for host tests, the in-process simulated
 * device, and (P6) the real board. No Arduino, no libc heap on the device path.
 *
 * Wire frame (little-endian throughout):
 *
 *   offset  size  field
 *   0       4     magic "AIUS"
 *   4       1     version (AIUS_CAP_VERSION)
 *   5       1     msg_type (AiusCapMsg)
 *   6       2     sequence
 *   8       4     payload_length (<= AIUS_CAP_MAX_PAYLOAD)
 *   12      N     payload
 *   12+N    4     CRC-32/ISO-HDLC over bytes [0 .. 12+N) (magic..payload)
 *
 * Framing is length-based; after corruption the decoder performs a bounded
 * magic rescan (advance one byte past a spurious magic and resynchronise).
 * The decoder tolerates partial and coalesced transfers.
 */

#define AIUS_CAP_VERSION      1u
#define AIUS_CAP_HEADER_LEN   12u
#define AIUS_CAP_CRC_LEN      4u
#define AIUS_CAP_MAX_PAYLOAD  65536u
#define AIUS_CAP_MAX_FRAME    (AIUS_CAP_HEADER_LEN + AIUS_CAP_MAX_PAYLOAD + AIUS_CAP_CRC_LEN)

/* Magic bytes, in wire order. */
#define AIUS_CAP_MAGIC0 0x41u /* 'A' */
#define AIUS_CAP_MAGIC1 0x49u /* 'I' */
#define AIUS_CAP_MAGIC2 0x55u /* 'U' */
#define AIUS_CAP_MAGIC3 0x53u /* 'S' */

/* Message types. Commands are host->device (0x01..0x3f); responses are
 * device->host (0x80..0xbf). The high bit distinguishes direction. */
typedef enum {
	/* Commands */
	AIUS_CAP_CMD_IDENTIFY      = 0x01,
	AIUS_CAP_CMD_RESET_METRICS = 0x02,
	AIUS_CAP_CMD_SUBMIT_FRAME  = 0x03, /* payload: one raw DTS Core access unit */
	AIUS_CAP_CMD_REQUEST_PCM   = 0x04, /* payload: empty; returns last decoded PCM */
	AIUS_CAP_CMD_RUN_CORPUS    = 0x05, /* payload: [u32 len][au]... batch decode */
	AIUS_CAP_CMD_RUN_SOAK      = 0x06, /* payload: [u32 iters][u32 len][au]... */
	AIUS_CAP_CMD_REBOOT_BOOTLOADER = 0x07, /* payload: empty; enter HalfKay */

	/* Responses */
	AIUS_CAP_RSP_ACK          = 0x80,
	AIUS_CAP_RSP_NACK         = 0x81,
	AIUS_CAP_RSP_CAPABILITIES = 0x82,
	AIUS_CAP_RSP_FRAME_RESULT = 0x83,
	AIUS_CAP_RSP_PCM_DATA     = 0x84,
	AIUS_CAP_RSP_DIAGNOSTIC   = 0x85,
	AIUS_CAP_RSP_SUMMARY      = 0x86
} AiusCapMsg;

/* Typed protocol/decode error codes, carried by NACK and diagnostic responses
 * and reported by the framer. */
typedef enum {
	AIUS_CAP_ERR_NONE = 0,
	AIUS_CAP_ERR_BAD_VERSION,
	AIUS_CAP_ERR_BAD_CRC,
	AIUS_CAP_ERR_UNKNOWN_TYPE,
	AIUS_CAP_ERR_DUP_SEQUENCE,
	AIUS_CAP_ERR_PAYLOAD_TOO_BIG,
	AIUS_CAP_ERR_MALFORMED_PAYLOAD,
	AIUS_CAP_ERR_TIMEOUT,
	AIUS_CAP_ERR_NO_PCM,     /* request-PCM before any successful decode */
	AIUS_CAP_ERR_DECODE,     /* decode failed; detail carries the AiusStatus */
	AIUS_CAP_ERR_INTERNAL
} AiusCapError;

/* Capability flag bits advertised in the capabilities response. */
enum {
	AIUS_CAP_FLAG_DTS_CORE = 1u << 0, /* decodes raw DTS Core access units */
	AIUS_CAP_FLAG_ARENA    = 1u << 1, /* no-heap arena decode path */
	AIUS_CAP_FLAG_SOAK     = 1u << 2  /* supports run-soak */
};

/* ---- Little-endian scalar helpers (unaligned-safe, byte-by-byte) ---- */

static inline void aius_cap_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static inline void aius_cap_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline void aius_cap_put_u64(uint8_t *p, uint64_t v)
{
	aius_cap_put_u32(p, (uint32_t)(v & 0xffffffffu));
	aius_cap_put_u32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

static inline uint16_t aius_cap_get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t aius_cap_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t aius_cap_get_u64(const uint8_t *p)
{
	return (uint64_t)aius_cap_get_u32(p) |
	       ((uint64_t)aius_cap_get_u32(p + 4) << 32);
}

/* ---- CRC-32/ISO-HDLC (poly 0xEDB88320, init/xorout 0xFFFFFFFF, reflected) ---- */

uint32_t aius_cap_crc32(const uint8_t *data, size_t len);

/* ---- Frame encoding ---- */

/* Encode one full frame (header + payload + CRC) into `dst`. Returns the total
 * frame length, or 0 if `dst_cap` is too small or `payload_len` exceeds
 * AIUS_CAP_MAX_PAYLOAD. `payload` may be NULL only when `payload_len` is 0. */
size_t aius_cap_encode(uint8_t *dst, size_t dst_cap, uint8_t msg_type,
                       uint16_t sequence, const uint8_t *payload,
                       uint32_t payload_len);

/* ---- Frame decoding (streaming framer) ---- */

typedef enum {
	AIUS_CAP_FRAME_NONE = 0, /* need more bytes */
	AIUS_CAP_FRAME_OK,       /* a valid frame is available in `out` */
	AIUS_CAP_FRAME_ERROR     /* a corruption/protocol event; see out->error */
} AiusCapFrameEvent;

typedef struct {
	uint8_t        version;
	uint8_t        msg_type;
	uint16_t       sequence;
	const uint8_t *payload;     /* into framer storage; valid until next call */
	uint32_t       payload_len;
	AiusCapError   error;       /* meaningful when event == AIUS_CAP_FRAME_ERROR */
} AiusCapFrame;

typedef struct {
	uint8_t buf[AIUS_CAP_MAX_FRAME];
	size_t  len;             /* bytes buffered */
	size_t  pending_consume; /* bytes to drop at the start of the next next() */
} AiusCapFramer;

void aius_cap_framer_init(AiusCapFramer *fr);

/* Append up to `len` bytes; returns the number accepted (< len when the buffer
 * is full, in which case the caller must drain with aius_cap_framer_next before
 * feeding more). One max-size frame always fits once the buffer is drained. */
size_t aius_cap_framer_feed(AiusCapFramer *fr, const uint8_t *data, size_t len);

/* Extract the next event. Returns AIUS_CAP_FRAME_NONE when more bytes are
 * needed. On AIUS_CAP_FRAME_OK, `out->payload` references framer storage that
 * stays valid only until the next feed/next call. */
AiusCapFrameEvent aius_cap_framer_next(AiusCapFramer *fr, AiusCapFrame *out);

/* Bytes currently buffered but not yet forming a complete frame (a partial
 * transfer in flight). Used by host-side timeout detection. */
size_t aius_cap_framer_pending(const AiusCapFramer *fr);

/* ---- Typed payload structs + codecs (all little-endian, fixed layout) ---- */

#define AIUS_CAP_CAP_NAME_LEN 16u
#define AIUS_CAP_CAPABILITIES_LEN 32u
#define AIUS_CAP_FRAME_RESULT_LEN 24u
#define AIUS_CAP_PCM_HEADER_LEN 16u
#define AIUS_CAP_SUMMARY_LEN 36u
#define AIUS_CAP_DIAGNOSTIC_HEADER_LEN 10u
#define AIUS_CAP_NACK_LEN 4u

typedef struct {
	uint8_t  abi_version;
	uint8_t  proto_version;
	uint16_t max_channels;
	uint32_t max_payload;
	uint32_t capability_flags;
	char     name[AIUS_CAP_CAP_NAME_LEN]; /* null-padded */
} AiusCapCapabilities;

typedef struct {
	uint8_t  status;          /* AiusStatus of the decode */
	uint16_t channels;
	uint32_t frames;
	uint32_t sample_rate_hz;
	uint32_t channel_mask;
	uint32_t arena_high_water;
	uint32_t alloc_count;
} AiusCapFrameResult;

typedef struct {
	uint16_t channels;
	uint32_t frames;
	uint32_t sample_rate_hz;
	uint32_t channel_mask;
	/* channel-major planar int32 LE samples follow the 16-byte header */
} AiusCapPcmHeader;

typedef struct {
	uint32_t frames_submitted;
	uint32_t frames_ok;
	uint32_t frames_failed;
	uint32_t iterations;
	uint64_t total_samples;
	uint32_t peak_arena_high_water;
	uint32_t alloc_violations; /* steady-state allocations observed (want 0) */
	uint8_t  last_status;      /* AiusStatus of the final decode */
} AiusCapSummary;

typedef struct {
	uint8_t  error_code;    /* AiusCapError */
	uint8_t  detail_status; /* AiusStatus, when error_code == DECODE */
	uint32_t context_index; /* frame index or soak iteration */
	/* a u16-length-prefixed message string follows the 10-byte header */
} AiusCapDiagnostic;

typedef struct {
	uint8_t  error_code;         /* AiusCapError */
	uint16_t offending_sequence;
} AiusCapNack;

/* Each put_* returns bytes written (0 on insufficient capacity). Each get_*
 * returns 1 on success, 0 on a malformed/short payload. */
size_t aius_cap_put_capabilities(uint8_t *dst, size_t cap, const AiusCapCapabilities *c);
int    aius_cap_get_capabilities(const uint8_t *p, uint32_t len, AiusCapCapabilities *c);

size_t aius_cap_put_frame_result(uint8_t *dst, size_t cap, const AiusCapFrameResult *r);
int    aius_cap_get_frame_result(const uint8_t *p, uint32_t len, AiusCapFrameResult *r);

size_t aius_cap_put_pcm_header(uint8_t *dst, size_t cap, const AiusCapPcmHeader *h);
int    aius_cap_get_pcm_header(const uint8_t *p, uint32_t len, AiusCapPcmHeader *h);

size_t aius_cap_put_summary(uint8_t *dst, size_t cap, const AiusCapSummary *s);
int    aius_cap_get_summary(const uint8_t *p, uint32_t len, AiusCapSummary *s);

/* Diagnostic carries a trailing message string (may be NULL/0). */
size_t aius_cap_put_diagnostic(uint8_t *dst, size_t cap, const AiusCapDiagnostic *d,
                               const char *message, uint16_t message_len);
int    aius_cap_get_diagnostic(const uint8_t *p, uint32_t len, AiusCapDiagnostic *d,
                               const char **message_out, uint16_t *message_len_out);

size_t aius_cap_put_nack(uint8_t *dst, size_t cap, const AiusCapNack *n);
int    aius_cap_get_nack(const uint8_t *p, uint32_t len, AiusCapNack *n);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_CAPABILITY_H */
