/* Baseline JPEG encoder (4:2:0 colour or greyscale, IJG quality scaling)
 * with optimised Huffman tables. Statistics come from a first pass (all rows for small images,
 * every fourth MCU row when streaming); every valid symbol keeps a code. */
#include "base.h"

static const uint8_t zigzag[64] = {0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
static const uint8_t std_lum[64] = {16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99};
static const uint8_t std_chr[64] = {17,18,24,47,99,99,99,99,18,21,26,66,99,99,99,99,24,26,56,99,99,99,99,99,47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99};

typedef struct {
    uint32_t w, h;
    uint8_t q[2][64];            /* natural order */
    float qinv[2][64];
    uint32_t freq[4][257];       /* DC lum, AC lum, DC chr, AC chr */
    uint8_t counts[4][16], syms[4][256]; int nsym[4];
    uint16_t code[4][256]; uint8_t size[4][256];
    int pred[3];
    BitW bw;
    float *ybuf, *cbuf;          /* one MCU row: 16 x W8 luma, 2 x 8 x W16/2 chroma */
    uint32_t w16;
    int gray;                    /* one component, 8x8 MCUs (gain maps) */
} Jpeg;

static float dct_c[8][8];
static int dct_ready;
static void dct_init(void) {
    if (dct_ready) return;
    static const double cosk[9] = {1.0, 0.98078528040323043, 0.92387953251128674, 0.83146961230254524, 0.70710678118654752,
        0.55557023301960218, 0.38268343236508977, 0.19509032201612826, 0.0};
    for (int u = 0; u < 8; u++) for (int x = 0; x < 8; x++) {
        int k = ((2 * x + 1) * u) % 32;      /* cos(k*pi/16) */
        double v = k <= 8 ? cosk[k] : k <= 16 ? -cosk[16 - k] : k <= 24 ? -cosk[k - 16] : cosk[32 - k];
        dct_c[u][x] = (float)(v * (u ? 0.5 : 0.35355339059327376));
    }
    dct_ready = 1;
}
/* Forward DCT + quantisation of one 8x8 block (values already level-shifted). */
static void fdct_quant(const float *blk, const float *qinv, int16_t *out) {
    float tmp[64];
    for (int y = 0; y < 8; y++) for (int u = 0; u < 8; u++) {
        float s = 0; for (int x = 0; x < 8; x++) s += dct_c[u][x] * blk[y * 8 + x];
        tmp[y * 8 + u] = s;
    }
    for (int u = 0; u < 8; u++) for (int v = 0; v < 8; v++) {
        float s = 0; for (int y = 0; y < 8; y++) s += dct_c[v][y] * tmp[y * 8 + u];
        float q = s * qinv[v * 8 + u];
        out[v * 8 + u] = (int16_t)(q < 0 ? q - 0.5f : q + 0.5f);
    }
}
static inline int jcat(int v) { unsigned a = (unsigned)(v < 0 ? -v : v); int n = 0; while (a) { n++; a >>= 1; } return n; }

static void block_stats(Jpeg *j, const int16_t *c, int comp) {
    int t = comp ? 2 : 0;
    int diff = c[0] - j->pred[comp]; j->pred[comp] = c[0];
    j->freq[t][jcat(diff)]++;
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = c[zigzag[k]];
        if (!v) { run++; continue; }
        while (run > 15) { j->freq[t + 1][0xF0]++; run -= 16; }
        j->freq[t + 1][run << 4 | jcat(v)]++; run = 0;
    }
    if (run) j->freq[t + 1][0]++;
}
static void block_write(Jpeg *j, const int16_t *c, int comp) {
    int t = comp ? 2 : 0;
    int diff = c[0] - j->pred[comp]; j->pred[comp] = c[0];
    int s = jcat(diff);
    bitw_put(&j->bw, j->code[t][s], j->size[t][s]);
    if (s) bitw_put(&j->bw, (uint32_t)(diff < 0 ? diff - 1 : diff), s);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = c[zigzag[k]];
        if (!v) { run++; continue; }
        while (run > 15) { bitw_put(&j->bw, j->code[t + 1][0xF0], j->size[t + 1][0xF0]); run -= 16; }
        int sz = jcat(v), sym = run << 4 | sz;
        bitw_put(&j->bw, j->code[t + 1][sym], j->size[t + 1][sym]);
        bitw_put(&j->bw, (uint32_t)(v < 0 ? v - 1 : v), sz);
        run = 0;
    }
    if (run) bitw_put(&j->bw, j->code[t + 1][0], j->size[t + 1][0]);
}

