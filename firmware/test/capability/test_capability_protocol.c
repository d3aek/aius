/* Aius capability protocol suite (capability-gate phase P5): CRC, frame
 * encode/decode, the streaming framer's partial/coalesced/resync behaviour, and
 * byte-exact golden wire vectors asserted in both directions. No decoder.
 *
 * Usage:
 *   test_capability_protocol <vectors_dir>            assert against goldens
 *   test_capability_protocol --update <vectors_dir>   (re)write the goldens
 *
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_capability.h"
#include "aius_codec.h"

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

/* ---- deterministic golden messages ---- */

typedef size_t (*build_fn)(uint8_t *buf, size_t cap);

static size_t build_identify(uint8_t *buf, size_t cap)
{
	return aius_cap_encode(buf, cap, AIUS_CAP_CMD_IDENTIFY, 1, NULL, 0);
}

static size_t build_capabilities(uint8_t *buf, size_t cap)
{
	AiusCapCapabilities c = {
		.abi_version = 1,
		.proto_version = 1,
		.max_channels = 6,
		.max_payload = AIUS_CAP_MAX_PAYLOAD,
		.capability_flags =
			AIUS_CAP_FLAG_DTS_CORE | AIUS_CAP_FLAG_ARENA | AIUS_CAP_FLAG_SOAK,
	};
	memset(c.name, 0, sizeof(c.name));
	memcpy(c.name, "AIUS-DTS", 8);
	uint8_t p[AIUS_CAP_CAPABILITIES_LEN];
	size_t pl = aius_cap_put_capabilities(p, sizeof(p), &c);
	return aius_cap_encode(buf, cap, AIUS_CAP_RSP_CAPABILITIES, 1, p, (uint32_t)pl);
}

static size_t build_submit_frame(uint8_t *buf, size_t cap)
{
	static const uint8_t au[8] = { 0x7f, 0xfe, 0x80, 0x01, 0x00, 0x11, 0x22, 0x33 };
	return aius_cap_encode(buf, cap, AIUS_CAP_CMD_SUBMIT_FRAME, 2, au, sizeof(au));
}

static size_t build_frame_result(uint8_t *buf, size_t cap)
{
	AiusCapFrameResult r = {
		.status = AIUS_OK,
		.channels = 2,
		.frames = 512,
		.sample_rate_hz = 48000,
		.channel_mask = AIUS_CH_FL | AIUS_CH_FR,
		.arena_high_water = 60864,
		.alloc_count = 0,
	};
	uint8_t p[AIUS_CAP_FRAME_RESULT_LEN];
	size_t pl = aius_cap_put_frame_result(p, sizeof(p), &r);
	return aius_cap_encode(buf, cap, AIUS_CAP_RSP_FRAME_RESULT, 2, p, (uint32_t)pl);
}

static size_t build_pcm_data(uint8_t *buf, size_t cap)
{
	AiusCapPcmHeader h = {
		.channels = 2,
		.frames = 2,
		.sample_rate_hz = 48000,
		.channel_mask = AIUS_CH_FL | AIUS_CH_FR,
	};
	uint8_t p[AIUS_CAP_PCM_HEADER_LEN + 2 * 2 * 4];
	size_t pl = aius_cap_put_pcm_header(p, sizeof(p), &h);
	const int32_t samples[4] = { 1, -1, 0x00010203, -2 }; /* ch0{1,-1} ch1{...,-2} */
	for (int i = 0; i < 4; i++) {
		aius_cap_put_u32(p + pl, (uint32_t)samples[i]);
		pl += 4;
	}
	return aius_cap_encode(buf, cap, AIUS_CAP_RSP_PCM_DATA, 3, p, (uint32_t)pl);
}

static size_t build_ack(uint8_t *buf, size_t cap)
{
	return aius_cap_encode(buf, cap, AIUS_CAP_RSP_ACK, 4, NULL, 0);
}

