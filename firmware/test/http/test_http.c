#include "aius_http.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))

static void parse_ok(const char *raw, AiusHttpRoute route)
{
	AiusHttpRequest request;
	AiusHttpResult result =
		aius_http_parse((const uint8_t *)raw, strlen(raw), &request);
	if (result != AIUS_HTTP_OK)
		fprintf(stderr, "parse_ok request %zu bytes \"%s\": expected result %d, actual %d\n",
		        strlen(raw), raw, (int)AIUS_HTTP_OK, (int)result);
	assert(result == AIUS_HTTP_OK);
	if (request.route != route)
		fprintf(stderr, "parse_ok request \"%s\": expected route %d, actual %d\n",
		        raw, (int)route, (int)request.route);
	assert(request.route == route);
}

static void test_dsp_config_route(void)
{
	AiusHttpRequest request;
	const char *get = "GET /api/dsp/config HTTP/1.0\r\n\r\n";
	const char *post = "POST /api/dsp/config HTTP/1.0\r\n\r\n";
	const char *put = "PUT /api/dsp/config HTTP/1.0\r\n\r\n";
	const char *delete = "DELETE /api/dsp/config HTTP/1.0\r\n\r\n";
	const char *near_miss = "GET /api/dsp/configuration HTTP/1.0\r\n\r\n";

	assert(aius_http_parse((const uint8_t *)get, strlen(get), &request) == AIUS_HTTP_OK);
	assert(request.route == AIUS_HTTP_ROUTE_DSP_CONFIG);
	assert(aius_http_parse((const uint8_t *)post, strlen(post), &request) ==
	       AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)put, strlen(put), &request) ==
	       AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)delete, strlen(delete), &request) ==
	       AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)near_miss, strlen(near_miss), &request) ==
	       AIUS_HTTP_ERR_ROUTE);
}

