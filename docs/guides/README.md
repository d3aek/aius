# Aius guides

Task how-tos for working on the Aius bench build. Design and delivery documents
live in [`../plans/`](../plans/); current-state descriptions of what exists live
in [`../reference/`](../reference/).

- **[Proto-board optical wiring — first real signal test](proto-optical-wiring.md)** —
  how to wire the RS PRO ORJ-5/OTJ-5 optical jacks directly to a Teensy 4.1 on a
  breadboard or proto board, the pinouts (the two jacks differ), the bypass parts
  and their tolerances, and what to observe for a real S/PDIF signal. This is the
  bench shape only; the carrier board adds a bypass mux and a reset-safe TX gate
  the bench omits.
