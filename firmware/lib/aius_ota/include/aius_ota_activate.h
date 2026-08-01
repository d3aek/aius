#ifndef AIUS_OTA_ACTIVATE_H
#define AIUS_OTA_ACTIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_ota.h"
#include "aius_ota_health.h"
#include "aius_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The activation sequencer: the portable half of the flash-programming slice.
 * It decides WHAT to erase, program, read back and record, in WHAT order, and
 * it refuses everything the published flash geometry does not admit. It does
 * none of it. Every effect -- the staged image's bytes, the three flash
 * primitives, the watchdog, the cycle counter and the health record's storage
 * -- is an injected callback, so the whole sequence is proven host-side against
 * a fake flash device before a board is asked.
 *
 * READ THIS BEFORE COMPOSING IT. Nothing in this library, including this file,
 * may execute during the erase/program window it orchestrates.
 * aius-ota-upgrade.md §The Teensy 4 memory model records why: on this part
 * ordinary code runs from ITCM and FLASHMEM is the opt-in to stay in flash, so
 * a FLASHMEM function becomes unexecutable while the flash is mid-operation --
 * and every definition in this library is FLASHMEM, deliberately, because it is
 * cold. THE SEQUENCER IS NOT THE PROGRAMMER. The composition slice owns:
 *
 *   - the run-to-completion caller (see aius_ota_activate_run()), which must
 *     itself be RAM-resident,
 *   - the RAM-resident primitive implementations wrapping
 *     eepromemu_flash_erase_sector() / eepromemu_flash_write(), whose whole
 *     call graph -- memcpy included -- must be verified non-FLASHMEM post-link,
 *   - the quiesce (owner decision 2026-07-31: audio stops during flashing;
 *     gate closed, audio service stopped, ISR sources parked), which is what
 *     discharges RM p.279 note 3's "no FLASH access from ANY master"
 *     structurally rather than by timing analysis,
 *   - the reset. This library never resets anything; it reports
 *     AIUS_OTA_ACTIVATE_READY_TO_RESET and stops, and SYSRESETREQ through the
 *     runtime's existing reset path is the caller's next act.
 *
 * A caller that drives this sequencer with the flash primitives resident in
 * flash will hang the part mid-erase. That is the one way to misuse it and no
 * signature can prevent it, so it is stated here, at the top, in the file a
 * composer reads first.
 *
 * SELF-FLASH BOUNDS LIVE HERE. aius-ota-upgrade.md §Self-flashing is possible
 * records that the shipped driver has "no bounds checking whatsoever" -- the
 * address is masked to 24 bits and passed straight through, and erasing sector
 * 0 destroys the FCB. This layer is where the bounds live: every address this
 * sequencer hands a primitive has passed aius_ota_activate_check_erase() or
 * aius_ota_activate_check_span(), and both are published so the composition's
 * primitive wrapper can re-check at the boundary it owns. */

/* --------------------------------------------------------------------------
 * Flash geometry
 *
 * Published, because three parties must agree on it: this sequencer, the
 * composition's RAM-resident primitives, and any host tool reasoning about
 * where an image lands. A second copy of "0x1000" is a second place to drift.
 * ------------------------------------------------------------------------ */

/* The boot flash's application-addressable base. HalfKay occupies 0 bytes of
 * the 8 MiB (it is a physically separate chip), so the whole region belongs to
 * the image -- aius-ota-upgrade.md §HalfKay cannot be bricked. */
#define AIUS_OTA_FLASH_BASE UINT32_C(0x60000000)

/* W25Q64JV sector erase (opcode 0x20), the granule
 * eepromemu_flash_erase_sector() drives. */
#define AIUS_OTA_FLASH_SECTOR_BYTES 4096u

