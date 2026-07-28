---
title: Aius OTA firmware upgrade
type: plans
scope: component
component: aius
status: draft
subtype: design
tags: [aius, firmware, teensy, ota, http, security, recovery]
updated: 2026-07-28
toc: true
---

# Aius OTA firmware upgrade

Whether the optical firmware can be upgraded by an HTTP push, what an A/B scheme
with watchdog fallback would look like on this part, and whether the network
surface is observable enough to make that safe. Investigated 2026-07-26 at the
owner's request.

**The feasibility question is answered: yes, with one gating unknown.** The part
can self-program its own flash using routines Teensyduino already ships, and
hardware A/B image selection genuinely exists on the i.MX RT1062 — but it is
gated behind an irreversible OTP fuse whose state on a Teensy cannot be
determined from anything available locally. See
[§Self-flashing is possible](#self-flashing-is-possible).

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

## Self-flashing is possible

**Teensyduino already ships a working FlexSPI erase/program driver for the boot
flash**, and it is exported for exactly this kind of use. `cores/teensy4/eeprom.c`
declares, above the comment *"To be called from LittleFS_Program, any other use at
your own risk!"*:

```c
void eepromemu_flash_write(void *addr, const void *data, uint32_t len);
void eepromemu_flash_erase_sector(void *addr);       /* 0x20, 4096 B */
void eepromemu_flash_erase_32K_block(void *addr);    /* 0x52 */
void eepromemu_flash_erase_64K_block(void *addr);    /* 0xD8 */
```

All four drive **FLEXSPI instance 1**, the boot flash — distinct from `FLEXSPI2`,
which serves the QSPI expansion sockets. They unlock the LUT, overwrite sequence
15, trigger via `FLEXSPI_IPCMD`, and poll. Cache maintenance is explicit and
mandatory: `arm_dcache_delete()` before each operation, then
`FLEXSPI_MCR0 |= SWRESET` to purge the FlexSPI AHB read FIFO. Both layers matter.

**No `FlasherX` is vendored and none is needed** — nothing in the tree references
it, and the primitives it would wrap are already present.

### The Teensy 4 memory model makes this safe by default, and it inverts a project rule

**On Teensy 4, RAM execution is the default, not the opt-in.** The linker maps
`*(.text*)` into `.text.itcm > ITCM AT> FLASH`, and `startup.c` copies it to ITCM
at boot. So ordinary compiled code runs from ITCM at `0x00000000`, and `FLASHMEM`
is the opt-in to *stay* in flash. That is why the EEPROM driver needs no special
annotation to be safe: verified against the compiled object, `flash_wait`,
`eepromemu_flash_write` and the three erase functions all land in plain `.text.*`
and therefore in ITCM.

**This inverts one of this firmware's own rules for the self-flash path.**
`firmware/README.md` requires `FLASHMEM` on every cold definition, enforced by two
post-link ITCM guards — correct for ITCM budget, and **exactly backwards for an
erase/program routine**, because a `FLASHMEM` function on that path becomes
unexecutable while the flash is mid-operation. A self-flash implementation must
have its whole call graph — including `memcpy` and any CRC or verify helper —
deliberately **not** `FLASHMEM`, and must verify that post-link. The existing
guards will not catch this; they check the opposite direction.

RM p.279 states the two obligations, and note 3 is the one that bites:

> 3. The user application must ensure that there is no FLASH access from any
>    masters (for example, Cortex-M7, USB, PXP) during the FLASH erase/program…
> 4. The user application should disable the global interrupt during the FLASH
>    erase/program if the code is executing on the NOR FLASH…

The core discharges note 4 with `__disable_irq()`. **Note 3 is not discharged** —
it says *no master*, not just the M7, so any DMA or ISR touching a `FLASHMEM`
function, a `PROGMEM` string or a `const` array in `.text.progmem` during the
operation is undefined behaviour.

Two gotchas in the shipped driver, both load-bearing:

- **`flash_wait()` unconditionally calls `__enable_irq()`.** There is no
  PRIMASK save/restore, so calling any of these from an already-interrupts-disabled
  context silently re-enables interrupts on return.
- **No bounds checking whatsoever.** The address is masked to 24 bits and passed
  straight through. Erasing sector 0 destroys the FCB.

### Hardware A/B selection exists — behind an OTP fuse

Three mechanisms, all documented:

- **A fuse-defined secondary image offset.** RM p.212, Table 9-10:
  `0x6E0[23:16]` `FLASH_SEC_IMAGE_OFFSET`, where offset = 256 KiB × fuse value,
  plus `0x6E0[15:12]` for image size. Shipped value 0 for both.
- **A warm-reset-persistent selector.** RM p.209, Table 9-8:
  `PERSIST_SECONDARY_BOOT` at `SRC_GPR10[30]`, and the table **explicitly names
  FlexSPI NOR** as a user. Teensyduino deliberately does not expose `SRC_GPR1`–
  `SRC_GPR10`, commenting that they are ROM-owned.
- **The supported entry point: the ROM `runBootloader` API.** RM p.287 gives the
  worked example verbatim — `uint32_t arg = 0xeb000001; runBootloader(&arg);` to
  select the redundant image. **Teensyduino already calls this exact API**
  (`usb.c:252`) with `0xEB120000` for the serial downloader, so the mechanism is
  proven live in the core; OTA would pass `0xEB000001` instead.

**The catch: those are complementary, not alternatives.** `runBootloader`
selects *image 1*, but *where image 1 lives* comes from the OTP fuse. **With the
fuse at 0 the secondary offset is 0 — the same address as the primary.** So
hardware A/B on this part requires blowing an irreversible fuse.

**And a soft reset re-enters the ROM**, which is the good news: `SCB_AIRCR`
asserts `SYSRESETREQ`, reported as `SRC_SRSR_LOCKUP_SYSRESETREQ`, and the core
visibly cleans up after the ROM on every entry (`startup.c` undoes ROM PIT usage).
The image is entered through `ENTRY(ImageVectorTable)` at `0x60001000`, which only
the ROM parses. So an image-selection decision is genuinely honoured on the next
boot — unlike `bkpt #251`, which halts the core and never reaches the ROM.

### HalfKay cannot be bricked, which lowers the stakes considerably

**HalfKay is not in the RT1062 at all** — neither its flash nor its boot ROM. It
is a physically separate chip that also owns power sequencing and RESET, reaching
the RT1062 through its debug port. That is why `bkpt #251` works as a handoff.
Consequences, all favourable:

- **It occupies 0 bytes of the 8 MiB flash**; the whole region is application
  addressable.
- **Application code cannot damage it.**
- Application code *can* make the board un-ROM-bootable by erasing the FCB at
  `0x60000000` or the IVT at `0x60001000` — but HalfKay survives and can reflash,
  so that is **recoverable, not fatal**. Worth designing for rather than fearing.

### What remains genuinely unknown

1. **Whether PJRC blows `0x6E0[23:16]` on Teensy 4.1.** The annotated RM covers
   `BT_FUSE_SEL` and the GPIO boot pins but is silent on `BOOT_CONFIG_MISC`.
   **This is the single gating fact for hardware A/B** and needs a runtime OCOTP
   read on real hardware, or PJRC. Note `imxrt.h` stops at `HW_OCOTP_0x6C0`, so
   there is no macro for it.
2. **Whether the ROM auto-fails-over primary → secondary for FlexSPI NOR.** The
   boot-flow figure shows no validity check and no fallback branch; documented
   auto-fallback covers Serial NAND (ECC) and SD/eMMC only. **Assume
   software-initiated switching only, and own the health check and revert.**
3. **W25Q64JV erase/program timings.** No datasheet locally and no timing figures
   in any core comment, so the audio-path stall cost is unquantified. This is the
   measurement that decides whether OTA can run with audio live.
4. Whether `bkpt #251` stays usable after the application has rewritten LUT
   sequence 15, which the driver leaves holding the last opcode.

### Sizing

`_flashimagelen` is **585,728 bytes** against a usable **8,126,464**, so two
copies fit comfortably. But the OTP secondary offset is quantised to 256 KiB, so a
realistic slot B base is `0x60100000` or `0x60200000` — and **slot B needs its own
FCB at its base and its own IVT at base+0x1000**, since the ROM applies the same
4 KiB IVT offset to whichever image it selects. The `bootdata.c` structures are
hard-coded to `0x60000000` and `.text.headers` is pinned by `ORIGIN(FLASH)`, so
**a second linker script is required.**

### Why the ESP32 comparison still does not carry over

Even with hardware selection available, the ESP32 model has a bootloader that
*validates* an image and rolls back on its own. Here the ROM does neither for
FlexSPI NOR: there is no validity check and no automatic fallback. **The health
check and the revert decision remain the application's**, which is the harder half
of the problem and the part the boot ledger below has to carry.

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
3. **The ledger's address is layout-dependent, which A/B has to solve.** Measured
   2026-07-26: adding a 16,396-byte `DMAMEM` ring pushed `g_aius_boot_slots` from
   `0x20201120` to `0x20205140`, so the retained ledger no longer validated and
   `boot_generation` restarted at 1. Benign there — the CRC check creates a fresh
   ledger by design, and the changed `build_id` would have cleared the same state
   anyway. **But two A/B images will differ in `DMAMEM` layout by construction, so
   they cannot share a ledger across a switch.** An A/B design must either pin the
   slots at a fixed address independent of section layout, or carry image health
   somewhere layout-independent. This is the concrete form of the "identifies a
   build, not a slot" gap above.
4. **Power-cycle survival is unaddressed.** Retained OCRAM survival across a
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

## Design constraints from the owner (2026-07-28)

- **A/B OTA is a COMPILE-TIME option, off by default in the single-image
  build.** A future fork may want the whole 8 MiB, and the option must not
  force the split on them. The option gates everything the dual-slot design
  actually costs: the second linker script and slot-B FCB/IVT headers, the
  dual-slot flash layout, and the OTA HTTP surface. This composes with the
  §Security recommendation above (OTA disableable, defaulting to disabled) as
  two layers: compiled out entirely versus compiled in but unarmed.
- **The fuse burn does not bind forks either way**, worth stating so the
  irreversibility is not overweighted: `FLASH_SEC_IMAGE_OFFSET` only tells
  the ROM where to look IF the secondary image is ever requested. A
  single-image build spanning the full 8 MiB runs identically on a blown or
  unblown board — it simply never invokes the selector, and nothing at the
  secondary offset needs to be valid. The burn constrains a board only when
  the A/B build asks it to.

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
4. **Read the OTP fuse on real hardware** to settle whether hardware A/B is
   available, and **measure a sector-erase stall** against the audio block. Those
   are the two remaining unknowns in
   [§Self-flashing is possible](#self-flashing-is-possible), and both need the
   device rather than more reading.

## Verdict, provisional

**It is feasible, and it is cheaper than it first looks — but do not build it
yet.** The transport is largely already there, the rollback substrate is most of
the way there, the flash primitives ship with the core, and HalfKay cannot be
bricked. That is a much better starting position than the deferral bullet implies.

What still stands in the way, in descending order:

- **The activate verb is broken and unauthenticated.** `POST /api/reboot` never
  answers and requires no credential. OTA cannot be built on it as it stands.
- **Half the recovery boundary has never run on hardware.** Recovery-safe mode has
  never executed and retained-OCRAM survival is unconfirmed. An A/B fallback
  resting on both would be building on unproven ground.
- **The gate is computable from a MAC address**, so image signing has to be added
  rather than assumed.
- **Two unknowns need the device**: the OTP fuse state, and the erase stall
  against the audio block.

**Fix the prerequisites and the answer likely becomes yes.** Three of the four are
small, and the third — running B6 — is the milestone the network-hardening plan
exists to reach anyway.

The single fact that would most change the shape is **the OTP fuse turning out to
be blown**, which would give hardware image selection and move most of the risk
below the application. If it is unblown, A/B costs an irreversible fuse write per
board, and a staged-image-plus-CDC-flash design becomes the better trade.

**One correction to the original deferral, though, and it stands regardless of
whether OTA is built.** §Deferred work says "HalfKay plus watchdog recovery-safe
mode is the recovery boundary" — and recovery-safe mode has never once executed.
That claim should be read as a design intention rather than a validated property
until B6 gate 3 runs.

## Related documents

- [Aius network and firmware recovery hardening](aius-network-hardening.md) —
  §Deciding principle, §Deferred work, the open-defect list, and B6.
- [Aius firmware I/O reference](../reference/firmware-io.md) — the SD job engine,
  the HTTP transaction and the retained boot slots.
- [USB control protocol](../reference/usb-control-protocol.md) — the CDC recovery
  control plane and the token's intended role.
- [Aius sample-rate following](aius-sample-rate-following.md) — unrelated, but it
  shares the per-epoch bounded-step model this design must fit.
