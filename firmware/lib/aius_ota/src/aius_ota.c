#include "aius_ota.h"

#include "aius_ota_health.h"

#include <stddef.h>
#include <string.h>

/* Cold code -> FLASH. Nothing in this library runs on the audio block path:
 * the container is parsed once per offer, the digest folded once per staged
 * chunk, the records written a handful of times per upgrade. FLASHMEM is
 * Teensyduino's __attribute__((section(".flashmem"))), defined here rather
 * than pulled from <avr/pgmspace.h> so the file keeps its portable,
 * Arduino-free compile on the host. The attribute must sit on each DEFINITION
 * -- GCC does not merge a `section` attribute from a forward declaration, so a
 * prototype-only FLASHMEM silently leaves the code in ITCM. Enforced post-link
 * by test/itcm-placement/verify_target.py once anything links this library.
 *
 * aius-ota-upgrade.md §The Teensy 4 memory model inverts this rule for the
 * self-flash erase/program path, where a FLASHMEM function becomes
 * unexecutable mid-operation. Nothing here is on that path: staging, digesting
 * and verification all complete before any erase begins. Do not call into this
 * library from inside an erase/program window. */
#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

_Static_assert(AIUS_OTA_HEADER_OFFSET_CRC32 + 4u == AIUS_OTA_HEADER_BYTES,
               "the header CRC is the last field");
_Static_assert(AIUS_OTA_HEADER_OFFSET_RESERVED1 +
	               AIUS_OTA_HEADER_RESERVED1_BYTES ==
	       AIUS_OTA_HEADER_OFFSET_CRC32,
               "the header reserved block runs up to the CRC");
_Static_assert(AIUS_OTA_HEADER_OFFSET_SIGNATURE == AIUS_OTA_HEADER_AUTH_BYTES,
               "the authenticated prefix stops at the signature slot");
_Static_assert(AIUS_OTA_RECORD_OFFSET_CRC32 + 4u == AIUS_OTA_RECORD_BYTES,
               "the staging record CRC is the last field");
_Static_assert(AIUS_OTA_RECORD_OFFSET_HEADER + AIUS_OTA_HEADER_BYTES ==
	               AIUS_OTA_RECORD_OFFSET_CRC32,
               "the staging record carries one whole header");
_Static_assert(AIUS_OTA_HEALTH_OFFSET_CRC32 + 4u ==
	               AIUS_OTA_HEALTH_RECORD_BYTES,
               "the health record CRC is the last field");
_Static_assert(AIUS_OTA_HEALTH_OFFSET_RESERVED1 +
	               AIUS_OTA_HEALTH_RESERVED1_BYTES ==
	       AIUS_OTA_HEALTH_OFFSET_CRC32,
               "the health reserved block runs up to the CRC");

/* --------------------------------------------------------------------------
 * Byte primitives
 *
 * A local CRC-32 and local little-endian accessors, as lib/aius_boot,
 * lib/aius_capability and lib/aius_net each carry their own: a portable engine
 * in this tree stays self-contained rather than acquiring a dependency for
 * twenty lines. Same reflected polynomial as the boot ledger's, so a reader
 * comparing the two records sees one CRC convention.
 * ------------------------------------------------------------------------ */

static FLASHMEM uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
	unsigned int bit;

	crc ^= byte;
	for (bit = 0u; bit < 8u; bit++) {
		uint32_t mask = 0u - (crc & 1u);
		crc = (crc >> 1) ^ (0xedb88320u & mask);
	}
	return crc;
}

static FLASHMEM uint32_t crc32_over(const uint8_t *bytes, size_t length)
{
	uint32_t crc = UINT32_MAX;
	size_t index;

	for (index = 0u; index < length; index++)
		crc = crc32_update(crc, bytes[index]);
	return crc ^ UINT32_MAX;
}

static FLASHMEM uint16_t get_u16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static FLASHMEM uint32_t get_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static FLASHMEM uint64_t get_u64(const uint8_t *bytes)
{
	return (uint64_t)get_u32(bytes) | ((uint64_t)get_u32(&bytes[4]) << 32);
}

static FLASHMEM void put_u16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value & 0xffu);
	bytes[1] = (uint8_t)(value >> 8);
}

static FLASHMEM void put_u32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xffu);
	bytes[1] = (uint8_t)((value >> 8) & 0xffu);
	bytes[2] = (uint8_t)((value >> 16) & 0xffu);
	bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static FLASHMEM void put_u64(uint8_t *bytes, uint64_t value)
{
	put_u32(bytes, (uint32_t)(value & 0xffffffffu));
	put_u32(&bytes[4], (uint32_t)(value >> 32));
}

/* Visits every position, no early return -- the same shape lib/aius_auth's
 * token comparison uses. Returns true when the two blocks differ. */
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

/* A NUL-padded ASCII field: at least one character, printable up to the first
 * NUL, and nothing but NUL after it. `width` is a compile-time constant at
 * every call site. */
static FLASHMEM bool text_field_valid(const uint8_t *field, size_t width)
{
	size_t index = 0u;

	if (field[0] == 0u)
		return false;
	while (index < width && field[index] != 0u) {
		if (field[index] < 0x20u || field[index] > 0x7eu)
			return false;
		index++;
	}
	while (index < width) {
		if (field[index] != 0u)
			return false;
		index++;
	}
	return true;
}

