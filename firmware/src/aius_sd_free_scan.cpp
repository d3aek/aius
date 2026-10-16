#include "aius_sd_free_scan.h"

#include "FsLib/FsLib.h"

#include "aius_sd.h"

/* Cold code -> FLASH, on each definition (see aius_sd_teensy.cpp for why). */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace {

constexpr uint32_t kSectorBytes = 512u;
constexpr uint32_t kFat16EntriesPerSector = 256u;
constexpr uint32_t kFat32EntriesPerSector = 128u;
constexpr uint32_t kBitmapBitsPerSector = 4096u;

struct Count {
	/* FAT: entries still to count. exFAT: bitmap bytes still to count. */
	uint32_t left;
	uint32_t found;
};

/* Upstream's own per-sector counters (FatPartition.cpp freeClusterCount_cb_*),
 * bounded by the entries left so the chunked total equals the one-shot walk. */
FLASHMEM void count_fat16(uint32_t, uint8_t *buf, void *context)
{
	Count *count = static_cast<Count *>(context);
	const uint16_t *p = reinterpret_cast<const uint16_t *>(buf);
	uint32_t n = count->left;
	if (n > kFat16EntriesPerSector)
		n = kFat16EntriesPerSector;
	for (uint32_t i = 0; i < n; ++i) {
		if (p[i] == 0u)
			count->found++;
	}
	count->left -= n;
}

FLASHMEM void count_fat32(uint32_t, uint8_t *buf, void *context)
{
	Count *count = static_cast<Count *>(context);
	const uint32_t *p = reinterpret_cast<const uint32_t *>(buf);
	uint32_t n = count->left;
	if (n > kFat32EntriesPerSector)
		n = kFat32EntriesPerSector;
	for (uint32_t i = 0; i < n; ++i) {
		if (p[i] == 0u)
			count->found++;
	}
	count->left -= n;
}

/* exFAT: found counts USED bits, whole bytes up to ceil(clusterCount / 8) as
 * ExFatPartition::freeClusterCount() does. */
FLASHMEM void count_bitmap(uint32_t, uint8_t *buf, void *context)
{
	Count *count = static_cast<Count *>(context);
	uint32_t n = count->left;
	if (n > kSectorBytes)
		n = kSectorBytes;
	for (uint32_t i = 0; i < n; ++i) {
		uint8_t byte = buf[i];
		while (byte != 0u) {
			count->found += byte & 1u;
			byte = static_cast<uint8_t>(byte >> 1);
		}
	}
	count->left -= n;
}

FLASHMEM AiusSdFreeScanResult make_result(AiusSdFreeScanStatus status)
{
	AiusSdFreeScanResult result;
	result.status = status;
	result.chunk_free = 0u;
	result.used_bytes = 0u;
	return result;
}

/* SD.usedSize()'s figure: (clusterCount - freeClusterCount) * bytesPerCluster,
 * the subtraction in uint32_t as upstream's is. */
FLASHMEM AiusSdFreeScanResult done_result(FsVolume *volume, uint64_t free_total)
{
	/* More free clusters than the volume has is a corrupt FAT (reserved entries
	 * 0 and 1 reading free): used would exceed TOTAL_SIZE, which the one-shot
	 * scan also faulted as a protocol fault. Never a silent 0. */
	if (free_total > volume->clusterCount())
		return make_result(AIUS_SD_FREE_SCAN_PROTOCOL);
	AiusSdFreeScanResult result = make_result(AIUS_SD_FREE_SCAN_DONE);
	const uint32_t used_clusters =
		volume->clusterCount() - static_cast<uint32_t>(free_total);
	result.used_bytes = static_cast<uint64_t>(used_clusters) *
	                    static_cast<uint64_t>(volume->bytesPerCluster());
	return result;
}

}

FLASHMEM uint32_t aius_sd_free_scan_chunk_bound(uint8_t fat_type)
{
	switch (fat_type) {
	case FAT_TYPE_FAT16:
		return AIUS_SD_USAGE_CHUNK_SECTORS * kFat16EntriesPerSector;
	case FAT_TYPE_FAT32:
		return AIUS_SD_USAGE_CHUNK_SECTORS * kFat32EntriesPerSector;
	case FAT_TYPE_EXFAT:
		return AIUS_SD_USAGE_CHUNK_SECTORS * kBitmapBitsPerSector;
	default:
		return 0u;
	}
}

FLASHMEM bool aius_sd_free_scan_chunk_ok(uint8_t fat_type, uint64_t chunk_free)
{
	return chunk_free <= aius_sd_free_scan_chunk_bound(fat_type);
}

