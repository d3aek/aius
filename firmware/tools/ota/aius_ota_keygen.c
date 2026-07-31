/* aius-ota-keygen: Ed25519 keypair generation for OTA image signing.
 *
 * Built and driven by tools/ota/aius_ota_sign.py; it is not meant to be run by
 * hand. Reads a 32-byte seed as 64 hex characters ON STDIN and writes the
 * keypair to two files, refusing to overwrite either.
 *
 *     aius-ota-keygen <public-out> <secret-out>   # seed hex on stdin
 *
 * WHY THIS IS A SEPARATE BINARY FROM THE SIGNER. TweetNaCl's key generation
 * calls the external `randombytes`, and lib/aius_ota's adapter defines that
 * symbol as a TRAP that never returns -- the device must never generate a key,
 * and a firmware image that linked key generation is a defect, not a feature.
 * So the two cannot share a link: this helper links the vendored library alone
 * and supplies the seeded `randombytes` below, while the signer links the
 * adapter and therefore carries the trap. The signer never trips it, which is
 * itself the proof that Ed25519 signing needs no entropy.
 *
 * WHY THE SEED COMES FROM OUTSIDE. Passing it in makes generation reproducible
 * (the committed test-only key is regenerable from a published seed) and keeps
 * the entropy decision in one place -- the Python driver's os.urandom(32) for a
 * real key. Stdin rather than argv so a seed never lands in a process listing.
 *
 * Exit codes: 0 ok; 1 usage, I/O or malformed seed; 2 an output file exists.
 */

/* open()/fdopen()/_exit() are POSIX, and -std=c11 hides them behind this. */
#define _POSIX_C_SOURCE 200809L

#include "tweetnacl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEED_BYTES 32u
#define PUBLIC_BYTES 32u
#define SECRET_BYTES 64u

static unsigned char g_seed[SEED_BYTES];
static unsigned long long g_seed_taken;

/* The seeded entropy source this helper alone supplies. It hands out the seed
 * exactly once and refuses any further request: crypto_sign_keypair asks for
 * 32 bytes once, and a second request would mean the vendored code is not doing
 * what this tool assumes. */
void randombytes(unsigned char *buffer, unsigned long long length)
{
	if (length != (unsigned long long)SEED_BYTES || g_seed_taken != 0u) {
		fprintf(stderr,
		        "aius-ota-keygen: unexpected randombytes(%llu) request "
		        "(already taken: %llu)\n",
		        length, g_seed_taken);
		_exit(1);
	}
	memcpy(buffer, g_seed, SEED_BYTES);
	g_seed_taken = length;
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

static int read_seed_from_stdin(void)
{
	char text[2u * SEED_BYTES + 8u];
	size_t length = 0u;
	size_t index;
	int character;

	while ((character = fgetc(stdin)) != EOF) {
		if (character == '\n' || character == '\r' ||
		    character == ' ' || character == '\t')
			continue;
		if (length >= sizeof(text)) {
			fprintf(stderr, "aius-ota-keygen: seed is too long\n");
			return 1;
		}
		text[length++] = (char)character;
	}
	if (length != 2u * SEED_BYTES) {
		fprintf(stderr,
		        "aius-ota-keygen: seed must be %u hex characters, got "
		        "%zu\n",
		        2u * SEED_BYTES, length);
		return 1;
	}
	for (index = 0u; index < SEED_BYTES; index++) {
		int high = hex_value((unsigned char)text[index * 2u]);
		int low = hex_value((unsigned char)text[index * 2u + 1u]);
		if (high < 0 || low < 0) {
			fprintf(stderr,
			        "aius-ota-keygen: seed is not hexadecimal\n");
			return 1;
		}
		g_seed[index] = (unsigned char)((high << 4) | low);
	}
	return 0;
}

/* O_EXCL is the refusal: a signing key that silently replaced an existing one
 * would invalidate every image already signed with it, and the operator would
 * find out from a device that stopped accepting upgrades. */
static int write_key_file(const char *path, const char *comment,
	                  const unsigned char *bytes, size_t length,
	                  mode_t mode)
{
	int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
	FILE *file;
	size_t index;

	if (descriptor < 0) {
		fprintf(stderr, "aius-ota-keygen: refusing to write %s: %s\n",
		        path, strerror(errno));
		return errno == EEXIST ? 2 : 1;
	}
	file = fdopen(descriptor, "w");
	if (file == NULL) {
		fprintf(stderr, "aius-ota-keygen: cannot open %s\n", path);
		(void)close(descriptor);
		return 1;
	}
	fputs(comment, file);
	for (index = 0u; index < length; index++)
		fprintf(file, "%02x", bytes[index]);
	fputc('\n', file);
	if (fclose(file) != 0) {
		fprintf(stderr, "aius-ota-keygen: cannot write %s\n", path);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char public_key[PUBLIC_BYTES];
	unsigned char secret_key[SECRET_BYTES];
	size_t index;
	int status;

	if (argc != 3) {
		fprintf(stderr,
		        "usage: aius-ota-keygen <public-out> <secret-out>  "
		        "(32-byte seed as hex on stdin)\n");
		return 1;
	}
	status = read_seed_from_stdin();
	if (status != 0)
		return status;

	crypto_sign_keypair(public_key, secret_key);

	status = write_key_file(
		argv[1],
		"# aius-ota Ed25519 PUBLIC key (32 bytes, hex).\n"
		"# signature_scheme tag 1: Ed25519 over the image "
		"authentication digest.\n",
		public_key, sizeof(public_key), 0644);
	if (status != 0)
		return status;
	status = write_key_file(
		argv[2],
		"# aius-ota Ed25519 SECRET key (64 bytes, hex: 32-byte seed "
		"then 32-byte public key).\n"
		"# Anyone holding this can sign firmware the device will "
		"accept. Keep it offline.\n",
		secret_key, sizeof(secret_key), 0600);
	if (status != 0)
		return status;

	printf("public_key=");
	for (index = 0u; index < sizeof(public_key); index++)
		printf("%02x", public_key[index]);
	printf("\n");
	printf("public_key_path=%s\n", argv[1]);
	printf("secret_key_path=%s\n", argv[2]);
	return 0;
}
