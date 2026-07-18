# DTS Core test vectors

Deterministic DTS Core corpus for the host differential test
(`../../test/dts/`). Each vector is a raw DTS Core bitstream plus FFmpeg's own
decode of it, so the test can compare the Aius/dcadec decode against FFmpeg
sample-for-sample.

Regenerate with `./generate.sh` (needs `ffmpeg` with the experimental `dca`
encoder). The script is the authoritative record of the exact FFmpeg commands;
the summary below is a pointer, not a second source of truth.

## Corpus

All vectors are 48 kHz, ~0.1 s (5120 samples), amplitude-limited to leave
encoder headroom.

- `stereo_tones` — L 440 Hz, R 660 Hz.
- `surround51_tones` — 5.1 with a distinct tone per channel (FL 300, FR 600,
  FC 900, LFE 60, BL 1200, BR 1500 Hz); distinct tones also verify no
  cross-channel bleed and correct channel ordering.
- `stereo_impulse` — stereo click train (impulse every 1024 samples).
- `surround51_impulse` — 5.1 click train on every channel.

## Files per vector

- `<name>.dts` — raw DTS Core, 16-bit big-endian, encoded with
  `ffmpeg ... -c:a dca -strict -2`.
- `<name>.ref.s32` — FFmpeg's decode, interleaved signed 32-bit little-endian
  PCM at 48 kHz (`-f s32le -acodec pcm_s32le`), the comparison reference.

The `.dts` sync word is `0x7FFE8001`; the test splits access units on it and
feeds each raw unit straight to the decoder.
