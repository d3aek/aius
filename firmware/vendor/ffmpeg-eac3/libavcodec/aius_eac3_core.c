/*
 * Aius E-AC-3 decoder core translation unit.
 *
 * Mirrors upstream ac3dec_float.c's include composition (the float decoder
 * is eac3dec.c + ac3dec.c compiled as ONE unit with IMDCT_TYPE set), but
 * stops before the AVCodec/AVOption registration block: the Aius fork drives
 * AC3DecodeContext through aius_eac3's calling convention, not through
 * libavcodec's codec registry.
 *
 * This file is part of the Aius FFmpeg fork and is licensed under the GNU
 * Lesser General Public License version 2.1 or later, like the code it
 * includes. See NOTICE.md for provenance (FFmpeg 7.1.5).
 */

#include "config_components.h"
#define IMDCT_TYPE AV_TX_FLOAT_MDCT

#include "libavutil/error.h"
#include "libavutil/internal.h"

#include "ac3dec.h"
#include "eac3dec.c"
#include "ac3dec.c"

/* Exported entry points over the (static) upstream decoder: the Aius
 * adapter drives these three; without them the whole static call graph is
 * dead code. Signatures mirror upstream's FFCodec callbacks. */

int aius_eac3_core_init(AVCodecContext *avctx)
{
	return ac3_decode_init(avctx);
}

int aius_eac3_core_frame(AVCodecContext *avctx, AVFrame *frame,
                         int *got_frame_ptr, AVPacket *avpkt)
{
	return ac3_decode_frame(avctx, frame, got_frame_ptr, avpkt);
}

int aius_eac3_core_end(AVCodecContext *avctx)
{
	return ac3_decode_end(avctx);
}
