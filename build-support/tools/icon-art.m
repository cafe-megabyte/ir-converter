// Draws assets/icon.png (transparent, free-standing badge) and
// assets/icon-maskable.png (full bleed, motif inside the safe zone).
// Not part of the build; regenerate manually after editing:
//   clang -fobjc-arc -framework Foundation -framework CoreGraphics -framework ImageIO \
//     build-support/tools/icon-art.m -o /tmp/icon-art && /tmp/icon-art assets/icon.png assets/icon-maskable.png
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>
static CGColorSpaceRef cs;
static CGColorRef rgba(double r, double g, double b, double a) { CGFloat c[4] = {r, g, b, a}; return CGColorCreate(cs, c); }
static void gradient(CGContextRef c, CGPoint a, CGPoint b, int n, const CGFloat *comps, const CGFloat *locs) {
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, n);
    CGContextDrawLinearGradient(c, g, a, b, kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(g);
}
/* Background in a unit box [0,1]^2 (y up): IR sky, sea, sand. */
static void background(CGContextRef c, CGRect box) {
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, box.origin.x, box.origin.y);
    CGContextScaleCTM(c, box.size.width, box.size.height);
    CGFloat sky[] = {0.05,0.13,0.42,1, 0.13,0.36,0.72,1, 0.62,0.80,0.92,1}, sl[] = {0, 0.55, 1}, el[] = {0, 1};
    CGContextSaveGState(c); CGContextClipToRect(c, CGRectMake(-1, 0.30, 3, 2));
    gradient(c, CGPointMake(0, 1), CGPointMake(0, 0.30), 3, sky, sl); CGContextRestoreGState(c);
    CGFloat sea[] = {0.03,0.06,0.16,1, 0.09,0.15,0.32,1};
    CGContextSaveGState(c); CGContextClipToRect(c, CGRectMake(-1, 0.17, 3, 0.13));
    gradient(c, CGPointMake(0, 0.30), CGPointMake(0, 0.17), 2, sea, el); CGContextRestoreGState(c);
    CGFloat sand[] = {0.84,0.86,0.89,1, 0.97,0.97,0.98,1};
    CGContextSaveGState(c); CGContextClipToRect(c, CGRectMake(-1, -2, 3, 2.17));
    gradient(c, CGPointMake(0, 0.17), CGPointMake(0, 0), 2, sand, el); CGContextRestoreGState(c);
    CGContextRestoreGState(c);
}
/* Palm in a unit box: glowing white, as foliage reflects infrared strongly. */
static void palm(CGContextRef c, CGRect box) {
    CGContextSaveGState(c);
    CGContextSetShadowWithColor(c, CGSizeZero, box.size.width * 0.04, rgba(1, 1, 1, 0.85));
    CGContextBeginTransparencyLayer(c, NULL);
    CGContextTranslateCTM(c, box.origin.x, box.origin.y);
    CGContextScaleCTM(c, box.size.width, box.size.height);
    CGPoint crown = CGPointMake(0.55, 0.69);
    CGMutablePathRef t = CGPathCreateMutable();          /* trunk, leaning and curved */
    CGPathMoveToPoint(t, NULL, 0.405, 0.11);
    CGPathAddCurveToPoint(t, NULL, 0.45, 0.36, 0.53, 0.50, crown.x - 0.013, crown.y);
    CGPathAddLineToPoint(t, NULL, crown.x + 0.013, crown.y);
    CGPathAddCurveToPoint(t, NULL, 0.56, 0.50, 0.49, 0.34, 0.47, 0.11);
    CGPathCloseSubpath(t);
    CGContextSetFillColorWithColor(c, rgba(0.95, 0.95, 0.97, 1));
    CGContextAddPath(c, t); CGContextFillPath(c); CGPathRelease(t);
    const double angles[] = {196, 172, 146, 120, 96, 72, 46, 20, -6, 222, -30};
    const double lengths[] = {0.33, 0.35, 0.30, 0.24, 0.20, 0.24, 0.31, 0.35, 0.32, 0.22, 0.20};
    CGContextSetFillColorWithColor(c, rgba(1, 1, 1, 1));
    for (int i = 0; i < 11; i++) {
        double a = angles[i] * M_PI / 180, L = lengths[i];
        double dx = cos(a), dy = sin(a), nx = -dy, ny = dx;
        double droop = 0.55 * L * fabs(dx) * fabs(dx);     /* outer fronds arch downwards */
        CGPoint tip = CGPointMake(crown.x + dx * L, crown.y + dy * L * 0.8 - droop * 0.6);
        CGPoint c1 = CGPointMake(crown.x + dx * L * 0.45, crown.y + dy * L * 0.45 + 0.05 * fabs(dx));
        double w = 0.05 * L / 0.3;
        CGMutablePathRef f = CGPathCreateMutable();
        CGPathMoveToPoint(f, NULL, crown.x, crown.y);
        CGPathAddCurveToPoint(f, NULL, c1.x + nx * w, c1.y + ny * w, tip.x + nx * w * 0.4, tip.y + ny * w * 0.4, tip.x, tip.y);
        CGPathAddCurveToPoint(f, NULL, tip.x - nx * w * 0.2, tip.y - ny * w * 0.2, c1.x - nx * w * 0.5, c1.y - ny * w * 0.5, crown.x, crown.y);
        CGPathCloseSubpath(f);
        CGContextAddPath(c, f); CGContextFillPath(c); CGPathRelease(f);
    }
    CGContextAddEllipseInRect(c, CGRectMake(crown.x - 0.025, crown.y - 0.02, 0.05, 0.04));
    CGContextFillPath(c);
    CGContextEndTransparencyLayer(c);
    CGContextRestoreGState(c);
}
static void scene(CGContextRef c, CGRect box) { background(c, box); palm(c, box); }
static void save(CGContextRef c, const char *path) {
    CGImageRef img = CGBitmapContextCreateImage(c);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL); CGImageDestinationFinalize(d); CFRelease(d); CGImageRelease(img);
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        const size_t N = 1024;
        cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        // 1. Transparent icon: round badge with a red-to-blue rim.
        CGContextRef c = CGBitmapContextCreate(NULL, N, N, 8, N * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
        CGRect outer = CGRectInset(CGRectMake(0, 0, N, N), 40, 40), inner = CGRectInset(outer, 34, 34);
        CGContextSaveGState(c);
        CGContextSetShadowWithColor(c, CGSizeMake(0, -10), 30, rgba(0, 0, 0, 0.35));
        CGContextAddEllipseInRect(c, outer); CGContextSetFillColorWithColor(c, rgba(0.5, 0.3, 0.6, 1)); CGContextFillPath(c);
        CGContextRestoreGState(c);
        CGContextSaveGState(c);
        CGContextAddEllipseInRect(c, outer); CGContextClip(c);
        CGFloat rim[] = {0.86,0.20,0.17,1, 0.55,0.22,0.58,1, 0.16,0.42,0.90,1};
        CGFloat rl[] = {0, 0.5, 1};
        gradient(c, CGPointMake(outer.origin.x, CGRectGetMaxY(outer)), CGPointMake(CGRectGetMaxX(outer), outer.origin.y), 3, rim, rl);
        CGContextRestoreGState(c);
        CGContextSaveGState(c);
        CGContextAddEllipseInRect(c, inner); CGContextClip(c);
        scene(c, inner);
        // Glossy highlight on the upper half.
        CGFloat gloss[] = {1,1,1,0.28, 1,1,1,0};
        CGFloat gl[] = {0, 1};
        gradient(c, CGPointMake(0, CGRectGetMaxY(inner)), CGPointMake(0, CGRectGetMidY(inner) + 60), 2, gloss, gl);
        CGContextRestoreGState(c);
        save(c, argv[1]);
        CGContextRelease(c);
        // 2. Maskable: full-bleed background; the palm stays inside the safe circle.
        c = CGBitmapContextCreate(NULL, N, N, 8, N * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        background(c, CGRectMake(0, 0, N, N));
        palm(c, CGRectMake(N * 0.15, N * 0.04, N * 0.70, N * 0.86));
        save(c, argv[2]);
        return 0;
    }
}
