#ifndef AIUS_TEST_FAKE_FNET_H
#define AIUS_TEST_FAKE_FNET_H

/* A stand-in for FNET 4.7.1 as the Teensy 4.1 build configures it, derived from
 * the dependency's own headers rather than from the code that calls it.
 *
 * That distinction is the whole point of this file. Its first version declared
 * exactly the names src/aius_network_teensy.cpp referenced, with values chosen
 * to make the caller compile -- so the test suite could only ever confirm the
 * caller's beliefs. It cost a live defect: with no FNET_ERR_CONNCLOSED in scope,
 * the only expressible way to write a "peer closed" row was `recv_result = 0`,
 * which is the POSIX convention and not FNET's, and read_once() tore live
 * connections down for months with a committed test asserting it was correct.
 *
 * Every declaration below therefore cites the real header it was copied from.
 * When a value here disagrees with the real one it is because the fake models
 * the *target's* ABI rather than the host's, and it says so.
 *
 * Sources, all under
 *   $PLATFORMIO_CORE_DIR/packages/framework-arduinoteensy/libraries/FNET/src/
 *   fnet.h                     FNET_VERSION "4.7.1"
 *   fnet_user_config.h         fnet_return_t, FNET_HTONS, FNET_CFG_* overrides
 *   stack/fnet_stdlib.h        fnet_size_t, fnet_ssize_t, fnet_bool_t, FNET_NULL
 *   stack/fnet_error.h         fnet_error_t
 *   stack/fnet_socket.h        the socket API, its enums and its structures
 *   stack/fnet_netif.h         the interface API and fnet_netif_statistics
 *   stack/fnet_stack.h         fnet_init(), fnet_poll(), fnet_init_params
 *   stack/fnet_ip4.h           fnet_ip4_addr_t, FNET_IP4_ADDR_INIT
 *   service/fnet_service.h     the service registry
 *   service/link/fnet_link.h   the link-detection service
 *   service/dhcp/fnet_dhcp_cln.h  the DHCPv4 client
 *
 * The configuration this models is the one fnet_config.h resolves for
 * ARDUINO_TEENSY41: FNET_CFG_IP4 = 1, FNET_CFG_IP6 = 0 (fnet_user_config.h:333
 * leaves it commented, stack/fnet_stack_config.h:49 defaults it to 0),
 * FNET_CFG_MULTITHREADING = 1, FNET_CFG_TIMER_ALT = 1, little-endian CPU.
 *
 * FNET_CFG_TLS is 0 on both sides, and that is now a pin rather than a
 * coincidence. FNET's own default is 2 (fnet_user_config.h:443-445, behind an
 * #ifndef), and platformio.ini used to override nothing -- so the *target*
 * compiled the vendor transport's `#if FNET_CFG_TLS` branches in
 * Nativesocket.cpp, NativeEthernetClient.cpp and NativeEthernetServer.cpp while
 * scripts/test.sh compiled this suite with -DFNET_CFG_TLS=0, and the suite was
 * proving code the image did not contain. `[common] build_flags` in
 * platformio.ini now carries -DFNET_CFG_TLS=0, so the two agree. Do not remove
 * either flag without removing both.
 */

#include <stddef.h>
#include <stdint.h>

/* fnet_user_config.h:13,332-333,472 and stack/fnet_stack_config.h:486. The real
 * FNET_CFG_SOCKET_MAX default is 12; the earlier fake said 8, which is
 * MAX_SOCK_NUM (NativeEthernet.h:39) -- the vendor's own limit, not FNET's.
 * EthernetClass::setSocketNum() clamps MAX_SOCK_NUM against this value
 * (NativeEthernet.cpp:69), so the two are not interchangeable. */
#define FNET_CFG_SOCKET_MAX 12
#define FNET_CFG_IP4 1
#define FNET_CFG_IP6 0
#define FNET_CFG_MULTITHREADING 1
#define FNET_CFG_TIMER_ALT 1
/* 0, which is what platformio.ini now pins the target to as well -- not FNET's
 * own #ifndef default of 2. Defaulting it to 2 here would compile the vendor's
 * `#if FNET_CFG_TLS` branches against fnet_tls_* declarations this fake does not
 * provide, so the gap was always going to close by pinning the target rather
 * than by faking a TLS service nobody wants compiled. */
#ifndef FNET_CFG_TLS
#define FNET_CFG_TLS 0
#endif

