/* Lossless JPEG (ITU T.81 process 14, "LJ92") as used by DNG compression 7,
 * plus the Huffman/bit-reader primitives shared with the Nikon decoder. */
#include "base.h"

/* ---- MSB-first bit reader; `stuffed` skips 0x00 after 0xFF (JPEG). ---- */
typedef struct {
    const uint8_t *p, *end;
    uint64_t acc; int bits;
    int stuffed, marker;      /* marker: a JPEG marker was reached */
} Bits;
static void bits_init(Bits *b, const uint8_t *p, const uint8_t *end, int stuffed) {
    b->p = p; b->end = end; b->acc = 0; b->bits = 0; b->stuffed = stuffed; b->marker = 0;
}
static void bits_fill(Bits *b) {
    while (b->bits <= 56) {
        uint32_t c = 0;
        if (b->p < b->end && !b->marker) {
            c = *b->p++;
            if (b->stuffed && c == 0xFF) {
                if (b->p < b->end && *b->p == 0x00) b->p++;
                else { b->marker = 1; b->p--; c = 0; }
            }
        }
        b->acc |= (uint64_t)c << (56 - b->bits);
        b->bits += 8;
    }
}
static inline uint32_t bits_peek(Bits *b, int n) { if (b->bits < n) bits_fill(b); return n ? (uint32_t)(b->acc >> (64 - n)) : 0; }
static inline void bits_skip(Bits *b, int n) { b->acc <<= n; b->bits -= n; }
static inline uint32_t bits_get(Bits *b, int n) { uint32_t v = bits_peek(b, n); bits_skip(b, n); return v; }

/* Huffman decoder with a 16-bit direct lookup: entry = length << 8 | symbol. */
typedef struct { uint16_t *lut; } Huff;
static int huff_build(Huff *h, const uint8_t counts[16], const uint8_t *symbols, int nsym) {
    h->lut = xcalloc(65536 * sizeof(uint16_t));
    if (!h->lut) return ERR_MEMORY;
    uint32_t code = 0; int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < counts[len - 1]; i++) {
            if (k >= nsym) return ERR_CORRUPT;
            uint32_t first = code << (16 - len), n = 1u << (16 - len);
            if (first + n > 65536) return ERR_CORRUPT;
            for (uint32_t j = 0; j < n; j++) h->lut[first + j] = (uint16_t)(len << 8 | symbols[k]);
            code++; k++;
        }
        code <<= 1;
    }
    return ERR_OK;
}
static void huff_free(Huff *h) { xfree(h->lut); h->lut = NULL; }
static inline int huff_decode(Huff *h, Bits *b) {
    uint16_t e = h->lut[bits_peek(b, 16)];
    if (!e) return -1;
    bits_skip(b, e >> 8);
    return e & 0xFF;
}
/* JPEG "EXTEND" of a magnitude category. */
static inline int32_t ljpeg_diff(Bits *b, int t) {
    if (t == 0) return 0;
    if (t == 16) return 32768;
    int32_t v = (int32_t)bits_get(b, t);
    if (v < (1 << (t - 1))) v -= (1 << t) - 1;
    return v;
}

/* Decodes one LJ92 stream into `out` (raster order, `count` samples of up
 * to 16 bits). Returns the frame geometry for validation. */
