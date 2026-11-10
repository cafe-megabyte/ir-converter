/* The whole core as one translation unit (whole-program optimisation without
 * LTO, which the small WASM linker does not consume). */
#include "mem.c"
#include "mathx.c"
#include "tiff.c"
#include "ljpeg.c"
#include "cameras.c"
#include "nef.c"
#include "dng.c"
#include "color.c"
#include "develop.c"
#include "jpeg.c"
#include "engine.c"
#include "export.c"
