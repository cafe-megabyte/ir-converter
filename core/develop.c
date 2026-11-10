/* Raw rendering: linearisation, white balance, highlight roll-off,
 * demosaicing (superpixel for previews, Malvar-He-Cutler at full size and for
 * zoomed previews; linear DNGs are already demosaiced and use their RGB
 * directly),
 * camera -> sRGB matrix, tone curve and sRGB encoding. The screen preview can
 * also be rendered for HDR displays: Display P3, extended range half floats. */
#include "raw.h"

/* HDR preview: midtones identical to SDR; from HDR_KNEE upwards highlights
 * brighten smoothly up to HDR_PEAK times SDR white (displays clip beyond
 * their own headroom). */
#define HDR_KNEE 0.55f
#define HDR_PEAK 3.0
typedef struct {
    float m[9];            /* white-balanced camera -> linear sRGB (incl. exposure) */
    float m_p3[9];         /* white-balanced camera -> linear Display P3 (incl. exposure) */
    float wb[3];           /* multipliers 1/neutral, smallest = 1 */
    uint8_t lut[4097];     /* linear [0,1] -> 8-bit sRGB with tone curve */
    float enc[4097];       /* linear [0,1] -> encoded value with tone curve (float) */
    float gain[1025];      /* HDR highlight gain over the knee parameter t in [0,1] */
    float boost[1025];     /* gain^(1/2.4): the same gain in encoded values */
    uint8_t gm[1025];      /* gain map code: 255 * log2(gain) / log2(HDR_PEAK) */
} Render;

typedef struct {
    const Raw *raw;
    int swap;              /* exchange red and blue photosites (linear DNG: samples) */
    float *lin[4];         /* per CFA parity (linear DNG: per sample): code -> normalised linear value */
    uint32_t codes;
} Linear;

static inline int color_at(const Raw *r, int swap, uint32_t y, uint32_t x) {
    int c = r->cfa[(y & 1) * 2 + (x & 1)];
    return swap ? 2 - c : c;
}

static void linear_free(Linear *l) { for (int i = 0; i < 4; i++) { xfree(l->lin[i]); l->lin[i] = NULL; } }
static int linear_init(Linear *l, const Raw *r, int swap) {
    xmemset(l, 0, sizeof(*l));
    l->raw = r; l->swap = swap;
    uint32_t top = 0;
    uint64_t n = (uint64_t)r->width * r->height * (r->spp == 3 ? 3 : 1);
    for (uint64_t i = 0; i < n; i++) if (r->data[i] > top) top = r->data[i];
    l->codes = top + 1;
    for (int p = 0; p < 4; p++) {
        l->lin[p] = xalloc(l->codes * sizeof(float));
        if (!l->lin[p]) { linear_free(l); return ERR_MEMORY; }
        double black = r->black[p], range = r->white - black;
        if (range <= 0) range = 1;
        for (uint32_t c = 0; c < l->codes; c++) {
            double v = r->lin_n ? r->lin[c < r->lin_n ? c : r->lin_n - 1] : c;
            v = (v - black) / range;
            l->lin[p][c] = (float)(v < 0 ? 0 : v);
        }
    }
    return ERR_OK;
}
/* DNG BlackLevelDeltaH/V, normalised for black level index p. */
static inline float black_delta(const Raw *r, uint32_t y, uint32_t x, int p) {
    double d = 0, range = r->white - r->black[p];
    if (r->black_h && x >= r->aa_left && x < r->aa_right) d += r->black_h[x - r->aa_left];
    if (r->black_v && y >= r->aa_top && y < r->aa_bottom) d += r->black_v[y - r->aa_top];
    return (float)(d / (range > 0 ? range : 1));
}
static inline float lin_at(const Linear *l, uint32_t y, uint32_t x) {
    const Raw *r = l->raw;
    int p = (y & 1) * 2 + (x & 1);
    float v = l->lin[p][r->data[(uint64_t)y * r->width + x]];
    if (r->black_h || r->black_v) { v -= black_delta(r, y, x, p); if (v < 0) v = 0; }
    return v;
}
/* Linear DNG: value of channel c (after the optional swap) at (y, x). */
static inline float lin_rgb(const Linear *l, uint32_t y, uint32_t x, int c) {
    const Raw *r = l->raw;
    int s = l->swap ? 2 - c : c;
    float v = l->lin[s][r->data[((uint64_t)y * r->width + x) * 3 + (uint32_t)s]];
    if (r->black_h || r->black_v) { v -= black_delta(r, y, x, s); if (v < 0) v = 0; }
    return v;
}

