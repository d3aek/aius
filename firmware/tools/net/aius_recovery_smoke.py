#!/usr/bin/env python3
"""Aius proportionate live recovery smoke -- host runner (plan Phase 16a).

Reads USB `ID`, HTTP status and UDP status from one deployed optical board,
decodes the recovery status fields and reports pass/fail per gate with a
readable per-run summary. The four gates are steps 2-5 of the live smoke in
`docs/plans/aius-network-hardening.md` §Phase 16:

  reboots  about five software reboots, each reaching DHCP, USB, HTTP and UDP
           ready inside the normative deadlines, with no unexpected watchdog
           reset
  stalls   one induced stall at an injectable checkpoint 0..8: reset inside
           8-10 s, the checkpoint reported, service back without intervention
  soak     a short soak of HTTP/UDP traffic with no unexpected reset, outage or
           leaked network generation
  dsp      the DSP strand's target validation: default audio unchanged, a
           GET/PUT/SD/reset config round-trip, meters rendering, and the SD
           control-surface editor loading

Every gate reports one of three outcomes. `pass` and `fail` are observations;
`unresolved` means the runner could not observe the evidence at all (device
unreachable before anything was proven, a mutating step not opted into, or a
missing input such as --checkpoint) and is never silently counted as a pass.

    tools/net/aius_recovery_smoke.py probe --target 192.0.2.66
    tools/net/aius_recovery_smoke.py plan
    tools/net/aius_recovery_smoke.py run --target 192.0.2.66 --gates soak
    tools/net/aius_recovery_smoke.py run --target 192.0.2.66 --gates reboots \\
        --serial /dev/ttyACM0 --allow-mutation

**The default invocation is read-only.** Every step that mutates the board --
the reboot cycles, the fault-image stall observation, and the DSP config
round-trip's PUT -- runs only with --allow-mutation, and the gates run in one
serialized order because there is one device and one fixture. Flashing itself
belongs to `scripts/run_recovery_smoke.sh`; `plan` prints the ordered plan of
work it drives, including the production restore between two fault images.

Env vars:
  AIUS_SD_AUTH   SD maintenance token for the DSP config PUT (default: unset;
                 --auth-token overrides, and `AIUS AUTH` over --serial is the
                 fallback)

Exit codes: 0 every selected gate passed; 2 a gate failed; 3 a gate was
            unresolved or the environment blocked the run; 4 usage error.
"""

import argparse
import dataclasses
import datetime
import http.client
import importlib.util
import json
import os
import pathlib
import select
import sys
import time

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
FIRMWARE_ROOT = TOOLS_DIR.parents[1]


