/* Exports: lossless DNG (raw data bit-exact; white balance, embedded profile
 * and previews updated) and full-resolution JPEG. */
#include "raw.h"
#include "srgb_icc.h"

#define APP_NAME "IR Converter"
#define APP_VERSION "1.0"
#define PREVIEW_QUALITY 85
#define JPEG_QUALITY 75

/* ---- XMP: make the embedded profile the default, drop stored WB ---- */
static int xmp_find(const char *s, uint32_t n, const char *pat, uint32_t from) {
    uint32_t m = (uint32_t)xstrlen(pat);
    for (uint32_t i = from; i + m <= n; i++) if (!xmemcmp(s + i, pat, m)) return (int)i;
    return -1;
}
/* Removes ` name="..."` attributes and `<name>...</name>` elements. */
static uint32_t xmp_strip(char *s, uint32_t n, const char *name) {
    char attr[64], open[64], close[64];
    uint32_t k = (uint32_t)xstrlen(name);
    if (k > 50) return n;
    xmemcpy(attr, name, k); attr[k] = '='; attr[k + 1] = '"'; attr[k + 2] = 0;
    open[0] = '<'; xmemcpy(open + 1, name, k); open[k + 1] = '>'; open[k + 2] = 0;
    close[0] = '<'; close[1] = '/'; xmemcpy(close + 2, name, k); close[k + 2] = '>'; close[k + 3] = 0;
    int i;
    while ((i = xmp_find(s, n, attr, 0)) >= 0) {
        if (i == 0 || (s[i - 1] != ' ' && s[i - 1] != '\n' && s[i - 1] != '\t' && s[i - 1] != '\r')) break;
        int e = xmp_find(s, n, "\"", (uint32_t)i + k + 2);
        if (e < 0) break;
        uint32_t start = (uint32_t)i - 1, end = (uint32_t)e + 1;
        xmemmove(s + start, s + end, n - end); n -= end - start;
    }
    while ((i = xmp_find(s, n, open, 0)) >= 0) {
        int e = xmp_find(s, n, close, (uint32_t)i);
        if (e < 0) break;
        uint32_t end = (uint32_t)e + (uint32_t)xstrlen(close);
        xmemmove(s + i, s + end, n - end); n -= end - (uint32_t)i;
    }
    return n;
}
static void set_xmp(Tiff *t, TiffIfd *i0) {
    static const char inject[] = " crd:CameraProfile=\"" APP_NAME "\"";
    static const char ns[] = " xmlns:crd=\"http://ns.adobe.com/camera-raw-defaults/1.0/\"";
    TiffEntry *e = tiff_find(i0, 0x02BC);
    uint32_t n = e ? e->size : 0;
    char *s = tiff_alloc(t, n + sizeof inject + sizeof ns + 512);
    if (!s) return;
    int desc = -1;
    if (e && n) {
        xmemcpy(s, e->data, n);
        static const char *drop[] = {"crs:WhiteBalance", "crs:Temperature", "crs:Tint", "crs:CameraProfile",
                                     "crs:CameraProfileDigest", "crd:CameraProfile", "crd:CameraProfileDigest"};
        for (uint32_t i = 0; i < sizeof drop / sizeof drop[0]; i++) n = xmp_strip(s, n, drop[i]);
        desc = xmp_find(s, n, "<rdf:Description", 0);
    }
    if (desc < 0) {
        static const char minimal[] =
            "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
            "<rdf:Description rdf:about=\"\" xmlns:crd=\"http://ns.adobe.com/camera-raw-defaults/1.0/\" crd:CameraProfile=\"" APP_NAME "\"/>"
            "</rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>";
        tiff_set_raw(t, i0, 0x02BC, T_BYTE, sizeof minimal - 1, minimal);
        return;
    }
    /* End of the rdf:Description start tag (outside quoted values). */
    uint32_t i = (uint32_t)desc, q = 0;
    for (; i < n; i++) { if (s[i] == '"') q = !q; else if (!q && s[i] == '>') break; }
    if (i >= n) return;
    if (s[i - 1] == '/') i--;
    int has_ns = 0;
    for (uint32_t k = (uint32_t)desc; k + 9 < i; k++) if (!xmemcmp(s + k, "xmlns:crd", 9)) has_ns = 1;
    uint32_t add = (uint32_t)(sizeof inject - 1) + (has_ns ? 0 : (uint32_t)(sizeof ns - 1));
    xmemmove(s + i + add, s + i, n - i);
    if (!has_ns) { xmemcpy(s + i, ns, sizeof ns - 1); i += sizeof ns - 1; }
    xmemcpy(s + i, inject, sizeof inject - 1);
    n += add;
    TiffEntry *ne = tiff_find(i0, 0x02BC);
    uint16_t type = ne ? ne->type : T_BYTE;
    tiff_set_raw(t, i0, 0x02BC, type == T_UNDEFINED ? T_UNDEFINED : T_BYTE, n, s);
}