/* Tone: sRGB encoding followed by a gentle S-curve for a print-like look.
 * With gamma563 (Adobe RGB output) the same toned linear value is encoded
 * with Adobe RGB's gamma instead, so the look is identical in every space. */
static double srgb_encode(double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * m_pow(v, 1 / 2.4) - 0.055; }
static double srgb_decode(double g) { return g <= 0.04045 ? g / 12.92 : m_pow((g + 0.055) / 1.055, 2.4); }
static void build_lut(Render *rp, int gamma563) {
    for (int i = 0; i <= 4096; i++) {
        double g = srgb_encode(i / 4096.0);
        g = g + 0.25 * (g * g * (3 - 2 * g) - g);
        if (gamma563) g = g > 0 ? m_pow(srgb_decode(g), 256.0 / 563.0) : 0;
        int v = (int)(g * 255 + 0.5);
        rp->lut[i] = (uint8_t)CLAMP(v, 0, 255);
        rp->enc[i] = (float)g;
    }
    for (int i = 0; i <= 1024; i++) {
        double t = i / 1024.0, ss = t * t * (3 - 2 * t);
        rp->gain[i] = (float)(1 + (HDR_PEAK - 1) * ss);
        rp->boost[i] = (float)m_pow(rp->gain[i], 1 / 2.4);
        int code = (int)(255 * m_log(rp->gain[i]) / m_log(HDR_PEAK) + 0.5);
        rp->gm[i] = (uint8_t)CLAMP(code, 0, 255);
    }
}
static inline uint8_t tone(const Render *rp, float v) {
    if (!(v > 0)) return rp->lut[0];
    if (v >= 1) return rp->lut[4096];
    return rp->lut[(int)(v * 4096 + 0.5f)];
}
/* White-balanced camera RGB -> 8-bit sRGB. Colours brighter than the output
 * range keep their hue and fade towards white (no hard per-channel clip,
 * which would turn bright areas cyan or magenta at extreme white balance). */
static inline void finish(const Render *rp, float w0, float w1, float w2, uint8_t *o) {
    const float *m = rp->m;
    float r = m[0] * w0 + m[1] * w1 + m[2] * w2;
    float g = m[3] * w0 + m[4] * w1 + m[5] * w2;
    float b = m[6] * w0 + m[7] * w1 + m[8] * w2;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    float l = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (l > 1) {
        float inv = 1 / l, s = 1 - inv;
        r = r * inv + (1 - r * inv) * s; g = g * inv + (1 - g * inv) * s; b = b * inv + (1 - b * inv) * s;
    }
    o[0] = tone(rp, r); o[1] = tone(rp, g); o[2] = tone(rp, b);
}

/* IEEE half float with round-to-nearest-even (values here are >= 0). */
static inline uint16_t to_half(float f) {
    union { float f; uint32_t u; } v = {f};
    uint32_t u = v.u, sign = (u >> 16) & 0x8000;
    u &= 0x7FFFFFFF;
    if (u >= 0x47800000) return (uint16_t)(sign | 0x7C00);            /* overflow -> inf */
    if (u < 0x38800000) {                                              /* subnormal / zero */
        if (u < 0x33000000) return (uint16_t)sign;
        uint32_t e = u >> 23, m = (u & 0x7FFFFF) | 0x800000, shift = 126 - e;
        uint32_t h = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return (uint16_t)(sign | h);
    }
    uint32_t h = ((u - 0x38000000) >> 13), rem = u & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;
    return (uint16_t)(sign | h);
}
static inline float enc_lookup(const Render *rp, float v) {
    if (!(v > 0)) return rp->enc[0];
    if (v >= 1) return rp->enc[4096];
    return rp->enc[(int)(v * 4096 + 0.5f)];
}
/* Index into gain[]/boost[]/gm[] for a Display P3 colour (channels >= 0). */
static inline int hdr_index(const float *c) {
    float l = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    float t = (l - HDR_KNEE) / (1 - HDR_KNEE);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return (int)(t * 1024 + 0.5f);
}
static inline void to_p3(const Render *rp, float w0, float w1, float w2, float *c) {
    const float *m = rp->m_p3;
    c[0] = m[0] * w0 + m[1] * w1 + m[2] * w2; c[1] = m[3] * w0 + m[4] * w1 + m[5] * w2; c[2] = m[6] * w0 + m[7] * w1 + m[8] * w2;
    for (int i = 0; i < 3; i++) if (!(c[i] > 0)) c[i] = 0;
}
/* HDR variant of finish(): Display P3, encoded with the (extended) sRGB
 * transfer function as the browser's extended-range canvas expects. */
