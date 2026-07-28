# AC-3 test vectors

Deterministic AC-3 corpus for the host differential test that lands with the
`aius_a52` wrapper ([`aius-ac3-support.md`](../../../docs/plans/aius-ac3-support.md)
phase 1). Each vector is a raw AC-3 bitstream plus FFmpeg's own decode of it, so
the test can compare the Aius/liba52 decode against FFmpeg sample-for-sample.
Committed ahead of the decode phase so that phase starts at the contract.

Regenerate with `./generate.sh` (needs `ffmpeg` with the native `ac3` encoder).
The script is the authoritative record of the exact FFmpeg commands; the
summary below is a pointer, not a second source of truth.

## Corpus

All vectors are 48 kHz, ~0.1 s (5120 samples), amplitude-limited to leave
encoder headroom. Bitrates are pinned in the script (640k for 5.1, 192k for
stereo) so committed bytes do not drift with FFmpeg's defaults.

- `stereo_tones` — L 440 Hz, R 660 Hz.
- `stereo_impulse` — stereo click train (impulse every 1024 samples).
- `surround51_impulse` — 5.1 click train on every channel.
- `surround51_ident` — the channel-identity vector: FL 300, FR 600, FC 900,
  LFE 60, BL 1200, BR 1500 Hz, each tone on the channel it names. Mirrors the
  hardware bench tone programme and the DTS corpus's `surround51_ident`. The
  a52 suite Goertzels every decoded plane against this assignment, so a
  decoder channel permutation fails the suite.

**Every multi-channel recipe states `join=...:map=` explicitly** — the
un-mapped `join` guess is defect 30 (it silently rotated the front three
channels of the DTS corpus's legacy vectors; full anatomy in
[`../dts_vectors/generate.sh`](../dts_vectors/generate.sh) and
[`../dts_vectors/README.md`](../dts_vectors/README.md) §The `join` channel
trap). This corpus is new, so none of the rotated recipes are carried over.

## Files per vector

- `<name>.ac3` — raw AC-3, encoded with `ffmpeg ... -c:a ac3`.
- `<name>.ref.s32` — FFmpeg's decode, interleaved signed 32-bit little-endian
  PCM at 48 kHz (`-f s32le -acodec pcm_s32le`), the comparison reference.

The AC-3 sync word is `0x0B77`; a 48 kHz frame carries 1536 samples, so the
target emits three 512-frame blocks per access unit
([`aius-ac3-support.md`](../../../docs/plans/aius-ac3-support.md) §Design
sketch).
