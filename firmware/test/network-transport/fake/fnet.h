#ifndef AIUS_TEST_FAKE_FNET_H
#define AIUS_TEST_FAKE_FNET_H

#include <stddef.h>
#include <stdint.h>

#define FNET_CFG_SOCKET_MAX 8
#define FNET_CFG_TLS 0
#define FNET_OK 0
#define FNET_ERR (-1)
#define FNET_ERR_AGAIN (-4)
#define FNET_TRUE 1
#define FNET_FALSE 0
#define FNET_NULL nullptr
#define FNET_CPU_ETH0_IF reinterpret_cast<void *>(1)
#define FNET_HTONS(value) (value)
#define FNET_IP4_ADDR_INIT(a, b, c, d) \
    (uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24))

#define AF_INET 2
#define SOCK_DGRAM 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_RCVBUF 2
#define SO_SNDBUF 3
#define SO_STATE 4
#define SO_RCVNUM 5
#define SO_SNDNUM 6
#define SO_LINGER 7
#define IPPROTO_TCP 6
#define IPPROTO_IP 0
#define TCP_NODELAY 1
#define IP_ADD_MEMBERSHIP 2
#define IP_MULTICAST_TTL 3
#define IP_TTL 4
#define SD_WRITE 1
#define MSG_PEEK 2
#define INADDR_ANY 0
#define FNET_SOCKET_EVENT_ALL 0xff
#define FNET_SOCKET_EVENT_OUT 1
#define FNET_SOCKET_EVENT_IN 2
#define FNET_SOCKET_EVENT_ERR 4
#define SS_CLOSED 0
#define SS_CLOSING 1
#define SS_CONNECTING 2
#define SS_CONNECTED 3
#define SS_LISTENING 4

typedef int fnet_return_t;
typedef int fnet_bool_t;
typedef uint32_t fnet_time_t;
typedef uint32_t fnet_uint32_t;
typedef int32_t fnet_int32_t;
typedef uint8_t fnet_uint8_t;
typedef int32_t fnet_ssize_t;
typedef size_t fnet_size_t;
typedef uint32_t fnet_flag_t;
typedef void *fnet_netif_desc_t;
typedef void *fnet_socket_t;
typedef int32_t fnet_link_desc_t;
typedef void *fnet_dhcp_cln_desc_t;
typedef int32_t fnet_service_desc_t;
typedef void *fnet_mutex_t;
typedef uint32_t fnet_ip4_addr_t;
typedef int fnet_socket_state_t;
typedef int fnet_socket_event_t;
typedef void *fnet_mdns_desc_t;
typedef void *fnet_mdns_service_desc_t;

typedef void (*fnet_link_callback_t)(fnet_netif_desc_t, fnet_bool_t, void *);
typedef void (*fnet_dhcp_cln_callback_t)(
    fnet_dhcp_cln_desc_t, fnet_netif_desc_t, void *);
typedef void (*fnet_service_poll_t)(void *);

struct fnet_mutex_api_t {
    fnet_return_t (*mutex_init)(fnet_mutex_t *);
    void (*mutex_release)(fnet_mutex_t *);
    void (*mutex_lock)(fnet_mutex_t *);
    void (*mutex_unlock)(fnet_mutex_t *);
};

struct fnet_timer_api_t {
    fnet_time_t (*timer_get_ms)(void);
    void (*timer_delay)(fnet_time_t);
};

struct fnet_init_params {
    uint8_t *netheap_ptr;
    size_t netheap_size;
    const fnet_mutex_api_t *mutex_api;
    const fnet_timer_api_t *timer_api;
};

struct fnet_link_params_t {
    fnet_netif_desc_t netif_desc;
    fnet_link_callback_t callback;
    void *callback_param;
};

struct fnet_dhcp_cln_params_t {
    fnet_netif_desc_t netif;
};

struct fnet_in_addr {
    uint32_t s_addr;
};

struct fnet_dhcp_cln_options {
    fnet_in_addr ip_address;
    fnet_in_addr netmask;
    fnet_in_addr gateway;
    fnet_in_addr dhcp_server;
};

struct fnet_sockaddr {
    uint16_t sa_port;
    uint8_t sa_data[4];
};

struct fnet_sockaddr_in {
    int sin_family;
    uint16_t sin_port;
    fnet_in_addr sin_addr;
};

struct fnet_ip_mreq {
    fnet_in_addr imr_multiaddr;
    int imr_interface;
};

struct fnet_linger {
    fnet_bool_t l_onoff;
    uint32_t l_linger;
};

struct fnet_socket_poll_t {
    fnet_socket_t s;
    fnet_socket_event_t events;
    fnet_socket_event_t events_occurred;
};

struct fnet_mdns_txt_key_t {
    const char *key;
    const char *value;
};

struct fnet_dns_resolved_addr_t {
    fnet_sockaddr resolved_addr;
};

