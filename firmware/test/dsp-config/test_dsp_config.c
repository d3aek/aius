#include "aius_dsp_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* A model is at safe defaults iff it is all-zero. */
static int is_default(const AiusDspModel *model)
{
	AiusDspModel zero;
	memset(&zero, 0, sizeof(zero));
	return memcmp(model, &zero, sizeof(zero)) == 0;
}

static void expect_error(const char *json, AiusDspConfigErrorCode expected,
	                         const char *label)
{
	AiusDspModel model;
	memset(&model, 0xa5, sizeof(model));
	AiusDspConfigErrorCode actual = aius_dsp_config_parse_put(
	    (const uint8_t *)json, strlen(json), &model);
	check(actual == expected, "%s: got error %d (%s), want %d (%s)", label,
	      (int)actual, aius_dsp_config_error_message(actual), (int)expected,
	      aius_dsp_config_error_message(expected));
	check(is_default(&model), "%s: failure did not restore safe defaults", label);
}

static AiusDspConfigErrorCode parse(const char *json, AiusDspModel *model)
{
	return aius_dsp_config_parse_put((const uint8_t *)json, strlen(json), model);
}

/* A complete, valid PUT body with non-default values in every field. */
static const char *const VALID_BODY =
	"{"
	"\"source\":\"sd\","
	"\"config_generation\":7,"
	"\"sample_rate_hz\":48000,"
	"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":false,"
	"\"limiter\":true,\"meters\":false},"
	"\"input_channels\":{"
	"\"fl\":{\"gain_db\":-3.5,\"eq\":[{\"type\":\"peaking\",\"frequency_hz\":1000,"
	"\"q\":1.5,\"gain_db\":4}]},"
	"\"fr\":{\"gain_db\":0,\"eq\":[{\"type\":\"highpass\",\"frequency_hz\":30,"
	"\"q\":0.7071067811865475,\"gain_db\":0},{\"type\":\"bypass\"}]},"
	"\"fc\":{\"gain_db\":12,\"eq\":[]},"
	"\"lfe\":{\"gain_db\":-60,\"eq\":[]},"
	"\"sl\":{\"gain_db\":0,\"eq\":[]},"
	"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
	"\"output_eq\":[{\"type\":\"peaking\",\"frequency_hz\":8000,\"q\":2,"
	"\"gain_db\":-6}],"
	"\"matrix\":{\"center_gain_linear\":0.7071067811865475,"
	"\"surround_gain_linear\":0.5,\"lfe_gain_linear\":2,"
	"\"lfe_crossover_hz\":80,\"headroom_linear\":0.9,"
	"\"clipfree\":true},"
	"\"limiter\":{\"enabled\":true,\"threshold_dbfs\":-3,\"attack_ms\":5,"
	"\"release_ms\":200,\"lookahead_ms\":2},"
	"\"base_generation\":41"
	"}";

static void test_valid_roundtrip(void)
{
	AiusDspModel m;
	memset(&m, 0xa5, sizeof(m));
	AiusDspConfigErrorCode error = parse(VALID_BODY, &m);
	check(error == AIUS_DSP_CONFIG_OK, "valid: got error %d (%s)", (int)error,
	      aius_dsp_config_error_message(error));
	if (error != AIUS_DSP_CONFIG_OK)
		return;

	check(m.sample_rate_hz == 48000u, "valid: sample_rate_hz %u", m.sample_rate_hz);
	check(m.base_generation == 41u, "valid: base_generation %u", m.base_generation);

	check(m.features.input_eq == 1u && m.features.matrix == 1u &&
	          m.features.output_eq == 0u && m.features.limiter == 1u &&
	          m.features.meters == 0u,
	      "valid: features round-trip wrong");

	/* fl: trim -3.5 + one peaking section. */
	check(m.input_channels[0].gain_db == -3.5, "valid: fl gain_db");
	check(m.input_channels[0].eq_count == 1u, "valid: fl eq_count");
	check(m.input_channels[0].eq[0].type == 2u &&
	          m.input_channels[0].eq[0].frequency_hz == 1000.0 &&
	          m.input_channels[0].eq[0].q == 1.5 &&
	          m.input_channels[0].eq[0].gain_db == 4.0,
	      "valid: fl eq[0] round-trip wrong");

	/* fr: two sections (highpass + bypass). */
	check(m.input_channels[1].eq_count == 2u, "valid: fr eq_count");
	check(m.input_channels[1].eq[0].type == 1u &&
	          m.input_channels[1].eq[0].frequency_hz == 30.0 &&
	          m.input_channels[1].eq[0].q == 0.7071067811865475,
	      "valid: fr eq[0] round-trip wrong");
	check(m.input_channels[1].eq[1].type == 0u, "valid: fr eq[1] bypass");

	check(m.input_channels[2].gain_db == 12.0, "valid: fc gain_db (max)");
	check(m.input_channels[3].gain_db == -60.0, "valid: lfe gain_db (min)");
	check(m.input_channels[4].eq_count == 0u, "valid: sl eq_count");
	check(m.input_channels[5].eq_count == 0u, "valid: sr eq_count");

	check(m.output_eq_count == 1u, "valid: output_eq_count");
	check(m.output_eq[0].type == 2u && m.output_eq[0].frequency_hz == 8000.0 &&
	          m.output_eq[0].q == 2.0 && m.output_eq[0].gain_db == -6.0,
	      "valid: output_eq[0] round-trip wrong");

	check(m.matrix.center_gain_linear == 0.7071067811865475 &&
	          m.matrix.surround_gain_linear == 0.5 &&
	          m.matrix.lfe_gain_linear == 2.0 &&
	          m.matrix.lfe_crossover_hz == 80.0 &&
	          m.matrix.headroom_linear == 0.9 && m.matrix.clipfree == 1u,
	      "valid: matrix round-trip wrong");

	check(m.limiter.enabled == 1u && m.limiter.threshold_dbfs == -3.0 &&
	          m.limiter.attack_ms == 5.0 && m.limiter.release_ms == 200.0 &&
	          m.limiter.lookahead_ms == 2.0,
	      "valid: limiter round-trip wrong");

	/* Reordered top-level keys and interior whitespace still parse. */
	AiusDspModel reordered;
	const char *ws =
		" {\n \"base_generation\" : 41 ,\n"
		" \"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},"
		" \"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		" \"output_eq\":[], \"input_channels\":{"
		"\"fl\":{\"gain_db\":0,\"eq\":[]},\"fr\":{\"gain_db\":0,\"eq\":[]},"
		"\"fc\":{\"gain_db\":0,\"eq\":[]},\"lfe\":{\"gain_db\":0,\"eq\":[]},"
		"\"sl\":{\"gain_db\":0,\"eq\":[]},\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		" \"features\":{\"input_eq\":false,\"matrix\":false,\"output_eq\":false,"
		"\"limiter\":false,\"meters\":false},"
		" \"sample_rate_hz\":96000, \"config_generation\":0, \"source\":\"sd\" }";
	AiusDspConfigErrorCode reorder_error = parse(ws, &reordered);
	check(reorder_error == AIUS_DSP_CONFIG_OK, "reordered/whitespace: error %d",
	      (int)reorder_error);
	check(reordered.sample_rate_hz == 96000u, "reordered: sample_rate_hz");
	check(reordered.matrix.headroom_linear == 1.0, "reordered: headroom==1 accepted");
}

