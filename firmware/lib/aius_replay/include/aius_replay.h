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
#define AIUS_REPLAY_BYTE_BUFFER_SIZE 4096u
#define AIUS_REPLAY_BYTE_BUFFER_COUNT 2u
#define AIUS_REPLAY_START_WATERMARK_WORDS 2048u
#define AIUS_REPLAY_MAX_ID_LENGTH 48u
#define AIUS_REPLAY_WORDS_PER_SECOND 48000u
#define AIUS_REPLAY_BYTES_PER_SECOND 192000u

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

/* Reader callbacks are called only by the service task.  A successful read may
 * return fewer bytes than requested, but must return an even byte count.  A
 * zero-byte successful read is EOF. */
typedef struct {
	void *ctx;
	AiusStatus (*open)(void *ctx, uint32_t file_bytes);
	AiusStatus (*read)(void *ctx, uint32_t offset_bytes, uint8_t *buffer,
	                   size_t capacity, size_t *bytes_read);
	void (*close)(void *ctx);
} AiusReplayReader;

typedef struct {
	AiusReplayReader reader;
	uint32_t file_bytes;
	uint32_t offset_bytes;
	uint32_t loops_completed;
	uint8_t buffer_index;
	size_t buffer_bytes;
	size_t buffer_position;
	bool eof;
	bool open;
} AiusReplayStream;

/* This object is caller-owned and may be placed in OCRAM/DMAMEM by the target
 * integration.  No member is dynamically allocated. */
typedef struct {
	AiusReplayWordRing ring;
	uint8_t byte_buffers[AIUS_REPLAY_BYTE_BUFFER_COUNT]
	                    [AIUS_REPLAY_BYTE_BUFFER_SIZE];
	uint16_t candidate_words[AIUS_REPLAY_START_WATERMARK_WORDS];
	uint16_t scratch_words[256];
	AiusReplayStream active;
	AiusReplayStream candidate;
	char id[AIUS_REPLAY_MAX_ID_LENGTH + 1u];
	uint32_t watermark_words;
	uint32_t underruns;
	AiusReplayState state;
	AiusReplaySource source;
	bool loop;
	uint32_t file_bytes;
	uint32_t candidate_word_count;
	AiusReplayDecoderStatus decoder;
} AiusReplayService;

void aius_replay_service_init(AiusReplayService *service);
AiusStatus aius_replay_service_init_with_watermark(AiusReplayService *service,
	                                               uint32_t watermark_words);

/* Start opens and prebuffers before publishing SOURCE_REPLAY.  If already in
 * replay, this is transactional: the old source remains in the active ring
 * until the replacement reaches the watermark. */
AiusStatus aius_replay_service_start(AiusReplayService *service,
	                                 const char *id, uint32_t file_bytes,
	                                 bool loop, const AiusReplayReader *reader);

/* Refill is service-side and bounded by the caller's consumption budget.  A
 * zero budget performs no I/O.  The service never runs a reader in pop_words. */
AiusStatus aius_replay_service_refill(AiusReplayService *service,
	                                  size_t budget_words);

/* The callback-facing wrapper marks a short pop as an underrun. */
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
