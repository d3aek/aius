/* optical-app oracle: one bounded application dispatch per HTTP route and USB
 * command.
 *
 * The fake application below is the whole seam: it owns no socket, no SD
 * handle, no clock and no delay, so a dispatch that reached for transport, SD,
 * the watchdog or a delay could not compile against it. Each test drives the
 * public entry points with requests produced by the real parser and asserts the
 * exact status, content type and body the pre-cutover firmware produces.
 */

#include "aius_dsp_config.h" /* first: declares the dsp.json serialiser */
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
	bool test_running;
	size_t stored_length;
	unsigned reboots;
	unsigned clears;
	unsigned sd_mode_calls;
	bool sd_mode_dma;
	bool sd_mode_busy;
	unsigned stops;

	/* What the last measure_start carried, and whether to accept it. */
	bool measure_start_result;
	char last_measure_id[AIUS_OPTICAL_APP_VECTOR_ID_STORAGE];
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
	/* Slot addressing + read-lease bookkeeping (Phase C). The read records
	 * what the dispatcher asked for and which slot it resolved to (the fake
	 * echoes an explicit slot and falls back to recorder_default_slot for a
	 * defaulted read -- on the real board that default is the NEWEST FROZEN
	 * slot, not the active one), and the lease acquire/release land in these
	 * counters so a test can prove the borrow's lease is taken then dropped. */
	bool last_recorder_has_slot;
	uint32_t last_recorder_slot;
	uint32_t recorder_default_slot;
	uint32_t recorder_lease_slot;
	uint32_t recorder_lease_acquires;
	uint32_t recorder_lease_releases;
	int32_t recorder_lease_balance;
	uint32_t last_recorder_release_slot;
	uint8_t recorder_data[16];
	uint32_t recorder_data_length;

	/* OTA: whether the surface exists at all, the verdict each verb hands
	 * back, what the last chunk carried, and the document the open read
	 * publishes. The fake holds no session -- the dispatcher holds no OTA
	 * state either, so a verdict per call is the whole seam. */
	bool ota_present;
	bool ota_status_ok;
	AiusOpticalAppOtaResult ota_chunk_result;
	AiusOpticalAppOtaResult ota_chunk_check_result;
	AiusOpticalAppOtaResult ota_arm_result;
	AiusOpticalAppOtaResult ota_verify_result;
	AiusOpticalAppOtaResult ota_abort_result;
	AiusOpticalAppOtaResult ota_activate_result;
	AiusOpticalAppOtaResult ota_enable_activation_result;
	AiusOpticalAppOtaResult ota_self_test_result;
	AiusOpticalAppOtaResult ota_rehearse_result;
	AiusOpticalAppOtaResult ota_boot_other_result;
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

static void fake_set_test_running(void *context, bool running)
{
	Fake *fake = (Fake *)context;
	record(fake, running ? "test_running:true" : "test_running:false");
	fake->test_running = running;
}

static bool fake_sd_set_mode(void *context, bool dma)
{
	Fake *fake = (Fake *)context;
	record(fake, "sd_set_mode");
	if (fake->sd_mode_busy)
		return false;
	fake->sd_mode_calls++;
	fake->sd_mode_dma = dma;
	return true;
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
	void *context, bool has_slot, uint32_t slot, uint32_t offset_bytes,
	uint32_t length_bytes, uint32_t generation, const uint8_t **data,
	uint32_t *length, uint32_t *resolved_slot)
{
	Fake *fake = (Fake *)context;
	uint32_t target = has_slot ? slot : fake->recorder_default_slot;
	record(fake, "recorder_read");
	fake->last_recorder_has_slot = has_slot;
	fake->last_recorder_slot = slot;
	fake->last_recorder_offset = offset_bytes;
	fake->last_recorder_length = length_bytes;
	fake->last_recorder_generation = generation;
	*resolved_slot = target;
	if (fake->recorder_read_result == AIUS_OPTICAL_APP_RECORDER_READ_OK) {
		*data = fake->recorder_data;
		*length = fake->recorder_data_length;
		/* The real handler takes the addressed slot's read lease on OK; the
		 * fake mirrors that so a test can watch acquire/release balance. */
		fake->recorder_lease_slot = target;
		fake->recorder_lease_acquires++;
		fake->recorder_lease_balance++;
	}
	return fake->recorder_read_result;
}

static void fake_recorder_read_release(void *context, uint32_t slot)
{
	Fake *fake = (Fake *)context;
	record(fake, "recorder_read_release");
	fake->last_recorder_release_slot = slot;
	fake->recorder_lease_releases++;
	fake->recorder_lease_balance--;
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

static AiusOpticalAppOtaResult fake_ota_chunk_check(void *context,
                                                    uint32_t offset,
                                                    const uint8_t *data,
                                                    size_t length,
                                                    uint32_t *next_offset)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_chunk_check");
	/* The same bounded-slice contract as the mutating chunk. */
	assert(data != NULL);
	assert(length != 0u && length <= AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES);
	assert(next_offset != NULL);
	fake->last_ota_offset = offset;
	fake->last_ota_length = length;
	fake->last_ota_data = data;
	*next_offset = fake->ota_next_offset;
	return fake->ota_chunk_check_result;
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

static AiusOpticalAppOtaResult fake_ota_boot_other(void *context)
{
	Fake *fake = (Fake *)context;
	record(fake, "ota_boot_other");
	return fake->ota_boot_other_result;
}

static AiusOpticalAppOta fake_ota(Fake *fake)
{
	AiusOpticalAppOta ota;
	memset(&ota, 0, sizeof(ota));
	ota.context = fake;
	ota.present = fake_ota_present;
	ota.status = fake_ota_status;
	ota.chunk = fake_ota_chunk;
	ota.chunk_check = fake_ota_chunk_check;
	ota.arm = fake_ota_arm;
	ota.verify = fake_ota_verify;
	ota.abort = fake_ota_abort;
	ota.activate = fake_ota_activate;
	ota.enable_activation = fake_ota_enable_activation;
	ota.self_test_flash = fake_ota_self_test_flash;
	ota.rehearse = fake_ota_rehearse;
	ota.boot_other = fake_ota_boot_other;
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
	app.store_public_config = fake_store_public_config;
	app.store_dsp_config = fake_store_dsp_config;
	app.read_directory = fake_read_directory;
	app.set_test_running = fake_set_test_running;
	app.clear_counters = fake_clear_counters;
	app.sd_set_mode = fake_sd_set_mode;
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
	app.recorder_read_release = fake_recorder_read_release;
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
	fixture->fake.ota_chunk_check_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_arm_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_verify_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_abort_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_activate_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_enable_activation_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_self_test_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_rehearse_result = AIUS_OPTICAL_APP_OTA_OK;
	fixture->fake.ota_boot_other_result = AIUS_OPTICAL_APP_OTA_OK;
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
	for (size_t i = 0; i < 23u; ++i) {
		AiusOpticalApp holed = fake_app(&fake);
		void **slots[] = {
			(void **)&holed.sd_ready, (void **)&holed.sd_busy,
			(void **)&holed.auth_matches, (void **)&holed.write_home,
			(void **)&holed.write_status, (void **)&holed.write_events,
			(void **)&holed.write_dsp_config, (void **)&holed.write_meters,
			(void **)&holed.store_public_config,
			(void **)&holed.store_dsp_config,
			(void **)&holed.set_test_running, (void **)&holed.clear_counters,
			(void **)&holed.request_reboot, (void **)&holed.write_usb_payload,
			(void **)&holed.upload_id, (void **)&holed.write_measure,
			(void **)&holed.measure_start, (void **)&holed.write_recorder,
			(void **)&holed.recorder_freeze, (void **)&holed.recorder_thaw,
			(void **)&holed.recorder_config, (void **)&holed.recorder_read,
			(void **)&holed.recorder_read_release
		};
		assert(ARRAY_LENGTH(slots) == 23u);
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
	/* Replay is retired (Phase 4i): its routes are unknown, which the
	 * transaction layer answers 404 (ERR_ROUTE), before any dispatch. */
	{
		static const char *const dead[] = {
			"GET /api/replays HTTP/1.0\r\n\r\n",
			"GET /api/replays/status HTTP/1.0\r\n\r\n",
			"POST /api/replays/start HTTP/1.0\r\nContent-Length: 15\r\n\r\n"
			"{\"id\":\"take1\"}\n",
			"POST /api/replays/stop HTTP/1.0\r\n\r\n",
			"DELETE /api/replays HTTP/1.0\r\n\r\n"
		};
		AiusHttpRequest dead_request;
		for (size_t i = 0; i < ARRAY_LENGTH(dead); ++i)
			assert(aius_http_parse((const uint8_t *)dead[i], strlen(dead[i]),
			                       &dead_request) == AIUS_HTTP_ERR_ROUTE);
	}

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

	/* POST /api/sd/mode (Phase 4e, E3): auth-gated like the other control
	 * routes; {"dma":<bool>} only; 503 sd_busy while a file is open. */
	expect_immediate(dispatch_request(&fixture, "POST /api/sd/mode",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"dma\": true}", -1),
	                 200, "{\"ok\":true,\"sd_mode\":\"dma\"}");
	assert(fixture.fake.sd_mode_calls == 1u && fixture.fake.sd_mode_dma);
	expect_immediate(dispatch_request(&fixture, "POST /api/sd/mode",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"dma\":false}", -1),
	                 200, "{\"ok\":true,\"sd_mode\":\"fifo\"}");
	assert(fixture.fake.sd_mode_calls == 2u && !fixture.fake.sd_mode_dma);
	expect_immediate(dispatch_request(&fixture, "POST /api/sd/mode",
	                                  "", "{\"dma\":true}", -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "POST /api/sd/mode",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"dma\":1}", -1),
	                 400, "{\"ok\":false,\"error\":\"sd_mode_body\"}");
	expect_immediate(dispatch_request(&fixture, "POST /api/sd/mode",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"dma\":true,\"x\":1}", -1),
	                 400, "{\"ok\":false,\"error\":\"sd_mode_body\"}");
	assert(fixture.fake.sd_mode_calls == 2u);
	fixture.fake.sd_mode_busy = true;
	expect_immediate(dispatch_request(&fixture, "POST /api/sd/mode",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{\"dma\":true}", -1),
	                 503, "{\"ok\":false,\"error\":\"sd_busy\"}");
	assert(fixture.fake.sd_mode_calls == 2u && !fixture.fake.sd_mode_dma);
	fixture.fake.sd_mode_busy = false;
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
		"GET /api/dsp/config"
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
		"GET /sd/replays/take1", /* replay retired: no longer a route of its own */
		"GET /sd/www/../secret",
		"GET /sd/www/a//b",
		"GET /sd/www/%2e%2e/x",
		"GET /sd/www/sub/../../x"
	};
	/* max_age pins the dispatcher's caching policy per suffix: day-long for
	 * the two stable asset kinds (.js/.css), zero for everything else --
	 * html so a card sync propagates, json because it is live data, binary
	 * because it covers presets. */
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
		{ "GET /sd/www/a/b/c/d.bin", "application/octet-stream", 0u },
		/* Phase 4g G2: fonts join the day-long set. */
		{ "GET /sd/www/fonts/a.woff2", "application/octet-stream", 86400u }
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
		/* Phase 4g G2, G7: every www/ read varies by encoding, and what is
		 * not day-long is no-cache; the card fallback carries no ETag. */
		assert(response->vary_accept_encoding);
		assert(response->cache_no_cache == (accepted[i].max_age == 0u));
		assert(response->etag == NULL);
		assert(!response->content_encoding_gzip);
	}

	{
		const AiusHttpApplicationResponse *response =
			dispatch_get(&fixture, "GET /sd/www/index.html");
		assert(strcmp(response->sd_job.target_path, "/aius/www/index.html") == 0);
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

	/* Replay retired: /sd/replays/ is an ordinary non-www SD file path. With a
	 * card the upload is refused as a path (507), with none the media gate
	 * answers first (503); neither reaches a job. No overlay in this fixture. */
	expect_immediate(dispatch_request(&fixture, "PUT /sd/replays/take1",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 507, "{\"ok\":false,\"error\":\"sd_upload\"}");
	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_request(&fixture, "PUT /sd/replays/take1",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 503, "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	fixture.fake.sd_ready = true;
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

/* RFC 8259 §2 makes whitespace insignificant before and after every structural
 * character, so a body is identified by what it means, not by how it is spaced
 * (compact JSON.stringify and spaced json.dumps are the same request).
 * Whitespace inside the quotes is data: it survives into the id and then fails
 * the id charset. */
static void test_measure_body_whitespace(void)
{
	static const char *const accepted[] = {
		"{\"id\":\"take1\"}",
		"{\"id\": \"take1\"}",
		"{ \"id\" : \"take1\" }",
		"  {\"id\":\"take1\"}  ",
		"\r\n\t{\"id\":\"take1\"}\n",
		"{\r\n  \"id\" :\t\"take1\"\r\n}",
		"{\"id\": \"take1\", \"f0_l\": 440, \"f0_r\": 440}",
		"{\"f0_r\" : 440 , \"id\" : \"take1\" , \"f0_l\" : 440}",
		"{\"id\": \"take1\", \"probes\": [ 40 , 100 ]}",
	};
	static const char *const rejected[] = {
		"{}", "{\"id\":\"\"}", "{\"id\":\"bad id\"}", "{\"id\":\"bad/id\"}",
		"{\"id\":\"unterminated}",
		"{\"id\": \"bad id\"}", "{\"id\":\"\tbad\"}", "{\"id\":\"take1 \"}",
		"{ \"id\" : \" \" }",
		"", "   ", "{", "}", "{\"id\": }", "{\"id\" \"take1\"}",
		"{\"id\": \"take1\",}",
		"{\"id\": \"take1\"} trailing", "{\"id\": \"take1\"}{",
		"[{\"id\": \"take1\"}]", "\"take1\"",
		"{\"id\": 1}", "{\"id\": true}", "{\"id\": [\"take1\"]}",
		"{\"id\": \"take1\", \"speed\": 2}", "{\"identity\": \"take1\"}",
		"{\"id\": \"take1\", \"id\": \"take2\"}",
		"{\"id\": \"take1\", \"loop\": true}",
		"{\"id\":\"take1\",\"f0_l\":440,\"f0_l\":440}",
		"{\"id\":\"take1\",\"f0_l\":4.5}",
		"{\"nested\":{\"id\":\"take1\"}}",
		"not json at all \"id\":\"take1\" either",
		/* 49 chars: one past AIUS_HTTP_MAX_VECTOR_ID */
		"{\"id\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"
	};
	Fixture fixture;
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(accepted); ++i) {
		fixture.fake.last_measure_id[0] = '\0';
		expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
		                                  "X-Aius-SD-Auth: token\r\n",
		                                  accepted[i], -1),
		                 200,
		                 "{\"ok\":true,\"state\":\"armed\",\"id\":\"take1\"}");
		assert(strcmp(fixture.fake.last_measure_id, "take1") == 0);
	}
	for (size_t i = 0; i < ARRAY_LENGTH(rejected); ++i) {
		const AiusHttpApplicationResponse *response;
		fixture.fake.last_measure_id[0] = '\0';
		response = dispatch_request(&fixture, "POST /api/measure/start",
		                            "X-Aius-SD-Auth: token\r\n", rejected[i], -1);
		if (response == NULL || response->status != 400)
			fprintf(stderr, "body \"%s\" was not refused\n", rejected[i]);
		expect_immediate(response, 400,
		                 "{\"ok\":false,\"error\":\"measure_body\"}");
		assert(fixture.fake.last_measure_id[0] == '\0');
	}

	/* The longest id the route admits still parses when spaced out. */
	expect_immediate(dispatch_request(&fixture, "POST /api/measure/start",
	                                  "X-Aius-SD-Auth: token\r\n",
	                                  "{ \"id\" : "
	                                  "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" }",
	                                  -1),
	                 200,
	                 "{\"ok\":true,\"state\":\"armed\",\"id\":\""
	                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
	assert(strlen(fixture.fake.last_measure_id) == AIUS_HTTP_MAX_VECTOR_ID);
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

/* The 405 method fall-through the parser rejects before dispatch ever sees
 * it (POST to /sd/<file>), plus the unknown-route tail. They are the dispatcher's
 * own last line, so they are driven from a hand-built request rather than the
 * parser -- and their bodies are asserted byte-for-byte like every other. */
static void test_unreachable_method_defaults(void)
{
	Fixture fixture;
	static const uint8_t resource[] = "www/index.html";
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
		"\"limiter_gr\":true,\"governor_trim\":true,"
		"\"missed_fill\":true,\"crc_reject\":true,"
		"\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}";
	static const struct { const char *line; const char *body; } gated[] = {
		{ "GET /api/recorder", NULL },
		{ "POST /api/recorder/freeze", NULL },
		{ "POST /api/recorder/thaw", NULL },
		{ "GET /api/recorder/data?offset=0&length=8&generation=0", NULL },
		{ "PUT /api/recorder/config",
		  "{\"overrun\":true,\"underrun\":true,\"carrier_loss\":true,"
		  "\"limiter_gr\":false,\"governor_trim\":true,"
		  "\"missed_fill\":true,\"crc_reject\":true,"
		  "\"gr_threshold_db\":0,"
		  "\"gr_hold_blocks\":1,\"postroll_blocks\":188}" }
	};
	/* Refused shapes: a missing key (twice -- once for the newest flag, so
	 * an added key is proven REQUIRED and not merely accepted), a
	 * duplicate, an unknown key, a positive threshold, an
	 * exponent-carrying number, and trailing bytes. */
	static const char *const bad_bodies[] = {
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"governor_trim\":true,"
		"\"missed_fill\":true,\"crc_reject\":true,"
		"\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"governor_trim\":true,"
		"\"missed_fill\":true,"
		"\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}",
		"{\"overrun\":true,\"overrun\":true,\"underrun\":false,"
		"\"carrier_loss\":true,\"limiter_gr\":true,"
		"\"governor_trim\":true,\"missed_fill\":true,\"crc_reject\":true,"
		"\"gr_threshold_db\":-6.5,\"gr_hold_blocks\":3,"
		"\"postroll_blocks\":0}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"governor_trim\":true,"
		"\"missed_fill\":true,\"crc_reject\":true,"
		"\"gr_threshold_db\":-6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0,\"extra\":1}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"governor_trim\":true,"
		"\"missed_fill\":true,\"crc_reject\":true,"
		"\"gr_threshold_db\":6.5,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"governor_trim\":true,"
		"\"missed_fill\":true,\"crc_reject\":true,"
		"\"gr_threshold_db\":-1e2,"
		"\"gr_hold_blocks\":3,\"postroll_blocks\":0}",
		"{\"overrun\":true,\"underrun\":false,\"carrier_loss\":true,"
		"\"limiter_gr\":true,\"governor_trim\":true,"
		"\"missed_fill\":true,\"crc_reject\":true,"
		"\"gr_threshold_db\":-6.5,"
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
	 * seven booleans, the dB as scanned, hold and post-roll verbatim
	 * (post-roll 0 is legal: freeze at the trigger boundary). */
	expect_immediate(dispatch_request(&fixture, "PUT /api/recorder/config",
	                                  auth, good_config, -1),
	                 200, "{\"ok\":true}");
	assert(fixture.fake.last_recorder_mask ==
	       (AIUS_OPTICAL_APP_RECORDER_MASK_OVERRUN |
	        AIUS_OPTICAL_APP_RECORDER_MASK_CARRIER_LOSS |
	        AIUS_OPTICAL_APP_RECORDER_MASK_LIMITER_GR |
	        AIUS_OPTICAL_APP_RECORDER_MASK_GOVERNOR_TRIM |
	        AIUS_OPTICAL_APP_RECORDER_MASK_MISSED_FILL |
	        AIUS_OPTICAL_APP_RECORDER_MASK_CRC_REJECT));
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
		/* No &slot= -- the handler is told has_slot is false and owns the
		 * default resolution (on the board: the newest frozen slot). */
		assert(!fixture.fake.last_recorder_has_slot);
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

/* GET /api/recorder/data slot addressing + read lease (Phase C). The parser
 * threads an optional &slot=; the dispatcher passes it through, takes the
 * addressed slot's read lease on a 200, and releases it on the next dispatch --
 * the borrow invariant's end-of-life for the immediate response. */
static void test_recorder_slot_addressing_and_lease(void)
{
	Fixture fixture;
	static const char *const auth = "X-Aius-SD-Auth: token\r\n";
	fixture_init(&fixture);
	fixture.fake.recorder_default_slot = 1u;

	/* An explicit slot is threaded verbatim: has_slot set, slot echoed. The
	 * 200 borrows the ring, the addressed slot's lease is taken, and the
	 * response carries its own completion hook (sd_settle) pointing at the
	 * dispatcher -- the release path, NOT a next-dispatch. */
	{
		const AiusHttpApplicationResponse *response = dispatch_request(
			&fixture,
			"GET /api/recorder/data?offset=0&length=8&generation=2&slot=3",
			auth, NULL, -1);
		assert(response != NULL);
		assert(response->kind == AIUS_HTTP_APP_IMMEDIATE);
		assert(response->status == 200u);
		assert(strcmp(response->content_type,
		              "application/octet-stream") == 0);
		assert(response->body == fixture.fake.recorder_data);
		assert(fixture.fake.last_recorder_has_slot);
		assert(fixture.fake.last_recorder_slot == 3u);
		assert(fixture.fake.recorder_lease_acquires == 1u);
		assert(fixture.fake.recorder_lease_slot == 3u);
		assert(fixture.fake.recorder_lease_releases == 0u);
		assert(fixture.fake.recorder_lease_balance == 1);
		/* The completion hook is registered on the response itself. */
		assert(response->sd_settle != NULL);
		assert(response->sd_settle_ctx == &fixture.dispatcher);

		/* THE GAP-CLOSED PROOF: firing the response's completion hook --
		 * what the transaction does at send-completion/close -- drops the
		 * lease to zero WITHOUT any subsequent dispatch. A one-shot client
		 * that GETs and closes no longer wedges the slot's recycle. */
		unsigned dispatches_before = fixture.dispatcher.http_dispatches;
		response->sd_settle(response->sd_settle_ctx, false);
		assert(fixture.fake.recorder_lease_releases == 1u);
		assert(fixture.fake.last_recorder_release_slot == 3u);
		assert(fixture.fake.recorder_lease_balance == 0);
		assert(fixture.dispatcher.http_dispatches == dispatches_before);
		/* Exactly once: a second fire (a hook the transaction would have
		 * nulled) releases nothing further -- no underflow. */
		response->sd_settle(response->sd_settle_ctx, false);
		assert(fixture.fake.recorder_lease_releases == 1u);
		assert(fixture.fake.recorder_lease_balance == 0);
	}

	/* Rapid back-to-back multi-slot pull (aius-output-jitter.md Phase E):
	 * each read's lease is released by ITS OWN completion hook before the
	 * next read's is taken, so the balance never climbs past one and two
	 * leases never wedge open. */
	{
		const AiusHttpApplicationResponse *first = dispatch_request(
			&fixture,
			"GET /api/recorder/data?offset=0&length=8&generation=5&slot=0",
			auth, NULL, -1);
		assert(first->status == 200u);
		assert(fixture.fake.recorder_lease_slot == 0u);
		assert(fixture.fake.recorder_lease_balance == 1);
		first->sd_settle(first->sd_settle_ctx, false);
		assert(fixture.fake.last_recorder_release_slot == 0u);
		assert(fixture.fake.recorder_lease_balance == 0);

		const AiusHttpApplicationResponse *second = dispatch_request(
			&fixture,
			"GET /api/recorder/data?offset=0&length=8&generation=6&slot=2",
			auth, NULL, -1);
		assert(second->status == 200u);
		assert(fixture.fake.recorder_lease_slot == 2u);
		assert(fixture.fake.recorder_lease_balance == 1);
		second->sd_settle(second->sd_settle_ctx, false);
		assert(fixture.fake.last_recorder_release_slot == 2u);
		assert(fixture.fake.recorder_lease_balance == 0);
		assert(fixture.fake.recorder_lease_acquires == 3u);
		assert(fixture.fake.recorder_lease_releases == 3u);
	}

	/* A defaulted read (no &slot=) is resolved by the handler, not the
	 * dispatcher: has_slot is false and the fake echoes recorder_default_slot
	 * into the lease; its own hook releases it. */
	{
		const AiusHttpApplicationResponse *response = dispatch_request(
			&fixture,
			"GET /api/recorder/data?offset=0&length=8&generation=9",
			auth, NULL, -1);
		assert(response->status == 200u);
		assert(!fixture.fake.last_recorder_has_slot);
		assert(fixture.fake.recorder_lease_slot == 1u);
		assert(fixture.fake.recorder_lease_acquires == 4u);
		response->sd_settle(response->sd_settle_ctx, false);
		assert(fixture.fake.last_recorder_release_slot == 1u);
		assert(fixture.fake.recorder_lease_balance == 0);
	}

	/* A refused read (stale/evicted generation) is 409, takes NO lease, and
	 * carries NO completion hook -- so nothing is released and nothing leaks. */
	fixture.fake.recorder_read_result = AIUS_OPTICAL_APP_RECORDER_READ_REFUSED;
	{
		const AiusHttpApplicationResponse *response = dispatch_request(
			&fixture,
			"GET /api/recorder/data?offset=0&length=8&generation=1&slot=2",
			auth, NULL, -1);
		expect_immediate(response, 409,
		                 "{\"ok\":false,\"error\":\"recorder_state\"}");
		assert(response->sd_settle == NULL);
	}
	assert(fixture.fake.recorder_lease_acquires == 4u);
	assert(fixture.fake.recorder_lease_releases == 4u);
	assert(fixture.fake.recorder_lease_balance == 0);
}

/* A data read that finds a lease STILL active releases it before taking the
 * new one, and counts the occurrence.
 *
 * This is the second half of the teardown leak: the transaction's completion
 * hook is the normal release path, but a path that bypasses it (a network
 * teardown that discarded the transaction before the fix, or any future
 * equivalent) leaves recorder_lease_active set. Overwriting recorder_lease_slot
 * on the next read would strand the old slot's reads_in_flight at 1 forever --
 * the bank then defers its round-robin when it reaches that slot and recording
 * stops until reboot. So the dispatcher repairs it, and records that it had to:
 * recorder_lease_leaks is a diagnostic for a path that should not exist, not a
 * supported mode. */
static void test_recorder_lease_double_acquire_releases(void)
{
	Fixture fixture;
	static const char *const auth = "X-Aius-SD-Auth: token\r\n";
	fixture_init(&fixture);

	const AiusHttpApplicationResponse *first = dispatch_request(
		&fixture,
		"GET /api/recorder/data?offset=0&length=8&generation=1&slot=1",
		auth, NULL, -1);
	assert(first->status == 200u);
	assert(fixture.fake.recorder_lease_slot == 1u);
	assert(fixture.fake.recorder_lease_balance == 1);
	assert(fixture.dispatcher.recorder_lease_leaks == 0u);

	/* The hook is deliberately NEVER fired -- exactly what a teardown
	 * mid-stream did. The next read must repair the leak, not compound it.
	 * (`first` is invalidated by this dispatch: the response lives in the
	 * dispatcher's single slot.) */
	const AiusHttpApplicationResponse *second = dispatch_request(
		&fixture,
		"GET /api/recorder/data?offset=0&length=8&generation=2&slot=3",
		auth, NULL, -1);
	assert(second->status == 200u);
	assert(fixture.dispatcher.recorder_lease_leaks == 1u);
	/* The OLD slot was released, not the new one, and exactly once. */
	assert(fixture.fake.recorder_lease_releases == 1u);
	assert(fixture.fake.last_recorder_release_slot == 1u);
	assert(fixture.fake.recorder_lease_acquires == 2u);
	assert(fixture.fake.recorder_lease_slot == 3u);
	assert(fixture.fake.recorder_lease_balance == 1);

	/* The second response's own hook still closes it out normally, and the
	 * repair counter does not move again. */
	second->sd_settle(second->sd_settle_ctx, false);
	assert(fixture.fake.last_recorder_release_slot == 3u);
	assert(fixture.fake.recorder_lease_releases == 2u);
	assert(fixture.fake.recorder_lease_balance == 0);
	assert(fixture.dispatcher.recorder_lease_leaks == 1u);

	/* A clean read after a clean release is not counted as a leak. */
	const AiusHttpApplicationResponse *third = dispatch_request(
		&fixture,
		"GET /api/recorder/data?offset=0&length=8&generation=3&slot=0",
		auth, NULL, -1);
	assert(third->status == 200u);
	assert(fixture.dispatcher.recorder_lease_leaks == 1u);
	third->sd_settle(third->sd_settle_ctx, false);
	assert(fixture.fake.recorder_lease_balance == 0);
}

/* ------------------------------------------------------------------ */
/* OTA -- staged firmware upgrade                                       */
/* ------------------------------------------------------------------ */

#define OTA_AUTH "X-Aius-SD-Auth: token\r\n"
#define OTA_ABSENT_BODY "{\"ok\":false,\"error\":\"ota_absent\"}"
#define BOOT_OTHER_ACCEPTED \
	"{\"ok\":true,\"rebooting\":true,\"boot_other\":true}"
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
		return "POST /api/clear"; /* POST, no body */
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
	AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT,
	AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER, AIUS_OPTICAL_APP_OTA_ROUTE_NONE
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

	for (size_t i = 0; i < 12u; ++i) {
		AiusOpticalAppOta holed = fake_ota(&fixture.fake);
		void **slots[] = {
			(void **)&holed.present, (void **)&holed.status,
			(void **)&holed.chunk, (void **)&holed.chunk_check,
			(void **)&holed.arm,
			(void **)&holed.verify, (void **)&holed.abort,
			(void **)&holed.activate, (void **)&holed.enable_activation,
			(void **)&holed.self_test_flash, (void **)&holed.rehearse,
			(void **)&holed.boot_other
		};
		assert(ARRAY_LENGTH(slots) == 12u);
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
		AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT,
		AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER
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
		"ota_present", "auth_matches", "sd_ready", "sd_busy",
		"ota_chunk_check"
	};
	/* The first chunk also names the REPLACE scratch pair, whose entropy is
	 * the application's. */
	static const char *const chunk0_calls[] = {
		"ota_present", "auth_matches", "sd_ready", "sd_busy",
		"ota_chunk_check", "upload_id"
	};
	static const char *const settle_calls[] = { "ota_chunk" };
	static const char *const refused_calls[] = {
		"ota_present", "auth_matches"
	};
	fixture_init(&fixture);

	/* In order. The first chunk carries the 256-byte container header. The
	 * 200 is the SD job's (durably staged to /aius/ota-staged.img), and the
	 * SESSION IS NOT TOUCHED AT DISPATCH: the route runs only the pure
	 * chunk_check, and the mutating chunk fires at the write's COMMITTED
	 * terminal through the response's settle hook (Phase 4a). The ack's
	 * next_offset is arithmetic -- start + length -- because the check
	 * pinned start to the session's next expected offset. */
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
	/* The promise is armed: the settle hook is on the response and the
	 * pending stash on the dispatcher, borrowing the same body slice. */
	assert(response->sd_settle != NULL);
	assert(response->sd_settle_ctx == &fixture.dispatcher);
	assert(fixture.dispatcher.ota_pending);
	assert(fixture.dispatcher.ota_pending_offset == 0u);
	assert(fixture.dispatcher.ota_pending_length == 512u);
	assert(fixture.dispatcher.ota_pending_data == fixture.request.body);
	/* THE COMMITTED TERMINAL: the transaction fires the hook, the deferred
	 * absorb runs with the stashed identity, and the pending clears. */
	fixture_reset_log(&fixture);
	response->sd_settle(response->sd_settle_ctx, true);
	expect_log(&fixture, settle_calls, ARRAY_LENGTH(settle_calls));
	assert(fixture.fake.last_ota_offset == 0u);
	assert(fixture.fake.last_ota_length == 512u);
	assert(!fixture.dispatcher.ota_pending);
	/* Settling twice is inert -- the transaction's exactly-once rule has a
	 * second guard here. */
	fixture_reset_log(&fixture);
	response->sd_settle(response->sd_settle_ctx, true);
	expect_log(&fixture, NULL, 0u);

	/* Resume. A later chunk states its own offset, so a connection lost
	 * between chunks costs one request and nothing else. The staged write seeks
	 * to that container offset. */
	fixture.fake.ota_next_offset = 1536u;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
	                             1024u, 512u, 585728u, "token");
	expect_ota_stage(response, 1536u, 512u, 1024u);
	assert(fixture.fake.last_ota_offset == 1024u);

	/* A FAULTED write settles uncommitted: the pending clears and the
	 * mutating chunk NEVER runs -- the session has not moved, so the
	 * client's 500 is honestly resumable (the incident's poisoned-session
	 * shape, closed). */
	assert(fixture.dispatcher.ota_pending);
	fixture_reset_log(&fixture);
	response->sd_settle(response->sd_settle_ctx, false);
	expect_log(&fixture, NULL, 0u);
	assert(!fixture.dispatcher.ota_pending);

	/* ONE ABSORB IN FLIGHT: while a promised chunk awaits its terminal the
	 * route answers the retryable 503, and the window closes with the
	 * settle. */
	fixture.fake.ota_next_offset = 1536u;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
	                              1024u, 512u, 585728u, "token");
	expect_ota_stage(response, 1536u, 512u, 1024u);
	assert(fixture.dispatcher.ota_pending);
	{
		const AiusHttpApplicationResponse *second =
			dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
			                   1536u, 512u, 585728u, "token");
		expect_immediate(second, 503,
		                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	}
	/* The refusal's dispatch rebuilt the request buffer, so settle the
	 * pending WITHOUT applying (its borrowed slice is gone -- exactly the
	 * released-connection shape the drain backstop clears). */
	fixture_reset_log(&fixture);
	aius_optical_app_ota_pending_clear(&fixture.dispatcher);
	assert(!fixture.dispatcher.ota_pending);

	/* A bad offset is the one refusal that carries an instruction rather
	 * than an apology: the core refuses out-of-order bytes without failing
	 * the session, so the answer says where to resume from. */
	fixture.fake.ota_chunk_check_result = AIUS_OPTICAL_APP_OTA_RANGE;
	fixture.fake.ota_next_offset = 1536u;
	expect_immediate(dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
	                                    4096u, 512u, 585728u, "token"),
	                 409,
	                 "{\"ok\":false,\"error\":\"ota_range\","
	                 "\"next_offset\":1536}");
	expect_log(&fixture, chunk_calls, ARRAY_LENGTH(chunk_calls));
	fixture.fake.ota_chunk_check_result = AIUS_OPTICAL_APP_OTA_OK;

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
	 * one-byte chunk further in is ordinary payload. Both stage (settled
	 * between, since one absorb may be in flight at a time). */
	fixture.fake.ota_next_offset = AIUS_OPTICAL_APP_OTA_HEADER_BYTES;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch), 0u,
	                             AIUS_OPTICAL_APP_OTA_HEADER_BYTES,
	                             585728u, "token");
	expect_ota_stage(response, AIUS_OPTICAL_APP_OTA_HEADER_BYTES,
	                 AIUS_OPTICAL_APP_OTA_HEADER_BYTES, 0u);
	response->sd_settle(response->sd_settle_ctx, true);
	fixture.fake.ota_next_offset = 257u;
	response = dispatch_ota_chunk(&fixture, scratch, sizeof(scratch),
	                             256u, 1u, 585728u, "token");
	expect_ota_stage(response, 257u, 1u, 256u);
	assert(fixture.fake.last_ota_length == 1u);
	response->sd_settle(response->sd_settle_ctx, true);

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
	response->sd_settle(response->sd_settle_ctx, true);
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
	 * session offset. A missing card is 503 sd_unavailable; a busy one is 503
	 * sd_busy -- the retryable token every SD route speaks, NOT 507, because a
	 * mid-stream chunk colliding with a root SD job is a transient hold a plain
	 * retry resolves (the 2026-08-04 defect-2 conviction). Neither reaches
	 * ota_chunk. */
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
		                 503, "{\"ok\":false,\"error\":\"sd_busy\"}");
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
		  "{\"ok\":false,\"error\":\"ota_storage\"}" },
		{ AIUS_OPTICAL_APP_OTA_IVT, 409,
		  "{\"ok\":false,\"error\":\"ota_ivt\"}" }
	};
	fixture_init(&fixture);

	for (size_t i = 0; i < ARRAY_LENGTH(verdicts); ++i) {
		fixture.fake.ota_chunk_check_result = verdicts[i].result;
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
		  "\"limiter_gr\":false,\"governor_trim\":true,"
		  "\"missed_fill\":true,\"crc_reject\":true,"
		  "\"gr_threshold_db\":0,"
		  "\"gr_hold_blocks\":1,\"postroll_blocks\":188}", -1 },
		{ "GET /api/recorder/data?offset=0&length=8&generation=0",
		  "X-Aius-SD-Auth: token\r\n", NULL, -1 },
		{ "GET /api/status/brief", NULL, NULL, -1 }
	};
	bool seen[AIUS_HTTP_ROUTE_OTA_BOOT_OTHER + 1u];
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
		assert(fixture.request.route <= AIUS_HTTP_ROUTE_OTA_BOOT_OTHER);
		seen[fixture.request.route] = true;
	}
	/* Phase 4f: the brief route is dispatched (404 without the optional
	 * writer, which this fixture leaves unwired). */
	assert(seen[AIUS_HTTP_ROUTE_STATUS_BRIEF]);
	/* AIUS_HTTP_ROUTE_UNKNOWN never survives the parser, so the inventory is
	 * every other enumerator. */
	for (unsigned route = AIUS_HTTP_ROUTE_HOME;
	     route <= AIUS_HTTP_ROUTE_RECORDER_DATA; ++route) {
		if (!seen[route])
			fprintf(stderr, "route %u has no dispatch coverage\n", route);
		assert(seen[route]);
	}
}