/* ---- profile: AsShotNeutral, scaled colour matrices, fixed forward matrix ---- */
static void set_profile(Tiff *t, TiffIfd *i0) {
    const Raw *r = &S.raw;
    double x, y, ref_xyz[3];
    temp_xy(5000, 0, &x, &y);
    xy_to_xyz(x, y, ref_xyz);
    M3 ab = m3_diag(r->ab);
    for (int s = 0; s < r->n_ill; s++) {
        M3 cm = m3_from(r->cm[s]), cc = m3_from(r->cc[s]), abcc = m3_mul(&ab, &cc), inv;
        if (!m3_inv(&abcc, &inv)) inv = m3_diag((double[3]){1, 1, 1});
        double ref[3], cam[3];
        m3_vec(&inv, S.n, ref);
        m3_vec(&cm, ref_xyz, cam);
        double k[3];
        for (int i = 0; i < 3; i++) k[i] = cam[i] > 1e-12 && ref[i] > 0 ? ref[i] / cam[i] : 1;
        M3 kd = m3_diag(k), out = m3_mul(&kd, &cm);
        tiff_set_srational(t, i0, (uint16_t)(0xC621 + s), out.m, 9);
    }
    /* Forward matrix reproducing exactly our rendering for AsShotNeutral. */
    M3 p = camera_to_pcs(&S.prof, S.n, S.temp, S.d0);
    double g = weight(&S.prof, 5000);
    M3 abcc = S.prof.n < 2 ? S.prof.abcc[0] : lerp(&S.prof.abcc[0], &S.prof.abcc[1], g), inv;
    if (!m3_inv(&abcc, &inv)) inv = m3_diag((double[3]){1, 1, 1});
    double ref[3], invn[3] = {1 / S.n[0], 1 / S.n[1], 1 / S.n[2]};
    m3_vec(&inv, S.n, ref);
    M3 a = m3_diag(invn), b = m3_diag(ref), t1 = m3_mul(&p, &a), t2 = m3_mul(&t1, &abcc), fm = m3_mul(&t2, &b);
    for (int s = 0; s < r->n_ill; s++) tiff_set_srational(t, i0, (uint16_t)(0xC714 + s), fm.m, 9);
    tiff_set_rational(t, i0, 0xC628, S.n, 3);
    tiff_remove(i0, 0xC629);                      /* AsShotWhiteXY */
    tiff_set_ascii(t, i0, 0xC6F8, APP_NAME);      /* ProfileName */
    tiff_remove(i0, 0xC6F6);                      /* AsShotProfileName */
    tiff_remove(i0, 0xC6F5);                      /* ExtraCameraProfiles */
    tiff_remove(i0, 0xCD31);                      /* CalibrationIlluminant3 */
    tiff_set_ascii(t, i0, 0x0131, APP_NAME " " APP_VERSION);
}
static void set_preview_info(Tiff *t, TiffIfd *ifd) {
    tiff_set_ascii(t, ifd, 0xC716, APP_NAME);
    tiff_set_ascii(t, ifd, 0xC717, APP_VERSION);
    tiff_remove(ifd, 0xC718); tiff_remove(ifd, 0xC719); tiff_remove(ifd, 0xC71B);
    tiff_set_long(t, ifd, 0xC71A, 2);             /* PreviewColorSpace: sRGB */
}

/* Keeps rendered preview buffers alive until the file is written. */
typedef struct { uint8_t *p[24]; int n; Buf jpegs[24]; int nj; } Keep;
static void keep_free(Keep *k) { for (int i = 0; i < k->n; i++) xfree(k->p[i]); for (int i = 0; i < k->nj; i++) buf_free(&k->jpegs[i]); }

/* Replaces the image of a rendered preview IFD by a new rendering of the
 * same size: uncompressed RGB stays uncompressed, everything else is JPEG. */
