#ifndef IRCONV_RAW_H
#define IRCONV_RAW_H
#include "tiff.h"

#define APP_NAME "IR Converter"
enum { RAW_NEF = 1, RAW_DNG = 2 };
enum { WARN_CAMERA_UNKNOWN = 1, WARN_NO_WB = 2 };

/* A raw image with everything the renderer and the DNG writer need: a Bayer
 * mosaic (spp = 1) or a linear DNG's demosaiced camera RGB (spp = 3, samples
 * interleaved). Colour indices: 0 = red, 1 = green, 2 = blue. cfa[] (Bayer
 * only) is relative to the buffer origin: colour at (y, x) =
 * cfa[(y & 1) * 2 + (x & 1)]. */
typedef struct {
    int kind;
    uint32_t width, height;
    int spp;                          /* samples per pixel: 1 Bayer, 3 linear RGB */
    uint16_t *data;                   /* raw codes, width * height * spp */
    uint16_t *lin; uint32_t lin_n;    /* linearisation table (optional) */
    double black[4];                  /* per CFA position (buffer parity); linear: per sample */
    double *black_h, *black_v;        /* DNG BlackLevelDeltaH/V (active area columns/rows) */
    double white;
    int cfa[4];
    int cfa_aa[4];                    /* pattern relative to the active area origin */
    uint32_t aa_top, aa_left, aa_bottom, aa_right;
    uint32_t crop_x, crop_y, crop_w, crop_h;   /* buffer coordinates */
    int orientation;
    /* Colour (DNG semantics). n_ill = number of calibration illuminants. */
    int n_ill, ill[2], has_fm;
    double cm[2][9], fm[2][9], cc[2][9], ab[3];
    double as_shot[3]; int has_as_shot;
    double as_shot_xy[2]; int has_as_shot_xy;
    double baseline_exposure;
    char make[64], model[64], unique_model[64];
    int warnings;
    /* Source container details. */
    TiffIfd *raw_ifd;
    const uint8_t *nikon_mn; uint32_t nikon_mn_len, nikon_mn_offset; int nikon_mn_be;
    /* Extra DNG tags for NEF conversion (from the camera table). */
    double baseline_noise, baseline_sharpness, linear_response_limit, shadow_scale, aa_strength;
} Raw;

int nef_load(Raw *raw, const Tiff *t);
int dng_load(Raw *raw, const Tiff *t);
void raw_free(Raw *raw);
void camera_apply(Raw *raw);
#endif
