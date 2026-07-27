# PCM measurement vectors

The stimulus corpus for the measurement loopback
([`aius-dsp-config-api.md` §Item 3 design](../../../docs/plans/aius-dsp-config-api.md)):
bare 16-bit IEC words with no IEC 61937 preamble, so the pipeline classifies
them `AIUS_PIPELINE_PCM` and passes the samples through bit-exactly at 24-bit
scale. They replace the DTS vectors as measurement stimulus — the FFmpeg `dca`
encode carries a −78.7 dB THD+N floor of its own, decodes at −22.5 dBFS, and
loops with a phase seam; these are seam-free by construction (loop-coherent,
so the replayed stream is a continuous tone).

- `measure_vectors.py generate` — rewrite `vectors/` and `manifest.txt`. The
  two generation-time obligations (no coincidental Pa/Pb preamble pair in
  either byte order; integer cycles for every tone over the file — per
  *vector*, so L 440 / R 660 needs lcm(1200, 800) = 2,400) are asserted before
  any byte is written.
- `measure_vectors.py validate` — re-assert the obligations plus tone
  content and residual against the committed bytes. Run by
  `scripts/test.sh --suite measure`.

The committed bytes are the provenance record: `sin()` is libm's, so
regeneration on another host is not guaranteed byte-identical, and validation
therefore checks obligations, not byte equality.

Upload to the board as `PUT /sd/replays/<name>` (they land at
`/aius/replays/<name>.iec`) and start with `POST /api/replays/start`. The
`pcm440_full` vector needs a config that opens the limiter — at the shipped
0.5 headroom the tap sees −6 dBFS against the −1 dBFS threshold.

`measure_run.py` is the committed driver for all of it against a live board
(`AIUS_DEVICE`, `AIUS_SD_AUTH`): `upload` chunks the corpus through
`PUT /sd/replays/<id>`, `measure <id> [f0_l f0_r [probes…]]` runs a session
end to end and prints `GET /api/measure`, and `isr-cost` runs the tap's
armed-vs-not `dsp_cycles_max` acceptance with its ±30 k gate. The measured
figures live in
[`signal-path.md` §Measured output quality](../../../docs/reference/signal-path.md).
