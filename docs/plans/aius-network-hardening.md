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

The linked `g_aius_boot_slots` symbol must be 32-byte aligned, occupy exactly
128 bytes in `.dmabuffers`, and satisfy
`address + 128 <= 0x2027ff80`. A post-link check fails every production or
fault build otherwise; this is the compile-time guard against overlap with the
pinned CrashReport range `0x2027ff80..0x2027ffff`.

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
    bool publish_config;
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
    bool publish_config;
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
requires capacity at least `AIUS_CONFIG_MAX_JSON_BYTES` (256). `start()` is
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
| `CONFIG_LOAD` | `OPEN_READ(target) → FILE_SIZE → READ* → CLOSE → PUBLISH_CONFIG → COMPLETE`. Target is required. Size must be `1..AIUS_CONFIG_MAX_JSON_BYTES` and fit the caller buffer. Positive partial reads accumulate contiguously until the declared size. `PUBLISH_CONFIG` borrows the complete buffer until its `OK`/`FAULT` event. Missing/open failure, empty input, oversize input, or early zero bytes fail after one close if opened. Result is `BYTES` with the published byte count. |
| `USAGE_REFRESH` | `TOTAL_SIZE → SIZE`, then `USED_SIZE → SIZE`, then `COMPLETE`. Result is `USAGE` with both 64-bit values; `used > total` is `PROTOCOL`. |
| `UPLOAD/REPLACE` | For the target, temp, and backup parent prefixes in that order, emit `EXISTS(parent) → BOOL` and emit `MKDIR(parent) → OK` only when false. Then `OPEN_WRITE_BEGIN(temp) → TRUNCATE(0)`, accept and write exactly `total` new bytes, `CLOSE`, and run the install sequence below. Zero-length replacement is valid only when `publish_config == false` and still opens, truncates, closes, and installs an empty file. |
| `UPLOAD/APPEND_PART` | For each target parent prefix, emit `EXISTS(parent) → BOOL` and `MKDIR(parent) → OK` only when false; `OPEN_READ(target) → FILE_SIZE → CLOSE` and require exact size `initial_offset`; `OPEN_APPEND(target)`; accept and write exactly `total`; `CLOSE`; `COMPLETE`. `target` is the durable part path. It is never promoted, renamed, or removed by failure/cancellation. |
| `UPLOAD/FINALIZE_PART` | For the target, temp, and backup parent prefixes in that order, emit `EXISTS(parent) → BOOL` and `MKDIR(parent) → OK` only when false; `OPEN_READ(temp) → FILE_SIZE → CLOSE` and require exact size `initial_offset`; `OPEN_APPEND(temp)`; accept and write exactly `total`; `CLOSE`; reopen/read size and require `initial_offset + total`; close; then run the install sequence. `temp` is the durable part and is preserved on pre-install failure or cancellation. Config publication is valid only when the final `initial_offset + total` is within the config limit. |
| `DOWNLOAD` | `OPEN_READ(target) → FILE_SIZE → SEEK(initial_offset) → READ* → OUTPUT* → CLOSE → COMPLETE`. After size capture, `initial_offset > captured_size` is `RANGE`. If `total == 0`, set `end_offset = captured_size`; otherwise require checked `initial_offset + total <= captured_size` or fail `RANGE`, then set that sum as `end_offset`. Result is `BYTES` with acknowledged output bytes. |
| `REPLAY_REFILL` | The same open/size/seek/read/output/close sequence, but `total` is a required positive requested maximum. After size capture, `initial_offset > captured_size` is `RANGE`; otherwise set `end_offset = initial_offset + min(total, captured_size - initial_offset)`. `initial_offset == captured_size` is a successful zero-byte refill. |
| `DELETE` | `EXISTS(target) → BOOL`; true requires `REMOVE(target) → OK` before `COMPLETE`. False completes when `require_existing == false`, or fails with `NOT_FOUND` and no mutation when `require_existing == true`. |

Request fields are not implicit:

| Kind/mode | `total` | `initial_offset` | `publish_config` / `content_type` |
|---|---|---|---|
| `PROBE`, `USAGE_REFRESH`, `DELETE` | must be zero | must be zero | false / null |
| `CONFIG_LOAD` | must be zero | must be zero | true / required non-null constant |
| `DOWNLOAD` | zero means through captured EOF; positive is exact length | `0..INT64_MAX` | false / required non-null constant |
| `REPLAY_REFILL` | required `1..INT64_MAX` maximum | `0..INT64_MAX` | false / null |
| `UPLOAD/REPLACE` | `0..INT64_MAX`; zero is valid only without publication | must be zero | optional; if true, total must be `1..AIUS_CONFIG_MAX_JSON_BYTES`, capacity at least that limit, and content type non-null |
| `UPLOAD/APPEND_PART` | `0..INT64_MAX`; zero is valid | `0..INT64_MAX` | false / null |
| `UPLOAD/FINALIZE_PART` | `0..INT64_MAX`; zero is valid | `0..INT64_MAX` | optional; if true, checked final size must be `1..AIUS_CONFIG_MAX_JSON_BYTES`, capacity at least that limit, and content type non-null |

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
`1..AIUS_CONFIG_MAX_JSON_BYTES`; `config_bytes` starts at zero, partial reads
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
  - name: agent-retained-reset-adapter
    owned-paths:
      - firmware/src/aius_boot_teensy.cpp
      - firmware/src/aius_boot_teensy.h
  - name: agent-retained-reset-independent-oracle
    owned-paths:
      - firmware/test/boot-target/
  - name: agent-fault-injector
    owned-paths:
      - firmware/src/aius_fault_teensy.cpp
      - firmware/src/aius_fault_teensy.h
  - name: agent-fault-injector-independent-oracle
    owned-paths:
      - firmware/test/fault-target/
  - name: agent-fault-build-identity
    owned-paths:
      - firmware/scripts/build.sh
      - firmware/scripts/build_metadata.py
  - name: agent-fault-build-independent-oracle
    owned-paths:
      - firmware/test/build-metadata/
  - name: agent-watchdog-build-coordinator
    start-after: [phase-10, agent-retained-reset-adapter, agent-retained-reset-independent-oracle, agent-fault-injector, agent-fault-injector-independent-oracle, agent-fault-build-identity, agent-fault-build-independent-oracle]
    owned-paths:
      - firmware/platformio.ini
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - firmware/scripts/test.sh --environment native --suite boot
    - firmware/scripts/test.sh --environment native --suite boot-target
    - firmware/scripts/test.sh --environment native --suite fault-target
    - firmware/scripts/test.sh --environment native --suite build-metadata
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
Run the first six concern-sized slices after this phase's clean-room gate.
The retained/reset implementation and its source-blind target oracle are
sequential. The fault injector is followed by a source-blind oracle that
compiles the real source for production and selectors `0..8`, proves exact
checkpoint matching/return behavior, and mutation-tests the single intentional
loop exception. Build identity may run in parallel with those pairs, followed
by its source-blind metadata oracle. The shared
`platformio.ini`/`scripts/test.sh` coordinator starts only after Phase 10 is
committed and all six slices are reviewed. All builds sharing
`.pio/build/teensy41-optical-fault` run serially.

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
  - name: agent-optical-cutover-source-oracle
    owned-paths:
      - firmware/test/network-transport/test_vendor_source.py
      - firmware/test/optical-cutover/
  - name: agent-optical-cutover-coordinator
    start-after: [agent-optical-cutover, agent-optical-cutover-source-oracle]
    owned-paths:
      - firmware/platformio.ini
      - firmware/README.md
      - firmware/scripts/test.sh
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
    - firmware/scripts/test.sh --environment native --suite optical-cutover
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
  parent-commit: "refactor(aius): cut over the optical runtime lifecycle"
```

The cutover implementation and source-blind structural oracle start together
after Phases 12 and 13. The oracle proves the exact allowed setup trace,
single-epoch loop delegation, and absence of direct transport, SD, delay,
fallback, rebind, or extra-feed paths without reading `optical_main.cpp`.
Their shared-file coordinator and all production/fault builds are serialized.

### Phase 15 — Button-free flash tooling

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
  - name: agent-optical-flash-coordinator
    start-after: [agent-cdc-host-control, agent-optical-flash-tool, agent-optical-flash-independent-oracle]
    owned-paths:
      - firmware/scripts/test.sh
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

Lock the host CLI, injected command seams, USB-serial selection, RawHID
fallback, HalfKay detection, identity derivation, deadlines, and exit codes
before the three implementation/oracle slices start. Only the coordinator
touches shared runner/docs paths.

### Phase 16 — Acceptance runner and evidence schema

```yaml
unit: phase-task
seams: [USB heartbeat, HTTP status, UDP status, fault selection, audio oracle, JSONL receipt]
contract-artifact: spec-only
dependencies: [phase-9, phase-13, phase-15]
outcome: "One command executes a selected gate and durably records every attempt."
acceptance:
  - schema validation covers every enum, nullable field, and range
  - append tests cover interruption, short write, corrupt tail, and hash chain
  - fake fixture covers every gate result and preserves failed attempts
