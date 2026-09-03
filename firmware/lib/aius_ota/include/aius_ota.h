#ifndef AIUS_OTA_H
#define AIUS_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Portable OTA core: the image container format and the staged-upgrade state
 * machine, with every platform effect injected.
 *
 * This library is PURE LOGIC. It never touches flash, SD, HTTP, USB or a
 * clock; it holds no image bytes; it allocates nothing. The caller streams the
 * staged image past it and it answers what the machine believes. The flash
 * primitives, the activation verb and the HTTP surface are a later,
 * device-gated slice -- see docs/plans/aius-ota-upgrade.md, whose owner
 * constraints this interface implements:
 *
 *   - Staged single-image recovery posture. There is no slot index anywhere in
 *     this interface: the OTP secondary-image fuse read back unblown on the
 *     board, hardware A/B does not exist without an irreversible burn, and the
 *     burn is ruled out. Nothing here needs revisiting if that changes -- an
 *     image is identified by what it IS (its payload digest), not by where it
 *     sits.
 *   - Image authentication is independent of the transport gate. The device
 *     auth token is FNV-1a-64 over the public MAC and is a physical-access
 *     bootstrap credential, not an authorization boundary. Nothing is armed
 *     for activation until a detached signature over the image's own
 *     authentication digest is accepted by the injected verifier.
 *   - Disabled by default, armed deliberately. A constructed session is
 *     DISARMED and every mutating entry point refuses until the operator arms
 *     the latch. The latch is deliberately NOT part of the persisted record,
 *     so a reboot always comes up disarmed.
 *
 * The whole library is a compile-time option's payload: nothing in the
 * firmware links it until the OTA build option is switched on. It builds and
 * is proven host-side regardless. */

/* --------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------ */

/* Typed result for every operation, in the shape lib/aius_codec_api's
 * AiusStatus established: zero is success, every refusal names its reason, and
 * a non-OK status never leaves the session partially mutated. Illegal
 * transitions are errors, never asserts -- this code runs on a board whose
 * recovery channel is physical. */
typedef enum {
	AIUS_OTA_OK = 0,
	AIUS_OTA_INVALID_ARG, /* null pointer, or a length the interface forbids */
	AIUS_OTA_DISARMED,    /* the feature latch is off; nothing was mutated */
	AIUS_OTA_STATE,       /* a legal call, illegal from the current state */
	AIUS_OTA_MALFORMED,   /* a container field failed a validation rule */
	AIUS_OTA_UNSUPPORTED, /* well-formed, but not for this build */
	AIUS_OTA_RANGE,       /* chunk offset is not the next expected offset */
	AIUS_OTA_OVERFLOW,    /* chunk would exceed the declared image length */
	AIUS_OTA_DIGEST,      /* staged payload digest != the header's digest */
	AIUS_OTA_SIGNATURE,   /* the injected verifier refused the image */
	AIUS_OTA_CORRUPT,     /* a persisted record failed magic/length/CRC */
	AIUS_OTA_NO_SPACE,    /* caller buffer smaller than the fixed record */
	/* The activation sequencer's own refusals (aius_ota_activate.h). Appended
	 * rather than grouped, because the staging record persists `fail_reason`
	 * as a byte: renumbering an existing value would re-read every record
	 * already written as a different reason. */
	AIUS_OTA_GEOMETRY,    /* an address or length the flash geometry forbids */
	AIUS_OTA_PRIMITIVE,   /* an injected activation primitive refused */
	AIUS_OTA_VERIFY,      /* a flash readback did not match the staged image */
	AIUS_OTA_INTERRUPTED, /* a prior activation is recorded in progress */
	/* The verify-time IVT bounds check (aius-outstanding-work-2026-09.md
	 * Phase 6): the staged payload is authentic but its image vector table
	 * is not an image linked for this device's flash window -- the tag,
	 * the entry pointer or the IVT self pointer is out of bounds, the image
	 * is too short to hold one, or the composition configured no window.
	 * Appended for the same record-persistence reason as the block above. */
	AIUS_OTA_IVT
} AiusOtaStatus;

/* --------------------------------------------------------------------------
 * Image container
 * ------------------------------------------------------------------------ */

/* "AIUO" as little-endian bytes, so a hex dump of the first four bytes reads
 * 41 49 55 4f -- the same human-readable-marker convention lib/aius_boot uses
 * for its retained "AIUB" ledger and "AIUA" abort latch.
 *
 * EVERY multi-byte field is little-endian. The target is a little-endian
 * Cortex-M7 and the host suites run little-endian, so this is the byte order
 * that needs no swap on either side; the digest is the one exception, being
 * big-endian by SHA-256's own definition. */