static int replace_preview(Tiff *t, TiffIfd *ifd, Keep *keep, int as_rgb) {
    uint32_t w = tiff_get_uint(t, ifd, 0x0100, 0), h = tiff_get_uint(t, ifd, 0x0101, 0);
    if (!w || !h || w > 20000 || h > 20000 || keep->n >= 24 || keep->nj >= 24) return ERR_FORMAT;
    uint8_t *rgb = render_preview(w, h);
    if (!rgb) return ERR_MEMORY;
    keep->p[keep->n++] = rgb;
    static const uint16_t bps[3] = {8, 8, 8};
    for (uint16_t tag = 0x0142; tag <= 0x0145; tag++) tiff_remove(ifd, tag);
    tiff_remove(ifd, 0x0201); tiff_remove(ifd, 0x0202);
    tiff_remove(ifd, 0xCD49); tiff_remove(ifd, 0xCD4A); tiff_remove(ifd, 0xCD4B);
    tiff_remove(ifd, 0xC740); tiff_remove(ifd, 0xC741); tiff_remove(ifd, 0xC74E); tiff_remove(ifd, 0xC7AA);
    tiff_set_u16(t, ifd, 0x0102, bps, 3);
    tiff_set_short(t, ifd, 0x0115, 3);
    tiff_set_short(t, ifd, 0x011C, 1);
    tiff_set_long(t, ifd, 0x0116, h);
    Blob b;
    if (as_rgb) {
        tiff_set_short(t, ifd, 0x0103, 1);
        tiff_set_short(t, ifd, 0x0106, 2);
        tiff_remove(ifd, 0x0212);
        b.ptr = rgb; b.len = w * h * 3;
    } else {
        Buf *j = &keep->jpegs[keep->nj++];
        int r = jpeg_encode_rgb(j, rgb, w, h, PREVIEW_QUALITY);
        if (r) return r;
        tiff_set_short(t, ifd, 0x0103, 7);
        tiff_set_short(t, ifd, 0x0106, 6);
        static const uint16_t ss[2] = {2, 2};
        tiff_set_u16(t, ifd, 0x0212, ss, 2);
        b.ptr = j->data; b.len = j->len;
    }
    tiff_set_blobs(t, ifd, 0x0111, 0x0117, &b, 1);
    return t->failed ? ERR_MEMORY : ERR_OK;
}
static int is_rendered(const Tiff *t, const TiffIfd *ifd) {
    uint32_t ph = tiff_get_uint(t, ifd, 0x0106, 0);
    return tiff_get_uint(t, ifd, 0x00FE, 0) == 1 && (ph == 1 || ph == 2 || ph == 6);
}
static int is_plain_rgb(const Tiff *t, const TiffIfd *ifd) {
    return tiff_get_uint(t, ifd, 0x0103, 0) == 1 && tiff_get_uint(t, ifd, 0x0106, 0) == 2;
}
static void swap_cfa_entry(Tiff *t, TiffIfd *ifd) {
    TiffEntry *pat = tiff_find(ifd, 0x828E);
    if (!pat || pat->count != 4) return;
    int plane[3] = {0, 1, 2};
    TiffEntry *pc = tiff_find(ifd, 0xC616);
    if (pc && pc->count == 3) for (int i = 0; i < 3; i++) plane[i] = pc->data[i];
    int index_of[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++) if (plane[i] <= 2) index_of[plane[i]] = i;
    uint8_t v[4];
    for (int i = 0; i < 4; i++) {
        int c = pat->data[i] <= 2 ? plane[pat->data[i]] : 1;
        v[i] = (uint8_t)index_of[c == 1 ? 1 : 2 - c];
    }
    tiff_set_raw(t, ifd, 0x828E, pat->type == T_UNDEFINED ? T_UNDEFINED : T_BYTE, 4, v);
}

static int export_from_dng(void) {
    Tiff t;
    int r = tiff_parse(&t, S.file, S.len);
    if (r) return r;
    Keep keep = {0};
    TiffIfd *i0 = t.ifd0;
    /* Locate the raw IFD of this fresh parse (same position as at load). */
    /* Copy the sub-IFD list first: adding tags to IFD0 may move its entries. */
    TiffIfd *orig[16]; uint32_t norig = 0;
    TiffEntry *subs = tiff_find(i0, 0x014A);
    for (uint32_t i = 0; subs && i < subs->nsub && norig < 16; i++) orig[norig++] = subs->sub[i];
    TiffIfd *raw = NULL;
    if (tiff_get_uint(&t, i0, 0x00FE, 0) == 0) raw = i0;
    for (uint32_t i = 0; !raw && i < norig; i++)
        if (tiff_get_uint(&t, orig[i], 0x00FE, 1) == 0) raw = orig[i];
    if (!raw) { tiff_free(&t); return ERR_FORMAT; }
    if (S.swap) swap_cfa_entry(&t, raw);
    set_profile(&t, i0);
    set_xmp(&t, i0);
    if (tiff_find(i0, 0xC716)) set_preview_info(&t, i0);
    if (is_rendered(&t, i0)) r = replace_preview(&t, i0, &keep, is_plain_rgb(&t, i0));
    if (!r && norig) {
        TiffIfd *list[16]; uint32_t n = 0;
        for (uint32_t i = 0; i < norig && !r; i++) {
            TiffIfd *s = orig[i];
            uint32_t type = tiff_get_uint(&t, s, 0x00FE, 0), ph = tiff_get_uint(&t, s, 0x0106, 0);
            /* Reduced-resolution raw proxies (DNG 1.7 fast-load data) hold
             * unswapped camera RGB; with the swap they would be wrong. */
            if (S.swap && type == 1 && (ph == 34892 || ph == 32803)) continue;
            if (is_rendered(&t, s)) { r = replace_preview(&t, s, &keep, is_plain_rgb(&t, s)); set_preview_info(&t, s); }
            list[n++] = s;
        }
        if (!r) tiff_set_subs(&t, i0, 0x014A, list, n);
    }
    if (!r) { buf_free(&S.out); r = tiff_write(&t, &S.out); }
    keep_free(&keep);
    tiff_free(&t);
    return r;
}

