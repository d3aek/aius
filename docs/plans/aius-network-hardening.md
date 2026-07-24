---
title: Aius network and firmware recovery hardening
type: plans
scope: component
component: aius
status: in-progress
tags: [firmware, ethernet, recovery, watchdog, testing]
updated: 2026-07-23
toc: true
---

# Aius network and firmware recovery hardening

The existing hardening is not accepted. Repeated local fixes changed timing and
retry behaviour without proving that initialization, socket ownership, USB
recovery, or watchdog recovery is bounded. This plan replaces those claims with
explicit portable state machines, one-shot target adapters, and evidence that
exercises the deployed firmware.

## Trusted fixture boundary

Treat the DHCP server, network routing, and USB-to-fibre extender as reliable
fixture components. A missing lease, unreachable service, failed software
bootloader transition, or post-flash mismatch is a firmware or firmware-tooling
failure until firmware evidence identifies a more specific cause.

- DHCP is the production address source. Remove the static fallback and its
  separate initialization path.
- Host routing is pre-provisioned fixture state. Do not add route repair or
  route diagnosis to firmware acceptance.
- USB CDC is the recovery control plane. A running or watchdog-recovered image
  must accept a software bootloader command without a Program-button press.
- HalfKay transfer over the USB-to-fibre path is trusted. A successful loader
  exit followed by the wrong application revision is a release failure.

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

## Clean-room plan gate

The first adversarial review on 2026-07-23 failed the plan before
implementation. This amendment resolves its ten findings:

- agent work is concern-sized rather than phase-sized;
- all implementer ABIs, budgets, matrices, and schemas are specified here
  before dispatch;
- the portable boot ledger precedes runtime integration;
- runtime ordering has a host-testable scheduler seam;
- CDC has byte/call quotas and incremental bootloader acknowledgement;
- every new native suite owns its runner registration;
- status compatibility has a byte-level schema and golden fixtures;
- verified reflash is the sole recovery-safe exit;
- each phase contract is a machine-readable YAML block;
- live gate execution is separate from acceptance-runner implementation.

A second recheck still failed on five gaps: shared I/O ownership, budget
delivery, boot-generation monotonicity, bounded SD ownership, and live-evidence
separation. This revision specifies those contracts and adds separate SD, live
gate, and final-audit phases. A final plan-to-build recheck must pass before
Phase 1 dispatch.

A third recheck found eight remaining implementation choices: phase
dependencies/outcomes, async target polling, complete SD jobs, HTTP/USB
application dispatch, retained target storage, composition sizing, wire-value
narrowing, and the receipt schema. This revision resolves them in exact ABIs
and 18 concern-sized phases. The later delivery audit remains independent of
this gate.

A fourth recheck narrowed the remaining gaps to HTTP/SD exchange ownership,
route-complete application callbacks, cache-line slot layout, fault-image
identity, injectable checkpoint terminology, and canonical receipt hashing.
Those contracts are now byte- and call-exact below.

A fifth recheck left two dispatch blockers: the upload byte handoff into the SD
job and the repository invocation for checkpoint-selectable builds. The SD ABI
now has an immediate-copy supply transition, and Phase 13 owns and proves the
two-argument fault build interface.

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
| `4 NETWORK` | supervisor + adapter | one transition/action | DHCP: 5 s; backoff: 1 s |
| `5 HTTP` | one client | one accept; RX 256 B; TX 512 B | HTTP ABI below |
| `6 UDP` | discovery/control | one frame | 2 ms service budget |
| `7 REPLAY` | replay refill | 2,048 bytes | one call |
| `8 SD` | deferred SD job | one transition; one primitive; 512 B | return or 8–10 s reset |
| `9 EPOCH_COMPLETE` | snapshot + watchdog | one snapshot/feed | 8 s watchdog |

Checkpoints `0..8` are injectable recovery checkpoints. `EPOCH_COMPLETE` is the
healthy completion marker immediately before feed and is never fault-injected.

After USB enumeration, `AIUS ID` must respond within 2 s. A normal boot must
obtain its DHCP address within 7 s and serve HTTP within 8 s. An induced stall
must reset in 8–10 s and return USB status within 2 s of re-enumeration.

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

## Build and fault-image identity

`scripts/build_metadata.py` derives identity without timestamps:

- source revision is `git rev-parse --short=12 HEAD`;
- production identity is
  `<source>+teensy41-optical`;
- fault identity is
  `<source>+teensy41-optical-fault.cp<checkpoint>`, where checkpoint is one
  decimal digit `0..8`;
- `build_id` is the first four SHA-256 digest bytes of the UTF-8 identity,
  interpreted little-endian.

The script defines `AIUS_BUILD_SOURCE_REV`, `AIUS_BUILD_TARGET`,
`AIUS_BUILD_FAULT_CHECKPOINT` (`-1` for production), `AIUS_BUILD_GIT_REV`
(the complete identity for compatibility), `AIUS_BUILD_ID`, and
`AIUS_BUILD_UTC`. UTC affects image bytes and audit data but not `build_id`.
Rebuilding the same source/target/checkpoint therefore preserves boot-ledger
identity; changing source, target kind, or checkpoint clears early-reset state.

