#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>

_Static_assert(sizeof(AudioObjectPropertyAddress) == 12, "property address");
_Static_assert(sizeof(AudioStreamBasicDescription) == 40, "ASBD");
_Static_assert(sizeof(AudioBuffer) == 16 && offsetof(AudioBuffer, mData) == 8, "buffer");
_Static_assert(offsetof(AudioBufferList, mBuffers) == 8, "buffer list");
_Static_assert(sizeof(SMPTETime) == 24 && sizeof(AudioTimeStamp) == 64 && offsetof(AudioTimeStamp, mFlags) == 56, "timestamp");
_Static_assert(sizeof(AudioValueRange) == 16 && sizeof(AudioComponentDescription) == 20 && sizeof(AURenderCallbackStruct) == 16, "audio ABI");
extern int machoman_audio_null_check(AudioDeviceIOProc, void *, const AudioStreamBasicDescription *);
extern int machoman_audio_callback_check(void *, void *, void *, void *);
extern int machoman_audio_callback_busy(void);
extern void machoman_audio_callback_nested(void *, void *);
struct context { unsigned calls, planar; uint64_t position; pthread_t thread; };
static pthread_key_t callback_key;
static OSStatus render(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
    const AudioTimeStamp *input_time, AudioBufferList *output, const AudioTimeStamp *output_time, void *refcon) {
    struct context *c = refcon;
    if (device != 0x100 || !now || !input || input->mNumberBuffers || !input_time || input_time->mFlags ||
        now != output_time || now->mSampleTime != c->position || now->mFlags != 7 || now->mRateScalar != 1 ||
        now->mHostTime != 1000000000ull + (uint64_t)((double)c->position * 1e9 / 48000)) return -10;
    uint64_t fpcr;
    __asm__("mrs %0, fpcr" : "=r"(fpcr));
    if (fpcr) return -11;
    if (!c->thread) c->thread = pthread_self();
    if (pthread_self() != c->thread || !machoman_audio_callback_busy()) return -12;
    if (!c->calls) {
        if (pthread_getspecific(callback_key) || pthread_setspecific(callback_key, c)) return -15;
    } else if (pthread_getspecific(callback_key) != c) return -15;
    if (output->mNumberBuffers != (c->planar ? 2 : 1)) return -13;
    unsigned frames = output->mBuffers[0].mDataByteSize / (c->planar ? 4 : 8);
    for (unsigned ch = 0; ch < 2; ++ch) {
        AudioBuffer *b = &output->mBuffers[c->planar ? ch : 0];
        if (b->mNumberChannels != (c->planar ? 1 : 2) || !b->mData || b->mDataByteSize != frames * b->mNumberChannels * 4) return -14;
        for (unsigned f = 0; f < frames; ++f)
            ((float *)b->mData)[c->planar ? f : f * 2 + ch] = (float)sin((double)(c->position + f) * (6.283185307179586 * 440 / 48000)) * (ch ? -0.25f : 0.25f);
    }
    c->position += frames; ++c->calls;
    fpcr = 1ull << 22; __asm__("msr fpcr, %0" : : "r"(fpcr));
    return 0;
}
static uint64_t fault_callback(void *p) {
    struct context *c = p;
    if (!c->thread) c->thread = pthread_self();
    *(volatile unsigned char *)(uintptr_t)1 = 1; return 0;
}
static uint64_t good_callback(void *p) {
    struct context *c = p;
    if (!c->thread) c->thread = pthread_self();
    if (pthread_self() != c->thread) return 1;
    ++c->calls; return 42;
}
static uint64_t nested_callback(void *p) {
    machoman_audio_callback_nested(fault_callback, p);
    ((struct context *)p)->calls += 100; // must not resume after the nested fault
    return 1;
}
int main(void) {
    if (pthread_key_create(&callback_key, 0)) abort();
    for (unsigned planar = 0; planar < 2; ++planar) {
        struct context c = {0}; c.planar = planar;
        AudioStreamBasicDescription f = {48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
            (planar ? kAudioFormatFlagIsNonInterleaved : 0), planar ? 4 : 8, 1, planar ? 4 : 8, 2, 32, 0};
        if (machoman_audio_null_check(render, &c, &f) || c.calls != 5 || c.position != 961 || !c.thread || c.thread == pthread_self()) abort();
    }
    struct context c = {0};
    if (machoman_audio_callback_check(fault_callback, nested_callback, good_callback, &c) || c.calls != 2 || !c.thread) abort();
    if (pthread_key_delete(callback_key)) abort();
    puts("CoreAudio ARM64 ABI, sine samples, IOProc args, prepared thread/FP state, stop and fault recovery passed");
    return 0;
}
