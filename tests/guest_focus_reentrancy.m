#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>

@interface FocusProbe : NSView {
@public
    NSWindow *owner;
    NSResponder *onBecome, *onResign;
    unsigned becomes, resigns;
    BOOL rejectBecome, nestedResult;
}
@end
@implementation FocusProbe
- (BOOL)becomeFirstResponder {
    if (++becomes > 10) abort();
    if (onBecome) nestedResult = [owner makeFirstResponder:onBecome];
    return !rejectBecome;
}
- (BOOL)resignFirstResponder {
    if (++resigns > 10) abort();
    if (onResign) {
        NSResponder *target = onResign;
        onResign = nil;
        nestedResult = [owner makeFirstResponder:target];
    }
    return YES;
}
@end
static void check(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "focus: %s\n", message); exit(1); }
}
int main(void) {
    @autoreleasepool {
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 200, 100)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        [window setReleasedWhenClosed:NO];
        FocusProbe *previous = [[FocusProbe alloc] initWithFrame:NSZeroRect];
        FocusProbe *candidate = [[FocusProbe alloc] initWithFrame:NSZeroRect];
        FocusProbe *other = [[FocusProbe alloc] initWithFrame:NSZeroRect];
        previous->owner = candidate->owner = other->owner = window;
        check([window makeFirstResponder:previous], "initial focus");
        previous->onBecome = candidate;
        candidate->rejectBecome = YES;
        check(![window makeFirstResponder:candidate], "candidate rejects focus");
        check(window.firstResponder == previous && previous->becomes == 2 && candidate->becomes == 1,
            "restore previous once without recursively retrying candidate");
        check(!previous->nestedResult, "pending rejected request reports failure");
        previous->onBecome = nil;
        previous->onResign = candidate;
        candidate->rejectBecome = NO;
        check([window makeFirstResponder:candidate] && !previous->nestedResult, "repeat request from resign callback is bounded");
        check(window.firstResponder == candidate, "outer request completes");
        candidate->onResign = other;
        check(![window makeFirstResponder:previous], "callback chooses a different target");
        check(window.firstResponder == other && candidate->nestedResult, "different nested request remains supported");
        check([window makeFirstResponder:nil], "clear focus");
        [window close];
        [previous release]; [candidate release]; [other release]; [window release];
    }
    puts("AppKit focus rollback and reentrant responder selection passed");
    return 0;
}
