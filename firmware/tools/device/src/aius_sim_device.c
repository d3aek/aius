/* In-process simulated capability device (capability-gate phase P5). Host-only
 * harness: a growable byte pipe wrapping the portable, no-heap device handler. */

#include "aius_sim_device.h"

#include <stdlib.h>
#include <string.h>

static void queue_reset(AiusSimQueue *q)
{
	q->len = 0;
	q->read = 0;
}

static void queue_append(AiusSimQueue *q, const uint8_t *data, size_t len)
{
	/* Compact fully-drained queue before growing. */
	if (q->read == q->len) {
		q->len = 0;
		q->read = 0;
	} else if (q->read > 0) {
		memmove(q->data, q->data + q->read, q->len - q->read);
		q->len -= q->read;
		q->read = 0;
	}
	if (q->len + len > q->cap) {
		size_t newcap = q->cap ? q->cap : 4096;
		while (newcap < q->len + len)
			newcap *= 2;
		uint8_t *g = realloc(q->data, newcap);
		if (g == NULL)
			return; /* drop on OOM; host will observe a short read */
		q->data = g;
		q->cap = newcap;
	}
	memcpy(q->data + q->len, data, len);
	q->len += len;
}

static size_t queue_read(AiusSimQueue *q, uint8_t *dst, size_t cap)
{
	size_t avail = q->len - q->read;
	size_t take = (cap < avail) ? cap : avail;
	memcpy(dst, q->data + q->read, take);
	q->read += take;
	if (q->read == q->len) {
		q->read = 0;
		q->len = 0;
	}
	return take;
}

/* Device emit sink: append the encoded response frame to the out queue. */
static void sim_emit(void *ctx, const uint8_t *frame, size_t len)
{
	AiusSimDevice *sim = (AiusSimDevice *)ctx;
	queue_append(&sim->out, frame, len);
}

AiusStatus aius_sim_device_init(AiusSimDevice *sim, void *arena_mem, size_t arena_bytes)
{
	memset(sim, 0, sizeof(*sim));
	if (aius_arena_init(&sim->arena, arena_mem, arena_bytes) != 0)
		return AIUS_INVALID_ARG;
	aius_cap_framer_init(&sim->in_framer);
	queue_reset(&sim->out);
	return aius_cap_device_init(&sim->dev, &sim->arena);
}

void aius_sim_device_destroy(AiusSimDevice *sim)
{
	aius_cap_device_destroy(&sim->dev);
	free(sim->out.data);
	sim->out.data = NULL;
	sim->out.cap = 0;
	queue_reset(&sim->out);
}

size_t aius_sim_device_write(AiusSimDevice *sim, const uint8_t *data, size_t len)
{
	size_t consumed = 0;
	while (len > 0) {
		size_t got = aius_cap_framer_feed(&sim->in_framer, data, len);
		data += got;
		len -= got;
		consumed += got;

		for (;;) {
			AiusCapFrame f;
			AiusCapFrameEvent ev = aius_cap_framer_next(&sim->in_framer, &f);
			if (ev == AIUS_CAP_FRAME_NONE)
				break;
			if (ev == AIUS_CAP_FRAME_OK)
				aius_cap_device_handle(&sim->dev, &f, sim_emit, sim);
			else
				aius_cap_device_handle_error(&sim->dev, &f, sim_emit, sim);
		}

		if (got == 0)
			break; /* no forward progress (should not happen once drained) */
	}
	return consumed;
}

size_t aius_sim_device_read(AiusSimDevice *sim, uint8_t *dst, size_t cap)
{
	return queue_read(&sim->out, dst, cap);
}

size_t aius_sim_device_available(const AiusSimDevice *sim)
{
	return sim->out.len - sim->out.read;
}