static FLASHMEM void text_field_copy(char *destination, const uint8_t *field,
	                             size_t width)
{
	memcpy(destination, field, width);
	destination[width] = '\0';
}

/* --------------------------------------------------------------------------
 * Image container
 * ------------------------------------------------------------------------ */

FLASHMEM AiusOtaStatus aius_ota_header_parse(AiusOtaImageHeader *header,
	                                     const uint8_t *bytes,
	                                     size_t length)
{
	AiusOtaImageHeader parsed;

	if (header == NULL || bytes == NULL || length != AIUS_OTA_HEADER_BYTES)
		return AIUS_OTA_INVALID_ARG;

	memset(&parsed, 0, sizeof(parsed));

	/* Every read below is bytes[<constant>] for a constant width. The one
	 * length check above is the only bound the parser needs, and no field's
	 * value ever selects an offset or a count. */
	parsed.magic = get_u32(&bytes[AIUS_OTA_HEADER_OFFSET_MAGIC]);
	if (parsed.magic != AIUS_OTA_MAGIC)
		return AIUS_OTA_MALFORMED;

	parsed.header_crc32 = get_u32(&bytes[AIUS_OTA_HEADER_OFFSET_CRC32]);
	if (parsed.header_crc32 !=
	    crc32_over(bytes, AIUS_OTA_HEADER_OFFSET_CRC32))
		return AIUS_OTA_MALFORMED;

	parsed.format_version =
		get_u16(&bytes[AIUS_OTA_HEADER_OFFSET_FORMAT_VERSION]);
	if (parsed.format_version != AIUS_OTA_FORMAT_VERSION)
		return AIUS_OTA_UNSUPPORTED;

	parsed.header_length =
		get_u16(&bytes[AIUS_OTA_HEADER_OFFSET_HEADER_LENGTH]);
	if (parsed.header_length != AIUS_OTA_HEADER_BYTES)
		return AIUS_OTA_MALFORMED;

	parsed.image_length =
		get_u32(&bytes[AIUS_OTA_HEADER_OFFSET_IMAGE_LENGTH]);
	if (parsed.image_length == 0u)
		return AIUS_OTA_MALFORMED;
	if (parsed.image_length > AIUS_OTA_MAX_IMAGE_BYTES)
		return AIUS_OTA_UNSUPPORTED;

	parsed.signature_scheme =
		get_u16(&bytes[AIUS_OTA_HEADER_OFFSET_SIGNATURE_SCHEME]);
	if (parsed.signature_scheme == AIUS_OTA_SIGSCHEME_NONE)
		return AIUS_OTA_MALFORMED;
	if (parsed.signature_scheme != AIUS_OTA_SIGSCHEME_ED25519)
		return AIUS_OTA_UNSUPPORTED;

	if (get_u16(&bytes[AIUS_OTA_HEADER_OFFSET_RESERVED0]) != 0u)
		return AIUS_OTA_MALFORMED;
	if (!block_is_zero(&bytes[AIUS_OTA_HEADER_OFFSET_RESERVED1],
	                   AIUS_OTA_HEADER_RESERVED1_BYTES))
		return AIUS_OTA_MALFORMED;

	if (!text_field_valid(&bytes[AIUS_OTA_HEADER_OFFSET_TARGET_ID],
	                      AIUS_OTA_TARGET_ID_BYTES))
		return AIUS_OTA_MALFORMED;
	if (!text_field_valid(&bytes[AIUS_OTA_HEADER_OFFSET_BUILD_REVISION],
	                      AIUS_OTA_BUILD_REVISION_BYTES))
		return AIUS_OTA_MALFORMED;

	parsed.build_utc = get_u64(&bytes[AIUS_OTA_HEADER_OFFSET_BUILD_UTC]);
	if (parsed.build_utc < AIUS_OTA_MIN_BUILD_UTC ||
	    parsed.build_utc >= AIUS_OTA_MAX_BUILD_UTC)
		return AIUS_OTA_MALFORMED;

	if (block_is_zero(&bytes[AIUS_OTA_HEADER_OFFSET_PAYLOAD_DIGEST],
	                  AIUS_OTA_DIGEST_BYTES))
		return AIUS_OTA_MALFORMED;
	if (block_is_zero(&bytes[AIUS_OTA_HEADER_OFFSET_SIGNATURE],
	                  AIUS_OTA_SIGNATURE_BYTES))
		return AIUS_OTA_MALFORMED;

	text_field_copy(parsed.target_id,
	                &bytes[AIUS_OTA_HEADER_OFFSET_TARGET_ID],
	                AIUS_OTA_TARGET_ID_BYTES);
	text_field_copy(parsed.build_revision,
	                &bytes[AIUS_OTA_HEADER_OFFSET_BUILD_REVISION],
	                AIUS_OTA_BUILD_REVISION_BYTES);
	memcpy(parsed.payload_digest,
	       &bytes[AIUS_OTA_HEADER_OFFSET_PAYLOAD_DIGEST],
	       AIUS_OTA_DIGEST_BYTES);
	memcpy(parsed.signature, &bytes[AIUS_OTA_HEADER_OFFSET_SIGNATURE],
	       AIUS_OTA_SIGNATURE_BYTES);

	*header = parsed;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_header_serialize(
	const AiusOtaImageHeader *header, uint8_t *bytes, size_t length)
{
	const char *target_end;
	const char *revision_end;
	size_t target_length;
	size_t revision_length;

	if (header == NULL || bytes == NULL ||
	    length != AIUS_OTA_HEADER_BYTES)
		return AIUS_OTA_INVALID_ARG;

	/* memchr rather than strlen: an unterminated caller-supplied array is a
	 * bad argument, not a licence to read past it. */
	target_end = (const char *)memchr(header->target_id, '\0',
	                                  sizeof(header->target_id));
	revision_end = (const char *)memchr(header->build_revision, '\0',
	                                    sizeof(header->build_revision));
	if (target_end == NULL || revision_end == NULL)
		return AIUS_OTA_INVALID_ARG;
	target_length = (size_t)(target_end - header->target_id);
	revision_length = (size_t)(revision_end - header->build_revision);
	if (target_length > AIUS_OTA_TARGET_ID_BYTES ||
	    revision_length > AIUS_OTA_BUILD_REVISION_BYTES)
		return AIUS_OTA_INVALID_ARG;

	memset(bytes, 0, AIUS_OTA_HEADER_BYTES);
	put_u32(&bytes[AIUS_OTA_HEADER_OFFSET_MAGIC], header->magic);
	put_u16(&bytes[AIUS_OTA_HEADER_OFFSET_FORMAT_VERSION],
	        header->format_version);
	put_u16(&bytes[AIUS_OTA_HEADER_OFFSET_HEADER_LENGTH],
	        header->header_length);
	put_u32(&bytes[AIUS_OTA_HEADER_OFFSET_IMAGE_LENGTH],
	        header->image_length);
	put_u16(&bytes[AIUS_OTA_HEADER_OFFSET_SIGNATURE_SCHEME],
	        header->signature_scheme);
	put_u64(&bytes[AIUS_OTA_HEADER_OFFSET_BUILD_UTC], header->build_utc);
	memcpy(&bytes[AIUS_OTA_HEADER_OFFSET_TARGET_ID], header->target_id,
	       target_length);
	memcpy(&bytes[AIUS_OTA_HEADER_OFFSET_BUILD_REVISION],
	       header->build_revision, revision_length);
	memcpy(&bytes[AIUS_OTA_HEADER_OFFSET_PAYLOAD_DIGEST],
	       header->payload_digest, AIUS_OTA_DIGEST_BYTES);
	memcpy(&bytes[AIUS_OTA_HEADER_OFFSET_SIGNATURE], header->signature,
	       AIUS_OTA_SIGNATURE_BYTES);
	put_u32(&bytes[AIUS_OTA_HEADER_OFFSET_CRC32],
	        crc32_over(bytes, AIUS_OTA_HEADER_OFFSET_CRC32));
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_header_auth_digest(
	const uint8_t *bytes, size_t length,
	uint8_t digest[AIUS_OTA_DIGEST_BYTES])
{
	if (bytes == NULL || digest == NULL ||
	    length != AIUS_OTA_HEADER_BYTES)
		return AIUS_OTA_INVALID_ARG;
	aius_sha256(bytes, AIUS_OTA_HEADER_AUTH_BYTES, digest);
	return AIUS_OTA_OK;
}

/* --------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------ */

/* Clears everything about the upgrade and keeps everything about the device:
 * the latch and the target identity are properties of the board, not of the
 * image being staged. */
static FLASHMEM void session_clear(AiusOtaSession *session)
{
	session->state = AIUS_OTA_STATE_IDLE;
	session->fail_reason = AIUS_OTA_OK;
	session->verifier_fault = 0;
	session->staged_length = 0u;
	memset(session->header_bytes, 0, sizeof(session->header_bytes));
	memset(&session->header, 0, sizeof(session->header));
	memset(&session->digest, 0, sizeof(session->digest));
}

static FLASHMEM void session_fail(AiusOtaSession *session,
	                          AiusOtaStatus reason)
{
	session->state = AIUS_OTA_STATE_FAILED;
	session->fail_reason = reason;
}

FLASHMEM AiusOtaStatus aius_ota_init(AiusOtaSession *session,
	                             const char *target_id,
	                             size_t target_id_length)
{
	size_t index;

	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;

	memset(session, 0, sizeof(*session));
	session->latch = AIUS_OTA_LATCH_DISARMED;
	session->state = AIUS_OTA_STATE_IDLE;

	if (target_id == NULL || target_id_length == 0u ||
	    target_id_length > AIUS_OTA_TARGET_ID_BYTES)
		return AIUS_OTA_INVALID_ARG;
	for (index = 0u; index < target_id_length; index++) {
		unsigned char character = (unsigned char)target_id[index];
		if (character < 0x20u || character > 0x7eu)
			return AIUS_OTA_INVALID_ARG;
	}
	memcpy(session->target_id, target_id, target_id_length);
	session->target_id[target_id_length] = '\0';
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_latch_arm(AiusOtaSession *session)
{
	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->target_id[0] == '\0')
		return AIUS_OTA_INVALID_ARG;
	session->latch = AIUS_OTA_LATCH_ARMED;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_latch_disarm(AiusOtaSession *session)
{
	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;
	session->latch = AIUS_OTA_LATCH_DISARMED;
	if (session->state != AIUS_OTA_STATE_ACTIVATION_REQUESTED)
		session_clear(session);
	return AIUS_OTA_OK;
}

FLASHMEM bool aius_ota_latch_is_armed(const AiusOtaSession *session)
{
	return session != NULL && session->latch == AIUS_OTA_LATCH_ARMED;
}

FLASHMEM AiusOtaStatus aius_ota_offer(AiusOtaSession *session,
	                              const uint8_t *bytes, size_t length)
{
	AiusOtaImageHeader header;
	AiusOtaStatus status;

	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->latch != AIUS_OTA_LATCH_ARMED)
		return AIUS_OTA_DISARMED;
	if (session->state != AIUS_OTA_STATE_IDLE &&
	    session->state != AIUS_OTA_STATE_FAILED)
		return AIUS_OTA_STATE;

	status = aius_ota_header_parse(&header, bytes, length);
	if (status != AIUS_OTA_OK)
		return status;
	if (strcmp(header.target_id, session->target_id) != 0)
		return AIUS_OTA_UNSUPPORTED;

	session_clear(session);
	memcpy(session->header_bytes, bytes, AIUS_OTA_HEADER_BYTES);
	session->header = header;
	aius_sha256_init(&session->digest);
	session->state = AIUS_OTA_STATE_OFFERED;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_absorb(AiusOtaSession *session,
	                               uint32_t offset, const uint8_t *data,
	                               size_t length)
{
	uint32_t remaining;

	if (session == NULL || data == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->latch != AIUS_OTA_LATCH_ARMED)
		return AIUS_OTA_DISARMED;
	if (session->state != AIUS_OTA_STATE_OFFERED &&
	    session->state != AIUS_OTA_STATE_STAGING)
		return AIUS_OTA_STATE;
	if (length == 0u || length > AIUS_OTA_MAX_CHUNK_BYTES)
		return AIUS_OTA_INVALID_ARG;
	if (offset != session->staged_length)
		return AIUS_OTA_RANGE;

	remaining = session->header.image_length - session->staged_length;
	if (length > (size_t)remaining)
		return AIUS_OTA_OVERFLOW;

	aius_sha256_update(&session->digest, data, length);
	session->staged_length += (uint32_t)length;
	session->state = session->staged_length == session->header.image_length
		? AIUS_OTA_STATE_STAGED
		: AIUS_OTA_STATE_STAGING;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_verify(AiusOtaSession *session,
	                               const AiusOtaVerifier *verifier)
{
	AiusSha256 snapshot;
	uint8_t payload_digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t auth_digest[AIUS_OTA_DIGEST_BYTES];
	int fault;

	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->latch != AIUS_OTA_LATCH_ARMED)
		return AIUS_OTA_DISARMED;
	if (session->state != AIUS_OTA_STATE_STAGED)
		return AIUS_OTA_STATE;
	if (verifier == NULL || verifier->verify == NULL)
		return AIUS_OTA_INVALID_ARG;

	/* Finalize a copy: the session's own hash state stays valid, so a
	 * record saved after a verification still restores a consistent
	 * session. */
	snapshot = session->digest;
	aius_sha256_final(&snapshot, payload_digest);
	if (blocks_differ(payload_digest, session->header.payload_digest,
	                  AIUS_OTA_DIGEST_BYTES)) {
		session_fail(session, AIUS_OTA_DIGEST);
		return AIUS_OTA_DIGEST;
	}

	aius_sha256(session->header_bytes, AIUS_OTA_HEADER_AUTH_BYTES,
	            auth_digest);
	fault = verifier->verify(auth_digest, session->header.signature,
	                         verifier->ctx);
	if (fault != 0) {
		session->verifier_fault = fault;
		session_fail(session, AIUS_OTA_SIGNATURE);
		return AIUS_OTA_SIGNATURE;
	}

	session->state = AIUS_OTA_STATE_VERIFIED;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_arm_image(AiusOtaSession *session)
{
	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->latch != AIUS_OTA_LATCH_ARMED)
		return AIUS_OTA_DISARMED;
	if (session->state != AIUS_OTA_STATE_VERIFIED)
		return AIUS_OTA_STATE;
	session->state = AIUS_OTA_STATE_ARMED;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_request_activation(AiusOtaSession *session)
{
	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->latch != AIUS_OTA_LATCH_ARMED)
		return AIUS_OTA_DISARMED;
	if (session->state != AIUS_OTA_STATE_ARMED)
		return AIUS_OTA_STATE;
	session->state = AIUS_OTA_STATE_ACTIVATION_REQUESTED;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_abort(AiusOtaSession *session)
{
	if (session == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->latch != AIUS_OTA_LATCH_ARMED)
		return AIUS_OTA_DISARMED;
	if (session->state == AIUS_OTA_STATE_ACTIVATION_REQUESTED)
		return AIUS_OTA_STATE;
	session_clear(session);
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaState aius_ota_state(const AiusOtaSession *session)
{
	return session == NULL ? AIUS_OTA_STATE_IDLE : session->state;
}

FLASHMEM uint32_t aius_ota_next_offset(const AiusOtaSession *session)
{
	return session == NULL ? 0u : session->staged_length;
}

FLASHMEM AiusOtaStatus aius_ota_progress(const AiusOtaSession *session,
	                                 AiusOtaProgress *progress)
{
	if (session == NULL || progress == NULL)
		return AIUS_OTA_INVALID_ARG;
	progress->state = session->state;
	progress->latch_armed = session->latch == AIUS_OTA_LATCH_ARMED;
	progress->image_length = session->header.image_length;
	progress->staged_length = session->staged_length;
	progress->next_offset = session->staged_length;
	progress->fail_reason = session->fail_reason;
	progress->verifier_fault = session->verifier_fault;
	return AIUS_OTA_OK;
}

/* --------------------------------------------------------------------------
 * Staging record
 * ------------------------------------------------------------------------ */

FLASHMEM AiusOtaStatus aius_ota_record_save(const AiusOtaSession *session,
	                                    uint8_t *bytes, size_t length)
{
	unsigned int index;

	if (session == NULL || bytes == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (length != AIUS_OTA_RECORD_BYTES)
		return AIUS_OTA_NO_SPACE;

	memset(bytes, 0, AIUS_OTA_RECORD_BYTES);
	put_u32(&bytes[AIUS_OTA_RECORD_OFFSET_MAGIC], AIUS_OTA_RECORD_MAGIC);
	put_u16(&bytes[AIUS_OTA_RECORD_OFFSET_VERSION],
	        AIUS_OTA_RECORD_VERSION);
	put_u16(&bytes[AIUS_OTA_RECORD_OFFSET_LENGTH], AIUS_OTA_RECORD_BYTES);
	bytes[AIUS_OTA_RECORD_OFFSET_STATE] = (uint8_t)session->state;
	bytes[AIUS_OTA_RECORD_OFFSET_FAIL_REASON] =
		(uint8_t)session->fail_reason;
	put_u32(&bytes[AIUS_OTA_RECORD_OFFSET_STAGED_LENGTH],
	        session->staged_length);
	for (index = 0u; index < 8u; index++)
		put_u32(&bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_STATE +
			       index * 4u],
		        session->digest.state[index]);
	put_u64(&bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_ABSORBED],
	        session->digest.absorbed);
	bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFERED] =
		session->digest.buffered;
	/* Only the buffered prefix is copied. aius_sha256_update() leaves
	 * whatever the previous block held past that point, so copying the
	 * whole 64 bytes would make two records of identical hash state differ
	 * -- and the load path checks the tail is zero, which is what makes the
	 * record canonical rather than merely sufficient. */
	if (session->digest.buffered != 0u)
		memcpy(&bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFER],
		       session->digest.buffer, session->digest.buffered);
	memcpy(&bytes[AIUS_OTA_RECORD_OFFSET_HEADER], session->header_bytes,
	       AIUS_OTA_HEADER_BYTES);
	put_u32(&bytes[AIUS_OTA_RECORD_OFFSET_CRC32],
	        crc32_over(bytes, AIUS_OTA_RECORD_OFFSET_CRC32));
	return AIUS_OTA_OK;
}

/* The state-dependent half of the record's consistency rules, kept together so
 * the reachable (state, staged_length) pairs read as one table. */
static FLASHMEM bool record_lengths_agree(AiusOtaState state,
	                                  uint32_t staged_length,
	                                  uint32_t image_length)
{
	switch (state) {
	case AIUS_OTA_STATE_IDLE:
	case AIUS_OTA_STATE_OFFERED:
		return staged_length == 0u;
	case AIUS_OTA_STATE_STAGING:
		return staged_length > 0u && staged_length < image_length;
	case AIUS_OTA_STATE_STAGED:
	case AIUS_OTA_STATE_VERIFIED:
	case AIUS_OTA_STATE_ARMED:
	case AIUS_OTA_STATE_ACTIVATION_REQUESTED:
	case AIUS_OTA_STATE_FAILED:
		return staged_length == image_length;
	}
	return false;
}

FLASHMEM AiusOtaStatus aius_ota_record_load(AiusOtaSession *session,
	                                    const uint8_t *bytes,
	                                    size_t length)
{
	AiusOtaImageHeader header;
	AiusOtaState state;
	AiusOtaStatus fail_reason;
	AiusOtaStatus status;
	uint32_t staged_length;
	uint8_t buffered;
	uint64_t absorbed;
	unsigned int index;

	if (session == NULL || bytes == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (session->latch != AIUS_OTA_LATCH_ARMED)
		return AIUS_OTA_DISARMED;
	if (length != AIUS_OTA_RECORD_BYTES)
		return AIUS_OTA_NO_SPACE;

	/* Fail closed from here: any refusal below leaves the session IDLE
	 * rather than half-restored. */
	session_clear(session);

	if (get_u32(&bytes[AIUS_OTA_RECORD_OFFSET_MAGIC]) !=
	    AIUS_OTA_RECORD_MAGIC)
		return AIUS_OTA_CORRUPT;
	if (get_u16(&bytes[AIUS_OTA_RECORD_OFFSET_VERSION]) !=
	    AIUS_OTA_RECORD_VERSION)
		return AIUS_OTA_CORRUPT;
	if (get_u16(&bytes[AIUS_OTA_RECORD_OFFSET_LENGTH]) !=
	    AIUS_OTA_RECORD_BYTES)
		return AIUS_OTA_CORRUPT;
	if (get_u32(&bytes[AIUS_OTA_RECORD_OFFSET_CRC32]) !=
	    crc32_over(bytes, AIUS_OTA_RECORD_OFFSET_CRC32))
		return AIUS_OTA_CORRUPT;
	if (get_u16(&bytes[AIUS_OTA_RECORD_OFFSET_RESERVED0]) != 0u)
		return AIUS_OTA_CORRUPT;
	if (!block_is_zero(&bytes[AIUS_OTA_RECORD_OFFSET_RESERVED1],
	                   AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFER -
		                   AIUS_OTA_RECORD_OFFSET_RESERVED1))
		return AIUS_OTA_CORRUPT;

	if (bytes[AIUS_OTA_RECORD_OFFSET_STATE] > AIUS_OTA_STATE_FAILED)
		return AIUS_OTA_CORRUPT;
	state = (AiusOtaState)bytes[AIUS_OTA_RECORD_OFFSET_STATE];
	fail_reason =
		(AiusOtaStatus)bytes[AIUS_OTA_RECORD_OFFSET_FAIL_REASON];
	if (state == AIUS_OTA_STATE_FAILED) {
		if (fail_reason != AIUS_OTA_DIGEST &&
		    fail_reason != AIUS_OTA_SIGNATURE)
			return AIUS_OTA_CORRUPT;
	} else if (fail_reason != AIUS_OTA_OK) {
		return AIUS_OTA_CORRUPT;
	}

	staged_length = get_u32(&bytes[AIUS_OTA_RECORD_OFFSET_STAGED_LENGTH]);
	buffered = bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFERED];
	absorbed = get_u64(&bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_ABSORBED]);

	if (state == AIUS_OTA_STATE_IDLE) {
		/* An idle record carries no image at all, and says so by being
		 * zero everywhere an image would be. */
		if (staged_length != 0u || absorbed != 0u || buffered != 0u)
			return AIUS_OTA_CORRUPT;
		if (!block_is_zero(&bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_STATE],
		                   AIUS_OTA_RECORD_OFFSET_HEADER -
			                   AIUS_OTA_RECORD_OFFSET_DIGEST_STATE))
			return AIUS_OTA_CORRUPT;
		if (!block_is_zero(&bytes[AIUS_OTA_RECORD_OFFSET_HEADER],
		                   AIUS_OTA_HEADER_BYTES))
			return AIUS_OTA_CORRUPT;
		return AIUS_OTA_OK;
	}

	status = aius_ota_header_parse(&header,
	                               &bytes[AIUS_OTA_RECORD_OFFSET_HEADER],
	                               AIUS_OTA_HEADER_BYTES);
	if (status != AIUS_OTA_OK)
		return AIUS_OTA_CORRUPT;
	if (strcmp(header.target_id, session->target_id) != 0)
		return AIUS_OTA_CORRUPT;

	if (!record_lengths_agree(state, staged_length, header.image_length))
		return AIUS_OTA_CORRUPT;
	if (absorbed != (uint64_t)staged_length)
		return AIUS_OTA_CORRUPT;
	if (buffered != (uint8_t)(staged_length % AIUS_SHA256_BLOCK_BYTES))
		return AIUS_OTA_CORRUPT;
	if (!block_is_zero(&bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFER +
			          buffered],
	                   AIUS_SHA256_BLOCK_BYTES - buffered))
		return AIUS_OTA_CORRUPT;

	memcpy(session->header_bytes, &bytes[AIUS_OTA_RECORD_OFFSET_HEADER],
	       AIUS_OTA_HEADER_BYTES);
	session->header = header;
	for (index = 0u; index < 8u; index++)
		session->digest.state[index] =
			get_u32(&bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_STATE +
				       index * 4u]);
	session->digest.absorbed = absorbed;
	session->digest.buffered = buffered;
	memcpy(session->digest.buffer,
	       &bytes[AIUS_OTA_RECORD_OFFSET_DIGEST_BUFFER],
	       AIUS_SHA256_BLOCK_BYTES);
	session->staged_length = staged_length;
	session->state = state;
	session->fail_reason = fail_reason;
	return AIUS_OTA_OK;
}

