#include "aius_audio_ingress.h"

#include <string.h>

static uint32_t load_u32(const volatile uint32_t *value)
{
	return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_u32(volatile uint32_t *value, uint32_t next)
{
	__atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static uint32_t current_level(const AiusAudioIngress *ingress)
{
	return load_u32(&ingress->written) - load_u32(&ingress->consumed);
}

static void record_gap(AiusAudioIngress *ingress,
	                   AiusAudioIngressGapCause cause)
{
	if (!ingress || cause >= AIUS_AUDIO_INGRESS_GAP_CAUSE_COUNT)
		return;
	ingress->dropped_chunks++;
	ingress->dropped_words += AIUS_AUDIO_INGRESS_CHUNK_WORDS;
	ingress->dropped_by_cause[cause]++;
	store_u32(&ingress->generation, load_u32(&ingress->generation) + 1u);
}

AiusStatus aius_audio_ingress_init(AiusAudioIngress *ingress, uint16_t *storage,
	                               size_t capacity_words)
{
	if (!ingress || !storage ||
	    capacity_words < AIUS_AUDIO_INGRESS_CHUNK_WORDS ||
	    capacity_words > AIUS_AUDIO_INGRESS_CAPACITY_WORDS ||
	    (capacity_words & (capacity_words - 1u)) != 0u ||
	    capacity_words % AIUS_AUDIO_INGRESS_CHUNK_WORDS != 0u)
		return AIUS_INVALID_ARG;
	memset(ingress, 0, sizeof(*ingress));
	ingress->words = storage;
	ingress->capacity_words = (uint32_t)capacity_words;
	ingress->mask = (uint32_t)capacity_words - 1u;
	return AIUS_OK;
}

AiusStatus aius_audio_ingress_reserve(AiusAudioIngress *ingress,
	                                  uint16_t **slot)
{
	if (!ingress || !slot || !ingress->words || ingress->reserved)
		return AIUS_INVALID_ARG;
	*slot = NULL;
	const uint32_t written = load_u32(&ingress->written);
	const uint32_t consumed = load_u32(&ingress->consumed);
	if (written - consumed >
	    ingress->capacity_words - AIUS_AUDIO_INGRESS_CHUNK_WORDS) {
		record_gap(ingress, AIUS_AUDIO_INGRESS_GAP_OVERFLOW);
		return AIUS_NO_SPACE;
	}
	ingress->reserved_written = written;
	ingress->reserved = 1u;
	*slot = ingress->words + (written & ingress->mask);
	return AIUS_OK;
}

AiusStatus aius_audio_ingress_commit(AiusAudioIngress *ingress)
{
	if (!ingress || !ingress->reserved)
		return AIUS_INVALID_ARG;
	const uint32_t next =
		ingress->reserved_written + AIUS_AUDIO_INGRESS_CHUNK_WORDS;
	store_u32(&ingress->written, next);
	ingress->reserved = 0u;
	const uint32_t level = next - load_u32(&ingress->consumed);
	if (level > ingress->high_water_words)
		ingress->high_water_words = level;
	return AIUS_OK;
}

void aius_audio_ingress_cancel_gap(AiusAudioIngress *ingress,
	                               AiusAudioIngressGapCause cause)
{
	if (!ingress)
		return;
	ingress->reserved = 0u;
	record_gap(ingress, cause);
}

void aius_audio_ingress_record_gap(AiusAudioIngress *ingress,
	                               AiusAudioIngressGapCause cause)
{
	record_gap(ingress, cause);
}

void aius_audio_ingress_record_source_change(AiusAudioIngress *ingress)
{
	if (!ingress)
		return;
	ingress->source_changes++;
	store_u32(&ingress->source_generation,
	          load_u32(&ingress->source_generation) + 1u);
	store_u32(&ingress->generation, load_u32(&ingress->generation) + 1u);
}

size_t aius_audio_ingress_peek(const AiusAudioIngress *ingress,
	                           const uint16_t **words)
{
	if (!ingress || !words || !ingress->words)
		return 0u;
	*words = NULL;
	const uint32_t consumed = load_u32(&ingress->consumed);
	const uint32_t available = load_u32(&ingress->written) - consumed;
	if (available == 0u)
		return 0u;
	uint32_t count =
		AIUS_AUDIO_INGRESS_CHUNK_WORDS -
		(consumed & (AIUS_AUDIO_INGRESS_CHUNK_WORDS - 1u));
	if (count > available)
		count = available;
	*words = ingress->words + (consumed & ingress->mask);
	return count;
}

AiusStatus aius_audio_ingress_consume(AiusAudioIngress *ingress,
	                                  size_t words)
{
	if (!ingress)
		return AIUS_INVALID_ARG;
	const uint32_t consumed = load_u32(&ingress->consumed);
	const uint32_t available = load_u32(&ingress->written) - consumed;
	const uint32_t in_chunk =
		AIUS_AUDIO_INGRESS_CHUNK_WORDS -
		(consumed & (AIUS_AUDIO_INGRESS_CHUNK_WORDS - 1u));
	if (words > available || words > in_chunk)
		return AIUS_INVALID_ARG;
	store_u32(&ingress->consumed, consumed + (uint32_t)words);
	return AIUS_OK;
}

uint32_t aius_audio_ingress_generation(const AiusAudioIngress *ingress)
{
	return ingress ? load_u32(&ingress->generation) : 0u;
}

uint32_t aius_audio_ingress_source_generation(
	const AiusAudioIngress *ingress)
{
	return ingress ? load_u32(&ingress->source_generation) : 0u;
}

uint32_t aius_audio_ingress_observed_generation(
	const AiusAudioIngress *ingress)
{
	return ingress ? ingress->observed_generation : 0u;
}

uint32_t aius_audio_ingress_observed_source_generation(
	const AiusAudioIngress *ingress)
{
	return ingress ? ingress->observed_source_generation : 0u;
}

void aius_audio_ingress_observe_generation(AiusAudioIngress *ingress,
	                                       uint32_t generation)
{
	if (ingress)
		ingress->observed_generation = generation;
}

void aius_audio_ingress_observe_source_generation(
	AiusAudioIngress *ingress, uint32_t generation)
{
	if (ingress)
		ingress->observed_source_generation = generation;
}

static uint32_t flush_words(AiusAudioIngress *ingress)
{
	const uint32_t consumed = load_u32(&ingress->consumed);
	const uint32_t written = load_u32(&ingress->written);
	store_u32(&ingress->consumed, written);
	return written - consumed;
}

void aius_audio_ingress_flush_gap(AiusAudioIngress *ingress)
{
	if (!ingress)
		return;
	ingress->gap_flushes++;
	ingress->gap_flush_words += flush_words(ingress);
}

void aius_audio_ingress_flush_source(AiusAudioIngress *ingress)
{
	if (!ingress)
		return;
	ingress->source_flushes++;
	ingress->source_flush_words += flush_words(ingress);
}

size_t aius_audio_ingress_level(const AiusAudioIngress *ingress)
{
	return ingress ? current_level(ingress) : 0u;
}

void aius_audio_ingress_get_diag(const AiusAudioIngress *ingress,
	                             AiusAudioIngressDiag *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!ingress)
		return;
	out->level_words = current_level(ingress);
	out->high_water_words = ingress->high_water_words;
	out->generation = load_u32(&ingress->generation);
	out->dropped_chunks = ingress->dropped_chunks;
	out->dropped_words = ingress->dropped_words;
	memcpy(out->dropped_by_cause, ingress->dropped_by_cause,
	       sizeof(out->dropped_by_cause));
	out->source_changes = ingress->source_changes;
	out->source_flushes = ingress->source_flushes;
	out->source_flush_words = ingress->source_flush_words;
	out->gap_flushes = ingress->gap_flushes;
	out->gap_flush_words = ingress->gap_flush_words;
}

void aius_audio_ingress_clear_producer_diag(AiusAudioIngress *ingress)
{
	if (!ingress)
		return;
	ingress->dropped_chunks = 0u;
	ingress->dropped_words = 0u;
	memset(ingress->dropped_by_cause, 0, sizeof(ingress->dropped_by_cause));
	ingress->source_changes = 0u;
	ingress->high_water_words = current_level(ingress);
}
