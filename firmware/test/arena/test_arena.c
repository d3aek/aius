#include "aius_arena.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
	static uint8_t backing[256];
	AiusArena a;

	/* init */
	assert(aius_arena_init(&a, backing, sizeof backing) == 0);
	assert(aius_arena_used(&a) == 0);
	assert(aius_arena_high_water(&a) == 0);
	assert(aius_arena_available(&a) == sizeof backing);

	/* basic alloc: first allocation starts at base */
	void *p = aius_arena_alloc(&a, 10, 1);
	assert(p == backing);
	assert(aius_arena_used(&a) == 10);

	/* alignment: next 16-aligned allocation is padded past offset 10 */
	void *q = aius_arena_alloc(&a, 4, 16);
	assert(q != NULL);
	assert(((uintptr_t)q & 15u) == 0);
	assert((uint8_t *)q >= backing + 10);
	assert(aius_arena_used(&a) == 16 + 4);

	/* high-water survives reset; used drops to 0 */
	size_t hw = aius_arena_high_water(&a);
	assert(hw == 20);
	aius_arena_reset(&a);
	assert(aius_arena_used(&a) == 0);
	assert(aius_arena_high_water(&a) == hw);

	/* exhaustion returns NULL and leaves the arena unchanged */
	void *big = aius_arena_alloc(&a, 1000, 1);
	assert(big == NULL);
	assert(aius_arena_used(&a) == 0);

	/* alignment must be a power of two */
	assert(aius_arena_alloc(&a, 4, 3) == NULL);
	assert(aius_arena_alloc(&a, 4, 0) == NULL);

	/* zero-size allocation returns a valid aligned pointer */
	void *z = aius_arena_alloc(&a, 0, 8);
	assert(z != NULL);
	assert(((uintptr_t)z & 7u) == 0);

	/* an allocation exactly filling the remainder succeeds; one more byte fails */
	aius_arena_reset(&a);
	void *full = aius_arena_alloc(&a, sizeof backing, 1);
	assert(full == backing);
	assert(aius_arena_available(&a) == 0);
	assert(aius_arena_alloc(&a, 1, 1) == NULL);

	/* invalid arguments */
	assert(aius_arena_init(NULL, backing, 10) == -1);
	assert(aius_arena_init(&a, NULL, 10) == -1);
	assert(aius_arena_alloc(NULL, 4, 1) == NULL);

	printf("arena: ok\n");
	return 0;
}
