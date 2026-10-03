# Aius plans

Design and delivery plans for the Aius carrier board and its `teensy41-optical`
firmware. Shipped plans are ordered by the date their work shipped; in-progress
plans by the date of their latest delivered phase.

Two documents are the entry points for "what is still open":

- [`aius-open-work-register.md`](aius-open-work-register.md): the live backlog.
  It points at authoritative homes and never duplicates their content; items are
  struck in the commit that closes them.
- [`aius-network-hardening.md` §Open defects](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking):
  the authoritative numbered defect ledger, in the order the defects are worth
  taking. Other plans cite defect numbers from it rather than restating them.

## Status

Each plan's `status:` front-matter states implementation state, not document
authority:

- `draft`: may be a completed analysis, deliberately parked
- `in-progress`
- `shipped`: still the authoritative contract record for what it built
- `superseded`, `abandoned`

Update it in the commit that ships the work; a plan still reading `in-progress`
after its phase lands sends the next reader to redo finished work.

## Shipped

- **[Aius H1 schematic capture](aius-h1-schematic.md)**, 2026-07-19. Feeds
  two-layer feasibility and layout; it authorizes no order.
  - ERC-clean KiCad 10 schematic of the carrier
  - generated from a checked-in connectivity spec
  - rendered multi-sheet SVG and PDF
- **[AIUS USB control protocol](aius-usb-control-protocol.md)**, 2026-07-31. The
  operator-facing description is
  [`../reference/usb-control-protocol.md`](../reference/usb-control-protocol.md).
  - line-oriented USB CDC maintenance and control channel
  - wire contract with safety and extensibility rules
  - firmware implementation plus host tooling
- **[Aius OTA firmware upgrade](aius-ota-upgrade.md)**, 2026-08-01.
  - HTTP-pushed firmware upgrade with watchdog fallback
  - resumable chunked staging, reused from replay uploads
  - the record that hardware A/B selection is gated behind an unblown OTP fuse
- **[Aius DTS capability gate](aius-capability-gate.md)**, 2026-08-04. The
  software-first milestone.
  - proof that a stock Teensy 4.1 parses IEC 61937 and decodes DTS Core 5.1
  - measured time and memory headroom for the later DSP and network work
- **[Aius DSP pipeline, SD editor, and configuration API](aius-dsp-config-api.md)**,
  2026-08-04. Shares one delivery sequence with
  [`aius-network-hardening.md`](aius-network-hardening.md); the two are changed
  together.
  - validated DSP config reads and writes, persisted to SD
  - edits applied at the block boundary, plus a meters endpoint
  - SD-hosted DSP editor with OpenAPI coverage
- **[Aius SD replay manager](aius-replay-manager.md)**, 2026-08-04.
  - SD-hosted replay of IEC 61937/DTS captures
  - upload, list, start, stop, loop and delete without reflashing
  - a device service that survives page navigation
- **[Aius SD-backed configuration and web assets](aius-sd-config-web.md)**,
  2026-08-04.
  - DSP configuration and web pages moved off program flash to SD
  - embedded recovery page when the card is absent or invalid
  - bounded static asset server, write authorisation and scheduling
- **[Aius OTA resilience: A/B slots and dark-window hardening](aius-ota-resilience.md)**,
  2026-08-16.
  - A/B flash slots that survive any in-window failure
  - retained dark-window forensics stamp
  - session and client hardening
- **[Aius rate-gate external capture tool](aius-rate-gate-capture-tool.md)**,
  2026-09-03.
  - board-to-U24 optical capture-and-reduce tool for rate gates
  - external-observation JSON for the sample-rate bench analyzer
- **[Aius flight recorder: debounce and rolling multi-slot buffer](aius-recorder-rolling-buffer.md)**,
  2026-09-03.
  - debounced trigger that ignores the power-on transient
  - rolling multi-slot capture bank at the same PSRAM footprint
  - multi-slot addressing over HTTP and in the egress tool
- **[Aius AC-3 decode support](aius-ac3-support.md)**, 2026-09-04 (decoder
  2026-07-29; live-TV and listening gates closed 2026-09-04).
  - AC-3 (Dolby Digital) decode over a liba52 fork, alongside DTS Core
  - decode moved off the audio ISR (defect 31)
  - replay, channel identity and 32/44.1/48 kHz integration