fan-out:
  - name: agent-recovery-receipt-kernel
    owned-paths:
      - firmware/tools/net/aius_recovery_receipt.py
      - firmware/test/recovery-runner/test_receipt.py
  - name: agent-recovery-observers
    owned-paths:
      - firmware/tools/net/aius_recovery_observers.py
      - firmware/test/recovery-runner/test_observers.py
  - name: agent-recovery-gates
    start-after: [agent-recovery-receipt-kernel, agent-recovery-observers]
    owned-paths:
      - firmware/tools/net/aius_recovery_acceptance.py
      - firmware/test/recovery-runner/test_gates.py
  - name: agent-recovery-shell
    owned-paths:
      - firmware/scripts/run_recovery_acceptance.sh
      - firmware/test/recovery-runner/test_cli.py
  - name: agent-recovery-runner-coordinator
    start-after: [agent-recovery-receipt-kernel, agent-recovery-observers, agent-recovery-gates, agent-recovery-shell]
    owned-paths:
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

Before fan-out, lock the exact CLI, the `flash|reboot|watchdog|adapter_fault|sd|soak|restore`
gate enum, fixture command protocols, timeout/evidence paths, RFC 8785
canonicalization, deterministic adapter-fault selector/build identity, and
audio-oracle executable/JSON contract. The receipt kernel and shell may start
in parallel; observers start independently once those prior contracts exist;
gates join receipt plus observers; only the coordinator touches `test.sh`.

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
  - {name: agent-live-preflight, gate: preflight}
  - {name: agent-live-flash, gate: flash, start-after: [agent-live-preflight]}
  - {name: agent-live-reboot, gate: reboot, start-after: [agent-live-flash]}
  - {name: agent-live-watchdog, gate: watchdog, start-after: [agent-live-reboot]}
  - {name: agent-live-adapter-fault, gate: adapter_fault, start-after: [agent-live-watchdog]}
  - {name: agent-live-sd, gate: sd, start-after: [agent-live-adapter-fault]}
  - {name: agent-live-soak, gate: soak, start-after: [agent-live-sd]}
  - {name: agent-live-restore, gate: restore, start-after: [agent-live-soak]}
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
The single fixture, USB enumeration, and shared fault build directory serialize
all mutating hardware groups in the listed order. Each group owns only its
gate-named receipt/transcript/artifact subtree and commits after restoring a
verified production image when it temporarily deploys a fault image.
Read-only validation for a completed group may overlap the next hardware
group. Selector builds and live checkpoint injections never run in parallel.
Final restore waits for every group and validator.

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
    read-only: true
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
Phase 18 deliberately remains one plan-wide reviewer after every evidence
commit and the verified production restore. Findings may fan out into
concern-sized correction slices, but the same complete reviewer reruns after
their targeted proofs.

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
- Phase 5 plan-to-build gate: failed before dispatch because the portable ABI
  lacked a normative transition/result oracle and incorrectly claimed target
  cutover and adapter scope. The contract now defines correlated actions,
  typed results, all job/mode sequences, path/arithmetic boundaries, buffer
  ownership, install/rollback authority, and timeout/cancellation cleanup. The
  first recheck also rejected overloaded internal state, contradictory zero-I/O
  faults, unsafe invalid-supply termination, unresolved side-effect authority,
  and ambiguous deadline precedence; explicit phases/counters and the final
  payload/correlation/cleanup rules above resolve those findings. The final
  sizing recheck rejected missing config accumulation, residual abort/id
  precedence, and one oversized agent; those rules are now total and Phase 5
  is split into an engine slice followed by an implementation-blind oracle
  slice. The definitive range recheck rejected an overloaded read
  `end_offset`; download/refill now derive it after file-size capture and fail
  out-of-file requests with the stable `RANGE` fault. The fresh recheck passed;
  Phase 5 is authorized for engine dispatch followed by the source-blind
  oracle slice. The first engine dispatch stopped before write because an
  unbounded streamed upload could not supply a complete `PUBLISH_CONFIG`
  buffer; publishing uploads are now config-sized and use an explicit
  post-commit reread before publication. The focused publication recheck
  passed and engine dispatch is authorized again. Engine authority review then
  found that an earlier abort could mask a later install-rename failure and
  wrongly delete its recovery temp; explicit `install_failed` authority now
  separates preservation from terminal-fault precedence. The focused
  install-authority matrix recheck passed.