/* Build a body identical to VALID_BODY except one field is textually replaced.
 * The macro-free approach: each mutation is written out as a full literal. */

static void test_argument_and_size_errors(void)
{
	AiusDspModel model;
	memset(&model, 0xa5, sizeof(model));
	check(aius_dsp_config_parse_put(NULL, 4u, &model) ==
	          AIUS_DSP_CONFIG_ERR_NULL_ARGUMENT,
	      "null json: wrong error");
	check(is_default(&model), "null json: not default");
	check(aius_dsp_config_parse_put((const uint8_t *)"{}", 2u, NULL) ==
	          AIUS_DSP_CONFIG_ERR_NULL_ARGUMENT,
	      "null out: wrong error");
	check(aius_dsp_config_parse_put((const uint8_t *)VALID_BODY, 0u, &model) ==
	          AIUS_DSP_CONFIG_ERR_EMPTY,
	      "empty: wrong error");
	check(is_default(&model), "empty: not default");

	/* Oversized body is rejected before parsing. Fill a valid-prefix buffer
	 * that exceeds the cap; the size gate fires first regardless of content. */
	static char oversized[AIUS_DSP_MODEL_MAX_JSON_BYTES + 32u];
	memset(oversized, ' ', sizeof(oversized));
	oversized[0] = '{';
	AiusDspConfigErrorCode big = aius_dsp_config_parse_put(
	    (const uint8_t *)oversized, sizeof(oversized), &model);
	check(big == AIUS_DSP_CONFIG_ERR_TOO_LARGE, "oversized: got %d", (int)big);
	check(is_default(&model), "oversized: not default");
}

static void test_malformed(void)
{
	expect_error("", AIUS_DSP_CONFIG_ERR_EMPTY, "empty string");
	expect_error("[]", AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "root array");
	expect_error("42", AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "root scalar");
	expect_error("{", AIUS_DSP_CONFIG_ERR_MALFORMED_JSON, "truncated root");
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0} trailing",
		AIUS_DSP_CONFIG_ERR_MALFORMED_JSON, "trailing garbage");
	expect_error("{\"source\":\"sd\",}", AIUS_DSP_CONFIG_ERR_MALFORMED_JSON,
	             "trailing comma");
	expect_error("{\"sour\\qce\":1}", AIUS_DSP_CONFIG_ERR_MALFORMED_JSON,
	             "bad escape in key");
}

/* GET /api/dsp/config carries two response-only keys (staged_generation,
 * config_pending) on top of the PUT contract. A consumer that round-trips
 * the GET document -- the native latency renderer, the bench's bypass PUT --
 * must not be refused for them, so the parser accepts and ignores both,
 * still rejects a duplicate, and never requires them. Found live 2026-09-02:
 * the renderer refused the first document that carried them. */
static void test_response_only_keys(void)
{
	static const char *const ROUND_TRIPPED_BODY =
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"staged_generation\":3,\"config_pending\":false,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":5}";
	AiusDspModel m;
	memset(&m, 0xa5, sizeof(m));
	AiusDspConfigErrorCode error = parse(ROUND_TRIPPED_BODY, &m);
	check(error == AIUS_DSP_CONFIG_OK,
	      "response-only keys: got error %d (%s)", (int)error,
	      aius_dsp_config_error_message(error));
	check(m.base_generation == 5u,
	      "response-only keys: base_generation %u", m.base_generation);

	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"staged_generation\":3,\"staged_generation\":3,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY, "duplicate staged_generation");
}