- **[Aius end-to-end audio latency](aius-e2e-latency.md)**, 2026-09-04.
  - causal latency measured through the physical optical loop
  - PCM bypass, PCM with DSP, DTS Core and AC-3 rows
  - board share held at or under 17 ms at 48 kHz
- **[Aius outstanding work: completion plan (2026-09)](aius-outstanding-work-2026-09.md)**,
  2026-09-04.
  - register and plan record reconciled
  - trim-cadence A/B and one-image rate acceptance re-run
  - slot-agnostic A/B posture restored, with OTA as the deploy path
  - OTA verify checks the image IVT bounds
- **[Aius DSP editor UX consolidation](aius-dsp-editor-ux-consolidation.md)**,
  2026-09-04.
  - sticky action bar with a single Apply
  - controls disabled until unlocked inline with the token
  - compact presets row and staged-versus-live labelling

## In progress

- **[Aius network and firmware recovery hardening](aius-network-hardening.md)**,
  last delivery 2026-07-26. Every phase is delivered; the status stays open for
  the defect ledger.
  - portable network supervisor, boot ledger and bounded SD jobs
  - bounded CDC recovery and a retained watchdog
  - button-free flash tooling and the live recovery smoke
- **[Aius optical-only prototype and remote validation](aius-optical-prototype.md)**,
  last delivery 2026-07-28.
  - full optical S/PDIF path with a real source and sink
  - control and telemetry over Ethernet once USB is disconnected
  - remote fixture runner
- **[Aius E-AC-3 (DD+) decode](aius-eac3-support.md)**, last delivery
  2026-08-02. Phases 1 to 4 are done; live evidence waits on a real DD+ source.
  - 5.1 E-AC-3 via an FFmpeg `ac3dec` subset, without `AVCodecContext`
  - codec-slot swap so one decoder is resident, rather than a larger arena
  - type-21 IEC 61937 parsing and dispatch
- **[Aius prototype cost model](aius-cost-model.md)**, last update 2026-09-03. A
  living model.
  - landed-cost estimate for three assembled carriers (PCB, enclosure, cable,
    freight, VAT, duty, contingency)
  - two-layer versus four-layer cost decision
  - costed BOM and quote acceptance report
- **[Aius input recovery ladder](aius-input-recovery.md)**, last delivery
  2026-09-03. Phase 1 only; phases 2 to 4 wait on field evidence.
  - observability for stuck input states (event ring, dark-interval record)
  - a level-triggered recovery rule, host-proven but not wired in
- **[Aius OTA slot-agnostic images: one build, one signature](aius-ota-slot-agnostic.md)**,
  last delivery 2026-09-03.
  - one image and one signature for either A/B slot
  - the chooser remaps the physical slot via FlexSPI
  - a single HalfKay `recovery.hex` cutover, then OTA only
- **[Aius sample-rate following](aius-sample-rate-following.md)**, last delivery
  2026-09-04.
  - output clock follows a 32, 44.1 or 48 kHz optical source, with no resampler
  - PLL4 whole-rate reconfiguration plus live `CCM_ANALOG_PLL_AUDIO_NUM` trim
  - runtime rate refusal and status observability
- **[Aius output jitter elimination](aius-output-jitter.md)**, last delivery
  2026-09-07. A listening pass is owed.
  - loop-jitter sources removed without adding latency
  - gate-open underrun counter, deterministic ring floor and latency governor
  - glitch timestamps and the AC-3/E-AC-3 syncframe CRC check
- **[Aius hardware spec](aius-hardware-spec.md)**, last update 2026-09-11. The
  single hardware source of truth.
  - buildable specification for the Teensy 4.1 carrier
  - locked architecture decisions and reset and fault behaviour
  - build gates and open hardware questions
- **[Aius H2/H3 manufacturing readiness](aius-h2-h3-manufacturing.md)**, last
  delivery 2026-09-12. The independent review and fabricator quotes are owed.
  - impedance, return-path and mechanical feasibility, with a four-layer verdict
  - footprints, placement and impedance-controlled routing
  - DRC/DFM-clean Gerber package

## Draft

- **[Aius TLS termination](aius-tls-termination.md)**: whether the firmware can
  terminate TLS with a cert and key loaded from SD, falling back to plain HTTP.
  The verdict is "feasible with caveats"; the document is **parked** and no phase
  is cleared to start.

No plan here is `superseded` or `abandoned`. Retired evidence and audit plans
were removed rather than demoted;
[`aius-hardware-spec.md`](aius-hardware-spec.md) §Provenance records which ones
and why.