The repository build interface is `firmware/scripts/build.sh`
with arguments `teensy41-optical-fault <checkpoint>`. Its required second
argument is one digit `0..8`; it rejects omission, extra arguments, and a
selector on any other environment with usage exit 4. The script exports
`AIUS_FAULT_CHECKPOINT` only to that PlatformIO invocation.
`build_metadata.py` rejects a missing/invalid selector in the fault environment
and rejects a selector in production. Metadata native tests cover known
SHA-256 vectors, all selectors, invalid invocations, and stable IDs across
timestamps. USB `ID` and HTTP status emit
`build_revision` and eight-hex-digit `build_id`. The flash tool computes the
final HEX SHA-256 and rejects a device whose identity/build ID differs from the
selected artifact. Each receipt records the same revision, build ID, and image
hash. Live acceptance artifacts are built only from a clean committed tree;
intermediate phase builds are not acceptance evidence.

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
    uint8_t active_slot;
    uint32_t sequence;
} AiusBootStorageTeensy;

void aius_boot_storage_teensy_init(
    AiusBootStorageTeensy *storage);

bool aius_boot_storage_teensy_load(
    AiusBootStorageTeensy *storage,
    AiusBootLedger *ledger);

bool aius_boot_storage_teensy_commit(
    AiusBootStorageTeensy *storage,
    const AiusBootLedger *ledger);

uint32_t aius_boot_teensy_reset_source(void);
AiusResetKind aius_boot_teensy_reset_kind(uint32_t raw_reset_source);
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

Reset classification is priority ordered: `SRC_SRSR_WDOG_RST_B` is
`WATCHDOG`; `SRC_SRSR_IPP_RESET_B` or `SRC_SRSR_CSU_RESET_B` is `COLD`;
`SRC_SRSR_JTAG_SW_RST` or `SRC_SRSR_LOCKUP_SYSRESETREQ` is `SOFTWARE`; all
remaining combinations are `OTHER`. Raw bits remain in telemetry. Tests cover
each bit and combination, watchdog priority, sequence wrap, every torn-write
boundary, corrupt slot/ledger CRC, and one valid/one invalid slot.

The fault image defines exactly one build flag,
`AIUS_FAULT_STALL_CHECKPOINT=0..8`. Immediately after persisting the matching
checkpoint, `aius_fault_checkpoint(uint16_t)` enters an intentional tight loop
in `aius_fault_teensy.cpp`; the production build compiles the function to a
return. This test-only file is the sole exception to the unbounded-loop source
guard. Phase 17 builds and records a distinct revision for every checkpoint.

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
} AiusNetworkTeensy;

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
without a primitive call. FNET `AGAIN` is `WOULD_BLOCK`; peer closure is
`CLOSED`; every other negative result or NativeEthernet write error is
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
    AIUS_SD_EVENT_NONE,
    AIUS_SD_EVENT_OK,
    AIUS_SD_EVENT_BYTES,
    AIUS_SD_EVENT_BOOL,
    AIUS_SD_EVENT_SIZE,
    AIUS_SD_EVENT_FAULT
} AiusSdEventKind;

typedef struct {
    AiusSdEventKind kind;
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
    AIUS_SD_ACTION_OPEN_APPEND,
    AIUS_SD_ACTION_SEEK,
    AIUS_SD_ACTION_NEED_INPUT,
    AIUS_SD_ACTION_READ,
    AIUS_SD_ACTION_WRITE,
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
    const char *path;
    const char *path_to;
    uint8_t *buffer;
    size_t length;
    uint32_t offset;
    uint32_t fault;
} AiusSdAction;

typedef struct {
    AiusSdJobKind kind;
    AiusSdInstallMode install_mode;
    const char *target_path;
    const char *temp_path;
    const char *backup_path;
    const char *content_type;
    size_t total;
    uint32_t initial_offset;
    bool publish_config;
} AiusSdJobRequest;