/* stack/fnet_stdlib.h:43. */
#define FNET_NULL ((void *)0)

/* stack/fnet_stdlib.h:55-65. Real: `unsigned long` / `long` / `unsigned int`.
 * Spelling them `long` here would make them 64-bit on this LP64 host while the
 * ILP32 target has them at 32, and that difference is observable: real
 * fnet_socket_getopt() writes exactly sizeof(fnet_uint32_t) bytes into the
 * caller's option variable (fnet_socket.c:1562-1571), so a 64-bit fnet_ssize_t
 * would leave EthernetClass::socketSendAvailable()'s `_max` half
 * uninitialised on the host and exact on the board. These typedefs therefore
 * model the target's widths, which is the ABI the dependency actually has where
 * it runs. */
typedef uint32_t fnet_size_t;
typedef int32_t fnet_ssize_t;
typedef unsigned int fnet_flag_t;
typedef unsigned int fnet_index_t;
typedef char fnet_char_t;

typedef uint8_t fnet_uint8_t;
typedef uint16_t fnet_uint16_t;
typedef uint32_t fnet_uint32_t;
typedef int8_t fnet_int8_t;
typedef int16_t fnet_int16_t;
typedef int32_t fnet_int32_t;

/* stack/fnet_stdlib.h:80-84 -- an enum, not an int. */
typedef enum {
    FNET_FALSE = 0,
    FNET_TRUE = 1
} fnet_bool_t;

/* fnet_user_config.h:60-64 -- an enum, not a pair of #defines. */
typedef enum {
    FNET_OK = 0,
    FNET_ERR = -1
} fnet_return_t;

/* stack/fnet_error.h:41-180. The full set, in the real numbering, whether or
 * not the adapter names it: a fake that publishes only the codes its caller
 * mentions is how the CONNCLOSED defect stayed invisible. FNET does not use the
 * POSIX receive convention -- a connected socket with an empty receive buffer
 * returns 0 from fnet_socket_recv() ("nothing yet", stack/fnet_tcp.c:776), and
 * peer closure arrives as FNET_ERR plus FNET_ERR_CONNCLOSED (fnet_tcp.c:768-772)
 * or FNET_ERR_NOTCONN (754-757) from fnet_error_get(). */
typedef enum {
    FNET_ERR_OK = 0,
    FNET_ERR_NO_DESC = -2,
    FNET_ERR_NOMEM = -3,
    FNET_ERR_AGAIN = -4,
    FNET_ERR_BOUNDREQ = -5,
    FNET_ERR_INVAL = -6,
    FNET_ERR_DESTADDRREQ = -7,
    FNET_ERR_MSGSIZE = -8,
    FNET_ERR_NOPROTOOPT = -9,
    FNET_ERR_PROTONOSUPPORT = -10,
    FNET_ERR_OPNOTSUPP = -11,
    FNET_ERR_AFNOSUPPORT = -12,
    FNET_ERR_ADDRINUSE = -14,
    FNET_ERR_ADDRNOTAVAIL = -15,
    FNET_ERR_NETUNREACH = -16,
    FNET_ERR_CONNABORTED = -17,
    FNET_ERR_CONNRESET = -18,
    FNET_ERR_ISCONN = -19,
    FNET_ERR_NOTCONN = -20,
    FNET_ERR_SHUTDOWN = -21,
    FNET_ERR_INPROGRESS = -22,
    FNET_ERR_TIMEDOUT = -23,
    FNET_ERR_HOSTUNREACH = -24,
    FNET_ERR_SYSNOTREADY = -25,
    FNET_ERR_CONNCLOSED = -26,
    FNET_ERR_IPDISABLED = -27,
    FNET_ERR_BAD_DESC = -28
} fnet_error_t;

/* port/cpu/fnet_cpu.h:83-120, little-endian branch. The earlier fake defined
 * FNET_HTONS as the identity -- the *big*-endian branch (:206) -- while its
 * FNET_IP4_ADDR_INIT expanded to the little-endian result. One fake, two
 * endiannesses, and neither caller noticed because every use round-trips
 * through the same macro. */
#define FNET_HTONS(short_var)                                    \
    ((fnet_uint16_t)(((((fnet_uint16_t)(short_var)) & 0x00FFU)    \
                      << 8U) |                                   \
                     ((((fnet_uint16_t)(short_var)) & 0xFF00U) >> 8U)))
