/* Native latency reference renderer.
 *
 * Consumes an exact interleaved 16-bit IEC 60958 payload-word carrier through
 * the public IEC parser and production pipeline, retains one decoder/DSP state
 * machine for the whole carrier, and writes continuous interleaved stereo
 * int32 samples at the Aius 24-bit scale plus deterministic per-burst timing
 * metadata. The caller normalizes a captured GET /api/dsp/config body into the
 * production PUT parser shape before invoking this executable.
 */

#include "aius_dsp_runtime_config.h"
#include "aius_iec61937.h"
#include "aius_pipeline.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_CHUNK 4096u
#define RENDER_FRAMES AIUS_PIPELINE_MAX_OUTPUT_FRAMES

typedef struct {
	uint64_t pa_index;
	uint32_t period_words;
	uint32_t payload_bits;
	uint32_t payload_bytes;
	uint16_t pc;
	uint8_t byte_swapped;

	uint32_t decoder_calls;
	uint32_t decode_rejects;
	uint32_t sink_blocks;
	uint32_t decoded_frames;
	uint32_t channel_mask;
	uint32_t sample_rate_hz;
	uint64_t pre_start;
	uint64_t pre_end;
	uint64_t post_zero;
	uint64_t post_last;
} RenderUnit;

typedef struct {
	RenderUnit *items;
	size_t count;
	size_t capacity;
} UnitList;

typedef struct {
	FILE *pcm;
	AiusDspRuntimeConfig runtime;
	AiusDspConfig process;
	double scratch[AIUS_MAX_CHANNELS][RENDER_FRAMES];
	double work_l[RENDER_FRAMES];
	double work_r[RENDER_FRAMES];
	int32_t output_l[RENDER_FRAMES];
	int32_t output_r[RENDER_FRAMES];
	int32_t interleaved[2u * RENDER_FRAMES];
	uint64_t rendered_frames;
	RenderUnit *current;
	int write_failed;
} RenderContext;

typedef struct {
	const char *carrier_path;
	const char *config_path;
	const char *pcm_path;
	const char *manifest_path;
	uint32_t sample_rate_hz;
} Options;

static uint16_t swap16(uint16_t value)
{
	return (uint16_t)((value << 8) | (value >> 8));
}

static int append_unit(UnitList *list, const RenderUnit *unit)
{
	RenderUnit *grown;
	size_t capacity;

	if (list->count == list->capacity) {
		capacity = list->capacity == 0u ? 64u : list->capacity * 2u;
		if (capacity < list->capacity ||
		    capacity > SIZE_MAX / sizeof(*list->items))
			return 0;
		grown = (RenderUnit *)realloc(
			list->items, capacity * sizeof(*list->items));
		if (grown == NULL)
			return 0;
		list->items = grown;
		list->capacity = capacity;
	}
	list->items[list->count++] = *unit;
	return 1;
}

static int file_word_count(FILE *file, uint64_t *words)
{
	long bytes;

	if (fseek(file, 0, SEEK_END) != 0)
		return 0;
	bytes = ftell(file);
	if (bytes < 0 || (bytes & 1l) != 0)
		return 0;
	if (fseek(file, 0, SEEK_SET) != 0)
		return 0;
	*words = (uint64_t)bytes / 2u;
	return 1;
}

