/* Shared production-model resolver tests.
 *
 * Firmware and the native latency renderer both consume
 * aius_dsp_runtime_config_apply(); these tests pin every model field, feature
 * bypass, accepted-rate dependency and state reset at that public seam. */

#include "aius_dsp_runtime_config.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		return 1; \
	} \
} while (0)

static AiusDspModel production_model(void)
{
	AiusDspModel model;
	size_t channel;

	memset(&model, 0, sizeof(model));
	model.sample_rate_hz = 48000u;
	model.features.input_eq = 1u;
	model.features.matrix = 1u;
	model.features.output_eq = 1u;
	model.features.limiter = 1u;
	model.features.meters = 1u;
	for (channel = 0; channel < AIUS_DSP_MODEL_INPUT_CHANNELS; channel++)
		model.input_channels[channel].gain_db = 0.0;
	model.matrix.center_gain_linear = 0.70710678118654746;
	model.matrix.surround_gain_linear = 0.70710678118654746;
	model.matrix.lfe_gain_linear = 0.31622776601683794;
	model.matrix.lfe_crossover_hz = 120.0;
	model.matrix.headroom_linear = 0.5;
	model.matrix.layout_aware = 1u;
	model.limiter.enabled = 1u;
	model.limiter.threshold_dbfs = -1.0;
	model.limiter.attack_ms = 0.5;
	model.limiter.release_ms = 100.0;
	model.limiter.lookahead_ms = 2.0;
	return model;
}

static int biquad_equal(AiusBiquad left, AiusBiquad right)
{
	return left.b0 == right.b0 && left.b1 == right.b1 &&
		left.b2 == right.b2 && left.a1 == right.a1 &&
		left.a2 == right.a2;
}

static int all_zero(const void *value, size_t size)
{
	const uint8_t *bytes = (const uint8_t *)value;
	size_t i;

	for (i = 0; i < size; i++) {
		if (bytes[i] != 0u)
			return 0;
	}
	return 1;
}

static int test_invalid_inputs_fail_closed(void)
{
	AiusDspRuntimeConfig runtime;
	AiusDspModel model = production_model();

	CHECK(aius_dsp_runtime_config_apply(NULL, &model, 48000u, NULL) ==
	      AIUS_INVALID_ARG);
	memset(&runtime, 0xa5, sizeof(runtime));
	CHECK(aius_dsp_runtime_config_apply(&runtime, NULL, 48000u, NULL) ==
	      AIUS_INVALID_ARG);
	CHECK(all_zero(&runtime, sizeof(runtime)));
	memset(&runtime, 0xa5, sizeof(runtime));
	CHECK(aius_dsp_runtime_config_apply(&runtime, &model, 0u, NULL) ==
	      AIUS_INVALID_ARG);
	CHECK(all_zero(&runtime, sizeof(runtime)));

	model.output_eq_count = AIUS_DSP_RUNTIME_OUTPUT_EQ_MAX + 1u;
	memset(&runtime, 0xa5, sizeof(runtime));
	CHECK(aius_dsp_runtime_config_apply(&runtime, &model, 48000u, NULL) ==
	      AIUS_INVALID_ARG);
	CHECK(all_zero(&runtime, sizeof(runtime)));
	model = production_model();
	model.input_channels[5].eq_count = AIUS_DSP_INPUT_EQ_MAX + 1u;
	memset(&runtime, 0xa5, sizeof(runtime));
	CHECK(aius_dsp_runtime_config_apply(&runtime, &model, 48000u, NULL) ==
	      AIUS_INVALID_ARG);
	CHECK(all_zero(&runtime, sizeof(runtime)));
	return 0;
}

