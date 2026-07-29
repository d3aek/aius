# a52dec 0.8.0 — upstream liba52 source snapshot

Snapshot of the **current** upstream liba52 release, kept because
**the upstream endpoints are unreachable from this network** and this is
the only copy we hold. Captured 2026-07-28 during the AC-3 decoder
alternatives investigation.

## Why this exists

The active fork under `../../../firmware/lib/aius_a52/a52/` is based on
**0.7.4**,
and the record originally claimed 0.7.4 was "the current release, from
2002". That was wrong: **0.7.5 (2004-03-03)** and **0.8.0 (2022-11-05)**
both exist, the latter maintained by the Adélie Linux project. Any future
rebase-or-fork decision needs to diff against this, and neither
`git.adelielinux.org/community/a52dec` nor the VideoLAN endpoint resolves
from here.

## Provenance

- Project: a52dec / liba52, upstream maintained at
  `git.adelielinux.org/community/a52dec` (unreachable from this network).
- Version: 0.8.0, released 2022-11-05.
- Obtained from a MacPorts distfiles mirror as `a52dec-0.8.0.tar.gz`,
  sha256 `03c181ce9c3fe0d2f5130de18dab9bd8bc63c354071515aa56983c74a9cffcc9`.
- License: GPL-2 (see the headers in each file).

## What is here

The decoder subset only — the 11 upstream files from which the active fork
started. A diff against `../../../firmware/lib/aius_a52/a52/` now shows the
fork's deliberate deletions and reductions. Autotools scaffolding, `libao`
output drivers and the `a52dec` player are excluded.

Sources: `parse.c`, `bit_allocate.c`, `bitstream.c`, `imdct.c`,
`downmix.c`. Headers: `a52_internal.h`, `bitstream.h`, `tables.h` (from
`liba52/`), `a52.h`, `attributes.h`, `mm_accel.h` (from `include/`).

## What 0.8.0 changes, relative to our 0.7.4 base

Diffed 2026-07-28; the analysis lives in
[`aius-ac3-support.md`](../../../docs/plans/aius-ac3-support.md) §Decoder
alternatives.

- **Fixes the `samples + 1027` typo** (`downmix.c`, should be `+ 1024`)
  that our adapter currently dodges by always requesting identity output.
- `bitstream_init` zeroes `current_word`, removing an uninitialised read.
- `do_bit_alloc = -1` on a `uint8_t` corrected in three places.
- `state->clev = state->slev = 0` added per frame.
- **Dither level changes 0.707 → 0.75** (`dither_gen` returns
  `(3*nstate)>>2`) — this moves decoder output, so adopting it forces a
  differential re-baseline.
- Adds a fixed-point mode (`LIBA52_FIXED`), unused on an FPU part.
- API-compatible for our configuration: `level_t == sample_t == float` in
  float mode.

**Both memory-safety defects we found are still present in 0.8.0.** Its
`bit_allocate.c` still indexes `(baptab+156)[...]` unclamped at all five
sites, `a52_init` still has no `memset`, and `bitstream.h` is
byte-identical to 0.7.4 — the reader remains unbounded. Our findings are
therefore live against maintained upstream, not archaeology.