static int read_header(
	FILE *carrier, RenderUnit *unit, uint64_t carrier_words)
{
	uint16_t header[4];
	uint16_t pa;
	uint16_t pb;
	uint16_t pc;
	uint16_t pd;

	if (unit->pa_index > carrier_words ||
	    carrier_words - unit->pa_index < 4u ||
	    unit->pa_index > (uint64_t)LONG_MAX / 2u)
		return 0;
	if (fseek(carrier, (long)(unit->pa_index * 2u), SEEK_SET) != 0)
		return 0;
	if (fread(header, sizeof(header[0]), 4u, carrier) != 4u)
		return 0;
	pa = header[0];
	unit->byte_swapped = pa == UINT16_C(0x72f8);
	if (pa != UINT16_C(0xf872) && !unit->byte_swapped)
		return 0;
	pb = unit->byte_swapped ? swap16(header[1]) : header[1];
	pc = unit->byte_swapped ? swap16(header[2]) : header[2];
	pd = unit->byte_swapped ? swap16(header[3]) : header[3];
	if (pb != UINT16_C(0x4e1f) ||
	    (pc & UINT16_C(0x001f)) != unit->pc ||
	    (uint32_t)((pd + 7u) / 8u) != unit->payload_bytes)
		return 0;
	unit->payload_bits = pd;
	return 1;
}

static int scan_units(
	const char *path,
	UnitList *units,
	uint64_t *carrier_words,
	AiusIecDiag *diag)
{
	FILE *carrier = NULL;
	AiusIec61937Parser *parser = NULL;
	uint16_t words[WORD_CHUNK];
	uint64_t absolute = 0u;
	size_t count;
	size_t offset;
	size_t consumed;
	AiusStatus status;
	int ok = 0;
	size_t i;

	carrier = fopen(path, "rb");
	if (carrier == NULL) {
		fprintf(stderr, "native-renderer: open carrier: %s\n",
		        strerror(errno));
		goto done;
	}
	if (!file_word_count(carrier, carrier_words)) {
		fprintf(stderr, "native-renderer: carrier is not an even word file\n");
		goto done;
	}
	if (aius_iec61937_parser_create(&parser, NULL) != AIUS_OK) {
		fprintf(stderr, "native-renderer: IEC parser create failed\n");
		goto done;
	}
	while ((count = fread(words, sizeof(words[0]), WORD_CHUNK, carrier)) >
	       0u) {
		offset = 0u;
		while (offset < count) {
			consumed = 0u;
			status = aius_iec61937_push(
				parser, words + offset, count - offset, &consumed);
			if (consumed == 0u) {
				fprintf(stderr,
				        "native-renderer: parser made no progress\n");
				goto done;
			}
			offset += consumed;
			absolute += consumed;
			if (status == AIUS_OK || status == AIUS_UNSUPPORTED) {
				AiusBurst burst;
				RenderUnit unit;
				uint64_t payload_words;

				if (aius_iec61937_current_burst(
					    parser, &burst) != AIUS_OK)
					goto done;
				payload_words = (burst.length + 1u) / 2u;
				if (absolute < payload_words + 4u)
					goto done;
				memset(&unit, 0, sizeof(unit));
				unit.pa_index =
					absolute - payload_words - 4u;
				unit.pc = burst.data_type;
				unit.payload_bytes = burst.length;
				if (!append_unit(units, &unit)) {
					fprintf(stderr,
					        "native-renderer: unit allocation failed\n");
					goto done;
				}
			} else if (status != AIUS_NEED_MORE) {
				fprintf(stderr,
				        "native-renderer: IEC parse rejected at word "
				        "%" PRIu64 " (status %d)\n",
				        absolute, (int)status);
				goto done;
			}
		}
	}
	if (ferror(carrier)) {
		fprintf(stderr, "native-renderer: carrier read failed\n");
		goto done;
	}
	if (absolute != *carrier_words ||
	    aius_iec61937_finish(parser) != AIUS_OK ||
	    aius_iec61937_get_diag(parser, diag) != AIUS_OK)
		goto done;

	for (i = 0u; i < units->count; i++) {
		RenderUnit *unit = &units->items[i];
		uint64_t next = i + 1u < units->count
			? units->items[i + 1u].pa_index
			: *carrier_words;

		if (!read_header(carrier, unit, *carrier_words) ||
		    next <= unit->pa_index ||
		    next - unit->pa_index > UINT32_MAX) {
			fprintf(stderr,
			        "native-renderer: invalid unit geometry at %zu\n",
			        i);
			goto done;
		}
		unit->period_words =
			(uint32_t)(next - unit->pa_index);
	}
	ok = 1;

done:
	aius_iec61937_parser_destroy(parser);
	if (carrier != NULL)
		fclose(carrier);
	return ok;
}

