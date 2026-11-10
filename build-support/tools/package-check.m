#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>

// Decode the actual product, including alpha and the maskable safe circle;
// neither a matching filename nor a PNG header is enough.
static BOOL imageValid(NSData *data, size_t width, size_t height, int mode) {
    if (!data) return NO;
    CGImageSourceRef decoder = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    if (!decoder) return NO;
    CGImageRef image = CGImageSourceCreateImageAtIndex(decoder, 0, NULL);
    CFRelease(decoder);
    if (!image) return NO;
    if (CGImageGetWidth(image) != width || CGImageGetHeight(image) != height) { CGImageRelease(image); return NO; }
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, width * 4, space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (!context) { CGImageRelease(image); return NO; }
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    const uint8_t *pixels = CGBitmapContextGetData(context);
    BOOL transparent = NO, artwork = NO, valid = YES;
    for (size_t y = 0; y < height && valid; ++y) for (size_t x = 0; x < width; ++x) {
        const uint8_t *pixel = pixels + (y * width + x) * 4;
        if (!pixel[3]) transparent = YES;
        if (pixel[2] > 32 && pixel[3] > 32) artwork = YES;
        if (mode && pixel[3] != 255) { valid = NO; break; }
    }
    CGContextRelease(context); CGImageRelease(image);
    return valid && artwork && (mode || transparent);
}
static uint32_t littleEndian(const uint8_t *bytes, unsigned count) {
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= (uint32_t)bytes[i] << (i * 8);
    return value;
}
static BOOL icoValid(NSData *data) {
    const size_t sizes[] = {16, 32, 48, 64, 128, 256};
    if (data.length < 102) return NO;
    const uint8_t *bytes = data.bytes;
    if (littleEndian(bytes, 2) != 0 || littleEndian(bytes + 2, 2) != 1 || littleEndian(bytes + 4, 2) != 6) return NO;
    NSUInteger expected = 102;
    for (unsigned i = 0; i < 6; ++i) {
        const uint8_t *entry = bytes + 6 + i * 16;
        size_t width = entry[0] ?: 256, height = entry[1] ?: 256;
        NSUInteger length = littleEndian(entry + 8, 4), offset = littleEndian(entry + 12, 4);
        if (width != sizes[i] || height != sizes[i] || offset != expected || !length || offset + length > data.length) return NO;
        if (!imageValid([data subdataWithRange:NSMakeRange(offset, length)], width, height, 0)) return NO;
        expected += length;
    }
    return expected == data.length;
}
static BOOL siteAssetsValid(NSString *root, NSString *prefix, NSString *html) {
    NSString *directory = [root stringByAppendingPathComponent:prefix];
    NSArray *specs = @[
        @[@"favicon-16.png", @16, @16, @0], @[@"favicon-32.png", @32, @32, @0], @[@"favicon-48.png", @48, @48, @0],
        @[@"apple-touch-icon-152.png", @152, @152, @0], @[@"apple-touch-icon-167.png", @167, @167, @0],
        @[@"apple-touch-icon.png", @180, @180, @0], @[@"icon-192.png", @192, @192, @0],
        @[@"icon-512.png", @512, @512, @0], @[@"icon-1024.png", @1024, @1024, @0],
        @[@"icon-maskable-512.png", @512, @512, @2], @[@"icon-maskable-1024.png", @1024, @1024, @2]
    ];
    for (NSArray *spec in specs)
        if (!imageValid([NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:spec[0]]],
            [spec[1] unsignedIntegerValue], [spec[2] unsignedIntegerValue], [spec[3] intValue])) return NO;
    if (!icoValid([NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"favicon.ico"]])) return NO;
    for (NSString *name in @[@"favicon.ico", @"apple-touch-icon.png", @"site.webmanifest"])
        if (![[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:name]]
            isEqual:[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:name]]]) return NO;
    NSData *bytes = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"site.webmanifest"]];
    NSDictionary *manifest = bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil] : nil;
    if (![manifest isKindOfClass:NSDictionary.class] || ![manifest[@"name"] isEqual:@"IR Converter"] ||
        manifest[@"id"] || ![manifest[@"start_url"] isEqual:@"./"] || ![manifest[@"scope"] isEqual:@"./"] ||
        ![manifest[@"display"] isEqual:@"standalone"] || ![manifest[@"icons"] isKindOfClass:NSArray.class] || [manifest[@"icons"] count] != 5) return NO;
    NSMutableSet *expected = [NSMutableSet new];
    for (NSNumber *size in @[@192, @512, @1024]) [expected addObject:[NSString stringWithFormat:@"%@icon-%@.png|%@x%@|any", prefix, size, size, size]];
    for (NSNumber *size in @[@512, @1024]) [expected addObject:[NSString stringWithFormat:@"%@icon-maskable-%@.png|%@x%@|maskable", prefix, size, size, size]];
    for (id icon in manifest[@"icons"]) {
        if (![icon isKindOfClass:NSDictionary.class] || ![icon[@"type"] isEqual:@"image/png"]) return NO;
        NSString *key = [NSString stringWithFormat:@"%@|%@|%@", icon[@"src"], icon[@"sizes"], icon[@"purpose"]];
        if (![expected containsObject:key]) return NO;
        [expected removeObject:key];
    }
    for (NSString *name in @[@"favicon.ico", @"favicon-16.png", @"favicon-32.png", @"favicon-48.png", @"apple-touch-icon-152.png", @"apple-touch-icon-167.png", @"apple-touch-icon.png"])
        if (![html containsString:[NSString stringWithFormat:@"href=\"%@%@\"", prefix, name]]) return NO;
    return !expected.count && ![html containsString:@"__RELEASE__"] &&
        [html containsString:@"<link rel=\"manifest\" href=\"site.webmanifest\">"];
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *root = [NSURL fileURLWithPath:@(argv[1])].path.stringByStandardizingPath;
        NSString *release = [[NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"release.txt"] encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (release.length != 36) return 1;
        NSString *prefix = [NSString stringWithFormat:@"releases/%@/", release];
        NSData *data = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:[prefix stringByAppendingString:@"offline-resources.json"]]];
        NSDictionary *inventory = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (![inventory isKindOfClass:NSDictionary.class] || ![inventory[@"release"] isEqual:release] || ![inventory[@"resources"] isKindOfClass:NSArray.class]) return 1;
        NSMutableSet *paths = [NSMutableSet new];
        for (id item in inventory[@"resources"]) {
            if (![item isKindOfClass:NSDictionary.class] || ![item[@"path"] isKindOfClass:NSString.class] || ![item[@"size"] isKindOfClass:NSNumber.class]) return 1;
            NSString *relative = item[@"path"];
            if (![relative hasPrefix:prefix] || [paths containsObject:relative]) return 1;
            [paths addObject:relative];
            NSString *path = [[root stringByAppendingPathComponent:relative] stringByStandardizingPath];
            if (![path hasPrefix:[root stringByAppendingString:@"/"]]) return 1;
            NSDictionary *attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
            if (!attributes || ![attributes[NSFileType] isEqual:NSFileTypeRegular] || [attributes fileSize] != [item[@"size"] unsignedLongLongValue] || ![attributes fileSize]) return 1;
        }
        for (NSString *name in @[@"index.html", @"app.js", @"app.css", @"worker.js", @"engine.wasm", @"XCC-LICENSE.txt",
            @"site.webmanifest", @"favicon.ico", @"favicon-16.png", @"favicon-32.png", @"favicon-48.png",
            @"apple-touch-icon-152.png", @"apple-touch-icon-167.png", @"apple-touch-icon.png",
            @"icon-192.png", @"icon-512.png", @"icon-1024.png", @"icon-maskable-512.png", @"icon-maskable-1024.png"])
            if (![paths containsObject:[prefix stringByAppendingString:name]]) return 1;
        NSData *index = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"index.html"]];
        NSData *versioned = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:[prefix stringByAppendingString:@"index.html"]]];
        if (!index || ![index isEqual:versioned]) return 1;
        if (!siteAssetsValid(root, prefix, [[NSString alloc] initWithData:index encoding:NSUTF8StringEncoding])) return 1;
        NSString *worker = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"service-worker.js"] encoding:NSUTF8StringEncoding error:nil];
        if (![worker containsString:[NSString stringWithFormat:@"const RELEASE = '%@'", release]]) return 1;
    }
    return 0;
}
