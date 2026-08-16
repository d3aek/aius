/* Host suite for lib/aius_ota: the OTA image container, the staged-upgrade
 * state machine, the power-loss record, the image-health rules and the
 * SHA-256 the first three are built on.
 *
 * Everything is driven through the public interface. The one deliberate
 * exception is `reseal()`, which recomputes a header's CRC-32 after a
 * wire-level mutation the parsed struct cannot express (a non-zero reserved
 * field, an unterminated string). It carries its own CRC copy exactly as
 * test/boot/test_boot.c does, so the suite can prove the field rules and the
 * CRC rule independently instead of having every mutation stopped at the CRC.
 */

#include "aius_ota.h"
#include "aius_ota_health.h"
#include "aius_ota_staging.h"
#include "aius_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define CHECK(condition) check((condition), #condition, __LINE__)

static const char *current_key = "startup";

static void check(int condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "ota oracle mismatch [%s] line %d: %s\n", current_key,
	        line, expression);
	exit(1);
}

/* -------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------- */

#define TEST_TARGET_ID "teensy41-optical"
#define TEST_REVISION "7a66153703d1"
#define TEST_BUILD_UTC UINT64_C(1753900000) /* 2025-07-30, inside the window */
#define TEST_IMAGE_BYTES 5000u

static uint8_t g_payload[TEST_IMAGE_BYTES];
static uint8_t g_other_payload[TEST_IMAGE_BYTES];

static void fill_payload(uint8_t *payload, uint32_t length, uint8_t seed)
{
	uint32_t index;

	for (index = 0u; index < length; index++)
		payload[index] = (uint8_t)((index * 31u + seed) & 0xffu);
}

static void to_hex(const uint8_t *bytes, size_t length, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	for (index = 0u; index < length; index++) {
		out[index * 2u] = digits[bytes[index] >> 4];
		out[index * 2u + 1u] = digits[bytes[index] & 0x0fu];
	}
	out[length * 2u] = '\0';
}

/* The test's own CRC-32, used only by reseal(). Same reflected polynomial the
 * library and lib/aius_boot use. */
static uint32_t test_crc32(const uint8_t *bytes, size_t length)
{
	uint32_t crc = UINT32_MAX;
	size_t index;

	for (index = 0u; index < length; index++) {
		unsigned int bit;
		crc ^= bytes[index];
		for (bit = 0u; bit < 8u; bit++) {
			uint32_t mask = 0u - (crc & 1u);
			crc = (crc >> 1) ^ (0xedb88320u & mask);
		}
	}
	return crc ^ UINT32_MAX;
}

static void reseal(uint8_t *wire)
{
	uint32_t crc = test_crc32(wire, AIUS_OTA_HEADER_OFFSET_CRC32);

	wire[AIUS_OTA_HEADER_OFFSET_CRC32] = (uint8_t)(crc & 0xffu);
	wire[AIUS_OTA_HEADER_OFFSET_CRC32 + 1u] = (uint8_t)((crc >> 8) & 0xffu);
	wire[AIUS_OTA_HEADER_OFFSET_CRC32 + 2u] =
		(uint8_t)((crc >> 16) & 0xffu);
	wire[AIUS_OTA_HEADER_OFFSET_CRC32 + 3u] = (uint8_t)(crc >> 24);
}

static AiusOtaImageHeader valid_header_struct(const uint8_t *payload,
	                                      uint32_t image_length)
{
	AiusOtaImageHeader header;

	memset(&header, 0, sizeof(header));
	header.magic = AIUS_OTA_MAGIC;
	header.format_version = AIUS_OTA_FORMAT_VERSION;
	header.header_length = (uint16_t)AIUS_OTA_HEADER_BYTES;
	header.image_length = image_length;
	header.signature_scheme = AIUS_OTA_SIGSCHEME_ED25519;
	header.build_utc = TEST_BUILD_UTC;
	memcpy(header.target_id, TEST_TARGET_ID, sizeof(TEST_TARGET_ID));
	memcpy(header.build_revision, TEST_REVISION, sizeof(TEST_REVISION));
	aius_sha256(payload, image_length, header.payload_digest);
	memset(header.signature, 0xa5, sizeof(header.signature));
	return header;
}

static void build_wire(uint8_t *wire, const uint8_t *payload,
	               uint32_t image_length)
{
	AiusOtaImageHeader header = valid_header_struct(payload, image_length);

	CHECK(aius_ota_header_serialize(&header, wire,
	                                AIUS_OTA_HEADER_BYTES) == AIUS_OTA_OK);
}

typedef struct {
	unsigned int calls;
	int result;
	uint8_t last_digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t last_signature[AIUS_OTA_SIGNATURE_BYTES];
} FakeVerifier;

static int fake_verify(const uint8_t digest[AIUS_OTA_DIGEST_BYTES],
	               const uint8_t signature[AIUS_OTA_SIGNATURE_BYTES],
	               void *ctx)
{
	FakeVerifier *fake = (FakeVerifier *)ctx;

	fake->calls++;
	memcpy(fake->last_digest, digest, AIUS_OTA_DIGEST_BYTES);
	memcpy(fake->last_signature, signature, AIUS_OTA_SIGNATURE_BYTES);
	return fake->result;
}

static AiusOtaVerifier verifier_over(FakeVerifier *fake)
{
	AiusOtaVerifier verifier;

	verifier.ctx = fake;
	verifier.verify = fake_verify;
	return verifier;
}

static void armed_session(AiusOtaSession *session)
{
	CHECK(aius_ota_init(session, TEST_TARGET_ID, strlen(TEST_TARGET_ID)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_latch_arm(session) == AIUS_OTA_OK);
}

/* Absorbs `payload` in `chunk` sized pieces from the session's next offset. */
static void stage_all(AiusOtaSession *session, const uint8_t *payload,
	              uint32_t image_length, uint32_t chunk)
{
	uint32_t offset = aius_ota_next_offset(session);

	while (offset < image_length) {
		uint32_t take = image_length - offset;
		if (take > chunk)
			take = chunk;
		CHECK(aius_ota_absorb(session, offset, &payload[offset],
		                      take) == AIUS_OTA_OK);
		offset += take;
		CHECK(aius_ota_next_offset(session) == offset);
	}
}

/* -------------------------------------------------------------------------
 * SHA-256
 * ---------------------------------------------------------------------- */

static void test_sha256_fips_vectors(void)
{
	static const char abc[] = "abc";
	static const char two_block[] =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	static const char multi_block[] =
		"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
		"hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
	uint8_t digest[AIUS_OTA_DIGEST_BYTES];
	char hex[AIUS_OTA_DIGEST_BYTES * 2u + 1u];
	AiusSha256 context;
	unsigned int index;

	current_key = "sha256.fips-empty";
	aius_sha256((const uint8_t *)"", 0u, digest);
	to_hex(digest, sizeof(digest), hex);
	CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb924"
	                  "27ae41e4649b934ca495991b7852b855") == 0);

	current_key = "sha256.fips-abc";
	aius_sha256((const uint8_t *)abc, strlen(abc), digest);
	to_hex(digest, sizeof(digest), hex);
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223"
	                  "b00361a396177a9cb410ff61f20015ad") == 0);

	/* 448 bits: the message that pads into a second block. */
	current_key = "sha256.fips-two-block";
	CHECK(strlen(two_block) == 56u);
	aius_sha256((const uint8_t *)two_block, strlen(two_block), digest);
	to_hex(digest, sizeof(digest), hex);
	CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039"
	                  "a33ce45964ff2167f6ecedd419db06c1") == 0);

	current_key = "sha256.fips-multi-block";
	CHECK(strlen(multi_block) == 112u);
	aius_sha256((const uint8_t *)multi_block, strlen(multi_block), digest);
	to_hex(digest, sizeof(digest), hex);
	CHECK(strcmp(hex, "cf5b16a778af8380036ce59e7b049237"
	                  "0b249b11e8f07a51afac45037afee9d1") == 0);

	/* One million 'a', fed 1000 at a time -- the long-message vector, and
	 * the only one that exercises the streaming path at scale. */
	current_key = "sha256.fips-million-a";
	aius_sha256_init(&context);
	for (index = 0u; index < 1000u; index++) {
		uint8_t block[1000];
		memset(block, 'a', sizeof(block));
		aius_sha256_update(&context, block, sizeof(block));
	}
	aius_sha256_final(&context, digest);
	to_hex(digest, sizeof(digest), hex);
	CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67"
	                  "f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_sha256_incremental_matches_one_shot(void)
{
	static const uint32_t splits[] = {1u, 2u, 31u, 63u, 64u, 65u, 127u,
	                                  128u, 512u, 4096u};
	uint8_t expected[AIUS_OTA_DIGEST_BYTES];
	size_t index;

	current_key = "sha256.incremental";
	aius_sha256(g_payload, TEST_IMAGE_BYTES, expected);

	for (index = 0u; index < ARRAY_COUNT(splits); index++) {
		AiusSha256 context;
		uint8_t digest[AIUS_OTA_DIGEST_BYTES];
		uint32_t offset = 0u;

		aius_sha256_init(&context);
		while (offset < TEST_IMAGE_BYTES) {
			uint32_t take = TEST_IMAGE_BYTES - offset;
			if (take > splits[index])
				take = splits[index];
			aius_sha256_update(&context, &g_payload[offset], take);
			offset += take;
		}
		aius_sha256_final(&context, digest);
		CHECK(memcmp(digest, expected, sizeof(digest)) == 0);
	}

	/* A struct copy is a valid snapshot: the interface promises it, and
	 * aius_ota_verify() depends on it. */
	current_key = "sha256.snapshot";
	{
		AiusSha256 context;
		AiusSha256 snapshot;
		uint8_t from_copy[AIUS_OTA_DIGEST_BYTES];
		uint8_t from_original[AIUS_OTA_DIGEST_BYTES];

		aius_sha256_init(&context);
		aius_sha256_update(&context, g_payload, TEST_IMAGE_BYTES);
		snapshot = context;
		aius_sha256_final(&snapshot, from_copy);
		aius_sha256_final(&context, from_original);
		CHECK(memcmp(from_copy, expected, sizeof(from_copy)) == 0);
		CHECK(memcmp(from_original, expected,
		             sizeof(from_original)) == 0);
	}
}

