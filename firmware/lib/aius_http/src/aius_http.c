#include "aius_http.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Cold code -> FLASH. Nothing here runs on the audio block path (this is
 * per-request HTTP parsing and response framing), so every definition below
 * carries FLASHMEM and lands in FLASH instead of the shared FlexRAM/ITCM pool
 * the audio pipeline needs. FLASHMEM is Teensyduino's
 * __attribute__((section(".flashmem"))) from <avr/pgmspace.h>; TEENSYDUINO is a
 * project-wide compiler -D, so the native host suites fall through to the no-op
 * and this file keeps its portable, Arduino-free compile. The attribute must
 * sit on each DEFINITION: GCC does not merge a `section` attribute from a
 * forward declaration, so a prototype-only FLASHMEM silently leaves the code in
 * ITCM. Enforced post-link by test/itcm-placement/verify_target.py. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

static FLASHMEM const uint8_t *find_bytes(const uint8_t *data, size_t length,
	                                const char *needle, size_t needle_length)
{
	if (needle_length == 0 || length < needle_length)
		return NULL;
	for (size_t i = 0; i + needle_length <= length; i++)
		if (memcmp(data + i, needle, needle_length) == 0)
			return data + i;
	return NULL;
}

static FLASHMEM int has_prefix(const uint8_t *value, size_t length,
	                     const char *prefix)
{
	size_t prefix_length = strlen(prefix);
	return length >= prefix_length && memcmp(value, prefix, prefix_length) == 0;
}

static FLASHMEM int decimal_u32(const uint8_t *value, size_t length, uint32_t *result)
{
	uint32_t parsed = 0;
	if (length == 0 || length > 10)
		return 0;
	for (size_t i = 0; i < length; i++) {
		if (value[i] < '0' || value[i] > '9')
			return 0;
		uint32_t digit = (uint32_t)(value[i] - '0');
		if (parsed > (UINT32_MAX - digit) / 10u)
			return 0;
		parsed = parsed * 10u + digit;
	}
	*result = parsed;
	return 1;
}

static FLASHMEM int decimal_u64(const uint8_t *value, size_t length, uint64_t *result)
{
	uint64_t parsed = 0;
	if (length == 0)
		return 0;
	for (size_t i = 0; i < length; i++) {
		uint64_t digit;
		if (value[i] < '0' || value[i] > '9')
			return 0;
		digit = (uint64_t)(value[i] - '0');
		if (parsed > (UINT64_MAX - digit) / UINT64_C(10))
			return 0;
		parsed = parsed * UINT64_C(10) + digit;
	}
	*result = parsed;
	return 1;
}

/* RFC 9110 5.1: field names are case-insensitive. Fold ASCII A-Z only -- never
 * tolower(), whose locale dependence would fold non-ASCII bytes, and never the
 * `| 0x20` shortcut, which also equates byte pairs differing solely in bit 5
 * ('-' 0x2D / CR 0x0D, '_' 0x5F / '?' 0x3F) and would match a corrupted name.
 * Field VALUES stay byte-exact: only the name passes through here. */
static FLASHMEM int ascii_equal_fold(uint8_t left, char right)
{
	if (left >= 'A' && left <= 'Z')
		left = (uint8_t)(left + ('a' - 'A'));
	if (right >= 'A' && right <= 'Z')
		right = (char)(right + ('a' - 'A'));
	return left == (uint8_t)right;
}

static FLASHMEM int header_name_equals(const uint8_t *line, size_t length,
	                          const char *name)
{
	size_t name_length = strlen(name);
	if (length != name_length)
		return 0;
	for (size_t i = 0; i < length; i++)
		if (!ascii_equal_fold(line[i], name[i]))
			return 0;
	return 1;
}

static FLASHMEM int token_equals(const uint8_t *value, size_t length, const char *token)
{
	size_t token_length = strlen(token);
	return length == token_length && memcmp(value, token, token_length) == 0;
}

