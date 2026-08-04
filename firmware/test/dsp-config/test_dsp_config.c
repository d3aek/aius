#include "aius_dsp_config.h"

#include <stdarg.h>
#include <stdio.h>
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

int main(void)
{
	test_valid_roundtrip();
	test_argument_and_size_errors();
	test_malformed();
	test_unknown_keys();
	test_missing_keys();
	test_bounds();
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