/* ------------------------------------------------------------------ */
/* Phase 4f: brief route, dispatch meter hooks, admission decisions     */
/* ------------------------------------------------------------------ */

static struct {
	unsigned brief_calls, begins, ends;
	AiusHttpRoute end_route;
	AiusHttpMethod end_method;
} g_4f;

static size_t fake_write_brief(void *context, uint8_t *body, size_t capacity)
{
	(void)context;
	g_4f.brief_calls++;
	return (size_t)snprintf((char *)body, capacity, "{\"carrier\":1}");
}

static void fake_dispatch_begin(void *context)
{
	(void)context;
	assert(g_4f.begins == g_4f.ends);
	g_4f.begins++;
}

static void fake_dispatch_end(void *context, AiusHttpRoute route,
                              AiusHttpMethod method)
{
	(void)context;
	g_4f.ends++;
	g_4f.end_route = route;
	g_4f.end_method = method;
}

static void test_status_brief_and_dispatch_hooks(void)
{
	Fixture fixture;
	memset(&g_4f, 0, sizeof(g_4f));
	fixture_init(&fixture);
	/* Optional: absent, the route answers 404 and no hook is required. */
	expect_immediate(dispatch_get(&fixture, "GET /api/status/brief"), 404,
	                 "{\"ok\":false}");
	fixture.dispatcher.app.write_status_brief = fake_write_brief;
	fixture.dispatcher.app.dispatch_begin = fake_dispatch_begin;
	fixture.dispatcher.app.dispatch_end = fake_dispatch_end;
	expect_immediate(dispatch_get(&fixture, "GET /api/status/brief"), 200,
	                 "{\"carrier\":1}");
	assert(g_4f.brief_calls == 1u);
	assert(g_4f.begins == 1u && g_4f.ends == 1u);
	assert(g_4f.end_route == AIUS_HTTP_ROUTE_STATUS_BRIEF);
	assert(g_4f.end_method == AIUS_HTTP_GET);
	expect_immediate(dispatch_get(&fixture, "GET /api/status"), 200,
	                 "{\"body\":\"status\"}");
	assert(g_4f.begins == 2u && g_4f.ends == 2u);
	assert(g_4f.end_route == AIUS_HTTP_ROUTE_STATUS);
	/* A refused control route is still bracketed (its stretch is metered). */
	(void)dispatch_request(&fixture, "POST /api/clear", NULL, NULL, -1);
	assert(g_4f.begins == 3u && g_4f.ends == 3u);
	assert(g_4f.end_route == AIUS_HTTP_ROUTE_CLEAR);
}

static AiusOpticalAdmitSample admit_sample(bool live, uint32_t level,
                                           bool fifo, uint32_t now_ms)
{
	AiusOpticalAdmitSample s;
	memset(&s, 0, sizeof(s));
	s.live_owns_ring = live;
	s.level_frames = level;
	s.fifo_mode = fifo;
	s.now_ms = now_ms;
	return s;
}

/* level at which `cost_us` is exactly admitted: 128 + ceil(cost * 48 / 1000) + 48. */
static uint32_t admit_level(uint32_t cost_us)
{
	return 128u + (cost_us * 48u + 999u) / 1000u + 48u;
}

static void test_admission_classes(void)
{
	static const struct { AiusHttpRoute route; AiusHttpMethod method;
	                      AiusOpticalAdmitClass klass; } classes[] = {
		{ AIUS_HTTP_ROUTE_STATUS, AIUS_HTTP_GET, AIUS_OPTICAL_ADMIT_STATUS },
		{ AIUS_HTTP_ROUTE_STATUS_BRIEF, AIUS_HTTP_GET, AIUS_OPTICAL_ADMIT_BRIEF },
		{ AIUS_HTTP_ROUTE_METERS, AIUS_HTTP_GET, AIUS_OPTICAL_ADMIT_METERS },
		{ AIUS_HTTP_ROUTE_EVENTS, AIUS_HTTP_GET, AIUS_OPTICAL_ADMIT_EVENTS },
		{ AIUS_HTTP_ROUTE_DSP_CONFIG, AIUS_HTTP_PUT, AIUS_OPTICAL_ADMIT_SETTINGS },
		{ AIUS_HTTP_ROUTE_CONFIG_PUBLIC, AIUS_HTTP_PUT, AIUS_OPTICAL_ADMIT_SETTINGS },
	};
	/* F13: never shed, whatever the margin. */
	static const struct { AiusHttpRoute route; AiusHttpMethod method; } never[] = {
		{ AIUS_HTTP_ROUTE_DSP_CONFIG, AIUS_HTTP_GET },
		{ AIUS_HTTP_ROUTE_SD_FILE, AIUS_HTTP_GET },
		{ AIUS_HTTP_ROUTE_RECORDER, AIUS_HTTP_GET },
		{ AIUS_HTTP_ROUTE_RECORDER_DATA, AIUS_HTTP_GET },
		{ AIUS_HTTP_ROUTE_RECORDER_CONFIG, AIUS_HTTP_PUT },
		{ AIUS_HTTP_ROUTE_START, AIUS_HTTP_POST },
		{ AIUS_HTTP_ROUTE_STOP, AIUS_HTTP_POST },
		{ AIUS_HTTP_ROUTE_CLEAR, AIUS_HTTP_POST },
		{ AIUS_HTTP_ROUTE_REBOOT, AIUS_HTTP_POST },
		{ AIUS_HTTP_ROUTE_SD_MODE, AIUS_HTTP_POST },
		{ AIUS_HTTP_ROUTE_OTA, AIUS_HTTP_GET },
		{ AIUS_HTTP_ROUTE_OTA_IMAGE, AIUS_HTTP_PUT },
		{ AIUS_HTTP_ROUTE_OTA_ACTIVATE, AIUS_HTTP_POST },
		{ AIUS_HTTP_ROUTE_HOME, AIUS_HTTP_GET },
	};
	AiusOpticalAdmission a;
	AiusOpticalAdmitSample s;

	aius_optical_admission_init(&a);
	for (size_t i = 0; i < ARRAY_LENGTH(classes); ++i) {
		AiusOpticalAdmitClass k = classes[i].klass;
		uint32_t cost = aius_optical_admission_cost_us(&a, k);
		uint32_t level;
		assert(aius_optical_admit_route_class(classes[i].route,
		                                      classes[i].method) == k);
		/* At boot the first dispatch is judged against the seed. */
		assert(cost > 0u);
		level = admit_level(cost);
		s = admit_sample(true, level, false, 0u);
		assert(aius_optical_admission_route(&a, classes[i].route,
		                                    classes[i].method, &s) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);
		s = admit_sample(true, level - 1u, false, 0u);
		assert(aius_optical_admission_route(&a, classes[i].route,
		                                    classes[i].method, &s) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
		/* F11: the gate flag clear (or set with the carrier unlocked, a
		 * replay) is no ownership: level 0 admits. live_owns_ring is the
		 * composition's one predicate, so both states arrive here as false. */
		s = admit_sample(false, 0u, false, 0u);
		assert(aius_optical_admission_route(&a, classes[i].route,
		                                    classes[i].method, &s) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);
		/* Live at level 0: every sheddable class sheds. */
		s = admit_sample(true, 0u, false, 0u);
		assert(aius_optical_admission_route(&a, classes[i].route,
		                                    classes[i].method, &s) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
	}
	assert(a.shed[AIUS_OPTICAL_ADMIT_SETTINGS] == 4u);
	assert(a.shed[AIUS_OPTICAL_ADMIT_STATUS] == 2u);
	/* The measured cost moves the boundary: 0.3 ms of longest stretch (a 9 ms
	 * dispatch whose nested passes the meter excluded) replaces the seed. */
	aius_optical_admission_note_dispatch(&a, AIUS_HTTP_ROUTE_STATUS,
	                                     AIUS_HTTP_GET, 300u, 0u);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_STATUS) == 300u);
	s = admit_sample(true, admit_level(300u), false, 0u);
	assert(aius_optical_admission_route(&a, AIUS_HTTP_ROUTE_STATUS,
	                                    AIUS_HTTP_GET, &s) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);

	for (size_t i = 0; i < ARRAY_LENGTH(never); ++i) {
		assert(aius_optical_admit_route_class(never[i].route, never[i].method) ==
		       AIUS_OPTICAL_ADMIT_NONE);
		s = admit_sample(true, 0u, true, 0u);
		assert(aius_optical_admission_route(&a, never[i].route,
		                                    never[i].method, &s) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);
	}
	/* An unlisted route's stretch still raises the watermark (F13). */
	aius_optical_admission_note_dispatch(&a, AIUS_HTTP_ROUTE_DSP_CONFIG,
	                                     AIUS_HTTP_GET, 4321u, 0u);
	assert(a.dispatch_max_us == 4321u);

	/* F8: POST /api/clear keeps every cost, resets the counters. */
	{
		uint32_t before[AIUS_OPTICAL_ADMIT_CLASSES];
		for (unsigned c = 0; c < AIUS_OPTICAL_ADMIT_CLASSES; ++c)
			before[c] = aius_optical_admission_cost_us(&a, (AiusOpticalAdmitClass)c);
		a.sd_fifo_defers = 9u;
		aius_optical_admission_clear(&a);
		for (unsigned c = 0; c < AIUS_OPTICAL_ADMIT_CLASSES; ++c) {
			assert(aius_optical_admission_cost_us(&a, (AiusOpticalAdmitClass)c) ==
			       before[c]);
			assert(a.shed[c] == 0u);
		}
		assert(a.dispatch_max_us == 0u && a.sd_fifo_defers == 0u);
	}
	assert(sizeof(a.window) == 128u); /* F12, F-B1: eight 16 B windows */
}

/* F12: an outlier stays the cost for at least 64 and at most 79 further
 * samples (gone after the 80th), from both bucket edges. */
static void test_admission_outlier_window(void)
{
	for (unsigned edge = 0; edge < 2u; ++edge) {
		AiusOpticalAdmission a;
		unsigned lead = edge == 0u ? 0u : AIUS_OPTICAL_ADMIT_BUCKET - 1u;
		unsigned gone_after = edge == 0u ? 80u : 65u;
		aius_optical_admission_init(&a);
		/* Fill whole buckets of 100 us so the outlier lands at a bucket
		 * edge: the first sample replaces the seed. */
		for (unsigned i = 0; i < 4u * AIUS_OPTICAL_ADMIT_BUCKET + lead; ++i)
			aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_METERS, 100u, 0u);
		aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_METERS, 9000u, 0u);
		for (unsigned k = 1; k <= 90u; ++k) {
			aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_METERS, 100u, 0u);
			uint32_t cost = aius_optical_admission_cost_us(&a,
			                                               AIUS_OPTICAL_ADMIT_METERS);
			if (k <= 64u)
				assert(cost == 9000u);
			if (k < gone_after)
				assert(cost == 9000u);
			else
				assert(cost == 100u);
		}
	}
	/* Saturation at 65,535 us. */
	{
		AiusOpticalAdmission a;
		aius_optical_admission_init(&a);
		aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_EVENTS, 200000u, 0u);
		assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_EVENTS) ==
		       65535u);
	}
}

static void test_admission_sd_start(void)
{
	AiusOpticalAdmission a;
	AiusOpticalAdmitSample s;
	AiusSdJobRequest download, ota;
	uint32_t dma_seed, fifo_seed;

	memset(&download, 0, sizeof(download));
	download.kind = AIUS_SD_JOB_DOWNLOAD;
	download.target_path = "/aius/www/index.html";
	memset(&ota, 0, sizeof(ota));
	ota.kind = AIUS_SD_JOB_UPLOAD;
	ota.target_path = "/aius/" AIUS_OPTICAL_APP_OTA_STAGED_NAME;
	assert(aius_optical_app_sd_job_is_ota_staging(&ota));
	assert(!aius_optical_app_sd_job_is_ota_staging(&download));

	aius_optical_admission_init(&a);
	dma_seed = aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SD_START_DMA);
	fifo_seed = aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SD_START_FIFO);
	assert(dma_seed > 0u && fifo_seed > dma_seed);

	/* DMA under margin: 503 at once, no job. */
	s = admit_sample(true, admit_level(dma_seed) - 1u, false, 0u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
	assert(a.shed[AIUS_OPTICAL_ADMIT_SD_START_DMA] == 1u);
	s = admit_sample(true, admit_level(dma_seed), false, 0u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);

	/* Not owning the ring: FIFO at level 0 neither defers nor sheds. */
	s = admit_sample(false, 0u, true, 0u);
	assert(!aius_optical_admission_sd_fifo_defer(&a, &s));
	assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);

	/* F-B2: bring-up. FIFO is the boot mode and live input owns the ring
	 * with the margin short, yet PROBE, then LOAD_CONFIG, then LOAD_DSP each
	 * start (no hold, none counted); an HTTP SD start is still deferred. The
	 * latch is set when LOAD_DSP ends, whatever it found, and kept by clear;
	 * from then on a root start is held. */
	s = admit_sample(true, 0u, true, 0u);
	assert(!aius_optical_admission_sd_fifo_defer(&a, &s)); /* PROBE */
	assert(!aius_optical_admission_sd_fifo_defer(&a, &s)); /* LOAD_CONFIG */
	assert(!aius_optical_admission_sd_fifo_defer(&a, &s)); /* LOAD_DSP */
	assert(a.sd_fifo_defers == 0u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_DEFER);
	a.sd_fifo_defers = 0u;
	a.defer_timing = 0u;
	aius_optical_admission_note_bringup_loaded(&a); /* LOAD_DSP ended */
	aius_optical_admission_clear(&a);
	assert(a.bringup_loaded != 0u);

	/* Round 4: an armed upload staging is orphaned only when the transaction
	 * can no longer start its write. */
	{
		static AiusHttpTransaction tx;
		memset(&tx, 0, sizeof(tx));
		tx.state = AIUS_HTTP_TX_IDLE; /* a teardown re-inited it */
		assert(aius_optical_app_sd_staging_orphaned(&tx, false, false));
		assert(!aius_optical_app_sd_staging_orphaned(&tx, true, false));
		assert(!aius_optical_app_sd_staging_orphaned(&tx, false, true));
		assert(!aius_optical_app_sd_staging_orphaned(NULL, false, false));
		tx.state = AIUS_HTTP_TX_DISPATCH;    /* the arming epoch */
		assert(!aius_optical_app_sd_staging_orphaned(&tx, false, false));
		tx.state = AIUS_HTTP_TX_SD_START;
		assert(!aius_optical_app_sd_staging_orphaned(&tx, false, false));
		tx.state = AIUS_HTTP_TX_SD_TRANSFER;
		assert(!aius_optical_app_sd_staging_orphaned(&tx, false, false));
		tx.state = AIUS_HTTP_TX_RECEIVE;     /* the next request */
		assert(aius_optical_app_sd_staging_orphaned(&tx, false, false));
	}

	/* FIFO, short, after bring-up: a root job's start is held. */
	assert(aius_optical_admission_sd_fifo_defer(&a, &s));
	assert(aius_optical_admission_sd_fifo_defer(&a, &s));
	assert(a.sd_fifo_defers == 2u);
	/* DMA mode never defers. */
	s = admit_sample(true, 0u, false, 0u);
	assert(!aius_optical_admission_sd_fifo_defer(&a, &s));

	/* An HTTP start held 500 ms of clock time answers 503, whether 20 or 500
	 * consults ran in that time (R4-N2). */
	for (unsigned step = 25u; step != 0u; step = step == 25u ? 1u : 0u) {
		uint32_t t0 = 1000u, t;
		aius_optical_admission_init(&a);
		for (t = t0; t < t0 + 500u; t += step) {
			s = admit_sample(true, 0u, true, t);
			assert(aius_optical_admission_sd_start(&a, &download, &s,
			                                       t != t0) ==
			       AIUS_OPTICAL_ADMIT_VERDICT_DEFER);
		}
		assert(a.sd_fifo_defers == 500u / step);
		s = admit_sample(true, 0u, true, t0 + 500u);
		assert(aius_optical_admission_sd_start(&a, &download, &s, true) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
		assert(a.shed[AIUS_OPTICAL_ADMIT_SD_START_FIFO] == 1u);
		/* SHED restarts the timer: the next start defers afresh. */
		s = admit_sample(true, 0u, true, t0 + 600u);
		assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_DEFER);
	}

	/* OTA staging chunks start with the margin short, in either mode (F9). */
	s = admit_sample(true, 0u, true, 0u);
	assert(aius_optical_admission_sd_start(&a, &ota, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);
	s = admit_sample(true, 0u, false, 0u);
	assert(aius_optical_admission_sd_start(&a, &ota, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);

	/* R4-S1: one window per mode, kept across clear and a mode change, so the
	 * first start after either is judged against the seed or the last
	 * window, never 0. */
	aius_optical_admission_init(&a);
	assert(a.bringup_loaded == 0u); /* init (boot) clears the latch */
	aius_optical_admission_note_bringup_loaded(&a);
	aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_SD_START_FIFO, 30000u, 0u);
	aius_optical_admission_clear(&a);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SD_START_FIFO) ==
	       30000u);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SD_START_DMA) ==
	       dma_seed);
	s = admit_sample(true, admit_level(dma_seed) - 1u, false, 0u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
	s = admit_sample(true, admit_level(30000u) - 1u, true, 0u);
	assert(aius_optical_admission_sd_fifo_defer(&a, &s));
	s = admit_sample(true, admit_level(30000u), true, 0u);
	assert(!aius_optical_admission_sd_fifo_defer(&a, &s));

	/* F-S2: the timer belongs to the held start. A teardown drops the held
	 * start 300 ms in; the next start's first consult restarts the timer, so
	 * it is held its own 500 ms instead of shedding at once. */
	aius_optical_admission_init(&a);
	s = admit_sample(true, 0u, true, 1000u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_DEFER);
	s = admit_sample(true, 0u, true, 1300u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, true) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_DEFER);
	s = admit_sample(true, 0u, true, 1600u); /* a different start */
	assert(aius_optical_admission_sd_start(&a, &download, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_DEFER);
	s = admit_sample(true, 0u, true, 2099u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, true) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_DEFER);
	assert(a.shed[AIUS_OPTICAL_ADMIT_SD_START_FIFO] == 0u);
	s = admit_sample(true, 0u, true, 2100u);
	assert(aius_optical_admission_sd_start(&a, &download, &s, true) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
	assert(a.shed[AIUS_OPTICAL_ADMIT_SD_START_FIFO] == 1u);
}

