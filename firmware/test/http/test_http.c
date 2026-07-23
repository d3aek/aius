#include "aius_http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void parse_ok(const char *raw, AiusHttpRoute route)
{
	AiusHttpRequest request;
	assert(aius_http_parse((const uint8_t *)raw, strlen(raw), &request) == AIUS_HTTP_OK);
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
	assert(aius_http_parse((const uint8_t *)odd, strlen(odd), &request) == AIUS_HTTP_OK);
	const char *bad_id = "GET /sd/replays/a.b HTTP/1.0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)bad_id, strlen(bad_id), &request) == AIUS_HTTP_ERR_ROUTE);
}

int main(void)
{
	AiusHttpRequest request;
	test_sd_auth_header();
	test_dsp_config_route();
	test_replay_routes();
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
	parse_ok("POST /api/mode HTTP/1.0\r\nContent-Length: 14\r\n\r\n{\"mode\":\"flat\"}",
	         AIUS_HTTP_ROUTE_MODE);
	const char *mix = "POST /api/mix HTTP/1.0\r\n\r\n{\"mix\":\"clipfree\"}";
	assert(aius_http_parse((const uint8_t *)mix, strlen(mix), &request) == AIUS_HTTP_OK);
	assert(request.mix == 2);
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
	       AIUS_HTTP_ERR_BODY);
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
