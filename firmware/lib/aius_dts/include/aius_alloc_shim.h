#ifndef AIUS_ALLOC_SHIM_H
#define AIUS_ALLOC_SHIM_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Link-time allocation interceptor for the DTS decode path.
 *
 * Built for the host test binaries with
 *   -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * so that every malloc/calloc/realloc/free reference in the DTS adapter and the
 * vendored dcadec `ta` tree allocator is redirected here WITHOUT editing any
 * vendored source. This is how the no-heap arena path is proven: once an arena
 * is armed, decoder allocations come from that arena and never touch libc.
 *
 * Two independent concerns:
 *   - Routing: while an arena is armed (aius_alloc_shim_set_arena), new
 *     allocations are served from the arena; otherwise they fall through to the
 *     real libc allocator (__real_*). Ownership on free/realloc is decided by
 *     pointer range, so a block allocated while armed is still freed correctly
 *     after disarming, and a libc block is never handed to the arena.
 *   - Metering: every arena-served allocation increments a counter. Reset it
 *     before a decode and read it after to prove a decode allocated nothing.
 *
 * Not thread-safe: the decoder and its tests are single-threaded.
 */

/* Arm/disarm arena routing. Passing NULL disarms (new allocations use libc).
 * The first armed arena's address range is remembered for the process lifetime
 * so its blocks route to the arena on free/realloc even after disarming. */
void aius_alloc_shim_set_arena(AiusArena *arena);

/* The currently armed arena, or NULL. */
AiusArena *aius_alloc_shim_arena(void);

/* Count of arena-served allocations since the last reset. */
uint32_t aius_alloc_shim_alloc_count(void);

/* Zero the allocation counter. */
void aius_alloc_shim_reset_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_ALLOC_SHIM_H */
