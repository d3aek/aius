/* The repository-pinned NativeEthernet transport, driven against a stand-in for
 * FNET 4.7.1 that is modelled on the dependency rather than on its caller.
 *
 * Read fake/fnet.h first: it explains why that distinction matters and cites the
 * real header behind every declaration. The rule this file follows is that a
 * fake outcome must be a *consequence* of modelled socket state, not a scalar
 * the test picked. `recv_result = 0` was a scalar the test picked, and it
 * asserted read_once()'s connection-tearing defect as correct behaviour for
 * months. Two deliberate injection hooks remain (force_recv_error,
 * force_send_error); each is labelled with what it exists to probe and why the
 * state model cannot reach it.
 */

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "NativeDns.h"
#include "NativeEthernet.h"
#include "aius_network_teensy.h"
#include "utility/NativeW5100.h"

FakeSerialClass Serial;

/* The constants, checked at compile time because a wrong number has no
 * behavioural witness to test: nothing in this transport reads back a value it
 * did not itself write, so the fake and the caller can agree on a number the
 * dependency has never heard of. Every one of these disagreed with real FNET
 * before this file was rewritten, and the build is now what rejects the drift.
 * Line references are to the FNET 4.7.1 headers cited in fake/fnet.h. */
static_assert(AF_INET == 1U, "fnet_socket.h:151 -- 2 is AF_INET6");
static_assert(AF_INET6 == 2U, "fnet_socket.h:155");
static_assert(SOL_SOCKET == 255255, "fnet_socket.h:367");
static_assert(SOCK_STREAM == 1U && SOCK_DGRAM == 2U, "fnet_socket.h:321-325");
/* fnet_socket.h:478-631, positionally. The earlier fake had all ten wrong and
 * aliased three IPPROTO_IP options onto SOL_SOCKET ones. */
static_assert(SO_LINGER == 3, "fnet_socket.h:493");
static_assert(SO_SNDBUF == 5 && SO_RCVBUF == 6, "fnet_socket.h:502-505");
static_assert(SO_STATE == 7 && SO_ERROR == 8, "fnet_socket.h:506-512");
static_assert(SO_RCVNUM == 10 && SO_SNDNUM == 11, "fnet_socket.h:515-520");
static_assert(TCP_NODELAY == 16, "fnet_socket.h:562");
static_assert(IP_TTL == 23 && IP_MULTICAST_TTL == 24, "fnet_socket.h:604-610");
static_assert(IP_ADD_MEMBERSHIP == 25, "fnet_socket.h:611");
static_assert(static_cast<int>(SO_STATE) != static_cast<int>(IP_TTL),
              "the fake once numbered these the same and ignored `level`");
static_assert(MSG_PEEK == 0x2U, "fnet_socket.h:668");
static_assert(SD_READ == 0x1U && SD_WRITE == 0x2U,
              "fnet_socket.h:684-694 -- 1 is SD_READ, not SD_WRITE");
static_assert(FNET_SOCKET_EVENT_IN == 1 && FNET_SOCKET_EVENT_OUT == 2,
              "fnet_socket.h:703-704 -- the fake once had these swapped");
static_assert(FNET_SOCKET_EVENT_ALL == 7, "fnet_socket.h:708, not 0xff");
static_assert(SS_CLOSED == 0 && SS_LISTENING == 4, "fnet_socket.h:342-349");
static_assert(FNET_OK == 0 && FNET_ERR == -1, "fnet_user_config.h:62-63");
static_assert(FNET_ERR_AGAIN == -4 && FNET_ERR_NOTCONN == -20 &&
                  FNET_ERR_CONNCLOSED == -26 && FNET_ERR_BAD_DESC == -28,
              "fnet_error.h:54,138,163,176");
static_assert(FNET_CFG_SOCKET_MAX == 12,
              "fnet_stack_config.h:486 -- 8 is the vendor's MAX_SOCK_NUM");
/* port/cpu/fnet_cpu.h:83-93: the CPU is little-endian, so this is a byte swap.
 * The earlier fake used the big-endian branch's identity macro while its
 * FNET_IP4_ADDR_INIT expanded to the little-endian result. */
static_assert(FNET_HTONS(80u) == 0x5000u, "FNET_HTONS is not the identity");
static_assert(FNET_IP4_ADDR_INIT(192, 168, 4, 21) == 0x1504a8c0u,
              "fnet_ip4.h:47 over a little-endian FNET_NTOHL");
/* FNET_CFG_IP6 == 0, so both address structures are 12 bytes -- the threshold
 * _fnet_socket_addr_check_len() (fnet_socket.c:1876) enforces. The earlier
 * fake's fnet_sockaddr was 6, below its own dependency's minimum. */
static_assert(sizeof(struct fnet_sockaddr) == 12u, "fnet_socket.h:268-280");
static_assert(sizeof(struct fnet_sockaddr_in) == 12u, "fnet_socket.h:210-219");
/* The target is ILP32, so FNET's `unsigned long`/`long` are 32 bits there. */
static_assert(sizeof(fnet_size_t) == 4u && sizeof(fnet_ssize_t) == 4u,
              "fnet_stdlib.h:55-60 at the target's widths");

namespace {

/* Socket buffer defaults, so a fresh descriptor starts where FNET's own
 * configuration puts it rather than at zero: fnet_user_config.h:16-17 raises the
 * TCP buffers to 32 KiB, stack/fnet_stack_config.h:574-585 leaves UDP at 2 KiB.
 * EthernetClass::socketBegin() then setopts both to Ethernet.socket_size
 * (Nativesocket.cpp:103-104), which is the value any test here actually sees. */
constexpr fnet_size_t kTcpBufferDefault = 32u * 1024u;
constexpr fnet_size_t kUdpBufferDefault = 2u * 1024u;
/* stack/fnet_tcp.h:127. */
constexpr fnet_size_t kTcpMaxBuffer = 0xfff0000u;
/* stack/fnet_tcp.c:833 -- a netheap that cannot hand out one and a half
 * Ethernet MTUs (stack/fnet_eth.h:52) collapses the send window to zero
 * however much room the socket buffer reports. */
constexpr fnet_size_t kNetbufFloor = 1500u + 1500u / 2u;
constexpr fnet_size_t kNetheapPlenty = 64u * 1024u;
constexpr size_t kSocketDataCapacity = 2048u;

/* The observable members of fnet_socket_if_t: the socket state SO_STATE reports,
 * the two buffer accounts SO_SNDBUF/SO_SNDNUM/SO_RCVBUF/SO_RCVNUM report, the
 * FIN latch _fnet_tcp_rcv() consults, and the shutdown flags the socket-layer
 * wrappers consult. */
struct FakeFnetSocket {
    bool allocated;
    fnet_socket_type_t type;
    fnet_socket_state_t state;
    fnet_uint16_t local_port;
    bool peer_known;
    bool fin_received;
    bool recv_shutdown;
    bool send_shutdown;
    fnet_error_t error;
    fnet_size_t send_max;
    fnet_size_t send_count;
    fnet_size_t recv_max;
    fnet_size_t recv_count;
    fnet_size_t recv_read;
    uint8_t recv_data[kSocketDataCapacity];
};

struct FakeState {
    int fnet_init_calls;
    int fnet_init_failures;
    int illegal_fnet_init_calls;
    int netif_init_calls;
    int netif_failures;
    int link_init_calls;
    int link_init_failures;
    int link_release_calls;
    int dhcp_init_calls;
    int dhcp_release_calls;
    int service_register_calls;
    int service_unregister_calls;
    int socket_create_calls;
    int socket_close_calls;
    int socket_close_failures;
    int send_calls;
    int sendto_calls;
    int recv_calls;
    int peek_calls;
    uint32_t ip4_address;
    int cleanup_callback_injections;
    int callback_socket_api_calls;
    bool fnet_active;
    bool netif_initialized;
    bool dhcp_enabled;
    bool inject_callbacks_during_cleanup;
    bool inside_cleanup_callback;
    /* PHY link state. False by default, which is what a real cold start looks
     * like: fnet_netif_init() has just released the PHY and nothing has
     * negotiated yet. */
    bool link_connected;
    /* FNET's own DHCP client state and the MAC's frame counters, the two
     * things the adapter's diagnostics ask the vendor stack for. */
    fnet_uint8_t dhcp_state;
    int dhcp_state_calls;
    bool statistics_available;
    int statistics_calls;
    fnet_uint32_t tx_packet;
    fnet_uint32_t rx_packet;
    fnet_link_callback_t link_callback;
    void *link_cookie;
    fnet_dhcp_cln_callback_t dhcp_callback;
    void *dhcp_cookie;
    fnet_service_poll_t service_callback;
    void *service_cookie;

    /* --- the modelled stack ------------------------------------------- */
    /* FNET's one global error word. Written by the fake wherever real FNET
     * writes it, so a classification the adapter makes is a consequence of the
     * operation rather than of a value the test set. */
    fnet_error_t socket_error;
    FakeFnetSocket sockets[FNET_CFG_SOCKET_MAX];
    /* _fnet_malloc_max_netbuf(), the netheap headroom fnet_tcp.c:831 reads. */
    fnet_size_t netheap_max;
    /* Completed connections waiting in the listener's accept queue. */
    int pending_connections;
    /* The two probes into paths the state model cannot reach. Real FNET's
     * receive and send paths never raise FNET_ERR_AGAIN and never raise
     * FNET_ERR_INVAL for the arguments this adapter passes, but the adapter's
     * classification switch has arms for them and for the default, and those
     * arms are worth covering. */
    fnet_error_t force_recv_error;
    fnet_error_t force_send_error;
    /* What EthernetServer::poll() handed fnet_socket_accept() as *addrlen. */
    fnet_size_t accept_addrlen_seen;
    bool accept_addrlen_seen_valid;
    bool accept_addrlen_enforced;

