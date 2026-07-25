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

namespace {

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
    int send_calls;
    int send_capacity;
    int send_result;
    int sendto_result;
    int recv_result;
    int recvfrom_result;
    int socket_error;
    int accepted_socket_number;
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
    fnet_socket_state_t socket_state[16];
    std::vector<std::string> events;
} fake = {};

int user_link_callbacks;
int user_dhcp_callbacks;

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

size_t socket_index(fnet_socket_t socket)
{
    uintptr_t encoded = reinterpret_cast<uintptr_t>(socket);
    assert(encoded > 0 && encoded <= 16);
    return encoded - 1u;
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

    fake.link_callback(nullptr, true, fake.link_cookie);
    fake.dhcp_callback(
        reinterpret_cast<void *>(0x22), nullptr, fake.dhcp_cookie);
    assert(user_link_callbacks == 1);
    assert(user_dhcp_callbacks == 1);
}

void test_capacity_bounded_single_send()
{
    EthernetClient client(0);
    fake.send_capacity = 0;
    fake.send_result = 99;
    uint8_t bytes[12] = {};
    int sends = fake.send_calls;
    assert(client.write(bytes, sizeof(bytes)) == 0);
    assert(fake.send_calls == sends);

    fake.send_capacity = 5;
    fake.send_result = 5;
    assert(client.write(bytes, sizeof(bytes)) == 5);
    assert(fake.send_calls == sends + 1);
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

    int socket_activity = fake.socket_create_calls + fake.socket_close_calls;
    late_link(nullptr, true, late_link_cookie);
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
     * NativeEthernet callback chain; the next poll releases STACK_STARTED
     * once, and only then does the supervisor's START_DHCP make sense. */
    fake.link_connected = true;
    fake.link_callback(nullptr, true, fake.link_cookie);
    assert(Ethernet.linkStatus() == LinkON);
    aius_network_teensy_poll(&adapter, 2u, &event);
    assert(event.kind == AIUS_NET_EVENT_STACK_STARTED);
    assert(event.generation == 9u);
    aius_network_teensy_poll(&adapter, 3u, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);

    /* Link-up was released by the link service's own callback, not by the
     * init-time snapshot -- which is exactly the distinction link_up_source
     * exists to draw. */
    assert(diag->link_up_source == AIUS_NETWORK_TEENSY_LINK_UP_CALLBACK);
    assert(diag->link_up_ms == 2u);
    assert(diag->link_callback_up_seen && diag->link_callback_up_ms == 2u);
    assert(diag->link_callbacks == 1u);

    aius_network_teensy_apply(&adapter, 4u, dhcp, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);
    /* FNET's own client state and the MAC's counters, read through the real
     * fnet_dhcp_cln_get_by_netif()/get_state() and fnet_netif_get_statistics()
     * calls the adapter makes. */
    assert(diag->dhcp_started_seen && diag->dhcp_start_ms == 4u);
    assert(diag->dhcp_state_at_start == 1u);
    assert(diag->frames_valid);
    assert(fake.dhcp_state_calls == 1);
    assert(fake.statistics_calls == 1);
    assert(aius_network_teensy_dhcp_state(&adapter) == 1u);
    assert(aius_network_teensy_link_live(&adapter) ==
           AIUS_NETWORK_TEENSY_LINK_UP);

    fake.tx_packet = 11;
    fake.rx_packet = 14;
    fake.dhcp_state = 5;
    aius_network_teensy_apply(&adapter, 5u, publish, &event);
    assert(adapter.listener_open && !adapter.udp_open);
    aius_network_teensy_apply(&adapter, 6u, none, &event);
    assert(adapter.online && adapter.udp_open);
    /* Going online ends the record: four frames left the MAC across this
     * generation's DHCP window and FNET reports the lease bound. */
    assert(diag->end_reason == AIUS_NETWORK_TEENSY_END_ONLINE);
    assert(diag->end_ms == 6u);
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

    fake.accepted_socket_number = 3;
    fake.service_callback(fake.service_cookie);
    io = aius_network_teensy_accept(&adapter);
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 0u &&
           io.fault == 0u);

    fake.recv_result = 5;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 5u &&
           io.fault == 0u);

    fake.recv_result = FNET_ERR;
    fake.socket_error = FNET_ERR_AGAIN;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    fake.recv_result = 0;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_CLOSED && io.bytes == 0u &&
           io.fault == 0u);

    fake.recv_result = FNET_ERR;
    fake.socket_error = -9;
    io = aius_network_teensy_read_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);

    fake.send_capacity = 4;
    fake.send_result = 4;
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 4u &&
           io.fault == 0u);

    int sends = fake.send_calls;
    fake.send_capacity = 0;
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u && fake.send_calls == sends);

    fake.send_capacity = 4;
    fake.send_result = FNET_ERR;
    fake.socket_error = FNET_ERR_AGAIN;
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);

    fake.socket_error = -9;
    io = aius_network_teensy_write_once(&adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);

    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    fake.recvfrom_result = 0;
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    fake.recvfrom_result = FNET_ERR;
    fake.socket_error = FNET_ERR_AGAIN;
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    fake.socket_error = -9;
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);

    fake.recvfrom_result = 5;
    io = aius_network_teensy_udp_receive_once(
        &adapter, buffer, sizeof(buffer));
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 5u &&
           io.fault == 0u);

    fake.sendto_result = FNET_ERR;
    fake.socket_error = FNET_ERR_AGAIN;
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_WOULD_BLOCK && io.bytes == 0u &&
           io.fault == 0u);

    fake.socket_error = -9;
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_FAULT && io.bytes == 0u &&
           io.fault == AIUS_NET_FAULT_SOCKET);

    fake.sendto_result = 4;
    io = aius_network_teensy_udp_send_once(&adapter, buffer, 4u);
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 4u &&
           io.fault == 0u);

    const int closes = fake.socket_close_calls;
    io = aius_network_teensy_close_once(&adapter);
    assert(io.result == AIUS_IO_PROGRESS && io.bytes == 0u &&
           io.fault == 0u);
    assert(fake.socket_close_calls == closes + 1);
    io = aius_network_teensy_close_once(&adapter);
    assert(io.result == AIUS_IO_CLOSED && io.bytes == 0u &&
           io.fault == 0u);
    assert(fake.socket_close_calls == closes + 1);
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
    fake.link_callback(nullptr, false, fake.link_cookie);
    assert(Ethernet.linkStatus() == LinkOFF);
    assert(aius_network_teensy_link(&adapter) ==
           AIUS_NETWORK_TEENSY_LINK_DOWN);
    fake.link_connected = true;
    fake.link_callback(nullptr, true, fake.link_cookie);
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

    aius_network_teensy_apply(&adapter, 200u, restart, &event);
    assert(adapter.stack_started);
    assert(fake.netif_init_calls == netif_inits);
    diag = aius_network_teensy_diag(&adapter);
    assert(diag->generation == 10u);
    assert(diag->netif_init == AIUS_NETWORK_TEENSY_NETIF_INIT_SKIPPED);

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

