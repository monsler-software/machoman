#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "KVO regression: %s\n", message); abort(); }
}
static char context_a, context_b;
static unsigned observers_destroyed;
static unsigned children_destroyed;
@interface LifetimeChildWindow : NSWindow @end
@implementation LifetimeChildWindow
- (void)dealloc { ++children_destroyed; [super dealloc]; }
@end
@interface WindowObserver : NSObject {
@public
    unsigned initial, prior, changes_a, changes_b;
    id expected_old, expected_new;
    BOOL remove_self;
    unsigned occlusion_changes;
    NSWindowOcclusionState expected_occlusion;
}
- (void)occlusionChanged:(NSNotification *)notification;
@end
@implementation WindowObserver
- (void)occlusionChanged:(NSNotification *)notification {
    ++occlusion_changes;
    check([(NSWindow *)[notification object] occlusionState] == expected_occlusion, "occlusion state precedes notification");
}
- (void)observeValueForKeyPath:(NSString *)path ofObject:(id)object
                       change:(NSDictionary *)change context:(void *)context {
    check([path isEqual:@"window"] && [change[NSKeyValueChangeKindKey] intValue] == NSKeyValueChangeSetting,
          "key and change kind");
    id old = change[NSKeyValueChangeOldKey], value = change[NSKeyValueChangeNewKey];
    if ([change[NSKeyValueChangeNotificationIsPriorKey] boolValue]) {
        ++prior;
        check(old == expected_old && !value && [object window] == (expected_old == [NSNull null] ? nil : expected_old),
              "prior notification precedes mutation");
    } else if (!old) {
        ++initial;
        check(value == [NSNull null], "initial notification boxes nil");
    } else {
        check(old == expected_old && value == expected_new, "old/new object values and nil boxing");
        check([object window] == (expected_new == [NSNull null] ? nil : expected_new), "new value is already visible");
        if (context == &context_a) ++changes_a;
        else if (context == &context_b) ++changes_b;
        else abort();
        if (remove_self) [object removeObserver:self forKeyPath:path context:context];
    }
}
- (void)dealloc { ++observers_destroyed; [super dealloc]; }
@end

