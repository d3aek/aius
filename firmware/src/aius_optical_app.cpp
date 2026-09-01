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

/* The one /sd/ resource served from outside www/: a GET-only carve-out for the
 * persistent DSP model, so the card's authoritative bytes can be retrieved
 * before a restorable PUT (aius-e2e-latency.md fails its bypass row closed
 * without them; no other route returns the file rather than the effective
 * runtime model). config.json gets no such carve-out -- it is still write-only
 * over the wire, and /api/status already serializes what it configures.
 *
 * It is an ALIAS for AIUS_OPTICAL_APP_DSP_NAME under the root above, not a path
 * composed through it. /sd/ is rooted at /aius/, so this resource read as an
 * ordinary tree path would name /aius/aius/dsp.json, which is not a file the
 * board ever writes; the spelling is fixed by the URL the latency runner
 * preflights. Matching is therefore exact-literal (resource_is_dsp_model), and
 * the target is built from the name rather than from the request bytes. */
#define AIUS_OPTICAL_APP_DSP_RESOURCE "aius/" AIUS_OPTICAL_APP_DSP_NAME

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

/* The read carve-out's whole gate: one exact-length, exact-byte comparison
 * against AIUS_OPTICAL_APP_DSP_RESOURCE. No prefix test and no component walk,
 * so no other spelling reaches the root through it -- a traversal, a trailing
 * slash, a case change or an extra byte is simply a different literal, and falls
 * through to the www/-only rules below. */
