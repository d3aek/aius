/* W8 indirect fixture: fx_ind_root makes one function-pointer call whose
 * targets (fx_tgt_small, fx_tgt_big) are address-taken by this object, and
 * calls fx_jump_table, whose ldr pc dispatch reads an inline table of entries
 * inside itself (intra-function, not a W4 site). */
#define NI __attribute__((noinline, noclone, used))

volatile int fx_sink;

NI int fx_tgt_small(int x)
{
	return x + 1;
}

NI int fx_tgt_big(int x)
{
	volatile char b[300];
	b[x & 255] = (char)x;
	return b[(x + 3) & 255];
}

int (*const fx_ops[2])(int) = { fx_tgt_small, fx_tgt_big };

__attribute__((naked, noinline, used)) int fx_jump_table(int i)
{
	__asm__ volatile(
		"	and r0, r0, #1\n"
		"	adr r1, 1f\n"
		"	ldr pc, [r1, r0, lsl #2]\n"
		"	.align 2\n"
		"1:	.word 2f + 1\n"
		"	.word 3f + 1\n"
		"2:	movs r0, #1\n"
		"	bx lr\n"
		"3:	movs r0, #2\n"
		"	bx lr\n");
}

NI int fx_ind_root(int x)
{
	int r = fx_ops[x & 1](x);
	return r + fx_jump_table(x) + 1;
}

NI void fx_reset(void)
{
	fx_sink = fx_ind_root(fx_sink);
	for (;;) {
	}
}
