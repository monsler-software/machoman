/* Cross-compile with the macOS SDK and run through machoman. This exercises
 * the actual arm64 Objective-C return ABI and Darwin temp-directory API. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "menu_nib_fixture.h"
#include <locale.h>
#include <xlocale.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <netdb.h>
#include <arpa/inet.h>

static void check_resolver_addresses(void) {
    for (int ipv6 = 0; ipv6 < 2; ++ipv6) {
        struct addrinfo hints = {0}, *results = (void *)1;
        hints.ai_family = ipv6 ? AF_INET6 : AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_CANONNAME;
        const char *ip = ipv6 ? "::1" : "127.0.0.1";
        if (getaddrinfo(ip, "12345", &hints, &results) || !results) abort();
        unsigned count = 0;
        for (struct addrinfo *p = results; p; p = p->ai_next) {
            if (++count > 10 || p->ai_family != hints.ai_family || !p->ai_addr ||
                p->ai_addr->sa_family != hints.ai_family || p->ai_addr->sa_len != p->ai_addrlen) abort();
            char name[NI_MAXHOST], service[NI_MAXSERV];
            if (getnameinfo(p->ai_addr, p->ai_addrlen, name, sizeof name, service, sizeof service,
                            NI_NUMERICHOST | NI_NUMERICSERV) || strcmp(name, ip) || strcmp(service, "12345")) abort();
            if (ipv6 && ntohs(((struct sockaddr_in6 *)p->ai_addr)->sin6_port) != 12345) abort();
            if (!ipv6 && ntohs(((struct sockaddr_in *)p->ai_addr)->sin_port) != 12345) abort();
        }
        freeaddrinfo(results);
        results = (void *)1;
        if (getaddrinfo("invalid numeric host", "12345", &hints, &results) != EAI_NONAME || results) abort();
        if (!*gai_strerror(EAI_NONAME)) abort();
    }
    puts("resolver IPv4/IPv6 sockaddr layout, numeric round trip and Darwin error codes passed");
}

static void check_task_memory_info(void) {
    mach_task_basic_info_data_t basic;
    memset(&basic, 0xa5, sizeof basic);
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&basic, &count) || count != MACH_TASK_BASIC_INFO_COUNT) abort();
    if (!basic.virtual_size || !basic.resident_size || basic.resident_size > basic.virtual_size || basic.virtual_size > (1ULL << 40)) abort();
    task_vm_info_data_t vm;
    memset(&vm, 0xa5, sizeof vm);
    count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) || count < TASK_VM_INFO_REV1_COUNT) abort();
    if (!vm.virtual_size || !vm.resident_size || !vm.phys_footprint || vm.page_size != sysconf(_SC_PAGESIZE)) abort();
    if (vm.virtual_size > (1ULL << 40) || vm.resident_size > vm.virtual_size) abort();
    struct { task_vm_info_data_t info; unsigned long canary; } truncated;
    memset(&truncated, 0xa5, sizeof truncated);
    count = TASK_VM_INFO_REV0_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&truncated.info, &count) || count != TASK_VM_INFO_REV0_COUNT) abort();
    if (truncated.info.phys_footprint != 0xa5a5a5a5a5a5a5a5ULL || truncated.canary != 0xa5a5a5a5a5a5a5a5UL) abort();
    unsigned char tiny[4] = {1, 2, 3, 4};
    count = 1;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)tiny, &count) != KERN_INVALID_ARGUMENT || tiny[0] != 1 || tiny[3] != 4) abort();
    count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), 9999, (task_info_t)&basic, &count) != KERN_INVALID_ARGUMENT) abort();
    printf("Mach memory info: virtual=%llu resident=%llu footprint=%llu bytes; revision bounds passed\n",
           vm.virtual_size, vm.resident_size, vm.phys_footprint);
}

extern int CGLSetCurrentContext(void *context);
extern void *CGLGetCurrentContext(void);
static void check_cgl_handles(void) {
    if (CGLSetCurrentContext(0) || CGLGetCurrentContext()) abort();
    /* Opaque EGL handles must never enter the Objective-C isa walk. */
    if (CGLSetCurrentContext((void *)1) != 10004 || CGLGetCurrentContext()) abort();
    puts("CGL rejects unknown opaque handles without dereferencing them");
}

static void check_locale_conversions(void) {
    char *end = 0;
    if (strtod_l("0", &end, 0) != 0 || *end) abort();
    if (strtod_l(" -12.5tail", &end, 0) != -12.5 || strcmp(end, "tail")) abort();
    if (strtof_l("1.25x", &end, 0) != 1.25f || strcmp(end, "x")) abort();
    if (strtol_l("-42x", &end, 10, 0) != -42 || *end != 'x') abort();
    if (strtoul_l("ffx", &end, 16, 0) != 255 || *end != 'x') abort();
    if (strtoll_l("-4294967297x", &end, 10, 0) != -4294967297LL || *end != 'x') abort();
    if (strtoull_l("4294967297x", &end, 10, 0) != 4294967297ULL || *end != 'x') abort();
    locale_t locale = newlocale(LC_NUMERIC_MASK, "C", 0);
    locale_t previous = uselocale(0);
    if (!locale || strtod_l("2.5x", &end, locale) != 2.5 || *end != 'x' || uselocale(0) != previous) abort();
    if (strtod_l("3.5x", &end, LC_GLOBAL_LOCALE) != 3.5 || *end != 'x' || uselocale(0) != previous) abort();
    errno = 0;
    (void)strtod_l("1e9999", &end, locale);
    if (errno != ERANGE || *end || uselocale(0) != previous) abort();
    freelocale(locale);
    puts("Darwin numeric conversions with NULL/explicit/global locale passed");
}

/* A guest initializer that tail-calls a host function returns through LR=0. */
__asm__(".section __TEXT,__text,regular,pure_instructions\n"
        ".p2align 2\n_smoke_tail_host_init:\n b _getpid\n"
        ".section __DATA,__mod_init_func,mod_init_funcs\n"
        ".p2align 3\n .quad _smoke_tail_host_init\n");

typedef struct { long majorVersion, minorVersion, patchVersion; } NSOperatingSystemVersion;
typedef struct { double x, y; } SmokePoint;
typedef struct { double width, height; } SmokeSize;
typedef struct { SmokePoint origin; SmokeSize size; } SmokeRect;
@interface NSObject { Class isa; }
+ (instancetype)alloc;
- (instancetype)init;
- (instancetype)retain;
- (void)release;
- (instancetype)autorelease;
- (void)dealloc;
- (instancetype)copy;
@end

static unsigned property_copies, property_values_destroyed;
@interface SmokeCopyValue : NSObject { @public int number; }
- (instancetype)copyWithZone:(void *)zone;
@end
@implementation SmokeCopyValue
- (instancetype)copyWithZone:(void *)zone {
    if (zone) abort();
    ++property_copies;
    SmokeCopyValue *result = [[SmokeCopyValue alloc] init];
    result->number = number;
    return result;
}
- (void)dealloc { ++property_values_destroyed; [super dealloc]; }
@end
@interface SmokeCopyHolder : NSObject
@property(nonatomic, copy) id value;
@property(atomic, copy) id atomicValue;
@property(nonatomic, copy) int (^callback)(void);
@end
@implementation SmokeCopyHolder
- (void)dealloc {
    self.value = 0;
    self.atomicValue = 0;
    self.callback = 0;
    [super dealloc];
}
@end
static void install_copy_callback(SmokeCopyHolder *holder, SmokeCopyValue *value) {
    int captured = 2;
    holder.callback = ^{ return captured + value->number; };
}
static void check_copy_properties(void) {
    SmokeCopyHolder *holder = [[SmokeCopyHolder alloc] init];
    SmokeCopyValue *source = [[SmokeCopyValue alloc] init];
    source->number = 41;
    holder.value = source;
    @autoreleasepool {
        if (holder.value == source || ((SmokeCopyValue *)holder.value)->number != 41) abort();
        holder.value = holder.value;
    }
    if (property_copies != 2 || property_values_destroyed != 1) abort();
    holder.atomicValue = source;
    @autoreleasepool {
        if (holder.atomicValue == source || ((SmokeCopyValue *)holder.atomicValue)->number != 41) abort();
    }
    id direct = [source copy];
    if (direct == source || ((SmokeCopyValue *)direct)->number != 41 || property_copies != 4) abort();
    [direct release];
    holder.value = 0;
    holder.atomicValue = 0;
    if (holder.value || holder.atomicValue || property_values_destroyed != 4) abort();
    install_copy_callback(holder, source);
    [source release];
    @autoreleasepool {
        if (property_values_destroyed != 4 || holder.callback() != 43) abort();
    }
    holder.callback = 0;
    if (property_values_destroyed != 5) abort();
    [holder release];
    puts("copy properties pass a nil zone, own replacements and preserve captured stack blocks");
}
@class NSWindow;
@interface NSView : NSObject
- (instancetype)initWithFrame:(SmokeRect)frame;
- (SmokeRect)frame;
- (SmokeSize)convertSizeToBacking:(SmokeSize)size;
- (void)setLayer:(id)layer;
- (void)setWantsLayer:(unsigned char)value;
- (void)addSubview:(NSView *)view;
- (void)removeFromSuperview;
- (NSWindow *)window;
- (void)setBounds:(SmokeRect)bounds;
- (void)setFrame:(SmokeRect)frame;
- (void)setFrameOrigin:(SmokePoint)origin;
- (void)setPostsFrameChangedNotifications:(unsigned char)value;
- (void)setPostsBoundsChangedNotifications:(unsigned char)value;
- (NSView *)hitTest:(SmokePoint)point;
- (SmokePoint)convertPoint:(SmokePoint)point fromView:(NSView *)view;
- (SmokePoint)convertPoint:(SmokePoint)point toView:(NSView *)view;
- (SmokeRect)convertRect:(SmokeRect)rect fromView:(NSView *)view;
@end
@interface SmokeView : NSView
@end
@implementation SmokeView
- (instancetype)initWithFrame:(SmokeRect)frame { return [super initWithFrame:frame]; }
@end
@interface SmokeFlippedView : NSView
@end
@implementation SmokeFlippedView
- (unsigned char)isFlipped { return 1; }
@end
@interface NSWindow : NSObject
- (instancetype)initWithContentRect:(SmokeRect)frame styleMask:(unsigned long)style
    backing:(unsigned long)backing defer:(unsigned char)defer;
