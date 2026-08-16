/* aius-ota-cli: build, sign and verify an OTA image container.
 *
 * Built and driven by tools/ota/aius_ota_sign.py; it is not meant to be run by
 * hand. Two verbs, both writing `key=value` lines to stdout so the driver and a
 * human read the same evidence:
 *
 *     aius-ota-cli sign   <image> <secret-key> <target-id> <build-revision> \
 *                         <build-utc> <container-out>
 *     aius-ota-cli verify <container> <public-key> <target-id>
 *
 * WHY THIS EXISTS IN C AT ALL. The container layout is lib/aius_ota's, and the
 * device reads it with aius_ota_header_parse(). A Python re-implementation of
 * the offsets would be a second copy of the format that agrees with the first
 * only until somebody edits one -- and the thing that copy would be trusted to
 * get right is exactly what the signature is computed over. So the signer calls
 * the same aius_ota_header_serialize() / aius_ota_header_auth_digest() the
 * firmware links, and Python drives it.
 *
 * `verify` is not a second implementation either: it drives a real
 * AiusOtaSession through offer -> absorb -> verify -> arm_image, so a container
 * that passes here has passed the device's own state machine, chunk contract
 * and digest rules, with the real Ed25519 adapter as the injected verifier.
 *
 * THE CONTAINER FILE. AIUS_OTA_HEADER_BYTES of header followed by exactly
 * `image_length` payload bytes. That concatenation is this tool's convention,
 * not the library's -- lib/aius_ota never sees a file, only an offered header
 * and a stream of chunks -- and it exists so one artifact carries both halves.
 * The transport slice may frame them differently without touching the format.
 *
 * `sign` verifies its own output before writing it, through the same path
 * `verify` uses, so this tool cannot emit a container it would refuse.
 *
 * Exit codes: 0 ok; 1 usage, I/O or malformed input; 2 a refusal (verification
 * failed, or an output file already exists).
 */

/* open()/fdopen() are POSIX, and -std=c11 hides them behind this. */
#define _POSIX_C_SOURCE 200809L

#include "aius_ota.h"
#include "aius_ota_ed25519.h"
#include "aius_sha256.h"
#include "tweetnacl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXIT_REFUSED 2

#define SECRET_KEY_BYTES 64u

static const char *status_name(AiusOtaStatus status)
{
	switch (status) {
	case AIUS_OTA_OK:
		return "OK";
	case AIUS_OTA_INVALID_ARG:
		return "INVALID_ARG";
	case AIUS_OTA_DISARMED:
		return "DISARMED";
	case AIUS_OTA_STATE:
		return "STATE";
	case AIUS_OTA_MALFORMED:
		return "MALFORMED";
	case AIUS_OTA_UNSUPPORTED:
		return "UNSUPPORTED";
	case AIUS_OTA_RANGE:
		return "RANGE";
	case AIUS_OTA_OVERFLOW:
		return "OVERFLOW";
	case AIUS_OTA_DIGEST:
		return "DIGEST";
	case AIUS_OTA_SIGNATURE:
		return "SIGNATURE";
	case AIUS_OTA_CORRUPT:
		return "CORRUPT";
	case AIUS_OTA_NO_SPACE:
		return "NO_SPACE";
	case AIUS_OTA_GEOMETRY:
		return "GEOMETRY";
	case AIUS_OTA_PRIMITIVE:
		return "PRIMITIVE";
	case AIUS_OTA_VERIFY:
		return "VERIFY";
	case AIUS_OTA_INTERRUPTED:
		return "INTERRUPTED";
	}
	return "UNKNOWN";
}

static const char *state_name(AiusOtaState state)
{
	switch (state) {
	case AIUS_OTA_STATE_IDLE:
		return "IDLE";
	case AIUS_OTA_STATE_OFFERED:
		return "OFFERED";
	case AIUS_OTA_STATE_STAGING:
		return "STAGING";
	case AIUS_OTA_STATE_STAGED:
		return "STAGED";
	case AIUS_OTA_STATE_VERIFIED:
		return "VERIFIED";
	case AIUS_OTA_STATE_ARMED:
		return "ARMED";
	case AIUS_OTA_STATE_ACTIVATION_REQUESTED:
		return "ACTIVATION_REQUESTED";
	case AIUS_OTA_STATE_FAILED:
		return "FAILED";
	}
	return "UNKNOWN";
}

static void print_hex(const char *key, const uint8_t *bytes, size_t length)
{
	size_t index;

	printf("%s=", key);
	for (index = 0u; index < length; index++)
		printf("%02x", bytes[index]);
	printf("\n");
}

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