/* F-B1: a window ages in wall time. A shed class adds no sample, yet regains
 * admission once its outlier's buckets age out on the fake clock (a bucket
 * closes 2,000 ms after it opened; the outlier's slot is overwritten at the
 * fifth close, 10 s after its bucket opened). */
/* Phase 4g G6, G9, R5-S1: the web-cache fill hold. "Starts" means the
 * scheduler would start a fill file: aius_optical_web_fill_held() false. */
static void test_web_fill_hold(void)
{
	AiusOpticalWebFill f;
	bool cancel = false;

	/* Idle: nothing holds; an HTTP SD start with no fill in flight goes to
	 * 4f admission untouched and latches nothing. */
	aius_optical_web_fill_init(&f);
	assert(!aius_optical_web_fill_held(&f, 0u));
	assert(!aius_optical_web_fill_sd_start(&f, false, &cancel));
	assert(!cancel && f.holds == 0u && f.latches == 0u);
	assert(!aius_optical_web_fill_take_defer(&f));

	/* A page miss finding a fill in flight: the first consult cancels
	 * once (the platform's cancel entry latches), later consults defer
	 * without cancelling again; the hold has no expiry while latched. */
	assert(aius_optical_web_fill_sd_start(&f, true, &cancel));
	assert(cancel);
	assert(aius_optical_web_fill_cancel(&f, true));
	assert(f.holds == 1u && f.cancels == 1u && f.latches == 1u);
	assert(aius_optical_web_fill_sd_start(&f, true, &cancel));
	assert(!cancel && f.cancels == 1u && f.latches == 1u);
	/* The fill has stopped: the held start goes to admission as a first
	 * consult exactly once, then starts; no fill starts in between. */
	assert(!aius_optical_web_fill_sd_start(&f, false, &cancel));
	assert(aius_optical_web_fill_take_defer(&f));
	assert(!aius_optical_web_fill_take_defer(&f));
	for (uint32_t t = 0u; t <= 600000u; t += 50000u)
		assert(aius_optical_web_fill_held(&f, t));
	/* Its job completes at 1000 ms: the release, 2 s after it. */
	aius_optical_web_fill_note_http_complete(&f, 1000u);
	assert(f.latches == 0u && f.http_canceller == 0u);
	assert(aius_optical_web_fill_held(&f, 2999u));
	assert(!aius_optical_web_fill_held(&f, 3000u));

	/* Every HTTP SD job completion extends to 2 s after it; an expiry only
	 * moves later (a completion stamped earlier does not pull it in). */
	aius_optical_web_fill_note_http_complete(&f, 4000u);
	aius_optical_web_fill_note_http_complete(&f, 5000u);
	aius_optical_web_fill_note_http_complete(&f, 4500u);
	assert(aius_optical_web_fill_held(&f, 6999u));
	assert(!aius_optical_web_fill_held(&f, 7000u));

	/* The 4h flush and freeze shape: a cancel with no HTTP job after it
	 * keeps the hold with no expiry until its release, then the next file
	 * starts 2 s after the release and never before. */
	aius_optical_web_fill_init(&f);
	assert(!aius_optical_web_fill_cancel(&f, false));
	assert(f.holds == 1u && f.cancels == 0u);
	assert(aius_optical_web_fill_held(&f, 1000000u));
	aius_optical_web_fill_note_http_complete(&f, 10000u);
	assert(aius_optical_web_fill_held(&f, 20000u)); /* still latched */
	aius_optical_web_fill_hold_release(&f, 30000u);
	for (uint32_t t = 30000u; t < 32000u; t += 100u)
		assert(aius_optical_web_fill_held(&f, t));
	assert(!aius_optical_web_fill_held(&f, 32000u));
	/* Two cancellers: the hold lapses only after both release. */
	aius_optical_web_fill_init(&f);
	(void)aius_optical_web_fill_cancel(&f, true);
	(void)aius_optical_web_fill_cancel(&f, false);
	aius_optical_web_fill_hold_release(&f, 100u);
	assert(aius_optical_web_fill_held(&f, 100000u));
	aius_optical_web_fill_hold_release(&f, 200u);
	assert(!aius_optical_web_fill_held(&f, 2200u));
	/* Expiry wraps with millis(). */
	aius_optical_web_fill_init(&f);
	aius_optical_web_fill_note_http_complete(&f, UINT32_MAX - 500u);
	assert(aius_optical_web_fill_held(&f, 1000u));
	assert(!aius_optical_web_fill_held(&f, 1500u));

	/* An HTTP canceller whose request ends with no job (shed, teardown)
	 * releases as its completion would have. */
	aius_optical_web_fill_init(&f);
	assert(aius_optical_web_fill_sd_start(&f, true, &cancel) && cancel);
	(void)aius_optical_web_fill_cancel(&f, true);
	aius_optical_web_fill_drop_http_canceller(&f, 50u);
	assert(f.latches == 0u && f.http_defer == 0u);
	assert(aius_optical_web_fill_held(&f, 2049u));
	assert(!aius_optical_web_fill_held(&f, 2050u));
	aius_optical_web_fill_drop_http_canceller(&f, 9000u); /* no-op */
	assert(!aius_optical_web_fill_held(&f, 9000u));
}

/* A lapsed hold stays lapsed however long ago it lapsed, and a later
 * completion or release sets the expiry to now + 2 s: "only later" applies
 * to a live hold alone. Warm-cache hits never call _note_http_complete, so a
 * gap past 2^31 ms between completions is the steady state, not a corner. */
static void test_web_fill_hold_long_lapse(void)
{
	static const uint32_t k_two31 = UINT32_C(0x80000000);
	static const uint32_t k_ten_days = UINT32_C(864000000);
	static const uint32_t k_gaps[] = {
		/* lapse (ms after the 14000 expiry) just under, at and just over
		 * 2^31, the audit's probe and nearly a whole wrap */
		UINT32_C(0x80000000) - 1u, UINT32_C(0x80000000),
		UINT32_C(0x80000000) + 1u, UINT32_C(0x80000000) + 3000u,
		UINT32_C(0xFFFFFFFF) - 5000u,
	};
	AiusOpticalWebFill f;
	uint32_t now;

	/* The audit's probe: complete at 12000, again at 12000 + 2^31 + 1000. */
	aius_optical_web_fill_init(&f);
	aius_optical_web_fill_note_http_complete(&f, 12000u);
	assert(f.hold_until_ms == 14000u);
	now = 12000u + k_two31 + 1000u;
	assert(!aius_optical_web_fill_held(&f, now));
	aius_optical_web_fill_note_http_complete(&f, now);
	assert(f.hold_until_ms == now + AIUS_OPTICAL_WEB_FILL_HOLD_MS);
	assert(aius_optical_web_fill_held(&f, now + 1999u));
	assert(!aius_optical_web_fill_held(&f, now + 2000u));
	assert(!aius_optical_web_fill_held(&f, now + k_ten_days));

	for (size_t i = 0; i < sizeof(k_gaps) / sizeof(k_gaps[0]); i++) {
		/* Never observed in between: held() alone sees it lapsed. */
		aius_optical_web_fill_init(&f);
		aius_optical_web_fill_note_http_complete(&f, 12000u);
		now = 14000u + k_gaps[i];
		assert(!aius_optical_web_fill_held(&f, now));
		assert(!aius_optical_web_fill_held(&f, now + k_ten_days));
		/* A completion re-arms from now. */
		aius_optical_web_fill_note_http_complete(&f, now);
		assert(f.hold_until_ms == now + AIUS_OPTICAL_WEB_FILL_HOLD_MS);
		assert(aius_optical_web_fill_held(&f, now + 1u));
		assert(!aius_optical_web_fill_held(&f, now + 2000u));
		/* So does a canceller's release. */
		aius_optical_web_fill_init(&f);
		aius_optical_web_fill_note_http_complete(&f, 12000u);
		(void)aius_optical_web_fill_cancel(&f, true);
		assert(aius_optical_web_fill_held(&f, now)); /* latched */
		aius_optical_web_fill_hold_release(&f, now);
		assert(f.latches == 0u);
		assert(f.hold_until_ms == now + AIUS_OPTICAL_WEB_FILL_HOLD_MS);
		assert(aius_optical_web_fill_held(&f, now + 1999u));
		assert(!aius_optical_web_fill_held(&f, now + 2000u));
	}

	/* A live hold still only moves later, a stale stamp included. */
	aius_optical_web_fill_init(&f);
	aius_optical_web_fill_note_http_complete(&f, 5000u);
	aius_optical_web_fill_note_http_complete(&f, 4500u);
	assert(f.hold_until_ms == 7000u);
}

/* Phase 4g, audit finding 3: the routes that read the card directly, and the
 * DISPATCH-arm wait they take while a fill file is in flight: one cancel, a
 * DEFER until the fill stops, the latch released at admission (hold 2 s from
 * then), and the route's end extending the hold like a job completion. */
static void test_web_fill_route(void)
{
	static const struct {
		AiusHttpRoute route;
		AiusHttpMethod method;
		const char *resource;
		bool reads_card;
	} k_routes[] = {
		{ AIUS_HTTP_ROUTE_SD_FILE, AIUS_HTTP_GET, "www/presets/", true },
		{ AIUS_HTTP_ROUTE_SD_FILE, AIUS_HTTP_GET, "www/", true },
		{ AIUS_HTTP_ROUTE_SD_FILE, AIUS_HTTP_GET, "www/index.html", false },
		{ AIUS_HTTP_ROUTE_SD_FILE, AIUS_HTTP_PUT, "www/presets/", false },
		{ AIUS_HTTP_ROUTE_SD_FILE, AIUS_HTTP_DELETE, "www/x.js", false },
		{ AIUS_HTTP_ROUTE_MEASURE_START, AIUS_HTTP_POST, NULL, false },
		{ AIUS_HTTP_ROUTE_MEASURE, AIUS_HTTP_GET, NULL, false },
		{ AIUS_HTTP_ROUTE_STATUS, AIUS_HTTP_GET, NULL, false },
	};
	AiusOpticalWebFill f;
	AiusHttpRequest request;
	bool cancel = true;

	for (size_t i = 0; i < ARRAY_LENGTH(k_routes); i++) {
		memset(&request, 0, sizeof(request));
		request.route = k_routes[i].route;
		request.method = k_routes[i].method;
		if (k_routes[i].resource != NULL) {
			request.resource = (const uint8_t *)k_routes[i].resource;
			request.resource_length = strlen(k_routes[i].resource);
		}
		assert(aius_optical_app_route_reads_card(&request) ==
		       k_routes[i].reads_card);
	}
	assert(!aius_optical_app_route_reads_card(NULL));

	/* No fill in flight: admitted at once, nothing latched; its end extends
	 * the hold 2 s past the end, and only an admitted route's end does. */
	aius_optical_web_fill_init(&f);
	aius_optical_web_fill_route_end(&f, 100u);
	assert(!aius_optical_web_fill_held(&f, 100u));
	assert(!aius_optical_web_fill_route(&f, false, 200u, &cancel));
	assert(!cancel && f.latches == 0u && f.holds == 0u);
	assert(!aius_optical_web_fill_held(&f, 200u));
	aius_optical_web_fill_route_end(&f, 300u);
	assert(aius_optical_web_fill_held(&f, 2299u));
	assert(!aius_optical_web_fill_held(&f, 2300u));
	aius_optical_web_fill_route_end(&f, 5000u); /* consumed: no-op */
	assert(!aius_optical_web_fill_held(&f, 5000u));

	/* A fill in flight: the first consult cancels, later ones only wait;
	 * the hold is latched with no expiry while the dispatch is held. */
	aius_optical_web_fill_init(&f);
	assert(aius_optical_web_fill_route(&f, true, 1000u, &cancel) && cancel);
	assert(aius_optical_web_fill_cancel(&f, true));
	assert(f.cancels == 1u && f.latches == 1u && f.http_canceller == 1u);
	assert(aius_optical_web_fill_route(&f, true, 1010u, &cancel) && !cancel);
	assert(f.cancels == 1u && f.latches == 1u);
	assert(aius_optical_web_fill_held(&f, 900000u));
	/* The fill stopped: admitted, the latch released at admission. */
	assert(!aius_optical_web_fill_route(&f, false, 1020u, &cancel) && !cancel);
	assert(f.latches == 0u && f.http_canceller == 0u && f.http_defer == 0u);
	assert(aius_optical_web_fill_held(&f, 3019u));
	assert(!aius_optical_web_fill_held(&f, 3020u));
	/* The route ran 500 ms (a long listing): the hold runs 2 s past its end. */
	aius_optical_web_fill_route_end(&f, 1520u);
	assert(aius_optical_web_fill_held(&f, 3519u));
	assert(!aius_optical_web_fill_held(&f, 3520u));
	/* A later held SD start is a fresh canceller, not a stale one. */
	assert(aius_optical_web_fill_sd_start(&f, true, &cancel) && cancel);
}

/* Phase 4g, audit finding 6: the cache follows the card. A card-gone fault
 * (the adapter's trusted mount lost, card still "ready" to the lifecycle,
 * the probe skipped in playback) empties the cache at once and parks the
 * walk; it restarts only once a mount appears, never on the stale mount. */
static void test_web_fill_follow_card(void)
{
	bool walk_card = false;
	uint32_t mounts_seen = 0u;

	/* Boot: no card, nothing to empty; the first mount starts the walk. */
	assert(!aius_optical_web_fill_follow_card(&walk_card, &mounts_seen, false,
	                                          false, 0u));
	assert(!walk_card);
	assert(aius_optical_web_fill_follow_card(&walk_card, &mounts_seen, true,
	                                         true, 1u));
	assert(walk_card && mounts_seen == 1u);
	assert(!aius_optical_web_fill_follow_card(&walk_card, &mounts_seen, true,
	                                          true, 1u));
	/* An HTTP SD job's card-gone fault drops the mount: emptied once. */
	assert(aius_optical_web_fill_follow_card(&walk_card, &mounts_seen, true,
	                                         false, 1u));
	assert(!walk_card);
	for (int i = 0; i < 4; i++)
		assert(!aius_optical_web_fill_follow_card(&walk_card, &mounts_seen,
		                                          true, false, 1u));
	assert(!walk_card); /* the walk stays parked while unmounted */
	/* The next mount (count advanced): the walk restarts on it. */
	assert(aius_optical_web_fill_follow_card(&walk_card, &mounts_seen, true,
	                                         true, 2u));
	assert(walk_card && mounts_seen == 2u);
	/* A remount with no fault seen between (swap, mode change). */
	assert(aius_optical_web_fill_follow_card(&walk_card, &mounts_seen, true,
	                                         true, 3u));
	assert(walk_card && mounts_seen == 3u);
	/* The lifecycle's card-absent transition. */
	assert(aius_optical_web_fill_follow_card(&walk_card, &mounts_seen, false,
	                                         true, 3u));
	assert(!walk_card);
	assert(!aius_optical_web_fill_follow_card(NULL, &mounts_seen, true, true,
	                                          4u));
}

static void test_admission_time_ageing(void)
{
	AiusOpticalAdmission a;
	AiusOpticalAdmitSample s;
	const uint32_t level = admit_level(300u);
	uint32_t seed;
	unsigned admitted_at = 0u;

	aius_optical_admission_init(&a);
	seed = aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SETTINGS);
	/* First measurement at t=0 opens a bucket; a 5 ms parse lands at 100. */
	aius_optical_admission_note_dispatch(&a, AIUS_HTTP_ROUTE_DSP_CONFIG,
	                                     AIUS_HTTP_PUT, 300u, 0u);
	aius_optical_admission_note_dispatch(&a, AIUS_HTTP_ROUTE_DSP_CONFIG,
	                                     AIUS_HTTP_PUT, 5000u, 100u);
	/* One admitted 300 us parse in the second bucket, [2000, 4000). */
	aius_optical_admission_note_dispatch(&a, AIUS_HTTP_ROUTE_DSP_CONFIG,
	                                     AIUS_HTTP_PUT, 300u, 2500u);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SETTINGS) ==
	       5000u);
	/* Every later PUT is shed (no sample) until the outlier ages out. */
	for (uint32_t t = 3000u; t <= 10000u; t += 100u) {
		AiusOpticalAdmitVerdict v;
		s = admit_sample(true, level, false, t);
		v = aius_optical_admission_route(&a, AIUS_HTTP_ROUTE_DSP_CONFIG,
		                                 AIUS_HTTP_PUT, &s);
		if (v == AIUS_OPTICAL_ADMIT_VERDICT_ADMIT && admitted_at == 0u)
			admitted_at = t;
		if (t < 10000u)
			assert(v == AIUS_OPTICAL_ADMIT_VERDICT_SHED);
	}
	assert(admitted_at == 10000u);
	/* The later bucket's measurement is the cost, not the seed or 0. */
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SETTINGS) ==
	       300u);
	assert(a.shed[AIUS_OPTICAL_ADMIT_SETTINGS] == (10000u - 3000u) / 100u);
	/* Once every bucket has aged out empty the window returns to its seed
	 * (never judged against 0, R4-S1): the 300 us bucket closed at 4000 and
	 * is overwritten at the fifth close after it, 12000. */
	aius_optical_admission_age(&a, 11999u);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SETTINGS) ==
	       300u);
	aius_optical_admission_age(&a, 12000u);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_SETTINGS) ==
	       seed);
	assert(a.window[AIUS_OPTICAL_ADMIT_SETTINGS].seeded == 1u);

	/* A saturated 65,535 us stretch (more than the 2,048-frame ring can
	 * cover) ages out the same way, with the class shed throughout. */
	aius_optical_admission_init(&a);
	aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_EVENTS, 200000u, 500u);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_EVENTS) ==
	       65535u);
	for (uint32_t t = 600u; t < 10500u; t += 250u) {
		s = admit_sample(true, 2048u, false, t);
		assert(aius_optical_admission_route(&a, AIUS_HTTP_ROUTE_EVENTS,
		                                    AIUS_HTTP_GET, &s) ==
		       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
	}
	s = admit_sample(true, 2048u, false, 10500u);
	assert(aius_optical_admission_route(&a, AIUS_HTTP_ROUTE_EVENTS,
	                                    AIUS_HTTP_GET, &s) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);
	assert(aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_EVENTS) ==
	       600u); /* the EVENTS seed */

	/* The SD-start windows age the same way through their consults. */
	aius_optical_admission_init(&a);
	aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_SD_START_DMA, 60000u, 0u);
	s = admit_sample(true, 2048u, false, 9999u);
	assert(aius_optical_admission_sd_start(&a, NULL, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_SHED);
	s = admit_sample(true, 2048u, false, 10000u);
	assert(aius_optical_admission_sd_start(&a, NULL, &s, false) ==
	       AIUS_OPTICAL_ADMIT_VERDICT_ADMIT);
}

/* F-S5: an outlier that is the class's first measurement (it replaces the
 * seed in all five slots) is an outlier at a bucket's first position: it
 * stays the cost for 79 further samples and is gone at the 80th. */
static void test_admission_first_measurement_outlier(void)
{
	AiusOpticalAdmission a;
	aius_optical_admission_init(&a);
	aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_BRIEF, 9000u, 0u);
	for (unsigned k = 1; k <= 90u; ++k) {
		uint32_t cost;
		aius_optical_admission_record(&a, AIUS_OPTICAL_ADMIT_BRIEF, 100u, 0u);
		cost = aius_optical_admission_cost_us(&a, AIUS_OPTICAL_ADMIT_BRIEF);
		assert(cost == (k < 80u ? 9000u : 100u));
	}
}

/* ------------------------------------------------------------------ */
/* Phase 4g: cache-first /sd/www reads against a fake web cache seam    */
/* ------------------------------------------------------------------ */

/* The dispatcher's direct yield point (lib/aius_producer_yield on target):
 * the host suite counts the body source's calls. */
static unsigned g_producer_yields;

bool aius_producer_yield_point(void)
{
	g_producer_yields++;
	return false;
}

#define FAKE_WEB_ENTRIES 4u

typedef struct {
	const char *path;
	const uint8_t *data;
	uint32_t length;
	const char *etag;
	bool verify_fails;
	bool entry_fault; /* its manifest entry fails its own CRC at lookup */
	long copy_fail_chunk; /* -1: never */
	int leased;
} FakeWebEntry;

typedef struct {
	FakeWebEntry entries[FAKE_WEB_ENTRIES];
	size_t count;
	unsigned lookups;
	unsigned leases;
	unsigned releases;
	unsigned verifies;
	unsigned copies;
	unsigned refills;
	unsigned listings;
	bool listing_complete;
	char last_lookup[64];
} FakeWeb;

static uint8_t g_web_bytes[5000];

static bool fake_web_lookup(void *context, const char *path, size_t length,
                            AiusOpticalAppWebHit *hit, char *etag,
                            size_t etag_capacity, bool *fault)
{
	FakeWeb *web = (FakeWeb *)context;
	web->lookups++;
	assert(fault != NULL);
	*fault = false;
	assert(length < sizeof(web->last_lookup));
	memcpy(web->last_lookup, path, length);
	web->last_lookup[length] = '\0';
	for (size_t i = 0; i < web->count; ++i) {
		FakeWebEntry *entry = &web->entries[i];
		if (strlen(entry->path) != length ||
		    memcmp(entry->path, path, length) != 0)
			continue;
		if (entry->entry_fault) {
			/* Dropped by the cache: a fault once, then a plain miss. */
			entry->path = "";
			*fault = true;
			return false;
		}
		assert(strlen(entry->etag) < etag_capacity);
		snprintf(etag, etag_capacity, "%s", entry->etag);
		hit->entry = (uint8_t)i;
		hit->length = entry->length;
		return true;
	}
	return false;
}

static bool fake_web_lease(void *context, uint8_t entry)
{
	FakeWeb *web = (FakeWeb *)context;
	assert(entry < web->count);
	web->leases++;
	web->entries[entry].leased++;
	return true;
}

static void fake_web_release(void *context, uint8_t entry)
{
	FakeWeb *web = (FakeWeb *)context;
	assert(entry < web->count);
	assert(web->entries[entry].leased > 0);
	web->releases++;
	web->entries[entry].leased--;
}

static bool fake_web_verify(void *context, uint8_t entry)
{
	FakeWeb *web = (FakeWeb *)context;
	assert(web->entries[entry].leased > 0); /* leased before the check */
	web->verifies++;
	return !web->entries[entry].verify_fails;
}

static bool fake_web_copy_chunk(void *context, uint8_t entry, uint16_t chunk,
                                uint8_t *dst, uint32_t *bytes)
{
	FakeWeb *web = (FakeWeb *)context;
	const FakeWebEntry *e = &web->entries[entry];
	uint32_t offset = (uint32_t)chunk * 512u;
	assert(e->leased > 0);
	web->copies++;
	if ((long)chunk == e->copy_fail_chunk || offset >= e->length)
		return false;
	*bytes = e->length - offset < 512u ? e->length - offset : 512u;
	memcpy(dst, e->data + offset, *bytes);
	return true;
}

