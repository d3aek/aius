/* Optical application dispatch -- see aius_optical_app.h.
 *
 * Every definition in this file is FLASHMEM. The attribute must sit on the
 * definition: GCC does not inherit a `section` attribute from a forward
 * declaration, so a prototype-only FLASHMEM silently leaves the code in ITCM,
 * and the teensy41-optical image has ~1.3 kB of ITCM left.
 */

#include "aius_optical_app.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <stdio.h>
#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

#define AIUS_OPTICAL_APP_SD_ROOT "/aius/"
#define AIUS_OPTICAL_APP_REPLAY_ROOT "/aius/replays/"

static const char *const kContentJson = "application/json";
static const char *const kContentHtml = "text/html; charset=utf-8";
static const char *const kContentBinary = "application/octet-stream";

/* ------------------------------------------------------------------ */
/* response construction                                               */
/* ------------------------------------------------------------------ */

static FLASHMEM const AiusHttpApplicationResponse *http_immediate(
	AiusOpticalAppDispatcher *dispatcher, unsigned status,
	const char *content_type, const uint8_t *body, size_t body_length)
{
	AiusHttpApplicationResponse *response = &dispatcher->http_response;
	memset(response, 0, sizeof(*response));
	response->kind = AIUS_HTTP_APP_IMMEDIATE;
	response->status = status;
	response->content_type = content_type;
	response->body = body_length != 0u ? body : NULL;
	response->body_length = body_length;
	return response;
}

/* String literals have static storage duration, so the borrowed body outlives
 * the call exactly as the transaction requires. */
static FLASHMEM const AiusHttpApplicationResponse *http_json(
	AiusOpticalAppDispatcher *dispatcher, unsigned status, const char *text)
{
	return http_immediate(dispatcher, status, kContentJson,
	                      (const uint8_t *)text, strlen(text));
}

/* Every JSON failure body is one envelope around a short token, so only the
 * tokens are stored. The 23 bytes of `{"ok":false,"error":"` + `"}` would
 * otherwise repeat once per distinct error, and that costs RAM rather than
 * FLASH on this target: imxrt1062_t41.ld maps *(.flashmem*) to FLASH but
 * *(.rodata*) to DTCM, the same region the runtime stack grows down through.
 * FLASHMEM moves the code; only deduplicating the literals moves the strings.
 *
 * The body is formatted into the caller-supplied response buffer, which is
 * present on every path that can reach here: aius_optical_app_init() refuses a
 * null buffer or one under AIUS_OPTICAL_APP_MIN_BODY, and both dispatch entry
 * points refuse an invalid dispatcher before any route -- including the
 * earliest refusals and the response-capacity-exhaustion path -- can run. The
 * caller copies the body out during the dispatch call (aius_http_transaction.c
 * format_response), so overwriting a serializer's rejected output here is safe.
 *
 * The longest envelope is 41 bytes against a 256-byte floor, so truncation is
 * unreachable; it degrades to the generic failure body the 404 default already
 * stores rather than recursing back through http_too_large(). */
static FLASHMEM const AiusHttpApplicationResponse *http_error(
	AiusOpticalAppDispatcher *dispatcher, unsigned status, const char *token)
{
	int written = snprintf((char *)dispatcher->body, dispatcher->body_capacity,
	                       "{\"ok\":false,\"error\":\"%s\"}", token);
	if (written <= 0 || (size_t)written >= dispatcher->body_capacity)
		return http_json(dispatcher, status, "{\"ok\":false}");
	return http_immediate(dispatcher, status, kContentJson, dispatcher->body,
	                      (size_t)written);
}

static FLASHMEM const AiusHttpApplicationResponse *http_too_large(
	AiusOpticalAppDispatcher *dispatcher)
{
	return http_error(dispatcher, 500, "response_too_large");
}

/* Publishes a body the dispatcher formatted into its own buffer. A snprintf
 * that did not fit becomes the same deterministic 500 as a serializer that did
 * not fit. */
