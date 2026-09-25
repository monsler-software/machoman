/* The Carbon leftovers a modern Cocoa program still calls: keyboard state,
 * the text input source that describes the keyboard layout, the Carbon
 * event-loop timers and a few Low Memory globals.
 *
 * There is no keyboard layout to publish — the compositor translates
 * keycodes for us through xkbcommon — so the input source is a real object
 * with an empty layout, and UCKeyTranslate produces no characters. A
 * program then falls back to the characters NSEvent already carries, which
 * is what it does on macOS when a layout has no mapping. */
#include "machoman/objc_framework.h"
#include "machoman/runtime.h"
#include "synthesise.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

using namespace mmfw;

namespace {

/* The input source object: an NSObject the program only ever passes back. */
id input_source() {
    static id source = nullptr;
    if (!source) {
        Class c = cls("NSObject");
        if (c) source = retain(obj(send(reinterpret_cast<id>(c), "new")));
    }
    return source;
}

/* An empty 'uchr' layout as NSData, so a program that asks for the layout
 * data gets a well-formed object rather than null. */
id empty_layout() {
    static id data = nullptr;
    if (!data) {
        Class c = cls("NSData");
        if (!c) return nullptr;
        static const std::uint8_t bytes[16] = {};
        const std::uint64_t args[2] = {reinterpret_cast<std::uintptr_t>(bytes), sizeof bytes};
        data = retain(obj(mm_objc_send(reinterpret_cast<id>(c), mm_objc_sel("dataWithBytes:length:"), 2, args)));
    }
    return data;
}

/* The property keys are opaque CFStrings; the only thing we can ask of one
 * is its text, and a caller may equally pass a class object or anything
 * else, so the receiver is checked before being messaged. */
bool is_layout_key(id key) {
    if (!key || !is_kind(key, cls("NSString"))) {
        if (key && std::getenv("MACHOMAN_STUB_TRACE"))
            std::fprintf(stderr, "carbon: TISGetInputSourceProperty key is a %s\n", mm_objc_class_name(mm_objc_object_class(key)));
        return false;
    }
    const char *name = reinterpret_cast<const char *>(send(key, "UTF8String"));
    return name && std::strstr(name, "UnicodeKeyLayoutData") != nullptr;
}

} // namespace

