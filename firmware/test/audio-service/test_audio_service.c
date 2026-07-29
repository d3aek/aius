#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_audio_service.h"

_Static_assert(AIUS_AUDIO_INPUT_WORDS_PER_EPOCH == 4096u,
               "controller input quota changed");
_Static_assert(AIUS_AUDIO_OUTPUT_STEPS_PER_EPOCH == 8u,
               "controller output quota changed");
_Static_assert(AIUS_AUDIO_SERVICE_BUDGET_US == 8000u,
               "controller time budget changed");

static int failures;

#define CHECK(condition, message)                                              \
	do {                                                                     \
		if (!(condition)) {                                                 \
			fprintf(stderr, "FAIL: %s\n", message);                       \
			failures++;                                                    \
		}                                                                    \
	} while (0)

typedef struct {
	uint32_t value;
	uint32_t step;
} FakeClock;

static uint32_t fake_clock_us(void *ctx)
{
	FakeClock *clock = ctx;
	const uint32_t value = clock->value;
	clock->value += clock->step;
	return value;
}

static void noop_critical(void *ctx)
{
	(void)ctx;
}

typedef struct {
	uint32_t calls;
	uint32_t committed;
	int block_once;
	const AiusPcmBlock *first_descriptor;
	const AiusPcmBlock *retry_descriptor;
	int32_t first_sample;
} Sink;

static AiusStatus consume(void *ctx, const AiusPcmBlock *block)
{
	Sink *sink = ctx;
	sink->calls++;
	if (sink->calls == 1u)
		sink->first_descriptor = block;
	else if (sink->calls == 2u)
		sink->retry_descriptor = block;
	sink->first_sample = block->planes[0][0];
	if (sink->block_once) {
		sink->block_once = 0;
		return AIUS_NO_SPACE;
	}
	sink->committed++;
	return AIUS_OK;
}

static void publish_chunk(AiusAudioIngress *ingress, const uint16_t *words,
	                      size_t count)
{
	uint16_t *slot = NULL;
	CHECK(aius_audio_ingress_reserve(ingress, &slot) == AIUS_OK,
	      "reserve ingress chunk");
	if (!slot)
		return;
	memset(slot, 0, AIUS_AUDIO_INGRESS_CHUNK_WORDS * sizeof(*slot));
	if (count > AIUS_AUDIO_INGRESS_CHUNK_WORDS)
		count = AIUS_AUDIO_INGRESS_CHUNK_WORDS;
	if (words && count)
		memcpy(slot, words, count * sizeof(*slot));
	CHECK(aius_audio_ingress_commit(ingress) == AIUS_OK,
	      "commit ingress chunk");
}

static uint16_t *load_wire(const char *dir, size_t *word_count)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/dts_be16.iec", dir);
	FILE *file = fopen(path, "rb");
	if (!file)
		return NULL;
	CHECK(fseek(file, 0, SEEK_END) == 0, "seek vector end");
	const long bytes = ftell(file);
	CHECK(bytes > 0 && (bytes & 1) == 0, "vector byte length");
	CHECK(fseek(file, 0, SEEK_SET) == 0, "seek vector start");
	uint8_t *raw = malloc((size_t)bytes);
	uint16_t *words = malloc((size_t)bytes);
	if (!raw || !words) {
		fclose(file);
		free(raw);
		free(words);
		return NULL;
	}
	CHECK(fread(raw, 1u, (size_t)bytes, file) == (size_t)bytes,
	      "read vector");
	fclose(file);
	*word_count = (size_t)bytes / 2u;
	for (size_t i = 0; i < *word_count; i++)
		words[i] = (uint16_t)((uint16_t)raw[2u * i] << 8) |
		           raw[2u * i + 1u];
	free(raw);
	return words;
}

static AiusPipeline *make_pipeline(Sink *sink)
{
	AiusPcmSink pcm_sink = { .ctx = sink, .consume = consume };
	AiusPipeline *pipeline = NULL;
	CHECK(aius_pipeline_create(&pipeline, NULL, &pcm_sink) == AIUS_OK,
	      "create pipeline");
	return pipeline;
}

