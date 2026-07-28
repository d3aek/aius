/* AiusDtsDecoder: portable adapter over the vendored dcadec DTS Core decoder.
 *
 * Implements aius_dts.h by driving the fixed-point core decoder directly
 * (core_parse -> core_filter) without dcadec's HD/EXSS context machinery, then
 * remapping dcadec's native speaker-ordered planar output to the AIUS_CH_*
 * convention. See lib/aius_dts/include/aius_dts.h and vendor/dcadec-core/NOTICE.md.
 */

#include "aius_dts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_alloc_shim.h"

/* Vendored dcadec headers (added to the include path by the build). */
#include "common.h"
#include "core_decoder.h"
#include "interpolator.h"
#include "ta.h"

/* The floating-point interpolation path is deliberately not vendored (the
 * decoder always runs bit-exact fixed-point so host and Teensy stay identical).
 * The vendored core still names these symbols in dead, non-bit-exact branches,
 * so provide aborting stubs to satisfy the link without pulling in the float
 * FIR tables. They are unreachable: DCADEC_FLAG_CORE_BIT_EXACT is always set. */
INTERPOLATE_LFE(lfe_float_fir)    { (void)pcm_samples; (void)lfe_samples; (void)npcmblocks; (void)dec_select; abort(); }
INTERPOLATE_LFE(lfe_float_fir_2x) { (void)pcm_samples; (void)lfe_samples; (void)npcmblocks; (void)dec_select; abort(); }
INTERPOLATE_LFE(lfe_float_iir)    { (void)pcm_samples; (void)lfe_samples; (void)npcmblocks; (void)dec_select; abort(); }
INTERPOLATE_SUB(sub32_float)      { (void)dsp; (void)pcm_samples; (void)subband_samples_lo; (void)subband_samples_hi; (void)nsamples; (void)perfect; abort(); }
INTERPOLATE_SUB(sub64_float)      { (void)dsp; (void)pcm_samples; (void)subband_samples_lo; (void)subband_samples_hi; (void)nsamples; (void)perfect; abort(); }

/* Process-wide mute for the hard-error log sink (see aius_dts_set_log_quiet). */
static int g_log_quiet;

void aius_dts_set_log_quiet(int quiet)
{
    g_log_quiet = quiet ? 1 : 0;
}

/* dcadec's logging sink. The vendored core reaches this via the dca_log macro
 * (obj->ctx passed through as `dca`). We run without a real context, so ctx is
 * NULL; surface only hard errors to stderr and stay silent otherwise. */
