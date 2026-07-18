/* Aius capability round-trip suite (capability-gate phase P5): the in-process
 * simulated device driven by the host runner over a byte pipe, decoding the real
 * DTS Core corpus end to end.
 *
 * Proves, without a board:
 *   - identify negotiates capabilities;
 *   - submit-frame decodes a raw DTS Core access unit and returns a per-frame
 *     result whose layout matches the known vector;
 *   - request-PCM returns the decoded PCM, carried losslessly over the wire
 *     (bit-exact vs a direct decode of the same units on a matched fresh
 *     decoder);
 *   - run-corpus streams per-frame results then a summary;
 *   - run-soak repeats a batch with zero steady-state allocations (the no-heap
 *     guarantee, observed through the protocol);
 *   - partial + coalesced transfers reassemble;
 *   - duplicate-sequence, unknown-type, no-PCM, and bad-CRC all yield typed
 *     NACKs.
 *
 * Usage: test_capability <vectors_dir>
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_capability.h"
#include "aius_codec.h"
#include "aius_dts.h"
#include "aius_host_runner.h"
#include "aius_sim_device.h"

#define ARENA_BYTES (96u * 1024u)
#define MAX_FRAMES  8192
#define SAMPLES_CAP (AIUS_MAX_CHANNELS * 2048)

static int g_failures;

#define CHECK(cond, ...)                                                        \
	do {                                                                        \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);               \
			fprintf(stderr, __VA_ARGS__);                                      \
			fputc('\n', stderr);                                               \
			g_failures++;                                                      \
		}                                                                       \
	} while (0)

struct vector {
	const char *name;
	uint16_t    channels;
	uint32_t    mask;
};

static const struct vector k_vectors[] = {
	{ "surround51_tones", 6,
	  AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR },
	{ "stereo_tones", 2, AIUS_CH_FL | AIUS_CH_FR },
	{ "surround51_impulse", 6,
	  AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR },
	{ "stereo_impulse", 2, AIUS_CH_FL | AIUS_CH_FR },
};

static uint8_t *read_file(const char *path, size_t *size_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *buf = malloc((size_t)n);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	if (got != (size_t)n) {
		free(buf);
		return NULL;
	}
	*size_out = got;
	return buf;
}

/* DTS Core sync words (0x7FFE8001) mark access-unit boundaries. */
static int find_frames(const uint8_t *data, size_t size, size_t *offsets, int max)
{
	int n = 0;
	for (size_t i = 0; i + 4 <= size; i++) {
		if (data[i] == 0x7f && data[i + 1] == 0xfe && data[i + 2] == 0x80 &&
		    data[i + 3] == 0x01) {
			if (n < max)
				offsets[n] = i;
			n++;
			i += 3;
		}
	}
	return n;
}

/* The arena backing must be static, not heap: the alloc shim remembers the
 * arena's address range for the process lifetime, so a heap-allocated backing
 * buffer sits inside that range and its free() is swallowed as an arena no-op
 * (a leak). Static storage sidesteps the issue exactly as the P3 arena test
 * does. Sims are used strictly sequentially, so one shared arena is sufficient;
 * each caller passes its own static AiusSimDevice + arena buffer for clarity. */
static int sim_open(AiusSimDevice *sim, uint8_t *mem)
{
	return aius_sim_device_init(sim, mem, ARENA_BYTES) == AIUS_OK;
}

/* ---- lossless PCM transport: fresh sim vs fresh matched libc decoder ---- */

