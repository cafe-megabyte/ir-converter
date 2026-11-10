/* Native checks of the core with the sample file samples/DSC_0080.NEF:
 * unbounded temperature round trips, lossless DNG export in every swap mode
 * (from the NEF, and from a DNG: the NEF's own export), previews of the
 * original sizes, JPEG export in every output space, zoomed views.
 * Writes exports to build/internal/tests/. */
#define IRCONV_NATIVE 1
#define SAMPLE_NEF "samples/DSC_0080.NEF"
#define SOURCE_DNG "build/internal/tests/NEF-plain.dng"   /* written in step 2 */
#include "../core/irconv.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = xalloc((size_t)len);
    if (fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); xfree(d); return NULL; }
    fclose(f); *n = (uint32_t)len;
    return d;
}
static void save(const char *path, const uint8_t *d, uint32_t n) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}
static int open_file(const char *path) {
    uint32_t n; uint8_t *d = load(path, &n);
    if (!d) return -1;
    int r = engine_open(d, n);
    if (r) xfree(d);
    return r;
}
/* Loads a DNG produced by an export into a separate Raw for comparison. */
static int load_buffer(const uint8_t *d, uint32_t n, Tiff *t, Raw *r) {
    xmemset(r, 0, sizeof(*r));
    int e = tiff_parse(t, d, n);
    if (!e) e = dng_load(r, t);
    return e;
}
static void preview_sizes(const Tiff *t, char *out) {
    out[0] = 0;
    const TiffIfd *list[20]; int n = 0;
    list[n++] = t->ifd0;
    TiffEntry *s = tiff_find(t->ifd0, 0x014A);
    for (uint32_t i = 0; s && i < s->nsub; i++) list[n++] = s->sub[i];
    for (int i = 0; i < n; i++) {
        uint32_t ph = tiff_get_uint(t, list[i], 0x0106, 0);
        if (tiff_get_uint(t, list[i], 0x00FE, 0) == 1 && (ph == 2 || ph == 6))
            sprintf(out + strlen(out), "%ux%u/%u ", tiff_get_uint(t, list[i], 0x0100, 0), tiff_get_uint(t, list[i], 0x0101, 0),
                    tiff_get_uint(t, list[i], 0x0103, 0));
    }
}
static int count_proxies(const Tiff *t) {
    int n = 0;
    TiffEntry *s = tiff_find(t->ifd0, 0x014A);
    for (uint32_t i = 0; s && i < s->nsub; i++)
        if (tiff_get_uint(t, s->sub[i], 0x00FE, 0) == 1 && tiff_get_uint(t, s->sub[i], 0x0106, 0) == 34892) n++;
    return n;
}

static void check_math(void) {
    CHECK(m_abs(m_exp(1) - 2.718281828459045) < 1e-14, "exp");
    CHECK(m_abs(m_log(10) - 2.302585092994046) < 1e-14, "log");
    CHECK(m_abs(m_pow(2, 0.5) - 1.4142135623730951) < 1e-14, "pow");
    CHECK(m_abs(m_exp(-700) / 9.85967654375977e-305 - 1) < 1e-12, "exp small");
}
static float from_half(uint16_t h) {
    int e = (h >> 10) & 31, m = h & 1023;
    float v = e ? (float)(1024 + m) * (float)m_pow(2, e - 25) : (float)m * (float)m_pow(2, -24);
    return h & 0x8000 ? -v : v;
}
static void check_hdr_math(void) {
    CHECK(to_half(1.0f) == 0x3C00 && to_half(0.5f) == 0x3800 && to_half(2.0f) == 0x4000 && to_half(65504.0f) == 0x7BFF, "half floats");
    CHECK(to_half(1.0f / 3) == 0x3555 && to_half(1e-7f) == 0x0002 && to_half(0) == 0, "half rounding");
    /* Bradford D50->D65 times XYZ->sRGB must give the known D50 sRGB matrix. */
    static const M3 bradford = {{0.9555766, -0.0230393, 0.0631636, -0.0282895, 1.0099416, 0.0210077, 0.0122982, -0.0204830, 1.3299098}};
    static const M3 srgb65 = {{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252}};
    static const double srgb50[9] = {3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415, 0.0334540, 0.0719453, -0.2289914, 1.4052427};
    M3 m = m3_mul(&srgb65, &bradford);
    for (int i = 0; i < 9; i++) CHECK(m_abs(m.m[i] - srgb50[i]) < 2e-4, "Bradford matrix %d", i);
    /* Display P3 maps D65 white to (1, 1, 1). */
    M3 p3 = xyz_to_display_p3();
    double w[3] = {0.3127 / 0.3290, 1.0, (1 - 0.3127 - 0.3290) / 0.3290}, o[3];
    m3_vec(&p3, w, o);
    for (int i = 0; i < 3; i++) CHECK(m_abs(o[i] - 1) < 1e-9, "P3 white");
    /* Same construction reproduces the published sRGB matrix (D65). */
    static const double r[2] = {0.64, 0.33}, g[2] = {0.30, 0.60}, b[2] = {0.15, 0.06}, wp[2] = {0.3127, 0.3290};
    M3 s = xyz_to_rgb_primaries(r, g, b, wp);
    for (int i = 0; i < 9; i++) CHECK(m_abs(s.m[i] - srgb65.m[i]) < 2e-3, "sRGB from primaries %d: %f", i, s.m[i]);
    printf("Display P3 row 0: %.7f %.7f %.7f\n", p3.m[0], p3.m[1], p3.m[2]);
}
/* Output spaces: the embedded ICC profiles have the transfer curves the
 * encoder assumes, and the red/blue exchange is an involution keeping white. */
