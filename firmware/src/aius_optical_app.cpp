/* Optical application dispatch -- see aius_optical_app.h.
 *
 * Every definition in this file is FLASHMEM. The attribute must sit on the
 * definition: GCC does not inherit a `section` attribute from a forward
 * declaration, so a prototype-only FLASHMEM silently leaves the code in ITCM,
 * and the teensy41-optical image has ~1.3 kB of ITCM left.
 */

/* First, so aius_optical_app.h declares the dsp.json serialiser. */
#include "aius_dsp_config.h"

#include "aius_optical_app.h"

#include "aius_json.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif
#ifndef PROGMEM
#define PROGMEM
#endif

/* lib/aius_producer_yield's yield point (aius_producer_yield.h), declared here
 * rather than included so the host dispatcher suite links its own counting
 * fake without the library on its include path. The cache-served body source
 * below is its direct caller (the send loop's producer-isolation yield-root
 * row, Phase 4g). */
extern "C" bool aius_producer_yield_point(void);

/* Phase 4g: the web cache seam (aius_optical_app_set_web_cache). DTCM, not a
 * dispatcher member: the dispatcher's OCRAM instance has no free word. */
static const AiusOpticalAppWebCache *s_web_cache;
/* Phase 4h: the write-behind overlay seam (aius_optical_app_set_sd_overlay). */
static const AiusOpticalAppSdOverlay *s_sd_overlay;

/* True while the overlay accepts writes (the carve is fitted, Q4). */
static FLASHMEM bool sd_overlay_present(void)
{
	return s_sd_overlay != NULL && s_sd_overlay->present(s_sd_overlay->context);
}

/* PROGMEM: flash, not DTCM .rodata (the .data cliff). */
static const char kSdFlushPending[] PROGMEM = "sd_flush_pending";
static const char kBootOtherAccepted[] PROGMEM =
	"{\"ok\":true,\"rebooting\":true,\"boot_other\":true}";

/* FR-S8: 409 sd_flush_pending while anything is unflushed. */
static FLASHMEM bool sd_flush_pending(void)
{
	return sd_overlay_present() &&
	       !s_sd_overlay->drained(s_sd_overlay->context);
}

#define AIUS_OPTICAL_APP_SD_ROOT "/aius/"
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
 * presets, which the pages re-list after every save --
 * caching any of those trades correctness for nothing. The operator pays one
 * hard refresh after an asset deploy, accepted 2026-07-27. */
#define AIUS_OPTICAL_APP_ASSET_MAX_AGE_S 86400u

/* Phase 4g G2: fonts join the day-long set (they also gain an ETag when served
 * from the web cache). PROGMEM: new literals stay out of DTCM .rodata. */
static const char kSuffixWoff2[] PROGMEM = ".woff2";

static FLASHMEM uint32_t resource_cache_max_age(const AiusHttpRequest *request)
{
	if (resource_suffix_is(request, ".js") ||
	    resource_suffix_is(request, ".css") ||
	    resource_suffix_is(request, kSuffixWoff2))
		return AIUS_OPTICAL_APP_ASSET_MAX_AGE_S;
	return 0u;
}

/* Phase 4g G2, G7: the cache headers of every /sd/www file response, from the
 * cache or the card. Everything outside the day-long set (pages, bundles,
 * .json, presets) is `no-cache`, so it is always revalidated (a 304 from the
 * cache is cheap); `Vary: Accept-Encoding` on all, since one page URL has a
 * gzip and a plain representation. The ETag is set by the cache path only. */
