/* Optical application dispatch -- see aius_optical_app.h.
 *
 * Every definition in this file is FLASHMEM. The attribute must sit on the
 * definition: GCC does not inherit a `section` attribute from a forward
 * declaration, so a prototype-only FLASHMEM silently leaves the code in ITCM,
 * and the teensy41-optical image has ~1.3 kB of ITCM left.
 */

#include "aius_optical_app.h"

#include "aius_json.h"

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
/* The two config files the pre-cutover firmware writes, under the same root:
 * optical_main.cpp:1742 and :1794 pass "/aius/config.json" and "/aius/dsp.json"
 * to http_sd_upload_begin_target(). Named once so the literal the /sd/ route's
 * public-config refusal matches against is the same one the write targets. */
#define AIUS_OPTICAL_APP_CONFIG_NAME "config.json"
#define AIUS_OPTICAL_APP_DSP_NAME "dsp.json"

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
	size_t name_length = sizeof(AIUS_OPTICAL_APP_CONFIG_NAME) - 1u;
	return request->resource != NULL &&
	       request->resource_length >= name_length &&
	       memcmp(request->resource + request->resource_length - name_length,
	              AIUS_OPTICAL_APP_CONFIG_NAME, name_length) == 0;
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

/* Day-long browser caching for the two stable static-asset suffixes, and ONLY
 * those. HTML stays uncached so a card sync propagates page changes on the
 * next load; .json is live data (config.json above all); binary covers the
 * presets and replay vectors, which the pages re-list after every save --
 * caching any of those trades correctness for nothing. The operator pays one
 * hard refresh after an asset deploy, accepted 2026-07-27. */
#define AIUS_OPTICAL_APP_ASSET_MAX_AGE_S 86400u

static FLASHMEM uint32_t resource_cache_max_age(const AiusHttpRequest *request)
{
	if (resource_suffix_is(request, ".js") ||
	    resource_suffix_is(request, ".css"))
		return AIUS_OPTICAL_APP_ASSET_MAX_AGE_S;
	return 0u;
}

/* Rejects traversal, percent-encoding, backslashes, empty or over-long path
 * components, and anything outside the /aius/www/ subtree -- the same rules the
 * pre-cutover http_sd_path() enforces. Takes the resource slice rather than the
 * request so the directory form below can validate its body by handing over the
 * same bytes with the trailing slash removed. */