static AiusStatus consume_pcm(void *opaque, const AiusPcmBlock *input)
{
	RenderContext *context = (RenderContext *)opaque;
	AiusPcmBlock output;
	uint64_t start;
	uint32_t frame;
	AiusStatus status;

	if (context == NULL || input == NULL ||
	    input->frames > RENDER_FRAMES)
		return AIUS_INVALID_ARG;
	memset(&output, 0, sizeof(output));
	output.planes[0] = context->output_l;
	output.planes[1] = context->output_r;
	start = context->rendered_frames;
	status = aius_dsp_process(&context->process, input, &output);
	if (status != AIUS_OK)
		return status;
	for (frame = 0u; frame < output.frames; frame++) {
		context->interleaved[2u * frame] =
			context->output_l[frame];
		context->interleaved[2u * frame + 1u] =
			context->output_r[frame];
	}
	if (fwrite(context->interleaved, sizeof(int32_t),
	           2u * output.frames, context->pcm) !=
	    2u * output.frames) {
		context->write_failed = 1;
		return AIUS_INVALID_ARG;
	}
	context->rendered_frames += output.frames;

	if (context->current != NULL) {
		RenderUnit *unit = context->current;
		const uint32_t delay = context->runtime.limiter_active
			? context->runtime.limiter.state.delay
			: 0u;

		if (unit->decoded_frames == 0u) {
			unit->pre_start = start;
			unit->post_zero = start + delay;
			unit->channel_mask = input->channel_mask;
			unit->sample_rate_hz = input->sample_rate_hz;
		} else if (unit->pre_end != start ||
		           unit->channel_mask != input->channel_mask ||
		           unit->sample_rate_hz != input->sample_rate_hz) {
			return AIUS_INVALID_ARG;
		}
		unit->decoded_frames += input->frames;
		unit->sink_blocks++;
		unit->pre_end = context->rendered_frames;
		unit->post_last =
			unit->pre_start + unit->decoded_frames - 1u + delay;
	}
	return AIUS_OK;
}

static int feed_words(
	FILE *carrier,
	AiusPipeline *pipeline,
	uint64_t start,
	uint64_t count)
{
	uint16_t words[WORD_CHUNK];
	size_t request;
	size_t got;

	if (start > (uint64_t)LONG_MAX / 2u ||
	    fseek(carrier, (long)(start * 2u), SEEK_SET) != 0)
		return 0;
	while (count > 0u) {
		request = count > WORD_CHUNK ? WORD_CHUNK : (size_t)count;
		got = fread(words, sizeof(words[0]), request, carrier);
		if (got != request ||
		    aius_pipeline_push(pipeline, words, got) != AIUS_OK)
			return 0;
		count -= got;
	}
	return 1;
}

static int flush_dsp_tail(RenderContext *context)
{
	int32_t zeros_l[RENDER_FRAMES] = {0};
	int32_t zeros_r[RENDER_FRAMES] = {0};
	AiusPcmBlock block;
	uint32_t remaining;

	remaining = context->runtime.limiter_active
		? context->runtime.limiter.state.delay
		: 0u;
	context->current = NULL;
	while (remaining > 0u) {
		memset(&block, 0, sizeof(block));
		block.planes[0] = zeros_l;
		block.planes[1] = zeros_r;
		block.channels = 2u;
		block.frames = remaining > RENDER_FRAMES
			? RENDER_FRAMES
			: remaining;
		block.sample_rate_hz = context->runtime.sample_rate_hz;
		block.channel_mask = AIUS_CH_FL | AIUS_CH_FR;
		if (consume_pcm(context, &block) != AIUS_OK)
			return 0;
		remaining -= block.frames;
	}
	return 1;
}

