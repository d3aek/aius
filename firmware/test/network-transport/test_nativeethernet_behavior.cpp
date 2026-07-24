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
    int cleanup_callback_injections;
    int callback_socket_api_calls;
    bool fnet_active;
    bool netif_initialized;
    bool dhcp_enabled;
    bool inject_callbacks_during_cleanup;
    bool inside_cleanup_callback;
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
    aius_network_teensy_apply(&adapter, 0u, start, &event);
    assert(event.kind == AIUS_NET_EVENT_STACK_STARTED);
    aius_network_teensy_apply(&adapter, 1u, dhcp, &event);
    assert(event.kind == AIUS_NET_EVENT_NONE);
    aius_network_teensy_apply(&adapter, 2u, publish, &event);
    assert(adapter.listener_open && !adapter.udp_open);
    aius_network_teensy_apply(&adapter, 3u, none, &event);
    assert(adapter.online && adapter.udp_open);

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

fnet_bool_t fnet_netif_is_connected(fnet_netif_desc_t) { return false; }

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
uint32_t fnet_netif_get_ip4_addr(fnet_netif_desc_t) { return 0; }
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
