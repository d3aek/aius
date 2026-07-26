#ifndef AIUS_SD_TEENSY_H
#define AIUS_SD_TEENSY_H

#ifndef __cplusplus
#error "aius_sd_teensy.h requires C++"
#endif

#include <stdint.h>

#include <SD.h>

#include "aius_sd.h"

#define AIUS_SD_TEENSY_DRIVER_FAULT UINT32_C(0x53444452)
#define AIUS_SD_TEENSY_PROTOCOL_FAULT UINT32_C(0x53445052)

typedef struct {
	File file;
	bool file_open;
	/* THE VOLUME IS MOUNTED AND BELIEVED LIVE.
	 *
	 * Every portable job begins at AIUS_SD_PHASE_BEGIN, so without this the
	 * adapter ran SD.begin() -- a full SDIO card re-identification and volume
	 * mount -- once per job. The first live measurement of a working replay
	 * put the cost of that beyond argument: 224 refill jobs, 224 mounts, and
	 * 20 of them (8.9 %) blocking for SdFat's whole BUSY_TIMEOUT_MICROS
	 * (1,000,000) before returning an I/O fault, while every other primitive
	 * in the same run completed in 15 us. The mount was both the throughput
	 * tax and the entire fault source.
	 *
	 * True here means a BEGIN is answered OK without touching the card. What
	 * keeps that honest is that it is REVALIDATED, not assumed: the 1 Hz probe
	 * calls SD.mediaPresent(), which on BUILTIN_SDCARD issues a real CMD13
	 * status request (SD.cpp:125), and a card that does not answer clears this
	 * flag. So hot removal is still detected on exactly the cadence it was
	 * before, and the next job performs a genuine mount.
	 *
	 * Cleared by: a failed mount, any driver fault, a media check that reports
	 * the card absent, aius_sd_teensy_invalidate_mount(), and init. */
	bool mounted;
	uint32_t last_duration_us;
	uint32_t longest_duration_us;
	uint32_t protocol_faults;
	uint32_t last_fault;
	/* Diagnostics. The adapter is the firmware's only caller of any SD
	 * primitive, so these are the whole truth about what reached the card.
	 *
	 * `mounts` counts SD.begin() calls that actually happened -- a BEGIN the
	 * file_open guard refused is not one. Latched around a job it answers
	 * "did the volume get remounted while this job held it", which is the
	 * question a job spanning many epochs cannot answer for itself.
	 *
	 * `driver_faults` counts the card saying no; `protocol_faults` above
	 * counts this adapter refusing to ask it. `last_protocol_action` names
	 * the refused action, so a refusal raised against a job this service
	 * never sees -- the replay refill job shares this adapter -- is still
	 * attributable. */
	uint32_t mounts;
	/* BEGINs answered from the cached mount, i.e. card transactions NOT
	 * performed. Beside `mounts` so the pair reads as a hit/miss ratio: a
	 * healthy running system should show reuses climbing and mounts nearly
	 * still. */
	uint32_t mount_reuses;
	uint32_t driver_faults;
	uint8_t last_protocol_action;
} AiusSdTeensy;

void aius_sd_teensy_init(AiusSdTeensy *adapter);

/* Forces the next BEGIN to perform a real mount.
 *
 * The adapter invalidates its own cache on every signal it can see -- a failed
 * mount, a driver fault, a media check that reports the card gone. This is for
 * the signals it CANNOT see: a caller that has decided the volume is no longer
 * trustworthy (a card transition the lifecycle observed, a driver reset) says so
 * explicitly rather than leaving the adapter to infer it. Memory only; it starts
 * no card transaction and cannot fail. */
void aius_sd_teensy_invalidate_mount(AiusSdTeensy *adapter);

AiusSdEvent aius_sd_teensy_execute(
	AiusSdTeensy *adapter,
	const AiusSdAction *action);

#endif
