# Aius plans

Design and delivery plans for the Aius carrier board and its `teensy41-optical`
firmware. Each plan's `status:` front-matter states **implementation state**, not
document authority — a `shipped` plan is still the authoritative contract record
for what it built, and a `draft` plan may be a completed analysis that is
deliberately parked.

Two documents are the entry points for "what is still open":

- [`aius-open-work-register.md`](aius-open-work-register.md) — the live backlog.
  It points at authoritative homes and never duplicates their content; items are
  struck in the commit that closes them.
- [`aius-network-hardening.md`](aius-network-hardening.md) — carries the
  authoritative numbered defect ledger (§Open defects, in the order they are
  worth taking). Other plans cite defect numbers from it rather than restating
  them.

## In progress

- **[Aius AC-3 decode support](aius-ac3-support.md)** — decode AC-3 (Dolby
  Digital) alongside DTS Core, with the defect-31 restructure that moves decode
  off the audio ISR folded in.
- **[Aius prototype cost model](aius-cost-model.md)** — should-cost estimate for
  three assembled carrier boards, covering PCB, enclosure, cable, freight, VAT,
  duty and contingency.
- **[Aius end-to-end audio latency](aius-e2e-latency.md)** — measure causal
  latency through the physical optical loop for PCM bypass, PCM with production
  DSP, DTS Core decode, and AC-3 decode.
- **[Aius E-AC-3 (DD+) decode](aius-eac3-support.md)** — the last decoder: 5.1
  E-AC-3 via the FFmpeg `ac3dec` subset, driven without `AVCodecContext`, using a
  codec-slot swap rather than a larger arena.
- **[Aius hardware spec](aius-hardware-spec.md)** — the buildable specification
  for the carrier board and the single hardware source of truth.
- **[Aius H2/H3 manufacturing readiness](aius-h2-h3-manufacturing.md)** — take
  the carrier from an ERC-clean schematic to an order-ready manufacturing
  package: feasibility, footprints, layout, impedance-controlled routing,
  DRC/DFM-clean Gerbers, an independent review, and a real fabricator quote.
- **[Aius network and firmware recovery hardening](aius-network-hardening.md)** —
  replaces unproven initialization, socket-ownership, USB and watchdog recovery
  claims with explicit portable state machines and evidence from the deployed
  firmware.
- **[Aius open-work register](aius-open-work-register.md)** — every open Aius
  item in one place, as links to authoritative homes.
- **[Aius optical-only prototype and remote validation](aius-optical-prototype.md)** —
  validate the complete physical optical path with a real S/PDIF source and sink
  while keeping control and telemetry over Ethernet after USB is disconnected.
- **[Aius sample-rate following](aius-sample-rate-following.md)** — make the
  output clock follow a live 32/44.1/48 kHz optical source by PLL4
  reconfiguration and live `CCM_ANALOG_PLL_AUDIO_NUM` trim, with no resampler.

## Draft

- **[Aius TLS termination](aius-tls-termination.md)** — whether the firmware can
  terminate TLS with a cert and key loaded from SD, falling back to plain HTTP.
  The verdict is "feasible with caveats"; the document is **parked** and no phase
  is cleared to start.

## Shipped

- **[Aius DTS capability gate](aius-capability-gate.md)** — the software-first
  milestone: prove a stock Teensy 4.1 parses IEC 61937 and decodes DTS Core 5.1
  with time and memory headroom for the later downmix, EQ, control and network
  work.
- **[Aius DSP pipeline, SD editor, and configuration API](aius-dsp-config-api.md)** —
  a bounded, editable DSP model served from SD with an HTTP configuration API.
  Shares one delivery sequence with `aius-network-hardening.md`; the two are
  changed together.
- **[Aius H1 schematic capture](aius-h1-schematic.md)** — the carrier's
  electrical connectivity as an ERC-clean KiCad 10 schematic generated from a
  checked-in connectivity spec. Feeds two-layer feasibility and layout; it
  authorizes no order.
- **[Aius OTA firmware upgrade](aius-ota-upgrade.md)** — HTTP-pushed firmware
  upgrade with watchdog fallback, and the record that hardware A/B image
  selection is gated behind an unblown OTP fuse.
- **[Aius SD replay manager](aius-replay-manager.md)** — SD-hosted replay of
  IEC 61937/DTS captures as an independent device service that survives page
  navigation.
- **[Aius SD-backed configuration and web assets](aius-sd-config-web.md)** —
  move persisted DSP configuration and web assets off program flash to SD, with
  an embedded recovery page when the card is absent or invalid.
- **[AIUS USB control protocol](aius-usb-control-protocol.md)** — the contract
  for the line-oriented USB CDC control channel. The operator-facing description
  is [`../reference/usb-control-protocol.md`](../reference/usb-control-protocol.md).

No plan here is `superseded` or `abandoned`. Retired evidence and audit plans
were removed rather than demoted; `aius-hardware-spec.md` §Provenance records
which ones and why.
