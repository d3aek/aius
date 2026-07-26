---
title: Aius TLS termination
type: plans
scope: component
component: aius
status: draft
subtype: design
tags: [aius, firmware, teensy, tls, mbedtls, fnet, sd, security, http]
updated: 2026-07-26
toc: true
---

# Aius TLS termination

Whether the optical firmware can terminate real TLS with a certificate and key
loaded from the SD card, serving HTTPS **only** when a valid pair is present and
falling back to today's plain HTTP when the card, the cert, or the key is
missing or unusable. Investigated 2026-07-26 at the owner's request.

**Verdict: feasible with caveats.** The handshake is genuinely resumable inside
the bounded epoch loop and the crypto cannot starve the audio path — both were
the disqualifying risks and both are cleared with source evidence. What remains
is a memory-placement problem with a measured solution, an integration seam that
does not exist yet, and a host-tooling rewrite that is the largest slice of the
work.

Companion to [`aius-network-hardening.md`](aius-network-hardening.md), whose
§Deciding principle governs, [`aius-ota-upgrade.md`](aius-ota-upgrade.md), and
[`firmware-io.md`](../reference/firmware-io.md).

## The requirement

- Certificate and private key live on the SD card.
- Missing SD, missing cert file, missing key, or an unusable pair: boot and
  serve plain HTTP on port 80 exactly as today.
- Valid pair: **listen TLS only.** No plain-HTTP listener at all.
- The cert is issued by the owner's existing CA behind `example.net`, for a stable
  DNS name (assume `aius.example.net`; the exact name is an operator choice).
- **No client skips verification.** Every host tool verifies against that CA.
- The private key sits unencrypted on FAT32. That risk is accepted, not
  mitigated (§Key at rest).

## Verdict summary

| Question | Answer | Evidence quality |
|---|---|---|
| Can the handshake be driven without blocking an epoch? | Yes | measured from source |
| Can crypto starve the 93.75 blocks/s audio path? | No, with a live carrier | measured from source |
| Does the memory fit? | Yes with a repo-owned mbedTLS config and linker script; no without | measured footprints, one unresolved bank figure |
| Is there hardware crypto acceleration? | No, pure software | measured |
| Does "TLS only" break the tooling? | Yes, seven files plus the B6 gate | enumerated |
| Is the build guard deletable? | No, and it must not be | read |

## The handshake is resumable, and this is the finding that decides it

**`fnet_tls_socket_connect()` blocks, and it is the wrong function.** It spins
until the handshake completes, with no yield to anything but the FNET timer
(`service/tls/fnet_tls.c:361-393` in the framework package):

```c
do
{
    res = mbedtls_ssl_handshake(tls_sock);
    ...
}
while((res == MBEDTLS_ERR_SSL_WANT_READ) || (res == MBEDTLS_ERR_SSL_WANT_WRITE));
```

