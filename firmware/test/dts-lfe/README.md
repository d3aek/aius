# DTS Core LFE bit-exact test — finding

Closes the P3 caveat that the Aius DTS Core decoder's LFE channel matched the
reference only loosely. It did — against **FFmpeg**, because FFmpeg reconstructs
the 64x-decimated LFE with a different interpolation FIR. Against a full
**dcadec** build (whose core we vendored) the LFE is **bit-exact**, and this
suite proves it.

## Result

Every channel of every committed vector — LFE included — matches dcadec's own
core decode **sample-for-sample**, with no lag alignment and no amplitude
scaling (`5120/5120` samples identical per channel):

| Decoder pair | Full-band channels | LFE channel |
|---|---|---|
| Aius/dcadec core vs **FFmpeg** (`dts` suite) | bit-exact | loosely correlated (different FIR) |
| Aius/dcadec core vs **full dcadec** (`dts-lfe` suite) | bit-exact | **bit-exact** |

The LFE carries real signal in the corpus (`surround51_tones` LFE is a 60 Hz
tone, peak 563277; `surround51_impulse` LFE is a decimated click train, peak
35202), so the match is a genuine reconstruction proof, not a silent-channel
tautology.

## Why bit-exact was expected

Both decoders run the identical fixed-point core path. The Aius adapter
(`lib/aius_dts/src/aius_dts.c`) always sets
`DCADEC_FLAG_CORE_ONLY | DCADEC_FLAG_CORE_BIT_EXACT`; under
`DCADEC_FLAG_CORE_BIT_EXACT` dcadec's `core_decoder.c` selects
`interpolate_lfe_fixed_fir` for the LFE (and the fixed sub-band interpolator).
The dcadec CLI reaches the same code with `-c` (core-only) + `-b` (force
fixed-point). Same source, same commit, same flags → same bits. dcadec's core
output carries the native 24-bit range right-aligned in int32, which is exactly
what `AiusDtsDecoder` emits, so the reference needs no scaling — the only
representation difference is against FFmpeg, whose `.ref.s32` is left-aligned
full-scale (`<< 8`).

## Reference provenance

`tools/dcadec_ref/*.dcadec.s32` are committed and consumed directly by this
suite; **running the committed test needs no dcadec**. Only regenerating the
references does. See `tools/dcadec_ref/README.md` for the exact build and
invocation. In short: a full dcadec clone at upstream commit `b93deed1` (the
commit we vendored) is built with its own Makefile, then each vector is decoded
with `dcadec -c -b -m -q <vec>.dts <out>.%s.wav` (core-only, fixed-point
bit-exact, native-layout per-channel mono WAVs), and `wav_to_s32` interleaves the
mono WAVs in AIUS channel order into the `.dcadec.s32` reference.

## Running

The `dts-lfe` suite compiles like the other DTS suites — same cflags, same
decode-path source list — to a uniquely-named binary, and takes the vectors dir
plus the reference dir:

```bash
.build/native/dts-lfe tools/dts_vectors/vectors tools/dcadec_ref
```

Green under `-fsanitize=address,undefined`.
