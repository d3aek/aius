/* optical-app oracle: one bounded application dispatch per HTTP route and USB
 * command.
 *
 * The fake application below is the whole seam: it owns no socket, no SD
 * handle, no clock and no delay, so a dispatch that reached for transport, SD,
 * the watchdog or a delay could not compile against it. Each test drives the
 * public entry points with requests produced by the real parser and asserts the
 * exact status, content type and body the pre-cutover firmware produces.
 */

#include "aius_optical_app.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define RUN_TEST(test) do { \
	fprintf(stderr, "optical-app oracle: %s\n", #test); \
	test(); \
} while (0)

#define LOG_ENTRIES 32u
#define LOG_STORAGE 40u

/* The fake card may hold more than one listing's worth, so the cap is provable
 * from the outside. */
#define FAKE_DIR_ENTRIES (AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES + 16u)
#define FAKE_DIR_NAME_STORAGE 64u

static const char *const AUTH_TOKEN = "token";

typedef struct {
	char name[FAKE_DIR_NAME_STORAGE];
	uint64_t size;
	bool is_directory;
} FakeDirEntry;

typedef struct {
	bool sd_ready;
	bool sd_busy;
	bool serializers_fit;
	AiusOpticalAppStoreResult public_store;
	AiusOpticalAppStoreResult dsp_store;
	AiusOpticalAppReplayResult replay_start_result;
	AiusOpticalAppReplayResult replay_delete_result;
	size_t usb_payload_length;
	char usb_payload[AIUS_OPTICAL_APP_USB_PAYLOAD_STORAGE + 8u];
	uint32_t upload_id;

	/* What the card holds for the next listing, what the application could not
	 * represent, and where it was asked to look. */
	FakeDirEntry dir_entries[FAKE_DIR_ENTRIES];
	size_t dir_entry_count;
	size_t dir_hidden;
	AiusOpticalAppDirResult dir_result;
	char dir_path[AIUS_SD_PATH_STORAGE];

	char log[LOG_ENTRIES][LOG_STORAGE];
	size_t log_count;
	uint32_t last_cursor;
	char last_replay_id[AIUS_OPTICAL_APP_REPLAY_ID_STORAGE];
	bool last_replay_loop;
	bool test_running;
	size_t stored_length;
	unsigned reboots;
	unsigned clears;
	unsigned stops;

	/* What the last measure_start carried, and whether to accept it. */
	bool measure_start_result;
	char last_measure_id[AIUS_OPTICAL_APP_REPLAY_ID_STORAGE];
	uint32_t last_measure_f0_l;
	uint32_t last_measure_f0_r;
	uint32_t last_measure_probes[AIUS_OPTICAL_APP_MEASURE_PROBES_MAX];
	size_t last_measure_probe_count;

	/* Recorder: whether the state machine accepts, what config arrived,
	 * what read window was asked for, and the bytes a read serves. */
	bool recorder_accept;
	AiusOpticalAppRecorderReadResult recorder_read_result;
	uint32_t last_recorder_mask;
	float last_recorder_gr_db;
	uint32_t last_recorder_hold;
	uint32_t last_recorder_postroll;
	uint32_t last_recorder_offset;
	uint32_t last_recorder_length;
	uint32_t last_recorder_generation;
	uint8_t recorder_data[16];
	uint32_t recorder_data_length;

	/* OTA: whether the surface exists at all, the verdict each verb hands
	 * back, what the last chunk carried, and the document the open read
	 * publishes. The fake holds no session -- the dispatcher holds no OTA
	 * state either, so a verdict per call is the whole seam. */
	bool ota_present;
	bool ota_status_ok;
	AiusOpticalAppOtaResult ota_chunk_result;
	AiusOpticalAppOtaResult ota_arm_result;
	AiusOpticalAppOtaResult ota_verify_result;
	AiusOpticalAppOtaResult ota_abort_result;
	AiusOpticalAppOtaResult ota_activate_result;
	AiusOpticalAppOtaResult ota_enable_activation_result;
	AiusOpticalAppOtaResult ota_self_test_result;
	AiusOpticalAppOtaResult ota_rehearse_result;
	bool last_ota_rehearse_injected;
	uint32_t ota_next_offset;
	uint32_t last_ota_offset;
	size_t last_ota_length;
	const uint8_t *last_ota_data;
	AiusOpticalAppOtaStatus ota_document;
} Fake;

typedef struct {
	Fake fake;
	AiusOpticalAppDispatcher dispatcher;
	uint8_t body[AIUS_HTTP_MAX_RESPONSE];
	uint8_t raw[AIUS_HTTP_MAX_REQUEST];
	AiusHttpRequest request;
} Fixture;

static void record(Fake *fake, const char *name)
{
	assert(fake->log_count < LOG_ENTRIES);
	assert(strlen(name) < LOG_STORAGE);
	snprintf(fake->log[fake->log_count], LOG_STORAGE, "%s", name);
	fake->log_count++;
}

/* ------------------------------------------------------------------ */
/* fake application                                                     */
/* ------------------------------------------------------------------ */

static bool fake_sd_ready(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "sd_ready");
	return fake->sd_ready;
}

static bool fake_sd_busy(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "sd_busy");
	return fake->sd_busy;
}

static bool fake_auth_matches(void *context, const uint8_t *token, size_t length)
{
	Fake *fake = (Fake *)context;
	record(fake, "auth_matches");
	return token != NULL && length == strlen(AUTH_TOKEN) &&
	       memcmp(token, AUTH_TOKEN, length) == 0;
}

static size_t fake_serialize(Fake *fake, const char *name, uint8_t *body,
                             size_t capacity)
{
	int written;
	record(fake, name);
	if (!fake->serializers_fit)
		return 0u;
	written = snprintf((char *)body, capacity, "{\"body\":\"%s\"}", name);
	assert(written > 0 && (size_t)written < capacity);
	return (size_t)written;
}

static size_t fake_write_home(void *context, uint8_t *body, size_t capacity)
{
	return fake_serialize((Fake *)context, "home", body, capacity);
}

static size_t fake_write_status(void *context, uint8_t *body, size_t capacity)
{
	return fake_serialize((Fake *)context, "status", body, capacity);
}

static size_t fake_write_events(void *context, uint32_t cursor, uint8_t *body,
                                size_t capacity)
{
	Fake *fake = (Fake *)context;
	fake->last_cursor = cursor;
	return fake_serialize(fake, "events", body, capacity);
}

static size_t fake_write_dsp_config(void *context, uint8_t *body, size_t capacity)
{
	return fake_serialize((Fake *)context, "dsp_config", body, capacity);
}

static size_t fake_write_meters(void *context, uint8_t *body, size_t capacity)
{
	return fake_serialize((Fake *)context, "meters", body, capacity);
}

static size_t fake_write_replay_status(void *context, uint8_t *body,
                                       size_t capacity)
{
	return fake_serialize((Fake *)context, "replay_status", body, capacity);
}

static size_t fake_write_replay_list(void *context, uint8_t *body,
                                     size_t capacity)
{
	return fake_serialize((Fake *)context, "replay_list", body, capacity);
}

static AiusOpticalAppStoreResult fake_store_public_config(void *context,
                                                          const uint8_t *body,
                                                          size_t length)
{
	Fake *fake = (Fake *)context;
	record(fake, "store_public_config");
	assert(length == 0u || body != NULL);
	fake->stored_length = length;
	return fake->public_store;
}

static AiusOpticalAppStoreResult fake_store_dsp_config(void *context,
                                                       const uint8_t *body,
                                                       size_t length)
{
	Fake *fake = (Fake *)context;
	record(fake, "store_dsp_config");
	assert(length == 0u || body != NULL);
	fake->stored_length = length;
	return fake->dsp_store;
}

/* One bounded batch, exactly as the header specifies it: fill up to `capacity`,
 * report what was left behind, never touch `entries` or `capacity`. A name too
 * long for the entry storage is counted in `hidden` by the application, which is
 * what the real adapter must do -- the dispatcher's own rejection path is
 * exercised separately, by returning a name it will refuse to link. */
static AiusOpticalAppDirResult fake_read_directory(void *context,
                                                   const char *path,
                                                   AiusOpticalAppDirBatch *batch)
{
	Fake *fake = (Fake *)context;
	record(fake, "read_directory");
	snprintf(fake->dir_path, sizeof(fake->dir_path), "%s", path);
	assert(batch->entries != NULL);
	assert(batch->capacity == AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES);
	if (fake->dir_result != AIUS_OPTICAL_APP_DIR_OK)
		return fake->dir_result;
	batch->hidden = fake->dir_hidden;
	for (size_t i = 0; i < fake->dir_entry_count; ++i) {
		AiusOpticalAppDirEntry *entry;
		if (batch->count == batch->capacity) {
			batch->more = true;
			break;
		}
		entry = &batch->entries[batch->count];
		snprintf(entry->name, sizeof(entry->name), "%s",
		         fake->dir_entries[i].name);
		entry->size_bytes = fake->dir_entries[i].size;
		entry->is_directory = fake->dir_entries[i].is_directory;
		batch->count++;
	}
	return AIUS_OPTICAL_APP_DIR_OK;
}

static AiusOpticalAppReplayResult fake_replay_start(void *context,
                                                    const char *id, bool loop)
{
	Fake *fake = (Fake *)context;
	record(fake, "replay_start");
	snprintf(fake->last_replay_id, sizeof(fake->last_replay_id), "%s", id);
	fake->last_replay_loop = loop;
	return fake->replay_start_result;
}

static AiusOpticalAppReplayResult fake_replay_delete(void *context,
                                                     const char *id)
{
	Fake *fake = (Fake *)context;
	record(fake, "replay_delete");
	snprintf(fake->last_replay_id, sizeof(fake->last_replay_id), "%s", id);
	return fake->replay_delete_result;
}

static void fake_replay_stop(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "replay_stop");
	fake->stops++;
}

static void fake_set_test_running(void *context, bool running)
{
	Fake *fake = (Fake *)context;
	record(fake, running ? "test_running:true" : "test_running:false");
	fake->test_running = running;
}

static void fake_clear_counters(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "clear_counters");
	fake->clears++;
}

static void fake_request_reboot(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "request_reboot");
	fake->reboots++;
}

static size_t fake_write_usb_payload(void *context, AiusUsbCommandKind kind,
                                     char *payload, size_t capacity)
{
	Fake *fake = (Fake *)context;
	size_t length = fake->usb_payload_length;
	record(fake, kind == AIUS_USB_COMMAND_ID ? "usb_payload:id" :
	             "usb_payload:auth");
	if (length == 0u)
		return 0u;
	if (length >= capacity)
		return length; /* the dispatcher must reject an over-long claim */
	memcpy(payload, fake->usb_payload, length);
	payload[length] = '\0';
	return length;
}

static uint32_t fake_upload_id(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "upload_id");
	return fake->upload_id;
}

static size_t fake_write_measure(void *context, uint8_t *body, size_t capacity)
{
	return fake_serialize((Fake *)context, "measure", body, capacity);
}

static bool fake_measure_start(void *context, const char *vector_id,
                               uint32_t f0_l_hz, uint32_t f0_r_hz,
                               const uint32_t *probes_hz, size_t probe_count)
{
	Fake *fake = (Fake *)context;
	record(fake, "measure_start");
	snprintf(fake->last_measure_id, sizeof(fake->last_measure_id), "%s",
	         vector_id);
	fake->last_measure_f0_l = f0_l_hz;
	fake->last_measure_f0_r = f0_r_hz;
	assert(probe_count <= AIUS_OPTICAL_APP_MEASURE_PROBES_MAX);
	memcpy(fake->last_measure_probes, probes_hz,
	       probe_count * sizeof(uint32_t));
	fake->last_measure_probe_count = probe_count;
	return fake->measure_start_result;
}

static size_t fake_write_recorder(void *context, uint8_t *body, size_t capacity)
{
	return fake_serialize((Fake *)context, "recorder", body, capacity);
}

static bool fake_recorder_freeze(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "recorder_freeze");
	return fake->recorder_accept;
}

static bool fake_recorder_thaw(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "recorder_thaw");
	return fake->recorder_accept;
}

static bool fake_recorder_config(void *context, uint32_t mask,
                                 float gr_threshold_db, uint32_t gr_hold_blocks,
                                 uint32_t postroll_blocks)
{
	Fake *fake = (Fake *)context;
	record(fake, "recorder_config");
	fake->last_recorder_mask = mask;
	fake->last_recorder_gr_db = gr_threshold_db;
	fake->last_recorder_hold = gr_hold_blocks;
	fake->last_recorder_postroll = postroll_blocks;
	return fake->recorder_accept;
}

static AiusOpticalAppRecorderReadResult fake_recorder_read(
	void *context, uint32_t offset_bytes, uint32_t length_bytes,
	uint32_t generation, const uint8_t **data, uint32_t *length)
{
	Fake *fake = (Fake *)context;
	record(fake, "recorder_read");
	fake->last_recorder_offset = offset_bytes;
	fake->last_recorder_length = length_bytes;
	fake->last_recorder_generation = generation;
	if (fake->recorder_read_result == AIUS_OPTICAL_APP_RECORDER_READ_OK) {
		*data = fake->recorder_data;
		*length = fake->recorder_data_length;
	}
	return fake->recorder_read_result;
}

static bool fake_ota_present(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_present");
	return fake->ota_present;
}

static bool fake_ota_status(void *context, AiusOpticalAppOtaStatus *status)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_status");
	if (!fake->ota_status_ok)
		return false;
	*status = fake->ota_document;
	return true;
}

static AiusOpticalAppOtaResult fake_ota_chunk(void *context, uint32_t offset,
                                              const uint8_t *data,
                                              size_t length,
                                              uint32_t *next_offset)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_chunk");
	/* The route must hand down a real, bounded slice of the request body. */
	assert(data != NULL);
	assert(length != 0u && length <= AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES);
	assert(next_offset != NULL);
	fake->last_ota_offset = offset;
	fake->last_ota_length = length;
	fake->last_ota_data = data;
	*next_offset = fake->ota_next_offset;
	return fake->ota_chunk_result;
}

static AiusOpticalAppOtaResult fake_ota_arm(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_arm");
	return fake->ota_arm_result;
}

static AiusOpticalAppOtaResult fake_ota_verify(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_verify");
	return fake->ota_verify_result;
}

static AiusOpticalAppOtaResult fake_ota_abort(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_abort");
	return fake->ota_abort_result;
}

static AiusOpticalAppOtaResult fake_ota_activate(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_activate");
	return fake->ota_activate_result;
}

static AiusOpticalAppOtaResult fake_ota_enable_activation(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_enable_activation");
	return fake->ota_enable_activation_result;
}

static AiusOpticalAppOtaResult fake_ota_self_test_flash(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_self_test_flash");
	return fake->ota_self_test_result;
}

static AiusOpticalAppOtaResult fake_ota_rehearse(void *context,
                                                 bool inject_fault)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_rehearse");
	fake->last_ota_rehearse_injected = inject_fault;
	return fake->ota_rehearse_result;
}

static AiusOpticalAppOta fake_ota(Fake *fake)
{
	AiusOpticalAppOta ota;
	memset(&ota, 0, sizeof(ota));
	ota.context = fake;
	ota.present = fake_ota_present;
	ota.status = fake_ota_status;
	ota.chunk = fake_ota_chunk;
	ota.arm = fake_ota_arm;
	ota.verify = fake_ota_verify;
	ota.abort = fake_ota_abort;
	ota.activate = fake_ota_activate;
	ota.enable_activation = fake_ota_enable_activation;
	ota.self_test_flash = fake_ota_self_test_flash;
	ota.rehearse = fake_ota_rehearse;
	return ota;
}

static AiusOpticalApp fake_app(Fake *fake)
{
	AiusOpticalApp app;
	memset(&app, 0, sizeof(app));
	app.context = fake;
	app.sd_ready = fake_sd_ready;
	app.sd_busy = fake_sd_busy;
	app.auth_matches = fake_auth_matches;
	app.write_home = fake_write_home;
	app.write_status = fake_write_status;
	app.write_events = fake_write_events;
	app.write_dsp_config = fake_write_dsp_config;
	app.write_meters = fake_write_meters;
	app.write_replay_status = fake_write_replay_status;
	app.write_replay_list = fake_write_replay_list;
	app.store_public_config = fake_store_public_config;
	app.store_dsp_config = fake_store_dsp_config;
	app.read_directory = fake_read_directory;
	app.replay_start = fake_replay_start;
	app.replay_delete = fake_replay_delete;
	app.replay_stop = fake_replay_stop;
	app.set_test_running = fake_set_test_running;
	app.clear_counters = fake_clear_counters;
	app.request_reboot = fake_request_reboot;
	app.write_usb_payload = fake_write_usb_payload;
	app.upload_id = fake_upload_id;
	app.write_measure = fake_write_measure;
	app.measure_start = fake_measure_start;
	app.write_recorder = fake_write_recorder;
	app.recorder_freeze = fake_recorder_freeze;
	app.recorder_thaw = fake_recorder_thaw;
	app.recorder_config = fake_recorder_config;
	app.recorder_read = fake_recorder_read;
	return app;
}

/* ------------------------------------------------------------------ */
/* fixture                                                              */
/* ------------------------------------------------------------------ */

/* The OTA document a permissive application publishes: an upgrade halfway
 * through staging, signed by the committed test-only key the plan ships as the
 * device key for now. Every field is the widest plausible value rather than a
 * placeholder, so the golden below is a document a real board could produce. */
static void fixture_init_ota_document(Fake *fake)
{
	AiusOpticalAppOtaStatus *document = &fake->ota_document;
	memset(document, 0, sizeof(*document));
	document->state = AIUS_OPTICAL_APP_OTA_STATE_STAGING;
	document->latch_armed = true;
	document->staged_bytes = 4096u;
	document->total_bytes = 585728u; /* _flashimagelen, per the plan's §Sizing */
	document->next_offset = 4096u;
	snprintf(document->target_id, sizeof(document->target_id),
	         "teensy41-optical");
	snprintf(document->build_revision, sizeof(document->build_revision),
	         "fecc4a0a2473");
	document->build_utc = 1753900000u;
	snprintf(document->key_fingerprint, sizeof(document->key_fingerprint),
	         "9f86d081884c7d65");
	document->test_key = true;
	/* The runtime activation latch: false at every boot, and this document is a
	 * board that has NOT yet had it flipped -- the default the activate route
	 * answers 409 from. test_ota_status_document_activation_available drives it
	 * true to prove the field tracks the latch. */
	document->activation_available = false;
	document->fault = AIUS_OPTICAL_APP_OTA_OK;
	document->verifier_fault = 0;
}