#define FNET_NTOHS(short_var) FNET_HTONS(short_var)
#define FNET_HTONL(long_var)                                     \
    ((fnet_uint32_t)((((fnet_uint32_t)(long_var) & 0x000000FFU) << 24U) |   \
                     (((fnet_uint32_t)(long_var) & 0x0000FF00U) << 8U) |    \
                     (((fnet_uint32_t)(long_var) & 0x00FF0000U) >> 8U) |    \
                     (((fnet_uint32_t)(long_var) & 0xFF000000U) >> 24U)))
#define FNET_NTOHL(long_var) FNET_HTONL(long_var)

/* stack/fnet_ip4.h:35,47 verbatim. */
typedef fnet_uint32_t fnet_ip4_addr_t;
#define FNET_IP4_ADDR_INIT(a, b, c, d)                                  \
    ((fnet_ip4_addr_t)(FNET_NTOHL((((fnet_uint32_t)(a) & 0xFFUL) << 24U) + \
                                 (((fnet_uint32_t)(b) & 0xFFUL) << 16U) + \
                                 (((fnet_uint32_t)(c) & 0xFFUL) << 8U) +  \
                                 ((fnet_uint32_t)(d) & 0xFFUL))))

/* port/cpu/fnet_cpu.h:465 is `((fnet_netif_desc_t)(&fnet_cpu_eth0_if))` -- an
 * opaque non-null descriptor, which is all any caller may assume. */
#define FNET_CPU_ETH0_IF reinterpret_cast<void *>(1)

/* stack/fnet_socket.h:125,137,142,147-171. AF_INET is 1, not 2: the earlier
 * fake's 2 is AF_INET6. */
#define INADDR_ANY (fnet_ip4_addr_t)(0x00000000U)
#define INADDR_BROADCAST (fnet_ip4_addr_t)(0xffffffffU)
typedef fnet_uint16_t fnet_address_family_t;
typedef fnet_uint32_t fnet_scope_id_t;
#define AF_UNSPEC (0U)
#define AF_INET (1U)
#define AF_INET6 (2U)
/* FNET_CFG_IP6 is 0 here, so sa_data covers an IPv4 address only and
 * sizeof(struct fnet_sockaddr) == sizeof(struct fnet_sockaddr_in) == 12. That
 * number is load-bearing: _fnet_socket_addr_check_len() (fnet_socket.c:1856)
 * rejects any addrlen below it with FNET_ERR_INVAL. */
#define FNET_SA_DATA_SIZE (sizeof(struct fnet_in_addr))

/* stack/fnet_socket.h:317-337. */
typedef enum {
    SOCK_UNSPEC = 0U,
    SOCK_STREAM = 1U,
    SOCK_DGRAM = 2U,
    SOCK_RAW = 3U
} fnet_socket_type_t;

/* stack/fnet_socket.h:342-349. */
typedef enum {
    SS_CLOSED = 0,
    SS_CLOSING,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_LISTENING
} fnet_socket_state_t;

/* stack/fnet_socket.h:355-369. SOL_SOCKET is 255255, not 1. The earlier fake's
 * 1 collided with SOCK_STREAM, TCP_NODELAY, SD_WRITE and
 * FNET_SOCKET_EVENT_OUT, all of which it also numbered 1. */
typedef enum {
    IPPROTO_IP = 0,
    IPPROTO_TCP = 6,
    IPPROTO_IPV6 = 41,
    SOL_SOCKET = 255255
} fnet_protocol_t;

/* stack/fnet_socket.h:478-631, in declaration order because the real enum
 * assigns no explicit values. Every one of the ten options the earlier fake
 * defined had the wrong number, and three of them aliased an IPPROTO_IP option
 * onto a SOL_SOCKET one (SO_RCVBUF/IP_ADD_MEMBERSHIP, SO_SNDBUF/
 * IP_MULTICAST_TTL, SO_STATE/IP_TTL) -- which mattered because the fake's
 * getopt ignored `level` and would have answered the wrong option. */