#define AIUS_OTA_MAGIC UINT32_C(0x4f554941)
#define AIUS_OTA_FORMAT_VERSION 1u
#define AIUS_OTA_HEADER_BYTES 256u
#define AIUS_OTA_TARGET_ID_BYTES 32u
#define AIUS_OTA_BUILD_REVISION_BYTES 32u
#define AIUS_OTA_DIGEST_BYTES AIUS_SHA256_DIGEST_BYTES
#define AIUS_OTA_SIGNATURE_BYTES 64u
#define AIUS_OTA_HEADER_RESERVED1_BYTES 68u

/* Field offsets, published because the container IS the interface: a host-side
 * signer, a future bootloader-side reader and this library's own tests must
 * all agree on the layout, and three copies of "88" would be three places to
 * drift. Fixed size, fixed offsets, no length-prefixed or optional fields --
 * so header validation is constant-bounds by construction and no field's value
 * can ever select an offset. */
#define AIUS_OTA_HEADER_OFFSET_MAGIC 0u
#define AIUS_OTA_HEADER_OFFSET_FORMAT_VERSION 4u
#define AIUS_OTA_HEADER_OFFSET_HEADER_LENGTH 6u
#define AIUS_OTA_HEADER_OFFSET_IMAGE_LENGTH 8u
#define AIUS_OTA_HEADER_OFFSET_SIGNATURE_SCHEME 12u
#define AIUS_OTA_HEADER_OFFSET_RESERVED0 14u
#define AIUS_OTA_HEADER_OFFSET_BUILD_UTC 16u
#define AIUS_OTA_HEADER_OFFSET_TARGET_ID 24u
#define AIUS_OTA_HEADER_OFFSET_BUILD_REVISION 56u
#define AIUS_OTA_HEADER_OFFSET_PAYLOAD_DIGEST 88u
#define AIUS_OTA_HEADER_OFFSET_SIGNATURE 120u
#define AIUS_OTA_HEADER_OFFSET_RESERVED1 184u
#define AIUS_OTA_HEADER_OFFSET_CRC32 252u

/* The authenticated prefix: magic through payload_digest inclusive, i.e. every
 * identity field plus the digest of the payload, stopping at the signature
 * slot. The image's AUTHENTICATION DIGEST is SHA-256 over exactly these bytes,
 * and that -- not the bare payload digest -- is what the signature covers and
 * what the injected verifier is handed.
 *
 * Why not sign the payload digest directly: a signature over the payload alone
 * authenticates the bytes but not the claims about them, so a valid image
 * could be replayed against the wrong board with a rewritten target_id, or its
 * build_revision and build_utc rolled back, with the signature still checking
 * out. Signing the prefix binds target_id, build_revision, build_utc,
 * image_length, format_version and signature_scheme to the payload at no
 * per-chunk cost: it is one extra 120-byte hash, computed once, at verify
 * time. The plan asks for "a signature the device verifies over the complete
 * image"; the complete image includes its own claims. */
#define AIUS_OTA_HEADER_AUTH_BYTES 120u

/* Structural bound on a staged image. 8,126,464 is the flash the part leaves
 * addressable to the application (aius-ota-upgrade.md §Sizing), so anything
 * larger cannot be staged on this board whatever the transport says. A staging
 * backend may bound it further; the container will not admit more. */
#define AIUS_OTA_MAX_IMAGE_BYTES UINT32_C(8126464)

/* Per-call absorb bound. The core never blocks, but it does hash, and this
 * firmware's scheduler is a bounded-step epoch loop: one unbounded absorb()
 * would hash megabytes inside a single epoch and drop audio. 4096 is one flash
 * sector, and at roughly 12 cycles/byte on a 600 MHz M7 it costs about 80 us
 * against the audio block's 10,667 us budget. The SD transport's own slice is
 * 512 bytes (AIUS_SD_MAX_TRANSFER), so the live path stays far under this. */
#define AIUS_OTA_MAX_CHUNK_BYTES 4096u

/* THE IMAGE VECTOR TABLE inside the payload. Every Teensy 4 image keeps the
 * boot ROM's layout at its own base: a flash-config sector, then at +0x1000
 * the eight-word IVT the ROM (or the first-stage chooser) parses -- header
 * word (tag 0xD1 in the low byte), entry pointer, two reserved, boot-data
 * pointer, the IVT's own address, CSF, reserved. The payload IS the image as
 * it will sit in flash, so these offsets are payload-relative, and
 * aius_ota_absorb() captures the 32 bytes as they stream past for the
 * verify-time bounds check; the number is the same 0x1000 lib/aius_ab
 * publishes as AIUS_AB_SLOT_IVT_OFFSET and aius_ota_activate.h as
 * AIUS_OTA_FLASH_IVT_OFFSET (pinned equal by _Static_asserts). */
#define AIUS_OTA_IVT_OFFSET 0x1000u
#define AIUS_OTA_IVT_BYTES 32u
#define AIUS_OTA_IVT_WORD_HEADER 0u
#define AIUS_OTA_IVT_WORD_ENTRY 1u
#define AIUS_OTA_IVT_WORD_SELF 5u

