---
title: Aius functional topology
type: reference
scope: component
component: aius
status: current
tags: [aius, hardware, topology, signal-path]
updated: 2026-08-04
toc: true
---

# Aius functional topology

The operating modes, blocks, data/control/power flows, clock domains, DMA
buffers, and latency boundaries of the Aius carrier. Companion to
[interfaces and states](interfaces-and-states.md) and the
[hardware spec](../plans/aius-hardware-spec.md). Block, flow and payload ids are
stable contract handles; where the shipped firmware has outgrown a name (the
`dts-decoder` block now hosts AC-3 and E-AC-3 as well; the `pcm-stereo-48k`
payload follows the accepted 32/44.1/48 kHz carrier live), the id is retained
and the notes column carries the current meaning.
[`signal-path.md`](signal-path.md) and [`firmware-io.md`](firmware-io.md) are
the firmware-level authorities for how these blocks are realised on the bench
build.

## Modes

| id | input_formats | output_format | active_blocks | error_mode | evidence_ids | notes |
| --- | --- | --- | --- | --- | --- | --- |
| pcm-processed | pcm-stereo | pcm-stereo-48k | downmix-eq<br>input-classifier<br>optical-rx<br>optical-tx<br>pcm-normalizer<br>raw-bypass-mux<br>spdif-capture<br>spdif-transmit<br>teensy<br>tx-gate | unsupported-muted | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE | Live output follows the accepted 32/44.1/48 kHz carrier (sample-rate following, shipped); the earlier fixed-48 kHz output payload is historical. |
| dts-processed | iec61937-dts-core<br>iec61937-ac3<br>iec61937-eac3 | pcm-stereo-48k | downmix-eq<br>dts-decoder<br>input-classifier<br>optical-rx<br>optical-tx<br>pcm-normalizer<br>raw-bypass-mux<br>spdif-capture<br>spdif-transmit<br>teensy<br>tx-gate | unsupported-muted | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE | Mode id retained; since the AC-3 (2026-07) and E-AC-3 (2026-08) routes it covers all three IEC 61937 families, decoded by one resident decoder at a time. Output rate follows the carrier as in pcm-processed. |
| diagnostic-raw-bypass | pcm-stereo<br>iec61937-dts-core | raw-spdif | optical-rx<br>optical-tx<br>raw-bypass-mux<br>tx-gate | fault-muted | CLIFF-ORJ5<br>CLIFF-OTJ5<br>VKSDR-TOSLINK-SCHEMATICS |  |
| unsupported-muted | none | none | input-classifier<br>optical-rx<br>spdif-capture<br>teensy | unsupported-muted | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| fault-muted | none | none |  | fault-muted | PJRC-IMXRT1060-RM<br>PJRC-TEENSY41-PAGE<br>PJRC-TEENSY41-SCHEMATIC |  |

## Blocks

| id | kind | owner | power_domain | clock_domain | evidence_ids | notes |
| --- | --- | --- | --- | --- | --- | --- |
| power-entry | boundary | external | usb-vbus-raw |  | PJRC-TEENSY41-SCHEMATIC |  |
| usb-bridge | hardware | carrier | protected-5v | usb-device-clock | PJRC-TEENSY41-SCHEMATIC |  |
| teensy | hardware | teensy-hardware | teensy-3v3 | audio-processing | PJRC-IMXRT1060-RM<br>PJRC-TEENSY41-PAGE<br>PJRC-TEENSY41-SCHEMATIC |  |
| optical-rx | hardware | carrier | teensy-3v3 | spdif-recovered | CLIFF-ORJ5<br>PJRC-SPDIF-RX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| spdif-capture | firmware | teensy-firmware | teensy-3v3 | spdif-recovered | CLIFF-ORJ5<br>PJRC-SPDIF-RX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| input-classifier | software | teensy-firmware | teensy-3v3 | audio-processing | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| dts-decoder | software | teensy-firmware | teensy-3v3 | audio-processing | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE | Id retained; the resident compressed decoder — DTS Core, AC-3 or E-AC-3, exactly one at a time in a fixed codec slot. |
| pcm-normalizer | software | teensy-firmware | teensy-3v3 | audio-processing | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| downmix-eq | software | teensy-firmware | teensy-3v3 | audio-processing | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| spdif-transmit | firmware | teensy-firmware | teensy-3v3 | spdif-transmit-clock | CLIFF-OTJ5<br>PJRC-SPDIF-TX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| raw-bypass-mux | hardware | carrier | teensy-3v3 |  | CLIFF-ORJ5<br>CLIFF-OTJ5<br>VKSDR-TOSLINK-SCHEMATICS |  |
| tx-gate | hardware | carrier | teensy-3v3 |  | CLIFF-ORJ5<br>CLIFF-OTJ5<br>VKSDR-TOSLINK-SCHEMATICS |  |
| optical-tx | hardware | carrier | teensy-3v3 | spdif-transmit-clock | CLIFF-OTJ5<br>PJRC-SPDIF-TX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| ethernet | hardware | teensy-hardware | teensy-3v3 | ethernet-rmii-clock | PJRC-ETHERNET-KIT<br>PJRC-IMXRT1060-RM |  |
| expansion | boundary | external | expansion-3v3 |  | PJRC-IMXRT1060-RM<br>PJRC-TEENSY41-PINOUT |  |