typedef struct {
	AiusAudioIngress *ingress;
	AiusPipeline *pipeline;
	uint32_t enter_calls;
	uint32_t exit_calls;
	int masked;
	int exit_saw_empty_ingress;
	int exit_saw_pre_reset_pipeline;
} ResetCritical;

static void enter_reset_critical(void *ctx)
{
	ResetCritical *critical = ctx;
	critical->enter_calls++;
	critical->masked = 1;
	/* This represents an old-source callback that completed immediately
	 * before the target masked further audio updates. */
	publish_chunk(critical->ingress, NULL, 0u);
}

static void exit_reset_critical(void *ctx)
{
	ResetCritical *critical = ctx;
	critical->exit_calls++;
	critical->exit_saw_empty_ingress =
		aius_audio_ingress_level(critical->ingress) == 0u;
	critical->exit_saw_pre_reset_pipeline =
		aius_pipeline_classification(critical->pipeline) ==
		AIUS_PIPELINE_PCM;
	critical->masked = 0;
	/* This represents the first new-source callback immediately after the
	 * target unmasks audio updates. It must survive the completed flush. */
	publish_chunk(critical->ingress, NULL, 0u);
}

static void test_word_quota(void)
{
	uint16_t storage[AIUS_AUDIO_INGRESS_CAPACITY_WORDS];
	AiusAudioIngress ingress;
	Sink sink = { 0 };
	AiusPipeline *pipeline = make_pipeline(&sink);
	FakeClock clock = { 0 };
	AiusAudioService service;
	CHECK(aius_audio_ingress_init(&ingress, storage,
	                              AIUS_AUDIO_INGRESS_CAPACITY_WORDS) == AIUS_OK,
	      "quota ingress init");
	CHECK(aius_audio_service_init(&service, &ingress, pipeline, fake_clock_us,
	                              &clock, noop_critical, noop_critical,
	                              NULL) == AIUS_OK,
	      "quota service init");
	for (size_t i = 0; i < 32u; i++)
		publish_chunk(&ingress, NULL, 0u);
	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK, "quota run");
	CHECK(aius_audio_ingress_level(&ingress) == 4096u,
	      "4096-word quota leaves half ring");
	AiusAudioServiceDiag diag;
	aius_audio_service_get_diag(&service, &diag);
	CHECK(diag.input_words_consumed == 4096u,
	      "word quota exact diagnostic");
	aius_pipeline_destroy(pipeline);
}

static void test_time_quota(void)
{
	uint16_t storage[512];
	AiusAudioIngress ingress;
	Sink sink = { 0 };
	AiusPipeline *pipeline = make_pipeline(&sink);
	FakeClock clock = { .step = 4000u };
	AiusAudioService service;
	CHECK(aius_audio_ingress_init(&ingress, storage, 512u) == AIUS_OK,
	      "time ingress init");
	CHECK(aius_audio_service_init(&service, &ingress, pipeline, fake_clock_us,
	                              &clock, noop_critical, noop_critical,
	                              NULL) == AIUS_OK,
	      "time service init");
	publish_chunk(&ingress, NULL, 0u);
	publish_chunk(&ingress, NULL, 0u);
	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK, "time run");
	CHECK(aius_audio_ingress_level(&ingress) == 256u,
	      "8ms check stops after first atomic ingest");
	AiusAudioServiceDiag diag;
	aius_audio_service_get_diag(&service, &diag);
	CHECK(diag.max_service_us == 12000u && diag.budget_overruns == 1u,
	      "over-budget unit counted once");
	aius_pipeline_destroy(pipeline);
}

