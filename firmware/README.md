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
| `lib/aius_pipeline/` | PCM/DTS classifier + parser→decoder end-to-end chaining. |
| `lib/aius_dsp/` | 5.1→stereo downmix + biquad EQ output DSP. |
| `lib/aius_capability/` | USB capability wire protocol + arena-driven device handler. |
| `lib/aius_replay/` | Replay service over a caller-supplied `AiusReplayReader`. |
| `lib/aius_boot/` | Retained boot ledger: generation, checkpoint, reset kind, recovery mode. |
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
| `src/` | Teensy entry points (`main.cpp`, `capability_main.cpp`, `optical_main.cpp`) + the Teensy adapters. |
| `tools/device/` | Simulated device + host runner, including the P6 hidraw transport. |
| `tools/net/` | HTTP and UDP exercisers for a running optical board. |
| `tools/dts_vectors/`, `tools/iec_corpus/` | Committed DTS/IEC test corpora + generators. |
| `tools/dcadec_ref/` | dcadec-reference generator for the LFE bit-exactness suite. |
| `tools/measure_vectors/` | Committed PCM stimulus vectors for the measurement loopback + their generator/validator (preamble-free and loop-coherence obligations asserted at generation, revalidated by the `measure` suite). |
| `tools/memory/` | Linker-map → per-bank memory report. |
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

**The engine runs at 48 kHz, and `teensy41-optical` pins it with
`-DAUDIO_SAMPLE_RATE_EXACT=48000.0f`.** `AudioStream.h` defaults that macro to
`44100.0f` behind an `#ifndef`, and `config_spdif3()` derives the whole S/PDIF
transmit clock tree from it — so the default silently made the optical output
44.1 kHz while the DTS decoder, the PCM passthrough tag, the DSP config, the meters
and the replay seam all said 48,000. The audible result was 48 kHz material playing
8.1 % slow, about 1.4 semitones flat, with every counter honestly zero because the
producer and the consumer both ran off the same wrong clock. DTS Core is 48 kHz, so
48 kHz is the rate this decoder must run at. `optical_main.cpp` `static_assert`s the
build flag equal to `AIUS_OPTICAL_SAMPLE_RATE_HZ`, which
`scripts/build_config.py` generates from `config/optical_dsp.json`.

**Nothing in the audio path resamples.** Neither S/PDIF peripheral converts rate, and
no code compares a decoded block's `sample_rate_hz` against the engine, so a
disagreement is neither absorbed nor counted — the queue stays level and the only
symptom is pitch. That is why the equality is enforced at compile time.