    std::vector<std::string> events;
} fake = {};

int user_link_callbacks;
int user_dhcp_callbacks;

void reset_socket_model()
{
    for (auto &socket : fake.sockets) {
        socket = FakeFnetSocket{};
    }
    fake.socket_error = FNET_ERR_OK;
    fake.netheap_max = kNetheapPlenty;
    fake.pending_connections = 0;
    fake.force_recv_error = FNET_ERR_OK;
    fake.force_send_error = FNET_ERR_OK;
    fake.accept_addrlen_seen = 0u;
    fake.accept_addrlen_seen_valid = false;
    /* Off by default, and the reason is a defect rather than convenience:
     * EthernetServer::poll() declares `fnet_size_t fromlen;` and passes it
     * without ever assigning it (NativeEthernetServer.cpp:339-340), while real
     * fnet_socket_accept() reads *addrlen and rejects 1..11 with
     * FNET_ERR_INVAL (fnet_socket.c:896-902 via _fnet_socket_addr_check_len,
     * :1856). Enforcing that here would make this suite's verdict depend on
     * whatever the host stack left in that slot -- the same lottery the board
     * runs. Measured on this host it is 0, one of only two values that survive
     * the check (0 skips it, >= 12 passes it), and it is 0 by luck rather than
     * by construction. The rule is implemented and proved directly by
     * test_accept_addrlen_contract(); the vendor-driven path records the length
     * it was given in accept_addrlen_seen instead. */
    fake.accept_addrlen_enforced = false;
}

void observe_socket_access()
{
    if (fake.inside_cleanup_callback)
        fake.callback_socket_api_calls++;
}

template <typename Callback, typename... Args>
void inject_cleanup_callback(Callback callback, Args... args)
{
    if (!fake.inject_callbacks_during_cleanup || callback == nullptr)
        return;
    fake.cleanup_callback_injections++;
    fake.inside_cleanup_callback = true;
    callback(args...);
    fake.inside_cleanup_callback = false;
}

void record(const char *event)
{
    fake.events.emplace_back(event);
}

size_t event_index(const char *event, size_t start = 0)
{
    assert(start <= fake.events.size());
    auto found = std::find(
        fake.events.begin() + static_cast<ptrdiff_t>(start),
        fake.events.end(), event);
    assert(found != fake.events.end());
    return static_cast<size_t>(found - fake.events.begin());
}

/* --- the modelled socket layer ----------------------------------------- */

fnet_socket_t socket_descriptor(size_t index)
{
    return reinterpret_cast<fnet_socket_t>(index + 1u);
}

/* _fnet_socket_desc_find() (fnet_socket.c:203): a descriptor that is not a live
 * socket answers nothing, and every caller turns that into FNET_ERR_BAD_DESC. */
FakeFnetSocket *socket_find(fnet_socket_t socket)
{
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(socket);
    if (encoded == 0u || encoded > FNET_CFG_SOCKET_MAX)
        return nullptr;
    FakeFnetSocket *found = &fake.sockets[encoded - 1u];
    return found->allocated ? found : nullptr;
}

void set_error(fnet_error_t error)
{
    fake.socket_error = error;
}

/* _fnet_socket_set_error() (fnet_socket.c:102): a socket-scoped failure writes
 * both the socket's own SO_ERROR and the one global word. */
void set_socket_error(FakeFnetSocket *socket, fnet_error_t error)
{
    socket->error = error;
    fake.socket_error = error;
}

struct fnet_sockaddr test_peer_address()
{
    struct fnet_sockaddr address = {};
    const fnet_ip4_addr_t peer = FNET_IP4_ADDR_INIT(192, 168, 4, 99);

    address.sa_family = AF_INET;
    address.sa_port = FNET_HTONS(51000u);
    std::memcpy(address.sa_data, &peer, sizeof(peer));
    return address;
}

bool address_is_unspecified(const struct fnet_sockaddr *address)
{
    fnet_ip4_addr_t value = 0u;

    std::memcpy(&value, address->sa_data, sizeof(value));
    return value == 0u;
}

/* _fnet_socket_addr_check_len() (fnet_socket.c:1856). A zero length is not
 * checked at all -- the real guard is `if(addr && addr_len)` -- and FNET_CFG_IP6
 * is 0 here, so anything below sizeof(struct fnet_sockaddr_in) is invalid. */
fnet_error_t addr_check_len(fnet_address_family_t family, fnet_size_t length)
{
    if (length == 0u)
        return FNET_ERR_OK;
    if ((family & AF_INET) == 0u)
        return FNET_ERR_AFNOSUPPORT;
    return length < sizeof(struct fnet_sockaddr_in) ? FNET_ERR_INVAL
                                                   : FNET_ERR_OK;
}

/* _fnet_tcp_rcv() (fnet_tcp.c:664-785), in its own order: the copy first, then
 * NOTCONN for an empty read on a socket that has left SS_CONNECTED, then the
 * foreign address, then CONNCLOSED for an empty read after the FIN. A zero
 * return means "nothing yet" and nothing else -- FNET has no POSIX EOF. */
fnet_ssize_t tcp_receive(FakeFnetSocket *socket, void *buffer,
                         fnet_size_t length, fnet_flag_t flags,
                         struct fnet_sockaddr *from)
{
    const bool remove = (flags & MSG_PEEK) == 0u;
    const fnet_size_t pending = socket->recv_count - socket->recv_read;

    if (length > pending) {
        length = pending;
    }
    if (length != 0u) {
        std::memcpy(buffer, socket->recv_data + socket->recv_read, length);
    }
    if (remove) {
        socket->recv_read += length;
    }
    if (length == 0u && socket->state != SS_CONNECTED) {
        set_socket_error(socket, FNET_ERR_NOTCONN);
        return FNET_ERR;
    }
    if (from != nullptr) {
        *from = socket->peer_known ? test_peer_address() : fnet_sockaddr{};
    }
    if (length == 0u && socket->fin_received) {
        set_socket_error(socket, FNET_ERR_CONNCLOSED);
        return FNET_ERR;
    }
    return static_cast<fnet_ssize_t>(length);
}

/* _fnet_udp_rcv() (fnet_udp.c:641-697) over _fnet_socket_buffer_read_address()
 * (fnet_socket.c:1804-1851). One datagram at a time; an empty buffer answers 0
 * with a zeroed address, and a datagram larger than the caller's buffer answers
 * FNET_ERR/FNET_ERR_MSGSIZE *and is still consumed* (:1827-1843). */
fnet_ssize_t udp_receive(FakeFnetSocket *socket, void *buffer,
                         fnet_size_t length, fnet_flag_t flags,
                         struct fnet_sockaddr *from)
{
    const bool remove = (flags & MSG_PEEK) == 0u;
    const fnet_size_t queued = socket->recv_count;

    if (from != nullptr) {
        *from = queued != 0u ? test_peer_address() : fnet_sockaddr{};
    }
    if (queued == 0u) {
        return 0;
    }
    if (length > queued) {
        length = queued;
    }
    std::memcpy(buffer, socket->recv_data, length);
    if (remove) {
        socket->recv_count = 0u;
        socket->recv_read = 0u;
    }
    if (length < queued) {
        set_socket_error(socket, FNET_ERR_MSGSIZE);
        return FNET_ERR;
    }
    return static_cast<fnet_ssize_t>(length);
}

/* _fnet_tcp_snd() (fnet_tcp.c:795-861). Three outcomes matter to the adapter
 * and the vendor collapses two of them: FNET_ERR/NOTCONN once the socket has
 * left SS_CONNECTED, a plain 0 when there is no room, and the accepted byte
 * count otherwise. The room is min(buffer headroom, netheap headroom), so
 * reported capacity can exceed what a send will take. */
fnet_ssize_t tcp_send(FakeFnetSocket *socket, fnet_size_t length)
{
    fnet_size_t freespace;

    if (length > kTcpMaxBuffer) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    if (socket->state != SS_CONNECTED) {
        set_socket_error(socket, FNET_ERR_NOTCONN);
        return FNET_ERR;
    }
    if (length == 0u) {
        return 0;
    }
    freespace = socket->send_max - socket->send_count;
    if (fake.netheap_max < kNetbufFloor) {
        freespace = 0u;
    } else if (freespace > fake.netheap_max) {
        freespace = fake.netheap_max;
    }
    if (freespace < length) {
        if (freespace == 0u) {
            return 0;
        }
        length = freespace;
    }
    socket->send_count += length;
    return static_cast<fnet_ssize_t>(length);
}

/* _fnet_udp_snd() (fnet_udp.c:511-612). A datagram longer than the send buffer
 * is FNET_ERR/MSGSIZE and a netbuf shortage is FNET_ERR/NOMEM; success is the
 * whole length. There is no zero-return would-block on this path at all. */
fnet_ssize_t udp_send(FakeFnetSocket *socket, fnet_size_t length)
{
    if (length > socket->send_max) {
        set_socket_error(socket, FNET_ERR_MSGSIZE);
        return FNET_ERR;
    }
    if (fake.netheap_max < length) {
        set_socket_error(socket, FNET_ERR_NOMEM);
        return FNET_ERR;
    }
    return static_cast<fnet_ssize_t>(length);
}

/* --- harness helpers over the model ------------------------------------ */

fnet_socket_t slot_descriptor(uint8_t slot)
{
    /* EthernetClass::socket_ptr is a volatile array; one read, then value
     * semantics. */
    return Ethernet.socket_ptr[slot];
}

FakeFnetSocket *socket_at_slot(uint8_t slot)
{
    FakeFnetSocket *socket = socket_find(slot_descriptor(slot));
    assert(socket != nullptr);
    return socket;
}

fnet_socket_t descriptor_of(const FakeFnetSocket *socket)
{
    return socket_descriptor(static_cast<size_t>(socket - &fake.sockets[0]));
}

/* The single accepted connection, found the way a test can see it: by state. */
FakeFnetSocket *connected_socket()
{
    FakeFnetSocket *found = nullptr;
    for (auto &socket : fake.sockets) {
        if (socket.allocated && socket.type == SOCK_STREAM &&
            socket.state == SS_CONNECTED) {
            assert(found == nullptr);
            found = &socket;
        }
    }
    assert(found != nullptr);
    return found;
}

FakeFnetSocket *datagram_socket()
{
    FakeFnetSocket *found = nullptr;
    for (auto &socket : fake.sockets) {
        if (socket.allocated && socket.type == SOCK_DGRAM) {
            assert(found == nullptr);
            found = &socket;
        }
    }
    assert(found != nullptr);
    return found;
}

FakeFnetSocket *listening_socket()
{
    FakeFnetSocket *found = nullptr;
    for (auto &socket : fake.sockets) {
        if (socket.allocated && socket.state == SS_LISTENING) {
            assert(found == nullptr);
            found = &socket;
        }
    }
    assert(found != nullptr);
    return found;
}

void deliver(FakeFnetSocket *socket, size_t length)
{
    assert(length <= kSocketDataCapacity);
    for (size_t index = 0; index < length; index++) {
        socket->recv_data[index] = static_cast<uint8_t>(index + 1u);
    }
    socket->recv_count = static_cast<fnet_size_t>(length);
    socket->recv_read = 0u;
}

void set_send_freespace(FakeFnetSocket *socket, fnet_size_t freespace)
{
    assert(freespace <= socket->send_max);
    socket->send_count = socket->send_max - freespace;
}

/* A connected TCP socket, obtained the only way this transport obtains one: a
 * listening EthernetServer, one completed connection in FNET's accept queue,
 * and the FNET service poll that installs it. Nothing here reaches past the
 * vendor's public surface, so the socket's buffer sizes are the ones
 * EthernetClass::socketBegin() chose rather than ones this file invented. */
uint8_t accept_one_connection(EthernetServer &server)
{
    server.begin();
    fake.pending_connections = 1;
    fake.service_callback(fake.service_cookie);
    const FakeFnetSocket *accepted = connected_socket();
    for (uint8_t slot = 0; slot < MAX_SOCK_NUM; slot++) {
        if (socket_find(slot_descriptor(slot)) == accepted)
            return slot;
    }
    assert(false);
    return MAX_SOCK_NUM;
}

void on_link(bool, void *)
{
    user_link_callbacks++;
}

void on_dhcp(void *)
{
    user_dhcp_callbacks++;
}

void test_retry_after_fnet_init_failure()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
    fake.fnet_init_failures = 1;

