#ifndef AIUS_DSP_CONFIG_H
#define AIUS_DSP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded, allocation-free parser for a PUT /api/dsp/config request body. The
 * accepted model mirrors build_config.py's validate_config() bounds exactly (for
 * the equivalent PUT wire shape). The parser never allocates, never recurses on
 * attacker-controlled depth, and never trusts strlen on the input. */

#define AIUS_DSP_MODEL_INPUT_CHANNELS 6u   /* fl, fr, fc, lfe, sl, sr, in that order */
#define AIUS_DSP_MODEL_EQ_MAX 2u
#define AIUS_DSP_MODEL_MAX_JSON_BYTES 12288u
#define AIUS_DSP_MODEL_ERROR_MESSAGE_MAX 40u

typedef enum {
	AIUS_DSP_CONFIG_OK = 0,
	AIUS_DSP_CONFIG_ERR_NULL_ARGUMENT,
	AIUS_DSP_CONFIG_ERR_EMPTY,
	AIUS_DSP_CONFIG_ERR_TOO_LARGE,
	AIUS_DSP_CONFIG_ERR_MALFORMED_JSON,
	AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY,
	AIUS_DSP_CONFIG_ERR_MISSING_KEY,
	AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY,
	AIUS_DSP_CONFIG_ERR_WRONG_TYPE,
	AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	AIUS_DSP_CONFIG_ERR_TOO_MANY_EQ,
	AIUS_DSP_CONFIG_ERR_INVALID_SOURCE,
	AIUS_DSP_CONFIG_ERR_INVALID_EQ_TYPE
} AiusDspConfigErrorCode;

typedef struct {
	uint8_t type;          /* 0=bypass, 1=highpass, 2=peaking -- matches build_config.py EQ_TYPES */
	double frequency_hz;
	double q;
	double gain_db;
} AiusDspModelEq;

typedef struct {
	double gain_db;
	uint8_t eq_count;
	AiusDspModelEq eq[AIUS_DSP_MODEL_EQ_MAX];
} AiusDspModelInputChannel;

typedef struct {
	uint8_t input_eq, matrix, output_eq, limiter, meters;
} AiusDspModelFeatures;

/* `lfe_crossover_hz` is the LFE fold-in crossover corner (the LR4 low-pass in
 * aius_dsp.h). Bounded [20, 500] and REQUIRED, like every other matrix key: the
 * range excludes 0 on purpose, because the DSP reads a zero corner as "fold the
 * LFE unfiltered" and that is an API capability rather than a product option.
 * Disabling the fold is lfe_gain_linear = 0. */
typedef struct {
	double center_gain_linear, surround_gain_linear, lfe_gain_linear;
	double lfe_crossover_hz, headroom_linear;
	uint8_t clipfree;
} AiusDspModelMatrix;

typedef struct {
	uint8_t enabled;
	double threshold_dbfs, attack_ms, release_ms, lookahead_ms;
} AiusDspModelLimiter;

typedef struct {
	uint32_t sample_rate_hz;
	AiusDspModelFeatures features;
	AiusDspModelInputChannel input_channels[AIUS_DSP_MODEL_INPUT_CHANNELS];
	uint8_t output_eq_count;
	AiusDspModelEq output_eq[AIUS_DSP_MODEL_EQ_MAX];
	AiusDspModelMatrix matrix;
	AiusDspModelLimiter limiter;
	uint32_t base_generation;  /* only meaningful after aius_dsp_config_parse_put */
} AiusDspModel;

/* Initialize a model to safe, all-zero defaults (unity/bypass everywhere). */
void aius_dsp_model_set_defaults(AiusDspModel *model);

/* Parse a PUT /api/dsp/config request body into *out. The wire shape is the same
 * eight top-level keys GET /api/dsp/config returns (source, config_generation,
 * sample_rate_hz, features, input_channels, output_eq, matrix, limiter) PLUS a
 * ninth top-level key "base_generation" (the optimistic-concurrency token, a
 * non-negative integer). "source" MUST be the literal string "sd".
 * "config_generation" must be present and a non-negative integer but its value
 * is not validated further. On success, out->base_generation is set from the
 * body and every other field is the validated model. On any failure, *out is
 * left at safe defaults and the specific error code is returned. */
AiusDspConfigErrorCode aius_dsp_config_parse_put(const uint8_t *json, size_t len,
	                                                AiusDspModel *out);

/* Return the stable, bounded message associated with an error code. */
const char *aius_dsp_config_error_message(AiusDspConfigErrorCode code);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_DSP_CONFIG_H */
