# IEC 61937 fixture pack

Reproducible generator for synthetic 5.1 IEC 61937 fixtures. It ships no
copyrighted audio. The material exercises the channel roles of a 5.1 programme:

- `channel_id_51` has a different steady tone on FL, FR, C, LFE, SL and SR.
- `channel_impulse_51` puts a transient in each channel at a different time.
- `cinematic_program_51` combines a changing centre speech proxy, a stereo
  front bed, low-frequency effects, and quieter surround ambience.

Each item has a WAV reference and three encoded forms:

- raw DTS Core (`<name>.dts`) and its IEC 61937 stream (`<name>.iec`);
- raw 48 kHz, 5.1, 448-kbit/s AC-3 (`<name>.ac3`) and its IEC 61937 stream
  (`<name>_ac3.iec`);
- raw 48 kHz, 5.1, 384-kbit/s E-AC-3 (`<name>.eac3`) and its type-21 IEC
  61937 stream (`<name>_eac3.iec`). The type-21 burst period rides a 4x-rate
  link, so word-clock playback runs at ~1/4 real time: structural proof only.

The IEC outputs come from FFmpeg's `spdif` muxer. Generate the default
30-second pack with:

```bash
./generate.sh
```

The pack lands in `generated/`, which is **not committed** (about 66 MB, all
reproducible). Use `OUT=/path/to/output DURATION=60 ./generate.sh` for another
location or length. It needs an FFmpeg build with the `dca` encoder.

The DTS encoder is experimental in FFmpeg, so bytes can differ between
versions. Validate every generated AC-3 stream:

```bash
./validate.py generated
```

The validator checks every 6144-byte AC-3 repetition period: the IEC preamble
and length, the 1792-byte payload and zero padding, the 48 kHz 5.1 AC-3
header, and exact identity with the raw `.ac3` after undoing the IEC payload's
pairwise byte swap.

## Channel identity

**Every `join` in the generator states `map=` explicitly, and it must.** Without
it `join` guesses the input-to-output assignment by channel name; each mono
`aevalsrc` declares layout `mono`, whose single channel is named FC, so all six
inputs claim FC and the guess rotates the front three (input 0 on FC, input 1
on FL, input 2 on FR), leaving LFE and the surrounds alone. Nothing errors and
only a per-channel tone identity check sees it.

Packs generated before 2026-07-28 carry that rotation, which produced defect 30
in [`aius-network-hardening.md`](../../docs/plans/aius-network-hardening.md): a
decoded capture with FR's tone in the left output, read as a decoder channel
permutation. **Regenerate any pack older than that date**; the firmware needs
no change.
