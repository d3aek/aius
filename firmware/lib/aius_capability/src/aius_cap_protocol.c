/* Aius capability protocol: CRC, frame encoding, and the streaming framer
 * (capability-gate phase P5). Portable C, no heap, no Arduino. */

#include "aius_capability.h"

#include <string.h>

/* CRC-32/ISO-HDLC (a.k.a. zlib/gzip CRC-32): reflected poly 0xEDB88320, init
 * and xorout 0xFFFFFFFF. Bitwise so no table needs provisioning on the target. */
uint32_t aius_cap_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
			crc = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return crc ^ 0xFFFFFFFFu;
}

size_t aius_cap_encode(uint8_t *dst, size_t dst_cap, uint8_t msg_type,
                       uint16_t sequence, const uint8_t *payload,
                       uint32_t payload_len)
{
	if (dst == NULL)
		return 0;
	if (payload_len > AIUS_CAP_MAX_PAYLOAD)
		return 0;
	if (payload == NULL && payload_len != 0)
		return 0;

	size_t total = (size_t)AIUS_CAP_HEADER_LEN + payload_len + AIUS_CAP_CRC_LEN;
	if (dst_cap < total)
		return 0;

	dst[0] = AIUS_CAP_MAGIC0;
	dst[1] = AIUS_CAP_MAGIC1;
	dst[2] = AIUS_CAP_MAGIC2;
	dst[3] = AIUS_CAP_MAGIC3;
	dst[4] = AIUS_CAP_VERSION;
	dst[5] = msg_type;
	aius_cap_put_u16(dst + 6, sequence);
	aius_cap_put_u32(dst + 8, payload_len);
	if (payload_len != 0)
		memcpy(dst + AIUS_CAP_HEADER_LEN, payload, payload_len);

	uint32_t crc = aius_cap_crc32(dst, (size_t)AIUS_CAP_HEADER_LEN + payload_len);
	aius_cap_put_u32(dst + AIUS_CAP_HEADER_LEN + payload_len, crc);
	return total;
}

void aius_cap_framer_init(AiusCapFramer *fr)
{
	fr->len = 0;
	fr->pending_consume = 0;
}

size_t aius_cap_framer_feed(AiusCapFramer *fr, const uint8_t *data, size_t len)
{
	/* Apply any deferred consume first so freshly fed bytes are contiguous. */
	if (fr->pending_consume != 0) {
		size_t drop = fr->pending_consume;
		if (drop > fr->len)
			drop = fr->len;
		memmove(fr->buf, fr->buf + drop, fr->len - drop);
		fr->len -= drop;
		fr->pending_consume = 0;
	}
	size_t space = sizeof(fr->buf) - fr->len;
	size_t take = (len < space) ? len : space;
	if (take != 0) {
		memcpy(fr->buf + fr->len, data, take);
		fr->len += take;
	}
	return take;
}

/* Drop `n` bytes from the front of the buffer. */
static void framer_drop(AiusCapFramer *fr, size_t n)
{
	if (n > fr->len)
		n = fr->len;
	memmove(fr->buf, fr->buf + n, fr->len - n);
	fr->len -= n;
}

/* Index of the first magic start in buf[0..len), or -1 if none. */
static long framer_find_magic(const AiusCapFramer *fr)
{
	if (fr->len < 4)
		return -1;
	for (size_t i = 0; i + 4 <= fr->len; i++) {
		if (fr->buf[i] == AIUS_CAP_MAGIC0 && fr->buf[i + 1] == AIUS_CAP_MAGIC1 &&
		    fr->buf[i + 2] == AIUS_CAP_MAGIC2 && fr->buf[i + 3] == AIUS_CAP_MAGIC3)
			return (long)i;
	}
	return -1;
}

AiusCapFrameEvent aius_cap_framer_next(AiusCapFramer *fr, AiusCapFrame *out)
{
	if (fr->pending_consume != 0) {
		framer_drop(fr, fr->pending_consume);
		fr->pending_consume = 0;
	}

	memset(out, 0, sizeof(*out));

	for (;;) {
		long m = framer_find_magic(fr);
		if (m < 0) {
			/* No magic. Keep at most the trailing 3 bytes, which could be
			 * the start of a magic split across transfers. Bounded. */
			if (fr->len > 3)
				framer_drop(fr, fr->len - 3);
			return AIUS_CAP_FRAME_NONE;
		}
		if (m > 0) {
			/* Garbage before the magic: rescan by discarding it. */
			framer_drop(fr, (size_t)m);
			continue;
		}

		if (fr->len < AIUS_CAP_HEADER_LEN)
			return AIUS_CAP_FRAME_NONE; /* header incomplete */

		uint8_t  version = fr->buf[4];
		uint8_t  type = fr->buf[5];
		uint16_t seq = aius_cap_get_u16(fr->buf + 6);
		uint32_t plen = aius_cap_get_u32(fr->buf + 8);

		if (plen > AIUS_CAP_MAX_PAYLOAD) {
			/* Spurious magic (or corruption): advance one byte and resync. */
			framer_drop(fr, 1);
			continue;
		}

		size_t total = (size_t)AIUS_CAP_HEADER_LEN + plen + AIUS_CAP_CRC_LEN;
		if (fr->len < total)
			return AIUS_CAP_FRAME_NONE; /* payload/CRC incomplete */

		uint32_t crc_calc =
			aius_cap_crc32(fr->buf, (size_t)AIUS_CAP_HEADER_LEN + plen);
		uint32_t crc_wire = aius_cap_get_u32(fr->buf + AIUS_CAP_HEADER_LEN + plen);
		if (crc_calc != crc_wire) {
			/* Corrupt frame: advance one byte past this magic and resync. */
			framer_drop(fr, 1);
			out->error = AIUS_CAP_ERR_BAD_CRC;
			out->sequence = seq;
			out->msg_type = type;
			return AIUS_CAP_FRAME_ERROR;
		}

		if (version != AIUS_CAP_VERSION) {
			/* Structurally sound but wrong version: consume and report. */
			fr->pending_consume = total;
			out->error = AIUS_CAP_ERR_BAD_VERSION;
			out->version = version;
			out->msg_type = type;
			out->sequence = seq;
			return AIUS_CAP_FRAME_ERROR;
		}

		fr->pending_consume = total;
		out->version = version;
		out->msg_type = type;
		out->sequence = seq;
		out->payload = fr->buf + AIUS_CAP_HEADER_LEN;
		out->payload_len = plen;
		out->error = AIUS_CAP_ERR_NONE;
		return AIUS_CAP_FRAME_OK;
	}
}

size_t aius_cap_framer_pending(const AiusCapFramer *fr)
{
	/* pending_consume marks an already-delivered frame not yet dropped; the
	 * genuinely-in-flight partial is whatever remains after that. */
	if (fr->pending_consume >= fr->len)
		return 0;
	return fr->len - fr->pending_consume;
}
