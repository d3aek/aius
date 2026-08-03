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
	/* Optional (nullptr = skip): drives the FNET service poll from the
	 * network job when AIUS_NET_POLL_SINGLE_CONTEXT=1. Optional rather
	 * than required so the host-suite fakes, which model the socket
	 * surface and not the stack's polling contract, need no change. */
	void (*stack_poll)(uint32_t now_ms);
};

struct AiusNetworkTeensyDiagPrimitives {
	bool (*netif_initialized)(void);
	AiusNetworkTeensyLink (*link_live)(void);
	uint8_t (*dhcp_state)(void);
	bool (*frames)(uint32_t *tx, uint32_t *rx);
	uint32_t (*uptime_ms)(void);
	/* Optional (nullptr = unavailable): the FNET client's actual
	 * lease/T1/T2 option values, host byte order, seconds. Load-bearing
	 * for the reopened periodic-WDOG second death mode (open-work
	 * register): the death schedule must be read from the client, not
	 * reconstructed from cadence. */
	bool (*dhcp_options)(uint32_t *lease_s, uint32_t *t1_s,
	                     uint32_t *t2_s);
	/* Optional (nullptr = unavailable): FNET netheap telemetry -- the
	 * count-anchor hypothesis's conviction instrument. */
	bool (*stack_heap)(uint32_t *free_bytes, uint32_t *largest_block);
};
#endif

constexpr uint16_t kHttpPort = 80u;
constexpr uint16_t kControlPort = 4242u;

/* The readiness fallback is bounded BY a DHCP deadline, not equal to one:
 * waiting for evidence that a DISCOVER can leave the board must never cost more
 * than sending one and discovering the same thing the hard way.
 *
 * This was `==` until 2026-07-26, which is stricter than the invariant it states
 * and coupled two constants that answer to different measurements. It fired when
 * AIUS_NET_DHCP_DEADLINE_MS moved 5,000 -> 9,000 to clear the measured 4 s lease
 * floor -- and satisfying it by raising this one too would have *lengthened*
 * worst-case bring-up for nothing, since the rx-ready gate releases on RX counter
 * movement at 0.85-2.11 s on every observed boot and never reaches its fallback.
 * `<=` is what the sentence above actually says. */
static_assert(AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS <=
	              AIUS_NET_DHCP_DEADLINE_MS,
	          "the receive-readiness fallback is bounded by one DHCP deadline");

uint32_t g_stale_callbacks;

/* Control datagrams the receive dropped because they exceeded the capacity the
 * caller offered. File-scope rather than a member of AiusNetworkTeensy for the
 * same reason g_stale_callbacks is: there is one FNET stack and one control
 * socket behind these primitives, and the primitive that observes the drop is
 * handed a buffer, not an adapter. Free-running since boot and never cleared by
 * a teardown -- it counts what the segment sent, not what one generation saw.
 */
uint32_t g_udp_oversize_drops;

/* Control datagrams the receive dropped because their source address could not
 * serve as the reply peer. Same shape and same reasons as the counter above:
 * one FNET stack, one control socket, free-running since boot, and the only
 * trace a refused datagram leaves.
 */
uint32_t g_udp_unusable_peer_drops;

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