/* A key file is comment lines starting with '#', then one line of hex. Reads
 * exactly `length` bytes and refuses anything else, so a truncated or
 * doubled-up key file fails here rather than producing a key-shaped prefix. */
static int read_key_file(const char *path, uint8_t *bytes, size_t length)
{
	FILE *file = fopen(path, "r");
	char line[512];
	size_t index;
	size_t seen = 0u;

	if (file == NULL) {
		fprintf(stderr, "aius-ota-cli: cannot read %s: %s\n", path,
		        strerror(errno));
		return 1;
	}
	while (fgets(line, (int)sizeof(line), file) != NULL) {
		size_t text = 0u;
		while (line[text] == ' ' || line[text] == '\t')
			text++;
		if (line[text] == '#' || line[text] == '\n' ||
		    line[text] == '\r' || line[text] == '\0')
			continue;
		for (index = text; line[index] != '\0'; index++) {
			int value = hex_value((unsigned char)line[index]);
			if (line[index] == '\n' || line[index] == '\r')
				break;
			if (value < 0) {
				fprintf(stderr,
				        "aius-ota-cli: %s is not a hex key "
				        "file\n",
				        path);
				(void)fclose(file);
				return 1;
			}
			if (seen >= length * 2u) {
				seen = length * 2u + 1u;
				break;
			}
			if ((seen % 2u) == 0u)
				bytes[seen / 2u] = (uint8_t)(value << 4);
			else
				bytes[seen / 2u] |= (uint8_t)value;
			seen++;
		}
		break;
	}
	(void)fclose(file);
	if (seen != length * 2u) {
		fprintf(stderr,
		        "aius-ota-cli: %s must hold %zu hex characters\n", path,
		        length * 2u);
		return 1;
	}
	return 0;
}

static int read_whole_file(const char *path, uint8_t **bytes, size_t *length)
{
	FILE *file = fopen(path, "rb");
	long size;
	uint8_t *buffer;

	*bytes = NULL;
	*length = 0u;
	if (file == NULL) {
		fprintf(stderr, "aius-ota-cli: cannot read %s: %s\n", path,
		        strerror(errno));
		return 1;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "aius-ota-cli: cannot size %s\n", path);
		(void)fclose(file);
		return 1;
	}
	buffer = (uint8_t *)malloc((size_t)size + 1u);
	if (buffer == NULL) {
		fprintf(stderr, "aius-ota-cli: out of memory for %s\n", path);
		(void)fclose(file);
		return 1;
	}
	if (size > 0 && fread(buffer, 1u, (size_t)size, file) != (size_t)size) {
		fprintf(stderr, "aius-ota-cli: short read on %s\n", path);
		free(buffer);
		(void)fclose(file);
		return 1;
	}
	(void)fclose(file);
	*bytes = buffer;
	*length = (size_t)size;
	return 0;
}

/* O_EXCL for the same reason the keygen helper uses it: a container is
 * evidence, and silently replacing one loses the artifact its hashes name. */
static int write_new_file(const char *path, const uint8_t *bytes, size_t length)
{
	int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	FILE *file;

	if (descriptor < 0) {
		fprintf(stderr, "aius-ota-cli: refusing to write %s: %s\n",
		        path, strerror(errno));
		return errno == EEXIST ? EXIT_REFUSED : 1;
	}
	file = fdopen(descriptor, "wb");
	if (file == NULL) {
		fprintf(stderr, "aius-ota-cli: cannot open %s\n", path);
		(void)close(descriptor);
		return 1;
	}
	if (length > 0u && fwrite(bytes, 1u, length, file) != length) {
		fprintf(stderr, "aius-ota-cli: short write on %s\n", path);
		(void)fclose(file);
		return 1;
	}
	if (fclose(file) != 0) {
		fprintf(stderr, "aius-ota-cli: cannot write %s\n", path);
		return 1;
	}
	return 0;
}

static int copy_text_field(char *destination, size_t capacity,
	                   const char *value, const char *what)
{
	size_t length = strlen(value);
	size_t index;

	if (length == 0u || length > capacity - 1u) {
		fprintf(stderr,
		        "aius-ota-cli: %s must be 1..%zu characters, got %zu\n",
		        what, capacity - 1u, length);
		return 1;
	}
	for (index = 0u; index < length; index++) {
		unsigned char character = (unsigned char)value[index];
		if (character < 0x20u || character > 0x7eu) {
			fprintf(stderr,
			        "aius-ota-cli: %s must be printable ASCII\n",
			        what);
			return 1;
		}
	}
	memset(destination, 0, capacity);
	memcpy(destination, value, length);
	return 0;
}

