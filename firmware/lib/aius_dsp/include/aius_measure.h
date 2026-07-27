#ifndef AIUS_MEASURE_H
#define AIUS_MEASURE_H

#include <stdint.h>

#include "aius_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Aius measurement loopback: the portable core of the software tap
 * (docs/plans/aius-dsp-config-api.md §Item 3 design).
 *
 * Two halves, both host-testable with no Arduino dependency:
 *
 *   1. The CAPTURE STATE MACHINE. The audio ISR copies the same stereo 24-bit
 *      frames it commits to the S/PDIF transmit ring into a caller-owned
 *      capture buffer while a capture is armed; the epoch loop arms, analyses
 *      and retires. One writer per transition (enumerated below), because the
 *      first draft's "epoch arms, ISR retires" hid a race: with no replay
 *      running the ISR never executes, so only the epoch side can time out an
 *      armed capture, and an epoch-side cancel can collide with an ISR
 *      mid-block retire.
 *
 *   2. The sliced ANALYSIS. Runs only over a frozen (captured) buffer, in
 *      epoch context, a bounded slice at a time, and produces the figures the
 *      audio-quality roadmap exists to make measured rather than believed:
 *      THD+N, SNR, harmonic THD, and multitone frequency-response points.
 *
 * METHODS, as corrected by the item 3 prebuild gate -- the split is
 * load-bearing, do not "simplify" it into one estimator:
 *
 *   - THD+N and SNR come from an IEEE 1057-shape 3-parameter sine fit at the
 *     known f0: sin/cos/DC products accumulated across slices, the fitted
 *     fundamental subtracted, residual power = noise + distortion. "Total
 *     power minus the fundamental's leakage bins" is FFT-grid vocabulary that
 *     Goertzel does not define, and Hann leakage outside any usable notch is
 *     ~-40 dB -- an estimator built that way prints window leakage as THD+N.
 *     The fit is exact at any f0 with NO window and no coherence requirement,
 *     so its residual bottoms out at the stimulus floor, not the window's.
 *   - SNR additionally excludes harmonics 2..10 from the residual; THD+N does
 *     not. The two are different numbers by definition, and the known-answer
 *     suite drives a signal (sine + -60 dB third harmonic) where they differ
 *     by ~80 dB to keep it that way.
 *   - Goertzel keeps the jobs it is right for: harmonic amplitudes (THD) and
 *     multitone frequency-response points, Hann-windowed, where >= 75-bin
 *     separations put the leakage below -120 dB (440 Hz at N = 8192 is 75.09
 *     bins from DC, and the negative-frequency image is below -130 dB). Any
 *     per-bin noise normalization states Hann's ENBW (1.5 bins).
 *
 * ARITHMETIC / DETERMINISM. Double precision throughout, under the same
 * obligation as aius_dsp.h: build host and target with -ffp-contract=off.
 * The per-sample path is pure +,-,* on doubles -- the fit bases and the Hann
 * window advance by coupled-rotation recurrences and each Goertzel bin by its
 * standard recurrence -- so a slice is bit-identical host-to-target. The
 * init-time cos/sin that seed those recurrences are the same libm dependence
 * the biquad designers (aius_biquad_lowpass et al.) already carry.
 *
 * NO HEAP, NO ALLOCATION. Buffers and state are caller-owned; on target the
 * 128 KiB sample buffer lives in DMAMEM (OCRAM, no init needed -- .bss.dma is
 * NOLOAD and the boot ROM scribbles OCRAM at reset, which is exactly why the
 * CONTROL state must live in DTCM, not DMAMEM) and this struct in DTCM. */

/* --- capture geometry ------------------------------------------------------ */

/* Frames per capture: 16384 frames x 2 ch x int32 = 128 KiB, 341 ms at 48 kHz,
 * exactly 32 x 512-frame pipeline blocks so a capture always retires on a
 * block boundary. */
#define AIUS_MEASURE_CAPTURE_FRAMES 16384u

/* The default analysis window: the LAST 8192 frames of a full capture, leaving
 * a 171 ms settle prefix for LR4 + limiter-release transients (thin, per the
 * design note -- vector and capture length are sized together). */
#define AIUS_MEASURE_WINDOW_FRAMES 8192u

/* The declared per-epoch analysis slice, hosted at the METRICS checkpoint on
 * target. A full 8192-sample pass completes in 32 slices. */
