/* Phase 4zd: the bounded, resumable used-size scan (src/aius_sd_free_scan.cpp)
 * against the REAL vendored SdFat (FatLib / ExFatLib / FsLib), mounted through
 * FsVolume::begin over a sparse RAM block device. FAT12, FAT16, FAT32 and exFAT
 * images carry free entries on both sides of every 32-sector chunk boundary and
 * in the final partial sector. Reference figures come from an independent hand
 * count of the generated pattern and from upstream freeClusterCount(). */

#include "FsLib/FsLib.h"
#include "aius_sd.h"
#include "aius_sd_free_scan.h"

#include <array>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

/* The step reads through the mounted volume's own device; the calls below still
 * name the RAM device for readability, and this drops that argument. */
#define aius_sd_free_scan_step(volume, device, ...) \
	aius_sd_free_scan_step(volume, __VA_ARGS__)

/* SdFat's inline Serial members name it; nothing here writes to it. */
namespace {
class NullStream : public Stream {
public:
	size_t write(uint8_t) override { return 1; }
	int available() override { return 0; }
	int read() override { return -1; }
	int peek() override { return -1; }
};
NullStream g_null_stream;
}
Stream &Serial = g_null_stream;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
			        __LINE__, #cond);                            \
			abort();                                             \
		}                                                            \
	} while (0)

namespace {

constexpr uint32_t kChunk = AIUS_SD_USAGE_CHUNK_SECTORS;

class RamDevice : public FsBlockDeviceInterface {
public:
	std::map<uint32_t, std::array<uint8_t, 512>> sectors;
	uint32_t count = 1u << 22;
	unsigned reads = 0;
	unsigned writes = 0;
	bool fail_reads = false;

	std::array<uint8_t, 512> &at(uint32_t sector)
	{
		auto it = sectors.find(sector);
		if (it == sectors.end()) {
			std::array<uint8_t, 512> zero;
			zero.fill(0);
			it = sectors.emplace(sector, zero).first;
		}
		return it->second;
	}
	bool isBusy() override { return false; }
	bool readSector(uint32_t sector, uint8_t *dst) override
	{
		reads++;
		if (fail_reads)
			return false;
		memcpy(dst, at(sector).data(), 512);
		return true;
	}
	bool readSectors(uint32_t sector, uint8_t *dst, size_t ns) override
	{
		for (size_t i = 0; i < ns; ++i)
			if (!readSector(sector + i, dst + 512 * i))
				return false;
		return true;
	}
	uint32_t sectorCount() override { return count; }
	bool syncDevice() override { return true; }
	bool writeSector(uint32_t sector, const uint8_t *src) override
	{
		writes++;
		memcpy(at(sector).data(), src, 512);
		return true;
	}
	bool writeSectors(uint32_t sector, const uint8_t *src, size_t ns) override
	{
		for (size_t i = 0; i < ns; ++i)
			writeSector(sector + i, src + 512 * i);
		return true;
	}
};

/* Which entries (FAT: clusters; exFAT: bit numbers) are free. */
struct Pattern {
	uint32_t first;      /* first counted index: 2 for FAT, 0 for exFAT bits */
	uint32_t last;       /* last counted index, inclusive */
	uint32_t per_sector; /* entries (or bits) per sector */
	int all_used_chunk;  /* a chunk with no free entry (BYTES 0), or -1 */
	bool fat_root;       /* index 2 is the FAT32 root cluster: used */
};

bool is_free(const Pattern &p, uint32_t i)
{
	if (i < p.first || i > p.last)
		return false;
	if (p.fat_root && i == 2u)
		return false;
	const uint32_t sector = i / p.per_sector;
	if (static_cast<int>(sector / kChunk) == p.all_used_chunk)
		return false;
	/* The final partial sector: the last two counted entries are free. */
	if (i == p.last || i == p.last - 1u)
		return true;
	/* Both sides of every chunk boundary. */
	const uint32_t r = i % p.per_sector;
	if (sector % kChunk == 0u && r == 0u)
		return true;
	if ((sector + 1u) % kChunk == 0u && r == p.per_sector - 1u)
		return true;
	return ((i * 2654435761u) >> 9) % 3u == 0u;
}

uint32_t count_free(const Pattern &p, uint32_t sector_lo, uint32_t sector_hi)
{
	uint32_t n = 0;
	for (uint32_t i = sector_lo * p.per_sector;
	     i < sector_hi * p.per_sector && i <= p.last; ++i)
		n += is_free(p, i);
	return n;
}

void le16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }

