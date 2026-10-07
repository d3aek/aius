# Rollback candidate: `df966f17b370`

**The fail-safe image to return to if the Phase 4b rework misbehaves**
(owner, 2026-10-07). It is the sample-rate following Phase 4a build
(aius commit `df966f1`, git tag `fw-rollback-df966f17b370`), deployed by
OTA on 2026-10-06 and run in the listening-room:

- 2026-10-06 short AC-3 session and 2026-10-07 full session (48 min
  polled, 12 min unpolled): every fill-loop criterion passed, no underrun
  except two caused by a web UI load (the defect Phase 4b fixes);
- healthy-boot self-commit in slot 1, `rollback_observed` false.

Records: `docs/plans/aius-sample-rate-following.md` Phase 4 §Device
evidence 2026-10-06 and 2026-10-07.

## Files

| File | sha256 | Use |
| --- | --- | --- |
| `aius-df966f17b370.ota` | `bb3e090a8e8b74fbbe0600aa08c3eb9508bd24f51763d633bcf8e46ac600f4ec` | Signed OTA container (home key, no `--slot`, canonical slot-agnostic image, 587,008 B) |
| `recovery-df966f17b370.hex` | `795e50b52a143f967bbae8ee2cfb4320aac47e90c505d189633a510e3a73047c` | USB HalfKay recovery (A/B chooser plus this canonical image), from the same `build_ab.sh` run |

## Rolling back

Check `sha256sum` against the table first.

- **OTA (normal path).** Read `/api/status` `ab` (it must be the
  slot-agnostic chooser, `ledger_valid true`, `booted_slot` 1 or 2), then
  from the repository root:

  ```bash
  python3 firmware/tools/ota/aius_ota_flash.py --device http://192.0.2.66 --auth-file .secrets/aius-sd-auth --activate firmware/releases/df966f17b370/aius-df966f17b370.ota
  ```

  Confirm `build_revision` reads `df966f17b370` and the healthy-boot
  self-commit (`trial_budget` 0, `rollback_observed` false).
- **USB recovery (OTA broken).** With the board on USB:

  ```bash
  firmware/scripts/flash_optical.sh firmware/releases/df966f17b370/recovery-df966f17b370.hex
  ```

  It reboots the board and stops all audio.

The SD card's web UI and config must match the firmware: if a later
change altered `firmware/sd/www/` or the card's config format, sync the
card from tag `fw-rollback-df966f17b370` as well.
