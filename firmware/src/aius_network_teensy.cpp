#include "aius_network_teensy.h"

#include <string.h>

#ifndef AIUS_NETWORK_TEENSY_TESTING
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>
#include <fnet.h>
#endif

/* The status accessors run in epoch and HTTP context, never per audio block,
 * so each of their definitions carries FLASHMEM -- on the definition rather
 * than a prototype, because GCC does not inherit a section attribute from a
 * forward declaration. The lifecycle and callback code below is deliberately
 * left in the default section: the link and DHCP callbacks run in the FNET
 * poll interrupt.
 */
#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

#ifndef AIUS_NETWORK_TEENSY_TESTING
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

struct AiusNetworkTeensyDiagPrimitives {
	bool (*netif_initialized)(void);
	AiusNetworkTeensyLink (*link_live)(void);
	uint8_t (*dhcp_state)(void);
	bool (*frames)(uint32_t *tx, uint32_t *rx);
	uint32_t (*uptime_ms)(void);
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

/* Wrap-safe: the same signed-difference comparison the portable supervisor
 * uses for its own deadlines, so a millis() wrap during the link wait cannot
 * turn into a 49-day one.
 */
bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
	return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
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
	if (adapter == nullptr) {
		return;
	}
	if (kind == AIUS_NET_EVENT_LEASE_ACQUIRED &&
	    generation == adapter->generation) {
		adapter->lease_acquired = true;
		adapter->dhcp_updates++;
	}
	if (adapter->callback_overflow) {
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
const AiusNetworkTeensyDiagPrimitives *g_diag_primitives;

uint8_t g_test_address[4];
uint32_t g_address_reads;

FLASHMEM void read_local_address(uint8_t *address)
{
	g_address_reads++;
	address[0] = g_test_address[0];
	address[1] = g_test_address[1];
	address[2] = g_test_address[2];
	address[3] = g_test_address[3];
}

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

FLASHMEM void read_local_address(uint8_t *address)
{
	const IPAddress local = Ethernet.localIP();
	address[0] = local[0];
	address[1] = local[1];
	address[2] = local[2];
	address[3] = local[3];
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

/* --- Diagnostic readers ---------------------------------------------------
 *
 * None of these participates in a decision. Each is called from the epoch's
 * action path or from a status serializer, never from the audio path and never
 * from a callback, so each carries FLASHMEM on its definition.
 *
 * fnet_netif_get_default() answers a null descriptor before fnet_init() has
 * ever run, which is exactly the state START_STACK asks netif_initialized() in
 * on the first generation of a cold boot. Every call below is null-safe there:
 * fnet_netif_is_initialized() walks an empty list, fnet_dhcp_cln_get_by_netif()
 * and fnet_netif_get_statistics() both test the descriptor first. The one that
 * is not is fnet_netif_is_connected(), which returns "connected by default"
 * for a null interface (fnet_netif.c:1015) -- so link_live() tests the
 * descriptor itself rather than trusting that answer.
 */

FLASHMEM bool netif_initialized()
{
	return fnet_netif_is_initialized(fnet_netif_get_default()) != FNET_FALSE;
}

FLASHMEM AiusNetworkTeensyLink link_live()
{
	fnet_netif_desc_t netif = fnet_netif_get_default();

	if (netif == nullptr) {
		return AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
	}
	return fnet_netif_is_connected(netif) != FNET_FALSE
		       ? AIUS_NETWORK_TEENSY_LINK_UP
		       : AIUS_NETWORK_TEENSY_LINK_DOWN;
}

FLASHMEM uint8_t dhcp_state()
{
	fnet_dhcp_cln_desc_t dhcp =
		fnet_dhcp_cln_get_by_netif(fnet_netif_get_default());

	return dhcp == nullptr ? AIUS_NETWORK_TEENSY_DHCP_STATE_NONE
		                   : (uint8_t)fnet_dhcp_cln_get_state(dhcp);
}

FLASHMEM bool frames(uint32_t *tx, uint32_t *rx)
{
	struct fnet_netif_statistics statistics = {};

	if (fnet_netif_get_statistics(fnet_netif_get_default(), &statistics) !=
	    FNET_OK) {
		return false;
	}
	*tx = (uint32_t)statistics.tx_packet;
	*rx = (uint32_t)statistics.rx_packet;
	return true;
}

FLASHMEM uint32_t uptime_ms()
{
	return (uint32_t)millis();
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

const AiusNetworkTeensyDiagPrimitives kProductionDiagPrimitives = {
	netif_initialized,
	link_live,
	dhcp_state,
	frames,
	uptime_ms,
};

const AiusNetworkTeensyPrimitives *g_primitives = &kProductionPrimitives;
const AiusNetworkTeensyDiagPrimitives *g_diag_primitives =
	&kProductionDiagPrimitives;

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

/* --- Diagnostic bookkeeping -----------------------------------------------
 *
 * Every function below writes AiusNetworkTeensyDiag and nothing else. None is
 * consulted by an action, an event or a deadline, and each optional primitive
 * is null-checked at its own call site so a table without the diagnostic
 * readers behaves exactly as one with them, minus the measurements.
 */

FLASHMEM uint32_t diag_uptime_ms()
{
	return g_diag_primitives != nullptr ? g_diag_primitives->uptime_ms() : 0u;
}

FLASHMEM uint8_t diag_dhcp_state()
{
	return g_diag_primitives != nullptr
		       ? g_diag_primitives->dhcp_state()
		       : (uint8_t)AIUS_NETWORK_TEENSY_DHCP_STATE_NONE;
}

FLASHMEM uint8_t diag_link_live()
{
	return g_diag_primitives != nullptr
		       ? (uint8_t)g_diag_primitives->link_live()
		       : (uint8_t)AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
}

FLASHMEM bool diag_frames(uint32_t *tx, uint32_t *rx)
{
	*tx = 0u;
	*rx = 0u;
	return g_diag_primitives != nullptr && g_diag_primitives->frames(tx, rx);
}

/* Offsets are from stack start and clamped at zero: a diagnostic must never
 * turn a clock that moved backwards into a 49-day interval.
 */
FLASHMEM uint32_t diag_offset(const AiusNetworkTeensy *adapter, uint32_t now_ms)
{
	const int32_t elapsed =
		(int32_t)(now_ms - adapter->diag.stack_started_ms_absolute);

	return elapsed > 0 ? (uint32_t)elapsed : 0u;
}

/* Opens the record for a generation, retaining the first generation that ever
 * ended before the previous record is overwritten. */
FLASHMEM void diag_begin(AiusNetworkTeensy *adapter, uint32_t generation,
	                     uint32_t now_ms)
{
	if (!adapter->diag_first_valid && adapter->diag.generation != 0u &&
	    adapter->diag.end_reason != AIUS_NETWORK_TEENSY_END_NONE) {
		adapter->diag_first = adapter->diag;
		adapter->diag_first_valid = true;
	}
	memset(&adapter->diag, 0, sizeof(adapter->diag));
	adapter->diag.generation = generation;
	adapter->diag.stack_started_ms_absolute = now_ms;
	adapter->diag.dhcp_state_at_start = AIUS_NETWORK_TEENSY_DHCP_STATE_NONE;
	adapter->diag.dhcp_state_at_end = AIUS_NETWORK_TEENSY_DHCP_STATE_NONE;
}

FLASHMEM void diag_note_link_up(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                            AiusNetworkTeensyLinkUpSource source)
{
	if (adapter->diag.link_up_source != AIUS_NETWORK_TEENSY_LINK_UP_NONE) {
		return;
	}
	adapter->diag.link_up_source = (uint8_t)source;
	adapter->diag.link_up_ms = diag_offset(adapter, now_ms);
}

/* The link service's own callback, counted whichever polarity it carries and
 * timestamped on its first connected delivery -- including the delivery the
 * exactly-once latch drops as a duplicate, which is precisely the case where
 * the snapshot released the gate and the fresh read is the interesting one. */
FLASHMEM void diag_note_link_callback(AiusNetworkTeensy *adapter,
	                                  uint32_t now_ms, bool connected)
{
	if (adapter->diag.link_callbacks < 255u) {
		adapter->diag.link_callbacks++;
	}
	if (connected && !adapter->diag.link_callback_up_seen) {
		adapter->diag.link_callback_up_seen = true;
		adapter->diag.link_callback_up_ms = diag_offset(adapter, now_ms);
	}
}

FLASHMEM void diag_note_dhcp_start(AiusNetworkTeensy *adapter, uint32_t now_ms)
{
	adapter->diag.dhcp_started_seen = true;
	adapter->diag.dhcp_start_ms = diag_offset(adapter, now_ms);
	adapter->diag.dhcp_state_at_start = diag_dhcp_state();
	adapter->diag.frames_valid =
		diag_frames(&adapter->diag.tx_at_dhcp_start,
		            &adapter->diag.rx_at_dhcp_start);
}

/* First end wins: a generation that goes online and is torn down later keeps
 * the moment it succeeded rather than the moment it was retired. */
FLASHMEM void diag_end(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                   AiusNetworkTeensyEnd reason)
{
	uint32_t tx = 0u;
	uint32_t rx = 0u;

	if (adapter->diag.generation == 0u ||
	    adapter->diag.end_reason != AIUS_NETWORK_TEENSY_END_NONE) {
		return;
	}
	adapter->diag.end_reason = (uint8_t)reason;
	adapter->diag.end_ms = diag_offset(adapter, now_ms);
	adapter->diag.dhcp_state_at_end = diag_dhcp_state();
	adapter->diag.link_live_at_end = diag_link_live();
	adapter->diag.link_at_end = adapter->diag.link_cached_last;
	if (adapter->diag.frames_valid && diag_frames(&tx, &rx)) {
		adapter->diag.tx_frames = tx - adapter->diag.tx_at_dhcp_start;
		adapter->diag.rx_frames = rx - adapter->diag.rx_at_dhcp_start;
	} else {
		adapter->diag.frames_valid = false;
	}
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

void advance_teardown(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                  AiusNetEvent *event)
{
	if (adapter->action_step == 0u) {
		diag_end(adapter, now_ms, AIUS_NETWORK_TEENSY_END_TEARDOWN);
		adapter->online = false;
		adapter->lease_acquired = false;
		adapter->link_up_reported = false;
		adapter->stack_started_ms = 0u;
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

void advance_publish(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                 AiusNetEvent *event)
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
	diag_end(adapter, now_ms, AIUS_NETWORK_TEENSY_END_ONLINE);
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
			advance_teardown(adapter, now_ms, event);
			return;
		}
		if (action.kind != AIUS_NET_ACTION_NONE &&
		    (action.kind != adapter->active_action.kind ||
		     action.generation != adapter->active_action.generation)) {
			protocol_fault(adapter, event);
			return;
		}
		if (adapter->active_action.kind == AIUS_NET_ACTION_PUBLISH_ONLINE) {
			advance_publish(adapter, now_ms, event);
		} else {
			advance_teardown(adapter, now_ms, event);
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
		advance_teardown(adapter, now_ms, event);
		return;
	}

	if (action.kind == AIUS_NET_ACTION_START_STACK) {
		if (adapter->stack_started || adapter->dhcp_started ||
		    adapter->listener_open || adapter->udp_open || adapter->online) {
			protocol_fault(adapter, event);
			return;
		}
		adapter->generation = action.generation;
		adapter->lease_acquired = false;
		adapter->link_up_reported = false;
		adapter->stack_started_ms = now_ms;
		diag_begin(adapter, action.generation, now_ms);
		g_primitives->callbacks_install(adapter, action.generation);
		adapter->stack_started = true;
		/* Diagnostic, measured around the call rather than after it,
		 * because beginNoWait() is only "no wait" on a generation that
		 * finds the interface already initialised: the one that runs
		 * fnet_netif_init() resets the PHY and blocks in FNET's
		 * autonegotiation wait for up to two seconds. Neither reading
		 * changes what the call does or when it is made.
		 */
		adapter->diag.netif_init =
			g_diag_primitives == nullptr
				? AIUS_NETWORK_TEENSY_NETIF_INIT_UNKNOWN
			: g_diag_primitives->netif_initialized()
				? AIUS_NETWORK_TEENSY_NETIF_INIT_SKIPPED
				: AIUS_NETWORK_TEENSY_NETIF_INIT_PERFORMED;
		const uint32_t entered_ms = diag_uptime_ms();
		const bool started = g_primitives->stack_start();
		const uint32_t blocked_ms = diag_uptime_ms() - entered_ms;
		adapter->diag.stack_start_blocking_ms =
			blocked_ms > 0xFFFFu ? 0xFFFFu : (uint16_t)blocked_ms;
		if (!started) {
			emit(event, AIUS_NET_EVENT_RESOURCE_FAULT, action.generation,
			     AIUS_NET_FAULT_STACK_START);
			return;
		}
		/* Starting the stack is not evidence that the link is up:
		 * beginNoWait() returns before the PHY has negotiated, and
		 * DHCP started on a dead link is a full deadline of dead air.
		 * A link that is already up produces no NativeEthernet
		 * callback -- it reports changes -- so the already-up case is
		 * this one read. Anything else waits for poll().
		 */
		const AiusNetworkTeensyLink link = g_primitives->link_status();
		adapter->diag.link_at_stack_start = (uint8_t)link;
		adapter->diag.link_cached_last = (uint8_t)link;
		if (link == AIUS_NETWORK_TEENSY_LINK_UP) {
			adapter->link_up_reported = true;
			diag_note_link_up(adapter, now_ms,
			                  AIUS_NETWORK_TEENSY_LINK_UP_STACK_START);
			emit(event, AIUS_NET_EVENT_STACK_STARTED,
			     action.generation);
		}
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
		diag_note_dhcp_start(adapter, now_ms);
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
	advance_publish(adapter, now_ms, event);
}

void aius_network_teensy_poll(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                          AiusNetEvent *event)
{
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
		/* The callback is one of three observers of a single link-up.
		 * The latch keeps STACK_STARTED exactly-once per generation, so
		 * a callback that follows the stack-start or poll read is
		 * dropped here rather than delivered as a duplicate. The
		 * diagnostic note is taken before the drop, deliberately: a
		 * dropped duplicate is the case where the init-time snapshot
		 * released the gate and the link service's own fresh read
		 * arrived afterwards, which is the comparison being measured.
		 */
		if (queued.kind == AIUS_NET_EVENT_STACK_STARTED ||
		    (queued.kind == AIUS_NET_EVENT_RESOURCE_FAULT &&
		     queued.fault == AIUS_NET_FAULT_LINK_LOST)) {
			diag_note_link_callback(
				adapter, now_ms,
				queued.kind == AIUS_NET_EVENT_STACK_STARTED);
		}
		if (queued.kind == AIUS_NET_EVENT_STACK_STARTED) {
			if (adapter->link_up_reported) {
				continue;
			}
			adapter->link_up_reported = true;
			diag_note_link_up(adapter, now_ms,
			                  AIUS_NETWORK_TEENSY_LINK_UP_CALLBACK);
		}
		*event = queued;
		return;
	}

	if (!adapter->stack_started) {
		return;
	}

	const AiusNetworkTeensyLink link = g_primitives->link_status();

	adapter->diag.link_cached_last = (uint8_t)link;

	if (adapter->link_up_reported) {
		/* Past the gate this is the health check it always was. */
		if (link == AIUS_NETWORK_TEENSY_LINK_DOWN) {
			emit(event, AIUS_NET_EVENT_RESOURCE_FAULT,
			     adapter->generation, AIUS_NET_FAULT_LINK_LOST);
		}
		return;
	}

	/* Waiting for link-up. An already-up link produces no callback, so this
	 * read is what observes it; UNKNOWN is not up and is not a fault, it
	 * just keeps waiting. The deadline is what makes the wait bounded: no
	 * link at all becomes a resource fault, which the supervisor turns into
	 * teardown, backoff and the next generation rather than a stall.
	 */
	if (link == AIUS_NETWORK_TEENSY_LINK_UP) {
		adapter->link_up_reported = true;
		diag_note_link_up(adapter, now_ms,
		                  AIUS_NETWORK_TEENSY_LINK_UP_POLL);
		emit(event, AIUS_NET_EVENT_STACK_STARTED, adapter->generation);
		return;
	}
	if (deadline_reached(now_ms, adapter->stack_started_ms +
		                          AIUS_NETWORK_TEENSY_LINK_DEADLINE_MS)) {
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

FLASHMEM AiusNetworkTeensyLink aius_network_teensy_link(
	const AiusNetworkTeensy *adapter)
{
	if (adapter == nullptr || !adapter->stack_started ||
	    !primitives_ready()) {
		return AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
	}
	const AiusNetworkTeensyLink link = g_primitives->link_status();
	if (link != AIUS_NETWORK_TEENSY_LINK_UP &&
	    link != AIUS_NETWORK_TEENSY_LINK_DOWN) {
		return AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
	}
	return link;
}

FLASHMEM bool aius_network_teensy_address(const AiusNetworkTeensy *adapter,
	                                      uint8_t *address)
{
	if (address == nullptr) {
		return false;
	}
	address[0] = 0u;
	address[1] = 0u;
	address[2] = 0u;
	address[3] = 0u;
	if (adapter == nullptr || !adapter->stack_started ||
	    !primitives_ready()) {
		return false;
	}
	read_local_address(address);
	return (address[0] | address[1] | address[2] | address[3]) != 0u;
}

FLASHMEM bool aius_network_teensy_dhcp_pending(
	const AiusNetworkTeensy *adapter)
{
	return adapter != nullptr && adapter->dhcp_started &&
	       !adapter->lease_acquired;
}

FLASHMEM const AiusNetworkTeensyDiag *aius_network_teensy_diag(
	const AiusNetworkTeensy *adapter)
{
	return adapter == nullptr ? nullptr : &adapter->diag;
}

FLASHMEM const AiusNetworkTeensyDiag *aius_network_teensy_diag_first(
	const AiusNetworkTeensy *adapter)
{
	return adapter == nullptr || !adapter->diag_first_valid
		       ? nullptr
		       : &adapter->diag_first;
}

FLASHMEM AiusNetworkTeensyLink aius_network_teensy_link_live(
	const AiusNetworkTeensy *adapter)
{
	if (adapter == nullptr || !adapter->stack_started ||
	    !primitives_ready()) {
		return AIUS_NETWORK_TEENSY_LINK_UNKNOWN;
	}
	return (AiusNetworkTeensyLink)diag_link_live();
}

FLASHMEM uint8_t aius_network_teensy_dhcp_state(
	const AiusNetworkTeensy *adapter)
{
	if (adapter == nullptr || !adapter->stack_started ||
	    !primitives_ready()) {
		return AIUS_NETWORK_TEENSY_DHCP_STATE_NONE;
	}
	return diag_dhcp_state();
}

FLASHMEM bool aius_network_teensy_frames(const AiusNetworkTeensy *adapter,
	                                     uint32_t *tx, uint32_t *rx)
{
	if (tx == nullptr || rx == nullptr) {
		return false;
	}
	*tx = 0u;
	*rx = 0u;
	if (adapter == nullptr || !adapter->stack_started ||
	    !primitives_ready()) {
		return false;
	}
	return diag_frames(tx, rx);
}

#ifdef AIUS_NETWORK_TEENSY_TESTING

void aius_network_teensy_test_set_diag_primitives(
	const AiusNetworkTeensyDiagPrimitives *primitives)
{
	g_diag_primitives = primitives;
}

void aius_network_teensy_test_set_primitives(
	const AiusNetworkTeensyPrimitives *primitives)
{
	g_primitives = primitives;
	g_diag_primitives = nullptr;
	g_stale_callbacks = 0u;
	g_test_address[0] = 0u;
	g_test_address[1] = 0u;
	g_test_address[2] = 0u;
	g_test_address[3] = 0u;
	g_address_reads = 0u;
}

FLASHMEM void aius_network_teensy_test_set_address(const uint8_t *address)
{
	g_test_address[0] = address[0];
	g_test_address[1] = address[1];
	g_test_address[2] = address[2];
	g_test_address[3] = address[3];
}

FLASHMEM uint32_t aius_network_teensy_test_address_reads()
{
	return g_address_reads;
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