void put_sector(RamDevice &dev, uint32_t sector, const std::vector<uint8_t> &bytes,
                size_t offset)
{
	memcpy(dev.at(sector).data(), bytes.data() + offset, 512);
}

struct Image {
	RamDevice dev;
	Pattern pattern;
	uint32_t clusters = 0;
	uint32_t total_sectors = 0; /* FAT or bitmap sectors the scan covers */
	uint32_t bytes_per_cluster = 512;
	uint8_t type = 0;
	uint32_t expected_free = 0;
};

void write_boot(Image &img, uint8_t type, uint32_t spc, uint32_t reserved,
                uint32_t fats, uint32_t fat_sectors, uint32_t root_entries,
                uint32_t data_start)
{
	std::vector<uint8_t> boot(512, 0);
	boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
	memcpy(&boot[3], "MSDOS5.0", 8);
	le16(&boot[11], 512);
	boot[13] = spc;
	le16(&boot[14], reserved);
	boot[16] = fats;
	le16(&boot[17], root_entries);
	boot[21] = 0xF8;
	const uint32_t total = data_start + img.clusters * spc;
	if (type == 32) {
		le16(&boot[19], 0);
		le32(&boot[32], total);
		le32(&boot[36], fat_sectors);
		le32(&boot[44], 2);
		boot[66] = 0x29;
		le32(&boot[67], 0x1234ABCD);
	} else {
		le16(&boot[19], total);
		le16(&boot[22], fat_sectors);
		boot[38] = 0x29;
		le32(&boot[39], 0x1234ABCD);
	}
	boot[510] = 0x55; boot[511] = 0xAA;
	put_sector(img.dev, 0, boot, 0);
}

void set_entry(std::vector<uint8_t> &fat, uint8_t type, uint32_t i, uint32_t v)
{
	if (type == 32) {
		le32(&fat[4 * i], v);
	} else if (type == 16) {
		le16(&fat[2 * i], v);
	} else {
		const uint32_t idx = i + i / 2;
		if (i & 1u) {
			fat[idx] = (fat[idx] & 0x0F) | ((v << 4) & 0xF0);
			fat[idx + 1] = v >> 4;
		} else {
			fat[idx] = v & 0xFF;
			fat[idx + 1] = (fat[idx + 1] & 0xF0) | ((v >> 8) & 0x0F);
		}
	}
}

/* A FAT image: type 12, 16 or 32 (the cluster count decides the type). */
void build_fat(Image &img, uint8_t type, uint32_t clusters, uint32_t spc,
               uint32_t fats, uint32_t fat_sectors, uint32_t root_entries,
               uint32_t reserved, int all_used_chunk)
{
	img.type = type;
	img.clusters = clusters;
	img.bytes_per_cluster = spc * 512;
	const uint32_t entries = clusters + 2;
	const uint32_t per_sector = type == 32 ? 128 : 256;
	img.total_sectors = (entries + per_sector - 1) / per_sector;
	const uint32_t root_sectors = (root_entries * 32 + 511) / 512;
	const uint32_t data_start = reserved + fats * fat_sectors + root_sectors;
	write_boot(img, type, spc, reserved, fats, fat_sectors, root_entries,
	           data_start);
	img.pattern = {2, clusters + 1, per_sector, all_used_chunk, type == 32};
	std::vector<uint8_t> fat(fat_sectors * 512, 0);
	const uint32_t eoc = type == 32 ? 0x0FFFFFFFu : type == 16 ? 0xFFFFu : 0xFFFu;
	set_entry(fat, type, 0, eoc & ~7u);
	set_entry(fat, type, 1, eoc);
	for (uint32_t i = 2; i <= clusters + 1; ++i) {
		if (is_free(img.pattern, i))
			img.expected_free++;
		else
			set_entry(fat, type, i, eoc);
	}
	for (uint32_t f = 0; f < fats; ++f)
		for (uint32_t s = 0; s < fat_sectors; ++s)
			put_sector(img.dev, reserved + f * fat_sectors + s, fat, s * 512);
}

