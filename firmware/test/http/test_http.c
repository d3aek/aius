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
	/* PUT /api/dsp/config writes the SD-hosted DSP config override; only POST
	 * and DELETE are rejected on this route now. */
	assert(aius_http_parse((const uint8_t *)put, strlen(put), &request) == AIUS_HTTP_OK);
	assert(request.route == AIUS_HTTP_ROUTE_DSP_CONFIG);
	assert(aius_http_parse((const uint8_t *)delete, strlen(delete), &request) ==
	       AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)near_miss, strlen(near_miss), &request) ==
	       AIUS_HTTP_ERR_ROUTE);
}

/* The measurement loopback's two routes: the public read is GET-only, the arm
 * is POST-only, and a near-miss path stays unknown. */
static void test_measure_routes(void)
{
	AiusHttpRequest request;
	const char *post_read = "POST /api/measure HTTP/1.0\r\n\r\n";
	const char *get_start = "GET /api/measure/start HTTP/1.0\r\n\r\n";
	const char *put_read = "PUT /api/measure HTTP/1.0\r\n\r\n";
	const char *near_miss = "GET /api/measurement HTTP/1.0\r\n\r\n";

	parse_ok("GET /api/measure HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_MEASURE);
	parse_ok("POST /api/measure/start HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}",
	         AIUS_HTTP_ROUTE_MEASURE_START);
	assert(aius_http_parse((const uint8_t *)post_read, strlen(post_read),
	                       &request) == AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)get_start, strlen(get_start),
	                       &request) == AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)put_read, strlen(put_read),
	                       &request) == AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)near_miss, strlen(near_miss),
	                       &request) == AIUS_HTTP_ERR_ROUTE);
}

/* The OTA routes (docs/plans/aius-ota-upgrade.md slice 3 + the activation
 * slice). GET /api/ota is the open read, PUT /api/ota/image the resumable
 * buffered chunk (admitted into the PUT allow-list and carrying a body +
 * Content-Range), and the six verbs (arm / verify / abort / activate /
 * enable-activation / self-test-flash) POST-only. The ids and their matching
 * are unconditional here, so the parser
 * recognises them in this suite even though it is compiled without AIUS_OTA. The
 * dispatcher, not the parser, owns the absent-then-token gate. */
