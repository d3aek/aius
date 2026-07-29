#ifndef AIUS_REPLAY_H
#define AIUS_REPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

#include "aius_codec.h"
#include "aius_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The replay seam is deliberately fixed-size.  The producer is the service
 * task and the consumer is the audio callback.
 *
 * SIZED BY LATENCY AND BY EPOCHS, NOT BY TASTE.  The consumer drains
 * AIUS_REPLAY_WORDS_PER_SECOND * 2 = 96,000 words per second, unconditionally,
 * because the audio clock does not wait.  The producer delivers in bursts: a
 * refill becomes a platform job, the job takes a fixed number of epochs to hand
 * its bytes over, and NOTHING reaches the ring until it does.  So the watermark
 * is not a comfort margin -- it is the audio the ring must hold to cover one
 * whole refill round trip, and a watermark shorter than that round trip
 * guarantees a drained ring on every cycle no matter how fast the card is.  The
 * 4,096-word ring and 2,048-word watermark this seam shipped with held 21 ms
 * against a refill that took ~33 ms to land: starvation by construction, and
 * exactly what the first live measurement found.
 *
 * The ring is twice the watermark so a full-size refill always fits the space
 * above it.  The two `#error`s below are those statements, not hopes: the
 * second exists because next_index() runs per word on the audio pop path, where
 * a non-power-of-two capacity would be a hardware divide per sample. */
#define AIUS_REPLAY_WORD_RING_CAPACITY 65536u
#define AIUS_REPLAY_START_WATERMARK_WORDS 32768u
#define AIUS_REPLAY_MAX_ID_LENGTH 48u
#define AIUS_REPLAY_WORDS_PER_SECOND 48000u
#define AIUS_REPLAY_BYTES_PER_SECOND 192000u

/* The largest byte range one planned refill may cover, and the largest chunk
 * one delivery may carry.  THEY ARE NOT THE SAME NUMBER, and the difference is
 * the whole lesson of this seam's second defect.
 *
 * The delivery bound is ONE SD PRIMITIVE'S WORTH, and the platform's primitive
 * is capped at 512 bytes by the portable SD engine itself
 * (AIUS_SD_MAX_TRANSFER in aius_sd.c, enforced in validate_request(): a job
 * whose transfer capacity exceeds it is rejected with AIUS_SD_FAULT_CAPACITY
 * before a single action is emitted).  A previous attempt at this defect raised
 * the replay job's transfer buffer to 8 KB to read a whole refill in one
 * primitive.  It did not merely fail to help -- every refill job was refused at
 * validation, so the replay produced nothing at all.  512 is therefore a
 * CONTRACT, not a tuning knob, and it is not this library's to change.
 *
 * The refill bound is a different question, and it is what buys the throughput.
 * The job costs a fixed per-job overhead (BEGIN, OPEN, SIZE, SEEK, CLOSE,
 * COMPLETE) plus one epoch per 512-byte read, so a refill of N bytes costs
 * 6 + N/512 epochs and delivers 512N/(N + 3072) bytes per epoch.  That rises
 * towards 512 as N grows: 293 bytes/epoch at N = 4 KB, 431 at 16 KB, 468 at
 * 32 KB.  Realtime is 192,000 B/s, so at the measured ~672 epochs/s the seam
 * needs 286 bytes/epoch to break even -- N = 4 KB clears it by 2 %, N = 32 KB
 * by 64 %.  A refill is also one mount, and a mount is the most expensive
 * primitive the card performs, so a bigger N cuts mounts in the same
 * proportion: 5.9/s here against the 30.5/s the starved image was spending.
 *
 * 32 KB of refill wants a 64 KB ring above a 64 KB watermark, all of which
 * lives in OCRAM through the DMAMEM placement below.  That is the trade
 * §Deciding principle asks for: audio reliability first, and spend the
 * plentiful bank rather than the scarce one. */
#define AIUS_REPLAY_REFILL_MAX_BYTES 32768u
#define AIUS_REPLAY_DELIVER_MAX_BYTES 512u
#define AIUS_REPLAY_DISCARD_MAX_WORDS 256u

