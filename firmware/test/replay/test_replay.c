/* Portable replay service: the ring, the byte/word format, and the plan/deliver
 * refill seam that replaced the synchronous AiusReplayReader in Phase 14c.
 *
 * The seam under test is deliberately handle-free: the service asks for a byte
 * RANGE, someone else performs the read, and the bytes come back. These tests
 * therefore never open anything -- the fake platform below is a byte array and a
 * chunk size, which is exactly what the target's bounded SD job is from the
 * service's point of view. */

#include "aius_replay.h"

#include <assert.h>
#include <string.h>

static void test_ring(void)
{
	AiusReplayWordRing ring;
	uint16_t input[] = {0xF872, 0x4E1F, 1, 2};
	uint16_t output[4] = {};
	aius_replay_word_ring_init(&ring);
	assert(aius_replay_word_ring_push(&ring, input, 4) == 4);
	assert(aius_replay_pop_words(&ring, output, 4) == 4);
	assert(memcmp(input, output, sizeof(input)) == 0);
}

static void test_format(void)
{
	const uint8_t bytes[] = {0x72, 0xF8, 0x1F, 0x4E};
	uint16_t words[2] = {};
	size_t count = 0;
	assert(aius_replay_decode_le_words(bytes, sizeof(bytes), words, 2, &count) == AIUS_OK);
	assert(count == 2 && words[0] == 0xF872 && words[1] == 0x4E1F);
	assert(aius_replay_decode_le_words(bytes, 3, words, 2, &count) == AIUS_INVALID_ARG);
	assert(aius_replay_id_is_valid("trailer-51"));
	assert(!aius_replay_id_is_valid("bad.id"));
}

/* Every bound below is spelled as the constant it comes from rather than as a
 * literal: this seam is sized by latency (see aius_replay.h), so the sizes move
 * when the measurement says they must, and a test that hard-codes them turns a
 * deliberate resize into a false failure.
 *
 * Each fixture's service is `static` for the same reason the target's is
 * DMAMEM: the ring is 128 KB and does not belong on a stack. */
#define WORDS(bytes) ((bytes) / 2u)
/* Large enough that the file is never the thing that ends a test early: a whole
 * ring plus a whole refill. */
#define TEST_FILE_BYTES \
	(AIUS_REPLAY_WORD_RING_CAPACITY * 2u + AIUS_REPLAY_REFILL_MAX_BYTES)
/* The refill budget the platform passes (AiusRuntimeBudget.max_rx_bytes at the
 * replay checkpoint, in words). Deliberately generous, so what bounds a plan is
 * AIUS_REPLAY_REFILL_MAX_BYTES and the ring, never this. */
#define TEST_BUDGET_WORDS AIUS_REPLAY_REFILL_MAX_BYTES

/* The stand-in for the target's bounded SD job: a file of `size` bytes served
 * AIUS_REPLAY_DELIVER_MAX_BYTES at a time, clamped at the end of the file
 * exactly as AIUS_SD_JOB_REPLAY_REFILL clamps its range to the real size.
 * Returns the number of bytes delivered for the whole planned range. */
static uint8_t g_file[TEST_FILE_BYTES];

static uint32_t serve_refill(AiusReplayService *service,
	                         const AiusReplayRefill *refill, uint32_t size)
{
	uint32_t served = 0u;
	while (served < refill->byte_count) {
		uint32_t offset = refill->offset_bytes + served;
		uint32_t chunk = refill->byte_count - served;
		if (chunk > AIUS_REPLAY_DELIVER_MAX_BYTES)
			chunk = AIUS_REPLAY_DELIVER_MAX_BYTES;
		if (offset >= size) break;
		if (offset + chunk > size) chunk = size - offset;
		assert(aius_replay_service_deliver(service, offset, g_file + offset,
		                                   chunk) == AIUS_OK);
		served += chunk;
	}
	return served;
}

static void fill_file(void)
{
	for (size_t i = 0; i < sizeof(g_file); i++)
		g_file[i] = (uint8_t)(i & 0xFFu);
}

/* Drives planned refills until the service stops asking, which is exactly the
 * platform's steady state: the ring at or above its watermark and the shared SD
 * primitive left free. */
static void fill_to_watermark(AiusReplayService *service)
{
	AiusReplayRefill refill = {};
	int rounds = 0;

	while (aius_replay_service_plan_refill(service, TEST_BUDGET_WORDS,
	                                       &refill)) {
		assert(serve_refill(service, &refill, sizeof(g_file)) ==
		       refill.byte_count);
		aius_replay_service_refill_complete(service, true);
		/* A generous multiple of the two rounds the current sizing takes, so
		 * a deliberate resize does not have to touch this number. */
		assert(++rounds < 32);
	}
}

/* Start publishes PREBUFFERING and touches nothing: no reader, no handle, and a
 * ring that is still empty when it returns. */
static void test_start_is_memory_only(void)
{
	static AiusReplayService service;
	AiusReplayStatus status = {};
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "prebuffer", false) == AIUS_OK);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PREBUFFERING);
	assert(status.source == AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY);
	assert(status.queue_words == 0u);
	assert(strcmp(status.id, "prebuffer") == 0);
	assert(aius_replay_service_start(&service, "bad.id", false) ==
	       AIUS_INVALID_ARG);
}