extern "C" {

/* ---- text input sources --------------------------------------------------- */

id TISCopyCurrentKeyboardLayoutInputSource_() DARWIN(TISCopyCurrentKeyboardLayoutInputSource);
id TISCopyCurrentKeyboardLayoutInputSource_() { return retain(input_source()); }
id TISCopyCurrentKeyboardInputSource_() DARWIN(TISCopyCurrentKeyboardInputSource);
id TISCopyCurrentKeyboardInputSource_() { return retain(input_source()); }
id TISGetInputSourceProperty_(id, id key) DARWIN(TISGetInputSourceProperty);
id TISGetInputSourceProperty_(id, id key) { return is_layout_key(key) ? empty_layout() : nullptr; }

/* No layout, so nothing is translated; the caller uses the characters the
 * event already carries. paramErr (-50) is what Carbon reports for a
 * layout it cannot use. */
std::int32_t UCKeyTranslate_(const void *layout, std::uint16_t, std::uint16_t, std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t *dead_state, std::uint64_t max_length,
                             std::uint64_t *actual_length, std::uint16_t *string) DARWIN(UCKeyTranslate);
std::int32_t UCKeyTranslate_(const void *layout, std::uint16_t, std::uint16_t, std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t *dead_state, std::uint64_t max_length,
                             std::uint64_t *actual_length, std::uint16_t *string) {
    (void)layout; (void)max_length; (void)string;
    if (dead_state) *dead_state = 0;
    if (actual_length) *actual_length = 0;
    return -50;
}

/* ---- keyboard and mouse state ---------------------------------------------- */

std::uint32_t GetCurrentKeyModifiers_() DARWIN(GetCurrentKeyModifiers);
std::uint32_t GetCurrentKeyModifiers_() {
    /* NSEvent tracks the modifiers; Carbon's bits are a different set and
     * nothing here reads them back, so report none held. */
    return 0;
}
void GetKeys_(std::uint32_t *keymap) DARWIN(GetKeys);
void GetKeys_(std::uint32_t *keymap) { if (keymap) std::memset(keymap, 0, 16); }
void HIGetMousePosition_(std::uint32_t, void *, double *point) DARWIN(HIGetMousePosition);
void HIGetMousePosition_(std::uint32_t, void *, double *point) {
    if (!point) return;
    /* NSEvent's class method already reports it in screen coordinates. */
    Class c = cls("NSEvent");
    if (!c) { point[0] = point[1] = 0; return; }
    send(reinterpret_cast<id>(c), "mouseLocation");
    const mm_host_api *api = mm_objc_host_api();
    mm_cpu_context *ctx = api ? api->current_context() : nullptr;
    if (ctx) { std::memcpy(&point[0], &ctx->v[0][0], 8); std::memcpy(&point[1], &ctx->v[1][0], 8); }
    else point[0] = point[1] = 0;
}

/* Low Memory globals: the keyboard type and the auto-repeat timings. */
std::uint8_t LMGetKbdType_() DARWIN(LMGetKbdType);
std::uint8_t LMGetKbdType_() { return 40; } /* kbdUS: an ANSI keyboard */
std::uint16_t LMGetKeyThresh_() DARWIN(LMGetKeyThresh);
std::uint16_t LMGetKeyThresh_() { return 30; }
std::uint16_t LMGetKeyRepThresh_() DARWIN(LMGetKeyRepThresh);
std::uint16_t LMGetKeyRepThresh_() { return 6; }

/* ---- Carbon event loop ------------------------------------------------------ */

/* One event loop per process, identified by a token; its timers are not
 * run, because a Carbon timer would have to drive the same run loop AppKit
 * already owns. A program that only installs an idle timer keeps working. */
void *GetCurrentEventLoop_() DARWIN(GetCurrentEventLoop);
void *GetCurrentEventLoop_() { static int loop = 0; return &loop; }
void *GetMainEventLoop_() DARWIN(GetMainEventLoop);
void *GetMainEventLoop_() { return GetCurrentEventLoop_(); }
std::int32_t InstallEventLoopTimer_(void *, double, double, void *, void *, void **out) DARWIN(InstallEventLoopTimer);
std::int32_t InstallEventLoopTimer_(void *, double, double, void *, void *, void **out) {
    static int timer = 0;
    if (out) *out = &timer;
    return 0;
}
std::int32_t RemoveEventLoopTimer_(void *) DARWIN(RemoveEventLoopTimer);
std::int32_t RemoveEventLoopTimer_(void *) { return 0; }
std::int32_t SetSystemUIMode_(std::uint32_t, std::uint32_t) DARWIN(SetSystemUIMode);
std::int32_t SetSystemUIMode_(std::uint32_t, std::uint32_t) { return 0; }

/* ---- process manager --------------------------------------------------------- */

/* The Process Manager enumerates the running applications. Under machoman
 * there is exactly one — this process — so the walk reports it once and
 * then answers procNotFound (-600). Reporting nothing at all is not the
 * same thing: a program that counts the copies of itself that are running
 * then finds zero and concludes it is the *second* instance, which is how
 * Ableton Live decided not to open its window.
 *
 * ProcessInfoRec is mac68k-aligned (Processes.h packs it to two bytes), so
 * the StringPtr really does sit at offset 4 and the whole record is 72
 * bytes; the offsets below are that layout. */
constexpr std::uint32_t our_psn_low = 2;   /* kCurrentProcess */

/* An OSType from the bundle's CFBundleSignature, which is what the record
 * has to carry for a program to recognise its own kind. */
std::uint32_t bundle_signature() {
    static std::uint32_t signature = [] {
        std::uint32_t value = 0x3f3f3f3f; /* '????', as a bundle without one */
        Class bundle = cls("NSBundle");
        if (!bundle) return value;
        id main = obj(send(reinterpret_cast<id>(bundle), "mainBundle"));
        if (!main) return value;
        Class string_class = cls("NSString");
        if (!string_class) return value;
        id key = obj(send(reinterpret_cast<id>(string_class), "stringWithUTF8String:", ret("CFBundleSignature")));
        id text = obj(send(main, "objectForInfoDictionaryKey:", ret(key)));
        if (!text) return value;
        const char *utf8 = reinterpret_cast<const char *>(send(text, "UTF8String"));
        if (!utf8 || std::strlen(utf8) != 4) return value;
        value = 0;
        for (int i = 0; i < 4; i++) value = (value << 8) | static_cast<unsigned char>(utf8[i]);
        return value;
    }();
    return signature;
}

void write_be32(void *record, std::size_t offset, std::uint32_t value) {
    /* The fields are native-endian UInt32s; only the OSTypes read like
     * four characters, and they are built big-endian above. */
    std::memcpy(static_cast<char *>(record) + offset, &value, 4);
}

std::int32_t GetNextProcess_(std::uint32_t *psn) DARWIN(GetNextProcess);
std::int32_t GetNextProcess_(std::uint32_t *psn) {
    if (!psn) return -50; /* paramErr */
    const bool from_start = psn[0] == 0 && psn[1] == 0; /* kNoProcess */
    if (!from_start) return -600;
    psn[0] = 0;
    psn[1] = our_psn_low;
    return 0;
}

std::int32_t GetProcessInformation_(const std::uint32_t *psn, void *info) DARWIN(GetProcessInformation);
std::int32_t GetProcessInformation_(const std::uint32_t *psn, void *info) {
    if (!psn || !info) return -50;
    if (psn[0] != 0 || psn[1] != our_psn_low) return -600;
    std::uint32_t length = 0;
    std::memcpy(&length, info, 4);
    if (length < 28) return -50;
    write_be32(info, 12, psn[0]);                 /* processNumber.highLongOfPSN */
    write_be32(info, 16, psn[1]);                 /* processNumber.lowLongOfPSN  */
    write_be32(info, 20, 0x4150504c);             /* processType = 'APPL'        */
    write_be32(info, 24, bundle_signature());     /* processSignature            */
    if (length >= 32) write_be32(info, 28, 0);    /* processMode                 */
    return 0;
}

__attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}

} // extern "C"