static inline void finish_hdr(const Render *rp, float w0, float w1, float w2, uint16_t *o) {
    float c[3];
    to_p3(rp, w0, w1, w2, c);
    float l = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    int ti = hdr_index(c);
    if (l > 1) {
        float inv = 1 / l, s = 1 - inv;
        for (int i = 0; i < 3; i++) c[i] = c[i] * inv + (1 - c[i] * inv) * s;
    }
    float bp = rp->boost[ti], gain = rp->gain[ti];
    for (int i = 0; i < 3; i++) {
        float g = enc_lookup(rp, c[i]), out = g;
        if (ti > 0) {
            /* enc(dec(g) * gain): exact in the power segment, linear below. */
            if (g <= 0.04045f && g * gain <= 0.04045f) out = g * gain;
            else out = (g + 0.055f) * bp - 0.055f;
        }
        o[i] = to_half(out);
    }
    o[3] = 0x3C00;   /* 1.0 */
}

/* ---- preview base: camera RGB (not white balanced) at reduced size ---- */
typedef struct { float *px; uint32_t w, h; } Base;
static void base_free(Base *b) { xfree(b->px); b->px = NULL; b->w = b->h = 0; }
/* Builds a base of (tw, th) from the half-size superpixels [i0, i0 + hw) x
 * [j0, j0 + hh) of the crop (superpixel i covers crop columns 2i, 2i + 1). */
static int base_build_rect(Base *b, const Linear *l, uint32_t i0, uint32_t j0, uint32_t hw, uint32_t hh,
                           uint32_t tw, uint32_t th) {
    const Raw *r = l->raw;
    b->w = tw; b->h = th;
    b->px = xcalloc((size_t)tw * th * 3 * sizeof(float));
    uint16_t *cnt = xcalloc((size_t)tw * sizeof(uint16_t));
    if (!b->px || !cnt) { xfree(cnt); base_free(b); return ERR_MEMORY; }
    float *row = xcalloc((size_t)tw * 3 * sizeof(float));
    if (!row) { xfree(cnt); base_free(b); return ERR_MEMORY; }
    uint32_t ty_cur = 0;
    for (uint32_t j = 0; j <= hh; j++) {
        uint32_t ty = j < hh ? (uint32_t)((uint64_t)j * th / hh) : th;
        if (ty != ty_cur) {   /* flush the finished output row */
            float *o = b->px + (size_t)ty_cur * tw * 3;
            for (uint32_t x = 0; x < tw; x++) {
                float k = cnt[x] ? 1.0f / cnt[x] : 0;
                for (int c = 0; c < 3; c++) o[x * 3 + c] = row[x * 3 + c] * k;
            }
            xmemset(row, 0, (size_t)tw * 3 * sizeof(float));
            xmemset(cnt, 0, (size_t)tw * sizeof(uint16_t));
            ty_cur = ty;
        }
        if (j == hh) break;
        uint32_t y = r->crop_y + 2 * (j0 + j);
        for (uint32_t i = 0; i < hw; i++) {
            uint32_t x = r->crop_x + 2 * (i0 + i), tx = (uint32_t)((uint64_t)i * tw / hw);
            float v[3] = {0, 0, 0};
            if (r->spp == 3) {   /* linear DNG: mean of the 2x2 pixels */
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++)
                    for (int c = 0; c < 3; c++) v[c] += lin_rgb(l, y + dy, x + dx, c) * 0.25f;
                row[tx * 3] += v[0]; row[tx * 3 + 1] += v[1]; row[tx * 3 + 2] += v[2];
            } else {
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++)
                    v[color_at(r, l->swap, y + dy, x + dx)] += lin_at(l, y + dy, x + dx);
                row[tx * 3] += v[0]; row[tx * 3 + 1] += v[1] * 0.5f; row[tx * 3 + 2] += v[2];
            }
            cnt[tx]++;
        }
    }
    xfree(row); xfree(cnt);
    return ERR_OK;
}
/* Builds a base whose size fits (maxw, maxh), at most half the crop size. */
static int base_build(Base *b, const Linear *l, uint32_t maxw, uint32_t maxh) {
    const Raw *r = l->raw;
    uint32_t hw = r->crop_w / 2, hh = r->crop_h / 2;
    if (!hw || !hh) return ERR_FORMAT;
    double s = 1;
    if (maxw < hw) s = (double)maxw / hw;
    if (maxh < hh * s) s = (double)maxh / hh;
    uint32_t tw = (uint32_t)(hw * s + 0.5), th = (uint32_t)(hh * s + 0.5);
    if (!tw) tw = 1;
    if (!th) th = 1;
    return base_build_rect(b, l, 0, 0, hw, hh, tw, th);
}

