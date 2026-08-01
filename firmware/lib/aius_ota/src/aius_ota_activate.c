#include "aius_ota_activate.h"

#include <stddef.h>
#include <string.h>

/* Cold code -> FLASH, the same rule and the same self-defined attribute the
 * rest of lib/aius_ota uses (see aius_ota.c for why the attribute sits on each
 * DEFINITION and not on a prototype).
 *
 * AND THE STANDING WARNING, because this file is the one where the rule and the
 * subject matter point in opposite directions: THIS LIBRARY NEVER RUNS DURING
 * THE ERASE WINDOW IT ORCHESTRATES. On Teensy 4 ordinary code is copied to ITCM
 * at boot and FLASHMEM is the opt-in to stay in flash, so every function here
 * becomes unexecutable the moment a sector erase starts
 * (aius-ota-upgrade.md §The Teensy 4 memory model, and RM p.279 note 3: no
 * FLASH access from ANY master during erase/program). That is correct and
 * deliberate: the sequencer DECIDES, it does not PROGRAM. The run-to-completion
 * caller and the primitive implementations it calls are the composition slice's
 * responsibility and must be RAM-resident with their whole call graph --
 * memcpy included -- verified non-FLASHMEM post-link. The two existing ITCM
 * guards check the opposite direction and will not catch a violation. */
#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

/* The geometry, pinned rather than described. Every one of these is an
 * arithmetic claim the sequencer's ordering, chunking and bounds depend on, and
 * a comment asserting them would not fail a build. */
_Static_assert(AIUS_OTA_FLASH_FCB_OFFSET == 0u,
               "the FCB sits at the base of the image");
_Static_assert(AIUS_OTA_FLASH_IVT_OFFSET == AIUS_OTA_FLASH_SECTOR_BYTES,
               "the IVT is the FIRST BYTE OF SECTOR 1, not part of sector 0");
_Static_assert(AIUS_OTA_FLASH_HEAD_BYTES == AIUS_OTA_FLASH_SECTOR_BYTES,
               "the head region programmed last is exactly one sector");
_Static_assert(AIUS_OTA_FLASH_SECTOR_BYTES % AIUS_OTA_ACTIVATE_CHUNK_BYTES ==
	               0u,
               "the chunk divides the sector, so no program crosses one");
_Static_assert(AIUS_OTA_FLASH_USABLE_BYTES % AIUS_OTA_FLASH_SECTOR_BYTES == 0u,
               "the usable region is a whole number of sectors");
_Static_assert(AIUS_OTA_ACTIVATE_MIN_IMAGE_BYTES > AIUS_OTA_FLASH_HEAD_BYTES,
               "every admissible image has a non-empty body past the head");
_Static_assert(UINT32_MAX - AIUS_OTA_FLASH_BASE >= AIUS_OTA_FLASH_USABLE_BYTES,
               "the flash window does not wrap the address space");
/* This is what makes the over-length arm of aius_ota_activate_check_image_length()
 * unreachable from a validated container rather than merely unlikely: header
 * rule 7 already caps image_length at AIUS_OTA_MAX_IMAGE_BYTES. The check stays
 * because this layer must not depend on the container having run first, and the
 * assert is what stops the two bounds drifting apart. */
_Static_assert(AIUS_OTA_MAX_IMAGE_BYTES <= AIUS_OTA_FLASH_USABLE_BYTES,
               "the container cannot admit an image the flash cannot hold");

/* --------------------------------------------------------------------------
 * Byte primitives
 *
 * Local, as every portable engine in this tree keeps its own twenty lines
 * rather than acquiring a dependency. `blocks_differ` visits every position
 * with no early return, the shape lib/aius_auth's token comparison and
 * aius_ota.c's digest comparison both use.
 * ------------------------------------------------------------------------ */

