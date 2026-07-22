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
#include "aius_dsp.h"
#include "aius_pipeline.h"
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

struct AiusEvent {
	uint32_t sequence;
	uint32_t uptime_ms;
	uint8_t type;
	uint8_t state;
	uint16_t detail;
};

static AiusEvent g_events[8];
static uint32_t g_event_sequence;
static uint8_t g_last_state;
static bool g_test_running;

static uint8_t current_state(void);

static void record_event(uint8_t type, uint8_t state, uint16_t detail)
{
	AiusEvent &event = g_events[g_event_sequence % 8u];
	event.sequence = ++g_event_sequence;
	event.uptime_ms = millis();
	event.type = type;
	event.state = state;
	event.detail = detail;
}

static void record_state(void)
{
	uint8_t state = current_state();
	if (state == g_last_state)
		return;
	g_last_state = state;
	record_event(1, state, 0);
}

class AiusAudioProcessor : public AudioStream
{
public:
	AiusAudioProcessor(void) : AudioStream(2, g_input_queue) {}

	void begin_pipeline(void)
	{
		(void)set_mode(AIUS_NET_MODE_FLAT);
		if (aius_arena_init(&g_arena, g_arena_memory, sizeof(g_arena_memory)) != 0) {
			g_create_status = AIUS_NO_SPACE;
			return;
		}
		AiusPcmSink sink = {this, consume_block};
		AiusPipelineConfig config = {AIUS_IEC61937_PACK_16, 0};
		g_create_status = aius_pipeline_create_arena(&g_pipeline, &config,
		                                             &sink, &g_arena);
	}

	AiusPipelineClass classification(void) const
	{
		return g_pipeline ? aius_pipeline_classification(g_pipeline)
		                  : AIUS_PIPELINE_UNKNOWN;
	}

	uint32_t block_count(void) const { return g_blocks; }
	uint32_t overrun_count(void) const { return g_overruns; }
	AiusStatus create_status(void) const { return g_create_status; }
	void clear_counters(void) { g_blocks = 0; g_overruns = 0; }
	uint8_t mode(void) const { return g_mode; }
	uint8_t mix(void) const { return g_mix; }
	bool set_mode(uint8_t mode)
	{
		if (mode > AIUS_NET_MODE_BASS_REDUCED)
			return false;
		noInterrupts();
		g_mode = mode;
		if (mode == AIUS_NET_MODE_FLAT) {
			g_eq[0] = aius_biquad_bypass();
			g_eq_count = 1;
		} else if (mode == AIUS_NET_MODE_SPEECH) {
			g_eq[0] = aius_biquad_highpass(48000.0, 120.0, 0.7071);
			g_eq[1] = aius_biquad_peaking(48000.0, 2500.0, 0.8, 3.0);
			g_eq_count = 2;
		} else {
			g_eq[0] = aius_biquad_highpass(48000.0, 80.0, 0.7071);
			g_eq_count = 1;
		}
		memset(g_eq_state, 0, sizeof(g_eq_state));
		interrupts();
		return true;
	}
	bool set_mix(uint8_t mix)
	{
		if (mix > AIUS_NET_MIX_CLIPFREE)
			return false;
		noInterrupts();
		g_mix = mix;
		g_downmix = aius_downmix_config_default();
		if (mix == AIUS_NET_MIX_LFE_MINUS10)
			g_downmix.lfe = AIUS_DSP_LFE_GAIN_MINUS10DB;
		else if (mix == AIUS_NET_MIX_CLIPFREE)
			g_downmix.headroom = aius_downmix_clipfree_headroom(&g_downmix);
		interrupts();
		return true;
	}

protected:
	void update(void) override
	{
		audio_block_t *left = receiveReadOnly(0);
		audio_block_t *right = receiveReadOnly(1);
		if (left != nullptr && right != nullptr && g_pipeline != nullptr) {
			uint16_t words[AUDIO_BLOCK_SAMPLES * 2];
			for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				words[2 * i] = (uint16_t)left->data[i];
				words[2 * i + 1] = (uint16_t)right->data[i];
			}
			AiusStatus st = aius_pipeline_push(g_pipeline, words,
			                                   AUDIO_BLOCK_SAMPLES * 2);
			if (st != AIUS_OK)
				g_overruns++;
		}
		if (left != nullptr)
			release(left);
		if (right != nullptr)
			release(right);

		emit_block();
	}

