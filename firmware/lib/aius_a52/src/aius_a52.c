/* AC-3 decoder adapter: identity-only liba52 fork behind AiusA52Decoder,
 * remapping liba52's LFE-first block output to the AIUS_CH_* plane convention
 * and the [-1,1] float samples to the AIUS_PCM_SAMPLE_BITS int32 scale.
 *
 * The contract, its gate findings, and every fork divergence are
 * recorded in aius-ac3-support.md §Phase 1 contract and
 * lib/aius_a52/a52/NOTICE.md. The load-bearing shape: the fork's bit reader
 * has a byte-exact fail-closed bound, and decode NEVER runs on the caller's
 * buffer, only on one owned maximum-frame scratch. The `a52` suite's
 * channel-identity check on the `surround51_ident` vector is what pins the
 * channel map (defect 30's lesson: the map is convicted by tones, not trusted
 * from documentation). */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "aius_a52.h"
#include "aius_alloc_shim.h"

#include "a52.h"

/* liba52 output block order is LFE first (when granted), then the acmod's
 * own order. Per acmod: the liba52 slot sequence expressed as AIUS_CH_* bits.
 * Modes absent from this table are UNSUPPORTED (mono-surround has no AIUS
 * plane; dual-mono is not a programme). */
struct a52_mode_map {
    int acmod;                              /* A52_* channel mode */
    uint16_t channels;                      /* slots liba52 emits (sans LFE) */
    unsigned int aius_bits[5];              /* slot -> AIUS_CH_*, in order */
};

static const struct a52_mode_map k_mode_map[] = {
    { A52_MONO,   1, { AIUS_CH_FC } },
    { A52_STEREO, 2, { AIUS_CH_FL, AIUS_CH_FR } },
    { A52_DOLBY,  2, { AIUS_CH_FL, AIUS_CH_FR } },
    { A52_3F,     3, { AIUS_CH_FL, AIUS_CH_FC, AIUS_CH_FR } },
    { A52_2F2R,   4, { AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_SL, AIUS_CH_SR } },
    { A52_3F2R,   5, { AIUS_CH_FL, AIUS_CH_FC, AIUS_CH_FR, AIUS_CH_SL,
                       AIUS_CH_SR } },
};

struct AiusA52Decoder {
    a52_state_t *state;
    AiusArena *arena;        /* NULL on the libc path */
    uint8_t *scratch;        /* AIUS_A52_MAX_FRAME */
    int32_t *planes[AIUS_MAX_CHANNELS];
    AiusMetrics metrics;
};

static const struct a52_mode_map *mode_lookup(int acmod)
{
    for (size_t i = 0; i < sizeof(k_mode_map) / sizeof(k_mode_map[0]); i++)
        if (k_mode_map[i].acmod == acmod)
            return &k_mode_map[i];
    return NULL;
}

/* Shared create body: when `arena` is non-NULL the shim is armed for the
 * duration so the handle, scratch, planes, and liba52's own allocations all
 * land in the arena; disarmed again on return (the aius_dts shape). */
static AiusStatus decoder_create(AiusA52Decoder **out, AiusArena *arena)
{
    if (!out)
        return AIUS_INVALID_ARG;
    *out = NULL;

    if (arena)
        aius_alloc_shim_set_arena(arena);

    AiusStatus st = AIUS_NO_SPACE;
    AiusA52Decoder *dec = calloc(1, sizeof(*dec));
    if (!dec)
        goto done;
    dec->arena = arena;

    dec->state = a52_init(0); /* accel 0: the pure C path, deterministic */
    if (!dec->state)
        goto fail;

    dec->scratch = malloc(AIUS_A52_MAX_FRAME);
    if (!dec->scratch)
        goto fail;
    memset(dec->scratch, 0, AIUS_A52_MAX_FRAME);

    for (int c = 0; c < AIUS_MAX_CHANNELS; c++) {
        dec->planes[c] = malloc(AIUS_A52_FRAME_SAMPLES * sizeof(int32_t));
        if (!dec->planes[c])
            goto fail;
    }

    if (arena)
        dec->metrics.arena_high_water = (uint32_t)aius_arena_high_water(arena);
    *out = dec;
    st = AIUS_OK;
    goto done;

fail:
    if (dec) {
        for (int c = 0; c < AIUS_MAX_CHANNELS; c++)
            free(dec->planes[c]);
        free(dec->scratch);
        if (dec->state)
            a52_free(dec->state);
        free(dec);
    }
done:
    if (arena)
        aius_alloc_shim_set_arena(NULL);
    return st;
}

AiusStatus aius_a52_decoder_create(AiusA52Decoder **out)
{
    return decoder_create(out, NULL);
}

AiusStatus aius_a52_decoder_create_arena(AiusA52Decoder **out, AiusArena *arena)
{
    if (!arena)
        return AIUS_INVALID_ARG;
    return decoder_create(out, arena);
}

/* One saturating float->int32 quantize at the AIUS_PCM_SAMPLE_BITS scale.
 * liba52 (level 1, bias 0) nominally emits [-1,1] but clipping is legal
 * upstream of us, so the clamp is unconditional. */
static int32_t sample_quantize(float v)
{
    float scaled = v * 8388608.0f; /* 2^23: full scale at 24 bits */
    if (scaled >= (float)AIUS_PCM_SAMPLE_MAX)
        return AIUS_PCM_SAMPLE_MAX;
    if (scaled <= (float)AIUS_PCM_SAMPLE_MIN)
        return AIUS_PCM_SAMPLE_MIN;
    return (int32_t)scaled;
}