/* The two structures the boot ROM parses, at their fixed offsets from base.
 * The FCB configures FlexSPI; the IVT names the entry point. Erasing EITHER
 * makes the board un-ROM-bootable -- and recoverable, because HalfKay survives
 * and can reflash. That is the property this sequencer's ordering is built on.
 *
 * THE ARITHMETIC, EXACTLY: the IVT offset is 0x1000 and the sector is 0x1000,
 * so the FCB is the whole of sector 0 and the IVT is the FIRST BYTE OF SECTOR
 * 1. They are not in the same sector. A reader who assumes "the first sector
 * holds the FCB and the IVT" is wrong by one sector, which is why both offsets
 * are published and a _Static_assert in aius_ota_activate.c pins the
 * relationship rather than a comment claiming it. */
#define AIUS_OTA_FLASH_FCB_OFFSET 0u
#define AIUS_OTA_FLASH_IVT_OFFSET 0x1000u

/* The head region programmed LAST: sector 0, the FCB. See
 * aius_ota_activate_begin() for the ordering argument. */
#define AIUS_OTA_FLASH_HEAD_BYTES AIUS_OTA_FLASH_SECTOR_BYTES

/* Flash the application may address, from aius-ota-upgrade.md §Sizing. The
 * container's own AIUS_OTA_MAX_IMAGE_BYTES is the same number by construction;
 * aius_ota_activate.c static-asserts they cannot drift apart. */
#define AIUS_OTA_FLASH_USABLE_BYTES AIUS_OTA_MAX_IMAGE_BYTES

/* The smallest thing this sequencer will call an image: one byte past the IVT.
 * Anything shorter cannot carry the structure the ROM parses, and would leave
 * the sequencer with no body to program at all. */
#define AIUS_OTA_ACTIVATE_MIN_IMAGE_BYTES (AIUS_OTA_FLASH_IVT_OFFSET + 1u)

/* The unit of every program and every readback compare. 256 is the part's page
 * program granule; it also divides the sector, which is what makes "every
 * program lies within one sector" true by arithmetic rather than by a runtime
 * clamp, and it is half the SD transport's own 512-byte slice
 * (AIUS_SD_MAX_TRANSFER), so a composition reading the staged image from the
 * card serves one chunk per SD read with nothing left over. */
#define AIUS_OTA_ACTIVATE_CHUNK_BYTES 256u

/* The watchdog cadence this sequencer promises, in primitive calls per feed.
 * ONE: every erase, every program, every readback and every staged read is
 * immediately preceded by a watchdog_feed(), so no two primitive calls are ever
 * adjacent. A sector erase on this part is specified as a range and the plan's
 * §Measuring the sector-erase stall could not bound it locally, so the cadence
 * is stated in operations -- which the operation trace can check -- rather than
 * in time, which it cannot. */
#define AIUS_OTA_ACTIVATE_MAX_OPS_PER_FEED 1u

/* --------------------------------------------------------------------------
 * Injected primitives
 * ------------------------------------------------------------------------ */

/* Reads `length` bytes of the STAGED image at `offset` (an offset into the
 * image, not a flash address) into `destination`. Returns 0 on success; any
 * non-zero value halts the sequencer and is latched verbatim, so an operator
 * sees the medium's own fault code.
 *
 * Chunked with an offset and a length because that is how the composition will
 * read it: from the SD card, whose job engine is bounded at 512 bytes a
 * transfer. The sequencer reads the staged image THREE times -- once to
 * program the body, once to verify and digest the whole of it, once to program
 * the head -- and never holds more than one chunk. */
typedef int (*AiusOtaActivateStagedReadFn)(void *ctx, uint32_t offset,
	                                   uint8_t *destination,
	                                   uint32_t length);

/* Erases the 4096-byte sector at `address` (an absolute flash address, always
 * sector-aligned and always inside the usable region -- checked before the
 * call). Returns 0 on success. */
typedef int (*AiusOtaActivateEraseFn)(void *ctx, uint32_t address);

/* Programs `length` bytes at `address`. The sequencer guarantees
 * 0 < length <= AIUS_OTA_ACTIVATE_CHUNK_BYTES, that the whole span lies inside
 * one sector, and that the sector was erased earlier in this same activation.
 * Returns 0 on success. */
typedef int (*AiusOtaActivateProgramFn)(void *ctx, uint32_t address,
	                                const uint8_t *data, uint32_t length);