static void test_lossless_pcm(const char *dir)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/surround51_tones.dts", dir);
	size_t size = 0;
	uint8_t *data = read_file(path, &size);
	CHECK(data != NULL, "lossless: cannot read %s", path);
	if (!data)
		return;

	size_t offs[MAX_FRAMES];
	int nframes = find_frames(data, size, offs, MAX_FRAMES);
	CHECK(nframes >= 4, "lossless: need >=4 frames, got %d", nframes);

	static uint8_t mem[ARENA_BYTES];
	static AiusSimDevice sim;
	if (!sim_open(&sim, mem)) {
		CHECK(0, "lossless: sim init failed");
		free(data);
		return;
	}
	AiusHostRunner run;
	aius_host_runner_bind_sim(&run, &sim);

	AiusDtsDecoder *ref = NULL;
	CHECK(aius_dts_decoder_create(&ref) == AIUS_OK, "lossless: ref decoder");

	int limit = nframes < 6 ? nframes : 6;
	static int32_t samples[SAMPLES_CAP];
	for (int fi = 0; fi < limit; fi++) {
		size_t start = offs[fi];
		size_t end = (fi + 1 < nframes) ? offs[fi + 1] : size;
		uint32_t au_len = (uint32_t)(end - start);

		AiusCapFrameResult res;
		CHECK(aius_host_submit_frame(&run, data + start, au_len, &res) == 0,
		      "lossless: submit frame %d", fi);
		CHECK(res.status == AIUS_OK, "lossless: frame %d status %u", fi, res.status);

		AiusCapPcmHeader hdr;
		uint32_t got = 0;
		CHECK(aius_host_request_pcm(&run, &hdr, samples, SAMPLES_CAP, &got) == 0,
		      "lossless: request pcm %d", fi);

		AiusPcmBlock blk;
		CHECK(aius_dts_decode_access_unit(ref, data + start, au_len, &blk) == AIUS_OK,
		      "lossless: ref decode %d", fi);

		CHECK(hdr.channels == blk.channels, "lossless: ch %u!=%u", hdr.channels,
		      blk.channels);
		CHECK(hdr.frames == blk.frames, "lossless: frames %u!=%u", hdr.frames,
		      blk.frames);
		CHECK(got == (uint32_t)blk.channels * blk.frames, "lossless: sample count");

		int mism = 0;
		for (uint16_t c = 0; c < blk.channels && !mism; c++)
			for (uint32_t n = 0; n < blk.frames; n++) {
				int32_t wire = samples[(size_t)c * blk.frames + n];
				if (wire != blk.planes[c][n]) {
					mism = 1;
					CHECK(0, "lossless: frame %d ch %u sample %u %d!=%d", fi, c, n,
					      wire, blk.planes[c][n]);
					break;
				}
			}
	}

	aius_dts_decoder_destroy(ref);
	aius_sim_device_destroy(&sim);
	free(data);
	printf("  lossless PCM transport: %d frame(s) bit-exact\n", limit);
}

/* ---- per-vector end-to-end over a fresh device ---- */

