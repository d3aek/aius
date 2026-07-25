---
title: AIUS USB application control protocol
status: active
---

# AIUS USB application control protocol

This is the application-level control protocol on the Teensy USB virtual
serial port. USB CDC means USB Communications Device Class. Linux normally
exposes this port as `/dev/ttyACM*`.

This document does not describe the separate RawHID capability transport, the
Ethernet HTTP API, or the Teensy HalfKay wire protocol.

## Transport

- ASCII, line-oriented commands.
- `\n`, `\r`, and `\r\n` terminate a command.
- A command contains at most 127 bytes before its terminator.
- Bytes outside printable ASCII (`0x20` through `0x7e`) are rejected.
- An overlong line is discarded through its terminator and produces one
  `LINE_TOO_LONG` error.
- Responses are ASCII lines beginning with `AIUS/1`.
- Other serial output is diagnostic text and must be ignored by protocol tools.

## Requests

```text
AIUS PROTOCOL 1
AIUS ID 7
AIUS AUTH 8
AIUS BOOTLOADER 9
```

IDs are unsigned decimal 32-bit values from 1 through 4,294,967,295. ID `0`
is reserved for commands without an ID. The ID-less form `AIUS AUTH` is retained
as a legacy alias and answers with ID `0`; `AIUS AUTH <id>` answers with the ID
it was given.

## Responses

Successful responses:

```text
AIUS/1 OK 0 PROTOCOL 1
AIUS/1 OK 7 ID protocol=1 revision=abc123 build=2026-07-22T12:00:00Z mode=optical ip=192.168.1.42 rsafe=0 nstate=2 nfault=0 ckpt=9 early=0 ngen=1 bgen=3 rsrc=64 epoch=48211 nrst=0
AIUS/1 OK 8 AUTH 0123456789abcdef
AIUS/1 OK 9 BOOTLOADER entering
```