/* Installs the OTA seam on an already-initialised dispatcher. Separate from
 * fake_app() because the table is separate: aius_optical_app_init() zeroes the
 * slot, so a fixture rebuilt around a different body buffer must re-install. */
static void fixture_wire_ota(Fixture *fixture)
{
	AiusOpticalAppOta ota = fake_ota(&fixture->fake);
	assert(aius_optical_app_set_ota(&fixture->dispatcher, &ota));
}

static void fixture_init(Fixture *fixture)
{
	AiusOpticalApp app;
	memset(fixture, 0, sizeof(*fixture));
	fixture->fake.sd_ready = true;
	fixture->fake.sd_busy = false;
	fixture->fake.serializers_fit = true;
	/* The delivered store callbacks validate into RAM and write nothing, so a
	 * good body is ACCEPTED and the dispatcher owns the write. */
	fixture->fake.public_store = AIUS_OPTICAL_APP_STORE_ACCEPTED;
	fixture->fake.dsp_store = AIUS_OPTICAL_APP_STORE_ACCEPTED;
	fixture->fake.replay_start_result = AIUS_OPTICAL_APP_REPLAY_OK;
	fixture->fake.replay_delete_result = AIUS_OPTICAL_APP_REPLAY_OK;
	fixture->fake.measure_start_result = true;
	fixture->fake.recorder_accept = true;
	fixture->fake.recorder_read_result = AIUS_OPTICAL_APP_RECORDER_READ_OK;
	memcpy(fixture->fake.recorder_data, "0123456789abcdef", 16u);
	fixture->fake.recorder_data_length = 16u;
	fixture->fake.dir_result = AIUS_OPTICAL_APP_DIR_OK;
	fixture->fake.upload_id = 0x1234abcdu;
	snprintf(fixture->fake.usb_payload, sizeof(fixture->fake.usb_payload),
	         "protocol=1 revision=test");
	fixture->fake.usb_payload_length = strlen(fixture->fake.usb_payload);
	fixture->fake.ota_present = true;
	fixture->fake.ota_status_ok = true;
	fixture->fake.ota_chunk_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_arm_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_verify_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_abort_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_activate_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_enable_activation_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_self_test_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_rehearse_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture_init_ota_document(&fixture->fake);
	app = fake_app(&fixture->fake);
	assert(aius_optical_app_init(&fixture->dispatcher, &app, fixture->body,
	                             sizeof(fixture->body)));
	fixture_wire_ota(fixture);
}

static void fixture_reset_log(Fixture *fixture)
{
	fixture->fake.log_count = 0;
}

/* Builds one request, runs it through the real parser and clears the call log,
 * so every golden is anchored to bytes a client could actually send. */
static void parse_request(Fixture *fixture, const char *start_line,
                          const char *headers, const char *body,
                          long declared_length)
{
	size_t body_length = body == NULL ? 0u : strlen(body);
	int written;
	AiusHttpResult result;
	char length_header[64];
	length_header[0] = '\0';
	if (declared_length >= 0)
		snprintf(length_header, sizeof(length_header),
		         "Content-Length: %ld\r\n", declared_length);
	else if (body_length != 0u)
		snprintf(length_header, sizeof(length_header),
		         "Content-Length: %zu\r\n", body_length);
	written = snprintf((char *)fixture->raw, sizeof(fixture->raw),
	                   "%s HTTP/1.0\r\n%s%s\r\n%s", start_line,
	                   headers == NULL ? "" : headers, length_header,
	                   body == NULL ? "" : body);
	assert(written > 0 && (size_t)written < sizeof(fixture->raw));
	result = aius_http_parse(fixture->raw, (size_t)written, &fixture->request);
	if (result != AIUS_HTTP_OK)
		fprintf(stderr, "unexpected parse result %d for \"%s\"\n", (int)result,
		        start_line);
	assert(result == AIUS_HTTP_OK);
	fixture_reset_log(fixture);
}

static const AiusHttpApplicationResponse *dispatch_request(
	Fixture *fixture, const char *start_line, const char *headers,
	const char *body, long declared_length)
{
	parse_request(fixture, start_line, headers, body, declared_length);
	return aius_optical_app_http_dispatch(&fixture->dispatcher,
	                                      &fixture->request);
}

static const AiusHttpApplicationResponse *dispatch_get(Fixture *fixture,
                                                       const char *start_line)
{
	return dispatch_request(fixture, start_line, NULL, NULL, -1);
}

static void expect_response(const AiusHttpApplicationResponse *response,
                            AiusHttpApplicationResponseKind kind,
                            unsigned status, const char *content_type,
                            const char *body)
{
	assert(response != NULL);
	if (response->status != status || response->kind != kind)
		fprintf(stderr, "expected kind %d status %u, actual kind %d status %u\n",
		        (int)kind, status, (int)response->kind, response->status);
	assert(response->kind == kind);
	assert(response->status == status);
	assert(response->content_type != NULL);
	assert(strcmp(response->content_type, content_type) == 0);
	if (body == NULL) {
		assert(response->body == NULL);
		assert(response->body_length == 0u);
		return;
	}
	assert(response->body != NULL);
	if (response->body_length != strlen(body) ||
	    memcmp(response->body, body, response->body_length) != 0)
		fprintf(stderr, "expected body \"%s\", actual \"%.*s\"\n", body,
		        (int)response->body_length, (const char *)response->body);
	assert(response->body_length == strlen(body));
	assert(memcmp(response->body, body, response->body_length) == 0);
}

static void expect_immediate(const AiusHttpApplicationResponse *response,
                             unsigned status, const char *body)
{
	expect_response(response, AIUS_HTTP_APP_IMMEDIATE, status,
	                "application/json", body);
	/* An immediate answer must leave the SD request completely zeroed; the
	 * transaction rejects the response otherwise. */
	assert(response->sd_job.kind == AIUS_SD_JOB_PROBE);
	assert(response->sd_job.target_path == NULL);
	assert(response->sd_job.temp_path == NULL);
	assert(response->sd_job.backup_path == NULL);
	assert(response->sd_job.content_type == NULL);
	assert(response->sd_job.total == 0u);
	assert(response->sd_job.initial_offset == 0u);
	assert(!response->sd_job.publish);
	assert(response->sd_job.publish_capacity == 0u);
	assert(!response->sd_job.require_existing);
}

/* An accepted config PUT answers with the write itself: an UPLOAD/REPLACE job
 * whose 200 the transaction formats now but holds until the commit lands, so
 * the pre-cutover 500 sd_commit / 507 sd_write surface survives. `total` is the
 * one field accept_application cross-checks against the transaction's
 * body_expected, and content_type must stay NULL on a non-publishing upload or
 * sd_request_fields_valid refuses the whole response. */
static void expect_config_job(const AiusHttpApplicationResponse *response,
                              const char *target, unsigned long sequence,
                              uint64_t total)
{
	char temp[AIUS_SD_PATH_STORAGE];
	char backup[AIUS_SD_PATH_STORAGE];
	snprintf(temp, sizeof(temp), "/aius/.upload-1234abcd-%08lx.tmp", sequence);
	snprintf(backup, sizeof(backup), "/aius/.backup-1234abcd-%08lx.bak",
	         sequence);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                "{\"ok\":true}");
	assert(response->sd_job.kind == AIUS_SD_JOB_UPLOAD);
	assert(response->sd_job.install_mode == AIUS_SD_INSTALL_REPLACE);
	/* Publishing the written bytes back would prove nothing the store callback
	 * has not already proved by parsing them. */
	assert(!response->sd_job.publish);
	assert(response->sd_job.publish_capacity == 0u);
	assert(!response->sd_job.require_existing);
	assert(response->sd_job.content_type == NULL);
	assert(response->sd_job.initial_offset == 0u);
	assert(response->sd_job.total == total);
	assert(strcmp(response->sd_job.target_path, target) == 0);
	assert(strcmp(response->sd_job.temp_path, temp) == 0);
	assert(strcmp(response->sd_job.backup_path, backup) == 0);
}

static void expect_log(const Fixture *fixture, const char *const *expected,
                       size_t count)
{
	if (fixture->fake.log_count != count) {
		fprintf(stderr, "expected %zu callbacks, actual %zu:", count,
		        fixture->fake.log_count);
		for (size_t i = 0; i < fixture->fake.log_count; ++i)
			fprintf(stderr, " %s", fixture->fake.log[i]);
		fprintf(stderr, "\n");
	}
	assert(fixture->fake.log_count == count);
	for (size_t i = 0; i < count; ++i) {
		if (strcmp(fixture->fake.log[i], expected[i]) != 0)
			fprintf(stderr, "callback %zu: expected %s, actual %s\n", i,
			        expected[i], fixture->fake.log[i]);
		assert(strcmp(fixture->fake.log[i], expected[i]) == 0);
	}
}

/* ------------------------------------------------------------------ */
/* tests                                                                */
/* ------------------------------------------------------------------ */

static void test_init_validation(void)
{
	Fake fake;
	AiusOpticalApp app;
	AiusOpticalAppDispatcher dispatcher;
	uint8_t body[AIUS_OPTICAL_APP_MIN_BODY];
	AiusHttpRequest request;
	AiusUsbApplicationRequest usb;

	memset(&fake, 0, sizeof(fake));
	memset(&request, 0, sizeof(request));
	memset(&usb, 0, sizeof(usb));
	app = fake_app(&fake);

	assert(!aius_optical_app_init(NULL, &app, body, sizeof(body)));
	assert(!aius_optical_app_init(&dispatcher, NULL, body, sizeof(body)));
	assert(!dispatcher.valid);
	assert(!aius_optical_app_init(&dispatcher, &app, NULL, sizeof(body)));
	assert(!aius_optical_app_init(&dispatcher, &app, body,
	                              AIUS_OPTICAL_APP_MIN_BODY - 1u));

	/* Every callback is required; a hole must be refused at init, never
	 * discovered as a null call during dispatch. */
	for (size_t i = 0; i < 27u; ++i) {
		AiusOpticalApp holed = fake_app(&fake);
		void **slots[] = {
			(void **)&holed.sd_ready, (void **)&holed.sd_busy,
			(void **)&holed.auth_matches, (void **)&holed.write_home,
			(void **)&holed.write_status, (void **)&holed.write_events,
			(void **)&holed.write_dsp_config, (void **)&holed.write_meters,
			(void **)&holed.write_replay_status,
			(void **)&holed.write_replay_list,
			(void **)&holed.store_public_config,
			(void **)&holed.store_dsp_config, (void **)&holed.replay_start,
			(void **)&holed.replay_delete, (void **)&holed.replay_stop,
			(void **)&holed.set_test_running, (void **)&holed.clear_counters,
			(void **)&holed.request_reboot, (void **)&holed.write_usb_payload,
			(void **)&holed.upload_id, (void **)&holed.write_measure,
			(void **)&holed.measure_start, (void **)&holed.write_recorder,
			(void **)&holed.recorder_freeze, (void **)&holed.recorder_thaw,
			(void **)&holed.recorder_config, (void **)&holed.recorder_read
		};
		assert(ARRAY_LENGTH(slots) == 27u);
		*slots[i] = NULL;
		assert(!aius_optical_app_init(&dispatcher, &holed, body, sizeof(body)));
		assert(aius_optical_app_http_dispatch(&dispatcher, &request) == NULL);
		assert(aius_optical_app_usb_dispatch(&dispatcher, &usb) == NULL);
	}

	assert(aius_optical_app_init(&dispatcher, &app, body, sizeof(body)));
	assert(dispatcher.valid);
	assert(aius_optical_app_http_dispatch(NULL, &request) == NULL);
	assert(aius_optical_app_http_dispatch(&dispatcher, NULL) == NULL);
	assert(aius_optical_app_usb_dispatch(NULL, &usb) == NULL);
	assert(aius_optical_app_usb_dispatch(&dispatcher, NULL) == NULL);
}

/* Every route that answers from a serializer or a constant, with a permissive
 * application. */
