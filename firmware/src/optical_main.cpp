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
	#include "aius_usb_control.h"
	#include "aius_dsp_config.h"
	#include "service/dhcp/fnet_dhcp_cln.h"
#include "stack/fnet_netif.h"
}
#include "aius_replay.h"
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
// HTTP request/response buffers live in OCRAM (DMAMEM), not DTCM: they are off
// the audio critical path (touched only during request handling) and each is
// written before it is read, so OCRAM's lack of startup zeroing is harmless.
// Relocating them frees ~16 KB of FlexRAM so ITCM can grow (see docs plan).
DMAMEM static uint8_t g_http_request[AIUS_HTTP_MAX_REQUEST];
DMAMEM static uint8_t g_http_response[AIUS_HTTP_MAX_RESPONSE];
// GET /api/dsp/config's response can be up to AIUS_OPTICAL_MODEL_MAX_JSON_BYTES
// (12288 B), larger than every other route's response and larger than the
// shared per-route stack `body[AIUS_HTTP_MAX_RESPONSE]` buffer in
// http_handle_request(). A dedicated OCRAM buffer avoids bumping that shared
// 8 KB stack array for every other route just to fit this one model.
DMAMEM static uint8_t g_dsp_config_body[AIUS_OPTICAL_MODEL_MAX_JSON_BYTES];
static constexpr size_t AIUS_SD_TRANSFER_SLICE = 512u;
static constexpr uint32_t AIUS_SD_DOWNLOAD_STALL_MS = 10000u;
static constexpr uint32_t AIUS_SD_UPLOAD_STALL_MS = 2000u;
static constexpr uint32_t AIUS_HTTP_WRITE_STALL_MS = 100u;
static constexpr uint32_t AIUS_NETWORK_LINK_DEBOUNCE_MS = 250u;
static constexpr uint32_t AIUS_NETWORK_RETRY_MS = 1000u;
DMAMEM static AiusReplayService g_replay_service;
static File g_replay_file;
static char g_replay_path[96];

static AiusStatus replay_reader_open(void *, uint32_t file_bytes)
{
	(void)file_bytes;
	if (g_replay_file) g_replay_file.close();
	g_replay_file = SD.open(g_replay_path, FILE_READ);
	return g_replay_file ? AIUS_OK : AIUS_MALFORMED;
}

static AiusStatus replay_reader_read(void *, uint32_t offset, uint8_t *buffer,
	                                  size_t capacity, size_t *bytes_read)
{
	if (!g_replay_file || buffer == nullptr || bytes_read == nullptr ||
	    !g_replay_file.seek(offset)) return AIUS_MALFORMED;
	int count = g_replay_file.read(buffer, capacity);
	if (count < 0) return AIUS_MALFORMED;
	*bytes_read = (size_t)count;
	return AIUS_OK;
}

static void replay_reader_close(void *)
{
	if (g_replay_file) g_replay_file.close();
}

static AiusReplayReader g_replay_reader = {
	nullptr, replay_reader_open, replay_reader_read, replay_reader_close
};

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
	uint32_t progress_ms;
	bool target_existed;
	bool backup_moved;
	bool config_upload;
	bool dsp_upload;
	bool replay_upload;
	bool replay_final;
	uint32_t replay_offset;
	uint32_t replay_total;
	uint16_t error_status;
	uint8_t transfer[AIUS_SD_TRANSFER_SLICE];
	char response_header[192];
};

DMAMEM static AiusSdJob g_sd_job = {};
static uint32_t g_sd_job_sequence;
static size_t g_http_request_length;
static size_t g_http_body_offset;
static uint32_t g_http_last_byte_ms;
static uint32_t g_http_requests;
static uint32_t g_http_rejections;
static uint32_t g_http_client_drops;
static uint32_t g_http_service_overruns;
static uint32_t g_http_server_rebinds;
static float g_cpu_temp_c;
static float g_cpu_temp_max_c;
static uint32_t g_cpu_temp_sample_ms;
static uint32_t g_cpu_temp_next_ms;
static bool g_cpu_temp_valid;
static IPAddress g_static_ip(AIUS_STATIC_IP[0], AIUS_STATIC_IP[1],
                             AIUS_STATIC_IP[2], AIUS_STATIC_IP[3]);
static IPAddress g_static_mask(AIUS_STATIC_MASK[0], AIUS_STATIC_MASK[1],
                               AIUS_STATIC_MASK[2], AIUS_STATIC_MASK[3]);
static bool g_dhcp;
static bool g_network_ready;
static uint32_t g_network_retry_at;
static bool g_ethernet_initialized;
static bool g_dhcp_pending;
static uint32_t g_network_link_down_at;
static uint32_t g_network_restarts;
static uint8_t g_dhcp_attempts;
static bool g_sd_ready;
static uint32_t g_dhcp_deadline;
static volatile uint32_t g_dhcp_discover_count;
static volatile uint32_t g_dhcp_update_count;
static uint32_t g_boot_epoch;
static uint32_t g_reset_source;
static bool g_watchdog_enabled;
static uint64_t g_sd_total_bytes;
static uint64_t g_sd_used_bytes;
static uint32_t g_sd_probe_at;
static bool g_sd_usage_refresh_pending;
static AudioInputSPDIF3 g_spdif_in;
static AiusConfig g_config;
static AiusConfig g_pending_config;
static bool g_config_upload_pending;
static uint32_t g_config_error_count;

/* The effective DSP model (source == "default" | "sd") and its runtime-reported
 * generation counter. g_active_model mirrors AIUS_OPTICAL_CONFIG's shape so the
 * GET serializer and apply_config() work identically whether the source is the
 * const factory default or a parsed SD override. Defaults to a plain copy of
 * AIUS_OPTICAL_CONFIG (a compile-time-constant aggregate, safe to read at static
 * init). g_config_generation is a per-boot-session counter: 0 at boot for the
 * default, incremented every time ANY new model (initial SD load, hot-reinsert
 * reload, or a PUT) is applied at an audio block boundary -- it is NOT persisted
 * across reboots (the SD file's own "config_generation" field is client-supplied
 * and untrusted; only "base_generation" on a PUT is a trusted concurrency
 * token). */
static FLASHMEM AiusOpticalConfig make_default_active_model(void)
{
	return AIUS_OPTICAL_CONFIG;
}
// Off the audio path (read/written only from apply_config()'s cold callers and
// the enqueue()-side block-boundary swap, never per-block), so DMAMEM (OCRAM)
// like g_http_request/g_http_response -- keeps this ~700-byte-each pair out of
// the tight shared FlexRAM/ITCM+DTCM+stack pool.
DMAMEM static AiusOpticalConfig g_active_model = make_default_active_model();
static bool g_dsp_source_sd;
static uint32_t g_config_generation;

/* Staged model awaiting a block-boundary-safe apply. Written from loop/HTTP
 * context (a validated PUT, or an SD (re)load); consumed once, at the top of
 * AiusAudioProcessor::enqueue() (audio-ISR context), which is the actual audio
 * block boundary. g_dsp_apply_pending is the single volatile handoff flag: the
 * writer always fully populates g_pending_dsp_model BEFORE setting it, so the
 * ISR never observes a partially-written model (see docs plan "Input trim/EQ +
 * limiter wiring done" note for the equivalent apply_config() out-of-line
 * rationale -- this is the same single-writer/single-reader flag idiom already
 * used elsewhere in this file, e.g. g_dhcp_discover_count). g_dsp_upload_pending
 * is the separate SD-job-lifecycle gate (mirrors g_config_upload_pending):
 * true only while a dsp_upload SD job is in flight for the currently staged
 * model, so a failed/cancelled job never arms g_dsp_apply_pending. DMAMEM for
 * the same reason as g_active_model: written from loop/HTTP context, read once
 * (a plain struct copy) at the block boundary, never touched per-block. */
DMAMEM static AiusOpticalConfig g_pending_dsp_model;
static bool g_dsp_upload_pending;
static volatile bool g_dsp_apply_pending;