fnet_return_t fnet_init(fnet_init_params *);
fnet_bool_t fnet_netif_is_initialized(fnet_netif_desc_t);
fnet_return_t fnet_netif_init(void *, const uint8_t *, size_t);
fnet_netif_desc_t fnet_netif_get_default();
fnet_bool_t fnet_netif_is_connected(fnet_netif_desc_t);
fnet_link_desc_t fnet_link_init(fnet_link_params_t *);
void fnet_link_release(fnet_link_desc_t);
fnet_dhcp_cln_desc_t fnet_dhcp_cln_init(fnet_dhcp_cln_params_t *);
void fnet_dhcp_cln_release(fnet_dhcp_cln_desc_t);
fnet_bool_t fnet_dhcp_cln_is_enabled(fnet_dhcp_cln_desc_t);
void fnet_dhcp_cln_set_response_timeout(fnet_dhcp_cln_desc_t, uint32_t);
void fnet_dhcp_cln_set_callback_updated(
    fnet_dhcp_cln_desc_t, fnet_dhcp_cln_callback_t, void *);
fnet_dhcp_cln_desc_t fnet_dhcp_cln_get_by_netif(fnet_netif_desc_t);
void fnet_dhcp_cln_get_options(
    fnet_dhcp_cln_desc_t, fnet_dhcp_cln_options *);
/* Diagnostic readers. fnet_dhcp_cln_get_state() returns FNET's own DHCP client
 * state; the values come from the private enum in
 * service/dhcp/fnet_dhcp_cln.c (0 disabled, 1 init, 2 selecting, 3 requesting,
 * 4 probing, 5 bound, 6 renewing, 7 rebinding, 8 init-reboot, 9 rebooting).
 * The real getter dereferences its descriptor without a null check, which is
 * why the adapter looks the descriptor up first. */
fnet_uint8_t fnet_dhcp_cln_get_state(fnet_dhcp_cln_desc_t);

struct fnet_netif_statistics {
    fnet_uint32_t tx_packet;
    fnet_uint32_t rx_packet;
};

fnet_return_t fnet_netif_get_statistics(
    fnet_netif_desc_t, fnet_netif_statistics *);

void fnet_poll();
fnet_service_desc_t fnet_service_register(fnet_service_poll_t, void *);
void fnet_service_unregister(fnet_service_desc_t);
fnet_socket_t fnet_socket_accept(
    fnet_socket_t, fnet_sockaddr *, fnet_size_t *);
fnet_socket_t fnet_socket(int, int, int);
fnet_return_t fnet_socket_bind(
    fnet_socket_t, const fnet_sockaddr *, fnet_size_t);
fnet_return_t fnet_socket_setopt(
    fnet_socket_t, int, int, const void *, fnet_size_t);
fnet_return_t fnet_socket_listen(fnet_socket_t, int);
fnet_return_t fnet_socket_connect(
    fnet_socket_t, const fnet_sockaddr *, fnet_size_t);
fnet_return_t fnet_socket_shutdown(fnet_socket_t, int);
fnet_ssize_t fnet_socket_send(
    fnet_socket_t, const void *, size_t, fnet_flag_t);
fnet_ssize_t fnet_socket_recv(
    fnet_socket_t, void *, size_t, fnet_flag_t);
fnet_ssize_t fnet_socket_sendto(
    fnet_socket_t, const void *, size_t, fnet_flag_t,
    const fnet_sockaddr *, fnet_size_t);
fnet_return_t fnet_socket_poll(fnet_socket_poll_t *, size_t);
fnet_return_t fnet_socket_getopt(
    fnet_socket_t, int, int, void *, fnet_size_t *);
fnet_ssize_t fnet_socket_recvfrom(
    fnet_socket_t, void *, size_t, fnet_flag_t, fnet_sockaddr *, fnet_size_t *);
fnet_return_t fnet_socket_getpeername(
    fnet_socket_t, fnet_sockaddr *, fnet_size_t *);
void fnet_socket_close(fnet_socket_t);
int8_t fnet_error_get();
void *fnet_memcpy(void *, const void *, size_t);
void *fnet_memset(void *, int, size_t);

void fnet_netif_get_hw_addr(fnet_netif_desc_t, uint8_t *, size_t);
uint32_t fnet_netif_get_ip4_addr(fnet_netif_desc_t);
uint32_t fnet_netif_get_ip4_subnet_mask(fnet_netif_desc_t);
uint32_t fnet_netif_get_ip4_gateway(fnet_netif_desc_t);
uint32_t fnet_netif_get_ip4_dns(fnet_netif_desc_t);
void fnet_netif_set_hw_addr(fnet_netif_desc_t, uint8_t *, size_t);
void fnet_netif_set_ip4_addr(fnet_netif_desc_t, uint32_t, uint32_t);
void fnet_netif_set_ip4_gateway(fnet_netif_desc_t, uint32_t);
void fnet_netif_set_ip4_dns(fnet_netif_desc_t, uint32_t);

#endif