## Data flows

| id | kind | source_id | sink_id | payload | enabled_modes | control_id | evidence_ids | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| optical-to-capture | data | optical-rx | spdif-capture | logic-spdif | pcm-processed<br>dts-processed<br>unsupported-muted |  | CLIFF-ORJ5<br>PJRC-SPDIF-RX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| capture-to-classifier | data | spdif-capture | input-classifier | logic-spdif | pcm-processed<br>dts-processed<br>unsupported-muted |  | CLIFF-ORJ5<br>PJRC-SPDIF-RX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| classifier-to-dts | data | input-classifier | dts-decoder | iec61937-dts-core | dts-processed |  | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE | Payload id retained; carries AC-3 (Pc 1) and E-AC-3 (Pc 21) bursts as well since the codec routes shipped. |
| classifier-to-pcm | data | input-classifier | pcm-normalizer | pcm-native | pcm-processed |  | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| dts-to-normalizer | data | dts-decoder | pcm-normalizer | pcm-native | dts-processed |  | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| pcm-to-normalizer | data | input-classifier | pcm-normalizer | pcm-native | pcm-processed |  | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| normalizer-to-dsp | data | pcm-normalizer | downmix-eq | pcm-stereo-48k | pcm-processed<br>dts-processed |  | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| dsp-to-transmit | data | downmix-eq | spdif-transmit | pcm-stereo-48k | pcm-processed<br>dts-processed |  | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| transmit-to-mux | data | spdif-transmit | raw-bypass-mux | logic-spdif | pcm-processed<br>dts-processed | spdif-path-select | CLIFF-OTJ5<br>PJRC-SPDIF-TX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| optical-to-raw-mux | data | optical-rx | raw-bypass-mux | logic-spdif | diagnostic-raw-bypass | spdif-path-select | CLIFF-ORJ5<br>CLIFF-OTJ5<br>VKSDR-TOSLINK-SCHEMATICS |  |
| mux-to-gate | data | raw-bypass-mux | tx-gate | logic-spdif | pcm-processed<br>dts-processed<br>diagnostic-raw-bypass | optical-tx-enable | CLIFF-ORJ5<br>CLIFF-OTJ5<br>VKSDR-TOSLINK-SCHEMATICS |  |
| gate-to-optical | data | tx-gate | optical-tx | optical-spdif | pcm-processed<br>dts-processed<br>diagnostic-raw-bypass | optical-tx-enable | CLIFF-OTJ5<br>PJRC-SPDIF-TX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |

## Control flows

| id | kind | source_id | sink_id | payload | enabled_modes | control_id | evidence_ids | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| path-select-control | control | teensy | raw-bypass-mux | path-select | pcm-processed<br>dts-processed<br>diagnostic-raw-bypass<br>unsupported-muted<br>fault-muted | spdif-path-select | CLIFF-ORJ5<br>CLIFF-OTJ5<br>VKSDR-TOSLINK-SCHEMATICS |  |
| tx-enable-control | control | teensy | tx-gate | tx-enable | pcm-processed<br>dts-processed<br>diagnostic-raw-bypass<br>unsupported-muted<br>fault-muted | optical-tx-enable | CLIFF-ORJ5<br>CLIFF-OTJ5<br>VKSDR-TOSLINK-SCHEMATICS |  |
| watchdog-reset-control | control | teensy | teensy | reset | pcm-processed<br>dts-processed<br>diagnostic-raw-bypass<br>unsupported-muted<br>fault-muted | teensy-watchdog | PJRC-IMXRT1060-RM<br>PJRC-TEENSY41-PAGE<br>PJRC-TEENSY41-SCHEMATIC |  |

## Power flows