/* NVIC priority handed to FNET's 1 kHz poll timer, and the reason the audio
 * path is not at its mercy.
 *
 * THE STACK POLLS FROM AN INTERRUPT, NOT FROM THE EPOCH LOOP. beginNoWait()
 * starts an IntervalTimer running fnet_poll every 1,000 us for the life of the
 * network generation (vendor/NativeEthernet/src/NativeEthernet.cpp:167). This
 * plan's isolation argument is framed throughout as "epoch loop, not audio
 * ISR", which is true of this firmware's own code and untrue of the stack it
 * links.
 *
 * AND THE DEFAULT IS THE WRONG WAY ROUND. IntervalTimer defaults to NVIC
 * priority 128 (cores/teensy4/IntervalTimer.h:108) while the audio software
 * interrupt -- the one that runs update_all(), enqueue() and the whole DSP --
 * sits at 208 (cores/teensy4/AudioStream.cpp:424). Lower is stronger on the
 * NVIC, so fnet_poll does not merely fail to yield to the DSP: it PREEMPTS it,
 * a thousand times a second, in the middle of a block. Open defect 7 records
 * this as "equal priority, neither preempts", which is the relationship between
 * fnet_poll and the S/PDIF *DMA* ISR (both 128) rather than the one that
 * carries the signal processing.
 *
 * 224 is one implemented level below 208. The Cortex-M7 here implements four
 * priority bits, so only the top nibble is significant: 208 is level 13 and 224
 * is level 14, making this the smallest demotion the hardware can express. The
 * DSP now preempts network polling and never the other way round.
 *
 * STARVATION IS NOT THE TRADE. The DSP spends 8.4-8.8 % of a block, so the poll
 * still gets over 90 % of the time; and a delayed poll costs a retransmit while
 * a preempted block callback costs audio. §Deciding principle settles which way
 * that goes.
 *
 * WHY HERE AND NOT ONCE AT SETUP. IntervalTimer::beginCycles() re-applies the
 * instance's own nvic_priority on every begin(), and beginNoWait() is called
 * afresh for each network generation -- so an override applied once at boot is
 * silently undone by the first network restart. It belongs immediately after
 * the call that arms the timer.
 *
 * Set through NVIC_SET_PRIORITY rather than IntervalTimer::priority() because
 * _fnet_poll is private to EthernetClass. The two are equivalent: all four PIT
 * channels share IRQ_PIT, so priority() resolves to this same write, and this
 * firmware instantiates no IntervalTimer of its own for that resolution to
 * disagree with. A vendor patch would buy nothing and cost a PATCHED_SHA256
 * baseline. */
#define AIUS_FNET_POLL_PRIORITY 224u

bool stack_start()
{
	const bool started = Ethernet.beginNoWait(g_mac);

	/* Unconditional: a begin() that reported failure may still have armed the
	 * timer, and leaving it above the audio path on the failure route is the
	 * case nobody would think to test. */
	NVIC_SET_PRIORITY(IRQ_PIT, AIUS_FNET_POLL_PRIORITY);
	return started;
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

/* Defined below with the send path, whose error table established the set. Shared
 * rather than duplicated: the two directions drifted apart once already, and that
 * asymmetry is what defect 9 was. */
bool send_error_is_closure(fnet_error_t error);

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
	/* FNET does not use the POSIX "zero means EOF" rule. _fnet_tcp_rcv()
	 * falls through to `return (fnet_int32_t)len;` with len == 0 for a
	 * connected socket whose receive buffer is empty (fnet_tcp.c:776), so a
	 * zero return is "nothing yet". Reading it as closure is what answered a
	 * POST whose body landed in a second segment with a zero-byte close: the
	 * read between the two segments tore the connection down.
	 */
	if (result == 0) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	/* Closure is FNET_ERR carrying any of the codes send_error_is_closure()
	 * enumerates -- the SAME set as the send path, and for the same reason.
	 *
	 * `_fnet_tcp_rcv()` itself only ever fails with CONNCLOSED (once the FIN has
	 * been received and the buffer is drained, fnet_tcp.c:768-772) or NOTCONN
	 * (once the socket has left SS_CONNECTED, :754-757), and those two were all
	 * this arm recognised. But `_fnet_socket_set_error()` **substitutes** a
	 * socket's latched `local_error` for whatever code the failing call computed
	 * (fnet_socket.c:104-108), so an ICMP- or timer-latched CONNRESET, TIMEDOUT,
	 * CONNABORTED, HOSTUNREACH, NOPROTOOPT or MSGSIZE can surface here too. Every
	 * one of them means the connection is gone, so mapping them to FAULT tore down
	 * a transaction that should simply have ended.
	 *
	 * MSGSIZE is the clearest case, and it is why the shared set is right rather
	 * than merely convenient: a receive cannot compute MSGSIZE for a TCP socket at
	 * all, so seeing it here can only be the latch, and therefore can only mean the
	 * connection.
	 *
	 * Sharing the predicate with the send path is deliberate. The two directions
	 * had drifted apart once already -- that asymmetry is what defect 9 was -- and
	 * one enumeration cannot drift from itself.
	 *
	 * AGAIN is kept because the contract names it, though this path never
	 * produces it. */
	const int error = fnet_error_get();
	if (send_error_is_closure((fnet_error_t)error)) {
		return io_event(AIUS_IO_CLOSED);
	}
	return error == FNET_ERR_AGAIN
		       ? io_event(AIUS_IO_WOULD_BLOCK)
		       : io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET);
}