FLASHMEM AiusSdFreeScanResult aius_sd_free_scan_step(
	FsVolume *volume,
	uint64_t cursor,
	uint64_t free_so_far,
	bool mounted)
{
	if (volume == nullptr)
		return make_result(AIUS_SD_FREE_SCAN_DRIVER);
	FsBlockDevice *device = volume->aiusBlockDevice();
	if (device == nullptr)
		return make_result(AIUS_SD_FREE_SCAN_DRIVER);
	const uint8_t fat_type = volume->fatType();

	if (fat_type == 12u) {
		/* At most 12 FAT sectors through m_fatCache: one upstream call. */
		if (cursor != 0u)
			return make_result(AIUS_SD_FREE_SCAN_PROTOCOL);
		/* freeClusterCount() is uint32_t over upstream's int32_t: its -1 read
		 * failure arrives as 0xFFFFFFFF, which no real count reaches. */
		const uint32_t free12 = volume->freeClusterCount();
		if (free12 == UINT32_MAX)
			return make_result(AIUS_SD_FREE_SCAN_DRIVER);
		return done_result(volume, free12);
	}

	uint32_t first_sector;
	uint32_t total_sectors;
	void (*counter)(uint32_t, uint8_t *, void *);
	Count count;
	count.found = 0u;
	if (fat_type == FAT_TYPE_FAT16 || fat_type == FAT_TYPE_FAT32) {
		const bool fat32 = fat_type == FAT_TYPE_FAT32;
		const uint32_t per_sector =
			fat32 ? kFat32EntriesPerSector : kFat16EntriesPerSector;
		/* Upstream counts entries 0 .. lastCluster: clusterCount() + 2. */
		const uint64_t entries = static_cast<uint64_t>(volume->clusterCount()) + 2u;
		total_sectors =
			static_cast<uint32_t>((entries + per_sector - 1u) / per_sector);
		first_sector = volume->fatStartSector();
		counter = fat32 ? count_fat32 : count_fat16;
		if (cursor >= total_sectors)
			return make_result(AIUS_SD_FREE_SCAN_PROTOCOL);
		if (cursor == 0u) {
			/* Also syncs the FAT cache, so the raw reads below see any dirty
			 * FAT sector. A known count answers with no FAT reads. */
			const int32_t known = volume->aiusFreeClusters(-1);
			if (known == -2)
				return make_result(AIUS_SD_FREE_SCAN_DRIVER);
			if (known >= 0)
				return done_result(volume, static_cast<uint32_t>(known));
		}
		const uint64_t done_entries = cursor * per_sector;
		count.left = static_cast<uint32_t>(entries - done_entries);
	} else if (fat_type == FAT_TYPE_EXFAT) {
		/* The heap start, where upstream's own scan begins. */
		const uint64_t bytes =
			(static_cast<uint64_t>(volume->clusterCount()) + 7u) / 8u;
		total_sectors =
			static_cast<uint32_t>((bytes + kSectorBytes - 1u) / kSectorBytes);
		first_sector = volume->dataStartSector();
		counter = count_bitmap;
		if (cursor >= total_sectors)
			return make_result(AIUS_SD_FREE_SCAN_PROTOCOL);
		count.left = static_cast<uint32_t>(bytes - cursor * kSectorBytes);
	} else {
		return make_result(AIUS_SD_FREE_SCAN_DRIVER);
	}

	uint32_t sectors = total_sectors - static_cast<uint32_t>(cursor);
	if (sectors > AIUS_SD_USAGE_CHUNK_SECTORS)
		sectors = AIUS_SD_USAGE_CHUNK_SECTORS;
	const bool final_chunk = cursor + sectors == total_sectors;

	uint8_t *buffer = volume->cacheClear();
	if (buffer == nullptr)
		return make_result(AIUS_SD_FREE_SCAN_DRIVER);
	if (!device->readSectorsCallback(
		    first_sector + static_cast<uint32_t>(cursor), buffer, sectors,
		    counter, &count))
		return make_result(AIUS_SD_FREE_SCAN_DRIVER);

	uint64_t chunk_free = count.found;
	if (fat_type == FAT_TYPE_EXFAT) {
		/* found counted used bits; the chunk covers min(sectors x 4,096,
		 * clusters left) bitmap bits. */
		const uint64_t bits_before = cursor * kBitmapBitsPerSector;
		uint64_t bits = static_cast<uint64_t>(sectors) * kBitmapBitsPerSector;
		const uint64_t remaining = volume->clusterCount() - bits_before;
		if (bits > remaining)
			bits = remaining;
		/* More used bits than the chunk has clusters: padding bits past
		 * clusterCount are set, a corrupt bitmap. */
		if (count.found > bits)
			return make_result(AIUS_SD_FREE_SCAN_PROTOCOL);
		chunk_free = bits - count.found;
	}
	if (!aius_sd_free_scan_chunk_ok(fat_type, chunk_free))
		return make_result(AIUS_SD_FREE_SCAN_PROTOCOL);

	const uint64_t free_total = free_so_far + chunk_free;
	if (!final_chunk) {
		AiusSdFreeScanResult result = make_result(AIUS_SD_FREE_SCAN_MORE);
		result.chunk_free = static_cast<uint32_t>(chunk_free);
		return result;
	}
	if (free_total > volume->clusterCount())
		return make_result(AIUS_SD_FREE_SCAN_PROTOCOL);
	if (fat_type != FAT_TYPE_EXFAT && mounted) {
		/* Later refreshes on this mount answer with no FAT reads. -2 is the
		 * FAT cache sync failing; -1 (no maintained count) means not stored. */
		if (volume->aiusFreeClusters(static_cast<int32_t>(free_total)) == -2)
			return make_result(AIUS_SD_FREE_SCAN_DRIVER);
	}
	return done_result(volume, free_total);
}