/* exFAT: 4 KiB clusters, the allocation bitmap at the heap start (sector 64). */
void build_exfat(Image &img, uint32_t clusters)
{
	img.type = 64;
	img.clusters = clusters;
	img.bytes_per_cluster = 4096;
	const uint32_t bytes = (clusters + 7) / 8;
	img.total_sectors = (bytes + 511) / 512;
	std::vector<uint8_t> boot(512, 0);
	boot[0] = 0xEB; boot[1] = 0x76; boot[2] = 0x90;
	memcpy(&boot[3], "EXFAT   ", 8);
	le32(&boot[80], 24);   /* fatOffset */
	le32(&boot[84], 8);    /* fatLength */
	le32(&boot[88], 64);   /* clusterHeapOffset */
	le32(&boot[92], clusters);
	le32(&boot[96], 4);    /* rootDirectoryCluster */
	boot[108] = 9;         /* bytesPerSectorShift */
	boot[109] = 3;         /* sectorsPerClusterShift */
	boot[110] = 1;
	boot[510] = 0x55; boot[511] = 0xAA;
	put_sector(img.dev, 0, boot, 0);
	/* Root directory cluster 4: a one-cluster chain. */
	std::vector<uint8_t> fat(512, 0);
	le32(&fat[16], 0xFFFFFFFFu);
	put_sector(img.dev, 24, fat, 0);
	img.pattern = {0, clusters - 1, 4096, -1, false};
	std::vector<uint8_t> bitmap(img.total_sectors * 512, 0);
	for (uint32_t k = 0; k < clusters; ++k) {
		if (is_free(img.pattern, k))
			img.expected_free++;
		else
			bitmap[k / 8] |= 1u << (k % 8);
	}
	for (uint32_t s = 0; s < img.total_sectors; ++s)
		put_sector(img.dev, 64 + s, bitmap, s * 512);
}

struct Scan {
	std::vector<uint32_t> chunks;
	AiusSdFreeScanResult last;
	unsigned max_reads = 0;
	unsigned total_reads = 0;
	uint32_t steps = 0;
};

Scan run_scan(FsVolume &vol, RamDevice &dev, bool mounted)
{
	Scan scan;
	uint64_t cursor = 0;
	uint64_t so_far = 0;
	for (;;) {
		const unsigned before = dev.reads;
		scan.last = aius_sd_free_scan_step(&vol, &dev, cursor, so_far, mounted);
		const unsigned used = dev.reads - before;
		scan.steps++;
		scan.total_reads += used;
		if (used > scan.max_reads)
			scan.max_reads = used;
		CHECK(scan.steps < 100000);
		if (scan.last.status != AIUS_SD_FREE_SCAN_MORE)
			return scan;
		scan.chunks.push_back(scan.last.chunk_free);
		so_far += scan.last.chunk_free;
		cursor += kChunk;
	}
}

uint64_t used_bytes(const Image &img, uint32_t free)
{
	return static_cast<uint64_t>(img.clusters - free) * img.bytes_per_cluster;
}

void check_chunked(Image &img, FsVolume &vol, const char *label, bool store)
{
	const uint32_t expected_steps = (img.total_sectors + kChunk - 1) / kChunk;
	Scan scan = run_scan(vol, img.dev, true);
	printf("%s: type %u, %u clusters, %u sectors, %u steps, free %u\n", label,
	       img.type, img.clusters, img.total_sectors, scan.steps,
	       img.expected_free);
	CHECK(scan.last.status == AIUS_SD_FREE_SCAN_DONE);
	CHECK(scan.steps == expected_steps);
	CHECK(scan.max_reads <= kChunk);
	CHECK(scan.total_reads == img.total_sectors);
	CHECK(scan.last.used_bytes == used_bytes(img, img.expected_free));
	uint64_t sum = 0;
	for (size_t k = 0; k < scan.chunks.size(); ++k) {
		const uint32_t lo = k * kChunk;
		CHECK(scan.chunks[k] == count_free(img.pattern, lo, lo + kChunk));
		CHECK(aius_sd_free_scan_chunk_ok(img.type, scan.chunks[k]));
		sum += scan.chunks[k];
	}
	/* The final step's chunk is not in chunks[]: total it from the pattern. */
	const uint32_t last_lo = (expected_steps - 1) * kChunk;
	sum += count_free(img.pattern, last_lo, img.total_sectors);
	CHECK(sum == img.expected_free);
	if (img.type != 64 && img.type != 12) {
		const int32_t stored = vol.aiusFreeClusters(-1);
		CHECK(store ? stored == static_cast<int32_t>(img.expected_free)
		            : stored == -1);
	}
}