static FLASHMEM void dsp_model_to_optical_config(const AiusDspModel &model, AiusOpticalConfig &out)
{
	out.sample_rate_hz = model.sample_rate_hz;
	out.features.input_eq = model.features.input_eq;
	out.features.matrix = model.features.matrix;
	out.features.output_eq = model.features.output_eq;
	out.features.limiter = model.features.limiter;
	out.features.meters = model.features.meters;
	for (size_t c = 0; c < AIUS_DSP_MODEL_INPUT_CHANNELS; c++) {
		out.input_channels[c].gain_db = model.input_channels[c].gain_db;
		out.input_channels[c].eq_count = model.input_channels[c].eq_count;
		for (size_t i = 0; i < AIUS_DSP_MODEL_EQ_MAX; i++) {
			out.input_channels[c].eq[i].type = model.input_channels[c].eq[i].type;
			out.input_channels[c].eq[i].frequency_hz = model.input_channels[c].eq[i].frequency_hz;
			out.input_channels[c].eq[i].q = model.input_channels[c].eq[i].q;
			out.input_channels[c].eq[i].gain_db = model.input_channels[c].eq[i].gain_db;
		}
	}
	out.output_eq_count = model.output_eq_count;
	for (size_t i = 0; i < AIUS_DSP_MODEL_EQ_MAX; i++) {
		out.output_eq[i].type = model.output_eq[i].type;
		out.output_eq[i].frequency_hz = model.output_eq[i].frequency_hz;
		out.output_eq[i].q = model.output_eq[i].q;
		out.output_eq[i].gain_db = model.output_eq[i].gain_db;
	}
	out.matrix.center_gain_linear = model.matrix.center_gain_linear;
	out.matrix.surround_gain_linear = model.matrix.surround_gain_linear;
	out.matrix.lfe_gain_linear = model.matrix.lfe_gain_linear;
	out.matrix.headroom_linear = model.matrix.headroom_linear;
	out.matrix.clipfree = model.matrix.clipfree;
	out.limiter.enabled = model.limiter.enabled;
	out.limiter.threshold_dbfs = model.limiter.threshold_dbfs;
	out.limiter.attack_ms = model.limiter.attack_ms;
	out.limiter.release_ms = model.limiter.release_ms;
	out.limiter.lookahead_ms = model.limiter.lookahead_ms;
}

/* Read and validate /aius/dsp.json (the SD override, written by a prior
 * successful PUT) and stage it for a block-boundary apply. A missing file is
 * the common case (no override configured) and is not an error. An oversized
 * or invalid file leaves the currently active model untouched -- called both
 * at boot (after SD.begin()) and on hot-reinsert (service_sd()), mirroring
 * load_sd_config()'s dual call sites. The audio pipeline is already running by
 * the time this runs (AudioMemory()/begin_pipeline() precede SD.begin() in
 * setup()), so this never mutates live DSP state directly -- same staged/
 * consumed-at-block-boundary path a runtime PUT uses. */
static FLASHMEM void stage_dsp_sd_override(void)
{
	File file = SD.open("/aius/dsp.json", FILE_READ);
	if (!file)
		return;
	uint8_t json[AIUS_DSP_MODEL_MAX_JSON_BYTES];
	size_t length = 0;
	while (file.available() && length < sizeof(json))
		json[length++] = (uint8_t)file.read();
	bool too_large = file.available();
	file.close();
	if (too_large)
		return;
	AiusDspModel parsed;
	if (aius_dsp_config_parse_put(json, length, &parsed) != AIUS_DSP_CONFIG_OK)
		return;
	dsp_model_to_optical_config(parsed, g_pending_dsp_model);
	g_dsp_apply_pending = true;
}
static char g_auth_token[AIUS_AUTH_TOKEN_STORAGE_SIZE];
static AiusUsbParser g_usb_parser;
static char g_usb_response[320];

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

static void watchdog_init(void)
{
	CCM_CCGR3 |= CCM_CCGR3_WDOG1(3);
	WDOG1_WMCR = 0;
	/* Sixteen seconds covers setup's SD probe plus the bounded DHCP attempt,
	 * while still recovering from a wedged driver or socket write. */
	constexpr uint8_t timeout = 31u;
	WDOG1_WCR |= WDOG_WCR_SRS | WDOG_WCR_WT(timeout);
	WDOG1_WCR |= WDOG_WCR_WDE | WDOG_WCR_WDT | WDOG_WCR_SRE;
	g_watchdog_enabled = true;
}

static void watchdog_feed(void)
{
	if (!g_watchdog_enabled)
		return;
	WDOG1_WSR = 0x5555;
	WDOG1_WSR = 0xAAAA;
}

static void service_hw_metrics(void)
{
	uint32_t now = millis();
	if ((int32_t)(now - g_cpu_temp_next_ms) < 0)
		return;
	g_cpu_temp_next_ms = now + 1000u;
	float temperature = tempmonGetTemp();
	if (temperature < -40.0f || temperature > 150.0f)
		return;
	g_cpu_temp_c = temperature;
	if (!g_cpu_temp_valid || temperature > g_cpu_temp_max_c)
		g_cpu_temp_max_c = temperature;
	g_cpu_temp_sample_ms = now;
	g_cpu_temp_valid = true;
}

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
	g_sd_job.progress_ms = 0;
	g_sd_job.target_existed = false;
	g_sd_job.backup_moved = false;
	g_sd_job.config_upload = false;
	g_sd_job.dsp_upload = false;
	g_sd_job.replay_upload = false;
	g_sd_job.replay_final = false;
	g_sd_job.replay_offset = 0;
	g_sd_job.replay_total = 0;
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