    assert(!Ethernet.beginNoWait(mac));
    assert(fake.fnet_init_calls == 1);
    assert(fake.netif_init_calls == 0);
    assert(fake.link_init_calls == 0);
    Ethernet.end();
    assert(fake.link_release_calls == 0);
    assert(fake.dhcp_release_calls == 0);
    assert(fake.socket_close_calls == 0);

    assert(Ethernet.beginNoWait(mac));
    assert(fake.fnet_init_calls == 2);
    assert(fake.illegal_fnet_init_calls == 0);
    assert(fake.netif_init_calls == 1);
    assert(fake.link_init_calls == 1);
    Ethernet.end();
    assert(fake.link_release_calls == 1);
}

void test_retry_after_netif_init_failure()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 2};
    fake.netif_failures = 1;

    assert(!Ethernet.beginNoWait(mac));
    assert(fake.fnet_init_calls == 1);
    assert(fake.netif_init_calls == 1);
    assert(fake.link_init_calls == 0);
    Ethernet.end();
    assert(fake.link_release_calls == 0);
    assert(fake.dhcp_release_calls == 0);
    assert(fake.socket_close_calls == 0);

    assert(Ethernet.beginNoWait(mac));
    assert(fake.fnet_init_calls == 1);
    assert(fake.illegal_fnet_init_calls == 0);
    assert(fake.netif_init_calls == 2);
    assert(fake.link_init_calls == 1);
    Ethernet.end();
    assert(fake.link_release_calls == 1);
}

void test_retry_after_link_init_failure()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 3};
    fake.link_init_failures = 1;

    assert(!Ethernet.beginNoWait(mac));
    assert(fake.fnet_init_calls == 1);
    assert(fake.netif_init_calls == 1);
    assert(fake.link_init_calls == 1);
    assert(fake.link_release_calls == 0);
    Ethernet.end();
    assert(fake.link_release_calls == 0);
    assert(fake.dhcp_release_calls == 0);
    assert(fake.socket_close_calls == 0);

    assert(Ethernet.beginNoWait(mac));
    assert(fake.fnet_init_calls == 1);
    assert(fake.illegal_fnet_init_calls == 0);
    assert(fake.netif_init_calls == 1);
    assert(fake.link_init_calls == 2);
    Ethernet.end();
    assert(fake.link_release_calls == 1);
}

void test_dhcp_ownership_and_stale_replacement()
{
    Ethernet.setLinkCallback(on_link);
    Ethernet.setDhcpCallback(on_dhcp);

    assert(Ethernet.dhcpStart(250));
    assert(fake.dhcp_init_calls == 1);
    assert(fake.dhcp_release_calls == 0);
    assert(Ethernet.dhcpStart(250));
    assert(fake.dhcp_init_calls == 1);

    fake.dhcp_enabled = false;
    assert(Ethernet.dhcpStart(250));
    assert(fake.dhcp_release_calls == 1);
    assert(fake.dhcp_init_calls == 2);
    assert(fake.events[fake.events.size() - 2] == "dhcp-release");
    assert(fake.events.back() == "dhcp-init");

    fake.link_callback(nullptr, FNET_TRUE, fake.link_cookie);
    fake.dhcp_callback(
        reinterpret_cast<void *>(0x22), nullptr, fake.dhcp_cookie);
    assert(user_link_callbacks == 1);
    assert(user_dhcp_callbacks == 1);
}

void test_ordered_idempotent_teardown_and_late_callbacks()
{
    EthernetServer server(8080);
    server.begin();
    assert(fake.socket_create_calls == 1);
    assert(fake.service_register_calls == 1);
    server.begin();
    assert(fake.socket_create_calls == 1);
    assert(fake.service_register_calls == 1);

    int service_unregisters = fake.service_unregister_calls;
    int socket_closes = fake.socket_close_calls;
    int link_callbacks = user_link_callbacks;
    int dhcp_callbacks = user_dhcp_callbacks;
    size_t server_teardown_event_start = fake.events.size();
    fake.inject_callbacks_during_cleanup = true;
    server.end();
    assert(fake.service_unregister_calls == service_unregisters + 1);
    assert(fake.socket_close_calls == socket_closes + 1);
    assert(fake.cleanup_callback_injections == 2);
    assert(fake.callback_socket_api_calls == 0);
    assert(user_link_callbacks == link_callbacks);
    assert(user_dhcp_callbacks == dhcp_callbacks);
    assert(Ethernet.linkStatus() == LinkON);
    assert(event_index("service-unregister", server_teardown_event_start) <
           event_index("socket-close", server_teardown_event_start));
    int cleanup_injections = fake.cleanup_callback_injections;
    server.end();
    assert(fake.service_unregister_calls == service_unregisters + 1);
    assert(fake.socket_close_calls == socket_closes + 1);
    assert(fake.cleanup_callback_injections == cleanup_injections);

    server.begin();
    assert(fake.socket_create_calls == 2);
    assert(fake.service_register_calls == 2);

    fnet_link_callback_t late_link = fake.link_callback;
    void *late_link_cookie = fake.link_cookie;
    fnet_dhcp_cln_callback_t late_dhcp = fake.dhcp_callback;
    void *late_dhcp_cookie = fake.dhcp_cookie;
    fnet_service_poll_t late_service = fake.service_callback;
    void *late_service_cookie = fake.service_cookie;

    service_unregisters = fake.service_unregister_calls;
    int link_releases = fake.link_release_calls;
    int dhcp_releases = fake.dhcp_release_calls;
    socket_closes = fake.socket_close_calls;
    cleanup_injections = fake.cleanup_callback_injections;
    link_callbacks = user_link_callbacks;
    dhcp_callbacks = user_dhcp_callbacks;
    size_t teardown_event_start = fake.events.size();
    Ethernet.end();
    assert(fake.service_unregister_calls == service_unregisters + 1);
    assert(fake.link_release_calls == link_releases + 1);
    assert(fake.dhcp_release_calls == dhcp_releases + 1);
    assert(fake.socket_close_calls == socket_closes + 1);
    assert(fake.cleanup_callback_injections == cleanup_injections + 4);
    assert(fake.callback_socket_api_calls == 0);
    assert(user_link_callbacks == link_callbacks);
    assert(user_dhcp_callbacks == dhcp_callbacks);
    assert(Ethernet.linkStatus() == LinkOFF);
    assert(event_index("service-unregister", teardown_event_start) <
           event_index("link-release", teardown_event_start));
    assert(event_index("link-release", teardown_event_start) <
           event_index("dhcp-release-final", teardown_event_start));
    assert(event_index("dhcp-release-final", teardown_event_start) <
           event_index("socket-close", teardown_event_start));

    Ethernet.end();
    assert(fake.service_unregister_calls == service_unregisters + 1);
    assert(fake.link_release_calls == link_releases + 1);
    assert(fake.dhcp_release_calls == dhcp_releases + 1);
    assert(fake.socket_close_calls == socket_closes + 1);

    /* Every teardown above went through fnet_socket_close(), and not one of
     * them was answered with FNET_ERR. That is the whole of the close contract
     * this transport can fail: both prot_detach implementations return FNET_OK
     * on every path (fnet_tcp.c:458-541, fnet_udp.c:462-468), so a descriptor
     * FNET still owns cannot refuse to close. */
    assert(fake.socket_close_failures == 0);

    int socket_activity = fake.socket_create_calls + fake.socket_close_calls;
    late_link(nullptr, FNET_TRUE, late_link_cookie);
    assert(Ethernet.linkStatus() == LinkOFF);
    late_dhcp(reinterpret_cast<void *>(0x22), nullptr, late_dhcp_cookie);
    late_service(late_service_cookie);
    assert(user_link_callbacks == link_callbacks);
    assert(user_dhcp_callbacks == dhcp_callbacks);
    assert(fake.socket_create_calls + fake.socket_close_calls == socket_activity);
    fake.inject_callbacks_during_cleanup = false;
}

void test_destroyed_server_has_no_live_object_cookie()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 2};
    assert(Ethernet.beginNoWait(mac));
    EthernetServer *server = new EthernetServer(8081);
    server->begin();
    fnet_service_poll_t late_service = fake.service_callback;
    void *late_cookie = fake.service_cookie;
    int unregisters = fake.service_unregister_calls;
    int closes = fake.socket_close_calls;
    delete server;
    assert(fake.service_unregister_calls == unregisters + 1);
    assert(fake.socket_close_calls == closes + 1);
    int socket_activity = fake.socket_create_calls + fake.socket_close_calls;
    late_service(late_cookie);
    assert(fake.socket_create_calls + fake.socket_close_calls == socket_activity);
    Ethernet.end();
}

/* Every assertion below fails against a fake whose fnet_socket_close() is void:
 * the type alone made the whole group inexpressible. */
void test_close_answers_and_frees_the_descriptor()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 5};
    assert(Ethernet.beginNoWait(mac));

    EthernetServer server(9100);
    const uint8_t slot = accept_one_connection(server);
    const fnet_socket_t descriptor = slot_descriptor(slot);
    assert(socket_find(descriptor) != nullptr);

    /* The one success. */
    assert(fnet_socket_close(descriptor) == FNET_OK);
    assert(socket_find(descriptor) == nullptr);

    /* And the one failure real FNET has here: a descriptor it no longer owns
     * (fnet_socket.c:767-771). EthernetClass::socketClose() reaches
     * fnet_socket_close() with socket_ptr[s] unchecked (Nativesocket.cpp:195),
     * so a double close is reachable -- and harmless, because a descriptor FNET
     * has already freed has nothing left to leak. */
    const int closes = fake.socket_close_calls;
    const int failures = fake.socket_close_failures;
    assert(fnet_socket_close(descriptor) == FNET_ERR);
    assert(fnet_error_get() == FNET_ERR_BAD_DESC);
    assert(fake.socket_close_calls == closes);
    assert(fake.socket_close_failures == failures + 1);

    /* A null descriptor is the same failure, and it is the one
     * EthernetServer::available() can produce: socketStatus() answers CLOSED
     * for socket_ptr[i] == nullptr as well as for SS_CLOSED, and the CLOSED arm
     * closes the socket regardless (NativeEthernetServer.cpp:217-219). Note
     * what that does to the global error word, which fnet_error.h:200-201 says
     * nothing clears. */
    set_error(FNET_ERR_OK);
    assert(fnet_socket_close(nullptr) == FNET_ERR);
    assert(fnet_error_get() == FNET_ERR_BAD_DESC);

    Ethernet.socket_ptr[slot] = nullptr;
    server.end();
    Ethernet.end();
}

/* fnet_socket_accept()'s addrlen contract, driven directly because the vendor
 * cannot drive it deterministically -- see reset_socket_model(). */
