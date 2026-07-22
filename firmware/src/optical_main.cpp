/* Aius optical prototype bring-up target.
 *
 * This target keeps the Ethernet service bounded and independent from USB. The
 * first bring-up build exposes discovery/status and the safe control boundary;
 * the S/PDIF runtime is added behind the same status snapshot in the next slice.
 */

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <AudioStream.h>
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>
#include <SD.h>
#include <input_spdif3.h>
#include <output_spdif3.h>

extern "C" {
#include "aius_net.h"
	#include "aius_dsp.h"
	#include "aius_pipeline.h"
	#include "aius_http.h"
	#include "service/dhcp/fnet_dhcp_cln.h"
#include "stack/fnet_netif.h"
}
#include "aius_optical_config.h"
#include "aius_auth.h"
#include "aius_config.h"

#ifndef AIUS_BUILD_GIT_REV
#define AIUS_BUILD_GIT_REV "unknown"
#endif
#ifndef AIUS_BUILD_UTC
#define AIUS_BUILD_UTC "unknown"
#endif

static constexpr uint16_t AIUS_NET_PORT = 4242;
static constexpr uint32_t AIUS_STATIC_IP[4] = {192, 168, 42, 42};
static constexpr uint32_t AIUS_STATIC_MASK[4] = {255, 255, 255, 0};

static byte g_mac[] = {0x02, 0x41, 0x49, 0x55, 0x01, 0x01};
static EthernetUDP g_udp;
static EthernetServer g_http_server(80);
static EthernetClient g_http_client;
static uint8_t g_http_request[AIUS_HTTP_MAX_REQUEST];
static uint8_t g_http_response[AIUS_HTTP_MAX_RESPONSE];
static constexpr size_t AIUS_SD_TRANSFER_SLICE = 512u;

enum AiusSdJobState : uint8_t {
	AIUS_SD_JOB_IDLE = 0,
	AIUS_SD_JOB_UPLOAD_OPEN,
	AIUS_SD_JOB_UPLOAD_MKDIR,
	AIUS_SD_JOB_UPLOAD_RECEIVE,
	AIUS_SD_JOB_UPLOAD_WRITE,
	AIUS_SD_JOB_UPLOAD_CHECK_TARGET,
	AIUS_SD_JOB_UPLOAD_BACKUP,
	AIUS_SD_JOB_UPLOAD_INSTALL,
	AIUS_SD_JOB_UPLOAD_ROLLBACK,
	AIUS_SD_JOB_UPLOAD_CLEANUP,
	AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP,
	AIUS_SD_JOB_DOWNLOAD_OPEN,
	AIUS_SD_JOB_DOWNLOAD_HEADER,
	AIUS_SD_JOB_DOWNLOAD_READ,
	AIUS_SD_JOB_DOWNLOAD_WRITE,
	AIUS_SD_JOB_DELETE
};

struct AiusSdJob {
	AiusSdJobState state;
	File file;
	char target_path[128];
	char temp_path[64];
	char backup_path[64];
	const char *content_type;
	uint32_t expected;
	uint32_t received;
	uint32_t written;
	uint32_t initial_offset;
	uint32_t initial_end;
	uint16_t mkdir_offset;
	uint16_t pending_length;
	uint16_t response_offset;
	uint16_t response_length;
	bool target_existed;
	bool backup_moved;
	bool config_upload;
	uint16_t error_status;
	uint8_t transfer[AIUS_SD_TRANSFER_SLICE];
	char response_header[192];
};

static AiusSdJob g_sd_job = {};
static uint32_t g_sd_job_sequence;
static size_t g_http_request_length;
static size_t g_http_body_offset;
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
static bool g_ethernet_initialized;
static bool g_dhcp_pending;
static bool g_sd_ready;
static uint32_t g_dhcp_deadline;
static uint32_t g_boot_epoch;
static uint64_t g_sd_total_bytes;
static uint64_t g_sd_used_bytes;
static uint32_t g_sd_probe_at;
static bool g_sd_usage_refresh_pending;
static AudioInputSPDIF3 g_spdif_in;
static AiusConfig g_config;
static AiusConfig g_pending_config;
static bool g_config_upload_pending;
static uint32_t g_config_error_count;
static char g_auth_token[AIUS_AUTH_TOKEN_STORAGE_SIZE];
static char g_usb_command[32];
static size_t g_usb_command_length;

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

static const char *sd_filesystem(void)
{
	if (!g_sd_ready)
		return "none";
	switch (SD.sdfs.vol()->fatType()) {
	case 12: return "fat12";
	case 16: return "fat16";
	case 32: return "fat32";
	default: return "unknown";
	}
}

static void refresh_sd_usage(void)
{
	if (!g_sd_ready) {
		g_sd_total_bytes = 0;
		g_sd_used_bytes = 0;
		return;
	}
	g_sd_total_bytes = SD.totalSize();
	g_sd_used_bytes = SD.usedSize();
}

static void load_sd_config(void)
{
	aius_config_set_defaults(&g_config);
	File file = SD.open("/aius/config.json", FILE_READ);
	if (!file) {
		g_config_error_count++;
		return;
	}
	uint8_t json[AIUS_CONFIG_MAX_JSON_BYTES];
	size_t length = 0;
	while (file.available() && length < sizeof(json))
		json[length++] = (uint8_t)file.read();
	bool too_large = file.available();
	file.close();
	if (too_large) {
		(void)aius_config_parse(json, sizeof(json) + 1u, &g_config);
		g_config_error_count++;
		return;
	}
	(void)aius_config_parse(json, length, &g_config);
	if (!g_config.valid)
		g_config_error_count++;
}

static void http_send(unsigned status, const uint8_t *body, size_t body_length);

static bool sd_job_active(void)
{
	return g_sd_job.state != AIUS_SD_JOB_IDLE;
}

static void sd_job_clear(void)
{
	if (g_sd_job.file)
		g_sd_job.file.close();
	g_sd_job.state = AIUS_SD_JOB_IDLE;
	g_sd_job.target_path[0] = 0;
	g_sd_job.temp_path[0] = 0;
	g_sd_job.backup_path[0] = 0;
	g_sd_job.content_type = nullptr;
	g_sd_job.expected = 0;
	g_sd_job.received = 0;
	g_sd_job.written = 0;
	g_sd_job.initial_offset = 0;
	g_sd_job.initial_end = 0;
	g_sd_job.mkdir_offset = 0;
	g_sd_job.pending_length = 0;
	g_sd_job.response_offset = 0;
	g_sd_job.response_length = 0;
	g_sd_job.target_existed = false;
	g_sd_job.backup_moved = false;
	g_sd_job.config_upload = false;
	g_sd_job.error_status = 0;
}

