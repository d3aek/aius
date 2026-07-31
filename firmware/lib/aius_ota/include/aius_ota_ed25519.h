#ifndef AIUS_OTA_ED25519_H
#define AIUS_OTA_ED25519_H

#include "aius_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Signature scheme 1 (Ed25519-over-auth-digest) as an implementation of
 * lib/aius_ota's injected verifier seam, over vendored TweetNaCl.
 *
 * The division of labour, and why the crypto is not in aius_ota.c: the core
 * stays pure logic with every platform effect injected, and key ownership plus
 * the scheme's arithmetic are exactly the effects it refuses to hold. This file
 * is the one adapter that binds the seam to a concrete scheme; the state
 * machine still proves out host-side against accepting and refusing fakes with
 * no crypto linked at all.
 *
 * WHAT IS SIGNED. The message is the image's 32-byte authentication digest --
 * SHA-256 over the header's authenticated prefix (magic through payload_digest,
 * AIUS_OTA_HEADER_AUTH_BYTES), which aius_ota_verify() computes and hands to
 * the seam. So one signature authenticates target_id, build_revision,
 * build_utc, image_length, format_version, signature_scheme AND the payload
 * digest together. See the tag registry beside AiusOtaSignatureScheme.
 *
 * CONSTANT-TIME HYGIENE, stated exactly. Signature verification here handles no
 * secret: the public key, the signature and the digest are all public by
 * construction, and the secret half of the keypair never exists on the device.
 * There is therefore no secret for a timing side channel to leak, and no
 * constant-time claim is made or needed about this path. That is a fact about
 * the inputs, not a property of the code -- TweetNaCl's verification arithmetic
 * happens to be branch-free over its scalars, but this adapter does not rely on
 * that and does not extend the claim to anything else.
 *
 * WHERE IT RUNS. Staging time only, once per offered image, never on the audio
 * path and never inside a flash erase/program window. It is not bounded-step:
 * one call is a full Ed25519 verification (a scalar multiplication and a
 * SHA-512), which is milliseconds on this part, so the caller schedules it as
 * its own epoch step rather than folding it into one. It is also stack-hungry
 * by embedded standards -- TweetNaCl's crypto_sign_open holds several field
 * elements plus a 64-byte hash on the stack, on the order of 1-2 KB -- so it
 * must not run on a small task stack or from an ISR.
 *
 * PLACEMENT. Everything defined here is FLASHMEM (cold) on the target. The
 * vendored TweetNaCl translation unit is not and cannot be, since it is
 * verbatim, so keeping it out of ITCM is a build-level decision no source file
 * can make. What a future target build has to do about that, with the measured
 * cost of each option, is in aius_ota_ed25519.c's placement note. */

#define AIUS_OTA_ED25519_PUBLIC_KEY_BYTES 32u

/* The verifier context. It carries the public key and nothing else: this
 * adapter has no configuration, and a key rotation is a new key struct rather
 * than a mutation of a live one. Bound to the seam by
 * aius_ota_ed25519_verifier(), which is the only supported way to build an
 * AiusOtaVerifier over it. */
typedef struct {
	uint8_t public_key[AIUS_OTA_ED25519_PUBLIC_KEY_BYTES];
} AiusOtaEd25519Key;

/* The verifier's own return values, latched verbatim by the session as
 * `verifier_fault` and published on a status endpoint, so an operator sees WHY
 * the check failed. Deliberately distinct values rather than a bare -1: "the
 * signature does not match" and "this board was never given a key" are
 * different operational problems with different fixes. */