static FLASHMEM const AiusHttpApplicationResponse *http_body_text(
	AiusOpticalAppDispatcher *dispatcher, unsigned status, int written)
{
	if (written <= 0 || (size_t)written >= dispatcher->body_capacity)
		return http_too_large(dispatcher);
	return http_immediate(dispatcher, status, kContentJson, dispatcher->body,
	                      (size_t)written);
}

static FLASHMEM const AiusHttpApplicationResponse *http_serialized(
	AiusOpticalAppDispatcher *dispatcher, const char *content_type,
	size_t length)
{
	if (length == 0u || length > dispatcher->body_capacity)
		return http_too_large(dispatcher);
	return http_immediate(dispatcher, 200, content_type, dispatcher->body,
	                      length);
}

/* The home page is the one HTML route, so its exhaustion answer is HTML too. */
static FLASHMEM const AiusHttpApplicationResponse *http_serialized_html(
	AiusOpticalAppDispatcher *dispatcher, size_t length)
{
	if (length == 0u || length > dispatcher->body_capacity)
		return http_immediate(dispatcher, 500, kContentHtml,
		                      (const uint8_t *)"<p>response_too_large</p>",
		                      sizeof("<p>response_too_large</p>") - 1u);
	return http_immediate(dispatcher, 200, kContentHtml, dispatcher->body,
	                      length);
}

/* ------------------------------------------------------------------ */
/* SD resource paths                                                   */
/* ------------------------------------------------------------------ */

static FLASHMEM bool path_append(char *path, size_t capacity, size_t *length,
                                 const char *text, size_t text_length)
{
	if (text_length >= capacity || *length > capacity - 1u - text_length)
		return false;
	memcpy(path + *length, text, text_length);
	*length += text_length;
	path[*length] = '\0';
	return true;
}

static FLASHMEM bool resource_is_config(const AiusHttpRequest *request)
{
	return request->resource != NULL && request->resource_length >= 11u &&
	       memcmp(request->resource + request->resource_length - 11u,
	              "config.json", 11u) == 0;
}

static FLASHMEM bool resource_suffix_is(const AiusHttpRequest *request,
                                        const char *suffix)
{
	size_t suffix_length = strlen(suffix);
	return request->resource_length >= suffix_length &&
	       memcmp(request->resource + request->resource_length - suffix_length,
	              suffix, suffix_length) == 0;
}

static FLASHMEM const char *resource_content_type(const AiusHttpRequest *request)
{
	if (resource_suffix_is(request, ".html"))
		return kContentHtml;
	if (resource_suffix_is(request, ".js"))
		return "application/javascript";
	if (resource_suffix_is(request, ".css"))
		return "text/css";
	if (resource_suffix_is(request, ".json"))
		return kContentJson;
	return kContentBinary;
}

/* Rejects traversal, percent-encoding, backslashes, empty or over-long path
 * components, and anything outside the /aius/www/ subtree -- the same rules the
 * pre-cutover http_sd_path() enforces. */
