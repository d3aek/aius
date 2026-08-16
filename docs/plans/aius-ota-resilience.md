---
title: Aius OTA resilience — A/B slots and dark-window hardening
type: plans
scope: component
component: aius
status: in-progress
tags: [aius, firmware, ota, flash, reliability]
updated: 2026-08-16
toc: true
---

# Aius OTA resilience — A/B slots and dark-window hardening

Scoped after the 2026-08-16 dark-window brick (open-work register entry of the
same date): an armed, twice-verified activation acknowledged, erased boot
flash, failed somewhere inside the erase→program→compare window, and halted
into the watchdog with nothing recorded — the cold-booted CPU enumerated as
the i.MX RT ROM serial downloader (`1fc9:0135`) and recovery required the
Program button on a device whose deployed location has no USB reach. The
image was innocent (identical hex booted first try via HalfKay). The owner's
standing requirement is remote upgrade with no physical-access failure mode.

## State — resume here

**Status:** IN-PROGRESS 2026-08-16. Phases 1, 2 and 4 are IMPLEMENTED with
every host proof green (commits of 2026-08-16; each phase's as-built note
sits under its heading). The forensic thread is RESOLVED from source (below
— no bench read needed or possible). The device still runs `d5dab0618f4c`;
NOTHING NEW HAS BEEN FLASHED, so every on-device half — Phase 1's rehearsal
stamp surviving a reboot, Phase 2's activation-with-recorder-armed + soak,
Phase 4's lit-carrier bring-up row — rides the next flash/bench session.

**Next action:** Phase 3, which is BLOCKED on the owner's flash-map decision
before 3a lands (the A/B split of the 8 MiB boot flash; a concrete proposal
is queued in chat 2026-08-16). The interim no-remote-activation rule stands
until Phase 3 ships.

**Evidence recovered from session scratch before the 7-day GC:** the working
uploader was promoted as `firmware/tools/ota/aius_ota_flash.py` (Phase 4b,
corrected order + resumable faults). The staged-session fault transcript's
durable facts (chunk 500 at offset 1,454,080; post-abort clean restage in
11.6 s / 387 chunks) are in this plan and the open-work register entry.