The `ID` payload is described field by field in
[§The ID response](#the-id-response).

Errors use this form:

```text
AIUS/1 ERR <id> <error-code> error
```

Defined error codes are `BAD_VERSION`, `BAD_COMMAND`, `BAD_ARGUMENT`,
`LINE_TOO_LONG`, `BUSY`, and `INTERNAL`. Error text is diagnostic and not a
stable machine field. Secrets never appear in errors or diagnostics.

## The ID response

`ID` reports the build identity, the leased address, and the whole recovery
snapshot as space-separated `key=value` pairs. It carries the recovery state
because recovery-safe mode services bounded USB only — Ethernet stays down, so
this is the one channel that still answers when the board is in it.

Identity and address fields:

- **`protocol`** — always `1`, the protocol version this document describes.
- **`revision`** — firmware build revision, stamped at compile time.
- **`build`** — build timestamp, stamped at compile time.
- **`mode`** — always `optical` for this image.
- **`ip`** — leased IPv4 address as a dotted quad. Always emitted; `0.0.0.0`
  when no lease is held.

### Recovery fields

**The ten recovery fields are one snapshot served over three channels.**
`GET /api/status`, the 99-byte UDP status payload (bytes 71 through 98), and
this line all read the same `AiusRecoveryStatus`, so each short key below names
exactly the same value as the JSON key and the UDP field beside it. The keys are
abbreviated only because the line has a byte bound; nothing longer fits.

| `ID` key | Status JSON key and UDP field | Domain |
| --- | --- | --- |
| `rsafe` | `recovery_safe` | `0` or `1` |
| `nstate` | `network_state` | `0`–`5`, network state below |
| `nfault` | `network_last_fault` | `0`–`10`, fault below |
| `ckpt` | `last_checkpoint` | `0`–`9`, checkpoint below |
| `early` | `watchdog_early_resets` | `0`–`255`, saturating |
| `ngen` | `network_generation` | unsigned 32-bit decimal |
| `bgen` | `boot_generation` | unsigned 32-bit, saturating |
| `rsrc` | `reset_source` | unsigned 32-bit, raw `SRC_SRSR` |
| `epoch` | `loop_epoch` | unsigned 32-bit, completed epochs |
| `nrst` | `network_restarts` | unsigned 32-bit, saturating |

`rsafe` is the recovery-safe flag: `1` means the runtime selected recovery-safe
mode, so Ethernet and SD mutation stay disabled and the only exit is a verified
new image entered through `AIUS BOOTLOADER`. `early` reaching `2` is what
selects that mode.

`nstate` is `AiusNetState`, from
[`aius_net.h`](../../firmware/lib/aius_net/include/aius_net.h):

| Value | State |
| --- | --- |
| `0` | `STACK_START` |
| `1` | `DHCP_WAIT` |
| `2` | `ONLINE` |
| `3` | `TEARDOWN` |
| `4` | `BACKOFF` |
| `5` | `RECOVERY_SAFE` |

`nfault` is `AiusNetFault` (same header): `0` none, `1` DHCP timeout, `2` link
lost, `3` stack start, `4` DHCP start, `5` listener, `6` UDP, `7` socket,
`8` teardown, `9` callback overflow, `10` driver protocol.

`ckpt` is the last checkpoint persisted before a service call, one of the
`AIUS_BOOT_CHECKPOINT_*` values in
[`aius_boot.h`](../../firmware/lib/aius_boot/include/aius_boot.h): `0` reset
capture, `1` USB RX,
`2` USB TX, `3` metrics, `4` network, `5` HTTP, `6` UDP, `7` replay, `8` SD,
`9` epoch complete. Only `9` marks a completed epoch; any other value on a
watchdog reset is an early reset.

### Fields this line does not carry

**`link`, `dhcp_pending`, `dhcp_discovers`, and `dhcp_updates` are not on the
`ID` line.** They were until the recovery fields landed, and their removal is
deliberate rather than only a budget consequence. All four describe a running
Ethernet stack, and the one state where `ID` is the only channel is
recovery-safe mode, where the stack is down and all four are meaningless.
`GET /api/status` serves all four whenever the stack is up. `nstate` and
`nfault` also say strictly more about a stalled lease — `DHCP_WAIT` versus
`BACKOFF`, and which fault caused it — than `link` and `dhcp_pending` ever did,
and `dhcp_discovers` has had no producer since the optical runtime cutover and
always read zero.

### Bound and failure mode

**The `ID` payload is bounded at 256 bytes and fails closed rather than
truncating.** The bound is the dispatcher's `AIUS_OPTICAL_APP_USB_PAYLOAD_MAX`.
The firmware's worst-case payload length is summed field by field from the
domains above and checked by a compile-time `static_assert`, so overrunning the
bound is a build failure, not a wire event. At runtime, a payload that does not
fit, is empty, or contains a non-printable byte yields no payload at all and the
device answers `AIUS/1 ERR <id> INTERNAL error`. A host never sees a truncated
or partial `ID` line.

## Command behavior

| Command | Behavior |
| --- | --- |
| `AIUS PROTOCOL 1` | Reports protocol version 1. |
| `AIUS ID <id>` | Reports build identity, the leased address, and the ten recovery fields. See [§The ID response](#the-id-response). |
| `AIUS AUTH [<id>]` | Returns the firmware-derived SD maintenance token. The token is disclosed only on this physical USB channel. |
| `AIUS BOOTLOADER <id>` | Writes the success response, drains USB for a bounded interval, then enters Teensy HalfKay. See [§Bootloader entry](#bootloader-entry). It has no HTTP or UDP equivalent. |

Commands are parsed in the main loop and never from the audio interrupt. The
bootloader action is dispatched only by the USB CDC command handler.

## Bootloader entry

**The response is transmitted to the host before the device leaves the
application, and the command is repeatable.** Entering HalfKay halts the core or
shuts USB down, so a handover that races the in-flight transfer both loses the
response and usually fails to enter the loader at all. The device therefore
performs a real drain, not a fixed settling delay.

The observable sequence after `AIUS BOOTLOADER <id>`:

1. `AIUS/1 OK <id> BOOTLOADER entering` is written to the CDC endpoint.
2. The device submits the buffered packet and waits until the USB transmit ring
   reports every queued transfer retired by the host — the host has the bytes.
3. It enters HalfKay. The port disappears and the device re-enumerates as
   VID/PID `16c0:0478`.

**Each wait is bounded at 500 ms and never blocks indefinitely.** A host that
stops accepting data delays entry by at most 500 ms while the response is being
written, and at most a further 500 ms while it is being drained; the device then
enters the loader regardless. The 8-second watchdog window covers both.

**A failed entry leaves the device serving, and the next request is answered.**
If the transition returns instead of reaching HalfKay, the control transaction is
released rather than latched: sending `AIUS BOOTLOADER <id>` again produces a new
`AIUS/1 OK <id> BOOTLOADER entering` and a fresh attempt. Earlier firmware
answered the first request and then silently ignored every later one, which left
the physical Program button as the only remaining route into the loader.

## Host procedure

1. Open the current `/dev/ttyACM*` at any conventional serial speed.
2. Send `AIUS AUTH\n` and read the `AIUS/1 OK 0 AUTH ...` response.
3. Correlate commands using their request ID and ignore non-`AIUS/1` lines.
4. To enter the loader without pressing the board button, send
   `AIUS BOOTLOADER <id>\n` and keep the loader waiting for VID/PID
   `16c0:0478`. If the port is still present a second later, send the command
   again with a new ID — see [§Bootloader entry](#bootloader-entry).

The command is a physical-access control boundary, not cryptographic
authentication. USB permissions and the enclosing host/guest topology remain
responsibilities of the operator.
