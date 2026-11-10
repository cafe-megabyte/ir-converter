/* DNG reading: Bayer CFA raw images and linear (demosaiced) camera RGB,
 * uncompressed or lossless JPEG, tiled or in strips, with all linearisation
 * and colour metadata. */
#include "raw.h"

void raw_free(Raw *raw) {
    xfree(raw->data); xfree(raw->lin); xfree(raw->black_h); xfree(raw->black_v);
    xmemset(raw, 0, sizeof(*raw));
}

static TiffIfd *find_raw_ifd(const Tiff *t, int *photometric) {
    const TiffIfd *cand[24]; int n = 0;
    cand[n++] = t->ifd0;
    TiffEntry *subs = tiff_find(t->ifd0, 0x014A);
    for (uint32_t i = 0; subs && i < subs->nsub && n < 24; i++) cand[n++] = subs->sub[i];
    for (int i = 0; i < n; i++) {
        if (tiff_get_uint(t, cand[i], 0x00FE, 0) != 0) continue;
        uint32_t ph = tiff_get_uint(t, cand[i], 0x0106, 0);
        if (ph == 32803 || ph == 34892) { *photometric = (int)ph; return (TiffIfd *)cand[i]; }
    }
    return NULL;
}

/* Decodes one tile or strip of bw x bh pixels with spp interleaved samples. */
static int decode_block(const Tiff *t, const Blob *b, uint32_t comp, uint32_t bps, uint32_t spp, uint16_t *out, uint32_t bw, uint32_t bh) {
    uint64_t n = (uint64_t)bw * bh * spp;
    if (comp == 1) {
        if (bps == 16) {
            if (b->len < n * 2) return ERR_CORRUPT;
            for (uint64_t i = 0; i < n; i++) out[i] = rd16(b->ptr + 2 * i, t->be);
        } else if (bps == 8) {
            if (b->len < n) return ERR_CORRUPT;
            for (uint64_t i = 0; i < n; i++) out[i] = b->ptr[i];
        } else {
            uint64_t row_bytes = ((uint64_t)bw * spp * bps + 7) / 8, row = (uint64_t)bw * spp;
            if (b->len < row_bytes * bh) return ERR_CORRUPT;
            for (uint32_t y = 0; y < bh; y++) {
                Bits r; bits_init(&r, b->ptr + row_bytes * y, b->ptr + row_bytes * (y + 1), 0);
                for (uint64_t x = 0; x < row; x++) out[y * row + x] = (uint16_t)bits_get(&r, (int)bps);
            }
        }
        return ERR_OK;
    }
    if (comp == 7) {
        LjInfo info;
        int r = ljpeg_decode(b->ptr, b->len, out, (uint32_t)n, &info);
        if (r) return r;
        if ((uint64_t)info.width * info.comps * info.height != n) return ERR_CORRUPT;
        return ERR_OK;
    }
    return ERR_UNSUPPORTED | (UNSUP_COMPRESSION << 8);
}

static int read_matrix(const Tiff *t, const TiffIfd *ifd, uint16_t tag, double *m) {
    return tiff_get_nums(t, ifd, tag, m, 9);
}

