# IEC 61937 parser test vectors

Deterministic known-answer corpus for the IEC 61937 burst-parser host test
(`../../test/iec61937/`). Each vector is a wire word stream plus, where a burst
completes, the exact payload bytes the parser must de-frame, so the test can
assert both the typed result and the byte-for-byte payload.

Regenerate with `./generate.sh` (needs only a C compiler; no ffmpeg). The C
generator `gen_iec_corpus.c` is the authoritative record of how each vector is
built; `vectors/manifest.txt` is the human-readable summary it emits, and the
per-vector detail below is a pointer, not a second source of truth.

## Wire format on disk

- `<name>.iec` — the burst as a sequence of 16-bit words, each serialized
  **big-endian** on disk (high byte first). The stored word values are exactly
  what the parser receives: little-endian-wire vectors store the byte-swapped
  preamble/payload words, and the parser auto-detects the byte order from Pa.
- `<name>.pay` — the expected canonical big-endian payload bytes (absent for
  vectors that never complete a burst; zero-length for a null-data burst).

Real DTS Core payloads are the first access unit (sync `0x7FFE8001` to the next
sync) extracted from the committed `../dts_vectors` corpus. The real AC-3
payload is the first `surround51_ident` frame from `../ac3_vectors`; its length
comes from `fscod`/`frmsizecod`, never a search for a second sync word. The
real E-AC-3 payload is the first `surround51_ident` frame from
`../eac3_vectors`; its length comes from the 11-bit `frmsiz` field,
`(frmsiz + 1)` words.

## Vectors

- `dts_be16` — real stereo DTS Core AU, 16-bit big-endian wire, type 13 -> `AIUS_OK`.
- `dts_le16` — same AU, byte-swapped little-endian wire -> `AIUS_OK`, identical payload.
- `dts_14bit` — same AU, 14-bit packing -> `AIUS_OK`, identical payload.
- `dts51_be16` — real 5.1 DTS Core AU, 16-bit big-endian -> `AIUS_OK`.
- `ac3_surround51_ident` — two real 5.1 AC-3 AUs, type 1 -> `AIUS_OK` twice;
  Pa starts are exactly 3072 words apart.
- `eac3_surround51_ident` — two real 5.1 E-AC-3 AUs, type 21 with byte-count
  Pd; Pa starts are exactly 12288 words apart (the E-AC-3 burst spans 1536
  samples on a 4x-rate link) -> `AIUS_UNSUPPORTED` twice until the E-AC-3
  plan's phase-3 dispatch takes type 21 in scope, then `AIUS_OK` twice.
- `type11`, `type12` — synthetic DTS burst types I and II -> `AIUS_OK`.
- `unsupported_eac3` — E-AC-3-shaped 1792-byte frame, data type 21 with
  byte-count Pd `0x0700` -> `AIUS_UNSUPPORTED`.
- `null_type` — data type 0, zero length -> `AIUS_UNSUPPORTED`, empty payload.
- `two_bursts` — two DTS bursts with pause stuffing between; drains as `AIUS_OK` twice.
- `padded` — pause stuffing around one burst -> `AIUS_OK`.
- `recover` — garbage and a false Pa, then a valid burst; bounded re-lock -> `AIUS_OK`.
- `error_flag` — Pc error flag set -> `AIUS_MALFORMED`.
- `zero_len` — length-code 0 on an in-scope DTS type -> `AIUS_MALFORMED`.
- `zero_len_ac3` — Pd 0 on in-scope AC-3 type 1 -> `AIUS_MALFORMED`.
- `overlong` — Pd demanding more than the parser buffer -> `AIUS_NO_SPACE`.
- `truncated` — full header, half the payload; end-of-stream -> `AIUS_TRUNCATED` via
  `aius_iec61937_finish`.

Each de-framing vector is additionally replayed under three chunkings (whole
buffer, one word at a time, and a 3-word stride) to prove word-boundary-agnostic
reassembly.