static void test_golden_immediate_routes(void)
{
	Fixture fixture;
	fixture_init(&fixture);

	expect_response(dispatch_get(&fixture, "GET /"), AIUS_HTTP_APP_IMMEDIATE,
	                200, "text/html; charset=utf-8", "{\"body\":\"home\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/status"), 200,
	                 "{\"body\":\"status\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/events"), 200,
	                 "{\"body\":\"events\"}");
	assert(fixture.fake.last_cursor == 0u);
	expect_immediate(dispatch_get(&fixture, "GET /api/events?cursor=42"), 200,
	                 "{\"body\":\"events\"}");
	assert(fixture.fake.last_cursor == 42u);
	expect_immediate(dispatch_get(&fixture, "GET /api/meters"), 200,
	                 "{\"body\":\"meters\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/measure"), 200,
	                 "{\"body\":\"measure\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/dsp/config"), 200,
	                 "{\"body\":\"dsp_config\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/sd"), 200,
	                 "{\"ready\":true}");
	expect_immediate(dispatch_get(&fixture, "GET /api/replays/status"), 200,
	                 "{\"body\":\"replay_status\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/replays"), 200,
	                 "{\"body\":\"replay_list\"}");

	/* The four control routes carry the token: they mutate the running system,
	 * so they are authenticated like every other mutating route. Their refusals
	 * are test_control_routes_are_authenticated below. */
	expect_immediate(dispatch_request(&fixture, "POST /api/start",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true,\"running\":true}");
	assert(fixture.fake.test_running);
	expect_immediate(dispatch_request(&fixture, "POST /api/stop",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true,\"running\":false}");
	assert(!fixture.fake.test_running);
	expect_immediate(dispatch_request(&fixture, "POST /api/clear",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true}");
	assert(fixture.fake.clears == 1u);
	expect_immediate(dispatch_request(&fixture, "POST /api/reboot",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true,\"rebooting\":true}");
	assert(fixture.fake.reboots == 1u);
}

/* The negative control for the security half of defect 12.
 *
 * Before this fix POST /api/start, /api/stop, /api/clear and /api/reboot
 * performed no auth_matches call and appeared in no auth-gated arm, so any host
 * that could reach port 80 could stop the audio path or reset the board --
 * including mid-upload. Every 401 row below answers 200 against that version and
 * performs the effect, which is what makes this a control rather than a
 * restatement.
 *
 * Absent and wrong are separate rows because they reach the callback
 * differently: an absent header leaves request->sd_auth NULL, a wrong one leaves
 * it pointing at bytes that do not match, and only the callback can tell those
 * apart. Both must be one 401 sd_auth, the same body the SD, replay and config
 * routes already refuse with.
 *
 * The read-only block at the end is the other half of the contract, and just as
 * load-bearing: GET /, /api/status, /api/events and /api/meters are deliberately
 * open because the recovery smoke runner polls them to decide whether the board
 * came back. Gating them would break the tooling and protect nothing. */
static void test_control_routes_are_authenticated(void)
{
	Fixture fixture;
	static const struct { const char *line; const char *body; } control[] = {
		{ "POST /api/start", "{\"ok\":true,\"running\":true}" },
		{ "POST /api/stop", "{\"ok\":true,\"running\":false}" },
		{ "POST /api/clear", "{\"ok\":true}" },
		{ "POST /api/reboot", "{\"ok\":true,\"rebooting\":true}" }
	};
	static const char *const public_routes[] = {
		"GET /api/status", "GET /api/events", "GET /api/meters",
		"GET /api/measure"
	};
	static const char *const refused_calls[] = { "auth_matches" };
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(control); ++i) {
		expect_immediate(dispatch_request(&fixture, control[i].line, NULL,
		                                  NULL, -1),
		                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
		expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
		expect_immediate(dispatch_request(&fixture, control[i].line,
		                                  "X-Aius-SD-Auth: wrong\r\n", NULL,
		                                  -1),
		                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
		expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	}

	/* The status code is not the property; the absent effect is. Eight
	 * refusals, and the test state never started, no counter was cleared and
	 * no reset was requested. */
	assert(!fixture.fake.test_running);
	assert(fixture.fake.clears == 0u);
	assert(fixture.fake.reboots == 0u);

	/* The correct token still serves all four, so the gate refuses rather than
	 * breaks the route. */
	for (size_t i = 0; i < ARRAY_LENGTH(control); ++i)
		expect_immediate(dispatch_request(&fixture, control[i].line,
		                                  "X-Aius-SD-Auth: token\r\n", NULL,
		                                  -1),
		                 200, control[i].body);
	assert(fixture.fake.clears == 1u);
	assert(fixture.fake.reboots == 1u);
	/* start then stop, so the flag lands back where it began. */
	assert(!fixture.fake.test_running);

	/* And the open routes stay open: served with no header at all, and no
	 * auth_matches call to gate them. */
	expect_response(dispatch_get(&fixture, "GET /"), AIUS_HTTP_APP_IMMEDIATE,
	                200, "text/html; charset=utf-8", "{\"body\":\"home\"}");
	assert(fixture.fake.log_count == 1u);
	assert(strcmp(fixture.fake.log[0], "home") == 0);
	for (size_t i = 0; i < ARRAY_LENGTH(public_routes); ++i) {
		(void)dispatch_get(&fixture, public_routes[i]);
		assert(fixture.fake.log_count == 1u);
		assert(strcmp(fixture.fake.log[0], "auth_matches") != 0);
	}
}

static void test_serializer_exhaustion(void)
{
	Fixture fixture;
	static const char *const json_routes[] = {
		"GET /api/status", "GET /api/events", "GET /api/meters",
		"GET /api/dsp/config", "GET /api/replays/status", "GET /api/replays"
	};
	fixture_init(&fixture);
	fixture.fake.serializers_fit = false;

	/* The home page keeps its HTML failure body; every JSON route shares one
	 * deterministic 500. */
	expect_response(dispatch_get(&fixture, "GET /"), AIUS_HTTP_APP_IMMEDIATE,
	                500, "text/html; charset=utf-8",
	                "<p>response_too_large</p>");
	for (size_t i = 0; i < ARRAY_LENGTH(json_routes); ++i)
		expect_immediate(dispatch_get(&fixture, json_routes[i]), 500,
		                 "{\"ok\":false,\"error\":\"response_too_large\"}");
}

static void test_sd_media_and_paths(void)
{
	Fixture fixture;
	/* "GET /sd/www/" is absent deliberately: a trailing slash names a directory
	 * and is the index route (test_sd_index), not a rejected file path. */
	static const char *const rejected[] = {
		"GET /sd/x",
		"GET /sd/etc/passwd",
		"GET /sd/www/../secret",
		"GET /sd/www/a//b",
		"GET /sd/www/%2e%2e/x",
		"GET /sd/www/sub/../../x"
	};
	/* max_age pins the dispatcher's caching policy per suffix: day-long for
	 * the two stable asset kinds (.js/.css), zero for everything else --
	 * html so a card sync propagates, json because it is live data, binary
	 * because it covers presets and replay vectors. */
	static const struct {
		const char *line;
		const char *type;
		uint32_t max_age;
	} accepted[] = {
		{ "GET /sd/www/index.html", "text/html; charset=utf-8", 0u },
		{ "GET /sd/www/app.js", "application/javascript", 86400u },
		{ "GET /sd/www/app.css", "text/css", 86400u },
		{ "GET /sd/www/model.json", "application/json", 0u },
		{ "GET /sd/www/logo.png", "application/octet-stream", 0u },
		{ "GET /sd/www/a/b/c/d.bin", "application/octet-stream", 0u }
	};
	fixture_init(&fixture);

	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_get(&fixture, "GET /api/sd"), 503,
	                 "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/index.html"), 503,
	                 "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	fixture.fake.sd_ready = true;

	for (size_t i = 0; i < ARRAY_LENGTH(rejected); ++i)
		expect_immediate(dispatch_get(&fixture, rejected[i]), 404,
		                 "{\"ok\":false,\"error\":\"sd_path\"}");

	for (size_t i = 0; i < ARRAY_LENGTH(accepted); ++i) {
		const AiusHttpApplicationResponse *response =
			dispatch_get(&fixture, accepted[i].line);
		expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, accepted[i].type,
		                NULL);
		assert(response->sd_job.kind == AIUS_SD_JOB_DOWNLOAD);
		assert(response->sd_job.content_type != NULL);
		assert(strcmp(response->sd_job.content_type, accepted[i].type) == 0);
		assert(response->sd_job.temp_path == NULL);
		assert(response->sd_job.backup_path == NULL);
		assert(!response->sd_job.publish);
		assert(!response->sd_job.require_existing);
		assert(response->cache_max_age_s == accepted[i].max_age);
	}

	{
		const AiusHttpApplicationResponse *response =
			dispatch_get(&fixture, "GET /sd/www/index.html");
		assert(strcmp(response->sd_job.target_path, "/aius/www/index.html") == 0);
	}
	{
		const AiusHttpApplicationResponse *response =
			dispatch_get(&fixture, "GET /sd/replays/take1");
		expect_response(response, AIUS_HTTP_APP_SD_JOB, 200,
		                "application/octet-stream", NULL);
		assert(response->sd_job.kind == AIUS_SD_JOB_DOWNLOAD);
		assert(strcmp(response->sd_job.target_path,
		              "/aius/replays/take1.iec") == 0);
		/* Replay vectors are re-listed and re-fetched by id; never cached. */
		assert(response->cache_max_age_s == 0u);
	}

	/* The /aius/<name> builder the config routes now share, at its limit: the
	 * longest resource the parser and the /sd/ rules admit is 96 bytes (three
	 * 30-byte components), and "/aius/" + that is 102 of the 128-byte path
	 * storage. */
	{
		static const char *const segment = "abcdefghijabcdefghijabcdefghij";
		char line[160];
		char expected[AIUS_SD_PATH_STORAGE];
		const AiusHttpApplicationResponse *response;
		snprintf(line, sizeof(line), "GET /sd/www/%s/%s/%s", segment, segment,
		         segment);
		snprintf(expected, sizeof(expected), "/aius/www/%s/%s/%s", segment,
		         segment, segment);
		assert(strlen(expected) == 102u);
		response = dispatch_get(&fixture, line);
		expect_response(response, AIUS_HTTP_APP_SD_JOB, 200,
		                "application/octet-stream", NULL);
		assert(strcmp(response->sd_job.target_path, expected) == 0);
	}

	fixture.fake.sd_busy = true;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/index.html"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	fixture.fake.sd_busy = false;
}

/* The one resource the download route serves from outside www/.
 *
 * Every row is a negative control against the version before the carve-out: it
 * answered 404 sd_path to "GET /sd/aius/dsp.json" as well, which is exactly what
 * fails the latency plan's bypass row closed -- the card's authoritative DSP
 * model could not be retrieved over the API at all, so no restorable artifact
 * existed to bracket a mutating PUT with.
 *
 * The carve-out is one exact literal and a read. The refusal rows below are the
 * property that matters: the model is reachable by its own URL and by nothing
 * else, and no write of any kind leaves www/. */
static void test_sd_dsp_model_carveout(void)
{
	Fixture fixture;
	const AiusHttpApplicationResponse *response;
	char carveout_target[AIUS_SD_PATH_STORAGE];
	AiusHttpApplicationResponse carveout;
	/* Neither the carve-out's sibling on the card, nor the model under any
	 * spelling but the exact literal: a traversal that resolves to it, a
	 * trailing slash, a doubled separator, a case change, a percent escape,
	 * an extra or a missing byte, and the two directory forms around it. */
	static const char *const rejected[] = {
		"GET /sd/aius/config.json",
		"GET /sd/config.json",
		"GET /sd/dsp.json",
		"GET /sd/aius/../aius/dsp.json",
		"GET /sd/www/../aius/dsp.json",
		"GET /sd/aius//dsp.json",
		"GET /sd/AIUS/dsp.json",
		"GET /sd/aius/dsp%2ejson",
		"GET /sd/aius/dsp.jsonx",
		"GET /sd/aius/dsp.jso",
		"GET /sd/aius/replays/take1.iec",
		"GET /sd/aius/dsp.json/",
		"GET /sd/aius/"
	};
	/* A download reads the card and takes no credential, so the whole call
	 * sequence is the media gate and the volume gate. No auth_matches. */
	static const char *const download_calls[] = { "sd_ready", "sd_busy" };
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(rejected); ++i)
		expect_immediate(dispatch_get(&fixture, rejected[i]), 404,
		                 "{\"ok\":false,\"error\":\"sd_path\"}");

	/* The carve-out itself: the pre-cutover /aius/dsp.json, verbatim, as live
	 * JSON the browser must not cache. */
	response = dispatch_get(&fixture, "GET /sd/aius/dsp.json");
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                NULL);
	assert(response->sd_job.kind == AIUS_SD_JOB_DOWNLOAD);
	assert(strcmp(response->sd_job.target_path, "/aius/dsp.json") == 0);
	assert(response->sd_job.content_type != NULL);
	assert(strcmp(response->sd_job.content_type, "application/json") == 0);
	assert(response->cache_max_age_s == 0u);
	/* A download installs nothing: no scratch pair, no publish, no length the
	 * dispatcher claims to know. */
	assert(response->sd_job.temp_path == NULL);
	assert(response->sd_job.backup_path == NULL);
	assert(!response->sd_job.publish);
	assert(!response->sd_job.require_existing);
	assert(response->sd_job.total == 0u);
	expect_log(&fixture, download_calls, ARRAY_LENGTH(download_calls));

	/* Field for field the job a .json download under www/ produces, target
	 * aside.
	 * That equivalence is how the absent-file answer is proved here: this seam
	 * never sees the card, and the transaction maps a DOWNLOAD that cannot be
	 * opened to 404 sd_not_found (aius_http_transaction.c select_sd_error), so
	 * an identical job necessarily gets an identical answer. The target string
	 * is copied first -- it borrows the dispatcher's path buffer, which the
	 * next dispatch overwrites. */
	carveout = *response;
	snprintf(carveout_target, sizeof(carveout_target), "%s",
	         response->sd_job.target_path);
	response = dispatch_get(&fixture, "GET /sd/www/model.json");
	assert(strcmp(carveout_target, "/aius/dsp.json") == 0);
	assert(strcmp(response->sd_job.target_path, "/aius/www/model.json") == 0);
	assert(carveout.kind == response->kind);
	assert(carveout.status == response->status);
	assert(carveout.cache_max_age_s == response->cache_max_age_s);
	assert(strcmp(carveout.content_type, response->content_type) == 0);
	assert(carveout.sd_job.kind == response->sd_job.kind);
	assert(carveout.sd_job.install_mode == response->sd_job.install_mode);
	assert(carveout.sd_job.require_existing == response->sd_job.require_existing);
	assert(carveout.sd_job.publish == response->sd_job.publish);
	assert(carveout.sd_job.total == response->sd_job.total);

	/* Same volume gates as every other download -- an absent card is not a
	 * path failure, and nothing reads the card while a job owns it. */
	fixture.fake.sd_busy = true;
	expect_immediate(dispatch_get(&fixture, "GET /sd/aius/dsp.json"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	fixture.fake.sd_busy = false;
	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_get(&fixture, "GET /sd/aius/dsp.json"), 503,
	                 "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	fixture.fake.sd_ready = true;

	/* Write and delete are untouched by the carve-out. The PUT keeps the
	 * streamed path's order of refusal (auth, then the www/-only rule) and the
	 * DELETE never gets past that rule, with or without a good token -- the
	 * same two bodies "/sd/etc/passwd" already gets. */
	expect_immediate(dispatch_request(&fixture, "PUT /sd/aius/dsp.json",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 507, "{\"ok\":false,\"error\":\"sd_upload\"}");
	expect_immediate(dispatch_request(&fixture, "PUT /sd/aius/dsp.json", NULL,
	                                  NULL, 16),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/aius/dsp.json",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 404, "{\"ok\":false,\"error\":\"sd_path\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/aius/dsp.json",
	                                  NULL, NULL, -1),
	                 404, "{\"ok\":false,\"error\":\"sd_path\"}");
}

/* ------------------------------------------------------------------ */
/* directory index                                                      */
/* ------------------------------------------------------------------ */

#define INDEX_PROLOGUE(path) \
	"<!DOCTYPE html><html><head><meta charset=\"utf-8\">" \
	"<title>/sd/" path "</title></head><body><h1>/sd/" path "</h1><ul>"
#define INDEX_PARENT "<li><a href=\"../\">../</a></li>"
#define INDEX_FOOTER(shown, hidden, truncated) \
	"</ul><p>" shown " shown, " hidden " hidden, truncated: " truncated \
	". Sizes in bytes; at most 64 entries per listing.</p></body></html>"

static void fake_dir_add(Fake *fake, const char *name, uint64_t size,
                         bool is_directory)
{
	assert(fake->dir_entry_count < FAKE_DIR_ENTRIES);
	snprintf(fake->dir_entries[fake->dir_entry_count].name,
	         FAKE_DIR_NAME_STORAGE, "%s", name);
	fake->dir_entries[fake->dir_entry_count].size = size;
	fake->dir_entries[fake->dir_entry_count].is_directory = is_directory;
	fake->dir_entry_count++;
}

static void expect_index(const AiusHttpApplicationResponse *response,
                         const char *body)
{
	expect_response(response, AIUS_HTTP_APP_IMMEDIATE, 200,
	                "text/html; charset=utf-8", body);
}

static void copy_body(const AiusHttpApplicationResponse *response, char *out,
                      size_t capacity)
{
	assert(response->body != NULL);
	assert(response->body_length < capacity);
	memcpy(out, response->body, response->body_length);
	out[response->body_length] = '\0';
}

static size_t count_occurrences(const char *haystack, const char *needle)
{
	size_t count = 0u;
	const char *cursor = haystack;
	for (;;) {
		cursor = strstr(cursor, needle);
		if (cursor == NULL)
			return count;
		cursor += strlen(needle);
		count++;
	}
}

/* The listing a browser gets, byte for byte: relative links back into the same
 * /sd/ namespace, a size per file, and a footer that states the counts. */
static void test_sd_index_listing(void)
{
	Fixture fixture;
	fixture_init(&fixture);

	/* An empty directory is a 200 with an empty list, not a 404. */
	expect_index(dispatch_get(&fixture, "GET /sd/www/"),
	             INDEX_PROLOGUE("www/") INDEX_FOOTER("0", "0", "no"));
	assert(strcmp(fixture.fake.dir_path, "/aius/www") == 0);
	{
		static const char *const calls[] = {
			"sd_ready", "sd_busy", "read_directory"
		};
		expect_log(&fixture, calls, ARRAY_LENGTH(calls));
	}

	fake_dir_add(&fixture.fake, "index.html", 4096u, false);
	fake_dir_add(&fixture.fake, "sub", 0u, true);
	expect_index(dispatch_get(&fixture, "GET /sd/www/"),
	             INDEX_PROLOGUE("www/")
	             "<li><a href=\"index.html\">index.html</a> 4096</li>"
	             "<li><a href=\"sub/\">sub/</a> dir</li>"
	             INDEX_FOOTER("2", "0", "no"));

	/* A nested listing names the nested card path and offers the parent link;
	 * the subtree root does not, because "../" from www/ leaves /sd/. */
	expect_index(dispatch_get(&fixture, "GET /sd/www/sub/"),
	             INDEX_PROLOGUE("www/sub/") INDEX_PARENT
	             "<li><a href=\"index.html\">index.html</a> 4096</li>"
	             "<li><a href=\"sub/\">sub/</a> dir</li>"
	             INDEX_FOOTER("2", "0", "no"));
	assert(strcmp(fixture.fake.dir_path, "/aius/www/sub") == 0);
}

/* Names the /sd/ route could not serve are reported, not drawn as dead links
 * and not silently dropped -- which is also what keeps the body HTML-safe. */
static void test_sd_index_hidden_entries(void)
{
	Fixture fixture;
	fixture_init(&fixture);

	fake_dir_add(&fixture.fake, "ok.bin", 7u, false);
	fake_dir_add(&fixture.fake, "two words.txt", 1u, false);
	fake_dir_add(&fixture.fake, "\"><script>x</script>", 1u, false);
	/* Dot entries are not entries a reader wants and are not "hidden". */
	fake_dir_add(&fixture.fake, ".", 0u, true);
	fake_dir_add(&fixture.fake, "..", 0u, true);
	/* Two more the application itself could not represent (a name longer than
	 * the entry storage). */
	fixture.fake.dir_hidden = 2u;

	expect_index(dispatch_get(&fixture, "GET /sd/www/"),
	             INDEX_PROLOGUE("www/")
	             "<li><a href=\"ok.bin\">ok.bin</a> 7</li>"
	             INDEX_FOOTER("1", "4", "no"));
}

/* The bound, from both sides: the entry cap and the response buffer. */
static void test_sd_index_bounds(void)
{
	Fixture fixture;
	/* The worst case the cap admits: every name at the 32-byte maximum and
	 * every size a full 20-digit uint64. */
	static const char *const longest = "abcdefghijabcdefghijabcdefghij12";
	char body[AIUS_HTTP_MAX_RESPONSE + 1u];
	const AiusHttpApplicationResponse *response;
	fixture_init(&fixture);

	for (size_t i = 0; i < FAKE_DIR_ENTRIES; ++i)
		fake_dir_add(&fixture.fake, longest, UINT64_MAX, false);
	assert(FAKE_DIR_ENTRIES > AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES);

	response = dispatch_get(&fixture, "GET /sd/www/");
	copy_body(response, body, sizeof(body));
	assert(response->kind == AIUS_HTTP_APP_IMMEDIATE);
	assert(response->status == 200u);
	/* Exactly the cap is listed, and the body says the listing stopped. */
	assert(count_occurrences(body, "<li>") ==
	       AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES);
	assert(strstr(body, INDEX_FOOTER("64", "0", "yes")) != NULL);
	/* And the whole worst case still frames as an ordinary response: it never
	 * needs the transaction's streaming path. */
	assert(response->body_length <= AIUS_HTTP_MAX_RESPONSE);

	/* A response buffer too small for the cap truncates on capacity instead,
	 * and says so the same way. */
	{
		Fixture small;
		AiusOpticalApp app;
		uint8_t small_body[1024];
		size_t shown;
		memcpy(&small, &fixture, sizeof(small));
		app = fake_app(&small.fake);
		assert(aius_optical_app_init(&small.dispatcher, &app, small_body,
		                             sizeof(small_body)));
		response = dispatch_get(&small, "GET /sd/www/");
		assert(response->status == 200u);
		copy_body(response, body, sizeof(body));
		shown = count_occurrences(body, "<li>");
		assert(shown > 0u && shown < AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES);
		assert(strstr(body, ", truncated: yes.") != NULL);
		assert(response->body_length <= sizeof(small_body));
	}

	/* A buffer that cannot even hold the framing degrades to the same
	 * deterministic 500 every other over-large body produces. */
	{
		Fixture tiny;
		AiusOpticalApp app;
		uint8_t tiny_body[AIUS_OPTICAL_APP_MIN_BODY];
		memcpy(&tiny, &fixture, sizeof(tiny));
		app = fake_app(&tiny.fake);
		assert(aius_optical_app_init(&tiny.dispatcher, &app, tiny_body,
		                             sizeof(tiny_body)));
		expect_response(dispatch_get(&tiny, "GET /sd/www/"),
		                AIUS_HTTP_APP_IMMEDIATE, 500,
		                "text/html; charset=utf-8", "<p>response_too_large</p>");
	}
}

static void test_sd_index_refusals(void)
{
	Fixture fixture;
	static const char *const bad_paths[] = {
		"GET /sd/etc/",
		"GET /sd/www//",
		"GET /sd/www/../",
		"GET /sd/www/a//b/",
		"GET /sd/www/../../"
	};
	static const char *const path_calls[] = { "sd_ready" };
	static const char *const busy_calls[] = { "sd_ready", "sd_busy" };
	fixture_init(&fixture);
	fake_dir_add(&fixture.fake, "ok.bin", 1u, false);

	/* A path outside the subtree, or one the download route would refuse,
	 * never reaches the card. */
	for (size_t i = 0; i < ARRAY_LENGTH(bad_paths); ++i) {
		expect_immediate(dispatch_get(&fixture, bad_paths[i]), 404,
		                 "{\"ok\":false,\"error\":\"sd_path\"}");
		expect_log(&fixture, path_calls, ARRAY_LENGTH(path_calls));
	}

	/* No raw enumeration while a job holds the volume. */
	fixture.fake.sd_busy = true;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	expect_log(&fixture, busy_calls, ARRAY_LENGTH(busy_calls));
	fixture.fake.sd_busy = false;

	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/"), 503,
	                 "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	expect_log(&fixture, path_calls, ARRAY_LENGTH(path_calls));
	fixture.fake.sd_ready = true;

	/* The three answers the card itself can give. */
	fixture.fake.dir_result = AIUS_OPTICAL_APP_DIR_NOT_FOUND;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/missing/"), 404,
	                 "{\"ok\":false,\"error\":\"sd_not_found\"}");
	fixture.fake.dir_result = AIUS_OPTICAL_APP_DIR_NOT_DIRECTORY;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/index.html/"), 404,
	                 "{\"ok\":false,\"error\":\"sd_not_directory\"}");
	fixture.fake.dir_result = AIUS_OPTICAL_APP_DIR_FAILED;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/"), 503,
	                 "{\"ok\":false,\"error\":\"sd_index\"}");
	fixture.fake.dir_result = AIUS_OPTICAL_APP_DIR_OK;

	/* A directory is not an upload target and not a delete target, and neither
	 * refusal touches the application. */
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 405, "{\"ok\":false,\"error\":\"sd_directory\"}");
	expect_log(&fixture, path_calls, 0u);
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/www/sub/",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 405, "{\"ok\":false,\"error\":\"sd_directory\"}");
	expect_log(&fixture, path_calls, 0u);
}

