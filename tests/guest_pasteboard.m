#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "pasteboard: %s\n", message); exit(1); }
}
@interface ClipboardKey : NSEvent { @public unsigned short code; }
@end
@implementation ClipboardKey
- (NSEventType)type { return NSEventTypeKeyDown; }
- (unsigned short)keyCode { return code; }
- (NSEventModifierFlags)modifierFlags { return NSEventModifierFlagCommand; }
@end
@interface LazyOwner : NSObject { @public unsigned calls, lost; }
@end
@implementation LazyOwner
- (void)pasteboard:(NSPasteboard *)p provideDataForType:(NSPasteboardType)t {
    ++calls; [p setString:@"lazy text" forType:t];
}
- (void)pasteboardChangedOwner:(NSPasteboard *)p { (void)p; ++lost; }
@end
int main(void) {
    @autoreleasepool {
        NSPasteboard *general = NSPasteboard.generalPasteboard;
        check(general == NSPasteboard.generalPasteboard && general == [NSPasteboard pasteboardWithName:NSPasteboardNameGeneral], "general singleton and name constant");
        check([NSPasteboardTypeString isEqualToString:@"public.utf8-plain-text"], "real UTI constant");
        NSPasteboard *a = [NSPasteboard pasteboardWithUniqueName], *b = [NSPasteboard pasteboardWithUniqueName];
        check(a != b && [NSPasteboard pasteboardWithName:a.name] == a, "named pasteboard identity");
        NSInteger count = [a clearContents];
        check(a.types.count == 0 && a.pasteboardItems.count == 0 && [a stringForType:NSPasteboardTypeString] == nil, "empty pasteboard");
        check([a setString:@"Привет 😀\nsecond line" forType:NSPasteboardTypeString], "write unicode");
        check(a.changeCount == count && [[a stringForType:NSStringPboardType] isEqualToString:@"Привет 😀\nsecond line"], "legacy string alias and changeCount");
        check([[a availableTypeFromArray:@[@"missing", NSStringPboardType]] isEqualToString:NSStringPboardType], "type preference returns requested alias");
        check([b stringForType:NSPasteboardTypeString] == nil, "named pasteboards isolated");
        const char binary[] = {'a', 0, 'b', (char)0xff};
        NSData *data = [NSData dataWithBytes:binary length:sizeof binary];
        check([a setData:data forType:@"org.machoman.binary"] && [[a dataForType:@"org.machoman.binary"] isEqualToData:data], "binary payload with NUL");
        check([a setData:[NSData data] forType:@"org.machoman.empty"] && [a dataForType:@"org.machoman.empty"] != nil && [a dataForType:@"org.machoman.empty"].length == 0, "empty data differs from absent type");
        NSDictionary *plist = [NSDictionary dictionaryWithObject:@[@"one", @2] forKey:@"value"];
        check([a setPropertyList:plist forType:@"org.machoman.plist"], "serialize property list");
        NSDictionary *decoded = [a propertyListForType:@"org.machoman.plist"];
        check(decoded != nil && [decoded[@"value"][0] isEqual:@"one"] && [decoded[@"value"][1] intValue] == 2, "property list round trip");
        check([a setPropertyList:@"custom string" forType:@"org.machoman.plist-string"] && [[a propertyListForType:@"org.machoman.plist-string"] isEqual:@"custom string"], "property list string round trip");
        NSPasteboardItem *item = [NSPasteboardItem new];
        [item setString:@"first" forType:NSPasteboardTypeString];
        [item setData:data forType:NSPasteboardTypePNG];
        check([[item stringForType:NSStringPboardType] isEqualToString:@"first"] && item.types.count == 2, "item data and aliases");
        [a clearContents];
        check([a writeObjects:@[item, @"second"]] && a.pasteboardItems.count == 2, "multiple writers");
        [item setString:@"mutated" forType:NSPasteboardTypeString];
        check([[a stringForType:NSPasteboardTypeString] isEqualToString:@"first"], "pasteboard owns writer snapshot");
        NSArray *strings = [a readObjectsForClasses:@[NSString.class] options:@{}];
        check(strings.count == 2 && [strings[0] isEqual:@"first"] && [strings[1] isEqual:@"second"], "read objects");
        check([a canReadObjectForClasses:@[NSString.class] options:@{}], "can read strings");
        check(![a writeObjects:@[[NSObject new]]] && a.pasteboardItems.count == 2, "unsupported writer fails without partial write");
        LazyOwner *owner = [LazyOwner new];
        [b declareTypes:@[NSPasteboardTypeString] owner:owner];
        check([[b stringForType:NSPasteboardTypeString] isEqual:@"lazy text"] && owner->calls == 1, "guest lazy owner callback");
        [b clearContents]; check(owner->lost == 1, "owner change callback");
        check([a clearContents] > count && a.types.count == 0, "change count advances on clearing");
        [general clearContents];
        NSTextView *editor = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 100, 50)];
        editor.string = @"a😀b"; editor.selectedRange = NSMakeRange(1, 2);
        [editor copy:nil];
        check([[general stringForType:NSPasteboardTypeString] isEqual:@"😀"] && [editor.string isEqual:@"a😀b"], "copy UTF-16 selection");
        [editor cut:nil]; check([editor.string isEqual:@"ab"] && editor.selectedRange.location == 1, "cut selection");
        [editor paste:nil]; check([editor.string isEqual:@"a😀b"] && editor.selectedRange.location == 3, "paste into selection");
        editor.editable = NO; [editor selectAll:nil]; [editor cut:nil]; [editor paste:nil];
        check([editor.string isEqual:@"a😀b"], "read-only editing");
        [editor copy:nil]; check([[general stringForType:NSPasteboardTypeString] isEqual:@"a😀b"], "read-only copy");
        editor.editable = YES;
        ClipboardKey *key = [ClipboardKey new]; key->code = 0; [editor keyDown:key];
        check(editor.selectedRange.length == 4, "Command-A selection");
        key->code = 7; [editor keyDown:key]; check(editor.string.length == 0, "Command-A and Command-X");
        key->code = 9; [editor keyDown:key]; check([editor.string isEqual:@"a😀b"], "Command-V");
        key->code = 8; [editor keyDown:key];
        [key release]; [editor release]; [owner release]; [item release];
    }
    puts("NSPasteboard typed data, named boards, writers and clipboard editing passed");
    return 0;
}
