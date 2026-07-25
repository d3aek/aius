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

static const char *const AUTH_TOKEN = "token";

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
	app.replay_start = fake_replay_start;
	app.replay_delete = fake_replay_delete;
	app.replay_stop = fake_replay_stop;
	app.set_test_running = fake_set_test_running;
	app.clear_counters = fake_clear_counters;
	app.request_reboot = fake_request_reboot;
	app.write_usb_payload = fake_write_usb_payload;
	app.upload_id = fake_upload_id;
	return app;
}

/* ------------------------------------------------------------------ */
/* fixture                                                              */
/* ------------------------------------------------------------------ */

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
	fixture->fake.upload_id = 0x1234abcdu;
	snprintf(fixture->fake.usb_payload, sizeof(fixture->fake.usb_payload),
	         "protocol=1 revision=test");
	fixture->fake.usb_payload_length = strlen(fixture->fake.usb_payload);
	app = fake_app(&fixture->fake);
	assert(aius_optical_app_init(&fixture->dispatcher, &app, fixture->body,
	                             sizeof(fixture->body)));
}

static void fixture_reset_log(Fixture *fixture)
{
	fixture->fake.log_count = 0;
}

/* Builds one request and runs it through the real parser before dispatch, so
 * every golden is anchored to bytes a client could actually send. */
static const AiusHttpApplicationResponse *dispatch_request(
	Fixture *fixture, const char *start_line, const char *headers,
	const char *body, long declared_length)
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
	for (size_t i = 0; i < 20u; ++i) {
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
			(void **)&holed.upload_id
		};
		assert(ARRAY_LENGTH(slots) == 20u);
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
	expect_immediate(dispatch_get(&fixture, "GET /api/dsp/config"), 200,
	                 "{\"body\":\"dsp_config\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/sd"), 200,
	                 "{\"ready\":true}");
	expect_immediate(dispatch_get(&fixture, "GET /api/replays/status"), 200,
	                 "{\"body\":\"replay_status\"}");
	expect_immediate(dispatch_get(&fixture, "GET /api/replays"), 200,
	                 "{\"body\":\"replay_list\"}");

	expect_immediate(dispatch_request(&fixture, "POST /api/start", NULL, NULL, -1),
	                 200, "{\"ok\":true,\"running\":true}");
	assert(fixture.fake.test_running);
	expect_immediate(dispatch_request(&fixture, "POST /api/stop", NULL, NULL, -1),
	                 200, "{\"ok\":true,\"running\":false}");
	assert(!fixture.fake.test_running);
	expect_immediate(dispatch_request(&fixture, "POST /api/clear", NULL, NULL, -1),
	                 200, "{\"ok\":true}");
	assert(fixture.fake.clears == 1u);
	expect_immediate(dispatch_request(&fixture, "POST /api/reboot", NULL, NULL, -1),
	                 200, "{\"ok\":true,\"rebooting\":true}");
	assert(fixture.fake.reboots == 1u);
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
	static const char *const rejected[] = {
		"GET /sd/x",
		"GET /sd/www/",
		"GET /sd/etc/passwd",
		"GET /sd/www/../secret",
		"GET /sd/www/a//b",
		"GET /sd/www/%2e%2e/x",
		"GET /sd/www/sub/../../x"
	};
	static const struct { const char *line; const char *type; } accepted[] = {
		{ "GET /sd/www/index.html", "text/html; charset=utf-8" },
		{ "GET /sd/www/app.js", "application/javascript" },
		{ "GET /sd/www/app.css", "text/css" },
		{ "GET /sd/www/model.json", "application/json" },
		{ "GET /sd/www/logo.png", "application/octet-stream" },
		{ "GET /sd/www/a/b/c/d.bin", "application/octet-stream" }
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

/* One dispatch touches exactly the callbacks its decision needs -- no probing,
 * no speculative serialization, and nothing after a refusal. */
static void test_callback_isolation(void)
{
	Fixture fixture;
	static const char *const status_calls[] = { "status" };
	static const char *const start_calls[] = { "test_running:true" };
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

	(void)dispatch_request(&fixture, "POST /api/start", NULL, NULL, -1);
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
static void test_route_coverage(void)
{
	Fixture fixture;
	static const struct { const char *line; const char *headers;
	                      const char *body; long declared; } requests[] = {
		{ "GET /", NULL, NULL, -1 },
		{ "GET /api/status", NULL, NULL, -1 },
		{ "GET /api/events", NULL, NULL, -1 },
		{ "POST /api/start", NULL, NULL, -1 },
		{ "POST /api/stop", NULL, NULL, -1 },
		{ "POST /api/clear", NULL, NULL, -1 },
		{ "POST /api/reboot", NULL, NULL, -1 },
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
		{ "GET /sd/replays/take1", NULL, NULL, -1 }
	};
	bool seen[AIUS_HTTP_ROUTE_SD_REPLAY + 1u];
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
		assert(fixture.request.route <= AIUS_HTTP_ROUTE_SD_REPLAY);
		seen[fixture.request.route] = true;
	}
	/* AIUS_HTTP_ROUTE_UNKNOWN never survives the parser, so the inventory is
	 * every other enumerator. */
	for (unsigned route = AIUS_HTTP_ROUTE_HOME;
	     route <= AIUS_HTTP_ROUTE_SD_REPLAY; ++route) {
		if (!seen[route])
			fprintf(stderr, "route %u has no dispatch coverage\n", route);
		assert(seen[route]);
	}
}

int main(void)
{
	RUN_TEST(test_init_validation);
	RUN_TEST(test_golden_immediate_routes);
	RUN_TEST(test_serializer_exhaustion);
	RUN_TEST(test_sd_media_and_paths);
	RUN_TEST(test_sd_delete);
	RUN_TEST(test_sd_upload);
	RUN_TEST(test_replay_chunk_upload);
	RUN_TEST(test_dsp_config_put);
	RUN_TEST(test_config_public_put);
	RUN_TEST(test_config_put_body_bounds);
	RUN_TEST(test_replay_control);
	RUN_TEST(test_callback_isolation);
	RUN_TEST(test_dispatch_counts);
	RUN_TEST(test_usb_commands);
	RUN_TEST(test_unreachable_method_defaults);
	RUN_TEST(test_route_coverage);
	fprintf(stderr, "optical-app oracle: all tests passed\n");
	return 0;
}
