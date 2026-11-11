#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CGDirectDisplayMetal.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void smoke_dispatch_retain(void *) __asm__("_dispatch_retain");
extern void smoke_dispatch_release(void *) __asm__("_dispatch_release");

static void check(int condition, const char* description) {
    if (condition) return;
    fprintf(stderr, "MEVK guest FAIL: %s\n", description);
    abort();
}

static void finish(id<MTLCommandBuffer> command) {
    [command commit];
    [command waitUntilCompleted];
    if (command.error) fprintf(stderr, "MEVK command error: %s\n", command.error.localizedDescription.UTF8String);
    check(command.status == MTLCommandBufferStatusCompleted && command.error == nil, "command completed");
}

static void check_identity(id<MTLDevice> device) {
    id<MTLDevice> again = MTLCreateSystemDefaultDevice();
    check(device == again, "default device identity");
    [again release];
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    check(devices.count == 1 && [devices objectAtIndex:0] == device, "device enumeration");
    [devices release];
    check([device conformsToProtocol:@protocol(MTLDevice)], "device protocol");
    check(![device supportsFamily:MTLGPUFamilyMac2], "unsupported GPU family is not advertised");
    check([device supportsTextureSampleCount:1] && ![device supportsTextureSampleCount:4],
          "single-sample rendering capability");
    id<MTLDevice> display_device = CGDirectDisplayCopyCurrentMetalDevice(1);
    check(display_device == device, "CoreGraphics display Metal device");
    [display_device release];
    check(CGDirectDisplayCopyCurrentMetalDevice(0) == nil &&
          CGDirectDisplayCopyCurrentMetalDevice(UINT32_MAX) == nil, "unknown display rejected");
    printf("Metal guest device: %s\n", device.name.UTF8String);
}

static void *signal_semaphore(void *context) {
    dispatch_semaphore_signal((dispatch_semaphore_t)context);
    return NULL;
}

static void check_semaphore(void) {
    check(dispatch_semaphore_create(-1) == nil, "negative semaphore count rejected");
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(3);
    check(semaphore && [(id)semaphore retain] == (id)semaphore,
          "Live frame semaphore Objective-C ownership");
    smoke_dispatch_release(semaphore);
    smoke_dispatch_retain(semaphore);
    [(id)semaphore release];
    check([(id)semaphore retainCount] == 1, "mixed dispatch and Objective-C ownership");
    for (int i = 0; i < 3; ++i)
        check(dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) == 0, "initial frame permits");
    check(dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) != 0, "empty semaphore poll");
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC);
    usleep(20000);
    check(dispatch_semaphore_wait(semaphore, deadline) != 0, "expired absolute semaphore deadline");
    check(dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC)) != 0,
          "finite semaphore timeout");
    check(dispatch_time(DISPATCH_TIME_FOREVER, 1) == DISPATCH_TIME_FOREVER &&
          dispatch_time(1, INT64_MIN) == 1 &&
          dispatch_time(DISPATCH_TIME_FOREVER - 1, 2) == DISPATCH_TIME_FOREVER,
          "dispatch deadline saturation");
    pthread_t worker;
    check(pthread_create(&worker, NULL, signal_semaphore, semaphore) == 0,
          "semaphore worker thread created");
    check(dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) == 0,
          "cross-thread semaphore signal");
    check(pthread_join(worker, NULL) == 0, "semaphore worker completed");
    check(dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) != 0,
          "timeout does not leave an extra permit");
    check(dispatch_semaphore_signal(semaphore) == 0 &&
          dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER) == 0,
          "signal without a waiter and forever wait");
    smoke_dispatch_release(semaphore);
    puts("Live frame semaphore ownership, deadlines and cross-thread signal passed");
}

