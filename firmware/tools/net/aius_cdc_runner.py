"""Host-side CDC control and button-free flashing for the Aius optical image.

WHAT THIS REPLACES. Flashing used to need a finger on the Program button, and
`scripts/flash_optical.sh` only softened that by asking the RawHID application
to reboot -- which works when the sketch is healthy and does nothing at all when
it is not. A single B6 gate-3 run cost nine button presses. This module is the
button-free path, and it is built on two mechanisms rather than one because
neither covers the whole failure surface:

  * `AIUS BOOTLOADER <id>` over CDC is the sanctioned route
    (docs/reference/usb-control-protocol.md §Bootloader entry). It drains the
    USB transmit ring before entering HalfKay, so the response reaches the host
    and the command is repeatable. It needs the epoch loop to be running.

  * Setting the CDC line coding to 134 baud is the hatch for when it is not.
    The Teensy core arms usb_reboot_timer from the SET_LINE_CODING handler and
    fires it from the SOF interrupt (cores/teensy4/usb.c:841, :383), so it
    reboots a board whose main loop is hung -- the only way back from a fault
    image stalled at checkpoint 1 or 2, where the USB control path never runs.

THE ACKNOWLEDGEMENT IS NOT THE PROOF. A bootloader request that is performed but
whose response never arrives looks identical to one that was refused, and the
firmware's own history contains exactly that defect twice (the bootloader-entry
race and POST /api/reboot). So entry is judged by the port DISAPPEARING, never
by the OK line: a lost acknowledgement with the port gone is a success, and an
acknowledged request with the port still present is not.

VERIFICATION IS AGAINST THE IMAGE, NOT AGAINST THE BUILD SCRIPT. After the
loader runs, the board is asked what it is (`AIUS ID`) and the answer must match
the identity read out of the .hex that was just written. The identity comes from
parsing the image itself rather than from re-running the build's own metadata
derivation, so a build that stamped the wrong thing is caught instead of
confirmed -- the same "a test sharing an expression with the code proves only
agreement" lesson this firmware has paid for repeatedly.

Every failure is closed: no identity found in the image, no answer from the
board, or an identity that does not match, all exit non-zero.
"""

import argparse
import glob
import os
import re
import select
import subprocess
import sys
import time

# The stable port path. /dev/ttyACMn is WRONG for anything that survives a
# reflash: Linux hands the re-enumerated port the next free index, so a board
# that comes back as ttyACM1 leaves a ttyACM0 caller permanently absent.
DEFAULT_PORT_GLOB = "/dev/serial/by-id/usb-Teensyduino_USB_Serial_*-if00"

# HalfKay's VID/PID, for the operator-facing message only; the loader finds it.
HALFKAY_USB_ID = "16c0:0478"

PROTOCOL_PREFIX = "AIUS/1 "

# One control exchange's ceiling. The device answers ID in well under a second
# when the epoch loop is running; this is generous enough to ride out an epoch
# that is busy with an SD job and short enough that six attempts stay bounded.
EXCHANGE_TIMEOUT_S = 3.0

# How long to wait for the port to vanish after a bootloader request. The
# firmware bounds its own drain at 500 ms and then enters the loader
# regardless, so anything beyond a couple of seconds means it did not go.
BOOTLOADER_EXIT_TIMEOUT_S = 5.0

# How long to wait for the application to re-enumerate after the loader runs.
# Measured USB readiness after a reboot is 1.51-1.64 s; the margin covers a
# cold enumeration on a busy host.
REENUMERATE_TIMEOUT_S = 30.0

# The identity strings the build stamps into the image (scripts/build_metadata.py).
# `unknown` is the revision the build uses when git is unavailable, and it must
# be matchable or a source-less build could never be verified.
IMAGE_REVISION_RE = re.compile(rb"(?:[0-9a-f]{12}|unknown)\+teensy41-[A-Za-z0-9_.-]+")
IMAGE_BUILD_UTC_RE = re.compile(rb"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z")


class CdcError(RuntimeError):
    """Any failure reaching, commanding or verifying the board."""


# --------------------------------------------------------------------------
# protocol framing
# --------------------------------------------------------------------------


def select_response(text, request_id):
    """Pick one request's answer out of whatever the CDC returned.

    The board interleaves diagnostic output with protocol lines and a partial
    line may precede the answer, so this matches on the framing and the request
    id rather than assuming the first line is the response. Same rule as
    aius_recovery_smoke.select_usb_response, and deliberately the same shape --
    two host tools disagreeing about the framing would be its own defect.
    """
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(PROTOCOL_PREFIX):
            continue
        parts = line.split(" ")
        if len(parts) >= 3 and parts[2] == str(request_id):
            return line
    return None