/* --------------------------------------------------------------------------
 * Image health
 * ------------------------------------------------------------------------ */

static FLASHMEM void health_adopt(AiusOtaHealth *health,
	                          const uint8_t *running_digest)
{
	health->state = AIUS_OTA_HEALTH_UNKNOWN;
	health->trial_boots_remaining = 0u;
	health->trial_boots_budget = 0u;
	memcpy(health->image_digest, running_digest, AIUS_OTA_DIGEST_BYTES);
	memset(health->previous_digest, 0, AIUS_OTA_DIGEST_BYTES);
}

static FLASHMEM void health_apply_rollback(AiusOtaHealth *health)
{
	memcpy(health->image_digest, health->previous_digest,
	       AIUS_OTA_DIGEST_BYTES);
	memset(health->previous_digest, 0, AIUS_OTA_DIGEST_BYTES);
	health->state = AIUS_OTA_HEALTH_ROLLED_BACK;
	health->trial_boots_remaining = 0u;
	health->trial_boots_budget = 0u;
	if (health->rollback_count != UINT32_MAX)
		health->rollback_count++;
}

FLASHMEM void aius_ota_health_init(AiusOtaHealth *health)
{
	if (health == NULL)
		return;
	memset(health, 0, sizeof(*health));
	health->state = AIUS_OTA_HEALTH_UNKNOWN;
}