static void test_ota_routes(void)
{
	AiusHttpRequest request;
	const char *post_ota = "POST /api/ota HTTP/1.0\r\n\r\n";
	const char *get_image = "GET /api/ota/image HTTP/1.0\r\n\r\n";
	const char *put_arm =
		"PUT /api/ota/arm HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}";
	const char *get_arm = "GET /api/ota/arm HTTP/1.0\r\n\r\n";
	const char *get_activate = "GET /api/ota/activate HTTP/1.0\r\n\r\n";
	const char *near_miss = "GET /api/otax HTTP/1.0\r\n\r\n";
	const char *bare = "GET /api/ot HTTP/1.0\r\n\r\n";

	parse_ok("GET /api/ota HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_OTA);
	parse_ok("POST /api/ota/arm HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_OTA_ARM);
	parse_ok("POST /api/ota/verify HTTP/1.0\r\n\r\n",
	         AIUS_HTTP_ROUTE_OTA_VERIFY);
	parse_ok("POST /api/ota/abort HTTP/1.0\r\n\r\n",
	         AIUS_HTTP_ROUTE_OTA_ABORT);
	/* The activation-wiring slice's three POST verbs. */
	parse_ok("POST /api/ota/activate HTTP/1.0\r\n\r\n",
	         AIUS_HTTP_ROUTE_OTA_ACTIVATE);
	parse_ok("POST /api/ota/enable-activation HTTP/1.0\r\n\r\n",
	         AIUS_HTTP_ROUTE_OTA_ENABLE_ACTIVATION);
	parse_ok("POST /api/ota/self-test-flash HTTP/1.0\r\n\r\n",
	         AIUS_HTTP_ROUTE_OTA_SELF_TEST_FLASH);

	/* PUT /api/ota/image carries a body and a Content-Range and is admitted
	 * into the PUT allow-list -- the resumable-chunk shape. */
	{
		const char *image =
			"PUT /api/ota/image HTTP/1.0\r\nContent-Length: 4\r\n"
			"Content-Range: bytes 0-3/1024\r\n\r\nABCD";
		assert(aius_http_parse((const uint8_t *)image, strlen(image),
		                       &request) == AIUS_HTTP_OK);
		assert(request.route == AIUS_HTTP_ROUTE_OTA_IMAGE);
		assert(request.has_content_range == 1u);
		assert(request.body_length == 4u);
	}

	/* Method rules: the read is GET-only, the image PUT-only, the verbs
	 * POST-only. */
	assert(aius_http_parse((const uint8_t *)post_ota, strlen(post_ota),
	                       &request) == AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)get_image, strlen(get_image),
	                       &request) == AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)put_arm, strlen(put_arm),
	                       &request) == AIUS_HTTP_ERR_METHOD);
	assert(aius_http_parse((const uint8_t *)get_arm, strlen(get_arm),
	                       &request) == AIUS_HTTP_ERR_METHOD);
	/* activate is POST-only too (enable-activation and self-test-flash share
	 * the arm above). */
	assert(aius_http_parse((const uint8_t *)get_activate, strlen(get_activate),
	                       &request) == AIUS_HTTP_ERR_METHOD);

	/* No shadow of a neighbouring route, and no prefix match. */
	assert(aius_http_parse((const uint8_t *)near_miss, strlen(near_miss),
	                       &request) == AIUS_HTTP_ERR_ROUTE);
	assert(aius_http_parse((const uint8_t *)bare, strlen(bare),
	                       &request) == AIUS_HTTP_ERR_ROUTE);
}

/* The directory index rides the existing /sd/ parse: a resource ending in a
 * slash is an ordinary AIUS_HTTP_ROUTE_SD_FILE resource, and the trailing slash
 * survives into `resource` for the dispatcher to read. That is why the listing
 * needed no new route and no parser change, and this pins it. */