static FLASHMEM bool resource_path_valid(const uint8_t *resource, size_t length)
{
	size_t component_length = 0u;
	unsigned components = 1u;
	if (resource == NULL || length < 5u || length > 96u ||
	    memcmp(resource, "www/", 4u) != 0)
		return false;
	for (size_t i = 4u; i < length; i++) {
		uint8_t ch = resource[i];
		if (ch == '%' || ch == '\\' || ch == 0u)
			return false;
		if (ch == '/') {
			if (i == 4u || i + 1u == length || resource[i - 1u] == '/' ||
			    component_length > 32u)
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
	for (size_t i = 4u; i + 1u < length; i++)
		if (resource[i] == '.' && resource[i + 1u] == '.' &&
		    (i == 4u || resource[i - 1u] == '/') &&
		    (i + 2u == length || resource[i + 2u] == '/'))
			return false;
	return component_length <= 32u;
}

/* A resource that names a directory is the same resource spelled with a
 * trailing slash -- the shape a browser already produces when a listing's
 * relative links are followed, and the reason the index needed no new route and
 * no parser change. */
static FLASHMEM bool resource_is_directory(const AiusHttpRequest *request)
{
	return request->resource != NULL && request->resource_length != 0u &&
	       request->resource[request->resource_length - 1u] == '/';
}

/* "www/" is the subtree root and has no body to check; anything deeper must be
 * a resource the download route would also accept, so the index and /sd/ admit
 * exactly the same paths. */
static FLASHMEM bool resource_directory_valid(const uint8_t *resource,
                                              size_t length)
{
	if (resource == NULL || length < 4u || length > 96u ||
	    resource[length - 1u] != '/' || memcmp(resource, "www/", 4u) != 0)
		return false;
	return length == 4u || resource_path_valid(resource, length - 1u);
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

/* /aius/<name> -- one layout for both the /sd/ resource tree and the two config
 * files the API writes. */
static FLASHMEM bool build_root_path(char *path, size_t capacity,
                                     const char *name, size_t name_length)
{
	size_t length = 0u;
	path[0] = '\0';
	return path_append(path, capacity, &length, AIUS_OPTICAL_APP_SD_ROOT,
	                   sizeof(AIUS_OPTICAL_APP_SD_ROOT) - 1u) &&
	       path_append(path, capacity, &length, name, name_length);
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

/* A read cursor over a request body: lib/aius_json's AiusJsonCursor -- three
 * words, no buffer, no allocation and no recursion, so it costs the epoch loop
 * nothing beyond the bytes it walks. The transport buffer is not
 * NUL-terminated, so every read is bounded by the cursor's `len` rather than by
 * a terminator, and no strlen() ever sees the body.
 *
 * Three of this grammar's primitives stay here rather than joining the shared
 * kit, because each is this reader's contract and not the config parsers':
 * whitespace is admitted before every structural character, a string's bytes
 * are borrowed in place rather than unescaped into a buffer, and a literal is
 * matched without a value-boundary check. Each is now a wrapper over a shared
 * primitive rather than a second copy of it. */

/* Consumes one structural character together with the whitespace RFC 8259 §2
 * permits before it -- space, tab, LF, CR and nothing else, so a form feed, a
 * vertical tab or an embedded NUL stays a parse failure. Every structural
 * character in the grammar below goes through here, which is what makes the
 * spacing insignificant. */
static FLASHMEM bool body_consume(AiusJsonCursor *cursor, uint8_t expected)
{
	aius_json_skip_whitespace(cursor);
	return aius_json_consume(cursor, expected) != 0;
}

/* Borrows a string's raw bytes in place -- nothing is copied and nothing is
 * unescaped, so whitespace between the quotes survives as the data it is. This
 * is why aius_json_take_string() is not used here: that one decodes escapes
 * into a bounded buffer, which this reader has no need of and no buffer for.
 * A backslash is refused outright rather than decoded: the two consumers are
 * this object's key set and replay_id_chars_valid(), and an escape can only
 * ever spell a byte one of them already accepts literally, so decoding would
 * add a second spelling for an id the card already has under its plain name. */
static FLASHMEM bool body_take_string(AiusJsonCursor *cursor,
                                      const uint8_t **text, size_t *length)
{
	size_t start;
	if (!body_consume(cursor, '"'))
		return false;
	start = cursor->pos;
	while (!aius_json_at_end(cursor)) {
		uint8_t ch = cursor->data[cursor->pos];
		if (ch == '"') {
			*text = cursor->data + start;
			*length = cursor->pos - start;
			cursor->pos++;
			return true;
		}
		if (ch == '\\' || ch < 0x20u)
			return false;
		cursor->pos++;
	}
	return false;
}

/* `true` / `false`. A trailing character is not this function's business: the
 * object grammar demands a comma or a closing brace next, so `truex` fails
 * there. That is why this is aius_json_match() rather than
 * aius_json_literal() -- the latter would refuse `truex` one step earlier, on
 * its own value-boundary check, which is the config parsers' grammar and not
 * this one's. */
static FLASHMEM bool body_take_literal(AiusJsonCursor *cursor, const char *text)
{
	aius_json_skip_whitespace(cursor);
	return aius_json_match(cursor, text) != 0;
}

/* The body reader borrows its key bytes in place, so there is no buffer for a
 * long key to overflow and the shared comparison's overflow flag is always 0. */
static FLASHMEM bool body_key_is(const uint8_t *key, size_t key_length,
                                 const char *expected)
{
	return aius_json_key_is(key, key_length, 0, expected) != 0;
}

/* Parses {"id":"<id>"[,"loop":<bool>]} in either key order, tolerating the
 * insignificant whitespace RFC 8259 §2 permits around every structural
 * character -- json.dumps' `{"id": "x", "loop": true}` and JSON.stringify's
 * compact form are the same request, which is the whole point of the rewrite
 * away from the substring scan this replaced.
 *
 * The object is flat, so one left-to-right cursor pass is the entire grammar:
 * no recursion, no allocation, no buffer beyond the dispatcher's existing
 * replay_id, and one bounded walk of a body the transport already capped.
 *
 * Whitespace is the only thing newly admitted. The pass is stricter than the
 * scan everywhere else, because it reads the document rather than hunting a
 * needle inside it: an unknown key, a duplicate key, a non-boolean `loop`, a
 * non-string `id`, a body that is not a lone object, or anything trailing the
 * closing brace is refused instead of ignored. */
static FLASHMEM bool replay_body_id(AiusOpticalAppDispatcher *dispatcher,
                                    const AiusHttpRequest *request, bool *loop)
{
	AiusJsonCursor cursor = { request->body, request->body_length, 0u };
	const uint8_t *id = NULL;
	size_t id_length = 0u;
	bool parsed_loop = false;
	bool seen_loop = false;

	dispatcher->replay_id[0] = '\0';
	*loop = false;
	if (request->body == NULL || !body_consume(&cursor, '{'))
		return false;
	for (;;) {
		const uint8_t *key;
		size_t key_length;
		if (!body_take_string(&cursor, &key, &key_length) ||
		    !body_consume(&cursor, ':'))
			return false;
		if (body_key_is(key, key_length, "id")) {
			/* A second "id" is a duplicate key, not an override. */
			if (id != NULL || !body_take_string(&cursor, &id, &id_length))
				return false;
		} else if (body_key_is(key, key_length, "loop")) {
			if (seen_loop)
				return false;
			seen_loop = true;
			if (body_take_literal(&cursor, "true"))
				parsed_loop = true;
			else if (!body_take_literal(&cursor, "false"))
				return false;
		} else {
			return false;
		}
		if (!body_consume(&cursor, ','))
			break;
	}
	if (!body_consume(&cursor, '}'))
		return false;
	aius_json_skip_whitespace(&cursor);
	if (!aius_json_at_end(&cursor))
		return false;
	if (id == NULL || !replay_id_chars_valid(id, id_length) ||
	    id_length >= sizeof(dispatcher->replay_id))
		return false;
	memcpy(dispatcher->replay_id, id, id_length);
	dispatcher->replay_id[id_length] = '\0';
	*loop = parsed_loop;
	return true;
}

/* Strict unsigned integer for the measure body: bare digits only (no sign, no
 * fraction, no exponent -- a frequency in Hz has no business carrying any),
 * bounded to 7 digits so the value stays far inside uint32. */
static FLASHMEM bool body_take_uint(AiusJsonCursor *cursor, uint32_t *value)
{
	aius_json_skip_whitespace(cursor);
	uint32_t parsed = 0;
	size_t digits = 0;
	while (cursor->pos < cursor->len) {
		uint8_t ch = cursor->data[cursor->pos];
		if (ch < '0' || ch > '9')
			break;
		if (++digits > 7u)
			return false;
		parsed = parsed * 10u + (uint32_t)(ch - '0');
		cursor->pos++;
	}
	if (digits == 0 || parsed == 0)
		return false; /* 0 Hz is schema-invalid: refuse at parse (400), not 409 */
	*value = parsed;
	return true;
}

/* POST /api/measure/start body, read with the same strictness as
 * replay_body_id above: {"id":"<vector>"} with optional "f0_l"/"f0_r"
 * (integer Hz, default 440 -- the corpus baseline) and an optional "probes"
 * array of up to AIUS_OPTICAL_APP_MEASURE_PROBES_MAX integer frequencies.
 * Unknown keys, duplicates, non-integer numbers and trailing bytes refuse;
 * range validation belongs to the analysis (fs/2 bounds), not the parser. */
static FLASHMEM bool measure_body(const AiusHttpRequest *request,
                                  char *id, size_t id_capacity,
                                  uint32_t *f0_l_hz, uint32_t *f0_r_hz,
                                  uint32_t *probes_hz, size_t *probe_count)
{
	AiusJsonCursor cursor = { request->body, request->body_length, 0u };
	const uint8_t *vector = NULL;
	size_t vector_length = 0u;
	bool seen_f0_l = false, seen_f0_r = false, seen_probes = false;

	id[0] = '\0';
	*f0_l_hz = 440u;
	*f0_r_hz = 440u;
	*probe_count = 0u;
	if (request->body == NULL || !body_consume(&cursor, '{'))
		return false;
	for (;;) {
		const uint8_t *key;
		size_t key_length;
		if (!body_take_string(&cursor, &key, &key_length) ||
		    !body_consume(&cursor, ':'))
			return false;
		if (body_key_is(key, key_length, "id")) {
			if (vector != NULL ||
			    !body_take_string(&cursor, &vector, &vector_length))
				return false;
		} else if (body_key_is(key, key_length, "f0_l")) {
			if (seen_f0_l || !body_take_uint(&cursor, f0_l_hz))
				return false;
			seen_f0_l = true;
		} else if (body_key_is(key, key_length, "f0_r")) {
			if (seen_f0_r || !body_take_uint(&cursor, f0_r_hz))
				return false;
			seen_f0_r = true;
		} else if (body_key_is(key, key_length, "probes")) {
			if (seen_probes || !body_consume(&cursor, '['))
				return false;
			seen_probes = true;
			if (!body_consume(&cursor, ']')) {
				do {
					if (*probe_count >= AIUS_OPTICAL_APP_MEASURE_PROBES_MAX ||
					    !body_take_uint(&cursor, &probes_hz[*probe_count]))
						return false;
					(*probe_count)++;
				} while (body_consume(&cursor, ','));
				if (!body_consume(&cursor, ']'))
					return false;
			}
		} else {
			return false;
		}
		if (!body_consume(&cursor, ','))
			break;
	}
	if (!body_consume(&cursor, '}'))
		return false;
	aius_json_skip_whitespace(&cursor);
	if (!aius_json_at_end(&cursor))
		return false;
	if (vector == NULL || !replay_id_chars_valid(vector, vector_length) ||
	    vector_length >= id_capacity)
		return false;
	memcpy(id, vector, vector_length);
	id[vector_length] = '\0';
	return true;
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_measure(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	if (request->route == AIUS_HTTP_ROUTE_MEASURE)
		return http_serialized(dispatcher, kContentJson,
		                       app->write_measure(app->context,
		                                          dispatcher->body,
		                                          dispatcher->body_capacity));
	/* MEASURE_START: the control-route auth gate already ran in the entry
	 * switch. Parsed into locals -- the callback copies synchronously, so
	 * nothing here joins the dispatcher's borrow set. */
	char id[AIUS_OPTICAL_APP_REPLAY_ID_STORAGE];
	uint32_t f0_l_hz, f0_r_hz;
	uint32_t probes_hz[AIUS_OPTICAL_APP_MEASURE_PROBES_MAX];
	size_t probe_count;
	if (!measure_body(request, id, sizeof(id), &f0_l_hz, &f0_r_hz,
	                  probes_hz, &probe_count))
		return http_error(dispatcher, 400, "measure_body");
	if (!app->measure_start(app->context, id, f0_l_hz, f0_r_hz, probes_hz,
	                        probe_count))
		/* Refused: a live capture, or parameters the analysis rejected
		 * (f0/probe outside (0, fs/2)). GET /api/measure says which. */
		return http_error(dispatcher, 409, "measure_start");
	return http_body_text(dispatcher, 200,
		snprintf((char *)dispatcher->body, dispatcher->body_capacity,
		         "{\"ok\":true,\"state\":\"armed\",\"id\":\"%s\"}", id));
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
	AiusOpticalAppDispatcher *dispatcher, const char *content_type,
	uint32_t cache_max_age_s)
{
	AiusHttpApplicationResponse *response =
		sd_job_response(dispatcher, 200, content_type, NULL, 0u);
	response->sd_job.kind = AIUS_SD_JOB_DOWNLOAD;
	response->sd_job.target_path = dispatcher->target_path;
	response->sd_job.content_type = content_type;
	response->cache_max_age_s = cache_max_age_s;
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
	if (!resource_path_valid(request->resource, request->resource_length) ||
	    !build_root_path(dispatcher->target_path,
	                     sizeof(dispatcher->target_path),
	                     (const char *)request->resource,
	                     request->resource_length) ||
	    !build_upload_scratch(dispatcher, true))
		return http_error(dispatcher, 507, "sd_upload");
	return sd_upload(dispatcher, AIUS_SD_INSTALL_REPLACE,
	                 request->content_length, 0u,
	                 (const uint8_t *)"{\"ok\":true}",
	                 sizeof("{\"ok\":true}") - 1u);
}

/* ------------------------------------------------------------------ */
/* directory index                                                     */
/* ------------------------------------------------------------------ */

/* The listing draws a link per entry, so it lists only names the /sd/ download
 * route would accept -- one component's worth of [A-Za-z0-9._-], at most
 * AIUS_OPTICAL_APP_INDEX_NAME_MAX bytes. Everything else is counted as hidden.
 * Excluding the rest is what makes the body HTML-safe without an escaper: no
 * quote, angle bracket or ampersand can survive this test. */
static FLASHMEM bool index_name_servable(const char *name)
{
	size_t length = 0u;
	while (name[length] != '\0') {
		char ch = name[length];
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
			return false;
		length++;
		if (length > AIUS_OPTICAL_APP_INDEX_NAME_MAX)
			return false;
	}
	return length != 0u;
}

/* Neither is an entry a reader wants listed, and neither is "hidden" in the
 * sense the footer reports, so both are dropped uncounted. */
static FLASHMEM bool index_name_is_dot(const char *name)
{
	return name[0] == '.' &&
	       (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/* Bytes held back from the row loop so the footer -- the part that states the
 * counts and whether the listing is complete -- can always be written. Its
 * longest form is 147 bytes, with both counts at 20 digits. */
#define AIUS_OPTICAL_APP_INDEX_FOOTER_RESERVE 192u

/* Formats one listing into the response buffer, returning its length or 0 when
 * even the framing does not fit (which becomes the same deterministic 500 as
 * any other over-large body).
 *
 * The listing is bounded twice over, and states which bound stopped it. The
 * application already stopped at AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES; this loop
 * additionally stops when the next row would eat the footer's reserve, so a
 * dispatcher with a small body buffer truncates honestly instead of overrunning
 * or emitting a 500. Either way `truncated: yes` appears in the body. */
static FLASHMEM size_t index_write(AiusOpticalAppDispatcher *dispatcher,
                                   const AiusHttpRequest *request,
                                   const AiusOpticalAppDirBatch *batch)
{
	char *body = (char *)dispatcher->body;
	size_t capacity = dispatcher->body_capacity;
	size_t length;
	size_t shown = 0u;
	size_t hidden = batch->hidden;
	bool truncated = batch->more;
	int written;

	if (capacity <= AIUS_OPTICAL_APP_INDEX_FOOTER_RESERVE)
		return 0u;
	written = snprintf(body, capacity,
	                   "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	                   "<title>/sd/%.*s</title></head><body><h1>/sd/%.*s</h1>"
	                   "<ul>",
	                   (int)request->resource_length,
	                   (const char *)request->resource,
	                   (int)request->resource_length,
	                   (const char *)request->resource);
	if (written <= 0 ||
	    (size_t)written >= capacity - AIUS_OPTICAL_APP_INDEX_FOOTER_RESERVE)
		return 0u;
	length = (size_t)written;
	/* "../" resolves to the parent listing in the browser, so the link only
	 * exists below the subtree root; from www/ it would resolve to /sd/, which
	 * is not a route. */
	if (request->resource_length > 4u) {
		written = snprintf(body + length, capacity - length,
		                   "<li><a href=\"../\">../</a></li>");
		if (written <= 0 ||
		    (size_t)written >= capacity - length -
			    AIUS_OPTICAL_APP_INDEX_FOOTER_RESERVE)
			return 0u;
		length += (size_t)written;
	}
	for (size_t i = 0; i < batch->count; i++) {
		const AiusOpticalAppDirEntry *entry = &batch->entries[i];
		if (index_name_is_dot(entry->name))
			continue;
		if (!index_name_servable(entry->name)) {
			hidden++;
			continue;
		}
		if (entry->is_directory)
			written = snprintf(body + length, capacity - length,
			                   "<li><a href=\"%s/\">%s/</a> dir</li>",
			                   entry->name, entry->name);
		else
			written = snprintf(body + length, capacity - length,
			                   "<li><a href=\"%s\">%s</a> %llu</li>",
			                   entry->name, entry->name,
			                   (unsigned long long)entry->size_bytes);
		if (written <= 0 ||
		    (size_t)written >= capacity - length -
			    AIUS_OPTICAL_APP_INDEX_FOOTER_RESERVE) {
			truncated = true;
			break;
		}
		length += (size_t)written;
		shown++;
	}
	written = snprintf(body + length, capacity - length,
	                   "</ul><p>%lu shown, %lu hidden, truncated: %s. Sizes in "
	                   "bytes; at most %u entries per listing.</p></body>"
	                   "</html>",
	                   (unsigned long)shown, (unsigned long)hidden,
	                   truncated ? "yes" : "no",
	                   (unsigned)AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES);
	if (written <= 0 || (size_t)written >= capacity - length)
		return 0u;
	return length + (size_t)written;
}

/* GET /sd/<dir>/ -- the /sd/ resource tree describing itself.
 *
 * Order of refusal: the method is a fact about the request, so it answers
 * first; then the capability, the media, the path, and finally the volume. The
 * sd_busy() gate is load-bearing rather than cosmetic -- read_directory reaches
 * the card directly, so it must never run while an SD job owns the volume. */
static FLASHMEM const AiusHttpApplicationResponse *dispatch_sd_index(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	AiusOpticalAppDirBatch batch;

	if (request->method != AIUS_HTTP_GET)
		return http_error(dispatcher, 405, "sd_directory");
	if (app->read_directory == NULL)
		return http_error(dispatcher, 503, "sd_index_unavailable");
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (!resource_directory_valid(request->resource,
	                              request->resource_length) ||
	    !build_root_path(dispatcher->target_path,
	                     sizeof(dispatcher->target_path),
	                     (const char *)request->resource,
	                     request->resource_length - 1u))
		return http_error(dispatcher, 404, "sd_path");
	if (app->sd_busy(app->context))
		return http_error(dispatcher, 503, "sd_busy");

	memset(dispatcher->index_entries, 0, sizeof(dispatcher->index_entries));
	memset(&batch, 0, sizeof(batch));
	batch.entries = dispatcher->index_entries;
	batch.capacity = AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES;
	switch (app->read_directory(app->context, dispatcher->target_path,
	                            &batch)) {
	case AIUS_OPTICAL_APP_DIR_OK:
		break;
	case AIUS_OPTICAL_APP_DIR_NOT_FOUND:
		return http_error(dispatcher, 404, "sd_not_found");
	case AIUS_OPTICAL_APP_DIR_NOT_DIRECTORY:
		return http_error(dispatcher, 404, "sd_not_directory");
	case AIUS_OPTICAL_APP_DIR_FAILED:
	default:
		return http_error(dispatcher, 503, "sd_index");
	}
	/* An application that over-reports is held to the cap here rather than
	 * trusted: the storage is the dispatcher's and so is the bound. */
	if (batch.count > AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES) {
		batch.count = AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES;
		batch.more = true;
	}
	batch.entries = dispatcher->index_entries;
	for (size_t i = 0; i < batch.count; i++)
		dispatcher->index_entries[i].name[AIUS_OPTICAL_APP_INDEX_NAME_MAX] =
			'\0';
	return http_serialized_html(dispatcher,
	                            index_write(dispatcher, request, &batch));
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_sd(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	const char *content_type;
	uint32_t cache_max_age_s = 0u;
	/* A resource naming a directory is a listing on GET and nothing else on any
	 * other method -- it is not an upload target and not a delete target. */
	if (request->route == AIUS_HTTP_ROUTE_SD_FILE &&
	    resource_is_directory(request))
		return dispatch_sd_index(dispatcher, request);
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
		if (!resource_path_valid(request->resource, request->resource_length) ||
		    !build_root_path(dispatcher->target_path,
		                     sizeof(dispatcher->target_path),
		                     (const char *)request->resource,
		                     request->resource_length))
			return http_error(dispatcher, 404, "sd_path");
		content_type = resource_content_type(request);
		cache_max_age_s = resource_cache_max_age(request);
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
		return sd_download(dispatcher, content_type, cache_max_age_s);
	if (request->method == AIUS_HTTP_DELETE)
		return sd_delete(dispatcher);
	return http_error(dispatcher, 405, "sd_method");
}

/* An accepted body becomes the write itself rather than an optimistic 200. The
 * transaction formats this response now but buffers it, entering
 * AIUS_HTTP_TX_SD_START immediately (aius_http_transaction.c:762) and reaching
 * SEND only on AIUS_HTTP_SD_COMPLETE, so the client sees 200 only after the
 * commit and a failed write still surfaces as the pre-cutover 500 sd_commit /
 * 507 sd_write through select_sd_error(). P1a admits the complete buffered body
 * that both these routes carry, provided total equals the body length.
 *
 * publish stays false by construction: the application parsed and validated the
 * model into RAM before accepting, so republishing the written bytes would
 * prove nothing the parser has not already proved.
 *
 * The on-card layout is the pre-cutover one -- /aius/<name> installed through
 * the /aius/.upload-*.tmp + /aius/.backup-*.bak scratch pair that
 * http_sd_upload_begin_target() built via sd_job_make_upload_paths()
 * (optical_main.cpp:1490, :1742, :1794). A name that will not fit keeps that
 * function's only refusal, 503 sd_busy. */
static FLASHMEM const AiusHttpApplicationResponse *store_upload(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request,
	const char *name, size_t name_length)
{
	if (!build_root_path(dispatcher->target_path,
	                     sizeof(dispatcher->target_path), name, name_length) ||
	    !build_upload_scratch(dispatcher, true))
		return http_error(dispatcher, 503, "sd_busy");
	/* total must equal the transaction's body_expected, which is the parsed
	 * Content-Length; a buffered route reaches dispatch only with a complete
	 * body, so that is also request->body_length. */
	return sd_upload(dispatcher, AIUS_SD_INSTALL_REPLACE,
	                 request->content_length, 0u,
	                 (const uint8_t *)"{\"ok\":true}",
	                 sizeof("{\"ok\":true}") - 1u);
}

/* Every refusal stays immediate and reaches the card not at all; only an
 * accepted body becomes a job. */
static FLASHMEM const AiusHttpApplicationResponse *dispatch_store(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request,
	AiusOpticalAppStoreResult result, const char *name, size_t name_length,
	const char *invalid_token)
{
	switch (result) {
	case AIUS_OPTICAL_APP_STORE_ACCEPTED:
		return store_upload(dispatcher, request, name, name_length);
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
	return dispatch_store(dispatcher, request,
	                      app->store_dsp_config(app->context, request->body,
	                                            request->body_length),
	                      AIUS_OPTICAL_APP_DSP_NAME,
	                      sizeof(AIUS_OPTICAL_APP_DSP_NAME) - 1u,
	                      "dsp_config_invalid");
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_config_public(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	return dispatch_store(dispatcher, request,
	                      app->store_public_config(app->context, request->body,
	                                               request->body_length),
	                      AIUS_OPTICAL_APP_CONFIG_NAME,
	                      sizeof(AIUS_OPTICAL_APP_CONFIG_NAME) - 1u,
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
		case AIUS_OPTICAL_APP_REPLAY_CARRIER_PRESENT:
			/* Its own token, not the generic 409 below: the live input owns
			 * the audio path, so this refusal clears when the carrier goes
			 * away rather than when the request changes. */
			return http_error(dispatcher, 409, "replay_carrier_present");
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
		/* The listing enumerates the card directly -- the portable engine has
		 * no directory job to move it onto -- so it is exactly as unsafe
		 * against a running job as the /sd/ index is, and gets the same gate
		 * for the same reason. Phase 14d found this one missing: the listing
		 * could walk /aius/replays while a refill, an upload or a probe held
		 * the volume. A busy volume answers 503 sd_busy, so the caller is told
		 * to retry rather than handed a listing read across someone else's
		 * transaction. */
		if (app->sd_busy(app->context))
			return http_error(dispatcher, 503, "sd_busy");
		return http_serialized(dispatcher, kContentJson,
		                       app->write_replay_list(app->context,
		                                              dispatcher->body,
		                                              dispatcher->body_capacity));
	}
	if (request->method == AIUS_HTTP_DELETE) {
		if (!replay_body_id(dispatcher, request, &loop))
			return http_error(dispatcher, 400, "replay_body");
		/* Same gate, same reason: replay_delete stats and removes on the card
		 * itself. Its cost is a constant two primitives, so the bound was never
		 * the problem here -- the missing exclusion was. Ordered after the body
		 * parse so a malformed request keeps answering 400 whatever the volume
		 * is doing. */
		if (app->sd_busy(app->context))
			return http_error(dispatcher, 503, "sd_busy");
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

/* read_directory is deliberately absent: it is the one optional entry, and a
 * table without it must still init so every other route keeps working on a
 * runtime that has not wired the listing yet. */
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
	       app->write_usb_payload != NULL && app->upload_id != NULL &&
	       app->write_measure != NULL && app->measure_start != NULL;
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

/* The four control routes -- every route that mutates the running system
 * without touching the card. They were the only mutating routes in the whole
 * surface that took no credential at all, so any host that could reach port 80
 * could stop the audio path or reset the board mid-upload. They are now gated on
 * X-Aius-SD-Auth exactly as the SD, replay and config-mutation routes are.
 *
 * The read-only routes -- GET on /, /api/status, /api/events, /api/meters,
 * /api/dsp/config, /api/replays, /api/replays/status and /sd/ -- stay open
 * deliberately: they mutate nothing, and the recovery smoke runner polls
 * /api/status and /api/events to decide whether the board came back. Gating a
 * route whose whole job is to be readable while the board is unhealthy would
 * break the tooling and protect nothing.
 *
 * What the gate is worth, stated honestly: the token is FNV-1a-64 over the
 * public OCOTP MAC through a compiled public salt, and aius_auth.h disclaims
 * cryptographic strength outright. It is a physical-access bootstrap credential.
 * This stops a casual or accidental POST -- a stale browser tab, a scanner, a
 * script pointed at the wrong host -- not an attacker who can see a frame from
 * the board and read this repository. */
static FLASHMEM bool route_is_control(AiusHttpRoute route)
{
	/* MEASURE_START is the fifth: arming a capture mutates the running system
	 * without touching the card, which is exactly this gate's definition. */
	return route == AIUS_HTTP_ROUTE_START ||
	       route == AIUS_HTTP_ROUTE_STOP ||
	       route == AIUS_HTTP_ROUTE_CLEAR ||
	       route == AIUS_HTTP_ROUTE_REBOOT ||
	       route == AIUS_HTTP_ROUTE_MEASURE_START;
}

FLASHMEM const AiusHttpApplicationResponse *aius_optical_app_http_dispatch(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app;
	if (dispatcher == NULL || !dispatcher->valid || request == NULL)
		return NULL;
	app = &dispatcher->app;
	dispatcher->http_dispatches++;
	/* Before the switch rather than inside each of the four arms: the refusal
	 * must reach the client without the effect having happened, and one gate
	 * cannot be forgotten by a fifth control route added later. */
	if (route_is_control(request->route) &&
	    !app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
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
	case AIUS_HTTP_ROUTE_MEASURE:
	case AIUS_HTTP_ROUTE_MEASURE_START:
		return dispatch_measure(dispatcher, request);
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
		/* The reset itself belongs to the runtime. Dispatch records the request
		 * and returns; it never resets and never delays.
		 *
		 * "After the response has been written" is what this comment used to
		 * claim, and it was wrong. Returning here only retains a pending
		 * application event: format_response() runs inside accept_application()
		 * on the NEXT transaction step, so at this point no byte of the 200
		 * exists yet, let alone reached the socket. The runtime must
		 * therefore gate the reset on the transaction draining to
		 * AIUS_HTTP_TX_IDLE, not on the epoch that dispatched finishing --
		 * optical_main.cpp reboot_drain_complete(). */
		app->request_reboot(app->context);
		return http_json(dispatcher, 200, "{\"ok\":true,\"rebooting\":true}");
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
