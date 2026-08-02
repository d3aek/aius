#!/usr/bin/env python3
"""Boot-lottery campaign driver: reboot, sample, classify, repeat.

The boot-lottery defect (aius-open-work-register.md, open defects / runtime)
degrades some boots of a byte-identical image while others are healthy. This
tool turns "boots deterministically healthy" into a measured claim: it warm
reboots the board N times over the button-free CDC route, samples GET
/api/status twice per boot, computes the cross-domain audio rates, decodes
the clk_regs clock-tree dump, and writes one JSONL record per boot plus a
verdict summary.

Usage:
  tools/boot_lottery/boot_campaign.py --ip 192.0.2.66 --boots 20 \
      --out "$CLAUDE_SESSION_SCRATCH/boot-campaign"
  tools/boot_lottery/boot_campaign.py --ip 192.0.2.66 --boots 1 --no-reboot \
      --out /tmp/one-shot   # sample the CURRENT boot without rebooting

Env:
  TEENSY_LOADER  teensy_loader_cli path
                 (default: ~/.platformio/packages/tool-teensy/teensy_loader_cli)

Exit codes: 0 all sampled boots healthy; 2 at least one degraded boot
            (the record names it); 3 a boot never came back / tooling failure.

Classification (healthy requires ALL):
  - audio update rate within 2% of the 375/s block rate (48 kHz / 128)
  - tx fill rate within 2% of the same 375/s
  - PLL4 DIV_SELECT 28, ENABLE set, BYPASS/POWERDOWN clear
  - CDCDR spdif0 mux on PLL4 with PRED 7 / PODF 0
  - STC TXCLK_SOURCE 1 with TXCLK_DF 27
The register checks are the conviction half: a degraded boot with nominal
registers acquits the clock tree; wrong registers convict it and name the
field.
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CDC_RUNNER = os.path.join(HERE, "..", "net", "aius_cdc_runner.py")

NOMINAL_UPDATE_HZ = 48000.0 / 128.0  # 375.0: audio block rate at 48 kHz
RATE_TOLERANCE = 0.02

# CCM_ANALOG_PLL_AUDIO fields (i.MX RT1062).
PLL_DIV_SELECT_MASK = 0x7F
PLL_POWERDOWN = 1 << 12
PLL_ENABLE = 1 << 13
PLL_BYPASS = 1 << 16
PLL_LOCK = 1 << 31

# CCM_CDCDR spdif0 fields.
CDCDR_SEL_SHIFT, CDCDR_SEL_MASK = 20, 0x3
CDCDR_PODF_SHIFT, CDCDR_PODF_MASK = 22, 0x7
CDCDR_PRED_SHIFT, CDCDR_PRED_MASK = 25, 0x7

# SPDIF_STC fields.
STC_TXCLK_DF_MASK = 0x7F
STC_TXCLK_SOURCE_SHIFT, STC_TXCLK_SOURCE_MASK = 8, 0x7

# SPDIF_SRPC fields.
SRPC_GAINSEL_SHIFT, SRPC_GAINSEL_MASK = 3, 0x7
SRPC_LOCK = 1 << 6
SRPC_CLKSRC_SHIFT, SRPC_CLKSRC_MASK = 7, 0xF


def run_cdc(action, port=None, timeout=30):
    command = [sys.executable, CDC_RUNNER]
    if port:
        command += ["--port", port]
    command.append(action)
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=timeout)


def loader_boot(loader):
    return subprocess.run([loader, "--mcu=imxrt1062", "-b"],
                          capture_output=True, text=True, timeout=60)


def fetch_status(ip, timeout=10):
    with urllib.request.urlopen("http://%s/api/status" % ip,
                                timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def wait_for_board(ip, port, deadline_s):
    """CDC first (it survives degraded boots), then HTTP."""
    start = time.monotonic()
    cdc_ok = False
    while time.monotonic() - start < deadline_s:
        if not cdc_ok:
            probe = run_cdc("id", port=port)
            cdc_ok = probe.returncode == 0
        if cdc_ok:
            try:
                fetch_status(ip, timeout=5)
                return True
            except OSError:
                pass
        time.sleep(2)
    return cdc_ok  # HTTP may be collapsed on a degraded boot; CDC is enough


def decode_regs(clk_regs):
    def word(name):
        return int(clk_regs[name], 16)

    pll = word("pll_audio")
    cdcdr = word("cdcdr")
    stc = word("spdif_stc")
    srpc = word("spdif_srpc")
    return {
        "pll_div_select": pll & PLL_DIV_SELECT_MASK,
        "pll_enable": bool(pll & PLL_ENABLE),
        "pll_bypass": bool(pll & PLL_BYPASS),
        "pll_powerdown": bool(pll & PLL_POWERDOWN),
        "pll_lock": bool(pll & PLL_LOCK),
        "pll_num": word("pll_audio_num"),
        "pll_denom": word("pll_audio_denom"),
        "cdcdr_sel": (cdcdr >> CDCDR_SEL_SHIFT) & CDCDR_SEL_MASK,
        "cdcdr_podf": (cdcdr >> CDCDR_PODF_SHIFT) & CDCDR_PODF_MASK,
        "cdcdr_pred": (cdcdr >> CDCDR_PRED_SHIFT) & CDCDR_PRED_MASK,
        "stc_txclk_df": stc & STC_TXCLK_DF_MASK,
        "stc_txclk_source":
            (stc >> STC_TXCLK_SOURCE_SHIFT) & STC_TXCLK_SOURCE_MASK,
        "srpc_gainsel": (srpc >> SRPC_GAINSEL_SHIFT) & SRPC_GAINSEL_MASK,
        "srpc_clksrc": (srpc >> SRPC_CLKSRC_SHIFT) & SRPC_CLKSRC_MASK,
        "srpc_lock": bool(srpc & SRPC_LOCK),
    }


def registers_nominal(decoded):
    faults = []
    if decoded["pll_div_select"] != 28:
        faults.append("pll_div_select=%d" % decoded["pll_div_select"])
    if not decoded["pll_enable"]:
        faults.append("pll_enable=0")
    if decoded["pll_bypass"]:
        faults.append("pll_bypass=1")
    if decoded["pll_powerdown"]:
        faults.append("pll_powerdown=1")
    if decoded["cdcdr_sel"] != 0:
        faults.append("cdcdr_sel=%d" % decoded["cdcdr_sel"])
    if decoded["cdcdr_pred"] != 7:
        faults.append("cdcdr_pred=%d" % decoded["cdcdr_pred"])
    if decoded["cdcdr_podf"] != 0:
        faults.append("cdcdr_podf=%d" % decoded["cdcdr_podf"])
    if decoded["stc_txclk_df"] != 27:
        faults.append("stc_txclk_df=%d" % decoded["stc_txclk_df"])
    if decoded["stc_txclk_source"] != 1:
        faults.append("stc_txclk_source=%d" % decoded["stc_txclk_source"])
    return faults


def rate_window(ip, window_s):
    a_time = time.monotonic()
    a = fetch_status(ip)
    time.sleep(window_s)
    b_time = time.monotonic()
    b = fetch_status(ip)
    dt = b_time - a_time
    return a, b, dt


def sample_boot(ip, window_s):
    a, b, dt = rate_window(ip, window_s)
    update_rate = (b["audio_update_calls"] - a["audio_update_calls"]) / dt
    fills_a = int(a["clk_regs"]["tx_fills"])
    fills_b = int(b["clk_regs"]["tx_fills"])
    fill_rate = (fills_b - fills_a) / dt
    epoch_rate = (b["loop_epoch"] - a["loop_epoch"]) / dt
    gap_rate = (b["audio_diag"]["gap_flushes"] -
                a["audio_diag"]["gap_flushes"]) / dt
    decoded = decode_regs(b["clk_regs"])
    reg_faults = registers_nominal(decoded)

    def near_nominal(rate):
        return abs(rate - NOMINAL_UPDATE_HZ) <= \
            NOMINAL_UPDATE_HZ * RATE_TOLERANCE

    healthy = (near_nominal(update_rate) and near_nominal(fill_rate) and
               not reg_faults)
    return {
        "healthy": healthy,
        "update_rate_hz": round(update_rate, 2),
        "tx_fill_rate_hz": round(fill_rate, 2),
        "epoch_rate_hz": round(epoch_rate, 1),
        "gap_flush_rate_hz": round(gap_rate, 2),
        "register_faults": reg_faults,
        "clk_regs": b["clk_regs"],
        "clk_regs_decoded": decoded,
        "boot_generation": b["boot_generation"],
        "reset_source": b["reset_source"],
        "audio_update_max_us": b["audio_update_max_us"],
        "runtime_epoch_max_us":
            b["audio_diag"]["runtime_epoch_max_us"],
        "carrier_rate_millihz": b["clock"]["carrier_rate_millihz"],
        "clock_master": b["clock"]["master"],
        "window_s": round(dt, 2),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Boot-lottery campaign: reboot, sample, classify.")
    parser.add_argument("--ip", required=True)
    parser.add_argument("--boots", type=int, default=10)
    parser.add_argument("--port", default=None, help="CDC port override")
    parser.add_argument("--out", required=True,
                        help="output directory (JSONL + summary)")
    parser.add_argument("--settle-seconds", type=float, default=12.0,
                        help="wait after boot before sampling")
    parser.add_argument("--sample-seconds", type=float, default=20.0,
                        help="rate window width")
    parser.add_argument("--boot-deadline-seconds", type=float, default=90.0)
    parser.add_argument("--no-reboot", action="store_true",
                        help="sample the current boot only")
    args = parser.parse_args()

    loader = os.environ.get(
        "TEENSY_LOADER",
        os.path.expanduser(
            "~/.platformio/packages/tool-teensy/teensy_loader_cli"))
    os.makedirs(args.out, exist_ok=True)
    records_path = os.path.join(args.out, "boots.jsonl")

    degraded = 0
    lost = 0
    with open(records_path, "a") as records:
        for boot in range(args.boots):
            if not args.no_reboot:
                entered = run_cdc("bootloader", port=args.port)
                if entered.returncode != 0:
                    print("boot %d: bootloader entry failed: %s"
                          % (boot, entered.stderr.strip()))
                    lost += 1
                    break
                booted = loader_boot(loader)
                if booted.returncode != 0:
                    print("boot %d: loader -b failed: %s"
                          % (boot, booted.stderr.strip()))
                    lost += 1
                    break
                if not wait_for_board(args.ip, args.port,
                                      args.boot_deadline_seconds):
                    print("boot %d: board never came back" % boot)
                    lost += 1
                    break
                time.sleep(args.settle_seconds)
            try:
                record = sample_boot(args.ip, args.sample_seconds)
            except (OSError, KeyError, ValueError) as error:
                record = {"healthy": False,
                          "sample_error": str(error)}
            record["boot_index"] = boot
            record["sampled_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                                 time.gmtime())
            records.write(json.dumps(record) + "\n")
            records.flush()
            verdict = "HEALTHY" if record.get("healthy") else "DEGRADED"
            if not record.get("healthy"):
                degraded += 1
            print("boot %d: %s update=%s/s fills=%s/s regs=%s"
                  % (boot, verdict,
                     record.get("update_rate_hz"),
                     record.get("tx_fill_rate_hz"),
                     record.get("register_faults", ["?"])))

    total = args.boots
    print("campaign: %d boots, %d degraded, %d lost -> %s"
          % (total, degraded, lost, records_path))
    if lost:
        return 3
    return 2 if degraded else 0


if __name__ == "__main__":
    raise SystemExit(main())
