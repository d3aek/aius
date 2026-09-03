/* Host suite for lib/aius_ota's Ed25519 verifier adapter: the real signature
 * scheme behind the seam slice 1 injected.
 *
 *   1. RFC 8032 test vectors 1 and 2, straight through TweetNaCl's raw
 *      crypto_sign / crypto_sign_open. This is the "is the vendored library the
 *      algorithm it claims to be" check, independent of anything in this tree,
 *      and it is what makes the rest of the file mean something.
 *   2. The key context's rules, including the all-zero key an unconfigured
 *      device would hold.
 *   3. The seam itself, with the committed TEST-ONLY keypair: a correct
 *      signature accepted, and a single flipped bit in the signature, in the
 *      digest, or in the public key each refused.
 *   4. The same through a whole AiusOtaSession -- offer, absorb, verify, arm --
 *      so the adapter is proven where the device actually calls it, including a
 *      tampered header (which changes the authentication digest and therefore
 *      the signature's message) and a tampered payload (which the digest check
 *      stops before the verifier is ever consulted).
 *   5. The randombytes trap firing, in a forked child, because a firmware image
 *      that generates keys is a defect and this is the guard that says so.
 *
 * WHAT THIS FILE PROVES WITHOUT ASSERTING IT. It signs with crypto_sign() while
 * linking lib/aius_ota's randombytes TRAP, which aborts the process if called.
 * Ed25519 signing is deterministic, so the trap never fires -- and if a future
 * scheme change made signing reach for entropy, every test here would die
 * rather than quietly sign with whatever the trap zeroed.
 *
 * The signing key is read from the committed test-only key files rather than
 * embedded, so the C suite, the host signer and the Python round-trip test all
 * consume the same bytes and cannot drift.
 *
 * Usage: test_ota_ed25519 <test-key-directory>
 */

/* fork()/waitpid() are POSIX, and -std=c11 hides them behind this. */
#define _POSIX_C_SOURCE 200809L

#include "aius_ota.h"
#include "aius_ota_ed25519.h"
#include "aius_sha256.h"
#include "tweetnacl.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) check((condition), #condition, __LINE__)

#define TEST_TARGET_ID "teensy41-optical"
#define TEST_REVISION "7a66153703d1"
#define TEST_BUILD_UTC UINT64_C(1753900000) /* 2025-07-30, inside the window */
#define TEST_IMAGE_BYTES 5096u
#define SECRET_KEY_BYTES 64u

static const char *current_key = "startup";
static uint8_t g_payload[TEST_IMAGE_BYTES];
static uint8_t g_public_key[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES];
static uint8_t g_secret_key[SECRET_KEY_BYTES];

static void check(int condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "ota ed25519 mismatch [%s] line %d: %s\n", current_key,
	        line, expression);
	exit(1);
}

/* -------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------- */

static int hex_value(int character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

/* The committed key files are comment lines then one line of hex, the shape
 * tools/ota/aius_ota_keygen.c writes. */
static void read_key_file(const char *directory, const char *name,
	                  uint8_t *bytes, size_t length)
{
	char path[512];
	char line[1024];
	FILE *file;
	size_t seen = 0u;
	size_t index;

	CHECK((size_t)snprintf(path, sizeof(path), "%s/%s", directory, name) <
	      sizeof(path));
	file = fopen(path, "r");
	if (file == NULL) {
		fprintf(stderr, "ota ed25519: cannot open %s\n", path);
		exit(1);
	}
	while (fgets(line, (int)sizeof(line), file) != NULL) {
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		for (index = 0u; line[index] != '\0'; index++) {
			int value = hex_value((unsigned char)line[index]);
			if (line[index] == '\n' || line[index] == '\r')
				break;
			CHECK(value >= 0);
			CHECK(seen < length * 2u);
			if ((seen % 2u) == 0u)
				bytes[seen / 2u] = (uint8_t)(value << 4);
			else
				bytes[seen / 2u] |= (uint8_t)value;
			seen++;
		}
		break;
	}
	CHECK(fclose(file) == 0);
	CHECK(seen == length * 2u);
}

static void fill_payload(uint8_t *payload, uint32_t length, uint8_t seed)
{
	uint32_t index;

	for (index = 0u; index < length; index++)
		payload[index] = (uint8_t)((index * 31u + seed) & 0xffu);
}

/* The link window the fixture session is configured for (the canonical
 * window of the slot-agnostic chooser, 0x60010000) and the Teensy IVT an
 * image linked there carries at +0x1000, so the payload passes the
 * verify-time IVT bounds check and the rows below stay about the curve. */
#define TEST_LINK_BASE UINT32_C(0x60010000)
#define TEST_LINK_BYTES UINT32_C(0x003D8000)

static void put_word(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xffu);
	bytes[1] = (uint8_t)((value >> 8) & 0xffu);
	bytes[2] = (uint8_t)((value >> 16) & 0xffu);
	bytes[3] = (uint8_t)(value >> 24);
}