/* ---- NEF -> DNG ---- */
static int jpeg_size(const uint8_t *p, uint32_t n, uint32_t *w, uint32_t *h) {
    uint32_t i = 2;
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return 0;
    while (i + 9 < n) {
        if (p[i] != 0xFF) { i++; continue; }
        uint8_t m = p[i + 1];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (uint32_t)(p[i + 5] << 8 | p[i + 6]); *w = (uint32_t)(p[i + 7] << 8 | p[i + 8]);
            return *w && *h;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        i += 2 + (uint32_t)(p[i + 2] << 8 | p[i + 3]);
    }
    return 0;
}
static int encode_raw_tiles(Tiff *t, TiffIfd *ifd, Buf *data) {
    const Raw *r = &S.raw;
    const uint32_t tw = 256, th = 256;
    uint32_t across = (r->width + tw - 1) / tw, down = (r->height + th - 1) / th, nt = across * down;
    uint16_t *tile = xalloc(tw * th * sizeof(uint16_t));
    uint32_t *offs = xalloc((nt + 1) * sizeof(uint32_t));
    Blob *blobs = xalloc(nt * sizeof(Blob));
    if (!tile || !offs || !blobs) { xfree(tile); xfree(offs); xfree(blobs); return ERR_MEMORY; }
    uint32_t freq[257] = {0};
    for (int pass = 0; pass < 2; pass++) {
        uint8_t counts[16], syms[256]; int nsym = 0;
        if (pass == 1) {
            for (int s = 0; s <= 16; s++) freq[s]++;
            huff_optimal(freq, counts, syms, &nsym);
        }
        for (uint32_t k = 0; k < nt; k++) {
            uint32_t x0 = (k % across) * tw, y0 = (k / across) * th;
            for (uint32_t y = 0; y < th; y++) {
                uint32_t sy = MIN(y0 + y, r->height - 1);
                for (uint32_t x = 0; x < tw; x++) {
                    /* Pad by repeating the last same-colour sample. */
                    uint32_t sx = x0 + x;
                    if (sx >= r->width) sx = r->width - 2 + ((sx - r->width) & 1);
                    if (y0 + y >= r->height) sy = r->height - 2 + ((y0 + y - r->height) & 1);
                    tile[y * tw + x] = r->data[(uint64_t)sy * r->width + sx];
                }
            }
            if (pass == 0) lj_count(tile, (int)tw / 2, (int)th, 2, 16, freq);
            else {
                offs[k] = data->len;
                int e = lj_encode(data, tile, (int)tw / 2, (int)th, 2, 16, counts, syms, nsym);
                if (e) { xfree(tile); xfree(offs); xfree(blobs); return e; }
            }
        }
    }
    offs[nt] = data->len;
    for (uint32_t k = 0; k < nt; k++) { blobs[k].ptr = data->data + offs[k]; blobs[k].len = offs[k + 1] - offs[k]; }
    tiff_set_long(t, ifd, 0x0142, tw);
    tiff_set_long(t, ifd, 0x0143, th);
    tiff_set_blobs(t, ifd, 0x0144, 0x0145, blobs, nt);
    xfree(tile); xfree(offs); xfree(blobs);
    return t->failed ? ERR_MEMORY : ERR_OK;
}
static void copy_tags(Tiff *t, TiffIfd *dst, const TiffIfd *src, const uint16_t *tags, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { TiffEntry *e = tiff_find(src, tags[i]); if (e) tiff_copy_entry(t, dst, e); }
}
static TiffIfd *copy_exif(Tiff *t, const TiffIfd *src) {
    TiffEntry *ex = tiff_find(src, 0x8769);
    if (!ex || !ex->nsub) return NULL;
    TiffIfd *s = ex->sub[0], *d = tiff_new_ifd(t);
    if (!d) return NULL;
    for (uint32_t i = 0; i < s->n; i++) if (s->e[i].tag != 0x927C) tiff_copy_entry(t, d, &s->e[i]);
    return d;
}
static int export_from_nef(void) {
    Tiff t;
    int r = tiff_parse(&t, S.file, S.len);
    if (r) return r;
    const Raw *rw = &S.raw;
    Keep keep = {0};
    Buf rawdata = {0};
    TiffIfd *n0 = t.ifd0;
    int thumb = tiff_get_uint(&t, n0, 0x0106, 0) == 2 && tiff_get_uint(&t, n0, 0x0100, 0) > 0;
    /* The NEF's large JPEG preview: a sub-IFD with JPEGInterchangeFormat. */
    uint32_t pw = 0, ph = 0;
    TiffEntry *subs = tiff_find(n0, 0x014A);
    for (uint32_t i = 0; subs && i < subs->nsub && !pw; i++) {
        TiffEntry *j = tiff_find(subs->sub[i], 0x0201);
        if (j && j->nblobs) jpeg_size(j->blobs[0].ptr, j->blobs[0].len, &pw, &ph);
    }
    TiffIfd *d0 = tiff_new_ifd(&t), *ri = thumb ? tiff_new_ifd(&t) : d0, *pv = pw ? tiff_new_ifd(&t) : NULL;
    if (!d0 || !ri || (pw && !pv)) { tiff_free(&t); return ERR_MEMORY; }
    static const uint16_t keep0[] = {0x010F, 0x0110, 0x0112, 0x011A, 0x011B, 0x0128, 0x0132, 0x013B, 0x8298, 0x8825, 0x9003};
    copy_tags(&t, d0, n0, keep0, sizeof keep0 / sizeof keep0[0]);
    tiff_remove(d0, 0x9003);
    TiffIfd *exif = copy_exif(&t, n0);
    if (exif) tiff_set_subs(&t, d0, 0x8769, &exif, 1);
    static const uint8_t ver[4] = {1, 4, 0, 0}, back[4] = {1, 1, 0, 0};
    tiff_set_raw(&t, d0, 0xC612, T_BYTE, 4, ver);
    tiff_set_raw(&t, d0, 0xC613, T_BYTE, 4, back);
    tiff_set_ascii(&t, d0, 0xC614, rw->unique_model[0] ? rw->unique_model : rw->model);
    for (int s = 0; s < rw->n_ill; s++) tiff_set_short(&t, d0, (uint16_t)(0xC65A + s), (uint16_t)rw->ill[s]);
    tiff_set_srational(&t, d0, 0xC62A, &rw->baseline_exposure, 1);
    tiff_set_rational(&t, d0, 0xC62B, &rw->baseline_noise, 1);
    tiff_set_rational(&t, d0, 0xC62C, &rw->baseline_sharpness, 1);
    tiff_set_rational(&t, d0, 0xC62E, &rw->linear_response_limit, 1);
    tiff_set_rational(&t, d0, 0xC633, &rw->shadow_scale, 1);
    tiff_set_long(&t, d0, 0xC6FD, 0);            /* ProfileEmbedPolicy: allow copying */
    set_profile(&t, d0);
    set_xmp(&t, d0);
    set_preview_info(&t, d0);
    if (S.name[0]) tiff_set_ascii(&t, d0, 0xC68B, S.name);
    /* Nikon MakerNote as DNG private data ("Adobe" "MakN"). */
    if (rw->nikon_mn && rw->nikon_mn_len) {
        uint32_t len = rw->nikon_mn_len + 6, tot = len + 14;
        uint8_t *p = tiff_alloc(&t, tot);
        if (p) {
            xmemcpy(p, "Adobe\0MakN", 10);
            wr32(p + 10, len, 1);
            p[14] = p[15] = t.be ? 'M' : 'I';
            wr32(p + 16, rw->nikon_mn_offset, 1);
            xmemcpy(p + 20, rw->nikon_mn, rw->nikon_mn_len);
            tiff_set_raw(&t, d0, 0xC634, T_BYTE, tot, p);
            tiff_set_short(&t, d0, 0xC635, 1);
        }
    }
    /* Raw IFD. */
    tiff_set_long(&t, ri, 0x00FE, 0);
    tiff_set_long(&t, ri, 0x0100, rw->width);
    tiff_set_long(&t, ri, 0x0101, rw->height);
    tiff_set_short(&t, ri, 0x0102, 16);
    tiff_set_short(&t, ri, 0x0103, 7);
    tiff_set_short(&t, ri, 0x0106, 32803);
    tiff_set_short(&t, ri, 0x0115, 1);
    tiff_set_short(&t, ri, 0x011C, 1);
    static const uint16_t dim[2] = {2, 2};
    tiff_set_u16(&t, ri, 0x828D, dim, 2);
    uint8_t pat[4];
    for (int i = 0; i < 4; i++) pat[i] = (uint8_t)(S.swap ? 2 - rw->cfa_aa[i] : rw->cfa_aa[i]);
    tiff_set_raw(&t, ri, 0x828E, T_BYTE, 4, pat);
    static const uint8_t planes[3] = {0, 1, 2};
    tiff_set_raw(&t, ri, 0xC616, T_BYTE, 3, planes);
    tiff_set_short(&t, ri, 0xC617, 1);
    if (rw->lin_n) tiff_set_u16(&t, ri, 0xC618, rw->lin, rw->lin_n);
    double bl[4]; int same = 1;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) bl[y * 2 + x] = rw->black[((rw->aa_top + y) & 1) * 2 + ((rw->aa_left + x) & 1)];
    for (int i = 1; i < 4; i++) same &= bl[i] == bl[0];
    uint16_t rep[2] = {(uint16_t)(same ? 1 : 2), (uint16_t)(same ? 1 : 2)};
    tiff_set_u16(&t, ri, 0xC619, rep, 2);
    tiff_set_rational(&t, ri, 0xC61A, bl, same ? 1 : 4);
    tiff_set_long(&t, ri, 0xC61D, (uint32_t)(rw->white + 0.5));
    double one2[2] = {1, 1}, org[2] = {rw->crop_x - rw->aa_left, rw->crop_y - rw->aa_top}, size[2] = {rw->crop_w, rw->crop_h};
    tiff_set_rational(&t, ri, 0xC61E, one2, 2);
    tiff_set_rational(&t, ri, 0xC61F, org, 2);
    tiff_set_rational(&t, ri, 0xC620, size, 2);
    tiff_set_rational(&t, ri, 0xC632, &rw->aa_strength, 1);
    tiff_set_rational(&t, ri, 0xC65C, one2, 1);
    uint32_t area[4] = {rw->aa_top, rw->aa_left, rw->aa_bottom, rw->aa_right};
    tiff_set_u32(&t, ri, 0xC68D, area, 4);
    r = encode_raw_tiles(&t, ri, &rawdata);
    /* Previews of the same sizes as in the NEF. */
    if (!r && thumb) {
        tiff_set_long(&t, d0, 0x00FE, 1);
        tiff_set_long(&t, d0, 0x0100, tiff_get_uint(&t, n0, 0x0100, 0));
        tiff_set_long(&t, d0, 0x0101, tiff_get_uint(&t, n0, 0x0101, 0));
        tiff_set_short(&t, d0, 0x0106, 2);
        r = replace_preview(&t, d0, &keep, 1);
    }
    if (!r && pv) {
        tiff_set_long(&t, pv, 0x00FE, 1);
        tiff_set_long(&t, pv, 0x0100, pw);
        tiff_set_long(&t, pv, 0x0101, ph);
        tiff_set_short(&t, pv, 0x0106, 6);
        r = replace_preview(&t, pv, &keep, 0);
        set_preview_info(&t, pv);
    }
    if (!r) {
        TiffIfd *list[2]; uint32_t n = 0;
        if (thumb) list[n++] = ri;
        if (pv) list[n++] = pv;
        if (n) tiff_set_subs(&t, d0, 0x014A, list, n);
        t.ifd0 = d0;
        buf_free(&S.out);
        r = tiff_write(&t, &S.out);
    }
    keep_free(&keep);
    buf_free(&rawdata);
    tiff_free(&t);
    return r;
}

