#ifndef AIUS_OTA_STAGING_H
#define AIUS_OTA_STAGING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The activation staging placement: WHERE in PSRAM the pre-window SD copy of
 * the image lands, as pure geometry the host suite can pin per board fitment.
 *
 * Scoped by aius-ota-resilience.md Phase 2, after the 2026-08-16 dark-window
 * brick: the old placement put staging ABOVE the recorder ring, which on a
 * 16 MiB board is entirely the pad-2 chip -- the one the board-level
 * investigation flagged as marginal. One marginal in-window readback in
 * 1.58 MB fails the byte-compare, and the halt policy turns that transient
 * read into a brick. The new placement claims the PAD-1 region the ring
 * normally owns: the ring's content is expendable by definition once an
 * activation is acknowledged, the recorder is PARKED (aius_recorder_park())
 * before the copy begins, and every in-window read then comes from the
 * known-good chip. The pad-2 straddle risk becomes activation-irrelevant.
 *
 * GEOMETRY, matching the composition's begin_pipeline(): pad 1 is the first
 * AIUS_OTA_STAGING_PAD_BYTES of the core-mapped PSRAM at 0x70000000
 * (external_psram_size covers BOTH pads when both are fitted); the first
 * `recorder_staging_bytes` of it are the recorder's producer slots, which the
 * placement skips -- not because they are live (the park stops them too) but
 * because the max image fits without them and disjointness needs no proof.
 * A board with no pad-1 chip but a probed pad-2 chip (the bench fitment)
 * stages on pad 2 at its XIP window because that is the only storage there
 * is; `on_pad2` reports it so ota_diag can say which chip the window read. */
#define AIUS_OTA_STAGING_PAD_BYTES UINT32_C(0x00800000) /* 8 MiB per pad */
/* Pad 2's XIP window base offset from 0x70000000 when pad 1 is empty:
 * chip-select A1 keeps its 64 MiB reset default, so A2 begins 64 MiB in.
 * Same derivation as the composition's AIUS_PSRAM_PAD2_PROBE_BASE. */
#define AIUS_OTA_STAGING_PAD2_OFFSET UINT32_C(0x04000000)

typedef struct {
	uint32_t offset;   /* from the FlexSPI2 base (0x70000000) */
	uint32_t capacity; /* bytes available for the staged image */
	bool on_pad2;      /* true only for the pad-2-only fitment */
} AiusOtaStagingPlacement;

/* Resolves the placement for a fitment. `mapped_psram_bytes` is the
 * core-mapped total (external_psram_size in bytes: 0, 8 MiB or 16 MiB);
 * `pad2_only_bytes` is the firmware's own pad-2 probe result, consulted only
 * when the core mapped nothing. False when no PSRAM is fitted anywhere or
 * the carve leaves no capacity -- the ota_no_psram refusal. */
bool aius_ota_staging_place(uint32_t mapped_psram_bytes,
	                    uint32_t pad2_only_bytes,
	                    uint32_t recorder_staging_bytes,
	                    AiusOtaStagingPlacement *placement);

#ifdef __cplusplus
}
#endif

#endif