typedef struct {
    AiusSdState state;
    AiusSdJobKind kind;
    AiusSdInstallMode install_mode;
    char target_path[AIUS_SD_PATH_STORAGE];
    char temp_path[AIUS_SD_PATH_STORAGE];
    char backup_path[AIUS_SD_PATH_STORAGE];
    const char *content_type;
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
    size_t total;
    size_t requested;
    size_t pending;
    uint32_t deadline_ms;
    uint32_t last_fault;
    bool target_existed;
    bool backup_moved;
    bool publish_config;
    bool close_emitted;
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
```

The caller owns the 1–512 byte transfer buffer. `start()` validates and copies
all non-null paths into fixed job storage; paths in later actions borrow those
copies. `content_type` is an application-owned constant. Upload requires
explicit target/temp/backup paths. `REPLACE` writes temp, closes, checks target,
moves an existing target to backup, installs temp, then removes backup.
`APPEND_PART` appends at `initial_offset` without promotion.
`FINALIZE_PART` verifies the part size then performs the same backup/install
sequence. A failed install attempts rollback exactly once; failure preserves
the backup and emits `FAIL`, never an endless retry. `publish_config` emits
`PUBLISH_CONFIG` only after successful installation. Download and replay
refill open/seek/read/close; delete emits one remove. Probe, config load, and
usage refresh use the same action vocabulary, so no SD call remains in
`setup()` or outside the runtime SD checkpoint.

A step emits at most one action and one state transition. `BYTES` may not exceed
the action length; zero bytes before the declared total is a fault. `BOOL` and
`SIZE` are accepted only for their matching action. Cancel is idempotent,
closes once, removes a disposable temp once, and rolls back a moved target once.
Native tests cover every state/event pair, all seven job kinds, all three
install modes, path/capacity boundaries, partial progress, every action fault,
deadline equality/wrap, install/rollback failure, and repeated cancellation.

After an upload file opens, `step()` enters `NEED_INPUT` and emits
`ACTION_NEED_INPUT` with length `min(capacity, total - offset)`, stored in
`requested`. The runtime maps that action to `AIUS_HTTP_SD_NEED_UPLOAD`. HTTP
reads no more than the requested length and returns it in one `SD_STEP`.
The runtime immediately calls `aius_sd_job_supply()`: only `NEED_INPUT` accepts
it; length must be `1..requested` and must not exceed remaining total. Supply
copies the borrowed bytes into the caller-owned job buffer, sets `pending`,
clears `requested`, enters `WRITE`, and performs no driver call. The next SD
epoch emits one `WRITE`. Partial `BYTES` advances the buffer offset and re-emits
only the unwritten suffix; after `pending` reaches zero, the job either emits
the next `NEED_INPUT` or begins commit when total bytes are written. Invalid or
zero supply selects `FAILED`. Thus upload input is copied before the HTTP event
expires and target writes never retain transport memory.

The target adapter ABI is:

```c
typedef struct {
    bool file_open;
    uint32_t last_duration_us;
    uint32_t longest_duration_us;
} AiusSdTeensy;

void aius_sd_teensy_init(AiusSdTeensy *adapter);
AiusSdEvent aius_sd_teensy_execute(
    AiusSdTeensy *adapter,
    const AiusSdAction *action);
```

`aius_sd_teensy_execute()` contains no loop, retry, or delay and invokes exactly
one matching `SD`/`File` primitive. It owns the sole target `File` handle.
`PUBLISH_CONFIG`, `COMPLETE`, and `FAIL` are application actions and never
enter the adapter. A source guard proves the one-call shape; a fake proves
action/result mapping; target receipts record each returned call duration and
require 99th percentile below 50 ms and maximum below 250 ms. These measurements
do not pretend to cancel a wedged library call: the Phase 14 fault image stalls
at checkpoint `SD` and must prove watchdog reset and USB recovery in 8–10 s.

## Bounded HTTP transaction ABI

Deepen `aius_http` with a transaction object; parsing and response formatting
remain its public codec.

```c
typedef enum {
    AIUS_HTTP_TX_IDLE,
    AIUS_HTTP_TX_RECEIVE,
    AIUS_HTTP_TX_DISPATCH,
    AIUS_HTTP_TX_SEND,
    AIUS_HTTP_TX_SD_TRANSFER,
    AIUS_HTTP_TX_CLOSE
} AiusHttpTransactionState;

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
    AIUS_HTTP_SD_STARTED,
    AIUS_HTTP_SD_NEED_UPLOAD,
    AIUS_HTTP_SD_DOWNLOAD_DATA,
    AIUS_HTTP_SD_COMPLETE,
    AIUS_HTTP_SD_FAILED
} AiusHttpSdEventKind;

typedef enum {
    AIUS_HTTP_EVENT_NONE,
    AIUS_HTTP_EVENT_ACCEPTED,
    AIUS_HTTP_EVENT_RX,
    AIUS_HTTP_EVENT_TX,
    AIUS_HTTP_EVENT_SD,
    AIUS_HTTP_EVENT_APPLICATION_RESPONSE,
    AIUS_HTTP_EVENT_CLOSED,
    AIUS_HTTP_EVENT_FAULT
} AiusHttpEventKind;

typedef struct {
    AiusHttpEventKind kind;
    AiusIoEvent io;
    const uint8_t *data;
    const AiusHttpApplicationResponse *application_response;
    AiusHttpSdEventKind sd_kind;
    size_t sd_length;
    uint64_t sd_size;
    uint32_t sd_fault;
} AiusHttpEvent;

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

typedef struct {
    AiusHttpActionKind kind;
    const uint8_t *data;
    size_t length;
    const AiusHttpRequest *request;
    const AiusSdJobRequest *sd_request;
} AiusHttpAction;

typedef struct {
    AiusHttpTransactionState state;
    uint8_t *request;
    size_t request_capacity;
    size_t request_length;
    uint8_t *response;
    size_t response_capacity;
    size_t response_length;
    size_t response_offset;
    uint32_t deadline_ms;
    AiusHttpRequest parsed_request;
    AiusSdJobRequest sd_request;
    bool dispatch_emitted;
    bool cleanup_emitted;
} AiusHttpTransaction;

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

The caller owns both fixed buffers for the transaction lifetime. RX event data
is borrowed for the step only. The step fully initializes `action` to `NONE`,
consumes at most 256 event bytes, emits at most one write of 512 bytes, and
makes at most one state transition.
- A successful parse stores `parsed_request` and emits `DISPATCH` exactly once.
  The request and all slices it references remain valid until the application
  response step returns.
- The application response is borrowed for one step. `IMMEDIATE` is formatted
  into the caller-owned response buffer. `SD_JOB` copies the request struct
  into transaction state and emits `SD_START` with a pointer to that copy.
  Its path pointers remain borrowed from `AiusOpticalApp` only until the
  runtime synchronously calls `aius_sd_job_start()`, which copies every path.
  A missing, duplicate, or out-of-state application response is a protocol
  fault and closes the transaction.
- If headers plus body exceed response capacity, the transaction formats a
  fixed 500 `response_too_large` response when that fits; otherwise it closes
  without a partial response. Application body pointers are never retained.
- `WOULD_BLOCK` preserves offsets and changes no counter except a deadline
  expiry. Partial writes advance exactly by the reported byte count.
- Disconnect or fault at every state selects `CLOSE`; cleanup is emitted once.
- Request timeout is 150 ms, response-write timeout 100 ms, upload timeout 2 s,
  and download timeout 10 s.
