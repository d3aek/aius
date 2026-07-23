#include "aius_replay.h"

#include <string.h>

static uint32_t next_index(uint32_t index)
{
	return (index + 1u) % AIUS_REPLAY_WORD_RING_CAPACITY;
}

void aius_replay_word_ring_init(AiusReplayWordRing *ring)
{
	if (ring == NULL) return;
	atomic_init(&ring->read_index, 0u);
	atomic_init(&ring->write_index, 0u);
}

void aius_replay_word_ring_reset(AiusReplayWordRing *ring)
{
	if (ring == NULL) return;
	atomic_store_explicit(&ring->read_index, 0u, memory_order_release);
	atomic_store_explicit(&ring->write_index, 0u, memory_order_release);
}

size_t aius_replay_word_ring_available(const AiusReplayWordRing *ring)
{
	if (ring == NULL) return 0u;
	uint32_t read = atomic_load_explicit(&ring->read_index, memory_order_acquire);
	uint32_t write = atomic_load_explicit(&ring->write_index, memory_order_acquire);
	return write >= read ? write - read : AIUS_REPLAY_WORD_RING_CAPACITY - read + write;
}

size_t aius_replay_word_ring_space(const AiusReplayWordRing *ring)
{
	return ring == NULL ? 0u : AIUS_REPLAY_WORD_RING_CAPACITY - 1u -
		aius_replay_word_ring_available(ring);
}

size_t aius_replay_word_ring_push(AiusReplayWordRing *ring,
	                              const uint16_t *words, size_t count)
{
	if (ring == NULL || words == NULL) return 0u;
	size_t pushed = 0u;
	while (pushed < count && aius_replay_word_ring_space(ring) != 0u) {
		uint32_t write = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
		ring->words[write] = words[pushed++];
		atomic_store_explicit(&ring->write_index, next_index(write), memory_order_release);
	}
	return pushed;
}

size_t aius_replay_pop_words(AiusReplayWordRing *ring, uint16_t *out,
	                         size_t capacity)
{
	if (ring == NULL || out == NULL) return 0u;
	size_t popped = 0u;
	while (popped < capacity && aius_replay_word_ring_available(ring) != 0u) {
		uint32_t read = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
		out[popped++] = ring->words[read];
		atomic_store_explicit(&ring->read_index, next_index(read), memory_order_release);
	}
	return popped;
}