typedef struct { int width, height, comps, precision; } LjInfo;
static int ljpeg_decode(const uint8_t *data, uint32_t len, uint16_t *out, uint32_t count, LjInfo *info) {
    const uint8_t *p = data, *end = data + len;
    Huff tables[4] = {{0}};
    int ret = ERR_CORRUPT, width = 0, height = 0, comps = 0, precision = 0, restart = 0;
    int comp_table[4] = {0};
    if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return ERR_CORRUPT;
    p += 2;
    while (p + 4 <= end) {
        if (p[0] != 0xFF) { p++; continue; }
        int marker = p[1];
        if (marker == 0xFF) { p++; continue; }
        uint32_t seg = (uint32_t)(p[2] << 8 | p[3]);
        const uint8_t *s = p + 4, *se = p + 2 + seg;
        if (seg < 2 || se > end) goto done;
        if (marker == 0xC3) {
            if (seg < 8) goto done;
            precision = s[0]; height = s[1] << 8 | s[2]; width = s[3] << 8 | s[4]; comps = s[5];
            if (comps < 1 || comps > 4 || precision < 2 || precision > 16 || (uint32_t)seg < 8u + 3u * comps) goto done;
        } else if (marker == 0xC4) {
            while (s + 17 <= se) {
                int id = s[0] & 3, n = 0;
                for (int i = 0; i < 16; i++) n += s[1 + i];
                if (s + 17 + n > se) goto done;
                huff_free(&tables[id]);
                if ((ret = huff_build(&tables[id], s + 1, s + 17, n)) != ERR_OK) goto done;
                ret = ERR_CORRUPT;
                s += 17 + n;
            }
        } else if (marker == 0xDD) {
            if (seg >= 4) restart = s[0] << 8 | s[1];
        } else if (marker == 0xDA) {
            int ns = s[0];
            if (ns != comps || !width || !height) goto done;
            for (int i = 0; i < ns; i++) comp_table[i] = (s[2 + 2 * i] >> 4) & 3;
            int pred = s[1 + 2 * ns], pt = s[3 + 2 * ns] & 15;
            for (int i = 0; i < ns; i++) if (!tables[comp_table[i]].lut) goto done;
            uint32_t row_len = (uint32_t)width * comps;
            if ((uint64_t)row_len * height > count) goto done;
            Bits b; bits_init(&b, se, end, 1);
            uint32_t mask = 0xFFFF, todo_restart = restart ? (uint32_t)restart : 0xFFFFFFFFu;
            int first_line = 1;
            for (int y = 0; y < height; y++) {
                uint16_t *row = out + (uint64_t)y * row_len, *up = y ? row - row_len : NULL;
                for (int x = 0; x < width; x++) {
                    if (!todo_restart) {   /* restart: realign to the RSTn marker, reset prediction */
                        const uint8_t *q = b.p;
                        while (q + 1 < end && !(q[0] == 0xFF && q[1] >= 0xD0 && q[1] <= 0xD7)) q++;
                        bits_init(&b, q + 2 <= end ? q + 2 : end, end, 1);
                        todo_restart = (uint32_t)restart; first_line = 1;
                    }
                    todo_restart--;
                    for (int c = 0; c < comps; c++) {
                        int t = huff_decode(&tables[comp_table[c]], &b);
                        if (t < 0 || t > 16) goto done;
                        int32_t diff = ljpeg_diff(&b, t);
                        int32_t pv;
                        uint32_t i = (uint32_t)x * comps + c;
                        if (first_line && x == 0) pv = 1 << (precision - pt - 1);
                        else if (first_line) pv = row[i - comps];
                        else if (x == 0) pv = up[i];
                        else {
                            int32_t ra = row[i - comps], rb = up[i], rc = up[i - comps];
                            switch (pred) {
                            case 1: pv = ra; break;
                            case 2: pv = rb; break;
                            case 3: pv = rc; break;
                            case 4: pv = ra + rb - rc; break;
                            case 5: pv = ra + ((rb - rc) >> 1); break;
                            case 6: pv = rb + ((ra - rc) >> 1); break;
                            case 7: pv = (ra + rb) >> 1; break;
                            default: pv = ra;
                            }
                        }
                        row[i] = (uint16_t)((uint32_t)(pv + diff) & mask);
                    }
                }
                first_line = 0;
            }
            if (pt) for (uint64_t i = 0; i < (uint64_t)row_len * height; i++) out[i] = (uint16_t)(out[i] << pt);
            if (info) { info->width = width; info->height = height; info->comps = comps; info->precision = precision; }
            ret = ERR_OK;
            goto done;
        } else if (marker == 0xD9) goto done;
        p = se;
    }
done:
    for (int i = 0; i < 4; i++) huff_free(&tables[i]);
    return ret;
}

/* ---- encoder ---- */
typedef struct { Buf *out; uint64_t acc; int bits; } BitW;
static void bitw_put(BitW *w, uint32_t code, int len) {
    w->acc = w->acc << len | (code & ((1u << len) - 1));
    w->bits += len;
    while (w->bits >= 8) {
        uint8_t c = (uint8_t)(w->acc >> (w->bits - 8));
        buf_byte(w->out, c);
        if (c == 0xFF) buf_byte(w->out, 0);
        w->bits -= 8;
    }
}
static void bitw_flush(BitW *w) { if (w->bits) bitw_put(w, 0x7F, 8 - w->bits); w->acc = 0; w->bits = 0; }

/* Optimal length-limited Huffman code (ITU T.81 Annex K.2). `freq` has 257
 * slots: symbols 0..255 plus the reserved one. Produces JPEG counts/symbols. */
static void huff_optimal(const uint32_t *freq_in, uint8_t counts[16], uint8_t *symbols, int *nsym) {
    int32_t freq[257]; int codesize[257], others[257], bits[33];
    for (int i = 0; i < 257; i++) { freq[i] = (int32_t)MIN(freq_in[i], 0x3FFFFFFFu); codesize[i] = 0; others[i] = -1; }
    freq[256] = 1;
    for (int i = 0; i < 33; i++) bits[i] = 0;
    for (;;) {
        int c1 = -1, c2 = -1; int32_t v = 0x7FFFFFFF;
        for (int i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v) { v = freq[i]; c1 = i; }
        v = 0x7FFFFFFF;
        for (int i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v && i != c1) { v = freq[i]; c2 = i; }
        if (c2 < 0) break;
        freq[c1] += freq[c2]; freq[c2] = 0;
        codesize[c1]++; while (others[c1] >= 0) { c1 = others[c1]; codesize[c1]++; }
        others[c1] = c2;
        codesize[c2]++; while (others[c2] >= 0) { c2 = others[c2]; codesize[c2]++; }
    }
    for (int i = 0; i <= 256; i++) if (codesize[i]) bits[MIN(codesize[i], 32)]++;
    for (int i = 32; i > 16; i--)
        while (bits[i] > 0) {
            int j = i - 2;
            while (bits[j] == 0) j--;
            bits[i] -= 2; bits[i - 1]++; bits[j + 1] += 2; bits[j]--;
        }
    int i = 16; while (bits[i] == 0) i--;
    bits[i]--;                                   /* drop the reserved symbol */
    for (int k = 0; k < 16; k++) counts[k] = (uint8_t)bits[k + 1];
    int n = 0;
    for (int len = 1; len <= 32; len++)
        for (int s = 0; s < 256; s++) if (codesize[s] == len) symbols[n++] = (uint8_t)s;
    *nsym = n;
}
static void huff_codes(const uint8_t counts[16], const uint8_t *symbols, uint16_t *code, uint8_t *size) {
    uint32_t c = 0; int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < counts[len - 1]; i++) { code[symbols[k]] = (uint16_t)c; size[symbols[k]] = (uint8_t)len; c++; k++; }
        c <<= 1;
    }
}
static inline int ssss(int32_t d) { uint32_t a = (uint32_t)(d < 0 ? -d : d); int n = 0; while (a) { n++; a >>= 1; } return n; }

