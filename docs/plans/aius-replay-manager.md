---
title: Aius SD replay manager
type: plans
scope: component
component: aius
status: in-progress
---

# Aius SD replay manager

## Objective

Provide a separate SD-hosted replay page for representative IEC 61937/DTS
captures. Replay is an independent device service: navigating between the
replay manager and DSP editor must not stop an active replay.

## Contract

- Replay files are uploaded to `/aius/replays/` and contain the 16-bit
  IEC 61937/S/PDIF payload-word stream consumed by `aius_pipeline_push`.
  Raw DTS access-unit uploads are not accepted in the first implementation
  because they bypass the real input framing boundary.
- File names are restricted to safe ASCII replay IDs and the server never
  accepts a client-supplied path outside `/aius/replays/`.
- Existing authenticated SD upload semantics are extended to
  `/sd/replays/<id>`. Uploads are transactional and may be resumed with
  fixed-size chunks without loading the whole file into RAM. A replay ID is
  one to 48 characters of `[A-Za-z0-9_-]`, and the stored file is
  `/aius/replays/<id>.iec`. An upload never replaces the file selected for
  replay until its final rename completes.
- Replay bytes are little-endian 16-bit IEC 61937 payload words. Every chunk
  has an even byte length and a byte offset aligned to two; an odd byte,
  invalid range, or total-size change rejects the chunk. The reader converts
  each pair to one `uint16_t` word and uses the compiled default
  `AIUS_IEC61937_PACK_16` packing. No raw DTS-access-unit or arbitrary PCM
  replay is accepted by this contract.
- Uploads use `PUT /sd/replays/<id>` with `Content-Range: bytes <start>-<end>/<total>`
  and `X-Aius-SD-Auth`. The first chunk creates `<id>.part` and a small
  `<id>.meta` record containing total size and next offset. Later chunks must
  begin at the recorded next offset; a repeated identical range is reported
  as already applied, while a gap or conflicting total returns 409. A final
  chunk is flushed, closed, validated for even size, and atomically renamed to
  `<id>.iec`; `.part` and `.meta` are removed. Disconnects preserve the
  partial upload and `GET /api/replays` reports it for resume. Only one upload
  or SD transfer job may run at once.
- Upload chunks are exactly 4096 bytes except the final chunk, which may be
  shorter but must be non-zero. `start` and `end` are inclusive, `end-start+1`
  must equal the body length, `total` is stable and even, and `end+1 <= total`.
  A committed chunk returns 200 with `{ "ok": true, "next_offset": N }`.
  A repeated committed range returns 200 with `{ "ok": true,
  "already_applied": true, "next_offset": N }`; gaps, overlaps, changed
  totals and range overlaps return 409; malformed headers, odd boundaries,
  odd body lengths, and invalid IDs return 400. A disconnect
  before commit leaves the prior next offset unchanged; the client retries
  that chunk. A disconnect after commit is discovered through the next offset.
- Route/auth matrix is fixed: `GET /api/replays` and
  `GET /api/replays/status` return 200 without auth; `POST
  /api/replays/start`, `POST /api/replays/stop`, `DELETE /api/replays`, and
  `PUT /sd/replays/<id>` require `X-Aius-SD-Auth`; `GET /sd/replays/<id>` also
  requires auth. Missing or invalid auth returns 401. Invalid IDs return 400,
  unknown IDs return 404, active-file conflicts return 409, and an occupied SD
  job returns 503.
- `GET /api/replays` returns `{ "ok": true, "files": [{ "id": string,
  "bytes": u32, "duration_seconds": number|null, "upload_next_offset":
  u32, "complete": boolean, "active": boolean }] }`. Start returns
  `{ "ok": true, "state": "prebuffering", "id": string }`; stop returns
  `{ "ok": true, "state": "stopped" }`; delete returns
  `{ "ok": true, "deleted": string }`. All errors return
  `{ "ok": false, "error": string }` with the status specified above.
- The replay reader runs outside the audio callback and feeds a bounded
  double-buffer queue. The audio callback consumes queued 16-bit words only;
  SD reads, seeks, file opens, and directory scans are forbidden there.
- The reader maintains a prebuffer watermark before starting and reports
  underruns. Looping seeks to offset zero while the final buffers remain in
  flight; a loop is gapless when the queue remains above the watermark.
- At most one replay is active. Upload and delete are rejected for the active
  file; starting another replay stops the current replay only after the new
  file has been opened and its prebuffer is ready. Stop resets the replay
  pipeline classifier and queue, but does not alter DSP configuration or page
  state. Optical input is suspended while replay is active and resumes after
  replay stop; the source switch occurs at a service boundary and the audio
  callback sees only one source at a time.
- The source state machine is `OPTICAL -> SWITCHING_TO_REPLAY -> REPLAY` and
  `REPLAY -> SWITCHING_TO_OPTICAL -> OPTICAL`. The service sets a pending
  source, disables the optical producer, resets the pipeline, waits for the
  replay prebuffer, then publishes the source enum. The audio callback reads
  the published enum once per block and consumes only that source. Stop waits
  for the current block boundary, drains/discards replay words, resets the
  pipeline, re-enables optical production, and publishes `OPTICAL`.
