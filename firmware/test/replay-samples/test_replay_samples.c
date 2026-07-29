/* Parser-backed oracle for generated AC-3 replay periods.
 *
 * The Python validator proves the complete fixed IEC layout. This independent
 * native oracle sends every generated word through the production parser and
 * proves that all periods publish the matching raw AC-3 AU without rejection.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_iec61937.h"

#define PERIOD_BYTES 6144u
#define PERIOD_WORDS (PERIOD_BYTES / 2u)
#define PAYLOAD_BYTES 1792u

static int fail(const char *path, const char *message)
{
	fprintf(stderr, "FAIL: %s: %s\n", path, message);
	return 1;
}

static uint8_t *load_file(const char *path, size_t *length)
{
	FILE *file = fopen(path, "rb");
	uint8_t *data;
	long end;

	if (!file || fseek(file, 0, SEEK_END) != 0) {
		if (file)
			fclose(file);
		return NULL;
	}
	end = ftell(file);
	if (end <= 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	data = malloc((size_t)end);
	if (!data || fread(data, 1, (size_t)end, file) != (size_t)end) {
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*length = (size_t)end;
	return data;
}

static int validate(const char *raw_path, const char *iec_path)
{
	uint8_t *raw = NULL;
	uint8_t *iec = NULL;
	uint16_t words[PERIOD_WORDS];
	AiusIec61937Parser *parser = NULL;
	AiusIecDiag diag;
	size_t raw_length = 0;
	size_t iec_length = 0;
	size_t periods;
	size_t burst_count = 0;
	int result = 1;

	raw = load_file(raw_path, &raw_length);
	iec = load_file(iec_path, &iec_length);
	if (!raw || !iec) {
		fail(iec_path, "cannot read non-empty raw/IEC pair");
		goto done;
	}
	if (iec_length % PERIOD_BYTES != 0) {
		fail(iec_path, "IEC size is not a whole number of periods");
		goto done;
	}
	periods = iec_length / PERIOD_BYTES;
	if (raw_length != periods * PAYLOAD_BYTES) {
		fail(raw_path, "raw length does not match IEC period count");
		goto done;
	}
	if (aius_iec61937_parser_create(&parser, NULL) != AIUS_OK) {
		fail(iec_path, "parser creation failed");
		goto done;
	}

	for (size_t period = 0; period < periods; ++period) {
		const uint8_t *period_bytes = iec + period * PERIOD_BYTES;
		size_t position = 0;
		size_t period_bursts = 0;

		for (size_t index = 0; index < PERIOD_WORDS; ++index) {
			words[index] = (uint16_t)period_bytes[index * 2] |
			               ((uint16_t)period_bytes[index * 2 + 1] << 8);
		}

		while (position < PERIOD_WORDS) {
			AiusStatus status;
			size_t consumed = 0;

			status = aius_iec61937_push(parser, words + position,
			                           PERIOD_WORDS - position, &consumed);
			if (consumed == 0) {
				fail(iec_path, "parser made no progress");
				goto done;
			}
			position += consumed;
			if (status == AIUS_OK) {
				AiusBurst burst;

				if (burst_count >= periods ||
				    aius_iec61937_current_burst(parser, &burst) != AIUS_OK ||
				    burst.data_type != 1 || burst.length != PAYLOAD_BYTES ||
				    memcmp(burst.payload, raw + burst_count * PAYLOAD_BYTES,
				           PAYLOAD_BYTES) != 0) {
					fail(iec_path, "parser published the wrong AC-3 burst");
					goto done;
				}
				++period_bursts;
				++burst_count;
			} else if (status != AIUS_NEED_MORE) {
				fail(iec_path, "parser rejected a generated period");
				goto done;
			}
		}
		if (period_bursts != 1) {
			fail(iec_path, "period did not publish exactly one burst");
			goto done;
		}
	}

	if (aius_iec61937_finish(parser) != AIUS_OK ||
	    aius_iec61937_get_diag(parser, &diag) != AIUS_OK ||
	    burst_count != periods || diag.bursts_ok != periods ||
	    diag.rejects_no_space != 0 || diag.rejects_malformed != 0 ||
	    diag.rejects_unsupported != 0) {
		fail(iec_path, "parser finish/diagnostics mismatch");
		goto done;
	}

	printf("ok: %s: %zu parser-accepted periods\n", iec_path, periods);
	result = 0;

done:
	aius_iec61937_parser_destroy(parser);
	free(iec);
	free(raw);
	return result;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: test_replay_samples RAW.ac3 STREAM_ac3.iec\n");
		return 2;
	}
	return validate(argv[1], argv[2]);
}
