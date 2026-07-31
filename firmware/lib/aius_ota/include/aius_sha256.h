#ifndef AIUS_SHA256_H
#define AIUS_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-256 (FIPS 180-4), written fresh for this tree: portable C11, no dynamic
 * allocation, no libc dependency beyond <string.h>, and no Arduino headers, so
 * the identical object compiles for the host suites and the Teensy target.
 *
 * It lives inside lib/aius_ota because the OTA image container is its only
 * caller today. The interface is deliberately generic (no OTA type appears in
 * it) so hoisting it to its own library costs a directory move and nothing
 * else, per the tree's "one adapter is a hypothetical seam, two is a real one"
 * rule -- there is exactly one caller, so it does not get its own library yet.
 *
 * The streaming interface is the point: the OTA staging machine absorbs an
 * image in bounded chunks and folds each one into the digest as it arrives, so
 * the verification cost is paid across the whole upload rather than as one
 * multi-megabyte re-read at the end. */

#define AIUS_SHA256_DIGEST_BYTES 32u
#define AIUS_SHA256_BLOCK_BYTES 64u

/* Plain-old-data by construction: no pointers, so a struct copy is a valid
 * snapshot of the hash state. aius_ota_verify() relies on that to finalize a
 * copy and leave the session's own context intact. */
typedef struct {
	uint32_t state[8];
	uint64_t absorbed; /* total bytes fed to update(), for the length pad */
	uint8_t buffer[AIUS_SHA256_BLOCK_BYTES];
	uint8_t buffered; /* 0..63 bytes held back for the next block */
} AiusSha256;

void aius_sha256_init(AiusSha256 *context);

/* Absorbs `length` bytes. A null `data` with a nonzero `length`, or a null
 * context, is a no-op rather than a fault: this is a leaf primitive with no
 * status vocabulary, and every caller in this library validates first. */
void aius_sha256_update(AiusSha256 *context, const uint8_t *data,
	                size_t length);

/* Pads, appends the 64-bit bit length and writes the digest big-endian.
 * The context is spent afterwards; copy the struct first to keep absorbing. */
void aius_sha256_final(AiusSha256 *context,
	               uint8_t digest[AIUS_SHA256_DIGEST_BYTES]);

/* One-shot convenience over the three calls above. */
void aius_sha256(const uint8_t *data, size_t length,
	         uint8_t digest[AIUS_SHA256_DIGEST_BYTES]);

#ifdef __cplusplus
}
#endif

#endif
