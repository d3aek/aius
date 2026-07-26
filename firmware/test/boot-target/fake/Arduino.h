#ifndef AIUS_BOOT_TARGET_FAKE_ARDUINO_H
#define AIUS_BOOT_TARGET_FAKE_ARDUINO_H

#include <stddef.h>
#include <stdint.h>

#define DMAMEM __attribute__((section(".dmabuffers")))

#define SRC_SRSR_IPP_RESET_B UINT32_C(0x00000001)
#define SRC_SRSR_LOCKUP_SYSRESETREQ UINT32_C(0x00000002)
#define SRC_SRSR_CSU_RESET_B UINT32_C(0x00000004)
#define SRC_SRSR_IPP_USER_RESET_B UINT32_C(0x00000008)
#define SRC_SRSR_WDOG_RST_B UINT32_C(0x00000010)
#define SRC_SRSR_JTAG_RST_B UINT32_C(0x00000020)
#define SRC_SRSR_JTAG_SW_RST UINT32_C(0x00000040)
/* Bit 7 is WDOG3, not a "warm boot". This fake declared
 * SRC_SRSR_WARM_BOOT at 0x80 until 2026-07-26 -- a name that does not exist in
 * imxrt.h, at the value imxrt.h assigns to a different cause. Nothing used it, so
 * it was a trap rather than a live defect: the first code to reach for bit 7 would
 * have compiled against an invented meaning and reported a WDOG3 reset as a warm
 * boot. Renamed to the real thing rather than deleted, because bit 7 is a real
 * reset cause a reader may legitimately want. */
#define SRC_SRSR_WDOG3_RST_B UINT32_C(0x00000080)
#define SRC_SRSR_TEMPSENSE_RST_B UINT32_C(0x00000100)

#define CCM_CCGR_ON UINT32_C(3)
#define CCM_CCGR3_WDOG1(value) ((uint32_t)(value) << 16)

#define WDOG_WCR_WDE UINT16_C(0x0004)
#define WDOG_WCR_WDT UINT16_C(0x0008)
#define WDOG_WCR_SRE UINT16_C(0x0040)
#define WDOG_WCR_SRS UINT16_C(0x0010)
/* The `& 0xFF` is the real macro's, and dropping it made this fake MORE permissive
 * than the hardware: a timeout value above 255 is masked by imxrt.h and silently
 * truncated to its low byte, where the unmasked form shifted the excess out of the
 * register entirely. Two different wrong answers, so a test that exercised an
 * over-range timeout would have agreed with neither the target nor itself. */
#define WDOG_WCR_WT(value) ((uint16_t)(((uint16_t)(value) & 0xFFu) << 8))

extern volatile uint32_t SRC_SRSR;
extern volatile uint32_t CCM_CCGR3;
extern volatile uint16_t WDOG1_WMCR;
extern volatile uint16_t WDOG1_WCR;
extern volatile uint16_t WDOG1_WSR;

void arm_dcache_delete(void *address, uint32_t size);
void arm_dcache_flush(void *address, uint32_t size);
void aius_test_arm_dsb(void);

#define __DSB() aius_test_arm_dsb()

#endif
