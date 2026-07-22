/* Aius optical prototype bring-up target.
 *
 * This target keeps the Ethernet service bounded and independent from USB. The
 * first bring-up build exposes discovery/status and the safe control boundary;
 * the S/PDIF runtime is added behind the same status snapshot in the next slice.
 */

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <AudioStream.h>
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>
#include <input_spdif3.h>
#include <output_spdif3.h>

extern "C" {
#include "aius_net.h"
	#include "aius_dsp.h"
	#include "aius_pipeline.h"
	#include "aius_http.h"
}

static constexpr uint16_t AIUS_NET_PORT = 4242;
static constexpr uint32_t AIUS_STATIC_IP[4] = {192, 168, 42, 42};
static constexpr uint32_t AIUS_STATIC_MASK[4] = {255, 255, 255, 0};

static byte g_mac[] = {0x02, 0x41, 0x49, 0x55, 0x01, 0x01};
static EthernetUDP g_udp;
static EthernetServer g_http_server(80);
static EthernetClient g_http_client;
static uint8_t g_http_request[AIUS_HTTP_MAX_REQUEST];
static uint8_t g_http_response[AIUS_HTTP_MAX_RESPONSE];
static size_t g_http_request_length;
static uint32_t g_http_last_byte_ms;
static uint32_t g_http_requests;
static uint32_t g_http_rejections;
static uint32_t g_http_client_drops;
static uint32_t g_http_service_overruns;
static IPAddress g_static_ip(AIUS_STATIC_IP[0], AIUS_STATIC_IP[1],
                             AIUS_STATIC_IP[2], AIUS_STATIC_IP[3]);
static IPAddress g_static_mask(AIUS_STATIC_MASK[0], AIUS_STATIC_MASK[1],
                               AIUS_STATIC_MASK[2], AIUS_STATIC_MASK[3]);
static bool g_dhcp;
static bool g_network_ready;
static uint32_t g_network_retry_at;
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
static uint32_t g_event_overflows;
static uint8_t g_last_state;
static bool g_test_running;
static volatile uint32_t g_network_drops;
static volatile uint32_t g_network_budget_overruns;

static uint8_t current_state(void);