- (NSView *)contentView;
- (void)close;
- (void)makeKeyAndOrderFront:(id)sender;
- (void)orderFront:(id)sender;
- (void)orderOut:(id)sender;
- (void)setContentView:(NSView *)view;
- (unsigned char)makeFirstResponder:(id)view;
- (id)firstResponder;
- (void)sendEvent:(id)event;
- (SmokePoint)convertBaseToScreen:(SmokePoint)point;
- (SmokePoint)convertScreenToBase:(SmokePoint)point;
- (SmokePoint)mouseLocationOutsideOfEventStream;
- (void)setContentMinSize:(SmokeSize)size;
- (SmokeSize)minSize;
- (void)setMaxSize:(SmokeSize)size;
- (SmokeSize)contentMaxSize;
@end
@interface NSApplication : NSObject
+ (instancetype)sharedApplication;
- (long)runModalForWindow:(NSWindow *)window;
- (NSWindow *)modalWindow;
- (void)stopModal;
- (void)stopModalWithCode:(long)code;
- (void)abortModal;
- (void)run;
- (void)stop:(id)sender;
- (id)windows;
- (id)orderedWindows;
- (void)sendEvent:(id)event;
- (id)currentEvent;
@end
@interface NSArray : NSObject
+ (instancetype)arrayWithObject:(id)value;
- (unsigned long)count;
- (id)objectAtIndex:(unsigned long)index;
@end
@interface NSEvent : NSObject
- (id)characters;
- (unsigned short)keyCode;
@end
@interface SmokeMouseEvent : NSEvent { unsigned long kind; }
- (instancetype)initWithType:(unsigned long)value;
@end
@implementation SmokeMouseEvent
- (instancetype)initWithType:(unsigned long)value { self = [super init]; kind = value; return self; }
- (unsigned long)type { return kind; }
@end
@interface SmokeInputView : NSView { @public unsigned long downCount, upCount, dragCount; }
@end
@implementation SmokeInputView
- (void)mouseDown:(id)event { ++downCount; }
- (void)mouseUp:(id)event { ++upCount; }
- (void)mouseDragged:(id)event { ++dragCount; }
@end
@interface NSOpenGLLayer : NSObject
@end
@interface SmokeDrawLayer : NSOpenGLLayer { unsigned long draws; }
- (unsigned long)drawCount;
@end
@implementation SmokeDrawLayer
- (unsigned char)canDrawInOpenGLContext:(id)context pixelFormat:(id)format
    forLayerTime:(double)time displayTime:(const void *)stamp { return 1; }
- (void)drawInOpenGLContext:(id)context pixelFormat:(id)format
    forLayerTime:(double)time displayTime:(const void *)stamp { ++draws; }
- (unsigned long)drawCount { return draws; }
@end
@interface SmokeSlowDrawLayer : SmokeDrawLayer
@end
@implementation SmokeSlowDrawLayer
- (void)drawInOpenGLContext:(id)context pixelFormat:(id)format
    forLayerTime:(double)time displayTime:(const void *)stamp {
    [super drawInOpenGLContext:context pixelFormat:format forLayerTime:time displayTime:stamp];
    usleep(25000); /* Longer than the frame throttle: still only one draw per pass. */
}
@end
@interface NSProcessInfo
+ (instancetype)processInfo;
- (NSOperatingSystemVersion)operatingSystemVersion;
@end
@interface NSString : NSObject
+ (instancetype)stringWithUTF8String:(const char *)value;
- (const char *)UTF8String;
- (unsigned short)characterAtIndex:(unsigned long)index;
@end
static unsigned load_count;
static pthread_t load_thread;
static unsigned loads_before_initializer;
@interface SmokeLoad : NSObject
@end
@implementation SmokeLoad
+ (void)load {
    load_thread = pthread_self();
    if (!load_thread || strcmp([[NSString stringWithUTF8String:"class load"] UTF8String], "class load")) abort();
    ++load_count;
}
@end
@interface SmokeLoad (Startup)
@end
@implementation SmokeLoad (Startup)
+ (void)load {
    if (load_count != 1 || pthread_self() != load_thread) abort();
    ++load_count;
}
@end
__attribute__((constructor)) static void check_load_before_initializer(void) {
    loads_before_initializer = load_count;
}
@interface NSData : NSObject
- (unsigned char)writeToFile:(NSString *)path atomically:(unsigned char)atomic;
@end
@interface NSError : NSObject
- (long)code;
@end
@interface NSNotification : NSObject
- (id)object;
@end
@interface NSNotificationCenter : NSObject
+ (instancetype)defaultCenter;
- (void)addObserver:(id)observer selector:(SEL)selector name:(NSString *)name object:(id)object;
- (void)removeObserver:(id)observer;
@end
extern NSString *const NSViewFrameDidChangeNotification, *const NSViewBoundsDidChangeNotification;
@interface SmokeGeometryObserver : NSObject { @public unsigned frames, bounds; SmokeRect lastFrame; }
- (void)frameChanged:(NSNotification *)notification;
- (void)boundsChanged:(NSNotification *)notification;
@end
@implementation SmokeGeometryObserver
- (void)frameChanged:(NSNotification *)notification { ++frames; lastFrame = [(NSView *)[notification object] frame]; }
- (void)boundsChanged:(NSNotification *)notification { ++bounds; }
@end
static void check_view_geometry_notifications(void) {
    NSView *view = [[NSView alloc] initWithFrame:(SmokeRect){{0, 0}, {1280, 750}}];
    SmokeGeometryObserver *observer = [[SmokeGeometryObserver alloc] init];
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:observer selector:@selector(frameChanged:) name:NSViewFrameDidChangeNotification object:view];
    [center addObserver:observer selector:@selector(boundsChanged:) name:NSViewBoundsDidChangeNotification object:view];
    [view setPostsFrameChangedNotifications:1];
    [view setPostsBoundsChangedNotifications:1];
    SmokeRect frame = {{0, 0}, {949, 1028}};
    [view setFrame:frame];
    if (observer->frames != 1 || observer->lastFrame.size.width != 949 || observer->lastFrame.size.height != 1028) abort();
    [view setFrame:frame];
    if (observer->frames != 1) abort();
    [view setFrameOrigin:(SmokePoint){11, 22}];
    if (observer->frames != 2 || observer->lastFrame.origin.x != 11 || observer->lastFrame.origin.y != 22) abort();
    [view setPostsFrameChangedNotifications:0];
    [view setFrameOrigin:(SmokePoint){33, 44}];
    if (observer->frames != 2) abort();
    unsigned previous = observer->bounds;
    SmokeRect bounds = {{2, 3}, {50, 60}};
    [view setBounds:bounds];
    if (observer->bounds != previous + 1) abort();
    [view setBounds:bounds];
    if (observer->bounds != previous + 1) abort();
    [view setPostsBoundsChangedNotifications:0];
    bounds.origin.x = 4;
    [view setBounds:bounds];
    if (observer->bounds != previous + 1) abort();
    [center removeObserver:observer];
    [observer release]; [view release];
    puts("view geometry notifications update guest layout after resize and respect opt-out");
}
@interface NSURL : NSObject
+ (instancetype)fileURLWithPath:(NSString *)path;
+ (instancetype)URLWithString:(NSString *)text;
- (instancetype)initWithString:(NSString *)text;
- (NSString *)path;
- (NSString *)scheme;
- (NSString *)absoluteString;
- (unsigned char)isFileURL;
- (NSData *)bookmarkDataWithOptions:(unsigned long)options
    includingResourceValuesForKeys:(id)keys relativeToURL:(NSURL *)relative error:(NSError **)error;
