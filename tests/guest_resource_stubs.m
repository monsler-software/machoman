#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

static void require(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
typedef struct { unsigned long a, b, c, d, e; } LargeValue;
typedef struct { float a, b, c, d; } FloatValue;
@protocol DynamicContract
@property(nonatomic, retain) NSString *title;
@property(nonatomic, retain) id child;
@property(nonatomic) double gain;
@property(nonatomic) float fraction;
@property(nonatomic) NSRect frame;
@property(nonatomic) NSRange range;
@property(nonatomic) LargeValue payload;
@property(nonatomic) FloatValue vector;
@property(nonatomic, getter=isEnabled, setter=enable:) BOOL enabled;
- (instancetype)initWithFlag:(BOOL)flag;
- (void *)unsafePointer;
- (BOOL)unsupportedOutput:(id *)output;
@end
@interface DynamicProbe : AVAsset <DynamicContract>
@end
@implementation DynamicProbe
@dynamic title, child, gain, fraction, frame, range, payload, vector, enabled;
@end

static void resources(void) {
    double absolute = CFAbsoluteTimeGetCurrent();
    require(absolute > 0. && fabs(absolute + kCFAbsoluteTimeIntervalSince1970 - [[NSDate date] timeIntervalSince1970]) < 1., "CF clock uses the 2001 epoch and FP return ABI");
    char root[] = "/tmp/machoman-resource-XXXXXX";
    require(mkdtemp(root) != NULL, "resource fixture");
    NSString *directory = [NSString stringWithUTF8String:root];
    NSString *path = [directory stringByAppendingPathComponent:@".file.app"];
    int fd = open(path.fileSystemRepresentation, O_WRONLY|O_CREAT|O_EXCL, 0600);
    require(fd >= 0 && write(fd, "resource", 8) == 8, "file created"); close(fd);
    struct timespec dates[2] = {{1234567890, 0}, {1234567891, 0}};
    require(!utimensat(AT_FDCWD, path.fileSystemRepresentation, dates, 0), "known timestamps");
    NSURL *url = [NSURL fileURLWithPath:path];
    id value = nil; NSError *error = nil;
    require([url getResourceValue:&value forKey:NSURLFileSizeKey error:&error] && [value longLongValue] == 8 && !error, "resource file size");
    require([url getResourceValue:&value forKey:NSURLIsHiddenKey error:NULL] && [value boolValue], "hidden file");
    require([url getResourceValue:&value forKey:NSURLIsPackageKey error:NULL] && ![value boolValue], "regular .app is not a package");
    require([url getResourceValue:&value forKey:NSURLContentModificationDateKey error:NULL] && [(NSDate *)value timeIntervalSince1970] == 1234567891., "date retains fractional ABI correctly");
    NSDictionary *values = [url resourceValuesForKeys:@[NSURLFileSizeKey, NSURLIsDirectoryKey, NSURLNameKey, @"MachomanUnavailableKey"] error:&error];
    require(values && !error && [[values objectForKey:NSURLFileSizeKey] longLongValue] == 8 && ![values objectForKey:@"MachomanUnavailableKey"], "bulk values omit unavailable properties");
    value = @"sentinel";
    require([url getResourceValue:&value forKey:@"MachomanUnavailableKey" error:&error] && !value && !error, "unavailable property is successful nil");
    require([url getResourceValue:NULL forKey:NSURLFileSizeKey error:NULL], "optional output pointers");
    fd = open(path.fileSystemRepresentation, O_WRONLY|O_APPEND); require(fd >= 0 && write(fd, "!", 1) == 1, "file changes"); close(fd);
    require([url getResourceValue:&value forKey:NSURLFileSizeKey error:NULL] && [value longLongValue] == 8, "resource cache");
    [url removeCachedResourceValueForKey:NSURLFileSizeKey];
    require([url getResourceValue:&value forKey:NSURLFileSizeKey error:NULL] && [value longLongValue] == 9, "cache invalidation");
    [url setTemporaryResourceValue:@"temporary" forKey:NSURLNameKey];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0., NO);
    require([url getResourceValue:&value forKey:NSURLNameKey error:NULL] && [value isEqual:@"temporary"], "temporary value survives main loop");
    fd = open(path.fileSystemRepresentation, O_WRONLY|O_APPEND); require(fd >= 0 && write(fd, "!", 1) == 1, "file changes again"); close(fd);
    require([url getResourceValue:&value forKey:NSURLFileSizeKey error:NULL] && [value longLongValue] == 10, "main loop invalidates Cocoa cache");
    NSString *linkPath = [directory stringByAppendingPathComponent:@"broken"];
    require(!symlink("missing", linkPath.fileSystemRepresentation), "dangling symlink");
    NSURL *link = [NSURL fileURLWithPath:linkPath];
    require([link getResourceValue:&value forKey:NSURLIsSymbolicLinkKey error:&error] && [value boolValue] && !error, "dangling symlink metadata");
    NSURL *missing = [NSURL fileURLWithPath:[directory stringByAppendingPathComponent:@"missing"]];
    value = @"sentinel";
    require(![missing getResourceValue:&value forKey:NSURLFileSizeKey error:&error] && !value && error.code == NSFileReadNoSuchFileError && [error.domain isEqual:NSCocoaErrorDomain], "missing file error and cleared output");
    error = nil;
    require(![[NSURL URLWithString:@"https://example.com/file"] getResourceValue:&value forKey:NSURLFileSizeKey error:&error] && error.code == NSFileReadUnsupportedSchemeError, "unsupported URL scheme");
    require(!unlink(linkPath.fileSystemRepresentation) && !unlink(path.fileSystemRepresentation) && !rmdir(root), "resource fixture removed");
}

