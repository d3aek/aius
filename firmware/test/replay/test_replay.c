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

int main(void)
{
	test_ring();
	test_format();
	return 0;
}
