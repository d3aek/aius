---
title: Aius network and firmware recovery hardening
type: plans
scope: component
component: aius
status: in-progress
tags: [firmware, ethernet, recovery, watchdog, testing]
updated: 2026-07-25
toc: true
---

# Aius network and firmware recovery hardening

The existing hardening is not accepted. Repeated local fixes changed timing and
retry behaviour without proving that initialization, socket ownership, USB
recovery, or watchdog recovery is bounded. This plan replaces those claims with
explicit portable state machines, one-shot target adapters, and evidence that
exercises the deployed firmware.

## Shared delivery sequence — 2026-07-24

**This section is identical in [`aius-network-hardening.md`](aius-network-hardening.md)
and [`aius-dsp-config-api.md`](aius-dsp-config-api.md). Change both together.** It
supersedes the separate execution handoffs both documents previously carried. Both
plans are active, they share one critical path, and neither finishes without the
other.

### Deciding principle

**Audio hot-path reliability decides every trade-off. Everything else is lower
priority.** Recorded 2026-07-25 as the standing tie-breaker for the rest of this
work, and it is not a tie-breaker of last resort — it is the first question to ask
of any choice.

Concretely, when a decision is contested:

- A feature that cannot be made safe for the audio path is dropped or deferred,
  not accommodated. Refusing a route is an acceptable outcome; risking a block
  callback is not. The `POST /api/replays/start` refusal from S2a is this rule
  applied.
- ITCM headroom belongs to code that genuinely runs per audio block. Anything
  else is `FLASHMEM` on each definition, and a slice that cannot hold to that is
  re-scoped rather than granted an exception.
- A bounded primitive beats a smaller diff. Where those conflict — as they did
  when growing the SD transfer slice would have made every primitive a 12 KB
  blocking call — the bound wins.
- Memory that is genuinely free is not an argument. OCRAM is plentiful and DTCM's
  free-for-locals zone is not; a change that spends the scarce bank to save the
  plentiful one is backwards.
- When a diagnostic or convenience field cannot be produced without reaching into
  a path that touches the audio timing budget, serve it as absent and say so.

### Resume here

**Phase 14 is delivered and the board is running it.** All fifteen slices landed
(S0, P1a, P1b, R1, R2, P2, S1, C1, S2a, S2b, S3a, S3b, S4, S5, S6, S7), plus 14b
(meters) and 14c (replay refill onto a bounded job). Four of those slices were not
in the original breakdown and were forced by what the work found: **C1** when S2a
linked the portable engines and RAM1 went 8,612 bytes over, **S3a/S3b** when S3
proved too large for one reviewable unit, and **S6/S7** when S5 found an
acceptance criterion unmet and then that nothing proved the device met it.

#### The board, and how to reach it

Serving at **192.0.2.66**. USB needs a group wrapper because the login is stale:
`sg dialout -c "<command>"` for the CDC port, `sg plugdev -c "<command>"` for the
Teensy loader — they are different groups and both are needed.

Flashing is now reliable: `AIUS BOOTLOADER <id>` over CDC enters HalfKay on the
first attempt (five consecutive trials with a reflash between each), against 48
failed attempts before that fix. The SD maintenance token is read with
`AIUS AUTH <id>`; it is stable for the life of the board and
[`docs/reference/usb-control-protocol.md`](../../docs/reference/usb-control-protocol.md)
explains why and where to keep it. The card's web tree is provisioned and correct.

**Confirmed on hardware:** boot, one epoch per `loop()`, the watchdog not
resetting in normal operation, SD probe and both config loads, the status surface
on all three channels, `POST /api/reboot`, software bootloader entry, the SD
directory index, and DHCP reaching ONLINE on generation 1 with no fault after the
readiness gate landed.

**Not yet confirmed on hardware:** anything in the audio path. There is no S/PDIF
carrier — the optical input is unsoldered — so `blocks` is 0, the DSP has never
run, and the meters endpoint has never published. The per-block cost in 14b is
still a static estimate.

**The next flash is the interesting one**, and carries three unvalidated things
at once: the SD failure diagnostics (`0bcc32eec`), and the clock handover
(`24ff70f7b`) which should make replay actually produce audio. What to look for,
with the input still unsoldered: start a replay and **`blocks` on
`GET /api/status` should climb continuously** while `overruns` stays at or near 0
— both were frozen at 0, which is how the defect was found. Stop it and `blocks`
should freeze with the output going digitally silent and `stop_reason:"request"`.

That run also finally yields the **measured per-block DSP cost** from the DWT
counters on `GET /api/meters`, which is the last estimate standing in this plan
and a prerequisite for Phase 14e.

Once the input is soldered: `blocks` must climb from passthrough alone with no
replay, and **starting a replay against a live carrier must be refused with
`409 replay_carrier_present`** — the refusal landed, so the start no longer
reaches `stopped`. `stop_reason:"carrier"` and its type-3 event are now reachable
only when the carrier arrives *during* an already-running replay, which is the
one case the refusal cannot cover.

#### What is in flight

Two slices were running when this session ended. **Check their state before
starting anything**: `git status`, and re-run the standing sweep.

Both landed before the session closed. The memory gate now agrees with the linker
symbols and reports `omits: NonCacheable 9756B` explicitly, and a `memory` suite is
registered and green.

#### The 409 refusal — shipped

`AIUS_OPTICAL_APP_REPLAY_CARRIER_PRESENT` runs from the enum through the
dispatcher arm, the replay-start error table and the `app_replay_start()` guard,
so a start against a live carrier now returns `409 replay_carrier_present`
instead of being admitted and reversed at the next checkpoint. It is a distinct
token from the generic `409 replay_start` on purpose: a client must be able to
tell "that id is unusable", which it retries with a different id, from "the input
is live", which clears only when the carrier goes away. A half-refusal reusing
the existing token was deliberately not shipped.

The guard sits ahead of the `g_sd_ready` and id-validity check, so a live carrier
refuses regardless of SD state and the client gets the accurate reason rather
than `replay_start` from an unrelated precondition.

**The guard itself has no native coverage and cannot have any.** `optical_main.cpp`
is target-only and `optical-cutover` is a structural suite; the dispatcher half is
covered by `optical-app`, but `AudioInputSPDIF3::pllLocked()` is proven only by
the target build and the on-device pass with the input soldered.

`stop_reason` is documented in [`aius-dsp-config-api.md`](aius-dsp-config-api.md)
§The 409 refusal, with all three values. Its durable home is arguably
[`aius-replay-manager.md`](aius-replay-manager.md)'s status-JSON bullet alongside
`state`, and `swagger.json` carries no replay schemas at all —
`input_class`, `loops_completed` and `stop_reason` are undocumented there. Both
are follow-up work, recorded here so they are not lost.

#### Open defects, in the order they are worth taking

1. ~~**`mem_report.py` under-reports DTCM by ~10.8 KB.**~~ **Fixed.** The gate now
   reads the linked ELF's section table and the linker's own symbols instead of
   regexing `teensy_size:`, so an orphan-placed section can no longer be invisible
   to it. Measured on `teensy41-optical`: DTCM free-for-locals is **9,280 bytes**
   (`_estack 0x20050000 - _ebss 0x2004dbc0`), not the 19,036 the summary claimed;
   the difference is `NonCacheable` at 9,756 bytes, which the report now names
   explicitly rather than dropping. The `teensy_size` summary is still parsed, but
   only to cross-check, and every bank where it disagrees is reported with the
   sections that account for the delta. A `memory` suite drives the CLI over a
   committed reduction of a real image and is registered in `scripts/test.sh`.
2. **`500 sd_failure` on large SD uploads**, roughly 1 in 15, succeeds on retry.
   Diagnostics landed (`0bcc32eec`) and are **not yet flashed**. A structural hole
   was found and is real — the root driver's yield cannot see an HTTP transaction
   admitted but whose `SD_START` has not yet been emitted, a one-epoch window — but
   the arithmetic gives ~1e-4 against an observed ~1-in-15, three orders of
   magnitude short. The one-line fix is recorded in §Phase 14f and deliberately
   **not applied**, because it would look like a resolution while possibly leaving
   the real defect. Flash, reproduce, then read `sd_diag.last_failure`; the
   convicting values for each hypothesis are tabulated there.
3. ~~**`"source":"default"` is a hardcoded literal** in the status JSON format
   string.~~ **Fixed.** It now serves `g_dsp_source_sd ? "sd" : "default"` — the
   same fact, from the same variable, as `GET /api/dsp/config`, so the two can no
   longer disagree. Note the field is the **effective DSP model source**, not the
   audio source: `optical_main.cpp:246`, the DSP GET serializer and the `Status`
   schema in `swagger.json` all pin it to `default | sd`, and the
   audio source is `state`. This entry previously read as though it should track
   the audio source; that reading contradicts the wire contract and was not
   implemented. `g_dsp_source_sd` is a latch — set on a successful SD override
   load and never cleared — so status inherits exactly the DSP GET's behaviour,
   latch included.
4. **`record_event` has only two call sites** — the S/PDIF lock transition and the
   test-tone toggle. Everything else observable is a counter, not an event. Closing
   this is the prerequisite to any push observability being worth having; see the
   syslog assessment.
5. ~~**The suite registry is hand-maintained with no guard.**~~ **Fixed.** The
   `registry` suite parses `scripts/test.sh` — it does not carry its own copy of
   the list — and fails on five kinds of drift: a suite directory with no arm, an
   arm naming a path that does not exist, an **entry point a registered arm never
   runs**, and either catalogue (the `# Suites:` header, the README table)
   disagreeing with the arms in either direction. The third check is the sharper
   one and is the class `test/config/test_config.c` actually was: a registered
   directory whose arm had forgotten one of its sources, which a directory-level
   check alone would miss. Suite name and directory name are deliberately not
   assumed equal — `test/dts/` backs three arms and `codec-api-contract` lives in
   `test/codec-api/` — so ownership derives from the paths each arm names. The
   non-suite allowlist is one explicit entry (`fixtures/`, shared LFS data with no
   runner), enumerated rather than pattern-matched so a real suite can never be
   excused. **Running it found no remaining orphans**: the audit's three were
   closed by earlier sessions, and the guard is now what keeps them closed. The
   true count is **36 arms over 30 suite directories**, not the "33 suites" this
   plan previously asserted. Adding a suite is now three lines the failure message
   dictates verbatim.
6. ~~**"At most one SD primitive per epoch" is false at checkpoint 5.**~~
   **FIXED 2026-07-26 (`5429235c1`), and the entry was stale for a day —
   2026-07-26 evening's handoff still listed it open.** Both ungated walks got
   the `app_read_directory` treatment in the replay-epoch commit, which shares
   the file and the contract: `http_replay_list_json` and `app_replay_delete`
   gate on `sd_busy()` and refuse `503 sd_busy` (existing vocabulary), the
   listing caps at the same 128-entry constant `app_read_directory` uses and
   reports `"truncated"` so a cut-short listing cannot pass for complete, and
   the memory-only start/stop/status arms are deliberately not gated — asserted,
   so a start stays admissible while the previous selection's refill drains.
   Known consequence: during playback the refill owns ~61 % of epochs, so
   `GET /api/replays` refuses about that often. The gate also makes an
   upload/listing overlap impossible — a plausible but unproven contributor to
   defect 2, deliberately not claimed as closed there.

   The original entry, for what it bounded: three routes reached the card
   outside the job engine; `app_read_directory` was the designed, bounded case,
   while the listing walked `/aius/replays` with `for (;;) openNextFile()` — no
   cap, no gate — and the delete spent `SD.exists()` + `SD.remove()` ungated.
   On a card with a large replays directory that was the most plausible
   unexplained 8 s reset in the image.
7. **FIXED 2026-07-26, and this entry understated it.** The demotion is
   `NVIC_SET_PRIORITY(IRQ_PIT, 224)` in `stack_start()`
   (`aius_network_teensy.cpp`), guarded three ways: a source guard that reads
   the framework's own `IRQ_SOFTWARE` value rather than pinning it, a
   behavioural assertion in `network-transport` that the demotion is re-applied
   on **every** network generation, and `IRQ_PIT` registered in `fake-fidelity`.

   **Two corrections, and the first inverts the conclusion.** "Equal priority
   means neither preempts" describes `fnet_poll` against the S/PDIF **DMA** ISR
   (both 128). It does not describe the path that carries the signal
   processing: `update_all()`, `enqueue()` and the whole of `aius_dsp_process`
   run in the audio **software** interrupt, which `AudioStream::update_setup()`
   sets to **208** (`cores/teensy4/AudioStream.cpp:424`). Lower is stronger on
   the NVIC, so `fnet_poll` did not merely fail to yield to the DSP — **it
   preempted it, a thousand times a second, mid-block.**

   Second: the fix cannot be applied once at boot.
   `IntervalTimer::beginCycles()` re-applies the instance's own default on every
   `begin()`, and `beginNoWait()` runs afresh per generation, so a setup-time
   override is silently undone by the first network restart. Observed as a
   negative control: a once-only variant passes every other assertion and fails
   exactly the per-generation one.

   224 is one *implemented* level below 208 — this M7 decodes four priority
   bits, so 209 would have been the same level and a no-op. That is the
   discriminating assertion the guard makes, and asserting `224 > 208` instead
   would have accepted the no-op. No vendor patch was needed: all four PIT
   channels share `IRQ_PIT`, so the NVIC write is exactly what the private
   `IntervalTimer::priority()` would have done.

   The original entry, retained because its measurement note still stands:

   **The FNET poll timer shares the audio ISR's interrupt priority**, so the
   plan's central isolation argument has a hole. `beginNoWait()` starts an
   `IntervalTimer` running `fnet_poll` every 1,000 µs for the life of the network
   generation (`vendor/NativeEthernet/src/NativeEthernet.cpp:167`). Teensy startup
   sets every IRQ to priority 128 (`cores/teensy4/startup.c:136`), the S/PDIF DMA
   attaches without overriding it
   (`evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp:67`), and
   `IntervalTimer` defaults to 128 (`cores/teensy4/IntervalTimer.h:108`). Equal
   priority means neither preempts the other, so **every microsecond `fnet_poll`
   spends at 1 kHz is a microsecond the 2.67 ms block callback waits.** The
   isolation argument is framed throughout as "epoch loop, not audio ISR", which
   is true of this firmware's own code and untrue of the stack it links. The
   duration of one `fnet_poll` call is not recorded anywhere in the repo and is
   the measurement that sizes this. Note this is also the only I/O actor outside
   the epoch loop, and it owns the 9,756-byte `NonCacheable` FEC buffer inside
   DTCM plus a 64 KiB FNET netheap from the OCRAM `new` heap
   (`NativeEthernet.cpp:90`) that the 91,456-byte OCRAM figure does not include.
8. ~~**Three JSON cursors, one seam, not hoisted.**~~ **Fixed.** `lib/aius_json/`
   now carries the eight primitives all three consumers genuinely share, and the
   803 lines of duplication are gone. Nothing had to be loosened: the replay
   parser's 60-body table passes unchanged, and the two places its grammar
   legitimately diverges — a literal form with no boundary check, and a
   borrow-in-place string reader that refuses backslash outright — are expressible
   as two-line wrappers over shared primitives rather than second copies. Each
   library keeps its own key set, bounds, depth cap and error enum, and
   `aius_dsp_config`'s `strtod`-free number accumulation stayed with its caller
   because hoisting a single-use helper is the accretion this seam exists to avoid.

   **It paid more than it was expected to: −3,004 bytes of code, of which −2,088
   came out of ITCM.** `aius_config.c` never carried `FLASHMEM` on its cursor, so
   those primitives were competing with the audio pipeline for FlexRAM; the shared
   library is `FLASHMEM` throughout and moved them to FLASH. Nobody had noticed.

   Two corrections to the record while resolving it. The extracted cursor was
   **not** more permissive than what it replaced on the `"1 2"` case — both
   original `value_boundary()` implementations also accepted whitespace and relied
   on the caller's next structural check, so the new rule (read past whitespace,
   then require `,`, `}`, `]` or end, without moving the cursor) is *stricter* than
   the code it replaced. And a test asserting that `skip_value` should refuse
   `"[1],"` was itself wrong: all three consumers call it mid-object and then check
   the separator themselves, so had `skip_value` consumed or refused the comma,
   `{"unknown":[1],"known":2}` would have started failing — a real regression the
   suite would have reported as green. `lib/aius_config/src/aius_config.c`
   and `lib/aius_dsp_config/src/aius_dsp_config.c` carry the same recursive-descent
   cursor primitives — `aius_dsp_config.c:103` says so in its own comment
   (`/* ---- Cursor primitives (mirror aius_config.c) ---- */`) — and the replay
   body reader now carries a third, a `BodyCursor` local to
   `aius_optical_app.cpp`. Three concrete consumers clears
   [§Architectural posture](../../../../CLAUDE.md)'s seam-arity rule, so the
   duplication should become `lib/aius_json` (at-end / peek / skip-whitespace /
   consume / string / literal / number) with all three depending on it. It was
   deliberately **not** done in the whitespace fix: it rewrites two clean
   libraries that do not have the defect, and adds a source to four link lines in
   `scripts/test.sh`. The third cursor was written to the same primitive shape so
   the hoist is a mechanical lift-and-delete. Routing the replay body through
   `aius_dsp_config` instead was considered and rejected — its only public entry
   is a DSP-model parse, so it would need a new generic API on a schema-specific
   library and the same link edit anyway.
9. ~~**`write_once()` has the receive path's defect and cannot be fixed in
   place.**~~ **FIXED 2026-07-26 — see the correction block below; "cannot be
   fixed in place" was wrong.**
   `read_once()`'s FNET misclassification is fixed; the send path has the same
   asymmetry one layer down. `EthernetClass::socketSend`
   (`vendor/NativeEthernet/src/Nativesocket.cpp:327-334`) returns `0` for **both**
   FNET outcomes — no room in the output buffer (`fnet_tcp.c:848-852`, a
   would-block) and a socket that has left `SS_CONNECTED` (`:817-821`, a closure)
   — and `EthernetClient::write` calls `setWriteError()` on the collapsed zero. So
   **a peer that closes mid-response is reported as a socket fault rather than
   `CLOSED`**, and **a transient netheap shortage is reported as a socket fault
   rather than `WOULD_BLOCK`**. The second is reachable because
   `socketSendAvailable()` is `SO_SNDBUF - SO_SNDNUM` and does not model
   `_fnet_tcp_snd`'s extra clamp (`fnet_tcp.c:833-836`), so reported capacity can
   exceed what the send will accept. Fixing it needs either a vendor patch —
   pinned by `PATCHED_SHA256` and the source-contract suite — or replacing
   `g_client.write()` with a raw `fnet_socket_send()`, which changes the write
   contract and the `NOTICE.md` patch inventory. Neither is a drive-by.

   **FIXED 2026-07-26, and "cannot be fixed in place" was wrong.** The raw
   `fnet_socket_send()` route needed **no vendor patch, no `NOTICE.md` change and no
   `PATCHED_SHA256` movement at all** — the adapter is already the designated owner
   of `fnet.h`, so calling the primitive directly is exactly what `read_once()`
   already does. The entry over-estimated the blast radius and deferred a fix that
   was a like-for-like change.

   The correction that matters more is the *classification*, which this entry also
   got too small. Mapping the send's error set needs more than the receive path's
   two codes, because `_fnet_socket_set_error()` **substitutes** a socket's latched
   `local_error` for whatever code the failing call computed
   (`fnet_socket.c:104-108`). So an ICMP- or timer-latched `CONNRESET`,
   `TIMEDOUT`, `CONNABORTED`, `HOSTUNREACH`, `NOPROTOOPT` or `MSGSIZE` can surface
   from a send. Every *structural* failure row is unreachable by construction on
   this socket (bounded 512-byte writes, never `shutdown()`, never a null buffer,
   TCP always has `prot_snd`, urgent data compiled out), which is what makes the
   enlarged set unambiguous rather than a guess: an `FNET_ERR` here means the socket
   left `SS_CONNECTED`, whichever code appears. `MSGSIZE` is the sharpest case — the
   send path's own length check answers `INVAL`, so `MSGSIZE` on a send can *only*
   be the latch, and therefore can only mean the connection.

   Live-or-latent, split rather than one answer: the **`WOULD_BLOCK`-as-`FAULT` half
   was live** — a write `FAULT` runs `transport_fault()`, which zeroes
   `response_length` and tears the transaction down, so a transient netheap shortage
   truncated a response already promised to a client. The `CLOSED`-as-`FAULT` half
   was latent, because for a `WRITE` both arms produce identical control flow and
   the only difference, `last_transport_fault`, has no production reader.
10. **The network-transport fake models FNET wrongly in five further places**,
    which matters more than any single one of them: a committed test asserted the
    `read_once()` defect *as correct behaviour* for months. The cause was a
    vocabulary gap rather than a POSIX assumption — `fake/fnet.h` defined only
    `FNET_ERR` and `FNET_ERR_AGAIN`, exactly the set the adapter referenced, so
    `recv_result = 0` was the only expressible way to write a closure row. The
    header was derived from the adapter, then the test confirmed the adapter
    against the header, and nothing in that loop ever touched the dependency. The
    tell nobody noticed: `udp_receive_once`'s `0 → WOULD_BLOCK` row sits about
    forty lines below the TCP `0 → CLOSED` row **in the same test function**,
    asserting the opposite mapping for the same return value. Still divergent:
    `fnet_socket_close()` is `void` in the fake against `fnet_return_t` real, so
    no test can express a failing close and `close_once()` nulls the descriptor
    regardless; `fnet_socket_recv()` ignores `MSG_PEEK` and so is always a peek,
    while the vendor's `socketRecvAvailable()` genuinely uses it; `fnet_error_get()`
    returns `int8_t` rather than the enum; `fnet_socket_accept()` does not set
    `AGAIN` on nothing-pending; and `getsockopt(SO_SNDNUM)` is hard-wired to 0,
    which is precisely what conceals defect 9. **A fake derived from the caller
    instead of the dependency can only ever confirm the caller's beliefs.**

    **The two UDP mappings are now fixed, with one correction to how they were
    described here.** `MSGSIZE` is a counted drop
    (`WOULD_BLOCK` plus `aius_network_teensy_udp_oversize_drops()`) and a send-side
    `NOMEM` is `WOULD_BLOCK`. But **the remote teardown was latent rather than
    live**: both UDP call sites in `optical_main.cpp` discard every non-`PROGRESS`
    result — `platform_udp` returns `true` regardless, and nobody escalates
    `udp_send_frame`'s bool — and the supervisor consumes `AiusNetEvent`, not
    `AiusIoEvent`. So the misclassification was **silent, not harmful**, and the
    first consumer to act on that fault, which defect 4's observability work would
    naturally have been, would have inherited the teardown. The earlier "any host
    on the segment can force a network teardown" framing overstated it, and the
    correction is worth keeping because it is the difference between a live
    vulnerability and a trap laid for the next change.

    The reachable error set is now exactly three on receive (`>0`, `0`, `MSGSIZE`),
    and the ICMP-latched rows are *provably* unreachable because this socket is
    never `connect()`ed — which is what makes the new `MSGSIZE` arm unambiguous.

    **Four further findings, reported not fixed.** `DESTADDRREQ` is remotely
    triggerable and still faults: FNET performs no source-address validation, so a
    datagram from **0.0.0.0** is accepted and latched as the reply peer, and the
    reply then fails. Same family as the two fixed rows — a peer with no usable
    return address, not a dead socket — and the right fix is at the cause, refusing
    to latch an unspecified peer. Relatedly **the peer latch is written by a
    receive that failed**, because FNET copies the foreign address *before* the
    `MSGSIZE` check, so a dropped oversized datagram repoints the reply
    destination. A send-side `NOMEM` drop is still uncounted, for the same reason
    the oversize counter is not yet published: every serializer lives in
    `optical_main.cpp`. And **datagrams above 2,048 bytes are refused at FNET's
    input with no error latched at all**, so that class of flood stays traceless
    inside the vendor stack and the new counter cannot see it.

    One comment retired with it: `optical_main.cpp` claimed an oversized datagram
    "is truncated by the receive and then fails decode, so it still lands in
    `g_network_drops`". FNET answers `FNET_ERR` rather than a short length, so no
    decode ever ran and nothing was counted — it was never true.
11. ~~**The output clock is 44.1 kHz while everything claims 48 kHz.**~~
    **FIXED 2026-07-26 (`fd68229a0`, plus the `24ff70f7b` clock handover):**
    the engine is pinned to 48 kHz with exact PLL4 arithmetic confirmed in the
    linked image, and the roadmap's post-fix on-board measurements run at
    93.3–94.5 blocks/s (48 kHz), not 86.1 (44.1 kHz). The fix also uncovered
    the worse live-carrier consequence — ~30 blocks/s silently discarded
    inside the vendor output unit when the graph ran at the carrier's rate
    against a 44.1 kHz transmit DMA. The original entry:

    Measured on the board: 86.1 blocks/s × 512 frames = 44,083 Hz, matching
    the Teensy audio engine's default `AUDIO_SAMPLE_RATE_EXACT` of `44100.0f`
    (`cores/teensy4/AudioStream.h:58`), which `platformio.ini` did not
    override. 48 kHz material therefore played ~8.1 % slow, about 1.4
    semitones flat, with every counter honestly zero. The entry's standing
    ask — some surface should report the clock/content *ratio* rather than
    each side's own belief — is not yet delivered and is exactly what the
    item 3 measurement loopback makes checkable.
12. ~~**`POST /api/reboot` resets the board but never answers**~~ **FIXED
    2026-07-26 (`ee3470f13`), both halves, and wire-confirmed by the B6 run that
    evening — gate 2 passed against `3576c3e3560f`, which is the client actually
    observing the 200.** The reset now waits on `AIUS_HTTP_TX_IDLE` with no
    action outstanding (a drain signal, reachable only after the last `WRITE`
    was accepted and the `CLOSE` completed), bounded at 500 ms after which the
    reset happens regardless — the request is honoured, only the
    acknowledgement is best-effort. One correction to the mechanism as pinned
    below: `accept_application()` formats *and* emits the first write in the
    same step, so N+1 writes and "off by two" was one epoch generous; the
    verified ordering is in the comments now.

    The auth half is equally closed, and the follow-up audit this entry asked
    for ("verify which paths still take no auth") finds none: the four control
    routes (`/api/reboot`, `/api/start`, `/api/stop`, `/api/clear`) are gated
    **before** the dispatch switch so a fifth control route cannot silently
    skip the check, and every other mutating arm was already gated —
    `dispatch_replays` requires the token for everything but its two public
    reads (status, `GET` listing), the DSP-config and SD-file `PUT`s and the
    replay `DELETE` all call `auth_matches`. Read-only routes stay open,
    positively pinned by test. The token's honest strength (FNV-1a over the
    broadcast MAC through a repo salt — stops accidents, not attackers) is
    recorded in the commit and the auth header itself. Host tooling moved in
    the same change: `aius_recovery_smoke.py` authenticates its reboot, and
    `aius_http_runner.py` threads `--auth-token` / `AIUS_SD_TOKEN` through its
    soak and reboot paths.

    The original entry: two consecutive attempts each timed
    out client-side while `boot_generation` advanced 21 → 22 → 23, so the reset is
    performed and the response is never delivered. **This blocks the B6 reboots
    gate outright** — the runner cannot distinguish "reboot refused" from "reboot
    performed but unacknowledged", and reports `UNRESOLVED: the first reboot could
    not be requested`.
    This plan's §Optical runtime claims `loop()` performs the reset "after the
    epoch, so the HTTP response or UDP ack that asked for it has already been
    written" — and *written* is the load-bearing word. It means written into the
    transaction's buffer, not flushed onto the wire and acknowledged by the peer.
    **This is the same defect class as the bootloader-entry race**: `Serial.write()`
    returned "buffered" and the runtime treated it as "transmitted", and the fix
    there was to wait on a genuine descriptor-retirement signal bounded at 500 ms.
    The reboot path needs the equivalent — do not reset until the transaction has
    actually flushed and closed, bounded so a wedged peer cannot prevent a
    requested reset. Note the earlier "confirmed live" claim for this route was
    based on observing `boot_generation` increment, which is exactly the evidence
    that does *not* distinguish these two cases.

    **The mechanism is now pinned exactly, and it is off by two epochs.** In epoch
    N checkpoint 5, the dispatcher sets `g_reboot_requested` and returns the 200,
    and the service calls `retain_application_response()` — the response is stored
    as a pending event and **is not even formatted yet**; `format_response()` runs
    inside `accept_application()`, which only executes when the *next* step
    consumes the retained event. So formatting would happen in epoch N+1 and the
    first ≤512-byte `WRITE` in epoch N+2. `loop()` calls `reset_application()` at
    the end of epoch **N**. The comments at `aius_optical_app.cpp:1092`,
    `optical_main.cpp:3746` and `firmware-io.md:741` all claim the reset happens
    "after the response has been written"; all three are wrong, because the
    transaction is a strict one-action-per-epoch machine and "after the epoch that
    dispatched" is two epochs *before* the first response byte. **The fix is to
    gate the reset on the transaction reaching a drained terminal state** —
    `AIUS_HTTP_TX_IDLE`, for which `restart_idle()` is the natural signal — rather
    than a fixed epoch delay, which would be fragile against `WOULD_BLOCK`
    retries.

    **Separately and worse: `POST /api/reboot` required no authentication** —
    no `auth_matches` call, in none of the auth-gated arms, so any host on the
    subnet could reset the board at will, including mid-upload; `/api/start`,
    `/api/stop` and `/api/clear` were equally open. Fixed with the
    acknowledgement half as recorded above.
13. ~~**PCM passthrough sits 48.16 dB below the DTS path, and the limiter can
    never engage on it.**~~ **FIXED 2026-07-26 (`fc58ac343`) — see the
    correction block below.** Found writing [§Phase 14e](#phase-14e--signal-path-and-dsp-reference).
    `pcm_feed` does `int32_t sample = (int32_t)(int16_t)w[i]` with **no left shift**
    (`lib/aius_pipeline/src/aius_pipeline.c:96`), while the DTS decoder emits
    24-bit-range samples. So a full-scale 16-bit PCM carrier reaches the wire around
    **−54.2 dBFS**. Worse than the level itself: **every downstream threshold is in
    24-bit units** — the limiter's −1.0 dBFS, the true-peak skip gate, the meter
    peak and RMS — so on the PCM branch **the limiter can never engage and the
    meters read 48 dB low.** It is committed behaviour, pinned by
    `test/pipeline/test_pipeline.c:281`, and predates the 24-bit output path. The fix
    is a `<< 8` at the passthrough boundary plus golden updates, **not** widening the
    limiter threshold. Note
    [`aius-sample-rate-following.md`](aius-sample-rate-following.md) inventories both
    branches' *rates* meticulously and never mentions their *levels* — the same
    blind spot in a different dimension.

    **FIXED 2026-07-26, and two of this entry's own predictions were wrong** — kept
    because the corrections are the useful part. Details in
    [`signal-path.md` §Stage 4c](../reference/signal-path.md#stage-4c--pcm-passthrough).

    - **"a `<< 8` at the passthrough boundary" was the wrong place.** It would have
      worked and would have left the next producer free to repeat the mistake. The
      actual cause is that `AiusPcmBlock` documented its planes only as "signed
      32-bit" and **never declared a sample scale**, so the decoder's 24-bit choice
      and the pass-through's 16-bit choice were *both conformant*. The fix declares
      the scale once in `aius_codec.h`, derives `AIUS_DSP_SAMPLE_MAX`/`_MIN` from it
      rather than restating the number, and binds both implementations with a
      `_Static_assert` — §Convention enforcement's compile-time rung, which a shift
      at one call site does not reach.
    - **"plus golden updates" was not needed, and that is itself diagnostic.** All
      four `dsp-vectors` goldens are DTS, so every one was byte-unchanged. A defect
      that changes real audio while touching no golden vector means the corpus does
      not cover that branch — not that the change was small. The pass-through has no
      committed vector at all; its only coverage was a hand-written test that
      asserted the block against a copy of `pcm_feed`'s own expression, which is
      exactly why it printed "1024 frames byte-exact" for the whole life of the
      defect. Reverting the fix still leaves that check green; only the new
      contract-terms check (`pcm-scale`) fails, reporting the measured −48.16 dB.
14. **The DTS decoder does not use the 128 KB DTCM arena reserved for it.**
    `[env:teensy41-optical]` `build_unflags` removes
    `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free`
    (`platformio.ini:197`), and the shim's routing depends entirely on those flags.
    Confirmed in the linked image: **zero `__wrap_malloc` symbols**, plain `malloc`
    present. So the decoder handle, its scratch and the whole dcadec `ta` tree come
    from the newlib OCRAM heap, and only the ≈5.2 kB `AiusPipeline` instance is
    arena-allocated. **About 126 KB of the scarcest bank is reserved and never
    touched** — 40 % of DTCM, on a bank whose free-for-locals zone is 25,376 bytes.
    No plan records a rationale for that `build_unflags` line, and it contradicts
    [`aius-dsp-config-api.md`](aius-dsp-config-api.md) §Target memory strategy,
    which says deterministic DTCM access "is the whole point for the decode/DSP
    arena". **Do not simply delete the arena or restore the flags** — establish
    first whether the unflag was deliberate (a wrap that misbehaved on target would
    be a reason) and whether the decoder is latency-sensitive to heap placement.
    Either outcome is worth having: reclaiming it would roughly sextuple the free
    DTCM zone, and restoring it would give the decoder the determinism it was
    designed for.
15. ~~**`features.matrix` and `features.output_eq` are inert.**~~ **FIXED
    2026-07-26.** `apply_config()` now reads all five flags, guarded by an
    `optical-cutover` check that reads the **comment-stripped** body — this file
    explains itself at length and now discusses both flags at length, so a check
    satisfied by prose would pass against a firmware that only talked about them.

    **The design call, recorded because it had two defensible readings.**
    `output_eq` is mechanical: zero sections is already the bypass, so the flag
    selects the count. `matrix` is not, because the downmix is structurally
    unavoidable — six planes have to become two — so "bypass" had to be defined.
    Taken: **FL→L, FR→R at unity**, i.e. zero centre, zero surround, zero LFE and
    unity headroom, which is exactly what `aius_dsp_process()`'s accumulate loop
    reduces to with those values, needing no second code path. Rejected:
    substituting the *default* matrix, which would make the flag mean "ignore my
    settings" rather than "remove this stage" and would leave no way to hear the
    stereo pair without the fold. The editor's own hint text settles it — a stage
    is unchecked "for latency and diagnostic comparisons", and the comparison
    worth hearing is the one with nothing folded in. Unity headroom is set
    explicitly rather than relying on `clipfree_headroom()` returning 1.0 for
    zeroed gains, so a later change there cannot put a gain back into the bypass.
16. ~~**The default config makes the input stage a no-op that still costs.**~~
    **FIXED 2026-07-26.** `g_input_stage_active` now additionally requires that
    some channel would change a sample — unity gain with no sections is skipped,
    reclaiming **3,072 quantizes per block** on the shipped default.

    Skipping is behaviourally identical rather than an approximation: the downmix
    reads `block->planes` when `dsp.input` is null, and the stage's only effect in
    that state was to hand it a verbatim copy of those same planes. Unity is
    tested exactly because it is the value the conversion *produces* rather than a
    tolerance — `aius_db_to_linear(0.0)` is `pow(10, 0) == 1.0` on host and
    target. A present-but-bypassed EQ section is deliberately **not** identity:
    `eq_sections` counts what the cascade walks, and the config distinguishes "no
    section" from "a bypass section" on purpose.

    **Not yet measured on hardware.** The saving is arithmetic until a carrier
    exists; `blocks` is 0, so `dsp_cycles_last` cannot show it. Re-measure with
    the first real signal.
17. ~~**The retained boot ledger no longer survives a reset, and B6 gate 2 is
    blocked on it.**~~ **FIXED 2026-07-26.** The cause was neither a neighbouring
    symbol stomp nor anything in this firmware: **the i.MX RT1062 boot ROM
    overwrites three fixed runs of OCRAM on every reset**, and the 24-bit output
    ring moved `g_aius_boot_slots` into the largest of them.

    Measured directly, on the board, before any other code runs — the raw slot
    bytes were byte-identical at `startup_early_hook`, `startup_middle_hook`,
    `startup_late_hook` and inside `aius_boot_storage_teensy_load()`, which rules
    out the Teensy startup path and this firmware; and a fill-reset-scan probe of
    `0x20200000`..`0x2027F800` returns the same three runs for a `SYSRESETREQ`
    (`SRC_SRSR=0x02`) and for a WDOG1 timeout (`SRC_SRSR=0x10`):
    `0x20200C60`..`0x20201100`, `0x20201B80`..`0x20201C40`,
    `0x20203A60`..`0x20207F60`. `0x20205140` is inside the third.

    **Both addresses were accidents.** `0x20201120` retained because it happened to
    fall in the 2,688-byte hole between the first two runs. `DMAMEM` packs
    `.bss.dma` in object order, so the ledger's address was a function of every DMA
    buffer declared ahead of it — which is why a 16 KiB audio ring broke retention
    with no diff to the boot adapter at all. The fix is placement, not a
    workaround: `link/aius_teensy41.ld` (selected by `board_build.ldscript`) carves
    `.bss.aius_retained` out immediately before `.bss.dma`, pinning the slots to
    `ORIGIN(RAM)` = `0x20200000` by construction and below `_heap_start` by
    construction. The full measured map and the probe method are recorded in
    [`firmware-io.md` §Retained OCRAM](../reference/firmware-io.md).

    **The guard was promoted, because it passed while retention was broken.** It
    checked alignment, size, section and the CrashReport bound — all necessary,
    none sufficient. Now: two link-time `ASSERT`s (the address equals
    `ORIGIN(RAM)`; the region fits below the ROM's first run at
    `ORIGIN(RAM)+0xC60`), a post-link check that the symbol is at exactly the
    pinned origin and intersects none of the three measured runs, a source guard
    that rejects a `DMAMEM` declaration outright, and a drift test that keeps our
    copy of the framework linker script byte-identical to the framework's apart
    from the carve-out.

    **Live proof.** `boot_generation` increments on every software reset —
    1→2→3→4→5, then 11→12→13→14→15→16 during the bring-up timing run, then 20→21→22
    — each with `reset_source=2`. For the watchdog case, a `teensy41-optical-fault`
    image stalling at checkpoint 5 came back in **recovery-safe mode**, twice
    independently, which is reachable only when a retained `early_watchdog_resets`
    is incremented to 2 across two consecutive watchdog resets. The ledger now also
    survives a HalfKay reflash of a different build id, with the generation
    carrying across.

17a. **`boot_generation`, `reset_source` and `watchdog_early_resets` all read 0 in
    recovery-safe mode**, on both `/api/status` and USB `AIUS ID`. Recovery-safe
    runs only checkpoints 1 and 2 (`aius_runtime.c:81`), so
    `platform_reset_capture()` at checkpoint 0 never runs and never populates
    `AiusRecoveryStatus`. The consequence is that **the moment the recovery
    boundary engages, the three fields that would explain why read zero** — so B6
    gate 3 cannot distinguish "latched correctly after two watchdog resets" from
    "the ledger is broken again". Found while proving 17; not fixed, because the
    fix is in `optical_main.cpp`/the scheduler rather than the boot adapter. The
    cheapest form is probably to publish the reset-capture snapshot from
    `capture_retained_boot_state()` rather than from a checkpoint callback.

17b. **The 24-bit output ring assumes DMAMEM is zeroed at reset, and it is not.**
    `aius_spdif_sources.cpp:21-24` says the ring is "uninitialised at reset, which
    is correct -- both cursors start at zero". `.bss.dma` is `NOLOAD` and the Teensy
    startup clears only `_sbss.._ebss` (DTCM), so `written`, `consumed` and
    `underruns` hold whatever survived — and after a reset that is boot-ROM scratch,
    because they sit at `g_aius_spdif_tx24 + 0x4000`, inside the ROM's third
    clobbered run. `aius_spdif_tx24_fill()` then transmits ring contents for
    `written - consumed` frames, an unsigned difference that can be arbitrarily
    large. Bounded (the index is masked, so there is no out-of-bounds write) but
    audible. Not fixed: `lib/aius_audio/**` is outside the boot work's remit and the
    one-line fix — zero the three cursors in `aius_spdif_tx24_begin()` — wants an
    audible check.

    **FIXED 2026-07-26.** `aius_spdif_tx24_reset()` zeroes the three cursors, called
    from `aius_spdif_tx24_begin()` **inside the same critical section as the handler
    swap** — resetting after `attachInterrupt()` would leave a one-interrupt-period
    window in which `fill()` still runs against ROM values. The 16 KiB buffer is
    deliberately *not* cleared: with both cursors at zero the ring reads as empty and
    no sample is read before a producer writes it, so clearing it would be wasted
    stores before the first block.

    **Why no test caught it, which is the transferable part:** every ring test in
    `test_dsp.c` opens with `memset(&ring, 0, sizeof ring)`, so all of them assert
    behaviour from the one state the target never comes up in. The new test seeds the
    cursors with the ROM words actually measured at that address, asserts the
    pre-condition that the ring looks non-empty, and then asserts the *audible*
    property — a `fill()` straight after reset emits digital silence and counts the
    substitution rather than leaking the samples still in the buffer. Negative
    control observed at exit 134. A `memset`-first test is worth distrusting anywhere
    the object under test lives in `.bss.dma`.
18. **Five source comments still quote the pre-fix DTCM free figure** —
   `aius_runtime_teensy.h:17` ("~22 kB"), `optical_main.cpp:77` ("24,412-byte"),
   `:140` ("22 kB"), `:292` ("23 kB"), `:2109` ("22,684-byte"). Measured is 9,280.
   `firmware/README.md` was corrected with the gate fix; the source was not. Every
   placement decision those comments justify is still correct — more so — but the
   arithmetic in them is wrong.
19. **`read_once()` has the same latch-substitution hole the send path just had, and
    a committed row asserts the misclassification.** Found fixing defect 9.
    `_fnet_tcp_rcv()` itself fails only with `NOTCONN`/`CONNCLOSED`, so a *latched*
    `CONNRESET`, `TIMEDOUT`, `CONNABORTED`, `HOSTUNREACH`, `NOPROTOOPT` or `MSGSIZE`
    surfacing on a receive equally means the connection is gone — but `read_once()`
    maps everything outside its two codes to `FAULT`. `send_error_is_closure()`
    applies unchanged, so the fix is a one-line reuse. What makes it more than a
    tidy-up: `test_nativeethernet_behavior.cpp:1224` **pins the wrong answer**
    (`force_recv_error = FNET_ERR_NOPROTOOPT` → `FAULT`), so this is a committed
    contract row to invert, not a gap to fill. Left deliberately — the same shape as
    the two write rows just flipped, but outside that fix's brief.
20. **The fake gets `_fnet_socket_addr_check_len(addr, 0)` backwards, and the suite
    asserts the wrong answer — and the real behaviour implicates the vendor's
    accept path.** `test_nativeethernet_behavior.cpp:871-877` claims a zero length
    "is not checked at all". Real FNET is
    `if (addr && addr_len) {...} else { result = FNET_ERR_INVAL; }`
    (`fnet_socket.c:1856-1893`), and at the accept call site `addr` is
    `&sock->local_addr`, never null — so a zero `addrlen` is **refused with
    `FNET_ERR_INVAL`**, not skipped. That matters beyond the fake:
    `EthernetServer::poll()` passes an **uninitialised** `fnet_size_t fromlen`
    (`NativeEthernetServer.cpp:339`), measured as 0 on the host. On the board, a
    stack slot that happens to hold 0 makes every `poll()` accept fail *while the
    connection stays queued* — a retry loop rather than a dropped connection, which
    is a plausible contributor to accept-path stalls and should be checked against
    §Phase 12's poll-priority work (defect 12) rather than in isolation.

    **Fake and test row FIXED 2026-07-26; the vendor half is confirmed and
    deliberately NOT patched yet.** Three things were established rather than
    assumed:

    - **The `else` is real.** `_fnet_socket_addr_check_len()` ends
      `else { result = FNET_ERR_INVAL; }` at `fnet_socket.c:1887-1890`, so a zero
      length is refused. **Why the original reading went wrong is the transferable
      part**: the caller's guard at `:896` is `if (addr && addrlen)` where both
      operands are *pointers*, and the callee's at `:1860` is
      `if (addr && addr_len)` where the second is a *value*. The two conditions are
      textually identical and mean different things — one asks whether an address
      was supplied at all, the other whether its length is usable. Reading either
      in place of the other inverts the answer, which is exactly what happened.
    - **It is on the shipped path.** The accept is not in `EthernetServer::accept()`
      — that only hands out an already-accepted socket. The real
      `fnet_socket_accept()` runs inside the FNET service callback at
      `NativeEthernetServer.cpp:334-340`, with `fnet_size_t fromlen;` uninitialised.
      **Every inbound connection this device accepts goes through it.** Both
      pointers are non-null, so the check runs against whatever the stack slot
      holds, and every value below `sizeof(struct fnet_sockaddr_in)` fails the
      accept while leaving the connection queued. Stack remnants are not uniformly
      random — they are dominated by small integers, zeros and prior loop counters —
      so the failing range is far more reachable than 16/2^32 suggests.
    - **The patch is a one-liner and the process already exists.**
      `src/NativeEthernetServer.cpp` is already an intentionally patched file with a
      `PATCHED_SHA256` baseline and a `NOTICE.md` entry, so
      `fnet_size_t fromlen = sizeof(from);` is a gated edit rather than a new
      precedent.

    **Not applied yet purely for sequencing**: the bring-up/accept timing was being
    measured on the board at the time, and changing the accept path underneath a
    running experiment invalidates its results. Apply it after those measurements
    land, and re-measure, since the fix may itself move the `http`-behind-`udp` gap.
21. **B6 gate 2's four "never became ready" lines were the ledger defect, not a
    second failure — but there is still a real timing gap, and it is on the board.**
    `observe_ready()` counts a channel ready only once `_is_new_boot()` is true
    (`aius_recovery_smoke.py:455-488`), i.e. once `boot_generation` exceeds the
    baseline. With the ledger broken that could never happen, so all four channels
    read "never" no matter how fast the network came up. One root cause, four
    symptoms.

    Re-measured with the ledger fixed, five cycles, `--serial` attached, the gate's
    own observer and its deadlines untouched (only `ready_limit` widened so a miss
    is timed rather than recorded as "never"). Every cycle `reset=software`
    (`SYSRESETREQ`, not cold), `early=0`, `rsafe=False`, `network_restarts=0`,
    `network_last_fault=0`, and `boot_generation` +1:

    | Channel | min | max | mean | deadline |
    |---|---|---|---|---|
    | `usb` | 4.04 s | 4.09 s | 4.06 s | 2.0 s |
    | `dhcp` | 8.90 s | 10.56 s | 9.90 s | 7.0 s |
    | `udp` | 8.90 s | 10.56 s | 9.90 s | 8.0 s |
    | `http` | 9.15 s | 10.82 s | 10.16 s | 8.0 s |

    Stable, not variable: `dhcp`/`udp` are bimodal at exactly 8.90 s or 10.56 s and
    `http` is always one 0.25 s poll behind them, so HTTP is not separately slow —
    the three come up together with the lease and the 1.66 s step looks like one
    discrete retry.

    **The latency is the board's own, not the ack or the host.** `/api/events` on the
    boot that measured 10.56 s reports `DHCP_UPDATE` at `uptime_ms=9688` and the
    `ONLINE` state change at `uptime_ms=10539` — board uptime, so the ack-to-reset
    and host-side overheads are under 0.1 s. It is also **not DHCP negotiation
    overrunning**: `AIUS_NET_DHCP_DEADLINE_MS` is 5,000 and no
    `AIUS_NET_FAULT_DHCP_TIMEOUT` fired (`network_last_fault=0`,
    `network_restarts=0`), so the DHCP wait completed inside its own 5 s budget and
    started late. The unexplained time is therefore in stack/PHY bring-up before the
    DHCP wait begins — `Ethernet.begin()` and link establishment — and that is what
    to instrument next. Defect 20's uninitialised `fromlen` is a candidate for the
    `http`-after-`udp` gap but cannot explain a lease at 9.7 s.

    **The `usb` figure is a measurement artifact and should not be read as a board
    defect.** The normative deadline is "after USB enumeration, `AIUS ID` must
    respond within 2 s" (§Runtime order and budgets), but every cycle reported
    `usb_basis=ack`: `observe_ready()` never observed the port absent, so it charged
    the board for host CDC re-enumeration. It cannot observe it — each loop iteration
    spends up to 1 s on `http_status()` and up to 3 s on `udp_status()` (timeout 1 s
    × 3 retries) while the board is down, so the ~1.5 s enumeration gap falls between
    two `usb_present()` samples. Fixing the basis is a runner change, and the
    deadlines were deliberately left alone pending a decision on the DHCP figure.

    **Superseded 2026-07-26 by the post-fix B6 run.** Every figure in the table
    above predates the `usb`-basis fix, the DHCP-deadline move and the
    accept-length fix, and none of them should be quoted now. Re-measured over
    twenty-nine software reboots of `3576c3e3560f`: `usb` 1.51–1.64 s
    (mean 1.57), `dhcp`/`udp` 7.09–10.79 s, `http` 7.09–11.04 s. The "unexplained
    time before the DHCP wait" the entry asks about is now instrumented and
    accounted for — 2,165–3,624 ms of stack start, `beginNoWait()` and the
    RX-ready gate — and the remainder is the DHCP window itself, which is
    quantised at 4,003–4,005 ms or 8,005 ms rather than variable. The deadlines
    that follow from that are in
    [§Runtime order and budgets](#runtime-order-and-budgets).
22. **`GET /api/dsp/config` cannot report a committed config on a board with no
    carrier, so a client cannot tell "no SD config" from "config committed".**
    Found by the post-fix B6 gate 5. `source` and `config_generation` are written in
    exactly one place — `AiusAudioProcessor::enqueue()`'s staged-config arm
    (`optical_main.cpp:880-886`) — which runs at an *audio block boundary*. A board
    with `carrier: 0` and `blocks: 0` never reaches it.

    Proved directly rather than inferred: a `PUT` moved `sd_diag.http_jobs` 0 → 1
    with `http_faults` 0 and `job_state` 15 (`AIUS_SD_DONE`), so **the write commits**,
    while both reported fields stayed at `0` / `"default"` for ten seconds and across
    fifteen subsequent reboots.

    Same shape as 17a: a fact that exists is reported only on a code path that
    happens not to run. The gate now distinguishes the two with an explicit
    `sd-job` basis used **only** when the board provably cannot apply, and which
    reports what it did not check — a non-reporting board *with* audio still fails.
    The firmware fix is a design call (report a committed source separately, or set
    it at commit time rather than at apply time) and was deliberately not taken
    under a gate deadline.
23. **`AIUS_NET_DHCP_DEADLINE_MS` at 9,000 ms bought one mode, not headroom.** The
    DHCP window is quantised by `FNET_CFG_DHCP_CLN_RESPONSE_TIMEOUT_MS`:
    **4,003–4,005 ms on 27 of 29 boots, 8,005 ms on 2** — one lost `DISCOVER` and its
    retransmission. So 9,000 ms clears the two-`DISCOVER` case by under a second, and
    a three-`DISCOVER` boot (≈12,005 ms) lands straight back on the
    teardown-and-rebind path the 5,000 → 9,000 move existed to remove.

    Recorded rather than changed, deliberately: that constant was just moved and
    re-measured, and moving it again in the same session without a fresh
    distribution would repeat the error the first move corrected. Decide it against
    measured two- and three-`DISCOVER` frequencies, and note the trade — waiting
    longer is right when the lease is merely delayed, and wrong when the segment
    will never answer, which is what the backoff-and-retry path is for.
24. ~~**About 4 % of rapid sequential HTTP requests fail outright, and the
    device's own counters say nothing happened.**~~ **CLOSED 2026-07-26 as
    characterized vendor-stack behaviour — instrumented, counted, bounded,
    self-recovering; disposition and full measurement below.** Found while
    establishing a baseline for defect 7's fix, on a board with **no carrier
    and `blocks: 0`** — so this is not audio contention.

    Measured, 200 sequential `GET /api/status` at ~40–55 req/s, four runs (two
    with the poll-priority fix, two without it):

    | | failures / 200 | p50 | p95 | max |
    |---|---|---|---|---|
    | poll at 224 | 9, 6 | 3.7 ms, 3.6 ms | 4.5 ms, 4.8 ms | 1,060 ms, 3,103 ms |
    | poll at 128 | 8, 8 | 3.6 ms, 3.5 ms | 4.7 ms, 4.6 ms | 2,061 ms, 1,039 ms |

    **The two are indistinguishable**, which is what clears the priority change
    of causing it and is the only reason those columns are here.

    The shape to explain: a tight median with a handful of complete failures and
    multi-second outliers, while `network_drops`, `network_budget_overruns`,
    `network_restarts`, `network_last_fault`, `http_server_rebinds` and
    `event_overflows` all stay at 0 across the run. **A failure the device does
    not count is the same blind spot as the four `http_*` counters with no
    producer** — those are exactly the fields that would have named this, and
    they served constant zero. (An earlier revision cited them as "defect 16";
    they were never a numbered defect — defect 16 is the input-stage identity
    no-op. The counters were the §Accepted wire-behaviour changes zero from
    slice S2b.)

    **CHARACTERIZED 2026-07-26, same evening the instrument landed.** Flashed
    `cff77ab4641b` (counters live) and re-measured. Every hypothesis the entry
    carried is now answered, and **the lost-SYN one is dead**: the failures
    reach dispatch (`http_requests` counts them) and die on the send path.
    Five runs, the signature identical in all:

    - **Every client-visible failure is `RemoteDisconnected` at ~103 ms and is
      counted by `http_client_drops`, one for one** (plus the ambient poller's
      own losses — see below). 103 ms is `RESPONSE_TIMEOUT_MS` (100 ms) plus
      overhead: the transaction dispatched, formatted, and could not transmit
      within its deadline, so it dropped the client — the bounded machine
      doing exactly what it was designed to do.
    - **The trigger is a connection count, not a rate.** 150-request bursts
      are clean at 278/s (max 5.3 ms); 200-request runs fail at ~#180 whether
      driven at 37/s or 82/s; a 400-request run fails at #28–32 and again at
      #236–239 (~204 apart). Decisive: two 150-bursts ~2 s apart — first
      clean, second fails at #28, i.e. cumulative ~187 since the window
      opened. Lingering per-connection state accumulates across bursts.
    - **The stall and the drops are one event.** Each failure burst is
      preceded or accompanied by one *successful* request at ~1 s, ~2 s, ~3 s
      or ~4.1 s (this and the original table's 1,039–3,103 ms outliers are
      FNET slow-timer multiples, 500 ms tick). ~5–8 drops per event, then bulk
      recovery and another ~200 clean connections.
    - **The runtime is exonerated by its own numbers**: `http_service_overruns`
      0 across 59k epochs including the stall (no HTTP service call exceeded
      2 ms), `network_budget_overruns`, `network_drops`, `network_restarts`,
      `event_overflows`, `overruns` all 0. The wedge is the vendored stack not
      draining, not this firmware's scheduling.

    **Working model** (the one thing not yet pinned to a source line): each
    closed connection lingers in TIME_WAIT — `FNET_TCP_TIME_WAIT` is 48 slow
    ticks = **24 s** — holding its control state out of the wrapper's 64 KiB
    FNET netheap (`FNET_STACK_HEAP_DEFAULT_SIZE`, `NativeEthernet.h:42`).
    64 KiB / ~190-connection threshold ≈ ~345 B per lingering connection,
    which is TCB-sized. What performs the bulk reclaim at exhaustion is not
    yet identified (FNET has abort-on-pressure paths; the wrapper's slot table
    is nulled at `close_once()` and is not the accumulator). If it ever needs
    pinning, the next instrument is a netheap gauge — FNET's mempool free
    stats surfaced into `net_diag` — not more black-box runs.

    **Disposition: no firmware change.** The 100 ms response deadline
    converting a stack wedge into a counted, bounded drop is correct
    behaviour; the alternative — waiting out a 1–4 s stall per transaction —
    is the unbounded shape this plan exists to remove. No realistic client
    approaches the trigger (the dashboard polls at 1 Hz; ~22 connections ever
    linger in the window). A client that genuinely needs >150 requests in
    under 24 s should reuse connections. Raising the netheap would move the
    threshold, not remove the cliff, and costs OCRAM against no observed
    need.

    **Two side-findings from the same runs:**

    - **An ambient poller exists.** ~0.93 req/s of `GET /api/status` from
      elsewhere on the subnet (almost certainly a browser tab left on the SD
      dashboard) — invisible until the counters landed, and it perturbs
      measurement runs: it shares the single transaction, its requests queue
      behind and drop alongside a saturating run's (`http_client_drops`
      advanced by 8 and 12 against 6 and 9 client-visible failures). Baseline
      arithmetic on this board must subtract ~1/s, or the tab should be
      closed first.
    - `http_requests` climbs ~1/s on an "idle" board for the same reason —
      a zero-rate expectation would be wrong, not the counter.

    Two candidates worth separating first: the second-order accept behaviour
    behind defect 20 (the vendor `fromlen` patch is applied, but the accept path
    was never re-measured afterwards as that entry asked), and plain TCP
    connection churn — the ~1 s and ~2–3 s outliers sit suspiciously close to
    retransmission timeouts, which would make them a lost SYN rather than
    anything in this firmware.

    **This matters for signal testing specifically:** a UI that silently drops
    one request in twenty-five looks like a device fault, and during bring-up it
    would be attributed to whatever was just wired in.

#### Decisions taken, so they are not re-opened

- **A network syslog sender was assessed and declined.** Not on cost — the event
  ring has two call sites, so it would carry a lock change and nothing else. An
  absent collector is undetectable in both directions, and the board cannot claim
  a timestamp. Polling with the existing runner closes most of the gap, and the
  retained boot ledger already covers the pre-reset case better than log lines
  would. If ever built, it belongs after B6. Full reasoning in §Network syslog.
- **The link gate is kept though it is inert here**, because it is correct for a
  genuinely dead link and bounds that at 3 s.
- **The replay refill port stayed out of Phase 14** and landed as 14c.

#### Next

~~**14f, the first flash and assumption validation**, remains the gate before 14d
and 14e … Then B6, the live recovery smoke, which has never run.~~

**All of that is delivered. B6 ran end to end on 2026-07-26 against
`3576c3e3560f`** — gates 2, 4 and 5 pass, gate 3 passes on all seven checkpoints
the firmware can recover from, and checkpoints 1 and 2 are UNRESOLVED for a
structural reason no runner work can change: `aius_fault_checkpoint()` runs
*before* the checkpoint's service, and recovery-safe mode runs exactly those two,
so a fault image there hangs with no USB control path. **The watchdog and recovery
contract this plan exists to establish is now proven on hardware**, including
recovery-safe mode latching after two early watchdog resets and returning to
service unaided — a boundary that had never once executed on a real board.

**Next**, in the order they are worth doing:

- ~~**B7 (Phase 15, button-free flash tooling)**~~ **DELIVERED 2026-07-26.**
  `tools/net/aius_cdc_runner.py` owns the CDC control protocol, HalfKay entry
  and post-flash verification; `scripts/flash_optical.sh` is now a thin
  dispatcher over it; the `flash` suite is registered and green (25 tests).
  **Proven on hardware with zero button presses**: `8d71629652fb` →
  `382aa531f042`, entered HalfKay via the CDC route and verified the running
  identity against the one parsed out of the `.hex`.

  Three design points worth not re-deriving:

  - **Entry is judged by the port disappearing, never by the acknowledgement.**
    A request performed but unacknowledged is indistinguishable from one
    refused, which is the same confusion behind both the bootloader-entry race
    and open defect 12. So a lost ack with the port gone is a success, and an
    acknowledged request with the port still present is not.
  - **The identity oracle parses the image rather than re-running
    `build_metadata.py`.** A verifier that recomputes the build's own
    expression proves agreement and nothing else — the recurring failure mode
    this plan records. It is also why reassembly is by absolute address: a
    naive concatenating reader fabricates a well-formed revision from two
    halves 16 KB apart, observed as a negative control.
  - **Both `revision` and `build` are compared.** Rebuilding the same commit
    yields the same revision string, so a board still running the old image
    would verify against revision alone; the timestamp is what separates "the
    flash landed" from "the loader did nothing".

- ~~**B8** is now the only remaining phase that changes what an operator has to
  do physically — the zero-button-press gate (Phase 16b) run against the
  tooling B7 just delivered.~~ **DELIVERED 2026-07-26, and nothing an operator
  does for this board involves a button any more.** One button-free CDC flash
  and one production restore, each verified by revision *and* build stamp:
  `cff77ab4641b` (23:34:49Z) → `8623061c2fa7` (23:47:59Z) → restored
  `cff77ab4641b` byte-for-byte from the saved production hex, the runner
  confirming both the outgoing and incoming identity on each leg. Board
  healthy after restore (`boot_generation` 247, `config_valid 1`,
  `recovery_safe 0`). Details under Phase 16b below.
- **Open defects 22 and 23**, both raised by the passing run rather than by a
  failure, and both design calls rather than repairs.
- **B9**, the dashboard retirement, which is independent of all of the above.

### Why the two strands are coupled

A firmware flash attempt during the DSP `PUT /api/dsp/config` work found the
deployed board crash-looping through DHCP on the legacy pre-cutover
`optical_main.cpp` path — the exact unbounded-recovery failure the network plan
exists to close. Only Phase 14 fixes it.

In the same window the DSP strand built its config PUT, its `/aius/dsp.json`
override load, and its `dsp_upload` job bookkeeping directly on top of the legacy
ad-hoc SD job machine and the legacy blocking HTTP path — all of which Phase 14
deletes. So the DSP strand cannot ship without the cutover, and the cutover now
carries DSP behaviour it did not originally own. Everything else in the DSP strand
(the SD-hosted control surface) is genuinely independent and runs in parallel.

### Corrections to earlier claims in both plans

- **`runtime-target` was wrongly listed as a delivered regression suite.** It did
  not exist when this section was written; Phase 12 has since created it, along
  with `optical-app` from Phase 11. The standing guard is now `net`, `boot`,
  `network-transport`, `sd`, `sd-target`, `http`, `usb-control`, `runtime`,
  `runtime-target`, `optical-app`, `boot-target`, and `fault-target`.
  `optical-cutover`, `flash`, and `recovery-runner` still do not exist and are
  created by Phases 14, 15, and 16.
- **`PUT /api/dsp/config` was not described in OpenAPI.** The write phase shipped
  the firmware without reaching `swagger.json`, so the DSP plan's
  OpenAPI gate was quietly unmet. Closed at step B5. `GET /api/meters` is still
  absent and lands with 14b.
- **Phase 10a's stated justification was wrong in one detail.** The DSP config
  `PUT` does not need the SD publish mechanism: it parses and validates the model
  from the HTTP body itself and writes the accepted bytes verbatim. The real
  second publish caller is the **boot / hot-reinsert load** of `/aius/dsp.json`,
  once that moves onto a portable `CONFIG_LOAD` job in Phase 14. Phase 10a is
  still required — for that path. It is delivered (`df9ea4483`).

### Structural obstacles the cutover must clear

Recorded here because they are cheap to miss and expensive to hit mid-phase:

- **The legacy SD symbols collide with the portable header in two places.**
  `optical_main.cpp:118` declares its own `struct AiusSdJob`, unrelated to the
  portable one in `aius_sd.h`, and `optical_main.cpp:115` declares an
  `AIUS_SD_JOB_DELETE` enumerator that redefines `aius_sd.h:20`. It compiles
  today only because the file never includes that header — but
  `aius_optical_app.h:21` includes it unconditionally, so merely naming the
  callback table pulls it in. Both legacy names must be renamed before any
  cutover work, which is what slice S0 is for.
- **The composed storage service cannot start a job on its own.** It accepts
  `AIUS_HTTP_ACTION_SD_START` and nothing else (`aius_runtime_storage.cpp:250`),
  and it acknowledges `AIUS_SD_ACTION_PUBLISH_CONFIG` into a saturating
  `publish_dropped` counter rather than delivering the payload
  (`aius_runtime_storage.cpp:134`). Every boot-time SD load in Phase 14 needs
  both gaps closed first.
- **`AiusRecoveryStatus` carries no IP, link state or DHCP counters**
  (`aius_net.h:146`), while `optical_main.cpp` reaches `Ethernet.localIP()` /
  `linkStatus()` in fourteen places including the status JSON, the UDP status
  payload and USB `ID`. Dropping the vendor include without first adding an
  accessor to `aius_network_teensy` leaves the board undiscoverable headless.
- **Any SD job remounts the card.** `aius_sd_job_start` enters
  `AIUS_SD_PHASE_BEGIN` for every job kind (`aius_sd.c:407`) and the adapter's
  `AIUS_SD_ACTION_BEGIN` calls `SD.begin(BUILTIN_SDCARD)`
  (`aius_sd_teensy.cpp:137`), guarded only by its own `file_open` flag. It cannot
  see a foreign handle such as `g_replay_file` (`optical_main.cpp:68`), so a
  config `PUT` or a probe re-mounts the volume beneath an open replay file.
- **`stage_dsp_sd_override()` is a blocking synchronous read.**
  `optical_main.cpp:279` reads up to 12,288 bytes one byte at a time from
  `setup()` and from `service_sd()`, and puts a 12,288-byte array on the stack
  while the RAM1 free-for-locals zone is 22,684 B. It violates the normative
  one-SD-primitive-per-epoch rule head-on. Porting it is part of Phase 14, not
  optional cleanup.
- **The legacy watchdog interval is 16 s, not the pinned 8 s.**
  `watchdog_init()` writes `WDOG_WCR_WT(31)`, and the feed has three call sites —
  `setup:2593`, `loop:2607` and `loop:2618`. The normative contract is one 8 s
  interval with exactly one feed after a complete epoch.
- **ITCM headroom is 2,120 bytes** (98.92 % used) as of slice S1. Every new
  per-epoch service, dispatcher, and adapter must be `FLASHMEM` unless it
  genuinely runs per audio block. Apply the attribute on each definition — GCC
  does not inherit a `section` attribute from a forward declaration, so a
  prototype-only `FLASHMEM` silently leaves the code in ITCM. Phases 11 and 12
  both held to zero ITCM this way.

  The figure was 1,416 bytes until S1, which **gained** 704 bytes of headroom
  rather than spending any. Referencing the previously single-use static
  serializers from the new callbacks changed GCC's inlining decisions, pushing
  code that had been inlined into ITCM callers out into the FLASHMEM adapters.
  Behaviour is unchanged; codegen is not. Expect the same effect to run in
  reverse at S2b, when the legacy callers those serializers shared are deleted.
- **The portable engines land in ITCM, not FLASH, and this broke the build at
  S2a.** On Teensy 4.1 a function with no `FLASHMEM` attribute goes to ITCM, and
  ITCM shares FlexRAM with DTCM — so engine code competes directly with the audio
  path's own budget. Phases 11 and 12 held their *services* to zero ITCM, but
  nobody had annotated the engines underneath them, which were unreferenced and
  therefore invisible until S2a linked them. Measured at that point:
  `aius_http_transaction.c` 8,688 B, `aius_sd.c` 8,292 B, `aius_sd_teensy.cpp`
  7,156 B, `aius_http.c` 3,464 B, `aius_network_teensy.cpp` 3,284 B,
  `aius_boot_teensy.cpp` + `aius_boot.c` 1,936 B, `aius_net.c` +
  `aius_runtime.c` 1,272 B — against 2,120 B of headroom. RAM1 overflowed by
  8,612 bytes and the build went red.

  **The plan predicted this cost would land in FLASH. It does not.** Slice C1
  moves the wholly-cold units to flash and adds a post-link guard so a new
  unannotated engine fails the build instead of silently eating the audio path's
  headroom. `aius_dsp`, `aius_pipeline`, `aius_iec61937` and `aius_audio` stay in
  ITCM because they genuinely run per audio block.
- **The bank report does not yet include Phases 11 or 12.** Nothing references
  the dispatcher or the composed runtime, so `--gc-sections` drops every section
  and `nm` on `firmware.elf` finds none of their symbols. Phase 14 is where
  roughly 9.4 KB of FLASH and ~907 bytes of DTCM string data become real, and
  where the ~35 KB runtime instance must be placed in `DMAMEM` rather than the
  22,684-byte DTCM free-for-locals zone.

### Critical path

Two tracks. Track A has no firmware-source overlap and runs concurrently
throughout. Track B is serialized because every phase in it touches
`optical_main.cpp` or the files that replace it.

**Track A — SD control surface** (independent, no dependencies)

| Step | Work | Gate |
|---|---|---|
| A1 | **Done** — `firmware/sd/www/index.html` + assets: every config control mapped to the documented JSON model, meters and canvas waveform from `GET /api/meters`, matching the existing `replays.*` precedent | `test/sd-editor/`, 36 tests green |
| A2 | Target asset smoke — the page loads and drives the real device | folded into the single on-device session (B6) |

**Track B — bounded runtime cutover** (strictly ordered)

| Step | Work | Gate |
|---|---|---|
| B0 | **Done** (`df9ea4483`) — Phase 10a, SD job publish-capacity generalization | `sd` + `http` suites, target build |
| B1 | **Done** — Phase 11, optical application dispatch **including the DSP routes** | `optical-app` suite green |
| B2 | **Done** — Phase 12, Teensy runtime service composition | `runtime-target` suite green |
| B3 | Phase 14 — optical runtime cutover **plus the DSP SD-override load port**, eleven slices after the 2026-07-25 re-scope | `optical-cutover` + all component suites |
| B4 | Phase 14b — meters endpoint and accumulate wiring | `optical-app` + ITCM budget |
| B5 | OpenAPI catch-up — `PUT /api/dsp/config` **done**, `GET /api/meters` with 14b | Vitest OpenAPI contract suite |
| B5a | Phase 14c — replay refill onto a bounded SD job, deferred out of Phase 14 | `replay` + `sd` + `runtime-target` |
| B5b | **Phase 14f — first flash and assumption validation.** Nothing in Phase 14 has ever executed; this is where the plan's assertions become measurements | the recorded validation log, one verdict per assumption |
| B5c | Phase 14d — the firmware I/O reference: every path, its memory, its rationale, and why it cannot disturb the audio hot path | doc-index check + a real build's bank report |
| B5d | Phase 14e — the signal path and DSP reference: block diagram, per-stage implementation, memory, and the measured per-block budget | `dsp` suite + figures taken from the device |
| B6 | ~~Phase 16a — live recovery smoke on the deployed board~~ **DONE 2026-07-26** on `3576c3e3560f` | gates 2, 4, 5 PASS; gate 3 PASS on all seven recoverable checkpoints; 1 and 2 UNRESOLVED, structurally |
| B7 | ~~Phase 15 — button-free flash tooling~~ **DONE 2026-07-26** (`7670c5e03`) | `flash` + `usb-control` suites |
| B8 | ~~Phase 16b — the zero-button-press flash gate, re-run once B7 exists~~ **DONE 2026-07-26**: flash `cff77ab4641b`→`8623061c2fa7` and restore, both runner-verified, zero presses | one clean button-free flash and restore |
| B9 | Retire the the app dashboard, keeping `swagger.json` and its contract suite | `ui-contract` and Vitest suites green after removal |

Phase 13's code baseline is delivered and unchanged. Phases 1–10 and 10a are
delivered and are not re-reviewed. Each remaining phase's gate is its green
native suite plus an owned-diff review — nothing heavier. The per-phase
source-blind oracle chains and clean-room delivery audits described in the
delivered-phase blocks are historical and are not re-run.

### Scope moved by this reconciliation

- **Phase 11 owns the DSP routes.** Its route inventory is every current HTTP
  route *plus* `GET /api/dsp/config`, `PUT /api/dsp/config`, and `GET /api/meters`.
  Writing the dispatcher without them means writing it twice.
- **Phase 14 absorbs the DSP SD-override load.** `stage_dsp_sd_override()`'s
  blocking read is replaced by a portable `CONFIG_LOAD` job with
  `publish_capacity = AIUS_DSP_MODEL_MAX_JSON_BYTES`, and the staged model still
  applies at the audio block boundary. This is what Phase 10a exists for. The
  `dsp_upload` write path moves onto the portable `UPLOAD/REPLACE` job with
  `publish = false` — the model is already parsed and validated in RAM, so
  re-publishing the written bytes would buy nothing.
- **The meters phase becomes Phase 14b**, owned by the network plan's phase
  sequence rather than standing alone in the DSP plan, because it wires into the
  post-cutover runtime and audio path. Its contract stays the DSP plan's.
- **The two live-hardware gates merge into one session.** The DSP plan's target
  validation (default audio unchanged, config round-trip, meters render, editor
  loads, short soak) and the network plan's recovery smoke run against one flash,
  as B6.
- **Phase 15 moves after the first live validation.** One manual button press is
  accepted to get the post-cutover image onto a board that currently cannot be
  reached any other way; the button-free tooling then removes the need for the
  next one.
- **The the app dashboard is out of scope until B9.** It is superseded by the
  device's own SD-hosted control surface and is queued for deletion as the last
  step, not repaired along the way. Two consequences to expect rather than
  investigate: the `control-shell.json` browser scenario **fails today** and stays
  failing until B9, because it asserts `/api/mode` and `/api/mix` are present in
  `swagger.json` while the OpenAPI suite asserts they are absent; and the
  dashboard's mode/mix buttons already 404 and will simply keep doing so. Do not
  spend time on either. B9 removes the dashboard, that scenario, and the
  `ui-contract-allowlist.json` entry for its stylesheet together, and coordinates
  with the ui-kit accessibility rollout that currently enrols the app. Nothing
  earlier in the sequence depends on it, including Phase 14's removal of the
  firmware-side `MODE`/`MIX` route enumerators.

## Removed evidence apparatus

The build-identity / reproducibility / evidence apparatus is removed. It
reintroduced the SHA-manifest, environment-fingerprint, and ten-image receipt
machinery that [`CLAUDE.md`](../../CLAUDE.md) records was
deleted on 2026-07-18 and must not return for this personal build.
`build.sh` / `build_metadata.py` revert to plain build metadata (git revision +
short build id) while keeping the `teensy41-optical-fault <checkpoint>`
test-image capability that the watchdog smoke needs. The stopped Phase 13
identity-correction working set (`build.sh`, `build_metadata.py`,
`test_build_evidence.py`, `test_build_metadata.py`) is discarded, not
committed.

## Trusted fixture boundary

**The board is connected directly to the development host, and the network beyond
that host is reliable fixture.** Treat the DHCP server and network routing as
trusted components. A missing lease, unreachable service, failed software
bootloader transition, or post-flash mismatch is a firmware or firmware-tooling
failure until firmware evidence identifies a more specific cause.

This is not a formality. Attributing a fault outward — to a switch, a cable, or
anything past the host — without firmware evidence pointing there ends the
investigation at the one place it cannot be fixed. If the evidence has not been
found yet, the honest statement is that the cause is not yet identified, not that
it is external.

- DHCP is the production address source. Remove the static fallback and its
  separate initialization path.
- Host routing is pre-provisioned fixture state. Do not add route repair or
  route diagnosis to firmware acceptance.
- USB CDC is the recovery control plane. A running or watchdog-recovered image
  must accept a software bootloader command without a Program-button press.
- HalfKay transfer is trusted. A successful loader exit followed by the wrong
  application revision is a release failure.

This decision supersedes the DHCP/static-fallback requirements in
[`aius-optical-prototype.md`](aius-optical-prototype.md) for the deployed optical
image. UDP discovery remains useful for identity, but not as an alternative
addressing scheme.

## Clean-room findings

The current green tests and telemetry do not support the earlier hardening
claims.

1. **TCP writes are not bounded.** `http_send_content()` checks elapsed time
   around `EthernetClient::write()` in
   `firmware/src/optical_main.cpp:1248`, but the pinned
   NativeEthernet implementation enters
   `while(socketSendAvailable(s) < len){}` inside that call. The caller's
   100 ms check cannot run while blocked. SD response writes reach the same
   call at `firmware/src/optical_main.cpp:578` and
   `firmware/src/optical_main.cpp:619`.
2. **Static fallback can deadlock.** NativeEthernet's static-address
   `Ethernet.begin()` ends in `while(!link_status){}`. The firmware reaches
   this path after its DHCP attempt accounting in
   `firmware/src/optical_main.cpp:2297`.
3. **Network ownership is split.** `start_network()` uses NativeEthernet to
   initialize the stack, while `ensure_dhcp_client()` releases and recreates
   FNET's DHCP client through private headers. NativeEthernet's link callback
   also owns that DHCP client. `g_ethernet_initialized` therefore describes
   application intent, not the actual FNET lifecycle.
4. **A network reset is not a stack reset.**
   `reset_network_service()` clears application flags and stops UDP, but does
   not stop the HTTP listener, unregister the server service, or deinitialize
   FNET. Calling `EthernetServer::begin()` again may allocate another socket
   and service registration. `http_server_rebinds` counts attempts, not
   successful leak-free recovery.
5. **The watchdog is unqualified.** It is fed at multiple points, including
   immediately before the secondary network service, and records only raw
   reset-source bits. There is no retained last-progress checkpoint, no early
   reset-loop escape, and no target test proving recovery from each boundary.
6. **The tests miss the failure surface.** The native `net` suite tests only
   UDP framing, `http` tests only parsing/formatting, and `net-runner` tests host
   Python against a localhost server. None drives initialization, runtime
   ordering, transport, or watchdog feeding.
7. **USB recovery is also unbounded.** `service_usb_control()` drains
   `Serial.available()` without an input quota, then uses `Serial.flush()` and
   `delay(20)` before bootloader entry.
8. **The optical flashing tool ignores the optical recovery channel.**
   `firmware/scripts/flash_optical.sh` requests software
   reboot only from RawHID (`16c0:0486`). The optical CDC image (`16c0:0483`)
   falls through to a loader wait and invites a manual button press.

The sequence of commits on 2026-07-23 confirms the patch pattern: DHCP
maintenance, DHCP restart, diagnostics, watchdog timing, link-state handling,
HTTP ordering, and listener rebinding changed independently in the same global
state machine. Those changes are observations, not accepted proof.

## Normative recovery contract

**The device must reach normal service or USB-recoverable safe mode without
physical intervention.**

- `setup()` captures retained reset state, arms the watchdog, places audio
  output in its safe state, constructs service objects, and returns. It does not
  probe SD, wait for link/DHCP, wait for USB, or start a peer transaction.
- One runtime epoch invokes mandatory services in the order below. Each call
  receives a quota. Network, USB, HTTP, metrics, UDP, and replay logic return
  without waiting for external progress.
- The Teensy SD library is synchronous and offers no cancellable primitive.
  Firmware therefore invokes at most one SD primitive per epoch. A returned
  call advances the portable state machine; a wedged call is bounded by the
  hardware watchdog and the retained `SD` checkpoint, not by a false
  cooperative timeout claim.
- The watchdog is fed exactly once after the complete epoch. The last checkpoint
  is persisted before each service call.
- A watchdog reset before `EPOCH_COMPLETE` increments the early-reset count.
  Two consecutive early watchdog resets select recovery-safe mode.
- Recovery-safe mode services bounded USB only. Ethernet and SD mutation stay
  disabled and audio output stays safe. Its only exit is a verified new image
  entered through `AIUS BOOTLOADER`; there is no "try normal again" command.
- A new build revision resets the retained early-reset count. Ten seconds of
  completed normal epochs also clears it.
- One network generation owns every DHCP, listener, UDP, and TCP resource.
  Teardown invalidates the generation before physical cleanup, so late events
  cannot publish readiness.
- A DHCP deadline is a firmware fault. It starts bounded teardown and backoff;
  it never selects a static address or implicates the trusted fixture.

### Runtime order and budgets

All millisecond deadlines use
`(int32_t)(now_ms - deadline_ms) >= 0`; every interval is below `INT32_MAX`.
Event input is processed before an equal deadline, so progress exactly at the
deadline wins. Counters saturate at `UINT32_MAX`.

| Checkpoint | Mandatory service | Per-epoch quota | Progress deadline |
|---|---|---|---|
| `0 RESET_CAPTURE` | retained boot | one read + one write | setup: 250 ms |
| `1 USB_RX` | CDC input | 64 bytes/calls | parser state; no wait |
| `2 USB_TX` | CDC output | 64 bytes; one write | bootloader ack: 500 ms |
| `3 METRICS` | due hardware metrics sample | one sample | no wait |
| `4 NETWORK` | supervisor + adapter | one transition/action | DHCP: 9 s; backoff: 1 s |
| `5 HTTP` | one client | one accept; RX 256 B; TX 512 B | HTTP ABI below |
| `6 UDP` | discovery/control | one frame | 2 ms service budget |
| `7 REPLAY` | replay refill | 2,048 bytes | one call |
| `8 SD` | deferred SD job | one transition; one primitive; 512 B | return or 8–10 s reset |
| `9 EPOCH_COMPLETE` | snapshot + watchdog | one snapshot/feed | 8 s watchdog |

Checkpoints `0..8` are injectable recovery checkpoints. `EPOCH_COMPLETE` is the
healthy completion marker immediately before feed and is never fault-injected.

After USB enumeration, `AIUS ID` must respond within 2 s. A normal boot must
obtain its DHCP address within 12 s and serve HTTP and UDP within 12 s. An
induced stall must reset in 8–10 s and return USB status within 2 s of
re-enumeration.

**The 12 s figure is measured, and it replaces a guess.** The original 7 s / 8 s
pair predates any on-board measurement and sits *below* even the fast case, so a
healthy board could not meet it. Measured over twenty-nine software reboots of
`3576c3e3560f`:

- The board's own pre-DHCP work — stack start, `beginNoWait()`, PHY reset and
  autonegotiation, the RX-ready gate — takes **2,165–3,624 ms**.
- The DHCP window that follows is **quantised**, not spread:
  **4,003–4,005 ms on 27 boots** and **8,005 ms on 2**. The step is
  `FNET_CFG_DHCP_CLN_RESPONSE_TIMEOUT_MS` — one lost `DISCOVER` and its
  retransmission. The segment owns those four seconds; asking more often was
  tried on the board and did not move them
  ([`src/aius_network_teensy.h`](../../firmware/src/aius_network_teensy.h)).
- Host observation adds up to one poll interval plus one probe timeout.

So the observable channel latency is bimodal: **7.09–8.49 s** with one `DISCOVER`
and **10.79–11.04 s** with two. 12 s is the worst of those plus ~1 s.

**What the margin is deliberately not for: a third `DISCOVER`.** That boot lands
near 14.8 s and fails this deadline — as it should. At two lost `DISCOVER`s the
board is already within 1 s of `AIUS_NET_DHCP_DEADLINE_MS` (9,000 ms) and about
to take the teardown-and-rebind path itself, so a deadline that absorbed the
third would hide exactly the event it exists to surface.

The 2 s USB budget is untouched: measured 1.51–1.64 s from re-enumeration across
the same boots, comfortably inside it.

## Shared portable I/O ABI

`AiusIoResult` lives in
`firmware/lib/aius_net/include/aius_io.h`. Phase 1 lands this
header before transport, HTTP, SD, or CDC implementation.

```c
typedef enum {
    AIUS_IO_PROGRESS,
    AIUS_IO_WOULD_BLOCK,
    AIUS_IO_CLOSED,
    AIUS_IO_FAULT
} AiusIoResult;

typedef struct {
    AiusIoResult result;
    size_t bytes;
    uint32_t fault;
} AiusIoEvent;
```

`bytes` is zero unless `result == AIUS_IO_PROGRESS`; it may not exceed the
requested buffer length. `fault` is zero except for `AIUS_IO_FAULT`. All state
objects below are caller-owned, initialized by their named init function, and
valid until the caller explicitly resets or discards them. All input buffers
remain caller-owned; output actions borrow them only until the next step call.

## Portable network supervisor ABI

Deepen `aius_net`; do not create another lifecycle module.

```c
typedef enum {
    AIUS_NET_STATE_STACK_START,
    AIUS_NET_STATE_DHCP_WAIT,
    AIUS_NET_STATE_ONLINE,
    AIUS_NET_STATE_TEARDOWN,
    AIUS_NET_STATE_BACKOFF,
    AIUS_NET_STATE_RECOVERY_SAFE
} AiusNetState;

typedef enum {
    AIUS_NET_EVENT_NONE,
    AIUS_NET_EVENT_STACK_STARTED,
    AIUS_NET_EVENT_LEASE_ACQUIRED,
    AIUS_NET_EVENT_RESOURCE_FAULT,
    AIUS_NET_EVENT_TEARDOWN_DONE,
    AIUS_NET_EVENT_TEARDOWN_FAULT
} AiusNetEventKind;

typedef enum {
    AIUS_NET_ACTION_NONE,
    AIUS_NET_ACTION_START_STACK,
    AIUS_NET_ACTION_START_DHCP,
    AIUS_NET_ACTION_TEARDOWN,
    AIUS_NET_ACTION_PUBLISH_ONLINE
} AiusNetActionKind;

typedef enum {
    AIUS_NET_FAULT_NONE = 0,
    AIUS_NET_FAULT_DHCP_TIMEOUT = 1,
    AIUS_NET_FAULT_LINK_LOST = 2,
    AIUS_NET_FAULT_STACK_START = 3,
    AIUS_NET_FAULT_DHCP_START = 4,
    AIUS_NET_FAULT_LISTENER = 5,
    AIUS_NET_FAULT_UDP = 6,
    AIUS_NET_FAULT_SOCKET = 7,
    AIUS_NET_FAULT_TEARDOWN = 8,
    AIUS_NET_FAULT_CALLBACK_OVERFLOW = 9,
    AIUS_NET_FAULT_DRIVER_PROTOCOL = 10
} AiusNetFault;

typedef struct {
    AiusNetEventKind kind;
    uint32_t generation;
    AiusNetFault fault;
} AiusNetEvent;

typedef struct {
    AiusNetActionKind kind;
    uint32_t generation;
} AiusNetAction;

typedef struct {
    AiusNetState state;
    uint32_t generation;
    uint32_t deadline_ms;
    uint32_t restarts;
    uint32_t ignored_events;
    uint32_t cleanup_faults;
    AiusNetFault last_fault;
} AiusNetSupervisor;

void aius_net_supervisor_init(
    AiusNetSupervisor *supervisor,
    uint32_t now_ms,
    bool recovery_safe,
    AiusNetAction *initial_action);

void aius_net_supervisor_step(
    AiusNetSupervisor *supervisor,
    uint32_t now_ms,
    AiusNetEvent event,
    AiusNetAction *action);
```

The deadlines are compile-time constants
`AIUS_NET_DHCP_DEADLINE_MS = 5000` and
`AIUS_NET_BACKOFF_MS = 1000`. Both functions fully initialize their output
action to `NONE` when no action is emitted. Null pointers are rejected by
returning without mutation. `aius_net_supervisor_step()` makes at most one state
transition per call.

- Initialization selects `RECOVERY_SAFE`, or `STACK_START` with generation 1
  and action `START_STACK`.
- Current-generation `STACK_STARTED` moves to `DHCP_WAIT`, sets the deadline,
  and returns `START_DHCP`.
- Current-generation `LEASE_ACQUIRED` in `DHCP_WAIT`, including at exact
  deadline, moves to `ONLINE` and returns `PUBLISH_ONLINE`.
- `RESOURCE_FAULT` in a live-resource state (`STACK_START`, `DHCP_WAIT`, or
  `ONLINE`) and DHCP expiry move to `TEARDOWN`, invalidate resource authority,
  record the saturated fault/restart counters, and return `TEARDOWN` with the
  invalidated generation number for cleanup correlation.
- `TEARDOWN_DONE` or `TEARDOWN_FAULT` moves to `BACKOFF`; cleanup failure is
  recorded but cannot retain authority for the invalidated generation.
- `TEARDOWN` and `BACKOFF` retain the invalidated generation number only for
  cleanup correlation. Resource callbacks in either state are impossible
  events, so they are ignored and counted even when the numeric generation
  matches.
- Backoff expiry increments the generation, skipping zero after wrap, moves to
  `STACK_START`, and returns `START_STACK`. Generation wrap is allowed only
  after teardown has invalidated all resources.
- Events with a stale generation, impossible event/state pairs, and duplicate
  callbacks are ignored and counted.

The exhaustive state/event test table is the Cartesian product of all six
states and six events, plus exact-deadline, `millis()` wrap, generation wrap,
counter saturation, and 10,000 generated event sequences. The oracle asserts
one live generation, at most one action per step, stale-event rejection, and
reachability of `ONLINE`, `BACKOFF`, or `RECOVERY_SAFE`.

## Build stamping

`scripts/build_metadata.py` produces plain build metadata: a compact build
revision (`git describe`-style, exported as `AIUS_BUILD_GIT_REV`) and a short
`AIUS_BUILD_ID` derived from the source revision and target. `build.sh` injects
those plus `AIUS_BUILD_UTC`. USB `ID` and HTTP status emit the build revision and
short build id so a deployed image is identifiable. Rebuilding the same source
and target preserves boot-ledger identity; a source or target change clears
early-reset state. Native metadata tests cover ID derivation, stability across
timestamps, and invalid invocations.

`build.sh` keeps the `teensy41-optical-fault <checkpoint>` test-image target
(`<checkpoint>` is one digit `0..8`, exported as `AIUS_FAULT_CHECKPOINT` to that
one PlatformIO invocation) because the watchdog smoke needs a fault image per
checkpoint. It carries no evidence-receipt, source-snapshot,
environment-fingerprint, or ten-image-matrix machinery. That hash-locked
provenance apparatus is removed per
[`CLAUDE.md`](../../CLAUDE.md), which records it was deleted on
2026-07-18 and must not return for this personal build. Final ELF/HEX hashes
remain the authority for produced bytes; cross-host reproducibility is not
claimed.

## Portable boot-ledger ABI

`aius_boot` is independent of Teensy registers and retained storage.

`AIUS_BOOT_MAGIC` is numeric `0x41495542`. Ledger CRC is
CRC-32/ISO-HDLC over exactly 24 bytes: every field before `crc32`, serialized
in declaration order and little-endian for 16/32-bit values. Validity requires
that CRC, magic, version 1, `last_checkpoint` in `0..9`, mode in `0..1`, and
zero reserved bits. Mutation functions are no-ops for null or invalid ledgers;
checkpoint values outside `0..9` are also no-ops. They never recompute a CRC
that would bless corrupted retained state.

```c
typedef enum {
    AIUS_BOOT_MODE_NORMAL = 0,
    AIUS_BOOT_MODE_RECOVERY_SAFE = 1
} AiusBootMode;

typedef enum {
    AIUS_RESET_COLD,
    AIUS_RESET_SOFTWARE,
    AIUS_RESET_WATCHDOG,
    AIUS_RESET_OTHER
} AiusResetKind;

typedef struct {
    uint32_t magic;                 /* "AIUB" */
    uint16_t version;               /* 1 */
    uint16_t last_checkpoint;
    uint32_t build_id;
    uint32_t boot_generation;
    uint32_t reset_source;
    uint8_t early_watchdog_resets;
    uint8_t mode;
    uint16_t reserved;
    uint32_t crc32;
} AiusBootLedger;

typedef struct {
    uint32_t build_id;
    uint32_t raw_reset_source;
    AiusResetKind reset_kind;
} AiusBootInput;

void aius_boot_init(
    AiusBootLedger *ledger,
    const AiusBootLedger *retained,
    AiusBootInput input);

void aius_boot_record_checkpoint(
    AiusBootLedger *ledger,
    uint16_t checkpoint);

void aius_boot_mark_healthy(
    AiusBootLedger *ledger);

bool aius_boot_is_valid(
    const AiusBootLedger *ledger);
```

- Invalid magic/version/CRC creates a normal ledger with generation 1.
- Every valid boot saturating-increments `boot_generation`. A new build ID
  independently clears early resets and selects normal mode.
- A watchdog reset with last checkpoint other than `EPOCH_COMPLETE` saturating-
  increments the early count. Count 2 selects recovery-safe mode.
- A non-watchdog reset preserves diagnostics but does not increment the count.
- Ten seconds of completed epochs clears the early count in normal mode.
- `last_checkpoint` is written before each service. `EPOCH_COMPLETE` is written
  immediately before the only watchdog feed.
- The Teensy adapter alone maps raw `SRC_SRSR` bits to `AiusResetKind`; portable
  tests supply the classification explicitly through `AiusBootInput`.
- Target retained storage must prove read-after-watchdog-reset retention and
  CRC rejection. Until that target proof passes, its values are diagnostic only.

## Teensy retained watchdog ABI

The target uses two 32-byte-aligned slots in Teensy 4.1 OCRAM
`.dmabuffers` (`NOLOAD`), below the core-reserved CrashReport area at
`0x2027ff80`. DTCM is rejected because startup clears it. Flash/EEPROM is
rejected because a checkpoint write occurs every epoch.

```c
#define AIUS_BOOT_SLOT_COMMITTED 0x41535542u

typedef struct __attribute__((aligned(32))) {
    uint32_t committed;
    uint32_t sequence;
    AiusBootLedger ledger;
    uint32_t slot_crc32;
    uint8_t padding[24];
} AiusBootSlot;

#ifdef __cplusplus
static_assert(sizeof(AiusBootSlot) == 64, "retained slot size");
static_assert(alignof(AiusBootSlot) == 32, "retained slot alignment");
#else
_Static_assert(sizeof(AiusBootSlot) == 64, "retained slot size");
_Static_assert(_Alignof(AiusBootSlot) == 32, "retained slot alignment");
#endif

typedef struct {
    void *context;
    bool (*read_slot)(void *context, uint8_t slot, AiusBootSlot *out);
    bool (*write_slot_bytes)(void *context, uint8_t slot, size_t offset,
                             const uint8_t *data, size_t length);
    void (*flush_slot_range)(void *context, uint8_t slot, size_t offset,
                             size_t length);
    void (*barrier)(void *context);
    uint32_t (*read_reset_source)(void *context);
    void (*clear_reset_source)(void *context, uint32_t captured);
    void (*enable_watchdog_clock)(void *context);
    void (*write_watchdog_wmcr)(void *context, uint16_t value);
    void (*write_watchdog_wcr)(void *context, uint16_t value);
    void (*write_watchdog_wsr)(void *context, uint16_t value);
} AiusBootTeensyPrimitives;

typedef struct {
    AiusBootTeensyPrimitives primitives;
    uint8_t active_slot;
    uint32_t sequence;
    bool valid;
} AiusBootStorageTeensy;

void aius_boot_storage_teensy_init(
    AiusBootStorageTeensy *storage,
    const AiusBootTeensyPrimitives *primitives);

bool aius_boot_storage_teensy_load(
    AiusBootStorageTeensy *storage,
    AiusBootLedger *ledger);

bool aius_boot_storage_teensy_commit(
    AiusBootStorageTeensy *storage,
    const AiusBootLedger *ledger);

uint32_t aius_boot_teensy_reset_source(
    const AiusBootTeensyPrimitives *primitives);
AiusResetKind aius_boot_teensy_reset_kind(uint32_t raw_reset_source);
bool aius_boot_teensy_watchdog_arm(
    const AiusBootTeensyPrimitives *primitives);
bool aius_boot_teensy_watchdog_feed(
    const AiusBootTeensyPrimitives *primitives);
AiusBootTeensyPrimitives aius_boot_teensy_hardware_primitives(void);
```

`g_aius_boot_slots[2]` is the only retained object and is declared `DMAMEM` and
`used`. Each 64-byte element begins on its own 32-byte cache-line boundary.
Padding is zeroed on commit but excluded from CRC. Slot CRC covers exactly the
little-endian bytes of `sequence` followed by the 28-byte ledger. Load validates
committed marker, slot CRC, then ledger CRC, and selects the newest valid
sequence using wrap-safe comparison. Commit invalidates the inactive slot,
writes sequence/ledger/zero padding/CRC, flushes both cache lines, executes
`dsb`, writes the committed marker last, flushes its cache line, executes
`dsb`, then changes the active index. A reset between any writes leaves the
previous slot authoritative. With no valid slot, portable boot initialization
receives no retained ledger.

The public primitive seam is the only storage/register/cache authority and is
used unchanged by the hardware and fake adapters. Initialization copies a
complete non-null primitive table; null storage is a no-op, while null or
incomplete primitives leave `valid == false`, `active_slot == UINT8_MAX`, and
`sequence == 0`. Load zeroes a non-null output before work and returns false
for null/invalid storage or output. Commit returns false for null/invalid
storage or ledger, including a ledger rejected by `aius_boot_is_valid`.
With no valid slot, the first commit targets slot 0 with sequence 1. Otherwise
it targets the inactive slot with selected sequence plus one, including
`UINT32_MAX → 0`.

A sequence is newer only when `(int32_t)(left - right) > 0`. Equality and the
exact half-range difference `0x80000000` select the lower slot index. Each
`write_slot_bytes` call may report any committed prefix and then fail; the
adapter stops immediately and does not publish the new active slot. The exact
commit calls are marker invalidation, sequence, ledger, zero padding, slot CRC,
two-line flush, barrier, committed marker, first-line flush, barrier. The fake
oracle injects reset after every call and every byte prefix of every write.

Reset classification is priority ordered: `SRC_SRSR_WDOG_RST_B` is
`WATCHDOG`; `SRC_SRSR_IPP_RESET_B` or `SRC_SRSR_CSU_RESET_B` is `COLD`;
`SRC_SRSR_JTAG_SW_RST` or `SRC_SRSR_LOCKUP_SYSRESETREQ` is `SOFTWARE`; all
remaining combinations are `OTHER`. Raw bits remain in telemetry. Tests cover
each bit and combination, watchdog priority, sequence wrap, every torn-write
boundary, corrupt slot/ledger CRC, and one valid/one invalid slot.

Reset capture reads the complete 32-bit `SRC_SRSR` exactly once, returns it
without masking, then acknowledges exactly the captured value through one
write-one-to-clear call. Watchdog arm enables WDOG1 clock, writes `WMCR=0`,
then writes one exact WCR value
`WDOG_WCR_SRS | WDOG_WCR_WT(15) | WDOG_WCR_WDE | WDOG_WCR_WDT |
WDOG_WCR_SRE`, selecting the pinned 8-second interval. Feed writes `0x5555`
then `0xAAAA` exactly once each. Null/incomplete hardware primitives return
false before any write.

The source `g_aius_boot_slots` object uses the `DMAMEM` input section
`.dmabuffers`; the pinned Teensy 4.1 linker script maps that input into linked
output section `.bss.dma`. The linked symbol must be 32-byte aligned, occupy
exactly 128 bytes in `.bss.dma`, and satisfy
`address + 128 <= 0x2027ff80`. A post-link check fails every production or
fault build otherwise; this is the compile-time guard against overlap with the
pinned CrashReport range `0x2027ff80..0x2027ffff`.

The fault image defines exactly one build flag,
`AIUS_FAULT_STALL_CHECKPOINT=0..8`. Immediately after persisting the matching
checkpoint, `aius_fault_checkpoint(uint16_t)` enters an intentional tight loop
in `aius_fault_teensy.cpp`; the production build compiles the function to a
return. This test-only file is the sole exception to the unbounded-loop source
guard. The Phase 16 live smoke builds a fault image per checkpoint `0..8`.

## Bounded target transport ABI

Only `aius_network_teensy.cpp` may include NativeEthernet/FNET headers. The
public header contains no vendor type:

```c
typedef struct {
    uint32_t generation;
    bool stack_started;
    bool dhcp_started;
    bool online;
    bool listener_open;
    bool udp_open;
    AiusNetEvent pending[4];
    uint8_t pending_read;
    uint8_t pending_count;
    bool callback_overflow;
    AiusNetAction active_action;
    uint8_t action_step;
    bool action_fault;
    bool lease_acquired;       /* added in Phase 14 R2; generation-scoped */
    uint32_t dhcp_updates;     /* added in Phase 14 R2; monotonic since init */
} AiusNetworkTeensy;

typedef enum {
    AIUS_NETWORK_TEENSY_LINK_UNKNOWN = 0,
    AIUS_NETWORK_TEENSY_LINK_UP = 1,
    AIUS_NETWORK_TEENSY_LINK_DOWN = 2
} AiusNetworkTeensyLink;

AiusNetworkTeensyLink aius_network_teensy_link(
    const AiusNetworkTeensy *adapter);

bool aius_network_teensy_address(
    const AiusNetworkTeensy *adapter,
    uint8_t address[4]);

bool aius_network_teensy_dhcp_pending(
    const AiusNetworkTeensy *adapter);

void aius_network_teensy_init(
    AiusNetworkTeensy *adapter);

void aius_network_teensy_apply(
    AiusNetworkTeensy *adapter,
    uint32_t now_ms,
    AiusNetAction action,
    AiusNetEvent *event);

void aius_network_teensy_poll(
    AiusNetworkTeensy *adapter,
    uint32_t now_ms,
    AiusNetEvent *event);

AiusIoEvent aius_network_teensy_accept(
    AiusNetworkTeensy *adapter);

AiusIoEvent aius_network_teensy_read_once(
    AiusNetworkTeensy *adapter,
    uint8_t *buffer,
    size_t capacity);

AiusIoEvent aius_network_teensy_write_once(
    AiusNetworkTeensy *adapter,
    const uint8_t *buffer,
    size_t length);

AiusIoEvent aius_network_teensy_close_once(
    AiusNetworkTeensy *adapter);

AiusIoEvent aius_network_teensy_udp_receive_once(
    AiusNetworkTeensy *adapter,
    uint8_t *buffer,
    size_t capacity);

AiusIoEvent aius_network_teensy_udp_send_once(
    AiusNetworkTeensy *adapter,
    const uint8_t *buffer,
    size_t length);
```

The three status accessors are read-only and carry no vendor type. Each performs
exactly one underlying read, and only while `stack_started` — a null, unwired or
stopped adapter returns its not-available answer (`UNKNOWN`, or `false` with
`0.0.0.0` written) without touching a primitive at all, so pre-start and
post-teardown are safe by construction rather than by the vendor tolerating a dead
stack. `AiusNetworkTeensyLink`'s values are pinned to the NativeEthernet
`EthernetLinkStatus` ordering so the numeric `link` field in status JSON, USB `ID`
and `swagger.json` keeps its meaning across the cutover; an out-of-contract driver
value degrades to `UNKNOWN`. The address read is a compile-time seam rather than an
`AiusNetworkTeensyPrimitives` entry, because that table is the lifecycle and
socket-I/O failure-injection seam and a status read needs no failure injection —
and because appending to it would break the positional aggregate initializer in
`test/runtime-target/`. Moving it into the table later is a coordinated edit.

`aius_network_teensy_apply()` fully initializes `event` to `NONE`, accepts or
advances at most one action, and emits an event only for that action's
generation. `START_STACK` and `START_DHCP` complete in one call.
`PUBLISH_ONLINE` is retained in `active_action`: one call opens the listener,
the next `apply(NONE)` call opens UDP, and only then does `online` become true.
An operation failure clears the active action and emits a current-generation
`RESOURCE_FAULT` for the failed resource. Reapplying the identical active
action is equivalent to `NONE`; a different non-teardown action is a
`DRIVER_PROTOCOL` resource fault. A current `TEARDOWN` action may preempt any
active action.

Teardown invalidates callbacks and the FIFO before its first target operation,
then retains the teardown action while successive calls close the listener,
UDP, and stack in that order. Each call attempts at most one still-live
resource. It records failures but continues cleanup; only after every resource
has been attempted does it emit one `TEARDOWN_DONE` or `TEARDOWN_FAULT`.
Intermediate calls return `NONE`. Repeated teardown after all flags are clear
is idempotent and emits `TEARDOWN_DONE` without a target call.

The adapter structure holds authoritative lifecycle and active-action flags;
vendor objects are file-static implementation details. The I/O functions
borrow caller buffers only for their call. Every call performs at most one
underlying state-changing or socket I/O operation and returns. `write_once()`
writes no more than current capacity and never waits. `close_once()` closes
and clears the sole accepted client and is idempotent. Data calls return
`PROGRESS` only with `1..capacity` bytes; zero capacity returns `WOULD_BLOCK`
without a primitive call.

FNET does not use the POSIX receive convention, and the mapping follows FNET's
rather than POSIX's. **A zero-byte FNET receive on a connected socket is
`WOULD_BLOCK`, not `CLOSED`** — `_fnet_tcp_rcv()` falls through to
`return (fnet_int32_t)len;` with `len == 0` whenever the receive buffer is empty
(`stack/fnet_tcp.c:776`), which is the ordinary state between two segments of one
request. **Peer closure surfaces instead as `FNET_ERR` with `fnet_error_get()`
returning `FNET_ERR_CONNCLOSED`** (FIN received, buffer drained; `fnet_tcp.c:768-772`)
**or `FNET_ERR_NOTCONN`** (socket has left `SS_CONNECTED`; `754-757`), and both map
to `CLOSED`. FNET `AGAIN` is `WOULD_BLOCK`, though this receive path never
produces it. Every other negative result or NativeEthernet write error is
`FAULT` with `AIUS_NET_FAULT_SOCKET`. Non-fault results have zero `fault`, and
`FAULT` has zero bytes and a nonzero fault.

NativeEthernet link/DHCP callbacks append `{kind, generation, fault}` to the
four-entry FIFO without performing lifecycle work. `poll()` fully initializes
`event` to `NONE` and returns at most one queued event. It discards and counts
events whose captured generation is no longer current. Duplicate link-up or
lease callbacks are delivered once and then rejected by the supervisor as
impossible duplicates. FIFO overflow clears the queue and emits one
current-generation `RESOURCE_FAULT` with `AIUS_NET_FAULT_CALLBACK_OVERFLOW`;
teardown invalidation clears all callbacks and queued events before cleanup.
`now_ms` is reserved for target health polling; one link-status read may emit a
current-generation `RESOURCE_FAULT`, but it may not start, stop, or wait for
DHCP.

The repository-pinned NativeEthernet snapshot is the MIT-licensed source from
`framework-arduinoteensy 1.162.0`. Its `NOTICE.md` records that version and an
Aius patch list. A checked-in manifest records the upstream SHA-256 for every
snapshotted file and the patched SHA-256 for every changed file. Required
patches are:

- `Ethernet.beginNoWait()` initializes stack/callbacks once and returns without
  waiting for link or DHCP;
- public DHCP start/stop operations keep FNET descriptor ownership inside the
  library;
- `EthernetClient::write()` makes one send attempt and returns zero on no
  capacity;
- `EthernetServer::begin()` is idempotent and `end()` unregisters its service
  and closes its listener;
- `Ethernet.end()` invalidates user callbacks, unregisters server and link
  services, then closes owned sockets and DHCP without a wait;
- a failed initialization step leaves a state that `end()` can clean and a
  later `beginNoWait()` can retry without reinitializing an enabled FNET stack.

The source guard rejects unbounded loops in these target-facing functions. It
also rejects NativeEthernet/FNET includes outside the pinned library and the
single designated adapter owner,
`firmware/src/aius_network_teensy.cpp`. One temporary
exact-path exception permits the pre-cutover
`firmware/src/optical_main.cpp`. The guard test proves its
rejection logic against synthetic forbidden application input and fails if any
additional owner or exception appears. Phase 14 removes the legacy includes
and exception in the same commit, after which the adapter is the only
application-level NativeEthernet/FNET owner.

The native transport suite compiles the patched C++ against fake Arduino/FNET
primitives. It injects failure at each initialization step and proves retry,
idempotent ordered teardown, late-callback rejection, DHCP descriptor
ownership, server service ownership, and zero-capacity/single-send write
semantics. Text guards supplement this behavioral harness; they are not the
behavior oracle. The suite also validates the immutable snapshot manifest and
the PlatformIO resolved-library metadata plus patched symbols from an optical
build.

## Bounded SD job ABI

Move the SD job state machine out of `optical_main.cpp` into `aius_sd`. The
portable module owns transaction state; a Teensy driver supplies one-shot file
operations.

```c
#define AIUS_SD_PATH_STORAGE 128u

typedef enum {
    AIUS_SD_JOB_PROBE,
    AIUS_SD_JOB_CONFIG_LOAD,
    AIUS_SD_JOB_USAGE_REFRESH,
    AIUS_SD_JOB_UPLOAD,
    AIUS_SD_JOB_DOWNLOAD,
    AIUS_SD_JOB_DELETE,
    AIUS_SD_JOB_REPLAY_REFILL
} AiusSdJobKind;

typedef enum {
    AIUS_SD_INSTALL_REPLACE,
    AIUS_SD_INSTALL_APPEND_PART,
    AIUS_SD_INSTALL_FINALIZE_PART
} AiusSdInstallMode;

typedef enum {
    AIUS_SD_IDLE,
    AIUS_SD_PREPARE,
    AIUS_SD_MKDIR,
    AIUS_SD_OPEN,
    AIUS_SD_NEED_INPUT,
    AIUS_SD_READ,
    AIUS_SD_WRITE,
    AIUS_SD_CHECK_TARGET,
    AIUS_SD_BACKUP,
    AIUS_SD_INSTALL,
    AIUS_SD_ROLLBACK,
    AIUS_SD_CLEANUP,
    AIUS_SD_PUBLISH,
    AIUS_SD_CANCEL_CLOSE,
    AIUS_SD_CANCEL_CLEANUP,
    AIUS_SD_DONE,
    AIUS_SD_FAILED
} AiusSdState;

typedef enum {
    AIUS_SD_PHASE_NONE,
    AIUS_SD_PHASE_BEGIN,
    AIUS_SD_PHASE_PROBE_MEDIA,
    AIUS_SD_PHASE_CONFIG_OPEN,
    AIUS_SD_PHASE_CONFIG_SIZE,
    AIUS_SD_PHASE_CONFIG_READ,
    AIUS_SD_PHASE_CONFIG_CLOSE,
    AIUS_SD_PHASE_CONFIG_PUBLISH,
    AIUS_SD_PHASE_USAGE_TOTAL,
    AIUS_SD_PHASE_USAGE_USED,
    AIUS_SD_PHASE_PATH_EXISTS,
    AIUS_SD_PHASE_MKDIR,
    AIUS_SD_PHASE_PART_SIZE_OPEN,
    AIUS_SD_PHASE_PART_SIZE,
    AIUS_SD_PHASE_PART_SIZE_CLOSE,
    AIUS_SD_PHASE_UPLOAD_OPEN,
    AIUS_SD_PHASE_UPLOAD_TRUNCATE,
    AIUS_SD_PHASE_UPLOAD_INPUT,
    AIUS_SD_PHASE_UPLOAD_WRITE,
    AIUS_SD_PHASE_UPLOAD_CLOSE,
    AIUS_SD_PHASE_FINAL_SIZE_OPEN,
    AIUS_SD_PHASE_FINAL_SIZE,
    AIUS_SD_PHASE_FINAL_SIZE_CLOSE,
    AIUS_SD_PHASE_DOWNLOAD_OPEN,
    AIUS_SD_PHASE_DOWNLOAD_SIZE,
    AIUS_SD_PHASE_DOWNLOAD_SEEK,
    AIUS_SD_PHASE_DOWNLOAD_READ,
    AIUS_SD_PHASE_DOWNLOAD_OUTPUT,
    AIUS_SD_PHASE_DOWNLOAD_CLOSE,
    AIUS_SD_PHASE_DELETE_REMOVE,
    AIUS_SD_PHASE_INSTALL_REMOVE_OLD_BACKUP,
    AIUS_SD_PHASE_INSTALL_BACKUP,
    AIUS_SD_PHASE_INSTALL_RENAME,
    AIUS_SD_PHASE_INSTALL_REMOVE_BACKUP,
    AIUS_SD_PHASE_INSTALL_PUBLISH_OPEN,
    AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE,
    AIUS_SD_PHASE_INSTALL_PUBLISH_READ,
    AIUS_SD_PHASE_INSTALL_PUBLISH_CLOSE,
    AIUS_SD_PHASE_INSTALL_PUBLISH,
    AIUS_SD_PHASE_ROLLBACK,
    AIUS_SD_PHASE_CLEANUP_CLOSE,
    AIUS_SD_PHASE_CLEANUP_TEMP,
    AIUS_SD_PHASE_TERMINAL
} AiusSdPhase;

typedef enum {
    AIUS_SD_EXISTS_NONE,
    AIUS_SD_EXISTS_MKDIR,
    AIUS_SD_EXISTS_DELETE,
    AIUS_SD_EXISTS_INSTALL_TARGET,
    AIUS_SD_EXISTS_REMOVE_OLD_BACKUP,
    AIUS_SD_EXISTS_REMOVE_COMMITTED_BACKUP,
    AIUS_SD_EXISTS_CLEANUP_TEMP
} AiusSdExistencePurpose;

typedef enum {
    AIUS_SD_EVENT_NONE,
    AIUS_SD_EVENT_OK,
    AIUS_SD_EVENT_BYTES,
    AIUS_SD_EVENT_BOOL,
    AIUS_SD_EVENT_SIZE,
    AIUS_SD_EVENT_FAULT
} AiusSdEventKind;

typedef struct {
    AiusSdEventKind kind;
    uint64_t action_id;
    size_t bytes;
    uint64_t size;
    bool value;
    uint32_t fault;
} AiusSdEvent;

typedef enum {
    AIUS_SD_ACTION_NONE,
    AIUS_SD_ACTION_BEGIN,
    AIUS_SD_ACTION_MEDIA_PRESENT,
    AIUS_SD_ACTION_MKDIR,
    AIUS_SD_ACTION_OPEN_READ,
    AIUS_SD_ACTION_OPEN_WRITE_BEGIN,
    AIUS_SD_ACTION_OPEN_APPEND,
    AIUS_SD_ACTION_TRUNCATE,
    AIUS_SD_ACTION_SEEK,
    AIUS_SD_ACTION_NEED_INPUT,
    AIUS_SD_ACTION_READ,
    AIUS_SD_ACTION_WRITE,
    AIUS_SD_ACTION_OUTPUT,
    AIUS_SD_ACTION_FILE_SIZE,
    AIUS_SD_ACTION_EXISTS,
    AIUS_SD_ACTION_RENAME,
    AIUS_SD_ACTION_REMOVE,
    AIUS_SD_ACTION_CLOSE,
    AIUS_SD_ACTION_TOTAL_SIZE,
    AIUS_SD_ACTION_USED_SIZE,
    AIUS_SD_ACTION_PUBLISH_CONFIG,
    AIUS_SD_ACTION_COMPLETE,
    AIUS_SD_ACTION_FAIL
} AiusSdActionKind;

typedef struct {
    AiusSdActionKind kind;
    uint64_t action_id;
    const char *path;
    const char *path_to;
    uint8_t *buffer;
    size_t length;
    uint64_t offset;
    uint32_t fault;
} AiusSdAction;

typedef struct {
    AiusSdJobKind kind;
    AiusSdInstallMode install_mode;
    const char *target_path;
    const char *temp_path;
    const char *backup_path;
    const char *content_type;
    uint64_t total;
    uint64_t initial_offset;
    bool publish;              /* generalized in Phase 10a; was publish_config */
    size_t publish_capacity;   /* generalized in Phase 10a; caller-supplied cap
                                 * (was the hardcoded AIUS_CONFIG_MAX_JSON_BYTES) */
    bool require_existing;
} AiusSdJobRequest;

typedef enum {
    AIUS_SD_RESULT_NONE,
    AIUS_SD_RESULT_PROBE,
    AIUS_SD_RESULT_USAGE,
    AIUS_SD_RESULT_BYTES,
    AIUS_SD_RESULT_COMPLETE,
    AIUS_SD_RESULT_FAULT
} AiusSdResultKind;

typedef enum {
    AIUS_SD_FAULT_NONE = 0,
    AIUS_SD_FAULT_INVALID_REQUEST = 1,
    AIUS_SD_FAULT_PROTOCOL = 2,
    AIUS_SD_FAULT_CAPACITY = 3,
    AIUS_SD_FAULT_PATH = 4,
    AIUS_SD_FAULT_OVERFLOW = 5,
    AIUS_SD_FAULT_TIMEOUT = 6,
    AIUS_SD_FAULT_SHORT_IO = 7,
    AIUS_SD_FAULT_MEDIA = 8,
    AIUS_SD_FAULT_OPEN = 9,
    AIUS_SD_FAULT_SEEK = 10,
    AIUS_SD_FAULT_IO = 11,
    AIUS_SD_FAULT_INSTALL = 12,
    AIUS_SD_FAULT_ROLLBACK = 13,
    AIUS_SD_FAULT_PUBLISH = 14,
    AIUS_SD_FAULT_CANCELLED = 15,
    AIUS_SD_FAULT_CLEANUP = 16,
    AIUS_SD_FAULT_RANGE = 17,
    AIUS_SD_FAULT_NOT_FOUND = 18
} AiusSdFault;

typedef struct {
    AiusSdResultKind kind;
    bool media_present;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t transferred_bytes;
    AiusSdFault fault;
    uint32_t driver_fault;
} AiusSdResult;

typedef struct {
    AiusSdState state;
    AiusSdPhase phase;
    AiusSdJobKind kind;
    AiusSdInstallMode install_mode;
    AiusSdExistencePurpose existence_purpose;
    char target_path[AIUS_SD_PATH_STORAGE];
    char temp_path[AIUS_SD_PATH_STORAGE];
    char backup_path[AIUS_SD_PATH_STORAGE];
    char directory_path[AIUS_SD_PATH_STORAGE];
    const char *content_type;
    uint8_t *buffer;
    size_t capacity;
    size_t buffer_offset;
    uint64_t initial_offset;
    uint64_t end_offset;
    uint64_t captured_size;
    uint64_t transferred;
    uint64_t total;
    size_t config_bytes;
    size_t requested;
    size_t pending;
    uint32_t deadline_ms;
    uint64_t next_action_id;
    uint64_t outstanding_action_id;
    AiusSdActionKind outstanding_action;
    uint8_t mkdir_path_index;
    uint8_t mkdir_offset;
    AiusSdFault last_fault;
    uint32_t last_driver_fault;
    AiusSdResult result;
    bool target_existed;
    bool backup_moved;
    bool install_failed;
    bool installed;
    bool file_open;
    bool publish;
    size_t publish_capacity;
    bool require_existing;
    bool close_emitted;
    bool cancel_requested;
    bool abort_requested;
    bool action_ids_exhausted;
    bool terminal_emitted;
} AiusSdJob;

void aius_sd_job_init(
    AiusSdJob *job,
    uint8_t *buffer,
    size_t capacity);

void aius_sd_job_start(
    AiusSdJob *job,
    const AiusSdJobRequest *request,
    uint32_t now_ms,
    uint32_t deadline_ms);

void aius_sd_job_cancel(
    AiusSdJob *job);

bool aius_sd_job_supply(
    AiusSdJob *job,
    const uint8_t *data,
    size_t length);

void aius_sd_job_step(
    AiusSdJob *job,
    uint32_t now_ms,
    AiusSdEvent event,
    AiusSdAction *action);

const AiusSdResult *aius_sd_job_result(
    const AiusSdJob *job);
```

Pointer behavior is total: `init(NULL)`, `start(NULL, ...)`, and `cancel(NULL)`
are no-ops; `supply(NULL, ...)` returns false; `result(NULL)` returns null.
`start(valid_job, NULL, ...)` records `INVALID_REQUEST`. `step()` initializes a
non-null action to `NONE` before any validation; a null job then returns, while
a null action for a live job records `PROTOCOL` and follows authority-safe
cleanup. `init()` may store an invalid buffer/capacity, but the next start
rejects it with `CAPACITY`. An otherwise valid authority-free start rejects
with terminal `OVERFLOW` and emits no correlated action when
`next_action_id > UINT64_MAX - 262` or IDs are exhausted. Equality is accepted:
`BEGIN` may consume `UINT64_MAX - 262`; after its `OK`, the next non-reserved
action detects the boundary and fails before any path mutation or file
authority.

Phase 5 adds an unused portable module. It does not move or claim the
synchronous `optical_main.cpp` SD calls; the Teensy adapter and runtime cutover
remain Phases 10 and 14.

### Portable SD job protocol

The caller owns a non-null 1–512 byte transfer buffer. `CONFIG_LOAD` additionally
requires capacity at least `publish_capacity` (the caller-supplied limit for the
file being loaded and published — see [§SD job ABI generalization](#phase-10a--sd-job-publish-capacity-generalization);
the feature-flag config caller passes `AIUS_CONFIG_MAX_JSON_BYTES` = 256).
`start()` is
valid only from `IDLE`, or from `DONE`/`FAILED` after `terminal_emitted`; it
clears the previous result, validates the complete request, copies every
required path, and selects `PREPARE`. A rejected start selects `FAILED` with
`INVALID_REQUEST`, `CAPACITY`, `PATH`, or `OVERFLOW` and later emits one
`FAIL`. `content_type` is an application-owned constant whose lifetime covers
the job.

Every accepted job first emits `BEGIN`. The application acknowledges it with
`OK`; `FAULT` fails the job directly without close, rollback, existence,
remove, or any other cleanup action because per-job storage initialization
never succeeded. After that common edge, the normative per-kind sequence is:

| Job/mode | Required sequence and completion result |
|---|---|
| `PROBE` | `MEDIA_PRESENT → BOOL`, then `COMPLETE`; result is `PROBE` with the returned presence. `false` is a successful absent-media result. |
| `CONFIG_LOAD` | `OPEN_READ(target) → FILE_SIZE → READ* → CLOSE → PUBLISH_CONFIG → COMPLETE`. Target is required. Size must be `1..publish_capacity` and fit the caller buffer. Positive partial reads accumulate contiguously until the declared size. `PUBLISH_CONFIG` borrows the complete buffer until its `OK`/`FAULT` event. Missing/open failure, empty input, oversize input, or early zero bytes fail after one close if opened. Result is `BYTES` with the published byte count. |
| `USAGE_REFRESH` | `TOTAL_SIZE → SIZE`, then `USED_SIZE → SIZE`, then `COMPLETE`. Result is `USAGE` with both 64-bit values; `used > total` is `PROTOCOL`. |
| `UPLOAD/REPLACE` | For the target, temp, and backup parent prefixes in that order, emit `EXISTS(parent) → BOOL` and emit `MKDIR(parent) → OK` only when false. Then `OPEN_WRITE_BEGIN(temp) → TRUNCATE(0)`, accept and write exactly `total` new bytes, `CLOSE`, and run the install sequence below. Zero-length replacement is valid only when `publish == false` and still opens, truncates, closes, and installs an empty file. |
| `UPLOAD/APPEND_PART` | For each target parent prefix, emit `EXISTS(parent) → BOOL` and `MKDIR(parent) → OK` only when false; `OPEN_READ(target) → FILE_SIZE → CLOSE` and require exact size `initial_offset`; `OPEN_APPEND(target)`; accept and write exactly `total`; `CLOSE`; `COMPLETE`. `target` is the durable part path. It is never promoted, renamed, or removed by failure/cancellation. |
| `UPLOAD/FINALIZE_PART` | For the target, temp, and backup parent prefixes in that order, emit `EXISTS(parent) → BOOL` and `MKDIR(parent) → OK` only when false; `OPEN_READ(temp) → FILE_SIZE → CLOSE` and require exact size `initial_offset`; `OPEN_APPEND(temp)`; accept and write exactly `total`; `CLOSE`; reopen/read size and require `initial_offset + total`; close; then run the install sequence. `temp` is the durable part and is preserved on pre-install failure or cancellation. Publication is valid only when the final `initial_offset + total` is within `publish_capacity`. |
| `DOWNLOAD` | `OPEN_READ(target) → FILE_SIZE → SEEK(initial_offset) → READ* → OUTPUT* → CLOSE → COMPLETE`. After size capture, `initial_offset > captured_size` is `RANGE`. If `total == 0`, set `end_offset = captured_size`; otherwise require checked `initial_offset + total <= captured_size` or fail `RANGE`, then set that sum as `end_offset`. Result is `BYTES` with acknowledged output bytes. |
| `REPLAY_REFILL` | The same open/size/seek/read/output/close sequence, but `total` is a required positive requested maximum. After size capture, `initial_offset > captured_size` is `RANGE`; otherwise set `end_offset = initial_offset + min(total, captured_size - initial_offset)`. `initial_offset == captured_size` is a successful zero-byte refill. |
| `DELETE` | `EXISTS(target) → BOOL`; true requires `REMOVE(target) → OK` before `COMPLETE`. False completes when `require_existing == false`, or fails with `NOT_FOUND` and no mutation when `require_existing == true`. |

Request fields are not implicit:

| Kind/mode | `total` | `initial_offset` | `publish`/`publish_capacity` / `content_type` |
|---|---|---|---|
| `PROBE`, `USAGE_REFRESH`, `DELETE` | must be zero | must be zero | false / zero / null |
| `CONFIG_LOAD` | must be zero | must be zero | true / required nonzero / required non-null constant |
| `DOWNLOAD` | zero means through captured EOF; positive is exact length | `0..INT64_MAX` | false / zero / required non-null constant |
| `REPLAY_REFILL` | required `1..INT64_MAX` maximum | `0..INT64_MAX` | false / zero / null |
| `UPLOAD/REPLACE` | `0..INT64_MAX`; zero is valid only without publication | must be zero | optional; if true, `publish_capacity` required nonzero, total must be `1..publish_capacity`, job capacity at least `publish_capacity`, and content type non-null |
| `UPLOAD/APPEND_PART` | `0..INT64_MAX`; zero is valid | `0..INT64_MAX` | false / zero / null |
| `UPLOAD/FINALIZE_PART` | `0..INT64_MAX`; zero is valid | `0..INT64_MAX` | optional; if true, `publish_capacity` required nonzero, checked final size must be `1..publish_capacity`, job capacity at least `publish_capacity`, and content type non-null |

Non-upload jobs require `install_mode == REPLACE`; any other value is
`INVALID_REQUEST`. A non-null content type where the table requires null is
also invalid. `require_existing` is valid only for `DELETE`; every other job
requires false. It exists so an HTTP delete can preserve its missing-file 404
without a target-side preflight or a time-of-check/time-of-use gap. Upload
request paths are exact:

- `REPLACE` requires distinct target, disposable temp, and backup paths.
- `APPEND_PART` requires only target; temp and backup must be null.
- `FINALIZE_PART` requires distinct final target, durable temp/part, and
  backup paths.
- `PROBE` and `USAGE_REFRESH` forbid every path. `CONFIG_LOAD`, `DOWNLOAD`,
  `DELETE`, and `REPLAY_REFILL` require only target.

Each path is an absolute canonical path of 1–127 bytes plus NUL. `/`, a
trailing slash, empty segments, `.` or `..` segments, backslash, control bytes,
and non-absolute paths are rejected. Required paths must be pairwise distinct.
All emitted path pointers borrow immutable job-owned copies. `directory_path`
holds one NUL-terminated proper parent prefix at a time. Parent preparation
walks from the first segment through the final parent and never emits `/`.
Each prefix first uses `EXISTS`; true advances to the next prefix without a
mutation, while false emits exactly one `MKDIR`.

`initial_offset + total` must not overflow `uint64_t`. Action offsets and file
sizes are 64-bit. The portable module accepts values through `INT64_MAX`;
larger request or driver sizes are `OVERFLOW`, allowing the later Teensy
adapter to reject an unsupported target range without truncation. Config and
transfer byte counts remain bounded by the 512-byte buffer. Successful config
load, upload, download, and refill jobs return `BYTES` with their acknowledged
new/published/transferred count. Delete returns `COMPLETE`; probe and usage use
their typed results. Every failed job returns `FAULT`.

The public fields have one meaning throughout a job: `phase` selects exactly
one row/edge above; `initial_offset` is the copied request value; for uploads,
`end_offset` is the checked exclusive end `initial_offset + total`; for
download/refill, `end_offset` is derived exactly once from the captured size by
the rows above. `captured_size` is the last accepted `FILE_SIZE`, and
`transferred` counts only acknowledged new upload bytes or acknowledged output
bytes. Upload remaining bytes are always `total - transferred`, and the absolute next write offset is
`initial_offset + transferred`. `config_bytes` counts accepted config reads;
config `READ` uses `buffer + config_bytes` with length
`captured_size - config_bytes`. `buffer_offset` and `pending` describe only an
accepted upload/output slice awaiting partial write/output completion.
`file_open` is sole open-handle authority. `mkdir_path_index` selects target,
temp, then backup as applicable; `mkdir_offset` selects the next proper parent
prefix. `existence_purpose` is non-`NONE` only while one `PATH_EXISTS`
correlation is outstanding or its `BOOL` result is being routed. It selects
exactly one continuation: parent creation, delete, install-target selection,
old-backup removal, committed-backup cleanup, or disposable-temp cleanup.
The explicit `phase` values distinguish initial part-size validation,
append/write, final-size verification, install, rollback, cleanup, and terminal
emission, including zero-byte uploads.

### Action/event oracle

One correlated action may be outstanding. Emission assigns a nonzero,
monotonically incremented 64-bit `action_id`, records its kind/id, and
initializes the returned action fully. IDs are job-object lifetime values:
`init()` starts at one and `start()` never resets or reuses them. Before a
non-reserved emission would consume an id above `UINT64_MAX - 262`, the job
records `OVERFLOW`. The final 262 IDs are reserved for authority-safe close,
rollback, existence/removal cleanup, and mandatory post-commit publication.
The bound is exact: with a moved backup and a 256-byte config whose reads each
make one-byte progress, post-commit work needs backup `EXISTS`/`REMOVE`, open,
size, 256 reads, close, and publish, totaling 262 correlated actions. All
post-commit publication actions and cleanup/rollback actions are emitted from
this reserve; ordinary begin/probe/path preparation/open/transfer/install work
is not. Thus exhaustion is detected before the install commit point unless
the complete worst-case mandatory tail can still finish. Consuming
`UINT64_MAX` sets `action_ids_exhausted` and leaves
`next_action_id` saturated at `UINT64_MAX`; it never increments, wraps, or
reuses zero. Terminal notifications are uncorrelated. Once exhausted, later
`start()` calls emit terminal `FAIL/OVERFLOW` without correlated work; only a
fresh, authority-free `init()` begins a new object lifetime at id one. A new
start is otherwise accepted only after the prior `COMPLETE`/`FAIL`
notification has been emitted.

A call while an action is outstanding may supply `NONE`, which must have every
payload field zero, leaves the job waiting, and emits `NONE`, or the single
matching event. With no outstanding action, only an all-zero `NONE` is valid;
a payload-bearing `NONE` or any non-`NONE` event records `PROTOCOL` and enters
cleanup. A zero/wrong action id, wrong event kind,
malformed payload, or different-action result records `PROTOCOL` and
`abort_requested` but does not clear the outstanding action: its side effect
remains unresolved. Only a later well-formed matching event may establish
whether that side effect occurred; its normal success/failure transition is
applied only to update `file_open`, bytes, or file authority, then the job
enters cleanup without emitting further normal work. Duplicate results arrive
with no outstanding action and follow the first rule. If the matching result
never arrives, the portable job remains boundedly waiting; the Phase 14
checkpoint watchdog is the recovery boundary.

| Outstanding action | Only successful event | Validation |
|---|---|---|
| `BEGIN`, `MKDIR`, `OPEN_*`, `TRUNCATE`, `SEEK`, `RENAME`, `REMOVE`, `CLOSE`, `PUBLISH_CONFIG`, `OUTPUT` | `OK` | `bytes == 0`, `size == 0`, `value == false`, `fault == 0` |
| `MEDIA_PRESENT`, `EXISTS` | `BOOL` | only `value` is meaningful |
| `FILE_SIZE`, `TOTAL_SIZE`, `USED_SIZE` | `SIZE` | only 64-bit `size` is meaningful and it must be `<= INT64_MAX` |
| `READ`, `WRITE` | `BYTES` | `0..outstanding length`; other fields zero; zero is correlated `SHORT_IO` |
| any correlated action | `FAULT` | bytes/size/value zero and nonzero driver fault |

Every action is fully initialized. Correlated rows have their assigned id;
uncorrelated rows have id zero. Any payload not named below is null/zero:

| Action | Exact nonzero payload |
|---|---|
| `NONE`, `BEGIN`, `MEDIA_PRESENT`, `CLOSE`, `TOTAL_SIZE`, `USED_SIZE`, `COMPLETE` | none (`BEGIN`, media, close, and sizes carry only their correlated id) |
| `MKDIR`, `OPEN_READ`, `OPEN_WRITE_BEGIN`, `OPEN_APPEND`, `EXISTS`, `REMOVE` | immutable `path` |
| `TRUNCATE` | `offset == 0`; it addresses the sole open file |
| `SEEK` | 64-bit absolute `offset` |
| `NEED_INPUT` | `length=requested`, `offset=initial_offset+transferred` |
| `READ` | config: `buffer + config_bytes`, `length=captured_size-config_bytes`, `offset=config_bytes`; download/refill: job `buffer`, `length=min(capacity, end_offset-(initial_offset+transferred))`, absolute `offset=initial_offset+transferred` |
| `WRITE` | job `buffer + buffer_offset`, `length=pending`, absolute `offset` |
| `OUTPUT` | immutable job `buffer`, positive acknowledged read `length`, absolute chunk-start `offset` |
| `FILE_SIZE` | none; it addresses the sole open file |
| `RENAME` | immutable source `path` and destination `path_to` |
| `PUBLISH_CONFIG` | immutable job `buffer`, complete config `length` |
| `FAIL` | stable local `fault` |

For `BOOL`, `SIZE`, and `BYTES`, every payload field not named in the row is
zero/false. `FAULT` preserves its driver value in `result.driver_fault` and maps by action
and authority: `MEDIA_PRESENT` to `MEDIA`; `OPEN_*` to `OPEN`; `TRUNCATE` and `SEEK` to
`SEEK`; backup/install rename to `INSTALL`; rollback rename to `ROLLBACK`;
`PUBLISH_CONFIG` to `PUBLISH`; close/remove/existence during terminal cleanup to
`CLEANUP`; install-target or old-backup existence to `INSTALL`; and `BEGIN`,
parent existence or `MKDIR`, read/write/output, size, delete existence,
ordinary remove, or ordinary close to `IO`. A correlated `READ` or `WRITE` zero progress
while bytes remain is `SHORT_IO`; bytes beyond the requested length are
`PROTOCOL`.
Positive short progress is valid. `WRITE` advances only the acknowledged
prefix and re-emits only the unwritten suffix. No job emits a read after its
captured byte target reaches zero, so early zero/EOF is always a fault.
During the explicit post-commit publication reread, open/size/read faults map
to `PUBLISH` instead of their ordinary pre-commit categories; its close fault
maps to `CLEANUP`.

Every existence-sensitive path mutation (`MKDIR` or `REMOVE`) is decomposed
into a correlated existence observation followed by at most one mutation.
Handle-authority operations `TRUNCATE(0)` and `CLOSE` are explicitly outside
this path-idempotency rule:

| `existence_purpose` | `BOOL false` continuation | `BOOL true` continuation |
|---|---|---|
| `MKDIR` | emit `MKDIR(directory_path)` | advance to the next parent prefix |
| `DELETE` | complete successfully when `require_existing == false`; otherwise fail `NOT_FOUND` with zero mutation | emit `REMOVE(target)` |
| `INSTALL_TARGET` | emit `RENAME(temp,target)` | begin `REMOVE_OLD_BACKUP` existence check |
| `REMOVE_OLD_BACKUP` | emit `RENAME(target,backup)` | emit `REMOVE(backup)`, then rename |
| `REMOVE_COMMITTED_BACKUP` | clear backup authority and continue publication/terminal routing | emit `REMOVE(backup)` |
| `CLEANUP_TEMP` | finish the already-recorded failure | emit `REMOVE(temp)` |

The `PATH_EXISTS` phase emits exactly one `EXISTS` for the purpose-selected
immutable path. It clears the purpose after consuming the matching `BOOL`.
`BOOL` with purpose `NONE`, a purpose/path mismatch, or any non-`BOOL` success
is `PROTOCOL`. A false existence result is the pinned Teensy API's only
observable absent result; a driver that can distinguish lookup failure returns
`FAULT`, never false. This removes idempotency guesses from the target adapter
without pretending the pinned boolean API exposes a richer error code.

`OPEN_WRITE_BEGIN` success establishes `file_open` and immediately selects
`UPLOAD_TRUNCATE`; `TRUNCATE(offset=0)` success selects zero-length close or
the normal input/write loop. `TRUNCATE` fault closes through authority-safe
cleanup and maps to `SEEK`. No upload byte is accepted before truncation
acknowledges successfully.

`NEED_INPUT`, `COMPLETE`, and `FAIL` are uncorrelated one-shot application
notifications and do not await an event. `NEED_INPUT` is emitted once per
request window; subsequent `step(NONE)` calls emit `NONE` until supply arrives.
`COMPLETE` or `FAIL` is emitted exactly once, after which steps emit `NONE`
until a valid restart. `OUTPUT` and `PUBLISH_CONFIG` are correlated application
actions: their borrowed buffer remains immutable until matching `OK`/`FAULT`,
so the runtime consumes data before reuse.

After an upload file opens, the job enters `NEED_INPUT` and emits length
`min(capacity, total - transferred)`, stored in `requested`. `supply()` succeeds only
in that state with non-null data and length `1..requested` not exceeding the
remaining total. It copies the bytes into the job buffer, sets `pending`,
clears `requested`, and selects `WRITE` without a driver action. Null, zero,
`requested + 1`, remaining `+ 1`, repeated supply, or supply in any other state
never retains caller memory. In the correct `NEED_INPUT` state, zero length is
`SHORT_IO`; null with positive length, length above requested/remaining, or any
second/out-of-state supply is `PROTOCOL`. Each records `abort_requested` and
enters the ordinary close/rollback/disposable-temp cleanup path. If another
action is outstanding, the job retains it exactly like malformed correlation
and waits for its matching result before cleanup. Supply against `IDLE`, or
against `DONE`/`FAILED` after their terminal notification, returns false
without mutating the terminal result. `FAILED` is selected only after active
authority is exhausted.
Accepted bytes remain job-owned until every partial `WRITE` consumes them.
Only then is the next `NEED_INPUT` emitted or commit begun.

For download/refill, each positive `READ` event is followed by one `OUTPUT`
action for exactly those bytes and their absolute file offset. The read buffer
is not reused and the next `READ` is not emitted until `OUTPUT/OK`. A short
positive read is valid; a zero read before the captured target is `SHORT_IO`.
File growth is ignored after `FILE_SIZE`; shrinkage is detected as short I/O.

### Installation, failure, timeout, and cancellation authority

`REPLACE` and `FINALIZE_PART` install through this exact authority table:

| Edge | Required behavior and authoritative files |
|---|---|
| `EXISTS(target) == false` | Skip backup and attempt `RENAME(temp,target)`. |
| `EXISTS(target) == true` | `EXISTS(backup)`; remove it only when present, then `RENAME(target,backup)`. Backup lookup/removal/rename failure leaves target authoritative and never attempts install. |
| Install succeeds | Target is authoritative and the commit point is crossed. Later cancellation is ignored. If a backup was moved, check its existence and remove it only when present. If publication is requested, run the post-commit reread sequence below before `PUBLISH_CONFIG`. |
| Install fails without backup | Target remains absent and temp/part is preserved; emit `FAIL/INSTALL`. |
| Install fails after backup | Attempt `RENAME(backup,target)` exactly once. Success restores target and preserves temp/part, then fails `INSTALL`. Failure preserves backup and temp/part and fails `ROLLBACK`. |
| Backup removal fails after commit | Target remains authoritative. Optional publish still runs; terminal result is `FAIL/CLEANUP` with the driver fault. |
| `PUBLISH_CONFIG` fails | Installed target remains authoritative; never roll back file installation; terminal result is `FAIL/PUBLISH`. |

If both post-commit backup removal and publication fail, `CLEANUP` wins and
retains the backup-removal driver fault; publication is still attempted but
does not replace the earlier higher-precedence failure.

Post-commit upload publication never borrows streamed upload history. It runs
`OPEN_READ(target) → FILE_SIZE → READ* → CLOSE → PUBLISH_CONFIG`. The captured
size must equal the installed final size and be
`1..publish_capacity`; `config_bytes` starts at zero, partial reads
accumulate into `buffer + config_bytes`, and publication borrows exactly those
complete bytes. Open/size/read/range failure records `PUBLISH`; close failure
records higher-precedence `CLEANUP`. Because commit already crossed, no
publication reread failure rolls the installed target back.

Disposable `REPLACE` temp is checked once and removed when present on
pre-install ordinary failure, timeout, or cancellation. It is preserved after an install/rollback failure
for diagnosis and possible manual recovery. `APPEND_PART` target and
`FINALIZE_PART` temp are durable and never removed by cleanup.
`install_failed` is set only when the correlated `RENAME(temp,target)` returns
`FAULT`, independently of terminal-fault precedence. Cleanup preserves temp
whenever that flag is set, including when an earlier cancellation, timeout, or
protocol fault remains the terminal cause after successful rollback. A
cancellation-triggered rollback with `install_failed == false` still removes a
disposable `REPLACE` temp once.

`deadline_ms` is an absolute wrapping 32-bit time; zero disables it. A nonzero
deadline at start must have signed delta `0..INT32_MAX` from `now_ms`.
`step()` uses this total precedence:

1. validate and consume one matching event to establish bytes, handle, and file
   authority;
2. a successful install rename crosses the commit point and wins over any
   earlier cancellation or deadline, so target remains authoritative and
   mandatory post-commit cleanup/publication finishes;
3. rollback failure overrides every prior cause; cleanup failure overrides
   every cause except rollback;
4. an abort cause already recorded before the call wins over ordinary
   completion from the event;
5. otherwise an event that completes the job wins before deadline evaluation;
6. otherwise `(int32_t)(now_ms - deadline_ms) >= 0` records `TIMEOUT` and
   begins cleanup.

The first ordinary abort cause among cancellation, timeout, protocol, short
I/O, and driver fault is retained. Thus a preexisting cancellation plus a final
close/read/write event finishes as `CANCELLED`; the same final event with no
prior abort completes successfully even at deadline equality. Expiry after
partial progress retains the progress but times out. If timeout/cancellation
occurs while an action is outstanding, it records the abort but retains that
unresolved action until its matching event establishes authority. Timeout
never selects an unsafe immediate terminal.

`cancel()` is idempotent and sets `cancel_requested`. If an action is
outstanding, its correlated event is consumed first because the side effect may
already have happened; no later normal action is emitted. Cancellation before
the install commit point closes an open file once, rolls back a moved backup
once, removes a disposable replace temp once, then emits `FAIL/CANCELLED`.
Cancellation after a successful install rename is ignored while mandatory
backup cleanup and optional config publication finish. A successful in-flight
backup rename therefore cannot be forgotten. Cleanup/rollback faults replace
`CANCELLED` with `CLEANUP`/`ROLLBACK` and preserve the driver fault.

`step()` consumes at most one event and emits at most one action. It may make
bounded internal state transitions that require no external work, but contains
no retry/delay and cannot execute a driver operation. The native oracle is
plan-owned: a table enumerates every reachable state/outstanding-action/event
triple above, the common protocol-fault result for every mismatched triple, all
seven job kinds and three upload modes, and model/property sequences for path
boundaries, 0/1/512/513 capacities, 64-bit arithmetic, deadline
equality/wrap, partial supply/read/write, every action fault, install/rollback,
repeated cancellation, and both `require_existing` delete branches. The
required-missing branch proves `NOT_FOUND`, zero `REMOVE` actions, and zero
driver fault; the optional-missing branch proves successful idempotent
completion.

The target adapter ABI is:

```cpp
#define AIUS_SD_TEENSY_DRIVER_FAULT UINT32_C(0x53444452)
#define AIUS_SD_TEENSY_PROTOCOL_FAULT UINT32_C(0x53445052)

typedef struct {
    File file;
    bool file_open;
    uint32_t last_duration_us;
    uint32_t longest_duration_us;
    uint32_t protocol_faults;
    uint32_t last_fault;
} AiusSdTeensy;

void aius_sd_teensy_init(AiusSdTeensy *adapter);
AiusSdEvent aius_sd_teensy_execute(
    AiusSdTeensy *adapter,
    const AiusSdAction *action);
```

The header is C++-only and includes the pinned Teensy `SD.h`; the composition
object therefore owns its concrete `File` directly rather than through a
singleton or hidden file-static handle. `init()` is valid only on a
default-constructed adapter that has no open file. It performs no SD/File
operation and zeros file authority, durations, diagnostics, and fault state.
Reinitializing an open adapter is a caller-precondition violation.

Every accepted portable job emits `BEGIN`, including repeated jobs.
`aius_sd_teensy_execute()` maps one eligible action to exactly one storage
primitive:

| Action | Sole storage primitive | Exact event and authority |
|---|---|---|
| `BEGIN` | `SD.begin(BUILTIN_SDCARD)` | true `OK`; false `FAULT(DRIVER)`; requires no open file |
| `MEDIA_PRESENT` | `SD.mediaPresent()` | `BOOL` |
| `MKDIR` | `SD.mkdir(path)` | true `OK`; false `FAULT(DRIVER)` |
| `OPEN_READ` | `SD.open(path, FILE_READ)` | valid `File` gives `OK` and sole open authority; invalid gives `FAULT(DRIVER)` |
| `OPEN_WRITE_BEGIN` | `SD.open(path, FILE_WRITE_BEGIN)` | valid `File` gives `OK` and sole open authority without claiming truncation; invalid gives `FAULT(DRIVER)` |
| `OPEN_APPEND` | `SD.open(path, FILE_WRITE)` | valid `File` gives `OK` and sole open authority at end; invalid gives `FAULT(DRIVER)` |
| `TRUNCATE` | `file.truncate(0)` | true `OK`; false `FAULT(DRIVER)`; retains open authority |
| `SEEK` | `file.seek(offset)` | true `OK`; false `FAULT(DRIVER)` |
| `READ` | `file.read(buffer,length)` | public `size_t` result `0..length` gives `BYTES`; any larger value, including the pinned `SIZE_MAX` conversion of an underlying `-1`, gives `FAULT(DRIVER)` |
| `WRITE` | `file.write(buffer,length)` | public `size_t` result `0..length` gives `BYTES`; any larger value gives `FAULT(DRIVER)` |
| `FILE_SIZE` | `file.size()` | `SIZE` |
| `EXISTS` | `SD.exists(path)` | `BOOL`; false is the only pinned absent result |
| `RENAME` | `SD.rename(path,path_to)` | true `OK`; false `FAULT(DRIVER)` |
| `REMOVE` | `SD.remove(path)` | true `OK`; false `FAULT(DRIVER)` |
| `CLOSE` | `file.close()` | the pinned call is void; adapter clears authority and returns `OK` |
| `TOTAL_SIZE` | `SD.totalSize()` | `SIZE` |
| `USED_SIZE` | `SD.usedSize()` | `SIZE` |

`NONE`, `NEED_INPUT`, `OUTPUT`, `PUBLISH_CONFIG`, `COMPLETE`, and `FAIL` are
not adapter actions and return an all-zero `NONE` event with zero primitive and
clock calls. An unknown action kind does the same protocol-fault path as a
malformed eligible action. Null adapter or action returns all-zero `NONE`;
there is no object in which to retain a diagnostic.

Eligible actions require a nonzero ID and exact named payload from the portable
action table. A null required path/buffer, unexpected named field, out-of-range
`length`, `offset > INT64_MAX`, `TRUNCATE` with nonzero offset, open action
while already open, or file action without open authority executes no
primitive and returns `FAULT(PROTOCOL)` echoing the supplied ID. It increments
`protocol_faults` saturating at `UINT32_MAX`, stores
`AIUS_SD_TEENSY_PROTOCOL_FAULT`, and zeros every unnamed event field. Storage
API failure stores `AIUS_SD_TEENSY_DRIVER_FAULT` without incrementing the
protocol counter. The adapter does not retain action IDs: a valid duplicate is
executed and echoed like any other call; correlation/duplicate rejection
belongs to the portable job.

For each valid eligible action, the adapter samples `micros()` immediately
before and after the one storage primitive. Unsigned subtraction is the exact
wrap-safe duration. Successful and failed storage calls update
`last_duration_us`; `longest_duration_us` is the unsigned maximum. A rejected,
application, null, or `NONE` call sets `last_duration_us` to zero and leaves
the maximum unchanged. The two clock samples are diagnostics and do not relax
the one-storage-primitive rule.

The target oracle compiles the real adapter source against a fake `SD.h`,
`File`, `SD`, and `micros()` implementation. Its fixed catalog covers every
one of 23 action kinds in enum order, rejects duplicate keys, and asserts 17
eligible storage actions plus 6 application/non-actions. It covers all
success/failure results, open-authority
transitions, exact payload validation, nulls, zero/malformed IDs, unnamed
fields, duplicate-ID echo behavior, counter saturation, duration equality and
wrap, read/write results `0`, `length`, `length+1`, and `SIZE_MAX`, and fully
zeroed event fields. It asserts exactly one storage primitive
and two clock samples for each valid eligible action, and zero for every
rejected/application action. A source guard rejects `for`, `while`, `do`,
recursion into `aius_sd_teensy_execute`, `delay`, and `delayMicroseconds`;
counts the single primitive site in each eligible switch branch; and has
mutation self-tests proving every forbidden form is rejected.

Target receipts record each returned call duration and require 99th percentile
below 50 ms and maximum below 250 ms. These measurements do not pretend to
cancel a wedged library call: the Phase 14 fault image stalls at checkpoint
`SD` and must prove watchdog reset and USB recovery in 8–10 s.

## Bounded HTTP transaction ABI

Deepen `aius_http` with a transaction object; parsing and response formatting
remain its public codec.

```c
#define AIUS_HTTP_RX_QUOTA 256u
#define AIUS_HTTP_TX_QUOTA 512u
#define AIUS_HTTP_SD_CAPACITY_MAX 512u
#define AIUS_HTTP_CONTENT_TYPE_STORAGE 64u
#define AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT UINT32_C(0x48545450)

typedef enum {
    AIUS_HTTP_TX_IDLE,
    AIUS_HTTP_TX_RECEIVE,
    AIUS_HTTP_TX_DISPATCH,
    AIUS_HTTP_TX_SD_START,
    AIUS_HTTP_TX_SD_TRANSFER,
    AIUS_HTTP_TX_SEND,
    AIUS_HTTP_TX_CLOSE
} AiusHttpTransactionState;

typedef enum {
    AIUS_HTTP_SD_PHASE_NONE,
    AIUS_HTTP_SD_PHASE_WAIT_START,
    AIUS_HTTP_SD_PHASE_UPLOAD_WAIT_INPUT,
    AIUS_HTTP_SD_PHASE_UPLOAD_SUPPLY,
    AIUS_HTTP_SD_PHASE_DOWNLOAD_ADVANCE,
    AIUS_HTTP_SD_PHASE_DOWNLOAD_WRITE,
    AIUS_HTTP_SD_PHASE_DOWNLOAD_ACK,
    AIUS_HTTP_SD_PHASE_WAIT_TERMINAL,
    AIUS_HTTP_SD_PHASE_CANCEL
} AiusHttpSdPhase;

typedef enum {
    AIUS_HTTP_APP_IMMEDIATE,
    AIUS_HTTP_APP_SD_JOB
} AiusHttpApplicationResponseKind;

typedef struct {
    AiusHttpApplicationResponseKind kind;
    unsigned status;
    const char *content_type;
    const uint8_t *body;
    size_t body_length;
    AiusSdJobRequest sd_job;
} AiusHttpApplicationResponse;

typedef enum {
    AIUS_HTTP_SD_EVENT_NONE,
    AIUS_HTTP_SD_STARTED,
    AIUS_HTTP_SD_PROGRESS,
    AIUS_HTTP_SD_NEED_UPLOAD,
    AIUS_HTTP_SD_DOWNLOAD_DATA,
    AIUS_HTTP_SD_COMPLETE,
    AIUS_HTTP_SD_FAILED,
    AIUS_HTTP_SD_CANCELLED
} AiusHttpSdEventKind;

typedef enum {
    AIUS_HTTP_ACTION_NONE,
    AIUS_HTTP_ACTION_ACCEPT,
    AIUS_HTTP_ACTION_READ,
    AIUS_HTTP_ACTION_DISPATCH,
    AIUS_HTTP_ACTION_WRITE,
    AIUS_HTTP_ACTION_SD_START,
    AIUS_HTTP_ACTION_SD_STEP,
    AIUS_HTTP_ACTION_SD_CANCEL,
    AIUS_HTTP_ACTION_CLOSE
} AiusHttpActionKind;

typedef enum {
    AIUS_HTTP_EVENT_NONE,
    AIUS_HTTP_EVENT_IO_RESULT,
    AIUS_HTTP_EVENT_APPLICATION_RESPONSE,
    AIUS_HTTP_EVENT_SD_RESULT,
    AIUS_HTTP_EVENT_PEER_CLOSED,
    AIUS_HTTP_EVENT_PEER_FAULT
} AiusHttpEventKind;

typedef struct {
    AiusHttpEventKind kind;
    uint64_t action_id;
    AiusHttpActionKind completed_action;
    AiusIoEvent io;
    const AiusHttpApplicationResponse *application_response;
    AiusHttpSdEventKind sd_kind;
    const uint8_t *sd_data;
    size_t sd_capacity;
    size_t sd_length;
    uint64_t sd_size;
    AiusSdFault sd_fault;
    uint32_t sd_driver_fault;
    uint32_t peer_fault;
} AiusHttpEvent;

typedef enum {
    AIUS_HTTP_SD_OP_NONE,
    AIUS_HTTP_SD_OP_ADVANCE,
    AIUS_HTTP_SD_OP_SUPPLY_UPLOAD,
    AIUS_HTTP_SD_OP_ACK_DOWNLOAD,
    AIUS_HTTP_SD_OP_FAIL_DOWNLOAD
} AiusHttpSdOperation;

typedef struct {
    AiusHttpActionKind kind;
    uint64_t action_id;
    uint8_t *read_buffer;
    const uint8_t *data;
    size_t length;
    const AiusHttpRequest *request;
    const AiusSdJobRequest *sd_request;
    AiusHttpSdOperation sd_operation;
    uint32_t fault;
} AiusHttpAction;

typedef enum {
    AIUS_HTTP_FAULT_NONE,
    AIUS_HTTP_FAULT_INVALID_INIT,
    AIUS_HTTP_FAULT_PROTOCOL,
    AIUS_HTTP_FAULT_REQUEST_TIMEOUT,
    AIUS_HTTP_FAULT_RESPONSE_TIMEOUT,
    AIUS_HTTP_FAULT_UPLOAD_TIMEOUT,
    AIUS_HTTP_FAULT_DOWNLOAD_TIMEOUT,
    AIUS_HTTP_FAULT_SD_TIMEOUT,
    AIUS_HTTP_FAULT_REQUEST_CAPACITY,
    AIUS_HTTP_FAULT_RESPONSE_CAPACITY,
    AIUS_HTTP_FAULT_ACTION_ID_EXHAUSTED,
    AIUS_HTTP_FAULT_TRANSPORT,
    AIUS_HTTP_FAULT_SD
} AiusHttpFault;

typedef struct {
    AiusHttpTransactionState state;
    AiusHttpSdPhase sd_phase;
    uint8_t *request;
    size_t request_capacity;
    size_t request_length;
    size_t header_length;
    uint64_t body_expected;
    size_t upload_prefix_offset;
    size_t upload_prefix_length;
    size_t upload_prefix_supplied;
    uint64_t upload_supplied;
    uint8_t *response;
    size_t response_capacity;
    size_t response_length;
    size_t response_offset;
    const uint8_t *download_data;
    size_t sd_capacity;
    uint64_t download_size;
    uint64_t download_sent;
    size_t download_length;
    size_t download_offset;
    uint32_t deadline_ms;
    uint64_t next_action_id;
    uint64_t outstanding_action_id;
    AiusHttpActionKind outstanding_action;
    size_t outstanding_length;
    AiusHttpRequest parsed_request;
    AiusSdJobRequest sd_request;
    char sd_target_path[AIUS_SD_PATH_STORAGE];
    char sd_temp_path[AIUS_SD_PATH_STORAGE];
    char sd_backup_path[AIUS_SD_PATH_STORAGE];
    char content_type[AIUS_HTTP_CONTENT_TYPE_STORAGE];
    char sd_content_type[AIUS_HTTP_CONTENT_TYPE_STORAGE];
    AiusHttpFault last_fault;
    AiusSdFault last_sd_fault;
    uint32_t last_transport_fault;
    uint32_t last_driver_fault;
    unsigned last_status;
    bool valid;
    bool client_active;
    bool transport_terminal;
    bool cleanup_requested;
    bool deadline_active;
    bool dispatch_emitted;
    bool close_emitted;
    bool response_started;
    bool response_is_download;
    bool sd_started;
    bool sd_cancel_emitted;
    bool sd_terminal;
    bool action_ids_exhausted;
} AiusHttpTransaction;

size_t aius_http_write_headers(
    uint8_t *output,
    size_t capacity,
    unsigned status,
    const char *content_type,
    uint64_t content_length);

void aius_http_transaction_init(
    AiusHttpTransaction *transaction,
    uint8_t *request,
    size_t request_capacity,
    uint8_t *response,
    size_t response_capacity,
    uint32_t now_ms);

void aius_http_transaction_step(
    AiusHttpTransaction *transaction,
    uint32_t now_ms,
    AiusHttpEvent event,
    AiusHttpAction *action);
```

The framing fields, parser, `aius_http_write_headers()`, and response writer
remain in `aius_http.h`. All transaction-only constants and declarations in
the block above live in the new `aius_http_transaction.h`, which includes
`aius_http.h`, `aius_io.h`, and `aius_sd.h`.
`aius_http_transaction.c` includes that narrow header and contains no target,
socket, or file API. Existing runtime code that uses only the codec continues
to include `aius_http.h`; Phase 14 includes the transaction header only after
removing its legacy local SD state machine. This avoids leaking the portable
SD job namespace across the pre-cutover optical translation unit.

### Codec and request-framing contract

`AiusHttpRequest` gains these fields:

```c
size_t header_length;
bool has_content_length;
uint64_t content_length;
const uint8_t *body;
size_t body_length;
bool body_complete;
```

`header_length` includes the terminating `\r\n\r\n`. `body` is
`data + header_length`; `body_length` is the currently supplied prefix.
`body_complete` is true only when `body_length == content_length`. The parser is
called only after the transaction has found a complete header block. Existing
`has_body` is true exactly when declared `content_length != 0`, including a
streamed request whose body prefix is still empty.

The framing rules are:

- zero or one exact `Content-Length` header is accepted. Its value is a
  non-empty unsigned decimal integer with no sign or surrounding syntax and
  must fit `uint64_t`. Duplicate headers, including equal duplicates, are
  malformed.
- absent `Content-Length` means zero. Bytes after the header when it is absent,
  or bytes beyond the declared length, are malformed. Any
  `Transfer-Encoding` header is rejected as malformed; chunked requests and
  request pipelining are unsupported.
- ordinary requests, including `/api/config/public`, dispatch only when the
  declared body is complete and fits both the transaction request buffer and
  `AIUS_HTTP_MAX_BODY`.
- only `PUT /sd/<resource>` and `PUT /sd/replays/<id>` are streamed. They
  require `Content-Length`, dispatch as soon as the complete header is parsed,
  and may declare a body larger than the request buffer. Any body prefix
  already read with the header remains in the request buffer and is supplied
  to SD before another socket read.
- the parser continues to validate route-specific bodies when
  `body_complete`. It never reads past `body_length`. An incomplete streamed
  body is valid only for the two streamed routes above.

The transaction searches only the newly extended suffix for the header
delimiter and never beyond the smaller of request capacity and
`AIUS_HTTP_MAX_REQUEST`. A buffer filled without a delimiter is
`REQUEST_CAPACITY`/413; after a delimiter, the codec separately enforces its
`AIUS_HTTP_MAX_HEADERS` field-byte bound. Each `READ` offers at most
`AIUS_HTTP_RX_QUOTA` bytes. Before dispatch it writes directly into the unused
suffix of the caller-owned request buffer. During streamed upload it supplies
the retained prefix first, then reuses the request buffer as transaction-owned
staging. `DISPATCH` is emitted exactly once. Every slice in `parsed_request`
remains valid through the correlated application-response step; no slice is
retained after that step.

The request and response buffers must both be non-null, have capacities in
`1..AIUS_HTTP_MAX_REQUEST` and `1..AIUS_HTTP_MAX_RESPONSE`, and not overlap.
Invalid initialization sets `valid = false` and `last_fault =
AIUS_HTTP_FAULT_INVALID_INIT`; every later step returns `NONE` without changing
authority. A null transaction is ignored. A non-null action is always fully
initialized to `NONE`, including on invalid input.

`aius_http_write_headers()` writes the same exact status line, content type,
64-bit decimal `Content-Length`, `Connection: close`, and final blank line as
`aius_http_write_response()`, but never copies or requires a body. It returns
only the header byte count, or zero for an unsupported status, invalid content
type, null output, or insufficient capacity. `aius_http_write_response()` uses
this seam and then appends its complete body. Download headers use it with
`sd_size`; the transaction engine does not duplicate codec formatting.

### Correlated action protocol

Every non-`NONE` action has a nonzero monotonically increasing `action_id`.
The transaction retains its kind, ID, and offered length until a valid
correlated event resolves it. IDs are never reused during the object lifetime.
Reserving past `UINT64_MAX` records `AIUS_HTTP_FAULT_ACTION_ID_EXHAUSTED`,
sets `action_ids_exhausted`, and enters permanent fail-safe quiescence without
emitting a wrapped action. Because no correlated cleanup ID remains, this is
the sole exit that cannot issue `CLOSE` or SD drain actions: `valid` becomes
false, all later steps return `NONE`, and buffers and authority remain
unreusable until `init()` or watchdog reset. The oracle injects this state;
normal operation cannot reach it within the device lifetime.

`IO_RESULT` must name the outstanding `ACCEPT`, `READ`, `WRITE`, or `CLOSE`
action and echo its ID. `READ` and `WRITE` `PROGRESS` is valid only with
`bytes` in `1..outstanding_length`; `ACCEPT` and `CLOSE` `PROGRESS` require
zero bytes. `WOULD_BLOCK` and `CLOSED` require zero bytes and zero fault;
`FAULT` requires zero bytes and nonzero fault. `ACCEPT` never returns
`CLOSED`. An I/O result for any other action is malformed.

`APPLICATION_RESPONSE` must correlate the outstanding `DISPATCH`.
`SD_RESULT` must correlate `SD_START`, `SD_STEP`, or `SD_CANCEL`.
`PEER_CLOSED` and `PEER_FAULT` are unsolicited, require zero action ID and
`completed_action = NONE`, and carry respectively no payload or one nonzero
`peer_fault`. Every field not named for an event is zero or null.

`READ` uses mutable `read_buffer` and requires `data = NULL`; `WRITE` and
upload-supply actions use immutable `data` and require `read_buffer = NULL`.
Every other action requires both pointers null unless its operation explicitly
names one.

`last_transport_fault` retains the nonzero `AiusIoEvent.fault` or
`peer_fault` that selected cleanup; HTTP-owned timeouts and protocol faults
leave it zero. It is distinct from the SD job’s `last_driver_fault`.

A null `action` makes `aius_http_transaction_step()` a pure no-op: it does not
validate or consume the supplied event, test a deadline, change a field, or
alter any authority. Callers must provide a non-null action to make progress.

`NONE` with no outstanding action drives the next action. `NONE` while a
transport or SD drain action is outstanding retains that authority and can
only cause deadline expiry. Because dispatch and job start are synchronous
runtime handshakes, `NONE` on the call after `DISPATCH` or `SD_START` is a
protocol fault. A duplicate, wrong-ID, wrong-action, wrong-state, unsolicited,
or malformed event is a protocol fault. Before any response byte it produces
the fixed protocol response below; after transmission starts it proceeds
directly to cleanup. No malformed event may consume a valid outstanding SD
authority.

An unsolicited peer event is valid only after accept. It consumes an
outstanding `READ`, `WRITE`, or `CLOSE`, because that transport operation can
no longer make progress. If `DISPATCH`, `SD_START`, `SD_STEP`, or `SD_CANCEL`
is outstanding, the transaction sets `cleanup_requested` but retains that
non-transport authority until its correlated event arrives. A malformed event
or timeout follows the same rule for outstanding SD authority. Once retained
authority resolves, cleanup emits transport close if it is not terminal, then
starts or continues SD cancellation. This serialization permits only one
outstanding action without abandoning a correlated SD side effect.

Initialization enters authority-free `IDLE`; `step(NONE)` emits one `ACCEPT`.
An accept `WOULD_BLOCK` returns to accept-ready `IDLE`; a successful accept
starts client authority, clears the prior client’s counters and flags, and
starts request reception. Accepted `CLOSED` or `FAULT` starts cleanup.

Before successful accept there is no client on which to send an error or
close. Any non-`NONE` event with no outstanding `ACCEPT`, or any malformed,
wrong-ID, wrong-action, unsolicited peer, or `CLOSED` result for `ACCEPT`,
records `AIUS_HTTP_FAULT_PROTOCOL`, consumes the one-shot accept authority, and
returns to restartable `IDLE` with `last_status = 0`. A well-formed accept
`FAULT` instead records `AIUS_HTTP_FAULT_TRANSPORT` and its driver fault before
the same restart. Neither path emits `WRITE` or `CLOSE`. The action-ID
exhaustion rule above instead selects permanent fail-safe quiescence.

Every accepted connection emits exactly one idempotent `CLOSE`, even after an
unsolicited peer close/fault. `CLOSE` `PROGRESS`, `CLOSED`, or `FAULT` exhausts
transport authority. `CLOSE` `WOULD_BLOCK` is an adapter protocol fault and
also exhausts transport authority; close is not retried. Per-client storage is
reset and the object becomes restartable `IDLE` only after transport authority
is exhausted and any SD job is terminal. `close_emitted` records issuance,
not acknowledgement.

### Application and response ownership

The application response is borrowed through its correlated step only:

- `IMMEDIATE` requires a supported status, a NUL-terminated content type of
  `1..AIUS_HTTP_CONTENT_TYPE_STORAGE-1` bytes, a non-null body when
  `body_length != 0`, and a zeroed `sd_job`. The complete response is formatted
  during that step; no application pointer is retained.
- `SD_JOB` obeys the same status, content-type, and body rules. For non-download
  jobs these fields are the eventual success response and are formatted during
  the application-response step. For download, `body` is null and
  `body_length` is zero; status and copied content type form the later download
  header.

`AIUS_SD_JOB_REPLAY_REFILL` is not a valid HTTP application response. Replay
refill owns a replay-buffer consumer rather than an HTTP representation and
remains outside the Phase 6 transaction. Returning it is an application
protocol fault.

An HTTP `UPLOAD` job is valid only for one of the two streamed `PUT` requests,
and its `sd_job.total` must equal the request’s declared `content_length`.
A streamed request may receive an `IMMEDIATE` rejection without consuming the
remaining body, or an `SD_JOB/UPLOAD`; every other SD kind is a protocol fault.
A non-streamed request may not return `SD_JOB/UPLOAD`.

For `SD_JOB`, the transaction validates the request and copies all three path
strings into its fixed path storage. A non-null `sd_job.content_type` must be a
NUL-terminated string of `1..AIUS_HTTP_CONTENT_TYPE_STORAGE-1` bytes and is
copied separately into `sd_content_type`; a null value remains null. The
stored `AiusSdJobRequest` points only into those retained fields. The top-level
response content type is always copied into `content_type`. For `DOWNLOAD`,
both content-type fields describe the downloaded representation and must be
byte-for-byte equal; for other job kinds they are independent and follow the
committed SD request-field table. The returned `SD_START` points to the stored
request, so the runtime can synchronously call `aius_sd_job_start()` before the
next HTTP step without borrowing application memory. `SD_START` starts the SD
job with deadline zero; HTTP owns the inactivity deadline and cancellation
protocol.

Permitted application statuses are `200`, `201`, `204`, `400`, `401`, `403`,
`404`, `405`, `408`, `409`, `413`, `416`, `500`, `503`, and `507`.
`aius_http_reason()` contains an exact reason for each, including `Conflict`
and `Range Not Satisfiable`. Any other status or invalid application field is
a protocol fault.

If a complete immediate or non-download success response does not fit, the
transaction replaces it with this complete response when that fits:

```text
500 application/json {"ok":false,"error":"response_too_large"}
```

If the fallback does not fit, no partial response is emitted. Download headers
are formatted only after the first `DOWNLOAD_DATA`, or download `COMPLETE` for
a zero-byte file, supplies the authoritative `sd_size`. Header overflow uses
the same fallback. A nonterminal job follows chunk release and cancellation;
a zero-byte job that reported terminal `COMPLETE` needs only transport cleanup.

### SD transfer and lifetime contract

An `SD_STARTED` event supplies only `sd_capacity`, which must be in
`1..AIUS_HTTP_SD_CAPACITY_MAX` and is retained by the transaction.
`SD_PROGRESS` has no payload and reports that one bounded runtime epoch
completed without reaching an application boundary. `NEED_UPLOAD` supplies a
positive `sd_length` no greater than retained `sd_capacity`.
`DOWNLOAD_DATA` supplies non-null `sd_data`, positive `sd_length` no greater
than retained `sd_capacity`, and the authoritative total `sd_size`; every
later chunk repeats the same size. `COMPLETE` carries `sd_size` only for a
zero-byte download and otherwise has no payload. A nonzero download may not
complete before at least one `DOWNLOAD_DATA`. `FAILED` carries nonzero typed
`sd_fault` and may carry `sd_driver_fault`. `CANCELLED` is terminal and may
carry an authoritative cleanup/rollback `sd_fault` and driver fault. Every
unnamed field, including `sd_capacity` after `STARTED`, is zero or null.

`SD_STEP` operations are exact:

- `ADVANCE` asks the owned job to advance one bounded epoch. It performs at
  most one target SD action and returns `SD_PROGRESS` when that epoch does not
  expose another semantic event.
- `SUPPLY_UPLOAD` carries transaction-owned staged bytes for immediate
  `aius_sd_job_supply()`. The buffer remains unchanged until the correlated SD
  result. Retained body-prefix bytes are supplied before any further `READ`.
- `ACK_DOWNLOAD` acknowledges one complete prior `OUTPUT` chunk only after
  every byte was accepted by transport.
- `FAIL_DOWNLOAD` resolves that `OUTPUT` with failure when transport cannot
  finish it.

`FAIL_DOWNLOAD` always carries
`fault = AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT`; every other HTTP action carries
zero fault. The runtime feeds that exact nonzero value as the correlated
`AiusSdEvent.fault` for `OUTPUT`. The SD engine therefore records
`AIUS_SD_FAULT_IO`, which is an internal drain mechanism rather than a new
HTTP-visible root cause.

The correlated SD result matrix is exhaustive:

| Outstanding action | Valid `sd_kind` values |
|---|---|
| `SD_START` | `STARTED`, `FAILED` |
| `SD_STEP/ADVANCE`, upload job | `PROGRESS`, `NEED_UPLOAD`, `COMPLETE`, `FAILED` |
| `SD_STEP/ADVANCE`, download job | `PROGRESS`, `DOWNLOAD_DATA`, `COMPLETE`, `FAILED` |
| `SD_STEP/ADVANCE`, other job | `PROGRESS`, `COMPLETE`, `FAILED` |
| `SD_STEP/SUPPLY_UPLOAD` | `PROGRESS` |
| `SD_STEP/ACK_DOWNLOAD` | `PROGRESS`, `DOWNLOAD_DATA`, `COMPLETE`, `FAILED` |
| `SD_STEP/FAIL_DOWNLOAD` | `PROGRESS`, `CANCELLED` |
| `SD_CANCEL` | `PROGRESS`, `CANCELLED` |
| `SD_STEP/ADVANCE` while cancelling | `PROGRESS`, `CANCELLED` |

Every other SD kind is a protocol fault and retains outstanding SD authority.
`STARTED` is accepted once. `NEED_UPLOAD` is valid only for `UPLOAD`;
`DOWNLOAD_DATA` only for `DOWNLOAD`; and `COMPLETE` is valid only when exact
upload/download byte accounting is already satisfied.
`SD_EVENT_NONE` is never a valid `SD_RESULT`.

The transaction never retains RX data owned by the transport; `READ` writes
directly into its request buffer. A download chunk is borrowed from the SD job
but remains immutable until the correlated `ACK_DOWNLOAD` or `FAIL_DOWNLOAD`,
not merely until the next HTTP call. The transaction retains its pointer,
length, and offset while partial writes or `WOULD_BLOCK` occur. It never asks
for another SD action while a chunk is unacknowledged.

For upload, `NEED_UPLOAD` causes a read of at most the smaller of 256 bytes,
the job’s offered capacity, and the declared body remainder. A positive read
is followed by `SUPPLY_UPLOAD`; exact supplied and declared offsets are
retained independently. Asking for input after the declared body is exhausted,
completing before all declared bytes were supplied, inconsistent download
sizes, zero/oversized chunks, or bytes beyond the declared body is a protocol
fault followed by cancellation.

Cleanup action ordering is exact after any retained outstanding action
resolves:

1. if a download chunk is unacknowledged, emit and resolve
   `SD_STEP/FAIL_DOWNLOAD`;
2. if a fixed error was selected before any response byte and transport is
   still usable, send that complete response;
3. if transport close has not been emitted, emit and resolve `CLOSE`;
4. if the SD job remains nonterminal, emit `SD_CANCEL` once and continue its
   `SD_STEP/ADVANCE` drain.

Thus the bounded chunk release precedes close only when needed; every other
cleanup sends any still-permitted deterministic error, closes transport, then
cancels SD. A peer/transport failure, response-write failure, or prior response
byte suppresses the pending fixed error. `SD_CANCEL` begins the acknowledged
drain.
Before calling cancel, the runtime feeds any retained adapter result to the job
so its correlated driver action is resolved. It calls `aius_sd_job_cancel()`
exactly once and returns `SD_PROGRESS` unless the job is already terminal. The
transaction then issues `SD_STEP/ADVANCE` epochs, each performing at most one
SD action, until rollback/cleanup returns `CANCELLED`.
If `FAIL_DOWNLOAD` itself returns terminal `CANCELLED`, the job has no remaining
authority and HTTP does not emit the otherwise-required `SD_CANCEL`.

When terminal SD state contains
`AIUS_SD_FAULT_IO/AIUS_HTTP_SD_OUTPUT_CANCEL_FAULT`, the runtime filters that
synthetic pair and returns `CANCELLED` with `sd_fault = NONE` and zero driver
fault. A rollback or cleanup fault that replaced it is returned with
`CANCELLED` and remains authoritative. Any other pre-existing SD fault is also
returned with `CANCELLED` and retained as authoritative. `sd_cancel_emitted`
prevents a second cancel call. HTTP does not restart, reset either buffer, or
allow another accept until both `transport_terminal` and `sd_terminal` are
true.

HTTP never calls a socket or file API and never loops over transport or SD
progress.

### Deadlines, transmission, and deterministic failures

Deadlines are inactivity deadlines: request `150 ms`, response write `100 ms`,
upload and other bounded SD control jobs `2 s`, and download `10 s`.
`PROBE`, `CONFIG_LOAD`, `USAGE_REFRESH`, and `DELETE` use the 2 s SD-control
deadline. `REPLAY_REFILL` is rejected by HTTP. Successful accept starts the
request deadline; `SD_STARTED` starts the deadline selected by the stored job
kind.

Validated positive RX, TX, upload-supply, `NEED_UPLOAD`, `DOWNLOAD_DATA`, and
`SD_PROGRESS` results rearm the matching deadline. For `SD_PROGRESS`, the
runtime guarantees that one target result or one finite job-state transition
was consumed; action issuance alone is not progress. `WOULD_BLOCK`,
application dispatch, and an unresolved action do not rearm. The
application-response and SD-start handshakes must resolve on the next call and
therefore have no separate deadline. Close has no retry deadline because it is
one-shot.

The response-write deadline starts at `now_ms + 100` when a response first
becomes sendable, before returning the first `WRITE`. Immediate and fixed
responses normally become sendable when selected. A fixed error selected while
correlated SD authority or an unacknowledged chunk is retained is formatted
but becomes sendable only after that authority resolves and the chunk is
failed. A non-download SD success response may be formatted earlier but
becomes sendable only on `SD_COMPLETE`. A download header becomes sendable
when the first `DOWNLOAD_DATA`, or zero-byte download `COMPLETE`, supplies
authoritative size. Merely formatting a deferred response and merely issuing
`WRITE` do not start or rearm the deadline.

Event and time precedence is exact:

1. validate the event; malformed payload, correlation, or state selects
   `AIUS_HTTP_FAULT_PROTOCOL` regardless of the clock;
2. consume a well-formed correlated event;
3. its terminal `CLOSED`, `FAULT`, `COMPLETE`, `FAILED`, or `CANCELLED`
   result wins over simultaneous expiry;
4. validated progress named above at equality wins and rearms;
5. only a remaining `NONE` or `WOULD_BLOCK` at equality selects timeout.

Use `(int32_t)(now_ms - deadline_ms) >= 0`; every interval is below
`INT32_MAX`. Request expiry selects `REQUEST_TIMEOUT`; response-write expiry
selects `RESPONSE_TIMEOUT`; upload expiry selects `UPLOAD_TIMEOUT`; download
expiry selects `DOWNLOAD_TIMEOUT`; and other SD-job expiry selects
`SD_TIMEOUT`. Any active SD job is cancelled through the drain protocol.
Request timeout formats the fixed 408 response. Response, upload, download,
and SD-control timeouts send no new response and retain `last_status = 0`
unless transmission had already selected a status.

`WRITE` is exactly the next
`min(AIUS_HTTP_TX_QUOTA, remaining)` bytes. Immediate responses and download
headers use `response + response_offset`; download bodies use
`download_data + download_offset`. `WOULD_BLOCK` preserves all offsets and a
later epoch reoffers the identical slice with a new action ID. Positive partial
progress advances exactly the accepted prefix and rearms the deadline. Header
and download-body offsets are independent. A download chunk is acknowledged
only after all its bytes are accepted. Completion closes once; a zero-byte
download still sends its header, confirms SD completion, and closes.

All fixed errors use `application/json`:

| Cause | Status | Exact body |
|---|---:|---|
| malformed request or body | 400 | `{"ok":false,"error":"bad_request"}` |
| method | 405 | `{"ok":false,"error":"method_not_allowed"}` |
| route | 404 | `{"ok":false,"error":"not_found"}` |
| request timeout | 408 | `{"ok":false,"error":"request_timeout"}` |
| request/body/header capacity | 413 | `{"ok":false,"error":"request_too_large"}` |
| protocol fault before TX | 400 | `{"ok":false,"error":"protocol"}` |
| response overflow | 500 | `{"ok":false,"error":"response_too_large"}` |
| download `OPEN` | 404 | `{"ok":false,"error":"sd_not_found"}` |
| SD `NOT_FOUND` | 404 | `{"ok":false,"error":"sd_not_found"}` |
| SD `RANGE` | 416 | `{"ok":false,"error":"sd_range"}` |
| SD `MEDIA` | 503 | `{"ok":false,"error":"sd_unavailable"}` |
| SD `CAPACITY` or `OVERFLOW` | 507 | `{"ok":false,"error":"sd_capacity"}` |
| every other SD failure | 500 | `{"ok":false,"error":"sd_failure"}` |

An SD `OPEN` maps to 404 only for download; `NOT_FOUND` maps to 404 for every
job, although the validated application contract emits it only for a
required-existing delete. Otherwise the remaining-SD mapping applies.
Transport-caused cancellation sends no response. A protocol or SD
failure after any response byte closes without attempting a second response.
The transaction retains `last_fault`, `last_sd_fault`, `last_driver_fault`, and
`last_status` through cleanup and restart until the next successful accept.

`last_fault` assignment is deterministic: parser `EMPTY`, `MALFORMED`, `BODY`,
`METHOD`, and `ROUTE` results use `PROTOCOL`; request/header/body size failures
use `REQUEST_CAPACITY`; invalid application/event state uses `PROTOCOL`;
response fallback or header capacity uses `RESPONSE_CAPACITY`; transport I/O
or peer failure uses `TRANSPORT`; and any non-timeout SD terminal fault uses
`SD`. The specific timeout assignments are listed above. `last_status` is the
selected application or fixed-response status, or zero when no response was
selected. A later transport failure does not erase an already selected status.
SD rollback/cleanup replacement remains authoritative in `last_sd_fault`;
otherwise the first validated terminal cause in the precedence list wins.

The independent oracle enumerates every reachable
state/outstanding-action/event triple and all unnamed-payload violations. It
also covers every parser result, both application kinds, every SD event and
operation, cancel/drain authority, capacities around every fixed response,
RX/TX splits at `0/1/255/256/257/511/512/513`, zero and partial download
chunks, content-length zero/maximum/overflow/duplicate/invalid cases,
deadline equality and wrap, action-ID exhaustion, duplicate
dispatch/close/cancel, cleanup-fault precedence, and multiple accept-close
restart sequences.

## Bounded CDC runtime ABI

Deepen `aius_usb_control` with a runtime transaction around the existing pure
parser.

```c
typedef enum {
    AIUS_USB_RUNTIME_IDLE,
    AIUS_USB_RUNTIME_DISPATCH,
    AIUS_USB_RUNTIME_TX,
    AIUS_USB_RUNTIME_ACK_PENDING,
    AIUS_USB_RUNTIME_ENTER_BOOTLOADER
} AiusUsbRuntimeState;

typedef struct {
    AiusUsbCommand command;
} AiusUsbApplicationRequest;

typedef struct {
    AiusUsbError error;
    const char *verb;
    size_t verb_length;
    const char *payload;
    size_t payload_length;
} AiusUsbApplicationResponse;

typedef struct {
    const uint8_t *rx;
    size_t rx_length;
    size_t tx_capacity;
    bool connected;
    bool tx_event_present;
    AiusIoEvent tx_event;
    const AiusUsbApplicationResponse *application_response;
} AiusUsbRuntimeInput;

typedef enum {
    AIUS_USB_ACTION_NONE,
    AIUS_USB_ACTION_DISPATCH,
    AIUS_USB_ACTION_WRITE,
    AIUS_USB_ACTION_ENTER_BOOTLOADER
} AiusUsbRuntimeActionKind;

typedef struct {
    AiusUsbRuntimeActionKind kind;
    const AiusUsbApplicationRequest *request;
    const uint8_t *tx;
    size_t tx_length;
    size_t rx_consumed;
} AiusUsbRuntimeAction;

typedef struct {
    AiusUsbParser parser;
    AiusUsbRuntimeState state;
    char *response;
    size_t response_capacity;
    size_t response_length;
    size_t response_offset;
    uint32_t bootloader_deadline_ms;
    uint32_t protocol_faults;
    size_t outstanding_length;
    AiusUsbApplicationRequest pending_request;
    bool valid;
    bool write_outstanding;
    bool dispatch_emitted;
    bool bootloader_authorized;
    bool enter_emitted;
} AiusUsbRuntime;

void aius_usb_runtime_init(
    AiusUsbRuntime *runtime,
    char *response,
    size_t response_capacity);

void aius_usb_runtime_step(
    AiusUsbRuntime *runtime,
    uint32_t now_ms,
    AiusUsbRuntimeInput input,
    AiusUsbRuntimeAction *action);
```

The caller owns RX and response buffers. The response buffer must be non-null,
with capacity `48..320`. Init prologue is exact: if `runtime == NULL`, return
without touching `response`; otherwise fully reset the runtime, touch no
response bytes, and leave `valid = false` when `response == NULL` or capacity
is outside `48..320`. The runtime object is a caller disjointness precondition
from the response span and, at every step, the RX span, application-response
object, verb span, and payload span; the target layout and native oracle
statically prove it. Step prologue order is exact: if `action == NULL`, return
without touching any object; otherwise initialize the action fully to `NONE`;
then if `runtime == NULL || !runtime->valid`, return; only then validate RX and
mutate counters, deadlines, or authority. Action storage is a caller
disjointness precondition from the runtime object, response span, RX span,
application-response object, verb span, and payload span; target and oracle
fixtures prove that layout. Calls violating either disjointness precondition
are outside the portable ABI rather than partially defined alias cases.
The step consumes at most the first 64 RX bytes, offers at most 64 TX bytes
within `tx_capacity`, and returns at most one action.

- On every step, before mutation, `rx == NULL` exactly when `rx_length == 0`
  and a nonempty RX span must not overlap the response buffer. A violation
  increments `protocol_faults` once, sets `rx_consumed = 0`, and suppresses RX
  parsing only; it cannot preempt a retained application response, correlated
  TX event, connection loss, or deadline. Valid RX consumes at most
  `min(rx_length, 64)` bytes and stops at the first parser terminal event.
  `rx_consumed` is determined by the entry state: it is the exact inclusive
  count through that terminal byte when entry state is `IDLE`, even when the
  returned action changes state; it is zero for other entry states. The caller
  retains `[rx_consumed, rx_length)`. A
  suppressed LF after CR is consumed and counted. Disconnect during an
  incomplete line resets the parser and discards only that incomplete line.
- A complete command is copied to `pending_request` and emits `DISPATCH`
  exactly once. `PROTOCOL`, `ID`, `AUTH`, and `BOOTLOADER` all use this seam;
  the application supplies protocol/build/status/auth payloads and authorizes
  bootloader entry. The request remains valid until the response step returns.
- An application response is borrowed only on the step immediately following
  `DISPATCH`. A missing, duplicate, or out-of-state response is a protocol
  fault. `protocol_faults` saturates at `UINT32_MAX`. Success has
  `error = NONE`, `verb != NULL`, `verb_length` in `1..10`, a case-sensitive
  canonical verb matching the pending command exactly:
  `PROTOCOL→PROTOCOL`, `ID→ID`, `AUTH→AUTH`, or
  `BOOTLOADER→BOOTLOADER`, and a
  printable-ASCII payload of at most 256 bytes with `payload == NULL` exactly
  when its length is zero. `BOOTLOADER` success requires zero payload; the
  runtime alone appends `entering`. Error responses have an enum in
  `BAD_VERSION..INTERNAL`, null verb and payload, and zero lengths. Formatting
  uses only explicit lengths; the legacy C-string formatter remains solely for
  the pre-Phase-14 adapter. Invalid application fields or formatting overflow
  select the exact fixed `AIUS/1 ERR <request-id> INTERNAL error\n` response.
  Application verb and payload spans must not overlap the response buffer.
  Parser errors use request ID zero. The minimum response capacity makes that
  response and the fixed bootloader acknowledgement fit.
- TX requests one write of at most 64 bytes per epoch. Zero capacity preserves
  the response offset and does not busy-wait. Emitting `WRITE` retains
  `write_outstanding` and `outstanding_length`. The next step must contain one
  `tx_event`: `PROGRESS` requires `bytes` in `1..outstanding_length` and zero
  fault; `WOULD_BLOCK` requires zero bytes/fault and preserves the offset;
  `CLOSED` requires zero bytes/fault; `FAULT` requires zero bytes and nonzero
  fault. Only `PROGRESS` advances by accepted bytes. Missing, malformed,
  duplicate, or unsolicited results increment `protocol_faults` without
  advancing the offset. A normal response aborts to restartable `IDLE`;
  authorized bootloader entry retains its one-shot authority.
- Only a valid successful response for the pending `BOOTLOADER` command
  authorizes HalfKay. Authorization formats the fixed
  `AIUS/1 OK <request-id> BOOTLOADER entering\n` acknowledgement and starts one
  non-rearming deadline at `now_ms + 500`. `ACK_PENDING` begins when its final
  `WRITE` is outstanding. A TX event is validated and consumed before the
  deadline is tested. Full acceptance enters immediately, including at exact
  equality. Otherwise `(int32_t)(now_ms - bootloader_deadline_ms) >= 0`,
  `CLOSED`, `FAULT`, or connection loss after authorization selects entry.
  Connection loss before authorization cannot bypass the application decision.
  Partial progress and `WOULD_BLOCK` never rearm the deadline.
- Selecting entry changes to terminal `ENTER_BOOTLOADER` and emits exactly one
  `ENTER_BOOTLOADER` action. Later calls return `NONE`; only reset or `init()`
  restarts it. Completing a normal/error response returns to `IDLE`. Normal
  disconnect aborts the transaction, clears parser/offset/write authority, and
  permits a clean reconnect.

Inputs are processed in this order: a retained application response, a
correlated TX event, connection loss, deadline, then new RX/action emission.
Thus bootloader authorization arriving with disconnection is validated first
and then enters immediately. The non-rearming deadline is tested in both
authorized `TX` and `ACK_PENDING`. `init()` clears every parser, buffer,
correlation, deadline, authorization, and emission field.

The correlated transition matrix is normative:

| ID | Entry authority | Input | Authority/post-state |
|---|---|---|---|
| `T01` | `IDLE` | disconnected | reset incomplete parser; consume no RX |
| `T02` | `IDLE` | connected, no RX | remain `IDLE` |
| `T03` | `IDLE` | incomplete RX | retain parser prefix |
| `T04` | `IDLE` | complete command | copy request; emit `DISPATCH` |
| `T05` | `IDLE` | parser terminal error | format request-ID-zero error; enter `TX` |
| `T06` | `DISPATCH` | valid normal success | format response; enter `TX` |
| `T07` | `DISPATCH` | valid application error | format response; enter `TX` |
| `T08` | `DISPATCH` | valid bootloader success | format fixed acknowledgement; authorize and enter `TX` |
| `T09` | `DISPATCH` | invalid response | increment once; format `INTERNAL`; enter `TX` |
| `T10` | `DISPATCH` | missing response | increment once; format `INTERNAL`; enter `TX` |
| `T11` | no dispatch | application response present | increment once; ignore response |
| `T12` | `TX`, no write | zero capacity | retain offset; emit `NONE` |
| `T13` | `TX`, no write | nonfinal capacity | emit one retained `WRITE` slice |
| `T14` | `TX`, no write | final normal slice | emit one retained `WRITE` slice |
| `T15` | authorized `TX`, no write | final bootloader slice | emit retained `WRITE`; enter `ACK_PENDING` |
| `T16` | write outstanding | valid partial `PROGRESS` | consume correlation; advance accepted bytes; offer at most one replacement |
| `T17` | write outstanding | valid full nonfinal `PROGRESS` | consume correlation; advance; remain `TX`; offer at most one next slice |
| `T18` | write outstanding | valid full final normal `PROGRESS` | consume correlation; advance; enter `IDLE` |
| `T19` | write outstanding | valid full final bootloader `PROGRESS` | consume correlation; advance; enter bootloader |
| `T20` | write outstanding | valid `WOULD_BLOCK` | consume correlation; preserve offset; offer at most one replacement |
| `T21` | write outstanding | valid `CLOSED`/`FAULT` | consume correlation; normal enters `IDLE`; authorized bootloader enters |
| `T22` | write outstanding | missing/malformed result, normal | increment once; consume correlation; enter `IDLE` |
| `T23` | write outstanding | missing/malformed result, bootloader | increment once; consume correlation; retain authorization; offer at most one replacement |
| `T24` | terminal bootloader | any input after one-shot emission | emit `NONE`; retain terminal state |

Each independently invalid application, TX, or RX input channel increments the
counter exactly once. Only the state-authoritative channel selects the
transition; unsolicited secondary inputs cannot advance offsets or replace its
result.

The source-blind oracle implements `T01..T24` as transition families with 36
exact stable variant keys. `T01..T15`, `T18`, and `T19` each have one variant;
`T16`, `T17`, and `T20` each have `.normal` and `.boot`; `T21` has
`.normal.closed`, `.normal.fault`, `.boot.closed`, and `.boot.fault`; `T22`
and `T23` each have `.missing` and `.malformed`; and `T24` has `.none`, `.rx`,
`.application`, `.tx`, and `.disconnect`. Each variant fixes entry state,
connection, capacity, time relation, authority, complete input payload,
expected fault delta, offsets, post-state, action, and retained authority. The
registry rejects duplicate family IDs and variant keys and asserts exactly 24
families and 36 variants. It also asserts 12
malformed/unnamed TX payload rows; 12 unsolicited/duplicate input rows; 8 RX
quota/terminal rows covering lengths and split positions
`0/1/63/64/65`, CR, LF suppression, and retained suffix; and 21 response rows:
four exact command/verb successes, six exact application-error enums, six
invalid pointer/length/verb/payload rows, and five capacity rows for invalid
`47`, minimum `48`, primary overflow at `48`, maximum `320`, and invalid `321`.
The registry further asserts 6 deadline rows covering before/equal/after with
and without `millis()` wrap, plus 4 repeated disconnect/reconnect transactions.
The test asserts each exact row count and rejects duplicate matrix keys.

Recovery-safe mode accepts `PROTOCOL`, `ID`, `AUTH`, and `BOOTLOADER`; verified
reflash is its only exit.

## Portable runtime scheduler ABI

`aius_runtime` owns service order and watchdog-feed eligibility. It has no
Arduino dependencies.

```c
typedef struct {
    uint16_t checkpoint;
    uint16_t max_calls;
    uint16_t max_rx_bytes;
    uint16_t max_tx_bytes;
    uint32_t service_budget_ms;
} AiusRuntimeBudget;

typedef struct {
    void *context;
    bool (*service)(
        void *context,
        const AiusRuntimeBudget *budget);
    void (*checkpoint)(void *context, uint16_t checkpoint);
    void (*feed_watchdog)(void *context);
} AiusRuntimeOps;

const AiusRuntimeBudget *aius_runtime_budget(
    uint16_t checkpoint);

bool aius_runtime_epoch(
    const AiusRuntimeOps *ops,
    bool recovery_safe);
```

The immutable descriptor table is literal:

| Checkpoint constant | `max_calls` | `max_rx_bytes` | `max_tx_bytes` | `service_budget_ms` |
|---|---:|---:|---:|---:|
| `AIUS_BOOT_CHECKPOINT_RESET_CAPTURE` | 2 | 0 | 0 | 250 |
| `AIUS_BOOT_CHECKPOINT_USB_RX` | 64 | 64 | 0 | 0 |
| `AIUS_BOOT_CHECKPOINT_USB_TX` | 1 | 0 | 64 | 500 |
| `AIUS_BOOT_CHECKPOINT_METRICS` | 1 | 0 | 0 | 0 |
| `AIUS_BOOT_CHECKPOINT_NETWORK` | 1 | 0 | 0 | 0 |
| `AIUS_BOOT_CHECKPOINT_HTTP` | 1 | 256 | 512 | 0 |
| `AIUS_BOOT_CHECKPOINT_UDP` | 1 | 532 | 532 | 2 |
| `AIUS_BOOT_CHECKPOINT_REPLAY` | 1 | 2048 | 0 | 0 |
| `AIUS_BOOT_CHECKPOINT_SD` | 1 | 512 | 512 | 0 |
| `AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE` | 0 | 0 | 0 | 0 |

`service_budget_ms` is a relative per-call service interval supplied to the
service, never an absolute timestamp and never measured by the scheduler. Zero
means no scheduler-supplied interval. DHCP/backoff, HTTP inactivity, SD
inactivity, and the watchdog remain owned by their portable service ABIs; the
500 ms USB value is the already-committed acknowledgement interval. UDP's 532
bytes are the 16-byte header, 512-byte maximum payload, and 4-byte CRC.
`aius_runtime_budget()` returns the same stable pointer for a valid checkpoint
for the process lifetime and `NULL` for `10..UINT16_MAX`.

`aius_runtime_epoch()` invokes the normative checkpoint table in order. Before
each service it calls `checkpoint()`. In normal mode it calls all nine services
then records `EPOCH_COMPLETE` and feeds once. In recovery-safe mode it calls
only bounded USB RX/TX, records `EPOCH_COMPLETE`, and feeds once. A service
returning false ends the epoch without feed; the next watchdog reset identifies
that checkpoint.

The scheduler is stateless: it has no init, construction, generation, counter,
or storage authority. It supplies immutable budgets and enforces only
order, mode selection, and feed eligibility; Phase 12 services enforce calls
and bytes. At entry it copies the complete `AiusRuntimeOps` value. `ops == NULL`
or any null `service`, `checkpoint`, or `feed_watchdog` callback returns false
before any callback. A null `context` is valid and forwarded unchanged.
Callbacks may mutate the original ops storage, including when context aliases
it, without changing the entry snapshot. Callback re-entry into
`aius_runtime_epoch()` with the same ops/context is a caller precondition
violation.

Normal trace is checkpoints `0..8`, each immediately followed by one service,
then checkpoint `9`, then one feed. Recovery-safe trace is checkpoint/service
`1`, checkpoint/service `2`, checkpoint `9`, feed. A false service ends the
trace immediately: no later checkpoint, service, epoch-complete checkpoint, or
feed. Return true means the complete trace ran and the void feed callback was
invoked; it does not claim a hardware feed result.

The source-blind oracle asserts 10 descriptor rows and all 65,526 invalid
selectors; two complete traces with event counts `20` and `6`; 11 failure
traces with normal lengths `2,4,...,18` and safe lengths `2,4`; 10 ops-validity
rows covering null ops, every callback-presence combination, and null context;
and two ops/context alias-mutation rows. Every registry rejects duplicate keys
and asserts its exact count.

## Optical target-composition ABI

Application semantics and target I/O pre-land outside the composition root.
`aius_optical_app` owns route/command dispatch and performs no transport, SD,
watchdog, or delay call.

```c
typedef struct {
    void *context;
    bool (*http_response)(
        void *context,
        const AiusHttpRequest *request,
        uint8_t *body,
        size_t body_capacity,
        AiusHttpApplicationResponse *response);
    bool (*usb_payload)(
        void *context,
        AiusUsbCommand command,
        char *verb,
        size_t verb_capacity,
        char *payload,
        size_t payload_capacity);
} AiusOpticalAppOps;

typedef struct {
    AiusOpticalAppOps ops;
    uint8_t *body;
    size_t body_capacity;
    char *usb_verb;
    size_t usb_verb_capacity;
    char *usb_payload;
    size_t usb_payload_capacity;
} AiusOpticalApp;

void aius_optical_app_init(
    AiusOpticalApp *app,
    const AiusOpticalAppOps *ops,
    uint8_t *body,
    size_t body_capacity,
    char *usb_verb,
    size_t usb_verb_capacity,
    char *usb_payload,
    size_t usb_payload_capacity);

void aius_optical_http_dispatch(
    AiusOpticalApp *app,
    const AiusHttpRequest *request,
    AiusHttpApplicationResponse *response);

void aius_optical_usb_dispatch(
    AiusOpticalApp *app,
    const AiusUsbApplicationRequest *request,
    AiusUsbApplicationResponse *response);
```

Both dispatch functions fully initialize their output and invoke exactly one
callback for a recognized request. `http_response` receives every parsed route,
including home, status, events, DSP config (`GET` returning the effective
model, `PUT` validating and returning an `SD_JOB` response with
`publish = true` and `publish_capacity = AIUS_DSP_MODEL_MAX_JSON_BYTES`, or an
`IMMEDIATE` 401/400/409 for an auth/validation/stale-generation failure),
controls, SD files, config, and replay list/status/start/stop/delete. It
supplies status, content type, body, and optional SD job through the complete
application response. Body pointers
must address the supplied app buffer or read-only static storage; path pointers
must address app-owned storage. They remain valid until the next dispatch.
Callback `false`, invalid pointer/length, unknown routes/commands, and buffer
exhaustion produce deterministic 4xx/5xx or `INTERNAL` responses. Native tests
drive every existing HTTP route and USB command through fake callbacks and
golden responses.

`aius_runtime_teensy` owns target service callbacks and buffers; its header
contains the complete composition object and no file-static lifecycle state:

```c
#define AIUS_CDC_ADAPTER_PROTOCOL_FAULT UINT32_C(0x55534250)

typedef struct {
    void *context;
    bool (*connected)(void *context);
    size_t (*write_capacity)(void *context);
    AiusIoEvent (*read_byte)(void *context, uint8_t *byte);
    AiusIoEvent (*write)(
        void *context,
        const uint8_t *data,
        size_t length);
    void (*enter_bootloader)(void *context);
} AiusCdcTeensyOps;

typedef struct {
    void *context;
    bool (*connected)(void *context);
    int (*read)(void *context);
    int (*available_for_write)(void *context);
    size_t (*write)(void *context, const uint8_t *data, size_t length);
    void (*enter_bootloader)(void *context);
} AiusCdcTeensyPrimitives;

typedef struct {
    AiusCdcTeensyPrimitives primitives;
    uint32_t protocol_faults;
    uint32_t last_fault;
} AiusCdcTeensy;

void aius_cdc_teensy_construct(
    AiusCdcTeensy *adapter,
    const AiusCdcTeensyPrimitives *primitives);

AiusCdcTeensyOps aius_cdc_teensy_ops(AiusCdcTeensy *adapter);

AiusCdcTeensyPrimitives aius_cdc_teensy_serial_primitives(void);

typedef struct {
    uint32_t boot_generation;
    uint32_t reset_source;
    uint8_t watchdog_early_resets;
} AiusRuntimeTeensyBootStatus;

typedef struct {
    void *context;
    bool (*reset_capture)(
        void *context,
        const AiusRuntimeBudget *budget,
        uint32_t now_ms,
        AiusRuntimeTeensyBootStatus *status);
    bool (*metrics)(
        void *context,
        const AiusRuntimeBudget *budget,
        uint32_t now_ms);
    bool (*udp)(
        void *context,
        const AiusRuntimeBudget *budget,
        uint32_t now_ms);
    bool (*replay)(
        void *context,
        const AiusRuntimeBudget *budget,
        uint32_t now_ms);
    void (*checkpoint)(void *context, uint16_t checkpoint);
    void (*feed_watchdog)(void *context);
} AiusRuntimeTeensyPlatformOps;

typedef struct {
    AiusNetSupervisor supervisor;
    AiusNetworkTeensy target;
    AiusHttpTransaction http;
    uint8_t http_request[AIUS_HTTP_MAX_REQUEST];
    uint8_t http_response[AIUS_HTTP_MAX_RESPONSE];
    AiusNetAction pending_network_action;
    AiusNetEvent pending_network_event;
    AiusHttpEvent pending_http_event;
    AiusHttpApplicationResponse pending_application_response;
    bool network_action_present;
    bool network_event_present;
    bool http_event_present;
    bool application_response_present;
} AiusRuntimeNetworkHttp;

typedef struct {
    AiusSdJob job;
    AiusSdTeensy target;
    uint8_t sd_transfer[512];
    AiusSdAction pending_action;
    AiusSdEvent pending_event;
    AiusHttpEvent pending_http_event;
    uint64_t http_action_id;
    AiusHttpActionKind http_action;
    bool action_present;
    bool event_present;
    bool http_event_present;
    bool http_authority;
} AiusRuntimeStorage;

typedef struct {
    AiusUsbRuntime runtime;
    AiusCdcTeensy adapter;
    char usb_response[320];
    char usb_verb[11];
    char usb_payload[257];
    AiusCdcTeensyOps ops;
    uint8_t usb_rx[64];
    size_t usb_rx_length;
    AiusIoEvent usb_write_event;
    AiusUsbApplicationResponse application_response;
    bool write_event_present;
    bool application_response_present;
    bool rx_terminal;
    bool bootloader_entered;
} AiusRuntimeUsb;

typedef struct {
    AiusRuntimeNetworkHttp network_http;
    AiusRuntimeStorage storage;
    AiusRuntimeUsb usb;
    AiusOpticalApp app;
    AiusRuntimeTeensyPlatformOps platform;
    AiusRuntimeTeensyBootStatus boot_status;
    uint16_t last_checkpoint;
    bool recovery_safe;
    uint32_t completed_epochs;
} AiusRuntimeTeensy;

void aius_runtime_network_http_construct(
    AiusRuntimeNetworkHttp *service,
    uint32_t now_ms,
    bool recovery_safe);

bool aius_runtime_network_service(
    AiusRuntimeNetworkHttp *service,
    const AiusRuntimeBudget *budget,
    uint32_t now_ms);

bool aius_runtime_http_service(
    AiusRuntimeNetworkHttp *service,
    const AiusRuntimeBudget *budget,
    uint32_t now_ms,
    AiusOpticalApp *app,
    const AiusHttpEvent *storage_event,
    AiusHttpAction *storage_action);

void aius_runtime_storage_construct(
    AiusRuntimeStorage *service);

bool aius_runtime_storage_service(
    AiusRuntimeStorage *service,
    const AiusRuntimeBudget *budget,
    uint32_t now_ms,
    const AiusHttpAction *http_action,
    AiusHttpEvent *http_event);

void aius_runtime_usb_construct(
    AiusRuntimeUsb *service,
    const AiusCdcTeensyPrimitives *primitives);

bool aius_runtime_usb_rx_service(
    AiusRuntimeUsb *service,
    const AiusRuntimeBudget *budget);

bool aius_runtime_usb_tx_service(
    AiusRuntimeUsb *service,
    const AiusRuntimeBudget *budget,
    uint32_t now_ms,
    AiusOpticalApp *app);

void aius_runtime_teensy_construct(
    AiusRuntimeTeensy *runtime,
    const AiusOpticalAppOps *app_ops,
    const AiusCdcTeensyPrimitives *cdc_primitives,
    const AiusRuntimeTeensyPlatformOps *platform_ops,
    uint32_t now_ms,
    bool recovery_safe);

bool aius_runtime_teensy_epoch(
    AiusRuntimeTeensy *runtime,
    uint32_t now_ms);

void aius_runtime_teensy_status(
    const AiusRuntimeTeensy *runtime,
    AiusRecoveryStatus *status);
```

Each service constructor initializes memory only. Network construction retains
the supervisor's initial action in `pending_network_action`, passes
`recovery_safe` to `aius_net_supervisor_init()`, and performs no target call.
The `NETWORK` checkpoint uses this strict one-operation precedence:

1. A pending supervisor action, or an adapter-retained multi-call action, calls
   `aius_network_teensy_apply()` once (`NONE` advances retained work) and stores
   its event for a later epoch.
2. Otherwise a pending network event calls `aius_net_supervisor_step()` once
   and stores its action for a later epoch.
3. Otherwise `aius_network_teensy_poll()` runs once and stores its event.

It never applies, steps, and polls in the same epoch. Each service function
otherwise consumes at most one retained input,
executes the exact supplied budget, and publishes at most one retained output.
The HTTP/storage bridge is explicit: root passes one storage event into
`aius_runtime_http_service()` and one returned SD-backed HTTP action into
`aius_runtime_storage_service()`; their action ID remains retained until the
storage service returns the correlated HTTP event. No service calls another
service or a scheduler callback.

`aius_cdc_teensy` owns the only production `Serial` mapping. `connected()`
returns the primitive connection sample. `read_byte()` returns `CLOSED` when
disconnected, maps primitive `-1` to `WOULD_BLOCK`, maps `0..255` to
`PROGRESS(bytes=1)` and stores that byte, and maps every other sentinel to
`FAULT(AIUS_CDC_ADAPTER_PROTOCOL_FAULT)` after incrementing the adapter's
saturating `protocol_faults` and storing the sentinel in `last_fault`.
`write_capacity()` maps primitive
values `<=0` to zero and clamps positive values to 64. `write()` requires an
offered length in `1..64`, returns `CLOSED` when disconnected, maps primitive
zero to `WOULD_BLOCK`, `1..offered` to `PROGRESS`, and an oversized result to
the protocol sentinel fault with the same one-time adapter diagnostic update.
Production `Serial` exposes no separate driver
fault; the fake primitive covers every normalized result. The sentinel and
adapter types live in `aius_cdc_teensy.h`, which Phase 12 owns.
`aius_cdc_teensy_serial_primitives()` lives in `aius_cdc_teensy.cpp`, is the
only function in the new target runtime that mentions `Serial`, and returns the
production primitive table used by Phase 14. Tests construct the same adapter
with fake primitives.

The USB application callback writes a NUL-terminated verb and payload within
the dedicated `11`- and `257`-byte buffers. `aius_optical_usb_dispatch()`
bounded-scans those buffers, sets the explicit lengths, and rejects missing
terminators or a verb inconsistent with the command.

Construction initializes memory only, copies the platform callbacks, constructs
the CDC adapter from the supplied primitives, and retains `recovery_safe` as
the scheduler mode for every epoch. Null callbacks or primitives make the
composition invalid; the epoch then returns false without checkpoint or feed.
The root's scheduler callback table is fixed: `RESET_CAPTURE`, `METRICS`,
`UDP`, and `REPLAY` invoke the same-named platform callback; `USB_RX` and
`USB_TX` invoke the two USB service functions; `NETWORK` and `HTTP` invoke the
two network/HTTP service functions; and `SD` invokes the storage service.
Before forwarding each checkpoint the root stores it in `last_checkpoint`.
`RESET_CAPTURE` fully initializes and returns `boot_status`; failure stops the
epoch without publishing a partial capture. `aius_runtime_teensy_status()` is
a pure snapshot: it combines retained `boot_status` and `last_checkpoint` with
network/runtime counters and performs no platform call. Checkpoint and feed
otherwise forward only to the platform callbacks. Native fakes
assert each callback, exact budget, retained input/output flag transition, and
failure short-circuit.
`completed_epochs` increments saturating at `UINT32_MAX` only when
`aius_runtime_epoch()` returns true after invoking the sole feed callback.
Invalid, incomplete, or service-failed epochs do not increment it.
`USB_RX` is the explicit exception to
the one-adapter-event service rule. While connected, it performs at most the
lesser of 64 one-byte CDC read attempts and available space in the retained
64-byte staging buffer; it does not call the portable USB step. Only
`PROGRESS(bytes=1, fault=0)` appends a byte. `WOULD_BLOCK(bytes=0, fault=0)`
stops cleanly. `CLOSED(bytes=0, fault=0)` or
`FAULT(bytes=0, fault!=0)` stops, marks the connection unavailable for that
epoch, clears staged RX, and stores the driver fault in
`usb.adapter.last_fault`.
`CLOSED` stores zero. Any other result is an adapter protocol fault: it follows
the same stop/clear behavior, stores `AIUS_CDC_ADAPTER_PROTOCOL_FAULT`, and
increments saturating `usb.adapter.protocol_faults`. A valid normalized
`FAULT`, including the sentinel returned by the adapter, is not counted again;
write-side raw oversize is diagnosed solely inside `aius_cdc_teensy::write()`.
Disconnection clears staged RX.
At `USB_TX`, it moves and clears the
prior retained application-response and write-event presence flags, samples
the bounded `write_capacity`, then calls
`aius_usb_runtime_step()` exactly once with staged RX, retained application
response/write result, the current connection sample combined with any
`USB_RX` terminal result, and current write capacity;
removes exactly `rx_consumed` bytes from staging; and handles at most one
returned action. `DISPATCH` is invoked synchronously and its response retained
for the next epoch. `WRITE` performs one nonblocking primitive call and retains
its `AiusIoEvent`. Only these newly produced results set the cleared presence
flags. If the `USB_TX` connection sample is false it clears staged RX again,
covering loss between checkpoints. `ENTER_BOOTLOADER` invokes the callback
once.

The epoch otherwise supplies exact portable budgets, routes at most one
adapter event/action per service, calls
`aius_network_teensy_apply()` with `NONE` when the supervisor emits no new
action so retained publish/teardown work advances, and returns the portable
scheduler result. It does not feed the watchdog itself; final composition
supplies the sole-feed callback. Generation teardown discards the HTTP
transaction before adapter cleanup. Native target-service tests use fake
network, SD, CDC, clock, checkpoint, and watchdog primitives to cover every
callback result and exact call count. Phase 7 does not edit `optical_main.cpp`
or `platformio.ini` and promises only the portable engine. The executable
checkpoint mapping above is delivered in Phase 12; the synchronous CDC
drain/flush/delay path remains until Phase 14. The Phase 7 optical build proves
compile/link compatibility only, not deployed runtime behavior.

## Status compatibility contract

The UDP payload remains version 1 and keeps its existing first 71 bytes
unchanged. New firmware emits 99 bytes. Old runners may ignore the suffix; the
new runner accepts both 71 and 99 bytes. Network mode `2` is parsed as
`legacy-static` for old evidence but is never emitted.

```c
typedef struct {
    AiusNetState network_state;
    bool recovery_safe;
    AiusNetFault network_last_fault;
    uint16_t last_checkpoint;
    uint32_t network_generation;
    uint32_t boot_generation;
    uint32_t reset_source;
    uint8_t watchdog_early_resets;
    uint32_t loop_epoch;
    uint32_t network_restarts;
} AiusRecoveryStatus;

size_t aius_net_status_encode(
    uint8_t *output,
    size_t capacity,
    const uint8_t legacy_status[71],
    const AiusRecoveryStatus *recovery);
```

`AiusRecoveryStatus` is declared in `aius_net.h` before target-composition
headers consume it. The encoder returns zero on invalid input or capacity and
otherwise copies the exact legacy bytes before appending the suffix.

| Offset | Size | Field |
|---|---|---|
| 0–70 | 71 | unchanged version-1 status |
| 71 | 1 | supervisor state |
| 72 | 1 | recovery-safe flag |
| 73 | 1 | `AiusNetFault` value |
| 74 | 1 | runtime checkpoint value `0..9` |
| 75 | 4 | network resource generation, little-endian |
| 79 | 4 | boot generation, little-endian |
| 83 | 4 | raw reset source, little-endian |
| 87 | 1 | consecutive early watchdog resets |
| 88 | 3 | zero reserved bytes |
| 91 | 4 | completed loop epoch count, little-endian |
| 95 | 4 | supervisor restart count, little-endian |

Commit golden 71-byte and 99-byte fixtures before target integration. HTTP JSON
and USB `ID` use these exact names: `network_state`, `network_generation`,
`network_last_fault`, `recovery_safe`, `boot_generation`, `reset_source`,
`watchdog_early_resets`, `last_checkpoint`, `loop_epoch`, and
`network_restarts`.

The contract fixtures are immutable inputs, not generated by either
implementation:

- `firmware/test/fixtures/status/status-v1-71.bin`
- `firmware/test/fixtures/status/status-v1-recovery-99.bin`

Bytes `0..70` of the 99-byte fixture equal the complete 71-byte fixture. The
71-byte fixture has this canonical legacy value set:

| Legacy field | Canonical value |
|---|---:|
| version / network mode / state / flags | `1 / 1 / 4 / 3` |
| boot epoch / uptime | `0x01020304 / 0x11121314` |
| IP / MAC / NTP valid | `192.0.2.45 / 02:41:49:55:a5:5a / true` |
| format / pipeline status / sample rate | `2 / 127 / 48000` |
| pipeline blocks / overruns | `0x21222324 / 0x31323334` |
| EQ / mix | `2 / 1` |
| event sequence / overflows | `0x41424344 / 0x51525354` |
| network drops / budget overruns | `0x61626364 / 0x71727374` |
| HTTP requests / rejections | `0x81828384 / 0x91929394` |
| HTTP client drops / service overruns | `0xa1a2a3a4 / 0xb1b2b3b4` |

Its exact hexadecimal bytes are:

```text
01 01 04 03 04 03 02 01 14 13 12 11 c0 00 02 2d
02 41 49 55 a5 5a 01 02 7f 80 bb 00 00 24 23 22
21 34 33 32 31 02 01 44 43 42 41 54 53 52 51 64
63 62 61 74 73 72 71 84 83 82 81 94 93 92 91 a4
a3 a2 a1 b4 b3 b2 b1
```

The recovery suffix uses these canonical values:

| Recovery field | Canonical value |
|---|---:|
| network state / recovery safe / last fault / checkpoint | `5 / true / 10 / 9` |
| network generation | `0xc1c2c3c4` |
| boot generation | `0xd1d2d3d4` |
| reset source / early resets | `0xe1e2e3e4 / 254` |
| reserved | `00 00 00` |
| loop epoch / network restarts | `0xf1f2f3f4 / 0x01020305` |

Its exact appended hexadecimal bytes are:

```text
05 01 0a 09 c4 c3 c2 c1 d4 d3 d2 d1 e4 e3 e2 e1
fe 00 00 00 f4 f3 f2 f1 05 03 02 01
```

The C proof encodes the canonical legacy fixture and recovery values, compares
all 99 bytes with the precommitted extended fixture, and independently compares
the first 71 bytes with the legacy fixture. The Python proof parses each
precommitted fixture to a fully specified expected mapping. Neither proof
generates expected bytes from the code under test.

`AiusNetFault` is the authoritative fault domain and is limited to its declared
`0..10` values; target-native error codes are mapped at the adapter boundary
and never enter status. Runtime checkpoints are limited to `0..9`. The encoder
rejects null output, legacy input, or recovery input; capacity `0..98`; a
legacy version byte other than `1`; a legacy network-mode byte outside
`{0,1}`; supervisor state outside `0..5`; fault outside `0..10`; or checkpoint
outside `0..9`. Rejection returns zero without modifying any output byte.
Valid state/fault/checkpoint endpoints `0/5`, `0/10`, and `0/9` encode, and
`UINT32_MAX` counters serialize without saturation or truncation. Mode `2`
therefore remains decodable as historical evidence but is never emitted by the
99-byte encoder.

The Python parser accepts exactly 71 or 99 bytes and requires legacy version
`1`. Existing legacy keys and value types remain unchanged. Mode `2` parses as
`legacy-static`; this intentionally replaces the current `static-fallback`
label for historical evidence. For a 99-byte payload, `network_state`,
`network_generation`, `network_last_fault`, `boot_generation`, `reset_source`,
`watchdog_early_resets`, `last_checkpoint`, `loop_epoch`, and
`network_restarts` are integers, while `recovery_safe` is `bool`. It rejects
every other payload length; version values other than `1`; supervisor states
`6` and `255`; recovery-safe values `2` and `255`; faults `11` and `255`;
checkpoints `10` and `255`; and each of the three reserved bytes when nonzero,
tested independently. Valid domain endpoints parse. No saturation or
truncation is permitted.

## Live smoke evidence

The proportionate live smoke (Phase 16) records a short human-readable summary
per run under `firmware/test/evidence/network-hardening/`: the deployed build
revision and short build id, the gate exercised, pass/fail, and the observed
recovery status fields (boot generation, reset source, early watchdog resets,
last checkpoint, network state/generation, ready latencies). No canonical-hash
receipt chain, RFC 8785 canonicalization, or LFS artifact matrix is required;
this is a personal single-device smoke, not a certification record.

## Implementation phases

Each phase is one concern and ends green, buildable, committed, and pushed. A
sub-agent edits/tests only its owned paths; the parent updates this plan, runs
the declared proof, commits explicit paths, and pushes.

**Gate for the remaining phases (11, 12, 14, 14b, 15, 16): green native suite
plus an owned-diff review — nothing heavier.** The per-phase source-blind oracle
chains and per-phase clean-room delivery audits described in the delivered-phase
blocks below (Phases 1–10) are historical; they are not re-run and are not
required for the remaining cutover and smoke work. Phases 1–10, Phase 10a, and
the Phase 13 code baseline are delivered and must not be re-reviewed.

The phases appear below in numeric order; the delivery order is the shared
critical path at the top of this document (B1 → B2 → B3 → B4 → B5 → B6 → B7 →
B8, i.e. 11, 12, 14, 14b, OpenAPI, 16a, 15, 16b).

### Phase 1 — Pure network supervisor

```yaml
unit: phase-task
status: completed
seams: [aius_net_supervisor_init, aius_net_supervisor_step]
contract-artifact: spec-only
dependencies: []
outcome: "A pure supervisor owns all network generations, deadlines, and recovery."
acceptance:
  - exhaustive state and event matrix matches the normative transition oracle
  - stale generations and duplicate callbacks never publish readiness
  - generated sequences preserve one-live-generation and progress invariants
fan-out:
  - name: agent-network-supervisor
    owned-paths:
      - firmware/lib/aius_net/include/aius_io.h
      - firmware/lib/aius_net/include/aius_net.h
      - firmware/lib/aius_net/src/aius_net.c
      - firmware/test/net/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite net
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): add deterministic network supervisor"
```

Implement exactly the supervisor ABI and exhaustive oracle above. Do not touch
target lifecycle code in this phase.

### Phase 2 — Portable boot ledger

```yaml
unit: component-behind-interface
status: completed
seams: [aius_boot_init, aius_boot_record_checkpoint, aius_boot_mark_healthy]
contract-artifact: spec-only
dependencies: []
outcome: "Portable reset history deterministically selects normal or recovery-safe mode."
acceptance:
  - CRC, magic, version, build change, and every reset kind are covered
  - boot generation is monotonic and counters saturate
  - healthy clear and checkpoint persistence match the boot ABI
fan-out:
  - name: agent-boot-ledger
    owned-paths:
      - firmware/lib/aius_boot/
      - firmware/test/boot/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite boot
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): add portable boot recovery ledger"
```

Cover CRC/magic/version, build change, reset causes, saturation, healthy clear,
and checkpoint persistence semantics. No Teensy register access lands here.

### Phase 3 — Pinned one-shot NativeEthernet

```yaml
unit: refactor-target
status: completed
seams: [beginNoWait, dhcpStart, dhcpStop, EthernetClient.write, EthernetServer.end, Ethernet.end]
contract-artifact: spec-only
dependencies: []
outcome: "The selected NativeEthernet source exposes only one-shot lifecycle primitives."
acceptance:
  - vendored source and license provenance match framework 1.162.0
  - immutable upstream/patched hashes identify every snapshotted file
  - source guard rejects unbounded waits in every target-facing primitive
  - source guard permits only the designated adapter owner and exact legacy
    optical_main.cpp exception until phase 14
  - fake FNET failures prove partial initialization can tear down and retry
  - fake callbacks prove unregister-before-close and late-event rejection
  - begin, write, server end, and stack end each return after one attempt
  - target metadata and symbols prove PlatformIO selected the patched snapshot
fan-out:
  - name: agent-nativeethernet-oneshot
    owned-paths:
      - firmware/vendor/NativeEthernet/
      - firmware/test/network-transport/
      - firmware/platformio.ini
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite network-transport
  buildable:
    - teensy41-optical
  parent-commit: "fix(aius): pin one-shot NativeEthernet primitives"
```

Snapshot only the required library source/license, record provenance, apply the
specified bounded patches, select the snapshot in PlatformIO, and prove the
target-facing source guard.

### Phase 4 — Teensy network adapter

```yaml
unit: component-behind-interface
status: completed
seams: [AiusNetworkTeensy, AiusIoResult, AiusNetAction, close_once]
contract-artifact: spec-only
dependencies: [phase-1, phase-3]
outcome: "One adapter owns the target stack and emits generation-scoped events."
acceptance:
  - fake matrix covers every action, resource fault, and teardown fault
  - publish and teardown attempt at most one target primitive per call
  - teardown preempts publish and reaches terminal cleanup despite every fault
  - accept, read, write, close, and UDP mappings cover progress, block, close, and fault
  - poll emits one queued event, rejects stale callbacks, and delivers duplicates for supervisor rejection
  - callback overflow becomes one current-generation resource fault
fan-out:
  - name: agent-teensy-network-adapter
    owned-paths:
      - firmware/src/aius_network_teensy.cpp
      - firmware/src/aius_network_teensy.h
      - firmware/test/network-transport/
      - firmware/platformio.ini
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite network-transport
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): add bounded Teensy network adapter"
```

Use a fake primitive table for the resource/fault matrix. Compile the real
adapter into the target but leave legacy runtime composition intact.

### Phase 5 — Bounded SD jobs

```yaml
unit: phase-task
status: completed
seams: [aius_sd_job_step, aius_sd_job_supply, AiusSdAction, AiusSdResult]
contract-artifact: spec-only
dependencies: [phase-1]
outcome: "An unused portable SD job module represents bounded, correlated, cancellable work."
acceptance:
  - the plan-owned state/action/event oracle is exhaustive for all seven job kinds
  - all three upload modes preserve the normative install/cancel path authority
  - output/config buffer ownership survives partial I/O and correlated acknowledgement
  - invalid paths, capacity, arithmetic, deadlines, and events fail with stable local faults
fan-out:
  - name: agent-sd-job-engine
    owned-paths:
      - firmware/lib/aius_sd/include/aius_sd.h
      - firmware/lib/aius_sd/src/aius_sd.c
  - name: agent-sd-independent-oracle
    owned-paths:
      - firmware/test/sd/test_aius_sd.c
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite sd
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "feat(aius): add bounded SD job state machine"
```

Run the slices sequentially. The engine agent implements only the public header
and state engine from the committed plan. After the parent verifies that header
against the plan, a fresh oracle agent reads the plan and public header but not
the engine source, then authors the independent transition/model/property
suite and runner registration. Implement no target adapter. The `File`
adapter, `optical_main.cpp` wiring, and removal of legacy synchronous SD calls
remain Phases 10 and 14.

### Phase 6 — Stateful HTTP transactions

```yaml
unit: component-behind-interface
status: completed
seams: [aius_http_transaction_step, AiusIoResult, bounded SD job]
contract-artifact: spec-only
dependencies: [phase-1, phase-5]
outcome: "One HTTP transaction bounds transport, application dispatch, and SD progress."
acceptance:
  - every transport and application failure state closes exactly once
  - dispatch occurs exactly once and response overflow is deterministic
  - partial RX, TX, and SD progress preserve exact offsets and deadlines
fan-out:
  - name: agent-http-codec-framing
    owned-paths:
      - firmware/lib/aius_http/include/aius_http.h
      - firmware/lib/aius_http/include/aius_http_transaction.h
      - firmware/lib/aius_http/src/aius_http.c
  - name: agent-http-transaction-engine
    owned-paths:
      - firmware/lib/aius_http/src/aius_http_transaction.c
  - name: agent-http-target-build-wiring
    owned-paths:
      - firmware/platformio.ini
  - name: agent-http-independent-oracle
    owned-paths:
      - firmware/test/http/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite http
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "feat(aius): model bounded HTTP transactions"
```

Run the slices sequentially. The codec slice lands the reviewed public framing
header and narrow transaction header. The engine slice implements only the new
transaction source. The target-build slice keeps `platformio.ini` as the
single source of truth by explicitly compiling `aius_http.c`,
`aius_http_transaction.c`, and `aius_sd.c`, exposing their include directories,
and excluding `aius_http` and `aius_sd` from PlatformIO library discovery. After
the parent verifies that implementation against the committed headers and the
target build passes, a fresh oracle agent reads the plan and public headers but
not either implementation source, then authors the independent
transition/model/property suite and runner registration. Phase 6 does not edit
`optical_main.cpp`, add a Teensy SD adapter, or cut over the runtime; those
remain later phases.

### Phase 7 — Bounded CDC recovery runtime

```yaml
unit: component-behind-interface
status: completed
seams: [aius_usb_runtime_step, AIUS_USB_COMMAND_BOOTLOADER]
contract-artifact: spec-only
dependencies: [phase-1]
outcome: "CDC parsing, application dispatch, TX, and bootloader entry are quota-bound."
acceptance:
  - continuous RX and partial or zero-capacity TX remain within quotas
  - every command dispatches exactly once with deterministic overflow behavior
  - acknowledgement completion, loss, deadline equality, and wrap select HalfKay once
fan-out:
  - name: agent-usb-runtime-engine
    owned-paths:
      - firmware/lib/aius_usb_control/include/aius_usb_control.h
      - firmware/lib/aius_usb_control/src/aius_usb_control.c
  - name: agent-usb-independent-oracle
    owned-paths:
      - firmware/test/usb-control/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite usb-control
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "feat(aius): bound the USB recovery runtime"
```

Run the slices sequentially. The engine slice implements the reviewed public
header and state engine. After the parent verifies that header against the
committed contract, a fresh oracle agent reads the plan and public header but
not the implementation source, then authors the independent transition model
and runner registration. Target composition still uses the old adapter until
Phase 14.

### Phase 8 — Portable runtime scheduler

```yaml
unit: component-behind-interface
status: completed
seams: [aius_runtime_epoch, AiusRuntimeOps, boot checkpoints]
contract-artifact: spec-only
dependencies: [phase-1, phase-2]
outcome: "One portable epoch supplies budgets and enforces order, selection, checkpoints, and feed eligibility."
acceptance:
  - immutable budgets exactly match the normative table
  - normal and recovery-safe service order and call counts are exact
  - early return records the failing checkpoint and never feeds
fan-out:
  - name: agent-runtime-scheduler-engine
    owned-paths:
      - firmware/lib/aius_runtime/
      - firmware/platformio.ini
  - name: agent-runtime-scheduler-independent-oracle
    owned-paths:
      - firmware/test/runtime/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite runtime
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "feat(aius): add deterministic runtime epochs"
```

Run the slices sequentially. The engine slice lands the reviewed public header,
stateless scheduler, and explicit optical source/include wiring. After the
parent verifies the header, a fresh source-blind oracle reads only the committed
contract and public header, then implements the exact registries and runner
registration without reading the scheduler source. Prove service order,
watchdog eligibility, early-return behaviour, and recovery-safe selection;
construction and network-generation behavior remain in their owning phases.

### Phase 9 — Status schema compatibility

```yaml
unit: phase-task
status: completed
seams: [aius_net_status_encode, parse_status]
deferred-consumers: [put_status, HTTP status JSON, USB ID]
contract-artifact: both
dependencies: [phase-1, phase-2]
outcome: "Old and new status consumers share one byte-exact recovery schema."
acceptance:
  - C encoding of canonical values byte-matches the precommitted 99-byte fixture
  - Python parses the precommitted 71-byte and 99-byte fixtures to exact mappings
  - all enumerated invalid domains reject without output mutation or narrowing
  - old payloads retain their first 71 bytes and mode 2 parses as legacy-static
fan-out:
  - name: agent-status-encoder
    owned-paths:
      - firmware/lib/aius_net/include/aius_net.h
      - firmware/lib/aius_net/src/aius_net.c
      - firmware/test/net/test_net.c
  - name: agent-status-runner
    owned-paths:
      - firmware/tools/net/aius_net_runner.py
      - firmware/test/net/test_net_runner.py
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite net
    - firmware/scripts/test.sh --environment native --suite net-runner
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): version recovery status telemetry"
```

The parent precommits the two exact LFS fixtures before either implementation
slice starts. Then run the disjoint C encoder and Python runner slices against
those read-only contract artifacts. Existing `net` and `net-runner` suite
registrations need no edit. Phase 9 does not edit `optical_main.cpp` or any
target composition; target UDP emission, HTTP JSON, and USB `ID` wiring remain
owned exclusively by Phase 14.

### Phase 10 — One-shot Teensy SD adapter

```yaml
unit: phase-task
seams: [aius_sd_job_step, AiusSdExistencePurpose, aius_sd_teensy_execute, Teensy SD and File]
contract-artifact: spec-only
dependencies: [phase-5]
status: completed
outcome: "Portable idempotency and truncation edges drive one observable target primitive per epoch."
acceptance:
  - every idempotent mutation is preceded by one correlated existence observation
  - write-begin and truncate are separate correlated actions before upload input
  - required delete distinguishes a missing target from idempotent absence without a preflight mutation
  - real adapter source maps the fixed action catalog to one storage primitive
  - source guard and mutation self-tests reject loops, retries, recursion, and delay
  - strict C and C++17 ASan/UBSan suites pass with exact event and authority fields
  - optical target compiles the adapter without constructing or calling it
fan-out:
  - name: agent-sd-portable-idempotency-engine
    owned-paths:
      - firmware/lib/aius_sd/include/aius_sd.h
      - firmware/lib/aius_sd/src/aius_sd.c
  - name: agent-sd-portable-idempotency-oracle
    owned-paths:
      - firmware/test/sd/test_aius_sd.c
  - name: agent-http-required-delete-compatibility
    owned-paths:
      - firmware/lib/aius_http/src/aius_http_transaction.c
  - name: agent-http-required-delete-independent-oracle
    owned-paths:
      - firmware/test/http/test_http_transaction.c
  - name: agent-teensy-sd-adapter
    owned-paths:
      - firmware/src/aius_sd_teensy.cpp
      - firmware/src/aius_sd_teensy.h
      - firmware/platformio.ini
  - name: agent-teensy-sd-independent-oracle
    owned-paths:
      - firmware/test/sd-target/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite sd
    - firmware/scripts/test.sh --environment native --suite http
    - firmware/scripts/test.sh --environment native --suite sd-target
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "feat(aius): add one-shot Teensy SD adapter"
```

Run all six slices sequentially. First amend the portable engine, then have a
fresh source-blind oracle read only the committed contract and public header
and strengthen the existing SD registry for every existence purpose, both bool
branches, malformed correlations, cancellation/deadline precedence, and the
write-begin/truncate authority edge. It also proves the exact 262-ID reserve
with the worst-case one-byte post-commit publication tail, the just-below
boundary abort before commit, authority-free start rejection above the
threshold, equality acceptance followed by pre-mutation overflow, BEGIN fault
with zero cleanup actions, cleanup use of the reserve, `UINT64_MAX` saturation,
and no ID reuse. After the parent verifies that header,
the HTTP compatibility slice requires `require_existing == false` for every
non-delete SD request, includes it in the all-zero immediate-response check,
accepts either value only for `DELETE`, and maps `NOT_FOUND` to exact status
404 body `{"ok":false,"error":"sd_not_found"}`. The eventual Phase 11 HTTP
DELETE dispatcher must set `require_existing = true`; internal idempotent
deletes explicitly use false. A fresh source-blind HTTP oracle adds the
non-delete rejection matrix, immediate-response zero-field rejection,
required-delete acceptance, and exact terminal 404 golden before the adapter
slice starts. The adapter slice implements the fixed C++ ABI and adds
`aius_sd_teensy.cpp` to the optical PlatformIO source filter without editing
`optical_main.cpp`. Finally a fresh adapter-source-blind oracle compiles the
real source against the fake `SD.h` contract and owns the runner registration.
No slice constructs or invokes the adapter in production before Phase 14.

### Phase 10a — SD job publish-capacity generalization

```yaml
unit: parent-only
status: completed
seams: [AiusSdJobRequest, AiusSdJob, aius_http_transaction sd_request_fields_valid]
contract-artifact: stubs-committed
dependencies: [phase-5, phase-6]
outcome: "The delivered SD job publish mechanism accepts any caller-supplied byte limit, not just the feature-flag config's 256 bytes."
acceptance:
  - aius_sd.c has no remaining dependency on aius_config.h or AIUS_CONFIG_MAX_JSON_BYTES
  - every publish-capacity bound check (start validation, CONFIG_LOAD, UPLOAD/REPLACE, UPLOAD/FINALIZE_PART, post-load size, post-install re-read size) uses the caller-supplied publish_capacity
  - publish without a matching nonzero publish_capacity, and publish_capacity set without publish, are both AIUS_SD_FAULT_INVALID_REQUEST
  - existing sd and http-transaction suites are green against the renamed field with no behavioural change for the feature-flag config caller (still passes AIUS_CONFIG_MAX_JSON_BYTES explicitly)
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite sd
    - firmware/scripts/test.sh --environment native --suite http
  buildable:
    - teensy41-optical
  parent-commit: "refactor(aius): generalize the SD job publish mechanism beyond the feature-flag config"
```

Discovered while resuming the DSP config API's PUT `/api/dsp/config` work
([`aius-dsp-config-api.md`](aius-dsp-config-api.md)): the delivered
`AiusSdJobRequest.publish_config` mechanism (Phase 5, `aius_sd.c`) and its
mirrored validation (Phase 6, `aius_http_transaction.c`'s
`sd_request_fields_valid()`) are structurally generic — they open the
just-installed or just-loaded file, read it, and hand the complete bytes to
the caller through one `AIUS_SD_ACTION_PUBLISH_CONFIG` action — but the byte
limit itself was hardcoded to `AIUS_CONFIG_MAX_JSON_BYTES` (256, the
feature-flag config's own limit) in both files, once as an `#include
"aius_config.h"` dependency in the otherwise-portable `aius_sd.c`, once as a
locally duplicated `CONFIG_PUBLICATION_MAX` constant in
`aius_http_transaction.c`. The DSP strand needs a second publish caller capped
at `AIUS_DSP_MODEL_MAX_JSON_BYTES` = 12288, so the limit becomes a request field
instead of a compile-time constant.

**Correction to the original framing:** that second caller is *not* the DSP
config `PUT`, as this block first claimed. The `PUT` parses and validates the
model from the HTTP body itself and writes the accepted bytes verbatim, so its
`UPLOAD/REPLACE` job runs with `publish = false`. The publish caller is the
**boot / hot-reinsert load** of `/aius/dsp.json`, which Phase 14 moves onto a
portable `CONFIG_LOAD` job with `publish_capacity = 12288`. The generalization
is required either way; only its justification changes.

`bool publish_config` becomes `bool publish` (same meaning, generic name) plus
a new `size_t publish_capacity` in both `AiusSdJobRequest` and `AiusSdJob`.
Every occurrence of `AIUS_CONFIG_MAX_JSON_BYTES` inside `aius_sd.c` (request
validation, the `CONFIG_LOAD` capacity check, `UPLOAD/REPLACE` and
`UPLOAD/FINALIZE_PART` total/final-size checks, the post-load
`AIUS_SD_PHASE_CONFIG_SIZE` check, and the post-install
`AIUS_SD_PHASE_INSTALL_PUBLISH_SIZE` re-read check) is replaced by
`request->publish_capacity` or `job->publish_capacity`; the file drops its
`aius_config.h` include entirely, removing a coupling the portable SD job
module never needed. `aius_http_transaction.c`'s `CONFIG_PUBLICATION_MAX`
constant and its `publish_config` references get the same treatment. New
validation makes the pairing explicit rather than implicit:
`publish && publish_capacity == 0` and `!publish && publish_capacity != 0` are
both `AIUS_SD_FAULT_INVALID_REQUEST`, matching this module's existing
not-implicit field discipline. `AIUS_SD_ACTION_PUBLISH_CONFIG` and the
`*_PUBLISH*` phase/state names keep their current (feature-config-flavored)
names — renaming enum identifiers throughout an already-delivered, tested
state machine buys nothing functional and inflates the diff; only their doc
comments note the mechanism is now generic. Existing callers (the
feature-flag config's `CONFIG_LOAD`/`UPLOAD` requests) pass
`publish_capacity = AIUS_CONFIG_MAX_JSON_BYTES` explicitly and are otherwise
behaviourally unchanged. `aius_sd_teensy.cpp` (Phase 10) needs no change — it
only implements one-shot file I/O primitives and never references
`publish`/`publish_capacity` directly.

Touches: `lib/aius_sd/include/aius_sd.h`, `lib/aius_sd/src/aius_sd.c`,
`lib/aius_http/src/aius_http_transaction.c`, `test/sd/test_aius_sd.c`,
`test/http/test_http_transaction.c`. Not touched: `aius_sd_teensy.cpp`/`.h`,
`test/sd-target/`, `optical_main.cpp` (its own `config_upload`/`dsp_upload`
ad-hoc SD-job bookkeeping predates this ABI and is superseded by Phase 14's
cutover onto the portable `AiusSdJob`, not by this phase).

**Delivered** (`df9ea4483`). The rename, the caller-supplied capacity at every
bound check, the dropped `aius_config.h` coupling, and the explicit
publish/`publish_capacity` pairing all landed; the `sd` suite's
invalid-request matrix gained a case for each half of the pairing. `sd` (64
fixtures, 1001 matrix trials, 27 lifecycle cases) and `http` are green and
`teensy41-optical` builds with all banks fitting. Note for the phases that
follow: that build reports **ITCM 99.31 % used, 1,352 bytes free** and RAM1
free-for-locals 22,684 B.

### Phase 11 — Optical application dispatch

```yaml
unit: component-behind-interface
seams: [aius_optical_http_dispatch, aius_optical_usb_dispatch, fake app callbacks]
contract-artifact: spec-only
dependencies: [phase-5, phase-6, phase-7, phase-9, phase-10a]
outcome: "Every HTTP route and USB command has exactly one bounded application dispatch."
acceptance:
  - every current route and command has a golden response
  - the route inventory includes GET /api/dsp/config, PUT /api/dsp/config, and GET /api/meters
  - callback failure and buffer exhaustion have deterministic errors
  - dispatch performs no transport, SD, watchdog, or delay call
fan-out:
  - name: agent-optical-app-dispatch
    owned-paths:
      - firmware/src/aius_optical_app.cpp
      - firmware/src/aius_optical_app.h
      - firmware/test/optical-app/
      - firmware/platformio.ini
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite optical-app
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): isolate optical application dispatch"
```

**The route inventory is the current routes plus the three DSP routes.** The DSP
strand added `GET /api/dsp/config` and `PUT /api/dsp/config` to
`optical_main.cpp` after this phase was written, and `GET /api/meters` lands in
Phase 14b. Declaring all three here means the dispatcher is written once instead
of being extended twice within three phases. Their behaviour is specified in
[`aius-dsp-config-api.md`](aius-dsp-config-api.md) §Contract decisions:
`PUT` requires `X-Aius-SD-Auth`, validates the bounded body through
`aius_dsp_config_parse_put()`, returns 409 on a stale `base_generation` without
touching SD or the active snapshot, and hands the accepted bytes to an SD job.
Dispatch itself stays pure — the SD job and the block-boundary apply belong to
Phases 12 and 14, not here.

Cold-code discipline is not optional at this point: ITCM has 1,352 bytes free.
Every dispatcher entry point is `FLASHMEM`, applied on the definition rather
than a prototype.

**Delivered.** `src/aius_optical_app.{h,cpp}` carry an `AiusOpticalApp` callback
table (20 required entries, each rejected at init when null) and a dispatcher
returning either an immediate response or an SD job request, for all 19 route
enumerators and all four USB commands plus the `NONE` case. The `optical-app`
suite (14 groups) asserts route coverage exhaustively, status/content-type/body
per decision, SD job requests field-by-field against `aius_sd.c`'s validation,
path-traversal and encoding rejection, refusal ordering, serializer exhaustion on
all seven serializers, and per-route callback isolation. `http` is green
unmodified. `AIUS_HTTP_ROUTE_METERS` is added to the enum and parser (GET-only)
so Phase 14b lands only a handler body. The unreferenced
`AIUS_HTTP_ROUTE_REPLAYS_DELETE` enumerator is removed — the parser never
produced it (DELETE on `/api/replays` is `REPLAYS` plus the DELETE method) and it
had zero references anywhere in the tree.

The FLASHMEM discipline holds: `arm-none-eabi-size -A` on the new object reports
`.text 0`, `.flashmem 3976`, `.rodata.str1.4 1275` — **zero ITCM**. The linked
image is currently byte-identical with and without the module, because nothing
references it yet, so those bytes are not in the bank report below and will
appear when Phase 12 constructs it.

**A linker fact worth carrying into Phases 12, 14, and 14b:** `imxrt1062_t41.ld`
maps `*(.flashmem*)` to FLASH but `*(.rodata*)` to **DTCM**. So `FLASHMEM` moves
code out of ITCM while its string literals still land in the 22,684-byte
free-for-locals zone.

**Convention that follows from it, and that later phases should copy:** store
the error **token** only and emit `{"ok":false,"error":"…"}` from a single
`http_error(dispatcher, status, token)` that formats into the caller-supplied
response buffer. Deduplicating literals buys RAM that `FLASHMEM` cannot.

Applied to this dispatcher it moved `.rodata.str1.4` from 1,275 to **907 bytes**
(−368) and `.flashmem` from 3,976 to **3,628** (−348), across 30 call sites and
17 distinct tokens, with every response byte unchanged — the suite's exact-body
assertions were left hardcoded rather than loosened, and the two tokens it never
reached (`sd_method`, `replay_method`, both unreachable defensive 405 defaults
because the parser rejects those methods first) gained a test that asserts their
full envelopes byte-for-byte.

**The 368 bytes are banked, not yet realised.** `arm-none-eabi-nm` on
`firmware.elf` finds no `aius_optical_app` symbols: the module compiles but
nothing links it until Phase 12 constructs it, so free-for-locals is still
22,684. The saving lands when Phase 12 pulls the object in.

`optical_main.cpp` has roughly 35 more instances of the same pattern and is the
larger prize; Phase 14 deletes them as part of the cutover and should apply this
convention to whatever it keeps. Declined as not worth the indirection: the
`{"ok":true,` success prefix, about 60 net bytes across seven literals, three of
which carry runtime arguments and would need two-stage formatting.

Post-phase bank figures: ITCM 195,192 used / **1,416 free**, DTCM 304,996 /
22,684, FLASH 530,908 / 7,595,556. ITCM free moved from 1,352 to 1,416 as a
result of this phase's own `aius_http.c` edits; no other session's firmware work
landed in that window.

### Phase 12 — Teensy runtime service composition

```yaml
unit: component-behind-interface
seams: [AiusRuntimeTeensy, portable scheduler, target adapters, fake primitives]
contract-artifact: spec-only
dependencies: [phase-1, phase-4, phase-5, phase-6, phase-7, phase-8, phase-9, phase-10, phase-11]
outcome: "Target services compose behind one construction-only, epoch-driven object."
acceptance:
  - exact checkpoint budgets reach every service callback
  - USB_RX handles at most 64 one-byte primitive results; every other service handles one adapter event or action
  - network teardown discards HTTP before resource cleanup
  - fake epoch covers every service result and exact call count
fan-out:
  - name: agent-teensy-cdc-adapter
    owned-paths:
      - firmware/src/aius_cdc_teensy.cpp
      - firmware/src/aius_cdc_teensy.h
  - name: agent-runtime-network-http-service
    owned-paths:
      - firmware/src/aius_runtime_network_http.cpp
      - firmware/src/aius_runtime_network_http.h
  - name: agent-runtime-storage-service
    owned-paths:
      - firmware/src/aius_runtime_storage.cpp
      - firmware/src/aius_runtime_storage.h
  - name: agent-runtime-usb-service
    owned-paths:
      - firmware/src/aius_runtime_usb.cpp
      - firmware/src/aius_runtime_usb.h
  - name: agent-teensy-runtime-composition
    owned-paths:
      - firmware/src/aius_runtime_teensy.cpp
      - firmware/src/aius_runtime_teensy.h
      - firmware/platformio.ini
  - name: agent-teensy-runtime-independent-oracle
    owned-paths:
      - firmware/test/runtime-target/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite runtime-target
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): compose bounded Teensy runtime services"
```

Run the slices sequentially in the listed order. The CDC slice owns only
primitive normalization. The network/HTTP, storage, and USB slices each deepen
one service behind the corresponding `AiusRuntimeNetworkHttp`,
`AiusRuntimeStorage`, or `AiusRuntimeUsb` state and header; none owns scheduler
order or another service. Their headers own the conceptual declarations shown
above. The final composition slice embeds those reviewed services, wires
cross-service actions and scheduler callbacks, retains mode, and updates the
target build. After the parent verifies every public header, a fresh
source-blind oracle reads only the committed contract and headers, then
implements fake-service coverage and runner registration without reading any
service or composition source.

**Delivered.** `src/aius_cdc_teensy.{h,cpp}` (the only `Serial` reference in the
new runtime), the three services `aius_runtime_network_http`,
`aius_runtime_storage` and `aius_runtime_usb`, and the `aius_runtime_teensy`
composition, with a `runtime-target` suite driving them through fake Arduino and
SD primitives. All ten standing suites plus `optical-app` stay green;
`teensy41-optical` builds with all banks fitting. Every new object is `.text 0`,
`.data 0`, `.bss 0` and **`.rodata 0`** — the services carry no string literals
at all — for 5,820 bytes of `.flashmem` across the five.

**Still nothing is linked.** `nm` on `firmware.elf` finds no symbol from any of
the five, nor from `aius_optical_app`, nor from the portable engines they drive.
`optical_main.cpp` is untouched, so nothing references the composition and
`--gc-sections` drops it. The bank report is byte-identical to the Phase 11
baseline and remains uninformative about this work's real cost. **Phase 14 should
expect roughly 9.4 KB of FLASH and Phase 11's ~907 bytes of DTCM string data to
become real at the moment it constructs the runtime.**

**Phase 14 must place the runtime instance in `DMAMEM`.** The composed object is
about 35 KB (13,312-byte HTTP request buffer, 8,192-byte response, 12,288-byte
app body, plus the services) against 22,684 bytes of DTCM free-for-locals. The
requirement is stated normatively in `aius_runtime_teensy.h`.

#### Correction to the NETWORK precedence — deadlines were unreachable

The literal `NETWORK` precedence in [§Optical target-composition
ABI](#optical-target-composition-abi) steps the supervisor only when an event is
retained. But the supervisor's two time-driven transitions have no event source:
an absent DHCP lease is silence, and after teardown `poll()` returns `NONE`
forever because `stack_started` is false. Read literally, `DHCP_WAIT` and
`BACKOFF` are therefore **terminal** — the composition could never time out DHCP
or restart the network, which is precisely the unbounded recovery this plan
exists to close.

**Resolution:** an idle poll (event `NONE`) is followed by one
`aius_net_supervisor_step(NONE)`. That still spends at most one adapter operation
per epoch and never applies and polls in the same epoch, so the budget table
holds. Covered by `net.dhcp-deadline` and `net.backoff-restart`. The ABI section
above should be amended to say so; this block is the normative statement until it
is.

#### Two further findings for Phase 14 and 14b

- **`AIUS_HTTP_MAX_RESPONSE` (8,192) is smaller than
  `AIUS_DSP_MODEL_MAX_JSON_BYTES` (12,288).** The dispatcher can serialize a DSP
  model the transaction's response buffer cannot carry, so a large
  `GET /api/dsp/config` would select the deterministic
  `500 response_too_large`. Pre-cutover this does not bite, because
  `optical_main.cpp` writes the socket itself from its own 12,288-byte
  `g_dsp_config_body`. It becomes a **cutover regression** the moment the route
  goes through the transaction. Phase 14 or 14b must raise the response capacity
  or stream the model.
- **Phase 11's delivered header, not this plan's sketch, is the ABI.** The
  conceptual `AiusOpticalAppOps`, the `usb_verb` / `usb_verb_capacity` init
  parameters, and the out-parameter dispatch signatures described above do not
  exist. What shipped is `AiusOpticalApp` (the 20-entry callback table),
  `AiusOpticalAppDispatcher`, a `bool`-returning `aius_optical_app_init()` that
  adopts a caller-owned body buffer, and dispatch functions returning
  `const …*`. Compose against the header.

Composition details worth keeping: an `AiusRuntimeStorage` `pending_action` pair
described above was removed under the deletion test, no path ever needing to
defer a driver action; a root-owned `storage_action` slot carries the checkpoint-5
action to checkpoint 8 so services still never call each other; and a
`release_http` path cancels a stranded SD job once and then drains at one
primitive per epoch, so a mid-job teardown cannot leave an open `File` behind an
owner that no longer exists.

**Not proven:** the HTTP-driven upload leg, because `accept_application()` admits
`AIUS_SD_JOB_UPLOAD` only for a streamed request, so no route reaches it
end-to-end today — the `NEED_UPLOAD` / `SUPPLY_UPLOAD` mapping is driven directly
against the storage service instead. `AIUS_SD_ACTION_PUBLISH_CONFIG` likewise has
no consumer yet; the storage service acknowledges it so the job still reaches a
terminal result and counts the drop in a saturating `publish_dropped`, making it
observable rather than silent. Both close with Phase 14's buffered-upload
decision.

### Phase 13 — Teensy retained watchdog adapter

```yaml
unit: component-behind-interface
status: completed
seams: [AiusBootStorageTeensy, SRC_SRSR, WDOG1, aius_fault_checkpoint]
contract-artifact: spec-only
dependencies: [phase-2, phase-8]
outcome: "Retained checkpoints and fault images are target-ready without changing main."
acceptance:
  - double-slot tests cover every torn-write boundary and sequence wrap
  - reset-bit priority table is exhaustive
  - production and each fault checkpoint build with a distinct short build id
  - production and checkpoint-selectable fault targets build
fan-out:
  - name: agent-fault-build-cleanup
    owned-paths:
      - firmware/scripts/build.sh
      - firmware/scripts/build_metadata.py
      - firmware/test/build-metadata/
review-intensity: standard
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite boot
    - firmware/scripts/test.sh --environment native --suite boot-target
    - firmware/scripts/test.sh --environment native --suite fault-target
    - firmware/scripts/test.sh --environment native --suite build-metadata
  buildable:
    - firmware/scripts/build.sh teensy41-optical
    - firmware/scripts/build.sh teensy41-optical-fault 0
  parent-commit: "refactor(aius): strip firmware evidence apparatus to plain build stamping"
```

The retained/reset adapter, its target oracle, and the fault injector plus its
oracle are delivered in baseline `8916df890`; the `boot`, `boot-target`, and
`fault-target` suites are green and `teensy41-optical` plus a fault image build.
That code is not re-reviewed.

The evidence/provenance apparatus is stripped. The stopped identity-correction
working set was discarded; `build_metadata.py` was already plain build stamping
(git revision + short build id, see [§Build stamping](#build-stamping));
`build.sh` keeps the `teensy41-optical-fault <checkpoint>` test target and the
optical build lock but no longer copies ELF/HEX artifacts or writes a receipt;
and the dead `test_build_evidence.py` oracle is removed. The `build-metadata`
suite is green and `teensy41-optical` builds with all banks fitting. Do not claim
OCRAM retention until the live smoke (Phase 16) confirms it.

### Phase 14 — Optical runtime cutover

```yaml
unit: refactor-target
status: completed
seams: [setup, loop, AiusRuntimeTeensy, AiusBootStorageTeensy]
contract-artifact: spec-only
dependencies:
  - phase-1
  - phase-2
  - phase-3
  - phase-4
  - phase-5
  - phase-6
  - phase-7
  - phase-8
  - phase-9
  - phase-10
  - phase-11
  - phase-12
  - phase-13
outcome: "setup and loop delegate to the reviewed lifecycle with no legacy path."
acceptance:
  - setup performs construction only and loop runs one epoch
  - static fallback, raw FNET access, rebind globals, blocking writes, and extra feeds are absent
  - the legacy struct AiusSdJob and service_sd_job() are gone and optical_main.cpp includes the portable aius_sd.h
  - the /aius/dsp.json override loads through a portable CONFIG_LOAD job, not a synchronous read, and no 12 KB stack frame remains
  - the DSP config PUT writes through a portable UPLOAD/REPLACE job with publish false
  - the watchdog arms at the pinned 8 s interval and is fed exactly once per complete epoch
  - the network-transport source guard permits only aius_network_teensy.cpp
  - status JSON, USB ID, and UDP expose the exact recovery fields
  - production and fault images build and all component suites remain green
fan-out:
  - name: agent-optical-cutover
    owned-paths:
      - firmware/src/optical_main.cpp
  - name: agent-optical-cutover-source-oracle
    owned-paths:
      - firmware/test/network-transport/test_vendor_source.py
      - firmware/test/optical-cutover/
  - name: agent-optical-cutover-integration
    start-after: [agent-optical-cutover, agent-optical-cutover-source-oracle]
    owned-paths:
      - firmware/platformio.ini
      - firmware/scripts/test.sh
  - name: agent-optical-cutover-docs
    start-after: [agent-optical-cutover]
    owned-paths:
      - firmware/README.md
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite network-transport
    - firmware/scripts/test.sh --environment native --suite net
    - firmware/scripts/test.sh --environment native --suite http
    - firmware/scripts/test.sh --environment native --suite runtime
    - firmware/scripts/test.sh --environment native --suite runtime-target
    - firmware/scripts/test.sh --environment native --suite replay
    - firmware/scripts/test.sh --environment native --suite usb-control
    - firmware/scripts/test.sh --environment native --suite config
    - firmware/scripts/test.sh --environment native --suite dsp-config
    - firmware/scripts/test.sh --environment native --suite sd
    - firmware/scripts/test.sh --environment native --suite sd-target
    - firmware/scripts/test.sh --environment native --suite optical-app
    - firmware/scripts/test.sh --environment native --suite optical-cutover
  buildable:
    - firmware/scripts/build.sh teensy41-optical
    - firmware/scripts/build.sh teensy41-optical-fault 0
  parent-commit: "refactor(aius): cut over the optical runtime lifecycle"
```

#### Scoped slice breakdown — re-scoped 2026-07-25 after its prebuild review

Phase 14 is the only phase that deletes a working path, and it has no hardware
validation until B6. Its first breakdown was rejected by the prebuild gate; this
is the corrected one, and
[§Prebuild review findings](#prebuild-review-findings--2026-07-25) records what
changed and why.

**What survives in `optical_main.cpp`.** The audio processor and its
`AudioConnection`s, the DSP model statics and the block-boundary `apply_config`
path, the `AiusEvent` ring with `record_event` / `record_state`, the replay reader
callbacks, and `current_state`. None of that is network or SD lifecycle and none
of it is touched.

**What dies.** The legacy `AiusSdJobState` enum, `struct AiusSdJob` and
`service_sd_job`; `service_http`, `service_usb_control`, `service_network`,
`ensure_dhcp_client`, `activate_network`, `reset_network_service`,
`supervise_network_link`, `start_network`; `service_sd`, `load_sd_config`,
`refresh_sd_usage` and `stage_dsp_sd_override`; `watchdog_init` /
`watchdog_feed`; the `g_dhcp` / `g_dhcp_pending` / `g_network_retry_at` /
`g_ethernet_initialized` / `g_http_server_rebinds` global set; and the existing
`setup()` / `loop()`.

**What moves rather than dies, and is most of the work.** The seven serializers
and the request handlers become the 20 entries of Phase 11's `AiusOpticalApp`
table. The mapping is one-to-one and mechanical:

| Callback | Existing body |
|---|---|
| `write_home` | `http_home_page` |
| `write_status` | `http_status_json` |
| `write_events` | `http_events_json` |
| `write_dsp_config` | `http_dsp_config_json` |
| `write_replay_status` / `write_replay_list` | `http_replay_status_json` / `http_replay_list_json` |
| `write_meters` | **new, Phase 14b** |
| `store_public_config` | `http_config_public_request` |
| `store_dsp_config` | `http_dsp_config_put_request` |
| `replay_start` / `replay_stop` / `replay_delete` | `http_replay_request` arms |
| `set_test_running` / `clear_counters` / `request_reboot` | `http_handle_request` arms |
| `auth_matches` | `http_sd_authorized` |
| `sd_ready` / `sd_busy` | `g_sd_ready` / `sd_job_active` |
| `write_usb_payload` | `service_usb_control`'s ID/AUTH arms |
| `upload_id` | `g_sd_job_sequence` |

**Slices.**

| Slice | Work | Owned paths | Gate |
|---|---|---|---|
| S0 | **Done** (`64de970b0`) — rename the legacy SD symbols out of the portable namespace: `AiusSdJob` → `LegacySdJob`, `AiusSdJobState` → `LegacySdJobState`, `AIUS_SD_JOB_*` → `LEGACY_SD_JOB_*`. No behaviour change | `src/optical_main.cpp` | target build; every suite unchanged |
| P1a | **Done** (`c76258485`) — transaction: admit an `UPLOAD` job for a complete buffered body, and serve a large immediate body from the application's own buffer instead of copying it into `response` | `lib/aius_http/`, `test/http/` | `http`, `runtime-target` green; target build |
| P1b | **Done** (`f4b5faa21`) — dispatcher: the two buffered config `PUT`s return an `UPLOAD/REPLACE` job rather than an immediate 200 | `src/aius_optical_app.{h,cpp}`, `test/optical-app/` | `optical-app` green |
| R1 | **Done** (`99fede35f`) — runtime seam: a root-initiated SD job entry point, and a publication-consumer callback so a `CONFIG_LOAD` payload reaches the application instead of being counted as dropped | `src/aius_runtime_storage.{h,cpp}`, `src/aius_runtime_teensy.{h,cpp}`, `test/runtime-target/` | `runtime-target` green; target build |
| R2 | **Done** (`2c3c4818f`) — network adapter: an IP and link-state accessor exposing no vendor type, so the status surface survives losing the `Ethernet.*` include | `src/aius_network_teensy.{h,cpp}`, `test/network-transport/` | `network-transport` green; target build |
| P2 | **Done** (`9ce6417e3`) — the `optical-cutover` structural suite in two groups — **positive** (setup trace is construction-only, `loop()` delegates one epoch, exactly one feed) and **absence** (no static fallback, no rebind globals, no raw FNET) — authored and runnable but not yet part of the standing guard | `test/optical-cutover/` | both groups run and fail against the pre-cutover file, by construction |
| S1 | **Done** (`7606632a5`) — the 20 callbacks as thin adapters over the existing serializer bodies. Additive: the legacy path still runs. Costs 2,048 B FLASH and **gains** 704 B of ITCM | `src/optical_main.cpp` | target build; `optical-app` green |
| S2a | **Done** (`d1dfcdfb7`) — construct `AiusRuntimeTeensy` in `DMAMEM`, implement the six platform callbacks, `setup()` becomes construction only and `loop()` one epoch, arm the 8 s watchdog through the boot adapter with one feed per epoch, and refuse `replay_start` pending 14c. The legacy path is still present but unreachable | `src/optical_main.cpp` | `optical-cutover` **positive group**; every component suite; both images build |
| C1 | **Done** (`6a170cd6f`), forced by S2a — move the wholly-cold portable engines and Teensy adapters out of ITCM with a portable cold-section macro on every definition, and add a post-link guard so an unannotated engine fails the build rather than silently taking the audio path's headroom | `lib/aius_http/`, `lib/aius_sd/`, `lib/aius_net/`, `lib/aius_runtime/`, `lib/aius_boot/`, `src/aius_sd_teensy.cpp`, `src/aius_boot_teensy.cpp`, `platformio.ini` + its post-link scripts | optical target builds with all banks fitting; the new guard passes; `http`, `sd`, `net`, `boot`, `runtime`, `sd-target`, `boot-target`, `runtime-target`, `optical-app` green |
| S2b | **Done** (`1e6391c93`) — delete everything in "what dies"; rewrite status JSON, UDP status and USB `ID` onto `AiusRecoveryStatus` plus R2's accessor; drop the NativeEthernet/FNET includes, the vendor guard's exact-path exception, and wire `optical-cutover` into the standing guard. **Also wire `aius_fault_checkpoint()`** — see below | `src/optical_main.cpp`, `src/aius_fault_teensy.cpp`, `test/network-transport/test_vendor_source.py`, `scripts/test.sh` | `optical-cutover` **both groups** + `network-transport` + every component suite |
| S3a | **Done** (`ee957f383`) — engine half, split out as the plan anticipated: the publish-only buffer on `AiusSdJob` with its setter and capacity checks, the chunked config read that keeps SD primitives at 512 bytes, and the model-sized publish buffer on the storage service | `lib/aius_sd/`, `src/aius_runtime_storage.{h,cpp}`, `test/sd/`, `test/runtime-target/` | `sd`, `sd-target`, `runtime-target`, `http`; target build + ITCM guard |
| S3b | **Done** (`79481d195`) — application half: the DSP override and the public config load become `CONFIG_LOAD`, probe and usage refresh become `PROBE` / `USAGE_REFRESH`, and **the config-commit arm is re-attached** so a staged model applies after a committed write. Also resolve `sd_filesystem` — `AiusSdResult` carries `media_present` / `total_bytes` / `used_bytes` and no FAT type (`aius_sd.h:214`), so either extend the probe result or serve the field as `"unknown"`. Depends on S3a | `src/optical_main.cpp` | `dsp-config`, `config`, `sd`, `runtime-target`; target build |
| S4 | **Done** — parts 2–3 (`df5906dcd`) swept `FLASHMEM` across `optical_main.cpp`, eliminated the never-running runtime destructor, and gave the post-link guard a per-symbol allowlist so only genuinely hot code may sit in ITCM; part 1 retired the `MODE` / `MIX` enumerators, their parser entries and body-parsing arms, and repaired the six dependent `test/http` cases. The plan had under-scoped part 1 — the dispatcher carried live `case` labels and `test/optical-app/` held the route-exhaustiveness assertion | `lib/aius_http/`, `test/http/`, `src/optical_main.cpp`, `src/aius_optical_app.cpp`, `test/optical-app/` | `http`, `net`, `optical-app`, `optical-cutover` |
| S5 | **Done** (`4b28a151e`) — `platformio.ini` / `scripts/test.sh` integration, the `itcm-placement` named suite, the firmware README, and retiring the dead `MODE` / `MIX` drives from the soak tool | `platformio.ini`, `scripts/test.sh`, `README.md`, `tools/net/`, `test/net/` | full suite sweep |
| S6 | **Done** (`545a841f0`), forced by an acceptance criterion S5 found unmet — expose the ten recovery fields on all three status channels: UDP extends 71 to 99 bytes to match the shape the runner already decoded, the status JSON gains eight keys, and USB `ID` gains the recovery set at the cost of four network fields that do not fit and are meaningless in recovery-safe mode | `src/optical_main.cpp`, `test/net/`, `swagger.json`, `test/swagger.test.js` | `net`, `net-runner`, `usb-control`, vitest OpenAPI; both images |
| S7 | **Done** (`793982ec7`) — prove the device actually emits those shapes, since no native suite compiles `optical_main.cpp` and all three channels had zero automated coverage; and correct the USB control protocol reference, stale since before this phase | `test/optical-cutover/`, `docs/reference/usb-control-protocol.md` | `optical-cutover` 6/6 positive, 5/5 absence |

**Ordering.** S0, P1a, R1, R2 and P2 are mutually independent and start together.
P1b starts after P1a. S1 starts after S0 and P1b, because the callback signatures
it implements are the ones P1b settles. S2a starts after S1, P2 and R1. S2b starts
after S2a and R2. S3, S4 and S5 follow in order. S2a through S4 are strictly
serialized on `optical_main.cpp`.

Owned paths are disjoint across the five opening slices, but their **gate runs are
not**: P1a and R1 both gate on `runtime-target`, and S0, P1a and R2 all gate on a
target build, while `scripts/test.sh` uses one shared build directory per suite
(`scripts/test.sh:435`) and the optical environment has a single `.pio` tree.
Serialize the gate runs even though the edits are concurrent.

**Why P1 comes before S1.** S1's `store_dsp_config` and `store_public_config`
semantics depend on whether the 200 is optimistic. Landing the ABI first means the
store path is written once. The alternative, cutting over first to get the board
bootable sooner and fixing the semantics before B6, gets hardware a phase earlier
at the cost of writing that path twice, which is the rework this reconciliation
exists to avoid.

**Risk to hold in view.** S2a and S2b are the single riskiest change in either
plan: together they delete the only currently-working path, and the
`optical-cutover` structural suite is the only automated guard on "setup performs
construction only". The memory picture also stops being hypothetical at S2a, when
roughly 9.4 KB of FLASH and ~907 bytes of DTCM string data from Phases 11 and 12
link for the first time. Expect the first real bank report of this work there, not
before. S2a's status output is knowingly transitional: it still reads the legacy
globals, which are no longer being updated, until S2b rewrites it.

This is the load-bearing phase: it swaps the legacy blocking network/SD path in
`optical_main.cpp` for the delivered bounded lifecycle, so it carries the highest
regression risk. The green native suites above are the guard; a structural check
(the `optical-cutover` suite plus the vendor source guard) proves the allowed
setup trace, single-epoch loop delegation, and absence of static fallback, raw
FNET access, rebind globals, blocking writes, or extra watchdog feeds. Build
production plus one fault image; the full per-checkpoint fault set is exercised
by the Phase 16a live smoke, not rebuilt here.

**This phase also carries the DSP strand's SD path**, because the DSP work built
it on the legacy machinery this phase deletes:

- **The legacy `struct AiusSdJob` must go**, and it collides more widely than
  first recorded. `optical_main.cpp:118` declares a same-named, unrelated struct,
  and `optical_main.cpp:115` declares an `AIUS_SD_JOB_DELETE` enumerator that
  redefines `aius_sd.h:20`. Because `aius_optical_app.h:21` includes `aius_sd.h`
  unconditionally, merely naming the callback table pulls the portable header in.
  S0 exists to clear both before S1 can be additive at all.
- **`stage_dsp_sd_override()` becomes a `CONFIG_LOAD` job.**
  `optical_main.cpp:279` currently reads `/aius/dsp.json` a byte at a time into
  a 12,288-byte stack array, from `setup()` and again from `service_sd()`. It
  becomes one portable `CONFIG_LOAD` job with
  `publish_capacity = AIUS_DSP_MODEL_MAX_JSON_BYTES`, driven one primitive per
  epoch, with the published bytes parsed by the same
  `aius_dsp_config_parse_put()` and staged for the block-boundary apply exactly
  as today. This is the caller [Phase 10a](#phase-10a--sd-job-publish-capacity-generalization)
  exists for — but it needs R1 first, because the delivered storage service
  starts a job only from an HTTP action (`aius_runtime_storage.cpp:250`) and
  discards a publication payload rather than delivering it
  (`aius_runtime_storage.cpp:134`). **R1 delivered both seams and found a third
  obstacle, which S3 must settle before it starts** — see below.

**Settled for S3 — a publish-only buffer in the engine.** `aius_sd.c:251` rejects
a `CONFIG_LOAD` whose `publish_capacity` exceeds the job's capacity, and the
composed job's capacity *is* the 512-byte transfer slice
(`AIUS_RUNTIME_STORAGE_TRANSFER`, `static_assert`-locked to
`AIUS_HTTP_SD_CAPACITY_MAX` at `aius_runtime_storage.h:46`), so the planned
`publish_capacity = AIUS_DSP_MODEL_MAX_JSON_BYTES` (12,288) would fail at start
with `AIUS_SD_FAULT_CAPACITY`.

This was reviewed on 2026-07-25 and the plan's earlier recommendation —
publish incrementally so the application accumulates into a buffer it already
owns — **was wrong on its facts and is withdrawn.** The application has no such
buffer: `optical_main.cpp:290`'s 12,288-byte array is a *stack local* inside the
blocking read, alive for one call and incapable of holding state across the ~24
epochs a bounded job takes. The only other model-sized buffers are the dispatcher
response body, which is not idle during a hot re-insert — one of S3's two
triggers — and a parsed model rather than raw JSON. Incremental publish would
therefore not avoid the 12 KB, only relocate it into a new application static, and
pay an engine rewrite on top: publish is single-shot by construction
(`aius_sd.c:1077` finishes the job in the publish arm), `config_bytes` carries
three meanings at once, the symmetric `INSTALL_PUBLISH` path keys post-commit
cleanup on a contiguous phase range, and no fault rule exists for a consumer
refusing chunk *k* after the file is closed.

**Growing the transfer slice is also rejected**, on a better ground than the
memory one first recorded: `job->capacity` sizes *every* transfer, including
download reads and upload input, so 12,288 would make every SD primitive a 12 KB
blocking call — directly against the one-bounded-primitive-per-epoch rule the
storage service exists to enforce.

**The memory objection does not hold and should not be repeated.** The buffer
lands in `AiusRuntimeStorage`, inside the `DMAMEM`-placed runtime object, taking
OCRAM from 15.4 % to 17.7 % used. DTCM is untouched. P1a's rejection of a bigger
response buffer was a *DTCM stack-frame* argument about the legacy path and does
not transfer to an OCRAM buffer inside the runtime object.

So S3 adds `publish_buffer` / `publish_buffer_capacity` to `AiusSdJob` with a
setter, defaulting to the existing `buffer` / `capacity` when null so every
current caller is unchanged; points the three capacity checks and the size event
at the new capacity; reads into `publish_buffer + config_bytes` in `min(remaining,
capacity)` chunks, so **SD primitives stay 512 bytes**; and keeps publish
single-shot. `AiusRuntimeStorage` gains the model-sized buffer, `sd_capacity`
stays 512, and the `static_assert` stays intact. The application parses in place
from the delivered pointer. Roughly six engine edit sites, and every existing `sd`
and `runtime-target` assertion stays green.

The honest cost, recorded so it is not rediscovered as a surprise: this puts a
12,288-byte application-shaped buffer inside a portable, application-agnostic
engine's consumer for a file read twice in a board's lifetime, and it breaks
`AiusSdJob`'s "one job, one buffer" invariant. That is accepted because the
per-epoch primitive bound is a watchdog-safety property and OCRAM is not scarce.
- **The DSP config `PUT` moves to `UPLOAD/REPLACE` with `publish = false`.**
  The model is already parsed and validated in RAM by the time the write starts,
  so re-publishing the written bytes would prove nothing the parser has not
  already proved. The staged-model handoff (`g_pending_dsp_model` /
  `g_dsp_upload_pending` / `g_dsp_apply_pending`) keeps its
  single-writer/single-reader shape; only the job underneath it changes.

The block-boundary apply itself does not move. It stays in the audio path, where
`config_generation` increments and `source` becomes `sd`, and remains the only
place the active model changes.

#### Findings from the slices themselves

Recorded as they were taken, so the final audit has something to check.

- **The `optical-cutover` positive group carried an absence assertion, and it
  cost a legacy mutation.** "No raw `WDOG_WCR_WT(` literal file-wide" was a
  positive-group check, but a file-wide absence is only satisfiable once the
  legacy arming is deleted — which is S2b's job. S2a could only pass its own gate
  by gutting `watchdog_init()`'s body, in a slice explicitly told not to touch
  legacy code. The assertion has moved to the absence group, beside the existing
  "legacy `watchdog_init()` is gone" check where it belongs. `watchdog_init()` is
  left emptied rather than restored: it is unreachable, it dies at S2b, and an
  inert body cannot arm a conflicting 16 s interval if anything ever called it.

- **`aius_fault_checkpoint()` is called from nowhere, so no fault image can
  stall.** The plan requires the Phase 16a live smoke to build a fault image per
  checkpoint `0..8`, each stalling immediately after persisting its checkpoint.
  Its only sensible call site is the runtime's checkpoint callback. S2a left it
  unwired because `aius_fault_checkpoint` is not `FLASHMEM` and wiring it would
  have spent the ITCM headroom that slice was told not to touch — the right call
  at the time, and moot once C1 lands. **S2b owns wiring it**, with `FLASHMEM` on
  its definition in `aius_fault_teensy.cpp`. Without this the whole watchdog
  recovery proof at B6 is vacuous: it would test images that cannot fail.

- **S2a's cutover has expected consequences that are not defects.** Nothing
  probes SD any more, so `g_sd_ready` stays false and every SD route refuses
  until S3 lands the `PROBE` and `CONFIG_LOAD` jobs; usage fields read zero for
  the same reason; and the status surface still reads the legacy globals until
  S2b rewrites it. Eight `-Wunused-function` warnings appear, all on the "what
  dies" list.

- **Deleting the legacy path freed OCRAM, not FLASH, and the plan expected the
  wrong bank again.** Those legacy functions were internal-linkage and already
  unreferenced once S2a stopped calling them, so GCC had discarded them at S2a —
  which is exactly what the eight unused-function warnings were reporting. What
  S2b actually reclaimed is the ~34.8 KB of `DMAMEM` statics they owned: the HTTP
  request and response buffers, the DSP body buffer and the legacy job. OCRAM used
  fell 34,848 bytes to 16.3 %, while FLASH moved 1,024, ITCM 512 and DTCM 256. The
  lesson generalizes on this target: deleting unreferenced static code is free
  because the linker already did it, and the real saving is always the statics.

- **ITCM held 1,936 bytes of cold code in `optical_main.cpp`, and "what may live
  there" was an opinion until S4.** Of 5,484 ITCM bytes only 880 were genuinely
  per-audio-block — the audio processor's `update`, `enqueue` and `consume_block`.
  A file-wide `FLASHMEM` sweep reclaimed 1,300, and the never-running
  `AiusRuntimeTeensy` destructor was eliminated by placement-new'ing the object
  rather than declaring it, since an embedded SD `File` made the implicit
  destructor non-trivial and registered an `__aeabi_atexit` teardown that cannot
  run on this target. The 636-byte static-init thunk is irreducible from inside
  the file: the pinned linker script routes `.text*` to ITCM, only `.flashmem*`
  reaches FLASH, and the thunk has no declaration to annotate.

  The post-link guard now carries a per-symbol allowlist for that object — three
  pinned hot sections that must be *present*, the one thunk, and the framework
  COMDAT classes — so any other ITCM symbol fails the link by name. Note the
  32 KiB block granularity: the reclaimed bytes show as padding rather than
  free-for-locals until the next boundary, which is exactly where they would
  otherwise have cost 32 KiB of DTCM.

- **The config-commit arm died with the legacy job, and S3 must re-attach it.**
  `app_store_public_config` and `app_store_dsp_config` still stage into
  `g_pending_config` / `g_pending_dsp_model` and arm their upload-pending flags,
  but the arm that consumed them on a committed write lived inside
  `service_sd_job`. Until S3 re-attaches it, a successful config `PUT` writes to
  the card without the model ever applying. The staging shape is deliberately
  unchanged, so this is a re-attachment rather than a rewrite.

#### Prebuild review findings — 2026-07-25

The gate rejected the first breakdown and these are its material findings, each
confirmed against source before the re-scope above was written. They are recorded
because every one of them is cheap to re-introduce.

- **S1 could not have been additive.** `aius_optical_app.h:21` includes
  `aius_sd.h`, so the callback table cannot be named without the portable header,
  and two legacy symbols collide with it. S0 is the fix.
- **The `CONFIG_LOAD` port had no seam to land on.** The composed storage service
  accepts `AIUS_HTTP_ACTION_SD_START` and nothing else
  (`aius_runtime_storage.cpp:250`), and acknowledges
  `AIUS_SD_ACTION_PUBLISH_CONFIG` into a saturating `publish_dropped` counter
  (`aius_runtime_storage.cpp:134`). Both gaps are known and documented at
  `aius_runtime_storage.h:72`. R1 closes them; without it S3 has nowhere to go.
- **The status surface would have lost `ip`, `link` and the DHCP counters.**
  `AiusRecoveryStatus` (`aius_net.h:146`) carries none of them, while
  `optical_main.cpp` reaches `Ethernet.localIP()` / `linkStatus()` in fourteen
  places including the status JSON (`:1387`), the UDP status payload (`:1155`)
  and USB `ID` (`:2275`, `:2280`). Removing the vendor-include exception without R2 would
  ship a board that is undiscoverable headless over all three channels, and would
  contradict `swagger.json`, which declares both fields and was committed at B5 as
  accurate.
- **The `optical-cutover` suite must not join the standing guard early.**
  `network-transport` is a declared green gate for this phase and
  `scripts/test.sh:334` runs the vendor guard inside it, so removing the
  exact-path exception before the includes actually disappear would leave every
  commit from P2 through S2a failing its own commit checkpoint. P2 now authors the
  suite; S2b wires it in and removes the exception in the same commit.
- **The six runtime platform callbacks were unlisted.**
  `AiusRuntimeTeensyPlatformOps` requires `reset_capture`, `metrics`, `udp`,
  `replay`, `checkpoint` and `feed_watchdog`, all non-null or the composition is
  invalid (`aius_runtime_teensy.h:63`). Only the watchdog appeared. The `udp` one
  is the largest single piece: `service_network` (`optical_main.cpp:2314`) handles
  nine command types and must be re-expressed against
  `aius_network_teensy_udp_receive_once` / `_send_once`. S2a owns the table.
- **The wider SD lifecycle was unlisted.** `service_sd`
  (`optical_main.cpp:777`) owns media-present polling, hot-insert `SD.begin`,
  `load_sd_config()` (a second blocking byte-at-a-time read, `:379`) and
  `refresh_sd_usage()` (`:368`), all of which feed the status JSON. S3 now owns
  them alongside the DSP override.
- **`loop()` feeds the watchdog at two points, not three.** The three call sites
  are `setup:2593`, `loop:2607` and `loop:2618`. The contract is unchanged — one
  8 s interval, one feed after a complete epoch — but the earlier count was wrong.

Two smaller traps worth carrying: `AIUS_OPTICAL_MODEL_MAX_JSON_BYTES` is a
generated `static const uint32_t` (`scripts/build_config.py:212`), not a macro,
so it is not interchangeable with `AIUS_DSP_MODEL_MAX_JSON_BYTES` in preprocessor
context even though both are 12288; and `request_reboot` only records
(`aius_optical_app.cpp:702`), so S2a must perform the actual reset after the
epoch.

#### Settled — buffered PUTs, and the large DSP response

Both of these were open questions in the first breakdown. Both are now decided.

**A `PUT` returns 200 only after the SD write commits, and it is cheap.** The
first breakdown assumed preserving that meant an expensive change to a delivered,
tested ABI. It does not. `accept_application()` formats the response at
`aius_http_transaction.c:753` but immediately enters `AIUS_HTTP_TX_SD_START`
(`:762`) — the bytes are buffered, not sent. `SEND` is reached only on
`AIUS_HTTP_SD_COMPLETE` (`:1004`), and on `AIUS_HTTP_SD_FAILED`
`select_sd_error()` (`:827`) reformats 500 or 507 because `response_started` is
still false. The delivered transaction already delivers commit-then-200
semantics for SD jobs.

What is actually optimistic is the *dispatcher*: `aius_optical_app.cpp:527` and
`:539` return `dispatch_store(...)` → an `AIUS_HTTP_APP_IMMEDIATE` 200. So the
work splits as P1a and P1b:

- **P1a** relaxes one admission rule. `accept_application:732` requires
  `was_streamed` for an `UPLOAD` job; it must also admit a complete buffered body
  whose `sd_request.total` equals `body_expected`, with `upload_prefix_offset` /
  `upload_prefix_length` set for the buffered case as `parse_ready_request:659`
  already does for the streamed one. Nothing downstream needs to change:
  `drive_sd` already supplies from `request + upload_prefix_offset +
  upload_prefix_supplied`, and `discard_request_slices:672` nulls only
  `parsed_request.body`, leaving the bytes in `transaction->request` where the
  prefix path reads them.
- **P1b** changes the two config routes to return that job instead of an
  immediate 200, which is where the real work is: extending
  `AiusOpticalAppStoreResult` and building target, temp and backup paths for
  routes that have never had them. The `optical-app` suite pins today's behaviour
  at `test/optical-app/test_optical_app.c:758`, so those two cases change with it.

**The large DSP model streams; the response capacity does not move.**
`AIUS_HTTP_MAX_RESPONSE` (8,192) is smaller than
`AIUS_DSP_MODEL_MAX_JSON_BYTES` (12,288), so a large `GET /api/dsp/config`
through the transaction would select the deterministic `500 response_too_large`.
Raising the cap to 12,544 was rejected: it costs 4,352 B of OCRAM inside
`AiusRuntimeNetworkHttp` (`aius_runtime_network_http.h:55`), forces a 12 KB
memcpy and ~24.8 KB resident for one GET, and — decisively — the same constant
sizes two *legacy* stack frames at `optical_main.cpp:1950` and `:2038`, which
would grow from 8 KB to 12.5 KB inside the 22,684 B DTCM free-for-locals zone
while the legacy path is still the only working one. P1a lands before S2b by
design, so that window is real.

Instead P1a copies the *shape* of the mechanism the transaction already has for
downloads — headers written into `response`, body sent from a foreign pointer
(`aius_http_transaction.c:968`, `:1324`) — and applies it to a large
`AIUS_HTTP_APP_IMMEDIATE` body. No cap change, no legacy stack growth, 4.3 KB
less OCRAM, and no 12 KB copy.

**It must not reuse `response_is_download` and `download_data` themselves.** Both
are load-bearing for SD download control flow: `drive_send:1337` emits
`AIUS_HTTP_SD_OP_ACK_DOWNLOAD` once the foreign buffer drains, `drive_cleanup:1403`
emits `FAIL_DOWNLOAD` whenever `download_data != NULL`, and `consume_sd_result:873`
keys `releases_download` off the same pair. An immediate response has no SD job, so
either arm is a protocol fault. P1a needs a distinct flag and pointer whose drain
path falls through to `request_cleanup`. `response_started` (`:1128`) and
`select_sd_error`'s `!response_started` guard (`:856`) are unaffected, because
`select_sd_error` is unreachable without a job.

**New invariant to record with it.** Streaming stretches the dispatcher's body
borrow — `aius_optical_app.h:107` says "until the next dispatch call" — across
roughly 25 writes at `AIUS_HTTP_TX_QUOTA` 512. That is safe today only because
`dispatch_emitted` blocks re-dispatch while in `TX_SEND`
(`aius_http_transaction.c:1533`), and because USB dispatch touches
`usb_payload` / `usb_response` rather than `body` (`aius_optical_app.cpp:719`)
despite sharing the dispatcher every epoch (`aius_runtime_teensy.cpp:120`). State
it in the header before 14b adds `write_meters`.

#### Accepted wire-behaviour changes

Phase 11 made three behaviour choices this phase inherits: serializer exhaustion
returns a deterministic `500 response_too_large` where the legacy path sent an
empty 200 or empty 500; a streamed replay `PUT` without `Content-Range` is now
`400 sd_upload` where the legacy path silently appended at offset 0 and never
finalized; and `GET /api/replays` with no card keeps its pre-cutover `200`
carrying `{"ok":false,"error":"sd_unavailable"}`.

**P1a supersedes the first of those for immediate bodies.** A body that exceeds
the response buffer now streams from the application's own buffer instead of
selecting `500 response_too_large`, which is the entire point of the slice — a
large `GET /api/dsp/config` serves its model rather than erroring.
`response_too_large` stays reachable, and covered, on the SD-job acknowledgement
path, which still copies or fails.

Three more are accepted here.

**Counters that lose their producer at S2b are served as zero.** The HTTP
counters `g_http_requests`, `g_http_rejections`, `g_http_client_drops` and
`g_http_service_overruns` are incremented only in the dying `service_http`
(`optical_main.cpp:2138` onward), and `http_server_rebinds` (`:1392`) goes with
the rebind globals. `network_mode` is derived from `g_network_ready` / `g_dhcp`
(`:1372`) and loses its producer too. All are emitted in the status JSON and at
fixed offsets 55–70 of the 71-byte UDP status payload (`:1175`), so they are
served as zero from S2b until a later phase gives them a producer; the UDP
payload keeps its length and field offsets. `network_restarts` and `reset_source`
survive through `AiusRecoveryStatus` (`aius_net.h:156`, `:153`).

**Addendum 2026-07-26 — the four `http_*` counters have producers again**,
raised to priority by open defect 24 (a failure the device does not count has
no instrument). The observation points are the ones that can actually see the
bounded transaction, not a resurrected `service_http`:

- `http_requests` — one per `AIUS_HTTP_ACTION_DISPATCH` in
  `aius_runtime_http_service()`: a parsed request handed to the application,
  the same "parsed and admitted" moment the legacy counter marked.
- `http_rejections` — a connection's first fault transitioning to `PROTOCOL`
  or `REQUEST_CAPACITY`: the fixed 4xx refusals (bad request line, unknown
  route, bad method, oversize, the null-dispatch 400). `reset_client()` clears
  `last_fault` per accepted connection, which is what makes the transition
  well-defined and at most one per connection.
- `http_client_drops` — transport losses counted per occurrence at the seam's
  own IO observation (`FAULT` on any transport action, `CLOSED` mid-read or
  mid-write), **not** via the `TRANSPORT` fault transition, which first-fault
  latching would undercount on a listener that faults repeatedly; plus one per
  connection first-faulting with a request/response/upload/download timeout —
  the modern spelling of the legacy 150 ms idle / upload-stall drop.
- `http_service_overruns` — the composition root times the HTTP checkpoint
  (`platform_checkpoint()` stamps after the HTTP commit, measures on the UDP
  checkpoint's arrival; the scheduler calls the hook immediately before each
  service, so the interval is one HTTP service call) against the checkpoint's
  now-declared `service_budget_ms` of 2 ms — the legacy 2,000 µs threshold
  carried into the budget table, where the `runtime` suite pins it.

The counters live in `AiusRuntimeNetworkHttp` (surviving network teardown —
they answer "since boot", pinned by `counters.survive-restart`) except the
overrun count, which is the root's own measurement beside
`g_network_budget_overruns`. `put_status()` writes bytes 55–70 from them and
the JSON still reads the payload's own bytes back, so the two channels still
cannot disagree. **`http_server_rebinds` stays zero for good**: the concept it
counted — an Ethernet server object dying and being re-begun mid-run — does
not exist post-cutover, and the nearest real fact, `network_restarts`, is
already served. Native coverage: `runtime-target` `counters.*` (six
scenarios), each mechanism separately, with an observed negative control on
the dispatch count.

**R2 rescued two of the three DHCP fields.** `dhcp_pending` and `dhcp_updates`
are served from the adapter's own generation-scoped state, so they keep their
producers. Only **`dhcp_discovers`** stays an accepted zero: the pinned
NativeEthernet snapshot wires `set_callback_updated` but installs no discover
callback, so producing it would need either a new vendor patch with its manifest
entries or the adapter reaching for the FNET DHCP descriptor that the library
deliberately owns — which §Bounded target transport ABI forbids. Not worth a
vendor patch for a diagnostic counter.

**`POST /api/replays/start` is refused from S2a until Phase 14c lands.**
`aius_replay_service_start` opens the file and then fills the ring synchronously
before returning (`aius_replay.c:159`), so as a `replay_start` callback it would
block inside a dispatcher whose contract is that it "touches no SD primitive …
and never delays" (`aius_optical_app.h:3`). Refusing it also removes the remount
hazard recorded above, because no foreign replay handle can be open while an SD
job runs. 14c restores the route.

**The watchdog interval changes.** The legacy 16 s interval (`WDOG_WCR_WT(31)`)
is replaced by the pinned 8 s interval with exactly one feed after a complete
epoch.

**Retiring `MODE` and `MIX` changes their 404 body, and `GET /api/mode` becomes a
404 rather than a 405.** Both routes already answered 404 for `POST`, but they got
there by parsing successfully and falling through the dispatcher's tail, which
returns `{"ok":false}`. Once the enumerators are gone they fail at route
resolution, so the transaction serves the standard `{"ok":false,"error":
"not_found"}`; and `GET` no longer reaches the method guard that produced the 405.
Both are inseparable from retirement — there is no way to delete the enumerators
and keep the old bodies — and both make the routes behave exactly like every other
unknown path. Accepted because nothing consumes them: `swagger.json` omits both,
`swagger.test.js` asserts their absence, and the dashboard that used them is
queued for deletion at B9.

Watch ITCM. The pre-cutover build has 1,352 bytes free, and this phase adds the
runtime composition on top. Everything it introduces runs in epoch or HTTP
context, not per audio block, so all of it is `FLASHMEM` — applied on each
definition, never via a prototype. If ITCM still does not fit, the escalation
order is in [`aius-dsp-config-api.md`](aius-dsp-config-api.md) §Target memory
strategy; relocating hot data is not on it.

### Phase 14b — Meters endpoint and accumulate wiring

```yaml
unit: phase-task
seams: [AiusMeter, AiusMeterSnapshot, aius_optical_http_dispatch, audio callback]
contract-artifact: spec-only
dependencies: [phase-11, phase-14]
outcome: "GET /api/meters serves a coherent bounded snapshot without disturbing the audio path."
acceptance:
  - the snapshot a reader observes is never torn, under the seqlock boundary
  - the per-block accumulate performs no allocation and no unbounded work
  - the response is fixed-size, with waveform capped at 128 points
  - levels are dBFS floored at -120.0, so every emitted number is finite
  - waveform points are per-mille integers 0..1000
  - sequence 0 is served as a valid 200, not a status-code special case
  - replays.js and the SD editor are updated in the same commit
  - ITCM stays within budget with the accumulate tap linked in
review-intensity: standard
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite dsp
    - firmware/scripts/test.sh --environment native --suite optical-app
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "feat(aius): serve bounded DSP meter snapshots"
```

The contract is [`aius-dsp-config-api.md`](aius-dsp-config-api.md) §Meters
endpoint, whose **settled wire shape** block is now normative rather than a set of
options: dBFS levels floored at `-120.0`, `input` as six positional entries in
`AIUS_CH_*` order, `output` as two-element `[L, R]` arrays, `waveform` as
per-mille integers `0..1000`, and `sequence == 0` served as a valid 200 meaning
"never published". Meter values are diagnostic snapshots and never become
configuration fields.

This phase owns both consumers. `replays.js` is already wrong on two counts
independent of the chosen shape (a scalar `output.peak_dbfs` assumption, and
waveform maths expecting `[-1, 1]` while the struct holds `int32_t` sample
units), and the SD editor degrades on a non-2xx rather than on `sequence == 0`.
Both are fixed here so producer and consumers stay consistent at the commit.

The portable `AiusMeter` / `AiusMeterSnapshot` module is already delivered and
host-proven; what lands here is the target wiring — the per-block accumulate tap
around `aius_dsp_process` in the post-cutover audio path, and the serializer
behind the route Phase 11 already declared. The accumulate tap is the one
genuinely hot addition in the remaining work and the only part of it that may
occupy ITCM; the serializer and the route handler are `FLASHMEM`.

It lands after Phase 14 rather than before because it wires into the audio
callback and the dispatcher, both of which the cutover reshapes. Writing it
first means writing it twice.

### Phase 14c — Replay refill onto a bounded SD job

```yaml
unit: component-behind-interface
seams: [AiusReplayReader, aius_replay service start/refill, AIUS_SD_JOB_REPLAY_REFILL]
contract-artifact: spec-only
dependencies: [phase-5, phase-10a, phase-14]
outcome: "Replay refill spends one bounded SD primitive per epoch like every other consumer."
acceptance:
  - the replay callback performs no direct SD primitive
  - service start no longer prebuffers to a watermark before returning, and POST /api/replays/start is restored
  - an epoch runs at most one SD primitive with a replay active and a storage job in flight
  - a storage job started while a replay is active does not invalidate the replay's read position
  - replay audio output is unchanged for a file that previously played correctly
review-intensity: standard
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite replay
    - firmware/scripts/test.sh --environment native --suite sd
    - firmware/scripts/test.sh --environment native --suite runtime-target
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "refactor(aius): drive replay refill from a bounded SD job"
```

Deferred out of Phase 14 deliberately, and it lands **before** B6. Replay start is
refused from S2a until it does — see §Accepted wire-behaviour changes.

`replay_reader_open` / `_read` / `_close` (`optical_main.cpp:71`) call `SD.open`,
`.seek` and `.read` directly, driven by
`aius_replay_service_refill(&g_replay_service, 2048u)` from `loop()` (`:2614`).
Post-cutover that becomes the runtime's `platform.replay` callback
(`aius_runtime_teensy.h:78`), running in the same epoch as the storage service's
own SD job. `AIUS_SD_JOB_REPLAY_REFILL` already exists in the portable machine
(`aius_sd.h:21`) and has no caller.

Two distinct problems, not one. The **budget** problem is two synchronous SD
consumers per epoch against a normative contract of one. The **correctness**
problem is worse and was missed on the first pass: every portable job begins at
`AIUS_SD_PHASE_BEGIN` (`aius_sd.c:407`) and the adapter's `AIUS_SD_ACTION_BEGIN`
calls `SD.begin(BUILTIN_SDCARD)` (`aius_sd_teensy.cpp:137`), guarded only by the
adapter's own `file_open` flag. It cannot see `g_replay_file`, so any config
`PUT`, download or probe re-mounts the volume beneath an open replay handle.

It is deferred rather than folded into Phase 14 because it is not a port.
`AiusReplayReader` is a synchronous pull seam embedded in `AiusReplayService`
(`aius_replay.h:111`), and `aius_replay_service_start` opens the file and fills
the ring before returning (`aius_replay.c:159`), so both the reader contract and
the transactional start have to change. That is a redesign of a portable library,
not `optical_main.cpp` work, and it does not belong in a slice serialized on that
file. Refusing replay start in the interim keeps the deferral honest: with no
replay handle open, neither problem above can occur.

It lands before B6 because B6 is the single live-hardware session, and validating
a soak against a board that cannot start a replay — or that corrupts one when an
SD job runs — would waste the flash.

### Phase 14f — First flash and assumption validation

```yaml
unit: parent-only
seams: [the deployed board, USB CDC control plane, Ethernet, SD, audio output]
contract-artifact: spec-only
dependencies: [phase-14, phase-14b, phase-14c]
outcome: "The cutover firmware runs on real hardware and the plan's untested assumptions become measurements."
acceptance:
  - the board boots the new image and answers AIUS ID with the expected revision
  - every assumption in the list below is confirmed or recorded as refuted
  - each refutation is filed as a defect with its evidence, not smoothed over
review-intensity: active
commit-checkpoint:
  green:
    - the recorded validation log, with a verdict per assumption
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "test(aius): record the first live validation of the cutover firmware"
```

**Nothing in Phase 14 has ever executed.** Fifteen slices, plus 14b and 14c, were
proven by native suites and a target build that links. A link proves the image
fits and resolves; it proves nothing about boot order, hardware init, timing, or
whether the thing serves a single byte. This phase is where that changes, and it
comes before the recovery smoke because a board that does not boot cannot be
smoke-tested.

**The USB control plane is reachable from this environment.** The agent user is in
`dialout` but its login is stale, so commands must be wrapped:
`sg dialout -c "<command>"`. Confirmed working 2026-07-25 against the deployed
board, which answered `AIUS ID` on `/dev/ttyACM0` (`16c0:0483`).

**State of the deployed board before this phase**, captured 2026-07-25:
revision `062e96ed193d`, built 2026-07-23 — the pre-cutover image. `ip=0.0.0.0`,
`link=2`, `dhcp_pending=1`, USB control plane answering.

**The defect on that image is now identified, and it is not a dead link.** The
DHCP server is issuing DHCPACK; the legacy stack simply never progresses past it.
So `link=2` is itself a firmware misreport — a lease cannot bind over a dead
link — and the real fault is that the application never publishes readiness after
the lease is applied, leaving `ip` at `0.0.0.0` and HTTP unbound. That is
[clean-room finding 3](#clean-room-findings) exactly: network ownership is split
between NativeEthernet's link callback and the firmware's own DHCP client
handling, so `g_ethernet_initialized` describes intent rather than the FNET
lifecycle. The image's own boot log names the workaround it was already
attempting — "DHCP client enabled after link callback gap".

**The prediction for the cutover image, recorded so 14f can falsify it.** The new
adapter should not reproduce this, for three independent reasons, each verified in
source rather than assumed:

- **Callbacks are armed before anything can fire them.** `START_STACK` calls
  `callbacks_install()` *before* `beginNoWait()` (`aius_network_teensy.cpp:588`
  precedes `:590`), and `START_DHCP` runs in a later epoch still. There is no
  window in which a lease can bind with no callback installed.
- **The install is order-independent anyway.** `setDhcpCallback` only stores a
  pointer into a static that the library's own FNET-level shim forwards through
  (`vendor/NativeEthernet/src/NativeEthernet.cpp:271`, `:430`).
- **FNET fires the callback on the initial bind, not only on renewal.**
  `_fnet_dhcp_cln_apply_params()` applies the address, enters `BOUND` and raises
  `callback_updated`, and it is called directly from the DHCPACK arm of
  `REQUESTING` — so the first lease produces `LEASE_ACQUIRED`, which takes the
  supervisor `DHCP_WAIT → ONLINE → PUBLISH_ONLINE`.

And if that reasoning is wrong anyway, the failure is bounded rather than silent:
a missed lease event expires the 5 s DHCP deadline, which drives teardown and
backoff and retries with a fresh generation. The legacy path had no such bound,
which is why it sits indefinitely.

**So 14f has a specific diagnosis to make.** If the board obtains an address and
serves HTTP, the finding is closed. If it instead cycles through DHCP timeouts
while the server logs DHCPACK, then the lease event is being missed despite the
ordering above, and the fix is a belt-and-braces poll of the bound address in
`poll()` rather than sole reliance on the callback — bounded, one read per epoch,
consistent with the ABI's existing allowance for a link-status read.

**Assumptions to validate.** Each has been asserted somewhere in this plan and
none has been measured. Record a verdict for every one.

*Boot and lifecycle*
- `setup()` returns without waiting, and the first epoch runs.
- The watchdog is armed at 8 s and fed once per epoch — the board does not reset
  in normal operation.
- Retained OCRAM survives a watchdog reset, and the boot ledger's CRC and
  double-slot commit behave as the target oracle claims. **The plan explicitly
  says not to claim OCRAM retention until this is confirmed.**
- The ten-second healthy mark clears the early-reset count.

*Network*
- The link comes up at all — the current board says it does not.
- DHCP obtains a lease within the 12 s the contract states, and HTTP serves
  within 12 s.
- A DHCP deadline drives bounded teardown and backoff rather than a hang.
- One generation owns every socket; a restart does not leak a listener.

*SD*
- The probe detects the card, and hot insert and removal are seen.
- `/aius/dsp.json` and `/aius/config.json` load through `CONFIG_LOAD` and apply.
- A config `PUT` commits and the model applies at the block boundary — the arm
  S3b re-attached.
- One SD primitive per epoch holds in practice, not just by construction.

*Audio and DSP*
- Default audio is unchanged from the pre-cutover image.
- The meters accumulate tap's real per-block cost, from the DWT counter 14b wired.
  The estimate is ~1.9 % of a 128-frame block period and ~1.4 % of a 512-frame
  one; **replace both with measurements.**
- No dropouts under sustained load with metering enabled.
- Replay start no longer refuses, plays, and does not tear when an SD job runs.

*Status surfaces*
- The 99-byte UDP payload decodes in the runner.
- Status JSON, USB `ID` and UDP agree, since they share one snapshot.
- The recovery fields report real values after an induced reset.

#### First flash — results, 2026-07-25

**Flashed and running.** `d716e43a5d69+teensy41-optical` went onto the board
through software bootloader entry — `AIUS BOOTLOADER 9` over USB CDC, **no
Program-button press**, on the *legacy* image. That closes one of this plan's
oldest open questions: the recovery control plane works. Note the loader needs the
`plugdev` group where the CDC port needs `dialout`; both are reachable with `sg`.

**The board serves HTTP.** `GET /api/status` returns in 8 ms at `192.0.2.66`,
with `link=1`, an SD card detected and sized through the portable `PROBE` job,
config loaded and valid, and all ten recovery fields present. The failure this
plan was written to fix is closed.

Confirmed live, against the assumption list above: `setup()` returns and epochs
run; the watchdog does not reset in normal operation
(`watchdog_early_resets=0`); SD probe and usage work; config loads; the status
channels carry the recovery state; and `POST /api/reboot` genuinely resets the
board (`boot_generation` increments, `reset_source` changes to the software-reset
value).

**Defect found, and it is the interesting one.** On *every* boot the first DHCP
attempt receives nothing for the full 5 s deadline, then teardown, 1 s backoff,
and generation 2 acquires a lease almost immediately. Measured across reboots:

```
t+3.84s  BOOTED       -> DHCP_WAIT  gen=1  ip=0.0.0.0
t+8.79s  BACKOFF      gen=1  fault=1 (DHCP_TIMEOUT)   <- 4.95 s, no response
t+9.74s  ONLINE       gen=2  ip=192.0.2.66            <- lease in under 1 s
```

The bounded recovery this plan exists to build is what saves it — the legacy image
hit a comparable condition and sat in it indefinitely, while this one detects,
tears down and retries without help. But the first attempt is dead time on every
boot, and the network generation is always 2.

**Confirmed from the DHCP server**, whose clock is authoritative (it is also the
NTP source; its log displays BST, one hour ahead of the UTC figures above). Across
two consecutive boots the server logged **nothing at all** during either
generation-1 window, then a complete exchange the instant generation 2 began:

```
14:51:03.65 UTC  boot -> DHCP_WAIT gen=1        server: (nothing, 5 s)
14:51:09    UTC  gen=2 -> ONLINE                server: DISCOVER/OFFER/REQUEST/ACK
14:51:23.48 UTC  boot -> DHCP_WAIT gen=1        server: (nothing, 5 s)
14:51:29    UTC  gen=2 -> ONLINE                server: DISCOVER/OFFER/REQUEST/ACK
```

Two things follow. The firmware is **not** ignoring a valid `DHCPOFFER` — the
worse of the two possible faults is ruled out, because nothing is transmitted at
all. And when a DISCOVER does reach the wire the whole four-packet exchange
completes inside one second, so the 5 s deadline is pure dead air rather than slow
negotiation.

**Cause.** `apply()` emits `AIUS_NET_EVENT_STACK_STARTED` as soon as
`beginNoWait()` returns, and the supervisor takes that as its cue to start DHCP.
`beginNoWait()` deliberately does not wait for link, so the first DISCOVER is sent
before the PHY has negotiated. By the time backoff expires the link is up, which
is why generation 2 always succeeds instantly. The fix must gate DHCP on actual
link-up **without** introducing a hang when the link is already up at stack start —
NativeEthernet reports link *changes*, so a callback-only trigger would wait
forever. `poll()`'s permitted link-status read is the natural observer.

**Two status regressions the live board exposed**, neither visible in any suite:
`network_mode` reads `"offline"` beside a real IP and `link=1`, because S2b
retired its producer and the plan accepted serving it as zero; and
`sd_filesystem` reads `"unknown"` because S3b removed a raw vendor call from the
serializer without moving the information anywhere. Both are being fixed at the
layer that owns the state rather than restored as raw calls.

#### Second flash — the link gate is inert, and what that proves

Reflashed with the link gate and both status fixes. Measured on the board:

- **`network_mode` now reads `dhcp`** beside its real address, and
  **`sd_filesystem` reads `fat32`** — both regressions closed, confirmed live.
- **The link gate changed nothing.** The board passed `STACK_START` almost
  immediately — observed in `DHCP_WAIT` on **generation 1** with `nfault=0` — then
  failed with `nfault=1` (`DHCP_TIMEOUT`), not `LINK_LOST`, and still needed
  generation 2 to acquire the lease.

The first DHCP attempt fails on every boot. Instrumentation settled it, and the
cause is in the interface bring-up path, not anywhere beyond the host.

**Measured on the board**, `GET /api/status` → `net_diag`:

| | generation 1 | generation 2 |
|---|---|---|
| `netif_init` | performed | skipped |
| `stack_start_blocking_ms` | 1635 | 0 |
| `dhcp_start_ms` | 1657 | 0 |
| `end_ms` | 6657 (exactly +5000) | 19 |
| `dhcp_state_at_end` | 2 — `SELECTING` | 5 — `BOUND` |
| `tx_frames` / `rx_frames` | 4 / 4 | 6 / 6 |
| `link_at_end` / `link_live_at_end` | 1 / 1 | 1 / 1 |

**The decisive number is `rx_frames: 4`.** Ambient broadcast traffic on this
segment runs at roughly **24 frames per 5 seconds**, measured directly from the
live board's counters across two idle windows (470 → 494 → 510). Generation 1
received **four** frames in its entire five-second window — about six times below
what merely being attached should deliver.

So during generation 1 the MAC was barely receiving. That makes
`dhcp_state_at_end: SELECTING` read correctly for the first time: FNET sent its
DISCOVER and never received an OFFER, because the receive path was not carrying
frames. The transmit side was impaired in the same window, which is why the DHCP
server logs no DISCOVER for it while logging the generation-2 exchange in full.

**Conclusion.** `fnet_netif_init()` returns, and `linkStatus()` reports up, while
the interface is not yet passing traffic in either direction for several seconds.
The 1,635 ms it spends hard-resetting the PHY and completing auto-negotiation is
real work — `beginNoWait()` is not no-wait on the generation that initialises —
but it is not sufficient for the FEC to be operational. Generation 2 succeeds
because it *skips* initialisation entirely and inherits an interface that has by
then settled.

**Two earlier explanations are retracted, both recorded so the reasoning is not
repeated.** The delay was attributed to hardware beyond the development host on
the strength of the server logging nothing — an absence says a packet did not
arrive, never where it was lost, and the trusted-fixture boundary exists to stop
exactly that inference. It was then attributed to a stale `linkStatus()` cache;
that cache is refreshed by FNET's own link poll within 200 ms and was honest
throughout, reporting up because the PHY genuinely was up.

**Why the link gate is inert here**, and why it is nonetheless kept: it consults
the one signal that was already saying up. The signal is not wrong about the PHY —
it simply does not mean what the gate needed it to mean. The gate remains correct
for a genuinely dead link, which it bounds at 3 s.

**The fix direction is a readiness condition, not a longer fixed delay.** A fixed
settle would encode one board's timing as a constant. The frame counters are a
real signal: ambient traffic arrives continuously, so a receive counter that has
started moving is direct evidence the interface is carrying traffic. Any condition
must stay bounded — if nothing is ever received, the existing DHCP deadline and
backoff must still apply rather than waiting indefinitely, and a quiet segment
must not deadlock the board.

Note for whoever implements it: `RMON_T_PACKETS` and its receive counterpart count
**all** frames at the MAC, not DHCP specifically, and `fnet_netif_init()` zeroes
them on the generation that initialises. Four transmitted frames never proved a
DISCOVER egressed, and that mistake is worth not repeating.

#### Bootloader entry — fixed and measured

Software bootloader entry was unusable: 48 consecutive attempts without reaching
HalfKay, against 4 for the pre-cutover image. Two defects, both fixed.

The response was never on the wire. `Serial.write()` copies into a ring and arms a
75 µs flush timer; its return value means buffered, not transmitted. The runtime
took that as completion and entered the bootloader immediately, so
`_reboot_Teensyduino_()` — which executes `bkpt #251` or writes `USB1_USBCMD = 0`
— ran with the response still in flight. The legacy path avoided this with a flush
and a 20 ms settle; the cutover dropped both, while the protocol reference
continued to promise a bounded flush.

Separately, the terminal state absorbed every later request: a second
`AIUS BOOTLOADER` received no response at all, which for a mode whose only exit is
that command was close to a brick.

Entry now waits on `availableForWrite()` returning to its idle high-water mark —
a genuine descriptor-retirement signal, stronger than a fixed delay — bounded at
500 ms, after which it proceeds anyway so the board can never wedge. The terminal
state is released on the following step, so a repeat request is answered.

**Measured after the fix: five consecutive trials, each with a full reflash
between them, entered HalfKay on the first attempt.** Note that a single trial on
the *unfixed* image also succeeded first time — the failure was always a race, so
one sample proves nothing and the repeated trials are the evidence.

A caveat worth keeping: the earlier "48 attempts, each answered" was inference,
not observation. That script only watched USB enumeration and never read the
replies, so it could not distinguish an unanswered request from an answered one.
The measurement script now reads each reply and reports boot generation and reset
source between attempts.

#### Readiness gate — measured, and it works

Flashed and measured. The board now reaches service on its **first** network
generation:

| | before | after |
|---|---|---|
| `network_generation` | 2 | **1** |
| `network_last_fault` | 1 `DHCP_TIMEOUT` | **0** |
| `network_restarts` | 1 | **0** |
| `net_diag.first` | a failed generation | **null** |
| `dhcp_state_at_end` | 2 `SELECTING` | 5 `BOUND` |

No teardown, no backoff, no failed generation. The board no longer needs a
failure-and-recovery cycle to get online.

The timings are worth reading rather than just the verdict. `beginNoWait()`
returned at 1,625 ms; the gate then held `STACK_STARTED` until **3,010 ms**,
waiting 1,385 ms for the first ambient frame to prove the interface was receiving.
So the receive path really did take well over a second beyond the link reporting
up — the gate is measuring something real, not padding.

DHCP then bound at 7,013 ms, about 4,000 ms after it started. That is almost
exactly FNET's DISCOVER retransmit interval, which says the *first* DISCOVER still
went unanswered and the retransmit carried it. The transmit path evidently settles
later than the receive path. The gate's value is that the whole exchange now fits
inside one 5 s deadline instead of exhausting it, so the attempt succeeds where it
previously had to fail and be retried on a fresh generation.

That also means there is more to reclaim here if boot time ever matters: the
remaining ~4 s is a lost first DISCOVER, not waiting. Nothing further should be
done about it without measuring the transmit side the way the receive side was
measured.

#### Third flash — the audio path runs, and what it measured

Flashed `8895628a36e6` from a clean tree. Boot generation 15, network generation
**1** with `network_last_fault=0` — the readiness gate still holds. First-attempt
HalfKay again.

**The DSP has run for the first time since the cutover.** `blocks` advances,
`GET /api/meters` publishes real values, and `stop_reason` is on the wire. The
clock handover (`24ff70f7b`) is confirmed on hardware: before this flash a replay
reported `playing` while `offset_bytes` sat at 4,096, `queue_words` at the
watermark and `blocks` at 0 — nothing consumed the queue.

**All three HTTP interop defects are closed on the device, proven with a stock
client rather than a crafted one.** A default `urllib` request trips every one at
once and none of them deliberately — it title-cases the field name to
`X-aius-sd-auth`, `http.client` writes headers and body in separate segments, and
`json.dumps` puts a space after every colon. Status, replay start, replay status
and replay stop: 4/4, no workarounds.

**The measured per-block DSP cost, the last estimate standing in this plan.**
From the DWT counters at 48 kHz with 512-frame blocks:

| | measured | this plan's estimate |
|---|---|---|
| `dsp_cycles_last` | 398,591 (664 µs at 600 MHz) | — |
| `dsp_cycles_max` | 431,995 (720 µs) | — |
| share of the 10,667 µs block period | **6.2 – 6.8 %** | ~1.4 % |

**So the estimate was low by roughly 4.5x.** It is still comfortable — 93 % of the
block period is free — but every figure derived from the old number is wrong, and
Phase 14e must carry these instead. Note this is a replay-fed measurement with no
S/PDIF carrier; the passthrough and DTS-decode paths are still unmeasured.

**Defect found: the replay refill runs at 65 % of realtime.** Sampled over 18 s:

```
     t   blocks     d/s  overrun     o/s    offset    kB/s  queue  under
   2.2     2057    62.6     3688    95.8    282624   125.3      0      5
   5.4     2252    61.6     4006    98.9    688128   123.2      0      5
  10.8     2582    61.2     4542   100.2   1363968   122.5      0      5
```

`blocks` advances at ~61/s against a realtime 93.8/s, and the file offset at
~123 kB/s against 187.5 kB/s. Both are the same 65 % ratio, so one bottleneck
gates both. **`queue_words` is 0 on every sample** — the consumer drains it
instantly and then overruns at ~99/s. The DSP is not the cause; it uses 6.2 % of
its budget. The bounded refill introduced by 14c simply does not deliver enough
bytes per second. `underruns` sits frozen at 5 throughout, so whatever it counts,
it is not counting this — a second, smaller defect.

**The event ring earned itself immediately.** The type-4 overrun events widened in
`c20a03c8e` are what made the overrun rate visible as a rate rather than a
number. They also expose a flaw in that widening: at ~99 overruns/s an eight-entry
ring overflows continuously (`event_overflows` 2,625 against `event_sequence`
2,633), so during a storm the ring carries only the storm. One event per counter
per epoch is not a tight enough bound for a counter that advances every epoch;
it should edge-detect entry to and exit from a sustained condition rather than
report every epoch's delta.

#### Fourth flash — continuous clean playback, and a sample-rate defect

`39ba43154318`. **46 seconds, four loop wraps, zero overruns and zero
underruns.** The previous image logged 3,368 and 9 over the same span.

```
     t   blocks     d/s  overrun     o/s    offset    kB/s  queue  under
   1.0       85    83.8        0     0.0    262144   235.0  43520      0
  22.3     1921    86.4        0     0.0    163840 -1665.0  35072      0
  45.7     3932    85.8        0     0.0    458752   189.2  44544      0
```

`queue_words` oscillates 32,768–46,848 and never approaches zero. `blocks`
advances at a metronomic 85.5–86.9/s across the whole run.

**The mount cache is what did it.** `replay_diag` after the run: `jobs` 255,
**`mounts` 0**, `mount_reuses` 353, **`faults` 0**, and `longest_primitive_us`
**5,524** against 1,020,036 before — a 185× reduction, with `longest_action` now
`OPEN_READ` at 5.5 ms rather than `BEGIN` at a full second.

**But the block rate says the output clock is wrong.** 86.1 blocks/s × 512 frames
is **44,083 Hz**, not the 48,000 the replay content, `GET /api/meters` and
`GET /api/dsp/config` all claim. The Teensy audio engine's
`AUDIO_SAMPLE_RATE_EXACT` defaults to `44100.0f`
(`cores/teensy4/AudioStream.h:58`) and nothing in `platformio.ini` overrides it,
so 44,100 is what the output actually runs at and the measurement matches it to
0.04 %.

**So 48 kHz content plays about 8.1 % slow — roughly 1.4 semitones flat.**
Nothing in the counters shows it, and that is the point: producer and consumer
both run off the output clock, so the queue is stable and the overrun counters
are honestly zero. The pipeline is perfectly self-consistent and the pitch is
wrong. Only the ratio between the *content's* rate and the *clock's* rate reveals
it, and no surface on the device reports that ratio.

This is why [§Phase 14e](#phase-14e--signal-path-and-dsp-reference) must state
the clock authoritatively rather than repeating the config's claim. It also makes
the earlier per-block budget slightly conservative: the block period is
`512 / 44,100` = **11,610 µs**, not the 10,667 µs assumed above, so the measured
398,591–431,995 cycles are **5.7–6.2 %** of budget rather than 6.2–6.8 %.

#### The SD directory index, and what it immediately found

`GET /sd/<dir>/` serves a listing on the device. Its first use found that the
card's contents are stale against the repository: `replays.css` is 0 bytes on the
card and downloads as 0, `replays.js` is 5,586 bytes against 6,799, and
`index.html`, `dsp-editor.css` and `dsp-editor.js` are absent entirely — an old,
partly-failed sync.

That is the feature working. The condition was undiagnosable before, because a
file could only be fetched by a name you already knew, and a truncated upload
looked identical to a missing one. Re-sync with `tools/aius-sd-sync.sh` before the
control surface is used for anything.

#### SD provisioning — two intermittent upload failures

The card was re-synced from the repository web tree. Five of six files landed
first time; the largest, a 29,566-byte script, failed. Investigating it found
**two distinct intermittent failures**, neither a size limit — a bisection at
4 KB, 8 KB, 12 KB, 16 KB, 24 KB and 29.5 KB had 24 KB fail while 29.5 KB
succeeded.

**A dropped connection, understood and mitigated.** Repeating a fixed upload ten
times failed once, and the failure closed in 0.04 s against 0.16 s for a success —
dropped immediately, not timed out. That is the bounded listener working as
designed: the transaction serves one client, and a connection arriving before the
previous one is fully released is dropped. Any sequential tool hits that window
eventually. Fixed in the tool with a bounded retry rather than by widening the
firmware's socket budget, which is deliberately small.

**A `500 sd_failure` on a large upload, not understood.** A later run failed the
same 29,566-byte file with `{"ok":false,"error":"sd_failure"}` — the SD write
itself failing, not a transport drop. It succeeded on retry. This is an open
defect and should not be treated as closed because the retry hides it.

The obvious suspect is the 1 Hz probe job introduced with the SD lifecycle port:
a probe runs `SD.begin()` every second, an upload of this size spans several
epochs, so they overlap routinely. The design says a root job cannot preempt an
HTTP-owned one, and the probe yields on exactly that predicate — so if this is the
cause, the yield is not covering something. **That has not been demonstrated**, and
the alternative that the card itself intermittently fails a write is not excluded.

To investigate: the storage service already counts faults, so correlating an
upload failure against the probe's job state, and against the SD fault code the
job reported, would separate the two. A run with the probe cadence lengthened
would settle it quickly.

#### The memory gate under-reports DTCM by ~10.8 KB

Found while assessing a syslog sender, and it invalidates a figure this plan and
the firmware README have both been quoting.

`mem_report.py` reports DTCM as 306,564 of 327,680 used, leaving **21,116 bytes**
free. The linker symbols say otherwise:

```
_ebss   = 0x2004D7C0
_estack = 0x20050000     ->  stack headroom = 10,304 bytes
NonCacheable @ 0x20020AC0, size 0x261C = 9,756 bytes
```

`NonCacheable` is orphan-placed **inside** DTCM, between `.data` and `.bss`, so it
consumes that address space — but the report excludes it from the used figure.
`_estack - _ebss` is authoritative, and it is 10,304 bytes, not 21,116.

**That free zone is the entire stack, main loop and audio ISR both.** `enqueue()`
alone places about 4,096 bytes of frame there in the audio interrupt path, plus
512 in `update()` — roughly 45 % of the true headroom, in the bank
§Deciding principle names as scarcest. Every "DTCM has ~20 KB free" statement in
this plan and in `firmware/README.md` should be read as half that.

`tools/memory/mem_report.py` is also the gate `build.sh` fails the build on, and
the 2026-07-25 suite audit found it has no test coverage of any kind. A
regex-parsing release gate that silently overstates the scarcest bank by 10.8 KB
is the highest-value untested surface in the firmware. Fixing the arithmetic and
giving it a suite belongs ahead of any new I/O path.

**Fixed, and the fix is structural rather than an added special case.** Excluding
`NonCacheable` by name would have left the next orphan section equally invisible,
so the tool no longer parses `teensy_size:` for its figures at all. It reads the
linked ELF: bank occupancy is the high-water mark of the linker's own watermark
symbol and *every* `SHF_ALLOC` section whose address falls in the bank's region,
so an orphan placed above `_ebss` is counted too. The bank geometry comes from
`cores/teensy4/imxrt1062_t41.ld` — notably `_estack = ORIGIN(DTCM) + ((16 -
_itcm_block_count) << 15)`, which is why the DTCM *bank* ends at `_estack` while
its *region* is the full 512 KiB. The linker does not error when `.bss` runs past
`_estack`; it silently eats the stack, which is exactly what this gate exists to
catch.

The summary is still parsed when a build log is supplied, but only to
cross-check, and each disagreement is reported with the sections that explain it.
Measured on `teensy41-optical` at `9f5b8dee8767`:

```
DTCM   318400 used / 9280 free   (_estack 0x20050000 - _ebss 0x2004dbc0)
  0x20000000   134848B  .data
  0x20020ec0     9756B  NonCacheable
  0x200234dc   173796B  .bss
  0x2004dbc0 ..........  free for locals (9280B) up to _estack
FLASH  summary 571868  elf 581632  delta +9764  omits: .fini 4B, NonCacheable 9756B
DTCM   summary 308644  elf 318400  delta +9756  omits: NonCacheable 9756B
```

The `memory` suite drives the CLI over a committed reduction of a real optical
image — section headers, program headers and linker symbols byte-for-byte from
the linker, plus the matching `teensy_size` log — and is registered in
`scripts/test.sh`. The firmware README's "about 24 kB" free-for-locals claim is
corrected to the measured 9,280 bytes in the same commit.

**Method.** Flash from a clean committed tree, never a working tree with
in-flight slice edits. Capture the `AIUS ID` line before and after. Keep a
validation log with one verdict per assumption and the evidence for it. A refuted
assumption is a defect with a plan entry, not a footnote.

**Recovery if the image is bad.** The Program button always reaches HalfKay
regardless of firmware state, so a failed image is recoverable by hand. The fault
images and recovery-safe mode exist for exactly this session.

**And the button is not the only way, which matters for any image that hangs.**
Setting the CDC line coding to **134 baud** reboots the board into HalfKay from the
Teensy core's SOF interrupt (`cores/teensy4/usb.c:841`, `:383`) — **independently of
the running sketch**, so it works on firmware that is wedged and answering nothing.
That is the difference between it and `AIUS BOOTLOADER <id>`, which needs a live
USB-control path.

Proven repeatedly during the B6 run: gate 3's checkpoints 1 and 2 are structurally
unrecoverable (`aius_fault_checkpoint()` runs *before* the checkpoint's service, and
recovery-safe runs exactly those two), so a fault image there hangs with no control
path at all. The 134-baud reboot restored it first try, both times, and **removed
nine Program-button presses from a single gate-3 run**. Any tooling that has to
recover a hung board should reach for this before asking an operator to press
anything — which makes it the natural mechanism for **B7**, the button-free flash
phase.

One caveat, learned the hard way: address the port through
`/dev/serial/by-id/usb-Teensyduino_USB_Serial_*-if00`, never `/dev/ttyACMn`. Linux
hands a re-enumerated port the next free index, so a board that returns as
`ttyACM1` leaves `ttyACM0` permanently absent — and a runner watching the old path
reports "usb never became ready" against the board for what is purely a host naming
artifact.

### Phase 14d — Firmware I/O reference

```yaml
unit: parent-only
seams: [every firmware I/O path, the audio block boundary]
contract-artifact: spec-only
dependencies: [phase-14, phase-14b, phase-14c]
outcome: "One checked-in reference describes every I/O operation, its cost, and why it cannot disturb the audio path."
acceptance:
  - every I/O path in the firmware appears, with none omitted
  - each entry states its buffers, their sizes and their memory bank
  - each entry gives the rationale for the implementation actually chosen, including rejected alternatives where the choice is non-obvious
  - each entry justifies why it cannot perturb the DSP hot path, in terms of execution context, bank residency and bounded work
  - every claim cites the source that makes it true, so the document can be re-checked rather than trusted
  - the bank figures match a real build, not an estimate
review-intensity: standard
commit-checkpoint:
  green:
    - the repo tooling tool doc-index -- --check
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "docs(aius): describe every firmware I/O path and its audio-path isolation"
```

**Deliberately scheduled last.** This lands after 14c because every earlier slice
still moves I/O: S2b deletes the legacy network and SD path, S3 moves the config
loads onto portable jobs, 14b adds the meters accumulate tap — the one genuinely
hot addition in the remaining work — and 14c moves replay refill onto a bounded
job. A document written before those describes a surface that no longer exists,
and its central claim, that nothing disturbs the audio path, would be unverifiable
against the shipped firmware.

**Home:** `docs/reference/firmware-io.md`, component scope.

**Required coverage.** Every I/O path, not merely the interesting ones. At the
time of writing that means: the I2S audio in and out that constitute the hot path
itself; the SD job engine and its Teensy adapter; the HTTP transaction over the
bounded TCP adapter; UDP discovery and control; USB CDC as the recovery control
plane; the retained OCRAM boot slots read and written each epoch; the replay file
reader; and the codec control transport. Confirm the inventory against the source
at the time rather than against this list, and say so if it has changed.

**What each entry must carry.** Its buffers with sizes and bank (`FLASH`, `ITCM`,
`DTCM`, `OCRAM`/`DMAMEM`), taken from a real build's bank report rather than
estimated. The rationale for the implementation chosen — and where this plan
rejected an alternative, the rejection and its reason, so the reasoning survives
the sessions that produced it. And the audio-path justification: which execution
context it runs in, why it cannot extend a block callback, what bounds its work
per epoch, and what would happen if that bound were violated.

**The audio-path argument is the point of the document.** The mechanisms it must
make explicit are the ones this plan spent its phases establishing: that every
service runs in the epoch loop rather than the audio ISR; that `FLASHMEM` on each
definition keeps ITCM free for code that genuinely runs per block; that large
buffers live in `DMAMEM` rather than the DTCM free-for-locals zone; that at most
one SD primitive is spent per epoch and a wedged one is bounded by the watchdog
rather than a false cooperative timeout; and that the meters snapshot crosses the
boundary through a seqlock rather than a lock the audio path could wait on.

### Network syslog — assessed and declined

Assessed 2026-07-25 for realtime observability once deployed. **Recommendation:
do not build it**, and the reason is not cost.

**The event ring has two call sites.** `record_event` is reached only from the
S/PDIF-lock and pipeline-class transition, and the test-tone toggle. Everything
else observable — overruns, network drops, budget overruns, HTTP rejections, DHCP
updates, config errors, network restarts, watchdog early resets — is a *counter*
in the status payload, not an event. A syslog sender added today would carry a
lock change and nothing else. **The deficiency is the source, not the transport.**

**An absent collector is undetectable.** On-subnet, FNET's ARP resolution holds
one netbuf and rate-limits requests, so the send reports success. Off-subnet, the
datagram reaches the gateway and is lost silently. Neither produces back-pressure,
so any design that queues in order to catch up is a design that grows with no
signal to stop.

**It cannot claim a timestamp.** There is no RTC read anywhere in the firmware and
the status payload's NTP-validity byte is hardcoded zero. A line can honestly
carry milliseconds since boot, the boot epoch, a sequence number and the fused-MAC
identity — nothing more. RFC 5424 at least permits `-` for TIMESTAMP where 3164
would force a fabrication.

**Also corrected: there is no periodic UDP status.** The only send path is
peer-limited to the source of the last received datagram. UDP status is pollable,
strictly request/response; the firmware emits nothing unsolicited.

**What to do instead.** `tools/net/aius_net_runner.py soak` already polls status
and cursor-based events at a tunable interval and writes one JSON line per sample,
with `event_sequence` and `event_overflows` reporting exactly what was missed.
Pointed at the collector host as a unit, that is an ingest pipeline today. The one
gap polling cannot close is the last events before a reset — and the retained boot
ledger already survives the reset and reports `reset_source`, `last_checkpoint`,
`boot_generation` and `watchdog_early_resets`, which is strictly more diagnostic
than the final log lines would have been.

**Spend the effort on `record_event` call sites**: edge-detect the counters that
already exist, in the loop context where they are already read, at one comparison
each and no new bank, transport or ISR exposure. That makes both existing channels
more useful and is a prerequisite to a sender being worth anything.

If it is built later it belongs **after B6**, not before: a push channel from an
image whose recovery behaviour has never been validated reports on behaviour
nobody has checked, and the sender would itself become a new I/O path for 14d to
document and 14f to return a verdict on. The smallest safe shape, should it come
to that, is a single unsent-cursor into the existing ring with a drop counter,
emitted only on epochs the control path did not use, at most one datagram per
fixed interval, reusing the open control socket — no queue, no retry, no ack, no
new checkpoint.

### Phase 14e — Signal path and DSP reference

```yaml
unit: parent-only
seams: [S/PDIF input, decode, DSP chain, output, the audio block boundary]
contract-artifact: spec-only
dependencies: [phase-14, phase-14b, phase-14c]
outcome: "One checked-in reference explains the audio signal path end to end, with a diagram, implementation detail and measured cost."
acceptance:
  - a block diagram carries the signal from optical input to analogue and digital output
  - every processing stage names the module and function that implements it
  - each stage states its buffers, their sizes and their memory bank
  - the per-block time budget is stated, with each stage's measured share of it
  - fixed-point and floating-point boundaries are explicit, including where conversion happens and why
  - the channel layout and downmix rules are stated once, authoritatively
  - every number comes from a real build or a real device, never an estimate presented as fact
review-intensity: standard
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite dsp
  buildable:
    - firmware/scripts/build.sh teensy41-optical
  parent-commit: "docs(aius): describe the audio signal path and DSP chain"
```

**Home:** `docs/reference/signal-path.md`, component scope.

Companion to [§Phase 14d](#phase-14d--firmware-io-reference), and deliberately
separate from it. 14d answers "what I/O exists and why it cannot disturb the audio
path"; this answers "what the audio path actually *is*". A reader who wants to
change the DSP should not have to reconstruct the chain from source.

**Required coverage.** The block diagram is the spine: optical S/PDIF in, the
IEC 61937 framing and DTS decode branch versus the PCM passthrough branch, the
channel map, the downmix, the EQ stages, the limiter, and both outputs. Mermaid,
per the repo's markdown rules.

Around it, per stage: the module and function that implements it; its buffers with
sizes and bank; its arithmetic domain, since the chain crosses between integer
sample units and floating point and the boundaries are not obvious from any one
file; and its share of the per-block budget.

**The budget section is the point.** State the block period at each supported rate
and block size, then each stage's measured cost against it, then the headroom left.
Phase 14b established the pattern by wiring the DWT cycle counter around the
per-block body and reporting it through `GET /api/meters` — so these numbers come
from the device, not from instruction counting. Note where a figure is still an
estimate and say so plainly.

**Record the decisions, not just the shapes.** Where a stage's implementation was
chosen over an alternative — fixed versus floating point, the decimation strategy,
the ring sizing, where metering taps sit and why — say so, so the next reader
inherits the reasoning rather than re-deriving it.

### Phase 15 — Button-free flash tooling

**DELIVERED 2026-07-26.** Shipped as `tools/net/aius_cdc_runner.py` (CDC control,
HalfKay entry, image-identity oracle, verified flash), a thin
`scripts/flash_optical.sh` dispatcher retaining the legacy RawHID handoff, and
the `flash` suite over a scripted transport and synthesised Intel HEX. The
fan-out below was not used — the slices were implemented in one pass because the
oracle and the implementation share the `.hex` parser, and splitting them would
have meant either duplicating it or inventing a seam for a single consumer.
See §Next for the three design points the work established.

```yaml
unit: phase-task
seams: [CDC discovery, AIUS BOOTLOADER, HalfKay, loader exit, revision verification]
contract-artifact: spec-only
dependencies: [phase-7, phase-14]
outcome: "A running optical image can always enter HalfKay and verify its replacement."
acceptance:
  - CDC and RawHID handoff paths are covered with injected commands
  - acknowledgement loss still accepts HalfKay enumeration
  - loader failure, timeouts, and stale revisions fail closed
fan-out:
  - name: agent-cdc-host-control
    owned-paths:
      - firmware/tools/net/aius_cdc_runner.py
      - firmware/test/flash/test_cdc_runner.py
  - name: agent-optical-flash-tool
    owned-paths:
      - firmware/scripts/flash_optical.sh
  - name: agent-optical-flash-independent-oracle
    owned-paths:
      - firmware/test/flash/test_flash_optical.py
      - firmware/test/flash/fake/
  - name: agent-optical-flash-suite-integration
    start-after: [agent-cdc-host-control, agent-optical-flash-tool, agent-optical-flash-independent-oracle]
    owned-paths:
      - firmware/scripts/test.sh
  - name: agent-optical-flash-docs
    start-after: [agent-cdc-host-control, agent-optical-flash-tool]
    owned-paths:
      - firmware/README.md
      - docs/reference/usb-control-protocol.md
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite flash
    - firmware/scripts/test.sh --environment native --suite usb-control
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): automate button-free optical flashing"
```

**This phase now runs after the first live validation (step B7, not B6).** The
deployed board is crash-looping and has to be reflashed before anything on it
can be proven; one manual Program-button press is accepted to get the
post-cutover image on, so the cutover reaches real hardware a whole phase
earlier. The button-free tooling then removes the need for the next press, and
Phase 16b re-runs the zero-button-press gate against it. Nothing about the
phase's own contract changes — only where it sits in the order.

Lock the host CLI, injected command seams, USB-serial selection, RawHID
fallback, HalfKay detection, identity derivation, deadlines, and exit codes
before the three implementation/oracle slices start. The source-blind oracle
does not read either implementation or their reports. Docs may overlap the
oracle; suite integration waits for all three initial slices. Fake tests may
parallelize, but the parent serializes any real CDC, RawHID, HalfKay, or
loader-fixture use.

### Phase 16 — Live recovery smoke

Split into **16a** (steps 2–5 below, at step B6, entered by one accepted manual
Program-button flash) and **16b** (step 1, the zero-button-press flash gate, at
step B8 once Phase 15 exists). The runner, its fake-fixture tests, and the
`recovery-runner` suite all land once, with 16a.

```yaml
unit: phase-task
seams: [USB heartbeat, HTTP status, UDP status, fault selection, deployed fixture]
contract-artifact: spec-only
dependencies: [phase-9, phase-13, phase-14, phase-14b]
outcome: "A small host runner drives a proportionate on-target recovery smoke and records a readable summary."
acceptance:
  - the runner decodes the recovery status fields and reports pass/fail per gate
  - fake-fixture native tests cover each gate result path
  - the live smoke below passes on the deployed device
fan-out:
  - name: agent-recovery-runner
    owned-paths:
      - firmware/tools/net/aius_recovery_smoke.py
      - firmware/test/recovery-runner/
      - firmware/scripts/run_recovery_smoke.sh
      - firmware/scripts/test.sh
review-intensity: standard
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite recovery-runner
  buildable:
    - firmware/scripts/build.sh teensy41-optical
    - firmware/scripts/build.sh teensy41-optical-fault 0
  parent-commit: "test(aius): add proportionate recovery smoke runner"
```

A single host runner reads USB `ID`, HTTP status, and UDP status, decodes the
recovery status fields ([§Status compatibility contract](#status-compatibility-contract)),
and reports pass/fail per gate. Fake-fixture native tests cover each gate result
path. The runner writes the readable per-run summary described in
[§Live smoke evidence](#live-smoke-evidence) — no JSONL receipt chain, canonical
hashing, or LFS artifact matrix.

The proportionate live smoke on the deployed device, run once after cutover:

1. **(16b, after Phase 15.)** One button-free CDC flash and one production
   restore, each verified by build revision, with zero button presses.
   **PASSED 2026-07-26** with the B7 tooling: flashed
   `8623061c2fa7+teensy41-optical` (built 23:47:59Z) over the running
   `cff77ab4641b` (23:34:49Z), then restored the saved production hex —
   the runner verified the outgoing and incoming identity (revision *and*
   build stamp, per the B7 rule that revision alone cannot distinguish a
   rebuild) on each leg, and the whole gate touched no button. The two
   images differed only by two docs commits, which is what made a
   distinct-revision round trip free. Board healthy after restore:
   `boot_generation` 247, `config_valid 1`, `recovery_safe 0`.
2. A handful of software reboots (about five) with DHCP, USB, HTTP, and UDP
   ready inside the normative deadlines and no unexpected watchdog reset.
3. One induced stall at each injectable checkpoint `0..8`, each resetting inside
   8–10 s, reporting its checkpoint, and returning to normal or recovery-safe
   service without intervention.
4. A short soak (about 30 minutes) of HTTP/UDP traffic with no unexpected reset,
   outage, or leaked generation.
5. **The DSP strand's target validation, in the same session:** default audio
   unchanged, a `GET`/`PUT`/SD/reset config round-trip, meters rendering, and
   the SD control-surface editor loading and driving the device. This replaces
   the separate DSP-only hardware session
   ([`aius-dsp-config-api.md`](aius-dsp-config-api.md) §Target validation) —
   one flash, one soak, both strands proven.

Steps 2–5 are 16a and run at step B6; they are entered by **one accepted manual
Program-button press**, because the deployed board is crash-looping and cannot
be reached any other way until the post-cutover image is on it. Step 1 is 16b
and waits for Phase 15.

The single fixture and USB enumeration serialize the mutating steps; each fault
image is restored to the production image before the next step. This is a
personal single-device smoke that confirms the cutover did not regress recovery,
not a certification matrix. There is no separate final clean-room delivery audit:
the green native suites plus the owned-diff review at the cutover commit carry
the plan.

## Deferred work

- Build reproducibility beyond the final ELF/HEX hashes is out of scope for this
  personal build. No host/toolchain fingerprint or hash-locked evidence contract.
- TLS, production authentication policy, and NTP remain separate concerns.
- Audio decoding and DSP behaviour remain independent of Ethernet recovery. The
  DSP *SD and HTTP surface* is not: Phase 14 carries the `/aius/dsp.json` load
  and config-PUT paths onto the bounded SD job, and Phase 14b serves the meter
  snapshot. Only the signal processing itself stays out of this plan.
- A dual-image application rollback scheme is not required. HalfKay plus
  watchdog recovery-safe mode is the recovery boundary.

## Delivered evidence

Phases 1–10, Phase 10a (`df9ea4483`), and the Phase 13 code baseline
(`8916df890`) are delivered. Each landed its native suite green under strict C11
with warnings-as-errors, ASan, and UBSan, and `teensy41-optical` builds with
every memory bank fitting and the pinned NativeEthernet linked-symbol proof
passing. The standing regression-guard suites are `net`, `boot`,
`network-transport`, `sd`, `sd-target`, `http`, `usb-control`, `runtime`,
`boot-target`, and `fault-target`. `runtime-target` is **not** among them —
`firmware/test/runtime-target/` does not exist yet and is created by Phase 12.

- The delivered modules: pure network supervisor, portable boot ledger, pinned
  one-shot NativeEthernet, Teensy network adapter, bounded SD jobs and one-shot
  Teensy SD adapter, stateful HTTP transactions, bounded CDC recovery runtime,
  portable runtime scheduler, versioned recovery status schema, and the retained
  Teensy watchdog adapter with its fault injector.
- The earlier per-phase clean-room plan gates and delivery re-audits are
  historical and are not repeated.
- The Phase 13 evidence/receipt matrix is abandoned along with the provenance
  apparatus it was built for (see [§Build stamping](#build-stamping)); the
  retained-watchdog and fault-injector code it wrapped remains delivered.
