/* The first-stage chooser: the never-erased image the ROM boots, whose whole
 * job is to pick slot A or slot B and jump (aius-ota-resilience.md Phase 3a).
 *
 * DELIBERATELY TINY AND FROZEN. This is the one piece of flash OTA never
 * touches, so every line here is a line that can never be fixed remotely --
 * its update story is HalfKay only, by design. It therefore does as close to
 * nothing as possible:
 *
 *   - no clock setup, no FlexRAM reconfiguration, no peripherals, no
 *     interrupts, no watchdog: the slot image's own ResetHandler is written
 *     to run from ROM-handoff state, and the less this stage disturbs that
 *     state the truer that assumption stays. Code executes XIP straight from
 *     flash at the ROM's boot clocks; the stack is the top of OCRAM (mapped
 *     at reset, far above the retained window this stage writes).
 *   - no flash WRITES: the boot record is read-only here. The one thing the
 *     chooser persists -- the trial boot count -- goes to the RETAINED OCRAM
 *     ledger (caches are still off at ROM handoff, so plain stores land),
 *     which survives the WDOG reset a crashing trial dies by.
 *   - all decision logic is lib/aius_ab's aius_ab_choose(), the SAME
 *     translation unit the host suite drives and the application links, so
 *     the three can never disagree about a rule.
 *
 * The jump: a slot image keeps the standard Teensy layout at its own base,
 * so its IVT sits at slot_base + 0x1000 and names its ResetHandler exactly
 * the way the head IVT names this file's entry. The chooser validates the
 * tag and the entry bounds (aius_ab_slot_ivt_sane) and branches; the slot's
 * ResetHandler then runs as if the ROM had jumped to it. Both slots insane
 * -- a state only a hand-mangled flash can produce, since activation never
 * marks a slot pending before its bytes verified -- parks in WFI for the
 * button, exactly the old recovery boundary.
 *
 * The FCB is not in this file: the build extracts the .flashconfig bytes
 * from the application build's own bootdata object, so the ROM boots the
 * chooser with byte-identical FlexSPI configuration to every image this
 * board has ever run. */

#include "aius_ab.h"

#define CHOOSER_STACK_TOP 0x20280000u /* top of OCRAM, mapped at reset */

/* FlexSPI1 AHB address-remap registers (aius-ota-slot-agnostic.md). The chooser
 * is freestanding -- no imxrt.h -- so the addresses are inlined here; they are
 * IOMUXC_GPR_GPR30/31/32 (FLEXSPI_REMAP_ADDR_START / END / OFFSET) at the
 * IOMUXC_GPR base 0x400AC000. Programming them makes the canonical link window
 * resolve to the chosen physical slot; they are cleared by every reset, which
 * is why the chooser -- run by the ROM on every boot, and living OUTSIDE the
 * window -- must re-establish them each time. */
#define AIUS_IOMUXC_GPR_REMAP_START 0x400AC078u
#define AIUS_IOMUXC_GPR_REMAP_END 0x400AC07Cu
#define AIUS_IOMUXC_GPR_REMAP_OFFSET 0x400AC080u

/* The ROM-parsed structures, hand-rolled: hdr tag 0xD1, and boot data
 * declaring the XIP image (start == flash base, so the ROM jumps in place). */
struct chooser_ivt {
	uint32_t hdr;
	uint32_t entry;
	uint32_t reserved1;
	uint32_t dcd;
	uint32_t boot_data;
	uint32_t self;
	uint32_t csf;
	uint32_t reserved2;
};

struct chooser_boot_data {
	uint32_t start;
	uint32_t length;
	uint32_t plugin;
};

void chooser_reset(void);
void chooser_main(void);

__attribute__((section(".bootdata"), used))
static const struct chooser_boot_data boot_data = {
	AIUS_AB_FLASH_BASE,
	/* The chooser's region only: the record sectors above it are data,
	 * not image. */
	0xE000u,
	0u,
};

__attribute__((section(".ivt"), used))
static const struct chooser_ivt image_vector_table = {
	0x432000D1u,
	(uint32_t)&chooser_reset,
	0u,
	0u,
	(uint32_t)&boot_data,
	(uint32_t)&image_vector_table,
	0u,
	0u,
};

/* The ROM branches here with no stack of ours: establish one, run the
 * decision, and never return (chooser_main ends in a jump or a WFI park). */
__attribute__((naked, section(".startup"), used))
void chooser_reset(void)
{
	__asm volatile("ldr sp, =%0\n\t"
	               "bl chooser_main\n\t"
	               "1: wfi\n\t"
	               "b 1b\n\t"
	               :
	               : "i"(CHOOSER_STACK_TOP));
}

static uint32_t flash_word(uint32_t address)
{
	return *(const volatile uint32_t *)address;
}

static bool slot_sane(uint8_t slot)
{
	uint32_t base = aius_ab_slot_base(slot);

	return aius_ab_slot_ivt_sane(
		slot, flash_word(base + AIUS_AB_SLOT_IVT_OFFSET),
		flash_word(base + AIUS_AB_SLOT_IVT_OFFSET + 4u));
}

