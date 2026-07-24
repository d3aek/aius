#ifndef AIUS_RUNTIME_H
#define AIUS_RUNTIME_H

#include "aius_boot.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint16_t checkpoint;
	uint16_t max_calls;
	uint16_t max_rx_bytes;
	uint16_t max_tx_bytes;
	uint32_t service_budget_ms;
} AiusRuntimeBudget;

typedef struct {
	void *context;
	bool (*service)(
	    void *context,
	    const AiusRuntimeBudget *budget);
	void (*checkpoint)(void *context, uint16_t checkpoint);
	void (*feed_watchdog)(void *context);
} AiusRuntimeOps;

const AiusRuntimeBudget *aius_runtime_budget(
    uint16_t checkpoint);

bool aius_runtime_epoch(
    const AiusRuntimeOps *ops,
    bool recovery_safe);

#ifdef __cplusplus
}
#endif

#endif
