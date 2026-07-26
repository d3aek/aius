/* Aius Teensy 4.1 link script — the framework's imxrt1062_t41.ld with ONE
 * addition: a retained OCRAM region carved out ahead of `.bss.dma`. Selected by
 * `board_build.ldscript` for the teensy41-optical envs. Everything between this
 * header and the trailing ASSERT block is a byte-for-byte copy of the framework
 * script apart from the `.bss.aius_retained` output section, and
 * test/boot-target/test_ldscript_drift.py fails if that stops being true.
 *
 * A COPY RATHER THAN AN AUGMENTATION, for a reason worth recording: a
 * supplementary script (`SECTIONS { ... } INSERT BEFORE .bss.dma`) cannot also
 * `INCLUDE imxrt1062_t41.ld`, because ld treats any script containing INSERT as
 * an augmentation of the DEFAULT script and discards that script's own SECTIONS
 * — the link then fails with "`.bss.dma` not found for insert". Passing the
 * augmentation as a link flag instead reaches ld BEFORE the board's script
 * whatever order platformio.ini lists them in, because GCC's link spec emits
 * `-Wl,`-forwarded options with the object list and `-T` options after them, so
 * `> RAM` resolves against a region that has not been declared yet. The copy is
 * the only form that is order-independent, and the drift test is what makes it
 * safe.
 *
 * WHY A FIXED ADDRESS AT ALL. The i.MX RT1062 boot ROM runs on every reset and
 * uses OCRAM as its own scratch. Measured on the deployed board (2026-07-26,
 * build 72bb041a56fb) by filling 0x20200000..0x2027F800 with a self-describing
 * pattern, resetting, and scanning it from `startup_early_hook` before any other
 * code runs. Three runs come back overwritten, identically for a SYSRESETREQ
 * (SRC_SRSR=0x02) and for a WDOG1 timeout (SRC_SRSR=0x10):
 *
 *     0x20200C60..0x20201100    1,184 B
 *     0x20201B80..0x20201C40      192 B
 *     0x20203A60..0x20207F60   17,664 B
 *
 * Everything else in that window survives both. `g_aius_boot_slots` used to be
 * DMAMEM, so it floated inside `.bss.dma` and its address was a function of every
 * DMA buffer declared ahead of it: at 0x20201120 it happened to land in the
 * 2,688-byte hole between the first two runs and retention worked; the 24-bit
 * S/PDIF output ring added 16 KiB ahead of it, pushed it to 0x20205140 — inside
 * the third run — and retention silently stopped, taking recovery-safe mode with
 * it. Neither address was chosen. This one is.
 *
 * WHY THE BOTTOM OF OCRAM. Placing `.bss.aius_retained` before `.bss.dma` pins it
 * at ORIGIN(RAM) by construction, so no DMAMEM buffer at any size or in any order
 * can move it. It is also below `_heap_start` (derived from the end of
 * `.bss.dma`), so the malloc/new heap can never reach it either. The 3,168 bytes
 * from ORIGIN(RAM) to the ROM's first clobbered run are the largest
 * measured-retained span with both properties.
 *
 * The trailing ASSERTs are the enforcement: the address and the headroom are link
 * errors, not conventions. `test/boot-target/verify_target.py` re-checks the same
 * contract post-link against the full measured map. */

MEMORY
{
	ITCM (rx):  ORIGIN = 0x00000000, LENGTH = 512K
	DTCM (rw):  ORIGIN = 0x20000000, LENGTH = 512K
	RAM (rw):   ORIGIN = 0x20200000, LENGTH = 512K
	FLASH (rx): ORIGIN = 0x60000000, LENGTH = 7936K
	ERAM (rw):  ORIGIN = 0x70000000, LENGTH = 32768K
}

ENTRY(ImageVectorTable)