typedef enum {
	AIUS_OTA_ED25519_OK = 0,
	/* A null digest, signature or context reached the seam. */
	AIUS_OTA_ED25519_FAULT_ARGUMENT = 1,
	/* The context holds the all-zero key, i.e. no key was ever installed.
	 * Fail-closed: an unconfigured device refuses every image rather than
	 * verifying against a key an attacker can also produce. */
	AIUS_OTA_ED25519_FAULT_NO_KEY = 2,
	/* crypto_sign_open() refused: wrong key, tampered signature, or a
	 * digest that is not the one signed. The three are indistinguishable by
	 * construction and are not separated here. */
	AIUS_OTA_ED25519_FAULT_SIGNATURE = 3,
	/* Accepted, but the recovered message is not the digest handed in.
	 * Unreachable while TweetNaCl keeps its documented contract (it only
	 * succeeds when the recovered message is the tail of the input it was
	 * given); kept as a cheap check on the vendored library rather than as
	 * a fault this code expects to see. */
	AIUS_OTA_ED25519_FAULT_MESSAGE = 4
} AiusOtaEd25519Fault;

/* Copies a raw 32-byte public key into `key`.
 *
 * INVALID_ARG on a null argument, a length other than
 * AIUS_OTA_ED25519_PUBLIC_KEY_BYTES, or an all-zero key -- zero is what an
 * uninitialised or wiped key slot reads as, so it is refused as an omission
 * rather than accepted as a value. `key` is zeroed (and therefore refuses
 * everything) on any refusal. */
AiusOtaStatus aius_ota_ed25519_key_init(AiusOtaEd25519Key *key,
	                                const uint8_t *bytes, size_t length);

/* Binds `key` into the seam: verifier->ctx = key, verifier->verify =
 * aius_ota_ed25519_verify. `key` is NOT copied, so it must outlive every
 * aius_ota_verify() call made through the returned verifier -- the composition
 * root's key is normally static storage.
 *
 * INVALID_ARG on a null argument or an all-zero key, for the reason above. */
AiusOtaStatus aius_ota_ed25519_verifier(AiusOtaVerifier *verifier,
	                                AiusOtaEd25519Key *key);

/* The seam function itself, matching AiusOtaVerifyFn. Exposed so a caller can
 * check a detached signature outside a session (the host signer's verify verb
 * does); inside a session it is reached through the verifier above. `ctx` is an
 * AiusOtaEd25519Key *. Returns 0 to accept, otherwise an AiusOtaEd25519Fault. */
int aius_ota_ed25519_verify(const uint8_t digest[AIUS_OTA_DIGEST_BYTES],
	                    const uint8_t signature[AIUS_OTA_SIGNATURE_BYTES],
	                    void *ctx);

/* TweetNaCl's entropy hook, DEFINED HERE AS A TRAP, and never as entropy.
 *
 * The vendored file declares `extern void randombytes(u8 *, u64)` and calls it
 * from the two KEYPAIR GENERATORS only -- crypto_sign_keypair and
 * crypto_box_keypair. Verification never calls it, and Ed25519 signing is
 * deterministic, so it never calls it either. The device has no key to
 * generate: it holds a public key and checks signatures, and key generation is
 * an operator step on a host with real entropy.
 *
 * So the link needs the symbol and the device must never use it. This
 * definition satisfies the linker and FAILS LOUDLY rather than returning: it
 * zeroes the buffer (so a defeated trap yields a visibly dead key rather than
 * plausible entropy) and then does not return -- abort() on the host, and on
 * the target an interrupts-on spin that lets the watchdog reset the board into
 * the recovery path aius_boot already owns. A device that reaches this line has
 * linked key generation into a firmware image that has no business generating
 * keys, and silently handing back zeros would turn that into a signing key an
 * attacker can reproduce.
 *
 * Declared here so callers see the contract and so the host suite can prove the
 * trap fires. Deliberately NOT _Noreturn: the vendored file declares the same
 * symbol without it, and mismatched _Noreturn declarations across translation
 * units are undefined behaviour (C11 6.7.4). The comment is the contract.
 *
 * A host tool that legitimately needs key generation (tools/ota's keygen
 * helper) links TweetNaCl WITHOUT this adapter and supplies its own seeded
 * randombytes. The trap is a strong symbol on purpose -- a weak one could be
 * displaced silently by anything else in a target link, which is precisely the
 * accident it exists to make impossible. */
void randombytes(unsigned char *buffer, unsigned long long length);

#ifdef __cplusplus
}
#endif

#endif
