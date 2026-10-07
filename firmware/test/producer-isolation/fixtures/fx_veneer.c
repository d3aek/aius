/* W8 veneer and relocation fixture (built with -g).
 *
 * Veneer: fx_flash_root (flash) calls fx_itcm_fn (ITCM) through
 * __fx_itcm_fn_veneer, and fx_itcm_fn calls a stub named yield back in
 * flash. A walk that stops at the veneer never sees yield.
 *
 * Relocations: fx_itcm_victim sits at ITCM address 0, so the data word 1 in
 * fx_data_words equals its Thumb address with no relocation against it; its
 * only R_ARM_ABS32 is the .debug_frame FDE's (non-loaded). fx_taken_target is
 * genuinely address-taken through fx_taken_ptr (the positive control). */
#define NI __attribute__((noinline, noclone, used))
#define ITCM_FIRST __attribute__((section(".itcm.first"), noinline, noclone, used))
#define ITCM __attribute__((section(".itcm.text"), noinline, noclone, used))

volatile int fx_sink;

void yield(void);

ITCM_FIRST int fx_itcm_victim(int x)
{
	return x * 3 + fx_sink;
}

ITCM int fx_itcm_fn(int x)
{
	volatile char buf[200];
	buf[x & 127] = (char)x;
	yield();
	return buf[(x + 1) & 127];
}

NI void yield(void)
{
	fx_sink++;
}

NI int fx_flash_root(int x)
{
	volatile char b[24];
	b[0] = (char)x;
	return fx_itcm_fn(b[0]) + 1;
}

NI int fx_taken_target(int x)
{
	return x + 7;
}

const unsigned int fx_data_words[2] = { 1u, 0x12345678u };
int (*const fx_taken_ptr)(int) = fx_taken_target;

NI void fx_reset(void)
{
	fx_sink = fx_flash_root(fx_sink) + fx_itcm_victim(fx_sink) + (int)fx_data_words[fx_sink & 1]
		+ fx_taken_ptr(fx_sink);
	for (;;) {
	}
}