static void sd_job_cancel(void)
{
	if (!sd_job_active())
		return;
	if (g_sd_job.file)
		g_sd_job.file.close();
	if (g_sd_job.backup_moved &&
	    (g_sd_job.state == AIUS_SD_JOB_UPLOAD_INSTALL ||
     g_sd_job.state == AIUS_SD_JOB_UPLOAD_ROLLBACK)) {
		g_sd_job.pending_length = 0;
		g_sd_job.error_status = 0;
		g_sd_job.state = AIUS_SD_JOB_UPLOAD_ROLLBACK;
		return;
	}
	if (!g_sd_ready) {
		/* Keep the cancellation marker until the card returns so the
		 * uniquely named temporary file can be removed by service_sd. */
		g_sd_job.pending_length = 0;
		g_sd_job.error_status = 0;
		g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
		return;
	}
	/* Cleanup is deferred as well. In particular, disconnect handling must not
	 * turn an HTTP service call into an SD write. */
	g_sd_job.pending_length = 0;
	g_sd_job.error_status = 0;
	g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
}

static void sd_job_drop_client(void)
{
	if (g_http_client)
		g_http_client.stop();
	g_http_client = EthernetClient();
	g_http_request_length = 0;
}

static uint32_t sd_job_id(void)
{
	return ++g_sd_job_sequence ^ millis() ^ g_boot_epoch;
}

static bool sd_job_make_upload_paths(const char *target)
{
	uint32_t id = sd_job_id();
	int temp_length = snprintf(g_sd_job.temp_path, sizeof(g_sd_job.temp_path),
	                           "/aius/.upload-%08lx-%08lx.tmp",
	                           (unsigned long)id, (unsigned long)g_sd_job_sequence);
	int backup_length = snprintf(g_sd_job.backup_path,
	                             sizeof(g_sd_job.backup_path),
	                             "/aius/.backup-%08lx-%08lx.bak",
	                             (unsigned long)id, (unsigned long)g_sd_job_sequence);
	return temp_length > 0 && backup_length > 0 &&
	       (size_t)temp_length < sizeof(g_sd_job.temp_path) &&
	       (size_t)backup_length < sizeof(g_sd_job.backup_path) &&
	       strlen(target) < sizeof(g_sd_job.target_path);
}

static void sd_job_send_error(unsigned status, const char *error)
{
	char body[64];
	int length = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
	                      error);
	if (length > 0 && (size_t)length < sizeof(body))
		http_send(status, (const uint8_t *)body, (size_t)length);
	else
		sd_job_drop_client();
	sd_job_clear();
}