static FLASHMEM AiusHttpRoute route_for(const uint8_t *path, size_t length,
	                             AiusHttpRequest *request)
{
	if (token_equals(path, length, "/"))
		return AIUS_HTTP_ROUTE_HOME;
	if (token_equals(path, length, "/api/status"))
		return AIUS_HTTP_ROUTE_STATUS;
	if (token_equals(path, length, "/api/events")) {
		request->cursor = 0;
		return AIUS_HTTP_ROUTE_EVENTS;
	}
	if (has_prefix(path, length, "/api/events?cursor=")) {
		const uint8_t *cursor = path + strlen("/api/events?cursor=");
		size_t cursor_length = length - strlen("/api/events?cursor=");
		return decimal_u32(cursor, cursor_length, &request->cursor) ?
			AIUS_HTTP_ROUTE_EVENTS : AIUS_HTTP_ROUTE_UNKNOWN;
	}
	if (token_equals(path, length, "/api/start"))
		return AIUS_HTTP_ROUTE_START;
	if (token_equals(path, length, "/api/stop"))
		return AIUS_HTTP_ROUTE_STOP;
	if (token_equals(path, length, "/api/clear"))
		return AIUS_HTTP_ROUTE_CLEAR;
	if (token_equals(path, length, "/api/reboot"))
		return AIUS_HTTP_ROUTE_REBOOT;
	if (token_equals(path, length, "/api/config/public"))
		return AIUS_HTTP_ROUTE_CONFIG_PUBLIC;
	if (token_equals(path, length, "/api/dsp/config"))
		return AIUS_HTTP_ROUTE_DSP_CONFIG;
	if (token_equals(path, length, "/api/meters"))
		return AIUS_HTTP_ROUTE_METERS;
	if (token_equals(path, length, "/api/status/brief"))
		return AIUS_HTTP_ROUTE_STATUS_BRIEF;
	if (token_equals(path, length, "/api/replays"))
		return AIUS_HTTP_ROUTE_REPLAYS;
	if (token_equals(path, length, "/api/replays/status"))
		return AIUS_HTTP_ROUTE_REPLAYS_STATUS;
	if (token_equals(path, length, "/api/replays/start"))
		return AIUS_HTTP_ROUTE_REPLAYS_START;
	if (token_equals(path, length, "/api/replays/stop"))
		return AIUS_HTTP_ROUTE_REPLAYS_STOP;
	if (token_equals(path, length, "/api/measure"))
		return AIUS_HTTP_ROUTE_MEASURE;
	if (token_equals(path, length, "/api/measure/start"))
		return AIUS_HTTP_ROUTE_MEASURE_START;
	if (token_equals(path, length, "/api/ota"))
		return AIUS_HTTP_ROUTE_OTA;
	if (token_equals(path, length, "/api/ota/image"))
		return AIUS_HTTP_ROUTE_OTA_IMAGE;
	if (token_equals(path, length, "/api/ota/arm"))
		return AIUS_HTTP_ROUTE_OTA_ARM;
	if (token_equals(path, length, "/api/ota/verify"))
		return AIUS_HTTP_ROUTE_OTA_VERIFY;
	if (token_equals(path, length, "/api/ota/abort"))
		return AIUS_HTTP_ROUTE_OTA_ABORT;
	if (token_equals(path, length, "/api/ota/activate"))
		return AIUS_HTTP_ROUTE_OTA_ACTIVATE;
	if (token_equals(path, length, "/api/ota/enable-activation"))
		return AIUS_HTTP_ROUTE_OTA_ENABLE_ACTIVATION;
	if (token_equals(path, length, "/api/ota/self-test-flash"))
		return AIUS_HTTP_ROUTE_OTA_SELF_TEST_FLASH;
	if (token_equals(path, length, "/api/ota/rehearse"))
		return AIUS_HTTP_ROUTE_OTA_REHEARSE;
	if (token_equals(path, length, "/api/ota/rehearse-fault"))
		return AIUS_HTTP_ROUTE_OTA_REHEARSE_FAULT;
	if (token_equals(path, length, "/api/recorder"))
		return AIUS_HTTP_ROUTE_RECORDER;
	if (token_equals(path, length, "/api/recorder/freeze"))
		return AIUS_HTTP_ROUTE_RECORDER_FREEZE;
	if (token_equals(path, length, "/api/recorder/thaw"))
		return AIUS_HTTP_ROUTE_RECORDER_THAW;
	if (token_equals(path, length, "/api/recorder/config"))
		return AIUS_HTTP_ROUTE_RECORDER_CONFIG;
	if (has_prefix(path, length, "/api/recorder/data?offset=")) {
		/* Fixed parameter order: offset, length, generation all required,
		 * an optional &slot=<n> last -- the events cursor's rigidity: a
		 * bounded parser, not a query engine. An unmodified single-shot
		 * client's request shape is byte-for-byte unchanged; the handler
		 * resolves an absent slot to the NEWEST FROZEN slot (409 when none
		 * is frozen), which is what "read the capture that just froze"
		 * means once a bank replaces the single ring. */
		const uint8_t *value = path + strlen("/api/recorder/data?offset=");
		size_t remaining = length - strlen("/api/recorder/data?offset=");
		const uint8_t *amp = memchr(value, '&', remaining);
		if (amp == NULL ||
		    !decimal_u32(value, (size_t)(amp - value),
		                 &request->recorder_offset))
			return AIUS_HTTP_ROUTE_UNKNOWN;
		remaining -= (size_t)(amp - value);
		if (!has_prefix(amp, remaining, "&length="))
			return AIUS_HTTP_ROUTE_UNKNOWN;
		value = amp + strlen("&length=");
		remaining -= strlen("&length=");
		amp = memchr(value, '&', remaining);
		if (amp == NULL ||
		    !decimal_u32(value, (size_t)(amp - value),
		                 &request->recorder_length))
			return AIUS_HTTP_ROUTE_UNKNOWN;
		remaining -= (size_t)(amp - value);
		if (!has_prefix(amp, remaining, "&generation="))
			return AIUS_HTTP_ROUTE_UNKNOWN;
		value = amp + strlen("&generation=");
		remaining -= strlen("&generation=");
		/* generation runs to the next '&' (a slot param follows) or to the
		 * end (no slot -- default to the active slot). */
		amp = memchr(value, '&', remaining);
		if (amp == NULL)
			return decimal_u32(value, remaining,
			                   &request->recorder_generation) ?
				AIUS_HTTP_ROUTE_RECORDER_DATA : AIUS_HTTP_ROUTE_UNKNOWN;
		if (!decimal_u32(value, (size_t)(amp - value),
		                 &request->recorder_generation))
			return AIUS_HTTP_ROUTE_UNKNOWN;
		remaining -= (size_t)(amp - value);
		if (!has_prefix(amp, remaining, "&slot="))
			return AIUS_HTTP_ROUTE_UNKNOWN;
		value = amp + strlen("&slot=");
		remaining -= strlen("&slot=");
		if (!decimal_u32(value, remaining, &request->recorder_slot))
			return AIUS_HTTP_ROUTE_UNKNOWN;
		request->recorder_has_slot = true;
		return AIUS_HTTP_ROUTE_RECORDER_DATA;
	}
	if (token_equals(path, length, "/api/sd"))
		return AIUS_HTTP_ROUTE_SD_STATUS;
	if (token_equals(path, length, "/api/sd/mode"))
		return AIUS_HTTP_ROUTE_SD_MODE;
	if (has_prefix(path, length, "/sd/replays/") &&
	    length > strlen("/sd/replays/")) {
		request->replay_id = path + strlen("/sd/replays/");
		request->replay_id_length = length - strlen("/sd/replays/");
		return AIUS_HTTP_ROUTE_SD_REPLAY;
	}
	if (has_prefix(path, length, "/sd/") && length > strlen("/sd/")) {
		request->resource = path + strlen("/sd/");
		request->resource_length = length - strlen("/sd/");
		return AIUS_HTTP_ROUTE_SD_FILE;
	}
	return AIUS_HTTP_ROUTE_UNKNOWN;
}

