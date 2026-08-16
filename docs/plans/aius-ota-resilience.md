---
title: Aius OTA resilience — A/B slots and dark-window hardening
type: plans
scope: component
component: aius
status: draft
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

## Open forensic thread

The health record's in-progress mark was written pre-window to injected
storage; whether it survived the HalfKay reflash (and names the reached
phase) has not been read back — worth one bench read before Phase 1 design
locks, since it may already convict or acquit the compare phase.
