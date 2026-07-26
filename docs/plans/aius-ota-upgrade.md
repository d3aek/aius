---
title: Aius OTA firmware upgrade
type: plans
scope: component
component: aius
status: draft
subtype: design
tags: [aius, firmware, teensy, ota, http, security, recovery]
updated: 2026-07-26
toc: true
---

# Aius OTA firmware upgrade

Whether the optical firmware can be upgraded by an HTTP push, what an A/B scheme
with watchdog fallback would look like on this part, and whether the network
surface is observable enough to make that safe. Investigated 2026-07-26 at the
owner's request.

**This document is incomplete in one specific and important way.** The transport,
rollback-substrate, observability and security questions are answered below with
citations. **The central feasibility question — whether the Teensy 4.1 can safely
self-program its own FLASH — was not reached** and is stated as open in
[§The unanswered question](#the-unanswered-question). No recommendation to build
should be taken from this document until that is settled, because everything else
is moot if it is not.

Companion to [`aius-network-hardening.md`](aius-network-hardening.md), whose
§Deciding principle governs, and [`firmware-io.md`](../reference/firmware-io.md).

## What this reopens

`aius-network-hardening.md` §Deferred work says:

> A dual-image application rollback scheme is not required. HalfKay plus
> watchdog recovery-safe mode is the recovery boundary.

That bullet is the entire statement in place; its supporting reasoning is
distributed — the Program button always reaches HalfKay regardless of firmware
state, CDC is the recovery control plane and a running or watchdog-recovered image
must accept `AIUS BOOTLOADER` without a button press, and the normative contract
requires reaching normal service or USB-recoverable safe mode without physical
intervention.

**Nothing technical has changed to invalidate that reasoning.** The owner wants
remote upgrade, which is a new requirement rather than a correction. Two
observations do weaken the original boundary slightly, and both are worth stating:

- **The boundary's own reliability rests on a mechanism whose sibling was broken.**
  `AIUS BOOTLOADER` entry over CDC was measured reliable — five consecutive trials
  with a reflash between each — but only after a fix for a buffered-treated-as-
  transmitted race. `POST /api/reboot` has the *same* defect class and it is still
  live (§Observability below). The boundary works; a neighbouring path built on the
  same assumption did not.
- **Recovery-safe mode, the second half of the boundary, has never executed on
  hardware.** Its proof is B6 gate 3, which is blocked behind gate 2, and the
  retained-OCRAM survival that latches the mode is explicitly not to be claimed
  until B6 confirms it. So half the stated recovery boundary is unvalidated.

## The unanswered question

**Can the Teensy 4.1 safely self-program its own FLASH, and is there any hardware
boot-image selection?**

Not established. This is the question the whole design turns on, and it needs:

- Whether `FlasherX`-style self-flashing (buffer an image in upper FLASH, then a
  small resident routine copies it down and reboots) is sound here — what must be
  resident, in which bank, and what happens on power loss mid-copy.
- Which address the part boots from, and whether the FCB/IVT arrangement permits
  pointing at either of two images. **If hardware boot-image selection exists it
  changes everything below**, because the arbiter stops being the application.
- How long a FLASH write or erase stalls the CPU, against a 2.67 ms audio block
  and an 8 s watchdog.

**Until those are answered, assume A/B is not achievable and read the rest of this
document as describing the transport and safety work that any upgrade mechanism
needs regardless.**

### Why the ESP32 comparison does not carry over

On ESP32 a second-stage bootloader arbitrates between OTA partitions, validates
them and rolls back — **the arbiter sits below the application.** Teensy 4.1 has
HalfKay, which has no A/B awareness, no validation and no ability to choose
between images. So on this part **the running application would have to arbitrate
its own replacement**, which is a materially harder problem than the ESP32
precedent suggests. That asymmetry is the single most important thing to
understand before costing this.

## The transport already exists, and should be reused

This is the strongest finding: **a resumable, chunked, multi-megabyte upload with
atomic install and rollback is already implemented and running.**

- **`AIUS_HTTP_MAX_BODY` (12,288) does not gate streamed PUTs.**
  `is_streamed_request()` (`lib/aius_http/src/aius_http.c:284`) exempts
  `PUT /sd/<path>` and `PUT /sd/replays/<id>`; the buffered-body branch that
  enforces `MAX_BODY` is the `else` (`:413`). A multi-megabyte `Content-Length` is
  admissible today, capped only at `INT64_MAX`.
- **`APPEND_PART` / `FINALIZE_PART` are a resumable chunk protocol.**
  `PUT /sd/replays/<id>` with `Content-Range` appends to `<id>.part` and answers
  `{"ok":true,"next_offset":N}`; the final chunk renames to `.iec`
  (`src/aius_optical_app.cpp:534`–`:557`). **Each chunk is a self-contained
  request, so it already survives connection loss between chunks** — which is
  exactly what a multi-megabyte firmware push over one socket needs.
- **Install is atomic with rollback** via a scratch pair,
  `/aius/.upload-%08lx-%08lx.tmp` and `/aius/.backup-%08lx-%08lx.bak`
  (`src/aius_optical_app.cpp:282`).

**Do not widen the per-epoch bounds.** `AIUS_SD_MAX_TRANSFER` is 512 and
`validate_request()` refuses any job whose capacity exceeds it
(`lib/aius_sd/src/aius_sd.c:23`, `:270`) — a lesson already learned the hard way
when an 8 KB replay slice produced a completely dead replay path. The runtime
budget table records why: the SD checkpoint's 512 "is the one number here that
bounds a BLOCKING primitive".

### Measured throughput, and it is adequate

One ≤256-byte chunk spans **four epochs** (read, supply, write, advance), giving
**≈64 bytes per epoch**. At the measured ~672 epochs/s that is **≈40–45 kB/s**, so
a 2 MB image takes **roughly 45–50 seconds** of continuous streaming before any
`WOULD_BLOCK` epochs. Slow, and entirely acceptable for firmware.

Note the epoch rate measured during audio playback is lower — 7,842/s idle falls
to 7,842 under replay load in the later measurement, and the ~672 figure predates
the mount-cache fix. **Re-measure before quoting a transfer time**; the shape of
the answer (tens of seconds) is what matters here.

### One thing that must change

**`/sd/` writes are locked to the `www/` prefix.** `src/aius_optical_app.cpp:184`
requires the resource to begin literally with `www/`, and the on-card path becomes
`/aius/` + resource. A firmware image needs either a new prefix or a new route —
it cannot live under the web tree.

## The rollback substrate mostly exists

`lib/aius_boot/` is a retained-OCRAM ledger, double-slot committed with a CRC,
holding generation, last checkpoint, reset kind, mode and `early_watchdog_resets`.
The mechanics an A/B scheme needs are already there:

- **Two watchdog resets that did not reach `EPOCH_COMPLETE` select recovery-safe
  mode** (`lib/aius_boot/src/aius_boot.c:132`). That *is* "the new image failed to
  come up twice, fall back".
- **`aius_boot_mark_healthy()` clears the early-reset count**, called on the first
  completed epoch at or after 10 s past construction
  (`src/optical_main.cpp:3765`). That *is* "the new image reached healthy, commit
  to it".
- **A changed `build_id` clears both the counter and the mode**
  (`aius_boot.c:128`), which is how a new image escapes recovery-safe.

Three gaps for A/B specifically:

1. **`mark_healthy()` only clears in `NORMAL` mode** (`aius_boot.c:154`) — a board
   already latched into recovery-safe does not un-latch by surviving 10 s. Correct
   today; an A/B scheme needs to decide whether a *newly flashed* image gets a
   fresh trial from recovery-safe.
2. **There is no notion of "which slot am I running".** The ledger identifies a
   build, not a slot.
3. **Power-cycle survival is unaddressed.** Retained OCRAM survival across a
   *watchdog reset* is designed for and **explicitly not to be claimed until B6
   confirms it**; survival across a power cycle is not discussed anywhere. An A/B
   scheme probably needs the latter, and DTCM was rejected because startup clears
   it while flash/EEPROM was rejected because a checkpoint is written every epoch
   — so a *rarely written* non-volatile slot marker is a different question from
   the one already answered, and is likely fine.

## Observability: the answer is no, in specific ways

The owner's instinct is right. Assessed against what an OTA operator needs at each
step — offered, staged, verified, activated, healthy, rolled back:

| Need | Available? |
|---|---|
| Upload accepted | no — `sd_diag.job_kind`/`job_state` say a job runs, not which |
| **Upload progress** | **no** — `AiusSdJob::transferred` and `AiusHttpTransaction::upload_supplied` are both live and **neither is serialized** |
| Upload failed, and why | partly — `sd_diag.last_failure` has fault, phase, driver fault and bytes, latched only on failure |
| Image verified | no mechanism exists |
| Activated / rebooting | **broken** — see below |
| Healthy after boot | yes — `boot_generation`, `watchdog_early_resets`, `recovery_safe` |
| Rolled back | no — nothing distinguishes a rollback from an ordinary recovery-safe latch |

Three concrete defects, all recorded in the network-hardening plan:

- **`POST /api/reboot` resets but never answers**, and the mechanism is now pinned:
  dispatch retains the response un-formatted, formatting is the *next* epoch and
  the first write the one after, while the reset fires at the end of the
  dispatching epoch. **An OTA "activate" verb needs exactly this fixed**, and the
  fix is to gate the reset on the transaction reaching `AIUS_HTTP_TX_IDLE`.
- **The four `http_*` counters have no producer at all** — `put_status()` never
  writes bytes 55..70 and the JSON reads them back from those same untouched
  bytes. So the HTTP layer emits **no** per-request telemetry: no request count, no
  rejection count, no dropped-connection count.
- **No event type covers anything HTTP or upload related.** The ring's ten types
  are audio, network and config only, so an operator watching `/api/events` sees
  nothing about an upgrade.

**Cheapest high-value fix, needing no new mechanism:** serialize
`AiusSdJob::transferred` and `AiusHttpTransaction::upload_supplied` into
`sd_diag`. Both are already live.

### The CDC parity question, resolved

Parity matters in **one direction only**, and it is not the direction the question
implies.

`AIUS ID`, `AIUS AUTH` and `AIUS BOOTLOADER` work when Ethernet does not — that is
CDC's entire purpose as the recovery control plane. And **recovery-safe mode runs
only checkpoints 1 and 2** (`lib/aius_runtime/src/aius_runtime.c:81`), skipping
`NETWORK`, `HTTP`, `UDP`, `REPLAY` and `SD` entirely, so a board that has fallen
back **cannot be reached over HTTP at all.**

So: HTTP does not need parity with CDC for *recovery*, because after a failed OTA
there is no HTTP. What it needs is enough observability to make the *upgrade*
auditable while the network is still up — which is the table above. **The rollback
path is CDC's job and always will be**, and that is an argument for OTA being
safe rather than against it: the fallback channel is independent of the channel
being used to upgrade.

## Security: the current gate is not an authorization boundary

An HTTP endpoint that replaces firmware is the most dangerous surface this device
could have, and the existing gate does not carry it.

- **The token is FNV-1a-64 over the public MAC.** `derive_auth_token()`
  (`src/optical_main.cpp:3553`) hashes the three fused OCOTP MAC words with `mac0`
  repeated to fill 16 bytes, through a **compiled public salt** whose own comment
  says it "is not a secret key" (`lib/aius_auth/src/aius_auth.c:15`). FNV-1a is
  trivially computable. **Anyone who can see the MAC — ARP, DHCP, mDNS — can
  derive the token offline.** The library header says outright that it "does not
  claim cryptographic strength… it is not a password hash, MAC, or secure secret".
- The comparison is constant-time-ish by construction — all 16 positions always
  visited, no early return (`aius_auth.c:82`) — but leaks length and accepts
  upper-case hex as equivalent.
- **There is no rate limiting, lockout, delay, counter or event on an auth
  failure.** The only throttle is structural: one socket, ~672 epochs/s, four
  epochs per round trip.
- **`POST /api/reboot`, `/api/start`, `/api/stop` and `/api/clear` require no
  authentication at all.** Any host on the subnet can reset the board mid-upload.

**Recommendation: image authentication must be independent of the transport
gate.** A signature the device verifies over the complete image, checked before
anything is activated, is the only thing that makes remote firmware replacement
defensible here — the token is a physical-access bootstrap credential and should
keep that job. **And OTA should be disableable, defaulting to disabled**, enabled
by a deliberate act rather than by being reachable. Requiring a physical action to
arm it is a legitimate and probably correct answer for a device whose recovery
channel is already physical.

## Prerequisites, in order

Nothing about OTA should be built before these, and each is independently
worthwhile:

1. **Fix `POST /api/reboot`** so it answers before it resets, and **authenticate
   it** along with `/api/start`, `/api/stop`, `/api/clear`. Recorded as open
   defects; needed by any activate verb.
2. **Serialize upload progress** — `transferred` and `upload_supplied` into
   `sd_diag`. Trivial, and it is the difference between an auditable upload and a
   blind one.
3. **Run B6.** Recovery-safe mode has never executed and retained-OCRAM survival
   is unconfirmed. **An A/B scheme whose fallback rests on both would be building
   on unproven ground** — this is the strongest sequencing argument in the
   document.
4. **Answer the self-flashing question** in
   [§The unanswered question](#the-unanswered-question).

## Verdict, provisional

**Do not build OTA yet, and not because it is a bad idea.** The transport is
largely already there and the rollback substrate is most of the way there, which
makes this cheaper than it first looks. But:

- The feasibility question that decides the whole shape is unanswered.
- Half the recovery boundary it would depend on has never run on hardware.
- The activate verb it needs is broken and unauthenticated.
- The gate it would authenticate with is computable from a MAC address.

**Fix the four prerequisites above and the picture changes materially** — three of
them are small, and the third is the milestone the network-hardening plan exists
to reach anyway. Then answer the self-flashing question and decide with real
numbers.

What would change this verdict fastest: hardware boot-image selection turning out
to exist. That would move the arbiter below the application and make this look far
more like the ESP32 story the owner has in mind.

## Related documents

- [Aius network and firmware recovery hardening](aius-network-hardening.md) —
  §Deciding principle, §Deferred work, the open-defect list, and B6.
- [Aius firmware I/O reference](../reference/firmware-io.md) — the SD job engine,
  the HTTP transaction and the retained boot slots.
- [USB control protocol](../reference/usb-control-protocol.md) — the CDC recovery
  control plane and the token's intended role.
- [Aius sample-rate following](aius-sample-rate-following.md) — unrelated, but it
  shares the per-epoch bounded-step model this design must fit.