static void test_unknown_keys(void)
{
	/* Top-level unknown key. */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0,"
		"\"extra\":1}",
		AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY, "top-level unknown key");

	/* features unknown key. */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true,\"bogus\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY, "features unknown key");

	/* input channel unknown key. */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[],\"pan\":0},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY, "input channel unknown key");

	/* EQ section unknown key. */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"peaking\",\"frequency_hz\":1000,\"q\":1,"
		"\"gain_db\":0,\"slope\":1}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY, "eq section unknown key");

	/* matrix unknown key. */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false,"
		"\"extra\":0},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY, "matrix unknown key");

	/* limiter unknown key. */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0,\"knee\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY, "limiter unknown key");
}

static void test_missing_keys(void)
{
	/* Missing top-level key (base_generation absent). */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0}}",
		AIUS_DSP_CONFIG_ERR_MISSING_KEY, "missing base_generation");

	/* Missing features key (meters absent). */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_MISSING_KEY, "missing features.meters");

	/* Missing input channel (sr absent). */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_MISSING_KEY, "missing input_channels.sr");

	/* Missing input channel sub-key (eq absent on fl). */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_MISSING_KEY, "missing fl.eq");

	/* Missing matrix key (clipfree absent). */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,\"headroom_linear\":1},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_MISSING_KEY, "missing matrix.clipfree");

	/* Missing limiter key (lookahead_ms absent). */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_MISSING_KEY, "missing limiter.lookahead_ms");
}

/* A valid body with one field textually substituted. The caller supplies a
 * full replacement body; these helpers keep the bound tests readable. */
static void body_bound(const char *body, AiusDspConfigErrorCode expected,
	                       const char *label)
{
	expect_error(body, expected, label);
}

/* A valid body with the two top-level uint32 fields substituted. */
static void check_u32_fields(const char *rate, const char *base,
                             AiusDspConfigErrorCode expected, const char *label)
{
	char body[1024];
	snprintf(body, sizeof(body),
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":%s,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":%s}",
		rate, base);
	expect_error(body, expected, label);
}

/* Huge doubles must be refused before the uint32 cast (UB otherwise). */
static void test_uint32_field_bounds(void)
{
	check_u32_fields("1e300", "0", AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	                 "sample_rate_hz 1e300");
	check_u32_fields("0.01e310", "0", AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	                 "sample_rate_hz 0.01e310");
	check_u32_fields("4294967296", "0", AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	                 "sample_rate_hz 2^32");
	check_u32_fields("48000", "1e300", AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	                 "base_generation 1e300");
	check_u32_fields("48000", "0.01e310", AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	                 "base_generation 0.01e310");
	check_u32_fields("48000", "4294967296", AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	                 "base_generation 2^32");
}

static void test_bounds(void)
{
	/* input trim below -60. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":-60.1,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "input trim below min");

	/* input trim above 12. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":12.5,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "input trim above max");

	/* EQ frequency out of range (non-bypass). */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"peaking\",\"frequency_hz\":24001,\"q\":1,"
		"\"gain_db\":0}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "eq frequency too high");

	/* EQ q out of range. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"peaking\",\"frequency_hz\":1000,\"q\":0.05,"
		"\"gain_db\":0}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "eq q too low");

	/* EQ gain out of range. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"peaking\",\"frequency_hz\":1000,\"q\":1,"
		"\"gain_db\":25}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "eq gain too high");

	/* matrix gain above 4. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":4.5,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "matrix gain above 4");

	/* limiter threshold above 0. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0.5,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "limiter threshold above 0");

	/* limiter lookahead above 5. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":6},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "limiter lookahead above 5");
}

/* The LFE fold-in crossover corner, bounded [20, 500].
 *
 * 0 IS OUT OF RANGE AND THAT IS THE INTERESTING CASE. The DSP reads a zero corner
 * as "fold the LFE unfiltered" (aius_dsp.h §AiusDownmixConfig), which is a real API
 * capability and not a product option -- an unfiltered fold is the mud the
 * crossover exists to prevent, and admitting 0 here would let a PUT reach it. The
 * way to turn the fold off is lfe_gain_linear = 0, which is bit-exactly the
 * pre-fold-in output. So this bound is a deliberate narrowing of the DSP's own
 * accepted range, and this test is what pins the narrowing. */
static void test_lfe_crossover(void)
{
	/* A body with the crossover set to `corner`, otherwise minimal and valid. */
#define LFE_XOVER_BODY(corner)                                                     \
	"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"     \
	"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"      \
	"\"limiter\":true,\"meters\":true},"                                       \
	"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"                    \
	"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"       \
	"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"      \
	"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"                      \
	"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"         \
	"\"lfe_gain_linear\":0.31622776601683794,\"lfe_crossover_hz\":" corner "," \
	"\"headroom_linear\":0.5,\"clipfree\":false},"                             \
	"\"limiter\":{\"enabled\":true,\"threshold_dbfs\":-1,\"attack_ms\":0.5,"   \
	"\"release_ms\":100,\"lookahead_ms\":2},\"base_generation\":0}"

	body_bound(LFE_XOVER_BODY("0"), AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	           "lfe_crossover_hz 0 (unfiltered fold is not configurable)");
	body_bound(LFE_XOVER_BODY("19.99"), AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	           "lfe_crossover_hz below 20");
	body_bound(LFE_XOVER_BODY("500.01"), AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	           "lfe_crossover_hz above 500");
	body_bound(LFE_XOVER_BODY("-120"), AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	           "lfe_crossover_hz negative");
	body_bound(LFE_XOVER_BODY("\"120\""), AIUS_DSP_CONFIG_ERR_WRONG_TYPE,
	           "lfe_crossover_hz string");

	/* Both endpoints and the shipped default parse, and the value round-trips. */
	static const char *const accepted[] = {"20", "80", "120", "500"};
	static const double expected[] = {20.0, 80.0, 120.0, 500.0};
	static const char *const bodies[] = {
		LFE_XOVER_BODY("20"), LFE_XOVER_BODY("80"),
		LFE_XOVER_BODY("120"), LFE_XOVER_BODY("500"),
	};
	for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
		AiusDspModel m;
		memset(&m, 0xa5, sizeof(m));
		AiusDspConfigErrorCode error = parse(bodies[i], &m);
		check(error == AIUS_DSP_CONFIG_OK, "lfe_crossover_hz %s: error %d (%s)",
		      accepted[i], (int)error, aius_dsp_config_error_message(error));
		if (error == AIUS_DSP_CONFIG_OK)
			check(m.matrix.lfe_crossover_hz == expected[i],
			      "lfe_crossover_hz %s round-tripped as %.17g", accepted[i],
			      m.matrix.lfe_crossover_hz);
	}
#undef LFE_XOVER_BODY
}