static unsigned long replacement(id self, SEL selector) { (void)self; (void)selector; return 777; }
static void dynamic_stubs(void) {
    DynamicProbe *probe = [[DynamicProbe alloc] initWithFlag:YES];
    require(probe != nil, "dynamic initializer preserves receiver");
    require(probe.title != nil && [probe.title isKindOfClass:[NSString class]] && probe.title.length == 0, "typed object placeholder uses declared class");
    probe.title = @"state"; require([probe.title isEqual:@"state"], "object setter/getter");
    probe.child = nil; require(probe.child == nil, "explicit nil remains nil");
    require(probe.gain == 0. && probe.fraction == 0.f, "zero FP defaults");
    probe.gain = 0.75; probe.fraction = 0.25f;
    require(probe.gain == 0.75 && probe.fraction == 0.25f, "FP state round trip");
    NSRect frame = probe.frame; require(frame.origin.x == 0. && frame.size.height == 0., "four-register HFA default");
    probe.frame = NSMakeRect(1., 2., 3., 4.); frame = probe.frame;
    require(frame.origin.x == 1. && frame.origin.y == 2. && frame.size.width == 3. && frame.size.height == 4., "four-register HFA round trip");
    probe.range = NSMakeRange(12, 34); require(probe.range.location == 12 && probe.range.length == 34, "two-register integer aggregate");
    struct { LargeValue value; unsigned long guard; } large; large.guard = 0xabcdef;
    large.value = probe.payload;
    require(!large.value.a && !large.value.e && large.guard == 0xabcdef, "indirect result uses known extent");
    probe.payload = (LargeValue){1, 2, 3, 4, 5}; large.value = probe.payload;
    require(large.value.a == 1 && large.value.e == 5 && large.guard == 0xabcdef, "indirect aggregate round trip");
    probe.vector = (FloatValue){1.f, 2.f, 3.f, 4.f}; FloatValue vector = probe.vector;
    require(vector.a == 1.f && vector.b == 2.f && vector.c == 3.f && vector.d == 4.f, "float HFA round trip");
    require(!probe.isEnabled, "custom bool getter default"); probe.enabled = YES; require(probe.isEnabled, "custom accessor state");
    require(![probe unsafePointer], "unsafe raw pointer is diagnosed");
    id output = @"sentinel"; require(![probe unsupportedOutput:&output] && [output isEqual:@"sentinel"], "unknown output never claims success");
    SEL selector = sel_registerName("unimplementedCounter");
    require(class_addMethod([DynamicProbe class], selector, (IMP)replacement, "Q@:"), "real method registered");
    require(((unsigned long (*)(id, SEL))objc_msgSend)(probe, selector) == 777, "real implementation has priority");
    [probe release];
}
int main(void) { @autoreleasepool { resources(); dynamic_stubs(); puts("NSURL resources and typed dynamic stubs passed"); } return 0; }