/* Renders the base, white balanced by wb. Output RGBA (rgba=1) or RGB, with
 * the EXIF orientation applied when `orient` is set. Output size: base size,
 * rotated for 5..8. */
enum { OUT_RGB8 = 0, OUT_RGBA8 = 1, OUT_RGBA_HALF = 2 };
static void base_render(const Base *b, const Render *rp, const float *wb, void *out, int format, int orient) {
    int o = orient ? orient : 1;
    int swapxy = o >= 5 && o <= 8;
    uint32_t ow = swapxy ? b->h : b->w;
    int ch = format == OUT_RGB8 ? 3 : 4;
    for (uint32_t y = 0; y < b->h; y++) {
        const float *s = b->px + (size_t)y * b->w * 3;
        for (uint32_t x = 0; x < b->w; x++, s += 3) {
            /* Map source (x, y) to the oriented output position. */
            uint32_t X = x, Y = y, W = b->w - 1, H = b->h - 1;
            switch (o) {
            case 2: X = W - x; break;
            case 3: X = W - x; Y = H - y; break;
            case 4: Y = H - y; break;
            case 5: X = y; Y = x; break;
            case 6: X = H - y; Y = x; break;
            case 7: X = H - y; Y = W - x; break;
            case 8: X = y; Y = W - x; break;
            }
            size_t at = ((size_t)Y * ow + X) * ch;
            if (format == OUT_RGBA_HALF) {
                finish_hdr(rp, s[0] * wb[0], s[1] * wb[1], s[2] * wb[2], (uint16_t *)out + at);
            } else {
                uint8_t *d = (uint8_t *)out + at;
                finish(rp, s[0] * wb[0], s[1] * wb[1], s[2] * wb[2], d);
                if (ch == 4) d[3] = 255;
            }
        }
    }
}

/* ---- full resolution, in strips (Malvar-He-Cutler demosaicing) ---- */
static inline uint32_t reflect(int64_t i, uint32_t n) {
    /* Mirror by two to keep the CFA parity. */
    while (i < 0 || i >= (int64_t)n) {
        if (i < 0) i = -i;
        if (i >= (int64_t)n) i = 2 * ((int64_t)n - 1) - i;
        if (n < 3) return 0;
    }
    return (uint32_t)i;
}
#define STRIP 16
/* Demosaics crop rows [y0, y0 + n) (n <= STRIP), columns [x0, x0 + w), of the
 * mosaic white balanced by wb, into rgb (n * w * 3, clipped at 0). win holds
 * (w + 4) * (STRIP + 4) floats. Pixels outside the crop are mirrored. */
