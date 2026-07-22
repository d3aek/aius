#include "aius_http.h"

#include <stdio.h>
#include <string.h>

static const uint8_t *find_bytes(const uint8_t *data, size_t length,
	                                const char *needle, size_t needle_length)
{
	if (needle_length == 0 || length < needle_length)
		return NULL;
	for (size_t i = 0; i + needle_length <= length; i++)
		if (memcmp(data + i, needle, needle_length) == 0)
			return data + i;
	return NULL;
}

static int has_prefix(const uint8_t *value, size_t length,
	                     const char *prefix)
{
	size_t prefix_length = strlen(prefix);
	return length >= prefix_length && memcmp(value, prefix, prefix_length) == 0;
}

static int decimal_u32(const uint8_t *value, size_t length, uint32_t *result)
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

static int token_equals(const uint8_t *value, size_t length, const char *token)
{
	size_t token_length = strlen(token);
	return length == token_length && memcmp(value, token, token_length) == 0;
}

static AiusHttpRoute route_for(const uint8_t *path, size_t length,
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
	if (token_equals(path, length, "/api/mode"))
		return AIUS_HTTP_ROUTE_MODE;
	if (token_equals(path, length, "/api/mix"))
		return AIUS_HTTP_ROUTE_MIX;
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
	if (token_equals(path, length, "/api/sd"))
		return AIUS_HTTP_ROUTE_SD_STATUS;
	if (has_prefix(path, length, "/sd/") && length > strlen("/sd/")) {
		request->resource = path + strlen("/sd/");
		request->resource_length = length - strlen("/sd/");
		return AIUS_HTTP_ROUTE_SD_FILE;
	}
	return AIUS_HTTP_ROUTE_UNKNOWN;
}

static int json_value(const uint8_t *body, size_t length, const char *key,
	                    const char *value)
{
	char expected[64];
	int expected_length = snprintf(expected, sizeof(expected), "{\"%s\":\"%s\"}",
	                               key, value);
	if (expected_length < 0 || (size_t)expected_length >= sizeof(expected))
		return 0;
	return length == (size_t)expected_length &&
	       memcmp(body, expected, length) == 0;
}

static AiusHttpResult extract_sd_auth(const uint8_t *headers, size_t length,
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

AiusHttpResult aius_http_parse(const uint8_t *data, size_t length,
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
	if (space2 == NULL ||
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
	AiusHttpResult auth_result = extract_sd_auth(header_start, header_length, request);
	if (auth_result != AIUS_HTTP_OK)
		return auth_result;
	const uint8_t *body = header_end + 4;
	size_t body_length = length - (size_t)(body - data);
	if (body_length > AIUS_HTTP_MAX_BODY)
		return AIUS_HTTP_ERR_TOO_LARGE;
	request->has_body = body_length != 0;

	request->route = route_for(space1 + 1, (size_t)(space2 - (space1 + 1)), request);
	if (request->route == AIUS_HTTP_ROUTE_UNKNOWN)
		return AIUS_HTTP_ERR_ROUTE;
	if (request->method == AIUS_HTTP_GET &&
	    (request->route == AIUS_HTTP_ROUTE_MODE || request->route == AIUS_HTTP_ROUTE_MIX ||
	     (request->route >= AIUS_HTTP_ROUTE_START &&
	      request->route <= AIUS_HTTP_ROUTE_REBOOT)))
		return AIUS_HTTP_ERR_METHOD;
	if (request->method == AIUS_HTTP_POST &&
	    (request->route == AIUS_HTTP_ROUTE_STATUS || request->route == AIUS_HTTP_ROUTE_EVENTS))
		return AIUS_HTTP_ERR_METHOD;
	if ((request->method == AIUS_HTTP_PUT || request->method == AIUS_HTTP_DELETE) &&
	    request->route != AIUS_HTTP_ROUTE_SD_FILE &&
	    request->route != AIUS_HTTP_ROUTE_CONFIG_PUBLIC)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_CONFIG_PUBLIC &&
	    request->method != AIUS_HTTP_PUT)
		return AIUS_HTTP_ERR_METHOD;
	if (request->route == AIUS_HTTP_ROUTE_DSP_CONFIG &&
	    request->method != AIUS_HTTP_GET)
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
	if ((request->route == AIUS_HTTP_ROUTE_MODE || request->route == AIUS_HTTP_ROUTE_MIX) &&
	    !request->has_body)
		return AIUS_HTTP_ERR_BODY;
	if (request->route == AIUS_HTTP_ROUTE_MODE) {
		if (json_value(body, body_length, "mode", "flat")) request->mode = 0;
		else if (json_value(body, body_length, "mode", "speech")) request->mode = 1;
		else if (json_value(body, body_length, "mode", "bass-reduced")) request->mode = 2;
		else return AIUS_HTTP_ERR_BODY;
	}
	if (request->route == AIUS_HTTP_ROUTE_MIX) {
		if (json_value(body, body_length, "mix", "standard")) request->mix = 0;
		else if (json_value(body, body_length, "mix", "lfe-minus10")) request->mix = 1;
		else if (json_value(body, body_length, "mix", "clipfree")) request->mix = 2;
		else return AIUS_HTTP_ERR_BODY;
	}
	return AIUS_HTTP_OK;
}

const char *aius_http_reason(unsigned status)
{
	switch (status) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 413: return "Payload Too Large";
	case 503: return "Service Unavailable";
	case 507: return "Insufficient Storage";
	default: return "Internal Server Error";
	}
}

size_t aius_http_write_response(uint8_t *output, size_t capacity,
	                              unsigned status, const char *content_type,
	                              const uint8_t *body, size_t body_length)
{
	if (output == NULL || content_type == NULL || body_length > AIUS_HTTP_MAX_RESPONSE)
		return 0;
	char header[192];
	int header_length = snprintf(header, sizeof(header),
		"HTTP/1.0 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
		status, aius_http_reason(status), content_type, (unsigned)body_length);
	if (header_length < 0 || (size_t)header_length + body_length > capacity)
		return 0;
	memcpy(output, header, (size_t)header_length);
	if (body_length != 0 && body != NULL)
		memcpy(output + header_length, body, body_length);
	return (size_t)header_length + body_length;
}
