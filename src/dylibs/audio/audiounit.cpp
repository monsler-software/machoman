/* AudioComponent facade. Output components will use the common PCM
 * engine once HAL/PipeWire output is available. */
#include <cstdint>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

extern "C" {

/* ---- AudioToolbox: no components ---------------------------------------- */

void *AudioComponentFindNext_(void *, const void *) DARWIN(AudioComponentFindNext);
void *AudioComponentFindNext_(void *, const void *) { return nullptr; }
std::int32_t AudioComponentInstanceNew_(void *, void **out) DARWIN(AudioComponentInstanceNew);
std::int32_t AudioComponentInstanceNew_(void *, void **out) {
    if (out) *out = nullptr;
    return -3000; /* badComponentInstance */
}
std::int32_t AudioComponentInstanceDispose_(void *) DARWIN(AudioComponentInstanceDispose);
std::int32_t AudioComponentInstanceDispose_(void *) { return 0; }
void *AudioComponentInstanceGetComponent_(void *) DARWIN(AudioComponentInstanceGetComponent);
void *AudioComponentInstanceGetComponent_(void *) { return nullptr; }
std::int32_t AudioComponentGetDescription_(void *, void *) DARWIN(AudioComponentGetDescription);
std::int32_t AudioComponentGetDescription_(void *, void *) { return -3000; }
std::int32_t AudioComponentCopyName_(void *, void **out) DARWIN(AudioComponentCopyName);
std::int32_t AudioComponentCopyName_(void *, void **out) {
    if (out) *out = nullptr;
    return -3000;
}

} // extern "C"