static size_t build_summary(uint8_t *buf, size_t cap)
{
	AiusCapSummary s = {
		.frames_submitted = 49,
		.frames_ok = 49,
		.frames_failed = 0,
		.iterations = 1,
		.total_samples = 49u * 512u * 2u,
		.peak_arena_high_water = 60864,
		.alloc_violations = 0,
		.last_status = AIUS_OK,
	};
	uint8_t p[AIUS_CAP_SUMMARY_LEN];
	size_t pl = aius_cap_put_summary(p, sizeof(p), &s);
	return aius_cap_encode(buf, cap, AIUS_CAP_RSP_SUMMARY, 5, p, (uint32_t)pl);
}

static size_t build_diagnostic(uint8_t *buf, size_t cap)
{
	AiusCapDiagnostic d = {
		.error_code = AIUS_CAP_ERR_DECODE,
		.detail_status = AIUS_MALFORMED,
		.context_index = 3,
	};
	static const char msg[] = "soak decode error";
	uint8_t p[AIUS_CAP_DIAGNOSTIC_HEADER_LEN + 64];
	size_t pl = aius_cap_put_diagnostic(p, sizeof(p), &d, msg, (uint16_t)strlen(msg));
	return aius_cap_encode(buf, cap, AIUS_CAP_RSP_DIAGNOSTIC, 6, p, (uint32_t)pl);
}

static size_t build_nack(uint8_t *buf, size_t cap)
{
	AiusCapNack n = { .error_code = AIUS_CAP_ERR_BAD_CRC, .offending_sequence = 7 };
	uint8_t p[AIUS_CAP_NACK_LEN];
	size_t pl = aius_cap_put_nack(p, sizeof(p), &n);
	return aius_cap_encode(buf, cap, AIUS_CAP_RSP_NACK, 7, p, (uint32_t)pl);
}

struct golden {
	const char *name;
	build_fn    build;
	uint8_t     msg_type;
	uint16_t    seq;
};

static const struct golden k_goldens[] = {
	{ "identify_cmd", build_identify, AIUS_CAP_CMD_IDENTIFY, 1 },
	{ "capabilities", build_capabilities, AIUS_CAP_RSP_CAPABILITIES, 1 },
	{ "submit_frame", build_submit_frame, AIUS_CAP_CMD_SUBMIT_FRAME, 2 },
	{ "frame_result", build_frame_result, AIUS_CAP_RSP_FRAME_RESULT, 2 },
	{ "pcm_data", build_pcm_data, AIUS_CAP_RSP_PCM_DATA, 3 },
	{ "ack", build_ack, AIUS_CAP_RSP_ACK, 4 },
	{ "summary", build_summary, AIUS_CAP_RSP_SUMMARY, 5 },
	{ "diagnostic", build_diagnostic, AIUS_CAP_RSP_DIAGNOSTIC, 6 },
	{ "nack", build_nack, AIUS_CAP_RSP_NACK, 7 },
};

static const size_t k_golden_count = sizeof(k_goldens) / sizeof(k_goldens[0]);

/* ---- CRC known-answer ---- */

static void test_crc(void)
{
	/* Standard CRC-32/ISO-HDLC check value for the ASCII "123456789". */
	uint32_t crc = aius_cap_crc32((const uint8_t *)"123456789", 9);
	CHECK(crc == 0xCBF43926u, "crc check value 0x%08x != 0xCBF43926", crc);
	CHECK(aius_cap_crc32(NULL, 0) == 0u, "crc of empty must be 0 (init^xorout)");
}

/* ---- framer behaviour ---- */

static int framer_expect_one(const uint8_t *bytes, size_t len, AiusCapFrame *out)
{
	static AiusCapFramer fr;
	aius_cap_framer_init(&fr);
	size_t off = 0;
	while (off < len)
		off += aius_cap_framer_feed(&fr, bytes + off, len - off);
	AiusCapFrameEvent ev = aius_cap_framer_next(&fr, out);
	return ev == AIUS_CAP_FRAME_OK ? 1 : 0;
}

