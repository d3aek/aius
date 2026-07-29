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

### 4. Define bit operations and quantizer constants

All formerly undefined signed left shifts now operate on unsigned values before
an explicit cast restores the required signed result; the signed bit reader
still performs its deliberate arithmetic sign extension. The bottom-half
readers handle their one reachable zero-bit initial state without shifting by
32. `swab32` widens each byte before shifting, and the two signed mantissa paths
use the same unsigned-shift round-trip.

The 36 distinct quantizer constants use multiplication by 32768 instead of
shifting negative integers. `tools/a52_probes/tabcheck.c` reads the actual
tables and proves their float representations unchanged. Exponent-switch
fallthrough is explicit, and the pure-C IMDCT path no longer prints an
unconditional diagnostic.

### 5. Bound the bit reader

`a52_bitstream_set_end` stores a byte end before `a52_frame`; the internal
`a52_bitstream_set_ptr` reset deliberately preserves it. The sole word-fill
choke point compares integer addresses, substitutes a zero word without
advancing at or beyond the end, and treats an unset NULL end as already
exhausted. Accepted adapter frames are 48 kHz, so their byte lengths and the
owned scratch base are four-byte aligned; every start below the end therefore
has one complete word remaining.

The F3a host probe derives its expected end independently from the scratch base
and validated frame length, then checks it at every fill. The shipped fuzz run
observes 3,889,968 fills, including 54,922 suppressed at the bound, with zero
end mismatches and its prior 5,617/60,183 OK/reject outcome unchanged. A
seven-byte direct-core case leaves the end unset and proves that every attempted
fill is suppressed under ASan.

F3b removes the 32 KiB proof pad, leaving one `AIUS_A52_MAX_FRAME` scratch.
The same fail-fast suites preserve all counts and decoded hashes with ASan
watching that exact allocation. The probe build's arena high-water is 57,680 B;
the reproducible production configuration
(`AIUS_A52_BOUND_PROBE=0 ... --suite a52`) measures 57,664 B.

### 6. Validate both AC-3 CRC regions

`a52_frame` now validates CRC-16/ANSI (poly `0x8005`, init 0, MSB-first,
no reflection or xorout) before reading header or bitstream state. It requires
zero residue for CRC1's `[buf + 2, fs58)` region, CRC2's `[fs58, frame_end)`
region, and their chained `[buf + 2, frame_end)` span. The frame end is
converted to integer addresses and rejected when NULL, reversed, shorter than
7 B, or longer than the 3,840 B AC-3 maximum before the CRC loop reads a byte.

The test-side repair is deliberately independent from production validation.
CRC2 is appended normally; CRC1 is solved with a cached inverse 16x16 GF(2)
influence matrix because its field precedes the bytes it protects. The repaired
fuzz stream keeps mutations across both CRC regions and reaches the decoder core
on 16,690 of 50,000 random feeds, above the asserted 16,000 floor. The F3c
deterministic baseline is 5,616 OK / 60,184 rejected, 3,889,955 reader fills,
54,909 suppressions, and zero end mismatches; all four clean PCM hashes remain
unchanged.

### 7. Reset discontinuities inside the core

`a52_reset_discontinuity` is the iterator's single incomplete-AU reset. It
zeroes the 1,536-float IMDCT delay, restores `lfsr_state` to 1, and fail-closes
the reader while retaining the exponent/BAP caches that a conformant next
frame replaces. Probe counters remain cumulative.

The adapter now begins one access unit and emits six reusable 256-frame blocks.
Its output planes shrink from 36,864 B to 6,144 B; measured arena high-water is
26,992 B with the reader probe and 26,976 B in the reproducible production
configuration. The suite primes all six delay slots and dither state, then
proves explicit abandon and live-begin preemption both reproduce a fresh
decoder's block 0 bit-for-bit. A seventh block call fails without entering the
core. The deterministic fuzz run exercises 11,074 core block rejections and
proves each disarms exactly. All four PCM hashes and the F3c baseline remain
exact.

## Remaining external accommodations

`../include/config.h` keeps `sample_t` as `float`, routes the sample-buffer
allocation through `malloc` for the Aius arena shim, and carries no behavioural
macro override. The a52 suites build under the common `-Werror` plus
ASan/UBSan flags with non-recovering undefined-behaviour checks, fail-fast
shift checking, and no fork-specific warning or sanitizer carve-out.

The adapter copies an access unit into one owned `AIUS_A52_MAX_FRAME` scratch.
The core CRC gate and reader bound are the load-bearing containment mechanisms.
