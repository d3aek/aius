#include "aius_ota_staging.h"

#include <string.h>

/* FLASHMEM like the rest of the cold OTA core: placement is resolved before
 * the dark window, never inside it. On the host the macro expands to
 * nothing and the identical object is what the ota suite proves. */
#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

FLASHMEM bool aius_ota_staging_place(uint32_t mapped_psram_bytes,
	                             uint32_t pad2_only_bytes,
	                             uint32_t recorder_staging_bytes,
	                             AiusOtaStagingPlacement *placement)
{
	uint32_t pad1_bytes;

	if (placement == NULL)
		return false;
	memset(placement, 0, sizeof(*placement));

	if (mapped_psram_bytes != 0u) {
		/* Pad 1 is the mapped region's first pad; a 16 MiB fitment maps
		 * both pads but the placement deliberately stays on the first. */
		pad1_bytes = mapped_psram_bytes < AIUS_OTA_STAGING_PAD_BYTES
			? mapped_psram_bytes
			: AIUS_OTA_STAGING_PAD_BYTES;
		if (recorder_staging_bytes >= pad1_bytes)
			return false;
		placement->offset = recorder_staging_bytes;
		placement->capacity = pad1_bytes - recorder_staging_bytes;
		placement->on_pad2 = false;
		return true;
	}
	if (pad2_only_bytes != 0u) {
		/* The bench fitment: one chip on the silkscreened-FLASH pad.
		 * There is no known-good alternative, so the whole chip is
		 * staging and the flag says so. */
		placement->offset = AIUS_OTA_STAGING_PAD2_OFFSET;
		placement->capacity = pad2_only_bytes;
		placement->on_pad2 = true;
		return true;
	}
	return false;
}
