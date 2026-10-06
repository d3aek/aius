#include "aius_codec.h"

#include <assert.h>
#include <stdio.h>

/* Compile-time contract checks: this suite exists to prove the portable ABI
 * headers compile cleanly as C and hold their documented invariants before any
 * implementation exists. */

_Static_assert(AIUS_CODEC_ABI_VERSION == 2u, "codec ABI is v2");
_Static_assert(AIUS_OK == 0, "AIUS_OK must be zero so {0} init is a success");
_Static_assert(AIUS_MAX_CHANNELS == 6, "5.1 is the widest layout in scope");

/* A default-initialized metrics block reads as OK with no work recorded. */
static AiusStatus consume_noop(void *ctx, const AiusPcmBlock *block)
{
	(void)ctx;
	(void)block;
	return AIUS_OK;
}

int main(void)
{
	/* The 5.1 channel mask has popcount equal to the plane count. */
	unsigned mask = AIUS_CH_FL | AIUS_CH_FR | AIUS_CH_FC | AIUS_CH_LFE | AIUS_CH_SL | AIUS_CH_SR;
	assert(__builtin_popcount(mask) == AIUS_MAX_CHANNELS);

	AiusMetrics m = {0};
	assert(m.last_status == AIUS_OK);
	assert(m.alloc_count == 0);

	AiusPcmSink sink = {.ctx = NULL, .consume = consume_noop};
	AiusPcmBlock block = {0};
	assert(sink.consume(sink.ctx, &block) == AIUS_OK);

	printf("codec-api-contract: ok\n");
	return 0;
}
