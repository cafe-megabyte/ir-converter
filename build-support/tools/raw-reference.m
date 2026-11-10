// Independent check with Apple's RAW engine (ImageIO): every exported DNG
// must open and render, and its colours must match our own rendering of the
// same settings (JPEG) within a coarse tolerance. JPEGs must carry sRGB and
// an HDR gain map: the MPF/Ultra HDR structure must be consistent, ImageIO
// must find the ISO 21496-1 map, and Core Image's HDR decode must equal the
// SDR image times a gain between 1 and 3.
// Usage: raw-reference DNG JPEG [DNG JPEG ...]
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#include <math.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
/* Finds an APPn segment whose payload starts with id; returns the payload. */
static const uint8_t *segment(NSData *d, NSUInteger start, int app, const char *id, NSUInteger *len) {
    const uint8_t *p = d.bytes; NSUInteger n = d.length, i = start + 2, idl = strlen(id) + 1;
    while (i + 4 <= n && p[i] == 0xFF && p[i + 1] != 0xDA) {
        NSUInteger l = (NSUInteger)p[i + 2] << 8 | p[i + 3];
        if (p[i + 1] == 0xE0 + app && l - 2 >= idl && !memcmp(p + i + 4, id, idl)) { *len = l - 2 - idl; return p + i + 4 + idl; }
        i += 2 + l;
    }
    return NULL;
}
static NSString *xml_attr(NSXMLDocument *doc, NSString *xpath) {
    NSArray *nodes = [doc nodesForXPath:xpath error:nil];
    return nodes.count ? [nodes[0] stringValue] : nil;
}
/* Container structure: MPF offsets, Ultra HDR XMP lengths and parameters. */
static BOOL check_structure(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile:path];
    NSUInteger len = 0;
    const uint8_t *mpf = segment(d, 0, 2, "MPF", &len);
    if (!mpf || len < 82 || be32(mpf + 4) != 8) { printf("FAIL %s: no MPF index\n", path.UTF8String); return NO; }
    const uint8_t *e = mpf + 50;
    uint32_t size0 = be32(e + 4), size1 = be32(e + 20), off1 = be32(e + 24);
    NSUInteger at = (NSUInteger)(mpf - (const uint8_t *)d.bytes) + off1;
    if (size0 + size1 != d.length || at != size0 || ((const uint8_t *)d.bytes)[at] != 0xFF || ((const uint8_t *)d.bytes)[at + 1] != 0xD8) {
        printf("FAIL %s: MPF sizes %u + %u, offset %u for %lu bytes\n", path.UTF8String, size0, size1, off1, (unsigned long)d.length);
        return NO;
    }
    const uint8_t *x = segment(d, 0, 1, "http://ns.adobe.com/xap/1.0/", &len);
    NSXMLDocument *px = x ? [[NSXMLDocument alloc] initWithData:[NSData dataWithBytes:x length:len] options:0 error:nil] : nil;
    NSString *length = xml_attr(px, @"//*[local-name()='Item'][@*[local-name()='Semantic']='GainMap']/@*[local-name()='Length']");
    NSString *version = xml_attr(px, @"//@*[local-name()='Version']");
    if (length.integerValue != (NSInteger)size1 || ![version isEqual:@"1.0"]) {
        printf("FAIL %s: Ultra HDR primary XMP (length %s, version %s)\n", path.UTF8String, length.UTF8String, version.UTF8String);
        return NO;
    }
    x = segment(d, at, 1, "http://ns.adobe.com/xap/1.0/", &len);
    NSXMLDocument *gx = x ? [[NSXMLDocument alloc] initWithData:[NSData dataWithBytes:x length:len] options:0 error:nil] : nil;
    double gmax = xml_attr(gx, @"//@*[local-name()='GainMapMax']").doubleValue;
    double cap = xml_attr(gx, @"//@*[local-name()='HDRCapacityMax']").doubleValue;
    if (fabs(gmax - log2(3)) > 1e-6 || fabs(cap - log2(3)) > 1e-6 || !segment(d, at, 2, "urn:iso:std:iso:ts:21496:-1", &len)) {
        printf("FAIL %s: gain map metadata\n", path.UTF8String);
        return NO;
    }
    return YES;
}
/* HDR decode by Apple: ISO gain map found, HDR = SDR x gain with gain in [1, 3]. */
static BOOL check_hdr(NSString *path, size_t w, size_t h) {
    NSURL *url = [NSURL fileURLWithPath:path];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    NSDictionary *info = src ? CFBridgingRelease(CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, 0, kCGImageAuxiliaryDataTypeISOGainMap)) : nil;
    if (src) CFRelease(src);
    NSDictionary *desc = info[(NSString *)kCGImageAuxiliaryDataInfoDataDescription];
    if ([desc[@"Width"] unsignedLongValue] != (w + 1) / 2 || [desc[@"Height"] unsignedLongValue] != (h + 1) / 2) {
        printf("FAIL %s: ImageIO finds no ISO 21496-1 gain map of %zux%zu\n", path.UTF8String, (w + 1) / 2, (h + 1) / 2);
        return NO;
    }
    CIImage *sdr = [CIImage imageWithContentsOfURL:url], *hdr = [CIImage imageWithContentsOfURL:url options:@{kCIImageExpandToHDR: @YES}];
    CIContext *ctx = [CIContext contextWithOptions:@{kCIContextWorkingFormat: @(kCIFormatRGBAf)}];
    CGColorSpaceRef lin = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    size_t n = w * h;
    float *a = malloc(n * 16), *b = malloc(n * 16);
    [ctx render:sdr toBitmap:a rowBytes:w * 16 bounds:sdr.extent format:kCIFormatRGBAf colorSpace:lin];
    [ctx render:hdr toBitmap:b rowBytes:w * 16 bounds:hdr.extent format:kCIFormatRGBAf colorSpace:lin];
    CGColorSpaceRelease(lin);
    double lo = 9, hi = 0; size_t bright = 0, used = 0;
    for (size_t i = 0; i < n; i++) {
        double ys = 0.2126 * a[i * 4] + 0.7152 * a[i * 4 + 1] + 0.0722 * a[i * 4 + 2];
        double yh = 0.2126 * b[i * 4] + 0.7152 * b[i * 4 + 1] + 0.0722 * b[i * 4 + 2];
        if (ys < 0.02) continue;
        double r = yh / ys; used++;
        lo = fmin(lo, r); hi = fmax(hi, r);
        if (r > 1.05) bright++;
    }
    free(a); free(b);
    BOOL ok = hdr.contentHeadroom > 2.9 && lo > 0.98 && hi < 3.05 && hi > 1.5;
    printf("%s %s: HDR gain map %zux%zu, headroom %.2f, HDR/SDR %.3f ... %.3f, %.2f %% of pixels brightened\n", ok ? "ok  " : "FAIL",
           path.lastPathComponent.UTF8String, (w + 1) / 2, (h + 1) / 2, hdr.contentHeadroom, lo, hi, 100.0 * bright / (used ? used : 1));
    return ok;
}

