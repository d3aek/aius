---
title: Aius user interface
type: reference
scope: component
component: aius
status: current
tags: [aius, ui, http, sd]
updated: 2026-08-05
toc: true
---

# Aius user interface

Aius serves its whole operator interface from the Teensy's SD card: four HTML
pages that read and write the device's own HTTP routes. This document shows each
page with a populated response behind it, what it reads and writes, and what a
reader could not guess from the markup. The screenshots are live captures from
the deployed board at `http://192.0.2.66`, driven by a real 5.1 source; see
[how these screenshots were made](#how-these-screenshots-were-made).

## How the interface is served

- The pages live on the card under `/sd/www/` and are served by the firmware's
  own HTTP server on **port 80, plain HTTP — there is no TLS** and no
  certificate anywhere in the firmware (`kHttpPort` in
  [`firmware/src/aius_network_teensy.cpp`](../../firmware/src/aius_network_teensy.cpp)).
  The device is a bench instrument on a trusted LAN.
- The real URLs are `http://<device>/sd/www/index.html`,
  `.../classic.html`, `.../status.html` and `.../upgrade.html`. The site root
  `/` is a small built-in fallback page compiled into flash
  ([`firmware/src/http_home_page.cpp`](../../firmware/src/http_home_page.cpp))
  that links into the SD tree and reports when no card is mounted.
- Each page is a classic script — `<page>.html`, then `sd-shared.js`, then
  `<page>.js`, no modules, no bundler, no build step, and none of the repo's
  `mr-*` UI kit: the device cannot reach a host application's module mounts.
  `sd-shared.js` owns the status pills, the never-throw `fetch` wrapper, the
  number formatters, the waveform renderer and token storage.
- The assets themselves are replaced over `PUT /sd/www/{path}` and removed with
  `DELETE /sd/www/{path}`, so the interface is updated without reflashing.
- **Reads are public; control routes are auth-gated.** Every mutating request
  carries `X-Aius-SD-Auth`, the 16 hexadecimal characters printed by the
  physical USB command `AIUS AUTH`. That covers `PUT /api/dsp/config`, the
  the OTA upload, the OTA
  verify/arm/abort/activate verbs, the SD asset writes, and
  `POST /api/start|stop|clear|reboot|measure/start`. Polling routes
  (`/api/status`, `/api/meters`, `/api/ota`) carry nothing, and
  the pages never attach the header to a `GET`. The token is a physical-access
  bootstrap credential, not an authorization boundary — for firmware
  replacement, the image signature is what makes the surface defensible. Pages
  that need it keep it in `localStorage` under the key `aius-sd-auth`.

## DSP editor

![The Aius DSP editor in its default dashboard view: an effective configuration
loaded from SD at generation 1 while a live 5.1 AC-3 carrier plays, a preset
list, then the signal-flow stages as one column of full-width bands — input
levels, the stacked EQ plot, the downmix routing diagram, limiter gain
reduction, output levels and the live processing meters.](images/dsp-editor.png)

`classic.html` + `dsp-editor.js` + `dsp-editor.css`. The classic editor (the mixer
dashboard is now `index.html`, the DSP editor tab): it edits the
single effective DSP configuration — per-channel input trim and EQ, the downmix
matrix, output EQ, the true-peak limiter, and the per-stage bypass flags — while
showing the live signal path around it.

Reads:

- `GET /api/dsp/config` on load and on *Reload from device*.
- `GET /api/meters` at 10 Hz for the level bars, limiter gain reduction and DSP
  cost.
- `GET /api/status` at 1 Hz for the two ends of the signal-flow strip: what the
  device is hearing, and what the transmit gate did with it.
- `GET /sd/www/presets/` — the firmware's own HTML directory index, parsed for
  `*.json` rows — and `GET /sd/www/presets/<name>.json` to load one.

Writes:

- `PUT /api/dsp/config` with the whole model plus `base_generation`.
- `PUT` and `DELETE /sd/www/presets/<name>.json` for preset save and delete.

Non-obvious:

- A `200` on the config PUT means the model is durable in `/aius/dsp.json` and
  *staged*, not live: the apply and the `config_generation` increment happen at
  the next audio block boundary.
- `base_generation` is optimistic concurrency. If the device has moved on, the
  PUT answers `409`, nothing is written, and the page raises a banner offering a
  reload rather than silently discarding either side's edits.
- Each stage's `enabled` box bypasses that stage for latency and diagnostic
  comparisons; the values behind it are still stored and still validated. IEC
  61937 framing and decoding cannot be bypassed.
- The EQ plot is a preview. The device designs the real biquads when a config is
  applied, so the curve is the page's own model of the cascade.
- **The spectrogram and the faint spectrum underlay behind the EQ curves were
  deleted on 2026-08-16, along with the client-side FFT that drove them.** Their
  feed was `analysis_window` in the meter snapshot, which output-jitter Phase A
  dropped from `/api/meters`: 1024 raw samples were ~9.3 kB of JSON formatted
  and sent inside a single HTTP epoch at the editor's 10 Hz poll, the loop's
  dominant recurring single-epoch cost. The page went on shipping the canvas for
  the rest of that day, feature-detecting the missing key and reporting
  *"history is paused"* — which describes a transient gap rather than a feed no
  device has. Nothing on the page consumes a sample window now. `git show
  b4a3914ce^` carries the FFT and band-mapping helpers if a future device ever
  publishes one again.
- `sequence == 0` in a meter snapshot is the documented "never published"
  sentinel and renders as *no data*, not as a silent signal.
- Preset names are capped at 27 characters (the SD path grammar's 32-byte
  segment minus `.json`) and any name ending in `config` is refused, because the
  firmware's write guard is a bare suffix match on `config.json` — such a file
  could be read but never rewritten or deleted.

![The same page with the Signal flow toolbar switched from Dashboard to Editor:
the same stages unframed and rule-separated rather than boxed into bands, from
input channel trim and EQ through the matrix values, limiter controls, output
levels and the live meter cards.](images/dsp-editor-form.png)

The *Signal flow* toolbar is the whole view control, and its two buttons are the
only thing on the strip that changes its shape. **Dashboard** (the default,
above) gives each stage its own full-width band — input levels, EQ cascades,
downmix matrix, limiter, output levels, live processing, in signal order, each
band as tall as its own content. **Editor** drops the band framing and runs the
same blocks as one continuous rule-separated column. Both views render the same
nodes, the same forms and the same per-stage *Apply* buttons, and drive the same
model. Nothing in either view is resizable — there are no cell handles and no
per-panel geometry to restore — and a phone renders the same single column a
desktop does, with the four canvases trimmed below 651 px of viewport width.

## Device status

![The Aius device status page with a 5.1 AC-3 optical carrier locked: the audio
path card shows the derived input source and the transmit gate, over
configuration, network, storage and board cards, every pill reading
live.](images/status.png)

`status.html` + `status.js`. One whole `GET /api/status` per second, rendered
into five cards. It is read-only and public — nothing on the page writes to the
device, so it carries no token and has no password field.

Reads `GET /api/status` at 1 Hz. Writes nothing.

Non-obvious:

- **The input source is the page's own derivation, not a device field.** The
  page derives it as carrier → *optical carrier*, else *none*. The Live
  processing card (meter envelope and output levels, fed by `GET /api/meters`)
  lives on this page.
- **The configuration card is two unrelated files, and saying so is the point.**
  The first two rows are `/aius/dsp.json` — the model the DSP editor writes,
  plus `model_origin`, which is the only field that separates an editor apply
  from a boot load of the same file. The boot config row is `/aius/config.json`,
  an optional file of three feature flags with **no firmware consumer today**;
  nothing writes it, so *absent* is every device's normal state and is rendered
  without a warning tone. Only a file the parser refused is marked. Until
  2026-08-16 the card showed `Configuration valid: no / not loaded (code 10) /
  1 error` in warning colour beside the DSP model source on every board, which
  reads as a verdict on the editor's *Apply* — a different file the editor
  cannot create.
- **The board card's forensic rows are absent in two different ways and the
  page never merges them.** A key the running image does not serve at all reads
  *not reported by this image*; a key the image serves as `null` reads what its
  emptiness means (*none recorded*, *no stamp retained*, *single-image build*).
  Reporting the second when the first is true would invent a clean bill of
  health nobody asked the board for. `fault_record` and `prev_boot` landed after
  the deployed slot-B image was built, so they read the first form until the
  next flash.
- The wire is integers where the vocabulary matters — `state`, `carrier`,
  `format`, `link`, `network_state`, `last_checkpoint`, `reset_source`,
  `recovery_safe`, the A/B slot fields, the dark-window state and phase, and
  every `tx_gate` member. The page prints the name and keeps the number beside
  it (`PCM (3)`, `online (2)`, `audio (9)`).
- **`last_checkpoint` 9 is the audio checkpoint, and only 10 means the epoch
  completed.** This page carried the pre-audio ten-name vocabulary until
  2026-08-16 and rendered every value from 9 up one name early, so a board that
  starved in audio reported `epoch-complete (9)` — the one word meaning it had
  not. `swagger.json` carried the same stale list and bounded the field at 9.
  The canonical table is
  [`usb-control-protocol.md`](usb-control-protocol.md), which was always right.
- On a failed poll every pill flips to `unreachable`, every grid dims, and the
  last good values stay on screen rather than being blanked — a frozen grid
  beside a confident-looking pill was the failure mode this replaced.
- `tx_gate` is latched once per epoch, so the gate terms describe the epoch the
  snapshot belongs to.

## Firmware upgrade

![The Aius firmware upgrade page in its idle state: the device OTA grid reads
idle with the feature latch armed and the signing key a rotated non-test key,
and the signed-container card waits with its header fields unread until a
container is chosen.](images/upgrade.png)

`upgrade.html` + `upgrade.js` + `upgrade.css`. Stages a signed firmware
container and walks it through the device's own upgrade machine: parsed,
uploaded, staged, digest verified, signature verified, armed. **Nothing on this
page programs flash** — activation is a separate, device-gated step.

Reads:

- `GET /api/ota` at 1 Hz; a `404 ota_absent` is a real answer (OTA compiled out,
  or the feature latch never armed) and disables every control on the page
  rather than leaving a half-working button.
- `GET /api/status` once, for the board identity a container's `target_id` is
  checked against.

Writes:

- `PUT /api/ota/image`, one 4096-byte chunk per request with a
  **container-absolute** `Content-Range`: the 256-byte header is the chunk at
  offset 0 and the payload follows at 256, so one number resumes an interrupted
  transfer whichever half it stopped in.
- `POST /api/ota/verify`, then `POST /api/ota/arm`.
- `POST /api/ota/abort` to drop a staged image.
- `POST /api/ota/activate`, rendered disabled until `/api/ota` reports
  `activation_available`.

Non-obvious:

- The 256-byte container header is parsed **in the browser before a byte is
  sent** — magic, format version, header length, target id, signature scheme,
  build-UTC plausibility window and the header CRC-32 — so an image for another
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
  published development signing key — anyone holding the repository could sign
  firmware it would accept. The live board reports a rotated key
  (`test_key: false`), so the banner is absent here.
- Since the defect-39 fix the page reads the firmware's real `OtaStatus` shape,
  so the idle grid above shows the device's own `armed`, `staged_bytes`,
  `total_bytes` and key fingerprint rather than the empty defaults the earlier
  field-name drift produced. Device identity still reads `unknown`: `/api/status`
  does not emit `target_id` (defect 40, open).

The staged, digest-verified, signature-verified and armed states are not shown
here: reaching them means signing and staging a real container onto the device,
which this capture pass deliberately skips. The page drives them from one log
that appends a step when it starts and settles it from the route's answer plus a
read of `/api/ota`, so it never shows a step the run did not reach, and the final
Activate entry stays disabled until the device gates it.

## How these screenshots were made

These are live captures from the deployed board at `http://192.0.2.66`, taken on
2026-09-04 in headless Chromium (1280x900, `deviceScaleFactor: 2`, full-page)
through `the repo tooling tool playwright-inspect` — navigate, wait for the page's
own data to populate, screenshot. Nothing is stubbed; every value on every page
is what the running firmware served at capture time.

The board decodes and downmixes a 5.1 surround source, so the meter-driven pages
need one playing. The **DSP editor** and the carrier-locked **status** capture
were driven by a real 5.1 AC-3 carrier: the reproducible sample pack from
[`generate.sh`](../../firmware/tools/iec_fixtures/generate.sh) (the `_ac3.iec`
IEC 61937 form) played out an ESI U24XL's optical S/PDIF into the board's TOSLINK
input with `aplay` at unity mixer, so the board locks a genuine AC-3 carrier and
every input channel reads live.

The replay manager page and its no-carrier status capture were retired with
replay (aius-sample-rate-following Phase 4i, 2026-10-12); the live captures here
are all carrier-driven.

`images/dsp-editor-hero.png` is not one of the six page captures: it is the
README's hero, the same dashboard clipped between two measured element edges —
flush with the top of the `#curve` canvas down to the bottom of `#gr-history`,
the limiter gain-reduction canvas. It opens and closes on a live graphic and
carries everything between them; the *Signal flow* header and the input level
meters sit above the cut and are absent. `#gr-history` is the lower anchor
because `#spectrogram`, the old one, was deleted with the analyser.

`images/aius-workbench.jpg` is a photograph of the bench hardware, not a render.
The firmware upgrade capture is the idle state only: its staged and armed states
are described above but not screenshotted, because reaching them means signing
and staging a container onto the live device.
