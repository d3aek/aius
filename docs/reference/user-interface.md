---
title: Aius user interface
type: reference
scope: component
component: aius
status: current
tags: [aius, ui, http, sd]
updated: 2026-10-17
toc: true
---

# Aius user interface

Aius serves its whole operator interface from the Teensy's SD card: three HTML
pages in one bench-style skin (nav labels `dsp`, `status`, `firmware`) that read
and write the device's own HTTP routes. This document shows each page with a
populated response behind it, what it reads and writes, and what a reader could
not guess from the markup. The screenshots render the real pages against device
responses captured read-only from the deployed board; see
[how these screenshots were made](#how-these-screenshots-were-made).

## How the interface is served

- The pages live on the card under `/sd/www/` and are served by the firmware's
  own HTTP server on **port 80, plain HTTP, with no TLS** and no certificate
  anywhere in the firmware (`kHttpPort` in
  [`firmware/src/aius_network_teensy.cpp`](../../firmware/src/aius_network_teensy.cpp)).
  The device is a bench instrument on a trusted LAN.
- The real URLs are `http://<device>/sd/www/index.html` (the DSP page),
  `.../status.html` and `.../upgrade.html` (the firmware page). `classic.html`
  was retired 2026-10-12 (Phase 4r) and no page links it. The site root `/` is a
  small built-in fallback page compiled into flash
  ([`firmware/src/http_home_page.cpp`](../../firmware/src/http_home_page.cpp))
  that links into the SD tree and reports when no card is mounted.
- Each page is a set of classic scripts, with no modules, no bundler, no build
  step, and none of the repo's `mr-*` UI kit: the device cannot reach
  a host application's module mounts. `sd-shared.js` loads first on every page and owns
  the status pills, the never-throw `fetch` wrapper, the serialised poller, the
  number formatters and token storage. Then `status.html` loads `status.js`,
  `upgrade.html` loads `upgrade.js`, and `index.html` loads `dsp-editor.js`
  followed by `mixer.js`.
- Every page shares `sd-chrome.css` (the bench skin: logo row with the three nav
  links, panels, pills, spec grids), with the fonts Archivo and IBM Plex Mono
  vendored under `firmware/sd/www/fonts/`. `index.html` adds `dsp-editor.css`
  and `mixer.css`; `upgrade.html` adds `upgrade.css`. The pages carry no visible
  page heading: each has a visually hidden `<h1>`, and panels are numbered
  kickers (`01 / Configuration`).
- The firmware serves `/sd/www` from a PSRAM gzip web cache with `ETag` and
  `If-None-Match` answering `304` (`firmware/src/aius_optical_app.cpp:1016`), falling
  back to the card on a miss.
- The assets themselves are replaced over `PUT /sd/www/{path}` and removed with
  `DELETE /sd/www/{path}`, so the interface is updated without reflashing.
- **Reads are public; control routes are auth-gated.** Every mutating request
  carries `X-Aius-SD-Auth`, the 16 hexadecimal characters printed by the
  physical USB command `AIUS AUTH`. That covers `PUT /api/dsp/config`, the OTA
  upload, the OTA verify/arm/abort/activate/boot-other verbs, the SD asset
  writes, `POST /api/start|stop|clear|reboot|measure/start`, `POST /api/sd/mode`,
  and the `/api/recorder*` routes, whose GETs are token-gated
  too (`firmware/src/aius_optical_app.cpp:3074` lists the six control routes).
  `PUT /api/config/public` is the one ungated mutating route, by design: it
  accepts only the three public feature flags (`firmware/src/aius_optical_app.cpp:1589`,
  `docs/plans/aius-sd-config-web.md:544`, `firmware/src/http_home_page.cpp:32`). Polling routes
  (`/api/status`, `/api/status/brief`, `/api/meters`, `/api/ota`) carry nothing,
  and the pages never attach the header to a `GET`. The token is a
  physical-access bootstrap credential, not an authorization boundary; for
  firmware replacement, the image signature is what makes the surface
  defensible. The DSP and firmware pages keep it in `localStorage` under the key
  `aius-sd-auth`; the status page never asks for one.

## DSP page

![The Aius DSP page with a 5.1 AC-3 carrier locked: the sticky action bar
over a signal-flow diagram with its audio path and live meter cards, the preset
card beside it, then the full-width matrix faders with output levels, the EQ
plot with its channel tabs, and the limiter gain-reduction
scope.](images/dsp-editor.png)

`index.html` + `dsp-editor.js` + `mixer.js` + `dsp-editor.css` + `mixer.css`.
`dsp-editor.js` owns the model, validation, polling and writes; `mixer.js` is a
skin that regroups the form it builds into faders, the signal diagram and EQ
channel tabs. It makes no request of its own and writes only through the
editor's form fields (`firmware/sd/www/mixer.js:11-17`). The page edits the
single effective DSP configuration (per-channel input trim and EQ, the downmix
matrix, output EQ, the true-peak limiter and the per-stage bypass flags) while
showing the live signal path around it.

Top to bottom:

- **Nav and action bar.** The logo row carries the nav (`dsp` current). Below it
  the sticky action bar holds an apply-state pill, a *no unapplied changes* pill
  that shows while the form diverges from the device, the **lock chip** (opens
  the token dialog; the token field lives in that dialog, not in the bar),
  **Revert** (re-reads the device, after confirming that edits on the page will
  be discarded) and **Apply**. Under it a status line, the standing note *Edits
  are staged on this page until Apply*, the field-error summary and the
  stale-generation notice.
- **00 / Signal** (left, one third of the first band on a wide screen; full
  width below 1080 px). The head carries a `json` link to `GET /api/dsp/config`,
  an LED and the **input** pill (`input: none`, or `input: optical carrier`
  plus the encoding, such as `bitstream (AC-3)` or `PCM`, and the rate in Hz).
  The SVG shows the six decoded inputs, the matrix core and the L and R
  outputs. Wire widths follow the matrix weights, box fills follow the input
  level needles, and the wires move only while the transmit gate is open. Under
  it the **output** pill (`output: gate open` or `gate dark`), then the **Audio
  path** grid (Audio state, Stream format, Gate carrier lock, Model rate,
  Blocks, Overruns), then the **Live** stage: a state pill, Queue, Latency and
  DSP load readouts, one meter card per channel group and a one-line summary.
- **01 / Presets** (right of Signal). A picker with *Load*, a *Save as* name box
  (counter `n / 27 characters`) with *Save*, and a *More* menu: import a file
  as a preset, download the selected preset, download the current form, delete
  the selected preset, refresh the list. Beneath it a status line and the
  preset origin.
- **02 / Matrix** (full width). A row of one chip per saved preset (a click loads
  it into the form), then the *clip-free headroom* and *layout-aware* chips.
  Then the centre, front, surround and LFE gain faders, the equation readout, the
  centre-versus-rest dial, the **Output levels** bars and the worst-case
  verdict. A strip of steppers carries crossover, headroom and surround delay.
  The *Exact values* disclosure holds the underlying fields and opens on its
  own when a value goes out of range.
- **03 / EQ** (full width). A legend selects curves on the stacked magnitude
  plot, with *Add section*, *Remove section* and a Q box beneath it. Beside the
  plot is a tab per channel (stereo pairs bond with a `link` chip) over that
  channel's input trim and EQ sections, plus an *Output EQ* tab.
- **04 / Limiter** (full width). A state pill, the gain-reduction bar and value,
  the gain-reduction history scope and the limiter fields.

Each stage head (input EQ, matrix, output EQ, limiter, live meters) carries that
stage's *enabled* box and an *edited* marker; a stage whose box is cleared dims
and shows a **bypassed** pill, without moving the layout. The view toggle of the
older page is gone from the visible page (`index.html:77-80` keeps its buttons
hidden because `dsp-editor.js` still writes the state): there is one layout, a
twelve-column board that collapses to one column on a phone, and nothing in it
is resizable.

Reads:

- `GET /api/dsp/config` on load and on *Revert*, and while an apply is pending.
- `GET /api/meters` at 4 Hz (250 ms) for the level bars, limiter gain reduction
  and DSP cost; it backs off to one try every 5 s while the route is absent
  (`firmware/sd/www/dsp-editor.js:46-48`).
- `GET /api/status/brief` at 1 Hz for the two flow pills (input carrier,
  encoding and rate; transmit gate) and the `sd_flush` verdict of the last save.
- `GET /api/status` every 5 s for the Audio path grid only. The brief body omits
  `format`, `blocks`, `overruns` and `tx_gate.carrier_locked`, and the full body
  is about 8.6 kB, so it runs at a fifth of the brief's cadence
  (`firmware/sd/www/dsp-editor.js:2330-2334`).
- `GET /sd/www/presets/` (the firmware's own HTML directory index, parsed for
  `*.json` rows) and `GET /sd/www/presets/<name>.json` to load one.

Writes:

- `PUT /api/dsp/config` with the whole model plus `base_generation`; one Apply
  sends the whole form, since no stage has its own send button.
- `PUT` and `DELETE /sd/www/presets/<name>.json` for preset save and delete.

Non-obvious:

- **The action bar is the only writer.** A fader, a curve drag or a keystroke
  stages a change on the page; nothing reaches the device until Apply. Loading a
  preset fills the form the same way and Apply still sends it.
- A `200` on the config PUT has two branches (`app_store_dsp_config`,
  `firmware/src/optical_main.cpp:7207-7232`). With the PSRAM write overlay present
  (`sd_overlay_present()`), the model is accepted into the overlay and *staged*, not durable on
  the card and not live: the apply and the `config_generation` increment happen at
  the next audio block boundary. Without it, the PUT is held until its SD write
  commits: the `200` is released at that commit and only the staged generation
  (`g_staged_generation`) advances there (`:7225-7231`, `sd_settle_http_write()`); a
  write that faults discards the staging. In both branches `config_generation`
  still advances at the next block boundary, in the apply path (`:2387`, `:2890`), so
  nothing is live at the `200`. The page then polls `GET /api/dsp/config` every
  500 ms, up to 40 times (about 20 s), until the generation changes and reports
  *Applied at generation N*; if it never moves it raises an error asking for a
  re-read.
- A `200` is also not "on the card". Settings writes answer at accept; the
  status line appends *Saved to the SD card.* only once `sd_flush` in the 1 Hz
  brief poll reports the queue drained, and otherwise says *Pending* (no card,
  or the card write is failing).
- A `503 audio_busy` or `503 sd_busy` on the PUT means nothing was written, so
  the page repeats it after `Retry-After` (clamped to 0.25 to 5 s) before
  reporting a rejection.
- `base_generation` is optimistic concurrency. If the device has moved on, the
  PUT answers `409`, nothing is written, and the page raises a notice offering
  *Reload device config* rather than silently discarding either side's edits.
- Each stage's `enabled` box bypasses that stage for latency and diagnostic
  comparisons; the values behind it are still stored and still validated. IEC
  61937 framing and decoding cannot be bypassed.
- The EQ plot is a preview. The device designs the real biquads when a config is
  applied, so the curve is the page's own model of the cascade.
- **The spectrogram and the spectrum underlay were deleted on 2026-08-16, along
  with the client-side FFT.** Their feed, `analysis_window`, was dropped from
  `/api/meters` by output-jitter Phase A (about 9.3 kB of JSON inside one HTTP
  epoch was the loop's dominant recurring cost). Nothing on the page consumes a
  sample window now. `git show b4a3914ce^` carries the FFT and band-mapping
  helpers if a future device publishes one again.
- `sequence == 0` in a meter snapshot is the documented "never published"
  sentinel and renders as *no data*, not as a silent signal.
- Preset names are capped at 27 characters (the SD path grammar's 32-byte
  segment minus `.json`) and any name ending in `config` is refused, because the
  firmware's write guard is a bare suffix match on `config.json`: such a file
  could be read but never rewritten or deleted. At most 16 presets can be saved
  (`firmware/sd/www/dsp-editor.js:1925`); the 17th save is refused until one is
  deleted.
- A preset carries only the five editable groups (features, input channels,
  output EQ, matrix, limiter); the sample rate never travels in one.
- The Audio path *Model rate* is the DSP model's design rate, not the carrier's
  negotiated rate; the carrier rate is in the input pill.

## Device status

![The Aius device status page: a row of three cards for configuration,
network and storage, then a full-width board card with the boot ledger, running
image and firmware slot, and the forensic records, every pill reading
live.](images/status.png)

`status.html` + `status.js`. One whole `GET /api/status` per second, rendered
into four numbered cards: **01 / Configuration**, **02 / Network**, **03 /
Storage** and **04 / Board**, each with a pill (`live`, or `unreachable`). The
page is read-only and public: nothing on it writes to the device, so it carries
no token and has no password field. It needs no audio carrier to look
populated, since the audio path now lives on the DSP page.

Reads `GET /api/status` at 1 Hz, and `GET /api/dsp/config` at load and every
10 s for one value (below). Writes nothing.

Non-obvious:

- **The page polls `/api/status` for everything but one field.** It has no
  meters card and no audio-path card; the audio path (state, format, carrier
  lock, blocks, overruns) moved to the DSP page's Signal panel, and the input
  source and transmit gate are that panel's two pills. Leaving this page open
  while listening costs one status request per second plus one small config read
  per 10 s.
- **The configuration card is two unrelated files, and saying so is the point.**
  The first three rows (DSP model, Applied by, DSP generation) are
  `/aius/dsp.json`, the model the DSP page writes. *Applied by* is
  `model_origin`, the only field that separates an editor apply from a boot load
  of the same file. The *Boot config file* row is `/aius/config.json`, an
  optional file of three feature flags with **no firmware consumer today**;
  nothing writes it, so *absent* is every device's normal state and is rendered
  without a warning tone. Only a file the parser refused is marked.
- **DSP generation is the one value not on `/api/status`.** The firmware keeps it
  off that body, so the page reads `config_generation` from `GET /api/dsp/config`
  every 10 s (`firmware/sd/www/status.js:438-461`). A failed read shows `--`; a
  `503` keeps the last value.
- **The board card's forensic rows are absent in two different ways and the
  page never merges them.** A key the running image does not serve at all reads
  *not reported by this image*; a key the image serves as `null` reads what its
  emptiness means (*none recorded*, *no stamp retained*, *single-image build*).
  Reporting the second when the first is true would invent a clean bill of
  health nobody asked the board for. The rows are Slot boot record, Chooser
  trial ledger, Previous boot, Last CPU fault, OTA dark window and Stall
  witness; Firmware slot and Build revision sit above them.
- The wire is integers where the vocabulary matters: `state`, `carrier`,
  `link`, `network_state`, `last_checkpoint`, `reset_source`, `recovery_safe`,
  the A/B slot fields and the dark-window state and phase. The page prints the
  name and keeps the number beside it (`online (2)`, `audio (9)`).
- **`last_checkpoint` 9 is the audio checkpoint, and only 10 means the epoch
  completed.** The page's list has all eleven names
  (`firmware/sd/www/status.js:62-74`), mirroring `AIUS_BOOT_CHECKPOINT_*`; the
  canonical table is [`usb-control-protocol.md`](usb-control-protocol.md).
- On a failed poll every pill flips to `unreachable` after three consecutive
  failures, every grid dims, and the last good values stay on screen rather
  than being blanked. A `503` is the device shedding load and never reaches that
  state.
- `/api/status` is an epoch-latched snapshot, so the board terms describe the
  epoch the snapshot belongs to; the poll line reads *Snapshot at epoch N*.

## Firmware page

![The Aius firmware page in its idle state: the device OTA grid reads idle with
the feature latch armed and a rotated non-test signing key, the firmware slots
card names the running slot, the other slot and both revisions beside a *Boot
the other slot* button, and the signed-container card waits with its header
fields unread until a container is chosen.](images/upgrade.png)

`upgrade.html` + `upgrade.js` + `upgrade.css`. Four numbered cards: **01 /
Device OTA**, **02 / Firmware slots**, **03 / Signed container** and **04 /
Progress log**. The nav label is `firmware`; the file is still `upgrade.html`.
The page stages a signed firmware container and walks it through the device's
own upgrade machine: parsed, uploaded, staged, digest verified, signature
verified, armed. **Nothing on this page programs flash** in the upload flow;
activation is a separate, device-gated step. The slots card is the one other
control, and it only asks the chooser for a trial boot.

Reads:

- `GET /api/ota` at 1 Hz; a `404 ota_absent` is a real answer (OTA compiled out,
  or the feature latch never armed) and disables every control on the page
  rather than leaving a half-working button.
- `GET /api/status` once at load, for the slots card (`ab`, `build_revision`)
  and a board identity to check a container's `target_id` against. The slots
  card is therefore read once per page load, not polled.

Writes:

- `PUT /api/ota/image`, one 4096-byte chunk per request with a
  **container-absolute** `Content-Range`: the 256-byte header is the chunk at
  offset 0 and the payload follows at 256, so one number resumes an interrupted
  transfer whichever half it stopped in.
- `POST /api/ota/verify`, then `POST /api/ota/arm`.
- `POST /api/ota/abort` to drop a staged image.
- `POST /api/ota/activate`, rendered disabled until `/api/ota` reports
  `activation_available`.
- `POST /api/ota/boot-other`, from the *Boot the other slot* button.

Non-obvious:

- **The slots card shows both A/B slots.** It lists the running slot and its
  revision, the other slot and its revision (the boot record's per-slot 12-hex
  revision; the running one is taken from `build_revision` before the `+`).
  The pill reads `slot A` or `slot B`, or `no A/B` on a single-image build.
- **Boot the other slot is a confirmed reboot, offered only when the status
  proves it will be accepted.** It is disabled, with the reason in the status
  line, when the boot record is not yet valid, a trial boot is still open, the
  boot record does not name the running slot, or the chooser predates slot
  switching (`firmware/sd/www/upgrade.js:1100-1117`). A confirm dialog names the
  target slot and revision and warns that an image without this feature has no
  way back except an OTA. The board trial-boots the other slot and falls back
  on its own if it never comes up healthy. The device is the authority: a refusal
  (`state`, `ivt`, `sd_flush_pending`, `unsupported`) is shown by name.
- The 256-byte container header is parsed **in the browser before a byte is
  sent** (magic, format version, header length, target id, signature scheme,
  build-UTC plausibility window and the header CRC-32), so an image for another
  board is refused here instead of after a multi-megabyte upload. The parse
  follows `aius_ota_header_parse()`'s rule order so a local refusal names the
  same reason the device would have named.
- One verb produces two log steps: `verify` finalises the payload digest first
  and only then hands the authentication digest to the signature verifier, so a
  digest refusal means the signature check never ran.
- Resume only applies to the *same* container: a `next_offset` belonging to a
  different image would resume a digest nobody can satisfy, so a mismatch
  restarts at 0 and *Abort* is the escape hatch.
- A red **TEST KEY** banner appears when the device reports it trusts the
  published development signing key: anyone holding the repository could sign
  firmware it would accept. The live board reports a rotated key
  (`test_key: false`), so the banner is absent here.
- The page reads the firmware's real `OtaStatus` shape, so the idle grid shows
  the device's own `armed`, `staged_bytes`, `total_bytes` and key fingerprint.
  *Device identity* still reads `unknown` on an idle board: `/api/status` does
  not emit `target_id` (defect 40, open), and the fallback, `image.target_id` in
  `/api/ota`, is the staged session's header, so it fills only once a container
  is offered (`firmware/sd/www/upgrade.js:692-695`).
- The SD auth token field sits inline in the Signed container card (the DSP page
  keeps it in a dialog) and is shared with it through `localStorage`.

The staged, digest-verified, signature-verified and armed states are not shown
here: reaching them means signing and staging a real container onto the device,
which the capture pass deliberately skips. The page drives them from one log
that appends a step when it starts and settles it from the route's answer plus a
read of `/api/ota`, so it never shows a step the run did not reach, and the final
Activate entry stays disabled until the device gates it.

## How these screenshots were made

Taken on 2026-10-17 in headless Chromium (1280x900, `deviceScaleFactor: 2`,
full-page) through `the repo tooling tool playwright-inspect`. The page files are
the unmodified `firmware/sd/www/` sources; a preview-only `fetch` shim answers
each device route from JSON fixtures captured read-only from the deployed
board, so nothing was sent to the device and every value shown is one the
firmware served:

- **Configuration, presets, OTA state and board status** (`/api/dsp/config`,
  the preset files, `/api/ota`, `/api/status`): captured 2026-10-17 from the
  running image `04f8b1b33b77` in slot B.
- **The audio path and the meters** (the carrier, format, clock, transmit gate
  and decoder fields of `/api/status`, and a 5 s loop of `/api/meters`
  snapshots): captured 2026-10-07 while the board played a 5.1 AC-3 carrier.
  The board had no source playing on 2026-10-17, and the DSP page only reads
  as populated with one.
- **The network card's address** is the documentation address `192.0.2.66`,
  not the board's own.

`/api/status/brief` is derived from the same merged status. The status and
firmware pages do not depend on a carrier.

`images/dsp-editor-hero.png` is not one of the page captures: it is the
README's hero, the same DSP page clipped between two measured element edges,
flush with the top of `#dashboard-input` (the *00 / Signal* panel, level with
*01 / Presets*) down to the bottom of `#dashboard-matrix` (the *02 / Matrix*
card). It carries the signal-flow diagram with its audio path and live meters,
the preset card and the matrix faders with their output levels; the action bar
sits above the cut, and the EQ and limiter below it.

`images/aius-workbench.jpg` is a photograph of the bench hardware, not a render.
The firmware page capture is the idle state only: its staged and armed states
are described above but not screenshotted, because reaching them means signing
and staging a container onto the live device. The replay manager page and the
Dashboard/Editor view toggle capture are retired (replay: aius-sample-rate-following
Phase 4i, 2026-10-12; the toggle: the mixer skin became the only layout).
