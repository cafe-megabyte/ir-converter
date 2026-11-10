/* Nikon NEF raw decoding (compression 34713, lossy and lossless variants,
 * 12/14 bit) and uncompressed NEF. The decompressed values are the codes
 * that a DNG stores together with the NEF curve as LinearizationTable. */
#include "raw.h"

/* Huffman specifications: 16 code-length counts followed by symbols.
 * A symbol encodes length (low nibble) and pre-shift (high nibble). */
static const uint8_t nikon_tree[6][32] = {
    {0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0, 5,4,3,6,2,7,1,0,8,9,11,10,12},                 /* 12-bit lossy */
    {0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0, 0x39,0x5a,0x38,0x27,0x16,5,4,3,2,1,0,11,12,12}, /* 12-bit lossy after split */
    {0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0, 5,4,6,3,7,2,8,1,9,0,10,11,12},                 /* 12-bit lossless */
    {0,1,4,3,1,1,1,1,1,2,0,0,0,0,0,0, 5,6,4,7,8,3,9,2,1,0,10,11,12,13,14},           /* 14-bit lossy */
    {0,1,5,1,1,1,1,1,1,1,2,0,0,0,0,0, 8,0x5c,0x4b,0x3a,0x29,7,6,5,4,3,2,1,0,13,14},  /* 14-bit lossy after split */
    {0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0, 7,6,8,5,9,4,10,3,11,12,2,0,1,13,14}            /* 14-bit lossless */
};
static int tree_symbols(const uint8_t *t) { int n = 0; for (int i = 0; i < 16; i++) n += t[i]; return n; }

/* Reads MakerNote tag 0x96 (linearization/compression metadata) and builds
 * the curve. Returns ERR_OK and fills tree/split/vpred. */
typedef struct { int tree, split; uint16_t vpred[4]; uint16_t *curve; uint32_t curve_len; } NikonMeta;
static int nikon_meta(const Tiff *mn, int bps, NikonMeta *m) {
    TiffEntry *e = tiff_find(mn->ifd0, 0x0096);
    xmemset(m, 0, sizeof(*m));
    uint32_t full = 1u << 16;
    m->curve = xalloc(full * sizeof(uint16_t));
    if (!m->curve) return ERR_MEMORY;
    for (uint32_t i = 0; i < full; i++) m->curve[i] = (uint16_t)i;
    uint32_t max = (1u << bps) & 0x7FFF;
    m->curve_len = max ? max : 0x4000;
    if (!e || e->size < 16) return ERR_UNSUPPORTED;
    const uint8_t *p = e->data, *end = e->data + e->size;
    int be = mn->be;
    int ver0 = p[0], ver1 = p[1];
    const uint8_t *q = p + 2;
    if (ver0 == 0x49 || ver1 == 0x58) q += 2110;
    if (ver0 == 0x46) m->tree = 2;
    if (bps == 14) m->tree += 3;
    if (q + 10 > end) return ERR_CORRUPT;
    for (int i = 0; i < 4; i++) m->vpred[i] = rd16(q + 2 * i, be);
    q += 8;
    uint32_t csize = rd16(q, be); q += 2;
    uint32_t step = csize > 1 ? max / (csize - 1) : 0;
    if (ver0 == 0x44 && ver1 == 0x20 && step > 0) {
        if (q + 2 * csize > end || (csize - 1) * step >= full) return ERR_CORRUPT;
        for (uint32_t i = 0; i < csize; i++) m->curve[i * step] = rd16(q + 2 * i, be);
        for (uint32_t i = 0; i < max; i++) {
            uint32_t r = i % step, a = i - r;
            m->curve[i] = (uint16_t)((m->curve[a] * (step - r) + m->curve[a + step] * r) / step);
        }
        if (p + 562 + 2 <= end) m->split = rd16(p + 562, be);
    } else if (ver0 != 0x46 && csize <= 0x4001) {
        if (q + 2 * csize > end) return ERR_CORRUPT;
        for (uint32_t i = 0; i < csize; i++) m->curve[i] = rd16(q + 2 * i, be);
        max = csize;
    }
    while (max > 2 && m->curve[max - 2] == m->curve[max - 1]) max--;
    m->curve_len = max;
    return ERR_OK;
}

