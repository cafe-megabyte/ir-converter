/* Session state behind the WASM API: one open image, its white balance
 * (unbounded temperature), the optional red/blue photosite swap and the
 * rendered view. */
#include "raw.h"

typedef struct {
    uint8_t *file; uint32_t len;
    char name[256];
    Tiff tiff;
    Raw raw;
    Profile prof;
    Linear lin;
    int swap;
    double asshot[3];          /* as-shot neutral in the current channel order */
    double t0, d0, corr[3];    /* fit of the as-shot neutral and its correction */
    double temp, n[3];
    Base base; uint32_t base_w, base_h;
    Render rp;
    uint8_t *view; uint32_t view_w, view_h; int view_hdr;
    Buf out;
    int error_detail;
    char camera[160];
} Session;
static Session S;

static void swap_rb(double *v) { double t = v[0]; v[0] = v[2]; v[2] = t; }

static void session_reset(void) {
    linear_free(&S.lin); base_free(&S.base); raw_free(&S.raw); tiff_free(&S.tiff);
    xfree(S.view); xfree(S.file); buf_free(&S.out);
    xmemset(&S, 0, sizeof(S));
}

/* Neutral for temperature t keeping the as-shot tint and correction. */
static void neutral_for(double t, double *n) {
    if (!model_neutral_ext(&S.prof, t, S.d0, n)) { n[0] = n[1] = n[2] = 1; }
    for (int i = 0; i < 3; i++) { if (!(n[i] > 1e-300)) n[i] = 1e-300; n[i] *= S.corr[i]; }
    normalize3(n);
    for (int i = 0; i < 3; i++) if (!(n[i] > 1e-12)) n[i] = 1e-12;
}
static void update_render(void) {
    M3 p = camera_to_pcs(&S.prof, S.n, S.temp, S.d0);
    static const M3 to_srgb = {{3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415, 0.0334540, 0.0719453, -0.2289914, 1.4052427}};
    /* XYZ (D50) -> linear Display P3: Bradford D50 -> D65, then P3 (D65). */
    static const M3 bradford = {{0.9555766, -0.0230393, 0.0631636, -0.0282895, 1.0099416, 0.0210077, 0.0122982, -0.0204830, 1.3299098}};
    M3 p3 = xyz_to_display_p3(), m = m3_mul(&to_srgb, &p), to_p3 = m3_mul(&p3, &bradford), mp = m3_mul(&to_p3, &p);
    double gain = m_pow(2.0, S.raw.baseline_exposure);
    for (int i = 0; i < 9; i++) { S.rp.m[i] = (float)(m.m[i] * gain); S.rp.m_p3[i] = (float)(mp.m[i] * gain); }
    for (int i = 0; i < 3; i++) S.rp.wb[i] = (float)(1.0 / S.n[i]);
}
static void setup_white(void) {
    double n[3];
    if (S.raw.has_as_shot) for (int i = 0; i < 3; i++) n[i] = S.raw.as_shot[i];
    else {
        double xyz[3];
        if (S.raw.has_as_shot_xy) xy_to_xyz(S.raw.as_shot_xy[0], S.raw.as_shot_xy[1], xyz);
        else { double x, y; temp_xy(5500, 0, &x, &y); xy_to_xyz(x, y, xyz); S.raw.warnings |= WARN_NO_WB; }
        M3 m = xyz_to_camera(&S.prof, 5500);
        m3_vec(&m, xyz, n);
        for (int i = 0; i < 3; i++) if (!(n[i] > 1e-6)) n[i] = 1e-6;
    }
    if (S.swap) swap_rb(n);
    normalize3(n);
    for (int i = 0; i < 3; i++) S.asshot[i] = n[i];
    fit_neutral(&S.prof, n, &S.t0, &S.d0);
    /* Correction so that the slider passes exactly through the as-shot value. */
    S.corr[0] = S.corr[1] = S.corr[2] = 1;
    double m[3];
    neutral_for(S.t0, m);
    for (int i = 0; i < 3; i++) S.corr[i] = n[i] / m[i];
    S.temp = S.t0;
    for (int i = 0; i < 3; i++) S.n[i] = n[i];
    update_render();
}