static void fake_web_refill(void *context)
{
	((FakeWeb *)context)->refills++;
}

static bool fake_web_listing(void *context, const char *dir, size_t dir_length,
                             AiusOpticalAppDirBatch *batch)
{
	FakeWeb *web = (FakeWeb *)context;
	web->listings++;
	if (!web->listing_complete || dir_length != 12u ||
	    memcmp(dir, "www/presets/", 12u) != 0)
		return false;
	snprintf(batch->entries[0].name, sizeof(batch->entries[0].name), "a.json");
	batch->entries[0].size_bytes = 10u;
	snprintf(batch->entries[1].name, sizeof(batch->entries[1].name), "b.json");
	batch->entries[1].size_bytes = 20u;
	batch->count = 2u;
	return true;
}

static AiusOpticalAppWebCache fake_web_seam(FakeWeb *web)
{
	AiusOpticalAppWebCache seam;
	memset(&seam, 0, sizeof(seam));
	seam.context = web;
	seam.lookup = fake_web_lookup;
	seam.lease = fake_web_lease;
	seam.release = fake_web_release;
	seam.verify = fake_web_verify;
	seam.copy_chunk = fake_web_copy_chunk;
	seam.refill = fake_web_refill;
	seam.listing = fake_web_listing;
	return seam;
}

static void fake_web_add(FakeWeb *web, const char *path, uint32_t length,
                         const char *etag)
{
	FakeWebEntry *entry;
	assert(web->count < FAKE_WEB_ENTRIES);
	assert(length <= sizeof(g_web_bytes));
	entry = &web->entries[web->count++];
	memset(entry, 0, sizeof(*entry));
	entry->path = path;
	entry->data = g_web_bytes;
	entry->length = length;
	entry->etag = etag;
	entry->copy_fail_chunk = -1;
}

/* Streams a chunk-sourced body the way the transport does, one chunk per
 * call into a 512 B buffer; false at the first refused chunk. */
static bool drain_body(const AiusHttpApplicationResponse *response,
                       uint32_t *chunks_out)
{
	uint8_t chunk[512];
	uint32_t total = 0u;
	uint32_t n = 0u;
	assert(response->body_chunk != NULL);
	while (total < response->body_length) {
		uint32_t bytes = 0u;
		if (!response->body_chunk(response->body_chunk_ctx, n, chunk, &bytes)) {
			*chunks_out = n;
			return false;
		}
		assert(bytes == (response->body_length - total < 512u
		                         ? response->body_length - total : 512u));
		assert(memcmp(chunk, g_web_bytes + total, bytes) == 0);
		total += bytes;
		n++;
	}
	*chunks_out = n;
	return true;
}

static void expect_cached_200(const AiusHttpApplicationResponse *response,
                              const char *type, uint32_t length,
                              const char *etag, bool gzip, uint32_t max_age)
{
	assert(response != NULL);
	assert(response->kind == AIUS_HTTP_APP_IMMEDIATE);
	assert(response->status == 200u);
	assert(strcmp(response->content_type, type) == 0);
	/* body and body_chunk_ctx share one slot: a chunk-sourced response
	 * carries the source context there, never a borrowed body. */
	assert(response->body_chunk_ctx != NULL);
	assert(response->body_length == length);
	assert(response->body_chunk != NULL);
	assert(response->sd_settle != NULL);
	assert(response->etag != NULL);
	assert(strcmp(response->etag, etag) == 0);
	assert(response->content_encoding_gzip == gzip);
	assert(response->vary_accept_encoding);
	assert(response->cache_max_age_s == max_age);
	assert(response->cache_no_cache == (max_age == 0u));
	assert(response->sd_job.kind == AIUS_SD_JOB_PROBE);
	assert(response->sd_job.target_path == NULL);
}

static void test_web_cache_seam_install(void)
{
	Fixture fixture;
	FakeWeb web;
	AiusOpticalAppWebCache seam;
	AiusOpticalAppDispatcher invalid;

	memset(&web, 0, sizeof(web));
	seam = fake_web_seam(&web);
	fixture_init(&fixture);
	memset(&invalid, 0, sizeof(invalid));
	assert(!aius_optical_app_set_web_cache(NULL, &seam));
	assert(!aius_optical_app_set_web_cache(&invalid, &seam));
	seam.listing = NULL;
	assert(!aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));
	/* A half-wired table leaves the cache absent: the card serves. */
	fake_web_add(&web, "www/app.js", 100u, "\"00000001-100\"");
	expect_response(dispatch_get(&fixture, "GET /sd/www/app.js"),
	                AIUS_HTTP_APP_SD_JOB, 200, "application/javascript", NULL);
	assert(web.lookups == 0u);
	seam = fake_web_seam(&web);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));
	assert(dispatch_get(&fixture, "GET /sd/www/app.js")->kind ==
	       AIUS_HTTP_APP_IMMEDIATE);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, NULL));
	/* Removal drops the outstanding lease. */
	assert(web.releases == 1u && web.entries[0].leased == 0);
	assert(dispatch_get(&fixture, "GET /sd/www/app.js")->kind ==
	       AIUS_HTTP_APP_SD_JOB);
	/* Re-init clears the seam. */
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));
	fixture_init(&fixture);
	assert(dispatch_get(&fixture, "GET /sd/www/app.js")->kind ==
	       AIUS_HTTP_APP_SD_JOB);
}

/* Hit, miss, 304, the body source's chunks, yields, lease and settle. */
static void test_web_cache_hit_miss_304(void)
{
	Fixture fixture;
	FakeWeb web;
	AiusOpticalAppWebCache seam;
	const AiusHttpApplicationResponse *response;
	uint32_t chunks = 0u;

	for (size_t i = 0; i < sizeof(g_web_bytes); ++i)
		g_web_bytes[i] = (uint8_t)(i * 7u + 3u);
	memset(&web, 0, sizeof(web));
	seam = fake_web_seam(&web);
	fixture_init(&fixture);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));

	/* Miss: the card's download job, with the G2 policy and no ETag. */
	response = dispatch_get(&fixture, "GET /sd/www/app.js");
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200,
	                "application/javascript", NULL);
	assert(response->sd_job.kind == AIUS_SD_JOB_DOWNLOAD);
	assert(strcmp(web.last_lookup, "www/app.js") == 0);
	assert(response->etag == NULL && response->vary_accept_encoding);
	assert(response->cache_max_age_s == 86400u);
	assert(web.leases == 0u);
	/* A miss on a busy card is still 503 sd_busy. */
	fixture.fake.sd_busy = true;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/app.js"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");

	/* Hit, with the volume busy: no card, no idle volume needed. */
	fake_web_add(&web, "www/app.js", 9u * 512u + 100u, "\"0badf00d-4708\"");
	response = dispatch_get(&fixture, "GET /sd/www/app.js");
	expect_cached_200(response, "application/javascript", 9u * 512u + 100u,
	                  "\"0badf00d-4708\"", false, 86400u);
	assert(web.leases == 1u && web.verifies == 1u);
	assert(fixture.dispatcher.web_lease_active);
	/* Ten chunks, the last short; a yield after every eighth. */
	g_producer_yields = 0u;
	assert(drain_body(response, &chunks));
	assert(chunks == 10u);
	assert(g_producer_yields == 1u);
	/* A partial write re-calls the same chunk: repeatable. */
	{
		uint8_t again[512];
		uint32_t bytes = 0u;
		assert(response->body_chunk(response->body_chunk_ctx, 9u, again,
		                            &bytes));
		assert(bytes == 100u &&
		       memcmp(again, g_web_bytes + 9u * 512u, 100u) == 0);
	}
	assert(web.refills == 0u);
	/* The settle hook releases exactly once. */
	response->sd_settle(response->sd_settle_ctx, false);
	response->sd_settle(response->sd_settle_ctx, false);
	assert(web.releases == 1u && web.entries[0].leased == 0);
	assert(!fixture.dispatcher.web_lease_active);
	fixture.fake.sd_busy = false;

	/* 304 on a matching If-None-Match: no lease, no verify, no body. */
	response = dispatch_request(&fixture, "GET /sd/www/app.js",
	                            "If-None-Match: \"0badf00d-4708\"\r\n", NULL,
	                            -1);
	assert(response->kind == AIUS_HTTP_APP_IMMEDIATE);
	assert(response->status == 304u);
	assert(response->body == NULL && response->body_length == 0u);
	assert(response->body_chunk == NULL && response->sd_settle == NULL);
	assert(strcmp(response->etag, "\"0badf00d-4708\"") == 0);
	assert(response->vary_accept_encoding);
	assert(response->cache_max_age_s == 86400u);
	assert(web.leases == 1u && web.verifies == 1u);
	/* A stale tag gets the 200. */
	response = dispatch_request(&fixture, "GET /sd/www/app.js",
	                            "If-None-Match: \"00000000-1\"\r\n", NULL, -1);
	assert(response->status == 200u && response->body_chunk != NULL);
	assert(web.leases == 2u);
	/* A lease the previous response never settled is dropped, not
	 * overwritten, at the next cache serve. */
	response = dispatch_get(&fixture, "GET /sd/www/app.js");
	assert(response->status == 200u);
	assert(web.releases == 2u && web.leases == 3u);
	assert(web.entries[0].leased == 1);
	response->sd_settle(response->sd_settle_ctx, false);
	assert(web.entries[0].leased == 0);

	/* Writes and the out-of-www carve-out never consult the cache. */
	web.lookups = 0u;
	(void)dispatch_request(&fixture, "DELETE /sd/www/app.js",
	                       "X-Aius-SD-Auth: token\r\n", NULL, -1);
	(void)dispatch_get(&fixture, "GET /sd/aius/dsp.json");
	(void)dispatch_get(&fixture, "GET /sd/replays/take1");
	assert(web.lookups == 0u);

	/* An empty cached file is a 200 with no body source and no lease. */
	fake_web_add(&web, "www/empty.css", 0u, "\"00000000-0\"");
	response = dispatch_get(&fixture, "GET /sd/www/empty.css");
	assert(response->kind == AIUS_HTTP_APP_IMMEDIATE && response->status == 200u);
	assert(response->body_length == 0u && response->body_chunk == NULL);
	assert(strcmp(response->etag, "\"00000000-0\"") == 0);
	assert(web.leases == 3u);
}

/* G1: a gzip-accepting client gets <page>.html.gz with Content-Encoding;
 * any other gets the plain page; a missing bundle falls back to the plain
 * entry. Pages are no-cache. */
static void test_web_cache_gzip_negotiation(void)
{
	Fixture fixture;
	FakeWeb web;
	AiusOpticalAppWebCache seam;
	const AiusHttpApplicationResponse *response;
	uint32_t chunks = 0u;

	memset(&web, 0, sizeof(web));
	seam = fake_web_seam(&web);
	fixture_init(&fixture);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));
	fake_web_add(&web, "www/index.html", 700u, "\"11111111-700\"");
	fake_web_add(&web, "www/index.html.gz", 300u, "\"22222222-300\"");

	response = dispatch_request(&fixture, "GET /sd/www/index.html",
	                            "Accept-Encoding: gzip, deflate\r\n", NULL, -1);
	expect_cached_200(response, "text/html; charset=utf-8", 300u,
	                  "\"22222222-300\"", true, 0u);
	assert(strcmp(web.last_lookup, "www/index.html.gz") == 0);
	assert(drain_body(response, &chunks) && chunks == 1u);
	response->sd_settle(response->sd_settle_ctx, false);

	/* The bundle's own tag revalidates the bundle. */
	response = dispatch_request(&fixture, "GET /sd/www/index.html",
	                            "Accept-Encoding: gzip\r\n"
	                            "If-None-Match: \"22222222-300\"\r\n", NULL, -1);
	assert(response->status == 304u && response->content_encoding_gzip);

	/* No Accept-Encoding, or gzip refused with q=0: the plain page. */
	response = dispatch_get(&fixture, "GET /sd/www/index.html");
	expect_cached_200(response, "text/html; charset=utf-8", 700u,
	                  "\"11111111-700\"", false, 0u);
	assert(strcmp(web.last_lookup, "www/index.html") == 0);
	response->sd_settle(response->sd_settle_ctx, false);
	response = dispatch_request(&fixture, "GET /sd/www/index.html",
	                            "Accept-Encoding: gzip;q=0\r\n", NULL, -1);
	expect_cached_200(response, "text/html; charset=utf-8", 700u,
	                  "\"11111111-700\"", false, 0u);
	response->sd_settle(response->sd_settle_ctx, false);

	/* Only a page is negotiated: a script is looked up as itself. */
	web.lookups = 0u;
	(void)dispatch_request(&fixture, "GET /sd/www/app.js",
	                       "Accept-Encoding: gzip\r\n", NULL, -1);
	assert(web.lookups == 1u && strcmp(web.last_lookup, "www/app.js") == 0);

	/* Bundle not cached (yet): the plain entry, unencoded. */
	web.entries[1].path = "absent.gz";
	response = dispatch_request(&fixture, "GET /sd/www/index.html",
	                            "Accept-Encoding: gzip\r\n", NULL, -1);
	expect_cached_200(response, "text/html; charset=utf-8", 700u,
	                  "\"11111111-700\"", false, 0u);
	response->sd_settle(response->sd_settle_ctx, false);
	assert(web.entries[0].leased == 0 && web.entries[1].leased == 0);
}

/* G3, G7: a pre-header CRC fault serves the card's download job and schedules
 * a refill; a mid-stream fault closes the connection (false) and schedules a
 * refill; the lease is released either way. */
static void test_web_cache_crc_faults(void)
{
	Fixture fixture;
	FakeWeb web;
	AiusOpticalAppWebCache seam;
	const AiusHttpApplicationResponse *response;
	uint32_t chunks = 0u;

	memset(&web, 0, sizeof(web));
	seam = fake_web_seam(&web);
	fixture_init(&fixture);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));
	fake_web_add(&web, "www/status.html", 2000u, "\"33333333-2000\"");

	web.entries[0].verify_fails = true;
	response = dispatch_get(&fixture, "GET /sd/www/status.html");
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200,
	                "text/html; charset=utf-8", NULL);
	assert(response->sd_job.kind == AIUS_SD_JOB_DOWNLOAD);
	assert(strcmp(response->sd_job.target_path, "/aius/www/status.html") == 0);
	assert(response->etag == NULL);
	assert(response->cache_no_cache && response->vary_accept_encoding);
	assert(web.verifies == 1u && web.refills == 1u);
	assert(web.leases == 1u && web.releases == 1u);
	assert(!fixture.dispatcher.web_lease_active);
	/* ... and a busy card answers 503 sd_busy rather than a faulted entry. */
	fixture.fake.sd_busy = true;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/status.html"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	fixture.fake.sd_busy = false;
	assert(web.refills == 2u && web.entries[0].leased == 0);

	web.entries[0].verify_fails = false;
	web.entries[0].copy_fail_chunk = 2;
	response = dispatch_get(&fixture, "GET /sd/www/status.html");
	expect_cached_200(response, "text/html; charset=utf-8", 2000u,
	                  "\"33333333-2000\"", false, 0u);
	assert(!drain_body(response, &chunks));
	assert(chunks == 2u);
	assert(web.refills == 3u);
	response->sd_settle(response->sd_settle_ctx, false);
	assert(web.entries[0].leased == 0 && web.releases == 3u);
	/* After the settle, the source refuses: no lease, no bytes. */
	{
		uint8_t buffer[512];
		uint32_t bytes = 0u;
		assert(!response->body_chunk(response->body_chunk_ctx, 0u, buffer,
		                             &bytes));
	}
}

/* Finding 5: a manifest entry that fails its own CRC at lookup is not a
 * plain miss. The card serves and a refill is scheduled once; the dropped
 * entry is a plain miss after that. A faulted .gz refills and falls back to
 * the plain page. */
static void test_web_cache_entry_fault_refills(void)
{
	Fixture fixture;
	FakeWeb web;
	AiusOpticalAppWebCache seam;
	const AiusHttpApplicationResponse *response;

	memset(&web, 0, sizeof(web));
	seam = fake_web_seam(&web);
	fixture_init(&fixture);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));
	fake_web_add(&web, "www/app.css", 900u, "\"44444444-900\"");
	web.entries[0].entry_fault = true;

	response = dispatch_get(&fixture, "GET /sd/www/app.css");
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "text/css", NULL);
	assert(response->sd_job.kind == AIUS_SD_JOB_DOWNLOAD);
	assert(web.refills == 1u && web.leases == 0u);
	/* Dropped: the next request is a plain miss, no second refill. */
	response = dispatch_get(&fixture, "GET /sd/www/app.css");
	assert(response->kind == AIUS_HTTP_APP_SD_JOB);
	assert(web.refills == 1u);

	/* The bundle's entry faults: refill, then the plain page serves. */
	fake_web_add(&web, "www/index.html", 700u, "\"11111111-700\"");
	fake_web_add(&web, "www/index.html.gz", 300u, "\"22222222-300\"");
	web.entries[2].entry_fault = true;
	response = dispatch_request(&fixture, "GET /sd/www/index.html",
	                            "Accept-Encoding: gzip\r\n", NULL, -1);
	expect_cached_200(response, "text/html; charset=utf-8", 700u,
	                  "\"11111111-700\"", false, 0u);
	assert(web.refills == 2u);
	response->sd_settle(response->sd_settle_ctx, false);
}

/* The presets listing from the manifest once complete (no card read, even
 * on a busy volume); from the card otherwise. */
static void test_web_cache_presets_listing(void)
{
	Fixture fixture;
	FakeWeb web;
	AiusOpticalAppWebCache seam;

	memset(&web, 0, sizeof(web));
	seam = fake_web_seam(&web);
	fixture_init(&fixture);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &seam));
	fake_dir_add(&fixture.fake, "card.json", 5u, false);

	expect_index(dispatch_get(&fixture, "GET /sd/www/presets/"),
	             INDEX_PROLOGUE("www/presets/") INDEX_PARENT
	             "<li><a href=\"card.json\">card.json</a> 5</li>"
	             INDEX_FOOTER("1", "0", "no"));
	assert(web.listings == 1u);
	assert(strcmp(fixture.fake.dir_path, "/aius/www/presets") == 0);

	web.listing_complete = true;
	fixture.fake.sd_busy = true;
	fixture.fake.dir_path[0] = '\0';
	expect_index(dispatch_get(&fixture, "GET /sd/www/presets/"),
	             INDEX_PROLOGUE("www/presets/") INDEX_PARENT
	             "<li><a href=\"a.json\">a.json</a> 10</li>"
	             "<li><a href=\"b.json\">b.json</a> 20</li>"
	             INDEX_FOOTER("2", "0", "no"));
	assert(fixture.fake.dir_path[0] == '\0');
	/* Any other directory still reads the card (and honours sd_busy). */
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	assert(web.listings == 3u);
}

/* Phase 4h: a fake write-behind overlay seam. It records what the dispatcher
 * asked for; the policy itself is lib/aius_sd_overlay's (sd-overlay suite). */
typedef struct {
	bool present;
	bool drained;
	AiusOpticalAppStoreResult put_result;
	AiusOpticalAppStoreResult tombstone_result;
	unsigned put_begins;
	unsigned put_aborts;
	unsigned tombstones;
	unsigned merges;
	uint32_t last_length;
	char last_rel[64];
	char last_dir[64];
} FakeOverlay;

static void fake_overlay_note(char *out, const char *rel, size_t length)
{
	assert(length < 64u);
	memcpy(out, rel, length);
	out[length] = '\0';
}

static bool fake_overlay_present(void *context)
{
	return ((FakeOverlay *)context)->present;
}

static AiusOpticalAppStoreResult fake_overlay_put_begin(void *context,
                                                        const char *rel,
                                                        size_t rel_length,
                                                        uint32_t length)
{
	FakeOverlay *overlay = (FakeOverlay *)context;
	overlay->put_begins++;
	overlay->last_length = length;
	fake_overlay_note(overlay->last_rel, rel, rel_length);
	return overlay->put_result;
}

static void fake_overlay_put_abort(void *context)
{
	((FakeOverlay *)context)->put_aborts++;
}

static AiusOpticalAppStoreResult fake_overlay_tombstone(void *context,
                                                        const char *rel,
                                                        size_t rel_length)
{
	FakeOverlay *overlay = (FakeOverlay *)context;
	overlay->tombstones++;
	fake_overlay_note(overlay->last_rel, rel, rel_length);
	return overlay->tombstone_result;
}

static bool fake_overlay_drained(void *context)
{
	return ((FakeOverlay *)context)->drained;
}

/* S6: hides card.json (tombstoned) and appends pending.json. */
static void fake_overlay_merge(void *context, const char *dir,
                               size_t dir_length, AiusOpticalAppDirBatch *batch)
{
	FakeOverlay *overlay = (FakeOverlay *)context;
	size_t kept = 0u;
	overlay->merges++;
	fake_overlay_note(overlay->last_dir, dir, dir_length);
	for (size_t i = 0; i < batch->count; ++i) {
		if (strcmp(batch->entries[i].name, "card.json") == 0)
			continue;
		batch->entries[kept++] = batch->entries[i];
	}
	batch->count = kept;
	snprintf(batch->entries[batch->count].name,
	         sizeof(batch->entries[0].name), "pending.json");
	batch->entries[batch->count].size_bytes = 7u;
	batch->entries[batch->count].is_directory = false;
	batch->count++;
}

static AiusOpticalAppSdOverlay fake_overlay_seam(FakeOverlay *overlay)
{
	AiusOpticalAppSdOverlay seam;
	memset(&seam, 0, sizeof(seam));
	seam.context = overlay;
	seam.present = fake_overlay_present;
	seam.put_begin = fake_overlay_put_begin;
	seam.put_abort = fake_overlay_put_abort;
	seam.tombstone = fake_overlay_tombstone;
	seam.drained = fake_overlay_drained;
	seam.merge_listing = fake_overlay_merge;
	return seam;
}

static void fake_overlay_init(FakeOverlay *overlay)
{
	memset(overlay, 0, sizeof(*overlay));
	overlay->present = true;
	overlay->drained = true;
	overlay->put_result = AIUS_OPTICAL_APP_STORE_OK;
	overlay->tombstone_result = AIUS_OPTICAL_APP_STORE_OK;
}

/* Install validation, and an 8 MiB fitment (present false) keeps every route
 * as today (Q4). */
static void test_sd_overlay_seam_install(void)
{
	Fixture fixture;
	FakeOverlay overlay;
	AiusOpticalAppSdOverlay seam;
	const AiusHttpApplicationResponse *response;

	fake_overlay_init(&overlay);
	seam = fake_overlay_seam(&overlay);
	fixture_init(&fixture);
	assert(!aius_optical_app_set_sd_overlay(NULL, &seam));
	seam.merge_listing = NULL;
	assert(!aius_optical_app_set_sd_overlay(&fixture.dispatcher, &seam));
	seam = fake_overlay_seam(&overlay);
	overlay.present = false;
	assert(aius_optical_app_set_sd_overlay(&fixture.dispatcher, &seam));
	response = dispatch_request(&fixture, "PUT /sd/www/index.html",
	                            "X-Aius-SD-Auth: token\r\n", NULL, 4096);
	assert(response->kind == AIUS_HTTP_APP_SD_JOB);
	assert(response->sd_job.install_mode == AIUS_SD_INSTALL_REPLACE);
	fixture.fake.sd_ready = false;
	expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                  "X-Aius-SD-Auth: token\r\n", "{}", -1),
	                 503, "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	overlay.drained = false;
	expect_immediate(dispatch_request(&fixture, "POST /api/reboot",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true,\"rebooting\":true}");
	assert(overlay.put_begins == 0u && overlay.tombstones == 0u);
	/* Re-init clears the seam. */
	overlay.present = true;
	fixture_init(&fixture);
	response = dispatch_request(&fixture, "PUT /sd/www/index.html",
	                            "X-Aius-SD-Auth: token\r\n", NULL, 4096);
	assert(response->sd_job.install_mode == AIUS_SD_INSTALL_REPLACE);
}