- Starting a different replay while already in `REPLAY` uses
  `REPLAY -> SWITCHING_TO_REPLAY -> REPLAY`: the service marks the old reader
  inactive, resets the word queue and pipeline, opens and prebuffers the new
  immutable file, then publishes the new replay ID. The audio callback keeps
  consuming the replay source enum throughout; if the new prebuffer fails, the
  service publishes `ERROR` and then resumes optical through the normal stop
  transition. There is no optical handoff during successful replay replacement.
- Replay control is HTTP-first and USB-control-compatible. The first HTTP
  surface uses fixed control routes; the parser additionally extracts and
  validates the replay ID only for the SD upload resource:
  - `GET /api/replays` list IDs, sizes, durations, upload offsets, and
    active/selected state;
  - `GET /api/replays/status` report state, file offset, queue depth,
    underruns, loops, decoder metrics, and current input class;
  - `POST /api/replays/start` with `{ "id": "...", "loop": boolean }`;
  - `POST /api/replays/stop`;
  - `DELETE /api/replays` with `{ "id": "..." }`;
  - authenticated `PUT /sd/replays/<id>` with `Content-Range` for file content;
  - `GET /sd/replays/<id>` for authenticated download/resume verification only.
- `GET /api/replays` and `GET /api/replays/status` are unauthenticated and
  read-only. Start, stop, delete, and upload require the existing
  `X-Aius-SD-Auth` token. The header name is exact and the token comparison
  uses the existing auth helper. Invalid or missing auth returns 401 without
  changing state. A control request changes state at the service boundary,
  never inside an audio block.
- The replay page is `/sd/www/replays.html`. It polls replay status and
  meters, provides a file picker plus upload progress, start/stop/loop/delete
  controls, and links to `/sd/www/index.html` (DSP editor) without reloading
  or resetting the device. The DSP editor links back to the replay page.
- The flash recovery page links to both SD pages but does not embed their
  controls. Missing SD reports the pages as unavailable.
- Stored paths are `/aius/www/replays.html`, `/aius/www/replays.js`, and
  `/aius/www/replays.css`; browser paths are `/sd/www/replays.html`,
  `/sd/www/replays.js`, and `/sd/www/replays.css`. The page reports a clear
  unavailable state when SD is absent.
- The existing DSP editor source is the SD asset set under
  `firmware/sd/www/` when present; its page is stored as
  `/aius/www/index.html`. The embedded recovery page is the `http_home_page()`
  function in `firmware/src/optical_main.cpp`. Integration
  adds visible links from that function and from the SD editor to both browser
  paths. A missing-card request to either page returns 503 JSON from the
  firmware route; a missing individual asset returns 404.

## Bounds and timing

- Individual replay size is bounded by the filesystem and the implementation's
  32-bit file-offset/status representation; FAT32's practical single-file
  ceiling is approximately 4 GiB, or about 6.2 hours at 192 kB/s.
- HTTP upload bodies remain bounded to the existing request limit. Large files
  use resumable range/chunk requests and never require a large HTTP response.
- The replay reader uses two fixed buffers in OCRAM, each a multiple of the
  512-byte SD sector size: two 4096-byte byte buffers for the SD producer and
  a 4096-word SPSC word ring for the callback consumer. The producer owns the
  byte buffers; the callback owns the ring read index; only atomic index
  publication crosses the seam. These buffers are `DMAMEM`/OCRAM and are
  statically allocated. The minimum start watermark is 2048 words and the
  measured refill margin is exposed in status. If the queue falls below the
  watermark, the service increments `underruns`, pauses ingestion safely, and
  reports the condition rather than blocking the audio callback.
- Replay timing is based on the source's 48 kHz IEC word cadence. The reader
  must not run as fast as SD; it schedules words against the audio pipeline's
  consumption budget and throttles when the queue is full.
- The callback-facing replay API is `aius_replay_pop_words()` and is bounded
  to the available ring contents; it performs no SD, allocation, locking, or
  seeking. The service-facing API owns file handles, refill, loop seek, and
  lifecycle. At EOF, loop mode reopens/rewinds only from the service task;
  non-loop mode marks EOF and lets the queue drain.
- On replay start, the service acquires the single SD-job lock, opens the
  immutable `.iec` file, fills the 2048-word watermark, then marks replay
  active and resets the pipeline. While active, upload, download, list, and
  delete are rejected with 409/503 as appropriate; the replay reader owns the
  SD handle. Stop releases the reader and lock only after the source switch and
  pipeline reset complete. A completed upload may replace an inactive file,
  never an active one.
- SD lock precedence is: active replay owns its open reader; an upload or
  download request returns 409 while replay is active; list/status use cached
  metadata and return 200 without touching the card; start/stop return 409
  while an upload or download job is active; delete returns 409 for an active
  replay and 503 for another SD job. The reader never shares a `File` handle
  with the upload/download state machine.