static void sd_job_abort_download(void)
{
	if (g_sd_job.file)
		g_sd_job.file.close();
	sd_job_drop_client();
	sd_job_clear();
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
	char body[128];
	int length = snprintf(body, sizeof(body),
	                      "{\"ok\":false,\"error\":\"%s\",\"received\":%lu,\"written\":%lu,\"expected\":%lu}",
	                      error, (unsigned long)g_sd_job.received,
	                      (unsigned long)g_sd_job.written,
	                      (unsigned long)g_sd_job.expected);
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
			/* FILE_WRITE is append mode. Replay upload validation already
			 * proved that the existing .part size equals replay_offset, so a
			 * seek to the exact end is both redundant and rejected by some
			 * SdFat/Arduino SD combinations. */
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
		{
			uint16_t remaining = g_sd_job.pending_length;
			uint16_t offset = 0;
			for (unsigned attempt = 0; remaining != 0 && attempt < 4u; attempt++) {
				size_t written = g_sd_job.file.write(g_sd_job.transfer + offset,
				                                    remaining);
				if (written > remaining)
					written = remaining;
				remaining = (uint16_t)(remaining - written);
				offset = (uint16_t)(offset + written);
				if (remaining != 0)
					delay(2);
			}
			if (remaining == 0) {
				g_sd_job.written += g_sd_job.pending_length;
				g_sd_job.pending_length = 0;
				g_sd_job.state = AIUS_SD_JOB_UPLOAD_RECEIVE;
				return;
			}
			g_sd_job.error_status = 507;
			g_sd_job.file.close();
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP;
			return;
		}
	case AIUS_SD_JOB_UPLOAD_CHECK_TARGET:
		if (g_sd_job.file)
			g_sd_job.file.close();
		if (g_sd_job.replay_upload && !g_sd_job.replay_final) {
			g_sd_job.state = AIUS_SD_JOB_UPLOAD_CLEANUP;
			return;
		}
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
		if (g_sd_job.replay_upload && !g_sd_job.replay_final) {
			char response[96];
			int length = snprintf(response, sizeof(response),
			                      "{\"ok\":true,\"next_offset\":%lu}",
			                      (unsigned long)(g_sd_job.replay_offset + g_sd_job.written));
			if (length > 0)
				http_send(200, (const uint8_t *)response, (size_t)length);
			sd_job_clear();
			return;
		}
		if (g_sd_job.backup_moved && !SD.remove(g_sd_job.backup_path))
			return;
		if (g_sd_job.config_upload && g_config_upload_pending) {
			g_config = g_pending_config;
			g_config_upload_pending = false;
		}
		if (g_sd_job.dsp_upload && g_dsp_upload_pending) {
			/* SD write is durable; the audio-side apply happens at the next
			 * block boundary (AiusAudioProcessor::enqueue), not here. */
			g_dsp_apply_pending = true;
			g_dsp_upload_pending = false;
		}
		g_sd_usage_refresh_pending = true;
		static const uint8_t upload_ok[] = "{\"ok\":true}";
		http_send(200, upload_ok, sizeof(upload_ok) - 1);
		sd_job_clear();
		return;
	case AIUS_SD_JOB_UPLOAD_FAIL_CLEANUP: {
		if (g_sd_job.file)
			g_sd_job.file.close();
		if (g_sd_job.temp_path[0] != 0 &&
		    !(g_sd_job.replay_upload && !g_sd_job.replay_final))
			(void)SD.remove(g_sd_job.temp_path);
		if (g_sd_job.error_status != 0) {
			if (g_sd_job.config_upload)
				g_config_upload_pending = false;
			if (g_sd_job.dsp_upload)
				g_dsp_upload_pending = false;
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
		if ((uint32_t)(millis() - g_sd_job.progress_ms) > AIUS_SD_DOWNLOAD_STALL_MS) {
			sd_job_abort_download();
			return;
		}
		size_t remaining = g_sd_job.response_length - g_sd_job.response_offset;
		int available = g_http_client.availableForWrite();
		if (available < 2)
			return;
		if ((size_t)available < remaining)
			remaining = (size_t)available;
		if (remaining > 1u)
			remaining /= 2u;
		size_t written = g_http_client.write(
			(const uint8_t *)g_sd_job.response_header + g_sd_job.response_offset,
			remaining);
		if (written == 0) {
			sd_job_abort_download();
			return;
		}
		g_sd_job.progress_ms = millis();
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
		if ((uint32_t)(millis() - g_sd_job.progress_ms) > AIUS_SD_DOWNLOAD_STALL_MS) {
			sd_job_abort_download();
			return;
		}
		int available = g_http_client.availableForWrite();
		if (available < 2)
			return;
		size_t length = g_sd_job.pending_length;
		if ((size_t)available < length)
			length = (size_t)available;
		if (length > 1u)
			length /= 2u;
		size_t written = g_http_client.write(g_sd_job.transfer, length);
		if (written == 0) {
			sd_job_abort_download();
			return;
		}
		g_sd_job.progress_ms = millis();
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
			stage_dsp_sd_override();
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
		apply_config(AIUS_OPTICAL_CONFIG);
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

	/* Resolve `cfg` (either AIUS_OPTICAL_CONFIG, the const factory default in
	 * FLASH, or a RAM-resident parsed SD override with the same field shape)
	 * into the runtime output-EQ biquads, downmix matrix, input-channel
	 * trim/EQ, and limiter. Cold path -- called at bring-up and, later, at a
	 * block-boundary config apply -- so it is FLASHMEM. The default config
	 * resolves to a bypass output EQ, the standard downmix, unity input trim
	 * with no EQ, and a disabled limiter, i.e. the audio path is bit-identical
	 * to the old flat-profile + standard-mix default.
	 *
	 * Defined out-of-line (below the class) rather than in-body: an in-body
	 * definition is implicitly inline (vague linkage / comdat), which conflicts
	 * at compile time with the plain FLASHMEM free functions already sharing the
	 * .flashmem section ("section type conflict") once the function is too large
	 * to fully inline away. Out-of-line gives it ordinary linkage instead. */
	FLASHMEM void apply_config(const AiusOpticalConfig &cfg);

protected:
	void update(void) override
	{
		audio_block_t *left = receiveReadOnly(0);
		audio_block_t *right = receiveReadOnly(1);
		if (g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY &&
		    g_pipeline != nullptr) {
			uint16_t words[AUDIO_BLOCK_SAMPLES * 2];
			size_t count = aius_replay_service_pop_words(&g_replay_service, words,
			                                             AUDIO_BLOCK_SAMPLES * 2);
			if (count == AUDIO_BLOCK_SAMPLES * 2) {
				AiusStatus st = aius_pipeline_push(g_pipeline, words, count);
				if (st != AIUS_OK) g_overruns++;
			} else {
				g_overruns++;
			}
		} else if (left != nullptr && right != nullptr && g_pipeline != nullptr) {
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
	AiusDownmixConfig g_downmix = aius_downmix_config_default();
	AiusBiquad g_eq[2];
	AiusBiquadState g_eq_state[4] = {};
	size_t g_eq_count = 0;
	/* Input stage: per-channel trim + up to two EQ sections, wired from
	 * AIUS_OPTICAL_CONFIG.input_channels[]. The scratch planes are hot per-block
	 * staging buffers input_stage_apply() copies decoded planes into before
	 * trim/EQ (see aius_dsp.h); sized to the largest block enqueue() accepts
	 * (AUDIO_BLOCK_SAMPLES * 4 frames) across all AIUS_MAX_CHANNELS planes. */
	AiusInputStage g_input_stage = aius_input_stage_default();
	AiusBiquad g_input_eq[AIUS_DSP_INPUT_CHANNELS][AIUS_DSP_INPUT_EQ_MAX] = {};
	AiusBiquadState g_input_eq_state[AIUS_DSP_INPUT_CHANNELS][AIUS_DSP_INPUT_EQ_MAX] = {};
	static constexpr uint32_t INPUT_SCRATCH_FRAMES = AUDIO_BLOCK_SAMPLES * 4;
	int32_t g_input_scratch[AIUS_MAX_CHANNELS][INPUT_SCRATCH_FRAMES];
	bool g_input_stage_active = false;
	AiusLimiter g_limiter = {};
	bool g_limiter_active = false;

	static AiusStatus consume_block(void *ctx, const AiusPcmBlock *block)
	{
		return static_cast<AiusAudioProcessor *>(ctx)->enqueue(block);
	}

	AiusStatus enqueue(const AiusPcmBlock *block)
	{
		/* Block-boundary config apply: the one point in the audio path where a
		 * staged model (a validated PUT or an SD (re)load) becomes active. Cheap
		 * in the common case (a single volatile bool read); apply_config() itself
		 * is bounded but not free (biquad coefficient trig), so this only runs
		 * once per staged change, never per block. */
		if (g_dsp_apply_pending) {
			g_active_model = g_pending_dsp_model;
			apply_config(g_active_model);
			g_dsp_source_sd = true;
			g_config_generation++;
			g_dsp_apply_pending = false;
		}
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
		dsp.input = g_input_stage_active ? &g_input_stage : nullptr;
		dsp.limiter = g_limiter_active ? &g_limiter : nullptr;
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

FLASHMEM void AiusAudioProcessor::apply_config(const AiusOpticalConfig &cfg)
{
	g_eq_count = cfg.output_eq_count;
	for (size_t i = 0; i < g_eq_count && i < 2; i++) {
		const AiusOpticalEqConfig &section = cfg.output_eq[i];
		if (section.type == 0)
			g_eq[i] = aius_biquad_bypass();
		else if (section.type == 1)
			g_eq[i] = aius_biquad_highpass(cfg.sample_rate_hz,
				section.frequency_hz, section.q);
		else
			g_eq[i] = aius_biquad_peaking(cfg.sample_rate_hz,
				section.frequency_hz, section.q, section.gain_db);
	}
	memset(g_eq_state, 0, sizeof(g_eq_state));
	g_downmix = aius_downmix_config_default();
	g_downmix.center = cfg.matrix.center_gain_linear;
	g_downmix.surround = cfg.matrix.surround_gain_linear;
	g_downmix.lfe = cfg.matrix.lfe_gain_linear;
	if (cfg.matrix.clipfree)
		g_downmix.headroom = aius_downmix_clipfree_headroom(&g_downmix);
	else
		g_downmix.headroom = cfg.matrix.headroom_linear;

	for (size_t c = 0; c < AIUS_DSP_INPUT_CHANNELS; c++) {
		const AiusOpticalInputChannelConfig &ic = cfg.input_channels[c];
		AiusInputChannelConfig &channel = g_input_stage.channels[c];
		channel.gain_linear = aius_db_to_linear(ic.gain_db);
		channel.eq_sections = ic.eq_count;
		for (size_t i = 0; i < ic.eq_count && i < AIUS_DSP_INPUT_EQ_MAX; i++) {
			const AiusOpticalEqConfig &section = ic.eq[i];
			if (section.type == 0)
				g_input_eq[c][i] = aius_biquad_bypass();
			else if (section.type == 1)
				g_input_eq[c][i] = aius_biquad_highpass(cfg.sample_rate_hz,
					section.frequency_hz, section.q);
			else
				g_input_eq[c][i] = aius_biquad_peaking(cfg.sample_rate_hz,
					section.frequency_hz, section.q, section.gain_db);
		}
		channel.eq = g_input_eq[c];
		channel.eq_state = g_input_eq_state[c];
	}
	memset(g_input_eq_state, 0, sizeof(g_input_eq_state));
	for (size_t p = 0; p < AIUS_MAX_CHANNELS; p++)
		g_input_stage.scratch[p] = g_input_scratch[p];
	g_input_stage_active = cfg.features.input_eq != 0;

	g_limiter.cfg.enabled = cfg.limiter.enabled;
	g_limiter.cfg.threshold_dbfs = cfg.limiter.threshold_dbfs;
	g_limiter.cfg.attack_ms = cfg.limiter.attack_ms;
	g_limiter.cfg.release_ms = cfg.limiter.release_ms;
	g_limiter.cfg.lookahead_ms = cfg.limiter.lookahead_ms;
	memset(&g_limiter.state, 0, sizeof(g_limiter.state));
	g_limiter_active = cfg.features.limiter != 0;
}

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
	/* Bytes 37/38 were the active mode/mix selection. The single-config model has
	 * no selection; byte 37 now reports the active config source (0 = the const
	 * factory default) and byte 38 is reserved. Both stay zero via the memset
	 * above, keeping the fixed 71-byte binary status schema unchanged. */
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

static FLASHMEM bool http_json_append(char *body, size_t capacity, size_t *length,
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

static FLASHMEM const char *http_dsp_eq_type(uint8_t type)
{
	switch (type) {
	case 0: return "bypass";
	case 1: return "highpass";
	case 2: return "peaking";
	default: return nullptr;
	}
}

/* Serialize one bounded EQ table (`sections` of `count`) as a JSON array. Cold
 * path (config read), so FLASHMEM. Returns false on capacity exhaustion or an
 * unknown section type. */
static FLASHMEM bool http_dsp_eq_array(char *output, size_t capacity, size_t *length,
                                       const AiusOpticalEqConfig *sections, uint8_t count)
{
	if (!http_json_append(output, capacity, length, "["))
		return false;
	for (uint8_t index = 0; index < count && index < 2u; index++) {
		const char *type = http_dsp_eq_type(sections[index].type);
		if (type == nullptr || !http_json_append(output, capacity, length,
			"%s{\"type\":\"%s\",\"frequency_hz\":%.17g,\"q\":%.17g,\"gain_db\":%.17g}",
			index == 0 ? "" : ",", type, sections[index].frequency_hz,
			sections[index].q, sections[index].gain_db))
			return false;
	}
	return http_json_append(output, capacity, length, "]");
}

/* Serialize the single effective DSP config: g_active_model, kept in sync with
 * whatever is actually running (the const factory default at boot, or the last
 * model applied at an audio block boundary). Cold path, fixed buffer,
 * capacity-checked; returns 0 (deterministic error) if the model cannot fit the
 * response budget. `source`/`config_generation` are runtime-reported, not part
 * of the model struct itself. */
static FLASHMEM size_t http_dsp_config_json(uint8_t *body, size_t capacity)
{
	static const char *const channel_names[6] = {"fl", "fr", "fc", "lfe", "sl", "sr"};
	const AiusOpticalConfig &cfg = g_active_model;
	if (capacity > AIUS_OPTICAL_MODEL_MAX_JSON_BYTES)
		capacity = AIUS_OPTICAL_MODEL_MAX_JSON_BYTES;
	char *output = (char *)body;
	size_t length = 0;
	if (!http_json_append(output, capacity, &length,
	                      "{\"source\":\"%s\",\"config_generation\":%lu,\"sample_rate_hz\":%lu,",
	                      g_dsp_source_sd ? "sd" : "default",
	                      (unsigned long)g_config_generation,
	                      (unsigned long)cfg.sample_rate_hz))
		return 0;
	if (!http_json_append(output, capacity, &length,
	                      "\"features\":{\"input_eq\":%s,\"matrix\":%s,\"output_eq\":%s,\"limiter\":%s,\"meters\":%s},",
	                      cfg.features.input_eq ? "true" : "false",
	                      cfg.features.matrix ? "true" : "false",
	                      cfg.features.output_eq ? "true" : "false",
	                      cfg.features.limiter ? "true" : "false",
	                      cfg.features.meters ? "true" : "false"))
		return 0;
	if (!http_json_append(output, capacity, &length, "\"input_channels\":{"))
		return 0;
	for (uint8_t channel = 0; channel < 6u; channel++) {
		const AiusOpticalInputChannelConfig &entry = cfg.input_channels[channel];
		if (!http_json_append(output, capacity, &length,
		                      "%s\"%s\":{\"gain_db\":%.17g,\"eq\":",
		                      channel == 0 ? "" : ",", channel_names[channel],
		                      entry.gain_db))
			return 0;
		if (!http_dsp_eq_array(output, capacity, &length, entry.eq, entry.eq_count))
			return 0;
		if (!http_json_append(output, capacity, &length, "}"))
			return 0;
	}
	if (!http_json_append(output, capacity, &length, "},\"output_eq\":"))
		return 0;
	if (!http_dsp_eq_array(output, capacity, &length, cfg.output_eq, cfg.output_eq_count))
		return 0;
	AiusDownmixConfig resolved = aius_downmix_config_default();
	resolved.center = cfg.matrix.center_gain_linear;
	resolved.surround = cfg.matrix.surround_gain_linear;
	resolved.lfe = cfg.matrix.lfe_gain_linear;
	double headroom = cfg.matrix.clipfree ?
		aius_downmix_clipfree_headroom(&resolved) : cfg.matrix.headroom_linear;
	if (!http_json_append(output, capacity, &length,
	                      ",\"matrix\":{\"center_gain_linear\":%.17g,\"surround_gain_linear\":%.17g,\"lfe_gain_linear\":%.17g,\"headroom_linear\":%.17g,\"clipfree\":%s},",
	                      cfg.matrix.center_gain_linear, cfg.matrix.surround_gain_linear,
	                      cfg.matrix.lfe_gain_linear, headroom,
	                      cfg.matrix.clipfree ? "true" : "false"))
		return 0;
	if (!http_json_append(output, capacity, &length,
	                      "\"limiter\":{\"enabled\":%s,\"threshold_dbfs\":%.17g,\"attack_ms\":%.17g,\"release_ms\":%.17g,\"lookahead_ms\":%.17g}}",
	                      cfg.limiter.enabled ? "true" : "false",
	                      cfg.limiter.threshold_dbfs, cfg.limiter.attack_ms,
	                      cfg.limiter.release_ms, cfg.limiter.lookahead_ms))
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
		"{\"version\":%u,\"state\":%u,\"carrier\":%u,\"format\":%u,\"sample_rate\":%lu,\"blocks\":%lu,\"overruns\":%lu,\"source\":\"default\",\"sd_present\":%u,\"sd_filesystem\":\"%s\",\"sd_total_bytes\":%llu,\"sd_used_bytes\":%llu,\"sd_free_bytes\":%llu,\"config_valid\":%u,\"config_error_code\":%u,\"config_error\":\"%s\",\"config_error_count\":%lu,\"network_mode\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"link\":%u,\"network_restarts\":%lu,\"dhcp_pending\":%u,\"dhcp_discovers\":%lu,\"dhcp_updates\":%lu,\"reset_source\":%lu,\"http_server_rebinds\":%lu,\"cpu_temp_c\":%.2f,\"cpu_temp_valid\":%u,\"cpu_temp_max_c\":%.2f,\"cpu_temp_age_ms\":%lu,\"build_revision\":\"%s\",\"build_utc\":\"%s\",\"event_sequence\":%lu,\"event_overflows\":%lu,\"network_drops\":%lu,\"network_budget_overruns\":%lu,\"http_requests\":%lu,\"http_rejections\":%lu,\"http_client_drops\":%lu,\"http_service_overruns\":%lu}",
		status[0], status[2], status[3] & 1u, status[23],
		(unsigned long)aius_net_get_u32(status + 25),
		(unsigned long)aius_net_get_u32(status + 29),
		(unsigned long)aius_net_get_u32(status + 33),
		g_sd_ready ? 1u : 0u, sd_filesystem(),
		(unsigned long long)g_sd_total_bytes,
		(unsigned long long)g_sd_used_bytes,
		(unsigned long long)(g_sd_total_bytes - g_sd_used_bytes),
		g_config.valid, (unsigned)g_config.error_code, g_config.error_message,
		(unsigned long)g_config_error_count,
		network_mode, status[12], status[13], status[14], status[15],
		(status[1] != 0 && Ethernet.linkStatus() == LinkON) ? 1u : 0u,
		(unsigned long)g_network_restarts, g_dhcp_pending ? 1u : 0u,
		(unsigned long)g_dhcp_discover_count,
		(unsigned long)g_dhcp_update_count,
		(unsigned long)g_reset_source,
		(unsigned long)g_http_server_rebinds,
		(double)g_cpu_temp_c, g_cpu_temp_valid ? 1u : 0u,
		(double)g_cpu_temp_max_c,
		g_cpu_temp_valid ? (unsigned long)(millis() - g_cpu_temp_sample_ms) : 0ul,
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
	uint32_t last_progress = millis();
	size_t offset = 0;
	for (; response_length != 0 && offset < response_length;) {
		if (!g_http_client || !g_http_client.connected() ||
			(uint32_t)(millis() - last_progress) > AIUS_HTTP_WRITE_STALL_MS)
			break;
		int available = g_http_client.availableForWrite();
		if (available < 1)
			continue;
		size_t length = response_length - offset;
		if (length > 512u)
			length = 512u;
		if ((size_t)available < length)
			length = (size_t)available;
		size_t written = g_http_client.write(g_http_response + offset, length);
		if (written == 0)
			continue;
		last_progress = millis();
		offset += written;
	}
	if (response_length != 0 && offset < response_length)
		g_http_client_drops++;
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
                                        bool config_upload, bool replay_upload,
                                        uint32_t expected,
                                        size_t initial_offset, size_t initial_end,
                                        bool dsp_upload = false)
{
	if (sd_job_active() || target == nullptr || content_type == nullptr ||
		strlen(target) >= sizeof(g_sd_job.target_path))
		return false;
	strncpy(g_sd_job.target_path, target, sizeof(g_sd_job.target_path));
	if (!sd_job_make_upload_paths(g_sd_job.target_path))
		return false;
	g_sd_job.content_type = content_type;
	g_sd_job.config_upload = config_upload;
	g_sd_job.dsp_upload = dsp_upload;
	g_sd_job.replay_upload = replay_upload;
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
	if (!http_sd_upload_begin_target(target, content_type, false,
	                                 request.route == AIUS_HTTP_ROUTE_SD_REPLAY,
	                                 expected, initial_offset, initial_end))
		return false;
	if (request.route == AIUS_HTTP_ROUTE_SD_REPLAY) {
		int part_length = snprintf(g_sd_job.temp_path, sizeof(g_sd_job.temp_path),
		                           "/aius/replays/%.*s.part",
		                           (int)request.replay_id_length, request.replay_id);
		if (part_length < 0 || (size_t)part_length >= sizeof(g_sd_job.temp_path))
			return false;
		g_sd_job.replay_offset = request.content_range_start;
		g_sd_job.replay_total = request.content_range_total;
		g_sd_job.replay_final = request.content_range_end + 1u == request.content_range_total;
		if (g_sd_job.replay_offset == 0u) {
			(void)SD.remove(g_sd_job.temp_path);
		} else {
			File partial = SD.open(g_sd_job.temp_path, FILE_READ);
			bool valid = partial && (uint32_t)partial.size() == g_sd_job.replay_offset;
			if (partial) partial.close();
			if (!valid) return false;
		}
	}
	return true;
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
		/* NativeEthernet can report bytes available immediately before a
		 * non-blocking read observes the socket transition. Treat that as
		 * temporary lack of progress; the receive idle timer owns timeout. */
		return true;
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
	if (request != nullptr && request->route == AIUS_HTTP_ROUTE_SD_REPLAY &&
	    request->replay_id != nullptr && request->replay_id_length > 0u &&
	    request->replay_id_length <= AIUS_HTTP_MAX_REPLAY_ID) {
		int written = snprintf(path, capacity, "/aius/replays/%.*s.iec",
		                       (int)request->replay_id_length, request->replay_id);
		if (written < 0 || (size_t)written >= capacity) return false;
		*content_type = "application/octet-stream";
		return true;
	}
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
		g_sd_job.progress_ms = millis();
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
								true, false, (uint32_t)length, g_http_body_offset,
	                                g_http_request_length)) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_busy\"}";
		http_send(503, body, sizeof(body) - 1);
		return;
	}
	g_pending_config = parsed;
	g_config_upload_pending = true;
	(void)request;
}

/* PUT /api/dsp/config: authenticated (X-Aius-SD-Auth), validated, bounded body.
 * On success this only makes the write to /aius/dsp.json durable and stages the
 * validated model -- it does NOT apply it. The actual apply (and
 * config_generation increment) happens at the next audio block boundary
 * (AiusAudioProcessor::enqueue), once service_sd_job() reaches
 * AIUS_SD_JOB_UPLOAD_CLEANUP for this job. Invalid input, a stale
 * base_generation, or an SD failure all leave both SD and the active DSP
 * snapshot untouched. */
static FLASHMEM void http_dsp_config_put_request(const AiusHttpRequest &request)
{
	if (!http_sd_authorized(request)) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_auth\"}";
		http_send(401, body, sizeof(body) - 1);
		return;
	}
	if (!g_sd_ready) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_unavailable\"}";
		http_send(503, body, sizeof(body) - 1);
		return;
	}
	size_t length = g_http_request_length - g_http_body_offset;
	AiusDspModel parsed;
	AiusDspConfigErrorCode error = aius_dsp_config_parse_put(
		g_http_request + g_http_body_offset, length, &parsed);
	if (error != AIUS_DSP_CONFIG_OK) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"dsp_config_invalid\"}";
		http_send(400, body, sizeof(body) - 1);
		return;
	}
	if (parsed.base_generation != g_config_generation) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"stale_generation\"}";
		http_send(409, body, sizeof(body) - 1);
		return;
	}
	/* The raw accepted body is written to SD verbatim (same pattern as
	 * config_upload above) -- it already carries "source":"sd" and a
	 * "base_generation" field, so it re-parses cleanly with the same
	 * aius_dsp_config_parse_put() at the next boot/hot-reinsert
	 * (stage_dsp_sd_override); the stale base_generation it then contains is
	 * simply never read for anything at that point. */
	if (!http_sd_upload_begin_target("/aius/dsp.json", "application/json",
	                                 false, false, (uint32_t)length,
	                                 g_http_body_offset, g_http_request_length,
	                                 true)) {
		static const uint8_t body[] = "{\"ok\":false,\"error\":\"sd_busy\"}";
		http_send(503, body, sizeof(body) - 1);
		return;
	}
	dsp_model_to_optical_config(parsed, g_pending_dsp_model);
	g_dsp_upload_pending = true;
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
		"<li><code>POST /api/start {}</code></li>"
		"<li><code>POST /api/stop {}</code></li>"
		"<li><code>POST /api/clear {}</code></li>"
		"<li><code>POST /api/reboot {}</code></li>"
		"<li><a href=\"/api/sd\">GET /api/sd</a></li>"
		"<li><code>PUT /sd/www/{path}</code> raw body with Content-Length and X-Aius-SD-Auth</li>"
		"<li><code>DELETE /sd/www/{path}</code> with X-Aius-SD-Auth</li>"
		"<li><code>PUT /api/config/public</code> JSON feature flags, no auth</li></ul>"
		"<h2>DSP configuration</h2><p><a href=\"/api/dsp/config\">GET /api/dsp/config</a> "
		"reports the single effective config: source, sample_rate_hz, features, input channels, output EQ, matrix, and limiter.</p>"
		"<pre id=dsp>Loading DSP configuration...</pre>"
		"<h2>SD web upload</h2><p>Get the token over USB with <code>AIUS AUTH</code>. "
		"The picker preserves relative paths and sends authenticated standard HTTP PUT requests.</p>"
		"<label>SD auth token <input id=t size=16 maxlength=16 autocomplete=off></label> "
		"<input id=f type=file multiple webkitdirectory directory><button onclick=upload()>Upload selected files</button>"
		"<pre id=u></pre>"
		"<h2>Control</h2><p>"
		"<button title=\"Enable test processing state\" onclick=go('/api/start')>Start</button> "
		"<button title=\"Disable test processing state\" onclick=go('/api/stop')>Stop</button> "
		"<button title=\"Clear audio and event counters\" onclick=go('/api/clear')>Clear counters</button> "
		"<button title=\"Restart the application without entering HalfKay\" onclick=reboot()>Reboot</button></p>"
		"<pre id=s>Loading status...</pre><p id=e></p></body><script>"
		"const s=document.getElementById('s'),e=document.getElementById('e'),u=document.getElementById('u'),d=document.getElementById('dsp');"
		"async function get(){try{s.textContent=JSON.stringify(await (await fetch('/api/status')).json(),null,2);e.textContent=''}catch(a){e.textContent=a}}"
		"async function getDsp(){try{const r=await fetch('/api/dsp/config');if(!r.ok)throw Error('HTTP '+r.status);d.textContent=JSON.stringify(await r.json(),null,2)}catch(a){d.textContent='DSP model unavailable: '+a}}"
		"async function go(a,b){try{await fetch(a,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});get()}catch(c){e.textContent=c}}"
		"async function reboot(){e.textContent='Rebooting...';try{await go('/api/reboot');setTimeout(get,2000)}catch(a){}}"
		"async function upload(){u.textContent='';const token=document.getElementById('t').value;const files=[...(document.getElementById('f').files||[])];for(const file of files){const rel=file.webkitRelativePath||file.name;const path=rel.split('/').slice(1).join('/')||rel;if(path.split('/').pop()==='config.json'){u.textContent+='refused '+path+'\\n';continue}try{const r=await fetch('/sd/www/'+path.split('/').map(encodeURIComponent).join('/'),{method:'PUT',headers:{'X-Aius-SD-Auth':token,'Content-Type':'application/octet-stream'},body:file});u.textContent+=r.status+' '+path+'\\n'}catch(a){u.textContent+='error '+path+' '+a+'\\n'}}}"
		"get();getDsp();setInterval(get,2000);setInterval(getDsp,2000)</script></html>",
		AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}

