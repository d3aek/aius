#ifndef AIUS_SD_FREE_SCAN_H
#define AIUS_SD_FREE_SCAN_H

#ifndef __cplusplus
#error "aius_sd_free_scan.h requires C++"
#endif

#include <stdint.h>

/* One bounded step of the USAGE_REFRESH used-size scan (Phase 4zd). The
 * portable job drives it: every USED_SIZE primitive scans at most
 * AIUS_SD_USAGE_CHUNK_SECTORS FAT (or exFAT bitmap) sectors, so no epoch spends
 * the whole FAT walk SD.usedSize() did (4.04 s in DMA on a 4 GB FAT32 card).
 *
 * The step is stateless: the cursor (a sector index into the FAT or bitmap) and
 * the free clusters counted so far travel in the action (offset / length), and
 * the job keeps them in job->transferred / job->captured_size. Opaque pointers
 * only, so aius_sd_teensy.cpp compiles against the sd-target fake SD.h as well
 * as the vendored SdFat. */
class FsVolume;

enum AiusSdFreeScanStatus {
	/* A non-final chunk: chunk_free is its free clusters (0 is legal). */
	AIUS_SD_FREE_SCAN_MORE = 0,
	/* The final chunk (or a known count at cursor 0): used_bytes is the answer. */
	AIUS_SD_FREE_SCAN_DONE,
	/* The request is malformed: a cursor beyond the scan, a cursor on FAT12 that
	 * is not 0, or a chunk over its type's entry count. */
	AIUS_SD_FREE_SCAN_PROTOCOL,
	/* The card or volume failed: no volume, a failed read, or a failed FAT cache
	 * sync. The adapter answers a hard driver fault. */
	AIUS_SD_FREE_SCAN_DRIVER
};

struct AiusSdFreeScanResult {
	AiusSdFreeScanStatus status;
	/* MORE: the chunk's free clusters, at most the type's entry count. */
	uint32_t chunk_free;
	/* DONE: (clusterCount - free) * bytesPerCluster, as SD.usedSize() computes. */
	uint64_t used_bytes;
};

/* The entry count one chunk of `fat_type` may answer (FsVolume::fatType()
 * numbering: 16, 32, 64 for exFAT): 8,192 FAT16, 4,096 FAT32, 131,072 exFAT.
 * 0 for FAT12 and any unknown type (neither is chunked). */
uint32_t aius_sd_free_scan_chunk_bound(uint8_t fat_type);

/* True when `chunk_free` fits the type's bound; the step refuses a chunk that
 * does not as a protocol fault. */
bool aius_sd_free_scan_chunk_ok(uint8_t fat_type, uint64_t chunk_free);

/* Scan one chunk starting at sector `cursor`, with `free_so_far` free clusters
 * already counted. `mounted` is the adapter's mount flag at this step: the
 * finished FAT count is stored in SdFat's maintained free count only if it
 * still holds at the final chunk (a dropped mount skips the store). A known
 * count at cursor 0 answers DONE with no FAT reads. */
AiusSdFreeScanResult aius_sd_free_scan_step(
	FsVolume *volume,
	uint64_t cursor,
	uint64_t free_so_far,
	bool mounted);

#endif