/* Program the FlexSPI remap so the canonical link window resolves to `slot`'s
 * physical flash, then barrier before the jump. A slot-agnostic image is linked
 * ONCE at the canonical window and the remap (offset 0 for slot A, +one slot
 * span for slot B) redirects it to the physical slot the chooser picked.
 *
 * NO FlexSPI RX-buffer flush (MCR0 SWRESET) is issued, deliberately. The
 * documented flush guards against a pre-remap prefetch line being served for a
 * canonical address after the window moves -- but the chooser executes ONLY in
 * the head region and never fetches a canonical-window address before this
 * point, so the AHB RX buffer holds no such line: the first post-jump canonical
 * fetch is a compulsory miss served under the new mapping. Issuing SWRESET here
 * would reset the very FlexSPI the chooser is executing XIP from, risking the
 * next instruction fetch in the un-recoverable first stage. DSB+ISB orders the
 * GPR writes and flushes the pipeline before the computed branch, which is what
 * this needs. BENCH-REQUIRED: confirm no stale canonical line survives; if one
 * ever does, do the flush from a RAM trampoline, never from flash. */
static void program_remap(uint8_t slot)
{
	AiusAbRemap remap;

	if (!aius_ab_remap_for_slot(slot, &remap))
		return;
	*(volatile uint32_t *)AIUS_IOMUXC_GPR_REMAP_START = remap.start;
	*(volatile uint32_t *)AIUS_IOMUXC_GPR_REMAP_END = remap.end;
	*(volatile uint32_t *)AIUS_IOMUXC_GPR_REMAP_OFFSET = remap.offset;
	__asm volatile("dsb\n\t"
	               "isb\n\t"
	               :
	               :
	               : "memory");
}

/* Program the remap for `slot`, then jump to its entry point. The entry pointer
 * is read from the slot's PHYSICAL IVT while the remap is still at its reset
 * (identity) value, and it is a CANONICAL address (every image links at the
 * canonical window), so after program_remap() the branch lands in the chosen
 * physical slot. Never returns. */
__attribute__((noreturn))
static void boot_slot_with_remap(uint8_t slot)
{
	uint32_t base = aius_ab_slot_base(slot);
	uint32_t entry =
		flash_word(base + AIUS_AB_SLOT_IVT_OFFSET + 4u);

	program_remap(slot);
	((void (*)(void))entry)();
	for (;;)
		__asm volatile("wfi");
}

void chooser_main(void)
{
	AiusAbRecord record;
	AiusAbLedger ledger;
	AiusAbChoice choice;
	uint8_t *retained = (uint8_t *)AIUS_AB_LEDGER_ADDRESS;
	const AiusAbRecord *record_ref = NULL;
	const AiusAbLedger *ledger_ref = NULL;
	uint8_t boot_slot;

	if (aius_ab_record_pick((const uint8_t *)AIUS_AB_RECORD_SECTOR_0,
	                        (const uint8_t *)AIUS_AB_RECORD_SECTOR_1,
	                        AIUS_AB_RECORD_BYTES, &record) >= 0)
		record_ref = &record;
	if (aius_ab_ledger_decode(&ledger, retained, AIUS_AB_LEDGER_BYTES))
		ledger_ref = &ledger;

	aius_ab_choose(record_ref, ledger_ref, &choice);

	/* The slot actually booted: the chosen one if its IVT is sane, else the
	 * other if IT is sane (the insane-fallback path, reachable only from a
	 * hand-mangled flash since activation never marks a slot pending before
	 * its bytes verify). AIUS_AB_SLOT_NONE means neither is bootable. */
	boot_slot = AIUS_AB_SLOT_NONE;
	if (slot_sane(choice.boot_slot))
		boot_slot = choice.boot_slot;
	else if (slot_sane(aius_ab_other_slot(choice.boot_slot)))
		boot_slot = aius_ab_other_slot(choice.boot_slot);

	/* Persist the ledger with booted_slot ALWAYS -- not only when the trial
	 * count changed -- because it is the application's SOLE channel to its
	 * physical slot under a single canonical build (aius-ota-slot-agnostic.md):
	 * the image's own address space is canonical whichever slot runs. On the
	 * insane-fallback path booted_slot names the slot truly taken, so the app
	 * reconciles against reality rather than the record. */
	if (boot_slot != AIUS_AB_SLOT_NONE)
		choice.ledger.booted_slot = boot_slot;
	(void)aius_ab_ledger_encode(&choice.ledger, retained,
	                            AIUS_AB_LEDGER_BYTES);

	if (boot_slot != AIUS_AB_SLOT_NONE)
		boot_slot_with_remap(boot_slot);
	/* Both slots unbootable: park for the Program button (HalfKay is a
	 * separate chip and always reachable). The WFI loop is in
	 * chooser_reset's tail. */
}