static int jpeg_init(Jpeg *j, uint32_t w, uint32_t h, int quality, int gray) {
    xmemset(j, 0, sizeof(*j));
    if (!w || !h || w > 65535 || h > 65535) return ERR_FORMAT;
    dct_init();
    j->w = w; j->h = h; j->gray = gray;
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    for (int i = 0; i < 64; i++) {
        int a = (std_lum[i] * scale + 50) / 100, b = (std_chr[i] * scale + 50) / 100;
        j->q[0][i] = (uint8_t)CLAMP(a, 1, 255); j->q[1][i] = (uint8_t)CLAMP(b, 1, 255);
        j->qinv[0][i] = 1.0f / j->q[0][i]; j->qinv[1][i] = 1.0f / j->q[1][i];
    }
    if (gray) {
        j->w16 = (w + 7) & ~7u;
        j->ybuf = xalloc((size_t)j->w16 * 8 * sizeof(float));
        return j->ybuf ? ERR_OK : ERR_MEMORY;
    }
    j->w16 = (w + 15) & ~15u;
    j->ybuf = xalloc((size_t)j->w16 * 16 * sizeof(float));
    j->cbuf = xalloc((size_t)j->w16 / 2 * 8 * 2 * sizeof(float));
    return j->ybuf && j->cbuf ? ERR_OK : ERR_MEMORY;
}
static void jpeg_free(Jpeg *j) { xfree(j->ybuf); xfree(j->cbuf); j->ybuf = j->cbuf = NULL; }

/* Converts up to 16 RGB rows into the MCU buffers (edge pixels replicated). */
static void mcu_load(Jpeg *j, const uint8_t *rgb, uint32_t rows) {
    uint32_t w = j->w, w16 = j->w16, cw = w16 / 2;
    for (uint32_t y = 0; y < 16; y++) {
        const uint8_t *src = rgb + (size_t)MIN(y, rows - 1) * w * 3;
        float *yr = j->ybuf + (size_t)y * w16;
        for (uint32_t x = 0; x < w16; x++) {
            const uint8_t *p = src + (size_t)MIN(x, w - 1) * 3;
            yr[x] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] - 128.0f;
        }
    }
    for (uint32_t y = 0; y < 8; y++) {
        const uint8_t *s0 = rgb + (size_t)MIN(2 * y, rows - 1) * w * 3, *s1 = rgb + (size_t)MIN(2 * y + 1, rows - 1) * w * 3;
        float *cb = j->cbuf + (size_t)y * cw, *cr = j->cbuf + (size_t)(8 + y) * cw;
        for (uint32_t x = 0; x < cw; x++) {
            uint32_t xa = MIN(2 * x, w - 1), xb = MIN(2 * x + 1, w - 1);
            float r = (s0[xa * 3] + s0[xb * 3] + s1[xa * 3] + s1[xb * 3]) * 0.25f;
            float g = (s0[xa * 3 + 1] + s0[xb * 3 + 1] + s1[xa * 3 + 1] + s1[xb * 3 + 1]) * 0.25f;
            float b = (s0[xa * 3 + 2] + s0[xb * 3 + 2] + s1[xa * 3 + 2] + s1[xb * 3 + 2]) * 0.25f;
            cb[x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
            cr[x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
        }
    }
}
/* Processes one MCU row: statistics (write = 0) or entropy coding. */
static void mcu_row(Jpeg *j, const uint8_t *rgb, uint32_t rows, int write) {
    mcu_load(j, rgb, rows);
    uint32_t w16 = j->w16, cw = w16 / 2;
    float blk[64]; int16_t coef[64];
    for (uint32_t mx = 0; mx < w16 / 16; mx++) {
        for (int b = 0; b < 4; b++) {
            uint32_t bx = mx * 16 + (b & 1) * 8, by = (b >> 1) * 8;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) blk[y * 8 + x] = j->ybuf[(by + y) * w16 + bx + x];
            fdct_quant(blk, j->qinv[0], coef);
            if (write) block_write(j, coef, 0); else block_stats(j, coef, 0);
        }
        for (int c = 0; c < 2; c++) {
            const float *src = j->cbuf + (size_t)c * 8 * cw;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) blk[y * 8 + x] = src[y * cw + mx * 8 + x];
            fdct_quant(blk, j->qinv[1], coef);
            if (write) block_write(j, coef, 1 + c); else block_stats(j, coef, 1 + c);
        }
    }
}
/* Greyscale: one row of 8x8 blocks from up to 8 rows of 8-bit samples. */
static void gray_row(Jpeg *j, const uint8_t *g, uint32_t rows, int write) {
    uint32_t w = j->w, w8 = j->w16;
    float blk[64]; int16_t coef[64];
    for (uint32_t bx = 0; bx < w8; bx += 8) {
        for (int y = 0; y < 8; y++) {
            const uint8_t *src = g + (size_t)MIN((uint32_t)y, rows - 1) * w;
            for (int x = 0; x < 8; x++) blk[y * 8 + x] = src[MIN(bx + x, w - 1)] - 128.0f;
        }
        fdct_quant(blk, j->qinv[0], coef);
        if (write) block_write(j, coef, 0); else block_stats(j, coef, 0);
    }
}
static void jpeg_tables(Jpeg *j) {
    for (int t = 0; t < (j->gray ? 2 : 4); t++) {
        if (t & 1) {   /* AC: EOB, ZRL and every run/size pair */
            j->freq[t][0]++; j->freq[t][0xF0]++;
            for (int r = 0; r < 16; r++) for (int s = 1; s <= 10; s++) j->freq[t][r << 4 | s]++;
        } else for (int s = 0; s <= 11; s++) j->freq[t][s]++;
        huff_optimal(j->freq[t], j->counts[t], j->syms[t], &j->nsym[t]);
        huff_codes(j->counts[t], j->syms[t], j->code[t], j->size[t]);
    }
    j->pred[0] = j->pred[1] = j->pred[2] = 0;
}
static void put16be(Buf *b, uint32_t v) { buf_byte(b, (uint8_t)(v >> 8)); buf_byte(b, (uint8_t)v); }
/* Writes an APPn segment: marker, length, identifier (with its NULs), data. */
static int jpeg_app(Buf *out, int n, const char *id, uint32_t id_len, const void *data, uint32_t len) {
    if (id_len + len + 2 > 65535) return 0;
    buf_byte(out, 0xFF); buf_byte(out, (uint8_t)(0xE0 + n)); put16be(out, id_len + len + 2);
    buf_put(out, id, id_len); buf_put(out, data, len);
    return 1;
}
static void jpeg_start(Buf *out, int jfif) {
    buf_byte(out, 0xFF); buf_byte(out, 0xD8);
    if (jfif) {   /* previews */
        static const uint8_t v[] = {1, 1, 0, 0, 1, 0, 1, 0, 0};
        jpeg_app(out, 0, "JFIF", 5, v, sizeof v);
    }
}
/* Quantisation and Huffman tables, frame and scan header; starts the scan. */
static void jpeg_frame(Jpeg *j, Buf *out) {
    int nq = j->gray ? 1 : 2, nt = j->gray ? 2 : 4;
    for (int t = 0; t < nq; t++) {
        buf_byte(out, 0xFF); buf_byte(out, 0xDB); put16be(out, 67); buf_byte(out, (uint8_t)t);
        for (int k = 0; k < 64; k++) buf_byte(out, j->q[t][zigzag[k]]);
    }
    static const uint8_t comps[9] = {1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1}, gray_comp[3] = {1, 0x11, 0};
    int nc = j->gray ? 1 : 3;
    buf_byte(out, 0xFF); buf_byte(out, 0xC0); put16be(out, 8 + 3 * nc); buf_byte(out, 8);
    put16be(out, j->h); put16be(out, j->w); buf_byte(out, (uint8_t)nc); buf_put(out, j->gray ? gray_comp : comps, 3 * nc);
    static const uint8_t ids[4] = {0x00, 0x10, 0x01, 0x11};
    for (int t = 0; t < nt; t++) {
        buf_byte(out, 0xFF); buf_byte(out, 0xC4); put16be(out, 19 + j->nsym[t]); buf_byte(out, ids[t]);
        buf_put(out, j->counts[t], 16); buf_put(out, j->syms[t], (uint32_t)j->nsym[t]);
    }
    static const uint8_t sos[12] = {0xFF, 0xDA, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0}, gray_sos[8] = {0xFF, 0xDA, 0, 8, 1, 1, 0x00, 0};
    if (j->gray) buf_put(out, gray_sos, 8); else buf_put(out, sos, 12);
    buf_byte(out, 63); buf_byte(out, 0);
    j->bw.out = out; j->bw.acc = 0; j->bw.bits = 0;
}
static void jpeg_finish(Jpeg *j, Buf *out) { bitw_flush(&j->bw); buf_byte(out, 0xFF); buf_byte(out, 0xD9); }

