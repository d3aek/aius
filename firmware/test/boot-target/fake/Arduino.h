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
#define SRC_SRSR_WARM_BOOT UINT32_C(0x00000080)
#define SRC_SRSR_TEMPSENSE_RST_B UINT32_C(0x00000100)

#define CCM_CCGR_ON UINT32_C(3)
#define CCM_CCGR3_WDOG1(value) ((uint32_t)(value) << 16)

#define WDOG_WCR_WDE UINT16_C(0x0004)
#define WDOG_WCR_WDT UINT16_C(0x0008)
#define WDOG_WCR_SRE UINT16_C(0x0040)
#define WDOG_WCR_SRS UINT16_C(0x0010)
#define WDOG_WCR_WT(value) ((uint16_t)((uint16_t)(value) << 8))

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
