#ifndef AIUS_AB_LAYOUT_H
#define AIUS_AB_LAYOUT_H

#include <stdint.h>

/* THE A/B FLASH MAP — the one place the partition arithmetic lives
 * (aius-ota-resilience.md Phase 3; owner approved 2026-08-16: 64 KiB head +
 * two equal slots, software-defined, no fuse burn).
 *
 * Three parties must agree on these numbers: the first-stage chooser (which
 * jumps by them), the application (which activates into the inactive slot
 * and writes the boot record), and the build tooling (which links each slot
 * image at its base and merges the recovery hex). A second copy of any of
 * them would be a second place to drift, so chooser, app and tools all
 * include THIS header and the host suite pins the arithmetic.
 *
 *   0x60000000  FCB              (sector 0 -- the ROM's FlexSPI config)
 *   0x60001000  IVT + chooser    (the never-erased first stage; its whole
 *                                 update story is deliberately HalfKay-only)
 *   0x6000E000  boot record A    (ping-pong pair: the record with the
 *   0x6000F000  boot record B     higher valid sequence wins)
 *   0x60010000  SLOT A           3,936 KiB
 *   0x603E8000  SLOT B           3,936 KiB
 *   0x607C0000  (end of the app-addressable region; the EEPROM-emulation
 *                tail above it is untouched by any of this)
 *
 * WHY THE HEAD IS NEVER ERASED. The 2026-08-16 brick was the single-image
 * scheme's designed cost: erase the FCB first, and any in-window failure
 * leaves an un-ROM-bootable board. Under this map OTA never touches the
 * head region at all -- the activation window is exactly the INACTIVE slot
 * -- so the ROM always finds a valid FCB+IVT and the chooser always runs.
 * A failed program, a corrupt image or a non-booting image all degrade to
 * "the chooser boots the other slot". */
#define AIUS_AB_FLASH_BASE UINT32_C(0x60000000)
#define AIUS_AB_HEAD_BYTES UINT32_C(0x00010000) /* 64 KiB, 16 sectors */
#define AIUS_AB_SECTOR_BYTES UINT32_C(0x1000)

/* The ping-pong boot-record sectors, inside the head region. Written ONLY by
 * the application (the chooser is read-only over flash by design: keeping
 * the flash-write driver out of the never-updated first stage keeps it too
 * simple to be wrong). */
#define AIUS_AB_RECORD_SECTOR_0 UINT32_C(0x6000E000)
#define AIUS_AB_RECORD_SECTOR_1 UINT32_C(0x6000F000)

#define AIUS_AB_SLOT_BYTES UINT32_C(0x003D8000) /* 3,936 KiB per slot */
#define AIUS_AB_SLOT_A_BASE (AIUS_AB_FLASH_BASE + AIUS_AB_HEAD_BYTES)
#define AIUS_AB_SLOT_B_BASE (AIUS_AB_SLOT_A_BASE + AIUS_AB_SLOT_BYTES)

/* THE CANONICAL LINK WINDOW (aius-ota-slot-agnostic.md). A slot-agnostic image
 * is linked ONCE at this window -- slot A's window -- and the first-stage
 * chooser's FlexSPI AHB remap makes the window resolve to whichever PHYSICAL
 * slot holds the running image (offset 0 for slot A, +AIUS_AB_SLOT_BYTES for
 * slot B). Because every image links here, a slot's IVT entry pointer is
 * validated against THIS window, not the slot's own physical base
 * (aius_ab_slot_ivt_sane), and the release builds and signs one binary.
 * The window is exactly slot A's, so HalfKay recovery (chooser + slot A at the
 * canonical base) needs no remap. */
#define AIUS_AB_CANONICAL_BASE AIUS_AB_SLOT_A_BASE
#define AIUS_AB_CANONICAL_BYTES AIUS_AB_SLOT_BYTES

/* Slot identities, as they appear in the boot record, the retained trial
 * ledger, the container header's slot tag and the AIUS_AB_SLOT build flag.
 * 0 is "no slot" -- the single-image build and the untagged container. */
#define AIUS_AB_SLOT_NONE 0u
#define AIUS_AB_SLOT_A 1u
#define AIUS_AB_SLOT_B 2u

/* Every slot image keeps the standard Teensy image layout at its own base
 * (flashconfig filler, then the IVT at +0x1000): the FCB copy is dead --
 * only sector 0's is ever read by the ROM -- but the uniform layout lets
 * the chooser find a slot's entry point exactly the way the ROM finds the
 * head's, and keeps every existing post-link tool working unchanged. */
#define AIUS_AB_SLOT_IVT_OFFSET UINT32_C(0x1000)

/* The RETAINED trial ledger: the chooser's only writable state, in the
 * ROM-safe retained OCRAM window (survives the WDOG reset a crashing trial
 * image dies by; cleared by a cold power cycle, which fail-closes to "fresh
 * trial"). The address is a CONTRACT between two different images -- the
 * chooser hardcodes it, the application's ldscript places its
 * `.aius_retained_ab` input section there -- and the post-link verifier
 * pins the application side to this constant. It sits immediately after the
 * OTA dark stamp: boot slots 128 + abort latch 8 (padded to 32-byte
 * alignment) + poll probe 64 + dark stamp 64 = 0x120 from ORIGIN(RAM).
 * Both sides fail closed on the magic/CRC, so a layout drift reads as "no
 * ledger", never as someone else's bytes. */
#define AIUS_AB_LEDGER_ADDRESS UINT32_C(0x20200120)
#define AIUS_AB_LEDGER_BYTES 32u

#endif
