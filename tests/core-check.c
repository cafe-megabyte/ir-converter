/* Native checks of the core against the sample files samples/DSC_0080.NEF
 * and samples/DSC_0080.DNG (the same NEF converted by Adobe DNG Converter):
 * bit-exact NEF decoding (vs. Adobe DNG Converter), unbounded temperature
 * round trips, lossless DNG export (DNG and NEF sources), previews of the
 * original sizes and JPEG export. Writes exports to build/internal/tests/. */
#define IRCONV_NATIVE 1
#define SAMPLE_NEF "samples/DSC_0080.NEF"
#define SAMPLE_DNG "samples/DSC_0080.DNG"
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

int main(void) {
    const char *dir = "build/internal/tests";
    char path[512], sizes[256], sizes2[256];
    check_math();
    check_hdr_math();

    /* 1. NEF and DNG Converter DNG: identical raw codes. */
    uint32_t na, nb;
    uint8_t *nef = load(SAMPLE_NEF, &na), *dng = load(SAMPLE_DNG, &nb);
    if (!nef || !dng) { printf("Sample files missing (" SAMPLE_NEF ", " SAMPLE_DNG "); skipping image checks.\n"); return failures != 0; }
    Tiff ta, tb; Raw ra = {0}, rb = {0};
    CHECK(!tiff_parse(&ta, nef, na) && !nef_load(&ra, &ta), "NEF load");
    CHECK(!tiff_parse(&tb, dng, nb) && !dng_load(&rb, &tb), "DNG load");
    uint64_t diff = 0;
    for (uint32_t y = 0; y < rb.height; y++) for (uint32_t x = 0; x < rb.width; x++)
        diff += ra.data[(uint64_t)y * ra.width + x] != rb.data[(uint64_t)y * rb.width + x];
    CHECK(diff == 0, "NEF codes differ from DNG Converter in %llu pixels", (unsigned long long)diff);
    CHECK(ra.crop_x == rb.crop_x && ra.crop_y == rb.crop_y && ra.crop_w == rb.crop_w && ra.crop_h == rb.crop_h, "crop");
    for (int i = 0; i < 4; i++) CHECK(ra.cfa[i] == rb.cfa[i], "cfa");
    for (int i = 0; i < 3; i++) CHECK(m_abs(ra.as_shot[i] / ra.as_shot[0] - rb.as_shot[i] / rb.as_shot[0]) < 1e-4, "as-shot neutral");
    printf("NEF decode: %ux%u codes identical to DNG Converter output\n", rb.width, rb.height);

    /* 2. Temperature model on the DNG. */
    CHECK(open_file(SAMPLE_DNG) == 0, "engine open DNG");
    printf("As-shot temperature: %.1f K (tint %.4f)\n", S.t0, S.d0);
    check_temperatures();
    CHECK(engine_set_swap(1) == 0, "swap");
    printf("As-shot temperature with R/B swap: %.1f K\n", S.t0);
    check_temperatures();

    /* 3. DNG -> DNG, with and without swap, at an extreme temperature. */
    for (int swap = 0; swap < 2; swap++) {
        uint32_t n0; uint8_t *src = load(SAMPLE_DNG, &n0);
        Tiff t0; tiff_parse(&t0, src, n0); preview_sizes(&t0, sizes);
        int proxies0 = count_proxies(&t0);
        CHECK(open_file(SAMPLE_DNG) == 0, "open " SAMPLE_DNG);
        engine_set_swap(swap);
        engine_set_temperature(swap ? 40000 : 1500);
        double n[3] = {S.n[0], S.n[1], S.n[2]};
        CHECK(engine_export_dng() == 0, "export DNG");
        snprintf(path, sizeof path, "%s/DNG-%s.dng", dir, swap ? "swap" : "plain");
        save(path, S.out.data, S.out.len);
        Tiff t1; Raw r1;
        uint8_t *copy = xalloc(S.out.len); memcpy(copy, S.out.data, S.out.len); uint32_t n1 = S.out.len;
        CHECK(load_buffer(copy, n1, &t1, &r1) == 0, "reload export");
        Raw r0; Tiff tr; load_buffer(src, n0, &tr, &r0);
        CHECK(r1.width == r0.width && r1.height == r0.height && !memcmp(r1.data, r0.data, (size_t)r0.width * r0.height * 2), "raw data not bit-exact");
        for (int i = 0; i < 4; i++) CHECK(r1.cfa[i] == (swap ? 2 - r0.cfa[i] : r0.cfa[i]), "CFA pattern");
        for (int i = 0; i < 3; i++) CHECK(m_abs(r1.as_shot[i] - n[i]) < 1e-6, "AsShotNeutral");
        preview_sizes(&t1, sizes2);
        CHECK(!strcmp(sizes, sizes2), "preview sizes changed");
        printf("DNG -> DNG %s: previews before [%s] after [%s], proxies %d -> %d, %u bytes\n", swap ? "swap" : "plain",
               sizes, sizes2, proxies0, count_proxies(&t1), n1);
        CHECK(count_proxies(&t1) == (swap ? 0 : proxies0), "fast-load proxies");
        /* Reference rendering of the same state for raw-reference. */
        CHECK(engine_export_jpeg() == 0, "export JPEG");
        snprintf(path, sizeof path, "%s/DNG-%s.jpg", dir, swap ? "swap" : "plain");
        save(path, S.out.data, S.out.len);
        snprintf(path, sizeof path, "%s/DNG-%s.dng", dir, swap ? "swap" : "plain");
        /* The exported file reopens with the chosen white balance. */
        CHECK(open_file(path) == 0, "reopen export");
        for (int i = 0; i < 3; i++) CHECK(m_abs(S.asshot[i] - n[i]) < 1e-6, "reopened neutral");
        tiff_free(&t1); raw_free(&r1); xfree(copy); tiff_free(&tr); raw_free(&r0); tiff_free(&t0); xfree(src);
    }

    /* 4. NEF -> DNG: codes and curve preserved; previews of the NEF's sizes. */
    for (int swap = 0; swap < 2; swap++) {
        CHECK(open_file(SAMPLE_NEF) == 0, "open NEF");
        memcpy(S.name, "DSC_0080.NEF", 13);
        engine_set_swap(swap);
        CHECK(engine_export_dng() == 0, "export NEF->DNG");
        snprintf(path, sizeof path, "%s/NEF-%s.dng", dir, swap ? "swap" : "plain");
        save(path, S.out.data, S.out.len);
        Tiff t1; Raw r1;
        uint8_t *copy = xalloc(S.out.len); memcpy(copy, S.out.data, S.out.len);
        CHECK(load_buffer(copy, S.out.len, &t1, &r1) == 0, "reload NEF export");
        CHECK(r1.width == ra.width && !memcmp(r1.data, ra.data, (size_t)ra.width * ra.height * 2), "NEF codes not preserved");
        CHECK(r1.lin_n == ra.lin_n && !memcmp(r1.lin, ra.lin, ra.lin_n * 2), "NEF curve not preserved");
        CHECK(r1.crop_w == 4288 && r1.crop_h == 2848, "NEF export crop");
        preview_sizes(&t1, sizes2);
        printf("NEF -> DNG %s: previews [%s], %u bytes\n", swap ? "swap" : "plain", sizes2, S.out.len);
        tiff_free(&t1); raw_free(&r1); xfree(copy);
    }

    /* 5. JPEG export. */
    CHECK(open_file(SAMPLE_NEF) == 0, "open NEF");
    engine_set_swap(1);
    engine_set_temperature(30000);
    CHECK(engine_export_jpeg() == 0, "export JPEG");
    CHECK(S.out.len > 100000 && S.out.data[0] == 0xFF && S.out.data[1] == 0xD8, "JPEG data");
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

    printf(failures ? "%d core check(s) failed\n" : "Core checks passed\n", failures);
    return failures != 0;
}
