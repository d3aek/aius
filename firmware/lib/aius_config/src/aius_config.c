#include "aius_config.h"

#include "aius_json.h"

#include <string.h>

/* Cold code -> FLASH. This is the SD/PUT config parser: it runs at boot and on
 * an operator's config write, never on the audio block path, so every
 * definition below carries FLASHMEM and lands in FLASH instead of the shared
 * FlexRAM/ITCM pool the audio pipeline needs. FLASHMEM is Teensyduino's
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
#ifndef PROGMEM
#define PROGMEM
#endif

#include <stdio.h>

#define AIUS_CONFIG_KEY_BUFFER_MAX 32u
/* Only the skip-unknown path recurses on attacker structure; cap its depth. */
#define AIUS_CONFIG_MAX_NESTING 4u

/* The JSON cursor primitives -- at-end, peek, skip-whitespace, consume, string,
 * literal, number, skip-value -- live in lib/aius_json and are shared with
 * lib/aius_dsp_config and the measure body reader in src/aius_optical_app.cpp.
 * What stays here is this schema: the feature key set, its required-key mask and
 * this library's error vocabulary. */

static FLASHMEM void copy_message(char *dst, const char *src)
{
	size_t i = 0;
	while (i + 1u < AIUS_CONFIG_ERROR_MESSAGE_MAX && src[i] != '\0') {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

FLASHMEM const char *aius_config_error_message(AiusConfigErrorCode code)
{
	switch (code) {
	case AIUS_CONFIG_OK:
		return "ok";
	case AIUS_CONFIG_ERR_NULL_ARGUMENT:
		return "null argument";
	case AIUS_CONFIG_ERR_EMPTY:
		return "empty input";
	case AIUS_CONFIG_ERR_TOO_LARGE:
		return "input too large";
	case AIUS_CONFIG_ERR_MALFORMED_JSON:
		return "malformed JSON";
	case AIUS_CONFIG_ERR_UNKNOWN_KEY:
		return "unknown key";
	case AIUS_CONFIG_ERR_DUPLICATE_KEY:
		return "duplicate key";
	case AIUS_CONFIG_ERR_MISSING_FEATURES:
		return "missing features";
	case AIUS_CONFIG_ERR_MISSING_FEATURE:
		return "missing feature";
	case AIUS_CONFIG_ERR_WRONG_TYPE:
		return "wrong type";
	case AIUS_CONFIG_ERR_NOT_LOADED:
		return "not loaded";
	default:
		return "invalid error code";
	}
}

static FLASHMEM void set_status(AiusConfig *config, AiusConfigErrorCode code)
{
	config->valid = (uint8_t)(code == AIUS_CONFIG_OK);
	config->error_code = code;
	copy_message(config->error_message, aius_config_error_message(code));
}

FLASHMEM void aius_config_set_defaults(AiusConfig *config)
{
	if (config == NULL)
		return;
	config->features.sd_web = 0u;
	config->features.remote_control = 0u;
	config->features.telemetry = 0u;
	set_status(config, AIUS_CONFIG_ERR_NOT_LOADED);
}

static FLASHMEM int skip_value(AiusJsonCursor *parser)
{
	return aius_json_skip_value(parser, 0u, AIUS_CONFIG_MAX_NESTING);
}

static FLASHMEM AiusConfigErrorCode parse_boolean(AiusJsonCursor *parser,
	                                              uint8_t *value)
{
	size_t start = parser->pos;

	if (aius_json_peek(parser) == 'f') {
		if (!aius_json_literal(parser, "false"))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		*value = 0u;
		return AIUS_CONFIG_OK;
	}
	if (aius_json_peek(parser) == 't') {
		if (!aius_json_literal(parser, "true"))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		*value = 1u;
		return AIUS_CONFIG_OK;
	}
	if (!skip_value(parser) || parser->pos == start)
		return AIUS_CONFIG_ERR_MALFORMED_JSON;
	return AIUS_CONFIG_ERR_WRONG_TYPE;
}

static FLASHMEM AiusConfigErrorCode
parse_features(AiusJsonCursor *parser, AiusConfigFeatures *features)
{
	uint8_t seen = 0u;
	if (!aius_json_consume(parser, '{')) {
		if (!skip_value(parser))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_CONFIG_ERR_WRONG_TYPE;
	}
	aius_json_skip_whitespace(parser);
	if (aius_json_consume(parser, '}'))
		return AIUS_CONFIG_ERR_MISSING_FEATURE;

	for (;;) {
		uint8_t key[AIUS_CONFIG_KEY_BUFFER_MAX];
		size_t key_len = 0;
		int key_overflow = 0;
		if (!aius_json_take_string(parser, key, sizeof(key), &key_len,
		                           &key_overflow))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		aius_json_skip_whitespace(parser);

		unsigned int feature = 0u;
		if (aius_json_key_is(key, key_len, key_overflow, "sd_web"))
			feature = 1u;
		else if (aius_json_key_is(key, key_len, key_overflow, "remote_control"))
			feature = 2u;
		else if (aius_json_key_is(key, key_len, key_overflow, "telemetry"))
			feature = 3u;
		else
			return AIUS_CONFIG_ERR_UNKNOWN_KEY;

		uint8_t bit = (uint8_t)(1u << (feature - 1u));
		if ((seen & bit) != 0u)
			return AIUS_CONFIG_ERR_DUPLICATE_KEY;
		if (!aius_json_consume(parser, ':'))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		aius_json_skip_whitespace(parser);
		uint8_t value = 0u;
		AiusConfigErrorCode value_error = parse_boolean(parser, &value);
		if (value_error != AIUS_CONFIG_OK)
			return value_error;
		switch (feature) {
		case 1u: features->sd_web = value; break;
		case 2u: features->remote_control = value; break;
		default: features->telemetry = value; break;
		}
		seen |= bit;
		aius_json_skip_whitespace(parser);
		if (aius_json_consume(parser, '}'))
			break;
		if (!aius_json_consume(parser, ','))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		aius_json_skip_whitespace(parser);
		if (aius_json_peek(parser) == '}')
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
	}

	if (seen != 0x07u)
		return AIUS_CONFIG_ERR_MISSING_FEATURE;
	return AIUS_CONFIG_OK;
}

FLASHMEM size_t aius_config_write(const AiusConfigFeatures *features,
                                  uint8_t *body, size_t capacity)
{
	/* PROGMEM: a literal here would land in DTCM .data on the target. */
	static const char format_config[] PROGMEM =
		"{\"features\":{\"sd_web\":%s,\"remote_control\":%s,"
		"\"telemetry\":%s}}\n";
	static const char kTrue[] PROGMEM = "true";
	static const char kFalse[] PROGMEM = "false";
	int written;

	if (features == NULL || body == NULL || capacity == 0u)
		return 0u;
	written = snprintf((char *)body, capacity, format_config,
	                   features->sd_web ? kTrue : kFalse,
	                   features->remote_control ? kTrue : kFalse,
	                   features->telemetry ? kTrue : kFalse);
	if (written <= 0 || (size_t)written >= capacity)
		return 0u;
	return (size_t)written;
}

FLASHMEM AiusConfigErrorCode aius_config_parse(const uint8_t *json, size_t len,
	                                               AiusConfig *out)
{
	if (out == NULL)
		return AIUS_CONFIG_ERR_NULL_ARGUMENT;
	aius_config_set_defaults(out);
	if (json == NULL)
		return (set_status(out, AIUS_CONFIG_ERR_NULL_ARGUMENT),
		        AIUS_CONFIG_ERR_NULL_ARGUMENT);
	if (len == 0u)
		return (set_status(out, AIUS_CONFIG_ERR_EMPTY), AIUS_CONFIG_ERR_EMPTY);
	if (len > AIUS_CONFIG_MAX_JSON_BYTES)
		return (set_status(out, AIUS_CONFIG_ERR_TOO_LARGE),
		        AIUS_CONFIG_ERR_TOO_LARGE);

	AiusJsonCursor parser = {json, len, 0u};
	AiusConfigFeatures parsed = {0u, 0u, 0u};
	AiusConfigErrorCode error = AIUS_CONFIG_OK;
	aius_json_skip_whitespace(&parser);
	if (!aius_json_consume(&parser, '{')) {
		error = skip_value(&parser) ?
		             AIUS_CONFIG_ERR_WRONG_TYPE :
		             AIUS_CONFIG_ERR_MALFORMED_JSON;
		goto fail;
	}
	aius_json_skip_whitespace(&parser);
	if (aius_json_consume(&parser, '}')) {
		error = AIUS_CONFIG_ERR_MISSING_FEATURES;
		goto fail;
	}

	int seen_features = 0;
	for (;;) {
		uint8_t key[AIUS_CONFIG_KEY_BUFFER_MAX];
		size_t key_len = 0;
		int key_overflow = 0;
		if (!aius_json_take_string(&parser, key, sizeof(key), &key_len,
		                           &key_overflow)) {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
		aius_json_skip_whitespace(&parser);
		if (!aius_json_key_is(key, key_len, key_overflow, "features")) {
			error = AIUS_CONFIG_ERR_UNKNOWN_KEY;
			goto fail;
		}
		if (seen_features) {
			error = AIUS_CONFIG_ERR_DUPLICATE_KEY;
			goto fail;
		}
		seen_features = 1;
		if (!aius_json_consume(&parser, ':')) {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
		aius_json_skip_whitespace(&parser);
		error = parse_features(&parser, &parsed);
		if (error != AIUS_CONFIG_OK)
			goto fail;
		aius_json_skip_whitespace(&parser);
		if (aius_json_consume(&parser, '}'))
			break;
		if (!aius_json_consume(&parser, ',')) {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
		aius_json_skip_whitespace(&parser);
		if (aius_json_peek(&parser) == '}') {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
	}
	aius_json_skip_whitespace(&parser);
	if (!aius_json_at_end(&parser)) {
		error = AIUS_CONFIG_ERR_MALFORMED_JSON;
		goto fail;
	}

	out->features = parsed;
	set_status(out, AIUS_CONFIG_OK);
	return AIUS_CONFIG_OK;

fail:
	set_status(out, error);
	return error;
}
