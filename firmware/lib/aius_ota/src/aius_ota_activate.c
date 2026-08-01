#include "aius_ota_activate.h"

#include <stddef.h>
#include <string.h>

/* TWO PLACEMENTS, DELIBERATELY, because this file's work spans the dark window
 * that the rest of lib/aius_ota is cold enough to sit out.
 *
 * FLASHMEM (cold -> FLASH) is the library's default and stays it for everything
 * that runs BEFORE quiesce: begin() (which now does the whole SHA-256 +
 * signature verification and the health mark), step_bound(), and the progress
 * queries. Keeping them in flash is what leaves ITCM budget for the step path.
 *
 * FASTRUN (RAM-resident, .fastrun -> ITCM on the target) is the opt-in for the
 * IN-WINDOW step path: aius_ota_activate_step() and everything it reaches. On
 * Teensy 4 ordinary code is copied to ITCM at boot and FLASHMEM is the opt-in to
 * STAY in flash, so a FLASHMEM function becomes unexecutable the moment a sector
 * erase starts (aius-ota-upgrade.md §The Teensy 4 memory model, and RM p.279
 * note 3: no FLASH access from ANY master during erase/program). The step path
 * must keep executing while the boot flash is mid-erase, so it is FASTRUN, and
 * it reaches nothing FLASHMEM -- not even aius_ota_state(): the session's state
 * is read as a plain struct field, because the accessor is cold and would fault.
 * On the host both attributes expand to nothing, so the identical object is what
 * the ota suite proves. */
#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

#ifndef FASTRUN
#if defined(__IMXRT1062__)
#define FASTRUN __attribute__((section(".fastrun")))
#else
#define FASTRUN
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
_Static_assert(AIUS_OTA_FLASH_HEAD_BYTES % AIUS_OTA_ACTIVATE_CHUNK_BYTES == 0u,
               "the head is a whole number of chunks, so PROGRAM_HEAD never "
               "programs a partial chunk");
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
 * `blocks_differ` is FASTRUN because it is the in-window byte-compare: it reads
 * the flash readback and the staged PSRAM source while the boot flash is
 * mid-erase, so it cannot live in flash. It visits every position with no early
 * return, the shape lib/aius_auth's token comparison and aius_ota.c's digest
 * comparison both use, and it doubles as the pre-window digest compare -- a
 * FASTRUN leaf is callable from a FLASHMEM caller, only not the reverse.
 * ------------------------------------------------------------------------ */