def parse_identity(line):
    """Decode `AIUS/1 OK <id> ID key=value ...` into a dict of fields."""
    parts = line.strip().split(" ")
    if len(parts) < 5 or parts[0] != "AIUS/1":
        raise CdcError("not an Aius control response: %r" % line)
    if parts[1] == "ERR":
        raise CdcError("device answered " + " ".join(parts[1:]))
    if parts[1] != "OK" or parts[3] != "ID":
        raise CdcError("not an ID response: %r" % line)
    fields = {}
    for token in parts[4:]:
        key, separator, value = token.partition("=")
        if not separator or not key:
            raise CdcError("malformed ID field: %r" % token)
        fields[key] = value
    return fields


def parse_auth(line):
    """Decode `AIUS/1 OK <id> AUTH <token>` into the token."""
    parts = line.strip().split(" ")
    if len(parts) < 5 or parts[1] != "OK" or parts[3] != "AUTH":
        raise CdcError("not an AUTH response: %r" % line)
    return parts[4]


# --------------------------------------------------------------------------
# image identity -- the independent oracle
# --------------------------------------------------------------------------


def parse_intel_hex(text):
    """Assemble an Intel HEX file into {absolute_address: byte}.

    Records are reassembled by absolute address rather than concatenated,
    because a stamped string can straddle a record boundary and concatenation
    would silently split it -- which would make the identity unfindable exactly
    when the file is large enough to matter.
    """
    memory = {}
    base = 0
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise CdcError("line %d is not an Intel HEX record" % number)
        try:
            payload = bytes.fromhex(line[1:])
        except ValueError as error:
            raise CdcError("line %d is not hexadecimal: %s" % (number, error)) from error
        if len(payload) < 5:
            raise CdcError("line %d is too short for a record" % number)
        count, address, kind = payload[0], (payload[1] << 8) | payload[2], payload[3]
        data = payload[4:-1]
        if len(data) != count:
            raise CdcError("line %d declares %d bytes and carries %d"
                           % (number, count, len(data)))
        if (sum(payload) & 0xFF) != 0:
            raise CdcError("line %d fails its checksum" % number)
        if kind == 0x00:
            for offset, value in enumerate(data):
                memory[base + address + offset] = value
        elif kind == 0x01:
            break
        elif kind == 0x04:
            base = ((data[0] << 8) | data[1]) << 16
        elif kind == 0x02:
            base = ((data[0] << 8) | data[1]) << 4
        # 0x03 and 0x05 carry a start address and hold no image bytes.
    return memory


def contiguous_runs(memory):
    """Yield each contiguous byte run of an assembled image, in address order."""
    run = bytearray()
    previous = None
    for address in sorted(memory):
        if previous is not None and address != previous + 1:
            yield bytes(run)
            run = bytearray()
        run.append(memory[address])
        previous = address
    if run:
        yield bytes(run)


def image_identity(hex_path):
    """Read the build identity the image stamps into its own flash.

    Returns {"build_revision": str, "build_utc": str}. Raises when either is
    absent, which is the fail-closed case: an image whose identity cannot be
    read cannot be verified after flashing, so it is refused before the loader
    is allowed to touch the board.
    """
    try:
        with open(hex_path, "r", encoding="ascii") as handle:
            text = handle.read()
    except OSError as error:
        raise CdcError("cannot read %s: %s" % (hex_path, error)) from error

    revision = None
    build_utc = None
    for run in contiguous_runs(parse_intel_hex(text)):
        if revision is None:
            found = IMAGE_REVISION_RE.search(run)
            if found:
                revision = found.group().decode("ascii")
        if build_utc is None:
            found = IMAGE_BUILD_UTC_RE.search(run)
            if found:
                build_utc = found.group().decode("ascii")
        if revision and build_utc:
            break

    if not revision:
        raise CdcError("no build revision found in %s" % hex_path)
    if not build_utc:
        raise CdcError("no build timestamp found in %s" % hex_path)
    return {"build_revision": revision, "build_utc": build_utc}


# --------------------------------------------------------------------------
# transport
# --------------------------------------------------------------------------


