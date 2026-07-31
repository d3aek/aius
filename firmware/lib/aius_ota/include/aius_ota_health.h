#ifndef AIUS_OTA_HEALTH_H
#define AIUS_OTA_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aius_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Image-health and rollback bookkeeping: the record and its transition rules,
 * logic only. Nothing here reads a clock, a reset cause or a watchdog, and
 * nothing here modifies lib/aius_boot -- it is designed to COMPOSE with the
 * boot ledger, and the wiring is a later slice.
 *
 * This answers, structurally, the four gaps aius-ota-upgrade.md §The rollback
 * substrate mostly exists records against the boot ledger:
 *
 *   1. "mark_healthy() only clears in NORMAL mode -- an A/B scheme needs to
 *      decide whether a newly flashed image gets a fresh trial from
 *      recovery-safe." Decided here: YES. This record is keyed on IMAGE
 *      IDENTITY, not on boot mode, so a different image always gets a fresh
 *      trial budget -- the same principle aius_boot already applies when a
 *      changed build_id clears the counter and the mode. The boot ledger keeps
 *      owning recovery-safe; the two are deliberately not coupled, and the
 *      caller decides what "healthy" means (today: the first complete epoch at
 *      least 10 s after construction).
 *
 *   2. "There is no notion of which slot am I running." There is no slot here
 *      at all. An image is identified by its payload SHA-256, which is
 *      slot-independent by construction and works unchanged for the staged
 *      single-image posture the owner chose and for fused A/B if it is ever
 *      authorized.
 *
 *   3. "The ledger's address is layout-dependent." This record is a byte
 *      layout with published offsets and its own CRC, so an image with a
 *      different DMAMEM layout reads exactly the same bytes.
 *
 *   4. "Power-cycle survival is unaddressed." The record is medium-agnostic
 *      and RARELY WRITTEN -- once per activation, once per boot, once per
 *      commit or rollback -- which is precisely the "rarely written
 *      non-volatile slot" that section says is a different question from the
 *      per-epoch checkpoint write, and likely fine.
 *
 * The four states are the vocabulary the observability table asks for:
 * "new image trial", "committed healthy", "rolled back", and the honest
 * "unknown" for an image nobody recorded (a USB reflash, a fresh board). */

#define AIUS_OTA_HEALTH_MAGIC UINT32_C(0x48554941) /* "AIUH" */
#define AIUS_OTA_HEALTH_VERSION 1u
#define AIUS_OTA_HEALTH_RECORD_BYTES 112u
#define AIUS_OTA_HEALTH_RESERVED1_BYTES 28u

/*    0   4  magic "AIUH"
 *    4   2  version = 1
 *    6   2  record_length = 112
 *    8   1  state
 *    9   1  trial_boots_remaining
 *   10   1  trial_boots_budget
 *   11   1  reserved0 = 0
 *   12   4  rollback_count
 *   16  32  image_digest
 *   48  32  previous_digest
 *   80  28  reserved1 = 0
 *  108   4  CRC-32 over bytes [0, 108)
 */
#define AIUS_OTA_HEALTH_OFFSET_MAGIC 0u
#define AIUS_OTA_HEALTH_OFFSET_VERSION 4u
#define AIUS_OTA_HEALTH_OFFSET_LENGTH 6u
#define AIUS_OTA_HEALTH_OFFSET_STATE 8u
#define AIUS_OTA_HEALTH_OFFSET_TRIAL_REMAINING 9u
#define AIUS_OTA_HEALTH_OFFSET_TRIAL_BUDGET 10u
#define AIUS_OTA_HEALTH_OFFSET_RESERVED0 11u
#define AIUS_OTA_HEALTH_OFFSET_ROLLBACK_COUNT 12u
#define AIUS_OTA_HEALTH_OFFSET_IMAGE_DIGEST 16u
#define AIUS_OTA_HEALTH_OFFSET_PREVIOUS_DIGEST 48u
#define AIUS_OTA_HEALTH_OFFSET_RESERVED1 80u
#define AIUS_OTA_HEALTH_OFFSET_CRC32 108u

/* Two, matching lib/aius_boot's two early watchdog resets before recovery-safe
 * latches. A trial image gets the same number of chances to reach healthy that
 * the existing recovery boundary already gives it, so the two mechanisms
 * cannot disagree about how many failures is too many. */
#define AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS 2u
#define AIUS_OTA_HEALTH_MAX_TRIAL_BOOTS 8u

typedef enum {
	AIUS_OTA_HEALTH_UNKNOWN = 0,     /* no recorded fate for the running image */
	AIUS_OTA_HEALTH_TRIAL = 1,       /* new image on trial, budget running */
	AIUS_OTA_HEALTH_COMMITTED = 2,   /* reached healthy; this is the good image */
	AIUS_OTA_HEALTH_ROLLED_BACK = 3  /* a trial failed; the fallback is running */
} AiusOtaHealthState;