static void test_framer_partial(void)
{
	uint8_t frame[128];
	size_t n = build_frame_result(frame, sizeof(frame));
	static AiusCapFramer fr;
	aius_cap_framer_init(&fr);
	AiusCapFrame f;
	/* Byte-by-byte: no complete frame until the final byte lands. */
	for (size_t i = 0; i < n; i++) {
		aius_cap_framer_feed(&fr, frame + i, 1);
		AiusCapFrameEvent ev = aius_cap_framer_next(&fr, &f);
		if (i + 1 < n)
			CHECK(ev == AIUS_CAP_FRAME_NONE, "premature frame at byte %zu", i);
		else
			CHECK(ev == AIUS_CAP_FRAME_OK, "no frame after last byte");
	}
	CHECK(f.msg_type == AIUS_CAP_RSP_FRAME_RESULT, "partial: wrong type");
}

static void test_framer_coalesced(void)
{
	uint8_t buf[256];
	size_t a = build_identify(buf, sizeof(buf));
	size_t b = build_ack(buf + a, sizeof(buf) - a);
	static AiusCapFramer fr;
	aius_cap_framer_init(&fr);
	size_t off = 0, total = a + b;
	while (off < total)
		off += aius_cap_framer_feed(&fr, buf + off, total - off);
	AiusCapFrame f1, f2, f3;
	CHECK(aius_cap_framer_next(&fr, &f1) == AIUS_CAP_FRAME_OK, "coalesced #1");
	CHECK(f1.msg_type == AIUS_CAP_CMD_IDENTIFY, "coalesced #1 type");
	CHECK(aius_cap_framer_next(&fr, &f2) == AIUS_CAP_FRAME_OK, "coalesced #2");
	CHECK(f2.msg_type == AIUS_CAP_RSP_ACK, "coalesced #2 type");
	CHECK(aius_cap_framer_next(&fr, &f3) == AIUS_CAP_FRAME_NONE, "coalesced drained");
}

static void test_framer_garbage_prefix(void)
{
	uint8_t buf[256];
	static const uint8_t junk[] = { 0xde, 0xad, 0xbe, 0x41, 0x00 };
	memcpy(buf, junk, sizeof(junk));
	size_t n = build_identify(buf + sizeof(junk), sizeof(buf) - sizeof(junk));
	AiusCapFrame f;
	CHECK(framer_expect_one(buf, sizeof(junk) + n, &f), "garbage prefix: no frame");
	CHECK(f.msg_type == AIUS_CAP_CMD_IDENTIFY, "garbage prefix: wrong type");
}

static void test_framer_bad_crc_resync(void)
{
	uint8_t buf[256];
	size_t a = build_frame_result(buf, sizeof(buf));
	buf[AIUS_CAP_HEADER_LEN + 3] ^= 0xff; /* corrupt a payload byte */
	size_t b = build_ack(buf + a, sizeof(buf) - a);
	static AiusCapFramer fr;
	aius_cap_framer_init(&fr);
	size_t off = 0, total = a + b;
	while (off < total)
		off += aius_cap_framer_feed(&fr, buf + off, total - off);
	AiusCapFrame f;
	AiusCapFrameEvent ev = aius_cap_framer_next(&fr, &f);
	CHECK(ev == AIUS_CAP_FRAME_ERROR && f.error == AIUS_CAP_ERR_BAD_CRC,
	      "bad CRC not reported (ev=%d err=%d)", ev, f.error);
	/* Resync: the following valid frame is still recovered. */
	int found = 0;
	for (int i = 0; i < 4; i++) {
		ev = aius_cap_framer_next(&fr, &f);
		if (ev == AIUS_CAP_FRAME_OK && f.msg_type == AIUS_CAP_RSP_ACK) {
			found = 1;
			break;
		}
		if (ev == AIUS_CAP_FRAME_NONE)
			break;
	}
	CHECK(found, "bad CRC: failed to resync to following frame");
}