static int test_every_model_field_resolves_and_state_resets(void)
{
	AiusDspRuntimeConfig runtime;
	AiusDspModel model = production_model();
	AiusMeter meter;
	AiusBiquad expected;
	size_t channel;

	model.output_eq_count = 2u;
	model.output_eq[0] = (AiusDspModelEq){1u, 90.0, 0.707, 0.0};
	/* type 3 = lowpass: like highpass it ignores gain_db (4.5 here is
	 * validated-but-unused), proving the resolver's lowpass branch rather
	 * than the peaking fall-through. Peaking stays covered by the input
	 * channel section below. */
	model.output_eq[1] = (AiusDspModelEq){3u, 2400.0, 1.2, 4.5};
	model.input_channels[0].gain_db = -3.0;
	model.input_channels[0].eq_count = 2u;
	model.input_channels[0].eq[0] =
		(AiusDspModelEq){0u, 1000.0, 1.0, 0.0};
	model.input_channels[0].eq[1] =
		(AiusDspModelEq){2u, 1750.0, 0.8, -2.5};
	model.matrix.center_gain_linear = 0.61;
	model.matrix.surround_gain_linear = 0.42;
	model.matrix.lfe_gain_linear = 0.23;
	model.matrix.lfe_crossover_hz = 87.0;
	model.matrix.headroom_linear = 0.37;
	model.matrix.clipfree = 0u;
	model.matrix.layout_aware = 0u;
	/* 5 ms: 480 samples at this test's 96 kHz rate, inside the 576-sample
	 * ring. (12 ms is a 48 kHz budget; a higher rate shrinks the usable
	 * maximum, and an over-budget request resolves fail-open to 0.) */
	model.matrix.surround_delay_ms = 5.0;
	model.limiter.enabled = 0u;
	model.limiter.threshold_dbfs = -3.5;
	model.limiter.attack_ms = 2.25;
	model.limiter.release_ms = 321.0;
	model.limiter.lookahead_ms = 4.0;

	memset(&runtime, 0xa5, sizeof(runtime));
	memset(&meter, 0xa5, sizeof(meter));
	CHECK(aius_dsp_runtime_config_apply(
		&runtime, &model, 96000u, &meter) == AIUS_OK);
	CHECK(runtime.sample_rate_hz == 96000u);
	CHECK(runtime.output_eq_count == 2u);
	expected = aius_biquad_highpass(96000.0, 90.0, 0.707);
	CHECK(biquad_equal(runtime.output_eq[0], expected));
	expected = aius_biquad_lowpass(96000.0, 2400.0, 1.2);
	CHECK(biquad_equal(runtime.output_eq[1], expected));
	CHECK(all_zero(runtime.output_eq_state,
	               sizeof(runtime.output_eq_state)));

	CHECK(runtime.downmix.center == 0.61);
	CHECK(runtime.downmix.surround == 0.42);
	CHECK(runtime.downmix.lfe == 0.23);
	CHECK(runtime.downmix.headroom == 0.37);
	CHECK(runtime.downmix.lfe_crossover_hz == 87.0);
	CHECK(runtime.downmix.lfe_fold == &runtime.lfe_fold);
	CHECK(runtime.downmix.layout_aware == 0);
	CHECK(runtime.lfe_fold.designed_fs == 96000.0);
	CHECK(runtime.lfe_fold.designed_hz == 87.0);
	CHECK(all_zero(runtime.lfe_fold.state,
	               sizeof(runtime.lfe_fold.state)));
	CHECK(runtime.downmix.surround_delay_ms == 5.0);
	CHECK(runtime.downmix.surround_delay == &runtime.surround_delay);
	CHECK(runtime.surround_delay.designed_fs == 96000.0);
	CHECK(runtime.surround_delay.designed_samples == 480u);
	CHECK(all_zero(runtime.surround_delay.ring,
	               sizeof(runtime.surround_delay.ring)));

	CHECK(runtime.input_stage_active == 1u);
	CHECK(runtime.input_stage.channels[0].gain_linear ==
	      aius_db_to_linear(-3.0));
	CHECK(runtime.input_stage.channels[0].eq_sections == 2u);
	CHECK(runtime.input_stage.channels[0].eq == runtime.input_eq[0]);
	CHECK(runtime.input_stage.channels[0].eq_state ==
	      runtime.input_eq_state[0]);
	CHECK(biquad_equal(runtime.input_eq[0][0],
	                   aius_biquad_bypass()));
	CHECK(biquad_equal(
		runtime.input_eq[0][1],
		aius_biquad_peaking(96000.0, 1750.0, 0.8, -2.5)));
	CHECK(all_zero(runtime.input_eq_state,
	               sizeof(runtime.input_eq_state)));
	for (channel = 1; channel < AIUS_DSP_INPUT_CHANNELS; channel++) {
		CHECK(runtime.input_stage.channels[channel].gain_linear == 1.0);
		CHECK(runtime.input_stage.channels[channel].eq_sections == 0u);
		CHECK(runtime.input_stage.channels[channel].eq ==
		      runtime.input_eq[channel]);
		CHECK(runtime.input_stage.channels[channel].eq_state ==
		      runtime.input_eq_state[channel]);
	}

	CHECK(runtime.limiter_active == 1u);
	CHECK(runtime.limiter.cfg.enabled == 0);
	CHECK(runtime.limiter.cfg.threshold_dbfs == -3.5);
	CHECK(runtime.limiter.cfg.attack_ms == 2.25);
	CHECK(runtime.limiter.cfg.release_ms == 321.0);
	CHECK(runtime.limiter.cfg.lookahead_ms == 4.0);
	CHECK(all_zero(&runtime.limiter.state,
	               sizeof(runtime.limiter.state)));

	CHECK(runtime.meters_active == 1u);
	CHECK(runtime.meter_window_frames == 4800u);
	CHECK(runtime.meter_us_per_frame_q16 ==
	      (uint32_t)((UINT64_C(1000000) << 16) / 96000u));
	CHECK(meter.sample_rate_hz == 96000u);
	CHECK(meter.seq == 0u);
	CHECK(meter.in_frames == 0u);
	CHECK(meter.out_frames == 0u);
	return 0;
}

