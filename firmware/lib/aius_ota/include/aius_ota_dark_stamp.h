#ifndef AIUS_OTA_DARK_STAMP_H
#define AIUS_OTA_DARK_STAMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The dark-window forensic stamp: the byte layout and transition vocabulary of
 * the retained-RAM record the activation sequencer writes across the flash
 * window, so a board that dies inside the window names its phase, step address,
 * compare offset and fault code to the next boot that can speak.
 *
 * Scoped after the 2026-08-16 dark-window brick (aius-ota-resilience.md
 * Phase 1): the halt policy converts an in-window fault into a WDOG reset with
 * nothing recorded, and the health record's in-progress mark -- a plain .bss
 * buffer that slice -- could not survive to be read. This record lives in the
 * ldscript's `.aius_retained` window (survives a watchdog or software reset;
 * OCRAM is not touched by the Teensy 4 startup), so it survives exactly the
 * reset path the halt policy takes, and a HalfKay reflash performed without a
 * power cycle still finds it.
 *
 * PURE LOGIC, like the rest of lib/aius_ota: this unit owns the layout, the
 * CRC and the fail-closed decode. WHERE the bytes live (the retained section,
 * the dcache flush) is the composition's; WHEN a stamp is taken is the
 * sequencer's (aius_ota_activate.h, the `dark_stamp` primitive).
 *
 * WHY A BYTE LAYOUT WITH ITS OWN CRC, like the staging record: the stamp is
 * written by one image and read by ANOTHER -- the whole point is that the
 * image that stamps a fault is not the image that reports it (a HalfKay
 * reflash sits between). Two images differ in layout by construction, so the
 * record is offsets-and-CRC, never a shared struct, and a decode that fails
 * any rule reports NO stamp rather than someone else's bytes.
 *
 *    0   4  magic "AIOD"
 *    4   2  version = 1
 *    6   2  record_length = 64
 *    8   1  state
 *    9   1  phase          (AiusOtaActivatePhase, as a byte)
 *   10   1  fault          (AiusOtaActivateFault, as a byte)
 *   11   1  flags
 *   12   4  primitive_fault (int32, two's complement, little-endian)
 *   16   4  step_address
 *   20   4  compare_offset
 *   24   4  programmed_bytes
 *   28   4  verified_bytes
 *   32   4  steps_taken
 *   36   4  steps_total
 *   40   8  image digest prefix (first 8 bytes of the payload SHA-256)
 *   48   8  dark_window_cycles
 *   56   4  reserved = 0
 *   60   4  CRC-32 over bytes [0, 60)
 */
#define AIUS_OTA_DARK_STAMP_MAGIC UINT32_C(0x444f4941) /* "AIOD" */
#define AIUS_OTA_DARK_STAMP_VERSION 1u
#define AIUS_OTA_DARK_STAMP_BYTES 64u
#define AIUS_OTA_DARK_STAMP_DIGEST_PREFIX_BYTES 8u

#define AIUS_OTA_DARK_STAMP_OFFSET_MAGIC 0u
#define AIUS_OTA_DARK_STAMP_OFFSET_VERSION 4u
#define AIUS_OTA_DARK_STAMP_OFFSET_LENGTH 6u
#define AIUS_OTA_DARK_STAMP_OFFSET_STATE 8u
#define AIUS_OTA_DARK_STAMP_OFFSET_PHASE 9u
#define AIUS_OTA_DARK_STAMP_OFFSET_FAULT 10u
#define AIUS_OTA_DARK_STAMP_OFFSET_FLAGS 11u
#define AIUS_OTA_DARK_STAMP_OFFSET_PRIMITIVE_FAULT 12u
#define AIUS_OTA_DARK_STAMP_OFFSET_STEP_ADDRESS 16u
#define AIUS_OTA_DARK_STAMP_OFFSET_COMPARE_OFFSET 20u
#define AIUS_OTA_DARK_STAMP_OFFSET_PROGRAMMED 24u
#define AIUS_OTA_DARK_STAMP_OFFSET_VERIFIED 28u
#define AIUS_OTA_DARK_STAMP_OFFSET_STEPS_TAKEN 32u
#define AIUS_OTA_DARK_STAMP_OFFSET_STEPS_TOTAL 36u
#define AIUS_OTA_DARK_STAMP_OFFSET_DIGEST_PREFIX 40u
#define AIUS_OTA_DARK_STAMP_OFFSET_DARK_CYCLES 48u
#define AIUS_OTA_DARK_STAMP_OFFSET_RESERVED 56u
#define AIUS_OTA_DARK_STAMP_OFFSET_CRC32 60u

/* The stamp's whole life, in write order. Every state names what is TRUE at
 * the moment of the write, so a reader who finds one after a reset needs no
 * further context:
 *
 *   ENTERED    begin() passed the whole pre-window verification and wrote the
 *              health mark; the first erase has not happened. A board that
 *              resets showing ENTERED died between the mark and the first
 *              step -- or wedged inside the first step, since the per-step
 *              stamp below is written BEFORE each operation runs.
 *   STEP       the sequencer is about to run the operation `step_address`
 *              names. A board that resets showing STEP wedged INSIDE that
 *              operation -- the erase, program or readback that never
 *              returned. This is the state the 2026-08-16 open question
 *              ("what did the window's program/compare path do") needed.
 *   FAULTED    the sequencer halted honestly: `fault`, `primitive_fault`,
 *              `step_address` and (for a compare fault) `compare_offset` name
 *              the verdict. Written before the WDOG-halt, so the reset that
 *              follows carries it.
 *   COMPLETED  the sequence reached READY_TO_RESET; `dark_window_cycles` is
 *              the bring-up evidence. The reset that follows is deliberate.
 *
 * NONE is never written; it is the decode's spelling of "no stamp". */
typedef enum {
	AIUS_OTA_DARK_STAMP_NONE = 0,
	AIUS_OTA_DARK_STAMP_ENTERED = 1,
	AIUS_OTA_DARK_STAMP_STEP = 2,
	AIUS_OTA_DARK_STAMP_FAULTED = 3,
	AIUS_OTA_DARK_STAMP_COMPLETED = 4
} AiusOtaDarkStampState;

/* Flag bits. REHEARSAL marks a stamp taken against a scratch window: the same
 * sequence, the same stamping path, but the boot region was never touched --
 * a reader must not mistake a rehearsal's injected fault for a field brick.
 * Set by the composition's rehearsal stamp writer, never by the sequencer. */
#define AIUS_OTA_DARK_STAMP_FLAG_REHEARSAL 0x01u

/* No compare fault: `compare_offset` carries this when the stamp's fault is
 * anything but a readback mismatch. UINT32_MAX rather than zero because zero
 * is a real image offset -- the first byte of the FCB. */
#define AIUS_OTA_DARK_STAMP_NO_COMPARE_OFFSET UINT32_MAX

/* The in-memory form the sequencer fills and the serializers read. Plain
 * integers only -- the enum fields travel as bytes so this header depends on
 * nothing else in the library. */
typedef struct {
	uint8_t state;
	uint8_t phase;
	uint8_t fault;
	uint8_t flags;
	int32_t primitive_fault;
	uint32_t step_address;
	uint32_t compare_offset;
	uint32_t programmed_bytes;
	uint32_t verified_bytes;
	uint32_t steps_taken;
	uint32_t steps_total;
	uint8_t digest_prefix[AIUS_OTA_DARK_STAMP_DIGEST_PREFIX_BYTES];
	uint64_t dark_window_cycles;
} AiusOtaDarkStampView;

/* Serializes `view` into exactly AIUS_OTA_DARK_STAMP_BYTES bytes and fills the
 * CRC. RAM-RESIDENT ON THE TARGET (FASTRUN, bitwise CRC, no tables): it is
 * called per step and at the FAULTED halt, both inside the dark window where
 * flash is unreadable. False only on null arguments or a wrong length. */
bool aius_ota_dark_stamp_encode(const AiusOtaDarkStampView *view,
	                        uint8_t *bytes, size_t length);

/* Fail-closed decode: magic, version, record_length, state domain and CRC all
 * checked; any violation returns false and leaves `view` zeroed (state NONE),
 * which is the honest state of a board with no stamp -- a cold power cycle,
 * a fresh board, or another image's layout. */
bool aius_ota_dark_stamp_decode(AiusOtaDarkStampView *view,
	                        const uint8_t *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
