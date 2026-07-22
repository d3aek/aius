#include "aius_config.h"

#include <string.h>

#define AIUS_CONFIG_KEY_BUFFER_MAX 32u
#define AIUS_CONFIG_MAX_NESTING 4u

typedef struct {
	const uint8_t *data;
	size_t len;
	size_t pos;
} AiusConfigParser;

static void copy_message(char *dst, const char *src)
{
	size_t i = 0;
	while (i + 1u < AIUS_CONFIG_ERROR_MESSAGE_MAX && src[i] != '\0') {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

const char *aius_config_error_message(AiusConfigErrorCode code)
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

static void set_status(AiusConfig *config, AiusConfigErrorCode code)
{
	config->valid = (uint8_t)(code == AIUS_CONFIG_OK);
	config->error_code = code;
	copy_message(config->error_message, aius_config_error_message(code));
}

void aius_config_set_defaults(AiusConfig *config)
{
	if (config == NULL)
		return;
	config->features.sd_web = 0u;
	config->features.remote_control = 0u;
	config->features.telemetry = 0u;
	set_status(config, AIUS_CONFIG_ERR_NOT_LOADED);
}

static int at_end(const AiusConfigParser *parser)
{
	return parser->pos == parser->len;
}

static uint8_t peek(const AiusConfigParser *parser)
{
	if (at_end(parser))
		return 0u;
	return parser->data[parser->pos];
}

static void skip_whitespace(AiusConfigParser *parser)
{
	while (!at_end(parser)) {
		uint8_t ch = peek(parser);
		if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
			return;
		parser->pos++;
	}
}

static int consume(AiusConfigParser *parser, uint8_t expected)
{
	if (peek(parser) != expected)
		return 0;
	parser->pos++;
	return 1;
}

static int hex_digit(uint8_t ch, uint16_t *value)
{
	if (ch >= '0' && ch <= '9') {
		*value = (uint16_t)(ch - '0');
		return 1;
	}
	if (ch >= 'a' && ch <= 'f') {
		*value = (uint16_t)(ch - 'a' + 10u);
		return 1;
	}
	if (ch >= 'A' && ch <= 'F') {
		*value = (uint16_t)(ch - 'A' + 10u);
		return 1;
	}
	return 0;
}

/* Parse and minimally decode a JSON string. The bounded key buffer is enough
 * for every accepted key. Longer strings are consumed but reported as not
 * matching any schema key. */
static int parse_string(AiusConfigParser *parser, uint8_t *dst,
	                       size_t dst_cap, size_t *length, int *overflow)
{
	size_t written = 0;
	size_t decoded_len = 0;
	int too_long = 0;

	if (!consume(parser, '"'))
		return 0;
	while (!at_end(parser)) {
		uint8_t ch = parser->data[parser->pos++];
		if (ch == '"') {
			if (length != NULL)
				*length = decoded_len;
			if (overflow != NULL)
				*overflow = too_long;
			if (dst != NULL && dst_cap != 0u && written < dst_cap)
				dst[written] = '\0';
			return 1;
		}
		if (ch < 0x20u)
			return 0;
		if (ch != '\\') {
			decoded_len++;
			if (dst != NULL) {
				if (written + 1u < dst_cap)
					dst[written++] = ch;
				else
					too_long = 1;
			}
			continue;
		}

		if (at_end(parser))
			return 0;
		ch = parser->data[parser->pos++];
		if (ch == 'u') {
			uint16_t codepoint = 0;
			for (unsigned int i = 0; i < 4u; i++) {
				uint16_t digit;
				if (at_end(parser) || !hex_digit(parser->data[parser->pos++],
				                                  &digit))
					return 0;
				codepoint = (uint16_t)((codepoint << 4) | digit);
			}
			decoded_len++;
			/* Only ASCII schema names can match. Preserve non-ASCII as a
			 * single impossible-to-match byte without widening the buffer. */
			uint8_t decoded = codepoint <= 0x7fu ? (uint8_t)codepoint : 0xffu;
			if (dst != NULL) {
				if (written + 1u < dst_cap)
					dst[written++] = decoded;
				else
					too_long = 1;
			}
			continue;
		}
		if (ch != '"' && ch != '\\' && ch != '/' && ch != 'b' &&
		    ch != 'f' && ch != 'n' && ch != 'r' && ch != 't')
			return 0;
		decoded_len++;
		if (dst != NULL) {
			uint8_t decoded = ch;
			switch (ch) {
			case 'b': decoded = '\b'; break;
			case 'f': decoded = '\f'; break;
			case 'n': decoded = '\n'; break;
			case 'r': decoded = '\r'; break;
			case 't': decoded = '\t'; break;
			default: break;
			}
			if (written + 1u < dst_cap)
				dst[written++] = decoded;
			else
				too_long = 1;
		}
	}
	return 0;
}

static int key_is(const uint8_t *key, size_t len, int overflow,
	              const char *expected)
{
	return !overflow && strlen(expected) == len &&
	       memcmp(key, expected, len) == 0;
}

static int literal_boundary(uint8_t ch)
{
	return ch == ' ' || ch == '\t' || ch == '\r' ||
	       ch == '\n' || ch == ',' || ch == '}' || ch == ']';
}

static int value_boundary(const AiusConfigParser *parser)
{
	return at_end(parser) || literal_boundary(peek(parser));
}

static int parse_literal(AiusConfigParser *parser, const char *text)
{
	size_t text_len = strlen(text);
	if (parser->pos + text_len > parser->len ||
	    memcmp(parser->data + parser->pos, text, text_len) != 0)
		return 0;
	if (parser->pos + text_len < parser->len &&
	    !literal_boundary(parser->data[parser->pos + text_len]))
		return 0;
	parser->pos += text_len;
	return 1;
}

static int parse_number(AiusConfigParser *parser)
{
	if (peek(parser) == '-') {
		parser->pos++;
		if (at_end(parser))
			return 0;
	}
	if (peek(parser) == '0') {
		parser->pos++;
		if (!at_end(parser) && parser->data[parser->pos] >= '0' &&
		    parser->data[parser->pos] <= '9')
			return 0;
	} else {
		if (peek(parser) < '1' || peek(parser) > '9')
			return 0;
		do {
			parser->pos++;
		} while (!at_end(parser) && parser->data[parser->pos] >= '0' &&
		         parser->data[parser->pos] <= '9');
	}
	if (peek(parser) == '.') {
		parser->pos++;
		if (at_end(parser) || peek(parser) < '0' || peek(parser) > '9')
			return 0;
		do {
			parser->pos++;
		} while (!at_end(parser) && parser->data[parser->pos] >= '0' &&
		         parser->data[parser->pos] <= '9');
	}
	if (peek(parser) == 'e' || peek(parser) == 'E') {
		parser->pos++;
		if (peek(parser) == '+' || peek(parser) == '-')
			parser->pos++;
		if (at_end(parser) || peek(parser) < '0' || peek(parser) > '9')
			return 0;
		do {
			parser->pos++;
		} while (!at_end(parser) && parser->data[parser->pos] >= '0' &&
		         parser->data[parser->pos] <= '9');
	}
	return value_boundary(parser);
}

static int skip_json_value(AiusConfigParser *parser, unsigned int depth);

static int skip_json_object(AiusConfigParser *parser, unsigned int depth)
{
	if (depth > AIUS_CONFIG_MAX_NESTING || !consume(parser, '{'))
		return 0;
	skip_whitespace(parser);
	if (consume(parser, '}'))
		return 1;
	for (;;) {
		if (!parse_string(parser, NULL, 0u, NULL, NULL))
			return 0;
		skip_whitespace(parser);
		if (!consume(parser, ':'))
			return 0;
		skip_whitespace(parser);
		if (!skip_json_value(parser, depth + 1u))
			return 0;
		skip_whitespace(parser);
		if (consume(parser, '}'))
			return 1;
		if (!consume(parser, ','))
			return 0;
		skip_whitespace(parser);
		if (peek(parser) == '}')
			return 0;
	}
}

static int skip_json_array(AiusConfigParser *parser, unsigned int depth)
{
	if (depth > AIUS_CONFIG_MAX_NESTING || !consume(parser, '['))
		return 0;
	skip_whitespace(parser);
	if (consume(parser, ']'))
		return 1;
	for (;;) {
		if (!skip_json_value(parser, depth + 1u))
			return 0;
		skip_whitespace(parser);
		if (consume(parser, ']'))
			return 1;
		if (!consume(parser, ','))
			return 0;
		skip_whitespace(parser);
		if (peek(parser) == ']')
			return 0;
	}
}

static int skip_json_value(AiusConfigParser *parser, unsigned int depth)
{
	switch (peek(parser)) {
	case '"':
		return parse_string(parser, NULL, 0u, NULL, NULL);
	case '{':
		return skip_json_object(parser, depth);
	case '[':
		return skip_json_array(parser, depth);
	case 'n':
		return parse_literal(parser, "null");
	case 't':
		return parse_literal(parser, "true");
	case 'f':
		return parse_literal(parser, "false");
	case '-':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		return parse_number(parser);
	default:
		return 0;
	}
}

static AiusConfigErrorCode parse_boolean(AiusConfigParser *parser,
	                                        uint8_t *value)
{
	size_t start = parser->pos;

	if (peek(parser) == 'f') {
		if (!parse_literal(parser, "false"))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		*value = 0u;
		return AIUS_CONFIG_OK;
	}
	if (peek(parser) == 't') {
		if (!parse_literal(parser, "true"))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		*value = 1u;
		return AIUS_CONFIG_OK;
	}
	if (!skip_json_value(parser, 0u) || parser->pos == start)
		return AIUS_CONFIG_ERR_MALFORMED_JSON;
	return AIUS_CONFIG_ERR_WRONG_TYPE;
}

static AiusConfigErrorCode parse_features(AiusConfigParser *parser,
	                                          AiusConfigFeatures *features)
{
	uint8_t seen = 0u;
	if (!consume(parser, '{')) {
		if (!skip_json_value(parser, 0u))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_CONFIG_ERR_WRONG_TYPE;
	}
	skip_whitespace(parser);
	if (consume(parser, '}'))
		return AIUS_CONFIG_ERR_MISSING_FEATURE;

	for (;;) {
		uint8_t key[AIUS_CONFIG_KEY_BUFFER_MAX];
		size_t key_len = 0;
		int key_overflow = 0;
		if (!parse_string(parser, key, sizeof(key), &key_len, &key_overflow))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		skip_whitespace(parser);

		unsigned int feature = 0u;
		if (key_is(key, key_len, key_overflow, "sd_web"))
			feature = 1u;
		else if (key_is(key, key_len, key_overflow, "remote_control"))
			feature = 2u;
		else if (key_is(key, key_len, key_overflow, "telemetry"))
			feature = 3u;
		else
			return AIUS_CONFIG_ERR_UNKNOWN_KEY;

		uint8_t bit = (uint8_t)(1u << (feature - 1u));
		if ((seen & bit) != 0u)
			return AIUS_CONFIG_ERR_DUPLICATE_KEY;
		if (!consume(parser, ':'))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		skip_whitespace(parser);
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
		skip_whitespace(parser);
		if (consume(parser, '}'))
			break;
		if (!consume(parser, ','))
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
		skip_whitespace(parser);
		if (peek(parser) == '}')
			return AIUS_CONFIG_ERR_MALFORMED_JSON;
	}

	if (seen != 0x07u)
		return AIUS_CONFIG_ERR_MISSING_FEATURE;
	return AIUS_CONFIG_OK;
}

AiusConfigErrorCode aius_config_parse(const uint8_t *json, size_t len,
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

	AiusConfigParser parser = {json, len, 0u};
	AiusConfigFeatures parsed = {0u, 0u, 0u};
	AiusConfigErrorCode error = AIUS_CONFIG_OK;
	skip_whitespace(&parser);
	if (!consume(&parser, '{')) {
		error = skip_json_value(&parser, 0u) ?
		             AIUS_CONFIG_ERR_WRONG_TYPE :
		             AIUS_CONFIG_ERR_MALFORMED_JSON;
		goto fail;
	}
	skip_whitespace(&parser);
	if (consume(&parser, '}')) {
		error = AIUS_CONFIG_ERR_MISSING_FEATURES;
		goto fail;
	}

	int seen_features = 0;
	for (;;) {
		uint8_t key[AIUS_CONFIG_KEY_BUFFER_MAX];
		size_t key_len = 0;
		int key_overflow = 0;
		if (!parse_string(&parser, key, sizeof(key), &key_len, &key_overflow)) {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
		skip_whitespace(&parser);
		if (!key_is(key, key_len, key_overflow, "features")) {
			error = AIUS_CONFIG_ERR_UNKNOWN_KEY;
			goto fail;
		}
		if (seen_features) {
			error = AIUS_CONFIG_ERR_DUPLICATE_KEY;
			goto fail;
		}
		seen_features = 1;
		if (!consume(&parser, ':')) {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
		skip_whitespace(&parser);
		error = parse_features(&parser, &parsed);
		if (error != AIUS_CONFIG_OK)
			goto fail;
		skip_whitespace(&parser);
		if (consume(&parser, '}'))
			break;
		if (!consume(&parser, ',')) {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
		skip_whitespace(&parser);
		if (peek(&parser) == '}') {
			error = AIUS_CONFIG_ERR_MALFORMED_JSON;
			goto fail;
		}
	}
	skip_whitespace(&parser);
	if (!at_end(&parser)) {
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
