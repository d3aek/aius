/* Link script for the first-stage chooser (aius-ota-resilience.md Phase 3a):
 * a freestanding XIP image occupying the head region's code span only --
 * FCB sector, IVT, then code -- and nothing else. No RAM sections at all:
 * the ASSERTs make .data and .bss link errors, because the chooser has no
 * startup to copy or zero them and a global that needed either would be a
 * bug the build must catch, not the boot. */

MEMORY
{
	FLASH (rx): ORIGIN = 0x60000000, LENGTH = 0xE000
}

ENTRY(chooser_reset)

SECTIONS
{
	.text.headers : {
		KEEP(*(.flashconfig))
		FILL(0xFF)
		. = ORIGIN(FLASH) + 0x1000;
		KEEP(*(.ivt))
		KEEP(*(.bootdata))
		. = ALIGN(4);
	} > FLASH

	.text : {
		KEEP(*(.startup))
		*(.text*)
		*(.rodata*)
		. = ALIGN(4);
	} > FLASH

	/DISCARD/ : {
		*(.ARM.exidx*)
		*(.ARM.extab*)
		*(.comment)
	}
}

ASSERT(SIZEOF(.text.headers) >= 0x1020,
	"chooser: the FCB or IVT is missing from the image")
ASSERT(DEFINED(chooser_main),
	"chooser: the decision entry point is missing")