static FASTRUN bool blocks_differ(const uint8_t *left, const uint8_t *right,
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
 *
 * check_erase_in / check_span_in are FASTRUN: the step path calls them before
 * every primitive, and the composition's RAM-resident wrapper re-checks
 * through them too, so they must execute while the flash is mid-erase. The
 * NULL-window resolution is a FASTRUN leaf for the same reason. The window
 * and image-length checks are pre-window only (begin and step_bound), so they
 * stay FLASHMEM.
 * ------------------------------------------------------------------------ */

/* NULL spells the boot window. A static const would be a FLASH read from a
 * FASTRUN path on the target, so the constants are materialized by value. */
static FASTRUN AiusOtaActivateWindow resolve_window(
	const AiusOtaActivateWindow *window)
{
	AiusOtaActivateWindow resolved;

	if (window != NULL)
		return *window;
	resolved.base = AIUS_OTA_FLASH_BASE;
	resolved.usable_bytes = AIUS_OTA_FLASH_USABLE_BYTES;
	return resolved;
}

FLASHMEM AiusOtaStatus aius_ota_activate_check_window(
	const AiusOtaActivateWindow *window)
{
	uint32_t offset;

	if (window == NULL)
		return AIUS_OTA_OK;
	if (window->base < AIUS_OTA_FLASH_BASE)
		return AIUS_OTA_GEOMETRY;
	offset = window->base - AIUS_OTA_FLASH_BASE;
	/* The physical sector grid, not merely internal alignment: check_span's
	 * one-sector rule divides from the window base, and only a grid-pinned
	 * base keeps that congruent with the part's real sector boundaries. */
	if (offset % AIUS_OTA_FLASH_SECTOR_BYTES != 0u)
		return AIUS_OTA_GEOMETRY;
	if (window->usable_bytes == 0u ||
	    window->usable_bytes % AIUS_OTA_FLASH_SECTOR_BYTES != 0u)
		return AIUS_OTA_GEOMETRY;
	if (offset >= AIUS_OTA_FLASH_USABLE_BYTES ||
	    window->usable_bytes > AIUS_OTA_FLASH_USABLE_BYTES - offset)
		return AIUS_OTA_GEOMETRY;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_activate_check_image_length_in(
	const AiusOtaActivateWindow *window, uint32_t image_length)
{
	AiusOtaActivateWindow resolved = resolve_window(window);

	if (image_length < AIUS_OTA_ACTIVATE_MIN_IMAGE_BYTES)
		return AIUS_OTA_GEOMETRY;
	if (image_length > resolved.usable_bytes)
		return AIUS_OTA_GEOMETRY;
	return AIUS_OTA_OK;
}

FLASHMEM AiusOtaStatus aius_ota_activate_check_image_length(
	uint32_t image_length)
{
	return aius_ota_activate_check_image_length_in(NULL, image_length);
}

FASTRUN AiusOtaStatus aius_ota_activate_check_erase_in(
	const AiusOtaActivateWindow *window, uint32_t address)
{
	AiusOtaActivateWindow resolved = resolve_window(window);
	uint32_t offset;

	if (address < resolved.base)
		return AIUS_OTA_GEOMETRY;
	offset = address - resolved.base;
	if (offset % AIUS_OTA_FLASH_SECTOR_BYTES != 0u)
		return AIUS_OTA_GEOMETRY;
	if (offset >= resolved.usable_bytes)
		return AIUS_OTA_GEOMETRY;
	return AIUS_OTA_OK;
}

FASTRUN AiusOtaStatus aius_ota_activate_check_erase(uint32_t address)
{
	return aius_ota_activate_check_erase_in(NULL, address);
}

FASTRUN AiusOtaStatus aius_ota_activate_check_span_in(
	const AiusOtaActivateWindow *window, uint32_t address, uint32_t length)
{
	AiusOtaActivateWindow resolved = resolve_window(window);
	uint32_t offset;

	if (length == 0u || length > AIUS_OTA_ACTIVATE_CHUNK_BYTES)
		return AIUS_OTA_INVALID_ARG;
	if (address < resolved.base)
		return AIUS_OTA_GEOMETRY;
	offset = address - resolved.base;
	if (offset >= resolved.usable_bytes ||
	    length > resolved.usable_bytes - offset)
		return AIUS_OTA_GEOMETRY;
	/* One sector, always. The driver behind this would happily straddle two
	 * and the part would wrap the write inside a page. Congruent with the
	 * physical grid because check_window pins the base to it. */
	if ((offset % AIUS_OTA_FLASH_SECTOR_BYTES) + length >
	    AIUS_OTA_FLASH_SECTOR_BYTES)
		return AIUS_OTA_GEOMETRY;
	return AIUS_OTA_OK;
}

FASTRUN AiusOtaStatus aius_ota_activate_check_span(uint32_t address,
	                                           uint32_t length)
{
	return aius_ota_activate_check_span_in(NULL, address, length);
}

FLASHMEM uint32_t aius_ota_activate_step_bound(uint32_t image_length)
{
	uint32_t sectors;
	uint32_t steps;

	if (aius_ota_activate_check_image_length(image_length) != AIUS_OTA_OK)
		return 0u;

	sectors = (image_length + AIUS_OTA_FLASH_SECTOR_BYTES - 1u) /
		  AIUS_OTA_FLASH_SECTOR_BYTES;
	steps = 1u;                 /* ERASE_HEAD: sector 0 */
	steps += sectors - 1u;      /* BODY: one erase per body sector */
	/* BODY: one program+readback+compare per body chunk. */
	steps += chunks_of(image_length - AIUS_OTA_FLASH_HEAD_BYTES);
	/* PROGRAM_HEAD: one program+readback+compare per head chunk. */
	steps += AIUS_OTA_FLASH_HEAD_BYTES / AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	return steps;
}

/* --------------------------------------------------------------------------
 * Primitive calls (FASTRUN: the in-window path)
 *
 * Every one goes through a wrapper that (a) feeds the watchdog first and (b)
 * checks the geometry first, so the cadence promise
 * (AIUS_OTA_ACTIVATE_MAX_OPS_PER_FEED) and the bounds promise are properties of
 * the ONE place a primitive can be reached from, not of each call site.
 * ------------------------------------------------------------------------ */

static FASTRUN AiusOtaStatus halt(AiusOtaActivator *activator,
	                          AiusOtaActivateFault fault,
	                          AiusOtaStatus status)
{
	activator->phase = AIUS_OTA_ACTIVATE_FAULTED;
	activator->fault = fault;
	return status;
}

static FASTRUN void feed(AiusOtaActivator *activator)
{
	activator->primitives.watchdog_feed(activator->primitives.ctx);
}

static FASTRUN AiusOtaStatus do_erase(AiusOtaActivator *activator,
	                              uint32_t address)
{
	AiusOtaStatus status =
		aius_ota_activate_check_erase_in(&activator->window, address);
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

static FASTRUN AiusOtaStatus do_program(AiusOtaActivator *activator,
	                                uint32_t address, const uint8_t *data,
	                                uint32_t length)
{
	AiusOtaStatus status = aius_ota_activate_check_span_in(
		&activator->window, address, length);
	int fault;

	if (status != AIUS_OTA_OK)
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_GEOMETRY,
		            status);

	feed(activator);
	fault = activator->primitives.program(activator->primitives.ctx,
	                                      address, data, length);
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_PROGRAM,
		            AIUS_OTA_PRIMITIVE);
	}
	return AIUS_OTA_OK;
}