static void stamp_ivt(uint8_t *payload, uint32_t link_base)
{
	uint8_t *ivt = &payload[AIUS_OTA_IVT_OFFSET];

	memset(ivt, 0, AIUS_OTA_IVT_BYTES);
	put_word(&ivt[AIUS_OTA_IVT_WORD_HEADER * 4u], 0x432000D1u);
	put_word(&ivt[AIUS_OTA_IVT_WORD_ENTRY * 4u], link_base + 0x15f1u);
	put_word(&ivt[4u * 4u],
	         link_base + AIUS_OTA_IVT_OFFSET + AIUS_OTA_IVT_BYTES);
	put_word(&ivt[AIUS_OTA_IVT_WORD_SELF * 4u],
	         link_base + AIUS_OTA_IVT_OFFSET);
}

/* The suite's own CRC-32, used only to re-seal a header after a wire-level
 * mutation -- the same device test/ota/test_ota.c uses, and for the same
 * reason: without it the CRC would stop every mutation before the rule under
 * test could run. */
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

/* Signs `digest` with the committed test secret key and returns the detached
 * 64-byte signature -- TweetNaCl's API is combined (signature||message), so the
 * signature is its first 64 bytes, exactly what the container stores. */
static void sign_digest(const uint8_t digest[AIUS_OTA_DIGEST_BYTES],
	                uint8_t signature[AIUS_OTA_SIGNATURE_BYTES])
{
	uint8_t signed_message[AIUS_OTA_SIGNATURE_BYTES +
		               AIUS_OTA_DIGEST_BYTES];
	unsigned long long signed_length = 0u;

	CHECK(crypto_sign(signed_message, &signed_length, digest,
	                  (unsigned long long)AIUS_OTA_DIGEST_BYTES,
	                  g_secret_key) == 0);
	CHECK(signed_length == sizeof(signed_message));
	memcpy(signature, signed_message, AIUS_OTA_SIGNATURE_BYTES);
}

/* A valid, signed wire header over `payload`. Built through the library's own
 * serializer so the layout has exactly one definition. */