/* B2, Q1, Q4, S2, S8, N3: with the carve, writes are accepted with no card. */
static void test_sd_overlay_writes(void)
{
	Fixture fixture;
	FakeOverlay overlay;
	AiusOpticalAppSdOverlay seam;
	const AiusHttpApplicationResponse *response;

	fake_overlay_init(&overlay);
	seam = fake_overlay_seam(&overlay);
	fixture_init(&fixture);
	assert(aius_optical_app_set_sd_overlay(&fixture.dispatcher, &seam));
	fixture.fake.sd_ready = false;
	fixture.fake.sd_busy = true;

	/* Settings: 200 at accept, card or no card. */
	fixture.fake.dsp_store = AIUS_OPTICAL_APP_STORE_OK;
	expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                  "X-Aius-SD-Auth: token\r\n", "{}", -1),
	                 200, "{\"ok\":true}");
	fixture.fake.public_store = AIUS_OPTICAL_APP_STORE_OK;
	expect_immediate(dispatch_request(&fixture, "PUT /api/config/public", NULL,
	                                  "{}", -1),
	                 200, "{\"ok\":true}");
	/* Store full: 503 sd_busy (Retry-After is the transport's). */
	fixture.fake.dsp_store = AIUS_OPTICAL_APP_STORE_BUSY;
	expect_immediate(dispatch_request(&fixture, "PUT /api/dsp/config",
	                                  "X-Aius-SD-Auth: token\r\n", "{}", -1),
	                 503, "{\"ok\":false,\"error\":\"sd_busy\"}");

	/* A www PUT reserves the put and streams it as a STORE request. */
	response = dispatch_request(&fixture, "PUT /sd/www/presets/x.json",
	                            "X-Aius-SD-Auth: token\r\n", NULL, 4096);
	expect_response(response, AIUS_HTTP_APP_SD_JOB, 200, "application/json",
	                "{\"ok\":true}");
	assert(response->sd_job.kind == AIUS_SD_JOB_UPLOAD);
	assert(response->sd_job.install_mode == AIUS_SD_INSTALL_STORE);
	assert(response->sd_job.total == 4096u);
	assert(response->sd_job.initial_offset == 0u);
	assert(!response->sd_job.publish);
	assert(response->sd_job.temp_path == NULL);
	assert(response->sd_job.backup_path == NULL);
	assert(strcmp(response->sd_job.target_path,
	              "/aius/www/presets/x.json") == 0);
	assert(overlay.put_begins == 1u && overlay.last_length == 4096u);
	assert(strcmp(overlay.last_rel, "www/presets/x.json") == 0);
	overlay.put_result = AIUS_OPTICAL_APP_STORE_BUSY;
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/index.html",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 503, "{\"ok\":false,\"error\":\"sd_busy\"}");
	overlay.put_result = AIUS_OPTICAL_APP_STORE_INVALID;
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/index.html",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 507, "{\"ok\":false,\"error\":\"sd_upload\"}");
	/* Phase 4l (B3): the preset cap is 409 preset_limit, never 503; BUSY
	 * above still answers 503 sd_busy. */
	overlay.put_result = AIUS_OPTICAL_APP_STORE_LIMIT;
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/presets/z.json",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 409, "{\"ok\":false,\"error\":\"preset_limit\"}");
	/* Refusal order before the store: config, then auth. */
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/index.html",
	                                  "X-Aius-SD-Auth: wrong\r\n", NULL, 16),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "PUT /sd/www/config.json",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, 16),
	                 403, "{\"ok\":false,\"error\":\"config_public_only\"}");
	assert(overlay.put_begins == 4u);
	/* Replay is retired: /sd/replays/ is an ordinary SD file path, which the
	 * www-only path rule refuses, authenticated and with a store present. */
	expect_immediate(dispatch_request(&fixture, "PUT /sd/replays/take1",
	                                  "X-Aius-SD-Auth: token\r\n"
	                                  "Content-Range: bytes 0-15/32\r\n",
	                                  NULL, 16),
	                 507, "{\"ok\":false,\"error\":\"sd_upload\"}");
	assert(overlay.put_begins == 4u);

	/* A www DELETE is a tombstone, 200 at accept with no card (S2). */
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/www/presets/x.json",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true}");
	assert(overlay.tombstones == 1u);
	assert(strcmp(overlay.last_rel, "www/presets/x.json") == 0);
	overlay.tombstone_result = AIUS_OPTICAL_APP_STORE_BUSY;
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/www/presets/y.json",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 503, "{\"ok\":false,\"error\":\"sd_busy\"}");
	expect_immediate(dispatch_request(&fixture, "DELETE /sd/www/presets/y.json",
	                                  NULL, NULL, -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	assert(overlay.tombstones == 2u);
}

/* Store-first reads (B2), the presets listing with no card (N7), the
 * card-read merge (S6) and the drain gate's 409 (FR-S8). */
static void test_sd_overlay_reads_and_drain(void)
{
	Fixture fixture;
	FakeOverlay overlay;
	AiusOpticalAppSdOverlay overlay_seam;
	FakeWeb web;
	AiusOpticalAppWebCache web_seam;
	uint32_t chunks = 0u;
	const AiusHttpApplicationResponse *response;

	for (size_t i = 0; i < sizeof(g_web_bytes); ++i)
		g_web_bytes[i] = (uint8_t)(i * 5u + 1u);
	memset(&web, 0, sizeof(web));
	web_seam = fake_web_seam(&web);
	fake_overlay_init(&overlay);
	overlay_seam = fake_overlay_seam(&overlay);
	fixture_init(&fixture);
	assert(aius_optical_app_set_web_cache(&fixture.dispatcher, &web_seam));
	assert(aius_optical_app_set_sd_overlay(&fixture.dispatcher,
	                                       &overlay_seam));
	fixture.fake.sd_ready = false;

	/* GET /sd/aius/dsp.json is served from the store with no card. */
	fake_web_add(&web, "dsp.json", 600u, "\"00000002-600\"");
	response = dispatch_get(&fixture, "GET /sd/aius/dsp.json");
	assert(response->kind == AIUS_HTTP_APP_IMMEDIATE);
	assert(response->status == 200u);
	assert(response->body_length == 600u);
	assert(strcmp(web.last_lookup, "dsp.json") == 0);
	assert(drain_body(response, &chunks) && chunks == 2u);
	/* A pending preset answers its own GET; a miss answers sd_unavailable. */
	fake_web_add(&web, "www/presets/x.json", 10u, "\"00000003-10\"");
	assert(dispatch_get(&fixture, "GET /sd/www/presets/x.json")->status ==
	       200u);
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/presets/z.json"),
	                 503, "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	/* No card, walk never ran: the presets listing is not the card's set. */
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/presets/"), 503,
	                 "{\"ok\":false,\"error\":\"sd_unavailable\"}");
	assert(overlay.merges == 0u);

	/* With the card, a card-read listing merges pending rows and hides
	 * tombstoned names. */
	fixture.fake.sd_ready = true;
	fake_dir_add(&fixture.fake, "card.json", 5u, false);
	fake_dir_add(&fixture.fake, "kept.json", 6u, false);
	expect_index(dispatch_get(&fixture, "GET /sd/www/presets/"),
	             INDEX_PROLOGUE("www/presets/") INDEX_PARENT
	             "<li><a href=\"kept.json\">kept.json</a> 6</li>"
	             "<li><a href=\"pending.json\">pending.json</a> 7</li>"
	             INDEX_FOOTER("2", "0", "no"));
	assert(overlay.merges == 1u);
	assert(strcmp(overlay.last_dir, "www/presets/") == 0);

	/* FR-S8: authentication first, then 409 while anything is unflushed. */
	overlay.drained = false;
	expect_immediate(dispatch_request(&fixture, "POST /api/reboot", NULL, NULL,
	                                  -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	expect_immediate(dispatch_request(&fixture, "POST /api/reboot",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"sd_flush_pending\"}");
	assert(fixture.fake.reboots == 0u);
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"sd_flush_pending\"}");
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"sd_flush_pending\"}");
	/* Phase 4y: boot-other is a reboot, under the same gate, and the
	 * callback is never asked. */
	expect_immediate(dispatch_ota(&fixture,
	                              AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER,
	                              OTA_AUTH, NULL, -1),
	                 409, "{\"ok\":false,\"error\":\"sd_flush_pending\"}");
	{
		static const char *const gated[] = { "ota_present", "auth_matches" };
		expect_log(&fixture, gated, ARRAY_LENGTH(gated));
	}
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
	                              NULL, NULL, -1),
	                 401, "{\"ok\":false,\"error\":\"sd_auth\"}");
	overlay.drained = true;
	expect_immediate(dispatch_request(&fixture, "POST /api/reboot",
	                                  "X-Aius-SD-Auth: token\r\n", NULL, -1),
	                 200, "{\"ok\":true,\"rebooting\":true}");
	expect_immediate(dispatch_ota(&fixture, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE,
	                              OTA_AUTH, NULL, -1),
	                 200, "{\"ok\":true,\"activating\":true}");
	expect_immediate(dispatch_ota(&fixture,
	                              AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER,
	                              OTA_AUTH, NULL, -1),
	                 200, BOOT_OTHER_ACCEPTED);
}

/* Phase 4h H10: the settings mount policy against fake parsers and a fake
 * store. A body "ok" parses; anything else is refused by the parser. */
typedef struct {
	bool carve;
	bool shadowed[2];
	unsigned parses[2];
	unsigned applies[2];
	unsigned target_failures[2];
	unsigned puts[2];
	unsigned config_errors; /* what settings_target_failed counts */
} FakeSettings;

static FakeSettings g_fake_settings;

static bool fake_settings_carve(void *context)
{
	return ((FakeSettings *)context)->carve;
}

static bool fake_settings_shadowed(void *context, AiusOpticalSettingsFile file)
{
	return ((FakeSettings *)context)->shadowed[file];
}

static bool fake_settings_load(void *context, AiusOpticalSettingsFile file,
                               const uint8_t *data, size_t length, bool apply)
{
	FakeSettings *settings = (FakeSettings *)context;
	settings->parses[file]++;
	if (length != 2u || memcmp(data, "ok", 2u) != 0)
		return false;
	if (apply)
		settings->applies[file]++;
	return true;
}

static void fake_settings_target_failed(void *context,
                                        AiusOpticalSettingsFile file)
{
	FakeSettings *settings = (FakeSettings *)context;
	settings->target_failures[file]++;
	if (file == AIUS_OPTICAL_SETTINGS_CONFIG)
		settings->config_errors++;
}

static void fake_settings_put_restore(void *context,
                                      AiusOpticalSettingsFile file,
                                      const uint8_t *data, size_t length)
{
	(void)data;
	assert(length == 2u);
	((FakeSettings *)context)->puts[file]++;
}

/* lib/aius_sd_overlay's table (its own suite proves the real one). */
static AiusOpticalSettingsDecision fake_settings_decide(
	bool target_valid, AiusOpticalSettingsBackup backup)
{
	if (target_valid)
		return AIUS_OPTICAL_SETTINGS_KEEP;
	if (backup == AIUS_OPTICAL_SETTINGS_BACKUP_PARSED)
		return AIUS_OPTICAL_SETTINGS_RESTORE;
	if (backup == AIUS_OPTICAL_SETTINGS_BACKUP_REFUSED)
		return AIUS_OPTICAL_SETTINGS_FAULT;
	return AIUS_OPTICAL_SETTINGS_NOTHING;
}

static const AiusOpticalSettingsOps g_fake_settings_ops = {
	&g_fake_settings, fake_settings_carve, fake_settings_shadowed,
	fake_settings_load, fake_settings_target_failed,
	fake_settings_put_restore, fake_settings_decide
};

/* One load step as the composition runs it: begin, an optional
 * publication of `body` (NULL: the job failed first), end. */
static bool settings_step(AiusOpticalSettingsMount *mount,
                          AiusOpticalSettingsFile file, bool backup,
                          const char *body, bool job_ok)
{
	aius_optical_settings_load_begin(mount);
	if (body != NULL)
		assert(aius_optical_settings_publish(mount, &g_fake_settings_ops, file,
		                                     backup, (const uint8_t *)body,
		                                     strlen(body)));
	return aius_optical_settings_load_end(mount, &g_fake_settings_ops, file,
	                                      backup, job_ok);
}

static void fake_settings_reset(AiusOpticalSettingsMount *mount)
{
	memset(&g_fake_settings, 0, sizeof(g_fake_settings));
	g_fake_settings.carve = true;
	memset(mount, 0, sizeof(*mount));
}

static void test_settings_mount_restore(void)
{
	AiusOpticalSettingsMount mount;
	const AiusOpticalSettingsFile dsp = AIUS_OPTICAL_SETTINGS_DSP;
	const AiusOpticalSettingsFile config = AIUS_OPTICAL_SETTINGS_CONFIG;

	/* Target valid: applied, no backup read (KEEP). */
	fake_settings_reset(&mount);
	assert(!settings_step(&mount, dsp, false, "ok", true));
	assert(g_fake_settings.applies[dsp] == 1u);
	assert(mount.restores == 0u && mount.restore_faults == 0u);

	/* dsp.json torn mid-file, valid backup: the target's parse refuses, the
	 * RESTORE step applies the backup's model once and puts it as a
	 * restore-created accept (its flush writes the target only); one
	 * settings_restores. */
	fake_settings_reset(&mount);
	assert(settings_step(&mount, dsp, false, "torn", true));
	assert(g_fake_settings.applies[dsp] == 0u);
	assert(!settings_step(&mount, dsp, true, "ok", true));
	assert(g_fake_settings.applies[dsp] == 1u);
	assert(g_fake_settings.puts[dsp] == 1u);
	assert(mount.restores == 1u && mount.restore_faults == 0u);

	/* config.json the same: the public config is restored instead of the
	 * defaults, with one config error for the failed target (FR-S11). */
	fake_settings_reset(&mount);
	assert(settings_step(&mount, config, false, "torn", true));
	assert(g_fake_settings.config_errors == 1u);
	assert(!settings_step(&mount, config, true, "ok", true));
	assert(g_fake_settings.applies[config] == 1u);
	assert(g_fake_settings.puts[config] == 1u);
	assert(g_fake_settings.config_errors == 1u);
	assert(mount.restores == 1u);

	/* A target invalid with a backup that reaches the parser and is refused:
	 * both files kept, nothing applied, the defaults with one config error,
	 * one settings_restore_faults. */
	fake_settings_reset(&mount);
	assert(settings_step(&mount, config, false, "torn", true));
	assert(!settings_step(&mount, config, true, "bad", true));
	assert(g_fake_settings.applies[config] == 0u);
	assert(g_fake_settings.puts[config] == 0u);
	assert(g_fake_settings.config_errors == 1u);
	assert(mount.restores == 0u && mount.restore_faults == 1u);
	fake_settings_reset(&mount);
	assert(settings_step(&mount, dsp, false, "torn", true));
	assert(!settings_step(&mount, dsp, true, "bad", true));
	assert(g_fake_settings.applies[dsp] == 0u);
	assert(mount.restore_faults == 1u);

	/* A target invalid with no backup file counts nothing (FR-S10). */
	fake_settings_reset(&mount);
	assert(settings_step(&mount, dsp, false, "torn", true));
	assert(!settings_step(&mount, dsp, true, NULL, false));
	assert(mount.restores == 0u && mount.restore_faults == 0u);

	/* A fresh card with neither file: the config defaults and its one error
	 * as today, and no restore counted. */
	fake_settings_reset(&mount);
	assert(settings_step(&mount, config, false, NULL, false));
	assert(g_fake_settings.config_errors == 1u);
	assert(g_fake_settings.target_failures[config] == 1u);
	assert(!settings_step(&mount, config, true, NULL, false));
	assert(settings_step(&mount, dsp, false, NULL, false));
	assert(!settings_step(&mount, dsp, true, NULL, false));
	assert(g_fake_settings.target_failures[dsp] == 1u);
	assert(g_fake_settings.config_errors == 1u);
	assert(mount.restores == 0u && mount.restore_faults == 0u);

	/* A path the overlay shadows is neither loaded nor restored (Q1, NS7):
	 * acknowledged unapplied, no defaults, no config error, no restore
	 * step, whether the card's file parses, is refused or is missing. */
	fake_settings_reset(&mount);
	g_fake_settings.shadowed[config] = true;
	g_fake_settings.shadowed[dsp] = true;
	assert(!settings_step(&mount, config, false, "ok", true));
	assert(!settings_step(&mount, config, false, "torn", true));
	assert(!settings_step(&mount, config, false, NULL, false));
	assert(!settings_step(&mount, dsp, false, "torn", true));
	assert(g_fake_settings.parses[config] == 0u);
	assert(g_fake_settings.parses[dsp] == 0u);
	assert(g_fake_settings.config_errors == 0u);
	assert(g_fake_settings.target_failures[dsp] == 0u);

	/* A write accepted between the failed target and its restore: the
	 * restore step's shadow check wins, nothing applied, put or counted. */
	fake_settings_reset(&mount);
	assert(settings_step(&mount, dsp, false, "torn", true));
	g_fake_settings.shadowed[dsp] = true;
	assert(!settings_step(&mount, dsp, true, "ok", true));
	assert(g_fake_settings.applies[dsp] == 0u);
	assert(g_fake_settings.puts[dsp] == 0u);
	assert(mount.restores == 0u && mount.restore_faults == 0u);

	/* An 8 MiB fitment (no carve) with a torn dsp.json runs no RESTORE
	 * step (N1). */
	fake_settings_reset(&mount);
	g_fake_settings.carve = false;
	assert(!settings_step(&mount, dsp, false, "torn", true));
	assert(!settings_step(&mount, config, false, "torn", true));
	assert(g_fake_settings.config_errors == 1u);
}

/* ---- Phase 4h: the write-behind flush coordinator (proof line 5074) ----
 * The fake platform carries the REAL 4g fill hold (AiusOpticalWebFill), so
 * the hold clauses (NS2, R5-S1, S9) are read off the policy that gates the
 * fill scheduler, not off a counter. */
typedef struct {
	AiusOpticalWebFill web;
	uint32_t now_ms; /* the platform clock a hold release reads */
	bool present, ready, held, due, drained, http_deferred, fill_in_flight;
	bool storage_ready, card_detected, playback, settled;
	AiusOpticalSdFlushStart start_result;
	AiusOpticalSdFlushJob job;
	const char *fill_rel;
	AiusOpticalAppStoreResult put_result;
	unsigned starts, fill_cancels, cancel_roots, put_begins, put_aborts;
	unsigned mounted, card_lost, readback_ok, readback_bad;
	unsigned job_ends[4];
	uint8_t passes[4];
	unsigned pass_count;
	/* D-S5: the adapter's side, as optical_main.cpp's callback sees it. */
	bool card_in_slot, dat3_remuxed, file_open;
	bool adapter_mounted; /* D-S9: ops.mounted */
	unsigned remuxes;
} FakeFlush;

static FakeFlush g_ff;

static bool ff_present(void *c) { return ((FakeFlush *)c)->present; }
static bool ff_ready(void *c) { return ((FakeFlush *)c)->ready; }
static bool ff_held(void *c) { return ((FakeFlush *)c)->held; }
static bool ff_due(void *c, uint32_t now_ms) { (void)now_ms; return ((FakeFlush *)c)->due; }
static bool ff_drained(void *c) { return ((FakeFlush *)c)->drained; }
static bool ff_http_deferred(void *c) { return ((FakeFlush *)c)->http_deferred; }
static bool ff_fill_in_flight(void *c) { return ((FakeFlush *)c)->fill_in_flight; }
static bool ff_storage_ready(void *c) { return ((FakeFlush *)c)->storage_ready; }
static bool ff_playback(void *c) { return ((FakeFlush *)c)->playback; }
static bool ff_card_detected(void *c) { return ((FakeFlush *)c)->card_detected; }
static bool ff_settled(void *c) { return ((FakeFlush *)c)->settled; }
static const char *ff_fill_rel(void *c) { return ((FakeFlush *)c)->fill_rel; }

/* The platform's one latching cancel (web_cache_fill_cancel). */
static void ff_fill_cancel(void *c)
{
	FakeFlush *f = (FakeFlush *)c;
	f->fill_cancels++;
	(void)aius_optical_web_fill_cancel(&f->web, f->fill_in_flight);
}

static void ff_fill_hold_release(void *c)
{
	FakeFlush *f = (FakeFlush *)c;
	aius_optical_web_fill_hold_release(&f->web, f->now_ms);
}

static AiusOpticalSdFlushStart ff_start(void *c, uint32_t now_ms,
                                        AiusOpticalSdFlushJob *job)
{
	FakeFlush *f = (FakeFlush *)c;
	(void)now_ms;
	f->starts++;
	*job = f->job;
	return f->start_result;
}

static void ff_job_end(void *c, AiusOpticalSdFlushOutcome outcome,
                       uint32_t now_ms)
{
	(void)now_ms;
	((FakeFlush *)c)->job_ends[outcome]++;
}

static void ff_note_pass(void *c, uint8_t copy)
{
	FakeFlush *f = (FakeFlush *)c;
	assert(f->pass_count < 4u);
	f->passes[f->pass_count++] = copy;
}

static void ff_note_readback(void *c, bool ok)
{
	FakeFlush *f = (FakeFlush *)c;
	if (ok)
		f->readback_ok++;
	else
		f->readback_bad++;
}

static void ff_note_mounted(void *c, uint32_t now_ms)
{
	(void)now_ms;
	((FakeFlush *)c)->mounted++;
}

static void ff_note_card_lost(void *c) { ((FakeFlush *)c)->card_lost++; }

/* Stops the fill file with no hold latched. */
static bool ff_cancel_root(void *c)
{
	FakeFlush *f = (FakeFlush *)c;
	f->cancel_roots++;
	return f->fill_in_flight;
}

static AiusOpticalAppStoreResult ff_put_begin(void *c, const char *rel,
                                              size_t rel_length,
                                              uint32_t length)
{
	FakeFlush *f = (FakeFlush *)c;
	(void)rel;
	(void)rel_length;
	(void)length;
	f->put_begins++;
	return f->put_result;
}

static void ff_put_abort(void *c) { ((FakeFlush *)c)->put_aborts++; }

/* sd_flush_card_present_after_fault: re-mux DAT3 (not with a file open)
 * and read it. */
static bool ff_mounted(void *c) { return ((FakeFlush *)c)->adapter_mounted; }

static bool ff_card_present_after_fault(void *c)
{
	FakeFlush *f = (FakeFlush *)c;
	if (f->file_open)
		return true;
	if (!f->dat3_remuxed) {
		f->dat3_remuxed = true;
		f->remuxes++;
	}
	return f->card_in_slot;
}

static const AiusOpticalSdFlushOps g_ff_ops = {
	&g_ff, ff_present, ff_ready, ff_held, ff_due, ff_drained,
	ff_http_deferred, ff_fill_in_flight, ff_fill_cancel, ff_fill_hold_release,
	ff_storage_ready, ff_start, ff_job_end, ff_note_pass, ff_note_readback,
	ff_note_mounted, ff_note_card_lost, ff_playback, ff_card_detected,
	ff_settled, ff_fill_rel, ff_cancel_root, ff_put_begin, ff_put_abort,
	ff_card_present_after_fault, ff_mounted
};

/* A carve-fitted, card-ready board with nothing pending and no fill. */
static void ff_reset(AiusOpticalSdFlush *flush)
{
	memset(&g_ff, 0, sizeof(g_ff));
	aius_optical_web_fill_init(&g_ff.web);
	g_ff.present = true;
	g_ff.ready = true;
	g_ff.drained = true;
	g_ff.storage_ready = true;
	g_ff.card_detected = true;
	g_ff.settled = true;
	g_ff.start_result = AIUS_OPTICAL_SD_FLUSH_START_STARTED;
	g_ff.put_result = AIUS_OPTICAL_APP_STORE_OK;
	g_ff.card_in_slot = true;
	g_ff.adapter_mounted = true; /* a mounted card: BEGIN reuses it */
	memset(flush, 0, sizeof(*flush));
}

static AiusOpticalSdFlushEnd ff_end_ok(uint8_t passes)
{
	AiusOpticalSdFlushEnd end;
	memset(&end, 0, sizeof(end));
	end.result = true;
	end.mounted = true;
	end.passes_closed = passes;
	return end;
}

/* NS2, B1: a www PUT of the path whose fill is in flight. */
static void test_sd_flush_put_stops_fill(void)
{
	AiusOpticalSdFlush flush;
	static AiusHttpTransaction tx;
	uint32_t fill_cancels = 0u;
	static const char rel[] = "www/presets/x.json";

	ff_reset(&flush);
	g_ff.fill_in_flight = true;
	g_ff.fill_rel = "www/presets/X.json"; /* FAT case-folds */
	assert(aius_optical_sd_flush_put_begin(&flush, &g_ff_ops, &fill_cancels,
	                                       rel, strlen(rel), 12u) ==
	       AIUS_OPTICAL_APP_STORE_OK);
	/* Stopped through the no-latch root cancel, counted in fill_cancels;
	 * the latching 4g cancel never ran, so no hold: the walk resumes at
	 * its next schedule epoch, not 2 s later. */
	assert(g_ff.cancel_roots == 1u && fill_cancels == 1u);
	assert(g_ff.fill_cancels == 0u && g_ff.web.latches == 0u);
	assert(!aius_optical_web_fill_held(&g_ff.web, 0u));
	assert(!aius_optical_web_fill_paused(&g_ff.web, 0u));
	assert(flush.put_open);
	/* The one streamed put: a second is BUSY until commit or abort. */
	assert(aius_optical_sd_flush_put_begin(&flush, &g_ff_ops, &fill_cancels,
	                                       rel, strlen(rel), 12u) ==
	       AIUS_OPTICAL_APP_STORE_BUSY);
	assert(g_ff.put_begins == 1u);
	assert(aius_optical_sd_flush_put_close(&flush));
	assert(!aius_optical_sd_flush_put_close(&flush));

	/* A fill of another path, or a prefix of this one, is left running. */
	g_ff.fill_rel = "www/presets/x.json.gz";
	assert(aius_optical_sd_flush_put_begin(&flush, &g_ff_ops, &fill_cancels,
	                                       rel, strlen(rel), 12u) ==
	       AIUS_OPTICAL_APP_STORE_OK);
	assert(g_ff.cancel_roots == 1u && fill_cancels == 1u);
	aius_optical_sd_flush_put_abort(&flush, &g_ff_ops);
	assert(g_ff.put_aborts == 1u && !flush.put_open);
	aius_optical_sd_flush_put_abort(&flush, &g_ff_ops);
	assert(g_ff.put_aborts == 1u);

	/* Store full: 503 sd_busy, no put open (Q4, N7). */
	g_ff.put_result = AIUS_OPTICAL_APP_STORE_BUSY;
	g_ff.fill_in_flight = false;
	assert(aius_optical_sd_flush_put_begin(&flush, &g_ff_ops, &fill_cancels,
	                                       rel, strlen(rel), 12u) ==
	       AIUS_OPTICAL_APP_STORE_BUSY);
	assert(!flush.put_open);

	/* NS5: torn down after DISPATCH and before SD_START (the transaction
	 * re-inited, no start held or queued): the sweep frees the PUTTING
	 * entry. While the transaction can still start or finish its sink, it
	 * stays. D4: torn down mid-body (a client dropped during a sync) is the
	 * same re-inited transaction. */
	g_ff.put_result = AIUS_OPTICAL_APP_STORE_OK;
	memset(&tx, 0, sizeof(tx));
	assert(aius_optical_sd_flush_put_begin(&flush, &g_ff_ops, &fill_cancels,
	                                       rel, strlen(rel), 12u) ==
	       AIUS_OPTICAL_APP_STORE_OK);
	tx.state = AIUS_HTTP_TX_DISPATCH;
	aius_optical_sd_flush_sweep_orphan(&flush, &g_ff_ops, &tx, false, false);
	tx.state = AIUS_HTTP_TX_SD_TRANSFER;
	aius_optical_sd_flush_sweep_orphan(&flush, &g_ff_ops, &tx, false, false);
	tx.state = AIUS_HTTP_TX_IDLE;
	aius_optical_sd_flush_sweep_orphan(&flush, &g_ff_ops, &tx, true, false);
	assert(flush.put_open && g_ff.put_aborts == 1u);
	aius_optical_sd_flush_sweep_orphan(&flush, &g_ff_ops, &tx, false, false);
	assert(!flush.put_open && g_ff.put_aborts == 2u);
	/* Nothing open: the sweep is a no-op. */
	aius_optical_sd_flush_sweep_orphan(&flush, &g_ff_ops, &tx, false, false);
	assert(g_ff.put_aborts == 2u);
}