void test_accept_addrlen_contract()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 6};
    assert(Ethernet.beginNoWait(mac));

    EthernetServer server(9101);
    server.begin();
    const fnet_socket_t listener = descriptor_of(listening_socket());

    struct fnet_sockaddr from = {};
    fake.accept_addrlen_enforced = true;

    /* Nothing pending is FNET_ERR_AGAIN, not a bare null (fnet_socket.c:909-914).
     * The earlier fake left the error word untouched here, so the adapter's
     * would-block arm was reached with whatever the previous call had set. */
    set_error(FNET_ERR_OK);
    fnet_size_t length = sizeof(from);
    assert(fnet_socket_accept(listener, &from, &length) == nullptr);
    assert(fnet_error_get() == FNET_ERR_AGAIN);

    /* A length below sizeof(struct fnet_sockaddr_in) is refused before the
     * queue is even consulted, so a pending connection is dropped. This is what
     * EthernetServer::poll() risks on every accept. */
    fake.pending_connections = 1;
    length = sizeof(struct fnet_sockaddr_in) - 1u;
    assert(fnet_socket_accept(listener, &from, &length) == nullptr);
    assert(fnet_error_get() == FNET_ERR_INVAL);
    assert(fake.pending_connections == 1);

    /* Zero is not checked at all -- the real guard is `if(addr && addr_len)`
     * around the check, so a zeroed length is accepted where a small non-zero
     * one is not. */
    length = 0u;
    fnet_socket_t accepted = fnet_socket_accept(listener, &from, &length);
    assert(accepted != nullptr);
    assert(fake.pending_connections == 0);
    assert(fnet_socket_close(accepted) == FNET_OK);

    /* A socket that is not listening is FNET_ERR_INVAL (fnet_socket.c:890-894).
     * A bound UDP socket is one: EthernetUDP::begin() leaves it SS_CLOSED. */
    EthernetUDP udp;
    assert(udp.begin(9102) == 1);
    fake.pending_connections = 1;
    length = sizeof(from);
    assert(fnet_socket_accept(descriptor_of(datagram_socket()), &from,
                              &length) == nullptr);
    assert(fnet_error_get() == FNET_ERR_INVAL);

    fake.pending_connections = 0;
    fake.accept_addrlen_enforced = false;
    udp.stop();
    server.end();
    Ethernet.end();
}

/* MSG_PEEK, which the earlier fake ignored entirely: it returned the same
 * scalar forever, so every receive was a peek and no test could tell. The
 * vendor relies on the distinction inside one call --
 * EthernetUDP::parsePacket() sizes the datagram with
 * EthernetClass::socketRecvAvailable(), which peeks (Nativesocket.cpp:282), and
 * then reads it with a plain fnet_socket_recvfrom(), which consumes
 * (NativeEthernetUdp.cpp:133-138). A fake whose MSG_PEEK consumed would drain
 * the datagram in the first of those two calls and parsePacket() would answer 0
 * for a packet that had arrived. */
void test_peek_does_not_consume_but_a_plain_receive_does()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 7};
    assert(Ethernet.beginNoWait(mac));

    EthernetUDP udp;
    assert(udp.begin(9103) == 1);
    FakeFnetSocket *control = datagram_socket();
    deliver(control, 6u);

    const int peeks = fake.peek_calls;
    assert(udp.parsePacket() == 6);
    assert(fake.peek_calls == peeks + 1);
    /* The plain read in the same call did consume it. */
    assert(udp.parsePacket() == 0);
    udp.stop();

    /* The TCP side of the same property, through the receive the transport
     * actually performs: flags 0, so it consumes. */
    EthernetServer server(9104);
    const uint8_t slot = accept_one_connection(server);
    FakeFnetSocket *socket = socket_at_slot(slot);
    EthernetClient client(slot);
    uint8_t data[8] = {};

    deliver(socket, 6u);
    assert(client.available() == 6);
    assert(client.read(data, sizeof(data)) == 6);
    /* Drained, still connected, no FIN: FNET answers 0, and 0 is "nothing yet"
     * rather than end of stream. */
    assert(client.available() == 0);
    assert(socket->recv_count - socket->recv_read == 0u);

    client.stop();
    server.end();
    Ethernet.end();
}

/* The send path at the vendor seam. Every row here is unreachable against the
 * earlier fake, which hard-wired SO_SNDNUM to 0 and so made
 * socketSendAvailable() identically SO_SNDBUF. */
void test_send_capacity_comes_from_two_accounts()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 8};
    assert(Ethernet.beginNoWait(mac));

    EthernetServer server(9105);
    const uint8_t slot = accept_one_connection(server);
    FakeFnetSocket *socket = socket_at_slot(slot);
    EthernetClient client(slot);
    uint8_t bytes[12] = {};

    /* SO_SNDBUF is what EthernetClass::socketBegin() set it to, not zero: a
     * socket this vendor created never reports a zero send buffer, so the only
     * honest route to "no capacity" is a full one. */
    assert(socket->send_max == FNET_SOCKET_DEFAULT_SIZE);
    set_send_freespace(socket, 0u);
    assert(client.availableForWrite() == 0);
    int sends = fake.send_calls;
    client.clearWriteError();
    assert(client.write(bytes, sizeof(bytes)) == 0);
    assert(fake.send_calls == sends);
    /* No error was set, so write_once() reads this as a would-block. */
    assert(client.getWriteError() == 0);

    /* Partial room clamps the write to it, once. */
    set_send_freespace(socket, 5u);
    assert(client.availableForWrite() == 5);
    sends = fake.send_calls;
    client.clearWriteError();
    assert(client.write(bytes, sizeof(bytes)) == 5);
    assert(fake.send_calls == sends + 1);
    assert(client.getWriteError() == 0);
    /* The accepted bytes are now pending, which is what SO_SNDNUM reports. */
    assert(client.availableForWrite() == 0);

    /* Open defect 9, made visible. The socket buffer has room and
     * socketSendAvailable() reports it, but fnet_tcp.c:831-836 clamps the send
     * window on the netheap and _fnet_tcp_snd() returns 0 -- a would-block.
     * EthernetClass::socketSend() maps that to 0 (Nativesocket.cpp:327-334) and
     * EthernetClient::write() calls setWriteError() on it, so write_once()
     * reports a socket fault where the truth is "retry later". */
    set_send_freespace(socket, socket->send_max);
    fake.netheap_max = kNetbufFloor - 1u;
    assert(client.availableForWrite() ==
           static_cast<int>(static_cast<uint16_t>(socket->send_max)));
    sends = fake.send_calls;
    client.clearWriteError();
    assert(client.write(bytes, sizeof(bytes)) == 0);
    assert(fake.send_calls == sends + 1);
    assert(client.getWriteError() != 0);
    fake.netheap_max = kNetheapPlenty;

    /* And the other half of the same collapse: a peer that closed. FNET says
     * FNET_ERR/FNET_ERR_NOTCONN (fnet_tcp.c:817-821); the vendor says 0 and the
     * write error, indistinguishable from the row above. */
    socket->state = SS_CLOSING;
    sends = fake.send_calls;
    client.clearWriteError();
    assert(client.write(bytes, sizeof(bytes)) == 0);
    assert(fake.send_calls == sends + 1);
    assert(client.getWriteError() != 0);
    assert(fnet_error_get() == FNET_ERR_NOTCONN);

    socket->state = SS_CONNECTED;
    client.stop();
    server.end();
    Ethernet.end();
}

/* getopt is the one place the vendor asks FNET a question rather than telling it
 * something, and the earlier fake answered every question with FNET_OK -- even
 * for an option it did not know, leaving the caller's variable untouched. */
void test_getopt_validates_level_length_and_option()
{
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 9};
    assert(Ethernet.beginNoWait(mac));

    EthernetServer server(9106);
    const uint8_t slot = accept_one_connection(server);
    const fnet_socket_t socket = slot_descriptor(slot);

    fnet_uint32_t value = 0xffffffffu;
    fnet_size_t length = sizeof(value);
    assert(fnet_socket_getopt(socket, SOL_SOCKET, SO_SNDBUF, &value,
                              &length) == FNET_OK);
    assert(value == FNET_SOCKET_DEFAULT_SIZE);
    /* Real FNET reports the length it wrote, and it writes a 32-bit word for
     * this option whatever the caller offered (fnet_socket.c:1554-1571). */
    assert(length == sizeof(fnet_uint32_t));

    /* Too short is FNET_ERR_INVAL, not a silent short write. */
    length = sizeof(fnet_uint32_t) - 1u;
    assert(fnet_socket_getopt(socket, SOL_SOCKET, SO_SNDBUF, &value,
                              &length) == FNET_ERR);
    assert(fnet_error_get() == FNET_ERR_INVAL);

    /* An option the socket level does not define is FNET_ERR_NOPROTOOPT
     * (fnet_socket.c:1644-1646). Under the earlier fake this returned FNET_OK
     * and left `value` as it found it. */
    length = sizeof(value);
    assert(fnet_socket_getopt(socket, SOL_SOCKET, SO_LISTENQLIMIT, &value,
                              &length) == FNET_OK);
    assert(fnet_socket_getopt(socket, SOL_SOCKET, IP_TTL, &value, &length) ==
           FNET_ERR);
    assert(fnet_error_get() == FNET_ERR_NOPROTOOPT);

    /* `level` is not decoration: the same option number means different things
     * at different levels, and the earlier fake ignored the parameter while
     * numbering SO_STATE and IP_TTL identically. */
    fnet_socket_state_t state = SS_CLOSED;
    length = sizeof(state);
    assert(fnet_socket_getopt(socket, SOL_SOCKET, SO_STATE, &state, &length) ==
           FNET_OK);
    assert(state == SS_CONNECTED);
    assert(static_cast<int>(SO_STATE) != static_cast<int>(IP_TTL));

    /* SO_ERROR hands over the socket-scoped error and clears it, while the
     * global word keeps it (fnet_socket.c:1603-1613, fnet_error.h:200-201). */
    fnet_error_t socket_error = FNET_ERR_OK;
    length = sizeof(socket_error);
    assert(fnet_socket_getopt(socket, SOL_SOCKET, SO_ERROR, &socket_error,
                              &length) == FNET_OK);
    assert(socket_error == FNET_ERR_NOPROTOOPT);
    assert(fnet_socket_getopt(socket, SOL_SOCKET, SO_ERROR, &socket_error,
                              &length) == FNET_OK);
    assert(socket_error == FNET_ERR_OK);
    assert(fnet_error_get() == FNET_ERR_NOPROTOOPT);

    /* A descriptor FNET does not own answers FNET_ERR_BAD_DESC, which is what
     * EthernetClass::socketStatus() walks into for socket_ptr[s] == nullptr --
     * and it then switches on an uninitialised fnet_socket_state_t
     * (Nativesocket.cpp:158-181). */
    length = sizeof(value);
    assert(fnet_socket_getopt(nullptr, SOL_SOCKET, SO_SNDBUF, &value,
                              &length) == FNET_ERR);
    assert(fnet_error_get() == FNET_ERR_BAD_DESC);

    EthernetClient(slot).stop();
    server.end();
    Ethernet.end();
}