fnet_return_t fnet_init(fnet_init_params *)
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
    return fake.netif_initialized;
}

fnet_return_t fnet_netif_init(void *, const uint8_t *, size_t)
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

fnet_netif_desc_t fnet_netif_get_default()
{
    return fake.netif_initialized ? reinterpret_cast<void *>(0x10) : nullptr;
}

fnet_bool_t fnet_netif_is_connected(fnet_netif_desc_t)
{
    return fake.link_connected;
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
        fake.link_callback, reinterpret_cast<void *>(0x10), true,
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
    return fake.dhcp_enabled;
}

void fnet_dhcp_cln_set_response_timeout(fnet_dhcp_cln_desc_t, uint32_t) {}

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
    fnet_dhcp_cln_desc_t, fnet_dhcp_cln_options *options)
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
    fnet_netif_desc_t netif, fnet_netif_statistics *statistics)
{
    fake.statistics_calls++;
    if (netif == nullptr || !fake.statistics_available)
        return FNET_ERR;
    statistics->tx_packet = fake.tx_packet;
    statistics->rx_packet = fake.rx_packet;
    return FNET_OK;
}

void fnet_poll() {}

fnet_service_desc_t fnet_service_register(
    fnet_service_poll_t callback, void *cookie)
{
    fake.service_register_calls++;
    fake.service_callback = callback;
    fake.service_cookie = cookie;
    record("service-register");
    return 31;
}

void fnet_service_unregister(fnet_service_desc_t)
{
    fake.service_unregister_calls++;
    record("service-unregister");
    inject_cleanup_callback(fake.service_callback, fake.service_cookie);
}

fnet_socket_t fnet_socket(int, int, int)
{
    observe_socket_access();
    fake.socket_create_calls++;
    size_t index = static_cast<size_t>(fake.socket_create_calls - 1);
    fake.socket_state[index] = 1;
    record("socket-create");
    return reinterpret_cast<void *>(index + 1u);
}

fnet_return_t fnet_socket_bind(
    fnet_socket_t, const fnet_sockaddr *, fnet_size_t)
{
    observe_socket_access();
    return FNET_OK;
}

fnet_return_t fnet_socket_setopt(
    fnet_socket_t, int, int, const void *, fnet_size_t)
{
    observe_socket_access();
    return FNET_OK;
}

fnet_return_t fnet_socket_listen(fnet_socket_t socket, int)
{
    observe_socket_access();
    fake.socket_state[socket_index(socket)] = SS_LISTENING;
    return FNET_OK;
}

