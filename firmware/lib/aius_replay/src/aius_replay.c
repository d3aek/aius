#include "aius_replay.h"

#include <string.h>

/* Cold code -> FLASH, hot code -> ITCM, and the split is the point of this file.
 *
 * The ONLY functions that run per audio block are the pop path --
 * aius_replay_service_pop_words, aius_replay_pop_words,
 * aius_replay_word_ring_available and next_index -- so those four carry no
 * attribute and stay in ITCM, where the audio callback needs them.  Everything
 * else (start, planning, delivery, status, stop) runs in epoch or HTTP context
 * once per refill at most, so it carries FLASHMEM and leaves ITCM to the code
 * that earns it (docs/plans/aius-network-hardening.md, "Deciding principle").
 *
 * FLASHMEM is Teensyduino's __attribute__((section(".flashmem"))) from
 * <avr/pgmspace.h>; TEENSYDUINO is a project-wide compiler -D, so the native
 * host suites fall through to the no-op and this file keeps its portable,
 * Arduino-free compile.  The attribute must sit on each DEFINITION: GCC does not
 * merge a `section` attribute from a forward declaration, so a prototype-only
 * FLASHMEM silently leaves the code in ITCM. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

/* Hot: reached from aius_replay_pop_words on the audio block path. */
static uint32_t next_index(uint32_t index)
{
	return (index + 1u) % AIUS_REPLAY_WORD_RING_CAPACITY;
}

FLASHMEM void aius_replay_word_ring_init(AiusReplayWordRing *ring)
{
	if (ring == NULL) return;
	atomic_init(&ring->read_index, 0u);
	atomic_init(&ring->write_index, 0u);
}

FLASHMEM void aius_replay_word_ring_reset(AiusReplayWordRing *ring)
{
	if (ring == NULL) return;
	atomic_store_explicit(&ring->read_index, 0u, memory_order_release);
	atomic_store_explicit(&ring->write_index, 0u, memory_order_release);
}

/* Hot: the pop path's occupancy test. */
size_t aius_replay_word_ring_available(const AiusReplayWordRing *ring)
{
	if (ring == NULL) return 0u;
	uint32_t read = atomic_load_explicit(&ring->read_index, memory_order_acquire);
	uint32_t write = atomic_load_explicit(&ring->write_index, memory_order_acquire);
	return write >= read ? write - read : AIUS_REPLAY_WORD_RING_CAPACITY - read + write;
}

FLASHMEM size_t aius_replay_word_ring_space(const AiusReplayWordRing *ring)
{
	return ring == NULL ? 0u : AIUS_REPLAY_WORD_RING_CAPACITY - 1u -
		aius_replay_word_ring_available(ring);
}