Reading only that function would give a BLOCKED verdict. But it is **client-only
and optional**, its own header says so (`fnet_tls.h:218-219`: *"It is optional,
as the handshake may be done inside fnet_tls_socket_recv() or
fnet_tls_socket_send() call if needed"*), and the server path never calls it.
`NativeEthernetServer.cpp:342-350` creates the TLS socket on accept and stops
there.

**The resumable path is `fnet_tls_socket_recv()`, and it is non-blocking by
construction.** Four facts, each read from source:

1. `mbedtls_ssl_read()` drives the handshake if one is pending, and returns its
   result verbatim (`mbedtls-2.12.0/src/ssl_tls.c:7181-7183`).
2. `mbedtls_ssl_handshake()` is a loop over `mbedtls_ssl_handshake_step()` that
   **breaks on the first non-zero return** (`ssl_tls.c:6958-6964`). A step that
   needs bytes returns `MBEDTLS_ERR_SSL_WANT_READ`, so the loop exits and the
   context keeps its state for the next call.
3. The BIO read callback maps FNET's "nothing yet" to `WANT_READ` rather than
   waiting (`fnet_tls.c:501-517`), and FNET's `fnet_socket_recv` never blocks —
   a connected socket with an empty buffer returns 0, which the Aius adapter
   already documents at `src/aius_network_teensy.cpp:306-315`.
4. `fnet_tls_socket_recv()` collapses `WANT_READ`/`WANT_WRITE` to a 0 return
   (`fnet_tls.c:429-432`), which is exactly the would-block value the epoch
   loop's HTTP checkpoint already understands.

So the handshake advances one step-group per poll and returns. **A polling epoch
loop is the natural driver for this API, not an obstacle to it.**

### What one poll actually costs

The handshake does *not* decompose into ten equal slices. `mbedtls_ssl_handshake`
runs every step that can proceed without new input, so the server's whole output
flight lands in one call. Two contiguous compute bursts result:

- **Burst 1** — after `ClientHello`: `ServerHello`, `Certificate`,
  `ServerKeyExchange`, `ServerHelloDone`. Contains the ECDHE ephemeral keypair
  generation **and** the ECDSA signature: two P-256 scalar multiplications.
- **Burst 2** — after `ClientKeyExchange`: the ECDH shared-secret computation,
  one more P-256 scalar multiplication, plus `ChangeCipherSpec` and `Finished`.

**Estimated cost, and it needs measuring:** 20-45 ms per P-256 scalar
multiplication, so roughly 50-100 ms for burst 1 and 25-50 ms for burst 2, total
**75-160 ms of contiguous CPU per handshake**. That range is an *estimate* from
published mbedTLS figures scaled to 600 MHz, adjusted for this build's
deliberately slow ECP settings (§No hardware acceleration). It is the single
number that most changes the shape of this design and it must come from the
device, not from this document. See [§Go/no-go gate](#gono-go-gate).

### Why that does not threaten the watchdog

- The watchdog period is **8 s**, fed exactly once after a complete epoch
  (`lib/aius_runtime/src/aius_runtime.c:97-100`; the `WT(15)` register write is
  at `src/aius_boot_teensy.cpp:404-408`).
- Checkpoint 5 (`HTTP`) has **no wall-clock budget at all** —
  `service_budget_ms` is 0 in the runtime budget table
  (`lib/aius_runtime/src/aius_runtime.c:28`), and 0 means "not bounded by this
  field" (`aius_runtime.h:31`).
- **A single epoch has already been measured at 1,016,348 µs on this board** and
  the image survived: a per-job SD remount, attributed at
  `src/optical_main.cpp:226` and `:1622-1624`. A 160 ms crypto burst is an order
  of magnitude *smaller* than a stall this firmware already absorbs.

A 160 ms burst is 2 % of the watchdog window. The watchdog is not the constraint.

### Why that does not starve the audio path

Three independent reasons, in descending order of importance:

- **With a live S/PDIF carrier the epoch loop is not in the audio path at all.**
  Receive DMA ISR → `AiusAudioProcessor::update()` → IEC 61937 de-framing → DTS
  decode → DSP → `enqueue()` → the `g_aius_spdif_tx24` ring → transmit DMA ISR.
  `loop()`'s own comment says it (`src/optical_main.cpp:3913-3915`): *"The audio
  path reaches the processor through `AudioStream`'s interrupt, never through
  here, so nothing in this function runs per block."* A crypto burst in thread
  context is invisible to it.
- **The crypto runs in thread context with interrupts enabled.** `fnet_tls.c`
  wraps every entry point in `fnet_service_mutex_lock()`, and
  `FNET_CFG_MULTITHREADING` is 1 (`fnet_user_config.h:13`), so those calls are
  real — but the mutex API NativeEthernet supplies is **four empty functions**
  (`vendor/NativeEthernet/src/NativeEthernet.cpp:401-412`). Nothing is disabled,
  nothing is masked. The audio DMA ISR at NVIC 128 and the software audio ISR at
  208 both preempt the burst freely. **This was the one plausible disqualifier
  and it is cleared.**
- **Replay playback, the only epoch-loop-fed audio, has ~341 ms of slack.** The
  replay word ring is 65,536 words = 131,072 B with a 32,768-word start
  watermark (`lib/aius_replay/include/aius_replay.h:40-44`), so at the moment a
  refill is planned there are 65,536 B = 341 ms of audio buffered at
  192,000 B/s. A 160 ms burst consumes 47 % of that slack. Tight but survivable,
  and worth a boundary check rather than an assumption.

### The real constraint is the transaction timeouts, not the watchdog

`lib/aius_http/src/aius_http_transaction.c:24-25`:

```c
#define REQUEST_TIMEOUT_MS UINT32_C(150)
#define RESPONSE_TIMEOUT_MS UINT32_C(100)
```

A handshake estimated at 75-160 ms sits astride both. **These constants, not the
8 s watchdog, are what a naive integration would break**, and they exist to
detect a wedged peer — raising them weakens exactly the property
`aius-network-hardening.md` was written to establish.

**Design answer: the handshake completes outside the transaction's timed
states.** The adapter owns a new pre-transaction phase — accept, create the TLS
socket, poll `fnet_tls_socket_recv` until it yields plaintext or fails — and only
then hands a client to `aius_http_transaction`. That phase carries its own
deadline (propose 3,000 ms, comfortably inside the watchdog and generous against
a 160 ms handshake plus network round trips) and its own failure counter. The
existing timeouts stay untouched, and the transaction never sees a socket that
has not finished its handshake.

### The integration seam does not exist today, and that is good news

**The Aius HTTP adapter does not use `EthernetClient` at all.** It calls FNET
directly on the raw socket (`src/aius_network_teensy.cpp:295` and `:391`):

```c
const fnet_ssize_t result =
    fnet_socket_recv(Ethernet.socket_ptr[socket], buffer, capacity, 0);
```

So flipping `FNET_CFG_TLS` on would **not** give this firmware TLS. Every TLS
fork in the vendored library lives in `EthernetClient::available()` and
`EthernetClass::socketSend()`, both of which this firmware bypasses. The adapter
must call `fnet_tls_socket()`, `fnet_tls_socket_recv()`, `fnet_tls_socket_send()`
and `fnet_tls_socket_close()` itself.

That is more work, and it is the better outcome, because the vendored TLS path
carries three defects this firmware would otherwise inherit:

- **A would-block is latched as a permanent write error.**
  `fnet_tls_socket_send()` returns 0 for `WANT_WRITE`; `socketSend()` turns any 0
  into 0 (`Nativesocket.cpp:327-334`); `EthernetClient::write()` then calls
  `setWriteError()` on it (`NativeEthernetClient.cpp:197-199`). The adapter's own
  comment at `aius_network_teensy.cpp:378-390` already records why it stopped
  using that path for plain TCP.
- **`socketSendAvailable()` measures the raw socket**, `SO_SNDBUF` minus
  `SO_SNDNUM` (`Nativesocket.cpp:337-349`), which is meaningless as a TLS
  plaintext budget once record headers, MAC and padding are in the way.
- **A failed handshake is invisible.** `fnet_tls_socket_recv` returns `FNET_ERR`
  for every mbedTLS failure alike; `EthernetClient::available()` prints a
  diagnostic to `Serial` from the epoch loop and returns 0
  (`NativeEthernetClient.cpp:226-251`) without closing. On a one-client-at-a-time
  server that is a wedged listener.

The adapter must therefore classify `FNET_ERR` from a TLS socket as **close the
connection and count it**, never as would-block. This is the single most likely
place for a TLS integration to produce a hang rather than an error.

**A partial-write caveat to design against:** mbedTLS requires that a
`mbedtls_ssl_write` returning `WANT_WRITE` be retried with the *same buffer and
length*. `fnet_tls_socket_send` erases the distinction, so the adapter's send
path must retain the exact pending slice across epochs rather than recomputing
it. The transaction already retains its I/O result across epochs
(`src/aius_runtime_network_http.cpp:37`), so the mechanism exists.

## No hardware acceleration — this build is pure software

**The i.MX RT1062's DCP co-processor is not used, and nothing in the tree could
use it.** Measured:

- Every `_ALT` hook in the mbedTLS config is commented out — `MBEDTLS_AES_ALT`,
  `MBEDTLS_SHA256_ALT`, `MBEDTLS_ECP_ALT`, `MBEDTLS_ECDSA_SIGN_ALT`,
  `MBEDTLS_ECP_INTERNAL_ALT` and the rest (`config.h:318-460`).
- `DCP` appears in the Teensy core **only** as register definitions in
  `cores/teensy4/imxrt.h` (`IMXRT_DCP_ADDRESS`, the `IRQ_DCP0..2` vectors marked
  `// TODO: ???`). There is no driver.
- `MBEDTLS_ENTROPY_HARDWARE_ALT` is off and `MBEDTLS_NO_PLATFORM_ENTROPY` is on,
  so the *only* entropy source is `fnet_rand()`, added with a 4-byte threshold
  and labelled `MBEDTLS_ENTROPY_SOURCE_STRONG` (`fnet_tls.c:143`, `:531-545`).
  **That label is a claim FNET has not earned** and it is worth a hard look
  before this ships (§Security posture).

What the build *does* have is the ARM `umlal` bignum inner loop —
`MBEDTLS_HAVE_ASM` is on (`config.h:81`) and the thumb2 branch at
`bn_mul.h:642-666` is selected, with `MULADDC_CANNOT_USE_R7` suppressed because
the project builds `-O2`. Plus `MBEDTLS_ECP_NIST_OPTIM` (`config.h:688`).

**Two settings make the ECP path deliberately slow**, both chosen for memory:

```c
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0 /* To reduce peak memory usage */
#define MBEDTLS_MPI_WINDOW_SIZE     1
#define MBEDTLS_ECP_WINDOW_SIZE     2
```

`config.h:47,50-51`. Window 2 against a default of 6, and no fixed-point comb
table, is roughly 1.5-2.5× slower per scalar multiplication than stock mbedTLS.
Both are candidates to relax once the memory budget is settled — raising
`MBEDTLS_ECP_WINDOW_SIZE` costs heap, not DTCM, and OCRAM has room. **Treat this
as the first lever if the measured handshake cost comes in at the top of the
estimated range.**

`MBEDTLS_MPI_WINDOW_SIZE 1` is the *worst possible* setting for RSA modular
exponentiation, which is one of several reasons RSA loses (§Key type).

## Memory — the blocker, and it has a measured solution

### Where the cost actually comes from

The `platformio.ini:77-83` record is that turning `FNET_CFG_TLS` on costs
**199,680 B FLASH, 19,136 B DTCM and 3,792 B ITCM**. That is a true measurement
of an outcome, but it does not say *what* the DTCM is, and the decomposition is
what makes the problem tractable.

**Measured 2026-07-26** by compiling all 79 mbedTLS translation units and
`sizes.c` struct probes with the project's own toolchain
(`toolchain-gccarmnoneeabi-teensy`, GCC 15.2.1) at `-O2 -mcpu=cortex-m7 -mthumb
-mfloat-abi=hard -mfpu=fpv5-d16`, then reading section and symbol sizes. No
PlatformIO env was built.

| Contributor | DTCM bytes | Provenance |
|---|---|---|
| mbedTLS `.rodata` (all TUs, pre-gc) | 25,649 | measured |
| mbedTLS `.data` + `.bss` (all TUs) | 696 | measured |
| `fnet_tls_if_list[2]` | 3,368 | computed from measured struct sizes |
| `fnet_tls_socket_if_list[3]` | 792 | computed |
| mbedTLS `.text` (would land in ITCM) | **0** | measured |

**`.rodata` is the dominant term, and it is in DTCM because of the linker
script, not because anything needs it there.** `cores/teensy4/imxrt1062_t41.ld:55-60`:

```text
.data : {
    *(.endpoint_queue)
    *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.rodata*)))
    *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.data*)))
    KEEP(*(.vectorsram))
} > DTCM  AT> FLASH
```

Every `const` not annotated `PROGMEM` is copied into the scarcest bank at boot.
The package config already routes the big tables to flash via
`#define MBEDTLSPROGMEM PROGMEM` and all code via
`#define MBEDTLSFLASHMEM FLASHMEM` (which is why mbedTLS costs zero ITCM), but
it leaves 25,649 B of unannotated `.rodata` behind. The top consumers, measured:
`error.c` 10,460, `ssl_ciphersuites.c` 3,581, `oid.c` 2,503, `x509_crt.c` 1,895,
`bignum.c` 1,150, `rsa.c` 975.

**The 3,792 B of ITCM is not mbedTLS.** It is `fnet_tls.c` plus the
`#if FNET_CFG_TLS` branches in NativeEthernet, none of which carry `FLASHMEM`.

### The ITCM cliff, which is the sharpest number here

`imxrt1062_t41.ld:104-106`:

```text
_itcm_block_count = (SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) + 0x7FFF) >> 15;
_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15);
```

**ITCM is allocated in 32 KiB blocks and every block subtracts 32,768 B from the
DTCM stack top** — that is, straight out of free-for-locals. So new ITCM is free
until it crosses a block boundary, then it costs 32 KiB of DTCM at once.

At revision `9f5b8dee8767` ITCM had **23,028 B free** inside its current 6 blocks
(`firmware-io.md:39`), so 3,792 B fits with room. That is consistent with the
recorded DTCM delta being 19,136 rather than 51,904. **But that headroom moves
with every commit, and where the image sits relative to the next boundary is a
gate item, not an assumption.** Forcing `fnet_tls.c` to flash removes the
question entirely and is nearly free.

### Trimming the config, validated

`MBEDTLS_CONFIG_FILE` is honoured by every mbedTLS header
(`x509_crt.h:27-31` and the same block in all of them), so
`-DMBEDTLS_CONFIG_FILE='"aius_mbedtls_config.h"'` plus an include path makes the
repo the owner of the entire mbedTLS configuration **without editing the
package**. That answers the "the config header lives outside the repo" problem
properly: the override is a build flag and a checked-in header, both pinnable and
both testable.

**Measured 2026-07-26**, deriving a TLS-1.2-only / ECDHE-ECDSA-only / P-256-only
config from the package one and rebuilding all 79 TUs against it:

| Section | Package config | Trimmed config | Delta |
|---|---|---|---|
| `.rodata` → DTCM | 25,649 | 4,650 | −20,999 |
| `.data` + `.bss` → DTCM | 696 | 132 | −564 |
| **DTCM subtotal** | **26,345** | **4,782** | **−21,563** |
| `.text` → ITCM | 0 | 0 | 0 |
| `.progmem` → FLASH | 49,544 | 12,140 | −37,404 |
| `.flashmem` → FLASH | 147,372 | 74,460 | −72,908 |

**All 79 files compile clean.** `check_config.h` forced exactly one additional
macro off beyond the wanted set — `MBEDTLS_X509_RSASSA_PSS_SUPPORT` — which is
itself the evidence that the trim is coherent rather than lucky. The derivation
script and the resulting header are reproducible; they belong in
`firmware/test/` and `firmware/config/` respectively when this is built.

Turned off: TLS 1.0, TLS 1.1, all of DTLS, every key exchange except
`ECDHE_ECDSA`, CBC/CFB/CTR modes, RSA, DHM, Camellia, ARIA, DES, XTEA, CCM,
CMAC, MD2/MD4/MD5, RIPEMD-160, SHA-1, every curve except P-256, X.509 CRL/CSR/
write, PEM write, PKCS#5/12, `mbedtls_strerror`, self-tests, renegotiation,
session tickets, the session cache, truncated HMAC, the fallback SCSV, SNI, and
ALPN.

That list *is* the hardening requirement, discharged at compile time rather than
by a runtime call. **Which matters, because FNET gives the application no runtime
lever at all**: `struct fnet_tls_if` is defined only inside `fnet_tls.c:51-63`
and `fnet_tls_desc_t` is an opaque `void *`, so the application cannot reach
`mbedtls_ssl_conf_min_version` or `mbedtls_ssl_conf_ciphersuites`. FNET's own
`min_version` call is `#if 0`'d with a `//TBD conf parameter` comment
(`fnet_tls.c:165-167`). Compile-time is not merely the better lever here, it is
the only one.

### Placement plan

**DTCM (`.data` / `.bss`), the scarce bank:**

| Allocation | Bytes | Note |
|---|---|---|
| mbedTLS `.rodata` + statics, trimmed | ≤ 4,782 | measured ceiling, pre-gc |
| `fnet_tls_if_list[1]` | 1,672 | `FNET_CFG_TLS=1`, cache context trimmed out |
| `fnet_tls_socket_if_list[1]` | 264 | `FNET_CFG_TLS_SOCKET_MAX=1` |
| NativeEthernet TLS members on globals | ~16 | `g_client`, `g_listener` |
| **Total** | **≤ 6,734** | down from 19,136 |

`FNET_CFG_TLS` defaults to 2 and `FNET_CFG_TLS_SOCKET_MAX` to 3
(`fnet_user_config.h:444`, `service/tls/fnet_tls_config.h:56`). This firmware
serves **one** client at a time (`src/aius_network_teensy.cpp:181,282`), so both
should be pinned to 1. That is 1,948 B rather than 4,160 B, and it is exactly the
same class of pin as the existing `FNET_CFG_TLS=0`.

Push mbedTLS `.rodata` and `fnet_tls.c`'s `.text` to flash with an in-repo
linker script and the DTCM figure drops again, toward ~2,000 B. PlatformIO's
Teensy builder honours `board_build.ldscript`
(`platforms/teensy/builder/frameworks/arduino.py:179-180`), so a checked-in
derivative of `imxrt1062_t41.ld` is the supported mechanism. **Treat this as the
second lever, not the first** — it is a bigger blast radius than a config header,
and `firmware/test/boot-target/test_ldscript_drift.py` already exists to police
it, which tells you the project has been here before.

**The trade to state:** `.rodata` in flash is read over FlexSPI rather than from
tightly-coupled memory. The M7's 16 KiB instruction cache and the FlexSPI AHB
buffers absorb most of it for the hot ECP inner loops, but it is a real slowdown
on the constant tables and it lands on the handshake cost this design is already
estimating. Measure the handshake with `.rodata` where it will actually ship.

**OCRAM (the libc heap) — where the record buffers go, and this is already
correct by accident:**

`imxrt1062_t41.ld:98` sets `_heap_start = ADDR(.bss.dma) + SIZEOF(.bss.dma)` in
the `RAM` region at `0x20200000`, i.e. **the newlib heap is OCRAM**. mbedTLS
allocates through plain `calloc`: `MBEDTLS_PLATFORM_MEMORY` is *not* defined
(measured — the probe symbol is absent) and `MBEDTLS_MEMORY_BUFFER_ALLOC_C` is
off, so `mbedtls_calloc` is libc `calloc` with no redirection hook available.

| Allocation | Bytes | Lifetime |
|---|---|---|
| `ssl->in_buf` (`MBEDTLS_SSL_IN_BUFFER_LEN`) | 10,557 | per connection |
| `ssl->out_buf` (`MBEDTLS_SSL_OUT_BUFFER_LEN`) | 10,557 | per connection |
| `mbedtls_ssl_handshake_params` | 1,464 | handshake only, then freed |
| `mbedtls_ssl_transform` | 208 | per connection |
| `mbedtls_ssl_session` | 124 | per connection |
| own-cert DER copy + `mbedtls_pk` key material | ~600-800 | once, at cert load |
| **Peak** | **~23,500** | |
| **Steady, post-handshake** | **~21,600** | |

All figures measured from the probe object except the DER copy, which scales
with the cert. Against OCRAM free — 293,504 B at revision `f76baa87c67d`
(`signal-path.md:640-645`) — this is 8 % of the free bank. **OCRAM is the right
home and no code change is needed to put it there.**

**Recommended content-length asymmetry.** `MBEDTLS_SSL_MAX_CONTENT_LEN` is
reduced to `1024 * 10` in the package config, which sets both directions.
That is a **latent compatibility bug for a server**: TLS 1.2 permits a client to
send 16,384-byte records, `MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` negotiation is being
trimmed out, and an oversized record earns a fatal alert. Set the two
independently instead:

```c
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384  /* spec maximum; we do not control the client */
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096  /* we send 512 B per epoch anyway */
```

That is 16,701 + 4,413 = 21,114 B of heap, identical to today's symmetric 10 KiB
pair, with strictly better interoperability. Free win.

### Open defect 14 is directly load-bearing, and the resolution direction matters

`[env:teensy41-optical]` removes the allocator wrap
(`platformio.ini:220-221`), so `malloc`/`calloc` resolve to plain newlib and the
heap is OCRAM. **That is what makes the placement above correct.**

`aius-network-hardening.md` defect 14 proposes restoring the wrap flags so the
DTS decoder uses its 128 KiB DTCM arena. **Restoring them without scoping would
break TLS badly**, and the mechanism is unambiguous
(`lib/aius_dts/src/aius_alloc_shim.c:95-113,140-141`):

```c
void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (g_arena) {
        ...
        void *p = arena_alloc(total);
```

The shim is a **global** switch, not a decoder-scoped one, and `__wrap_free` on
an arena block *is a no-op* (`:140-141`, "arena reclaims en masse via reset").
So with the wrap restored and an arena armed, every TLS connection would take
~21.6 KB out of the DTCM arena and never give it back — the arena would be
exhausted after roughly six connections, in the scarcest bank, with no error
path. **Defect 14 and this plan must be resolved together, and the resolution
must make the shim arena-scoped rather than global.** Recording this here is the
point; it is not obvious from either document alone.

### The bank figure that has to be settled first

Two measurements of DTCM free-for-locals disagree and the design's headroom
depends on which is current:

| Source | Value | Revision |
|---|---|---|
| `firmware-io.md:40,48,51` | 9,280 B | `9f5b8dee8767` |
| `signal-path.md:640-645`, which explicitly retires the above | 25,376 B | `f76baa87c67d` |
| `platformio.ini:78` (TLS-off baseline) | 43,488 B | unattributed, no commit |

Against 25,376 B a ≤6,734 B delta leaves 18,642 B and is comfortable. Against
9,280 B it leaves 2,546 B, and the audio ISR's `enqueue()` stack frame alone is
4,096 B (`signal-path.md:658`) — that is not a budget, it is a fault. **Which
figure is current is a gate item.** Note also that `platformio.ini`'s 43,488
cannot be reconciled with either and predates the `mem_report.py` fix that
stopped hiding the 9,756-byte `NonCacheable` block; it should be re-measured or
struck.

## Certificate and key on the SD card

### Provenance and the CSR flow

The cert is issued by the owner's existing CA, for a stable DNS name. **Generate
the keypair on the host, not the board.** Three reasons: the board's entropy is
`fnet_rand()` and is not fit to seed a long-lived key (§Security posture); a
CSR-signing round trip needs a filesystem and a CA client, both of which the host
has; and keeping it host-side means zero firmware code for a once-per-device
operation.

Propose a single new script, `firmware/tools/aius-cert-provision.sh`, doing:

1. `openssl ecparam -name prime256v1 -genkey` into `aius-key.pem`.
2. A CSR with `subjectAltName = DNS:<name>`, submitted to the CA by whatever
   mechanism the CA offers.
3. The returned leaf, plus the issuing chain, concatenated into `aius-cert.pem`.
4. Both pushed to the card as `/aius/tls/cert.pem` and `/aius/tls/key.pem`.

There is **no `openssl` invocation, no `*.pem`, and no cert tooling anywhere in
the subtree today**, so this is all new.

**Step 4 has a problem worth naming now.** `PUT /sd/...` writes are locked to the
`www/` prefix (`src/aius_optical_app.cpp:184`), which is the same limitation
`aius-ota-upgrade.md` §One thing that must change already records for firmware
images. A key must not live under the web tree, so either a second prefix is
added or the card is provisioned out-of-band with the card in a reader. **For a
private key, out-of-band is the better answer anyway** — it avoids ever shipping
the key over the network the key exists to protect. Recommend: card in a reader,
no new HTTP route, and say so explicitly rather than leaving it to inference.

### The DNS-SAN consequence, and what the firmware owes it

The cert carries a **DNS SAN, not an IP SAN**. The board comes up on DHCP and is
currently at `192.0.2.66`. Consequences:

- **Operator tasks, not firmware tasks:** choosing the name, a DHCP reservation
  or static lease so the name resolves stably, and the DNS record itself. None of
  this belongs in the image.
- **Every client must connect by name.** `https://192.0.2.66/` fails hostname
  verification, correctly, and no tool may skip that check. Every host tool and
  every doc URL becomes the name.
- **The firmware should verify the pairing at boot, and this is worth the code.**
  A name/SAN mismatch is otherwise a *silent* failure: the board serves TLS
  happily and every client rejects it, with the board reporting success. Since
  the board must be told its own name anyway (to report it, and to make the
  mismatch checkable), put the expected name in the existing config surface and
  have the boot-time validator compare it against the parsed SAN, using
  `mbedtls_x509_crt`'s `subject_alt_names` list. A mismatch is a **cert
  validation failure**, so it falls open to HTTP under the policy below, and it
  surfaces in `/api/status` with a distinguishable reason.

### What "invalid" means, concretely

The policy is fail-open, so classification decides behaviour. Six outcomes, and
they are not equivalent:

| Condition | Detected where | Classification |
|---|---|---|
| No card, or mount fails | SD job engine | fall open, `tls_state = no_card` |
| `cert.pem` or `key.pem` absent | SD job engine | fall open, `tls_state = absent` |
| Cert fails to parse | `mbedtls_x509_crt_parse` | fall open, `tls_state = cert_parse_failed` |
| Key fails to parse | `mbedtls_pk_parse_key` | fall open, `tls_state = key_parse_failed` |
| Key does not match cert | `mbedtls_pk_check_pair` | fall open, `tls_state = key_mismatch` |
| Cert SAN does not match configured name | app comparison | fall open, `tls_state = name_mismatch` |
| Cert outside its validity window | app comparison, **not mbedTLS** | see below |

Two of these need care.

**Key/cert mismatch is not detected by the FNET call.**
`fnet_tls_set_own_certificate` parses both and calls
`mbedtls_ssl_conf_own_cert`, which is a bare list append
(`ssl_tls.c:6226-6230` → `ssl_append_key_cert`) and returns success for a
mismatched pair. The failure would first appear as a broken handshake with a
live client, which is precisely the outcome the fail-open policy exists to avoid.
**The application must validate before handing the buffers over**: parse the cert
and key itself, call `mbedtls_pk_check_pair` (available,
`pk.c:308`), then discard the temporaries and call
`fnet_tls_set_own_certificate` with the same buffers. Cost is a transient
duplicate parse — one `mbedtls_x509_crt` (308 B) plus heap for the DER — and it
converts a live-traffic failure into a boot-time classification.

**Expiry cannot be checked by mbedTLS in this build, and there is no clock
anyway.** `MBEDTLS_HAVE_TIME_DATE` is off (measured — the probe symbol is
absent), and the consequence is explicit in the source
(`x509.c:986-999`): `mbedtls_x509_time_is_past()` and
`mbedtls_x509_time_is_future()` both `return 0` unconditionally. The parsed
`valid_from` / `valid_to` fields are still populated from the DER, so the
application *could* compare them — but the firmware does not use the Teensy RTC
at all (no `Teensy3Clock`, `rtc_get` or SNVS reference anywhere in
`firmware/src/`) and there is no NTP.

**Recommendation: do not treat expiry as a validity condition. Report it.**
Serialize `valid_from`, `valid_to` and the SAN into `/api/status` so a human or
a monitoring job notices an impending expiry, and leave the enforcement decision
to whoever renews. A device that bricks its own HTTPS on a date it cannot
reliably know is worse than one that serves a stale cert its clients will
reject. If enforcement is later wanted, wiring the RTC is the prerequisite and
should be its own change.

### Loading within the one-SD-primitive-per-epoch rule

Loading is unremarkable and that is the point. A P-256 leaf plus a two-cert chain
is roughly 2.5 KB of PEM; the key is ~250 B. At `AIUS_SD_MAX_TRANSFER` = 512 B
per epoch that is about **six epochs**, against a rule
(`aius_runtime.c:865-869` equivalent in the plan; enforced by
`validate_request()` at `lib/aius_sd/src/aius_sd.c:23,270`) that permits one
primitive and 512 bytes per epoch.

Sequencing matters more than cost:

- The load must complete **before** the listener opens, because the choice of
  listener is what the cert decides. That is a new ordering constraint on
  checkpoint 4 (`NETWORK`) — it must not `begin()` a listener until the storage
  path has reported a verdict.
- The load must not be able to *delay* the listener indefinitely. Bound it: if
  no verdict is available within the boot deadline the normative contract
  already sets for HTTP (serve within 8 s of boot), treat that as `no_card` and
  open the plain-HTTP listener. **Fail-open must include fail-open-on-slow, not
  just fail-open-on-absent**, or a marginal card turns a documented 8 s deadline
  into an outage.
- Recovery-safe mode runs only checkpoints 1 and 2
  (`lib/aius_runtime/src/aius_runtime.c:81-87`) and therefore has no HTTP at
  all. TLS changes nothing there, and CDC remains the recovery control plane.
- The card is mounted per job today, and a per-job remount is the source of the
  measured 1,016,348 µs stall. Loading two files means two jobs unless they are
  batched. **Batch them**, or accept two remounts on the boot path.

## Key at rest — accepted risk, stated once

**The private key sits unencrypted on a FAT32 SD card, and physical access to
the card is treated as total compromise.** This is a decision, not an oversight.
A derive-from-fused-MAC scheme and an encrypted key file were both considered and
declined: the first is not a secret (the MAC is broadcast in every ARP reply, and
the existing token already demonstrates the failure mode — see
`aius-ota-upgrade.md` §Security), and the second needs a key-encryption key with
nowhere to live on a part whose only secret store is an OTP fuse nobody wants to
burn.

**The card now carries two secrets.** This key, and whatever the maintenance /
authentication token model settles on. Anyone who removes the card gets both, can
impersonate the device on the LAN for as long as the cert is valid, and can
replace the web assets the device serves. Mitigation is physical and
operational: the device lives inside the owner's home LAN, the card is inside the
enclosure, and revocation is "reissue from the CA and reprovision the card".

That is a defensible posture for a personal LAN audio processor. It would not be
defensible for anything reachable from outside the LAN, and **it is not a licence
to expose the device**. Record the same statement in
[`firmware-io.md`](../reference/firmware-io.md) alongside the SD layout, so the next
reader meets it where they will already be looking rather than only here.

## Security posture — what TLS does and does not buy here

Stating this plainly is more useful than the feature.

**What it buys:** the `X-Aius-SD-Auth` token stops crossing the LAN in
cleartext; SD uploads and DSP config writes gain integrity and confidentiality;
and the browser editor stops being trivially modifiable in flight.

**What it does not buy, and each is worth naming:**

- **The UDP control plane on 4242 is untouched and stays unauthenticated.**
  `CMD_SET_MODE`, `CMD_CLEAR_COUNTERS`, `CMD_START_TEST`, `CMD_STOP_TEST` and
  `CMD_SET_MIX` are mutating verbs any host on the subnet can send
  (`firmware/tools/net/aius_net_runner.py:19-26`). Encrypting HTTP while leaving
  that open moves the attack, it does not close it. **Either 4242 gets an
  authentication story in the same programme or the plan should say out loud that
  TLS is confidentiality for the web surface only.**
- **`POST /api/reboot`, `/api/start`, `/api/stop` and `/api/clear` still require
  no credential** (`aius-network-hardening.md:419-424`). TLS authenticates the
  *server* to the client, never the reverse.
- **The auth token remains derivable from the MAC.** FNV-1a-64 through a public
  salt (`aius-ota-upgrade.md` §Security). TLS hides it in transit and does
  nothing about the fact that it need not be observed to be known.
- **The entropy source is weak and mbedTLS is told it is strong.**
  `fnet_rand()` is registered as `MBEDTLS_ENTROPY_SOURCE_STRONG` with a 4-byte
  threshold (`fnet_tls.c:143`) and `MBEDTLS_NO_PLATFORM_ENTROPY` leaves it as the
  only source. Every ECDHE ephemeral key and every ECDSA nonce comes from that
  DRBG. **A weak ECDSA nonce leaks the private key**, so this is not a
  theoretical complaint. The i.MX RT1062 has a TRNG and a hardware UID; a
  `MBEDTLS_ENTROPY_HARDWARE_ALT` implementation seeded from the TRNG is the
  right fix and it is small. **Treat this as a prerequisite, not a follow-up.**

## What "TLS only" breaks

This is the largest slice of the work and it is not "update the tools". Nine
items, enumerated. The good news first: **every browser asset survives
untouched**, because all six files under `firmware/sd/www/` fetch root-relative
URLs only (`dsp-editor.js:468`, `replays.js:7,31` and their call sites) and a
page served over HTTPS inherits the scheme. Both `test/sd-editor/` vitest suites
stub `fetch` and are scheme-agnostic. Zero frontend changes.

### The CA trust path, which every item below depends on

**A private CA's certificate is not a secret.** It is a public key plus a name,
published by every organisation that runs one. So the cheapest correct answer is
to **commit the CA certificate to the repo** — propose
`firmware/tools/net/local-ca.pem` — and have every tool default to it, with an
environment variable override (`AIUS_CA_BUNDLE`) for the case where the CA is
rotated before the repo is updated. That gives verification with no operator
setup, no skip-verify path anywhere, and a single place to rotate.

The alternative, an env var pointing at the operator's own bundle with no
default, is also defensible but makes every fresh checkout fail until someone
reads the docs. **Recommend the committed CA plus override.** Add a test that
asserts the committed PEM parses and is a CA certificate, so a truncated paste
fails in CI rather than at 2 a.m. against a board.

### Item by item

- **`firmware/tools/net/aius_http_runner.py`** — four connection sites.
  `http.client.HTTPConnection` at `:50` and `:74` become `HTTPSConnection` with a
  shared `ssl.SSLContext` loading the CA. Port default flips from 80 to 443 at
  `:205`. **The hard part is `:95` and `:123`**, which are deliberate raw
  `socket.create_connection` calls writing malformed HTTP/1.0 bytes to exercise
  the parser's rejection paths. Under TLS they need `ctx.wrap_socket`, *and the
  tests change meaning*: a handshake failure and a parser rejection both surface
  as a closed socket at `recv(256)`, so the assertions have to distinguish them
  or the coverage silently evaporates. Budget real thought here, not a
  find-and-replace.
- **`firmware/tools/net/aius_recovery_smoke.py`** — the connection at `:320`,
  port defaults at `:141` and `:1030`. **The timing constants are the subtle
  break.** `observe_ready()` polls with a 1.0 s per-request timeout (`:155`)
  against `HTTP_DEADLINE_S = 8.0` (`:87`); once every poll carries a handshake,
  the measured "HTTP ready" latency at `:459` stops measuring listener readiness
  and starts measuring handshake cost. The soak gate does the same once a second
  for 1,800 s (`:703`). These need session reuse, or a deliberate decision that
  the deadline now includes a handshake and a restated normative budget.
- **The B6 recovery smoke gate itself.** B6 is entered by an accepted manual
  Program-button flash and is already blocked on two defects (12 and 17). It is
  the milestone `aius-network-hardening.md` exists to reach. **Making TLS a
  precondition for B6 would be a serious sequencing error** — see
  [§Sequencing](#sequencing-against-b6-and-ota).
- **`firmware/tools/aius-sd-sync.sh`** — `curl` at `:82-87` gains
  `--cacert <bundle>`; callers pass `https://<name>`. Its 4-attempt retry loop
  (`:80-94`) exists because the board serves one client at a time and drops
  overlapping connections; handshakes lengthen the occupancy window, so the
  retry budget probably needs raising. Measure the 1-in-10 accept-drop rate
  (`firmware-io.md:668-673`) again with TLS on before choosing a number.
- **`firmware/tools/replay_samples/upload.sh`** — `curl` at `:31` and `:46-51`
  need `--cacert`, and there is a worse problem: **the loop runs one `curl` per
  4,096-byte chunk** (`:43`), so one full TCP and TLS handshake per 4 KB. At an
  estimated 75-160 ms of board CPU per handshake that is unusable for a
  multi-megabyte replay. This script needs session reuse or a Python rewrite
  against the shared runner. **This is the item most likely to be
  underestimated.**
- **`firmware/test/net/test_aius_http_runner.py`** — the local `HTTPServer` at
  `:42` needs `ssl.SSLContext.wrap_socket` and a throwaway cert, generated at
  test time rather than committed. The port-80 literal at `:85` (which asserts a
  connection failure) needs revisiting.
- **`firmware/src/aius_network_teensy.cpp`** — `kHttpPort = 80u` at `:54` and
  `EthernetServer g_listener(kHttpPort)` at `:179`. Under this design the port
  becomes conditional on the cert verdict: 443 with a valid pair, 80 without.
- **`swagger.json:10`**, outside this subtree — `"url":
  "http://{deviceHost}"` pins the scheme literally. Becomes `https://`, or gains
  a `{scheme}` server variable so the fail-open case is describable. Its contract
  test is `test/swagger.test.js`.
- **The OTA design.** [`aius-ota-upgrade.md`](aius-ota-upgrade.md) is built on
  the chunked `PUT /sd/replays/<id>` protocol with `Content-Range`, one
  self-contained request per chunk — chosen precisely because it survives
  connection loss between chunks. **Per-chunk handshakes would make a 2 MB image
  push cost tens of seconds of pure crypto on top of the ~45-50 s of transfer.**
  Either the OTA client holds one TLS session across many chunk requests (which
  it should; HTTP keep-alive over one connection, and the board's one-client
  model makes that natural) or the chunk size rises substantially. This is a
  design constraint on OTA, and it should be recorded there once this plan is
  agreed.

**Discovery advertises nothing, which is the one lucky break.** The UDP status
payload has no port or scheme field (`aius_net_runner.py:58-119`), and mDNS is
never called — `EthernetMDNS` exists in the vendored library
(`NativeEthernet.h:411-430`) and no firmware source references it. So nothing
needs changing, but equally **nothing tells a client the board is now on 443
under a different name**, and the fail-open case means the port is not even
static. Serializing `tls_state` and the served port into `/api/status` and the
UDP payload is the fix; the payload is length-gated at 71/99 bytes
(`aius_net_runner.py:59`) so growing it is a coordinated firmware plus runner
change with the same compatibility dance already documented.

**Documentation carrying literal `http://` URLs:** `firmware/README.md:189`,
`firmware/tools/replay_samples/README.md:26`, and
`docs/plans/aius-sd-config-web.md:211`. Files carrying a bare `192.0.2.66` are
more numerous and mostly become the DNS name; they are prose edits, not a
design question.

## The build guard changes value, it does not go away

`firmware/test/network-transport/test_vendor_source.py:325-392` exists so this
flag cannot drift, and its docstring states the general principle
(`:336-340`): *"it is the general rule that a configuration macro the fake fixes
must be pinned on the target too, and the only way to keep it is to compare the
two build descriptions."*

**The guard must survive, with its asserted value changed and its coverage
widened.** Concretely:

- `target_values == ["1"]` and `suite_values == ["1"]` instead of `["0"]`, with
  `target_values == suite_values` unchanged. The equality assertion is the whole
  point and is untouched.
- The drift-sensitivity self-tests at `:375-382` stay exactly as they are.
- **The `tls_declarations == []` assertion inverts.** The fake `fnet.h` currently
  declares no `fnet_tls_*` function *because the image has none*. Once the
  adapter calls the TLS API the fake must declare exactly the surface the
  adapter uses — `fnet_tls_socket`, `fnet_tls_socket_recv`,
  `fnet_tls_socket_send`, `fnet_tls_socket_close`, `fnet_tls_init`,
  `fnet_tls_set_own_certificate`, `fnet_tls_release` — and the assertion becomes
  set equality against that list. Same property, opposite polarity: the fake and
  the image agree on which functions exist.
- Three new pins in the same test, because each is a rule the build can reject:
  `MBEDTLS_CONFIG_FILE` resolves to the in-repo header on both sides; the in-repo
  header has `MBEDTLS_SSL_PROTO_TLS1` and `MBEDTLS_SSL_PROTO_TLS1_1` **not**
  defined and offers exactly the intended ciphersuite set;
  `FNET_CFG_TLS_SOCKET_MAX` is pinned to 1.
- **A post-link bank-floor guard.** The env already runs three `post:` hooks
  (`platformio.ini:25-28`), two of them ITCM placement checks. Add an assertion
  that DTCM free-for-locals stays above a floor and that `_itcm_block_count` has
  not incremented. That converts the two scariest numbers in this document from
  things an agent must remember into things the build rejects.

**Deleting the guard would be the single worst outcome of this work.** It was
added because the suite and the image had silently disagreed for the entire life
of the feature.

## Key type — EC P-256, not RSA-2048

**Recommend P-256 with ECDHE-ECDSA and AES-128-GCM-SHA256.** Five reasons, in
order of weight:

1. **Handshake cost.** ECDHE-ECDSA needs three P-256 scalar multiplications
   (ephemeral keygen, ECDSA sign, ECDH shared secret). ECDHE-RSA needs the
   ephemeral keygen *plus* an RSA-2048 private operation, and
   `MBEDTLS_MPI_WINDOW_SIZE 1` (`config.h:50`) is the slowest available
   modular-exponentiation setting. Static RSA avoids the ephemeral but abandons
   forward secrecy and still pays the RSA private op. EC is estimated at roughly
   half the CPU, and CPU is the resource this design is spending.
2. **SHA-384 does not exist in this build.** `MBEDTLS_SHA512_C` is disabled
   (measured), with a package comment reporting a suspected 48-vs-64-byte buffer
   bug in the SHA-384 wrappers. So every `..._SHA384` ciphersuite is off the
   table regardless of key type, and the cert must be SHA-256-signed. That is
   normal for P-256 and slightly constraining for RSA.
3. **Size on the card and on the wire.** A P-256 key is ~250 B of PEM against
   ~1.7 KB for RSA-2048, and the leaf is ~450-550 B of DER against ~1.2 KB.
   Fewer SD epochs, smaller `Certificate` message, smaller `out_buf` pressure.
4. **The trimmed config drops RSA entirely** (validated: all 79 files compile
   with `MBEDTLS_RSA_C` off), which is where most of the reclaimed flash and a
   share of the reclaimed DTCM comes from. Choosing RSA gives that back.
5. **The curve set is already narrowed to it.** The package config leaves only
   P-256 and P-384 enabled among the NIST curves; the trim takes it to P-256
   alone.

**The one argument for RSA is client compatibility with genuinely ancient
software**, and it does not apply: the clients are `curl`, Python's `ssl`, and a
current browser, all of which have preferred ECDSA for a decade.

## Staged plan

**Nothing here is built until the gate passes.** The first two stages are
measurement and host-side work with no firmware risk; the gate sits between
them and any change to the image.

### Stage 0 — settle the numbers (no code)

- Re-measure `scripts/build.sh teensy41-optical` from a clean committed tree and
  publish DTCM free-for-locals, ITCM free, ITCM block count and OCRAM free.
  **Resolve the 9,280 / 25,376 / 43,488 contradiction** and strike whichever
  figures are stale, including `platformio.ini:78`.
- Build one throwaway instrumented image with the trimmed config, TLS on,
  `FNET_CFG_TLS=1` and `FNET_CFG_TLS_SOCKET_MAX=1`, and record the *actual* per-bank
  deltas against the predictions in this document. Predictions above are
  measured per-object sums before `--gc-sections`; the linked image is the
  authority.
- **Measure one handshake on the device.** ECDHE-ECDSA P-256, DWT cycle counters
  around the two bursts, with `.rodata` wherever it will ship. This is the number
  the whole design rests on.
- Measure `fnet_poll`'s duration while a handshake is in flight. It runs at
  NVIC 128, equal to the audio DMA ISR, and its cost has never been measured
  (`firmware-io.md:910-912`). It is the one path where a long compute would be
  fatal rather than merely slow, and it is the only unquantified one left.

### Go/no-go gate

Proceed only if **all** hold:

1. Measured DTCM free-for-locals, with TLS on and the trimmed config, leaves at
   least **12 KB** headroom. The audio ISR's `enqueue()` frame is 4,096 B and the
   main-loop stack shares the same zone; 12 KB is two clear margins, not one.
2. `_itcm_block_count` has **not** incremented. If it has, the linker-script
   `.text` relocation is mandatory rather than optional and this stage repeats.
3. Measured handshake CPU is **under 400 ms**. That leaves a 20× margin on the
   watchdog and consumes under half the replay ring's 341 ms of slack in a single
   burst. Above 400 ms, relax `MBEDTLS_ECP_WINDOW_SIZE` and re-measure before
   reconsidering.
4. `fnet_poll` duration during a handshake stays under **500 µs**, so a 2.67 ms
   audio block callback keeps most of its period.
5. The entropy question has an owner. Either a TRNG-backed
   `MBEDTLS_ENTROPY_HARDWARE_ALT` is in scope for stage 2, or there is a written
   decision to ship ECDSA nonces from `fnet_rand()`. **Not an open question at
   build time.**

A failure on 1 or 2 is a memory-placement problem with known levers. A failure on
3 or 4 is closer to disqualifying for the current architecture, and the answer
would be to move TLS termination off the board — a reverse proxy on the LAN
terminating HTTPS in front of a plain-HTTP Aius — which is a legitimate outcome
worth stating before anyone starts.

### Stage 1 — host side, first delivery boundary

Deliverable: **the board is untouched, and a host can already verify a cert
against the CA.** Committed CA PEM plus its parse test; the provisioning script
generating a P-256 key, a DNS-SAN CSR, and assembling the chain; the two Python
runners carrying an `--ca-bundle` / `AIUS_CA_BUNDLE` path and an explicit
`--tls` / `--no-tls` selection that still defaults to plain HTTP. Everything
passes against the current plain-HTTP board.

This is the right first boundary because it is the largest slice, it carries no
firmware risk, and it makes the rest testable. It also lands the cert on the card
before any image needs it.

### Stage 2 — the image

The repo-owned `aius_mbedtls_config.h`, its derivation script and its assertions;
`FNET_CFG_TLS=1` and `FNET_CFG_TLS_SOCKET_MAX=1`; the guard changes described
above; the boot-time load, validate and classify path; the adapter's TLS socket
calls with correct `FNET_ERR` handling and retained partial writes; the
pre-transaction handshake phase with its own deadline; the conditional listener;
`tls_state` plus cert validity dates and SAN in `/api/status`; and the
linker-script relocation if the gate demanded it. The TRNG entropy source lands
here if it is in scope.

### Stage 3 — the rest of the tooling

`aius-sd-sync.sh`, the replay uploader's session reuse or rewrite, the local
test fixture's TLS server, `swagger.json`, the doc URLs, and the UDP payload
extension that publishes `tls_state` and the served port.

## Sequencing against B6 and OTA

**Do not put TLS in front of B6.** B6 is the live recovery smoke that has never
run; it is blocked on defects 12 and 17; and it is the milestone
`aius-network-hardening.md` exists to reach. Making it depend on a new TLS
listener and rewritten host tooling would push the project's oldest unproven
claim further out for a feature that is not on its critical path. **Run B6 over
plain HTTP, then add TLS**, and let stage 1 above proceed in parallel because it
touches nothing B6 needs.

For OTA the ordering is the reverse: **OTA should not ship over plain HTTP.** A
remote firmware-replacement endpoint whose transport is unencrypted and whose
credential is derivable from a MAC address is the surface
`aius-ota-upgrade.md` §Security already flags. TLS is a reasonable precondition
for OTA even though it is not one for B6, and OTA's own prerequisite list
(fix `POST /api/reboot`, serialize upload progress, run B6, read the OTP fuse)
is unaffected by anything here.

## Open questions for the owner

1. **The DNS name and the lease.** `aius.example.net` is an assumption. The
   name, the DHCP reservation or static lease, and the DNS record are operator
   steps; the firmware only needs to be told the name so it can check the SAN.
2. **Does the UDP control plane get an authentication story in the same
   programme?** If not, this plan should state that TLS is confidentiality for
   the web surface only, and that mutating control remains open on 4242.
3. **Entropy.** Is a TRNG-backed entropy source in scope for stage 2, or is
   shipping ECDSA nonces from `fnet_rand()` an accepted risk like the key at
   rest? This one has a sharper failure mode than the key on the card, because it
   leaks the key without anyone touching the device.
4. **Is a LAN reverse proxy an acceptable fallback** if the gate fails on
   handshake cost or `fnet_poll`? Knowing the answer in advance decides whether a
   gate failure ends the work or redirects it.

## Related documents

- [Aius network and firmware recovery hardening](aius-network-hardening.md) —
  §Deciding principle, the open-defect list (12, 14, 17), and B6.
- [Aius OTA firmware upgrade](aius-ota-upgrade.md) — the chunked transport this
  plan constrains, and the security assessment of the auth token.
- [Aius firmware I/O reference](../reference/firmware-io.md) — the bank table,
  the SD job engine, the HTTP transaction, and the home for the key-at-rest
  statement.
- [Aius signal path](../reference/signal-path.md) — the current bank
  measurements and the audio-path timing this plan depends on.
- [Aius DSP configuration API](aius-dsp-config-api.md) — §Target memory strategy,
  which open defect 14 contradicts.