private:
	static constexpr uint32_t RING_FRAMES = 2048;
	static constexpr size_t ARENA_BYTES = 128u * 1024u;
	audio_block_t *g_input_queue[2] = {nullptr, nullptr};
	static uint8_t g_arena_memory[ARENA_BYTES] __attribute__((aligned(16)));
	AiusArena g_arena;
	AiusPipeline *g_pipeline = nullptr;
	AiusStatus g_create_status = AIUS_INVALID_ARG;
	int32_t g_left[RING_FRAMES];
	int32_t g_right[RING_FRAMES];
	uint32_t g_read = 0;
	uint32_t g_write = 0;
	uint32_t g_count = 0;
	uint32_t g_blocks = 0;
	uint32_t g_overruns = 0;
	uint8_t g_mode = AIUS_NET_MODE_FLAT;
	uint8_t g_mix = AIUS_NET_MIX_STANDARD;
	AiusDownmixConfig g_downmix = aius_downmix_config_default();
	AiusBiquad g_eq[2];
	AiusBiquadState g_eq_state[4] = {};
	size_t g_eq_count = 1;

	static AiusStatus consume_block(void *ctx, const AiusPcmBlock *block)
	{
		return static_cast<AiusAudioProcessor *>(ctx)->enqueue(block);
	}

	AiusStatus enqueue(const AiusPcmBlock *block)
	{
		if (block == nullptr || block->frames > AUDIO_BLOCK_SAMPLES * 4 ||
		    block->planes[0] == nullptr)
			return AIUS_INVALID_ARG;
		if (block->frames > RING_FRAMES - g_count)
			return AIUS_NO_SPACE;
		int32_t processed_left[AUDIO_BLOCK_SAMPLES * 4];
		int32_t processed_right[AUDIO_BLOCK_SAMPLES * 4];
		AiusPcmBlock processed = {};
		processed.planes[0] = processed_left;
		processed.planes[1] = processed_right;
		AiusDspConfig dsp = {};
		dsp.downmix = g_downmix;
		dsp.eq = g_eq;
		dsp.eq_sections = g_eq_count;
		dsp.eq_state = g_eq_state;
		AiusStatus dsp_status = aius_dsp_process(&dsp, block, &processed);
		if (dsp_status != AIUS_OK)
			return dsp_status;
		for (uint32_t i = 0; i < block->frames; i++) {
			int32_t l = processed_left[i] >> 8;
			int32_t r = processed_right[i] >> 8;
			if (l > 32767) l = 32767;
			if (l < -32768) l = -32768;
			if (r > 32767) r = 32767;
			if (r < -32768) r = -32768;
			g_left[g_write] = l;
			g_right[g_write] = r;
			g_write = (g_write + 1) % RING_FRAMES;
		}
		g_count += block->frames;
		g_blocks++;
		return AIUS_OK;
	}

	void emit_block(void)
	{
		audio_block_t *left = allocate();
		audio_block_t *right = allocate();
		if (left == nullptr || right == nullptr) {
			if (left != nullptr) release(left);
			if (right != nullptr) release(right);
			g_overruns++;
			return;
		}
		for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			if (g_count != 0) {
				left->data[i] = (int16_t)g_left[g_read];
				right->data[i] = (int16_t)g_right[g_read];
				g_read = (g_read + 1) % RING_FRAMES;
				g_count--;
			} else {
				left->data[i] = 0;
				right->data[i] = 0;
			}
		}
		transmit(left, 0);
		transmit(right, 1);
		release(left);
		release(right);
	}
};

uint8_t AiusAudioProcessor::g_arena_memory[AiusAudioProcessor::ARENA_BYTES]
	__attribute__((aligned(16)));

static AiusAudioProcessor g_audio_processor;
static AudioOutputSPDIF3 g_spdif_out;
static AudioConnection g_spdif_left(g_spdif_in, 0, g_audio_processor, 0);
static AudioConnection g_spdif_right(g_spdif_in, 1, g_audio_processor, 1);
static AudioConnection g_audio_left(g_audio_processor, 0, g_spdif_out, 0);
static AudioConnection g_audio_right(g_audio_processor, 1, g_spdif_out, 1);

static uint8_t current_state(void)
{
	AiusPipelineClass klass = g_audio_processor.classification();
	return !AudioInputSPDIF3::pllLocked() ? 2 :
	       klass == AIUS_PIPELINE_DTS ? 4 :
	       klass == AIUS_PIPELINE_PCM ? 3 : 2;
}