fnet_return_t fnet_socket_connect(
    fnet_socket_t, const fnet_sockaddr *, fnet_size_t)
{
    observe_socket_access();
    return FNET_OK;
}

fnet_return_t fnet_socket_shutdown(fnet_socket_t, int) { return FNET_OK; }

fnet_ssize_t fnet_socket_send(
    fnet_socket_t, const void *, size_t length, fnet_flag_t)
{
    observe_socket_access();
    fake.send_calls++;
    record("socket-send");
    return std::min<int>(fake.send_result, static_cast<int>(length));
}

fnet_ssize_t fnet_socket_recv(
    fnet_socket_t, void *, size_t, fnet_flag_t)
{
    observe_socket_access();
    return static_cast<fnet_ssize_t>(fake.recv_result);
}

fnet_ssize_t fnet_socket_sendto(
    fnet_socket_t, const void *, size_t length, fnet_flag_t,
    const fnet_sockaddr *, fnet_size_t)
{
    observe_socket_access();
    if (fake.sendto_result != 0)
        return static_cast<fnet_ssize_t>(fake.sendto_result);
    return static_cast<fnet_ssize_t>(length);
}

fnet_socket_t fnet_socket_accept(
    fnet_socket_t, fnet_sockaddr *, fnet_size_t *)
{
    observe_socket_access();
    if (fake.accepted_socket_number == 0)
        return nullptr;
    const int socket_number = fake.accepted_socket_number;
    fake.accepted_socket_number = 0;
    fake.socket_state[socket_number - 1] = SS_CONNECTED;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(socket_number));
}

fnet_return_t fnet_socket_poll(fnet_socket_poll_t *, size_t) { return 0; }

fnet_return_t fnet_socket_getopt(
    fnet_socket_t socket, int, int option, void *value, fnet_size_t *)
{
    observe_socket_access();
    if (option == SO_STATE) {
        *static_cast<fnet_socket_state_t *>(value) =
            fake.socket_state[socket_index(socket)];
    } else if (option == SO_SNDNUM) {
        *static_cast<fnet_ssize_t *>(value) = 0;
    } else if (option == SO_SNDBUF) {
        *static_cast<fnet_ssize_t *>(value) = fake.send_capacity;
    } else if (option == SO_RCVNUM) {
        *static_cast<fnet_ssize_t *>(value) = 0;
    }
    return FNET_OK;
}

fnet_ssize_t fnet_socket_recvfrom(
    fnet_socket_t, void *, size_t, fnet_flag_t, fnet_sockaddr *, fnet_size_t *)
{
    observe_socket_access();
    return static_cast<fnet_ssize_t>(fake.recvfrom_result);
}

fnet_return_t fnet_socket_getpeername(
    fnet_socket_t, fnet_sockaddr *, fnet_size_t *)
{
    observe_socket_access();
    return FNET_ERR;
}

void fnet_socket_close(fnet_socket_t socket)
{
    observe_socket_access();
    if (socket == nullptr)
        return;
    fake.socket_state[socket_index(socket)] = 0;
    fake.socket_close_calls++;
    record("socket-close");
    inject_cleanup_callback(fake.service_callback, fake.service_cookie);
}

int8_t fnet_error_get() { return static_cast<int8_t>(fake.socket_error); }
void *fnet_memcpy(void *dst, const void *src, size_t size) {
    return std::memcpy(dst, src, size);
}
void *fnet_memset(void *dst, int value, size_t size) {
    return std::memset(dst, value, size);
}

void fnet_netif_get_hw_addr(fnet_netif_desc_t, uint8_t *, size_t) {}
uint32_t fnet_netif_get_ip4_addr(fnet_netif_desc_t) { return fake.ip4_address; }
uint32_t fnet_netif_get_ip4_subnet_mask(fnet_netif_desc_t) { return 0; }
uint32_t fnet_netif_get_ip4_gateway(fnet_netif_desc_t) { return 0; }
uint32_t fnet_netif_get_ip4_dns(fnet_netif_desc_t) { return 0; }
void fnet_netif_set_hw_addr(fnet_netif_desc_t, uint8_t *, size_t) {}
void fnet_netif_set_ip4_addr(fnet_netif_desc_t, uint32_t, uint32_t) {}
void fnet_netif_set_ip4_gateway(fnet_netif_desc_t, uint32_t) {}
void fnet_netif_set_ip4_dns(fnet_netif_desc_t, uint32_t) {}

void DNSClient::begin(const IPAddress &) {}
int DNSClient::getHostByName(const char *, IPAddress &, uint16_t) { return 0; }
int DNSClient::inet_aton(const char *, IPAddress &) { return 0; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::string scenario(argv[1]);
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
    test_capacity_bounded_single_send();
    test_ordered_idempotent_teardown_and_late_callbacks();
    test_destroyed_server_has_no_live_object_cookie();
    std::puts("NativeEthernet fake-FNET behavior: PASS");
    return 0;
}
