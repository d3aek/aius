/* W8 allocator fixture.
 *
 * fx_alloc_root calls __wrap_calloc and __wrap_realloc, each of which calls a
 * stub named yield: the producer walk stops at them (W3 leaves) and passes.
 * __wrap_malloc reaches a stub named aius_sdio_wait_hook through
 * fx_malloc_helper, so W5 (2) fails on this image. fx_cyc_a and fx_cyc_b form
 * a two-function cycle under fx_cycle_root. fx_dyn_fn has a dynamic frame
 * (stack-bound refuses it). */
#define NI __attribute__((noinline, noclone, used))

volatile int fx_sink;

NI void yield(void)
{
	fx_sink++;
}

NI void aius_sdio_wait_hook(void)
{
	fx_sink += 2;
}

NI void fx_malloc_helper(void)
{
	aius_sdio_wait_hook();
	fx_sink++;
}

NI void *__wrap_malloc(unsigned n)
{
	fx_malloc_helper();
	return (void *)n;
}

NI void *__wrap_calloc(unsigned a, unsigned b)
{
	yield();
	return (void *)(a * b);
}

NI void *__wrap_realloc(void *p, unsigned n)
{
	yield();
	return (char *)p + n;
}

NI void *fx_alloc_root(unsigned n)
{
	void *p = __wrap_calloc(n, 2);
	fx_sink++;
	return (char *)__wrap_realloc(p, n) + 1;
}

NI int fx_exit_fn(int x)
{
	volatile char b[40];
	b[x & 31] = (char)x;
	return b[(x + 1) & 31];
}

NI int fx_cyc_b(int x);

NI int fx_cyc_a(int x)
{
	volatile char b[16];
	b[0] = (char)x;
	return x > 0 ? fx_cyc_b(x - 1) + b[0] : 0;
}

NI int fx_cyc_b(int x)
{
	volatile char b[32];
	b[0] = (char)x;
	return x > 0 ? fx_cyc_a(x - 2) + b[0] : fx_exit_fn(x);
}

NI int fx_cycle_root(int x)
{
	return fx_cyc_a(x) + 1;
}

NI int fx_dyn_fn(int n)
{
	volatile char b[n];
	b[0] = (char)n;
	return b[n - 1];
}

NI int fx_dyn_root(int n)
{
	return fx_dyn_fn(n) + 1;
}

NI void fx_reset(void)
{
	fx_sink = (int)(unsigned)fx_alloc_root(3) + (int)(unsigned)__wrap_malloc(4) + fx_cycle_root(fx_sink)
		+ fx_dyn_root(fx_sink);
	for (;;) {
	}
}
