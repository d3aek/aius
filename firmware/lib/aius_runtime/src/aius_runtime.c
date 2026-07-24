#include "aius_runtime.h"

#include <stddef.h>

static const AiusRuntimeBudget runtime_budgets[] = {
	{AIUS_BOOT_CHECKPOINT_RESET_CAPTURE, 2u, 0u, 0u, 250u},
	{AIUS_BOOT_CHECKPOINT_USB_RX, 64u, 64u, 0u, 0u},
	{AIUS_BOOT_CHECKPOINT_USB_TX, 1u, 0u, 64u, 500u},
	{AIUS_BOOT_CHECKPOINT_METRICS, 1u, 0u, 0u, 0u},
	{AIUS_BOOT_CHECKPOINT_NETWORK, 1u, 0u, 0u, 0u},
	{AIUS_BOOT_CHECKPOINT_HTTP, 1u, 256u, 512u, 0u},
	{AIUS_BOOT_CHECKPOINT_UDP, 1u, 532u, 532u, 2u},
	{AIUS_BOOT_CHECKPOINT_REPLAY, 1u, 2048u, 0u, 0u},
	{AIUS_BOOT_CHECKPOINT_SD, 1u, 512u, 512u, 0u},
	{AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE, 0u, 0u, 0u, 0u},
};

static bool runtime_service(
    const AiusRuntimeOps *ops,
    uint16_t checkpoint)
{
	const AiusRuntimeBudget *budget = aius_runtime_budget(checkpoint);

	ops->checkpoint(ops->context, checkpoint);
	return ops->service(ops->context, budget);
}

const AiusRuntimeBudget *aius_runtime_budget(
    uint16_t checkpoint)
{
	if (checkpoint > AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE)
		return NULL;

	return &runtime_budgets[checkpoint];
}

bool aius_runtime_epoch(
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
		     checkpoint <= AIUS_BOOT_CHECKPOINT_SD;
		     ++checkpoint) {
			if (!runtime_service(&snapshot, checkpoint))
				return false;
		}
	}

	snapshot.checkpoint(
	    snapshot.context, AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE);
	snapshot.feed_watchdog(snapshot.context);
	return true;
}