static int nikon_decode(const uint8_t *data, uint32_t len, uint32_t width, uint32_t height,
                        const NikonMeta *m, uint16_t *out, uint32_t *max_code) {
    Huff h = {0};
    const uint8_t *t = nikon_tree[m->tree];
    int r = huff_build(&h, t, t + 16, tree_symbols(t));
    if (r) return r;
    Bits b; bits_init(&b, data, data + len, 0);
    int16_t vpred[2][2] = {{(int16_t)m->vpred[0], (int16_t)m->vpred[1]}, {(int16_t)m->vpred[2], (int16_t)m->vpred[3]}};
    int16_t hpred[2] = {0, 0};
    uint32_t top = 0;
    for (uint32_t row = 0; row < height; row++) {
        if (m->split && row == (uint32_t)m->split) {
            huff_free(&h);
            const uint8_t *t2 = nikon_tree[m->tree + 1];
            if ((r = huff_build(&h, t2, t2 + 16, tree_symbols(t2)))) return r;
        }
        uint16_t *o = out + (uint64_t)row * width;
        for (uint32_t col = 0; col < width; col++) {
            int sym = huff_decode(&h, &b);
            if (sym < 0) { huff_free(&h); return ERR_CORRUPT; }
            int len = sym & 15, shl = sym >> 4;
            int32_t diff = 0;
            if (len) {
                int n = len - shl;
                diff = (int32_t)(((bits_get(&b, n < 0 ? 0 : n) << 1) + 1) << shl >> 1);
                if ((diff & (1 << (len - 1))) == 0) diff -= (1 << len) - !shl;
            }
            int16_t v;
            if (col < 2) v = hpred[col] = (int16_t)(vpred[row & 1][col] += (int16_t)diff);
            else v = hpred[col & 1] = (int16_t)(hpred[col & 1] + diff);
            uint16_t code = (uint16_t)CLAMP(v, 0, 0x3FFF);
            o[col] = code;
            if (code > top) top = code;
        }
    }
    huff_free(&h);
    *max_code = top;
    return ERR_OK;
}