- Phase 5 delivery: the native `sd` suite passes under strict C11,
  warnings-as-errors, ASan, and UBSan with 58 named phase/action fixtures, 910
  event/payload/authority matrix trials, and 15 lifecycle cases;
  `teensy41-optical` builds with all banks fitting. The source-blind oracle
  caught rollback terminal re-entry and suppressed action-id exhaustion
  notification in the engine. Two delivery reviews then rejected
  scenario-only mismatch coverage and missing append/finalize cancellation
  rows. The count-asserted cross-product oracle and durable-file cancellation
  sequences passed the final clean-room audit.
- Phase 6 delivery: the native `http` suite passes under strict C11,
  warnings-as-errors, ASan, and UBSan with 32 named transaction tests, 111
  natural authority/event rows, all 63 SD operation/result edges, 68
  unnamed-field mutations from valid SD baselines, 39 fixed-response capacity
  boundaries, real ACK deadline/wrap cases, and repeated restart and duplicate
  authority cases. The source-blind oracle exposed strict framing,
  pre-accept restart, SD result-matrix, transport-root, retained-dispatch,
  download-deadline, publication-bound, and streamed-upload backpressure
  defects before delivery. Four delivery audits rejected self-confirming or
  incomplete precedence, payload, capacity, and lifecycle proof; the final
  fresh audit passed. `teensy41-optical` explicitly compiles the HTTP codec,
  transaction engine, and SD engine, passes pinned NativeEthernet linked-symbol
  verification, and fits every memory bank.
- Phase 7 delivery: the native `usb-control` suite passes under strict C11,
  warnings-as-errors, ASan, and UBSan with count-asserted registries covering
  24 transition families, 36 stable variants, 12 malformed or unnamed TX
  rows, 12 unsolicited or duplicate input rows, 8 RX boundary rows, 21
  application-response rows, 6 wrap-aware deadline rows in both authorized
  `TX` and `ACK_PENDING`, and 4 reconnect transactions. The source-blind
  oracle proves exact replacement writes after partial, blocked, and malformed
  correlations; bootloader denial cannot retain entry authority. The first
  delivery audit rejected missing ACK-pending, replacement-write, and negative
  authorization proof; the strengthened oracle passed a fresh clean-room
  re-audit. `teensy41-optical` compiles the bounded runtime beside the unchanged
  legacy adapter, passes pinned NativeEthernet linked-symbol verification, and
  fits every memory bank.
- Phase 8 delivery: the native `runtime` suite passes under strict C11,
  warnings-as-errors, ASan, and UBSan with 10 literal descriptor rows, all
  65,526 invalid selectors, exact 20-event normal and 6-event recovery-safe
  traces, 11 fail-fast traces, 10 ops-validity rows, and 2 entry-snapshot alias
  mutation rows. The source-blind oracle proves stable budget pointers, exact
  checkpoint/service order, no feed after incomplete epochs, one feed after a
  complete epoch, null-context forwarding, and immunity to callback mutation
  of the caller's ops storage. The focused clean-room delivery audit passed.
  `teensy41-optical` explicitly compiles the stateless scheduler, passes pinned
  NativeEthernet linked-symbol verification, and fits every memory bank.
- Phase 9 delivery: the native `net` and Python `net-runner` suites pass
  against the two precommitted LFS contract fixtures. The C proof byte-matches
  the canonical 99-byte encoding and independent 71-byte prefix; exhausts
  capacities `0..98`, invalid versions and modes, all 65,526 invalid
  checkpoints, enum/fault domains, valid endpoints, and `UINT32_MAX` counters;
  and proves rejection leaves output unchanged. The Python proof accepts only
  71/99-byte payloads, exhausts protocol-representable invalid lengths,
  validates every recovery domain and reserved byte independently, preserves
  legacy keys/types, and maps historical mode `2` to `legacy-static`. The
  first plan gate rejected contradictory target scope, missing fixed fixtures,
  incomplete parser semantics, and an oversized slice; the amended,
  fixture-backed decomposition passed a fresh plan recheck and clean-room
  delivery audit. `teensy41-optical` compiles the portable encoder beside the
  still-legacy target emitter, passes pinned NativeEthernet linked-symbol
  verification, and fits every memory bank.
