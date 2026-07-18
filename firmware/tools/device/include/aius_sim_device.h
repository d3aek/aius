#ifndef AIUS_SIM_DEVICE_H
#define AIUS_SIM_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"
#include "aius_cap_device.h"
#include "aius_capability.h"
#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In-process simulated capability device (capability-gate phase P5).
 *
 * Wraps the portable device handler behind a byte pipe: the host runner writes
 * command bytes in and reads response bytes out, with no real USB. The device's
 * framing, decode, and response codec are exactly the code that runs on the
 * board at P6; only the transport (this byte pipe vs USB-Serial) differs.
 *
 * The response queue grows on the host heap — the simulator is a host-only test
 * harness, not target code. The device handler it drives stays no-heap.
 */

typedef struct {
	uint8_t *data;
	size_t   len;   /* bytes written */
	size_t   cap;   /* allocated */
	size_t   read;  /* read cursor */
} AiusSimQueue;

typedef struct AiusSimDevice {
	AiusArena     arena;
	AiusCapDevice dev;
	AiusCapFramer in_framer;
	AiusSimQueue  out; /* device -> host response bytes */
} AiusSimDevice;

/* Provision the simulator. `arena_mem`/`arena_bytes` is caller-owned backing for
 * the decoder arena (size it from the P3 5.1 high-water, e.g. 96 KiB). Returns
 * the decoder create status. */
AiusStatus aius_sim_device_init(AiusSimDevice *sim, void *arena_mem, size_t arena_bytes);

void aius_sim_device_destroy(AiusSimDevice *sim);

/* Host -> device: feed command bytes. Handles partial and coalesced transfers;
 * responses are queued for reading. Returns bytes consumed (== len). */
size_t aius_sim_device_write(AiusSimDevice *sim, const uint8_t *data, size_t len);

/* Device -> host: drain up to `cap` queued response bytes. Returns bytes copied. */
size_t aius_sim_device_read(AiusSimDevice *sim, uint8_t *dst, size_t cap);

/* Queued, unread response bytes. */
size_t aius_sim_device_available(const AiusSimDevice *sim);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_SIM_DEVICE_H */