int engine_export_dng(void) {
    if (!S.raw.data) return ERR_STATE;
    return S.raw.kind == RAW_DNG ? export_from_dng() : export_from_nef();
}

/* ---- JPEG ---- */
static int build_exif(Buf *out) {
    Tiff t;
    int r = tiff_parse(&t, S.file, S.len);
    if (r) return r;
    TiffIfd *src = t.ifd0, *d0 = tiff_new_ifd(&t);
    if (!d0) { tiff_free(&t); return ERR_MEMORY; }
    static const uint16_t keep0[] = {0x010F, 0x0110, 0x011A, 0x011B, 0x0128, 0x0132, 0x013B, 0x8298, 0x8825};
    copy_tags(&t, d0, src, keep0, sizeof keep0 / sizeof keep0[0]);
    tiff_set_short(&t, d0, 0x0112, (uint16_t)CLAMP(S.raw.orientation, 1, 8));
    tiff_set_ascii(&t, d0, 0x0131, APP_NAME " " APP_VERSION);
    TiffIfd *exif = copy_exif(&t, src);
    if (exif) {
        static const uint16_t drop[] = {0xA302, 0x9286, 0xA005, 0x0201, 0x0202};
        for (uint32_t i = 0; i < sizeof drop / sizeof drop[0]; i++) tiff_remove(exif, drop[i]);
        tiff_set_short(&t, exif, 0xA001, 1);
        tiff_set_long(&t, exif, 0xA002, S.raw.crop_w);
        tiff_set_long(&t, exif, 0xA003, S.raw.crop_h);
        tiff_set_subs(&t, d0, 0x8769, &exif, 1);
    }
    t.ifd0 = d0;
    r = tiff_write(&t, out);
    /* EXIF must fit into one APP1 segment: drop large opaque entries. */
    while (!r && out->len > 65000 && exif) {
        uint32_t big = 0, at = 0;
        for (uint32_t i = 0; i < exif->n; i++) if (exif->e[i].size > big && !exif->e[i].nsub) { big = exif->e[i].size; at = i; }
        if (!big) break;
        exif->e[at] = exif->e[--exif->n];
        out->len = 0;
        r = tiff_write(&t, out);
    }
    tiff_free(&t);
    if (!r && out->len > 65000) out->len = 0;
    return r;
}
/* HDR JPEG: the SDR image as before, followed by a greyscale gain map at half
 * resolution (MPF container). The map stores log2(gain) of the HDR preview
 * (1x ... HDR_PEAK, scalar per pixel), signalled twice: ISO 21496-1 (Apple,
 * Chrome) and Adobe/Google Ultra HDR XMP (Android). Offsets are zero, so the
 * HDR rendition is exactly SDR (linear) x gain. */