void test_adapter_socket_result_mapping()
{
    AiusNetworkTeensy adapter = {};
    AiusNetEvent event = {};
    const AiusNetAction start = {AIUS_NET_ACTION_START_STACK, 9u};
    const AiusNetAction dhcp = {AIUS_NET_ACTION_START_DHCP, 9u};
    const AiusNetAction publish = {AIUS_NET_ACTION_PUBLISH_ONLINE, 9u};
    const AiusNetAction none = {AIUS_NET_ACTION_NONE, 0u};
    uint8_t buffer[16] = {};

    aius_network_teensy_init(&adapter);
    /* beginNoWait() returns before the PHY has negotiated, so a started stack
     * is not a usable link and STACK_STARTED -- the supervisor's cue to send a
     * DISCOVER -- is withheld until link-up is actually observed. */
    assert(!fake.link_connected);
    fake.dhcp_state = 1;
    fake.statistics_available = true;
    fake.tx_packet = 7;
    fake.rx_packet = 9;
    aius_network_teensy_apply(&adapter, 0u, start, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);
    assert(adapter.stack_started && !adapter.link_up_reported);

    /* The diagnostics, measured through the real vendor code rather than a
     * stand-in for it. This generation is the cold one: nothing was
     * initialised, so beginNoWait()'s own guard let fnet_netif_init() run, and
     * the cached link member it sampled on the way out reads down. */
    const AiusNetworkTeensyDiag *diag = aius_network_teensy_diag(&adapter);
    assert(diag->generation == 9u);
    assert(diag->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_PERFORMED);
    assert(fake.netif_init_calls == 1);
    assert(diag->link_at_stack_start == AIUS_NETWORK_TEENSY_LINK_DOWN);
    assert(diag->link_up_source == AIUS_NETWORK_TEENSY_LINK_UP_NONE);

    aius_network_teensy_poll(&adapter, 1u, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);

    /* The FNET link service reports the transition through the real
     * NativeEthernet callback chain. Link-up is observed, but this generation
     * ran the real fnet_netif_init(), so STACK_STARTED is withheld: the
     * interface reports up several seconds before the FEC carries a frame, and
     * the counters are what say when it does. */
    fake.link_connected = true;
    fake.link_callback(nullptr, FNET_TRUE, fake.link_cookie);
    assert(Ethernet.linkStatus() == LinkON);
    aius_network_teensy_poll(&adapter, 2u, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);
    assert(adapter.link_up_reported && adapter.rx_ready_pending);

    /* Link-up was released by the link service's own callback, not by the
     * init-time snapshot -- which is exactly the distinction link_up_source
     * exists to draw. */
    assert(diag->link_up_source == AIUS_NETWORK_TEENSY_LINK_UP_CALLBACK);
    assert(diag->link_up_ms == 2u);
    assert(diag->link_callback_up_seen && diag->link_callback_up_ms == 2u);
    assert(diag->link_callbacks == 1u);

    /* The wait spends its epoch on fnet_netif_get_statistics() rather than the
     * link member: the first poll takes the baseline, the second sees it
     * unmoved, and the frame the MAC clocks in releases STACK_STARTED once. */
    const int statistics_before_wait = fake.statistics_calls;
    aius_network_teensy_poll(&adapter, 3u, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);
    assert(fake.statistics_calls == statistics_before_wait + 1);
    aius_network_teensy_poll(&adapter, 4u, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);
    assert(adapter.rx_ready_pending);

    fake.rx_packet = 10;
    aius_network_teensy_poll(&adapter, 5u, &event);
    assert(event.kind == AIUS_NET_EVENT_STACK_STARTED);
    assert(event.generation == 9u);
    assert(!adapter.rx_ready_pending);
    assert(diag->rx_ready_source == AIUS_NETWORK_TEENSY_RX_READY_COUNTER);
    assert(diag->rx_ready_ms == 5u);
    aius_network_teensy_poll(&adapter, 6u, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);

    const int statistics_before_dhcp = fake.statistics_calls;
    aius_network_teensy_apply(&adapter, 7u, dhcp, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);
    /* FNET's own client state and the MAC's counters, read through the real
     * fnet_dhcp_cln_get_by_netif()/get_state() and fnet_netif_get_statistics()
     * calls the adapter makes. */
    assert(diag->dhcp_started_seen && diag->dhcp_start_ms == 7u);
    assert(diag->dhcp_state_at_start == 1u);
    assert(diag->frames_valid);
    assert(fake.dhcp_state_calls == 1);
    assert(fake.statistics_calls == statistics_before_dhcp + 1);
    assert(aius_network_teensy_dhcp_state(&adapter) == 1u);
    assert(aius_network_teensy_link_live(&adapter) ==
           AIUS_NETWORK_TEENSY_LINK_UP);

    fake.tx_packet = 11;
    fake.rx_packet = 15;
    fake.dhcp_state = 5;
    aius_network_teensy_apply(&adapter, 8u, publish, &event);
    assert(adapter.listener_open && !adapter.udp_open);
    aius_network_teensy_apply(&adapter, 9u, none, &event);
    assert(adapter.online && adapter.udp_open);
    /* Going online ends the record: four frames left the MAC across this
     * generation's DHCP window and FNET reports the lease bound. */
    assert(diag->end_reason == AIUS_NETWORK_TEENSY_END_ONLINE);
    assert(diag->end_ms == 9u);
    assert(diag->dhcp_state_at_end == 5u);
    assert(diag->tx_frames == 4u && diag->rx_frames == 5u);
    assert(diag->link_at_end == AIUS_NETWORK_TEENSY_LINK_UP);
    assert(diag->link_live_at_end == AIUS_NETWORK_TEENSY_LINK_UP);
    /* millis() is 0 in this harness, so the blocking measurement has nothing
     * to see here; test_network_teensy.cpp covers it against a moving clock. */
    assert(diag->stack_start_blocking_ms == 0u);

    AiusIoEvent io = aius_network_teensy_accept(&adapter);
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);
    /* That would-block did not come from FNET. EthernetServer::accept() only
     * walks EthernetClass::socketStatus() looking for a slot already in
     * SS_CONNECTED (NativeEthernetServer.cpp:238-254); the one caller of
     * fnet_socket_accept() is EthernetServer::poll(), which runs in the FNET
     * service callback and reports nothing back. So FNET's own
     * FNET_ERR_AGAIN-on-nothing-pending never reaches the adapter, and the
     * error word here is still whatever the last socket call left. */
    assert(fnet_error_get() == FNET_ERR_OK);

    fake.pending_connections = 1;
    fake.service_callback(fake.service_cookie);
    io = aius_network_teensy_accept(&adapter);
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 0u &&
           io.fault == 0u);
    /* The length EthernetServer::poll() supplied. It never assigned it
     * (NativeEthernetServer.cpp:339), so real FNET decides this connection's
     * fate on stack contents; test_accept_addrlen_contract() drives the rule. */
    assert(fake.accept_addrlen_seen_valid);

    FakeFnetSocket *client = connected_socket();

    /* --- the receive classification, from socket state ------------------ */

    deliver(client, 5u);
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 5u &&
           io.fault == 0u);

    /* Those five bytes are gone. read_once() passes no MSG_PEEK, so a second
     * read of a connected socket with a drained buffer is FNET's "nothing yet"
     * -- a zero return, which must not tear the connection down. A POST whose
     * body lands in a second segment reads exactly this between the segments,
     * and answering it with a zero-byte close is the defect this row exists
     * for (fnet_tcp.c:776). */
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    /* Buffered data outranks a received FIN: FNET drains first and only reports
     * closure once the buffer is empty (fnet_tcp.c:768-772). */
    deliver(client, 4u);
    client->fin_received = true;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 4u &&
           io.fault == 0u);

    /* Drained, FIN received, still SS_CONNECTED: FNET_ERR carrying
     * FNET_ERR_CONNCLOSED. */
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_CLOSED && io.bytes == 0u && io.fault == 0u);
    assert(fnet_error_get() == FNET_ERR_CONNCLOSED);

    /* And the other closure code: a socket that has left SS_CONNECTED answers
     * FNET_ERR_NOTCONN instead (fnet_tcp.c:754-757). */
    client->fin_received = false;
    client->state = SS_CLOSING;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_CLOSED && io.bytes == 0u && io.fault == 0u);
    assert(fnet_error_get() == FNET_ERR_NOTCONN);
    client->state = SS_CONNECTED;

    /* The remaining arms of the adapter's switch. Neither code is reachable on
     * FNET's TCP receive path, which is why they are injected rather than
     * staged: FNET_ERR_INVAL is what the recvfrom wrapper raises for a null
     * buffer (fnet_socket.c:1098) and FNET_ERR_NOPROTOOPT is a code the adapter
     * names nowhere at all. Both must be a socket fault, not a guess at
     * closure. */
    fake.force_recv_error = FNET_ERR_INVAL;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);

    fake.force_recv_error = FNET_ERR_NOPROTOOPT;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);

    /* FNET_ERR_AGAIN is likewise unreachable from _fnet_tcp_rcv(), but the
     * adapter keeps an arm for it because the contract names it. */
    fake.force_recv_error = FNET_ERR_AGAIN;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);
    fake.force_recv_error = FNET_ERR_OK;

    /* --- the send classification --------------------------------------- */

    set_send_freespace(client, 4u);
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 4u &&
           io.fault == 0u);

    int sends = fake.send_calls;
    set_send_freespace(client, 0u);
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u && fake.send_calls == sends);

    /* Open defect 9 at the adapter seam. Capacity is reported, the send takes
     * nothing because the netheap is short, and the collapse in
     * EthernetClass::socketSend() plus EthernetClient::write() turns a
     * would-block into a socket fault. Deliberately pinned as it behaves today:
     * this row flips the moment write_once() stops going through
     * g_client.write(). */
    set_send_freespace(client, client->send_max);
    fake.netheap_max = kNetbufFloor - 1u;
    sends = fake.send_calls;
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);
    assert(fake.send_calls == sends + 1);
    fake.netheap_max = kNetheapPlenty;

    /* And the same fault for the opposite cause: the peer closed mid-response.
     * FNET says FNET_ERR/FNET_ERR_NOTCONN, the adapter reports a socket fault
     * where AIUS_IO_CLOSED is the truth. Also open defect 9. */
    client->state = SS_CLOSING;
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);
    assert(fnet_error_get() == FNET_ERR_NOTCONN);
    client->state = SS_CONNECTED;
    set_send_freespace(client, client->send_max);

    fake.force_send_error = FNET_ERR_NOPROTOOPT;
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);
    fake.force_send_error = FNET_ERR_OK;

    /* --- the UDP control socket ---------------------------------------- */

    FakeFnetSocket *control = datagram_socket();

    /* No peer has been learned yet, so the adapter withholds the send. */
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    /* An empty datagram buffer is 0, and for UDP that really is "nothing yet"
     * (fnet_socket.c:1845-1848). This is the row that sat forty lines below the
     * TCP one asserting the opposite mapping for the same value; both are
     * right, and it was the TCP one that was wrong. */
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    deliver(control, 5u);
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 5u &&
           io.fault == 0u);
    /* One datagram, consumed. */
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    /* A datagram larger than the caller's buffer is FNET_ERR/FNET_ERR_MSGSIZE,
     * and _fnet_socket_buffer_read_address() has already dequeued it
     * (fnet_socket.c:1827-1843) -- so the datagram is gone whatever the adapter
     * concludes. It is oversized *input*, not a broken socket: any host on the
     * segment can send one to port 4242 with no credential, so classifying it
     * as a socket fault handed an unauthenticated peer one fault per packet.
     * The receive drops it and reports "nothing this epoch" instead, and counts
     * it, because a drop nothing counts is the invisibility this suite exists to
     * remove. */
    assert(aius_network_teensy_udp_oversize_drops() == 0u);
    deliver(control, sizeof(buffer) + 1u);
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);
    assert(fnet_error_get() == FNET_ERR_MSGSIZE);
    assert(control->recv_count == 0u);
    assert(aius_network_teensy_udp_oversize_drops() == 1u);

    /* One drop per packet, and the socket survives every one of them: a second
     * oversized datagram counts again and still reports no fault. That is the
     * whole difference between rejecting an input and faulting a transport --
     * a flood of these costs the generation nothing but the counter. */
    deliver(control, sizeof(buffer) + 1u);
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);
    assert(aius_network_teensy_udp_oversize_drops() == 2u);

    /* Learn a peer, then send. UDP send has no zero return: it answers the
     * whole length or FNET_ERR (fnet_udp.c:602-611). The receive that follows
     * two dropped datagrams is an ordinary one, and it does not count. */
    deliver(control, 3u);
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 3u);
    assert(aius_network_teensy_udp_oversize_drops() == 2u);
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 4u &&
           io.fault == 0u);

    /* A netbuf shortage on the send is FNET_ERR/FNET_ERR_NOMEM
     * (fnet_udp.c:543-547): the netheap could not spare one datagram's worth of
     * buffer at this instant. That is a transient shortage and not a dead
     * socket, so it is a retry: a socket fault would be the transport declaring
     * the socket unusable over a condition the next epoch may well have
     * cleared. */
    fake.netheap_max = 0u;
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);
    assert(fnet_error_get() == FNET_ERR_NOMEM);
    fake.netheap_max = kNetheapPlenty;

    /* And the retry is a real one: the same socket sends normally the moment the
     * netheap recovers, which is what makes WOULD_BLOCK the truthful answer
     * rather than a softer way of saying "lost". */
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 4u &&
           io.fault == 0u);

    fake.force_send_error = FNET_ERR_AGAIN;
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);
    fake.force_send_error = FNET_ERR_OK;

    fake.force_recv_error = FNET_ERR_AGAIN;
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);
    fake.force_recv_error = FNET_ERR_OK;

    /* Narrowing the fault classification must not empty it. Neither code below
     * is reachable on FNET's UDP paths -- FNET_ERR_SHUTDOWN needs a
     * fnet_socket_shutdown() this adapter never calls, and FNET_ERR_NOPROTOOPT
     * only ever arrives latched from an ICMP report on a *connected* socket,
     * which the control socket is not -- so both are injected. Both must still
     * be a socket fault, and neither may be counted as a dropped datagram. */
    const uint32_t drops_before_faults =
        aius_network_teensy_udp_oversize_drops();
    fake.force_send_error = FNET_ERR_SHUTDOWN;
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);
    fake.force_send_error = FNET_ERR_OK;

    fake.force_recv_error = FNET_ERR_NOPROTOOPT;
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);
    fake.force_recv_error = FNET_ERR_OK;
    assert(aius_network_teensy_udp_oversize_drops() == drops_before_faults);

    /* --- close --------------------------------------------------------- */

    const int closes = fake.socket_close_calls;
    io = aius_network_teensy_close_once(&adapter);
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 0u &&
           io.fault == 0u);
    assert(fake.socket_close_calls == closes + 1);
    /* FNET answered FNET_OK and freed the descriptor, so nothing leaked. */
    assert(fake.socket_close_failures == 0);
    io = aius_network_teensy_close_once(&adapter);
    assert(io.result == AIUS_IO_CLOSED && io.bytes == 0u &&
           io.fault == 0u);
    assert(fake.socket_close_calls == closes + 1);
    assert(fake.socket_close_failures == 0);
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_CLOSED && io.bytes == 0u &&
           io.fault == 0u);
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_CLOSED && io.bytes == 0u &&
           io.fault == 0u);

    /* The status accessors against the real vendor path: FNET's address word
     * reaches the caller as four octets in wire order, and the vendor link
     * enum arrives without its type.
     */
    uint8_t address[4] = {0xffu, 0xffu, 0xffu, 0xffu};
    assert(!aius_network_teensy_address(&adapter, address));
    assert(address[0] == 0u && address[1] == 0u && address[2] == 0u &&
           address[3] == 0u);
    assert(aius_network_teensy_link(&adapter) ==
           AIUS_NETWORK_TEENSY_LINK_UP);

    fake.ip4_address = 0x1504a8c0u;
    assert(aius_network_teensy_address(&adapter, address));
    assert(address[0] == 192u && address[1] == 168u && address[2] == 4u &&
           address[3] == 21u);

    fake.link_connected = false;
    fake.link_callback(nullptr, FNET_FALSE, fake.link_cookie);
    assert(Ethernet.linkStatus() == LinkOFF);
    assert(aius_network_teensy_link(&adapter) ==
           AIUS_NETWORK_TEENSY_LINK_DOWN);
    fake.link_connected = true;
    fake.link_callback(nullptr, FNET_TRUE, fake.link_cookie);
    assert(Ethernet.linkStatus() == LinkON);
    assert(aius_network_teensy_link(&adapter) ==
           AIUS_NETWORK_TEENSY_LINK_UP);

    /* The asymmetry, through the vendor code that causes it. Teardown runs
     * Ethernet.end(), which releases the link service and the DHCP client but
     * does not de-initialise the netif -- so the next generation's beginNoWait()
     * finds fnet_netif_is_initialized() true and skips the initialisation the
     * first one performed. Two generations within one boot therefore do not run
     * the same code, and this is where that stops being an inference.
     */
    const AiusNetAction teardown = {AIUS_NET_ACTION_TEARDOWN, 9u};
    const AiusNetAction restart = {AIUS_NET_ACTION_START_STACK, 10u};
    const int netif_inits = fake.netif_init_calls;
    aius_network_teensy_apply(&adapter, 100u, teardown, &event);
    for (int step = 0; step < 4 && event.kind != AIUS_NET_EVENT_TEARDOWN_DONE;
         step++) {
        aius_network_teensy_apply(&adapter, 101u, none, &event);
    }
    assert(event.kind == AIUS_NET_EVENT_TEARDOWN_DONE);
    assert(!adapter.stack_started);
    /* Not one close in the whole teardown was refused. */
    assert(fake.socket_close_failures == 0);

    const int statistics_before_restart = fake.statistics_calls;
    aius_network_teensy_apply(&adapter, 200u, restart, &event);
    assert(adapter.stack_started);
    assert(fake.netif_init_calls == netif_inits);
    diag = aius_network_teensy_diag(&adapter);
    assert(diag->generation == 10u);
    assert(diag->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_SKIPPED);

    /* And it is not delayed. The generation that skips the initialisation is
     * the one that binds in 19 ms on the board, so it waits for no evidence
     * and spends no counter read: STACK_STARTED lands in this same call. */
    assert(event.kind == AIUS_NET_EVENT_STACK_STARTED);
    assert(event.generation == 10u);
    assert(!adapter.rx_ready_required && !adapter.rx_ready_pending);
    assert(diag->rx_ready_source ==
           AIUS_NETWORK_TEENSY_RX_READY_NOT_REQUIRED);
    assert(fake.statistics_calls == statistics_before_restart);

    /* And the generation that failed is the one retained, not the last. */
    const AiusNetworkTeensyDiag *first = aius_network_teensy_diag_first(&adapter);
    assert(first != nullptr);
    assert(first->generation == 9u);
    assert(first->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_PERFORMED);
}

} // namespace

