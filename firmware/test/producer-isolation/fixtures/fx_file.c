/* W8 File-shaped fixture (CR4-B1), linked twice: plain and -DFX_SECOND_SITE.
 *
 * fx_file_write is address-taken only by the vtable _ZTV4File, reaches the
 * hook stub through fx_fatfile_write, and its only caller is the indirect
 * call in the stub named _write (the Print-slot shape). With _write's one
 * site pinned as the libc row and not followed, W5 (2) passes; the
 * FX_SECOND_SITE link adds a second, unpinned indirect site in _write and
 * fails. */
#define NI __attribute__((noinline, noclone, used))

volatile int fx_sink;

NI void aius_sdio_wait_hook(void)
{
	fx_sink += 2;
}

NI int fx_fatfile_write(int x)
{
	aius_sdio_wait_hook();
	return x + 1;
}

NI int fx_file_write(void *self, int x)
{
	(void)self;
	return fx_fatfile_write(x) + 1;
}

struct fx_vt {
	int (*write)(void *self, int x);
};

const struct fx_vt fx_file_vtable __asm__("_ZTV4File") = { fx_file_write };

struct fx_printer {
	const struct fx_vt *vt;
};

struct fx_printer *volatile fx_print;
#ifdef FX_SECOND_SITE
int (*volatile fx_extra)(int);
#endif

NI int _write(int fd, const char *buf, int n)
{
	(void)buf;
	int r = fx_print->vt->write(fx_print, n);
#ifdef FX_SECOND_SITE
	r += fx_extra(fd);
#else
	r += fd;
#endif
	return r + 1;
}

NI int fx_producer_run(int x)
{
	return x + 1;
}

NI void fx_reset(void)
{
	fx_sink = _write(1, 0, 3) + fx_producer_run(fx_sink);
	for (;;) {
	}
}
