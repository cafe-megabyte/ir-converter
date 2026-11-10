/* Session state behind the WASM API: one open image, its white balance
 * (unbounded temperature), the red/blue swap and the rendered view.
 *
 * Swap modes: SWAP_RAW exchanges the red and blue photosites before white
 * balance and colour matrix. SWAP_POST develops normally and exchanges red
 * and blue of the result in the chosen output space (sRGB, Display P3 or
 * Adobe RGB), like a channel mixer. Both use the same white balance scale:
 * internally the photosites are swapped in both modes, and SWAP_POST
 * conjugates the colour matrix with the exchange, so a temperature means the
 * same physical white balance in both and switching only changes the order of
 * swap and colour matrix. */
#include "raw.h"

enum { SWAP_NONE = 0, SWAP_RAW = 1, SWAP_POST = 2 };

typedef struct {
    uint8_t *file; uint32_t len;
    char name[256];
    Tiff tiff;
    Raw raw;
    Profile prof;
    Linear lin;
    int swap;                  /* SWAP_NONE, SWAP_RAW or SWAP_POST */
    int space;                 /* output space of SWAP_POST (SPACE_*) */
    int camera_space;          /* colour space set in the camera: 0 unknown, 1 sRGB, 2 Adobe RGB */
    M3 pcs;                    /* white-balanced camera RGB (engine channel order) -> XYZ D50 */
    double asshot[3];          /* as-shot neutral in the current channel order */
    double t0, d0, corr[3];    /* fit of the as-shot neutral and its correction */
    double temp, n[3];
    Base base; uint32_t base_w, base_h;
    Base zoom; uint32_t zoom_key[6];   /* zoomed view; key of a cached superpixel base, all 0 if none */
    Render rp;
    uint8_t *view; uint32_t view_w, view_h; int view_hdr;
    Buf out;
    int error_detail;
    char camera[160];
} Session;
static Session S;

static void swap_rb(double *v) { double t = v[0]; v[0] = v[2]; v[2] = t; }

