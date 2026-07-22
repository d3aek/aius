---
title: AIUS USB control protocol
status: complete
owner: aius
---

# AIUS USB control protocol

The implemented operator reference is [USB application control protocol](../reference/usb-control-protocol.md).

## Scope

This contract defines the USB CDC control channel exposed by the optical
firmware. USB CDC means USB Communications Device Class: the Teensy presents a
virtual serial port, normally `/dev/ttyACM*`. It is the application-level
maintenance and local-control protocol. The Ethernet HTTP API and the separate
RawHID capability transport are out of scope.

## Design decisions

- The channel is line-oriented ASCII with `\n`, `\r`, or `\r\n` command
  termination. Firmware emits one response line per complete command.
- Every response has a stable status prefix and a request identifier, so host
  tools can correlate replies and safely ignore unsolicited device text.
- Commands are ASCII tokens with `key=value` arguments. Unknown commands and
  malformed arguments return structured errors rather than silence.
- The protocol is versioned by `AIUS PROTOCOL 1`; additions must be backward
  compatible, while incompatible changes require a new major version.
- Input is bounded at 127 bytes before the terminator. The implementation
  buffer is 128 bytes including NUL. Overflow enters discard-until-terminator
  mode and emits one `LINE_TOO_LONG` error, then resumes with the next line.
- Only printable ASCII bytes (`0x20` through `0x7e`) are valid command bytes.
  NUL, other controls, and bytes above `0x7e` discard the current line and
  return `BAD_ARGUMENT`. An empty line returns `BAD_COMMAND` with ID 0.
- The bootloader command, when implemented, is USB CDC-only. It is not exposed
  through HTTP or UDP.
- No command may perform unbounded work in the USB service path or audio
  interrupt. Long operations must report acceptance and complete asynchronously.

## Wire contract

Host request examples:

```text
AIUS PROTOCOL 1
AIUS ID 7
AIUS AUTH 8
AIUS BOOTLOADER 9
```

Response grammar:

```text
AIUS/1 OK <request-id> <payload>
AIUS/1 ERR <request-id> <error-code> <message>
```

`<request-id>` is a decimal unsigned 32-bit value in the range 1 through
4,294,967,295. `0` is reserved for requests without an identifier. Payloads
contain only printable ASCII and are space-delimited in protocol version 1.
Secrets must never appear in an error response or unsolicited diagnostic.

Application responses are identified by the `AIUS/1` prefix. Existing firmware
diagnostics may still appear on the CDC stream during boot and runtime, but
they are not protocol responses and host tools must ignore lines without that
prefix. New diagnostic lines must not include the auth token.

Required version-1 commands:

| Command | Response/purpose |
| --- | --- |
| `AIUS PROTOCOL 1` | Reports the supported CDC protocol version. |
| `AIUS ID <id>` | Returns firmware revision, build date, USB product mode, and protocol version. |
| `AIUS AUTH <id>` | Returns the physical-access SD token for the requested ID. The token is emitted only in the successful payload. |
| `AIUS BOOTLOADER <id>` | Emits the success response, flushes USB for a bounded interval, then enters HalfKay; CDC-only. |

Compatibility aliases may be retained temporarily: `AIUS AUTH` maps to ID `0`
and returns `AIUS/1 OK 0 AUTH <token>`. The legacy `AIUS AUTH <token>` response
is not retained because it cannot be correlated or self-described. The legacy
silent behavior for unknown commands is removed once this protocol ships.

Defined errors are `BAD_VERSION`, `BAD_COMMAND`, `BAD_ARGUMENT`,
`LINE_TOO_LONG`, `BUSY`, and `INTERNAL`. Error text is diagnostic only and is
not a machine-stable field.

## Safety and extensibility

The parser must reject control characters, embedded NUL, non-ASCII bytes,
duplicate arguments, and trailing tokens not defined by the command. New
commands must reserve a unique verb and document whether they are synchronous,
asynchronous, or state-changing. `AIUS BOOTLOADER` acknowledges before the
reset and has no network equivalent.

The command dispatcher is the only place that may invoke application actions.
HTTP and UDP dispatch tables must not contain a bootloader operation. If token
derivation is unavailable, AUTH returns `AIUS/1 ERR <id> INTERNAL
auth-unavailable` without secret material.

## Clean-room review resolution

The initial review was RED. This contract resolves its findings by specifying
the exact line bound and overflow recovery, byte validation, empty-line and
CR/LF behavior, response correlation, diagnostic interleaving, token
non-disclosure, bounded bootloader acknowledgement, and HTTP/UDP negative-route
requirements.

## Implementation phases

### Phase 1: contract and review

**unit:** central CDC wire contract and compatibility table  
**seams:** USB CDC line reader ↔ parser; parser ↔ command dispatcher  
**contract-artifact:** this document, command/error grammar, golden examples  
**fan-out:** spec owner; clean-room verifier; no firmware edits  
**review-intensity:** adversarial malformed input, overflow, legacy behavior,
secret leakage, and bootloader exposure  
**commit-checkpoint:** reviewed plan committed and pushed

**status:** complete. The reviewed contract was committed as `129d81562`.

### Phase 2: firmware implementation

**unit:** bounded application-command parser and dispatcher in the optical firmware  
**seams:** `Serial` adapter ↔ pure parser; dispatcher ↔ existing auth/reset
helpers  
**contract-artifact:** parser header/tests and response goldens  
**fan-out:** parser module and optical integration have disjoint paths  
**review-intensity:** full command/error matrix and no audio-path allocations  
**commit-checkpoint:** native protocol tests and clean target build

**status:** complete. Native `usb-control`, HTTP, config, and DSP suites pass;
the `teensy41-optical` target builds within all memory-bank budgets.

### Phase 3: tooling and validation

**unit:** USB operator tool and central operator documentation  
**seams:** `/dev/ttyACM*` ↔ request correlation; loader ↔ bootloader response  
**contract-artifact:** central reference page and operator command examples
**fan-out:** tool, docs, and target smoke tests are disjoint  
**review-intensity:** reconnect, stale replies, partial lines, and bootloader
transition  
**commit-checkpoint:** host tests, target build, and live USB smoke test

**status:** complete. The live target accepted PROTOCOL, ID, AUTH, and
structured-error commands. USB-only BOOTLOADER caused HalfKay enumeration,
the loader flashed the image, and Ethernet returned afterward.

## Acceptance criteria

- The current `AIUS AUTH` workflow remains compatible through the documented
  alias until its removal is explicitly scheduled.
- Unknown and malformed input always produces a bounded, parseable error.
- `AIUS BOOTLOADER` is accepted only on USB CDC and does not acquire an HTTP or
  UDP route.
- Host tests cover all commands, errors, line endings, overflow, invalid bytes,
  duplicate/trailing arguments, and response formatting. The dispatcher never
  sends the token on an error path.
- The target build remains within the existing flash, RAM, and audio timing
  budgets.

## Delivered evidence

- Native `usb-control`, HTTP, config, and DSP suites pass.
- `teensy41-optical` builds with 505,312 bytes flash used, 28,220 bytes FlexRAM
  free, and 501,216 bytes OCRAM free.
- Live target revision `3f70e4572995` returned structured CDC responses.
- Live `AIUS BOOTLOADER 9` transition was observed by `teensy_loader_cli` as
  `Found HalfKay Bootloader`, followed by successful programming and Ethernet
  recovery.