static size_t http_replay_status_json(uint8_t *body, size_t capacity)
{
	AiusReplayStatus replay = {};
	(void)aius_replay_service_status(&g_replay_service, &replay);
	const char *state = replay.state == AIUS_REPLAY_PLAYING ? "playing" :
		replay.state == AIUS_REPLAY_PREBUFFERING ? "prebuffering" :
		replay.state == AIUS_REPLAY_EOF ? "eof" :
		replay.state == AIUS_REPLAY_ERROR ? "error" : "stopped";
	const char *klass = replay.decoder.classification == AIUS_PIPELINE_DTS ? "dts" :
		replay.decoder.classification == AIUS_PIPELINE_PCM ? "pcm" : "unknown";
	int length = snprintf((char *)body, capacity,
		"{\"ok\":true,\"state\":\"%s\",\"id\":\"%s\",\"file_bytes\":%lu,\"offset_bytes\":%lu,\"duration_seconds\":%.3f,\"loop\":%s,\"loops_completed\":%lu,\"queue_words\":%lu,\"watermark_words\":%lu,\"underruns\":%lu,\"input_class\":\"%s\",\"decoder_errors\":%lu}",
		state, replay.id, (unsigned long)replay.file_bytes,
		(unsigned long)replay.offset_bytes, replay.duration_seconds,
		replay.loop ? "true" : "false", (unsigned long)replay.loops_completed,
		(unsigned long)replay.queue_words, (unsigned long)replay.watermark_words,
		(unsigned long)replay.underruns, klass, (unsigned long)replay.decoder_errors);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}