static FLASHMEM AiusHttpResult extract_content_range(const uint8_t *headers, size_t length,
	                                         AiusHttpRequest *request)
{
	/* Name folds (RFC 9110 5.1); the unit token stays byte-exact, so a
	 * "BYTES" unit is still an unrecognised range and is ignored, as before. */
	static const char value_prefix[] = " bytes ";
	const size_t value_prefix_length = sizeof(value_prefix) - 1u;
	const uint8_t *cursor = headers;
	const uint8_t *end = headers + length;
	while (cursor < end) {
		const uint8_t *line_end = find_bytes(cursor, (size_t)(end - cursor), "\r\n", 2);
		const uint8_t *colon;
		if (line_end == NULL) line_end = end;
		size_t line_length = (size_t)(line_end - cursor);
		colon = (const uint8_t *)memchr(cursor, ':', line_length);
		if (colon != NULL &&
		    header_name_equals(cursor, (size_t)(colon - cursor), "Content-Range") &&
		    (size_t)(line_end - (colon + 1)) >= value_prefix_length &&
		    memcmp(colon + 1, value_prefix, value_prefix_length) == 0) {
			const uint8_t *value = colon + 1 + value_prefix_length;
			const uint8_t *dash = (const uint8_t *)memchr(value, '-',
				(size_t)(line_end - value));
			const uint8_t *slash = dash == NULL ? NULL : (const uint8_t *)memchr(dash + 1,
				'/', (size_t)(line_end - (dash + 1)));
			if (dash == NULL || slash == NULL ||
			    !decimal_u32(value, (size_t)(dash - value), &request->content_range_start) ||
			    !decimal_u32(dash + 1, (size_t)(slash - (dash + 1)), &request->content_range_end) ||
			    !decimal_u32(slash + 1, (size_t)(line_end - (slash + 1)), &request->content_range_total) ||
			    request->content_range_start > request->content_range_end ||
			    request->content_range_total == 0u ||
			    request->content_range_end >= request->content_range_total)
				return AIUS_HTTP_ERR_BODY;
			request->has_content_range = 1u;
		}
		if (line_end == end) break;
		cursor = line_end + 2;
	}
	return AIUS_HTTP_OK;
}

