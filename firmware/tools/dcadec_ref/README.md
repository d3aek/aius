# dcadec reference PCM generator

Builds a full dcadec CLI and decodes the DTS Core vectors to committed
bit-exact reference PCM for the `dts-lfe` test (`../../test/dts-lfe/`). The
references are the ground truth that the Aius decoder's LFE (and every other
channel) is proven bit-exact against.

## Files

- `build-dcadec.sh` — thin driver over the dcadec clone's own Makefile.
- `generate-reference.sh` — decodes each vector and writes `<name>.dcadec.s32`.
- `wav_to_s32.c` — interleaves dcadec's native-layout mono WAVs into one
  interleaved signed-32-bit-LE stream, in AIUS channel order.
- `*.dcadec.s32` — the committed references (interleaved int32 LE, native 24-bit
  range, AIUS channel order: FL, FR, FC, LFE, SL, SR restricted to present
  channels). The committed `dts-lfe` suite reads these directly and needs no
  dcadec.

## Regenerating

Needs a dcadec clone at the **same upstream commit we vendored** (`b93deed1`);
its `libdcadec` core is what the reference must come from.

```bash
DCADEC_SRC=/path/to/dcadec tools/dcadec_ref/generate-reference.sh
```

`DCADEC_SRC` defaults to `$CLAUDE_SESSION_SCRATCH/dcadec`. `OUT` overrides the
reference output dir (default: this directory).

## Why these flags

The Aius adapter always decodes with
`DCADEC_FLAG_CORE_ONLY | DCADEC_FLAG_CORE_BIT_EXACT`. The generator invokes the
CLI as `dcadec -c -b -m -q`:

- `-c` — core only (`DCADEC_FLAG_CORE_ONLY`).
- `-b` — force fixed-point core interpolation (`DCADEC_FLAG_CORE_BIT_EXACT`),
  which routes the LFE through `interpolate_lfe_fixed_fir` — the same FIR the
  adapter uses.
- `-m` — native-layout, one mono WAV per DTS channel named by its dcadec speaker
  name, so channel identity is unambiguous (no dca-to-WAV remap).
- `-q` — quiet.

For a raw DTS Core `.dts` (no DTS-HD container) there is no delay/padding metadata,
so dcadec applies no trimming — its per-frame output count matches the adapter's
exactly, and every sample lines up at lag 0.
