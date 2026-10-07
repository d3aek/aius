# Aius firmware

The Teensy 4.1 firmware: portable, host-testable codec code, and the optical
bring-up target that serves it over Ethernet, USB and SD. See the
[capability gate](../docs/plans/aius-capability-gate.md) for the phased plan and
the measured pass/fail budgets, and
[network and firmware recovery hardening](../docs/plans/aius-network-hardening.md)
for the bounded runtime the optical target now runs on.

The optical target's application-level USB control commands are defined in the
[USB control protocol reference](../docs/reference/usb-control-protocol.md).

Open this directory in the VS Code PlatformIO extension. Codec, parser, DSP, and
measurement code stays behind portable C-compatible interfaces with no Arduino
dependency, so the identical source runs in host tests and on the Teensy.

## Layout

| Path | Purpose |
| --- | --- |
| `lib/aius_codec_api/` | Portable C ABI (`aius_arena.h`, `aius_codec.h`) + the arena allocator. |
| `lib/aius_iec61937/` | IEC 61937 data-burst parser. |
| `lib/aius_dts/` | `AiusDtsDecoder` adapter over the vendored DTS Core + the arena alloc shim. |
| `lib/aius_a52/` | `AiusA52Decoder` adapter over the hardened liba52 fork, with six-step AC-3 AU decode. |
| `lib/aius_clock/` | Portable supported-rate tracker and pure S/PDIF PLL4 planner. |
| `lib/aius_pipeline/` | PCM/DTS/AC-3 classifier + parser→decoder end-to-end chaining. |
| `lib/aius_dsp/` | 5.1→stereo downmix + biquad EQ output DSP. |
| `lib/aius_dsp_runtime_config/` | Shared production-model resolver for firmware and native renderers: feature bypass, coefficient design, state reset and meter cadence. |
| `lib/aius_capability/` | USB capability wire protocol + arena-driven device handler. |
| `lib/aius_replay/` | Replay service over a caller-supplied `AiusReplayReader`. |
| `lib/aius_boot/` | Retained boot ledger: generation, checkpoint, reset kind, recovery mode. |
| `lib/aius_ota/` | Portable OTA image container, staged-upgrade state machine and image-health records, plus the Ed25519 verifier adapter (`aius_ota_ed25519.c`, signature scheme tag 1) behind the injected signature seam, over the verbatim TweetNaCl in `vendor/tweetnacl/`. |
| `lib/aius_runtime/` | Epoch scheduler + the per-checkpoint budget table. |
| `lib/aius_net/` | UDP frame codec + the network generation supervisor. |
| `lib/aius_http/` | HTTP request parser + the one bounded server transaction. |
| `lib/aius_sd/` | Bounded SD job machine (probe, config load, upload, download, delete). |
| `lib/aius_usb_control/` | USB control command parser + runtime transaction. |
| `lib/aius_config/`, `lib/aius_dsp_config/` | Feature-flag config + DSP model JSON validators. |
| `lib/aius_auth/` | Physical-access token derived from the fused MAC words. |
| `lib/aius_audio/` | Subset build of the PJRC S/PDIF3 input/output units + the audio clock, from the retained source snapshots (the framework `Audio` library is `lib_ignore`d). |
| `vendor/dcadec-core/` | Minimal `dcadec` fixed-point DTS Core (LGPL 2.1, verbatim; see its `NOTICE.md`). |
| `vendor/NativeEthernet/` | Pinned Ethernet stack; the framework copy is `lib_ignore`d and the pin is proved post-link. |
| `vendor/tweetnacl/` | TweetNaCl 20140427 verbatim (public domain; see its `NOTICE.md`): `crypto_sign_open` only, behind the OTA Ed25519 adapter. |
| `src/` | Teensy entry points (`main.cpp`, `capability_main.cpp`, `optical_main.cpp`) + the Teensy adapters. |
| `tools/device/` | Simulated device + host runner, including the P6 hidraw transport. |
| `tools/net/` | HTTP and UDP exercisers for a running optical board. |
| `tools/dts_vectors/`, `tools/iec_corpus/` | Committed DTS/IEC test corpora + generators. |
| `tools/replay_samples/` | Additive DTS/AC-3 SD replay-pack generator and AC-3 transport validator. |
| `tools/dcadec_ref/` | dcadec-reference generator for the LFE bit-exactness suite. |
| `tools/measure_vectors/` | Committed PCM stimulus vectors for the measurement loopback + their generator/validator (preamble-free and loop-coherence obligations asserted at generation, revalidated by the `measure` suite). |
| `tools/memory/` | Linker-map → per-bank memory report. |
| `tools/ota/` | Host OTA image signer: Ed25519 keygen, container signing and verification through the firmware's own state machine. Compiles its two native helpers on demand from `lib/aius_ota` + `vendor/tweetnacl`; `--help` prints the operator key-generation command. |
| `test/` | Host test suites (one directory per suite). |
| `scripts/test.sh` | Native (host, gcc) test runner — the firmware's test entry point. |
| `scripts/build.sh` | Teensy 4.1 cross-build (`pio run`) + per-bank memory report. |
| `scripts/run_hid.sh` | Host P6 proof over Teensy RawHID (`/dev/hidrawN`). |
| `scripts/flash_and_run.sh` | Bounded flash, re-enumeration wait, and P6 proof. |
| `scripts/flash_optical.sh` | Optical-image flash wrapper; waits for HalfKay and reports completion. |
| `platformio.ini` | Teensy 4.1 cross-build environments. |

