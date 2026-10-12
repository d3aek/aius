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

Vectors are 0.1 s of source, which the encoder pads to complete AC-3 frames,
and are amplitude-limited to leave encoder headroom. Bitrates are pinned in
the script (640k for 5.1, 192k for differential stereo) so committed bytes do
not drift with FFmpeg's defaults.

- `stereo_tones` — L 440 Hz, R 660 Hz.
- `stereo_tones_44100` / `stereo_tones_32000` — the stereo tones at the
  other two AC-3 sample rates.
- `stereo_tones_44100_32k` — 32 kb/s CRC-boundary oracle. Its 138/140/140
  byte frame sequence distinguishes the specified word-rounded CRC1 boundary
  from a byte-level `5/8` approximation. It is decoded only for CRC acceptance,
  not compared with FFmpeg under the high-bitrate fidelity tolerance.
- `stereo_tones_44100` — the same stereo programme at 44.1 kHz.
- `stereo_tones_32000` — the same stereo programme at 32 kHz.
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
- `<name>.ref.s32` — FFmpeg's decode for differential vectors, interleaved
  signed 32-bit little-endian
  PCM at the encoded rate (`-f s32le -acodec pcm_s32le`), the comparison
  reference.

The AC-3 sync word is `0x0B77`; every AC-3 frame carries 1536 samples, so the
target emits six 256-frame blocks per access unit
([`aius-ac3-support.md`](../../../docs/plans/aius-ac3-support.md) §Design
sketch).

## Gate-5 stimulus — `gen_iec_stream.py`

Gate 5 of [`aius-sample-rate-following.md`](../../../docs/plans/aius-sample-rate-following.md)
(§Acceptance › Device, item 5) is encoded 44.1 kHz AC-3 INTO the board's
receiver: carrier, decoder and output nominal rates must agree with no
mismatch or refusal. The board only produces output, so the stimulus
is host-side and this generator produces it. It is not a corpus vector: the
output is ~23 MB for 130 s and is written to session scratch, never committed.

- `gen_iec_stream.py --rate {44100,48000} --seconds N --output X.wav` —
  encodes a tone programme with FFmpeg's `ac3` encoder (`--program tone`, a
  stereo `--tone-hz` sine at 192 kb/s, default 997 Hz; or
  `--program surround51_ident`, the corpus's 5.1 channel-identity programme at
  448 kb/s), muxes it with `-f spdif` exactly as `../latency/bench.py` does,
  and wraps the 16-bit little-endian wire words as a 2-channel 16-bit PCM WAV
  at the link rate. `--keep-intermediates` also writes `X.ac3` (raw
  elementary stream) and `X.iec` (raw spdif words) for the parser-backed
  oracles (`../../test/iec-fixtures/test_iec_fixtures.c` takes a
  48 kHz 5.1 pair). `--inspect X.wav` re-reads a stream and reports its
  burst count, data type and period. Exit codes: 1 ffmpeg failed, 2 stream
  refused (odd byte count, truncated final 6144-byte period, wrong data
  type), 3 ffmpeg missing, 4 usage.
- **Unity gain is the precondition.** IEC 61937 rides the PCM link as sample
  words, so the U24 XL must pass them untouched. It resets to 70/70 (−20 dB)
  on every USB re-enumeration, and −20 dB destroys IEC sync while passing
  every PCM level check
  ([`../source_qualification/README.md`](../source_qualification/README.md)
  §Re-verify both before EVERY measurement session). Re-apply
  `amixer -c 0 cset numid=5 110,110` before each playback and after any
  cable blink.
- **Gate-5 recipe** (board on the bench, U24 XL optical out → board RX):

  ```bash
  amixer -c 0 cset numid=5 110,110
  firmware/tools/ac3_vectors/gen_iec_stream.py --rate 44100 --seconds 130 \
      --output "$CLAUDE_SESSION_SCRATCH/gate5-ac3-44100.wav"
  aplay -D hw:0,0 "$CLAUDE_SESSION_SCRATCH/gate5-ac3-44100.wav" &
  python3 firmware/tools/net/aius_rate_bench.py run encoded --rate 44100 \
      --codec ac3 --target 192.0.2.66 \
      --evidence "$CLAUDE_SESSION_SCRATCH/gate5-ac3-44100.jsonl"
  ```

  Start `aplay` first: the bench's `encoded` scenario polls for 120 s after a
  10 s settle and requires every settled sample to report `format` 2,
  `pipeline_diag.class` 2 and `codec_pc` 1 at a live 44.1 kHz rate, so the
  130 s stream outlasts the run. Before the first 44.1 kHz run, prove the U24
  passes a bitstream at unity with a 48 kHz smoke (`--rate 48000 --seconds 10
  --program surround51_ident`) and confirm `/api/status` classifies AC-3.
  Every burst is 6144 bytes (1536 frames × 4) at either rate; a 130 s
  44.1 kHz stream is 3733 bursts.