static FASTRUN AiusOtaStatus do_flash_read(AiusOtaActivator *activator,
	                                   uint32_t address, uint32_t length)
{
	AiusOtaStatus status = aius_ota_activate_check_span_in(
		&activator->window, address, length);
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
 * The dark-window phases (FASTRUN)
 * ------------------------------------------------------------------------ */

static FASTRUN uint32_t chunk_at(const AiusOtaActivator *activator,
	                         uint32_t cursor)
{
	uint32_t remaining = activator->image_length - cursor;

	return remaining < AIUS_OTA_ACTIVATE_CHUNK_BYTES
		? remaining
		: AIUS_OTA_ACTIVATE_CHUNK_BYTES;
}

/* Program one chunk from the staged source, read it back, and byte-compare the
 * readback against the SAME staged bytes. No digest: the whole image was
 * digested and signature-checked before the dark window (see
 * aius_ota_activate_begin()), so the in-window job is only to prove that what
 * the flash now holds equals what was verified. The compare is an inlined
 * blocks_differ over the caller's linear staged view, so it reaches no medium
 * and no hash while the flash is erased. Returns OK, or halts VERIFY. */
static FASTRUN AiusOtaStatus program_and_compare(AiusOtaActivator *activator,
	                                         uint32_t offset,
	                                         uint32_t length)
{
	const uint8_t *source = activator->staged + offset;
	AiusOtaStatus status;

	status = do_program(activator, activator->window.base + offset, source,
	                    length);
	if (status != AIUS_OTA_OK)
		return status;
	status = do_flash_read(activator, activator->window.base + offset,
	                       length);
	if (status != AIUS_OTA_OK)
		return status;
	if (blocks_differ(source, activator->readback, length))
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_VERIFY,
		            AIUS_OTA_VERIFY);

	activator->programmed_bytes += length;
	activator->verified_bytes += length;
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
static FASTRUN AiusOtaStatus step_erase_head(AiusOtaActivator *activator)
{
	AiusOtaStatus status = do_erase(activator, activator->window.base);

	if (status != AIUS_OTA_OK)
		return status;

	activator->phase = AIUS_OTA_ACTIVATE_BODY;
	activator->cursor = AIUS_OTA_FLASH_HEAD_BYTES;
	activator->erase_pending = true;
	return AIUS_OTA_OK;
}

/* BODY -- sectors 1..N-1, erase then program-and-compare, one operation-unit
 * per step. Each body chunk is programmed from the staged image and immediately
 * read back and compared, so a flash cell that reported success and did not
 * take is caught here, mid-body, while the FCB is still erased. */
static FASTRUN AiusOtaStatus step_body(AiusOtaActivator *activator)
{
	AiusOtaStatus status;
	uint32_t take;

	if (activator->erase_pending) {
		status = do_erase(activator,
		                  activator->window.base + activator->cursor);
		if (status != AIUS_OTA_OK)
			return status;
		activator->erase_pending = false;
		return AIUS_OTA_OK;
	}

	take = chunk_at(activator, activator->cursor);
	status = program_and_compare(activator, activator->cursor, take);
	if (status != AIUS_OTA_OK)
		return status;

	activator->cursor += take;
	if (activator->cursor == activator->image_length) {
		activator->phase = AIUS_OTA_ACTIVATE_PROGRAM_HEAD;
		activator->cursor = 0u;
	} else if (activator->cursor % AIUS_OTA_FLASH_SECTOR_BYTES == 0u) {
		activator->erase_pending = true;
	}
	return AIUS_OTA_OK;
}

/* PROGRAM_HEAD -- sector 0 LAST, from the staged image, one full chunk per step
 * (the head is a whole number of chunks). The same program-then-readback-compare
 * as the body, so a bad FCB byte is caught the instant it is written -- which is
 * the last thing that can go wrong before the caller may reset. */
static FASTRUN AiusOtaStatus step_program_head(AiusOtaActivator *activator)
{
	AiusOtaStatus status =
		program_and_compare(activator, activator->cursor,
		                    AIUS_OTA_ACTIVATE_CHUNK_BYTES);

	if (status != AIUS_OTA_OK)
		return status;

	activator->cursor += AIUS_OTA_ACTIVATE_CHUNK_BYTES;
	if (activator->cursor == AIUS_OTA_FLASH_HEAD_BYTES)
		activator->phase = AIUS_OTA_ACTIVATE_READY_TO_RESET;
	return AIUS_OTA_OK;
}

/* --------------------------------------------------------------------------
 * Pre-window verification and mark (FLASHMEM)
 * ------------------------------------------------------------------------ */

/* The WHOLE verification, run before the dark window, over the caller's staged
 * RAM copy against the armed header. Two independent checks, neither subsuming
 * the other:
 *
 *   1. Digest recompute: SHA-256 over every staged byte, compared against the
 *      armed header's payload_digest. This is what the staging record
 *      explicitly does not attest -- "it cannot say that the staging medium
 *      still holds those bytes; re-reading and re-digesting before programming
 *      is the activation slice's obligation". A card that decayed between
 *      staging and activation fails HERE, before anything is flashed.
 *   2. Signature: the injected verifier over the header's authentication
 *      digest, the same message and seam aius_ota_verify() checked at staging
 *      time, re-run so activation stands on its own authority rather than on a
 *      verdict a prior boot reached.
 *
 * A failure of either halts FAULTED with nothing flashed and no mark written.
 * This is the move that lets the dark window drop SHA-256 entirely. */
static FLASHMEM AiusOtaStatus verify_staged(AiusOtaActivator *activator,
	                                    const AiusOtaVerifier *verifier)
{
	uint8_t digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t auth[AIUS_OTA_DIGEST_BYTES];
	int fault;

	aius_sha256(activator->staged, activator->image_length, digest);
	if (blocks_differ(digest, activator->image_digest,
	                  AIUS_OTA_DIGEST_BYTES))
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_DIGEST,
		            AIUS_OTA_DIGEST);

	aius_sha256(activator->session->header_bytes,
		    AIUS_OTA_HEADER_AUTH_BYTES, auth);
	fault = verifier->verify(auth, activator->session->header.signature,
	                         verifier->ctx);
	if (fault != 0) {
		activator->primitive_fault = fault;
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_SIGNATURE,
		            AIUS_OTA_SIGNATURE);
	}
	return AIUS_OTA_OK;
}

