# Vendored: FFmpeg (E-)AC-3 float decoder (minimal subset)

Minimal set of FFmpeg sources needed to build the E-AC-3 (Dolby Digital
Plus) float decoder for Aius, driven WITHOUT libavcodec's codec registry:
`shim/aius_eac3_core.c`'s exported wrappers call the (upstream-static)
decoder entry points directly, and `shim/aius_avshim.c` replaces the
framework surface the decode path actually touches (output-buffer provider,
native-mask channel-layout ops, logging sink, side-data no-ops, tx debug
stubs). The AVCodec/AVOption/threading/parser-registry machinery is
deliberately not vendored.

## Upstream

- Project: FFmpeg — https://ffmpeg.org
- Version: 7.1.5 (Debian source pool `ffmpeg_7.1.5.orig.tar.xz`,
  <https://deb.debian.org/debian/pool/main/f/ffmpeg/>, fetched 2026-08-01)
- License: GNU Lesser General Public License v2.1 or later (see
  `COPYING.LGPLv2.1`). Every vendored source retains its upstream header.
  The fork-owned files (`libavcodec/aius_eac3_core.c`, `shim/*`) are
  LGPL-2.1-or-later like the code they include.

## Layout

- `libavcodec/`, `libavutil/` — upstream sources, byte-for-byte except the
  six edited files below (verified by `cmp` against the extracted tarball
  at vendoring time).
- `shim/` — fork-owned: `config.h` / `config_components.h` replace the
  configure-generated headers (portable C paths, both decoder
  personalities, `AIUS_EAC3_51_ONLY`); `aius_avshim.{c,h}` is the framework
  replacement described above.

## Fork edits (all tagged `AIUS FORK EDIT [<id>]` in-line)

- `[tx-float-only]` — `libavutil/tx.c`: the double/int32/SIMD codelet lists
  are dropped from the dispatch array; the (E-)AC-3 float decoder is the
  only av_tx client, and each extra sample type costs its own
  multi-megabyte twiddle ladder.
- `[tx-table-ladder]` — `libavutil/tx_template.c`: the power-of-two twiddle
  ladder stops at 2048 instead of 2,097,152. Upstream's ladder is ~4.2 MB
  of link-time BSS per sample type; the decoder's transforms are
  MDCT-128/256. Whole ladder after the trim: ~2.7 KB.
- `[tx-codelet-ladder]` — `libavutil/tx_template.c`: split-radix FFT
  codelets above 1024 dropped with their tables; a transform larger than
  2048 now fails at `av_tx_init` rather than at a missing table.
- `[eac3-51-only]` — `libavcodec/ac3defs.h` (`EAC3_MAX_CHANNELS` 16 -> 7,
  -73,728 B of context), `libavcodec/ac3dec.c` (dependent substreams are
  consumed, never decoded; the extended-channel-map block is compiled out),
  `libavcodec/eac3dec.c` (the 16-bit chanmap wire width is spelled `16`
  where upstream spelled it `EAC3_MAX_CHANNELS`). Owner decision
  2026-08-01: Aius decodes the independent (5.1) substream only.
- `[input-buffer-cap]` — `libavcodec/ac3dec.h` (`AC3_FRAME_BUFFER_SIZE`
  32,768 -> 4,224: one max legal syncframe, 2048 words, plus 128 bytes so
  the following substream's header still parses for the consume path),
  `libavcodec/ac3dec.c` (`buf_size` is clamped to the cap right after the
  packet copy, keeping the frame GetBitContext, the CRC span, and the
  following-substream header parse inside the copy). -28,544 B of context.
  Upstream keeps a whole-superframe copy; independent-only decode needs
  one syncframe.

## Measurements (host x86-64, -O2, at vendoring time)

- Bit-identity: the standalone fork decodes a 29-frame 5.1 E-AC-3 vector
  byte-identically to upstream 7.1.5's own minimal build (`--disable-all
  --enable-decoder=eac3`): FNV-1a-64 `47b3267c5d527165` from both.
- Footprint: ~87 KB text + ~33 KB rodata; BSS ~38 KB (dominated by the
  32,800 B lazily-generated CRC tables) after the tx trims collapsed the
  upstream 16.8 MB table BSS.
- Working set: `sizeof(AC3DecodeContext)` = 132,960 after `[eac3-51-only]`
  and `[input-buffer-cap]` (235,232 upstream); +8,672 B of one-time init
  allocations (tx contexts, float_dsp, downmix scratch). Steady state
  allocates ZERO per frame (20 allocs / 4 frees TOTAL across a 29-frame
  decode -- the scoping estimate of 15 malloc/free per frame does not hold
  on this path). Measured adapter slot need (`eac3` suite arena pass,
  context + handle/planes + init allocs): 179,232 B high-water.
- CRC tables are HARDCODED (`CONFIG_HARDCODED_TABLES 1` in
  `shim/config.h`, a fork-owned knob rather than a source edit): crc.c
  compiles its 257-entry const tables (8,224 B rodata) instead of lazily
  generating 8 x 1024-entry tables into 32,800 B of BSS -- BSS the target
  would pay in DTCM. Marginally slower byte-wise CRC; decode
  bit-identity unaffected.

## Consumers

Built by `scripts/test.sh` (`eac3` / `eac3-fuzz` suites) under the
`aius_eac3` adapter (arena-backed, alloc-shim routed, `AiusPcmBlock` out);
target integration (FLASH-cold placement, arena growth) is E-AC-3 plan
Phase 4.