/* Upstream's one-shot figure on its own mount of the same bytes. */
void check_matches_upstream(Image &img)
{
	FsVolume ref;
	CHECK(ref.begin(&img.dev, false, static_cast<uint8_t>(0)));
	CHECK(ref.freeClusterCount() == img.expected_free);
}

void test_fat12()
{
	Image img;
	build_fat(img, 12, 4000, 1, 1, 12, 512, 1, -1);
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	CHECK(vol.fatType() == 12);
	check_matches_upstream(img);
	/* One upstream call: a single step, never MORE. */
	Scan scan = run_scan(vol, img.dev, true);
	CHECK(scan.steps == 1);
	CHECK(scan.last.status == AIUS_SD_FREE_SCAN_DONE);
	CHECK(scan.last.used_bytes == used_bytes(img, img.expected_free));
	CHECK(scan.total_reads <= 30);
	/* A nonzero cursor has no meaning on FAT12. */
	CHECK(aius_sd_free_scan_step(&vol, &img.dev, kChunk, 0, true).status ==
	      AIUS_SD_FREE_SCAN_PROTOCOL);
	printf("fat12: one call, %u reads\n", scan.total_reads);
}

void test_fat16()
{
	Image img;
	build_fat(img, 16, 20000, 1, 2, 79, 512, 1, -1);
	check_matches_upstream(img);
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	CHECK(vol.fatType() == 16);
	CHECK(img.total_sectors == 79);
	check_chunked(img, vol, "fat16", true);

	/* Second refresh on the same mount: the stored count, 0 reads. */
	const unsigned before = img.dev.reads;
	AiusSdFreeScanResult again =
		aius_sd_free_scan_step(&vol, &img.dev, 0, 0, true);
	CHECK(again.status == AIUS_SD_FREE_SCAN_DONE);
	CHECK(again.used_bytes == used_bytes(img, img.expected_free));
	CHECK(img.dev.reads == before);

	/* A cursor past the scan is a protocol fault (the job does not check). */
	FsVolume fresh;
	CHECK(fresh.begin(&img.dev, false, static_cast<uint8_t>(0)));
	CHECK(aius_sd_free_scan_step(&fresh, &img.dev, 79, 0, true).status ==
	      AIUS_SD_FREE_SCAN_PROTOCOL);
	CHECK(aius_sd_free_scan_step(&fresh, &img.dev, 96, 0, true).status ==
	      AIUS_SD_FREE_SCAN_PROTOCOL);
	CHECK(aius_sd_free_scan_step(&fresh, &img.dev, 32, 0, true).status ==
	      AIUS_SD_FREE_SCAN_MORE);
	CHECK(aius_sd_free_scan_step(&fresh, &img.dev, 64, 0, true).status ==
	      AIUS_SD_FREE_SCAN_DONE);

	/* A failed read is a driver fault. */
	FsVolume broken;
	CHECK(broken.begin(&img.dev, false, static_cast<uint8_t>(0)));
	img.dev.fail_reads = true;
	CHECK(aius_sd_free_scan_step(&broken, &img.dev, 0, 0, true).status ==
	      AIUS_SD_FREE_SCAN_DRIVER);
	img.dev.fail_reads = false;
}

void test_fat32()
{
	Image img;
	build_fat(img, 32, 66000, 1, 2, 520, 0, 32, 3);
	check_matches_upstream(img);
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	CHECK(vol.fatType() == 32);
	CHECK(img.total_sectors == 516);
	/* Mount dropped mid-scan: nothing is stored. */
	{
		Scan dropped = run_scan(vol, img.dev, false);
		CHECK(dropped.last.status == AIUS_SD_FREE_SCAN_DONE);
		CHECK(dropped.last.used_bytes == used_bytes(img, img.expected_free));
		CHECK(vol.aiusFreeClusters(-1) == -1);
	}
	/* The chunk with every entry used answers BYTES 0 and the scan goes on. */
	check_chunked(img, vol, "fat32", true);
	{
		FsVolume fresh;
		CHECK(fresh.begin(&img.dev, false, static_cast<uint8_t>(0)));
		Scan scan = run_scan(fresh, img.dev, true);
		CHECK(scan.chunks.size() > 3 && scan.chunks[3] == 0);
		CHECK(scan.chunks[2] > 0 && scan.chunks[4] > 0);
	}
	/* The stored count answers the next refresh with 0 reads. */
	const unsigned before = img.dev.reads;
	CHECK(aius_sd_free_scan_step(&vol, &img.dev, 0, 0, true).status ==
	      AIUS_SD_FREE_SCAN_DONE);
	CHECK(img.dev.reads == before);
}

