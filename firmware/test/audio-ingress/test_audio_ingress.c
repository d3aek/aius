#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aius_audio_ingress.h"

static int failures;
static uint32_t allocations;

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);

void *__wrap_malloc(size_t size)
{
	allocations++;
	return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
	allocations++;
	return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	allocations++;
	return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
	__real_free(ptr);
}

#define CHECK(condition, message)                                              \
	do {                                                                     \
		if (!(condition)) {                                                 \
			fprintf(stderr, "FAIL: %s\n", message);                       \
			failures++;                                                    \
		}                                                                    \
	} while (0)

static void publish(AiusAudioIngress *ingress, uint16_t value)
{
	uint16_t *slot = NULL;
	CHECK(aius_audio_ingress_reserve(ingress, &slot) == AIUS_OK,
	      "reserve");
	if (!slot)
		return;
	for (size_t i = 0; i < AIUS_AUDIO_INGRESS_CHUNK_WORDS; i++)
		slot[i] = (uint16_t)(value + i);
	CHECK(aius_audio_ingress_commit(ingress) == AIUS_OK, "commit");
}

static void test_wrap_full_and_prefix(void)
{
	uint16_t storage[512];
	AiusAudioIngress ingress;
	CHECK(aius_audio_ingress_init(&ingress, storage, 512u) == AIUS_OK,
	      "init 512");
	publish(&ingress, 100u);
	publish(&ingress, 1000u);

	uint16_t *slot = (uint16_t *)(uintptr_t)1u;
	CHECK(aius_audio_ingress_reserve(&ingress, &slot) == AIUS_NO_SPACE,
	      "full reserve");
	CHECK(slot == NULL, "full publishes no slot");

	AiusAudioIngressDiag diag;
	aius_audio_ingress_get_diag(&ingress, &diag);
	CHECK(diag.level_words == 512u && diag.high_water_words == 512u,
	      "full level/high-water");
	CHECK(diag.generation == 1u && diag.dropped_chunks == 1u &&
	          diag.dropped_words == 256u &&
	          diag.dropped_by_cause[AIUS_AUDIO_INGRESS_GAP_OVERFLOW] == 1u,
	      "overflow gap diagnostics");

	const uint16_t *words = NULL;
	CHECK(aius_audio_ingress_peek(&ingress, &words) == 256u &&
	          words[0] == 100u,
	      "peek first chunk");
	CHECK(aius_audio_ingress_consume(&ingress, 100u) == AIUS_OK,
	      "partial consume");
	CHECK(aius_audio_ingress_peek(&ingress, &words) == 156u &&
	          words[0] == 200u,
	      "peek retained suffix");
	CHECK(aius_audio_ingress_consume(&ingress, 157u) == AIUS_INVALID_ARG,
	      "cannot cross original chunk");
	CHECK(aius_audio_ingress_consume(&ingress, 156u) == AIUS_OK,
	      "consume suffix");
	CHECK(aius_audio_ingress_peek(&ingress, &words) == 256u &&
	          words[0] == 1000u,
	      "peek second chunk");
	CHECK(aius_audio_ingress_consume(&ingress, 256u) == AIUS_OK,
	      "consume second chunk");

	publish(&ingress, 2000u);
	CHECK(aius_audio_ingress_peek(&ingress, &words) == 256u &&
	          words == storage && words[0] == 2000u,
	      "storage wraps by chunk");
}

static void test_generation_flush_and_clear(void)
{
	uint16_t storage[1024];
	AiusAudioIngress ingress;
	CHECK(aius_audio_ingress_init(&ingress, storage, 1024u) == AIUS_OK,
	      "init diagnostics");
	publish(&ingress, 1u);
	aius_audio_ingress_record_gap(
		&ingress, AIUS_AUDIO_INGRESS_GAP_MISSING_LIVE);
	aius_audio_ingress_record_gap(
		&ingress, AIUS_AUDIO_INGRESS_GAP_SHORT_REPLAY);
	aius_audio_ingress_record_source_change(&ingress);
	CHECK(aius_audio_ingress_generation(&ingress) == 3u,
	      "every gap/source transition advances generation");
	CHECK(aius_audio_ingress_source_generation(&ingress) == 1u,
	      "source transition advances independent generation");
	aius_audio_ingress_observe_source_generation(&ingress, 1u);
	CHECK(aius_audio_ingress_observed_source_generation(&ingress) == 1u,
	      "consumer observes source generation");
	aius_audio_ingress_flush_gap(&ingress);

	AiusAudioIngressDiag diag;
	aius_audio_ingress_get_diag(&ingress, &diag);
	CHECK(diag.gap_flushes == 1u && diag.gap_flush_words == 256u,
	      "gap flush disposition");

	publish(&ingress, 10u);
	aius_audio_ingress_flush_source(&ingress);
	aius_audio_ingress_get_diag(&ingress, &diag);
	CHECK(diag.source_flushes == 1u && diag.source_flush_words == 256u,
	      "source flush disposition");

	publish(&ingress, 20u);
	aius_audio_ingress_record_gap(
		&ingress, AIUS_AUDIO_INGRESS_GAP_MISSING_LIVE);
	aius_audio_ingress_clear_producer_diag(&ingress);
	aius_audio_ingress_get_diag(&ingress, &diag);
	CHECK(diag.level_words == 256u && diag.high_water_words == 256u,
	      "clear rebases high-water to level");
	CHECK(diag.dropped_chunks == 0u && diag.source_changes == 0u,
	      "clear producer interval counters");

	publish(&ingress, 30u);
	aius_audio_ingress_record_gap(
		&ingress, AIUS_AUDIO_INGRESS_GAP_SHORT_REPLAY);
	aius_audio_ingress_get_diag(&ingress, &diag);
	CHECK(diag.high_water_words == 512u && diag.dropped_chunks == 1u &&
	          diag.dropped_by_cause[AIUS_AUDIO_INGRESS_GAP_SHORT_REPLAY] == 1u,
	      "post-clear publish belongs to new interval");
}

static void test_cursor_wrap_and_validation(void)
{
	uint16_t storage[512];
	AiusAudioIngress ingress;
	CHECK(aius_audio_ingress_init(NULL, storage, 512u) == AIUS_INVALID_ARG,
	      "null init");
	CHECK(aius_audio_ingress_init(&ingress, storage, 300u) ==
	          AIUS_INVALID_ARG,
	      "non-power-of-two init");
	CHECK(aius_audio_ingress_init(&ingress, storage, 512u) == AIUS_OK,
	      "wrap init");
	ingress.written = UINT32_MAX - 255u;
	ingress.consumed = UINT32_MAX - 255u;
	publish(&ingress, 77u);
	CHECK(ingress.written == 0u && aius_audio_ingress_level(&ingress) == 256u,
	      "free-running cursor wraps");
	const uint16_t *words = NULL;
	CHECK(aius_audio_ingress_peek(&ingress, &words) == 256u &&
	          words == storage + 256u,
	      "wrapped cursor addresses storage");
	CHECK(aius_audio_ingress_consume(&ingress, 256u) == AIUS_OK &&
	          ingress.consumed == 0u,
	      "consumer cursor wraps");
}

int main(void)
{
	const uint32_t before = allocations;
	test_wrap_full_and_prefix();
	test_generation_flush_and_clear();
	test_cursor_wrap_and_validation();
	CHECK(allocations == before, "steady operations allocate");
	if (failures) {
		fprintf(stderr, "RESULT: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio ingress: exact chunks, wrap, gaps, flushes, clear: ok\n");
	return 0;
}