static void test_sd_directory_resource(void)
{
	AiusHttpRequest request;
	static const struct { const char *raw; const char *resource; } rows[] = {
		{ "GET /sd/www/ HTTP/1.0\r\n\r\n", "www/" },
		{ "GET /sd/www/sub/ HTTP/1.0\r\n\r\n", "www/sub/" },
		{ "GET /sd/www/index.html HTTP/1.0\r\n\r\n", "www/index.html" }
	};
	/* "/sd/" alone stays unknown: the prefix match needs at least one byte of
	 * resource, so the card root is not browsable and the subtree root is
	 * spelled /sd/www/. */
	const char *root = "GET /sd/ HTTP/1.0\r\n\r\n";

	for (size_t i = 0; i < ARRAY_LENGTH(rows); ++i) {
		assert(aius_http_parse((const uint8_t *)rows[i].raw,
		                       strlen(rows[i].raw), &request) == AIUS_HTTP_OK);
		assert(request.route == AIUS_HTTP_ROUTE_SD_FILE);
		assert(request.resource_length == strlen(rows[i].resource));
		assert(memcmp(request.resource, rows[i].resource,
		              request.resource_length) == 0);
	}
	assert(aius_http_parse((const uint8_t *)root, strlen(root), &request) ==
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
	/* Field names are case-insensitive (RFC 9110 5.1), so a lowercase spelling
	 * binds the token exactly as the canonical one does. Name-case coverage
	 * proper lives in test_sd_auth_header_name_case(). */
	const char *other_case = "GET /api/status HTTP/1.0\r\n"
	                        "x-aius-sd-auth: 0123456789abcdef\r\n\r\n";
	/* Duplicate rejection is name-folded too: these two spellings are one
	 * header, so the second is still a duplicate. */
	const char *duplicate_mixed_case = "GET /api/status HTTP/1.0\r\n"
	                       "X-Aius-SD-Auth: 0123456789abcdef\r\n"
	                       "x-AiUs-Sd-AuTh: fedcba9876543210\r\n\r\n";
	/* The length and malformed guards fire on a folded name, not just the
	 * canonical one. */
	const char *malformed_folded = "GET /api/status HTTP/1.0\r\n"
	                        "X-AIUS-SD-AUTH:\r\n\r\n";
	const char *oversized_folded = "GET /api/status HTTP/1.0\r\n"
	                       "x-aius-sd-auth: 0123456789abcdef0\r\n\r\n";

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
	assert(aius_http_parse((const uint8_t *)other_case, strlen(other_case), &request) ==
	       AIUS_HTTP_OK);
	assert(request.sd_auth_length == AIUS_HTTP_MAX_SD_AUTH);
	assert(memcmp(request.sd_auth, "0123456789abcdef", AIUS_HTTP_MAX_SD_AUTH) == 0);
	assert(aius_http_parse((const uint8_t *)duplicate_mixed_case,
	                       strlen(duplicate_mixed_case), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	assert(aius_http_parse((const uint8_t *)malformed_folded,
	                       strlen(malformed_folded), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	assert(aius_http_parse((const uint8_t *)oversized_folded,
	                       strlen(oversized_folded), &request) ==
	       AIUS_HTTP_ERR_TOO_LARGE);
}

/* RFC 9110 5.1 -- "Field names are case-insensitive". Every spelling of the
 * name below must bind the same token, and the token VALUE must survive
 * byte-for-byte: `token` is deliberately mixed case, so a fold that leaked from
 * the name onto the value would fail the memcmp.
 *
 * The `matches == 0` rows are near misses. "bit5-non-alpha" is the one a
 * careless `| 0x20` (or XOR 0x20) fold gets wrong: it equates any two bytes
 * differing only in bit 5, including non-alphabetic pairs such as '_' 0x5F /
 * '?' 0x3F. This name's only reachable such pair is '-' 0x2D / CR 0x0D, so the
 * row swaps the first hyphen for a bare CR -- which is not "\r\n" and so does
 * not end the header line. A correct ASCII-only fold rejects it; a bit-5 fold
 * accepts a corrupted field name. */
static void test_sd_auth_header_name_case(void)
{
	static const char token[] = "AbCdEf0123456789";
	static const struct {
		const char *key;
		const char *name;
		int matches;
	} rows[] = {
		{ "canonical", "X-Aius-SD-Auth", 1 },
		{ "lower", "x-aius-sd-auth", 1 },
		{ "upper", "X-AIUS-SD-AUTH", 1 },
		{ "mixed", "x-AiUs-Sd-AuTh", 1 },
		/* urllib.request title-cases field names; this is the live 401. */
		{ "title", "X-aius-sd-auth", 1 },
		{ "suffixed", "X-Aius-SD-Auth-Extra", 0 },
		{ "truncated", "X-Aius-SD-Aut", 0 },
		{ "letter-differs", "X-Aius-SD-Autz", 0 },
		{ "bit5-non-alpha", "X\rAius-SD-Auth", 0 }
	};

	for (size_t i = 0; i < ARRAY_LENGTH(rows); ++i) {
		AiusHttpRequest request;
		char raw[160];
		int length;
		AiusHttpResult result;
		for (size_t j = 0; j < i; ++j)
			assert(strcmp(rows[i].key, rows[j].key) != 0);
		length = snprintf(raw, sizeof(raw),
		                  "GET /api/status HTTP/1.0\r\n%s: %s\r\n\r\n",
		                  rows[i].name, token);
		assert(length > 0);
		assert((size_t)length < sizeof(raw));
		result = aius_http_parse((const uint8_t *)raw, (size_t)length, &request);
		if (result != AIUS_HTTP_OK)
			fprintf(stderr, "sd-auth name %s: expected result %d, actual %d\n",
			        rows[i].key, (int)AIUS_HTTP_OK, (int)result);
		assert(result == AIUS_HTTP_OK);
		if (rows[i].matches) {
			if (request.sd_auth == NULL)
				fprintf(stderr, "sd-auth name %s: expected a bound token\n",
				        rows[i].key);
			assert(request.sd_auth != NULL);
			assert(request.sd_auth_length == strlen(token));
			/* Byte-for-byte: the value keeps its own case. */
			assert(memcmp(request.sd_auth, token, strlen(token)) == 0);
		} else {
			if (request.sd_auth != NULL)
				fprintf(stderr, "sd-auth name %s: expected no match, bound %zu bytes\n",
				        rows[i].key, request.sd_auth_length);
			assert(request.sd_auth == NULL);
			assert(request.sd_auth_length == 0);
		}
	}
}

/* Same rule for Content-Range: the NAME folds, the value does not. A "BYTES"
 * unit token is an unrecognised range and is ignored exactly as before, which
 * on a complete PUT to /sd/replays/<id> surfaces as ERR_BODY -- that is what
 * every non-matching row asserts. */
static void test_content_range_header_name_case(void)
{
	static const struct {
		const char *key;
		const char *name;
		const char *value;
		int matches;
	} rows[] = {
		{ "canonical", "Content-Range", " bytes 0-3/8", 1 },
		{ "lower", "content-range", " bytes 0-3/8", 1 },
		{ "upper", "CONTENT-RANGE", " bytes 0-3/8", 1 },
		{ "mixed", "CoNtEnT-RaNgE", " bytes 0-3/8", 1 },
		{ "suffixed", "Content-Range-Extra", " bytes 0-3/8", 0 },
		{ "truncated", "Content-Rang", " bytes 0-3/8", 0 },
		{ "bit5-non-alpha", "Content\rRange", " bytes 0-3/8", 0 },
		/* Value stays case-sensitive: an upper-case unit is not "bytes". */
		{ "value-case", "Content-Range", " BYTES 0-3/8", 0 }
	};

	for (size_t i = 0; i < ARRAY_LENGTH(rows); ++i) {
		AiusHttpRequest request;
		char raw[192];
		int length;
		AiusHttpResult result;
		AiusHttpResult expected = rows[i].matches ? AIUS_HTTP_OK : AIUS_HTTP_ERR_BODY;
		for (size_t j = 0; j < i; ++j)
			assert(strcmp(rows[i].key, rows[j].key) != 0);
		length = snprintf(raw, sizeof(raw),
		                  "PUT /sd/replays/take1 HTTP/1.0\r\n%s:%s\r\n"
		                  "Content-Length: 4\r\n\r\nABCD",
		                  rows[i].name, rows[i].value);
		assert(length > 0);
		assert((size_t)length < sizeof(raw));
		result = aius_http_parse((const uint8_t *)raw, (size_t)length, &request);
		if (result != expected)
			fprintf(stderr, "content-range name %s: expected result %d, actual %d\n",
			        rows[i].key, (int)expected, (int)result);
		assert(result == expected);
		if (rows[i].matches) {
			assert(request.has_content_range);
			assert(request.content_range_start == 0);
			assert(request.content_range_end == 3);
			assert(request.content_range_total == 8);
		} else {
			assert(!request.has_content_range);
		}
	}
}

/* Content-Length and Transfer-Encoding already folded their names; these rows
 * pin that, including the same bit-5 near miss, so the three parsed headers
 * share one proven rule. */
static void test_framing_header_name_case(void)
{
	static const struct {
		const char *key;
		const char *name;
		int matches;
	} lengths[] = {
		{ "canonical", "Content-Length", 1 },
		{ "lower", "content-length", 1 },
		{ "upper", "CONTENT-LENGTH", 1 },
		{ "mixed", "CoNtEnT-LeNgTh", 1 },
		{ "suffixed", "Content-Length-Extra", 0 },
		{ "bit5-non-alpha", "Content\rLength", 0 }
	};
	static const struct {
		const char *key;
		const char *name;
		int matches;
	} encodings[] = {
		{ "canonical", "Transfer-Encoding", 1 },
		{ "lower", "transfer-encoding", 1 },
		{ "upper", "TRANSFER-ENCODING", 1 },
		{ "mixed", "TrAnSfEr-EnCoDiNg", 1 },
		{ "suffixed", "Transfer-Encoding-Extra", 0 },
		{ "bit5-non-alpha", "Transfer\rEncoding", 0 }
	};
	AiusHttpRequest request;
	/* One header spelled two ways is still a duplicate. */
	const char *duplicate =
		"POST /api/clear HTTP/1.0\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n";

	for (size_t i = 0; i < ARRAY_LENGTH(lengths); ++i) {
		char raw[192];
		int length;
		AiusHttpResult result;
		/* Body bytes with no recognised Content-Length are malformed, so a
		 * missed name is observable without a second assertion path. */
		AiusHttpResult expected =
			lengths[i].matches ? AIUS_HTTP_OK : AIUS_HTTP_ERR_MALFORMED;
		length = snprintf(raw, sizeof(raw),
		                  "PUT /api/config/public HTTP/1.0\r\n%s: 2\r\n\r\n{}",
		                  lengths[i].name);
		assert(length > 0);
		assert((size_t)length < sizeof(raw));
		result = aius_http_parse((const uint8_t *)raw, (size_t)length, &request);
		if (result != expected)
			fprintf(stderr, "content-length name %s: expected result %d, actual %d\n",
			        lengths[i].key, (int)expected, (int)result);
		assert(result == expected);
		if (lengths[i].matches) {
			assert(request.has_content_length);
			assert(request.content_length == 2);
		}
	}

	for (size_t i = 0; i < ARRAY_LENGTH(encodings); ++i) {
		char raw[192];
		int length;
		AiusHttpResult result;
		AiusHttpResult expected =
			encodings[i].matches ? AIUS_HTTP_ERR_MALFORMED : AIUS_HTTP_OK;
		length = snprintf(raw, sizeof(raw),
		                  "POST /api/clear HTTP/1.0\r\n%s: chunked\r\n\r\n",
		                  encodings[i].name);
		assert(length > 0);
		assert((size_t)length < sizeof(raw));
		result = aius_http_parse((const uint8_t *)raw, (size_t)length, &request);
		if (result != expected)
			fprintf(stderr, "transfer-encoding name %s: expected result %d, actual %d\n",
			        encodings[i].key, (int)expected, (int)result);
		assert(result == expected);
	}

	assert(aius_http_parse((const uint8_t *)duplicate, strlen(duplicate), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
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
	/* Framing is decided before the route table is consulted, so any POST route
	 * exercises these two; they moved off the retired /api/mode. */
	const char *duplicate =
		"POST /api/clear HTTP/1.0\r\nContent-Length: 0\r\n"
		"Content-Length: 0\r\n\r\n";
	assert(aius_http_parse((const uint8_t *)duplicate, strlen(duplicate), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *transfer =
		"POST /api/clear HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n";
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
	uint8_t headers[224];
	size_t length = aius_http_write_headers(headers, sizeof(headers), 416,
	                                        "application/octet-stream",
	                                        UINT64_MAX, 0u);
	assert(length != 0);
	headers[length] = 0;
	/* Zero max-age is BYTE-IDENTICAL to the pre-cache header set: no
	 * Cache-Control line at all, not max-age=0. */
	assert(strcmp((const char *)headers,
	              "HTTP/1.0 416 Range Not Satisfiable\r\n"
	              "Content-Type: application/octet-stream\r\n"
	              "Content-Length: 18446744073709551615\r\n"
	              "Connection: close\r\n\r\n") == 0);
	assert(aius_http_write_headers(NULL, sizeof(headers), 200, "text/plain", 0, 0u) == 0);
	assert(aius_http_write_headers(headers, sizeof(headers), 418, "text/plain", 0, 0u) == 0);
	assert(aius_http_write_headers(headers, sizeof(headers), 200, "", 0, 0u) == 0);
	assert(aius_http_write_headers(headers, 1, 200, "text/plain", 0, 0u) == 0);
}

static void test_response_headers_cache(void)
{
	uint8_t headers[224];
	/* The one value the dispatcher ships. */
	size_t length = aius_http_write_headers(headers, sizeof(headers), 200,
	                                        "text/css", 1234, 86400u);
	assert(length != 0);
	headers[length] = 0;
	assert(strcmp((const char *)headers,
	              "HTTP/1.0 200 OK\r\n"
	              "Content-Type: text/css\r\n"
	              "Content-Length: 1234\r\n"
	              "Cache-Control: max-age=86400\r\n"
	              "Connection: close\r\n\r\n") == 0);
	/* The joint worst case still fits: UINT64_MAX length, UINT32_MAX
	 * max-age, longest reason line the writer serves for octet-stream. */
	length = aius_http_write_headers(headers, sizeof(headers), 416,
	                                 "application/octet-stream",
	                                 UINT64_MAX, UINT32_MAX);
	assert(length != 0);
	headers[length] = 0;
	assert(strstr((const char *)headers,
	              "Cache-Control: max-age=4294967295\r\n") != NULL);
	assert(strstr((const char *)headers, "Connection: close\r\n\r\n") != NULL);
	/* And the capacity refusal still refuses. */
	assert(aius_http_write_headers(headers, 1, 200, "text/css", 0, 86400u) == 0);
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
	/* ERR_BODY now comes from the replay-chunk rule: a complete PUT body on
	 * /sd/replays/<id> with no Content-Range cannot be placed in the file. */
	static const char body[] =
		"PUT /sd/replays/trailer-51 HTTP/1.0\r\nContent-Length: 4\r\n\r\nABCD";
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
	test_sd_auth_header_name_case();
	test_content_range_header_name_case();
	test_framing_header_name_case();
	test_sd_directory_resource();
	test_dsp_config_route();
	test_measure_routes();
	test_ota_routes();
	test_replay_routes();
	test_framing();
	test_response_headers();
	test_response_headers_cache();
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
	/* The recorder surface: status/data are GET, freeze/thaw POST, config
	 * PUT; the data route's three params are fixed-order and all
	 * required. */
	parse_ok("GET /api/recorder HTTP/1.0\r\n\r\n", AIUS_HTTP_ROUTE_RECORDER);
	parse_ok("POST /api/recorder/freeze HTTP/1.0\r\n\r\n",
	         AIUS_HTTP_ROUTE_RECORDER_FREEZE);
	parse_ok("POST /api/recorder/thaw HTTP/1.0\r\n\r\n",
	         AIUS_HTTP_ROUTE_RECORDER_THAW);
	parse_ok("PUT /api/recorder/config HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}",
	         AIUS_HTTP_ROUTE_RECORDER_CONFIG);
	{
		const char *data_line =
			"GET /api/recorder/data?offset=65536&length=4096&generation=3"
			" HTTP/1.0\r\n\r\n";
		assert(aius_http_parse((const uint8_t *)data_line,
		                       strlen(data_line), &request) == AIUS_HTTP_OK);
		assert(request.route == AIUS_HTTP_ROUTE_RECORDER_DATA);
		assert(request.recorder_offset == 65536u);
		assert(request.recorder_length == 4096u);
		assert(request.recorder_generation == 3u);
	}
	{
		/* Missing generation, reordered params, bare data path, non-GET
		 * on data, GET on freeze, POST on config: all refused. */
		const char *missing =
			"GET /api/recorder/data?offset=0&length=8 HTTP/1.0\r\n\r\n";
		assert(aius_http_parse((const uint8_t *)missing, strlen(missing),
		                       &request) == AIUS_HTTP_ERR_ROUTE);
		const char *reordered =
			"GET /api/recorder/data?length=8&offset=0&generation=0"
			" HTTP/1.0\r\n\r\n";
		assert(aius_http_parse((const uint8_t *)reordered,
		                       strlen(reordered), &request) ==
		       AIUS_HTTP_ERR_ROUTE);
		const char *bare = "GET /api/recorder/data HTTP/1.0\r\n\r\n";
		assert(aius_http_parse((const uint8_t *)bare, strlen(bare),
		                       &request) == AIUS_HTTP_ERR_ROUTE);
		const char *post_data =
			"POST /api/recorder/data?offset=0&length=8&generation=0"
			" HTTP/1.0\r\n\r\n";
		assert(aius_http_parse((const uint8_t *)post_data,
		                       strlen(post_data), &request) ==
		       AIUS_HTTP_ERR_METHOD);
		const char *get_freeze = "GET /api/recorder/freeze HTTP/1.0\r\n\r\n";
		assert(aius_http_parse((const uint8_t *)get_freeze,
		                       strlen(get_freeze), &request) ==
		       AIUS_HTTP_ERR_METHOD);
		const char *post_config =
			"POST /api/recorder/config HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}";
		assert(aius_http_parse((const uint8_t *)post_config,
		                       strlen(post_config), &request) ==
		       AIUS_HTTP_ERR_METHOD);
	}
	parse_ok("GET /api/events HTTP/1.1\r\nHost: device\r\n\r\n", AIUS_HTTP_ROUTE_EVENTS);
	/* /api/mode and /api/mix are retired: the route table no longer knows them,
	 * so a well-formed request for either is an unknown route. */
	const char *mode = "POST /api/mode HTTP/1.0\r\nContent-Length: 15\r\n\r\n{\"mode\":\"flat\"}";
	assert(aius_http_parse((const uint8_t *)mode, strlen(mode), &request) ==
	       AIUS_HTTP_ERR_ROUTE);
	const char *retired_mix =
		"POST /api/mix HTTP/1.0\r\nContent-Length: 18\r\n\r\n{\"mix\":\"clipfree\"}";
	assert(aius_http_parse((const uint8_t *)retired_mix, strlen(retired_mix), &request) ==
	       AIUS_HTTP_ERR_ROUTE);
	/* Body bytes with no Content-Length are malformed whatever the POST route. */
	const char *undeclared_post =
		"POST /api/replays/stop HTTP/1.0\r\n\r\n{\"id\":\"take1\"}";
	assert(aius_http_parse((const uint8_t *)undeclared_post,
	                       strlen(undeclared_post), &request) ==
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
	const char *empty_body = "POST /api/replays/start HTTP/1.0\r\n\r\n{}";
	assert(aius_http_parse((const uint8_t *)empty_body, strlen(empty_body), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);
	const char *truncated = "GET /api/status HTTP/1.0\r\n";
	assert(aius_http_parse((const uint8_t *)truncated, strlen(truncated), &request) ==
	       AIUS_HTTP_ERR_MALFORMED);

	uint8_t response[128];
	static const uint8_t body[] = "{}";
	size_t response_length = aius_http_write_response(response, sizeof(response), 200,
	                                                 "application/json", body, 2, 0u);
	assert(response_length != 0);
	response[response_length] = 0;
	assert(strstr((const char *)response, "Connection: close") != NULL);
	assert(strstr((const char *)response, "Cache-Control") == NULL);
	assert(aius_http_write_response(response, 10, 200, "application/json", body, 2, 0u) == 0);
	puts("ok http parser");
	return 0;
}
