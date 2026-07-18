#ifndef AIUS_ARENA_H
#define AIUS_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned linear (bump) allocator.
 *
 * The decoder allocates only from an arena, and only during initialization or a
 * declared stream-format change; ordinary frame decoding performs no allocation.
 * All storage is owned by the caller: the arena never calls malloc/free, so the
 * same code runs on the host and on the Teensy with statically reserved memory.
 */
typedef struct {
	uint8_t *base;       /* caller-owned backing storage */
	size_t   capacity;   /* bytes in base */
	size_t   used;       /* current bump offset (0 <= used <= capacity) */
	size_t   high_water; /* peak `used` since init; survives reset */
} AiusArena;

/* Bind an arena to caller-owned storage. `base` must be non-NULL with
 * `capacity` bytes, or NULL only when `capacity` is 0. Returns 0 on success,
 * -1 on invalid arguments. */
int aius_arena_init(AiusArena *arena, void *base, size_t capacity);

/* Allocate `size` bytes aligned to `alignment` (a power of two, >= 1). Returns a
 * pointer within the arena, or NULL if it does not fit or an argument is invalid.
 * The allocation lives until aius_arena_reset. A zero-size allocation returns a
 * valid aligned pointer and consumes only alignment padding. */
void *aius_arena_alloc(AiusArena *arena, size_t size, size_t alignment);

/* Free every allocation, retaining the backing storage and the high-water mark. */
void aius_arena_reset(AiusArena *arena);

/* Bytes currently allocated. */
size_t aius_arena_used(const AiusArena *arena);

/* Peak bytes ever allocated since init (survives reset). */
size_t aius_arena_high_water(const AiusArena *arena);

/* Bytes still available at the current bump offset (ignores alignment padding). */
size_t aius_arena_available(const AiusArena *arena);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_ARENA_H */
