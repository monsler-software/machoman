#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include "nib_archive_fixture.h"
static void require(BOOL ok, const char *message) { if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); } }
static int launches = 0;
@interface DialogApplication : NSApplication @end
@implementation DialogApplication
- (void)finishLaunching {
    [super finishLaunching];
    ++launches;
    require([[self.mainMenu title] isEqualToString:@"Menu from NIBArchive"], "main nib installed menu");
    [self stop:nil];
}
@end
@interface ChoiceTarget : NSObject { @public int clicks; }
- (void)changed:(id)sender;
@end
@implementation ChoiceTarget
- (void)changed:(id)sender { ++clicks; }
@end
int main(int argc, const char **argv) {
    @autoreleasepool {
        NSString *resources = [NSBundle mainBundle].resourcePath;
        require([[NSData dataWithBytes:nib_archive_fixture length:sizeof nib_archive_fixture]
            writeToFile:[resources stringByAppendingPathComponent:@"MainMenu.nib"] atomically:NO], "write nib fixture");
        require(NSApplicationMain(argc, argv) == 0 && launches == 1, "principal application entry point");
        require([NSApp isKindOfClass:[DialogApplication class]], "principal subclass instantiated");
        [NSApp finishLaunching]; require(launches == 2, "subclass can call finishLaunching again");
        NSOpenPanel *open = [NSOpenPanel openPanel]; NSSavePanel *save = [NSSavePanel savePanel];
        require(open && save && open != (id)save, "panel factories");
        open.canChooseFiles = NO; open.canChooseDirectories = YES; open.allowsMultipleSelection = YES;
        open.directoryURL = [NSURL fileURLWithPath:@"/tmp"];
        save.allowedFileTypes = @[@"txt"]; save.nameFieldStringValue = @"test.txt";
        require(!open.canChooseFiles && open.canChooseDirectories && open.allowsMultipleSelection, "open options");
        require([open.directoryURL.path isEqualToString:@"/tmp"] && [[save.allowedFileTypes objectAtIndex:0] isEqualToString:@"txt"], "directory and filter state");
        require([save runModal] == NSModalResponseCancel && save.URL == nil, "headless cancels without success");
        __block int completions = 0;
        [open beginWithCompletionHandler:^(NSModalResponse response) { require(response == NSModalResponseCancel, "async cancel"); ++completions; }];
        require(completions == 0, "completion deferred past begin");
        [NSApp run]; require(completions == 0, "stopped run does not execute completion");
        // A modal wait services all native requests and invokes the copied block.
        [save runModal]; require(completions == 1 && open.URLs.count == 0, "completion once on UI thread");
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 100, 100) styleMask:0 backing:NSBackingStoreBuffered defer:NO];
        NSButton *button = [[NSButton alloc] initWithFrame:NSMakeRect(10, 10, 66, 24)];
        button.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        button.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:button];
        [window.contentView setFrameSize:NSMakeSize(480, 320)];
        require(NSEqualRects(button.frame, NSMakeRect(10, 10, 66, 24)), "constraint-managed control skips legacy autoresizing");
        button.translatesAutoresizingMaskIntoConstraints = YES;
        [window.contentView setFrameSize:NSMakeSize(580, 420)];
        require(button.frame.size.width == 166 && button.frame.size.height == 124, "legacy springs still resize controls");
        [button release];
        NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(30, 40, 160, 24)];
        [window.contentView addSubview:popup];
        [popup addItemsWithTitles:@[@"physical", @"logical"]];
        require(popup.menu.numberOfItems == 2 && popup.selectedItem == [popup itemAtIndex:0], "popup owns real menu items");
        NSMenuItem *logical = [popup itemAtIndex:1]; logical.tag = 42; logical.representedObject = @"logical value";
        [popup insertItemWithTitle:@"other" atIndex:0];
        require(popup.indexOfSelectedItem == 1, "insertion preserves selected item identity");
        require([popup selectItemWithTag:42] && [popup.selectedItem.representedObject isEqual:@"logical value"], "selection by item tag preserves represented value");
        require(![popup selectItemWithTag:99] && [popup indexOfItemWithTag:99] == -1, "absent tag does not change selection");
        logical.title = @"renamed";
        require([popup.titleOfSelectedItem isEqual:@"renamed"], "popup reflects changes to menu item titles");
        [popup removeItemAtIndex:0];
        require(popup.selectedItem == logical && popup.indexOfSelectedItem == 1, "removal preserves selection identity");
        [popup removeAllItems];
        for (int i = 0; i < 25; ++i) [popup addItemWithTitle:[NSString stringWithFormat:@"Item %d", i]];
        ChoiceTarget *target = [ChoiceTarget new]; popup.target = target; popup.action = @selector(changed:);
        [popup mouseDown:nil];
        require(window.childWindows.count == 1, "opening popup creates child menu");
        NSWindow *menuWindow = [[window.childWindows objectAtIndex:0] retain];
        NSView *list = [(NSScrollView *)menuWindow.contentView documentView];
        require(list.subviews.count == 25, "long popup keeps all items in a scrollable list");
        [(NSButton *)[list.subviews objectAtIndex:24] performClick:nil];
        require(popup.indexOfSelectedItem == 24 && target->clicks == 1 && !menuWindow.visible && window.childWindows.count == 0,
                "choosing popup row updates control, sends one action and dismisses menu");
        [menuWindow release];
        [target release]; [popup release];
        NSDocumentController *documents = [NSDocumentController sharedDocumentController];
        [documents clearRecentDocuments:nil];
        NSURL *first = [NSURL fileURLWithPath:@"/tmp/scan one"], *second = [NSURL fileURLWithPath:@"/tmp/scan two"];
        [documents noteNewRecentDocumentURL:first]; [documents noteNewRecentDocumentURL:second];
        [documents noteNewRecentDocumentURL:first];
        require(documents.recentDocumentURLs.count == 2 && [[documents.recentDocumentURLs objectAtIndex:0].path isEqual:@"/tmp/scan one"],
                "recent scans are ordered and deduplicated");
        NSDocumentController *loaded = [[NSDocumentController alloc] init];
        require(loaded.recentDocumentURLs.count == 2 && [[loaded.recentDocumentURLs objectAtIndex:1].path isEqual:@"/tmp/scan two"],
                "document history reloads URL strings from preferences");
        [loaded release]; [documents clearRecentDocuments:nil];
        require(documents.recentDocumentURLs.count == 0, "clear history removes recent scans");
        NSWindowController *controller = [[NSWindowController alloc] initWithWindow:window];
        require(controller.window == window && controller.isWindowLoaded, "window controller ownership");
        [controller release]; [window release];
        puts("application principal class, NIBArchive menu, file panels and async cancellation passed");
    }
    return 0;
}