int main(void) {
    @autoreleasepool {
        NSRect frame = NSMakeRect(0, 0, 100, 100);
        NSWindow *first = [[NSWindow alloc] initWithContentRect:frame styleMask:0 backing:NSBackingStoreBuffered defer:NO];
        NSWindow *second = [[NSWindow alloc] initWithContentRect:frame styleMask:0 backing:NSBackingStoreBuffered defer:NO];
        NSView *parent = [[NSView alloc] initWithFrame:frame];
        NSView *view = [[NSView alloc] initWithFrame:frame];
        [parent addSubview:view];
        WindowObserver *observer = [[WindowObserver alloc] init];
        @autoreleasepool {
            NSMutableString *subrole = [NSMutableString stringWithString:@"AXDialog"];
            [first setAccessibilitySubrole:subrole];
            [subrole appendString:@"-changed"];
        }
        check([[first accessibilitySubrole] isEqual:@"AXDialog"], "accessibility subrole copies string contents");
        [first setAccessibilitySubrole:nil];
        check(![first accessibilitySubrole], "accessibility subrole can be cleared");
        @autoreleasepool {
            NSURL *url = [[NSURL alloc] initWithString:@"file:///tmp/placement.als"];
            first.representedURL = url;
            [url release];
        }
        check([first.representedURL.absoluteString isEqual:@"file:///tmp/placement.als"], "window retains represented URL");
        first.representedURL = nil;
        check(!first.representedURL, "represented URL can be cleared");
        check(first.occlusionState == 0, "initially hidden window");
        [[NSNotificationCenter defaultCenter] addObserver:observer selector:@selector(occlusionChanged:)
            name:NSWindowDidChangeOcclusionStateNotification object:first];
        observer->expected_occlusion = NSWindowOcclusionStateVisible;
        [first orderFront:nil]; [first orderFront:nil];
        check(observer->occlusion_changes == 1, "visible transition fires once");
        observer->expected_occlusion = 0;
        [first orderOut:nil]; [first orderOut:nil];
        check(observer->occlusion_changes == 2, "hidden transition fires once");
        observer->expected_occlusion = NSWindowOcclusionStateVisible; [first orderFront:nil];
        observer->expected_occlusion = 0; [first close];
        check(observer->occlusion_changes == 4, "close changes occlusion state");
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        LifetimeChildWindow *child = [[LifetimeChildWindow alloc] initWithContentRect:frame
            styleMask:0 backing:NSBackingStoreBuffered defer:NO];
        [child setAlphaValue:0];
        check(child.alphaValue == 0, "transparent child alpha is preserved");
        @autoreleasepool {
            [first addChildWindow:child ordered:NSWindowAbove];
            NSArray *snapshot = [[first childWindows] copy];
            [first addChildWindow:child ordered:NSWindowAbove];
            check(child.parentWindow == first && first.childWindows.count == 1, "child relationship and duplicate ordering");
            [child addChildWindow:first ordered:NSWindowAbove];
            check(!first.parentWindow, "parent cycle is rejected");
            [second addChildWindow:child ordered:NSWindowAbove];
            check(child.parentWindow == second && first.childWindows.count == 0 && snapshot.count == 1, "reparent and independent child snapshot");
            [snapshot release];
            [second orderFront:nil]; check(child.visible, "child follows parent order-in");
            [second orderOut:nil]; check(!child.visible, "child follows parent order-out");
            [second removeChildWindow:child]; check(!child.parentWindow, "child removal clears weak parent");
            [second addChildWindow:child ordered:NSWindowAbove];
        }
        [child release];
        check(!children_destroyed, "parent retains child window");
        [view addObserver:observer forKeyPath:@"window"
                  options:NSKeyValueObservingOptionNew | NSKeyValueObservingOptionOld |
                          NSKeyValueObservingOptionInitial | NSKeyValueObservingOptionPrior context:&context_a];
        [view addObserver:observer forKeyPath:@"window"
                  options:NSKeyValueObservingOptionNew | NSKeyValueObservingOptionOld context:&context_b];
        check(observer->initial == 1, "initial callback fires once");
        observer->expected_old = [NSNull null]; observer->expected_new = first;
        [first setContentView:parent];
        check(observer->changes_a == 1 && observer->changes_b == 1 && observer->prior == 1, "nested view receives window attachment");
        check([view valueForKey:@"window"] == first, "object-valued KVC");
        [first setContentView:parent];
        check(observer->changes_a == 1, "unchanged attachment does not notify");
        [view removeObserver:observer forKeyPath:@"window" context:&context_b];
        observer->expected_old = first; observer->expected_new = [NSNull null];
        [view removeFromSuperview];
        check(observer->changes_a == 2 && observer->changes_b == 1 && observer->prior == 2, "context removal and detach");
        observer->expected_old = [NSNull null]; observer->expected_new = second;
        observer->remove_self = YES;
        [[second contentView] addSubview:view];
        check(observer->changes_a == 3, "callback can remove its own registration");
        [view removeFromSuperview];
        check(observer->changes_a == 3, "removed registration stays silent");
        [view addObserver:observer forKeyPath:@"window" options:NSKeyValueObservingOptionNew context:&context_a];
        [view removeObserver:observer forKeyPath:@"window"];
        [[second contentView] addSubview:view];
        check(observer->changes_a == 3, "plain removal");
        [view addObserver:observer forKeyPath:@"window" options:NSKeyValueObservingOptionNew context:&context_a];
        [observer release];
        check(observers_destroyed == 1, "observation does not retain observer");
        [view removeFromSuperview]; // Expired weak observer must be skipped.
        [view release]; [parent release]; [first release]; [second release];
    }
    check(children_destroyed == 1, "parent destruction releases child window");
    puts("KVO window attachment, old/new/prior/initial, contexts, reentrancy and weak lifetime passed");
}