static int test_feature_bypasses_and_clipfree_resolution(void)
{
	AiusDspRuntimeConfig runtime;
	AiusDspModel model = production_model();
	AiusDspConfig process;
	double scratch_marker;
	double work_l_marker;
	double work_r_marker;

	model.output_eq_count = 1u;
	model.output_eq[0] =
		(AiusDspModelEq){2u, 1000.0, 1.0, 12.0};
	model.input_channels[2].gain_db = 6.0;
	model.input_channels[3].eq_count = 1u;
	model.input_channels[3].eq[0] =
		(AiusDspModelEq){1u, 80.0, 0.7, 0.0};
	model.matrix.center_gain_linear = 0.8;
	model.matrix.surround_gain_linear = 0.4;
	model.matrix.lfe_gain_linear = 0.2;
	model.matrix.clipfree = 1u;

	CHECK(aius_dsp_runtime_config_apply(
		&runtime, &model, 48000u, NULL) == AIUS_OK);
	CHECK(runtime.downmix.headroom ==
	      aius_downmix_clipfree_headroom(&runtime.downmix));

	model.features.input_eq = 0u;
	model.features.matrix = 0u;
	model.features.output_eq = 0u;
	model.features.limiter = 0u;
	model.features.meters = 0u;
	/* Non-vacuous gate check: the model ASKS for a delay the disabled
	 * matrix feature must refuse. */
	model.matrix.surround_delay_ms = 5.0;
	CHECK(aius_dsp_runtime_config_apply(
		&runtime, &model, 48000u, NULL) == AIUS_OK);
	CHECK(runtime.output_eq_count == 0u);
	CHECK(runtime.input_stage_active == 0u);
	CHECK(runtime.downmix.center == 0.0);
	CHECK(runtime.downmix.surround == 0.0);
	CHECK(runtime.downmix.lfe == 0.0);
	CHECK(runtime.downmix.headroom == 1.0);
	/* The delay rides the matrix feature gate alongside the fold gains. */
	CHECK(runtime.downmix.surround_delay_ms == 0.0);
	CHECK(runtime.surround_delay.designed_samples == 0u);
	CHECK(runtime.limiter_active == 0u);
	CHECK(runtime.meters_active == 0u);

	memset(&process, 0, sizeof(process));
	process.scratch[0] = &scratch_marker;
	process.work_l = &work_l_marker;
	process.work_r = &work_r_marker;
	aius_dsp_runtime_config_bind(&runtime, &process);
	CHECK(process.downmix.center == 0.0);
	CHECK(process.eq == runtime.output_eq);
	CHECK(process.eq_sections == 0u);
	CHECK(process.eq_state == runtime.output_eq_state);
	CHECK(process.input == NULL);
	CHECK(process.limiter == NULL);
	CHECK(process.scratch[0] == &scratch_marker);
	CHECK(process.work_l == &work_l_marker);
	CHECK(process.work_r == &work_r_marker);
	return 0;
}

