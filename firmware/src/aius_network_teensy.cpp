#include "aius_network_teensy.h"

#include <string.h>

#ifndef AIUS_NETWORK_TEENSY_TESTING
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>
#include <fnet.h>
#endif

namespace {

#ifndef AIUS_NETWORK_TEENSY_TESTING
enum AiusNetworkTeensyLink {
	AIUS_NETWORK_TEENSY_LINK_UNKNOWN,
	AIUS_NETWORK_TEENSY_LINK_UP,
	AIUS_NETWORK_TEENSY_LINK_DOWN
};

struct AiusNetworkTeensyPrimitives {
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
};
#endif

constexpr uint16_t kHttpPort = 80u;
constexpr uint16_t kControlPort = 4242u;

uint32_t g_stale_callbacks;

AiusNetEvent none_event()
{
	return {AIUS_NET_EVENT_NONE, 0u, AIUS_NET_FAULT_NONE};
}

AiusIoEvent io_event(AiusIoResult result, size_t bytes = 0u,
	                 uint32_t fault = 0u)
{
	return {result, bytes, fault};
}

void emit(AiusNetEvent *event, AiusNetEventKind kind, uint32_t generation,
	      AiusNetFault fault = AIUS_NET_FAULT_NONE)
{
	event->kind = kind;
	event->generation = generation;
	event->fault = fault;
}

void clear_pending(AiusNetworkTeensy *adapter)
{
	adapter->pending_read = 0u;
	adapter->pending_count = 0u;
	adapter->callback_overflow = false;
}

void clear_active(AiusNetworkTeensy *adapter)
{
	adapter->active_action.kind = AIUS_NET_ACTION_NONE;
	adapter->active_action.generation = 0u;
	adapter->action_step = 0u;
	adapter->action_fault = false;
}

void enqueue(AiusNetworkTeensy *adapter, uint32_t generation,
	         AiusNetEventKind kind, AiusNetFault fault)
{
	if (adapter == nullptr || adapter->callback_overflow) {
		return;
	}
	if (adapter->pending_count == 4u) {
		adapter->pending_read = 0u;
		adapter->pending_count = 0u;
		adapter->callback_overflow = true;
		return;
	}
	const uint8_t index =
		(uint8_t)((adapter->pending_read + adapter->pending_count) % 4u);
	adapter->pending[index] = {kind, generation, fault};
	adapter->pending_count++;
}

#ifdef AIUS_NETWORK_TEENSY_TESTING

const AiusNetworkTeensyPrimitives *g_primitives;

#else

struct CallbackContext {
	AiusNetworkTeensy *adapter;
	uint32_t generation;
	bool valid;
};

CallbackContext g_callback;
uint8_t g_mac[6] = {0x02u, 0x41u, 0x49u, 0x55u, 0x01u, 0x01u};
EthernetServer g_listener(kHttpPort);
EthernetUDP g_udp;
EthernetClient g_client;
fnet_sockaddr g_udp_peer;
bool g_udp_peer_valid;

void link_callback(bool connected, void *context)
{
	auto *callback = static_cast<CallbackContext *>(context);
	if (callback == nullptr || !callback->valid) {
		return;
	}
	enqueue(callback->adapter, callback->generation,
	        connected ? AIUS_NET_EVENT_STACK_STARTED
	                  : AIUS_NET_EVENT_RESOURCE_FAULT,
	        connected ? AIUS_NET_FAULT_NONE : AIUS_NET_FAULT_LINK_LOST);
}

void dhcp_callback(void *context)
{
	auto *callback = static_cast<CallbackContext *>(context);
	if (callback == nullptr || !callback->valid) {
		return;
	}
	enqueue(callback->adapter, callback->generation,
	        AIUS_NET_EVENT_LEASE_ACQUIRED, AIUS_NET_FAULT_NONE);
}

void callbacks_install(AiusNetworkTeensy *adapter, uint32_t generation)
{
	g_callback = {adapter, generation, true};
	Ethernet.setLinkCallback(link_callback, &g_callback);
	Ethernet.setDhcpCallback(dhcp_callback, &g_callback);
}

void callbacks_clear()
{
	g_callback.valid = false;
}

bool stack_start()
{
	return Ethernet.beginNoWait(g_mac);
}

bool dhcp_start()
{
	return Ethernet.dhcpStart();
}

bool listener_open()
{
	g_listener.begin();
	return g_listener.service_descriptor != 0;
}

bool udp_open()
{
	g_udp_peer_valid = false;
	return g_udp.begin(kControlPort) != 0u;
}

bool listener_close()
{
	g_listener.end();
	return true;
}

bool udp_close()
{
	g_udp.stop();
	g_udp_peer_valid = false;
	return true;
}

bool stack_close()
{
	Ethernet.end();
	g_client = EthernetClient();
	return true;
}

AiusNetworkTeensyLink link_status()
{
	switch (Ethernet.linkStatus()) {
	case LinkON:
		return AIUS_NETWORK_TEENSY_LINK_UP;
	case LinkOFF:
		return AIUS_NETWORK_TEENSY_LINK_DOWN;
	default:
		return AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
	}
}

AiusIoEvent accept_once()
{
	if (g_client.getSocketNumber() < MAX_SOCK_NUM) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	EthernetClient candidate = g_listener.accept();
	if (candidate.getSocketNumber() >= MAX_SOCK_NUM) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	g_client = candidate;
	return io_event(AIUS_IO_PROGRESS);
}

AiusIoEvent read_once(uint8_t *buffer, size_t capacity)
{
	const uint8_t socket = g_client.getSocketNumber();
	if (socket >= MAX_SOCK_NUM || Ethernet.socket_ptr[socket] == nullptr) {
		return io_event(AIUS_IO_CLOSED);
	}
	const fnet_ssize_t result =
		fnet_socket_recv(Ethernet.socket_ptr[socket], buffer, capacity, 0);
	if (result > 0) {
		return io_event(AIUS_IO_PROGRESS, (size_t)result);
	}
	if (result == 0) {
		return io_event(AIUS_IO_CLOSED);
	}
	return fnet_error_get() == FNET_ERR_AGAIN
		       ? io_event(AIUS_IO_WOULD_BLOCK)
		       : io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET);
}

