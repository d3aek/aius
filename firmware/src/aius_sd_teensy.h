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
} AiusSdTeensy;

void aius_sd_teensy_init(AiusSdTeensy *adapter);

AiusSdEvent aius_sd_teensy_execute(
	AiusSdTeensy *adapter,
	const AiusSdAction *action);

#endif