static void service_sd_job(void)
{
	switch (g_sd_job.state) {
	case AIUS_SD_JOB_UPLOAD_MKDIR: {
		const char *separator = strchr(g_sd_job.target_path + g_sd_job.mkdir_offset, '/');
		if (separator == nullptr) {
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_OPEN;
			return;
		}
		char directory[sizeof(g_sd_job.target_path)];
		size_t length = (size_t)(separator - g_sd_job.target_path);
		memcpy(directory, g_sd_job.target_path, length);
		directory[length] = '\0';
		(void)SD.mkdir(directory);
		g_sd_job.mkdir_offset = (uint16_t)(length + 1u);
		return;
	}
	case AIUS_SD_JOB_UPLOAD_OPEN:
		g_sd_job.file = SD.open(g_sd_job.temp_path, FILE_WRITE);
		if (!g_sd_job.file) {
			g_sd_job.error_status = 507;
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
		} else {
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_RECEIVE;
		}
		return;
	case AIUS_SD_JOB_UPLOAD_RECEIVE:
		if (g_sd_job.pending_length != 0)
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_WRITE;
		else if (g_sd_job.written == g_sd_job.expected)
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_CHECK_TARGET;
		return;
	case AIUS_SD_JOB_UPLOAD_WRITE:
		if (!g_sd_job.file || g_sd_job.pending_length == 0) {
			g_sd_job.error_status = 507;
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
			return;
		}
		if (g_sd_job.file.write(g_sd_job.transfer, g_sd_job.pending_length) !=
		    g_sd_job.pending_length) {
			g_sd_job.error_status = 507;
			g_sd_job.file.close();
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
			return;
		}
		g_sd_job.written += g_sd_job.pending_length;
		g_sd_job.pending_length = 0;
		g_sd_job.state = AIUS_SD_JOB_UPLOAD_RECEIVE;
		return;
	case AIUS_SD_JOB_UPLOAD_CHECK_TARGET:
		if (g_sd_job.file)
			g_sd_job.file.close();
		g_sd_job.target_existed = SD.exists(g_sd_job.target_path);
		g_sd_job.state = g_sd_job.target_existed ?
			AIUS_SD_JOB_UPLOAD_BACKUP : AIUS_SD_JOB_UPLOAD_INSTALL;
		return;
	case AIUS_SD_JOB_UPLOAD_BACKUP:
		if (!SD.rename(g_sd_job.target_path, g_sd_job.backup_path)) {
			g_sd_job.error_status = 500;
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
			return;
		}
		g_sd_job.backup_moved = true;
		g_sd_job.state = AIUS_SD_JOB_UPLOAD_INSTALL;
		return;
	case AIUS_SD_JOB_UPLOAD_INSTALL:
		if (SD.rename(g_sd_job.temp_path, g_sd_job.target_path)) {
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_CLEANUP;
		} else if (g_sd_job.backup_moved) {
			g_sd_job.error_status = 500;
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_ROLLBACK;
		} else {
			g_sd_job.error_status = 500;
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
		}
		return;
	case AIUS_SD_JOB_UPLOAD_ROLLBACK:
		/* The target was moved aside before the install. Keep retrying this
		 * single filesystem operation until the old target is restored. */
		if (SD.rename(g_sd_job.backup_path, g_sd_job.target_path))
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
		return;
	case AIUS_SD_JOB_UPLOAD_CLEANUP:
		if (g_sd_job.backup_moved && !SD.remove(g_sd_job.backup_path))
			return;
		if (g_sd_job.config_upload && g_config_upload_pending) {
			g_config = g_pending_config;
			g_config_upload_pending = false;
		}
		g_sd_usage_refresh_pending = true;
		static const uint8_t upload_ok[] = "{\"ok\":true}";
		http_send(200, upload_ok, sizeof(upload_ok) - 1);
		sd_job_clear();
		return;
	case AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP: {
		if (g_sd_job.file)
			g_sd_job.file.close();
		if (g_sd_job.temp_path[0] != 0)
			(void)SD.remove(g_sd_job.temp_path);
		if (g_sd_job.error_status != 0) {
			if (g_sd_job.config_upload)
				g_config_upload_pending = false;
			unsigned status = g_sd_job.error_status;
			sd_job_send_error(status, status == 500 ? "sd_commit" : "sd_write");
		} else {
			sd_job_drop_client();
			sd_job_clear();
		}
		return;
	}
	case AIUS_SD_JOB_DOWNLOAD_OPEN: {
		g_sd_job.file = SD.open(g_sd_job.target_path, FILE_READ);
		if (!g_sd_job.file) {
			sd_job_send_error(404, "not_found");
			return;
		}
		uint32_t size = (uint32_t)g_sd_job.file.size();
		int length = snprintf(g_sd_job.response_header,
		                      sizeof(g_sd_job.response_header),
		                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
		                      "Content-Length: %lu\r\nConnection: close\r\n\r\n",
		                      g_sd_job.content_type,
		                      (unsigned long)size);
		if (length < 0 || (size_t)length >= sizeof(g_sd_job.response_header)) {
			sd_job_send_error(500, "response_too_large");
			return;
		}
		g_sd_job.response_length = (uint16_t)length;
		g_sd_job.response_offset = 0;
		g_sd_job.state = AIUS_SD_JOB_DOWNLOAD_HEADER;
		return;
	}
	case AIUS_SD_JOB_DOWNLOAD_HEADER: {
		size_t remaining = g_sd_job.response_length - g_sd_job.response_offset;
		size_t written = g_http_client.write(
			(const uint8_t *)g_sd_job.response_header + g_sd_job.response_offset,
			remaining);
		if (written == 0) {
			sd_job_drop_client();
			sd_job_clear();
			return;
		}
		g_sd_job.response_offset += (uint16_t)written;
		if (g_sd_job.response_offset == g_sd_job.response_length)
			g_sd_job.state = AIUS_SD_JOB_DOWNLOAD_READ;
		return;
	}
	case AIUS_SD_JOB_DOWNLOAD_READ: {
		size_t length = g_sd_job.file.read(g_sd_job.transfer,
		                                  sizeof(g_sd_job.transfer));
		if (length == 0) {
			g_sd_job.file.close();
			g_http_client.stop();
			g_http_client = EthernetClient();
			g_http_request_length = 0;
			sd_job_clear();
			return;
		}
		g_sd_job.pending_length = (uint16_t)length;
		g_sd_job.state = AIUS_SD_JOB_DOWNLOAD_WRITE;
		return;
	}
	case AIUS_SD_JOB_DOWNLOAD_WRITE: {
		size_t written = g_http_client.write(g_sd_job.transfer,
		                                    g_sd_job.pending_length);
		if (written == 0) {
			sd_job_drop_client();
			sd_job_clear();
			return;
		}
		if (written < g_sd_job.pending_length) {
			memmove(g_sd_job.transfer, g_sd_job.transfer + written,
			        g_sd_job.pending_length - written);
		}
		g_sd_job.pending_length -= (uint16_t)written;
		if (g_sd_job.pending_length == 0)
			g_sd_job.state = AIUS_SD_JOB_DOWNLOAD_READ;
		return;
	}
	case AIUS_SD_JOB_DELETE:
		if (!SD.remove(g_sd_job.target_path))
			sd_job_send_error(404, "not_found");
		else {
			g_sd_usage_refresh_pending = true;
			static const uint8_t delete_ok[] = "{\"ok\":true}";
			http_send(200, delete_ok, sizeof(delete_ok) - 1);
			sd_job_clear();
		}
		return;
	case AIUS_SD_JOB_IDLE:
		return;
	}
}

static void service_sd(void)
{
	uint32_t now = millis();
	if (g_sd_ready && sd_job_active() && !SD.mediaPresent()) {
		g_sd_ready = false;
		refresh_sd_usage();
		sd_job_cancel();
		sd_job_drop_client();
		Serial.printf("AIUS SD removed\\n");
		return;
	}
	if (!g_sd_ready) {
		if ((int32_t)(now - g_sd_probe_at) < 0)
			return;
		g_sd_probe_at = now + 1000u;
		if (SD.begin(BUILTIN_SDCARD)) {
			g_sd_ready = true;
			(void)SD.mkdir("/aius");
			load_sd_config();
			refresh_sd_usage();
			Serial.printf("AIUS SD inserted fs=%s total=%llu\\n", sd_filesystem(),
			              (unsigned long long)g_sd_total_bytes);
		}
		return;
	}
	if (sd_job_active()) {
		service_sd_job();
		return;
	}
	if ((int32_t)(now - g_sd_probe_at) < 0)
		return;
	g_sd_probe_at = now + 1000u;
	if (!SD.mediaPresent()) {
		g_sd_ready = false;
		refresh_sd_usage();
		Serial.printf("AIUS SD removed\\n");
	} else if (g_sd_usage_refresh_pending) {
		refresh_sd_usage();
		g_sd_usage_refresh_pending = false;
	}
}

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
	bool mode_pending(void) const { return g_mode_pending; }
	bool mix_pending(void) const { return g_mix_pending; }
	bool set_mode(uint8_t mode)
	{
		if (mode >= AIUS_OPTICAL_PROFILE_COUNT)
			return false;
		noInterrupts();
		g_pending_mode = mode;
		g_mode_pending = true;
		interrupts();
		return true;
	}
	bool set_mix(uint8_t mix)
	{
		if (mix >= AIUS_OPTICAL_MIX_COUNT)
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
			const AiusOpticalProfileConfig &profile = AIUS_OPTICAL_PROFILES[mode];
			g_eq_count = profile.eq_count;
			for (size_t i = 0; i < g_eq_count; i++) {
				const AiusOpticalEqConfig &section = profile.eq[i];
				if (section.type == 0)
					g_eq[i] = aius_biquad_bypass();
				else if (section.type == 1)
					g_eq[i] = aius_biquad_highpass(AIUS_OPTICAL_SAMPLE_RATE_HZ,
						section.frequency_hz, section.q);
				else
					g_eq[i] = aius_biquad_peaking(AIUS_OPTICAL_SAMPLE_RATE_HZ,
						section.frequency_hz, section.q, section.gain_db);
			}
			memset(g_eq_state, 0, sizeof(g_eq_state));
		}
		if (g_mix_pending) {
			uint8_t mix = g_pending_mix;
			g_mix_pending = false;
			g_mix = mix;
			const AiusOpticalMixConfig &config = AIUS_OPTICAL_MIXES[mix];
			g_downmix = aius_downmix_config_default();
			g_downmix.lfe = config.lfe;
			if (config.clipfree)
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
			uint32_t digit = (uint32_t)(g_http_request[i] - '0');
			if (value > (UINT32_MAX - digit) / 10u)
				return -1;
			value = value * 10u + digit;
			i++;
		}
		*expected = value;
		return 1;
	}
	return 1;
}