- SD admission is serialized by the service task: HTTP handlers only enqueue
  one bounded operation record and never acquire the SD lock. The service
  accepts an operation only when `sd_owner == idle` or when it is the owning
  replay-control transition; otherwise it returns the deterministic conflict
  above. The owner is published before opening a file and cleared after close,
  rename, or rollback, so start/stop and upload/download cannot interleave.
- IEC conversion is explicit: each stored pair is `[low_byte, high_byte]`,
  converted to `(uint16_t)low | ((uint16_t)high << 8)`. The native fixture
  contains preamble words `0xF872, 0x4E1F` in that representation and verifies
  DTS classification. A file with odd length or a partial final word is
  rejected before activation.
- Status JSON is fixed-shape and reports `state` (`stopped`, `prebuffering`,
  `playing`, `eof`, `underrun`, `error`), `id`, `file_bytes`, `offset_bytes`,
  `duration_seconds`, `loop`, `loops_completed`, `queue_words`,
  `watermark_words`, `underruns`, `input_class`, `decoder_errors`, and
  `upload_next_offset`, and `decoder` with `classification`, `bursts_seen`,
  `frames_decoded`, `last_status`, and `errors`. `offset_bytes` is the next
  byte to be read from the active file; `duration_seconds` is total bytes /
  192000 for the fixed 48 kHz 16-bit transport. Listing scans completed `.iec`
  files in the service task, caches their byte size and derived duration, and
  reports `null` only for an incomplete `.part` upload or an unavailable file.
  Decoder fields are zeroed with `classification: "unknown"` while stopped.
  Byte counts and counters are unsigned 32-bit values; durations and queue
  values are numeric units, not formatted strings.

## UI behavior

- The replay page is a standalone static HTML/CSS/JS asset set served from SD,
  not part of the upstream repository application dashboard.
- Navigation is ordinary browser navigation. Playback state lives in firmware,
  so leaving the page does not stop replay; returning reconstructs the view from
  `/api/replays/status`.
- The page shows per-file size, estimated duration, upload state, active file,
  current position, loop count, queue fill, underruns, classifier state,
  decoder errors, and links to the live meter/editor surfaces.
- Upload progress uses the server's `upload_next_offset` and resumes after a
  refresh or disconnect. A token is entered manually or pasted from the
  USB-only `AIUS AUTH` response; no browser-to-USB bridge is assumed.
- Uploading, deleting, and changing the active replay requires a visible auth
  token field or a token obtained over USB; playback status itself does not.

## Phases

### Phase 1: contract and native route tests

```yaml
unit: test-surface
seams: [HTTP route parser, replay JSON model, upload path contract]
contract-artifact: this plan
fan-out:
  - name: replay-route-tests
    owned-paths: [firmware/lib/aius_http/test/replay*]
review-intensity: active
commit-checkpoint: native HTTP tests for all methods, replay-ID/path rejection,
Content-Range parsing, auth header extraction, and control body shapes
```

### Phase 2: replay service

```yaml
unit: component-behind-interface
seams: [SD reader, bounded word queue, pipeline push boundary]
contract-artifact: spec-only
fan-out:
  - name: replay-service
    owned-paths: [firmware/lib/aius_replay/**]
review-intensity: active
commit-checkpoint: native queue/timing/loop tests; no SD access from callback-facing API;
byte-order, odd-byte, overflow, underrun, and EOF behavior covered
```

### Phase 3: optical HTTP integration

```yaml
unit: vertical-slice
seams: [HTTP routes, SD jobs, replay service, status serializer]
contract-artifact: spec-only
fan-out:
  - name: replay-integration
    owned-paths: [firmware/lib/aius_http/**, firmware/src/optical_main.cpp]
review-intensity: active
commit-checkpoint: target build and route/service integration tests, including
upload resume/rename lock behavior and source switching
```

### Phase 4: SD pages and regression fixtures

```yaml
unit: component-behind-interface
seams: [replay API, DSP editor navigation, persistent playback state]
contract-artifact: spec-only
fan-out:
  - name: replay-page
    owned-paths: [firmware/sd/www/replays.html, firmware/sd/www/replays.js, firmware/sd/www/replays.css]
review-intensity: observer
commit-checkpoint: static asset contract tests and target asset smoke
```

## Acceptance

- A replay can be uploaded, listed, started, stopped, looped, and deleted
  without firmware flashing.
- Leaving `/sd/www/replays.html` for the DSP editor and returning preserves
  playback and reports accurate position/queue/underrun state.
- A replay exercises IEC classification, DTS de-framing, decoding, DSP, and
  meter telemetry through the same input boundary as optical operation.
- SD stalls never block the audio callback; an underflow is observable and
  recoverable.
- Native tests, target build, and live HTTP smoke pass before commit.

This repository has no Aius-specific requirements registry or `Aius.Regress`
project. Requirement coverage is recorded by the native HTTP/replay tests,
static-page contract tests, target build, and live smoke/soak evidence named in
the phase checkpoints; unrelated repository telemetry gates are not part of
this feature's acceptance. The clean-room verifier also reported those absent
repository-wide gates and the expected absence of implementation at the
pre-build boundary; those are not replay contract findings.