/* THE SIGNATURE SCHEME TAG REGISTRY -- the wire values of the `signature_scheme`
 * field at AIUS_OTA_HEADER_OFFSET_SIGNATURE_SCHEME, and the only place a tag is
 * allocated.
 *
 *   0  NONE     No scheme. Not a value an image may carry: header rule 8
 *                refuses it as MALFORMED. It exists only so an unsigned image
 *                has a spelling the validator can reject by name rather than
 *                falling through the "unknown tag" arm as if it were a scheme
 *                from the future.
 *   1  ED25519  Ed25519 (RFC 8032 PureEdDSA, Curve25519, SHA-512) over the
 *                image's 32-byte AUTHENTICATION DIGEST -- i.e. over
 *                aius_ota_header_auth_digest() of the header, NOT over the
 *                payload and NOT over the header's own SHA-512. The device
 *                side is lib/aius_ota/src/aius_ota_ed25519.c over vendored
 *                TweetNaCl; the host side is tools/ota/aius_ota_sign.py.
 *                Public key 32 bytes, signature 64 -- which is why the
 *                container's signature slot is 64.
 *
 * A TAG NAMES THE WHOLE RECIPE, not just the algorithm: the curve, the hash and
 * WHAT IS SIGNED are one indivisible claim, because a verifier that agrees on
 * the algorithm but not on the message accepts a signature over something else.
 * Changing any of the three is a new tag, never a re-reading of an old one.
 *
 * Allocating a tag: append the next integer, never renumber and never reuse a
 * retired one. A device that does not know a tag refuses the image with
 * UNSUPPORTED (header rule 9), which is what lets a fleet carrying mixed
 * firmware reject rather than misinterpret. The signature slot is a fixed 64
 * bytes, so a scheme whose signature does not fit is a format-version bump
 * rather than a variable-length field. */
typedef enum {
	AIUS_OTA_SIGSCHEME_NONE = 0,
	AIUS_OTA_SIGSCHEME_ED25519 = 1
} AiusOtaSignatureScheme;

/* Plausibility window for build_utc, in Unix seconds: [2020-01-01, 2100-01-01).
 * Zero, 1970 and a far-future date all mean the builder did not have a clock,
 * and an image whose provenance timestamp is a lie is refused rather than
 * staged. */
#define AIUS_OTA_MIN_BUILD_UTC UINT64_C(1577836800)
#define AIUS_OTA_MAX_BUILD_UTC UINT64_C(4102444800)

/* Slot identities in the container's slot tag (the low byte of the former
 * reserved0 field, aius-ota-resilience.md Phase 3b): NONE is the untagged
 * single-image container -- byte-identical to a pre-A/B header, which is
 * what lets old devices keep refusing tagged containers as reserved-field
 * violations and new devices keep accepting old containers where the
 * composition allows. Values mirror lib/aius_ab's AIUS_AB_SLOT_*; the tag
 * sits inside the authenticated prefix, so it is signed. */
#define AIUS_OTA_SLOT_NONE 0u
#define AIUS_OTA_SLOT_A 1u
#define AIUS_OTA_SLOT_B 2u

/* The parsed header. Strings arrive NUL-padded on the wire and are stored
 * NUL-TERMINATED here (hence the +1), so a caller can print or strcmp them
 * without carrying a length alongside. */
typedef struct {
	uint32_t magic;
	uint16_t format_version;
	uint16_t header_length;
	uint32_t image_length;
	uint16_t signature_scheme;
	uint8_t slot_id; /* AIUS_OTA_SLOT_*; the image's linked-for slot */
	uint64_t build_utc;
	char target_id[AIUS_OTA_TARGET_ID_BYTES + 1u];
	char build_revision[AIUS_OTA_BUILD_REVISION_BYTES + 1u];
	uint8_t payload_digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t signature[AIUS_OTA_SIGNATURE_BYTES];
	uint32_t header_crc32;
} AiusOtaImageHeader;