static int http_has_content_length(size_t header_end)
{
	for (size_t i = 0; i + 16 < header_end; i++)
		if (memcmp(g_http_request + i, "Content-Length: ", 16) == 0)
			return 1;
	return 0;
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

static bool http_json_append(char *body, size_t capacity, size_t *length,
	                           const char *format, ...)
{
	if (*length >= capacity)
		return false;
	va_list arguments;
	va_start(arguments, format);
	int written = vsnprintf(body + *length, capacity - *length, format,
	                        arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= capacity - *length)
		return false;
	*length += (size_t)written;
	return true;
}

static const char *http_dsp_eq_type(uint8_t type)
{
	switch (type) {
	case 0: return "bypass";
	case 1: return "highpass";
	case 2: return "peaking";
	default: return nullptr;
	}
}

static size_t http_dsp_config_json(uint8_t *body, size_t capacity)
{
	if (capacity > AIUS_OPTICAL_MODEL_MAX_JSON_BYTES)
		capacity = AIUS_OPTICAL_MODEL_MAX_JSON_BYTES;
	char *output = (char *)body;
	size_t length = 0;
	if (!http_json_append(output, capacity, &length,
	                      "{\"schema_version\":1,\"sample_rate_hz\":%lu,\"profiles\":[",
	                      (unsigned long)AIUS_OPTICAL_SAMPLE_RATE_HZ))
		return 0;
	for (uint8_t profile_index = 0; profile_index < AIUS_OPTICAL_PROFILE_COUNT;
	     profile_index++) {
		const AiusOpticalProfileConfig &profile = AIUS_OPTICAL_PROFILES[profile_index];
		if (!http_json_append(output, capacity, &length, "%s{\"id\":\"%s\",\"eq\":[",
		                      profile_index == 0 ? "" : ",", profile.name))
			return 0;
		for (uint8_t section_index = 0; section_index < profile.eq_count;
		     section_index++) {
			const AiusOpticalEqConfig &section = profile.eq[section_index];
			const char *type = http_dsp_eq_type(section.type);
			if (type == nullptr || !http_json_append(
				output, capacity, &length,
				"%s{\"type\":\"%s\",\"frequency_hz\":%.17g,\"q\":%.17g,\"gain_db\":%.17g}",
				section_index == 0 ? "" : ",", type, section.frequency_hz,
				section.q, section.gain_db))
				return 0;
		}
		if (!http_json_append(output, capacity, &length, "]}"))
			return 0;
	}
	if (!http_json_append(output, capacity, &length, "],\"mixes\":["))
		return 0;
	for (uint8_t mix_index = 0; mix_index < AIUS_OPTICAL_MIX_COUNT; mix_index++) {
		const AiusOpticalMixConfig &mix = AIUS_OPTICAL_MIXES[mix_index];
		AiusDownmixConfig downmix = aius_downmix_config_default();
		downmix.lfe = mix.lfe;
		double headroom = mix.clipfree ? aius_downmix_clipfree_headroom(&downmix) :
		                    mix.headroom;
		if (!http_json_append(output, capacity, &length,
		                      "%s{\"id\":\"%s\",\"lfe_gain_linear\":%.17g,\"headroom_linear\":%.17g,\"clipfree\":%s}",
		                      mix_index == 0 ? "" : ",", mix.name, mix.lfe,
		                      headroom, mix.clipfree ? "true" : "false"))
			return 0;
	}
	if (!http_json_append(output, capacity, &length,
	                      "],\"active\":{\"profile_id\":\"%s\",\"profile_index\":%u,\"mix_id\":\"%s\",\"mix_index\":%u,\"profile_pending\":%u,\"mix_pending\":%u}}",
	                      AIUS_OPTICAL_PROFILES[g_audio_processor.mode()].name,
	                      g_audio_processor.mode(),
	                      AIUS_OPTICAL_MIXES[g_audio_processor.mix()].name,
	                      g_audio_processor.mix(),
	                      g_audio_processor.mode_pending() ? 1u : 0u,
	                      g_audio_processor.mix_pending() ? 1u : 0u))
		return 0;
	return length;
}

static size_t http_status_json(uint8_t *body, size_t capacity)
{
	uint8_t status[71];
	uint16_t status_length = 0;
	put_status(status, &status_length);
	if (capacity == 0 || status_length < 71)
		return 0;
	const char *network_mode = status[1] == 0 ? "offline" :
		(status[1] == 1 ? "dhcp" : "static");
	int written = snprintf((char *)body, capacity,
		"{\"version\":%u,\"state\":%u,\"carrier\":%u,\"format\":%u,\"sample_rate\":%lu,\"blocks\":%lu,\"overruns\":%lu,\"mode\":%u,\"mix\":%u,\"mode_pending\":%u,\"mix_pending\":%u,\"sd_present\":%u,\"sd_filesystem\":\"%s\",\"sd_total_bytes\":%llu,\"sd_used_bytes\":%llu,\"sd_free_bytes\":%llu,\"config_valid\":%u,\"config_error_code\":%u,\"config_error\":\"%s\",\"config_error_count\":%lu,\"network_mode\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"link\":%u,\"build_revision\":\"%s\",\"build_utc\":\"%s\",\"event_sequence\":%lu,\"event_overflows\":%lu,\"network_drops\":%lu,\"network_budget_overruns\":%lu,\"http_requests\":%lu,\"http_rejections\":%lu,\"http_client_drops\":%lu,\"http_service_overruns\":%lu}",
		status[0], status[2], status[3] & 1u, status[23],
		(unsigned long)aius_net_get_u32(status + 25),
		(unsigned long)aius_net_get_u32(status + 29),
		(unsigned long)aius_net_get_u32(status + 33), status[37], status[38],
		g_audio_processor.mode_pending() ? 1u : 0u,
		g_audio_processor.mix_pending() ? 1u : 0u,
		g_sd_ready ? 1u : 0u, sd_filesystem(),
		(unsigned long long)g_sd_total_bytes,
		(unsigned long long)g_sd_used_bytes,
		(unsigned long long)(g_sd_total_bytes - g_sd_used_bytes),
		g_config.valid, (unsigned)g_config.error_code, g_config.error_message,
		(unsigned long)g_config_error_count,
		network_mode, status[12], status[13], status[14], status[15],
		(status[1] != 0 && Ethernet.linkStatus() == LinkON) ? 1u : 0u,
		AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC,
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

static void http_send_content(unsigned status, const char *content_type,
                              const uint8_t *body, size_t body_length)
{
	size_t response_length = aius_http_write_response(g_http_response,
		sizeof(g_http_response), status, content_type, body, body_length);
	for (size_t offset = 0; response_length != 0 && offset < response_length;) {
		size_t length = response_length - offset;
		if (length > 512u)
			length = 512u;
		size_t written = g_http_client.write(g_http_response + offset, length);
		if (written == 0)
			break;
		offset += written;
	}
	g_http_client.stop();
	g_http_client = EthernetClient();
}

static void http_send(unsigned status, const uint8_t *body, size_t body_length)
{
	http_send_content(status, "application/json", body, body_length);
}

static bool http_sd_path(const AiusHttpRequest *request, char *path,
                         size_t capacity, const char **content_type);

static bool http_sd_authorized(const AiusHttpRequest &request)
{
	return aius_auth_token_matches(g_auth_token, (const char *)request.sd_auth,
	                               request.sd_auth_length) != 0;
}

static bool http_sd_resource_is_config(const AiusHttpRequest &request)
{
	return request.resource_length >= 11u &&
		memcmp(request.resource + request.resource_length - 11u,
		       "config.json", 11u) == 0;
}

static bool http_sd_upload_begin_target(const char *target,
                                        const char *content_type,
                                        bool config_upload, uint32_t expected,
                                        size_t initial_offset, size_t initial_end)
{
	if (sd_job_active() || target == nullptr || content_type == nullptr ||
		strlen(target) >= sizeof(g_sd_job.target_path))
		return false;
	strncpy(g_sd_job.target_path, target, sizeof(g_sd_job.target_path));
	if (!sd_job_make_upload_paths(g_sd_job.target_path))
		return false;
	g_sd_job.content_type = content_type;
	g_sd_job.config_upload = config_upload;
	g_sd_job.expected = expected;
	g_sd_job.initial_offset = (uint32_t)initial_offset;
	g_sd_job.initial_end = (uint32_t)initial_end;
	g_sd_job.mkdir_offset = 6u;
	g_sd_job.state = AIUS_SD_JOB_UPLOAD_MKDIR;
	return true;
}

static bool http_sd_upload_begin(const AiusHttpRequest &request,
                                 uint32_t expected, size_t initial_offset,
                                 size_t initial_end)
{
	char target[sizeof(g_sd_job.target_path)];
	const char *content_type = nullptr;
	if (!http_sd_path(&request, target, sizeof(target), &content_type))
		return false;
	return http_sd_upload_begin_target(target, content_type, false, expected,
	                                  initial_offset, initial_end);
}

static bool http_sd_stage_upload(void)
{
	if (!sd_job_active() ||
	    (g_sd_job.state != AIUS_SD_JOB_UPLOAD_OPEN &&
	     g_sd_job.state != AIUS_SD_JOB_UPLOAD_RECEIVE) ||
	    g_sd_job.pending_length != 0 ||
	    g_sd_job.received == g_sd_job.expected)
		return true;
	size_t remaining = g_sd_job.expected - g_sd_job.received;
	if (g_sd_job.initial_offset < g_sd_job.initial_end) {
		size_t available = g_sd_job.initial_end - g_sd_job.initial_offset;
		size_t length = available < remaining ? available : remaining;
		if (length > sizeof(g_sd_job.transfer))
			length = sizeof(g_sd_job.transfer);
		memcpy(g_sd_job.transfer, g_http_request + g_sd_job.initial_offset,
		       length);
		g_sd_job.initial_offset += (uint32_t)length;
		g_sd_job.pending_length = (uint16_t)length;
		g_sd_job.received += (uint32_t)length;
		return true;
	}
	if (!g_http_client.available())
		return true;
	size_t available = (size_t)g_http_client.available();
	size_t length = available < remaining ? available : remaining;
	if (length > sizeof(g_sd_job.transfer))
		length = sizeof(g_sd_job.transfer);
	int got = g_http_client.read(g_sd_job.transfer, length);
	if (got <= 0)
		return false;
	g_http_last_byte_ms = millis();
	g_sd_job.pending_length = (uint16_t)got;
	g_sd_job.received += (uint32_t)got;
	return true;
}

static void http_sd_upload_receive_service(void)
{
	if (!http_sd_stage_upload()) {
		g_sd_job.error_status = 507;
		g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
		return;
	}
}

static bool http_sd_path(const AiusHttpRequest *request, char *path,
                         size_t capacity, const char **content_type)
{
	if (request == nullptr || request->resource == nullptr ||
		request->resource_length < 5u || request->resource_length > 96u ||
		memcmp(request->resource, "www/", 4) != 0)
		return false;
	const char *resource = (const char *)request->resource;
	size_t component_length = 0u;
	for (size_t i = 4u; i < request->resource_length; i++) {
		uint8_t ch = request->resource[i];
		if (ch == '%' || ch == '\\' || ch == 0u)
			return false;
		if (ch == '/') {
			if (i == 4u || i + 1u == request->resource_length ||
				request->resource[i - 1u] == '/' || component_length > 32u)
				return false;
			component_length = 0u;
			continue;
		}
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
			return false;
		component_length++;
		if (component_length > 32u)
			return false;
	}
	for (size_t i = 4u; i + 1u < request->resource_length; i++)
		if (resource[i] == '.' && resource[i + 1u] == '.' &&
			(i == 4u || resource[i - 1u] == '/') &&
			(i + 2u == request->resource_length || resource[i + 2u] == '/'))
			return false;
	if (component_length > 32u)
		return false;
	unsigned components = 1u;
	for (size_t i = 4u; i < request->resource_length; i++)
		if (resource[i] == '/') {
			components++;
			if (components > 8u)
				return false;
		}
	const char *type = "application/octet-stream";
	if (request->resource_length >= 5u &&
		memcmp(resource + request->resource_length - 5u, ".html", 5u) == 0)
		type = "text/html; charset=utf-8";
	else if (request->resource_length >= 3u &&
		memcmp(resource + request->resource_length - 3u, ".js", 3u) == 0)
		type = "application/javascript";
	else if (request->resource_length >= 4u &&
		memcmp(resource + request->resource_length - 4u, ".css", 4u) == 0)
		type = "text/css";
	else if (request->resource_length >= 5u &&
		memcmp(resource + request->resource_length - 5u, ".json", 5u) == 0)
		type = "application/json";
	int written = snprintf(path, capacity, "/aius/%.*s",
		(int)request->resource_length, request->resource);
	if (written < 0 || (size_t)written >= capacity)
		return false;
	*content_type = type;
	return true;
}

static void http_sd_request(const AiusHttpRequest &request)
{
	if (!g_sd_ready) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_unavailable\"}";
		http_send(503, body, sizeof(body) - 1);
		return;
	}
	if (request.route == AIUS_HTTP_ROUTE_SD_STATUS) {
		static const uint8_t body[] = "{\"ready\":true}";
		http_send(200, body, sizeof(body) - 1);
		return;
	}
	char path[sizeof(g_sd_job.target_path)];
	const char *content_type = nullptr;
	if (!http_sd_path(&request, path, sizeof(path), &content_type)) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_path\"}";
		http_send(404, body, sizeof(body) - 1);
		return;
	}
	if (request.method != AIUS_HTTP_GET && http_sd_resource_is_config(request)) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"config_public_only\"}";
		http_send(403, body, sizeof(body) - 1);
		return;
	}
	if (request.method == AIUS_HTTP_PUT || request.method == AIUS_HTTP_DELETE) {
		if (!http_sd_authorized(request)) {
			static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_auth\"}";
			http_send(401, body, sizeof(body) - 1);
			return;
		}
	}
	if (request.method == AIUS_HTTP_GET) {
		if (sd_job_active()) {
			static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_busy\"}";
			http_send(503, body, sizeof(body) - 1);
			return;
		}
		strncpy(g_sd_job.target_path, path, sizeof(g_sd_job.target_path));
		g_sd_job.content_type = content_type;
		g_sd_job.state = AIUS_SD_JOB_DOWNLOAD_OPEN;
		return;
	}
	if (request.method == AIUS_HTTP_DELETE) {
		if (sd_job_active()) {
			static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_busy\"}";
			http_send(503, body, sizeof(body) - 1);
			return;
		}
		strncpy(g_sd_job.target_path, path, sizeof(g_sd_job.target_path));
		g_sd_job.state = AIUS_SD_JOB_DELETE;
		return;
	}
	static const uint8_t error[] = "{\"ok\":false,\"error\":\"sd_upload\"}";
	http_send(400, error, sizeof(error) - 1);
}