static FLASHMEM bool blocks_differ(const uint8_t *left, const uint8_t *right,
	                           uint32_t length)
{
	uint8_t accumulator = 0u;
	uint32_t index;

	for (index = 0u; index < length; index++)
		accumulator |= (uint8_t)(left[index] ^ right[index]);
	return accumulator != 0u;
}

static FLASHMEM bool block_is_zero(const uint8_t *bytes, uint32_t length)
{
	uint8_t accumulator = 0u;
	uint32_t index;

	for (index = 0u; index < length; index++)
		accumulator |= bytes[index];
	return accumulator == 0u;
}

static FLASHMEM uint32_t chunks_of(uint32_t bytes)
{
	return (bytes + AIUS_OTA_ACTIVATE_CHUNK_BYTES - 1u) /
	       AIUS_OTA_ACTIVATE_CHUNK_BYTES;
}

/* --------------------------------------------------------------------------
 * Geometry
 * ------------------------------------------------------------------------ */

FLASHMEM AiusOtaStatus aius_ota_activate_check_image_length(
	uint32_t image_length)
{
	if (image_length < AIUS_OTA_ACTIVATE_MIN_IMAGE_BYTES)
		return AIUS_OTA_GEOMETRY;
	if (image_length > AIUS_OTA_FLASH_USABLE_BYTES)
		return AIUS_OTA_GEOMETRY;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_activate_check_erase(uint32_t address)
{
	uint32_t offset;

	if (address < AIUS_OTA_FLASH_BASE)
		return AIUS_OTA_GEOMETRY;
	offset = address - AIUS_OTA_FLASH_BASE;
	if (offset % AIUS_OTA_FLASH_SECTOR_BYTES != 0u)
		return AIUS_OTA_GEOMETRY;
	if (offset >= AIUS_OTA_FLASH_USABLE_BYTES)
		return AIUS_OTA_GEOMETRY;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_activate_check_span(uint32_t address,
	                                            uint32_t length)
{
	uint32_t offset;

	if (length == 0u || length > AIUS_OTA_ACTIVATE_CHUNK_BYTES)
		return AIUS_OTA_INVALID_ARG;
	if (address < AIUS_OTA_FLASH_BASE)
		return AIUS_OTA_GEOMETRY;
	offset = address - AIUS_OTA_FLASH_BASE;
	if (offset >= AIUS_OTA_FLASH_USABLE_BYTES ||
	    length > AIUS_OTA_FLASH_USABLE_BYTES - offset)
		return AIUS_OTA_GEOMETRY;
	/* One sector, always. The driver behind this would happily straddle two
	 * and the part would wrap the write inside a page. */
	if ((offset % AIUS_OTA_FLASH_SECTOR_BYTES) + length >
	    AIUS_OTA_FLASH_SECTOR_BYTES)
		return AIUS_OTA_GEOMETRY;
	return AIUS_OTA_OK;
}

FLASHMEM uint32_t aius_ota_activate_step_bound(uint32_t image_length)
{
	uint32_t sectors;
	uint32_t steps;

	if (aius_ota_activate_check_image_length(image_length) != AIUS_OTA_OK)
		return 0u;

	sectors = (image_length + AIUS_OTA_FLASH_SECTOR_BYTES - 1u) /
		  AIUS_OTA_FLASH_SECTOR_BYTES;
	steps = 1u;                 /* MARK: one health-record write */
	steps += 1u;                /* ERASE_HEAD: sector 0 */
	steps += sectors - 1u;      /* BODY: one erase per body sector */
	steps += chunks_of(image_length - AIUS_OTA_FLASH_HEAD_BYTES);
	steps += chunks_of(image_length); /* VERIFY: every byte, once */
	steps += AIUS_OTA_FLASH_HEAD_BYTES / AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	steps += AIUS_OTA_FLASH_HEAD_BYTES / AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	return steps;
}

/* --------------------------------------------------------------------------
 * Primitive calls
 *
 * Every one of them goes through a wrapper that (a) feeds the watchdog first
 * and (b) checks the geometry first, so the cadence promise
 * (AIUS_OTA_ACTIVATE_MAX_OPS_PER_FEED) and the bounds promise are properties of
 * the ONE place a primitive can be reached from, not of each call site.
 * ------------------------------------------------------------------------ */

static FLASHMEM AiusOtaStatus halt(AiusOtaActivator *activator,
	                           AiusOtaActivateFault fault,
	                           AiusOtaStatus status)
{
	activator->phase = AIUS_OTA_ACTIVATE_FAULTED;
	activator->fault = fault;
	return status;
}

static FLASHMEM void feed(AiusOtaActivator *activator)
{
	activator->primitives.watchdog_feed(activator->primitives.ctx);
}

static FLASHMEM AiusOtaStatus do_staged_read(AiusOtaActivator *activator,
	                                     uint32_t offset, uint32_t length)
{
	int fault;

	/* An internal invariant, not a caller's argument: the cursor arithmetic
	 * is this file's own. It is checked anyway because the alternative is a
	 * read past the staged image on a future edit. */
	if (length == 0u || length > AIUS_OTA_ACTIVATE_CHUNK_BYTES ||
	    offset > activator->image_length ||
	    length > activator->image_length - offset)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_GEOMETRY,
		            AIUS_OTA_GEOMETRY);

	feed(activator);
	fault = activator->primitives.staged_read(activator->primitives.ctx,
	                                          offset, activator->staged,
	                                          length);
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_STAGED_READ,
		            AIUS_OTA_PRIMITIVE);
	}
	return AIUS_OTA_OK;
}

