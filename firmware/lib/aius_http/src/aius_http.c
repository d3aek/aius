#include "aius_http.h"

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
	if (token_equals(path, length, "/api/replays"))
		return AIUS_HTTP_ROUTE_REPLAYS;
	if (token_equals(path, length, "/api/replays/status"))
		return AIUS_HTTP_ROUTE_REPLAYS_STATUS;
	if (token_equals(path, length, "/api/replays/start"))
		return AIUS_HTTP_ROUTE_REPLAYS_START;
	if (token_equals(path, length, "/api/replays/stop"))
		return AIUS_HTTP_ROUTE_REPLAYS_STOP;
	if (token_equals(path, length, "/api/sd"))
		return AIUS_HTTP_ROUTE_SD_STATUS;
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
	static const char prefix[] = "Content-Range: bytes ";
	const uint8_t *cursor = headers;
	const uint8_t *end = headers + length;
	while (cursor < end) {
		const uint8_t *line_end = find_bytes(cursor, (size_t)(end - cursor), "\r\n", 2);
		if (line_end == NULL) line_end = end;
		size_t line_length = (size_t)(line_end - cursor);
		if (line_length >= sizeof(prefix) - 1u &&
		    memcmp(cursor, prefix, sizeof(prefix) - 1u) == 0) {
			const uint8_t *value = cursor + sizeof(prefix) - 1u;
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
	static const char header_name[] = "X-Aius-SD-Auth";
	const size_t header_name_length = sizeof(header_name) - 1u;
	const uint8_t *cursor = headers;
	const uint8_t *end = headers + length;

	while (cursor < end) {
		const uint8_t *line_end = find_bytes(cursor, (size_t)(end - cursor),
		                                     "\r\n", 2);
		if (line_end == NULL)
			line_end = end;
		size_t line_length = (size_t)(line_end - cursor);
		if (line_length > header_name_length &&
		    memcmp(cursor, header_name, header_name_length) == 0 &&
		    cursor[header_name_length] == ':') {
			const uint8_t *value = cursor + header_name_length + 1u;
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
	    request->route != AIUS_HTTP_ROUTE_DSP_CONFIG)
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
	if (request->route == AIUS_HTTP_ROUTE_REPLAYS &&
	    request->method != AIUS_HTTP_GET && request->method != AIUS_HTTP_DELETE)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_SD_STATUS && request->method != AIUS_HTTP_GET)
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

FLASHMEM size_t aius_http_write_headers(uint8_t *output, size_t capacity,
	                           unsigned status, const char *content_type,
	                           uint64_t content_length)
{
	char header[192];
	const char *reason = aius_http_reason(status);
	int header_length;
	if (output == NULL || reason == NULL || !valid_content_type(content_type))
		return 0;
	header_length = snprintf(header, sizeof(header),
		"HTTP/1.0 %u %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
		status, reason, content_type, (unsigned long long)content_length);
	if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
	    (size_t)header_length > capacity)
		return 0;
	memcpy(output, header, (size_t)header_length);
	return (size_t)header_length;
}

FLASHMEM size_t aius_http_write_response(uint8_t *output, size_t capacity,
	                              unsigned status, const char *content_type,
	                              const uint8_t *body, size_t body_length)
{
	uint8_t header[192];
	size_t header_length;
	if (output == NULL || (body_length != 0u && body == NULL) ||
	    body_length > AIUS_HTTP_MAX_RESPONSE)
		return 0;
	header_length = aius_http_write_headers(header, sizeof(header), status,
	                                        content_type, (uint64_t)body_length);
	if (header_length == 0u || body_length > capacity ||
	    header_length > capacity - body_length)
		return 0;
	memcpy(output, header, header_length);
	if (body_length != 0u)
		memcpy(output + header_length, body, body_length);
	return header_length + body_length;
}
