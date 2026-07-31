#include "aius_sha256.h"

#include <stddef.h>
#include <string.h>

/* Cold code -> FLASH. Nothing here runs on the audio block path: the digest is
 * folded in once per staged chunk, at the HTTP/SD cadence, not per audio block.
 * FLASHMEM is Teensyduino's __attribute__((section(".flashmem"))); the target
 * macro is defined here rather than pulled from <avr/pgmspace.h> so this file
 * keeps its portable, Arduino-free compile on the host. The attribute must sit
 * on each DEFINITION -- GCC does not merge a `section` attribute from a forward
 * declaration, so a prototype-only FLASHMEM silently leaves the code in ITCM.
 *
 * One caveat this library inherits from aius-ota-upgrade.md: a routine on the
 * self-flash erase/program path must NOT be FLASHMEM, because a FLASHMEM
 * function is unexecutable while the flash is mid-operation. Nothing in this
 * library is on that path -- staging, digesting and verification all complete
 * before any erase begins -- and the later activation slice owns that
 * inversion for its own code. Do not call into this library from inside an
 * erase/program window. */
#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

static const uint32_t k_round_constants[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static FLASHMEM uint32_t rotate_right(uint32_t value, unsigned int bits)
{
	/* Never called with 0 or 32, so the complementary shift is in domain. */
	return (value >> bits) | (value << (32u - bits));
}

static FLASHMEM uint32_t load_be32(const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
	       ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static FLASHMEM void store_be32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)((value >> 16) & 0xffu);
	bytes[2] = (uint8_t)((value >> 8) & 0xffu);
	bytes[3] = (uint8_t)(value & 0xffu);
}

static FLASHMEM void compress_block(uint32_t state[8], const uint8_t *block)
{
	uint32_t schedule[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned int index;

	for (index = 0u; index < 16u; index++)
		schedule[index] = load_be32(&block[index * 4u]);
	for (index = 16u; index < 64u; index++) {
		uint32_t s0 = rotate_right(schedule[index - 15u], 7) ^
			rotate_right(schedule[index - 15u], 18) ^
			(schedule[index - 15u] >> 3);
		uint32_t s1 = rotate_right(schedule[index - 2u], 17) ^
			rotate_right(schedule[index - 2u], 19) ^
			(schedule[index - 2u] >> 10);
		schedule[index] = schedule[index - 16u] + s0 +
			schedule[index - 7u] + s1;
	}

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];
	f = state[5];
	g = state[6];
	h = state[7];

	for (index = 0u; index < 64u; index++) {
		uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
			rotate_right(e, 25);
		uint32_t choose = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + s1 + choose + k_round_constants[index] +
			schedule[index];
		uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
			rotate_right(a, 22);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

FLASHMEM void aius_sha256_init(AiusSha256 *context)
{
	if (context == NULL)
		return;
	memset(context, 0, sizeof(*context));
	context->state[0] = 0x6a09e667u;
	context->state[1] = 0xbb67ae85u;
	context->state[2] = 0x3c6ef372u;
	context->state[3] = 0xa54ff53au;
	context->state[4] = 0x510e527fu;
	context->state[5] = 0x9b05688cu;
	context->state[6] = 0x1f83d9abu;
	context->state[7] = 0x5be0cd19u;
}

FLASHMEM void aius_sha256_update(AiusSha256 *context, const uint8_t *data,
	                         size_t length)
{
	size_t consumed = 0u;

	if (context == NULL || (data == NULL && length != 0u))
		return;

	if (context->buffered != 0u) {
		size_t room = AIUS_SHA256_BLOCK_BYTES - context->buffered;
		size_t take = length < room ? length : room;
		memcpy(&context->buffer[context->buffered], data, take);
		context->buffered = (uint8_t)(context->buffered + take);
		context->absorbed += take;
		consumed = take;
		if (context->buffered < AIUS_SHA256_BLOCK_BYTES)
			return;
		compress_block(context->state, context->buffer);
		context->buffered = 0u;
	}

	while (length - consumed >= AIUS_SHA256_BLOCK_BYTES) {
		compress_block(context->state, &data[consumed]);
		consumed += AIUS_SHA256_BLOCK_BYTES;
		context->absorbed += AIUS_SHA256_BLOCK_BYTES;
	}

	if (consumed < length) {
		size_t tail = length - consumed;
		memcpy(context->buffer, &data[consumed], tail);
		context->buffered = (uint8_t)tail;
		context->absorbed += tail;
	}
}

FLASHMEM void aius_sha256_final(AiusSha256 *context,
	                        uint8_t digest[AIUS_SHA256_DIGEST_BYTES])
{
	uint64_t bit_length;
	unsigned int index;

	if (context == NULL || digest == NULL)
		return;

	bit_length = context->absorbed * 8u;
	context->buffer[context->buffered] = 0x80u;
	context->buffered = (uint8_t)(context->buffered + 1u);
	if (context->buffered > AIUS_SHA256_BLOCK_BYTES - 8u) {
		memset(&context->buffer[context->buffered], 0,
		       AIUS_SHA256_BLOCK_BYTES - context->buffered);
		compress_block(context->state, context->buffer);
		context->buffered = 0u;
	}
	memset(&context->buffer[context->buffered], 0,
	       (AIUS_SHA256_BLOCK_BYTES - 8u) - context->buffered);
	store_be32(&context->buffer[AIUS_SHA256_BLOCK_BYTES - 8u],
	           (uint32_t)(bit_length >> 32));
	store_be32(&context->buffer[AIUS_SHA256_BLOCK_BYTES - 4u],
	           (uint32_t)(bit_length & 0xffffffffu));
	compress_block(context->state, context->buffer);

	for (index = 0u; index < 8u; index++)
		store_be32(&digest[index * 4u], context->state[index]);
	memset(context, 0, sizeof(*context));
}

FLASHMEM void aius_sha256(const uint8_t *data, size_t length,
	                  uint8_t digest[AIUS_SHA256_DIGEST_BYTES])
{
	AiusSha256 context;

	aius_sha256_init(&context);
	aius_sha256_update(&context, data, length);
	aius_sha256_final(&context, digest);
}