static void record_event(uint8_t type, uint8_t state, uint16_t detail)
{
	AiusEvent &event = g_events[g_event_sequence % 8u];
	if (g_event_sequence >= 8u)
		g_event_overflows++;
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

	uint32_t block_count(void) const
	{
		noInterrupts();
		uint32_t value = g_blocks;
		interrupts();
		return value;
	}
	uint32_t overrun_count(void) const
	{
		noInterrupts();
		uint32_t value = g_overruns;
		interrupts();
		return value;
	}
	AiusStatus create_status(void) const { return g_create_status; }
	void clear_counters(void)
	{
		noInterrupts();
		g_blocks = 0;
		g_overruns = 0;
		interrupts();
	}
	uint8_t mode(void) const { return g_mode; }
	uint8_t mix(void) const { return g_mix; }
	bool set_mode(uint8_t mode)
	{
		if (mode > AIUS_NET_MODE_BASS_REDUCED)
			return false;
		noInterrupts();
		g_pending_mode = mode;
		g_mode_pending = true;
		interrupts();
		return true;
	}
	bool set_mix(uint8_t mix)
	{
		if (mix > AIUS_NET_MIX_CLIPFREE)
			return false;
		noInterrupts();
		g_pending_mix = mix;
		g_mix_pending = true;
		interrupts();
		return true;
	}

protected:
	void update(void) override
	{
		apply_pending_controls();
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
	volatile uint8_t g_pending_mode = AIUS_NET_MODE_FLAT;
	volatile uint8_t g_pending_mix = AIUS_NET_MIX_STANDARD;
	volatile bool g_mode_pending = false;
	volatile bool g_mix_pending = false;
	AiusDownmixConfig g_downmix = aius_downmix_config_default();
	AiusBiquad g_eq[2];
	AiusBiquadState g_eq_state[4] = {};
	size_t g_eq_count = 1;

	static AiusStatus consume_block(void *ctx, const AiusPcmBlock *block)
	{
		return static_cast<AiusAudioProcessor *>(ctx)->enqueue(block);
	}

	void apply_pending_controls(void)
	{
		if (g_mode_pending) {
			uint8_t mode = g_pending_mode;
			g_mode_pending = false;
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
		}
		if (g_mix_pending) {
			uint8_t mix = g_pending_mix;
			g_mix_pending = false;
			g_mix = mix;
			g_downmix = aius_downmix_config_default();
			if (mix == AIUS_NET_MIX_LFE_MINUS10)
				g_downmix.lfe = AIUS_DSP_LFE_GAIN_MINUS10DB;
			else if (mix == AIUS_NET_MIX_CLIPFREE)
				g_downmix.headroom = aius_downmix_clipfree_headroom(&g_downmix);
		}
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
	/* Versioned, explicitly packed status payload. The original 39 bytes remain
	 * stable; diagnostic counters append at 39: event sequence, event overflow,
	 * network drops, and network budget overruns. */
	record_state();
	noInterrupts();
	uint32_t event_sequence = g_event_sequence;
	uint32_t event_overflows = g_event_overflows;
	uint32_t network_drops = g_network_drops;
	uint32_t network_budget_overruns = g_network_budget_overruns;
	uint32_t http_requests = g_http_requests;
	uint32_t http_rejections = g_http_rejections;
	uint32_t http_client_drops = g_http_client_drops;
	uint32_t http_service_overruns = g_http_service_overruns;
	interrupts();
	memset(payload, 0, 71);
	payload[0] = 1;
	payload[1] = !g_network_ready ? 0 : (g_dhcp ? 1 : 2);
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
	aius_net_put_u32(payload + 39, event_sequence);
	aius_net_put_u32(payload + 43, event_overflows);
	aius_net_put_u32(payload + 47, network_drops);
	aius_net_put_u32(payload + 51, network_budget_overruns);
	aius_net_put_u32(payload + 55, http_requests);
	aius_net_put_u32(payload + 59, http_rejections);
	aius_net_put_u32(payload + 63, http_client_drops);
	aius_net_put_u32(payload + 67, http_service_overruns);
	*length = 71;
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

static int http_header_end(size_t *body_offset)
{
	for (size_t i = 3; i < g_http_request_length; i++) {
		if (g_http_request[i - 3] == '\r' && g_http_request[i - 2] == '\n' &&
		    g_http_request[i - 1] == '\r' && g_http_request[i] == '\n') {
			*body_offset = i + 1;
			return 1;
		}
	}
	return 0;
}

static int http_expected_body(size_t header_end, size_t *expected)
{
	*expected = 0;
	for (size_t i = 0; i + 16 < header_end; i++) {
		if (memcmp(g_http_request + i, "Content-Length: ", 16) != 0)
			continue;
		i += 16;
		uint32_t value = 0;
		while (i < header_end && g_http_request[i] >= '0' && g_http_request[i] <= '9') {
			value = value * 10u + (uint32_t)(g_http_request[i] - '0');
			if (value > AIUS_HTTP_MAX_BODY)
				return -1;
			i++;
		}
		*expected = value;
		return 1;
	}
	return 1;
}

static unsigned http_error_status(AiusHttpResult result)
{
	switch (result) {
	case AIUS_HTTP_ERR_TOO_LARGE: return 413;
	case AIUS_HTTP_ERR_TIMEOUT: return 408;
	case AIUS_HTTP_ERR_METHOD: return 405;
	case AIUS_HTTP_ERR_ROUTE: return 404;
	default: return 400;
	}
}

static size_t http_status_json(uint8_t *body, size_t capacity)
{
	uint8_t status[71];
	uint16_t status_length = 0;
	put_status(status, &status_length);
	if (capacity == 0 || status_length < 71)
		return 0;
	int written = snprintf((char *)body, capacity,
		"{\"version\":%u,\"state\":%u,\"carrier\":%u,\"format\":%u,\"sample_rate\":%lu,\"blocks\":%lu,\"overruns\":%lu,\"mode\":%u,\"mix\":%u,\"event_sequence\":%lu,\"event_overflows\":%lu,\"network_drops\":%lu,\"network_budget_overruns\":%lu,\"http_requests\":%lu,\"http_rejections\":%lu,\"http_client_drops\":%lu,\"http_service_overruns\":%lu}",
		status[0], status[2], status[3] & 1u, status[23],
		(unsigned long)aius_net_get_u32(status + 25),
		(unsigned long)aius_net_get_u32(status + 29),
		(unsigned long)aius_net_get_u32(status + 33), status[37], status[38],
		(unsigned long)aius_net_get_u32(status + 39),
		(unsigned long)aius_net_get_u32(status + 43),
		(unsigned long)aius_net_get_u32(status + 47),
		(unsigned long)aius_net_get_u32(status + 51),
		(unsigned long)aius_net_get_u32(status + 55),
		(unsigned long)aius_net_get_u32(status + 59),
		(unsigned long)aius_net_get_u32(status + 63),
		(unsigned long)aius_net_get_u32(status + 67));
	return written > 0 && (size_t)written < capacity ? (size_t)written : 0;
}

static size_t http_events_json(uint32_t cursor, uint8_t *body, size_t capacity)
{
	int written = snprintf((char *)body, capacity, "{\"cursor\":%lu,\"next\":%lu,\"events\":[",
		(unsigned long)cursor, (unsigned long)g_event_sequence);
	if (written < 0 || (size_t)written >= capacity)
		return 0;
	size_t length = (size_t)written;
	uint8_t count = 0;
	uint32_t first = g_event_sequence > 8u ? g_event_sequence - 7u : 1u;
	for (uint32_t sequence = first; sequence <= g_event_sequence && count < 8; sequence++) {
		const AiusEvent &event = g_events[(sequence - 1u) % 8u];
		if (event.sequence != sequence || event.sequence <= cursor)
			continue;
		written = snprintf((char *)body + length, capacity - length,
			"%s{\"sequence\":%lu,\"uptime_ms\":%lu,\"type\":%u,\"state\":%u,\"detail\":%u}",
			count == 0 ? "" : ",", (unsigned long)event.sequence,
			(unsigned long)event.uptime_ms, event.type, event.state, event.detail);
		if (written < 0 || (size_t)written >= capacity - length)
			return 0;
		length += (size_t)written;
		count++;
	}
	if (length + 2 >= capacity)
		return 0;
	body[length++] = ']';
	body[length++] = '}';
	body[length] = 0;
	return length;
}

static void http_send(unsigned status, const uint8_t *body, size_t body_length)
{
	size_t response_length = aius_http_write_response(g_http_response,
		sizeof(g_http_response), status, "application/json", body, body_length);
	if (response_length != 0)
		(void)g_http_client.write(g_http_response, response_length);
	g_http_client.stop();
}

static void http_handle_request(void)
{
	AiusHttpRequest request;
	AiusHttpResult result = aius_http_parse(g_http_request, g_http_request_length, &request);
	if (result != AIUS_HTTP_OK) {
		static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"invalid_request\"}";
		g_http_rejections++;
		http_send(http_error_status(result), error_body, sizeof(error_body) - 1);
		return;
	}
	g_http_requests++;
	uint8_t body[AIUS_HTTP_MAX_RESPONSE];
	size_t body_length = 0;
	unsigned status = 200;
	switch (request.route) {
	case AIUS_HTTP_ROUTE_STATUS:
		body_length = http_status_json(body, sizeof(body));
		break;
	case AIUS_HTTP_ROUTE_EVENTS:
		body_length = http_events_json(request.cursor, body, sizeof(body));
		break;
	case AIUS_HTTP_ROUTE_MODE:
		if (!g_audio_processor.set_mode(request.mode)) status = 400;
		body_length = snprintf((char *)body, sizeof(body), "{\"ok\":%s,\"mode\":%u}",
			status == 200 ? "true" : "false", g_audio_processor.mode());
		break;
	case AIUS_HTTP_ROUTE_MIX:
		if (!g_audio_processor.set_mix(request.mix)) status = 400;
		body_length = snprintf((char *)body, sizeof(body), "{\"ok\":%s,\"mix\":%u}",
			status == 200 ? "true" : "false", g_audio_processor.mix());
		break;
	case AIUS_HTTP_ROUTE_START:
		if (!g_test_running) {
			g_test_running = true;
			record_event(2, current_state(), 1);
		}
		body_length = snprintf((char *)body, sizeof(body), "{\"ok\":true,\"running\":true}");
		break;
	case AIUS_HTTP_ROUTE_STOP:
		if (g_test_running) {
			g_test_running = false;
			record_event(2, current_state(), 0);
		}
		body_length = snprintf((char *)body, sizeof(body), "{\"ok\":true,\"running\":false}");
		break;
	case AIUS_HTTP_ROUTE_CLEAR:
		g_audio_processor.clear_counters();
		body_length = snprintf((char *)body, sizeof(body), "{\"ok\":true}");
		break;
	case AIUS_HTTP_ROUTE_REBOOT:
		body_length = snprintf((char *)body, sizeof(body), "{\"ok\":true,\"rebooting\":true}");
		http_send(status, body, (size_t)body_length);
		_reboot_Teensyduino_();
		return;
	default:
		status = 404;
		body_length = snprintf((char *)body, sizeof(body), "{\"ok\":false}");
		break;
	}
	if (body_length < 0 || (size_t)body_length >= sizeof(body)) {
		static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"response_too_large\"}";
		http_send(500, error_body, sizeof(error_body) - 1);
		return;
	}
	http_send(status, body, (size_t)body_length);
}

static void service_http(void)
{
	uint32_t started = micros();
	if (!g_http_client || !g_http_client.connected()) {
		EthernetClient candidate = g_http_server.available();
		if (candidate) {
			if (g_http_client)
				g_http_client_drops++;
			g_http_client = candidate;
			g_http_request_length = 0;
			g_http_last_byte_ms = millis();
		}
	}
	if (!g_http_client)
		return;
	while (g_http_client.available() && g_http_request_length < AIUS_HTTP_MAX_REQUEST) {
		int value = g_http_client.read();
		if (value < 0)
			break;
		g_http_request[g_http_request_length++] = (uint8_t)value;
		g_http_last_byte_ms = millis();
		if ((uint32_t)(micros() - started) >= 1500u)
			break;
	}
	if (g_http_request_length >= sizeof(g_http_request)) {
		g_http_rejections++;
		static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"request_too_large\"}";
		http_send(413, error_body, sizeof(error_body) - 1);
	} else {
		size_t body_offset = 0;
		if (http_header_end(&body_offset)) {
			size_t expected_body = 0;
			int body_result = http_expected_body(body_offset - 2u, &expected_body);
			if (body_result < 0 || g_http_request_length - body_offset > expected_body) {
				g_http_rejections++;
				static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"body_too_large\"}";
				http_send(413, error_body, sizeof(error_body) - 1);
			} else if (g_http_request_length - body_offset == expected_body)
				http_handle_request();
		}
	}
	if (g_http_client && (uint32_t)(millis() - g_http_last_byte_ms) > 150u) {
		g_http_client_drops++;
		g_http_client.stop();
		g_http_request_length = 0;
	}
	if ((uint32_t)(micros() - started) > 2000u)
		g_http_service_overruns++;
}

static void service_network(void)
{
	uint32_t started = micros();
	int packet_size = g_udp.parsePacket();
	if (packet_size <= 0)
		return;
	uint8_t packet[AIUS_NET_MAX_FRAME];
	if (packet_size > (int)sizeof(packet)) {
		g_network_drops++;
		while (g_udp.available())
			(void)g_udp.read();
		return;
	}
	int got = g_udp.read(packet, sizeof(packet));
	if (got != packet_size) {
		g_network_drops++;
		return;
	}
	AiusNetFrame request;
	if (aius_net_decode(packet, (size_t)got, &request) != AIUS_NET_OK) {
		g_network_drops++;
		return;
	}
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
		_reboot_Teensyduino_();
		break;
	case AIUS_NET_CMD_GET_EVENTS: {
		uint32_t cursor = request.payload_len >= 4 ?
			aius_net_get_u32(request.payload) : 0;
		uint8_t count = 0;
		uint32_t first = g_event_sequence > 8u ? g_event_sequence - 7u : 1u;
		for (uint32_t sequence = first; sequence <= g_event_sequence && count < 4; sequence++) {
			const AiusEvent &event = g_events[(sequence - 1u) % 8u];
			if (event.sequence != sequence || event.sequence <= cursor)
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
	if ((uint32_t)(micros() - started) > 2000u)
		g_network_budget_overruns++;
}

static void start_network(void)
{
	/* NativeEthernet provides a bounded DHCP attempt. Static mode is used only
	 * after DHCP fails, keeping the fallback usable on a direct cable. */
	if (g_network_ready)
		return;
	g_dhcp = Ethernet.begin(g_mac, 5000, 250) != 0;
	if (!g_dhcp && Ethernet.linkStatus() == LinkON) {
		Ethernet.begin(g_mac, g_static_ip, IPAddress(0, 0, 0, 0),
		              IPAddress(0, 0, 0, 0), g_static_mask);
	}
	g_network_ready = Ethernet.linkStatus() == LinkON &&
	                  (g_dhcp || Ethernet.localIP() != IPAddress(0, 0, 0, 0));
	if (!g_network_ready) {
		g_network_retry_at = millis() + 5000u;
		Serial.printf("AIUS Ethernet unavailable: link=%u hardware=%u\\n",
		              (unsigned)Ethernet.linkStatus(),
		              (unsigned)Ethernet.hardwareStatus());
		return;
	}
	g_udp.begin(AIUS_NET_PORT);
	g_http_server.begin();
	Serial.printf("AIUS Ethernet ready: dhcp=%u ip=%u.%u.%u.%u link=%u\\n",
	              g_dhcp ? 1u : 0u, Ethernet.localIP()[0], Ethernet.localIP()[1],
	              Ethernet.localIP()[2], Ethernet.localIP()[3],
	              (unsigned)Ethernet.linkStatus());
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
	if (g_network_ready) {
		service_network();
		service_http();
	} else if ((int32_t)(millis() - g_network_retry_at) >= 0) {
		start_network();
	}
	if (g_dhcp)
		(void)Ethernet.maintain();
}
