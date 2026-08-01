/* Aius fork component selection: the (E-)AC-3 float decoder unit compiles
 * with both decoder personalities available; everything else is 0 by the
 * undefined-macro rule (see shim/config.h). */
#ifndef AIUS_FFMPEG_CONFIG_COMPONENTS_H
#define AIUS_FFMPEG_CONFIG_COMPONENTS_H

#define CONFIG_AC3_DECODER 1
#define CONFIG_EAC3_DECODER 1
/* ac3_parser.c compiles ENOSYS stubs for ff_ac3_parse_header without this. */
#define CONFIG_AC3_PARSER 1

#endif /* AIUS_FFMPEG_CONFIG_COMPONENTS_H */
