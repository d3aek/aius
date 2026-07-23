---
title: Aius network and webserver hardening
status: in_progress
owner: firmware
---

# Aius network and webserver hardening

The deployed optical image must remain recoverable when DHCP, Ethernet link
state, a TCP peer, or the SD card behaves badly. No network or web operation
may permanently stop the foreground loop or require a reflash.

## Contract

- `unit`: foreground network supervisor, bounded HTTP transport, watchdog
  recovery, and native regression tests.
- `seams`: NativeEthernet lifecycle ↔ service loop; TCP writes ↔ bounded HTTP
  response state; loop heartbeat ↔ hardware watchdog; reset reason ↔ status.
- `contract-artifact`: this plan, fixed service budgets, recovery counters, and
  native tests for timeout, disconnect, link loss, and repeated startup.
- `fan-out`: one owner for firmware lifecycle and transport changes; one owner
  for native test coverage; parent owns live flash and target validation.
- `review-intensity`: adversarial review of blocking calls, reset loops, link
  flapping, partial responses, DHCP retry, and audio-loop interference.
- `commit-checkpoint`: implementation and tests build cleanly, target image
  fits, then live recovery evidence is recorded here.

## Phases

1. Inventory blocking calls and reproduce the failure shape. **Complete:**
   foreground HTTP writes and absent link recovery are confirmed risks.
2. Add bounded network lifecycle and HTTP transport recovery. Keep one client,
   reject stalled peers, abort partial SD jobs, restart DHCP/network after link
   loss, and retain static fallback. **Implemented:** link-debounced network
   epoch reset, two bounded DHCP attempts, bounded response writes, and
   disconnect cleanup.
3. Add a hardware watchdog and reset diagnostics. Feed it only from the main
   loop after critical services have returned; expose reset cause and recovery
   counters in status. **Implemented:** WDOG1 startup/feed and status fields.
4. Add native tests, build the optical target, flash, and validate link loss,
   DHCP retry, malformed/stalled HTTP, reboot, and long control traffic while
   audio/replay continues. **In progress:** HTTP/net/replay native tests and
   target build pass; the firmware now services HTTP before secondary UDP
   traffic, feeds the watchdog at that seam, and rebinds a lost listener.
   Live HTTP validation is still pending. The latest image was built and the
   loader detected the 16c0:0478 bootloader, but the write failed and the board
   returned to 16c0:0483 application mode. The next session should use the
   loader's explicit wait mode while the Program button is pressed, then verify
   the image revision over USB and probe `/api/status` at the DHCP lease.

## Next-session handoff

- Current pushed firmware checkpoint: HTTP-first loop, watchdog checkpoint,
  listener rebinding, and `http_server_rebinds` telemetry.
- Build command: `firmware/scripts/build.sh teensy41-optical`.
- Focused tests: `firmware/scripts/test.sh --environment
  native --suite http` and `--suite net`.
- Image: `firmware/.pio/build/teensy41-optical/firmware.hex`.
- Flash command, using the guest's `plugdev` access:
  `sg plugdev -c "$HOME/.platformio/packages/tool-teensy/teensy_loader_cli
  --mcu=imxrt1062 -w -v
  firmware/.pio/build/teensy41-optical/firmware.hex"`.
- Do not infer a routing fault from the HTTP timeout. The current working
  hypothesis is firmware startup/service failure or an unreliable bootloader
  transfer over the USB-over-fibre path.

## Non-goals

The watchdog is a last-resort recovery mechanism, not permission for unbounded
network calls. Audio processing remains independent of Ethernet and SD I/O.
Production TLS, authentication policy, and NTP remain separate concerns.
