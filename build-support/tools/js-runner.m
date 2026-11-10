// Runs a JavaScript check in JavaScriptCore (with WebAssembly) against the
// built engine. Files are exchanged as Uint8Array without per-byte copies.
#import <Foundation/Foundation.h>
#import <JavaScriptCore/JavaScriptCore.h>

static void release_bytes(void *bytes, void *context) { (void)context; free(bytes); }
static JSValue *typed_array(JSContext *context, NSData *data) {
    void *copy = malloc(data.length ? data.length : 1);
    memcpy(copy, data.bytes, data.length);
    JSValueRef exception = NULL;
    JSObjectRef array = JSObjectMakeTypedArrayWithBytesNoCopy(context.JSGlobalContextRef, kJSTypedArrayTypeUint8Array,
        copy, data.length, release_bytes, NULL, &exception);
    return exception ? nil : [JSValue valueWithJSValueRef:array inContext:context];
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        NSData *module = [NSData dataWithContentsOfFile:@(argv[1])];
        NSString *script = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil];
        if (!module || !script) return 2;
        JSContext *context = [JSContext new];
        __block BOOL failed = NO;
        context.exceptionHandler = ^(JSContext *ctx, JSValue *exception) {
            (void)ctx; fprintf(stderr, "%s\n", exception.toString.UTF8String); failed = YES;
        };
        context[@"moduleBytes"] = typed_array(context, module);
        context[@"report"] = ^(NSString *s) { puts(s.UTF8String); fflush(stdout); };
        context[@"readText"] = ^NSString *(NSString *path) { return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil]; };
        context[@"readBytes"] = ^JSValue *(NSString *path) {
            NSData *d = [NSData dataWithContentsOfFile:path];
            return d ? typed_array([JSContext currentContext], d) : [JSValue valueWithNullInContext:[JSContext currentContext]];
        };
        context[@"writeBytes"] = ^BOOL(NSString *path, JSValue *value) {
            NSString *root = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingString:@"/build/internal/"];
            NSString *absolute = [[NSURL fileURLWithPath:path].path stringByStandardizingPath];
            if (![absolute hasPrefix:root]) return NO;
            JSContextRef ctx = [JSContext currentContext].JSGlobalContextRef;
            JSObjectRef object = JSValueToObject(ctx, value.JSValueRef, NULL);
            if (!object || JSValueGetTypedArrayType(ctx, value.JSValueRef, NULL) != kJSTypedArrayTypeUint8Array) return NO;
            void *bytes = JSObjectGetTypedArrayBytesPtr(ctx, object, NULL);
            size_t length = JSObjectGetTypedArrayByteLength(ctx, object, NULL);
            return [[NSData dataWithBytes:bytes length:length] writeToFile:absolute atomically:NO];
        };
        [context evaluateScript:script withSourceURL:[NSURL fileURLWithPath:@(argv[2])]];
        // WebAssembly compilation and promises settle in the run loop.
        for (int i = 0; i < 6000 && !failed && ![context[@"testDone"] toBool]; i++)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        if (failed || [context[@"testFailed"] toBool] || ![context[@"testDone"] toBool]) { if (!failed) fprintf(stderr, "Check did not finish.\n"); return 1; }
        return 0;
    }
}