uint16_t aius_replay_word_from_le(const uint8_t bytes[2])
{
	return bytes == NULL ? (uint16_t)0 :
		(uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

AiusStatus aius_replay_decode_le_words(const uint8_t *bytes, size_t byte_count,
	                                   uint16_t *out, size_t word_capacity,
	                                   size_t *word_count)
{
	if (word_count == NULL || (byte_count != 0u && (bytes == NULL || out == NULL)) ||
	    (byte_count & 1u) != 0u) return AIUS_INVALID_ARG;
	size_t words = byte_count / 2u;
	if (words > word_capacity) return AIUS_NO_SPACE;
	for (size_t i = 0; i < words; i++) out[i] = aius_replay_word_from_le(bytes + i * 2u);
	*word_count = words;
	return AIUS_OK;
}

bool aius_replay_id_is_valid(const char *id)
{
	if (id == NULL || id[0] == '\0') return false;
	size_t length = strlen(id);
	if (length > AIUS_REPLAY_MAX_ID_LENGTH) return false;
	for (size_t i = 0; i < length; i++) {
		unsigned char ch = (unsigned char)id[i];
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
	}
	return true;
}

void aius_replay_service_init(AiusReplayService *service)
{
	if (service == NULL) return;
	memset(service, 0, sizeof(*service));
	aius_replay_word_ring_init(&service->ring);
	service->watermark_words = AIUS_REPLAY_START_WATERMARK_WORDS;
	service->state = AIUS_REPLAY_STOPPED;
	service->source = AIUS_REPLAY_SOURCE_OPTICAL;
}

AiusStatus aius_replay_service_init_with_watermark(AiusReplayService *service,
	                                               uint32_t watermark_words)
{
	if (service == NULL || watermark_words == 0u ||
	    watermark_words >= AIUS_REPLAY_WORD_RING_CAPACITY) return AIUS_INVALID_ARG;
	aius_replay_service_init(service);
	service->watermark_words = watermark_words;
	return AIUS_OK;
}

static AiusStatus fill(AiusReplayService *service, AiusReplayStream *stream,
	                   size_t budget)
{
	while (budget != 0u && aius_replay_word_ring_space(&service->ring) != 0u &&
	       !stream->eof) {
		size_t bytes = 0u;
		uint32_t offset = stream->offset_bytes;
		size_t word_limit = budget;
		size_t ring_space = aius_replay_word_ring_space(&service->ring);
		if (word_limit > ring_space)
			word_limit = ring_space;
		size_t byte_capacity = word_limit * 2u;
		if (byte_capacity > AIUS_REPLAY_BYTE_BUFFER_SIZE)
			byte_capacity = AIUS_REPLAY_BYTE_BUFFER_SIZE;
		if (byte_capacity > sizeof(service->scratch_words))
			byte_capacity = sizeof(service->scratch_words);
		AiusStatus status = stream->reader.read(stream->reader.ctx, offset,
			service->byte_buffers[stream->buffer_index],
			byte_capacity, &bytes);
		if (status != AIUS_OK) return status;
		if (bytes == 0u) { stream->eof = true; break; }
		if ((bytes & 1u) != 0u || bytes > byte_capacity) return AIUS_MALFORMED;
		size_t words = bytes / 2u;
		for (size_t i = 0; i < words; i++)
			service->scratch_words[i] = aius_replay_word_from_le(
				service->byte_buffers[stream->buffer_index] + i * 2u);
		if (aius_replay_word_ring_push(&service->ring, service->scratch_words, words) != words)
			return AIUS_NO_SPACE;
		stream->offset_bytes += (uint32_t)bytes;
		budget -= words;
		stream->buffer_index ^= 1u;
		if (stream->offset_bytes >= service->file_bytes) stream->eof = true;
	}
	return AIUS_OK;
}

AiusStatus aius_replay_service_start(AiusReplayService *service, const char *id,
	                                 uint32_t file_bytes, bool loop,
	                                 const AiusReplayReader *reader)
{
	if (service == NULL || !aius_replay_id_is_valid(id) || file_bytes == 0u ||
	    (file_bytes & 1u) != 0u || reader == NULL || reader->open == NULL ||
	    reader->read == NULL || reader->close == NULL) return AIUS_INVALID_ARG;
	if (reader->open(reader->ctx, file_bytes) != AIUS_OK) return AIUS_MALFORMED;
	AiusReplayStream candidate = {.reader = *reader, .file_bytes = file_bytes, .open = true};
	service->file_bytes = file_bytes;
	service->state = AIUS_REPLAY_PREBUFFERING;
	service->source = AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY;
	aius_replay_word_ring_reset(&service->ring);
	AiusStatus status = fill(service, &candidate, AIUS_REPLAY_WORD_RING_CAPACITY - 1u);
	if (status != AIUS_OK || aius_replay_word_ring_available(&service->ring) < service->watermark_words) {
		reader->close(reader->ctx);
		service->state = AIUS_REPLAY_ERROR;
		service->source = AIUS_REPLAY_SOURCE_ERROR;
		return status == AIUS_OK ? AIUS_NO_SPACE : status;
	}
	strncpy(service->id, id, sizeof(service->id) - 1u);
	service->id[sizeof(service->id) - 1u] = '\0';
	service->active = candidate;
	service->loop = loop;
	service->state = AIUS_REPLAY_PLAYING;
	service->source = AIUS_REPLAY_SOURCE_REPLAY;
	return AIUS_OK;
}

AiusStatus aius_replay_service_refill(AiusReplayService *service, size_t budget_words)
{
	if (service == NULL || service->state == AIUS_REPLAY_STOPPED) return AIUS_INVALID_ARG;
	if (!service->active.open) return AIUS_INVALID_ARG;
	AiusStatus status = fill(service, &service->active, budget_words);
	if (status != AIUS_OK) { service->state = AIUS_REPLAY_ERROR; service->decoder.errors++; return status; }
	if (service->active.eof && service->loop) {
		service->active.offset_bytes = 0u;
		service->active.eof = false;
		service->active.loops_completed++;
	}
	if (service->active.eof && aius_replay_word_ring_available(&service->ring) == 0u)
		service->state = AIUS_REPLAY_EOF;
	return AIUS_OK;
}

size_t aius_replay_service_pop_words(AiusReplayService *service, uint16_t *out,
	                                size_t capacity)
{
	if (service == NULL) return 0u;
	size_t count = aius_replay_pop_words(&service->ring, out, capacity);
	if (service->state == AIUS_REPLAY_PLAYING && count < capacity) {
		service->underruns++;
		service->state = AIUS_REPLAY_UNDERRUN;
	}
	return count;
}

void aius_replay_service_stop(AiusReplayService *service)
{
	if (service == NULL) return;
	if (service->active.open && service->active.reader.close != NULL)
		service->active.reader.close(service->active.reader.ctx);
	aius_replay_word_ring_reset(&service->ring);
	service->active.open = false;
	service->state = AIUS_REPLAY_STOPPED;
	service->source = AIUS_REPLAY_SOURCE_OPTICAL;
}

AiusStatus aius_replay_service_status(const AiusReplayService *service,
	                                  AiusReplayStatus *out)
{
	if (service == NULL || out == NULL) return AIUS_INVALID_ARG;
	memset(out, 0, sizeof(*out));
	out->state = service->state; out->source = service->source;
	strncpy(out->id, service->id, sizeof(out->id) - 1u);
	out->file_bytes = service->file_bytes;
	out->offset_bytes = service->active.offset_bytes;
	out->duration_seconds = (double)service->file_bytes / AIUS_REPLAY_BYTES_PER_SECOND;
	out->loop = service->loop; out->loops_completed = service->active.loops_completed;
	out->queue_words = (uint32_t)aius_replay_word_ring_available(&service->ring);
	out->watermark_words = service->watermark_words; out->underruns = service->underruns;
	out->input_class = service->decoder.classification;
	out->decoder_errors = service->decoder.errors; out->decoder = service->decoder;
	return AIUS_OK;
}

void aius_replay_service_set_decoder_status(AiusReplayService *service,
                                        const AiusReplayDecoderStatus *status)
{
	if (service != NULL && status != NULL) service->decoder = *status;
}