#define AIUS_MEASURE_SLICE_FRAMES 256u

/* --- capture state machine -------------------------------------------------- */

/* One writer per transition:
 *
 *   idle       -> armed       epoch  (aius_measure_arm; every parameter stored
 *                                     before the state store -- stage/commit)
 *   armed      -> capturing   ISR    (first block after the arm)
 *   capturing  -> captured    ISR    (buffer complete; the ISR's terminal --
 *                                     captured means buffer-complete, NOT
 *                                     figures-published)
 *   armed/
 *   capturing  -> cancelling  epoch  (aius_measure_cancel sets the token only)
 *   capturing  -> failed      ISR    (arm-time generation/overrun latch moved
 *                                     at a block boundary; also from armed,
 *                                     when the latch check fails on the very
 *                                     first block)
 *   cancelling -> failed      ISR    (acknowledges the token at its next
 *                                     block) or epoch (aius_measure_reclaim,
 *                                     after a bounded number of epochs with
 *                                     the block counter unchanged -- the one
 *                                     case where the ISR is provably not
 *                                     running). THE ONE DUAL-TERMINAL EDGE,
 *                                     benign by construction: both writers
 *                                     store the same terminal state value,
 *                                     and the ISR cannot act again once the
 *                                     epoch moves failed -> idle. The native
 *                                     suite drives exactly that interleaving.
 *   captured   -> analysing   epoch  (aius_measure_begin_analysis; analysis
 *                                     reads only captured buffers)
 *   analysing  -> done        epoch  (aius_measure_finish_analysis; done
 *                                     means the figures are published)
 *   done/failed-> idle        epoch  (aius_measure_release)
 *
 * The ISR side acts only in armed/capturing/cancelling and ignores every
 * other state, so a stale block that lands after an epoch-side retire is a
 * no-op rather than a corruption. */
typedef enum {
    AIUS_MEASURE_IDLE = 0,
    AIUS_MEASURE_ARMED,
    AIUS_MEASURE_CAPTURING,
    AIUS_MEASURE_CAPTURED,
    AIUS_MEASURE_ANALYSING,
    AIUS_MEASURE_DONE,
    AIUS_MEASURE_CANCELLING,
    AIUS_MEASURE_FAILED
} AiusMeasureState;

/* Why a capture retired to failed. GENERATION and OVERRUN are the arm-time
 * latch checks: enqueue() applies a staged DSP model at any block boundary and
 * an overrun drops 512 frames outright, and either splice inside a window is
 * exactly the seam artifact the stimulus work paid to remove. CANCELLED is
 * the ISR's acknowledgement of the cancel token; RECLAIMED is the epoch's,
 * when the ISR is provably not running. */
typedef enum {
    AIUS_MEASURE_FAIL_NONE = 0,
    AIUS_MEASURE_FAIL_GENERATION,
    AIUS_MEASURE_FAIL_OVERRUN,
    AIUS_MEASURE_FAIL_CANCELLED,
    AIUS_MEASURE_FAIL_RECLAIMED
} AiusMeasureFailReason;

/* Caller-owned capture control. On target this struct lives in DTCM (never
 * DMAMEM: .bss.dma is NOLOAD and the boot ROM scribbles OCRAM at reset); the
 * sample planes it points at live in DMAMEM, which needs no init for the
 * transmit ring's same reason. `state` and `cursor` are read across the
 * ISR/epoch boundary, so they are volatile; the target adapter owes the
 * ISR/epoch pair its memory barriers, exactly as it does for AiusMeter's
 * seqlock. */
typedef struct {
    volatile uint32_t state;       /* AiusMeasureState */
    uint32_t fail_reason;          /* AiusMeasureFailReason, valid in failed */
    int32_t *l;                    /* caller-owned plane, frames_cap samples */
    int32_t *r;                    /* caller-owned plane, frames_cap samples */
    uint32_t frames_cap;           /* capture length (target: CAPTURE_FRAMES) */
    volatile uint32_t cursor;      /* frames captured; ISR-owned while capturing */
    uint32_t armed_generation;     /* config generation latched at arm */
    uint32_t armed_overruns;       /* overrun counter latched at arm */
} AiusMeasureCapture;

/* Bind the planes and reset to idle. Planes must hold frames_cap samples each.
 * Call once, cold, before the steady-state loop. */
