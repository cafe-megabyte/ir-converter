/* Per-model data for NEF files, which carry no colour matrices or crop.
 * Values for the D90 come from Adobe DNG Converter 18.7's output for the same
 * NEF (samples/DSC_0080*.dng). Its WhiteLevel 3767 corresponds to raw code
 * 732; the white level follows the decoded NEF curve at that code. Unknown models fall back to an sRGB-like
 * matrix and the full sensor area; the app shows a warning. */
#include "raw.h"

typedef struct {
    const char *model, *unique;
    uint32_t aa_right, aa_bottom;        /* 0 = use full raw size */
    uint32_t crop_x, crop_y, crop_w, crop_h;
    int ill1, ill2;
    double cm1[9], cm2[9], fm1[9], fm2[9];
    double baseline_exposure;
    uint32_t white_code;                 /* raw code of sensor saturation, 0 = curve end */
} Camera;

static const Camera cameras[] = {
    {"NIKON D90", "Nikon D90", 4310, 2868, 11, 10, 4288, 2848, 17, 21,
     {0.8437, -0.2581, 0.0132, -0.6948, 1.4143, 0.3100, -0.1215, 0.1778, 0.8300},
     {0.7309, -0.1403, -0.0519, -0.8474, 1.6008, 0.2622, -0.2433, 0.2826, 0.8064},
     {0.8031, 0.1702, -0.0090, 0.4054, 0.7851, -0.1905, 0.0695, -0.1657, 0.9213},
     {0.8031, 0.1702, -0.0090, 0.4054, 0.7851, -0.1905, 0.0695, -0.1657, 0.9213},
     0.25, 732},
};

static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

void camera_apply(Raw *raw) {
    const Camera *c = NULL;
    for (uint32_t i = 0; i < sizeof cameras / sizeof cameras[0]; i++)
        if (str_eq(raw->model, cameras[i].model)) c = &cameras[i];
    for (int i = 0; i < 3; i++) raw->ab[i] = 1;
    for (int k = 0; k < 2; k++) for (int i = 0; i < 9; i++) raw->cc[k][i] = i % 4 == 0;
    raw->baseline_noise = raw->baseline_sharpness = raw->linear_response_limit = raw->shadow_scale = raw->aa_strength = 1;
    if (!c) {
        /* XYZ (D65) -> linear sRGB: a neutral stand-in for unknown cameras. */
        static const double srgb[9] = {3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252};
        raw->n_ill = 1; raw->ill[0] = 21; raw->has_fm = 0;
        for (int i = 0; i < 9; i++) raw->cm[0][i] = srgb[i];
        xmemcpy(raw->unique_model, raw->model, sizeof raw->unique_model);
        raw->warnings |= WARN_CAMERA_UNKNOWN;
        return;
    }
    if (c->aa_right && c->aa_right <= raw->width) raw->aa_right = c->aa_right;
    if (c->aa_bottom && c->aa_bottom <= raw->height) raw->aa_bottom = c->aa_bottom;
    if (raw->aa_left + c->crop_x + c->crop_w <= raw->aa_right && raw->aa_top + c->crop_y + c->crop_h <= raw->aa_bottom) {
        raw->crop_x = raw->aa_left + c->crop_x; raw->crop_y = raw->aa_top + c->crop_y;
        raw->crop_w = c->crop_w; raw->crop_h = c->crop_h;
    }
    raw->n_ill = 2; raw->ill[0] = c->ill1; raw->ill[1] = c->ill2; raw->has_fm = 1;
    for (int i = 0; i < 9; i++) { raw->cm[0][i] = c->cm1[i]; raw->cm[1][i] = c->cm2[i]; raw->fm[0][i] = c->fm1[i]; raw->fm[1][i] = c->fm2[i]; }
    raw->baseline_exposure = c->baseline_exposure;
    if (c->white_code && raw->lin && c->white_code < raw->lin_n) raw->white = raw->lin[c->white_code];
    int k = 0;
    for (; c->unique[k] && k < 63; k++) raw->unique_model[k] = c->unique[k];
    raw->unique_model[k] = 0;
}
