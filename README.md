# Aius

Aius is a personal, non-commercial audio processor built on a Teensy 4.1. It
takes the optical S/PDIF output of a television, decodes whatever surround
format the broadcast happens to be in, folds it down to stereo through a
downmix and EQ of my own choosing, and sends it back out over optical to an
existing DAC.

The problem it solves is narrow and real: a TV will happily send you DTS, Dolby
Digital or plain stereo depending on the channel, and a stereo DAC wants none of
it. Aius sits in between and always emits clean stereo PCM.

```text
TOSLINK in → S/PDIF capture → PCM / IEC 61937 classification
  → DTS Core / AC-3 / E-AC-3 decode → downmix + EQ → gated TOSLINK out
```

The firmware runs on bench hardware today, taking a real optical source and
driving a real optical sink. Nearly all of the engineering effort is in that
firmware, so it is where the interesting parts of this repository are.

## What the firmware does

Three decoders share one resident slot: DTS Core over a vendored `dcadec`, AC-3
over a hardened liba52 fork, and E-AC-3 over an FFmpeg fork. The pipeline
classifies each burst and swaps decoders as the broadcast changes. Output
follows the source rate (32, 44.1 or 48 kHz), and the transmitter sits behind a
reset-safe gate, so a booting or confused board is silent rather than noisy.

Around the audio path the board serves a small web interface off its SD card,
accepts signed over-the-air firmware upgrades, and keeps a PSRAM flight recorder
that can be triggered to capture the audio ring when something goes wrong.

Three things shaped the code more than anything else:

- **The DSP is bit-identical between host and target.** The same portable C
  runs in host tests and on the Teensy, and the target build pins
  `-ffp-contract=off` so the ARM's fused multiply-add cannot round differently
  from the host. Every golden vector therefore means something about the
  shipped image, not just about the test machine.
- **Memory budgets are enforced at link time, not by discipline.** The linker
  script asserts an OCRAM heap floor, an ITCM ceiling and a DTCM stack floor. A
  careless addition fails the build rather than bricking a boot, which is
  exactly what it did once before those assertions existed.
- **Nothing runs unbounded on the audio path.** The runtime is a scheduler of
  bounded epochs with a per-checkpoint budget table, a fed watchdog, a retained
  boot ledger that survives reset, and a sticky recovery-safe mode.

There is roughly twice as much test code as library code, spread across host
suites that need no hardware to run. Exact figures, and the commands that
produced them, are in [project scale](docs/reference/project-scale.md).

## Start here

- [Signal path](docs/reference/signal-path.md) — the audio chain stage by
  stage, with the buffers, budgets and rounding behaviour of each.
- [User interface](docs/reference/user-interface.md) — the SD-hosted web pages
  for status, DSP editing, replay management and upgrades, with screenshots.
- [USB control protocol](docs/reference/usb-control-protocol.md), and the HTTP
  API as a committed OpenAPI document — the two
  control surfaces. HTTP is plain on port 80 with auth-gated control routes, and
  there is a UDP control socket on 4242 as well.
- [Open work register](docs/plans/aius-open-work-register.md) — the live
  backlog and defect log. Every defect carries its conviction narrative, and
  several carry retractions where the first verdict turned out to be wrong.
- [Project scale](docs/reference/project-scale.md) — how much work is in here
  and how each figure was measured.

[`firmware/README.md`](firmware/README.md) maps the code itself.
[`CONTRIBUTING.md`](CONTRIBUTING.md) covers building, testing, flashing and the
KiCad environment. Each directory under [`docs/`](docs/) carries its own index.

## Hardware, and what is next

The bench build is a socketed Teensy 4.1 wired to purchased TOSLINK modules on a
proto board ([wiring guide](docs/guides/proto-optical-wiring.md)). That is
enough to run and measure everything above, and it is what every measurement in
this repository was taken on.

A carrier board exists as a captured, ERC-clean KiCad schematic: Teensy socket,
optical in and out, the transmitter gate and a diagnostic bypass mux, native
Ethernet with a magjack, a single rear USB-C 5 V input, keyed expansion headers,
and a stock Hammond enclosure. It has not been laid out or ordered, and **no PCB
order is authorised by the current state of this repository**. Two layers is a
hypothesis rather than a decision; four happen only if a documented electrical
constraint forces it.

What remains is two-layer feasibility against a real fabricator stackup, then
layout, then a prototype order.

## Licence and reuse

This is a personal project, published so the work can be read. It vendors
third-party decoder cores under their own licences — see each `NOTICE.md` under
`firmware/vendor/`. The evidence library holds public datasheets and reference
material retained for personal research; do not republish those bytes.
