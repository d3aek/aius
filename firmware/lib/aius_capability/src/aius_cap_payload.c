/* Aius capability protocol: typed payload encode/decode (capability-gate P5).
 *
 * Every payload is fixed little-endian layout so the committed golden wire
 * vectors are byte-exact. Getters validate length before reading. */

#include "aius_capability.h"

#include <string.h>

size_t aius_cap_put_capabilities(uint8_t *dst, size_t cap, const AiusCapCapabilities *c)
{
	if (cap < AIUS_CAP_CAPABILITIES_LEN)
		return 0;
	dst[0] = c->abi_version;
	dst[1] = c->proto_version;
	aius_cap_put_u16(dst + 2, c->max_channels);
	aius_cap_put_u32(dst + 4, c->max_payload);
	aius_cap_put_u32(dst + 8, c->capability_flags);
	memcpy(dst + 12, c->name, AIUS_CAP_CAP_NAME_LEN);
	return AIUS_CAP_CAPABILITIES_LEN;
}

int aius_cap_get_capabilities(const uint8_t *p, uint32_t len, AiusCapCapabilities *c)
{
	if (len != AIUS_CAP_CAPABILITIES_LEN)
		return 0;
	c->abi_version = p[0];
	c->proto_version = p[1];
	c->max_channels = aius_cap_get_u16(p + 2);
	c->max_payload = aius_cap_get_u32(p + 4);
	c->capability_flags = aius_cap_get_u32(p + 8);
	memcpy(c->name, p + 12, AIUS_CAP_CAP_NAME_LEN);
	return 1;
}

size_t aius_cap_put_frame_result(uint8_t *dst, size_t cap, const AiusCapFrameResult *r)
{
	if (cap < AIUS_CAP_FRAME_RESULT_LEN)
		return 0;
	dst[0] = r->status;
	dst[1] = 0; /* reserved */
	aius_cap_put_u16(dst + 2, r->channels);
	aius_cap_put_u32(dst + 4, r->frames);
	aius_cap_put_u32(dst + 8, r->sample_rate_hz);
	aius_cap_put_u32(dst + 12, r->channel_mask);
	aius_cap_put_u32(dst + 16, r->arena_high_water);
	aius_cap_put_u32(dst + 20, r->alloc_count);
	return AIUS_CAP_FRAME_RESULT_LEN;
}

int aius_cap_get_frame_result(const uint8_t *p, uint32_t len, AiusCapFrameResult *r)
{
	if (len != AIUS_CAP_FRAME_RESULT_LEN)
		return 0;
	r->status = p[0];
	r->channels = aius_cap_get_u16(p + 2);
	r->frames = aius_cap_get_u32(p + 4);
	r->sample_rate_hz = aius_cap_get_u32(p + 8);
	r->channel_mask = aius_cap_get_u32(p + 12);
	r->arena_high_water = aius_cap_get_u32(p + 16);
	r->alloc_count = aius_cap_get_u32(p + 20);
	return 1;
}

size_t aius_cap_put_pcm_header(uint8_t *dst, size_t cap, const AiusCapPcmHeader *h)
{
	if (cap < AIUS_CAP_PCM_HEADER_LEN)
		return 0;
	aius_cap_put_u16(dst + 0, h->channels);
	aius_cap_put_u16(dst + 2, 0); /* reserved */
	aius_cap_put_u32(dst + 4, h->frames);
	aius_cap_put_u32(dst + 8, h->sample_rate_hz);
	aius_cap_put_u32(dst + 12, h->channel_mask);
	return AIUS_CAP_PCM_HEADER_LEN;
}

int aius_cap_get_pcm_header(const uint8_t *p, uint32_t len, AiusCapPcmHeader *h)
{
	if (len < AIUS_CAP_PCM_HEADER_LEN)
		return 0;
	h->channels = aius_cap_get_u16(p + 0);
	h->frames = aius_cap_get_u32(p + 4);
	h->sample_rate_hz = aius_cap_get_u32(p + 8);
	h->channel_mask = aius_cap_get_u32(p + 12);
	return 1;
}