/* -------------------------------------------------------------------------
 * Container header
 * ---------------------------------------------------------------------- */

static void test_header_round_trip(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	AiusOtaImageHeader source;
	AiusOtaImageHeader parsed;
	uint8_t expected_digest[AIUS_OTA_DIGEST_BYTES];

	current_key = "header.round-trip";
	source = valid_header_struct(g_payload, TEST_IMAGE_BYTES);
	CHECK(aius_ota_header_serialize(&source, wire, sizeof(wire)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_OK);

	CHECK(parsed.magic == AIUS_OTA_MAGIC);
	CHECK(parsed.format_version == AIUS_OTA_FORMAT_VERSION);
	CHECK(parsed.header_length == AIUS_OTA_HEADER_BYTES);
	CHECK(parsed.image_length == TEST_IMAGE_BYTES);
	CHECK(parsed.signature_scheme == AIUS_OTA_SIGSCHEME_ED25519);
	CHECK(parsed.build_utc == TEST_BUILD_UTC);
	CHECK(strcmp(parsed.target_id, TEST_TARGET_ID) == 0);
	CHECK(strcmp(parsed.build_revision, TEST_REVISION) == 0);
	aius_sha256(g_payload, TEST_IMAGE_BYTES, expected_digest);
	CHECK(memcmp(parsed.payload_digest, expected_digest,
	             sizeof(expected_digest)) == 0);
	CHECK(memcmp(parsed.signature, source.signature,
	             sizeof(source.signature)) == 0);
	CHECK(parsed.header_crc32 ==
	      test_crc32(wire, AIUS_OTA_HEADER_OFFSET_CRC32));

	/* The magic reads "AIUO" in a hex dump, which is the whole point of
	 * choosing it. */
	CHECK(wire[0] == 'A' && wire[1] == 'I' && wire[2] == 'U' &&
	      wire[3] == 'O');

	current_key = "header.arguments";
	CHECK(aius_ota_header_parse(NULL, wire, sizeof(wire)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_parse(&parsed, NULL, sizeof(wire)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire) - 1u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire) + 1u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_serialize(&source, wire, sizeof(wire) - 1u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_serialize(NULL, wire, sizeof(wire)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_serialize(&source, NULL, sizeof(wire)) ==
	      AIUS_OTA_INVALID_ARG);

	/* An unterminated string field is a bad argument, not a read past the
	 * end of the array. */
	current_key = "header.unterminated";
	source = valid_header_struct(g_payload, TEST_IMAGE_BYTES);
	memset(source.target_id, 'x', sizeof(source.target_id));
	CHECK(aius_ota_header_serialize(&source, wire, sizeof(wire)) ==
	      AIUS_OTA_INVALID_ARG);
	source = valid_header_struct(g_payload, TEST_IMAGE_BYTES);
	memset(source.build_revision, 'x', sizeof(source.build_revision));
	CHECK(aius_ota_header_serialize(&source, wire, sizeof(wire)) ==
	      AIUS_OTA_INVALID_ARG);
}

/* Field rules expressible in the struct: mutate, re-serialize (which reseals
 * the CRC), and expect the named refusal. */
static void test_header_rejects_struct_fields(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	AiusOtaImageHeader header;
	AiusOtaImageHeader parsed;

#define REJECTS(key_name, mutation, status)                                   \
	do {                                                                   \
		current_key = key_name;                                        \
		header = valid_header_struct(g_payload, TEST_IMAGE_BYTES);     \
		mutation;                                                      \
		CHECK(aius_ota_header_serialize(&header, wire, sizeof(wire)) ==\
		      AIUS_OTA_OK);                                            \
		CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==    \
		      (status));                                               \
	} while (0)

	REJECTS("header.magic", header.magic = 0x41495542u, AIUS_OTA_MALFORMED);
	REJECTS("header.magic-zero", header.magic = 0u, AIUS_OTA_MALFORMED);
	REJECTS("header.version-0", header.format_version = 0u,
	        AIUS_OTA_UNSUPPORTED);
	REJECTS("header.version-2", header.format_version = 2u,
	        AIUS_OTA_UNSUPPORTED);
	REJECTS("header.header-length", header.header_length = 128u,
	        AIUS_OTA_MALFORMED);
	REJECTS("header.image-length-zero", header.image_length = 0u,
	        AIUS_OTA_MALFORMED);
	REJECTS("header.image-length-max",
	        header.image_length = AIUS_OTA_MAX_IMAGE_BYTES + 1u,
	        AIUS_OTA_UNSUPPORTED);
	REJECTS("header.scheme-none",
	        header.signature_scheme = AIUS_OTA_SIGSCHEME_NONE,
	        AIUS_OTA_MALFORMED);
	REJECTS("header.scheme-unknown", header.signature_scheme = 9u,
	        AIUS_OTA_UNSUPPORTED);
	REJECTS("header.build-utc-zero", header.build_utc = 0u,
	        AIUS_OTA_MALFORMED);
	REJECTS("header.build-utc-early",
	        header.build_utc = AIUS_OTA_MIN_BUILD_UTC - 1u,
	        AIUS_OTA_MALFORMED);
	REJECTS("header.build-utc-late",
	        header.build_utc = AIUS_OTA_MAX_BUILD_UTC,
	        AIUS_OTA_MALFORMED);
	REJECTS("header.digest-zero",
	        memset(header.payload_digest, 0, sizeof(header.payload_digest)),
	        AIUS_OTA_MALFORMED);
	REJECTS("header.signature-zero",
	        memset(header.signature, 0, sizeof(header.signature)),
	        AIUS_OTA_MALFORMED);
	REJECTS("header.target-empty", header.target_id[0] = '\0',
	        AIUS_OTA_MALFORMED);
	REJECTS("header.revision-empty", header.build_revision[0] = '\0',
	        AIUS_OTA_MALFORMED);

	/* Boundaries that must be ACCEPTED, so the bounds are proven exclusive
	 * on one side and inclusive on the other. */
	REJECTS("header.image-length-one", header.image_length = 1u,
	        AIUS_OTA_OK);
	REJECTS("header.image-length-at-max",
	        header.image_length = AIUS_OTA_MAX_IMAGE_BYTES, AIUS_OTA_OK);
	REJECTS("header.build-utc-min", header.build_utc =
		        AIUS_OTA_MIN_BUILD_UTC, AIUS_OTA_OK);
	REJECTS("header.build-utc-last",
	        header.build_utc = AIUS_OTA_MAX_BUILD_UTC - 1u, AIUS_OTA_OK);
	REJECTS("header.target-full",
	        memset(header.target_id, 'z', AIUS_OTA_TARGET_ID_BYTES),
	        AIUS_OTA_OK);

#undef REJECTS
}

/* Rules that only exist on the wire. Each mutation is resealed, so the CRC
 * cannot mask the field rule under test; the last case proves the CRC rule
 * itself by NOT resealing. */
static void test_header_rejects_wire_fields(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	AiusOtaImageHeader parsed;

	current_key = "header.reserved0";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_RESERVED0] = 1u;
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	current_key = "header.reserved1-first";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_RESERVED1] = 0x01u;
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	current_key = "header.reserved1-last";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_RESERVED1 +
	     AIUS_OTA_HEADER_RESERVED1_BYTES - 1u] = 0x80u;
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	current_key = "header.target-unprintable";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_TARGET_ID + 2u] = 0x1fu;
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	current_key = "header.target-high-byte";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_TARGET_ID + 2u] = 0x80u;
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	/* Bytes hiding past the terminator: the field must be NUL-PADDED, not
	 * merely NUL-terminated, or two headers could differ in bytes the
	 * signature covers while presenting the same string. */
	current_key = "header.target-tail-garbage";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_TARGET_ID +
	     AIUS_OTA_TARGET_ID_BYTES - 1u] = 'x';
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	current_key = "header.revision-tail-garbage";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_BUILD_REVISION +
	     AIUS_OTA_BUILD_REVISION_BYTES - 1u] = 'x';
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	current_key = "header.revision-unprintable";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_BUILD_REVISION + 1u] = 0x7fu;
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);

	/* No reseal: a single flipped payload-digest byte must be caught by the
	 * CRC, which is what makes every case above a real field test. */
	current_key = "header.crc";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_PAYLOAD_DIGEST] ^= 0x01u;
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);
	reseal(wire);
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_OK);

	current_key = "header.crc-field";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_CRC32 + 3u] ^= 0x40u;
	CHECK(aius_ota_header_parse(&parsed, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);
}