/* Reads `length` bytes of FLASH at `address` back into `destination`, under
 * the same span guarantees as the program callback. Returns 0 on success.
 *
 * Separate from a plain memcpy off the address because the target's read path
 * is not a plain load: the driver's cache maintenance (arm_dcache_delete(),
 * then FLEXSPI_MCR0 |= SWRESET to purge the AHB read FIFO) is mandatory after
 * an erase or program, and a readback that skipped it would verify the FIFO
 * rather than the flash. Whoever owns that maintenance owns this callback. */
typedef int (*AiusOtaActivateFlashReadFn)(void *ctx, uint32_t address,
	                                  uint8_t *destination,
	                                  uint32_t length);

/* Feeds the watchdog. No return: a watchdog that cannot be fed is not a
 * condition this sequencer could act on. */
typedef void (*AiusOtaActivateWatchdogFn)(void *ctx);

/* A free-running monotonic cycle counter (ARM_DWT_CYCCNT on the target).
 * 32-bit and therefore wrapping -- at 600 MHz it wraps every 7.16 s, which one
 * sector erase can plausibly approach -- so the sequencer only ever takes
 * DIFFERENCES across one step and accumulates them into 64 bits. Never compare
 * two samples with < or >. */
typedef uint32_t (*AiusOtaActivateCyclesFn)(void *ctx);

/* Reads / writes the persisted image-health record, exactly
 * AIUS_OTA_HEALTH_RECORD_BYTES bytes, wherever the composition persists it.
 * Returns 0 on success.
 *
 * A board with no record yet must return 0 with zeroed bytes rather than a
 * fault: aius_ota_health_load() reports those as CORRUPT and the sequencer
 * reads that, fail-closed, as "no recorded history", which is the honest state
 * of a fresh board and must not block its first upgrade. Reserve a non-zero
 * return for a medium that genuinely could not be read. */
typedef int (*AiusOtaActivateHealthReadFn)(void *ctx, uint8_t *bytes,
	                                   uint32_t length);
typedef int (*AiusOtaActivateHealthWriteFn)(void *ctx, const uint8_t *bytes,
	                                    uint32_t length);

typedef struct {
	void *ctx;
	AiusOtaActivateStagedReadFn staged_read;
	AiusOtaActivateEraseFn erase_sector;
	AiusOtaActivateProgramFn program;
	AiusOtaActivateFlashReadFn flash_read;
	AiusOtaActivateWatchdogFn watchdog_feed;
	AiusOtaActivateCyclesFn cycles;
	AiusOtaActivateHealthReadFn health_read;
	AiusOtaActivateHealthWriteFn health_write;
} AiusOtaActivatePrimitives;

/* --------------------------------------------------------------------------
 * Sequence
 * ------------------------------------------------------------------------ */

/* The phases, in the order they run. Each is driven by
 * aius_ota_activate_step(), which does BOUNDED work per call -- one erase, one
 * programmed chunk, one verified chunk -- so the phase is a position in the
 * sequence, not a blocking routine.
 *
 * IDLE and the two terminals are outside the sequence proper. Everything from
 * ERASE_HEAD to VERIFY_HEAD is the DARK WINDOW: the FCB is erased, the board
 * cannot boot, and the caller must be quiesced and running to completion.
 * aius_ota_activate_is_dark() answers "is the next step inside it". */
typedef enum {
	AIUS_OTA_ACTIVATE_IDLE = 0,       /* begin() not called, or it refused */
	AIUS_OTA_ACTIVATE_MARK = 1,       /* write the activation mark; no flash */
	AIUS_OTA_ACTIVATE_ERASE_HEAD = 2, /* erase sector 0: the FCB goes first */
	AIUS_OTA_ACTIVATE_BODY = 3,       /* erase+program sectors 1..N-1 */
	AIUS_OTA_ACTIVATE_VERIFY = 4,     /* readback compare + digest recompute */
	AIUS_OTA_ACTIVATE_PROGRAM_HEAD = 5, /* program sector 0, LAST */
	AIUS_OTA_ACTIVATE_VERIFY_HEAD = 6,  /* final head readback */
	AIUS_OTA_ACTIVATE_READY_TO_RESET = 7, /* terminal: the caller resets */
	AIUS_OTA_ACTIVATE_FAULTED = 8         /* terminal: halted, honestly */
} AiusOtaActivatePhase;

