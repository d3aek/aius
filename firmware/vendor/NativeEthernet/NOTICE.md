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

The native `network-transport` suite guards these source contracts and the
single temporary legacy FNET-include exception recorded there.