static void check_spaces(void) {
    uint32_t len;
    const uint8_t *trc = icc_tag(SPACES[SPACE_SRGB].icc, SPACES[SPACE_SRGB].icc_len, "rTRC", &len);
    CHECK(trc && !memcmp(trc, "curv", 4) && rd32(trc + 8, 1) == 1024, "sRGB TRC table");
    for (uint32_t i = 1; trc && i < 1024; i += 97) {
        double v = rd16(trc + 12 + 2 * i, 1) / 65535.0, g = i / 1023.0;
        CHECK(m_abs(v - srgb_decode(g)) < 2e-3, "sRGB TRC %u: %f", i, v);
    }
    trc = icc_tag(SPACES[SPACE_P3].icc, SPACES[SPACE_P3].icc_len, "rTRC", &len);
    CHECK(trc && !memcmp(trc, "para", 4) && rd16(trc + 8, 1) == 3, "Display P3 TRC type");
    static const double srgb_para[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
    for (int i = 0; trc && i < 5; i++) CHECK(m_abs((int32_t)rd32(trc + 12 + 4 * i, 1) / 65536.0 - srgb_para[i]) < 2e-4, "P3 TRC parameter %d", i);
    trc = icc_tag(SPACES[SPACE_ADOBE].icc, SPACES[SPACE_ADOBE].icc_len, "rTRC", &len);
    CHECK(trc && !memcmp(trc, "curv", 4) && rd32(trc + 8, 1) == 1 && rd16(trc + 12, 1) == 563, "Adobe RGB gamma 563/256");
    /* The sRGB colorants match the published D50 sRGB matrix. */
    static const double srgb50[9] = {3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415, 0.0334540, 0.0719453, -0.2289914, 1.4052427};
    M3 to = xyz_to_space(SPACE_SRGB);
    for (int i = 0; i < 9; i++) CHECK(m_abs(to.m[i] - srgb50[i]) < 1e-3, "sRGB colorants %d", i);
    for (int sp = 0; sp < SPACE_COUNT; sp++) {
        M3 k = swap_in_space(sp), kk = m3_mul(&k, &k);
        double w[3];
        m3_vec(&k, D50, w);
        for (int i = 0; i < 9; i++) CHECK(m_abs(kk.m[i] - (i % 4 == 0)) < 1e-9, "swap in space %d is not an involution", sp);
        for (int i = 0; i < 3; i++) CHECK(m_abs(w[i] / D50[i] - 1) < 1e-3, "swap in space %d moves white", sp);
    }
}
/* Mean and maximum difference of two 8-bit views. */
static void view_diff(const uint8_t *a, const uint8_t *b, size_t n, double *mean, int *max) {
    double sum = 0; *max = 0;
    for (size_t i = 0; i < n; i++) { int d = abs(a[i] - b[i]); sum += d; if (d > *max) *max = d; }
    *mean = sum / (n ? n : 1);
}
static uint8_t *view_copy(void) {
    if (engine_render_view(1072, 712, 0)) return NULL;
    uint8_t *v = xalloc(S.view_w * S.view_h * 4);
    if (v) memcpy(v, S.view, S.view_w * S.view_h * 4);
    return v;
}
static int contains(const uint8_t *hay, uint32_t n, const uint8_t *needle, uint32_t m) {
    for (uint32_t i = 0; i + m <= n; i++) if (hay[i] == needle[0] && !memcmp(hay + i, needle, m)) return 1;
    return 0;
}
static const char *MODE[3] = {"plain", "swap", "post"};

static void check_temperatures(void) {
    static const double temps[] = {10, 300, 1000, 2000, 3500, 6500, 12000, 50000, 1e6};
    for (unsigned i = 0; i < sizeof temps / sizeof temps[0]; i++) {
        double n[3], t, d;
        CHECK(model_neutral_ext(&S.prof, temps[i], 0.01, n), "no neutral at %g", temps[i]);
        fit_neutral(&S.prof, n, &t, &d);
        CHECK(m_abs(t / temps[i] - 1) < 1e-4 && m_abs(d - 0.01) < 1e-5, "temperature round trip %g -> %g (tint %g)", temps[i], t, d);
    }
    /* The slider must pass exactly through the as-shot neutral. */
    double n[3];
    neutral_for(S.t0, n);
    for (int i = 0; i < 3; i++) CHECK(m_abs(n[i] - S.asshot[i]) < 1e-9, "as-shot neutral reproduction");
    /* Unbounded and monotonic: higher temperature -> bluer neutral, without
     * saturation at either end (each doubling changes the ratio). */
    double prev = 1e300;
    for (double t = 1; t < 1e9; t *= 2) {
        neutral_for(t, n);
        double ratio = n[0] / n[2];
        CHECK(ratio < prev * 0.999, "red/blue neutral ratio not decreasing at %g K (%g -> %g)", t, prev, ratio);
        prev = ratio;
    }
}

/* Zoomed views of the open image: at 1:1 they equal the JPEG pipeline pixel
 * for pixel in every orientation, and the whole image at the fit size equals
 * the normal view. Saves the views compared by wasm-check. */
static void check_zoom(const char *dir) {
    char path[512];
    uint32_t cw = S.raw.crop_w, ch = S.raw.crop_h;
    uint8_t *full = xalloc((size_t)cw * ch * 3);
    Full f = {0};
    CHECK(full && full_init(&f, &S.lin, &S.rp) == 0, "zoom reference");
    if (!full || !f.win) { xfree(full); full_free(&f); return; }
    for (uint32_t y = 0; y < ch; y += 16) full_rows(&f, y, MIN(16, ch - y), full + (size_t)y * cw * 3, NULL);
    full_free(&f);
    int orient0 = S.raw.orientation, bad = 0;
    for (int o = 1; o <= 8; o++) {
        S.raw.orientation = o;
        int rot = o >= 5;
        /* At the crop corner (mirrored neighbours) and inside, odd offsets. */
        static const double R[2][4] = {{0, 0, 97, 61}, {1001, 703, 300, 200}};
        for (int k = 0; k < 2; k++) {
            uint32_t X0 = (uint32_t)R[k][0], Y0 = (uint32_t)R[k][1], w = (uint32_t)R[k][2], h = (uint32_t)R[k][3];
            if (engine_render_region(X0, Y0, w, h, w, h, 0) || S.view_w != w || S.view_h != h) { bad++; continue; }
            uint32_t W = rot ? ch : cw, H = rot ? cw : ch;
            for (uint32_t Y = Y0; Y < Y0 + h; Y++)
                for (uint32_t X = X0; X < X0 + w; X++) {
                    uint32_t x, y;   /* inverse of the mapping in base_render */
                    switch (o) {
                    case 2: x = W - 1 - X; y = Y; break;
                    case 3: x = W - 1 - X; y = H - 1 - Y; break;
                    case 4: x = X; y = H - 1 - Y; break;
                    case 5: x = Y; y = X; break;
                    case 6: x = Y; y = W - 1 - X; break;
                    case 7: x = H - 1 - Y; y = W - 1 - X; break;
                    case 8: x = H - 1 - Y; y = X; break;
                    default: x = X; y = Y; break;
                    }
                    const uint8_t *a = S.view + ((size_t)(Y - Y0) * w + (X - X0)) * 4, *b = full + ((size_t)y * cw + x) * 3;
                    if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) bad++;
                }
        }
    }
    S.raw.orientation = orient0;
    CHECK(!bad, "zoomed view at 1:1 differs from the JPEG pipeline (%d)", bad);
    xfree(full);
    /* Whole image at the fit size: superpixels, identical to the normal view. */
    uint8_t *fit = view_copy();
    CHECK(fit && engine_render_region(0, 0, oriented_w(), oriented_h(), 1072, 712, 0) == 0 && S.view_w == 1072
          && !memcmp(fit, S.view, (size_t)1072 * 712 * 4), "whole image zoomed differs from the view");
    xfree(fit);
    CHECK(engine_render_region(1001.5, 703.25, 1200, 800, 400, 266, 0) == 0 && S.view_w == 400, "zoomed superpixel view");
    snprintf(path, sizeof path, "%s/view-zoom-half.rgba", dir);
    save(path, S.view, S.view_w * S.view_h * 4);
    CHECK(engine_render_region(1001.5, 703.25, 600, 400, 450, 300, 0) == 0 && S.view_w == 450, "zoomed demosaiced view");
    snprintf(path, sizeof path, "%s/view-zoom.rgba", dir);
    save(path, S.view, S.view_w * S.view_h * 4);
    CHECK(engine_render_region(1001.5, 703.25, 600, 400, 450, 300, 1) == 0 && S.view_w == 450, "zoomed HDR view");
    snprintf(path, sizeof path, "%s/view-zoom-hdr.bin", dir);
    save(path, S.view, S.view_w * S.view_h * 8);
    CHECK(engine_render_region(5000, 0, 10, 10, 10, 10, 0) != 0, "zoom outside the image is refused");
    printf("Zoomed views: 1:1 equals the JPEG pipeline in all orientations\n");
}