- The runtime consumes `SD_START` before the next transaction step, initializes
  its owned job, and returns `SD_STARTED` or `SD_FAILED`. While transferring,
  each `SD_STEP` advances the owned job and at most one target SD action.
  `NEED_UPLOAD` makes HTTP read at most 256 bytes; the following `SD_STEP`
  carries those borrowed bytes for immediate copy to the 512-byte job buffer.
  `DOWNLOAD_DATA` borrows that buffer until the next step; HTTP writes at most
  512 bytes before requesting more. `sd_size` supplies the download
  `Content-Length`. `COMPLETE` sends the already-formatted success response;
  `FAILED` formats the mapped deterministic error. Disconnect emits
  `SD_CANCEL`, which the runtime consumes once before returning `CLOSED`.
  HTTP never calls a file API or loops over SD progress.

The failure matrix covers `WOULD_BLOCK`, zero-byte progress, every partial
read/write split, disconnect/fault in every state, response capacity, invalid
transport byte counts, SD open/read/write/commit/cancel failures, and deadline
equality/wrap.

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
    const char *payload;
} AiusUsbApplicationResponse;

typedef struct {
    const uint8_t *rx;
    size_t rx_length;
    size_t tx_capacity;
    size_t tx_accepted;
    bool connected;
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
} AiusUsbRuntimeAction;