/* Encodes a complete in-memory RGB image (JFIF, no metadata). */
static int jpeg_encode_rgb(Buf *out, const uint8_t *rgb, uint32_t w, uint32_t h, int quality) {
    Jpeg j;
    int r = jpeg_init(&j, w, h, quality, 0);
    if (r) { jpeg_free(&j); return r; }
    for (uint32_t y = 0; y < h; y += 16) mcu_row(&j, rgb + (size_t)y * w * 3, MIN(16, h - y), 0);
    jpeg_tables(&j);
    jpeg_start(out, 1);
    jpeg_frame(&j, out);
    for (uint32_t y = 0; y < h; y += 16) mcu_row(&j, rgb + (size_t)y * w * 3, MIN(16, h - y), 1);
    jpeg_finish(&j, out);
    jpeg_free(&j);
    return out->failed ? ERR_MEMORY : ERR_OK;
}
/* Encodes a greyscale image; app holds ready-made APPn segments. */
static int jpeg_encode_gray(Buf *out, const uint8_t *g, uint32_t w, uint32_t h, int quality, const Buf *app) {
    Jpeg j;
    int r = jpeg_init(&j, w, h, quality, 1);
    if (r) { jpeg_free(&j); return r; }
    for (uint32_t y = 0; y < h; y += 8) gray_row(&j, g + (size_t)y * w, MIN(8, h - y), 0);
    jpeg_tables(&j);
    jpeg_start(out, 0);
    buf_put(out, app->data, app->len);
    jpeg_frame(&j, out);
    for (uint32_t y = 0; y < h; y += 8) gray_row(&j, g + (size_t)y * w, MIN(8, h - y), 1);
    jpeg_finish(&j, out);
    jpeg_free(&j);
    return out->failed ? ERR_MEMORY : ERR_OK;
}