AiusIoEvent write_once(const uint8_t *buffer, size_t length)
{
	if (g_client.getSocketNumber() >= MAX_SOCK_NUM) {
		return io_event(AIUS_IO_CLOSED);
	}
	g_client.clearWriteError();
	const size_t result = g_client.write(buffer, length);
	if (result != 0u) {
		return io_event(AIUS_IO_PROGRESS, result);
	}
	return g_client.getWriteError() == 0
		       ? io_event(AIUS_IO_WOULD_BLOCK)
		       : io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET);
}

AiusIoEvent close_once()
{
	const uint8_t socket = g_client.getSocketNumber();
	if (socket >= MAX_SOCK_NUM || Ethernet.socket_ptr[socket] == nullptr) {
		g_client = EthernetClient();
		return io_event(AIUS_IO_CLOSED);
	}
	fnet_socket_close(Ethernet.socket_ptr[socket]);
	Ethernet.socket_ptr[socket] = nullptr;
	g_client = EthernetClient();
	return io_event(AIUS_IO_PROGRESS);
}

AiusIoEvent udp_receive_once(uint8_t *buffer, size_t capacity)
{
	if (g_udp.sockindex >= MAX_SOCK_NUM) {
		return io_event(AIUS_IO_CLOSED);
	}
	fnet_size_t peer_size = sizeof(g_udp_peer);
	const fnet_ssize_t result =
		fnet_socket_recvfrom(Ethernet.socket_ptr[g_udp.sockindex], buffer,
		                    capacity, 0, &g_udp_peer, &peer_size);
	if (result > 0) {
		g_udp_peer_valid = true;
		return io_event(AIUS_IO_PROGRESS, (size_t)result);
	}
	if (result == 0) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	if (result == FNET_ERR && fnet_error_get() == FNET_ERR_AGAIN) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	return io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET);
}

AiusIoEvent udp_send_once(const uint8_t *buffer, size_t length)
{
	if (g_udp.sockindex >= MAX_SOCK_NUM) {
		return io_event(AIUS_IO_CLOSED);
	}
	if (!g_udp_peer_valid) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	const fnet_ssize_t result =
		fnet_socket_sendto(Ethernet.socket_ptr[g_udp.sockindex], buffer,
		                  length, 0, &g_udp_peer, sizeof(g_udp_peer));
	if (result > 0) {
		return io_event(AIUS_IO_PROGRESS, (size_t)result);
	}
	if (result == 0) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	return fnet_error_get() == FNET_ERR_AGAIN
		       ? io_event(AIUS_IO_WOULD_BLOCK)
		       : io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET);
}

