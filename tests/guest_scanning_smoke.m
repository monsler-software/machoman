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
    int children=0;for(FTSENT *e=fts_children(tree,0);e;e=e->fts_link){require(e->fts_parent==entry&&e->fts_level==1,"child parent and level");++children;}require(children==4,"children use Darwin linked list");
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
