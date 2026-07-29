#ifndef AIUS_AUDIO_SERVICE_H
#define AIUS_AUDIO_SERVICE_H

#include <stdint.h>

#include "aius_audio_ingress.h"
#include "aius_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_AUDIO_INPUT_WORDS_PER_EPOCH 4096u
#define AIUS_AUDIO_OUTPUT_STEPS_PER_EPOCH 8u
#define AIUS_AUDIO_SERVICE_BUDGET_US 8000u

typedef uint32_t (*AiusAudioServiceClockUs)(void *ctx);
typedef void (*AiusAudioServiceCriticalSection)(void *ctx);

typedef struct {
	uint32_t checkpoint_calls;
	uint32_t input_words_consumed;
	uint32_t service_steps_attempted;
	uint32_t service_steps_committed;
	uint32_t transmit_stops;
	uint32_t generation_resets;
	uint32_t requested_resets;
	uint32_t max_service_us;
	uint32_t budget_overruns;
	AiusStatus last_failure;
} AiusAudioServiceDiag;

typedef struct {
	AiusAudioIngress *ingress;
	AiusPipeline *pipeline;
	AiusAudioServiceClockUs clock_us;
	void *clock_ctx;
	AiusAudioServiceCriticalSection enter_reset_critical;
	AiusAudioServiceCriticalSection exit_reset_critical;
	void *reset_critical_ctx;
	uint32_t observed_reset_sequence;
	AiusAudioServiceDiag diag;
	uint8_t valid;
} AiusAudioService;

AiusStatus aius_audio_service_init(AiusAudioService *service,
	                               AiusAudioIngress *ingress,
	                               AiusPipeline *pipeline,
	                               AiusAudioServiceClockUs clock_us,
	                               void *clock_ctx,
	                               AiusAudioServiceCriticalSection
	                                   enter_reset_critical,
	                               AiusAudioServiceCriticalSection
	                                   exit_reset_critical,
	                               void *reset_critical_ctx);

/* Run one bounded checkpoint. `reset_sequence` is the caller's latest
 * source/carrier reset request. All recoverable parse/decode/sink failures are
 * retained in diagnostics; invalid construction is the only fatal result.
 * The required critical-section hooks mask only the ingress producer while a
 * requested reset flushes and acknowledges its sequence. Pipeline reset and
 * all bounded service work run after the producer is unmasked. */
AiusStatus aius_audio_service_run(AiusAudioService *service,
	                              uint32_t reset_sequence);

void aius_audio_service_get_diag(const AiusAudioService *service,
	                             AiusAudioServiceDiag *out);
void aius_audio_service_clear_diag(AiusAudioService *service);

#ifdef __cplusplus
}
#endif

#endif
