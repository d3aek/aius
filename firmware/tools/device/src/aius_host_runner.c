/* Host-side capability runner (capability-gate phase P5). Frames commands,
 * drives a transport seam, reassembles responses, and records an audit
 * transcript. Host-only; the sim transport adapter lives here. */

#include "aius_host_runner.h"

#include <string.h>

#include "aius_sim_device.h"

void aius_host_runner_init(AiusHostRunner *r, const AiusHostTransport *tp)
{
	memset(r, 0, sizeof(*r));
	r->tp = *tp;
	aius_cap_framer_init(&r->resp);
	r->next_seq = 1;
	r->audit = NULL;
	r->write_chunk = 0;
	r->max_polls = 64;
	r->last_error = AIUS_CAP_ERR_NONE;
}

void aius_host_runner_set_audit(AiusHostRunner *r, FILE *f)
{
	r->audit = f;
}

void aius_host_runner_set_write_chunk(AiusHostRunner *r, size_t chunk)
{
	r->write_chunk = chunk;
}

/* ---- sim transport adapter ---- */

static size_t sim_tp_write(void *ctx, const uint8_t *data, size_t len)
{
	return aius_sim_device_write((AiusSimDevice *)ctx, data, len);
}

static size_t sim_tp_read(void *ctx, uint8_t *dst, size_t cap)
{
	return aius_sim_device_read((AiusSimDevice *)ctx, dst, cap);
}

static size_t sim_tp_available(void *ctx)
{
	return aius_sim_device_available((AiusSimDevice *)ctx);
}

void aius_host_runner_bind_sim(AiusHostRunner *r, struct AiusSimDevice *sim)
{
	AiusHostTransport tp = {
		.ctx = sim,
		.write = sim_tp_write,
		.read = sim_tp_read,
		.available = sim_tp_available,
	};
	aius_host_runner_init(r, &tp);
}

/* ---- audit ---- */

static void audit_frame(AiusHostRunner *r, char dir, const uint8_t *frame, size_t len)
{
	if (r->audit == NULL || len < AIUS_CAP_HEADER_LEN + AIUS_CAP_CRC_LEN)
		return;
	uint8_t  type = frame[5];
	uint16_t seq = aius_cap_get_u16(frame + 6);
	uint32_t plen = aius_cap_get_u32(frame + 8);
	uint32_t crc = aius_cap_get_u32(frame + AIUS_CAP_HEADER_LEN + plen);
	fprintf(r->audit, "%c seq=%u type=0x%02x plen=%u crc=%08x bytes=%zu :",
	        dir, seq, type, plen, crc, len);
	size_t preview = plen < 32 ? plen : 32;
	for (size_t i = 0; i < preview; i++)
		fprintf(r->audit, " %02x", frame[AIUS_CAP_HEADER_LEN + i]);
	if (preview < plen)
		fprintf(r->audit, " ...");
	fputc('\n', r->audit);
}

/* ---- send / recv ---- */

static int host_send(AiusHostRunner *r, uint8_t type, const uint8_t *payload,
                     uint32_t plen, uint16_t *seq_out)
{
	static uint8_t frame[AIUS_CAP_MAX_FRAME];
	uint16_t seq = r->next_seq++;
	size_t n = aius_cap_encode(frame, sizeof(frame), type, seq, payload, plen);
	if (n == 0) {
		r->last_error = AIUS_CAP_ERR_PAYLOAD_TOO_BIG;
		return -1;
	}
	audit_frame(r, '>', frame, n);

	size_t off = 0;
	while (off < n) {
		size_t chunk = r->write_chunk ? r->write_chunk : (n - off);
		if (chunk > n - off)
			chunk = n - off;
		size_t w = r->tp.write(r->tp.ctx, frame + off, chunk);
		if (w == 0) {
			r->last_error = AIUS_CAP_ERR_INTERNAL;
			return -1;
		}
		off += w;
	}
	if (seq_out != NULL)
		*seq_out = seq;
	return 0;
}

/* Receive one response frame. Returns 1 (OK frame in *out), 2 (framer error
 * event in *out), or 0 on timeout (last_error = TIMEOUT). */