/* Parses and fully validates `length` bytes of wire header.
 *
 * Every rule, in the order applied -- the order matters, because a corrupted
 * header must report corruption rather than whichever field the corruption
 * happened to land in:
 *
 *   1. header/bytes non-null and length == AIUS_OTA_HEADER_BYTES ... INVALID_ARG
 *   2. magic == "AIUO" .............................................. MALFORMED
 *   3. header_crc32 == CRC-32 over bytes [0, 252) ................... MALFORMED
 *   4. format_version == 1 ........................................ UNSUPPORTED
 *   5. header_length == 256 (v1 pins it) ............................ MALFORMED
 *   6. image_length != 0 ............................................ MALFORMED
 *   7. image_length <= AIUS_OTA_MAX_IMAGE_BYTES ................... UNSUPPORTED
 *   8. signature_scheme != NONE ..................................... MALFORMED
 *   9. signature_scheme is a known scheme ......................... UNSUPPORTED
 *  10. slot_id (reserved0's LOW byte) is NONE, A or B .............. MALFORMED
 *  10a. reserved0's HIGH byte and reserved1[68] are all zero ........ MALFORMED
 *  11. target_id: non-empty, printable ASCII, NUL-padded to the end . MALFORMED
 *  12. build_revision: same three rules ............................. MALFORMED
 *  13. build_utc in [2020-01-01, 2100-01-01) ....................... MALFORMED
 *  14. payload_digest is not all-zero .............................. MALFORMED
 *  15. signature is not all-zero ................................... MALFORMED
 *
 * Rule 10 is NOT "reserved0 is zero", and the difference is load-bearing for
 * anything that re-implements this parse: the A/B scheme claimed reserved0's
 * low byte as the slot tag, so 0, 1 and 2 are all valid values there and only
 * the high byte kept the reserved-must-be-zero rule. Reading the whole word as
 * reserved refuses every slot-tagged container ever signed. Whether a valid tag
 * is the one THIS board wants is a separate question, asked at offer time
 * against the session's expected slot and answered UNSUPPORTED, not MALFORMED.
 *
 * Rules 14 and 15 reject the "nobody filled this in" header: an all-zero
 * digest or signature is not a value, it is an omission, and a build pipeline
 * that forgot to sign must fail loudly at the device rather than at the
 * verifier.
 *
 * CONSTANT-BOUNDS BY CONSTRUCTION: after the single length check every read is
 * bytes[<compile-time constant>] for a compile-time constant width. No field's
 * value selects an offset, a length or a loop bound, so a hostile header
 * cannot steer the parser. Nothing is retained from `bytes`; the parsed struct
 * is a copy. */
AiusOtaStatus aius_ota_header_parse(AiusOtaImageHeader *header,
	                            const uint8_t *bytes, size_t length);

/* Writes `header` as wire bytes and fills in header_crc32 (the caller's
 * header_crc32 field is ignored and overwritten). The reserved fields are
 * zeroed. Exists for the host-side image signer and for tests, which build
 * valid headers through this interface and then mutate single wire bytes to
 * prove each rejection -- the alternative is a second, drifting copy of the
 * layout in the test file. */
AiusOtaStatus aius_ota_header_serialize(const AiusOtaImageHeader *header,
	                                uint8_t *bytes, size_t length);

/* SHA-256 over the authenticated prefix, bytes [0, AIUS_OTA_HEADER_AUTH_BYTES)
 * of a wire header. This is what a signature covers and what the verifier
 * seam receives. Available separately so the host-side signer computes it from
 * the identical code path the device checks. */
AiusOtaStatus aius_ota_header_auth_digest(
	const uint8_t *bytes, size_t length,
	uint8_t digest[AIUS_OTA_DIGEST_BYTES]);

/* --------------------------------------------------------------------------
 * Verifier seam
 * ------------------------------------------------------------------------ */

/* The signature check, injected. No crypto beyond SHA-256 lives in this
 * library: key ownership, the scheme's arithmetic and any hardware
 * acceleration are the caller's, and this seam is what lets the state machine
 * be proven host-side against an accepting and a refusing verifier without
 * either.
 *
 * Returns 0 to accept; any non-zero value refuses and is latched verbatim in
 * the session as `verifier_fault`, so an operator sees WHY the verifier said
 * no rather than just that it did. */
typedef int (*AiusOtaVerifyFn)(const uint8_t digest[AIUS_OTA_DIGEST_BYTES],
	                       const uint8_t signature[AIUS_OTA_SIGNATURE_BYTES],
	                       void *ctx);

typedef struct {
	void *ctx;
	AiusOtaVerifyFn verify;
} AiusOtaVerifier;

/* --------------------------------------------------------------------------
 * Staged-upgrade state machine
 * ------------------------------------------------------------------------ */

/* The upgrade's whole lifecycle, and the vocabulary the operator-observability
 * table in aius-ota-upgrade.md asks for:
 *
 *   IDLE -offer-> OFFERED -absorb-> STAGING -absorb(last)-> STAGED
 *        -verify-> VERIFIED -arm_image-> ARMED -request_activation->
 *        ACTIVATION_REQUESTED
 *
 * FAILED is entered only by a verification refusal (digest, signature or IVT
 * bounds). A chunk-level refusal (RANGE, OVERFLOW) does NOT fail the session: it mirrors
 * the APPEND_PART protocol, where a bad range is one request's 400 and the
 * client resumes from next_offset. Losing a megabyte of staging to a
 * mis-ordered chunk would be a worse contract than the transport already has.
 *
 * abort() returns any state to IDLE except ACTIVATION_REQUESTED, which has no
 * software exit by design: the request is the last decision before the device
 * commits, and un-requesting it belongs to the activation slice that knows
 * whether programming has begun. A reset clears it. */