static void test_partial_retry_and_no_alloc(const char *dir)
{
	size_t word_count = 0u;
	uint16_t *wire = load_wire(dir, &word_count);
	CHECK(wire != NULL && word_count < 1024u, "load short DTS burst");
	if (!wire)
		return;

	uint16_t storage[2048];
	AiusAudioIngress ingress;
	Sink sink = { .block_once = 1 };
	AiusPipeline *pipeline = make_pipeline(&sink);
	FakeClock clock = { 0 };
	AiusAudioService service;
	CHECK(aius_audio_ingress_init(&ingress, storage, 2048u) == AIUS_OK,
	      "partial ingress init");
	CHECK(aius_audio_service_init(&service, &ingress, pipeline, fake_clock_us,
	                              &clock, noop_critical, noop_critical,
	                              NULL) == AIUS_OK,
	      "partial service init");
	for (size_t off = 0u; off < 1024u;
	     off += AIUS_AUDIO_INGRESS_CHUNK_WORDS) {
		const size_t take =
			off < word_count
				? (word_count - off < AIUS_AUDIO_INGRESS_CHUNK_WORDS
				       ? word_count - off
				       : AIUS_AUDIO_INGRESS_CHUNK_WORDS)
				: 0u;
		publish_chunk(&ingress, take ? wire + off : NULL, take);
	}

	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK,
	      "partial first run");
	CHECK(sink.calls == 1u && sink.committed == 0u,
	      "first decoded output is blocked");
	CHECK(aius_audio_ingress_level(&ingress) == 1024u - word_count,
	      "mid-chunk suffix retained in ingress");
	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK,
	      "partial retry run");
	CHECK(sink.calls == 2u && sink.committed == 1u,
	      "retry commits once");
	CHECK(sink.first_descriptor == sink.retry_descriptor,
	      "retry uses identical retained descriptor");
	CHECK(aius_audio_ingress_level(&ingress) == 0u,
	      "suffix re-presented after service");

	AiusPipelineServiceDiag pipeline_diag;
	aius_pipeline_get_service_diag(pipeline, &pipeline_diag);
	CHECK(pipeline_diag.decode_units == 1u &&
	          pipeline_diag.sink_attempts == 2u &&
	          pipeline_diag.committed_blocks == 1u &&
	          pipeline_diag.transmit_stalls == 1u &&
	          pipeline_diag.retained_retries == 1u,
	      "decode once and retry diagnostics");
	AiusAudioServiceDiag service_diag;
	aius_audio_service_get_diag(&service, &service_diag);
	CHECK(service_diag.input_words_consumed == 1024u &&
	          service_diag.service_steps_attempted == 2u &&
	          service_diag.service_steps_committed == 1u &&
	          service_diag.transmit_stops == 1u,
	      "controller partial/backpressure diagnostics");

	for (size_t off = 0u; off < 1024u;
	     off += AIUS_AUDIO_INGRESS_CHUNK_WORDS) {
		const size_t take =
			off < word_count
				? (word_count - off < AIUS_AUDIO_INGRESS_CHUNK_WORDS
				       ? word_count - off
				       : AIUS_AUDIO_INGRESS_CHUNK_WORDS)
				: 0u;
		publish_chunk(&ingress, take ? wire + off : NULL, take);
	}
	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK,
	      "steady controller run");
	AiusMetrics metrics;
	CHECK(aius_pipeline_get_metrics(pipeline, &metrics) == AIUS_OK &&
	          metrics.alloc_count == 0u,
	      "steady controller/decode path allocates");
	aius_pipeline_destroy(pipeline);
	free(wire);
}