/* Why the halt is typed rather than a bare status: an operator standing at a
 * board that will not boot needs to know which of the six primitives said no,
 * and a status alone cannot carry that. The primitive's OWN return value is
 * latched beside it (`primitive_fault`), the same shape the session latches
 * `verifier_fault`. */
typedef enum {
	AIUS_OTA_ACTIVATE_FAULT_NONE = 0,
	AIUS_OTA_ACTIVATE_FAULT_STAGED_READ,   /* the staged image would not read */
	AIUS_OTA_ACTIVATE_FAULT_ERASE,         /* erase_sector() refused */
	AIUS_OTA_ACTIVATE_FAULT_PROGRAM,       /* program() refused */
	AIUS_OTA_ACTIVATE_FAULT_FLASH_READ,    /* flash_read() refused */
	AIUS_OTA_ACTIVATE_FAULT_HEALTH_READ,   /* the health record would not read */
	AIUS_OTA_ACTIVATE_FAULT_HEALTH_WRITE,  /* the health record would not write */
	AIUS_OTA_ACTIVATE_FAULT_HEALTH_RULE,   /* the health record refused the mark */
	AIUS_OTA_ACTIVATE_FAULT_VERIFY,        /* flash readback != the staged image */
	AIUS_OTA_ACTIVATE_FAULT_DIGEST,        /* staged image != the armed digest */
	AIUS_OTA_ACTIVATE_FAULT_HEAD_VERIFY,   /* the head readback != what verify saw */
	AIUS_OTA_ACTIVATE_FAULT_GEOMETRY,      /* an address/length the geometry forbids */
	AIUS_OTA_ACTIVATE_FAULT_INTERRUPTED,   /* a prior activation is in progress */
	AIUS_OTA_ACTIVATE_FAULT_SESSION,       /* the session moved under the sequencer */
	AIUS_OTA_ACTIVATE_FAULT_INTERNAL       /* the step budget was exceeded */
} AiusOtaActivateFault;

/* NO RETRY, ANYWHERE. Every fault above halts the sequencer in FAULTED and it
 * stays there: aius_ota_activate_step() will not run another primitive. The
 * plan's recovery boundary is HalfKay plus watchdog recovery-safe mode, both of
 * which need the board's state to be HONEST, not optimistic. A retry loop over
 * a flash primitive that just failed mid-erase would spend the operator's one
 * reliable diagnosis -- "which operation, at which address, with which driver
 * code" -- to buy an outcome the driver already said it could not deliver. */

typedef struct {
	AiusOtaActivatePhase phase;
	AiusOtaActivateFault fault;
	int primitive_fault;
	bool dark; /* the NEXT step must run quiesced, to completion */
	uint32_t image_length;
	/* Both count flash operations, not staged reads, and both land on
	 * image_length exactly once the sequence completes: every byte is
	 * programmed once (body during BODY, head during PROGRAM_HEAD) and read
	 * back once (body during VERIFY, head during VERIFY_HEAD). */
	uint32_t programmed_bytes;
	uint32_t verified_bytes;
	uint32_t steps_taken;
	uint32_t steps_total; /* aius_ota_activate_step_bound(image_length) */
	/* Step evidence, from the injected cycle counter. Accumulated from
	 * per-step differences, so a wrapping 32-bit counter is not a lie.
	 * `dark_window_cycles` is the number aius-ota-upgrade.md wants out of
	 * bring-up: the plan dropped the dedicated stall measurement on the
	 * grounds that "the dark-window duration falls out of the activation
	 * slice's own bring-up evidence". This is that evidence. */
	uint64_t dark_window_cycles;
	uint32_t max_step_cycles;
} AiusOtaActivateProgress;