/* Whether an FNET_ERR from this socket's send means the connection is gone
 * rather than that the socket is broken.
 *
 * The set is larger than the receive side's two codes, and the reason is
 * _fnet_socket_set_error() (fnet_socket.c:102-113): when a socket carries a
 * latched `local_error` it *substitutes* that code for the one the failing call
 * chose. TCP latches one on an ICMP report or a timer expiry --
 * FNET_ERR_MSGSIZE for a forced fragmentation drop, FNET_ERR_HOSTUNREACH for
 * the three unreachable notifications, FNET_ERR_NOPROTOOPT for a parameter
 * problem, FNET_ERR_CONNRESET for an unreachable port or protocol
 * (fnet_tcp.c:359-377), FNET_ERR_CONNABORTED and FNET_ERR_TIMEDOUT from the
 * abort and connection timers (2816-2853) -- so the code a failed send reports
 * is not reliably the code that send computed.
 *
 * What makes every one of them a closure here is that the substitution only
 * fires on a call that is *already* failing, and on this socket the only
 * reachable structural failure is "no longer connected": the length check
 * (fnet_tcp.c:810-814, FNET_ERR_INVAL above 0xfff0000 bytes) cannot trigger
 * because every write is at most one AIUS_HTTP_TX_QUOTA of 512 bytes, the null
 * buffer and bad descriptor rows are refused by this adapter before FNET sees
 * them, FNET_ERR_SHUTDOWN needs a fnet_socket_shutdown() this adapter never
 * calls, and FNET_ERR_OPNOTSUPP needs a protocol with no prot_snd. Four of the
 * latch codes close the socket themselves (fnet_tcp.c:375, 2820, 2853, which
 * sets SS_CLOSED and zeroes the foreign address at 3991-3992); the other two
 * surface later, on the first call that fails because a timer already did.
 *
 * FNET_ERR_CONNCLOSED is not produced on the send path at all -- it is
 * _fnet_tcp_rcv()'s drained-after-FIN code -- and is listed so the two
 * directions classify the same code the same way.
 */
bool send_error_is_closure(fnet_error_t error)
{
	switch (error) {
	case FNET_ERR_NOTCONN:
	case FNET_ERR_CONNCLOSED:
	case FNET_ERR_CONNRESET:
	case FNET_ERR_CONNABORTED:
	case FNET_ERR_TIMEDOUT:
	case FNET_ERR_HOSTUNREACH:
	case FNET_ERR_NOPROTOOPT:
	case FNET_ERR_MSGSIZE:
	case FNET_ERR_SHUTDOWN:
		return true;
	default:
		return false;
	}
}