void dca_format_log(struct dcadec_context *dca, int level,
                    const char *file, int line, const char *fmt, ...)
{
    (void)dca;
    (void)file;
    (void)line;
    if (g_log_quiet)
        return;
    if ((level & ~DCADEC_LOG_ONCE) != DCADEC_LOG_ERROR)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[aius_dts] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

struct AiusDtsDecoder {
    struct core_decoder *core;  /* ta root; owns all decoder sub-allocations */
    uint8_t *inbuf;             /* aligned, padded scratch for one access unit */
    size_t   inbuf_cap;
    AiusArena *arena;           /* non-NULL => allocation routes through this arena */
    AiusMetrics metrics;        /* measurements from the most recent decode */
};

/* dcadec native speaker -> AIUS canonical plane, in AIUS emission order.
 *
 * THE INDIRECTION IS THE POINT: the DTS Core does not carry its primary
 * channels in the order this device emits them. A 5.1 core stream is AMODE 9,
 * whose primary channel order is CENTRE FIRST -- C, L, R, Ls, Rs -- and the
 * vendored core's map_prm_ch_to_spkr scatters those into output_samples[]
 * indexed by SPEAKER_*, never packed. This table reads that array back in AIUS
 * order (FL, FR, FC, LFE, SL, SR), which is the order aius_dts.h promises and
 * the order aius_dsp's mask-driven plane lookup expects.
 *
 * A rotation of the front three is therefore the natural failure mode here, and
 * it is invisible to the differential suite (which compares our plane c against
 * FFmpeg's channel c, so any shared permutation cancels) and to per-channel
 * level meters (five equal tones look identical however they are ordered). The
 * `dts` suite's channel-identity check on the `surround51_ident` vector is what
 * pins this table: it Goertzels each decoded plane for the tone its channel
 * owns. Defect 30 was a false alarm against this map -- the rotation was in the
 * source material -- and that check is what proved it. */
struct spkr_map {
    int          spkr_mask;  /* SPEAKER_MASK_* */
    int          spkr_index; /* SPEAKER_* (index into core->output_samples) */
    unsigned int aius_bit;   /* AIUS_CH_* */
};

static const struct spkr_map k_spkr_map[] = {
    { SPEAKER_MASK_L,    SPEAKER_L,    AIUS_CH_FL  },
    { SPEAKER_MASK_R,    SPEAKER_R,    AIUS_CH_FR  },
    { SPEAKER_MASK_C,    SPEAKER_C,    AIUS_CH_FC  },
    { SPEAKER_MASK_LFE1, SPEAKER_LFE1, AIUS_CH_LFE },
    { SPEAKER_MASK_Ls,   SPEAKER_Ls,   AIUS_CH_SL  },
    { SPEAKER_MASK_Rs,   SPEAKER_Rs,   AIUS_CH_SR  },
};

#define AIUS_SUPPORTED_MASK \
    (SPEAKER_MASK_L | SPEAKER_MASK_R | SPEAKER_MASK_C | \
     SPEAKER_MASK_LFE1 | SPEAKER_MASK_Ls | SPEAKER_MASK_Rs)

/* Allocate the handle + core. When `arena` is non-NULL, the allocation shim is
 * armed for the duration so the handle and the ta root land in the arena; it is
 * disarmed again on return. When NULL, allocation falls through to libc. */
static AiusStatus decoder_create(AiusDtsDecoder **out, AiusArena *arena)
{
    if (!out)
        return AIUS_INVALID_ARG;
    *out = NULL;

    if (arena)
        aius_alloc_shim_set_arena(arena);

    AiusDtsDecoder *dec = calloc(1, sizeof(*dec));
    if (dec) {
        /* The core must be ta-allocated so its sub-buffers attach as children. */
        dec->core = ta_znew(NULL, struct core_decoder);
    }

    if (arena)
        aius_alloc_shim_set_arena(NULL);

    if (!dec)
        return AIUS_NO_SPACE;
    if (!dec->core) {
        /* Only reachable in the libc path; an arena-backed handle is reclaimed
         * with the arena, and freeing it here is a harmless no-op besides. */
        free(dec);
        return AIUS_NO_SPACE;
    }

    dec->core->ctx = NULL;
    dec->arena = arena;
    dec->metrics.last_status = AIUS_OK;

    *out = dec;
    return AIUS_OK;
}

AiusStatus aius_dts_decoder_create(AiusDtsDecoder **out)
{
    return decoder_create(out, NULL);
}

AiusStatus aius_dts_decoder_create_arena(AiusDtsDecoder **out, AiusArena *arena)
{
    if (!arena)
        return AIUS_INVALID_ARG;
    return decoder_create(out, arena);
}

AiusStatus aius_dts_get_metrics(const AiusDtsDecoder *dec, AiusMetrics *out)
{
    if (!dec || !out)
        return AIUS_INVALID_ARG;
    *out = dec->metrics;
    return AIUS_OK;
}

void aius_dts_decoder_destroy(AiusDtsDecoder *dec)
{
    if (!dec)
        return;
    AiusArena *arena = dec->arena;
    if (arena)
        aius_alloc_shim_set_arena(arena);
    if (dec->core) {
        core_clear(dec->core);
        ta_free(dec->core);
    }
    free(dec->inbuf);
    free(dec);
    if (arena)
        aius_alloc_shim_set_arena(NULL);
}

/* Copy the access unit into a 4-byte-aligned buffer with DCADEC_BUFFER_PADDING
 * trailing zero bytes (the bit reader may over-read up to that much). */
static AiusStatus stage_input(AiusDtsDecoder *dec, const uint8_t *data,
                              size_t size, size_t *padded_out)
{
    size_t padded = (size + 3u) & ~(size_t)3u;
    size_t need = padded + DCADEC_BUFFER_PADDING;
    if (need > dec->inbuf_cap) {
        uint8_t *grown = realloc(dec->inbuf, need);
        if (!grown)
            return AIUS_NO_SPACE;
        dec->inbuf = grown;
        dec->inbuf_cap = need;
    }
    memcpy(dec->inbuf, data, size);
    memset(dec->inbuf + size, 0, need - size);
    *padded_out = padded;
    return AIUS_OK;
}

static AiusStatus map_core_error(int ret)
{
    switch (ret) {
    case -DCADEC_ENOSYNC:
        return AIUS_MALFORMED;
    case -DCADEC_EBADDATA:
    case -DCADEC_EBADCRC:
    case -DCADEC_EBADREAD:
        return AIUS_MALFORMED;
    case -DCADEC_ENOSUP:
        return AIUS_UNSUPPORTED;
    case -DCADEC_ENOMEM:
        return AIUS_NO_SPACE;
    default:
        return AIUS_MALFORMED;
    }
}

/* Core of a decode, run with the allocation shim already armed (or not) by the
 * caller. Returns the typed status; on AIUS_OK, *block references decoder-owned
 * planar PCM. */
static AiusStatus decode_locked(AiusDtsDecoder *dec, const uint8_t *data,
                                size_t size, AiusPcmBlock *block)
{
    size_t padded = 0;
    AiusStatus st = stage_input(dec, data, size, &padded);
    if (st != AIUS_OK)
        return st;

    const int flags = DCADEC_FLAG_CORE_ONLY | DCADEC_FLAG_CORE_BIT_EXACT;

    int ret = core_parse(dec->core, dec->inbuf, (int)size, flags, NULL);
    if (ret < 0)
        return map_core_error(ret);

    /* Only the 5.1-and-narrower layouts are in scope for AIUS. */
    if (dec->core->ch_mask & ~AIUS_SUPPORTED_MASK)
        return AIUS_UNSUPPORTED;

    ret = core_filter(dec->core, flags);
    if (ret < 0)
        return map_core_error(ret);

    memset(block, 0, sizeof(*block));
    block->frames = (uint32_t)dec->core->npcmsamples;
    block->sample_rate_hz = (uint32_t)dec->core->output_rate;

    uint16_t nch = 0;
    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof(k_spkr_map) / sizeof(k_spkr_map[0]); i++) {
        if (!(dec->core->ch_mask & k_spkr_map[i].spkr_mask))
            continue;
        /* dcadec's output_samples is int*; AiusPcmBlock.planes is int32_t*. The
         * two pointee types are representation-identical (both 32-bit), but on
         * arm-none-eabi newlib int32_t is `long int` (distinct from int), so the
         * assignment needs an explicit cast to compile there. */
        block->planes[nch] = (int32_t *)dec->core->output_samples[k_spkr_map[i].spkr_index];
        mask |= k_spkr_map[i].aius_bit;
        nch++;
    }
    block->channels = nch;
    block->channel_mask = mask;

    return AIUS_OK;
}

AiusStatus aius_dts_decode_access_unit(AiusDtsDecoder *dec,
                                       const uint8_t *data, size_t size,
                                       AiusPcmBlock *block)
{
    if (!dec || !data || !block || size == 0 || size > (size_t)INT_MAX)
        return AIUS_INVALID_ARG;

    if (dec->arena) {
        aius_alloc_shim_set_arena(dec->arena);
        aius_alloc_shim_reset_count();
    }

    AiusStatus st = decode_locked(dec, data, size, block);

    if (dec->arena) {
        dec->metrics.alloc_count = aius_alloc_shim_alloc_count();
        dec->metrics.arena_high_water = (uint32_t)aius_arena_high_water(dec->arena);
        aius_alloc_shim_set_arena(NULL);
    } else {
        dec->metrics.alloc_count = 0;
        dec->metrics.arena_high_water = 0;
    }
    dec->metrics.last_status = st;

    return st;
}