+ (NSData *)bookmarkDataWithContentsOfURL:(NSURL *)url error:(NSError **)error;
+ (instancetype)URLByResolvingBookmarkData:(NSData *)data options:(unsigned long)options
    relativeToURL:(NSURL *)relative bookmarkDataIsStale:(unsigned char *)stale error:(NSError **)error;
+ (unsigned char)writeBookmarkData:(NSData *)data toURL:(NSURL *)url
    options:(unsigned long)options error:(NSError **)error;
@end

extern id CFURLCreateWithString(void *, NSString *, id);
static void check_cfurl_string_construction(void) {
    NSURL *owned;
    const char *text = "https://www.ableton.com/account/authorize/application";
    @autoreleasepool {
        NSString *input = [NSString stringWithUTF8String:text];
        owned = CFURLCreateWithString(0, input, 0);
        NSURL *factory = [NSURL URLWithString:input];
        NSURL *initialized = [[NSURL alloc] initWithString:input];
        if (!owned || strcmp([[owned scheme] UTF8String], "https") || [owned isFileURL] ||
            strcmp([[owned absoluteString] UTF8String], text) ||
            strcmp([[factory absoluteString] UTF8String], text) ||
            strcmp([[initialized absoluteString] UTF8String], text)) abort();
        [initialized release];
    }
    if (strcmp([[owned absoluteString] UTF8String], text)) abort();
    [owned release];
    @autoreleasepool {
        NSURL *file = CFURLCreateWithString(0, @"file:///tmp/runtime%20smoke.txt", 0);
        if (![file isFileURL] || strcmp([[file path] UTF8String], "/tmp/runtime smoke.txt")) abort();
        [file release];
    }
    puts("CFURL string creation constructs NSURL payload and returns an owned URL");
}

typedef struct { double a, b, c, d, tx, ty; } SmokeFontMatrix;
extern id CTFontCreateWithName(NSString *, double, const SmokeFontMatrix *);
extern id CTFontCopyAttribute(id, NSString *);
extern double CTFontGetSize(id);
extern unsigned long CTFontGetTypeID(void), CFGetTypeID(id);
extern unsigned char CFNumberGetValue(id, long, void *);
extern long CFDataGetLength(id);
extern const unsigned char *CFDataGetBytePtr(id);
extern NSString *const kCTFontNameAttribute, *const kCTFontURLAttribute, *const kCTFontSizeAttribute,
    *const kCTFontMatrixAttribute, *const kCTFontFamilyNameAttribute;
static void check_font_attributes(void) {
    SmokeFontMatrix matrix = {2, 0, 0, 3, 4, 5};
    id font = CTFontCreateWithName([NSString stringWithUTF8String:"sans-serif"], 17, &matrix);
    if (!font || CFGetTypeID(font) != CTFontGetTypeID() || CTFontGetSize(font) != 17) abort();
    NSString *name = CTFontCopyAttribute(font, kCTFontNameAttribute);
    NSString *family = CTFontCopyAttribute(font, kCTFontFamilyNameAttribute);
    NSURL *url = CTFontCopyAttribute(font, kCTFontURLAttribute);
    id size = CTFontCopyAttribute(font, kCTFontSizeAttribute);
    id data = CTFontCopyAttribute(font, kCTFontMatrixAttribute);
    double points = 0;
    if (!name || !strlen([name UTF8String]) || !family || !strlen([family UTF8String])) abort();
    if (!url || access([[url path] UTF8String], R_OK) || !CFNumberGetValue(size, 13, &points) || points != 17) abort();
    if (!data || CFDataGetLength(data) != sizeof matrix || memcmp(CFDataGetBytePtr(data), &matrix, sizeof matrix)) abort();
    if (CTFontCopyAttribute(font, [NSString stringWithUTF8String:"unknown-font-attribute"])) abort();
    [font release]; /* Copy results must survive releasing their font. */
    if (!strlen([name UTF8String]) || access([[url path] UTF8String], R_OK)) abort();
    [name release]; [family release]; [url release]; [size release]; [data release];
    font = CTFontCreateWithName([NSString stringWithUTF8String:"machoman-missing-font-123"], 0, 0);
    if (!font || CTFontGetSize(font) != 12) abort();
    [font release];
    puts("CoreText named fonts, real file URLs and owned attributes passed");
}

extern NSString *const AVFileTypeQuickTimeMovie, *const AVFileTypeMPEG4;
extern NSString *UTTypeCopyDescription(NSString *identifier);
static void check_file_type_descriptions(void) {
    if (strcmp([AVFileTypeQuickTimeMovie UTF8String], "com.apple.quicktime-movie") ||
        strcmp([AVFileTypeMPEG4 UTF8String], "public.mpeg-4")) abort();
    NSString *description = UTTypeCopyDescription(AVFileTypeQuickTimeMovie);
    if (!description || ![description UTF8String] || !strlen([description UTF8String])) abort();
    [description release];
    description = UTTypeCopyDescription(AVFileTypeMPEG4);
    if (!description || ![description UTF8String] || !strlen([description UTF8String])) abort();
    [description release];
    if (UTTypeCopyDescription([NSString stringWithUTF8String:"org.machoman.unknown-type"])) abort();
    puts("AVFoundation UTI constants and owned CoreServices descriptions passed");
}

static void check_bookmark_files(void) {
    char path[] = "/tmp/machoman-bookmark-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) abort();
    const char ordinary[] = "InstantMappings-HowTo.txt is an ordinary text file\n";
    if (write(fd, ordinary, sizeof ordinary - 1) != sizeof ordinary - 1 || close(fd)) abort();
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    NSError *error = 0;
    /* Reading an ordinary file must not fabricate a bookmark pointing at
     * that same file: Live otherwise resolves it again without progressing. */
    if ([NSURL bookmarkDataWithContentsOfURL:url error:&error] || !error || [error code] != 259) abort();
    if ([NSURL bookmarkDataWithContentsOfURL:url error:0]) abort();

    NSData *bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:0
        relativeToURL:0 error:&error];
    if (!bookmark || error) abort();
    if (![NSURL writeBookmarkData:bookmark toURL:url options:0 error:&error] || error) abort();
    NSData *read = [NSURL bookmarkDataWithContentsOfURL:url error:&error];
    if (!read || error) abort();
    struct { unsigned char value, canary[3]; } stale = {1, {0x12, 0x34, 0x56}};
    NSURL *resolved = [NSURL URLByResolvingBookmarkData:read options:0 relativeToURL:0
        bookmarkDataIsStale:&stale.value error:&error];
    if (!resolved || error || strcmp([[resolved path] UTF8String], path) || stale.value ||
        stale.canary[0] != 0x12 || stale.canary[1] != 0x34 || stale.canary[2] != 0x56) abort();
    if (unlink(path)) abort();
    if ([NSURL bookmarkDataWithContentsOfURL:url error:&error] || !error || [error code] != 260) abort();
    puts("ordinary files rejected; bookmark file read/write and BOOL output passed");
}

extern void *os_log_create(const char *, const char *);
extern void *os_retain(void *);
extern void os_release(void *);
struct os_log_s;
extern struct os_log_s _os_log_default, _os_log_disabled;
extern int os_log_type_enabled(void *, unsigned char);
extern id objc_retain(id);
extern void objc_release(id);
extern void _os_log_impl(void *, void *, unsigned char, const char *, unsigned char *, unsigned int);

