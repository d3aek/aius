#ifndef AIUS_HTTP_H
#define AIUS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AIUS_HTTP_MAX_REQUEST/AIUS_HTTP_MAX_BODY must fit the largest non-streamed
 * body accepted by any route: PUT /api/dsp/config, bounded to 12288 bytes
 * (AIUS_DSP_MODEL_MAX_JSON_BYTES in aius_dsp_config.h). 1024 bytes of headroom
 * covers the request line + headers (X-Aius-SD-Auth, Content-Length, etc.)
 * above the body itself. */
#define AIUS_HTTP_MAX_REQUEST 13312u
#define AIUS_HTTP_MAX_HEADERS 512u
#define AIUS_HTTP_MAX_SD_AUTH 16u /* fixed-format SD auth token width */
#define AIUS_HTTP_MAX_REPLAY_ID 48u
#define AIUS_HTTP_MAX_BODY 12288u
#define AIUS_HTTP_MAX_RESPONSE 8192u
#define AIUS_HTTP_CONTENT_TYPE_STORAGE 64u

typedef enum {
	AIUS_HTTP_GET = 1,
	AIUS_HTTP_POST = 2,
	AIUS_HTTP_DELETE = 3,
	AIUS_HTTP_PUT = 4
} AiusHttpMethod;

typedef enum {
	AIUS_HTTP_ROUTE_UNKNOWN = 0,
	AIUS_HTTP_ROUTE_HOME,
	AIUS_HTTP_ROUTE_STATUS,
	AIUS_HTTP_ROUTE_EVENTS,
	AIUS_HTTP_ROUTE_MODE,
	AIUS_HTTP_ROUTE_MIX,
	AIUS_HTTP_ROUTE_START,
	AIUS_HTTP_ROUTE_STOP,
	AIUS_HTTP_ROUTE_CLEAR,
	AIUS_HTTP_ROUTE_REBOOT,
	AIUS_HTTP_ROUTE_CONFIG_PUBLIC,
	AIUS_HTTP_ROUTE_SD_STATUS,
	AIUS_HTTP_ROUTE_SD_FILE,
	AIUS_HTTP_ROUTE_DSP_CONFIG,
	AIUS_HTTP_ROUTE_REPLAYS,
	AIUS_HTTP_ROUTE_REPLAYS_STATUS,
	AIUS_HTTP_ROUTE_REPLAYS_START,
	AIUS_HTTP_ROUTE_REPLAYS_STOP,
	AIUS_HTTP_ROUTE_REPLAYS_DELETE,
	AIUS_HTTP_ROUTE_SD_REPLAY
} AiusHttpRoute;

typedef enum {
	AIUS_HTTP_OK = 0,
	AIUS_HTTP_ERR_EMPTY,
	AIUS_HTTP_ERR_MALFORMED,
	AIUS_HTTP_ERR_TIMEOUT,
	AIUS_HTTP_ERR_TOO_LARGE,
	AIUS_HTTP_ERR_METHOD,
	AIUS_HTTP_ERR_ROUTE,
	AIUS_HTTP_ERR_BODY
} AiusHttpResult;

typedef struct {
	AiusHttpMethod method;
	AiusHttpRoute route;
	uint32_t cursor;
	uint8_t mode;
	uint8_t mix;
	uint8_t has_body;
	size_t header_length;
	bool has_content_length;
	uint64_t content_length;
	const uint8_t *body;
	size_t body_length;
	bool body_complete;
	const uint8_t *resource;
	size_t resource_length;
	/* Replay ID for /sd/replays/<id>, pointing into the parsed request bytes. */
	const uint8_t *replay_id;
	size_t replay_id_length;
	/* Optional exact-header value, pointing into the parsed request bytes. */
	const uint8_t *sd_auth;
	size_t sd_auth_length;
	/* Optional validated Content-Range metadata. */
	uint8_t has_content_range;
	uint32_t content_range_start;
	uint32_t content_range_end;
	uint32_t content_range_total;
} AiusHttpRequest;

AiusHttpResult aius_http_parse(const uint8_t *data, size_t length,
	                               AiusHttpRequest *request);

const char *aius_http_reason(unsigned status);
size_t aius_http_write_headers(uint8_t *output, size_t capacity,
	                           unsigned status, const char *content_type,
	                           uint64_t content_length);
size_t aius_http_write_response(uint8_t *output, size_t capacity,
	                              unsigned status, const char *content_type,
	                              const uint8_t *body, size_t body_length);

#ifdef __cplusplus
}
#endif

#endif