static void test_framer_bad_version(void)
{
	uint8_t buf[128];
	size_t n = build_identify(buf, sizeof(buf));
	uint32_t plen = aius_cap_get_u32(buf + 8);
	buf[4] = 2; /* wrong version */
	uint32_t crc = aius_cap_crc32(buf, AIUS_CAP_HEADER_LEN + plen);
	aius_cap_put_u32(buf + AIUS_CAP_HEADER_LEN + plen, crc);
	static AiusCapFramer fr;
	aius_cap_framer_init(&fr);
	aius_cap_framer_feed(&fr, buf, n);
	AiusCapFrame f;
	AiusCapFrameEvent ev = aius_cap_framer_next(&fr, &f);
	CHECK(ev == AIUS_CAP_FRAME_ERROR && f.error == AIUS_CAP_ERR_BAD_VERSION,
	      "bad version not reported (ev=%d err=%d)", ev, f.error);
}

static void test_framer_oversized_length(void)
{
	uint8_t buf[256];
	/* A spurious magic with an impossible length must be skipped, and a real
	 * frame after it recovered. */
	buf[0] = AIUS_CAP_MAGIC0;
	buf[1] = AIUS_CAP_MAGIC1;
	buf[2] = AIUS_CAP_MAGIC2;
	buf[3] = AIUS_CAP_MAGIC3;
	buf[4] = AIUS_CAP_VERSION;
	buf[5] = 0x00;
	aius_cap_put_u16(buf + 6, 0);
	aius_cap_put_u32(buf + 8, 0xffffffffu); /* > MAX_PAYLOAD */
	size_t n = build_ack(buf + 12, sizeof(buf) - 12);
	AiusCapFrame f;
	CHECK(framer_expect_one(buf, 12 + n, &f), "oversized length: no recovery");
	CHECK(f.msg_type == AIUS_CAP_RSP_ACK, "oversized length: wrong type");
}

static void test_framer_timeout_partial(void)
{
	/* A truncated frame never completes: the framer reports NONE and holds the
	 * bytes as pending, which the host-side timeout keys off. */
	uint8_t frame[128];
	size_t n = build_frame_result(frame, sizeof(frame));
	static AiusCapFramer fr;
	aius_cap_framer_init(&fr);
	aius_cap_framer_feed(&fr, frame, n - 4); /* drop the CRC */
	AiusCapFrame f;
	CHECK(aius_cap_framer_next(&fr, &f) == AIUS_CAP_FRAME_NONE,
	      "truncated frame must not complete");
	CHECK(aius_cap_framer_pending(&fr) == n - 4, "pending byte count wrong");
}

/* ---- golden vectors ---- */

static uint8_t *read_file(const char *path, size_t *len_out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (n < 0) {
		fclose(fp);
		return NULL;
	}
	uint8_t *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, fp);
	fclose(fp);
	if (got != (size_t)n) {
		free(buf);
		return NULL;
	}
	*len_out = got;
	return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return -1;
	size_t w = fwrite(data, 1, len, fp);
	fclose(fp);
	return w == len ? 0 : -1;
}

static int update_goldens(const char *dir)
{
	uint8_t buf[4096];
	for (size_t i = 0; i < k_golden_count; i++) {
		size_t n = k_goldens[i].build(buf, sizeof(buf));
		if (n == 0) {
			fprintf(stderr, "build %s failed\n", k_goldens[i].name);
			return 1;
		}
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s.bin", dir, k_goldens[i].name);
		if (write_file(path, buf, n) != 0) {
			fprintf(stderr, "write %s failed\n", path);
			return 1;
		}
		printf("  wrote %s (%zu bytes)\n", path, n);
	}
	return 0;
}