static void build_signed_wire(uint8_t *wire, const uint8_t *payload,
	                      uint32_t image_length)
{
	AiusOtaImageHeader header;
	uint8_t auth_digest[AIUS_OTA_DIGEST_BYTES];

	memset(&header, 0, sizeof(header));
	header.magic = AIUS_OTA_MAGIC;
	header.format_version = (uint16_t)AIUS_OTA_FORMAT_VERSION;
	header.header_length = (uint16_t)AIUS_OTA_HEADER_BYTES;
	header.image_length = image_length;
	header.signature_scheme = (uint16_t)AIUS_OTA_SIGSCHEME_ED25519;
	header.build_utc = TEST_BUILD_UTC;
	memcpy(header.target_id, TEST_TARGET_ID, sizeof(TEST_TARGET_ID));
	memcpy(header.build_revision, TEST_REVISION, sizeof(TEST_REVISION));
	aius_sha256(payload, image_length, header.payload_digest);
	/* The signature slot sits outside the authenticated prefix, so the
	 * digest is final before the slot is filled. */
	memset(header.signature, 0, sizeof(header.signature));
	CHECK(aius_ota_header_serialize(&header, wire,
	                                AIUS_OTA_HEADER_BYTES) == AIUS_OTA_OK);
	CHECK(aius_ota_header_auth_digest(wire, AIUS_OTA_HEADER_BYTES,
	                                  auth_digest) == AIUS_OTA_OK);
	sign_digest(auth_digest, header.signature);
	CHECK(aius_ota_header_serialize(&header, wire,
	                                AIUS_OTA_HEADER_BYTES) == AIUS_OTA_OK);
}

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
	}
}

/* -------------------------------------------------------------------------
 * 1. RFC 8032 known answers
 * ---------------------------------------------------------------------- */

/* RFC 8032 section 7.1, TEST 1 (empty message) and TEST 2 (one byte, 0x72),
 * verbatim. Both directions are exercised: the signature this library produces
 * must equal the published one, and it must open. A vector that only opened
 * would not catch a signer that produced a different valid signature, which is
 * exactly the failure mode a deterministic scheme is chosen to avoid. */
static const uint8_t k_vector1_seed[32] = {
	0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4,
	0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
	0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60
};
static const uint8_t k_vector1_public[32] = {
	0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3,
	0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
	0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a
};
static const uint8_t k_vector1_signature[64] = {
	0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc,
	0x80, 0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74,
	0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55, 0x5f, 0xb8, 0x82, 0x15,
	0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
	0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43,
	0x8e, 0x7a, 0x10, 0x0b
};

static const uint8_t k_vector2_seed[32] = {
	0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46,
	0xec, 0x11, 0x4e, 0x0f, 0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24,
	0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb
};
static const uint8_t k_vector2_public[32] = {
	0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a, 0xa7,
	0x4d, 0x1b, 0x7e, 0xbc, 0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c,
	0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c
};
static const uint8_t k_vector2_message[1] = { 0x72 };
static const uint8_t k_vector2_signature[64] = {
	0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82, 0x0b,
	0x5f, 0x64, 0x25, 0x40, 0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f,
	0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda, 0x08, 0x5a, 0xc1, 0xe4,
	0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
	0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee, 0xb0, 0x0d, 0x29, 0x16,
	0x12, 0xbb, 0x0c, 0x00
};

static void run_rfc8032_vector(const char *label, const uint8_t seed[32],
	                       const uint8_t public_key[32],
	                       const uint8_t *message, size_t message_length,
	                       const uint8_t expected[64])
{
	uint8_t secret_key[SECRET_KEY_BYTES];
	uint8_t signed_message[64 + 1];
	uint8_t opened[64 + 1];
	unsigned long long signed_length = 0u;
	unsigned long long opened_length = 0u;

	current_key = label;
	CHECK(message_length <= 1u);
	memcpy(secret_key, seed, 32u);
	memcpy(&secret_key[32], public_key, 32u);

	CHECK(crypto_sign(signed_message, &signed_length, message,
	                  (unsigned long long)message_length,
	                  secret_key) == 0);
	CHECK(signed_length == 64u + message_length);
	CHECK(memcmp(signed_message, expected, 64u) == 0);

	CHECK(crypto_sign_open(opened, &opened_length, signed_message,
	                       signed_length, public_key) == 0);
	CHECK(opened_length == message_length);
	CHECK(message_length == 0u ||
	      memcmp(opened, message, message_length) == 0);

	/* One flipped signature bit, refused. */
	signed_message[7] ^= 0x01u;
	CHECK(crypto_sign_open(opened, &opened_length, signed_message,
	                       signed_length, public_key) != 0);
	signed_message[7] ^= 0x01u;

	/* One flipped public-key bit, refused. */
	{
		uint8_t wrong_key[32];
		memcpy(wrong_key, public_key, sizeof(wrong_key));
		wrong_key[0] ^= 0x01u;
		CHECK(crypto_sign_open(opened, &opened_length, signed_message,
		                       signed_length, wrong_key) != 0);
	}
}