static FLASHMEM void www_cache_policy(const AiusHttpRequest *request,
                                      AiusHttpApplicationResponse *response)
{
	response->cache_max_age_s = resource_cache_max_age(request);
	response->cache_no_cache = response->cache_max_age_s == 0u;
	response->vary_accept_encoding = true;
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

static FLASHMEM bool vector_id_chars_valid(const uint8_t *id, size_t length)
{
	if (id == NULL || length == 0u || length > AIUS_HTTP_MAX_VECTOR_ID)
		return false;
	for (size_t i = 0; i < length; i++) {
		uint8_t ch = id[i];
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
			return false;
	}
	return true;
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
 * this object's key set and vector_id_chars_valid(), and an escape can only
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

/* POST /api/measure/start body: one left-to-right cursor pass over a flat
 * object, tolerating the insignificant whitespace RFC 8259 §2 permits around
 * every structural character, with no recursion and no allocation. An unknown
 * key, a duplicate key, a non-string `id` or anything trailing the closing
 * brace is refused. {"id":"<vector>"} with optional "f0_l"/"f0_r"
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
	if (vector == NULL || !vector_id_chars_valid(vector, vector_length) ||
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
 *  "governor_trim":bool,"missed_fill":bool,"crc_reject":bool,
 *  "gr_threshold_db":<0|-n[.n]>,
 *  "gr_hold_blocks":<uint>,"postroll_blocks":<uint>}. Requiring the full set is
 * the same
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
		{ "governor_trim",
		  AIUS_OPTICAL_APP_RECORDER_MASK_GOVERNOR_TRIM },
		{ "missed_fill", AIUS_OPTICAL_APP_RECORDER_MASK_MISSED_FILL },
		{ "crc_reject", AIUS_OPTICAL_APP_RECORDER_MASK_CRC_REJECT },
	};
	/* One `seen` slot per flag, then one per scalar. Derived from the table
	 * rather than written as a digit: the flag count used to be four
	 * separate literals here, and adding a flag had to change every one of
	 * them or the new key silently read as a scalar's slot. */
	const size_t flag_count = sizeof(flags) / sizeof(flags[0]);
	bool seen[10] = { false, false, false, false, false,
	                  false, false, false, false, false };

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
		for (size_t flag = 0; flag < flag_count; flag++) {
			if (!body_key_is(key, key_length, flags[flag].key))
				continue;
			matched = flag;
			if (body_take_literal(&cursor, "true"))
				*mask |= flags[flag].bit;
			else if (!body_take_literal(&cursor, "false"))
				return false;
			break;
		}
		if (matched >= flag_count &&
		    body_key_is(key, key_length, "gr_threshold_db")) {
			matched = flag_count;
			if (!body_take_db(&cursor, gr_threshold_db))
				return false;
		} else if (matched >= flag_count &&
		           body_key_is(key, key_length, "gr_hold_blocks")) {
			matched = flag_count + 1u;
			if (!body_take_uint(&cursor, gr_hold_blocks))
				return false;
		} else if (matched >= flag_count &&
		           body_key_is(key, key_length, "postroll_blocks")) {
			matched = flag_count + 2u;
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

/* Give back an outstanding recorder read lease, if there is one. Returns
 * whether it released, so a caller that should never have found one can count
 * the occurrence. Clearing the flag before the callback keeps it single-shot:
 * a double release would underflow the bank's reads_in_flight. */
static FLASHMEM bool recorder_lease_drop(AiusOpticalAppDispatcher *dispatcher)
{
	if (!dispatcher->recorder_lease_active)
		return false;
	dispatcher->recorder_lease_active = false;
	dispatcher->app.recorder_read_release(dispatcher->app.context,
	                                      dispatcher->recorder_lease_slot);
	return true;
}

/* The transaction's completion hook for a recorder-data read (registered on the
 * READ_OK immediate response). The 200 borrows a pointer into one bank slot's
 * PSRAM ring for the streamed body's lifetime; this fires EXACTLY ONCE at the
 * borrow's true end -- the response has drained and the transaction recycles, a
 * mid-stream abort tore the connection down, the send deadline expired, or a
 * network teardown discarded the transaction outright
 * (aius_http_transaction_discard) -- and drops that slot's read lease. Reached
 * with no follow-up request, which is what a one-shot client (a single curl /
 * pull-tool GET that closes after its 200) needs: the round-robin must be able
 * to recycle the slot again. `committed` is meaningless for an immediate
 * response and ignored. */
static FLASHMEM void recorder_read_settle(void *ctx, bool committed)
{
	AiusOpticalAppDispatcher *dispatcher = (AiusOpticalAppDispatcher *)ctx;
	(void)committed;
	if (dispatcher == NULL)
		return;
	(void)recorder_lease_drop(dispatcher);
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
		 * deliberately NO app_sd_busy test (a card job can own most
		 * epochs; an sd_busy gate would make the
		 * recorder unusable exactly when it matters). READ_OK takes the
		 * addressed slot's read lease (in recorder_read); the response's
		 * completion hook (recorder_read_settle) drops it when the borrow
		 * ends -- at genuine completion/close, no follow-up request
		 * required. */
		/* A lease still held at the START of a new data read is a leak, not
		 * a live borrow: the single-transaction model means the previous
		 * response is over by the time a second dispatch arrives, so its
		 * hook should already have fired. Release it here rather than
		 * overwrite recorder_lease_slot and strand the old slot's count at
		 * 1 forever -- the bank then defers its round-robin on that slot and
		 * recording stops until reboot. Counted, because reaching this arm
		 * means some path bypassed the completion hook and that is worth
		 * seeing rather than silently repairing. */
		if (recorder_lease_drop(dispatcher))
			dispatcher->recorder_lease_leaks++;
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
			dispatcher->recorder_lease_slot = (uint8_t)resolved_slot;
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
	char id[AIUS_OPTICAL_APP_VECTOR_ID_STORAGE];
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

/* Parses {"dma":<bool>} with the measure body's grammar: one key, no duplicate,
 * nothing trailing the closing brace. */
static FLASHMEM bool sd_mode_body(const AiusHttpRequest *request, bool *dma)
{
	AiusJsonCursor cursor = { request->body, request->body_length, 0u };
	const uint8_t *key;
	size_t key_length;

	if (request->body == NULL || !body_consume(&cursor, '{') ||
	    !body_take_string(&cursor, &key, &key_length) ||
	    !body_consume(&cursor, ':') ||
	    !body_key_is(key, key_length, "dma"))
		return false;
	if (body_take_literal(&cursor, "true"))
		*dma = true;
	else if (body_take_literal(&cursor, "false"))
		*dma = false;
	else
		return false;
	if (!body_consume(&cursor, '}'))
		return false;
	aius_json_skip_whitespace(&cursor);
	return aius_json_at_end(&cursor);
}

/* POST /api/sd/mode. The control-route auth gate already ran; an incomplete
 * body is a 400 like every other small-body route. */
static FLASHMEM const AiusHttpApplicationResponse *dispatch_sd_mode(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	bool dma = false;

	if (app->sd_set_mode == NULL)
		return http_json(dispatcher, 404, "{\"ok\":false}");
	if (!request->body_complete || !sd_mode_body(request, &dma))
		return http_error(dispatcher, 400, "sd_mode_body");
	if (!app->sd_set_mode(app->context, dma))
		return http_error(dispatcher, 503, "sd_busy");
	return http_json(dispatcher, 200,
	                 dma ? "{\"ok\":true,\"sd_mode\":\"dma\"}"
	                     : "{\"ok\":true,\"sd_mode\":\"fifo\"}");
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

/* ------------------------------------------------------------------ */
/* Phase 4g: cache-first /sd/www reads (G1 to G3, G7)                  */
/* ------------------------------------------------------------------ */

static const char kSuffixGz[] PROGMEM = ".gz";

/* Gives back an outstanding web-cache lease, if any; single-shot as
 * recorder_lease_drop() is. */
static FLASHMEM bool web_lease_drop(AiusOpticalAppDispatcher *dispatcher)
{
	const AiusOpticalAppWebCache *web = s_web_cache;
	if (!dispatcher->web_lease_active)
		return false;
	dispatcher->web_lease_active = false;
	if (web != NULL)
		web->release(web->context, dispatcher->web_lease_entry);
	return true;
}

/* The completion hook of a cache-served body: fires exactly once at the
 * borrow's end (sent, aborted, timed out or discarded) and drops the lease,
 * as recorder_read_settle does for the recorder data read. */
static FLASHMEM void web_cache_settle(void *ctx, bool committed)
{
	AiusOpticalAppDispatcher *dispatcher = (AiusOpticalAppDispatcher *)ctx;
	(void)committed;
	if (dispatcher != NULL)
		(void)web_lease_drop(dispatcher);
}

/* The body source of a cache-served response (AiusHttpChunkFn), called from
 * the transport's write_body_chunk() with its 512 B stack buffer: chunk
 * `chunk` is flush-deleted, copied into `dst` and CRC'd on the copy
 * (copy_chunk), so the bytes checked are the bytes sent (G7). A mismatch
 * (counted in www_cache.crc_faults by the cache) schedules a refill and
 * returns false, which closes the connection; the browser retries. Repeatable
 * for a partial write's re-call. A producer yield point every
 * AIUS_OPTICAL_APP_WEB_YIELD_CHUNKS chunks; noinline: it is the send loop's
 * yield-root row (producer-isolation W2) and stack-bound's chain root under
 * write_body_chunk's frame. */
static FLASHMEM __attribute__((noinline)) bool web_cache_body_chunk(
	void *ctx, uint32_t chunk, uint8_t *dst, uint32_t *bytes)
{
	AiusOpticalAppDispatcher *dispatcher = (AiusOpticalAppDispatcher *)ctx;
	const AiusOpticalAppWebCache *web = s_web_cache;

	if (dispatcher == NULL || web == NULL || !dispatcher->web_lease_active ||
	    chunk > UINT16_MAX)
		return false;
	if (!web->copy_chunk(web->context, dispatcher->web_lease_entry,
	                     (uint16_t)chunk, dst, bytes)) {
		web->refill(web->context);
		return false;
	}
	if ((chunk + 1u) % AIUS_OPTICAL_APP_WEB_YIELD_CHUNKS == 0u)
		(void)aius_producer_yield_point();
	return true;
}

/* The cache-first GET of a /sd/www file whose card path is already in
 * target_path. NULL: not served from the cache (absent, a miss, or a
 * manifest-entry or pre-header CRC fault, which also schedules a refill; a
 * faulted .gz entry refills and falls back to the plain one), so the caller
 * reads the card. A client accepting gzip asks for `<page>.html.gz` first (G1). A
 * matching If-None-Match answers 304 with no lease and no verify. Otherwise
 * the entry is leased before the headers, every chunk verified on the chip,
 * and the body streamed per chunk from the lease, released by the settle
 * hook. The ETag waits in temp_path and the gzip name in backup_path, both
 * idle for a read and borrowed until the next dispatch. */
static FLASHMEM const AiusHttpApplicationResponse *dispatch_www_cached(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request,
	const char *content_type)
{
	/* The cache keys relative to /aius/ (4h NS4): www/<page>. */
	static const size_t kRelOffset = sizeof(AIUS_OPTICAL_APP_SD_ROOT) - 1u;
	const AiusOpticalAppWebCache *web = s_web_cache;
	const char *rel = dispatcher->target_path + kRelOffset;
	size_t rel_length;
	char *etag = dispatcher->temp_path;
	AiusOpticalAppWebHit hit;
	AiusHttpApplicationResponse *response;
	bool gzip = false;
	bool found;
	bool fault = false;
	bool plain_fault = false;

	if (web == NULL)
		return NULL;
	rel_length = strlen(rel);
	/* The previous cache-served response's hook has fired by now (the
	 * single-transaction model); a lease still held is dropped, never
	 * overwritten. */
	(void)web_lease_drop(dispatcher);
	if (request->accepts_gzip && resource_suffix_is(request, ".html") &&
	    rel_length + sizeof(kSuffixGz) <= sizeof(dispatcher->backup_path)) {
		memcpy(dispatcher->backup_path, rel, rel_length);
		memcpy(dispatcher->backup_path + rel_length, kSuffixGz,
		       sizeof(kSuffixGz));
		gzip = web->lookup(web->context, dispatcher->backup_path,
		                   rel_length + sizeof(kSuffixGz) - 1u, &hit, etag,
		                   sizeof(dispatcher->temp_path), &fault);
	}
	found = gzip || web->lookup(web->context, rel, rel_length, &hit, etag,
	                            sizeof(dispatcher->temp_path), &plain_fault);
	/* A manifest entry that failed its own CRC was dropped (the cache's
	 * count follows it): refetch it, as a chunk fault does. */
	if (fault || plain_fault)
		web->refill(web->context);
	if (!found)
		return NULL;

	if (aius_http_if_none_match(request, etag)) {
		response = (AiusHttpApplicationResponse *)http_immediate(
			dispatcher, 304, NULL, NULL, 0u);
	} else if (hit.length == 0u) {
		response = (AiusHttpApplicationResponse *)http_immediate(
			dispatcher, 200, content_type, NULL, 0u);
	} else {
		if (!web->lease(web->context, hit.entry))
			return NULL;
		if (!web->verify(web->context, hit.entry)) {
			web->release(web->context, hit.entry);
			web->refill(web->context);
			return NULL;
		}
		response = (AiusHttpApplicationResponse *)http_immediate(
			dispatcher, 200, content_type, NULL, 0u);
		response->body_length = hit.length;
		response->body_chunk = web_cache_body_chunk;
		response->body_chunk_ctx = dispatcher;
		response->sd_settle = web_cache_settle;
		response->sd_settle_ctx = dispatcher;
		dispatcher->web_lease_entry = hit.entry;
		dispatcher->web_lease_active = true;
	}
	www_cache_policy(request, response);
	response->etag = etag;
	response->content_encoding_gzip = gzip;
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
	/* Legacy order for the streamed PUT path: the refusal to overwrite the
	 * public config outranks authentication, which outranks media state. */
	if (resource_is_config(request))
		return http_error(dispatcher, 403, "config_public_only");
	if (!app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	/* Phase 4h (S8, B2, Q4): a www PUT is accepted into the store, card or
	 * no card and whatever job owns the volume; the STORE request streams
	 * the body into it and the 200 follows the commit. */
	if (sd_overlay_present()) {
		static const size_t kRelOffset = sizeof(AIUS_OPTICAL_APP_SD_ROOT) - 1u;
		const char *rel;
		AiusHttpApplicationResponse *response;
		if (!resource_path_valid(request->resource,
		                         request->resource_length) ||
		    !build_root_path(dispatcher->target_path,
		                     sizeof(dispatcher->target_path),
		                     (const char *)request->resource,
		                     request->resource_length) ||
		    request->content_length > UINT32_MAX)
			return http_error(dispatcher, 507, "sd_upload");
		rel = dispatcher->target_path + kRelOffset;
		switch (s_sd_overlay->put_begin(s_sd_overlay->context, rel,
		                                strlen(rel),
		                                (uint32_t)request->content_length)) {
		case AIUS_OPTICAL_APP_STORE_OK:
			break;
		case AIUS_OPTICAL_APP_STORE_INVALID:
			return http_error(dispatcher, 507, "sd_upload");
		case AIUS_OPTICAL_APP_STORE_LIMIT:
			return http_error(dispatcher, 409, "preset_limit");
		default:
			return http_error(dispatcher, 503, "sd_busy");
		}
		response = (AiusHttpApplicationResponse *)sd_upload(
			dispatcher, AIUS_SD_INSTALL_STORE, request->content_length, 0u,
			(const uint8_t *)"{\"ok\":true}", sizeof("{\"ok\":true}") - 1u);
		response->sd_job.temp_path = NULL;
		response->sd_job.backup_path = NULL;
		return response;
	}
	if (!app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (app->sd_busy(app->context))
		return http_error(dispatcher, 507, "sd_upload");
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

/* The listing body for one batch, from the card or the manifest. */
static FLASHMEM const AiusHttpApplicationResponse *index_respond(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request,
	AiusOpticalAppDirBatch *batch)
{
	/* An application that over-reports is held to the cap here rather than
	 * trusted: the storage is the dispatcher's and so is the bound. */
	if (batch->count > AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES) {
		batch->count = AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES;
		batch->more = true;
	}
	batch->entries = dispatcher->index_entries;
	for (size_t i = 0; i < batch->count; i++)
		dispatcher->index_entries[i].name[AIUS_OPTICAL_APP_INDEX_NAME_MAX] =
			'\0';
	return http_serialized_html(dispatcher,
	                            index_write(dispatcher, request, batch));
}

/* GET /sd/<dir>/ -- the /sd/ resource tree describing itself.
 *
 * Order of refusal: the method is a fact about the request, so it answers
 * first; then the capability, the media, the path, and finally the volume. The
 * sd_busy() gate is load-bearing rather than cosmetic -- read_directory reaches
 * the card directly, so it must not run while a job writes the volume. Phase
 * 4g (G6): a CACHE_FILL file in flight never reaches here. This GET reads the
 * card directly (aius_optical_app_route_reads_card), so the runtime's DISPATCH
 * arm holds it while the composition's admission cancels the fill and waits
 * for it to stop (a held DISPATCH, aius_runtime_network_http.cpp); its end
 * extends the fill hold like an HTTP SD job's completion. sd_busy() is
 * therefore never true because of a fill. */
static FLASHMEM const AiusHttpApplicationResponse *dispatch_sd_index(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	AiusOpticalAppDirBatch batch;

	if (request->method != AIUS_HTTP_GET)
		return http_error(dispatcher, 405, "sd_directory");
	if (app->read_directory == NULL)
		return http_error(dispatcher, 503, "sd_index_unavailable");
	/* Phase 4h (B2): with the overlay a manifest-served listing needs no
	 * card; the card gate follows the manifest attempt. With no card the
	 * walk never ran, so presets/ still answers sd_unavailable (N7). */
	if (!sd_overlay_present() && !app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (!resource_directory_valid(request->resource,
	                              request->resource_length) ||
	    !build_root_path(dispatcher->target_path,
	                     sizeof(dispatcher->target_path),
	                     (const char *)request->resource,
	                     request->resource_length - 1u))
		return http_error(dispatcher, 404, "sd_path");

	memset(dispatcher->index_entries, 0, sizeof(dispatcher->index_entries));
	memset(&batch, 0, sizeof(batch));
	batch.entries = dispatcher->index_entries;
	batch.capacity = AIUS_OPTICAL_APP_INDEX_MAX_ENTRIES;
	/* Phase 4g: a directory the fill has wholly published (presets/) lists
	 * from the manifest, with no card read and no idle volume needed. The
	 * resource is "www/<dir>/", relative to /aius/ as the cache's keys are
	 * (4h NS4), so it is the cache's directory key as it stands. */
	if (s_web_cache != NULL &&
	    s_web_cache->listing(s_web_cache->context,
	                         (const char *)request->resource,
	                         request->resource_length, &batch))
		return index_respond(dispatcher, request, &batch);
	if (sd_overlay_present() && !app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (app->sd_busy(app->context))
		return http_error(dispatcher, 503, "sd_busy");

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
	/* Phase 4h (S6): pending rows merged, tombstoned names hidden. */
	if (sd_overlay_present())
		s_sd_overlay->merge_listing(s_sd_overlay->context,
		                            (const char *)request->resource,
		                            request->resource_length, &batch);
	return index_respond(dispatcher, request, &batch);
}

static FLASHMEM const AiusHttpApplicationResponse *dispatch_sd(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
	const char *content_type;
	uint32_t cache_max_age_s = 0u;
	bool www = false;
	bool dsp_alias = false;
	bool overlay;
	/* A resource naming a directory is a listing on GET and nothing else on any
	 * other method -- it is not an upload target and not a delete target. */
	if (request->route == AIUS_HTTP_ROUTE_SD_FILE &&
	    resource_is_directory(request))
		return dispatch_sd_index(dispatcher, request);
	if (request->method == AIUS_HTTP_PUT)
		return dispatch_sd_upload(dispatcher, request);
	/* Phase 4h (B2, Q1): with the overlay, a www or dsp.json read looks up
	 * the store first and a www DELETE is a tombstone, so their card gate
	 * moves below; the status route keeps it here. */
	overlay = sd_overlay_present();
	if ((!overlay || request->route == AIUS_HTTP_ROUTE_SD_STATUS) &&
	    !app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (request->route == AIUS_HTTP_ROUTE_SD_STATUS)
		return http_json(dispatcher, 200, "{\"ready\":true}");
	if (request->method == AIUS_HTTP_GET &&
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
		dsp_alias = true;
	} else {
		if (!resource_path_valid(request->resource, request->resource_length) ||
		    !build_root_path(dispatcher->target_path,
		                     sizeof(dispatcher->target_path),
		                     (const char *)request->resource,
		                     request->resource_length))
			return http_error(dispatcher, 404, "sd_path");
		content_type = resource_content_type(request);
		cache_max_age_s = resource_cache_max_age(request);
		www = true;
	}
	if (request->method != AIUS_HTTP_GET && resource_is_config(request))
		return http_error(dispatcher, 403, "config_public_only");
	if (request->method == AIUS_HTTP_DELETE &&
	    !app->auth_matches(app->context, request->sd_auth,
	                       request->sd_auth_length))
		return http_error(dispatcher, 401, "sd_auth");
	/* Phase 4g: a www/ read is served from the PSRAM cache first, so a warm
	 * page load needs no card and no idle volume. */
	if ((www || (dsp_alias && overlay)) && request->method == AIUS_HTTP_GET) {
		const AiusHttpApplicationResponse *cached =
			dispatch_www_cached(dispatcher, request, content_type);
		if (cached != NULL)
			return cached;
	}
	/* Phase 4h (S2, N3): a www DELETE is accepted as a tombstone, 200
	 * whether or not the card holds the file; 503 sd_busy with no entry. */
	if (overlay && www && request->method == AIUS_HTTP_DELETE) {
		static const size_t kRelOffset = sizeof(AIUS_OPTICAL_APP_SD_ROOT) - 1u;
		const char *rel = dispatcher->target_path + kRelOffset;
		if (s_sd_overlay->tombstone(s_sd_overlay->context, rel,
		                            strlen(rel)) != AIUS_OPTICAL_APP_STORE_OK)
			return http_error(dispatcher, 503, "sd_busy");
		return http_json(dispatcher, 200, "{\"ok\":true}");
	}
	if (overlay && !app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	if (app->sd_busy(app->context))
		return http_error(dispatcher, 503, "sd_busy");
	if (request->method == AIUS_HTTP_GET) {
		const AiusHttpApplicationResponse *download =
			sd_download(dispatcher, content_type, cache_max_age_s);
		/* The card fallback keeps the G2 policy minus the ETag (the card
		 * file's CRC is not known until the fill has read it). */
		if (www)
			www_cache_policy(request, &dispatcher->http_response);
		return download;
	}
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
	/* Phase 4h (B2, Q1): with the overlay the store accepts with no card. */
	if (!sd_overlay_present() && !app->sd_ready(app->context))
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
	if (!sd_overlay_present() && !app->sd_ready(app->context))
		return http_error(dispatcher, 503, "sd_unavailable");
	return dispatch_store(dispatcher, request,
	                      app->store_public_config(app->context, request->body,
	                                               request->body_length),
	                      AIUS_OPTICAL_APP_CONFIG_NAME,
	                      sizeof(AIUS_OPTICAL_APP_CONFIG_NAME) - 1u,
	                      "config_invalid");
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
	{ 500u, "ota_self_test_failed" },    /* SELF_TEST_FAILED */
	{ 409u, "ota_ivt" }                  /* IVT */
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
 * socket needs, and the reason this is a chunked upload rather than
 * a session.
 *
 * DURABLE STAGING, ABSORB DEFERRED TO THE COMMIT (aius-ota-resilience.md
 * Phase 4a). On an accepted chunk the route writes the chunk's bytes to the
 * fixed staging file /aius/ota-staged.img through the existing bounded SD job
 * engine (AIUS_SD_INSTALL_APPEND_PART at the chunk's file offset, exactly the
 * the chunked-append shape), so the container survives to activation. The
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
	 * at, which is the same refusal a Content-Range-less chunk gets. */
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
	       ota->rehearse != NULL && ota->boot_other != NULL;
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
		 * new image; the reset is deferred past the response drain, as reboot.
		 * Phase 4h (FR-S8): never over an unflushed write. */
		if (sd_flush_pending())
			return http_error(dispatcher, 409, kSdFlushPending);
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
		if (sd_flush_pending()) /* Phase 4h (FR-S8) */
			return http_error(dispatcher, 409, kSdFlushPending);
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
	case AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER:
		/* Phase 4y: a reboot, so the reboot route's FR-S8 gate first. The
		 * callback validates and sets flags only; the record write runs in
		 * the deferred reboot service after this 200 drains. */
		if (sd_flush_pending())
			return http_error(dispatcher, 409, kSdFlushPending);
		return ota_verdict_answer(dispatcher, ota->boot_other(ota->context),
		                          kBootOtherAccepted);
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
	       app->store_public_config != NULL && app->store_dsp_config != NULL &&
	       app->set_test_running != NULL &&
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
	s_web_cache = NULL;
	s_sd_overlay = NULL;
	return true;
}

/* ---- Phase 4h: the write-behind flush coordinator ---- */

/* CR-B1: the remount schedule. A pending schedule waits until it comes
 * due; a lapsed or never-set one reads due and stays lapsed, so no uptime or
 * schedule age past 2^31 ms makes it pending again. */
static FLASHMEM void sd_remount_schedule(AiusOpticalSdFlush *flush,
                                         uint32_t at_ms)
{
	flush->remount_at_ms = at_ms;
	flush->remount_armed = true;
}

static FLASHMEM bool sd_remount_waiting(AiusOpticalSdFlush *flush,
                                        uint32_t now_ms)
{
	if (!flush->remount_armed)
		return false;
	if ((int32_t)(now_ms - flush->remount_at_ms) < 0)
		return true;
	flush->remount_armed = false;
	return false;
}

FLASHMEM void aius_optical_sd_flush_lapse(AiusOpticalSdFlush *flush,
                                          uint32_t now_ms)
{
	(void)sd_remount_waiting(flush, now_ms);
	if (flush->mount_seen &&
	    now_ms - flush->mounted_at_ms >= AIUS_OPTICAL_SD_FLUSH_FLAP_MS)
		flush->mount_seen = false;
	/* Not while a fault remount waits: its quiet window starts at the
	 * remount (D-S8). */
	if (flush->fault_backoff != 0u && !flush->fault_remount &&
	    now_ms - flush->last_fault_ms >= AIUS_OPTICAL_SD_FAULT_QUIET_MS)
		flush->fault_backoff = 0u;
}

static FLASHMEM void sd_flush_backoff(AiusOpticalSdFlush *flush)
{
	uint32_t interval = flush->remount_interval_ms == 0u
	                            ? AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS
	                            : flush->remount_interval_ms;
	interval = interval >= AIUS_OPTICAL_SD_FLUSH_REMOUNT_MAX_MS / 2u
	                   ? AIUS_OPTICAL_SD_FLUSH_REMOUNT_MAX_MS
	                   : interval * 2u;
	flush->remount_interval_ms = interval;
}

/* The stopped fill's latch, dropped by its canceller's end (S9). */
static FLASHMEM void sd_flush_release_fill(bool *cancelled,
                                           const AiusOpticalSdFlushOps *ops)
{
	if (!*cancelled)
		return;
	*cancelled = false;
	ops->fill_hold_release(ops->context);
}

FLASHMEM bool aius_optical_sd_card_gone(bool card_ready, bool mounted,
                                        bool card_detected)
{
	return !card_ready || (!mounted && !card_detected);
}

FLASHMEM void aius_optical_sd_note_direct_fault(AiusOpticalSdFlush *flush)
{
	flush->direct_faults++;
}

FLASHMEM AiusOpticalSdBeginGate aius_optical_sd_begin_blocked(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	uint32_t now_ms)
{
	void *const context = ops->context;
	if (ops->mounted(context) || !ops->playback(context))
		return AIUS_OPTICAL_SD_BEGIN_NONE;
	if ((flush->fault_remount ||
	     (ops->present(context) && ops->held(context))) &&
	    sd_remount_waiting(flush, now_ms))
		return AIUS_OPTICAL_SD_BEGIN_WAIT;
	return aius_optical_sd_no_card_begin_blocked(flush, ops)
	               ? AIUS_OPTICAL_SD_BEGIN_NO_CARD
	               : AIUS_OPTICAL_SD_BEGIN_NONE;
}

FLASHMEM bool aius_optical_sd_step_begin_blocked(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	uint32_t now_ms)
{
	if (flush->remount_running)
		return false;
	return aius_optical_sd_begin_blocked(flush, ops, now_ms) !=
	       AIUS_OPTICAL_SD_BEGIN_NONE;
}

FLASHMEM bool aius_optical_sd_no_card_begin_blocked(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops)
{
	void *const context = ops->context;
	(void)flush;
	if (ops->mounted(context) || !ops->playback(context))
		return false;
	/* DAT3 is read only once re-muxed (the adapter answers present
	 * otherwise, and the BEGIN then runs as today, D3). */
	if (!ops->card_detected(context)) {
		ops->note_card_lost(context);
		return true;
	}
	return false;
}

/* D-S8: the fault remount's delay, doubling while faults recur within the
 * quiet window (a mount between them included), reset only by quiet. */
static FLASHMEM void sd_fault_schedule(AiusOpticalSdFlush *flush,
                                       uint32_t now_ms)
{
	uint32_t delay;
	if (flush->fault_backoff != 0u &&
	    now_ms - flush->last_fault_ms < AIUS_OPTICAL_SD_FAULT_QUIET_MS) {
		if (flush->fault_backoff < 6u)
			flush->fault_backoff++;
	} else {
		flush->fault_backoff = 1u;
	}
	flush->last_fault_ms = now_ms;
	delay = AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS << (flush->fault_backoff - 1u);
	if (delay > AIUS_OPTICAL_SD_FLUSH_REMOUNT_MAX_MS)
		delay = AIUS_OPTICAL_SD_FLUSH_REMOUNT_MAX_MS;
	sd_remount_schedule(flush, now_ms + delay);
}

FLASHMEM AiusOpticalSdCardLoss aius_optical_sd_card_loss(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	bool card_ready, bool mounted, bool file_open, uint32_t driver_faults,
	uint32_t now_ms)
{
	const uint32_t faults = driver_faults + flush->direct_faults;
	aius_optical_sd_flush_lapse(flush, now_ms);
	if (!card_ready)
		return AIUS_OPTICAL_SD_CARD_GONE;
	if (mounted) {
		flush->faults_seen = faults;
		flush->loss = AIUS_OPTICAL_SD_CARD_KEPT;
		flush->fault_remount = false;
		return AIUS_OPTICAL_SD_CARD_KEPT;
	}
	if (faults != flush->faults_seen) {
		/* D-N7: no carve, no re-mux and no self-initiated remount. */
		if (!ops->present(ops->context)) {
			flush->faults_seen = faults;
			flush->loss = AIUS_OPTICAL_SD_CARD_GONE;
			return AIUS_OPTICAL_SD_CARD_GONE;
		}
		/* D-S7: DAT3 is the bus's while a file is open: wait. */
		if (file_open)
			return AIUS_OPTICAL_SD_CARD_PENDING;
		flush->faults_seen = faults;
		if (!ops->card_present_after_fault(ops->context)) {
			flush->loss = AIUS_OPTICAL_SD_CARD_GONE;
			flush->fault_remount = false;
			ops->note_card_lost(ops->context);
			aius_optical_sd_flush_note_card_gone(flush, now_ms);
		} else {
			flush->loss = AIUS_OPTICAL_SD_CARD_FAULT_PRESENT;
			/* A retry already scheduled keeps its own backoff. */
			if (!flush->fault_remount)
				sd_fault_schedule(flush, now_ms);
			flush->fault_remount = true;
		}
	}
	return (AiusOpticalSdCardLoss)flush->loss;
}

FLASHMEM uint32_t aius_optical_sd_flush_scratch_id(uint32_t file_crc,
                                                   const char *target_path)
{
	/* FNV-1a over the path, folded with the CRC. */
	uint32_t hash = 2166136261u;
	if (target_path != NULL)
		for (const char *p = target_path; *p != '\0'; p++)
			hash = (hash ^ (uint8_t)*p) * 16777619u;
	return hash ^ file_crc;
}

FLASHMEM void aius_optical_sd_flush_note_card_gone(AiusOpticalSdFlush *flush,
                                                   uint32_t now_ms)
{
	if (flush->mount_seen &&
	    now_ms - flush->mounted_at_ms < AIUS_OPTICAL_SD_FLUSH_FLAP_MS)
		sd_flush_backoff(flush);
	flush->mount_seen = false;
}

FLASHMEM void aius_optical_sd_flush_note_probe(AiusOpticalSdFlush *flush,
                                               const AiusOpticalSdFlushOps *ops,
                                               bool present, uint32_t now_ms)
{
	const bool retry = flush->remount_running;
	flush->remount_running = false;
	if (present) {
		/* D-S5: the mount is back. D-S8: the quiet window runs from this
		 * mount, so a fault right after it still doubles the delay. */
		if (flush->fault_remount)
			flush->last_fault_ms = now_ms;
		flush->fault_remount = false;
		if (ops->held(ops->context)) {
			ops->note_mounted(ops->context, now_ms);
			flush->mounted_at_ms = now_ms;
			flush->mount_seen = true;
		}
		return;
	}
	ops->note_card_lost(ops->context);
	if (retry)
		sd_flush_backoff(flush);
}

/* H3: with the probe skipped for playback, a held flush retries its BEGIN
 * through a probe step at most once per interval and only while DAT3 reads a
 * card (S1); a DAT3 false-to-true edge (a card reinserted, NS3) resets the
 * interval. */
static FLASHMEM bool sd_flush_remount_due(AiusOpticalSdFlush *flush,
                                          const AiusOpticalSdFlushOps *ops,
                                          uint32_t now_ms)
{
	void *const context = ops->context;
	/* D-S5: a fault-dropped mount of a present card remounts on the same
	 * schedule, carve or no carve, pending writes or none (its card ops
	 * answer sd_unavailable until it does). */
	const bool fault = flush->fault_remount;
	if (!ops->present(context) || /* D-N7: carve only */
	    (!fault && (!ops->held(context) || !ops->playback(context))) ||
	    flush->frozen || sd_remount_waiting(flush, now_ms))
		return false;
	if (!fault && ops->drained(context))
		return false;
	if (!ops->card_detected(context)) {
		flush->remount_interval_ms = 0u; /* the next true is an edge */
		sd_remount_schedule(flush, now_ms + AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS);
		/* D-S9: a card pulled after a fault: card-gone, the hold set. */
		ops->note_card_lost(context);
		if (fault) {
			flush->fault_remount = false;
			flush->loss = AIUS_OPTICAL_SD_CARD_GONE;
		}
		return false;
	}
	sd_remount_schedule(flush,
	                    now_ms + (flush->remount_interval_ms == 0u
	                                      ? AIUS_OPTICAL_SD_FLUSH_REMOUNT_MS
	                                      : flush->remount_interval_ms));
	flush->remount_running = true;
	return true;
}

FLASHMEM AiusOpticalSdIdleStep aius_optical_sd_idle_step(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	uint32_t now_ms, bool probe_due)
{
	/* D-N9: no step is chosen when this runs, so a remount probe step chosen
	 * earlier and dropped no longer holds the remount slot (its verdict,
	 * when it ran, was consumed by note_probe first). */
	flush->remount_running = false;
	if (flush->frozen || aius_optical_sd_flush_start(flush, ops, now_ms))
		return AIUS_OPTICAL_SD_IDLE_WAIT;
	if (sd_flush_remount_due(flush, ops, now_ms))
		return AIUS_OPTICAL_SD_IDLE_REMOUNT;
	/* NS7: no probe BEGIN while DAT3 (re-muxed, no mount held) reads no
	 * card; a boot's first probe always runs (D3). */
	if (probe_due && !ops->card_detected(ops->context)) {
		ops->note_card_lost(ops->context);
		return AIUS_OPTICAL_SD_IDLE_NO_CARD;
	}
	/* No routine card touch while live audio is on the wire (the probe-skip
	 * predicate; first bring-up exempt). */
	if (!probe_due || ops->playback(ops->context))
		/* D-S9: a fill file BEGINs too. */
		return aius_optical_sd_begin_blocked(flush, ops, now_ms) !=
		                       AIUS_OPTICAL_SD_BEGIN_NONE
		               ? AIUS_OPTICAL_SD_IDLE_WAIT
		               : AIUS_OPTICAL_SD_IDLE_FILL;
	return AIUS_OPTICAL_SD_IDLE_PROBE;
}

/* The flush start's gate prefix, shared with the scan preempt: RUNNING when a
 * flush already runs, OPEN when every check holds, CLOSED otherwise. */
typedef enum {
	SD_FLUSH_GATE_CLOSED = 0,
	SD_FLUSH_GATE_RUNNING,
	SD_FLUSH_GATE_OPEN
} SdFlushGate;

static FLASHMEM SdFlushGate sd_flush_gate(AiusOpticalSdFlush *flush,
                                          const AiusOpticalSdFlushOps *ops,
                                          uint32_t now_ms)
{
	void *const context = ops->context;

	if (flush->running)
		return SD_FLUSH_GATE_RUNNING;
	if (!ops->present(context) || flush->frozen || !ops->ready(context) ||
	    ops->held(context))
		return SD_FLUSH_GATE_CLOSED;
	/* D-S9: after a fault on a present card the flush waits for the fault
	 * remount (D-S8's schedule), and no flush BEGIN starts against the
	 * BEGIN gate. */
	if (flush->fault_remount ||
	    aius_optical_sd_begin_blocked(flush, ops, now_ms) !=
		    AIUS_OPTICAL_SD_BEGIN_NONE)
		return SD_FLUSH_GATE_CLOSED;
	if (!ops->due(context, now_ms))
		return SD_FLUSH_GATE_CLOSED;
	/* G6: an HTTP SD start or a card-reading DISPATCH held behind the fill
	 * runs first. */
	if (ops->http_deferred(context))
		return SD_FLUSH_GATE_CLOSED;
	return SD_FLUSH_GATE_OPEN;
}

FLASHMEM void aius_optical_sd_flush_preempt_scan(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	uint32_t now_ms, bool scan_running)
{
	if (!scan_running || flush->cancelled_fill ||
	    sd_flush_gate(flush, ops, now_ms) != SD_FLUSH_GATE_OPEN)
		return;
	ops->fill_cancel(ops->context);
	flush->cancelled_fill = true;
}

FLASHMEM bool aius_optical_sd_usage_may_start(const AiusOpticalSdFlush *flush)
{
	return !flush->cancelled_fill;
}

FLASHMEM bool aius_optical_sd_flush_start(AiusOpticalSdFlush *flush,
                                          const AiusOpticalSdFlushOps *ops,
                                          uint32_t now_ms)
{
	void *const context = ops->context;
	AiusOpticalSdFlushJob job;
	AiusOpticalSdFlushStart started;
	const SdFlushGate gate = sd_flush_gate(flush, ops, now_ms);

	if (gate != SD_FLUSH_GATE_OPEN)
		return gate == SD_FLUSH_GATE_RUNNING;
	if (ops->fill_in_flight(context)) {
		if (!flush->cancelled_fill) {
			ops->fill_cancel(context);
			flush->cancelled_fill = true;
		}
		return true;
	}
	if (!ops->storage_ready(context))
		return flush->cancelled_fill;
	memset(&job, 0, sizeof(job));
	started = ops->start(context, now_ms, &job);
	if (started != AIUS_OPTICAL_SD_FLUSH_START_STARTED) {
		/* The engine refused the request itself: a counted failure, backed
		 * off by the overlay, never a spin. */
		if (started == AIUS_OPTICAL_SD_FLUSH_START_REFUSED)
			ops->job_end(context, AIUS_OPTICAL_SD_FLUSH_FAILED, now_ms);
		sd_flush_release_fill(&flush->cancelled_fill, ops);
		return false;
	}
	flush->running = true;
	flush->copy_before_write = job.copy_before_write;
	flush->target_first = job.target_first;
	flush->skip_backup = job.skip_backup;
	return true;
}

FLASHMEM void aius_optical_sd_flush_settle(AiusOpticalSdFlush *flush,
                                           const AiusOpticalSdFlushOps *ops,
                                           const AiusOpticalSdFlushEnd *end,
                                           uint32_t now_ms)
{
	void *const context = ops->context;
	AiusOpticalSdFlushOutcome outcome = AIUS_OPTICAL_SD_FLUSH_OK;

	if (!flush->running)
		return;
	/* D-S7: a faulted end whose card-loss verdict waits on an open file
	 * stays unsettled (no S5 try, no outcome) until it is classified. */
	if ((!end->result || end->fault) && end->loss_pending)
		return;
	flush->running = false;
	if (!end->result || end->fault) {
		if (end->input_faulted)
			outcome = AIUS_OPTICAL_SD_FLUSH_CHUNK_CRC;
		else if (aius_optical_sd_card_gone(true, end->mounted,
		                                   end->card_detected))
			outcome = AIUS_OPTICAL_SD_FLUSH_CARD_GONE;
		else
			outcome = AIUS_OPTICAL_SD_FLUSH_FAILED;
	}
	/* CR2-S1: the pass notes come from the engine's own job record, whatever
	 * the outcome or the result kind (a root-input CRC fault in the second
	 * pass still reports the first pass closed), before the job end (the
	 * overlay notes the running job). */
	if (flush->copy_before_write) {
		/* AiusWebCacheSafeCopy: 0 target, 1 backup. */
		const uint8_t first =
			flush->skip_backup || flush->target_first ? 0u : 1u;
		const uint8_t passes = flush->skip_backup ? 1u : 2u;
		if (end->passes_closed >= 1u)
			ops->note_pass(context, first);
		if (passes == 2u && end->passes_closed >= 2u)
			ops->note_pass(context, (uint8_t)(first ^ 1u));
		if (end->passes_closed >= passes &&
		    (outcome == AIUS_OPTICAL_SD_FLUSH_OK || end->readback_mismatch))
			ops->note_readback(context,
			                   outcome == AIUS_OPTICAL_SD_FLUSH_OK);
	}
	ops->job_end(context, outcome, now_ms);
	if (outcome == AIUS_OPTICAL_SD_FLUSH_CARD_GONE)
		aius_optical_sd_flush_note_card_gone(flush, now_ms);
	sd_flush_release_fill(&flush->cancelled_fill, ops);
}

FLASHMEM bool aius_optical_sd_flush_drained(const AiusOpticalSdFlushOps *ops)
{
	return !ops->present(ops->context) || ops->drained(ops->context);
}

FLASHMEM AiusOpticalSdGate aius_optical_sd_write_gate(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	uint32_t now_ms)
{
	if (!flush->frozen) {
		if (!aius_optical_sd_flush_drained(ops))
			return AIUS_OPTICAL_SD_GATE_DRAIN;
		/* The latch (S8): no SD job starts; a fill file in flight is
		 * stopped at its next primitive. */
		flush->frozen = true;
		flush->freeze_latched_at_ms = now_ms;
		if (ops->fill_in_flight(ops->context)) {
			ops->fill_cancel(ops->context);
			flush->freeze_cancelled_fill = true;
		}
	}
	if (ops->settled(ops->context))
		return AIUS_OPTICAL_SD_GATE_PASS;
	if (now_ms - flush->freeze_latched_at_ms >= AIUS_OPTICAL_SD_FREEZE_MS)
		return AIUS_OPTICAL_SD_GATE_BUSY;
	return AIUS_OPTICAL_SD_GATE_WAIT;
}

FLASHMEM void aius_optical_sd_freeze_release(AiusOpticalSdFlush *flush,
                                             const AiusOpticalSdFlushOps *ops)
{
	if (!flush->frozen)
		return;
	flush->frozen = false;
	sd_flush_release_fill(&flush->freeze_cancelled_fill, ops);
}

/* ASCII case-folded equality, as FAT names compare. */
static FLASHMEM bool sd_flush_rel_equal(const char *a, const char *b,
                                        size_t length)
{
	for (size_t i = 0; i < length; i++) {
		char x = a[i];
		char y = b[i];
		if (x >= 'A' && x <= 'Z')
			x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = (char)(y - 'A' + 'a');
		if (x != y || x == '\0')
			return false;
	}
	return a[length] == '\0';
}

FLASHMEM AiusOpticalAppStoreResult aius_optical_sd_flush_put_begin(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	uint32_t *fill_cancels, const char *rel, size_t rel_length,
	uint32_t length)
{
	void *const context = ops->context;
	AiusOpticalAppStoreResult result;
	if (flush->put_open)
		return AIUS_OPTICAL_APP_STORE_BUSY;
	if (ops->fill_in_flight(context)) {
		const char *fill = ops->fill_rel(context);
		if (fill != NULL && sd_flush_rel_equal(fill, rel, rel_length) &&
		    ops->cancel_root(context))
			(*fill_cancels)++;
	}
	result = ops->put_begin(context, rel, rel_length, length);
	if (result == AIUS_OPTICAL_APP_STORE_OK)
		flush->put_open = true;
	return result;
}

FLASHMEM bool aius_optical_sd_flush_put_close(AiusOpticalSdFlush *flush)
{
	const bool open = flush->put_open;
	flush->put_open = false;
	return open;
}

FLASHMEM void aius_optical_sd_flush_put_abort(AiusOpticalSdFlush *flush,
                                              const AiusOpticalSdFlushOps *ops)
{
	if (!aius_optical_sd_flush_put_close(flush))
		return;
	ops->put_abort(ops->context);
}

FLASHMEM void aius_optical_sd_flush_sweep_orphan(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	const AiusHttpTransaction *http, bool deferred_start_present,
	bool storage_action_present)
{
	if (!flush->put_open ||
	    !aius_optical_app_sd_staging_orphaned(http, deferred_start_present,
	                                          storage_action_present))
		return;
	aius_optical_sd_flush_put_abort(flush, ops);
}

/* An abandon: unfreeze and clear the request (R5-S2). */
static FLASHMEM void sd_service_abandon(AiusOpticalSdFlush *flush,
                                        const AiusOpticalSdFlushOps *ops,
                                        volatile bool *requested)
{
	aius_optical_sd_freeze_release(flush, ops);
	*requested = false;
}

FLASHMEM AiusOpticalSdService aius_optical_sd_service_gate(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	volatile bool *requested, uint32_t now_ms, bool reset_on_busy)
{
	if (!*requested)
		return AIUS_OPTICAL_SD_SERVICE_IDLE;
	switch (aius_optical_sd_write_gate(flush, ops, now_ms)) {
	case AIUS_OPTICAL_SD_GATE_WAIT:
		return AIUS_OPTICAL_SD_SERVICE_WAIT;
	case AIUS_OPTICAL_SD_GATE_PASS:
		return AIUS_OPTICAL_SD_SERVICE_RUN;
	case AIUS_OPTICAL_SD_GATE_BUSY:
		if (reset_on_busy)
			return AIUS_OPTICAL_SD_SERVICE_RUN;
		sd_service_abandon(flush, ops, requested);
		return AIUS_OPTICAL_SD_SERVICE_SD_BUSY;
	case AIUS_OPTICAL_SD_GATE_DRAIN:
	default:
		sd_service_abandon(flush, ops, requested);
		return AIUS_OPTICAL_SD_SERVICE_FLUSH_PENDING;
	}
}

FLASHMEM bool aius_optical_sd_service_final(AiusOpticalSdFlush *flush,
                                            const AiusOpticalSdFlushOps *ops,
                                            volatile bool *requested)
{
	if (aius_optical_sd_flush_drained(ops))
		return true;
	sd_service_abandon(flush, ops, requested);
	return false;
}

/* Phase 4y: the deferred acts' mutual exclusion (aius_optical_app.h). */
FLASHMEM bool aius_optical_flash_act_excluded(
	AiusOpticalFlashAct act, const AiusOpticalPendingActs *pending)
{
	if (pending == NULL)
		return true;
	switch (act) {
	case AIUS_OPTICAL_ACT_BOOT_OTHER:
		return pending->reboot || pending->activation ||
		       pending->rehearsal || pending->boot_other;
	case AIUS_OPTICAL_ACT_REHEARSE:
		return pending->activation || pending->rehearsal ||
		       pending->boot_other;
	case AIUS_OPTICAL_ACT_ACTIVATE:
	case AIUS_OPTICAL_ACT_SELF_TEST:
		return pending->boot_other;
	default:
		return true;
	}
}

FLASHMEM AiusOpticalAppOtaResult aius_optical_boot_other_check(
	const AiusOpticalBootOther *state, bool (*other_ivt_sane)(void *context),
	void *context)
{
	if (state == NULL || !state->ab_built || !state->ab_enabled)
		return AIUS_OPTICAL_APP_OTA_UNSUPPORTED;
	if (!state->record_valid ||
	    state->pending_slot != AIUS_OPTICAL_AB_SLOT_NONE ||
	    (state->running_slot != AIUS_OPTICAL_AB_SLOT_A &&
	     state->running_slot != AIUS_OPTICAL_AB_SLOT_B) ||
	    state->active_slot != state->running_slot ||
	    state->booted_slot == AIUS_OPTICAL_AB_SLOT_NONE)
		return AIUS_OPTICAL_APP_OTA_STATE_REFUSED;
	if (aius_optical_flash_act_excluded(AIUS_OPTICAL_ACT_BOOT_OTHER,
	                                    &state->pending))
		return AIUS_OPTICAL_APP_OTA_STATE_REFUSED;
	/* An upgrade in flight owns the other slot's future. */
	if (state->ota_state >= AIUS_OPTICAL_APP_OTA_STATE_OFFERED &&
	    state->ota_state <= AIUS_OPTICAL_APP_OTA_STATE_ACTIVATION_REQUESTED)
		return AIUS_OPTICAL_APP_OTA_STATE_REFUSED;
	if (state->running_slot == AIUS_OPTICAL_AB_SLOT_A &&
	    (other_ivt_sane == NULL || !other_ivt_sane(context)))
		return AIUS_OPTICAL_APP_OTA_IVT;
	return AIUS_OPTICAL_APP_OTA_OK;
}

FLASHMEM AiusOpticalRebootStep aius_optical_reboot_finish(
	AiusOpticalSdFlush *flush, const AiusOpticalSdFlushOps *ops,
	const AiusOpticalRebootActs *acts, volatile bool *reboot_requested,
	volatile bool *switch_requested, bool drained)
{
	if (!drained) {
		/* The gate or the final query abandoned (and already unfroze and
		 * cleared the reboot) before anything was written: the record
		 * stands. */
		*switch_requested = false;
		return AIUS_OPTICAL_REBOOT_ABANDONED;
	}
	if (*switch_requested && !acts->switch_write(acts->context)) {
		*switch_requested = false;
		sd_service_abandon(flush, ops, reboot_requested);
		return AIUS_OPTICAL_REBOOT_SWITCH_FAILED;
	}
	return AIUS_OPTICAL_REBOOT_RESET;
}

FLASHMEM bool aius_optical_app_card_ops_ready(bool card_ready, bool dat3_remuxed)
{
	return card_ready && !dat3_remuxed;
}

FLASHMEM bool aius_optical_app_http_idle(const AiusHttpTransaction *http)
{
	return http != NULL && http->state == AIUS_HTTP_TX_IDLE &&
	       http->outstanding_action == AIUS_HTTP_ACTION_NONE;
}

FLASHMEM bool aius_optical_settings_reput(const AiusHttpTransaction *http,
                                          uint8_t *body, size_t capacity,
                                          AiusOpticalSettingsWriter writer,
                                          const uint8_t **bytes,
                                          uint32_t *length)
{
	size_t written;
	if (!aius_optical_app_http_idle(http) || body == NULL || writer == NULL)
		return false;
	written = writer(body, capacity);
	if (written == 0u)
		return false;
	*bytes = body;
	*length = (uint32_t)written;
	return true;
}

/* ---- the dsp.json serialiser (moved from optical_main.cpp) ---- */

static FLASHMEM bool dsp_json_append(char *body, size_t capacity,
                                     size_t *length, const char *format, ...)
{
	va_list arguments;
	int written;
	if (*length >= capacity)
		return false;
	va_start(arguments, format);
	written = vsnprintf(body + *length, capacity - *length, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= capacity - *length)
		return false;
	*length += (size_t)written;
	return true;
}

static FLASHMEM const char *dsp_eq_type(uint8_t type)
{
	switch (type) {
	case 0: return "bypass";
	case 1: return "highpass";
	case 2: return "peaking";
	case 3: return "lowpass";
	default: return NULL;
	}
}

/* One bounded EQ table as a JSON array; false on capacity exhaustion or an
 * unknown section type. */
static FLASHMEM bool dsp_eq_array(char *output, size_t capacity,
                                  size_t *length,
                                  const AiusDspModelEq *sections,
                                  uint8_t count)
{
	if (!dsp_json_append(output, capacity, length, "["))
		return false;
	for (uint8_t index = 0; index < count && index < 2u; index++) {
		const char *type = dsp_eq_type(sections[index].type);
		if (type == NULL || !dsp_json_append(output, capacity, length,
			"%s{\"type\":\"%s\",\"frequency_hz\":%.17g,\"q\":%.17g,\"gain_db\":%.17g}",
			index == 0 ? "" : ",", type, sections[index].frequency_hz,
			sections[index].q, sections[index].gain_db))
			return false;
	}
	return dsp_json_append(output, capacity, length, "]");
}

FLASHMEM size_t aius_optical_dsp_config_write(
	const AiusDspModel *model, const AiusOpticalDspConfigHeader *header,
	uint8_t *body, size_t capacity)
{
	static const char *const channel_names[6] = {"fl", "fr", "fc", "lfe", "sl", "sr"};
	char *output = (char *)body;
	size_t length = 0;

	if (model == NULL || header == NULL || body == NULL)
		return 0u;
	if (!dsp_json_append(output, capacity, &length,
	                     "{\"source\":\"%s\",\"config_generation\":%lu,\"staged_generation\":%lu,\"config_pending\":%s,\"sample_rate_hz\":%lu,",
	                     header->source != NULL ? header->source : "default",
	                     (unsigned long)header->config_generation,
	                     (unsigned long)header->staged_generation,
	                     header->config_pending ? "true" : "false",
	                     (unsigned long)model->sample_rate_hz))
		return 0;
	if (!dsp_json_append(output, capacity, &length,
	                     "\"features\":{\"input_eq\":%s,\"matrix\":%s,\"output_eq\":%s,\"limiter\":%s,\"meters\":%s},",
	                     model->features.input_eq ? "true" : "false",
	                     model->features.matrix ? "true" : "false",
	                     model->features.output_eq ? "true" : "false",
	                     model->features.limiter ? "true" : "false",
	                     model->features.meters ? "true" : "false"))
		return 0;
	if (!dsp_json_append(output, capacity, &length, "\"input_channels\":{"))
		return 0;
	for (uint8_t channel = 0; channel < 6u; channel++) {
		const AiusDspModelInputChannel *entry = &model->input_channels[channel];
		if (!dsp_json_append(output, capacity, &length,
		                     "%s\"%s\":{\"gain_db\":%.17g,\"eq\":",
		                     channel == 0 ? "" : ",", channel_names[channel],
		                     entry->gain_db))
			return 0;
		if (!dsp_eq_array(output, capacity, &length, entry->eq,
		                  entry->eq_count))
			return 0;
		if (!dsp_json_append(output, capacity, &length, "}"))
			return 0;
	}
	if (!dsp_json_append(output, capacity, &length, "},\"output_eq\":"))
		return 0;
	if (!dsp_eq_array(output, capacity, &length, model->output_eq,
	                  model->output_eq_count))
		return 0;
	/* Emit-what-you-parse: lfe_crossover_hz, layout_aware and
	 * surround_delay_ms are always emitted, so a GET-modify-PUT round trip
	 * cannot fail on a missing key or reset a hand-set value; model_origin
	 * is NOT served here (the PUT parser answers UNKNOWN_KEY for it). */
	if (!dsp_json_append(output, capacity, &length,
	                     ",\"matrix\":{\"center_gain_linear\":%.17g,\"surround_gain_linear\":%.17g,\"lfe_gain_linear\":%.17g,\"lfe_crossover_hz\":%.17g,\"headroom_linear\":%.17g,\"clipfree\":%s,\"layout_aware\":%s,\"surround_delay_ms\":%.17g},",
	                     model->matrix.center_gain_linear,
	                     model->matrix.surround_gain_linear,
	                     model->matrix.lfe_gain_linear,
	                     model->matrix.lfe_crossover_hz,
	                     model->matrix.headroom_linear,
	                     model->matrix.clipfree ? "true" : "false",
	                     model->matrix.layout_aware ? "true" : "false",
	                     model->matrix.surround_delay_ms))
		return 0;
	if (!dsp_json_append(output, capacity, &length,
	                     "\"limiter\":{\"enabled\":%s,\"threshold_dbfs\":%.17g,\"attack_ms\":%.17g,\"release_ms\":%.17g,\"lookahead_ms\":%.17g}",
	                     model->limiter.enabled ? "true" : "false",
	                     model->limiter.threshold_dbfs,
	                     model->limiter.attack_ms, model->limiter.release_ms,
	                     model->limiter.lookahead_ms))
		return 0;
	/* The file form: the load's parser (aius_dsp_config_parse_put) requires
	 * base_generation, so a dsp.json without it would be refused at the next
	 * mount (NS6). */
	if (header->file_form &&
	    !dsp_json_append(output, capacity, &length, ",\"base_generation\":%lu",
	                     (unsigned long)header->config_generation))
		return 0;
	if (!dsp_json_append(output, capacity, &length, "}"))
		return 0;
	return length;
}

/* ---- Phase 4h H10: the settings mount load and restore ---- */

enum {
	SETTINGS_VERDICT_NONE = 0,     /* nothing published (the job failed first) */
	SETTINGS_VERDICT_PARSED = 1,   /* parsed and applied */
	SETTINGS_VERDICT_REFUSED = 2,  /* bytes reached the parser and were refused */
	SETTINGS_VERDICT_SHADOWED = 3  /* the store shadows the target: untouched */
};

FLASHMEM void aius_optical_settings_load_begin(AiusOpticalSettingsMount *mount)
{
	mount->verdict = SETTINGS_VERDICT_NONE;
}

FLASHMEM bool aius_optical_settings_publish(AiusOpticalSettingsMount *mount,
                                            const AiusOpticalSettingsOps *ops,
                                            AiusOpticalSettingsFile file,
                                            bool backup, const uint8_t *data,
                                            size_t length)
{
	const bool shadowed = ops->shadowed(ops->context, file);
	if (!backup && shadowed) {
		/* Q1, B6: acknowledged, not applied; no defaults, no error. */
		mount->verdict = SETTINGS_VERDICT_SHADOWED;
		return true;
	}
	/* A backup whose target a write accepted since the failure now shadows
	 * is parsed for its verdict only, never applied or put. */
	if (!ops->load(ops->context, file, data, length, !shadowed)) {
		mount->verdict = SETTINGS_VERDICT_REFUSED;
		/* The target's one failure count (FR-S11); a refused backup
		 * counts nothing more. */
		if (!backup)
			ops->target_failed(ops->context, file);
		return true;
	}
	if (shadowed) {
		mount->verdict = SETTINGS_VERDICT_SHADOWED;
		return true;
	}
	if (backup)
		ops->put_restore(ops->context, file, data, length);
	mount->verdict = SETTINGS_VERDICT_PARSED;
	return true;
}

FLASHMEM bool aius_optical_settings_load_end(AiusOpticalSettingsMount *mount,
                                             const AiusOpticalSettingsOps *ops,
                                             AiusOpticalSettingsFile file,
                                             bool backup, bool job_ok)
{
	const uint8_t verdict = mount->verdict;
	mount->verdict = SETTINGS_VERDICT_NONE;
	if (!backup) {
		if (verdict == SETTINGS_VERDICT_SHADOWED ||
		    ops->shadowed(ops->context, file))
			return false;
		/* Failed before its bytes reached the parser (open, size,
		 * read): the same one fallback a refused parse took. */
		if (!job_ok && verdict == SETTINGS_VERDICT_NONE)
			ops->target_failed(ops->context, file);
		return verdict != SETTINGS_VERDICT_PARSED &&
		       ops->carve_present(ops->context);
	}
	AiusOpticalSettingsBackup result;
	switch (verdict) {
	case SETTINGS_VERDICT_PARSED:
		result = AIUS_OPTICAL_SETTINGS_BACKUP_PARSED;
		break;
	case SETTINGS_VERDICT_REFUSED:
		result = AIUS_OPTICAL_SETTINGS_BACKUP_REFUSED;
		break;
	case SETTINGS_VERDICT_SHADOWED:
		result = AIUS_OPTICAL_SETTINGS_BACKUP_NOT_READ;
		break;
	default:
		result = AIUS_OPTICAL_SETTINGS_BACKUP_LOAD_FAILED;
		break;
	}
	switch (ops->decide(false, result)) {
	case AIUS_OPTICAL_SETTINGS_RESTORE:
		mount->restores++;
		break;
	case AIUS_OPTICAL_SETTINGS_FAULT:
		mount->restore_faults++;
		break;
	default:
		break;
	}
	return false;
}

FLASHMEM bool aius_optical_app_set_sd_overlay(
	AiusOpticalAppDispatcher *dispatcher,
	const AiusOpticalAppSdOverlay *overlay)
{
	if (dispatcher == NULL || !dispatcher->valid)
		return false;
	s_sd_overlay = NULL;
	if (overlay == NULL)
		return true; /* deliberate removal */
	if (overlay->present == NULL || overlay->put_begin == NULL ||
	    overlay->put_abort == NULL || overlay->tombstone == NULL ||
	    overlay->drained == NULL || overlay->merge_listing == NULL)
		return false;
	s_sd_overlay = overlay;
	return true;
}

FLASHMEM bool aius_optical_app_set_web_cache(
	AiusOpticalAppDispatcher *dispatcher,
	const AiusOpticalAppWebCache *web_cache)
{
	if (dispatcher == NULL || !dispatcher->valid)
		return false;
	(void)web_lease_drop(dispatcher);
	s_web_cache = NULL;
	if (web_cache == NULL)
		return true; /* deliberate removal */
	if (web_cache->lookup == NULL || web_cache->lease == NULL ||
	    web_cache->release == NULL || web_cache->verify == NULL ||
	    web_cache->copy_chunk == NULL || web_cache->refill == NULL ||
	    web_cache->listing == NULL)
		return false;
	s_web_cache = web_cache;
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
 * X-Aius-SD-Auth exactly as the SD and config-mutation routes are.
 *
 * The read-only routes -- GET on /, /api/status, /api/events, /api/meters,
 * /api/dsp/config and /sd/ -- stay open
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
	       route == AIUS_HTTP_ROUTE_MEASURE_START ||
	       route == AIUS_HTTP_ROUTE_SD_MODE;
}

static FLASHMEM const AiusHttpApplicationResponse *http_dispatch_route(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request);

/* Phase 4f F5: the optional begin/end pair brackets the whole route decision,
 * so the composition's meter sees every stretch the dispatch runs. */
FLASHMEM const AiusHttpApplicationResponse *aius_optical_app_http_dispatch(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app;
	const AiusHttpApplicationResponse *response;
	if (dispatcher == NULL || !dispatcher->valid || request == NULL)
		return NULL;
	app = &dispatcher->app;
	dispatcher->http_dispatches++;
	if (app->dispatch_begin != NULL)
		app->dispatch_begin(app->context);
	response = http_dispatch_route(dispatcher, request);
	if (app->dispatch_end != NULL)
		app->dispatch_end(app->context, request->route, request->method);
	return response;
}

static FLASHMEM const AiusHttpApplicationResponse *http_dispatch_route(
	AiusOpticalAppDispatcher *dispatcher, const AiusHttpRequest *request)
{
	const AiusOpticalApp *app = &dispatcher->app;
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
	case AIUS_HTTP_ROUTE_STATUS_BRIEF:
		if (app->write_status_brief == NULL)
			break;
		return http_serialized(dispatcher, kContentJson,
		                       app->write_status_brief(app->context,
		                                               dispatcher->body,
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
		return dispatch_sd(dispatcher, request);
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
	case AIUS_HTTP_ROUTE_SD_MODE:
		return dispatch_sd_mode(dispatcher, request);
	case AIUS_HTTP_ROUTE_REBOOT:
		/* Phase 4h (FR-S8): no reboot over an unflushed write. */
		if (sd_flush_pending())
			return http_error(dispatcher, 409, kSdFlushPending);
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
	case AIUS_HTTP_ROUTE_OTA_BOOT_OTHER:
		return dispatch_ota(dispatcher, AIUS_OPTICAL_APP_OTA_ROUTE_BOOT_OTHER,
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

/* ------------------------------------------------------------------ */
/* Admission (Phase 4f)                                                */
/* ------------------------------------------------------------------ */

#ifndef PROGMEM
#define PROGMEM
#endif

/* F8: each class's declared seed, the estimated longest stretch (us) its
 * dispatch runs between yield points, judged until the first measurement.
 * PROGMEM: flash, not DTCM .rodata (4e §Memory, the .data cliff). */
static const uint16_t kAdmitSeedUs[AIUS_OPTICAL_ADMIT_CLASSES] PROGMEM = {
	600u,   /* STATUS: one append after the per-append yields */
	200u,   /* BRIEF: one short snprintf */
	400u,   /* METERS */
	600u,   /* EVENTS: one page */
	2000u,  /* SETTINGS: a body parse with no yield point (F7) */
	1500u,  /* DIRECTORY: one raw directory read between card waits */
	500u,   /* SD start, DMA: one window stretch between hooks */
	20000u  /* SD start, FIFO: a whole card primitive */
};

#define ADMIT_COUNT_MASK 0x1fu
#define ADMIT_OLDEST_SHIFT 5u

static FLASHMEM void admit_seed(AiusOpticalCostWindow *window, uint16_t seed)
{
	for (unsigned i = 0; i < AIUS_OPTICAL_ADMIT_SLOTS; i++)
		window->slot[i] = seed;
	window->count = 0u;
	window->seeded = 1u;
	window->opened_ms = 0u;
}

/* F-B1: every whole 2,000 ms since the current bucket opened retires it over
 * the oldest completed one and opens an empty one, so a window ages in wall
 * time whether or not its class adds samples. Five or more periods, or every
 * slot aged to 0, leave no measurement in the window: it returns to its seed
 * (F8, R4-S1). A seeded window has no bucket open and does not age. */
static FLASHMEM void admit_age(AiusOpticalCostWindow *w, unsigned klass,
                               uint32_t now_ms)
{
	uint32_t periods;
	unsigned oldest;
	uint16_t any = 0u;

	if (w->seeded != 0u)
		return;
	periods = (uint32_t)(now_ms - w->opened_ms) / AIUS_OPTICAL_ADMIT_BUCKET_MS;
	if (periods == 0u)
		return;
	if (periods >= AIUS_OPTICAL_ADMIT_SLOTS) {
		admit_seed(w, kAdmitSeedUs[klass]);
		return;
	}
	oldest = (unsigned)(w->count >> ADMIT_OLDEST_SHIFT) & 3u;
	for (uint32_t p = 0; p < periods; p++) {
		w->slot[oldest] = w->slot[AIUS_OPTICAL_ADMIT_SLOTS - 1u];
		oldest = (oldest + 1u) & 3u;
		w->slot[AIUS_OPTICAL_ADMIT_SLOTS - 1u] = 0u;
	}
	for (unsigned i = 0; i < AIUS_OPTICAL_ADMIT_SLOTS; i++)
		any |= w->slot[i];
	if (any == 0u) {
		admit_seed(w, kAdmitSeedUs[klass]);
		return;
	}
	w->count = (uint8_t)(oldest << ADMIT_OLDEST_SHIFT);
	w->opened_ms += periods * AIUS_OPTICAL_ADMIT_BUCKET_MS;
}

FLASHMEM void aius_optical_admission_age(AiusOpticalAdmission *admission,
                                         uint32_t now_ms)
{
	if (admission == NULL)
		return;
	for (unsigned c = 0; c < AIUS_OPTICAL_ADMIT_CLASSES; c++)
		admit_age(&admission->window[c], c, now_ms);
}

FLASHMEM void aius_optical_admission_init(AiusOpticalAdmission *admission)
{
	if (admission == NULL)
		return;
	memset(admission, 0, sizeof(*admission));
	for (unsigned c = 0; c < AIUS_OPTICAL_ADMIT_CLASSES; c++)
		admit_seed(&admission->window[c], kAdmitSeedUs[c]);
}

FLASHMEM void aius_optical_admission_clear(AiusOpticalAdmission *admission)
{
	if (admission == NULL)
		return;
	memset(admission->shed, 0, sizeof(admission->shed));
	admission->dispatch_max_us = 0u;
	admission->sd_fifo_defers = 0u;
}

FLASHMEM AiusOpticalAdmitClass aius_optical_admit_route_class(
	AiusHttpRoute route, AiusHttpMethod method)
{
	switch (route) {
	case AIUS_HTTP_ROUTE_STATUS:
		return method == AIUS_HTTP_GET ? AIUS_OPTICAL_ADMIT_STATUS
		                               : AIUS_OPTICAL_ADMIT_NONE;
	case AIUS_HTTP_ROUTE_STATUS_BRIEF:
		return method == AIUS_HTTP_GET ? AIUS_OPTICAL_ADMIT_BRIEF
		                               : AIUS_OPTICAL_ADMIT_NONE;
	case AIUS_HTTP_ROUTE_METERS:
		return method == AIUS_HTTP_GET ? AIUS_OPTICAL_ADMIT_METERS
		                               : AIUS_OPTICAL_ADMIT_NONE;
	case AIUS_HTTP_ROUTE_EVENTS:
		return method == AIUS_HTTP_GET ? AIUS_OPTICAL_ADMIT_EVENTS
		                               : AIUS_OPTICAL_ADMIT_NONE;
	case AIUS_HTTP_ROUTE_DSP_CONFIG:
	case AIUS_HTTP_ROUTE_CONFIG_PUBLIC:
		/* The GET is a one-shot serialise a 503 would turn into a failed
		 * page load (F13); only the PUT parse is a class. */
		return method == AIUS_HTTP_PUT ? AIUS_OPTICAL_ADMIT_SETTINGS
		                               : AIUS_OPTICAL_ADMIT_NONE;
	default:
		return AIUS_OPTICAL_ADMIT_NONE;
	}
}

FLASHMEM void aius_optical_admission_record(AiusOpticalAdmission *admission,
                                            AiusOpticalAdmitClass klass,
                                            uint32_t us, uint32_t now_ms)
{
	AiusOpticalCostWindow *w;
	uint16_t v;
	unsigned count, oldest;

	if (admission == NULL || (unsigned)klass >= AIUS_OPTICAL_ADMIT_CLASSES)
		return;
	w = &admission->window[klass];
	admit_age(w, (unsigned)klass, now_ms);
	v = us > 0xffffu ? (uint16_t)0xffffu : (uint16_t)us;
	if (w->seeded != 0u) {
		/* F8: the first measurement replaces the seed in every slot and
		 * opens the first bucket. */
		for (unsigned i = 0; i < AIUS_OPTICAL_ADMIT_SLOTS; i++)
			w->slot[i] = v;
		w->seeded = 0u;
		w->count = 1u;
		w->opened_ms = now_ms;
		return;
	}
	count = w->count & ADMIT_COUNT_MASK;
	oldest = (unsigned)(w->count >> ADMIT_OLDEST_SHIFT) & 3u;
	if (count >= AIUS_OPTICAL_ADMIT_BUCKET) {
		/* Retire the full current bucket over the oldest completed one;
		 * the next bucket opens with this sample. */
		w->slot[oldest] = w->slot[AIUS_OPTICAL_ADMIT_SLOTS - 1u];
		oldest = (oldest + 1u) & 3u;
		count = 0u;
		w->opened_ms = now_ms;
	}
	if (count == 0u || v > w->slot[AIUS_OPTICAL_ADMIT_SLOTS - 1u])
		w->slot[AIUS_OPTICAL_ADMIT_SLOTS - 1u] = v;
	count++;
	w->count = (uint8_t)(count | (oldest << ADMIT_OLDEST_SHIFT));
}

FLASHMEM void aius_optical_admission_note_dispatch(
	AiusOpticalAdmission *admission, AiusHttpRoute route,
	AiusHttpMethod method, uint32_t us, uint32_t now_ms)
{
	if (admission == NULL)
		return;
	if (us > admission->dispatch_max_us)
		admission->dispatch_max_us = us;
	aius_optical_admission_record(admission,
	                              aius_optical_admit_route_class(route, method),
	                              us, now_ms);
}

FLASHMEM uint32_t aius_optical_admission_cost_us(
	const AiusOpticalAdmission *admission, AiusOpticalAdmitClass klass)
{
	uint16_t cost = 0u;
	if (admission == NULL || (unsigned)klass >= AIUS_OPTICAL_ADMIT_CLASSES)
		return 0u;
	for (unsigned i = 0; i < AIUS_OPTICAL_ADMIT_SLOTS; i++)
		if (admission->window[klass].slot[i] > cost)
			cost = admission->window[klass].slot[i];
	return cost;
}

FLASHMEM bool aius_optical_admission_short(
	const AiusOpticalAdmission *admission, AiusOpticalAdmitClass klass,
	const AiusOpticalAdmitSample *sample)
{
	int64_t margin, need;
	if (admission == NULL || sample == NULL || !sample->live_owns_ring ||
	    (unsigned)klass >= AIUS_OPTICAL_ADMIT_CLASSES)
		return false;
	margin = (int64_t)sample->level_frames - AIUS_OPTICAL_ADMIT_UNDERRUN_FRAMES;
	need = ((int64_t)aius_optical_admission_cost_us(admission, klass) *
	                AIUS_OPTICAL_ADMIT_FRAMES_PER_MS + 999) / 1000 +
	       AIUS_OPTICAL_ADMIT_GUARD_FRAMES;
	return margin < need;
}

FLASHMEM AiusOpticalAdmitVerdict aius_optical_admission_route(
	AiusOpticalAdmission *admission, AiusHttpRoute route,
	AiusHttpMethod method, const AiusOpticalAdmitSample *sample)
{
	AiusOpticalAdmitClass klass = aius_optical_admit_route_class(route, method);
	if (klass == AIUS_OPTICAL_ADMIT_NONE || admission == NULL || sample == NULL)
		return AIUS_OPTICAL_ADMIT_VERDICT_ADMIT;
	/* F-B1: a shed adds no sample, so the window ages here too. */
	admit_age(&admission->window[klass], (unsigned)klass, sample->now_ms);
	if (!aius_optical_admission_short(admission, klass, sample))
		return AIUS_OPTICAL_ADMIT_VERDICT_ADMIT;
	admission->shed[klass]++;
	return AIUS_OPTICAL_ADMIT_VERDICT_SHED;
}

FLASHMEM bool aius_optical_admission_sd_fifo_defer(
	AiusOpticalAdmission *admission, const AiusOpticalAdmitSample *sample)
{
	if (admission == NULL || sample == NULL || !sample->fifo_mode ||
	    admission->bringup_loaded == 0u)
		return false;
	admit_age(&admission->window[AIUS_OPTICAL_ADMIT_SD_START_FIFO],
	          AIUS_OPTICAL_ADMIT_SD_START_FIFO, sample->now_ms);
	if (!aius_optical_admission_short(admission,
	                                  AIUS_OPTICAL_ADMIT_SD_START_FIFO, sample))
		return false;
	admission->sd_fifo_defers++;
	return true;
}

FLASHMEM void aius_optical_admission_note_bringup_loaded(
	AiusOpticalAdmission *admission)
{
	if (admission != NULL)
		admission->bringup_loaded = 1u;
}

FLASHMEM bool aius_optical_app_sd_staging_orphaned(
	const AiusHttpTransaction *http, bool start_held, bool start_queued)
{
	if (http == NULL || start_held || start_queued)
		return false;
	return http->state != AIUS_HTTP_TX_DISPATCH &&
	       http->state != AIUS_HTTP_TX_SD_START &&
	       http->state != AIUS_HTTP_TX_SD_TRANSFER;
}

FLASHMEM bool aius_optical_app_sd_job_is_ota_staging(const AiusSdJobRequest *job)
{
	static const char kName[] PROGMEM = AIUS_OPTICAL_APP_OTA_STAGED_NAME;
	size_t length, name = sizeof(kName) - 1u;
	if (job == NULL || job->kind != AIUS_SD_JOB_UPLOAD ||
	    job->target_path == NULL)
		return false;
	length = strlen(job->target_path);
	return length >= name &&
	       memcmp(job->target_path + length - name, kName, name) == 0;
}

FLASHMEM AiusOpticalAdmitVerdict aius_optical_admission_sd_start(
	AiusOpticalAdmission *admission, const AiusSdJobRequest *job,
	const AiusOpticalAdmitSample *sample, bool reconsult)
{
	AiusOpticalAdmitClass klass;
	if (admission == NULL || sample == NULL ||
	    aius_optical_app_sd_job_is_ota_staging(job))
		return AIUS_OPTICAL_ADMIT_VERDICT_ADMIT;
	klass = sample->fifo_mode ? AIUS_OPTICAL_ADMIT_SD_START_FIFO
	                          : AIUS_OPTICAL_ADMIT_SD_START_DMA;
	admit_age(&admission->window[klass], (unsigned)klass, sample->now_ms);
	/* F-S2: the timer belongs to the held start; a first consult is a
	 * different start (a teardown may have dropped the held one). */
	if (!reconsult)
		admission->defer_timing = 0u;
	if (sample->fifo_mode) {
		/* sd_fifo_defer()'s predicate; the held epoch is counted only
		 * when the verdict is DEFER. */
		if (!aius_optical_admission_short(admission, klass, sample)) {
			admission->defer_timing = 0u;
			return AIUS_OPTICAL_ADMIT_VERDICT_ADMIT;
		}
		if (admission->defer_timing == 0u) {
			admission->defer_timing = 1u;
			admission->defer_started_ms = sample->now_ms;
		}
		if ((uint32_t)(sample->now_ms - admission->defer_started_ms) <
		    AIUS_OPTICAL_ADMIT_SD_DEFER_MS) {
			admission->sd_fifo_defers++;
			return AIUS_OPTICAL_ADMIT_VERDICT_DEFER;
		}
	} else if (!aius_optical_admission_short(admission, klass, sample)) {
		admission->defer_timing = 0u;
		return AIUS_OPTICAL_ADMIT_VERDICT_ADMIT;
	}
	admission->defer_timing = 0u;
	admission->shed[klass]++;
	return AIUS_OPTICAL_ADMIT_VERDICT_SHED;
}

/* ------------------------------------------------------------------ */
/* Phase 4g G6, G9: the web-cache fill hold (see aius_optical_app.h)   */
/* ------------------------------------------------------------------ */

FLASHMEM void aius_optical_web_fill_init(AiusOpticalWebFill *fill)
{
	if (fill != NULL)
		memset(fill, 0, sizeof(*fill));
}

/* Whether the armed expiry is still ahead of `now_ms`. Every expiry is set
 * AIUS_OPTICAL_WEB_FILL_HOLD_MS after the `now_ms` of its call, so a live hold
 * is at most that far ahead; the window allows one more hold length for a
 * caller's stale stamp (R5-S1's "only later"). Anything else has lapsed,
 * however long ago: a signed difference would read a hold that lapsed more
 * than 2^31 ms back (warm-cache hits never call _note_http_complete, so long
 * idle gaps are the steady state) as live again. A gap within the window of
 * a whole 2^32 ms wrap aliases for at most 4 s. */
static FLASHMEM bool web_fill_live(const AiusOpticalWebFill *fill,
                                   uint32_t now_ms)
{
	const uint32_t ahead = fill->hold_until_ms - now_ms;
	return fill->armed && ahead != 0u &&
	       ahead <= 2u * AIUS_OPTICAL_WEB_FILL_HOLD_MS;
}

/* Sets the expiry to AIUS_OPTICAL_WEB_FILL_HOLD_MS after `now_ms`; a live
 * hold's expiry only moves later (R5-S1), a lapsed one is replaced. */
static FLASHMEM void web_fill_extend(AiusOpticalWebFill *fill, uint32_t now_ms)
{
	const uint32_t until = now_ms + AIUS_OPTICAL_WEB_FILL_HOLD_MS;
	if (!web_fill_live(fill, now_ms) ||
	    (int32_t)(until - fill->hold_until_ms) > 0)
		fill->hold_until_ms = until;
	fill->armed = 1u;
}

FLASHMEM bool aius_optical_web_fill_cancel(AiusOpticalWebFill *fill,
                                           bool fill_in_flight)
{
	if (fill == NULL)
		return false;
	if (fill->latches != UINT8_MAX)
		fill->latches++;
	if (fill->holds != UINT32_MAX)
		fill->holds++;
	if (fill_in_flight && fill->cancels != UINT32_MAX)
		fill->cancels++;
	return fill_in_flight;
}

FLASHMEM void aius_optical_web_fill_hold_release(AiusOpticalWebFill *fill,
                                                 uint32_t now_ms)
{
	if (fill == NULL)
		return;
	if (fill->latches != 0u)
		fill->latches--;
	web_fill_extend(fill, now_ms);
}

FLASHMEM void aius_optical_web_fill_note_http_complete(
	AiusOpticalWebFill *fill, uint32_t now_ms)
{
	if (fill == NULL)
		return;
	if (fill->http_canceller) {
		fill->http_canceller = 0u;
		aius_optical_web_fill_hold_release(fill, now_ms);
		return;
	}
	web_fill_extend(fill, now_ms);
}

FLASHMEM void aius_optical_web_fill_drop_http_canceller(
	AiusOpticalWebFill *fill, uint32_t now_ms)
{
	if (fill == NULL || !fill->http_canceller)
		return;
	fill->http_canceller = 0u;
	fill->http_defer = 0u;
	aius_optical_web_fill_hold_release(fill, now_ms);
}

FLASHMEM bool aius_optical_web_fill_held(const AiusOpticalWebFill *fill,
                                         uint32_t now_ms)
{
	if (fill == NULL)
		return false;
	return fill->latches != 0u || web_fill_live(fill, now_ms);
}

FLASHMEM bool aius_optical_web_fill_paused(const AiusOpticalWebFill *fill,
                                           uint32_t now_ms)
{
	return aius_optical_web_fill_held(fill, now_ms);
}

FLASHMEM bool aius_optical_web_fill_sd_start(AiusOpticalWebFill *fill,
                                             bool fill_in_flight,
                                             bool *cancel)
{
	if (cancel != NULL)
		*cancel = false;
	if (fill == NULL || !fill_in_flight)
		return false;
	/* The first consult of this held start cancels: the caller calls its
	 * one cancel entry (which latches through aius_optical_web_fill_cancel),
	 * and this request's completion is that latch's release. */
	if (!fill->http_canceller) {
		fill->http_canceller = 1u;
		if (cancel != NULL)
			*cancel = true;
	}
	fill->http_defer = 1u;
	return true;
}

FLASHMEM bool aius_optical_web_fill_take_defer(AiusOpticalWebFill *fill)
{
	bool held;
	if (fill == NULL)
		return false;
	held = fill->http_defer != 0u;
	fill->http_defer = 0u;
	return held;
}

FLASHMEM bool aius_optical_app_route_reads_card(const AiusHttpRequest *request)
{
	if (request == NULL)
		return false;
	return request->route == AIUS_HTTP_ROUTE_SD_FILE &&
	       request->method == AIUS_HTTP_GET && resource_is_directory(request);
}

FLASHMEM bool aius_optical_web_fill_route(AiusOpticalWebFill *fill,
                                          bool fill_in_flight, uint32_t now_ms,
                                          bool *cancel)
{
	if (cancel != NULL)
		*cancel = false;
	if (fill == NULL)
		return false;
	/* The same wait as an HTTP SD start: cancel once, hold until stopped. */
	if (aius_optical_web_fill_sd_start(fill, fill_in_flight, cancel))
		return true;
	/* Stopped (or never in flight): a held dispatch's canceller ends its
	 * latch now, since the route runs to completion in this call. */
	if (aius_optical_web_fill_take_defer(fill) && fill->http_canceller) {
		fill->http_canceller = 0u;
		aius_optical_web_fill_hold_release(fill, now_ms);
	}
	fill->card_route = 1u;
	return false;
}

FLASHMEM void aius_optical_web_fill_route_end(AiusOpticalWebFill *fill,
                                              uint32_t now_ms)
{
	if (fill == NULL || fill->card_route == 0u)
		return;
	fill->card_route = 0u;
	web_fill_extend(fill, now_ms);
}

FLASHMEM bool aius_optical_web_fill_follow_card(bool *walk_card,
                                                uint32_t *mounts_seen,
                                                bool ready, bool mounted,
                                                uint32_t mounts)
{
	if (walk_card == NULL || mounts_seen == NULL)
		return false;
	if (!ready || !mounted) {
		/* Card gone: empty now; the walk waits for the next mount. */
		if (!*walk_card)
			return false;
		*walk_card = false;
		return true;
	}
	if (*walk_card && mounts == *mounts_seen)
		return false;
	/* A first or new mount: the walk restarts on it, from an empty cache. */
	*walk_card = true;
	*mounts_seen = mounts;
	return true;
}