extern void *CFRunLoopGetCurrent(void);
extern void *CFRunLoopGetMain(void);
extern void *kCFRunLoopDefaultMode;
extern int CFRunLoopRunInMode(void *, double, unsigned char);
extern void CFRunLoopPerformBlock(void *, void *, void (^)(void));
extern void *GetMainEventLoop(void);
extern void *GetCurrentEventLoop(void);
extern int InstallEventLoopTimer(void *, double, double, void (*)(void *, void *), void *, void **);
extern int SetEventLoopTimerNextFireTime(void *, double);
extern int RemoveEventLoopTimer(void *);
extern void dispatch_once_f(long *, void *, void (*)(void *)) __attribute__((weak_import));
extern void dispatch_once(long *, void (^)(void)) __attribute__((weak_import));

static long nested_once;
static void once_callback(void *context) {
    int *count = context;
    ++*count;
    dispatch_once(&nested_once, ^{ ++*count; });
}
static void check_dispatch_once(void) {
    if (!dispatch_once_f || !dispatch_once) abort();
    long once = 0;
    int count = 0;
    dispatch_once_f(&once, &count, once_callback);
    dispatch_once_f(&once, &count, once_callback);
    if (count != 2 || once != -1 || nested_once != -1) abort();
    puts("weak dispatch_once imports and nested guest callbacks passed");
}

extern void *dispatch_queue_create(const char *, void *);
extern void *dispatch_get_global_queue(long, unsigned long);
extern void dispatch_async(void *, void (^)(void));
extern void dispatch_async_f(void *, void *, void (*)(void *));
extern void *dispatch_semaphore_create(long);
extern long dispatch_semaphore_wait(void *, unsigned long long);
extern long dispatch_semaphore_signal(void *);
extern unsigned long long dispatch_time(unsigned long long, long long);
static _Thread_local unsigned long callback_count;
struct AsyncState { void *semaphore; unsigned long caller, worker, first_count, second_count; };
static void check_worker_context(struct AsyncState *state) {
    unsigned long worker = (unsigned long)pthread_self();
    void *stack_end = pthread_get_stackaddr_np(pthread_self());
    size_t stack_size = pthread_get_stacksize_np(pthread_self());
    unsigned long local = 0;
    if (!worker || worker == state->caller || !stack_end || stack_size < 512 * 1024 ||
        (char *)&local >= (char *)stack_end || (char *)&local < (char *)stack_end - stack_size) abort();
    if (state->worker && worker != state->worker) abort();
    state->worker = worker;
}
static void async_function(void *context) {
    struct AsyncState *state = context;
    check_worker_context(state);
    state->second_count = ++callback_count;
    dispatch_semaphore_signal(state->semaphore);
}
static void check_dispatch_workers(void) {
    void *serial = dispatch_queue_create("runtime-smoke.serial", 0);
    struct AsyncState state = {dispatch_semaphore_create(0), (unsigned long)pthread_self(), 0, 0, 0};
    struct AsyncState *state_ptr = &state;
    dispatch_async(serial, ^{
        check_worker_context(state_ptr);
        state_ptr->first_count = ++callback_count;
        dispatch_semaphore_signal(state_ptr->semaphore);
    });
    if (dispatch_semaphore_wait(state.semaphore, dispatch_time(0, 2000000000LL))) {
        fputs("dispatch_async guest block never executed on its serial worker\n", stderr);
        abort();
    }
    dispatch_async_f(serial, &state, async_function);
    if (dispatch_semaphore_wait(state.semaphore, dispatch_time(0, 2000000000LL)) ||
        state.first_count != 1 || state.second_count != 2) abort();
    /* Global queue callbacks also need their own guest stack and identity. */
    struct AsyncState global = {dispatch_semaphore_create(0), state.caller, 0, 0, 0};
    dispatch_async_f(dispatch_get_global_queue(0, 0), &global, async_function);
    if (dispatch_semaphore_wait(global.semaphore, dispatch_time(0, 2000000000LL)) ||
        !global.worker || !global.second_count) abort();
    puts("dispatch workers execute guest blocks/functions with stable identity, stack and TLS");
}

struct TimerState { void *timer, *loop; int count, remove_self; };
static void timer_callback(void *timer, void *info) {
    struct TimerState *state = info;
    if (timer != state->timer || CFRunLoopGetCurrent() != state->loop) abort();
    ++state->count;
    if (state->remove_self && RemoveEventLoopTimer(timer) != 0) abort();
}
static void pump_loop(void) {
    for (int i = 0; i < 5; ++i) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, 0);
}
static unsigned deferred_objects_released, deferred_blocks_called;
@interface SmokeDeferredCapture : NSObject
- (int)value;
@end
@implementation SmokeDeferredCapture
- (int)value { return 12345; }
- (void)dealloc { ++deferred_objects_released; [super dealloc]; }
@end
__attribute__((noinline)) static void enqueue_deferred_block(void) {
    @autoreleasepool {
        SmokeDeferredCapture *capture = [[SmokeDeferredCapture alloc] init];
        __block int value = 17;
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
            if ([capture value] != 12345 || ++value != 18) abort();
            ++deferred_blocks_called;
        });
        [capture release];
    }
}
__attribute__((noinline)) static void overwrite_deferred_stack(void) {
    volatile unsigned char bytes[8192];
    for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)i;
}
static void check_deferred_block_lifetime(void) {
    enqueue_deferred_block();
    if (deferred_objects_released || deferred_blocks_called) abort();
    overwrite_deferred_stack();
    pump_loop();
    if (deferred_objects_released != 1 || deferred_blocks_called != 1) abort();
    pump_loop();
    if (deferred_objects_released != 1 || deferred_blocks_called != 1) abort();
    puts("CFRunLoop deferred blocks survive stack/pool exit, retain captures and release after firing");
}

@interface NSDraggingItem : NSObject
- (instancetype)initWithPasteboardWriter:(id)writer;
- (id)item;
- (void)setDraggingFrame:(SmokeRect)frame contents:(id)contents;
- (SmokeRect)draggingFrame;
@end
static void check_dragging_item_construction(void) {
    unsigned released = deferred_objects_released;
    @autoreleasepool {
        SmokeDeferredCapture *writer = [[SmokeDeferredCapture alloc] init];
        SmokeDeferredCapture *contents = [[SmokeDeferredCapture alloc] init];
        NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
        if (!item || [item item] != writer) abort();
        [item setDraggingFrame:(SmokeRect){{11, 22}, {33, 44}} contents:contents];
        NSArray *items = [[NSArray arrayWithObject:item] retain];
        [writer release]; [contents release]; [item release];
        if (deferred_objects_released != released) abort();
        item = [items objectAtIndex:0];
        if ([item item] != writer) abort();
        SmokeRect frame = [item draggingFrame];
        if (frame.origin.x != 11 || frame.origin.y != 22 || frame.size.width != 33 || frame.size.height != 44) abort();
        [item setDraggingFrame:(SmokeRect){{1, 2}, {3, 4}} contents:0];
        if (deferred_objects_released != released + 1) abort();
        [items release];
    }
    if (deferred_objects_released != released + 2) abort();
    puts("NSDraggingItem constructs a non-nil array element, preserves frame ABI and owns writer/contents");
}
@interface NSView (SmokeDragging)
- (id)beginDraggingSessionWithItems:(id)items event:(id)event source:(id)source;
@end
@interface SmokeDragEvent : NSObject { @public unsigned long eventType, button, code; }
@end
@implementation SmokeDragEvent
- (unsigned long)type { return eventType; }
- (unsigned long)buttonNumber { return button; }
- (unsigned short)keyCode { return code; }
- (id)window { return 0; }
@end
@interface SmokeDragSource : NSObject { @public unsigned begins, ends; id lastSession; }
@end
@implementation SmokeDragSource
- (void)draggingSession:(id)session willBeginAtPoint:(SmokePoint)point { ++begins; lastSession = session; }
- (void)draggingSession:(id)session endedAtPoint:(SmokePoint)point operation:(unsigned long)operation {
    if (session != lastSession || operation != 0) abort(); ++ends;
}
@end
static void check_dragging_completion_and_size_limits(void) {
    @autoreleasepool {
        NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {640, 480}} styleMask:9 backing:2 defer:0];
        [window setContentMinSize:(SmokeSize){321.25, 234.5}];
        [window setMaxSize:(SmokeSize){1920, 1080}];
        SmokeSize lo = [window minSize], hi = [window contentMaxSize];
        if (lo.width != 321.25 || lo.height != 234.5 || hi.width != 1920 || hi.height != 1080) abort();
        SmokeDragSource *source = [[SmokeDragSource alloc] init];
        SmokeDragEvent *event = [[SmokeDragEvent alloc] init];
        NSArray *items = [NSArray arrayWithObject:source];
        NSApplication *app = [NSApplication sharedApplication];
        for (unsigned i = 0; i < 3; ++i) {
            id session = [[window contentView] beginDraggingSessionWithItems:items event:event source:source];
            if (!session || source->begins != i + 1) abort();
            if (i == 0) {
                event->eventType = 2; event->button = 1; [app sendEvent:event];
                if (source->ends) abort();
                event->button = 0; [app sendEvent:event]; [app sendEvent:event];
            } else if (i == 1) {
                event->eventType = 10; event->code = 53; [app sendEvent:event];
            } else [window close];
            if (source->ends != i + 1) abort();
        }
        [event release]; [source release]; [window release];
    }
    puts("drag session ends exactly once on matching release/Escape/close; window size limits preserve CGSize ABI");
}
static void check_run_loop(void) {
    void *loop = CFRunLoopGetCurrent();
    if (CFRunLoopGetMain() != loop || GetMainEventLoop() != loop || GetCurrentEventLoop() != loop) abort();
    __block int block_called = 0;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{ ++block_called; });
    pump_loop();
    if (block_called != 1) abort();

    struct TimerState once = {0, loop, 0, 0};
    if (InstallEventLoopTimer(loop, -1, -1, timer_callback, &once, &once.timer) != 0) abort();
    pump_loop();
    if (once.count != 0 || SetEventLoopTimerNextFireTime(once.timer, 0) != 0) abort();
    pump_loop();
    if (once.count != 1 || SetEventLoopTimerNextFireTime(once.timer, 0.001) != 0) abort();
    pump_loop();
    if (once.count != 2 || RemoveEventLoopTimer(once.timer) != 0) abort();

    struct TimerState repeat = {0, loop, 0, 0};
    if (InstallEventLoopTimer(loop, 0, 0.002, timer_callback, &repeat, &repeat.timer) != 0) abort();
    pump_loop();
    if (repeat.count < 2 || SetEventLoopTimerNextFireTime(repeat.timer, -1) != 0) abort();
    int count = repeat.count;
    pump_loop();
    if (repeat.count != count || SetEventLoopTimerNextFireTime(repeat.timer, 0) != 0) abort();
    pump_loop();
    if (repeat.count <= count || RemoveEventLoopTimer(repeat.timer) != 0) abort();
    count = repeat.count;
    pump_loop();
    if (repeat.count != count || SetEventLoopTimerNextFireTime(repeat.timer, 0) == 0) abort();

    struct TimerState self = {0, loop, 0, 1};
    if (InstallEventLoopTimer(loop, 0, 0.001, timer_callback, &self, &self.timer) != 0) abort();
    pump_loop();
    if (self.count != 1) abort();
    puts("main run loop blocks and Carbon timers passed");
}

