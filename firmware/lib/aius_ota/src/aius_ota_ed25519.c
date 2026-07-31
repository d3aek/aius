#include "aius_ota_ed25519.h"

#include "tweetnacl.h"

#include <stddef.h>
#include <string.h>

#if !defined(__IMXRT1062__)
#include <stdio.h>
#include <stdlib.h>
#endif

/* Cold code -> FLASH, for the reasons aius_ota.c's header block gives: nothing
 * here runs per audio block, and the attribute must sit on each DEFINITION
 * because GCC does not merge a `section` attribute from a forward declaration.
 *
 * PLACEMENT ON THE TARGET, AND WHAT A FUTURE TARGET BUILD MUST DO.
 *
 * Nothing in lib/aius_ota is linked into a firmware image today -- the OTA
 * build option is not wired yet, which is why `aius_ota` appears in neither
 * platformio.ini nor test/itcm-placement/verify_target.py. When it is wired,
 * the ITCM guard reacts to HOW the two kinds of translation unit are compiled,
 * and the two are not symmetric:
 *
 *   - This adapter, aius_ota.c and aius_sha256.c carry FLASHMEM on every
 *     definition, so they emit `.flashmem` rather than `.text*` and pass the
 *     guard as they stand. They belong in COLD_OBJECTS, with lib/aius_ota/ in
 *     COLD_TREES, so that a later unannotated definition in this library fails
 *     the build instead of quietly taking ITCM.
 *
 *   - vendor/tweetnacl/tweetnacl.c is verbatim and has no annotation to give.
 *     Compiled as part of an LDF-VISIBLE LIBRARY -- which is what it would
 *     become if it were copied or symlinked under lib/aius_ota/ -- it trips the
 *     guard's default-deny LDF arm, and the only remedy that arm offers is
 *     ITCM_ALLOWED_LDF_LIBS, which exempts the WHOLE library and would take
 *     this adapter and the OTA core out of the gate with it. So it must not be
 *     built as part of this library. The tree's precedent for a verbatim
 *     vendored .c is vendor/dcadec-core: named one file at a time in
 *     build_src_filter, which lands objects at
 *     $BUILD_DIR/vendor/<name>/<file>.c.o -- a path the LDF gate's
 *     lib<hash>/<libname>/ pattern does not match and COLD_TREES' lib/aius_*
 *     prefixes do not cover. The host suite already follows that shape: the
 *     `ota` arm of scripts/test.sh compiles the vendored file to its own
 *     object with its accommodations scoped to it.
 *
 * That precedent carries a cost worth stating rather than inheriting silently.
 * A build_src_filter object is invisible to EVERY check in the placement guard,
 * and link/aius_teensy41.ld sends `*(.text*)` to ITCM, so the vendored file
 * would sit in ITCM by default. Measured with the pinned toolchain
 * (arm-none-eabi-gcc -O2 -mcpu=cortex-m7 -ffunction-sections, 2026-07-31):
 * 7,692 B of `.text` for the whole translation unit, of which ~4,528 B is the
 * crypto_sign_open call graph that survives --gc-sections, plus roughly 1.9 KB
 * of its 2,117 B `.rodata` -- and that linker script collects `*(.rodata*)`
 * into DTCM, the scarcest bank on this board. Verification runs once per staged
 * image, at staging time, never per audio block; none of it belongs in ITCM.
 *
 * So the target slice has to choose deliberately between:
 *   a. accepting ~4.5 KB of ITCM (the dcadec precedent, unpriced by the guard),
 *      or
 *   b. pinning the vendored object into flash in link/aius_teensy41.ld --
 *      EXCLUDE_FILE(*tweetnacl.c.o) on `.text.itcm`'s `*(.text*)`, and
 *      `*tweetnacl.c.o(.text*)` collected in `.text.code` beside
 *      `*(.flashmem*)` -- which also means updating
 *      test/boot-target/test_ldscript_drift.py, the guard that keeps that
 *      copied script honest against the framework's.
 *
 * Neither choice is this slice's to make: no target build links any of this
 * yet, and platformio.ini and the placement guard are deliberately untouched
 * here. */
#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

/* The container's slot sizes and the scheme's are the same numbers; assert it
 * rather than trust the coincidence, so a format change that widened either
 * fails here instead of overflowing a buffer below. */
_Static_assert(AIUS_OTA_SIGNATURE_BYTES == crypto_sign_BYTES,
               "the container's signature slot is one Ed25519 signature");
_Static_assert(AIUS_OTA_ED25519_PUBLIC_KEY_BYTES == crypto_sign_PUBLICKEYBYTES,
               "the key context holds one Ed25519 public key");
_Static_assert(AIUS_OTA_DIGEST_BYTES == 32u,
               "the signed message is one SHA-256 digest");

/* TweetNaCl's signature API is combined, not detached: crypto_sign_open() takes
 * signature||message and writes the recovered message out. The container stores
 * the signature detached from the digest it covers, so the seam reassembles the
 * 96 bytes on the stack for the call and takes the recovered message back. */