/* Planned refills carry the ring to the watermark, and only then does the
 * service publish PLAYING / SOURCE_REPLAY -- which is what lets the integration
 * keep its live input until the replacement is genuinely ready. */
static void test_refill_reaches_watermark(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};
	AiusReplayStatus status = {};
	int rounds = 0;

	fill_file();
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "watermark", false) == AIUS_OK);
	assert(aius_replay_service_set_file_bytes(&service, sizeof(g_file)) == AIUS_OK);

	while (aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill)) {
		assert(refill.byte_count != 0u &&
		       refill.byte_count <= AIUS_REPLAY_REFILL_MAX_BYTES);
		assert((refill.byte_count & 1u) == 0u);
		assert(serve_refill(&service, &refill, sizeof(g_file)) ==
		       refill.byte_count);
		aius_replay_service_refill_complete(&service, true);
		assert(++rounds < 8);
	}
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PLAYING);
	assert(status.source == AIUS_REPLAY_SOURCE_REPLAY);
	assert(status.queue_words >= AIUS_REPLAY_START_WATERMARK_WORDS);
	assert(status.offset_bytes == status.queue_words * 2u);
	assert(status.file_bytes == sizeof(g_file));

	/* The burst gate: above the watermark no refill is wanted, so the shared
	 * platform primitive is left free. */
	assert(!aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));

	/* The words that reached the ring are the file's, little-endian. */
	uint16_t words[4] = {};
	assert(aius_replay_service_pop_words(&service, words, 4) == 4);
	assert(words[0] == aius_replay_word_from_le(g_file));
	assert(words[3] == aius_replay_word_from_le(g_file + 6));
}

/* A chunk that is not at the service's read position is refused rather than
 * decoded into the stream: the reordered or stale delivery a shared, restartable
 * platform job can produce must never reach the audio ring. */
static void test_deliver_rejects_out_of_position(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};

	fill_file();
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "position", false) == AIUS_OK);
	/* Nothing is planned yet, so nothing may be delivered. */
	assert(aius_replay_service_deliver(&service, 0u, g_file, 512u) ==
	       AIUS_INVALID_ARG);
	assert(aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	assert(aius_replay_service_deliver(&service, 512u, g_file, 512u) ==
	       AIUS_INVALID_ARG);
	assert(aius_replay_service_deliver(&service, 0u, g_file, 511u) ==
	       AIUS_MALFORMED);
	assert(aius_replay_service_deliver(&service, 0u, g_file,
	                                   AIUS_REPLAY_DELIVER_MAX_BYTES + 2u) ==
	       AIUS_NO_SPACE);
	assert(aius_replay_service_deliver(&service, 0u, g_file, 512u) == AIUS_OK);
	assert(aius_replay_service_deliver(&service, 512u, g_file + 512, 512u) ==
	       AIUS_OK);
}

/* A refill that comes back short is the end of the file -- the only EOF signal
 * available before the platform has published a size. */
static void test_short_refill_is_eof(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};
	AiusReplayStatus status = {};
	uint16_t words[256] = {};

	fill_file();
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "short", false) == AIUS_OK);
	assert(aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	assert(refill.byte_count == AIUS_REPLAY_REFILL_MAX_BYTES);
	/* 1,024 of the 4,096 planned bytes: the file ended. */
	assert(serve_refill(&service, &refill, 1024u) == 1024u);
	aius_replay_service_refill_complete(&service, true);

	/* Nothing further is asked for, and the state is not yet EOF because the
	 * ring still holds audio. */
	assert(!aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PREBUFFERING && status.queue_words == 512u);

	/* Drained: the replay is over, and that is EOF rather than an underrun. */
	assert(aius_replay_service_pop_words(&service, words, 256u) == 256u);
	assert(aius_replay_service_pop_words(&service, words, 256u) == 256u);
	assert(!aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_EOF);
	assert(status.underruns == 0u);
}

/* Looping wraps the read position instead of ending, and counts the lap. */
static void test_loop_wraps_the_position(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};
	AiusReplayStatus status = {};

	fill_file();
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "looper", true) == AIUS_OK);
	assert(aius_replay_service_set_file_bytes(&service, 1024u) == AIUS_OK);
	assert(aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	/* The plan is clamped to the published size. */
	assert(refill.offset_bytes == 0u && refill.byte_count == 1024u);
	assert(serve_refill(&service, &refill, 1024u) == 1024u);
	aius_replay_service_refill_complete(&service, true);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.loops_completed == 1u);
	assert(status.offset_bytes == 0u);
	assert(aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	assert(refill.offset_bytes == 0u);
}

/* A failed platform read is an ERROR the status surface reports, and one decoder
 * error -- the accounting the pre-14c refill kept. */
