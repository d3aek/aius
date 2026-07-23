#include "aius_replay.h"

#include <assert.h>
#include <string.h>

static void test_ring(void)
{
	AiusReplayWordRing ring;
	uint16_t input[] = {0xF872, 0x4E1F, 1, 2};
	uint16_t output[4] = {};
	aius_replay_word_ring_init(&ring);
	assert(aius_replay_word_ring_push(&ring, input, 4) == 4);
	assert(aius_replay_pop_words(&ring, output, 4) == 4);
	assert(memcmp(input, output, sizeof(input)) == 0);
}

static void test_format(void)
{
	const uint8_t bytes[] = {0x72, 0xF8, 0x1F, 0x4E};
	uint16_t words[2] = {};
	size_t count = 0;
	assert(aius_replay_decode_le_words(bytes, sizeof(bytes), words, 2, &count) == AIUS_OK);
	assert(count == 2 && words[0] == 0xF872 && words[1] == 0x4E1F);
	assert(aius_replay_decode_le_words(bytes, 3, words, 2, &count) == AIUS_INVALID_ARG);
	assert(aius_replay_id_is_valid("trailer-51"));
	assert(!aius_replay_id_is_valid("bad.id"));
}

struct memory_reader {
	uint8_t bytes[8192];
};

static AiusStatus memory_open(void *ctx, uint32_t file_bytes)
{
	(void)ctx;
	return file_bytes == 8192u ? AIUS_OK : AIUS_INVALID_ARG;
}

static AiusStatus memory_read(void *ctx, uint32_t offset, uint8_t *buffer,
	                            size_t capacity, size_t *bytes_read)
{
	struct memory_reader *reader = ctx;
	if (offset >= sizeof(reader->bytes)) {
		*bytes_read = 0;
		return AIUS_OK;
	}
	size_t length = sizeof(reader->bytes) - offset;
	if (length > capacity) length = capacity;
	memcpy(buffer, reader->bytes + offset, length);
	*bytes_read = length;
	return AIUS_OK;
}

static void memory_close(void *ctx)
{
	(void)ctx;
}

static void test_service_prebuffer(void)
{
	struct memory_reader memory = {};
	AiusReplayReader reader = {
		&memory, memory_open, memory_read, memory_close
	};
	AiusReplayService service;
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "prebuffer", 8192u, false,
	                                &reader) == AIUS_OK);
	AiusReplayStatus status = {};
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PLAYING);
	assert(status.queue_words >= AIUS_REPLAY_START_WATERMARK_WORDS);
	aius_replay_service_stop(&service);
}

int main(void)
{
	test_ring();
	test_format();
	test_service_prebuffer();
	return 0;
}
