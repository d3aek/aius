# Forked: liba52 AC-3 decoder, identity output only

This is the Aius-owned AC-3 core used by the adapter in `../src/`. It began as
the minimal decoder subset from a52dec/liba52 0.7.4 and is now deliberately
forked: Aius needs only the stream's native channel coding, so carrying
liba52's downmix/upmix policy and its associated state makes the decoder less
auditable without serving a caller.

The complete contract and gate findings live in
[`aius-ac3-support.md`](../../../../docs/plans/aius-ac3-support.md).

## Upstream and license

- Project: a52dec / liba52, originally by Aaron Holtzman and Michel
  Lespinasse.
- Base version: 0.7.4.
- Source archive:
  `http://deb.debian.org/debian/pool/main/a/a52dec/a52dec_0.7.4.orig.tar.gz`.
- Archive sha256:
  `a21d724ab3b3933330194353687df82c475b5dfb997513eef4c25de6c865ec33`.
- License: GNU General Public License v2; see `COPYING`. Every retained source
  preserves its upstream GPL header.
- The preserved 0.8.0 comparison snapshot is under
  `../../../../evidence/source-snapshots/A52DEC-0.8.0-UPSTREAM/`.

## Retained subset

Sources:

- `parse.c`: sync/header parse, frame and block orchestration, dynamic range.
- `bit_allocate.c`: bit allocation.
- `bitstream.c`: bit reader.
- `imdct.c`: IMDCT and its generated tables.

Headers:

- `a52.h`, `a52_internal.h`, `bitstream.h`, `tables.h`, `mm_accel.h`.

`COPYING` and this divergence log complete the tree.

## Deliberate deletions

- `downmix.c`: removed. The adapter always requests the stream's own coding;
  the fork rejects any non-identity channel request before block decode.
- `attributes.h`: removed because nothing in this subset includes it.

`mm_accel.h` remains: `imdct.c` still includes it for the optional
`LIBA52_DJBFFT` branch. The Aius build selects the pure-C path with
`a52_init(0)`.

## Divergence from 0.7.4

### 1. Bound `baptab` indexing

`bit_allocate.c` routes all five upstream `(baptab+156)[...]` accesses
through `aius_baptab_lookup`. The upstream minimum-index claim is wrong for
`floorcod == 7`: the computed index reaches four bytes before the table.
Clamping to the table's constant edge regions is equivalent to extending
those regions and leaves conformant output unchanged. `a52-fuzz` found the
original global-buffer-overflow on its first run; the defect remains in
upstream 0.8.0.

### 2. Initialize decoder state

`parse.c:a52_init` zeroes the state before assigning the sample buffer.
Upstream accepts a malformed first block with `EXP_REUSE`; without
initialization, exponent data can index outside the 25-entry factor table.
Zero initialization makes that malformed case bounded and deterministic.
`a52-fuzz` found this defect on its second run; upstream 0.8.0 also retains
it.

### 3. Reduce output to the identity contract

The adapter's public contract has one channel policy: decode the stream's own
coding, optionally including its LFE plane. The fork reifies that contract:

- `a52_frame` consumes `dsurmod`, `cmixlev`, and `surmixlev` exactly as
  upstream does, but stores only the post-`dsurmod` coding. The value-unused
  mix-level reads remain because deleting them would desynchronize every
  following bit.
- A requested channel coding different from that post-`dsurmod` coding
  returns failure. The existing conditional LFE grant remains.
- `clev`, `slev`, and `downmixed` state are removed.
- Every coded channel receives the same dynamic-range coefficient and follows
  its own block-switch IMDCT path. No channel is mixed, widened, or reordered
  inside the core.

The native suite pins this reduction with four byte-exact whole-vector hashes
and direct core checks for `A52_DOLBY` negotiation and non-identity rejection.

## Remaining external accommodations

`../include/config.h` keeps `sample_t` as `float`, routes the sample-buffer
allocation through `malloc` for the Aius arena shim, and currently suppresses
liba52's unconditional IMDCT `fprintf`. `../../../scripts/test.sh` scopes the
remaining upstream warning and sanitizer carve-outs to the a52 build.

The bit reader is still unbounded in this checkpoint. The adapter therefore
copies an access unit into an owned `AIUS_A52_MAX_FRAME` plus 32 KiB padded
scratch before calling the core. Fork steps F2 and F3 remove the sanitizer
carve-outs, add a byte-exact reader bound, and then remove that pad; until
those commits land, the pad remains the active containment mechanism.