FLASHMEM AiusOtaStatus aius_ota_health_note_activation(
	AiusOtaHealth *health,
	const uint8_t image_digest[AIUS_OTA_DIGEST_BYTES], uint8_t trial_boots)
{
	if (health == NULL || image_digest == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (block_is_zero(image_digest, AIUS_OTA_DIGEST_BYTES))
		return AIUS_OTA_INVALID_ARG;
	if (trial_boots == 0u || trial_boots > AIUS_OTA_HEALTH_MAX_TRIAL_BOOTS)
		return AIUS_OTA_INVALID_ARG;

	if (health->state == AIUS_OTA_HEALTH_TRIAL &&
	    !blocks_differ(image_digest, health->image_digest,
	                   AIUS_OTA_DIGEST_BYTES))
		return AIUS_OTA_OK;

	if (health->state != AIUS_OTA_HEALTH_TRIAL) {
		/* The fallback becomes whatever this record was describing. A
		 * superseding trial keeps the fallback it already had, so an
		 * image that never committed can never become the rollback
		 * target. */
		memcpy(health->previous_digest, health->image_digest,
		       AIUS_OTA_DIGEST_BYTES);
	}
	memcpy(health->image_digest, image_digest, AIUS_OTA_DIGEST_BYTES);
	health->state = AIUS_OTA_HEALTH_TRIAL;
	health->trial_boots_budget = trial_boots;
	health->trial_boots_remaining = trial_boots;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_health_note_boot(
	AiusOtaHealth *health,
	const uint8_t running_digest[AIUS_OTA_DIGEST_BYTES],
	AiusOtaHealthAction *action)
{
	AiusOtaHealthAction outcome = AIUS_OTA_HEALTH_ACTION_NONE;

	if (health == NULL || running_digest == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (block_is_zero(running_digest, AIUS_OTA_DIGEST_BYTES))
		return AIUS_OTA_INVALID_ARG;

	if (health->state == AIUS_OTA_HEALTH_TRIAL) {
		if (!blocks_differ(running_digest, health->image_digest,
		                   AIUS_OTA_DIGEST_BYTES)) {
			if (health->trial_boots_remaining != 0u)
				health->trial_boots_remaining--;
			outcome = health->trial_boots_remaining == 0u
				? AIUS_OTA_HEALTH_ACTION_ROLL_BACK
				: AIUS_OTA_HEALTH_ACTION_TRIAL_CONTINUES;
		} else if (!block_is_zero(health->previous_digest,
		                          AIUS_OTA_DIGEST_BYTES) &&
		           !blocks_differ(running_digest,
		                          health->previous_digest,
		                          AIUS_OTA_DIGEST_BYTES)) {
			health_apply_rollback(health);
			outcome = AIUS_OTA_HEALTH_ACTION_ROLLED_BACK;
		} else {
			health_adopt(health, running_digest);
			outcome = AIUS_OTA_HEALTH_ACTION_ADOPTED;
		}
	} else if (blocks_differ(running_digest, health->image_digest,
	                         AIUS_OTA_DIGEST_BYTES)) {
		health_adopt(health, running_digest);
		outcome = AIUS_OTA_HEALTH_ACTION_ADOPTED;
	}

	if (action != NULL)
		*action = outcome;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_health_mark_healthy(
	AiusOtaHealth *health,
	const uint8_t running_digest[AIUS_OTA_DIGEST_BYTES])
{
	if (health == NULL || running_digest == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (blocks_differ(running_digest, health->image_digest,
	                  AIUS_OTA_DIGEST_BYTES))
		return AIUS_OTA_STATE;
	if (health->state == AIUS_OTA_HEALTH_COMMITTED)
		return AIUS_OTA_OK;
	if (health->state != AIUS_OTA_HEALTH_TRIAL)
		return AIUS_OTA_STATE;
	health->state = AIUS_OTA_HEALTH_COMMITTED;
	health->trial_boots_remaining = 0u;
	health->trial_boots_budget = 0u;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_health_note_rollback(AiusOtaHealth *health)
{
	if (health == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (health->state != AIUS_OTA_HEALTH_TRIAL)
		return AIUS_OTA_STATE;
	if (block_is_zero(health->previous_digest, AIUS_OTA_DIGEST_BYTES))
		return AIUS_OTA_STATE;
	health_apply_rollback(health);
	return AIUS_OTA_OK;
}

FLASHMEM bool aius_ota_health_is_trialing(const AiusOtaHealth *health)
{
	return health != NULL && health->state == AIUS_OTA_HEALTH_TRIAL;
}

FLASHMEM AiusOtaStatus aius_ota_health_save(const AiusOtaHealth *health,
	                                    uint8_t *bytes, size_t length)
{
	if (health == NULL || bytes == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (length != AIUS_OTA_HEALTH_RECORD_BYTES)
		return AIUS_OTA_NO_SPACE;

	memset(bytes, 0, AIUS_OTA_HEALTH_RECORD_BYTES);
	put_u32(&bytes[AIUS_OTA_HEALTH_OFFSET_MAGIC], AIUS_OTA_HEALTH_MAGIC);
	put_u16(&bytes[AIUS_OTA_HEALTH_OFFSET_VERSION],
	        AIUS_OTA_HEALTH_VERSION);
	put_u16(&bytes[AIUS_OTA_HEALTH_OFFSET_LENGTH],
	        AIUS_OTA_HEALTH_RECORD_BYTES);
	bytes[AIUS_OTA_HEALTH_OFFSET_STATE] = (uint8_t)health->state;
	bytes[AIUS_OTA_HEALTH_OFFSET_TRIAL_REMAINING] =
		health->trial_boots_remaining;
	bytes[AIUS_OTA_HEALTH_OFFSET_TRIAL_BUDGET] = health->trial_boots_budget;
	put_u32(&bytes[AIUS_OTA_HEALTH_OFFSET_ROLLBACK_COUNT],
	        health->rollback_count);
	memcpy(&bytes[AIUS_OTA_HEALTH_OFFSET_IMAGE_DIGEST],
	       health->image_digest, AIUS_OTA_DIGEST_BYTES);
	memcpy(&bytes[AIUS_OTA_HEALTH_OFFSET_PREVIOUS_DIGEST],
	       health->previous_digest, AIUS_OTA_DIGEST_BYTES);
	put_u32(&bytes[AIUS_OTA_HEALTH_OFFSET_CRC32],
	        crc32_over(bytes, AIUS_OTA_HEALTH_OFFSET_CRC32));
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_health_load(AiusOtaHealth *health,
	                                    const uint8_t *bytes,
	                                    size_t length)
{
	AiusOtaHealthState state;
	uint8_t remaining;
	uint8_t budget;

	if (health == NULL || bytes == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (length != AIUS_OTA_HEALTH_RECORD_BYTES)
		return AIUS_OTA_NO_SPACE;

	aius_ota_health_init(health);

	if (get_u32(&bytes[AIUS_OTA_HEALTH_OFFSET_MAGIC]) !=
	    AIUS_OTA_HEALTH_MAGIC)
		return AIUS_OTA_CORRUPT;
	if (get_u16(&bytes[AIUS_OTA_HEALTH_OFFSET_VERSION]) !=
	    AIUS_OTA_HEALTH_VERSION)
		return AIUS_OTA_CORRUPT;
	if (get_u16(&bytes[AIUS_OTA_HEALTH_OFFSET_LENGTH]) !=
	    AIUS_OTA_HEALTH_RECORD_BYTES)
		return AIUS_OTA_CORRUPT;
	if (get_u32(&bytes[AIUS_OTA_HEALTH_OFFSET_CRC32]) !=
	    crc32_over(bytes, AIUS_OTA_HEALTH_OFFSET_CRC32))
		return AIUS_OTA_CORRUPT;
	if (bytes[AIUS_OTA_HEALTH_OFFSET_RESERVED0] != 0u)
		return AIUS_OTA_CORRUPT;
	if (!block_is_zero(&bytes[AIUS_OTA_HEALTH_OFFSET_RESERVED1],
	                   AIUS_OTA_HEALTH_RESERVED1_BYTES))
		return AIUS_OTA_CORRUPT;
	if (bytes[AIUS_OTA_HEALTH_OFFSET_STATE] > AIUS_OTA_HEALTH_ROLLED_BACK)
		return AIUS_OTA_CORRUPT;

	state = (AiusOtaHealthState)bytes[AIUS_OTA_HEALTH_OFFSET_STATE];
	remaining = bytes[AIUS_OTA_HEALTH_OFFSET_TRIAL_REMAINING];
	budget = bytes[AIUS_OTA_HEALTH_OFFSET_TRIAL_BUDGET];
	if (budget > AIUS_OTA_HEALTH_MAX_TRIAL_BOOTS || remaining > budget)
		return AIUS_OTA_CORRUPT;
	if (state == AIUS_OTA_HEALTH_TRIAL) {
		/* A trial with no image, or a fallback that is also the trial
		 * image, is not a state this machine can reach. */
		if (budget == 0u ||
		    block_is_zero(&bytes[AIUS_OTA_HEALTH_OFFSET_IMAGE_DIGEST],
		                  AIUS_OTA_DIGEST_BYTES))
			return AIUS_OTA_CORRUPT;
	} else if (budget != 0u || remaining != 0u) {
		return AIUS_OTA_CORRUPT;
	}

	health->state = state;
	health->trial_boots_remaining = remaining;
	health->trial_boots_budget = budget;
	health->rollback_count =
		get_u32(&bytes[AIUS_OTA_HEALTH_OFFSET_ROLLBACK_COUNT]);
	memcpy(health->image_digest,
	       &bytes[AIUS_OTA_HEALTH_OFFSET_IMAGE_DIGEST],
	       AIUS_OTA_DIGEST_BYTES);
	memcpy(health->previous_digest,
	       &bytes[AIUS_OTA_HEALTH_OFFSET_PREVIOUS_DIGEST],
	       AIUS_OTA_DIGEST_BYTES);
	return AIUS_OTA_OK;
}
