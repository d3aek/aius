# P6 RawHID validation

- Target: Teensy 4.1, `teensy41-capability`
- Transport: PCI-passed Renesas `0000:18:00.0` USB controller
- Flash path: software RawHID reset to HalfKay, `teensy_loader_cli`, automatic RawHID re-enumeration
- Firmware image: 165,888 bytes; all memory banks fit
- Corpus: 40 DTS Core access units, 40 passed, 0 failed
- Allocation violations: 0; maximum observed arena high-water: 61,520 bytes
- Raw transcript: `p6-hidraw-validated.audit`

Large RawHID commands use acknowledged 32-byte fragments, because longer logical
HID reports were observed to corrupt.