static FLASHMEM bool resource_is_dsp_model(const AiusHttpRequest *request)
{
	size_t length = sizeof(AIUS_OPTICAL_APP_DSP_RESOURCE) - 1u;
	return request->resource != NULL && request->resource_length == length &&
	       memcmp(request->resource, AIUS_OPTICAL_APP_DSP_RESOURCE,
	              length) == 0;
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
 * same bytes with the trailing slash removed.
 *
 * Still www/-only, carve-out included: AIUS_OPTICAL_APP_DSP_RESOURCE is matched
 * before this runs and never passes through it, so this stays the sole rule for
 * every path the route composes from request bytes -- which is every upload,
 * every delete and every listing. The carve-out widens reads by exactly one
 * file and widens writes not at all. */
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
 * exactly the same TREE.
 *
 * The download route's single-file carve-out is the one thing they no longer
 * agree on, and deliberately: it is not a directory, it is not under www/, and
 * no listing links to it -- "/sd/aius/" and "/sd/aius/dsp.json/" are both
 * ordinary 404 sd_path here. A reader reaches the model by knowing its URL, not
 * by browsing to it. */
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

/* Strict signed decibel value for the recorder config: "0", or "-" followed
 * by up to three digits and an optional single fraction digit ("-6", "-6.5").
 * A GR threshold has no business carrying exponents, plus signs or deeper
 * precision, and the bounded grammar keeps this a scanner, not strtod. */
static FLASHMEM bool body_take_db(AiusJsonCursor *cursor, float *value)
{
	aius_json_skip_whitespace(cursor);
	bool negative = false;
	uint32_t whole = 0;
	uint32_t frac = 0;
	size_t digits = 0;
	if (cursor->pos < cursor->len && cursor->data[cursor->pos] == '-') {
		negative = true;
		cursor->pos++;
	}
	while (cursor->pos < cursor->len) {
		uint8_t ch = cursor->data[cursor->pos];
		if (ch < '0' || ch > '9')
			break;
		if (++digits > 3u)
			return false;
		whole = whole * 10u + (uint32_t)(ch - '0');
		cursor->pos++;
	}
	if (digits == 0)
		return false;
	if (cursor->pos < cursor->len && cursor->data[cursor->pos] == '.') {
		cursor->pos++;
		if (cursor->pos >= cursor->len)
			return false;
		uint8_t ch = cursor->data[cursor->pos];
		if (ch < '0' || ch > '9')
			return false;
		frac = (uint32_t)(ch - '0');
		cursor->pos++;
	}
	float parsed = (float)whole + (float)frac / 10.0f;
	*value = negative ? -parsed : parsed;
	return true;
}

/* PUT /api/recorder/config body, with the measure parser's strictness turned
 * all the way up: every key REQUIRED exactly once, any order, nothing else --
 * {"overrun":bool,"underrun":bool,"carrier_loss":bool,"limiter_gr":bool,
 *  "gr_threshold_db":<0|-n[.n]>,"gr_hold_blocks":<uint>,
 *  "postroll_blocks":<uint>}. Requiring the full set is the same
 * anti-drift stance as the DSP PUT's all-eight-keys rule: a partial config
 * that silently keeps unstated fields is how two clients disagree about what
 * is armed. */
static FLASHMEM bool recorder_config_body(const AiusHttpRequest *request,
                                          uint32_t *mask,
                                          float *gr_threshold_db,
                                          uint32_t *gr_hold_blocks,
                                          uint32_t *postroll_blocks)
{
	AiusJsonCursor cursor = { request->body, request->body_length, 0u };
	static const struct {
		const char *key;
		uint32_t bit;
	} flags[] = {
		{ "overrun", AIUS_OPTICAL_APP_RECORDER_MASK_OVERRUN },
		{ "underrun", AIUS_OPTICAL_APP_RECORDER_MASK_UNDERRUN },
		{ "carrier_loss", AIUS_OPTICAL_APP_RECORDER_MASK_CARRIER_LOSS },
		{ "limiter_gr", AIUS_OPTICAL_APP_RECORDER_MASK_LIMITER_GR },
	};
	bool seen[7] = { false, false, false, false, false, false, false };

	*mask = 0u;
	*gr_threshold_db = 0.0f;
	*gr_hold_blocks = 0u;
	*postroll_blocks = 0u;
	if (request->body == NULL || !body_consume(&cursor, '{'))
		return false;
	for (;;) {
		const uint8_t *key;
		size_t key_length;
		size_t matched = sizeof(seen) / sizeof(seen[0]);
		if (!body_take_string(&cursor, &key, &key_length) ||
		    !body_consume(&cursor, ':'))
			return false;
		for (size_t flag = 0; flag < 4u; flag++) {
			if (!body_key_is(key, key_length, flags[flag].key))
				continue;
			matched = flag;
			if (body_take_literal(&cursor, "true"))
				*mask |= flags[flag].bit;
			else if (!body_take_literal(&cursor, "false"))
				return false;
			break;
		}
		if (matched > 4u && body_key_is(key, key_length, "gr_threshold_db")) {
			matched = 4;
			if (!body_take_db(&cursor, gr_threshold_db))
				return false;
		} else if (matched > 4u &&
		           body_key_is(key, key_length, "gr_hold_blocks")) {
			matched = 5;
			if (!body_take_uint(&cursor, gr_hold_blocks))
				return false;
		} else if (matched > 4u &&
		           body_key_is(key, key_length, "postroll_blocks")) {
			matched = 6;
			/* body_take_uint refuses 0, but a zero post-roll is a
			 * legal "freeze at the trigger boundary": take the
			 * literal 0 explicitly first. */
			if (body_take_literal(&cursor, "0"))
				*postroll_blocks = 0u;
			else if (!body_take_uint(&cursor, postroll_blocks))
				return false;
		}
		if (matched >= sizeof(seen) / sizeof(seen[0]) || seen[matched])
			return false;
		seen[matched] = true;
		if (!body_consume(&cursor, ','))
			break;
	}
	if (!body_consume(&cursor, '}'))
		return false;
	aius_json_skip_whitespace(&cursor);
	if (!aius_json_at_end(&cursor))
		return false;
	for (size_t i = 0; i < sizeof(seen) / sizeof(seen[0]); i++)
		if (!seen[i])
			return false;
	/* A positive threshold is a malformed VALUE, not a state refusal: the
	 * scanner accepts bare digits, so the sign rule lands here (400), and
	 * the recorder lib's own check never has to answer for the wire. */
	if (*gr_threshold_db > 0.0f)
		return false;
	return true;
}

/* The transaction's completion hook for a recorder-data read (registered on the
 * READ_OK immediate response). The 200 borrows a pointer into one bank slot's
 * PSRAM ring for the streamed body's lifetime; this fires EXACTLY ONCE when the
 * response has drained and the transaction recycles -- or when a mid-stream
 * abort tears the connection down -- and drops that slot's read lease. That is
 * the true end of the borrow, reached with no follow-up request, which is what a
 * one-shot client (a single curl / pull-tool GET that closes after its 200)
 * needs: the round-robin must be able to recycle the slot again. `committed` is
 * meaningless for an immediate response and ignored. */
static FLASHMEM void recorder_read_settle(void *ctx, bool committed)
{
	AiusOpticalAppDispatcher *dispatcher = (AiusOpticalAppDispatcher *)ctx;
	(void)committed;
	if (dispatcher == NULL || !dispatcher->recorder_lease_active)
		return;
	dispatcher->recorder_lease_active = false;
	dispatcher->app.recorder_read_release(dispatcher->app.context,
	                                      dispatcher->recorder_lease_slot);
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_recorder(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	/* EVERY recorder route is token-gated, the GETs included -- the data
	 * read carries recorded audio and the header reveals trigger state
	 * (contract §HTTP surface; aius_auth.h's weak-boundary disclaimer
	 * applies as everywhere else). */
	if (!app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	switch (request->route) {
	case AIUS_HTTP_ROUTE_RECORDER:
		return http_serialized(dispatcher, kContentJson,
		                       app->write_recorder(app->context,
		                                           dispatcher->body,
		                                           dispatcher->body_capacity));
	case AIUS_HTTP_ROUTE_RECORDER_FREEZE:
		return app->recorder_freeze(app->context)
			? http_json(dispatcher, 200,
			            "{\"ok\":true,\"state\":\"freezing\"}")
			: http_error(dispatcher, 409, "recorder_state");
	case AIUS_HTTP_ROUTE_RECORDER_THAW:
		return app->recorder_thaw(app->context)
			? http_json(dispatcher, 200,
			            "{\"ok\":true,\"state\":\"recording\"}")
			: http_error(dispatcher, 409, "recorder_state");
	case AIUS_HTTP_ROUTE_RECORDER_CONFIG: {
		uint32_t mask, hold, postroll;
		float gr_db;
		if (!recorder_config_body(request, &mask, &gr_db, &hold,
		                          &postroll))
			return http_error(dispatcher, 400, "recorder_body");
		return app->recorder_config(app->context, mask, gr_db, hold,
		                            postroll)
			? http_json(dispatcher, 200, "{\"ok\":true}")
			: http_error(dispatcher, 409, "recorder_state");
	}
	case AIUS_HTTP_ROUTE_RECORDER_DATA: {
		const uint8_t *data = NULL;
		uint32_t length = 0u;
		uint32_t resolved_slot = 0u;
		/* The 200 borrows PSRAM directly: stable because data resolves
		 * only in FROZEN and a thaw is necessarily a later transaction
		 * (single-transaction model) -- no copy, no SD job, and
		 * deliberately NO app_sd_busy test (the refill owns ~61 % of
		 * epochs during replay; an sd_busy gate would make the
		 * recorder unusable exactly when it matters). READ_OK takes the
		 * addressed slot's read lease (in recorder_read); the response's
		 * completion hook (recorder_read_settle) drops it when the borrow
		 * ends -- at genuine completion/close, no follow-up request
		 * required. */
		switch (app->recorder_read(app->context, request->recorder_has_slot,
		                           request->recorder_slot,
		                           request->recorder_offset,
		                           request->recorder_length,
		                           request->recorder_generation, &data,
		                           &length, &resolved_slot)) {
		case AIUS_OPTICAL_APP_RECORDER_READ_OK: {
			const AiusHttpApplicationResponse *response =
				http_immediate(dispatcher, 200, kContentBinary, data,
				               length);
			dispatcher->recorder_lease_active = true;
			dispatcher->recorder_lease_slot = resolved_slot;
			dispatcher->http_response.sd_settle = recorder_read_settle;
			dispatcher->http_response.sd_settle_ctx = dispatcher;
			return response;
		}
		case AIUS_OPTICAL_APP_RECORDER_READ_INVALID:
			return http_error(dispatcher, 400, "recorder_read");
		case AIUS_OPTICAL_APP_RECORDER_READ_REFUSED:
		default:
			return http_error(dispatcher, 409, "recorder_state");
		}
	}
	default:
		return http_error(dispatcher, 405, "recorder_method");
	}
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
	} else if (request->method == AIUS_HTTP_GET &&
	           resource_is_dsp_model(request)) {
		/* GET-only by construction, which is what keeps the carve-out a read:
		 * a PUT never arrives here (it left for dispatch_sd_upload above, and
		 * is refused there by the www/-only path rule), and a DELETE falls
		 * through to the same rule below. The target is built from the name,
		 * never from the request bytes -- the resource is an alias, not a path.
		 *
		 * Content type and caching come from the shared suffix policy rather
		 * than from constants here, so ".json" answers application/json
		 * uncached exactly as a .json download under www/ does, and a later
		 * change to that policy cannot leave this one row behind. */
		if (!build_root_path(dispatcher->target_path,
		                     sizeof(dispatcher->target_path),
		                     AIUS_OPTICAL_APP_DSP_NAME,
		                     sizeof(AIUS_OPTICAL_APP_DSP_NAME) - 1u))
			return http_error(dispatcher, 404, "sd_path");
		content_type = resource_content_type(request);
		cache_max_age_s = resource_cache_max_age(request);
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
/* OTA -- staged firmware upgrade                                      */
/* ------------------------------------------------------------------ */

/* The verdict map: one row per AiusOpticalAppOtaResult, in enum order, holding
 * the refusal's HTTP status AND its wire token together. They are allocated in
 * one place because they are one decision -- a verdict that answered 409
 * ota_state on a refusal and printed a different name in the status document
 * would describe two different failures to the same operator.
 *
 * Indexed rather than switched precisely so the two stay paired: a new verdict
 * is one row rather than two arms in two separate switches that could disagree.
 * A verdict with no row at all -- an application built against a longer enum --
 * is bounded below rather than read off the end. */
static const struct {
	unsigned status;
	const char *token;
} kOtaVerdicts[] = {
	{ 200u, "none" },            /* OK */
	{ 409u, "ota_disarmed" },    /* DISARMED */
	{ 409u, "ota_state" },       /* STATE_REFUSED */
	{ 409u, "ota_range" },       /* RANGE */
	{ 400u, "ota_malformed" },   /* MALFORMED */
	{ 400u, "ota_unsupported" }, /* UNSUPPORTED */
	{ 409u, "ota_digest" },      /* DIGEST */
	{ 409u, "ota_signature" },   /* SIGNATURE */
	{ 507u, "ota_storage" },     /* STORAGE */
	{ 409u, "ota_activation_disabled" }, /* ACTIVATION_DISABLED */
	{ 500u, "ota_self_test_failed" }     /* SELF_TEST_FAILED */
};

#define AIUS_OPTICAL_APP_OTA_VERDICTS \
	(sizeof(kOtaVerdicts) / sizeof(kOtaVerdicts[0]))

/* An index outside the table is a verdict from a build this dispatcher was not
 * compiled against; it becomes the generic state refusal rather than a read off
 * the end. */
static FLASHMEM size_t ota_verdict_row(AiusOpticalAppOtaResult result)
{
	size_t row = (size_t)result;
	return row < AIUS_OPTICAL_APP_OTA_VERDICTS
		? row : (size_t)AIUS_OPTICAL_APP_OTA_STATE_REFUSED;
}

static FLASHMEM const AiusHttpApplicationResponse *ota_refusal(
	AiusOpticalAppDispatcher *dispatcher, AiusOpticalAppOtaResult result)
{
	size_t row = ota_verdict_row(result);
	return http_error(dispatcher, kOtaVerdicts[row].status,
	                  kOtaVerdicts[row].token);
}

/* The three verdict-only verbs: an accepted call answers its own constant, and
 * every refusal goes through the one map. */
static FLASHMEM const AiusHttpApplicationResponse *ota_verdict_answer(
	AiusOpticalAppDispatcher *dispatcher, AiusOpticalAppOtaResult result,
	const char *accepted)
{
	if (result == AIUS_OPTICAL_APP_OTA_OK)
		return http_json(dispatcher, 200, accepted);
	return ota_refusal(dispatcher, result);
}

static FLASHMEM const char *ota_state_name(AiusOpticalAppOtaState state)
{
	switch (state) {
	case AIUS_OPTICAL_APP_OTA_STATE_IDLE:
		return "idle";
	case AIUS_OPTICAL_APP_OTA_STATE_OFFERED:
		return "offered";
	case AIUS_OPTICAL_APP_OTA_STATE_STAGING:
		return "staging";
	case AIUS_OPTICAL_APP_OTA_STATE_STAGED:
		return "staged";
	case AIUS_OPTICAL_APP_OTA_STATE_VERIFIED:
		return "verified";
	case AIUS_OPTICAL_APP_OTA_STATE_ARMED:
		return "armed";
	case AIUS_OPTICAL_APP_OTA_STATE_ACTIVATION_REQUESTED:
		return "activation_requested";
	case AIUS_OPTICAL_APP_OTA_STATE_FAILED:
		return "failed";
	default:
		break;
	}
	/* A state this build has no name for. Named rather than printed as a
	 * number, so a page renders something a human can read instead of a
	 * bare integer it would have to decode. */
	return "unknown";
}

/* Copies one identity field into the document's storage, or leaves it EMPTY.
 *
 * The document draws these straight into JSON, so it publishes only what it can
 * draw: any byte outside printable ASCII, and either of the two characters that
 * would close or escape a JSON string, empties the WHOLE field. Escaping was
 * rejected for the reason the /sd/ index rejects it -- there is no escaper here
 * and adding one to make a hostile identity representable buys nothing, because
 * an identity is a claim and a partially rewritten claim is a different claim.
 * An over-long field (an application that filled its storage without a
 * terminator, say) empties for the same reason rather than truncating.
 *
 * `in` and `out` are both AIUS_OPTICAL_APP_OTA_TEXT_STORAGE wide, so the walk
 * is bounded by `capacity` and never reads past the status struct's field. */
static FLASHMEM void ota_text_copy(char *out, size_t capacity, const char *in)
{
	size_t length = 0u;
	out[0] = '\0';
	while (length + 1u < capacity && in[length] != '\0') {
		unsigned char ch = (unsigned char)in[length];
		if (ch < 0x20u || ch > 0x7eu || ch == '"' || ch == '\\')
			return;
		length++;
	}
	if (in[length] != '\0')
		return; /* no terminator inside the field: publish nothing */
	memcpy(out, in, length);
	out[length] = '\0';
}

/* GET /api/ota -- the open read, and the only OTA route that takes no token.
 *
 * It stays open for the reason /api/status and /api/events do: it mutates
 * nothing, and a page that cannot read it cannot render the disabled or
 * TEST KEY state the plan requires it to show. It discloses that this build
 * carries OTA, which the 404 on an absent build discloses anyway.
 *
 * "activation_available" is the runtime latch the application supplies through
 * the status struct: it is false at every boot and true only after the operator
 * has flipped it (POST /api/ota/enable-activation) following the bench
 * scratch-sector recovery proof. A page reads it to decide whether to offer the
 * activate step. */
static FLASHMEM const AiusHttpApplicationResponse *ota_status_document(
	AiusOpticalAppDispatcher *dispatcher)
{
	const AiusOpticalAppOta *ota = &dispatcher->ota;
	AiusOpticalAppOtaStatus status;
	char target_id[AIUS_OPTICAL_APP_OTA_TEXT_STORAGE];
	char build_revision[AIUS_OPTICAL_APP_OTA_TEXT_STORAGE];
	char fingerprint[AIUS_OPTICAL_APP_OTA_TEXT_STORAGE];

	memset(&status, 0, sizeof(status));
	if (!ota->status(ota->context, &status))
		return http_error(dispatcher, 503, "ota_status");
	ota_text_copy(target_id, sizeof(target_id), status.target_id);
	ota_text_copy(build_revision, sizeof(build_revision),
	              status.build_revision);
	ota_text_copy(fingerprint, sizeof(fingerprint), status.key_fingerprint);
	return http_body_text(dispatcher, 200,
		snprintf((char *)dispatcher->body, dispatcher->body_capacity,
		         "{\"armed\":%s,\"state\":\"%s\",\"staged_bytes\":%lu,"
		         "\"total_bytes\":%lu,\"next_offset\":%lu,"
		         "\"image\":{\"target_id\":\"%s\",\"build_revision\":\"%s\","
		         "\"build_utc\":%llu},"
		         "\"key\":{\"fingerprint\":\"%s\",\"test_key\":%s},"
		         "\"activation_available\":%s,"
		         "\"fault\":\"%s\",\"verifier_fault\":%d}",
		         status.latch_armed ? "true" : "false",
		         ota_state_name(status.state),
		         (unsigned long)status.staged_bytes,
		         (unsigned long)status.total_bytes,
		         (unsigned long)status.next_offset,
		         target_id, build_revision,
		         (unsigned long long)status.build_utc,
		         fingerprint, status.test_key ? "true" : "false",
		         status.activation_available ? "true" : "false",
		         kOtaVerdicts[ota_verdict_row(status.fault)].token,
		         status.verifier_fault));
}

/* PUT /api/ota/image -- the APPEND_PART protocol, verb for verb.
 *
 * Every request is self-contained: Content-Range says where these bytes belong,
 * the body carries them whole, and the answer states the next offset. So a
 * dropped connection costs one chunk and the client resumes from what it was
 * last told -- which is the property a multi-megabyte firmware push over one
 * socket needs, and the reason this mirrors the replay upload rather than
 * inventing a session.
 *
 * DURABLE STAGING, ABSORB DEFERRED TO THE COMMIT (aius-ota-resilience.md
 * Phase 4a). On an accepted chunk the route writes the chunk's bytes to the
 * fixed staging file /aius/ota-staged.img through the existing bounded SD job
 * engine (AIUS_SD_INSTALL_APPEND_PART at the chunk's file offset, exactly the
 * replay APPEND_PART upload), so the container survives to activation. The
 * 200 is the SD job's -- withheld until the write commits.
 *
 * The session's absorb no longer runs here. It used to run synchronously
 * BEFORE the write, and a chunk whose write then faulted left the session
 * hash ahead of the card: the client's resume (the session's next_offset)
 * skipped the hole, the eventual verify passed on bytes the card never got,
 * and only activation's pre-window re-digest convicted it -- a doomed
 * session whose only remedy was a full restage (the 2026-08-16 incident's
 * "poisoned session"). Now the route asks the PURE chunk_check for the exact
 * refusal absorb would earn, stashes the accepted chunk on the dispatcher,
 * and the transaction's sd_settle hook runs the real absorb at the write's
 * COMMITTED terminal (the borrowed body is still intact there). A faulted
 * write settles uncommitted: the session has not moved, and the 500/503 the
 * client sees is honestly resumable. The pre-formatted ack can promise
 * next_offset arithmetically -- the check pinned the chunk's offset to the
 * session's next expected one. */
FLASHMEM void aius_optical_app_ota_pending_clear(
	AiusOpticalAppDispatcher *dispatcher)
{
	if (dispatcher == NULL)
		return;
	dispatcher->ota_pending = false;
	dispatcher->ota_pending_offset = 0u;
	dispatcher->ota_pending_data = NULL;
	dispatcher->ota_pending_length = 0u;
}

/* The transaction's settle hook for a staged OTA chunk: the deferred absorb.
 * Runs the REAL chunk mutation only at the write's COMMITTED terminal, so a
 * faulted or cancelled write leaves the session untouched and the client's
 * retry genuinely resumable. The absorb's own verdict is deliberately
 * unanswered -- the 200 has already been promised -- and the one way it can
 * refuse after a passing precheck is a session that moved between dispatch
 * and terminal (an abort raced the write), where the client's next chunk
 * earns a RANGE with the honest next_offset and self-heals. */
static FLASHMEM void ota_chunk_settle(void *ctx, bool committed)
{
	AiusOpticalAppDispatcher *dispatcher = (AiusOpticalAppDispatcher *)ctx;
	uint32_t next_offset = 0u;

	if (dispatcher == NULL || !dispatcher->ota_pending)
		return;
	if (committed && dispatcher->ota.chunk != NULL)
		(void)dispatcher->ota.chunk(dispatcher->ota.context,
		                            dispatcher->ota_pending_offset,
		                            dispatcher->ota_pending_data,
		                            dispatcher->ota_pending_length,
		                            &next_offset);
	aius_optical_app_ota_pending_clear(dispatcher);
}

static FLASHMEM const AiusHttpApplicationResponse *ota_image_chunk(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalAppOta *ota = &dispatcher->ota;
	const AiusOpticalApp *app = &dispatcher->app;
	uint32_t next_offset = 0u;
	uint64_t span;
	AiusOpticalAppOtaResult result;
	int written;

	/* 1. A range and a body. Without the range the chunk has no offset to be
	 * at, which is the same refusal a Content-Range-less replay PUT gets. */
	if (!request->has_content_range || request->body == NULL ||
	    request->body_length == 0u)
		return http_error(dispatcher, 400, "ota_chunk");
	/* 2. The range describes THESE bytes. The parser has already checked
	 * start <= end < total; that the span equals what arrived is this
	 * route's check, and it is what stops a client from claiming an offset
	 * range it did not send. */
	span = (uint64_t)request->content_range_end -
	       (uint64_t)request->content_range_start + 1u;
	if (span != (uint64_t)request->body_length)
		return http_error(dispatcher, 400, "ota_chunk");
	/* 3. One chunk is one bounded epoch's worth of hashing. Refused here
	 * rather than clamped: a truncated chunk would break the strict
	 * contiguous contract the core enforces on the next offset. */
	if (request->body_length > AIUS_OPTICAL_APP_OTA_MAX_CHUNK_BYTES)
		return http_error(dispatcher, 400, "ota_chunk");
	/* 4. HEADER FIRST. The container header is a fixed 256 bytes at offset
	 * 0, and the core validates it whole -- magic, CRC, identity, digest and
	 * signature slot together -- so half a header is not an early fragment
	 * of a valid offer, it is an offer nothing can validate. Refusing it at
	 * the route keeps that a request-shape error (400) instead of a
	 * state-machine failure the operator would have to abort out of. */
	if (request->content_range_start == 0u &&
	    request->body_length < AIUS_OPTICAL_APP_OTA_HEADER_BYTES)
		return http_error(dispatcher, 400, "ota_header");
	/* 5. The card, before the check: durable staging needs it, and a chunk
	 * it cannot take must not advance the session's offset. A busy volume
	 * is 503 sd_busy -- the retryable vocabulary every other SD route
	 * speaks -- NOT 507: mid-stream chunks legitimately collide with root
	 * SD jobs holding the volume (convicted 2026-08-04, three collisions
	 * in ~1,150 staged chunks, the defect-2 "1 in 15 large uploads"
	 * arithmetic), and answering "the staging medium refused" for a
	 * transient hold sent clients into failure paths a plain retry
	 * resolves. 507 remains the genuine storage refusals below. */
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (app->sd_busy(app->context))
		return http_error(dispatcher, 503, "sd_busy");
	/* 6. One absorb in flight at a time: while a promised chunk awaits its
	 * write's terminal (a one-epoch window), a second chunk could not be
	 * prechecked against the session the pending absorb is about to
	 * advance. The same retryable answer as a busy volume. */
	if (dispatcher->ota_pending)
		return http_error(dispatcher, 503, "sd_busy");
	result = ota->chunk_check(ota->context, request->content_range_start,
	                          request->body, request->body_length,
	                          &next_offset);
	/* A range refusal is the one that carries an instruction rather than an
	 * apology, so it answers with next_offset beside the error: the client
	 * seeks there and continues instead of restarting a multi-megabyte
	 * upload. It does NOT fail the session -- that is the transport's
	 * contract and the core keeps it. And it reaches the card not at all. */
	if (result == AIUS_OPTICAL_APP_OTA_RANGE)
		return http_body_text(dispatcher, 409,
			snprintf((char *)dispatcher->body, dispatcher->body_capacity,
			         "{\"ok\":false,\"error\":\"ota_range\","
			         "\"next_offset\":%lu}",
			         (unsigned long)next_offset));
	if (result != AIUS_OPTICAL_APP_OTA_OK)
		return ota_refusal(dispatcher, result);
	/* Accepted: stage this chunk durably. The 200 becomes the SD job's, its
	 * body the next_offset ack pre-formatted here -- arithmetically, since
	 * the committed absorb of THIS chunk lands the file cursor at exactly
	 * start + length. APPEND_PART writes body_length bytes at the container
	 * file offset, so a dropped connection still costs one chunk. */
	if (!build_root_path(dispatcher->target_path,
	                     sizeof(dispatcher->target_path),
	                     AIUS_OPTICAL_APP_OTA_STAGED_NAME,
	                     sizeof(AIUS_OPTICAL_APP_OTA_STAGED_NAME) - 1u))
		return http_error(dispatcher, 507, "ota_storage");
	written = snprintf((char *)dispatcher->body, dispatcher->body_capacity,
	                   "{\"ok\":true,\"next_offset\":%lu}",
	                   (unsigned long)(request->content_range_start +
	                                   (uint32_t)request->body_length));
	if (written <= 0 || (size_t)written >= dispatcher->body_capacity)
		return http_too_large(dispatcher);
	/* THE FIRST CHUNK RESTARTS THE FILE. The staged image persists across
	 * upgrades (and across an interrupted one), and APPEND_PART's
	 * strict-append contract would refuse offset 0 against it forever --
	 * 416 sd_range with no route that can remove the file. So offset 0
	 * stages through the atomic REPLACE pair (truncate by rename, the same
	 * scratch/backup dance every web-tree PUT uses), mirroring the
	 * session-level rule above where a re-sent offset 0 is a deliberate
	 * restart; every later chunk appends. */
	{
		const AiusHttpApplicationResponse *response;
		if (request->content_range_start == 0u) {
			if (!build_upload_scratch(dispatcher, true))
				return http_error(dispatcher, 507, "ota_storage");
			response = sd_upload(dispatcher, AIUS_SD_INSTALL_REPLACE,
			                     request->content_length, 0u,
			                     dispatcher->body, (size_t)written);
		} else {
			response = sd_upload(dispatcher,
			                     AIUS_SD_INSTALL_APPEND_PART,
			                     request->content_length,
			                     request->content_range_start,
			                     dispatcher->body, (size_t)written);
		}
		/* The promise: absorb at the COMMITTED terminal, through the
		 * transaction's settle hook. The stashed data borrows the
		 * request body, which outlives the job (the hook fires before
		 * the owning transaction recycles). */
		dispatcher->ota_pending = true;
		dispatcher->ota_pending_offset = request->content_range_start;
		dispatcher->ota_pending_data = request->body;
		dispatcher->ota_pending_length = request->body_length;
		dispatcher->http_response.sd_settle = ota_chunk_settle;
		dispatcher->http_response.sd_settle_ctx = dispatcher;
		return response;
	}
}

static FLASHMEM bool ota_wired(const AiusOpticalAppOta *ota)
{
	return ota->present != NULL && ota->status != NULL &&
	       ota->chunk != NULL && ota->chunk_check != NULL &&
	       ota->arm != NULL && ota->verify != NULL &&
	       ota->abort != NULL && ota->activate != NULL &&
	       ota->enable_activation != NULL && ota->self_test_flash != NULL &&
	       ota->rehearse != NULL;
}

/* Both gates run BEFORE the switch, for the reason route_is_control()'s gate
 * does: a sixth verb added later cannot forget them.
 *
 * Absent first. A build without OTA -- compiled out, or wired but reporting
 * itself unavailable -- answers one 404 for every verb, so no route can be the
 * one that leaks a half-built surface, and the page gets a single fact to
 * render its disabled state from.
 *
 * Then the token, for everything except the open read. Firmware replacement is
 * the most dangerous surface this device has (the plan's §Security), so the
 * four mutating verbs carry the same credential the control routes do. What
 * that is worth is stated exactly where route_is_control() states it: the token
 * is FNV-1a-64 over the public MAC, a physical-access bootstrap credential that
 * stops a stale tab or a scanner. THE IMAGE SIGNATURE, not this gate, is what
 * makes remote replacement defensible -- which is why nothing here can activate
 * an image, and why verify() runs the signature through the core. */
static FLASHMEM const AiusHttpApplicationResponse *dispatch_ota(
	AiusOpticalAppDispatcher *dispatcher, AiusOpticalAppOtaRoute route,
	const AiusHttpRequest *request)
{
	const AiusOpticalAppOta *ota = &dispatcher->ota;
	const AiusOpticalApp *app = &dispatcher->app;

	if (!ota_wired(ota) || !ota->present(ota->context))
		return http_error(dispatcher, 404, "ota_absent");
	if (route != AIUS_OPTICAL_APP_OTA_ROUTE_STATUS &&
	    !app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	switch (route) {
	case AIUS_OPTICAL_APP_OTA_ROUTE_STATUS:
		return ota_status_document(dispatcher);
	case AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE:
		return ota_image_chunk(dispatcher, request);
	case AIUS_OPTICAL_APP_OTA_ROUTE_ARM:
		/* The feature latch, not the image: arming makes an upgrade
		 * possible, it does not start one. */
		return ota_verdict_answer(dispatcher, ota->arm(ota->context),
		                          "{\"ok\":true,\"armed\":true}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY:
		return ota_verdict_answer(dispatcher, ota->verify(ota->context),
		                          "{\"ok\":true,\"state\":\"verified\"}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_ABORT:
		return ota_verdict_answer(dispatcher, ota->abort(ota->context),
		                          "{\"ok\":true,\"state\":\"idle\"}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE:
		/* The composition gates this on the runtime latch and ARMED state and,
		 * on acceptance, commits to quiesce + program + SYSRESETREQ. The 200 is
		 * the acknowledgement the client sees before the board resets into the
		 * new image; the reset is deferred past the response drain, as reboot. */
		return ota_verdict_answer(dispatcher, ota->activate(ota->context),
		                          "{\"ok\":true,\"activating\":true}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_ENABLE_ACTIVATION:
		return ota_verdict_answer(dispatcher,
		                          ota->enable_activation(ota->context),
		                          "{\"ok\":true,\"activation_enabled\":true}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST:
		/* OK is the pass verdict; a mismatch is 500 ota_self_test_failed and a
		 * latch-off is 409 ota_activation_disabled, both through the verdict
		 * map. The pass/fail also lands in ota_diag for a status poller. */
		return ota_verdict_answer(dispatcher,
		                          ota->self_test_flash(ota->context),
		                          "{\"ok\":true,\"self_test_flash\":\"pass\"}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE:
		/* Accepted-and-deferred, like activate: the 200 drains, then loop()
		 * runs the full sequence against the scratch window with the reset
		 * emulated. The verdict lands in ota_diag when the board resumes. */
		return ota_verdict_answer(dispatcher,
		                          ota->rehearse(ota->context, false),
		                          "{\"ok\":true,\"rehearsing\":true}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT:
		return ota_verdict_answer(
			dispatcher, ota->rehearse(ota->context, true),
			"{\"ok\":true,\"rehearsing\":true,\"fault_injection\":true}");
	case AIUS_OPTICAL_APP_OTA_ROUTE_NONE:
	default:
		break;
	}
	/* Not an OTA verb. Unreachable from a parsed request once the routes are
	 * wired -- the parser owns method rules per path -- so this is the
	 * deterministic answer to a caller that named no verb. */
	return http_error(dispatcher, 405, "ota_method");
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
	       app->write_measure != NULL && app->measure_start != NULL &&
	       app->write_recorder != NULL && app->recorder_freeze != NULL &&
	       app->recorder_thaw != NULL && app->recorder_config != NULL &&
	       app->recorder_read != NULL && app->recorder_read_release != NULL;
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

/* Fail-closed on a half-wired table: OTA stays absent (404 on every verb)
 * rather than dispatching into whichever callback happened to be supplied. */
FLASHMEM bool aius_optical_app_set_ota(AiusOpticalAppDispatcher *dispatcher,
                                       const AiusOpticalAppOta *ota)
{
	if (dispatcher == NULL || !dispatcher->valid)
		return false;
	memset(&dispatcher->ota, 0, sizeof(dispatcher->ota));
	if (ota == NULL)
		return true; /* deliberate removal, not a failure */
	if (!ota_wired(ota))
		return false;
	dispatcher->ota = *ota;
	return true;
}

FLASHMEM const AiusHttpApplicationResponse *aius_optical_app_ota_dispatch(
	AiusOpticalAppDispatcher *dispatcher, AiusOpticalAppOtaRoute route,
	const AiusHttpRequest *request)
{
	if (dispatcher == NULL || !dispatcher->valid || request == NULL)
		return NULL;
	dispatcher->http_dispatches++;
	return dispatch_ota(dispatcher, route, request);
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
	case AIUS_HTTP_ROUTE_RECORDER:
	case AIUS_HTTP_ROUTE_RECORDER_FREEZE:
	case AIUS_HTTP_ROUTE_RECORDER_THAW:
	case AIUS_HTTP_ROUTE_RECORDER_CONFIG:
	case AIUS_HTTP_ROUTE_RECORDER_DATA:
		return dispatch_recorder(dispatcher, request);
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
	/* The OTA verbs route through the internal dispatch_ota (NOT the counted
	 * public entry point), which owns its own absent-then-token gate ahead of
	 * its switch -- so these routes are deliberately absent from
	 * route_is_control() above, and an OTA-less build answers one 404
	 * ota_absent rather than a 401 that would advertise the surface. The route
	 * ids are unconditional; when AIUS_OTA is off the sub-table is never
	 * installed and ota_wired() is false. */
	case AIUS_HTTP_ROUTE_OTA:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_STATUS, request);
	case AIUS_HTTP_ROUTE_OTA_IMAGE:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_IMAGE, request);
	case AIUS_HTTP_ROUTE_OTA_ARM:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_ARM, request);
	case AIUS_HTTP_ROUTE_OTA_VERIFY:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_VERIFY, request);
	case AIUS_HTTP_ROUTE_OTA_ABORT:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_ABORT, request);
	case AIUS_HTTP_ROUTE_OTA_ACTIVATE:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_ACTIVATE, request);
	case AIUS_HTTP_ROUTE_OTA_ENABLE_ACTIVATION:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_ENABLE_ACTIVATION,
		                    request);
	case AIUS_HTTP_ROUTE_OTA_SELF_TEST_FLASH:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_SELF_TEST, request);
	case AIUS_HTTP_ROUTE_OTA_REHEARSE:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE, request);
	case AIUS_HTTP_ROUTE_OTA_REHEARSE_FAULT:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_REHEARSE_FAULT,
		                    request);
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
