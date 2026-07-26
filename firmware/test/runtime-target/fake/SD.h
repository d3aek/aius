/* Host stand-in for the pinned Teensy SD.h surface aius_sd_teensy.cpp uses.
 * Same shape as test/sd-target/fake/SD.h; micros() comes from the fake
 * Arduino.h so both adapters share one clock. */

#ifndef AIUS_RUNTIME_TARGET_FAKE_SD_H
#define AIUS_RUNTIME_TARGET_FAKE_SD_H

#include "Arduino.h"

#include <stddef.h>
#include <stdint.h>

/* Values must equal the real ones -- see the sibling note in
 * test/sd-target/fake/SD.h. All three FILE_* were off by one here too, and the
 * duplication is exactly why: two fakes of one header drift independently, and
 * both were wrong in the same way with nothing comparing either to the source.
 * test_fake_fidelity.py now checks both against the package headers. */
#define BUILTIN_SDCARD 254u
#define FILE_READ 0u
#define FILE_WRITE 1u
#define FILE_WRITE_BEGIN 2u

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

/* SD.sdfs.vol()->fatType(): the mounted volume's cached type, read by the probe
 * while it holds the volume. Not a card primitive, so the fake must not count
 * one -- that is what proves the volume type costs no extra SD primitive. */
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

#endif