## Optical runtime

`src/optical_main.cpp` is the Ethernet bring-up target, built as
`teensy41-optical`. Since the Phase 14 cutover `setup()` constructs and `loop()`
runs one bounded epoch, and every transport, SD and watchdog effect leaves
through that runtime rather than through a blocking call in this file. What
remains here is the audio processor and its DSP model, the event ring, the
serializers, and the platform callbacks the runtime asks for.

**`setup()` performs construction only.** It captures the retained reset state,
arms the watchdog, brings up audio memory and the S/PDIF pipeline, derives the
device auth token, and constructs `AiusRuntimeTeensy`. There is no SD probe, no
link or DHCP wait, no USB wait, and no loop waiting on anything external. The
network comes up through the supervisor's retained initial action at checkpoint
4, and the SD probe plus both config loads are jobs the first epoch starts.

**`loop()` runs exactly one epoch, then the two things the epoch itself must not
do**: mark the boot healthy on the first complete epoch at least 10 s after
construction, and perform a requested reset.

**The reset waits for the transaction to drain, and "after the epoch" is not
enough.** This paragraph used to claim the response "has already been written" by
then, which was wrong and cost a live defect: dispatch only *retains* the response,
so formatting and the first write happen in the following epoch and the close
later still — the reset landed an epoch before the first byte. It now waits for
`AIUS_HTTP_TX_IDLE` with no action outstanding, bounded at 500 ms, after which it
resets anyway so a wedged or vanished peer cannot hold off a requested reset.
There is no UDP reboot path; `POST /api/reboot` is the only writer.

An epoch walks the checkpoints in order, each with a fixed call/byte/time budget
from `lib/aius_runtime/`:

| # | Checkpoint | Work |
| --- | --- | --- |
| 0 | `RESET_CAPTURE` | Publishes the retained reset facts `setup()` captured once. |
| 1–2 | `USB_RX`, `USB_TX` | Bounded CDC drain into a 64-byte staging buffer, then one control-transaction step. |
| 3 | `METRICS` | One hardware-metrics sample. |
| 4 | `NETWORK` | One supervisor step or one adapter operation, never both. |
| 5 | `HTTP` | One transaction step; at most one SD-backed action is admitted. |
| 6 | `UDP` | One datagram in, at most one reply out. |
| 7 | `REPLAY` | Replay refill, and the SD lifecycle driver. |
| 8 | `SD` | At most one SD primitive. |
| 9 | `EPOCH_COMPLETE` | Recorded, then the single watchdog feed. |

A service that cannot complete returns false and the epoch ends there, without
the feed — which is what makes a wedged call a bounded reset rather than a hang.

### The audio clock

**The boot and replay clock run at 48 kHz, and `teensy41-optical` pins that
default with
`-DAUDIO_SAMPLE_RATE_EXACT=48000.0f`.** `AudioStream.h` defaults that macro to
`44100.0f` behind an `#ifndef`, and `config_spdif3()` derives the whole S/PDIF
transmit clock tree from it — so the default silently made the optical output
44.1 kHz while the DTS decoder, the PCM passthrough tag, the DSP config, the meters
and the replay seam all said 48,000. The audible result was 48 kHz material playing
8.1 % slow, about 1.4 semitones flat, with every counter honestly zero because the
producer and the consumer both ran off the same wrong clock.
`optical_main.cpp` still `static_assert`s the boot flag equal to the 48 kHz
generated model/replay contract, which
`scripts/build_config.py` generates from `config/optical_dsp.json`.

**Nothing in the audio path resamples.** The portable clock tracker recognizes
32, 44.1 and 48 kHz carriers; PCM, DTS and AC-3 blocks retain their nominal
rate; and rate-dependent DSP coefficients and meters use that block rate.
For live input, checkpoint 7 applies a whole-rate PLL4 plan and then follows the
source oscillator with boundary-queued numerator trims. The input remains graph
master. Replay restores the exact 48,000,000 millihertz plan before output takes
graph ownership. A nominal change closes the physical pad and flushes stale
output; a same-nominal trim keeps the carrier and ring continuous while marking
exactly four 128-frame transmit periods invalid. A block/clock mismatch is
counted and refused before DSP/output state advances. The `clock` object on
`GET /api/status` reports carrier, stream and applied-output truth separately.

**The engine and codec work units have different sizes.**
`AUDIO_BLOCK_SAMPLES` is the core default 128 frames and is the *engine* block, so
`update()` runs at the active rate divided by 128 (375 times/s for replay). A DTS access unit and
`AIUS_PIPELINE_PCM_BLOCK_FRAMES` passthrough block are 512 frames. An AC-3 access
unit is six 256-frame decode units; its DWT metric is their completed-AU
aggregate, while partial or abandoned AUs publish no timing result.

### Portable engines behind Teensy adapters

Every bounded state machine is portable C compiled by the host suites; the
Teensy-specific code is a one-shot adapter underneath it, and the composition
root wires the two together.