static void test_refill_failure_is_error(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};
	AiusReplayStatus status = {};

	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "broken", false) == AIUS_OK);
	assert(aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	aius_replay_service_refill_complete(&service, false);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_ERROR);
	assert(status.source == AIUS_REPLAY_SOURCE_ERROR);
	assert(status.decoder_errors == 1u);
	assert(!aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
}

/* Every refill planned while the ring is below the watermark is a WHOLE refill.
 *
 * This is the sizing invariant the first live measurement bought. The platform
 * reads a planned range in slices of its transfer buffer and spends one epoch
 * per slice hand-over, so a plan the ring silently shrank costs the same epoch
 * overhead for fewer bytes -- which is the 65 %-of-realtime refill, in one
 * sentence. Asserted as equality rather than "at most" for that reason. */
static void test_every_plan_below_the_watermark_is_full_size(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};
	int rounds = 0;

	fill_file();
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "fullsize", false) == AIUS_OK);
	while (aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS,
	                                       &refill)) {
		assert(refill.byte_count == AIUS_REPLAY_REFILL_MAX_BYTES);
		assert(serve_refill(&service, &refill, sizeof(g_file)) ==
		       refill.byte_count);
		aius_replay_service_refill_complete(&service, true);
		assert(++rounds < 32);
	}
	assert(rounds >= 2);
	/* And the watermark holds at least one refill's worth of audio, so the
	 * ring cannot be drained by the round trip that refills it. */
	assert(WORDS(AIUS_REPLAY_REFILL_MAX_BYTES) <=
	       AIUS_REPLAY_START_WATERMARK_WORDS);
}

/* Underrun accounting is unchanged by the redesign: a short pop while playing
 * counts one underrun and publishes UNDERRUN. What IS new is that the count
 * keeps working afterwards. */
static void test_underrun_accounting(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};
	AiusReplayStatus status = {};
	/* Static, not automatic: a whole ring is 32 KB and this seam's own budget
	 * doc is about not spending the stack. */
	static uint16_t words[AIUS_REPLAY_WORD_RING_CAPACITY];
	uint32_t queued;

	fill_file();
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "underrun", false) == AIUS_OK);
	fill_to_watermark(&service);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PLAYING);
	queued = status.queue_words;
	assert(queued >= AIUS_REPLAY_START_WATERMARK_WORDS);

	/* An exact pop is not an underrun. */
	assert(aius_replay_service_pop_words(&service, words, queued) == queued);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PLAYING && status.underruns == 0u);

	/* A pop against the drained ring is. */
	assert(aius_replay_service_pop_words(&service, words, 1u) == 0u);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_UNDERRUN);
	assert(status.underruns == 1u);
	/* Refill resumes for an underrunning replay rather than giving up. */
	assert(aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	aius_replay_service_abandon_refill(&service);

	/* THE REGRESSION THIS COVERS. UNDERRUN used to be a one-way door: nothing
	 * promoted it back to PLAYING, and only a PLAYING service counts a short
	 * pop, so the second starvation and every one after it was uncountable.
	 * `underruns` then reads as a frozen number while the audio is visibly
	 * starving -- which is what the first live run saw, and it silenced the one
	 * counter that should have named the defect. A ring refilled past the
	 * watermark is playing again, and the next starvation counts. */
	fill_to_watermark(&service);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PLAYING);
	queued = status.queue_words;
	assert(aius_replay_service_pop_words(&service, words, queued + 1u) ==
	       queued);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_UNDERRUN);
	assert(status.underruns == 2u);
}

/* Start selects a new replay over a live one without waiting for anything, and
 * stop returns the service to optical. */
static void test_restart_and_stop(void)
{
	static AiusReplayService service;
	AiusReplayRefill refill = {};
	AiusReplayStatus status = {};

	fill_file();
	aius_replay_service_init(&service);
	assert(aius_replay_service_start(&service, "first", false) == AIUS_OK);
	assert(aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
	assert(serve_refill(&service, &refill, sizeof(g_file)) == refill.byte_count);
	aius_replay_service_refill_complete(&service, true);

	assert(aius_replay_service_start(&service, "second", false) == AIUS_OK);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_PREBUFFERING);
	assert(status.queue_words == 0u && status.offset_bytes == 0u);
	assert(strcmp(status.id, "second") == 0);
	/* The abandoned plan cannot deliver into the replacement. */
	assert(aius_replay_service_deliver(&service, 0u, g_file, 512u) ==
	       AIUS_INVALID_ARG);

	aius_replay_service_stop(&service);
	assert(aius_replay_service_status(&service, &status) == AIUS_OK);
	assert(status.state == AIUS_REPLAY_STOPPED);
	assert(status.source == AIUS_REPLAY_SOURCE_OPTICAL);
	assert(!aius_replay_service_plan_refill(&service, TEST_BUDGET_WORDS, &refill));
}

int main(void)
{
	test_ring();
	test_format();
	test_start_is_memory_only();
	test_refill_reaches_watermark();
	test_deliver_rejects_out_of_position();
	test_short_refill_is_eof();
	test_loop_wraps_the_position();
	test_refill_failure_is_error();
	test_every_plan_below_the_watermark_is_full_size();
	test_underrun_accounting();
	test_restart_and_stop();
	return 0;
}
