/* CoreMIDI clients and ports; native MIDI transport is still pending.
 * CoreAudio HAL and AudioComponent APIs live in separate source files. */
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

__attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    if (trace()) std::fprintf(stderr, "audio: %s has no implementation\n", name);
    return mm_synthesise_symbol(name, flags);
}

} // extern "C"