static void test_header_auth_digest_binds_the_prefix(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t baseline[AIUS_OTA_DIGEST_BYTES];
	uint8_t expected[AIUS_OTA_DIGEST_BYTES];

	current_key = "header.auth-digest";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), baseline) ==
	      AIUS_OTA_OK);
	aius_sha256(wire, AIUS_OTA_HEADER_AUTH_BYTES, expected);
	CHECK(memcmp(baseline, expected, sizeof(expected)) == 0);

	CHECK(aius_ota_header_auth_digest(NULL, sizeof(wire), digest) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), NULL) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire) - 1u, digest) ==
	      AIUS_OTA_INVALID_ARG);

	/* Every identity claim is inside the signed prefix ... */
	current_key = "header.auth-digest-covers-identity";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_TARGET_ID] = 'X';
	reseal(wire);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), digest) ==
	      AIUS_OTA_OK);
	CHECK(memcmp(digest, baseline, sizeof(digest)) != 0);

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_BUILD_UTC] ^= 0x01u;
	reseal(wire);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), digest) ==
	      AIUS_OTA_OK);
	CHECK(memcmp(digest, baseline, sizeof(digest)) != 0);

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_IMAGE_LENGTH] ^= 0x01u;
	reseal(wire);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), digest) ==
	      AIUS_OTA_OK);
	CHECK(memcmp(digest, baseline, sizeof(digest)) != 0);

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_PAYLOAD_DIGEST + 31u] ^= 0x01u;
	reseal(wire);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), digest) ==
	      AIUS_OTA_OK);
	CHECK(memcmp(digest, baseline, sizeof(digest)) != 0);

	/* ... and the signature slot and the CRC are outside it, or a signer
	 * could not compute what it is about to sign. */
	current_key = "header.auth-digest-excludes-signature";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_SIGNATURE] ^= 0xffu;
	wire[AIUS_OTA_HEADER_OFFSET_SIGNATURE + 63u] ^= 0xffu;
	reseal(wire);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), digest) ==
	      AIUS_OTA_OK);
	CHECK(memcmp(digest, baseline, sizeof(digest)) == 0);
}

/* -------------------------------------------------------------------------
 * Latch
 * ---------------------------------------------------------------------- */

static void test_init_rejects_bad_target_ids(void)
{
	char over_long[AIUS_OTA_TARGET_ID_BYTES + 2u];
	AiusOtaSession session;

	memset(over_long, 'a', sizeof(over_long) - 1u);
	over_long[sizeof(over_long) - 1u] = '\0';

	current_key = "init.target-id";
	CHECK(aius_ota_init(NULL, TEST_TARGET_ID, strlen(TEST_TARGET_ID)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_init(&session, NULL, 4u) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_init(&session, TEST_TARGET_ID, 0u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_init(&session, over_long, strlen(over_long)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_init(&session, "bad\x01id", 6u) ==
	      AIUS_OTA_INVALID_ARG);

	/* A rejected init still leaves a session that refuses everything
	 * rather than one that is merely unspecified. */
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
	CHECK(!aius_ota_latch_is_armed(&session));
	CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_INVALID_ARG);
	CHECK(!aius_ota_latch_is_armed(&session));

	current_key = "init.target-id-max";
	CHECK(aius_ota_init(&session, over_long,
	                    AIUS_OTA_TARGET_ID_BYTES) == AIUS_OTA_OK);
	CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
}

static void test_disarmed_refuses_every_mutation(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t record[AIUS_OTA_RECORD_BYTES];
	AiusOtaSession session;
	AiusOtaSession before;
	FakeVerifier fake;
	AiusOtaVerifier verifier;

	memset(&fake, 0, sizeof(fake));
	verifier = verifier_over(&fake);
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);

	current_key = "latch.constructed-disarmed";
	CHECK(aius_ota_init(&session, TEST_TARGET_ID,
	                    strlen(TEST_TARGET_ID)) == AIUS_OTA_OK);
	CHECK(!aius_ota_latch_is_armed(&session));
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);

	/* A record to feed the disarmed load path. */
	armed_session(&before);
	CHECK(aius_ota_offer(&before, wire, sizeof(wire)) == AIUS_OTA_OK);
	CHECK(aius_ota_record_save(&before, record, sizeof(record)) ==
	      AIUS_OTA_OK);

	current_key = "latch.disarmed-refusals";
	before = session;
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
	      AIUS_OTA_DISARMED);
	CHECK(aius_ota_absorb(&session, 0u, g_payload, 16u) ==
	      AIUS_OTA_DISARMED);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_DISARMED);
	CHECK(aius_ota_arm_image(&session) == AIUS_OTA_DISARMED);
	CHECK(aius_ota_request_activation(&session) == AIUS_OTA_DISARMED);
	CHECK(aius_ota_abort(&session) == AIUS_OTA_DISARMED);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_DISARMED);
	CHECK(fake.calls == 0u);
	/* Not one byte of the session moved. */
	CHECK(memcmp(&session, &before, sizeof(session)) == 0);

	/* Saving is a read of the session, so it works while disarmed -- that
	 * is how a caller persists the cleared slot a disarm produces. */
	current_key = "latch.disarmed-save";
	CHECK(aius_ota_record_save(&session, record, sizeof(record)) ==
	      AIUS_OTA_OK);

	current_key = "latch.disarm-clears";
	armed_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	stage_all(&session, g_payload, 1024u, 256u);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGING);
	CHECK(aius_ota_latch_disarm(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
	CHECK(aius_ota_next_offset(&session) == 0u);
	/* Re-arming does not resurrect the abandoned staging. */
	CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);

	current_key = "latch.null";
	CHECK(aius_ota_latch_arm(NULL) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_latch_disarm(NULL) == AIUS_OTA_INVALID_ARG);
	CHECK(!aius_ota_latch_is_armed(NULL));
	CHECK(aius_ota_state(NULL) == AIUS_OTA_STATE_IDLE);
	CHECK(aius_ota_next_offset(NULL) == 0u);
}

/* -------------------------------------------------------------------------
 * Offer and staging
 * ---------------------------------------------------------------------- */

static void test_offer_checks_target_identity(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	AiusOtaImageHeader header;
	AiusOtaSession session;

	current_key = "offer.target-mismatch";
	armed_session(&session);
	header = valid_header_struct(g_payload, TEST_IMAGE_BYTES);
	memcpy(header.target_id, "teensy41-capability",
	       sizeof("teensy41-capability"));
	CHECK(aius_ota_header_serialize(&header, wire, sizeof(wire)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
	      AIUS_OTA_UNSUPPORTED);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);

	/* A prefix of the board's own id is a different board. */
	current_key = "offer.target-prefix";
	header = valid_header_struct(g_payload, TEST_IMAGE_BYTES);
	memcpy(header.target_id, "teensy41", sizeof("teensy41"));
	CHECK(aius_ota_header_serialize(&header, wire, sizeof(wire)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
	      AIUS_OTA_UNSUPPORTED);

	current_key = "offer.malformed-propagates";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_MAGIC] ^= 0xffu;
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
	      AIUS_OTA_MALFORMED);
	CHECK(aius_ota_offer(&session, NULL, sizeof(wire)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);

	current_key = "offer.accepts";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_OFFERED);
	CHECK(aius_ota_next_offset(&session) == 0u);
}

static void test_staging_absorbs_and_completes(void)
{
	static const uint32_t chunks[] = {1u, 63u, 64u, 512u,
	                                  AIUS_OTA_MAX_CHUNK_BYTES};
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	size_t index;

	current_key = "staging.chunk-sizes";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	for (index = 0u; index < ARRAY_COUNT(chunks); index++) {
		AiusOtaSession session;
		FakeVerifier fake;
		AiusOtaVerifier verifier;

		memset(&fake, 0, sizeof(fake));
		verifier = verifier_over(&fake);
		armed_session(&session);
		CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
		      AIUS_OTA_OK);
		stage_all(&session, g_payload, TEST_IMAGE_BYTES,
		          chunks[index]);
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGED);
		CHECK(aius_ota_next_offset(&session) == TEST_IMAGE_BYTES);
		/* Every chunk size must reach the same digest, which is the
		 * whole claim of the incremental path. */
		CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_OK);
		CHECK(fake.calls == 1u);
	}
}