static void check_blits(id<MTLDevice> device) {
    unsigned char pattern[256], expected[256];
    for (size_t i = 0; i < sizeof pattern; ++i) pattern[i] = (unsigned char)(i * 17 + 3);
    memset(expected, 0x6b, sizeof expected);
    id<MTLBuffer> source = [device newBufferWithBytes:pattern length:sizeof pattern options:MTLResourceStorageModeShared];
    id<MTLBuffer> scratch = [device newBufferWithLength:sizeof pattern options:MTLResourceHazardTrackingModeTracked];
    id<MTLBuffer> output = [device newBufferWithBytes:expected length:sizeof expected options:0];
    check(source && scratch && output, "shared buffer allocation");
    check(source.length == sizeof pattern && source.contents && source.device == device, "buffer length, mapping and device");
    check(source.storageMode == MTLStorageModeShared && source.cpuCacheMode == MTLCPUCacheModeDefaultCache,
          "shared/default-cache mode");
    check(source.hazardTrackingMode == MTLHazardTrackingModeTracked &&
          source.resourceOptions == MTLResourceHazardTrackingModeTracked, "tracked buffer hazards");
    check([source conformsToProtocol:@protocol(MTLBuffer)] && [source conformsToProtocol:@protocol(MTLResource)],
          "buffer protocols");
    NSMutableString* label = [NSMutableString stringWithUTF8String:"original label"];
    source.label = label;
    [label appendString:@" changed"];
    check([source.label isEqualToString:@"original label"], "label copies its string");
    source.label = nil;
    check(source.label == nil, "nil label");

    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(queue && queue.device == device, "queue device");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    check(command && command.commandQueue == queue && command.device == device && command.retainedReferences,
          "command ownership");
    check(command.status == MTLCommandBufferStatusNotEnqueued, "initial command state");
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    check(encoder && encoder.device == device, "encoder device");
    check([encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)] &&
          [encoder conformsToProtocol:@protocol(MTLCommandEncoder)], "encoder protocols");
    [encoder fillBuffer:scratch range:NSMakeRange(0, 256) value:0x12];
    [encoder copyFromBuffer:source sourceOffset:4 toBuffer:scratch destinationOffset:8 size:128];
    [encoder fillBuffer:scratch range:NSMakeRange(32, 16) value:0xad];
    [encoder copyFromBuffer:scratch sourceOffset:0 toBuffer:output destinationOffset:0 size:256];
    [encoder endEncoding];
    check(memcmp(output.contents, expected, sizeof expected) == 0, "recording leaves destination untouched");
    check([source retainCount] > 1 && [scratch retainCount] > 1, "commands retain Objective-C resource wrappers");
    [source release];
    [scratch release];
    finish(command);
    memset(expected, 0x12, sizeof expected);
    memcpy(expected + 8, pattern + 4, 128);
    memset(expected + 32, 0xad, 16);
    check(memcmp(output.contents, expected, sizeof expected) == 0, "Metal-to-Vulkan fill/copy readback and NSRange ABI");

    // Two separate encoders on one batch, then dependent queue submissions.
    id<MTLCommandBuffer> first = [queue commandBuffer];
    id<MTLBlitCommandEncoder> first_encoder = [first blitCommandEncoder];
    [first_encoder fillBuffer:output range:NSMakeRange(0, 128) value:0x47];
    [first_encoder endEncoding];
    id<MTLBlitCommandEncoder> second_encoder = [first blitCommandEncoder];
    [second_encoder fillBuffer:output range:NSMakeRange(128, 128) value:0x47];
    [second_encoder endEncoding];
    id<MTLBuffer> copied = [device newBufferWithLength:256 options:0];
    id<MTLCommandBuffer> second = [queue commandBuffer];
    id<MTLBlitCommandEncoder> copy_encoder = [second blitCommandEncoder];
    [copy_encoder copyFromBuffer:output sourceOffset:0 toBuffer:copied destinationOffset:0 size:256];
    [copy_encoder endEncoding];
    [first commit];
    [second commit];
    [second waitUntilCompleted];
    check(first.status == MTLCommandBufferStatusCompleted && second.status == MTLCommandBufferStatusCompleted,
          "dependent submissions completed");
    memset(expected, 0x47, sizeof expected);
    check(memcmp(copied.contents, expected, sizeof expected) == 0, "dependencies across Metal submissions");
    [copied release];
    [output release];
    [queue release];
    puts("Metal blit ordering, lifetime and GPU readback passed");
}

