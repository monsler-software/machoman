#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <fts.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <dispatch/dispatch.h>
static void require(BOOL ok,const char *s){if(!ok){fprintf(stderr,"FAIL: %s\n",s);exit(1);}}
@interface ScanProbe : NSObject {
@public NSConditionLock *queue;NSLock *progress;BOOL cancelled;int steps,updates,delayed;BOOL main_callback;
}
- (void)worker:(id)argument;
- (void)update:(id)argument;
- (void)tick:(id)argument;
- (void)cancelAction:(id)sender;
@end
@implementation ScanProbe
- (instancetype)init{self=[super init];queue=[[NSConditionLock alloc]initWithCondition:0];progress=[[NSLock alloc]init];return self;}
- (void)worker:(id)argument {
    require(![NSThread isMainThread],"scan worker has a distinct thread");
    [queue lockWhenCondition:1];
    for(int i=0;i<100;++i){[progress lock];BOOL stop=cancelled;if(!stop)++steps;[progress unlock];if(stop)break;
        [self performSelectorOnMainThread:@selector(update:) withObject:argument waitUntilDone:YES];
        [NSThread sleepForTimeInterval:.001];}
    [queue unlockWithCondition:2];
}
- (void)update:(id)argument{require([NSThread isMainThread]&&[argument isEqual:@"scan"],"progress callback on UI thread");++updates;main_callback=YES;}
- (void)tick:(id)argument{require([NSThread isMainThread]&&[argument isEqual:@"tick"],"delayed callback arguments");++delayed;}
- (void)cancelAction:(id)sender{[progress lock];cancelled=YES;[progress unlock];}
- (void)dealloc{[queue release];[progress release];[super dealloc];}
@end
struct counter{NSLock *lock;int value;};
static void *increment(void *arg){struct counter *c=arg;for(int i=0;i<2000;++i){[c->lock lock];++c->value;[c->lock unlock];}return NULL;}
static int comparisons;
static int compare_entries(const FTSENT **a,const FTSENT **b){++comparisons;require((*a)->fts_namelen==strlen((*a)->fts_name),"comparator sees Darwin file entry");return strcmp((*a)->fts_name,(*b)->fts_name);}
static void filesystem_probe(void){
    char root[]="/tmp/machoman-scan-XXXXXX";require(mkdtemp(root)!=NULL,"host scan fixture created");
    char file[1024],hard[1024],link[1024],skip[1024],inside[1024];
    snprintf(file,sizeof file,"%s/file space.dat",root);snprintf(hard,sizeof hard,"%s/hard.dat",root);snprintf(link,sizeof link,"%s/link",root);snprintf(skip,sizeof skip,"%s/skip",root);snprintf(inside,sizeof inside,"%s/skip/hidden.dat",root);
    int fd=open(file,O_WRONLY|O_CREAT|O_EXCL,0600);require(fd>=0&&write(fd,"scan data",9)==9,"guest writes host file");close(fd);
    require(!mkdir(skip,0700),"guest creates host subfolder");fd=open(inside,O_WRONLY|O_CREAT,0600);require(fd>=0,"nested file created");close(fd);
    require(!linkat(AT_FDCWD,file,AT_FDCWD,hard,0)&&!symlink("file space.dat",link),"hard and symbolic links created");
    BOOL isDir=NO;NSFileManager *fm=[NSFileManager defaultManager];NSString *path=[NSString stringWithUTF8String:root];require([fm fileExistsAtPath:path isDirectory:&isDir]&&isDir,"Foundation sees host folder");
    require([[fm contentsOfDirectoryAtPath:path error:NULL]count]==4,"Foundation lists host contents");
    char cwd[1024],after[1024];require(getcwd(cwd,sizeof cwd)!=NULL,"initial cwd");
    char *paths[]={root,NULL};FTS *tree=fts_open(paths,FTS_PHYSICAL,compare_entries);require(tree!=NULL,"Darwin traversal opens host folder");
    FTSENT *entry=fts_read(tree);require(entry&&entry->fts_info==FTS_D&&entry->fts_level==0&&S_ISDIR(entry->fts_statp->st_mode),"root type and stat layout");entry->fts_number=123;entry->fts_pointer=tree;
    int children=0;for(FTSENT *e=fts_children(tree,0);e;e=e->fts_link){require(e->fts_parent==entry&&e->fts_level==1,"child parent and level");require(e->fts_statp!=NULL,"child metadata is available before traversal");++children;}require(children==4,"children use Darwin linked list");
    char *unsorted_paths[]={root,NULL};FTS *unsorted=fts_open(unsorted_paths,FTS_PHYSICAL|FTS_XDEV,NULL);
    require(unsorted&&fts_read(unsorted)->fts_info==FTS_D,"unsorted traversal opens root");
    int unsorted_children=0;for(FTSENT *e=fts_children(unsorted,0);e;e=e->fts_link){require(e->fts_statp&&e->fts_info!=FTS_NSOK&&(S_ISREG(e->fts_statp->st_mode)||S_ISDIR(e->fts_statp->st_mode)||S_ISLNK(e->fts_statp->st_mode)),"unsorted children have Darwin stat metadata");require(strlen(e->fts_path)==e->fts_pathlen&&strstr(e->fts_path,e->fts_name)!=NULL,"unsorted child path includes name");++unsorted_children;}
    require(unsorted_children==4&&!fts_close(unsorted),"unsorted child metadata preserves all entries");
    int files=0,symlinks=0,postorder=0;
    while((entry=fts_read(tree))){
        require(strcmp(entry->fts_name,"hidden.dat"),"FTS_SKIP suppresses subtree");
        if(entry->fts_info==FTS_F){require(entry->fts_statp&&entry->fts_statp->st_size==9&&entry->fts_statp->st_nlink==2,"file size and hard link stat conversion");require(entry->fts_parent->fts_number==123&&entry->fts_parent->fts_pointer==tree,"caller metadata remains on parent");++files;}
        if(entry->fts_info==FTS_SL)++symlinks;
        if(entry->fts_info==FTS_D&&entry->fts_level==1)require(!fts_set(tree,entry,FTS_SKIP),"skip directory instruction");
        if(entry->fts_info==FTS_DP)++postorder;
    }
    require(files==2&&symlinks==1&&postorder==2&&comparisons>0,"complete traversal and comparator callback");require(!fts_close(tree),"traversal closes");require(getcwd(after,sizeof after)&&!strcmp(cwd,after),"worker traversal preserves process cwd");
    struct{struct statfs volume;unsigned long guard;}v;memset(&v,0,sizeof v);v.guard=0xabcdef;
    require(!statfs(root,&v.volume)&&v.volume.f_bsize>0&&v.volume.f_blocks>0&&v.volume.f_bavail<=v.volume.f_blocks&&v.guard==0xabcdef,"Darwin filesystem capacity layout");
    require(unlink(inside)==0&&rmdir(skip)==0&&unlink(link)==0&&unlink(hard)==0&&unlink(file)==0&&rmdir(root)==0,"scan fixture removed");
}
int main(){@autoreleasepool{
    [NSApplication sharedApplication];
    NSNumberFormatter *format=[[NSNumberFormatter alloc]init];require([format.decimalSeparator isEqual:@"."],"number formatter returns a real separator string");format.maximumFractionDigits=2;format.minimumFractionDigits=1;require([[format stringFromNumber:@1.25]isEqual:@"1.25"],"number formatter decimal precision");[format release];
    NSMutableAttributedString *text=[[NSMutableAttributedString alloc]initWithString:@"Scanning files"];
    [text addAttribute:NSForegroundColorAttributeName value:[NSColor grayColor] range:NSMakeRange(0,text.length)];
    NSTextField *label=[[NSTextField alloc]initWithFrame:NSMakeRect(0,0,100,20)];label.attributedStringValue=text;require([label.stringValue isEqual:@"Scanning files"],"attributed progress text reaches standard control");[label release];[text release];
    require([@"1 KiB" rangeOfString:@"."].location==NSNotFound&&[@"abc" rangeOfString:@"z" options:0].location==NSNotFound&&[@"abc" rangeOfString:@"z" options:0 range:NSMakeRange(0,3)].location==NSNotFound,"NSString absent range uses Darwin NSNotFound");
    require([@[@"a"] indexOfObject:@"b"]==NSNotFound,"NSArray absent index uses Darwin NSNotFound");
    NSRect slice,rest;NSDivideRect(NSMakeRect(10,20,30,40),&slice,&rest,8,NSMaxYEdge);require(slice.origin.y==52&&slice.size.height==8&&rest.size.height==32&&NSIsEmptyRect(NSZeroRect),"Foundation rect geometry and out parameters");
    NSBitmapImageRep *rgb32=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:NULL pixelsWide:3 pixelsHigh:2 bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:32];
    require(rgb32&&rgb32.bytesPerRow==12&&rgb32.bitmapData!=NULL,"padded RGB32 bitmap used by GrandPerspective");[rgb32 release];
    require([NSNumber numberWithFloat:.25f].floatValue==.25f&&[[NSNumber alloc]initWithFloat:1.5f].doubleValue==1.5&&[@"0.25" floatValue]==.25f,"single precision argument and result ABI");
    NSUserDefaults *defaults=[NSUserDefaults standardUserDefaults];[defaults setObject:@"0.125" forKey:@"machomanSmokeFloat"];
    require([defaults floatForKey:@"machomanSmokeFloat"]==.125f,"numeric string preference");[defaults removeObjectForKey:@"machomanSmokeFloat"];
    [defaults setFloat:.2f forKey:@"machomanSmokeFloat"];
    require([defaults floatForKey:@"machomanSmokeFloat"]==.2f,"NSUserDefaults float setter ABI");[defaults removeObjectForKey:@"machomanSmokeFloat"];
    __block BOOL dispatched=NO;dispatch_async(dispatch_get_main_queue(),^{require([NSThread isMainThread],"main queue block thread");dispatched=YES;});
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,.001,NO);require(dispatched,"dispatch main queue object symbol ABI");
    NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:NULL pixelsWide:3 pixelsHigh:2 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:16 bitsPerPixel:32];
    require(bitmap&&bitmap.pixelsWide==3&&bitmap.pixelsHigh==2&&bitmap.bytesPerRow==16&&bitmap.bitsPerPixel==32&&bitmap.bitmapData&&bitmap.bitmapFormat==NSBitmapFormatAlphaNonpremultiplied,"bitmap allocation and ARM64 stack arguments");
    NSUInteger rgba[]={255,64,32,128},got[4]={0};[bitmap setPixel:rgba atX:2 y:1];[bitmap getPixel:got atX:2 y:1];
    require(!memcmp(rgba,got,sizeof rgba)&&bitmap.bitmapData[24]==255&&bitmap.bitmapData[27]==128,"stable bitmap stride and pixel components");
    NSImage *image=[[NSImage alloc]initWithSize:NSMakeSize(3,2)];[image addRepresentation:bitmap];[bitmap release];
    require(image.representations.count==1&&[[image.representations objectAtIndex:0]bitmapData]!=NULL,"image retains bitmap representation");[image removeRepresentation:[image.representations objectAtIndex:0]];require(!image.representations.count,"image removes representation");[image release];
    NSColorList *palette=[[NSColorList alloc]initWithName:@"Scan palette"];
    NSColorList *filePalette=[[NSColorList alloc]initWithName:@"File palette" fromFile:[NSString stringWithUTF8String:getenv("MACHOMAN_TEST_PALETTE")]];
    require(filePalette.allKeys.count==2&&[[filePalette.allKeys objectAtIndex:0]isEqual:@"red"]&&[filePalette colorWithKey:@"red"].redComponent==1&&[filePalette colorWithKey:@"cyan"].blueComponent==1,"binary keyed color palette preserves order and RGB values");[filePalette release];
    require([[NSColorList alloc]initWithName:@"Absent" fromFile:@"/tmp/machoman-absent-palette.clr"]==nil,"missing palette reports failure");
    [palette insertColor:[NSColor cyanColor] key:@"cyan" atIndex:0];[palette insertColor:[NSColor magentaColor] key:@"magenta" atIndex:1];
    require(palette.allKeys.count==2&&[[palette.allKeys objectAtIndex:0]isEqual:@"cyan"]&&[palette colorWithKey:@"cyan"].greenComponent==1.&&[palette colorWithKey:@"cyan"].redComponent==0.,"ordered color palette and standard RGB colors");
    [palette setColor:[NSColor orangeColor] forKey:@"cyan"];require(palette.allKeys.count==2&&[palette colorWithKey:@"cyan"].greenComponent==.5,"palette replacement preserves keys");
    [palette removeColorWithKey:@"magenta"];require(palette.allKeys.count==1&&[palette colorWithKey:@"magenta"]==nil,"palette removal");[palette release];
    filesystem_probe();
    NSMutableSet *seen=[[NSMutableSet alloc]initWithCapacity:4];[seen addObject:@"scan"];[seen addObject:@"scan"];require(seen.count==1&&[NSMutableSet setWithCapacity:2]!=nil,"mutable set capacity and deduplication");[seen release];
    [NSApp showHelp:nil];
    struct counter c={[[NSLock alloc]init],0};pthread_t a,b;require(!pthread_create(&a,NULL,increment,&c)&&!pthread_create(&b,NULL,increment,&c),"concurrent lock workers");pthread_join(a,NULL);pthread_join(b,NULL);
    require(c.value==4000,"NSLock excludes concurrent updates");[c.lock release];
    NSRecursiveLock *recursive=[[NSRecursiveLock alloc]init];[recursive lock];require([recursive tryLock],"recursive lock permits same owner");[recursive unlock];[recursive unlock];[recursive release];
    NSCondition *condition=[[NSCondition alloc]init];[condition lock];require(![condition waitUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]],"condition timed wait expires");[condition unlock];[condition release];
    ScanProbe *probe=[[ScanProbe alloc]init];
    require(![probe->queue tryLockWhenCondition:1],"condition lock waits for correct value");
    require(![probe->queue lockWhenCondition:1 beforeDate:[NSDate dateWithTimeIntervalSinceNow:.002]],"condition lock timeout");
    [NSThread detachNewThreadSelector:@selector(worker:) toTarget:probe withObject:@"scan"];
    [probe->queue lock];[probe->queue unlockWithCondition:1];
    [probe performSelector:@selector(tick:) withObject:@"tick" afterDelay:.003];
    [probe performSelector:@selector(tick:) withObject:@"cancelled" afterDelay:.001];
    [NSObject cancelPreviousPerformRequestsWithTarget:probe selector:@selector(tick:) object:@"cancelled"];
    NSButton *cancel=[[NSButton alloc]initWithFrame:NSMakeRect(0,0,70,24)];cancel.target=probe;cancel.action=@selector(cancelAction:);
    BOOL cancelled=NO;for(int i=0;i<200;++i){CFRunLoopRunInMode(kCFRunLoopDefaultMode,.003,NO);
        if(probe->updates&&!cancelled){[cancel performClick:nil];cancelled=YES;}
        if([probe->queue condition]==2&&probe->delayed==1)break;}
    require(probe->main_callback&&probe->updates>0&&probe->delayed==1,"scan updates and delayed callback complete");
    require(cancelled&&probe->cancelled&&[probe->queue condition]==2&&probe->steps<100,"Cancel releases scan worker and completes task");
    NSFont *font=[NSFont monospacedDigitSystemFontOfSize:12 weight:0];require(font&&font.pointSize==12,"monospaced progress font");
    NSView *view=[[NSView alloc]initWithFrame:NSMakeRect(0,0,100,100)];NSToolTipTag tag=[view addToolTipRect:NSMakeRect(0,0,50,50) owner:probe userData:NULL];require(tag>0,"tooltip tag allocated");[view removeToolTip:tag];[view release];
    require([NSAssertionHandler currentHandler]!=nil,"assertion handler exists");
    [cancel release];[probe release];puts("scan worker, locks, delayed UI updates, cancellation and progress controls passed");
}return 0;}
