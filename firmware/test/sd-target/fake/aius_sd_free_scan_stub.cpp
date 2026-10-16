/* Host link stand-in for src/aius_sd_free_scan.cpp in suites that compile
 * aius_sd_teensy.cpp against a fake SD.h (runtime-target): the real unit needs
 * the vendored SdFat, which sd-free-scan covers. Never reached by those suites
 * (no USED_SIZE action runs); aborts if it ever is, as the fake
 * SDClass::usedSize() does. */
#include "aius_sd_free_scan.h"

#include <stdlib.h>

AiusSdFreeScanResult aius_sd_free_scan_step(
	FsVolume *, uint64_t, uint64_t, bool)
{
	abort();
}