void test_exfat()
{
	Image img;
	build_exfat(img, 4096u * 40u + 100u);
	check_matches_upstream(img);
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	CHECK(vol.fatType() == 64);
	CHECK(img.total_sectors == 41);
	CHECK(vol.dataStartSector() == 64);
	check_chunked(img, vol, "exfat", false);
	/* exFAT keeps no count: the next refresh rescans every sector. */
	Scan again = run_scan(vol, img.dev, true);
	CHECK(again.total_reads == img.total_sectors);
	CHECK(again.last.used_bytes == used_bytes(img, img.expected_free));
	CHECK(aius_sd_free_scan_step(&vol, &img.dev, 41, 0, true).status ==
	      AIUS_SD_FREE_SCAN_PROTOCOL);
}

/* A dirty FAT cache sector is synced before the raw reads (USE_SEPARATE_FAT_CACHE
 * through the SdFatConfig.h guard): the cluster a pending write took is used. */
void test_dirty_fat_cache_is_synced()
{
	Image img;
	build_fat(img, 16, 20000, 1, 2, 79, 512, 1, -1);
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	FsFile file = vol.open("a.txt", O_RDWR | O_CREAT);
	CHECK(file);
	const uint8_t byte = 'x';
	CHECK(file.write(&byte, 1) == 1);
	/* The cluster the write took: the first free one after the search start. */
	uint32_t taken = 2;
	while (!is_free(img.pattern, taken))
		taken++;
	const uint32_t fat_sector = 1 + taken / 256;
	const size_t fat_offset = 2 * (taken % 256);
	CHECK(img.dev.at(fat_sector)[fat_offset] == 0 &&
	      img.dev.at(fat_sector)[fat_offset + 1] == 0);
	Scan scan = run_scan(vol, img.dev, true);
	/* The device holds the new FAT entry only because the scan synced it. */
	CHECK(img.dev.at(fat_sector)[fat_offset] != 0 ||
	      img.dev.at(fat_sector)[fat_offset + 1] != 0);
	CHECK(scan.last.status == AIUS_SD_FREE_SCAN_DONE);
	CHECK(scan.last.used_bytes == used_bytes(img, img.expected_free - 1));
	file.close();
}

/* Used never exceeds TOTAL_SIZE: a FAT whose entries all read free counts
 * clusterCount + 2 (reserved entries 0 and 1), a protocol fault and not stored,
 * never a silent 0. Mid-scan steps never store the count. */
void test_used_never_exceeds_total_and_no_mid_scan_store()
{
	Image img;
	build_fat(img, 16, 20000, 1, 2, 79, 512, 1, -1);
	std::vector<uint8_t> zeros(512, 0);
	for (uint32_t s = 0; s < 79; ++s)
		put_sector(img.dev, 1 + s, zeros, 0);
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	Scan scan = run_scan(vol, img.dev, true);
	CHECK(scan.last.status == AIUS_SD_FREE_SCAN_PROTOCOL);
	CHECK(vol.aiusFreeClusters(-1) == -1);

	/* FAT12: a read failure inside the one upstream call is a driver fault. */
	Image img12;
	build_fat(img12, 12, 4000, 1, 1, 12, 512, 1, -1);
	FsVolume vol12;
	CHECK(vol12.begin(&img12.dev, false, static_cast<uint8_t>(0)));
	img12.dev.fail_reads = true;
	CHECK(aius_sd_free_scan_step(&vol12, &img12.dev, 0, 0, true).status ==
	      AIUS_SD_FREE_SCAN_DRIVER);

	Image img2;
	build_fat(img2, 32, 66000, 1, 2, 520, 0, 32, 3);
	FsVolume vol2;
	CHECK(vol2.begin(&img2.dev, false, static_cast<uint8_t>(0)));
	CHECK(aius_sd_free_scan_step(&vol2, &img2.dev, 0, 0, true).status ==
	      AIUS_SD_FREE_SCAN_MORE);
	CHECK(aius_sd_free_scan_step(&vol2, &img2.dev, 32, 5, true).status ==
	      AIUS_SD_FREE_SCAN_MORE);
	CHECK(vol2.aiusFreeClusters(-1) == -1);
	/* A known count short-circuits at cursor 0 only. */
	CHECK(vol2.aiusFreeClusters(7) == 7);
	const unsigned before = img2.dev.reads;
	CHECK(aius_sd_free_scan_step(&vol2, &img2.dev, 0, 0, true).status ==
	      AIUS_SD_FREE_SCAN_DONE);
	CHECK(img2.dev.reads == before);
	CHECK(aius_sd_free_scan_step(&vol2, &img2.dev, 32, 0, true).status ==
	      AIUS_SD_FREE_SCAN_MORE);
	CHECK(img2.dev.reads == before + kChunk);
}