static int test_identity_input_and_wire_only_fields(void)
{
	AiusDspRuntimeConfig baseline;
	AiusDspRuntimeConfig perturbed;
	AiusDspModel model = production_model();

	CHECK(aius_dsp_runtime_config_apply(
		&baseline, &model, 48000u, NULL) == AIUS_OK);
	CHECK(baseline.input_stage_active == 0u);

	model.sample_rate_hz = 44100u;
	model.base_generation = UINT32_MAX;
	CHECK(aius_dsp_runtime_config_apply(
		&perturbed, &model, 48000u, NULL) == AIUS_OK);
	CHECK(perturbed.sample_rate_hz == baseline.sample_rate_hz);
	CHECK(perturbed.meter_window_frames ==
	      baseline.meter_window_frames);
	CHECK(perturbed.meter_us_per_frame_q16 ==
	      baseline.meter_us_per_frame_q16);
	CHECK(biquad_equal(
		perturbed.lfe_fold.lp[0], baseline.lfe_fold.lp[0]));
	CHECK(biquad_equal(
		perturbed.lfe_fold.lp[1], baseline.lfe_fold.lp[1]));

	model.input_channels[0].eq_count = 1u;
	model.input_channels[0].eq[0] =
		(AiusDspModelEq){0u, 1000.0, 1.0, 0.0};
	CHECK(aius_dsp_runtime_config_apply(
		&perturbed, &model, 48000u, NULL) == AIUS_OK);
	CHECK(perturbed.input_stage_active == 1u);
	return 0;
}

