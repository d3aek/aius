#include "aius_dsp_runtime_config.h"

#include <string.h>

#if AIUS_DSP_MODEL_INPUT_CHANNELS != AIUS_DSP_INPUT_CHANNELS
#error "production model and DSP input channel counts must agree"
#endif

#if AIUS_DSP_MODEL_EQ_MAX != AIUS_DSP_INPUT_EQ_MAX
#error "production model and DSP input EQ bounds must agree"
#endif

#if defined(ARDUINO_TEENSY41)
#define AIUS_DSP_RUNTIME_COLD __attribute__((section(".flashmem")))
#define AIUS_DSP_RUNTIME_AUDIO \
	__attribute__((section(".flashmem.audio_enqueue")))
#else
#define AIUS_DSP_RUNTIME_COLD
#define AIUS_DSP_RUNTIME_AUDIO
#endif

static AiusBiquad AIUS_DSP_RUNTIME_COLD
resolve_eq(const AiusDspModelEq *section, uint32_t rate)
{
	if (section->type == 0u)
		return aius_biquad_bypass();
	if (section->type == 1u)
		return aius_biquad_highpass(
			(double)rate, section->frequency_hz, section->q);
	return aius_biquad_peaking(
		(double)rate, section->frequency_hz, section->q,
		section->gain_db);
}

AIUS_DSP_RUNTIME_COLD
AiusStatus aius_dsp_runtime_config_apply(
	AiusDspRuntimeConfig *runtime,
	const AiusDspModel *model,
	uint32_t stream_rate_hz,
	AiusMeter *meter)
{
	size_t channel;
	size_t section;
	int input_changes_audio = 0;

	if (runtime == NULL)
		return AIUS_INVALID_ARG;
	memset(runtime, 0, sizeof(*runtime));
	if (model == NULL || stream_rate_hz == 0u ||
	    model->output_eq_count > AIUS_DSP_RUNTIME_OUTPUT_EQ_MAX)
		return AIUS_INVALID_ARG;
	for (channel = 0; channel < AIUS_DSP_INPUT_CHANNELS; channel++) {
		if (model->input_channels[channel].eq_count >
		    AIUS_DSP_INPUT_EQ_MAX)
			return AIUS_INVALID_ARG;
	}

	runtime->sample_rate_hz = stream_rate_hz;
	runtime->output_eq_count =
		model->features.output_eq ? model->output_eq_count : 0u;
	for (section = 0; section < runtime->output_eq_count; section++)
		runtime->output_eq[section] =
			resolve_eq(&model->output_eq[section], stream_rate_hz);

	runtime->downmix = aius_downmix_config_default();
	if (model->features.matrix) {
		runtime->downmix.center = model->matrix.center_gain_linear;
		runtime->downmix.surround = model->matrix.surround_gain_linear;
		runtime->downmix.lfe = model->matrix.lfe_gain_linear;
	} else {
		runtime->downmix.center = 0.0;
		runtime->downmix.surround = 0.0;
		runtime->downmix.lfe = 0.0;
	}
	runtime->downmix.lfe_crossover_hz =
		model->matrix.lfe_crossover_hz;
	runtime->downmix.lfe_fold = &runtime->lfe_fold;
	(void)aius_lfe_fold_init(
		&runtime->lfe_fold, (double)stream_rate_hz,
		model->matrix.lfe_crossover_hz);
	runtime->downmix.layout_aware = model->matrix.layout_aware;
	/* Surround decorrelation: state resolved (and reset) here like the
	 * crossover above; a failed init leaves designed_samples 0, which the
	 * downmix reads as the undelayed fold (fail open, aius_dsp.h). The
	 * feature gate rides `matrix` -- with the matrix feature off the delay
	 * is forced 0 alongside the fold gains above. */
	runtime->downmix.surround_delay_ms =
		model->features.matrix ? model->matrix.surround_delay_ms : 0.0;
	runtime->downmix.surround_delay = &runtime->surround_delay;
	(void)aius_surround_delay_init(
		&runtime->surround_delay, (double)stream_rate_hz,
		runtime->downmix.surround_delay_ms);
	if (!model->features.matrix)
		runtime->downmix.headroom = 1.0;
	else if (model->matrix.clipfree)
		runtime->downmix.headroom =
			aius_downmix_clipfree_headroom(&runtime->downmix);
	else
		runtime->downmix.headroom =
			model->matrix.headroom_linear;

	runtime->input_stage = aius_input_stage_default();
	for (channel = 0; channel < AIUS_DSP_INPUT_CHANNELS; channel++) {
		const AiusDspModelInputChannel *input =
			&model->input_channels[channel];
		AiusInputChannelConfig *resolved =
			&runtime->input_stage.channels[channel];

		resolved->gain_linear = aius_db_to_linear(input->gain_db);
		resolved->eq_sections = input->eq_count;
		for (section = 0; section < input->eq_count; section++)
			runtime->input_eq[channel][section] =
				resolve_eq(&input->eq[section], stream_rate_hz);
		resolved->eq = runtime->input_eq[channel];
		resolved->eq_state = runtime->input_eq_state[channel];
		if (resolved->gain_linear != 1.0 ||
		    resolved->eq_sections != 0u)
			input_changes_audio = 1;
	}
	runtime->input_stage_active =
		(uint8_t)(model->features.input_eq && input_changes_audio);

	runtime->limiter.cfg.enabled = model->limiter.enabled;
	runtime->limiter.cfg.threshold_dbfs =
		model->limiter.threshold_dbfs;
	runtime->limiter.cfg.attack_ms = model->limiter.attack_ms;
	runtime->limiter.cfg.release_ms = model->limiter.release_ms;
	runtime->limiter.cfg.lookahead_ms = model->limiter.lookahead_ms;
	runtime->limiter_active = model->features.limiter != 0u;

	runtime->meter_window_frames =
		stream_rate_hz / AIUS_DSP_RUNTIME_METER_PUBLISH_HZ;
	if (runtime->meter_window_frames == 0u)
		runtime->meter_window_frames = 1u;
	runtime->meter_us_per_frame_q16 =
		(uint32_t)((UINT64_C(1000000) << 16) / stream_rate_hz);
	runtime->meters_active = model->features.meters != 0u;
	if (meter != NULL)
		aius_meter_init(meter, stream_rate_hz);

	return AIUS_OK;
}

/* This is per-block work, but it belongs to the serial AUDIO epoch rather than
 * the bounded AudioStream ingress producer. Keep it beside enqueue() in flash:
 * an exact-image A/B tied the live-ingress regression to this object's default
 * ITCM placement. The mechanism is NOT established -- both legs perturbed code
 * layout, so the legs are confounded -- and flash placement is retained because
 * it is the configuration measured safe, not because the cause is understood.
 * test/itcm-placement/verify_target.py enforces it. */
AIUS_DSP_RUNTIME_AUDIO
void aius_dsp_runtime_config_bind(
	AiusDspRuntimeConfig *runtime,
	AiusDspConfig *process)
{
	if (runtime == NULL || process == NULL)
		return;
	process->downmix = runtime->downmix;
	process->eq = runtime->output_eq;
	process->eq_sections = runtime->output_eq_count;
	process->eq_state = runtime->output_eq_state;
	process->input = runtime->input_stage_active
		? &runtime->input_stage
		: NULL;
	process->limiter = runtime->limiter_active
		? &runtime->limiter
		: NULL;
}
