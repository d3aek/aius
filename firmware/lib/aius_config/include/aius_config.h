#ifndef AIUS_CONFIG_H
#define AIUS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The public configuration is deliberately small enough to parse in one
 * bounded call. The parser never allocates and never retains the input. */
#define AIUS_CONFIG_MAX_JSON_BYTES 256u
#define AIUS_CONFIG_ERROR_MESSAGE_MAX 32u

typedef enum {
	AIUS_CONFIG_OK = 0,
	AIUS_CONFIG_ERR_NULL_ARGUMENT,
	AIUS_CONFIG_ERR_EMPTY,
	AIUS_CONFIG_ERR_TOO_LARGE,
	AIUS_CONFIG_ERR_MALFORMED_JSON,
	AIUS_CONFIG_ERR_UNKNOWN_KEY,
	AIUS_CONFIG_ERR_DUPLICATE_KEY,
	AIUS_CONFIG_ERR_MISSING_FEATURES,
	AIUS_CONFIG_ERR_MISSING_FEATURE,
	AIUS_CONFIG_ERR_WRONG_TYPE,
	AIUS_CONFIG_ERR_NOT_LOADED
} AiusConfigErrorCode;

typedef struct {
	uint8_t sd_web;
	uint8_t remote_control;
	uint8_t telemetry;
} AiusConfigFeatures;

typedef struct {
	AiusConfigFeatures features;
	uint8_t valid;
	AiusConfigErrorCode error_code;
	char error_message[AIUS_CONFIG_ERROR_MESSAGE_MAX];
} AiusConfig;

/* Initialize a result to safe, disabled defaults. */
void aius_config_set_defaults(AiusConfig *config);

/* Parse one complete JSON document into *out. Invalid input leaves safe
 * defaults in *out and records a bounded error code/message. */
AiusConfigErrorCode aius_config_parse(const uint8_t *json, size_t len,
	                                      AiusConfig *out);

/* Phase 4h NS6: serialise `features` as /aius/config.json in the one form
 * aius_config_parse accepts (parsing the output gives `features` back), with
 * a trailing newline and a NUL after it. Returns the length written (without
 * the NUL), or 0 for a NULL argument or when it does not fit `capacity`. */
size_t aius_config_write(const AiusConfigFeatures *features, uint8_t *body,
                         size_t capacity);

/* Return the stable, bounded message associated with an error code. */
const char *aius_config_error_message(AiusConfigErrorCode code);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_CONFIG_H */