typedef enum {
	AIUS_OTA_STATE_IDLE = 0,
	AIUS_OTA_STATE_OFFERED = 1,
	AIUS_OTA_STATE_STAGING = 2,
	AIUS_OTA_STATE_STAGED = 3,
	AIUS_OTA_STATE_VERIFIED = 4,
	AIUS_OTA_STATE_ARMED = 5,
	AIUS_OTA_STATE_ACTIVATION_REQUESTED = 6,
	AIUS_OTA_STATE_FAILED = 7
} AiusOtaState;

/* The feature latch, and the reason it is not spelled "armed": ARMED is also a
 * STATE, meaning "this image is verified and ready to activate". They are
 * different questions. The latch asks "may this device do OTA at all", the
 * state asks "how far has this upgrade got". A session constructs DISARMED and
 * the latch never persists across a reset. */
typedef enum {
	AIUS_OTA_LATCH_DISARMED = 0,
	AIUS_OTA_LATCH_ARMED = 1
} AiusOtaLatch;

typedef struct {
	AiusOtaLatch latch;
	AiusOtaState state;
	/* The status that drove FAILED (DIGEST, SIGNATURE or IVT), AIUS_OTA_OK
	 * otherwise. Latched so the failure survives the call that caused it and
	 * can be published on a status endpoint, and persisted in the record. */
	AiusOtaStatus fail_reason;
	/* The verifier's own return value on a refusal. A LIVE diagnostic: it
	 * is deliberately not persisted, because it describes a check another
	 * boot performed and `fail_reason` already carries the actionable
	 * half. */
	int verifier_fault;
	/* This build's identity, supplied at construction and compared against
	 * every offered header's target_id. The core cannot know its own board;
	 * the composition root does. */
	char target_id[AIUS_OTA_TARGET_ID_BYTES + 1u];
	/* The slot tag every offered container must carry
	 * (aius_ota_set_expected_slot): NONE for the single-image posture,
	 * else the slot the composition will activate into -- its INACTIVE
	 * slot. Like target_id, the session cannot know it; the composition
	 * root does, and re-states it whenever the active slot changes. */
	uint8_t expected_slot;
	/* The flash window an offered image must have been LINKED for
	 * (aius_ota_set_link_window): the canonical window under the
	 * slot-agnostic chooser, the inactive slot's physical window under the
	 * old per-slot chooser, the boot window on the plain single-image build.
	 * A device property like target_id and expected_slot, never persisted.
	 * Zero bytes -- the construction default -- means "no window known" and
	 * verify() refuses every image rather than guess. */
	uint32_t link_base;
	uint32_t link_bytes;
	/* The offered header, kept as wire bytes AND parsed. The bytes are what
	 * the record persists and what the authentication digest is computed
	 * over, so restoring re-runs the one validator rather than trusting a
	 * second decode path. */
	uint8_t header_bytes[AIUS_OTA_HEADER_BYTES];
	AiusOtaImageHeader header;
	uint32_t staged_length;
	AiusSha256 digest;
	/* The payload's IVT, captured byte-for-byte by absorb() as the bytes at
	 * [AIUS_OTA_IVT_OFFSET, +AIUS_OTA_IVT_BYTES) stream past, whatever the
	 * chunk boundaries. Judged by verify() AFTER the digest and signature,
	 * so the words judged are exactly the words that were authenticated; a
	 * staging medium need not be re-read. Bytes not yet staged are zero, and
	 * the record persists the block so a resumed staging keeps it. */
	uint8_t ivt[AIUS_OTA_IVT_BYTES];
} AiusOtaSession;

/* Deliberately absent from the session: the digest computed at verify() and
 * the authentication digest handed to the verifier. Both are derivable --
 * a verified session's payload digest IS `header.payload_digest` (that is what
 * verification proved), and the authentication digest is
 * aius_ota_header_auth_digest(session->header_bytes). Storing 64 bytes of
 * derivable state per session would also make the record either incomplete or
 * 64 bytes fatter, for a value only a failed verification could distinguish. */

/* Everything a status endpoint needs, in one read, covering the plan's
 * offered / staged / verified / activated observability gaps. */
typedef struct {
	AiusOtaState state;
	bool latch_armed;
	uint32_t image_length;
	uint32_t staged_length;
	uint32_t next_offset;
	AiusOtaStatus fail_reason;
	int verifier_fault;
} AiusOtaProgress;

/* Constructs a DISARMED, IDLE session bound to `target_id` (the board/env
 * identity string an offered header must match, e.g. "teensy41-optical").
 * The id is copied; the caller's storage need not outlive the call.
 *
 * INVALID_ARG when the id is absent, empty, longer than
 * AIUS_OTA_TARGET_ID_BYTES or not printable ASCII -- a session that matches
 * any target is a footgun, so there is no "match anything" spelling. The
 * session is zeroed (and therefore refuses everything) either way. */
