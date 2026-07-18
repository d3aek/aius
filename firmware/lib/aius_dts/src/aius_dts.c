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

/* dcadec's logging sink. The vendored core reaches this via the dca_log macro
 * (obj->ctx passed through as `dca`). We run without a real context, so ctx is
 * NULL; surface only hard errors to stderr and stay silent otherwise. */
void dca_format_log(struct dcadec_context *dca, int level,
                    const char *file, int line, const char *fmt, ...)
{
    (void)dca;
    (void)file;
    (void)line;
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
};

/* dcadec native speaker -> AIUS canonical plane, in AIUS emission order. */
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

AiusStatus aius_dts_decoder_create(AiusDtsDecoder **out)
{
    if (!out)
        return AIUS_INVALID_ARG;
    *out = NULL;

    AiusDtsDecoder *dec = calloc(1, sizeof(*dec));
    if (!dec)
        return AIUS_NO_SPACE;

    /* The core must be ta-allocated so its sub-buffers can attach as children. */
    dec->core = ta_znew(NULL, struct core_decoder);
    if (!dec->core) {
        free(dec);
        return AIUS_NO_SPACE;
    }
    dec->core->ctx = NULL;

    *out = dec;
    return AIUS_OK;
}

void aius_dts_decoder_destroy(AiusDtsDecoder *dec)
{
    if (!dec)
        return;
    if (dec->core) {
        core_clear(dec->core);
        ta_free(dec->core);
    }
    free(dec->inbuf);
    free(dec);
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

AiusStatus aius_dts_decode_access_unit(AiusDtsDecoder *dec,
                                       const uint8_t *data, size_t size,
                                       AiusPcmBlock *block)
{
    if (!dec || !data || !block || size == 0 || size > (size_t)INT_MAX)
        return AIUS_INVALID_ARG;

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
        block->planes[nch] = dec->core->output_samples[k_spkr_map[i].spkr_index];
        mask |= k_spkr_map[i].aius_bit;
        nch++;
    }
    block->channels = nch;
    block->channel_mask = mask;

    return AIUS_OK;
}