/* matrix.layout_aware: THE ONE OPTIONAL MATRIX KEY.
 *
 * The flag decides whether headroom_linear is the SUMMING-layout pad (true: mono
 * and stereo FL|FR pass through at unity) or the uniform pad on every layout
 * (false: the pre-2026-07-28 behaviour). Everything below drives REAL JSON
 * BODIES through aius_dsp_config_parse_put, never a hand-built struct, because
 * the two defects this pin exists for are both parse-level and invisible to a
 * struct test:
 *
 *   - THE REQUIRED-KEY INVERSION. The required-key check is a MASK,
 *     (seen & 0x3F) != 0x3F. Written as the naive `seen != 0x3F` it would reject
 *     every body that CARRIES the field -- the editor's own -- and written as
 *     `seen != 0x7F` it would reject every body that does not, which is every
 *     config written before the field existed. The first two cases here fail on
 *     one spelling each.
 *   - THE ABSENT-READS-ON DEFAULT. aius_dsp_model_set_defaults() is a memset to
 *     zero, so the default cannot live there; it lives in parse_matrix's local
 *     initializer. If it were dropped, an old config would parse to
 *     layout_aware == 0 and the fix would ship as a silent no-op on exactly the
 *     installed base it was written for. */
static void test_layout_aware(void)
{
	/* A body with the matrix object's tail spelled by the caller, so "absent",
	 * "true" and "false" are the same body apart from the key under test. */
#define LAYOUT_BODY(tail)                                                          \
	"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"     \
	"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"      \
	"\"limiter\":true,\"meters\":true},"                                       \
	"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"                    \
	"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"       \
	"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"      \
	"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"                      \
	"\"matrix\":{\"center_gain_linear\":0.7071067811865475,"                   \
	"\"surround_gain_linear\":0.7071067811865475,"                             \
	"\"lfe_gain_linear\":0.31622776601683794,\"lfe_crossover_hz\":120,"        \
	"\"headroom_linear\":0.5,\"clipfree\":false" tail "},"                     \
	"\"limiter\":{\"enabled\":true,\"threshold_dbfs\":-1,\"attack_ms\":0.5,"   \
	"\"release_ms\":100,\"lookahead_ms\":2},\"base_generation\":0}"

	static const struct {
		const char *body;
		uint8_t expect;
		const char *label;
	} accepted[] = {
		{LAYOUT_BODY(""), 1u, "absent reads ON (a pre-2026-07-28 config)"},
		{LAYOUT_BODY(",\"layout_aware\":true"), 1u, "present true"},
		{LAYOUT_BODY(",\"layout_aware\":false"), 0u, "present false"},
	};
	for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++) {
		AiusDspModel m;
		memset(&m, 0xa5, sizeof(m));
		AiusDspConfigErrorCode error = parse(accepted[i].body, &m);
		check(error == AIUS_DSP_CONFIG_OK, "layout_aware %s: error %d (%s)",
		      accepted[i].label, (int)error,
		      aius_dsp_config_error_message(error));
		if (error != AIUS_DSP_CONFIG_OK)
			continue;
		check(m.matrix.layout_aware == accepted[i].expect,
		      "layout_aware %s: parsed %u, want %u", accepted[i].label,
		      (unsigned)m.matrix.layout_aware, (unsigned)accepted[i].expect);
		/* And the rest of the matrix is unharmed by the optional key's arm. */
		check(m.matrix.headroom_linear == 0.5 &&
		          m.matrix.lfe_crossover_hz == 120.0 &&
		          m.matrix.clipfree == 0u,
		      "layout_aware %s: the rest of the matrix moved",
		      accepted[i].label);
	}

	/* It is a BOOLEAN, and a duplicate is still a duplicate: optional does not
	 * mean unvalidated. */
	body_bound(LAYOUT_BODY(",\"layout_aware\":1"),
	           AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "layout_aware number");
	body_bound(LAYOUT_BODY(",\"layout_aware\":\"true\""),
	           AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "layout_aware string");
	body_bound(LAYOUT_BODY(",\"layout_aware\":true,\"layout_aware\":false"),
	           AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY, "layout_aware duplicated");

	/* The six legacy keys stay REQUIRED: carrying the new one does not excuse a
	 * missing old one. (clipfree is dropped here, layout_aware supplied.) */
	expect_error(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"layout_aware\":true},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_MISSING_KEY,
		"matrix.clipfree still required when layout_aware is present");