static FLASHMEM AiusOtaStatus do_erase(AiusOtaActivator *activator,
	                               uint32_t address)
{
	AiusOtaStatus status = aius_ota_activate_check_erase(address);
	int fault;

	if (status != AIUS_OTA_OK)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_GEOMETRY,
		            status);

	feed(activator);
	fault = activator->primitives.erase_sector(activator->primitives.ctx,
	                                           address);
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_ERASE,
		            AIUS_OTA_PRIMITIVE);
	}
	return AIUS_OTA_OK;
}

static FLASHMEM AiusOtaStatus do_program(AiusOtaActivator *activator,
	                                 uint32_t address, uint32_t length)
{
	AiusOtaStatus status = aius_ota_activate_check_span(address, length);
	int fault;

	if (status != AIUS_OTA_OK)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_GEOMETRY,
		            status);

	feed(activator);
	fault = activator->primitives.program(activator->primitives.ctx,
	                                      address, activator->staged,
	                                      length);
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_PROGRAM,
		            AIUS_OTA_PRIMITIVE);
	}
	return AIUS_OTA_OK;
}

static FLASHMEM AiusOtaStatus do_flash_read(AiusOtaActivator *activator,
	                                    uint32_t address, uint32_t length)
{
	AiusOtaStatus status = aius_ota_activate_check_span(address, length);
	int fault;

	if (status != AIUS_OTA_OK)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_GEOMETRY,
		            status);

	feed(activator);
	fault = activator->primitives.flash_read(activator->primitives.ctx,
	                                         address, activator->readback,
	                                         length);
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_FLASH_READ,
		            AIUS_OTA_PRIMITIVE);
	}
	return AIUS_OTA_OK;
}

/* --------------------------------------------------------------------------
 * The phases
 * ------------------------------------------------------------------------ */

static FLASHMEM uint32_t chunk_at(const AiusOtaActivator *activator,
	                          uint32_t cursor)
{
	uint32_t remaining = activator->image_length - cursor;

	return remaining < AIUS_OTA_ACTIVATE_CHUNK_BYTES
		? remaining
		: AIUS_OTA_ACTIVATE_CHUNK_BYTES;
}