/* What note_boot() concluded about the boot it was just told about. */
typedef enum {
	AIUS_OTA_HEALTH_ACTION_NONE = 0,
	AIUS_OTA_HEALTH_ACTION_TRIAL_CONTINUES, /* budget spent one, some left */
	AIUS_OTA_HEALTH_ACTION_ROLL_BACK,       /* budget exhausted; caller must revert */
	AIUS_OTA_HEALTH_ACTION_ROLLED_BACK,     /* the fallback booted; trial failed */
	AIUS_OTA_HEALTH_ACTION_ADOPTED          /* an image nobody recorded is running */
} AiusOtaHealthAction;

/* The in-memory form. The CRC lives only in the serialized bytes, unlike
 * lib/aius_boot's ledger which carries it in the struct -- here the struct is
 * never the persisted artifact, the byte layout is.
 *
 * INVARIANT: `image_digest` always names the image whose fate this record
 * describes -- the trial image while TRIAL, otherwise the image believed to be
 * running. `previous_digest` names the fallback a rollback would return to,
 * and is all-zero when there is none. */
typedef struct {
	AiusOtaHealthState state;
	uint8_t trial_boots_remaining;
	uint8_t trial_boots_budget;
	uint32_t rollback_count;
	uint8_t image_digest[AIUS_OTA_DIGEST_BYTES];
	uint8_t previous_digest[AIUS_OTA_DIGEST_BYTES];
} AiusOtaHealth;

/* UNKNOWN, no image, no fallback. */
void aius_ota_health_init(AiusOtaHealth *health);

/* Records that `image_digest` is being activated and starts its trial with
 * `trial_boots` boots of budget.
 *
 * The fallback is chosen so a failed trial can never become a rollback target:
 * from COMMITTED / ROLLED_BACK / UNKNOWN the fallback becomes the currently
 * recorded image, but from TRIAL (a second image superseding a first that
 * never committed) the EXISTING fallback is kept.
 *
 * IDEMPOTENT for the image already on trial: re-noting the same digest is OK
 * and deliberately does NOT refresh the budget. A power loss between the
 * activation write and the reboot must not hand a boot-looping image an
 * unlimited supply of fresh trials, and re-noting is exactly what a
 * power-loss-tolerant activation path does on retry.
 *
 * INVALID_ARG on a null argument, an all-zero digest, or a budget of zero or
 * more than AIUS_OTA_HEALTH_MAX_TRIAL_BOOTS. */
AiusOtaStatus aius_ota_health_note_activation(
	AiusOtaHealth *health,
	const uint8_t image_digest[AIUS_OTA_DIGEST_BYTES],
	uint8_t trial_boots);

/* Tells the record which image just booted, and reports what follows.
 *
 *   TRIAL + the trial image booted   -> spend one boot of budget.
 *                                       TRIAL_CONTINUES, or ROLL_BACK when the
 *                                       budget reaches zero. The record stays
 *                                       TRIAL until note_rollback() confirms
 *                                       the revert actually happened.
 *   TRIAL + the fallback booted      -> the trial image did not come up at all.
 *                                       ROLLED_BACK, applied immediately: the
 *                                       fallback becomes the recorded image and
 *                                       rollback_count increments. This is the
 *                                       slot-free failure detector -- identity
 *                                       comparison, no slot index needed.
 *   any state + an unrecorded image  -> ADOPTED. Someone flashed over USB;
 *                                       the record adopts it as UNKNOWN rather
 *                                       than keeping a fate that belongs to a
 *                                       different image. Mirrors aius_boot
 *                                       clearing its state on a changed
 *                                       build_id.
 *   otherwise                        -> NONE.
 *
 * `action` may be null when the caller only wants the state updated. */
AiusOtaStatus aius_ota_health_note_boot(
	AiusOtaHealth *health,
	const uint8_t running_digest[AIUS_OTA_DIGEST_BYTES],
	AiusOtaHealthAction *action);

/* TRIAL -> COMMITTED for the image on trial. Idempotent once COMMITTED.
 * AIUS_OTA_STATE from any other state, or for any other image -- an image
 * cannot vouch for one it is not.
 *
 * `previous_digest` is deliberately KEPT on commit: it is no longer a rollback
 * target, but it is the provenance of what this image replaced, and an
 * operator asking "what did this board upgrade from" has nowhere else to
 * look. */
AiusOtaStatus aius_ota_health_mark_healthy(
	AiusOtaHealth *health,
	const uint8_t running_digest[AIUS_OTA_DIGEST_BYTES]);

/* Confirms a revert to the fallback: TRIAL -> ROLLED_BACK, the fallback
 * becomes the recorded image and rollback_count increments (saturating).
 * AIUS_OTA_STATE unless the state is TRIAL and a fallback exists. */
AiusOtaStatus aius_ota_health_note_rollback(AiusOtaHealth *health);

bool aius_ota_health_is_trialing(const AiusOtaHealth *health);

AiusOtaStatus aius_ota_health_save(const AiusOtaHealth *health, uint8_t *bytes,
	                           size_t length);

/* Fail-closed like the staging record: a magic/length/CRC/domain violation is
 * CORRUPT and leaves the record initialized to UNKNOWN, which is the same
 * state a board with no record at all is in. */
AiusOtaStatus aius_ota_health_load(AiusOtaHealth *health, const uint8_t *bytes,
	                           size_t length);

#ifdef __cplusplus
}
#endif

#endif
