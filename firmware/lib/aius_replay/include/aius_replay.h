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
 * task and the consumer is the audio callback. */
#define AIUS_REPLAY_WORD_RING_CAPACITY 4096u
#define AIUS_REPLAY_START_WATERMARK_WORDS 2048u
#define AIUS_REPLAY_MAX_ID_LENGTH 48u
#define AIUS_REPLAY_WORDS_PER_SECOND 48000u
#define AIUS_REPLAY_BYTES_PER_SECOND 192000u

/* The largest byte range one planned refill may cover, and the largest chunk
 * one delivery may carry.
 *
 * The refill bound caps how long a single bounded SD job holds the card, which
 * is what the platform's other consumers wait on; it is deliberately about one
 * ring's worth so a refill is one job per ~40 ms of audio rather than a dribble
 * of jobs per epoch.  The delivery bound is one transfer slice, matching the
 * portable SD engine's per-primitive read, and is what sizes `scratch_words`. */
#define AIUS_REPLAY_REFILL_MAX_BYTES 4096u
#define AIUS_REPLAY_DELIVER_MAX_BYTES 512u

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
	 * batch instead of pushing word by word. */
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
 * AIUS_REPLAY_DELIVER_MAX_BYTES per call, always even.  Promotes PREBUFFERING to
 * PLAYING once the ring reaches the watermark. */
AiusStatus aius_replay_service_deliver(AiusReplayService *service,
	                                   uint32_t offset_bytes,
	                                   const uint8_t *bytes,
	                                   size_t byte_count);

/* Drops the in-flight plan without consuming it: the platform could not perform
 * the read this epoch (its shared primitive belongs to someone else) and will
 * ask again.  Memory only, and deliberately NOT the same as a zero-byte
 * completion -- a refusal is not the end of the file. */
void aius_replay_service_abandon_refill(AiusReplayService *service);

/* Settles the in-flight refill.  `ok` false means the platform's read failed,
 * which is an ERROR the status surface reports and one decoder error.  `ok` true
 * with fewer bytes delivered than planned means the file ended: the service
 * either wraps to zero (loop) or stops asking. */
void aius_replay_service_refill_complete(AiusReplayService *service, bool ok);

/* The callback-facing wrapper marks a short pop as an underrun.
 *
 * UNCHANGED BY THE REFILL REDESIGN, deliberately: this runs per audio block, so
 * it must not gain a lock, a branch that can wait, or any refill work. */
size_t aius_replay_service_pop_words(AiusReplayService *service,
	                                uint16_t *out, size_t capacity);

void aius_replay_service_stop(AiusReplayService *service);
AiusStatus aius_replay_service_status(const AiusReplayService *service,
	                                  AiusReplayStatus *out);
void aius_replay_service_set_decoder_status(AiusReplayService *service,
	                                        const AiusReplayDecoderStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_REPLAY_H */