static void demosaic_rows(const Linear *l, const float *wb, uint32_t x0, uint32_t w, uint32_t y0, uint32_t n,
                          float *win, float *rgb) {
    const Raw *r = l->raw;
    uint32_t cw = r->crop_w, chh = r->crop_h;
    int sw = l->swap;
    if (r->spp == 3) {   /* linear DNG: already demosaiced */
        for (uint32_t k = 0; k < n; k++) {
            uint32_t y = y0 + k + r->crop_y;
            float *o = rgb + (size_t)k * w * 3;
            for (uint32_t i = 0; i < w; i++, o += 3)
                for (int c = 0; c < 3; c++) o[c] = lin_rgb(l, y, x0 + i + r->crop_x, c) * wb[c];
        }
        return;
    }
    /* Fill the white-balanced mosaic window (n + 4 rows, w + 4 columns). */
    uint32_t ww = w + 4;
    for (uint32_t k = 0; k < n + 4; k++) {
        uint32_t y = reflect((int64_t)y0 + k - 2, chh) + r->crop_y;
        float *wr = win + (size_t)k * ww;
        for (uint32_t i = 0; i < ww; i++) {
            uint32_t x = reflect((int64_t)x0 + i - 2, cw) + r->crop_x;
            wr[i] = lin_at(l, y, x) * wb[color_at(r, sw, y, x)];
        }
    }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t y = y0 + k + r->crop_y;
        const float *c0 = win + (size_t)(k + 2) * ww + 2;
        float *o = rgb + (size_t)k * w * 3;
        for (uint32_t i = 0; i < w; i++, c0++, o += 3) {
            uint32_t x = x0 + i + r->crop_x;
            int c = color_at(r, sw, y, x);
#define P(dy, dx) c0[(dy) * (int)ww + (dx)]
            float p0 = P(0, 0), *v = o;
            float axial2 = P(-2, 0) + P(2, 0) + P(0, -2) + P(0, 2);
            float diag = P(-1, -1) + P(-1, 1) + P(1, -1) + P(1, 1);
            if (c == 1) {
                float h = (5 * p0 + 4 * (P(0, -1) + P(0, 1)) - (P(0, -2) + P(0, 2)) - diag + 0.5f * (P(-2, 0) + P(2, 0))) * 0.125f;
                float vv = (5 * p0 + 4 * (P(-1, 0) + P(1, 0)) - (P(-2, 0) + P(2, 0)) - diag + 0.5f * (P(0, -2) + P(0, 2))) * 0.125f;
                int hc = color_at(r, sw, y, x + 1);   /* colour of the horizontal neighbours */
                v[1] = p0; v[hc] = h; v[2 - hc] = vv;
            } else {
                float g = (4 * p0 + 2 * (P(-1, 0) + P(1, 0) + P(0, -1) + P(0, 1)) - axial2) * 0.125f;
                float d = (6 * p0 + 2 * diag - 1.5f * axial2) * 0.125f;
                v[c] = p0; v[1] = g; v[2 - c] = d;
            }
#undef P
            for (int j = 0; j < 3; j++) if (v[j] < 0) v[j] = 0;
        }
    }
}

typedef struct {
    const Linear *l; const Render *rp;
    uint32_t w, h;          /* crop size */
    float *win, *rgb;
} Full;
static int full_init(Full *f, const Linear *l, const Render *rp) {
    f->l = l; f->rp = rp; f->w = l->raw->crop_w; f->h = l->raw->crop_h;
    f->win = xalloc((size_t)(f->w + 4) * (STRIP + 4) * sizeof(float));
    f->rgb = xalloc((size_t)f->w * STRIP * 3 * sizeof(float));
    return f->win && f->rgb ? ERR_OK : ERR_MEMORY;
}
static void full_free(Full *f) { xfree(f->win); xfree(f->rgb); f->win = f->rgb = NULL; }
/* Renders crop rows [y0, y0 + n) (n <= 16) as RGB into out (n * w * 3) and,
 * when gm is given, the HDR gain map code of every pixel into gm (n * w). */