class SerialTransport:
    """The real CDC port. Opened per exchange, never held.

    Holding the descriptor across a reflash is what makes a stale /dev/ttyACMn
    reference survive as a working handle to nothing; opening per exchange means
    every call re-resolves the by-id symlink to whatever the board is now.
    """

    def __init__(self, port, timeout=EXCHANGE_TIMEOUT_S):
        self.port = port
        self.timeout = timeout

    def present(self):
        return os.path.exists(self.port)

    def exchange(self, command, request_id):
        try:
            descriptor = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as error:
            raise CdcError("open %s: %s" % (self.port, error)) from error
        try:
            os.write(descriptor, command.encode("ascii"))
            deadline = time.monotonic() + self.timeout
            buffer = ""
            while time.monotonic() < deadline:
                ready, _, _ = select.select([descriptor], [], [],
                                            max(0.0, deadline - time.monotonic()))
                if not ready:
                    continue
                buffer += os.read(descriptor, 512).decode("ascii", errors="replace")
                line = select_response(buffer, request_id)
                if line:
                    return line
        except OSError as error:
            raise CdcError("exchange on %s: %s" % (self.port, error)) from error
        finally:
            os.close(descriptor)
        return None

    def force_reboot(self):
        """The 134-baud hatch. Works while the sketch's main loop is hung."""
        import termios

        try:
            descriptor = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as error:
            raise CdcError("open %s: %s" % (self.port, error)) from error
        try:
            attributes = termios.tcgetattr(descriptor)
            attributes[4] = termios.B134
            attributes[5] = termios.B134
            termios.tcsetattr(descriptor, termios.TCSANOW, attributes)
        except OSError as error:
            raise CdcError("set 134 baud on %s: %s" % (self.port, error)) from error
        finally:
            os.close(descriptor)


# --------------------------------------------------------------------------
# link
# --------------------------------------------------------------------------


class CdcLink:
    """One board's control channel. `transport` is injectable for tests."""

    def __init__(self, port=None, transport=None, sleep=time.sleep,
                 clock=time.monotonic):
        if transport is None:
            if port is None:
                port = discover_port()
            transport = SerialTransport(port)
        self.transport = transport
        self.port = port
        self._sleep = sleep
        self._clock = clock
        self._request_id = 0

    def _next_id(self):
        # Wraps below the protocol's 32-bit ceiling; 0 is reserved for the
        # id-less legacy forms, so it is skipped rather than sent.
        self._request_id = (self._request_id % 4294967294) + 1
        return self._request_id

    def command(self, verb):
        request_id = self._next_id()
        line = self.transport.exchange("AIUS %s %d\n" % (verb, request_id),
                                       request_id)
        if line is None:
            raise CdcError("no answer to AIUS %s %d" % (verb, request_id))
        return line

    def identity(self):
        return parse_identity(self.command("ID"))

    def auth(self):
        return parse_auth(self.command("AUTH"))

    def wait_absent(self, timeout):
        deadline = self._clock() + timeout
        while self._clock() < deadline:
            if not self.transport.present():
                return True
            self._sleep(0.1)
        return not self.transport.present()

    def wait_present(self, timeout):
        deadline = self._clock() + timeout
        while self._clock() < deadline:
            if self.transport.present():
                return True
            self._sleep(0.1)
        return self.transport.present()

    def enter_bootloader(self, attempts=3, use_hatch=True):
        """Get the board into HalfKay, or raise.

        THE PORT DISAPPEARING IS THE ONLY EVIDENCE ACCEPTED. A CdcError from the
        exchange is not fatal here and deliberately so: `AIUS BOOTLOADER` may
        enter the loader before its acknowledgement drains, and on a hung sketch
        no answer was ever coming. Both cases are settled by the same check.
        """
        last = None
        for attempt in range(1, attempts + 1):
            try:
                self.command("BOOTLOADER")
            except CdcError as error:
                last = error
            if self.wait_absent(BOOTLOADER_EXIT_TIMEOUT_S):
                return "cdc"
            if use_hatch:
                # The sketch is not servicing commands. Reboot it from the SOF
                # interrupt instead, which does not need the epoch loop.
                try:
                    self.transport.force_reboot()
                except CdcError as error:
                    last = error
                if self.wait_absent(BOOTLOADER_EXIT_TIMEOUT_S):
                    return "line-coding"
            del attempt
        raise CdcError("board never entered HalfKay"
                       + (" (last error: %s)" % last if last else ""))


