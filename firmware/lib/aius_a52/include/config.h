/* config.h for the identity-only liba52 fork (lib/aius_a52/a52).
 * ONLY the forked a52 sources include this file; the
 * adapter and the test sources must not. Every choice here is a recorded
 * decision (aius-ac3-support.md; lib/aius_a52/a52/NOTICE.md):
 *
 * - HAVE_MEMALIGN deliberately NOT defined: parse.c then allocates the sample
 *   buffer via malloc, so the aius_alloc_shim --wrap routes every liba52
 *   allocation through an AiusArena when one is armed.
 * - LIBA52_DOUBLE deliberately NOT defined: sample_t stays float (the target
 *   FPU is single-precision).
 *
 * Diagnostics are owned by the forked source. config.h carries no behavioural
 * macro overrides. */

#ifndef AIUS_A52_VENDOR_CONFIG_H
#define AIUS_A52_VENDOR_CONFIG_H

/* liba52's sources use <inttypes.h> types; upstream's autotooled config
 * provided them the same way. */
#include <inttypes.h>

#endif /* AIUS_A52_VENDOR_CONFIG_H */
