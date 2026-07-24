#ifndef AIUS_NETWORK_TEENSY_H
#define AIUS_NETWORK_TEENSY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_io.h"
#include "aius_net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t generation;
	bool stack_started;
	bool dhcp_started;
	bool online;
	bool listener_open;
	bool udp_open;
	AiusNetEvent pending[4];
	uint8_t pending_read;
	uint8_t pending_count;
	bool callback_overflow;
	AiusNetAction active_action;
	uint8_t action_step;
	bool action_fault;
} AiusNetworkTeensy;

void aius_network_teensy_init(AiusNetworkTeensy *adapter);

void aius_network_teensy_apply(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                           AiusNetAction action, AiusNetEvent *event);

void aius_network_teensy_poll(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                          AiusNetEvent *event);

AiusIoEvent aius_network_teensy_accept(AiusNetworkTeensy *adapter);

AiusIoEvent aius_network_teensy_read_once(AiusNetworkTeensy *adapter,
	                                      uint8_t *buffer, size_t capacity);

AiusIoEvent aius_network_teensy_write_once(AiusNetworkTeensy *adapter,
	                                       const uint8_t *buffer,
	                                       size_t length);

AiusIoEvent aius_network_teensy_close_once(AiusNetworkTeensy *adapter);

AiusIoEvent aius_network_teensy_udp_receive_once(AiusNetworkTeensy *adapter,
	                                             uint8_t *buffer,
	                                             size_t capacity);

AiusIoEvent aius_network_teensy_udp_send_once(AiusNetworkTeensy *adapter,
	                                          const uint8_t *buffer,
	                                          size_t length);

#ifdef AIUS_NETWORK_TEENSY_TESTING

typedef enum {
	AIUS_NETWORK_TEENSY_LINK_UNKNOWN,
	AIUS_NETWORK_TEENSY_LINK_UP,
	AIUS_NETWORK_TEENSY_LINK_DOWN
} AiusNetworkTeensyLink;

typedef struct {
	void (*callbacks_install)(AiusNetworkTeensy *adapter,
	                          uint32_t generation);
	void (*callbacks_clear)(void);
	bool (*stack_start)(void);
	bool (*dhcp_start)(void);
	bool (*listener_open)(void);
	bool (*udp_open)(void);
	bool (*listener_close)(void);
	bool (*udp_close)(void);
	bool (*stack_close)(void);
	AiusNetworkTeensyLink (*link_status)(void);
	AiusIoEvent (*accept_once)(void);
	AiusIoEvent (*read_once)(uint8_t *buffer, size_t capacity);
	AiusIoEvent (*write_once)(const uint8_t *buffer, size_t length);
	AiusIoEvent (*close_once)(void);
	AiusIoEvent (*udp_receive_once)(uint8_t *buffer, size_t capacity);
	AiusIoEvent (*udp_send_once)(const uint8_t *buffer, size_t length);
} AiusNetworkTeensyPrimitives;

void aius_network_teensy_test_set_primitives(
	const AiusNetworkTeensyPrimitives *primitives);

void aius_network_teensy_test_callback(AiusNetworkTeensy *adapter,
	                                   uint32_t generation,
	                                   AiusNetEventKind kind,
	                                   AiusNetFault fault);

uint32_t aius_network_teensy_test_stale_callbacks(void);

#endif

#ifdef __cplusplus
}
#endif

#endif