static void test_sd_auth_header(void)
{
	AiusHttpRequest request;
	const char *absent = "GET /api/status HTTP/1.0\r\n\r\n";
	const char *valid = "GET /api/status HTTP/1.0\r\n"
	                   "X-Aius-SD-Auth: 0123456789abcdef\r\n\r\n";
	const char *malformed = "GET /api/status HTTP/1.0\r\n"
	                        "X-Aius-SD-Auth:\r\n\r\n";
	const char *oversized = "GET /api/status HTTP/1.0\r\n"
	                       "X-Aius-SD-Auth: 0123456789abcdef0\r\n\r\n";
	const char *duplicate = "GET /api/status HTTP/1.0\r\n"
	                       "X-Aius-SD-Auth: 0123456789abcdef\r\n"
	                       "X-Aius-SD-Auth: fedcba9876543210\r\n\r\n";
	const char *wrong_case = "GET /api/status HTTP/1.0\r\n"
	                        "x-aius-sd-auth: 0123456789abcdef\r\n\r\n";

	assert(aius_http_parse((const uint8_t *)absent, strlen(absent), &request) == AIUS_HTTP_OK);
	assert(request.sd_auth == NULL);
	assert(request.sd_auth_length == 0);
	assert(aius_http_parse((const uint8_t *)valid, strlen(valid), &request) == AIUS_HTTP_OK);
	assert(request.sd_auth_length == AIUS_HTTP_MAX_SD_AUTH);
	assert(memcmp(request.sd_auth, "0123456789abcdef", AIUS_HTTP_MAX_SD_AUTH) == 0);
	assert(aius_http_parse((const uint8_t *)malformed, strlen(malformed), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	assert(aius_http_parse((const uint8_t *)oversized, strlen(oversized), &request) ==
	       AIUS_HTTP_ERR_TOO_LARGE);
	assert(aius_http_parse((const uint8_t *)duplicate, strlen(duplicate), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	assert(aius_http_parse((const uint8_t *)wrong_case, strlen(wrong_case), &request) ==
	       AIUS_HTTP_OK);
	assert(request.sd_auth == NULL);
	assert(request.sd_auth_length == 0);
}

static void test_replay_routes(void)
{
	AiusHttpRequest request;
	const char *upload = "PUT /sd/replays/trailer-51 HTTP/1.0\r\n"
		"X-Aius-SD-Auth: 0123456789abcdef\r\n"
		"Content-Range: bytes 0-3/8\r\nContent-Length: 4\r\n\r\nABCD";
	assert(aius_http_parse((const uint8_t *)upload, strlen(upload), &request) == AIUS_HTTP_OK);
	assert(request.route == AIUS_HTTP_ROUTE_SD_REPLAY);
	assert(request.replay_id_length == 10);
	assert(request.has_content_range);
	assert(request.content_range_start == 0);
	assert(request.content_range_end == 3);
	assert(request.content_range_total == 8);
	parse_ok("GET /api/replays HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_REPLAYS);
	parse_ok("GET /api/replays/status HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_REPLAYS_STATUS);
	parse_ok("POST /api/replays/start HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}",
	         AIUS_HTTP_ROUTE_REPLAYS_START);
	parse_ok("POST /api/replays/stop HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_REPLAYS_STOP);
	const char *odd = "PUT /sd/replays/x HTTP/1.0\r\nContent-Range: bytes 0-2/4\r\n\r\nABC";
	assert(aius_http_parse((const uint8_t *)odd, strlen(odd), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *bad_id = "GET /sd/replays/a.b HTTP/1.0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)bad_id, strlen(bad_id), &request) == AIUS_HTTP_ERR_ROUTE);
}

static void test_framing(void)
{
	AiusHttpRequest request;
	const char *empty = "GET /api/status HTTP/1.0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)empty, strlen(empty), &request) == AIUS_HTTP_OK);
	assert(request.header_length == strlen(empty));
	assert(!request.has_content_length);
	assert(request.content_length == 0);
	assert(request.body == (const uint8_t *)empty + strlen(empty));
	assert(request.body_length == 0);
	assert(request.body_complete);
	assert(request.has_body == 0);

	const char *complete =
		"PUT /api/config/public HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}";
	assert(aius_http_parse((const uint8_t *)complete, strlen(complete), &request) ==
	       AIUS_HTTP_OK);
	assert(request.has_content_length);
	assert(request.content_length == 2);
	assert(request.body_length == 2);
	assert(request.body_complete);
	assert(request.has_body == 1);
	assert(memcmp(request.body, "{}", 2) == 0);

	const char *streamed =
		"PUT /sd/index.html HTTP/1.0\r\nContent-Length: 5\r\n\r\nab";
	assert(aius_http_parse((const uint8_t *)streamed, strlen(streamed), &request) ==
	       AIUS_HTTP_OK);
	assert(request.route == AIUS_HTTP_ROUTE_SD_FILE);
	assert(request.content_length == 5);
	assert(request.body_length == 2);
	assert(!request.body_complete);
	assert(request.has_body == 1);

	const char *streamed_empty =
		"PUT /sd/replays/r1 HTTP/1.0\r\nContent-Length: 5\r\n\r\n";
	AiusHttpResult streamed_empty_result =
		aius_http_parse((const uint8_t *)streamed_empty, strlen(streamed_empty),
		                &request);
	if (streamed_empty_result != AIUS_HTTP_OK)
		fprintf(stderr, "streamed empty-prefix result: %d\n",
		        (int)streamed_empty_result);
	assert(streamed_empty_result == AIUS_HTTP_OK);
	assert(request.body_length == 0);
	assert(!request.body_complete);

	const char *ordinary_incomplete =
		"PUT /api/config/public HTTP/1.0\r\nContent-Length: 5\r\n\r\nab";
	assert(aius_http_parse((const uint8_t *)ordinary_incomplete,
	                       strlen(ordinary_incomplete), &request) ==
	       AIUS_HTTP_ERR_BODY);
	const char *undeclared = "GET /api/status HTTP/1.0\r\n\r\nx";
	assert(aius_http_parse((const uint8_t *)undeclared, strlen(undeclared), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *excess =
		"PUT /api/config/public HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}x";
	assert(aius_http_parse((const uint8_t *)excess, strlen(excess), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *duplicate =
		"POST /api/mode HTTP/1.0\r\nContent-Length: 0\r\n"
		"Content-Length: 0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)duplicate, strlen(duplicate), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *transfer =
		"POST /api/mode HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)transfer, strlen(transfer), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);

	static const char *const invalid_lengths[] = {
		"", "+1", "-1", " 1", "1 ", "1x", "18446744073709551616"
	};
	for (size_t i = 0; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); ++i) {
		char raw[160];
		int length = snprintf(raw, sizeof(raw),
		                      "PUT /sd/x HTTP/1.0\r\nContent-Length: %s\r\n\r\n",
		                      invalid_lengths[i]);
		assert(length > 0);
		assert((size_t)length < sizeof(raw));
		AiusHttpResult result =
			aius_http_parse((const uint8_t *)raw, (size_t)length, &request);
		if (result != AIUS_HTTP_ERR_MALFORMED)
			fprintf(stderr,
			        "invalid Content-Length \"%s\": expected %d, actual %d\n",
			        invalid_lengths[i], (int)AIUS_HTTP_ERR_MALFORMED,
			        (int)result);
		assert(result == AIUS_HTTP_ERR_MALFORMED);
	}

	const char *maximum =
		"PUT /sd/x HTTP/1.0\r\nContent-Length: 18446744073709551615\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)maximum, strlen(maximum), &request) ==
	       AIUS_HTTP_OK);
	assert(request.content_length == UINT64_MAX);
	assert(!request.body_complete);
}

static void test_response_headers(void)
{
	uint8_t headers[192];
	size_t length = aius_http_write_headers(headers, sizeof(headers), 416,
	                                        "application/octet-stream",
	                                        UINT64_MAX);
	assert(length != 0);
	headers[length] = 0;
	assert(strcmp((const char *)headers,
	              "HTTP/1.0 416 Range Not Satisfiable\r\n"
	              "Content-Type: application/octet-stream\r\n"
	              "Content-Length: 18446744073709551615\r\n"
	              "Connection: close\r\n\r\n") == 0);
	assert(aius_http_write_headers(NULL, sizeof(headers), 200, "text/plain", 0) == 0);
	assert(aius_http_write_headers(headers, sizeof(headers), 418, "text/plain", 0) == 0);
	assert(aius_http_write_headers(headers, sizeof(headers), 200, "", 0) == 0);
	assert(aius_http_write_headers(headers, 1, 200, "text/plain", 0) == 0);
}

static void test_parser_result_registry(void)
{
	struct ParserResultRow {
		const char *key;
		const char *raw;
		size_t length;
		AiusHttpResult expected;
	};
	static const char ok[] = "GET /api/status HTTP/1.0\r\n\r\n";
	static const char malformed[] = "GET /api/status HTTP/1.0\r\n";
	static const char too_large[] =
		"GET /api/status HTTP/1.0\r\n"
		"X-Aius-SD-Auth: 0123456789abcdef0\r\n\r\n";
	static const char method[] = "PUT /api/status HTTP/1.0\r\n\r\n";
	static const char route[] = "GET /not-a-route HTTP/1.0\r\n\r\n";
	static const char body[] =
		"POST /api/mode HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}";
	static const struct ParserResultRow rows[] = {
		{ "parse.ok", ok, sizeof(ok) - 1u, AIUS_HTTP_OK },
		{ "parse.empty", "", 0, AIUS_HTTP_ERR_EMPTY },
		{ "parse.malformed", malformed, sizeof(malformed) - 1u,
		  AIUS_HTTP_ERR_MALFORMED },
		{ "parse.too-large", too_large, sizeof(too_large) - 1u,
		  AIUS_HTTP_ERR_TOO_LARGE },
		{ "parse.method", method, sizeof(method) - 1u,
		  AIUS_HTTP_ERR_METHOD },
		{ "parse.route", route, sizeof(route) - 1u, AIUS_HTTP_ERR_ROUTE },
		{ "parse.body", body, sizeof(body) - 1u, AIUS_HTTP_ERR_BODY },
	};
	assert(ARRAY_LENGTH(rows) == 7);
	for (size_t i = 0; i < ARRAY_LENGTH(rows); ++i) {
		for (size_t j = 0; j < i; ++j)
			assert(strcmp(rows[i].key, rows[j].key) != 0);
		AiusHttpRequest request;
		AiusHttpResult actual =
			aius_http_parse((const uint8_t *)rows[i].raw,
			                rows[i].length, &request);
		if (actual != rows[i].expected)
			fprintf(stderr,
			        "%s: expected result %d, actual %d\n",
			        rows[i].key, (int)rows[i].expected, (int)actual);
		assert(actual == rows[i].expected);
	}
}

int main(void)
{
	AiusHttpRequest request;
	test_sd_auth_header();
	test_dsp_config_route();
	test_replay_routes();
	test_framing();
	test_response_headers();
	test_parser_result_registry();
	parse_ok("GET / HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_HOME);
	parse_ok("GET /api/sd HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_SD_STATUS);
	parse_ok("GET /sd/index.html HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_SD_FILE);
	parse_ok("PUT /api/config/public HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}",
	         AIUS_HTTP_ROUTE_CONFIG_PUBLIC);
	parse_ok("PUT /sd/index.html HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}",
	         AIUS_HTTP_ROUTE_SD_FILE);
	parse_ok("GET /api/status HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_STATUS);
	parse_ok("GET /api/events?cursor=42 HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_EVENTS);
	parse_ok("GET /api/events HTTP/1.1\r\nHost: device\r\n\r\n", AIUS_HTTP_ROUTE_EVENTS);
	parse_ok("POST /api/mode HTTP/1.0\r\nContent-Length: 15\r\n\r\n{\"mode\":\"flat\"}",
	         AIUS_HTTP_ROUTE_MODE);
	const char *mix = "POST /api/mix HTTP/1.0\r\n\r\n{\"mix\":\"clipfree\"}";
	assert(aius_http_parse((const uint8_t *)mix, strlen(mix), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *get_start = "GET /api/start HTTP/1.0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)get_start, strlen(get_start), &request) ==
	       AIUS_HTTP_ERR_METHOD);
	const char *put = "PUT /api/status HTTP/1.0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)put, strlen(put), &request) == AIUS_HTTP_ERR_METHOD);
	const char *nope = "GET /nope HTTP/1.0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)nope, strlen(nope), &request) == AIUS_HTTP_ERR_ROUTE);
	parse_ok("GET /api/status HTTP/1.1\r\nHost: device\r\nConnection: close\r\n\r\n",
	         AIUS_HTTP_ROUTE_STATUS);
	const char *empty_body = "POST /api/mode HTTP/1.0\r\n\r\n{}";
	assert(aius_http_parse((const uint8_t *)empty_body, strlen(empty_body), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *truncated = "GET /api/status HTTP/1.0\r\n";
	assert(aius_http_parse((const uint8_t *)truncated, strlen(truncated), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);

	uint8_t response[128];
	static const uint8_t body[] = "{}";
	size_t response_length = aius_http_write_response(response, sizeof(response), 200,
	                                                 "application/json", body, 2);
	assert(response_length != 0);
	response[response_length] = 0;
	assert(strstr((const char *)response, "Connection: close") != NULL);
	assert(aius_http_write_response(response, 10, 200, "application/json", body, 2) == 0);
	puts("ok http parser");
	return 0;
}
