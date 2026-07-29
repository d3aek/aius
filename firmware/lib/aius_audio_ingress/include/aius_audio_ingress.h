#ifndef AIUS_AUDIO_INGRESS_H
#define AIUS_AUDIO_INGRESS_H

#include <stddef.h>
#include <stdint.h>

#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIUS_AUDIO_INGRESS_CHUNK_WORDS 256u
#define AIUS_AUDIO_INGRESS_CAPACITY_WORDS 8192u

typedef enum {
	AIUS_AUDIO_INGRESS_GAP_OVERFLOW = 0,
	AIUS_AUDIO_INGRESS_GAP_MISSING_LIVE,
	AIUS_AUDIO_INGRESS_GAP_SHORT_REPLAY,
	AIUS_AUDIO_INGRESS_GAP_CAUSE_COUNT
} AiusAudioIngressGapCause;

typedef struct {
	uint32_t level_words;
	uint32_t high_water_words;
	uint32_t generation;
	uint32_t dropped_chunks;
	uint32_t dropped_words;
	uint32_t dropped_by_cause[AIUS_AUDIO_INGRESS_GAP_CAUSE_COUNT];
	uint32_t source_changes;
	uint32_t source_flushes;
	uint32_t source_flush_words;
	uint32_t gap_flushes;
	uint32_t gap_flush_words;
} AiusAudioIngressDiag;

typedef struct {
	uint16_t *words;
	uint32_t capacity_words;
	uint32_t mask;
	volatile uint32_t written;
	volatile uint32_t consumed;
	volatile uint32_t generation;
	/* Independent source sequence is published before its matching generic
	 * generation, allowing the consumer to attribute the discontinuity. */
	volatile uint32_t source_generation;
	uint32_t observed_generation;
	uint32_t observed_source_generation;
	uint32_t reserved_written;
	uint8_t reserved;

	uint32_t high_water_words;
	uint32_t dropped_chunks;
	uint32_t dropped_words;
	uint32_t dropped_by_cause[AIUS_AUDIO_INGRESS_GAP_CAUSE_COUNT];
	uint32_t source_changes;
	uint32_t source_flushes;
	uint32_t source_flush_words;
	uint32_t gap_flushes;
	uint32_t gap_flush_words;
} AiusAudioIngress;

/* `capacity_words` is a power-of-two multiple of the exact engine chunk and is
 * at most AIUS_AUDIO_INGRESS_CAPACITY_WORDS. The target supplies exactly the
 * maximum. The storage and instance remain caller-owned. */
AiusStatus aius_audio_ingress_init(AiusAudioIngress *ingress, uint16_t *storage,
	                               size_t capacity_words);

/* Producer operations. reserve publishes no data. commit publishes the whole
 * 256-word slot. A full reserve records one overflow gap and returns NO_SPACE. */
AiusStatus aius_audio_ingress_reserve(AiusAudioIngress *ingress,
	                                  uint16_t **slot);
AiusStatus aius_audio_ingress_commit(AiusAudioIngress *ingress);
void aius_audio_ingress_cancel_gap(AiusAudioIngress *ingress,
	                               AiusAudioIngressGapCause cause);
void aius_audio_ingress_record_gap(AiusAudioIngress *ingress,
	                               AiusAudioIngressGapCause cause);
void aius_audio_ingress_record_source_change(AiusAudioIngress *ingress);

/* Consumer operations. peek never crosses an original 256-word chunk boundary;
 * consume advances only the returned prefix. */
size_t aius_audio_ingress_peek(const AiusAudioIngress *ingress,
	                           const uint16_t **words);
AiusStatus aius_audio_ingress_consume(AiusAudioIngress *ingress,
	                                  size_t words);
uint32_t aius_audio_ingress_generation(const AiusAudioIngress *ingress);
uint32_t aius_audio_ingress_source_generation(
	const AiusAudioIngress *ingress);
uint32_t aius_audio_ingress_observed_generation(
	const AiusAudioIngress *ingress);
uint32_t aius_audio_ingress_observed_source_generation(
	const AiusAudioIngress *ingress);
void aius_audio_ingress_observe_generation(AiusAudioIngress *ingress,
	                                       uint32_t generation);
void aius_audio_ingress_observe_source_generation(
	AiusAudioIngress *ingress, uint32_t generation);
void aius_audio_ingress_flush_gap(AiusAudioIngress *ingress);
void aius_audio_ingress_flush_source(AiusAudioIngress *ingress);

size_t aius_audio_ingress_level(const AiusAudioIngress *ingress);
void aius_audio_ingress_get_diag(const AiusAudioIngress *ingress,
	                             AiusAudioIngressDiag *out);

/* Called with the producer masked. It clears only producer-owned interval
 * counters and rebases high-water to the current level. */
void aius_audio_ingress_clear_producer_diag(AiusAudioIngress *ingress);

#ifdef __cplusplus
}
#endif

#endif