/* The flush start's precedence (G6, G9, S8, S9, R5-S1, Q3) and the job end. */
static void test_sd_flush_start_and_settle(void)
{
	AiusOpticalSdFlush flush;
	AiusOpticalSdFlushEnd end;

	/* Nothing due, held, frozen, card-unready or no carve: no start. */
	ff_reset(&flush);
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	g_ff.due = true;
	g_ff.held = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	g_ff.held = false;
	g_ff.ready = false;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	g_ff.ready = true;
	g_ff.present = false;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	g_ff.present = true;
	flush.frozen = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	flush.frozen = false;
	assert(g_ff.starts == 0u);

	/* S8: a page miss held at SD_START or a /sd/ index held at DISPATCH
	 * behind a fill runs before the flush starts. */
	g_ff.http_deferred = true;
	g_ff.fill_in_flight = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	assert(g_ff.fill_cancels == 0u && g_ff.starts == 0u);
	g_ff.http_deferred = false;

	/* G9: the flush stops the fill file once (latching the 4g hold) and
	 * owns the pass until it stops; then waits for the engine. */
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 1u));
	assert(g_ff.fill_cancels == 1u && g_ff.web.latches == 1u);
	g_ff.fill_in_flight = false;
	g_ff.storage_ready = false;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 2u));
	assert(g_ff.starts == 0u);
	g_ff.storage_ready = true;
	g_ff.job.copy_before_write = true;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 3u));
	assert(g_ff.starts == 1u && flush.running);
	/* Q3: the start consults no replay state; one job at a time. */
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 4u));
	assert(g_ff.starts == 1u);
	assert(aius_optical_web_fill_held(&g_ff.web, 100000u)); /* latched */

	/* R5-S1: the job end releases the flush's latch, and the fill file
	 * resumes 2 s after it (no HTTP SD job after). Backup first, two
	 * passes, read-back OK, the notes before the job end. */
	g_ff.now_ms = 5000u;
	end = ff_end_ok(2u);
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 5000u);
	assert(!flush.running && g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_OK] == 1u);
	assert(g_ff.pass_count == 2u && g_ff.passes[0] == 1u &&
	       g_ff.passes[1] == 0u && g_ff.readback_ok == 1u);
	assert(aius_optical_web_fill_held(&g_ff.web, 6999u));
	assert(!aius_optical_web_fill_held(&g_ff.web, 7000u));
	/* A second settle with nothing running is a no-op. */
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 5001u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_OK] == 1u);

	/* S9: a flush that cancelled no fill releases no hold: an HTTP
	 * canceller's latch keeps its own (no) expiry. */
	ff_reset(&flush);
	g_ff.due = true;
	assert(aius_optical_web_fill_cancel(&g_ff.web, false) == false);
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	end = ff_end_ok(0u);
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 10u);
	assert(g_ff.web.latches == 1u);
	assert(aius_optical_web_fill_held(&g_ff.web, 1000000u));

	/* Outcomes: chunk CRC, card gone, failed; a target-first and a
	 * skip-backup COPY_BEFORE_WRITE; a read-back mismatch. */
	ff_reset(&flush);
	g_ff.due = true;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	end = ff_end_ok(0u);
	end.fault = true;
	end.input_faulted = true;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 1u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CHUNK_CRC] == 1u);
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 2u));
	end.input_faulted = false;
	end.mounted = false;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 3u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CARD_GONE] == 1u);
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 4u));
	end.mounted = true;
	end.result = false;
	end.fault = false;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 5u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_FAILED] == 1u);
	g_ff.job.copy_before_write = true;
	g_ff.job.target_first = true;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 6u));
	end = ff_end_ok(2u);
	end.fault = true;
	end.readback_mismatch = true;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 7u);
	assert(g_ff.pass_count == 2u && g_ff.passes[0] == 0u &&
	       g_ff.passes[1] == 1u && g_ff.readback_bad == 1u);
	g_ff.job.skip_backup = true;
	g_ff.job.target_first = false;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 8u));
	end = ff_end_ok(1u);
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 9u);
	assert(g_ff.pass_count == 3u && g_ff.passes[2] == 0u &&
	       g_ff.readback_ok == 1u);

	/* No job due after the fill stopped: the flush's latch is released;
	 * an engine refusal is a counted FAILED, never a spin. */
	ff_reset(&flush);
	g_ff.due = true;
	g_ff.fill_in_flight = true;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	g_ff.fill_in_flight = false;
	g_ff.start_result = AIUS_OPTICAL_SD_FLUSH_START_NONE;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 1u));
	assert(g_ff.web.latches == 0u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_FAILED] == 0u);
	g_ff.start_result = AIUS_OPTICAL_SD_FLUSH_START_REFUSED;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 2u));
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_FAILED] == 1u);
	assert(!flush.running);
}

/* Disconnect and remount (H3, H11, S1, N5, NS3, NS7) through the idle step. */
static void test_sd_flush_remount_retry(void)
{
	AiusOpticalSdFlush flush;
	AiusOpticalSdFlushEnd end;
	uint32_t t;

	/* H3/H11: a card-gone fault on a flush job: CARD_GONE (the overlay
	 * sets held); held, no flush starts however dirty. */
	ff_reset(&flush);
	g_ff.due = true;
	g_ff.drained = false;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	end = ff_end_ok(0u);
	end.fault = true;
	end.mounted = false;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 10u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CARD_GONE] == 1u);
	g_ff.held = true;
	g_ff.ready = false;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 20u));
	g_ff.ready = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 20u));
	assert(g_ff.starts == 1u);

	/* Off playback the ordinary probe runs (PROBE when due, else FILL). */
	ff_reset(&flush);
	g_ff.held = true;
	g_ff.drained = false;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, true) ==
	       AIUS_OPTICAL_SD_IDLE_PROBE);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, false) ==
	       AIUS_OPTICAL_SD_IDLE_FILL);

	/* S1: with the source on, the held flush retries its BEGIN at most once
	 * per AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS and only while DAT3 reads a
	 * card; a detected card whose BEGIN keeps failing backs off to 60 s. */
	g_ff.playback = true;
	t = 1000u;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT);
	{
		static const uint32_t intervals[] = { 4000u, 8000u, 16000u, 32000u,
		                                      60000u, 60000u };
		/* As the platform runs it: the chosen probe step completes (its
		 * verdict, note_probe) before the next idle step. */
		uint32_t at = t;
		uint32_t due = t + AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS;
		for (size_t i = 0; i < ARRAY_LENGTH(intervals); i++) {
			aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, false, at);
			assert(flush.remount_interval_ms == intervals[i]);
			assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, due - 1u,
			                                 false) ==
			       AIUS_OPTICAL_SD_IDLE_FILL);
			assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, due,
			                                 false) ==
			       AIUS_OPTICAL_SD_IDLE_REMOUNT);
			at = due;
			due = at + intervals[i];
		}
		at = due;
		/* NS3: DAT3 reads no card: no retry, and the next true is an edge
		 * that resets the interval. */
		g_ff.card_detected = false;
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, at, false) ==
		       AIUS_OPTICAL_SD_IDLE_FILL);
		assert(flush.remount_interval_ms == 0u);
		g_ff.card_detected = true;
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, at + 1999u,
		                                 false) ==
		       AIUS_OPTICAL_SD_IDLE_FILL);
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, at + 2000u,
		                                 false) ==
		       AIUS_OPTICAL_SD_IDLE_REMOUNT);
		t = at + 2000u;
	}
	/* A returned card mounts on that retry: held clears (the overlay's
	 * note_mounted), then the flush starts with the source still on. */
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, true, t);
	assert(g_ff.mounted == 1u && flush.mount_seen);
	g_ff.held = false;
	g_ff.due = true;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t + 1u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(flush.running);

	/* N5: a card-gone fault 3 s after that held-clearing mount doubles the
	 * retry interval; one 6 s after a later mount does not. */
	flush.running = false;
	flush.remount_interval_ms = 0u;
	aius_optical_sd_flush_note_card_gone(&flush, t + 3000u);
	assert(flush.remount_interval_ms == 4000u && !flush.mount_seen);
	aius_optical_sd_flush_note_card_gone(&flush, t + 3001u);
	assert(flush.remount_interval_ms == 4000u);
	g_ff.held = true;
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, true, t + 10000u);
	aius_optical_sd_flush_note_card_gone(&flush, t + 16000u);
	assert(flush.remount_interval_ms == 4000u);

	/* A drained store, a freeze or the carve absent: no retry. */
	ff_reset(&flush);
	g_ff.held = true;
	g_ff.playback = true;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, false) ==
	       AIUS_OPTICAL_SD_IDLE_FILL);
	g_ff.drained = false;
	flush.frozen = true;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, true) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	flush.frozen = false;
	g_ff.present = false;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, false) ==
	       AIUS_OPTICAL_SD_IDLE_FILL);

	/* NS7: a cardless boot: the probe issues no BEGIN while DAT3 reads
	 * absent (the no-card verdict noted), and runs once it reads a card. */
	ff_reset(&flush);
	g_ff.card_detected = false;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, true) ==
	       AIUS_OPTICAL_SD_IDLE_NO_CARD);
	assert(g_ff.card_lost == 1u);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, false) ==
	       AIUS_OPTICAL_SD_IDLE_FILL);
	g_ff.card_detected = true;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, true) ==
	       AIUS_OPTICAL_SD_IDLE_PROBE);
	/* A probe's no-card verdict that was no retry notes the loss only. */
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, false, 0u);
	assert(g_ff.card_lost == 2u && flush.remount_interval_ms == 0u);
	/* A mount with nothing held is no held-clearing mount. */
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, true, 0u);
	assert(g_ff.mounted == 0u && !flush.mount_seen);
}

/* The OTA/reboot drain gate and the H9 freeze (FR-S8, D6, R4-B1, R4-S3,
 * R5-S1, S8, S9). */
static void test_sd_flush_write_gate(void)
{
	AiusOpticalSdFlush flush;

	/* FR-S8, D6: anything dirty, flushing or tombstoned (a store-full
	 * faulted entry stays pending): DRAIN, no latch. No carve: drained. */
	ff_reset(&flush);
	g_ff.drained = false;
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 0u) ==
	       AIUS_OPTICAL_SD_GATE_DRAIN);
	assert(!flush.frozen && !aius_optical_sd_flush_drained(&g_ff_ops));
	g_ff.present = false;
	assert(aius_optical_sd_flush_drained(&g_ff_ops));
	g_ff.present = true;
	/* Once the entry frees and flushes, the gate passes. */
	g_ff.drained = true;
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 1u) ==
	       AIUS_OPTICAL_SD_GATE_PASS);
	aius_optical_sd_freeze_release(&flush, &g_ff_ops);

	/* R4-B1: the first re-check passes; the latch stops the in-flight fill
	 * file (R4-S3: it no longer abandons, the gate waits for it); no SD job
	 * of ours starts (no flush, no probe, no fill). */
	ff_reset(&flush);
	g_ff.fill_in_flight = true;
	g_ff.settled = false;
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 100u) ==
	       AIUS_OPTICAL_SD_GATE_WAIT);
	assert(flush.frozen && g_ff.fill_cancels == 1u);
	g_ff.due = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 101u));
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 101u, true) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(g_ff.starts == 0u);
	g_ff.due = false;
	g_ff.fill_in_flight = false;
	g_ff.settled = true;
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 200u) ==
	       AIUS_OPTICAL_SD_GATE_PASS);
	assert(g_ff.fill_cancels == 1u); /* latched once */
	/* A PUT accepted during the wait: the final drain query fails, the
	 * service abandons and unfreezes; the freeze's fill latch is released
	 * and the fill resumes 2 s after the unfreeze (R5-S1); the PUT then
	 * flushes. */
	g_ff.drained = false;
	assert(!aius_optical_sd_flush_drained(&g_ff_ops));
	g_ff.now_ms = 300u;
	aius_optical_sd_freeze_release(&flush, &g_ff_ops);
	assert(!flush.frozen && g_ff.web.latches == 0u);
	assert(aius_optical_web_fill_held(&g_ff.web, 2299u));
	assert(!aius_optical_web_fill_held(&g_ff.web, 2300u));
	g_ff.due = true;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 301u));
	assert(g_ff.starts == 1u);
	/* The abandoned request re-enters the gate only as DRAIN until the
	 * flush drains (R5-S2's gate half; the services clear the request). */
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 302u) ==
	       AIUS_OPTICAL_SD_GATE_DRAIN);
	assert(!flush.frozen);

	/* An in-flight SD job outlasting AIUS_OPTICAL_SD_FREEZE_MS: BUSY
	 * (ota_sd_busy) at the ceiling, WAIT before it. */
	ff_reset(&flush);
	g_ff.settled = false;
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 1000u) ==
	       AIUS_OPTICAL_SD_GATE_WAIT);
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 2999u) ==
	       AIUS_OPTICAL_SD_GATE_WAIT);
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 3000u) ==
	       AIUS_OPTICAL_SD_GATE_BUSY);
	/* S9: an unfreeze that cancelled no fill releases no hold: an HTTP
	 * canceller's latch keeps its own expiry. */
	assert(!aius_optical_web_fill_cancel(&g_ff.web, false));
	aius_optical_sd_freeze_release(&flush, &g_ff_ops);
	assert(g_ff.web.latches == 1u);
	aius_optical_sd_freeze_release(&flush, &g_ff_ops); /* idempotent */
	assert(g_ff.web.latches == 1u);
}

/* Phase 4zd: a USAGE_REFRESH scan is fill_in_flight true with fill_rel NULL.
 * A flush start and the H9 freeze cancel it through the one fill_cancel op;
 * a card request and a STORE put do not. */
static void test_sd_scan_flush_and_freeze(void)
{
	AiusOpticalSdFlush flush;
	bool requested;
	uint32_t fill_cancels = 0u;
	static const char rel[] = "www/presets/x.json";

	/* A flush start cancels the scan once and waits for the root to stop. */
	ff_reset(&flush);
	g_ff.due = true;
	g_ff.fill_in_flight = true;
	assert(g_ff.fill_rel == NULL);
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	assert(g_ff.fill_cancels == 1u && flush.cancelled_fill && g_ff.starts == 0u);
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 1u));
	assert(g_ff.fill_cancels == 1u && g_ff.starts == 0u);
	g_ff.fill_in_flight = false;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 2u));
	assert(g_ff.starts == 1u && flush.running);

	/* The H9 freeze (reboot and OTA: reset_on_busy false) cancels the scan
	 * and does not abandon while it stops inside the 2 s ceiling. */
	ff_reset(&flush);
	g_ff.settled = false;
	g_ff.fill_in_flight = true;
	requested = true;
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested, 0u,
	                                    false) == AIUS_OPTICAL_SD_SERVICE_WAIT);
	assert(flush.frozen && requested && g_ff.fill_cancels == 1u);
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested,
	                                    AIUS_OPTICAL_SD_FREEZE_MS - 1u, false) ==
	       AIUS_OPTICAL_SD_SERVICE_WAIT);
	assert(requested && flush.frozen);
	g_ff.fill_in_flight = false;
	g_ff.settled = true;
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested,
	                                    AIUS_OPTICAL_SD_FREEZE_MS - 1u, false) ==
	       AIUS_OPTICAL_SD_SERVICE_RUN);
	assert(g_ff.fill_cancels == 1u);

	/* A STORE put during a scan cancels nothing. */
	ff_reset(&flush);
	g_ff.fill_in_flight = true;
	assert(aius_optical_sd_flush_put_begin(&flush, &g_ff_ops, &fill_cancels,
	                                       rel, strlen(rel), 12u) ==
	       AIUS_OPTICAL_APP_STORE_OK);
	assert(g_ff.cancel_roots == 0u && g_ff.fill_cancels == 0u &&
	       fill_cancels == 0u && g_ff.put_begins == 1u);
	g_ff.put_result = AIUS_OPTICAL_APP_STORE_BUSY;
	aius_optical_sd_flush_put_abort(&flush, &g_ff_ops);
	assert(aius_optical_sd_flush_put_begin(&flush, &g_ff_ops, &fill_cancels,
	                                       rel, strlen(rel), 12u) ==
	       AIUS_OPTICAL_APP_STORE_BUSY);
	assert(g_ff.cancel_roots == 0u && g_ff.fill_cancels == 0u &&
	       fill_cancels == 0u);
}

/* A card route with sd_busy (a scan owns the root) answers 503 sd_busy and
 * calls no cancel. */
static void test_sd_scan_card_route_busy(void)
{
	Fixture fixture;
	AiusOpticalSdFlush flush;

	ff_reset(&flush);
	g_ff.fill_in_flight = true;
	fixture_init(&fixture);
	fixture.fake.sd_busy = true;
	expect_immediate(dispatch_get(&fixture, "GET /sd/www/index.html"), 503,
	                 "{\"ok\":false,\"error\":\"sd_busy\"}");
	/* The fixture is not wired to g_ff_ops: the card-route no-cancel
	 * guarantee is pinned by the optical-cutover text checks. */
}

/* The preempt, the shared prefix and the usage-start gate. */
static void test_sd_flush_preempt_scan(void)
{
	AiusOpticalSdFlush flush;

	/* Due flush + running scan: cancels once; a second call does not. */
	ff_reset(&flush);
	g_ff.due = true;
	g_ff.fill_in_flight = true;
	assert(aius_optical_sd_usage_may_start(&flush));
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	assert(g_ff.fill_cancels == 1u && flush.cancelled_fill);
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 1u, true);
	assert(g_ff.fill_cancels == 1u);
	assert(!aius_optical_sd_usage_may_start(&flush));

	/* Nothing happens with no scan, nothing due, held, frozen, an HTTP start
	 * deferred, or a flush already running. */
	ff_reset(&flush);
	g_ff.due = true;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, false);
	g_ff.due = false;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	g_ff.due = true;
	g_ff.held = true;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	g_ff.held = false;
	flush.frozen = true;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	flush.frozen = false;
	g_ff.http_deferred = true;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	g_ff.http_deferred = false;
	flush.fault_remount = true;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	flush.fault_remount = false;
	flush.running = true;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	flush.running = false;
	assert(g_ff.fill_cancels == 0u && !flush.cancelled_fill);
	assert(aius_optical_sd_usage_may_start(&flush));

	/* The factored prefix answers true for a running flush, so the idle step
	 * answers IDLE_WAIT. */
	ff_reset(&flush);
	flush.running = true;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0u, true) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);

	/* A preempted scan waits for its flush: with cancelled_fill set and the
	 * next epoch's prefix failing, the usage refresh may not start. */
	ff_reset(&flush);
	g_ff.due = true;
	g_ff.fill_in_flight = true;
	aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, 0u, true);
	g_ff.fill_in_flight = false;
	g_ff.http_deferred = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 1u));
	assert(!aius_optical_sd_usage_may_start(&flush));
	g_ff.http_deferred = false;
	g_ff.held = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 2u));
	assert(!aius_optical_sd_usage_may_start(&flush));
	g_ff.held = false;
	flush.fault_remount = true;
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 3u));
	assert(!aius_optical_sd_usage_may_start(&flush));
	flush.fault_remount = false;
	/* BEGIN blocked: no mount, live playback, DAT3 reads no card. */
	g_ff.adapter_mounted = false;
	g_ff.playback = true;
	g_ff.card_detected = false;
	assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops, 3u) !=
	       AIUS_OPTICAL_SD_BEGIN_NONE);
	assert(!aius_optical_sd_flush_start(&flush, &g_ff_ops, 3u));
	assert(!aius_optical_sd_usage_may_start(&flush));
	g_ff.adapter_mounted = true;
	g_ff.playback = false;
	g_ff.card_detected = true;
	/* Once the flush settles, the refresh may run. */
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 4u));
	{
		AiusOpticalSdFlushEnd end = ff_end_ok(0u);
		aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 5u);
	}
	assert(aius_optical_sd_usage_may_start(&flush));
}

/* A reboot request with a dirty entry behind a scan reaches the flush, not
 * DRAIN, inside the 500 ms deadline when the flush itself fits. The epoch
 * composes sd_lifecycle_service: preempt, return while the root runs, else
 * the idle step. */
static void test_sd_scan_reboot_reaches_flush(void)
{
	AiusOpticalSdFlush flush;
	uint32_t t;
	bool flush_started = false;
	bool drained = false;

	ff_reset(&flush);
	g_ff.due = true;
	g_ff.drained = false;
	g_ff.fill_in_flight = true; /* the scan */
	{
		/* The firmware polls the drain gate with the request pending; the
		 * 500 ms deadline (optical_main.cpp) is its last poll. DRAIN abandons,
		 * so the gate is consulted when drained or at the deadline. */
		volatile bool requested = true;
		bool passed = false;

		for (t = 0u; t < 500u; t += 10u) {
			const bool scan_running = g_ff.fill_in_flight;
			AiusOpticalSdService svc;

			aius_optical_sd_flush_preempt_scan(&flush, &g_ff_ops, t,
			                                   scan_running);
			/* The scan stops at its next chunk boundary after the cancel. */
			if (g_ff.fill_cancels != 0u && t >= 30u)
				g_ff.fill_in_flight = false;
			if (!scan_running) {
				if (flush.running) {
					if (t >= 60u) {
						AiusOpticalSdFlushEnd end = ff_end_ok(0u);
						aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end,
						                             t);
						g_ff.drained = true;
					}
				} else if (aius_optical_sd_idle_step(&flush, &g_ff_ops, t,
				                                     false) ==
				               AIUS_OPTICAL_SD_IDLE_WAIT &&
				           flush.running) {
					flush_started = true;
				}
			}
			if (!g_ff.drained && t + 10u < 500u)
				continue;
			svc = aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested, t,
			                                   false);
			/* Never abandoned: not FLUSH_PENDING, request kept. */
			assert(svc == AIUS_OPTICAL_SD_SERVICE_RUN && requested);
			passed = true;
			break;
		}
		drained = passed;
	}
	assert(flush_started && drained && g_ff.starts == 1u && t < 500u);
	assert(g_ff.fill_cancels == 1u);
}

/* R4-B1, R5-S2, the freeze ceiling: the services' SD step. */
static void test_sd_service_gate(void)
{
	AiusOpticalSdFlush flush;
	bool requested;
	uint32_t runs = 0u;

	/* Unflushed writes: abandoned (ota_flush_pending), request cleared;
	 * called every epoch after, until the flush drains and beyond, it never
	 * runs again (no park, no reset). */
	ff_reset(&flush);
	g_ff.drained = false;
	requested = true;
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested, 0u,
	                                    false) ==
	       AIUS_OPTICAL_SD_SERVICE_FLUSH_PENDING);
	assert(!requested && !flush.frozen);
	for (uint32_t epoch = 1u; epoch < 200u; epoch++) {
		if (epoch == 100u)
			g_ff.drained = true;
		if (aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested,
		                                 epoch * 10u, false) ==
		    AIUS_OPTICAL_SD_SERVICE_RUN)
			runs++;
	}
	assert(runs == 0u && !flush.frozen);

	/* The freeze ceiling: the OTA services abandon with ota_sd_busy,
	 * unfrozen and cleared, the fill latch released; the reboot service
	 * proceeds (it resets). */
	ff_reset(&flush);
	g_ff.settled = false;
	g_ff.fill_in_flight = true;
	requested = true;
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested, 0u,
	                                    false) == AIUS_OPTICAL_SD_SERVICE_WAIT);
	assert(flush.frozen && requested && g_ff.web.latches == 1u);
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested,
	                                    AIUS_OPTICAL_SD_FREEZE_MS, false) ==
	       AIUS_OPTICAL_SD_SERVICE_SD_BUSY);
	assert(!requested && !flush.frozen && g_ff.web.latches == 0u);
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested,
	                                    AIUS_OPTICAL_SD_FREEZE_MS + 1u,
	                                    false) == AIUS_OPTICAL_SD_SERVICE_IDLE);
	ff_reset(&flush);
	g_ff.settled = false;
	requested = true;
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested, 0u,
	                                    true) == AIUS_OPTICAL_SD_SERVICE_WAIT);
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested,
	                                    AIUS_OPTICAL_SD_FREEZE_MS, true) ==
	       AIUS_OPTICAL_SD_SERVICE_RUN);
	assert(requested && aius_optical_sd_service_final(&flush, &g_ff_ops,
	                                                  &requested));

	/* R4-B1: the gate passes, a PUT is accepted during the wait, the final
	 * query fails: abandoned before the one-way step, unfrozen, cleared. */
	ff_reset(&flush);
	requested = true;
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested, 0u,
	                                    false) == AIUS_OPTICAL_SD_SERVICE_RUN);
	assert(flush.frozen);
	g_ff.drained = false;
	assert(!aius_optical_sd_service_final(&flush, &g_ff_ops, &requested));
	assert(!requested && !flush.frozen);
	/* S2: a pre-4h preset delete drained (no backup): OTA proceeds. */
	g_ff.drained = true;
	requested = true;
	assert(aius_optical_sd_service_gate(&flush, &g_ff_ops, &requested, 1u,
	                                    false) == AIUS_OPTICAL_SD_SERVICE_RUN);
	assert(aius_optical_sd_service_final(&flush, &g_ff_ops, &requested));
}

/* E3 and D1. */
static size_t g_reput_writes;

static size_t reput_writer(uint8_t *body, size_t capacity)
{
	g_reput_writes++;
	if (capacity < 3u)
		return 0u;
	memcpy(body, "{}\n", 3u);
	return 3u;
}

static size_t reput_writer_full(uint8_t *body, size_t capacity)
{
	(void)body;
	(void)capacity;
	return 0u;
}