AiusStatus aius_measure_capture_init(AiusMeasureCapture *c,
                                     int32_t *l, int32_t *r,
                                     uint32_t frames_cap);

/* Epoch: idle -> armed. Latches the config generation and the overrun counter
 * the caller reads AT ARM; the ISR retires the capture to failed if either
 * has moved at any later block boundary. All fields are stored before the
 * state store (the transmit ring's stage/commit rule), so an ISR that sees
 * `armed` sees complete parameters. Any state but idle: AIUS_INVALID_ARG. */
AiusStatus aius_measure_arm(AiusMeasureCapture *c,
                            uint32_t config_generation,
                            uint32_t overruns);

/* ISR, once per committed block: the tap. In armed/capturing, first re-checks
 * the arm-time latches against the values passed here (retiring to failed on
 * a mismatch, WITHOUT copying the block), then copies `frames` samples of the
 * committed stereo pair into the capture planes and retires to captured when
 * they are full. In cancelling, acknowledges the token (-> failed, cancelled)
 * and copies nothing. In every other state: a no-op. Bounded, allocation-free.
 * NOT marked FLASHMEM: this is the one function here that genuinely runs per
 * audio block. */
void aius_measure_isr_block(AiusMeasureCapture *c,
                            const int32_t *l, const int32_t *r,
                            uint32_t frames,
                            uint32_t config_generation,
                            uint32_t overruns);

/* Epoch: armed/capturing -> cancelling. Sets the token only; the ISR
 * acknowledges it at its next block. Any other state: AIUS_INVALID_ARG. */
AiusStatus aius_measure_cancel(AiusMeasureCapture *c);

/* Epoch: cancelling -> failed (reclaimed), for the one case where the ISR is
 * provably not running -- the caller has watched its block counter stay
 * unchanged for a bounded number of epochs. If the ISR's acknowledgement won
 * the race between the epoch's check and this call, the capture is already
 * failed (cancelled): that is the benign dual-terminal edge -- same terminal
 * state, first reason kept -- and this returns AIUS_OK. Any other state:
 * AIUS_INVALID_ARG. */
AiusStatus aius_measure_reclaim(AiusMeasureCapture *c);

/* Epoch: captured -> analysing. Analysis reads only captured buffers; this is
 * the gate that says so. Any other state: AIUS_INVALID_ARG. */
AiusStatus aius_measure_begin_analysis(AiusMeasureCapture *c);

/* Epoch: analysing -> done, once the figures are published. */
AiusStatus aius_measure_finish_analysis(AiusMeasureCapture *c);

/* Epoch: done/failed -> idle. After this the ISR cannot act on the retired
 * capture again (it ignores idle), which is what makes the dual-terminal
 * cancel edge safe to leave benign. Any other state: AIUS_INVALID_ARG. */
AiusStatus aius_measure_release(AiusMeasureCapture *c);

/* --- sliced analysis -------------------------------------------------------- */

/* Harmonics 2..10 of f0 (9 bins), per the SNR definition. Bins at or above
 * fs/2 are dropped at init and reported absent. */
#define AIUS_MEASURE_HARMONICS 9u

/* Multitone frequency-response probe bins per analysis. */
#define AIUS_MEASURE_PROBES_MAX 8u

/* dB floor substituted when a power underflows to zero (a silent capture, an
 * absent harmonic): finite, deterministic, and far below every resolvable
 * floor the surface states. */
#define AIUS_MEASURE_DB_FLOOR (-400.0)

/* The published figures. Amplitudes are dBFS re AIUS_DSP_SAMPLE_MAX (the
 * 24-bit full scale the tap copies); ratios are dB re the fitted fundamental.
 * Resolvable floors are a property of the STIMULUS, not of this struct: a
 * 16-bit replay vector bottoms THD+N/SNR at ~-93..-96 dB and Goertzel THD
 * stays usable to ~-95 dBc; the -140 dB inter-stage floor is not verifiable
 * on-device by any 16-bit transport and stays host-proven. The HTTP surface
 * states the floor beside each figure. */
