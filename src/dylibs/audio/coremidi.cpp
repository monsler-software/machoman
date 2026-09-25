/* CoreMIDI and CoreAudio with no devices.
 *
 * Audio and MIDI are deliberately not implemented yet; what a program needs
 * from them before it can show its interface is an honest "there is
 * nothing here": calls succeed, clients and ports are real (non-zero)
 * object references, and every device count is zero. Returning 0 from a
 * create call instead — which is what the generic stub does — looks like a
 * null client and makes programs crash rather than carry on.
 *
 * Anything that would actually move audio or MIDI data reports that it is
 * not implemented through the shared symbol synthesis. */
#include "machoman/objc_framework.h"
#include "synthesise.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

namespace {

/* Object references handed to the program; each is just a token. */
std::mutex g_mu;
std::uint32_t g_next_ref = 0x100;
std::uint32_t new_ref() {
    std::lock_guard lock(g_mu);
    return g_next_ref++;
}

bool trace() { static const bool on = std::getenv("MACHOMAN_STUB_TRACE") != nullptr; return on; }

/* MIDIPacketList, as the program lays it out: a count followed by packets
 * of {timestamp, length, data[]}. The list functions here only have to
 * build a well-formed list; nothing consumes it. */
struct MIDIPacket {
    std::uint64_t timestamp;
    std::uint16_t length;
    std::uint8_t data[256];
};
struct MIDIPacketList {
    std::uint32_t numPackets;
    MIDIPacket packet[1];
};

} // namespace

extern "C" {

/* ---- CoreMIDI ------------------------------------------------------------ */

std::int32_t MIDIClientCreate_(void *name, void *notify, void *ctx, std::uint32_t *out) DARWIN(MIDIClientCreate);
std::int32_t MIDIClientCreate_(void *name, void *notify, void *ctx, std::uint32_t *out) {
    (void)name; (void)notify; (void)ctx;
    if (out) *out = new_ref();
    return 0;
}
std::int32_t MIDIClientDispose_(std::uint32_t) DARWIN(MIDIClientDispose);
std::int32_t MIDIClientDispose_(std::uint32_t) { return 0; }

std::int32_t MIDIInputPortCreate_(std::uint32_t, void *, void *, void *, std::uint32_t *out) DARWIN(MIDIInputPortCreate);
std::int32_t MIDIInputPortCreate_(std::uint32_t, void *, void *, void *, std::uint32_t *out) {
    if (out) *out = new_ref();
    return 0;
}
std::int32_t MIDIOutputPortCreate_(std::uint32_t, void *, std::uint32_t *out) DARWIN(MIDIOutputPortCreate);
std::int32_t MIDIOutputPortCreate_(std::uint32_t, void *, std::uint32_t *out) {
    if (out) *out = new_ref();
    return 0;
}
std::int32_t MIDIPortDispose_(std::uint32_t) DARWIN(MIDIPortDispose);
std::int32_t MIDIPortDispose_(std::uint32_t) { return 0; }

/* No devices: every source/destination query comes back empty. */
std::uint64_t MIDIGetNumberOfSources_() DARWIN(MIDIGetNumberOfSources);
std::uint64_t MIDIGetNumberOfSources_() { return 0; }
std::uint64_t MIDIGetNumberOfDestinations_() DARWIN(MIDIGetNumberOfDestinations);
std::uint64_t MIDIGetNumberOfDestinations_() { return 0; }
std::uint64_t MIDIGetNumberOfDevices_() DARWIN(MIDIGetNumberOfDevices);
std::uint64_t MIDIGetNumberOfDevices_() { return 0; }
std::uint32_t MIDIGetSource_(std::uint64_t) DARWIN(MIDIGetSource);
std::uint32_t MIDIGetSource_(std::uint64_t) { return 0; }
std::uint32_t MIDIGetDestination_(std::uint64_t) DARWIN(MIDIGetDestination);
std::uint32_t MIDIGetDestination_(std::uint64_t) { return 0; }
std::uint32_t MIDIGetDevice_(std::uint64_t) DARWIN(MIDIGetDevice);
std::uint32_t MIDIGetDevice_(std::uint64_t) { return 0; }

/* kMIDIObjectNotFound (-10842) for properties of an object that does not exist. */
std::int32_t MIDIObjectGetStringProperty_(std::uint32_t obj, void *, void **out) DARWIN(MIDIObjectGetStringProperty);
std::int32_t MIDIObjectGetStringProperty_(std::uint32_t obj, void *, void **out) {
    (void)obj;
    if (out) *out = nullptr;
    return -10842;
}
std::int32_t MIDIObjectGetIntegerProperty_(std::uint32_t obj, void *, std::int32_t *out) DARWIN(MIDIObjectGetIntegerProperty);
std::int32_t MIDIObjectGetIntegerProperty_(std::uint32_t obj, void *, std::int32_t *out) {
    (void)obj;
    if (out) *out = 0;
    return -10842;
}

std::int32_t MIDIPortConnectSource_(std::uint32_t, std::uint32_t, void *) DARWIN(MIDIPortConnectSource);
std::int32_t MIDIPortConnectSource_(std::uint32_t, std::uint32_t, void *) { return 0; }
std::int32_t MIDIPortDisconnectSource_(std::uint32_t, std::uint32_t) DARWIN(MIDIPortDisconnectSource);
std::int32_t MIDIPortDisconnectSource_(std::uint32_t, std::uint32_t) { return 0; }

/* Sending goes nowhere, but the packet list has to stay well formed. */
MIDIPacket *MIDIPacketListInit_(MIDIPacketList *list) DARWIN(MIDIPacketListInit);
MIDIPacket *MIDIPacketListInit_(MIDIPacketList *list) {
    if (!list) return nullptr;
    list->numPackets = 0;
    return &list->packet[0];
}
MIDIPacket *MIDIPacketListAdd_(MIDIPacketList *list, std::uint64_t size, MIDIPacket *cur, std::uint64_t timestamp,
                               std::uint64_t length, const std::uint8_t *data) DARWIN(MIDIPacketListAdd);
MIDIPacket *MIDIPacketListAdd_(MIDIPacketList *list, std::uint64_t size, MIDIPacket *cur, std::uint64_t timestamp,
                               std::uint64_t length, const std::uint8_t *data) {
    if (!list || !cur || length > sizeof(MIDIPacket::data)) return nullptr;
    const auto *end = reinterpret_cast<const std::uint8_t *>(list) + size;
    auto *next = reinterpret_cast<std::uint8_t *>(cur) + 10 + length;
    if (next > end) return nullptr;
    cur->timestamp = timestamp;
    cur->length = static_cast<std::uint16_t>(length);
    if (data) std::memcpy(cur->data, data, length);
    list->numPackets++;
    return reinterpret_cast<MIDIPacket *>(next);
}
std::int32_t MIDISend_(std::uint32_t, std::uint32_t, const MIDIPacketList *) DARWIN(MIDISend);
std::int32_t MIDISend_(std::uint32_t, std::uint32_t, const MIDIPacketList *) { return 0; }
std::int32_t MIDISendSysex_(void *) DARWIN(MIDISendSysex);
std::int32_t MIDISendSysex_(void *) { return 0; }
std::int32_t MIDIRestart_() DARWIN(MIDIRestart);
std::int32_t MIDIRestart_() { return 0; }

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

struct AudioObjectPropertyAddress { std::uint32_t selector, scope, element; };

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

__attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    if (trace()) std::fprintf(stderr, "audio: %s has no implementation\n", name);
    return mm_synthesise_symbol(name, flags);
}

} // extern "C"