@interface NSTimer : NSObject
+ (instancetype)timerWithTimeInterval:(double)interval target:(id)target selector:(SEL)selector userInfo:(id)info repeats:(unsigned char)repeats;
+ (instancetype)scheduledTimerWithTimeInterval:(double)interval target:(id)target selector:(SEL)selector userInfo:(id)info repeats:(unsigned char)repeats;
+ (instancetype)scheduledTimerWithTimeInterval:(double)interval repeats:(unsigned char)repeats block:(void (^)(NSTimer *))block;
- (void)fire;
- (void)invalidate;
- (unsigned char)isValid;
- (id)userInfo;
- (double)timeInterval;
@end
@interface NSRunLoop : NSObject
+ (instancetype)mainRunLoop;
- (void)addTimer:(NSTimer *)timer forMode:(id)mode;
@end
static unsigned cocoa_targets_destroyed;
@interface SmokeTimerTarget : NSObject {
@public unsigned count, invalidate_self;
    NSTimer *expected;
    void *loop;
    id info;
}
- (void)tick:(NSTimer *)timer;
@end
@implementation SmokeTimerTarget
- (void)tick:(NSTimer *)timer {
    if (timer != expected || CFRunLoopGetCurrent() != loop || [timer userInfo] != info) abort();
    ++count;
    if (invalidate_self) [timer invalidate];
}
- (void)dealloc { ++cocoa_targets_destroyed; [super dealloc]; }
@end
static void check_cocoa_timers(void) {
    @autoreleasepool {
        SmokeTimerTarget *target = [[SmokeTimerTarget alloc] init];
        target->loop = CFRunLoopGetCurrent(); target->info = @"timer payload";
        NSTimer *once = [NSTimer timerWithTimeInterval:0.002 target:target selector:@selector(tick:) userInfo:target->info repeats:0];
        target->expected = once;
        if (![once isValid] || [once timeInterval] != 0.002 || [once userInfo] != target->info) abort();
        pump_loop();
        if (target->count) abort(); /* factory does not schedule automatically */
        [[NSRunLoop mainRunLoop] addTimer:once forMode:(id)kCFRunLoopDefaultMode];
        [[NSRunLoop mainRunLoop] addTimer:once forMode:@"NSEventTrackingRunLoopMode"];
        [[NSRunLoop mainRunLoop] addTimer:once forMode:@"NSModalPanelRunLoopMode"];
        pump_loop();
        if (target->count != 1 || [once isValid]) abort();
        [once fire]; pump_loop();
        if (target->count != 1) abort();
        target->count = 0; target->invalidate_self = 1;
        NSTimer *repeat = [NSTimer scheduledTimerWithTimeInterval:0.002 target:target selector:@selector(tick:) userInfo:target->info repeats:1];
        target->expected = repeat;
        pump_loop();
        if (target->count != 1 || [repeat isValid]) abort();
        unsigned destroyed = cocoa_targets_destroyed;
        [target release];
        if (cocoa_targets_destroyed != destroyed + 1) abort();
    }
    unsigned destroyed = cocoa_targets_destroyed;
    @autoreleasepool {
        SmokeTimerTarget *target = [[SmokeTimerTarget alloc] init];
        target->loop = CFRunLoopGetCurrent(); target->invalidate_self = 1;
        target->expected = [NSTimer scheduledTimerWithTimeInterval:0.002 target:target selector:@selector(tick:) userInfo:0 repeats:1];
        [target release]; /* scheduled timer must retain target across pool exit */
    }
    if (cocoa_targets_destroyed != destroyed) abort();
    pump_loop();
    if (cocoa_targets_destroyed != destroyed + 1) abort();
    __block unsigned block_count = 0;
    NSTimer *block_timer = [NSTimer scheduledTimerWithTimeInterval:0.002 repeats:0 block:^(NSTimer *timer) {
        if (![timer isValid] || CFRunLoopGetCurrent() != CFRunLoopGetMain()) abort();
        ++block_count;
    }];
    pump_loop();
    if (block_count != 1 || [block_timer isValid]) abort();
    puts("Cocoa timers: scheduling, mode deduplication, guest callbacks and invalidation ownership passed");
}

@interface NSResponder : NSObject
- (void)interpretKeyEvents:(NSArray *)events;
@end
@interface NSView (SmokeKeys)
- (void)interpretKeyEvents:(NSArray *)events;
@end
@interface SmokeKeyEvent : NSEvent { @public unsigned short code; unsigned long modifiers; id text; NSWindow *window; }
@end
@implementation SmokeKeyEvent
- (unsigned long)type { return 10; }
- (unsigned short)keyCode { return code; }
- (unsigned long)modifierFlags { return modifiers; }
- (id)characters { return text; }
- (id)charactersIgnoringModifiers { return text; }
- (NSWindow *)window { return window; }
@end
@interface SmokeKeyView : NSView { @public unsigned keys, inserts, commands, equivalents; id last_text; SEL last_command; }
@end
@implementation SmokeKeyView
- (unsigned char)performKeyEquivalent:(SmokeKeyEvent *)event {
    if (event->modifiers & (1UL << 20)) { ++equivalents; return 1; }
    return 0;
}
- (void)keyDown:(SmokeKeyEvent *)event { ++keys; [self interpretKeyEvents:[NSArray arrayWithObject:event]]; }
- (void)insertText:(id)text replacementRange:(struct { unsigned long location, length; })range {
    if (range.location != 0x7fffffffffffffffUL || range.length) abort();
    ++inserts; last_text = text;
}
- (void)doCommandBySelector:(SEL)command { ++commands; last_command = command; }
@end
@interface SmokeFocusView : SmokeKeyView { @public unsigned char refuse_resign, refuse_become; unsigned becomes, resigns; }
@end
@implementation SmokeFocusView
- (unsigned char)becomeFirstResponder { ++becomes; return !refuse_become; }
- (unsigned char)resignFirstResponder { ++resigns; return !refuse_resign; }
@end
@interface SmokeShortcutView : NSView { @public unsigned shortcuts; }
@end
@implementation SmokeShortcutView
- (unsigned char)performKeyEquivalent:(SmokeKeyEvent *)event {
    if ([event->text characterAtIndex:0] == 's') { ++shortcuts; return 1; }
    return 0;
}
@end
static void check_keyboard_responder_paths(void) {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {200, 100}} styleMask:0 backing:2 defer:0];
    SmokeKeyView *view = [[SmokeKeyView alloc] initWithFrame:(SmokeRect){{0, 0}, {200, 100}}];
    [window setContentView:view];
    SmokeKeyEvent *event = [[SmokeKeyEvent alloc] init];
    event->code = 0; event->text = @"a";
    event->window = window;
    [[NSApplication sharedApplication] sendEvent:event];
    if ([[NSApplication sharedApplication] currentEvent] != event) abort();
    if (view->keys != 1 || view->inserts != 1 || view->last_text != event->text) abort();
    [window makeFirstResponder:view];
    event->code = 51; event->text = @"\x7f";
    [window sendEvent:event];
    if (view->keys != 2 || view->commands != 1 || view->last_command != @selector(deleteBackward:)) abort();
    event->code = 0; event->text = @"a"; event->modifiers = 1UL << 20;
    [window sendEvent:event];
    if (view->equivalents != 1 || view->keys != 2 || view->inserts != 1) abort();
    event->modifiers = 1UL << 18;
    [window sendEvent:event];
    if (view->keys != 3 || view->inserts != 1) abort();
    event->modifiers = 0; event->text = @"Привет";
    [window sendEvent:event];
    if (view->inserts != 2 || view->last_text != event->text) abort();
    [event release]; [window close]; [window release]; [view release];
    puts("keyboard responder routing, key equivalents, Unicode insertion and editing commands passed");
}