| Engine | Adapter | Runtime service |
| --- | --- | --- |
| `lib/aius_net/` supervisor | `src/aius_network_teensy.cpp` | `src/aius_runtime_network_http.cpp` |
| `lib/aius_http/` parser + transaction | (same adapter) | (same service) |
| `lib/aius_sd/` job machine | `src/aius_sd_teensy.cpp` | `src/aius_runtime_storage.cpp` |
| `lib/aius_usb_control/` | `src/aius_cdc_teensy.cpp` | `src/aius_runtime_usb.cpp` |
| `lib/aius_boot/` ledger | `src/aius_boot_teensy.cpp` | (platform callbacks) |
| `lib/aius_runtime/` scheduler | — | `src/aius_runtime_teensy.cpp` |

`src/aius_optical_app.cpp` is the dispatcher between them and the application:
one bounded decision per HTTP route and USB command, over the twenty callbacks
`optical_main.cpp` implements. It touches no SD primitive and never delays, so
`request_reboot` only records the request and `loop()` performs the reset.

`src/aius_fault_teensy.cpp` is inert in a production image; in a
`teensy41-optical-fault` build it stalls forever at the selected checkpoint, so
the recovery path has something real to recover from.

### Staged firmware upgrade (OTA)

Behind the `AIUS_OTA` compile-time option (off by default; the `teensy41-optical`
env sets it), `optical_main.cpp` composes the staged-upgrade surface
(docs/plans/aius-ota-upgrade.md): a single `AiusOtaSession`, the compiled-in
Ed25519 public key, the RAM-resident flash primitives and activation sequencer,
and the nine `AiusOpticalAppOta` callbacks installed on the dispatcher with
`aius_optical_app_set_ota` after the runtime is constructed. The routes are
`GET /api/ota` (open read), `PUT /api/ota/image` (the resumable Content-Range
chunk — a 256-byte container header at offset 0 then the payload), and
`POST /api/ota/{arm,verify,abort,activate,enable-activation,self-test-flash}`; the
dispatcher owns their absent-then-token gate. `GET /api/status` grows an
`ota_diag` object. `enable-activation` and `self-test-flash` are auth-gated
bench/operator surfaces and are deliberately not in the OpenAPI contract (only
`/api/ota/activate` is documented).

The feature latch is armed at composition — the arm/verify/abort verbs run the
staged-upgrade state machine up to and including `ARMED` (`POST /api/ota/arm` is
`aius_ota_arm_image`, verified → armed). The compiled-in key is the committed
**home** deployment keypair (`config/ota-home-signing-key.{pub,sec}`, the
2026-08-04 rotation; fingerprint `df29355e5a8d7b8c`, reported as `test_key`
false). The `test/ota/test-only-signing-key.{pub,sec}` pair is the host suite's
fixture only: a container it signs is refused by the device at verify with
`409 ota_signature`. The fingerprint is the first 8 bytes of `SHA-256(key)` in
hex. Verify runs the Ed25519 check over the header's authentication digest
against that key.