static FLASHMEM bool resource_path_valid(const AiusHttpRequest *request)
{
	const char *resource = (const char *)request->resource;
	size_t component_length = 0u;
	unsigned components = 1u;
	if (request->resource == NULL || request->resource_length < 5u ||
	    request->resource_length > 96u ||
	    memcmp(request->resource, "www/", 4u) != 0)
		return false;
	for (size_t i = 4u; i < request->resource_length; i++) {
		uint8_t ch = request->resource[i];
		if (ch == '%' || ch == '\\' || ch == 0u)
			return false;
		if (ch == '/') {
			if (i == 4u || i + 1u == request->resource_length ||
			    request->resource[i - 1u] == '/' || component_length > 32u)
				return false;
			component_length = 0u;
			components++;
			if (components > 8u)
				return false;
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
	return component_length <= 32u;
}

static FLASHMEM bool replay_id_chars_valid(const uint8_t *id, size_t length)
{
	if (id == NULL || length == 0u || length > AIUS_HTTP_MAX_REPLAY_ID)
		return false;
	for (size_t i = 0; i < length; i++) {
		uint8_t ch = id[i];
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
			return false;
	}
	return true;
}

/* /aius/replays/<id><suffix> */
static FLASHMEM bool build_replay_path(char *path, size_t capacity,
                                       const uint8_t *id, size_t id_length,
                                       const char *suffix)
{
	size_t length = 0u;
	path[0] = '\0';
	return path_append(path, capacity, &length, AIUS_OPTICAL_APP_REPLAY_ROOT,
	                   sizeof(AIUS_OPTICAL_APP_REPLAY_ROOT) - 1u) &&
	       path_append(path, capacity, &length, (const char *)id, id_length) &&
	       path_append(path, capacity, &length, suffix, strlen(suffix));
}

/* /aius/<resource> */
static FLASHMEM bool build_resource_path(char *path, size_t capacity,
                                         const AiusHttpRequest *request)
{
	size_t length = 0u;
	path[0] = '\0';
	return path_append(path, capacity, &length, AIUS_OPTICAL_APP_SD_ROOT,
	                   sizeof(AIUS_OPTICAL_APP_SD_ROOT) - 1u) &&
	       path_append(path, capacity, &length, (const char *)request->resource,
	                   request->resource_length);
}

/* Distinct per upload so an orphaned temporary from an earlier job can never be
 * mistaken for this one's; the entropy source is the application's. */
static FLASHMEM bool build_upload_scratch(AiusOpticalAppDispatcher *dispatcher,
                                          bool with_temp)
{
	uint32_t id = dispatcher->app.upload_id(dispatcher->app.context);
	int backup_length;
	dispatcher->upload_sequence++;
	if (with_temp) {
		int temp_length = snprintf(dispatcher->temp_path,
		                           sizeof(dispatcher->temp_path),
		                           "/aius/.upload-%08lx-%08lx.tmp",
		                           (unsigned long)id,
		                           (unsigned long)dispatcher->upload_sequence);
		if (temp_length <= 0 ||
		    (size_t)temp_length >= sizeof(dispatcher->temp_path))
			return false;
	}
	backup_length = snprintf(dispatcher->backup_path,
	                         sizeof(dispatcher->backup_path),
	                         "/aius/.backup-%08lx-%08lx.bak",
	                         (unsigned long)id,
	                         (unsigned long)dispatcher->upload_sequence);
	return backup_length > 0 &&
	       (size_t)backup_length < sizeof(dispatcher->backup_path);
}

/* ------------------------------------------------------------------ */
/* request body helpers                                                */
/* ------------------------------------------------------------------ */

static FLASHMEM const uint8_t *find_bytes(const uint8_t *data, size_t length,
                                          const char *needle)
{
	size_t needle_length = strlen(needle);
	if (data == NULL || needle_length == 0u || length < needle_length)
		return NULL;
	for (size_t i = 0; i + needle_length <= length; i++)
		if (memcmp(data + i, needle, needle_length) == 0)
			return data + i;
	return NULL;
}

/* Extracts {"id":"<id>"[,"loop":true]} from a bounded body. No allocation, no
 * NUL-termination assumption about the transport buffer. */
static FLASHMEM bool replay_body_id(AiusOpticalAppDispatcher *dispatcher,
                                    const AiusHttpRequest *request, bool *loop)
{
	const uint8_t *body = request->body;
	size_t length = request->body_length;
	const uint8_t *start = find_bytes(body, length, "\"id\":\"");
	const uint8_t *end;
	size_t id_length;
	dispatcher->replay_id[0] = '\0';
	*loop = false;
	if (start == NULL)
		return false;
	start += sizeof("\"id\":\"") - 1u;
	end = (const uint8_t *)memchr(start, '"', (size_t)(body + length - start));
	if (end == NULL)
		return false;
	id_length = (size_t)(end - start);
	if (!replay_id_chars_valid(start, id_length) ||
	    id_length >= sizeof(dispatcher->replay_id))
		return false;
	memcpy(dispatcher->replay_id, start, id_length);
	dispatcher->replay_id[id_length] = '\0';
	*loop = find_bytes(body, length, "\"loop\":true") != NULL;
	return true;
}

/* ------------------------------------------------------------------ */
/* SD job responses                                                    */
/* ------------------------------------------------------------------ */

static FLASHMEM AiusHttpApplicationResponse *sd_job_response(
	AiusOpticalAppDispatcher *dispatcher, unsigned status,
	const char *content_type, const uint8_t *body, size_t body_length)
{
	AiusHttpApplicationResponse *response = &dispatcher->http_response;
	memset(response, 0, sizeof(*response));
	response->kind = AIUS_HTTP_APP_SD_JOB;
	response->status = status;
	response->content_type = content_type;
	response->body = body_length != 0u ? body : NULL;
	response->body_length = body_length;
	response->sd_job.install_mode = AIUS_SD_INSTALL_REPLACE;
	return response;
}

static FLASHMEM const AiusHttpApplicationResponse *sd_download(
	AiusOpticalAppDispatcher *dispatcher, const char *content_type)
{
	AiusHttpApplicationResponse *response =
		sd_job_response(dispatcher, 200, content_type, NULL, 0u);
	response->sd_job.kind = AIUS_SD_JOB_DOWNLOAD;
	response->sd_job.target_path = dispatcher->target_path;
	response->sd_job.content_type = content_type;
	return response;
}

static FLASHMEM const AiusHttpApplicationResponse *sd_delete(
	AiusOpticalAppDispatcher *dispatcher)
{
	AiusHttpApplicationResponse *response =
		sd_job_response(dispatcher, 200, kContentJson,
		                (const uint8_t *)"{\"ok\":true}",
		                sizeof("{\"ok\":true}") - 1u);
	response->sd_job.kind = AIUS_SD_JOB_DELETE;
	response->sd_job.target_path = dispatcher->target_path;
	response->sd_job.require_existing = true;
	return response;
}

static FLASHMEM const AiusHttpApplicationResponse *sd_upload(
	AiusOpticalAppDispatcher *dispatcher, AiusSdInstallMode mode,
	uint64_t total, uint64_t initial_offset, const uint8_t *body,
	size_t body_length)
{
	AiusHttpApplicationResponse *response =
		sd_job_response(dispatcher, 200, kContentJson, body, body_length);
	response->sd_job.kind = AIUS_SD_JOB_UPLOAD;
	response->sd_job.install_mode = mode;
	response->sd_job.target_path = dispatcher->target_path;
	response->sd_job.total = total;
	response->sd_job.initial_offset = initial_offset;
	if (mode != AIUS_SD_INSTALL_APPEND_PART) {
		response->sd_job.temp_path = dispatcher->temp_path;
		response->sd_job.backup_path = dispatcher->backup_path;
	}
	return response;
}

/* ------------------------------------------------------------------ */
/* route handlers                                                      */
/* ------------------------------------------------------------------ */

static FLASHMEM const AiusHttpApplicationResponse *dispatch_sd_upload(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	bool replay = request->route == AIUS_HTTP_ROUTE_SD_REPLAY;
	/* Legacy order for the streamed PUT path: the refusal to overwrite the
	 * public config outranks authentication, which outranks media state. */
	if (!replay && resource_is_config(request))
		return http_error(dispatcher, 403, "config_public_only");
	if (!app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (app->sd_busy(app->context))
		return http_error(dispatcher, 507, "sd_upload");
	if (replay) {
		int written;
		bool final_chunk;
		if (!request->has_content_range ||
		    !replay_id_chars_valid(request->replay_id,
		                           request->replay_id_length))
			return http_error(dispatcher, 400, "sd_upload");
		final_chunk = (uint64_t)request->content_range_end + 1u ==
			(uint64_t)request->content_range_total;
		if (!final_chunk) {
			if (!build_replay_path(dispatcher->target_path,
			                       sizeof(dispatcher->target_path),
			                       request->replay_id,
			                       request->replay_id_length, ".part"))
				return http_error(dispatcher, 507, "sd_upload");
			written = snprintf((char *)dispatcher->body,
			                   dispatcher->body_capacity,
			                   "{\"ok\":true,\"next_offset\":%lu}",
			                   (unsigned long)request->content_range_end + 1ul);
			if (written <= 0 || (size_t)written >= dispatcher->body_capacity)
				return http_too_large(dispatcher);
			return sd_upload(dispatcher, AIUS_SD_INSTALL_APPEND_PART,
			                 request->content_length,
			                 request->content_range_start, dispatcher->body,
			                 (size_t)written);
		}
		if (!build_replay_path(dispatcher->temp_path,
		                       sizeof(dispatcher->temp_path),
		                       request->replay_id, request->replay_id_length,
		                       ".part") ||
		    !build_replay_path(dispatcher->target_path,
		                       sizeof(dispatcher->target_path),
		                       request->replay_id, request->replay_id_length,
		                       ".iec") ||
		    !build_upload_scratch(dispatcher, false))
			return http_error(dispatcher, 507, "sd_upload");
		return sd_upload(dispatcher, AIUS_SD_INSTALL_FINALIZE_PART,
		                 request->content_length, request->content_range_start,
		                 (const uint8_t *)"{\"ok\":true}",
		                 sizeof("{\"ok\":true}") - 1u);
	}
	if (!resource_path_valid(request) ||
	    !build_resource_path(dispatcher->target_path,
	                         sizeof(dispatcher->target_path), request) ||
	    !build_upload_scratch(dispatcher, true))
		return http_error(dispatcher, 507, "sd_upload");
	return sd_upload(dispatcher, AIUS_SD_INSTALL_REPLACE,
	                 request->content_length, 0u,
	                 (const uint8_t *)"{\"ok\":true}",
	                 sizeof("{\"ok\":true}") - 1u);
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_sd(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	const char *content_type;
	if (request->method == AIUS_HTTP_PUT)
		return dispatch_sd_upload(dispatcher, request);
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (request->route == AIUS_HTTP_ROUTE_SD_STATUS)
		return http_json(dispatcher, 200, "{\"ready\":true}");
	if (request->route == AIUS_HTTP_ROUTE_SD_REPLAY) {
		if (!replay_id_chars_valid(request->replay_id,
		                           request->replay_id_length) ||
		    !build_replay_path(dispatcher->target_path,
		                       sizeof(dispatcher->target_path),
		                       request->replay_id, request->replay_id_length,
		                       ".iec"))
			return http_error(dispatcher, 404, "sd_path");
		content_type = kContentBinary;
	} else {
		if (!resource_path_valid(request) ||
		    !build_resource_path(dispatcher->target_path,
		                         sizeof(dispatcher->target_path), request))
			return http_error(dispatcher, 404, "sd_path");
		content_type = resource_content_type(request);
	}
	if (request->method != AIUS_HTTP_GET && resource_is_config(request))
		return http_error(dispatcher, 403, "config_public_only");
	if (request->method == AIUS_HTTP_DELETE &&
	    !app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	if (app->sd_busy(app->context))
		return http_error(dispatcher, 503, "sd_busy");
	if (request->method == AIUS_HTTP_GET)
		return sd_download(dispatcher, content_type);
	if (request->method == AIUS_HTTP_DELETE)
		return sd_delete(dispatcher);
	return http_error(dispatcher, 405, "sd_method");
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_store(
	AiusOpticalAppDispatcher *dispatcher, AiusOpticalAppStoreResult result,
	const char *invalid_token)
{
	switch (result) {
	case AIUS_OPTICAL_APP_STORE_OK:
		return http_json(dispatcher, 200, "{\"ok\":true}");
	case AIUS_OPTICAL_APP_STORE_STALE:
		return http_error(dispatcher, 409, "stale_generation");
	case AIUS_OPTICAL_APP_STORE_BUSY:
		return http_error(dispatcher, 503, "sd_busy");
	case AIUS_OPTICAL_APP_STORE_INVALID:
	default:
		break;
	}
	return http_error(dispatcher, 400, invalid_token);
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_dsp_config(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	if (request->method != AIUS_HTTP_PUT)
		return http_serialized(dispatcher, kContentJson,
		                       app->write_dsp_config(app->context,
		                                             dispatcher->body,
		                                             dispatcher->body_capacity));
	if (!app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	return dispatch_store(dispatcher,
	                      app->store_dsp_config(app->context, request->body,
	                                            request->body_length),
	                      "dsp_config_invalid");
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_config_public(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	return dispatch_store(dispatcher,
	                      app->store_public_config(app->context, request->body,
	                                               request->body_length),
	                      "config_invalid");
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_replays(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	bool loop = false;
	bool public_route = request->route == AIUS_HTTP_ROUTE_REPLAYS_STATUS ||
		(request->route == AIUS_HTTP_ROUTE_REPLAYS &&
		 request->method == AIUS_HTTP_GET);
	if (!public_route &&
	    !app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS_STATUS)
		return http_serialized(dispatcher, kContentJson,
		                       app->write_replay_status(app->context,
		                                                dispatcher->body,
		                                                dispatcher->body_capacity));
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS_STOP) {
		app->replay_stop(app->context);
		return http_json(dispatcher, 200,
		                 "{\"ok\":true,\"state\":\"stopped\"}");
	}
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS_START) {
		if (!app->sd_ready(app->context) ||
		    !replay_body_id(dispatcher, request, &loop))
			return http_error(dispatcher, 400, "replay_body");
		switch (app->replay_start(app->context, dispatcher->replay_id, loop)) {
		case AIUS_OPTICAL_APP_REPLAY_OK:
			return http_body_text(dispatcher, 200,
				snprintf((char *)dispatcher->body, dispatcher->body_capacity,
				         "{\"ok\":true,\"state\":\"prebuffering\",\"id\":\"%s\"}",
				         dispatcher->replay_id));
		case AIUS_OPTICAL_APP_REPLAY_NOT_FOUND:
			return http_error(dispatcher, 404, "replay_not_found");
		case AIUS_OPTICAL_APP_REPLAY_ACTIVE:
		case AIUS_OPTICAL_APP_REPLAY_FAILED:
		default:
			break;
		}
		return http_error(dispatcher, 409, "replay_start");
	}
	if (request->method == AIUS_HTTP_GET) {
		/* Pre-cutover behaviour: the listing reports missing media in its own
		 * body with a 200, it does not fail the request. */
		if (!app->sd_ready(app->context))
			return http_error(dispatcher, 200, "sd_unavailable");
		return http_serialized(dispatcher, kContentJson,
		                       app->write_replay_list(app->context,
		                                              dispatcher->body,
		                                              dispatcher->body_capacity));
	}
	if (request->method == AIUS_HTTP_DELETE) {
		if (!replay_body_id(dispatcher, request, &loop))
			return http_error(dispatcher, 400, "replay_body");
		switch (app->replay_delete(app->context, dispatcher->replay_id)) {
		case AIUS_OPTICAL_APP_REPLAY_OK:
			return http_body_text(dispatcher, 200,
				snprintf((char *)dispatcher->body, dispatcher->body_capacity,
				         "{\"ok\":true,\"deleted\":\"%s\"}",
				         dispatcher->replay_id));
		case AIUS_OPTICAL_APP_REPLAY_ACTIVE:
			return http_error(dispatcher, 409, "replay_active");
		case AIUS_OPTICAL_APP_REPLAY_NOT_FOUND:
			return http_error(dispatcher, 404, "replay_not_found");
		case AIUS_OPTICAL_APP_REPLAY_FAILED:
		default:
			break;
		}
		return http_error(dispatcher, 503, "replay_delete");
	}
	return http_error(dispatcher, 405, "replay_method");
}

/* ------------------------------------------------------------------ */
/* public entry points                                                 */
/* ------------------------------------------------------------------ */

static FLASHMEM bool app_complete(const AiusOpticalApp *app)
{
	return app->sd_ready != NULL && app->sd_busy != NULL &&
	       app->auth_matches != NULL && app->write_home != NULL &&
	       app->write_status != NULL && app->write_events != NULL &&
	       app->write_dsp_config != NULL && app->write_meters != NULL &&
	       app->write_replay_status != NULL && app->write_replay_list != NULL &&
	       app->store_public_config != NULL && app->store_dsp_config != NULL &&
	       app->replay_start != NULL && app->replay_delete != NULL &&
	       app->replay_stop != NULL && app->set_test_running != NULL &&
	       app->clear_counters != NULL && app->request_reboot != NULL &&
	       app->write_usb_payload != NULL && app->upload_id != NULL;
}

FLASHMEM bool aius_optical_app_init(AiusOpticalAppDispatcher *dispatcher,
                                    const AiusOpticalApp *app,
                                    uint8_t *body, size_t body_capacity)
{
	if (dispatcher == NULL)
		return false;
	memset(dispatcher, 0, sizeof(*dispatcher));
	if (app == NULL || body == NULL ||
	    body_capacity < AIUS_OPTICAL_APP_MIN_BODY || !app_complete(app))
		return false;
	dispatcher->app = *app;
	dispatcher->body = body;
	dispatcher->body_capacity = body_capacity;
	dispatcher->valid = true;
	return true;
}

FLASHMEM const AiusHttpApplicationResponse *aius_optical_app_http_dispatch(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app;
	if (dispatcher == NULL || !dispatcher->valid || request == NULL)
		return NULL;
	app = &dispatcher->app;
	dispatcher->http_dispatches++;
	switch (request->route) {
	case AIUS_HTTP_ROUTE_HOME:
		return http_serialized_html(dispatcher,
		                            app->write_home(app->context,
		                                            dispatcher->body,
		                                            dispatcher->body_capacity));
	case AIUS_HTTP_ROUTE_STATUS:
		return http_serialized(dispatcher, kContentJson,
		                       app->write_status(app->context, dispatcher->body,
		                                         dispatcher->body_capacity));
	case AIUS_HTTP_ROUTE_EVENTS:
		return http_serialized(dispatcher, kContentJson,
		                       app->write_events(app->context, request->cursor,
		                                         dispatcher->body,
		                                         dispatcher->body_capacity));
	case AIUS_HTTP_ROUTE_METERS:
		return http_serialized(dispatcher, kContentJson,
		                       app->write_meters(app->context, dispatcher->body,
		                                         dispatcher->body_capacity));
	case AIUS_HTTP_ROUTE_DSP_CONFIG:
		return dispatch_dsp_config(dispatcher, request);
	case AIUS_HTTP_ROUTE_CONFIG_PUBLIC:
		return dispatch_config_public(dispatcher, request);
	case AIUS_HTTP_ROUTE_SD_STATUS:
	case AIUS_HTTP_ROUTE_SD_FILE:
	case AIUS_HTTP_ROUTE_SD_REPLAY:
		return dispatch_sd(dispatcher, request);
	case AIUS_HTTP_ROUTE_REPLAYS:
	case AIUS_HTTP_ROUTE_REPLAYS_STATUS:
	case AIUS_HTTP_ROUTE_REPLAYS_START:
	case AIUS_HTTP_ROUTE_REPLAYS_STOP:
		return dispatch_replays(dispatcher, request);
	case AIUS_HTTP_ROUTE_START:
		app->set_test_running(app->context, true);
		return http_json(dispatcher, 200, "{\"ok\":true,\"running\":true}");
	case AIUS_HTTP_ROUTE_STOP:
		app->set_test_running(app->context, false);
		return http_json(dispatcher, 200, "{\"ok\":true,\"running\":false}");
	case AIUS_HTTP_ROUTE_CLEAR:
		app->clear_counters(app->context);
		return http_json(dispatcher, 200, "{\"ok\":true}");
	case AIUS_HTTP_ROUTE_REBOOT:
		/* The reset itself belongs to the runtime, after the response has been
		 * written. Dispatch records the request and returns. */
		app->request_reboot(app->context);
		return http_json(dispatcher, 200, "{\"ok\":true,\"rebooting\":true}");
	/* /api/mode and /api/mix still parse into routes but the DSP single-config
	 * work removed everything that dispatched them; they answer 404 exactly as
	 * the pre-cutover switch default does. */
	case AIUS_HTTP_ROUTE_MODE:
	case AIUS_HTTP_ROUTE_MIX:
	case AIUS_HTTP_ROUTE_UNKNOWN:
	default:
		break;
	}
	return http_json(dispatcher, 404, "{\"ok\":false}");
}

static FLASHMEM const AiusUsbApplicationResponse *usb_error(
	AiusOpticalAppDispatcher *dispatcher, AiusUsbError error)
{
	AiusUsbApplicationResponse *response = &dispatcher->usb_response;
	memset(response, 0, sizeof(*response));
	response->error = error;
	return response;
}

static FLASHMEM const AiusUsbApplicationResponse *usb_ok(
	AiusOpticalAppDispatcher *dispatcher, const char *verb,
	const char *payload, size_t payload_length)
{
	AiusUsbApplicationResponse *response = &dispatcher->usb_response;
	memset(response, 0, sizeof(*response));
	response->error = AIUS_USB_ERROR_NONE;
	response->verb = verb;
	response->verb_length = strlen(verb);
	response->payload = payload_length != 0u ? payload : NULL;
	response->payload_length = payload_length;
	return response;
}

/* Asks the application for one bounded, printable payload. Anything the USB
 * runtime would reject becomes a deterministic INTERNAL error here instead. */
static FLASHMEM const AiusUsbApplicationResponse *usb_payload(
	AiusOpticalAppDispatcher *dispatcher, AiusUsbCommandKind kind,
	const char *verb)
{
	const AiusOpticalApp *app = &dispatcher->app;
	size_t length = app->write_usb_payload(app->context, kind,
	                                       dispatcher->usb_payload,
	                                       sizeof(dispatcher->usb_payload));
	if (length == 0u || length > AIUS_OPTICAL_APP_USB_PAYLOAD_MAX)
		return usb_error(dispatcher, AIUS_USB_ERROR_INTERNAL);
	for (size_t i = 0; i < length; i++) {
		unsigned char value = (unsigned char)dispatcher->usb_payload[i];
		if (value < 0x20u || value > 0x7eu)
			return usb_error(dispatcher, AIUS_USB_ERROR_INTERNAL);
	}
	return usb_ok(dispatcher, verb, dispatcher->usb_payload, length);
}

FLASHMEM const AiusUsbApplicationResponse *aius_optical_app_usb_dispatch(
	AiusOpticalAppDispatcher *dispatcher,
	const AiusUsbApplicationRequest *request)
{
	if (dispatcher == NULL || !dispatcher->valid || request == NULL)
		return NULL;
	dispatcher->usb_dispatches++;
	switch (request->command.kind) {
	case AIUS_USB_COMMAND_PROTOCOL:
		return usb_ok(dispatcher, "PROTOCOL", "1", 1u);
	case AIUS_USB_COMMAND_ID:
		return usb_payload(dispatcher, AIUS_USB_COMMAND_ID, "ID");
	case AIUS_USB_COMMAND_AUTH:
		return usb_payload(dispatcher, AIUS_USB_COMMAND_AUTH, "AUTH");
	case AIUS_USB_COMMAND_BOOTLOADER:
		/* The control runtime supplies the "entering" payload and owns the
		 * transition; a payload here would be rejected. */
		return usb_ok(dispatcher, "BOOTLOADER", NULL, 0u);
	case AIUS_USB_COMMAND_NONE:
	default:
		break;
	}
	return usb_error(dispatcher, AIUS_USB_ERROR_INTERNAL);
}