int main(void) {
    const char *dir = "build/internal/tests";
    char path[512], sizes[256], sizes2[256];
    check_math();
    check_hdr_math();
    check_spaces();

    /* 1. NEF: defaults and the temperature model. */
    uint32_t na;
    uint8_t *nef = load(SAMPLE_NEF, &na);
    if (!nef) { printf("Sample file missing (" SAMPLE_NEF "); skipping image checks.\n"); return failures != 0; }
    Tiff ta; Raw ra = {0};
    CHECK(!tiff_parse(&ta, nef, na) && !nef_load(&ra, &ta), "NEF load");
    CHECK(open_file(SAMPLE_NEF) == 0, "engine open NEF");
    CHECK(S.swap == SWAP_RAW && S.camera_space == 1 && S.space == SPACE_SRGB, "defaults: swap %d, camera space %d, space %d",
          S.swap, S.camera_space, S.space);
    CHECK(engine_set_swap(SWAP_NONE) == 0, "no swap");
    printf("As-shot temperature: %.1f K (tint %.4f)\n", S.t0, S.d0);
    check_temperatures();
    CHECK(engine_set_swap(SWAP_RAW) == 0, "swap");
    printf("As-shot temperature with R/B swap: %.1f K\n", S.t0);
    check_temperatures();
    /* Raw swap <-> swap after development: same temperature scale. */
    engine_set_temperature(5000);
    CHECK(engine_set_swap(SWAP_POST) == 0 && S.temp == 5000 && engine_set_swap(SWAP_RAW) == 0 && S.temp == 5000, "swap modes keep the temperature");
    CHECK(engine_set_swap(3) != 0 && engine_set_space(3) != 0, "invalid swap mode or space accepted");

    /* 2. NEF -> DNG: codes and curve preserved; previews of the NEF's sizes.
     * The plain export is the DNG source of step 3. */
    for (int swap = 0; swap < 3; swap++) {
        CHECK(open_file(SAMPLE_NEF) == 0, "open NEF");
        CHECK(S.camera_space == 1, "NEF camera colour space %d", S.camera_space);
        memcpy(S.name, "DSC_0080.NEF", 13);
        engine_set_swap(swap);
        CHECK(engine_export_dng() == 0, "export NEF->DNG");
        snprintf(path, sizeof path, "%s/NEF-%s.dng", dir, MODE[swap]);
        save(path, S.out.data, S.out.len);
        Tiff t1; Raw r1;
        uint8_t *copy = xalloc(S.out.len); memcpy(copy, S.out.data, S.out.len);
        CHECK(load_buffer(copy, S.out.len, &t1, &r1) == 0, "reload NEF export");
        CHECK(r1.width == ra.width && !memcmp(r1.data, ra.data, (size_t)ra.width * ra.height * 2), "NEF codes not preserved");
        CHECK(r1.lin_n == ra.lin_n && !memcmp(r1.lin, ra.lin, ra.lin_n * 2), "NEF curve not preserved");
        CHECK(r1.crop_w == 4288 && r1.crop_h == 2848, "NEF export crop");
        for (int i = 0; i < 4; i++) CHECK(r1.cfa[i] == (swap == SWAP_RAW ? 2 - ra.cfa[i] : ra.cfa[i]), "NEF export CFA pattern");
        preview_sizes(&t1, sizes2);
        printf("NEF -> DNG %s: previews [%s], %u bytes\n", MODE[swap], sizes2, S.out.len);
        tiff_free(&t1); raw_free(&r1); xfree(copy);
    }

    /* 3. DNG -> DNG in every swap mode, at extreme temperatures. The swap
     * after development (here in Adobe RGB) keeps the photosites and puts
     * the exchange into the profile; reopening must reproduce the rendering. */
    CHECK(open_file(SOURCE_DNG) == 0 && S.swap == SWAP_NONE, "own DNG export opens without swap");
    for (int swap = 0; swap < 3; swap++) {
        uint32_t n0; uint8_t *src = load(SOURCE_DNG, &n0);
        Tiff t0; tiff_parse(&t0, src, n0); preview_sizes(&t0, sizes);
        int proxies0 = count_proxies(&t0);
        CHECK(open_file(SOURCE_DNG) == 0, "open " SOURCE_DNG);
        engine_set_swap(swap);
        if (swap == SWAP_POST) engine_set_space(SPACE_ADOBE);
        /* The source is an export: its profile reads the as-shot white as 5000 K.
         * These match 1500 K, 40000 K and 6500 K of the NEF physically. */
        engine_set_temperature(swap == SWAP_RAW ? 100000 : swap == SWAP_POST ? 60000 : 4200);
        double n[3] = {S.n[0], S.n[1], S.n[2]};
        if (swap == SWAP_POST) { n[0] = S.n[2]; n[2] = S.n[0]; }   /* the file holds the physical neutral */
        uint8_t *before = view_copy();
        CHECK(engine_export_dng() == 0, "export DNG");
        snprintf(path, sizeof path, "%s/DNG-%s.dng", dir, MODE[swap]);
        save(path, S.out.data, S.out.len);
        Tiff t1; Raw r1;
        uint8_t *copy = xalloc(S.out.len); memcpy(copy, S.out.data, S.out.len); uint32_t n1 = S.out.len;
        CHECK(load_buffer(copy, n1, &t1, &r1) == 0, "reload export");
        Raw r0; Tiff tr; load_buffer(src, n0, &tr, &r0);
        CHECK(r1.width == r0.width && r1.height == r0.height && !memcmp(r1.data, r0.data, (size_t)r0.width * r0.height * 2), "raw data not bit-exact");
        for (int i = 0; i < 4; i++) CHECK(r1.cfa[i] == (swap == SWAP_RAW ? 2 - r0.cfa[i] : r0.cfa[i]), "CFA pattern");
        for (int i = 0; i < 3; i++) CHECK(m_abs(r1.as_shot[i] - n[i]) < 1e-6, "AsShotNeutral");
        preview_sizes(&t1, sizes2);
        CHECK(!strcmp(sizes, sizes2), "preview sizes changed");
        printf("DNG -> DNG %s: previews before [%s] after [%s], proxies %d -> %d, %u bytes\n", MODE[swap],
               sizes, sizes2, proxies0, count_proxies(&t1), n1);
        CHECK(count_proxies(&t1) == (swap == SWAP_RAW ? 0 : proxies0), "fast-load proxies");
        /* Reference rendering of the same state for raw-reference. */
        CHECK(engine_export_jpeg() == 0, "export JPEG");
        snprintf(path, sizeof path, "%s/DNG-%s.jpg", dir, MODE[swap]);
        save(path, S.out.data, S.out.len);
        snprintf(path, sizeof path, "%s/DNG-%s.dng", dir, MODE[swap]);
        /* The exported file reopens as is (no second swap), with the chosen
         * white balance and the same rendering. */
        CHECK(open_file(path) == 0, "reopen export");
        CHECK(S.swap == SWAP_NONE, "own export reopens with swap mode %d", S.swap);
        for (int i = 0; i < 3; i++) CHECK(m_abs(S.asshot[i] - n[i]) < 1e-6, "reopened neutral");
        uint8_t *after = view_copy();
        double mean; int max;
        view_diff(before, after, S.view_w * S.view_h * 4, &mean, &max);
        CHECK(max <= 2, "reopened %s export renders differently (max %d, mean %.3f)", MODE[swap], max, mean);
        xfree(before); xfree(after);
        tiff_free(&t1); raw_free(&r1); xfree(copy); tiff_free(&tr); raw_free(&r0); tiff_free(&t0); xfree(src);
    }

    /* 4. JPEG export: sRGB, and after development in Display P3 / Adobe RGB
     * with the matching ICC profile. */
    CHECK(open_file(SAMPLE_NEF) == 0 && engine_set_swap(SWAP_POST) == 0, "open NEF");
    for (int sp = SPACE_P3; sp < SPACE_COUNT; sp++) {
        engine_set_space(sp);
        CHECK(engine_export_jpeg() == 0, "export JPEG in space %d", sp);
        CHECK(contains(S.out.data, S.out.len, SPACES[sp].icc, SPACES[sp].icc_len), "JPEG lacks the ICC profile of space %d", sp);
        CHECK(!contains(S.out.data, S.out.len, srgb_icc, srgb_icc_len), "JPEG in space %d carries the sRGB profile", sp);
    }
    engine_set_swap(SWAP_RAW);
    engine_set_temperature(30000);
    CHECK(engine_export_jpeg() == 0, "export JPEG");
    CHECK(S.out.len > 100000 && S.out.data[0] == 0xFF && S.out.data[1] == 0xD8, "JPEG data");
    CHECK(contains(S.out.data, S.out.len, srgb_icc, srgb_icc_len), "JPEG lacks the sRGB profile");
    snprintf(path, sizeof path, "%s/NEF-swap.jpg", dir);
    save(path, S.out.data, S.out.len);
    printf("JPEG: %u bytes\n", S.out.len);
    CHECK(engine_render_view(1600, 1200, 0) == 0 && S.view_w == 1600, "view render %ux%u", S.view_w, S.view_h);
    snprintf(path, sizeof path, "%s/view.rgba", dir);
    save(path, S.view, S.view_w * S.view_h * 4);
    /* HDR view: half floats, highlights above SDR white, midtones unchanged. */
    uint8_t *sdr = xalloc(S.view_w * S.view_h * 4);
    memcpy(sdr, S.view, S.view_w * S.view_h * 4);
    CHECK(engine_render_view(1600, 1200, 1) == 0 && S.view_w == 1600, "HDR view render");
    const uint16_t *hv = (const uint16_t *)S.view;
    float top = 0; int bad = 0, gray = 0, graydiff = 0;
    for (uint32_t i = 0; i < S.view_w * S.view_h; i++) {
        for (int c = 0; c < 3; c++) { float v = from_half(hv[i * 4 + c]); if (!(v >= 0 && v < 4)) bad++; if (v > top) top = v; }
        /* Near-neutral midtones: HDR (P3) and SDR (sRGB) agree. */
        const uint8_t *q = sdr + i * 4;
        if (q[1] > 60 && q[1] < 120 && abs(q[0] - q[1]) < 4 && abs(q[2] - q[1]) < 4) {
            gray++;
            if (fabsf(from_half(hv[i * 4 + 1]) * 255 - q[1]) > 3) graydiff++;
        }
    }
    CHECK(!bad && top > 1.5f, "HDR values (max %.2f, invalid %d)", top, bad);
    CHECK(gray > 1000 && graydiff < gray / 100, "HDR midtones differ from SDR (%d of %d)", graydiff, gray);
    printf("HDR view: peak %.2f x SDR white (encoded), %d neutral midtone pixels match SDR\n", top, gray - graydiff);
    snprintf(path, sizeof path, "%s/view-hdr.bin", dir);
    save(path, S.view, S.view_w * S.view_h * 8);
    xfree(sdr);
    check_zoom(dir);

    printf(failures ? "%d core check(s) failed\n" : "Core checks passed\n", failures);
    return failures != 0;
}
