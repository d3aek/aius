#ifndef AIUS_PRODUCER_YIELD_PLATFORM_H
#define AIUS_PRODUCER_YIELD_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

/* Platform probes the producer-yield library calls. Link-time injected: the
 * firmware (optical_main.cpp) defines them over the real hardware, the host
 * suite (test/producer-yield) defines them over a fake cycle clock. Direct
 * calls, not function pointers, so the producer path carries no register-
 * indirect site (producer-isolation W4) and no pointer-table DTCM. Every probe
 * must be safe from the main loop (thread mode); none may block.
 *
 *   cycles           free-running 32-bit CPU cycle counter (DWT CYCCNT)
 *   ipsr             IPSR, 0 in thread mode
 *   primask          PRIMASK, 0 when interrupts are enabled
 *   irq_software_on  NVIC IRQ_SOFTWARE enabled (the audio ISR can run)
 *   audio_ready      audio is up and not quiesced (ota_quiesce clears it)
 *   recovery_safe    the epoch is recovery-safe (no producer work allowed)
 *   work_pending     ingress is non-empty
 *   run_pass         one pass of aius_audio_service_run (the AUDIO budget)
 *   ledger_checkpoint  the boot ledger's current last_checkpoint
 *   ledger_record      record_checkpoint(cp) AND commit it (E5 wrapper's
 *                      only ledger write; the lib owns no ledger object)
 */
uint32_t aius_producer_yield_plat_cycles(void);
uint32_t aius_producer_yield_plat_ipsr(void);
uint32_t aius_producer_yield_plat_primask(void);
bool aius_producer_yield_plat_irq_software_on(void);
bool aius_producer_yield_plat_audio_ready(void);
bool aius_producer_yield_plat_recovery_safe(void);
bool aius_producer_yield_plat_work_pending(void);
void aius_producer_yield_plat_run_pass(void);
uint16_t aius_producer_yield_plat_ledger_checkpoint(void);
void aius_producer_yield_plat_ledger_record(uint16_t checkpoint);

#endif