static FLASHMEM AiusHttpResult extract_framing(const uint8_t *headers, size_t length,
	                                  AiusHttpRequest *request)
{
	const uint8_t *cursor = headers;
	const uint8_t *end = headers + length;

	while (cursor < end) {
		const uint8_t *line_end = find_bytes(cursor, (size_t)(end - cursor),
		                                     "\r\n", 2);
		const uint8_t *colon;
		size_t name_length;
		if (line_end == NULL)
			line_end = end;
		colon = (const uint8_t *)memchr(cursor, ':',
		                              (size_t)(line_end - cursor));
		if (colon != NULL) {
			name_length = (size_t)(colon - cursor);
			if (header_name_equals(cursor, name_length, "Transfer-Encoding"))
				return AIUS_HTTP_ERR_MALFORMED;
			if (header_name_equals(cursor, name_length, "Content-Length")) {
				const uint8_t *value = colon + 1;
				if (request->has_content_length)
					return AIUS_HTTP_ERR_MALFORMED;
				if (value == line_end || *value != ' ')
					return AIUS_HTTP_ERR_MALFORMED;
				value++;
				if (!decimal_u64(value, (size_t)(line_end - value),
				                 &request->content_length))
					return AIUS_HTTP_ERR_MALFORMED;
				request->has_content_length = true;
			}
		}
		if (line_end == end)
			break;
		cursor = line_end + 2;
	}
	return AIUS_HTTP_OK;
}

static FLASHMEM AiusHttpResult extract_sd_auth(const uint8_t *headers, size_t length,
	                                   AiusHttpRequest *request)
{
	/* Name folds (RFC 9110 5.1) so urllib's "X-aius-sd-auth" and any other
	 * conforming spelling reach the same token check; the token itself is
	 * still compared byte-for-byte by the caller. */
	const uint8_t *cursor = headers;
	const uint8_t *end = headers + length;

	while (cursor < end) {
		const uint8_t *line_end = find_bytes(cursor, (size_t)(end - cursor),
		                                     "\r\n", 2);
		const uint8_t *colon;
		if (line_end == NULL)
			line_end = end;
		size_t line_length = (size_t)(line_end - cursor);
		colon = (const uint8_t *)memchr(cursor, ':', line_length);
		if (colon != NULL &&
		    header_name_equals(cursor, (size_t)(colon - cursor), "X-Aius-SD-Auth")) {
			const uint8_t *value = colon + 1u;
			const uint8_t *value_end = line_end;
			while (value < value_end && (*value == ' ' || *value == '\t'))
				value++;
			while (value_end > value &&
			       (value_end[-1] == ' ' || value_end[-1] == '\t'))
				value_end--;
			if (value == value_end)
				return AIUS_HTTP_ERR_MALFORMED;
			if ((size_t)(value_end - value) > AIUS_HTTP_MAX_SD_AUTH)
				return AIUS_HTTP_ERR_TOO_LARGE;
			if (request->sd_auth != NULL)
				return AIUS_HTTP_ERR_MALFORMED;
			request->sd_auth = value;
			request->sd_auth_length = (size_t)(value_end - value);
		}
		if (line_end == end)
			break;
		cursor = line_end + 2;
	}
	return AIUS_HTTP_OK;
}

static FLASHMEM const uint8_t *trim_ws(const uint8_t *start, const uint8_t *end,
	                                const uint8_t **trimmed_end)
{
	while (start < end && (*start == ' ' || *start == '\t'))
		start++;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	*trimmed_end = end;
	return start;
}

/* RFC 9110 12.4.2 qvalue: 0, 0., 0.0 .. 0.000 are zero; anything else is not. */
static FLASHMEM int qvalue_is_zero(const uint8_t *value, const uint8_t *end)
{
	if (value >= end || *value != '0')
		return 0;
	value++;
	if (value < end && *value == '.')
		value++;
	else if (value < end)
		return 0;
	while (value < end)
		if (*value++ != '0')
			return 0;
	return 1;
}

/* Accept-Encoding verdicts for one coding name: not listed, refused (q=0) or
 * accepted (any other q, or none). */
enum {
	AIUS_HTTP_CODING_UNLISTED = 0,
	AIUS_HTTP_CODING_REFUSED,
	AIUS_HTTP_CODING_ACCEPTED
};

/* One Accept-Encoding value. Only gzip and the "*" wildcard matter; the last
 * entry of each wins. The caller combines them after every header line
 * (Phase 4h N2): gzip by name decides, else "*" with q > 0 selects gzip, as
 * RFC 9110 12.5.3 lets "*" match any coding not otherwise listed. */