typedef struct {
	AiusOtaActivatePhase phase;
	AiusOtaActivateFault fault;
	int primitive_fault;
	/* Borrowed, not owned. Held so every step can re-check that the session
	 * is still the one that authorized this activation. */
	AiusOtaSession *session;
	AiusOtaActivatePrimitives primitives;
	AiusOtaHealth health;
	uint32_t image_length;
	uint8_t image_digest[AIUS_OTA_DIGEST_BYTES];
	/* Position within the current phase: a byte offset for BODY, VERIFY,
	 * PROGRAM_HEAD and VERIFY_HEAD. */
	uint32_t cursor;
	bool erase_pending; /* BODY: the next step erases rather than programs */
	uint32_t programmed_bytes;
	uint32_t verified_bytes;
	uint32_t steps_taken;
	uint32_t steps_total;
	AiusSha256 image_hash;
	AiusSha256 head_hash;
	/* The head region's digest as the VERIFY pass read it from the staged
	 * image, checked against a readback of the programmed head. This is what
	 * lets the head be verified without buffering 4 KiB: the reference is 32
	 * bytes and it was captured before the head was written. */
	uint8_t head_digest[AIUS_OTA_DIGEST_BYTES];
	uint64_t dark_window_cycles;
	uint32_t max_step_cycles;
	uint8_t staged[AIUS_OTA_ACTIVATE_CHUNK_BYTES];
	uint8_t readback[AIUS_OTA_ACTIVATE_CHUNK_BYTES];
} AiusOtaActivator;

/* Checks every precondition and arms the sequence. Nothing is erased,
 * programmed or written here.
 *
 * `running_digest` is the payload digest of the image CURRENTLY RUNNING -- the
 * same 32 bytes the composition already hands aius_ota_health_note_boot() at
 * every boot, so this asks for nothing new. All-zero spells "unknown", which is
 * fail-closed for the resume refusal below.
 *
 * The rules, in the order applied:
 *
 *   1. activator/session/primitives/running_digest non-null, and every one of
 *      the eight callbacks present ......................... INVALID_ARG
 *   2. the feature latch is armed .............................. DISARMED
 *   3. session state == ACTIVATION_REQUESTED ...................... STATE
 *   4. staged_length == image_length .............................. STATE
 *   5. image_length in [MIN_IMAGE_BYTES, FLASH_USABLE_BYTES] ... GEOMETRY
 *   6. the armed header's payload_digest is not all-zero ...... MALFORMED
 *   7. health_read() succeeds ................................. PRIMITIVE
 *   8. no prior activation of THIS image is recorded in
 *      progress ............................................. INTERRUPTED
 *
 * Rules 1-4 leave the activator IDLE: they are the caller's error or the
 * session's, nothing about this image was judged. Rules 5-8 leave it FAULTED
 * with a typed fault: this image cannot be activated on this board, and that
 * is a verdict worth latching.
 *
 * RULE 8 IS THE POWER-LOSS INTERLOCK, and it is why `running_digest` is
 * required. The health record is marked before the first erase (see
 * AIUS_OTA_ACTIVATE_MARK), so a record reading TRIAL for the image about to be
 * activated, while some OTHER image is running, can only mean one thing: an
 * activation of it started and did not finish. The flash may hold half an
 * image. The sequencer refuses -- INTERRUPTED -- rather than resuming into a
 * state nobody can vouch for; aius_ota_health_note_activation()'s idempotence
 * would have permitted a silent retry, and this declines it deliberately. The
 * remedies are the plan's: HalfKay if the board is dark, otherwise
 * aius_ota_abandon_activation() and a fresh staged image. The one case this
 * does NOT refuse is re-activating the image that is already running its trial,
 * where the record and the running image agree and nothing is half-written.
 *
 * A corrupt or absent health record is NOT a refusal: aius_ota_health_load()
 * fails closed to UNKNOWN, which is exactly a fresh board, and a fresh board
 * must be able to take its first upgrade. */
AiusOtaStatus aius_ota_activate_begin(
	AiusOtaActivator *activator, AiusOtaSession *session,
	const AiusOtaActivatePrimitives *primitives,
	const uint8_t running_digest[AIUS_OTA_DIGEST_BYTES]);