/* Drives the device's own state machine over a container: offer the header,
 * absorb the payload in AIUS_OTA_MAX_CHUNK_BYTES chunks exactly as the
 * transport will, verify against the injected Ed25519 adapter, then arm.
 * Reports the first refusal and where it happened. */
static int drive_session(const uint8_t *container, size_t container_length,
	                 const uint8_t *public_key, const char *target_id,
	                 int quiet)
{
	AiusOtaSession session;
	AiusOtaEd25519Key key;
	AiusOtaVerifier verifier;
	AiusOtaImageHeader header;
	AiusOtaProgress progress;
	AiusOtaStatus status;
	uint8_t auth_digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t payload_digest[AIUS_OTA_DIGEST_BYTES];
	const uint8_t *payload;
	uint32_t payload_length;
	uint32_t offset;

	if (container_length < AIUS_OTA_HEADER_BYTES) {
		fprintf(stderr, "aius-ota-cli: container is shorter than its "
		                "header\n");
		return 1;
	}
	payload = &container[AIUS_OTA_HEADER_BYTES];
	payload_length = (uint32_t)(container_length - AIUS_OTA_HEADER_BYTES);

	status = aius_ota_ed25519_key_init(&key, public_key,
	                                   AIUS_OTA_ED25519_PUBLIC_KEY_BYTES);
	if (status != AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: unusable public key (%s)\n",
		        status_name(status));
		return 1;
	}
	if (aius_ota_ed25519_verifier(&verifier, &key) != AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: cannot bind the verifier\n");
		return 1;
	}

	status = aius_ota_header_parse(&header, container,
	                               AIUS_OTA_HEADER_BYTES);
	if (status != AIUS_OTA_OK) {
		printf("refusal=header_parse\n");
		printf("status=%s\n", status_name(status));
		return EXIT_REFUSED;
	}
	if (header.image_length != payload_length) {
		printf("refusal=container_length\n");
		printf("header_image_length=%u\n",
		       (unsigned int)header.image_length);
		printf("container_payload_length=%u\n",
		       (unsigned int)payload_length);
		return EXIT_REFUSED;
	}

	status = aius_ota_init(&session, target_id, strlen(target_id));
	if (status != AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: bad target id (%s)\n",
		        status_name(status));
		return 1;
	}
	if (aius_ota_latch_arm(&session) != AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: cannot arm the session\n");
		return 1;
	}
	/* Expect whatever slot the container declares: this drive verifies the
	 * container against a device CONFIGURED FOR IT -- the wrong-hole
	 * refusal is the device's per-fitment judgment, not the signer's. */
	if (aius_ota_set_expected_slot(&session, header.slot_id) !=
	    AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: cannot set the expected slot\n");
		return 1;
	}

	status = aius_ota_offer(&session, container, AIUS_OTA_HEADER_BYTES);
	if (status != AIUS_OTA_OK) {
		printf("refusal=offer\n");
		printf("status=%s\n", status_name(status));
		return EXIT_REFUSED;
	}

	offset = 0u;
	while (offset < payload_length) {
		uint32_t take = payload_length - offset;
		if (take > AIUS_OTA_MAX_CHUNK_BYTES)
			take = AIUS_OTA_MAX_CHUNK_BYTES;
		status = aius_ota_absorb(&session, offset, &payload[offset],
		                         take);
		if (status != AIUS_OTA_OK) {
			printf("refusal=absorb\n");
			printf("status=%s\n", status_name(status));
			printf("offset=%u\n", (unsigned int)offset);
			return EXIT_REFUSED;
		}
		offset += take;
	}

	status = aius_ota_verify(&session, &verifier);
	if (status != AIUS_OTA_OK) {
		(void)aius_ota_progress(&session, &progress);
		printf("refusal=verify\n");
		printf("status=%s\n", status_name(status));
		printf("verifier_fault=%d\n", progress.verifier_fault);
		printf("state=%s\n", state_name(progress.state));
		return EXIT_REFUSED;
	}
	status = aius_ota_arm_image(&session);
	if (status != AIUS_OTA_OK) {
		printf("refusal=arm\n");
		printf("status=%s\n", status_name(status));
		return EXIT_REFUSED;
	}

	if (quiet)
		return 0;

	aius_sha256(payload, payload_length, payload_digest);
	(void)aius_ota_header_auth_digest(container, AIUS_OTA_HEADER_BYTES,
	                                  auth_digest);
	(void)aius_ota_progress(&session, &progress);
	printf("state=%s\n", state_name(progress.state));
	printf("target_id=%s\n", header.target_id);
	printf("build_revision=%s\n", header.build_revision);
	printf("build_utc=%llu\n", (unsigned long long)header.build_utc);
	printf("signature_scheme=%u\n",
	       (unsigned int)header.signature_scheme);
	printf("image_length=%u\n", (unsigned int)header.image_length);
	printf("header_bytes=%u\n", (unsigned int)AIUS_OTA_HEADER_BYTES);
	printf("auth_bytes=%u\n", (unsigned int)AIUS_OTA_HEADER_AUTH_BYTES);
	print_hex("payload_sha256", payload_digest, sizeof(payload_digest));
	print_hex("auth_digest", auth_digest, sizeof(auth_digest));
	print_hex("signature", header.signature, sizeof(header.signature));
	print_hex("public_key", public_key,
	          AIUS_OTA_ED25519_PUBLIC_KEY_BYTES);
	return 0;
}

