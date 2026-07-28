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
- `surround51_tones` — 5.1 with a distinct tone per channel, so no channel can
  bleed into another undetected. **The front three are rotated** — the decoded
  FL carries 600 Hz, FR 900 Hz and FC 300 Hz — see §The `join` channel trap.
- `stereo_impulse` — stereo click train (impulse every 1024 samples).
- `surround51_impulse` — 5.1 click train on every channel.
- `surround51_ident` — the channel-identity vector: FL 300, FR 600, FC 900,
  LFE 60, BL 1200, BR 1500 Hz, each tone on the channel it names. Mirrors the
  hardware bench tone programme. The `dts` suite Goertzels every decoded plane
  against this assignment, so a decoder channel permutation fails the suite.

## The `join` channel trap

`join` **guesses** the input-to-output channel assignment when `map=` is absent,
and it guesses by channel NAME. Every mono lavfi source declares layout `mono`,
whose single channel is named FC, so all six inputs claim FC: the guess satisfies
output FC from the first input and fills the rest positionally from what remains.
The front three come out rotated — input 0 to FC, input 1 to FL, input 2 to FR —
while LFE and the surrounds are untouched. Nothing errors and every channel is
present, so only a per-channel tone identity check sees it.

That is the whole of defect 30 in
[`aius-network-hardening.md`](../../../docs/plans/aius-network-hardening.md): a
hardware capture of the tone programme showed the left output carrying FR's tone,
which reads as a decoder channel permutation and was in fact this, in the source
material. Always state `map=` on a `join` whose channel identity matters.

`surround51_tones` and `surround51_impulse` keep the un-mapped recipe on purpose.
Their jobs — differential decode, the DSP goldens, the bit-exact LFE reference —
do not depend on which channel carries which tone, and regenerating them would
invalidate `../dcadec_ref/*.dcadec.s32`, which needs an upstream dcadec build to
reproduce. `surround51_ident` is the mapped one.

## Files per vector

- `<name>.dts` — raw DTS Core, 16-bit big-endian, encoded with
  `ffmpeg ... -c:a dca -strict -2`.
- `<name>.ref.s32` — FFmpeg's decode, interleaved signed 32-bit little-endian
  PCM at 48 kHz (`-f s32le -acodec pcm_s32le`), the comparison reference.

The `.dts` sync word is `0x7FFE8001`; the test splits access units on it and
feeds each raw unit straight to the decoder.