typedef struct {
    AiusUsbParser parser;
    AiusUsbRuntimeState state;
    char *response;
    size_t response_capacity;
    size_t response_length;
    size_t response_offset;
    uint32_t bootloader_deadline_ms;
    AiusUsbApplicationRequest pending_request;
    bool dispatch_emitted;
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

The caller owns RX and response buffers. `tx_accepted` acknowledges bytes from
the prior action and may not exceed that action's length. The step consumes at
most the first 64 RX bytes, offers at most 64 TX bytes within `tx_capacity`,
fully initializes the action, and makes at most one state transition.

- RX consumes at most 64 bytes or 64 one-byte read attempts per epoch.
- A complete command is copied to `pending_request` and emits `DISPATCH`
  exactly once. `PROTOCOL`, `ID`, `AUTH`, and `BOOTLOADER` all use this seam;
  the application supplies protocol/build/status/auth payloads and authorizes
  bootloader entry. The request remains valid until the response step returns.
- An application response is borrowed for one step. Its `verb` and `payload`
  are copied through the existing formatter. A formatter overflow emits the
  fixed `INTERNAL` error if that fits, otherwise drops the response and returns
  to idle. Missing, duplicate, or out-of-state responses are ignored and
  counted as protocol faults.
- TX requests one write of at most 64 bytes per epoch. Zero capacity preserves
  the response offset and does not busy-wait.
- `AIUS BOOTLOADER <id>` enters `ACK_PENDING`. HalfKay entry occurs after the
  full acknowledgement is accepted by CDC, or after a 500 ms deadline. There
  is no `flush()` or `delay()`.
- If USB disappears after the command, the host treats HalfKay enumeration as
  success even when it did not receive the acknowledgement.
- Continuous RX, line overflow, partial TX, zero TX capacity, disconnect, exact
  deadline, and `millis()` wrap are native-test cases.

Recovery-safe mode accepts `PROTOCOL`, `ID`, `AUTH`, and `BOOTLOADER`; verified
reflash is its only exit.

## Portable runtime scheduler ABI

`aius_runtime` owns service order and watchdog-feed eligibility. It has no
Arduino dependencies.

```c
typedef struct {
    uint16_t checkpoint;
    uint16_t max_calls;
    uint16_t max_bytes;
    uint32_t progress_deadline_ms;
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

`aius_runtime_epoch()` invokes the normative checkpoint table in order. Before
each service it calls `checkpoint()`. In normal mode it calls all eight services
then records `EPOCH_COMPLETE` and feeds once. In recovery-safe mode it calls
only bounded USB RX/TX, records `EPOCH_COMPLETE`, and feeds once. A service
returning false ends the epoch without feed; the next watchdog reset identifies
that checkpoint. `aius_runtime_budget()` returns immutable compile-time
descriptors matching every table row; tests compare the full mapping. Host
tests assert exact order, construction-only setup
contract, one feed, no feed on early return, safe-mode selection, and isolated
network-generation restart.

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
including home, status, events, DSP config, controls, SD files, config, and
replay list/status/start/stop/delete. It supplies status, content type, body,
and optional SD job through the complete application response. Body pointers
must address the supplied app buffer or read-only static storage; path pointers
must address app-owned storage. They remain valid until the next dispatch.
Callback `false`, invalid pointer/length, unknown routes/commands, and buffer
exhaustion produce deterministic 4xx/5xx or `INTERNAL` responses. Native tests
drive every existing HTTP route and USB command through fake callbacks and
golden responses.

`aius_runtime_teensy` owns target service callbacks and buffers; its header
contains the complete composition object and no file-static lifecycle state:

```c
typedef struct {
    AiusNetSupervisor supervisor;
    AiusNetworkTeensy network;
    AiusHttpTransaction http;
    AiusSdJob sd;
    AiusSdTeensy sd_target;
    AiusUsbRuntime usb;
    AiusOpticalApp app;
    uint8_t http_request[AIUS_HTTP_MAX_REQUEST];
    uint8_t http_response[AIUS_HTTP_MAX_RESPONSE];
    uint8_t sd_transfer[512];
    char usb_response[320];
    uint32_t completed_epochs;
} AiusRuntimeTeensy;

void aius_runtime_teensy_construct(
    AiusRuntimeTeensy *runtime,
    const AiusOpticalAppOps *app_ops,
    uint32_t now_ms,
    bool recovery_safe);

bool aius_runtime_teensy_epoch(
    AiusRuntimeTeensy *runtime,
    uint32_t now_ms);

void aius_runtime_teensy_status(
    const AiusRuntimeTeensy *runtime,
    AiusRecoveryStatus *status);
```

Construction initializes memory only. The epoch supplies exact portable
budgets, routes at most one adapter event/action per service, calls
`aius_network_teensy_apply()` with `NONE` when the supervisor emits no new
action so retained publish/teardown work advances, and returns the portable
scheduler result. It does not feed the watchdog itself; final composition
supplies the sole-feed callback. Generation teardown discards the HTTP
transaction before adapter cleanup. Native target-service tests use fake
network, SD, CDC, clock, checkpoint, and watchdog primitives to cover every
callback result and exact call count.

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
`watchdog_early_resets`, `last_checkpoint`, and `loop_epoch`.

`AiusNetFault` is the authoritative fault domain and is limited to its declared
`0..10` values; target-native error codes are mapped at the adapter boundary
and never enter status. Runtime checkpoints are limited to `0..9`. The encoder
rejects any out-of-domain enum/checkpoint and emits no packet. The decoder
rejects supervisor states `6..255`, recovery-safe values other than `0` or `1`,
reserved fault values `11..255`, checkpoints `10..255`, nonzero reserved bytes,
and payload lengths other than 71 or 99. No saturation or truncation is
permitted.

## Recovery evidence schema

Each acceptance attempt appends one UTF-8 JSON object and newline. Schema
`aius-recovery-receipt/1` has these exact required keys; no key is optional.

```json
{
  "schema": "aius-recovery-receipt/1",
  "run_id": "32 lowercase hex characters",
  "sequence": 1,
  "attempt_id": "<run_id>/000001",
  "started_at": "RFC3339 UTC with Z",
  "ended_at": "RFC3339 UTC with Z",
  "target": {
    "usb_serial": "string",
    "fixture": "string"
  },
  "firmware": {
    "revision": "string",
    "build_id": "8 lowercase hex characters",
    "build_utc": "RFC3339 UTC with Z",
    "image_sha256": "64 lowercase hex characters"
  },
  "gate": "flash|reboot|watchdog|adapter_fault|soak|restore",
  "fault": null,
  "expected": {
    "checkpoint": null,
    "network_fault": null
  },
  "observed": {
    "boot_generation": 1,
    "reset_source": 0,
    "watchdog_early_resets": 0,
    "last_checkpoint": 9,
    "network_generation": 1,
    "network_state": 2,
    "network_fault": 0,
    "loop_epoch": 1
  },
  "latency_ms": {
    "loader": null,
    "usb_ready": 0,
    "dhcp_ready": 0,
    "http_ready": 0,
    "watchdog_reset": null
  },
  "services": {
    "usb": "pass",
    "dhcp": "pass",
    "http": "pass",
    "udp": "pass",
    "audio": "pass"
  },
  "result": "pass",
  "error": null,
  "artifacts": [],
  "previous_record_sha256": null,
  "record_sha256": "64 lowercase hex characters"
}
```

`fault` is null or one of `stall`, `would_block`, `listener_loss`,
`partial_write`, `peer_close`, `teardown`, `callback_overflow`, or
`sd_stall`. Expected checkpoint is null or integer `0..8`; expected network
fault is null or integer `0..10`. Observed integer ranges match the wire ABI;
an unavailable observation is null, never zero-filled. Each latency is a
nonnegative integer or null. Every service is `pass`, `fail`, or
`not_applicable`. Result is `pass`, `fail`, or `interrupted`. Error is null on
pass, otherwise `{"code": "<enum>", "message": "<string>"}` where code is one
of `timeout`, `identity`, `revision`, `protocol`, `service`, `audio`,
`artifact`, `operator_interrupt`, or `runner_internal`.

Each artifact is
`{"kind":"receipt|transcript|packet|audio|image|log","path":"<repo-relative>",
"sha256":"<64 lowercase hex>","bytes":<nonnegative integer>}`. Authored
receipts/transcripts point under `firmware/test/evidence/network-hardening/`;
binary/audio artifacts point under `docs/reference/datasheets.mdnetwork-hardening/`.
`record_sha256` hashes canonical JSON with that field omitted; the previous
hash chains complete records within one run.

Canonical JSON means RFC 8785 JSON Canonicalization Scheme encoded as UTF-8.
The hash input is the complete record object with only `record_sha256` removed;
`previous_record_sha256` remains null for sequence 1 and the prior lowercase
hex digest thereafter. No trailing newline enters the digest. Required unit
vector:

```text
object without record_sha256:
{"schema":"aius-recovery-receipt/1","sequence":1,"previous_record_sha256":null}

RFC 8785 bytes:
{"previous_record_sha256":null,"schema":"aius-recovery-receipt/1","sequence":1}

SHA-256:
0ab1b1eda6a925271476b230f52ad8bc5cb5e291275aebf408c9f430e130a9e5
```

The runner generates `run_id` once from 16 random bytes and monotonically
increments sequence. It serializes a record to one line of at most 4096 bytes,
locks the run file, opens with `O_APPEND`, performs one write, calls `fsync`,
and unlocks. A short write is `runner_internal`; the original file is retained
without repair for diagnosis, renamed with `.partial`, and a new run ID is
required.
Startup rejects a non-newline-terminated or hash-invalid log. It never edits,
deduplicates, or overwrites an earlier attempt.

## Implementation phases

Each phase is one concern and ends green, buildable, committed, and pushed. A
sub-agent edits/tests only its owned paths; the parent updates this plan, runs
the declared proof, commits explicit paths, and pushes.

### Phase 1 — Pure network supervisor

```yaml
unit: component-behind-interface
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
unit: component-behind-interface
seams: [aius_sd_job_step, AiusSdAction, one-shot Teensy SD driver]
contract-artifact: spec-only
dependencies: [phase-1]
outcome: "All SD work is represented as a portable, cancellable one-action state machine."
acceptance:
  - every state and event pair is covered for all seven job kinds
  - replace, append, finalize, rollback, and cancellation preserve path authority
  - invalid paths, byte counts, deadlines, and driver events fail deterministically
fan-out:
  - name: agent-bounded-sd-job
    owned-paths:
      - firmware/lib/aius_sd/
      - firmware/test/sd/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite sd
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): add bounded SD job state machine"
```

Implement the portable SD ABI and complete fake-driver transition matrix.
Target `File` wiring remains in Phase 10.

### Phase 6 — Stateful HTTP transactions

```yaml
unit: component-behind-interface
seams: [aius_http_transaction_step, AiusIoResult, bounded SD job]
contract-artifact: spec-only
dependencies: [phase-1, phase-5]
outcome: "One HTTP transaction bounds transport, application dispatch, and SD progress."
acceptance:
  - every transport and application failure state closes exactly once
  - dispatch occurs exactly once and response overflow is deterministic
  - partial RX, TX, and SD progress preserve exact offsets and deadlines