const AiusNetworkTeensyPrimitives kProductionPrimitives = {
	callbacks_install,
	callbacks_clear,
	stack_start,
	dhcp_start,
	listener_open,
	udp_open,
	listener_close,
	udp_close,
	stack_close,
	link_status,
	accept_once,
	read_once,
	write_once,
	close_once,
	udp_receive_once,
	udp_send_once,
};

const AiusNetworkTeensyPrimitives *g_primitives = &kProductionPrimitives;

#endif

bool primitives_ready()
{
	return g_primitives != nullptr && g_primitives->callbacks_install != nullptr &&
	       g_primitives->callbacks_clear != nullptr &&
	       g_primitives->stack_start != nullptr &&
	       g_primitives->dhcp_start != nullptr &&
	       g_primitives->listener_open != nullptr &&
	       g_primitives->udp_open != nullptr &&
	       g_primitives->listener_close != nullptr &&
	       g_primitives->udp_close != nullptr &&
	       g_primitives->stack_close != nullptr &&
	       g_primitives->link_status != nullptr &&
	       g_primitives->accept_once != nullptr &&
	       g_primitives->read_once != nullptr &&
	       g_primitives->write_once != nullptr &&
	       g_primitives->close_once != nullptr &&
	       g_primitives->udp_receive_once != nullptr &&
	       g_primitives->udp_send_once != nullptr;
}

void protocol_fault(AiusNetworkTeensy *adapter, AiusNetEvent *event)
{
	clear_active(adapter);
	emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, adapter->generation,
	     AIUS_NET_FAULT_DRIVER_PROTOCOL);
}

void finish_teardown(AiusNetworkTeensy *adapter, AiusNetEvent *event)
{
	const bool failed = adapter->action_fault;
	const uint32_t generation = adapter->active_action.generation;
	clear_active(adapter);
	emit(event,
	     failed ? AIUS_NET_EVENT_TEARDOWN_FAULT
	            : AIUS_NET_EVENT_TEARDOWN_DONE,
	     generation,
	     failed ? AIUS_NET_FAULT_TEARDOWN : AIUS_NET_FAULT_NONE);
}

void advance_teardown(AiusNetworkTeensy *adapter, AiusNetEvent *event)
{
	if (adapter->action_step == 0u) {
		adapter->online = false;
		clear_pending(adapter);
		if (!adapter->listener_open && !adapter->udp_open &&
		    !adapter->stack_started && !adapter->dhcp_started) {
			finish_teardown(adapter, event);
			return;
		}
		g_primitives->callbacks_clear();
		adapter->action_step = 1u;
	}

	if (adapter->action_step == 1u) {
		adapter->action_step = 2u;
		if (adapter->listener_open) {
			if (!g_primitives->listener_close()) {
				adapter->action_fault = true;
			}
			adapter->listener_open = false;
			return;
		}
	}
	if (adapter->action_step == 2u) {
		adapter->action_step = 3u;
		if (adapter->udp_open) {
			if (!g_primitives->udp_close()) {
				adapter->action_fault = true;
			}
			adapter->udp_open = false;
			return;
		}
	}
	if (adapter->action_step == 3u) {
		adapter->action_step = 4u;
		if (adapter->stack_started || adapter->dhcp_started) {
			if (!g_primitives->stack_close()) {
				adapter->action_fault = true;
			}
			adapter->stack_started = false;
			adapter->dhcp_started = false;
			finish_teardown(adapter, event);
			return;
		}
	}
	finish_teardown(adapter, event);
}

void advance_publish(AiusNetworkTeensy *adapter, AiusNetEvent *event)
{
	if (adapter->action_step == 0u) {
		adapter->listener_open = true;
		if (!g_primitives->listener_open()) {
			clear_active(adapter);
			emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, adapter->generation,
			     AIUS_NET_FAULT_LISTENER);
			return;
		}
		adapter->action_step = 1u;
		return;
	}

	adapter->udp_open = true;
	if (!g_primitives->udp_open()) {
		clear_active(adapter);
		emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, adapter->generation,
		     AIUS_NET_FAULT_UDP);
		return;
	}
	adapter->online = true;
	clear_active(adapter);
}

