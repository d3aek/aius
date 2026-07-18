#ifndef AIUS_HOST_RUNNER_H
#define AIUS_HOST_RUNNER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "aius_capability.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side capability runner (capability-gate phase P5).
 *
 * Frames commands, drives a transport, parses responses, and records a raw
 * audit transcript so a run is reviewable without re-running. The transport is
 * a two-function seam (write/read/available) so the identical runner drives the
 * in-process simulator now and the real USB-Serial link at P6.
 */

typedef struct {
	void  *ctx;
	size_t (*write)(void *ctx, const uint8_t *data, size_t len);
	size_t (*read)(void *ctx, uint8_t *dst, size_t cap);
	size_t (*available)(void *ctx);
} AiusHostTransport;

typedef struct {
	AiusHostTransport tp;
	AiusCapFramer     resp;      /* response reassembly */
	uint16_t          next_seq;
	FILE             *audit;     /* transcript sink, or NULL */
	size_t            write_chunk; /* 0 = one write; >0 = split to exercise partials */
	int               max_polls; /* recv attempts before timeout */
	AiusCapError      last_error;
} AiusHostRunner;

void aius_host_runner_init(AiusHostRunner *r, const AiusHostTransport *tp);

/* Record every framed command/response to `f` (may be NULL to disable). */
void aius_host_runner_set_audit(AiusHostRunner *r, FILE *f);

/* Split each command into `chunk`-byte writes (0 restores single-write). Used to
 * prove the device reassembles partial + coalesced transfers. */
void aius_host_runner_set_write_chunk(AiusHostRunner *r, size_t chunk);

/* Bind a runner to an AiusSimDevice via the transport seam (defined in the
 * host_runner TU so callers need not include the sim header to wire one up). */
struct AiusSimDevice;
void aius_host_runner_bind_sim(AiusHostRunner *r, struct AiusSimDevice *sim);

/* Build a [u32 len][au]... batch payload from `count` access units. Returns the
 * batch length, or 0 if it would exceed `cap` / AIUS_CAP_MAX_PAYLOAD. */
size_t aius_host_build_batch(uint8_t *dst, size_t cap, const uint8_t *const *aus,
                             const uint32_t *au_lens, size_t count);

/* High-level commands. Each returns 0 on success, -1 on transport/timeout, or
 * -2 when the device replied with an unexpected type / NACK (see last_error). */
int aius_host_identify(AiusHostRunner *r, AiusCapCapabilities *out);
int aius_host_reset_metrics(AiusHostRunner *r);
int aius_host_submit_frame(AiusHostRunner *r, const uint8_t *au, uint32_t len,
                           AiusCapFrameResult *out);
/* Fills `hdr` and up to `max_samples` channel-major planar int32 samples. */
int aius_host_request_pcm(AiusHostRunner *r, AiusCapPcmHeader *hdr,
                          int32_t *samples, uint32_t max_samples,
                          uint32_t *got_samples);
/* Streams frame results (counted into `frame_results` if non-NULL) then reads
 * the terminal summary. */
int aius_host_run_corpus(AiusHostRunner *r, const uint8_t *batch, uint32_t len,
                         AiusCapSummary *sum, uint32_t *frame_results);
int aius_host_run_soak(AiusHostRunner *r, uint32_t iterations, const uint8_t *batch,
                       uint32_t len, AiusCapSummary *sum);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_HOST_RUNNER_H */
