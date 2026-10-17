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

/* The mode every boot mounts the card in: 1 is DMA SDIO, 0 FIFO. A build
 * switch, not a persisted setting, so an A/B rollback lands on the previous
 * image's own boot mode (Phase 4zk). aius_sd_teensy_init still starts FIFO;
 * aius_runtime_storage_construct applies this after init. */
#ifndef AIUS_SD_BOOT_DMA
#define AIUS_SD_BOOT_DMA 0
#endif

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
	 * the refused action, so a refusal stays attributable to the action that
	 * raised it. */
	uint32_t mounts;
	/* BEGINs answered from the cached mount, i.e. card transactions NOT
	 * performed. Beside `mounts` so the pair reads as a hit/miss ratio: a
	 * healthy running system should show reuses climbing and mounts nearly
	 * still. */
	uint32_t mount_reuses;
	/* A failed primitive whose driver error code is zero (the card answered
	 * and refused: a not-found OPEN). Not a driver fault; the mount is kept. */
	uint32_t open_misses;
	/* A failed mount with no card inserted (cardDetected() false). Not a
	 * driver fault. */
	uint32_t absent_mounts;
	/* The driver's 1 s BUSY_TIMEOUT_MICROS expiries, mirrored after each
	 * primitive; nonzero in DMA mode is the DMA hang. */
	uint32_t busy_timeouts;
	/* Mount mode: true is DMA SDIO, false FIFO. Set by
	 * aius_sd_teensy_set_mode; the next real mount uses it. Init clears it to
	 * false; the boot value is AIUS_SD_BOOT_DMA, set right after init. */
	bool dma;
	/* Pin 46 (DAT3) is a pulled-down GPIO rather than USDHC-muxed, so
	 * SD.cardDetected() reads the card. Set by a failed begin (the vendored
	 * SD.cpp re-muxes it) and by aius_sd_teensy_invalidate_mount()'s
	 * re-mux; cleared by a successful begin; never touched by a mode change
	 * (Phase 4h NS3, E3). While set, a card read through SdFat's volume
	 * runs behind an un-muxed DAT3, so direct card ops refuse. */
	bool dat3_remuxed;
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
 * explicitly rather than leaving the adapter to infer it. It starts no card
 * transaction and cannot fail. A card-gone signal: with no file open and DAT3
 * not already re-muxed, it re-muxes pin 46 to INPUT_PULLDOWN and waits 5 us,
 * so aius_sd_teensy_card_detected() can read the card (Phase 4h NS3). */
void aius_sd_teensy_invalidate_mount(AiusSdTeensy *adapter);

/* Whether a card is in the slot, with no card command: SD.cardDetected(), the
 * DAT3 level, read only while the adapter holds no mount and DAT3 is re-muxed
 * (dat3_remuxed); otherwise, a boot's first probe included, true, so BEGIN
 * runs as before (D3). A false read counts absent_mounts (S1, N7). The
 * playback remount retry and the probe start a BEGIN only when this is true.
 * Null answers false. */
bool aius_sd_teensy_card_detected(AiusSdTeensy *adapter);

/* Selects the mount mode. A change invalidates the mount (the next BEGIN
 * remounts in the new mode) only when no file is open; with one open only the
 * mode is recorded. Memory only; starts no card transaction, and never re-muxes
 * DAT3: the card and SdFat's volume stay usable until that BEGIN (E3). */
void aius_sd_teensy_set_mode(AiusSdTeensy *adapter, bool dma);

AiusSdEvent aius_sd_teensy_execute(
	AiusSdTeensy *adapter,
	const AiusSdAction *action);

#endif