/* read_directory is the one optional callback: a runtime that has not wired it
 * still initialises and still serves every other route, and the index route
 * answers a deterministic 503 rather than a null call. */
static void test_sd_index_unwired(void)
{
	Fixture fixture;
	AiusOpticalApp app;
	static const char *const no_calls[] = { "" };
	fixture_init(&fixture);
	app = fake_app(&fixture.fake);
	app.read_directory = NULL;
	assert(aius_optical_app_init(&fixture.dispatcher, &app, fixture.body,
	                             sizeof(fixture.body)));

	expect_immediate(dispatch_get(&fixture, "GET /sd/www/"), 503,
	                 "{\"ok\":false,\"error\":\"sd_index_unavailable\"}");
	expect_log(&fixture, no_calls, 0u);
	/* The rest of the table is untouched by the hole. */
	expect_response(dispatch_get(&fixture, "GET /api/status"),
	                AIUS_HTTP_APP_IMMEDIATE, 200, "application/json",
	                "{\"body\":\"status\"}");
}

static void test_sd_delete(void)
{
	Fixture fixture;
	const AiusHttpApplicationResponse *response;
	fixture_init(&fixture);

	response = dispatch_request(&fixture, "DELETE /sd/www/index.html",
	                            "X-Aius-SD-Auth: token\r\n", NULL, -1);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                "{\"ok\":true}");
	assert(response->sd_job.kind == AIUS_SD_JOB_DELETE);
	assert(response->sd_job.require_existing);
	assert(strcmp(response->sd_job.target_path, "/aius/www/index.html") == 0);
	assert(response->sd_job.content_type == NULL);
	assert(response->sd_job.temp_path == NULL);
	assert(response->sd_job.backup_path == NULL);

	expect_immediate(dispatch_request(&fixture, "DELETE /sd/www/index.html",
	                                  "X-Aius-SD-Auth: wrong\r\n", NULL, -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/www/index.html",
	                                  NULL, NULL, -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/www/config.json",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 403, "{\"ok\":false,\"error\":\"config_public_only\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/etc/passwd",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 404, "{\"ok\":false,\"error\":\"sd_path\"}");
}

static void test_sd_upload(void)
{
	Fixture fixture;
	const AiusHttpApplicationResponse *response;
	fixture_init(&fixture);

	/* Ordinary streamed PUT: replace through a distinct temporary + backup. */
	response = dispatch_request(&fixture, "PUT /sd/www/index.html",
	                            "X-Aius-SD-Auth: token\r\n", NULL, 4096);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                "{\"ok\":true}");
	assert(response->sd_job.kind == AIUS_SD_JOB_UPLOAD);
	assert(response->sd_job.install_mode == AIUS_SD_INSTALL_REPLACE);
	assert(response->sd_job.total == 4096u);
	assert(response->sd_job.initial_offset == 0u);
	assert(response->sd_job.content_type == NULL);
	assert(!response->sd_job.publish);
	assert(response->sd_job.publish_capacity == 0u);
	assert(strcmp(response->sd_job.target_path, "/aius/www/index.html") == 0);
	assert(strcmp(response->sd_job.temp_path,
	              "/aius/.upload-1234abcd-00000001.tmp") == 0);
	assert(strcmp(response->sd_job.backup_path,
	              "/aius/.backup-1234abcd-00000001.bak") == 0);

	/* Consecutive uploads never reuse a scratch name. */
	response = dispatch_request(&fixture, "PUT /sd/www/index.html",
	                            "X-Aius-SD-Auth: token\r\n", NULL, 4096);
	assert(strcmp(response->sd_job.temp_path,
	              "/aius/.upload-1234abcd-00000002.tmp") == 0);

	/* Order of refusal on the streamed path: config, then auth, then media. */
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/config.json",
	                                  "X-Aius-SD-Auth: wrong\r\n", NULL, 16),
	                 403, "{\"ok\":false,\"error\":\"config_public_only\"}");
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/index.html",
	                                  "X-Aius-SD-Auth: wrong\r\n", NULL, 16),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/index.html",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 503, "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	fixture.fake.sd_ready = true;
	fixture.fake.sd_busy = true;
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/index.html",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 507, "{\"ok\":false,\"error\":\"sd_upload\"}");
	fixture.fake.sd_busy = false;
	expect_immediate(dispatch_request(&fixture, "PUT /sd/etc/passwd",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 507, "{\"ok\":false,\"error\":\"sd_upload\"}");
}

static void test_replay_chunk_upload(void)
{
	Fixture fixture;
	const AiusHttpApplicationResponse *response;
	fixture_init(&fixture);

	response = dispatch_request(&fixture, "PUT /sd/replays/take1",
	                            "X-Aius-SD-Auth: token\r\n"
	                            "Content-Range: bytes 0-511/2048\r\n",
	                            NULL, 512);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                "{\"ok\":true,\"next_offset\":512}");
	assert(response->sd_job.kind == AIUS_SD_JOB_UPLOAD);
	assert(response->sd_job.install_mode == AIUS_SD_INSTALL_APPEND_PART);
	assert(strcmp(response->sd_job.target_path,
	              "/aius/replays/take1.part") == 0);
	assert(response->sd_job.temp_path == NULL);
	assert(response->sd_job.backup_path == NULL);
	assert(response->sd_job.content_type == NULL);
	assert(response->sd_job.total == 512u);
	assert(response->sd_job.initial_offset == 0u);

	response = dispatch_request(&fixture, "PUT /sd/replays/take1",
	                            "X-Aius-SD-Auth: token\r\n"
	                            "Content-Range: bytes 512-1023/2048\r\n",
	                            NULL, 512);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                "{\"ok\":true,\"next_offset\":1024}");
	assert(response->sd_job.initial_offset == 512u);

	/* Final chunk installs the .part file under its playable name. */
	response = dispatch_request(&fixture, "PUT /sd/replays/take1",
	                            "X-Aius-SD-Auth: token\r\n"
	                            "Content-Range: bytes 1536-2047/2048\r\n",
	                            NULL, 512);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                "{\"ok\":true}");
	assert(response->sd_job.install_mode == AIUS_SD_INSTALL_FINALIZE_PART);
	assert(strcmp(response->sd_job.target_path,
	              "/aius/replays/take1.iec") == 0);
	assert(strcmp(response->sd_job.temp_path,
	              "/aius/replays/take1.part") == 0);
	assert(strcmp(response->sd_job.backup_path,
	              "/aius/.backup-1234abcd-00000001.bak") == 0);
	assert(response->sd_job.total == 512u);
	assert(response->sd_job.initial_offset == 1536u);

	/* A streamed replay PUT without Content-Range would append at offset zero
	 * and never finalize; refuse it deterministically instead. */
	expect_immediate(dispatch_request(&fixture, "PUT /sd/replays/take1",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 512),
	                 400, "{\"ok\":false,\"error\":\"sd_upload\"}");
	expect_immediate(dispatch_request(&fixture, "PUT /sd/replays/take1",
	                                  "X-Aius-SD-Auth: wrong\r\n"
	                                  "Content-Range: bytes 0-511/2048\r\n",
	                                  NULL, 512),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
}

static void test_dsp_config_put(void)
{
	Fixture fixture;
	static const struct {
		AiusOpticalAppStoreResult result;
		unsigned status;
		const char *body;
	} refusals[] = {
		{ AIUS_OPTICAL_APP_STORE_INVALID, 400,
		  "{\"ok\":false,\"error\":\"dsp_config_invalid\"}" },
		{ AIUS_OPTICAL_APP_STORE_STALE, 409,
		  "{\"ok\":false,\"error\":\"stale_generation\"}" },
		{ AIUS_OPTICAL_APP_STORE_BUSY, 503,
		  "{\"ok\":false,\"error\":\"sd_busy\"}" }
	};
	static const char *const put_body = "{\"base_generation\":1}";
	fixture_init(&fixture);

	/* Accepted: the model is already staged in RAM, so the answer is the SD
	 * write of the verbatim body to the pre-cutover /aius/dsp.json. */
	expect_config_job(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                   "X-Aius-SD-Auth: token\r\n", put_body,
	                                   -1),
	                  "/aius/dsp.json", 1ul, strlen(put_body));
	assert(fixture.fake.stored_length == strlen(put_body));

	/* Consecutive writes never reuse a scratch name. */
	expect_config_job(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                   "X-Aius-SD-Auth: token\r\n", put_body,
	                                   -1),
	                  "/aius/dsp.json", 2ul, strlen(put_body));

	/* A store that handled the body completely keeps the immediate 200. */
	fixture.fake.dsp_store = AIUS_OPTICAL_APP_STORE_OK;
	expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                  "X-Aius-SD-Auth: token\r\n", put_body, -1),
	                 200, "{\"ok\":true}");

	/* Every store refusal stays immediate, with its pre-cutover status. */
	for (size_t i = 0; i < ARRAY_LENGTH(refusals); ++i) {
		fixture.fake.dsp_store = refusals[i].result;
		expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config",
		                                  "X-Aius-SD-Auth: token\r\n", put_body,
		                                  -1),
		                 refusals[i].status, refusals[i].body);
		assert(fixture.fake.stored_length == strlen(put_body));
	}

	/* Neither the immediate 200 nor any refusal consumed a scratch name: the
	 * next accepted write is still sequence 3. */
	fixture.fake.dsp_store = AIUS_OPTICAL_APP_STORE_ACCEPTED;
	expect_config_job(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                   "X-Aius-SD-Auth: token\r\n", put_body,
	                                   -1),
	                  "/aius/dsp.json", 3ul, strlen(put_body));

	expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                  "X-Aius-SD-Auth: wrong\r\n", "{}", -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config", NULL,
	                                  "{}", -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                  "X-Aius-SD-Auth: token\r\n", "{}", -1),
	                 503, "{\"ok\":false,\"error\":\"sd_unavailable\"}");
}

static void test_config_public_put(void)
{
	Fixture fixture;
	fixture_init(&fixture);

	expect_config_job(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                   "{\"replay\":true}", -1),
	                  "/aius/config.json", 1ul, strlen("{\"replay\":true}"));
	assert(fixture.fake.stored_length == strlen("{\"replay\":true}"));

	fixture.fake.public_store = AIUS_OPTICAL_APP_STORE_OK;
	expect_immediate(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                  "{\"replay\":true}", -1),
	                 200, "{\"ok\":true}");

	fixture.fake.public_store = AIUS_OPTICAL_APP_STORE_INVALID;
	expect_immediate(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                  "{}", -1),
	                 400, "{\"ok\":false,\"error\":\"config_invalid\"}");
	fixture.fake.public_store = AIUS_OPTICAL_APP_STORE_STALE;
	expect_immediate(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                  "{}", -1),
	                 409, "{\"ok\":false,\"error\":\"stale_generation\"}");
	fixture.fake.public_store = AIUS_OPTICAL_APP_STORE_BUSY;
	expect_immediate(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                  "{}", -1),
	                 503, "{\"ok\":false,\"error\":\"sd_busy\"}");

	fixture.fake.public_store = AIUS_OPTICAL_APP_STORE_ACCEPTED;
	expect_config_job(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                   "{}", -1),
	                  "/aius/config.json", 2ul, strlen("{}"));

	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                  "{}", -1),
	                 503, "{\"ok\":false,\"error\":\"sd_unavailable\"}");
}

/* The two ends of the buffered body the write carries. `total` must equal the
 * parsed Content-Length at both, because accept_application refuses the whole
 * response when it does not match the transaction's body_expected. */
static void test_config_put_body_bounds(void)
{
	static char large[AIUS_HTTP_MAX_BODY + 1u];
	Fixture fixture;
	fixture_init(&fixture);

	/* AIUS_HTTP_MAX_BODY is the largest non-streamed body the parser admits,
	 * and it is exactly the DSP model's serialized ceiling. */
	memset(large, 'x', AIUS_HTTP_MAX_BODY);
	large[0] = '{';
	large[AIUS_HTTP_MAX_BODY - 1u] = '}';
	large[AIUS_HTTP_MAX_BODY] = '\0';
	expect_config_job(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                   "X-Aius-SD-Auth: token\r\n", large, -1),
	                  "/aius/dsp.json", 1ul, (uint64_t)AIUS_HTTP_MAX_BODY);
	assert(fixture.fake.stored_length == AIUS_HTTP_MAX_BODY);

	/* An empty body is the application's judgement, not the dispatcher's; the
	 * real parser rejects it, and that refusal must stay immediate. */
	fixture.fake.public_store = AIUS_OPTICAL_APP_STORE_INVALID;
	expect_immediate(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                  NULL, 0),
	                 400, "{\"ok\":false,\"error\":\"config_invalid\"}");
	assert(fixture.fake.stored_length == 0u);
}

static void test_replay_control(void)
{
	Fixture fixture;
	fixture_init(&fixture);

	/* Status and listing are public; every mutation needs the SD token. */
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/stop",
	                                  "X-Aius-SD-Auth: wrong\r\n", NULL, -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/stop",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true,\"state\":\"stopped\"}");
	assert(fixture.fake.stops == 1u);

	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\",\"loop\":true}", -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"prebuffering\",\"id\":\"take1\"}");
	assert(strcmp(fixture.fake.last_replay_id, "take1") == 0);
	assert(fixture.fake.last_replay_loop);

	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take2\"}", -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"prebuffering\",\"id\":\"take2\"}");
	assert(!fixture.fake.last_replay_loop);

	fixture.fake.replay_start_result = AIUS_OPTICAL_APP_REPLAY_NOT_FOUND;
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 404, "{\"ok\":false,\"error\":\"replay_not_found\"}");
	fixture.fake.replay_start_result = AIUS_OPTICAL_APP_REPLAY_FAILED;
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 409, "{\"ok\":false,\"error\":\"replay_start\"}");
	/* A live carrier refuses with its own token, so a client can tell it from
	 * every other 409 on this route. */
	fixture.fake.replay_start_result = AIUS_OPTICAL_APP_REPLAY_CARRIER_PRESENT;
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 409, "{\"ok\":false,\"error\":\"replay_carrier_present\"}");

	/* A malformed or unusable id never reaches the application. */
	static const char *const bad_bodies[] = {
		"{}", "{\"id\":\"\"}", "{\"id\":\"bad id\"}", "{\"id\":\"bad/id\"}",
		"{\"id\":\"unterminated}"
	};
	for (size_t i = 0; i < ARRAY_LENGTH(bad_bodies); ++i) {
		expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  bad_bodies[i], -1),
		                 400, "{\"ok\":false,\"error\":\"replay_body\"}");
		expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  bad_bodies[i], -1),
		                 400, "{\"ok\":false,\"error\":\"replay_body\"}");
	}

	fixture.fake.replay_start_result = AIUS_OPTICAL_APP_REPLAY_OK;
	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 400, "{\"ok\":false,\"error\":\"replay_body\"}");
	/* The listing keeps its pre-cutover shape: 200 carrying its own error. */
	expect_immediate(dispatch_get(&fixture, "GET /api/replays"), 200,
	                 "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	fixture.fake.sd_ready = true;

	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 200, "{\"ok\":true,\"deleted\":\"take1\"}");
	fixture.fake.replay_delete_result = AIUS_OPTICAL_APP_REPLAY_ACTIVE;
	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 409, "{\"ok\":false,\"error\":\"replay_active\"}");
	fixture.fake.replay_delete_result = AIUS_OPTICAL_APP_REPLAY_NOT_FOUND;
	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 404, "{\"ok\":false,\"error\":\"replay_not_found\"}");
	fixture.fake.replay_delete_result = AIUS_OPTICAL_APP_REPLAY_FAILED;
	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 503, "{\"ok\":false,\"error\":\"replay_delete\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: wrong\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
}