static void assert_goldens(const char *dir)
{
	uint8_t buf[4096];
	for (size_t i = 0; i < k_golden_count; i++) {
		const struct golden *g = &k_goldens[i];
		size_t n = g->build(buf, sizeof(buf));
		CHECK(n != 0, "%s: build failed", g->name);

		char path[1024];
		snprintf(path, sizeof(path), "%s/%s.bin", dir, g->name);
		size_t flen = 0;
		uint8_t *fdata = read_file(path, &flen);
		CHECK(fdata != NULL, "%s: cannot read %s", g->name, path);
		if (!fdata)
			continue;

		/* Direction 1: our encoder reproduces the committed bytes exactly. */
		CHECK(flen == n && memcmp(fdata, buf, n) == 0,
		      "%s: encoded bytes differ from golden (n=%zu flen=%zu)", g->name, n,
		      flen);

		/* Direction 2: the framer decodes the committed bytes to the right frame. */
		AiusCapFrame f;
		CHECK(framer_expect_one(fdata, flen, &f), "%s: golden did not decode",
		      g->name);
		CHECK(f.msg_type == g->msg_type, "%s: decoded type 0x%02x != 0x%02x",
		      g->name, f.msg_type, g->msg_type);
		CHECK(f.sequence == g->seq, "%s: decoded seq %u != %u", g->name,
		      f.sequence, g->seq);
		free(fdata);
	}

	/* Spot-check typed getters straight off a committed golden. */
	char path[1024];
	snprintf(path, sizeof(path), "%s/capabilities.bin", dir);
	size_t flen = 0;
	uint8_t *fdata = read_file(path, &flen);
	if (fdata) {
		AiusCapFrame f;
		if (framer_expect_one(fdata, flen, &f)) {
			AiusCapCapabilities c;
			CHECK(aius_cap_get_capabilities(f.payload, f.payload_len, &c),
			      "capabilities getter failed");
			CHECK(c.max_channels == 6 && c.max_payload == AIUS_CAP_MAX_PAYLOAD,
			      "capabilities fields wrong");
			CHECK(memcmp(c.name, "AIUS-DTS", 8) == 0, "capabilities name wrong");
		}
		free(fdata);
	}
}

/* ---- payload round-trip (both directions, no files) ---- */

static void test_payload_roundtrip(void)
{
	uint8_t p[256];

	AiusCapSummary s = { 3, 3, 0, 2, 6144, 60864, 0, AIUS_OK }, s2;
	size_t n = aius_cap_put_summary(p, sizeof(p), &s);
	CHECK(n == AIUS_CAP_SUMMARY_LEN, "summary put len");
	CHECK(aius_cap_get_summary(p, (uint32_t)n, &s2), "summary get");
	CHECK(s2.total_samples == 6144 && s2.iterations == 2 && s2.frames_ok == 3,
	      "summary round-trip");

	AiusCapDiagnostic d = { AIUS_CAP_ERR_DECODE, AIUS_TRUNCATED, 9 }, d2;
	n = aius_cap_put_diagnostic(p, sizeof(p), &d, "x", 1);
	const char *m = NULL;
	uint16_t ml = 0;
	CHECK(aius_cap_get_diagnostic(p, (uint32_t)n, &d2, &m, &ml), "diag get");
	CHECK(d2.context_index == 9 && ml == 1 && m && m[0] == 'x',
	      "diagnostic round-trip");

	/* Short payloads are rejected. */
	AiusCapNack nk;
	CHECK(!aius_cap_get_nack(p, 3, &nk), "short nack must be rejected");
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--update") == 0) {
		printf("capability-protocol: writing golden vectors\n");
		return update_goldens(argv[2]);
	}
	if (argc != 2) {
		fprintf(stderr, "usage: %s [--update] <vectors_dir>\n", argv[0]);
		return 2;
	}

	printf("capability-protocol suite\n");
	test_crc();
	test_framer_partial();
	test_framer_coalesced();
	test_framer_garbage_prefix();
	test_framer_bad_crc_resync();
	test_framer_bad_version();
	test_framer_oversized_length();
	test_framer_timeout_partial();
	test_payload_roundtrip();
	assert_goldens(argv[1]);

	if (g_failures) {
		printf("RESULT: %d check(s) FAILED\n", g_failures);
		return 1;
	}
	printf("RESULT: all checks passed\n");
	return 0;
}
