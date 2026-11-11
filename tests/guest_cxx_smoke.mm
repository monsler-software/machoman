/* Objective-C++ ivars must be constructed before -init and remain alive
 * during -dealloc. Exercise rebased NSView ivars and an empty subclass. */
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>

static int sequence[16], count;
static void record(int value) { sequence[count++] = value; }
struct BaseMember {
    BaseMember() { record(1); }
    virtual int value() const { return 41; }
    virtual ~BaseMember() { record(4); }
};
struct DerivedMember {
    DerivedMember() { record(2); }
    virtual int value() const { return 43; }
    virtual ~DerivedMember() { record(3); }
};

@interface CxxBaseView : NSView { BaseMember member; }
- (int)baseValue;
@end
@implementation CxxBaseView
- (int)baseValue { BaseMember *ptr = &member; return ptr->value(); }
- (void)dealloc {
    if ([self baseValue] != 41 || count != 2) abort();
    [super dealloc];
}
@end
@interface CxxDerivedView : CxxBaseView { DerivedMember derived; }
- (int)derivedValue;
@end
@implementation CxxDerivedView
- (int)derivedValue { DerivedMember *ptr = &derived; return ptr->value(); }
- (void)dealloc {
    if ([self derivedValue] != 43 || count != 2) abort();
    [super dealloc];
}
@end
@interface CxxInheritedView : CxxDerivedView @end
@implementation CxxInheritedView @end

int main() {
    CxxInheritedView *view = [CxxInheritedView alloc];
    if (count != 2 || sequence[0] != 1 || sequence[1] != 2) abort();
    if ([view baseValue] != 41 || [view derivedValue] != 43) abort();
    view = [view initWithFrame:NSMakeRect(11, 22, 640, 480)];
    NSRect frame = [view frame];
    if (frame.origin.x != 11 || frame.size.width != 640) abort();
    [view release];
    if (count != 4 || sequence[2] != 3 || sequence[3] != 4) abort();
    /* +new must use the host superclass allocator before C++ ivars. */
    count = 0;
    view = [CxxInheritedView new];
    if (count != 2 || [view baseValue] != 41 || [view derivedValue] != 43) abort();
    [view release];
    if (count != 4 || sequence[2] != 3 || sequence[3] != 4) abort();
    puts("guest Objective-C++ construction and destruction passed");
}