/* Prediction residuals for predictor 1 with interleaved components, as
 * modular 16-bit differences in [-32768, 32767]. */
static inline int32_t lj_residual(const uint16_t *s, uint32_t row_len, uint32_t x, uint32_t y, int comps, int c, int precision) {
    uint32_t i = y * row_len + x * comps + c;
    int32_t pv = y == 0 ? (x == 0 ? 1 << (precision - 1) : s[i - comps]) : (x == 0 ? s[i - row_len] : s[i - comps]);
    int32_t d = (int32_t)(int16_t)(uint16_t)((uint32_t)s[i] - (uint32_t)pv);
    return d;
}
void lj_count(const uint16_t *s, int width, int height, int comps, int precision, uint32_t *freq) {
    uint32_t row_len = (uint32_t)width * comps;
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) for (int c = 0; c < comps; c++) {
        int32_t d = lj_residual(s, row_len, (uint32_t)x, (uint32_t)y, comps, c, precision);
        freq[d == -32768 ? 16 : ssss(d)]++;
    }
}
/* Encodes `s` (width x height samples x comps, raster) with predictor 1. */
int lj_encode(Buf *out, const uint16_t *s, int width, int height, int comps, int precision,
              const uint8_t counts[16], const uint8_t *symbols, int nsym) {
    uint16_t code[256] = {0}; uint8_t size[256] = {0};
    huff_codes(counts, symbols, code, size);
    uint8_t hdr[64]; int n = 0;
    hdr[n++] = 0xFF; hdr[n++] = 0xD8;
    hdr[n++] = 0xFF; hdr[n++] = 0xC3; hdr[n++] = 0; hdr[n++] = (uint8_t)(8 + 3 * comps); hdr[n++] = (uint8_t)precision;
    hdr[n++] = (uint8_t)(height >> 8); hdr[n++] = (uint8_t)height; hdr[n++] = (uint8_t)(width >> 8); hdr[n++] = (uint8_t)width;
    hdr[n++] = (uint8_t)comps;
    for (int c = 0; c < comps; c++) { hdr[n++] = (uint8_t)c; hdr[n++] = 0x11; hdr[n++] = 0; }
    buf_put(out, hdr, (uint32_t)n);
    uint8_t dht[4 + 17 + 256]; int m = 0;
    dht[m++] = 0xFF; dht[m++] = 0xC4; dht[m++] = (uint8_t)((19 + nsym) >> 8); dht[m++] = (uint8_t)(19 + nsym); dht[m++] = 0x00;
    for (int i = 0; i < 16; i++) dht[m++] = counts[i];
    for (int i = 0; i < nsym; i++) dht[m++] = symbols[i];
    buf_put(out, dht, (uint32_t)m);
    n = 0;
    hdr[n++] = 0xFF; hdr[n++] = 0xDA; hdr[n++] = 0; hdr[n++] = (uint8_t)(6 + 2 * comps); hdr[n++] = (uint8_t)comps;
    for (int c = 0; c < comps; c++) { hdr[n++] = (uint8_t)c; hdr[n++] = 0x00; }
    hdr[n++] = 1; hdr[n++] = 0; hdr[n++] = 0;    /* predictor 1, Se, Ah/Al */
    buf_put(out, hdr, (uint32_t)n);
    BitW w = {out, 0, 0};
    uint32_t row_len = (uint32_t)width * comps;
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) for (int c = 0; c < comps; c++) {
        int32_t d = lj_residual(s, row_len, (uint32_t)x, (uint32_t)y, comps, c, precision);
        int t = d == -32768 ? 16 : ssss(d);
        if (!size[t]) return ERR_CORRUPT;
        bitw_put(&w, code[t], size[t]);
        if (t && t < 16) bitw_put(&w, (uint32_t)(d < 0 ? d - 1 : d), t);
    }
    bitw_flush(&w);
    buf_byte(out, 0xFF); buf_byte(out, 0xD9);
    return out->failed ? ERR_MEMORY : ERR_OK;
}