/* MARK -- the only step outside the dark window that touches persistent state,
 * and the whole reason power-loss forensics can tell "never started" from
 * "interrupted".
 *
 * There is ONE activation transition in the slice-1 health record --
 * aius_ota_health_note_activation() -- and its own contract already describes
 * this write: idempotent for the image already on trial, deliberately not
 * refreshing the budget, because "a power loss between the activation write and
 * the reboot must not hand a boot-looping image an unlimited supply of fresh
 * trials". So the in-progress mark and the new-image-trial transition are the
 * SAME WRITE, made here, before the first erase, rather than two writes with
 * the second on success. That is a deviation from a literal reading of the
 * sequencing contract ("on success: transition to new-image-trial") and it is
 * the honest one: a second write on success would either be a no-op or would
 * need a record state the slice-1 layout does not have, and inventing one means
 * restructuring a record another image has to read back.
 *
 * HOW A READER TELLS THE THREE CASES APART, which is all the forensics needs:
 *
 *   record does not name the new image      -> never started.
 *   TRIAL(new) and the new image is running -> started, finished, on trial.
 *   TRIAL(new) and it is NOT running        -> started and interrupted.
 *
 * The third case is exactly aius_ota_activate_begin()'s rule 8, and on the next
 * boot aius_ota_health_note_boot() resolves it without help: the fallback
 * booting while the record says TRIAL is already ROLLED_BACK with the count
 * incremented.
 *
 * WHICH IS ALSO WHY A FAULT WRITES NOTHING HERE. A halted activation leaves the
 * mark standing, because the mark is the true statement -- an activation of
 * this image was begun and did not complete. Rewriting it to anything else
 * would be a claim the sequencer cannot support: nothing has rolled back, the
 * new image is not running, and the flash may be half-programmed. */
static FLASHMEM AiusOtaStatus step_mark(AiusOtaActivator *activator)
{
	uint8_t bytes[AIUS_OTA_HEALTH_RECORD_BYTES];
	AiusOtaStatus status;
	int fault;

	status = aius_ota_health_note_activation(
		&activator->health, activator->image_digest,
		AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS);
	if (status != AIUS_OTA_OK)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_HEALTH_RULE,
		            status);
	status = aius_ota_health_save(&activator->health, bytes, sizeof(bytes));
	if (status != AIUS_OTA_OK)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_HEALTH_RULE,
		            status);

	feed(activator);
	fault = activator->primitives.health_write(activator->primitives.ctx,
	                                           bytes, sizeof(bytes));
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_HEALTH_WRITE,
		            AIUS_OTA_PRIMITIVE);
	}

	activator->phase = AIUS_OTA_ACTIVATE_ERASE_HEAD;
	return AIUS_OTA_OK;
}

/* ERASE_HEAD -- sector 0, the FCB, erased FIRST and reprogrammed LAST.
 *
 * The ordering is the whole safety argument, so it is worth stating in full.
 * The IVT is at base + 0x1000, which is the first byte of SECTOR 1, so the body
 * pass reprograms the NEW image's IVT early -- long before the new image is
 * complete. If sector 0 still held the old, valid FCB at that moment, a reset
 * would find a bootable-looking image: valid FCB, valid IVT, half a program
 * behind it. Erasing the FCB first removes that window entirely. From here
 * until PROGRAM_HEAD completes, the ROM cannot configure FlexSPI, so it never
 * reaches the IVT and cannot boot anything, whole or half.
 *
 * aius-ota-upgrade.md accepts exactly this outcome: erasing the FCB at
 * 0x60000000 or the IVT at 0x60001000 makes the board un-ROM-bootable, and
 * HalfKay -- a physically separate chip that occupies none of this flash and
 * cannot be damaged by application code -- survives to reflash it. An
 * interrupted activation is therefore a HalfKay rescue by DESIGN, not by
 * accident. */
