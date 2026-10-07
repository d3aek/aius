/* Parse a DSP model body with aius_dsp_config_parse_put and print every model
 * field, one "name=value" per line (doubles as %.17g, so equal text means equal
 * double). Used by test_dsp_model_pad.py to prove a padded body decodes to the
 * same model as its input.
 *
 * Usage: dsp_model_dump <body-file>
 * Exit: 0 parsed, 1 parser refused (message on stderr), 2 usage or read error. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "aius_dsp_config.h"

static void dump_eq(const char *prefix, const AiusDspModelEq *eq)
{
	printf("%s.type=%u\n", prefix, (unsigned)eq->type);
	printf("%s.frequency_hz=%.17g\n", prefix, eq->frequency_hz);
	printf("%s.q=%.17g\n", prefix, eq->q);
	printf("%s.gain_db=%.17g\n", prefix, eq->gain_db);
}

int main(int argc, char **argv)
{
	static const char *const names[AIUS_DSP_MODEL_INPUT_CHANNELS] = {
		"fl", "fr", "fc", "lfe", "sl", "sr"};
	if (argc != 2) {
		fprintf(stderr, "usage: dsp_model_dump <body-file>\n");
		return 2;
	}
	FILE *file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 2;
	}
	/* One byte past the limit so an oversize body reaches the parser's own
	 * TOO_LARGE refusal instead of being truncated here. */
	static uint8_t body[AIUS_DSP_MODEL_MAX_JSON_BYTES + 1u];
	size_t len = fread(body, 1u, sizeof(body), file);
	fclose(file);

	AiusDspModel model;
	AiusDspConfigErrorCode error = aius_dsp_config_parse_put(body, len, &model);
	if (error != AIUS_DSP_CONFIG_OK) {
		fprintf(stderr, "refused: %s\n", aius_dsp_config_error_message(error));
		return 1;
	}

	printf("sample_rate_hz=%u\n", (unsigned)model.sample_rate_hz);
	printf("base_generation=%u\n", (unsigned)model.base_generation);
	printf("features.input_eq=%u\n", (unsigned)model.features.input_eq);
	printf("features.matrix=%u\n", (unsigned)model.features.matrix);
	printf("features.output_eq=%u\n", (unsigned)model.features.output_eq);
	printf("features.limiter=%u\n", (unsigned)model.features.limiter);
	printf("features.meters=%u\n", (unsigned)model.features.meters);
	for (unsigned c = 0; c < AIUS_DSP_MODEL_INPUT_CHANNELS; c++) {
		const AiusDspModelInputChannel *ch = &model.input_channels[c];
		printf("input.%s.gain_db=%.17g\n", names[c], ch->gain_db);
		printf("input.%s.eq_count=%u\n", names[c], (unsigned)ch->eq_count);
		for (unsigned e = 0; e < AIUS_DSP_MODEL_EQ_MAX; e++) {
			char prefix[32];
			snprintf(prefix, sizeof(prefix), "input.%s.eq%u", names[c], e);
			dump_eq(prefix, &ch->eq[e]);
		}
	}
	printf("output_eq_count=%u\n", (unsigned)model.output_eq_count);
	for (unsigned e = 0; e < AIUS_DSP_MODEL_EQ_MAX; e++) {
		char prefix[32];
		snprintf(prefix, sizeof(prefix), "output_eq%u", e);
		dump_eq(prefix, &model.output_eq[e]);
	}
	printf("matrix.center_gain_linear=%.17g\n", model.matrix.center_gain_linear);
	printf("matrix.surround_gain_linear=%.17g\n", model.matrix.surround_gain_linear);
	printf("matrix.lfe_gain_linear=%.17g\n", model.matrix.lfe_gain_linear);
	printf("matrix.lfe_crossover_hz=%.17g\n", model.matrix.lfe_crossover_hz);
	printf("matrix.headroom_linear=%.17g\n", model.matrix.headroom_linear);
	printf("matrix.surround_delay_ms=%.17g\n", model.matrix.surround_delay_ms);
	printf("matrix.clipfree=%u\n", (unsigned)model.matrix.clipfree);
	printf("matrix.layout_aware=%u\n", (unsigned)model.matrix.layout_aware);
	printf("limiter.enabled=%u\n", (unsigned)model.limiter.enabled);
	printf("limiter.threshold_dbfs=%.17g\n", model.limiter.threshold_dbfs);
	printf("limiter.attack_ms=%.17g\n", model.limiter.attack_ms);
	printf("limiter.release_ms=%.17g\n", model.limiter.release_ms);
	printf("limiter.lookahead_ms=%.17g\n", model.limiter.lookahead_ms);
	return 0;
}