static void test_chunk_sequencing_is_strict(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t oversized[AIUS_OTA_MAX_CHUNK_BYTES + 1u];
	AiusOtaSession session;
	AiusOtaSession before;

	current_key = "staging.sequencing";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	memset(oversized, 0x5a, sizeof(oversized));
	armed_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);

	/* A first chunk at a non-zero offset is a gap. */
	before = session;
	CHECK(aius_ota_absorb(&session, 1u, g_payload, 16u) == AIUS_OTA_RANGE);
	CHECK(memcmp(&session, &before, sizeof(session)) == 0);

	CHECK(aius_ota_absorb(&session, 0u, g_payload, 256u) == AIUS_OTA_OK);
	CHECK(aius_ota_next_offset(&session) == 256u);

	/* Gap, overlap and a byte-identical re-send of the accepted chunk are
	 * all RANGE, and none of them moves the session. */
	before = session;
	CHECK(aius_ota_absorb(&session, 257u, &g_payload[257], 16u) ==
	      AIUS_OTA_RANGE);
	CHECK(aius_ota_absorb(&session, 255u, &g_payload[255], 16u) ==
	      AIUS_OTA_RANGE);
	CHECK(aius_ota_absorb(&session, 0u, g_payload, 256u) ==
	      AIUS_OTA_RANGE);
	CHECK(aius_ota_absorb(&session, 0u, g_payload, 1u) == AIUS_OTA_RANGE);
	CHECK(memcmp(&session, &before, sizeof(session)) == 0);

	/* Argument rules: an empty chunk cannot advance anything and an
	 * over-large one would blow the per-epoch work bound. */
	CHECK(aius_ota_absorb(&session, 256u, &g_payload[256], 0u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_absorb(&session, 256u, oversized,
	                      sizeof(oversized)) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_absorb(&session, 256u, NULL, 16u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_absorb(NULL, 256u, &g_payload[256], 16u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(memcmp(&session, &before, sizeof(session)) == 0);

	/* Exactly at the bound is accepted. */
	CHECK(aius_ota_absorb(&session, 256u, &g_payload[256],
	                      AIUS_OTA_MAX_CHUNK_BYTES) == AIUS_OTA_OK);

	/* Resume from the reported offset, then overrun the declared length. */
	current_key = "staging.overflow";
	stage_all(&session, g_payload, TEST_IMAGE_BYTES - 8u, 512u);
	CHECK(aius_ota_next_offset(&session) == TEST_IMAGE_BYTES - 8u);
	before = session;
	CHECK(aius_ota_absorb(&session, TEST_IMAGE_BYTES - 8u,
	                      &g_payload[TEST_IMAGE_BYTES - 8u], 9u) ==
	      AIUS_OTA_OVERFLOW);
	/* Refused whole: a truncated absorb would break the strict contract. */
	CHECK(memcmp(&session, &before, sizeof(session)) == 0);
	CHECK(aius_ota_absorb(&session, TEST_IMAGE_BYTES - 8u,
	                      &g_payload[TEST_IMAGE_BYTES - 8u], 8u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGED);

	/* A single-byte image is the degenerate case the length bound allows. */
	current_key = "staging.one-byte-image";
	build_wire(wire, g_payload, 1u);
	armed_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	CHECK(aius_ota_absorb(&session, 0u, g_payload, 2u) ==
	      AIUS_OTA_OVERFLOW);
	CHECK(aius_ota_absorb(&session, 0u, g_payload, 1u) == AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGED);
}

/* -------------------------------------------------------------------------
 * Verification
 * ---------------------------------------------------------------------- */

static void test_verify_seam_accept_and_refuse(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t expected_auth[AIUS_OTA_DIGEST_BYTES];
	AiusOtaSession session;
	AiusOtaProgress progress;
	FakeVerifier fake;
	AiusOtaVerifier verifier;

	current_key = "verify.accepts";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	CHECK(aius_ota_header_auth_digest(wire, sizeof(wire), expected_auth) ==
	      AIUS_OTA_OK);
	memset(&fake, 0, sizeof(fake));
	verifier = verifier_over(&fake);
	armed_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	stage_all(&session, g_payload, TEST_IMAGE_BYTES, 512u);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_VERIFIED);
	CHECK(fake.calls == 1u);

	/* The seam is handed the AUTHENTICATION digest -- the hash of the
	 * header's identity prefix -- and the header's detached signature. */
	CHECK(memcmp(fake.last_digest, expected_auth,
	             sizeof(expected_auth)) == 0);
	CHECK(fake.last_signature[0] == 0xa5u);
	CHECK(fake.last_signature[AIUS_OTA_SIGNATURE_BYTES - 1u] == 0xa5u);

	current_key = "verify.missing-verifier";
	{
		AiusOtaVerifier empty;
		AiusOtaSession staged;

		memset(&fake, 0, sizeof(fake));
		armed_session(&staged);
		CHECK(aius_ota_offer(&staged, wire, sizeof(wire)) ==
		      AIUS_OTA_OK);
		stage_all(&staged, g_payload, TEST_IMAGE_BYTES, 4096u);
		empty.ctx = &fake;
		empty.verify = NULL;
		CHECK(aius_ota_verify(&staged, &empty) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_verify(&staged, NULL) == AIUS_OTA_INVALID_ARG);
		/* A missing verifier is never an accepting one. */
		CHECK(aius_ota_state(&staged) == AIUS_OTA_STATE_STAGED);
	}

	current_key = "verify.refused";
	memset(&fake, 0, sizeof(fake));
	fake.result = 7;
	verifier = verifier_over(&fake);
	armed_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	stage_all(&session, g_payload, TEST_IMAGE_BYTES, 1024u);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_SIGNATURE);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_FAILED);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.fail_reason == AIUS_OTA_SIGNATURE);
	CHECK(progress.verifier_fault == 7);
	CHECK(progress.staged_length == TEST_IMAGE_BYTES);
	CHECK(progress.image_length == TEST_IMAGE_BYTES);
	CHECK(progress.latch_armed);
	/* A failed session refuses a second verification rather than looping. */
	fake.result = 0;
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_STATE);
	/* Re-offering is the way back. */
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_OFFERED);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.fail_reason == AIUS_OTA_OK);

	current_key = "verify.digest-mismatch";
	memset(&fake, 0, sizeof(fake));
	verifier = verifier_over(&fake);
	armed_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	stage_all(&session, g_other_payload, TEST_IMAGE_BYTES, 512u);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_DIGEST);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_FAILED);
	/* The signature check is never reached on a digest mismatch. */
	CHECK(fake.calls == 0u);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.fail_reason == AIUS_OTA_DIGEST);
	CHECK(progress.verifier_fault == 0);

	/* One flipped byte in the middle of a multi-megabyte-shaped stage is
	 * the case the digest exists for. */
	current_key = "verify.one-flipped-byte";
	memset(&fake, 0, sizeof(fake));
	verifier = verifier_over(&fake);
	armed_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	{
		uint8_t corrupted[TEST_IMAGE_BYTES];

		memcpy(corrupted, g_payload, sizeof(corrupted));
		corrupted[2500] ^= 0x01u;
		stage_all(&session, corrupted, TEST_IMAGE_BYTES, 512u);
	}
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_DIGEST);
	CHECK(fake.calls == 0u);

	current_key = "verify.arguments";
	CHECK(aius_ota_verify(NULL, &verifier) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_progress(NULL, &progress) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_progress(&session, NULL) == AIUS_OTA_INVALID_ARG);
}

/* -------------------------------------------------------------------------
 * Transition matrix
 * ---------------------------------------------------------------------- */

typedef enum {
	OP_OFFER = 0,
	OP_ABSORB,
	OP_VERIFY,
	OP_ARM_IMAGE,
	OP_REQUEST_ACTIVATION,
	OP_ABORT,
	OP_COUNT
} TestOp;

static const char *const op_names[OP_COUNT] = {
	"offer", "absorb", "verify", "arm_image", "request_activation", "abort"
};

static const char *const state_names[] = {
	"idle", "offered", "staging", "staged",
	"verified", "armed", "activation_requested", "failed"
};

/* Drives a freshly armed session to `target`. FAILED is reached the only way
 * the machine allows: staging the wrong bytes and verifying them. */
static void drive_to_state(AiusOtaSession *session, AiusOtaState target,
	                   const uint8_t *wire, FakeVerifier *fake)
{
	AiusOtaVerifier verifier = verifier_over(fake);

	armed_session(session);
	if (target == AIUS_OTA_STATE_IDLE)
		return;
	CHECK(aius_ota_offer(session, wire, AIUS_OTA_HEADER_BYTES) ==
	      AIUS_OTA_OK);
	if (target == AIUS_OTA_STATE_OFFERED)
		return;
	if (target == AIUS_OTA_STATE_STAGING) {
		stage_all(session, g_payload, TEST_IMAGE_BYTES - 1u, 512u);
		return;
	}
	if (target == AIUS_OTA_STATE_FAILED) {
		stage_all(session, g_other_payload, TEST_IMAGE_BYTES, 512u);
		CHECK(aius_ota_verify(session, &verifier) == AIUS_OTA_DIGEST);
		return;
	}
	stage_all(session, g_payload, TEST_IMAGE_BYTES, 512u);
	if (target == AIUS_OTA_STATE_STAGED)
		return;
	CHECK(aius_ota_verify(session, &verifier) == AIUS_OTA_OK);
	if (target == AIUS_OTA_STATE_VERIFIED)
		return;
	CHECK(aius_ota_arm_image(session) == AIUS_OTA_OK);
	if (target == AIUS_OTA_STATE_ARMED)
		return;
	CHECK(aius_ota_request_activation(session) == AIUS_OTA_OK);
}

static AiusOtaStatus apply_op(AiusOtaSession *session, TestOp op,
	                      const uint8_t *wire, FakeVerifier *fake)
{
	AiusOtaVerifier verifier = verifier_over(fake);
	uint32_t offset;

	switch (op) {
	case OP_OFFER:
		return aius_ota_offer(session, wire, AIUS_OTA_HEADER_BYTES);
	case OP_ABSORB:
		/* Always at the session's own next offset, so a refusal is a
		 * state refusal and never a range one. */
		offset = aius_ota_next_offset(session);
		return aius_ota_absorb(session, offset, &g_payload[offset], 1u);
	case OP_VERIFY:
		return aius_ota_verify(session, &verifier);
	case OP_ARM_IMAGE:
		return aius_ota_arm_image(session);
	case OP_REQUEST_ACTIVATION:
		return aius_ota_request_activation(session);
	case OP_ABORT:
		return aius_ota_abort(session);
	case OP_COUNT:
		break;
	}
	CHECK(0);
	return AIUS_OTA_INVALID_ARG;
}