static unsigned pool_objects_released;
static unsigned destroyed_content_views;
@interface SmokeDestroyedView : NSView
@end
@implementation SmokeDestroyedView
- (void)dealloc { ++destroyed_content_views; [super dealloc]; }
@end
static unsigned long registered_window_count(void) {
    @autoreleasepool { return [[[NSApplication sharedApplication] windows] count]; }
}
static void check_ordered_windows(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        unsigned long before = [[app orderedWindows] count];
        NSWindow *a = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {50, 50}} styleMask:0 backing:2 defer:0];
        NSWindow *b = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {50, 50}} styleMask:0 backing:2 defer:0];
        if ([[app orderedWindows] count] != before) abort();
        [a orderFront:0]; [b orderFront:0];
        NSArray *snapshot = [app orderedWindows];
        if ([snapshot count] != before + 2 || [snapshot objectAtIndex:0] != b || [snapshot objectAtIndex:1] != a) abort();
        [a orderFront:0];
        if ([[app orderedWindows] objectAtIndex:0] != a || [snapshot objectAtIndex:0] != b) abort();
        [a orderOut:0];
        if ([[app orderedWindows] count] != before + 1 || [[app orderedWindows] objectAtIndex:0] != b) abort();
        [a close]; [a orderFront:0];
        if ([[app orderedWindows] count] != before + 2 || [[app orderedWindows] objectAtIndex:0] != a) abort();
        [a close]; [b close]; [a release]; [b release];
        if ([[app orderedWindows] count] != before) abort();
    }
    puts("orderedWindows tracks front order, hidden windows, reopening and independent snapshots");
}
static void check_window_destruction(void) {
    unsigned long before = registered_window_count();
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {50, 50}}
        styleMask:0 backing:2 defer:0];
    NSView *content = [[SmokeDestroyedView alloc] initWithFrame:(SmokeRect){{0, 0}, {50, 50}}];
    [window setContentView:content];
    [content release];
    if (registered_window_count() != before + 1) abort();
    [window release]; /* Releasing the last reference must also unmap/unregister. */
    if (registered_window_count() != before || destroyed_content_views != 1) abort();
    puts("last window release unregisters it and releases its content view");
}
@interface SmokePoolObject : NSObject
@end
@implementation SmokePoolObject
- (void)dealloc { ++pool_objects_released; [super dealloc]; }
@end
static void check_event_loop_pools(void) {
    NSApplication *app = [NSApplication sharedApplication];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {50, 50}}
        styleMask:0 backing:2 defer:0];
    @autoreleasepool {
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
            [[[SmokePoolObject alloc] init] autorelease];
            [app stopModal];
        });
        [app runModalForWindow:window];
        if (pool_objects_released != 1) abort();
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
            [[[SmokePoolObject alloc] init] autorelease];
            [app stop:0];
        });
        [app run];
        if (pool_objects_released != 2) abort();
    }
    [window close]; [window release];
    puts("main and modal event loops release temporary objects after each turn");
}

static void check_modal_loop(void) {
    NSApplication *app = [NSApplication sharedApplication];
    NSWindow *outer = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {100, 100}}
        styleMask:0 backing:2 defer:0];
    NSWindow *inner = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {50, 50}}
        styleMask:0 backing:2 defer:0];
    __block int state = 0;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
        if ([app modalWindow] != outer) abort();
        state = 1;
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
            if ([app modalWindow] != inner) abort();
            state = 2;
            [app stopModalWithCode:42];
        });
        if ([app runModalForWindow:inner] != 42 || state != 2 || [app modalWindow] != outer) abort();
        state = 3;
        [app stopModalWithCode:-7];
    });
    if ([app runModalForWindow:outer] != -7 || state != 3 || [app modalWindow]) abort();
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{ [app stopModal]; });
    if ([app runModalForWindow:outer] != -1000) abort();
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{ [app abortModal]; });
    if ([app runModalForWindow:outer] != -1001) abort();
    [inner close]; [outer close]; [inner release]; [outer release];
    puts("nested modal loops pump guest blocks and preserve stop/abort responses");
}

static void stop_drawing_timer(void *timer, void *info) { [(NSApplication *)info stopModal]; }
static void check_slow_layer_single_frame(void) {
    NSApplication *app = [NSApplication sharedApplication];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {100, 100}}
        styleMask:0 backing:2 defer:0];
    SmokeSlowDrawLayer *layer = [[SmokeSlowDrawLayer alloc] init];
    [[window contentView] setLayer:layer];
    [[window contentView] setWantsLayer:1];
    void *timer = 0;
    if (InstallEventLoopTimer(CFRunLoopGetMain(), 0, -1, stop_drawing_timer, app, &timer)) abort();
    if ([app runModalForWindow:window] != -1000 || RemoveEventLoopTimer(timer)) abort();
    fprintf(stderr, "slow layer frames in one pass: %lu\n", [layer drawCount]);
    if ([layer drawCount] != 1) abort();
    [window close]; [window release]; [layer release];
    puts("a slow OpenGL frame is drawn once per display pass");
}
static void check_multiple_layer_frames(void) {
    NSApplication *app = [NSApplication sharedApplication];
    NSWindow *windows[2];
    SmokeDrawLayer *layers[2];
    for (int i = 0; i < 2; ++i) {
        windows[i] = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {100, 100}}
            styleMask:0 backing:2 defer:0];
        layers[i] = [[SmokeDrawLayer alloc] init];
        [[windows[i] contentView] setLayer:layers[i]];
        [[windows[i] contentView] setWantsLayer:1];
        [windows[i] makeKeyAndOrderFront:0];
    }
    void *timer = 0;
    if (InstallEventLoopTimer(CFRunLoopGetMain(), 0.08, -1, stop_drawing_timer, app, &timer)) abort();
    if ([app runModalForWindow:windows[0]] != -1000 || RemoveEventLoopTimer(timer)) abort();
    /* Both windows must receive frames during the same loop. A global
     * throttle lets the first window consume every frame of the second. */
    unsigned long first = [layers[0] drawCount], second = [layers[1] drawCount];
    fprintf(stderr, "layer frames: %lu, %lu\n", first, second);
    if (first < 2 || second < 2) abort();
    for (int i = 0; i < 2; ++i) { [windows[i] close]; [windows[i] release]; [layers[i] release]; }
    puts("multiple OpenGL layers receive frames without starving each other");
}

