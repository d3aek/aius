#include "aius_ota_dark_stamp.h"

#include <string.h>

/* Placement mirrors aius_ota_activate.c: encode is FASTRUN because it runs
 * inside the dark window (per step and at the FAULTED halt), decode is
 * FLASHMEM because it runs once at boot with flash intact. On the host both
 * expand to nothing and the identical object is what the ota suite proves. */
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

_Static_assert(AIUS_OTA_DARK_STAMP_OFFSET_CRC32 + 4u ==
	               AIUS_OTA_DARK_STAMP_BYTES,
               "the stamp CRC is the last field");
_Static_assert(AIUS_OTA_DARK_STAMP_OFFSET_RESERVED + 4u ==
	               AIUS_OTA_DARK_STAMP_OFFSET_CRC32,
               "the reserved word runs up to the CRC");

/* Bitwise CRC-32 (IEEE 802.3 reflected, the convention every aius record
 * uses), deliberately TABLE-FREE: a 1 KiB lookup table would be .rodata, and
 * this function must execute while flash -- where a table could land -- is
 * mid-erase. 60 bytes x 8 bits is ~500 cycles, noise against one erase. */
static FASTRUN uint32_t crc32_bitwise(const uint8_t *bytes, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t index;
	unsigned bit;

	for (index = 0u; index < length; index++) {
		crc ^= bytes[index];
		for (bit = 0u; bit < 8u; bit++) {
			if (crc & 1u)
				crc = (crc >> 1) ^ UINT32_C(0xedb88320);
			else
				crc >>= 1;
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static FASTRUN void put_u16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value & 0xffu);
	bytes[1] = (uint8_t)(value >> 8);
}

static FASTRUN void put_u32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xffu);
	bytes[1] = (uint8_t)((value >> 8) & 0xffu);
	bytes[2] = (uint8_t)((value >> 16) & 0xffu);
	bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static FASTRUN void put_u64(uint8_t *bytes, uint64_t value)
{
	put_u32(bytes, (uint32_t)(value & UINT32_MAX));
	put_u32(bytes + 4, (uint32_t)(value >> 32));
}

static FLASHMEM uint16_t get_u16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static FLASHMEM uint32_t get_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static FLASHMEM uint64_t get_u64(const uint8_t *bytes)
{
	return (uint64_t)get_u32(bytes) | ((uint64_t)get_u32(bytes + 4) << 32);
}

FASTRUN bool aius_ota_dark_stamp_encode(const AiusOtaDarkStampView *view,
	                                uint8_t *bytes, size_t length)
{
	size_t index;

	if (view == NULL || bytes == NULL ||
	    length != AIUS_OTA_DARK_STAMP_BYTES)
		return false;

	/* No memset: this runs in-window and the layout covers every byte, so
	 * each field is written explicitly and the reserved word by hand. */
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_MAGIC],
	        AIUS_OTA_DARK_STAMP_MAGIC);
	put_u16(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_VERSION],
	        AIUS_OTA_DARK_STAMP_VERSION);
	put_u16(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_LENGTH],
	        AIUS_OTA_DARK_STAMP_BYTES);
	bytes[AIUS_OTA_DARK_STAMP_OFFSET_STATE] = view->state;
	bytes[AIUS_OTA_DARK_STAMP_OFFSET_PHASE] = view->phase;
	bytes[AIUS_OTA_DARK_STAMP_OFFSET_FAULT] = view->fault;
	bytes[AIUS_OTA_DARK_STAMP_OFFSET_FLAGS] = view->flags;
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_PRIMITIVE_FAULT],
	        (uint32_t)view->primitive_fault);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_STEP_ADDRESS],
	        view->step_address);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_COMPARE_OFFSET],
	        view->compare_offset);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_PROGRAMMED],
	        view->programmed_bytes);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_VERIFIED],
	        view->verified_bytes);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_STEPS_TAKEN],
	        view->steps_taken);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_STEPS_TOTAL],
	        view->steps_total);
	for (index = 0u; index < AIUS_OTA_DARK_STAMP_DIGEST_PREFIX_BYTES;
	     index++)
		bytes[AIUS_OTA_DARK_STAMP_OFFSET_DIGEST_PREFIX + index] =
			view->digest_prefix[index];
	put_u64(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_DARK_CYCLES],
	        view->dark_window_cycles);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_RESERVED], 0u);
	put_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_CRC32],
	        crc32_bitwise(bytes, AIUS_OTA_DARK_STAMP_OFFSET_CRC32));
	return true;
}

FLASHMEM bool aius_ota_dark_stamp_decode(AiusOtaDarkStampView *view,
	                                 const uint8_t *bytes, size_t length)
{
	uint8_t state;

	if (view == NULL)
		return false;
	memset(view, 0, sizeof(*view));
	view->compare_offset = AIUS_OTA_DARK_STAMP_NO_COMPARE_OFFSET;

	if (bytes == NULL || length != AIUS_OTA_DARK_STAMP_BYTES)
		return false;
	if (get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_MAGIC]) !=
	    AIUS_OTA_DARK_STAMP_MAGIC)
		return false;
	if (get_u16(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_VERSION]) !=
	    AIUS_OTA_DARK_STAMP_VERSION)
		return false;
	if (get_u16(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_LENGTH]) !=
	    AIUS_OTA_DARK_STAMP_BYTES)
		return false;
	if (get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_CRC32]) !=
	    crc32_bitwise(bytes, AIUS_OTA_DARK_STAMP_OFFSET_CRC32))
		return false;
	state = bytes[AIUS_OTA_DARK_STAMP_OFFSET_STATE];
	if (state < (uint8_t)AIUS_OTA_DARK_STAMP_ENTERED ||
	    state > (uint8_t)AIUS_OTA_DARK_STAMP_COMPLETED)
		return false;

	view->state = state;
	view->phase = bytes[AIUS_OTA_DARK_STAMP_OFFSET_PHASE];
	view->fault = bytes[AIUS_OTA_DARK_STAMP_OFFSET_FAULT];
	view->flags = bytes[AIUS_OTA_DARK_STAMP_OFFSET_FLAGS];
	view->primitive_fault = (int32_t)get_u32(
		&bytes[AIUS_OTA_DARK_STAMP_OFFSET_PRIMITIVE_FAULT]);
	view->step_address =
		get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_STEP_ADDRESS]);
	view->compare_offset =
		get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_COMPARE_OFFSET]);
	view->programmed_bytes =
		get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_PROGRAMMED]);
	view->verified_bytes =
		get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_VERIFIED]);
	view->steps_taken =
		get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_STEPS_TAKEN]);
	view->steps_total =
		get_u32(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_STEPS_TOTAL]);
	memcpy(view->digest_prefix,
	       &bytes[AIUS_OTA_DARK_STAMP_OFFSET_DIGEST_PREFIX],
	       AIUS_OTA_DARK_STAMP_DIGEST_PREFIX_BYTES);
	view->dark_window_cycles =
		get_u64(&bytes[AIUS_OTA_DARK_STAMP_OFFSET_DARK_CYCLES]);
	return true;
}
