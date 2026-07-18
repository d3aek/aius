/* Link-time --wrap allocation shim: route DTS decoder allocation through an
 * AiusArena and count it, without editing any vendored dcadec source. See
 * ../include/aius_alloc_shim.h for the contract. */

#include "aius_alloc_shim.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The real libc entry points, resolved by the linker's --wrap machinery. */
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);

/* max_align_t is 16 on the LP64 host and the Teensy 4.1; ta.c already assumes a
 * 16-byte malloc alignment (its MIN_ALIGN). We prepend a 16-byte cookie holding
 * the user size (for realloc/free) and hand back a 16-aligned user pointer. */
#define AIUS_SHIM_ALIGN  16u

struct shim_cookie {
    size_t user_size;
    size_t _pad; /* keeps the cookie 16 bytes so the user pointer stays 16-aligned */
};

static AiusArena *g_arena;        /* armed arena, or NULL for libc passthrough */
static uintptr_t g_owned_lo;      /* [lo, hi) address range owned by an arena */
static uintptr_t g_owned_hi;
static uint32_t g_alloc_count;

void aius_alloc_shim_set_arena(AiusArena *arena)
{
    g_arena = arena;
    if (arena && arena->base) {
        uintptr_t lo = (uintptr_t)arena->base;
        uintptr_t hi = lo + arena->capacity;
        /* Union across every arena ever armed so freeing an old block still
         * routes to the arena. Tests use a single arena, so this is exact. */
        if (g_owned_hi == 0) {
            g_owned_lo = lo;
            g_owned_hi = hi;
        } else {
            if (lo < g_owned_lo)
                g_owned_lo = lo;
            if (hi > g_owned_hi)
                g_owned_hi = hi;
        }
    }
}

AiusArena *aius_alloc_shim_arena(void)
{
    return g_arena;
}

uint32_t aius_alloc_shim_alloc_count(void)
{
    return g_alloc_count;
}

void aius_alloc_shim_reset_count(void)
{
    g_alloc_count = 0;
}

static int owned_by_arena(const void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    return p >= g_owned_lo && p < g_owned_hi;
}

/* Allocate `size` user bytes from the armed arena, prefixed by a cookie. */
static void *arena_alloc(size_t size)
{
    size_t total = size + sizeof(struct shim_cookie);
    if (total < size) /* overflow */
        return NULL;
    void *raw = aius_arena_alloc(g_arena, total, AIUS_SHIM_ALIGN);
    if (!raw)
        return NULL;
    struct shim_cookie *ck = (struct shim_cookie *)raw;
    ck->user_size = size;
    g_alloc_count++;
    return (uint8_t *)raw + sizeof(struct shim_cookie);
}

static size_t arena_user_size(const void *ptr)
{
    const struct shim_cookie *ck =
        (const struct shim_cookie *)((const uint8_t *)ptr - sizeof(struct shim_cookie));
    return ck->user_size;
}

void *__wrap_malloc(size_t size)
{
    if (g_arena)
        return arena_alloc(size);
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (g_arena) {
        size_t total = nmemb * size;
        if (nmemb != 0 && total / nmemb != size) /* overflow */
            return NULL;
        void *p = arena_alloc(total);
        if (p)
            memset(p, 0, total);
        return p;
    }
    return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (owned_by_arena(ptr)) {
        /* Arena-owned block: bump-allocate a fresh region and copy. The old
         * region is not reclaimed (a bump allocator only grows), which is fine:
         * decoder reallocation happens only during warmup, never steady state. */
        if (size == 0)
            return NULL;
        size_t old = arena_user_size(ptr);
        void *n = arena_alloc(size);
        if (!n)
            return NULL;
        memcpy(n, ptr, old < size ? old : size);
        return n;
    }
    if (g_arena && ptr == NULL)
        return arena_alloc(size);
    return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
    if (ptr == NULL)
        return;
    if (owned_by_arena(ptr))
        return; /* arena reclaims en masse via reset; individual free is a no-op */
    __real_free(ptr);
}
