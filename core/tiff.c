/* TIFF/EP container: bounds-checked parser and a generic re-writer. All tag
 * values stay in the container's byte order, so unknown tags, MakerNotes and
 * DNG private data survive unchanged; only offsets are recomputed. */
#include "tiff.h"

uint16_t rd16(const uint8_t *p, int be) { return be ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]); }
uint32_t rd32(const uint8_t *p, int be) {
    return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
              : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
void wr16(uint8_t *p, uint16_t v, int be) { if (be) { p[0] = v >> 8; p[1] = (uint8_t)v; } else { p[0] = (uint8_t)v; p[1] = v >> 8; } }
void wr32(uint8_t *p, uint32_t v, int be) {
    if (be) { p[0] = v >> 24; p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
    else { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = v >> 24; }
}

void *tiff_alloc(Tiff *t, uint32_t size) {
    size = (size + 7u) & ~7u;
    ArenaChunk *c = t->chunks;
    if (!c || c->cap - c->used < size) {
        uint32_t cap = size > 65536 - sizeof(ArenaChunk) ? size : 65536 - (uint32_t)sizeof(ArenaChunk);
        c = xalloc(sizeof(ArenaChunk) + cap);
        if (!c) { t->failed = 1; return NULL; }
        c->used = 0; c->cap = cap;
        if (t->chunks && size >= 16384) { c->next = t->chunks->next; t->chunks->next = c; }  /* keep small-chunk head */
        else { c->next = t->chunks; t->chunks = c; }
    }
    void *p = (uint8_t *)(c + 1) + c->used;
    c->used += size;
    return p;
}
void tiff_free(Tiff *t) {
    ArenaChunk *c = t->chunks;
    while (c) { ArenaChunk *n = c->next; xfree(c); c = n; }
    t->chunks = NULL; t->ifd0 = NULL;
}

int tiff_type_size(uint16_t type) {
    switch (type) {
    case T_BYTE: case T_ASCII: case T_SBYTE: case T_UNDEFINED: return 1;
    case T_SHORT: case T_SSHORT: return 2;
    case T_LONG: case T_SLONG: case T_FLOAT: case T_IFD: return 4;
    case T_RATIONAL: case T_SRATIONAL: case T_DOUBLE: return 8;
    default: return 0;
    }
}

TiffIfd *tiff_new_ifd(Tiff *t) {
    TiffIfd *ifd = tiff_alloc(t, sizeof(TiffIfd));
    if (ifd) xmemset(ifd, 0, sizeof(*ifd));
    return ifd;
}
static TiffEntry *add_entry(Tiff *t, TiffIfd *ifd) {
    if (ifd->n == ifd->cap) {
        uint32_t cap = ifd->cap ? ifd->cap * 2 : 32;
        TiffEntry *e = tiff_alloc(t, cap * sizeof(TiffEntry));
        if (!e) return NULL;
        if (ifd->n) xmemcpy(e, ifd->e, ifd->n * sizeof(TiffEntry));
        ifd->e = e; ifd->cap = cap;
    }
    TiffEntry *e = &ifd->e[ifd->n++];
    xmemset(e, 0, sizeof(*e));
    return e;
}
static int is_ifd_tag(uint16_t tag) { return tag == 0x014A || tag == 0x8769 || tag == 0x8825 || tag == 0xA005; }
static uint16_t count_tag_for(uint16_t tag) {
    return tag == 0x0111 ? 0x0117 : tag == 0x0144 ? 0x0145 : tag == 0x0201 ? 0x0202 : 0;
}

static TiffIfd *parse_ifd(Tiff *t, uint32_t off, int depth);
static int parse_chain(Tiff *t, uint32_t off, int depth, TiffIfd **out) {
    *out = NULL;
    TiffIfd **link = out;
    for (int guard = 0; off && guard < 16; guard++) {
        TiffIfd *ifd = parse_ifd(t, off, depth);
        if (!ifd) return t->failed ? ERR_MEMORY : ERR_FORMAT;
        *link = ifd; link = &ifd->next;
        uint32_t n = rd16(t->base + off, t->be);
        off = rd32(t->base + off + 2 + 12 * n, t->be);
        if (depth > 0) break;   /* sub-IFD chains are not followed */
    }
    return ERR_OK;
}
static TiffIfd *parse_ifd(Tiff *t, uint32_t off, int depth) {
    if (depth > 4 || !t->ifd_budget--) return NULL;
    if (off & 1 && off + 1 < t->len) { /* tolerate odd offsets */ }
    if (off > t->len || t->len - off < 2) return NULL;
    uint32_t n = rd16(t->base + off, t->be);
    if (!n || n > 2000 || (uint64_t)off + 2 + 12ull * n + 4 > t->len) return NULL;
    TiffIfd *ifd = tiff_new_ifd(t);
    if (!ifd) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = t->base + off + 2 + 12 * i;
        uint16_t type = rd16(p + 2, t->be);
        uint32_t count = rd32(p + 4, t->be);
        int ts = tiff_type_size(type);
        if (!ts) continue;                          /* unknown type: drop */
        uint64_t size = (uint64_t)count * ts;
        const uint8_t *data;
        if (size <= 4) data = p + 8;
        else {
            uint32_t vo = rd32(p + 8, t->be);
            if (vo > t->len || size > t->len - vo) continue;   /* broken value: drop */
            data = t->base + vo;
        }
        TiffEntry *e = add_entry(t, ifd);
        if (!e) return NULL;
        e->tag = rd16(p, t->be); e->type = type; e->count = count; e->data = data; e->size = (uint32_t)size;
    }
    /* Resolve sub-IFDs and data blocks. */
    for (uint32_t i = 0; i < ifd->n; i++) {
        TiffEntry *e = &ifd->e[i];
        if (is_ifd_tag(e->tag) && (e->type == T_LONG || e->type == T_IFD) && e->count && e->count <= 16) {
            e->sub = tiff_alloc(t, e->count * sizeof(TiffIfd *));
            if (!e->sub) return NULL;
            for (uint32_t k = 0; k < e->count; k++) {
                TiffIfd *s;
                if (parse_chain(t, rd32(e->data + 4 * k, t->be), depth + 1, &s) == ERR_OK && s) e->sub[e->nsub++] = s;
            }
            if (!e->nsub) { e->sub = NULL; }
        }
        uint16_t ct = count_tag_for(e->tag);
        TiffEntry *c = ct ? tiff_find(ifd, ct) : NULL;
        if (c && c->count == e->count && e->count && e->count < 1000000) {
            e->blobs = tiff_alloc(t, e->count * sizeof(Blob));
            if (!e->blobs) return NULL;
            e->nblobs = e->count;
            for (uint32_t k = 0; k < e->count; k++) {
                uint32_t o = tiff_uint(t, e, k), l = tiff_uint(t, c, k);
                if (o > t->len) o = t->len;
                if (l > t->len - o) l = t->len - o;
                e->blobs[k].ptr = t->base + o; e->blobs[k].len = l;
            }
        }
    }
    return ifd;
}
int tiff_parse(Tiff *t, const uint8_t *data, uint32_t len) {
    xmemset(t, 0, sizeof(*t));
    if (len < 8) return ERR_FORMAT;
    if (data[0] == 'I' && data[1] == 'I') t->be = 0;
    else if (data[0] == 'M' && data[1] == 'M') t->be = 1;
    else return ERR_FORMAT;
    if (rd16(data + 2, t->be) != 42) return ERR_FORMAT;
    t->base = data; t->len = len; t->ifd_budget = 256;
    int r = parse_chain(t, rd32(data + 4, t->be), 0, &t->ifd0);
    if (r == ERR_OK && !t->ifd0) r = ERR_FORMAT;
    if (r != ERR_OK) tiff_free(t);
    return r;
}

TiffEntry *tiff_find(const TiffIfd *ifd, uint16_t tag) {
    if (!ifd) return NULL;
    for (uint32_t i = 0; i < ifd->n; i++) if (ifd->e[i].tag == tag) return &ifd->e[i];
    return NULL;
}
uint32_t tiff_uint(const Tiff *t, const TiffEntry *e, uint32_t i) {
    double v = tiff_num(t, e, i);
    return v <= 0 ? 0 : v >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)(v + 0.5);
}
double tiff_num(const Tiff *t, const TiffEntry *e, uint32_t i) {
    if (!e || i >= e->count) return 0;
    const uint8_t *p = e->data + (uint32_t)tiff_type_size(e->type) * i;
    switch (e->type) {
    case T_BYTE: case T_UNDEFINED: case T_ASCII: return p[0];
    case T_SBYTE: return (int8_t)p[0];
    case T_SHORT: return rd16(p, t->be);
    case T_SSHORT: return (int16_t)rd16(p, t->be);
    case T_LONG: case T_IFD: return rd32(p, t->be);
    case T_SLONG: return (int32_t)rd32(p, t->be);
    case T_RATIONAL: { uint32_t d = rd32(p + 4, t->be); return d ? (double)rd32(p, t->be) / d : 0; }
    case T_SRATIONAL: { int32_t d = (int32_t)rd32(p + 4, t->be); return d ? (double)(int32_t)rd32(p, t->be) / d : 0; }
    case T_FLOAT: { union { uint32_t u; float f; } v = {rd32(p, t->be)}; return v.f; }
    case T_DOUBLE: {
        union { uint64_t u; double d; } v;
        uint64_t hi = rd32(p, t->be), lo = rd32(p + 4, t->be);
        v.u = t->be ? hi << 32 | lo : lo << 32 | hi;
        return v.d;
    }
    }
    return 0;
}
uint32_t tiff_get_uint(const Tiff *t, const TiffIfd *ifd, uint16_t tag, uint32_t def) {
    TiffEntry *e = tiff_find(ifd, tag);
    return e && e->count ? tiff_uint(t, e, 0) : def;
}
double tiff_get_num(const Tiff *t, const TiffIfd *ifd, uint16_t tag, uint32_t i, double def) {
    TiffEntry *e = tiff_find(ifd, tag);
    return e && i < e->count ? tiff_num(t, e, i) : def;
}
int tiff_get_nums(const Tiff *t, const TiffIfd *ifd, uint16_t tag, double *out, uint32_t n) {
    TiffEntry *e = tiff_find(ifd, tag);
    if (!e || e->count < n) return 0;
    for (uint32_t i = 0; i < n; i++) out[i] = tiff_num(t, e, i);
    return 1;
}
void tiff_get_str(const TiffIfd *ifd, uint16_t tag, char *out, uint32_t n) {
    TiffEntry *e = tiff_find(ifd, tag);
    uint32_t k = 0;
    if (e && (e->type == T_ASCII || e->type == T_BYTE || e->type == T_UNDEFINED))
        for (; k + 1 < n && k < e->size && e->data[k]; k++) out[k] = (char)e->data[k];
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static TiffEntry *slot(Tiff *t, TiffIfd *ifd, uint16_t tag) {
    TiffEntry *e = tiff_find(ifd, tag);
    if (e) { xmemset(e, 0, sizeof(*e)); e->tag = tag; return e; }
    e = add_entry(t, ifd);
    if (e) e->tag = tag;
    return e;
}
TiffEntry *tiff_set_raw(Tiff *t, TiffIfd *ifd, uint16_t tag, uint16_t type, uint32_t count, const void *bytes) {
    uint32_t size = count * (uint32_t)tiff_type_size(type);
    uint8_t *copy = tiff_alloc(t, size ? size : 1);
    TiffEntry *e = copy ? slot(t, ifd, tag) : NULL;
    if (!e) { t->failed = 1; return NULL; }
    if (size) xmemcpy(copy, bytes, size);
    e->type = type; e->count = count; e->data = copy; e->size = size;
    return e;
}
static uint8_t *set_new(Tiff *t, TiffIfd *ifd, uint16_t tag, uint16_t type, uint32_t count) {
    uint32_t size = count * (uint32_t)tiff_type_size(type);
    uint8_t *data = tiff_alloc(t, size ? size : 1);
    TiffEntry *e = data ? slot(t, ifd, tag) : NULL;
    if (!e) { t->failed = 1; return NULL; }
    e->type = type; e->count = count; e->data = data; e->size = size;
    return data;
}
void tiff_set_u16(Tiff *t, TiffIfd *ifd, uint16_t tag, const uint16_t *v, uint32_t n) {
    uint8_t *d = set_new(t, ifd, tag, T_SHORT, n);
    if (d) for (uint32_t i = 0; i < n; i++) wr16(d + 2 * i, v[i], t->be);
}
void tiff_set_u32(Tiff *t, TiffIfd *ifd, uint16_t tag, const uint32_t *v, uint32_t n) {
    uint8_t *d = set_new(t, ifd, tag, T_LONG, n);
    if (d) for (uint32_t i = 0; i < n; i++) wr32(d + 4 * i, v[i], t->be);
}
void tiff_set_long(Tiff *t, TiffIfd *ifd, uint16_t tag, uint32_t v) { tiff_set_u32(t, ifd, tag, &v, 1); }
void tiff_set_short(Tiff *t, TiffIfd *ifd, uint16_t tag, uint16_t v) { tiff_set_u16(t, ifd, tag, &v, 1); }
void tiff_set_rational(Tiff *t, TiffIfd *ifd, uint16_t tag, const double *v, uint32_t n) {
    uint8_t *d = set_new(t, ifd, tag, T_RATIONAL, n);
    if (!d) return;
    for (uint32_t i = 0; i < n; i++) {
        double x = v[i] < 0 ? 0 : v[i];
        uint32_t den = 1000000000u;
        while (den > 1 && x * den > 4294967295.0) den /= 10;
        wr32(d + 8 * i, (uint32_t)(x * den + 0.5), t->be); wr32(d + 8 * i + 4, den, t->be);
    }
}
void tiff_set_srational(Tiff *t, TiffIfd *ifd, uint16_t tag, const double *v, uint32_t n) {
    uint8_t *d = set_new(t, ifd, tag, T_SRATIONAL, n);
    if (!d) return;
    for (uint32_t i = 0; i < n; i++) {
        double x = v[i];
        int32_t den = 1000000;
        while (den > 1 && (x * den > 2147483647.0 || x * den < -2147483647.0)) den /= 10;
        double s = x * den;
        wr32(d + 8 * i, (uint32_t)(int32_t)(s < 0 ? s - 0.5 : s + 0.5), t->be); wr32(d + 8 * i + 4, (uint32_t)den, t->be);
    }
}
void tiff_set_ascii(Tiff *t, TiffIfd *ifd, uint16_t tag, const char *s) {
    tiff_set_raw(t, ifd, tag, T_ASCII, (uint32_t)xstrlen(s) + 1, s);
}
void tiff_set_subs(Tiff *t, TiffIfd *ifd, uint16_t tag, TiffIfd **subs, uint32_t n) {
    uint8_t *d = set_new(t, ifd, tag, T_LONG, n);
    TiffIfd **s = tiff_alloc(t, n * sizeof(TiffIfd *) + 1);
    TiffEntry *e = tiff_find(ifd, tag);
    if (!d || !s || !e) { t->failed = 1; return; }
    for (uint32_t i = 0; i < n; i++) s[i] = subs[i];
    e->sub = s; e->nsub = n;
}
void tiff_set_blobs(Tiff *t, TiffIfd *ifd, uint16_t offtag, uint16_t cnttag, const Blob *b, uint32_t n) {
    uint8_t *d = set_new(t, ifd, offtag, T_LONG, n);
    Blob *copy = tiff_alloc(t, n * sizeof(Blob) + 1);
    TiffEntry *e = tiff_find(ifd, offtag);
    if (!d || !copy || !e) { t->failed = 1; return; }
    for (uint32_t i = 0; i < n; i++) copy[i] = b[i];
    e->blobs = copy; e->nblobs = n;
    uint8_t *c = set_new(t, ifd, cnttag, T_LONG, n);
    if (c) for (uint32_t i = 0; i < n; i++) wr32(c + 4 * i, b[i].len, t->be);
}
void tiff_copy_entry(Tiff *t, TiffIfd *ifd, const TiffEntry *e) {
    TiffEntry *d = slot(t, ifd, e->tag);
    if (!d) { t->failed = 1; return; }
    *d = *e;
}
void tiff_remove(TiffIfd *ifd, uint16_t tag) {
    for (uint32_t i = 0; i < ifd->n; i++)
        if (ifd->e[i].tag == tag) { ifd->e[i] = ifd->e[--ifd->n]; return; }
}

/* ---- writer ---- */
typedef struct { TiffIfd **list; uint32_t n, cap; uint32_t *offset; } IfdList;
static int collect(Tiff *t, IfdList *l, TiffIfd *ifd) {
    for (; ifd; ifd = ifd->next) {
        for (uint32_t i = 0; i < l->n; i++) if (l->list[i] == ifd) return ERR_FORMAT;   /* shared/cyclic */
        if (l->n == l->cap) {
            uint32_t cap = l->cap ? l->cap * 2 : 16;
            TiffIfd **n = tiff_alloc(t, cap * sizeof(TiffIfd *));
            if (!n) return ERR_MEMORY;
            if (l->n) xmemcpy(n, l->list, l->n * sizeof(TiffIfd *));
            l->list = n; l->cap = cap;
        }
        l->list[l->n++] = ifd;
        /* TIFF requires ascending tags. */
        for (uint32_t i = 1; i < ifd->n; i++) {
            TiffEntry e = ifd->e[i]; uint32_t j = i;
            while (j > 0 && ifd->e[j - 1].tag > e.tag) { ifd->e[j] = ifd->e[j - 1]; j--; }
            ifd->e[j] = e;
        }
        for (uint32_t i = 0; i < ifd->n; i++)
            for (uint32_t k = 0; k < ifd->e[i].nsub; k++) {
                int r = collect(t, l, ifd->e[i].sub[k]);
                if (r) return r;
            }
    }
    return ERR_OK;
}
static uint32_t ifd_index(const IfdList *l, const TiffIfd *ifd) {
    for (uint32_t i = 0; i < l->n; i++) if (l->list[i] == ifd) return i;
    return 0;
}
static uint32_t value_size(const TiffEntry *e) {
    if (e->nsub) return e->nsub > 1 ? 4 * e->nsub : 0;
    if (e->nblobs) return e->nblobs > 1 ? 4 * e->nblobs : 0;
    return e->size > 4 ? (e->size + 1u) & ~1u : 0;
}
int tiff_write(Tiff *t, Buf *out) {
    if (t->failed) return ERR_MEMORY;
    IfdList l = {0};
    int r = collect(t, &l, t->ifd0);
    if (r) return r;
    l.offset = tiff_alloc(t, l.n * sizeof(uint32_t) + 4);
    if (!l.offset) return ERR_MEMORY;
    /* Layout: header, every IFD followed by its values, then all data blocks. */
    uint64_t pos = 8;
    for (uint32_t i = 0; i < l.n; i++) {
        TiffIfd *ifd = l.list[i];
        l.offset[i] = (uint32_t)pos;
        pos += 2 + 12ull * ifd->n + 4;
        for (uint32_t k = 0; k < ifd->n; k++) pos += value_size(&ifd->e[k]);
    }
    uint64_t data_start = (pos + 15) & ~15ull, total = data_start;
    for (uint32_t i = 0; i < l.n; i++)
        for (uint32_t k = 0; k < l.list[i]->n; k++)
            for (uint32_t b = 0; b < l.list[i]->e[k].nblobs; b++) total += (l.list[i]->e[k].blobs[b].len + 15u) & ~15u;
    if (total > 0xF0000000u) return ERR_MEMORY;
    uint32_t base = out->len;
    buf_zero(out, (uint32_t)total);
    if (out->failed) return ERR_MEMORY;
    uint8_t *o = out->data + base;
    int be = t->be;
    o[0] = o[1] = be ? 'M' : 'I'; wr16(o + 2, 42, be); wr32(o + 4, l.offset[0], be);
    uint32_t blob_pos = (uint32_t)data_start;
    for (uint32_t i = 0; i < l.n; i++) {
        TiffIfd *ifd = l.list[i];
        uint32_t p = l.offset[i], vpos = p + 2 + 12 * ifd->n + 4;
        wr16(o + p, (uint16_t)ifd->n, be);
        for (uint32_t k = 0; k < ifd->n; k++) {
            TiffEntry *e = &ifd->e[k];
            uint8_t *d = o + p + 2 + 12 * k;
            wr16(d, e->tag, be);
            uint32_t vs = value_size(e);
            uint8_t *val = vs ? o + vpos : d + 8;
            if (vs) wr32(d + 8, vpos, be);
            if (e->nsub) {
                wr16(d + 2, T_LONG, be); wr32(d + 4, e->nsub, be);
                for (uint32_t s = 0; s < e->nsub; s++) wr32(val + 4 * s, l.offset[ifd_index(&l, e->sub[s])], be);
            } else if (e->nblobs) {
                wr16(d + 2, T_LONG, be); wr32(d + 4, e->nblobs, be);
                for (uint32_t b = 0; b < e->nblobs; b++) {
                    wr32(val + 4 * b, blob_pos, be);
                    xmemcpy(o + blob_pos, e->blobs[b].ptr, e->blobs[b].len);
                    blob_pos += (e->blobs[b].len + 15u) & ~15u;
                }
            } else {
                wr16(d + 2, e->type, be); wr32(d + 4, e->count, be);
                xmemcpy(val, e->data, e->size);
            }
            vpos += vs;
        }
        /* Only the top-level chain links to a next IFD. */
        uint32_t next = 0;
        if (ifd->next) next = l.offset[ifd_index(&l, ifd->next)];
        wr32(o + p + 2 + 12 * ifd->n, next, be);
    }
    return ERR_OK;
}
