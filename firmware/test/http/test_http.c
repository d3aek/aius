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

int main(void)
{
	AiusHttpRequest request;
	parse_ok("GET / HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_HOME);
	parse_ok("GET /api/sd HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_SD_STATUS);
	parse_ok("GET /sd/index.html HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_SD_FILE);
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