static int host_recv(AiusHostRunner *r, AiusCapFrame *out)
{
	int polls = r->max_polls;
	for (;;) {
		AiusCapFrameEvent ev = aius_cap_framer_next(&r->resp, out);
		if (ev == AIUS_CAP_FRAME_OK)
			return 1;
		if (ev == AIUS_CAP_FRAME_ERROR)
			return 2;

		size_t avail = r->tp.available(r->tp.ctx);
		if (avail == 0) {
			if (--polls <= 0) {
				r->last_error = AIUS_CAP_ERR_TIMEOUT;
				return 0;
			}
			continue;
		}
		uint8_t buf[4096];
		size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
		size_t got = r->tp.read(r->tp.ctx, buf, want);
		size_t fed = aius_cap_framer_feed(&r->resp, buf, got);
		(void)fed; /* payloads in this suite stay well under one frame */
	}
}

/* Log a device-sent frame after a successful recv. The framer stores the whole
 * frame contiguously; reconstruct the leading header for the audit line. */
static void audit_recv(AiusHostRunner *r, const AiusCapFrame *f)
{
	if (r->audit == NULL)
		return;
	fprintf(r->audit, "< seq=%u type=0x%02x plen=%u :", f->sequence, f->msg_type,
	        f->payload_len);
	size_t preview = f->payload_len < 32 ? f->payload_len : 32;
	for (size_t i = 0; i < preview; i++)
		fprintf(r->audit, " %02x", f->payload[i]);
	if (preview < f->payload_len)
		fprintf(r->audit, " ...");
	fputc('\n', r->audit);
}

/* ---- batch builder ---- */

size_t aius_host_build_batch(uint8_t *dst, size_t cap, const uint8_t *const *aus,
                             const uint32_t *au_lens, size_t count)
{
	size_t pos = 0;
	for (size_t i = 0; i < count; i++) {
		if (pos + 4 + au_lens[i] > cap || pos + 4 + au_lens[i] > AIUS_CAP_MAX_PAYLOAD)
			return 0;
		aius_cap_put_u32(dst + pos, au_lens[i]);
		pos += 4;
		memcpy(dst + pos, aus[i], au_lens[i]);
		pos += au_lens[i];
	}
	return pos;
}

/* ---- high-level commands ---- */

int aius_host_identify(AiusHostRunner *r, AiusCapCapabilities *out)
{
	if (host_send(r, AIUS_CAP_CMD_IDENTIFY, NULL, 0, NULL) != 0)
		return -1;
	AiusCapFrame f;
	for (;;) {
		if (host_recv(r, &f) != 1)
			return -1;
		audit_recv(r, &f);
		/* A target reset report may precede the first response after boot. */
		if (f.msg_type == AIUS_CAP_RSP_DIAGNOSTIC && f.sequence == 0)
			continue;
		break;
	}
	if (f.msg_type != AIUS_CAP_RSP_CAPABILITIES ||
	    !aius_cap_get_capabilities(f.payload, f.payload_len, out)) {
		r->last_error = AIUS_CAP_ERR_INTERNAL;
		return -2;
	}
	return 0;
}

int aius_host_reset_metrics(AiusHostRunner *r)
{
	if (host_send(r, AIUS_CAP_CMD_RESET_METRICS, NULL, 0, NULL) != 0)
		return -1;
	AiusCapFrame f;
	if (host_recv(r, &f) != 1)
		return -1;
	audit_recv(r, &f);
	if (f.msg_type != AIUS_CAP_RSP_ACK) {
		r->last_error = AIUS_CAP_ERR_INTERNAL;
		return -2;
	}
	return 0;
}

int aius_host_submit_frame(AiusHostRunner *r, const uint8_t *au, uint32_t len,
                           AiusCapFrameResult *out)
{
	if (host_send(r, AIUS_CAP_CMD_SUBMIT_FRAME, au, len, NULL) != 0)
		return -1;
	AiusCapFrame f;
	if (host_recv(r, &f) != 1)
		return -1;
	audit_recv(r, &f);
	if (f.msg_type != AIUS_CAP_RSP_FRAME_RESULT ||
	    !aius_cap_get_frame_result(f.payload, f.payload_len, out)) {
		r->last_error = AIUS_CAP_ERR_INTERNAL;
		return -2;
	}
	return 0;
}

