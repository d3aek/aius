/* Aius framework shim for the vendored (E-)AC-3 decoder core.
 *
 * Everything libavcodec's registry/AVOption/threading world used to provide
 * is reduced to what the decode path actually calls:
 *   - ff_get_buffer: output planes come from a caller-registered provider
 *     (the aius_eac3 adapter's arena on target, static planes in tests).
 *   - channel-layout ops: the decoder only builds native-order masks.
 *   - logging: routed to one overridable sink; quiet by default.
 *   - downmix/matrix side data: the Aius pipeline consumes planar PCM
 *     directly, so side-data updates are deliberate no-ops.
 *
 * This file is part of the Aius FFmpeg fork (LGPL-2.1-or-later). */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/channel_layout.h"
#include "libavutil/downmix_info.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/internal.h"
#include "libavutil/log.h"

#include "libavcodec/avcodec.h"

#include "aius_avshim.h"

/* ---- output-buffer provider ---------------------------------------------- */

static AiusAvGetBufferFn g_get_buffer;
static void *g_get_buffer_opaque;

void aius_avshim_set_get_buffer(AiusAvGetBufferFn fn, void *opaque)
{
    g_get_buffer = fn;
    g_get_buffer_opaque = opaque;
}

int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    (void)flags;
    if (!g_get_buffer)
        return AVERROR(EINVAL);
    /* Mirror upstream decode.c: the frame's shape comes from the context
     * (the decoder set avctx->ch_layout/sample_fmt/sample_rate before
     * requesting the buffer), and audio consumers write through
     * extended_data, which for <= AV_NUM_DATA_POINTERS channels aliases
     * data. Native-order mask layouts copy without allocation. */
    frame->ch_layout = avctx->ch_layout;
    frame->format = avctx->sample_fmt;
    frame->sample_rate = avctx->sample_rate;
    int ret = g_get_buffer(g_get_buffer_opaque, avctx, frame);
    if (ret < 0)
        return ret;
    frame->extended_data = frame->data;
    return 0;
}

/* ---- channel layout (native mask order only) ------------------------------ */

int av_channel_layout_from_mask(AVChannelLayout *layout, uint64_t mask)
{
    if (!mask)
        return AVERROR(EINVAL);
    memset(layout, 0, sizeof(*layout));
    layout->order = AV_CHANNEL_ORDER_NATIVE;
    layout->nb_channels = __builtin_popcountll(mask);
    layout->u.mask = mask;
    return 0;
}

int av_channel_layout_compare(const AVChannelLayout *a,
                              const AVChannelLayout *b)
{
    if (a->nb_channels != b->nb_channels)
        return 1;
    if (a->order == AV_CHANNEL_ORDER_NATIVE &&
        b->order == AV_CHANNEL_ORDER_NATIVE)
        return a->u.mask != b->u.mask;
    return 1;
}

void av_channel_layout_uninit(AVChannelLayout *layout)
{
    memset(layout, 0, sizeof(*layout));
}

int av_channel_layout_index_from_channel(const AVChannelLayout *layout,
                                         enum AVChannel channel)
{
    if (layout->order != AV_CHANNEL_ORDER_NATIVE ||
        (unsigned)channel >= 64u ||
        !(layout->u.mask & (1ULL << channel)))
        return AVERROR(EINVAL);
    return __builtin_popcountll(layout->u.mask &
                                ((1ULL << channel) - 1u));
}

/* ---- logging -------------------------------------------------------------- */

static AiusAvLogFn g_log_fn;

void aius_avshim_set_log(AiusAvLogFn fn)
{
    g_log_fn = fn;
}

void av_log(void *avcl, int level, const char *fmt, ...)
{
    (void)avcl;
    if (!g_log_fn)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log_fn(level, buf);
}

void avpriv_request_sample(void *avc, const char *msg, ...)
{
    (void)avc;
    if (!g_log_fn)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    g_log_fn(AV_LOG_WARNING, buf);
}

/* ---- tx.c debug/CPU plumbing: quiet stubs ---------------------------------
 *
 * tx.c formats its codelet-selection trace through AVBPrint before handing
 * it to av_log, and consults av_get_cpu_flags for SIMD codelet gating. The
 * fork compiles only the C codelets and discards decoder logs by default,
 * so both collapse to stubs (the bprint buffer stays a valid empty string
 * for any %s consumer). */

#include "libavutil/bprint.h"
#include "libavutil/cpu.h"

void av_bprint_init(AVBPrint *buf, unsigned size_init, unsigned size_max)
{
    (void)size_init;
    (void)size_max;
    memset(buf, 0, sizeof(*buf));
    buf->str = buf->reserved_internal_buffer;
    buf->size = sizeof(buf->reserved_internal_buffer);
}

void av_bprintf(AVBPrint *buf, const char *fmt, ...)
{
    (void)buf;
    (void)fmt;
}

int av_get_cpu_flags(void)
{
    return 0; /* pure C codelets only */
}

/* ---- parser-registry link stubs -------------------------------------------
 *
 * ac3_parser.c is vendored for ff_ac3_parse_header/ff_ac3_find_syncword, but
 * compiling it also emits the AVCodecParser registry struct, whose callback
 * slots name the framework's ff_aac_ac3_parse/ff_parse_close. Nothing ever
 * invokes that struct here (no parser registry exists), and ASan's global
 * registration keeps it alive past --gc-sections, so the two externs are
 * satisfied with unreachable abort stubs -- the same shape the dcadec vendor
 * uses for its dead float-interpolation branches (see its NOTICE.md). */

#include <stdlib.h>

#include "libavcodec/aac_ac3_parser.h"
#include "libavcodec/parser.h"

int ff_aac_ac3_parse(AVCodecParserContext *s1, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    (void)s1;
    (void)avctx;
    (void)poutbuf;
    (void)poutbuf_size;
    (void)buf;
    (void)buf_size;
    abort(); /* unreachable: no parser registry in the fork */
}

void ff_parse_close(AVCodecParserContext *s)
{
    (void)s;
    abort(); /* unreachable: no parser registry in the fork */
}

/* ---- side data: deliberate no-ops ----------------------------------------- */

AVDownmixInfo *av_downmix_info_update_side_data(AVFrame *frame)
{
    /* ac3dec treats NULL as ENOMEM and fails the whole frame, so hand it a
     * writable sink. Aius consumes planar PCM directly and never reads
     * downmix metadata; one static instance (single-threaded decode) simply
     * absorbs the writes. */
    static AVDownmixInfo sink;
    (void)frame;
    return &sink;
}

int ff_side_data_update_matrix_encoding(AVFrame *frame,
                                        enum AVMatrixEncoding matrix_encoding)
{
    (void)frame;
    (void)matrix_encoding;
    return 0;
}
