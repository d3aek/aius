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
is reserved for commands without an ID. `AIUS AUTH` is retained as a legacy
alias and uses ID `0`.

## Responses

Successful responses:

```text
AIUS/1 OK 0 PROTOCOL 1
AIUS/1 OK 7 ID protocol=1 revision=abc123 build=2026-07-22T12:00:00Z mode=optical
AIUS/1 OK 8 AUTH 0123456789abcdef
AIUS/1 OK 9 BOOTLOADER entering
```

Errors use this form:

```text
AIUS/1 ERR <id> <error-code> error
```

Defined error codes are `BAD_VERSION`, `BAD_COMMAND`, `BAD_ARGUMENT`,
`LINE_TOO_LONG`, `BUSY`, and `INTERNAL`. Error text is diagnostic and not a
stable machine field. Secrets never appear in errors or diagnostics.

## Command behavior

| Command | Behavior |
| --- | --- |
| `AIUS PROTOCOL 1` | Reports protocol version 1. |
| `AIUS ID <id>` | Reports protocol version, firmware revision, build timestamp, and optical product mode. |
| `AIUS AUTH [<id>]` | Returns the firmware-derived SD maintenance token. The token is disclosed only on this physical USB channel. |
| `AIUS BOOTLOADER <id>` | Writes the success response, flushes USB for a bounded interval, then enters Teensy HalfKay. It has no HTTP or UDP equivalent. |

Commands are parsed in the main loop and never from the audio interrupt. The
bootloader action is dispatched only by the USB CDC command handler.

## Host procedure

1. Open the current `/dev/ttyACM*` at any conventional serial speed.
2. Send `AIUS AUTH\n` and read the `AIUS/1 OK 0 AUTH ...` response.
3. Correlate commands using their request ID and ignore non-`AIUS/1` lines.
4. To enter the loader without pressing the board button, send
   `AIUS BOOTLOADER <id>\n` and keep the loader waiting for VID/PID
   `16c0:0478`.

The command is a physical-access control boundary, not cryptographic
authentication. USB permissions and the enclosing host/guest topology remain
responsibilities of the operator.