def discover_port(pattern=DEFAULT_PORT_GLOB):
    matches = sorted(glob.glob(pattern))
    if not matches:
        raise CdcError("no Teensy CDC port matching %s" % pattern)
    if len(matches) > 1:
        raise CdcError("%d Teensy CDC ports match %s; pass --port"
                       % (len(matches), pattern))
    return matches[0]


# --------------------------------------------------------------------------
# flashing
# --------------------------------------------------------------------------


def run_loader(loader, hex_path, runner=subprocess.run, timeout=120):
    """Invoke teensy_loader_cli against a waiting HalfKay device."""
    try:
        completed = runner([loader, "--mcu=imxrt1062", "-w", "-v", hex_path],
                           timeout=timeout)
    except OSError as error:
        raise CdcError("cannot run %s: %s" % (loader, error)) from error
    except subprocess.TimeoutExpired as error:
        raise CdcError("loader timed out after %ss" % timeout) from error
    if completed.returncode != 0:
        raise CdcError("loader exited %d" % completed.returncode)
    return True


def verify_identity(link, expected, timeout=REENUMERATE_TIMEOUT_S):
    """Confirm the running image is the one just written, or raise.

    Both fields are compared. The revision alone is not enough: rebuilding the
    same commit produces the same revision string, so an image that was never
    actually written would verify against it. build_utc is stamped per build and
    is what distinguishes "the new image is running" from "the old one still
    is".
    """
    if not link.wait_present(timeout):
        raise CdcError("board did not re-enumerate within %ss" % timeout)
    # The port reappearing precedes the application answering; retry until the
    # control channel is actually up rather than failing the first exchange.
    deadline = link._clock() + timeout
    last = None
    while link._clock() < deadline:
        try:
            running = link.identity()
            break
        except CdcError as error:
            last = error
            link._sleep(0.25)
    else:
        raise CdcError("board never answered AIUS ID after flashing"
                       + (" (last error: %s)" % last if last else ""))

    actual = {"build_revision": running.get("revision"),
              "build_utc": running.get("build")}
    if actual != expected:
        raise CdcError("running image is %s built %s; expected %s built %s"
                       % (actual["build_revision"], actual["build_utc"],
                          expected["build_revision"], expected["build_utc"]))
    return actual


def flash(hex_path, link, loader, runner=subprocess.run, log=print):
    """The whole button-free cycle, failing closed at every step."""
    expected = image_identity(hex_path)
    log("==> Image claims %s built %s"
        % (expected["build_revision"], expected["build_utc"]))

    try:
        before = link.identity()
        log("==> Board is running %s built %s"
            % (before.get("revision"), before.get("build")))
    except CdcError as error:
        # Not fatal: a crash-looping or recovery-safe board may not answer, and
        # that is precisely a board worth reflashing.
        log("==> Board did not answer AIUS ID (%s); continuing" % error)

    route = link.enter_bootloader()
    log("==> Entered HalfKay via %s; loader is waiting for %s"
        % (route, HALFKAY_USB_ID))
    run_loader(loader, hex_path, runner=runner)
    running = verify_identity(link, expected)
    log("==> Verified %s built %s"
        % (running["build_revision"], running["build_utc"]))
    return running


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def default_loader():
    return os.environ.get(
        "TEENSY_LOADER",
        os.path.expanduser("~/.platformio/packages/tool-teensy/teensy_loader_cli"))


def build_parser():
    parser = argparse.ArgumentParser(
        description="Aius CDC control and button-free flashing.")
    parser.add_argument("--port", default=None,
                        help="CDC port (default: the unique %s)" % DEFAULT_PORT_GLOB)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("id", help="print the AIUS ID fields")
    sub.add_parser("auth", help="print the SD maintenance token")
    sub.add_parser("bootloader", help="enter HalfKay and exit")
    identity = sub.add_parser("identity", help="print a .hex image's stamped identity")
    identity.add_argument("hex")
    flash_parser = sub.add_parser("flash", help="flash a .hex and verify it")
    flash_parser.add_argument("hex")
    flash_parser.add_argument("--loader", default=None)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.action == "identity":
            found = image_identity(args.hex)
            print("%s %s" % (found["build_revision"], found["build_utc"]))
            return 0

        link = CdcLink(port=args.port)
        if args.action == "id":
            for key, value in link.identity().items():
                print("%s=%s" % (key, value))
        elif args.action == "auth":
            print(link.auth())
        elif args.action == "bootloader":
            print("entered HalfKay via %s" % link.enter_bootloader())
        elif args.action == "flash":
            flash(args.hex, link, args.loader or default_loader())
    except CdcError as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