/* The two replay routes that reach the card directly are excluded while a job
 * owns the volume, exactly as GET /sd/<dir>/ already is.
 *
 * Both enumerate or stat the card outside the bounded job engine -- the
 * portable engine has no directory job, and delete is two constant primitives
 * -- so overlapping a running job means walking a volume someone else is
 * mid-transaction on. The Phase 14d audit found neither gated. A refused call
 * answers 503 sd_busy: the same distinguishable token every other SD route
 * uses, and a refusal rather than a listing read across a foreign transaction
 * or a 404 that only means "the card was busy".
 *
 * The memory-only replay routes are deliberately NOT gated, and that is
 * asserted here rather than left implicit: start, stop and status touch no
 * primitive, and start must in particular still be admitted while a refill job
 * for the previous selection is draining, because cancelling it is exactly what
 * start does. */
static void test_replay_card_routes_are_gated(void)
{
	Fixture fixture;
	static const char *const list_calls[] = { "sd_ready", "sd_busy" };
	static const char *const delete_calls[] = { "auth_matches", "sd_busy" };
	static const char *const start_calls[] = {
		"auth_matches", "sd_ready", "replay_start"
	};
	static const char *const stop_calls[] = { "auth_matches", "replay_stop" };
	static const char *const status_calls[] = { "replay_status" };

	fixture_init(&fixture);
	fixture.fake.sd_busy = true;

	/* The listing never reaches the serializer, so no enumeration happens. */
	expect_immediate(dispatch_get(&fixture, "GET /api/replays"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	expect_log(&fixture, list_calls, ARRAY_LENGTH(list_calls));

	/* Delete never reaches the application, so nothing is stat'd or removed. */
	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 503, "{\"ok\":false,\"error\":\"sd_busy\"}");
	expect_log(&fixture, delete_calls, ARRAY_LENGTH(delete_calls));

	/* A malformed body is still a 400 whatever the volume is doing: the
	 * request is wrong regardless, and answering 503 would tell the client to
	 * retry a request that can never succeed. */
	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: token\r\n", "{}", -1),
	                 400, "{\"ok\":false,\"error\":\"replay_body\"}");

	/* And the memory-only routes are unaffected by a busy volume. */
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"prebuffering\",\"id\":\"take1\"}");
	expect_log(&fixture, start_calls, ARRAY_LENGTH(start_calls));
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/stop",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true,\"state\":\"stopped\"}");
	expect_log(&fixture, stop_calls, ARRAY_LENGTH(stop_calls));
	expect_immediate(dispatch_get(&fixture, "GET /api/replays/status"), 200,
	                 "{\"body\":\"replay_status\"}");
	expect_log(&fixture, status_calls, ARRAY_LENGTH(status_calls));

	/* Idle again, and both routes serve. */
	fixture.fake.sd_busy = false;
	expect_immediate(dispatch_get(&fixture, "GET /api/replays"), 200,
	                 "{\"body\":\"replay_list\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"take1\"}", -1),
	                 200, "{\"ok\":true,\"deleted\":\"take1\"}");
}

/* RFC 8259 §2 makes whitespace insignificant before and after every structural
 * character, so a body is identified by what it means, not by how it is spaced.
 * The compact form the bundled page happens to emit (JSON.stringify) and the
 * spaced form every Python client emits (json.dumps) are the same request.
 *
 * Whitespace inside the quotes is data, not structure: it must survive into the
 * id and then fail the id charset, exactly as it did before. */
static void test_replay_body_whitespace(void)
{
	/* Each body carries id "take1" and, where a loop key is present, loop true. */
	static const struct {
		const char *body;
		bool loop;
	} accepted[] = {
		/* the pre-fix compact form, unchanged */
		{ "{\"id\":\"take1\",\"loop\":true}", true },
		{ "{\"id\":\"take1\"}", false },
		/* json.dumps default: one space after every colon and comma */
		{ "{\"id\": \"take1\", \"loop\": true}", true },
		{ "{\"id\": \"take1\"}", false },
		/* space before the colon, and around the comma */
		{ "{\"id\" : \"take1\" , \"loop\" : true}", true },
		/* leading and trailing whitespace around the whole body */
		{ "  {\"id\":\"take1\"}  ", false },
		{ "\r\n\t{\"id\":\"take1\",\"loop\":true}\n", true },
		/* newlines and tabs are whitespace wherever a space would be */
		{ "{\n\t\"id\": \"take1\",\n\t\"loop\": true\n}", true },
		{ "{\r\n  \"id\" :\t\"take1\"\r\n}", false },
		/* inside the braces, either side of the first key */
		{ "{ \"id\":\"take1\" }", false },
		/* loop false is a value, not an absent key */
		{ "{\"id\": \"take1\", \"loop\": false}", false },
		/* key order is not part of the grammar */
		{ "{\"loop\" : true , \"id\" : \"take1\"}", true },
	};
	/* Whitespace tolerance is not licence for junk: every entry here is either
	 * invalid JSON or a valid document this route does not accept. */
	static const char *const rejected[] = {
		/* the pre-fix rejections, still rejected */
		"{}", "{\"id\":\"\"}", "{\"id\":\"bad id\"}", "{\"id\":\"bad/id\"}",
		"{\"id\":\"unterminated}",
		/* whitespace inside the string is preserved, so it still fails the
		 * charset -- a parser that stripped it would admit "badid" */
		"{\"id\": \"bad id\"}", "{\"id\":\"\tbad\"}", "{\"id\":\"take1 \"}",
		"{ \"id\" : \" \" }",
		/* whitespace does not repair a broken document */
		"", "   ", "{", "}", "{\"id\": }", "{\"id\" \"take1\"}",
		"{\"id\": \"take1\",}", "{\"id\": \"take1\" \"loop\": true}",
		"{\"id\": \"take1\"} trailing", "{\"id\": \"take1\"}{",
		"[{\"id\": \"take1\"}]", "\"take1\"",
		/* wrong-typed or unparsable loop */
		"{\"id\": \"take1\", \"loop\": \"true\"}",
		"{\"id\": \"take1\", \"loop\": 1}",
		"{\"id\": \"take1\", \"loop\": truex}",
		"{\"id\": \"take1\", \"loop\": null}",
		/* wrong-typed id */
		"{\"id\": 1}", "{\"id\": true}", "{\"id\": [\"take1\"]}",
		/* unknown field, duplicate key, missing key */
		"{\"id\": \"take1\", \"speed\": 2}", "{\"identity\": \"take1\"}",
		"{\"id\": \"take1\", \"id\": \"take2\"}",
		"{\"loop\": true, \"loop\": false, \"id\": \"take1\"}",
		"{\"loop\": true}",
		/* an over-long id (49 chars, one past AIUS_HTTP_MAX_REPLAY_ID) */
		"{\"id\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"
	};
	/* The substring scan this parser replaced accepted every one of these: it
	 * hunted "id":" anywhere in the bytes and never read the document around
	 * it, so junk, an extra field, a second id, a non-boolean loop and even a
	 * body that was not JSON all started a replay. Reading the object left to
	 * right is what refuses them, and they are compact on purpose -- spelled
	 * with a space the old scan refused them for the wrong reason. */
	static const char *const newly_rejected[] = {
		"{\"id\":\"take1\",\"speed\":2}",
		"{\"identity\":\"take1\",\"id\":\"take1\"}",
		"{\"id\":\"take1\",\"id\":\"take2\"}",
		"{\"id\":\"take1\",\"loop\":\"true\"}",
		"{\"id\":\"take1\",\"loop\":1}",
		"{\"id\":\"take1\",\"loop\":truex}",
		"{\"id\":\"take1\",\"loop\":true,\"loop\":false}",
		"{\"id\":\"take1\",\"loop\":true",
		"{\"id\":\"take1\"} trailing",
		"{\"id\":\"take1\"}{",
		"[{\"id\":\"take1\"}]",
		"garbage{\"id\":\"take1\"}",
		"{\"nested\":{\"id\":\"take1\"}}",
		"not json at all \"id\":\"take1\" either"
	};
	Fixture fixture;
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(accepted); ++i) {
		fixture.fake.last_replay_id[0] = '\0';
		fixture.fake.last_replay_loop = !accepted[i].loop;
		expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  accepted[i].body, -1),
		                 200,
		                 "{\"ok\":true,\"state\":\"prebuffering\",\"id\":\"take1\"}");
		if (strcmp(fixture.fake.last_replay_id, "take1") != 0)
			fprintf(stderr, "body \"%s\" yielded id \"%s\"\n", accepted[i].body,
			        fixture.fake.last_replay_id);
		assert(strcmp(fixture.fake.last_replay_id, "take1") == 0);
		if (fixture.fake.last_replay_loop != accepted[i].loop)
			fprintf(stderr, "body \"%s\" yielded loop %d\n", accepted[i].body,
			        (int)fixture.fake.last_replay_loop);
		assert(fixture.fake.last_replay_loop == accepted[i].loop);

		/* DELETE reads the same body through the same parser. */
		fixture.fake.last_replay_id[0] = '\0';
		expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  accepted[i].body, -1),
		                 200, "{\"ok\":true,\"deleted\":\"take1\"}");
		assert(strcmp(fixture.fake.last_replay_id, "take1") == 0);
	}

	for (size_t i = 0; i < ARRAY_LENGTH(rejected); ++i) {
		const AiusHttpApplicationResponse *response;
		fixture.fake.last_replay_id[0] = '\0';
		response = dispatch_request(&fixture, "POST /api/replays/start",
		                            "X-Aius-SD-Auth: token\r\n", rejected[i], -1);
		if (response == NULL || response->status != 400)
			fprintf(stderr, "body \"%s\" was not refused\n", rejected[i]);
		expect_immediate(response, 400,
		                 "{\"ok\":false,\"error\":\"replay_body\"}");
		expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  rejected[i], -1),
		                 400, "{\"ok\":false,\"error\":\"replay_body\"}");
		/* A refused body never named a replay to the application. */
		assert(fixture.fake.last_replay_id[0] == '\0');
	}

	for (size_t i = 0; i < ARRAY_LENGTH(newly_rejected); ++i) {
		const AiusHttpApplicationResponse *response;
		fixture.fake.last_replay_id[0] = '\0';
		response = dispatch_request(&fixture, "POST /api/replays/start",
		                            "X-Aius-SD-Auth: token\r\n",
		                            newly_rejected[i], -1);
		if (response == NULL || response->status != 400)
			fprintf(stderr, "body \"%s\" was not refused\n", newly_rejected[i]);
		expect_immediate(response, 400,
		                 "{\"ok\":false,\"error\":\"replay_body\"}");
		expect_immediate(dispatch_request(&fixture, "DELETE /api/replays",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  newly_rejected[i], -1),
		                 400, "{\"ok\":false,\"error\":\"replay_body\"}");
		assert(fixture.fake.last_replay_id[0] == '\0');
	}

	/* The longest id the route admits still parses when spaced out. */
	fixture.fake.last_replay_id[0] = '\0';
	expect_immediate(dispatch_request(&fixture, "POST /api/replays/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{ \"id\" : "
	                                  "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" }",
	                                  -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"prebuffering\",\"id\":\""
	                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
	assert(strlen(fixture.fake.last_replay_id) == AIUS_HTTP_MAX_REPLAY_ID);
}

/* One dispatch touches exactly the callbacks its decision needs -- no probing,
 * no speculative serialization, and nothing after a refusal. */
static void test_measure_control(void)
{
	Fixture fixture;
	fixture_init(&fixture);

	/* The arm is a control route: refused with no token and with a wrong one,
	 * before the body is even parsed. */
	static const char *const refused_calls[] = { "auth_matches" };
	expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
	                                  NULL, "{\"id\":\"pcm440_m6\"}", -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
	                                  "X-Aius-SD-Auth: wrong\r\n",
	                                  "{\"id\":\"pcm440_m6\"}", -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	assert(fixture.fake.last_measure_id[0] == '\0');

	/* Minimal body: both fit frequencies default to the corpus baseline. */
	expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"pcm440_m6\"}", -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"armed\",\"id\":\"pcm440_m6\"}");
	assert(strcmp(fixture.fake.last_measure_id, "pcm440_m6") == 0);
	assert(fixture.fake.last_measure_f0_l == 440u);
	assert(fixture.fake.last_measure_f0_r == 440u);
	assert(fixture.fake.last_measure_probe_count == 0u);

	/* Full body: per-channel frequencies and probe bins arrive as sent. */
	expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"pcm440_660_split\","
	                                  "\"f0_l\":440,\"f0_r\":660,"
	                                  "\"probes\":[40,100,16000]}", -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"armed\","
	                 "\"id\":\"pcm440_660_split\"}");
	assert(fixture.fake.last_measure_f0_l == 440u);
	assert(fixture.fake.last_measure_f0_r == 660u);
	assert(fixture.fake.last_measure_probe_count == 3u);
	assert(fixture.fake.last_measure_probes[0] == 40u);
	assert(fixture.fake.last_measure_probes[2] == 16000u);

	/* An empty probes array is legal and arrives as zero probes. */
	expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"pcm440_m6\",\"probes\":[]}",
	                                  -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"armed\",\"id\":\"pcm440_m6\"}");
	assert(fixture.fake.last_measure_probe_count == 0u);

	/* A refusal from the application is the 409, verbatim. */
	fixture.fake.measure_start_result = false;
	expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"id\":\"pcm440_m6\"}", -1),
	                 409, "{\"ok\":false,\"error\":\"measure_start\"}");
	fixture.fake.measure_start_result = true;

	/* Malformed bodies never reach the application: missing id, bad id
	 * charset, duplicate keys, unknown keys, a ninth probe, a fractional
	 * frequency, and trailing bytes. */
	fixture_reset_log(&fixture);
	static const char *const bad_bodies[] = {
		"{}",
		"{\"id\":\"bad id\"}",
		"{\"id\":\"a\",\"id\":\"b\"}",
		"{\"id\":\"a\",\"f0_l\":440,\"f0_l\":440}",
		"{\"id\":\"a\",\"volume\":1}",
		"{\"id\":\"a\",\"probes\":[1,2,3,4,5,6,7,8,9]}",
		"{\"id\":\"a\",\"f0_l\":440.5}",
		"{\"id\":\"a\",\"f0_l\":0}",
		"{\"id\":\"a\",\"probes\":[0]}",
		"{\"id\":\"a\"} x"
	};
	for (size_t i = 0; i < ARRAY_LENGTH(bad_bodies); ++i)
		expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  bad_bodies[i], -1),
		                 400, "{\"ok\":false,\"error\":\"measure_body\"}");
	for (size_t i = 0; i < fixture.fake.log_count; ++i)
		assert(strcmp(fixture.fake.log[i], "measure_start") != 0);

	/* The public read serves with no credential and no auth call. */
	fixture_reset_log(&fixture);
	expect_immediate(dispatch_get(&fixture, "GET /api/measure"), 200,
	                 "{\"body\":\"measure\"}");
	assert(fixture.fake.log_count == 1u);
	assert(strcmp(fixture.fake.log[0], "measure") == 0);
}

static void test_callback_isolation(void)
{
	Fixture fixture;
	static const char *const status_calls[] = { "status" };
	/* The control routes are gated before the switch, so the credential check
	 * is the first callback and the effect the second. */
	static const char *const start_calls[] = {
		"auth_matches", "test_running:true"
	};
	static const char *const download_calls[] = { "sd_ready", "sd_busy" };
	static const char *const delete_calls[] = {
		"sd_ready", "auth_matches", "sd_busy"
	};
	static const char *const upload_calls[] = {
		"auth_matches", "sd_ready", "sd_busy", "upload_id"
	};
	/* An accepted store reaches upload_id, because the answer is now the write
	 * and the write needs its scratch names. */
	static const char *const dsp_put_calls[] = {
		"auth_matches", "sd_ready", "store_dsp_config", "upload_id"
	};
	static const char *const config_put_calls[] = {
		"sd_ready", "store_public_config", "upload_id"
	};
	/* A refused store stops there: no scratch name, so nothing was prepared for
	 * the card. */
	static const char *const dsp_put_refused_calls[] = {
		"auth_matches", "sd_ready", "store_dsp_config"
	};
	static const char *const unauthorized_calls[] = { "auth_matches" };
	static const char *const unknown_route_calls[] = { "" };
	AiusHttpRequest unknown;
	fixture_init(&fixture);

	(void)dispatch_get(&fixture, "GET /api/status");
	expect_log(&fixture, status_calls, ARRAY_LENGTH(status_calls));

	(void)dispatch_request(&fixture, "POST /api/start",
	                       "X-Aius-SD-Auth: token\r\n", NULL, -1);
	expect_log(&fixture, start_calls, ARRAY_LENGTH(start_calls));

	(void)dispatch_get(&fixture, "GET /sd/www/index.html");
	expect_log(&fixture, download_calls, ARRAY_LENGTH(download_calls));

	(void)dispatch_request(&fixture, "DELETE /sd/www/index.html",
	                       "X-Aius-SD-Auth: token\r\n", NULL, -1);
	expect_log(&fixture, delete_calls, ARRAY_LENGTH(delete_calls));

	(void)dispatch_request(&fixture, "PUT /sd/www/index.html",
	                       "X-Aius-SD-Auth: token\r\n", NULL, 64);
	expect_log(&fixture, upload_calls, ARRAY_LENGTH(upload_calls));

	(void)dispatch_request(&fixture, "PUT /api/dsp/config",
	                       "X-Aius-SD-Auth: token\r\n", "{}", -1);
	expect_log(&fixture, dsp_put_calls, ARRAY_LENGTH(dsp_put_calls));

	(void)dispatch_request(&fixture, "PUT /api/config/public", NULL, "{}", -1);
	expect_log(&fixture, config_put_calls, ARRAY_LENGTH(config_put_calls));

	fixture.fake.dsp_store = AIUS_OPTICAL_APP_STORE_INVALID;
	(void)dispatch_request(&fixture, "PUT /api/dsp/config",
	                       "X-Aius-SD-Auth: token\r\n", "{}", -1);
	expect_log(&fixture, dsp_put_refused_calls,
	           ARRAY_LENGTH(dsp_put_refused_calls));
	fixture.fake.dsp_store = AIUS_OPTICAL_APP_STORE_ACCEPTED;

	/* A refused write stops at the refusal: no store, no media probe. */
	(void)dispatch_request(&fixture, "PUT /api/dsp/config",
	                       "X-Aius-SD-Auth: wrong\r\n", "{}", -1);
	expect_log(&fixture, unauthorized_calls, ARRAY_LENGTH(unauthorized_calls));

	/* A route with no dispatch arm answers from the switch tail and touches no
	 * application callback. The parser cannot produce one any more -- /api/mode
	 * and /api/mix used to reach here -- so the request is built by hand. */
	memset(&unknown, 0, sizeof(unknown));
	unknown.method = AIUS_HTTP_POST;
	unknown.route = AIUS_HTTP_ROUTE_UNKNOWN;
	fixture_reset_log(&fixture);
	(void)aius_optical_app_http_dispatch(&fixture.dispatcher, &unknown);
	expect_log(&fixture, unknown_route_calls, 0u);
}