unsigned long millis() { return 0; }
unsigned long micros() { return 0; }
void delay(unsigned long) {}
void yield() {}

fnet_return_t fnet_init(struct fnet_init_params *)
{
    fake.fnet_init_calls++;
    record("fnet-init");
    if (fake.fnet_active)
        fake.illegal_fnet_init_calls++;
    if (fake.fnet_init_failures > 0) {
        fake.fnet_init_failures--;
        return FNET_ERR;
    }
    fake.fnet_active = true;
    return FNET_OK;
}

fnet_bool_t fnet_netif_is_initialized(fnet_netif_desc_t)
{
    return fake.netif_initialized ? FNET_TRUE : FNET_FALSE;
}

fnet_return_t fnet_netif_init(fnet_netif_desc_t, fnet_uint8_t *, fnet_size_t)
{
    fake.netif_init_calls++;
    record("netif-init");
    if (fake.netif_failures > 0) {
        fake.netif_failures--;
        return FNET_ERR;
    }
    fake.netif_initialized = true;
    return FNET_OK;
}

fnet_netif_desc_t fnet_netif_get_default(void)
{
    return fake.netif_initialized ? reinterpret_cast<void *>(0x10) : nullptr;
}

fnet_bool_t fnet_netif_is_connected(fnet_netif_desc_t netif)
{
    /* fnet_netif.c:1013-1015 answers FNET_TRUE for a null interface -- "is
     * connected by default" -- which is why the adapter's link_live() tests the
     * descriptor itself instead of trusting this. Modelled, so that defence is
     * covered rather than assumed. */
    if (netif == nullptr)
        return FNET_TRUE;
    return fake.link_connected ? FNET_TRUE : FNET_FALSE;
}

fnet_link_desc_t fnet_link_init(fnet_link_params_t *params)
{
    fake.link_init_calls++;
    fake.link_callback = params->callback;
    fake.link_cookie = params->callback_param;
    record("link-init");
    if (fake.link_init_failures > 0) {
        fake.link_init_failures--;
        return 0;
    }
    return 11;
}

void fnet_link_release(fnet_link_desc_t)
{
    fake.link_release_calls++;
    record("link-release");
    inject_cleanup_callback(
        fake.link_callback, reinterpret_cast<void *>(0x10), FNET_TRUE,
        fake.link_cookie);
    assert(Ethernet.linkStatus() == LinkOFF);
}

fnet_dhcp_cln_desc_t fnet_dhcp_cln_init(fnet_dhcp_cln_params_t *)
{
    fake.dhcp_init_calls++;
    fake.dhcp_enabled = true;
    record("dhcp-init");
    return reinterpret_cast<void *>(0x22);
}

void fnet_dhcp_cln_release(fnet_dhcp_cln_desc_t)
{
    fake.dhcp_release_calls++;
    fake.dhcp_enabled = false;
    record(fake.dhcp_release_calls == 1
               ? "dhcp-release"
               : "dhcp-release-final");
    inject_cleanup_callback(
        fake.dhcp_callback, reinterpret_cast<void *>(0x22),
        reinterpret_cast<void *>(0x10), fake.dhcp_cookie);
}

