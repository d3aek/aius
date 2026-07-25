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

/* Link state without a vendor type. The values match the NativeEthernet
 * EthernetLinkStatus ordering (Unknown, LinkON, LinkOFF) so a status surface
 * that used to publish the raw vendor value keeps its numbering.
 */
typedef enum {
	AIUS_NETWORK_TEENSY_LINK_UNKNOWN = 0,
	AIUS_NETWORK_TEENSY_LINK_UP = 1,
	AIUS_NETWORK_TEENSY_LINK_DOWN = 2
} AiusNetworkTeensyLink;

/* lease_acquired and dhcp_updates are DHCP state the adapter already sees on
 * its own lease callback: lease_acquired is generation-scoped and cleared at
 * every stack start and teardown, dhcp_updates counts leases since init and is
 * never cleared. Neither costs an underlying read.
 */
typedef struct {
	uint32_t generation;
	bool stack_started;
	bool dhcp_started;
	bool lease_acquired;
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
	uint32_t dhcp_updates;
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

/* Status accessors for the discovery surfaces (status JSON, UDP status payload,
 * USB ID). Each performs at most one underlying read, never waits, and is safe
 * in any supervisor state: before the stack starts, while an action is in
 * flight, and after teardown they return the not-available answer without
 * touching a dead stack.
 */

/* UNKNOWN whenever the stack is not started, so a caller can distinguish
 * "no link" from "nothing to ask".
 */
AiusNetworkTeensyLink aius_network_teensy_link(
	const AiusNetworkTeensy *adapter);

/* Writes the current IPv4 address as four octets, first octet first, and
 * returns whether that address is usable. A false return always leaves
 * 0.0.0.0 behind, so an unchecked caller still serializes a defined value.
 */
bool aius_network_teensy_address(const AiusNetworkTeensy *adapter,
	                             uint8_t address[4]);

/* DHCP has been asked for a lease on the current generation and no lease has
 * arrived yet. Reads adapter state only.
 */
bool aius_network_teensy_dhcp_pending(const AiusNetworkTeensy *adapter);

#ifdef AIUS_NETWORK_TEENSY_TESTING

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

/* The address read is a compile-time seam rather than an entry in the
 * primitives table above: that table is the lifecycle and socket-I/O
 * failure-injection seam, and a status read needs no failure injection, only a
 * value and a read count.
 */
void aius_network_teensy_test_set_address(const uint8_t address[4]);

uint32_t aius_network_teensy_test_address_reads(void);

#endif

#ifdef __cplusplus
}
#endif

#endif
