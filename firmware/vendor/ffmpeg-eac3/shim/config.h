/* Aius fork build configuration: replaces FFmpeg's configure-generated
 * config.h. Undefined ARCH_/HAVE_/CONFIG_ -family macros evaluate as 0 in
 * "#if" context, which selects the portable C paths throughout; only knobs
 * that must be non-zero, or that are referenced outside "#if", are defined
 * here. AV_HAVE_* live in libavutil/avconfig.h. */
#ifndef AIUS_FFMPEG_CONFIG_H
#define AIUS_FFMPEG_CONFIG_H

#define HAVE_THREADS 0
#define HAVE_FAST_UNALIGNED 0
/* gcc/clang on both host and Cortex-M7 have the clz/ctz builtins. */
#define HAVE_FAST_CLZ 1

/* Referenced as a plain expression (not only in "#if"), so it must exist.
 * 64-bit host: 1. The target (Cortex-M7) build sets 0. */
#ifdef __aarch64__
#define HAVE_FAST_64BIT 1
#elif defined(__x86_64__)
#define HAVE_FAST_64BIT 1
#else
#define HAVE_FAST_64BIT 0
#endif

/* libm: both the glibc host and the target's newlib provide the full C99
 * set, so libavutil/libm.h must NOT emit its static fallbacks (they clash
 * with the libc declarations). */
#define HAVE_ATANF 1
#define HAVE_ATAN2F 1
#define HAVE_CBRT 1
#define HAVE_CBRTF 1
#define HAVE_COPYSIGN 1
#define HAVE_COSF 1
#define HAVE_ERF 1
#define HAVE_EXPF 1
#define HAVE_EXP2 1
#define HAVE_EXP2F 1
#define HAVE_HYPOT 1
#define HAVE_ISFINITE 1
#define HAVE_ISINF 1
#define HAVE_ISNAN 1
#define HAVE_LDEXPF 1
#define HAVE_LLRINT 1
#define HAVE_LLRINTF 1
#define HAVE_LOG2 1
#define HAVE_LOG2F 1
#define HAVE_LOG10F 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_POWF 1
#define HAVE_RINT 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_SINF 1
#define HAVE_TRUNC 1
#define HAVE_TRUNCF 1

#define CONFIG_MEMORY_POISONING 0
#define CONFIG_FTRAPV 0
#define CONFIG_FASTDIV 0

/* Hardcoded CRC tables (E-AC-3 plan phase 4, the DTCM flash-const rung):
 * with this off, av_crc_get_table() lazily generates 8 x 1024-entry
 * tables into 32,800 B of BSS -- which lands in the target's scarcest
 * bank. With it on, crc.c compiles its own 257-entry const tables
 * (8,224 B of rodata, flash-pinned on target) and the lazy path is
 * never taken. The byte-wise av_crc walk over 257-entry tables is
 * marginally slower per byte; the decode-cycle budget measurement in
 * phase 4 covers it. Values are arithmetic constants -- bit-identity
 * of the decode is untouched (host suite hashes re-prove). */
#define CONFIG_HARDCODED_TABLES 1

/* Fork policy: decode the independent (5.1) substream only; dependent
 * substreams are consumed, never decoded. See the [eac3-51-only] edits in
 * ac3defs.h / ac3dec.c / eac3dec.c. */
#define AIUS_EAC3_51_ONLY 1

#define FFMPEG_CONFIGURATION "aius-eac3-fork"
#define FFMPEG_LICENSE "LGPL version 2.1 or later"

#endif /* AIUS_FFMPEG_CONFIG_H */