static int load_model(
	const char *path,
	uint32_t sample_rate_hz,
	AiusDspModel *model)
{
	FILE *file;
	uint8_t *json;
	long length;
	int ok = 0;

	file = fopen(path, "rb");
	if (file == NULL)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0)
		goto done;
	length = ftell(file);
	if (length <= 0 || length > (long)AIUS_DSP_MODEL_MAX_JSON_BYTES ||
	    fseek(file, 0, SEEK_SET) != 0)
		goto done;
	json = (uint8_t *)malloc((size_t)length);
	if (json == NULL)
		goto done;
	if (fread(json, 1u, (size_t)length, file) == (size_t)length &&
	    aius_dsp_config_parse_put(json, (size_t)length, model) ==
		    AIUS_DSP_CONFIG_OK &&
	    model->sample_rate_hz == sample_rate_hz)
		ok = 1;
	free(json);

done:
	fclose(file);
	return ok;
}

static int render(
	const Options *options,
	UnitList *units,
	uint64_t carrier_words,
	AiusPipelineServiceDiag *service_diag,
	AiusIecDiag *pipeline_parser_diag,
	AiusPipelineClass *classification,
	uint16_t *codec_pc,
	uint64_t *rendered_frames)
{
	FILE *carrier = NULL;
	RenderContext *context = NULL;
	AiusPipeline *pipeline = NULL;
	AiusDspModel model;
	AiusPcmSink sink;
	AiusPipelineConfig config;
	AiusPipelineServiceDiag before;
	AiusPipelineServiceDiag after;
	uint64_t start;
	uint64_t end;
	size_t i;
	size_t channel;
	int ok = 0;

	if (!load_model(
		    options->config_path, options->sample_rate_hz, &model)) {
		fprintf(stderr,
		        "native-renderer: normalized DSP config rejected\n");
		return 0;
	}
	context = (RenderContext *)calloc(1u, sizeof(*context));
	if (context == NULL)
		goto done;
	context->pcm = fopen(options->pcm_path, "wb");
	if (context->pcm == NULL)
		goto done;
	if (aius_dsp_runtime_config_apply(
		    &context->runtime, &model, options->sample_rate_hz,
		    NULL) != AIUS_OK)
		goto done;
	aius_dsp_runtime_config_bind(
		&context->runtime, &context->process);
	for (channel = 0u; channel < AIUS_MAX_CHANNELS; channel++)
		context->process.scratch[channel] =
			context->scratch[channel];
	context->process.work_l = context->work_l;
	context->process.work_r = context->work_r;

	sink.ctx = context;
	sink.consume = consume_pcm;
	memset(&config, 0, sizeof(config));
	config.packing = AIUS_IEC61937_PACK_16;
	if (aius_pipeline_create(&pipeline, &config, &sink) != AIUS_OK ||
	    aius_pipeline_set_pcm_sample_rate(
		    pipeline, options->sample_rate_hz) != AIUS_OK)
		goto done;
	carrier = fopen(options->carrier_path, "rb");
	if (carrier == NULL)
		goto done;

	start = 0u;
	if (units->count > 0u) {
		end = units->items[0].pa_index;
		if (!feed_words(carrier, pipeline, start, end - start))
			goto done;
		start = end;
	}
	for (i = 0u; i < units->count; i++) {
		RenderUnit *unit = &units->items[i];

		end = i + 1u < units->count
			? units->items[i + 1u].pa_index
			: carrier_words;
		context->current = unit;
		aius_pipeline_get_service_diag(pipeline, &before);
		if (!feed_words(carrier, pipeline, start, end - start))
			goto done;
		aius_pipeline_get_service_diag(pipeline, &after);
		unit->decoder_calls =
			after.decode_units - before.decode_units;
		unit->decode_rejects =
			after.decode_rejects - before.decode_rejects;
		if (unit->decoded_frames == 0u ||
		    unit->decode_rejects != 0u) {
			fprintf(stderr,
			        "native-renderer: unit %zu did not decode cleanly\n",
			        i);
			goto done;
		}
		start = end;
	}
	context->current = NULL;
	if (units->count == 0u &&
	    !feed_words(carrier, pipeline, 0u, carrier_words))
		goto done;
	if (aius_pipeline_finish(pipeline) != AIUS_OK ||
	    !flush_dsp_tail(context) || context->write_failed)
		goto done;
	aius_pipeline_get_service_diag(pipeline, service_diag);
	if (aius_pipeline_get_parser_diag(
		    pipeline, pipeline_parser_diag) != AIUS_OK)
		goto done;
	*classification = aius_pipeline_classification(pipeline);
	*codec_pc = aius_pipeline_codec_pc(pipeline);
	*rendered_frames = context->rendered_frames;
	ok = 1;

done:
	if (carrier != NULL)
		fclose(carrier);
	aius_pipeline_destroy(pipeline);
	if (context != NULL) {
		if (context->pcm != NULL)
			fclose(context->pcm);
		free(context);
	}
	return ok;
}

