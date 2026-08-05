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
reader could not guess from the markup. The screenshots were rendered locally
rather than captured from a live device; see
[how these screenshots were made](#how-these-screenshots-were-made).

## How the interface is served

- The pages live on the card under `/sd/www/` and are served by the firmware's
  own HTTP server on **port 80, plain HTTP — there is no TLS** and no
  certificate anywhere in the firmware (`kHttpPort` in
  [`firmware/src/aius_network_teensy.cpp`](../../firmware/src/aius_network_teensy.cpp)).
  The device is a bench instrument on a trusted LAN.
- The real URLs are `http://<device>/sd/www/index.html`,
  `.../replays.html`, `.../status.html` and `.../upgrade.html`. The site root
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
  replay start/stop/delete verbs, the replay and OTA uploads, the OTA
  verify/arm/abort/activate verbs, the SD asset writes, and
  `POST /api/start|stop|clear|reboot|measure/start`. Polling routes
  (`/api/status`, `/api/meters`, `/api/replays*`, `/api/ota`) carry nothing, and
  the pages never attach the header to a `GET`. The token is a physical-access
  bootstrap credential, not an authorization boundary — for firmware
  replacement, the image signature is what makes the surface defensible. Pages
  that need it keep it in `localStorage` under the key `aius-sd-auth`.

## DSP editor

![The Aius DSP editor in its default dashboard view: an effective configuration
loaded from SD at generation 4, a preset list, then the signal-flow stages as
one column of full-width bands — input levels, the stacked EQ plot with a
spectrum underlay, the downmix routing diagram, limiter gain reduction, output
levels and a live spectrogram.](images/dsp-editor.png)

`index.html` + `dsp-editor.js` + `dsp-editor.css`. The main page: it edits the
single effective DSP configuration — per-channel input trim and EQ, the downmix
matrix, output EQ, the true-peak limiter, and the per-stage bypass flags — while
showing the live signal path around it.

Reads:

- `GET /api/dsp/config` on load and on *Reload from device*.
- `GET /api/meters` at 10 Hz for the level bars, limiter gain reduction, DSP
  cost and the spectrogram.
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
- The spectrogram and the faint underlay behind the EQ curves are driven by
  `analysis_window` in the meter snapshot — 1024 raw 24-bit samples with
  `analysis_full_scale` alongside. It is strobed, not gapless (one window per
  100 ms poll), and when a snapshot carries no window the underlay is not drawn.
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

## Replay manager

![The Aius replay manager playing a capture: the playback grid shows position,
queue fill and the DTS input classifier, the library lists four captures with
one incomplete, and the live processing card draws the meter
envelope.](images/replays.png)

`replays.html` + `replays.js` + `replays.css`. Uploads IEC 61937 captures to the
card and feeds them through the same bitstream decoder, DSP, limiter and meter
path as the optical input — so a capture can stand in for a source that is not
on the bench.

Reads:

- `GET /api/replays` every 5 seconds for the library listing (the slowest tick:
  it walks the card).
- `GET /api/replays/status` at 1 Hz for the playback grid.
- `GET /api/meters` at 250 ms for the waveform and output levels.

Writes:

- `POST /api/replays/start` and `POST /api/replays/stop`.
- `DELETE /api/replays` to remove a capture, behind a confirm.
- `PUT /sd/replays/<id>` for the upload, one 4096-byte `Content-Range` chunk per
  request; every chunk but the last answers `next_offset`.

Non-obvious:

- An incomplete `.part` capture serves its size as **both** `bytes` and
  `upload_next_offset`, so the device cannot state the total the operator was
  aiming at. The row therefore says how much is on the card rather than claiming
  a fraction, unless this page session started that upload itself.
- *Play* is disabled on an incomplete capture — its words stop mid-stream.
  Re-uploading the same ID resumes from where the card left off.
- The selected replay is page state, not device state, so the 5-second library
  refresh cannot wipe a choice mid-gesture. The device's active replay only ever
  seeds an empty selection.
- A card holding more replays than one listing can carry flips the library pill
  to `truncated` and says the list is incomplete.
- Underruns are the one counter here that means the audio broke, and are
  highlighted once non-zero.

## Device status

![The Aius device status page with an optical carrier locked: the audio path
card shows the derived input source and the transmit gate, over configuration,
network, storage and board cards, every pill reading live.](images/status.png)

`status.html` + `status.js`. One whole `GET /api/status` per second, rendered
into five cards. It is read-only and public — nothing on the page writes to the
device, so it carries no token and has no password field.

Reads `GET /api/status` at 1 Hz. Writes nothing.

Non-obvious:

- **The input source is the page's own derivation, not a device field.** The
  page derives it as carrier → *optical carrier*, else the transmit gate's
  replay term → *replay*, else *none*.
- The wire is integers where the vocabulary matters — `state`, `carrier`,
  `format`, `link`, `network_state`, `last_checkpoint`, `reset_source`,
  `recovery_safe` and every `tx_gate` member. The page prints the name and keeps
  the number beside it (`PCM (3)`, `online (2)`, `epoch-complete (9)`).
- On a failed poll every pill flips to `unreachable`, every grid dims, and the
  last good values stay on screen rather than being blanked — a frozen grid
  beside a confident-looking pill was the failure mode this replaced.
- `tx_gate` is latched once per epoch, so the gate terms describe the epoch the
  snapshot belongs to.

![The same page with no optical carrier and a replay feeding the transmit gate:
the input source reads replay, the carrier reads absent, the sample rate is
withheld and the gate's replay term reads running.](images/status-replay.png)

The second capture is the same page against a device with no carrier and a
replay running — the case that exercises the derivation above. The sample rate
reads `-- (no carrier)` rather than inventing a zero.

## Firmware upgrade

![The Aius firmware upgrade page with a container selected: the device OTA grid
reads idle with the feature latch armed, and the parsed container header shows
target, build revision, payload length, signature scheme, payload digest and
header CRC-32.](images/upgrade.png)

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
  firmware it would accept. The stub behind these captures reports a rotated key
  (`test_key: false`), so the banner is absent from them.
- The page's `/api/ota` field names and the firmware's current serializer have
  drifted. `upgrade.js` reads `latch_armed`, `staged_length`, `image_length`,
  `fail_reason`, `test_key`, `key_fingerprint` and a top-level `target_id`;
  `ota_status_document()` in
  [`firmware/src/aius_optical_app.cpp`](../../firmware/src/aius_optical_app.cpp)
  emits `armed`, `staged_bytes`, `total_bytes`, `fault`, a nested
  `key.{fingerprint,test_key}` and a nested `image.target_id`. `state`,
  `next_offset`, `verifier_fault` and `activation_available` agree. Against the
  firmware as committed, the affected cells would read their empty defaults
  (`disarmed`, `0.0 KiB`, `unreported`, `unknown`) while the run itself still
  works. The two captures in this section were rendered against a stub serving
  the names the page reads.

![The same page after a completed staging run: the device OTA state reads armed,
and the progress log carries seven green steps from container selection through
upload, staging, digest, signature and arming, ending at a disabled Activate
button.](images/upgrade-armed.png)

The second capture is the end of a full run. The log appends a step when it
starts and settles it from the route's answer plus a read of `/api/ota`, so it
never shows a step the run did not reach. The final entry stays disabled with
its reason stated, because activation is the device-gated step.

## How these screenshots were made

These are **not captures from a live device.** They were rendered on 2026-08-05
from the committed [`firmware/sd/www`](../../firmware/sd/www) assets, loaded in
headless Chromium (1280x900, `deviceScaleFactor: 2`) against a local stub HTTP
server that answers the `/api/*` and `/sd/*` routes each page polls. The stub's
responses are derived from the vitest fixtures and stub data in
[`test/sd-editor`](../../test/sd-editor) — `meters-snapshot.json`,
`dsp-config-sd.json`, the `/api/status` snapshot in `status.test.js`, and the
replay and OTA wire shapes those suites pin — cross-checked against
[`swagger.json`](../../../../apps/aius/swagger.json) and the firmware's own
serializers. The captures were taken only after each page's data had populated.

`images/dsp-editor-hero.png` is not one of the seven page captures: it is the
README's hero, the same dashboard captured the same way and clipped in the
browser between two measured element edges — flush with the top of the `#curve`
canvas, down to just past `#spectrogram`. It therefore opens and closes on the
two live graphics and carries everything between them. The *Signal flow* header
and the input level meters sit above the cut and are absent.

Four things in the images are authored rather than fixture-derived, because no
fixture states them:

- The replay IDs, sizes and playback counters, and the preset names. These are
  operator-chosen values on the card; `replays.test.js` builds its rows from
  helpers rather than a checked-in fixture, so these follow that shape.
- The meter `analysis_window` and `waveform` arrays, which are synthesised —
  `dsp-editor.test.js` synthesises its analysis window too, rather than checking
  in 1024 integers. The window length (1024), the full scale (8388607) and the
  128-point per-mille envelope are the firmware's own constants.
- `matrix.surround_delay_ms: 0`, added to `dsp-config-sd.json`. That fixture
  predates the field, and the editor collects it, so a config omitting it loads
  as *unsaved changes* on arrival.
- The firmware container on the upgrade page. Its 256-byte header is written to
  `aius_ota.h`'s published offsets over a payload that is not a real image, and
  its signature is a placeholder rather than a real Ed25519 signature.

Everything else — the `/api/status` snapshot, the DSP model, the meter levels,
the limiter reduction, the card capacity and the OTA record — is the fixture
value verbatim.