fan-out:
  - name: agent-http-transaction
    owned-paths:
      - firmware/lib/aius_http/include/aius_http.h
      - firmware/lib/aius_http/src/aius_http.c
      - firmware/test/http/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite http
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): model bounded HTTP transactions"
```

Cover the complete failure matrix through the public transaction interface.

### Phase 7 — Bounded CDC recovery runtime

```yaml
unit: component-behind-interface
seams: [aius_usb_runtime_step, AIUS_USB_COMMAND_BOOTLOADER]
contract-artifact: spec-only
dependencies: [phase-1]
outcome: "CDC parsing, application dispatch, TX, and bootloader entry are quota-bound."
acceptance:
  - continuous RX and partial or zero-capacity TX remain within quotas
  - every command dispatches exactly once with deterministic overflow behavior
  - acknowledgement completion, loss, deadline equality, and wrap select HalfKay once
fan-out:
  - name: agent-usb-runtime
    owned-paths:
      - firmware/lib/aius_usb_control/
      - firmware/test/usb-control/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite usb-control
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): bound the USB recovery runtime"
```

Implement the RX/TX quotas and incremental bootloader transition. Target
composition still uses the old adapter until Phase 14.

### Phase 8 — Portable runtime scheduler

```yaml
unit: component-behind-interface
seams: [aius_runtime_epoch, AiusRuntimeOps, boot checkpoints]
contract-artifact: spec-only
dependencies: [phase-1, phase-2]
outcome: "One portable epoch enforces service order, quotas, checkpoints, and feed eligibility."
acceptance:
  - immutable budgets exactly match the normative table
  - normal and recovery-safe service order and call counts are exact
  - early return records the failing checkpoint and never feeds
fan-out:
  - name: agent-runtime-scheduler
    owned-paths:
      - firmware/lib/aius_runtime/
      - firmware/test/runtime/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite runtime
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): add deterministic runtime epochs"
```

Prove exact service order, construction-only setup contract, watchdog
eligibility, early-return behaviour, recovery-safe selection, and generation
restart isolation.

### Phase 9 — Status schema compatibility

```yaml
unit: test-surface
seams: [put_status, parse_status, HTTP status JSON, USB ID]
contract-artifact: spec-only
dependencies: [phase-1, phase-2]
outcome: "Old and new status consumers share one byte-exact recovery schema."
acceptance:
  - golden 71-byte and 99-byte fixtures round-trip
  - reserved values, invalid lengths, and narrowing attempts are rejected
  - old payloads retain their first 71 bytes and legacy-static parse behavior
fan-out:
  - name: agent-status-schema
    owned-paths:
      - firmware/lib/aius_net/include/aius_net.h
      - firmware/lib/aius_net/src/aius_net.c
      - firmware/test/net/
      - firmware/tools/net/aius_net_runner.py
      - firmware/test/net/test_net_runner.py
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite net
    - firmware/scripts/test.sh --environment native --suite net-runner
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): version recovery status telemetry"
```

Land golden 71/99-byte fixtures and old/new parser assertions before target
integration. Target JSON/USB wiring remains Phase 14.

### Phase 10 — One-shot Teensy SD adapter

```yaml
unit: component-behind-interface
seams: [aius_sd_teensy_execute, AiusSdAction, Teensy SD and File]
contract-artifact: spec-only
dependencies: [phase-5]
outcome: "Every target SD call is one observable primitive behind the portable job."
acceptance:
  - source guard rejects loops, retries, and delay in the adapter
  - fake maps every action to one primitive and one event
  - optical target builds with the adapter unused
fan-out:
  - name: agent-teensy-sd-adapter
    owned-paths:
      - firmware/src/aius_sd_teensy.cpp
      - firmware/src/aius_sd_teensy.h
      - firmware/test/sd-target/
      - firmware/platformio.ini
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite sd-target
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): add one-shot Teensy SD adapter"
```

### Phase 11 — Optical application dispatch

```yaml
unit: component-behind-interface
seams: [aius_optical_http_dispatch, aius_optical_usb_dispatch, fake app callbacks]
contract-artifact: spec-only
dependencies: [phase-5, phase-6, phase-7, phase-9]
outcome: "Every HTTP route and USB command has exactly one bounded application dispatch."
acceptance:
  - every current route and command has a golden response
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

