# AC-3 decoder probes

One-shot verification tools built during the 2026-07-28 adversarial gate on
the liba52 fork contract
([`aius-ac3-support.md`](../../../docs/plans/aius-ac3-support.md)
§SUPERSEDED — the fork). Kept because they are the measurement instruments
the fork's steps depend on, and re-deriving them is pure waste.

These are **probes, not suites** — they are not registered in
`scripts/test.sh` and the `registry` suite does not know about them. Build
them ad hoc against the decoder subset (the same include paths and sources
the `a52` arm uses; the probes need `-Ilib/aius_a52/include`,
`-I<decoder tree>`, and the adapter plus its sources).

## `hash_a52.c` — the byte-identity oracle

FNV-1a over every decoded plane across the whole corpus. **This is what
fork step F0 turns into an asserted suite check**, so the reduction in F1
has an oracle that can actually see a few LSB of drift — the shipped `a52`
suite asserts only a max per-sample LSB tolerance and an SNR printed to
0.1 dB, neither of which moves on small non-peak drift.

The construction is load-bearing and must be reproduced exactly or the
baselines are meaningless: 64-bit FNV-1a, canonical basis
`14695981039346656037`, prime `1099511628211`; one hash per vector,
accumulated across every access unit in stream order; iteration
channel-major then sample; each sample taken as `(uint32_t)` and fed as
four little-endian bytes.

Baselines against the unmodified 0.7.4 vendor tree:

| Vector | FNV-1a |
|---|---|
| `stereo_tones` | `7d7d27ae1ae01351` |
| `stereo_impulse` | `e5ace9204e8da8bb` |
| `surround51_impulse` | `27ca745607a8b834` |
| `surround51_ident` | `d549a217bb66b3c6` |

## `tabcheck.c` — the quantizer-table rewrite proof

Compares `tables.h`'s `(-n << 15)` quantizer constants against the
`n * 32768` form that fork step F2 substitutes to clear 297
`-Wshift-negative-value` diagnostics. Verified **0 bit-level differences
across all 36 distinct constants**, which is what makes F2's claim of
byte-identical output safe to assert.

## Reproducing the two synthetic inputs the gate used

Neither is committed; both are one command.

**A Dolby Surround (`dsurmod = 2`) corpus variant.** The committed corpus
has no such vector, which is why the fork's A52_DOLBY flags defect could
not be convicted by the suite. Patch **byte 6 of every frame** from `0x43`
to `0x53` in the two stereo vectors (frames are 768 B in
`stereo_tones`/`stereo_impulse`); the decoder then reports `A52_DOLBY`
where it previously reported `A52_STEREO`.

**An E-AC-3 IEC 61937 stream**, the evidence behind defect 34 (type 21,
12,288-word period, Pd in BYTES unlike AC-3's bits):

```bash
ffmpeg -f lavfi -i "sine=frequency=440:duration=1:sample_rate=48000" \
  -c:a eac3 -b:a 448k eac3.tmp.eac3
ffmpeg -i eac3.tmp.eac3 -c:a copy -f spdif eac3.tmp.iec
```