size_t aius_cap_put_summary(uint8_t *dst, size_t cap, const AiusCapSummary *s)
{
	if (cap < AIUS_CAP_SUMMARY_LEN)
		return 0;
	aius_cap_put_u32(dst + 0, s->frames_submitted);
	aius_cap_put_u32(dst + 4, s->frames_ok);
	aius_cap_put_u32(dst + 8, s->frames_failed);
	aius_cap_put_u32(dst + 12, s->iterations);
	aius_cap_put_u64(dst + 16, s->total_samples);
	aius_cap_put_u32(dst + 24, s->peak_arena_high_water);
	aius_cap_put_u32(dst + 28, s->alloc_violations);
	dst[32] = s->last_status;
	dst[33] = 0; /* reserved */
	dst[34] = 0;
	dst[35] = 0;
	return AIUS_CAP_SUMMARY_LEN;
}

int aius_cap_get_summary(const uint8_t *p, uint32_t len, AiusCapSummary *s)
{
	if (len != AIUS_CAP_SUMMARY_LEN)
		return 0;
	s->frames_submitted = aius_cap_get_u32(p + 0);
	s->frames_ok = aius_cap_get_u32(p + 4);
	s->frames_failed = aius_cap_get_u32(p + 8);
	s->iterations = aius_cap_get_u32(p + 12);
	s->total_samples = aius_cap_get_u64(p + 16);
	s->peak_arena_high_water = aius_cap_get_u32(p + 24);
	s->alloc_violations = aius_cap_get_u32(p + 28);
	s->last_status = p[32];
	return 1;
}

size_t aius_cap_put_diagnostic(uint8_t *dst, size_t cap, const AiusCapDiagnostic *d,
                               const char *message, uint16_t message_len)
{
	if (message == NULL)
		message_len = 0;
	size_t total = (size_t)AIUS_CAP_DIAGNOSTIC_HEADER_LEN + message_len;
	if (cap < total)
		return 0;
	dst[0] = d->error_code;
	dst[1] = d->detail_status;
	dst[2] = 0; /* reserved */
	dst[3] = 0;
	aius_cap_put_u32(dst + 4, d->context_index);
	aius_cap_put_u16(dst + 8, message_len);
	if (message_len != 0)
		memcpy(dst + AIUS_CAP_DIAGNOSTIC_HEADER_LEN, message, message_len);
	return total;
}

int aius_cap_get_diagnostic(const uint8_t *p, uint32_t len, AiusCapDiagnostic *d,
                            const char **message_out, uint16_t *message_len_out)
{
	if (len < AIUS_CAP_DIAGNOSTIC_HEADER_LEN)
		return 0;
	d->error_code = p[0];
	d->detail_status = p[1];
	d->context_index = aius_cap_get_u32(p + 4);
	uint16_t mlen = aius_cap_get_u16(p + 8);
	if ((uint32_t)AIUS_CAP_DIAGNOSTIC_HEADER_LEN + mlen != len)
		return 0;
	if (message_out != NULL)
		*message_out = mlen ? (const char *)(p + AIUS_CAP_DIAGNOSTIC_HEADER_LEN) : NULL;
	if (message_len_out != NULL)
		*message_len_out = mlen;
	return 1;
}

size_t aius_cap_put_nack(uint8_t *dst, size_t cap, const AiusCapNack *n)
{
	if (cap < AIUS_CAP_NACK_LEN)
		return 0;
	dst[0] = n->error_code;
	dst[1] = 0; /* reserved */
	aius_cap_put_u16(dst + 2, n->offending_sequence);
	return AIUS_CAP_NACK_LEN;
}

int aius_cap_get_nack(const uint8_t *p, uint32_t len, AiusCapNack *n)
{
	if (len != AIUS_CAP_NACK_LEN)
		return 0;
	n->error_code = p[0];
	n->offending_sequence = aius_cap_get_u16(p + 2);
	return 1;
}