### Phase 12 — Teensy runtime service composition

```yaml
unit: component-behind-interface
seams: [AiusRuntimeTeensy, portable scheduler, target adapters, fake primitives]
contract-artifact: spec-only
dependencies: [phase-1, phase-4, phase-5, phase-6, phase-7, phase-8, phase-9, phase-10, phase-11]
outcome: "Target services compose behind one construction-only, epoch-driven object."
acceptance:
  - exact checkpoint budgets reach every service callback
  - one adapter event or action is handled per service
  - network teardown discards HTTP before resource cleanup
  - fake epoch covers every service result and exact call count
fan-out:
  - name: agent-teensy-runtime-services
    owned-paths:
      - firmware/src/aius_runtime_teensy.cpp
      - firmware/src/aius_runtime_teensy.h
      - firmware/test/runtime-target/
      - firmware/platformio.ini
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite runtime-target
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): compose bounded Teensy runtime services"
```

### Phase 13 — Teensy retained watchdog adapter

```yaml
unit: component-behind-interface
seams: [AiusBootStorageTeensy, SRC_SRSR, WDOG1, aius_fault_checkpoint]
contract-artifact: spec-only
dependencies: [phase-2, phase-8]
outcome: "Retained checkpoints and fault images are target-ready without changing main."
acceptance:
  - double-slot tests cover every torn-write boundary and sequence wrap
  - reset-bit priority table is exhaustive
  - production and all fault selectors have distinct verified build IDs
  - production and checkpoint-selectable fault targets build
fan-out:
  - name: agent-watchdog-adapter
    owned-paths:
      - firmware/src/aius_boot_teensy.cpp
      - firmware/src/aius_boot_teensy.h
      - firmware/src/aius_fault_teensy.cpp
      - firmware/src/aius_fault_teensy.h
      - firmware/scripts/build.sh
      - firmware/scripts/build_metadata.py
      - firmware/platformio.ini
      - firmware/test/boot/
      - firmware/test/build-metadata/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite boot
    - firmware/scripts/test.sh --environment native --suite build-metadata
    - firmware/scripts/test.sh --environment native --suite runtime
  buildable:
    - firmware/scripts/build.sh teensy41-optical
    - firmware/scripts/build.sh teensy41-optical-fault 0
    - firmware/scripts/build.sh teensy41-optical-fault 1
    - firmware/scripts/build.sh teensy41-optical-fault 2
    - firmware/scripts/build.sh teensy41-optical-fault 3
    - firmware/scripts/build.sh teensy41-optical-fault 4
    - firmware/scripts/build.sh teensy41-optical-fault 5
    - firmware/scripts/build.sh teensy41-optical-fault 6
    - firmware/scripts/build.sh teensy41-optical-fault 7
    - firmware/scripts/build.sh teensy41-optical-fault 8
  parent-commit: "feat(aius): add retained Teensy watchdog adapter"
```

Do not claim OCRAM retention until live Phase 17 evidence.

### Phase 14 — Optical runtime cutover

```yaml
unit: refactor-target
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
  - the network-transport source guard permits only aius_network_teensy.cpp
  - status JSON, USB ID, and UDP expose the exact recovery fields
  - production and fault images build and all component suites remain green
fan-out:
  - name: agent-optical-cutover
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/platformio.ini
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
  buildable:
    - firmware/scripts/build.sh teensy41-optical
    - firmware/scripts/build.sh teensy41-optical-fault 0
  parent-commit: "refactor(aius): cut over the optical runtime lifecycle"
```

### Phase 15 — Button-free flash tooling

```yaml
unit: test-surface
seams: [CDC discovery, AIUS BOOTLOADER, HalfKay, loader exit, revision verification]
contract-artifact: spec-only
dependencies: [phase-7, phase-14]
outcome: "A running optical image can always enter HalfKay and verify its replacement."
acceptance:
  - CDC and RawHID handoff paths are covered with injected commands
  - acknowledgement loss still accepts HalfKay enumeration
  - loader failure, timeouts, and stale revisions fail closed
fan-out:
  - name: agent-optical-flash-tool
    owned-paths:
      - firmware/scripts/flash_optical.sh
      - firmware/test/flash/
      - firmware/scripts/test.sh
      - firmware/README.md
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite flash
    - firmware/scripts/test.sh --environment native --suite usb-control
  buildable:
    - teensy41-optical
  parent-commit: "feat(aius): automate button-free optical flashing"
```

### Phase 16 — Acceptance runner and evidence schema

```yaml
unit: test-surface
seams: [USB heartbeat, HTTP status, UDP status, fault selection, audio oracle, JSONL receipt]
contract-artifact: spec-only
dependencies: [phase-9, phase-13, phase-15]
outcome: "One command executes a selected gate and durably records every attempt."
acceptance:
  - schema validation covers every enum, nullable field, and range
  - append tests cover interruption, short write, corrupt tail, and hash chain
  - fake fixture covers every gate result and preserves failed attempts
fan-out:
  - name: agent-recovery-acceptance-runner
    owned-paths:
      - firmware/tools/net/
      - firmware/test/net/
      - firmware/scripts/run_recovery_acceptance.sh
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite net-runner
    - firmware/scripts/test.sh --environment native --suite recovery-runner
  buildable:
    - firmware/scripts/build.sh teensy41-optical
    - firmware/scripts/build.sh teensy41-optical-fault 0
  parent-commit: "test(aius): automate firmware recovery acceptance"
```

