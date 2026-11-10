/* Memory and byte helpers. On wasm32 a small boundary-tag allocator manages
 * the linear memory above the initial pages; native checks use libc. */
#include "base.h"

void xmemcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    while (n >= 8) { a[0]=b[0]; a[1]=b[1]; a[2]=b[2]; a[3]=b[3]; a[4]=b[4]; a[5]=b[5]; a[6]=b[6]; a[7]=b[7]; a += 8; b += 8; n -= 8; }
    while (n--) *a++ = *b++;
}
void xmemmove(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a == b || !n) return;
    if (a < b || a >= b + n) { xmemcpy(d, s, n); return; }
    while (n--) a[n] = b[n];
}
void xmemset(void *d, int c, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; }
int xmemcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
size_t xstrlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

#ifdef IRCONV_NATIVE
#include <stdlib.h>
void *xalloc(size_t size) { return malloc(size ? size : 1); }
void *xrealloc(void *p, size_t size) { return realloc(p, size ? size : 1); }
void xfree(void *p) { free(p); }
#else
/* Block layout: [size|used][prev_size] payload... Sizes include the 8-byte
 * header and are multiples of 8. The heap starts above the memory that the
 * linker reserved for data and stack, so no linker symbol is required. */
#define HDR 8u
static uint8_t *heap_start, *heap_top;
static uint32_t *hdr(uint8_t *b) { return (uint32_t *)b; }
static int heap_init(void) {
    if (heap_start) return 1;
    heap_start = heap_top = (uint8_t *)(uintptr_t)(__builtin_wasm_memory_size(0) * 65536u);
    return 1;
}
static int heap_extend(uint32_t bytes) {
    uintptr_t need = (uintptr_t)heap_top + bytes;
    uintptr_t have = __builtin_wasm_memory_size(0) * 65536u;
    if (need < (uintptr_t)heap_top) return 0;
    if (need > have) {
        uintptr_t pages = (need - have + 65535u) / 65536u;
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return 0;
    }
    return 1;
}
void *xalloc(size_t size) {
    heap_init();
    if (size > 0xF0000000u) return NULL;
    uint32_t need = (uint32_t)((size + HDR + 7u) & ~7u);
    if (need < 16) need = 16;
    uint8_t *b = heap_start, *last = NULL;
    while (b < heap_top) {
        uint32_t s = hdr(b)[0] & ~7u;
        if (!(hdr(b)[0] & 1u) && s >= need) {
            if (s - need >= 64) {
                uint8_t *rest = b + need;
                hdr(rest)[0] = s - need; hdr(rest)[1] = need;
                if (rest + (s - need) < heap_top) hdr(rest + (s - need))[1] = s - need;
                s = need;
            }
            hdr(b)[0] = s | 1u;
            return b + HDR;
        }
        last = b; b += s;
    }
    if (last && !(hdr(last)[0] & 1u)) {   /* grow the free block at the top */
        uint32_t s = hdr(last)[0];
        if (!heap_extend(need - s)) return NULL;
        heap_top += need - s;
        hdr(last)[0] = need | 1u;
        return last + HDR;
    }
    if (!heap_extend(need)) return NULL;
    b = heap_top; heap_top += need;
    hdr(b)[0] = need | 1u; hdr(b)[1] = last ? (hdr(last)[0] & ~7u) : 0;
    return b + HDR;
}
void xfree(void *p) {
    if (!p) return;
    uint8_t *b = (uint8_t *)p - HDR;
    uint32_t s = hdr(b)[0] & ~7u;
    uint8_t *next = b + s;
    if (next < heap_top && !(hdr(next)[0] & 1u)) s += hdr(next)[0];
    if (b > heap_start) {
        uint8_t *prev = b - hdr(b)[1];
        if (!(hdr(prev)[0] & 1u)) { s += hdr(prev)[0]; b = prev; }
    }
    hdr(b)[0] = s;
    if (b + s < heap_top) hdr(b + s)[1] = s;
}
void *xrealloc(void *p, size_t size) {
    if (!p) return xalloc(size);
    uint8_t *b = (uint8_t *)p - HDR;
    uint32_t have = (hdr(b)[0] & ~7u) - HDR;
    if (size <= have) return p;
    void *q = xalloc(size);
    if (!q) return NULL;
    xmemcpy(q, p, have);
    xfree(p);
    return q;
}
/* Clang may lower struct copies to these symbols. */
void *memcpy(void *d, const void *s, size_t n) { xmemcpy(d, s, n); return d; }
void *memmove(void *d, const void *s, size_t n) { xmemmove(d, s, n); return d; }
void *memset(void *d, int c, size_t n) { xmemset(d, c, n); return d; }
#endif

void *xcalloc(size_t size) { void *p = xalloc(size); if (p) xmemset(p, 0, size); return p; }

void buf_reserve(Buf *b, uint32_t extra) {
    if (b->failed) return;
    if ((uint64_t)b->len + extra > 0xF0000000u) { b->failed = 1; return; }
    if (b->len + extra <= b->cap) return;
    uint32_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) cap = cap < 0x40000000u ? cap * 2 : b->len + extra;
    uint8_t *d = xrealloc(b->data, cap);
    if (!d) { b->failed = 1; return; }
    b->data = d; b->cap = cap;
}
void buf_put(Buf *b, const void *p, uint32_t n) {
    buf_reserve(b, n);
    if (b->failed) return;
    xmemcpy(b->data + b->len, p, n); b->len += n;
}
void buf_byte(Buf *b, uint8_t v) {
    if (b->len < b->cap) { b->data[b->len++] = v; return; }
    buf_put(b, &v, 1);
}
void buf_zero(Buf *b, uint32_t n) {
    buf_reserve(b, n);
    if (b->failed) return;
    xmemset(b->data + b->len, 0, n); b->len += n;
}
void buf_free(Buf *b) { xfree(b->data); b->data = NULL; b->len = b->cap = 0; b->failed = 0; }