**Decisions taken at scoping (owner, in chat):** remote upgrade without a
physical-access failure mode is the requirement ("the whole point of OTA
was a watchdog and fallback to the prior image"); the interim
no-remote-activation rule stands until Phase 3 ships.

## What is established

- **The failure is environmental to the window, not the offered image.** The
  window code that ran belonged to the then-running `197523cbab76` image,
  which had performed a successful activation the previous day.
- **The window's failure policy is halt-anonymous-and-let-WDOG-reset**
  (`ota_activate_run_and_finalize()`: on FAULTED, interrupts off, `wfi`, no
  watchdog feed, no record). By design the board resets into whatever flash
  now holds; nothing names the fault, the phase, or the failing address.
- **The PSRAM staging view sits on the marginal chip.** `ota_psram_staging`
  is placed above the 8 MiB recorder ring on a 16 MiB board — i.e. on pad 2,
  the chip the board-level investigation flagged as marginal (register,
  recorder-ring straddle note). The pre-window SHA proves one clean read of
  that region; the in-window byte-compare re-reads it under different timing.
  One marginal read in 1.58 MB fails the compare, faults the sequencer, and
  the halt policy converts a transient read error into a brick. This is the
  leading mechanism hypothesis; it also explains intermittency (yesterday
  clean, today fatal).
- **Single-slot cannot fall back once erase starts.** The recorded descope
  ("a dual-image application rollback scheme is not required; HalfKay plus
  watchdog recovery-safe mode is the recovery boundary",
  `aius-network-hardening.md` §Deferred, quoted in `aius-ota-upgrade.md`)
  held exactly as written — and its cost is physical access, which the
  deployment no longer has.

## Phases

### Phase 1 — name the next failure: retained dark-window forensics

Code + host proof landed 2026-08-16; the hardware half of the proof (a
fault-injected rehearsal's stamp read back after a reboot) waits on bench
access and rides the next bench session. As built: the sequencer gained an
optional `dark_stamp` primitive called on window entry, BEFORE every step
(so a wedged primitive names itself — the incident's exact gap), and at the
FAULTED/COMPLETED terminal; fault sites latch `fault_address` +
`fault_compare_offset` (first mismatching byte, image-absolute); the record
is `lib/aius_ota/aius_ota_dark_stamp.{h,c}` (64 B, CRC'd, layout-independent
across images), stored in the new `.aius_retained_ota` input section (last
in the retained window) and dcache-flushed per write; served as `ota_dark`
on `/api/status` and `od=state:phase:fault:hexaddr` on the `AIUS ID` line.
Pre-window refusals deliberately never stamp — a refusal must not overwrite
a prior window's evidence. Rehearsals stamp with a REHEARSAL flag.
Funding note: the ID line's `od=` outgrew the USB payload bound — widened
256→280 (frame-fit static assert added); the stamp's FASTRUN additions
outgrew ITCM — funded by FLASHMEM-annotating `aius_usb_control` (now under
the itcm-placement gate) and flash-pinning its and `http_home_page`'s
rodata.

- outcome: an in-window fault stamps phase, step address, compare offset and
  fault code into the retained-RAM window (`.aius_retained`, survives WDOG
  reset) before the halt, and both the USB `AIUS ID` line and `/api/status`
  serve it on the next boot that can speak — a field brick names itself even
  from the ROM-downloader recovery path (read after HalfKay reflash).
- seams: `ota_activate_run_and_finalize()` and the FASTRUN step path
  (`lib/aius_ota` sequencer exposes phase/address already), the retained
  layout (budgeted alongside `AiusPollProbe`), ID-line + status serializers,
  swagger.
- targeted-proof: host sequencer suite pins the stamp on every FAULTED arm;
  rehearsal on hardware (scratch window, deliberately failed compare via
  test hook) shows the stamp surviving a reboot.
- unit: parent-only

### Phase 2 — take the marginal chip out of the activation path

Code + host proof landed 2026-08-16; the bench half (a full activation with
the recorder previously armed, plus a rehearsal soak at the new placement)
rides the next bench session. As built: placement is the pure
`aius_ota_staging_place()` (`lib/aius_ota/aius_ota_staging.{h,c}`,
host-pinned for the 16/8/0 MB fitments) — pad-1 base + the 32 KiB producer
carve, capacity to the 8 MiB pad boundary, which a static assert proves
still holds a maximum image; the pad-2-only bench fitment stays on pad 2
(the only storage there is) and reports `on_pad2`. The recorder gained the
one-way `aius_recorder_park()` (any state → ABSENT, same epoch context as
the producer), called by BOTH deferred flash services before the SD→PSRAM
copy; `ota_psram_staging()` refuses a pad-1 placement while the recorder is
unparked, so mis-ordering is structurally an `ota_no_psram` refusal rather
than a copy racing a live producer. Deliberate cost, recorded: a REFUSED
activation leaves the recorder parked until the next reboot — the same
down-until-reboot posture a rehearsal already leaves audio in.

- outcome: the activation staging view never touches pad 2: the recorder is
  quiesced for the activation session and the staged image is placed in the
  pad-1 region the ring normally owns (the ring's content is expendable by
  definition once an activation is acknowledged). In-window reads therefore
  come only from the known-good chip. The pad-2 straddle risk becomes
  activation-irrelevant.
- seams: `ota_psram_staging()` placement + the recorder arm/disarm contract
  (`aius_recorder` absent/frozen states already model a missing ring);
  runtime assert that staging and any live ring never overlap.
- targeted-proof: host: staging-placement unit rows (16 MB, 8 MB, 0 MB
  boards); hardware: one full activation on the bench with the recorder
  previously armed, plus a soak of repeated rehearsals whose window content
  is staged at the new placement.
- unit: parent-only

### Phase 3 — A/B slots: survive any in-window failure

The real fix for the descoped fallback. FlexSPI NOR XIP on the RT1062 has no
ROM-level A/B for this boot medium, so the scheme is application-owned:

- **3a — first-stage chooser.** A minimal, never-erased first stage at the
  boot address: reads the health/slot record, picks slot A or B, jumps. It
  becomes the sole occupant of the protected head region; OTA never erases
  it. Its own update story is deliberately "HalfKay only".
- **3b — slot layout + dual-linked images.** XIP code is address-bound, so
  each release builds twice (slot-A base, slot-B base); the device offers
  and accepts only the image linked for its INACTIVE slot; the container
  header grows a slot identity. 8 MiB flash against a ~0.6 MiB image leaves
  ample room for two slots plus the EEPROM-emulation tail.
- **3c — commit/rollback protocol.** Activation programs the inactive slot
  (the active one is never erased: an in-window failure of any kind leaves
  the board bootable), marks the new slot boot-pending, reboots; the new
  image self-confirms healthy boot (EPOCH_COMPLETE reached, network up)
  and commits, else the chooser's trial budget falls back to the old slot.
  This subsumes the health-mark machinery `aius-ota-upgrade.md` built.
- outcome: a failed program, a corrupt image, or a non-booting image all
  degrade to "still running the old image", remotely, with no button.
- seams: linker scripts (`link/`), build (`scripts/build.sh` two-target),
  signer/container (slot identity), `lib/aius_ota` sequencer windows,
  boot-flow docs; HalfKay compatibility (a full-image reflash writes
  first-stage + slot A and resets the record).
- targeted-proof: host suite over the chooser's record states (fresh,
  trial, committed, corrupt); bench: full A→B→A upgrade cycle, then a
  deliberately corrupted offer and a mid-window power cut, both recovering
  unaided.
- unit: parent-only
- dependencies: Phase 1 (forensics first), Phase 2 (stable staging), and an
  owner decision on the flash map before 3a lands.

### Phase 4 — session and client hardening from this cycle

Shipped 2026-08-16 (host proofs green; the on-device halves ride the next
flash). As built: (a) the session absorb moved to the SD write's COMMITTED
terminal — `aius_ota_offer_check`/`aius_ota_absorb_check` (pure previews in
the core), a `chunk_check` seam verb, a dispatcher pending-chunk stash and a
new `sd_settle` hook on the transaction's SD-job responses (exactly-once at
COMPLETE/FAILED/CANCELLED or a terminal-less recycle), with a drain backstop
in the composition's settle arm; a faulted chunk now leaves the session
exactly where it was. (b) `tools/ota/aius_ota_flash.py` promoted from the
session scratch with the incident's lessons: the state machine's actual
stage→verify→arm(→activate) order, resume from the device's `next_offset`
on 500/503/507 and transport faults, and activation gated behind
`--activate` with the interim rule printed; proven against a state-machine
fake including the landed-but-unanswered chunk case. (c) the Phase C guard
gained the `g_sd_bringup_done` once-per-boot latch — the first mount is
exempt from the carrier deferral, steady-state deferral unchanged.
Memory note: the seam's 64 B of OCRAM took the ratchet to exactly the
defect-26 128 KiB floor; the next OCRAM byte must be funded.

- outcome: the smaller defects this incident surfaced are closed:
  (a) a failed staging chunk no longer poisons the session digest — absorb
  moves to SD-write completion (or the session re-digests the file tail on
  resume), so a 500 answers with a resumable, not a doomed, session;
  (b) the operator client (now `tools/ota/aius_ota_flash.py`, promoted from
  session scratch) drives stage→verify→arm→activate in the state machine's
  actual order, resumes from the device's `next_offset`, and treats
  500/503/507 as resumable;
  (c) the output-jitter Phase C guard exempts first SD bring-up
  (`g_sd_ready` latch) so a carrier lit at boot no longer starves the
  mount (register entry of 2026-08-16).
- seams: `aius_ota_absorb`/staging engine, new committed client tool,
  `sd_lifecycle_service()` guard.
- targeted-proof: host rows: chunk-fault-then-resume ends in a verifiable
  session; guard row: bring-up completes under a lit carrier, deferral
  resumes after; client: dry-run against the state machine fake.
- unit: parent-only

## Interim operating rule (until Phase 3 ships)

**No remote activation without a same-day physical-access plan.** Staging and
arming are always safe; `POST /api/ota/activate` is the commitment point. The
2026-08-16 incident is the cost baseline for ignoring this.

## Forensic thread — RESOLVED 2026-08-16, from source

**The pre-window mark categorically could not have survived; there is
nothing to read on the bench.** The deployed image's health-record
"persistence" is a plain `.bss` RAM buffer — `g_ota_health_bytes` in
`optical_main.cpp`, whose own comment records that durable storage was "a
later concern" deferred at bench bring-up. Three independent destructions,
any one sufficient: the recovery involved a cold power cycle (RAM lost);
the reflashed image's startup zeroes `.bss` regardless; and no surface ever
served the buffer anyway. The record can neither convict nor acquit the
compare phase — the incident's in-window evidence is simply gone, which is
itself the finding: it is exactly the gap Phase 1's retained stamp closes
(retained OCRAM survives the WDOG reset the halt policy takes, and a
HalfKay rescue performed without a power cycle).

Consequence recorded for Phase 3c: the health record's power-loss interlock
(activate-begin rule 8) currently only works within one powered session for
the same reason; genuinely persistent health storage lands with 3c's
commit/rollback protocol, which subsumes the health-mark machinery.