#undef LAYOUT_BODY
}

/* matrix.surround_delay_ms: the SECOND optional matrix key (bit 7), the
 * pre-fold surround decorrelation delay. Absent reads 0.0 -- the undelayed
 * fold every config written before the field existed asks for -- and unlike
 * layout_aware the zero default survives a memset, so only the parse-level
 * properties need pinning: bounds, types, duplicates, and the required-mask
 * exclusion. */
static void test_surround_delay_ms(void)
{
#define SDELAY_BODY(tail)                                                          \
	"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"     \
	"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"      \
	"\"limiter\":true,\"meters\":true},"                                       \
	"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"                    \
	"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"       \
	"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"      \
	"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"                      \
	"\"matrix\":{\"center_gain_linear\":0.7071067811865475,"                   \
	"\"surround_gain_linear\":0.7071067811865475,"                             \
	"\"lfe_gain_linear\":0.31622776601683794,\"lfe_crossover_hz\":120,"        \
	"\"headroom_linear\":0.5,\"clipfree\":false" tail "},"                     \
	"\"limiter\":{\"enabled\":true,\"threshold_dbfs\":-1,\"attack_ms\":0.5,"   \
	"\"release_ms\":100,\"lookahead_ms\":2},\"base_generation\":0}"

	static const struct {
		const char *body;
		double expect;
		const char *label;
	} accepted[] = {
		{SDELAY_BODY(""), 0.0, "absent reads 0 (the undelayed fold)"},
		{SDELAY_BODY(",\"surround_delay_ms\":0"), 0.0, "present zero"},
		{SDELAY_BODY(",\"surround_delay_ms\":8"), 8.0, "present mid-range"},
		{SDELAY_BODY(",\"surround_delay_ms\":12"), 12.0, "present at the cap"},
	};
	for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++) {
		AiusDspModel m;
		memset(&m, 0xa5, sizeof(m));
		AiusDspConfigErrorCode error = parse(accepted[i].body, &m);
		check(error == AIUS_DSP_CONFIG_OK,
		      "surround_delay_ms %s: error %d (%s)", accepted[i].label,
		      (int)error, aius_dsp_config_error_message(error));
		if (error != AIUS_DSP_CONFIG_OK)
			continue;
		check(m.matrix.surround_delay_ms == accepted[i].expect,
		      "surround_delay_ms %s: parsed %.17g, want %.17g",
		      accepted[i].label, m.matrix.surround_delay_ms,
		      accepted[i].expect);
		check(m.matrix.headroom_linear == 0.5 &&
		          m.matrix.lfe_crossover_hz == 120.0 &&
		          m.matrix.layout_aware == 1u,
		      "surround_delay_ms %s: the rest of the matrix moved",
		      accepted[i].label);
	}

	/* Optional does not mean unvalidated: bounds, type and duplicates all
	 * still refuse. */
	body_bound(SDELAY_BODY(",\"surround_delay_ms\":12.5"),
	           AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "surround_delay_ms over cap");
	body_bound(SDELAY_BODY(",\"surround_delay_ms\":-1"),
	           AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "surround_delay_ms negative");
	body_bound(SDELAY_BODY(",\"surround_delay_ms\":\"8\""),
	           AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "surround_delay_ms string");
	body_bound(SDELAY_BODY(
	               ",\"surround_delay_ms\":8,\"surround_delay_ms\":4"),
	           AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY,
	           "surround_delay_ms duplicated");
#undef SDELAY_BODY
}

static void test_headroom(void)
{
	/* headroom_linear == 0 rejected (must be > 0). */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":0,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "headroom == 0");

	/* headroom_linear > 1 rejected. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1.5,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "headroom > 1");
	/* headroom_linear == 1 is accepted (exercised in test_valid_roundtrip). */
}

static void test_sample_rate(void)
{
	/* Non-integer sample rate rejected. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000.5,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "sample_rate non-integer");

	/* Zero sample rate rejected. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":0,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "sample_rate zero");

	/* Negative sample rate rejected. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":-48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "sample_rate negative");
}

static void test_wrong_types(void)
{
	/* sample_rate_hz as a string. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":\"48000\","
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "sample_rate string");

	/* features.input_eq as a number (bool expected). */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":1,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "features.input_eq number");

	/* features as a boolean (object expected). */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":true,"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "features boolean");

	/* EQ type as a number (string expected). */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":2,\"frequency_hz\":1000,\"q\":1,\"gain_db\":0}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "eq type number");
}

