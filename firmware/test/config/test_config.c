#include "aius_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);

static unsigned int allocation_calls;

void *__wrap_malloc(size_t size)
{
	allocation_calls++;
	return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
	allocation_calls++;
	return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	allocation_calls++;
	return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
	allocation_calls++;
	__real_free(ptr);
}

static unsigned int failures;

static void check(int condition, const char *format, ...)
{
	if (condition)
		return;
	failures++;
	va_list args;
	va_start(args, format);
	fputs("FAIL: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static void check_safe_defaults(const AiusConfig *config, const char *label)
{
	check(config->valid == 0u, "%s: invalid result marked valid", label);
	check(config->features.sd_web == 0u &&
	          config->features.remote_control == 0u &&
	          config->features.telemetry == 0u,
	      "%s: invalid result did not restore safe defaults", label);
	check(memchr(config->error_message, '\0', AIUS_CONFIG_ERROR_MESSAGE_MAX) != NULL,
	      "%s: error message is not bounded", label);
	check(config->error_message[0] != '\0', "%s: missing error message", label);
}

static void expect_error(const char *json, size_t len,
	                         AiusConfigErrorCode expected, const char *label)
{
	AiusConfig config;
	memset(&config, 0xa5, sizeof(config));
	unsigned int allocations_before = allocation_calls;
	AiusConfigErrorCode actual = aius_config_parse(
	    (const uint8_t *)json, len, &config);
	check(actual == expected, "%s: got error %d, want %d", label, actual,
	      expected);
	check(config.error_code == expected, "%s: result error code mismatch", label);
	check(strcmp(config.error_message, aius_config_error_message(expected)) == 0,
	      "%s: unexpected error message '%s'", label, config.error_message);
	check_safe_defaults(&config, label);
	check(allocation_calls == allocations_before,
	      "%s: parser used heap (%u calls)", label,
	      allocation_calls - allocations_before);
}

static void expect_valid(const char *json, size_t len, uint8_t sd_web,
	                         uint8_t remote_control, uint8_t telemetry,
	                         const char *label)
{
	AiusConfig config;
	unsigned int allocations_before = allocation_calls;
	AiusConfigErrorCode actual = aius_config_parse(
	    (const uint8_t *)json, len, &config);
	check(actual == AIUS_CONFIG_OK, "%s: got error %d", label, actual);
	check(config.error_code == AIUS_CONFIG_OK && config.valid == 1u,
	      "%s: successful result status is wrong", label);
	check(config.features.sd_web == sd_web &&
	          config.features.remote_control == remote_control &&
	          config.features.telemetry == telemetry,
	      "%s: parsed flags are wrong", label);
	check(strcmp(config.error_message, "ok") == 0,
	      "%s: successful message is not 'ok'", label);
	check(allocation_calls == allocations_before,
	      "%s: parser used heap (%u calls)", label,
	      allocation_calls - allocations_before);
}

static void test_defaults_and_valid_documents(void)
{
	AiusConfig config;
	memset(&config, 0xa5, sizeof(config));
	aius_config_set_defaults(&config);
	check_safe_defaults(&config, "defaults");
	check(config.error_code == AIUS_CONFIG_ERR_NOT_LOADED,
	      "defaults: wrong status code");
	check(strcmp(config.error_message, "not loaded") == 0,
	      "defaults: wrong status message");

	const char *canonical =
	    "{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	    "\"telemetry\":true}}";
	expect_valid(canonical, strlen(canonical), 1u, 0u, 1u, "canonical");

	const char *reordered =
	    " \n{ \"features\" : { \"telemetry\" : false, "
	    "\"sd_web\" : false, \"remote_control\" : true } } \t\n";
	expect_valid(reordered, strlen(reordered), 0u, 1u, 0u, "whitespace/order");

	const char *escaped_keys =
	    "{\"featur\\u0065s\":{\"sd_\\u0077eb\":true,"
	    "\"remote_\\u0063ontrol\":false,\"telemetry\":true}}";
	expect_valid(escaped_keys, strlen(escaped_keys), 1u, 0u, 1u,
	             "escaped schema keys");

	char at_limit[AIUS_CONFIG_MAX_JSON_BYTES + 1u];
	size_t canonical_len = strlen(canonical);
	memcpy(at_limit, canonical, canonical_len);
	memset(at_limit + canonical_len, ' ',
	       AIUS_CONFIG_MAX_JSON_BYTES - canonical_len);
	expect_valid(at_limit, AIUS_CONFIG_MAX_JSON_BYTES, 1u, 0u, 1u,
	             "maximum accepted length");
}

static void test_argument_and_size_errors(void)
{
	AiusConfig config;
	aius_config_set_defaults(NULL);
	check(aius_config_parse(NULL, 1u, &config) ==
	          AIUS_CONFIG_ERR_NULL_ARGUMENT,
	      "null input: wrong error");
	check_safe_defaults(&config, "null input");
	check(aius_config_parse((const uint8_t *)"{}", 0u, &config) ==
	          AIUS_CONFIG_ERR_EMPTY,
	      "empty input: wrong error");
	check_safe_defaults(&config, "empty input");
	check(aius_config_parse((const uint8_t *)"{}", 2u, NULL) ==
	          AIUS_CONFIG_ERR_NULL_ARGUMENT,
	      "null output: wrong error");

	char oversized[AIUS_CONFIG_MAX_JSON_BYTES + 1u];
	memset(oversized, ' ', sizeof(oversized));
	expect_error(oversized, sizeof(oversized), AIUS_CONFIG_ERR_TOO_LARGE,
	             "oversized input");
}

static void test_malformed_documents(void)
{
	expect_error("", 0u, AIUS_CONFIG_ERR_EMPTY, "empty document");
	expect_error("[ ]", 3u, AIUS_CONFIG_ERR_WRONG_TYPE, "root array");
	expect_error("null", 4u, AIUS_CONFIG_ERR_WRONG_TYPE, "root scalar");
	expect_error("{", 1u, AIUS_CONFIG_ERR_MALFORMED_JSON, "truncated root");
	expect_error("{\"features\":", 12u,
	             AIUS_CONFIG_ERR_MALFORMED_JSON, "missing features value");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	             "\"telemetry\":true}",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
                   "\"telemetry\":true}"),
	             AIUS_CONFIG_ERR_MALFORMED_JSON, "missing root close");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	             "\"telemetry\":true}} trailing",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
                   "\"telemetry\":true}} trailing"),
	             AIUS_CONFIG_ERR_MALFORMED_JSON, "trailing data");
	expect_error("{\"features\\q\":{}}", strlen("{\"features\\q\":{}}"),
	             AIUS_CONFIG_ERR_MALFORMED_JSON, "bad escape");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	             "\"telemetry\":true,}}",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
                   "\"telemetry\":true,}}"),
	             AIUS_CONFIG_ERR_MALFORMED_JSON, "trailing comma");
}