typedef enum {
    SO_ACCEPTCONN,
    SO_KEEPALIVE,
    SO_DONTROUTE,
    SO_LINGER,
    SO_OOBINLINE,
    SO_SNDBUF,
    SO_RCVBUF,
    SO_STATE,
    SO_ERROR,
    SO_TYPE,
    SO_RCVNUM,
    SO_SNDNUM,
    SO_LISTENQLIMIT,
    SO_LISTENQLEN,
    TCP_MSS,
    TCP_BSD,
    TCP_NODELAY,
    TCP_FINRCVD,
    TCP_URGRCVD,
    TCP_KEEPIDLE,
    TCP_KEEPINTVL,
    TCP_KEEPCNT,
    IP_TOS,
    IP_TTL,
    IP_MULTICAST_TTL,
    IP_ADD_MEMBERSHIP,
    IP_DROP_MEMBERSHIP,
    IPV6_UNICAST_HOPS,
    IPV6_MULTICAST_HOPS,
    IPV6_JOIN_GROUP,
    IPV6_LEAVE_GROUP
} fnet_socket_options_t;

/* stack/fnet_socket.h:663-678. */
typedef enum {
    MSG_OOB = 0x1U,
    MSG_PEEK = 0x2U,
    MSG_DONTROUTE = 0x4U
} fnet_msg_flags_t;

/* stack/fnet_socket.h:684-694. SD_WRITE is 0x2, not 1 -- the earlier fake's 1
 * is SD_READ, so a fake that ever modelled shutdown would have disabled
 * reception where the vendor asked to disable transmission. */
typedef enum {
    SD_READ = 0x1U,
    SD_WRITE = 0x2U,
    SD_BOTH = 0x3U
} fnet_sd_flags_t;

/* stack/fnet_socket.h:700-709. IN is bit 0 and OUT is bit 1; the earlier fake
 * had the two swapped and put ALL at 0xff instead of 7. */
typedef enum {
    FNET_SOCKET_EVENT_NONE = 0,
    FNET_SOCKET_EVENT_IN = 1 << 0,
    FNET_SOCKET_EVENT_OUT = 1 << 1,
    FNET_SOCKET_EVENT_ERR = 1 << 2,
    FNET_SOCKET_EVENT_ALL = 7
} fnet_socket_event_t;

typedef uint32_t fnet_time_t;
typedef void *fnet_netif_desc_t;
typedef void *fnet_socket_t;
typedef fnet_int32_t fnet_link_desc_t;
typedef void *fnet_dhcp_cln_desc_t;
/* service/fnet_service.h:72 -- a pointer, not an int32. */
typedef void *fnet_service_desc_t;
typedef void *fnet_mutex_t;
typedef void *fnet_mdns_desc_t;
typedef void *fnet_mdns_service_desc_t;

typedef void (*fnet_link_callback_t)(fnet_netif_desc_t, fnet_bool_t, void *);
typedef void (*fnet_dhcp_cln_callback_t)(
    fnet_dhcp_cln_desc_t, fnet_netif_desc_t, void *);
typedef void (*fnet_service_poll_t)(void *);

/* stack/fnet_stack.h:95-101,111-115,122-142. */
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
    void *netheap_ptr;
    fnet_size_t netheap_size;
    const fnet_mutex_api_t *mutex_api;
    const fnet_timer_api_t *timer_api;
};

/* service/link/fnet_link.h:65-71. */
struct fnet_link_params_t {
    fnet_netif_desc_t netif_desc;
    fnet_link_callback_t callback;
    void *callback_param;
};

/* stack/fnet_socket.h:196-199. */
struct fnet_in_addr {
    fnet_ip4_addr_t s_addr;
};

/* service/dhcp/fnet_dhcp_cln.h:109-125. The earlier fake carried only `netif`,
 * so the other three members were unwritable and an uninitialised one would
 * have been invisible here. EthernetClass::dhcpStart() does zero the structure
 * (`fnet_dhcp_cln_params_t params = {}`, NativeEthernet.cpp:184) so nothing is
 * wrong today -- but that is a property of the caller worth being able to
 * observe rather than one the fake should assume. */
typedef struct {
    fnet_netif_desc_t netif;
    struct fnet_in_addr requested_ip_address;
    fnet_uint32_t requested_lease_time;
    fnet_bool_t probe_addr;
} fnet_dhcp_cln_params_t;

/* service/dhcp/fnet_dhcp_cln.h:134-168, with FNET_CFG_DHCP_CLN_BROADCAST and
 * FNET_CFG_DNS at their configured values. */
struct fnet_dhcp_cln_options {
    struct fnet_in_addr ip_address;
    struct fnet_in_addr netmask;
    struct fnet_in_addr gateway;
    struct fnet_in_addr dhcp_server;
    struct fnet_in_addr dns;
    fnet_uint32_t t1;
    fnet_uint32_t t2;
    fnet_uint32_t lease_time;
};

