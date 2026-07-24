#include "aius_fault_teensy.h"

#if defined(AIUS_FAULT_STALL_CHECKPOINT)
static_assert(AIUS_FAULT_STALL_CHECKPOINT >= 0 &&
	              AIUS_FAULT_STALL_CHECKPOINT <= 8,
              "AIUS_FAULT_STALL_CHECKPOINT must be in the range 0..8");
#endif

void aius_fault_checkpoint(uint16_t checkpoint)
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
