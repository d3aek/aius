#ifndef AIUS_RUNTIME_H
#define AIUS_RUNTIME_H

#include "aius_boot.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One checkpoint's PER-VISIT work allowance. Every checkpoint is visited
 * once per epoch except AUDIO, which the scheduler visits twice (both sides
 * of the SD primitive, output-jitter Phase B) -- a per-epoch reading of an
 * AUDIO quota is therefore half the true ceiling.
 *
 * UNITS, STATED BECAUSE THEY WERE ONCE ASSUMED. `max_rx_bytes` and
 * `max_tx_bytes` are BYTES MOVED OVER A TRANSPORT in one visit to the
 * checkpoint -- what may be received from, or sent to, a socket, a serial
 * endpoint or a datagram in that call. They are not a general-purpose quota and
 * they are not words.
 *
 * A consumer whose work is not transport I/O (the service checkpoint, for
 * one) should take its bound from the seam that owns it, not from here.
 *
 * `max_calls` is service calls, `service_budget_ms` a wall-clock allowance in
 * milliseconds; zero means "not bounded by this field". */
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
