#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void check(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "notifications: %s\n", message); exit(1); }
}
static void *post(void *object) {
    @autoreleasepool {
        [[NSNotificationCenter defaultCenter] postNotificationName:@"BackgroundUpdate" object:(id)object];
    }
    return NULL;
}
int main(void) {
    @autoreleasepool {
        NSOperationQueue *main = NSOperationQueue.mainQueue;
        check(main != nil && main == NSOperationQueue.mainQueue, "main queue identity");
        check(NSOperationQueue.currentQueue == main, "current main queue");
        __block BOOL operation = NO;
        [main addOperationWithBlock:^{ check(NSThread.isMainThread, "operation main thread"); operation = YES; }];
        check(!operation, "queued operation is deferred");
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
        check(operation, "main operation executed");
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        NSObject *object = [[NSObject alloc] init];
        __block int callbacks = 0;
        id token = [center addObserverForName:@"BackgroundUpdate" object:object queue:main usingBlock:^(NSNotification *note) {
            check(NSThread.isMainThread, "observer main thread");
            check(note.object == object, "retained notification object");
            ++callbacks;
        }];
        pthread_t thread;
        check(pthread_create(&thread, NULL, post, object) == 0, "posting thread");
        check(pthread_join(thread, NULL) == 0, "posting thread completes");
        check(callbacks == 0, "background notification not delivered on posting thread");
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
        check(callbacks == 1, "observer delivered on main loop");
        [center postNotificationName:@"BackgroundUpdate" object:nil];
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
        check(callbacks == 1, "nil poster does not match specified object");
        [center removeObserver:token];
        [center postNotificationName:@"BackgroundUpdate" object:object];
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
        check(callbacks == 1, "removed observer");
        __block id inlineToken = nil;
        __block BOOL inlineCalled = NO;
        inlineToken = [center addObserverForName:@"InlineUpdate" object:nil queue:nil usingBlock:^(NSNotification *note) {
            check([note.name isEqualToString:@"InlineUpdate"], "inline notification");
            [center removeObserver:inlineToken];
            inlineCalled = YES;
        }];
        [center postNotificationName:@"InlineUpdate" object:nil];
        check(inlineCalled, "nil queue synchronous and reentrant removal");
        [object release];
    }
    puts("Cocoa notification queues and reentrant observer delivery passed");
    return 0;
}
