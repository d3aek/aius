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
		/* AIUS ADDITION — TweetNaCl pinned to flash; see the EXCLUDE_FILE on
		 * .text.itcm and aius_ota_ed25519.c's placement note. */
		*tweetnacl.c.o(.text*)
		/* AIUS ADDITION — the ffmpeg-eac3 fork pinned to flash (E-AC-3
		 * phase 4): ~87 KiB of decoder text against an ITCM bank with
		 * ~2 KiB free. FLASH-cold by plan; the fork's rodata rides
		 * .text.aius_flashro below. */
		*vendor/ffmpeg-eac3/*.o(.text*)
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

	/* AIUS ADDITION — read-only data pinned to flash (E-AC-3 phase 4 DTCM
	 * funding). The framework script copies ALL .rodata into DTCM .data;
	 * these claims keep it in flash instead, read through the cached XIP
	 * path. The vendored decoder tables (dcadec ~79 KiB dominated by the
	 * two cold VQ tables, liba52 ~4 KiB) and two string/table-heavy
	 * first-party objects fund the decoder arena's growth to the E-AC-3
	 * slot; the decode-cycle cost of the hot-table subset is re-measured
	 * against the 8 ms budget as part of this phase's target acceptance.
	 * Claimed here, BEFORE .data, because input sections bind to the
	 * first output section that names them — the framework's .data line
	 * below stays byte-identical. */
	.text.aius_flashro : {
		*vendor/dcadec-core/*.o(.rodata*)
		*vendor/ffmpeg-eac3/*.o(.rodata*)
		*lib/aius_a52/a52/*.o(.rodata*)
		*src/aius_optical_app.cpp.o(.rodata*)
		*lib/aius_http/src/aius_http_transaction.c.o(.rodata*)
		. = ALIGN(4);
	} > FLASH

	.text.itcm : {
		. = . + 32; /* MPU to trap NULL pointer deref */
		*(.fastrun)
		*(EXCLUDE_FILE(*tweetnacl.c.o *vendor/ffmpeg-eac3/*.o) .text*) /* AIUS: TweetNaCl + ffmpeg-eac3 -> flash */
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
		KEEP(*(.aius_retained_latch))
		. = ALIGN(32);
		__aius_retained_end = .;
	} > RAM

	.bss.dma (NOLOAD) : {
		*(.hab_log)
		*(.dmabuffers)
		. = ALIGN(32);
	} > RAM

	/* AIUS ADDITION — flight-recorder staging, deliberately AFTER every
	 * .bss.dma input so adding or resizing it can never move a pre-existing
	 * DMAMEM object. 2026-07-27: declaring the 64 KiB staging ring DMAMEM
	 * displaced every later .bss.dma object and made the image unbootable
	 * (aius-network-hardening.md defect 26); this section is the placement
	 * fix, and the heap start moves past it in the footer below. */
	.bss.aius_staging (NOLOAD) : {
		__aius_staging_start = .;
		KEEP(*(.aius_recorder_staging))
		. = ALIGN(32);
		__aius_staging_end = .;
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
/* The staging carve-out: the heap begins after it, overriding the vendor line
 * (a later top-level assignment wins), and the section itself must sit at the
 * end of .bss.dma so no pre-existing DMAMEM address is a function of its
 * size. */
_heap_start = ADDR(.bss.aius_staging) + SIZEOF(.bss.aius_staging);
ASSERT(ADDR(.bss.aius_staging) >= ADDR(.bss.dma) + SIZEOF(.bss.dma),
	"aius: the staging section must follow .bss.dma, never sit inside it")
ASSERT(_heap_start >= ADDR(.bss.dma) + SIZEOF(.bss.dma),
	"aius: the heap must start after both .bss.dma and the staging carve-out")
/* The defect 26 gate. NativeEthernet mallocs ~96 KiB from this heap at
 * network bring-up (64 KiB FNET stack heap + 8 x 4 KiB socket buffers); a
 * heap squeezed below that makes the malloc fail, the network service abort
 * every epoch unfed, and the board watchdog into recovery-safe on every
 * boot. 128 KiB = the measured demand plus a third of headroom; a static
 * growth that trips this fails the BUILD, which is where this class of
 * defect belongs. */
ASSERT(_heap_end - _heap_start >= 0x20000,
	"aius: OCRAM heap below the 128 KiB floor NativeEthernet's boot-time mallocs need (defect 26)")
/* ITCM consumes FlexRAM in 32 KiB blocks and moves _estack down by one block
 * for every block it claims. Six blocks leave the measured 37,856-byte DTCM
 * stack/locals zone intact; a seventh would reduce it to 5,088 bytes. Keep
 * this an upper bound rather than equality so code shrinkage remains legal. */
ASSERT(_itcm_block_count <= 6,
	"aius: ITCM growth would claim a seventh FlexRAM block and remove 32 KiB from the DTCM stack/locals zone")
/* The DTCM stack/locals floor. Nothing in the framework script bounds DTCM
 * statics: the bank is declared 512K, so an over-claim links clean and hands
 * the stack a sliver. E-AC-3 phase 4 grows the decoder arena into this bank;
 * this ASSERT turns an unfunded growth into a link error. 24,352 B is the
 * smallest zone this board has demonstrably run with (the pre-TLS-fix
 * figure recorded in platformio.ini). */
ASSERT(_estack - _ebss >= 24352,
	"aius: DTCM stack/locals zone below the 24,352 B proven floor (fund arena growth by pinning more cold rodata to flash)")