/* Consecutive failed refills before the replay gives up.
 *
 * A failed refill used to be terminal, which made every transient permanent.
 * Eight in a row is the line between "the card hiccupped" and "the card is not
 * going to answer", and it is deliberately a small number: a fault that is not
 * going away fails fast -- an open against a deleted file costs two epochs, so
 * the whole run is spent in well under a tenth of a second and the replay stops
 * with a reason rather than hammering the card forever.  A successful refill
 * clears the run, so this bounds a persistent fault and not an intermittent
 * one. */
#define AIUS_REPLAY_REFILL_FAULT_LIMIT 8u

/* A refill is planned only while the ring is BELOW the watermark, so the space
 * above the watermark is all a plan can ever be given.  If a full-size refill
 * did not fit there, every plan would silently shrink and the epoch cost per
 * byte would climb straight back to where this defect started. */
#if (AIUS_REPLAY_WORD_RING_CAPACITY - 1u - AIUS_REPLAY_START_WATERMARK_WORDS) * \
	2u < AIUS_REPLAY_REFILL_MAX_BYTES
#error "the ring space above the watermark must hold one full refill"
#endif

/* next_index() is on the audio pop path and reduces modulo this capacity.  A
 * power of two is an AND; anything else is a hardware divide per word, ~1,024
 * of them per audio block. */
#if (AIUS_REPLAY_WORD_RING_CAPACITY & (AIUS_REPLAY_WORD_RING_CAPACITY - 1u)) != 0u
#error "the word ring capacity must be a power of two -- next_index() is hot"
#endif

typedef struct {
	uint16_t words[AIUS_REPLAY_WORD_RING_CAPACITY];
#ifdef __cplusplus
	std::atomic<uint32_t> read_index;
	std::atomic<uint32_t> write_index;
#else
	_Atomic uint32_t read_index;
	_Atomic uint32_t write_index;
#endif
} AiusReplayWordRing;

/* Initialize/reset is service-side only.  Push and pop are the two SPSC
 * operations; neither waits, allocates, locks, or touches a file. */
void aius_replay_word_ring_init(AiusReplayWordRing *ring);
void aius_replay_word_ring_reset(AiusReplayWordRing *ring);
size_t aius_replay_word_ring_push(AiusReplayWordRing *ring,
	                              const uint16_t *words, size_t count);
size_t aius_replay_pop_words(AiusReplayWordRing *ring, uint16_t *out,
	                         size_t capacity);
size_t aius_replay_word_ring_available(const AiusReplayWordRing *ring);
size_t aius_replay_word_ring_space(const AiusReplayWordRing *ring);

/* Convert the stored [low, high] byte representation to one IEC word. */
uint16_t aius_replay_word_from_le(const uint8_t bytes[2]);

/* Decode a complete byte range without partial output.  Odd byte lengths are
 * malformed because the replay format contains only 16-bit words. */
AiusStatus aius_replay_decode_le_words(const uint8_t *bytes, size_t byte_count,
	                                   uint16_t *out, size_t word_capacity,
	                                   size_t *word_count);

/* Safe replay IDs are shared by the service and the later HTTP adapter. */
bool aius_replay_id_is_valid(const char *id);

typedef enum {
	AIUS_REPLAY_STOPPED = 0,
	AIUS_REPLAY_PREBUFFERING,
	AIUS_REPLAY_PLAYING,
	AIUS_REPLAY_EOF,
	AIUS_REPLAY_UNDERRUN,
	AIUS_REPLAY_ERROR
} AiusReplayState;

typedef enum {
	AIUS_REPLAY_SOURCE_OPTICAL = 0,
	AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY,
	AIUS_REPLAY_SOURCE_REPLAY,
	AIUS_REPLAY_SOURCE_SWITCHING_TO_OPTICAL,
	AIUS_REPLAY_SOURCE_ERROR
} AiusReplaySource;

typedef struct {
	AiusPipelineClass classification;
	uint32_t bursts_seen;
	uint32_t frames_decoded;
	AiusStatus last_status;
	uint32_t errors;
} AiusReplayDecoderStatus;