/* MARK -- the health record's in-progress mark, written before the first erase,
 * and the whole reason power-loss forensics can tell "never started" from
 * "interrupted".
 *
 * There is ONE activation transition in the slice-1 health record --
 * aius_ota_health_note_activation() -- and its own contract already describes
 * this write: idempotent for the image already on trial, deliberately not
 * refreshing the budget, because "a power loss between the activation write and
 * the reboot must not hand a boot-looping image an unlimited supply of fresh
 * trials". So the in-progress mark and the new-image-trial transition are the
 * SAME WRITE, made here, before the first erase.
 *
 * HOW A READER TELLS THE THREE CASES APART, which is all the forensics needs:
 *
 *   record does not name the new image      -> never started.
 *   TRIAL(new) and the new image is running -> started, finished, on trial.
 *   TRIAL(new) and it is NOT running        -> started and interrupted.
 *
 * The third case is exactly aius_ota_activate_begin()'s rule 8, and on the next
 * boot aius_ota_health_note_boot() resolves it without help. A halted
 * activation leaves the mark standing, because the mark is the true statement --
 * an activation of this image was begun and did not complete. */
static FLASHMEM AiusOtaStatus mark_in_progress(AiusOtaActivator *activator)
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
	return AIUS_OTA_OK;
}

/* --------------------------------------------------------------------------
 * Interface
 * ------------------------------------------------------------------------ */