static int write_manifest(
	const Options *options,
	const UnitList *units,
	uint64_t carrier_words,
	uint64_t rendered_frames,
	const AiusIecDiag *scan_diag,
	const AiusIecDiag *pipeline_diag,
	const AiusPipelineServiceDiag *service,
	AiusPipelineClass classification,
	uint16_t codec_pc)
{
	FILE *file;
	size_t i;

	file = fopen(options->manifest_path, "wb");
	if (file == NULL)
		return 0;
	fprintf(file,
	        "{\n"
	        "  \"schema\":\"aius-latency-native-render-v1\",\n"
	        "  \"sample_rate_hz\":%u,\n"
	        "  \"carrier_words\":%" PRIu64 ",\n"
	        "  \"rendered_frames\":%" PRIu64 ",\n"
	        "  \"pipeline\":{\"class\":%d,\"codec_pc\":%u,"
	        "\"decode_units\":%u,\"committed_blocks\":%u,"
	        "\"parse_rejects\":%u,\"decode_rejects\":%u,"
	        "\"output_drops\":%u,\"last_failure\":%d},\n"
	        "  \"parser\":{\"scan_ok\":%u,\"scan_malformed\":%u,"
	        "\"scan_no_space\":%u,\"scan_unsupported\":%u,"
	        "\"pipeline_ok\":%u,\"pipeline_malformed\":%u,"
	        "\"pipeline_no_space\":%u,\"pipeline_unsupported\":%u},\n"
	        "  \"units\":[\n",
	        options->sample_rate_hz,
	        carrier_words,
	        rendered_frames,
	        (int)classification,
	        codec_pc,
	        service->decode_units,
	        service->committed_blocks,
	        service->parse_rejects,
	        service->decode_rejects,
	        service->output_drops,
	        (int)service->last_failure,
	        scan_diag->bursts_ok,
	        scan_diag->rejects_malformed,
	        scan_diag->rejects_no_space,
	        scan_diag->rejects_unsupported,
	        pipeline_diag->bursts_ok,
	        pipeline_diag->rejects_malformed,
	        pipeline_diag->rejects_no_space,
	        pipeline_diag->rejects_unsupported);
	for (i = 0u; i < units->count; i++) {
		const RenderUnit *unit = &units->items[i];
		fprintf(file,
		        "    {\"index\":%zu,\"pa_index\":%" PRIu64
		        ",\"byte_order\":\"%s\",\"pc\":%u,"
		        "\"payload_bits\":%u,\"payload_bytes\":%u,"
		        "\"period_words\":%u,\"decoder_calls\":%u,"
		        "\"decode_rejects\":%u,\"sink_blocks\":%u,"
		        "\"decoded_frames\":%u,\"channel_mask\":%u,"
		        "\"sample_rate_hz\":%u,\"pre_start\":%" PRIu64
		        ",\"pre_end\":%" PRIu64
		        ",\"post_frame_zero\":%" PRIu64
		        ",\"post_frame_last\":%" PRIu64 "}%s\n",
		        i,
		        unit->pa_index,
		        /* fread() presents raw wire bytes as a host uint16_t. On
		         * the supported little-endian host, a big-endian Pa byte
		         * sequence is therefore the parser's swapped value. */
		        unit->byte_swapped ? "big-endian" : "little-endian",
		        unit->pc,
		        unit->payload_bits,
		        unit->payload_bytes,
		        unit->period_words,
		        unit->decoder_calls,
		        unit->decode_rejects,
		        unit->sink_blocks,
		        unit->decoded_frames,
		        unit->channel_mask,
		        unit->sample_rate_hz,
		        unit->pre_start,
		        unit->pre_end,
		        unit->post_zero,
		        unit->post_last,
		        i + 1u == units->count ? "" : ",");
	}
	fputs("  ]\n}\n", file);
	if (fclose(file) != 0)
		return 0;
	return 1;
}

