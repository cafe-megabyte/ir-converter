#ifndef IRCONV_TIFF_H
#define IRCONV_TIFF_H
#include "base.h"

enum { T_BYTE = 1, T_ASCII, T_SHORT, T_LONG, T_RATIONAL, T_SBYTE, T_UNDEFINED, T_SSHORT,
       T_SLONG, T_SRATIONAL, T_FLOAT, T_DOUBLE, T_IFD };

typedef struct TiffIfd TiffIfd;
typedef struct { const uint8_t *ptr; uint32_t len; } Blob;
typedef struct {
    uint16_t tag, type;
    uint32_t count;
    const uint8_t *data;      /* value bytes in the container byte order */
    uint32_t size;
    TiffIfd **sub; uint32_t nsub;   /* IFD pointer tags (SubIFDs, Exif, GPS, Interop) */
    Blob *blobs; uint32_t nblobs;   /* data offset tags (strips, tiles, JPEG) */
} TiffEntry;
struct TiffIfd { TiffEntry *e; uint32_t n, cap; TiffIfd *next; };

typedef struct ArenaChunk { struct ArenaChunk *next; uint32_t used, cap; } ArenaChunk;
typedef struct {
    int be;                      /* 1 = big-endian ("MM") */
    const uint8_t *base; uint32_t len;
    TiffIfd *ifd0;
    ArenaChunk *chunks;
    int failed;                  /* allocation failure while editing */
    uint32_t ifd_budget;         /* guards against cyclic files */
} Tiff;

int tiff_parse(Tiff *t, const uint8_t *data, uint32_t len);
void tiff_free(Tiff *t);
void *tiff_alloc(Tiff *t, uint32_t size);

uint16_t rd16(const uint8_t *p, int be);
uint32_t rd32(const uint8_t *p, int be);
void wr16(uint8_t *p, uint16_t v, int be);
void wr32(uint8_t *p, uint32_t v, int be);

TiffEntry *tiff_find(const TiffIfd *ifd, uint16_t tag);
int tiff_type_size(uint16_t type);
uint32_t tiff_uint(const Tiff *t, const TiffEntry *e, uint32_t i);
double tiff_num(const Tiff *t, const TiffEntry *e, uint32_t i);
uint32_t tiff_get_uint(const Tiff *t, const TiffIfd *ifd, uint16_t tag, uint32_t def);
double tiff_get_num(const Tiff *t, const TiffIfd *ifd, uint16_t tag, uint32_t i, double def);
int tiff_get_nums(const Tiff *t, const TiffIfd *ifd, uint16_t tag, double *out, uint32_t n);
void tiff_get_str(const TiffIfd *ifd, uint16_t tag, char *out, uint32_t n);

TiffIfd *tiff_new_ifd(Tiff *t);
TiffEntry *tiff_set_raw(Tiff *t, TiffIfd *ifd, uint16_t tag, uint16_t type, uint32_t count, const void *bytes);
void tiff_set_u16(Tiff *t, TiffIfd *ifd, uint16_t tag, const uint16_t *v, uint32_t n);
void tiff_set_u32(Tiff *t, TiffIfd *ifd, uint16_t tag, const uint32_t *v, uint32_t n);
void tiff_set_long(Tiff *t, TiffIfd *ifd, uint16_t tag, uint32_t v);
void tiff_set_short(Tiff *t, TiffIfd *ifd, uint16_t tag, uint16_t v);
void tiff_set_rational(Tiff *t, TiffIfd *ifd, uint16_t tag, const double *v, uint32_t n);
void tiff_set_srational(Tiff *t, TiffIfd *ifd, uint16_t tag, const double *v, uint32_t n);
void tiff_set_ascii(Tiff *t, TiffIfd *ifd, uint16_t tag, const char *s);
void tiff_set_subs(Tiff *t, TiffIfd *ifd, uint16_t tag, TiffIfd **subs, uint32_t n);
void tiff_set_blobs(Tiff *t, TiffIfd *ifd, uint16_t offtag, uint16_t cnttag, const Blob *b, uint32_t n);
void tiff_copy_entry(Tiff *t, TiffIfd *ifd, const TiffEntry *e);
void tiff_remove(TiffIfd *ifd, uint16_t tag);

/* Serialises t->ifd0 (and its chain) as a complete TIFF file in t->be order. */
int tiff_write(Tiff *t, Buf *out);
#endif
