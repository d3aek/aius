/* Aius optical prototype bring-up target.
 *
 * This target keeps the Ethernet service bounded and independent from USB. The
 * first bring-up build exposes discovery/status and the safe control boundary;
 * the S/PDIF runtime is added behind the same status snapshot in the next slice.
 */

#include <Arduino.h>
#include <AudioStream.h>
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>
#include <input_spdif3.h>
#include <output_spdif3.h>

extern "C" {
#include "aius_net.h"
}

static constexpr uint16_t AIUS_NET_PORT = 4242;
static constexpr uint32_t AIUS_STATIC_IP[4] = {192, 168, 42, 42};
static constexpr uint32_t AIUS_STATIC_MASK[4] = {255, 255, 255, 0};

static byte g_mac[] = {0x02, 0x41, 0x49, 0x55, 0x01, 0x01};
static EthernetUDP g_udp;
static IPAddress g_static_ip(AIUS_STATIC_IP[0], AIUS_STATIC_IP[1],
                             AIUS_STATIC_IP[2], AIUS_STATIC_IP[3]);
static IPAddress g_static_mask(AIUS_STATIC_MASK[0], AIUS_STATIC_MASK[1],
                               AIUS_STATIC_MASK[2], AIUS_STATIC_MASK[3]);
static bool g_dhcp;
static uint32_t g_boot_epoch;
static AudioInputSPDIF3 g_spdif_in;
static AudioOutputSPDIF3 g_spdif_out;
static AudioConnection g_spdif_left(g_spdif_in, 0, g_spdif_out, 0);
static AudioConnection g_spdif_right(g_spdif_in, 1, g_spdif_out, 1);

static void put_status(uint8_t *payload, uint16_t *length)
{
	/* Versioned, explicitly packed status payload:
	 * 0 version, 1 network mode (1 DHCP/2 fallback), 2 state, 3 flags,
	 * 4 boot epoch, 8 uptime ms, 12 IPv4, 16 MAC, 22 NTP-valid flag. */
	memset(payload, 0, 23);
	payload[0] = 1;
	payload[1] = g_dhcp ? 1 : 2;
	payload[2] = AudioInputSPDIF3::pllLocked() ? 2 : 1;
	if (AudioInputSPDIF3::pllLocked())
		payload[3] |= 1u;
	aius_net_put_u32(payload + 4, g_boot_epoch);
	aius_net_put_u32(payload + 8, millis());
	IPAddress ip = Ethernet.localIP();
	for (unsigned i = 0; i < 4; i++)
		payload[12 + i] = ip[i];
	memcpy(payload + 16, g_mac, sizeof(g_mac));
	payload[22] = 0; /* NTP validity: not yet implemented by NativeEthernet adapter */
	*length = 23;
}

static void send_response(uint8_t type, uint32_t request_id,
                          const uint8_t *payload, uint16_t length)
{
	uint8_t frame[AIUS_NET_MAX_FRAME];
	size_t frame_len = aius_net_encode(frame, sizeof(frame), type, 0,
	                                   request_id, payload, length);
	if (frame_len == 0)
		return;
	g_udp.beginPacket(g_udp.remoteIP(), g_udp.remotePort());
	g_udp.write(frame, frame_len);
	g_udp.endPacket();
}

static void send_nack(uint32_t request_id, uint8_t error)
{
	uint8_t payload[1] = {error};
	send_response(AIUS_NET_RSP_NACK, request_id, payload, sizeof(payload));
}

static void service_network(void)
{
	int packet_size = g_udp.parsePacket();
	if (packet_size <= 0)
		return;
	uint8_t packet[AIUS_NET_MAX_FRAME];
	if (packet_size > (int)sizeof(packet)) {
		while (g_udp.available())
			(void)g_udp.read();
		return;
	}
	int got = g_udp.read(packet, sizeof(packet));
	if (got != packet_size)
		return;
	AiusNetFrame request;
	if (aius_net_decode(packet, (size_t)got, &request) != AIUS_NET_OK)
		return;
	uint8_t payload[AIUS_NET_MAX_PAYLOAD];
	uint16_t length = 0;
	switch (request.type) {
	case AIUS_NET_CMD_DISCOVER:
		put_status(payload, &length);
		send_response(AIUS_NET_RSP_DISCOVERY, request.request_id,
		              payload, length);
		break;
	case AIUS_NET_CMD_GET_STATUS:
		put_status(payload, &length);
		send_response(AIUS_NET_RSP_STATUS, request.request_id,
		              payload, length);
		break;
	case AIUS_NET_CMD_CLEAR_COUNTERS:
	case AIUS_NET_CMD_START_TEST:
	case AIUS_NET_CMD_STOP_TEST:
		/* Accepted as idempotent bring-up commands; runtime counters are added
		 * before these commands are enabled for physical acceptance. */
		send_response(AIUS_NET_RSP_ACK, request.request_id, nullptr, 0);
		break;
	default:
		send_nack(request.request_id, AIUS_NET_ERR_TYPE);
		break;
	}
}

static void start_network(void)
{
	/* NativeEthernet provides a bounded DHCP attempt. Static mode is used only
	 * after DHCP fails, keeping the fallback usable on a direct cable. */
	g_dhcp = Ethernet.begin(g_mac, 5000, 250) != 0;
	if (!g_dhcp)
		Ethernet.begin(g_mac, g_static_ip, IPAddress(0, 0, 0, 0),
		              IPAddress(0, 0, 0, 0), g_static_mask);
	g_udp.begin(AIUS_NET_PORT);
}

void setup(void)
{
	Serial.begin(115200);
	AudioMemory(24);
	g_boot_epoch = micros();
	start_network();
}

void loop(void)
{
	service_network();
	if (g_dhcp)
		(void)Ethernet.maintain();
}