typedef struct {
	AiusReplayState state;
	AiusReplaySource source;
	char id[AIUS_REPLAY_MAX_ID_LENGTH + 1u];
	uint32_t file_bytes;
	uint32_t offset_bytes;
	double duration_seconds;
	bool loop;
	uint32_t loops_completed;
	uint32_t queue_words;
	uint32_t watermark_words;
	uint32_t underruns;
	AiusPipelineClass input_class;
	uint32_t decoder_errors;
	/* Consecutive failed refills right now.  Non-zero on a PLAYING service is
	 * the observable difference between "recovering from a fault" and "fine",
	 * which `decoder_errors` alone cannot say because it never goes down. */
	uint32_t refill_faults;
	uint32_t upload_next_offset;
	AiusReplayDecoderStatus decoder;
} AiusReplayStatus;

/* One bounded byte range the service wants read, produced by
 * aius_replay_service_plan_refill() and performed by the platform's bounded SD
 * job.  `byte_count` is never zero, always even, never more than
 * AIUS_REPLAY_REFILL_MAX_BYTES, and never more than the ring can accept. */
typedef struct {
	uint32_t offset_bytes;
	uint32_t byte_count;
} AiusReplayRefill;

/* This object is caller-owned and may be placed in OCRAM/DMAMEM by the target
 * integration.  No member is dynamically allocated.
 *
 * WHY THERE IS NO READER CALLBACK.  Refill used to run through an
 * AiusReplayReader whose open/read/close returned immediately, which forced the
 * platform to own an SD file handle and to block inside its own service call.
 * On a target whose every storage job re-mounts the volume, an outlived handle
 * is not merely slow -- it is a correctness bug, because the mount happens
 * underneath it.  The seam is therefore a plan/deliver pair: the service owns a
 * byte OFFSET and never a handle, and the platform performs the read with the
 * same bounded, one-primitive-per-epoch job every other consumer uses.  A
 * storage job started mid-replay can no longer invalidate anything, because
 * there is nothing of the replay's open across it. */
typedef struct {
	AiusReplayWordRing ring;
	/* One delivery's worth of decoded words, so a delivery converts in one
	 * batch instead of pushing word by word.  This object is placed in
	 * DMAMEM/OCRAM by the target integration precisely so the ring and this
	 * buffer cost the plentiful bank rather than DTCM's free-for-locals zone
	 * (§Deciding principle: memory that is genuinely free is not an argument
	 * the other way round). */
	uint16_t scratch_words[AIUS_REPLAY_DELIVER_MAX_BYTES / 2u];
	char id[AIUS_REPLAY_MAX_ID_LENGTH + 1u];
	uint32_t watermark_words;
	uint32_t underruns;
	AiusReplayState state;
	AiusReplaySource source;
	bool loop;
	/* Zero until the platform publishes the size it learned from the file
	 * itself; the service treats zero as "not yet known" and leans on a
	 * short refill to find the end instead. */
	uint32_t file_bytes;
	/* The whole of the replay's read position: the next byte to read.  This
	 * is what replaces the open file handle. */
	uint32_t offset_bytes;
	uint32_t loops_completed;
	bool eof;
	/* A replay is selected: start() has run and stop() has not. */
	bool started;
	/* The one in-flight planned refill, so the service (not the platform)
	 * owns the accounting that decides EOF. */
	bool refill_planned;
	uint32_t refill_offset;
	uint32_t refill_bytes;
	/* Failed refills SINCE THE LAST SUCCESSFUL ONE, not since start: the bound
	 * separates a transient from a fault that is not going away, and only a run
	 * of them means the latter.  Reset by any completed refill. */
	uint32_t refill_faults;
	AiusReplayDecoderStatus decoder;
} AiusReplayService;

void aius_replay_service_init(AiusReplayService *service);
AiusStatus aius_replay_service_init_with_watermark(AiusReplayService *service,
	                                               uint32_t watermark_words);

