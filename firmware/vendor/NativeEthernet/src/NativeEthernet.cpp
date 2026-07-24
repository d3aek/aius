/* Copyright 2018 Paul Stoffregen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <Arduino.h>
#include <fnet.h>
#include "NativeEthernet.h"
#include "utility/NativeW5100.h"

IPAddress EthernetClass::_dnsServerAddress;
uint8_t* EthernetClass::stack_heap_ptr = NULL;
size_t EthernetClass::stack_heap_size = 0;
ssize_t EthernetClass::socket_size = 0;
uint8_t EthernetClass::socket_num = 0;
IntervalTimer EthernetClass::_fnet_poll;
volatile boolean EthernetClass::link_status = 0;
bool EthernetClass::storage_initialized = false;
bool EthernetClass::fnet_initialized = false;
bool EthernetClass::stack_initialized = false;
fnet_link_desc_t EthernetClass::link_descriptor = 0;
fnet_dhcp_cln_desc_t EthernetClass::dhcp_descriptor = nullptr;
EthernetServer *EthernetClass::active_servers[MAX_SOCK_NUM] = {};
EthernetClass::LinkCallback EthernetClass::user_link_callback = nullptr;
void *EthernetClass::user_link_context = nullptr;
EthernetClass::DhcpCallback EthernetClass::user_dhcp_callback = nullptr;
void *EthernetClass::user_dhcp_context = nullptr;
DMAMEM uint8_t** EthernetClass::socket_buf_receive;
DMAMEM uint16_t* EthernetClass::socket_buf_index;
DMAMEM uint8_t** EthernetClass::socket_buf_transmit;
DMAMEM uint16_t* EthernetClass::socket_buf_len;
DMAMEM uint16_t* EthernetClass::socket_port;
DMAMEM uint8_t** EthernetClass::socket_addr;
volatile fnet_socket_t* EthernetClass::socket_ptr;

void EthernetClass::setStackHeap(uint8_t* _stack_heap_ptr, size_t _stack_heap_size){
    if(stack_heap_ptr != NULL) return;
    stack_heap_ptr = _stack_heap_ptr;
    stack_heap_size = _stack_heap_size;
}

void EthernetClass::setStackHeap(size_t _stack_heap_size){
    if(stack_heap_ptr != NULL) return;
    stack_heap_size = _stack_heap_size;
}

void EthernetClass::setSocketSize(size_t _socket_size){
    if(socket_size != 0) return;
    socket_size = _socket_size;
}

void EthernetClass::setSocketNum(uint8_t _socket_num){
    if(socket_num != 0) return;
    if(_socket_num > FNET_CFG_SOCKET_MAX) socket_num = FNET_CFG_SOCKET_MAX;
    else socket_num = _socket_num;
}

int EthernetClass::begin(uint8_t *mac, unsigned long timeout, unsigned long responseTimeout)
{
    (void)timeout;
    return beginNoWait(mac) && dhcpStart(responseTimeout);
}

bool EthernetClass::beginNoWait(uint8_t *mac)
{
    if (stack_initialized)
        return true;
    if (mac == nullptr)
        return false;

    if (!storage_initialized) {
        if (stack_heap_size == 0)
            stack_heap_size = FNET_STACK_HEAP_DEFAULT_SIZE;
        if (stack_heap_ptr == nullptr)
            stack_heap_ptr = new uint8_t[stack_heap_size];
        if (socket_size == 0)
            socket_size = FNET_SOCKET_DEFAULT_SIZE;
        if (socket_num == 0)
            socket_num = MAX_SOCK_NUM;

        socket_buf_transmit = new uint8_t *[socket_num];
        socket_buf_receive = new uint8_t *[socket_num];
        socket_buf_len = new uint16_t[socket_num];
        socket_port = new uint16_t[socket_num];
        socket_addr = new uint8_t *[socket_num];
        socket_ptr = new fnet_socket_t[socket_num];
        socket_buf_index = new uint16_t[socket_num];
        EthernetServer::server_port = new uint16_t[socket_num];
#if FNET_CFG_TLS
        EthernetServer::tls_socket_ptr = new fnet_tls_socket_t[socket_num];
        EthernetServer::_tls = new bool[socket_num];
#endif

        for (uint8_t i = 0; i < socket_num; i++) {
            socket_buf_transmit[i] = new uint8_t[socket_size];
            socket_buf_receive[i] = new uint8_t[socket_size];
            socket_addr[i] = new uint8_t[4];
            socket_ptr[i] = nullptr;
            socket_buf_len[i] = 0;
            socket_buf_index[i] = 0;
            socket_port[i] = 0;
            EthernetServer::server_port[i] = 0;
#if FNET_CFG_TLS
            EthernetServer::_tls[i] = false;
            EthernetServer::tls_socket_ptr[i] = nullptr;
#endif
        }

        storage_initialized = true;
    }

    if (!fnet_initialized &&
        fnet_netif_is_initialized(fnet_netif_get_default()))
        fnet_initialized = true;
    if (!fnet_initialized) {
        struct fnet_init_params init_params;
        init_params.netheap_ptr = stack_heap_ptr;
        init_params.netheap_size = stack_heap_size;

        static const fnet_mutex_api_t teensy_mutex_api = {
            .mutex_init = teensy_mutex_init,
            .mutex_release = teensy_mutex_release,
            .mutex_lock = teensy_mutex_lock,
            .mutex_unlock = teensy_mutex_unlock,
        };
        static const fnet_timer_api_t timer_api = {
          .timer_get_ms = timer_get_ms,
          .timer_delay = 0,
        };
        init_params.mutex_api = &teensy_mutex_api;
        init_params.timer_api = &timer_api;
        if (fnet_init(&init_params) == FNET_ERR)
            return false;
        fnet_initialized = true;
    }
    if (!fnet_netif_is_initialized(fnet_netif_get_default())) {
        if (fnet_netif_init(FNET_CPU_ETH0_IF, mac, 6) == FNET_ERR)
            return false;
    }

    fnet_netif_desc_t netif = fnet_netif_get_default();
    if (netif == nullptr)
        return false;
    fnet_link_params_t link_params = {};
    link_params.netif_desc = netif;
    link_params.callback = link_callback;
    link_params.callback_param = nullptr;
    link_descriptor = fnet_link_init(&link_params);
    if (link_descriptor == 0)
        return false;
    link_status = fnet_netif_is_connected(netif);
    _fnet_poll.begin(fnet_poll, FNET_POLL_TIME);
    stack_initialized = true;
    return true;
}

bool EthernetClass::dhcpStart(unsigned long responseTimeout)
{
    if (!stack_initialized)
        return false;
    if (dhcp_descriptor != nullptr &&
        fnet_dhcp_cln_is_enabled(dhcp_descriptor))
        return true;
    if (dhcp_descriptor != nullptr) {
        fnet_dhcp_cln_release(dhcp_descriptor);
        dhcp_descriptor = nullptr;
    }

    fnet_dhcp_cln_params_t params = {};
    params.netif = fnet_netif_get_default();
    dhcp_descriptor = fnet_dhcp_cln_init(&params);
    if (dhcp_descriptor == nullptr)
        return false;
    fnet_dhcp_cln_set_response_timeout(dhcp_descriptor, responseTimeout);
    fnet_dhcp_cln_set_callback_updated(
        dhcp_descriptor, dhcp_cln_callback_updated, nullptr);
    return true;
}

void EthernetClass::dhcpStop()
{
    if (dhcp_descriptor == nullptr)
        return;
    fnet_dhcp_cln_release(dhcp_descriptor);
    dhcp_descriptor = nullptr;
}

void EthernetClass::end()
{
    user_link_callback = nullptr;
    user_link_context = nullptr;
    user_dhcp_callback = nullptr;
    user_dhcp_context = nullptr;
    stack_initialized = false;
    link_status = false;

    EthernetServer *servers[MAX_SOCK_NUM] = {};
    for (uint8_t i = 0; i < MAX_SOCK_NUM; i++) {
        servers[i] = active_servers[i];
    }
    for (uint8_t i = MAX_SOCK_NUM; i > 0; i--) {
        if (servers[i - 1] != nullptr)
            servers[i - 1]->invalidateService();
    }
    if (link_descriptor != 0) {
        fnet_link_release(link_descriptor);
        link_descriptor = 0;
    }
    dhcpStop();

    for (uint8_t i = MAX_SOCK_NUM; i > 0; i--) {
        if (servers[i - 1] != nullptr)
            servers[i - 1]->closeListener();
    }
    if (!storage_initialized)
        return;
    for (uint8_t i = 0; i < socket_num; i++) {
        if (socket_ptr[i] != nullptr)
            socketClose(i);
        EthernetServer::server_port[i] = 0;
    }
}

uint8_t EthernetClass::registerServer(EthernetServer *server)
{
    if (server == nullptr)
        return MAX_SOCK_NUM;
    for (uint8_t i = 0; i < MAX_SOCK_NUM; i++) {
        if (active_servers[i] == nullptr) {
            active_servers[i] = server;
            return i;
        }
    }
    return MAX_SOCK_NUM;
}

void EthernetClass::unregisterServer(EthernetServer *server, uint8_t slot)
{
    if (slot < MAX_SOCK_NUM && active_servers[slot] == server)
        active_servers[slot] = nullptr;
}

EthernetServer *EthernetClass::serverAt(uint8_t slot)
{
    if (slot >= MAX_SOCK_NUM)
        return nullptr;
    return active_servers[slot];
}

void EthernetClass::setLinkCallback(LinkCallback callback, void *context)
{
    user_link_callback = callback;
    user_link_context = context;
}

void EthernetClass::setDhcpCallback(DhcpCallback callback, void *context)
{
    user_dhcp_callback = callback;
    user_dhcp_context = context;
}

void EthernetClass::begin(uint8_t *mac, IPAddress ip)
{
	// Assume the DNS server will be the machine on the same network as the local IP
	// but with last octet being '1'
	IPAddress dns = ip;
	dns[3] = 1;
	begin(mac, ip, dns);
}

void EthernetClass::begin(uint8_t *mac, IPAddress ip, IPAddress dns)
{
	// Assume the gateway will be the machine on the same network as the local IP
	// but with last octet being '1'
	IPAddress gateway = ip;
	gateway[3] = 1;
	begin(mac, ip, dns, gateway);
}

void EthernetClass::begin(uint8_t *mac, IPAddress ip, IPAddress dns, IPAddress gateway)
{
	IPAddress subnet(255, 255, 255, 0);
	begin(mac, ip, dns, gateway, subnet);
}

void EthernetClass::begin(uint8_t *mac, IPAddress ip, IPAddress dns, IPAddress gateway, IPAddress subnet)
{
    if (!beginNoWait(mac))
        return;
    dhcpStop();
    fnet_netif_set_ip4_addr(fnet_netif_get_default(), ip, subnet);
    fnet_netif_set_ip4_gateway(fnet_netif_get_default(), gateway);
    fnet_netif_set_ip4_dns(fnet_netif_get_default(), dns);
}

void EthernetClass::init(uint8_t sspin)
{
	
}

EthernetLinkStatus EthernetClass::linkStatus()
{
	switch ((uint8_t)link_status) {
		case 0:  return LinkOFF;
		case 1: return LinkON;
		default:       return Unknown;
	}
}

EthernetHardwareStatus EthernetClass::hardwareStatus()
{
    return EthernetW5500;
}

int EthernetClass::maintain()
{
	return 0; //DHCP already maintained
}


void EthernetClass::MACAddress(uint8_t *mac_address)
{
    fnet_netif_get_hw_addr(fnet_netif_get_default(), mac_address, 6);
}

IPAddress EthernetClass::localIP()
{
	return IPAddress(fnet_netif_get_ip4_addr(fnet_netif_get_default()));
}

IPAddress EthernetClass::subnetMask()
{
	return IPAddress(fnet_netif_get_ip4_subnet_mask(fnet_netif_get_default()));
}

IPAddress EthernetClass::gatewayIP()
{
	return IPAddress(fnet_netif_get_ip4_gateway(fnet_netif_get_default()));
}

IPAddress EthernetClass::dhcpServerIP()
{
    struct fnet_dhcp_cln_options current_options;
    fnet_dhcp_cln_get_options(fnet_dhcp_cln_get_by_netif(fnet_netif_get_default()), &current_options);
    
    return IPAddress(current_options.dhcp_server.s_addr);
}

void EthernetClass::setMACAddress(const uint8_t *mac_address)
{
	fnet_netif_set_hw_addr(fnet_netif_get_default(), (fnet_uint8_t*)mac_address, 6);
}

void EthernetClass::setLocalIP(const IPAddress local_ip)
{
    fnet_netif_set_ip4_addr(fnet_netif_get_default(), *const_cast<IPAddress*>(&local_ip), subnetMask());
}

void EthernetClass::setSubnetMask(const IPAddress subnet)
{
	fnet_netif_set_ip4_addr(fnet_netif_get_default(), localIP(), *const_cast<IPAddress*>(&subnet));
}

void EthernetClass::setGatewayIP(const IPAddress gateway)
{
	fnet_netif_set_ip4_gateway(fnet_netif_get_default(), *const_cast<IPAddress*>(&gateway));
}

void EthernetClass::setDnsServerIP(const IPAddress dns_server)
{
    fnet_netif_set_ip4_dns(fnet_netif_get_default(), *const_cast<IPAddress*>(&dns_server));
}

void EthernetClass::setRetransmissionTimeout(uint16_t milliseconds)
{
	//Not needed, probably
}

void EthernetClass::setRetransmissionCount(uint8_t num)
{
	//Not needed, probably
}



fnet_return_t EthernetClass::teensy_mutex_init(fnet_mutex_t *mutex) {
  return FNET_OK;
}

void EthernetClass::teensy_mutex_release(fnet_mutex_t *mutex) {
}

void EthernetClass::teensy_mutex_lock(fnet_mutex_t *mutex) {
}

void EthernetClass::teensy_mutex_unlock(fnet_mutex_t *mutex) {
}

fnet_time_t EthernetClass::timer_get_ms(void){ //Used for multi-thread version
    fnet_time_t result;
    result =  millis();
    return result;
}

void EthernetClass::link_callback(fnet_netif_desc_t netif, fnet_bool_t connected, void *callback_param){
  (void)netif;
  (void)callback_param;
  if (!stack_initialized)
      return;
  link_status = connected;
  if (user_link_callback != nullptr)
      user_link_callback(connected, user_link_context);
}

void EthernetClass::dhcp_cln_callback_updated(fnet_dhcp_cln_desc_t _dhcp_desc, fnet_netif_desc_t netif, void *p) { //Called when DHCP updates
  (void)_dhcp_desc;
  (void)netif;
  (void)p;
  if (user_dhcp_callback != nullptr)
      user_dhcp_callback(user_dhcp_context);
}




EthernetClass Ethernet;