static void test_card_ops_and_reput(void)
{
	static AiusHttpTransaction tx;
	uint8_t body[16];
	const uint8_t *bytes = NULL;
	uint32_t length = 0u;

	/* E3: DAT3 re-muxed after a card-gone invalidation: no direct card op
	 * (503 sd_unavailable through sd_ready, test_sd_index_refusals); a
	 * mode-only drop leaves DAT3 clear and the card reads as today. */
	assert(!aius_optical_app_card_ops_ready(true, true));
	assert(aius_optical_app_card_ops_ready(true, false));
	assert(!aius_optical_app_card_ops_ready(false, false));

	/* D1: a GET /api/status response being sent: the re-put waits and does
	 * not serialise; the first idle step serialises into the body. */
	memset(&tx, 0, sizeof(tx));
	tx.state = AIUS_HTTP_TX_SEND;
	tx.outstanding_action = AIUS_HTTP_ACTION_WRITE;
	assert(!aius_optical_settings_reput(&tx, body, sizeof(body), reput_writer,
	                                    &bytes, &length));
	tx.state = AIUS_HTTP_TX_IDLE;
	assert(!aius_optical_settings_reput(&tx, body, sizeof(body), reput_writer,
	                                    &bytes, &length));
	assert(g_reput_writes == 0u);
	tx.outstanding_action = AIUS_HTTP_ACTION_NONE;
	assert(aius_optical_app_http_idle(&tx));
	assert(aius_optical_settings_reput(&tx, body, sizeof(body), reput_writer,
	                                   &bytes, &length));
	assert(g_reput_writes == 1u && bytes == body && length == 3u);
	assert(!aius_optical_settings_reput(&tx, body, sizeof(body),
	                                    reput_writer_full, &bytes, &length));
	assert(!aius_optical_settings_reput(NULL, body, sizeof(body),
	                                    reput_writer, &bytes, &length));
	assert(!aius_optical_settings_reput(&tx, NULL, 0u, reput_writer, &bytes,
	                                    &length));
}

/* lib/aius_json's number reader is not correctly rounded at 17 significant
 * digits, so a %.17g double can come back one ulp off: compare within 1e-12
 * relative (far below any audible or validated quantity). */
static bool near(double a, double b)
{
	const double scale = (a < 0 ? -a : a) + (b < 0 ? -b : b) + 1e-300;
	const double diff = a > b ? a - b : b - a;
	return diff <= scale * 1e-12;
}

static bool dsp_eqs_equal(const AiusDspModelEq *a, const AiusDspModelEq *b,
                          uint8_t count)
{
	for (uint8_t i = 0; i < count; i++)
		if (a[i].type != b[i].type ||
		    (a[i].type != 0u &&
		     (!near(a[i].frequency_hz, b[i].frequency_hz) ||
		      !near(a[i].q, b[i].q) || !near(a[i].gain_db, b[i].gain_db))))
			return false;
	return true;
}

/* Every field the model carries (bypass sections compare by type only:
 * their parameters are unused). */
static bool dsp_models_equal(const AiusDspModel *a, const AiusDspModel *b)
{
	if (a->sample_rate_hz != b->sample_rate_hz ||
	    memcmp(&a->features, &b->features, sizeof(a->features)) != 0 ||
	    a->output_eq_count != b->output_eq_count ||
	    !dsp_eqs_equal(a->output_eq, b->output_eq, a->output_eq_count))
		return false;
	for (unsigned c = 0; c < AIUS_DSP_MODEL_INPUT_CHANNELS; c++) {
		const AiusDspModelInputChannel *x = &a->input_channels[c];
		const AiusDspModelInputChannel *y = &b->input_channels[c];
		if (!near(x->gain_db, y->gain_db) || x->eq_count != y->eq_count ||
		    !dsp_eqs_equal(x->eq, y->eq, x->eq_count))
			return false;
	}
	return near(a->matrix.center_gain_linear, b->matrix.center_gain_linear) &&
	       near(a->matrix.surround_gain_linear, b->matrix.surround_gain_linear) &&
	       near(a->matrix.lfe_gain_linear, b->matrix.lfe_gain_linear) &&
	       near(a->matrix.lfe_crossover_hz, b->matrix.lfe_crossover_hz) &&
	       near(a->matrix.headroom_linear, b->matrix.headroom_linear) &&
	       near(a->matrix.surround_delay_ms, b->matrix.surround_delay_ms) &&
	       a->matrix.clipfree == b->matrix.clipfree &&
	       a->matrix.layout_aware == b->matrix.layout_aware &&
	       a->limiter.enabled == b->limiter.enabled &&
	       near(a->limiter.threshold_dbfs, b->limiter.threshold_dbfs) &&
	       near(a->limiter.attack_ms, b->limiter.attack_ms) &&
	       near(a->limiter.release_ms, b->limiter.release_ms) &&
	       near(a->limiter.lookahead_ms, b->limiter.lookahead_ms);
}

/* NS6, dsp half: write_dsp_config's serialiser output parses with the real
 * PUT parser to AIUS_DSP_CONFIG_OK and the running model; GET serves the new
 * config_generation with that model. */
static void test_dsp_config_round_trip(void)
{
	static const char kPut[] =
		"{\"source\":\"sd\",\"config_generation\":41,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":false},"
		"\"input_channels\":{"
		"\"fl\":{\"gain_db\":-3.5,\"eq\":[{\"type\":\"peaking\",\"frequency_hz\":1000,"
		"\"q\":1.5,\"gain_db\":4}]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[{\"type\":\"highpass\",\"frequency_hz\":30,"
		"\"q\":0.7071067811865475,\"gain_db\":0},{\"type\":\"bypass\"}]},"
		"\"fc\":{\"gain_db\":12,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":-60,\"eq\":[]},"
		"\"sl\":{\"gain_db\":0.1,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"peaking\",\"frequency_hz\":8000,\"q\":2,"
		"\"gain_db\":-6}],"
		"\"matrix\":{\"center_gain_linear\":0.7071067811865475,"
		"\"surround_gain_linear\":0.5,\"lfe_gain_linear\":2,"
		"\"lfe_crossover_hz\":80,\"headroom_linear\":0.9,"
		"\"clipfree\":false,\"layout_aware\":false,\"surround_delay_ms\":3.25},"
		"\"limiter\":{\"enabled\":true,\"threshold_dbfs\":-3,\"attack_ms\":5,"
		"\"release_ms\":200,\"lookahead_ms\":2},"
		"\"base_generation\":41}";
	static uint8_t body[4096];
	AiusDspModel running;
	AiusDspModel parsed;
	AiusOpticalDspConfigHeader header;
	size_t length;

	assert(aius_dsp_config_parse_put((const uint8_t *)kPut, strlen(kPut),
	                                 &running) == AIUS_DSP_CONFIG_OK);
	memset(&header, 0, sizeof(header));
	header.source = "sd";
	header.config_generation = 42u; /* advanced by the Apply */
	header.staged_generation = 7u;
	/* The GET document serves the new generation and model, and has no
	 * base_generation: the load's parser refuses it, which is why the Q2
	 * re-put writes the file form. */
	length = aius_optical_dsp_config_write(&running, &header, body,
	                                       sizeof(body) - 1u);
	assert(length > 0u);
	body[length] = '\0';
	assert(strstr((const char *)body, "\"config_generation\":42,") != NULL);
	assert(strstr((const char *)body, "\"source\":\"sd\"") != NULL);
	assert(strstr((const char *)body, "base_generation") == NULL);
	/* The round-trip contract: no model_origin on this route. */
	assert(strstr((const char *)body, "model_origin") == NULL);
	assert(aius_dsp_config_parse_put(body, length, &parsed) ==
	       AIUS_DSP_CONFIG_ERR_MISSING_KEY);
	header.file_form = true;
	length = aius_optical_dsp_config_write(&running, &header, body,
	                                       sizeof(body) - 1u);
	assert(length > 0u);
	body[length] = '\0';
	{
		const AiusDspConfigErrorCode code =
			aius_dsp_config_parse_put(body, length, &parsed);
		if (code != AIUS_DSP_CONFIG_OK)
			fprintf(stderr, "NS6: parse_put %d (%s) over %s\n", (int)code,
			        aius_dsp_config_error_message(code), (const char *)body);
		assert(code == AIUS_DSP_CONFIG_OK);
	}
	assert(parsed.base_generation == 42u);
	assert(dsp_models_equal(&parsed, &running));
	/* A body one byte short of the document writes nothing. */
	assert(aius_optical_dsp_config_write(&running, &header, body, length) ==
	       0u);
	/* The default model's source. */
	header.source = "default";
	length = aius_optical_dsp_config_write(&running, &header, body,
	                                       sizeof(body) - 1u);
	body[length] = '\0';
	assert(strstr((const char *)body, "\"source\":\"default\"") != NULL);
}

/* Phase 4t: a config stored clipfree true with headroom 0.5 GETs 0.5. The
 * writer emits the stored model pad (not the resolved clip-free one). */
static void test_dsp_config_serves_stored_headroom_under_clipfree(void)
{
	AiusDspModel model;
	AiusOpticalDspConfigHeader header;
	static uint8_t body[4096];
	size_t length;

	aius_dsp_model_set_defaults(&model);
	model.matrix.clipfree = 1u;
	model.matrix.headroom_linear = 0.5;
	memset(&header, 0, sizeof(header));
	header.source = "sd";
	length = aius_optical_dsp_config_write(&model, &header, body,
	                                       sizeof(body) - 1u);
	assert(length > 0u);
	body[length] = '\0';
	assert(strstr((const char *)body, "\"headroom_linear\":0.5,") != NULL);
	assert(strstr((const char *)body, "\"clipfree\":true") != NULL);
}

/* Delivery audit: D-S1 (a write error is not a card-gone), D-S2 (a
 * backed-off entry does not steal the fill's passes), D-N3 (scratch names
 * are boot-independent). */
static void test_sd_flush_audit_fixes(void)
{
	AiusOpticalSdFlush flush;
	AiusOpticalSdFlushEnd end;
	uint32_t id;

	/* D-S1: the classification. */
	assert(!aius_optical_sd_card_gone(true, true, true));
	assert(!aius_optical_sd_card_gone(true, false, true));
	assert(aius_optical_sd_card_gone(true, false, false));
	assert(aius_optical_sd_card_gone(false, true, true));

	/* D-S1: a flush write fails with the mount dropped and the card still
	 * detected: FAILED (a counted S5 try, the overlay's backoff), not
	 * CARD_GONE: no hold, no flap backoff, and the 1 Hz probe has nothing to
	 * clear, so no once-a-second remount loop. */
	ff_reset(&flush);
	g_ff.due = true;
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	flush.mount_seen = true;
	flush.mounted_at_ms = 0u;
	end = ff_end_ok(0u);
	end.fault = true;
	end.mounted = false;
	end.card_detected = true;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 100u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_FAILED] == 1u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CARD_GONE] == 0u);
	assert(flush.remount_interval_ms == 0u && flush.mount_seen);
	/* The same fault with DAT3 reading no card is the card-gone fault. */
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 200u));
	end.card_detected = false;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 300u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CARD_GONE] == 1u);
	assert(flush.remount_interval_ms == 4000u); /* N5: 300 ms after a mount */

	/* D-S2: an entry backed off (S5) or CRC-faulted is pending (not
	 * drained) but not due: the flush start never cancels the fill, so every
	 * idle step runs the walk's next file until it completes. */
	ff_reset(&flush);
	g_ff.drained = false;
	g_ff.due = false;
	g_ff.fill_in_flight = true;
	for (uint32_t pass = 0u; pass < 50u; pass++)
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, pass * 10u,
		                                 false) ==
		       AIUS_OPTICAL_SD_IDLE_FILL);
	assert(g_ff.fill_cancels == 0u && g_ff.web.latches == 0u);
	assert(!aius_optical_web_fill_paused(&g_ff.web, 500u));
	/* Once the entry is due again, the flush takes the pass. */
	g_ff.due = true;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 600u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(g_ff.fill_cancels == 1u);

	/* D-N3: boot-independent (a pure function of version and target), and
	 * distinct across targets and across versions of one target. */
	id = aius_optical_sd_flush_scratch_id(0x12345678u, "/aius/www/a.html");
	assert(id == aius_optical_sd_flush_scratch_id(0x12345678u,
	                                              "/aius/www/a.html"));
	assert(id != aius_optical_sd_flush_scratch_id(0x12345678u,
	                                              "/aius/www/b.html"));
	assert(id != aius_optical_sd_flush_scratch_id(0x12345679u,
	                                              "/aius/www/a.html"));
	/* The old per-boot counter's first name (1) is not what a boot's first
	 * REPLACE of an arbitrary file produces. */
	assert(aius_optical_sd_flush_scratch_id(0u, "/aius/www/index.html") != 1u);
}

/* D-S5: the card-loss classification at its call sites. Each step runs the
 * lifecycle's sequence from optical_main.cpp: the per-pass observation
 * (aius_optical_sd_card_loss with the adapter's mounted and driver_faults),
 * the idle step, a remount probe's BEGIN, and app_sd_ready's
 * aius_optical_app_card_ops_ready(g_sd_ready, dat3_remuxed) for the direct
 * card ops (a /sd/ index, the replay listing, a card GET miss). */
typedef struct {
	bool mounted;
	uint32_t driver_faults;
	bool begin_ok;
} FakeAdapter;

static AiusOpticalSdCardLoss d5_observe(AiusOpticalSdFlush *flush,
                                        const FakeAdapter *adapter,
                                        uint32_t now_ms)
{
	g_ff.adapter_mounted = adapter->mounted;
	return aius_optical_sd_card_loss(flush, &g_ff_ops, true, adapter->mounted,
	                                 g_ff.file_open, adapter->driver_faults,
	                                 now_ms);
}

/* sd_flush_settle: the engine's terminal of a faulted flush job, with the
 * card-loss verdict taken at the settle (D-S7: PENDING with a file open). */
static void d5_settle(AiusOpticalSdFlush *flush, const FakeAdapter *adapter,
                      AiusOpticalSdFlushEnd *end, uint32_t now_ms)
{
	const AiusOpticalSdCardLoss loss = d5_observe(flush, adapter, now_ms);
	*end = ff_end_ok(0u);
	end->fault = true;
	end->mounted = adapter->mounted;
	end->loss_pending = loss == AIUS_OPTICAL_SD_CARD_PENDING;
	end->card_detected = loss != AIUS_OPTICAL_SD_CARD_GONE;
	aius_optical_sd_flush_settle(flush, &g_ff_ops, end, now_ms);
}

/* A probe step's BEGIN and its verdict (sd_step_complete). */
static void d5_probe(AiusOpticalSdFlush *flush, FakeAdapter *adapter,
                     uint32_t now_ms)
{
	const bool mounted = g_ff.card_in_slot && adapter->begin_ok;
	if (mounted) {
		adapter->mounted = true;
		g_ff.dat3_remuxed = false; /* a successful begin re-muxes to USDHC */
	} else if (g_ff.card_in_slot) {
		adapter->driver_faults++; /* a detected card whose BEGIN fails */
	}
	g_ff.adapter_mounted = adapter->mounted;
	aius_optical_sd_flush_note_probe(flush, &g_ff_ops, mounted, now_ms);
}

static bool d5_card_ops(void)
{
	return aius_optical_app_card_ops_ready(true, g_ff.dat3_remuxed);
}

static void test_sd_card_loss_call_sites(void)
{
	AiusOpticalSdFlush flush;
	AiusOpticalSdFlushEnd end;
	FakeAdapter adapter = { true, 0u, true };
	uint32_t t;

	/* POST /api/sd/mode: the mount drops with no fault. No re-mux, card ops
	 * answer as today, nothing remounts early (the next storage BEGIN does),
	 * during playback or not (E3). */
	ff_reset(&flush);
	g_ff.playback = true;
	assert(d5_observe(&flush, &adapter, 0u) == AIUS_OPTICAL_SD_CARD_KEPT);
	adapter.mounted = false;
	for (t = 1u; t < 5000u; t += 500u) {
		assert(d5_observe(&flush, &adapter, t) == AIUS_OPTICAL_SD_CARD_KEPT);
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t, false) ==
		       AIUS_OPTICAL_SD_IDLE_FILL);
	}
	assert(g_ff.remuxes == 0u && !g_ff.dat3_remuxed && d5_card_ops());
	assert(!flush.fault_remount);

	/* A write fault on a present card: one re-mux, the flush job fails as a
	 * counted S5 try (FAILED, no hold); the card ops answer sd_unavailable
	 * until the fault remount AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS later (D-S8),
	 * during playback too, then as today. */
	ff_reset(&flush);
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	adapter.begin_ok = true;
	g_ff.playback = true;
	g_ff.due = true;
	assert(d5_observe(&flush, &adapter, 0u) == AIUS_OPTICAL_SD_CARD_KEPT);
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	adapter.mounted = false;
	adapter.driver_faults++;
	d5_settle(&flush, &adapter, &end, 10u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_FAILED] == 1u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CARD_GONE] == 0u);
	assert(g_ff.remuxes == 1u && !d5_card_ops() && g_ff.card_lost == 0u);
	/* The fill's follow-card check in the same pass agrees (latched). */
	assert(d5_observe(&flush, &adapter, 10u) ==
	       AIUS_OPTICAL_SD_CARD_FAULT_PRESENT);
	assert(g_ff.remuxes == 1u);
	g_ff.due = false; /* the overlay's S5 backoff */
	/* D-S9: no BEGIN (a fill file's) ahead of the remount's schedule. */
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 11u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 2009u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 2010u, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT);
	d5_probe(&flush, &adapter, 2010u);
	assert(adapter.mounted && d5_card_ops() && !flush.fault_remount);
	assert(d5_observe(&flush, &adapter, 2011u) == AIUS_OPTICAL_SD_CARD_KEPT);

	/* D-S8: a present card with a bad sector, during playback: every mount
	 * restarts the walk and the walk faults on the same file at once. Over
	 * ten minutes the remounts are bounded: the delay doubles 2, 4, 8, 16,
	 * 32 s to the 60 s cap, a successful mount between faults resetting
	 * nothing. */
	{
		uint32_t remounts = 0u;
		uint32_t last_remount = 0u;
		uint32_t gaps[16];
		ff_reset(&flush);
		adapter.mounted = true;
		adapter.driver_faults = 0u;
		adapter.begin_ok = true;
		g_ff.playback = true;
		(void)d5_observe(&flush, &adapter, 0u);
		for (t = 100u; t <= 600000u; t += 100u) {
			if (adapter.mounted) { /* the walk's READ faults */
				adapter.mounted = false;
				adapter.driver_faults++;
				g_ff.dat3_remuxed = false;
			}
			(void)d5_observe(&flush, &adapter, t);
			if (aius_optical_sd_idle_step(&flush, &g_ff_ops, t, false) ==
			    AIUS_OPTICAL_SD_IDLE_REMOUNT) {
				if (remounts < 16u)
					gaps[remounts] = t - last_remount;
				last_remount = t;
				remounts++;
				d5_probe(&flush, &adapter, t);
			}
		}
		assert(remounts >= 6u && remounts <= 15u);
		assert(gaps[1] >= 4000u && gaps[2] >= 8000u && gaps[3] >= 16000u &&
		       gaps[4] >= 32000u && gaps[5] >= 60000u);
		assert(flush.fault_backoff == 6u);
		/* A quiet minute resets the schedule. */
		adapter.mounted = true;
		(void)d5_observe(&flush, &adapter, t);
		adapter.mounted = false;
		adapter.driver_faults++;
		g_ff.dat3_remuxed = false;
		t += AIUS_OPTICAL_SD_FAULT_QUIET_MS;
		(void)d5_observe(&flush, &adapter, t);
		assert(flush.fault_backoff == 1u);
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t + 2000u,
		                                 false) ==
		       AIUS_OPTICAL_SD_IDLE_REMOUNT);
	}

	/* D-S7: a card pulled mid-flush with the job's file still open is not
	 * classified (DAT3 is the bus's): no re-mux, no outcome, no S5 try, no
	 * remount. Once the file closes: card-gone, held, CARD_GONE, and no
	 * BEGIN while DAT3 reads absent. */
	ff_reset(&flush);
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	adapter.begin_ok = true;
	g_ff.playback = true;
	g_ff.due = true;
	g_ff.drained = false;
	assert(d5_observe(&flush, &adapter, 0u) == AIUS_OPTICAL_SD_CARD_KEPT);
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	g_ff.card_in_slot = false;
	g_ff.card_detected = false;
	adapter.mounted = false;
	adapter.driver_faults++;
	g_ff.file_open = true;
	d5_settle(&flush, &adapter, &end, 10u);
	assert(flush.running && g_ff.remuxes == 0u && !flush.fault_remount);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_FAILED] == 0u &&
	       g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CARD_GONE] == 0u);
	assert(d5_observe(&flush, &adapter, 20u) == AIUS_OPTICAL_SD_CARD_PENDING);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 30u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	g_ff.file_open = false;
	/* The lifecycle's per-pass observation (no file open) classifies. */
	assert(d5_observe(&flush, &adapter, 40u) == AIUS_OPTICAL_SD_CARD_GONE);
	assert(g_ff.remuxes == 1u && g_ff.card_lost == 1u && !d5_card_ops());
	g_ff.held = true; /* the overlay's note_card_lost */
	d5_settle(&flush, &adapter, &end, 50u);
	assert(!flush.running);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CARD_GONE] == 1u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_FAILED] == 0u);
	for (t = 60u; t < 20000u; t += 100u)
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t, false) !=
		       AIUS_OPTICAL_SD_IDLE_REMOUNT);

	/* D-N7: no carve (8 MiB): a fault drop is card-gone with no re-mux and
	 * no self-initiated remount during playback; card ops as today. */
	ff_reset(&flush);
	g_ff.present = false;
	g_ff.playback = true;
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	(void)d5_observe(&flush, &adapter, 0u);
	adapter.mounted = false;
	adapter.driver_faults++;
	assert(d5_observe(&flush, &adapter, 10u) == AIUS_OPTICAL_SD_CARD_GONE);
	assert(g_ff.remuxes == 0u && d5_card_ops() && !flush.fault_remount);
	for (t = 20u; t < 120000u; t += 1000u)
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t, false) !=
		       AIUS_OPTICAL_SD_IDLE_REMOUNT);

	/* D-S9 (a): a fill READ fault on a bad sector during playback with flush
	 * entries due: the flush does not BEGIN at once; it waits for the fault
	 * remount on D-S8's schedule, and starts after it. */
	ff_reset(&flush);
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	adapter.begin_ok = true;
	g_ff.playback = true;
	g_ff.due = true;
	g_ff.drained = false;
	(void)d5_observe(&flush, &adapter, 0u);
	adapter.mounted = false;
	adapter.driver_faults++;
	assert(d5_observe(&flush, &adapter, 10u) ==
	       AIUS_OPTICAL_SD_CARD_FAULT_PRESENT);
	for (t = 11u; t < 2010u; t += 100u) {
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t, false) ==
		       AIUS_OPTICAL_SD_IDLE_WAIT);
		assert(!flush.running && g_ff.starts == 0u);
	}
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 2010u, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT);
	d5_probe(&flush, &adapter, 2010u);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 2011u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(flush.running && g_ff.starts == 1u); /* on the new mount */

	/* D-S9 (b): after FAULT_PRESENT the card is pulled during playback: at
	 * the remount's time DAT3 reads absent, so the hold is set and the
	 * fault remount ends; no BEGIN of any kind (flush, fill, HTTP, replay:
	 * the one gate) starts while DAT3 reads absent. */
	ff_reset(&flush);
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	g_ff.playback = true;
	g_ff.due = true;
	g_ff.drained = false;
	(void)d5_observe(&flush, &adapter, 0u);
	adapter.mounted = false;
	adapter.driver_faults++;
	assert(d5_observe(&flush, &adapter, 10u) ==
	       AIUS_OPTICAL_SD_CARD_FAULT_PRESENT);
	g_ff.card_in_slot = false;
	g_ff.card_detected = false;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 2010u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(g_ff.card_lost >= 1u && !flush.fault_remount);
	assert(d5_observe(&flush, &adapter, 2011u) == AIUS_OPTICAL_SD_CARD_GONE);
	g_ff.held = true; /* the overlay's note_card_lost */
	unsigned no_card_gates = 0u;
	for (t = 2012u; t < 120000u; t += 250u) {
		assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, t, false) ==
		       AIUS_OPTICAL_SD_IDLE_WAIT);
		/* The held retry's schedule answers WAIT without reading DAT3 while
		 * it is pending; once due, DAT3 reads no card (B1). */
		const bool pending = flush.remount_armed &&
		                     (int32_t)(t - flush.remount_at_ms) < 0;
		const AiusOpticalSdBeginGate gate =
			aius_optical_sd_begin_blocked(&flush, &g_ff_ops, t);
		assert(gate == (pending ? AIUS_OPTICAL_SD_BEGIN_WAIT
		                        : AIUS_OPTICAL_SD_BEGIN_NO_CARD));
		no_card_gates += gate == AIUS_OPTICAL_SD_BEGIN_NO_CARD;
	}
	/* The lifecycle re-arms the schedule at every due pass, so the gate
	 * sees it pending above. Once the schedule is due and nothing re-arms
	 * it, DAT3 is read once and reads no card. */
	{
		const unsigned lost = g_ff.card_lost;
		assert(flush.remount_armed);
		assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops,
		                                     flush.remount_at_ms) ==
		       AIUS_OPTICAL_SD_BEGIN_NO_CARD);
		assert(g_ff.card_lost == lost + 1u);
		no_card_gates++;
	}
	assert(no_card_gates > 0u);
	assert(g_ff.starts == 0u && !flush.running);
	/* The card returns: DAT3 reads present, the held retry remounts. */
	g_ff.card_in_slot = true;
	g_ff.card_detected = true;
	{
		bool remounted = false;
		for (t = 120000u; t <= 122000u && !remounted; t += 50u)
			remounted = aius_optical_sd_idle_step(&flush, &g_ff_ops, t,
			                                      false) ==
			            AIUS_OPTICAL_SD_IDLE_REMOUNT;
		assert(remounted); /* within one AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS */
	}
	/* Off playback the gate never blocks (the 1 Hz probe owns detection),
	 * and a reused mount is never blocked. */
	g_ff.playback = false;
	g_ff.card_detected = false;
	assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops, 0u) ==
	       AIUS_OPTICAL_SD_BEGIN_NONE);
	g_ff.playback = true;
	g_ff.adapter_mounted = true;
	assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops, 0u) ==
	       AIUS_OPTICAL_SD_BEGIN_NONE);

	/* D-S10: a transient fault mid-replay (a refill's, a flush's or an HTTP
	 * job's) with the card present: the next refill's BEGIN is not held to
	 * the fault remount's schedule, so it remounts at once and the replay
	 * continues; every other BEGIN still waits. A pulled card stops it. */
	ff_reset(&flush);
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	adapter.begin_ok = true;
	g_ff.playback = true; /* the replay holds the gate open */
	(void)d5_observe(&flush, &adapter, 0u);
	adapter.mounted = false;
	adapter.driver_faults++;
	assert(d5_observe(&flush, &adapter, 10u) ==
	       AIUS_OPTICAL_SD_CARD_FAULT_PRESENT);
	for (t = 11u; t < 2010u; t += 100u) {
		assert(!aius_optical_sd_no_card_begin_blocked(&flush, &g_ff_ops));
		assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops, t) ==
		       AIUS_OPTICAL_SD_BEGIN_WAIT);
	}
	/* The refill's BEGIN remounts: the fault is over for everyone. */
	adapter.mounted = true;
	g_ff.dat3_remuxed = false;
	assert(d5_observe(&flush, &adapter, 20u) == AIUS_OPTICAL_SD_CARD_KEPT);
	assert(!flush.fault_remount &&
	       aius_optical_sd_begin_blocked(&flush, &g_ff_ops, 21u) ==
		       AIUS_OPTICAL_SD_BEGIN_NONE);
	/* Pulled mid-replay: after the fault DAT3 reads absent: the refill's
	 * BEGIN is blocked (the replay stops) and the hold is set. */
	adapter.mounted = false;
	adapter.driver_faults++;
	g_ff.card_in_slot = false;
	g_ff.card_detected = false;
	assert(d5_observe(&flush, &adapter, 30u) == AIUS_OPTICAL_SD_CARD_GONE);
	{
		const unsigned lost = g_ff.card_lost;
		assert(aius_optical_sd_no_card_begin_blocked(&flush, &g_ff_ops));
		assert(g_ff.card_lost == lost + 1u);
	}

	/* D-N9: the remount slot belongs to the one probe step the retry chose.
	 * While that step is deferred (STORE active, F2 FIFO defer) it alone
	 * passes the gate; any other BEGIN stays blocked to the schedule; and a
	 * dropped step frees the slot at the next idle step. */
	ff_reset(&flush);
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	g_ff.playback = true;
	(void)d5_observe(&flush, &adapter, 0u);
	adapter.mounted = false;
	adapter.driver_faults++;
	assert(d5_observe(&flush, &adapter, 10u) ==
	       AIUS_OPTICAL_SD_CARD_FAULT_PRESENT);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 2010u, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT);
	assert(flush.remount_running);
	for (t = 2011u; t < 3000u; t += 100u) { /* the step deferred */
		assert(!aius_optical_sd_step_begin_blocked(&flush, &g_ff_ops, t));
		assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops, t) ==
		       AIUS_OPTICAL_SD_BEGIN_WAIT);
	}
	/* Dropped (the step never ran): the next idle step frees the slot,
	 * and a later step start is gated like any BEGIN. */
	(void)aius_optical_sd_idle_step(&flush, &g_ff_ops, 3000u, false);
	assert(!flush.remount_running);
	assert(aius_optical_sd_step_begin_blocked(&flush, &g_ff_ops, 3001u));

	/* A lifecycle with the card not ready is card-gone whatever the mount;
	 * a direct card op's fault counts like the adapter's. */
	assert(aius_optical_sd_card_loss(&flush, &g_ff_ops, false, true, false,
	                                 0u, 0u) == AIUS_OPTICAL_SD_CARD_GONE);
	ff_reset(&flush);
	adapter.mounted = true;
	adapter.driver_faults = 0u;
	assert(d5_observe(&flush, &adapter, 0u) == AIUS_OPTICAL_SD_CARD_KEPT);
	adapter.mounted = false;
	aius_optical_sd_note_direct_fault(&flush);
	assert(d5_observe(&flush, &adapter, 1u) ==
	       AIUS_OPTICAL_SD_CARD_FAULT_PRESENT);
}