static void test_schema_errors(void)
{
	expect_error("{}", 2u, AIUS_CONFIG_ERR_MISSING_FEATURES,
	             "missing features object");
	expect_error("{\"other\":true}", strlen("{\"other\":true}"),
	             AIUS_CONFIG_ERR_UNKNOWN_KEY, "unknown root key");
	expect_error("{\"features\":{\"other\":true}}",
	             strlen("{\"features\":{\"other\":true}}"),
	             AIUS_CONFIG_ERR_UNKNOWN_KEY, "unknown feature key");
	expect_error("{\"features\":{\"sd_web\":true,\"sd_web\":false,"
	             "\"remote_control\":false,\"telemetry\":true}}",
	             strlen("{\"features\":{\"sd_web\":true,\"sd_web\":false,"
	                   "\"remote_control\":false,\"telemetry\":true}}"),
	             AIUS_CONFIG_ERR_DUPLICATE_KEY, "duplicate feature key");
	expect_error("{\"features\":{},\"features\":{}}",
	             strlen("{\"features\":{},\"features\":{}}"),
	             AIUS_CONFIG_ERR_MISSING_FEATURE, "duplicate root after bad object");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	             "\"telemetry\":true},\"features\":{\"sd_web\":true,"
	             "\"remote_control\":false,\"telemetry\":true}}",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	                   "\"telemetry\":true},\"features\":{\"sd_web\":true,"
	                   "\"remote_control\":false,\"telemetry\":true}}"),
	             AIUS_CONFIG_ERR_DUPLICATE_KEY, "duplicate root key");

	const char *missing_sd = "{\"features\":{\"remote_control\":false,\"telemetry\":true}}";
	const char *missing_remote = "{\"features\":{\"sd_web\":true,\"telemetry\":true}}";
	const char *missing_telemetry = "{\"features\":{\"sd_web\":true,\"remote_control\":false}}";
	expect_error(missing_sd, strlen(missing_sd), AIUS_CONFIG_ERR_MISSING_FEATURE,
	             "missing sd_web");
	expect_error(missing_remote, strlen(missing_remote),
	             AIUS_CONFIG_ERR_MISSING_FEATURE, "missing remote_control");
	expect_error(missing_telemetry, strlen(missing_telemetry),
	             AIUS_CONFIG_ERR_MISSING_FEATURE, "missing telemetry");

	expect_error("{\"features\":true}", strlen("{\"features\":true}"),
	             AIUS_CONFIG_ERR_WRONG_TYPE, "features boolean");
	expect_error("{\"features\":{\"sd_web\":\"true\",\"remote_control\":false,"
	             "\"telemetry\":true}}",
	             strlen("{\"features\":{\"sd_web\":\"true\",\"remote_control\":false,"
	                   "\"telemetry\":true}}"),
	             AIUS_CONFIG_ERR_WRONG_TYPE, "sd_web string");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":1,"
	             "\"telemetry\":true}}",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":1,"
	                   "\"telemetry\":true}}"),
	             AIUS_CONFIG_ERR_WRONG_TYPE, "remote_control number");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	             "\"telemetry\":null}}",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	                   "\"telemetry\":null}}"),
	             AIUS_CONFIG_ERR_WRONG_TYPE, "telemetry null");
	expect_error("{\"features\":{\"sd_web\":tru,\"remote_control\":false,"
	             "\"telemetry\":true}}",
	             strlen("{\"features\":{\"sd_web\":tru,\"remote_control\":false,"
	                   "\"telemetry\":true}}"),
	             AIUS_CONFIG_ERR_MALFORMED_JSON, "truncated boolean");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	             "\"telemetry\":{}}}",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	                   "\"telemetry\":{}}}"),
	             AIUS_CONFIG_ERR_WRONG_TYPE, "telemetry object");
	expect_error("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	             "\"telemetry\":[true]}}",
	             strlen("{\"features\":{\"sd_web\":true,\"remote_control\":false,"
	                   "\"telemetry\":[true]}}"),
	             AIUS_CONFIG_ERR_WRONG_TYPE, "telemetry array");

}

int main(void)
{
	test_defaults_and_valid_documents();
	test_argument_and_size_errors();
	test_malformed_documents();
	test_schema_errors();
	if (failures != 0u) {
		fprintf(stderr, "RESULT: %u check(s) failed\n", failures);
		return 1;
	}
	puts("ok aius config contract (bounded, strict, no heap)");
	return 0;
}