static FLASHMEM AiusOtaStatus step_erase_head(AiusOtaActivator *activator)
{
	AiusOtaStatus status = do_erase(activator, AIUS_OTA_FLASH_BASE);

	if (status != AIUS_OTA_OK)
		return status;

	activator->phase = AIUS_OTA_ACTIVATE_BODY;
	activator->cursor = AIUS_OTA_FLASH_HEAD_BYTES;
	activator->erase_pending = true;
	return AIUS_OTA_OK;
}

/* BODY -- sectors 1..N-1, erase then program, one operation per step. */
static FLASHMEM AiusOtaStatus step_body(AiusOtaActivator *activator)
{
	AiusOtaStatus status;
	uint32_t take;

	if (activator->erase_pending) {
		status = do_erase(activator,
		                  AIUS_OTA_FLASH_BASE + activator->cursor);
		if (status != AIUS_OTA_OK)
			return status;
		activator->erase_pending = false;
		return AIUS_OTA_OK;
	}

	take = chunk_at(activator, activator->cursor);
	status = do_staged_read(activator, activator->cursor, take);
	if (status != AIUS_OTA_OK)
		return status;
	status = do_program(activator, AIUS_OTA_FLASH_BASE + activator->cursor,
	                    take);
	if (status != AIUS_OTA_OK)
		return status;

	activator->cursor += take;
	activator->programmed_bytes += take;
	if (activator->cursor == activator->image_length) {
		activator->phase = AIUS_OTA_ACTIVATE_VERIFY;
		activator->cursor = 0u;
		aius_sha256_init(&activator->image_hash);
		aius_sha256_init(&activator->head_hash);
	} else if (activator->cursor % AIUS_OTA_FLASH_SECTOR_BYTES == 0u) {
		activator->erase_pending = true;
	}
	return AIUS_OTA_OK;
}

/* VERIFY -- one pass over the whole image, doing two independent jobs, and it
 * runs BEFORE the head is programmed so that a failure here still leaves the
 * board in the erased-FCB state rather than a bootable wrong image.
 *
 *   1. Readback compare, for every body byte: what the flash now holds against
 *      what the staged image says it should. This is what catches a program
 *      that reported success and did not take.
 *   2. Digest recompute, over every byte INCLUDING the head, compared against
 *      the armed header's payload_digest. This is what the staging record
 *      explicitly does not attest -- "it cannot say that the staging medium
 *      still holds those bytes; re-reading and re-digesting before programming
 *      is the activation slice's obligation". This is that obligation.
 *
 * Neither subsumes the other. The compare catches flash that disagrees with the
 * medium; the digest catches a medium that disagrees with what was verified and
 * signed. A card that decayed between staging and activation passes the compare
 * (both sides read the same wrong bytes) and fails the digest.
 *
 * The head's bytes cannot be compared here -- sector 0 is erased -- so they are
 * hashed into a second context instead, and that 32-byte digest is the
 * reference VERIFY_HEAD checks the programmed head against. */
static FLASHMEM AiusOtaStatus step_verify(AiusOtaActivator *activator)
{
	uint8_t digest[AIUS_OTA_DIGEST_BYTES];
	AiusOtaStatus status;
	uint32_t take;

	take = chunk_at(activator, activator->cursor);
	status = do_staged_read(activator, activator->cursor, take);
	if (status != AIUS_OTA_OK)
		return status;
	aius_sha256_update(&activator->image_hash, activator->staged, take);

	if (activator->cursor < AIUS_OTA_FLASH_HEAD_BYTES) {
		aius_sha256_update(&activator->head_hash, activator->staged,
		                   take);
	} else {
		status = do_flash_read(activator,
		                       AIUS_OTA_FLASH_BASE + activator->cursor,
		                       take);
		if (status != AIUS_OTA_OK)
			return status;
		if (blocks_differ(activator->staged, activator->readback, take))
			return halt(activator, AIUS_OTA_ACTIVATE_FAULT_VERIFY,
			            AIUS_OTA_VERIFY);
		activator->verified_bytes += take;
	}

	activator->cursor += take;
	if (activator->cursor != activator->image_length)
		return AIUS_OTA_OK;

	aius_sha256_final(&activator->image_hash, digest);
	if (blocks_differ(digest, activator->image_digest,
	                  AIUS_OTA_DIGEST_BYTES))
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_DIGEST,
		            AIUS_OTA_DIGEST);
	aius_sha256_final(&activator->head_hash, activator->head_digest);

	activator->phase = AIUS_OTA_ACTIVATE_PROGRAM_HEAD;
	activator->cursor = 0u;
	return AIUS_OTA_OK;
}

