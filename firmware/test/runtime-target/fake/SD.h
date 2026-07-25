/* Host stand-in for the pinned Teensy SD.h surface aius_sd_teensy.cpp uses.
 * Same shape as test/sd-target/fake/SD.h; micros() comes from the fake
 * Arduino.h so both adapters share one clock. */

#ifndef AIUS_RUNTIME_TARGET_FAKE_SD_H
#define AIUS_RUNTIME_TARGET_FAKE_SD_H

#include "Arduino.h"

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
};

extern SDClass SD;

#endif
