---
title: Aius read-only DSP configuration API and model surface
status: planned
---

# Aius read-only DSP configuration API and model surface

## Objective

Expose the compiled optical DSP schema and the active runtime selection through
one bounded read-only HTTP model. Make the model visible from the embedded page
and describe the same wire shape in the app-hosted OpenAPI document.

## Current gap

`firmware/config/optical_dsp.json` is converted at build time into bounded
profile and mix tables by `scripts/build_config.py`. The firmware currently
returns only numeric `mode` and `mix` selections from `/api/status`; the
embedded page shows names but not EQ sections or resolved mix coefficients.
The SD `/aius/config.json` file is a separate feature-flag document and is not
the DSP schema.

## Contract decisions

- Add `GET /api/dsp/config`; it is read-only and does not touch SD or flash.
- The response is bounded by the existing HTTP response budget and contains:
  - `schema_version` (integer, initially `1`)
  - `sample_rate_hz` (integer)
  - `profiles[]`: `id`, `eq[]`
  - `eq[]`: `type` (`bypass`, `highpass`, or `peaking`),
    `frequency_hz`, `q`, and `gain_db`
  - `mixes[]`: `id`, `center_gain_linear`, `surround_gain_linear`,
    `lfe_gain_linear`, `headroom_linear`, and `clipfree`
  - `active`: `profile_id`, `profile_index`, `mix_id`, `mix_index`,
    `profile_pending`, and `mix_pending`
- The finite generator limits are eight profiles, eight mixes, two EQ sections
  per profile, 24 ASCII characters per ID, and unique IDs within each table.
  The response model is capped at 6,144 JSON bytes, below the 8,192-byte HTTP
  response capacity. The build generator rejects over-limit counts, IDs with
  unsupported characters, duplicate IDs, and non-finite numeric values before
  emitting the header.
- OpenAPI marks objects `additionalProperties: false`, arrays with their exact
  `maxItems`, numeric fields with units and finite-number expectations, and
  `schema_version` as the compatibility discriminator.
- `id` is the stable config name, not an array position. Indices are included
  only as diagnostic compatibility fields.
- `headroom_linear` is the resolved runtime value. `clipfree` indicates that
  the configured mix selected the DSP clip-free headroom policy; the response
  does not expose the build-time string sentinel. For the current default
  downmix coefficients, clip-free resolves to
  `1 / (1 + 0.7071067811865475 + 0.7071067811865475 + abs(lfe_gain_linear))`;
  standard and lfe-minus10 retain unity headroom.
- `center_gain_linear` is the shared FC-to-L/R coefficient. Increasing it
  raises dialogue/voice relative to FL/FR; zero removes the centre channel.
- `surround_gain_linear` is the shared SL-to-L and SR-to-R coefficient.
  Increasing it blends the rear channels more strongly while preserving their
  left/right placement; zero removes the surround contribution.
- The coefficients are symmetric by design. Independent SL/SR gains are not
  part of this model because they would change image balance rather than
  control surround blend.
- Mix coefficients are finite and bounded to `0 <= gain <= 4` linear
  (`-inf` is not a valid control value; the upper bound is approximately
  `+12 dB`). Configured non-clipfree headroom is bounded to `0 < h <= 1`.
- The endpoint reports the compiled DSP table, not the SD feature flags and not
  an editable configuration. A later persistence/write API must version its
  own contract rather than silently reusing this read-only model.
- The built-in page adds a compact monospace DSP section: sample rate, active
  profile/mix, pending state, profile EQ rows, and mix rows. It fetches the
  endpoint independently of the status poll and remains usable without SD.
- OpenAPI is the canonical external model description. It includes schemas,
  field descriptions, enums, examples, and the new path. The device page
  links to the route and includes the field names so the recovery interface is
  self-describing even without the app dashboard.

## Phases

### Phase 1: contract artifact and adversarial review

**unit:** response model, bounded JSON shape, and OpenAPI schemas  
**seams:** generated DSP tables ↔ model fields; model ↔ OpenAPI/page contract  
**contract-artifact:** this plan, `swagger.json`, model example  
**fan-out:** OpenAPI/test owner edits app contract; firmware owner consumes only the resolved field list  
**review-intensity:** field naming, numeric units, enum drift, response-size bound, and future write/persistence confusion  
**commit-checkpoint:** JSON validation, OpenAPI tests, and the RED review resolutions recorded before firmware implementation

Add the model schemas and example to OpenAPI, extend the app regression to
assert the route, required fields, bounds, enums, and example, and record the
6,144-byte response bound. The clean-room review found the initial contract
RED; these finite bounds, route requirements, and resolved headroom semantics
are the amendments required before Phase 2.

### Phase 2: bounded firmware endpoint

**unit:** read-only DSP model serializer and HTTP route  
**seams:** `AIUS_OPTICAL_*` generated tables ↔ serializer; serializer ↔ HTTP response  
**contract-artifact:** `GET /api/dsp/config` response and native route tests  
**fan-out:** HTTP parser/tests own route recognition; optical firmware owns serialization and runtime selection fields  
**review-intensity:** every supported EQ/mix variant, unknown enum guard, truncation, pending transitions, and no SD/flash calls  
**commit-checkpoint:** native HTTP route/method tests, generator validation tests, target build, and response-size assertion green

Implement the route and serializer using fixed buffers and `snprintf`-style
capacity checks. Serialize the generated table rather than duplicating values
in firmware code. Return a deterministic error if the model cannot fit the
response budget, and add a build-time/native assertion covering the configured
maximum table dimensions.

### Phase 3: embedded model view and target validation

**unit:** built-in DSP configuration panel and live endpoint smoke/soak checks  
**seams:** page fetch ↔ model rendering; endpoint polling ↔ audio critical path  
**contract-artifact:** embedded page field list, OpenAPI example, soak evidence  
**fan-out:** embedded HTML/JS owner edits `optical_main.cpp`; validation owner runs target smoke and soak  
**review-intensity:** absent SD, malformed feature config, pending changes, large model, and concurrent endpoint polling during soak  
**commit-checkpoint:** page smoke, OpenAPI regression, target response comparison, and soak with zero audio/HTTP-service overruns

Render the model without adding control semantics: this surface explains the
active compiled configuration; existing mode/mix POST controls remain the
separate runtime-selection interface. Validate that the endpoint and page
continue to work with the SD absent and while the HTTP soak is exercising the
control surface.

## Test scope and evidence

There is no `Aius.Regress` project or hardware requirements registry in this
repository. The authoritative regression scope is the existing
`firmware/scripts/test.sh --environment native --suite http`, the generated
config validation test added to that runner, the Vitest OpenAPI contract test,
the target PlatformIO build, and live target smoke/soak evidence. The embedded
page contract will also have a Playwright fixture regression that loads a
captured model response and asserts the field-to-DOM mapping; target-only
network checks remain a separate hardware gate.

## Acceptance gates

- `GET /api/dsp/config` returns the complete compiled profile/EQ/mix model with
  stable IDs and resolved units.
- OpenAPI schemas and examples describe every returned field and enum.
- The embedded page visibly reports the same active profile, EQ sections, mix
  coefficients, and pending state.
- The model is read-only, performs no SD or flash I/O, and stays within the
  response capacity without affecting audio processing.
- Existing `/api/status`, mode/mix controls, fallback page, and SD feature
  configuration continue to work unchanged.
