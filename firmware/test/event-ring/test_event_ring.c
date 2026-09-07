/* The device event ring (aius-input-recovery Phase 1).
 *
 * The rows are the two facts the plan's Phase 1 needs proven on the host rather
 * than assumed: an OSD-navigation burst at the plan's own event rate does not
 * overflow a ring of AIUS_EVENT_RING_DEPTH, and a client paging by `cursor` in
 * AIUS_EVENT_RING_HTTP_PAGE-sized pages recovers every retained entry exactly
 * once, in order, whether its cursor is current, behind the ring, or stale from
 * a previous boot. The overwrite rows pin the accounting the field read relies
 * on: `overflows` counts every entry lost, and a lost sequence answers NULL
 * rather than the entry that took its slot. */

#include "aius_event_ring.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

/* One OSD source transition as the plan costs it: two state edges (unlocked,
 * then the relocked class), one rate episode, one clock fault or overrun edge.
 * Type values are opaque to the ring; these are optical_main.cpp's. */
static void transition(AiusEventRing *ring, uint32_t at_ms, uint16_t rate_hz)
{
	aius_event_ring_record(ring, at_ms, 1u, 2u, 0u);
	aius_event_ring_record(ring, at_ms + 30u, 1u, 3u, 0u);
	aius_event_ring_record(ring, at_ms + 1200u, 12u, 3u, rate_hz);
	aius_event_ring_record(ring, at_ms + 1250u, 13u, 3u, 1u);
}

/* Walk pages from `cursor` the way GET /api/events serves them, collecting
 * every sequence seen; returns how many. Stops when a page drains the ring. */
static uint32_t page_all(const AiusEventRing *ring, uint32_t cursor,
                         uint32_t page, uint32_t *seen, uint32_t seen_capacity)
{
	uint32_t total = 0u;

	for (;;) {
		uint32_t count = 0u;
		uint32_t sequence;
		const uint32_t next =
			aius_event_ring_page_next(ring, cursor, page);
		for (sequence = aius_event_ring_page_start(ring, cursor);
		     sequence <= ring->sequence && count < page; sequence++) {
			const AiusEvent *event =
				aius_event_ring_at(ring, sequence);
			if (event == NULL)
				continue;
			assert(total < seen_capacity);
			seen[total++] = event->sequence;
			count++;
		}
		/* `next` is the last sequence emitted when the page was full, and
		 * the newest sequence when the page drained the ring. */
		if (count == page && sequence <= ring->sequence)
			assert(next == seen[total - 1u]);
		else
			assert(next == ring->sequence);
		if (next == ring->sequence || next == cursor)
			return total;
		cursor = next;
	}
}

