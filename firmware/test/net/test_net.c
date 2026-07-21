#include "aius_net.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_round_trip(void)
{
	uint8_t frame[AIUS_NET_MAX_FRAME];
	const uint8_t payload[] = {0x00, 0x12, 0xa5, 0xff};
	size_t len = aius_net_encode(frame, sizeof(frame), AIUS_NET_CMD_GET_STATUS,
	                             0x1234, 0xdeadbeefu, payload, sizeof(payload));
	assert(len == AIUS_NET_HEADER_LEN + sizeof(payload) + AIUS_NET_CRC_LEN);
	AiusNetFrame out;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_OK);
	assert(out.type == AIUS_NET_CMD_GET_STATUS);
	assert(out.flags == 0x1234);
	assert(out.request_id == 0xdeadbeefu);
	assert(out.payload_len == sizeof(payload));
	assert(memcmp(out.payload, payload, sizeof(payload)) == 0);
}

static void test_rejects(void)
{
	uint8_t frame[AIUS_NET_MAX_FRAME];
	size_t len = aius_net_encode(frame, sizeof(frame), AIUS_NET_CMD_DISCOVER,
	                             0, 7, NULL, 0);
	AiusNetFrame out;
	assert(aius_net_decode(frame, len - 1, &out) == AIUS_NET_ERR_SHORT);
	frame[0] ^= 1;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_MAGIC);
	frame[0] ^= 1;
	frame[4]++;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_VERSION);
	frame[4]--;
	frame[14] = 1;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_RESERVED);
	frame[14] = 0;
	frame[len - 1] ^= 0x80;
	assert(aius_net_decode(frame, len, &out) == AIUS_NET_ERR_CRC);
	assert(aius_net_encode(frame, sizeof(frame), 1, 0, 0, NULL, 1) == 0);
}

int main(void)
{
	test_round_trip();
	test_rejects();
	puts("ok net protocol");
	return 0;
}