static size_t http_replay_list_json(uint8_t *body, size_t capacity)
{
	if (!g_sd_ready) return (size_t)snprintf((char *)body, capacity,
		"{\"ok\":false,\"error\":\"sd_unavailable\"}");
	File directory = SD.open("/aius/replays");
	if (!directory || !directory.isDirectory()) {
		if (directory) directory.close();
		return (size_t)snprintf((char *)body, capacity, "{\"ok\":true,\"files\":[]}");
	}
	size_t length = 0u;
	int written = snprintf((char *)body, capacity, "{\"ok\":true,\"files\":[");
	if (written < 0 || (size_t)written >= capacity) { directory.close(); return 0u; }
	length = (size_t)written;
	bool first = true;
	for (;;) {
		File entry = directory.openNextFile();
		if (!entry) break;
		if (!entry.isDirectory()) {
			const char *name = entry.name();
			const char *base = strrchr(name, '/');
			base = base == nullptr ? name : base + 1;
			size_t name_length = strlen(base);
			bool complete = name_length > 4u &&
				memcmp(base + name_length - 4u, ".iec", 4u) == 0;
			bool partial = name_length > 5u &&
				memcmp(base + name_length - 5u, ".part", 5u) == 0;
			if (complete || partial) {
				size_t suffix = complete ? 4u : 5u;
				size_t id_length = name_length - suffix;
				if (id_length <= AIUS_HTTP_MAX_REPLAY_ID) {
					char duration[24];
					snprintf(duration, sizeof(duration), complete ? "%.3f" : "null",
					         complete ? (double)entry.size() / 192000.0 : 0.0);
					written = snprintf((char *)body + length, capacity - length,
						"%s{\"id\":\"%.*s\",\"bytes\":%lu,\"duration_seconds\":%s,\"upload_next_offset\":%lu,\"complete\":%s,\"active\":%s}",
						first ? "" : ",", (int)id_length, base,
						(unsigned long)entry.size(), duration,
						(unsigned long)entry.size(), complete ? "true" : "false",
						(strlen(g_replay_service.id) == id_length &&
						 memcmp(base, g_replay_service.id, id_length) == 0 && complete) ?
						"true" : "false");
					if (written < 0 || (size_t)written >= capacity - length) {
						entry.close(); directory.close(); return 0u;
					}
					length += (size_t)written;
					first = false;
				}
			}
		}
		entry.close();
	}
	directory.close();
	if (length + 2u >= capacity) return 0u;
	body[length++] = ']'; body[length++] = '}'; body[length] = 0;
	return length;
}

