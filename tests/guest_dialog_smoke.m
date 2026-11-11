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
        NSWindowController *controller = [[NSWindowController alloc] initWithWindow:window];
        require(controller.window == window && controller.isWindowLoaded, "window controller ownership");
        [controller release]; [window release];
        puts("application principal class, NIBArchive menu, file panels and async cancellation passed");
    }
    return 0;
}