/* Fills `raw` from a parsed NEF container. */
int nef_load(Raw *raw, const Tiff *t) {
    char make[64]; tiff_get_str(t->ifd0, 0x010F, make, sizeof make);
    if (xmemcmp(make, "NIKON", 5)) return ERR_FORMAT;
    /* The raw image: the sub-IFD with NewSubFileType 0 and CFA photometric. */
    TiffIfd *ifd = NULL;
    TiffEntry *subs = tiff_find(t->ifd0, 0x014A);
    for (uint32_t i = 0; subs && i < subs->nsub; i++) {
        TiffIfd *s = subs->sub[i];
        if (tiff_get_uint(t, s, 0x00FE, 1) == 0 && tiff_get_uint(t, s, 0x0106, 0) == 32803) { ifd = s; break; }
    }
    if (!ifd) return ERR_UNSUPPORTED | (UNSUP_NO_RAW << 8);
    uint32_t w = tiff_get_uint(t, ifd, 0x0100, 0), h = tiff_get_uint(t, ifd, 0x0101, 0);
    uint32_t bps = tiff_get_uint(t, ifd, 0x0102, 12), comp = tiff_get_uint(t, ifd, 0x0103, 1);
    TiffEntry *strips = tiff_find(ifd, 0x0111);
    if (!w || !h || w > 20000 || h > 20000 || !strips || !strips->nblobs) return ERR_FORMAT;
    if (bps != 12 && bps != 14) return ERR_UNSUPPORTED | (UNSUP_LAYOUT << 8);
    /* MakerNote: "Nikon\0" + version + embedded TIFF. */
    TiffEntry *exif = tiff_find(t->ifd0, 0x8769);
    TiffEntry *mne = exif && exif->nsub ? tiff_find(exif->sub[0], 0x927C) : NULL;
    Tiff mn = {0};
    if (!mne || mne->size < 18 || xmemcmp(mne->data, "Nikon\0", 6) || tiff_parse(&mn, mne->data + 10, mne->size - 10))
        return ERR_UNSUPPORTED | (UNSUP_COMPRESSION << 8);
    raw->spp = 1;
    raw->nikon_mn = mne->data; raw->nikon_mn_len = mne->size;
    raw->nikon_mn_be = mn.be;
    raw->nikon_mn_offset = (uint32_t)(mne->data - t->base);
    /* White balance: WB_RBLevels (red and blue multipliers relative to green). */
    TiffEntry *wb = tiff_find(mn.ifd0, 0x000C);
    if (wb && wb->count >= 2 && tiff_num(&mn, wb, 0) > 0 && tiff_num(&mn, wb, 1) > 0) {
        raw->as_shot[0] = 1.0 / tiff_num(&mn, wb, 0); raw->as_shot[1] = 1.0; raw->as_shot[2] = 1.0 / tiff_num(&mn, wb, 1);
        raw->has_as_shot = 1;
    }
    /* Black level (newer models: tag 0x3D, four values at the sensor bit depth). */
    TiffEntry *bl = tiff_find(mn.ifd0, 0x003D);
    double black = 0;
    if (bl && bl->count >= 4) black = (tiff_num(&mn, bl, 0) + tiff_num(&mn, bl, 1) + tiff_num(&mn, bl, 2) + tiff_num(&mn, bl, 3)) / 4;

    raw->width = w; raw->height = h;
    raw->data = xalloc((size_t)w * h * sizeof(uint16_t));
    if (!raw->data) { tiff_free(&mn); return ERR_MEMORY; }
    int r = ERR_OK;
    uint32_t max_code = 0;
    if (comp == 34713) {
        NikonMeta m;
        r = nikon_meta(&mn, (int)bps, &m);
        if (r == ERR_OK) r = nikon_decode(strips->blobs[0].ptr, strips->blobs[0].len, w, h, &m, raw->data, &max_code);
        if (r == ERR_OK) {
            /* The curve becomes the DNG LinearizationTable; codes beyond its
             * end map to its last entry, exactly like the camera curve's
             * saturated plateau. An identity curve needs no table. */
            uint32_t n = m.curve_len, identity = 1;
            for (uint32_t i = 0; i < n && identity; i++) identity = m.curve[i] == i;
            if (max_code >= n && !identity) n = MIN(max_code + 1, 0x4000u);
            if (!identity) {
                raw->lin = xalloc(n * sizeof(uint16_t));
                if (!raw->lin) r = ERR_MEMORY;
                else { xmemcpy(raw->lin, m.curve, n * sizeof(uint16_t)); raw->lin_n = n; }
            }
            raw->white = m.curve[m.curve_len - 1];
        }
        xfree(m.curve);
        if (r == ERR_UNSUPPORTED) r = ERR_UNSUPPORTED | (UNSUP_COMPRESSION << 8);
    } else if (comp == 1) {
        const Blob *s = &strips->blobs[0];
        uint64_t n = (uint64_t)w * h;
        if (strips->nblobs == 1 && s->len >= n * 2) {
            for (uint64_t i = 0; i < n; i++) raw->data[i] = rd16(s->ptr + 2 * i, t->be);
        } else if (strips->nblobs == 1 && s->len >= n * bps / 8) {
            Bits b; bits_init(&b, s->ptr, s->ptr + s->len, 0);
            for (uint64_t i = 0; i < n; i++) raw->data[i] = (uint16_t)bits_get(&b, (int)bps);
        } else r = ERR_UNSUPPORTED | (UNSUP_LAYOUT << 8);
        raw->white = (double)((1u << bps) - 1);
    } else r = ERR_UNSUPPORTED | (UNSUP_COMPRESSION << 8);
    tiff_free(&mn);
    if (r) return r;
    for (int i = 0; i < 4; i++) raw->black[i] = black;

    /* CFA pattern of the raw IFD. */
    TiffEntry *cfa = tiff_find(ifd, 0x828E);
    if (!cfa || cfa->count != 4) return ERR_UNSUPPORTED | (UNSUP_CFA << 8);
    for (int i = 0; i < 4; i++) { raw->cfa[i] = cfa->data[i]; raw->cfa_aa[i] = cfa->data[i]; if (raw->cfa[i] > 2) return ERR_UNSUPPORTED | (UNSUP_CFA << 8); }
    raw->orientation = (int)tiff_get_uint(t, t->ifd0, 0x0112, 1);
    tiff_get_str(t->ifd0, 0x010F, raw->make, sizeof raw->make);
    tiff_get_str(t->ifd0, 0x0110, raw->model, sizeof raw->model);
    raw->kind = RAW_NEF;
    raw->raw_ifd = ifd;
    raw->aa_top = 0; raw->aa_left = 0; raw->aa_bottom = h; raw->aa_right = w;
    raw->crop_x = 0; raw->crop_y = 0; raw->crop_w = w; raw->crop_h = h;
    raw->baseline_exposure = 0;
    camera_apply(raw);
    return ERR_OK;
}