/* PROGRAM_HEAD -- sector 0 LAST, from the staged image again (this library
 * holds no image bytes, by the same rule the staging machine follows). A third
 * read of the medium is not a risk taken blindly: VERIFY_HEAD hashes the
 * programmed result and checks it against the digest VERIFY captured, so a
 * medium that changed between the two reads cannot slip a different FCB in. */
static FLASHMEM AiusOtaStatus step_program_head(AiusOtaActivator *activator)
{
	AiusOtaStatus status;

	status = do_staged_read(activator, activator->cursor,
	                        AIUS_OTA_ACTIVATE_CHUNK_BYTES);
	if (status != AIUS_OTA_OK)
		return status;
	status = do_program(activator, AIUS_OTA_FLASH_BASE + activator->cursor,
	                    AIUS_OTA_ACTIVATE_CHUNK_BYTES);
	if (status != AIUS_OTA_OK)
		return status;

	activator->cursor += AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	activator->programmed_bytes += AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	if (activator->cursor == AIUS_OTA_FLASH_HEAD_BYTES) {
		activator->phase = AIUS_OTA_ACTIVATE_VERIFY_HEAD;
		activator->cursor = 0u;
		aius_sha256_init(&activator->head_hash);
	}
	return AIUS_OTA_OK;
}

/* VERIFY_HEAD -- the last thing that happens before the caller may reset: read
 * the FCB back out of flash and prove it is the one VERIFY digested. */
static FLASHMEM AiusOtaStatus step_verify_head(AiusOtaActivator *activator)
{
	uint8_t digest[AIUS_OTA_DIGEST_BYTES];
	AiusOtaStatus status;

	status = do_flash_read(activator,
	                       AIUS_OTA_FLASH_BASE + activator->cursor,
	                       AIUS_OTA_ACTIVATE_CHUNK_BYTES);
	if (status != AIUS_OTA_OK)
		return status;
	aius_sha256_update(&activator->head_hash, activator->readback,
	                   AIUS_OTA_ACTIVATE_CHUNK_BYTES);

	activator->cursor += AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	activator->verified_bytes += AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	if (activator->cursor != AIUS_OTA_FLASH_HEAD_BYTES)
		return AIUS_OTA_OK;

	aius_sha256_final(&activator->head_hash, digest);
	if (blocks_differ(digest, activator->head_digest,
	                  AIUS_OTA_DIGEST_BYTES))
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_HEAD_VERIFY,
		            AIUS_OTA_VERIFY);

	activator->phase = AIUS_OTA_ACTIVATE_READY_TO_RESET;
	return AIUS_OTA_OK;
}

/* --------------------------------------------------------------------------
 * Interface
 * ------------------------------------------------------------------------ */

