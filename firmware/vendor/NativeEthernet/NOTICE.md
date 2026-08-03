# NativeEthernet provenance and Aius patches

This directory is a source snapshot of the MIT-licensed `NativeEthernet`
library shipped in PlatformIO package `framework-arduinoteensy` version
`1.162.0`, selected by `teensy@5.2.0`.

Snapshot source:
`libraries/NativeEthernet/{src,LICENSE,library.json,library.properties}`.
Acquired from the locally installed immutable PlatformIO package on
2026-07-24. Examples and editor metadata are omitted because the firmware
build does not consume them. `LICENSE` is the unmodified upstream license.
`UPSTREAM_SHA256` records every copied file's immutable 1.162.0 baseline.
`PATCHED_SHA256` records the immutable expected result of every intentional
Aius patch, so later edits fail the source-contract suite until reviewed.
The only files intentionally different from those baselines are:

- `src/NativeEthernet.cpp`
- `src/NativeEthernet.h`
- `src/NativeEthernetClient.cpp`
- `src/NativeEthernetServer.cpp`
- `src/Nativesocket.cpp`
- `library.json` and `library.properties`, whose library name is changed to
  `AiusNativeEthernet` so PlatformIO can ignore the framework copy by name
  while resolving this snapshot explicitly.

The Aius patch is intentionally confined to the lifecycle primitives used by
the target adapter:

- `Ethernet.beginNoWait()` allocates library storage once, initializes FNET
  and link callbacks once, and does not start or wait for DHCP.
- `Ethernet.dhcpStart()` and `Ethernet.dhcpStop()` own the FNET DHCP
  descriptor. Link and DHCP callbacks only forward observations to registered
  user callbacks.
- all static-address `begin()` overloads return after configuration and the
  DHCP `begin()` compatibility overload starts DHCP without waiting for a
  lease.
- `EthernetClient::write()` observes current capacity once, attempts at most
  one send, and returns zero when no capacity is available.
- TCP sockets disable linger, and the socket send primitive contains no
  capacity wait.
- `EthernetServer::begin()` is idempotent. `EthernetServer::end()` unregisters
  its FNET service and closes its listener without waiting.
- `Ethernet.end()` stops DHCP and closes each library-owned socket without
  waiting. The allocated FNET stack remains initialized for deterministic
  retry without repeated heap allocation.
- the FNET service callback that accepts inbound connections initialises
  `fromlen` to `sizeof(from)`. Upstream leaves it **uninitialised** and passes it
  to `fnet_socket_accept()`, which refuses any length below
  `sizeof(struct fnet_sockaddr_in)` — zero included, via the trailing `else` in
  `_fnet_socket_addr_check_len()`. The accept therefore succeeded or failed on
  whatever the stack slot held, and a failure leaves the connection **queued**, so
  the symptom is a retried accept rather than a lost connection. This is the one
  patch here that fixes an upstream defect rather than adapting upstream behaviour
  to the Aius lifecycle; it is confined to one initialiser.
- `Ethernet.dhcpOptions()` exposes the FNET client's lease/T1/T2 option
  values (host byte order, seconds) for the aius periodic-WDOG
  investigation: the death schedule must be read from the client's own
  state, not reconstructed from observed cadence.
- `Ethernet.stackHeapStatus()` exposes the FNET netheap telemetry pair
  (free bytes; largest allocatable block) for the same investigation's
  count-anchor hypothesis: a per-connection netheap leak convicts or
  acquits on the served decline curve.
- every path into `fnet_poll()` routes through `EthernetClass::pollProbed()`
  (`src/NativeEthernet.cpp`, `src/NativeEthernetServer.cpp`), which maintains
  the retained `AiusPollProbe` (header in the firmware's
  `lib/aius_boot/include`, storage in the ldscript's `.aius_retained` window):
  fnet_poll is not reentrant, the port's FNET mutexes are no-ops, and upstream
  ran the whole service stack from a 1 kHz IntervalTimer ISR while main-loop
  server code also polled inline — convicted as the periodic DHCP-T1 renewal
  watchdog wedge (aius open-work register). With
  `AIUS_NET_POLL_SINGLE_CONTEXT=1` `beginNoWait()` arms no IntervalTimer at
  all and the application's network job drives the poll, making the stack
  single-context; at `0` the historic ISR model is preserved (instrumented)
  for on-device confirmation.

The native `network-transport` suite guards these source contracts and the
single temporary legacy FNET-include exception recorded there.