/* CR-B1: the seam's millis windows past 2^31 ms of uptime and across the
 * 2^32 wrap (as test_web_fill_hold_long_lapse does for the fill hold). */
static void test_sd_flush_long_lapse(void)
{
	AiusOpticalSdFlush flush;
	const uint32_t late = 0x80000100u;

	/* The remount schedule, never set (remount_at_ms 0): a card reseated
	 * during playback past 2^31 ms of uptime retries at once and passes the
	 * BEGIN gate. */
	ff_reset(&flush);
	g_ff.held = true;
	g_ff.drained = false;
	g_ff.playback = true;
	g_ff.adapter_mounted = false;
	assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops, late) ==
	       AIUS_OPTICAL_SD_BEGIN_NONE);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, late, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT);
	/* A schedule set across the wrap waits until due, then is due. */
	ff_reset(&flush);
	g_ff.held = true;
	g_ff.drained = false;
	g_ff.playback = true;
	g_ff.adapter_mounted = false;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 0xFFFFFC00u, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT); /* due 0xFFFFFC00 + 2000 = 976 */
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, false, 0xFFFFFC00u);
	assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops, 975u) ==
	       AIUS_OPTICAL_SD_BEGIN_WAIT);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 975u, false) ==
	       AIUS_OPTICAL_SD_IDLE_WAIT);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 976u, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT);
	/* A schedule observed due once stays lapsed however old it gets. */
	ff_reset(&flush);
	g_ff.held = true;
	g_ff.drained = false;
	g_ff.playback = true;
	g_ff.adapter_mounted = false;
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 1000u, false) ==
	       AIUS_OPTICAL_SD_IDLE_REMOUNT); /* due 3000 */
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, false, 1000u);
	aius_optical_sd_flush_lapse(&flush, 3000u); /* the lifecycle's pass */
	assert(!flush.remount_armed);
	assert(aius_optical_sd_begin_blocked(&flush, &g_ff_ops,
	                                     3000u + 0x80000100u) ==
	       AIUS_OPTICAL_SD_BEGIN_NONE);
	assert(aius_optical_sd_idle_step(&flush, &g_ff_ops, 3000u + 0x80000100u,
	                                 false) == AIUS_OPTICAL_SD_IDLE_REMOUNT);

	/* The N5 flap window near 2^31 and across the wrap: a gone card 3 s
	 * after a held-clearing mount backs off; one 2^32 ms + 3 s later (an
	 * aliased stamp) does not, the window having lapsed. */
	ff_reset(&flush);
	g_ff.held = true;
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, true, late);
	aius_optical_sd_flush_note_card_gone(&flush, late + 3000u);
	assert(flush.remount_interval_ms == 4000u);
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, true, 0xFFFFF000u);
	aius_optical_sd_flush_note_card_gone(&flush, 0xFFFFF000u + 3000u);
	assert(flush.remount_interval_ms == 8000u); /* across the wrap */
	aius_optical_sd_flush_note_probe(&flush, &g_ff_ops, true, 100u);
	aius_optical_sd_flush_lapse(&flush, 100u + 5000u);
	assert(!flush.mount_seen);
	aius_optical_sd_flush_note_card_gone(&flush, 100u + 3000u); /* aliased */
	assert(flush.remount_interval_ms == 8000u);

	/* The D-S8 quiet window across the wrap: a fault right after the remount
	 * doubles; after a lapsed quiet minute an aliased stamp resets. */
	{
		FakeAdapter adapter = { true, 0u, true };
		ff_reset(&flush);
		g_ff.playback = true;
		(void)d5_observe(&flush, &adapter, 0xFFFFFF00u);
		adapter.mounted = false;
		adapter.driver_faults++;
		g_ff.dat3_remuxed = false;
		(void)d5_observe(&flush, &adapter, 0xFFFFFF00u);
		assert(flush.fault_backoff == 1u);
		d5_probe(&flush, &adapter, 0xFFFFFF00u + 2000u); /* remount: 1744 */
		adapter.mounted = false;
		adapter.driver_faults++;
		g_ff.dat3_remuxed = false;
		(void)d5_observe(&flush, &adapter, 1800u);
		assert(flush.fault_backoff == 2u);
		d5_probe(&flush, &adapter, 1800u + 4000u);
		(void)d5_observe(&flush, &adapter, 5800u + 60000u); /* quiet */
		assert(flush.fault_backoff == 0u);
		adapter.mounted = false;
		adapter.driver_faults++;
		g_ff.dat3_remuxed = false;
		(void)d5_observe(&flush, &adapter, 5800u + 100u); /* aliased, 2^32 */
		assert(flush.fault_backoff == 1u);
	}

	/* The H9 freeze ceiling near 2^31 and across the wrap. */
	ff_reset(&flush);
	g_ff.settled = false;
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, late) ==
	       AIUS_OPTICAL_SD_GATE_WAIT);
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, late + 1999u) ==
	       AIUS_OPTICAL_SD_GATE_WAIT);
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, late + 2000u) ==
	       AIUS_OPTICAL_SD_GATE_BUSY);
	aius_optical_sd_freeze_release(&flush, &g_ff_ops);
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 0xFFFFFC00u) ==
	       AIUS_OPTICAL_SD_GATE_WAIT);
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 975u) ==
	       AIUS_OPTICAL_SD_GATE_WAIT);
	assert(aius_optical_sd_write_gate(&flush, &g_ff_ops, 976u) ==
	       AIUS_OPTICAL_SD_GATE_BUSY);
}

/* CR2-S1: a root-input CRC fault in the target pass after the backup pass
 * closed: the overlay still hears the backup pass closed (passes_closed 1),
 * before the job end, whatever the terminal's result; so the safe copy is the
 * backup and the overlay's next pick writes target first (its own suite). */
static void test_sd_flush_pass_notes_on_fault(void)
{
	AiusOpticalSdFlush flush;
	AiusOpticalSdFlushEnd end;

	ff_reset(&flush);
	g_ff.due = true;
	g_ff.job.copy_before_write = true; /* backup first (not target_first) */
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 0u));
	memset(&end, 0, sizeof(end));
	end.result = false;       /* the terminal's result cleared or replaced */
	end.fault = true;
	end.input_faulted = true; /* the target pass's chunk CRC */
	end.mounted = true;
	end.card_detected = true;
	end.passes_closed = 1u;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 10u);
	assert(g_ff.pass_count == 1u && g_ff.passes[0] == 1u); /* backup */
	assert(g_ff.readback_ok == 0u && g_ff.readback_bad == 0u);
	assert(g_ff.job_ends[AIUS_OPTICAL_SD_FLUSH_CHUNK_CRC] == 1u);
	/* No pass closed: no note. */
	assert(aius_optical_sd_flush_start(&flush, &g_ff_ops, 20u));
	end.passes_closed = 0u;
	aius_optical_sd_flush_settle(&flush, &g_ff_ops, &end, 30u);
	assert(g_ff.pass_count == 1u);
}

/* ------------------------------------------------------------------ */
/* Phase 4y: boot the other slot                                       */
/* ------------------------------------------------------------------ */

/* The dispatcher half: auth (test_ota_routes_are_authenticated) and the
 * FR-S8 gate (test_sd_overlay_reads_and_drain) are pinned elsewhere; here the
 * accepted body, that dispatch reaches the one flag-setting callback and
 * nothing else, and every refusal through the verdict map. */
static void test_boot_other_dispatch(void)
{
	Fixture fixture;
	static const char *const calls[] = {
		"ota_present", "auth_matches", "ota_boot_other"
	};
	static const struct {
		AiusOpticalAppOtaResult result;
		unsigned status;
		const char *body;
	} refusals[] = {
		{ AIUS_OPTICAL_APP_OTA_UNSUPPORTED, 400u,
		  "{\"ok\":false,\"error\":\"ota_unsupported\"}" },
		{ AIUS_OPTICAL_APP_OTA_STATE_REFUSED, 409u,
		  "{\"ok\":false,\"error\":\"ota_state\"}" },
		{ AIUS_OPTICAL_APP_OTA_IVT, 409u,
		  "{\"ok\":false,\"error\":\"ota_ivt\"}" },
	};
	fixture_init(&fixture);

	expect_immediate(dispatch_ota(&fixture,
	                              AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER,
	                              OTA_AUTH, NULL, -1),
	                 200, BOOT_OTHER_ACCEPTED);
	expect_log(&fixture, calls, ARRAY_LENGTH(calls));
	assert(fixture.fake.reboots == 0u); /* not the plain reboot callback */
	for (size_t i = 0; i < ARRAY_LENGTH(refusals); ++i) {
		fixture.fake.ota_boot_other_result = refusals[i].result;
		expect_immediate(dispatch_ota(&fixture,
		                              AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER,
		                              OTA_AUTH, NULL, -1),
		                 refusals[i].status, refusals[i].body);
		expect_log(&fixture, calls, ARRAY_LENGTH(calls));
	}
}

/* The composition's IVT read, counted: running slot B must never call it. */
static unsigned g_ivt_reads;
static bool g_ivt_sane;

static bool fake_other_ivt_sane(void *context)
{
	(void)context;
	g_ivt_reads++;
	return g_ivt_sane;
}

/* A healthy board on the slot-agnostic chooser, running slot A. */
static AiusOpticalBootOther boot_other_ok(void)
{
	AiusOpticalBootOther state;
	memset(&state, 0, sizeof(state));
	state.ab_built = true;
	state.ab_enabled = true;
	state.record_valid = true;
	state.active_slot = AIUS_OPTICAL_AB_SLOT_A;
	state.pending_slot = AIUS_OPTICAL_AB_SLOT_NONE;
	state.running_slot = AIUS_OPTICAL_AB_SLOT_A;
	state.booted_slot = AIUS_OPTICAL_AB_SLOT_A;
	state.ota_state = AIUS_OPTICAL_APP_OTA_STATE_IDLE;
	return state;
}

static void test_boot_other_admission(void)
{
	AiusOpticalBootOther state;

	/* Success, running A: slot B's IVT is read exactly once. */
	g_ivt_reads = 0u;
	g_ivt_sane = true;
	state = boot_other_ok();
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_OK);
	assert(g_ivt_reads == 1u);
	g_ivt_sane = false;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_IVT);

	/* Running B: no check at all, even with an insane answer waiting. */
	g_ivt_reads = 0u;
	state.active_slot = AIUS_OPTICAL_AB_SLOT_B;
	state.running_slot = AIUS_OPTICAL_AB_SLOT_B;
	state.booted_slot = AIUS_OPTICAL_AB_SLOT_B;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_OK);
	assert(g_ivt_reads == 0u);
	g_ivt_sane = true;

	/* The single-image build, and A/B whose ledger contract failed. */
	state = boot_other_ok();
	state.ab_built = false;
	assert(aius_optical_boot_other_check(&state, NULL, NULL) ==
	       AIUS_OPTICAL_APP_OTA_UNSUPPORTED);
	state = boot_other_ok();
	state.ab_enabled = false;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_UNSUPPORTED);
	assert(aius_optical_boot_other_check(NULL, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_UNSUPPORTED);

	/* Each record / ledger predicate refuses on its own. */
	g_ivt_reads = 0u;
	state = boot_other_ok();
	state.record_valid = false;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);
	state = boot_other_ok();
	state.pending_slot = AIUS_OPTICAL_AB_SLOT_A; /* a trial still open */
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);
	state = boot_other_ok();
	state.active_slot = AIUS_OPTICAL_AB_SLOT_B; /* active is not running */
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);
	state = boot_other_ok();
	state.booted_slot = AIUS_OPTICAL_AB_SLOT_NONE; /* old chooser's ledger */
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);

	/* Every pending deferred act refuses it. */
	state = boot_other_ok();
	state.pending.reboot = true;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);
	state = boot_other_ok();
	state.pending.activation = true;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);
	state = boot_other_ok();
	state.pending.rehearsal = true;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);
	state = boot_other_ok();
	state.pending.boot_other = true;
	assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane, NULL) ==
	       AIUS_OPTICAL_APP_OTA_STATE_REFUSED);

	/* An OTA session OFFERED through ACTIVATION_REQUESTED refuses; IDLE
	 * and FAILED do not. */
	for (unsigned s = AIUS_OPTICAL_APP_OTA_STATE_IDLE;
	     s <= AIUS_OPTICAL_APP_OTA_STATE_FAILED; ++s) {
		const bool busy = s >= AIUS_OPTICAL_APP_OTA_STATE_OFFERED &&
		                  s <= AIUS_OPTICAL_APP_OTA_STATE_ACTIVATION_REQUESTED;
		state = boot_other_ok();
		state.ota_state = (AiusOpticalAppOtaState)s;
		assert(aius_optical_boot_other_check(&state, fake_other_ivt_sane,
		                                     NULL) ==
		       (busy ? AIUS_OPTICAL_APP_OTA_STATE_REFUSED
		             : AIUS_OPTICAL_APP_OTA_OK));
	}
	/* No refusal above reached the IVT read except the two successes. */
	assert(g_ivt_reads == 2u);
}

/* The exclusion in both directions. */
static void test_boot_other_exclusion(void)
{
	AiusOpticalPendingActs pending;

	memset(&pending, 0, sizeof(pending));
	assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_ACTIVATE,
	                                        &pending));
	assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_REHEARSE,
	                                        &pending));
	assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_SELF_TEST,
	                                        &pending));
	assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_BOOT_OTHER,
	                                        &pending));

	/* A pending switch refuses activate, rehearse and self-test. */
	pending.boot_other = true;
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_ACTIVATE,
	                                       &pending));
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_REHEARSE,
	                                       &pending));
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_SELF_TEST,
	                                       &pending));
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_BOOT_OTHER,
	                                       &pending));

	/* A pending activation or rehearsal refuses the switch (and rehearse
	 * keeps its own two flags); a plain reboot refuses only the switch. */
	memset(&pending, 0, sizeof(pending));
	pending.activation = true;
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_BOOT_OTHER,
	                                       &pending));
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_REHEARSE,
	                                       &pending));
	memset(&pending, 0, sizeof(pending));
	pending.rehearsal = true;
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_BOOT_OTHER,
	                                       &pending));
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_REHEARSE,
	                                       &pending));
	memset(&pending, 0, sizeof(pending));
	pending.reboot = true;
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_BOOT_OTHER,
	                                       &pending));
	assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_ACTIVATE,
	                                        &pending));
	assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_REHEARSE,
	                                        &pending));
	assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_SELF_TEST,
	                                        &pending));
	assert(aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_BOOT_OTHER,
	                                       NULL));
}

/* The deferred reboot service's record write, recorded. */
static struct {
	char order[8];
	unsigned count;
	bool write_ok;
	bool frozen_at_write;
} g_reboot_acts;

static bool fake_switch_write(void *context)
{
	AiusOpticalSdFlush *flush = (AiusOpticalSdFlush *)context;
	g_reboot_acts.order[g_reboot_acts.count++] = 'w';
	g_reboot_acts.frozen_at_write = flush->frozen;
	return g_reboot_acts.write_ok;
}

/* One drained epoch of service_reboot_request (optical_main.cpp), composed
 * exactly as the firmware composes it: the SD gate (reset past the freeze
 * ceiling), WAIT returning early, then aius_optical_reboot_finish over gate
 * RUN && the final drain query. RESET stands for tx_gate_actuate(false) +
 * reset_application(). */
#define REBOOT_EPOCH_WAIT (-1)

static int reboot_epoch(AiusOpticalSdFlush *flush,
                        const AiusOpticalRebootActs *acts, bool *reboot,
                        bool *switch_requested, uint32_t now_ms)
{
	const AiusOpticalSdService gate = aius_optical_sd_service_gate(
		flush, &g_ff_ops, reboot, now_ms, true);
	if (gate == AIUS_OPTICAL_SD_SERVICE_WAIT)
		return REBOOT_EPOCH_WAIT;
	return (int)aius_optical_reboot_finish(
		flush, &g_ff_ops, acts, reboot, switch_requested,
		gate == AIUS_OPTICAL_SD_SERVICE_RUN &&
			aius_optical_sd_service_final(flush, &g_ff_ops, reboot));
}

static void test_boot_other_reboot_service(void)
{
	AiusOpticalSdFlush flush;
	AiusOpticalRebootActs acts;
	bool reboot;
	bool switch_requested;

	acts.context = &flush;
	acts.switch_write = fake_switch_write;

	/* A switch: the record write runs after the SD final passed (the
	 * freeze is held), and RESET -- the caller's cue for tx_gate_actuate
	 * (false) + reset_application() -- comes only after it landed. */
	ff_reset(&flush);
	memset(&g_reboot_acts, 0, sizeof(g_reboot_acts));
	g_reboot_acts.write_ok = true;
	reboot = switch_requested = true;
	assert(reboot_epoch(&flush, &acts, &reboot, &switch_requested,0u) ==
	       AIUS_OPTICAL_REBOOT_RESET);
	assert(g_reboot_acts.count == 1u && g_reboot_acts.order[0] == 'w');
	assert(g_reboot_acts.frozen_at_write);

	/* A plain reboot never writes the record. */
	ff_reset(&flush);
	memset(&g_reboot_acts, 0, sizeof(g_reboot_acts));
	reboot = true;
	switch_requested = false;
	assert(reboot_epoch(&flush, &acts, &reboot, &switch_requested,0u) ==
	       AIUS_OPTICAL_REBOOT_RESET);
	assert(g_reboot_acts.count == 0u);

	/* The freeze wait: nothing written, nothing cleared. */
	ff_reset(&flush);
	memset(&g_reboot_acts, 0, sizeof(g_reboot_acts));
	g_ff.settled = false;
	reboot = switch_requested = true;
	assert(reboot_epoch(&flush, &acts, &reboot, &switch_requested,0u) ==
	       REBOOT_EPOCH_WAIT);
	assert(g_reboot_acts.count == 0u && reboot && switch_requested);

	/* Non-reset exit 1, the SD gate abandons (unflushed write): the record
	 * is untouched and BOTH flags clear, so the exclusion cannot latch. */
	ff_reset(&flush);
	memset(&g_reboot_acts, 0, sizeof(g_reboot_acts));
	g_ff.drained = false;
	reboot = switch_requested = true;
	assert(reboot_epoch(&flush, &acts, &reboot, &switch_requested,0u) ==
	       AIUS_OPTICAL_REBOOT_ABANDONED);
	assert(g_reboot_acts.count == 0u);
	assert(!reboot && !switch_requested && !flush.frozen);

	/* The same through the final drain query (a PUT accepted mid-wait). */
	ff_reset(&flush);
	memset(&g_reboot_acts, 0, sizeof(g_reboot_acts));
	g_ff.settled = false;
	reboot = switch_requested = true;
	assert(reboot_epoch(&flush, &acts, &reboot, &switch_requested,0u) ==
	       REBOOT_EPOCH_WAIT);
	g_ff.settled = true;
	g_ff.drained = false;
	assert(reboot_epoch(&flush, &acts, &reboot, &switch_requested,1u) ==
	       AIUS_OPTICAL_REBOOT_ABANDONED);
	assert(g_reboot_acts.count == 0u);
	assert(!reboot && !switch_requested && !flush.frozen);

	/* Non-reset exit 2, the record write fails: no reset, unfrozen, both
	 * flags cleared; the caller surfaces the fault. */
	ff_reset(&flush);
	memset(&g_reboot_acts, 0, sizeof(g_reboot_acts));
	g_reboot_acts.write_ok = false;
	reboot = switch_requested = true;
	assert(reboot_epoch(&flush, &acts, &reboot, &switch_requested,0u) ==
	       AIUS_OPTICAL_REBOOT_SWITCH_FAILED);
	assert(g_reboot_acts.count == 1u && g_reboot_acts.order[0] == 'w');
	assert(!reboot && !switch_requested && !flush.frozen);
	/* Cleared, so a later activate is admitted again. */
	{
		AiusOpticalPendingActs pending;
		memset(&pending, 0, sizeof(pending));
		pending.reboot = reboot;
		pending.boot_other = switch_requested;
		assert(!aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_ACTIVATE,
		                                        &pending));
	}
}

int main(void)
{
	RUN_TEST(test_boot_other_dispatch);
	RUN_TEST(test_boot_other_admission);
	RUN_TEST(test_boot_other_exclusion);
	RUN_TEST(test_boot_other_reboot_service);
	RUN_TEST(test_sd_flush_pass_notes_on_fault);
	RUN_TEST(test_sd_flush_long_lapse);
	RUN_TEST(test_sd_card_loss_call_sites);
	RUN_TEST(test_sd_flush_audit_fixes);
	RUN_TEST(test_dsp_config_round_trip);
	RUN_TEST(test_dsp_config_serves_stored_headroom_under_clipfree);
	RUN_TEST(test_card_ops_and_reput);
	RUN_TEST(test_sd_service_gate);
	RUN_TEST(test_sd_scan_flush_and_freeze);
	RUN_TEST(test_sd_scan_card_route_busy);
	RUN_TEST(test_sd_flush_preempt_scan);
	RUN_TEST(test_sd_scan_reboot_reaches_flush);
	RUN_TEST(test_sd_flush_put_stops_fill);
	RUN_TEST(test_sd_flush_start_and_settle);
	RUN_TEST(test_sd_flush_remount_retry);
	RUN_TEST(test_sd_flush_write_gate);
	RUN_TEST(test_settings_mount_restore);
	RUN_TEST(test_sd_overlay_seam_install);
	RUN_TEST(test_sd_overlay_writes);
	RUN_TEST(test_sd_overlay_reads_and_drain);
	RUN_TEST(test_web_cache_seam_install);
	RUN_TEST(test_web_cache_hit_miss_304);
	RUN_TEST(test_web_cache_gzip_negotiation);
	RUN_TEST(test_web_cache_crc_faults);
	RUN_TEST(test_web_cache_entry_fault_refills);
	RUN_TEST(test_web_cache_presets_listing);
	RUN_TEST(test_status_brief_and_dispatch_hooks);
	RUN_TEST(test_admission_classes);
	RUN_TEST(test_admission_outlier_window);
	RUN_TEST(test_admission_sd_start);
	RUN_TEST(test_admission_time_ageing);
	RUN_TEST(test_web_fill_hold);
	RUN_TEST(test_web_fill_route);
	RUN_TEST(test_web_fill_follow_card);
	RUN_TEST(test_web_fill_hold_long_lapse);
	RUN_TEST(test_admission_first_measurement_outlier);
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
	RUN_TEST(test_dsp_config_put);
	RUN_TEST(test_config_public_put);
	RUN_TEST(test_config_put_body_bounds);
	RUN_TEST(test_measure_body_whitespace);
	RUN_TEST(test_measure_control);
	RUN_TEST(test_callback_isolation);
	RUN_TEST(test_dispatch_counts);
	RUN_TEST(test_usb_commands);
	RUN_TEST(test_unreachable_method_defaults);
	RUN_TEST(test_recorder_routes);
	RUN_TEST(test_recorder_slot_addressing_and_lease);
	RUN_TEST(test_recorder_lease_double_acquire_releases);
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
