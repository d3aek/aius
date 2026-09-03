#ifndef AIUS_EVENT_RING_H
#define AIUS_EVENT_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The device event ring, pure and portable (aius-input-recovery Phase 1).
 *
 * WHAT IT HOLDS. Every AiusEvent optical_main.cpp records: state edges,
 * counter edges, rate and clock episodes. The entry's `type` byte is a wire
 * contract served verbatim by GET /api/events and the UDP GET_EVENTS reply
 * (aius_net.h); the values are allocated once in optical_main.cpp and never
 * renumbered, and this header takes no position on them.
 *
 * DEPTH VERSUS PAGE. Before this header the ring was eight entries deep and
 * GET /api/events served at most eight, so one number did two jobs and the
 * route's `cursor` could page nothing: a burst longer than eight events was
 * lost before any poller saw it. The two are now separate constants. The ring
 * depth is sized from the field read of 2026-08-18 (aius-input-recovery.md):
 * 134 ring events over a 49-hour boot in roughly 16 OSD sessions, about eight
 * per session with NO poller present (a state edge is recorded per status poll,
 * so a polled board adds two per source transition). One OSD transition
 * therefore costs at most four entries -- two state edges plus one rate episode
 * plus one clock fault or overrun edge -- and 32 entries hold eight such
 * transitions between two polls, which is the whole field-average session with
 * margin. The page sizes are unchanged wire facts: eight per HTTP page, four
 * per UDP reply (1 + 4 * 12 bytes in the payload).
 *
 * PLACEMENT. The ring stays a plain zero-initialised static in DTCM. OCRAM was
 * the alternative and is closed: the post-link memory verifier
 * (test/memory/verify_target.py) holds OCRAM free at the defect-26 128 KiB
 * heap floor with 32 B of margin, so a DMAMEM ring would have to be funded by
 * moving something else out first. Staying in .bss also keeps the reader's
 * staleness test exact -- a slot that has never been written reads sequence 0,
 * which no event carries -- where an uninitialised DMAMEM ring would need a
 * generation stamp to tell garbage from an old entry.
 *
 * PAGING. Sequences start at 1 and only ever rise; slot (s - 1) % depth holds
 * sequence s until a later sequence congruent to it overwrites the slot. A page
 * starts at the later of (cursor + 1) and the oldest retained sequence, and
 * `next` is the sequence a client passes back to continue: the last entry of a
 * full page, or the newest sequence when the page drained the ring. A client
 * that receives a full page polls again immediately; `overflows` still counts
 * every entry overwritten, so what a slow poller missed remains a reading. */

#ifndef AIUS_EVENT_RING_DEPTH
#define AIUS_EVENT_RING_DEPTH 32u
#endif

#define AIUS_EVENT_RING_HTTP_PAGE 8u
#define AIUS_EVENT_RING_UDP_PAGE 4u

typedef struct {
	uint32_t sequence;
	uint32_t uptime_ms;
	uint8_t type;
	uint8_t state;
	uint16_t detail;
} AiusEvent;

typedef struct {
	AiusEvent entries[AIUS_EVENT_RING_DEPTH];
	/* Lifetime count of recorded events; the newest entry's sequence. */
	uint32_t sequence;
	/* Entries overwritten because the ring was full when they were recorded. */
	uint32_t overflows;
} AiusEventRing;

static inline void aius_event_ring_reset(AiusEventRing *ring)
{
	uint32_t index;

	for (index = 0u; index < AIUS_EVENT_RING_DEPTH; index++) {
		ring->entries[index].sequence = 0u;
		ring->entries[index].uptime_ms = 0u;
		ring->entries[index].type = 0u;
		ring->entries[index].state = 0u;
		ring->entries[index].detail = 0u;
	}
	ring->sequence = 0u;
	ring->overflows = 0u;
}

/* Record one event. Field writes only: reachable from the AUDIO checkpoint. */
static inline void aius_event_ring_record(AiusEventRing *ring,
                                          uint32_t uptime_ms, uint8_t type,
                                          uint8_t state, uint16_t detail)
{
	AiusEvent *event = &ring->entries[ring->sequence % AIUS_EVENT_RING_DEPTH];

	if (ring->sequence >= AIUS_EVENT_RING_DEPTH)
		ring->overflows++;
	event->sequence = ++ring->sequence;
	event->uptime_ms = uptime_ms;
	event->type = type;
	event->state = state;
	event->detail = detail;
}

/* The oldest sequence the ring still holds; 1 while it has never wrapped. */
static inline uint32_t aius_event_ring_oldest(const AiusEventRing *ring)
{
	return ring->sequence > AIUS_EVENT_RING_DEPTH
		? ring->sequence - AIUS_EVENT_RING_DEPTH + 1u
		: 1u;
}

/* The first sequence a page after `cursor` should walk. Greater than
 * `ring->sequence` when there is nothing to serve, a cursor of UINT32_MAX
 * included (cursor + 1 would wrap to 0 and re-serve the whole ring). */
static inline uint32_t aius_event_ring_page_start(const AiusEventRing *ring,
                                                  uint32_t cursor)
{
	const uint32_t oldest = aius_event_ring_oldest(ring);

	if (cursor >= ring->sequence)
		return ring->sequence + 1u;
	return cursor >= oldest ? cursor + 1u : oldest;
}

/* The cursor a client passes back after a page of at most `page` entries
 * following `cursor`: the last sequence of a full page, else the newest. */
static inline uint32_t aius_event_ring_page_next(const AiusEventRing *ring,
                                                 uint32_t cursor, uint32_t page)
{
	const uint32_t start = aius_event_ring_page_start(ring, cursor);

	if (start > ring->sequence)
		return ring->sequence;
	return ring->sequence - start + 1u > page ? start + page - 1u
	                                          : ring->sequence;
}

/* The entry carrying `sequence`, or NULL when the ring no longer (or never)
 * holds it. The slot's own sequence is the staleness test. */
static inline const AiusEvent *aius_event_ring_at(const AiusEventRing *ring,
                                                  uint32_t sequence)
{
	const AiusEvent *event;

	if (sequence == 0u)
		return NULL;
	event = &ring->entries[(sequence - 1u) % AIUS_EVENT_RING_DEPTH];
	return event->sequence == sequence ? event : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* AIUS_EVENT_RING_H */