int main(void)
{
	static AiusEventRing ring;
	uint32_t seen[4u * AIUS_EVENT_RING_DEPTH];

	/* The depth is a design constant, not the page: the page stays the
	 * eight-entry wire fact and the ring is deeper than it. */
	assert(AIUS_EVENT_RING_DEPTH >= 32u);
	assert(AIUS_EVENT_RING_DEPTH > AIUS_EVENT_RING_HTTP_PAGE);
	assert(AIUS_EVENT_RING_HTTP_PAGE == 8u);
	assert(AIUS_EVENT_RING_UDP_PAGE == 4u);
	assert(sizeof(AiusEvent) == 12u);

	/* Empty ring: nothing to serve, nothing overflowed, `next` is 0. */
	{
		aius_event_ring_reset(&ring);
		assert(ring.sequence == 0u);
		assert(ring.overflows == 0u);
		assert(aius_event_ring_oldest(&ring) == 1u);
		assert(aius_event_ring_page_start(&ring, 0u) == 1u);
		assert(aius_event_ring_page_next(&ring, 0u,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 0u);
		assert(aius_event_ring_at(&ring, 0u) == NULL);
		assert(aius_event_ring_at(&ring, 1u) == NULL);
		assert(page_all(&ring, 0u, AIUS_EVENT_RING_HTTP_PAGE, seen,
		                sizeof(seen) / sizeof(seen[0])) == 0u);
	}

	/* The ring-depth row: an OSD burst of eight source transitions at the
	 * plan's four events each lands 32 events between two polls and loses
	 * none of them. Every entry is retrievable by its own sequence. */
	{
		uint32_t index;
		aius_event_ring_reset(&ring);
		for (index = 0u; index < 8u; index++)
			transition(&ring, 1000u + index * 2000u,
			           (uint16_t)(index & 1u ? 44100u : 48000u));
		assert(ring.sequence == 32u);
		assert(ring.overflows == 0u);
		assert(aius_event_ring_oldest(&ring) == 1u);
		for (index = 1u; index <= 32u; index++) {
			const AiusEvent *event = aius_event_ring_at(&ring, index);
			assert(event != NULL);
			assert(event->sequence == index);
		}
		/* The first transition's entries carry what was recorded. */
		assert(aius_event_ring_at(&ring, 1u)->uptime_ms == 1000u);
		assert(aius_event_ring_at(&ring, 1u)->type == 1u);
		assert(aius_event_ring_at(&ring, 1u)->state == 2u);
		assert(aius_event_ring_at(&ring, 3u)->type == 12u);
		assert(aius_event_ring_at(&ring, 3u)->detail == 48000u);
		assert(aius_event_ring_at(&ring, 4u)->type == 13u);

		/* A poller behind the whole burst pages it in four full pages
		 * of eight, in order, with no gap and no duplicate. */
		const uint32_t total = page_all(&ring, 0u,
		                                AIUS_EVENT_RING_HTTP_PAGE, seen,
		                                sizeof(seen) / sizeof(seen[0]));
		assert(total == 32u);
		for (index = 0u; index < total; index++)
			assert(seen[index] == index + 1u);
		/* The first page's `next` is its last entry, not the newest. */
		assert(aius_event_ring_page_next(&ring, 0u,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 8u);
		assert(aius_event_ring_page_next(&ring, 8u,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 16u);
		assert(aius_event_ring_page_next(&ring, 24u,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 32u);
		/* A short final page drains to the newest sequence. */
		assert(aius_event_ring_page_next(&ring, 30u,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 32u);
		/* The UDP page is four entries; same chaining, eight replies. */
		assert(page_all(&ring, 0u, AIUS_EVENT_RING_UDP_PAGE, seen,
		                sizeof(seen) / sizeof(seen[0])) == 32u);
		assert(aius_event_ring_page_next(&ring, 0u,
		                                 AIUS_EVENT_RING_UDP_PAGE) == 4u);
	}

	/* A cursor mid-ring starts the page right after it. */
	{
		assert(aius_event_ring_page_start(&ring, 10u) == 11u);
		assert(page_all(&ring, 10u, AIUS_EVENT_RING_HTTP_PAGE, seen,
		                sizeof(seen) / sizeof(seen[0])) == 22u);
		assert(seen[0] == 11u);
		assert(seen[21] == 32u);
	}

	/* A current cursor serves an empty page and `next` stays put. */
	{
		assert(aius_event_ring_page_start(&ring, 32u) == 33u);
		assert(aius_event_ring_page_next(&ring, 32u,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 32u);
		assert(page_all(&ring, 32u, AIUS_EVENT_RING_HTTP_PAGE, seen,
		                sizeof(seen) / sizeof(seen[0])) == 0u);
	}

	/* A stale cursor from a previous boot (ahead of this boot's sequence)
	 * serves nothing and hands back the newest sequence, so the client
	 * re-bases rather than stalling forever. Same answer as before paging. */
	{
		/* Past the newest sequence, so the walk is empty by construction
		 * (and cannot wrap, see the UINT32_MAX row). */
		assert(aius_event_ring_page_start(&ring, 500u) == 33u);
		assert(aius_event_ring_page_next(&ring, 500u,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 32u);
		assert(page_all(&ring, 500u, AIUS_EVENT_RING_HTTP_PAGE, seen,
		                sizeof(seen) / sizeof(seen[0])) == 0u);
	}

	/* The 33rd event is the first overflow: it takes slot 0, sequence 1 is
	 * gone and answers NULL rather than the newcomer, and the oldest
	 * retained sequence moves to 2. A poller behind the ring starts there. */
	{
		aius_event_ring_record(&ring, 20000u, 4u, 3u, 7u);
		assert(ring.sequence == 33u);
		assert(ring.overflows == 1u);
		assert(aius_event_ring_at(&ring, 1u) == NULL);
		assert(aius_event_ring_at(&ring, 33u) != NULL);
		assert(aius_event_ring_at(&ring, 33u)->detail == 7u);
		assert(aius_event_ring_oldest(&ring) == 2u);
		assert(aius_event_ring_page_start(&ring, 0u) == 2u);
		assert(aius_event_ring_page_start(&ring, 1u) == 2u);
		assert(aius_event_ring_page_start(&ring, 2u) == 3u);
		const uint32_t total = page_all(&ring, 0u,
		                                AIUS_EVENT_RING_HTTP_PAGE, seen,
		                                sizeof(seen) / sizeof(seen[0]));
		assert(total == 32u);
		assert(seen[0] == 2u);
		assert(seen[31] == 33u);
	}

	/* A burst twice the depth loses exactly the excess, and the accounting
	 * says so: the reader sees the newest 32 and `overflows` the 32 lost. */
	{
		uint32_t index;
		aius_event_ring_reset(&ring);
		for (index = 0u; index < 16u; index++)
			transition(&ring, index * 1500u, 48000u);
		assert(ring.sequence == 64u);
		assert(ring.overflows == 32u);
		assert(aius_event_ring_oldest(&ring) == 33u);
		const uint32_t total = page_all(&ring, 0u,
		                                AIUS_EVENT_RING_HTTP_PAGE, seen,
		                                sizeof(seen) / sizeof(seen[0]));
		assert(total == 32u);
		assert(seen[0] == 33u);
		assert(seen[31] == 64u);
	}

	/* The output-glitch pair (glitch-timestamping 2026-09-07). Types 14 and
	 * 15 are posted from the AUDIO producer rather than the epoch collapser,
	 * so a single service pass can land both: the trim the governor made and
	 * the underrun episode that ended in the same block. Both must retain
	 * their own type and detail -- an episode's frames saturated at the
	 * u16 ceiling, a trim's dropped frames verbatim -- and neither may
	 * disturb the state byte the pollers read. */
	{
		aius_event_ring_reset(&ring);
		aius_event_ring_record(&ring, 61000u, 15u, 3u, 1920u);
		aius_event_ring_record(&ring, 61000u, 14u, 3u, 65535u);
		assert(ring.sequence == 2u);
		assert(ring.overflows == 0u);
		assert(aius_event_ring_at(&ring, 1u)->type == 15u);
		assert(aius_event_ring_at(&ring, 1u)->detail == 1920u);
		assert(aius_event_ring_at(&ring, 1u)->state == 3u);
		assert(aius_event_ring_at(&ring, 2u)->type == 14u);
		assert(aius_event_ring_at(&ring, 2u)->detail == 65535u);
		/* Same millisecond, distinct sequences: the ordering a reader
		 * needs when a trim and an episode share one block. */
		assert(aius_event_ring_at(&ring, 1u)->uptime_ms ==
		       aius_event_ring_at(&ring, 2u)->uptime_ms);
		assert(page_all(&ring, 0u, AIUS_EVENT_RING_HTTP_PAGE, seen,
		                sizeof(seen) / sizeof(seen[0])) == 2u);
		assert(seen[0] == 1u && seen[1] == 2u);
	}

	/* A reset returns the ring to the empty state, every slot included. */
	{
		aius_event_ring_reset(&ring);
		assert(ring.sequence == 0u);
		assert(ring.overflows == 0u);
		uint32_t index;
		for (index = 0u; index < AIUS_EVENT_RING_DEPTH; index++)
			assert(ring.entries[index].sequence == 0u);
	}

	/* cursor=4294967295: cursor + 1 wraps to 0, which would walk the whole
	 * ring from sequence 1 and hand back a `next` of 8 forever. The page
	 * start must be past the newest sequence like any other stale cursor. */
	{
		uint32_t index;
		aius_event_ring_reset(&ring);
		for (index = 0u; index < 3u; index++)
			transition(&ring, index * 1500u, 48000u);
		assert(ring.sequence == 12u);
		assert(aius_event_ring_page_start(&ring, UINT32_MAX) == 13u);
		assert(aius_event_ring_page_next(&ring, UINT32_MAX,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 12u);
		assert(page_all(&ring, UINT32_MAX, AIUS_EVENT_RING_HTTP_PAGE, seen,
		                sizeof(seen) / sizeof(seen[0])) == 0u);
		/* The empty ring answers the same way. */
		aius_event_ring_reset(&ring);
		assert(aius_event_ring_page_start(&ring, UINT32_MAX) == 1u);
		assert(aius_event_ring_page_next(&ring, UINT32_MAX,
		                                 AIUS_EVENT_RING_HTTP_PAGE) == 0u);
	}

	puts("Aius event ring test (depth, overflow accounting, cursor paging)");
	puts("RESULT: all checks passed");
	return 0;
}