static void run_vector(const char *dir, const struct vector *v)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.dts", dir, v->name);
	size_t size = 0;
	uint8_t *data = read_file(path, &size);
	CHECK(data != NULL, "%s: cannot read", v->name);
	if (!data)
		return;

	size_t offs[MAX_FRAMES];
	int nframes = find_frames(data, size, offs, MAX_FRAMES);
	CHECK(nframes > 0 && nframes <= MAX_FRAMES, "%s: frame count %d", v->name,
	      nframes);
	if (nframes <= 0) {
		free(data);
		return;
	}

	static uint8_t mem[ARENA_BYTES];
	static AiusSimDevice sim;
	if (!sim_open(&sim, mem)) {
		CHECK(0, "%s: sim init", v->name);
		free(data);
		return;
	}
	AiusHostRunner run;
	aius_host_runner_bind_sim(&run, &sim);

	CHECK(aius_host_reset_metrics(&run) == 0, "%s: reset-metrics", v->name);

	/* submit-frame + request-PCM per access unit. */
	static int32_t samples[SAMPLES_CAP];
	int ok = 0;
	for (int fi = 0; fi < nframes; fi++) {
		size_t start = offs[fi];
		size_t end = (fi + 1 < nframes) ? offs[fi + 1] : size;
		uint32_t au_len = (uint32_t)(end - start);

		AiusCapFrameResult res;
		if (aius_host_submit_frame(&run, data + start, au_len, &res) != 0) {
			CHECK(0, "%s: submit frame %d", v->name, fi);
			break;
		}
		if (res.status != AIUS_OK) {
			CHECK(0, "%s: frame %d status %u", v->name, fi, res.status);
			break;
		}
		if (res.channels != v->channels || res.channel_mask != v->mask) {
			CHECK(0, "%s: frame %d ch=%u mask=0x%x", v->name, fi, res.channels,
			      res.channel_mask);
			break;
		}
		AiusCapPcmHeader hdr;
		uint32_t got = 0;
		if (aius_host_request_pcm(&run, &hdr, samples, SAMPLES_CAP, &got) != 0) {
			CHECK(0, "%s: request pcm %d", v->name, fi);
			break;
		}
		if (hdr.channels != res.channels || hdr.frames != res.frames) {
			CHECK(0, "%s: pcm header mismatch frame %d", v->name, fi);
			break;
		}
		ok++;
	}
	CHECK(ok == nframes, "%s: only %d/%d frames ok", v->name, ok, nframes);

	/* run-corpus: batch decode with streamed per-frame results + a summary. */
	static uint8_t batch[AIUS_CAP_MAX_PAYLOAD];
	const uint8_t *aus[MAX_FRAMES];
	uint32_t au_lens[MAX_FRAMES];
	for (int fi = 0; fi < nframes; fi++) {
		size_t start = offs[fi];
		size_t end = (fi + 1 < nframes) ? offs[fi + 1] : size;
		aus[fi] = data + start;
		au_lens[fi] = (uint32_t)(end - start);
	}
	size_t blen = aius_host_build_batch(batch, sizeof(batch), aus, au_lens,
	                                    (size_t)nframes);
	CHECK(blen > 0, "%s: batch build", v->name);

	AiusCapSummary sum;
	uint32_t streamed = 0;
	CHECK(aius_host_run_corpus(&run, batch, (uint32_t)blen, &sum, &streamed) == 0,
	      "%s: run-corpus", v->name);
	CHECK(streamed == (uint32_t)nframes, "%s: streamed %u != %d", v->name, streamed,
	      nframes);
	CHECK(sum.frames_ok == (uint32_t)nframes && sum.frames_failed == 0,
	      "%s: corpus ok=%u failed=%u", v->name, sum.frames_ok, sum.frames_failed);
	CHECK(sum.last_status == AIUS_OK, "%s: corpus last_status %u", v->name,
	      sum.last_status);
	CHECK(sum.peak_arena_high_water > 0 && sum.peak_arena_high_water < ARENA_BYTES,
	      "%s: corpus high-water %u", v->name, sum.peak_arena_high_water);

	/* run-soak: repeat the batch; steady state must allocate nothing. */
	const uint32_t iters = 8;
	AiusCapSummary soak;
	CHECK(aius_host_run_soak(&run, iters, batch, (uint32_t)blen, &soak) == 0,
	      "%s: run-soak", v->name);
	CHECK(soak.iterations == iters, "%s: soak iters %u", v->name, soak.iterations);
	CHECK(soak.frames_submitted == iters * (uint32_t)nframes,
	      "%s: soak submitted %u", v->name, soak.frames_submitted);
	CHECK(soak.frames_failed == 0, "%s: soak failed %u", v->name, soak.frames_failed);
	CHECK(soak.alloc_violations == 0,
	      "%s: soak steady-state allocations %u (want 0)", v->name,
	      soak.alloc_violations);

	printf("  %-20s frames=%d corpus_ok=%u soak(%ux)=%u hw=%u alloc_viol=%u\n",
	       v->name, nframes, sum.frames_ok, iters, soak.frames_submitted,
	       sum.peak_arena_high_water, soak.alloc_violations);

	aius_sim_device_destroy(&sim);
	free(data);
}

/* ---- identify + capabilities + audit transcript ---- */

