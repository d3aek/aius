"""Scope the ffmpeg-eac3 fork's build accommodations to exactly its sources.

The fork TUs and the aius_eac3 adapter need include paths (shim, fork root,
libavcodec) and -DHAVE_AV_CONFIG_H that must NOT leak env-wide: liba52's
sources include "config.h" and must keep resolving lib/aius_a52/include's
copy, while the fork's must resolve shim/config.h -- with both directories
on one env-wide path, -I order silently mis-configures one side or the
other (the phase-4 prebuild gate's blocker 3). PlatformIO cannot scope
flags inside build_src_filter, so this pre-script attaches them per-source
via build middleware; every other TU compiles exactly as before.

The host suites get the same separation from eac3_compile_objects in
scripts/test.sh; this file is the target-side twin.
"""

Import("env")  # noqa: F821  (SCons construction environment)

import os

FW_ROOT = env["PROJECT_DIR"]  # noqa: F821
FORK_ROOT = os.path.join(FW_ROOT, "vendor", "ffmpeg-eac3")

FORK_CPPPATH = [
    os.path.join(FORK_ROOT, "shim"),
    FORK_ROOT,
    os.path.join(FORK_ROOT, "libavcodec"),
]

# -Wno-unused-function mirrors the host eac3_build note: ac3dec.h carries
# static prototypes for the fork's internal split, and the adapter includes
# it for the context type. The target compiles with -Wall (no -Werror), so
# that one is log hygiene. -Wno-error=incompatible-pointer-types is NOT:
# newlib-arm typedefs int32_t as `long` (glibc says `int`), the fork mixes
# int* with int32_t* in ways upstream compiles warning-only on arm-linux,
# and GCC >= 14 hard-errors that diagnostic in C. Same 32-bit
# representation either way; downgraded back to the warning upstream sees,
# for these sources only.
FORK_CCFLAGS = [
    "-DHAVE_AV_CONFIG_H",
    "-Wno-unused-function",
    "-Wno-error=incompatible-pointer-types",
]


def fork_scoped(node):
    # The fork paths PREPEND: the env path carries lib/aius_a52/include,
    # whose config.h would otherwise win the quoted-include search and
    # silently mis-configure every fork TU (libm.h then emits static
    # fallbacks that collide with newlib). shim-first is the same -I order
    # the host eac3_build uses.
    return env.Object(  # noqa: F821
        node,
        CPPPATH=FORK_CPPPATH + env["CPPPATH"],  # noqa: F821
        CCFLAGS=env["CCFLAGS"] + FORK_CCFLAGS,  # noqa: F821
    )


env.AddBuildMiddleware(fork_scoped, "*/vendor/ffmpeg-eac3/*")  # noqa: F821
env.AddBuildMiddleware(fork_scoped, "*/lib/aius_eac3/src/*")  # noqa: F821