static FLASHMEM void scan_accept_encoding(const uint8_t *cursor, const uint8_t *end,
	                              int *gzip, int *star)
{
	while (cursor < end) {
		const uint8_t *comma = (const uint8_t *)memchr(cursor, ',', (size_t)(end - cursor));
		const uint8_t *item_end = comma == NULL ? end : comma;
		const uint8_t *semi = (const uint8_t *)memchr(cursor, ';', (size_t)(item_end - cursor));
		const uint8_t *coding_end;
		const uint8_t *coding = trim_ws(cursor, semi == NULL ? item_end : semi, &coding_end);
		int *verdict = NULL;
		if (header_name_equals(coding, (size_t)(coding_end - coding), "gzip"))
			verdict = gzip;
		else if (coding_end - coding == 1 && coding[0] == '*')
			verdict = star;
		if (verdict != NULL) {
			int zero = 0;
			if (semi != NULL) {
				const uint8_t *param_end;
				const uint8_t *param = trim_ws(semi + 1, item_end, &param_end);
				if (param_end - param >= 2 && (param[0] == 'q' || param[0] == 'Q')) {
					const uint8_t *eq_end;
					const uint8_t *eq = trim_ws(param + 1, param_end, &eq_end);
					if (eq < eq_end && *eq == '=') {
						const uint8_t *q_end;
						const uint8_t *q = trim_ws(eq + 1, eq_end, &q_end);
						zero = qvalue_is_zero(q, q_end);
					}
				}
			}
			*verdict = zero ? AIUS_HTTP_CODING_REFUSED : AIUS_HTTP_CODING_ACCEPTED;
		}
		if (comma == NULL)
			break;
		cursor = comma + 1;
	}
}

static FLASHMEM void extract_cache_headers(const uint8_t *base,
	                                  const uint8_t *headers, size_t length,
	                                  AiusHttpRequest *request)
{
	const uint8_t *cursor = headers;
	const uint8_t *end = headers + length;
	int gzip = AIUS_HTTP_CODING_UNLISTED;
	int star = AIUS_HTTP_CODING_UNLISTED;
	while (cursor < end) {
		const uint8_t *line_end = find_bytes(cursor, (size_t)(end - cursor), "\r\n", 2);
		const uint8_t *colon;
		if (line_end == NULL) line_end = end;
		colon = (const uint8_t *)memchr(cursor, ':', (size_t)(line_end - cursor));
		if (colon != NULL) {
			size_t name_length = (size_t)(colon - cursor);
			const uint8_t *value_end;
			const uint8_t *value;
			if (header_name_equals(cursor, name_length, "Accept-Encoding")) {
				value = trim_ws(colon + 1, line_end, &value_end);
				scan_accept_encoding(value, value_end, &gzip, &star);
			} else if (header_name_equals(cursor, name_length, "If-None-Match") &&
			           request->if_none_match_offset == 0u) {
				value = trim_ws(colon + 1, line_end, &value_end);
				if (value < value_end)
					request->if_none_match_offset =
						(uint16_t)(value - base);
			}
		}
		if (line_end == end) break;
		cursor = line_end + 2;
	}
	request->accepts_gzip =
		gzip == AIUS_HTTP_CODING_ACCEPTED ||
		(gzip == AIUS_HTTP_CODING_UNLISTED && star == AIUS_HTTP_CODING_ACCEPTED);
}

FLASHMEM bool aius_http_if_none_match(const AiusHttpRequest *request, const char *etag)
{
	const uint8_t *cursor, *end;
	size_t etag_length;
	if (request == NULL || etag == NULL || request->if_none_match_offset == 0u ||
	    request->body == NULL)
		return false;
	etag_length = strlen(etag);
	cursor = request->body - request->header_length + request->if_none_match_offset;
	/* The value runs to the line end; the parsed request always holds the
	 * terminating CRLF CRLF after the headers, so the scan is bounded. */
	end = cursor;
	while (*end != '\r' && *end != '\n')
		end++;
	while (cursor < end) {
		const uint8_t *close;
		while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == ','))
			cursor++;
		if (cursor >= end)
			break;
		if (*cursor == '*')
			return true;
		if (end - cursor >= 2 && cursor[0] == 'W' && cursor[1] == '/')
			cursor += 2;
		if (cursor >= end || *cursor != '"')
			return false; /* malformed: no match */
		close = (const uint8_t *)memchr(cursor + 1, '"', (size_t)(end - (cursor + 1)));
		if (close == NULL)
			return false;
		if ((size_t)(close + 1 - cursor) == etag_length &&
		    memcmp(cursor, etag, etag_length) == 0)
			return true;
		cursor = close + 1;
	}
	return false;
}

static FLASHMEM int is_streamed_request(const AiusHttpRequest *request)
{
	return request->method == AIUS_HTTP_PUT &&
	       (request->route == AIUS_HTTP_ROUTE_SD_FILE ||
	        request->route == AIUS_HTTP_ROUTE_SD_REPLAY);
}