#define GAINMAP_QUALITY 85
#define LOG2_PEAK_N 15849625u            /* log2(HDR_PEAK) = 1.5849625 */
#define LOG2_PEAK_D 10000000u
#define LOG2_PEAK_S "1.5849625"
static const char xmp_id[] = "http://ns.adobe.com/xap/1.0/";              /* + NUL */
static const char iso_id[] = "urn:iso:std:iso:ts:21496:-1";               /* + NUL */
static const char xpacket_begin[] = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
static const char xpacket_end[] = "<?xpacket end=\"w\"?>";
static uint32_t put_text(char *d, const char *s) { uint32_t n = 0; while (s[n]) { if (d) d[n] = s[n]; n++; } return n; }
static uint32_t put_dec(char *d, uint32_t v) {
    char t[10]; uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) if (d) d[i] = t[n - 1 - i];
    return n;
}
/* Primary image XMP (Ultra HDR container directory), padded to `size`
 * bytes; returns the unpadded size (size 0: just measure). */
static uint32_t primary_xmp(char *d, uint32_t size, uint32_t gainmap_len) {
    uint32_t n = put_text(d, xpacket_begin);
    n += put_text(d ? d + n : NULL,
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"" APP_NAME "\">\n"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "<rdf:Description rdf:about=\"\" xmlns:Container=\"http://ns.google.com/photos/1.0/container/\"\n"
        " xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\"\n"
        " xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\">\n"
        "<Container:Directory><rdf:Seq>\n"
        "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"Primary\" Item:Mime=\"image/jpeg\"/></rdf:li>\n"
        "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"GainMap\" Item:Mime=\"image/jpeg\" Item:Length=\"");
    n += put_dec(d ? d + n : NULL, gainmap_len);
    n += put_text(d ? d + n : NULL, "\"/></rdf:li>\n</rdf:Seq></Container:Directory>\n</rdf:Description>\n</rdf:RDF>\n</x:xmpmeta>\n");
    uint32_t used = n + (uint32_t)sizeof xpacket_end - 1;
    if (d && size >= used) {
        for (uint32_t i = n; i < size - (sizeof xpacket_end - 1); i++) d[i] = ' ';
        put_text(d + size - (sizeof xpacket_end - 1), xpacket_end);
    }
    return used;
}
static const char gainmap_xmp[] =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"" APP_NAME "\">\n"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "<rdf:Description rdf:about=\"\" xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n"
    " hdrgm:Version=\"1.0\" hdrgm:GainMapMin=\"0\" hdrgm:GainMapMax=\"" LOG2_PEAK_S "\" hdrgm:Gamma=\"1\"\n"
    " hdrgm:OffsetSDR=\"0\" hdrgm:OffsetHDR=\"0\" hdrgm:HDRCapacityMin=\"0\" hdrgm:HDRCapacityMax=\"" LOG2_PEAK_S "\"\n"
    " hdrgm:BaseRenditionIsHDR=\"False\"/>\n"
    "</rdf:RDF>\n</x:xmpmeta>\n";