### Phase 17 — Live hardware gates

```yaml
unit: test-surface
seams: [deployed fixture, acceptance runner, raw evidence]
contract-artifact: spec-only
dependencies: [phase-14, phase-15, phase-16]
outcome: "Target evidence proves button-free recovery and bounded failure behavior."
acceptance:
  - ten button-free flashes
  - one hundred normal reboots
  - ten watchdog recoveries per injectable checkpoint 0..8
  - complete adapter fault matrix
  - eight-hour concurrent soak
  - production image restored and verified
fan-out:
  - name: agent-live-recovery-gates
    owned-paths:
      - firmware/test/evidence/network-hardening/
      - docs/reference/datasheets.mdnetwork-hardening/
review-intensity: active
commit-checkpoint:
  green:
    - all seven acceptance groups have valid chained receipts
  buildable:
    - deployed teensy41-optical production image
  parent-commit: "test(aius): prove firmware recovery on target"
```

Acceptance requires:

1. Ten consecutive optical flashes through CDC software bootloader entry, exact
   revision verification, and zero button presses.
2. One hundred software reboots with DHCP, USB, HTTP, and UDP ready inside the
   normative deadlines and no unexpected watchdog reset.
3. Ten induced stalls at every injectable checkpoint `0..8`. Every run resets
   inside 8–10 s, reports the exact checkpoint, and returns to normal or
   recovery-safe service without intervention.
4. Every adapter fault (`WOULD_BLOCK`, listener loss, partial write, peer close,
   teardown fault, callback overflow) reaches its specified state while USB
   remains responsive.
5. Every returned SD primitive meets the timing distribution, an SD checkpoint
   stall resets in 8–10 s, and USB recovery returns without a button press.
6. Eight hours of HTTP/UDP/SD traffic has zero unexpected resets, unexplained
   outages, leaked generations, or independent optical-audio discontinuities.
7. Production firmware is restored through the same workflow and its exact
   revision is recorded.

Authored JSONL receipts and text transcripts live under
`firmware/test/evidence/network-hardening/`. Raw binary/audio captures live
under `docs/reference/datasheets.mdnetwork-hardening/` and use Git LFS.

### Phase 18 — Final clean-room delivery audit

```yaml
unit: test-surface
seams: [plan, delivered code, authored receipts, raw evidence]
contract-artifact: spec-only
dependencies: [phase-17]
outcome: "An independent reviewer accounts for every promise before shipment."
acceptance:
  - every plan promise independently matches code and evidence
  - every divergence is fixed or recorded
  - production image identity matches the final restore receipt
fan-out:
  - name: reviewer-final-delivery
    owned-paths: []
review-intensity: active
commit-checkpoint:
  green:
    - clean-room delivery audit passes
  buildable:
    - deployed teensy41-optical production image
  parent-commit: "docs(aius): close firmware recovery hardening"
```

The reviewer sees only this plan, code, authored receipts, and raw evidence.
The parent applies required corrections, marks the plan `shipped`, commits, and
pushes.

## Deferred work

- TLS, production authentication policy, and NTP remain separate concerns.
- Audio decoding and DSP behaviour remain independent of Ethernet recovery.
- A dual-image application rollback scheme is not required. HalfKay plus
  watchdog recovery-safe mode is the recovery boundary.

## Delivered evidence

- Pre-change baseline on 2026-07-23: native `net` codec suite passes and
  `teensy41-optical` builds with PlatformIO 6.1.16, Teensy platform 5.2.0,
  framework 1.162.0, and all memory banks fitting. This proves only the starting
  checkpoint, not hardening efficacy.
- First clean-room plan gate: failed before dispatch; all findings are resolved
  in the contract and decomposition above.
- Final clean-room plan-to-build recheck: passed after five failed reviews and
  their amendments. Phase 1 is authorized for dispatch.
- Phase 1: native `net` suite and `teensy41-optical` build pass. The first
  delivery audit rejected ambiguous invalidated-generation wording and a
  mirror-image oracle; the amended live-resource contract and independent
  table/model proof passed the clean-room re-audit.
- Phase 2: native `boot` suite and `teensy41-optical` build pass. The first
  delivery audit rejected a self-confirming CRC test and missing invalid-state
  branches; the fixed 24-byte known-answer vector and complete public-interface
  proof passed the clean-room re-audit.
- Phase 3: native `network-transport` source and ASan/UBSan fake-FNET suites
  pass across fresh FNET, netif, link, and full-lifecycle processes;
  `teensy41-optical` builds with only the pinned `AiusNativeEthernet` source,
  and its post-link symbol proof passes. Four clean-room delivery reviews
  rejected partial-init deadlock, callback-unsafe teardown, self-confirming
  source checks, incomplete fault injection, mutable patch identity,
  bypassable or overbroad loop guards, and late link-state mutation. The final
  review passed after executable cleanup-time callback injection, exact
  upstream/patched hashes, target resolver enforcement, and late-event
  authority revocation were delivered.
- Phase 4: native `network-transport` and `teensy41-optical` pass with the
  adapter compiled beside the unchanged legacy runtime. The first delivery
  audit rejected a primitive call during clean teardown and sampled I/O
  coverage. The corrected zero-call teardown, exhaustive fake operation/result
  matrix, malformed-result rejection, and production fake-FNET TCP/UDP branch
  proof passed the clean-room re-audit.