AiusOtaStatus aius_ota_init(AiusOtaSession *session, const char *target_id,
	                    size_t target_id_length);

/* States which slot tag an offered container must carry from here on:
 * NONE accepts only untagged containers (the single-image posture and the
 * construction default), A or B accepts only that slot's. UNSUPPORTED at
 * offer on a mismatch -- the image is well-formed, just not for this hole,
 * the same verdict a wrong target_id earns. Callable in any state; it
 * guards future offers, never a session already staged. INVALID_ARG for a
 * value outside the slot domain. */
AiusOtaStatus aius_ota_set_expected_slot(AiusOtaSession *session,
	                                 uint8_t slot);

/* States the flash window every offered image must have been LINKED for:
 * [base, base + bytes). verify() judges the staged payload's IVT against it
 * (aius_ab_ivt_sane_in_window plus the IVT self pointer, which must equal
 * base + AIUS_OTA_IVT_OFFSET -- the image "as it would sit in the window").
 * The composition root sets it once from its posture, exactly as it sets the
 * expected slot: the canonical link window under the slot-agnostic chooser,
 * the inactive slot's physical window under the old per-slot chooser, the
 * boot window (AIUS_OTA_FLASH_BASE, +AIUS_OTA_FLASH_USABLE_BYTES) on the plain
 * single-image build. Callable in any state; it governs future verifies.
 * INVALID_ARG for a zero-length window or one that wraps the address space;
 * a session that was never given a window refuses at verify() with IVT. */
AiusOtaStatus aius_ota_set_link_window(AiusOtaSession *session,
	                               uint32_t base, uint32_t bytes);

/* The deliberate act. Arming does not start an upgrade; it makes one
 * possible. */
AiusOtaStatus aius_ota_latch_arm(AiusOtaSession *session);

/* Disarming also clears the session -- an operator saying "stop" should not
 * leave a half-staged image that a later re-arm silently resumes. The one
 * exception is ACTIVATION_REQUESTED, which disarm leaves intact for the same
 * reason abort() refuses it. */
AiusOtaStatus aius_ota_latch_disarm(AiusOtaSession *session);

bool aius_ota_latch_is_armed(const AiusOtaSession *session);

/* IDLE or FAILED -> OFFERED. Validates the header, then checks its target_id
 * against this session's (UNSUPPORTED on mismatch: the image is well-formed,
 * just not for this board). A fresh offer resets the staging digest, so
 * re-offering is how a caller restarts after a failure. */
AiusOtaStatus aius_ota_offer(AiusOtaSession *session, const uint8_t *bytes,
	                     size_t length);

/* PURE preview of the transport's restart-shaped offer: every refusal the
 * chunk route's offset-0 path (abort-if-offered, then offer) would earn,
 * with the session untouched. Exists for the deferred-absorb staging contract
 * (aius-ota-resilience.md Phase 4a): the route must be able to promise a
 * chunk's acceptance BEFORE the SD write commits, and only a check that
 * cannot mutate can be issued twice.
 *
 * The STATE rule is therefore the ROUTE's, not offer()'s own: any state
 * except ACTIVATION_REQUESTED admits, because a re-sent offset 0 is a
 * deliberate restart and abort() reaches IDLE from everything else. All the
 * header rules are aius_ota_header_parse()'s; the target check is offer()'s.
 * `parsed` (optional) receives the validated header so the caller can bound
 * a same-chunk payload remainder against image_length without a second
 * parse. */
AiusOtaStatus aius_ota_offer_check(const AiusOtaSession *session,
	                           const uint8_t *bytes, size_t length,
	                           AiusOtaImageHeader *parsed);

/* Absorbs one chunk of the payload at `offset`. OFFERED or STAGING only.
 *
 * The offset contract is STRICT MONOTONIC CONTIGUOUS, mirroring the
 * APPEND_PART semantics the SD job already enforces (aius_sd.c faults
 * AIUS_SD_FAULT_RANGE unless the existing part's size equals the chunk's
 * start): `offset` must equal aius_ota_next_offset() exactly. A gap, an
 * overlap and a byte-identical re-send of the last chunk are all RANGE.
 *
 * The re-send case is the deliberate one. It is the plausible resume shape --
 * the client sent a chunk, the acknowledgement was lost, it retries -- and it
 * is still refused, because an incremental digest cannot be rewound: accepting
 * a re-send means either re-hashing bytes already folded in (a wrong digest)
 * or trusting that the retry is byte-identical (unverifiable). The client's
 * resume protocol is the one the transport already has: read next_offset and
 * continue from there. A RANGE refusal leaves the session untouched, so that
 * costs one round trip and nothing else.
 *
 * OVERFLOW when the chunk would carry the staged length past image_length; the
 * chunk is refused whole rather than truncated, so the strict contract holds.
 * INVALID_ARG on a zero-length chunk or one over AIUS_OTA_MAX_CHUNK_BYTES.
 * The last chunk that completes image_length promotes the session to STAGED. */