int aius_host_request_pcm(AiusHostRunner *r, AiusCapPcmHeader *hdr,
                          int32_t *samples, uint32_t max_samples,
                          uint32_t *got_samples)
{
	if (host_send(r, AIUS_CAP_CMD_REQUEST_PCM, NULL, 0, NULL) != 0)
		return -1;
	AiusCapFrame f;
	if (host_recv(r, &f) != 1)
		return -1;
	audit_recv(r, &f);
	if (f.msg_type == AIUS_CAP_RSP_NACK) {
		AiusCapNack n;
		if (aius_cap_get_nack(f.payload, f.payload_len, &n))
			r->last_error = (AiusCapError)n.error_code;
		return -2;
	}
	if (f.msg_type != AIUS_CAP_RSP_PCM_DATA ||
	    !aius_cap_get_pcm_header(f.payload, f.payload_len, hdr)) {
		r->last_error = AIUS_CAP_ERR_INTERNAL;
		return -2;
	}
	uint32_t n_samples = (uint32_t)hdr->channels * hdr->frames;
	if ((uint64_t)AIUS_CAP_PCM_HEADER_LEN + (uint64_t)n_samples * 4 != f.payload_len) {
		r->last_error = AIUS_CAP_ERR_MALFORMED_PAYLOAD;
		return -2;
	}
	uint32_t copy = n_samples < max_samples ? n_samples : max_samples;
	const uint8_t *sp = f.payload + AIUS_CAP_PCM_HEADER_LEN;
	for (uint32_t i = 0; i < copy; i++)
		samples[i] = (int32_t)aius_cap_get_u32(sp + (size_t)i * 4);
	if (got_samples != NULL)
		*got_samples = copy;
	return 0;
}

/* Read streamed responses until the terminal summary. */
static int recv_until_summary(AiusHostRunner *r, AiusCapSummary *sum,
                              uint32_t *frame_results)
{
	if (frame_results != NULL)
		*frame_results = 0;
	for (;;) {
		AiusCapFrame f;
		if (host_recv(r, &f) != 1)
			return -1;
		audit_recv(r, &f);
		if (f.msg_type == AIUS_CAP_RSP_FRAME_RESULT) {
			if (frame_results != NULL)
				(*frame_results)++;
			continue;
		}
		if (f.msg_type == AIUS_CAP_RSP_DIAGNOSTIC)
			continue; /* informational; summary still follows */
		if (f.msg_type == AIUS_CAP_RSP_NACK) {
			AiusCapNack n;
			if (aius_cap_get_nack(f.payload, f.payload_len, &n))
				r->last_error = (AiusCapError)n.error_code;
			return -2;
		}
		if (f.msg_type == AIUS_CAP_RSP_SUMMARY) {
			if (!aius_cap_get_summary(f.payload, f.payload_len, sum)) {
				r->last_error = AIUS_CAP_ERR_MALFORMED_PAYLOAD;
				return -2;
			}
			return 0;
		}
		r->last_error = AIUS_CAP_ERR_INTERNAL;
		return -2;
	}
}

int aius_host_run_corpus(AiusHostRunner *r, const uint8_t *batch, uint32_t len,
                         AiusCapSummary *sum, uint32_t *frame_results)
{
	if (host_send(r, AIUS_CAP_CMD_RUN_CORPUS, batch, len, NULL) != 0)
		return -1;
	return recv_until_summary(r, sum, frame_results);
}

int aius_host_run_soak(AiusHostRunner *r, uint32_t iterations, const uint8_t *batch,
                       uint32_t len, AiusCapSummary *sum)
{
	static uint8_t payload[AIUS_CAP_MAX_PAYLOAD];
	if ((uint64_t)len + 4 > AIUS_CAP_MAX_PAYLOAD) {
		r->last_error = AIUS_CAP_ERR_PAYLOAD_TOO_BIG;
		return -1;
	}
	aius_cap_put_u32(payload, iterations);
	memcpy(payload + 4, batch, len);
	if (host_send(r, AIUS_CAP_CMD_RUN_SOAK, payload, len + 4, NULL) != 0)
		return -1;
	return recv_until_summary(r, sum, NULL);
}