FLASHMEM AiusHttpResult aius_http_parse(const uint8_t *data, size_t length,
	                               AiusHttpRequest *request)
{
	if (data == NULL || request == NULL || length == 0)
		return AIUS_HTTP_ERR_EMPTY;
	if (length > AIUS_HTTP_MAX_REQUEST)
		return AIUS_HTTP_ERR_TOO_LARGE;
	memset(request, 0, sizeof(*request));

	const uint8_t *line_end = find_bytes(data, length, "\r\n", 2);
	if (line_end == NULL)
		return AIUS_HTTP_ERR_MALFORMED;
	size_t request_line_length = (size_t)(line_end - data);
	const uint8_t *space1 = (const uint8_t *)memchr(data, ' ', request_line_length);
	if (space1 == NULL)
		return AIUS_HTTP_ERR_MALFORMED;
	const uint8_t *space2 = (const uint8_t *)memchr(space1 + 1, ' ',
		request_line_length - (size_t)(space1 + 1 - data));
	if (space2 == NULL || (size_t)(line_end - (space2 + 1)) != 8u ||
	    (memcmp(space2 + 1, "HTTP/1.0", 8) != 0 &&
	     memcmp(space2 + 1, "HTTP/1.1", 8) != 0))
		return AIUS_HTTP_ERR_MALFORMED;
	if ((size_t)(space1 - data) == 3 && memcmp(data, "GET", 3) == 0)
		request->method = AIUS_HTTP_GET;
	else if ((size_t)(space1 - data) == 4 && memcmp(data, "POST", 4) == 0)
		request->method = AIUS_HTTP_POST;
	else if ((size_t)(space1 - data) == 6 && memcmp(data, "DELETE", 6) == 0)
		request->method = AIUS_HTTP_DELETE;
	else if ((size_t)(space1 - data) == 3 && memcmp(data, "PUT", 3) == 0)
		request->method = AIUS_HTTP_PUT;
	else
		return AIUS_HTTP_ERR_METHOD;

	const uint8_t *header_start = line_end + 2;
	const uint8_t *header_end = find_bytes(line_end,
		length - (size_t)(line_end - data), "\r\n\r\n", 4);
	if (header_end == NULL)
		return AIUS_HTTP_ERR_MALFORMED;
	size_t header_length = header_end < header_start ? 0u :
		(size_t)(header_end - header_start);
	if (header_length > AIUS_HTTP_MAX_HEADERS)
		return AIUS_HTTP_ERR_TOO_LARGE;
	request->header_length = (size_t)(header_end - data) + 4u;
	AiusHttpResult framing_result = extract_framing(header_start, header_length, request);
	if (framing_result != AIUS_HTTP_OK)
		return framing_result;
	AiusHttpResult auth_result = extract_sd_auth(header_start, header_length, request);
	if (auth_result != AIUS_HTTP_OK)
		return auth_result;
	AiusHttpResult range_result = extract_content_range(header_start, header_length, request);
	if (range_result != AIUS_HTTP_OK)
		return range_result;
	extract_cache_headers(data, header_start, header_length, request);
	const uint8_t *body = header_end + 4;
	size_t body_length = length - (size_t)(body - data);
	request->body = body;
	request->body_length = body_length;
	request->has_body = request->content_length != 0u;
	if (!request->has_content_length && body_length != 0u)
		return AIUS_HTTP_ERR_MALFORMED;
	if ((uint64_t)body_length > request->content_length)
		return AIUS_HTTP_ERR_MALFORMED;
	request->body_complete = (uint64_t)body_length == request->content_length;

	request->route = route_for(space1 + 1, (size_t)(space2 - (space1 + 1)), request);
	if (request->route == AIUS_HTTP_ROUTE_UNKNOWN)
		return AIUS_HTTP_ERR_ROUTE;
	if (request->method == AIUS_HTTP_GET &&
	    request->route >= AIUS_HTTP_ROUTE_START &&
	    request->route <= AIUS_HTTP_ROUTE_REBOOT)
		return AIUS_HTTP_ERR_METHOD;
	if (request->method == AIUS_HTTP_POST &&
	    (request->route == AIUS_HTTP_ROUTE_STATUS || request->route == AIUS_HTTP_ROUTE_EVENTS))
		return AIUS_HTTP_ERR_METHOD;
	if ((request->method == AIUS_HTTP_PUT || request->method == AIUS_HTTP_DELETE) &&
	    request->route != AIUS_HTTP_ROUTE_SD_FILE &&
	    request->route != AIUS_HTTP_ROUTE_SD_REPLAY &&
	    request->route != AIUS_HTTP_ROUTE_CONFIG_PUBLIC &&
	    request->route != AIUS_HTTP_ROUTE_REPLAYS &&
	    request->route != AIUS_HTTP_ROUTE_DSP_CONFIG &&
	    request->route != AIUS_HTTP_ROUTE_RECORDER_CONFIG &&
	    request->route != AIUS_HTTP_ROUTE_OTA_IMAGE)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_CONFIG_PUBLIC &&
	    request->method != AIUS_HTTP_PUT)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_DSP_CONFIG &&
	    request->method != AIUS_HTTP_GET && request->method != AIUS_HTTP_PUT)
		return AIUS_HTTP_ERR_METHOD;
	/* Read-only meter snapshot; the serializer lands with Phase 14b. */
	if (request->route == AIUS_HTTP_ROUTE_METERS && request->method != AIUS_HTTP_GET)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS_STATUS && request->method != AIUS_HTTP_GET)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS_START && request->method != AIUS_HTTP_POST)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS_STOP && request->method != AIUS_HTTP_POST)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_MEASURE && request->method != AIUS_HTTP_GET)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_MEASURE_START && request->method != AIUS_HTTP_POST)
		return AIUS_HTTP_ERR_METHOD;
	if ((request->route == AIUS_HTTP_ROUTE_RECORDER ||
	     request->route == AIUS_HTTP_ROUTE_RECORDER_DATA) &&
	    request->method != AIUS_HTTP_GET)
		return AIUS_HTTP_ERR_METHOD;
	if ((request->route == AIUS_HTTP_ROUTE_RECORDER_FREEZE ||
	     request->route == AIUS_HTTP_ROUTE_RECORDER_THAW) &&
	    request->method != AIUS_HTTP_POST)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_RECORDER_CONFIG &&
	    request->method != AIUS_HTTP_PUT)
		return AIUS_HTTP_ERR_METHOD;
	/* OTA method rules. GET /api/ota (open read), PUT /api/ota/image (the
	 * resumable buffered chunk -- admitted into the PUT allow-list above), and
	 * POST for the three verbs. The dispatcher owns the absent-then-token gate,
	 * so the parser only shapes method here. */
	if (request->route == AIUS_HTTP_ROUTE_OTA &&
	    request->method != AIUS_HTTP_GET)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_OTA_IMAGE &&
	    request->method != AIUS_HTTP_PUT)
		return AIUS_HTTP_ERR_METHOD;
	if ((request->route == AIUS_HTTP_ROUTE_OTA_ARM ||
	     request->route == AIUS_HTTP_ROUTE_OTA_VERIFY ||
	     request->route == AIUS_HTTP_ROUTE_OTA_ABORT ||
	     request->route == AIUS_HTTP_ROUTE_OTA_ACTIVATE ||
	     request->route == AIUS_HTTP_ROUTE_OTA_ENABLE_ACTIVATION ||
	     request->route == AIUS_HTTP_ROUTE_OTA_SELF_TEST_FLASH ||
	     request->route == AIUS_HTTP_ROUTE_OTA_REHEARSE ||
	     request->route == AIUS_HTTP_ROUTE_OTA_REHEARSE_FAULT) &&
	    request->method != AIUS_HTTP_POST)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS &&
	    request->method != AIUS_HTTP_GET && request->method != AIUS_HTTP_DELETE)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_SD_STATUS && request->method != AIUS_HTTP_GET)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_SD_MODE && request->method != AIUS_HTTP_POST)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_STATUS_BRIEF && request->method != AIUS_HTTP_GET)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_SD_FILE &&
	    request->method != AIUS_HTTP_GET && request->method != AIUS_HTTP_PUT &&
	    request->method != AIUS_HTTP_DELETE)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_SD_FILE &&
	    (request->resource_length == 0 || request->resource_length > 96))
		return AIUS_HTTP_ERR_ROUTE;
	if (request->route == AIUS_HTTP_ROUTE_SD_REPLAY) {
		if (request->replay_id_length == 0 || request->replay_id_length > AIUS_HTTP_MAX_REPLAY_ID)
			return AIUS_HTTP_ERR_ROUTE;
		for (size_t i = 0; i < request->replay_id_length; i++) {
			uint8_t ch = request->replay_id[i];
			if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
				return AIUS_HTTP_ERR_ROUTE;
		}
		if (request->method == AIUS_HTTP_PUT && request->body_complete &&
		    !request->has_content_range)
			return AIUS_HTTP_ERR_BODY;
		if (request->method != AIUS_HTTP_GET && request->method != AIUS_HTTP_PUT)
			return AIUS_HTTP_ERR_METHOD;
	}
	if (is_streamed_request(request)) {
		if (!request->has_content_length)
			return AIUS_HTTP_ERR_BODY;
	} else {
		if (request->content_length > AIUS_HTTP_MAX_BODY)
			return AIUS_HTTP_ERR_TOO_LARGE;
		if (!request->body_complete)
			return AIUS_HTTP_ERR_BODY;
	}
	return AIUS_HTTP_OK;
}

