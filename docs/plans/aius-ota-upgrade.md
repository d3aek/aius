---
title: Aius OTA firmware upgrade
type: plans
scope: component
component: aius
status: shipped
subtype: design
tags: [aius, firmware, teensy, ota, http, security, recovery]
updated: 2026-08-04
toc: true
---

# Aius OTA firmware upgrade

Whether the optical firmware can be upgraded by an HTTP push, what an A/B scheme
with watchdog fallback would look like on this part, and whether the network
surface is observable enough to make that safe. Investigated 2026-07-26 at the
owner's request.

**Owner decision 2026-07-31: OTA is the next key feature.** Complete this
plan's investigation verdict, then implementation; E-AC-3 decode is
considered only after OTA lands
([`aius-eac3-support.md`](aius-eac3-support.md)). Sequencing rider from the
same decision: the easy-to-prove open-defect set closes first, and
hard-to-prove defects wait for the incoming ESI U24 XL bench session
([`aius-open-work-register.md`](aius-open-work-register.md)).

**The feasibility question is answered: yes, and the gating unknown is now
resolved.** The part can self-program its own flash using routines Teensyduino
already ships, and hardware A/B image selection genuinely exists on the
i.MX RT1062 — but it is gated behind an irreversible OTP fuse, **read on the
board 2026-07-28: unblown (`0x00000000`)**, so hardware A/B does not exist on
this board without a burn, and the owner has ruled a burn out for the scoping
phase (see [§Design constraints from the owner](#design-constraints-from-the-owner-2026-07-28)).
See [§Self-flashing is possible](#self-flashing-is-possible).

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
  transmitted race. `POST /api/reboot` had the *same* defect class; it was fixed
  the same way on 2026-07-28 ([prerequisite 1](#prerequisites-in-order)) and
  passed B6 gate 2 on hardware 2026-07-26. The boundary works; a neighbouring
  path built on the same assumption did not, which is the point that survives
  the fix.
- ~~**Recovery-safe mode, the second half of the boundary, has never executed
  on hardware.**~~ **Resolved by B6 on 2026-07-26.** Recovery-safe mode latched
  after two early watchdog resets and returned to service unaided across every
  recoverable injected checkpoint. Checkpoints 1 and 2 remain structurally
  unrecoverable because recovery-safe itself runs exactly those two USB
  checkpoints; that is a known boundary, not an unrun gate.

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

1. ~~**Whether PJRC blows `0x6E0[23:16]` on Teensy 4.1.**~~ **Resolved
   2026-07-28: the real board read `0x00000000`.** The annotated RM covers
   `BT_FUSE_SEL` and the GPIO boot pins but is silent on `BOOT_CONFIG_MISC`.
   Hardware A/B is therefore unavailable without an irreversible burn, which
   the owner has ruled out during investigation. ~~Note `imxrt.h` stops at
   `HW_OCOTP_0x6C0`, so there is no macro for it.~~ **Wrong, corrected
   2026-07-28:** only the
   *numerically named* macros stop there; `HW_OCOTP_MISC_CONF1` is fuse `0x6E0`.
   The read is built and served on `/api/status`
   ([§The fuse read, as built](#the-fuse-read-as-built)).
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
   *watchdog reset* is proven by B6; survival across a power cycle is not
   discussed anywhere. An A/B
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
| **Upload progress** | **yes, since 2026-07-28** — `sd_diag.upload_supplied`, `job_transferred`, `job_total` ([prerequisite 2](#prerequisites-in-order)) |
| Upload failed, and why | partly — `sd_diag.last_failure` has fault, phase, driver fault and bytes, latched only on failure |
| Image verified | no mechanism exists |
| Activated / rebooting | **fixed 2026-07-28 and proven by B6 gate 2** — see below |
| Healthy after boot | yes — `boot_generation`, `watchdog_early_resets`, `recovery_safe` |
| Rolled back | no — nothing distinguishes a rollback from an ordinary recovery-safe latch |

Two remaining gaps and one resolved defect, all recorded in the
network-hardening plan:

- ~~**`POST /api/reboot` resets but never answers**~~ — **fixed 2026-07-28.** The
  mechanism was pinned first: dispatch retains the response un-formatted,
  formatting is the *next* epoch and the first write the one after, while the
  reset fired at the end of the dispatching epoch. The reset is now gated on the
  transaction reaching `AIUS_HTTP_TX_IDLE` with no action outstanding, bounded at
  500 ms so a wedged peer cannot veto a requested reset. **An OTA "activate" verb
  needed exactly this**, and B6 gate 2 now distinguishes "performed" from
  "acknowledged" on the board.
- ~~**The four `http_*` counters have no producer at all.**~~ **Resolved.**
  The runtime now produces and publishes request, rejection and client-drop
  counts; `http_server_rebinds` is deliberately always zero because the server
  is bound once per network generation.
- **No event type covers anything HTTP or upload related.** The ring's ten types
  are audio, network and config only, so an operator watching `/api/events` sees
  nothing about an upgrade.

~~**Cheapest high-value fix, needing no new mechanism:** serialize
`AiusSdJob::transferred` and `AiusHttpTransaction::upload_supplied` into
`sd_diag`. Both are already live.~~ **Done 2026-07-28**, as
`sd_diag.job_transferred` and `sd_diag.upload_supplied`, with `job_total` beside
them so the pair reads as progress rather than as two counts.

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
  epochs per round trip. Still open.
- ~~**`POST /api/reboot`, `/api/start`, `/api/stop` and `/api/clear` require no
  authentication at all.** Any host on the subnet can reset the board
  mid-upload.~~ **Gated 2026-07-28** on `X-Aius-SD-Auth`, along with
  `POST /api/measure/start`, before the dispatch switch rather than inside each
  arm — so a sixth control route cannot be added without the gate. **What that
  is worth, stated honestly:** the token is FNV-1a-64 over the public MAC, so
  this stops a stale browser tab, a scanner or a misdirected script, not an
  attacker who can see a frame from the board. The bullet below still governs.

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
- **No fuse burn, under any circumstances, while OTA is being scoped**
  (2026-07-28, after the fuse read came back unblown). This document is a
  scoping exercise, not an implementation commitment. The path is: explore
  the staged-image design first — WITHOUT burning the fuse — and only after
  that exploration decide whether to permanently modify the board. That
  exploration is now the owner's required verdict gate before the AC-3
  four-hour and by-ear tests; it remains investigation, not permission to
  implement OTA. Until the owner explicitly revisits this, no work item here
  may write OCOTP, and the read-only structural guards in
  [§The fuse read, as built](#the-fuse-read-as-built) stay load-bearing.

## Implementation slice 1 — portable core (delivered 2026-07-31)

`lib/aius_ota` plus the host `ota` suite (21 tests, mutation negative
controls) deliver the transport- and flash-free half: a 256-byte
fixed-offset image container with its own CRC and published field offsets;
the staged-upgrade state machine (strict APPEND_PART-shaped chunk
sequencing, incremental SHA-256 with midstate-carrying power-loss records,
canonical serialization, fail-closed restore); a feature latch separate
from image readiness (constructed disarmed, never persisted); and
slot-independent image-health records keyed on payload digest with a
non-refillable trial budget matching `aius_boot`'s two-reset rule,
answering §rollback-substrate gaps 1-4 structurally. The signature seam is
injected and covers a header-binding digest (magic through
`payload_digest`), so a valid signature also authenticates `target_id` and
build identity, not just the payload bytes; the crypto implementation and
scheme choice are the next reviewed slice. Remaining slices, in order:
signature vendoring (slice 2, in flight); transport/route wiring behind
the compile-time option (slice 3), which also delivers the SD web
interface's manual upgrade page (owner request 2026-07-31): a page in the
existing nav strip with a step-by-step progress log narrated from the
state machine — chunked upload with byte offsets from the resumable
protocol's `next_offset`, staged/verified/armed transitions from the OTA
records and an `ota_diag` status object, the activate step rendered but
visibly disabled until the activation slice ships its verb, and a
graceful disabled state when OTA is compiled out or disarmed; then the
device-gated activation/flash-primitive slice, which waits only for
verdict item 5's remaining empirical scratch-sector check (analytically
resolved 2026-08-01; the recovery channel is proven healthy this session)
and afterwards lights up the page's activate step and
reboot-to-new-identity monitoring.

## Incident 2026-08-01: first real end-to-end activation left the board in ROM SDP

The staging path was proven end to end on hardware first: a dev-key-signed
container of the exact running image (`0b080d465330`, `objcopy -O binary`
of the ELF — verified to carry a valid FCB at offset 0 and IVT at 0x1000)
uploaded resumably (461,056 B; one transient SD-append spike resolved on
resume), the device recomputed SHA-256 to `verified`, and the Ed25519
check reached `armed`. That half works.

The activation then flashed a non-bootable image: after
`POST /api/ota/activate` the board reset into the RT1062 ROM serial
downloader (`1fc9:0135`, "SE Blank RT Family") — the ROM's fallback when
the FlexSPI boot image is invalid. Not a brick; the recoverable state the
design anticipated. ~~**Root cause is a firmware bug in the dark-window
sequence, not the payload:** the signed payload carried a correct FCB, yet
the board came up with an unbootable boot region, so sector 0 (the FCB,
programmed LAST) ended blank/wrong while the sequence still issued
`SYSRESETREQ`. The dark-window byte-compare that must fault-and-halt on a
bad head instead let the reset proceed~~ — a path the host fakes and the
scratch-sector self-test (one sector, no FCB-last-then-reset) do not
cover. **The activate route must stay disabled until this is fixed and
re-proven by a method that does not risk the boot region — e.g. a
full-sequence rehearsal against a scratch flash region with an emulated
reset, asserting the head-compare fault actually halts.**

**Root cause corrected 2026-08-01, from code, after recovery. Two
defects, one deterministic and one intermittent, both of the same class:
something other than the RAM-resident step path executed from the flash
being rewritten.** The struck-through inference was wrong: the head
byte-compare never got the chance to lie, because the sequence died
mid-BODY, long before PROGRAM_HEAD — which is also why sector 0 was blank
(erased first, never reprogrammed).

**Finding 1, the deterministic killer: the dark-window run loop itself
executed from flash.** This core's `FASTRUN` is section placement ONLY
(`pgmspace.h`: `__attribute__((section(".fastrun")))`, no `noinline`), so
GCC inlined the single-caller FASTRUN `ota_activate_run_and_finalize()`
through FLASHMEM `service_ota_activation()` into FLASHMEM `loop()` — the
linked incident-image shape (no `run_and_finalize` symbol; the loop's
code inside `loop()` at a flash address ~94 KiB into the image). The
BODY pass erases ascending; the moment `do_erase()` returned from
erasing the sector holding `loop()`'s own code, the next instruction
fetch came from a blank sector → fault. Per-object ITCM guards could not
see it: an inlined copy has no section of its own.

**Finding 2, the intermittent class: every interrupt source stayed live
through the window.** `ota_quiesce()` parked only the audio ISR; the run
loop executed with interrupts ENABLED between primitives; and even
inside each primitive the mask did not hold — the core driver's
`flash_wait()` ends every erase/program with an unconditional
`__enable_irq()` (`cores/teensy4/eeprom.c`), so with USB SOF at 1 kHz
and ENET frames pending, an ISR was taken inside virtually every one of
the ~1,900 primitive calls. On this part all `.rodata` lives in flash:
an ISR whose const data (or FLASHMEM callee) sat in the
currently-erased sector read blanks and faulted.

Either fault lands in the core's `unused_interrupt_vector()`, which ends
in `SCB_AIRCR = 0x05FA0004` (`startup.c`): **the SYSRESETREQ was the
crash handler's**, issued after its 8-second spin, with the FCB still
erased → ROM SDP. The observed sequence (a client-visible 200, then the
board in SDP) matches, and the self-test could not have caught either
finding: one scratch sector mutates no ISR-reachable or loop-resident
content.

**Fix 1 (this commit): `noinline` restored on the whole dark-window
surface.** `AIUS_OTA_DARK_WINDOW_FN` (FASTRUN + `noinline`) on the run
loops, the quiesce and the interrupt park/restore, so each is a real
ITCM symbol; and a new post-link gate in the itcm-placement suite
(`dark-window ITCM residency`) reads the LINKED image's symbol table and
fails if any of the 18 dark-window symbols is missing (inlined away) or
outside ITCM — the check per-object section tables structurally cannot
perform, with mutation tests for both failure spellings.

**Fix 2 (this commit): the dark window now parks every interrupt
source, not merely PRIMASK.** `ota_quiesce()` gained a fourth step,
`ota_park_interrupt_sources()`: `__disable_irq()`, all five `NVIC_ICER`
banks cleared, SysTick's `TICKINT` off, PendSV/SysTick pendings wiped —
the same move the core's own fault handler makes ("disable all NVIC
interrupts, as usb_isr() might use `__enable_irq()`"). With no source
enabled, the driver's internal enable takes nothing, which discharges RM
note 3 for the M7 across the whole window instead of only inside each
primitive. The park is snapshot-restorable for the rehearsal below;
one-way for a real activation. An optical-cutover guard pins the park's
step order and the restore's completeness, with mutation self-tests.

**The re-proof surface (this commit): `POST /api/ota/rehearse` and
`/api/ota/rehearse-fault`** — the full activation sequence (pre-window
verify over the PSRAM copy, quiesce with everything parked, head erased
first / programmed last, byte-compare per chunk) against a scratch flash
window at `0x60400000`, with the reset EMULATED and interrupts restored
after. The sequencer's begin() now takes an injectable flash window, the
composition's primitives re-bound against it through their ctx, so an
address below the scratch base is structurally unreachable from a
rehearsal (host-proven with a whole-trace address sweep). The rehearsal
runs on a COPY of the session (the real one stays ARMED; no re-upload
between rehearsal and activation) and a scratch health record (no trial
budget spent). The negative arm injects a one-bit fault into the first
head-chunk readback and must land FAULTED with the VERIFY fault — the
incident's demanded "assert the head-compare fault actually halts", on
the real driver. Verdict plus `rehearsal_dark_us` / `rehearsal_max_step_us`
land in `ota_diag`; both routes are auth- and latch-gated bench surface,
deliberately absent from the OpenAPI path contract like the self-test.
**Gate to lift the incident hold: both rehearsal arms pass on the bench;
only then is a real activation attempted again.**

**Rehearsal evidence, 2026-08-01, image `f228f06890e1+teensy41-optical`
(the fixed build), 64 KiB signed rehearsal container.** The POSITIVE arm
PASSED: the full sequence — SD→PSRAM copy, SHA-256 + Ed25519 re-verify,
quiesce with every interrupt source parked, head erased first, 15 body
sectors erased/programmed/byte-compared, head programmed last and
compared, emulated reset — ran against the scratch window and the board
came back serving 4.4 s after the accept, session still ARMED.
`rehearsal_dark_us` 668,814 (0.67 s of dark window for 272 steps over
16 sectors — extrapolating linearly, ≈4.7 s for the real 452 KiB
image); `rehearsal_max_step_us` 40,095 — the longest single operation,
a sector erase, wildly over the 10,667 µs audio-block budget (§Item 4's
audio-stops-during-flashing decision was necessary, not merely
convenient) and comfortably inside the one-feed-per-operation watchdog
cadence. The container was deliberately small: the per-operation
numbers are size-independent, and the bench SD card was by then failing
multi-chunk writes (see below) — the full-size container never staged.

The FAULT-INJECTED arm also PASSED, on a minimum-geometry (4,097-byte)
container after the card degraded further: the injected one-bit fault
in the first head-chunk readback was convicted by the in-window
byte-compare — `rehearsal_fault` 7 (`FAULT_VERIFY`), phase FAULTED, no
reset taken — and the board returned to service 3.7 s after the accept
(`rehearsal_dark_us` 76,633; `rehearsal_max_step_us` 41,230, consistent
with the positive arm's erase ceiling). That is the incident's demanded
"assert the head-compare fault actually halts", on the real driver
against real flash. **Both arms pass: the incident hold is lifted
mechanically.** ~~What still blocks the full-size signed end-to-end is
the bench SD card itself~~ — **corrected the same day: the staging
chaos (multi-second write stalls, driver faults, remounts, watchdog
resets wiping the session and latch, 25 failed offset-0 restarts) was
the board's own boot-lottery epoch crawl, not the card**, which an A/B
against the pre-change image exonerated on the owner's challenge. On a
healthy boot the same card takes the same writes in 50 ms. The open
defect and its shipped detector are in
[`aius-open-work-register.md`](aius-open-work-register.md) §Open
defects — runtime; the end-to-end proceeds on a healthy boot.

**THE SIGNED END-TO-END UPGRADE COMPLETED 2026-08-01 ~11:23, closing
the activation slice.** On a healthy boot of `b51f694b585c` (the
detector commit's build): the 463,104-byte signed container of
`44425d6e6d1c` staged over the resumable transport in **3.1 s with zero
restarts** — the same card, the same protocol that burned 25 restarts
during the degraded-boot period — then device-verified (SHA-256),
armed (Ed25519), and activated: 200 acknowledged, response drained,
SD→PSRAM copy plus full re-verify pre-window, every interrupt source
parked, the body erased/programmed/byte-compared with the FCB last,
`SYSRESETREQ`, and the board booted the NEW image clean —
`revision=44425d6e6d1c build=2026-08-01T11:20:10Z`, `recovery_safe` 0,
`watchdog_early_resets` 0, `reset_source` 2 (the activation's own
deliberate reset), healthy epoch rate, OTA session fresh and re-armed.
The running firmware was replaced over Ethernet for the first time,
with the recovery boundary never needed.

**What remains open after this slice:** the boot-lottery defect (the
register owns it, detector shipped); key rotation off the published
dev pair before the repo goes public or the device leaves the bench
(owner decision 2026-07-31 stands); and the upgrade page's activate
step, which lights whenever the operator enables activation —
`activation_available` correctly gates it boot-by-boot.

**Update 2026-08-04 — that list is closed.** The boot-lottery defect
was convicted and fixed 2026-08-02 (S/PDIF DPLL junk-lock boot gate,
`7b1305314`; 28/28 healthy flash-boots on the reproducer lineage that
failed 3/3 without it — full record in
[`aius-open-work-register.md`](aius-open-work-register.md) §Open
defects — runtime). Key rotation happened 2026-08-04, owner-directed:
the standing device key is the repo-committed home pair
(`firmware/config/ota-home-signing-key.{pub,sec}`), with the compiled
public key generated from that file resource at build time
(`9b6d3474f`); a hardcoded `ota_diag` `test_key` serializer literal
that missed the rotation was caught on the device and fixed
(`17bd5d077`); and the home-key acceptance passed end to end on
hardware — sign with the committed secret → host verify → device stage
→ `verified` → abort, fingerprint `df29355e5a8d7b8c`, `test_key` false
(`cd67cb3f6`). The production cycle (off-repo keygen, swap the file,
rebuild, one signed upgrade) stays the documented later step.

**Also fixed on the way to the re-proof (found live, 2026-08-01): a
fresh staging could never start over the previous one.** The staged file
persists across upgrades, and APPEND_PART's strict-append contract
refused offset 0 against it forever (416 `sd_range`, and no route can
delete `/aius/ota-staged.img`). The first chunk now stages through the
atomic REPLACE pair — truncate by rename, the same scratch/backup dance
every web-tree PUT uses — mirroring the session rule that a re-sent
offset 0 is a deliberate restart; every later chunk appends as before.

**Known wart, recorded not fixed: a failed staging job diverges the
session from the file.** The route absorbs a chunk into the session
(offset + running digest) BEFORE the SD job writes it, so a job that
fails after absorption leaves the session one chunk ahead of the file —
every session-guided resume then 416s, and the only recovery is a
deliberate offset-0 restart (which the fix above makes cheap and which
the bench uploader now performs automatically). Re-ordering
write-then-absorb would need the digest midstate rolled back on job
failure; not worth it while a restart costs a minute. ~~The bench card
is also the operational risk to watch~~ — **corrected the same day: the
"failing card" was the board itself in a degraded boot state.** The
multi-second SD write stalls, driver faults, remounts and watchdog
resets that wrecked staging were all downstream of a boot-lottery epoch
crawl (the byte-identical image runs healthy on other boots, 50 ms
writes over the same card); the card was exonerated by a controlled
A/B after the owner challenged the verdict. The open defect, its
evidence and the shipped detector (`checkpoint_walltime_us` /
`cw=`) live in
[`aius-open-work-register.md`](aius-open-work-register.md) §Open
defects — runtime.

Recovery needs the physical program button (the board is in RT1062 ROM
SDP, not HalfKay/CDC, so no software path from here without NXP SDP
flashloader tooling this bench lacks): press the Teensy program button
once to enter HalfKay, then `scripts/flash_optical.sh` restores a known
image. Raw incident artifacts and the signed container are in the session
scratch.

The activation-wiring slice is delivered and gated: the FASTRUN
RAM-resident flash primitives (PRIMASK-guarded, range-checked, a distinct
`WDOG1_WSR` watchdog writer), durable SD staging to `/aius/ota-staged.img`,
the quiesce (gate closed → audio stopped → ISR parked), three auth-gated
HTTP routes (`/api/ota/activate`, `/api/ota/enable-activation`,
`/api/ota/self-test-flash`), and the item-5 scratch-sector self-test at
`0x60400000`. `activation_available` stays false and `activate` answers
409 until the operator flips `g_ota_activation_enabled`. This deliberately
raised the gated-`reset_application` invariant from one site to two (reboot
+ activation). The OCRAM floor held at +0 (the ~1 KiB activator is DTCM).

**Two frozen-core caveats block enabling the real dark-window activation
(not the self-test):** (1) `aius_ota_activate_step()` and its verify graph
(including `aius_sha256`) are FLASHMEM, so they execute from the image
region being erased. (2) `ota_staged_read` reaches the framework SD driver,
whose code is likewise in that region. Both fault once BODY erase begins.

**The final slice's architecture (analysis 2026-08-01), which makes the
dark window minimal instead of moving a multi-KB graph into the ~4.7 KB-free
ITCM.** Split verification out of the window: BEFORE quiesce, while flash
and SD still execute, copy the whole staged image SD→PSRAM and run the
SHA-256 + Ed25519 verify over the PSRAM copy against the armed header
(abort with nothing flashed on any mismatch). The dark window then needs
no SHA-256 and no SD — only erase → program-from-PSRAM →
byte-compare-against-PSRAM (an inlined `memcmp`, not a digest), so the
RAM-resident set shrinks to the small step loop plus the already-FASTRUN
primitives, which fits ITCM. PSRAM is on FlexSPI2, independent of the boot
flash on FlexSPI1, so a RAM-resident reader reads it throughout the erase.
Constraints this must honor: PSRAM presence is runtime (`external_psram_size`
/ the pad-2 probe) — with no PSRAM fitted, activation is refused, not
attempted; and the staging buffer must not overlap the recorder ring
(quiesce stops the recorder, but the address regions must still be
partitioned, not aliased). This reworks the activation sequencer in
`lib/aius_ota` (verify-before-window, byte-compare-within) and its 15 test
groups. Until it lands and is bench-proven, `activation_available` stays
false; the item-5 self-test (one scratch sector, primitives only, no
sequencer/SD/PSRAM) is the safe standing recovery proof.

The composition (up to ARMED) is delivered and live on hardware. The five
route ids parse in `lib/aius_http` (unconditional, so host suites cover
them without the option); the six real callbacks over one `AiusOtaSession`,
the health-record store and the compiled-in dev public key are installed
under `#if AIUS_OTA`; `ota_diag` joins `GET /api/status`; TweetNaCl is
pinned to flash by the linker script (readelf confirms every symbol above
`0x60000000`, zero ITCM); and swagger documents the whole surface so the
drift gate passes. The `AIUS_OTA` option gates every heavy cost — with it
off the `lib/aius_ota` + TweetNaCl objects are `--gc-sections`-dropped
(~14.4 KB flash+DTCM, zero ITCM). One always-present cost remains: the
`AiusOpticalAppOta` dispatcher seam is 32 B of OCRAM, consistent with the
recorder/replay/measure seams beside it; the memory ratchet was reconciled
to 131392 B and the real defect-26 guard (the 128 KiB linker `ASSERT`)
retains 320 B of margin. Exact image `66fc5a069d31+teensy41-optical`, built
with the option on, flashed and booted clean: IP assigned, link up, HTTP
serving, `recovery_safe` 0 and `watchdog_early_resets` 0 on boot generation
22 — direct proof NativeEthernet's ~96 KiB boot mallocs succeeded at the
reconciled free value. `GET /api/ota` returns the live status document
(armed, idle, key fingerprint `78cafee9c572...`, `test_key` true,
`activation_available` false). What remains is the activation-wiring slice
(RAM-resident flash primitives + the activate verb, gated on item 5's
scratch-sector bring-up check) and the signed end-to-end upgrade.

Slice 3B is delivered: the auth-gated `/api/ota/*` route layer behind app
callback seams, absent-by-construction until composition wires it. Slice
3A is delivered: the SD upgrade page with the step-by-step log. The
activation sequencer core is delivered too: `aius_ota_activate` programs
the staged image over injected flash primitives with the FCB erased first
and programmed last (so an interrupted activation is a designed HalfKay
rescue, never a half-booted image), a verify pass that both reads back
against the staged bytes and recomputes the digest against the armed
header before the head is touched, one watchdog feed per primitive, and a
health-record activation-in-progress mark written before the first erase
so power-loss forensics distinguish never-started from interrupted. It
serves the dark-window-evidence bullet via
`AiusOtaActivateProgress.dark_window_cycles`. What remains is composition
(HTTP route ids in `lib/aius_http`, the six real callbacks, session and
records and `ota_diag` in `optical_main.cpp`, the TweetNaCl linker pin,
swagger), then verdict item 5 on the bench, then the signed end-to-end
upgrade.

Slice 2 is delivered: the Ed25519 adapter (scheme tag 1, faults latched
distinctly, entropy trap on the never-used sign path) and the host signer
(keygen/sign/verify driving the firmware's own header serializer and
session, never a re-implementation; committed test-only keypair from a
published seed). Placement finding for the target build, measured with
the pinned toolchain: TweetNaCl's `crypto_sign_open` call graph is
~4.5 KB `.text` plus ~1.9 KB `.rodata`, and the dcadec-style
`build_src_filter` route would land those in ITCM and DTCM invisibly —
against ~5 KB of ITCM headroom, that is not acceptable unpriced, so the
selected mechanism is the linker-script pin: `EXCLUDE_FILE(*tweetnacl.c.o)`
on the ITCM `.text` collector plus an explicit `.text.code` placement,
with `test/boot-target/test_ldscript_drift.py` updated to match. That
lands with slice 3's target wiring.

Owner decision 2026-07-31: the committed test-only keypair serves as the
device key for now (private bench; its seed is published, so it provides
no authenticity against repo readers and must rotate before the repo goes
public or the device leaves the bench). Slice 3 keeps that state visible:
the compiled-in public key is one swap point, `ota_diag` reports the
active key's fingerprint plus an explicit test-key flag when it matches
the published dev key, and the upgrade page banners TEST KEY in that
state. Rotation is keygen + rebuild + one signed upgrade.

**Rotated 2026-08-04** to the repo-committed home pair, taking exactly
the documented shape (keygen + rebuild + one signed acceptance) — see
the dated update under the incident section above (`9b6d3474f`,
`17bd5d077`, `cd67cb3f6`).

## Investigation completion gate

“OTA fully investigated” means this plan can issue a finite build/defer verdict.
It does **not** require an OTA implementation, and it never authorizes an OTP
fuse burn. By owner decision 2026-07-29, the AC-3 four-hour and by-ear mission
tests wait for this verdict.

The verdict requires all of the following:

1. ~~B6 proves the watchdog/recovery boundary and acknowledged reboot.~~
   **Done 2026-07-26**, with checkpoints 1 and 2 explicitly unresolved by
   structure.
2. ~~Read and disposition `FLASH_SEC_IMAGE_OFFSET`.~~ **Done 2026-07-28:**
   unblown, so no hardware A/B without a prohibited fuse burn.
3. Reproduce or disposition network-hardening defect 25, the conditional
   flash-loader-specific reboot failure. An unattended upgrade cannot be called
   recoverable while unattended reflash is conditionally broken.
4. Measure erase and program stalls, including the LUT/interrupt constraints,
   and decide whether audio must stop while staging to boot flash.
5. Prove the HalfKay/ROM recovery path still works after the flash primitive's
   LUT use, or state the exact recovery limitation.
6. Choose between staged single-image recovery and A/B-with-fuse as a product
   posture, without burning the fuse during investigation.
7. Specify image signature/key ownership, activation, rollback, power-loss
   state, slot/build identity, and power-cycle-persistent health state.
8. Close or explicitly accept the operator-observability gaps for offered,
   staged, verified, activated, healthy and rolled-back states.

The outcome is one of: **build staged single-image OTA**, **seek separate
authorization for fused A/B**, or **defer OTA with the recovery and security
reasons recorded**.

### Investigation verdict, 2026-07-31: build staged single-image OTA

The owner directed OTA as the next key feature (2026-07-31), which issues
the build verdict; the fuse prohibition stands untouched. Item-by-item:

- **Item 3, defect 25 — dispositioned for OTA, still open for the bench.**
  The instrumented `fa=` latch has ridden four flashes since landing
  (including `fecc4a0a2473` and `5adde600f880` on 2026-07-31), all booting
  clean: 0/4 against the pre-instrument 4/4. More decisively for THIS
  verdict, the defect implicates the loader CDC session, a path OTA never
  uses: self-flash activates via `SYSRESETREQ` through the ROM, not
  HalfKay. Unattended bench reflash remains conditionally unproven; OTA
  activation is not gated on it.
- **Item 4 — resolved by owner decision, 2026-07-31: audio stops during
  flashing.** OTA does not need to run with the device in use, so the
  activation slice designs a deliberate quiesce (gate closed, audio
  service stopped, ISR sources parked) around the erase/program window,
  which also discharges RM note 3's no-flash-access-from-any-master
  obligation structurally instead of by timing analysis. The dedicated
  stall measurement is dropped: no other feature consumes flash-stall
  timing (nothing else writes boot flash at runtime), and the dark-window
  duration falls out of the activation slice's own bring-up evidence for
  operator expectations. **Item 5 — analytically resolved 2026-08-01;
  recovery is not compromised by the driver's LUT use.** Read against
  Teensyduino's `eepromemu_flash_*` (the exact driver OTA would call):
  every erase/program rewrites only LUT **sequence 15** (`FLEXSPI_LUT60`
  through `LUT63`, the IP-command sequence) and leaves **sequence 0**, the
  AHB read sequence XIP executes from, untouched — so code keeps running
  from flash after an erase, and `bkpt #251`/HalfKay entry needs no FlexSPI
  sequence at all (HalfKay is a separate chip on the debug port). The
  activation `SYSRESETREQ` then resets the FlexSPI controller and the boot
  ROM reloads the whole LUT from the FCB, so sequence 15's residual opcode
  cannot outlive the reset. The load-bearing recovery channel is proven
  healthy on this exact board: CDC→HalfKay entry and reflash succeeded
  repeatedly across this session's flashes, and `AIUS ID` over CDC answered
  immediately after the latest. The remaining empirical step — running the
  real driver against a scratch flash sector far above the image, then
  confirming XIP and HalfKay both survive — folds into the activation
  slice's own bench bring-up (it needs a firmware test hook that only that
  slice ships) and must pass there before the first real activation. Slices
  2 and 3 do not touch the flash and proceed regardless. **Empirically
  closed 2026-08-01** on exact image `a01b3f8f2c0e+teensy41-optical`:
  `POST /api/ota/self-test-flash` ran the real FASTRUN primitives (erase +
  program + readback) on the scratch sector at `0x60400000` and returned
  `pass`; the board kept serving HTTP on the same boot generation with
  `recovery_safe` 0, `watchdog_early_resets` 0 and zero overruns (XIP
  survived the LUT-sequence-15 rewrite), and `AIUS ID` over CDC answered
  immediately afterward (the HalfKay/CDC recovery plane is intact). The
  driver's LUT use breaks neither execution-in-place nor recovery.
- **Item 6 — posture chosen: staged single-image.** Stage to SD through
  the existing resumable chunked upload machinery, verify from SD, program
  the boot flash inside a dark window, `SYSRESETREQ`; recovery remains the
  proven HalfKay + watchdog recovery-safe boundary. No fuse, no slot-B
  linker script.
- **Item 7 — specified.** Container, staging, power-loss records and
  image-health/trial rules: slice 1 (delivered). Signature: Ed25519 over
  the header-binding digest (scheme tag 1), vendored TweetNaCl verify-only,
  compiled-in public key, operator-held private key, host signer tool:
  slice 2 (in flight). Activation, dark-window policy and the health
  record's rollback semantics (trial budget 2, matching the watchdog
  rule; a failed trial latches recovery-safe and the record names the
  fallback identity): activation slice, after items 4/5.
- **Item 8 — closed where cheap, accepted where not.** Upload progress and
  failure diagnosis exist (`sd_diag`); staged/verified/armed states are
  slice-1 record fields the wiring slice serializes; OTA event types ride
  the wiring slice. Accepted gap, stated: "rolled back" is distinguishable
  only through the health record, not a dedicated event, until the
  activation slice lands.

By the 2026-07-29 decision, the AC-3 four-hour and by-ear gates are
unblocked by this verdict (they still also wait on sample-rate
acceptance).

## Prerequisites, in order

Nothing about OTA should be built before these, and each is independently
worthwhile:

1. ~~**Fix `POST /api/reboot`** so it answers before it resets, and
   **authenticate it** along with `/api/start`, `/api/stop`, `/api/clear`.~~
   **Done 2026-07-28.** The reset is gated on the transaction reaching
   `AIUS_HTTP_TX_IDLE` with no action outstanding, bounded at 500 ms
   (`firmware/src/optical_main.cpp` `reboot_drain_complete()` /
   `service_reboot_request()`), and all four routes plus
   `POST /api/measure/start` are gated on `X-Aius-SD-Auth` before the dispatch
   switch (`firmware/src/aius_optical_app.cpp` `route_is_control()`). Three
   suites hold it: the http transaction oracle walks the 200 one step at a time
   and asserts `TX_IDLE` absent at every step before the close completes, the
   optical-app oracle asserts 401 with no token and with a wrong one for all
   five routes, and the optical-cutover structural guard rejects a `loop()` that
   resets directly, an unbounded wait, or a fixed-epoch delay in place of the
   drain predicate. **B6 gate 2 passed on hardware 2026-07-26.**
2. ~~**Serialize upload progress** — `transferred` and `upload_supplied` into
   `sd_diag`.~~ **Done 2026-07-28.** `sd_diag` now carries
   `upload_supplied` (bytes the transaction has handed down),
   `job_transferred` (bytes the job has committed to the card) and `job_total`
   (the requested size, so a reader has a denominator). The transaction is
   passed into `aius_runtime_storage_diag_json()` rather than mirrored inside
   the storage service, because a mirror beside the authority is the same drift
   this object exists to remove. **The reading that pays for it:**
   `upload_supplied` advancing while `job_transferred` stands still is a card
   that has stopped accepting writes — previously indistinguishable from an
   idle board.
3. ~~**Run B6.**~~ **Done 2026-07-26.** Recovery-safe mode latched after two
   early watchdog resets and returned to service unaided at all seven
   recoverable checkpoints. Checkpoints 1 and 2 are structurally unresolved
   because they are the only services recovery-safe mode itself runs.
4. **Read the OTP fuse on real hardware** to settle whether hardware A/B is
   available, and **measure a sector-erase stall** against the audio block.
   These were the two device-only checks in
   [§Self-flashing is possible](#self-flashing-is-possible); the fuse read is
   done and the stall measurement remains.
   - ~~The fuse read.~~ **Done 2026-07-28, mechanism and reading.**
     `GET /api/status` carries
     `boot_fuse_0x6e0: {word, read, sec_image_offset, sec_image_size}` — the
     full word hex-encoded, plus `[23:16]` and `[15:12]` decoded. Read-only by
     construction and by guard: see
     [§The fuse read, as built](#the-fuse-read-as-built). **The board answered
     `word 0x00000000` (`read: true`, both fields 0): the fuse is unblown and
     hardware A/B is unavailable without an irreversible burn — which the
     owner has ruled out for scoping
     ([§Design constraints](#design-constraints-from-the-owner-2026-07-28)).
     Any investigation continuation therefore starts with the staged-image
     design; it does not authorize implementation.**
   - **The erase stall.** No flash was written or erased in this batch. The
     measurement is designed rather than performed: see
     [§Measuring the sector-erase stall](#measuring-the-sector-erase-stall).

### The fuse read, as built

**One 32-bit load from an OCOTP shadow register, and nothing else.**
`aius_boot_teensy_boot_fuse()` (`firmware/src/aius_boot_teensy.cpp`) reads
`HW_OCOTP_MISC_CONF1` on the target and decodes it;
`aius_boot_teensy_decode_boot_fuse()` is the pure half, so the shift and mask
are exercised on the host over fabricated words before any board is asked.

**One correction to [§What remains genuinely unknown](#what-remains-genuinely-unknown).**
That section says `imxrt.h` "stops at `HW_OCOTP_0x6C0`, so there is no macro for
it". The *numerically named* macros stop there; the next two words are named
rather than numbered — `HW_OCOTP_MISC_CONF0` (fuse `0x6D0`) and
`HW_OCOTP_MISC_CONF1` (fuse `0x6E0`). So the framework does carry this register
and no hand-rolled address literal is needed. The derivation that proves they
are the same word:

- `imxrt.h:427` — `IMXRT_OCOTP_ADDRESS 0x401F4000`.
- `imxrt.h:7612` — `IMXRT_OCOTP_VALUE` is at `IMXRT_OCOTP_ADDRESS + 0x400`, so
  the shadow block begins at `0x401F4400`.
- `HW_OCOTP_0x6C0` is `IMXRT_OCOTP_VALUE.offset2C0` = `0x401F46C0`, i.e. the
  `0xNNN` in a fuse-map name is `IMXRT_OCOTP_ADDRESS + 0xNNN`. Fuse `0x6E0` is
  therefore `0x401F4400 + 0x2E0` = **`0x401F46E0`**, which is
  `IMXRT_OCOTP_VALUE.offset2E0` = `HW_OCOTP_MISC_CONF1`.
- Cross-checked on a word whose fuse address is known independently:
  `HW_OCOTP_MAC0` is `IMXRT_OCOTP_VALUE.offset220` = `0x401F4620`, and MAC0 is
  documented at fuse `0x620`. Same relation from a different entry.

**Read-only, and enforced rather than intended.** The shadow register is loaded
by the boot ROM; reading it is a plain AHB load with no `OCOTP_CTRL` address or
unlock sequence, no `BUSY` poll, no `READ_CTRL`, no `RELOAD_SHADOWS` — none of
the machinery a fuse write must go through. Two structural guards reject a
regression: `test/boot-target/test_source_guard.py` and the optical-cutover
check both fail on any assignment or read-modify-write to an `HW_OCOTP_*` /
`IMXRT_OCOTP*` symbol, and on any raw `0x401F4xxx` address literal that would
bypass the first check. Both carry mutation cases, so a guard that could never
fire is itself a failure.

**The host sentinel is `0xFFFFFFFF`, deliberately not zero.** Zero is the
*shipped* value of both fields, so a zero sentinel would have made a host build
report exactly the answer a genuine unblown board produces — and "is this fuse
blown" is the entire question. `0xFFFFFFFF` decodes to a secondary-image offset
of 255 × 256 KiB = 63.75 MiB on a part with 8 MiB of flash, which is
structurally impossible, so a client that ignores `read` still cannot be misled.

**What to do with the answer.** `sec_image_offset` 0 means the secondary image
base is the primary's address and hardware A/B is unavailable without an
irreversible burn; anything nonzero means the selector already has somewhere to
point.

**The reading, from the real board (2026-07-28,** image `7a66153703d1`
`teensy41-optical`**):** `{word: "0x00000000", read: true, sec_image_offset: 0,
sec_image_size: 0}`. Unblown — the shipped value, exactly what the sentinel
design anticipated. Hardware A/B does not exist on this board today, and per
[§Design constraints](#design-constraints-from-the-owner-2026-07-28) no burn
may change that during scoping; the staged-image design is the exploration
path, now part of the investigation completion gate.

### Measuring the sector-erase stall

**Nothing was erased or written in this batch, and nothing should be until this
runs deliberately.** The remaining unknown is W25Q64JV erase/program timing, and
what it costs is the audio block: the decoded block is 512 frames at 48 kHz, so
`enqueue()` has a **10,667 µs** budget and runs 93.75 times a second. A sector
erase that exceeds that budget drops audio; one that fits does not. No datasheet
is held locally and no core comment gives a figure, so the number has to be
measured on the part.

**The measurement, in one sentence:** bracket a single 4 KiB sector erase in DWT
cycles, on a scratch sector far above the image, with the audio path live, and
compare the measured stall against the block budget.

Preconditions, all of which must hold before the first erase:

- **B6 has run.** Recovery-safe mode and retained-OCRAM survival are the
  fallback if the measurement wedges the board, and neither has executed
  ([prerequisite 3](#prerequisites-in-order)).
- **The scratch sector is chosen and asserted at compile time.**
  `_flashimagelen` is 585,728 bytes; a scratch sector at `0x60700000` is over
  6 MiB clear of it and clear of any plausible slot B at `0x60100000` /
  `0x60200000`. A `static_assert` against `_flashimagelen` is the guard, not a
  comment — the driver has **no bounds checking whatsoever** and erasing sector
  0 destroys the FCB.
- **The whole call graph is verified non-`FLASHMEM` post-link**, including
  `memcpy` and any helper. This inverts the firmware's own rule and the two
  existing ITCM guards check the opposite direction, so it needs its own
  post-link assertion ([§The Teensy 4 memory model](#the-teensy-4-memory-model-makes-this-safe-by-default-and-it-inverts-a-project-rule)).
- **RM note 3 is discharged, not assumed.** The core's `__disable_irq()` covers
  note 4 only. No DMA or ISR may touch a `FLASHMEM` function, a `PROGMEM`
  string or a `const` array in `.text.progmem` during the operation — which the
  audio path does, so the first measurement runs with the audio engine
  **stopped**, and the live-audio measurement is a second, separate run.
- **`flash_wait()` unconditionally calls `__enable_irq()`.** There is no
  PRIMASK save/restore, so the bracket must not assume interrupts are still
  disabled on return.

Shape of the run, in order:

1. **Audio stopped, one erase.** Bracket `eepromemu_flash_erase_sector()` with
   `ARM_DWT_CYCCNT` (the counter the per-block cycle meters already use), and
   record the delta as microseconds at 600 MHz. This is the number that decides
   everything else; take at least 16 samples and report min / max / median,
   because NOR erase time is specified as a range, not a value.
2. **Compare against 10,667 µs.** Under budget → an erase can run between
   blocks. Over budget → OTA cannot run with audio live and the design becomes
   "stage to SD, stop audio, flash, reboot", which is a materially cheaper
   design than A/B and worth knowing early.
3. **Audio live, one erase**, only if step 2 is under budget. The convicting
   reading is the existing underrun / overrun counters on `GET /api/meters`,
   not the DWT delta: a stall that fits the budget on paper but drops a block
   is what the counters exist to catch.
4. **Record the measured range in this document**, and only then decide whether
   the erase-during-audio case needs a block-boundary scheduler.

**A zero-risk probe was considered and rejected for this batch.** Reading the
FlexSPI LUT and `FLEXSPI_FLSHA1CR0` geometry is a plain register read and would
be safe; issuing a JEDEC ID read (`0x9F`) to identify the part is not, because
it drives `FLEXSPI_IPCMD` over LUT sequence 15 — the same sequence the erase
driver overwrites, and the thing
[§What remains genuinely unknown](#what-remains-genuinely-unknown) item 4 is
already unsure about with respect to `bkpt #251`. The geometry read is the
natural first step of the run above rather than a separate errand.

## Verdict, provisional

**It is feasible, and it is cheaper than it first looks — but do not build it
yet.** The transport is largely already there, the rollback substrate is most of
the way there, the flash primitives ship with the core, and HalfKay cannot be
bricked. That is a much better starting position than the deferral bullet implies.

What still stands in the way, in descending order:

- ~~**The activate verb is broken and unauthenticated.**~~ **Fixed 2026-07-28
  and proven by B6 gate 2.** `POST /api/reboot` answers before it resets and all
  five mutating routes carry a credential.
- ~~**Half the recovery boundary has never run on hardware.**~~ **Resolved by
  B6 on 2026-07-26** for every recoverable checkpoint; checkpoints 1 and 2 are
  explicitly outside the recovery-safe mechanism.
- **The gate is computable from a MAC address**, so image signing has to be added
  rather than assumed.
- ~~**The OTP fuse state needs the device.**~~ **Resolved: unblown.** The erase
  and program stalls against the audio block remain designed but unmeasured.

**Complete the investigation gate and the answer becomes finite.** B6 and the
fuse read are done. The remaining work is the flash behavior and recovery proof,
defect 25, the staged-image/A/B posture, image authentication, persistent
activation/rollback state, and operator observability.

The fact that most changed the shape is now resolved: **the OTP fuse is
unblown**. Hardware A/B would therefore cost an irreversible fuse write per
board, prohibited during investigation, so staged-image-plus-CDC recovery is
the path to investigate first.

**One correction to the original deferral, though, and it stands regardless of
whether OTA is built.** §Deferred work says "HalfKay plus watchdog recovery-safe
mode is the recovery boundary". That recovery boundary is now validated by B6
for every checkpoint it can structurally recover; checkpoints 1 and 2 remain
outside it.

## Related documents

- [Aius network and firmware recovery hardening](aius-network-hardening.md) —
  §Deciding principle, §Deferred work, the open-defect list, and B6.
- [Aius firmware I/O reference](../reference/firmware-io.md) — the SD job engine,
  the HTTP transaction and the retained boot slots.
- [USB control protocol](../reference/usb-control-protocol.md) — the CDC recovery
  control plane and the token's intended role.
- [Aius sample-rate following](aius-sample-rate-following.md) — unrelated, but it
  shares the per-epoch bounded-step model this design must fit.
