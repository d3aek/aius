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
	/* The service checkpoint moves no transport bytes: its work is a bounded
	 * per-epoch service call. This row's rx figure is therefore vestigial
	 * and is deliberately NOT a transfer quota. Left at its historical
	 * value rather than zeroed so no runtime assertion moves; see
	 * AiusRuntimeBudget in the header for the unit contract. */
	{AIUS_BOOT_CHECKPOINT_SERVICE, 1u, 2048u, 0u, 0u},
	/* The SD checkpoint's 512 is load-bearing and is the one number here that
	 * bounds a BLOCKING primitive: it is the transfer slice every storage job
	 * reads or writes in one call, and it matches AIUS_SD_MAX_TRANSFER in the
	 * portable engine, which refuses any job whose capacity exceeds it. The
	 * plan's rejection of a 12 KB per-primitive slice is this number. */
	{AIUS_BOOT_CHECKPOINT_SD, 1u, 512u, 512u, 0u},
	/* Audio's transport byte quotas are zero because its word and output-step
	 * limits belong to the audio service seam. This row grants one callback
	 * PER VISIT and its scheduler-visible wall allowance; the epoch visits
	 * the AUDIO checkpoint twice (before and after SD, output-jitter Phase
	 * B), so the audio seam's per-epoch ceilings are two of everything this
	 * row and its seam constants name per pass. */
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
		     checkpoint <= AIUS_BOOT_CHECKPOINT_SERVICE;
		     ++checkpoint) {
			if (!runtime_service(&snapshot, checkpoint))
				return false;
		}
		if (!runtime_service(&snapshot, AIUS_BOOT_CHECKPOINT_AUDIO))
			return false;
		if (!runtime_service(&snapshot, AIUS_BOOT_CHECKPOINT_SD))
			return false;
		/* Second AUDIO pass, so the transmit ring is topped up on BOTH
		 * sides of the SD checkpoint (output-jitter plan Phase B). With a
		 * single pass, one inter-audio gap carried every other service --
		 * an SD primitive and the next epoch's HTTP job could stack into
		 * the same gap against a 128-frame (2.67 ms) open-gate cushion.
		 * With this pass the SD primitive sits alone in its gap and HTTP
		 * heads the other, so worst-case pipeline absence is ONE heavy
		 * segment plus light checkpoints, not their sum. The audio
		 * service is pending-work-driven and returns in microseconds
		 * when the first pass drained everything, so the steady-state
		 * cost is one extra dispatch per epoch. */
		if (!runtime_service(&snapshot, AIUS_BOOT_CHECKPOINT_AUDIO))
			return false;
	}

	snapshot.checkpoint(
	    snapshot.context, AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	snapshot.feed_watchdog(snapshot.context);
	return true;
}