static void test_eq_type(void)
{
	/* Unsupported EQ type string. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"lowshelf\",\"frequency_hz\":1000,\"q\":1,"
		"\"gain_db\":0}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_INVALID_EQ_TYPE, "unsupported eq type");

	/* Missing EQ type. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"frequency_hz\":1000,\"q\":1,\"gain_db\":0}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_INVALID_EQ_TYPE, "missing eq type");

	/* lowpass parses to type 3, like highpass a gain-less filter. */
	AiusDspModel m;
	memset(&m, 0xa5, sizeof(m));
	AiusDspConfigErrorCode error = parse(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"lowpass\",\"frequency_hz\":12000,\"q\":0.7071,"
		"\"gain_db\":0}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		&m);
	check(error == AIUS_DSP_CONFIG_OK, "lowpass: got error %d (%s)", (int)error,
	      aius_dsp_config_error_message(error));
	check(error != AIUS_DSP_CONFIG_OK ||
	          (m.output_eq_count == 1u && m.output_eq[0].type == 3u &&
	           m.output_eq[0].frequency_hz == 12000.0),
	      "lowpass: output_eq[0] round-trip wrong");
}

static void test_too_many_eq(void)
{
	/* Three sections in output_eq. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},"
		"\"output_eq\":[{\"type\":\"bypass\"},{\"type\":\"bypass\"},"
		"{\"type\":\"bypass\"}],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_TOO_MANY_EQ, "output_eq three sections");

	/* Three sections in an input channel's eq. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[{\"type\":\"bypass\"},"
		"{\"type\":\"bypass\"},{\"type\":\"bypass\"}]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_TOO_MANY_EQ, "input channel three sections");
}

static void test_source(void)
{
	/* source == "default" ACCEPTED, and this row asserted the opposite until
	 * 2026-07-26. `GET /api/dsp/config` serves "default" on a board with no
	 * SD-hosted config, and `source` is a required key, so rejecting it made the
	 * natural GET-then-PUT round-trip fail 400 on an untouched board -- which is
	 * how B6 gate 5 failed on hardware. The value is accepted and ignored: a
	 * successful PUT always writes /aius/dsp.json, so the resulting source is "sd"
	 * whatever the body claimed. */
	static const char *const DEFAULT_SOURCE_BODY =
		"{\"source\":\"default\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}";

	/* A direct parse, not body_bound(): that helper also asserts the model was
	 * reset to safe defaults, which is a FAILURE-path property. Using it for a
	 * success case reports "failure did not restore safe defaults" and would
	 * tempt the wrong fix. */
	AiusDspModel default_source_model;
	memset(&default_source_model, 0xa5, sizeof(default_source_model));
	AiusDspConfigErrorCode default_source_error =
		parse(DEFAULT_SOURCE_BODY, &default_source_model);
	check(default_source_error == AIUS_DSP_CONFIG_OK,
	      "source default: got error %d (%s), want OK",
	      (int)default_source_error,
	      aius_dsp_config_error_message(default_source_error));
	/* And the body really was applied rather than merely not rejected -- if the
	 * parse bailed early the model would still hold the 0xa5 fill. */
	check(default_source_model.sample_rate_hz == 48000u,
	      "source default: body not applied (sample_rate_hz %lu)",
	      (unsigned long)default_source_model.sample_rate_hz);

	/* arbitrary source string still rejected -- widening the enum to the two
	 * values GET can serve is not the same as skipping the field, so a typo or a
	 * wrong-shaped body is still caught. This row is what keeps the fix a
	 * widening rather than a removal. */
	body_bound(
		"{\"source\":\"usb\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_INVALID_SOURCE, "source arbitrary");

	/* source as a number (string expected). */
	body_bound(
		"{\"source\":5,\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "source number");
}

static void test_base_generation(void)
{
	/* base_generation negative. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":-1}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "base_generation negative");

	/* base_generation fractional. */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":1.5}",
		AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "base_generation fractional");

	/* base_generation wrong type (string). */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":\"1\"}",
		AIUS_DSP_CONFIG_ERR_WRONG_TYPE, "base_generation string");
}

static void test_duplicate_keys(void)
{
	/* Duplicate top-level key. */
	body_bound(
		"{\"source\":\"sd\",\"source\":\"sd\",\"config_generation\":0,"
		"\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY, "duplicate top-level key");

	/* Duplicate nested key (matrix.clipfree twice). */
	body_bound(
		"{\"source\":\"sd\",\"config_generation\":0,\"sample_rate_hz\":48000,"
		"\"features\":{\"input_eq\":true,\"matrix\":true,\"output_eq\":true,"
		"\"limiter\":true,\"meters\":true},"
		"\"input_channels\":{\"fl\":{\"gain_db\":0,\"eq\":[]},"
		"\"fr\":{\"gain_db\":0,\"eq\":[]},\"fc\":{\"gain_db\":0,\"eq\":[]},"
		"\"lfe\":{\"gain_db\":0,\"eq\":[]},\"sl\":{\"gain_db\":0,\"eq\":[]},"
		"\"sr\":{\"gain_db\":0,\"eq\":[]}},\"output_eq\":[],"
		"\"matrix\":{\"center_gain_linear\":0,\"surround_gain_linear\":0,"
		"\"lfe_gain_linear\":0,\"lfe_crossover_hz\":120,"
		"\"headroom_linear\":1,\"clipfree\":false,"
		"\"clipfree\":true},"
		"\"limiter\":{\"enabled\":false,\"threshold_dbfs\":0,\"attack_ms\":1,"
		"\"release_ms\":100,\"lookahead_ms\":0},\"base_generation\":0}",
		AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY, "duplicate matrix key");
}

/* ---- Number decode: correctly rounded, round-trip stable ------------------
 * The decoder is static, so drive it through matrix.center_gain_linear
 * (bounds [0, 4]) with VALID_BODY's own text swapped for the number under test.
 * The oracle is the host strtod(), which is correctly rounded. */
static AiusDspConfigErrorCode parse_center(const char *number, double *out)
{
	static const char KEY[] = "\"center_gain_linear\":0.7071067811865475,";
	static char body[2048];
	const char *at = strstr(VALID_BODY, KEY);
	if (at == NULL)
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	size_t head = (size_t)(at - VALID_BODY) + strlen("\"center_gain_linear\":");
	int n = snprintf(body, sizeof(body), "%.*s%s%s", (int)head, VALID_BODY,
	                 number, at + strlen(KEY) - 1);
	if (n <= 0 || (size_t)n >= sizeof(body))
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	AiusDspModel m;
	memset(&m, 0xa5, sizeof(m));
	AiusDspConfigErrorCode error = parse(body, &m);
	if (error == AIUS_DSP_CONFIG_OK)
		*out = m.matrix.center_gain_linear;
	return error;
}

static int same_bits(double a, double b)
{
	return memcmp(&a, &b, sizeof(a)) == 0;
}

static void expect_decodes_like_strtod(const char *text)
{
	double got = 0.0;
	AiusDspConfigErrorCode error = parse_center(text, &got);
	double want = strtod(text, NULL);
	check(error == AIUS_DSP_CONFIG_OK, "decode '%s': error %d", text, (int)error);
	if (error == AIUS_DSP_CONFIG_OK)
		check(same_bits(got, want), "decode '%s': got %.17g want %.17g", text,
		      got, want);
}

static uint64_t lcg_state = 0x9e3779b97f4a7c15ull;
static uint64_t lcg_next(void)
{
	lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
	return lcg_state ^ (lcg_state >> 29);
}

static void test_number_decode_vectors(void)
{
	static const char *const TEXTS[] = {
		/* The register's live and typed drift cases. */
		"0.3162277660168385", "0.31622776601683855", "0.03", "0.1", "0.3",
		"0.30000000000000004", "0.7071067811865475", "0.7071067811865476",
		/* 16- and 17-digit mantissas. */
		"1.234567890123456", "1.2345678901234567", "3.1415926535897931",
		"0.12345678901234567", "0.9999999999999999", "0.99999999999999989",
		"0.99999999999999994", "2.7182818284590452", "9007199254740993e-16",
		"9007199254740992e-16", "12345678901234567e-16", "1234567890123456e-15",
		/* Exponent forms: e, E, signed. */
		"3.162277660168385e-1", "3.162277660168385E-1", "3162277660168385e-16",
		"3162277660168385E-16", "0.3162277660168385e0", "0.3162277660168385e+0",
		"31.62277660168385e-2", "0.03e+1", "3E-2", "3e-2", "1e-1", "1E-1",
		"0.5e+0", "5e-1", "5E-1", "0.0005e3", "400e-2", "4e0", "0.04E2",
		/* Exact ties (round to even) and either side of them. */
		"1.00000000000000011102230246251565404236316680908203125",
		"1.00000000000000011102230246251565404236316680908203124",
		"1.00000000000000011102230246251565404236316680908203126",
		"0.99999999999999994448884876874217297881841659545898437",
		"0.999999999999999944488848768742172978818416595458984375",
		"0.99999999999999994448884876874217297881841659545898438",
		/* Float boundaries: subnormals, smallest normals, the upper bound. */
		"5e-324", "4.9406564584124654e-324", "2.4703282292062327e-324",
		"2.4703282292062328e-324", "2.4703282292062329e-324", "1e-323",
		"2.2250738585072009e-308", "2.2250738585072011e-308",
		"2.2250738585072014e-308", "2.2250738585072019e-308",
		"1e-320", "1e-400", "0e0", "0", "0.0", "-0", "-0.0",
		"4", "4.0", "3.9999999999999996", "4.0000000000000001",
		"4.0000000000000004", "0.4e1", "40e-1",
		/* Long mantissas (kept digits beyond 19, sticky). */
		"0.1234567890123456789012345678901234567890123456789012345",
		"3.1415926535897932384626433832795028841971693993751058209",
		"0.00000000000000000000000000000000000000000001234567890123",
		"1234567890123456789012345678901234567890123456789e-49",
	};
	for (size_t i = 0; i < sizeof(TEXTS) / sizeof(TEXTS[0]); i++)
		expect_decodes_like_strtod(TEXTS[i]);

	/* Just above the bound is still refused, the bound itself accepted. */
	double v = 0.0;
	check(parse_center("4.000000000000001", &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	      "decode: 4.000000000000001 must be out of range");
	check(parse_center("4.0000000000000004", &v) == AIUS_DSP_CONFIG_OK && v == 4.0,
	      "decode: 4.0000000000000004 rounds to 4");
}

static void test_number_decode_roundtrip(void)
{
	/* Every float the live model carries (fixtures/dsp_config_live_*.json) and
	 * the VALID_BODY set, through the serialiser's %.17g and back. */
	static const double LIVE[] = {
		0.35000000000000031, 0.85500000000000065, 0.3162277660168385,
		0.31622776601683855, 0.7071067811865475, 0.5, 2.0, 0.9, 1.5, 3.5, 0.03,
		0.1, 1.0, 0.0, 3.9999999999999996,
	};
	char text[64];
	for (size_t i = 0; i < sizeof(LIVE) / sizeof(LIVE[0]); i++) {
		snprintf(text, sizeof(text), "%.17g", LIVE[i]);
		double got = -1.0;
		check(parse_center(text, &got) == AIUS_DSP_CONFIG_OK &&
		          same_bits(got, LIVE[i]),
		      "roundtrip %%.17g '%s' drifted (got %.17g)", text, got);
	}

	/* A spread: random doubles over many binades, %.17g must be bit-stable and
	 * the shorter %.15g / %.16g forms must match strtod. */
	for (int i = 0; i < 20000; i++) {
		uint64_t r = lcg_next();
		double base = (double)(r >> 11) / 9007199254740992.0; /* [0,1) */
		int shift = (int)(lcg_next() % 60u);
		double value = base * 4.0;
		for (int s = 0; s < shift % 40; s++)
			value *= 0.5;
		if (i % 7 == 0)
			value = base * 4.0;
		if (value > 4.0)
			value = 4.0;
		snprintf(text, sizeof(text), "%.17g", value);
		double got = -1.0;
		AiusDspConfigErrorCode error = parse_center(text, &got);
		check(error == AIUS_DSP_CONFIG_OK && same_bits(got, value),
		      "roundtrip '%s': got %.17g", text, got);
		snprintf(text, sizeof(text), "%.16g", value);
		expect_decodes_like_strtod(text);
		snprintf(text, sizeof(text), "%.15e", value);
		expect_decodes_like_strtod(text);
	}

	/* Random digit strings (1..40 digits) at decimal exponents down to the
	 * subnormals, against strtod. */
	for (int i = 0; i < 20000; i++) {
		char digits[48];
		int count = 1 + (int)(lcg_next() % 40u);
		digits[0] = (char)('1' + lcg_next() % 9u);
		for (int d = 1; d < count; d++)
			digits[d] = (char)('0' + lcg_next() % 10u);
		digits[count] = '\0';
		int exponent = -1 - (int)(lcg_next() % 344u);
		snprintf(text, sizeof(text), "%.1s.%se%d", digits,
		         count > 1 ? digits + 1 : "0", exponent);
		expect_decodes_like_strtod(text);
	}
}

static void test_number_decode_refusals(void)
{
	double v = 0.0;
	/* Out of range / non-finite stay refused. */
	check(parse_center("1e999", &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "1e999");
	check(parse_center("1.8e308", &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "1.8e308");
	check(parse_center("-1", &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "-1");
	check(parse_center("0e999", &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "0e999");
	check(parse_center("0.0E+309", &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "0E+309");
	check(parse_center("5", &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE, "5");
	check(parse_center("1e-999", &v) == AIUS_DSP_CONFIG_OK && v == 0.0,
	      "1e-999 underflows to 0");
	check(parse_center("0e308", &v) == AIUS_DSP_CONFIG_OK && v == 0.0, "0e308");
	/* Length cap: 63 bytes accepted, 64 refused. */
	char text[96];
	memset(text, '0', sizeof(text));
	text[0] = '0';
	text[1] = '.';
	text[62] = '1';
	text[63] = '\0';
	check(parse_center(text, &v) == AIUS_DSP_CONFIG_OK, "63-byte number");
	text[63] = '1';
	text[64] = '\0';
	check(parse_center(text, &v) == AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE,
	      "64-byte number must be out of range");
	/* Grammar and type refusals. */
	static const char *const BAD[] = { "01", "1.", ".5", "+1", "1e", "1e+",
		"-", "--1", "1.5.2", "0x10", "NaN", "Infinity", "1_0", "\"1\"",
		"true", "null", "[1]" };
	for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++)
		check(parse_center(BAD[i], &v) != AIUS_DSP_CONFIG_OK,
		      "'%s' must be refused", BAD[i]);
}

int main(void)
{
	test_number_decode_vectors();
	test_number_decode_roundtrip();
	test_number_decode_refusals();
	test_valid_roundtrip();
	test_argument_and_size_errors();
	test_malformed();
	test_unknown_keys();
	test_response_only_keys();
	test_missing_keys();
	test_bounds();
	test_uint32_field_bounds();
	test_lfe_crossover();
	test_layout_aware();
	test_surround_delay_ms();
	test_headroom();
	test_sample_rate();
	test_wrong_types();
	test_eq_type();
	test_too_many_eq();
	test_source();
	test_base_generation();
	test_duplicate_keys();

	if (failures != 0u) {
		fprintf(stderr, "RESULT: %u check(s) failed\n", failures);
		return 1;
	}
	puts("ok aius dsp config (bounded PUT validator, no heap)");
	return 0;
}
