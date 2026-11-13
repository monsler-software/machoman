/* SDK-compiled integration test: the same public APIs used by DAWs and games.
 * The PipeWire fixture captures the samples and supplies hotplug/default events. */
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "audio check failed at line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static OSStatus io_render(AudioDeviceID, const AudioTimeStamp *, const AudioBufferList *, const AudioTimeStamp *,
                          AudioBufferList *, const AudioTimeStamp *, void *);
static AudioObjectPropertyAddress address(UInt32 selector, UInt32 scope) {
    return (AudioObjectPropertyAddress){selector, scope, kAudioObjectPropertyElementMain};
}
static UInt32 scalar(AudioObjectID object, UInt32 selector, UInt32 scope) {
    AudioObjectPropertyAddress a = address(selector, scope);
    UInt32 result = 0xdeadbeef, size = sizeof result;
    CHECK(AudioObjectGetPropertyData(object, &a, 0, NULL, &size, &result) == noErr && size == sizeof result);
    return result;
}
static UInt32 unit_scalar(AudioUnit unit, UInt32 property, UInt32 scope, UInt32 element) {
    UInt32 result = 0xdeadbeef, size = sizeof result;
    CHECK(AudioUnitGetProperty(unit, property, scope, element, &result, &size) == noErr && size == sizeof result);
    return result;
}
static AudioComponent output_component(UInt32 subtype) {
    AudioComponentDescription wanted = {kAudioUnitType_Output, subtype, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(NULL, &wanted);
    CHECK(component && !AudioComponentFindNext(component, &wanted) && AudioComponentCount(&wanted) == 1);
    AudioComponentDescription actual;
    CHECK(!AudioComponentGetDescription(component, &actual) && actual.componentSubType == subtype);
    CFStringRef name = NULL;
    CHECK(!AudioComponentCopyName(component, &name) && name && CFStringGetLength(name)); CFRelease(name);
    return component;
}
static void api_checks(int none) {
    AudioObjectPropertyAddress devices = address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 size = 99;
    CHECK(AudioObjectHasProperty(kAudioObjectSystemObject, &devices));
    CHECK(!AudioObjectHasProperty(0xdeadbeef, &devices));
    CHECK(AudioObjectGetPropertyDataSize(0xdeadbeef, &devices, 0, NULL, &size) != noErr && size == 0);
    CHECK(!AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices, 0, NULL, &size));
    CHECK(none ? size == 0 : size >= sizeof(AudioDeviceID));
    if (none) { UInt32 untouched = 0x12345678; CHECK(!AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices, 0, NULL, &size, &untouched)); CHECK(untouched == 0x12345678); }
    CHECK(scalar(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal) == 0);
    CHECK((scalar(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal) == 0) == none);
    AudioDeviceIOProcID bad = (AudioDeviceIOProcID)1;
    CHECK(AudioDeviceCreateIOProcID(0xdeadbeef, io_render, NULL, &bad) != noErr && !bad);
    AudioUnit unit = NULL;
    AudioComponent component = output_component(kAudioUnitSubType_DefaultOutput);
    CHECK(!AudioComponentInstanceNew(component, &unit) && unit);
    CHECK(AudioComponentInstanceGetComponent(unit) == component);
    CHECK(AudioOutputUnitStart(unit) == kAudioUnitErr_Uninitialized);
    struct { Boolean writable; unsigned char guard[3]; } result = {0xff, {0xaa, 0xbb, 0xcc}};
    CHECK(!AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &size, &result.writable));
    CHECK(size == sizeof(AudioStreamBasicDescription) && result.writable == 1 && result.guard[0] == 0xaa && result.guard[2] == 0xcc);
    size = 3; UInt32 guard = 0x12345678;
    CHECK(AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &guard, &size) != noErr);
    CHECK(guard == 0x12345678 && size == sizeof guard);
    UInt32 enable = 1;
    CHECK(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enable, sizeof enable) != noErr);
    CHECK(!unit_scalar(unit, kAudioOutputUnitProperty_HasIO, kAudioUnitScope_Input, 1));
    if (none) CHECK(AudioUnitInitialize(unit) != noErr);
    CHECK(!AudioComponentInstanceDispose(unit));
    CHECK(AudioComponentInstanceDispose(unit) != noErr);
    CHECK(!AudioComponentInstanceGetComponent(unit));
}
static atomic_uint property_calls, block_calls, default_calls, device_calls, unit_device_calls;
static void unit_device_listener(void *refcon, AudioUnit unit, AudioUnitPropertyID property,
                                 AudioUnitScope scope, AudioUnitElement element) {
    if (unit == *(AudioUnit *)refcon && property == kAudioOutputUnitProperty_CurrentDevice &&
        scope == kAudioUnitScope_Global && element == 0) atomic_fetch_add(&unit_device_calls, 1);
}
static OSStatus property_listener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses, void *refcon) {
    if (!object || count != 1 || !addresses || refcon != &property_calls) return -1;
    atomic_fetch_add(&property_calls, 1); return 0;
}
static OSStatus system_listener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses, void *refcon) {
    (void)refcon;
    if (object != kAudioObjectSystemObject || count != 1) return -1;
    if (addresses[0].mSelector == kAudioHardwarePropertyDefaultOutputDevice) atomic_fetch_add(&default_calls, 1);
    if (addresses[0].mSelector == kAudioHardwarePropertyDevices) atomic_fetch_add(&device_calls, 1);
    return 0;
}
static void wait_count(atomic_uint *value, unsigned count) {
    for (unsigned i = 0; i < 500 && atomic_load(value) < count; ++i) usleep(10000);
    CHECK(atomic_load(value) >= count);
}
static void device_checks(AudioDeviceID device) {
    AudioObjectPropertyAddress a = address(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
    CFStringRef name = NULL; UInt32 size = sizeof name;
    CHECK(!AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &name) && name);
    char text[1024]; CHECK(CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8));
    printf("CoreAudio output: %s\n", text); CFRelease(name);
    a.mSelector = kAudioDevicePropertyDeviceUID;
    size = sizeof name; CHECK(!AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &name));
    AudioObjectPropertyAddress translate = address(kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal);
    AudioDeviceID translated = 0; size = sizeof translated;
    CHECK(!AudioObjectGetPropertyData(kAudioObjectSystemObject, &translate, sizeof name, &name, &size, &translated));
    CHECK(translated == device); CFRelease(name);
    a = address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
    CHECK(!AudioObjectGetPropertyDataSize(device, &a, 0, NULL, &size) && size == offsetof(AudioBufferList, mBuffers));
    AudioBufferList buffers = {0}; size = sizeof buffers;
    CHECK(!AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &buffers) && buffers.mNumberBuffers == 0);
    a.mScope = kAudioObjectPropertyScopeOutput; size = sizeof buffers;
    CHECK(!AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &buffers));
    CHECK(buffers.mNumberBuffers == 1 && buffers.mBuffers[0].mNumberChannels == 2 && !buffers.mBuffers[0].mData);
    AudioStreamID stream = scalar(device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    a = address(kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal);
    AudioStreamBasicDescription format; size = sizeof format;
    CHECK(!AudioObjectGetPropertyData(stream, &a, 0, NULL, &size, &format));
    CHECK(format.mFormatID == kAudioFormatLinearPCM && format.mBitsPerChannel == 32 && format.mBytesPerFrame == 8);
    UInt32 guard = 0x12345678; size = 3;
    a = address(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    CHECK(scalar(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeOutput) ==
          scalar(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal));
    AudioObjectPropertyAddress rate_address = address(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeOutput);
    Float64 rate = 0; UInt32 rate_size = sizeof rate;
    CHECK(!AudioObjectGetPropertyData(device, &rate_address, 0, NULL, &rate_size, &rate) && rate == 48000);
    CHECK(AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &guard) == kAudioHardwareBadPropertySizeError);
    CHECK(guard == 0x12345678 && size == sizeof guard);
    Boolean settable = false;
    CHECK(!AudioObjectIsPropertySettable(device, &a, &settable) && settable);
    dispatch_queue_t queue = dispatch_queue_create("audio-property-test", NULL);
    AudioObjectPropertyListenerBlock block = ^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
        if (count == 1 && addresses[0].mSelector == kAudioDevicePropertyBufferFrameSize) atomic_fetch_add(&block_calls, 1);
    };
    CHECK(!AudioObjectAddPropertyListener(device, &a, property_listener, &property_calls));
    CHECK(!AudioObjectAddPropertyListenerBlock(device, &a, queue, block));
    UInt32 frames = 128;
    CHECK(!AudioObjectSetPropertyData(device, &a, 0, NULL, sizeof frames, &frames));
    wait_count(&property_calls, 1); wait_count(&block_calls, 1);
    CHECK(!AudioObjectRemovePropertyListener(device, &a, property_listener, &property_calls));
    CHECK(!AudioObjectRemovePropertyListenerBlock(device, &a, queue, block));
    unsigned functions = atomic_load(&property_calls), blocks = atomic_load(&block_calls);
    frames = 64; CHECK(!AudioObjectSetPropertyData(device, &a, 0, NULL, sizeof frames, &frames));
    usleep(150000); CHECK(atomic_load(&property_calls) == functions && atomic_load(&block_calls) == blocks);
    frames = 128; CHECK(!AudioObjectSetPropertyData(device, &a, 0, NULL, sizeof frames, &frames));
    dispatch_release(queue);
}
struct render_context {
    AudioDeviceID device;
    AudioDeviceIOProcID proc;
    AudioUnit unit;
    atomic_uint calls, stopped, failures;
    unsigned planar, stop_inside, fail, expected_frames;
    pthread_t thread;
};
static OSStatus unit_render(void *opaque, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                            UInt32 bus, UInt32 frames, AudioBufferList *data) {
    struct render_context *c = opaque;
    if (!c->thread) c->thread = pthread_self();
    if (!flags || !time || bus || !frames || frames > 128 || pthread_self() != c->thread ||
        !(time->mFlags & kAudioTimeStampHostTimeValid) || data->mNumberBuffers != (c->planar ? 2 : 1)) {
        atomic_fetch_add(&c->failures, 1); return -99;
    }
    for (UInt32 ch = 0; ch < 2; ++ch) {
        AudioBuffer *b = &data->mBuffers[c->planar ? ch : 0];
        if (b->mNumberChannels != (c->planar ? 1 : 2) || b->mDataByteSize != frames * b->mNumberChannels * 4) {
            atomic_fetch_add(&c->failures, 1); return -99;
        }
        for (UInt32 f = 0; f < frames; ++f) ((float *)b->mData)[c->planar ? f : 2 * f + ch] = ch ? -0.125f : 0.25f;
    }
    const unsigned calls = atomic_fetch_add(&c->calls, 1) + 1;
    if (c->fail == 1 && calls == 4) return -123;
    if (c->fail == 2 && calls == 4) *(volatile unsigned char *)(uintptr_t)1 = 1;
    if (c->fail == 3 && calls == 4) data->mNumberBuffers = 99;
    if (c->stop_inside && calls == 4) {
        if (AudioOutputUnitStop(c->unit)) atomic_fetch_add(&c->failures, 1);
        atomic_store(&c->stopped, 1);
    }
    return noErr;
}
static OSStatus io_render(AudioDeviceID device, const AudioTimeStamp *now, const AudioBufferList *input,
                          const AudioTimeStamp *input_time, AudioBufferList *data, const AudioTimeStamp *time, void *opaque) {
    struct render_context *c = opaque;
    if (!c->thread) c->thread = pthread_self();
    if (device != c->device || !now || !input || input->mNumberBuffers || !input_time || input_time->mFlags ||
        !time || data->mNumberBuffers != 1 || data->mBuffers[0].mNumberChannels != 2 || pthread_self() != c->thread) {
        atomic_fetch_add(&c->failures, 1); return -99;
    }
    const UInt32 frames = data->mBuffers[0].mDataByteSize / 8;
    if (frames != c->expected_frames) { atomic_fetch_add(&c->failures, 1); return -99; }
    AudioTimeStamp sample = {.mSampleTime = time->mSampleTime, .mFlags = kAudioTimeStampSampleTimeValid};
    AudioTimeStamp translated = {.mFlags = kAudioTimeStampHostTimeValid};
    if (AudioDeviceTranslateTime(device, &sample, &translated) ||
        llabs((long long)translated.mHostTime - (long long)time->mHostTime) > 256) {
        atomic_fetch_add(&c->failures, 1); return -99;
    }
    float *samples = data->mBuffers[0].mData;
    for (UInt32 f = 0; f < frames; ++f) { samples[f * 2] = 0.125f; samples[f * 2 + 1] = -0.25f; }
    const unsigned calls = atomic_fetch_add(&c->calls, 1) + 1;
    if (c->stop_inside && calls == 8) {
        if (AudioDeviceStop(device, c->proc)) atomic_fetch_add(&c->failures, 1);
        atomic_store(&c->stopped, 1);
    }
    return noErr;
}
static void hal_playback(AudioDeviceID device) {
    // Deliberately not a power of two: the PipeWire graph may use a different
    // quantum, but the HAL IOProc must still get the requested block size.
    const UInt32 frames = 96;
    AudioObjectPropertyAddress a = address(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeOutput);
    CHECK(!AudioObjectSetPropertyData(device, &a, 0, NULL, sizeof frames, &frames));
    struct render_context c = {.device = device, .stop_inside = 1, .expected_frames = frames};
    CHECK(!AudioDeviceCreateIOProcID(device, io_render, &c, &c.proc));
    CHECK(!AudioDeviceStart(device, c.proc)); wait_count(&c.stopped, 1);
    CHECK(!AudioDeviceStop(device, c.proc) && !atomic_load(&c.failures));
    unsigned calls = atomic_load(&c.calls); usleep(60000); CHECK(atomic_load(&c.calls) == calls);
    CHECK(c.thread && c.thread != pthread_self());
    c.thread = NULL; c.stop_inside = 0;
    CHECK(!AudioDeviceStart(device, c.proc)); wait_count(&c.calls, calls + 8);
    CHECK(!AudioDeviceStop(device, c.proc) && !atomic_load(&c.failures));
    CHECK(!AudioDeviceDestroyIOProcID(device, c.proc));
    CHECK(AudioDeviceDestroyIOProcID(device, c.proc) != noErr);
}
static void make_unit(struct render_context *c, UInt32 subtype) {
    CHECK(!AudioComponentInstanceNew(output_component(subtype), &c->unit));
    if (subtype == kAudioUnitSubType_HALOutput)
        CHECK(!AudioUnitSetProperty(c->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &c->device, sizeof c->device));
    AudioStreamBasicDescription format = {48000, kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | (c->planar ? kAudioFormatFlagIsNonInterleaved : 0),
        c->planar ? 4 : 8, 1, c->planar ? 4 : 8, 2, 32, 0};
    CHECK(!AudioUnitSetProperty(c->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof format));
    UInt32 frames = 128;
    CHECK(!AudioUnitSetProperty(c->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &frames, sizeof frames));
    AURenderCallbackStruct callback = {unit_render, c};
    CHECK(!AudioUnitSetProperty(c->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof callback));
    CHECK(!AudioUnitInitialize(c->unit));
}
static void unit_playback(AudioDeviceID device) {
    struct render_context c = {.device = device, .planar = 1, .stop_inside = 1};
    make_unit(&c, kAudioUnitSubType_HALOutput);
    struct { UInt32 count; AudioBuffer buffers[2]; } data = {2, {{1, 0, NULL}, {1, 0, NULL}}};
    AudioTimeStamp time = {.mHostTime = AudioGetCurrentHostTime(), .mFlags = kAudioTimeStampHostTimeValid};
    AudioUnitRenderActionFlags flags = 0;
    CHECK(!AudioUnitRender(c.unit, &flags, &time, 0, 64, (AudioBufferList *)&data));
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(data.buffers[i].mData && data.buffers[i].mDataByteSize == 256);
        CHECK(((float *)data.buffers[i].mData)[63] == (i ? -0.125f : 0.25f));
    }
    c.thread = NULL; atomic_store(&c.calls, 0);
    CHECK(!AudioOutputUnitStart(c.unit)); wait_count(&c.stopped, 1);
    CHECK(!AudioOutputUnitStop(c.unit) && !atomic_load(&c.failures));
    unsigned calls = atomic_load(&c.calls); usleep(60000); CHECK(atomic_load(&c.calls) == calls);
    CHECK(!unit_scalar(c.unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0));
    CHECK(!AudioUnitUninitialize(c.unit));
    c.thread = NULL; c.stop_inside = 0; c.fail = 1; atomic_store(&c.calls, 0);
    CHECK(!AudioUnitInitialize(c.unit) && !AudioOutputUnitStart(c.unit)); wait_count(&c.calls, 4);
    for (unsigned i = 0; i < 100 && unit_scalar(c.unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0); ++i) usleep(10000);
    CHECK(!unit_scalar(c.unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0));
    CHECK((SInt32)unit_scalar(c.unit, kAudioUnitProperty_LastRenderError, kAudioUnitScope_Global, 0) == -123);
    CHECK(!AudioOutputUnitStop(c.unit) && !AudioUnitUninitialize(c.unit));
    for (unsigned failure = 2; failure <= 3; ++failure) {
        c.thread = NULL; c.fail = failure; atomic_store(&c.calls, 0);
        CHECK(!AudioUnitInitialize(c.unit) && !AudioOutputUnitStart(c.unit)); wait_count(&c.calls, 4);
        for (unsigned i = 0; i < 100 && unit_scalar(c.unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0); ++i) usleep(10000);
        CHECK(!unit_scalar(c.unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0));
        CHECK(unit_scalar(c.unit, kAudioUnitProperty_LastRenderError, kAudioUnitScope_Global, 0) != 0);
        CHECK(!AudioOutputUnitStop(c.unit) && !AudioUnitUninitialize(c.unit));
    }
    CHECK(!AudioComponentInstanceDispose(c.unit) && !atomic_load(&c.failures));
}
static void default_and_hotplug(AudioDeviceID first) {
    AudioObjectPropertyAddress def = address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectPropertyAddress devices = address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    CHECK(!AudioObjectAddPropertyListener(kAudioObjectSystemObject, &def, system_listener, NULL));
    CHECK(!AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devices, system_listener, NULL));
    struct render_context c = {0}; make_unit(&c, kAudioUnitSubType_DefaultOutput);
    CHECK(!AudioUnitAddPropertyListener(c.unit, kAudioOutputUnitProperty_CurrentDevice, unit_device_listener, &c.unit));
    CHECK(!AudioOutputUnitStart(c.unit)); wait_count(&c.calls, 8);
    CHECK(scalar(first, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal));
    puts("CHANGE_DEFAULT"); fflush(stdout); wait_count(&default_calls, 1);
    wait_count(&unit_device_calls, 1);
    AudioDeviceID second = scalar(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    CHECK(second && second != first);
    CHECK(unit_scalar(c.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0) == second);
    CHECK(!scalar(first, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal));
    CHECK(scalar(second, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal));
    unsigned calls = atomic_load(&c.calls); wait_count(&c.calls, calls + 24);
    puts("REMOVE_DEVICE"); fflush(stdout); wait_count(&device_calls, 1); wait_count(&default_calls, 2);
    wait_count(&unit_device_calls, 2);
    CHECK(scalar(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal) == first);
    UInt32 size = 0; CHECK(!AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices, 0, NULL, &size) && size == sizeof(AudioDeviceID));
    CHECK(unit_scalar(c.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0) == first);
    CHECK(scalar(first, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal));
    calls = atomic_load(&c.calls); wait_count(&c.calls, calls + 8);
    CHECK(!AudioOutputUnitStop(c.unit) && !AudioUnitUninitialize(c.unit) && !AudioComponentInstanceDispose(c.unit));
    CHECK(!scalar(first, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal));
    CHECK(!atomic_load(&c.failures));
    CHECK(!AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &def, system_listener, NULL));
    CHECK(!AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &devices, system_listener, NULL));
}
static void selected_device_disconnect(void) {
    CFStringRef uid = CFSTR("pipewire:machoman-test-sink-3");
    AudioObjectPropertyAddress a = address(kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal);
    AudioDeviceID device = 0; UInt32 size = sizeof device;
    CHECK(!AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &device) && device);
    struct render_context c = {.device = device};
    make_unit(&c, kAudioUnitSubType_HALOutput);
    CHECK(!AudioOutputUnitStart(c.unit)); wait_count(&c.calls, 8);
    puts("DISCONNECT_DEVICE"); fflush(stdout);
    for (unsigned i = 0; i < 500 && unit_scalar(c.unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0); ++i) usleep(10000);
    CHECK(!unit_scalar(c.unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0));
    CHECK(unit_scalar(c.unit, kAudioUnitProperty_LastRenderError, kAudioUnitScope_Global, 0));
    CHECK(AudioOutputUnitStart(c.unit) != noErr);
    CHECK(!AudioOutputUnitStop(c.unit) && !AudioComponentInstanceDispose(c.unit));
    const unsigned calls = atomic_load(&c.calls); usleep(60000);
    CHECK(atomic_load(&c.calls) == calls && !atomic_load(&c.failures));
}
int main(int argc, char **argv) {
    const int none = argc > 1 && !strcmp(argv[1], "none");
    if (argc > 1 && !strcmp(argv[1], "inspect")) {
        AudioObjectPropertyAddress a = address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
        UInt32 size = 0; CHECK(!AudioObjectGetPropertyDataSize(1, &a, 0, NULL, &size));
        AudioDeviceID *ids = malloc(size ? size : 1); CHECK(!AudioObjectGetPropertyData(1, &a, 0, NULL, &size, ids));
        AudioDeviceID def = scalar(1, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
        for (unsigned i = 0; i < size / sizeof *ids; ++i) {
            CFStringRef name; UInt32 len = sizeof name;
            a = address(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
            CHECK(!AudioObjectGetPropertyData(ids[i], &a, 0, NULL, &len, &name));
            char text[1024]; CHECK(CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8));
            printf("%u%s: %s\n", ids[i], ids[i] == def ? " (default)" : "", text); CFRelease(name);
        }
        free(ids); return 0;
    }
    api_checks(none);
    if (!none) {
        const AudioDeviceID device = scalar(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
        device_checks(device); hal_playback(device); unit_playback(device);
        selected_device_disconnect(); default_and_hotplug(device);
        // Returning from main must quiesce an output the application forgot to stop.
        struct render_context *remaining = calloc(1, sizeof *remaining); CHECK(remaining);
        make_unit(remaining, kAudioUnitSubType_DefaultOutput);
        CHECK(!AudioOutputUnitStart(remaining->unit)); wait_count(&remaining->calls, 8);
    }
    puts(none ? "CoreAudio no-device and AudioUnit validation passed" :
        "CoreAudio PipeWire HAL, AudioUnit PCM, callbacks, stop, errors, defaults and hotplug passed");
    return 0;
}