static int parse_options(int argc, char **argv, Options *options)
{
	int i;

	memset(options, 0, sizeof(*options));
	options->sample_rate_hz = 48000u;
	for (i = 1; i < argc; i += 2) {
		if (i + 1 >= argc)
			return 0;
		if (strcmp(argv[i], "--carrier") == 0)
			options->carrier_path = argv[i + 1];
		else if (strcmp(argv[i], "--config") == 0)
			options->config_path = argv[i + 1];
		else if (strcmp(argv[i], "--pcm") == 0)
			options->pcm_path = argv[i + 1];
		else if (strcmp(argv[i], "--manifest") == 0)
			options->manifest_path = argv[i + 1];
		else if (strcmp(argv[i], "--rate") == 0) {
			char *end = NULL;
			unsigned long rate = strtoul(argv[i + 1], &end, 10);

			if (end == argv[i + 1] || *end != '\0' ||
			    rate == 0ul || rate > UINT32_MAX)
				return 0;
			options->sample_rate_hz = (uint32_t)rate;
		} else {
			return 0;
		}
	}
	return options->carrier_path != NULL &&
		options->config_path != NULL &&
		options->pcm_path != NULL &&
		options->manifest_path != NULL;
}

int main(int argc, char **argv)
{
	Options options;
	UnitList units = {0};
	uint64_t carrier_words = 0u;
	uint64_t rendered_frames = 0u;
	AiusIecDiag scan_diag;
	AiusIecDiag pipeline_diag;
	AiusPipelineServiceDiag service;
	AiusPipelineClass classification;
	uint16_t codec_pc;
	int ok = 0;

	if (!parse_options(argc, argv, &options)) {
		fprintf(stderr,
		        "usage: native-renderer --carrier FILE --config FILE "
		        "--pcm FILE --manifest FILE [--rate HZ]\n");
		goto done;
	}
	memset(&scan_diag, 0, sizeof(scan_diag));
	memset(&pipeline_diag, 0, sizeof(pipeline_diag));
	memset(&service, 0, sizeof(service));
	if (!scan_units(
		    options.carrier_path, &units, &carrier_words, &scan_diag) ||
	    !render(
		    &options, &units, carrier_words, &service,
		    &pipeline_diag, &classification, &codec_pc,
		    &rendered_frames) ||
	    !write_manifest(
		    &options, &units, carrier_words, rendered_frames,
		    &scan_diag, &pipeline_diag, &service, classification,
		    codec_pc)) {
		fprintf(stderr, "native-renderer: failed\n");
		goto done;
	}
	ok = 1;

done:
	free(units.items);
	return ok ? 0 : 1;
}
