#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>

static void fail(NSString *message) {
    fprintf(stderr, "Site assets: %s\n", message.UTF8String);
    exit(1);
}
static void writeData(NSData *data, NSString *path) {
    NSError *error = nil;
    // The staging directory is published atomically by build.sh. Avoid
    // Foundation's temporary replacements outside the build sandbox here.
    if (!data || ![data writeToFile:path options:0 error:&error])
        fail([NSString stringWithFormat:@"Cannot write %@: %@", path, error ?: @"encoding failed"]);
}
static CGContextRef bitmap(size_t width, size_t height) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, width * 4, space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (!context) fail(@"Cannot allocate image bitmap.");
    return context;
}
static NSData *png(CGImageRef source, size_t width, size_t height, double fraction, BOOL opaque) {
    CGContextRef context = bitmap(width, height);
    if (opaque) {
        CGContextSetRGBFillColor(context, 0, 0, 0, 1);
        CGContextFillRect(context, CGRectMake(0, 0, width, height));
    }
    double scale = fmin((double)width / CGImageGetWidth(source), (double)height / CGImageGetHeight(source)) * fraction;
    double w = CGImageGetWidth(source) * scale, h = CGImageGetHeight(source) * scale;
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake((width - w) / 2, (height - h) / 2, w, h), source);
    CGImageRef image = CGBitmapContextCreateImage(context);
    NSMutableData *data = [NSMutableData data];
    CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.png"), 1, NULL);
    if (!image || !destination) fail(@"Cannot create PNG encoder.");
    CGImageDestinationAddImage(destination, image, NULL);
    if (!CGImageDestinationFinalize(destination)) fail(@"PNG encoding failed.");
    CFRelease(destination);
    CGImageRelease(image);
    CGContextRelease(context);
    return data;
}
static void littleEndian(NSMutableData *data, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) { uint8_t byte = value >> (i * 8); [data appendBytes:&byte length:1]; }
}
static NSData *ico(CGImageRef source) {
    NSArray<NSNumber *> *sizes = @[@16, @32, @48, @64, @128, @256];
    NSMutableArray<NSData *> *frames = [NSMutableArray new];
    for (NSNumber *size in sizes) [frames addObject:png(source, size.unsignedIntegerValue, size.unsignedIntegerValue, 1, NO)];
    NSMutableData *data = [NSMutableData data];
    littleEndian(data, 0, 2); littleEndian(data, 1, 2); littleEndian(data, (uint32_t)sizes.count, 2);
    uint32_t offset = 6 + 16 * (uint32_t)sizes.count;
    for (NSUInteger i = 0; i < sizes.count; ++i) {
        littleEndian(data, sizes[i].unsignedIntValue % 256, 1);
        littleEndian(data, sizes[i].unsignedIntValue % 256, 1);
        littleEndian(data, 0, 1); littleEndian(data, 0, 1);
        littleEndian(data, 1, 2); littleEndian(data, 32, 2);
        littleEndian(data, (uint32_t)frames[i].length, 4); littleEndian(data, offset, 4);
        offset += frames[i].length;
    }
    for (NSData *frame in frames) [data appendData:frame];
    return data;
}
static CGImageRef load(NSString *path) {
    CGImageSourceRef decoder = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    CGImageRef image = decoder ? CGImageSourceCreateImageAtIndex(decoder, 0, NULL) : NULL;
    if (decoder) CFRelease(decoder);
    if (!image) fail([@"Cannot decode " stringByAppendingString:path]);
    if (CGImageGetWidth(image) != CGImageGetHeight(image) || CGImageGetWidth(image) < 1024)
        fail([path stringByAppendingString:@" must be square and at least 1024 pixels wide."]);
    return image;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 6) { fprintf(stderr, "Usage: site-assets ICON MASKABLE_ICON STAGE RELEASE HTML_TEMPLATE\n"); return 2; }
        NSString *root = @(argv[3]), *release = @(argv[4]);
        NSString *prefix = [NSString stringWithFormat:@"releases/%@/", release];
        NSString *directory = [root stringByAppendingPathComponent:prefix];
        // The regular icon keeps its transparency (iOS shows it on a light or
        // dark background); the maskable icon is a separate full-bleed image.
        CGImageRef source = load(@(argv[1])), maskable = load(@(argv[2]));
        NSArray *images = @[
            @[@"favicon-16.png", @16, @16, @1, @NO],
            @[@"favicon-32.png", @32, @32, @1, @NO],
            @[@"favicon-48.png", @48, @48, @1, @NO],
            @[@"apple-touch-icon-152.png", @152, @152, @1, @NO],
            @[@"apple-touch-icon-167.png", @167, @167, @1, @NO],
            @[@"apple-touch-icon.png", @180, @180, @1, @NO],
            @[@"icon-192.png", @192, @192, @1, @NO],
            @[@"icon-512.png", @512, @512, @1, @NO],
            @[@"icon-1024.png", @1024, @1024, @1, @NO],
            @[@"icon-maskable-512.png", @512, @512, @1, @YES],
            @[@"icon-maskable-1024.png", @1024, @1024, @1, @YES]
        ];
        for (NSArray *spec in images)
            writeData(png([spec[4] boolValue] ? maskable : source, [spec[1] unsignedIntegerValue], [spec[2] unsignedIntegerValue],
                [spec[3] doubleValue], [spec[4] boolValue]), [directory stringByAppendingPathComponent:spec[0]]);
        writeData(ico(source), [directory stringByAppendingPathComponent:@"favicon.ico"]);
        CGImageRelease(source); CGImageRelease(maskable);

        NSMutableArray *icons = [NSMutableArray new];
        for (NSNumber *size in @[@192, @512, @1024])
            [icons addObject:@{@"src": [prefix stringByAppendingFormat:@"icon-%@.png", size],
                @"sizes": [NSString stringWithFormat:@"%@x%@", size, size], @"type": @"image/png", @"purpose": @"any"}];
        for (NSNumber *size in @[@512, @1024])
            [icons addObject:@{@"src": [prefix stringByAppendingFormat:@"icon-maskable-%@.png", size],
                @"sizes": [NSString stringWithFormat:@"%@x%@", size, size], @"type": @"image/png", @"purpose": @"maskable"}];
        // Without a hardcoded deployment path, let identity default to the
        // stable start_url. An explicit relative id would resolve at the origin
        // root and collide with other subdirectory installations.
        NSDictionary *manifest = @{@"name": @"IR-conv", @"short_name": @"IR-conv",
            @"description": @"Infrared raw converter: red/blue swap and unbounded white balance for NEF and DNG, computed locally.",
            @"lang": @"en", @"start_url": @"./", @"scope": @"./", @"display": @"standalone",
            @"background_color": @"#111214", @"theme_color": @"#111214", @"icons": icons};
        writeData([NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil],
            [directory stringByAppendingPathComponent:@"site.webmanifest"]);
        // Conventional stable URLs also work before service worker installation.
        for (NSString *name in @[@"favicon.ico", @"apple-touch-icon.png", @"site.webmanifest"])
            writeData([NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:name]], [root stringByAppendingPathComponent:name]);

        NSString *document = [NSString stringWithContentsOfFile:@(argv[5]) encoding:NSUTF8StringEncoding error:nil];
        if (![document containsString:@"__RELEASE__"]) fail(@"HTML template is missing the release placeholder.");
        document = [document stringByReplacingOccurrencesOfString:@"__RELEASE__" withString:release];
        NSData *bytes = [document dataUsingEncoding:NSUTF8StringEncoding];
        writeData(bytes, [root stringByAppendingPathComponent:@"index.html"]);
        writeData(bytes, [directory stringByAppendingPathComponent:@"index.html"]);
        puts("Generated favicons, Apple Touch, PWA and maskable icons and the manifest.");
    }
    return 0;
}