static int command_sign(int argc, char **argv)
{
	AiusOtaImageHeader header;
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	uint8_t secret_key[SECRET_KEY_BYTES];
	uint8_t auth_digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t signed_message[AIUS_OTA_SIGNATURE_BYTES +
		               AIUS_OTA_DIGEST_BYTES];
	uint8_t container_digest[AIUS_OTA_DIGEST_BYTES];
	unsigned long long signed_length = 0u;
	uint8_t *image = NULL;
	uint8_t *container = NULL;
	size_t image_length = 0u;
	size_t container_length;
	unsigned long long build_utc;
	char *end = NULL;
	int status;

	uint8_t slot_id = AIUS_OTA_SLOT_NONE;

	if (argc != 8 && argc != 9) {
		fprintf(stderr,
		        "usage: aius-ota-cli sign <image> <secret-key> "
		        "<target-id> <build-revision> <build-utc> "
		        "<container-out> [a|b]\n");
		return 1;
	}
	if (argc == 9) {
		/* The A/B slot tag (aius-ota-resilience.md Phase 3b): which hole
		 * this image was LINKED for. Omitted = the untagged single-image
		 * container. */
		if (strcmp(argv[8], "a") == 0) {
			slot_id = AIUS_OTA_SLOT_A;
		} else if (strcmp(argv[8], "b") == 0) {
			slot_id = AIUS_OTA_SLOT_B;
		} else {
			fprintf(stderr,
			        "aius-ota-cli: slot must be 'a' or 'b'\n");
			return 1;
		}
	}
	if (read_whole_file(argv[2], &image, &image_length) != 0)
		return 1;
	if (image_length == 0u ||
	    image_length > (size_t)AIUS_OTA_MAX_IMAGE_BYTES) {
		fprintf(stderr,
		        "aius-ota-cli: image must be 1..%u bytes, got %zu\n",
		        (unsigned int)AIUS_OTA_MAX_IMAGE_BYTES, image_length);
		free(image);
		return 1;
	}
	if (read_key_file(argv[3], secret_key, sizeof(secret_key)) != 0) {
		free(image);
		return 1;
	}
	errno = 0;
	build_utc = strtoull(argv[6], &end, 10);
	if (errno != 0 || end == argv[6] || *end != '\0') {
		fprintf(stderr, "aius-ota-cli: build-utc must be a Unix "
		                "timestamp in seconds\n");
		free(image);
		return 1;
	}

	memset(&header, 0, sizeof(header));
	header.magic = AIUS_OTA_MAGIC;
	header.format_version = (uint16_t)AIUS_OTA_FORMAT_VERSION;
	header.header_length = (uint16_t)AIUS_OTA_HEADER_BYTES;
	header.image_length = (uint32_t)image_length;
	header.signature_scheme = (uint16_t)AIUS_OTA_SIGSCHEME_ED25519;
	header.slot_id = slot_id;
	header.build_utc = (uint64_t)build_utc;
	if (copy_text_field(header.target_id, sizeof(header.target_id), argv[4],
	                    "target-id") != 0 ||
	    copy_text_field(header.build_revision,
	                    sizeof(header.build_revision), argv[5],
	                    "build-revision") != 0) {
		free(image);
		return 1;
	}
	aius_sha256(image, image_length, header.payload_digest);

	/* Serialize once with the signature slot still zero: the authenticated
	 * prefix stops before that slot, so the digest signed below is already
	 * final. The second serialize only fills the slot and re-CRCs. */
	if (aius_ota_header_serialize(&header, wire, sizeof(wire)) !=
	    AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: cannot serialize the header\n");
		free(image);
		return 1;
	}
	if (aius_ota_header_auth_digest(wire, sizeof(wire), auth_digest) !=
	    AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: cannot digest the header\n");
		free(image);
		return 1;
	}
	/* Ed25519 signing is deterministic and calls no entropy source: this
	 * binary links lib/aius_ota's randombytes TRAP, so a scheme that did
	 * reach for randomness would abort here rather than sign. */
	if (crypto_sign(signed_message, &signed_length, auth_digest,
	                (unsigned long long)sizeof(auth_digest),
	                secret_key) != 0 ||
	    signed_length != sizeof(signed_message)) {
		fprintf(stderr, "aius-ota-cli: signing failed\n");
		free(image);
		return 1;
	}
	memcpy(header.signature, signed_message, AIUS_OTA_SIGNATURE_BYTES);
	if (aius_ota_header_serialize(&header, wire, sizeof(wire)) !=
	    AIUS_OTA_OK) {
		fprintf(stderr, "aius-ota-cli: cannot re-serialize the "
		                "header\n");
		free(image);
		return 1;
	}

	container_length = AIUS_OTA_HEADER_BYTES + image_length;
	container = (uint8_t *)malloc(container_length);
	if (container == NULL) {
		fprintf(stderr, "aius-ota-cli: out of memory\n");
		free(image);
		return 1;
	}
	memcpy(container, wire, AIUS_OTA_HEADER_BYTES);
	memcpy(&container[AIUS_OTA_HEADER_BYTES], image, image_length);
	free(image);

	/* Self-check before the artifact exists: the container this tool is
	 * about to write must pass the device's own state machine. */
	status = drive_session(container, container_length,
	                       &secret_key[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES],
	                       header.target_id, 1);
	if (status != 0) {
		fprintf(stderr, "aius-ota-cli: refusing to write a container "
		                "this tool cannot verify\n");
		free(container);
		return status == EXIT_REFUSED ? 1 : status;
	}

	status = write_new_file(argv[7], container, container_length);
	if (status != 0) {
		free(container);
		return status;
	}
	aius_sha256(container, container_length, container_digest);

	printf("target_id=%s\n", header.target_id);
	printf("build_revision=%s\n", header.build_revision);
	printf("build_utc=%llu\n", (unsigned long long)header.build_utc);
	printf("signature_scheme=%u\n",
	       (unsigned int)header.signature_scheme);
	printf("image_length=%u\n", (unsigned int)header.image_length);
	printf("header_bytes=%u\n", (unsigned int)AIUS_OTA_HEADER_BYTES);
	printf("auth_bytes=%u\n", (unsigned int)AIUS_OTA_HEADER_AUTH_BYTES);
	printf("container_bytes=%zu\n", container_length);
	printf("container_path=%s\n", argv[7]);
	print_hex("payload_sha256", header.payload_digest,
	          sizeof(header.payload_digest));
	print_hex("auth_digest", auth_digest, sizeof(auth_digest));
	print_hex("signature", header.signature, sizeof(header.signature));
	print_hex("public_key",
	          &secret_key[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES],
	          AIUS_OTA_ED25519_PUBLIC_KEY_BYTES);
	print_hex("container_sha256", container_digest,
	          sizeof(container_digest));
	free(container);
	return 0;
}

static int command_verify(int argc, char **argv)
{
	uint8_t public_key[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES];
	uint8_t *container = NULL;
	size_t container_length = 0u;
	int status;

	if (argc != 5) {
		fprintf(stderr, "usage: aius-ota-cli verify <container> "
		                "<public-key> <target-id>\n");
		return 1;
	}
	if (read_key_file(argv[3], public_key, sizeof(public_key)) != 0)
		return 1;
	if (read_whole_file(argv[2], &container, &container_length) != 0)
		return 1;
	status = drive_session(container, container_length, public_key, argv[4],
	                       0);
	free(container);
	return status;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: aius-ota-cli <sign|verify> ...\n");
		return 1;
	}
	if (strcmp(argv[1], "sign") == 0)
		return command_sign(argc, argv);
	if (strcmp(argv[1], "verify") == 0)
		return command_verify(argc, argv);
	fprintf(stderr, "aius-ota-cli: unknown command %s\n", argv[1]);
	return 1;
}