/* The send half of read_once()'s asymmetry, closed at the same seam: FNET
 * directly, not through EthernetClient.
 *
 * EthernetClass::socketSend() answers 0 for a would-block and 0 for every
 * FNET_ERR alike (Nativesocket.cpp:327-334, printing the error to Serial from
 * the epoch loop on the way), and EthernetClient::write() calls
 * setWriteError() on the collapsed zero (NativeEthernetClient.cpp:196-199). So
 * a peer that closed mid-response and a netheap that could not spare a netbuf
 * for this segment both arrived here as a socket fault. The second was the live
 * one: a socket fault abandons the response and cleans the transaction up
 * (aius_http_transaction.c:1177), so a shortage that the next epoch would have
 * cleared truncated a reply the client had already been promised.
 */
AiusIoEvent write_once(const uint8_t *buffer, size_t length)
{
	const uint8_t socket = g_client.getSocketNumber();
	if (socket >= MAX_SOCK_NUM || Ethernet.socket_ptr[socket] == nullptr) {
		return io_event(AIUS_IO_CLOSED);
	}
	const fnet_ssize_t result = fnet_socket_send(
		Ethernet.socket_ptr[socket], buffer, (fnet_size_t)length, 0);
	if (result > 0) {
		return io_event(AIUS_IO_PROGRESS, (size_t)result);
	}
	/* A zero is the whole would-block family and nothing else. _fnet_tcp_snd()
	 * answers it when the output buffer has no room, when the netheap cannot
	 * hand out one and a half Ethernet MTUs so the window collapses to zero
	 * whatever the buffer reports (fnet_tcp.c:828-851), when
	 * _fnet_netbuf_from_buf() fails outright (910-913) and when appending the
	 * record fails (956-960). Every one of them is this instant's shortage: the
	 * segment was not queued, the socket is untouched, and the next epoch's
	 * write may take it.
	 */
	if (result == 0) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	const fnet_error_t error = fnet_error_get();
	/* Unreachable from _fnet_tcp_snd(), kept because the contract names it --
	 * the same arm, for the same reason, as on the receive side. */
	if (error == FNET_ERR_AGAIN) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	if (send_error_is_closure(error)) {
		return io_event(AIUS_IO_CLOSED);
	}
	return io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET);
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

/* Whether a datagram's source address can be this socket's reply destination.
 *
 * FNET performs no source-address validation whatsoever. _fnet_ip4_input_low()
 * checks the length, the version, the header length, the checksum and the
 * *destination* address (fnet_ip4.c:556-576); _fnet_udp_input() demultiplexes
 * on the local address and port only (fnet_udp.c:269-345); and
 * _fnet_ip4_set_socket_addr() copies the source word out of the header verbatim
 * (fnet_ip4.c:486-492). So whatever a sender puts in the source field of a
 * datagram aimed at the control port arrives here as the candidate peer, from
 * any host on the segment, with no credential.
 *
 * Four kinds of source cannot be a reply destination:
 *
 *   - a family other than AF_INET, which the reply's own
 *     _fnet_socket_addr_check_len() answers FNET_ERR_AFNOSUPPORT for
 *     (fnet_socket.c:1874-1885). A zeroed sockaddr reads as family 0 and lands
 *     here, which is what a peer never written by any receive looks like;
 *   - the unspecified address, 0.0.0.0, which the reply answers
 *     FNET_ERR_DESTADDRREQ for (fnet_socket.c:988-992) -- tested with FNET's own
 *     predicate rather than a local comparison;
 *   - the limited broadcast address and any multicast group, because a reply to
 *     either turns one unauthenticated datagram into a frame every host on the
 *     segment receives;
 *   - port 0, which no reply can be delivered to.
 *
 * Refusing to latch is the fix at the cause. The alternative -- latch nothing
 * but deliver the datagram -- was not taken: the control protocol answers every
 * request, so admitting one whose acknowledgement cannot be addressed either
 * loses the answer or sends it to whichever legitimate peer was latched before,
 * and the dispatcher has no way to know which. A request that cannot be answered
 * is not serviceable, so the epoch is told there was nothing to service and the
 * counter carries the drop.
 */
