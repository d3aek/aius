#include "aius_audio_service.h"

#include <string.h>

#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

static FLASHMEM uint32_t elapsed_us(uint32_t start, uint32_t now)
{
	return now - start;
}

FLASHMEM AiusStatus aius_audio_service_init(AiusAudioService *service,
	                               AiusAudioIngress *ingress,
	                               AiusPipeline *pipeline,
	                               AiusAudioServiceClockUs clock_us,
	                               void *clock_ctx,
	                               AiusAudioServiceCriticalSection
	                                   enter_reset_critical,
	                               AiusAudioServiceCriticalSection
	                                   exit_reset_critical,
	                               void *reset_critical_ctx)
{
	if (!service || !ingress || !pipeline || !clock_us ||
	    !enter_reset_critical || !exit_reset_critical)
		return AIUS_INVALID_ARG;
	memset(service, 0, sizeof(*service));
	service->ingress = ingress;
	service->pipeline = pipeline;
	service->clock_us = clock_us;
	service->clock_ctx = clock_ctx;
	service->enter_reset_critical = enter_reset_critical;
	service->exit_reset_critical = exit_reset_critical;
	service->reset_critical_ctx = reset_critical_ctx;
	service->observed_reset_sequence = 0u;
	service->diag.last_failure = AIUS_OK;
	service->valid = 1u;
	return AIUS_OK;
}

static FLASHMEM void apply_reset_sequence(AiusAudioService *service,
	                                      uint32_t reset_sequence)
{
	if (reset_sequence == service->observed_reset_sequence)
		return;
	service->enter_reset_critical(service->reset_critical_ctx);
	aius_audio_ingress_flush_source(service->ingress);
	service->observed_reset_sequence = reset_sequence;
	aius_audio_ingress_observe_generation(
		service->ingress,
		aius_audio_ingress_generation(service->ingress));
	aius_audio_ingress_observe_source_generation(
		service->ingress,
		aius_audio_ingress_source_generation(service->ingress));
	service->exit_reset_critical(service->reset_critical_ctx);
	aius_pipeline_reset(service->pipeline);
	service->diag.requested_resets++;
}

static FLASHMEM void apply_generation(AiusAudioService *service)
{
	const uint32_t generation =
		aius_audio_ingress_generation(service->ingress);
	if (generation ==
	    aius_audio_ingress_observed_generation(service->ingress))
		return;
	const uint32_t source_generation =
		aius_audio_ingress_source_generation(service->ingress);
	if (source_generation !=
	    aius_audio_ingress_observed_source_generation(service->ingress))
		aius_audio_ingress_flush_source(service->ingress);
	else
		aius_audio_ingress_flush_gap(service->ingress);
	aius_pipeline_reset(service->pipeline);
	aius_audio_ingress_observe_generation(service->ingress, generation);
	aius_audio_ingress_observe_source_generation(service->ingress,
	                                             source_generation);
	service->diag.generation_resets++;
}

FLASHMEM AiusStatus aius_audio_service_run(AiusAudioService *service,
	                                       uint32_t reset_sequence)
{
	if (!service || !service->valid || !service->ingress ||
	    !service->pipeline || !service->clock_us ||
	    !service->enter_reset_critical || !service->exit_reset_critical)
		return AIUS_INVALID_ARG;

	service->diag.checkpoint_calls++;
	const uint32_t started = service->clock_us(service->clock_ctx);
	uint32_t words = 0u;
	uint32_t steps = 0u;
	int overrun_recorded = 0;
	apply_reset_sequence(service, reset_sequence);

	for (;;) {
		apply_generation(service);

		const uint32_t now = service->clock_us(service->clock_ctx);
		const uint32_t elapsed = elapsed_us(started, now);
		if (elapsed >= AIUS_AUDIO_SERVICE_BUDGET_US) {
			if (elapsed > AIUS_AUDIO_SERVICE_BUDGET_US)
				service->diag.budget_overruns++;
			overrun_recorded = elapsed > AIUS_AUDIO_SERVICE_BUDGET_US;
			break;
		}

		if (aius_pipeline_has_pending(service->pipeline)) {
			if (steps >= AIUS_AUDIO_OUTPUT_STEPS_PER_EPOCH)
				break;
			AiusPipelineServiceResult result;
			AiusPipelineServiceDiag before;
			AiusPipelineServiceDiag after;
			aius_pipeline_get_service_diag(service->pipeline, &before);
			AiusStatus st =
				aius_pipeline_service_one(service->pipeline, &result);
			aius_pipeline_get_service_diag(service->pipeline, &after);
			const uint32_t step_us = elapsed_us(
				now, service->clock_us(service->clock_ctx));
			if (step_us > service->diag.max_step_us)
				service->diag.max_step_us = step_us;
			steps++;
			service->diag.service_steps_attempted++;
			if (after.committed_blocks != before.committed_blocks)
				service->diag.service_steps_committed++;
			if (result == AIUS_PIPELINE_SERVICE_TRANSMIT_BLOCKED) {
				service->diag.transmit_stops++;
				break;
			}
			if (st != AIUS_OK) {
				service->diag.last_failure = st;
				/* Pipeline service makes non-backpressure failures
				 * recoverable before returning them. */
			}
			continue;
		}

		if (words >= AIUS_AUDIO_INPUT_WORDS_PER_EPOCH)
			break;

		const uint16_t *chunk = NULL;
		size_t offered = aius_audio_ingress_peek(service->ingress, &chunk);
		if (offered == 0u &&
		    !aius_pipeline_has_buffered_input(service->pipeline))
			break;
		if (offered > AIUS_AUDIO_INPUT_WORDS_PER_EPOCH - words)
			offered = AIUS_AUDIO_INPUT_WORDS_PER_EPOCH - words;

		size_t consumed = 0u;
		AiusStatus st =
			aius_pipeline_ingest(service->pipeline, chunk, offered, &consumed);
		const uint32_t ingest_us = elapsed_us(
			now, service->clock_us(service->clock_ctx));
		if (ingest_us > service->diag.max_ingest_us)
			service->diag.max_ingest_us = ingest_us;
		if (st != AIUS_OK) {
			service->diag.last_failure = st;
			aius_pipeline_reset(service->pipeline);
			break;
		}
		if (consumed > 0u) {
			if (aius_audio_ingress_consume(service->ingress, consumed) !=
			    AIUS_OK) {
				service->diag.last_failure = AIUS_INVALID_ARG;
				aius_pipeline_reset(service->pipeline);
				break;
			}
			words += (uint32_t)consumed;
			service->diag.input_words_consumed += (uint32_t)consumed;
		}
		if (consumed == 0u &&
		    !aius_pipeline_has_pending(service->pipeline) &&
		    !aius_pipeline_has_buffered_input(service->pipeline))
			break;
	}

	const uint32_t total =
		elapsed_us(started, service->clock_us(service->clock_ctx));
	if (total > service->diag.max_service_us)
		service->diag.max_service_us = total;
	if (total > AIUS_AUDIO_SERVICE_BUDGET_US && !overrun_recorded)
		service->diag.budget_overruns++;
	return AIUS_OK;
}

FLASHMEM void aius_audio_service_get_diag(
	const AiusAudioService *service, AiusAudioServiceDiag *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (service)
		*out = service->diag;
}

FLASHMEM void aius_audio_service_clear_diag(AiusAudioService *service)
{
	if (!service)
		return;
	memset(&service->diag, 0, sizeof(service->diag));
	service->diag.last_failure = AIUS_OK;
}
