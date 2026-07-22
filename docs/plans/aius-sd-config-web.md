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
- `GET /sd/{resource}` reads a file, capped at 1536 bytes.
- `PUT /sd/{resource}` writes through `/aius/.upload.tmp`, then replaces the
  target, capped at 1536 bytes.
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
