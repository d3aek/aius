/* Aius framework shim interface: the two hooks the embedding adapter (or a
 * test harness) registers before driving the decoder core. Part of the Aius
 * FFmpeg fork (LGPL-2.1-or-later). */
#ifndef AIUS_AVSHIM_H
#define AIUS_AVSHIM_H

#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provide output planes for a frame whose nb_samples/ch_layout the decoder
 * has just set: fill frame->data[ch]. Return 0 or a negative AVERROR. */
typedef int (*AiusAvGetBufferFn)(void *opaque, AVCodecContext *avctx,
                                 AVFrame *frame);
void aius_avshim_set_get_buffer(AiusAvGetBufferFn fn, void *opaque);

/* Receive decoder log lines (already formatted). NULL (the default)
 * silences them. */
typedef void (*AiusAvLogFn)(int level, const char *message);
void aius_avshim_set_log(AiusAvLogFn fn);

/* The decoder core's exported entry points (see aius_eac3_core.c). */
int aius_eac3_core_init(AVCodecContext *avctx);
int aius_eac3_core_frame(AVCodecContext *avctx, AVFrame *frame,
                         int *got_frame_ptr, AVPacket *avpkt);
int aius_eac3_core_end(AVCodecContext *avctx);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_AVSHIM_H */
