#include "aius_arena.h"

static int aius_is_power_of_two(size_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

int aius_arena_init(AiusArena *arena, void *base, size_t capacity)
{
	if (arena == NULL)
		return -1;
	if (base == NULL && capacity != 0)
		return -1;
	arena->base = (uint8_t *)base;
	arena->capacity = capacity;
	arena->used = 0;
	arena->high_water = 0;
	return 0;
}

void *aius_arena_alloc(AiusArena *arena, size_t size, size_t alignment)
{
	if (arena == NULL || !aius_is_power_of_two(alignment))
		return NULL;

	/* Round the current offset up to `alignment`. `used <= capacity` is an
	 * invariant, so `capacity - offset` never underflows. */
	size_t offset = arena->used;
	size_t misalign = offset & (alignment - 1);
	size_t padding = misalign ? (alignment - misalign) : 0;
	if (padding > arena->capacity - offset)
		return NULL;
	offset += padding;

	if (size > arena->capacity - offset)
		return NULL;

	void *p = arena->base + offset;
	arena->used = offset + size;
	if (arena->used > arena->high_water)
		arena->high_water = arena->used;
	return p;
}

void aius_arena_reset(AiusArena *arena)
{
	if (arena == NULL)
		return;
	arena->used = 0;
}

size_t aius_arena_used(const AiusArena *arena)
{
	return arena->used;
}

size_t aius_arena_high_water(const AiusArena *arena)
{
	return arena->high_water;
}

size_t aius_arena_available(const AiusArena *arena)
{
	return arena->capacity - arena->used;
}