AiusOtaStatus aius_ota_absorb(AiusOtaSession *session, uint32_t offset,
	                      const uint8_t *data, size_t length);

/* PURE preview of aius_ota_absorb(): the exact refusal an absorb of `length`
 * bytes at `offset` would earn right now, with the session untouched -- the
 * bytes themselves are not needed because absorb() never judges content,
 * only geometry and state. Same Phase 4a rationale as offer_check: OK here
 * is the route's license to answer a durable-staging 200 whose absorb runs
 * at the SD write's committed terminal. */
AiusOtaStatus aius_ota_absorb_check(const AiusOtaSession *session,
	                            uint32_t offset, size_t length);

/* STAGED -> VERIFIED, in three independent steps, in this order.
 *
 * First the incremental digest is finalized (on a COPY, so the session's hash
 * state stays valid for a subsequent record save) and compared against the
 * header's payload_digest with a no-early-return comparison. A mismatch is
 * DIGEST and fails the session: the staged bytes are not the offered image and
 * no retry can change that.
 *
 * Then the authentication digest over the header's authenticated prefix is
 * computed and handed, with the detached signature, to the injected verifier.
 * Non-zero is SIGNATURE and fails the session, with the verifier's own return
 * value latched.
 *
 * Last, the authenticated payload's IVT (captured by absorb()) is judged
 * against the link window (aius_ota_set_link_window): the header tag, the
 * entry pointer inside the window, the IVT self pointer at exactly
 * base + AIUS_OTA_IVT_OFFSET. Any miss -- or an image too short to hold an
 * IVT, or no window configured -- is IVT and fails the session the same way.
 * This is the 2026-08-16 guard: a byte-perfect, correctly signed container
 * whose payload is not a bootable image for this window (Intel-hex TEXT that
 * day) is refused before a byte of flash is spent, instead of relying on the
 * chooser's post-flash sanity check to save the slot. Last rather than first
 * because the words are only trustworthy once they are authenticated, and a
 * tampered payload must keep reading as DIGEST.
 *
 * INVALID_ARG when the verifier or its function pointer is absent -- a missing
 * verifier is never treated as an accepting one. */
AiusOtaStatus aius_ota_verify(AiusOtaSession *session,
	                      const AiusOtaVerifier *verifier);

/* VERIFIED -> ARMED. Separate from verify() so that "these bytes are
 * authentic" and "activate them" are two decisions an operator makes
 * separately. */
AiusOtaStatus aius_ota_arm_image(AiusOtaSession *session);

/* ARMED -> ACTIVATION_REQUESTED. This slice stops here: nothing programs
 * flash, nothing reboots. The activation slice reads this state. */
AiusOtaStatus aius_ota_request_activation(AiusOtaSession *session);

/* Any state except ACTIVATION_REQUESTED -> IDLE. */
AiusOtaStatus aius_ota_abort(AiusOtaSession *session);

/* ACTIVATION_REQUESTED -> IDLE: the exit aius_ota_abort() reserved for the
 * activation slice, and the only transition out of that state (a reset is the
 * other way out, and a restore replaces the session rather than moving it).
 *
 * It gives up on a requested activation. It does not, and cannot, undo flash:
 * whether anything was erased is the ACTIVATION SEQUENCER's knowledge, not the
 * session's, so this entry point does not pretend to judge it. The interlock
 * lives where the knowledge is -- aius_ota_activate_step() re-checks the
 * session on every step and halts with FAULT_SESSION if this was called
 * mid-window, rather than programming flash on a withdrawn authorization.
 *
 * The image is cleared, so the next upgrade is a fresh offer. That is the
 * deliberate half: the plan's rule for a failed activation is HalfKay plus an
 * honest state, never a silent retry of the same staged bytes. STATE from any
 * state other than ACTIVATION_REQUESTED -- aius_ota_abort() already covers
 * those, and two spellings of one transition would be one too many. */
AiusOtaStatus aius_ota_abandon_activation(AiusOtaSession *session);

AiusOtaState aius_ota_state(const AiusOtaSession *session);

/* The byte offset the next chunk must carry -- the same number the
 * APPEND_PART transport answers as `next_offset`. Zero for a null or
 * un-offered session. */
uint32_t aius_ota_next_offset(const AiusOtaSession *session);

AiusOtaStatus aius_ota_progress(const AiusOtaSession *session,
	                        AiusOtaProgress *progress);

/* --------------------------------------------------------------------------
 * Power-loss record
 * ------------------------------------------------------------------------ */

