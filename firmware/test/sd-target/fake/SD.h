#ifndef AIUS_SD_TARGET_FAKE_SD_H
#define AIUS_SD_TARGET_FAKE_SD_H

#include <stddef.h>
#include <stdint.h>

#define BUILTIN_SDCARD 254u
#define FILE_READ 1u
#define FILE_WRITE 2u
#define FILE_WRITE_BEGIN 3u

class File {
public:
	File();
	explicit File(bool valid);

	explicit operator bool() const;
	bool truncate(uint64_t size);
	bool seek(uint64_t position);
	size_t read(uint8_t *buffer, size_t length);
	size_t write(const uint8_t *buffer, size_t length);
	uint64_t size();
	void close();

private:
	bool valid_;
};

/* SD.sdfs.vol()->fatType(): the mounted volume's cached type. Not a card
 * primitive -- the real SdFat reads a member the mount already filled -- so the
 * fake deliberately does not record a call, and the adapter's one-primitive
 * accounting is unaffected by reading it. */
class FsVolume {
public:
	uint8_t fatType() const;
};

class SdFs {
public:
	FsVolume *vol();

private:
	FsVolume volume_;
};

class SDClass {
public:
	bool begin(uint8_t chip_select);
	bool mediaPresent();
	bool mkdir(const char *path);
	File open(const char *path, uint8_t mode);
	bool exists(const char *path);
	bool rename(const char *path, const char *path_to);
	bool remove(const char *path);
	uint64_t totalSize();
	uint64_t usedSize();

	SdFs sdfs;
};

extern SDClass SD;

uint32_t micros();

#endif