static uint8_t *put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return p + 4; }
static uint8_t *put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return p + 2; }
/* ISO 21496-1 GainMapMetadata: one channel, base colour space. */
static uint32_t iso_metadata(uint8_t *p0) {
    static const uint32_t v[] = {0, 1, LOG2_PEAK_N, LOG2_PEAK_D,          /* base / alternate headroom */
                                 0, 1, LOG2_PEAK_N, LOG2_PEAK_D, 1, 1, 0, 1, 0, 1};  /* min, max, gamma, offsets */
    uint8_t *p = put16(put16(p0, 0), 0);   /* minimum_version, writer_version */
    *p++ = 0x40;                           /* single channel, use_base_colour_space */
    for (uint32_t i = 0; i < sizeof v / sizeof v[0]; i++) p = put32be(p, v[i]);
    return (uint32_t)(p - p0);
}
/* MPF (CIPA DC-007) index of the two images; offsets relative to the TIFF
 * header that follows "MPF\0". */
#define MPF_SIZE 82
static void mpf_index(uint8_t *p, uint32_t primary_len, uint32_t gainmap_len, uint32_t gainmap_offset) {
    xmemset(p, 0, MPF_SIZE);
    p[0] = 'M'; p[1] = 'M'; put16(p + 2, 42); put32be(p + 4, 8);
    uint8_t *e = put16(p + 8, 3);
    e = put16(e, 0xB000); e = put16(e, 7); e = put32be(e, 4); e[0] = '0'; e[1] = '1'; e[2] = '0'; e[3] = '0'; e += 4;
    e = put16(e, 0xB001); e = put16(e, 4); e = put32be(e, 1); e = put32be(e, 2);
    e = put16(e, 0xB002); e = put16(e, 7); e = put32be(e, 32); e = put32be(e, 50);
    e = put32be(e, 0);                                   /* no next IFD */
    e = put32be(e, 0x030000); e = put32be(e, primary_len); e = put32be(e, 0); e += 4;   /* baseline primary */
    e = put32be(e, 0); e = put32be(e, gainmap_len); e = put32be(e, gainmap_offset);
}
int engine_export_jpeg(void) {
    if (!S.raw.data) return ERR_STATE;
    Buf exif = {0}, app = {0};
    build_exif(&exif);
    Jpeg j;
    Full f = {0};
    uint32_t w = S.raw.crop_w, h = S.raw.crop_h, gw = (w + 1) / 2, gh = (h + 1) / 2;
    int r = jpeg_init(&j, w, h, JPEG_QUALITY, 0);
    uint8_t *rows = xalloc((size_t)w * 16 * 3), *codes = xalloc((size_t)w * 16), *gm = xalloc((size_t)gw * gh);
    char *xmp = NULL;
    if (!r && (!rows || !codes || !gm)) r = ERR_MEMORY;
    if (!r) r = full_init(&f, &S.lin, &S.rp);
    if (!r) {
        /* Huffman statistics from every fourth MCU row, then encoding. */
        for (uint32_t y = 0; y < h; y += 64) {
            uint32_t n = MIN(16, h - y);
            full_rows(&f, y, n, rows, NULL);
            mcu_row(&j, rows, n, 0);
        }
        jpeg_tables(&j);
        buf_free(&S.out);
        Buf *o = &S.out;
        jpeg_start(o, 0);
        if (exif.len) jpeg_app(o, 1, "Exif\0", 6, exif.data, exif.len);
        uint32_t xmp_size = primary_xmp(NULL, 0, 0xFFFFFFFFu) + 16, xmp_at = o->len + 4 + sizeof xmp_id;
        xmp = xalloc(xmp_size);
        if (!xmp) r = ERR_MEMORY;
        else {
            primary_xmp(xmp, xmp_size, 0);
            jpeg_app(o, 1, xmp_id, sizeof xmp_id, xmp, xmp_size);
        }
        jpeg_app(o, 2, "ICC_PROFILE\0\1\1", 14, srgb_icc, srgb_icc_len);
        static const uint8_t iso_version[4] = {0, 0, 0, 0};
        jpeg_app(o, 2, iso_id, sizeof iso_id, iso_version, 4);
        uint8_t mpf[MPF_SIZE] = {0};
        uint32_t mpf_at = o->len + 4 + 4;
        jpeg_app(o, 2, "MPF", 4, mpf, MPF_SIZE);
        jpeg_frame(&j, o);
        for (uint32_t y = 0; y < h; y += 16) {
            uint32_t n = MIN(16, h - y);
            full_rows(&f, y, n, rows, codes);
            mcu_row(&j, rows, n, 1);
            /* 2x2 average of the gain codes (y is even). */
            for (uint32_t k = 0; k < n; k += 2) {
                const uint8_t *a = codes + (size_t)k * w, *b = k + 1 < n ? a + w : a;
                uint8_t *g = gm + (size_t)(y + k) / 2 * gw;
                for (uint32_t x = 0; x < gw; x++) {
                    uint32_t x1 = MIN(2 * x + 1, w - 1);
                    g[x] = (uint8_t)((a[2 * x] + a[x1] + b[2 * x] + b[x1] + 2) / 4);
                }
            }
        }
        jpeg_finish(&j, o);
        uint32_t primary_len = o->len;
        uint8_t iso[64];
        jpeg_app(&app, 1, xmp_id, sizeof xmp_id, gainmap_xmp, sizeof gainmap_xmp - 1);
        jpeg_app(&app, 2, iso_id, sizeof iso_id, iso, iso_metadata(iso));
        if (!r && app.failed) r = ERR_MEMORY;
        if (!r) r = jpeg_encode_gray(o, gm, gw, gh, GAINMAP_QUALITY, &app);
        if (!r && o->failed) r = ERR_MEMORY;
        if (!r) {
            uint32_t gainmap_len = o->len - primary_len;
            primary_xmp(xmp, xmp_size, gainmap_len);
            xmemcpy(o->data + xmp_at, xmp, xmp_size);
            mpf_index(o->data + mpf_at, primary_len, gainmap_len, primary_len - mpf_at);
        }
    }
    full_free(&f); xfree(rows); xfree(codes); xfree(gm); xfree(xmp); jpeg_free(&j); buf_free(&exif); buf_free(&app);
    return r;
}