FLASHMEM const char *aius_http_reason(unsigned status)
{
	switch (status) {
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 304: return "Not Modified";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 409: return "Conflict";
	case 413: return "Payload Too Large";
	case 416: return "Range Not Satisfiable";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	case 507: return "Insufficient Storage";
	default: return NULL;
	}
}

static FLASHMEM int valid_content_type(const char *content_type)
{
	size_t length = 0;
	if (content_type == NULL)
		return 0;
	while (length < AIUS_HTTP_CONTENT_TYPE_STORAGE && content_type[length] != '\0') {
		unsigned char value = (unsigned char)content_type[length];
		if (value < 0x20u || value > 0x7eu)
			return 0;
		length++;
	}
	return length != 0u && length < AIUS_HTTP_CONTENT_TYPE_STORAGE;
}

static FLASHMEM int append_fmt(uint8_t *output, size_t capacity, size_t *pos,
	                      const char *format, ...)
{
	va_list args;
	int n;
	if (*pos > capacity)
		return 0;
	va_start(args, format);
	n = vsnprintf((char *)output + *pos, capacity - *pos, format, args);
	va_end(args);
	if (n < 0 || (size_t)n > capacity - *pos)
		return 0;
	/* An exact fit (n == remaining) is legal -- the header block needs no NUL --
	 * but vsnprintf spent the last byte on one. Every line ends in '\n'. */
	if ((size_t)n == capacity - *pos)
		output[*pos + (size_t)n - 1u] = '\n';
	*pos += (size_t)n;
	return 1;
}