static int test_two_consumers_render_identically_across_blocks(void)
{
	enum { FRAMES = 64 };
	AiusDspRuntimeConfig firmware_runtime;
	AiusDspRuntimeConfig renderer_runtime;
	AiusDspModel model = production_model();
	AiusDspConfig firmware_process;
	AiusDspConfig renderer_process;
	AiusPcmBlock input;
	AiusPcmBlock firmware_output;
	AiusPcmBlock renderer_output;
	int32_t input_planes[AIUS_MAX_CHANNELS][FRAMES];
	double firmware_scratch[AIUS_MAX_CHANNELS][FRAMES];
	double renderer_scratch[AIUS_MAX_CHANNELS][FRAMES];
	double firmware_work_l[FRAMES];
	double firmware_work_r[FRAMES];
	double renderer_work_l[FRAMES];
	double renderer_work_r[FRAMES];
	int32_t firmware_l[FRAMES];
	int32_t firmware_r[FRAMES];
	int32_t renderer_l[FRAMES];
	int32_t renderer_r[FRAMES];
	size_t channel;
	size_t frame;
	unsigned block;

	model.output_eq_count = 1u;
	model.output_eq[0] =
		(AiusDspModelEq){2u, 2200.0, 0.9, 2.0};
	model.input_channels[0].gain_db = -1.5;
	model.input_channels[4].eq_count = 1u;
	model.input_channels[4].eq[0] =
		(AiusDspModelEq){1u, 95.0, 0.707, 0.0};
	model.matrix.center_gain_linear = 0.65;
	model.matrix.surround_gain_linear = 0.55;
	model.matrix.lfe_gain_linear = 0.2;
	model.matrix.headroom_linear = 0.45;
	model.limiter.threshold_dbfs = -6.0;
	model.limiter.attack_ms = 0.5;
	model.limiter.release_ms = 80.0;
	model.limiter.lookahead_ms = 1.0;

	CHECK(aius_dsp_runtime_config_apply(
		&firmware_runtime, &model, 48000u, NULL) == AIUS_OK);
	CHECK(aius_dsp_runtime_config_apply(
		&renderer_runtime, &model, 48000u, NULL) == AIUS_OK);
	memset(&firmware_process, 0, sizeof(firmware_process));
	memset(&renderer_process, 0, sizeof(renderer_process));
	aius_dsp_runtime_config_bind(
		&firmware_runtime, &firmware_process);
	aius_dsp_runtime_config_bind(
		&renderer_runtime, &renderer_process);
	for (channel = 0; channel < AIUS_MAX_CHANNELS; channel++) {
		firmware_process.scratch[channel] =
			firmware_scratch[channel];
		renderer_process.scratch[channel] =
			renderer_scratch[channel];
	}
	firmware_process.work_l = firmware_work_l;
	firmware_process.work_r = firmware_work_r;
	renderer_process.work_l = renderer_work_l;
	renderer_process.work_r = renderer_work_r;

	memset(&input, 0, sizeof(input));
	for (channel = 0; channel < AIUS_MAX_CHANNELS; channel++)
		input.planes[channel] = input_planes[channel];
	input.channels = AIUS_MAX_CHANNELS;
	input.frames = FRAMES;
	input.sample_rate_hz = 48000u;
	input.channel_mask =
		AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC |
		AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR;
	memset(&firmware_output, 0, sizeof(firmware_output));
	memset(&renderer_output, 0, sizeof(renderer_output));
	firmware_output.planes[0] = firmware_l;
	firmware_output.planes[1] = firmware_r;
	renderer_output.planes[0] = renderer_l;
	renderer_output.planes[1] = renderer_r;

	for (block = 0; block < 2u; block++) {
		for (channel = 0; channel < AIUS_MAX_CHANNELS; channel++) {
			for (frame = 0; frame < FRAMES; frame++) {
				const int sign = ((frame + channel + block) & 1u)
					? -1
					: 1;
				input_planes[channel][frame] =
					sign * (int32_t)(
						400000u + 17000u * channel +
						3000u * frame + 11000u * block);
			}
		}
		CHECK(aius_dsp_process(
			&firmware_process, &input, &firmware_output) == AIUS_OK);
		CHECK(aius_dsp_process(
			&renderer_process, &input, &renderer_output) == AIUS_OK);
		CHECK(memcmp(firmware_l, renderer_l,
		             sizeof(firmware_l)) == 0);
		CHECK(memcmp(firmware_r, renderer_r,
		             sizeof(firmware_r)) == 0);
	}
	return 0;
}

int main(void)
{
	CHECK(test_invalid_inputs_fail_closed() == 0);
	CHECK(test_every_model_field_resolves_and_state_resets() == 0);
	CHECK(test_feature_bypasses_and_clipfree_resolution() == 0);
	CHECK(test_identity_input_and_wire_only_fields() == 0);
	CHECK(test_two_consumers_render_identically_across_blocks() == 0);
	puts("ok aius dsp runtime config (shared production resolver)");
	return 0;
}
