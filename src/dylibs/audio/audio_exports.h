#ifndef MACHOMAN_AUDIO_EXPORTS_H
#define MACHOMAN_AUDIO_EXPORTS_H
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define AUDIO_EXPORT(name) __attribute__((visibility("default"))) __asm__("_" #name)
inline bool audio_trace_enabled() {
    static const bool enabled = std::getenv("MACHOMAN_AUDIO_TRACE") != nullptr;
    return enabled;
}
inline void audio_trace_property(const char *operation, std::uint32_t object, std::uint32_t selector,
                                 std::uint32_t scope, std::uint32_t element, std::int32_t status) {
    if (audio_trace_enabled()) std::fprintf(stderr, "audio: %s object=%u property=0x%08x scope=0x%08x element=%u status=%d\n",
        operation, object, selector, scope, element, status);
}

extern "C" {
void *audio_cf_string(void *, const char *, std::uint32_t) __asm__("_CFStringCreateWithCString");
int audio_cf_get_string(void *, char *, long, std::uint32_t) __asm__("_CFStringGetCString");
void *audio_global_queue(long, unsigned long) __asm__("_dispatch_get_global_queue");
}
#endif
