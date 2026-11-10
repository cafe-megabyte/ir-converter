/* Shared definitions for the IR Converter core. The core is freestanding C11:
 * it compiles unchanged for wasm32 (no libc) and natively for the checks. */
#ifndef IRCONV_BASE_H
#define IRCONV_BASE_H
#include <stdint.h>
#include <stddef.h>

enum {
    ERR_OK = 0,
    ERR_MEMORY = 1,        /* allocation failed */
    ERR_FORMAT = 2,        /* not a TIFF-based raw file / corrupt structure */
    ERR_UNSUPPORTED = 3,   /* valid file, unsupported variant (see detail) */
    ERR_CORRUPT = 4,       /* compressed data damaged or truncated */
    ERR_STATE = 5          /* API used without an open image */
};
/* Detail codes for ERR_UNSUPPORTED, shown to the user by the web app. */
enum {
    UNSUP_NONE = 0,
    UNSUP_COMPRESSION = 1, /* e.g. lossy/JPEG XL DNG or Nikon HE/HE* */
    UNSUP_LINEAR_RAW = 2,  /* demosaiced (linear) DNG */
    UNSUP_CFA = 3,         /* not a 2x2 RGB Bayer pattern */
    UNSUP_LAYOUT = 4,      /* planar, multi-sample or unusual bit depth */
    UNSUP_NO_RAW = 5       /* no raw image found */
};

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

void *xalloc(size_t size);           /* uninitialised; NULL on failure */
void *xcalloc(size_t size);          /* zeroed */
void *xrealloc(void *p, size_t size);
void xfree(void *p);
void xmemcpy(void *d, const void *s, size_t n);
void xmemmove(void *d, const void *s, size_t n);
void xmemset(void *d, int c, size_t n);
int xmemcmp(const void *a, const void *b, size_t n);
size_t xstrlen(const char *s);

double m_exp(double x);
double m_expm1(double x);
double m_log(double x);
double m_pow(double x, double y);
double m_floor(double x);
static inline double m_sqrt(double x) { return __builtin_sqrt(x); }
static inline double m_abs(double x) { return x < 0 ? -x : x; }

/* A growable byte buffer used by all writers. */
typedef struct { uint8_t *data; uint32_t len, cap; int failed; } Buf;
void buf_reserve(Buf *b, uint32_t extra);
void buf_put(Buf *b, const void *p, uint32_t n);
void buf_byte(Buf *b, uint8_t v);
void buf_zero(Buf *b, uint32_t n);
void buf_free(Buf *b);
#endif