fnet_bool_t fnet_dhcp_cln_is_enabled(fnet_dhcp_cln_desc_t)
{
    return fake.dhcp_enabled ? FNET_TRUE : FNET_FALSE;
}

void fnet_dhcp_cln_set_response_timeout(fnet_dhcp_cln_desc_t, fnet_time_t) {}

void fnet_dhcp_cln_set_callback_updated(
    fnet_dhcp_cln_desc_t, fnet_dhcp_cln_callback_t callback, void *cookie)
{
    fake.dhcp_callback = callback;
    fake.dhcp_cookie = cookie;
}

fnet_dhcp_cln_desc_t fnet_dhcp_cln_get_by_netif(fnet_netif_desc_t)
{
    return fake.dhcp_enabled ? reinterpret_cast<void *>(0x22) : nullptr;
}

void fnet_dhcp_cln_get_options(
    fnet_dhcp_cln_desc_t, struct fnet_dhcp_cln_options *options)
{
    std::memset(options, 0, sizeof(*options));
}

/* The two diagnostic readers, faked at the same seam as the rest of FNET.
 * fnet_dhcp_cln_get_state() asserts its descriptor rather than tolerating a
 * null: the real one dereferences without checking, so a caller that reaches it
 * with nothing is a defect this fake must expose, not absorb. */
fnet_uint8_t fnet_dhcp_cln_get_state(fnet_dhcp_cln_desc_t desc)
{
    assert(desc != nullptr);
    fake.dhcp_state_calls++;
    return fake.dhcp_state;
}

fnet_return_t fnet_netif_get_statistics(
    fnet_netif_desc_t netif, struct fnet_netif_statistics *statistics)
{
    fake.statistics_calls++;
    /* fnet_netif.c:1063 tests the descriptor, the pointer and the driver hook
     * before touching anything. */
    if (netif == nullptr || statistics == nullptr || !fake.statistics_available)
        return FNET_ERR;
    statistics->tx_packet = fake.tx_packet;
    statistics->rx_packet = fake.rx_packet;
    return FNET_OK;
}

void fnet_poll(void) {}

fnet_service_desc_t fnet_service_register(
    fnet_service_poll_t callback, void *cookie)
{
    fake.service_register_calls++;
    fake.service_callback = callback;
    fake.service_cookie = cookie;
    record("service-register");
    return reinterpret_cast<fnet_service_desc_t>(0x31);
}

void fnet_service_unregister(fnet_service_desc_t)
{
    fake.service_unregister_calls++;
    record("service-unregister");
    inject_cleanup_callback(fake.service_callback, fake.service_cookie);
}

fnet_socket_t fnet_socket(
    fnet_address_family_t family, fnet_socket_type_t type, fnet_uint32_t)
{
    observe_socket_access();
    fake.socket_create_calls++;
    record("socket-create");
    /* fnet_socket.c:440-443: an unsupported family or type finds no protocol. */
    if (family != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM)) {
        set_error(FNET_ERR_PROTONOSUPPORT);
        return nullptr;
    }
    for (size_t index = 0; index < FNET_CFG_SOCKET_MAX; index++) {
        FakeFnetSocket *socket = &fake.sockets[index];
        if (socket->allocated)
            continue;
        *socket = FakeFnetSocket{};
        socket->allocated = true;
        socket->type = type;
        socket->state = SS_CLOSED;
        socket->send_max =
            type == SOCK_STREAM ? kTcpBufferDefault : kUdpBufferDefault;
        socket->recv_max = socket->send_max;
        return socket_descriptor(index);
    }
    /* fnet_socket.c:434-437. */
    set_error(FNET_ERR_NO_DESC);
    return nullptr;
}

fnet_return_t fnet_socket_bind(
    fnet_socket_t descriptor, const struct fnet_sockaddr *name,
    fnet_size_t namelen)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if (name == nullptr) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    const fnet_error_t error = addr_check_len(name->sa_family, namelen);
    if (error != FNET_ERR_OK) {
        set_socket_error(socket, error);
        return FNET_ERR;
    }
    socket->local_port = name->sa_port;
    return FNET_OK;
}

fnet_return_t fnet_socket_setopt(
    fnet_socket_t descriptor, fnet_protocol_t level,
    fnet_socket_options_t option, const void *value, fnet_size_t length)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if (value == nullptr || length == 0u) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    if (level != SOL_SOCKET) {
        /* fnet_socket.c:1370-1380 hands the call to the protocol's own
         * setsockopt. Only the options this transport sets are modelled. */
        switch (option) {
        case TCP_NODELAY:
        case IP_TTL:
        case IP_MULTICAST_TTL:
            if (length < sizeof(fnet_uint32_t)) {
                set_socket_error(socket, FNET_ERR_INVAL);
                return FNET_ERR;
            }
            return FNET_OK;
        case IP_ADD_MEMBERSHIP:
            if (length < sizeof(struct fnet_ip_mreq)) {
                set_socket_error(socket, FNET_ERR_INVAL);
                return FNET_ERR;
            }
            return FNET_OK;
        default:
            set_socket_error(socket, FNET_ERR_NOPROTOOPT);
            return FNET_ERR;
        }
    }
    switch (option) {
    case SO_LINGER:
        if (length < sizeof(struct fnet_linger)) {
            set_socket_error(socket, FNET_ERR_INVAL);
            return FNET_ERR;
        }
        return FNET_OK;
    case SO_SNDBUF:
    case SO_RCVBUF:
        if (length < sizeof(fnet_uint32_t)) {
            set_socket_error(socket, FNET_ERR_INVAL);
            return FNET_ERR;
        }
        if (option == SO_SNDBUF)
            socket->send_max = *static_cast<const fnet_uint32_t *>(value);
        else
            socket->recv_max = *static_cast<const fnet_uint32_t *>(value);
        return FNET_OK;
    default:
        set_socket_error(socket, FNET_ERR_NOPROTOOPT);
        return FNET_ERR;
    }
}

fnet_return_t fnet_socket_listen(fnet_socket_t descriptor, fnet_size_t)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    /* fnet_socket.c:833-843. */
    if (socket->state == SS_CONNECTING || socket->state == SS_CONNECTED) {
        set_socket_error(socket, FNET_ERR_ISCONN);
        return FNET_ERR;
    }
    if (socket->local_port == 0u) {
        set_socket_error(socket, FNET_ERR_BOUNDREQ);
        return FNET_ERR;
    }
    socket->state = SS_LISTENING;
    return FNET_OK;
}

fnet_return_t fnet_socket_connect(
    fnet_socket_t descriptor, struct fnet_sockaddr *name, fnet_size_t namelen)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if (name == nullptr) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    const fnet_error_t error = addr_check_len(name->sa_family, namelen);
    if (error != FNET_ERR_OK) {
        set_socket_error(socket, error);
        return FNET_ERR;
    }
    if (address_is_unspecified(name)) {
        set_socket_error(socket, FNET_ERR_DESTADDRREQ);
        return FNET_ERR;
    }
    socket->state = SS_CONNECTING;
    socket->peer_known = true;
    return FNET_OK;
}

fnet_return_t fnet_socket_shutdown(fnet_socket_t descriptor, fnet_sd_flags_t how)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if ((how & SD_READ) != 0)
        socket->recv_shutdown = true;
    if ((how & SD_WRITE) != 0)
        socket->send_shutdown = true;
    return FNET_OK;
}

/* fnet_socket_sendto() (fnet_socket.c:961-1037); fnet_socket_send() is the same
 * function with a null destination (:1042-1045). */
fnet_ssize_t socket_send_impl(
    fnet_socket_t descriptor, const void *buffer, fnet_size_t length,
    fnet_flag_t flags, const struct fnet_sockaddr *to, fnet_size_t tolen)
{
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if (to == nullptr || tolen == 0u) {
        if (!socket->peer_known) {
            set_socket_error(socket, FNET_ERR_NOTCONN);
            return FNET_ERR;
        }
    } else {
        const fnet_error_t error = addr_check_len(to->sa_family, tolen);
        if (error != FNET_ERR_OK) {
            set_socket_error(socket, error);
            return FNET_ERR;
        }
        if (address_is_unspecified(to)) {
            set_socket_error(socket, FNET_ERR_DESTADDRREQ);
            return FNET_ERR;
        }
    }
    if (buffer == nullptr) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    if (socket->send_shutdown) {
        set_socket_error(socket, FNET_ERR_SHUTDOWN);
        return FNET_ERR;
    }
    if (fake.force_send_error != FNET_ERR_OK) {
        set_socket_error(socket, fake.force_send_error);
        return FNET_ERR;
    }
    (void)flags;
    return socket->type == SOCK_STREAM ? tcp_send(socket, length)
                                       : udp_send(socket, length);
}

fnet_ssize_t fnet_socket_send(
    fnet_socket_t descriptor, const void *buffer, fnet_size_t length,
    fnet_flag_t flags)
{
    observe_socket_access();
    fake.send_calls++;
    record("socket-send");
    return socket_send_impl(descriptor, buffer, length, flags, nullptr, 0u);
}

fnet_ssize_t fnet_socket_sendto(
    fnet_socket_t descriptor, const void *buffer, fnet_size_t length,
    fnet_flag_t flags, const struct fnet_sockaddr *to, fnet_size_t tolen)
{
    observe_socket_access();
    fake.sendto_calls++;
    return socket_send_impl(descriptor, buffer, length, flags, to, tolen);
}

/* fnet_socket_recvfrom() (fnet_socket.c:1051-1119); fnet_socket_recv() is the
 * same function with no address out-parameter (:1124-1127). */
fnet_ssize_t fnet_socket_recvfrom(
    fnet_socket_t descriptor, void *buffer, fnet_size_t length,
    fnet_flag_t flags, struct fnet_sockaddr *from, fnet_size_t *fromlen)
{
    observe_socket_access();
    fake.recv_calls++;
    if ((flags & MSG_PEEK) != 0u)
        fake.peek_calls++;
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if (buffer == nullptr) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    if (socket->local_port == 0u && socket->type != SOCK_RAW) {
        set_socket_error(socket, FNET_ERR_BOUNDREQ);
        return FNET_ERR;
    }
    if (from != nullptr && fromlen != nullptr) {
        const fnet_error_t error = addr_check_len(AF_INET, *fromlen);
        if (error != FNET_ERR_OK) {
            set_socket_error(socket, error);
            return FNET_ERR;
        }
    }
    if (socket->recv_shutdown) {
        set_socket_error(socket, FNET_ERR_SHUTDOWN);
        return FNET_ERR;
    }
    if (fake.force_recv_error != FNET_ERR_OK) {
        set_socket_error(socket, fake.force_recv_error);
        return FNET_ERR;
    }
    struct fnet_sockaddr *out = (from != nullptr && fromlen != nullptr)
                                    ? from
                                    : nullptr;
    return socket->type == SOCK_STREAM
               ? tcp_receive(socket, buffer, length, flags, out)
               : udp_receive(socket, buffer, length, flags, out);
}

