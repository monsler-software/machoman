#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include "nib_archive_fixture.h"
#include "nib_lifetime_fixture.h"
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
@interface ChoiceInputEvent : NSEvent { @public NSEventType kind; unsigned short code; NSWindow *host; }
@end
@implementation ChoiceInputEvent
- (NSEventType)type { return kind; }
- (unsigned short)keyCode { return code; }
- (NSWindow *)window { return host; }
@end
@interface ChoiceMouseButton : NSButton { @public int clicks; }
@end
@implementation ChoiceMouseButton
- (void)performClick:(id)sender { ++clicks; }
@end
static int nibOwnerDeallocations = 0;
@interface NibLifetimeOwner : NSObject @end
@implementation NibLifetimeOwner
- (void)dealloc { ++nibOwnerDeallocations; [super dealloc]; }
@end
@interface RankingTableDelegate : NSObject { @public int changes; NSInteger observedRow; }
@end
@implementation RankingTableDelegate
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { return 40; }
- (id)tableView:(NSTableView *)table objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row { return @"raw value"; }
- (void)tableView:(NSTableView *)table willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] initWithString:[NSString stringWithFormat:@"Type %ld", (long)row]];
    [text addAttribute:NSForegroundColorAttributeName value:[NSColor grayColor] range:NSMakeRange(0,text.length)];
    [cell setAttributedStringValue:text]; [text release];
}
- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    ++changes; observedRow = [(NSTableView *)notification.object selectedRow];
}
@end
int main(int argc, const char **argv) {
    @autoreleasepool {
        char *previousLocale = strdup(setlocale(LC_NUMERIC,NULL));
        for (int i=0;i<2;++i) {
            if (!setlocale(LC_NUMERIC,i ? "ru_RU.utf8" : "C")) continue;
            NSData *xml = [NSPropertyListSerialization dataWithPropertyList:@{@"gradient":[NSNumber numberWithDouble:.5]}
                format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
            require(xml != nil,"serialize a numeric plist");
            NSString *serialized = [[NSString alloc] initWithData:xml encoding:NSUTF8StringEncoding];
            require([serialized rangeOfString:@"<real>0.5</real>"].location != NSNotFound,
                    "plist real serialization ignores the numeric locale");
            NSDictionary *parsed = [NSPropertyListSerialization propertyListWithData:xml options:0 format:NULL error:NULL];
            require([[parsed objectForKey:@"gradient"] doubleValue] == .5,
                    "plist real roundtrip preserves a gradient in every locale");
            [serialized release];
        }
        setlocale(LC_NUMERIC,previousLocale); free(previousLocale);
        NSString *resources = [NSBundle mainBundle].resourcePath;
        require([[NSData dataWithBytes:nib_archive_fixture length:sizeof nib_archive_fixture]
            writeToFile:[resources stringByAppendingPathComponent:@"MainMenu.nib"] atomically:NO], "write nib fixture");
        require(NSApplicationMain(argc, argv) == 0 && launches == 1, "principal application entry point");
        require([NSApp isKindOfClass:[DialogApplication class]], "principal subclass instantiated");
        [NSApp finishLaunching]; require(launches == 2, "subclass can call finishLaunching again");
        require([[NSData dataWithBytes:nib_lifetime_fixture length:sizeof nib_lifetime_fixture]
            writeToFile:[resources stringByAppendingPathComponent:@"LifetimeTable.nib"] atomically:NO], "write nested nib fixture");
        NibLifetimeOwner *nibOwner = [NibLifetimeOwner new];
        NSTableView *lifetimeTable = nil;
        NSTableCellView *firstCell = nil, *secondCell = nil;
        @autoreleasepool {
            NSArray *top = nil;
            require([[NSBundle mainBundle] loadNibNamed:@"LifetimeTable" owner:nibOwner topLevelObjects:&top], "load table nib");
            require(top.count == 1 && [[top objectAtIndex:0] isKindOfClass:[NSTableView class]],
                    "top-level objects exclude parents, owner and nil placeholders");
            lifetimeTable = [top objectAtIndex:0];
            firstCell = [[lifetimeTable makeViewWithIdentifier:@"LifetimeCell" owner:nibOwner] retain];
            secondCell = [[lifetimeTable makeViewWithIdentifier:@"LifetimeCell" owner:nibOwner] retain];
            require(firstCell && secondCell && firstCell != secondCell, "table templates create distinct row views");
            [(NSTextField *)[firstCell.subviews objectAtIndex:0] setStringValue:@"first row"];
            [(NSTextField *)[secondCell.subviews objectAtIndex:0] setStringValue:@"second row"];
        }
        require(nibOwnerDeallocations == 0 && nibOwner.retainCount == 1,
                "draining template temporaries preserves File's Owner ownership");
        require([firstCell isKindOfClass:[NSTableCellView class]] && [secondCell isKindOfClass:[NSTableCellView class]] &&
                firstCell.subviews.count == 1 && secondCell.subviews.count == 1,
                "retained table cells and their subviews survive pool drain");
        require([[(NSTextField *)[firstCell.subviews objectAtIndex:0] stringValue] isEqual:@"first row"] &&
                [[(NSTextField *)[secondCell.subviews objectAtIndex:0] stringValue] isEqual:@"second row"],
                "row contents survive template cleanup independently");
        [firstCell release]; [secondCell release]; [lifetimeTable release]; [nibOwner release];
        require(nibOwnerDeallocations == 1, "File's Owner deallocates exactly once when released by its caller");
        NSIndexSet *largeIndexes = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(1ULL << 40, 1ULL << 35)];
        require(largeIndexes.count == (1ULL << 35) && largeIndexes.firstIndex == (1ULL << 40) &&
                largeIndexes.lastIndex == (1ULL << 40) + (1ULL << 35) - 1 &&
                [largeIndexes indexGreaterThanIndex:largeIndexes.lastIndex] == NSNotFound,
                "index sets retain 64-bit ranges and return NSNotFound after the last index");
        NSMutableIndexSet *indexes = [NSMutableIndexSet indexSetWithIndexesInRange:NSMakeRange(2, 4)];
        [indexes addIndexesInRange:NSMakeRange(5, 3)]; [indexes removeIndexesInRange:NSMakeRange(3, 2)];
        require(indexes.count == 4 && indexes.firstIndex == 2 && indexes.lastIndex == 7 &&
                [indexes indexGreaterThanIndex:2] == 5 && [indexes indexLessThanIndex:5] == 2,
                "index set updates preserve distinct indexes and gaps");
        NSIndexSet *snapshot = [indexes copy]; [indexes addIndex:9];
        require(![snapshot containsIndex:9] && [indexes containsIndex:9], "immutable index set copies survive source mutation");
        NSIndexSet *equalSnapshot = [[snapshot mutableCopy] autorelease];
        require([snapshot isEqual:equalSnapshot] && snapshot.hash == equalSnapshot.hash,
                "equal immutable and mutable index sets have the same hash");
        [snapshot release];
        require([(id)kUTTypeIdentifierKey isEqual:@"UTTypeIdentifier"] &&
                [(id)kUTTypeDescriptionKey isEqual:@"UTTypeDescription"] &&
                [(id)kUTTypeConformsToKey isEqual:@"UTTypeConformsTo"], "UTI declaration keys match Info.plist keys");
        NSDictionary *movieDeclaration = nil;
        @autoreleasepool {
            movieDeclaration = (id)UTTypeCopyDeclaration(CFSTR("public.movie"));
        }
        require([[movieDeclaration objectForKey:(id)kUTTypeIdentifierKey] isEqual:@"public.movie"] &&
                [[movieDeclaration objectForKey:(id)kUTTypeDescriptionKey] isEqual:@"Movie"] &&
                [[movieDeclaration objectForKey:(id)kUTTypeConformsToKey] containsObject:@"public.audiovisual-content"],
                "copied UTI declarations preserve their fields after pool drain");
        [movieDeclaration release];
        for (NSString *type in @[@"public.audio", @"public.image", @"com.apple.application", @"com.apple.package",
                                  @"public.archive", @"public.executable", @"public.data", @"public.mp3"]) {
            NSDictionary *declaration = (id)UTTypeCopyDeclaration((CFStringRef)type);
            require(declaration != nil && [[declaration objectForKey:(id)kUTTypeConformsToKey] isKindOfClass:[NSArray class]],
                    "ranking categories and media types have usable parent declarations");
            [declaration release];
        }
        require(UTTypeCopyDeclaration(CFSTR("org.machoman.undeclared-type")) == NULL,
                "undeclared UTI returns no fabricated declaration");
        RankingTableDelegate *rankingDelegate = [RankingTableDelegate new];
        NSTableView *rankingTable = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 200, 800)];
        NSTableColumn *rankingColumn = [[NSTableColumn alloc] initWithIdentifier:@"type"];
        [rankingTable addTableColumn:rankingColumn]; [rankingColumn release];
        rankingTable.dataSource = (id)rankingDelegate; rankingTable.delegate = (id)rankingDelegate;
        [rankingTable selectRowIndexes:[NSIndexSet indexSetWithIndex:3] byExtendingSelection:NO];
        require(rankingTable.selectedRow == 3 && rankingDelegate->observedRow == 3 && rankingDelegate->changes == 1,
                "programmatic selection updates the row before notifying its delegate");
        [rankingTable selectRowIndexes:[NSIndexSet indexSetWithIndex:3] byExtendingSelection:NO];
        require(rankingDelegate->changes == 1, "unchanged selection does not notify again");
        rankingTable.allowsMultipleSelection = YES;
        [rankingTable selectRowIndexes:[NSIndexSet indexSetWithIndex:7] byExtendingSelection:YES];
        require(rankingTable.numberOfSelectedRows == 2 && [rankingTable.selectedRowIndexes containsIndex:3] &&
                [rankingTable.selectedRowIndexes containsIndex:7], "extended selection retains both rows");
        id preparedCell = [rankingTable preparedCellAtColumn:0 row:7];
        require([[preparedCell stringValue] isEqual:@"Type 7"], "willDisplayCell supplies the attributed row text");
        NSRange effectiveRange = NSMakeRange(NSNotFound,0);
        NSColor *rowColor = [[preparedCell attributedStringValue] attribute:NSForegroundColorAttributeName atIndex:2 effectiveRange:&effectiveRange];
        require(rowColor.redComponent == .5 && effectiveRange.location == 0 && effectiveRange.length == 6,
                "prepared cells preserve the hidden row foreground color and effective range");
        NSScrollView *rankingScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 200, 100)];
        rankingScroll.documentView = rankingTable;
        rankingScroll.autohidesScrollers = YES;
        rankingScroll.hasVerticalScroller = YES; rankingScroll.hasHorizontalScroller = YES;
        [rankingScroll tile];
        require(!rankingScroll.verticalScroller.hidden && rankingScroll.horizontalScroller.hidden &&
                NSMaxX(rankingScroll.contentView.frame) <= NSMinX(rankingScroll.verticalScroller.frame),
                "overflow shows only the needed scroller without overlapping the table");
        require(rankingScroll.contentView.flipped, "clip view follows the table coordinate system");
        [rankingTable scrollRowToVisible:35];
        require(NSMinY(rankingScroll.contentView.bounds) <= 700 && NSMaxY(rankingScroll.contentView.bounds) >= 720,
                "scrollRowToVisible reveals rows below the viewport");
        [rankingTable scrollRowToVisible:0];
        require(NSMinY(rankingScroll.contentView.bounds) == 0, "scrollRowToVisible can return to the first row");
        rankingScroll.verticalScroller.doubleValue = 1;
        [NSApp sendAction:rankingScroll.verticalScroller.action to:rankingScroll.verticalScroller.target from:rankingScroll.verticalScroller];
        require(NSMaxY(rankingScroll.contentView.bounds) == NSMaxY(rankingTable.frame) &&
                rankingScroll.verticalScroller.knobProportion < 1,
                "moving the scrollbar reveals the bottom and reflects the document size");
        NSView *shortDocument = [[NSView alloc] initWithFrame:NSMakeRect(0,0,100,50)];
        rankingScroll.documentView = shortDocument; [rankingScroll tile];
        require(rankingScroll.verticalScroller.hidden && rankingScroll.horizontalScroller.hidden &&
                rankingScroll.contentView.bounds.origin.y == 0,
                "fitting content hides both scrollers and clears stale scroll offsets");
        [shortDocument release];
        [rankingTable deselectAll:nil];
        require(rankingTable.selectedRow == -1 && rankingTable.selectedRowIndexes.count == 0,
                "empty selection returns the Cocoa sentinel");
        [rankingScroll release]; [rankingTable release]; [rankingDelegate release];
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
        ChoiceInputEvent *input = [ChoiceInputEvent new]; input->host = window;
        [popup mouseDown:input]; input->kind = NSEventTypeKeyDown; input->code = 53;
        [NSApp sendEvent:input];
        require(window.childWindows.count==0 && popup.indexOfSelectedItem==24 && target->clicks==1,
                "Escape from the parent window dismisses the menu without changing selection");
        [popup selectItemAtIndex:0]; [popup mouseDown:input]; input->code = 125;
        [NSApp sendEvent:input]; input->code = 36; [NSApp sendEvent:input];
        require(window.childWindows.count==0 && popup.indexOfSelectedItem==1 && target->clicks==2,
                "arrow and Return events from the parent select one menu row");
        [popup mouseDown:input]; input->kind = NSEventTypeLeftMouseDown;
        [NSApp sendEvent:input];
        require(window.childWindows.count==0 && popup.indexOfSelectedItem==1 && target->clicks==2,
                "outside click dismisses the menu without selecting a row");
        ChoiceMouseButton *mouseButton = [[ChoiceMouseButton alloc] initWithFrame:NSMakeRect(0,0,40,24)];
        NSEvent *mouse = [NSEvent new];
        [mouseButton mouseDown:mouse]; [mouseButton mouseUp:mouse];
        require(mouseButton->clicks==1,"mouse release dispatches the subclass performClick handler");
        [mouse release]; [mouseButton release]; [input release];
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
        NSRect beforeSize = window.frame;
        [window setContentSize:NSMakeSize(400, 589)];
        require(NSEqualSizes(window.contentView.frame.size, NSMakeSize(400, 589)) &&
                NSMaxY(window.frame) == NSMaxY(beforeSize), "content sizing updates the view and preserves the window top edge");
        NSBox *box = [[NSBox alloc] initWithFrame:NSMakeRect(20, 20, 360, 130)];
        NSView *boxContent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 118)];
        [box setContentView:boxContent];
        [window.contentView addSubview:box];
        [box setFrameSize:NSMakeSize(440, 150)];
        [box layoutSubtreeIfNeeded];
        require(box.contentView == boxContent && boxContent.superview == box &&
                NSEqualSizes(boxContent.frame.size, NSMakeSize(440, 138)), "box content follows its container and reserves title space");
        [boxContent release]; [box release];
        NSWindowController *controller = [[NSWindowController alloc] initWithWindow:window];
        require(controller.window == window && controller.isWindowLoaded, "window controller ownership");
        [controller release]; [window release];
        puts("application principal class, NIBArchive menu, file panels and async cancellation passed");
    }
    return 0;
}