static void http_config_public_request(const AiusHttpRequest &request)
{
	if (!g_sd_ready) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_unavailable\"}";
		http_send(503, body, sizeof(body) - 1);
		return;
	}
	AiusConfig parsed;
	size_t length = g_http_request_length - g_http_body_offset;
	AiusConfigErrorCode error = aius_config_parse(
		g_http_request + g_http_body_offset, length, &parsed);
	if (error != AIUS_CONFIG_OK) {
		g_config = parsed;
		g_config_error_count++;
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"config_invalid\"}";
		http_send(400, body, sizeof(body) - 1);
		return;
	}
	if (!http_sd_upload_begin_target("/aius/config.json", "application/json",
	                                true, (uint32_t)length, g_http_body_offset,
	                                g_http_request_length)) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_busy\"}";
		http_send(503, body, sizeof(body) - 1);
		return;
	}
	g_pending_config = parsed;
	g_config_upload_pending = true;
	(void)request;
}

static void reset_application(void)
{
	__disable_irq();
	SCB_AIRCR = 0x05FA0004;
	asm volatile("dsb sy" ::: "memory");
	while (true)
		asm volatile("isb" ::: "memory");
}

static size_t http_home_page(uint8_t *body, size_t capacity)
{
	int length = snprintf((char *)body, capacity,
		"<!doctype html><html><head><meta charset=\"utf-8\"><title>Aius</title>"
		"<style>body{font-family:monospace;white-space:pre-wrap}code{font-family:inherit}</style></head>"
		"<body><h1>Aius optical firmware</h1><p>Git revision: %s<br>Build UTC: %s</p>"
		"<h2>API</h2><ul><li><a href=\"/api/status\">GET /api/status</a></li>"
		"<li><a href=\"/api/events\">GET /api/events</a></li>"
		"<li><code>POST /api/mode {&quot;mode&quot;:&quot;flat&quot;}</code></li>"
		"<li><code>POST /api/mix {&quot;mix&quot;:&quot;standard&quot;}</code></li>"
		"<li><code>POST /api/start {}</code></li>"
		"<li><code>POST /api/stop {}</code></li>"
		"<li><code>POST /api/clear {}</code></li>"
		"<li><code>POST /api/reboot {}</code></li>"
		"<li><a href=\"/api/sd\">GET /api/sd</a></li>"
		"<li><code>PUT /sd/www/{path}</code> raw body with Content-Length and X-Aius-SD-Auth</li>"
		"<li><code>DELETE /sd/www/{path}</code> with X-Aius-SD-Auth</li>"
		"<li><code>PUT /api/config/public</code> JSON feature flags, no auth</li></ul>"
		"<h2>SD web upload</h2><p>Get the token over USB with <code>AIUS AUTH</code>. "
		"The picker preserves relative paths and sends authenticated standard HTTP PUT requests.</p>"
		"<label>SD auth token <input id=t size=16 maxlength=16 autocomplete=off></label> "
		"<input id=f type=file multiple webkitdirectory directory><button onclick=upload()>Upload selected files</button>"
		"<pre id=u></pre>"
		"<h2>Control</h2><label>Mode <select id=m>"
		"<option>%s</option><option>%s</option><option>%s</option></select></label> "
		"<label>Mix <select id=x><option>%s</option><option>%s</option>"
		"<option>%s</option></select></label> <button onclick=go('/api/mode',{mode:m.value})>Set mode</button> "
		"<button onclick=go('/api/mix',{mix:x.value})>Set mix</button><p>"
		"<button title=\"Enable test processing state\" onclick=go('/api/start')>Start</button> "
		"<button title=\"Disable test processing state\" onclick=go('/api/stop')>Stop</button> "
		"<button title=\"Clear audio and event counters\" onclick=go('/api/clear')>Clear counters</button> "
		"<button title=\"Restart the application without entering HalfKay\" onclick=reboot()>Reboot</button></p>"
		"<pre id=s>Loading status...</pre><p id=e></p></body><script>"
		"const s=document.getElementById('s'),e=document.getElementById('e'),u=document.getElementById('u');"
		"async function get(){try{s.textContent=JSON.stringify(await (await fetch('/api/status')).json(),null,2);e.textContent=''}catch(a){e.textContent=a}}"
		"async function go(a,b){try{await fetch(a,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});get()}catch(c){e.textContent=c}}"
		"async function reboot(){e.textContent='Rebooting...';try{await go('/api/reboot');setTimeout(get,2000)}catch(a){}}"
		"async function upload(){u.textContent='';const token=document.getElementById('t').value;const files=[...(document.getElementById('f').files||[])];for(const file of files){const rel=file.webkitRelativePath||file.name;const path=rel.split('/').slice(1).join('/')||rel;if(path.split('/').pop()==='config.json'){u.textContent+='refused '+path+'\\n';continue}try{const r=await fetch('/sd/www/'+path.split('/').map(encodeURIComponent).join('/'),{method:'PUT',headers:{'X-Aius-SD-Auth':token,'Content-Type':'application/octet-stream'},body:file});u.textContent+=r.status+' '+path+'\\n'}catch(a){u.textContent+='error '+path+' '+a+'\\n'}}}"
		"get();setInterval(get,2000)</script></html>",
		AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC,
		AIUS_OPTICAL_PROFILES[0].name, AIUS_OPTICAL_PROFILES[1].name,
		AIUS_OPTICAL_PROFILES[2].name, AIUS_OPTICAL_MIXES[0].name,
		AIUS_OPTICAL_MIXES[1].name, AIUS_OPTICAL_MIXES[2].name);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
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
	case AIUS_HTTP_ROUTE_CONFIG_PUBLIC:
		http_config_public_request(request);
		return;
	case AIUS_HTTP_ROUTE_SD_STATUS:
	case AIUS_HTTP_ROUTE_SD_FILE:
		http_sd_request(request);
		return;
	case AIUS_HTTP_ROUTE_HOME:
		body_length = http_home_page(body, sizeof(body));
		if (body_length == 0) {
			static const uint8_t error_body[] = "<p>response_too_large</p>";
			http_send_content(500, "text/html; charset=utf-8", error_body,
			                 sizeof(error_body) - 1);
			return;
		}
		http_send_content(status, "text/html; charset=utf-8", body, body_length);
		return;
	case AIUS_HTTP_ROUTE_STATUS:
		body_length = http_status_json(body, sizeof(body));
		break;
	case AIUS_HTTP_ROUTE_DSP_CONFIG:
		body_length = http_dsp_config_json(body, sizeof(body));
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
		delay(20);
		reset_application();
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
	if (sd_job_active()) {
		if (!g_http_client || !g_http_client.connected()) {
			g_http_client_drops++;
			sd_job_cancel();
			sd_job_drop_client();
			return;
		}
		if (g_sd_job.state == AIUS_SD_JOB_UPLOAD_OPEN ||
		    g_sd_job.state == AIUS_SD_JOB_UPLOAD_RECEIVE) {
			http_sd_upload_receive_service();
			if (g_sd_job.received < g_sd_job.expected &&
			    (uint32_t)(millis() - g_http_last_byte_ms) > 150u) {
				g_http_client_drops++;
				sd_job_cancel();
				sd_job_drop_client();
			}
		}
		return;
	}
	if (!g_http_client || !g_http_client.connected()) {
		EthernetClient candidate = g_http_server.accept();
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
			g_http_body_offset = body_offset;
			size_t expected_body = 0;
			int body_result = http_expected_body(body_offset - 2u, &expected_body);
			AiusHttpRequest request;
			AiusHttpResult parse_result = aius_http_parse(g_http_request, body_offset,
			                                             &request);
			if (parse_result == AIUS_HTTP_OK &&
			    request.route == AIUS_HTTP_ROUTE_SD_FILE &&
			    request.method == AIUS_HTTP_PUT &&
			    http_sd_resource_is_config(request)) {
				static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"config_public_only\"}";
				http_send(403, error_body, sizeof(error_body) - 1);
			} else if (parse_result == AIUS_HTTP_OK &&
			           request.route == AIUS_HTTP_ROUTE_SD_FILE &&
			           request.method == AIUS_HTTP_PUT &&
			           !http_sd_authorized(request)) {
				static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"sd_auth\"}";
				http_send(401, error_body, sizeof(error_body) - 1);
			} else if (parse_result == AIUS_HTTP_OK &&
			           request.route == AIUS_HTTP_ROUTE_SD_FILE &&
			           request.method == AIUS_HTTP_PUT && !g_sd_ready) {
				static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"sd_unavailable\"}";
				http_send(503, error_body, sizeof(error_body) - 1);
			} else if (parse_result == AIUS_HTTP_OK &&
			           request.route == AIUS_HTTP_ROUTE_SD_FILE &&
			           request.method == AIUS_HTTP_PUT) {
				if (body_result < 0 || !http_has_content_length(body_offset - 2u) ||
				    g_http_request_length - body_offset > expected_body ||
				    !http_sd_upload_begin(request, (uint32_t)expected_body,
				                          body_offset, g_http_request_length)) {
					sd_job_clear();
					g_http_rejections++;
					static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"sd_upload\"}";
					http_send(body_result < 0 ? 413 : 507, error_body,
					          sizeof(error_body) - 1);
				} else {
					/* Keep the already-received body in the request buffer until
					 * the receipt state stages it. No SD call occurs here. */
					return;
				}
			} else if (body_result < 0 || g_http_request_length - body_offset > expected_body) {
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
		g_http_client = EthernetClient();
		g_http_request_length = 0;
	}
	if ((uint32_t)(micros() - started) > 2000u)
		g_http_service_overruns++;
}

static void service_usb_auth(void)
{
	while (Serial.available()) {
		int value = Serial.read();
		if (value < 0)
			return;
		if (value == '\n' || value == '\r') {
			g_usb_command[g_usb_command_length] = '\0';
			if (strcmp(g_usb_command, "AIUS AUTH") == 0)
				Serial.printf("AIUS AUTH %s\n", g_auth_token);
			g_usb_command_length = 0;
			continue;
		}
		if (g_usb_command_length + 1u < sizeof(g_usb_command))
			g_usb_command[g_usb_command_length++] = (char)value;
		else
			g_usb_command_length = 0;
	}
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

static bool ensure_dhcp_client(void)
{
	fnet_netif_desc_t netif = fnet_netif_get_default();
	if (netif == nullptr)
		return false;
	fnet_dhcp_cln_desc_t client = fnet_dhcp_cln_get_by_netif(netif);
	if (client != nullptr && fnet_dhcp_cln_is_enabled(client))
		return true;
	static fnet_dhcp_cln_params_t params = {};
	params.netif = netif;
	client = fnet_dhcp_cln_init(&params);
	if (client == nullptr)
		return false;
	fnet_dhcp_cln_set_response_timeout(client, 250);
	return true;
}

static void activate_network(bool dhcp)
{
	g_dhcp = dhcp;
	g_dhcp_pending = false;
	g_network_ready = true;
	g_udp.begin(AIUS_NET_PORT);
	g_http_server.begin();
	Serial.printf("AIUS Ethernet ready: dhcp=%u ip=%u.%u.%u.%u link=%u\\n",
	              g_dhcp ? 1u : 0u, Ethernet.localIP()[0], Ethernet.localIP()[1],
	              Ethernet.localIP()[2], Ethernet.localIP()[3],
	              (unsigned)Ethernet.linkStatus());
}

static void start_network(void)
{
	/* NativeEthernet provides a bounded DHCP attempt. Static mode is used only
	 * after DHCP fails, keeping the fallback usable on a direct cable. */
	if (g_network_ready)
		return;
	if (g_dhcp_pending) {
		if (Ethernet.localIP() != IPAddress(0, 0, 0, 0)) {
			activate_network(true);
			return;
		}
		if ((int32_t)(millis() - g_dhcp_deadline) < 0)
			return;
		g_dhcp_pending = false;
	}
	if (g_ethernet_initialized && Ethernet.linkStatus() != LinkON) {
		g_network_retry_at = millis() + 5000u;
		return;
	}
	if (g_ethernet_initialized && Ethernet.linkStatus() == LinkON) {
		Serial.printf("AIUS Ethernet DHCP retry exhausted; static fallback begin\\n");
		Ethernet.begin(g_mac, g_static_ip, IPAddress(0, 0, 0, 0),
		              IPAddress(0, 0, 0, 0), g_static_mask);
		if (Ethernet.localIP() != IPAddress(0, 0, 0, 0)) {
			activate_network(false);
			return;
		}
	}
	if (g_ethernet_initialized) {
		g_network_retry_at = millis() + 5000u;
		return;
	}
	Serial.printf("AIUS Ethernet MAC configured=%02x:%02x:%02x:%02x:%02x:%02x\\n",
	              g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
	Serial.printf("AIUS Ethernet init: DHCP begin\\n");
	g_ethernet_initialized = true;
	g_dhcp = Ethernet.begin(g_mac, 5000, 250) != 0;
	byte actual_mac[6] = {};
	Ethernet.MACAddress(actual_mac);
	Serial.printf("AIUS Ethernet MAC actual=%02x:%02x:%02x:%02x:%02x:%02x\\n",
	              actual_mac[0], actual_mac[1], actual_mac[2], actual_mac[3],
	              actual_mac[4], actual_mac[5]);
	Serial.printf("AIUS Ethernet DHCP result=%u link=%u\\n", g_dhcp ? 1u : 0u,
	              (unsigned)Ethernet.linkStatus());
	if (g_dhcp) {
		activate_network(true);
		return;
	}
	if (ensure_dhcp_client()) {
		g_dhcp_pending = true;
		g_dhcp_deadline = millis() + 5000u;
		Serial.printf("AIUS Ethernet DHCP client enabled after link callback gap\\n");
		return;
	}
	g_network_retry_at = millis() + 5000u;
	Serial.printf("AIUS Ethernet unavailable: link=%u hardware=%u\\n",
	              (unsigned)Ethernet.linkStatus(),
	              (unsigned)Ethernet.hardwareStatus());
}

void setup(void)
{
	Serial.begin(115200);
	uint8_t auth_material[AIUS_AUTH_DEVICE_MATERIAL_SIZE];
	uint32_t mac0 = HW_OCOTP_MAC0;
	uint32_t mac1 = HW_OCOTP_MAC1;
	uint32_t mac2 = HW_OCOTP_MAC2;
	memcpy(auth_material, &mac0, sizeof(mac0));
	memcpy(auth_material + 4u, &mac1, sizeof(mac1));
	memcpy(auth_material + 8u, &mac2, sizeof(mac2));
	memcpy(auth_material + 12u, &mac0, sizeof(mac0));
	(void)aius_auth_derive_token(auth_material, g_auth_token);
	aius_config_set_defaults(&g_config);
	AudioMemory(24);
	g_audio_processor.begin_pipeline();
	g_boot_epoch = micros();
	g_sd_ready = SD.begin(BUILTIN_SDCARD);
	if (g_sd_ready) {
		(void)SD.mkdir("/aius");
		load_sd_config();
	}
	refresh_sd_usage();
	Serial.printf("AIUS SD ready=%u\\n", g_sd_ready ? 1u : 0u);
	start_network();
}

void loop(void)
{
	service_usb_auth();
	if (g_network_ready) {
		service_network();
		service_http();
	} else if ((int32_t)(millis() - g_network_retry_at) >= 0) {
		start_network();
	}
	if (g_dhcp)
		(void)Ethernet.maintain();
	service_sd();
}