static void test_rfc8032_vectors(void)
{
	run_rfc8032_vector("rfc8032.vector1", k_vector1_seed, k_vector1_public,
	                   NULL, 0u, k_vector1_signature);
	run_rfc8032_vector("rfc8032.vector2", k_vector2_seed, k_vector2_public,
	                   k_vector2_message, sizeof(k_vector2_message),
	                   k_vector2_signature);
}

/* -------------------------------------------------------------------------
 * 2. The key context
 * ---------------------------------------------------------------------- */

static void test_key_context_rules(void)
{
	AiusOtaEd25519Key key;
	AiusOtaVerifier verifier;
	uint8_t zeros[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES];

	current_key = "key.init";
	memset(zeros, 0, sizeof(zeros));
	CHECK(aius_ota_ed25519_key_init(NULL, g_public_key,
	                                sizeof(g_public_key)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_ed25519_key_init(&key, NULL, sizeof(g_public_key)) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_ed25519_key_init(&key, g_public_key,
	                                sizeof(g_public_key) - 1u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_ed25519_key_init(&key, g_public_key,
	                                sizeof(g_public_key) + 1u) ==
	      AIUS_OTA_INVALID_ARG);
	/* The all-zero key is an omission, not a value. */
	CHECK(aius_ota_ed25519_key_init(&key, zeros, sizeof(zeros)) ==
	      AIUS_OTA_INVALID_ARG);
	/* Every refusal leaves the context zeroed, so it refuses too. */
	CHECK(memcmp(key.public_key, zeros, sizeof(zeros)) == 0);

	CHECK(aius_ota_ed25519_key_init(&key, g_public_key,
	                                sizeof(g_public_key)) == AIUS_OTA_OK);
	CHECK(memcmp(key.public_key, g_public_key, sizeof(g_public_key)) == 0);

	current_key = "key.verifier";
	CHECK(aius_ota_ed25519_verifier(NULL, &key) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_ed25519_verifier(&verifier, NULL) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_ed25519_verifier(&verifier, &key) == AIUS_OTA_OK);
	CHECK(verifier.ctx == &key);
	CHECK(verifier.verify == aius_ota_ed25519_verify);

	/* A wiped key cannot be bound, and cannot verify if one is smuggled
	 * past the binder. */
	memset(&key, 0, sizeof(key));
	CHECK(aius_ota_ed25519_verifier(&verifier, &key) ==
	      AIUS_OTA_INVALID_ARG);
}

/* -------------------------------------------------------------------------
 * 3. The seam, with real signatures
 * ---------------------------------------------------------------------- */

static void test_seam_accepts_and_refuses(void)
{
	AiusOtaEd25519Key key;
	AiusOtaEd25519Key wiped;
	uint8_t digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t signature[AIUS_OTA_SIGNATURE_BYTES];
	uint8_t mutated[AIUS_OTA_SIGNATURE_BYTES];
	uint8_t other_digest[AIUS_OTA_DIGEST_BYTES];
	unsigned int index;

	current_key = "seam.accept";
	aius_sha256(g_payload, TEST_IMAGE_BYTES, digest);
	sign_digest(digest, signature);
	CHECK(aius_ota_ed25519_key_init(&key, g_public_key,
	                                sizeof(g_public_key)) == AIUS_OTA_OK);
	CHECK(aius_ota_ed25519_verify(digest, signature, &key) ==
	      AIUS_OTA_ED25519_OK);
	/* Verification is a pure function of its three inputs: same answer
	 * every time, no state carried between calls. */
	CHECK(aius_ota_ed25519_verify(digest, signature, &key) ==
	      AIUS_OTA_ED25519_OK);

	current_key = "seam.flipped-signature-bit";
	/* Every byte of the signature, one bit each: the R half (0..31) and the
	 * S half (32..63) fail through different arms of the check, and a
	 * verifier that ignored either half would pass a narrower test. */
	for (index = 0u; index < AIUS_OTA_SIGNATURE_BYTES; index++) {
		memcpy(mutated, signature, sizeof(mutated));
		mutated[index] ^= 0x01u;
		CHECK(aius_ota_ed25519_verify(digest, mutated, &key) ==
		      AIUS_OTA_ED25519_FAULT_SIGNATURE);
	}

	current_key = "seam.flipped-digest-bit";
	for (index = 0u; index < AIUS_OTA_DIGEST_BYTES; index++) {
		memcpy(other_digest, digest, sizeof(other_digest));
		other_digest[index] ^= 0x01u;
		CHECK(aius_ota_ed25519_verify(other_digest, signature, &key) ==
		      AIUS_OTA_ED25519_FAULT_SIGNATURE);
	}

	current_key = "seam.flipped-public-key-bit";
	for (index = 0u; index < AIUS_OTA_ED25519_PUBLIC_KEY_BYTES; index++) {
		AiusOtaEd25519Key wrong;
		uint8_t wrong_bytes[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES];

		memcpy(wrong_bytes, g_public_key, sizeof(wrong_bytes));
		wrong_bytes[index] ^= 0x01u;
		CHECK(aius_ota_ed25519_key_init(&wrong, wrong_bytes,
		                                sizeof(wrong_bytes)) ==
		      AIUS_OTA_OK);
		/* A flipped bit either names a different point or names no
		 * point at all; both are one refusal, deliberately not
		 * distinguished. */
		CHECK(aius_ota_ed25519_verify(digest, signature, &wrong) ==
		      AIUS_OTA_ED25519_FAULT_SIGNATURE);
	}

	current_key = "seam.arguments";
	CHECK(aius_ota_ed25519_verify(NULL, signature, &key) ==
	      AIUS_OTA_ED25519_FAULT_ARGUMENT);
	CHECK(aius_ota_ed25519_verify(digest, NULL, &key) ==
	      AIUS_OTA_ED25519_FAULT_ARGUMENT);
	CHECK(aius_ota_ed25519_verify(digest, signature, NULL) ==
	      AIUS_OTA_ED25519_FAULT_ARGUMENT);

	current_key = "seam.no-key";
	memset(&wiped, 0, sizeof(wiped));
	CHECK(aius_ota_ed25519_verify(digest, signature, &wiped) ==
	      AIUS_OTA_ED25519_FAULT_NO_KEY);
}

/* -------------------------------------------------------------------------
 * 4. Through the session
 * ---------------------------------------------------------------------- */

static void arm_session(AiusOtaSession *session)
{
	CHECK(aius_ota_init(session, TEST_TARGET_ID, strlen(TEST_TARGET_ID)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_latch_arm(session) == AIUS_OTA_OK);
	CHECK(aius_ota_set_link_window(session, TEST_LINK_BASE,
	                               TEST_LINK_BYTES) == AIUS_OTA_OK);
}

static void test_session_reaches_armed(void)
{
	AiusOtaSession session;
	AiusOtaEd25519Key key;
	AiusOtaVerifier verifier;
	AiusOtaProgress progress;
	uint8_t wire[AIUS_OTA_HEADER_BYTES];

	current_key = "session.armed";
	build_signed_wire(wire, g_payload, TEST_IMAGE_BYTES);
	CHECK(aius_ota_ed25519_key_init(&key, g_public_key,
	                                sizeof(g_public_key)) == AIUS_OTA_OK);
	CHECK(aius_ota_ed25519_verifier(&verifier, &key) == AIUS_OTA_OK);

	arm_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	/* 4096 is AIUS_OTA_MAX_CHUNK_BYTES: the image spans one full chunk plus
	 * a short tail, so the last absorb is the one that completes it. */
	stage_all(&session, g_payload, TEST_IMAGE_BYTES,
	          AIUS_OTA_MAX_CHUNK_BYTES);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_STAGED);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_OK);
	CHECK(aius_ota_arm_image(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.state == AIUS_OTA_STATE_ARMED);
	CHECK(progress.fail_reason == AIUS_OTA_OK);
	CHECK(progress.verifier_fault == 0);

	current_key = "session.tampered-signature";
	/* One bit of the stored signature, which is outside the authenticated
	 * prefix -- so the CRC must be re-sealed for the header to parse at all,
	 * and the refusal comes from the curve rather than from the CRC. */
	build_signed_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_SIGNATURE] ^= 0x01u;
	reseal(wire);
	arm_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	stage_all(&session, g_payload, TEST_IMAGE_BYTES, 1024u);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_SIGNATURE);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.state == AIUS_OTA_STATE_FAILED);
	CHECK(progress.fail_reason == AIUS_OTA_SIGNATURE);
	/* The adapter's own fault code reaches the operator verbatim. */
	CHECK(progress.verifier_fault == AIUS_OTA_ED25519_FAULT_SIGNATURE);

	current_key = "session.tampered-identity";
	/* build_revision is INSIDE the authenticated prefix: rewriting it
	 * changes the authentication digest, so the signature no longer covers
	 * this header even though every field is individually well-formed.
	 * This is what signing the prefix rather than the payload digest buys.
	 */
	build_signed_wire(wire, g_payload, TEST_IMAGE_BYTES);
	wire[AIUS_OTA_HEADER_OFFSET_BUILD_REVISION] ^= 0x01u;
	reseal(wire);
	arm_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	stage_all(&session, g_payload, TEST_IMAGE_BYTES, 1024u);
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_SIGNATURE);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.verifier_fault == AIUS_OTA_ED25519_FAULT_SIGNATURE);

	current_key = "session.tampered-payload";
	/* The payload digest is checked before the signature, so a swapped
	 * payload never reaches the curve: DIGEST, and the verifier's fault
	 * stays zero because it was never called. */
	build_signed_wire(wire, g_payload, TEST_IMAGE_BYTES);
	arm_session(&session);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	g_payload[17] ^= 0x01u;
	stage_all(&session, g_payload, TEST_IMAGE_BYTES, 1024u);
	g_payload[17] ^= 0x01u;
	CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_DIGEST);
	CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
	CHECK(progress.fail_reason == AIUS_OTA_DIGEST);
	CHECK(progress.verifier_fault == 0);

	current_key = "session.ivt-refused-after-the-curve";
	/* A correctly signed container whose payload is not an image for this
	 * window (the entry pointer moved into RAM): the real curve accepts,
	 * and only then does the IVT bounds check refuse -- with its own
	 * reason and no verifier fault, since the verifier said yes. */
	{
		uint8_t not_an_image[TEST_IMAGE_BYTES];

		memcpy(not_an_image, g_payload, sizeof(not_an_image));
		put_word(&not_an_image[AIUS_OTA_IVT_OFFSET +
		                       AIUS_OTA_IVT_WORD_ENTRY * 4u],
		         0x20200000u);
		build_signed_wire(wire, not_an_image, TEST_IMAGE_BYTES);
		arm_session(&session);
		CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
		stage_all(&session, not_an_image, TEST_IMAGE_BYTES, 1024u);
		CHECK(aius_ota_verify(&session, &verifier) == AIUS_OTA_IVT);
		CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
		CHECK(progress.state == AIUS_OTA_STATE_FAILED);
		CHECK(progress.fail_reason == AIUS_OTA_IVT);
		CHECK(progress.verifier_fault == 0);
		CHECK(aius_ota_arm_image(&session) == AIUS_OTA_STATE);
		CHECK(aius_ota_abort(&session) == AIUS_OTA_OK);
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
	}

	current_key = "session.wrong-key";
	/* The image is intact and correctly signed; this device simply holds a
	 * different key. Same refusal, which is the point: a fleet keyed to
	 * another signer rejects rather than misinterprets. */
	{
		AiusOtaEd25519Key wrong;
		AiusOtaVerifier wrong_verifier;
		uint8_t wrong_bytes[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES];

		memcpy(wrong_bytes, k_vector1_public, sizeof(wrong_bytes));
		CHECK(aius_ota_ed25519_key_init(&wrong, wrong_bytes,
		                                sizeof(wrong_bytes)) ==
		      AIUS_OTA_OK);
		CHECK(aius_ota_ed25519_verifier(&wrong_verifier, &wrong) ==
		      AIUS_OTA_OK);
		build_signed_wire(wire, g_payload, TEST_IMAGE_BYTES);
		arm_session(&session);
		CHECK(aius_ota_offer(&session, wire, sizeof(wire)) ==
		      AIUS_OTA_OK);
		stage_all(&session, g_payload, TEST_IMAGE_BYTES, 1024u);
		CHECK(aius_ota_verify(&session, &wrong_verifier) ==
		      AIUS_OTA_SIGNATURE);
		CHECK(aius_ota_progress(&session, &progress) == AIUS_OTA_OK);
		CHECK(progress.verifier_fault ==
		      AIUS_OTA_ED25519_FAULT_SIGNATURE);
	}
}

