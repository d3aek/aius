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

`measure_run.py` plays them through the U24 with `aplay` (no upload, no
replay): see below.

**These vectors reach the tap at their own level.** PCM pass-through is a stereo
FL|FR layout, which sums nothing, so the downmix headroom pad does not apply to
it ([`signal-path.md` §Channel layout and downmix rules](../../../docs/reference/signal-path.md#channel-layout-and-downmix-rules)):
a −6 dBFS vector measures at −6 dBFS and `pcm440_full` engages the limiter at
the shipped config. Before the layout-aware pad landed (2026-07-28) every
figure sat 6.02 dB lower and `pcm440_full` needed `headroom_linear` raised to
1.0 to reach the −1 dBFS threshold at all; recorded measurements from before
that date read 6.02 dB quiet for that reason and not because anything drifted.

`measure_run.py` is the committed driver for all of it against a live board
(`AIUS_DEVICE`, `AIUS_SD_AUTH`, `AIUS_ALSA_DEVICE` default `hw:U24XL,0`).
Arming, for `measure` and `isr-cost` alike: set the U24 playback mixer to unity
(`amixer -c 0 cset numid=5 110,110`) and read it back, start one continuous
`aplay` stream fed the vector's PCM repeated (never an `aplay` re-run per
loop), wait for `/api/status` `tx_gate.open` and `clock.state` `tracking`, then
`POST /api/measure/start`; the render stops when the run ends or fails.
`measure <id> [f0_l f0_r [probes…]]` runs a session end to end and prints
`GET /api/measure`, `suite` runs every vector with its manifest arguments and
prints `{id: session}` (redirect it to record a baseline), and `isr-cost` runs
the tap's armed-vs-not `dsp_cycles_max` acceptance with its ±30 k gate.

`--baseline FILE` (a recorded `suite` output) compares a run against it. A
vector passes when `fail_reason` is `none`, each channel's `thd_n_db`,
`snr_db` and `thd_db` sit within 1.0 dB of the baseline's (a figure at its
floor in both runs counts as equal; the `floors` objects must match) and each
`pcm_fr_multitone` `probes_dbfs` entry is within 0.1 dB; `suite --baseline`
also requires `isr-cost` PASS. The comparison is owner bench work, not a
test gate. The measured
figures live in
[`signal-path.md` §Measured output quality](../../../docs/reference/signal-path.md).