static void test_generation_and_reset_sequence(void)
{
	uint16_t storage[1024];
	AiusAudioIngress ingress;
	Sink sink = { 0 };
	AiusPipeline *pipeline = make_pipeline(&sink);
	FakeClock clock = { 0 };
	AiusAudioService service;
	CHECK(aius_audio_ingress_init(&ingress, storage, 1024u) == AIUS_OK,
	      "reset ingress init");
	publish_chunk(&ingress, NULL, 0u);
	aius_audio_ingress_record_gap(
		&ingress, AIUS_AUDIO_INGRESS_GAP_MISSING_LIVE);
	CHECK(aius_audio_service_init(&service, &ingress, pipeline, fake_clock_us,
	                              &clock, noop_critical, noop_critical,
	                              NULL) == AIUS_OK,
	      "reset service init");
	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK,
	      "generation reset run");
	CHECK(aius_audio_ingress_level(&ingress) == 0u,
	      "generation flushes queued words");

	publish_chunk(&ingress, NULL, 0u);
	CHECK(aius_audio_service_run(&service, 7u) == AIUS_OK,
	      "requested reset run");
	CHECK(aius_audio_ingress_level(&ingress) == 0u,
	      "requested reset flushes queued words");

	publish_chunk(&ingress, NULL, 0u);
	aius_audio_ingress_record_source_change(&ingress);
	CHECK(aius_audio_service_run(&service, 7u) == AIUS_OK,
	      "producer source-change run");
	CHECK(aius_audio_ingress_level(&ingress) == 0u,
	      "producer source change flushes queued words");

	AiusAudioServiceDiag diag;
	aius_audio_service_get_diag(&service, &diag);
	CHECK(diag.generation_resets == 2u && diag.requested_resets == 1u,
	      "gap, source and requested reset sequences observed");
	AiusAudioIngressDiag ingress_diag;
	aius_audio_ingress_get_diag(&ingress, &ingress_diag);
	CHECK(ingress_diag.gap_flushes == 1u &&
	          ingress_diag.source_flushes == 2u &&
	          ingress_diag.gap_flush_words == 256u &&
	          ingress_diag.source_flush_words == 512u,
	      "reset dispositions visible");
	aius_pipeline_destroy(pipeline);
}

static void test_requested_reset_critical_section(void)
{
	uint16_t storage[512];
	AiusAudioIngress ingress;
	Sink sink = { 0 };
	AiusPipeline *pipeline = make_pipeline(&sink);
	FakeClock clock = { 0 };
	AiusAudioService service;
	ResetCritical critical = {
		.ingress = &ingress,
		.pipeline = pipeline,
	};
	CHECK(aius_audio_ingress_init(&ingress, storage, 512u) == AIUS_OK,
	      "critical ingress init");
	CHECK(aius_audio_service_init(
	          &service, &ingress, pipeline, fake_clock_us, &clock,
	          enter_reset_critical, exit_reset_critical, &critical) == AIUS_OK,
	      "critical service init");
	publish_chunk(&ingress, NULL, 0u);
	publish_chunk(&ingress, NULL, 0u);
	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK &&
	          aius_pipeline_classification(pipeline) == AIUS_PIPELINE_PCM,
	      "critical pre-reset pipeline is typed");

	CHECK(aius_audio_service_run(&service, 1u) == AIUS_OK,
	      "critical requested reset run");
	CHECK(critical.enter_calls == 1u && critical.exit_calls == 1u &&
	          !critical.masked && critical.exit_saw_empty_ingress &&
	          critical.exit_saw_pre_reset_pipeline,
	      "flush/ack is masked and pipeline reset follows unmask");
	CHECK(aius_audio_ingress_level(&ingress) == 0u &&
	          aius_pipeline_classification(pipeline) ==
	              AIUS_PIPELINE_UNKNOWN,
	      "old-source chunk flushed and new-source chunk parsed after reset");
	AiusAudioIngressDiag ingress_diag;
	aius_audio_ingress_get_diag(&ingress, &ingress_diag);
	CHECK(ingress_diag.source_flushes == 1u &&
	          ingress_diag.source_flush_words == 256u,
	      "critical flush drops exactly the racing old-source chunk");
	aius_pipeline_destroy(pipeline);
}