bool peer_usable(const fnet_sockaddr *peer)
{
	fnet_ip4_addr_t address = 0u;

	if ((peer->sa_family & AF_INET) == 0u) {
		return false;
	}
	if (fnet_socket_addr_is_unspecified(peer) != FNET_FALSE) {
		return false;
	}
	if (peer->sa_port == 0u) {
		return false;
	}
	if (fnet_socket_addr_is_multicast(peer) != FNET_FALSE) {
		return false;
	}
	memcpy(&address, peer->sa_data, sizeof(address));
	return address != INADDR_BROADCAST;
}

AiusIoEvent udp_receive_once(uint8_t *buffer, size_t capacity)
{
	if (g_udp.sockindex >= MAX_SOCK_NUM) {
		return io_event(AIUS_IO_CLOSED);
	}
	/* The foreign address goes into a local rather than straight into the
	 * latch, and that is structural: _fnet_socket_buffer_read_address()
	 * copies the address at fnet_socket.c:1825 and only then decides whether
	 * the datagram fitted (:1827-1830), so handing it the latch let a *dropped*
	 * oversized datagram repoint the reply destination of a conversation it was
	 * never part of. A receive that delivers nothing now cannot touch the peer
	 * at all, whatever FNET writes.
	 */
	fnet_sockaddr source = {};
	fnet_size_t peer_size = sizeof(source);
	const fnet_ssize_t result =
		fnet_socket_recvfrom(Ethernet.socket_ptr[g_udp.sockindex], buffer,
		                    capacity, 0, &source, &peer_size);
	if (result > 0) {
		if (!peer_usable(&source)) {
			g_udp_unusable_peer_drops++;
			return io_event(AIUS_IO_WOULD_BLOCK);
		}
		g_udp_peer = source;
		g_udp_peer_valid = true;
		return io_event(AIUS_IO_PROGRESS, (size_t)result);
	}
	/* An empty datagram queue is a plain 0 and nothing else
	 * (fnet_socket.c:1845-1848). FNET has no POSIX EOF here either, but unlike
	 * the TCP path this one never had the closure reading to lose.
	 */
	if (result == 0) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	const fnet_error_t error = fnet_error_get();
	/* MSGSIZE is the only error this receive can actually raise, and it is
	 * rejected *input* rather than a broken socket: a datagram larger than
	 * `capacity` makes _fnet_socket_buffer_read_address() answer FNET_ERR
	 * (fnet_socket.c:1827-1830) after it has already dequeued the datagram
	 * (:1832-1843), so the packet is gone whatever is concluded here. Any host
	 * on the segment can send one to the control port with no credential, so
	 * calling it a socket fault let an unauthenticated peer raise one transport
	 * fault per packet. The only reason that was not already a remote network
	 * teardown is that the UDP checkpoint discards every non-PROGRESS result it
	 * gets (optical_main.cpp platform_udp) -- so the misclassification was
	 * silent rather than harmless, and the first consumer to act on this fault
	 * would have inherited the teardown. Drop it, count it, and answer the epoch
	 * honestly: there is nothing to dispatch. The counter is what stops the drop
	 * being silent in the other direction.
	 */
	if (error == FNET_ERR_MSGSIZE) {
		g_udp_oversize_drops++;
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	/* Not reachable from _fnet_udp_rcv(); kept because the contract names it. */
	if (error == FNET_ERR_AGAIN) {
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
	/* _fnet_udp_snd() has no zero return -- it answers the whole length or
	 * FNET_ERR (fnet_udp.c:602-611) -- so this arm exists for the contract.
	 */
	if (result == 0) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	const fnet_error_t error = fnet_error_get();
	/* A netheap with no netbuf to spare for this datagram is a transient
	 * shortage, not a dead socket: _fnet_netbuf_from_buf() failing is
	 * FNET_ERR/NOMEM (fnet_udp.c:543-547) and so is the IP header allocation
	 * one layer down (fnet_ip4.c:252-256). The next epoch's send may take it,
	 * so a socket fault would be this transport declaring its own socket
	 * unusable over a condition that clears itself. AGAIN is unreachable on this
	 * path and kept for the contract, exactly as on the receive side.
	 *
	 * FNET_ERR_DESTADDRREQ is the same family one step further out: the latched
	 * peer carries no usable address, so this datagram has nowhere to go
	 * (fnet_socket.c:988-992). That is a property of the destination, not of the
	 * socket -- the socket sends normally the moment a usable peer is learned --
	 * so it is a withheld send, exactly like the no-peer-yet arm above, and not
	 * a transport fault an unauthenticated sender could raise at will. The
	 * receive's peer_usable() gate now makes it unreachable from here, which is
	 * the point: the cause and the classification are fixed independently, so
	 * neither depends on the other having been right.
	 */
	if (error == FNET_ERR_NOMEM || error == FNET_ERR_AGAIN ||
	    error == FNET_ERR_DESTADDRREQ) {
		return io_event(AIUS_IO_WOULD_BLOCK);
	}
	return io_event(AIUS_IO_FAULT, 0u, AIUS_NET_FAULT_SOCKET);
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

/* Single-context FNET: with AIUS_NET_POLL_SINGLE_CONTEXT=1 no IntervalTimer
 * is armed (vendor/NativeEthernet), and this is the only steady-state driver
 * of the service poll -- main-loop context, once per millisecond. The server
 * spin inside NativeEthernetServer still polls inline, but from this same
 * context, so fnet_poll can no longer re-enter: the no-op FNET mutexes are
 * sound again. Conviction chain: open-work register, periodic-WDOG entry. */
#if AIUS_NET_POLL_SINGLE_CONTEXT
void stack_poll(uint32_t now_ms)
{
	static uint32_t last_poll_ms;

	if (now_ms == last_poll_ms)
		return;
	last_poll_ms = now_ms;
	EthernetClass::pollProbed(AIUS_POLL_CTX_NETWORK_JOB);
}
#endif

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
#if AIUS_NET_POLL_SINGLE_CONTEXT
	stack_poll,
#else
	nullptr,
#endif
};

FLASHMEM bool dhcp_options(uint32_t *lease_s, uint32_t *t1_s,
                           uint32_t *t2_s)
{
	return Ethernet.dhcpOptions(lease_s, t1_s, t2_s);
}

FLASHMEM bool stack_heap(uint32_t *free_bytes, uint32_t *largest_block)
{
	return Ethernet.stackHeapStatus(free_bytes, largest_block);
}

const AiusNetworkTeensyDiagPrimitives kProductionDiagPrimitives = {
	netif_initialized,
	link_live,
	dhcp_state,
	frames,
	uptime_ms,
	dhcp_options,
	stack_heap,
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

FLASHMEM bool diag_dhcp_options(uint32_t *lease_s, uint32_t *t1_s,
                                uint32_t *t2_s)
{
	return g_diag_primitives != nullptr &&
	       g_diag_primitives->dhcp_options != nullptr &&
	       g_diag_primitives->dhcp_options(lease_s, t1_s, t2_s);
}

FLASHMEM bool diag_stack_heap(uint32_t *free_bytes, uint32_t *largest_block)
{
	return g_diag_primitives != nullptr &&
	       g_diag_primitives->stack_heap != nullptr &&
	       g_diag_primitives->stack_heap(free_bytes, largest_block);
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

/* --- Receive-readiness gate -----------------------------------------------
 *
 * Between link-up and STACK_STARTED, for the one generation that actually
 * initialised the interface. Everything here runs in the epoch's action or poll
 * path, never in a callback and never per audio block, so each definition
 * carries FLASHMEM.
 */

FLASHMEM void rx_ready_release(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                           AiusNetworkTeensyRxReady source)
{
	adapter->rx_ready_pending = false;
	adapter->rx_ready_baseline_valid = false;
	adapter->rx_ready_baseline = 0u;
	adapter->diag.rx_ready_source = (uint8_t)source;
	adapter->diag.rx_ready_ms = diag_offset(adapter, now_ms);
}

/* One link-up observation, from whichever of the three observers saw it first.
 *
 * Returns whether STACK_STARTED may be emitted now. A generation that skipped
 * fnet_netif_init() inherits an interface that is already carrying traffic and
 * releases here, in the same call it always did. A generation that performed the
 * initialisation has an interface that reports link up several seconds before
 * the FEC passes a frame in either direction, so it opens the readiness gate
 * instead and poll() releases it.
 */
FLASHMEM bool link_up_observed(AiusNetworkTeensy *adapter, uint32_t now_ms,
	                           AiusNetworkTeensyLinkUpSource source)
{
	adapter->link_up_reported = true;
	diag_note_link_up(adapter, now_ms, source);
	if (!adapter->rx_ready_required) {
		adapter->diag.rx_ready_source =
			(uint8_t)AIUS_NETWORK_TEENSY_RX_READY_NOT_REQUIRED;
		return true;
	}
	adapter->rx_ready_pending = true;
	adapter->rx_ready_baseline_valid = false;
	adapter->rx_ready_baseline = 0u;
	return false;
}

/* One epoch of the wait, spending the epoch's single underlying read on the
 * frame counters rather than on the link status. Returns whether the gate
 * released.
 *
 * The baseline is sampled lazily, on the first poll of the wait, so opening the
 * gate costs no call of its own. Movement is an unsigned difference against it:
 * these are free-running MAC counters that wrap, and a wrap is traffic, not the
 * absence of it. A driver that refuses the statistics call leaves the deadline
 * as the only release, which is the same bounded fallback a silent segment gets.
 */
FLASHMEM bool rx_ready_step(AiusNetworkTeensy *adapter, uint32_t now_ms)
{
	uint32_t tx = 0u;
	uint32_t rx = 0u;

	if (diag_frames(&tx, &rx)) {
		if (!adapter->rx_ready_baseline_valid) {
			adapter->rx_ready_baseline = rx;
			adapter->rx_ready_baseline_valid = true;
		} else if (rx - adapter->rx_ready_baseline != 0u) {
			rx_ready_release(adapter, now_ms,
			                 AIUS_NETWORK_TEENSY_RX_READY_COUNTER);
			return true;
		}
	}
	if (deadline_reached(now_ms,
		                 adapter->stack_started_ms +
		                     AIUS_NETWORK_TEENSY_RX_READY_DEADLINE_MS)) {
		rx_ready_release(adapter, now_ms,
		                 AIUS_NETWORK_TEENSY_RX_READY_DEADLINE);
		return true;
	}
	return false;
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
		adapter->rx_ready_required = false;
		adapter->rx_ready_pending = false;
		adapter->rx_ready_baseline_valid = false;
		adapter->rx_ready_baseline = 0u;
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
		adapter->rx_ready_pending = false;
		adapter->rx_ready_baseline_valid = false;
		adapter->rx_ready_baseline = 0u;
		adapter->stack_started_ms = now_ms;
		diag_begin(adapter, action.generation, now_ms);
		g_primitives->callbacks_install(adapter, action.generation);
		adapter->stack_started = true;
		/* The one netif-init probe of the generation, and the only place
		 * the readiness requirement is decided. Measured around the call
		 * rather than after it, because beginNoWait() is only "no wait"
		 * on a generation that finds the interface already initialised:
		 * the one that runs fnet_netif_init() resets the PHY and blocks
		 * in FNET's autonegotiation wait for up to two seconds. Neither
		 * reading changes what the call does or when it is made.
		 *
		 * The decision reads these locals rather than the record it also
		 * fills: the record stays diagnostic, and an adapter with no
		 * measurement table requires no wait at all.
		 */
		const bool netif_probed = g_diag_primitives != nullptr;
		const bool netif_already_up =
			netif_probed && g_diag_primitives->netif_initialized();
		adapter->rx_ready_required = netif_probed && !netif_already_up;
		adapter->diag.netif_init =
			!netif_probed
				? AIUS_NETWORK_TEENSY_NETIF_INIT_UNKNOWN
			: netif_already_up
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
		 *
		 * Nor is link-up evidence that the interface passes traffic.
		 * The generation that initialised it reads up here and still
		 * cannot carry a DISCOVER, so it opens the readiness gate and
		 * poll() releases STACK_STARTED; only the generation that
		 * inherits a settled interface releases in this call.
		 */
		const AiusNetworkTeensyLink link = g_primitives->link_status();
		adapter->diag.link_at_stack_start = (uint8_t)link;
		adapter->diag.link_cached_last = (uint8_t)link;
		if (link == AIUS_NETWORK_TEENSY_LINK_UP &&
		    link_up_observed(adapter, now_ms,
		                     AIUS_NETWORK_TEENSY_LINK_UP_STACK_START)) {
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
	if (g_primitives->stack_poll != nullptr) {
		g_primitives->stack_poll(now_ms);
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
			/* Link-up is observed here, but a generation that
			 * initialised the interface does not pass STACK_STARTED
			 * on: the readiness gate opens instead and the drain
			 * continues, so the callback is consumed rather than
			 * delivered early.
			 */
			if (!link_up_observed(
				    adapter, now_ms,
				    AIUS_NETWORK_TEENSY_LINK_UP_CALLBACK)) {
				continue;
			}
		}
		*event = queued;
		return;
	}

	if (!adapter->stack_started) {
		return;
	}

	/* The readiness wait owns this epoch's one underlying read while it is
	 * open, so the counter read never joins the link read -- it replaces it.
	 * Link loss in this window still reaches the supervisor through the
	 * NativeEthernet link callback drained above.
	 */
	if (adapter->rx_ready_pending) {
		if (rx_ready_step(adapter, now_ms)) {
			emit(event, AIUS_NET_EVENT_STACK_STARTED,
			     adapter->generation);
		}
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
	 *
	 * Observing link-up may open the readiness gate rather than release
	 * STACK_STARTED, in which case the next epoch's read is the counter one.
	 */
	if (link == AIUS_NETWORK_TEENSY_LINK_UP) {
		if (link_up_observed(adapter, now_ms,
			                 AIUS_NETWORK_TEENSY_LINK_UP_POLL)) {
			emit(event, AIUS_NET_EVENT_STACK_STARTED,
			     adapter->generation);
		}
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

FLASHMEM bool aius_network_teensy_dhcp_options(
	const AiusNetworkTeensy *adapter, uint32_t *lease_s, uint32_t *t1_s,
	uint32_t *t2_s)
{
	if (lease_s == nullptr || t1_s == nullptr || t2_s == nullptr) {
		return false;
	}
	*lease_s = 0u;
	*t1_s = 0u;
	*t2_s = 0u;
	if (adapter == nullptr || !adapter->stack_started ||
	    !primitives_ready()) {
		return false;
	}
	return diag_dhcp_options(lease_s, t1_s, t2_s);
}

FLASHMEM bool aius_network_teensy_stack_heap(
	const AiusNetworkTeensy *adapter, uint32_t *free_bytes,
	uint32_t *largest_block)
{
	if (free_bytes == nullptr || largest_block == nullptr) {
		return false;
	}
	*free_bytes = 0u;
	*largest_block = 0u;
	if (adapter == nullptr || !adapter->stack_started ||
	    !primitives_ready()) {
		return false;
	}
	return diag_stack_heap(free_bytes, largest_block);
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

FLASHMEM uint32_t aius_network_teensy_udp_oversize_drops()
{
	return g_udp_oversize_drops;
}

FLASHMEM uint32_t aius_network_teensy_udp_unusable_peer_drops()
{
	return g_udp_unusable_peer_drops;
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