static void check_errors(id<MTLDevice> device) {
    check([device newBufferWithLength:0 options:0] == nil, "zero allocation rejected");
    check([device newBufferWithLength:64 options:MTLResourceStorageModePrivate] == nil, "private storage rejected");
    const void* missing_bytes = NULL;
    check([device newBufferWithBytes:missing_bytes length:64 options:0] == nil, "null source bytes rejected");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> data = [device newBufferWithLength:64 options:0];
    memset(data.contents, 0x33, 64);
    id<MTLCommandBuffer> invalid = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [invalid blitCommandEncoder];
    [encoder fillBuffer:data range:NSMakeRange(0, 64) value:0x99];
    [encoder fillBuffer:data range:NSMakeRange(UINT64_MAX, 4) value:0];
    [encoder endEncoding];
    [invalid commit];
    [invalid waitUntilCompleted];
    check(invalid.status == MTLCommandBufferStatusError && invalid.error, "invalid range produces command error");
    check([invalid.error.domain isEqualToString:MTLCommandBufferErrorDomain], "Metal command error domain");
    check([invalid.error.localizedDescription containsString:@"outside"], "command diagnostic preserved");
    for (int i = 0; i < 64; ++i) check(((unsigned char*)data.contents)[i] == 0x33, "invalid batch was not submitted");

    id<MTLCommandBuffer> active = [queue commandBuffer];
    id<MTLBlitCommandEncoder> active_encoder = [active blitCommandEncoder];
    [active_encoder fillBuffer:data range:NSMakeRange(0, 64) value:0x77];
    [active commit];
    check(active.status == MTLCommandBufferStatusError && active.error, "commit before endEncoding rejected");
    [active_encoder endEncoding];

    id<MTLCommandBuffer> overlapping = [queue commandBuffer];
    id<MTLBlitCommandEncoder> a = [overlapping blitCommandEncoder];
    check([overlapping blitCommandEncoder] == nil && overlapping.status == MTLCommandBufferStatusError,
          "simultaneous encoders rejected");
    [a endEncoding];

    id<MTLCommandBuffer> ended = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e = [ended blitCommandEncoder];
    [e endEncoding];
    [e fillBuffer:data range:NSMakeRange(0, 4) value:0];
    check(ended.status == MTLCommandBufferStatusError, "use after endEncoding rejected");

    id<MTLCommandBuffer> duplicate = [queue commandBuffer];
    id<MTLBlitCommandEncoder> d = [duplicate blitCommandEncoder];
    [d fillBuffer:data range:NSMakeRange(0, 64) value:0x18];
    [d endEncoding];
    [duplicate commit];
    [duplicate commit];
    [duplicate waitUntilCompleted];
    check(duplicate.status == MTLCommandBufferStatusError, "repeated commit rejected");
    for (int i = 0; i < 64; ++i)
        check(((unsigned char*)data.contents)[i] == 0x18, "wait still completes previously committed work after a misuse");

    NSError* shader_error = nil;
    check([device newLibraryWithSource:@"kernel void unused() {}" options:nil error:&shader_error] == nil && shader_error,
          "missing shader translator reports NSError");
    check([shader_error.domain isEqualToString:MTLLibraryErrorDomain], "Metal library error domain");
    [data release];
    [queue release];
    puts("Metal encoder validation and shader errors passed");
}

int main(void) {
    @autoreleasepool {
        check_semaphore();
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        check(device != nil, "MTLCreateSystemDefaultDevice");
        check_identity(device);
        check_blits(device);
        check_errors(device);
        [device release];
    }
    puts("MEVK Metal guest smoke passed");
    return 0;
}