static bool http_replay_body_id(char *id, size_t capacity, bool *loop)
{
	const char *body = (const char *)g_http_request + g_http_body_offset;
	const char *id_start = strstr(body, "\"id\":\"");
	if (id_start == nullptr) return false;
	id_start += 6;
	const char *id_end = strchr(id_start, '"');
	if (id_end == nullptr || (size_t)(id_end - id_start) == 0u ||
	    (size_t)(id_end - id_start) >= capacity) return false;
	memcpy(id, id_start, (size_t)(id_end - id_start));
	id[id_end - id_start] = '\0';
	*loop = strstr(body, "\"loop\":true") != nullptr;
	return aius_replay_id_is_valid(id);
}

static void http_replay_request(const AiusHttpRequest &request)
{
	uint8_t body[AIUS_HTTP_MAX_RESPONSE];
	if (!(request.route == AIUS_HTTP_ROUTE_REPLAYS_STATUS ||
	      (request.route == AIUS_HTTP_ROUTE_REPLAYS && request.method == AIUS_HTTP_GET)) &&
	    !http_sd_authorized(request)) {
		static const uint8_t unauthorized[] = "{\"ok\":false,\"error\":\"sd_auth\"}";
		http_send(401, unauthorized, sizeof(unauthorized) - 1u);
		return;
	}
	if (request.route == AIUS_HTTP_ROUTE_REPLAYS_STATUS) {
		size_t length = http_replay_status_json(body, sizeof(body));
		http_send(200, body, length);
		return;
	}
	if (request.route == AIUS_HTTP_ROUTE_REPLAYS && request.method == AIUS_HTTP_GET) {
		size_t length = http_replay_list_json(body, sizeof(body));
		http_send(length == 0u ? 500u : 200u, body, length);
		return;
	}
	if (request.route == AIUS_HTTP_ROUTE_REPLAYS && request.method == AIUS_HTTP_DELETE) {
		char id[AIUS_REPLAY_MAX_ID_LENGTH + 1u] = {};
		bool loop = false;
		(void)loop;
		if (!http_replay_body_id(id, sizeof(id), &loop)) {
			static const uint8_t bad[] = "{\"ok\":false,\"error\":\"replay_body\"}";
			http_send(400, bad, sizeof(bad) - 1u);
			return;
		}
		if (strcmp(id, g_replay_service.id) == 0 &&
		    g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY) {
			static const uint8_t active[] = "{\"ok\":false,\"error\":\"replay_active\"}";
			http_send(409, active, sizeof(active) - 1u);
			return;
		}
		char path[96];
		snprintf(path, sizeof(path), "/aius/replays/%s.iec", id);
		if (!SD.exists(path)) {
			static const uint8_t missing[] = "{\"ok\":false,\"error\":\"replay_not_found\"}";
			http_send(404, missing, sizeof(missing) - 1u);
			return;
		}
		if (!SD.remove(path)) {
			static const uint8_t failed[] = "{\"ok\":false,\"error\":\"replay_delete\"}";
			http_send(503, failed, sizeof(failed) - 1u);
			return;
		}
		int length = snprintf((char *)body, sizeof(body), "{\"ok\":true,\"deleted\":\"%s\"}", id);
		http_send(200, body, (size_t)length);
		return;
	}
	if (request.route == AIUS_HTTP_ROUTE_REPLAYS_STOP) {
		aius_replay_service_stop(&g_replay_service);
		static const uint8_t stopped[] = "{\"ok\":true,\"state\":\"stopped\"}";
		http_send(200, stopped, sizeof(stopped) - 1u);
		return;
	}
	if (request.route == AIUS_HTTP_ROUTE_REPLAYS_START) {
		char id[AIUS_REPLAY_MAX_ID_LENGTH + 1u] = {};
		bool loop = false;
		if (!g_sd_ready || !http_replay_body_id(id, sizeof(id), &loop)) {
			static const uint8_t bad[] = "{\"ok\":false,\"error\":\"replay_body\"}";
			http_send(400, bad, sizeof(bad) - 1u);
			return;
		}
		snprintf(g_replay_path, sizeof(g_replay_path), "/aius/replays/%s.iec", id);
		File file = SD.open(g_replay_path, FILE_READ);
		if (!file) { static const uint8_t missing[] = "{\"ok\":false,\"error\":\"replay_not_found\"}"; http_send(404, missing, sizeof(missing) - 1u); return; }
		uint32_t bytes = (uint32_t)file.size(); file.close();
		AiusStatus replay_status = aius_replay_service_start(&g_replay_service, id, bytes, loop, &g_replay_reader);
		if (replay_status != AIUS_OK) { static const uint8_t failed[] = "{\"ok\":false,\"error\":\"replay_start\"}"; http_send(409, failed, sizeof(failed) - 1u); return; }
		int length = snprintf((char *)body, sizeof(body), "{\"ok\":true,\"state\":\"prebuffering\",\"id\":\"%s\"}", id);
		http_send(200, body, (size_t)length);
		return;
	}
	static const uint8_t unsupported[] = "{\"ok\":false,\"error\":\"replay_method\"}";
	http_send(405, unsupported, sizeof(unsupported) - 1u);
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
	case AIUS_HTTP_ROUTE_REPLAYS:
	case AIUS_HTTP_ROUTE_REPLAYS_STATUS:
	case AIUS_HTTP_ROUTE_REPLAYS_START:
	case AIUS_HTTP_ROUTE_REPLAYS_STOP:
		http_replay_request(request);
		return;
	case AIUS_HTTP_ROUTE_SD_STATUS:
	case AIUS_HTTP_ROUTE_SD_FILE:
	case AIUS_HTTP_ROUTE_SD_REPLAY:
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
		if (request.method == AIUS_HTTP_PUT) {
			http_dsp_config_put_request(request);
			return;
		}
		body_length = http_dsp_config_json(g_dsp_config_body, sizeof(g_dsp_config_body));
		http_send_content(status, "application/json", g_dsp_config_body, body_length);
		return;
	case AIUS_HTTP_ROUTE_EVENTS:
		body_length = http_events_json(request.cursor, body, sizeof(body));
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
		bool transport_open = g_http_client &&
			(g_http_client.connected() || g_http_client.available());
		bool upload_waiting =
			g_sd_job.state == AIUS_SD_JOB_UPLOAD_OPEN ||
			g_sd_job.state == AIUS_SD_JOB_UPLOAD_RECEIVE;
		bool download_waiting =
			g_sd_job.state == AIUS_SD_JOB_DOWNLOAD_OPEN ||
			g_sd_job.state == AIUS_SD_JOB_DOWNLOAD_HEADER ||
			g_sd_job.state == AIUS_SD_JOB_DOWNLOAD_READ ||
			g_sd_job.state == AIUS_SD_JOB_DOWNLOAD_WRITE;
		bool upload_can_wait = upload_waiting &&
			(g_sd_job.pending_length != 0u ||
			 g_sd_job.received == g_sd_job.expected ||
			 (uint32_t)(millis() - g_http_last_byte_ms) <= AIUS_SD_UPLOAD_STALL_MS);
		bool download_can_wait = download_waiting &&
			(uint32_t)(millis() - g_sd_job.progress_ms) <= AIUS_SD_DOWNLOAD_STALL_MS;
		if (!transport_open && !upload_can_wait && !download_can_wait) {
			g_http_client_drops++;
			sd_job_cancel();
			sd_job_drop_client();
			return;
		}
		if (g_sd_job.state == AIUS_SD_JOB_UPLOAD_OPEN ||
		    g_sd_job.state == AIUS_SD_JOB_UPLOAD_RECEIVE) {
			http_sd_upload_receive_service();
			if (g_sd_job.received < g_sd_job.expected &&
			    g_sd_job.pending_length == 0u &&
			    (uint32_t)(millis() - g_http_last_byte_ms) > AIUS_SD_UPLOAD_STALL_MS) {
				g_http_client_drops++;
				sd_job_cancel();
				sd_job_drop_client();
			}
		}
		return;
	}
	if (!g_http_server) {
		g_http_server.begin();
		g_http_server_rebinds++;
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
			    (request.route == AIUS_HTTP_ROUTE_SD_FILE ||
			     request.route == AIUS_HTTP_ROUTE_SD_REPLAY) &&
			    request.method == AIUS_HTTP_PUT &&
			    http_sd_resource_is_config(request)) {
				static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"config_public_only\"}";
				http_send(403, error_body, sizeof(error_body) - 1);
			} else if (parse_result == AIUS_HTTP_OK &&
			           (request.route == AIUS_HTTP_ROUTE_SD_FILE ||
			            request.route == AIUS_HTTP_ROUTE_SD_REPLAY) &&
			           request.method == AIUS_HTTP_PUT &&
			           !http_sd_authorized(request)) {
				static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"sd_auth\"}";
				http_send(401, error_body, sizeof(error_body) - 1);
			} else if (parse_result == AIUS_HTTP_OK &&
			           (request.route == AIUS_HTTP_ROUTE_SD_FILE ||
			            request.route == AIUS_HTTP_ROUTE_SD_REPLAY) &&
			           request.method == AIUS_HTTP_PUT && !g_sd_ready) {
				static const uint8_t error_body[] = "{\"ok\":false,\"error\":\"sd_unavailable\"}";
				http_send(503, error_body, sizeof(error_body) - 1);
			} else if (parse_result == AIUS_HTTP_OK &&
			           (request.route == AIUS_HTTP_ROUTE_SD_FILE ||
			            request.route == AIUS_HTTP_ROUTE_SD_REPLAY) &&
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

static void service_usb_control(void)
{
	while (Serial.available()) {
		int value = Serial.read();
		if (value < 0)
			return;
		AiusUsbEvent event;
		if (!aius_usb_control_feed(&g_usb_parser, (uint8_t)value, &event))
			continue;
		size_t response_length;
		if (event.type == AIUS_USB_EVENT_ERROR) {
			response_length = aius_usb_control_write_error(g_usb_response,
			                                             sizeof(g_usb_response), 0,
			                                             event.error);
		} else {
			const uint32_t request_id = event.command.request_id;
			switch (event.command.kind) {
			case AIUS_USB_COMMAND_PROTOCOL:
				response_length = aius_usb_control_write_ok(g_usb_response,
				                                             sizeof(g_usb_response),
				                                             request_id, "PROTOCOL", "1");
				break;
			case AIUS_USB_COMMAND_ID: {
				char payload[192];
				IPAddress ip = Ethernet.localIP();
				snprintf(payload, sizeof(payload),
						"protocol=1 revision=%s build=%s mode=optical ip=%u.%u.%u.%u link=%u dhcp_pending=%u dhcp_discovers=%lu dhcp_updates=%lu",
				         AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC,
				         ip[0], ip[1], ip[2], ip[3],
				         (unsigned)Ethernet.linkStatus(), g_dhcp_pending ? 1u : 0u,
				         (unsigned long)g_dhcp_discover_count,
				         (unsigned long)g_dhcp_update_count);
				response_length = aius_usb_control_write_ok(g_usb_response,
				                                             sizeof(g_usb_response),
				                                             request_id, "ID", payload);
				break;
			}
			case AIUS_USB_COMMAND_AUTH:
				response_length = aius_usb_control_write_ok(g_usb_response,
				                                             sizeof(g_usb_response),
				                                             request_id, "AUTH", g_auth_token);
				break;
			case AIUS_USB_COMMAND_BOOTLOADER:
				response_length = aius_usb_control_write_ok(g_usb_response,
				                                             sizeof(g_usb_response),
				                                             request_id, "BOOTLOADER", "entering");
				Serial.write((const uint8_t *)g_usb_response, response_length);
				Serial.flush();
				delay(20);
				_reboot_Teensyduino_();
				return;
			default:
				response_length = aius_usb_control_write_error(g_usb_response,
				                                             sizeof(g_usb_response), 0,
				                                             AIUS_USB_ERROR_INTERNAL);
				break;
			}
		}
		if (response_length != 0)
			Serial.write((const uint8_t *)g_usb_response, response_length);
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
	case AIUS_NET_CMD_SET_MIX:
		/* Mode/mix selection is retired by the single-config model. The command
		 * IDs remain in the binary protocol, but there is no selection to set, so
		 * the device rejects them rather than silently no-op-acking. */
		send_nack(request.request_id, AIUS_NET_ERR_TYPE);
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

static bool ensure_dhcp_client(bool restart)
{
	fnet_netif_desc_t netif = fnet_netif_get_default();
	if (netif == nullptr)
		return false;
	fnet_dhcp_cln_desc_t client = fnet_dhcp_cln_get_by_netif(netif);
	if (client != nullptr && fnet_dhcp_cln_is_enabled(client)) {
		if (!restart)
			return true;
		fnet_dhcp_cln_release(client);
	}
	static fnet_dhcp_cln_params_t params = {};
	params.netif = netif;
	client = fnet_dhcp_cln_init(&params);
	if (client == nullptr)
		return false;
	fnet_dhcp_cln_set_response_timeout(client, 250);
	fnet_dhcp_cln_set_callback_discover(
		client,
		[](fnet_dhcp_cln_desc_t, fnet_netif_desc_t, void *) {
			g_dhcp_discover_count++;
		},
		nullptr);
	fnet_dhcp_cln_set_callback_updated(
		client,
		[](fnet_dhcp_cln_desc_t, fnet_netif_desc_t, void *) {
			g_dhcp_update_count++;
		},
		nullptr);
	return true;
}

static void activate_network(bool dhcp)
{
	g_dhcp = dhcp;
	g_dhcp_pending = false;
	g_dhcp_attempts = 0;
	g_network_ready = true;
	g_udp.begin(AIUS_NET_PORT);
	g_http_server.begin();
	Serial.printf("AIUS Ethernet ready: dhcp=%u ip=%u.%u.%u.%u link=%u\\n",
	              g_dhcp ? 1u : 0u, Ethernet.localIP()[0], Ethernet.localIP()[1],
	              Ethernet.localIP()[2], Ethernet.localIP()[3],
	              (unsigned)Ethernet.linkStatus());
}

static void reset_network_service(void)
{
	if (sd_job_active())
		sd_job_clear();
	if (g_http_client)
		g_http_client.stop();
	g_http_client = EthernetClient();
	g_http_request_length = 0;
	g_udp.stop();
	g_network_ready = false;
	g_dhcp = false;
	g_dhcp_pending = false;
	g_ethernet_initialized = false;
	g_dhcp_attempts = 0;
	g_network_link_down_at = 0;
	g_network_retry_at = millis() + AIUS_NETWORK_RETRY_MS;
	g_network_restarts++;
}

static void supervise_network_link(void)
{
	if (!g_ethernet_initialized || !g_network_ready)
		return;
	if (Ethernet.linkStatus() != LinkOFF) {
		g_network_link_down_at = 0;
		return;
	}
	uint32_t now = millis();
	if (g_network_link_down_at == 0)
		g_network_link_down_at = now;
	else if ((uint32_t)(now - g_network_link_down_at) >=
		         AIUS_NETWORK_LINK_DEBOUNCE_MS)
		reset_network_service();
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
		if (g_dhcp_attempts < 2u) {
			g_ethernet_initialized = false;
			g_network_retry_at = millis();
			return;
		}
	}
	if (g_ethernet_initialized && Ethernet.linkStatus() == LinkOFF) {
		g_network_retry_at = millis() + 5000u;
		return;
	}
	if (g_ethernet_initialized && Ethernet.linkStatus() == LinkON) {
		if (g_dhcp_attempts < 2u) {
			g_ethernet_initialized = false;
			g_network_retry_at = millis();
			return;
		}
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
	Serial.printf("AIUS Ethernet init: DHCP begin reset_source=%lu\\n",
	              (unsigned long)g_reset_source);
	g_ethernet_initialized = true;
	g_dhcp_attempts++;
	/* Keep the synchronous library probe below the watchdog budget. The FNET
	 * client below owns the subsequent bounded retry cycle. */
	g_dhcp = Ethernet.begin(g_mac, 1000, 250) != 0;
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
	if (ensure_dhcp_client(true)) {
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
	g_reset_source = SRC_SRSR & 0xffu;
	SRC_SRSR = g_reset_source;
	watchdog_init();
	Serial.begin(115200);
	aius_usb_control_init(&g_usb_parser);
	uint8_t auth_material[AIUS_AUTH_DEVICE_MATERIAL_SIZE];
	uint32_t mac0 = HW_OCOTP_MAC0;
	uint32_t mac1 = HW_OCOTP_MAC1;
	uint32_t mac2 = HW_OCOTP_MAC2;
	memcpy(auth_material, &mac0, sizeof(mac0));
	memcpy(auth_material + 4u, &mac1, sizeof(mac1));
	memcpy(auth_material + 8u, &mac2, sizeof(mac2));
	memcpy(auth_material + 12u, &mac0, sizeof(mac0));
	(void)aius_auth_derive_token(auth_material, g_auth_token);
	aius_replay_service_init(&g_replay_service);
	aius_config_set_defaults(&g_config);
	AudioMemory(24);
	g_audio_processor.begin_pipeline();
	g_boot_epoch = micros();
	g_sd_ready = SD.begin(BUILTIN_SDCARD);
	if (g_sd_ready) {
		(void)SD.mkdir("/aius");
		load_sd_config();
		stage_dsp_sd_override();
	}
	refresh_sd_usage();
	Serial.printf("AIUS SD ready=%u\\n", g_sd_ready ? 1u : 0u);
	watchdog_feed();
	start_network();
}

void loop(void)
{
	service_usb_control();
	service_hw_metrics();
	supervise_network_link();
	if (g_network_ready) {
		service_http();
		/* HTTP is the recovery/control plane. Do not let a UDP/FNET poll stall
		 * starve it, and establish a watchdog checkpoint before entering the
		 * secondary discovery service. */
		watchdog_feed();
		service_network();
	} else if ((int32_t)(millis() - g_network_retry_at) >= 0) {
		start_network();
	}
	if (g_dhcp || g_dhcp_pending)
		(void)Ethernet.maintain();
	if (g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY ||
	    g_replay_service.state == AIUS_REPLAY_PREBUFFERING)
		(void)aius_replay_service_refill(&g_replay_service, 2048u);
	service_sd();
	watchdog_feed();
}
