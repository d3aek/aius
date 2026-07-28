# Vendored: liba52 AC-3 decoder (minimal subset)

Minimal set of `a52dec` sources needed to build the AC-3 (Dolby Digital)
decoder for the Aius AC-3 phase
([`aius-ac3-support.md`](../../../docs/plans/aius-ac3-support.md)). The
`src/a52dec.c` player, `libao` output drivers and the autotools build are
intentionally excluded; the Aius adapter in `../../lib/aius_a52/` drives the
library directly (`a52_init` → `a52_syncinfo` → `a52_frame` → 6 × `a52_block`).

## Upstream

- Project: a52dec / liba52 — VideoLAN (originally Aaron Holtzman, Michel
  Lespinasse).
- Version: 0.7.4, the release every distribution ships. Fetched as Debian's
  untouched `a52dec_0.7.4.orig.tar.gz` from
  `http://deb.debian.org/debian/pool/main/a/a52dec/` (the VideoLAN GitLab
  archive endpoint requires authentication and `liba52.videolan.org` no
  longer resolves). sha256 of the tarball:
  `a21d724ab3b3933330194353687df82c475b5dfb997513eef4c25de6c865ec33`.
- License: GNU General Public License v2 (see `COPYING`). Every vendored
  source retains its upstream GPL header. Acceptable for this personal,
  non-commercial build (decision recorded in the plan).

## Vendored files (verbatim)

Sources (5, from upstream `liba52/`):
- `parse.c`         — sync/header parse, frame + block orchestration, dynrng
- `bit_allocate.c`  — bit allocation
- `bitstream.c`     — bit reader (UNBOUNDED — see the adapter note below)
- `imdct.c`         — IMDCT (static tables built at `a52_imdct_init`)
- `downmix.c`       — downmix/upmix mixers (identity requests bypass them)

Headers (6):
- `a52_internal.h`, `bitstream.h`, `tables.h` (from `liba52/`)
- `a52.h`, `attributes.h`, `mm_accel.h` (from `include/`; `attributes.h` is
  included by nothing in this subset but kept so the header set matches
  upstream's public include contract)

Total: 12 files including `COPYING` and this notice.

## Changelog / modifications

**TWO edits, in `bit_allocate.c` and `parse.c`; everything else is
byte-for-byte upstream.** The dcadec-core vendor dir needs no edits at all,
and both of these are documented here rather than hidden because neither
could be accommodated from outside. Both were found by `a52-fuzz` on its
first two runs, and both are memory-safety defects in stock liba52 0.7.4 —
the current release, unfixed in Debian's `a52dec_0.7.4-22` patch series.
Neither changes a conformant decode: the `a52` differential is
bit-identical before and after (worst 1906 LSB / 69.3 dB either way).

**Edit 1: a bounds clamp in `a52_bit_allocate` (`bit_allocate.c`).**
Upstream indexes `(baptab+156)[mask + 4 * exp[j]]` behind a comment claiming
`min(mask+4*exp) = -156`. That bound is wrong for `floorcod == 7`:
`COMPUTE_MASK` ends with `mask -= floor` when `mask >= 0`, so
`mask >= -floor`, and floorcod 7 is `floortab 0x1400 >> 5 = 160`. With an
exponent of 0 the index reaches **-160**, reading up to 4 bytes *before*
`baptab` — a global-buffer-overflow, found by `a52-fuzz` on its first run and
convicted by ASan. It is reachable from a **legal** bitstream (floorcod 7 is
a valid field value), not merely a corrupt one, so no input validation in the
adapter could prevent it. Clamping the index to the table's real bounds is
behaviour-identical to correctly extending its padding — the front region is
all 16 and the tail all 0 — so no conformant decode changes. All five
`(baptab+156)[...]` sites route through one `aius_baptab_lookup` helper.

**Edit 2: zero the state in `a52_init` (`parse.c`).** Upstream `malloc`s the
state and leaves it uninitialized, and nothing rejects a first block coding
`EXP_REUSE` — a stream the spec forbids but 0.7.4 accepts. `coeff_get` then
indexes `factor[exp[i]]` (25 entries) with whatever the heap held;
`a52-fuzz` caught `exp[i] == 190`. A `memset` at init bounds the class by
construction: every later `exp[]` value comes from `parse_exponents`, which
rejects anything above 24, so a malformed first-block reuse decodes as
silence instead of reading out of bounds. `endmant`/`bap`, governed the same
way, also start defined.

The build accommodates everything else from outside, in the `a52` suites of
`../../scripts/test.sh` and the adapter `../../lib/aius_a52/`:

1. The adapter supplies the `config.h` liba52 includes. It deliberately
   does NOT define `HAVE_MEMALIGN` — `parse.c` then allocates the 12,288 B
   sample buffer via `malloc`, so the `aius_alloc_shim` link-time wrap
   (`-Wl,--wrap=malloc,...`) routes every liba52 allocation (the state
   struct + the sample buffer; there are no others, and none per-frame)
   through an `AiusArena` when one is armed. It does NOT define
   `LIBA52_DOUBLE`, so `sample_t` is `float` (the target FPU is
   single-precision). It `#include <stdio.h>` first and THEN defines
   `fprintf` away as a no-op macro (`#define fprintf(...) ((void)0)`):
   `a52_imdct_init` unconditionally fprintf's "No accelerated IMDCT
   transform found" to stderr on every init and offers no flag to silence
   it. The ordering is load-bearing — `imdct.c` includes `config.h` before
   `<stdio.h>`, so defining the macro without pre-including stdio would
   mangle glibc's own `fprintf` declaration.
2. Suite-compile carve-outs (scoped to the vendor TUs, mirroring the
   dcadec-core precedent): `-Wno-unused-parameter` (unused `mm_accel`
   parameter in `a52_imdct_init`), `-Wno-implicit-fallthrough` (deliberate
   switch cascades in `parse.c` exponent decode and `downmix.c` mixers),
   `-Wno-shift-negative-value` (`tables.h`'s `(-2 << 15)` quantizer-table
   macros fire the COMPILE-time warning under `-Werror`, not just UBSan),
   `-fno-sanitize=shift` (`current_word << 32` on every bitstream re-entry
   plus signed-left-shift mantissa idioms — noise by upstream design, not
   defects), `-D_GNU_SOURCE` (`M_PI` under strict `-std=c11`, as for
   dcadec), `-lm` (`imdct.c` uses libm at table init).
3. **The bit reader is unbounded** — no end pointer, no bits-remaining
   check. Worst-case syntax consumption for one hostile frame is ~25 KiB
   (per block: `skipl` up to 511 bytes, up to 16-bit mantissas for
   5 × 253 fbw + 216 coupling + 7 LFE coefficients, exponent groups;
   × 6 blocks) against a 128-byte minimum valid 48 kHz frame. The adapter
   therefore never hands liba52 the parser's AU buffer: it copies each AU
   into a zero-filled scratch of `AIUS_A52_MAX_FRAME` (3840 B, the AC-3
   maximum frame size) + `AIUS_A52_PARSE_PAD` (32 KiB, covering the bound
   above with margin), so an overread parses zeros inside owned memory and
   surfaces as a typed parse error, never out-of-bounds.
4. Known upstream defect, dodged rather than patched: the 3F1R→3F2R upmix
   path executes `memcpy (samples + 1027, ...)` where 1024 is meant
   (`downmix.c:586`), shifting SR by 3 samples. The adapter always requests
   the stream's OWN coding (`syncinfo_flags & (A52_CHANNEL_MASK|A52_LFE)`),
   so no widening/downmix path — including this one — ever runs.
