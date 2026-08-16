#include "aius_fault_teensy.h"

#include "aius_boot.h"

/* Cold code -> FLASH. This runs once per checkpoint visit, twelve times per
 * normal epoch (the AUDIO checkpoint is visited twice since output-jitter
 * Phase B), and never on the audio block path, so it must not occupy ITCM. In a
 * production image it compiles to a plain return; in a fault image it stalls
 * and never comes back. FLASHMEM is Teensyduino's
 * __attribute__((section(".flashmem"))); TEENSYDUINO is a project-wide compiler
 * -D, so the native fault-target oracle falls through to the no-op. The
 * attribute sits on the DEFINITION -- GCC does not merge a `section` attribute
 * from the prototype in aius_fault_teensy.h, so a prototype-only FLASHMEM would
 * silently leave this in ITCM. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

#if defined(AIUS_FAULT_STALL_CHECKPOINT)
static_assert(AIUS_FAULT_STALL_CHECKPOINT >= 0 &&
	              AIUS_FAULT_STALL_CHECKPOINT <=
	                  AIUS_BOOT_CHECKPOINT_AUDIO,
              "AIUS_FAULT_STALL_CHECKPOINT must be in the range 0..9");
#endif

FLASHMEM void aius_fault_checkpoint(uint16_t checkpoint)
{
#if defined(AIUS_FAULT_STALL_CHECKPOINT)
	if (checkpoint == (uint16_t)AIUS_FAULT_STALL_CHECKPOINT) {
		for (;;) {
			__asm__ volatile("" ::: "memory");
		}
	}
#else
	(void)checkpoint;
#endif
}