/* Selects a replay and publishes PREBUFFERING / SWITCHING_TO_REPLAY.
 *
 * MEMORY ONLY.  It opens nothing, reads nothing and waits for nothing, so it is
 * legal from a dispatcher contracted to touch no SD primitive and never delay.
 * The ring reaches its watermark later, through planned refills; until it does,
 * `source` stays SWITCHING_TO_REPLAY and an integration that gates its audio on
 * SOURCE_REPLAY keeps playing its live input.  The caller learns the outcome
 * from the status surface, not from this call. */
AiusStatus aius_replay_service_start(AiusReplayService *service,
	                                 const char *id, bool loop);

/* Publishes the file size the platform learned while performing a refill.  Used
 * for the reported duration and for the EOF boundary; a size that disagrees with
 * an already-consumed offset, or an odd one, is rejected and leaves the service
 * on its short-refill EOF detection. */
AiusStatus aius_replay_service_set_file_bytes(AiusReplayService *service,
	                                          uint32_t file_bytes);

/* One service tick, called once per platform epoch.  Memory only.
 *
 * Settles the drained-EOF transition, then decides whether a bounded read is
 * wanted: true with `out` filled, false when the ring is above its watermark,
 * the file is exhausted, a refill is already in flight, or no replay is
 * selected.  A false is not an error and costs nothing -- a platform whose
 * shared SD primitive is busy simply asks again next epoch. */
bool aius_replay_service_plan_refill(AiusReplayService *service,
	                                 size_t budget_words,
	                                 AiusReplayRefill *out);

/* Hands one chunk of the planned range to the ring.  `offset_bytes` must be the
 * service's current read position, so a stale or reordered chunk is rejected
 * rather than decoded into the audio stream.  At most
 * AIUS_REPLAY_DELIVER_MAX_BYTES per call, always even.  Promotes PREBUFFERING --
 * and UNDERRUN, so a recovered replay can starve again countably -- to PLAYING
 * once the ring reaches the watermark. */
AiusStatus aius_replay_service_deliver(AiusReplayService *service,
	                                   uint32_t offset_bytes,
	                                   const uint8_t *bytes,
	                                   size_t byte_count);

/* Drops the in-flight plan without consuming it: the platform could not perform
 * the read this epoch (its shared primitive belongs to someone else) and will
 * ask again.  Memory only, and deliberately NOT the same as a zero-byte
 * completion -- a refusal is not the end of the file. */
void aius_replay_service_abandon_refill(AiusReplayService *service);

/* Settles the in-flight refill.
 *
 * `ok` false is one decoder error and one consecutive fault, and is RECOVERABLE:
 * the bytes the job did deliver stay in the ring, the read position stays where
 * they left it, and the next epoch plans again from there.  Only
 * AIUS_REPLAY_REFILL_FAULT_LIMIT faults IN A ROW publish ERROR and stop the
 * replay, so a transient costs a gap in the audio rather than the rest of the
 * file.  A completed refill clears the run.
 *
 * `ok` true with fewer bytes delivered than planned means the file ended: the
 * service either wraps to zero (loop) or stops asking. */
void aius_replay_service_refill_complete(AiusReplayService *service, bool ok);

/* The callback-facing wrapper marks a short pop as an underrun.
 *
 * UNCHANGED BY THE REFILL REDESIGN, deliberately: this runs per audio block, so
 * it must not gain a lock, a branch that can wait, or any refill work. */
size_t aius_replay_service_pop_words(AiusReplayService *service,
	                                uint16_t *out, size_t capacity);

/* Advance the same callback timeline without copying samples. A short discard
 * has exactly the same underrun transition and accounting as a short pop.
 * Work is clamped to one 256-word audio callback. */
size_t aius_replay_service_discard_words(AiusReplayService *service,
	                                    size_t capacity);

void aius_replay_service_stop(AiusReplayService *service);
AiusStatus aius_replay_service_status(const AiusReplayService *service,
	                                  AiusReplayStatus *out);
void aius_replay_service_set_decoder_status(AiusReplayService *service,
	                                        const AiusReplayDecoderStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_REPLAY_H */