/* The staging record: a fixed-size, CRC'd byte layout the caller persists
 * wherever it persists things, so a reset mid-staging resumes instead of
 * restarting a multi-megabyte upload.
 *
 * WHY BYTES AND NOT A STRUCT, unlike lib/aius_boot's retained ledger. The boot
 * ledger lives in retained OCRAM inside one image's own address space, so a
 * struct with an embedded CRC is exactly right for it. An OTA record is
 * written by one image and read by another, possibly after a power cycle, and
 * aius-ota-upgrade.md §The rollback substrate records what that costs: a
 * 16,396-byte DMAMEM addition moved the boot ledger's address by 16 KiB, and
 * two images differ in layout by construction. A byte layout with published
 * offsets and its own CRC is layout-independent by definition -- gaps 3 and 4
 * of that section, answered structurally rather than by convention.
 *
 * The SHA-256 midstate is in the record, and it has to be: without it a
 * resumed staging could not continue the incremental digest and would have to
 * re-read every byte already staged, which is the cost the incremental digest
 * exists to avoid.
 *
 * WHAT THE RECORD DOES NOT ATTEST. It says what a previous boot absorbed. It
 * cannot say that the staging medium still holds those bytes -- nothing but
 * re-reading them can. Re-reading and re-digesting before programming is the
 * activation slice's obligation, not this one's.
 *
 * It carries the whole session's IMAGE state except `verifier_fault` (a live
 * diagnostic; `fail_reason` is persisted). The device properties -- the
 * feature latch (a reboot always comes up disarmed), target_id, expected_slot
 * and the link window -- are the composition's to re-state, not the record's.
 *
 * Version 2 (2026-09-03) appended the captured IVT block so a resumed staging
 * keeps the words verify() judges; a version-1 record (384 bytes) is refused
 * as CORRUPT by its version and length, which is the intended fail-closed
 * answer for the one field it could not have carried.
 *
 *    0   4  magic "AIUR"
 *    4   2  record_version = 2
 *    6   2  record_length  = 416
 *    8   1  state
 *    9   1  fail_reason
 *   10   2  reserved0 = 0
 *   12   4  staged_length
 *   16  32  digest state h[0..7], little-endian words
 *   48   8  digest absorbed-byte count
 *   56   1  digest buffered bytes (0..63)
 *   57   3  reserved1 = 0
 *   60  64  digest block buffer (bytes past `buffered` are zero)
 *  124 256  the offered header, verbatim wire bytes
 *  380  32  the payload's IVT as captured so far (bytes not yet staged are
 *           zero, which is what makes the record canonical)
 *  412   4  CRC-32 over bytes [0, 412)
 */
#define AIUS_OTA_RECORD_MAGIC UINT32_C(0x52554941) /* "AIUR" */
#define AIUS_OTA_RECORD_VERSION 2u
#define AIUS_OTA_RECORD_BYTES 416u
#define AIUS_OTA_RECORD_OFFSET_MAGIC 0u
#define AIUS_OTA_RECORD_OFFSET_VERSION 4u
#define AIUS_OTA_RECORD_OFFSET_LENGTH 6u
#define AIUS_OTA_RECORD_OFFSET_STATE 8u
#define AIUS_OTA_RECORD_OFFSET_FAIL_REASON 9u
#define AIUS_OTA_RECORD_OFFSET_RESERVED0 10u
#define AIUS_OTA_RECORD_OFFSET_STAGED_LENGTH 12u
#define AIUS_OTA_RECORD_OFFSET_DIGEST_STATE 16u
#define AIUS_OTA_RECORD_OFFSET_DIGEST_ABSORBED 48u
#define AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFERED 56u
#define AIUS_OTA_RECORD_OFFSET_RESERVED1 57u
#define AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFER 60u
#define AIUS_OTA_RECORD_OFFSET_HEADER 124u
#define AIUS_OTA_RECORD_OFFSET_IVT 380u
#define AIUS_OTA_RECORD_OFFSET_CRC32 412u

/* Serializes the session. Permitted while disarmed: the record is a read of
 * the session, and persisting the IDLE a disarm produced is exactly how a
 * caller clears the slot. NO_SPACE when `length` is not the fixed size. */
AiusOtaStatus aius_ota_record_save(const AiusOtaSession *session,
	                           uint8_t *bytes, size_t length);

/* Restores a session from a record. MUTATING, so it needs the latch armed --
 * which is the intended shape: after a reset the operator re-arms
 * deliberately, and only then does a half-staged upgrade come back.
 *
 * The record's header bytes are re-parsed through aius_ota_header_parse() and
 * re-checked against the session's target_id, so a restore cannot admit a
 * header the offer path would have refused. Every internal consistency rule is
 * checked too -- state in domain, staged_length against image_length and
 * against the state, the digest's absorbed count against staged_length, the
 * buffered count against staged_length % 64, the buffer's tail zeroed -- and
 * any violation is CORRUPT.
 *
 * FAIL-CLOSED: any failure leaves the session IDLE with nothing staged (the
 * latch and target_id survive), so a corrupt record restarts the upgrade
 * rather than resuming a state nobody can vouch for. */
AiusOtaStatus aius_ota_record_load(AiusOtaSession *session,
	                           const uint8_t *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