AiusIoEvent validate_io(AiusIoEvent event, size_t capacity)
{
	if (event.result < AIUS_IO_PROGRESS || event.result > AIUS_IO_FAULT ||
	    (event.result != AIUS_IO_PROGRESS && event.bytes != 0u) ||
	    event.bytes > capacity ||
	    (event.result == AIUS_IO_FAULT && event.fault == 0u) ||
	    (event.result != AIUS_IO_FAULT && event.fault != 0u)) {
		return io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	return event;
}

AiusIoEvent validate_data_io(AiusIoEvent event, size_t capacity)
{
	event = validate_io(event, capacity);
	if (event.result == AIUS_IO_PROGRESS && event.bytes == 0u) {
		return io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	return event;
}

} // namespace

extern "C" {

void aius_network_teensy_init(AiusNetworkTeensy *adapter)
{
	if (adapter == nullptr) {
		return;
	}
	memset(adapter, 0, sizeof(*adapter));
	adapter->active_action.kind = AIUS_NET_ACTION_NONE;
}

void aius_network_teensy_apply(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                           AiusNetAction action, AiusNetEvent *event)
{
	(void)now_ms;
	if (event == nullptr) {
		return;
	}
	*event = none_event();
	if (adapter == nullptr) {
		return;
	}
	if (!primitives_ready()) {
		protocol_fault(adapter, event);
		return;
	}

	const bool has_active =
		adapter->active_action.kind != AIUS_NET_ACTION_NONE;
	if (has_active) {
		if (adapter->active_action.kind != AIUS_NET_ACTION_TEARDOWN &&
		    action.kind == AIUS_NET_ACTION_TEARDOWN &&
		    action.generation == adapter->generation) {
			adapter->active_action = action;
			adapter->action_step = 0u;
			adapter->action_fault = false;
			advance_teardown(adapter, event);
			return;
		}
		if (action.kind != AIUS_NET_ACTION_NONE &&
		    (action.kind != adapter->active_action.kind ||
		     action.generation != adapter->active_action.generation)) {
			protocol_fault(adapter, event);
			return;
		}
		if (adapter->active_action.kind == AIUS_NET_ACTION_PUBLISH_ONLINE) {
			advance_publish(adapter, event);
		} else {
			advance_teardown(adapter, event);
		}
		return;
	}

	if (action.kind == AIUS_NET_ACTION_NONE) {
		return;
	}
	if (action.kind > AIUS_NET_ACTION_PUBLISH_ONLINE ||
	    action.generation == 0u) {
		protocol_fault(adapter, event);
		return;
	}

	if (action.kind == AIUS_NET_ACTION_TEARDOWN) {
		if (adapter->generation != 0u &&
		    action.generation != adapter->generation) {
			protocol_fault(adapter, event);
			return;
		}
		adapter->generation = action.generation;
		adapter->active_action = action;
		adapter->action_step = 0u;
		adapter->action_fault = false;
		advance_teardown(adapter, event);
		return;
	}

	if (action.kind == AIUS_NET_ACTION_START_STACK) {
		if (adapter->stack_started || adapter->dhcp_started ||
		    adapter->listener_open || adapter->udp_open || adapter->online) {
			protocol_fault(adapter, event);
			return;
		}
		adapter->generation = action.generation;
		g_primitives->callbacks_install(adapter, action.generation);
		adapter->stack_started = true;
		if (!g_primitives->stack_start()) {
			emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, action.generation,
			     AIUS_NET_FAULT_STACK_START);
			return;
		}
		emit(event, AIUS_NET_EVENT_STACK_STARTED, action.generation);
		return;
	}

	if (action.generation != adapter->generation ||
	    !adapter->stack_started) {
		protocol_fault(adapter, event);
		return;
	}

	if (action.kind == AIUS_NET_ACTION_START_DHCP) {
		if (adapter->dhcp_started || adapter->listener_open ||
		    adapter->udp_open || adapter->online) {
			protocol_fault(adapter, event);
			return;
		}
		adapter->dhcp_started = true;
		if (!g_primitives->dhcp_start()) {
			emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, action.generation,
			     AIUS_NET_FAULT_DHCP_START);
		}
		return;
	}

	if (action.kind != AIUS_NET_ACTION_PUBLISH_ONLINE ||
	    !adapter->dhcp_started || adapter->listener_open ||
	    adapter->udp_open || adapter->online) {
		protocol_fault(adapter, event);
		return;
	}
	adapter->active_action = action;
	adapter->action_step = 0u;
	adapter->action_fault = false;
	advance_publish(adapter, event);
}