static BOOL mean_rgb(NSString *path, double rgb[3], size_t *w, size_t *h, NSDictionary **props) {
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    if (!src) return NO;
    if (props) *props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, NULL));
    CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!image) return NO;
    *w = CGImageGetWidth(image); *h = CGImageGetHeight(image);
    const size_t sw = 192, sh = 128;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(NULL, sw, sh, 8, sw * 4, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, sw, sh), image);
    CGImageRelease(image);
    const uint8_t *p = CGBitmapContextGetData(ctx);
    double sum[3] = {0, 0, 0};
    for (size_t i = 0; i < sw * sh; i++) for (int c = 0; c < 3; c++) sum[c] += p[i * 4 + c];
    for (int c = 0; c < 3; c++) rgb[c] = sum[c] / (sw * sh);
    CGContextRelease(ctx);
    return YES;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 3 || (argc - 1) % 2) return 2;
        int failures = 0;
        for (int i = 1; i + 1 < argc; i += 2) {
            NSString *dng = @(argv[i]), *jpg = @(argv[i + 1]);
            double a[3], b[3]; size_t aw, ah, bw, bh; NSDictionary *jp = nil;
            if (!mean_rgb(dng, a, &aw, &ah, NULL)) { printf("FAIL ImageIO cannot render %s\n", argv[i]); failures++; continue; }
            if (!mean_rgb(jpg, b, &bw, &bh, &jp)) { printf("FAIL ImageIO cannot decode %s\n", argv[i + 1]); failures++; continue; }
            NSString *profile = jp[(NSString *)kCGImagePropertyProfileName];
            if (![profile containsString:@"sRGB"]) { printf("FAIL %s: profile %s\n", argv[i + 1], profile.UTF8String); failures++; }
            if (aw != bw || ah != bh) { printf("FAIL size %zux%zu vs %zux%zu\n", aw, ah, bw, bh); failures++; }
            if (!check_structure(jpg) || !check_hdr(jpg, bw, bh)) failures++;
            // Compare mean chromaticity (r, g, b shares), not brightness: tone
            // curves and gamut handling of the two renderers differ.
            double sa = a[0] + a[1] + a[2], sb = b[0] + b[1] + b[2], dist = 0;
            for (int c = 0; c < 3; c++) dist = fmax(dist, fabs(a[c] / sa - b[c] / sb));
            BOOL ok = dist < 0.08;
            printf("%s %s: Apple RGB %.0f/%.0f/%.0f, IR-conv RGB %.0f/%.0f/%.0f (distance %.3f)\n", ok ? "ok  " : "FAIL",
                   dng.lastPathComponent.UTF8String, a[0], a[1], a[2], b[0], b[1], b[2], dist);
            if (!ok) failures++;
        }
        return failures ? 1 : 0;
    }
}