/* stack/fnet_socket.h:268-280 and 210-219. sa_family and sa_scope_id were both
 * absent from the earlier fake and sa_data was sized 4 by coincidence rather
 * than by FNET_SA_DATA_SIZE, so the structure was 6 bytes where the target has
 * 12 -- below the 12 that _fnet_socket_addr_check_len() demands. */
struct fnet_sockaddr {
    fnet_address_family_t sa_family;
    fnet_uint16_t sa_port;
    fnet_scope_id_t sa_scope_id;
    fnet_uint8_t sa_data[4];
};

struct fnet_sockaddr_in {
    fnet_address_family_t sin_family;
    fnet_uint16_t sin_port;
    fnet_scope_id_t sin_scope_id;
    struct fnet_in_addr sin_addr;
};

/* stack/fnet_socket.h:291-296. */
struct fnet_ip_mreq {
    struct fnet_in_addr imr_multiaddr;
    fnet_scope_id_t imr_interface;
};

/* stack/fnet_socket.h:636-647 -- l_linger is 16-bit. */
struct fnet_linger {
    fnet_bool_t l_onoff;
    fnet_uint16_t l_linger;
};

/* stack/fnet_socket.h:714-720. */
typedef struct {
    fnet_socket_t s;
    fnet_socket_event_t events;
    fnet_socket_event_t events_occurred;
} fnet_socket_poll_t;

struct fnet_mdns_txt_key_t {
    const char *key;
    const char *value;
};

struct fnet_dns_resolved_addr_t {
    fnet_sockaddr resolved_addr;
};

/* stack/fnet_netif.h:54-58 verbatim. */
struct fnet_netif_statistics {
    fnet_uint32_t tx_packet;
    fnet_uint32_t rx_packet;
};

/* --- stack/fnet_stack.h ------------------------------------------------- */
fnet_return_t fnet_init(struct fnet_init_params *);
void fnet_poll(void);

/* --- stack/fnet_netif.h ------------------------------------------------- */
fnet_return_t fnet_netif_init(
    fnet_netif_desc_t, fnet_uint8_t *, fnet_size_t);
fnet_bool_t fnet_netif_is_initialized(fnet_netif_desc_t);
fnet_netif_desc_t fnet_netif_get_default(void);
fnet_bool_t fnet_netif_is_connected(fnet_netif_desc_t);
fnet_return_t fnet_netif_get_statistics(
    fnet_netif_desc_t, struct fnet_netif_statistics *);
/* Both hw-address calls answer fnet_return_t (fnet_netif.h:442,472); the
 * earlier fake declared them void, so a failing read was inexpressible. */
fnet_return_t fnet_netif_get_hw_addr(
    fnet_netif_desc_t, fnet_uint8_t *, fnet_size_t);
fnet_return_t fnet_netif_set_hw_addr(
    fnet_netif_desc_t, fnet_uint8_t *, fnet_size_t);
fnet_ip4_addr_t fnet_netif_get_ip4_addr(fnet_netif_desc_t);
fnet_ip4_addr_t fnet_netif_get_ip4_subnet_mask(fnet_netif_desc_t);
fnet_ip4_addr_t fnet_netif_get_ip4_gateway(fnet_netif_desc_t);
fnet_ip4_addr_t fnet_netif_get_ip4_dns(fnet_netif_desc_t);
void fnet_netif_set_ip4_addr(
    fnet_netif_desc_t, fnet_ip4_addr_t, fnet_ip4_addr_t);
void fnet_netif_set_ip4_gateway(fnet_netif_desc_t, fnet_ip4_addr_t);
void fnet_netif_set_ip4_dns(fnet_netif_desc_t, fnet_ip4_addr_t);

/* --- service/link/fnet_link.h ------------------------------------------ */
fnet_link_desc_t fnet_link_init(fnet_link_params_t *);
void fnet_link_release(fnet_link_desc_t);

/* --- service/dhcp/fnet_dhcp_cln.h -------------------------------------- */
fnet_dhcp_cln_desc_t fnet_dhcp_cln_init(fnet_dhcp_cln_params_t *);
void fnet_dhcp_cln_release(fnet_dhcp_cln_desc_t);
fnet_bool_t fnet_dhcp_cln_is_enabled(fnet_dhcp_cln_desc_t);
void fnet_dhcp_cln_set_response_timeout(fnet_dhcp_cln_desc_t, fnet_time_t);
void fnet_dhcp_cln_set_callback_updated(
    fnet_dhcp_cln_desc_t, fnet_dhcp_cln_callback_t, void *);