FLASHMEM AiusOtaStatus aius_ota_activate_begin(
	AiusOtaActivator *activator, AiusOtaSession *session,
	const AiusOtaActivatePrimitives *primitives,
	const AiusOtaVerifier *verifier,
	const AiusOtaActivateWindow *window,
	const uint8_t *staged, uint32_t staged_length,
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

	if (session == NULL || primitives == NULL || verifier == NULL ||
	    verifier->verify == NULL || staged == NULL || running_digest == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (primitives->erase_sector == NULL || primitives->program == NULL ||
	    primitives->flash_read == NULL || primitives->watchdog_feed == NULL ||
	    primitives->cycles == NULL || primitives->health_read == NULL ||
	    primitives->health_write == NULL)
		return AIUS_OTA_INVALID_ARG;
	if (!aius_ota_latch_is_armed(session))
		return AIUS_OTA_DISARMED;
	if (aius_ota_state(session) != AIUS_OTA_STATE_ACTIVATION_REQUESTED)
		return AIUS_OTA_STATE;

	status = aius_ota_progress(session, &progress);
	if (status != AIUS_OTA_OK)
		return status;
	/* The session must be fully staged, and the caller's linear view must be
	 * exactly that image -- a short PSRAM copy would program past its end. */
	if (progress.staged_length != progress.image_length ||
	    staged_length != progress.image_length)
		return AIUS_OTA_STATE;

	/* Rule 4b: a malformed window is the caller's configuration error, not
	 * a verdict about the image -- the activator stays IDLE. */
	status = aius_ota_activate_check_window(window);
	if (status != AIUS_OTA_OK)
		return status;

	/* From here the refusals are verdicts about this image on this board, so
	 * they latch a typed fault rather than just returning. */
	activator->session = session;
	activator->primitives = *primitives;
	activator->window = resolve_window(window);
	activator->staged = staged;
	activator->image_length = progress.image_length;
	activator->steps_total =
		aius_ota_activate_step_bound(progress.image_length);
	memcpy(activator->image_digest, session->header.payload_digest,
	       AIUS_OTA_DIGEST_BYTES);

	status = aius_ota_activate_check_image_length_in(
		&activator->window, progress.image_length);
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
	 * fresh board must be able to take its first upgrade. */
	(void)aius_ota_health_load(&activator->health, bytes, sizeof(bytes));

	/* RULE 8, the power-loss interlock, BEFORE the verify: a half-written
	 * flash is a refusal that outranks any digest verdict, and a resume must
	 * not spend a SHA-256 over the staged image to reach the same "no". */
	if (aius_ota_health_is_trialing(&activator->health) &&
	    !blocks_differ(activator->health.image_digest,
	                   activator->image_digest, AIUS_OTA_DIGEST_BYTES) &&
	    blocks_differ(running_digest, activator->image_digest,
	                  AIUS_OTA_DIGEST_BYTES))
		return halt(activator, AIUS_OTA_ACTIVATE_FAULT_INTERRUPTED,
		            AIUS_OTA_INTERRUPTED);

	/* THE PRE-WINDOW VERIFICATION. If it fails, nothing is flashed and no
	 * mark is written -- the board keeps serving. */
	status = verify_staged(activator, verifier);
	if (status != AIUS_OTA_OK)
		return status;

	/* The mark is the last pre-window act; a fault here still leaves a
	 * bootable board (nothing erased), with the mark either standing (the
	 * write took) or absent (it did not). */
	status = mark_in_progress(activator);
	if (status != AIUS_OTA_OK)
		return status;

	activator->phase = AIUS_OTA_ACTIVATE_ERASE_HEAD;
	return AIUS_OTA_OK;
}

FASTRUN AiusOtaStatus aius_ota_activate_step(AiusOtaActivator *activator)
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
	 * ACTIVATION_REQUESTED mid-sequence has withdrawn it. Read as a plain
	 * struct field, NOT through aius_ota_state(), which is FLASHMEM and would
	 * fault while the flash is erased. */
	if (activator->session == NULL ||
	    activator->session->state != AIUS_OTA_STATE_ACTIVATION_REQUESTED)
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
	case AIUS_OTA_ACTIVATE_ERASE_HEAD:
		status = step_erase_head(activator);
		break;
	case AIUS_OTA_ACTIVATE_BODY:
		status = step_body(activator);
		break;
	case AIUS_OTA_ACTIVATE_PROGRAM_HEAD:
		status = step_program_head(activator);
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

FASTRUN bool aius_ota_activate_is_dark(const AiusOtaActivator *activator)
{
	return activator != NULL &&
	       activator->phase >= AIUS_OTA_ACTIVATE_ERASE_HEAD &&
	       activator->phase <= AIUS_OTA_ACTIVATE_PROGRAM_HEAD;
}

FASTRUN AiusOtaActivatePhase aius_ota_activate_phase(
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