static void test_identify_and_audit(void)
{
	static uint8_t mem[ARENA_BYTES];
	static AiusSimDevice sim;
	if (!sim_open(&sim, mem)) {
		CHECK(0, "identify: sim init");
		return;
	}
	AiusHostRunner run;
	aius_host_runner_bind_sim(&run, &sim);

	FILE *audit = tmpfile();
	aius_host_runner_set_audit(&run, audit);

	AiusCapCapabilities c;
	CHECK(aius_host_identify(&run, &c) == 0, "identify failed");
	CHECK(c.abi_version == AIUS_CODEC_ABI_VERSION, "abi %u", c.abi_version);
	CHECK(c.proto_version == AIUS_CAP_VERSION, "proto %u", c.proto_version);
	CHECK(c.max_channels == AIUS_MAX_CHANNELS, "max_channels %u", c.max_channels);
	CHECK(c.max_payload == AIUS_CAP_MAX_PAYLOAD, "max_payload %u", c.max_payload);
	CHECK((c.capability_flags &
	       (AIUS_CAP_FLAG_DTS_CORE | AIUS_CAP_FLAG_ARENA | AIUS_CAP_FLAG_SOAK)) ==
	          (AIUS_CAP_FLAG_DTS_CORE | AIUS_CAP_FLAG_ARENA | AIUS_CAP_FLAG_SOAK),
	      "flags 0x%x", c.capability_flags);
	CHECK(memcmp(c.name, "AIUS-DTS", 8) == 0, "name");

	/* request-PCM before any decode -> typed NO_PCM NACK. */
	AiusCapPcmHeader hdr;
	uint32_t got = 0;
	static int32_t samples[SAMPLES_CAP];
	CHECK(aius_host_request_pcm(&run, &hdr, samples, SAMPLES_CAP, &got) == -2 &&
	          run.last_error == AIUS_CAP_ERR_NO_PCM,
	      "no-pcm not reported (err=%d)", run.last_error);

	if (audit) {
		long pos = ftell(audit);
		CHECK(pos > 0, "audit transcript empty");
		fclose(audit);
	}

	aius_sim_device_destroy(&sim);
}

/* ---- partial + coalesced transfers ---- */

static void test_partial_transfer(const char *dir)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/stereo_tones.dts", dir);
	size_t size = 0;
	uint8_t *data = read_file(path, &size);
	CHECK(data != NULL, "partial: read");
	if (!data)
		return;
	size_t offs[MAX_FRAMES];
	int nframes = find_frames(data, size, offs, MAX_FRAMES);
	CHECK(nframes > 0, "partial: frames");
	if (nframes <= 0) {
		free(data);
		return;
	}

	static uint8_t mem[ARENA_BYTES];
	static AiusSimDevice sim;
	if (!sim_open(&sim, mem)) {
		CHECK(0, "partial: sim init");
		free(data);
		return;
	}
	AiusHostRunner run;
	aius_host_runner_bind_sim(&run, &sim);
	aius_host_runner_set_write_chunk(&run, 1); /* byte-at-a-time writes */

	AiusCapCapabilities c;
	CHECK(aius_host_identify(&run, &c) == 0, "partial: identify");

	size_t end = (nframes > 1) ? offs[1] : size;
	AiusCapFrameResult res;
	CHECK(aius_host_submit_frame(&run, data + offs[0], (uint32_t)(end - offs[0]),
	                             &res) == 0,
	      "partial: submit");
	CHECK(res.status == AIUS_OK && res.channels == 2, "partial: decode ch=%u",
	      res.channels);
	printf("  partial (1-byte writes): identify + submit reassembled\n");

	aius_sim_device_destroy(&sim);
	free(data);
}

/* ---- typed error responses over the wire ---- */

/* Write raw bytes to the sim, then parse the next response frame. Returns 1 on
 * a decoded frame. */
static int raw_exchange(AiusSimDevice *sim, const uint8_t *bytes, size_t len,
                        AiusCapFrame *out)
{
	aius_sim_device_write(sim, bytes, len);
	static AiusCapFramer fr;
	aius_cap_framer_init(&fr);
	uint8_t buf[4096];
	for (;;) {
		AiusCapFrameEvent ev = aius_cap_framer_next(&fr, out);
		if (ev != AIUS_CAP_FRAME_NONE)
			return ev == AIUS_CAP_FRAME_OK ? 1 : 2;
		size_t avail = aius_sim_device_available(sim);
		if (avail == 0)
			return 0;
		size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
		size_t got = aius_sim_device_read(sim, buf, want);
		aius_cap_framer_feed(&fr, buf, got);
	}
}