int dng_load(Raw *raw, const Tiff *t) {
    const TiffIfd *i0 = t->ifd0;
    if (!tiff_find(i0, 0xC612)) return ERR_FORMAT;
    int photometric = 0;
    TiffIfd *ifd = find_raw_ifd(t, &photometric);
    if (!ifd) return ERR_UNSUPPORTED | (UNSUP_NO_RAW << 8);
    int linear = photometric == 34892;
    uint32_t w = tiff_get_uint(t, ifd, 0x0100, 0), h = tiff_get_uint(t, ifd, 0x0101, 0);
    uint32_t bps = tiff_get_uint(t, ifd, 0x0102, 16), comp = tiff_get_uint(t, ifd, 0x0103, 1);
    uint32_t spp = tiff_get_uint(t, ifd, 0x0115, 1);
    if (!w || !h || w > 30000 || h > 30000) return ERR_FORMAT;
    /* Linear DNGs: three samples of camera RGB (the profile's channels). */
    if (linear && spp != 3) return ERR_UNSUPPORTED | (UNSUP_LINEAR_RAW << 8);
    if ((!linear && spp != 1) || bps < 8 || bps > 16 || tiff_get_uint(t, ifd, 0x011C, 1) != 1) return ERR_UNSUPPORTED | (UNSUP_LAYOUT << 8);
    if (comp != 1 && comp != 7) return ERR_UNSUPPORTED | (UNSUP_COMPRESSION << 8);
    if (!linear) {
        if (tiff_get_uint(t, ifd, 0x828D, 2) != 2 || tiff_get_num(t, ifd, 0x828D, 1, 2) != 2 || tiff_get_uint(t, ifd, 0xC617, 1) != 1)
            return ERR_UNSUPPORTED | (UNSUP_CFA << 8);
        TiffEntry *pat = tiff_find(ifd, 0x828E);
        if (!pat || pat->count != 4) return ERR_UNSUPPORTED | (UNSUP_CFA << 8);
        int plane[3] = {0, 1, 2};
        TiffEntry *pc = tiff_find(ifd, 0xC616);
        if (pc) { if (pc->count != 3) return ERR_UNSUPPORTED | (UNSUP_CFA << 8); for (int i = 0; i < 3; i++) plane[i] = pc->data[i]; }
        int counts[3] = {0, 0, 0};
        for (int i = 0; i < 4; i++) {
            int v = pat->data[i];
            if (v > 2 || plane[v] > 2) return ERR_UNSUPPORTED | (UNSUP_CFA << 8);
            raw->cfa_aa[i] = plane[v]; counts[plane[v]]++;
        }
        if (counts[0] != 1 || counts[1] != 2 || counts[2] != 1) return ERR_UNSUPPORTED | (UNSUP_CFA << 8);
    }

    raw->spp = (int)spp;
    raw->width = w; raw->height = h;
    raw->data = xalloc((size_t)w * h * spp * sizeof(uint16_t));
    if (!raw->data) return ERR_MEMORY;
    TiffEntry *tiles = tiff_find(ifd, 0x0144), *strips = tiff_find(ifd, 0x0111);
    uint32_t bw, bh, across, down;
    TiffEntry *blocks;
    if (tiles) {
        bw = tiff_get_uint(t, ifd, 0x0142, 0); bh = tiff_get_uint(t, ifd, 0x0143, 0);
        if (!bw || !bh || bw > 65536 || bh > 65536) return ERR_FORMAT;
        across = (w + bw - 1) / bw; down = (h + bh - 1) / bh; blocks = tiles;
    } else if (strips) {
        bw = w; bh = tiff_get_uint(t, ifd, 0x0116, h); if (!bh || bh > h) bh = h;
        across = 1; down = (h + bh - 1) / bh; blocks = strips;
    } else return ERR_FORMAT;
    if (blocks->nblobs < across * down) return ERR_CORRUPT;
    uint16_t *tmp = xalloc((size_t)bw * bh * spp * sizeof(uint16_t));
    if (!tmp) return ERR_MEMORY;
    int r = ERR_OK;
    for (uint32_t ty = 0; ty < down && !r; ty++)
        for (uint32_t tx = 0; tx < across && !r; tx++) {
            uint32_t rows = tiles ? bh : MIN(bh, h - ty * bh);   /* last strip may be short */
            r = decode_block(t, &blocks->blobs[ty * across + tx], comp, bps, spp, tmp, bw, rows);
            if (r) break;
            uint32_t x0 = tx * bw, y0 = ty * bh, cw = MIN(bw, w - x0), ch = MIN(rows, h - y0);
            for (uint32_t y = 0; y < ch; y++)
                xmemcpy(raw->data + ((uint64_t)(y0 + y) * w + x0) * spp, tmp + (uint64_t)y * bw * spp, (size_t)cw * spp * sizeof(uint16_t));
        }
    xfree(tmp);
    if (r) return r;

    /* Areas: ActiveArea (top, left, bottom, right), crop relative to it. */
    raw->aa_top = 0; raw->aa_left = 0; raw->aa_bottom = h; raw->aa_right = w;
    double aa[4];
    if (tiff_get_nums(t, ifd, 0xC68D, aa, 4) && aa[0] < aa[2] && aa[1] < aa[3] && aa[2] <= h && aa[3] <= w) {
        raw->aa_top = (uint32_t)aa[0]; raw->aa_left = (uint32_t)aa[1]; raw->aa_bottom = (uint32_t)aa[2]; raw->aa_right = (uint32_t)aa[3];
    }
    uint32_t aw = raw->aa_right - raw->aa_left, ah = raw->aa_bottom - raw->aa_top;
    double co[2] = {0, 0}, cs[2] = {aw, ah};
    tiff_get_nums(t, ifd, 0xC61F, co, 2); tiff_get_nums(t, ifd, 0xC620, cs, 2);
    uint32_t cx = (uint32_t)(co[0] + 0.5), cy = (uint32_t)(co[1] + 0.5), cw = (uint32_t)(cs[0] + 0.5), chh = (uint32_t)(cs[1] + 0.5);
    if (cx + cw > aw || cy + chh > ah || !cw || !chh) { cx = cy = 0; cw = aw; chh = ah; }
    raw->crop_x = raw->aa_left + cx; raw->crop_y = raw->aa_top + cy; raw->crop_w = cw; raw->crop_h = chh;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++)
        raw->cfa[((raw->aa_top + y) & 1) * 2 + ((raw->aa_left + x) & 1)] = raw->cfa_aa[y * 2 + x];

    /* Linearisation and levels. */
    TiffEntry *lin = tiff_find(ifd, 0xC618);
    if (lin && lin->count >= 2 && lin->count <= 65536) {
        raw->lin = xalloc(lin->count * sizeof(uint16_t));
        if (!raw->lin) return ERR_MEMORY;
        for (uint32_t i = 0; i < lin->count; i++) raw->lin[i] = (uint16_t)tiff_uint(t, lin, i);
        raw->lin_n = lin->count;
    }
    uint32_t rr = tiff_get_uint(t, ifd, 0xC619, 1), rc = (uint32_t)tiff_get_num(t, ifd, 0xC619, 1, 1);
    TiffEntry *bl = tiff_find(ifd, 0xC61A);
    for (int i = 0; i < 4; i++) raw->black[i] = 0;
    if (bl && rr && rc && rr <= 16 && rc <= 16 && linear) {
        /* One value per sample in each repeat cell: average the cells. */
        for (uint32_t c = 0; c < spp; c++) {
            double sum = 0; int cnt = 0;
            for (uint32_t k = c; k < rr * rc * spp && k < bl->count; k += spp) { sum += tiff_num(t, bl, k); cnt++; }
            raw->black[c] = cnt ? sum / cnt : 0;
        }
    } else if (bl && rr && rc && rr <= 16 && rc <= 16) {
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
            /* Average all repeat cells that share this CFA parity. */
            double sum = 0; int cnt = 0;
            for (uint32_t ry = (uint32_t)y % rr; ry < rr; ry += (rr == 1 ? 1 : 2))
                for (uint32_t rx = (uint32_t)x % rc; rx < rc; rx += (rc == 1 ? 1 : 2)) {
                    uint32_t k = ry * rc + rx;
                    if (k < bl->count) { sum += tiff_num(t, bl, k); cnt++; }
                }
            raw->black[((raw->aa_top + y) & 1) * 2 + ((raw->aa_left + x) & 1)] = cnt ? sum / cnt : 0;
        }
    }
    TiffEntry *dh = tiff_find(ifd, 0xC61B), *dv = tiff_find(ifd, 0xC61C);
    if (dh && dh->count == aw) {
        raw->black_h = xalloc(aw * sizeof(double));
        if (!raw->black_h) return ERR_MEMORY;
        for (uint32_t i = 0; i < aw; i++) raw->black_h[i] = tiff_num(t, dh, i);
    }
    if (dv && dv->count == ah) {
        raw->black_v = xalloc(ah * sizeof(double));
        if (!raw->black_v) return ERR_MEMORY;
        for (uint32_t i = 0; i < ah; i++) raw->black_v[i] = tiff_num(t, dv, i);
    }
    raw->white = tiff_get_num(t, ifd, 0xC61D, 0, (double)((1u << bps) - 1));

    /* Colour. */
    raw->n_ill = 0;
    if (read_matrix(t, i0, 0xC621, raw->cm[0])) {
        raw->n_ill = 1;
        raw->ill[0] = (int)tiff_get_uint(t, i0, 0xC65A, 21);
        if (read_matrix(t, i0, 0xC622, raw->cm[1])) { raw->n_ill = 2; raw->ill[1] = (int)tiff_get_uint(t, i0, 0xC65B, 21); }
    }
    if (!raw->n_ill) return ERR_UNSUPPORTED | (UNSUP_LAYOUT << 8);
    raw->has_fm = read_matrix(t, i0, 0xC714, raw->fm[0]) && (raw->n_ill == 1 || read_matrix(t, i0, 0xC715, raw->fm[1]));
    /* Camera calibration applies only if its signature matches the profile's. */
    char csig[128], psig[128];
    tiff_get_str(i0, 0xC6F3, csig, sizeof csig); tiff_get_str(i0, 0xC6F4, psig, sizeof psig);
    int use_cc = 1;
    for (int i = 0; i < 128; i++) { if (csig[i] != psig[i]) { use_cc = 0; break; } if (!csig[i]) break; }
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 9; i++) raw->cc[k][i] = i % 4 == 0;
        if (use_cc) read_matrix(t, i0, (uint16_t)(0xC623 + k), raw->cc[k]);
    }
    for (int i = 0; i < 3; i++) raw->ab[i] = tiff_get_num(t, i0, 0xC627, (uint32_t)i, 1);
    if (tiff_get_nums(t, i0, 0xC628, raw->as_shot, 3) && raw->as_shot[0] > 0 && raw->as_shot[1] > 0 && raw->as_shot[2] > 0)
        raw->has_as_shot = 1;
    else if (tiff_get_nums(t, i0, 0xC629, raw->as_shot_xy, 2) && raw->as_shot_xy[0] > 0 && raw->as_shot_xy[1] > 0)
        raw->has_as_shot_xy = 1;
    raw->baseline_exposure = tiff_get_num(t, i0, 0xC62A, 0, 0);
    raw->orientation = (int)tiff_get_uint(t, i0, 0x0112, 1);
    tiff_get_str(i0, 0x010F, raw->make, sizeof raw->make);
    tiff_get_str(i0, 0x0110, raw->model, sizeof raw->model);
    tiff_get_str(i0, 0xC614, raw->unique_model, sizeof raw->unique_model);
    raw->kind = RAW_DNG;
    raw->raw_ifd = ifd;
    return ERR_OK;
}