SECTIONS
{
	.text.headers : {
		KEEP(*(.flashconfig))
		FILL(0xFF)
		. = ORIGIN(FLASH) + 0x1000;
		KEEP(*(.ivt))
		KEEP(*(.bootdata))
		. = ALIGN(1024);
	} > FLASH

	.text.code : {
		KEEP(*(.startup))
		*(.flashmem*)
		. = ALIGN(4);
		KEEP(*(.init))
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		__init_array_start = .;
		KEEP (*(.init_array))
		__init_array_end = .;
		. = ALIGN(4);
	} > FLASH

	.text.progmem : {
		*(.progmem*)
		. = ALIGN(4);
	} > FLASH

	.text.itcm : {
		. = . + 32; /* MPU to trap NULL pointer deref */
		*(.fastrun)
		*(.text*)
		. = ALIGN(16);
	} > ITCM  AT> FLASH

	.ARM.exidx : {
		__exidx_start = .;
		*(.ARM.exidx* .ARM.extab.text* .gnu.linkonce.armexidx.*)
		__exidx_end = .;
	} > ITCM  AT> FLASH

	.data : {
		*(.endpoint_queue)   
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.rodata*)))
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.data*)))
		KEEP(*(.vectorsram))
	} > DTCM  AT> FLASH

	.bss ALIGN(4) : {
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(COMMON)))
		. = ALIGN(32);
		. = . + 32; /* MPU to trap stack overflow */
	} > DTCM

	/* AIUS ADDITION — retained across reset, pinned to ORIGIN(RAM). See the
	 * header. Must stay immediately before .bss.dma: that is what fixes the
	 * address, and .bss.dma then simply starts after it. */
	.bss.aius_retained (NOLOAD) : {
		__aius_retained_start = .;
		KEEP(*(.aius_retained))
		. = ALIGN(32);
		__aius_retained_end = .;
	} > RAM

	.bss.dma (NOLOAD) : {
		*(.hab_log)
		*(.dmabuffers)
		. = ALIGN(32);
	} > RAM

	.bss.extram (NOLOAD) : {
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.externalram)))
		. = ALIGN(32);
	} > ERAM

	.text.csf : {
		FILL(0xFF)
		. = ALIGN(1024);
		KEEP(*(.csf))
		__text_csf_end = .;
	} > FLASH

	_stext = ADDR(.text.itcm);
	_etext = ADDR(.text.itcm) + SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx);
	_stextload = LOADADDR(.text.itcm);

	_sdata = ADDR(.data);
	_edata = ADDR(.data) + SIZEOF(.data);
	_sdataload = LOADADDR(.data);

	_sbss = ADDR(.bss);
	_ebss = ADDR(.bss) + SIZEOF(.bss);

	_heap_start = ADDR(.bss.dma) + SIZEOF(.bss.dma);
	_heap_end = ORIGIN(RAM) + LENGTH(RAM);

	_extram_start = ADDR(.bss.extram);
	_extram_end = ADDR(.bss.extram) + SIZEOF(.bss.extram);

	_itcm_block_count = (SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) + 0x7FFF) >> 15;
	_flexram_bank_config = 0xAAAAAAAA | ((1 << (_itcm_block_count * 2)) - 1);
	_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15);

	_flashimagelen = __text_csf_end - ORIGIN(FLASH);
	_teensy_model_identifier = 0x25;

	.debug_info     0 : { *(.debug_info) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }

}

/* AIUS ADDITION — the enforcement. 0xC60 is the offset of the boot ROM's first
 * clobbered OCRAM run from ORIGIN(RAM); growing the retained region past it puts
 * retained state where the ROM writes, which is the defect these lines exist to
 * make impossible. */
ASSERT(ADDR(.bss.aius_retained) == ORIGIN(RAM),
	"aius: the retained region must start at ORIGIN(RAM); a floating address is what broke boot-ledger retention once already")
ASSERT(SIZEOF(.bss.aius_retained) <= 0xC60,
	"aius: the retained region would extend into the boot ROM's OCRAM scratch at ORIGIN(RAM)+0xC60")
ASSERT(ADDR(.bss.dma) == ADDR(.bss.aius_retained) + SIZEOF(.bss.aius_retained),
	"aius: .bss.dma must follow the retained region inside RAM, not overlap it")