AiusStatus aius_a52_decode_access_unit(AiusA52Decoder *dec,
                                       const uint8_t *data, size_t size,
                                       AiusPcmBlock *block)
{
    if (!dec || !data || size == 0 || !block)
        return AIUS_INVALID_ARG;

    AiusStatus st;
    if (dec->arena) {
        aius_alloc_shim_set_arena(dec->arena);
        aius_alloc_shim_reset_count();
    }

    /* a52_syncinfo reads buf[0..6] unconditionally: length is OUR guard. */
    if (size < 7 || size > AIUS_A52_MAX_FRAME) {
        st = AIUS_MALFORMED;
        goto out;
    }

    /* The AU is parsed ONLY from the owned scratch. */
    memcpy(dec->scratch, data, size);

    int flags = 0, sample_rate = 0, bit_rate = 0;
    int frame_length = a52_syncinfo(dec->scratch, &flags, &sample_rate,
                                    &bit_rate);
    if (frame_length <= 0 || (size_t)frame_length != size) {
        /* Bad sync/bsid/frmsizecod, or the burst payload disagrees with the
         * header's own length. a52_frame never runs on a buffer syncinfo did
         * not accept (its halfrate[] lookup is unguarded). */
        st = AIUS_MALFORMED;
        goto out;
    }
    if (sample_rate != 48000) {
        st = AIUS_UNSUPPORTED;
        goto out;
    }

    const struct a52_mode_map *map = mode_lookup(flags & A52_CHANNEL_MASK);
    if (!map) {
        st = AIUS_UNSUPPORTED;
        goto out;
    }
    int lfe = (flags & A52_LFE) != 0;

#ifdef AIUS_A52_BOUND_PROBE
    a52_bitstream_probe_expect_end(dec->state, dec->scratch,
                                   (uint32_t)frame_length);
#endif
    a52_bitstream_set_end(dec->state, dec->scratch + frame_length);

    /* Identity request: the stream's own coding, level 1, bias 0. Never a
     * widening, never a downmix (upstream's 3F1R memcpy typo stays dormant). */
    int request = (flags & (A52_CHANNEL_MASK | A52_LFE));
    sample_t level = 1.0f;
    if (a52_frame(dec->state, dec->scratch, &request, &level, 0.0f)) {
        st = AIUS_MALFORMED;
        goto out;
    }

    uint16_t channels = (uint16_t)(map->channels + (lfe ? 1 : 0));
    uint32_t mask = lfe ? AIUS_CH_LFE : 0u;
    for (uint16_t s = 0; s < map->channels; s++)
        mask |= map->aius_bits[s];

    /* AIUS plane index for each liba52 slot: planes are tightly packed in
     * AIUS_CH_* bit order, liba52 blocks are LFE first then the acmod order. */
    for (int b = 0; b < 6; b++) {
        if (a52_block(dec->state)) {
            st = AIUS_MALFORMED;
            goto out;
        }
        const sample_t *samples = a52_samples(dec->state);
        uint32_t base = (uint32_t)b * 256u;

        int slot = 0;
        if (lfe) {
            /* LFE is always plane index popcount(mask & (LFE-1)) — compute
             * directly: bits below AIUS_CH_LFE present in the mask. */
            uint32_t below = mask & (AIUS_CH_LFE - 1u);
            int idx = 0;
            while (below) {
                idx += (int)(below & 1u);
                below >>= 1;
            }
            for (uint32_t n = 0; n < 256; n++)
                dec->planes[idx][base + n] =
                    sample_quantize(samples[slot * 256 + (int)n]);
            slot++;
        }
        for (uint16_t s = 0; s < map->channels; s++) {
            uint32_t below = mask & (map->aius_bits[s] - 1u);
            int idx = 0;
            while (below) {
                idx += (int)(below & 1u);
                below >>= 1;
            }
            for (uint32_t n = 0; n < 256; n++)
                dec->planes[idx][base + n] =
                    sample_quantize(samples[slot * 256 + (int)n]);
            slot++;
        }
    }

    memset(block, 0, sizeof(*block));
    for (uint16_t c = 0; c < channels; c++)
        block->planes[c] = dec->planes[c];
    block->channels = channels;
    block->frames = AIUS_A52_FRAME_SAMPLES;
    block->sample_rate_hz = 48000;
    block->channel_mask = mask;
    st = AIUS_OK;

out:
    if (dec->arena) {
        dec->metrics.alloc_count = aius_alloc_shim_alloc_count();
        dec->metrics.arena_high_water =
            (uint32_t)aius_arena_high_water(dec->arena);
        aius_alloc_shim_set_arena(NULL);
    } else {
        dec->metrics.alloc_count = 0;
    }
    dec->metrics.last_status = st;
    return st;
}

AiusStatus aius_a52_get_metrics(const AiusA52Decoder *dec, AiusMetrics *out)
{
    if (!dec || !out)
        return AIUS_INVALID_ARG;
    *out = dec->metrics;
    return AIUS_OK;
}

#ifdef AIUS_A52_BOUND_PROBE
AiusStatus aius_a52_get_bound_probe(const AiusA52Decoder *dec,
                                    uint32_t *fills,
                                    uint32_t *suppressed_fills,
                                    uint32_t *end_mismatches)
{
    if (!dec || !fills || !suppressed_fills || !end_mismatches)
        return AIUS_INVALID_ARG;
    a52_bitstream_get_probe(dec->state, fills, suppressed_fills,
                            end_mismatches);
    return AIUS_OK;
}
#endif

void aius_a52_decoder_destroy(AiusA52Decoder *dec)
{
    if (!dec)
        return;
    /* The shim frees by pointer range, so arena blocks route back to the
     * arena even while disarmed (mirroring aius_dts_decoder_destroy). */
    for (int c = 0; c < AIUS_MAX_CHANNELS; c++)
        free(dec->planes[c]);
    free(dec->scratch);
    if (dec->state)
        a52_free(dec->state);
    free(dec);
}