static void test_transition_matrix(void)
{
	/* Rows follow AiusOtaState; columns follow TestOp. */
	static const AiusOtaStatus expected[8][OP_COUNT] = {
		/* IDLE     */ {AIUS_OTA_OK, AIUS_OTA_STATE, AIUS_OTA_STATE,
		                AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_OK},
		/* OFFERED  */ {AIUS_OTA_STATE, AIUS_OTA_OK, AIUS_OTA_STATE,
		                AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_OK},
		/* STAGING  */ {AIUS_OTA_STATE, AIUS_OTA_OK, AIUS_OTA_STATE,
		                AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_OK},
		/* STAGED   */ {AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_OK,
		                AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_OK},
		/* VERIFIED */ {AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_STATE,
		                AIUS_OTA_OK, AIUS_OTA_STATE, AIUS_OTA_OK},
		/* ARMED    */ {AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_STATE,
		                AIUS_OTA_STATE, AIUS_OTA_OK, AIUS_OTA_OK},
		/* ACTIVATE */ {AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_STATE,
		                AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_STATE},
		/* FAILED   */ {AIUS_OTA_OK, AIUS_OTA_STATE, AIUS_OTA_STATE,
		                AIUS_OTA_STATE, AIUS_OTA_STATE, AIUS_OTA_OK}
	};
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	char key[96];
	unsigned int state_index;
	unsigned int op_index;

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	for (state_index = 0u; state_index < ARRAY_COUNT(expected);
	     state_index++) {
		for (op_index = 0u; op_index < OP_COUNT; op_index++) {
			AiusOtaSession session;
			FakeVerifier fake;
			AiusOtaState state = (AiusOtaState)state_index;

			snprintf(key, sizeof(key), "matrix.%s.%s",
			         state_names[state_index], op_names[op_index]);
			current_key = key;
			memset(&fake, 0, sizeof(fake));
			drive_to_state(&session, state, wire, &fake);
			CHECK(aius_ota_state(&session) == state);
			CHECK(apply_op(&session, (TestOp)op_index, wire,
			               &fake) ==
			      expected[state_index][op_index]);
		}
	}

	/* Abort really does clear, from every state that allows it. */
	current_key = "matrix.abort-clears";
	for (state_index = 0u; state_index < ARRAY_COUNT(expected);
	     state_index++) {
		AiusOtaSession session;
		FakeVerifier fake;
		AiusOtaState state = (AiusOtaState)state_index;

		if (state == AIUS_OTA_STATE_ACTIVATION_REQUESTED)
			continue;
		memset(&fake, 0, sizeof(fake));
		drive_to_state(&session, state, wire, &fake);
		CHECK(aius_ota_abort(&session) == AIUS_OTA_OK);
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
		CHECK(aius_ota_next_offset(&session) == 0u);
		CHECK(aius_ota_latch_is_armed(&session));
	}

	/* ACTIVATION_REQUESTED has no software exit, and disarming is not a
	 * back door to one. */
	current_key = "matrix.activation-is-terminal";
	{
		AiusOtaSession session;
		FakeVerifier fake;

		memset(&fake, 0, sizeof(fake));
		drive_to_state(&session,
		               AIUS_OTA_STATE_ACTIVATION_REQUESTED, wire,
		               &fake);
		CHECK(aius_ota_latch_disarm(&session) == AIUS_OTA_OK);
		CHECK(aius_ota_state(&session) ==
		      AIUS_OTA_STATE_ACTIVATION_REQUESTED);
		CHECK(!aius_ota_latch_is_armed(&session));
	}
}

/* -------------------------------------------------------------------------
 * Power-loss record
 * ---------------------------------------------------------------------- */

static void test_record_round_trip_every_state(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t record[AIUS_OTA_RECORD_BYTES];
	char key[96];
	unsigned int state_index;

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	for (state_index = 0u; state_index <= AIUS_OTA_STATE_FAILED;
	     state_index++) {
		AiusOtaSession saved;
		AiusOtaSession restored;
		FakeVerifier fake;
		AiusOtaState state = (AiusOtaState)state_index;

		snprintf(key, sizeof(key), "record.round-trip.%s",
		         state_names[state_index]);
		current_key = key;
		memset(&fake, 0, sizeof(fake));
		drive_to_state(&saved, state, wire, &fake);
		CHECK(aius_ota_record_save(&saved, record, sizeof(record)) ==
		      AIUS_OTA_OK);

		/* A reboot: a brand new session, disarmed, then deliberately
		 * armed before the record is allowed back in. */
		armed_session(&restored);
		CHECK(aius_ota_record_load(&restored, record,
		                           sizeof(record)) == AIUS_OTA_OK);
		CHECK(aius_ota_state(&restored) == state);
		CHECK(aius_ota_next_offset(&restored) ==
		      aius_ota_next_offset(&saved));
		/* The whole session, including the digest midstate and the
		 * latched failure reason, comes back byte for byte. */
		CHECK(memcmp(&restored, &saved, sizeof(restored)) == 0);
	}

	/* The two documented exceptions: the latch never persists, and the
	 * verifier's own fault code is a live diagnostic while the fail_reason
	 * that matters is persisted. */
	current_key = "record.round-trip.verifier-fault";
	{
		AiusOtaSession saved;
		AiusOtaSession restored;
		AiusOtaProgress progress;
		FakeVerifier fake;
		AiusOtaVerifier verifier;

		memset(&fake, 0, sizeof(fake));
		fake.result = -9;
		verifier = verifier_over(&fake);
		armed_session(&saved);
		CHECK(aius_ota_offer(&saved, wire, sizeof(wire)) ==
		      AIUS_OTA_OK);
		stage_all(&saved, g_payload, TEST_IMAGE_BYTES, 512u);
		CHECK(aius_ota_verify(&saved, &verifier) ==
		      AIUS_OTA_SIGNATURE);
		CHECK(aius_ota_record_save(&saved, record, sizeof(record)) ==
		      AIUS_OTA_OK);

		CHECK(aius_ota_init(&restored, TEST_TARGET_ID,
		                    strlen(TEST_TARGET_ID)) == AIUS_OTA_OK);
		CHECK(aius_ota_record_load(&restored, record,
		                           sizeof(record)) ==
		      AIUS_OTA_DISARMED);
		CHECK(aius_ota_latch_arm(&restored) == AIUS_OTA_OK);
		CHECK(aius_ota_record_load(&restored, record,
		                           sizeof(record)) == AIUS_OTA_OK);
		CHECK(aius_ota_progress(&restored, &progress) == AIUS_OTA_OK);
		CHECK(progress.state == AIUS_OTA_STATE_FAILED);
		CHECK(progress.fail_reason == AIUS_OTA_SIGNATURE);
		CHECK(progress.verifier_fault == 0);
	}
}

static void test_record_resumes_staging(void)
{
	static const uint32_t breakpoints[] = {1u, 63u, 64u, 65u, 512u,
	                                       TEST_IMAGE_BYTES - 1u};
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t record[AIUS_OTA_RECORD_BYTES];
	size_t index;

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	for (index = 0u; index < ARRAY_COUNT(breakpoints); index++) {
		AiusOtaSession session;
		FakeVerifier fake;
		AiusOtaVerifier verifier;
		uint32_t breakpoint = breakpoints[index];

		current_key = "record.resume";
		memset(&fake, 0, sizeof(fake));
		verifier = verifier_over(&fake);
		armed_session(&session);
		CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
		      AIUS_OTA_OK);
		stage_all(&session, g_payload, breakpoint, 128u);
		CHECK(aius_ota_record_save(&session, record,
		                           sizeof(record)) == AIUS_OTA_OK);

		/* Power loss here. The bytes already staged are not re-sent;
		 * the digest picks up exactly where it stopped, which is the
		 * whole reason the midstate is in the record. */
		armed_session(&session);
		CHECK(aius_ota_record_load(&session, record,
		                           sizeof(record)) == AIUS_OTA_OK);
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGING);
		CHECK(aius_ota_next_offset(&session) == breakpoint);
		stage_all(&session, g_payload, TEST_IMAGE_BYTES, 333u);
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGED);
		CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_OK);
		CHECK(fake.calls == 1u);
	}

	/* A record written by a board with a different target identity must not
	 * restore, however intact its CRC. */
	current_key = "record.foreign-target";
	{
		AiusOtaSession session;
		FakeVerifier fake;

		memset(&fake, 0, sizeof(fake));
		drive_to_state(&session, AIUS_OTA_STATE_STAGING, wire, &fake);
		CHECK(aius_ota_record_save(&session, record,
		                           sizeof(record)) == AIUS_OTA_OK);
		CHECK(aius_ota_init(&session, "teensy41-capability",
		                    strlen("teensy41-capability")) ==
		      AIUS_OTA_OK);
		CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
		CHECK(aius_ota_record_load(&session, record,
		                           sizeof(record)) ==
		      AIUS_OTA_CORRUPT);
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
	}
}

static void test_record_rejections_fail_closed(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t record[AIUS_OTA_RECORD_BYTES];
	uint8_t mutated[AIUS_OTA_RECORD_BYTES];
	AiusOtaSession session;
	AiusOtaSession template_session;
	FakeVerifier fake;

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&template_session, AIUS_OTA_STATE_STAGING, wire, &fake);
	CHECK(aius_ota_record_save(&template_session, record,
	                           sizeof(record)) == AIUS_OTA_OK);