static void test_dispatch_counts(void)
{
	Fixture fixture;
	AiusUsbApplicationRequest usb;
	fixture_init(&fixture);
	memset(&usb, 0, sizeof(usb));
	usb.command.kind = AIUS_USB_COMMAND_PROTOCOL;

	assert(fixture.dispatcher.http_dispatches == 0u);
	(void)dispatch_get(&fixture, "GET /api/status");
	(void)dispatch_get(&fixture, "GET /api/status");
	assert(fixture.dispatcher.http_dispatches == 2u);
	assert(fixture.dispatcher.usb_dispatches == 0u);
	(void)aius_optical_app_usb_dispatch(&fixture.dispatcher, &usb);
	assert(fixture.dispatcher.usb_dispatches == 1u);
}

static void expect_usb(const AiusUsbApplicationResponse *response,
                       const char *verb, const char *payload)
{
	assert(response != NULL);
	assert(response->error == AIUS_USB_ERROR_NONE);
	assert(response->verb != NULL);
	assert(response->verb_length == strlen(verb));
	assert(memcmp(response->verb, verb, response->verb_length) == 0);
	if (payload == NULL) {
		assert(response->payload == NULL);
		assert(response->payload_length == 0u);
		return;
	}
	assert(response->payload != NULL);
	assert(response->payload_length == strlen(payload));
	assert(memcmp(response->payload, payload, response->payload_length) == 0);
}

static const AiusUsbApplicationResponse *dispatch_usb(Fixture *fixture,
                                                      AiusUsbCommandKind kind)
{
	AiusUsbApplicationRequest request;
	memset(&request, 0, sizeof(request));
	request.command.kind = kind;
	request.command.request_id = 7u;
	fixture_reset_log(fixture);
	return aius_optical_app_usb_dispatch(&fixture->dispatcher, &request);
}

static void test_usb_commands(void)
{
	Fixture fixture;
	const AiusUsbApplicationResponse *response;
	fixture_init(&fixture);

	expect_usb(dispatch_usb(&fixture, AIUS_USB_COMMAND_PROTOCOL), "PROTOCOL",
	           "1");
	expect_usb(dispatch_usb(&fixture, AIUS_USB_COMMAND_ID), "ID",
	           "protocol=1 revision=test");
	expect_usb(dispatch_usb(&fixture, AIUS_USB_COMMAND_AUTH), "AUTH",
	           "protocol=1 revision=test");
	/* The control runtime owns the "entering" payload and the transition; a
	 * payload from the application would be rejected there. */
	expect_usb(dispatch_usb(&fixture, AIUS_USB_COMMAND_BOOTLOADER),
	           "BOOTLOADER", NULL);

	response = dispatch_usb(&fixture, AIUS_USB_COMMAND_NONE);
	assert(response != NULL);
	assert(response->error == AIUS_USB_ERROR_INTERNAL);
	assert(response->verb == NULL && response->verb_length == 0u);
	assert(response->payload == NULL && response->payload_length == 0u);

	/* A payload the application cannot produce, one that overruns the storage,
	 * and one carrying a non-printable byte all become the same deterministic
	 * INTERNAL error rather than a response the USB runtime would fault on. */
	fixture.fake.usb_payload_length = 0u;
	response = dispatch_usb(&fixture, AIUS_USB_COMMAND_ID);
	assert(response->error == AIUS_USB_ERROR_INTERNAL);

	fixture.fake.usb_payload_length = AIUS_OPTICAL_APP_USB_PAYLOAD_MAX + 1u;
	response = dispatch_usb(&fixture, AIUS_USB_COMMAND_ID);
	assert(response->error == AIUS_USB_ERROR_INTERNAL);

	memset(fixture.fake.usb_payload, 'a', 8u);
	fixture.fake.usb_payload[3] = '\n';
	fixture.fake.usb_payload_length = 8u;
	response = dispatch_usb(&fixture, AIUS_USB_COMMAND_AUTH);
	assert(response->error == AIUS_USB_ERROR_INTERNAL);

	/* The longest payload the runtime accepts still round-trips. */
	memset(fixture.fake.usb_payload, 'z', AIUS_OPTICAL_APP_USB_PAYLOAD_MAX);
	fixture.fake.usb_payload[AIUS_OPTICAL_APP_USB_PAYLOAD_MAX] = '\0';
	fixture.fake.usb_payload_length = AIUS_OPTICAL_APP_USB_PAYLOAD_MAX;
	response = dispatch_usb(&fixture, AIUS_USB_COMMAND_ID);
	assert(response->error == AIUS_USB_ERROR_NONE);
	assert(response->payload_length == AIUS_OPTICAL_APP_USB_PAYLOAD_MAX);
}

/* The two 405 method fall-throughs the parser rejects before dispatch ever sees
 * them (POST to /sd/<file> and POST to /api/replays). They are the dispatcher's
 * own last line, so they are driven from a hand-built request rather than the
 * parser -- and their bodies are asserted byte-for-byte like every other. */
static void test_unreachable_method_defaults(void)
{
	Fixture fixture;
	static const uint8_t resource[] = "www/index.html";
	static const uint8_t auth[] = "token";
	AiusHttpRequest request;
	fixture_init(&fixture);

	memset(&request, 0, sizeof(request));
	request.method = AIUS_HTTP_POST;
	request.route = AIUS_HTTP_ROUTE_SD_FILE;
	request.resource = resource;
	request.resource_length = sizeof(resource) - 1u;
	expect_immediate(aius_optical_app_http_dispatch(&fixture.dispatcher,
	                                                &request),
	                 405, "{\"ok\":false,\"error\":\"sd_method\"}");

	memset(&request, 0, sizeof(request));
	request.method = AIUS_HTTP_POST;
	request.route = AIUS_HTTP_ROUTE_REPLAYS;
	request.sd_auth = auth;
	request.sd_auth_length = sizeof(auth) - 1u;
	expect_immediate(aius_optical_app_http_dispatch(&fixture.dispatcher,
	                                                &request),
	                 405, "{\"ok\":false,\"error\":\"replay_method\"}");

	/* The switch tail. /api/mode and /api/mix reached it while they still
	 * parsed; with them retired no byte sequence produces a dispatchable
	 * unknown route, so the 404 is proven from a hand-built request. */
	memset(&request, 0, sizeof(request));
	request.method = AIUS_HTTP_POST;
	request.route = AIUS_HTTP_ROUTE_UNKNOWN;
	expect_immediate(aius_optical_app_http_dispatch(&fixture.dispatcher,
	                                                &request),
	                 404, "{\"ok\":false}");
}

/* Every route the parser can produce reaches a dispatch decision -- no route
 * falls through without a status. */
/* The recorder surface: the all-routes token gate (GETs included -- the data
 * read carries recorded audio and the header reveals trigger state), the
 * status serializer, the 200/409 state mappings, the config body's
 * all-keys-required strictness, and the data read's three-way result. */
static void test_recorder_routes(void)
{
	Fixture fixture;
	static const char *const auth = "X-Aius-SD-Auth: token\r\n";
	static const char *const good_config =
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}";
	static const struct { const char *line; const char *body; } gated[] = {
		{ "GET /api/recorder", NULL },
		{ "POST /api/recorder/freeze", NULL },
		{ "POST /api/recorder/thaw", NULL },
		{ "GET /api/recorder/data?offset=0&length=8&generation=0", NULL },
		{ "PUT /api/recorder/config",
		  "{\"overrun\":true,\"underrun\":true,\"carrier_loss\":true,"
		  "\"limiter_gr\":false,\"gr_threshold_db\":0,"
		  "\"gr_hold_blocks\":1,\"postroll_blocks\":188}" }
	};
	/* Refused shapes: a missing key, a duplicate, an unknown key, a
	 * positive threshold, an exponent-carrying number, and trailing
	 * bytes. */
	static const char *const bad_bodies[] = {
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3}",
		"{\"overrun\":true,\"overrun\":true,\"underrun\":false,"
		"\"carrier_loss\":true,\"limiter_gr\":true,"
		"\"gr_threshold_db\":-6.5,\"gr_hold_blocks\":3,"
		"\"postroll_blocks\":0}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0,\"extra\":1}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"gr_threshold_db\":6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"gr_threshold_db\":-1e2,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}x"
	};
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(gated); ++i) {
		expect_immediate(dispatch_request(&fixture, gated[i].line, NULL,
		                                  gated[i].body, -1),
		                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
		expect_immediate(dispatch_request(&fixture, gated[i].line,
		                                  "X-Aius-SD-Auth: wrong\r\n",
		                                  gated[i].body, -1),
		                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	}
	/* No refusal reached a callback. */
	assert(fixture.fake.last_recorder_mask == 0u);

	expect_immediate(dispatch_request(&fixture, "GET /api/recorder", auth,
	                                  NULL, -1),
	                 200, "{\"body\":\"recorder\"}");
	expect_immediate(dispatch_request(&fixture, "POST /api/recorder/freeze",
	                                  auth, NULL, -1),
	                 200, "{\"ok\":true,\"state\":\"freezing\"}");
	expect_immediate(dispatch_request(&fixture, "POST /api/recorder/thaw",
	                                  auth, NULL, -1),
	                 200, "{\"ok\":true,\"state\":\"recording\"}");
	fixture.fake.recorder_accept = false;
	expect_immediate(dispatch_request(&fixture, "POST /api/recorder/freeze",
	                                  auth, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"recorder_state\"}");
	expect_immediate(dispatch_request(&fixture, "POST /api/recorder/thaw",
	                                  auth, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"recorder_state\"}");
	expect_immediate(dispatch_request(&fixture, "PUT /api/recorder/config",
	                                  auth, good_config, -1),
	                 409, "{\"ok\":false,\"error\":\"recorder_state\"}");
	fixture.fake.recorder_accept = true;

	/* A good config passes every parsed value through: mask bits from the
	 * four booleans, the dB as scanned, hold and post-roll verbatim
	 * (post-roll 0 is legal: freeze at the trigger boundary). */
	expect_immediate(dispatch_request(&fixture, "PUT /api/recorder/config",
	                                  auth, good_config, -1),
	                 200, "{\"ok\":true}");
	assert(fixture.fake.last_recorder_mask ==
	       (AIUS_OPTICAL_APP_RECORDER_MASK_OVERRUN |
	        AIUS_OPTICAL_APP_RECORDER_MASK_CARRIER_LOSS |
	        AIUS_OPTICAL_APP_RECORDER_MASK_LIMITER_GR));
	assert(fixture.fake.last_recorder_gr_db == -6.5f);
	assert(fixture.fake.last_recorder_hold == 3u);
	assert(fixture.fake.last_recorder_postroll == 0u);

	for (size_t i = 0; i < ARRAY_LENGTH(bad_bodies); ++i) {
		fixture.fake.last_recorder_mask = 0xffffffffu;
		expect_immediate(dispatch_request(&fixture,
		                                  "PUT /api/recorder/config", auth,
		                                  bad_bodies[i], -1),
		                 400, "{\"ok\":false,\"error\":\"recorder_body\"}");
		/* The parse refused before the callback ran. */
		assert(fixture.fake.last_recorder_mask == 0xffffffffu);
	}

	/* The data read: params pass through untouched, the 200 borrows the
	 * application's storage directly (on target: the PSRAM ring), and the
	 * INVALID/REFUSED results map to 400/409. */
	{
		const AiusHttpApplicationResponse *response = dispatch_request(
			&fixture,
			"GET /api/recorder/data?offset=4096&length=512&generation=7",
			auth, NULL, -1);
		assert(response != NULL);
		assert(response->kind == AIUS_HTTP_APP_IMMEDIATE);
		assert(response->status == 200u);
		assert(strcmp(response->content_type,
		              "application/octet-stream") == 0);
		/* The 200 borrows the application's storage directly (on target:
		 * the PSRAM ring) -- pointer identity, not a copy. */
		assert(response->body == fixture.fake.recorder_data);
		assert(response->body_length == fixture.fake.recorder_data_length);
		assert(fixture.fake.last_recorder_offset == 4096u);
		assert(fixture.fake.last_recorder_length == 512u);
		assert(fixture.fake.last_recorder_generation == 7u);
	}
	fixture.fake.recorder_read_result =
		AIUS_OPTICAL_APP_RECORDER_READ_INVALID;
	expect_immediate(dispatch_request(&fixture,
	                                  "GET /api/recorder/data?offset=4&length=8&generation=0",
	                                  auth, NULL, -1),
	                 400, "{\"ok\":false,\"error\":\"recorder_read\"}");
	fixture.fake.recorder_read_result =
		AIUS_OPTICAL_APP_RECORDER_READ_REFUSED;
	expect_immediate(dispatch_request(&fixture,
	                                  "GET /api/recorder/data?offset=0&length=8&generation=1",
	                                  auth, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"recorder_state\"}");
}

/* ------------------------------------------------------------------ */
/* OTA -- staged firmware upgrade                                       */
/* ------------------------------------------------------------------ */

#define OTA_AUTH "X-Aius-SD-Auth: token\r\n"
#define OTA_ABSENT_BODY "{\"ok\":false,\"error\":\"ota_absent\"}"
#define OTA_CHUNK_BODY "{\"ok\":false,\"error\":\"ota_chunk\"}"

/* The carrier line, and why an OTA request needs one.
 *
 * lib/aius_http does not allocate the five /api/ota paths yet -- that is the
 * composition slice's half of this seam -- and aius_http_parse() refuses an
 * unknown path outright, so no byte sequence can reach the dispatcher AS an OTA
 * route today. Each verb therefore rides a real path whose method and body
 * rules match its own, and the verb itself is handed to the dispatcher
 * explicitly.
 *
 * What that costs is exactly one thing: the path-to-verb mapping. Everything
 * the OTA routes actually read -- the method-shaped framing, X-Aius-SD-Auth,
 * Content-Range and the body slice -- is still parsed from bytes a client could
 * send, by the real parser, so no golden below is anchored to a hand-built
 * struct. */
static const char *ota_carrier(AiusOpticalAppOtaRoute route)
{
	switch (route) {
	case AIUS_OPTICAL_APP_OTA_ROUTE_STATUS:
		return "GET /api/status"; /* GET, no body */
	case AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE:
		return "PUT /api/dsp/config"; /* buffered PUT carrying a body */
	default:
		return "POST /api/replays/stop"; /* POST, no body */
	}
}

static const AiusHttpApplicationResponse *dispatch_ota(
	Fixture *fixture, AiusOpticalAppOtaRoute route, const char *headers,
	const char *body, long declared_length)
{
	parse_request(fixture, ota_carrier(route), headers, body, declared_length);
	return aius_optical_app_ota_dispatch(&fixture->dispatcher, route,
	                                     &fixture->request);
}

/* One image chunk as a client sends it: the range that says where these bytes
 * belong, and the bytes. The payload is printable filler because this seam
 * never looks inside it -- it bounds the slice and hands it down. */
static const AiusHttpApplicationResponse *dispatch_ota_chunk(
	Fixture *fixture, char *scratch, size_t scratch_capacity,
	unsigned long start, unsigned long length, unsigned long total,
	const char *token)
{
	char headers[128];
	assert(length != 0u && length + 1u <= scratch_capacity);
	memset(scratch, 'A', length);
	scratch[length] = '\0';
	snprintf(headers, sizeof(headers),
	         "X-Aius-SD-Auth: %s\r\nContent-Range: bytes %lu-%lu/%lu\r\n",
	         token, start, start + length - 1u, total);
	return dispatch_ota(fixture, AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE, headers,
	                    scratch, (long)length);
}

static const AiusOpticalAppOtaRoute kOtaRoutes[] = {
	AIUS_OPTICAL_APP_OTA_ROUTE_STATUS, AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE,
	AIUS_OPTICAL_APP_OTA_ROUTE_ARM, AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY,
	AIUS_OPTICAL_APP_OTA_ROUTE_ABORT, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
	AIUS_OPTICAL_APP_OTA_ROUTE_ENABLE_ACTIVATION,
	AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST,
	AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE,
	AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT, AIUS_OPTICAL_APP_OTA_ROUTE_NONE
};

/* OTA is a compile-time option that defaults OFF, so "this build has no OTA" is
 * a state the dispatcher holds rather than a table it cannot construct. Both
 * spellings of absent -- never wired, and wired but reporting itself
 * unavailable -- answer one 404 for every verb INCLUDING a verb that is not one
 * (ROUTE_NONE), because the gate runs before the switch. A new verb cannot
 * dodge it by forgetting to check. */
static void test_ota_absent_gate(void)
{
	Fixture fixture;
	static const char *const present_calls[] = { "ota_present" };
	fixture_init(&fixture);

	/* Compiled out: no table was ever installed, and not one callback runs
	 * -- not even the credential check. */
	assert(aius_optical_app_set_ota(&fixture.dispatcher, NULL));
	for (size_t i = 0; i < ARRAY_LENGTH(kOtaRoutes); ++i) {
		expect_immediate(dispatch_ota(&fixture, kOtaRoutes[i], OTA_AUTH, NULL,
		                              -1),
		                 404, OTA_ABSENT_BODY);
		expect_log(&fixture, present_calls, 0u);
	}

	/* Compiled in, reported unavailable: the same 404 and the same body, so
	 * a page renders one disabled state rather than two. */
	fixture_wire_ota(&fixture);
	fixture.fake.ota_present = false;
	for (size_t i = 0; i < ARRAY_LENGTH(kOtaRoutes); ++i) {
		expect_immediate(dispatch_ota(&fixture, kOtaRoutes[i], OTA_AUTH, NULL,
		                              -1),
		                 404, OTA_ABSENT_BODY);
		expect_log(&fixture, present_calls, ARRAY_LENGTH(present_calls));
	}

	/* And an absent build never reaches a chunk decision either: the gate
	 * outranks every request-shape rule below it. */
	fixture.fake.last_ota_length = 0u;
	{
		char scratch[512];
		expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
		                                    0u, 256u, 1024u, "token"),
		                 404, OTA_ABSENT_BODY);
	}
	assert(fixture.fake.last_ota_length == 0u);
}

/* The table is all-or-nothing: a hole in any slot is refused at registration,
 * never discovered as a null call at dispatch. */
static void test_ota_table_registration(void)
{
	Fixture fixture;
	AiusOpticalAppOta ota;
	AiusOpticalAppDispatcher uninitialised;
	fixture_init(&fixture);
	ota = fake_ota(&fixture.fake);

	for (size_t i = 0; i < 10u; ++i) {
		AiusOpticalAppOta holed = fake_ota(&fixture.fake);
		void **slots[] = {
			(void **)&holed.present, (void **)&holed.status,
			(void **)&holed.chunk, (void **)&holed.arm,
			(void **)&holed.verify, (void **)&holed.abort,
			(void **)&holed.activate, (void **)&holed.enable_activation,
			(void **)&holed.self_test_flash, (void **)&holed.rehearse
		};
		assert(ARRAY_LENGTH(slots) == 10u);
		*slots[i] = NULL;
		assert(!aius_optical_app_set_ota(&fixture.dispatcher, &holed));
		/* Refused AND left absent -- fail-closed, not half-wired. */
		expect_immediate(dispatch_ota(&fixture,
		                              AIUS_OPTICAL_APP_OTA_ROUTE_ARM, OTA_AUTH,
		                              NULL, -1),
		                 404, OTA_ABSENT_BODY);
	}

	/* Removal is a request, not a failure; a dispatcher that never
	 * initialised is a failure. */
	assert(aius_optical_app_set_ota(&fixture.dispatcher, NULL));
	assert(!aius_optical_app_set_ota(NULL, &ota));
	memset(&uninitialised, 0, sizeof(uninitialised));
	assert(!aius_optical_app_set_ota(&uninitialised, &ota));

	/* The OTA entry point refuses exactly what the HTTP one refuses. */
	assert(aius_optical_app_ota_dispatch(NULL, AIUS_OPTICAL_APP_OTA_ROUTE_ARM,
	                                     &fixture.request) == NULL);
	assert(aius_optical_app_ota_dispatch(&fixture.dispatcher,
	                                     AIUS_OPTICAL_APP_OTA_ROUTE_ARM,
	                                     NULL) == NULL);
	assert(aius_optical_app_ota_dispatch(&uninitialised,
	                                     AIUS_OPTICAL_APP_OTA_ROUTE_ARM,
	                                     &fixture.request) == NULL);
}

/* Firmware replacement is the most dangerous surface this device has, so the
 * four mutating verbs carry the same credential the control routes do -- and
 * the gate is ordered before every request-shape rule, so an unauthenticated
 * malformed chunk is a 401 rather than a 400 that tells a stranger how the
 * protocol works.
 *
 * The open read is the deliberate exception: it mutates nothing, and a page
 * that cannot read it cannot render the disabled or TEST KEY state at all. */
static void test_ota_routes_are_authenticated(void)
{
	Fixture fixture;
	static const AiusOpticalAppOtaRoute mutating[] = {
		AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE, AIUS_OPTICAL_APP_OTA_ROUTE_ARM,
		AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY, AIUS_OPTICAL_APP_OTA_ROUTE_ABORT,
		AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
		AIUS_OPTICAL_APP_OTA_ROUTE_ENABLE_ACTIVATION,
		AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST,
		AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE,
		AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT
	};
	static const char *const refused_calls[] = {
		"ota_present", "auth_matches"
	};
	static const char *const status_calls[] = { "ota_present", "ota_status" };
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(mutating); ++i) {
		expect_immediate(dispatch_ota(&fixture, mutating[i], NULL, NULL, -1),
		                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
		expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
		expect_immediate(dispatch_ota(&fixture, mutating[i],
		                              "X-Aius-SD-Auth: wrong\r\n", NULL, -1),
		                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
		expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	}
	/* No refusal reached the OTA seam: nothing was staged, armed, verified
	 * or aborted on the way to a 401. */
	assert(fixture.fake.last_ota_length == 0u);

	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,
	                              NULL, NULL, -1),
	                 200,
	                 "{\"armed\":true,\"state\":\"staging\","
	                 "\"staged_bytes\":4096,\"total_bytes\":585728,"
	                 "\"next_offset\":4096,"
	                 "\"image\":{\"target_id\":\"teensy41-optical\","
	                 "\"build_revision\":\"fecc4a0a2473\","
	                 "\"build_utc\":1753900000},"
	                 "\"key\":{\"fingerprint\":\"9f86d081884c7d65\","
	                 "\"test_key\":true},"
	                 "\"activation_available\":false,"
	                 "\"fault\":\"none\",\"verifier_fault\":0}");
	expect_log(&fixture, status_calls, ARRAY_LENGTH(status_calls));
}

/* PUT /api/ota/image, chunk for chunk: the APPEND_PART protocol the replay
 * upload already answers with, applied to a firmware image. Self-contained
 * requests, next_offset answers, a resumable refusal -- and four bounded
 * request-shape rules the route owns before the application is asked anything.
 */
/* An accepted OTA chunk is now durably staged: the 200 is the SD job's, writing
 * the chunk's bytes to /aius/ota-staged.img at its container file offset through
 * the bounded engine (APPEND_PART), its body the next_offset ack. total is the
 * chunk's own byte count, exactly the replay APPEND_PART shape. */
static void expect_ota_stage(const AiusHttpApplicationResponse *response,
                             unsigned long next_offset, uint64_t total,
                             uint64_t initial_offset)
{
	char body[64];
	snprintf(body, sizeof(body), "{\"ok\":true,\"next_offset\":%lu}",
	         next_offset);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                body);
	assert(response->sd_job.kind == AIUS_SD_JOB_UPLOAD);
	assert(strcmp(response->sd_job.target_path, "/aius/ota-staged.img") == 0);
	assert(response->sd_job.total == total);
	assert(response->sd_job.initial_offset == initial_offset);
	if (initial_offset == 0u) {
		/* The first chunk RESTARTS the persistent staged file through the
		 * atomic REPLACE pair; strict append against last upgrade's file
		 * would refuse offset 0 forever (416 sd_range, no delete route). */
		assert(response->sd_job.install_mode == AIUS_SD_INSTALL_REPLACE);
		assert(response->sd_job.temp_path != NULL);
		assert(strncmp(response->sd_job.temp_path, "/aius/.upload-",
		               sizeof("/aius/.upload-") - 1u) == 0);
		assert(response->sd_job.backup_path != NULL);
		assert(strncmp(response->sd_job.backup_path, "/aius/.backup-",
		               sizeof("/aius/.backup-") - 1u) == 0);
	} else {
		assert(response->sd_job.install_mode ==
		       AIUS_SD_INSTALL_APPEND_PART);
		assert(response->sd_job.temp_path == NULL);
		assert(response->sd_job.backup_path == NULL);
	}
}