FLASHMEM AiusOtaStatus aius_ota_activate_begin(
	AiusOtaActivator *activator, AiusOtaSession *session,
	const AiusOtaActivatePrimitives *primitives,
	const uint8_t running_digest[AIUS_OTA_DIGEST_BYTES])
{
	uint8_t bytes[AIUS_OTA_HEALTH_RECORD_BYTES];
	AiusOtaProgress progress;
	AiusOtaStatus status;
	int fault;

	if (activator == NULL)
		return AIUS_OTA_INVALID_ARG;

	/* Zeroed first, so a refused begin() leaves an activator that refuses
	 * every step rather than one holding half a setup. */
	memset(activator, 0, sizeof(*activator));
	aius_ota_health_init(&activator->health);

	if (session == NULL || primitives == NULL || running_digest == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (primitives->staged_read == NULL || primitives->erase_sector == NULL ||
	    primitives->program == NULL || primitives->flash_read == NULL ||
	    primitives->watchdog_feed == NULL || primitives->cycles == NULL ||
	    primitives->health_read == NULL || primitives->health_write == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (!aius_ota_latch_is_armed(session))
		return AIUS_OTA_DISARMED;
	if (aius_ota_state(session) != AIUS_OTA_STATE_ACTIVATION_REQUESTED)
		return AIUS_OTA_STATE;

	status = aius_ota_progress(session, &progress);
	if (status != AIUS_OTA_OK)
		return status;
	if (progress.staged_length != progress.image_length)
		return AIUS_OTA_STATE;

	/* From here the refusals are verdicts about this image on this board, so
	 * they latch a typed fault rather than just returning. */
	activator->session = session;
	activator->primitives = *primitives;
	activator->image_length = progress.image_length;
	activator->steps_total =
		aius_ota_activate_step_bound(progress.image_length);
	memcpy(activator->image_digest, session->header.payload_digest,
	       AIUS_OTA_DIGEST_BYTES);

	status = aius_ota_activate_check_image_length(progress.image_length);
	if (status != AIUS_OTA_OK)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_GEOMETRY,
		            status);
	if (block_is_zero(activator->image_digest, AIUS_OTA_DIGEST_BYTES))
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_HEALTH_RULE,
		            AIUS_OTA_MALFORMED);

	feed(activator);
	fault = activator->primitives.health_read(activator->primitives.ctx,
	                                          bytes, sizeof(bytes));
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_HEALTH_READ,
		            AIUS_OTA_PRIMITIVE);
	}
	/* CORRUPT is not a refusal. The loader fails closed to UNKNOWN, which is
	 * precisely the state of a board that has never recorded an image, and a
	 * fresh board must be able to take its first upgrade. What is lost is the
	 * fallback identity, and that is the honest consequence of an unreadable
	 * record rather than something to paper over. */
	(void)aius_ota_health_load(&activator->health, bytes, sizeof(bytes));

	if (aius_ota_health_is_trialing(&activator->health) &&
	    !blocks_differ(activator->health.image_digest,
	                   activator->image_digest, AIUS_OTA_DIGEST_BYTES) &&
	    blocks_differ(running_digest, activator->image_digest,
	                  AIUS_OTA_DIGEST_BYTES))
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_INTERRUPTED,
		            AIUS_OTA_INTERRUPTED);

	activator->phase = AIUS_OTA_ACTIVATE_MARK;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_activate_step(AiusOtaActivator *activator)
{
	AiusOtaStatus status;
	uint32_t started;
	uint32_t elapsed;
	bool dark;

	if (activator == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (activator->phase == AIUS_OTA_ACTIVATE_IDLE ||
	    activator->phase == AIUS_OTA_ACTIVATE_READY_TO_RESET ||
	    activator->phase == AIUS_OTA_ACTIVATE_FAULTED)
		return AIUS_OTA_STATE;

	/* The authorization is re-read, not remembered: a session moved out of
	 * ACTIVATION_REQUESTED mid-sequence has withdrawn it. */
	if (aius_ota_state(activator->session) !=
	    AIUS_OTA_STATE_ACTIVATION_REQUESTED)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_SESSION,
		            AIUS_OTA_STATE);
	/* Unreachable while the step machine and step_bound() agree, which the
	 * suite asserts exactly. It exists because the run-to-completion loop
	 * runs with the device quiesced and a non-advancing step there would
	 * hang the board with no watchdog to save it. */
	if (activator->steps_taken >= activator->steps_total)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_INTERNAL,
		            AIUS_OTA_STATE);

	dark = aius_ota_activate_is_dark(activator);
	started = activator->primitives.cycles(activator->primitives.ctx);

	switch (activator->phase) {
	case AIUS_OTA_ACTIVATE_MARK:
		status = step_mark(activator);
		break;
	case AIUS_OTA_ACTIVATE_ERASE_HEAD:
		status = step_erase_head(activator);
		break;
	case AIUS_OTA_ACTIVATE_BODY:
		status = step_body(activator);
		break;
	case AIUS_OTA_ACTIVATE_VERIFY:
		status = step_verify(activator);
		break;
	case AIUS_OTA_ACTIVATE_PROGRAM_HEAD:
		status = step_program_head(activator);
		break;
	case AIUS_OTA_ACTIVATE_VERIFY_HEAD:
		status = step_verify_head(activator);
		break;
	case AIUS_OTA_ACTIVATE_IDLE:
	case AIUS_OTA_ACTIVATE_READY_TO_RESET:
	case AIUS_OTA_ACTIVATE_FAULTED:
	default:
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_INTERNAL,
		            AIUS_OTA_STATE);
	}

	/* Modular difference of a free-running 32-bit counter: correct across a
	 * wrap, which one sector erase at 600 MHz can reach. Never a comparison
	 * of two raw samples. */
	elapsed = activator->primitives.cycles(activator->primitives.ctx) -
		  started;
	activator->steps_taken++;
	if (elapsed > activator->max_step_cycles)
		activator->max_step_cycles = elapsed;
	if (dark)
		activator->dark_window_cycles += elapsed;
	return status;
}