#define REJECT_RECORD(key_name, mutation, status)                             \
	do {                                                                   \
		current_key = key_name;                                        \
		memcpy(mutated, record, sizeof(mutated));                      \
		mutation;                                                      \
		armed_session(&session);                                       \
		CHECK(aius_ota_record_load(&session, mutated,                  \
		                           sizeof(mutated)) == (status));      \
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);        \
		CHECK(aius_ota_next_offset(&session) == 0u);                   \
		CHECK(aius_ota_latch_is_armed(&session));                      \
	} while (0)

	/* The unmutated record is the control: it must load. */
	current_key = "record.control";
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGING);

	REJECT_RECORD("record.magic",
	              mutated[AIUS_OTA_RECORD_OFFSET_MAGIC] ^= 0x01u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.version",
	              mutated[AIUS_OTA_RECORD_OFFSET_VERSION] = 2u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.length",
	              mutated[AIUS_OTA_RECORD_OFFSET_LENGTH] = 0u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.crc",
	              mutated[AIUS_OTA_RECORD_OFFSET_CRC32] ^= 0x80u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.header-byte",
	              mutated[AIUS_OTA_RECORD_OFFSET_HEADER + 30u] ^= 0x01u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.digest-state",
	              mutated[AIUS_OTA_RECORD_OFFSET_DIGEST_STATE] ^= 0x01u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.staged-length",
	              mutated[AIUS_OTA_RECORD_OFFSET_STAGED_LENGTH] ^= 0x01u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.state-out-of-domain",
	              mutated[AIUS_OTA_RECORD_OFFSET_STATE] = 200u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.reserved0",
	              mutated[AIUS_OTA_RECORD_OFFSET_RESERVED0] = 1u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.reserved1",
	              mutated[AIUS_OTA_RECORD_OFFSET_RESERVED1 + 2u] = 1u,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.buffer-tail",
	              mutated[AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFER +
	                      AIUS_SHA256_BLOCK_BYTES - 1u] = 0xffu,
	              AIUS_OTA_CORRUPT);
	REJECT_RECORD("record.spurious-fail-reason",
	              mutated[AIUS_OTA_RECORD_OFFSET_FAIL_REASON] =
		              (uint8_t)AIUS_OTA_DIGEST,
	              AIUS_OTA_CORRUPT);
	/* Every field above is CRC-covered, so each mutation is caught by the
	 * CRC before its own rule fires. Resealing is not possible from here
	 * without a second copy of the record layout in the test, so the field
	 * rules themselves are proven by the consistency cases below, which are
	 * built through the interface. */

