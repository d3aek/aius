---
title: Aius SD-backed configuration and web assets
status: active
---

# Aius SD-backed configuration and web assets

## Objective

Move persisted DSP configuration and optional web assets off program flash while
keeping the audio path deterministic and retaining a minimal embedded recovery
page when the card is absent or invalid.

## Decisions

- The SD card is optional. Boot must remain successful without it.
- Active DSP configuration is always held in internal RAM and applied only at an
  audio block boundary.
- Configuration writes are explicit, bounded, and journaled. UI changes do not
  write the card automatically.
- Web assets are served in bounded chunks; no request may block the audio path.
- The embedded page remains the fallback and reports SD availability/config state.
- Asset paths are allowlisted; traversal, symlinks, and arbitrary filesystem
  enumeration are not exposed.

## Current implementation slice

The firmware now initializes the optional built-in SD card and exposes bounded
raw file operations for `index.html`, `app.js`, `style.css`, and `config.json`:

- `GET /api/sd` reports card availability.
- `GET /sd/{resource}` streams the file in bounded SD/RAM chunks.
- `PUT /sd/{resource}` streams through `/aius/.upload.tmp`, then replaces the
  target. `Content-Length` is required; the body is no longer limited by the
  firmware request buffer.
- `DELETE /sd/{resource}` removes an allowlisted file.

The embedded page remains the root page in this slice. Uploaded `index.html`
is available explicitly at `/sd/index.html`; root-page handoff is part of the
static asset server and control-surface migration phases. `config.json` is
stored but is not yet an active DSP configuration source; the build-time JSON
continues to define the current runtime profiles until the journaled parser and
activation phases land.

## Phases

### Phase 1: bounded runtime configuration

**unit:** portable config parser, validator, and journal record contract  
**seams:** JSON bytes ↔ validated runtime config; config ↔ audio-boundary apply  
**contract-artifact:** `config/optical_dsp.json`, parser API, CRC/versioned journal layout  
**fan-out:** parser tests own `lib/aius_config`; firmware adapter owns SD I/O  
**review-intensity:** exhaustive malformed, range, truncation, duplicate, and CRC cases  
**commit-checkpoint:** native config tests and cross-build green

Implement read-only load first, with factory defaults on any error. Limit the
profile count, EQ sections, names, and numeric ranges at parse time.

### Phase 2: SD persistence adapter

**unit:** Teensy SD initialization, two-slot journal read/write, and status fields  
**seams:** Arduino SD filesystem ↔ portable record validator  
**contract-artifact:** `/aius/config.0`, `/aius/config.1`, generation/CRC rules  
**fan-out:** SD adapter owns only `optical_main.cpp` and its SD helper; no DSP edits  
**review-intensity:** missing card, power loss between slots, full card, corrupt CRC, downgrade  
**commit-checkpoint:** build green and no-card boot path validated

### Phase 3: bounded static asset server

**unit:** allowlisted SD asset lookup and chunked HTTP response service  
**seams:** HTTP path ↔ asset manifest; SD read slice ↔ Ethernet write slice  
**contract-artifact:** `/aius/index.html`, `/aius/app.js`, `/aius/style.css`, MIME map  
**fan-out:** asset server owns HTTP adapter changes; page remains fallback-owned  
**review-intensity:** traversal, missing files, oversized files, disconnects, and timing budget  
**commit-checkpoint:** native route tests, target build, and fallback page smoke test

### Phase 4: control-surface migration

**unit:** SD page controls and explicit save/reload operations  
**seams:** browser API ↔ volatile config; save action ↔ journal writer  
**contract-artifact:** `GET/PUT /api/config`, `POST /api/config/save`, status telemetry  
**fan-out:** page assets own UI; firmware API owns validation and persistence  
**review-intensity:** pending-vs-active state, reboot recovery, and no-write-on-slider-change  
**commit-checkpoint:** HTTP control tests, SD fixture tests, and soak with card absent/present

## Acceptance gates

- No SD card produces the existing embedded page and factory DSP defaults.
- Invalid or interrupted config writes never prevent boot and never partially
  enter the audio path.
- Runtime control changes do not write flash or SD until explicit save.
- SD reads and writes stay outside the audio interrupt and bounded service slice.
- Removing the card during HTTP service does not reset or stall audio.
- The complete SD-backed page is optional; the fallback page always remains usable.

## Extension plan: SD scheduling, feature flags, and physical-access authorization

### Phase 5: budgeted SD service scheduling

**unit:** deferred SD read/write jobs and cached utilization refresh  
**seams:** HTTP transport ↔ SD job queue; SD job completion ↔ HTTP response  
**contract-artifact:** bounded job state, 512-byte transfer slices, status timing counters  
**fan-out:** firmware SD scheduler owns `optical_main.cpp`; runner owns timing assertions  
**review-intensity:** disconnects, partial writes, card removal, queue starvation, audio-overrun correlation  
**commit-checkpoint:** build, native parser/runner tests, target CRUD, and soak with zero audio overruns

SD writes must not run synchronously inside the HTTP service budget. The HTTP
handler will stage bounded data and the main-loop SD service will perform one
small transfer at a time, preserving atomic temporary-file replacement. Usage
scans remain cached and refreshed by the SD maintenance service.

### Phase 6: SD feature-flag configuration

**unit:** bounded feature-flag loader with defaults and invalid-file fallback  
**seams:** `/aius/config.json` bytes ↔ volatile flags; flags ↔ HTTP/DSP policy  
**contract-artifact:** `features` object with `sd_web`, `sd_config`, `remote_control`, and `telemetry` booleans  
**fan-out:** portable flag parser/tests own the config contract; firmware adapter owns loading; OpenAPI owns exposure  
**review-intensity:** missing card, malformed JSON, duplicate keys, unknown flags, safe defaults, reboot persistence  
**commit-checkpoint:** native parser tests, target load/status test, and config replacement test