static void full_rows(Full *f, uint32_t y0, uint32_t n, uint8_t *out, uint8_t *gm) {
    const Render *rp = f->rp;
    demosaic_rows(f->l, rp->wb, 0, f->w, y0, n, f->win, f->rgb);
    const float *v = f->rgb;
    uint8_t *o = out;
    for (size_t i = 0; i < (size_t)n * f->w; i++, v += 3, o += 3) {
        finish(rp, v[0], v[1], v[2], o);
        if (gm) {
            float c[3];
            to_p3(rp, v[0], v[1], v[2], c);
            gm[i] = rp->gm[hdr_index(c)];
        }
    }
}

/* Builds a base of (tw, th) <= (w, h) from the crop rectangle (x0, y0, w, h)
 * at full resolution: demosaiced like the JPEG, white balanced by wb (so it is
 * rendered with unit multipliers), area-averaged down to the target size. */
static int region_build(Base *b, const Linear *l, const float *wb, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                        uint32_t tw, uint32_t th) {
    b->w = tw; b->h = th;
    b->px = xcalloc((size_t)tw * th * 3 * sizeof(float));
    float *win = xalloc((size_t)(w + 4) * (STRIP + 4) * sizeof(float)), *rgb = xalloc((size_t)w * STRIP * 3 * sizeof(float));
    uint32_t *cx = xcalloc((size_t)tw * sizeof(uint32_t)), *cy = xcalloc((size_t)th * sizeof(uint32_t));
    int r = b->px && win && rgb && cx && cy ? ERR_OK : ERR_MEMORY;
    if (!r) {
        for (uint32_t i = 0; i < w; i++) cx[(uint64_t)i * tw / w]++;
        for (uint32_t y = 0; y < h; y += STRIP) {
            uint32_t n = MIN(STRIP, h - y);
            demosaic_rows(l, wb, x0, w, y0 + y, n, win, rgb);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t ty = (uint32_t)((uint64_t)(y + k) * th / h);
                float *o = b->px + (size_t)ty * tw * 3;
                const float *v = rgb + (size_t)k * w * 3;
                cy[ty]++;
                for (uint32_t i = 0; i < w; i++, v += 3) {
                    float *d = o + (size_t)((uint64_t)i * tw / w) * 3;
                    d[0] += v[0]; d[1] += v[1]; d[2] += v[2];
                }
            }
        }
        for (uint32_t y = 0; y < th; y++)
            for (uint32_t x = 0; x < tw; x++) {
                float k = 1.0f / ((float)cx[x] * (float)cy[y]);
                float *d = b->px + ((size_t)y * tw + x) * 3;
                d[0] *= k; d[1] *= k; d[2] *= k;
            }
    }
    xfree(win); xfree(rgb); xfree(cx); xfree(cy);
    if (r) base_free(b);
    return r;
}

/* Area-averaging resize of an RGB image to (tw, th), centred in a black
 * canvas of (cw, ch) when the aspect ratios differ (letterboxing). */
static void rgb_fit(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst, uint32_t cw, uint32_t ch) {
    double s = MIN((double)cw / sw, (double)ch / sh);
    uint32_t tw = (uint32_t)(sw * s + 0.5), th = (uint32_t)(sh * s + 0.5);
    if (tw > cw) tw = cw;
    if (th > ch) th = ch;
    if (!tw) tw = 1;
    if (!th) th = 1;
    uint32_t ox = (cw - tw) / 2, oy = (ch - th) / 2;
    xmemset(dst, 0, (size_t)cw * ch * 3);
    for (uint32_t y = 0; y < th; y++) {
        uint32_t y0 = (uint32_t)((uint64_t)y * sh / th), y1 = (uint32_t)((uint64_t)(y + 1) * sh / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < tw; x++) {
            uint32_t x0 = (uint32_t)((uint64_t)x * sw / tw), x1 = (uint32_t)((uint64_t)(x + 1) * sw / tw);
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t acc[3] = {0, 0, 0}, n = 0;
            for (uint32_t yy = y0; yy < y1 && yy < sh; yy++)
                for (uint32_t xx = x0; xx < x1 && xx < sw; xx++) {
                    const uint8_t *p = src + ((size_t)yy * sw + xx) * 3;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; n++;
                }
            uint8_t *d = dst + ((size_t)(y + oy) * cw + x + ox) * 3;
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)((acc[c] + n / 2) / (n ? n : 1));
        }
    }
}
