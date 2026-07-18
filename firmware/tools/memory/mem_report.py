#!/usr/bin/env python3
"""Per-bank memory report for the Aius Teensy 4.1 capability firmware (gate P4).

Parses the authoritative `teensy_size:` summary that the Teensy platform prints
when it links `firmware.elf`, and re-projects it into a per-bank table:

    FLASH  — program image (0x60000000, 7936 KiB on the Teensy 4.1)
    ITCM   — instruction tightly-coupled RAM (RAM1 code block, 32 KiB-granular)
    DTCM   — data tightly-coupled RAM (RAM1 variables + the stack/local free zone)
    OCRAM  — RAM2 (0x20200000, 512 KiB; DMAMEM + malloc/new heap)

ITCM and DTCM share the 512 KiB FlexRAM: Teensyduino rounds the ITCM code block up
to a 32 KiB boundary and gives the remainder to DTCM. The `teensy_size` `padding`
field is exactly that ITCM rounding, so the split is exact rather than assumed.

The decoder's statically-reserved arena is uninitialised `.bss` and therefore lives
in DTCM; `--arena-bytes` annotates how much of the DTCM `variables` figure it is.

Usage:
    mem_report.py --size-log <pio-build-log> [--arena-bytes N]
    mem_report.py --size-log - < build.log        # read log from stdin

Exit codes:
    0  every bank fits
    2  a bank is over capacity (reported loudly)
    3  the teensy_size summary was not found in the input
    4  usage error
"""
import argparse
import re
import sys

FLEXRAM_TOTAL = 512 * 1024  # ITCM + DTCM share this 512 KiB block.


def _grab(pattern, text, label):
    m = re.search(pattern, text)
    if not m:
        raise SystemExit(f"3:could not find {label} in teensy_size output")
    return int(m.group(1))


def parse_teensy_size(text):
    """Extract the three teensy_size lines into a flat dict of byte counts."""
    if "teensy_size:" not in text:
        raise SystemExit("3:no 'teensy_size:' summary in the build output")
    return {
        "flash_code": _grab(r"FLASH:\s*code:(\d+)", text, "FLASH code"),
        "flash_data": _grab(r"FLASH:.*?data:(\d+)", text, "FLASH data"),
        "flash_headers": _grab(r"FLASH:.*?headers:(\d+)", text, "FLASH headers"),
        "flash_free": _grab(r"free for files:(\d+)", text, "FLASH free"),
        "ram1_vars": _grab(r"RAM1:\s*variables:(\d+)", text, "RAM1 variables"),
        "ram1_code": _grab(r"RAM1:.*?code:(\d+)", text, "RAM1 code"),
        "ram1_padding": _grab(r"RAM1:.*?padding:(\d+)", text, "RAM1 padding"),
        "ram1_free": _grab(r"free for local variables:(\d+)", text, "RAM1 free"),
        "ram2_vars": _grab(r"RAM2:\s*variables:(\d+)", text, "RAM2 variables"),
        "ram2_free": _grab(r"free for malloc/new:(\d+)", text, "RAM2 free"),
    }


class Bank:
    def __init__(self, name, used, capacity, note=""):
        self.name = name
        self.used = used
        self.capacity = capacity
        self.note = note

    @property
    def free(self):
        return self.capacity - self.used

    @property
    def pct(self):
        return 100.0 * self.used / self.capacity if self.capacity else 0.0

    @property
    def over(self):
        return self.used > self.capacity