static FLASHMEM int valid_etag(const char *etag)
{
	size_t length = 0;
	while (length <= AIUS_HTTP_ETAG_MAX && etag[length] != '\0') {
		unsigned char value = (unsigned char)etag[length];
		if (value < 0x20u || value > 0x7eu)
			return 0;
		length++;
	}
	return length >= 2u && length <= AIUS_HTTP_ETAG_MAX;
}

FLASHMEM size_t aius_http_write_headers_ex(uint8_t *output, size_t capacity,
	                           unsigned status, const char *content_type,
	                           uint64_t content_length,
	                           uint32_t cache_max_age_s,
	                           const AiusHttpCacheOptions *options)
{
	/* Zero cache_max_age_s means BYTE-IDENTICAL headers to the pre-cache
	 * firmware, not "max-age=0": absence already means uncacheable here. */
	const char *reason = aius_http_reason(status);
	const int not_modified = status == 304u;
	size_t pos = 0;
	if (output == NULL || reason == NULL)
		return 0;
	if (options != NULL && options->etag != NULL && !valid_etag(options->etag))
		return 0;
	if (not_modified) {
		if (!append_fmt(output, capacity, &pos, "HTTP/1.0 %u %s\r\n", status, reason))
			return 0;
	} else {
		if (!valid_content_type(content_type) ||
		    !append_fmt(output, capacity, &pos,
		        "HTTP/1.0 %u %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n",
		        status, reason, content_type, (unsigned long long)content_length))
			return 0;
	}
	if (options != NULL && options->cache_no_cache) {
		if (!append_fmt(output, capacity, &pos, "Cache-Control: no-cache\r\n"))
			return 0;
	} else if (cache_max_age_s != 0u) {
		if (!append_fmt(output, capacity, &pos, "Cache-Control: max-age=%lu\r\n",
		                (unsigned long)cache_max_age_s))
			return 0;
	}
	if (options != NULL) {
		if (options->content_encoding_gzip && !not_modified &&
		    !append_fmt(output, capacity, &pos, "Content-Encoding: gzip\r\n"))
			return 0;
		if (options->etag != NULL &&
		    !append_fmt(output, capacity, &pos, "ETag: %s\r\n", options->etag))
			return 0;
		if (options->vary_accept_encoding &&
		    !append_fmt(output, capacity, &pos, "Vary: Accept-Encoding\r\n"))
			return 0;
	}
	/* Only a 503 gains Retry-After; every other status is byte-identical. */
	if (status == 503u && !append_fmt(output, capacity, &pos, "Retry-After: 1\r\n"))
		return 0;
	if (!append_fmt(output, capacity, &pos, "Connection: close\r\n\r\n"))
		return 0;
	return pos;
}

FLASHMEM size_t aius_http_write_headers(uint8_t *output, size_t capacity,
	                           unsigned status, const char *content_type,
	                           uint64_t content_length,
	                           uint32_t cache_max_age_s)
{
	/* A 304 needs the _ex form's no-body framing; the legacy entry keeps its
	 * contract of always framing a body. */
	if (status == 304u)
		return 0;
	return aius_http_write_headers_ex(output, capacity, status, content_type,
	                                  content_length, cache_max_age_s, NULL);
}

FLASHMEM size_t aius_http_write_response(uint8_t *output, size_t capacity,
	                              unsigned status, const char *content_type,
	                              const uint8_t *body, size_t body_length,
	                              uint32_t cache_max_age_s)
{
	uint8_t header[256];
	size_t header_length;
	if (output == NULL || (body_length != 0u && body == NULL) ||
	    body_length > AIUS_HTTP_MAX_RESPONSE)
		return 0;
	header_length = aius_http_write_headers(header, sizeof(header), status,
	                                        content_type, (uint64_t)body_length,
	                                        cache_max_age_s);
	if (header_length == 0u || body_length > capacity ||
	    header_length > capacity - body_length)
		return 0;
	memcpy(output, header, header_length);
	if (body_length != 0u)
		memcpy(output + header_length, body, body_length);
	return header_length + body_length;
}
