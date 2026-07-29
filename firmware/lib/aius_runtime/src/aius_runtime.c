#include "aius_runtime.h"

#include <stddef.h>

/* Cold code -> FLASH. Nothing here runs on the audio block path (this is the
 * per-epoch checkpoint walk), so every definition below carries FLASHMEM and
 * lands in FLASH instead of the shared FlexRAM/ITCM pool the audio pipeline
 * needs. FLASHMEM is Teensyduino's __attribute__((section(".flashmem"))) from
 * <avr/pgmspace.h>; TEENSYDUINO is a project-wide compiler -D, so the native
 * host suites fall through to the no-op and this file keeps its portable,
 * Arduino-free compile. The attribute must sit on each DEFINITION: GCC does not
 * merge a `section` attribute from a forward declaration, so a prototype-only
 * FLASHMEM silently leaves the code in ITCM. Enforced post-link by
 * test/itcm-placement/verify_target.py. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

static const AiusRuntimeBudget runtime_budgets[] = {
	{AIUS_BOOT_CHECKPOINT_RESET_CAPTURE, 2u, 0u, 0u, 250u},
	{AIUS_BOOT_CHECKPOINT_USB_RX, 64u, 64u, 0u, 0u},
	{AIUS_BOOT_CHECKPOINT_USB_TX, 1u, 0u, 64u, 500u},
	/* METRICS hosts the measurement loopback's sliced analysis (256 samples
	 * per epoch through both channel accumulators, ~2 x 170 us worst case)
	 * beside the 1 Hz hardware sampler, so its wall-clock allowance is now a
	 * declared number rather than 0: "no epoch computes more than its budget"
	 * must bind to something, and 1 ms is the field's smallest expressible
	 * bound. The composition root times the checkpoint against it and counts
	 * overruns on the HTTP checkpoint's pattern; nothing aborts on one. */
	{AIUS_BOOT_CHECKPOINT_METRICS, 1u, 0u, 0u, 1u},
	{AIUS_BOOT_CHECKPOINT_NETWORK, 1u, 0u, 0u, 0u},
	/* The HTTP wall-clock allowance is the legacy service_http() overrun
	 * threshold (2,000 us) carried forward: the composition root times the
	 * checkpoint against it and counts http_service_overruns. Nothing
	 * aborts on it -- it is the declared bound a healthy epoch stays
	 * inside, and the counter is the evidence when one does not. */
	{AIUS_BOOT_CHECKPOINT_HTTP, 1u, 256u, 512u, 2u},
	{AIUS_BOOT_CHECKPOINT_UDP, 1u, 532u, 532u, 2u},
	/* The replay checkpoint moves no transport bytes: its work is a bounded
	 * SD job, whose size is AIUS_REPLAY_REFILL_MAX_BYTES and whose per-epoch
	 * cost is one card primitive. This row's rx figure is therefore vestigial
	 * and is deliberately NOT the refill quota -- reading it as one (and as
	 * words, at that) is what regressed the replay. Left at its historical
	 * value rather than zeroed so no runtime assertion moves; see
	 * AiusRuntimeBudget in the header for the unit contract. */
	{AIUS_BOOT_CHECKPOINT_REPLAY, 1u, 2048u, 0u, 0u},
	/* The SD checkpoint's 512 is load-bearing and is the one number here that
	 * bounds a BLOCKING primitive: it is the transfer slice every storage job
	 * reads or writes in one call, and it matches AIUS_SD_MAX_TRANSFER in the
	 * portable engine, which refuses any job whose capacity exceeds it. The
	 * plan's rejection of a 12 KB per-primitive slice is this number. */
	{AIUS_BOOT_CHECKPOINT_SD, 1u, 512u, 512u, 0u},
	/* Audio's transport byte quotas are zero because its word and output-step
	 * limits belong to the audio service seam. This row only grants one
	 * callback and its scheduler-visible wall allowance. */
	{AIUS_BOOT_CHECKPOINT_AUDIO, 1u, 0u, 0u, 8u},
	{AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE, 0u, 0u, 0u, 0u},
};

static FLASHMEM bool runtime_service(
    const AiusRuntimeOps *ops,
    uint16_t checkpoint)
{
	const AiusRuntimeBudget *budget = aius_runtime_budget(checkpoint);

	ops->checkpoint(ops->context, checkpoint);
	return ops->service(ops->context, budget);
}

FLASHMEM const AiusRuntimeBudget *aius_runtime_budget(
    uint16_t checkpoint)
{
	if (checkpoint > AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE)
		return NULL;

	return &runtime_budgets[checkpoint];
}

FLASHMEM bool aius_runtime_epoch(
    const AiusRuntimeOps *ops,
    bool recovery_safe)
{
	AiusRuntimeOps snapshot;
	uint16_t checkpoint;

	if (ops == NULL)
		return false;

	snapshot = *ops;
	if (snapshot.service == NULL || snapshot.checkpoint == NULL ||
	    snapshot.feed_watchdog == NULL)
		return false;

	if (recovery_safe) {
		if (!runtime_service(
		        &snapshot, AIUS_BOOT_CHECKPOINT_USB_RX))
			return false;
		if (!runtime_service(
		        &snapshot, AIUS_BOOT_CHECKPOINT_USB_TX))
			return false;
	} else {
		for (checkpoint = AIUS_BOOT_CHECKPOINT_RESET_CAPTURE;
		     checkpoint <= AIUS_BOOT_CHECKPOINT_REPLAY;
		     ++checkpoint) {
			if (!runtime_service(&snapshot, checkpoint))
				return false;
		}
		if (!runtime_service(&snapshot, AIUS_BOOT_CHECKPOINT_AUDIO))
			return false;
		if (!runtime_service(&snapshot, AIUS_BOOT_CHECKPOINT_SD))
			return false;
	}

	snapshot.checkpoint(
	    snapshot.context, AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	snapshot.feed_watchdog(snapshot.context);
	return true;
}
