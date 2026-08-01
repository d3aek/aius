# E-AC-3 test vectors

Deterministic E-AC-3 (Dolby Digital Plus) corpus for the host differential
test that lands with the `aius_eac3` adapter
([`aius-eac3-support.md`](../../../docs/plans/aius-eac3-support.md) phase 2).
Each vector is a raw E-AC-3 bitstream plus FFmpeg's own decode of it, so the
test can compare the Aius fork decode against FFmpeg sample-for-sample.

Regenerate with `./generate.sh` (needs `ffmpeg` with the native `eac3`
encoder). The script is the authoritative record of the exact FFmpeg
commands; the summary below is a pointer, not a second source of truth.

## Corpus

Vectors are 0.1 s of source, which the encoder pads to complete syncframes,
and are amplitude-limited to leave encoder headroom. Bitrates are pinned in
the script (96k mono, 192k stereo, 384k 5.1) so committed bytes do not drift
with FFmpeg's defaults. FFmpeg's `eac3` encoder emits independent substreams
only, which matches the box's 5.1-only scope (the fork consumes dependent
substreams without decoding them; that path is exercised by fuzz, not by
this corpus).

- `mono_tone` — 440 Hz on the single (FC) channel.
- `stereo_tones` — L 440 Hz, R 660 Hz.
- `stereo_tones_44100` / `stereo_tones_32000` — the stereo tones at the
  other two full sample rates.
- `surround51_impulse` — 5.1 click train on every channel.
- `surround51_ident` — the channel-identity vector: FL 300, FR 600, FC 900,
  LFE 60, BL 1200, BR 1500 Hz, each tone on the channel it names. Mirrors
  the AC-3 and DTS corpora's `surround51_ident`.

**Every multi-channel recipe states `join=...:map=` explicitly** — the
un-mapped `join` guess is defect 30 (full anatomy in
[`../dts_vectors/README.md`](../dts_vectors/README.md) §The `join` channel
trap). This corpus is new, so none of the rotated recipes are carried over.

## Files per vector

- `<name>.eac3` — raw E-AC-3, encoded with `ffmpeg ... -c:a eac3`.
- `<name>.ref.s32` — FFmpeg's decode, interleaved signed 32-bit
  little-endian PCM at the encoded rate (`-f s32le -acodec pcm_s32le`,
  demuxer pinned with `-f eac3`), the comparison reference.

The sync word is `0x0B77`; an E-AC-3 syncframe's length is
`(frmsiz + 1) * 2` bytes from the 11-bit `frmsiz` header field, and a frame
carries 1, 2, 3, or 6 audio blocks of 256 samples (these vectors all encode
6-block frames).