def _load_sibling(name):
    """Load a sibling runner by path.

    The two existing runners are plain scripts in this directory, not an
    installed package, and this module is itself loaded by path from the test
    suite -- so neither `import` nor a sys.path insert is reliable here.
    """
    spec = importlib.util.spec_from_file_location(name, TOOLS_DIR / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The UDP status decode, the frame codec and the HTTP request/response helpers
# are the existing runners' job; this module adds gates on top of them and
# never re-implements either wire format.
udp_runner = _load_sibling("aius_net_runner")
http_runner = _load_sibling("aius_http_runner")

# Normative deadlines, §Runtime order and budgets. UDP has no deadline of its
# own: it shares one network generation with HTTP, so it is held to the same
# bound as HTTP and a later UDP than HTTP is a real finding, not a tolerance.
#
# The 12 s network figure is measured, not chosen, and it is a bound on a
# bimodal distribution rather than on a spread. Over twenty-nine software reboots
# of 3576c3e3560f the board's own pre-DHCP work took 2,165-3,624 ms, and the DHCP
# window that follows it is quantised by FNET_CFG_DHCP_CLN_RESPONSE_TIMEOUT_MS:
# 4,003-4,005 ms when the first DISCOVER is answered (27 boots) and 8,005 ms when
# it is not and the retransmission is (2 boots). The segment owns that 4 s step
# and the board cannot shorten it -- see src/aius_network_teensy.h, where asking
# more often was tried on the board and did not move it. Host observation adds up
# to one poll interval plus one probe timeout. So the observed channel latency is
# ~7.1-8.5 s with one DISCOVER and ~10.8-11.0 s with two, and 12 s is the worst
# of those plus ~1 s.
#
# What the margin is NOT for: a third DISCOVER. That boot would land near 14.8 s
# and fail this deadline, which is intended -- at two lost DISCOVERs the board is
# also within 1 s of AIUS_NET_DHCP_DEADLINE_MS (9,000 ms) and about to take the
# teardown-and-rebind path itself. A gate that absorbed it would hide the event
# the deadline exists to surface.
#
# The previous 7 s / 8 s pair was set before any of this was measured and sat
# below even the fast mode's floor, so it could not be met on a healthy board.
USB_ID_DEADLINE_S = 2.0
DHCP_DEADLINE_S = 12.0
HTTP_DEADLINE_S = 12.0
UDP_DEADLINE_S = 12.0
STALL_RESET_MIN_S = 8.0
STALL_RESET_MAX_S = 10.0
# How often observe_ready() samples the CDC port while waiting. A presence
# sample is one path stat, so the cost is negligible next to the transport
# reads it sits between; the value only has to be well under the ~1.5 s the
# host takes to re-enumerate the port after a reset.
USB_SAMPLE_INTERVAL_S = 0.05
# AiusSdState's terminal success value (lib/aius_sd/include/aius_sd.h), as the
# status serves it. Named here so the SD-job commit basis reads as a state and
# not as a magic number.
SD_JOB_DONE = 15
INJECTABLE_CHECKPOINTS = tuple(range(0, 9))
CHECKPOINT_NAMES = ("RESET_CAPTURE", "USB_RX", "USB_TX", "METRICS", "NETWORK",
                    "HTTP", "UDP", "REPLAY", "SD", "EPOCH_COMPLETE")

# SRC_SRSR bits, mirroring aius_boot_teensy_reset_kind() in
# src/aius_boot_teensy.cpp. The status carries the raw retained value, so the
# host has to classify it the same way the firmware does.
SRSR_IPP_RESET_B = 0x00000001
SRSR_LOCKUP_SYSRESETREQ = 0x00000002
SRSR_CSU_RESET_B = 0x00000004
SRSR_WDOG_RST_B = 0x00000010
SRSR_JTAG_SW_RST = 0x00000040

PASS = "pass"
FAIL = "fail"
UNRESOLVED = "unresolved"

GATE_NAMES = ("reboots", "stalls", "soak", "dsp")

EDITOR_RESOURCES = ("www/index.html", "www/dsp-editor.js", "www/dsp-editor.css")

# The ten recovery fields, in the one spelling HTTP JSON, USB `ID` and the UDP
# suffix all resolve to (§Status compatibility contract).
RECOVERY_FIELDS = ("network_state", "recovery_safe", "network_last_fault",
                   "last_checkpoint", "network_generation", "boot_generation",
                   "reset_source", "watchdog_early_resets", "loop_epoch",
                   "network_restarts")

# USB `ID` abbreviates each field; optical_main.cpp:1923 is the same table.
USB_FIELD_NAMES = {
    "rsafe": "recovery_safe",
    "nstate": "network_state",
    "nfault": "network_last_fault",
    "ckpt": "last_checkpoint",
    "early": "watchdog_early_resets",
    "ngen": "network_generation",
    "bgen": "boot_generation",
    "rsrc": "reset_source",
    "epoch": "loop_epoch",
    "nrst": "network_restarts",
}


class LinkError(RuntimeError):
    """One device read did not produce an answer. Never a gate verdict itself."""


@dataclasses.dataclass
class SmokeOptions:
    target: str = ""
    http_port: int = 80
    udp_port: int = udp_runner.PORT
    serial: str = None
    auth_token: str = None
    gates: tuple = GATE_NAMES
    allow_mutation: bool = False
    reboots: int = 5
    checkpoint: int = None
    soak_duration: float = 1800.0
    soak_interval: float = 1.0
    soak_tolerated_outages: int = 0
    poll_interval: float = 0.25
    # Transport timeout used only by observe_ready()'s polls. Short because
    # every failed poll's timeout is quantisation on the latencies the reboot
    # gate reports, and the deadlines it reports them against start at 2 s.
    ready_probe_timeout: float = 0.3
    # Cap on the CDC re-enumeration watch at the start of observe_ready().
    # Above the ~1.5 s the host takes and well below the earliest possible
    # DHCP/HTTP/UDP answer, so the watch never hides a transport becoming ready.
    usb_watch_limit: float = 4.0
    ready_limit: float = 20.0
    stall_horizon: float = 45.0
    reset_margin: float = 2.0
    timeout: float = 1.0
    evidence_dir: str = None


@dataclasses.dataclass
class GateResult:
    name: str
    outcome: str
    detail: str
    observations: list = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class SmokeRun:
    target: str
    started_utc: str
    identity: dict
    results: list = dataclasses.field(default_factory=list)


# --------------------------------------------------------------------------
# decode helpers -- pure, and the only place a wire spelling becomes a field
# --------------------------------------------------------------------------


def reset_kind(raw_reset_source):
    """Classify a raw SRC_SRSR value exactly as the firmware adapter does."""
    if raw_reset_source is None:
        return "unknown"
    if raw_reset_source & SRSR_WDOG_RST_B:
        return "watchdog"
    if raw_reset_source & (SRSR_IPP_RESET_B | SRSR_CSU_RESET_B):
        return "cold"
    if raw_reset_source & (SRSR_JTAG_SW_RST | SRSR_LOCKUP_SYSRESETREQ):
        return "software"
    return "other"


def select_usb_response(text, request_id):
    """Pick the response line for one request out of whatever the CDC returned.

    A board that is also printing diagnostics interleaves lines, and a partial
    line may precede the answer, so match on the framing rather than assuming
    the first line is the response.
    """
    prefix = "AIUS/1 "
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(prefix):
            continue
        parts = line.split(" ")
        if len(parts) >= 3 and parts[2] == str(request_id):
            return line
    return None


def parse_usb_id(line):
    """Decode `AIUS/1 OK <id> ID key=value ...` into the shared field names."""
    parts = line.strip().split(" ")
    if len(parts) < 5 or parts[0] != "AIUS/1":
        raise ValueError("not an Aius control response")
    if parts[1] == "ERR":
        raise ValueError("device answered " + " ".join(parts[1:]))
    if parts[1] != "OK" or parts[3] != "ID":
        raise ValueError("not an ID response")
    fields = {"request_id": int(parts[2])}
    for token in parts[4:]:
        key, separator, value = token.partition("=")
        if not separator or not key:
            raise ValueError("malformed ID field: " + token)
        fields[USB_FIELD_NAMES.get(key, key)] = _number_or_text(value)
    return normalize_recovery(fields)


def _number_or_text(value):
    try:
        return int(value)
    except ValueError:
        return value


def normalize_recovery(raw):
    """One mapping shape for all three channels.

    HTTP serves `recovery_safe` as 0/1 like its other booleans and USB `ID`
    serves it as rsafe=0/1, while the UDP decode already returns a bool. The
    gates compare across channels, so the flag is normalized once here.
    """
    result = dict(raw)
    if "recovery_safe" in result:
        result["recovery_safe"] = bool(result["recovery_safe"])
    return result


def dhcp_bound(status):
    """True when the status reports a usable DHCP address on this generation."""
    address = status.get("ip")
    if not address or address == "0.0.0.0":
        return False
    return not status.get("dhcp_pending", 0)


def deadline_failures(latencies):
    """Name every ready channel that missed its normative deadline."""
    deadlines = {"dhcp": DHCP_DEADLINE_S, "http": HTTP_DEADLINE_S,
                 "udp": UDP_DEADLINE_S, "usb": USB_ID_DEADLINE_S}
    failures = []
    for channel, latency in sorted(latencies.items()):
        limit = deadlines[channel]
        if latency is None:
            failures.append("%s never became ready (deadline %.1fs)" % (channel, limit))
        elif latency > limit:
            failures.append("%s ready at %.2fs, deadline %.1fs" % (channel, latency, limit))
    return failures


# --------------------------------------------------------------------------
# device link -- the one seam the fake-fixture tests replace
# --------------------------------------------------------------------------


class DeviceLink:
    """Bounded reads of one deployed board over HTTP, UDP and USB CDC."""

    def __init__(self, options):
        self.options = options
        self.http_target = (options.target, options.http_port)
        self.udp_target = (options.target, options.udp_port)
        self.serial = options.serial
        self.timeout = options.timeout
        self._request_id = 0

    # clock -- injected so a gate's waiting is drivable in a host test
    def now(self):
        return time.monotonic()

    def sleep(self, seconds):
        time.sleep(max(0.0, seconds))

    def http_json(self, method, path, body=None, auth=False, timeout=None):
        if auth:
            return self._http_authenticated(method, path, body)
        try:
            return http_runner.request(self.http_target, method, path, body,
                                       timeout=self.timeout if timeout is None
                                       else timeout)
        except (OSError, ValueError, http.client.HTTPException) as error:
            raise LinkError("%s %s: %s" % (method, path, error)) from error

    def _http_authenticated(self, method, path, body):
        """The one call the shared HTTP runner cannot make.

        `PUT /api/dsp/config` is refused 401 without X-Aius-SD-Auth
        (aius_optical_app.cpp:780), and aius_http_runner.request() sends a
        fixed header set. Everything else on this link goes through it.
        """
        token = self.options.auth_token or os.environ.get("AIUS_SD_AUTH")
        if not token and self.serial:
            # The board derives the token from its own fused MAC words and
            # serves it on `AIUS AUTH`, so a session with the CDC port open
            # never needs it passed in.
            token = self.usb_auth()
        if not token:
            raise LinkError("no SD maintenance token for %s %s" % (method, path))
        encoded = None if body is None else json.dumps(body, separators=(",", ":"))
        connection = http.client.HTTPConnection(*self.http_target, timeout=self.timeout)
        headers = {"Connection": "close", "X-Aius-SD-Auth": token}
        if encoded is not None:
            headers["Content-Type"] = "application/json"
        try:
            connection.request(method, path, encoded, headers)
            response = connection.getresponse()
            raw = response.read()
            return {"status": response.status,
                    "body": json.loads(raw.decode("utf-8")) if raw else None}
        except (OSError, ValueError, http.client.HTTPException) as error:
            raise LinkError("%s %s: %s" % (method, path, error)) from error
        finally:
            connection.close()

    def http_bytes(self, path):
        try:
            return http_runner.request_bytes(self.http_target, "GET", path,
                                             timeout=self.timeout)
        except (OSError, ValueError, http.client.HTTPException) as error:
            raise LinkError("GET %s: %s" % (path, error)) from error

    def http_status(self, timeout=None):
        result = self.http_json("GET", "/api/status", timeout=timeout)
        if result["status"] != 200 or not isinstance(result["body"], dict):
            raise LinkError("GET /api/status answered %s" % result["status"])
        return normalize_recovery(result["body"])

    def udp_status(self, retries=3, timeout=None):
        # `retries` is a parameter because the default three attempts make one
        # call cost three timeouts, and a caller that is also watching the CDC
        # port cannot afford a single unobserved 3 s window -- see
        # observe_ready(). A plain status read still gets the retries.
        try:
            response, _address = udp_runner.request(
                self.udp_target, udp_runner.CMD_GET_STATUS,
                timeout=self.timeout if timeout is None else timeout,
                retries=retries)
            return normalize_recovery(udp_runner.parse_status(response["payload"]))
        except (OSError, TimeoutError, ValueError) as error:
            raise LinkError("UDP status: %s" % error) from error

    def usb_present(self):
        return bool(self.serial) and os.path.exists(self.serial)

    def usb_id(self):
        return self._usb_command("ID")

    def usb_auth(self):
        response = self._usb_command("AUTH", parser=lambda line: line.split(" ")[4])
        return response

    def _usb_command(self, verb, parser=None):
        if not self.serial:
            raise LinkError("no --serial port configured")
        self._request_id += 1
        request_id = self._request_id
        line = self._exchange("AIUS %s %d\n" % (verb, request_id), request_id)
        try:
            return parser(line) if parser else parse_usb_id(line)
        except (IndexError, ValueError) as error:
            raise LinkError("USB %s: %s" % (verb, error)) from error

    def _exchange(self, command, request_id):
        """Write one control line and read until its framed answer arrives."""
        try:
            descriptor = os.open(self.serial, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as error:
            raise LinkError("open %s: %s" % (self.serial, error)) from error
        try:
            os.write(descriptor, command.encode("ascii"))
            deadline = time.monotonic() + self.timeout + USB_ID_DEADLINE_S
            buffer = ""
            while time.monotonic() < deadline:
                ready, _, _ = select.select([descriptor], [], [],
                                            max(0.0, deadline - time.monotonic()))
                if not ready:
                    continue
                buffer += os.read(descriptor, 512).decode("ascii", errors="replace")
                line = select_usb_response(buffer, request_id)
                if line:
                    return line
        except OSError as error:
            raise LinkError("USB exchange on %s: %s" % (self.serial, error)) from error
        finally:
            os.close(descriptor)
        raise LinkError("no USB answer for request %d" % request_id)

    def reboot(self):
        # Authenticated since the four control routes were gated: an absent
        # X-Aius-SD-Auth is 401, and the resulting LinkError would surface as the
        # same "the first reboot could not be requested" this gate already
        # reported for the unacknowledged-reset defect -- an identical message for
        # an unrelated reason, which is exactly the trap worth not walking into.
        result = self.http_json("POST", "/api/reboot", auth=True)
        if result["status"] != 200:
            raise LinkError("POST /api/reboot answered %s" % result["status"])
        return result


# --------------------------------------------------------------------------
# shared observation helpers
# --------------------------------------------------------------------------


def read_any_status(link, want_usb=False):
    """One recovery snapshot from whichever channel answers, USB first.

    USB is tried first when a port is configured because it is the only
    channel recovery-safe mode serves.
    """
    errors = []
    readers = []
    if want_usb:
        readers.append(("usb", link.usb_id))
    readers.extend((("http", link.http_status), ("udp", link.udp_status)))
    for channel, reader in readers:
        try:
            status = reader()
        except LinkError as error:
            errors.append("%s: %s" % (channel, error))
            continue
        return channel, status
    raise LinkError("; ".join(errors) or "no channel configured")


def observe_ready(link, options, started, baseline_generation, want_usb):
    """Time each ready channel of the boot that follows `baseline_generation`.

    A channel counts only once its status reports a boot generation past the
    baseline, so the outgoing boot answering one last request cannot be
    mistaken for the new one becoming ready.

    The USB budget is the normative one -- "after USB enumeration, `AIUS ID`
    must respond within 2 s" (§Runtime order and budgets) -- so the port has to
    be *observed absent* for the board to be charged from re-enumeration rather
    than from the reboot acknowledgement. Sampling it once per loop iteration
    could not do that: an iteration spent up to 1 s in http_status() and up to
    3 s in udp_status() (1 s x 3 retries) while the board was down, so the
    ~1.5 s enumeration gap fell entirely between two samples and every cycle
    reported `usb_basis=ack`, charging the board for host CDC re-enumeration it
    did not cause.

    The fix has three parts.

    The port is watched, and nothing else, until it has dropped and come back --
    bounded by `options.usb_watch_limit`. Nothing is lost by not probing the
    transports meanwhile: the board's CDC re-enumerates around 1.5 s after the
    reset and it cannot serve DHCP, HTTP or UDP for several seconds after that,
    so the watch ends long before the first transport answer. This is what makes
    re-enumeration observable to USB_SAMPLE_INTERVAL_S instead of to whatever a
    blocking read left over.

    The basis is then *derived at the end* rather than latched on the first
    present sample. Latching would be wrong in the other direction now that the
    first sample lands at t=0: the port is still enumerated for the few hundred
    milliseconds between the reboot acknowledgement and the reset, so the first
    sample sees it present and would conclude "it never went away" before it had
    a chance to. Only a present-after-absent observation establishes
    re-enumeration, and nothing else can.

    Finally the transport polls use `options.ready_probe_timeout` rather than
    `options.timeout`, and one UDP attempt rather than three. These reads all
    fail while the board is down, so their timeouts are pure quantisation on
    every latency this function reports: at the 1 s link timeout an iteration
    cost 4 s and the four channels came back rounded to the same iteration
    boundary. The board is being timed against a 2 s deadline; the instrument
    cannot have 4 s of granularity.

    A note on `--serial`: pass the stable
    /dev/serial/by-id/usb-Teensyduino_USB_Serial_*-if00 path, not /dev/ttyACMn.
    Linux hands the re-enumerated port the next free index, so a board that
    comes back as ttyACM1 leaves a --serial of ttyACM0 permanently absent -- the
    ID never answers, and `usb never became ready` is reported against the board
    for a host naming artifact.
    """
    latencies = {"dhcp": None, "http": None, "udp": None}
    if want_usb:
        latencies["usb"] = None
    usb = {"absent_seen": False, "reenumerated_at": None, "answered_at": None}
    probe_timeout = options.ready_probe_timeout

    def sample_usb():
        """One presence observation. A path stat, so it is called often."""
        if not want_usb or usb["reenumerated_at"] is not None:
            return
        if not link.usb_present():
            usb["absent_seen"] = True
        elif usb["absent_seen"]:
            usb["reenumerated_at"] = link.now()

    def wait(duration):
        """Sleep, sampling the CDC port at USB_SAMPLE_INTERVAL_S while waiting."""
        if not want_usb or usb["answered_at"] is not None:
            link.sleep(duration)
            return
        remaining = duration
        while remaining > 0.0:
            slice_s = min(USB_SAMPLE_INTERVAL_S, remaining)
            link.sleep(slice_s)
            remaining -= slice_s
            sample_usb()

    # The re-enumeration watch. Nothing else runs until the port has dropped
    # and returned, or the cap expires.
    if want_usb:
        watch_limit = started + options.usb_watch_limit
        while link.now() < watch_limit and usb["reenumerated_at"] is None:
            sample_usb()
            if usb["reenumerated_at"] is None:
                link.sleep(USB_SAMPLE_INTERVAL_S)

    limit = started + options.ready_limit
    while link.now() < limit:
        sample_usb()
        # The USB attempt leads the iteration. _exchange() blocks until the
        # framed answer arrives or its own bound expires, so an attempt already
        # in flight returns the instant the board answers -- it is the finest
        # instrument here, and behind the transport reads it was quantised to
        # their timeouts.
        if want_usb and usb["answered_at"] is None and link.usb_present():
            try:
                status = link.usb_id()
                if _is_new_boot(status, baseline_generation):
                    usb["answered_at"] = link.now()
            except LinkError:
                pass
            sample_usb()
        if latencies["http"] is None or latencies["dhcp"] is None:
            try:
                status = link.http_status(timeout=probe_timeout)
                if _is_new_boot(status, baseline_generation):
                    if latencies["http"] is None:
                        latencies["http"] = link.now() - started
                    if latencies["dhcp"] is None and dhcp_bound(status):
                        latencies["dhcp"] = link.now() - started
            except LinkError:
                pass
        sample_usb()
        if latencies["udp"] is None or latencies["dhcp"] is None:
            try:
                status = link.udp_status(retries=1, timeout=probe_timeout)
                if _is_new_boot(status, baseline_generation):
                    if latencies["udp"] is None:
                        latencies["udp"] = link.now() - started
                    if latencies["dhcp"] is None and dhcp_bound(status):
                        latencies["dhcp"] = link.now() - started
            except LinkError:
                pass
        sample_usb()
        transports_ready = all(latencies[channel] is not None
                               for channel in ("dhcp", "http", "udp"))
        if transports_ready and (not want_usb or usb["answered_at"] is not None):
            break
        wait(options.poll_interval)

    # The origin, and therefore the basis, is only decidable once the whole
    # observation is over -- see the docstring.
    usb_basis = "enumeration" if usb["reenumerated_at"] is not None else "ack"
    if want_usb:
        origin = usb["reenumerated_at"] if usb_basis == "enumeration" else started
        latencies["usb"] = (None if usb["answered_at"] is None
                            else usb["answered_at"] - origin)
    return latencies, usb_basis


def _is_new_boot(status, baseline_generation):
    if baseline_generation is None:
        return True
    generation = status.get("boot_generation")
    return generation is not None and generation > baseline_generation


def format_latencies(latencies):
    return " ".join(
        "%s=%s" % (channel, "none" if value is None else "%.2fs" % value)
        for channel, value in sorted(latencies.items()))


# --------------------------------------------------------------------------
# gates
# --------------------------------------------------------------------------


def gate_reboots(link, options):
    """Step 2 -- about five software reboots inside the normative deadlines."""
    name = "reboots"
    if not options.allow_mutation:
        return GateResult(name, UNRESOLVED,
                          "read-only run: %d reboots not attempted" % options.reboots,
                          ["pass --allow-mutation to drive the reboot cycles"])
    want_usb = bool(options.serial)
    try:
        channel, baseline = read_any_status(link, want_usb)
    except LinkError as error:
        return GateResult(name, UNRESOLVED,
                          "device unreachable before the first reboot",
                          [str(error)])
    observations = ["baseline via %s: boot_generation=%s early=%s" %
                    (channel, baseline.get("boot_generation"),
                     baseline.get("watchdog_early_resets"))]
    generation = baseline.get("boot_generation")
    failures = []
    completed = 0
    for cycle in range(1, options.reboots + 1):
        try:
            link.reboot()
        except LinkError as error:
            if cycle == 1:
                return GateResult(name, UNRESOLVED,
                                  "the first reboot could not be requested",
                                  observations + [str(error)])
            failures.append("cycle %d: reboot not accepted (%s)" % (cycle, error))
            break
        started = link.now()
        latencies, usb_basis = observe_ready(link, options, started, generation,
                                             want_usb)
        cycle_failures = deadline_failures(latencies)
        detail = "cycle %d %s" % (cycle, format_latencies(latencies))
        if want_usb:
            detail += " (usb budget from %s)" % usb_basis
        try:
            _channel, status = read_any_status(link, want_usb)
        except LinkError as error:
            cycle_failures.append("no status after the reboot (%s)" % error)
            status = {}
        else:
            kind = reset_kind(status.get("reset_source"))
            early = status.get("watchdog_early_resets")
            detail += " bgen=%s reset=%s early=%s" % (
                status.get("boot_generation"), kind, early)
            if kind == "watchdog":
                cycle_failures.append("cycle %d: unexpected watchdog reset" % cycle)
            if early:
                cycle_failures.append(
                    "cycle %d: watchdog_early_resets=%s" % (cycle, early))
            if status.get("recovery_safe"):
                cycle_failures.append("cycle %d: came back in recovery-safe mode" % cycle)
            if generation is not None and status.get("boot_generation") is not None:
                if status["boot_generation"] != generation + 1:
                    cycle_failures.append(
                        "cycle %d: boot_generation %s -> %s, expected +1" %
                        (cycle, generation, status["boot_generation"]))
            generation = status.get("boot_generation", generation)
        observations.append(detail)
        failures.extend(cycle_failures)
        if cycle_failures:
            break
        completed += 1
    if failures:
        return GateResult(name, FAIL,
                          "%d/%d cycles completed; %s" % (completed, options.reboots,
                                                          failures[0]),
                          observations + failures)
    return GateResult(name, PASS,
                      "%d/%d reboots ready inside the deadlines, no watchdog reset"
                      % (completed, options.reboots), observations)


def gate_stalls(link, options):
    """Step 3 -- one induced stall at one injectable checkpoint.

    The fault image stalls the first time the epoch reaches its selected
    checkpoint (src/aius_fault_teensy.cpp), so the runner does not induce
    anything itself: it observes the reset the stall causes. One checkpoint per
    invocation, because each needs its own image and its own restore.

    **The board is not readable while it is resetting, and that is structural,
    not a timing problem.** aius_fault_checkpoint() spins forever, and it is
    called from the checkpoint callback *before* the checkpoint's service runs
    -- so a fault image serves USB at most once per boot (the single USB_RX /
    USB_TX pass of its one and only epoch) and never at all when the stall is at
    checkpoint 0, 1 or 2. HTTP and UDP need checkpoints 4-6, which no fault
    image reaches. So no channel answers between the induced resets; the board
    becomes readable only once two consecutive early watchdog resets latch
    recovery-safe mode, which runs checkpoints 1 and 2 only and therefore never
    reaches the stall again.

    This gate therefore has two evidence paths, and says which one it used:

    - **cycle** -- two or more boots were actually observed. The full criteria
      apply: the reset interval sits in the 8-10 s watchdog window, the reported
      checkpoint matches, the reset source is the watchdog.
    - **recovery-safe latch** -- nothing answered until the board came back in
      recovery-safe mode. What that proves is still the substance of the plan's
      step 3: the stall reset the board, it did so at least twice (early
      watchdog resets reached the threshold), the reset source was the watchdog,
      and service returned without intervention. What it cannot prove is the
      8-10 s interval or the reported checkpoint, and both are reported as
      unchecked rather than quietly passed. The stalled checkpoint is not
      recoverable after the fact at all: AiusRecoveryStatus.last_checkpoint is
      the live runtime's, and the retained ledger's copy is overwritten by the
      recovered boot's own first checkpoint.
    """
    name = "stalls"
    checkpoint = options.checkpoint
    if checkpoint is None:
        return GateResult(name, UNRESOLVED,
                          "no fault image declared",
                          ["pass --checkpoint 0..8 naming the flashed fault image"])
    if checkpoint not in INJECTABLE_CHECKPOINTS:
        return GateResult(name, UNRESOLVED,
                          "checkpoint %s is not injectable" % checkpoint,
                          ["injectable checkpoints are %s" %
                           ", ".join(str(value) for value in INJECTABLE_CHECKPOINTS)])
    if not options.allow_mutation:
        return GateResult(name, UNRESOLVED,
                          "read-only run: a fault image was not flashed",
                          ["pass --allow-mutation once the fault image is on the board"])
    want_usb = bool(options.serial)
    label = "%d %s" % (checkpoint, CHECKPOINT_NAMES[checkpoint])
    observations = []
    # The first read is retried across the whole horizon rather than taken once:
    # a fault image is unreadable for as long as it is resetting, so an
    # immediate failure means "the stall is working", not "nothing happened".
    started = link.now()
    horizon = started + options.stall_horizon
    baseline = None
    channel = None
    last_error = None
    while link.now() < horizon:
        try:
            channel, baseline = read_any_status(link, want_usb)
            break
        except LinkError as error:
            last_error = error
            link.sleep(options.poll_interval)
    if baseline is None:
        return GateResult(name, UNRESOLVED,
                          "checkpoint %s: no channel answered within %.0fs" %
                          (label, options.stall_horizon),
                          observations + [str(last_error)])
    observations.append(
        "checkpoint %s, first answer via %s at %.2fs: boot_generation=%s "
        "recovery_safe=%s early=%s" %
        (label, channel, link.now() - started, baseline.get("boot_generation"),
         baseline.get("recovery_safe"), baseline.get("watchdog_early_resets")))
    boots = [(baseline.get("boot_generation"), link.now(), baseline)]
    while link.now() < horizon and len(boots) < 3:
        try:
            _channel, status = read_any_status(link, want_usb)
        except LinkError:
            link.sleep(options.poll_interval)
            continue
        generation = status.get("boot_generation")
        if generation is not None and boots[-1][0] is not None and generation > boots[-1][0]:
            boots.append((generation, link.now(), status))
            observations.append("boot_generation %s observed at %.2fs" %
                                (generation, link.now() - boots[0][1]))
        link.sleep(options.poll_interval)
    if len(boots) < 2:
        return _stall_recovery_safe_result(name, label, baseline, observations,
                                           options)
    failures = []
    if len(boots) >= 3:
        interval = boots[2][1] - boots[1][1]
        basis = "cycle"
    else:
        interval = boots[1][1] - boots[0][1]
        basis = "first-observation"
    observations.append("reset interval %.2fs (%s basis)" % (interval, basis))
    upper = STALL_RESET_MAX_S + options.reset_margin
    if interval > upper:
        failures.append("reset took %.2fs, above %.1f-%.1fs (+%.1fs observation margin)"
                        % (interval, STALL_RESET_MIN_S, STALL_RESET_MAX_S,
                           options.reset_margin))
    # A first-observation interval starts when the host first saw the board,
    # not when the board booted, so it can only be an upper bound; the lower
    # bound is asserted for a full cycle between two fresh boots.
    if basis == "cycle" and interval < STALL_RESET_MIN_S:
        failures.append("reset took %.2fs, below the %.1fs watchdog interval"
                        % (interval, STALL_RESET_MIN_S))
    after = boots[-1][2]
    reported = after.get("last_checkpoint")
    observations.append("reported last_checkpoint=%s early=%s recovery_safe=%s" %
                        (reported, after.get("watchdog_early_resets"),
                         after.get("recovery_safe")))
    if reported != checkpoint:
        failures.append("reported checkpoint %s, expected %s" % (reported, checkpoint))
    if reset_kind(after.get("reset_source")) != "watchdog":
        failures.append("reset source %s, expected a watchdog reset" %
                        reset_kind(after.get("reset_source")))
    service = "recovery-safe" if after.get("recovery_safe") else "normal"
    observations.append("service returned: %s, without intervention" % service)
    if failures:
        return GateResult(name, FAIL,
                          "checkpoint %s: %s" % (label, failures[0]),
                          observations + failures)
    return GateResult(name, PASS,
                      "checkpoint %s reset in %.2fs and returned to %s service"
                      % (label, interval, service), observations)


def _stall_recovery_safe_result(name, label, status, observations, options):
    """The evidence path for a fault image that was never readable mid-loop.

    Only reachable when no second boot was observed. A board that answered once
    and never advanced its generation has either latched recovery-safe -- which
    takes two early watchdog resets and is what the recovery boundary exists to
    do -- or it never reset at all, and those two are exactly what the retained
    fields distinguish. Before they were published in recovery-safe mode they
    all read 0 and the two were indistinguishable on the wire.
    """
    early = status.get("watchdog_early_resets")
    kind = reset_kind(status.get("reset_source"))
    observations = observations + [
        "no second boot observed: the fault image is unreadable while it "
        "resets (see gate_stalls)",
        "retained state: boot_generation=%s reset_source=%s (%s) "
        "watchdog_early_resets=%s recovery_safe=%s" %
        (status.get("boot_generation"), status.get("reset_source"), kind,
         early, status.get("recovery_safe")),
        "not checked on this path: the 8-10 s reset interval, and the "
        "stalled checkpoint (not retained past the recovered boot)"]
    if not status.get("recovery_safe"):
        return GateResult(name, FAIL,
                          "checkpoint %s: no reset within %.0fs and the board "
                          "is not in recovery-safe mode" %
                          (label, options.stall_horizon), observations)
    failures = []
    if kind != "watchdog":
        failures.append("reset source %s, expected a watchdog reset" % kind)
    if not early or early < 2:
        failures.append("watchdog_early_resets=%s, expected at least 2 -- "
                        "recovery-safe mode cannot be reached with fewer" % early)
    if not status.get("boot_generation"):
        failures.append("boot_generation=%s: the retained ledger did not "
                        "survive the induced resets" % status.get("boot_generation"))
    if failures:
        return GateResult(name, FAIL, "checkpoint %s: %s" % (label, failures[0]),
                          observations + failures)
    return GateResult(name, PASS,
                      "checkpoint %s latched recovery-safe after %s early "
                      "watchdog resets and returned to service without "
                      "intervention" % (label, early), observations)


def gate_soak(link, options):
    """Step 4 -- a short read-only soak of HTTP and UDP traffic."""
    name = "soak"
    try:
        baseline = link.http_status()
    except LinkError as http_error:
        try:
            baseline = link.udp_status()
        except LinkError as udp_error:
            return GateResult(name, UNRESOLVED,
                              "device unreachable: the soak never started",
                              [str(http_error), str(udp_error)])
    observations = ["baseline boot_generation=%s network_generation=%s "
                    "network_restarts=%s" %
                    (baseline.get("boot_generation"),
                     baseline.get("network_generation"),
                     baseline.get("network_restarts"))]
    started = link.now()
    deadline = started + options.soak_duration
    samples = 0
    outages = 0
    failures = []
    cursor = 0
    last = baseline
    while link.now() < deadline:
        samples += 1
        sample = None
        for reader in (link.http_status, link.udp_status):
            try:
                sample = reader()
            except LinkError:
                continue
            break
        if sample is None:
            outages += 1
            if outages > options.soak_tolerated_outages:
                failures.append("outage at %.0fs (sample %d): no HTTP or UDP answer"
                                % (link.now() - started, samples))
                break
            link.sleep(options.soak_interval)
            continue
        try:
            events = link.http_json("GET", "/api/events?cursor=%d" % cursor)
            if isinstance(events.get("body"), dict):
                cursor = int(events["body"].get("next", cursor))
        except LinkError:
            pass
        for field, label in (("boot_generation", "unexpected reset"),
                             ("network_generation", "leaked network generation"),
                             ("network_restarts", "network restart")):
            before = baseline.get(field)
            now = sample.get(field)
            if before is not None and now is not None and now != before:
                failures.append("%s at %.0fs: %s %s -> %s" %
                                (label, link.now() - started, field, before, now))
        last = sample
        if failures:
            break
        link.sleep(options.soak_interval)
    observations.append("%d samples over %.0fs, %d outages tolerated %d" %
                        (samples, link.now() - started, outages,
                         options.soak_tolerated_outages))
    for field in ("http_service_overruns", "network_drops",
                  "network_budget_overruns", "http_client_drops"):
        before, now = baseline.get(field), last.get(field)
        if before is not None and now is not None:
            observations.append("%s %s -> %s" % (field, before, now))
    if failures:
        return GateResult(name, FAIL, failures[0], observations + failures)
    return GateResult(name, PASS,
                      "%d samples over %.0fs with no reset, outage or leaked generation"
                      % (samples, link.now() - started), observations)


def gate_dsp(link, options):
    """Step 5 -- the DSP strand's target validation in the same session."""
    name = "dsp"
    observations = []
    failures = []
    try:
        before = link.http_json("GET", "/api/dsp/config")
        status_before = link.http_status()
    except LinkError as error:
        return GateResult(name, UNRESOLVED,
                          "device unreachable: no DSP surface observed", [str(error)])
    if before["status"] != 200 or not isinstance(before["body"], dict):
        return GateResult(name, FAIL,
                          "GET /api/dsp/config answered %s" % before["status"],
                          observations)
    model = before["body"]
    observations.append("dsp config source=%s config_generation=%s sample_rate_hz=%s" %
                        (model.get("source"), model.get("config_generation"),
                         model.get("sample_rate_hz")))
    if model.get("source") != "default":
        failures.append("default audio changed: source=%s" % model.get("source"))
    failures.extend(_check_meters(link, observations))
    failures.extend(_check_editor(link, observations))
    if not options.allow_mutation:
        return GateResult(name, UNRESOLVED,
                          "read-only run: the GET/PUT/SD/reset round-trip was skipped",
                          observations + failures +
                          ["pass --allow-mutation to drive the config round-trip"])
    round_trip = _config_round_trip(link, options, model, observations)
    if round_trip is None:
        return GateResult(name, UNRESOLVED,
                          "the config round-trip could not be driven",
                          observations + failures)
    failures.extend(round_trip)
    try:
        status_after = link.http_status()
    except LinkError as error:
        failures.append("no status after the round-trip (%s)" % error)
        status_after = {}
    for field in ("state", "format", "sample_rate"):
        if field in status_before and status_before.get(field) != status_after.get(field):
            failures.append("audio %s changed: %s -> %s" %
                            (field, status_before.get(field), status_after.get(field)))
    if failures:
        return GateResult(name, FAIL, failures[0], observations + failures)
    return GateResult(name, PASS,
                      "default audio unchanged, config round-trip committed and reset, "
                      "meters and editor served", observations)


def _check_meters(link, observations):
    try:
        meters = link.http_json("GET", "/api/meters")
    except LinkError as error:
        return ["meters unreadable (%s)" % error]
    if meters["status"] != 200 or not isinstance(meters["body"], dict):
        return ["GET /api/meters answered %s" % meters["status"]]
    body = meters["body"]
    observations.append("meters sequence=%s inputs=%d waveform=%d" %
                        (body.get("sequence"), len(body.get("input") or []),
                         len(body.get("waveform") or [])))
    failures = []
    channels = body.get("input")
    if not isinstance(channels, list) or len(channels) != 6:
        failures.append("meters served %s input channels, expected 6" %
                        (len(channels) if isinstance(channels, list) else "no"))
    else:
        for index, channel in enumerate(channels):
            if not isinstance(channel, dict) or "peak_dbfs" not in channel \
                    or "rms_dbfs" not in channel:
                failures.append("meters input channel %d has no peak/rms" % index)
    output = body.get("output")
    if not isinstance(output, dict) or len(output.get("peak_dbfs") or []) != 2:
        failures.append("meters output is not a stereo peak/rms pair")
    if not isinstance(body.get("waveform"), list):
        failures.append("meters served no waveform")
    return failures


def _check_editor(link, observations):
    failures = []
    for resource in EDITOR_RESOURCES:
        try:
            result = link.http_bytes("/sd/" + resource)
        except LinkError as error:
            failures.append("editor resource %s unreadable (%s)" % (resource, error))
            continue
        length = len(result.get("body") or b"")
        observations.append("editor /sd/%s -> %s, %d bytes" %
                            (resource, result["status"], length))
        if result["status"] != 200 or length == 0:
            failures.append("editor resource %s answered %s with %d bytes" %
                            (resource, result["status"], length))
    return failures


def _commit_without_apply(link, generation, committed, before_counters,
                          observations):
    """The commit evidence for a board whose block-boundary apply cannot run.

    Only the SD-job path is available here, and it is only accepted when the
    board actually cannot apply. On a board with audio, an unreported commit is
    the failure it looks like.
    """
    try:
        status = link.http_status()
    except LinkError as error:
        return ["no status to establish the commit basis (%s)" % error]
    if _apply_is_reachable(status):
        failures = []
        if committed.get("source") != "sd":
            failures.append("config did not commit to SD: source=%s" %
                            committed.get("source"))
        if committed.get("config_generation") != generation + 1:
            failures.append("config_generation %s -> %s, expected +1" %
                            (generation, committed.get("config_generation")))
        return failures
    observations.append(
        "commit basis: sd-job -- carrier=%s blocks=%s, so no audio block "
        "boundary occurred and the staged model cannot have applied" %
        (status.get("carrier"), status.get("blocks")))
    after_counters = _sd_write_counters(status)
    if before_counters is None or after_counters is None:
        return ["no SD job counters to witness the write"]
    observations.append("sd_diag http_jobs %s -> %s, http_faults %s -> %s, "
                        "job_state %s" %
                        (before_counters["jobs"], after_counters["jobs"],
                         before_counters["faults"], after_counters["faults"],
                         after_counters["state"]))
    failures = []
    if not isinstance(after_counters["jobs"], int) or \
            not isinstance(before_counters["jobs"], int) or \
            after_counters["jobs"] <= before_counters["jobs"]:
        failures.append("the PUT started no SD job: http_jobs %s -> %s" %
                        (before_counters["jobs"], after_counters["jobs"]))
    if after_counters["faults"] != before_counters["faults"]:
        failures.append("the SD write faulted: http_faults %s -> %s" %
                        (before_counters["faults"], after_counters["faults"]))
    if after_counters["state"] != SD_JOB_DONE:
        failures.append("the SD job ended in state %s, expected DONE (%d)" %
                        (after_counters["state"], SD_JOB_DONE))
    observations.append("not checked on this path: the block-boundary apply, "
                        "and therefore source and config_generation")
    return failures


def _sd_write_counters(status):
    """The SD job counters that witness a durable write, or None."""
    diag = status.get("sd_diag")
    if not isinstance(diag, dict):
        return None
    return {"jobs": diag.get("http_jobs"), "faults": diag.get("http_faults"),
            "state": diag.get("job_state")}


def _apply_is_reachable(status):
    """False when no audio block boundary can occur, so no staged model applies.

    `source` and `config_generation` are written in exactly one place --
    AiusAudioProcessor::enqueue()'s staged-config arm, which runs at an audio
    block boundary (optical_main.cpp:880-886). A board with no optical carrier
    produces no block, so neither field can move however well the write went.
    """
    return bool(status.get("carrier")) or bool(status.get("blocks"))


def _config_round_trip(link, options, model, observations):
    """GET -> PUT (commits to SD) -> verify -> reset to the captured default.

    Returns the failure list, or None when the round-trip could not be driven
    at all (no token, no SD) -- an unresolved gate rather than a failed one.

    **The commit has two evidence paths, and this says which one it used**, for
    the same reason gate_stalls does: the observable one is not always reachable.

    - **applied** -- `GET /api/dsp/config` reports `source: "sd"` and the next
      config generation. This is the full claim: written to SD *and* active.
    - **sd-job** -- the board has no carrier and has processed no audio block, so
      the block-boundary apply provably cannot have run and both fields must
      still read their boot values. The durable write is then witnessed by the
      SD job counters instead: one more completed HTTP-owned job, no new fault,
      terminal state DONE. What that cannot show is the apply, and it is reported
      as unchecked rather than quietly passed.

    A board that has audio and still does not report the commit is a failure on
    either path.
    """
    generation = model.get("config_generation")
    if generation is None:
        return None
    try:
        before_status = link.http_status()
    except LinkError:
        before_status = {}
    before_counters = _sd_write_counters(before_status)
    body = dict(model)
    body["base_generation"] = generation
    try:
        put = link.http_json("PUT", "/api/dsp/config", body, auth=True)
    except LinkError as error:
        observations.append("config PUT not driven: %s" % error)
        return None
    observations.append("PUT /api/dsp/config -> %s" % put["status"])
    if put["status"] in (401, 503):
        observations.append("round-trip blocked: %s" % put["status"])
        return None
    failures = []
    if put["status"] != 200:
        failures.append("PUT /api/dsp/config answered %s" % put["status"])
        return failures
    try:
        after = link.http_json("GET", "/api/dsp/config")
    except LinkError as error:
        return ["no config after the PUT (%s)" % error]
    committed = after["body"] if isinstance(after.get("body"), dict) else {}
    observations.append("after PUT source=%s config_generation=%s" %
                        (committed.get("source"), committed.get("config_generation")))
    applied = (committed.get("source") == "sd" and
               committed.get("config_generation") == generation + 1)
    if applied:
        observations.append("commit basis: applied (source=sd, generation +1)")
    else:
        failures.extend(_commit_without_apply(link, generation, committed,
                                              before_counters, observations))
    reset_body = dict(model)
    reset_body["base_generation"] = committed.get("config_generation", generation + 1)
    try:
        reset = link.http_json("PUT", "/api/dsp/config", reset_body, auth=True)
        restored = link.http_json("GET", "/api/dsp/config")
    except LinkError as error:
        return failures + ["config not reset (%s)" % error]
    observations.append("reset PUT -> %s" % reset["status"])
    if reset["status"] != 200:
        failures.append("reset PUT answered %s" % reset["status"])
        return failures
    final = restored["body"] if isinstance(restored.get("body"), dict) else {}
    for field, value in model.items():
        if field in ("source", "config_generation"):
            continue
        if final.get(field) != value:
            failures.append("reset left %s at %r, expected %r" %
                            (field, final.get(field), value))
    return failures


GATE_FUNCTIONS = {"reboots": gate_reboots, "stalls": gate_stalls,
                  "soak": gate_soak, "dsp": gate_dsp}


# --------------------------------------------------------------------------
# plan of work, summary, entry point
# --------------------------------------------------------------------------


def plan_of_work(options):
    """The ordered, serialized plan the shell entry point drives.

    One device and one USB enumeration serialize every mutating step, and each
    fault image is restored to the production image before the next one is
    built, so a failed restore stops the run rather than carrying a fault image
    into the following checkpoint.
    """
    lines = ["one device, one fixture: every mutating step below is serialized",
             "read-only probe: USB ID, HTTP status, UDP status"]
    if "reboots" in options.gates:
        lines.append("gate reboots (mutating): %d x POST /api/reboot, each ready "
                     "inside DHCP %.0fs / HTTP %.0fs / UDP %.0fs / USB %.0fs"
                     % (options.reboots, DHCP_DEADLINE_S, HTTP_DEADLINE_S,
                        UDP_DEADLINE_S, USB_ID_DEADLINE_S))
    if "stalls" in options.gates:
        checkpoints = (INJECTABLE_CHECKPOINTS if options.checkpoint is None
                       else (options.checkpoint,))
        for checkpoint in checkpoints:
            label = "%d %s" % (checkpoint, CHECKPOINT_NAMES[checkpoint])
            lines.append("gate stalls (mutating) checkpoint %s: build "
                         "teensy41-optical-fault %d" % (label, checkpoint))
            lines.append("gate stalls (mutating) checkpoint %s: flash the fault image"
                         % label)
            lines.append("gate stalls (mutating) checkpoint %s: observe reset in "
                         "%.0f-%.0fs, reported checkpoint, service back"
                         % (label, STALL_RESET_MIN_S, STALL_RESET_MAX_S))
            lines.append("gate stalls (mutating) checkpoint %s: restore "
                         "teensy41-optical and verify the build revision" % label)
    if "soak" in options.gates:
        lines.append("gate soak (read-only): %.0fs of HTTP/UDP traffic"
                     % options.soak_duration)
    if "dsp" in options.gates:
        lines.append("gate dsp (mutating round-trip): default audio, "
                     "GET/PUT/SD/reset config, meters, SD editor")
    return lines


def format_summary(run):
    """The readable per-run summary of §Live smoke evidence."""
    identity = run.identity or {}
    lines = ["==> Aius recovery smoke %s target=%s" % (run.started_utc, run.target),
             "    revision=%s build=%s via=%s" %
             (identity.get("revision", "unknown"), identity.get("build", "unknown"),
              identity.get("channel", "none"))]
    for field in RECOVERY_FIELDS:
        if field in identity:
            lines.append("    %s=%s" % (field, identity[field]))
    for result in run.results:
        lines.append("%-10s %-10s %s" % (result.name, result.outcome.upper(),
                                         result.detail))
        for observation in result.observations:
            lines.append("    %s" % observation)
    lines.append("==> %s" % _verdict(run.results))
    return "\n".join(lines)


def _verdict(results):
    if any(result.outcome == FAIL for result in results):
        return "FAIL: " + ", ".join(result.name for result in results
                                    if result.outcome == FAIL)
    if any(result.outcome == UNRESOLVED for result in results):
        return "UNRESOLVED: " + ", ".join(result.name for result in results
                                          if result.outcome == UNRESOLVED)
    return "PASS: " + ", ".join(result.name for result in results)


def exit_code(results):
    if any(result.outcome == FAIL for result in results):
        return 2
    if any(result.outcome == UNRESOLVED for result in results):
        return 3
    return 0


def read_identity(link, want_usb):
    """Build revision and the recovery fields, for the summary header."""
    try:
        channel, status = read_any_status(link, want_usb)
    except LinkError as error:
        return {"channel": "none", "error": str(error)}
    identity = {"channel": channel}
    identity["revision"] = status.get("revision") or status.get("build_revision")
    identity["build"] = status.get("build") or status.get("build_utc")
    for field in RECOVERY_FIELDS:
        if field in status:
            identity[field] = status[field]
    identity["reset_kind"] = reset_kind(status.get("reset_source"))
    return identity


def run_gates(link, options):
    run = SmokeRun(target=options.target,
                   started_utc=datetime.datetime.now(datetime.timezone.utc)
                   .strftime("%Y-%m-%dT%H:%M:%SZ"),
                   identity=read_identity(link, bool(options.serial)))
    for name in GATE_NAMES:
        if name in options.gates:
            run.results.append(GATE_FUNCTIONS[name](link, options))
    return run


def write_evidence(run, directory):
    path = pathlib.Path(directory) / ("recovery-smoke-%s.txt" %
                                      run.started_utc.replace(":", "").replace("-", ""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(format_summary(run) + "\n", encoding="utf-8")
    return path


def build_parser():
    parser = argparse.ArgumentParser(
        description="Aius proportionate live recovery smoke (plan Phase 16a)")
    parser.add_argument("command", choices=("probe", "plan", "run"))
    parser.add_argument("--target", default="", help="device IPv4 address or host")
    parser.add_argument("--http-port", type=int, default=80)
    parser.add_argument("--udp-port", type=int, default=udp_runner.PORT)
    parser.add_argument("--serial", default=None,
                        help="CDC port for USB ID, e.g. /dev/ttyACM0")
    parser.add_argument("--auth-token", default=None,
                        help="SD maintenance token (default: $AIUS_SD_AUTH)")
    parser.add_argument("--gates", default="all",
                        help="comma-separated: %s, or all" % ",".join(GATE_NAMES))
    parser.add_argument("--allow-mutation", action="store_true",
                        help="opt in to the steps that mutate the board")
    parser.add_argument("--reboots", type=int, default=5)
    parser.add_argument("--checkpoint", type=int, default=None,
                        help="the injectable checkpoint 0..8 the flashed fault image stalls at")
    parser.add_argument("--soak-duration", type=float, default=1800.0)
    parser.add_argument("--soak-interval", type=float, default=1.0)
    parser.add_argument("--soak-tolerated-outages", type=int, default=0)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    parser.add_argument("--ready-probe-timeout", type=float, default=0.3,
                        help="transport timeout for the readiness polls only")
    parser.add_argument("--usb-watch-limit", type=float, default=4.0,
                        help="cap on the CDC re-enumeration watch after a reboot")
    parser.add_argument("--ready-limit", type=float, default=20.0)
    parser.add_argument("--stall-horizon", type=float, default=45.0)
    parser.add_argument("--reset-margin", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--evidence-dir", default=None,
                        help="default: firmware/test/evidence/network-hardening")
    return parser


def options_from_args(args, parser):
    if args.gates == "all":
        gates = GATE_NAMES
    else:
        gates = tuple(name.strip() for name in args.gates.split(",") if name.strip())
        unknown = [name for name in gates if name not in GATE_NAMES]
        if unknown:
            parser.error("unknown gate(s): %s" % ", ".join(unknown))
        if not gates:
            parser.error("--gates selected nothing")
    if args.command in ("probe", "run") and not args.target:
        parser.error("--target is required for %s" % args.command)
    return SmokeOptions(
        target=args.target, http_port=args.http_port, udp_port=args.udp_port,
        serial=args.serial, auth_token=args.auth_token, gates=gates,
        allow_mutation=args.allow_mutation, reboots=args.reboots,
        checkpoint=args.checkpoint, soak_duration=args.soak_duration,
        soak_interval=args.soak_interval,
        soak_tolerated_outages=args.soak_tolerated_outages,
        poll_interval=args.poll_interval,
        ready_probe_timeout=args.ready_probe_timeout,
        usb_watch_limit=args.usb_watch_limit,
        ready_limit=args.ready_limit,
        stall_horizon=args.stall_horizon, reset_margin=args.reset_margin,
        timeout=args.timeout,
        evidence_dir=args.evidence_dir or
        str(FIRMWARE_ROOT / "test" / "evidence" / "network-hardening"))


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    options = options_from_args(args, parser)
    if args.command == "plan":
        for line in plan_of_work(options):
            print("==> " + line)
        return 0
    link = DeviceLink(options)
    if args.command == "probe":
        probe = {"usb_id": None, "http_status": None, "udp_status": None}
        errors = {}
        for channel, reader in (("usb_id", link.usb_id),
                                ("http_status", link.http_status),
                                ("udp_status", link.udp_status)):
            try:
                probe[channel] = reader()
            except LinkError as error:
                errors[channel] = str(error)
        print(json.dumps({"probe": probe, "errors": errors}, indent=2,
                         sort_keys=True, default=str))
        return 0 if any(value is not None for value in probe.values()) else 3
    run = run_gates(link, options)
    summary = format_summary(run)
    print(summary)
    path = write_evidence(run, options.evidence_dir)
    print("==> summary written to %s" % path)
    return exit_code(run.results)


if __name__ == "__main__":
    sys.exit(main())