void aius_network_teensy_poll(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                          AiusNetEvent *event)
{
	(void)now_ms;
	if (event == nullptr) {
		return;
	}
	*event = none_event();
	if (adapter == nullptr) {
		return;
	}
	if (!primitives_ready()) {
		protocol_fault(adapter, event);
		return;
	}
	if (adapter->callback_overflow) {
		adapter->callback_overflow = false;
		emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, adapter->generation,
		     AIUS_NET_FAULT_CALLBACK_OVERFLOW);
		return;
	}

	for (uint8_t inspected = 0u;
	     inspected < 4u && adapter->pending_count != 0u; inspected++) {
		const AiusNetEvent queued = adapter->pending[adapter->pending_read];
		adapter->pending_read = (uint8_t)((adapter->pending_read + 1u) % 4u);
		adapter->pending_count--;
		if (queued.generation != adapter->generation) {
			g_stale_callbacks++;
			continue;
		}
		*event = queued;
		return;
	}

	if (adapter->stack_started &&
	    g_primitives->link_status() == AIUS_NETWORK_TEENSY_LINK_DOWN) {
		emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, adapter->generation,
		     AIUS_NET_FAULT_LINK_LOST);
	}
}

AiusIoEvent aius_network_teensy_accept(AiusNetworkTeensy *adapter)
{
	if (adapter == nullptr || !primitives_ready()) {
		return io_event(AIUS_IO_FAULT, 0u,
		                AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	if (!adapter->online || !adapter->listener_open) {
		return io_event(AIUS_IO_CLOSED);
	}
	return validate_io(g_primitives->accept_once(), 0u);
}

AiusIoEvent aius_network_teensy_read_once(AiusNetworkTeensy *adapter,
	                                      uint8_t *buffer, size_t capacity)
{
	if (adapter == nullptr || !primitives_ready() ||
	    (buffer == nullptr && capacity != 0u)) {
		return io_event(AIUS_IO_FAULT, 0u,
		                AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	if (!adapter->online) {
		return io_event(AIUS_IO_CLOSED);
	}
	if (capacity == 0u) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	return validate_data_io(g_primitives->read_once(buffer, capacity),
	                        capacity);
}

AiusIoEvent aius_network_teensy_write_once(AiusNetworkTeensy *adapter,
	                                       const uint8_t *buffer,
	                                       size_t length)
{
	if (adapter == nullptr || !primitives_ready() ||
	    (buffer == nullptr && length != 0u)) {
		return io_event(AIUS_IO_FAULT, 0u,
		                AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	if (!adapter->online) {
		return io_event(AIUS_IO_CLOSED);
	}
	if (length == 0u) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	return validate_data_io(g_primitives->write_once(buffer, length), length);
}

AiusIoEvent aius_network_teensy_close_once(AiusNetworkTeensy *adapter)
{
	if (adapter == nullptr || !primitives_ready()) {
		return io_event(AIUS_IO_FAULT, 0u,
		                AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	if (!adapter->online) {
		return io_event(AIUS_IO_CLOSED);
	}
	return validate_io(g_primitives->close_once(), 0u);
}

AiusIoEvent aius_network_teensy_udp_receive_once(AiusNetworkTeensy *adapter,
	                                             uint8_t *buffer,
	                                             size_t capacity)
{
	if (adapter == nullptr || !primitives_ready() ||
	    (buffer == nullptr && capacity != 0u)) {
		return io_event(AIUS_IO_FAULT, 0u,
		                AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	if (!adapter->online || !adapter->udp_open) {
		return io_event(AIUS_IO_CLOSED);
	}
	if (capacity == 0u) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	return validate_data_io(g_primitives->udp_receive_once(buffer, capacity),
	                        capacity);
}

AiusIoEvent aius_network_teensy_udp_send_once(AiusNetworkTeensy *adapter,
	                                          const uint8_t *buffer,
	                                          size_t length)
{
	if (adapter == nullptr || !primitives_ready() ||
	    (buffer == nullptr && length != 0u)) {
		return io_event(AIUS_IO_FAULT, 0u,
		                AIUS_NET_FAULT_DRIVER_PROTOCOL);
	}
	if (!adapter->online || !adapter->udp_open) {
		return io_event(AIUS_IO_CLOSED);
	}
	if (length == 0u) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	return validate_data_io(g_primitives->udp_send_once(buffer, length),
	                        length);
}

#ifdef AIUS_NETWORK_TEENSY_TESTING

void aius_network_teensy_test_set_primitives(
	const AiusNetworkTeensyPrimitives *primitives)
{
	g_primitives = primitives;
	g_stale_callbacks = 0u;
}

void aius_network_teensy_test_callback(AiusNetworkTeensy *adapter,
	                                   uint32_t generation,
	                                   AiusNetEventKind kind,
	                                   AiusNetFault fault)
{
	enqueue(adapter, generation, kind, fault);
}

uint32_t aius_network_teensy_test_stale_callbacks()
{
	return g_stale_callbacks;
}

#endif

} // extern "C"