**Two block sizes, and the per-block figures name different ones.**
`AUDIO_BLOCK_SAMPLES` is the core default 128 frames and is the *engine* block, so
`update()` runs 375 times a second. The *decoded* block is 512 frames — a DTS Core
access unit, and `AIUS_PIPELINE_PCM_BLOCK_FRAMES` for passthrough — so `enqueue()`,
which is what `blocks` counts and what the DWT cycle counters bracket, runs 93.75
times a second with a 10,667 µs budget.

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
tools/aius-sd-sync.sh --device http://<address> --www sd/www --token <token>
```

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
  body buffer, against a DTCM free-for-locals zone of **rather under 10 kB**
  (`_estack - _ebss`, `teensy41-optical`; 9,248 bytes as of `c20a03c8e`, and it
  moves with every static added, so read it from the build); a DTCM instance
  could not fit. The
  active and pending DSP models, the replay service and the three UDP buffers are
  placed the same way. That zone is the whole stack, main loop and audio ISR
  both — `enqueue()` alone places about 4 kB of frame in it from the audio
  interrupt path.
- **Two post-link ITCM guards**, in `test/itcm-placement/verify_target.py` and
  run by every optical build. The first fails the link if any declared cold
  object emits executable `.text`, and makes the declaration non-optional: a new
  object under a cold engine tree that nobody declared fails the build. The
  second pins the one object that may contribute ITCM, `optical_main.cpp`, to an
  exact per-symbol allowlist of its audio-block functions, so "this is
  legitimately hot" is a reviewable claim rather than an opinion.

`scripts/build.sh` prints the per-bank report and fails when a bank is over
capacity; `scripts/test.sh --suite itcm-placement` re-runs the two guards over
an existing build.

## Running host tests

Requires `gcc`. From anywhere, `scripts/test.sh --environment native --suite <name>`.
Suites (all build with `-Wall -Wextra -Werror` + ASan/UBSan):

| Suite | Covers |
| --- | --- |
| `arena`, `codec-api-contract` | The allocator and ABI contract. |
| `dts` | DTS Core decode differential vs FFmpeg (committed corpus). |
| `dts-arena` | No-heap arena decode path + allocation metrics. |
| `dts-fuzz` | DTS decoder robustness fuzz. |
| `dts-lfe` | LFE bit-exactness vs a full dcadec build. |
| `a52` | AC-3 decode differential vs FFmpeg (committed corpus), channel identity by Goertzel, arena/metrics contract. |
| `a52-fuzz` | AC-3 adapter robustness fuzz: seam hunt + random corruption under the termination oracle (liba52 has no CRC and an unbounded bit reader — the padded scratch is what this proves). |
| `iec61937`, `iec61937-fuzz` | IEC 61937 parser known-answer + fuzz. |
| `pipeline` | Classifier + parser→decoder end-to-end. |
| `capability-protocol`, `capability` | USB protocol goldens + sim device round-trip. |
| `dsp`, `dsp-vectors` | Downmix + EQ known-answer, and the same through the public DSP interface over the DTS corpus. |
| `measure` | The measurement loopback's portable core: every capture state-machine edge (both dual-terminal cancel interleavings included) and the sliced sine-fit + Goertzel analysis known answers, with the pure-sine leakage canary whose tolerance is never widened. |
| `auth` | The SD maintenance token's derivation, firmware side and the host generator together. |
| `config`, `dsp-config` | The optical DSP config header generator and the runtime feature-flag parser; plus the `PUT /api/dsp/config` validator. |
| `json` | The one JSON read cursor those two parsers and the replay body reader share: bounded by length, allocation-free, and strict about the value terminators (`,` `}` `]` or end of input) that keep `1 2` and `truex` out. |
| `net`, `net-runner` | UDP codec + network supervisor oracle; the host-side runner tools. |
| `recovery-runner` | The live recovery smoke's host runner: every gate on all three result paths against a scripted device on a virtual clock. |
| `flash` | The button-free flash path: HalfKay entry judged by the port going away rather than by an acknowledgement, the image-identity oracle over synthesised Intel HEX, and every fail-closed arm. |
| `http` | Request parser + the bounded server transaction. |
| `usb-control` | USB control command parser + runtime transaction. |
| `replay` | Replay service over a fake reader. |
| `tx-gate` | The transmit-gate predicate's whole truth table (no carrier and no replay → no output carrier). |
| `recorder` | The flight recorder's portable core: freeze state machine, trigger edges/qualification, staging drain + gap accounting, chunk arithmetic, and the firmware-wide extmem-heap ban. |
| `boot`, `boot-target` | Retained ledger and recovery-mode rules; the Teensy boot adapter + its post-link verifier. |
| `sd`, `sd-target` | Portable SD job machine; the one-shot Teensy SD adapter. |
| `runtime`, `runtime-target` | Epoch scheduler; the composed runtime against fake primitives. |
| `network-transport` | NativeEthernet behaviour + the vendor-header source contract. |
| `optical-app` | One bounded dispatch decision per HTTP route and USB command. |
| `optical-cutover` | Structural guard on `optical_main.cpp`: construction-only `setup()`, one-epoch `loop()`, one watchdog feed, and the absence of the legacy path. |
| `fault-target` | Fault-image source guard + checkpoint selection. |
| `build-metadata` | Compile-time build id, revision and fault-selector derivation. |
| `memory` | The per-bank release gate `build.sh` fails the build on, driven over a committed ELF reduction of a real optical image. |
| `itcm-placement` | The two post-link ITCM guards, re-run over an existing optical build. |
| `fake-fidelity` | Every host fake's constants against the real framework headers, both directions: a listed constant must exist upstream with the same value, and a fake must not invent one. A fake with a wrong constant agrees with itself and with the tests asserting the same macro, so only an external reference catches it. |
| `registry` | The guard on this table: fails when it, the `# Suites:` header in `scripts/test.sh` and that script's `case` arms disagree with the directories under `test/`. |

The `dts`/`dts-lfe` suites run against committed reference PCM, so `ffmpeg` /
`dcadec` are needed only to regenerate the corpora, not to run the suites. This
firmware's own `scripts/test.sh` is the test entry point; the repo-root
`scripts/test.sh` is a .NET/vitest/pytest discovery runner and does not drive the
C suites.

Every suite is host-only and self-contained except `itcm-placement`, which
inspects the linked objects of a cross-build. It exits 3 with the build command
to run when no image is present, and reads
`.pio/build/$AIUS_ITCM_ENV` (default `teensy41-optical`).

Cross-build for the Teensy with `scripts/build.sh` (needs PlatformIO; see its
header for the pinned-venv recipe). Both optical images must build with every
bank fitting and both ITCM guards passing:

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
the reproducible Teensy cross-build + per-bank memory report (P4, all banks fit),
the USB capability protocol + simulated device + host runner (P5), the PCM/DTS
classifier + end-to-end pipeline, and the 5.1→stereo downmix + EQ output DSP.

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
`X-Aius-SD-Auth`. **Confirmation is B6 gate 2**, which has not yet run.

Not yet confirmed on hardware: the S/PDIF input path. The optical input is
unsoldered, so there is no carrier, passthrough and DTS decode have never run
against a live stream, and the per-block cost is
still a static estimate.

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
