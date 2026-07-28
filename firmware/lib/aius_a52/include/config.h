/* config.h for the vendored liba52 (vendor/a52-core) — the zero-edit
 * accommodation seam. ONLY the vendored a52 sources include this file; the
 * adapter and the test sources must not. Every choice here is a recorded
 * decision (aius-ac3-support.md §Phase 1 contract; vendor/a52-core/NOTICE.md):
 *
 * - HAVE_MEMALIGN deliberately NOT defined: parse.c then allocates the sample
 *   buffer via malloc, so the aius_alloc_shim --wrap routes every liba52
 *   allocation through an AiusArena when one is armed.
 * - LIBA52_DOUBLE deliberately NOT defined: sample_t stays float (the target
 *   FPU is single-precision).
 * - fprintf is no-op'd: a52_imdct_init unconditionally fprintf's "No
 *   accelerated IMDCT transform found" to stderr on every init with no flag
 *   to silence it. <stdio.h> is included FIRST so its own fprintf declaration
 *   is compiled before the macro exists — imdct.c includes config.h before
 *   stdio, and the include guard keeps the later include inert. */

#ifndef AIUS_A52_VENDOR_CONFIG_H
#define AIUS_A52_VENDOR_CONFIG_H

#include <stdio.h>

#define fprintf(...) ((void)0)

/* liba52's sources use <inttypes.h> types; upstream's autotooled config
 * provided them the same way. */
#include <inttypes.h>

#endif /* AIUS_A52_VENDOR_CONFIG_H */
