#include "aius_net.h"

#include <string.h>

#define AIUS_NET_MAGIC0 0x41u
#define AIUS_NET_MAGIC1 0x55u
#define AIUS_NET_MAGIC2 0x4Eu
#define AIUS_NET_MAGIC3 0x31u

uint32_t aius_net_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffu;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

size_t aius_net_encode(uint8_t *dst, size_t dst_cap, uint8_t type,
                       uint16_t flags, uint32_t request_id,
                       const uint8_t *payload, uint16_t payload_len)
{
	size_t total = AIUS_NET_HEADER_LEN + (size_t)payload_len + AIUS_NET_CRC_LEN;
	if (dst == NULL || total > dst_cap || payload_len > AIUS_NET_MAX_PAYLOAD ||
	    (payload_len != 0 && payload == NULL))
		return 0;
	dst[0] = AIUS_NET_MAGIC0;
	dst[1] = AIUS_NET_MAGIC1;
	dst[2] = AIUS_NET_MAGIC2;
	dst[3] = AIUS_NET_MAGIC3;
	dst[4] = AIUS_NET_VERSION;
	dst[5] = type;
	aius_net_put_u16(dst + 6, flags);
	aius_net_put_u32(dst + 8, request_id);
	aius_net_put_u16(dst + 12, payload_len);
	aius_net_put_u16(dst + 14, 0);
	if (payload_len != 0)
		memcpy(dst + AIUS_NET_HEADER_LEN, payload, payload_len);
	aius_net_put_u32(dst + total - AIUS_NET_CRC_LEN,
	                 aius_net_crc32(dst, total - AIUS_NET_CRC_LEN));
	return total;
}

AiusNetError aius_net_decode(const uint8_t *src, size_t len,
                             AiusNetFrame *out)
{
	if (src == NULL || out == NULL)
		return AIUS_NET_ERR_SHORT;
	if (len < AIUS_NET_HEADER_LEN + AIUS_NET_CRC_LEN)
		return AIUS_NET_ERR_SHORT;
	if (src[0] != AIUS_NET_MAGIC0 || src[1] != AIUS_NET_MAGIC1 ||
	    src[2] != AIUS_NET_MAGIC2 || src[3] != AIUS_NET_MAGIC3)
		return AIUS_NET_ERR_MAGIC;
	if (src[4] != AIUS_NET_VERSION)
		return AIUS_NET_ERR_VERSION;
	uint16_t payload_len = aius_net_get_u16(src + 12);
	if (payload_len > AIUS_NET_MAX_PAYLOAD)
		return AIUS_NET_ERR_LENGTH;
	if (aius_net_get_u16(src + 14) != 0)
		return AIUS_NET_ERR_RESERVED;
	size_t total = AIUS_NET_HEADER_LEN + (size_t)payload_len + AIUS_NET_CRC_LEN;
	if (total != len)
		return AIUS_NET_ERR_LENGTH;
	if (aius_net_get_u32(src + total - AIUS_NET_CRC_LEN) !=
	    aius_net_crc32(src, total - AIUS_NET_CRC_LEN))
		return AIUS_NET_ERR_CRC;
	if (src[5] == 0)
		return AIUS_NET_ERR_TYPE;
	out->version = src[4];
	out->type = src[5];
	out->flags = aius_net_get_u16(src + 6);
	out->request_id = aius_net_get_u32(src + 8);
	out->payload = src + AIUS_NET_HEADER_LEN;
	out->payload_len = payload_len;
	return AIUS_NET_OK;
}
