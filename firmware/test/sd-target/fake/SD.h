#ifndef AIUS_SD_TARGET_FAKE_SD_H
#define AIUS_SD_TARGET_FAKE_SD_H

#include <stddef.h>
#include <stdint.h>

/* VALUES ARE THE CONTRACT, not just the names. These must equal the real ones:
 * BUILTIN_SDCARD from the SD library, and the FILE_* trio from the core's FS.h
 * (SD.h undefines any prior FILE_READ/FILE_WRITE and defers to FS.h).
 *
 * All three FILE_* were off by one until 2026-07-26 -- 1/2/3 against the real
 * 0/1/2 -- so **this fake's FILE_READ was the real FILE_WRITE**. The suite could
 * not catch it, because the tests assert `open_mode == FILE_READ` using these
 * same macros, so both sides moved together (the same self-referential shape that
 * hid a 48 dB level defect in the pipeline suite). It stayed latent only because
 * aius_sd_teensy.cpp passes the constants through by NAME and the target compiles
 * against the real headers. The trap it left: any hardcoded `1` reads as "open for
 * read" against this fake and opens for WRITE on hardware. Verified against the
 * package headers by test_fake_fidelity.py. */
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

/* SD.sdfs.vol()->fatType(): the mounted volume's cached type. Not a card
 * primitive -- the real SdFat reads a member the mount already filled -- so the
 * fake deliberately does not record a call, and the adapter's one-primitive
 * accounting is unaffected by reading it. */
class FsVolume {
public:
	uint8_t fatType() const;
	/* The Aius vendor forwarder (vendor/SdFat/src/FsLib/FsVolume.h, Phase
	 * 4h CR4-B2): the mounted partition's cacheClear(), nullptr on a failed
	 * sync or with no volume. Signature checked by test_fake_fidelity.py. */
	uint8_t* cacheClear();
};

class SdFs {
public:
	FsVolume *vol();

private:
	FsVolume volume_;
};

/* SdFat's two partition types and their cache drop, same signature as
 * vendor/SdFat/src/FatLib/FatPartition.h:95 and ExFatLib/ExFatPartition.h:78
 * (checked by test_fake_fidelity.py). The fake FsVolume::cacheClear() reaches
 * whichever the test mounted, as the vendor forwarder does. */
class FatPartition {
public:
	uint8_t* cacheClear();
};

class ExFatPartition {
public:
	uint8_t* cacheClear();
};

/* Teensy core pin control the adapter's DAT3 re-mux uses. INPUT_PULLDOWN's
 * value is the core's (cores/teensy4/core_pins.h, test_fake_fidelity.py). */
#define INPUT_PULLDOWN 3
void pinMode(uint8_t pin, uint8_t mode);
void delayMicroseconds(uint32_t usec);

class SDClass {
public:
	bool begin(uint8_t chip_select, bool dma);
	/* Vendored driver error state (firmware/vendor/SD): not card primitives,
	 * so the fake records no call for them. */
	void clearError();
	uint8_t sdErrorCode();
	bool cardDetected();
	uint32_t busyTimeouts();
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
