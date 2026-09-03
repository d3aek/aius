# Gate-1 trim-cadence image A/B — 2026-09-03 evidence

The two `aius_rate_bench.py run 48k-10m` evidence files the completion plan's
Phase 3 A/B was judged on, kept because decision D4 (the gate-1 trim bound)
is settled on them and session scratch is GC'd after 7 days. Both are the
tool's JSONL, gzip-compressed, LFS-tracked (`*.jsonl.gz`). Decompress with
`gunzip -k` and re-analyze with
`python3 firmware/tools/net/aius_rate_bench.py analyze 48k-10m --evidence <file>`.

| File | Image | Post-settle trims | Uncompressed sha256 |
|---|---|---|---|
| `gate1-legA-25109fca3f97-ebc7a4c9.jsonl.gz` | `25109fca3f97+teensy41-optical-slot-a` (deployed, A/B chooser) | 53 | `ebc7a4c9ac3e915a0bf9944485dbbb90c4217586513a949ce60a7b17bcda67e7` |
| `gate1-legB-405ce9768532-451c2748.jsonl.gz` | `405ce9768532+teensy41-optical` (nearest committed tree to the 2026-08-02 observation) | 52 | `451c2748d22400e6e081b0a5032c07ee0508628d7e8a8dbb458217bf4c6a3f80` |
| `gate1-92aae16706f5-7b738639.jsonl.gz` | `92aae16706f5+teensy41-optical-slot-a` (Phase 4 acceptance image; every other check green) | 53 | `7b738639e224fcd9fc103b5151dbd2f5f10456ec7351d72b83edd923ab600422` |

Same source (ESI U24 XL, free-running 48 kHz, mixer at unity), same day,
same bench. Record and reading:
[`../../../../docs/plans/aius-sample-rate-following.md`](../../../../docs/plans/aius-sample-rate-following.md)
§Device evidence — 2026-09-03.
