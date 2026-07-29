#ifndef AIUS_PIPELINE_H
#define AIUS_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"
#include "aius_codec.h"
#include "aius_dts.h"
#include "aius_iec61937.h"

#ifdef __cplusplus
extern "C" {
#endif

/* S/PDIF input pipeline: classify -> (IEC 61937 de-frame -> DTS Core decode)
 * or PCM pass-through (capability-gate phase, chains P2 parser into P3 decoder).
 *
 * Consumes the 16-bit S/PDIF (IEC 60958) subframe payload-word stream and routes
 * it two ways off a single classification:
 *
 *   AIUS_PIPELINE_DTS  the stream carries IEC 61937 non-linear PCM (DTS Core).
 *                      The pipeline drives AiusIec61937Parser to unwrap bursts
 *                      and feeds each burst payload (a raw DTS Core access unit)
 *                      into AiusDtsDecoder, emitting decoded planar AiusPcmBlocks.
 *   AIUS_PIPELINE_PCM  the stream is IEC 60958 linear PCM. The words are treated
 *                      as interleaved L,R 16-bit samples and emitted (no decode)
 *                      as 2ch / 48 kHz blocks, left-aligned to the AiusPcmBlock
 *                      sample scale so they arrive at the same full scale as a
 *                      decoded DTS block. Bit-exact -- the scaling is a shift, not
 *                      a resample -- but NOT "verbatim": this header said verbatim
 *                      while the samples stayed in 16-bit units, and that cost
 *                      48.16 dB on every pass-through stream. See aius_codec.h
 *                      §SAMPLE SCALE.
 *
 * Classification detects the IEC 61937 preamble (Pa=0xF872, Pb=0x4E1F, either
 * wire byte order) within a bounded word window; a stream with no preamble in the
 * window is linear PCM. See aius_pipeline_classify.
 *
 * CLASSIFICATION IS NOT ONE-SHOT (defect 28). A typed stream keeps being
 * re-judged in both directions:
 *
 *   PCM -> DTS   the PCM route scans every word for preamble pairs. TWO pairs
 *                of the same byte-order form at plausible spacing (see
 *                AIUS_PIPELINE_RETYPE_*_SPACING_WORDS) re-type the stream to
 *                DTS mid-flight: staged pass-through frames are dropped (at a
 *                genuine format switch they are burst data misread as PCM),
 *                the parser is reset and fed from the validated pair onward.
 *                One pair is never enough -- a random 16-bit-word pair match
 *                in real audio must not flip a live stream.
 *   DTS -> PCM   words consumed by the DTS route with NO parser burst outcome
 *                (completed OR rejected -- either is framing evidence) starve
 *                the class back to UNKNOWN after
 *                AIUS_PIPELINE_DTS_STARVE_WORDS, and the next words
 *                re-classify. Framed-but-unsupported streams (AC-3, pause and
 *                null bursts) complete bursts and therefore never starve:
 *                they stay DTS-classified and silent by design, not as noise.
 *
 * Both transitions count into lifetime diagnostics (see
 * aius_pipeline_get_reclass_counters); aius_pipeline_reset() clears the
 * detector/starvation state but NOT the counters.
 *
 * NOTHING REACHES THE SINK UNTIL THE PCM CLASS IS CONFIRMED. The PCM typing is
 * decided by the ABSENCE of a preamble in a bounded window, so a stream entered
 * mid-burst types PCM and would play full-scale IEC burst data on whatever the
 * sink drives. A PCM run therefore emits nothing for its first
 * AIUS_PIPELINE_PCM_CONFIRM_WORDS words while the re-type detector runs; the
 * words are DROPPED, not delayed, so the pass-through adds no latency -- it
 * starts late. See AIUS_PIPELINE_PCM_CONFIRM_WORDS.
 *
 * Every decoded or passed-through block is delivered to the caller's AiusPcmSink.
 * The steady-state push path performs no allocation beyond the DTS decoder's own
 * arena/heap (classify buffer and PCM staging live in the fixed instance). Only
 * create allocates: the instance, the IEC parser, and the DTS decoder.
 */

/* Words scanned for a preamble before a stream is declared linear PCM. Also the
 * capacity of the fixed classification staging buffer in the instance. */
#define AIUS_PIPELINE_CLASSIFY_MAX_WORDS 512u

/* PCM->DTS re-type pair-spacing bounds, in words. Two same-form preamble pairs
 * whose spacing lies inside [MIN, MAX] re-type a PCM stream to DTS.
 *
 * MIN sits below the smallest real DTS repetition period (1024 words for
 * 512-frame access units) and rejects trivially-close random pairs. It MUST
 * stay >= AIUS_PIPELINE_CLASSIFY_MAX_WORDS: that coupling is what makes a
 * re-type inside finish()'s PCM-promotion flush impossible (asserted in the
 * implementation). Pairs closer than MIN keep the OLDER candidate -- pause and
 * null data-burst idles repeat pairs at sub-MIN intervals, and the accumulated
 * spacing from the older pair is what lets such a stream re-type at all.
 *
 * MAX is 2x the largest DTS period (4096 words). A candidate older than MAX is
 * expired (also kills uint32 position-wrap aliasing of an hours-stale
 * candidate). Random-match arithmetic: one random pair in full-scale random
 * PCM about every 6-12 h; a second same-form pair within MAX of it is a ~2e-6
 * conditional -- a false re-type is effectively impossible. */
#define AIUS_PIPELINE_RETYPE_MIN_SPACING_WORDS 512u
#define AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS 8192u

/* Words consumed by the DTS route with no parser burst outcome (completed or
 * rejected) before the class starves back to UNKNOWN. >= 4x the worst
 * legitimate NEED_MORE span (~2341-word 14-bit accumulation, ~4096-word
 * type-III period); ~170 ms at 48 kHz bounds the silent interval after a live
 * DTS->PCM material switch. Evaluated at push boundaries, so the observable
 * re-classification needs subsequent input. */
#define AIUS_PIPELINE_DTS_STARVE_WORDS 16384u

/* Frames per emitted PCM pass-through block (staged in the fixed instance). */
#define AIUS_PIPELINE_PCM_BLOCK_FRAMES 512u
#define AIUS_PIPELINE_MAX_OUTPUT_FRAMES 512u

/* Words of a PCM run that pass in silence before the pass-through is allowed to
 * reach the sink -- the confirmation squelch.
 *
 * While the class is PCM and fewer than CONFIRM words have been consumed SINCE
 * THE CLASS WAS ENTERED -- counting from the first word of the run, the staged
 * classification-window words INCLUDED -- the re-type detector runs exactly as
 * it does once confirmed, but nothing is staged and no block is emitted. The
 * boundary is run word CONFIRM exactly, taken from a per-run word counter and
 * never from a push offset, so the boundary lands identically under any push
 * sizing (1-word chunks included).
 *
 * MUST stay >= AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS (asserted in the
 * implementation). The widest supported burst period is 4096 words, so even the
 * worst re-type phase validates its second pair no later than the boundary word
 * itself -- a mis-typed stream can never reach the sink through a classification
 * window. MUST also be EVEN (asserted): the boundary word is an L sample, and
 * that parity is what makes the channel split at the boundary knowable.
 *
 * Emission is quantized to AIUS_PIPELINE_PCM_BLOCK_FRAMES, so the first frame
 * can only reach the sink at run word CONFIRM + 2 * PCM_BLOCK_FRAMES. A caller
 * that never calls aius_pipeline_finish() (the target does not) therefore emits
 * NOTHING at all from a PCM run shorter than 9216 words (~96 ms at 48 kHz);
 * genuine PCM opens ~85 ms late plus up to one block of that quantization. */
#define AIUS_PIPELINE_PCM_CONFIRM_WORDS 8192u

/* Sample rate stamped on PCM pass-through blocks. */
#define AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ 48000u

/* Typed classification of the payload-word stream. */
typedef enum {
	AIUS_PIPELINE_UNKNOWN = 0, /* undecided: fewer than `window` words, no preamble yet */
	AIUS_PIPELINE_PCM,         /* IEC 60958 linear PCM (no preamble within the window) */
	AIUS_PIPELINE_DTS          /* IEC 61937 non-linear (DTS Core): preamble detected */
} AiusPipelineClass;

/* Stateless classifier over a contiguous payload-word buffer. Scans the first
 * `window` words (0 selects AIUS_PIPELINE_CLASSIFY_MAX_WORDS) for the IEC 61937
 * preamble pair Pa/Pb in either wire byte order:
 *
 *   AIUS_PIPELINE_DTS      a preamble pair was found within the window.
 *   AIUS_PIPELINE_PCM      at least `window` words present with no preamble.
 *   AIUS_PIPELINE_UNKNOWN  fewer than `window` words and none found yet — the
 *                          caller supplies more before deciding.
 */
AiusPipelineClass aius_pipeline_classify(const uint16_t *words, size_t count,
                                         size_t window);

/* Pipeline configuration. A zeroed struct selects the defaults (16-bit packing,
 * the maximum classification window). */
typedef struct {
	AiusIec61937Packing packing;    /* burst payload packing for the DTS path */
	size_t classify_window_words;   /* 0 => AIUS_PIPELINE_CLASSIFY_MAX_WORDS; clamped to it */
} AiusPipelineConfig;

/* Opaque pipeline instance. */
typedef struct AiusPipeline AiusPipeline;

typedef enum {
	AIUS_PIPELINE_SERVICE_IDLE = 0,
	AIUS_PIPELINE_SERVICE_PROGRESSED,
	AIUS_PIPELINE_SERVICE_TRANSMIT_BLOCKED
} AiusPipelineServiceResult;

typedef struct {
	uint32_t decode_units;
	uint32_t sink_attempts;
	uint32_t committed_blocks;
	uint32_t transmit_stalls;
	uint32_t retained_retries;
	uint32_t parse_rejects;
	uint32_t decode_rejects;
	uint32_t output_drops;
	uint32_t au_abandons;
	AiusStatus last_failure;
} AiusPipelineServiceDiag;

/* Create a pipeline whose DTS decoder allocates through libc (host convenience).
 * `cfg` may be NULL for defaults; `sink` (with a non-NULL consume) is required
 * and copied. On success *out holds a heap-owned handle; destroy with
 * aius_pipeline_destroy. Returns AIUS_OK, AIUS_NO_SPACE, or AIUS_INVALID_ARG. */
AiusStatus aius_pipeline_create(AiusPipeline **out,
                                const AiusPipelineConfig *cfg,
                                const AiusPcmSink *sink);

/* Create a pipeline whose instance and DTS decoder come from `arena` (the target-
 * shaped no-libc-heap path for the decode side; the IEC parser has no arena
 * provisioning and is libc-allocated at create in both modes). `arena` must
 * outlive the pipeline. The owner MAY carve its own long-lived allocations from
 * the arena BEFORE this call (the target carves the DSP chain's double staging
 * there -- see optical_main begin_pipeline); after create the remaining arena
 * belongs to the pipeline alone, and nothing may ever reset it. Size it from
 * the decoder's arena high-water (see aius_dts_get_metrics) plus the instance
 * plus any owner carve. Returns AIUS_OK, AIUS_NO_SPACE, or AIUS_INVALID_ARG. */
AiusStatus aius_pipeline_create_arena(AiusPipeline **out,
                                      const AiusPipelineConfig *cfg,
                                      const AiusPcmSink *sink, AiusArena *arena);

/* Push `count` S/PDIF payload words through the synchronous compatibility
 * driver, emitting zero or more decoded/passed-through blocks to the sink.
 * It resumes exact input suffixes only while service succeeds. Its legacy
 * signature cannot return a retained suffix, so sink AIUS_NO_SPACE resets the
 * pending item and loses the rest of this call. Returns:
 *
 *   AIUS_OK          input processed; any produced blocks were delivered.
 *   AIUS_INVALID_ARG p is NULL, or words is NULL with count > 0.
 *   <sink status>    the sink returned non-OK for a block; processing stops and
 *                    the sink's status is returned (e.g. AIUS_NO_SPACE).
 *
 * A per-burst parse/decode fault (UNSUPPORTED / MALFORMED / NO_SPACE / a DTS
 * decode error) is non-fatal: that burst is skipped, the stream re-locks, and the
 * fault is recorded (see aius_pipeline_get_metrics.last_status). */
AiusStatus aius_pipeline_push(AiusPipeline *p, const uint16_t *words,
                              size_t count);

/* Deferred target path. Ingest classifies and de-frames only: it never calls a
 * decoder or sink. It reports the exact accepted prefix and stops when one
 * output work item is pending. Parser-owned payload remains valid until that
 * item is serviced or reset. */
AiusStatus aius_pipeline_ingest(AiusPipeline *p, const uint16_t *words,
	                            size_t count, size_t *consumed);

/* Perform at most one output work unit. AIUS_NO_SPACE is reported as
 * TRANSMIT_BLOCKED and retains the identical PCM descriptor for retry without
 * another decode. Other sink failures reset pending stream state. */
AiusStatus aius_pipeline_service_one(AiusPipeline *p,
	                                 AiusPipelineServiceResult *result);

/* Produce a non-copying, at-most-512-frame view into a decoder-owned block.
 * `frame_offset` advances only after the caller commits the prior view. */
AiusStatus aius_pipeline_slice_pcm_block(const AiusPcmBlock *source,
	                                     uint32_t frame_offset,
	                                     AiusPcmBlock *slice);

int aius_pipeline_has_pending(const AiusPipeline *p);
int aius_pipeline_has_buffered_input(const AiusPipeline *p);
void aius_pipeline_get_service_diag(const AiusPipeline *p,
	                                AiusPipelineServiceDiag *out);
void aius_pipeline_clear_service_diag(AiusPipeline *p);

/* Signal end of input: flush a partial PCM block, and finalize the parser
 * (AIUS_TRUNCATED if a DTS burst was mid-parse). Returns AIUS_OK or the first
 * non-OK status encountered while flushing. */
AiusStatus aius_pipeline_finish(AiusPipeline *p);

/* Drop all in-progress state and return to the UNKNOWN (unclassified) state.
 * NULL is a no-op. */
void aius_pipeline_reset(AiusPipeline *p);

/* Release a libc-created pipeline and its owned parser/decoder. NULL is a no-op.
 * An arena-created pipeline's storage is reclaimed by discarding the caller's
 * arena; this call still runs the parser/decoder teardown. */
void aius_pipeline_destroy(AiusPipeline *p);

/* The current classification (AIUS_PIPELINE_UNKNOWN until decided). */
AiusPipelineClass aius_pipeline_classification(const AiusPipeline *p);

/* Copy the DTS decoder's most-recent measurements into *out (see
 * aius_dts_get_metrics). `last_status` is the last parse/decode result the
 * pipeline observed. Returns AIUS_OK or AIUS_INVALID_ARG. */
AiusStatus aius_pipeline_get_metrics(const AiusPipeline *p, AiusMetrics *out);

/* Copy the IEC 61937 parser's reject tallies (see AiusIecDiag). Returns
 * AIUS_OK or AIUS_INVALID_ARG. */
AiusStatus aius_pipeline_get_parser_diag(const AiusPipeline *p,
                                         AiusIecDiag *out);

/* Lifetime re-classification counters: *retype counts PCM->DTS re-types (the
 * validated-pair path), *reclassify counts DTS->UNKNOWN starvation resets.
 * Both survive aius_pipeline_reset() and clear only at create -- on a live
 * board a non-zero retype_count is what distinguishes a mid-stream re-type
 * from a reboot that happened to classify DTS. Returns AIUS_OK or
 * AIUS_INVALID_ARG. */
AiusStatus aius_pipeline_get_reclass_counters(const AiusPipeline *p,
                                              uint32_t *retype,
                                              uint32_t *reclassify);

/* Confirmation-squelch diagnostics (see AIUS_PIPELINE_PCM_CONFIRM_WORDS).
 *
 * *squelched_frames is the lifetime count of FRAMES the squelch dropped (words
 * / 2, floor -- a trailing half-word is not counted). Like the reclass counters
 * it survives aius_pipeline_reset() and clears only at create.
 * *pcm_confirmed is LIVE gate state -- 1 exactly while the class is PCM and its
 * run has reached the confirm boundary -- so a verifier reads the gate rather
 * than inferring it from a counter delta. Returns AIUS_OK or AIUS_INVALID_ARG. */
AiusStatus aius_pipeline_get_squelch_diag(const AiusPipeline *p,
                                          uint32_t *squelched_frames,
                                          int *pcm_confirmed);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_PIPELINE_H */