Flags are loaded into RAM and never consulted from the audio interrupt. Invalid
or absent configuration leaves safe defaults. The authentication policy is not
feature-toggleable, so a malformed or malicious config cannot disable write
authorization for assets.

### Phase 7: physical-access SD write authorization

**unit:** per-device firmware-derived token disclosure over USB and HTTP header verification  
**seams:** USB `AIUS AUTH` command ↔ token; `X-Aius-SD-Auth` header ↔ write policy  
**contract-artifact:** token format and authenticated PUT/DELETE rules in OpenAPI and runner  
**fan-out:** firmware auth owns USB/HTTP code; runner owns credential acquisition/use tests; docs own operator procedure  
**review-intensity:** missing/wrong/replayed token, config exception, malformed headers, reboot stability, no HTTP disclosure  
**commit-checkpoint:** native auth tests, USB command smoke test, authenticated asset CRUD, unauthenticated config update

The token is derived from firmware-held device material and a compiled salt,
printed only in response to an exact physical USB command. Asset PUT/DELETE
requires the token in `X-Aius-SD-Auth`; `config.json` PUT remains the explicit
operator exception requested for frequent user changes. DELETE of config and
all non-allowlisted paths remain rejected.

### Revised file and configuration boundary

The fixed four-file allowlist is too narrow for iterative UI development. The
replacement contract is:

- `/aius/www/` is a path-confined UI asset tree. Arbitrary nested files are
  accepted within bounded path and request limits; `..`, duplicate separators,
  absolute paths, hidden upload staging names, and filesystem escape are
  rejected. UI asset PUT/DELETE requires `X-Aius-SD-Auth`.
- `/aius/config.json` is not directly writable over HTTP. The public section is
  updated through a dedicated API operation after parsing and ACL validation.
- The public configuration ACL is firmware-owned and declares which keys may
  be changed without the token. Protected DSP, network, auth, and filesystem
  policy keys cannot be changed through that exception.
- Invalid JSON, schema violations, rejected public keys, and failed SD commits
  are retained in status as a bounded error code/message and never enter the
  active runtime configuration.

JSON remains the external format because the browser dashboard and future
tooling need nested objects, arrays, and a stable self-describing contract. It
will be parsed only on SD load or an explicit configuration update, outside the
audio interrupt, using a bounded tokenizer/validator. A line-oriented key/value
format would be cheaper to parse but would duplicate the web contract and make
ACL/schema evolution harder; it is not warranted for this device.

## Adversarial review resolutions

The clean-room review returned RED. These constraints resolve its findings before
implementation:

- The UI root is `/aius/www/`, with a maximum decoded path length of 96 bytes,
  maximum eight components, maximum 32 bytes per component, and only ASCII
  alphanumerics plus `.`, `_`, `-`, and `/`. Percent encoding, backslashes,
  empty components, `.` and `..` are rejected rather than normalized. The
  firmware never exposes directory enumeration or symlink traversal.
- There is one SD job at a time and one 512-byte SD transfer slice per main
  loop. HTTP body receipt, SD writes, response reads, rename/rollback, and
  utilization scans are separate job states. A disconnect cancels the job and
  removes only its uniquely named temporary file.
- Replacement uses a unique temporary name and rollback protocol: rename the
  existing target to a backup, rename the temporary file into place, restore
  the backup if the second rename fails, and delete the backup only after
  commit. The old target is never removed before a replacement is durable.
- The unauthenticated configuration exception is `PUT /api/config/public`, not
  direct `PUT /sd/config.json`. Its body is a bounded JSON object containing
  only the firmware-owned public ACL keys `features.sd_web`,
  `features.remote_control`, and `features.telemetry`. It performs merge-and-
  replace of those keys, rejects unknown or duplicate keys, and cannot modify
  DSP, network, auth, filesystem, or asset policy.
- `/aius/config.json` is parsed only when loaded or explicitly updated. The
  accepted initial schema is the feature object above, with safe defaults on
  missing or invalid data. Status retains `config_valid`,
  `config_error_code`, and `config_error_count` until the next successful load
  or reboot.
- The USB credential is a fixed-format per-device token derived from
  firmware-held device material and a compiled salt. The exact physical USB
  command is `AIUS AUTH\n`; it prints the token once per command and never over
  Ethernet. Asset writes and deletes require an exact constant-time comparison
  of `X-Aius-SD-Auth`; public-config writes do not. The token is not accepted
  for protected configuration or filesystem paths.

### Phase 8: self-hosted maintenance tooling

**unit:** device-hosted upload form and standalone shell uploader  
**seams:** browser/file picker ↔ authenticated asset PUT; shell file tree ↔ device API  
**contract-artifact:** upload form served by the device; `tools/aius-sd-sync.sh`  
**fan-out:** embedded UI owns the form; shell tool owns transport and token input; OpenAPI owns examples  
**review-intensity:** nested paths, auth failures, config ACL separation, partial sync, missing tools, and no-repo execution  
**commit-checkpoint:** shell syntax check, mocked HTTP integration tests, and target upload smoke test

The device home page will include an upload form and exact curl-compatible
request examples. The standalone script will accept a device URL, a local
`www/` tree, and an auth token obtained over USB; it will upload files using
standard HTTP PUT with `Content-Length`, preserve relative paths, and refuse to
send `config.json` through the asset path. It will run from a copied file on a
maintenance host and will not depend on Python, PlatformIO, or this repository.