/* -------------------------------------------------------------------------
 * 5. The entropy trap
 * ---------------------------------------------------------------------- */

/* The trap does not return, so it is exercised in a forked child: the parent
 * asserts the child died of SIGABRT rather than carrying on. A stub that
 * returned zeroed "entropy" would let a firmware image generate a key an
 * attacker can reproduce, which is the whole reason the trap exists -- so
 * "it aborts" is the behaviour under test, not an implementation detail. */
static void test_randombytes_traps(void)
{
	pid_t child;
	int status = 0;

	current_key = "randombytes.trap";
	fflush(stdout);
	fflush(stderr);
	puts("  (one deliberate randombytes trap message follows)");
	fflush(stdout);

	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		uint8_t buffer[32];

		memset(buffer, 0xa5, sizeof(buffer));
		randombytes(buffer, sizeof(buffer));
		/* Unreachable: the trap never returns. Exiting with a
		 * distinctive code makes a trap that DID return show up as a
		 * clean exit rather than as a pass. */
		_exit(9);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(!WIFEXITED(status));
	CHECK(WIFSIGNALED(status));
	CHECK(WTERMSIG(status) == SIGABRT);
}

int main(int argc, char **argv)
{
	const char *key_directory;

	if (argc != 2) {
		fprintf(stderr,
		        "usage: test_ota_ed25519 <test-key-directory>\n");
		return 2;
	}
	key_directory = argv[1];
	read_key_file(key_directory, "test-only-signing-key.pub", g_public_key,
	              sizeof(g_public_key));
	read_key_file(key_directory, "test-only-signing-key.sec", g_secret_key,
	              sizeof(g_secret_key));
	/* The committed pair must actually be a pair: TweetNaCl stores the
	 * public key in the secret key's tail, and a mismatched pair would make
	 * every signature below unverifiable for reasons nothing else would
	 * explain. */
	current_key = "committed-key";
	CHECK(memcmp(&g_secret_key[32], g_public_key,
	             AIUS_OTA_ED25519_PUBLIC_KEY_BYTES) == 0);

	fill_payload(g_payload, TEST_IMAGE_BYTES, 0x00u);
	stamp_ivt(g_payload, TEST_LINK_BASE);

	test_rfc8032_vectors();
	test_key_context_rules();
	test_seam_accepts_and_refuses();
	test_session_reaches_armed();
	test_randombytes_traps();
	puts("ota ed25519 adapter tests passed");
	return 0;
}