static void check_window_mouse_routing(void) {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{11, 22}, {100, 100}}
        styleMask:0 backing:2 defer:0];
    SmokePoint screen = [window convertBaseToScreen:(SmokePoint){30, 40}];
    SmokePoint local = [window convertScreenToBase:screen];
    if (screen.x != 41 || screen.y != 62 || local.x != 30 || local.y != 40) abort();
    local = [window mouseLocationOutsideOfEventStream];
    if (local.x != -11 || local.y != -22) abort();
    SmokeInputView *hit = [[SmokeInputView alloc] initWithFrame:(SmokeRect){{0, 0}, {100, 100}}];
    SmokeInputView *other = [[SmokeInputView alloc] initWithFrame:(SmokeRect){{0, 0}, {100, 100}}];
    [window setContentView:hit];
    [window makeFirstResponder:other];
    /* Native NSEvent payload has location {0,0}; the guest overrides type.
     * Up and drag must follow the down target even with another responder. */
    const unsigned long types[] = {1, 6, 2};
    for (int i = 0; i < 3; ++i) {
        SmokeMouseEvent *event = [[SmokeMouseEvent alloc] initWithType:types[i]];
        [window sendEvent:event];
        [event release];
    }
    if (hit->downCount != 1 || hit->dragCount != 1 || hit->upCount != 1 ||
        other->downCount || other->dragCount || other->upCount) abort();
    [window makeFirstResponder:0];
    [window close]; [window release]; [hit release]; [other release];
    puts("window coordinate conversion and mouse down/drag/up target routing passed");
}

static void check_flipped_view_coordinates(void) {
    NSView *root = [[SmokeFlippedView alloc] initWithFrame:(SmokeRect){{0, 0}, {446, 107}}];
    SmokePoint p = [root convertPoint:(SmokePoint){385, 22} fromView:0];
    if (p.x != 385 || p.y != 85) abort(); /* Live's OK button uses flipped coordinates. */
    NSView *child = [[NSView alloc] initWithFrame:(SmokeRect){{10, 20}, {100, 30}}];
    [root addSubview:child];
    p = [child convertPoint:(SmokePoint){5, 7} toView:0];
    if (p.x != 15 || p.y != 64 || [root hitTest:p] != child) abort();
    p = [child convertPoint:p fromView:0];
    if (p.x != 5 || p.y != 7) abort();
    [child setBounds:(SmokeRect){{2, 3}, {50, 15}}];
    p = [child convertPoint:(SmokePoint){7, 8} toView:0];
    if (p.x != 20 || p.y != 67) abort();
    p = [child convertPoint:p fromView:0];
    if (p.x != 7 || p.y != 8) abort();
    SmokeRect r = [root convertRect:(SmokeRect){{0, 0}, {10, 15}} fromView:0];
    if (r.origin.x != 0 || r.origin.y != 92 || r.size.width != 10 || r.size.height != 15) abort();
    [child release]; [root release];
    puts("flipped view points, bounds scaling, hit testing and rectangle conversion passed");
}

@interface NSMenu : NSObject
- (instancetype)initWithTitle:(id)title;
- (void)addItem:(id)item;
- (void)insertItem:(id)item atIndex:(long)index;
- (void)removeItemAtIndex:(long)index;
- (void)setSubmenu:(id)submenu forItem:(id)item;
- (long)numberOfItems;
- (id)itemAtIndex:(long)index;
- (id)itemWithTag:(long)tag;
- (void)update;
- (unsigned char)performKeyEquivalent:(id)event;
@end
@interface NSMenuItem : NSObject
- (instancetype)initWithTitle:(id)title action:(SEL)action keyEquivalent:(id)key;
+ (id)separatorItem;
- (unsigned char)isSeparatorItem;
- (void)setTarget:(id)target;
- (id)menu;
- (void)setTag:(long)tag;
- (long)tag;
- (void)setHidden:(unsigned char)hidden;
- (unsigned char)isHidden;
- (void)setEnabled:(unsigned char)enabled;
- (unsigned char)isEnabled;
- (void)setKeyEquivalent:(id)key;
- (id)keyEquivalent;
- (void)setKeyEquivalentModifierMask:(unsigned long)mask;
- (unsigned long)keyEquivalentModifierMask;
@end
@interface NSApplication (SmokeMenu)
- (void)setMainMenu:(id)menu;
- (id)mainMenu;
- (unsigned char)sendAction:(SEL)action to:(id)target from:(id)sender;
@end
@interface SmokeMenuTarget : NSView { @public unsigned actions; id last_sender; }
@end
@implementation SmokeMenuTarget
- (void)menuAction:(id)sender { ++actions; last_sender = sender; }
- (unsigned char)validateMenuItem:(NSMenuItem *)item { return [item tag] != 99; }
@end
@interface NSBundle : NSObject
+ (instancetype)mainBundle;
- (id)bundlePath;
- (unsigned char)loadNibNamed:(id)name owner:(id)owner topLevelObjects:(id *)objects;
@end
@interface NSCursor : NSObject
+ (id)arrowCursor;
+ (id)IBeamCursor;
+ (id)crosshairCursor;
+ (id)currentCursor;
+ (void)pop;
+ (void)hide;
+ (void)unhide;
+ (void)setHiddenUntilMouseMoves:(unsigned char)value;
- (void)set;
- (void)push;
- (instancetype)initWithImage:(id)image hotSpot:(SmokePoint)point;
- (SmokePoint)hotSpot;
- (id)image;
@end
static void check_cursor_state(void) {
    NSCursor *arrow = [NSCursor arrowCursor], *beam = [NSCursor IBeamCursor];
    [arrow set]; if ([NSCursor currentCursor] != arrow) abort();
    [beam push]; if ([NSCursor currentCursor] != beam) abort();
    [[NSCursor crosshairCursor] push]; [NSCursor pop]; if ([NSCursor currentCursor] != beam) abort();
    [NSCursor pop]; if ([NSCursor currentCursor] != arrow) abort();
    [NSCursor hide]; [NSCursor hide]; [NSCursor unhide]; [NSCursor unhide];
    [NSCursor setHiddenUntilMouseMoves:1]; [NSCursor setHiddenUntilMouseMoves:0];
    NSCursor *custom = [[NSCursor alloc] initWithImage:0 hotSpot:(SmokePoint){3.5, 7.25}];
    SmokePoint hotspot = [custom hotSpot]; if (hotspot.x != 3.5 || hotspot.y != 7.25 || [custom image]) abort();
    [custom set]; [custom release]; if ([NSCursor currentCursor] == arrow) abort(); [arrow set];
    puts("NSCursor current state, nested stack, ownership and hotspot ABI passed");
}
static void check_nib_application_menu(void) {
    NSBundle *bundle = [NSBundle mainBundle];
    const char *root = [[bundle bundlePath] UTF8String]; if (!root || !*root) abort();
    char path[4096]; snprintf(path, sizeof path, "%s/Contents", root); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/Contents/Resources", root); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/Contents/Resources/NativeMenuSmoke.nib", root);
    FILE *file = fopen(path, "wb"); if (!file || fwrite(menu_nib_fixture, 1, sizeof menu_nib_fixture, file) != sizeof menu_nib_fixture) abort(); fclose(file);
    NSApplication *app = [NSApplication sharedApplication]; [app setMainMenu:0];
    if (![bundle loadNibNamed:@"NativeMenuSmoke" owner:app topLevelObjects:0]) abort();
    NSMenu *menu = [app mainMenu]; if (!menu || [menu numberOfItems] != 1) abort();
    [app setMainMenu:0]; unlink(path);
    puts("Interface Builder _NSMainMenu role installs NSApplication.mainMenu");
}
static void check_native_menu_model(void) {
    NSApplication *app = [NSApplication sharedApplication];
    NSMenu *root = [[NSMenu alloc] initWithTitle:@"Application"];
    NSMenu *submenu = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:@"File" action:0 keyEquivalent:@""];
    [root addItem:top]; [root setSubmenu:submenu forItem:top]; [app setMainMenu:root];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {200, 100}} styleMask:0 backing:2 defer:0];
    SmokeMenuTarget *target = [[SmokeMenuTarget alloc] initWithFrame:(SmokeRect){{0, 0}, {200, 100}}];
    [window setContentView:target]; [window makeFirstResponder:target]; [window makeKeyAndOrderFront:0];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Action" action:@selector(menuAction:) keyEquivalent:@"m"];
    [item setTag:42]; [submenu addItem:item];
    [submenu insertItem:[NSMenuItem separatorItem] atIndex:0];
    if ([submenu numberOfItems] != 2 || ![[submenu itemAtIndex:0] isSeparatorItem] || [submenu itemWithTag:42] != item || [item menu] != submenu) abort();
    [item setHidden:1]; if (![item isHidden]) abort(); [item setHidden:0];
    [item setKeyEquivalent:@"m"]; [item setKeyEquivalentModifierMask:1UL << 20];
    if ([item keyEquivalentModifierMask] != (1UL << 20) || strcmp([[item keyEquivalent] UTF8String], "m")) abort();
    SmokeKeyEvent *event = [[SmokeKeyEvent alloc] init]; event->text = @"m"; event->modifiers = 1UL << 20; event->window = window;
    [submenu update];
    if (![item isEnabled] || ![root performKeyEquivalent:event] || target->actions != 1 || target->last_sender != item) abort();
    if (![app sendAction:@selector(menuAction:) to:target from:item] || target->actions != 2) abort();
    [item setTag:99]; [submenu update];
    if ([item isEnabled] || [root performKeyEquivalent:event] || target->actions != 2) abort();
    [submenu removeItemAtIndex:0]; if ([submenu numberOfItems] != 1) abort();
    [submenu removeItemAtIndex:0]; if ([item menu] || [submenu numberOfItems]) abort();
    [app setMainMenu:0]; [window close];
    [event release]; [item release]; [top release]; [submenu release]; [root release]; [window release]; [target release];
    puts("native menu mutation, validation, Ctrl shortcut and responder-chain actions passed");
}