typedef struct {
    double f0_hz;             /* the fit frequency, echoed */
    uint32_t frames;          /* samples the figures were computed over */
    double fundamental_dbfs;  /* fitted fundamental amplitude */
    double dc_dbfs;           /* fitted DC offset magnitude */
    double thd_n_db;          /* residual (noise+distortion) power / fundamental */
    double snr_db;            /* fundamental / (residual minus harmonics 2..10) */
    double thd_db;            /* harmonics 2..10 power sum / fundamental (dBc) */
    uint32_t harmonics;       /* leading valid entries of harmonic_dbc */
    double harmonic_dbc[AIUS_MEASURE_HARMONICS]; /* [0] = 2nd .. [8] = 10th */
    uint32_t probes;          /* leading valid entries of probe_dbfs */
    double probe_dbfs[AIUS_MEASURE_PROBES_MAX];  /* per requested probe */
} AiusMeasureFigures;

/* Accumulator state for one channel's sliced pass. ~600 bytes, caller-owned,
 * epoch-context only. The slice sequence is deterministic: feeding the same
 * window as one slice or as 32 x 256-sample slices accumulates the identical
 * sequence of double operations, so the figures are bit-identical either way
 * (the known-answer suite pins that). */
typedef struct {
    double fs_hz;
    double f0_hz;
    uint32_t n_total;   /* planned window length */
    uint32_t n;         /* samples accumulated so far */
    /* 3-parameter sine-fit bases (coupled rotation at w0) + normal-equation sums */
    double bc, bs;              /* cos(w0 n), sin(w0 n) at the cursor */
    double rot_c, rot_s;        /* cos(w0), sin(w0) */
    double scc, sss, scs, sc, ss;   /* Gram sums */
    /* Data sums carry Neumaier compensation (the *_e terms): the residual
     * identity at finish subtracts two ~2^56 quantities to expose a residual
     * fourteen orders down, and naively accumulated rounding error lands AT
     * the 24-bit quantization floor the pure-sine canary pins -- the canary
     * caught exactly that. */
    double sxc, sxs, sx, sxx;       /* data sums */
    double sxc_e, sxs_e, sx_e, sxx_e; /* their compensation terms */
    /* Hann window oscillator (rotation at 2*pi/n_total) + window running sum */
    double wc, ws;
    double wrot_c, wrot_s;
    double wsum;
    /* Hann-windowed Goertzel bins: harmonics then probes */
    uint32_t bins;
    uint32_t harmonics;
    uint32_t probes;
    double bin_cosw[AIUS_MEASURE_HARMONICS + AIUS_MEASURE_PROBES_MAX];
    double bin_sinw[AIUS_MEASURE_HARMONICS + AIUS_MEASURE_PROBES_MAX];
    double bin_s1[AIUS_MEASURE_HARMONICS + AIUS_MEASURE_PROBES_MAX];
    double bin_s2[AIUS_MEASURE_HARMONICS + AIUS_MEASURE_PROBES_MAX];
} AiusMeasureAnalysis;

/* Prepare a sliced pass over `window_frames` samples at `fs_hz` with the
 * fundamental at `f0_hz`, plus up to AIUS_MEASURE_PROBES_MAX frequency-
 * response probe bins (probe_hz may be NULL when probes is 0). Harmonic bins
 * 2..10 of f0 are installed as long as they sit below fs/2. Evaluates
 * transcendentals -- call it cold (epoch context). AIUS_INVALID_ARG for NULL,
 * non-positive rates, f0 outside (0, fs/2), window_frames == 0, probes over
 * the cap, or a probe outside (0, fs/2). */
AiusStatus aius_measure_analysis_init(AiusMeasureAnalysis *a,
                                      double fs_hz, double f0_hz,
                                      uint32_t window_frames,
                                      const double *probe_hz, uint32_t probes);

/* Accumulate the next `frames` samples of the window (epoch context; pass
 * AIUS_MEASURE_SLICE_FRAMES per epoch on target). Samples beyond the planned
 * window length are ignored. Pure double arithmetic, no transcendentals, no
 * allocation. */
AiusStatus aius_measure_analysis_slice(AiusMeasureAnalysis *a,
                                       const int32_t *samples,
                                       uint32_t frames);

/* Solve the fit and publish the figures. Requires the full planned window to
 * have been accumulated (n == n_total): a short window would silently report
 * figures for a measurement that did not happen as declared. Epoch context;
 * the 3x3 normal-equation solve and the dB conversions run here, once. */
AiusStatus aius_measure_analysis_finish(const AiusMeasureAnalysis *a,
                                        AiusMeasureFigures *out);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_MEASURE_H */