#undef REJECT_RECORD

	current_key = "record.sizes";
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record) - 1u) ==
	      AIUS_OTA_NO_SPACE);
	CHECK(aius_ota_record_load(&session, record, sizeof(record) + 1u) ==
	      AIUS_OTA_NO_SPACE);
	CHECK(aius_ota_record_load(&session, NULL, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_record_load(NULL, record, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_record_save(&session, record, sizeof(record) - 1u) ==
	      AIUS_OTA_NO_SPACE);
	CHECK(aius_ota_record_save(&session, NULL, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_record_save(NULL, record, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);

	/* An all-zero slot -- an erased sector, or a board that never staged
	 * anything -- is refused, not read as a valid idle record. */
	current_key = "record.erased-slot";
	memset(mutated, 0, sizeof(mutated));
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, mutated, sizeof(mutated)) ==
	      AIUS_OTA_CORRUPT);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);

	/* An 0xff slot likewise. */
	memset(mutated, 0xff, sizeof(mutated));
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, mutated, sizeof(mutated)) ==
	      AIUS_OTA_CORRUPT);

	/* A saved IDLE session is the legitimate "nothing in flight" record. */
	current_key = "record.idle";
	armed_session(&session);
	CHECK(aius_ota_record_save(&session, mutated, sizeof(mutated)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, mutated, sizeof(mutated)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
}

/* The consistency rules that no single-byte mutation can express, because
 * every one of them needs the CRC recomputed over a deliberately inconsistent
 * body. Built by saving a real session and re-saving it after a targeted lie
 * about its own contents. */
static void test_record_consistency_rules(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t record[AIUS_OTA_RECORD_BYTES];
	AiusOtaSession source;
	AiusOtaSession session;
	FakeVerifier fake;

	build_wire(wire, g_payload, TEST_IMAGE_BYTES);

	/* STAGING claiming the whole image is staged: the state and the length
	 * disagree, and only the pair is checkable. */
	current_key = "record.staging-claims-complete";
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&source, AIUS_OTA_STATE_STAGED, wire, &fake);
	source.state = AIUS_OTA_STATE_STAGING;
	CHECK(aius_ota_record_save(&source, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_CORRUPT);

	/* OFFERED with bytes already staged. */
	current_key = "record.offered-with-bytes";
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&source, AIUS_OTA_STATE_STAGING, wire, &fake);
	source.state = AIUS_OTA_STATE_OFFERED;
	CHECK(aius_ota_record_save(&source, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_CORRUPT);

	/* STAGED with fewer bytes than the header declares. */
	current_key = "record.staged-short";
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&source, AIUS_OTA_STATE_STAGING, wire, &fake);
	source.state = AIUS_OTA_STATE_STAGED;
	CHECK(aius_ota_record_save(&source, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_CORRUPT);

	/* The digest's own byte count disagreeing with staged_length: the
	 * cheap cross-check that catches a half-written record. */
	current_key = "record.digest-count-disagrees";
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&source, AIUS_OTA_STATE_STAGING, wire, &fake);
	source.digest.absorbed += 1u;
	CHECK(aius_ota_record_save(&source, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_CORRUPT);

	/* Buffered byte count that is not staged_length mod 64. */
	current_key = "record.digest-buffered-disagrees";
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&source, AIUS_OTA_STATE_STAGING, wire, &fake);
	source.digest.buffered = (uint8_t)((source.digest.buffered + 1u) %
	                                   AIUS_SHA256_BLOCK_BYTES);
	CHECK(aius_ota_record_save(&source, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_CORRUPT);

	/* An IDLE record that still carries an image. */
	current_key = "record.idle-with-image";
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&source, AIUS_OTA_STATE_STAGING, wire, &fake);
	source.state = AIUS_OTA_STATE_IDLE;
	CHECK(aius_ota_record_save(&source, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_CORRUPT);

	/* A FAILED record whose reason is not a verification failure. */
	current_key = "record.failed-wrong-reason";
	memset(&fake, 0, sizeof(fake));
	drive_to_state(&source, AIUS_OTA_STATE_FAILED, wire, &fake);
	source.fail_reason = AIUS_OTA_RANGE;
	CHECK(aius_ota_record_save(&source, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	armed_session(&session);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_CORRUPT);

	/* The record is canonical: two sessions with the same hash state
	 * serialize to the same bytes, whatever stale bytes their block
	 * buffers happen to hold. */
	current_key = "record.canonical";
	{
		uint8_t first[AIUS_OTA_RECORD_BYTES];
		uint8_t second[AIUS_OTA_RECORD_BYTES];
		AiusOtaSession one;
		AiusOtaSession two;

		memset(&fake, 0, sizeof(fake));
		armed_session(&one);
		CHECK(aius_ota_offer(&one, wire, sizeof(wire)) == AIUS_OTA_OK);
		stage_all(&one, g_payload, 100u, 100u);
		armed_session(&two);
		CHECK(aius_ota_offer(&two, wire, sizeof(wire)) == AIUS_OTA_OK);
		stage_all(&two, g_payload, 100u, 7u);
		CHECK(aius_ota_record_save(&one, first, sizeof(first)) ==
		      AIUS_OTA_OK);
		CHECK(aius_ota_record_save(&two, second, sizeof(second)) ==
		      AIUS_OTA_OK);
		CHECK(memcmp(first, second, sizeof(first)) == 0);
	}
}

/* -------------------------------------------------------------------------
 * Image health
 * ---------------------------------------------------------------------- */

static void digest_of(uint8_t *digest, uint8_t seed)
{
	memset(digest, seed, AIUS_OTA_DIGEST_BYTES);
	digest[0] = (uint8_t)(seed + 1u);
}

static void test_health_trial_commits(void)
{
	uint8_t image_a[AIUS_OTA_DIGEST_BYTES];
	uint8_t image_b[AIUS_OTA_DIGEST_BYTES];
	AiusOtaHealth health;
	AiusOtaHealthAction action;

	digest_of(image_a, 0x11u);
	digest_of(image_b, 0x22u);

	current_key = "health.init";
	aius_ota_health_init(&health);
	CHECK(health.state == AIUS_OTA_HEALTH_UNKNOWN);
	CHECK(!aius_ota_health_is_trialing(&health));
	CHECK(!aius_ota_health_is_trialing(NULL));

	/* The board is running image A and nobody recorded it: adopted. */
	current_key = "health.adopts-unrecorded";
	CHECK(aius_ota_health_note_boot(&health, image_a, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_ADOPTED);
	CHECK(health.state == AIUS_OTA_HEALTH_UNKNOWN);
	CHECK(memcmp(health.image_digest, image_a, sizeof(image_a)) == 0);
	CHECK(aius_ota_health_note_boot(&health, image_a, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_NONE);

	current_key = "health.trial-then-commit";
	CHECK(aius_ota_health_note_activation(
		      &health, image_b,
		      AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS) == AIUS_OTA_OK);
	CHECK(health.state == AIUS_OTA_HEALTH_TRIAL);
	CHECK(aius_ota_health_is_trialing(&health));
	CHECK(memcmp(health.image_digest, image_b, sizeof(image_b)) == 0);
	CHECK(memcmp(health.previous_digest, image_a, sizeof(image_a)) == 0);
	CHECK(health.trial_boots_remaining ==
	      AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS);

	CHECK(aius_ota_health_note_boot(&health, image_b, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_TRIAL_CONTINUES);
	CHECK(health.trial_boots_remaining == 1u);
	CHECK(aius_ota_health_mark_healthy(&health, image_b) == AIUS_OTA_OK);
	CHECK(health.state == AIUS_OTA_HEALTH_COMMITTED);
	CHECK(health.trial_boots_remaining == 0u);
	/* Provenance survives the commit even though it is no longer a
	 * rollback target. */
	CHECK(memcmp(health.previous_digest, image_a, sizeof(image_a)) == 0);
	/* Idempotent, and only for the image that is actually running. */
	CHECK(aius_ota_health_mark_healthy(&health, image_b) == AIUS_OTA_OK);
	CHECK(aius_ota_health_mark_healthy(&health, image_a) ==
	      AIUS_OTA_STATE);
	CHECK(aius_ota_health_note_rollback(&health) == AIUS_OTA_STATE);
}

static void test_health_trial_budget_and_rollback(void)
{
	uint8_t image_a[AIUS_OTA_DIGEST_BYTES];
	uint8_t image_b[AIUS_OTA_DIGEST_BYTES];
	uint8_t image_c[AIUS_OTA_DIGEST_BYTES];
	AiusOtaHealth health;
	AiusOtaHealthAction action;

	digest_of(image_a, 0x11u);
	digest_of(image_b, 0x22u);
	digest_of(image_c, 0x33u);

	/* The trial image boots but never reaches healthy: the budget runs out
	 * and the record asks for a rollback, without applying one itself --
	 * the revert is the caller's act, and note_rollback() confirms it. */
	current_key = "health.budget-exhausts";
	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_activation(&health, image_b, 2u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_boot(&health, image_b, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_TRIAL_CONTINUES);
	CHECK(aius_ota_health_note_boot(&health, image_b, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_ROLL_BACK);
	CHECK(health.state == AIUS_OTA_HEALTH_TRIAL);
	/* Further boots keep asking, they do not go negative. */
	CHECK(aius_ota_health_note_boot(&health, image_b, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_ROLL_BACK);
	CHECK(health.trial_boots_remaining == 0u);

	CHECK(aius_ota_health_note_rollback(&health) == AIUS_OTA_OK);
	CHECK(health.state == AIUS_OTA_HEALTH_ROLLED_BACK);
	CHECK(memcmp(health.image_digest, image_a, sizeof(image_a)) == 0);
	CHECK(health.rollback_count == 1u);
	CHECK(aius_ota_health_note_rollback(&health) == AIUS_OTA_STATE);

	/* The trial image never comes up at all and the fallback boots: the
	 * record works that out from identity alone, with no slot index and no
	 * help from the caller. */
	current_key = "health.fallback-booted";
	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_mark_healthy(&health, image_a) ==
	      AIUS_OTA_STATE);
	CHECK(aius_ota_health_note_activation(&health, image_b, 2u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_boot(&health, image_a, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_ROLLED_BACK);
	CHECK(health.state == AIUS_OTA_HEALTH_ROLLED_BACK);
	CHECK(memcmp(health.image_digest, image_a, sizeof(image_a)) == 0);
	CHECK(health.rollback_count == 1u);

	/* A third image appearing mid-trial is somebody at the USB port. */
	current_key = "health.third-image-adopted";
	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_activation(&health, image_b, 2u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_boot(&health, image_c, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_ADOPTED);
	CHECK(health.state == AIUS_OTA_HEALTH_UNKNOWN);
	CHECK(memcmp(health.image_digest, image_c, sizeof(image_c)) == 0);
	CHECK(health.trial_boots_budget == 0u);

	/* A superseding trial keeps the last COMMITTED image as the fallback,
	 * never the trial it replaces -- rolling back to an image that never
	 * proved itself would be the worst possible choice. */
	current_key = "health.supersede-keeps-fallback";
	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_activation(&health, image_b, 2u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_activation(&health, image_c, 2u) ==
	      AIUS_OTA_OK);
	CHECK(memcmp(health.image_digest, image_c, sizeof(image_c)) == 0);
	CHECK(memcmp(health.previous_digest, image_a, sizeof(image_a)) == 0);

	/* Re-noting the SAME activation is idempotent and does NOT refill the
	 * budget: a power loss between the activation write and the reboot must
	 * not hand a boot-looping image unlimited trials. */
	current_key = "health.reactivation-idempotent";
	CHECK(aius_ota_health_note_boot(&health, image_c, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_TRIAL_CONTINUES);
	CHECK(health.trial_boots_remaining == 1u);
	CHECK(aius_ota_health_note_activation(&health, image_c, 2u) ==
	      AIUS_OTA_OK);
	CHECK(health.trial_boots_remaining == 1u);
	CHECK(memcmp(health.previous_digest, image_a, sizeof(image_a)) == 0);

	current_key = "health.arguments";
	{
		uint8_t zero[AIUS_OTA_DIGEST_BYTES];

		memset(zero, 0, sizeof(zero));
		aius_ota_health_init(NULL);
		CHECK(aius_ota_health_note_activation(NULL, image_a, 2u) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_activation(&health, NULL, 2u) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_activation(&health, zero, 2u) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_activation(&health, image_a, 0u) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_activation(
			      &health, image_a,
			      AIUS_OTA_HEALTH_MAX_TRIAL_BOOTS + 1u) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_boot(NULL, image_a, NULL) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_boot(&health, NULL, NULL) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_boot(&health, zero, NULL) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_mark_healthy(NULL, image_a) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_mark_healthy(&health, NULL) ==
		      AIUS_OTA_INVALID_ARG);
		CHECK(aius_ota_health_note_rollback(NULL) ==
		      AIUS_OTA_INVALID_ARG);
	}
}

static void test_health_record_round_trip(void)
{
	uint8_t image_a[AIUS_OTA_DIGEST_BYTES];
	uint8_t image_b[AIUS_OTA_DIGEST_BYTES];
	uint8_t record[AIUS_OTA_HEALTH_RECORD_BYTES];
	uint8_t mutated[AIUS_OTA_HEALTH_RECORD_BYTES];
	AiusOtaHealth health;
	AiusOtaHealth restored;

	digest_of(image_a, 0x11u);
	digest_of(image_b, 0x22u);

	current_key = "health.record-round-trip";
	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_activation(&health, image_b, 2u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_boot(&health, image_b, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_save(&health, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_load(&restored, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(memcmp(&restored, &health, sizeof(health)) == 0);

	/* Each recorded state survives the round trip. */
	current_key = "health.record-every-state";
	CHECK(aius_ota_health_mark_healthy(&health, image_b) == AIUS_OTA_OK);
	CHECK(aius_ota_health_save(&health, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_load(&restored, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(restored.state == AIUS_OTA_HEALTH_COMMITTED);
	CHECK(memcmp(&restored, &health, sizeof(health)) == 0);

	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_activation(&health, image_b, 2u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_rollback(&health) == AIUS_OTA_OK);
	CHECK(aius_ota_health_save(&health, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_load(&restored, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(restored.state == AIUS_OTA_HEALTH_ROLLED_BACK);
	CHECK(restored.rollback_count == 1u);

	current_key = "health.record-rejections";
	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_note_activation(&health, image_b, 2u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_save(&health, record, sizeof(record)) ==
	      AIUS_OTA_OK);

#define REJECT_HEALTH(key_name, mutation)                                     \
	do {                                                                   \
		current_key = key_name;                                        \
		memcpy(mutated, record, sizeof(mutated));                      \
		mutation;                                                      \
		CHECK(aius_ota_health_load(&restored, mutated,                 \
		                           sizeof(mutated)) ==                 \
		      AIUS_OTA_CORRUPT);                                       \
		CHECK(restored.state == AIUS_OTA_HEALTH_UNKNOWN);              \
		CHECK(restored.trial_boots_budget == 0u);                      \
	} while (0)

	REJECT_HEALTH("health.magic",
	              mutated[AIUS_OTA_HEALTH_OFFSET_MAGIC] ^= 0x01u);
	REJECT_HEALTH("health.version",
	              mutated[AIUS_OTA_HEALTH_OFFSET_VERSION] = 9u);
	REJECT_HEALTH("health.length",
	              mutated[AIUS_OTA_HEALTH_OFFSET_LENGTH] = 0u);
	REJECT_HEALTH("health.crc",
	              mutated[AIUS_OTA_HEALTH_OFFSET_CRC32 + 1u] ^= 0x10u);
	REJECT_HEALTH("health.state-domain",
	              mutated[AIUS_OTA_HEALTH_OFFSET_STATE] = 4u);
	REJECT_HEALTH("health.digest-byte",
	              mutated[AIUS_OTA_HEALTH_OFFSET_IMAGE_DIGEST + 5u] ^=
		              0x01u);
	REJECT_HEALTH("health.reserved0",
	              mutated[AIUS_OTA_HEALTH_OFFSET_RESERVED0] = 1u);
	REJECT_HEALTH("health.reserved1",
	              mutated[AIUS_OTA_HEALTH_OFFSET_RESERVED1 + 27u] = 1u);
	REJECT_HEALTH("health.erased-slot", memset(mutated, 0xff,
		                                  sizeof(mutated)));
	REJECT_HEALTH("health.zeroed-slot", memset(mutated, 0,
		                                  sizeof(mutated)));

#undef REJECT_HEALTH

	current_key = "health.record-domain-rules";
	/* States this machine cannot reach, each built by lying in the struct
	 * and re-serializing so the CRC agrees with the lie -- otherwise the
	 * CRC would catch every one of them and the domain rules would never
	 * run. A fresh trial record per case, because note_activation() is
	 * idempotent for the image already on trial and would not undo the
	 * previous case's lie. */
#define REJECT_HEALTH_DOMAIN(key_name, lie)                                   \
	do {                                                                   \
		current_key = key_name;                                        \
		aius_ota_health_init(&health);                                 \
		CHECK(aius_ota_health_note_boot(&health, image_a, NULL) ==     \
		      AIUS_OTA_OK);                                            \
		CHECK(aius_ota_health_note_activation(&health, image_b, 2u) == \
		      AIUS_OTA_OK);                                            \
		lie;                                                           \
		CHECK(aius_ota_health_save(&health, mutated,                   \
		                           sizeof(mutated)) == AIUS_OTA_OK);   \
		CHECK(aius_ota_health_load(&restored, mutated,                 \
		                           sizeof(mutated)) ==                 \
		      AIUS_OTA_CORRUPT);                                       \
	} while (0)

	REJECT_HEALTH_DOMAIN("health.trial-without-budget",
	                     health.trial_boots_budget = 0u;
	                     health.trial_boots_remaining = 0u);
	REJECT_HEALTH_DOMAIN("health.committed-with-budget",
	                     health.state = AIUS_OTA_HEALTH_COMMITTED);
	REJECT_HEALTH_DOMAIN("health.remaining-over-budget",
	                     health.trial_boots_remaining = 3u);
	REJECT_HEALTH_DOMAIN("health.trial-without-image",
	                     memset(health.image_digest, 0,
	                            sizeof(health.image_digest)));

#undef REJECT_HEALTH_DOMAIN

	current_key = "health.record-arguments";
	CHECK(aius_ota_health_save(NULL, record, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_health_save(&health, NULL, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_health_save(&health, record, sizeof(record) - 1u) ==
	      AIUS_OTA_NO_SPACE);
	CHECK(aius_ota_health_load(NULL, record, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_health_load(&restored, NULL, sizeof(record)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_health_load(&restored, record, sizeof(record) + 1u) ==
	      AIUS_OTA_NO_SPACE);
}

/* -------------------------------------------------------------------------
 * End-to-end
 * ---------------------------------------------------------------------- */

/* The whole slice in one pass, in the order an operator would drive it, with
 * a power loss in the middle -- offered, staged, verified, armed, activation
 * requested, then the health record's trial and commit. */
static void test_end_to_end_upgrade(void)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t record[AIUS_OTA_RECORD_BYTES];
	uint8_t health_record[AIUS_OTA_HEALTH_RECORD_BYTES];
	uint8_t running[AIUS_OTA_DIGEST_BYTES];
	AiusOtaSession session;
	AiusOtaProgress progress;
	AiusOtaHealth health;
	AiusOtaHealthAction action;
	FakeVerifier fake;
	AiusOtaVerifier verifier;

	current_key = "end-to-end";
	build_wire(wire, g_payload, TEST_IMAGE_BYTES);
	memset(&fake, 0, sizeof(fake));
	verifier = verifier_over(&fake);

	/* Boot: disarmed, nothing possible. */
	CHECK(aius_ota_init(&session, TEST_TARGET_ID,
	                    strlen(TEST_TARGET_ID)) == AIUS_OTA_OK);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
	      AIUS_OTA_DISARMED);

	/* The operator arms, the image is offered and staged. */
	CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	stage_all(&session, g_payload, 2048u, 512u);
	CHECK(aius_ota_record_save(&session, record, sizeof(record)) ==
	      AIUS_OTA_OK);

	/* Power loss. The board comes back disarmed, as it always does. */
	CHECK(aius_ota_init(&session, TEST_TARGET_ID,
	                    strlen(TEST_TARGET_ID)) == AIUS_OTA_OK);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_DISARMED);
	CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_next_offset(&session) == 2048u);

	stage_all(&session, g_payload, TEST_IMAGE_BYTES, 512u);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_OK);
	CHECK(aius_ota_arm_image(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_request_activation(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.state == AIUS_OTA_STATE_ACTIVATION_REQUESTED);
	CHECK(progress.staged_length == progress.image_length);
	CHECK(progress.fail_reason == AIUS_OTA_OK);

	/* The health record picks the story up at activation. The image's
	 * identity is its payload digest -- the same 32 bytes the container
	 * carried and the staging machine just proved. */
	memcpy(running, session.header.payload_digest, sizeof(running));
	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_activation(
		      &health, running,
		      AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS) == AIUS_OTA_OK);
	CHECK(aius_ota_health_save(&health, health_record,
	                           sizeof(health_record)) == AIUS_OTA_OK);

	/* Next boot: the new image runs, spends a trial boot and commits. */
	CHECK(aius_ota_health_load(&health, health_record,
	                           sizeof(health_record)) == AIUS_OTA_OK);
	CHECK(aius_ota_health_note_boot(&health, running, &action) ==
	      AIUS_OTA_OK);
	CHECK(action == AIUS_OTA_HEALTH_ACTION_TRIAL_CONTINUES);
	CHECK(aius_ota_health_mark_healthy(&health, running) == AIUS_OTA_OK);
	CHECK(health.state == AIUS_OTA_HEALTH_COMMITTED);
	CHECK(health.rollback_count == 0u);
}

/* -------------------------------------------------------------------------
 * Activation staging placement (aius-ota-resilience.md Phase 2)
 * ---------------------------------------------------------------------- */

static void test_staging_placement_per_fitment(void)
{
	AiusOtaStagingPlacement placement;
	const uint32_t mb = 1048576u;
	const uint32_t carve = 32768u; /* RECORDER_STAGING_BYTES on target */

	/* 16 MB board (both pads): staging claims the pad-1 region the ring
	 * normally owns -- entirely BELOW the 8 MiB pad boundary, so the
	 * marginal pad-2 chip is never in the activation read path. */
	current_key = "staging.16mb";
	CHECK(aius_ota_staging_place(16u * mb, 0u, carve, &placement));
	CHECK(!placement.on_pad2);
	CHECK(placement.offset == carve);
	CHECK(placement.capacity == 8u * mb - carve);
	CHECK(placement.offset + placement.capacity <=
	      AIUS_OTA_STAGING_PAD_BYTES);
	/* And a maximum image still fits -- the fix must not shrink the
	 * largest deliverable image. */
	CHECK(placement.capacity >= AIUS_OTA_MAX_IMAGE_BYTES);

	/* 8 MB board (pad 1 only): identical placement -- the ring region is
	 * smaller but the pad is the same. */
	current_key = "staging.8mb";
	CHECK(aius_ota_staging_place(8u * mb, 0u, carve, &placement));
	CHECK(!placement.on_pad2);
	CHECK(placement.offset == carve);
	CHECK(placement.capacity == 8u * mb - carve);
	CHECK(placement.capacity >= AIUS_OTA_MAX_IMAGE_BYTES);

	/* 0 MB mapped, one chip on the silkscreened-FLASH pad-2 socket (the
	 * bench fitment): the whole chip at its XIP window, flagged. */
	current_key = "staging.pad2-only";
	CHECK(aius_ota_staging_place(0u, 8u * mb, carve, &placement));
	CHECK(placement.on_pad2);
	CHECK(placement.offset == AIUS_OTA_STAGING_PAD2_OFFSET);
	CHECK(placement.capacity == 8u * mb);

	/* Bare board: refused -- the ota_no_psram arm. */
	current_key = "staging.bare";
	CHECK(!aius_ota_staging_place(0u, 0u, carve, &placement));
	CHECK(placement.capacity == 0u);

	/* A carve that consumes the whole pad leaves nothing to stage into:
	 * refused rather than a zero-length or wrapped region. */
	current_key = "staging.carve-consumes-pad";
	CHECK(!aius_ota_staging_place(8u * mb, 0u, 8u * mb, &placement));
	CHECK(!aius_ota_staging_place(4u * mb, 0u, 4u * mb, &placement));
	CHECK(!aius_ota_staging_place(0u, 0u, 0u, NULL));
}

int main(void)
{
	fill_payload(g_payload, TEST_IMAGE_BYTES, 0x00u);
	fill_payload(g_other_payload, TEST_IMAGE_BYTES, 0x7fu);

	test_sha256_fips_vectors();
	test_sha256_incremental_matches_one_shot();
	test_header_round_trip();
	test_header_rejects_struct_fields();
	test_header_rejects_wire_fields();
	test_header_auth_digest_binds_the_prefix();
	test_init_rejects_bad_target_ids();
	test_disarmed_refuses_every_mutation();
	test_offer_checks_target_identity();
	test_staging_absorbs_and_completes();
	test_chunk_sequencing_is_strict();
	test_verify_seam_accept_and_refuse();
	test_transition_matrix();
	test_record_round_trip_every_state();
	test_record_resumes_staging();
	test_record_rejections_fail_closed();
	test_record_consistency_rules();
	test_health_trial_commits();
	test_health_trial_budget_and_rollback();
	test_health_record_round_trip();
	test_end_to_end_upgrade();
	test_staging_placement_per_fitment();
	puts("ota core tests passed");
	return 0;
}