static void test_ota_image_chunks(void)
{
	Fixture fixture;
	char scratch[AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES + 2u];
	const AiusHttpApplicationResponse *response;
	static const char *const chunk_calls[] = {
		"ota_present", "auth_matches", "sd_ready", "sd_busy", "ota_chunk"
	};
	/* The first chunk also names the REPLACE scratch pair, whose entropy is
	 * the application's. */
	static const char *const chunk0_calls[] = {
		"ota_present", "auth_matches", "sd_ready", "sd_busy", "ota_chunk",
		"upload_id"
	};
	static const char *const refused_calls[] = {
		"ota_present", "auth_matches"
	};
	fixture_init(&fixture);

	/* In order. The first chunk carries the 256-byte container header, and the
	 * answer is the transport's own next offset -- but the 200 is now the SD
	 * job's, because the chunk is durably staged to /aius/ota-staged.img. */
	fixture.fake.ota_next_offset = 512u;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch), 0u, 512u,
	                              585728u, "token");
	expect_ota_stage(response, 512u, 512u, 0u);
	expect_log(&fixture, chunk0_calls, ARRAY_LENGTH(chunk0_calls));
	assert(fixture.fake.last_ota_offset == 0u);
	assert(fixture.fake.last_ota_length == 512u);
	/* The slice handed down IS the parsed request body: borrowed in place
	 * for the call, never copied into the dispatcher. */
	assert(fixture.fake.last_ota_data == fixture.request.body);

	/* Resume. A later chunk states its own offset, so a connection lost
	 * between chunks costs one request and nothing else. The staged write seeks
	 * to that container offset. */
	fixture.fake.ota_next_offset = 1536u;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
	                             1024u, 512u, 585728u, "token");
	expect_ota_stage(response, 1536u, 512u, 1024u);
	assert(fixture.fake.last_ota_offset == 1024u);

	/* A bad offset is the one refusal that carries an instruction rather
	 * than an apology: the core refuses out-of-order bytes without failing
	 * the session, so the answer says where to resume from. */
	fixture.fake.ota_chunk_result = AIUS_OPTICAL_APP_OTA_RANGE;
	fixture.fake.ota_next_offset = 1536u;
	expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
	                                    4096u, 512u, 585728u, "token"),
	                 409,
	                 "{\"ok\":false,\"error\":\"ota_range\","
	                 "\"next_offset\":1536}");
	expect_log(&fixture, chunk_calls, ARRAY_LENGTH(chunk_calls));
	fixture.fake.ota_chunk_result = AIUS_OPTICAL_APP_OTA_OK;

	/* HEADER FIRST. The container header is validated whole -- magic, CRC,
	 * identity, digest and signature slot together -- so half of one is not
	 * an early fragment of a valid offer, and the route refuses it before
	 * the application sees a byte. */
	fixture.fake.last_ota_length = 0u;
	expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch), 0u,
	                                    AIUS_OPTICAL_APP_OTA_HEADER_BYTES - 1u,
	                                    585728u, "token"),
	                 400, "{\"ok\":false,\"error\":\"ota_header\"}");
	expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	assert(fixture.fake.last_ota_length == 0u);

	/* Exactly one header is enough, and the rule is offset-zero only: a
	 * one-byte chunk further in is ordinary payload. Both stage. */
	fixture.fake.ota_next_offset = AIUS_OPTICAL_APP_OTA_HEADER_BYTES;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch), 0u,
	                             AIUS_OPTICAL_APP_OTA_HEADER_BYTES,
	                             585728u, "token");
	expect_ota_stage(response, AIUS_OPTICAL_APP_OTA_HEADER_BYTES,
	                 AIUS_OPTICAL_APP_OTA_HEADER_BYTES, 0u);
	fixture.fake.ota_next_offset = 257u;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
	                             256u, 1u, 585728u, "token");
	expect_ota_stage(response, 257u, 1u, 256u);
	assert(fixture.fake.last_ota_length == 1u);

	/* One chunk is one bounded epoch's worth of hashing. The cap is
	 * admitted exactly, and one byte past it is refused whole rather than
	 * clamped -- a truncated chunk would break the contiguous contract the
	 * next offset rests on. */
	fixture.fake.ota_next_offset = AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch), 0u,
	                             AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES,
	                             585728u, "token");
	expect_ota_stage(response, AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES,
	                 AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES, 0u);
	assert(fixture.fake.last_ota_length ==
	       AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES);
	fixture.fake.last_ota_length = 0u;
	expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch), 0u,
	                                    AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES +
	                                        1u,
	                                    585728u, "token"),
	                 400, OTA_CHUNK_BODY);
	expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	assert(fixture.fake.last_ota_length == 0u);

	/* No range: the bytes have no offset to be at, which is the refusal a
	 * Content-Range-less replay PUT already gets. A range that does not
	 * describe what arrived, and an empty body, are the same 400 -- and
	 * none of the three reaches the application. */
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE,
	                              OTA_AUTH, "payload", -1),
	                 400, OTA_CHUNK_BODY);
	expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE,
	                              OTA_AUTH
	                              "Content-Range: bytes 0-255/1024\r\n",
	                              "short", -1),
	                 400, OTA_CHUNK_BODY);
	expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE,
	                              OTA_AUTH
	                              "Content-Range: bytes 0-255/1024\r\n",
	                              NULL, 0),
	                 400, OTA_CHUNK_BODY);
	expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
	assert(fixture.fake.last_ota_length == 0u);

	/* Durable staging needs the card, so the chunk route now checks it -- after
	 * the request-shape rules (a malformed chunk is still 400, above) but BEFORE
	 * the application hashes, so a chunk the card cannot take never advances the
	 * session offset. A missing card is 503 sd_unavailable, a busy one 507
	 * ota_storage, and neither reaches ota_chunk. */
	{
		static const char *const sd_refused[] = {
			"ota_present", "auth_matches", "sd_ready"
		};
		static const char *const sd_busy_refused[] = {
			"ota_present", "auth_matches", "sd_ready", "sd_busy"
		};
		fixture.fake.last_ota_length = 0u;
		fixture.fake.sd_ready = false;
		expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
		                                    0u, 512u, 585728u, "token"),
		                 503, "{\"ok\":false,\"error\":\"sd_unavailable\"}");
		expect_log(&fixture, sd_refused, ARRAY_LENGTH(sd_refused));
		fixture.fake.sd_ready = true;
		fixture.fake.sd_busy = true;
		expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
		                                    0u, 512u, 585728u, "token"),
		                 507, "{\"ok\":false,\"error\":\"ota_storage\"}");
		expect_log(&fixture, sd_busy_refused, ARRAY_LENGTH(sd_busy_refused));
		fixture.fake.sd_busy = false;
		assert(fixture.fake.last_ota_length == 0u);
	}
}

/* Every verdict the seam can answer, mapped to exactly one status and one
 * token, proven through two different verbs so the map cannot drift per route.
 * RANGE is absent here because it is the chunk route's own answer and carries
 * next_offset; test_ota_image_chunks holds it. */
static void test_ota_verdict_map(void)
{
	Fixture fixture;
	char scratch[512];
	static const struct {
		AiusOpticalAppOtaResult result;
		unsigned status;
		const char *body;
	} verdicts[] = {
		{ AIUS_OPTICAL_APP_OTA_DISARMED, 409,
		  "{\"ok\":false,\"error\":\"ota_disarmed\"}" },
		{ AIUS_OPTICAL_APP_OTA_STATE_REFUSED, 409,
		  "{\"ok\":false,\"error\":\"ota_state\"}" },
		{ AIUS_OPTICAL_APP_OTA_MALFORMED, 400,
		  "{\"ok\":false,\"error\":\"ota_malformed\"}" },
		{ AIUS_OPTICAL_APP_OTA_UNSUPPORTED, 400,
		  "{\"ok\":false,\"error\":\"ota_unsupported\"}" },
		{ AIUS_OPTICAL_APP_OTA_DIGEST, 409,
		  "{\"ok\":false,\"error\":\"ota_digest\"}" },
		{ AIUS_OPTICAL_APP_OTA_SIGNATURE, 409,
		  "{\"ok\":false,\"error\":\"ota_signature\"}" },
		{ AIUS_OPTICAL_APP_OTA_STORAGE, 507,
		  "{\"ok\":false,\"error\":\"ota_storage\"}" }
	};
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(verdicts); ++i) {
		fixture.fake.ota_chunk_result = verdicts[i].result;
		expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
		                                    0u, 256u, 1024u, "token"),
		                 verdicts[i].status, verdicts[i].body);
		fixture.fake.ota_verify_result = verdicts[i].result;
		expect_immediate(dispatch_ota(&fixture,
		                              AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY,
		                              OTA_AUTH, NULL, -1),
		                 verdicts[i].status, verdicts[i].body);
	}
}

/* The three verdict-only verbs, and the one thing each accepted answer must
 * say: what changed. */
static void test_ota_verbs(void)
{
	Fixture fixture;
	uint32_t dispatches;
	static const char *const arm_calls[] = {
		"ota_present", "auth_matches", "ota_arm"
	};
	static const char *const verify_calls[] = {
		"ota_present", "auth_matches", "ota_verify"
	};
	static const char *const abort_calls[] = {
		"ota_present", "auth_matches", "ota_abort"
	};
	static const char *const refused_calls[] = {
		"ota_present", "auth_matches"
	};
	fixture_init(&fixture);

	/* Arming is the FEATURE LATCH, not the image: it makes an upgrade
	 * possible rather than starting one, so the body names the latch. */
	dispatches = fixture.dispatcher.http_dispatches;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ARM,
	                              OTA_AUTH, NULL, -1),
	                 200, "{\"ok\":true,\"armed\":true}");
	expect_log(&fixture, arm_calls, ARRAY_LENGTH(arm_calls));
	/* An OTA dispatch is an HTTP dispatch and counts exactly once. */
	assert(fixture.dispatcher.http_dispatches == dispatches + 1u);

	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY,
	                              OTA_AUTH, NULL, -1),
	                 200, "{\"ok\":true,\"state\":\"verified\"}");
	expect_log(&fixture, verify_calls, ARRAY_LENGTH(verify_calls));

	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ABORT,
	                              OTA_AUTH, NULL, -1),
	                 200, "{\"ok\":true,\"state\":\"idle\"}");
	expect_log(&fixture, abort_calls, ARRAY_LENGTH(abort_calls));

	/* The characteristic refusal of each verb: a latch that will not arm, a
	 * signature the verifier rejected, and the one state abort cannot leave
	 * (ACTIVATION_REQUESTED has no software exit by design). */
	fixture.fake.ota_arm_result = AIUS_OPTICAL_APP_OTA_UNSUPPORTED;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ARM,
	                              OTA_AUTH, NULL, -1),
	                 400, "{\"ok\":false,\"error\":\"ota_unsupported\"}");
	fixture.fake.ota_verify_result = AIUS_OPTICAL_APP_OTA_SIGNATURE;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"ota_signature\"}");
	fixture.fake.ota_abort_result = AIUS_OPTICAL_APP_OTA_STATE_REFUSED;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ABORT,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"ota_state\"}");

	/* A request that names no verb answers deterministically and touches
	 * nothing -- ROUTE_NONE is not a parsed route, so asking for one is not a
	 * way to find a half-built route. */
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_NONE,
	                              OTA_AUTH, NULL, -1),
	                 405, "{\"ok\":false,\"error\":\"ota_method\"}");
	expect_log(&fixture, refused_calls, ARRAY_LENGTH(refused_calls));
}