FLASHMEM size_t aius_replay_word_ring_push(AiusReplayWordRing *ring,
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

/* Hot: the audio callback's consumption of the ring. */
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

FLASHMEM uint16_t aius_replay_word_from_le(const uint8_t bytes[2])
{
	return bytes == NULL ? (uint16_t)0 :
		(uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

FLASHMEM AiusStatus aius_replay_decode_le_words(const uint8_t *bytes, size_t byte_count,
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

FLASHMEM bool aius_replay_id_is_valid(const char *id)
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

FLASHMEM void aius_replay_service_init(AiusReplayService *service)
{
	if (service == NULL) return;
	memset(service, 0, sizeof(*service));
	aius_replay_word_ring_init(&service->ring);
	service->watermark_words = AIUS_REPLAY_START_WATERMARK_WORDS;
	service->state = AIUS_REPLAY_STOPPED;
	service->source = AIUS_REPLAY_SOURCE_OPTICAL;
}

FLASHMEM AiusStatus aius_replay_service_init_with_watermark(AiusReplayService *service,
	                                               uint32_t watermark_words)
{
	if (service == NULL || watermark_words == 0u ||
	    watermark_words >= AIUS_REPLAY_WORD_RING_CAPACITY) return AIUS_INVALID_ARG;
	aius_replay_service_init(service);
	service->watermark_words = watermark_words;
	return AIUS_OK;
}

FLASHMEM AiusStatus aius_replay_service_start(AiusReplayService *service,
	                                 const char *id, bool loop)
{
	if (service == NULL || !aius_replay_id_is_valid(id)) return AIUS_INVALID_ARG;
	strncpy(service->id, id, sizeof(service->id) - 1u);
	service->id[sizeof(service->id) - 1u] = '\0';
	service->loop = loop;
	service->file_bytes = 0u;
	service->offset_bytes = 0u;
	service->loops_completed = 0u;
	service->eof = false;
	service->started = true;
	/* Any refill planned for the previous selection is abandoned here; the
	 * platform cancels its in-flight job and delivers nothing from it, and a
	 * late delivery would be rejected by the offset check anyway. */
	service->refill_planned = false;
	service->refill_offset = 0u;
	service->refill_bytes = 0u;
	aius_replay_word_ring_reset(&service->ring);
	service->state = AIUS_REPLAY_PREBUFFERING;
	service->source = AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY;
	return AIUS_OK;
}

FLASHMEM AiusStatus aius_replay_service_set_file_bytes(AiusReplayService *service,
	                                          uint32_t file_bytes)
{
	if (service == NULL || !service->started || file_bytes == 0u ||
	    (file_bytes & 1u) != 0u || file_bytes < service->offset_bytes)
		return AIUS_INVALID_ARG;
	service->file_bytes = file_bytes;
	if (service->offset_bytes >= file_bytes) service->eof = true;
	return AIUS_OK;
}

FLASHMEM bool aius_replay_service_plan_refill(AiusReplayService *service,
	                                 size_t budget_words,
	                                 AiusReplayRefill *out)
{
	if (service == NULL || out == NULL || !service->started ||
	    service->refill_planned) return false;

	/* The file is exhausted and the ring has drained: this is the end of the
	 * replay, not an underrun.  Settled here because planning is the one
	 * service call that runs every epoch -- the pop path must not grow the
	 * branch, and nothing else is polled. */
	if (service->eof && aius_replay_word_ring_available(&service->ring) == 0u &&
	    service->state != AIUS_REPLAY_ERROR)
		service->state = AIUS_REPLAY_EOF;

	if (service->state == AIUS_REPLAY_ERROR ||
	    service->state == AIUS_REPLAY_EOF ||
	    service->state == AIUS_REPLAY_STOPPED || service->eof) return false;

	size_t space = aius_replay_word_ring_space(&service->ring);
	/* Refill in bursts rather than every epoch: above the watermark the ring
	 * already holds enough audio to cover a whole job, and the shared SD
	 * primitive is better left free for the platform's other consumers. */
	if (space == 0u ||
	    aius_replay_word_ring_available(&service->ring) >= service->watermark_words)
		return false;

	size_t words = budget_words < space ? budget_words : space;
	size_t bytes = words * 2u;
	if (bytes > AIUS_REPLAY_REFILL_MAX_BYTES) bytes = AIUS_REPLAY_REFILL_MAX_BYTES;
	if (service->file_bytes != 0u &&
	    bytes > (size_t)(service->file_bytes - service->offset_bytes))
		bytes = (size_t)(service->file_bytes - service->offset_bytes);
	bytes &= ~(size_t)1u;
	if (bytes == 0u) return false;

	service->refill_planned = true;
	service->refill_offset = service->offset_bytes;
	service->refill_bytes = (uint32_t)bytes;
	out->offset_bytes = service->offset_bytes;
	out->byte_count = (uint32_t)bytes;
	return true;
}

FLASHMEM AiusStatus aius_replay_service_deliver(AiusReplayService *service,
	                                   uint32_t offset_bytes,
	                                   const uint8_t *bytes,
	                                   size_t byte_count)
{
	if (service == NULL || bytes == NULL || byte_count == 0u ||
	    !service->started || !service->refill_planned) return AIUS_INVALID_ARG;
	if (offset_bytes != service->offset_bytes) return AIUS_INVALID_ARG;
	if ((byte_count & 1u) != 0u) return AIUS_MALFORMED;
	if (byte_count > sizeof(service->scratch_words) ||
	    offset_bytes + byte_count >
		    service->refill_offset + service->refill_bytes)
		return AIUS_NO_SPACE;

	size_t words = byte_count / 2u;
	if (aius_replay_word_ring_space(&service->ring) < words) return AIUS_NO_SPACE;
	for (size_t i = 0; i < words; i++)
		service->scratch_words[i] = aius_replay_word_from_le(bytes + i * 2u);
	if (aius_replay_word_ring_push(&service->ring, service->scratch_words,
	                               words) != words)
		return AIUS_NO_SPACE;

	service->offset_bytes += (uint32_t)byte_count;
	if (service->file_bytes != 0u && service->offset_bytes >= service->file_bytes)
		service->eof = true;
	/* UNDERRUN re-arms alongside PREBUFFERING, and that is a fix rather than a
	 * tidy-up.  Only a PLAYING service counts a short pop
	 * (aius_replay_service_pop_words), and UNDERRUN used to be a one-way door:
	 * a recovered ring never went back to PLAYING, so the SECOND starvation and
	 * every one after it was uncountable.  `underruns` then reads as a frozen
	 * number while the audio is visibly starving -- which is what the first live
	 * measurement saw, and it made the one counter that should have named this
	 * defect say nothing at all.  Refilled past the watermark, the service is
	 * playing again by every other measure; say so, and let the next starvation
	 * count. */
	if ((service->state == AIUS_REPLAY_PREBUFFERING ||
	     service->state == AIUS_REPLAY_UNDERRUN) &&
	    aius_replay_word_ring_available(&service->ring) >= service->watermark_words) {
		service->state = AIUS_REPLAY_PLAYING;
		service->source = AIUS_REPLAY_SOURCE_REPLAY;
	}
	return AIUS_OK;
}

FLASHMEM void aius_replay_service_abandon_refill(AiusReplayService *service)
{
	if (service == NULL) return;
	service->refill_planned = false;
	service->refill_offset = 0u;
	service->refill_bytes = 0u;
}

FLASHMEM void aius_replay_service_refill_complete(AiusReplayService *service, bool ok)
{
	if (service == NULL || !service->started || !service->refill_planned) return;
	const uint32_t delivered = service->offset_bytes - service->refill_offset;

	service->refill_planned = false;
	if (!ok) {
		if (service->decoder.errors != UINT32_MAX) service->decoder.errors++;
		service->state = AIUS_REPLAY_ERROR;
		service->source = AIUS_REPLAY_SOURCE_ERROR;
		return;
	}
	/* A job that returned less than it asked for read past the end of the
	 * file, which is how EOF is found when the size is not yet published. */
	if (delivered < service->refill_bytes) service->eof = true;
	if (service->eof && service->loop) {
		service->offset_bytes = 0u;
		service->eof = false;
		if (service->loops_completed != UINT32_MAX) service->loops_completed++;
	}
}

/* Hot: one call per audio block, and the only replay function on that path. */
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

FLASHMEM void aius_replay_service_stop(AiusReplayService *service)
{
	if (service == NULL) return;
	aius_replay_word_ring_reset(&service->ring);
	service->started = false;
	service->refill_planned = false;
	service->eof = false;
	service->state = AIUS_REPLAY_STOPPED;
	service->source = AIUS_REPLAY_SOURCE_OPTICAL;
}

FLASHMEM AiusStatus aius_replay_service_status(const AiusReplayService *service,
	                                  AiusReplayStatus *out)
{
	if (service == NULL || out == NULL) return AIUS_INVALID_ARG;
	memset(out, 0, sizeof(*out));
	out->state = service->state; out->source = service->source;
	/* Bounded copy rather than strncpy: `out` is already zeroed, so the
	 * terminator is in place, and strncpy of exactly sizeof(id)-1 bytes from
	 * an equally sized array is what -Wstringop-truncation reports. */
	for (size_t i = 0; i < sizeof(out->id) - 1u && service->id[i] != '\0'; i++)
		out->id[i] = service->id[i];
	out->file_bytes = service->file_bytes;
	out->offset_bytes = service->offset_bytes;
	out->duration_seconds = (double)service->file_bytes / AIUS_REPLAY_BYTES_PER_SECOND;
	out->loop = service->loop; out->loops_completed = service->loops_completed;
	out->queue_words = (uint32_t)aius_replay_word_ring_available(&service->ring);
	out->watermark_words = service->watermark_words; out->underruns = service->underruns;
	out->input_class = service->decoder.classification;
	out->decoder_errors = service->decoder.errors; out->decoder = service->decoder;
	return AIUS_OK;
}

FLASHMEM void aius_replay_service_set_decoder_status(AiusReplayService *service,
                                        const AiusReplayDecoderStatus *status)
{
	if (service != NULL && status != NULL) service->decoder = *status;
}
