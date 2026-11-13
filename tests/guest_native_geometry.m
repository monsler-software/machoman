#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>

/* Keep calls out of line so clang cannot replace the exported functions
 * with its inline NSGeometry implementations. */
extern BOOL exported_point_in_rect(NSPoint, NSRect) __asm__("_NSPointInRect");
static BOOL (*volatile point_in_rect)(NSPoint, NSRect) = exported_point_in_rect;

static NSRect drawn_rect;
static NSPoint hit_point;
static unsigned draw_count;
@interface GeometryView : NSView
- (NSRect)shiftedRect:(NSRect)rect by:(CGFloat)delta;
@end
@implementation GeometryView
- (NSRect)frame { return [super frame]; }
- (void)drawRect:(NSRect)rect { drawn_rect = rect; ++draw_count; }
- (NSView *)hitTest:(NSPoint)point { hit_point = point; return self; }
- (NSRect)shiftedRect:(NSRect)rect by:(CGFloat)delta {
    rect.origin.x += delta;
    rect.origin.y -= delta;
    return rect;
}
@end
static void check_rect(NSRect r, double x, double y, double w, double h) {
    if (r.origin.x != x || r.origin.y != y || r.size.width != w || r.size.height != h) {
        fprintf(stderr, "geometry got {%g,%g,%g,%g}, expected {%g,%g,%g,%g}\n",
                r.origin.x, r.origin.y, r.size.width, r.size.height, x, y, w, h);
        exit(1);
    }
}
int main(void) {
    @autoreleasepool {
        if (!point_in_rect(NSMakePoint(50, 60), NSMakeRect(10, 20, 80, 90)) ||
            point_in_rect(NSMakePoint(90, 60), NSMakeRect(10, 20, 80, 90)) ||
            point_in_rect(NSMakePoint(50, 110), NSMakeRect(10, 20, 80, 90)) ||
            point_in_rect(NSMakePoint(0, 0), NSZeroRect)) abort();
        GeometryView *v = [[GeometryView alloc] initWithFrame:NSMakeRect(11, 13, 120, 80)];
        check_rect([v frame], 11, 13, 120, 80);
        [v setFrame:NSMakeRect(17, 19, 140, 90)];
        check_rect([v frame], 17, 19, 140, 90);
        [v setFrameOrigin:NSMakePoint(23, 29)];
        [v setFrameSize:NSMakeSize(160, 100)];
        check_rect([v frame], 23, 29, 160, 100);
        check_rect([v shiftedRect:NSMakeRect(1, 2, 3, 4) by:5], 6, -3, 3, 4);
        check_rect([v convertRectToBacking:NSMakeRect(7, 8, 9, 10)], 7, 8, 9, 10);
        [v display];
        if (draw_count != 1) abort();
        check_rect(drawn_rect, 0, 0, 160, 100);
        NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
        [root addSubview:v];
        if ([root hitTest:NSMakePoint(50, 60)] != v || hit_point.x != 50 || hit_point.y != 60) abort();
        NSRange range = [@"abcdef" rangeOfString:@"cd"];
        if (range.location != 2 || range.length != 2) abort();
        NSString *part = [@"abcdef" substringWithRange:NSMakeRange(1, 3)];
        if (![part isEqualToString:@"bcd"]) abort();
        NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
        if (version.majorVersion <= 0 || version.minorVersion < 0 || version.patchVersion < 0) abort();
    }
    puts("Darwin Cocoa aggregate arguments, returns and superclass dispatch passed");
    return 0;
}