int engine_open(uint8_t *file, uint32_t len) {
    session_reset();
    S.file = file; S.len = len;
    build_lut(&S.rp);
    int r = tiff_parse(&S.tiff, file, len);
    if (r == ERR_OK) {
        r = tiff_find(S.tiff.ifd0, 0xC612) ? dng_load(&S.raw, &S.tiff) : nef_load(&S.raw, &S.tiff);
    }
    if (r == ERR_OK) {
        profile_from_raw(&S.raw, &S.prof);
        r = linear_init(&S.lin, &S.raw, 0);
    }
    if (r != ERR_OK) {
        int detail = r >> 8;
        S.file = NULL;   /* the caller keeps ownership on failure */
        session_reset();
        S.error_detail = detail;
        return r & 0xFF;
    }
    setup_white();
    uint32_t k = 0;
    for (const char *s = S.raw.make; *s && k < 70; s++) S.camera[k++] = *s;
    if (k) S.camera[k++] = ' ';
    for (const char *s = S.raw.model; *s && k < 150; s++) S.camera[k++] = *s;
    S.camera[k] = 0;
    return ERR_OK;
}
int engine_set_swap(int on) {
    if (!S.raw.data) return ERR_STATE;
    on = !!on;
    if (on == S.swap) return ERR_OK;
    S.swap = on;
    linear_free(&S.lin); base_free(&S.base); S.base_w = S.base_h = 0;
    int r = linear_init(&S.lin, &S.raw, on);
    if (r) return r;
    setup_white();
    return ERR_OK;
}
int engine_set_temperature(double t) {
    if (!S.raw.data) return ERR_STATE;
    if (!(t > 0)) return ERR_STATE;
    if (t < 1e-6) t = 1e-6;
    if (t > 1e15) t = 1e15;
    S.temp = t;
    neutral_for(t, S.n);
    update_render();
    return ERR_OK;
}
int engine_reset_white(void) {
    if (!S.raw.data) return ERR_STATE;
    S.temp = S.t0;
    for (int i = 0; i < 3; i++) S.n[i] = S.asshot[i];
    update_render();
    return ERR_OK;
}
/* Oriented size of the final image. */
uint32_t oriented_w(void) { return S.raw.orientation >= 5 && S.raw.orientation <= 8 ? S.raw.crop_h : S.raw.crop_w; }
uint32_t oriented_h(void) { return S.raw.orientation >= 5 && S.raw.orientation <= 8 ? S.raw.crop_w : S.raw.crop_h; }

int engine_render_view(uint32_t maxw, uint32_t maxh, int hdr) {
    if (!S.raw.data) return ERR_STATE;
    int rot = S.raw.orientation >= 5 && S.raw.orientation <= 8;
    uint32_t bw = rot ? maxh : maxw, bh = rot ? maxw : maxh;
    hdr = !!hdr;
    if (!S.base.px || bw != S.base_w || bh != S.base_h || hdr != S.view_hdr) {
        if (!S.base.px || bw != S.base_w || bh != S.base_h) {
            base_free(&S.base);
            int r = base_build(&S.base, &S.lin, bw, bh);
            if (r) return r;
            S.base_w = bw; S.base_h = bh;
        }
        xfree(S.view);
        S.view = xalloc((size_t)S.base.w * S.base.h * (hdr ? 8 : 4));
        if (!S.view) return ERR_MEMORY;
        S.view_hdr = hdr;
        S.view_w = rot ? S.base.h : S.base.w; S.view_h = rot ? S.base.w : S.base.h;
    }
    base_render(&S.base, &S.rp, S.view, hdr ? OUT_RGBA_HALF : OUT_RGBA8, S.raw.orientation);
    return ERR_OK;
}

/* Renders an RGB preview of exactly (w, h) in raw orientation (letterboxed). */
static uint8_t *render_preview(uint32_t w, uint32_t h) {
    uint8_t *dst = xalloc((size_t)w * h * 3);
    if (!dst) return NULL;
    /* Previews larger than half size use the full-resolution pipeline. */
    if (w > S.raw.crop_w / 2 || h > S.raw.crop_h / 2) {
        uint32_t fw = S.raw.crop_w, fh = S.raw.crop_h;
        uint8_t *full = xalloc((size_t)fw * fh * 3);
        Full f = {0};
        if (!full || full_init(&f, &S.lin, &S.rp)) { xfree(full); xfree(dst); return NULL; }
        for (uint32_t y = 0; y < fh; y += 16) full_rows(&f, y, MIN(16, fh - y), full + (size_t)y * fw * 3, NULL);
        full_free(&f);
        if (fw == w && fh == h) { xfree(dst); return full; }
        rgb_fit(full, fw, fh, dst, w, h);
        xfree(full);
        return dst;
    }
    Base b = {0};
    if (base_build(&b, &S.lin, w, h)) { xfree(dst); return NULL; }
    uint8_t *tmp = xalloc((size_t)b.w * b.h * 3);
    if (!tmp) { base_free(&b); xfree(dst); return NULL; }
    base_render(&b, &S.rp, tmp, OUT_RGB8, 0);
    rgb_fit(tmp, b.w, b.h, dst, w, h);
    xfree(tmp); base_free(&b);
    return dst;
}
