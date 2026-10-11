#ifndef AIUS_DSP_RUNTIME_CONFIG_H
#define AIUS_DSP_RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_dsp.h"
#include "aius_dsp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cold, allocation-free resolution of the production DSP model into the exact
 * state consumed by aius_dsp_process(). Firmware and native tools share this
 * adapter; neither may reinterpret feature flags or redesign coefficients.
 *
 * Scratch/work planes remain process-owner storage because their size follows
 * the caller's maximum block. Meter storage likewise remains adapter-owner
 * placement (OCRAM on Teensy), while this object owns its enable/cadence
 * resolution and resets a supplied meter when a model is applied.
 *
 * The resolved object is address-stable state: downmix/input members point to
 * storage inside the same object. Do not byte-copy or move it after apply;
 * apply the model again at the destination address instead. */

#define AIUS_DSP_RUNTIME_OUTPUT_EQ_MAX AIUS_DSP_MODEL_EQ_MAX
#define AIUS_DSP_RUNTIME_METER_PUBLISH_HZ 20u

typedef struct {
	AiusDownmixConfig downmix;
	AiusLfeFold lfe_fold;
	AiusSurroundDelay surround_delay;

	AiusBiquad output_eq[AIUS_DSP_RUNTIME_OUTPUT_EQ_MAX];
	AiusBiquadState output_eq_state[2u * AIUS_DSP_RUNTIME_OUTPUT_EQ_MAX];
	size_t output_eq_count;

	AiusInputStage input_stage;
	AiusBiquad input_eq[AIUS_DSP_INPUT_CHANNELS][AIUS_DSP_INPUT_EQ_MAX];
	AiusBiquadState input_eq_state[AIUS_DSP_INPUT_CHANNELS][AIUS_DSP_INPUT_EQ_MAX];
	uint8_t input_stage_active;

	AiusLimiter limiter;
	uint8_t limiter_active;

	uint8_t meters_active;
	uint32_t meter_window_frames;
	uint32_t meter_us_per_frame_q16;
	uint32_t sample_rate_hz;
} AiusDspRuntimeConfig;

/* Resolve a complete validated model for a non-zero accepted stream rate.
 * Every stateful section is reset. `meter` may be NULL for a consumer that does
 * not collect meters; cadence and enable state are still resolved identically.
 * Returns AIUS_INVALID_ARG for null inputs, zero rate, or model counts that
 * exceed the public DSP bounds. On failure `runtime` is zeroed. */
AiusStatus aius_dsp_runtime_config_apply(
	AiusDspRuntimeConfig *runtime,
	const AiusDspModel *model,
	uint32_t stream_rate_hz,
	AiusMeter *meter);

/* Bind the resolved stages into an AiusDspConfig. Only adapter-owned fields are
 * assigned; caller-owned scratch/work pointers are retained. */
void aius_dsp_runtime_config_bind(
	AiusDspRuntimeConfig *runtime,
	AiusDspConfig *process);

/* Idle apply (Phase 4j). A staged model applies at the next block, or, with
 * no block reaching the DSP for this long, from the service checkpoint. */
#define AIUS_DSP_IDLE_APPLY_MS 100u

/* True when an apply is pending and no block has reached the consume point
 * for AIUS_DSP_IDLE_APPLY_MS. Wrap-safe in uint32 milliseconds. Takes no gate
 * input: a locked carrier that delivers no blocks counts as idle. */
bool aius_dsp_idle_apply_due(
	bool apply_pending,
	uint32_t last_block_ms,
	uint32_t now_ms);

/* One apply decision shared by the block arm and the idle arm.
 * copy: copy the pending model into the active model (and adopt its origin,
 *   clear the pending flag). build: call apply_config at build_rate_hz (the
 *   caller then records build_rate_hz as the applied rate).
 *   generation_steps: how many times to advance the config generation (0 or
 *   1; a model copy plus a rate change is still one step). */
typedef struct {
	bool copy;
	bool build;
	uint32_t build_rate_hz;
	uint32_t generation_steps;
} AiusDspApplyDecision;

/* block_rate_hz 0 selects the idle arm: pending copies and builds at
 * applied_rate_hz with one step; not pending does nothing. A non-zero block
 * rate builds when a model is pending or it differs from applied_rate_hz,
 * at block_rate_hz, with one step. */
AiusDspApplyDecision aius_dsp_apply_decide(
	bool pending,
	uint32_t applied_rate_hz,
	uint32_t block_rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_DSP_RUNTIME_CONFIG_H */