static void check_text_focus_and_shortcuts(void) {
    NSApplication *app = [NSApplication sharedApplication];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{0, 0}, {200, 100}} styleMask:0 backing:2 defer:0];
    SmokeShortcutView *root = [[SmokeShortcutView alloc] initWithFrame:(SmokeRect){{0, 0}, {200, 100}}];
    SmokeFocusView *editor = [[SmokeFocusView alloc] initWithFrame:(SmokeRect){{0, 0}, {100, 100}}];
    SmokeFocusView *other = [[SmokeFocusView alloc] initWithFrame:(SmokeRect){{100, 0}, {100, 100}}];
    [window setContentView:root]; [root addSubview:editor]; [root addSubview:other];
    if (![window makeFirstResponder:editor] || [window firstResponder] != editor) abort();
    editor->refuse_resign = 1;
    if ([window makeFirstResponder:other] || [window firstResponder] != editor || other->becomes) abort();
    editor->refuse_resign = 0; other->refuse_become = 1;
    if ([window makeFirstResponder:other] || [window firstResponder] != editor || editor->becomes != 2) abort();
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Application"];
    SmokeMenuTarget *action = [[SmokeMenuTarget alloc] initWithFrame:(SmokeRect){{0, 0}, {1, 1}}];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Solo" action:@selector(menuAction:) keyEquivalent:@"s"];
    [item setTarget:action]; [item setKeyEquivalentModifierMask:0]; [menu addItem:item]; [app setMainMenu:menu];
    SmokeKeyEvent *event = [[SmokeKeyEvent alloc] init]; event->code = 1; event->text = @"s"; event->window = window;
    for (unsigned i = 0; i < 3; ++i) [app sendEvent:event];
    if (editor->inserts != 3 || editor->keys != 3 || root->shortcuts || action->actions || [window firstResponder] != editor) abort();
    event->modifiers = 1UL << 17; event->text = @"S"; [app sendEvent:event];
    event->modifiers = 1UL << 19; event->text = @"ß"; [app sendEvent:event];
    event->modifiers = 0; event->text = @"с"; [app sendEvent:event];
    if (editor->inserts != 6 || root->shortcuts || action->actions) abort();
    event->text = @"s"; event->modifiers = 1UL << 20;
    [item setKeyEquivalentModifierMask:1UL << 20]; [app sendEvent:event];
    if (action->actions != 1 || editor->inserts != 6) abort();
    [item setEnabled:0]; [app sendEvent:event];
    if (root->shortcuts != 1 || editor->inserts != 6) abort();
    [window makeFirstResponder:root]; event->modifiers = 0;
    [item setEnabled:1]; [item setKeyEquivalentModifierMask:0]; [app sendEvent:event];
    if (action->actions != 2) abort();
    [item setEnabled:0]; [app sendEvent:event]; if (root->shortcuts != 2) abort();
    if (![window makeFirstResponder:editor]) abort();
    editor->refuse_resign = 1; [editor removeFromSuperview];
    if ([window firstResponder] || [editor window]) abort();
    [app sendEvent:event]; if (editor->inserts != 6 || root->shortcuts != 3) abort();
    [window makeFirstResponder:root];
    NSView *replacement = [[NSView alloc] initWithFrame:(SmokeRect){{0, 0}, {200, 100}}];
    [window setContentView:replacement];
    if ([window firstResponder] || [root window] || [other window]) abort();
    [replacement release];
    [app setMainMenu:0]; [window makeFirstResponder:0]; [window close];
    [event release]; [item release]; [menu release]; [action release];
    [window release]; [root release]; [editor release]; [other release];
    puts("text focus survives rejected transitions; typing precedes bare shortcuts; Command shortcuts remain available");
}

int main(void) {
    check_copy_properties();
    check_task_memory_info();
    check_resolver_addresses();
    check_cfurl_string_construction();
    if (load_count != 2 || loads_before_initializer != 2 || pthread_self() != load_thread) abort();
    puts("Objective-C class/category load ran before initializers on the main guest thread");
    check_view_geometry_notifications();
    check_font_attributes();
    check_cgl_handles();
    check_locale_conversions();
    check_file_type_descriptions();
    check_bookmark_files();
    check_dispatch_once();
    check_dispatch_workers();
    check_run_loop();
    check_deferred_block_lifetime();
    check_dragging_item_construction();
    check_dragging_completion_and_size_limits();
    check_cocoa_timers();
    check_keyboard_responder_paths();
    check_native_menu_model();
    check_text_focus_and_shortcuts();
    check_nib_application_menu();
    check_cursor_state();
    check_ordered_windows();
    check_modal_loop();
    check_event_loop_pools();
    check_window_destruction();
    check_multiple_layer_frames();
    check_slow_layer_single_frame();
    check_window_mouse_routing();
    check_flipped_view_coordinates();
    NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
    printf("version=%ld.%ld.%ld\n", version.majorVersion, version.minorVersion, version.patchVersion);
    if (version.majorVersion != 12 || version.minorVersion != 0 || version.patchVersion != 0) abort();

    NSView *view = [[SmokeView alloc] initWithFrame:(SmokeRect){{11, 22}, {640, 480}}];
    SmokeRect frame = [view frame];
    printf("frame=%g,%g %gx%g\n", frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
    if (frame.origin.x != 11 || frame.origin.y != 22 || frame.size.width != 640 || frame.size.height != 480) abort();
    SmokeSize backing = [view convertSizeToBacking:(SmokeSize){640, 480}];
    if (backing.width != 640 || backing.height != 480) abort();
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(SmokeRect){{11, 22}, {640, 480}}
        styleMask:0 backing:2 defer:0];
    SmokeRect content = [[window contentView] frame];
    printf("content=%g,%g %gx%g\n", content.origin.x, content.origin.y, content.size.width, content.size.height);
    if (content.origin.x != 0 || content.origin.y != 0 || content.size.width != 640 || content.size.height != 480) abort();

    char directory[4096];
    size_t needed = confstr(65537, directory, sizeof directory);
    if (!needed || needed > sizeof directory) abort();
    char short_buffer[2] = {'?', '?'};
    if (confstr(65537, short_buffer, sizeof short_buffer) != needed || short_buffer[1] != 0) abort();
    char path[4096];
    snprintf(path, sizeof path, "%ssmoke-XXXXXX", directory);
    int fd = mkstemp(path);
    if (fd < 0) abort();
    close(fd);
    if (unlink(path) != 0) abort();
    printf("temp directory writable: %s\n", directory);

    char subsystem[] = "runtime-smoke", category[] = "owned-strings";
    void *log = os_log_create(subsystem, category);
    if (objc_retain((id)log) != (id)log || [(id)log retain] != (id)log || os_retain(log) != log) abort();
    os_release(log);
    [(id)log release];
    os_release(log); /* ARC's remaining reference keeps the handle alive. */
    memset(subsystem, 'x', sizeof subsystem - 1);
    memset(category, 'x', sizeof category - 1);
    unsigned char arguments[2] = {0, 0};
    _os_log_impl(0, log, 1, "log strings survived caller mutation", arguments, sizeof arguments);
    objc_release((id)log);
    if (objc_retain((id)&_os_log_default) != (id)&_os_log_default) abort();
    objc_release((id)&_os_log_default);
    _os_log_impl(0, &_os_log_default, 1, "default log survived retain/release", arguments, sizeof arguments);
    if (objc_retain((id)&_os_log_disabled) != (id)&_os_log_disabled || os_log_type_enabled(&_os_log_disabled, 1)) abort();
    objc_release((id)&_os_log_disabled);
    _os_log_impl(0, &_os_log_disabled, 1, "disabled log should be silent", arguments, sizeof arguments);
    puts("os_log Objective-C and C ownership passed");
    puts("guest runtime smoke passed");
    return 0;
}