| id | kind | source_id | sink_id | payload | enabled_modes | control_id | evidence_ids | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| usb-vbus-entry | power | power-entry | usb-bridge | usb-vbus |  |  | PJRC-TEENSY41-SCHEMATIC |  |
| protected-vbus | power | usb-bridge | teensy | protected-5v |  |  | PJRC-TEENSY41-SCHEMATIC |  |
| teensy-power | power | teensy | optical-rx | teensy-3v3 |  |  | PJRC-IMXRT1060-RM<br>PJRC-TEENSY41-PAGE<br>PJRC-TEENSY41-SCHEMATIC |  |
| carrier-3v3 | power | teensy | ethernet | teensy-3v3 |  |  | PJRC-IMXRT1060-RM<br>PJRC-TEENSY41-PAGE<br>PJRC-TEENSY41-SCHEMATIC |  |
| expansion-power | power | teensy | expansion | expansion-3v3 |  |  | PJRC-IMXRT1060-RM<br>PJRC-TEENSY41-PINOUT |  |

## Clock domains

| id | source_id | consumer_ids | synchronous_with | frequency_status | evidence_ids | notes |
| --- | --- | --- | --- | --- | --- | --- |
| spdif-recovered | optical-rx | input-classifier<br>spdif-capture |  | evidence-locked | CLIFF-ORJ5<br>PJRC-SPDIF-RX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS |  |
| audio-processing | teensy | downmix-eq<br>dts-decoder<br>input-classifier<br>pcm-normalizer<br>teensy |  | software-derived | PJRC-IMXRT1060-RM<br>PJRC-SPDIF-RX-SOURCE<br>PJRC-SPDIF-TX-SOURCE |  |
| spdif-transmit-clock | spdif-transmit | optical-tx<br>spdif-transmit | audio-processing | software-derived | CLIFF-OTJ5<br>PJRC-SPDIF-TX-SOURCE<br>VKSDR-TOSLINK-SCHEMATICS | PLL4 follows the accepted carrier nominal (32/44.1/48 kHz) including its oscillator error; exactly 48 kHz for boot. See [`signal-path.md` §The clock](signal-path.md#the-clock). |
| usb-device-clock | usb-bridge | teensy<br>usb-bridge |  | evidence-locked | PJRC-TEENSY41-SCHEMATIC |  |
| ethernet-rmii-clock | ethernet | ethernet<br>teensy |  | evidence-locked | PJRC-ETHERNET-KIT<br>PJRC-IMXRT1060-RM |  |

## Buffers and DMA ownership

| id | owner | producer | consumer | storage_class | overflow_policy | measurement_gate | notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| spdif-rx-dma | spdif-capture | spdif-capture | input-classifier | dma-ring | drop-and-mute | P3 |  |
| compressed-burst-ring | input-classifier | input-classifier | dts-decoder | ring-buffer | reset-pipeline-and-mute | P6 |  |
| decoded-pcm-pool | dts-decoder | dts-decoder | pcm-normalizer | block-pool | backpressure | P6 |  |
| dsp-output-ring | downmix-eq | downmix-eq | spdif-transmit | ring-buffer | drop-and-mute | P7 |  |
| spdif-tx-dma | spdif-transmit | spdif-transmit | raw-bypass-mux | dma-ring | reset-pipeline-and-mute | P7 |  |

These are contract-level abstractions. The shipped firmware realises them as the
8,192-word ingress SPSC ring, the DTCM decoder arena, the 24-bit `g_aius_spdif_tx24`
transmit ring and the PSRAM flight-recorder ring — sizes,
banks and ownership are catalogued in [`firmware-io.md`](firmware-io.md) and
[`signal-path.md`](signal-path.md), which are the authorities for the bench build.

## Latency boundaries

| id | source | sink | budget_status | measurement_gate | notes |
| --- | --- | --- | --- | --- | --- |
| capture | optical-rx | spdif-capture | deferred | P6 |  |
| burst-assembly | spdif-capture | input-classifier | deferred | P6 |  |
| decode | dts-decoder | pcm-normalizer | deferred | P6 |  |
| dsp | pcm-normalizer | downmix-eq | deferred | P7 |  |
| transmit-queue | downmix-eq | spdif-transmit | deferred | P7 |  |

## Evidence

| evidence_id |
| --- |
| CLIFF-ORJ5 |
| CLIFF-OTJ5 |
| PJRC-ETHERNET-KIT |
| PJRC-IMXRT1060-RM |
| PJRC-SPDIF-RX-SOURCE |
| PJRC-SPDIF-TX-SOURCE |
| PJRC-TEENSY41-PAGE |
| PJRC-TEENSY41-PINOUT |
| PJRC-TEENSY41-SCHEMATIC |
| VKSDR-TOSLINK-SCHEMATICS |