#define SIGNED_MESSAGE_BYTES (AIUS_OTA_SIGNATURE_BYTES + AIUS_OTA_DIGEST_BYTES)

/* Visits every position, no early return, in the shape lib/aius_ota and
 * lib/aius_auth use. Nothing here is secret (see the header's constant-time
 * note); this is house style for a byte-block comparison, not a claim. */
static FLASHMEM bool blocks_differ(const uint8_t *left, const uint8_t *right,
	                           size_t length)
{
	uint8_t accumulator = 0u;
	size_t index;

	for (index = 0u; index < length; index++)
		accumulator |= (uint8_t)(left[index] ^ right[index]);
	return accumulator != 0u;
}

static FLASHMEM bool block_is_zero(const uint8_t *bytes, size_t length)
{
	uint8_t accumulator = 0u;
	size_t index;

	for (index = 0u; index < length; index++)
		accumulator |= bytes[index];
	return accumulator == 0u;
}

FLASHMEM AiusOtaStatus aius_ota_ed25519_key_init(AiusOtaEd25519Key *key,
	                                         const uint8_t *bytes,
	                                         size_t length)
{
	if (key == NULL)
		return AIUS_OTA_INVALID_ARG;
	memset(key, 0, sizeof(*key));
	if (bytes == NULL || length != AIUS_OTA_ED25519_PUBLIC_KEY_BYTES)
		return AIUS_OTA_INVALID_ARG;
	if (block_is_zero(bytes, length))
		return AIUS_OTA_INVALID_ARG;
	memcpy(key->public_key, bytes, AIUS_OTA_ED25519_PUBLIC_KEY_BYTES);
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_ed25519_verifier(AiusOtaVerifier *verifier,
	                                         AiusOtaEd25519Key *key)
{
	if (verifier == NULL || key == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (block_is_zero(key->public_key, AIUS_OTA_ED25519_PUBLIC_KEY_BYTES))
		return AIUS_OTA_INVALID_ARG;
	verifier->ctx = key;
	verifier->verify = aius_ota_ed25519_verify;
	return AIUS_OTA_OK;
}

FLASHMEM int aius_ota_ed25519_verify(
	const uint8_t digest[AIUS_OTA_DIGEST_BYTES],
	const uint8_t signature[AIUS_OTA_SIGNATURE_BYTES], void *ctx)
{
	const AiusOtaEd25519Key *key = (const AiusOtaEd25519Key *)ctx;
	uint8_t signed_message[SIGNED_MESSAGE_BYTES];
	uint8_t opened[SIGNED_MESSAGE_BYTES];
	unsigned long long opened_length = 0u;

	if (digest == NULL || signature == NULL || key == NULL)
		return AIUS_OTA_ED25519_FAULT_ARGUMENT;
	/* Re-checked here and not only at key_init: a session restored from a
	 * zeroed struct, or a composition root that skipped the key install,
	 * must refuse rather than ask the curve about the identity point. */
	if (block_is_zero(key->public_key, AIUS_OTA_ED25519_PUBLIC_KEY_BYTES))
		return AIUS_OTA_ED25519_FAULT_NO_KEY;

	memcpy(signed_message, signature, AIUS_OTA_SIGNATURE_BYTES);
	memcpy(&signed_message[AIUS_OTA_SIGNATURE_BYTES], digest,
	       AIUS_OTA_DIGEST_BYTES);

	/* `opened` must be as large as the whole signed message: TweetNaCl uses
	 * it as scratch for the hash input before it is the output buffer. */
	if (crypto_sign_open(opened, &opened_length, signed_message,
	                     (unsigned long long)SIGNED_MESSAGE_BYTES,
	                     key->public_key) != 0)
		return AIUS_OTA_ED25519_FAULT_SIGNATURE;
	if (opened_length != (unsigned long long)AIUS_OTA_DIGEST_BYTES)
		return AIUS_OTA_ED25519_FAULT_MESSAGE;
	if (blocks_differ(opened, digest, AIUS_OTA_DIGEST_BYTES))
		return AIUS_OTA_ED25519_FAULT_MESSAGE;
	return AIUS_OTA_ED25519_OK;
}

/* The entropy trap. Contract, rationale and the reason it is a strong symbol
 * are in the header beside the declaration. */
FLASHMEM void randombytes(unsigned char *buffer, unsigned long long length)
{
	unsigned long long index;

	for (index = 0u; index < length; index++)
		buffer[index] = 0u;

#if defined(__IMXRT1062__)
	/* Loud on the board is a watchdog reset, not a print: this runs before
	 * any transport is trustworthy, and aius_boot already turns repeated
	 * early resets into recovery-safe mode with a physical recovery
	 * channel. Interrupts stay on so the watchdog can actually fire. */
	for (;;) {
	}
#else
	fputs("aius_ota_ed25519: randombytes() was called -- this firmware has "
	      "linked TweetNaCl key generation, which it must never do. The "
	      "device verifies signatures; keys are generated on a host by "
	      "tools/ota/aius_ota_sign.py keygen.\n",
	      stderr);
	fflush(stderr);
	abort();
#endif
}