/* The activation-wiring slice's three verbs at the dispatch seam: each forwards
 * to its callback, maps the verdict through the one map, and answers its own
 * accepted body. The composition owns the runtime latch and the real flash
 * work; here the fake supplies the verdict, so this proves the seam plumbing
 * (which callback, which body, which status) and nothing about flash. */
static void test_ota_activation_verbs(void)
{
	Fixture fixture;
	static const char *const activate_calls[] = {
		"ota_present", "auth_matches", "ota_activate"
	};
	static const char *const enable_calls[] = {
		"ota_present", "auth_matches", "ota_enable_activation"
	};
	static const char *const self_test_calls[] = {
		"ota_present", "auth_matches", "ota_self_test_flash"
	};
	fixture_init(&fixture);

	/* Accepted answers. activate acknowledges before the deferred reset;
	 * enable-activation reports the latch flipped; self-test reports the pass
	 * the composition read back from the scratch sector. */
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
	                              OTA_AUTH, NULL, -1),
	                 200, "{\"ok\":true,\"activating\":true}");
	expect_log(&fixture, activate_calls, ARRAY_LENGTH(activate_calls));

	expect_immediate(dispatch_ota(&fixture,
	                              AIUS_OPTICAL_APP_OTA_ROUTE_ENABLE_ACTIVATION,
	                              OTA_AUTH, NULL, -1),
	                 200, "{\"ok\":true,\"activation_enabled\":true}");
	expect_log(&fixture, enable_calls, ARRAY_LENGTH(enable_calls));

	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST,
	                              OTA_AUTH, NULL, -1),
	                 200, "{\"ok\":true,\"self_test_flash\":\"pass\"}");
	expect_log(&fixture, self_test_calls, ARRAY_LENGTH(self_test_calls));

	/* The latch-off refusal: activation is not enabled on this device until the
	 * operator flips it after the bench recovery proof. Both the gated verbs
	 * answer 409 ota_activation_disabled. */
	fixture.fake.ota_activate_result = AIUS_OPTICAL_APP_OTA_ACTIVATION_DISABLED;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"ota_activation_disabled\"}");
	fixture.fake.ota_self_test_result =
		AIUS_OPTICAL_APP_OTA_ACTIVATION_DISABLED;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"ota_activation_disabled\"}");

	/* activate refuses a non-ARMED image with the generic state refusal (the
	 * composition maps request_activation's STATE there). */
	fixture.fake.ota_activate_result = AIUS_OPTICAL_APP_OTA_STATE_REFUSED;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"ota_state\"}");

	/* A self-test whose read-back mismatched is a device fault, not a client
	 * error: 500 ota_self_test_failed, distinct from the pass and the latch-off
	 * refusal so the bench operator can tell the three apart. */
	fixture.fake.ota_self_test_result = AIUS_OPTICAL_APP_OTA_SELF_TEST_FAILED;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST,
	                              OTA_AUTH, NULL, -1),
	                 500, "{\"ok\":false,\"error\":\"ota_self_test_failed\"}");

	/* The rehearsal pair: both routes reach the ONE callback with the arm
	 * flag that names them, and both acknowledge accepted-and-deferred --
	 * the verdict arrives later in ota_diag, not in this body. */
	{
		static const char *const rehearse_calls[] = {
			"ota_present", "auth_matches", "ota_rehearse"
		};
		expect_immediate(dispatch_ota(&fixture,
		                              AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE,
		                              OTA_AUTH, NULL, -1),
		                 200, "{\"ok\":true,\"rehearsing\":true}");
		expect_log(&fixture, rehearse_calls,
		           ARRAY_LENGTH(rehearse_calls));
		assert(!fixture.fake.last_ota_rehearse_injected);

		expect_immediate(
			dispatch_ota(&fixture,
			             AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT,
			             OTA_AUTH, NULL, -1),
			200,
			"{\"ok\":true,\"rehearsing\":true,"
			"\"fault_injection\":true}");
		expect_log(&fixture, rehearse_calls,
		           ARRAY_LENGTH(rehearse_calls));
		assert(fixture.fake.last_ota_rehearse_injected);

		/* The latch-off and busy refusals ride the same verdict map as
		 * every OTA verb. */
		fixture.fake.ota_rehearse_result =
			AIUS_OPTICAL_APP_OTA_ACTIVATION_DISABLED;
		expect_immediate(
			dispatch_ota(&fixture,
			             AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE,
			             OTA_AUTH, NULL, -1),
			409,
			"{\"ok\":false,\"error\":\"ota_activation_disabled\"}");
		fixture.fake.ota_rehearse_result =
			AIUS_OPTICAL_APP_OTA_STATE_REFUSED;
		expect_immediate(
			dispatch_ota(&fixture,
			             AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT,
			             OTA_AUTH, NULL, -1),
			409, "{\"ok\":false,\"error\":\"ota_state\"}");
	}
}

/* activation_available in GET /api/ota tracks the runtime latch the application
 * supplies, so a page offers the activate step exactly when the device will run
 * it. The fixture default is a board with the latch off (409 on activate); this
 * flips the document's field and proves the wire follows. */
static void test_ota_status_document_activation_available(void)
{
	Fixture fixture;
	char body[AIUS_HTTP_MAX_RESPONSE + 1u];
	fixture_init(&fixture);

	copy_body(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,
	                       NULL, NULL, -1),
	          body, sizeof(body));
	assert(strstr(body, "\"activation_available\":false") != NULL);

	fixture.fake.ota_document.activation_available = true;
	copy_body(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,
	                       NULL, NULL, -1),
	          body, sizeof(body));
	assert(strstr(body, "\"activation_available\":true") != NULL);
}

/* GET /api/ota -- the document a page narrates the whole upgrade from. */
static void test_ota_status_document(void)
{
	Fixture fixture;
	char body[AIUS_HTTP_MAX_RESPONSE + 1u];
	static const struct {
		AiusOpticalAppOtaState state;
		const char *name;
	} states[] = {
		{ AIUS_OPTICAL_APP_OTA_STATE_IDLE, "\"state\":\"idle\"" },
		{ AIUS_OPTICAL_APP_OTA_STATE_OFFERED, "\"state\":\"offered\"" },
		{ AIUS_OPTICAL_APP_OTA_STATE_STAGING, "\"state\":\"staging\"" },
		{ AIUS_OPTICAL_APP_OTA_STATE_STAGED, "\"state\":\"staged\"" },
		{ AIUS_OPTICAL_APP_OTA_STATE_VERIFIED, "\"state\":\"verified\"" },
		{ AIUS_OPTICAL_APP_OTA_STATE_ARMED, "\"state\":\"armed\"" },
		{ AIUS_OPTICAL_APP_OTA_STATE_ACTIVATION_REQUESTED,
		  "\"state\":\"activation_requested\"" },
		{ AIUS_OPTICAL_APP_OTA_STATE_FAILED, "\"state\":\"failed\"" }
	};
	/* An identity the document cannot draw: the two JSON string
	 * metacharacters, a control byte, and a field with no terminator at
	 * all. */
	static const char *const undrawable[] = {
		"aius\"optical", "aius\\optical", "aius\toptical"
	};
	fixture_init(&fixture);

	/* Every state the core can be in has a name a human can read, and no
	 * state prints as a bare number. */
	for (size_t i = 0; i < ARRAY_LENGTH(states); ++i) {
		fixture.fake.ota_document.state = states[i].state;
		copy_body(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,
		                       NULL, NULL, -1),
		          body, sizeof(body));
		assert(strstr(body, states[i].name) != NULL);
		/* THE FACT THIS SLICE EXISTS TO PUBLISH: activation ships later,
		 * and this is how the page knows not to offer the step. */
		assert(strstr(body, "\"activation_available\":false") != NULL);
	}
	fixture_init_ota_document(&fixture.fake);

	/* The key's identity passes through untouched, test-key flag included:
	 * the committed dev keypair is the device key for now and a page must
	 * be able to banner that. */
	fixture.fake.ota_document.test_key = false;
	snprintf(fixture.fake.ota_document.key_fingerprint,
	         sizeof(fixture.fake.ota_document.key_fingerprint),
	         "0011223344556677");
	fixture.fake.ota_document.fault = AIUS_OPTICAL_APP_OTA_SIGNATURE;
	fixture.fake.ota_document.verifier_fault = 3; /* FAULT_SIGNATURE */
	copy_body(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS, NULL,
	                       NULL, -1),
	          body, sizeof(body));
	assert(strstr(body,
	              "\"key\":{\"fingerprint\":\"0011223344556677\","
	              "\"test_key\":false}") != NULL);
	/* A latched fault is named with the same token the refusal used, not a
	 * second vocabulary, and the verifier's own code rides beside it. */
	assert(strstr(body, "\"fault\":\"ota_signature\",\"verifier_fault\":3") !=
	       NULL);
	fixture_init_ota_document(&fixture.fake);

	/* The widest identity the container admits is published whole. */
	memset(fixture.fake.ota_document.build_revision, 'a',
	       AIUS_OPTICAL_APP_OTA_TEXT_MAX);
	fixture.fake.ota_document.build_revision[AIUS_OPTICAL_APP_OTA_TEXT_MAX] =
		'\0';
	copy_body(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS, NULL,
	                       NULL, -1),
	          body, sizeof(body));
	assert(strstr(body,
	              "\"build_revision\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"") !=
	       NULL);

	/* One this document cannot draw is published EMPTY rather than escaped
	 * or truncated -- half a claim with a quote deleted from the middle is a
	 * different claim, and the body stays parseable JSON either way. */
	for (size_t i = 0; i < ARRAY_LENGTH(undrawable); ++i) {
		fixture_init_ota_document(&fixture.fake);
		snprintf(fixture.fake.ota_document.target_id,
		         sizeof(fixture.fake.ota_document.target_id), "%s",
		         undrawable[i]);
		copy_body(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,
		                       NULL, NULL, -1),
		          body, sizeof(body));
		assert(strstr(body, "\"target_id\":\"\"") != NULL);
		/* And the rest of the document is unaffected. */
		assert(strstr(body, "\"build_revision\":\"fecc4a0a2473\"") != NULL);
	}
	fixture_init_ota_document(&fixture.fake);
	memset(fixture.fake.ota_document.target_id, 'a',
	       sizeof(fixture.fake.ota_document.target_id));
	copy_body(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS, NULL,
	                       NULL, -1),
	          body, sizeof(body));
	assert(strstr(body, "\"target_id\":\"\"") != NULL);
	fixture_init_ota_document(&fixture.fake);

	/* An application that cannot answer is a 503, not a document of zeros. */
	fixture.fake.ota_status_ok = false;
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,
	                              NULL, NULL, -1),
	                 503, "{\"ok\":false,\"error\":\"ota_status\"}");
	fixture.fake.ota_status_ok = true;

	/* A response buffer too small for the document degrades to the same
	 * deterministic 500 every other over-long body produces. The composed
	 * runtime hands over AIUS_HTTP_MAX_RESPONSE; this is the floor. */
	{
		Fixture tiny;
		AiusOpticalApp app;
		uint8_t tiny_body[AIUS_OPTICAL_APP_MIN_BODY];
		memcpy(&tiny, &fixture, sizeof(tiny));
		app = fake_app(&tiny.fake);
		assert(aius_optical_app_init(&tiny.dispatcher, &app, tiny_body,
		                             sizeof(tiny_body)));
		fixture_wire_ota(&tiny);
		expect_immediate(dispatch_ota(&tiny, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS,
		                              NULL, NULL, -1),
		                 500, "{\"ok\":false,\"error\":\"response_too_large\"}");
	}
}

static void test_route_coverage(void)
{
	Fixture fixture;
	static const struct { const char *line; const char *headers;
	                      const char *body; long declared; } requests[] = {
		{ "GET /", NULL, NULL, -1 },
		{ "GET /api/status", NULL, NULL, -1 },
		{ "GET /api/events", NULL, NULL, -1 },
		{ "POST /api/start", "X-Aius-SD-Auth: token\r\n", NULL, -1 },
		{ "POST /api/stop", "X-Aius-SD-Auth: token\r\n", NULL, -1 },
		{ "POST /api/clear", "X-Aius-SD-Auth: token\r\n", NULL, -1 },
		{ "POST /api/reboot", "X-Aius-SD-Auth: token\r\n", NULL, -1 },
		{ "PUT /api/config/public", NULL, "{}", -1 },
		{ "GET /api/sd", NULL, NULL, -1 },
		{ "GET /sd/www/index.html", NULL, NULL, -1 },
		{ "GET /api/dsp/config", NULL, NULL, -1 },
		{ "PUT /api/dsp/config", "X-Aius-SD-Auth: token\r\n", "{}", -1 },
		{ "GET /api/meters", NULL, NULL, -1 },
		{ "GET /api/replays", NULL, NULL, -1 },
		{ "GET /api/replays/status", NULL, NULL, -1 },
		{ "POST /api/replays/start", "X-Aius-SD-Auth: token\r\n",
		  "{\"id\":\"take1\"}", -1 },
		{ "POST /api/replays/stop", "X-Aius-SD-Auth: token\r\n", NULL, -1 },
		{ "DELETE /api/replays", "X-Aius-SD-Auth: token\r\n",
		  "{\"id\":\"take1\"}", -1 },
		{ "GET /sd/replays/take1", NULL, NULL, -1 },
		{ "GET /api/measure", NULL, NULL, -1 },
		{ "POST /api/measure/start", "X-Aius-SD-Auth: token\r\n",
		  "{\"id\":\"pcm440_m6\"}", -1 },
		{ "GET /api/recorder", "X-Aius-SD-Auth: token\r\n", NULL, -1 },
		{ "POST /api/recorder/freeze", "X-Aius-SD-Auth: token\r\n", NULL,
		  -1 },
		{ "POST /api/recorder/thaw", "X-Aius-SD-Auth: token\r\n", NULL,
		  -1 },
		{ "PUT /api/recorder/config", "X-Aius-SD-Auth: token\r\n",
		  "{\"overrun\":true,\"underrun\":true,\"carrier_loss\":true,"
		  "\"limiter_gr\":false,\"gr_threshold_db\":0,"
		  "\"gr_hold_blocks\":1,\"postroll_blocks\":188}", -1 },
		{ "GET /api/recorder/data?offset=0&length=8&generation=0",
		  "X-Aius-SD-Auth: token\r\n", NULL, -1 }
	};
	bool seen[AIUS_HTTP_ROUTE_RECORDER_DATA + 1u];
	memset(seen, 0, sizeof(seen));
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(requests); ++i) {
		const AiusHttpApplicationResponse *response =
			dispatch_request(&fixture, requests[i].line, requests[i].headers,
			                 requests[i].body, requests[i].declared);
		assert(response != NULL);
		assert(response->status >= 200u && response->status < 600u);
		assert(response->content_type != NULL);
		assert(response->body_length == 0u || response->body != NULL);
		assert(fixture.request.route <= AIUS_HTTP_ROUTE_RECORDER_DATA);
		seen[fixture.request.route] = true;
	}
	/* AIUS_HTTP_ROUTE_UNKNOWN never survives the parser, so the inventory is
	 * every other enumerator. */
	for (unsigned route = AIUS_HTTP_ROUTE_HOME;
	     route <= AIUS_HTTP_ROUTE_RECORDER_DATA; ++route) {
		if (!seen[route])
			fprintf(stderr, "route %u has no dispatch coverage\n", route);
		assert(seen[route]);
	}
}

int main(void)
{
	RUN_TEST(test_init_validation);
	RUN_TEST(test_golden_immediate_routes);
	RUN_TEST(test_control_routes_are_authenticated);
	RUN_TEST(test_serializer_exhaustion);
	RUN_TEST(test_sd_media_and_paths);
	RUN_TEST(test_sd_dsp_model_carveout);
	RUN_TEST(test_sd_index_listing);
	RUN_TEST(test_sd_index_hidden_entries);
	RUN_TEST(test_sd_index_bounds);
	RUN_TEST(test_sd_index_refusals);
	RUN_TEST(test_sd_index_unwired);
	RUN_TEST(test_sd_delete);
	RUN_TEST(test_sd_upload);
	RUN_TEST(test_replay_chunk_upload);
	RUN_TEST(test_dsp_config_put);
	RUN_TEST(test_config_public_put);
	RUN_TEST(test_config_put_body_bounds);
	RUN_TEST(test_replay_control);
	RUN_TEST(test_replay_card_routes_are_gated);
	RUN_TEST(test_replay_body_whitespace);
	RUN_TEST(test_measure_control);
	RUN_TEST(test_callback_isolation);
	RUN_TEST(test_dispatch_counts);
	RUN_TEST(test_usb_commands);
	RUN_TEST(test_unreachable_method_defaults);
	RUN_TEST(test_recorder_routes);
	RUN_TEST(test_ota_absent_gate);
	RUN_TEST(test_ota_table_registration);
	RUN_TEST(test_ota_routes_are_authenticated);
	RUN_TEST(test_ota_image_chunks);
	RUN_TEST(test_ota_verdict_map);
	RUN_TEST(test_ota_verbs);
	RUN_TEST(test_ota_activation_verbs);
	RUN_TEST(test_ota_status_document);
	RUN_TEST(test_ota_status_document_activation_available);
	RUN_TEST(test_route_coverage);
	fprintf(stderr, "optical-app oracle: all tests passed\n");
	return 0;
}