FLASHMEM AiusOtaStatus aius_ota_activate_run(AiusOtaActivator *activator)
{
	AiusOtaStatus status;

	if (activator == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (activator->phase == AIUS_OTA_ACTIVATE_IDLE)
		return AIUS_OTA_STATE;

	while (activator->phase != AIUS_OTA_ACTIVATE_READY_TO_RESET &&
	       activator->phase != AIUS_OTA_ACTIVATE_FAULTED) {
		status = aius_ota_activate_step(activator);
		if (status != AIUS_OTA_OK)
			return status;
	}
	return activator->phase == AIUS_OTA_ACTIVATE_READY_TO_RESET
		? AIUS_OTA_OK
		: AIUS_OTA_STATE;
}

FLASHMEM bool aius_ota_activate_is_dark(const AiusOtaActivator *activator)
{
	return activator != NULL &&
	       activator->phase >= AIUS_OTA_ACTIVATE_ERASE_HEAD &&
	       activator->phase <= AIUS_OTA_ACTIVATE_VERIFY_HEAD;
}

FLASHMEM AiusOtaActivatePhase aius_ota_activate_phase(
	const AiusOtaActivator *activator)
{
	return activator == NULL ? AIUS_OTA_ACTIVATE_IDLE : activator->phase;
}

FLASHMEM AiusOtaStatus aius_ota_activate_progress(
	const AiusOtaActivator *activator, AiusOtaActivateProgress *progress)
{
	if (activator == NULL || progress == NULL)
		return AIUS_OTA_INVALID_ARG;
	progress->phase = activator->phase;
	progress->fault = activator->fault;
	progress->primitive_fault = activator->primitive_fault;
	progress->dark = aius_ota_activate_is_dark(activator);
	progress->image_length = activator->image_length;
	progress->programmed_bytes = activator->programmed_bytes;
	progress->verified_bytes = activator->verified_bytes;
	progress->steps_taken = activator->steps_taken;
	progress->steps_total = activator->steps_total;
	progress->dark_window_cycles = activator->dark_window_cycles;
	progress->max_step_cycles = activator->max_step_cycles;
	return AIUS_OTA_OK;
}