/* Runs ONE bounded step and returns. AIUS_OTA_OK means the step ran and the
 * sequence continues or has reached READY_TO_RESET; any other status means the
 * sequencer has halted in FAULTED and named why.
 *
 * The bound per call is one primitive operation plus at most one staged read:
 * one sector erase, or one 256-byte program, or one 256-byte readback compare.
 * That is what lets the pre-dark phases be driven from the epoch loop. It is
 * NOT a claim about time -- a sector erase on this part is a millisecond-scale
 * operation with no local upper bound -- which is why the caller must be in the
 * dark window before it steps into ERASE_HEAD.
 *
 * Every step re-checks that the session is still ACTIVATION_REQUESTED. A
 * session moved out from under a live sequence (aius_ota_abandon_activation()
 * called mid-window, say) halts it with FAULT_SESSION rather than continuing to
 * program flash on an authorization that has been withdrawn.
 *
 * The FEATURE LATCH is deliberately NOT re-checked per step. It is checked once
 * at begin(); after the FCB is erased the only outcomes are "finish and reset"
 * and "halt into HalfKay", and disarming the latch mid-window must not be a
 * third one that leaves the board dark for a reason unrelated to the flash. */
AiusOtaStatus aius_ota_activate_step(AiusOtaActivator *activator);

/* Steps until a terminal phase: the RUN-TO-COMPLETION mode. Returns
 * AIUS_OTA_OK only at READY_TO_RESET.
 *
 * THIS is the entry point the composition calls once the device is quiesced,
 * and THIS is the function that must be RAM-resident along with everything it
 * reaches -- which this library, being FLASHMEM throughout, is not. The caller
 * therefore does not call this one either: it writes its own loop over
 * aius_ota_activate_step() in a non-FLASHMEM translation unit, and this
 * function stands as the executable specification of that loop (and is what the
 * host suite drives). See the file header. */
AiusOtaStatus aius_ota_activate_run(AiusOtaActivator *activator);

/* True when the NEXT step must run with the device quiesced and driven to
 * completion: the phase is between ERASE_HEAD and VERIFY_HEAD inclusive. The
 * transition to true happens the moment MARK completes, so the composition
 * polls this each epoch and quiesces on the edge. */
bool aius_ota_activate_is_dark(const AiusOtaActivator *activator);

AiusOtaActivatePhase aius_ota_activate_phase(const AiusOtaActivator *activator);

AiusOtaStatus aius_ota_activate_progress(const AiusOtaActivator *activator,
	                                 AiusOtaActivateProgress *progress);

/* The EXACT number of steps a fault-free activation of `image_length` takes.
 * Not an estimate: aius_ota_activate_run() uses it as a runaway guard (a
 * sequence that exceeds it has stopped making progress and halts with
 * FAULT_INTERNAL), the caller uses it as the denominator of a progress
 * fraction, and the host suite asserts the happy path lands on it exactly, so
 * the formula and the step machine cannot drift apart silently.
 *
 * Zero for a length the geometry refuses. */
uint32_t aius_ota_activate_step_bound(uint32_t image_length);

/* THE BOUNDS THE SHIPPED DRIVER DOES NOT HAVE. Both are pure and published: the
 * sequencer routes every primitive call through them, and the composition's
 * RAM-resident wrapper should re-check at its own boundary, because the driver
 * behind it masks the address to 24 bits and erases whatever that names.
 *
 * check_erase: sector-aligned, and inside [BASE, BASE + USABLE).
 * check_span:  0 < length <= CHUNK_BYTES, the whole span inside ONE sector, and
 *              inside [BASE, BASE + USABLE).
 *
 * GEOMETRY names an address the flash does not have or an alignment it cannot
 * accept; INVALID_ARG names a length this interface forbids. */
AiusOtaStatus aius_ota_activate_check_erase(uint32_t address);
AiusOtaStatus aius_ota_activate_check_span(uint32_t address, uint32_t length);

/* The image-length rule of rule 5 above, separately callable so a caller can
 * refuse an image before staging megabytes of it. */
AiusOtaStatus aius_ota_activate_check_image_length(uint32_t image_length);

#ifdef __cplusplus
}
#endif

#endif