static void test_rejected_work_not_committed(void)
{
	uint16_t storage[512];
	AiusAudioIngress ingress;
	Sink sink = { 0 };
	AiusPipeline *pipeline = make_pipeline(&sink);
	FakeClock clock = { 0 };
	AiusAudioService service;
	CHECK(aius_audio_ingress_init(&ingress, storage, 512u) == AIUS_OK,
	      "reject ingress init");
	const uint16_t malformed_dts[] = {
		0xf872u, 0x4e1fu, 0x000du, 0x0010u, 0u, 0u
	};
	publish_chunk(&ingress, malformed_dts,
	              sizeof(malformed_dts) / sizeof(malformed_dts[0]));
	publish_chunk(&ingress, NULL, 0u);
	CHECK(aius_audio_service_init(&service, &ingress, pipeline, fake_clock_us,
	                              &clock, noop_critical, noop_critical,
	                              NULL) == AIUS_OK,
	      "reject service init");
	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK,
	      "reject service run");
	AiusAudioServiceDiag diag;
	aius_audio_service_get_diag(&service, &diag);
	CHECK(diag.service_steps_attempted == 1u &&
	          diag.service_steps_committed == 0u,
	      "rejected decode is attempted but not committed");
	AiusPipelineServiceDiag pipeline_diag;
	aius_pipeline_get_service_diag(pipeline, &pipeline_diag);
	CHECK(pipeline_diag.decode_rejects == 1u &&
	          pipeline_diag.committed_blocks == 0u && sink.calls == 0u,
	      "rejected decode emits no output");
	aius_pipeline_destroy(pipeline);
}

static void test_output_step_quota(void)
{
	uint16_t storage[512];
	uint16_t staged[512] = { 0 };
	const uint16_t malformed_dts[] = {
		0xf872u, 0x4e1fu, 0x000du, 0x0010u, 0u, 0u
	};
	for (size_t off = 0u;
	     off + sizeof(malformed_dts) / sizeof(malformed_dts[0]) <=
	         sizeof(staged) / sizeof(staged[0]);
	     off += sizeof(malformed_dts) / sizeof(malformed_dts[0]))
		memcpy(staged + off, malformed_dts, sizeof(malformed_dts));

	AiusAudioIngress ingress;
	Sink sink = { 0 };
	AiusPipeline *pipeline = make_pipeline(&sink);
	FakeClock clock = { 0 };
	AiusAudioService service;
	CHECK(aius_audio_ingress_init(&ingress, storage, 512u) == AIUS_OK,
	      "step quota ingress init");
	publish_chunk(&ingress, staged, 256u);
	publish_chunk(&ingress, staged + 256u, 256u);
	CHECK(aius_audio_service_init(&service, &ingress, pipeline, fake_clock_us,
	                              &clock, noop_critical, noop_critical,
	                              NULL) == AIUS_OK,
	      "step quota service init");

	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK,
	      "step quota first run");
	AiusAudioServiceDiag service_diag;
	aius_audio_service_get_diag(&service, &service_diag);
	AiusPipelineServiceDiag pipeline_diag;
	aius_pipeline_get_service_diag(pipeline, &pipeline_diag);
	CHECK(service_diag.service_steps_attempted == 8u &&
	          service_diag.service_steps_committed == 0u &&
	          pipeline_diag.decode_units == 8u &&
	          aius_pipeline_has_pending(pipeline),
	      "first epoch stops at eight steps with ninth work retained");

	CHECK(aius_audio_service_run(&service, 0u) == AIUS_OK,
	      "step quota resume run");
	aius_audio_service_get_diag(&service, &service_diag);
	aius_pipeline_get_service_diag(pipeline, &pipeline_diag);
	CHECK(service_diag.service_steps_attempted == 16u &&
	          service_diag.service_steps_committed == 0u &&
	          pipeline_diag.decode_units == 16u &&
	          aius_pipeline_has_pending(pipeline),
	      "next epoch resumes pending work without repeated decode");
	CHECK(sink.calls == 0u, "rejected quota work reaches no sink");
	aius_pipeline_destroy(pipeline);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: test_audio_service <iec-corpus-dir>\n");
		return 2;
	}
	test_word_quota();
	test_time_quota();
	test_partial_retry_and_no_alloc(argv[1]);
	test_generation_and_reset_sequence();
	test_requested_reset_critical_section();
	test_rejected_work_not_committed();
	test_output_step_quota();
	if (failures) {
		fprintf(stderr, "RESULT: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio service: quotas, suffix, retry, resets, no-allocation: ok\n");
	return 0;
}