static void session_reset(void) {
    linear_free(&S.lin); base_free(&S.base); base_free(&S.zoom); raw_free(&S.raw); tiff_free(&S.tiff);
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
static const M3 PERM_RB = {{0, 0, 1, 0, 1, 0, 1, 0, 0}};
/* Channel order of the exported file equals the engine's (swapped) order
 * only for the raw swap of a Bayer image, whose CFA pattern is rewritten. */
static int file_order_swapped(void) { return S.swap == SWAP_RAW && S.raw.spp == 1; }
/* Neutral and matrix (white-balanced camera RGB -> XYZ D50) in the channel
 * order of the exported file. The raw swap of a linear DNG keeps the samples
 * and moves the exchange into neutral and matrix. In SWAP_POST the matrix is
 * that of normal development with the physical neutral, followed by the
 * red/blue exchange in the output space. */
static M3 file_pcs(double *n) {
    if (S.swap == SWAP_NONE || file_order_swapped()) {
        for (int i = 0; i < 3; i++) n[i] = S.n[i];
        return camera_to_pcs(&S.prof, S.n, S.temp, S.d0);
    }
    n[0] = S.n[2]; n[1] = S.n[1]; n[2] = S.n[0];
    if (S.swap == SWAP_RAW) {
        M3 p = camera_to_pcs(&S.prof, S.n, S.temp, S.d0);
        return m3_mul(&p, &PERM_RB);
    }
    double t, d;
    fit_neutral(&S.prof, n, &t, &d);
    M3 p = camera_to_pcs(&S.prof, n, t, d), k = swap_in_space(S.space);
    return m3_mul(&k, &p);
}
static void update_render(void) {
    double n[3];
    M3 p = file_pcs(n);
    if (S.swap != SWAP_NONE && !file_order_swapped()) p = m3_mul(&p, &PERM_RB);   /* engine order is swapped */
    S.pcs = p;
    M3 to_srgb = xyz_to_space(SPACE_SRGB);
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

/* Colour space set in the camera for its own JPEGs (1 sRGB, 2 Adobe RGB, 0
 * unknown): Nikon MakerNote (NEF, or DNG private data), EXIF, DNG preview. */
static int nikon_space(const uint8_t *mn, uint32_t len) {
    Tiff t = {0};
    if (len < 18 || xmemcmp(mn, "Nikon\0", 6) || tiff_parse(&t, mn + 10, len - 10)) return 0;
    uint32_t v = tiff_get_uint(&t, t.ifd0, 0x001E, 0);
    tiff_free(&t);
    return v == 1 || v == 2 ? (int)v : 0;
}
static int detect_camera_space(void) {
    const Tiff *t = &S.tiff;
    int s = S.raw.nikon_mn ? nikon_space(S.raw.nikon_mn, S.raw.nikon_mn_len) : 0;
    TiffEntry *pd = tiff_find(t->ifd0, 0xC634);
    if (!s && pd && pd->size > 20 && !xmemcmp(pd->data, "Adobe\0MakN", 10)) s = nikon_space(pd->data + 20, pd->size - 20);
    TiffEntry *ex = tiff_find(t->ifd0, 0x8769);
    if (!s && ex && ex->nsub) {
        uint32_t cs = tiff_get_uint(t, ex->sub[0], 0xA001, 0);
        TiffEntry *io = tiff_find(ex->sub[0], 0xA005);
        TiffEntry *idx = io && io->nsub ? tiff_find(io->sub[0], 0x0001) : NULL;
        if (cs == 1) s = 1;
        else if (cs == 0xFFFF && idx && idx->size >= 3 && !xmemcmp(idx->data, "R03", 3)) s = 2;
    }
    if (!s) {
        uint32_t pc = tiff_get_uint(t, t->ifd0, 0xC71A, 0);
        s = pc == 2 ? 1 : pc == 3 ? 2 : 0;
    }
    return s;
}

int engine_open(uint8_t *file, uint32_t len) {
    session_reset();
    S.file = file; S.len = len;
    build_lut(&S.rp, 0);
    int r = tiff_parse(&S.tiff, file, len);
    if (r == ERR_OK) {
        r = tiff_find(S.tiff.ifd0, 0xC612) ? dng_load(&S.raw, &S.tiff) : nef_load(&S.raw, &S.tiff);
    }
    if (r == ERR_OK) {
        profile_from_raw(&S.raw, &S.prof);
        /* Default: swap in the raw data. A DNG written by IR Converter
         * already holds its swap (CFA pattern or profile) and opens as is. */
        char profile[32];
        tiff_get_str(S.tiff.ifd0, 0xC6F8, profile, sizeof profile);
        S.swap = xmemcmp(profile, APP_NAME, sizeof APP_NAME) ? SWAP_RAW : SWAP_NONE;
        S.camera_space = detect_camera_space();
        S.space = S.camera_space == 2 ? SPACE_ADOBE : SPACE_SRGB;
        r = linear_init(&S.lin, &S.raw, S.swap != SWAP_NONE);
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
int engine_set_swap(int mode) {
    if (!S.raw.data || mode < SWAP_NONE || mode > SWAP_POST) return ERR_STATE;
    if (mode == S.swap) return ERR_OK;
    int relinearise = (mode != SWAP_NONE) != (S.swap != SWAP_NONE);
    S.swap = mode;
    if (!relinearise) { update_render(); return ERR_OK; }   /* raw <-> post: same white balance */
    linear_free(&S.lin); base_free(&S.base); S.base_w = S.base_h = 0;
    base_free(&S.zoom); xmemset(S.zoom_key, 0, sizeof S.zoom_key);
    int r = linear_init(&S.lin, &S.raw, mode != SWAP_NONE);
    if (r) return r;
    setup_white();
    return ERR_OK;
}
int engine_set_space(int space) {
    if (!S.raw.data || space < 0 || space >= SPACE_COUNT) return ERR_STATE;
    S.space = space;
    update_render();
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

/* Makes S.view hold a (w, h) view in raw orientation. */
static int view_alloc(uint32_t w, uint32_t h, int hdr) {
    int rot = S.raw.orientation >= 5 && S.raw.orientation <= 8;
    uint32_t vw = rot ? h : w, vh = rot ? w : h;
    if (S.view && vw == S.view_w && vh == S.view_h && hdr == S.view_hdr) return ERR_OK;
    xfree(S.view);
    S.view = xalloc((size_t)w * h * (hdr ? 8 : 4));
    S.view_w = S.view_h = 0;
    if (!S.view) return ERR_MEMORY;
    S.view_hdr = hdr; S.view_w = vw; S.view_h = vh;
    return ERR_OK;
}
int engine_render_view(uint32_t maxw, uint32_t maxh, int hdr) {
    if (!S.raw.data) return ERR_STATE;
    int rot = S.raw.orientation >= 5 && S.raw.orientation <= 8;
    uint32_t bw = rot ? maxh : maxw, bh = rot ? maxw : maxh;
    hdr = !!hdr;
    if (!S.base.px || bw != S.base_w || bh != S.base_h) {
        base_free(&S.base);
        int r = base_build(&S.base, &S.lin, bw, bh);
        if (r) return r;
        S.base_w = bw; S.base_h = bh;
    }
    int r = view_alloc(S.base.w, S.base.h, hdr);
    if (r) return r;
    base_render(&S.base, &S.rp, S.rp.wb, S.view, hdr ? OUT_RGBA_HALF : OUT_RGBA8, S.raw.orientation);
    return ERR_OK;
}
/* Zoomed view: renders the rectangle (x, y, w, h) of the oriented crop at
 * (outw, outh), or at the rectangle's own size if that is smaller (the UI
 * magnifies further). Down to half size the view uses superpixels like the
 * full view (cached, so only colour changes per frame). Larger, the rectangle
 * is demosaiced like the JPEG for every frame, since that depends on the
 * white balance. */
int engine_render_region(double x, double y, double w, double h, uint32_t outw, uint32_t outh, int hdr) {
    if (!S.raw.data) return ERR_STATE;
    if (!(w > 0) || !(h > 0) || !outw || !outh) return ERR_STATE;
    uint32_t W = oriented_w(), H = oriented_h(), cw = S.raw.crop_w, ch = S.raw.crop_h;
    double fx0 = x < 0 ? 0 : x, fy0 = y < 0 ? 0 : y, fx1 = x + w > W ? W : x + w, fy1 = y + h > H ? H : y + h;
    if (!(fx1 > fx0) || !(fy1 > fy0)) return ERR_STATE;
    uint32_t X0 = (uint32_t)fx0, Y0 = (uint32_t)fy0, X1 = (uint32_t)-m_floor(-fx1), Y1 = (uint32_t)-m_floor(-fy1);
    if (X1 > W) X1 = W;
    if (Y1 > H) Y1 = H;
    /* The same rectangle in the crop, before orientation (see base_render). */
    uint32_t rx, ry, rw, rh;
    switch (S.raw.orientation) {
    case 2: rx = cw - X1; ry = Y0; rw = X1 - X0; rh = Y1 - Y0; break;
    case 3: rx = cw - X1; ry = ch - Y1; rw = X1 - X0; rh = Y1 - Y0; break;
    case 4: rx = X0; ry = ch - Y1; rw = X1 - X0; rh = Y1 - Y0; break;
    case 5: rx = Y0; ry = X0; rw = Y1 - Y0; rh = X1 - X0; break;
    case 6: rx = Y0; ry = ch - X1; rw = Y1 - Y0; rh = X1 - X0; break;
    case 7: rx = cw - Y1; ry = ch - X1; rw = Y1 - Y0; rh = X1 - X0; break;
    case 8: rx = cw - Y1; ry = X0; rw = Y1 - Y0; rh = X1 - X0; break;
    default: rx = X0; ry = Y0; rw = X1 - X0; rh = Y1 - Y0; break;
    }
    int rot = S.raw.orientation >= 5 && S.raw.orientation <= 8;
    uint32_t bw = rot ? outh : outw, bh = rot ? outw : outh;
    if (bw > rw) bw = rw;
    if (bh > rh) bh = rh;
    hdr = !!hdr;
    const float *wb = S.rp.wb;
    static const float ONE[3] = {1, 1, 1};
    if (2 * bw <= rw && 2 * bh <= rh) {
        uint32_t i0 = rx / 2, j0 = ry / 2, hw = (rx + rw + 1) / 2 - i0, hh = (ry + rh + 1) / 2 - j0;
        if (i0 + hw > cw / 2) hw = cw / 2 - i0;
        if (j0 + hh > ch / 2) hh = ch / 2 - j0;
        if (!hw || !hh) return ERR_STATE;
        if (bw > hw) bw = hw;
        if (bh > hh) bh = hh;
        uint32_t key[6] = {i0 + 1, j0, hw, hh, bw, bh};
        if (!S.zoom.px || xmemcmp(key, S.zoom_key, sizeof key)) {
            base_free(&S.zoom); xmemset(S.zoom_key, 0, sizeof S.zoom_key);
            int r = base_build_rect(&S.zoom, &S.lin, i0, j0, hw, hh, bw, bh);
            if (r) return r;
            xmemcpy(S.zoom_key, key, sizeof key);
        }
    } else {
        base_free(&S.zoom); xmemset(S.zoom_key, 0, sizeof S.zoom_key);
        int r = region_build(&S.zoom, &S.lin, wb, rx, ry, rw, rh, bw, bh);
        if (r) return r;
        wb = ONE;
    }
    int r = view_alloc(S.zoom.w, S.zoom.h, hdr);
    if (r) return r;
    base_render(&S.zoom, &S.rp, wb, S.view, hdr ? OUT_RGBA_HALF : OUT_RGBA8, S.raw.orientation);
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
    base_render(&b, &S.rp, S.rp.wb, tmp, OUT_RGB8, 0);
    rgb_fit(tmp, b.w, b.h, dst, w, h);
    xfree(tmp); base_free(&b);
    return dst;
}
