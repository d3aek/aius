#ifndef AIUS_NET_H
#define AIUS_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP control datagram. The wire format is deliberately independent of the
 * Arduino Ethernet library and native C struct layout.
 *
 *   0  4  magic "AUN1"
 *   4  1  protocol version
 *   5  1  message type
 *   6  2  flags, little-endian
 *   8  4  request ID, little-endian
 *  12  2  payload length, little-endian
 *  14  2  reserved, must be zero
 *  16  N  payload
 *  16+N 4 CRC-32/ISO-HDLC over header and payload
 */
#define AIUS_NET_VERSION 1u
#define AIUS_NET_HEADER_LEN 16u
#define AIUS_NET_CRC_LEN 4u
#define AIUS_NET_MAX_PAYLOAD 512u
#define AIUS_NET_MAX_FRAME (AIUS_NET_HEADER_LEN + AIUS_NET_MAX_PAYLOAD + AIUS_NET_CRC_LEN)

enum {
	AIUS_NET_CMD_DISCOVER = 0x01,
	AIUS_NET_CMD_GET_STATUS = 0x02,
	AIUS_NET_CMD_SET_MODE = 0x03,
	AIUS_NET_CMD_CLEAR_COUNTERS = 0x04,
	AIUS_NET_CMD_START_TEST = 0x05,
	AIUS_NET_CMD_STOP_TEST = 0x06,
	AIUS_NET_CMD_REBOOT = 0x07,
	AIUS_NET_CMD_GET_EVENTS = 0x08,
	AIUS_NET_CMD_SET_MIX = 0x09,
	AIUS_NET_RSP_ACK = 0x80,
	AIUS_NET_RSP_NACK = 0x81,
	AIUS_NET_RSP_DISCOVERY = 0x82,
	AIUS_NET_RSP_STATUS = 0x83,
	AIUS_NET_RSP_EVENTS = 0x84
};

enum {
	AIUS_NET_MODE_FLAT = 0,
	AIUS_NET_MODE_SPEECH = 1,
	AIUS_NET_MODE_BASS_REDUCED = 2
};

enum {
	AIUS_NET_MIX_STANDARD = 0,
	AIUS_NET_MIX_LFE_MINUS10 = 1,
	AIUS_NET_MIX_CLIPFREE = 2
};

typedef enum {
	AIUS_NET_OK = 0,
	AIUS_NET_ERR_CAPACITY,
	AIUS_NET_ERR_SHORT,
	AIUS_NET_ERR_MAGIC,
	AIUS_NET_ERR_VERSION,
	AIUS_NET_ERR_LENGTH,
	AIUS_NET_ERR_RESERVED,
	AIUS_NET_ERR_CRC,
	AIUS_NET_ERR_TYPE,
	AIUS_NET_ERR_PAYLOAD
} AiusNetError;

typedef struct {
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint32_t request_id;
	const uint8_t *payload;
	uint16_t payload_len;
} AiusNetFrame;

#define AIUS_NET_DHCP_DEADLINE_MS 5000u
#define AIUS_NET_BACKOFF_MS 1000u

typedef enum {
	AIUS_NET_STATE_STACK_START,
	AIUS_NET_STATE_DHCP_WAIT,
	AIUS_NET_STATE_ONLINE,
	AIUS_NET_STATE_TEARDOWN,
	AIUS_NET_STATE_BACKOFF,
	AIUS_NET_STATE_RECOVERY_SAFE
} AiusNetState;

typedef enum {
	AIUS_NET_EVENT_NONE,
	AIUS_NET_EVENT_STACK_STARTED,
	AIUS_NET_EVENT_LEASE_ACQUIRED,
	AIUS_NET_EVENT_RESOURCE_FAULT,
	AIUS_NET_EVENT_TEARDOWN_DONE,
	AIUS_NET_EVENT_TEARDOWN_FAULT
} AiusNetEventKind;

typedef enum {
	AIUS_NET_ACTION_NONE,
	AIUS_NET_ACTION_START_STACK,
	AIUS_NET_ACTION_START_DHCP,
	AIUS_NET_ACTION_TEARDOWN,
	AIUS_NET_ACTION_PUBLISH_ONLINE
} AiusNetActionKind;

typedef enum {
	AIUS_NET_FAULT_NONE = 0,
	AIUS_NET_FAULT_DHCP_TIMEOUT = 1,
	AIUS_NET_FAULT_LINK_LOST = 2,
	AIUS_NET_FAULT_STACK_START = 3,
	AIUS_NET_FAULT_DHCP_START = 4,
	AIUS_NET_FAULT_LISTENER = 5,
	AIUS_NET_FAULT_UDP = 6,
	AIUS_NET_FAULT_SOCKET = 7,
	AIUS_NET_FAULT_TEARDOWN = 8,
	AIUS_NET_FAULT_CALLBACK_OVERFLOW = 9,
	AIUS_NET_FAULT_DRIVER_PROTOCOL = 10
} AiusNetFault;

typedef struct {
	AiusNetEventKind kind;
	uint32_t generation;
	AiusNetFault fault;
} AiusNetEvent;

typedef struct {
	AiusNetActionKind kind;
	uint32_t generation;
} AiusNetAction;

typedef struct {
	AiusNetState state;
	uint32_t generation;
	uint32_t deadline_ms;
	uint32_t restarts;
	uint32_t ignored_events;
	uint32_t cleanup_faults;
	AiusNetFault last_fault;
} AiusNetSupervisor;

typedef struct {
	AiusNetState network_state;
	bool recovery_safe;
	AiusNetFault network_last_fault;
	uint16_t last_checkpoint;
	uint32_t network_generation;
	uint32_t boot_generation;
	uint32_t reset_source;
	uint8_t watchdog_early_resets;
	uint32_t loop_epoch;
	uint32_t network_restarts;
} AiusRecoveryStatus;

uint32_t aius_net_crc32(const uint8_t *data, size_t len);

size_t aius_net_encode(uint8_t *dst, size_t dst_cap, uint8_t type,
	                       uint16_t flags, uint32_t request_id,
	                       const uint8_t *payload, uint16_t payload_len);

size_t aius_net_status_encode(
	uint8_t *output,
	size_t capacity,
	const uint8_t legacy_status[71],
	const AiusRecoveryStatus *recovery);

AiusNetError aius_net_decode(const uint8_t *src, size_t len,
	                             AiusNetFrame *out);

void aius_net_supervisor_init(AiusNetSupervisor *supervisor, uint32_t now_ms,
	                          bool recovery_safe,
	                          AiusNetAction *initial_action);

void aius_net_supervisor_step(AiusNetSupervisor *supervisor, uint32_t now_ms,
	                          AiusNetEvent event, AiusNetAction *action);

static inline void aius_net_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)(v >> 8);
}

static inline void aius_net_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t aius_net_get_u16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t aius_net_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif

#endif