/* A corrupt exFAT bitmap (padding bits past clusterCount set) is a protocol
 * fault, not a silent clamp. */
void test_exfat_padding_bits_set_is_protocol()
{
	Image img;
	build_exfat(img, 4096u * 40u + 100u);
	/* Final chunk fully allocated, padding included: the last sector holds 100
	 * valid bits (byte 12 carries bits 96..103, 100..103 are padding), so the
	 * used-bit count exceeds the chunk's clusters. */
	for (uint32_t s = 32; s < 40; ++s)
		img.dev.at(64 + s).fill(0xFF);
	for (size_t b = 0; b < 13; ++b)
		img.dev.at(64 + 40)[b] = 0xFF;
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	Scan scan = run_scan(vol, img.dev, true);
	CHECK(scan.last.status == AIUS_SD_FREE_SCAN_PROTOCOL);
}

/* Mounted for chunks 0..n-2, dropped at the final chunk: nothing is stored. */
void test_mount_dropped_at_final_chunk_stores_nothing()
{
	Image img;
	build_fat(img, 32, 66000, 1, 2, 520, 0, 32, 3);
	FsVolume vol;
	CHECK(vol.begin(&img.dev, false, static_cast<uint8_t>(0)));
	uint64_t cursor = 0;
	uint64_t so_far = 0;
	for (;;) {
		const bool final_chunk = cursor + kChunk >= img.total_sectors;
		AiusSdFreeScanResult r = aius_sd_free_scan_step(
			&vol, &img.dev, cursor, so_far, !final_chunk);
		if (final_chunk) {
			CHECK(r.status == AIUS_SD_FREE_SCAN_DONE);
			CHECK(r.used_bytes == used_bytes(img, img.expected_free));
			break;
		}
		CHECK(r.status == AIUS_SD_FREE_SCAN_MORE);
		so_far += r.chunk_free;
		cursor += kChunk;
	}
	CHECK(vol.aiusFreeClusters(-1) == -1);
}

void test_chunk_bounds()
{
	CHECK(aius_sd_free_scan_chunk_bound(32) == 4096);
	CHECK(aius_sd_free_scan_chunk_bound(16) == 8192);
	CHECK(aius_sd_free_scan_chunk_bound(64) == AIUS_SD_USAGE_CHUNK_MAX_ENTRIES);
	CHECK(aius_sd_free_scan_chunk_bound(12) == 0);
	const uint8_t types[] = {16, 32, 64};
	for (uint8_t type : types) {
		const uint32_t bound = aius_sd_free_scan_chunk_bound(type);
		CHECK(aius_sd_free_scan_chunk_ok(type, bound));
		CHECK(!aius_sd_free_scan_chunk_ok(type, bound + 1u));
	}
	/* A chunk legal for exFAT is over a FAT32 chunk's count. */
	CHECK(!aius_sd_free_scan_chunk_ok(32, 8192));
}

}

int main()
{
	test_chunk_bounds();
	test_exfat_padding_bits_set_is_protocol();
	test_mount_dropped_at_final_chunk_stores_nothing();
	test_used_never_exceeds_total_and_no_mid_scan_store();
	test_fat12();
	test_fat16();
	test_fat32();
	test_exfat();
	test_dirty_fat_cache_is_synced();
	printf("sd-free-scan: ok\n");
	return 0;
}
