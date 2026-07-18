# Vendored: dcadec DTS Core decoder (minimal subset)

Minimal set of `dcadec` sources needed to build the DTS **Core** decoder
(fixed-point synthesis path) for Aius capability-gate phase P3. The HD/EXSS
machinery (XLL, LBR, EXSS parsing, waveout, stream framing, the `dca_context`
driver) is intentionally excluded; the Aius adapter in
`../../lib/aius_dts/` drives the core directly (`core_parse` -> `core_filter`).

## Upstream

- Project: dcadec — https://github.com/foo86/dcadec
- Commit:  `b93deed1a231dd6dd7e39b9fe7d2abe05aa00158` (pinned)
- License: GNU Lesser General Public License v2.1 (see `COPYING.LGPLv2.1`).
  Every vendored source retains its upstream LGPL header. The `ta.c` / `ta.h`
  tree allocator carries its own permissive (ISC-style) header, retained as-is.

## Vendored files (verbatim, from upstream `libdcadec/`)

Sources (8):
- `bitstream.c`          — bit reader
- `core_decoder.c`       — DTS Core parse + filterbank (the decoder proper)
- `dmix_tables.c`        — downmix coefficient tables
- `idct_fixed.c`         — fixed-point IDCT (bit-exact transform)
- `idct_float.c`         — supplies `idct_init` only (builds the IDCT context;
                           the float transforms it also defines are unused on
                           the fixed path)
- `interpolator.c`       — `interpolator_create` / `interpolator_clear`
- `interpolator_fixed.c` — fixed-point sub-band + LFE interpolation
- `ta.c`                 — tree allocator used by the core

Headers (16):
- `bitstream.h`, `common.h`, `compiler.h`, `core_decoder.h`, `core_huffman.h`,
  `core_tables.h`, `core_vectors.h`, `dca_context.h`, `dmix_tables.h`,
  `exss_parser.h`, `fir_fixed.h`, `fixed_math.h`, `huffman.h`, `idct.h`,
  `interpolator.h`, `ta.h`

`exss_parser.h` is header-only here: `core_decoder.c` needs `struct exss_asset`
to compile, but no `exss_parser.c` function is linked (we always pass
`asset = NULL`, so `core_parse_exss` and the EXSS extension paths never run).

Total: 24 files, ~17.4k lines (dominated by the `core_vectors.h` /
`core_huffman.h` VQ + Huffman tables).

Deliberately NOT vendored: `dca_context.c`, `dca_stream.c`, `dca_frame.c`,
`dca_waveout.c`, `dcadec.c`, `dcacut.c`, `exss_parser.c`, `xll_decoder.c`,
`lbr_decoder.c`, `interpolator_float.c`, `idct_float.c`'s float transforms,
`fir_float.h`, and the `*_float.*` / `xll_*` / `lbr_*` families.

## Changelog / modifications

**Zero edits to the vendored files** — all 24 are byte-for-byte upstream. The
build accommodates them entirely from outside, in the `dts` suite of
`../../scripts/test.sh` and in the adapter `../../lib/aius_dts/src/aius_dts.c`:

1. `-D_GNU_SOURCE` — `idct_float.c` uses `M_PI`, which glibc's `<math.h>` hides
   under strict `-std=c11`. Defined for the suite compile only.
2. `-Wno-expansion-to-defined` — the upstream `AT_LEAST_GCC()` feature macro
   uses `defined` inside a macro expansion, which `-Wall` flags. Suppressed for
   the suite compile only; it is a portability nit, not a defect.
3. The adapter defines the five floating-point interpolation symbols the core
   names in dead, non-bit-exact branches (`interpolate_lfe_float_fir`,
   `..._fir_2x`, `..._iir`, `interpolate_sub32_float`, `interpolate_sub64_float`)
   as `abort()` stubs. This satisfies the link without vendoring the ~74 KB
   float FIR path; the stubs are unreachable because the adapter always sets
   `DCADEC_FLAG_CORE_BIT_EXACT`.
4. The adapter provides `dca_format_log` (normally in the excluded
   `dca_context.c`), the sink the core's `dca_log` macro targets. It runs with
   `core->ctx == NULL` and surfaces only hard errors.