static void test_typed_errors(void)
{
	static uint8_t mem[ARENA_BYTES];
	static AiusSimDevice sim;
	uint8_t frame[128];
	uint8_t both[256];

	/* Duplicate sequence: two identifies with the same seq; second -> NACK. */
	if (sim_open(&sim, mem)) {
		size_t n = aius_cap_encode(frame, sizeof(frame), AIUS_CAP_CMD_IDENTIFY,
		                           0x4242, NULL, 0);
		memcpy(both, frame, n);
		memcpy(both + n, frame, n);
		aius_sim_device_write(&sim, both, 2 * n);
		uint8_t buf[512];
		size_t got = aius_sim_device_read(&sim, buf, sizeof(buf));
		AiusCapFramer fr;
		aius_cap_framer_init(&fr);
		aius_cap_framer_feed(&fr, buf, got);
		AiusCapFrame f1, f2;
		CHECK(aius_cap_framer_next(&fr, &f1) == AIUS_CAP_FRAME_OK &&
		          f1.msg_type == AIUS_CAP_RSP_CAPABILITIES,
		      "dup: first response");
		CHECK(aius_cap_framer_next(&fr, &f2) == AIUS_CAP_FRAME_OK &&
		          f2.msg_type == AIUS_CAP_RSP_NACK,
		      "dup: second not NACK");
		AiusCapNack nk;
		CHECK(aius_cap_get_nack(f2.payload, f2.payload_len, &nk) &&
		          nk.error_code == AIUS_CAP_ERR_DUP_SEQUENCE,
		      "dup: wrong error");
		aius_sim_device_destroy(&sim);
	}

	/* Unknown message type -> NACK. */
	if (sim_open(&sim, mem)) {
		size_t n = aius_cap_encode(frame, sizeof(frame), 0x7f, 5, NULL, 0);
		AiusCapFrame f;
		CHECK(raw_exchange(&sim, frame, n, &f) == 1 &&
		          f.msg_type == AIUS_CAP_RSP_NACK,
		      "unknown-type: no NACK");
		AiusCapNack nk;
		CHECK(aius_cap_get_nack(f.payload, f.payload_len, &nk) &&
		          nk.error_code == AIUS_CAP_ERR_UNKNOWN_TYPE,
		      "unknown-type: wrong error");
		aius_sim_device_destroy(&sim);
	}

	/* Corrupted CRC on the wire -> NACK(BAD_CRC). */
	if (sim_open(&sim, mem)) {
		size_t n = aius_cap_encode(frame, sizeof(frame), AIUS_CAP_CMD_IDENTIFY, 9,
		                           NULL, 0);
		frame[6] ^= 0xff; /* mutate the sequence, leaving the CRC stale */
		AiusCapFrame f;
		int e = raw_exchange(&sim, frame, n, &f);
		CHECK(e == 1 && f.msg_type == AIUS_CAP_RSP_NACK, "bad-crc: no NACK");
		AiusCapNack nk;
		CHECK(aius_cap_get_nack(f.payload, f.payload_len, &nk) &&
		          nk.error_code == AIUS_CAP_ERR_BAD_CRC,
		      "bad-crc: wrong error");
		aius_sim_device_destroy(&sim);
	}
	printf("  typed errors: dup-sequence, unknown-type, bad-CRC all NACKed\n");
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <vectors_dir>\n", argv[0]);
		return 2;
	}
	const char *dir = argv[1];

	printf("Aius capability round-trip suite (sim device + host runner)\n");
	test_identify_and_audit();
	test_lossless_pcm(dir);
	for (size_t i = 0; i < sizeof(k_vectors) / sizeof(k_vectors[0]); i++)
		run_vector(dir, &k_vectors[i]);
	test_partial_transfer(dir);
	test_typed_errors();

	if (g_failures) {
		printf("RESULT: %d check(s) FAILED\n", g_failures);
		return 1;
	}
	printf("RESULT: all checks passed\n");
	return 0;
}
