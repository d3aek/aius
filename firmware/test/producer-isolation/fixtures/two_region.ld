/* W8 fixture link script: flash at 0x60000000 and ITCM at 0 lie more than
 * 16 MB apart, so every Thumb bl between them goes through a linker veneer
 * (as on the Teensy 4.1 image). .itcm.first pins one function at address 0
 * so a plain data word of 1 equals its Thumb address. */
ENTRY(fx_reset)

MEMORY
{
	ITCM  (rwx) : ORIGIN = 0x00000000, LENGTH = 512K
	DTCM  (rw)  : ORIGIN = 0x20000000, LENGTH = 64K
	FLASH (rx)  : ORIGIN = 0x60000000, LENGTH = 1M
}

SECTIONS
{
	.text.itcm : {
		KEEP(*(.itcm.first))
		*(.itcm.text*)
	} > ITCM

	.text.code : {
		*(.text*)
		*(.rodata*)
	} > FLASH

	.data : {
		*(.data*)
	} > DTCM

	.bss (NOLOAD) : {
		*(.bss*)
		*(COMMON)
	} > DTCM

	_ebss = ADDR(.bss) + SIZEOF(.bss);
	_estack = ORIGIN(DTCM) + LENGTH(DTCM);
}
