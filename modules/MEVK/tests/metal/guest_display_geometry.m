#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "MEVK display FAIL: %s\n", message); abort(); }
}
int main(void) {
    @autoreleasepool {
        CGDirectDisplayID display = CGMainDisplayID();
        check(display == 1, "main display identity");
        CGRect bounds = CGDisplayBounds(display);
        NSRect screen = [[NSScreen mainScreen] frame];
        check(bounds.origin.x == screen.origin.x && bounds.origin.y == screen.origin.y &&
              bounds.size.width == screen.size.width && bounds.size.height == screen.size.height &&
              bounds.size.width > 0 && bounds.size.height > 0, "display matches NSScreen");
        // Volatile inputs keep the calls external and exercise the aggregate ABI.
        volatile double width = 949, height = 335;
        CGRect rect = CGRectMake(17, 23, width, height);
        check(CGRectGetWidth(rect) == 949 && CGRectGetHeight(rect) == 335,
              "CGRect size survives guest ABI");
        CGRect missing = CGDisplayBounds(UINT32_MAX);
        check(missing.size.width == 0 && missing.size.height == 0, "unknown display");
        check(CGDisplayIDToOpenGLDisplayMask(display) == 1 &&
              CGDisplayIDToOpenGLDisplayMask(0) == 0, "display mask");
    }
    puts("MEVK display geometry passed");
    return 0;
}
