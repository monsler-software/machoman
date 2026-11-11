/* CoreAudio HAL facade. Physical devices remain unavailable until the
 * PipeWire output transport is ready. The PCM engine is tested offline. */
#include "darwin_audio_abi.h"
#include <cstdint>
#include <ctime>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

extern "C" {

/* ---- CoreAudio ----------------------------------------------------------- */

/* Mach absolute time is nanoseconds here (see libSystem), so the host-time
 * conversions are the identity. */
std::uint64_t AudioConvertHostTimeToNanos_(std::uint64_t t) DARWIN(AudioConvertHostTimeToNanos);
std::uint64_t AudioConvertHostTimeToNanos_(std::uint64_t t) { return t; }
std::uint64_t AudioConvertNanosToHostTime_(std::uint64_t t) DARWIN(AudioConvertNanosToHostTime);
std::uint64_t AudioConvertNanosToHostTime_(std::uint64_t t) { return t; }
std::uint64_t AudioGetCurrentHostTime_() DARWIN(AudioGetCurrentHostTime);
std::uint64_t AudioGetCurrentHostTime_() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

/* The system object answers "no devices"; every other query says the
 * property is unknown (kAudioHardwareUnknownPropertyError). */
constexpr std::uint32_t kAudioHardwareNoError = 0, kAudioHardwareUnknownPropertyError = 0x77686f3f /* 'who?' */;
constexpr std::uint32_t kAudioHardwarePropertyDevices = 0x64657623 /* 'dev#' */;

using AudioObjectPropertyAddress = mm::audio::darwin::property_address;

std::int32_t AudioObjectGetPropertyDataSize_(std::uint32_t, const AudioObjectPropertyAddress *addr, std::uint32_t,
                                             const void *, std::uint32_t *size) DARWIN(AudioObjectGetPropertyDataSize);
std::int32_t AudioObjectGetPropertyDataSize_(std::uint32_t, const AudioObjectPropertyAddress *addr, std::uint32_t,
                                             const void *, std::uint32_t *size) {
    if (addr && addr->selector == kAudioHardwarePropertyDevices) {
        if (size) *size = 0;
        return kAudioHardwareNoError;
    }
    if (size) *size = 0;
    return static_cast<std::int32_t>(kAudioHardwareUnknownPropertyError);
}
std::int32_t AudioObjectGetPropertyData_(std::uint32_t, const AudioObjectPropertyAddress *addr, std::uint32_t,
                                         const void *, std::uint32_t *size, void *) DARWIN(AudioObjectGetPropertyData);
std::int32_t AudioObjectGetPropertyData_(std::uint32_t, const AudioObjectPropertyAddress *addr, std::uint32_t,
                                         const void *, std::uint32_t *size, void *) {
    if (addr && addr->selector == kAudioHardwarePropertyDevices) {
        if (size) *size = 0;
        return kAudioHardwareNoError;
    }
    if (size) *size = 0;
    return static_cast<std::int32_t>(kAudioHardwareUnknownPropertyError);
}
std::int32_t AudioObjectSetPropertyData_(std::uint32_t, const void *, std::uint32_t, const void *, std::uint32_t,
                                         const void *) DARWIN(AudioObjectSetPropertyData);
std::int32_t AudioObjectSetPropertyData_(std::uint32_t, const void *, std::uint32_t, const void *, std::uint32_t,
                                         const void *) {
    return static_cast<std::int32_t>(kAudioHardwareUnknownPropertyError);
}
std::uint8_t AudioObjectHasProperty_(std::uint32_t, const AudioObjectPropertyAddress *addr) DARWIN(AudioObjectHasProperty);
std::uint8_t AudioObjectHasProperty_(std::uint32_t, const AudioObjectPropertyAddress *addr) {
    return addr && addr->selector == kAudioHardwarePropertyDevices ? 1 : 0;
}
std::int32_t AudioObjectAddPropertyListenerBlock_(std::uint32_t, const void *, void *, void *) DARWIN(AudioObjectAddPropertyListenerBlock);
std::int32_t AudioObjectAddPropertyListenerBlock_(std::uint32_t, const void *, void *, void *) { return 0; }
std::int32_t AudioObjectRemovePropertyListenerBlock_(std::uint32_t, const void *, void *, void *) DARWIN(AudioObjectRemovePropertyListenerBlock);
std::int32_t AudioObjectRemovePropertyListenerBlock_(std::uint32_t, const void *, void *, void *) { return 0; }

/* No device exists, so anything asked of one fails with
 * kAudioHardwareBadDeviceError ('!dev'). */
constexpr std::int32_t kAudioHardwareBadDeviceError = 0x21646576;
std::int32_t AudioDeviceCreateIOProcID_(std::uint32_t, void *, void *, void **out) DARWIN(AudioDeviceCreateIOProcID);
std::int32_t AudioDeviceCreateIOProcID_(std::uint32_t, void *, void *, void **out) {
    if (out) *out = nullptr;
    return kAudioHardwareBadDeviceError;
}
std::int32_t AudioDeviceDestroyIOProcID_(std::uint32_t, void *) DARWIN(AudioDeviceDestroyIOProcID);
std::int32_t AudioDeviceDestroyIOProcID_(std::uint32_t, void *) { return kAudioHardwareBadDeviceError; }
std::int32_t AudioDeviceStart_(std::uint32_t, void *) DARWIN(AudioDeviceStart);
std::int32_t AudioDeviceStart_(std::uint32_t, void *) { return kAudioHardwareBadDeviceError; }
std::int32_t AudioDeviceStop_(std::uint32_t, void *) DARWIN(AudioDeviceStop);
std::int32_t AudioDeviceStop_(std::uint32_t, void *) { return kAudioHardwareBadDeviceError; }
std::int32_t AudioDeviceTranslateTime_(std::uint32_t, const void *, void *) DARWIN(AudioDeviceTranslateTime);
std::int32_t AudioDeviceTranslateTime_(std::uint32_t, const void *, void *) { return kAudioHardwareBadDeviceError; }

} // extern "C"