static void put_status(uint8_t *payload, uint16_t *length)
{
	/* Versioned, explicitly packed status payload:
	 * 0 version, 1 network mode (1 DHCP/2 fallback), 2 state, 3 flags,
	 * 4 boot epoch, 8 uptime ms, 12 IPv4, 16 MAC, 22 NTP-valid flag. */
	record_state();
	memset(payload, 0, 39);
	payload[0] = 1;
	payload[1] = g_dhcp ? 1 : 2;
	AiusPipelineClass klass = g_audio_processor.classification();
	payload[2] = g_last_state;
	if (AudioInputSPDIF3::pllLocked())
		payload[3] |= 1u;
	aius_net_put_u32(payload + 4, g_boot_epoch);
	aius_net_put_u32(payload + 8, millis());
	IPAddress ip = Ethernet.localIP();
	for (unsigned i = 0; i < 4; i++)
		payload[12 + i] = ip[i];
	memcpy(payload + 16, g_mac, sizeof(g_mac));
	payload[22] = 0; /* NTP validity: not yet implemented by NativeEthernet adapter */
	if (g_test_running)
		payload[3] |= 2u;
	payload[23] = (uint8_t)klass;
	payload[24] = (uint8_t)g_audio_processor.create_status();
	aius_net_put_u32(payload + 25, AudioInputSPDIF3::sampleRate());
	aius_net_put_u32(payload + 29, g_audio_processor.block_count());
	aius_net_put_u32(payload + 33, g_audio_processor.overrun_count());
	payload[37] = g_audio_processor.mode();
	payload[38] = g_audio_processor.mix();
	*length = 39;
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
		g_audio_processor.clear_counters();
		send_response(AIUS_NET_RSP_ACK, request.request_id, nullptr, 0);
		break;
	case AIUS_NET_CMD_SET_MODE:
		if (request.payload_len != 1 || !g_audio_processor.set_mode(request.payload[0])) {
			send_nack(request.request_id, AIUS_NET_ERR_PAYLOAD);
			break;
		}
		send_response(AIUS_NET_RSP_ACK, request.request_id, nullptr, 0);
		break;
	case AIUS_NET_CMD_SET_MIX:
		if (request.payload_len != 1 || !g_audio_processor.set_mix(request.payload[0])) {
			send_nack(request.request_id, AIUS_NET_ERR_PAYLOAD);
			break;
		}
		send_response(AIUS_NET_RSP_ACK, request.request_id, nullptr, 0);
		break;
	case AIUS_NET_CMD_START_TEST:
		if (!g_test_running) {
			g_test_running = true;
			record_event(2, current_state(), 1);
		}
		send_response(AIUS_NET_RSP_ACK, request.request_id, nullptr, 0);
		break;
	case AIUS_NET_CMD_STOP_TEST:
		if (g_test_running) {
			g_test_running = false;
			record_event(2, current_state(), 0);
		}
		send_response(AIUS_NET_RSP_ACK, request.request_id, nullptr, 0);
		break;
	case AIUS_NET_CMD_REBOOT:
		send_response(AIUS_NET_RSP_ACK, request.request_id, nullptr, 0);
		delay(5);
		_reboot_Teensyduino_();
		break;
	case AIUS_NET_CMD_GET_EVENTS: {
		uint32_t cursor = request.payload_len >= 4 ?
			aius_net_get_u32(request.payload) : 0;
		uint8_t count = 0;
		for (uint32_t i = 0; i < 8 && count < 4; i++) {
			const AiusEvent &event = g_events[i];
			if (event.sequence <= cursor || event.sequence == 0)
				continue;
			uint8_t *dst = payload + 1 + count * 12;
			aius_net_put_u32(dst, event.sequence);
			aius_net_put_u32(dst + 4, event.uptime_ms);
			dst[8] = event.type;
			dst[9] = event.state;
			aius_net_put_u16(dst + 10, event.detail);
			count++;
		}
		payload[0] = count;
		length = (uint16_t)(1 + count * 12);
		send_response(AIUS_NET_RSP_EVENTS, request.request_id, payload, length);
		break;
	}
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
	g_audio_processor.begin_pipeline();
	g_boot_epoch = micros();
	start_network();
}

void loop(void)
{
	service_network();
	if (g_dhcp)
		(void)Ethernet.maintain();
}
