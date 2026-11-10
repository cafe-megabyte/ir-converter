#import <Foundation/Foundation.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
static BOOL send_all(int fd, const void *data, size_t size) {
    const uint8_t *p = data;
    while (size) { ssize_t n = send(fd, p, size, 0); if (n <= 0) return NO; p += n; size -= n; } return YES;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        NSString *root = [NSURL fileURLWithPath:@(argv[1])].path.stringByStandardizingPath;
        int port = argc > 2 ? atoi(argv[2]) : 8080;
        if (port < 1 || port > 65535) return 2;
        signal(SIGPIPE, SIG_IGN); int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr = {.sin_family=AF_INET, .sin_port=htons(port), .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        if (bind(fd, (void *)&addr, sizeof(addr)) || listen(fd, 16)) { perror("server"); return 1; }
        printf("IR-conv: http://localhost:%d/ (Ctrl-C to stop)\n", port); fflush(stdout);
        for (;;) { @autoreleasepool {
            int client = accept(fd, NULL, NULL); if (client < 0) { if (errno == EINTR) continue; break; }
            struct timeval timeout = {5, 0}; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            char buffer[8192]; ssize_t n = recv(client, buffer, sizeof(buffer)-1, 0);
            if (n <= 0) { close(client); continue; } buffer[n] = 0;
            NSString *request = @(buffer); NSArray *parts = [request componentsSeparatedByString:@" "];
            NSString *method = parts.count ? parts[0] : @"";
            NSString *path = parts.count > 1 ? [parts[1] componentsSeparatedByString:@"?"].firstObject.stringByRemovingPercentEncoding : nil;
            if ([path hasSuffix:@"/"]) path = [path stringByAppendingString:@"index.html"];
            NSString *file = path ? [[root stringByAppendingPathComponent:path] stringByStandardizingPath] : @"";
            BOOL allowed = ([method isEqual:@"GET"] || [method isEqual:@"HEAD"]) && [file hasPrefix:[root stringByAppendingString:@"/"]];
            NSData *body = allowed ? [NSData dataWithContentsOfFile:file] : nil;
            int status = body ? 200 : 404;
            if (!body) body = [@"Not found\n" dataUsingEncoding:NSUTF8StringEncoding];
            NSDictionary *types = @{@"html":@"text/html; charset=utf-8", @"js":@"text/javascript; charset=utf-8", @"css":@"text/css; charset=utf-8", @"wasm":@"application/wasm", @"json":@"application/json", @"webmanifest":@"application/manifest+json", @"png":@"image/png", @"ico":@"image/x-icon", @"jpg":@"image/jpeg", @"dng":@"image/x-adobe-dng", @"nef":@"image/x-nikon-nef"};
            NSString *type = types[file.pathExtension] ?: @"application/octet-stream";
            NSString *header = [NSString stringWithFormat:@"HTTP/1.1 %d %@\r\nContent-Type: %@\r\nContent-Length: %lu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", status, status == 200 ? @"OK" : @"Not Found", type, (unsigned long)body.length];
            NSData *h = [header dataUsingEncoding:NSUTF8StringEncoding]; send_all(client, h.bytes, h.length);
            if (![method isEqual:@"HEAD"]) send_all(client, body.bytes, body.length); close(client);
        }} close(fd);
    } return 0;
}