fnet_ssize_t fnet_socket_recv(
    fnet_socket_t descriptor, void *buffer, fnet_size_t length,
    fnet_flag_t flags)
{
    return fnet_socket_recvfrom(
        descriptor, buffer, length, flags, nullptr, nullptr);
}

fnet_socket_t fnet_socket_accept(
    fnet_socket_t descriptor, struct fnet_sockaddr *addr, fnet_size_t *addrlen)
{
    observe_socket_access();
    FakeFnetSocket *listener = socket_find(descriptor);
    if (listener == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return nullptr;
    }
    /* fnet_socket.c:890-894. */
    if (listener->state != SS_LISTENING) {
        set_socket_error(listener, FNET_ERR_INVAL);
        return nullptr;
    }
    if (addr != nullptr && addrlen != nullptr) {
        fake.accept_addrlen_seen = *addrlen;
        fake.accept_addrlen_seen_valid = true;
        if (fake.accept_addrlen_enforced) {
            /* fnet_socket.c:896-902 checks the *listener's* family, not the
             * caller's buffer, so an uninitialised addr is irrelevant and an
             * uninitialised addrlen is decisive. */
            const fnet_error_t error = addr_check_len(AF_INET, *addrlen);
            if (error != FNET_ERR_OK) {
                set_socket_error(listener, error);
                return nullptr;
            }
        }
    }
    if (fake.pending_connections <= 0) {
        /* fnet_socket.c:909-914: prot_accept() finding nothing is
         * FNET_ERR_AGAIN, and the descriptor it speculatively allocated is
         * given back. */
        set_socket_error(listener, FNET_ERR_AGAIN);
        return nullptr;
    }
    for (size_t index = 0; index < FNET_CFG_SOCKET_MAX; index++) {
        FakeFnetSocket *accepted = &fake.sockets[index];
        if (accepted->allocated)
            continue;
        *accepted = FakeFnetSocket{};
        accepted->allocated = true;
        accepted->type = SOCK_STREAM;
        accepted->state = SS_CONNECTED;
        accepted->peer_known = true;
        /* An accepted socket inherits the listener's local address and its
         * buffer sizes (_fnet_socket_copy(), fnet_socket.c:384). */
        accepted->local_port = listener->local_port;
        accepted->send_max = listener->send_max;
        accepted->recv_max = listener->recv_max;
        fake.pending_connections--;
        if (addr != nullptr && addrlen != nullptr)
            *addr = test_peer_address();
        return socket_descriptor(index);
    }
    /* fnet_socket.c:927-931. */
    set_socket_error(listener, FNET_ERR_NO_DESC);
    return nullptr;
}

fnet_size_t fnet_socket_poll(fnet_socket_poll_t *, fnet_size_t)
{
    /* Real FNET answers the number of sockets with a pending event and fills
     * events_occurred. Zero -- no socket ready -- is a legitimate answer and
     * the only one this suite needs: EthernetServer::available()'s poll block
     * is diagnostic Serial output either way (NativeEthernetServer.cpp:151-179)
     * and EthernetServer::accept(), the arm this transport uses, never polls. */
    return 0u;
}

fnet_return_t fnet_socket_getopt(
    fnet_socket_t descriptor, fnet_protocol_t level,
    fnet_socket_options_t option, void *value, fnet_size_t *length)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if (value == nullptr || length == nullptr) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    if (level != SOL_SOCKET) {
        /* fnet_socket.c:1487-1497. _fnet_ip_getsockopt() and
         * _fnet_tcp_getsockopt() answer the protocol-level options; none of
         * them is read by this transport, so the modelled answer is the real
         * one for an option the level does not serve. */
        set_socket_error(socket, FNET_ERR_NOPROTOOPT);
        return FNET_ERR;
    }
    switch (option) {
    case SO_SNDBUF:
    case SO_RCVBUF:
    case SO_SNDNUM:
    case SO_RCVNUM:
    case SO_ACCEPTCONN:
    case SO_LISTENQLIMIT:
    case SO_LISTENQLEN: {
        if (*length < sizeof(fnet_uint32_t)) {
            set_socket_error(socket, FNET_ERR_INVAL);
            return FNET_ERR;
        }
        *length = sizeof(fnet_uint32_t);
        fnet_uint32_t answer = 0u;
        switch (option) {
        case SO_SNDBUF:
            answer = socket->send_max;
            break;
        case SO_RCVBUF:
            answer = socket->recv_max;
            break;
        case SO_SNDNUM:
            answer = socket->send_count;
            break;
        case SO_RCVNUM:
            answer = socket->recv_count - socket->recv_read;
            break;
        case SO_ACCEPTCONN:
            answer = socket->state == SS_LISTENING ? 1u : 0u;
            break;
        case SO_LISTENQLEN:
            answer = static_cast<fnet_uint32_t>(
                fake.pending_connections > 0 ? fake.pending_connections : 0);
            break;
        default:
            answer = 0u;
            break;
        }
        *static_cast<fnet_uint32_t *>(value) = answer;
        return FNET_OK;
    }
    case SO_STATE:
        if (*length < sizeof(fnet_socket_state_t)) {
            set_socket_error(socket, FNET_ERR_INVAL);
            return FNET_ERR;
        }
        *length = sizeof(fnet_socket_state_t);
        *static_cast<fnet_socket_state_t *>(value) = socket->state;
        return FNET_OK;
    case SO_TYPE:
        if (*length < sizeof(fnet_socket_type_t)) {
            set_socket_error(socket, FNET_ERR_INVAL);
            return FNET_ERR;
        }
        *length = sizeof(fnet_socket_type_t);
        *static_cast<fnet_socket_type_t *>(value) = socket->type;
        return FNET_OK;
    case SO_ERROR:
        if (*length < sizeof(fnet_error_t)) {
            set_socket_error(socket, FNET_ERR_INVAL);
            return FNET_ERR;
        }
        *length = sizeof(fnet_error_t);
        *static_cast<fnet_error_t *>(value) = socket->error;
        /* fnet_socket.c:1612 -- reading it clears the socket's copy, and only
         * the socket's copy. */
        socket->error = FNET_ERR_OK;
        return FNET_OK;
    default:
        set_socket_error(socket, FNET_ERR_NOPROTOOPT);
        return FNET_ERR;
    }
}

fnet_return_t fnet_socket_getpeername(
    fnet_socket_t descriptor, struct fnet_sockaddr *name, fnet_size_t *namelen)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        set_error(FNET_ERR_BAD_DESC);
        return FNET_ERR;
    }
    if (name == nullptr || namelen == nullptr) {
        set_socket_error(socket, FNET_ERR_INVAL);
        return FNET_ERR;
    }
    const fnet_error_t error = addr_check_len(AF_INET, *namelen);
    if (error != FNET_ERR_OK) {
        set_socket_error(socket, error);
        return FNET_ERR;
    }
    if (!socket->peer_known) {
        set_socket_error(socket, FNET_ERR_NOTCONN);
        return FNET_ERR;
    }
    *name = test_peer_address();
    return FNET_OK;
}

fnet_return_t fnet_socket_close(fnet_socket_t descriptor)
{
    observe_socket_access();
    FakeFnetSocket *socket = socket_find(descriptor);
    if (socket == nullptr) {
        /* fnet_socket.c:767-771. The descriptor is not FNET's, so there is
         * nothing to release and nothing to leak -- but the global error word
         * is overwritten, and fnet_error.h:200-201 says nothing clears it. */
        set_error(FNET_ERR_BAD_DESC);
        fake.socket_close_failures++;
        return FNET_ERR;
    }
    socket->allocated = false;
    fake.socket_close_calls++;
    record("socket-close");
    inject_cleanup_callback(fake.service_callback, fake.service_cookie);
    return FNET_OK;
}

fnet_error_t fnet_error_get(void) { return fake.socket_error; }
void fnet_error_set(fnet_error_t error) { fake.socket_error = error; }

void fnet_memcpy(void *dst, const void *src, fnet_size_t size)
{
    std::memcpy(dst, src, size);
}

void fnet_memset(void *dst, fnet_uint8_t value, fnet_size_t size)
{
    std::memset(dst, value, size);
}

fnet_return_t fnet_netif_get_hw_addr(
    fnet_netif_desc_t netif, fnet_uint8_t *, fnet_size_t)
{
    return netif == nullptr ? FNET_ERR : FNET_OK;
}

fnet_return_t fnet_netif_set_hw_addr(
    fnet_netif_desc_t netif, fnet_uint8_t *, fnet_size_t)
{
    return netif == nullptr ? FNET_ERR : FNET_OK;
}

fnet_ip4_addr_t fnet_netif_get_ip4_addr(fnet_netif_desc_t)
{
    return fake.ip4_address;
}
fnet_ip4_addr_t fnet_netif_get_ip4_subnet_mask(fnet_netif_desc_t) { return 0; }
fnet_ip4_addr_t fnet_netif_get_ip4_gateway(fnet_netif_desc_t) { return 0; }
fnet_ip4_addr_t fnet_netif_get_ip4_dns(fnet_netif_desc_t) { return 0; }
void fnet_netif_set_ip4_addr(
    fnet_netif_desc_t, fnet_ip4_addr_t, fnet_ip4_addr_t) {}
void fnet_netif_set_ip4_gateway(fnet_netif_desc_t, fnet_ip4_addr_t) {}
void fnet_netif_set_ip4_dns(fnet_netif_desc_t, fnet_ip4_addr_t) {}

void DNSClient::begin(const IPAddress &) {}
int DNSClient::getHostByName(const char *, IPAddress &, uint16_t) { return 0; }
int DNSClient::inet_aton(const char *, IPAddress &) { return 0; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::string scenario(argv[1]);
    reset_socket_model();
    if (scenario == "fnet-init-failure") {
        test_retry_after_fnet_init_failure();
        std::puts("NativeEthernet fnet-init retry: PASS");
        return 0;
    }
    if (scenario == "netif-init-failure") {
        test_retry_after_netif_init_failure();
        std::puts("NativeEthernet netif-init retry: PASS");
        return 0;
    }
    if (scenario == "link-init-failure") {
        test_retry_after_link_init_failure();
        std::puts("NativeEthernet link-init retry: PASS");
        return 0;
    }
    if (scenario == "adapter-io") {
        test_adapter_socket_result_mapping();
        std::puts("Teensy adapter raw socket mapping: PASS");
        return 0;
    }
    assert(scenario == "lifecycle");
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 4};
    assert(Ethernet.beginNoWait(mac));
    test_dhcp_ownership_and_stale_replacement();
    test_ordered_idempotent_teardown_and_late_callbacks();
    test_destroyed_server_has_no_live_object_cookie();
    /* The FNET-fidelity group. Each of these needs a socket of its own, so they
     * run after the teardown-ordering tests that assert absolute create and
     * close counts. */
    test_close_answers_and_frees_the_descriptor();
    test_accept_addrlen_contract();
    test_peek_does_not_consume_but_a_plain_receive_does();
    test_send_capacity_comes_from_two_accounts();
    test_getopt_validates_level_length_and_option();
    std::puts("NativeEthernet fake-FNET behavior: PASS");
    return 0;
}