def banks_from(v):
    """Project the teensy_size fields onto the four physical banks."""
    flash_used = v["flash_code"] + v["flash_data"] + v["flash_headers"]
    flash_cap = flash_used + v["flash_free"]

    # ITCM: the code block, rounded up to a 32 KiB FlexRAM boundary. `padding` is
    # exactly that rounding, so the block (= capacity dedicated to ITCM) is
    # code + padding, and the padding is dead space inside the bank.
    itcm_used = v["ram1_code"]
    itcm_cap = v["ram1_code"] + v["ram1_padding"]

    # DTCM: whatever FlexRAM the ITCM block did not claim. `variables` is the
    # static allocation (incl. the decoder arena); the free zone is where the
    # stack and locals live (stack grows down from the top of DTCM).
    dtcm_cap = FLEXRAM_TOTAL - itcm_cap
    dtcm_used = v["ram1_vars"]

    ocram_used = v["ram2_vars"]
    ocram_cap = v["ram2_vars"] + v["ram2_free"]

    return [
        Bank("FLASH", flash_used, flash_cap,
             f"code {v['flash_code']} + data {v['flash_data']} + headers {v['flash_headers']}"),
        Bank("ITCM", itcm_used, itcm_cap,
             f"fast code; {v['ram1_padding']}B dead padding to the 32 KiB block"),
        Bank("DTCM", dtcm_used, dtcm_cap,
             f"static vars; {v['ram1_free']}B free zone holds the stack + locals"),
        Bank("OCRAM", ocram_used, ocram_cap,
             "RAM2: DMAMEM + malloc/new heap"),
    ]


def kib(n):
    return f"{n / 1024.0:.1f}K"


def flexram_rollup(banks):
    """The physically meaningful RAM1 view: ITCM + DTCM share one 512 KiB block,
    repartitioned at build time, so the elastic headroom is the shared free zone."""
    itcm = next(b for b in banks if b.name == "ITCM")
    dtcm = next(b for b in banks if b.name == "DTCM")
    used = itcm.capacity + dtcm.used  # ITCM block committed + DTCM static vars.
    return Bank("FlexRAM", used, FLEXRAM_TOTAL,
                "RAM1 = ITCM+DTCM; free zone is the shared stack/local budget")


def render(banks, arena_bytes):
    lines = []
    lines.append("==> Aius Teensy 4.1 per-bank memory report")
    header = f"  {'BANK':<8} {'USED':>10} {'FREE':>12} {'CAPACITY':>12}  {'USED%':>7}  {'FREE%':>6}"
    lines.append(header)
    lines.append("  " + "-" * (len(header) - 2))
    worst_over = False
    rows = list(banks)
    rows.insert(3, flexram_rollup(banks))  # FlexRAM roll-up just before OCRAM.
    for b in rows:
        flag = "  <<< OVER CAPACITY" if b.over else ""
        if b.over:
            worst_over = True
        indent = "  " if b.name != "FlexRAM" else "  "
        lines.append(
            f"{indent}{b.name:<8} {b.used:>10} {b.free:>12} {b.capacity:>12}  {b.pct:>6.2f}%"
            f"  {100.0 - b.pct:>5.1f}%{flag}"
        )
        lines.append(f"             {kib(b.used)} used / {kib(b.capacity)} — {b.note}")
    if arena_bytes is not None:
        dtcm = next(b for b in banks if b.name == "DTCM")
        share = 100.0 * arena_bytes / dtcm.capacity
        lines.append(
            f"  note: DTS decoder static arena = {arena_bytes}B "
            f"({kib(arena_bytes)}), {share:.1f}% of DTCM, inside the DTCM 'static vars' figure"
        )
    lines.append("  note: ITCM and DTCM split the 512 KiB FlexRAM elastically; the "
                 "32 KiB-granular ITCM block")
    lines.append("        rounding is why ITCM shows padding. The FlexRAM row is the "
                 "true RAM1 headroom.")
    return "\n".join(lines), worst_over


def main(argv):
    ap = argparse.ArgumentParser(add_help=True, description=__doc__)
    ap.add_argument("--size-log", required=True,
                    help="file with the pio/teensy_size build output, or '-' for stdin")
    ap.add_argument("--arena-bytes", type=int, default=None,
                    help="decoder static arena size, for the DTCM annotation")
    try:
        args = ap.parse_args(argv[1:])
    except SystemExit:
        return 4

    if args.size_log == "-":
        text = sys.stdin.read()
    else:
        with open(args.size_log, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()

    try:
        fields = parse_teensy_size(text)
    except SystemExit as e:
        msg = str(e)
        if msg.startswith("3:"):
            print("ERROR: " + msg[2:], file=sys.stderr)
            return 3
        raise

    banks = banks_from(fields)
    report, over = render(banks, args.arena_bytes)
    print(report)
    if over:
        print("ERROR: a memory bank is over capacity — firmware does not fit",
              file=sys.stderr)
        return 2
    print("==> All banks fit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
