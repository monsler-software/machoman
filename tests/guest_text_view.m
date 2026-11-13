#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>

static void check(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "text view: %s\n", message); exit(1); }
}
@interface KeyProbe : NSEvent { @public NSEventType probeType; unsigned short code; NSEventModifierFlags flags; }
@end
@implementation KeyProbe
- (NSEventType)type { return probeType; }
- (NSString *)characters { return @"z"; }
- (unsigned short)keyCode { return code; }
- (NSEventModifierFlags)modifierFlags { return flags; }
@end
@interface PointerProbe : NSView { @public unsigned downs, ups, moves; }
@end
@implementation PointerProbe
- (void)mouseDown:(NSEvent *)event { ++downs; }
- (void)mouseUp:(NSEvent *)event { ++ups; }
- (void)mouseMoved:(NSEvent *)event { ++moves; }
@end
@interface TextDelegate : NSObject <NSTextViewDelegate> { @public unsigned selections, changes; NSString *lastText; BOOL marked; }
@end
@implementation TextDelegate
- (void)textViewDidChangeSelection:(NSNotification *)notification {
    ++selections;
    NSTextView *view = notification.object;
    [lastText release]; lastText = [view.string copy]; marked = view.hasMarkedText;
}
- (void)textDidChange:(NSNotification *)notification { ++changes; }
- (void)dealloc { [lastText release]; [super dealloc]; }
@end
@interface FieldProbe : NSTextView { @public unsigned insertions; }
@end
@implementation FieldProbe
- (void)insertText:(id)text replacementRange:(NSRange)range {
    ++insertions;
    [super insertText:text replacementRange:range];
}
@end
int main(void) {
    @autoreleasepool {
        FieldProbe *editor = [[FieldProbe alloc] initWithFrame:NSMakeRect(5, 10, 150, 90)];
        check([editor isKindOfClass:[NSView class]] && editor.frame.size.width == 150, "view superclass and frame ABI");
        check([editor.string isEqualToString:@""] && editor.selectedRange.location == 0 &&
            editor.selectedRange.length == 0 && editor.markedRange.location == NSNotFound &&
            editor.markedRange.length == 0 && !editor.hasMarkedText, "initial string and range return ABI");
        [editor setFieldEditor:YES];
        check(editor.isFieldEditor, "field editor state");
        editor.string = @"abc";
        editor.selectedRange = NSMakeRange(1, 2);
        [editor insertText:@"x" replacementRange:NSMakeRange(NSNotFound, 0)];
        check([editor.string isEqualToString:@"ax"] && editor.selectedRange.location == 2, "selection replacement");
        KeyProbe *event = [KeyProbe new];
        event->probeType = NSEventTypeKeyDown; event->code = 6;
        [editor keyDown:event];
        check(editor->insertions == 2 && [editor.string isEqualToString:@"axz"], "keyDown invokes guest insertText override and super");
        TextDelegate *delegate = [TextDelegate new];
        editor.delegate = delegate;
        [editor keyDown:event];
        check(delegate->selections == 1 && delegate->changes == 1 &&
            [delegate->lastText isEqualToString:@"axzz"] && !delegate->marked,
            "each keystroke notifies the delegate with committed text before keyDown returns");
        event->probeType = NSEventTypeFlagsChanged; event->code = 57;
        [editor interpretKeyEvents:@[event]];
        event->probeType = NSEventTypeKeyDown; event->code = 59;
        [editor interpretKeyEvents:@[event]];
        event->code = 6; event->flags = NSEventModifierFlagControl;
        [editor interpretKeyEvents:@[event]];
        event->flags = NSEventModifierFlagCommand;
        [editor interpretKeyEvents:@[event]];
        event->flags = 0; event->code = 48;
        [editor interpretKeyEvents:@[event]];
        check([editor.string isEqualToString:@"axzz"] && delegate->changes == 1,
            "modifier events, Control/Command shortcuts and field-editor Tab do not insert text");
        editor.string = @"axz";
        [editor replaceCharactersInRange:NSMakeRange(0, editor.string.length) withString:@""];
        check(editor.string.length == 0 && editor.selectedRange.location == 0,
            "composition cleanup replaces the NSText buffer through its NSRange ABI");
        editor.string = @"axz";
        editor.selectedRange = NSMakeRange(3, 0);
        [editor setMarkedText:@"你好" selectedRange:NSMakeRange(1, 1) replacementRange:NSMakeRange(NSNotFound, 0)];
        check(editor.hasMarkedText && editor.markedRange.location == 3 && editor.markedRange.length == 2 &&
            editor.selectedRange.location == 4 && editor.selectedRange.length == 1, "marked text and selected ranges");
        check(delegate->marked && [delegate->lastText isEqualToString:@"axz你好"], "composition callback sees final marked state");
        [editor insertText:@"!" replacementRange:NSMakeRange(NSNotFound, 0)];
        check([editor.string isEqualToString:@"axz!"] && !editor.hasMarkedText, "committing marked text");
        editor.string = @"😀";
        editor.selectedRange = NSMakeRange(2, 0);
        [editor deleteBackward:nil];
        check(editor.string.length == 0 && editor.selectedRange.location == 0, "delete UTF-16 surrogate pair");
        editor.string = @"ab";
        editor.selectedRange = NSMakeRange(0, 1);
        [editor setMarkedText:@"x" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        [editor setMarkedText:@"yz" selectedRange:NSMakeRange(2, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        check([editor.string isEqualToString:@"yzb"] && editor.markedRange.length == 2, "replace existing composition");
        [editor unmarkText];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 300, 200)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
        PointerProbe *render = [[PointerProbe alloc] initWithFrame:root.bounds];
        NSTextView *zeroEditor = [[NSTextView alloc] initWithFrame:NSZeroRect];
        [root addSubview:render]; [render addSubview:zeroEditor]; window.contentView = root;
        [window makeFirstResponder:zeroEditor];
        check([root hitTest:NSMakePoint(90, 70)] == render,
            "zero-sized field editor does not intercept pointer hit testing");
        event->probeType = NSEventTypeMouseMoved; [window sendEvent:event];
        event->probeType = NSEventTypeLeftMouseDown; [window sendEvent:event];
        event->probeType = NSEventTypeLeftMouseUp; [window sendEvent:event];
        check(render->moves == 1 && render->downs == 1 && render->ups == 1,
            "window sends pointer events to rendering view while field editor has keyboard focus");
        NSStackView *container = [NSStackView stackViewWithViews:@[[NSView new]]];
        NSBox *box = [NSBox new];
        [box addSubview:container]; [root addSubview:box];
        check([container isKindOfClass:NSView.class] && container.window == window &&
            container.arrangedSubviews.count == 1 && ((NSView *)container.arrangedSubviews[0]).window == window,
            "stack containers have a real view payload and recursively inherit window membership");
        [window makeFirstResponder:nil]; [zeroEditor release]; [render release]; [root release];
        [box release]; [window release];
        editor.delegate = nil;
        [delegate release]; [event release]; [editor release];
    }
    puts("NSTextView field editor, guest text callbacks and UTF-16 range ABI passed");
    return 0;
}