fnet_dhcp_cln_desc_t fnet_dhcp_cln_get_by_netif(fnet_netif_desc_t);
void fnet_dhcp_cln_get_options(
    fnet_dhcp_cln_desc_t, struct fnet_dhcp_cln_options *);
/* fnet_dhcp_cln.h:236. The values come from the private enum in
 * service/dhcp/fnet_dhcp_cln.c (0 disabled, 1 init, 2 selecting, 3 requesting,
 * 4 probing, 5 bound, 6 renewing, 7 rebinding, 8 init-reboot, 9 rebooting).
 * The real getter dereferences its descriptor without a null check, which is
 * why the adapter looks the descriptor up first. */
fnet_uint8_t fnet_dhcp_cln_get_state(fnet_dhcp_cln_desc_t);

/* --- service/fnet_service.h -------------------------------------------- */
fnet_service_desc_t fnet_service_register(fnet_service_poll_t, void *);
void fnet_service_unregister(fnet_service_desc_t);

/* --- stack/fnet_socket.h ----------------------------------------------- */
fnet_socket_t fnet_socket(
    fnet_address_family_t, fnet_socket_type_t, fnet_uint32_t);
fnet_return_t fnet_socket_bind(
    fnet_socket_t, const struct fnet_sockaddr *, fnet_size_t);
fnet_return_t fnet_socket_listen(fnet_socket_t, fnet_size_t);
fnet_socket_t fnet_socket_accept(
    fnet_socket_t, struct fnet_sockaddr *, fnet_size_t *);
/* fnet_socket.h:943 -- `name` is not const in the real declaration. */
fnet_return_t fnet_socket_connect(
    fnet_socket_t, struct fnet_sockaddr *, fnet_size_t);
fnet_return_t fnet_socket_shutdown(fnet_socket_t, fnet_sd_flags_t);
/* fnet_socket.h:1230 -- fnet_return_t, not void. A void close cannot express a
 * failing one, and the only failure real FNET has here is a descriptor it does
 * not know (fnet_socket.c:767-771: FNET_ERR_BAD_DESC; both prot_detach
 * implementations, _fnet_tcp_close and _fnet_udp_detach, return FNET_OK on
 * every path). */
fnet_return_t fnet_socket_close(fnet_socket_t);
fnet_ssize_t fnet_socket_send(
    fnet_socket_t, const void *, fnet_size_t, fnet_flag_t);
fnet_ssize_t fnet_socket_recv(
    fnet_socket_t, void *, fnet_size_t, fnet_flag_t);
fnet_ssize_t fnet_socket_sendto(
    fnet_socket_t, const void *, fnet_size_t, fnet_flag_t,
    const struct fnet_sockaddr *, fnet_size_t);
fnet_ssize_t fnet_socket_recvfrom(
    fnet_socket_t, void *, fnet_size_t, fnet_flag_t, struct fnet_sockaddr *,
    fnet_size_t *);
/* fnet_socket.h:1433 -- fnet_size_t (a count of ready sockets), not
 * fnet_return_t. */
fnet_size_t fnet_socket_poll(fnet_socket_poll_t *, fnet_size_t);
fnet_return_t fnet_socket_setopt(
    fnet_socket_t, fnet_protocol_t, fnet_socket_options_t, const void *,
    fnet_size_t);
fnet_return_t fnet_socket_getopt(
    fnet_socket_t, fnet_protocol_t, fnet_socket_options_t, void *,
    fnet_size_t *);
fnet_return_t fnet_socket_getpeername(
    fnet_socket_t, struct fnet_sockaddr *, fnet_size_t *);

/* --- stack/fnet_error.h ------------------------------------------------- */
/* fnet_error.h:205,222. The error is one global word, and fnet_error.h:200-201
 * is explicit that neither a successful call nor a read of it clears the value
 * -- so a stale code is a state a caller can genuinely observe. Returning
 * int8_t, as the earlier fake did, hid that this is the enum above. */
fnet_error_t fnet_error_get(void);
void fnet_error_set(fnet_error_t);

/* --- stack/fnet_stdlib.h ------------------------------------------------ */
/* Both return void in the real header (:112,155), and fnet_memset's value
 * parameter is fnet_uint8_t rather than int. */
void fnet_memcpy(void *, const void *, fnet_size_t);
void fnet_memset(void *, fnet_uint8_t, fnet_size_t);

#endif