**Durable staging.** `PUT /api/ota/image` drives `aius_ota_absorb` (incremental
SHA-256, which is what `verify` checks) AND writes each container chunk to
`/aius/ota-staged.img` through the bounded SD job engine (`AIUS_SD_INSTALL_APPEND_PART`
at the chunk's file offset, exactly the replay upload); the 200 is the SD job's.
`absorb` hashes at dispatch while the SD write is async, so a chunk whose write
fails leaves the session hash ahead of the card — which the activation's
pre-window re-digest catches (below), so a corrupt image faults activation rather
than ever being flashed. A reboot mid-staging comes up disarmed (the session does
not persist), leaving the file inert until a fresh offer.

**Activation — verify before the window, byte-compare within it.** A second,
runtime `g_ota_activation_enabled` latch (distinct from the feature latch, false
at every boot) gates the flash trigger: while it is off `POST /api/ota/activate`
answers `409 ota_activation_disabled` and `activation_available` is false.
`POST /api/ota/enable-activation` flips it — the deliberate operator act performed
on the bench after `POST /api/ota/self-test-flash` (the item-5 scratch-sector
recovery proof: real erase/program/read-back against one sector 4 MiB into flash,
far above the image, asserted above `_flashimagelen`) passes. The activate verb
also refuses synchronously with `ota_no_psram` when no PSRAM is fitted
(`external_psram_size == 0` AND the pad-2 probe `g_psram_pad2_mb == 0`), reported
in `ota_diag`. When enabled and `ARMED`, `activate` acknowledges
(`{"ok":true,"activating":true}`), and after the response drains — the deferred
shape `POST /api/reboot` uses — `service_ota_activation` does the whole
verification while flash and SD still run: it copies the staged image SD→PSRAM
into a region disjoint from the flight-recorder ring (`ota_psram_staging`, above
the ring, runtime-asserted), then `aius_ota_activate_begin` re-runs SHA-256 +
Ed25519 over that PSRAM copy against the armed header and writes the health
in-progress mark — aborting with nothing flashed on any mismatch. Only then does
the board quiesce (gate closed, audio service stopped, ISR sources parked,
discharging RM note 3) and run the dark window, which erases → programs from the
PSRAM view → byte-compares the readback against it (an inlined compare, no digest,
no SD), FCB erased first and programmed last, then `SYSRESETREQ`s into the new
image. The flash primitives, the run-to-completion finalizer AND the reworked
in-window sequencer step path are all `FASTRUN` (RAM-resident, `.fastrun` → ITCM;
`aius_ota_activate.c`'s pre-window `begin`/verify stays `FLASHMEM`), guard the
driver's two gotchas (PRIMASK save/restore around each op; every address
range-checked through `aius_ota_activate_check_*` before the no-bounds-check
driver), and feed the watchdog through a distinct RAM-resident `WDOG1_WSR` writer
since the FLASHMEM `aius_boot_teensy_watchdog_feed` cannot run mid-erase.

`vendor/tweetnacl/tweetnacl.c` is verbatim and un-annotated, so
`link/aius_teensy41.ld` pins its `.text` into flash and excludes it from ITCM;
with `AIUS_OTA` undefined the whole surface plus the linked `lib/aius_ota` and
TweetNaCl objects are dropped by `--gc-sections`, so the option is image-neutral
off.

### SD lifecycle — bounded jobs, one primitive per epoch

The Teensy SD library is synchronous and offers no cancellable call, so a
checkpoint performing two primitives would double the worst case the watchdog
has to absorb. Every SD path is therefore a portable job driven one primitive
per epoch at checkpoint 8, with the checkpoint-7 driver in `optical_main.cpp`
choosing the next step.

The root-initiated lifecycle is a `PROBE` on a one-second tick; a card that
appears runs `CONFIG_LOAD` for `/aius/config.json` then `/aius/dsp.json`, then a
`USAGE_REFRESH`. Boot and hot re-insert are the same transition. HTTP config
`PUT`s take the other owner: the dispatcher stages the validated bytes and
returns an `UPLOAD` job, and the client's 200 is withheld until that write
commits. One job runs at a time and neither owner preempts the other — a boot
load that lands mid-upload waits, so a config can be late but never lost.

### Provisioning the card, and the token you must keep

The device serves its own control surface from the card, and every write to it
over HTTP needs the SD maintenance token. **Read the token over USB and record it
before the board is deployed without a USB host** — USB is the only channel that
discloses it, so once the cable is gone there is no way to recover it from the
device.

```text
$ AIUS AUTH 3
AIUS/1 OK 3 AUTH 6b3ff32e4c2505cc
```

It is stable for the life of the board: `derive_auth_token()` derives it from the
MAC words fused into the chip at manufacture, so it survives reboots, reflashes
and card swaps, and it is unique per board. Keep it in the estate's secret store
rather than in this repository — it authorises network writes to the card. If it
is lost, reattaching USB and reading it again is the only recovery. Full contract:
[`docs/reference/usb-control-protocol.md`](../docs/reference/usb-control-protocol.md).

Upload the web tree with:

```bash
tools/aius-sd-sync.sh --device http://<address> --www sd/www --token-file <path>
```

Add `--file <name>` (repeatable, relative to `--www`) to upload only the named
files, e.g. `--file openapi.json`; each upload during playback costs dropouts, so
send only what changed.

The tool retries each file. That is not decoration: the device serves one HTTP
client at a time and drops a connection arriving before the previous one is
released, so a sequential run hits that window roughly once in ten uploads. A
large upload has also been seen to fail once with `500 sd_failure` and succeed on
retry, which is an open defect rather than an understood one — see the plan's
Phase 14f notes before assuming a failed sync is benign.

Confirm the result by opening `/sd/www/` in a browser: the directory index lists
every file with its size, so a truncated or missing upload is visible rather than
being indistinguishable from a file you simply have not requested.

### Recovery model

**The watchdog is armed at a pinned 8 s and fed exactly once per complete
epoch.** `aius_boot_teensy_watchdog_arm()` writes `WDOG_WCR_WT(15)`, i.e.
16 × 0.5 s, and `platform_feed_watchdog()` — reached only from
`EPOCH_COMPLETE` — is the only place the firmware writes the service register.

Before each service call the current checkpoint is committed to a retained
ledger through the boot adapter's double-slot storage in `.bss.dma`, CRC-covered
so a torn write reads back invalid. After a reset the ledger decides the boot
generation, the early-reset count and the mode: a watchdog reset whose last
checkpoint was not `EPOCH_COMPLETE` increments the count, and the second
consecutive one selects **recovery-safe** mode. A changed build id clears both.

In recovery-safe mode the scheduler runs only `USB_RX` and `USB_TX` before
`EPOCH_COMPLETE`. No network, SD or replay work happens at all, so a board
looping on a wedged service still enumerates over USB and can be re-flashed.

The count is cleared by a complete epoch reached at least 10 s after
construction, but only in normal mode — recovery-safe is deliberately sticky and
is left only by flashing an image with a different build id.

### Memory discipline

ITCM shares the 512 KiB FlexRAM with DTCM, so cold code left in ITCM is taken
straight out of the audio path's headroom. Three rules, all enforced:

- **`FLASHMEM` on every cold definition** — not on a prototype, because GCC does
  not merge a `section` attribute from a forward declaration. Each portable
  source defines a no-op fallback so the host suites still compile Arduino-free.
- **Large buffers in `DMAMEM`** (OCRAM). The composed runtime is roughly 35 kB,
  dominated by the HTTP request/response buffers and the model-sized application
  body buffer, against a DTCM free-for-locals zone of **25,696 bytes**
  (`_estack - _ebss` on the canonical `teensy41-optical-slot-a` image at
  aius-sample-rate-following Phase 4e start; 25,728 B on the plain image; it
  moves with every static addition, so read it from the build); a DTCM
  instance could not fit. The
  active and pending DSP models, the replay service and the three UDP buffers are
  placed the same way. That zone is the whole stack for both the main loop and
  audio ISR. `enqueue()` uses about 4 kB of frame at the bounded AUDIO
  checkpoint; the ISR now only publishes ingress. Since Phase 4e a nested
  producer pass also runs on the main stack under an SD wait or a checkpoint
  yield point; `stack-bound` proves the deepest path plus 2,048 B fits the
  zone, and `audio_diag.stack_free_min_bytes` is the device floor (boot
  paint, 1 Hz scan).
- **Nine post-link ITCM checks**, in `test/itcm-placement/verify_target.py` and
  run by every optical build. They reject executable `.text` from declared cold
  objects, pin the mixed ingress library to its five producer entry points, pin
  the producer-yield object (Phase 4e) to its guard, wait, point and visit
  entry points plus the nested-pass wrapper, and pin `optical_main.cpp` to its
  exact `HOT_SECTIONS` set (the ingress producer, the per-decode DWT reader and
  the stall witness); the last three read the linked image for the OTA
  dark-window, TX and flash-pinned residency. New objects under a cold engine tree must be declared,
  so "this is legitimately hot" remains a reviewable claim rather than an
  opinion. The fourth closes the half `build_src_filter` paths cannot reach:
  PlatformIO's LDF lands each resolved library at `lib<hash>/<libname>/`, so
  every object of that shape is grouped by library and a library emitting any
  `.text` must be named in `ITCM_ALLOWED_LDF_LIBS` — a new library, or a second
  source inside a library that was cold, fails the build by default (the
  recorder, config and auth libraries are gated to zero since 2026-09-02, when
  every definition went `FLASHMEM`; the named recorder-helper check that
  predated that retired with it). The fifth is the precondition for the other
  four: all of them read one object file at a time, so the guard refuses an
  environment whose `platformio.ini` flags contain `-flto`.
- **A six-block ITCM link ceiling.** The linker rejects
  `_itcm_block_count > 6`; otherwise a small code increase could silently take a
  seventh 32 KiB FlexRAM block and remove the same amount from the shared
  stack/local zone.
- **A post-link audio-memory guard**, in `test/memory/verify_target.py`, pins the
  16 KiB OCRAM ingress, the 213.6 KiB DTCM decoder arena, zero OCRAM recorder
  staging, and a per-env OCRAM free floor (`OCRAM_FREE_FLOORS`: 131,776 B plain
  and fault, 131,744 B slot-a and slot-b, the Phase 4e start measurements,
  above the 131,040 B `0x1FFE0` link floor). A floor only rises to a fresh
  measurement, so any OCRAM growth in any optical env fails the build.

`scripts/build.sh` prints the per-bank report and fails when a bank is over
capacity; `scripts/test.sh --suite itcm-placement` re-runs the ITCM checks over
an existing build.

## Running host tests

Requires `gcc`. From anywhere, `scripts/test.sh --environment native --suite <name>`.
`latency-runner` additionally requires Python NumPy and `ffmpeg`; it builds the
native production-pipeline reference renderer at
`.build/native/latency-native-renderer`. Live collection requires
`libasound.so.2` and readable board status and DSP-config endpoints.
Suites (all build with `-Wall -Wextra -Werror` + ASan/UBSan):

| Suite | Covers |
| --- | --- |
| `ab` | A/B boot state (`lib/aius_ab`): flash map arithmetic, boot-record + trial-ledger codecs, ping-pong pick, the chooser decision rule and slot IVT sanity. |
| `arena`, `codec-api-contract` | The allocator and ABI contract. |
| `audio-ingress` | Exact 256-word SPSC publish, wrap, gaps, flush and diagnostics. |
| `audio-service` | Deferred ingress/service quotas, suffix retry, resets and backpressure. |
| `clock` | Stable 32/44.1/48 kHz acquisition, quiescence, target cadence and PLL-plan arithmetic. |
| `clock-target` | Fake-register target adapter: bounded full apply, exact four-period invalid hold, queued NUM trim, timeout fail-dark behavior, and SRFM millihertz measurement. |
| `dts` | DTS Core decode differential vs FFmpeg (committed corpus). |
| `dts-arena` | No-heap arena decode path + allocation metrics. |
| `dts-fuzz` | DTS decoder robustness fuzz. |
| `dts-lfe` | LFE bit-exactness vs a full dcadec build. |
| `a52` | AC-3 decode differential vs FFmpeg (committed corpus), channel identity by Goertzel, arena/metrics contract. |
| `a52-fuzz` | AC-3 adapter robustness fuzz: seam hunt + random corruption under the termination oracle, including the fork's CRC and bounded bit-reader guards. |
| `ac3-vectors` | The gate-5 stimulus generator (`tools/ac3_vectors/gen_iec_stream.py`): WAV-wrapped IEC 61937 AC-3 at 44.1/48 kHz, the burst scan, the odd-byte / truncated-period refusals and exit codes; the 1 s ffmpeg case skips without ffmpeg. |
| `eac3` | E-AC-3 decode differential vs FFmpeg (committed corpus), regression hash, arena/no-alloc steady state, malformed family. |
| `eac3-fuzz` | E-AC-3 adapter robustness fuzz: seam hunt + random corruption under the termination oracle. |
| `iec61937`, `iec61937-fuzz` | IEC 61937 parser known-answer + fuzz. |
| `replay-samples` | Generate additive DTS/AC-3 replay packs, validate AC-3 IEC periods and payloads, and consume them through the production IEC parser. |
| `pipeline` | Classifier + parser→decoder end-to-end, including the AC-3 / E-AC-3 syncframe CRC gate (corpus spans, single-bit flips) and its silent substitution (output-jitter Phase I). |
| `capability-protocol`, `capability` | USB protocol goldens + sim device round-trip. |
| `dsp`, `dsp-vectors` | Downmix + EQ known-answer, shared production-model runtime resolution, and the public DSP interface over the DTS corpus. |
| `downmix-identity` | Decode → downmix composed channel identity: each source channel's own tone reaching the stereo slot its route names, at that route's coefficient, for DTS and AC-3, with a rotated-front-three negative control. |
| `measure` | The measurement loopback's portable core: every capture state-machine edge (both dual-terminal cancel interleavings included) and the sliced sine-fit + Goertzel analysis known answers, with the pure-sine leakage canary whose tolerance is never widened. |
| `auth` | The SD maintenance token's derivation, firmware side and the host generator together. |
| `config`, `dsp-config` | The optical DSP config header generator and the runtime feature-flag parser; plus the `PUT /api/dsp/config` validator. |
| `json` | The one JSON read cursor those two parsers and the replay body reader share: bounded by length, allocation-free, and strict about the value terminators (`,` `}` `]` or end of input) that keep `1 2` and `truex` out. |
| `net`, `net-runner` | UDP codec + network supervisor oracle; the host-side runner tools. |
| `rate-runner` | The read-only sample-rate bench collector and pure analyzer, including drift, transition, mismatch, encoded and waveform evidence gates. |
| `rate-gate-capture` | The host-side optical capture reducers behind gates 2, 4 and 7: the output-rate model and its self-referential marking, the two capture↔status time origins, the amplitude-derived discontinuity detector at the documented ±410 stimulus (held block, 1- and 2-sample slips), the flight-recorder fiducial measurement of `old_rate_output_frames` and its no-transition-captured failure, and arecord's exit/overrun/short-WAV faults. Synthetic waveforms; no board and no ALSA device. |
| `latency-runner` | The formal linked-ALSA latency harness: exact carrier/config/build identity, retained native decoded and rendered streams, AC-3 unit and payload-hash oracles, unique mode-specific probes, ambiguity/SNR/clipping/affine gates, exact linked-trigger evidence, runtime status acceptance and fail-closed manifests. |
| `recovery-runner` | The live recovery smoke's host runner: every gate on all three result paths against a scripted device on a virtual clock. |
| `flash` | The button-free flash path: HalfKay entry judged by the port going away rather than by an acknowledgement, the image-identity oracle over synthesised Intel HEX, and every fail-closed arm. |
| `http` | Request parser + the bounded server transaction. |
| `usb-control` | USB control command parser + runtime transaction. |
| `replay` | Replay service over a fake reader. |
| `tx-gate` | The transmit-gate predicate's whole truth table (no carrier and no replay → no output carrier). |
| `tx-governor` | The latency governor's window-minimum floor estimator and one-skip trim decision (aius-output-jitter Phase E). |
| `tx-fill-sensor` | The phase-corrected transmit-ring fill statistic: DMA-position occupancy, 16 sub-window minima, exclusions, torn-read retry and reset (aius-sample-rate-following Phase 3). |
| `tx-fill-witness` | The transmit ISR's missed-fill detector (same DMA half filled twice = an interrupt a whole period late, so the DMA repeated a stale half) and the ring rebase that stops the miss from standing as permanent latency. |
| `underrun-episodes` | The gate-open underrun episode record served in `clock.underrun_episodes`: episode boundaries at the accepted-block cadence, the eight-deep ring, closed-gate truncation, and the millisecond and counter wraps. |
| `input-liveness` | Why nothing is reaching the transmit sink and for how long, over the gate-closed region the production-gap watermark discards (aius-input-recovery Phase 1). |
| `input-dark` | The observe-only dark-interval record (entry/exit phase, class, tracker acceptance, gate) and the per-phase gate-closed watermark served in `input_diag` (aius-input-recovery Phase 1). |
| `event-ring` | The device event ring: depth against an OSD-navigation burst, overwrite accounting, and `cursor` paging for GET /api/events and UDP GET_EVENTS (aius-input-recovery Phase 1). |
| `recorder` | The flight recorder's portable core: freeze state machine, trigger edges/qualification, exact eight-step × 512-frame staging burst, PSRAM carve geometry, drain + gap accounting, chunk arithmetic, and the firmware-wide extmem-heap ban. |
| `boot`, `boot-target` | Retained ledger and recovery-mode rules; the Teensy boot adapter + its post-link verifier. |
| `sd`, `sd-target` | Portable SD job machine; the one-shot Teensy SD adapter. |
| `runtime`, `runtime-target` | Epoch scheduler; the composed runtime against fake primitives. |
| `network-transport` | NativeEthernet behaviour + the vendor-header source contract. |
| `optical-app` | One bounded dispatch decision per HTTP route and USB command. |
| `ota` | The OTA image container's per-field validation, the staged-upgrade state machine (strict chunk offsets, resume, incremental digest, injected verifier seam), the power-loss record and the slot-independent image-health rules; then the real Ed25519 behind that seam — RFC 8032 vectors 1 and 2 through the vendored library, accept plus every single-bit refusal in the signature, digest and public key, the entropy trap firing in a forked child, and the host signer's containers driven end to end to `ARMED`; and the flash-activation sequencer over injected primitives and a linear staged view — verify-before-window (SHA-256 + signature in `begin`, digest/signature refusals before anything is flashed), in-window byte-compare catching a mis-programmed sector, FCB-erased-first / programmed-last proven by operation trace, watchdog cadence, and power-loss truncation at every step. |
| `optical-cutover` | Structural guard on `optical_main.cpp`: construction-only `setup()`, one-epoch `loop()`, ingress-only audio callback, deferred AUDIO ownership/preflight, one watchdog feed, exact storage/diagnostics, and the absence of the legacy path. |
| `status-body-bound` | Worst-case `GET /api/status` body, summed from the emitters' printf formats (ILP32 conversion widths, loop bounds and `%s` sizes resolved from source, fail-closed on anything unmodelled), against `AIUS_RUNTIME_TEENSY_STATUS_BODY` (16,384 B) plus its NUL; also run at a limit equal to the worst case, which must fail. |
| `fault-target` | Fault-image source guard + checkpoint selection. |
| `build-metadata` | Compile-time build id, revision and fault-selector derivation. |
| `memory` | The per-bank release gate plus the optical post-link ingress/arena/staging/headroom proof against the per-env OCRAM floor table (a snapshot 1 B under any env's floor and an env with no entry both fail); the native suite is driven over a committed ELF reduction of a real optical image. |
| `sdfat-vendor` | The vendored SdFat and SD snapshots (Phase 4e): upstream and patched SHA-256 source contract, the SD wait hook only in the `yieldTimeout` and ACMD41 loops, the DTCM-only DMA bounce, the FAT volume-serial accessor. |
| `producer-yield` | The SD-wait and checkpoint producer yield (`lib/aius_producer_yield`) over a fake cycle clock: guard truth table, pend stamp, the nested-pass ledger wrapper, latency and window non-yield watermarks, clear. |
| `producer-isolation` | The post-link walk over the optical images: producer roots reach no SD, network, USB or stdio symbol, no hook under an allocator or stdio function, every hook-reaching SD caller and yield point is a table row; plus the link-map provenance of the vendored SD stack. Fixture ELFs always; real-ELF cases need a build. |
| `stack-bound` | Worst-case call depth over the same walk plus the exception chain, against the canonical image's DTCM stack zone less 2,048 B. Fixture ELFs always; real-ELF cases need a build. |
| `itcm-placement` | The post-link ITCM checks, re-run over an existing optical build, plus the host-only tests of the LDF-library gate, the producer-yield pin and the `-flto` precondition. |
| `fake-fidelity` | Every host fake's constants against the real framework headers, three ways: a listed constant must exist upstream with the same value, *every* enum member `fake/fnet.h` declares must match FNET's own (no name list, so a positional renumber cannot hide), and a fake must not invent a constant. Each kind is then re-run over a mutated copy of the fake, so the suite proves it can still see the defects it was written for. A fake with a wrong constant agrees with itself and with the tests asserting the same macro, so only an external reference catches it. |
| `registry` | The guard on this table: fails when it, the `# Suites:` header in `scripts/test.sh` and that script's `case` arms disagree with the directories under `test/`. |
| `sd-editor` | The SD-card web pages (status, DSP editor, replay manager, upgrade) as vitest suites at the repo root's `test/sd-editor/`, run with this repo as the vitest root; needs `pnpm` (resolved through the superproject when this repo is a submodule). |

The `dts`/`dts-lfe` suites run against committed reference PCM, so `ffmpeg` /
`dcadec` are needed only to regenerate the corpora, not to run the suites. This
firmware's own `scripts/test.sh` is the test entry point; the repo-root
`scripts/test.sh` is a .NET/vitest/pytest discovery runner and does not drive the
C suites.

Every suite is host-only and self-contained except `itcm-placement`, which
inspects the linked objects of a cross-build (`producer-isolation` and
`stack-bound` also read one, but skip their real-ELF cases without it). It exits 3 with the build command
to run when no image is present, and reads
`.pio/build/$AIUS_ITCM_ENV` (default `teensy41-optical`).

Cross-build for the Teensy with `scripts/build.sh` (needs PlatformIO; see its
header for the pinned-venv recipe). Both optical images must build with every
bank fitting and every post-link check passing:

```bash
scripts/build.sh teensy41-optical
scripts/build.sh teensy41-optical-fault 0
```

### Optical flashing session rule

**The running optical image enters HalfKay on request — no Program button.** Send
`AIUS BOOTLOADER <id>` over the USB CDC control plane and the image flushes its
response, waits for the descriptor to retire (bounded at 500 ms), and enters the
bootloader. Measured at five consecutive trials with a full reflash between each,
every one entering on the first attempt. The Program button remains the
unconditional fallback for an image that will not run at all.

Note the two USB paths need different groups, and the login is stale, so both
need a wrapper: `sg dialout -c` for the CDC port, `sg plugdev -c` for the loader.

Start the loader before requesting bootloader entry:

```bash
sg plugdev -c '$HOME/.platformio/packages/tool-teensy/teensy_loader_cli --mcu=imxrt1062 -w -v .pio/build/teensy41-optical/firmware.hex'
```

The loader owns a long-running interactive session while it waits for HalfKay.
The caller must keep that session handle, poll it until it exits, and only then
continue with the post-flash probe. A later conversation turn is not an
automatic completion callback. Treat `Found HalfKay Bootloader`, `Booting`, and
the process exit status as the flash completion evidence; do not report the
flash as complete while the loader is still waiting.

## Status

Everything that can be done without the physical Teensy is done and green on the
host: the portable ABI + arena, the IEC 61937 parser (P2), the `dcadec` DTS Core
decoder bit-exact to a full dcadec build including LFE (P3, no-heap arena path),
the hardened liba52 AC-3 decoder differential-tested against FFmpeg,
the reproducible Teensy cross-build + per-bank memory report (P4, all banks fit),
the USB capability protocol + simulated device + host runner (P5), the
PCM/DTS/AC-3 classifier + end-to-end pipeline, and the 5.1→stereo downmix + EQ
output DSP.

The P6 target entry point is `teensy41-capability`, using Teensy RawHID with the
same framed protocol and device handler as the host simulator. Build it with
`scripts/build.sh teensy41-capability`, flash it with PlatformIO/Teensy loader,
then run `scripts/flash_and_run.sh .pio/build/teensy41-capability/firmware.hex \
tools/dts_vectors/vectors`. The wrapper requests the running image to enter
HalfKay, flashes it, resolves the new application interface by VID/PID and
interface number, and writes a raw transcript unless an audit path is supplied.
Large RawHID commands use acknowledged 32-byte fragments, because longer logical
reports were observed to corrupt. The physical P6 corpus passes all 40 vectors;
P7 soak remains.

Set `AIUS_HID_ITERATIONS` when invoking `scripts/run_hid.sh` to repeat the full
corpus in one device session; use `/dev/null` as the audit path for a long soak.

The optical target has completed its runtime cutover and **runs on hardware**,
serving HTTP at its DHCP address. Confirmed live: boot, one epoch per `loop()`,
the watchdog not resetting in normal operation, SD probe and both config loads,
the status surface on all three channels, software bootloader entry, the SD
directory index, DHCP reaching ONLINE on the first network generation, and the
**audio path end to end** — the DSP runs, `GET /api/meters` publishes measured
per-block cycles, and replay plays continuously across loop wraps at 48 kHz with
zero overruns and zero underruns.

**`POST /api/reboot` is deliberately not in that list.** It was, and the claim was
wrong: the board reset but never delivered the response, because the reset fired
in the epoch that dispatched it rather than after the answer reached the wire. The
old evidence — watching `boot_generation` increment — is exactly what cannot
distinguish "performed" from "acknowledged". The reset now waits for the
transaction to drain, bounded at 500 ms, and the four control routes
(`/api/reboot`, `/api/start`, `/api/stop`, `/api/clear`) are gated on
`X-Aius-SD-Auth`. **B6 gate 2 passed on hardware 2026-07-26**, distinguishing
the delivered response from the subsequent reset.

The S/PDIF input has confirmed carrier plus live PCM passthrough. Live DTS
decode passed for 1,857 seconds on the tagged +438 ppm clock-isolation image.
On exact production image `bc05232880a5+teensy41-optical`, bounded AC-3 also
passes both source paths: 120 seconds from CM6206 IEC958 and 120 seconds from
clock-coherent SD replay, with continuous decode/output progress, no
parse/decode rejection, AUDIO overrun, drop, abandon or watchdog reset, and
completed-AU DWT measurements around 1.114 million cycles. The on-device
committed-wire oracle identifies all six 5.1 channels with 100.52 dB or better
unwanted-side rejection and centre/LFE left-right matching reported as 0.00 dB.
Sample-rate following remains the technical prerequisite for the four-hour TV
and by-ear mission gates. The owner has deliberately scheduled those gates
after the OTA investigation verdict as well; the bounded runs do not waive
them.

Three HTTP interop defects are open, all found by pointing a non-browser client
at the board and all making the control surface work only for the exact byte
shapes the bundled browser surface happens to emit: request header names are
matched case-sensitively against `X-Aius-SD-Auth`, a request whose body arrives
in a second TCP segment gets no response at all, and the replay body parser
rejects the insignificant JSON whitespace `json.dumps` emits by default. Until
they land, drive the board with compact JSON in a single write and exact header
case.

`tools/net/aius_http_runner.py` and `tools/net/aius_net_runner.py` exercise a
running board over HTTP and UDP. The HTTP soak drives only routes the firmware
still serves; the coverage that leaves behind is recorded in its module
docstring.
