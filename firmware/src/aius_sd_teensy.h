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
	uint32_t driver_faults;
	uint8_t last_protocol_action;
} AiusSdTeensy;

void aius_sd_teensy_init(AiusSdTeensy *adapter);

AiusSdEvent aius_sd_teensy_execute(
	AiusSdTeensy *adapter,
	const AiusSdAction *action);

#endif
