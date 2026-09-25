/* Synthesising a framework symbol from its name.
 *
 * Shared by the stub framework (which stands in for every macOS framework
 * machoman does not implement, linked under each of their paths) and by the
 * real frameworks, for the constants they have not defined yet: the runtime
 * asks a host library for each bind it cannot resolve statically through
 * `machoman_resolve_symbol`, and this answers:
 *
 *   _OBJC_CLASS_$_X / _OBJC_METACLASS_$_X  a real objc class named X,
 *                                          subclass of NSObject, so the
 *                                          guest can reference and
 *                                          subclass it; a message it does
 *                                          not inherit aborts naming the
 *                                          selector (libobjc does that)
 *   k…, …Key, …Name, …Domain, …Notification, …                  an NSString
 *                                          whose value is the symbol name
 *                                          (Apple's constants are opaque
 *                                          tokens; the name is a unique,
 *                                          printable, comparable one)
 *   everything else                        a function that reports the
 *                                          call once and returns 0
 *
 * This keeps unimplemented frameworks honest: a program only fails when it
 * actually depends on behaviour, and the message says which symbol it was.
 * $MACHOMAN_STUB_TRACE=1 logs every synthesised symbol. */
#include "machoman/objc_host.h"
#include "machoman/runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

#define EXPORT __attribute__((visibility("default")))

#include "synthesise.h"

namespace {

std::mutex g_mu;
std::unordered_map<std::string, void *> g_cache;

bool trace() { static const bool on = std::getenv("MACHOMAN_STUB_TRACE") != nullptr; return on; }

/* ---- function stubs ------------------------------------------------------ */

struct stub_entry { std::string symbol; };

extern "C" std::uint64_t mm_stub_report(stub_entry *e) {
    static std::mutex mu;
    static std::unordered_map<std::string, int> seen;
    std::lock_guard lock(mu);
    if (seen[e->symbol]++ == 0)
        std::fprintf(stderr, "stub: %s is not implemented (call returns 0)\n", e->symbol.c_str());
    return 0;
}

/* One trampoline per symbol so the report can name it:
 *   mov rdi, <entry>; mov rax, mm_stub_report; jmp rax
 * They are cut from a shared executable page. */
void *make_function_stub(const std::string &symbol) {
    static std::uint8_t *page = nullptr;
    static std::size_t used = 4096;
    constexpr std::size_t slot = 32;
    if (used + slot > 4096) {
        void *p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        page = static_cast<std::uint8_t *>(p);
        used = 0;
    }
    std::uint8_t *code = page + used;
    used += slot;

    auto *e = new stub_entry{symbol};
    std::size_t i = 0;
    code[i++] = 0x48; code[i++] = 0xBF;                       /* movabs rdi, imm64 */
    const auto arg = reinterpret_cast<std::uint64_t>(e);
    std::memcpy(code + i, &arg, 8); i += 8;
    code[i++] = 0x48; code[i++] = 0xB8;                       /* movabs rax, imm64 */
    const auto fn = reinterpret_cast<std::uint64_t>(&mm_stub_report);
    std::memcpy(code + i, &fn, 8); i += 8;
    code[i++] = 0xFF; code[i++] = 0xE0;                       /* jmp rax */
    return code;
}

/* ---- classes and constants ------------------------------------------------ */

/* Class pairs are allocated here because the guest binds to their address. */
struct class_pair { mm_objc_class cls, meta; };

mm_objc_class *stub_class(const std::string &name, bool want_meta) {
    /* An umbrella framework re-exports the classes its members define, so
     * a class that already exists is that class, not a new stub. */
    if (mm_objc_class *existing = mm_objc_class_named(name.c_str()))
        return want_meta ? mm_objc_object_class(reinterpret_cast<mm_id>(existing)) : existing;
    static std::unordered_map<std::string, class_pair *> classes;
    auto it = classes.find(name);
    if (it == classes.end()) {
        auto *p = new class_pair{};
        mm_objc_class *nsobject = mm_objc_class_named("NSObject");
        mm_objc_define_class(name.c_str(), nsobject, sizeof(void *), &p->cls, &p->meta);
        it = classes.emplace(name, p).first;
    }
    return want_meta ? &it->second->meta : &it->second->cls;
}

/* A constant like `extern NSString *const kFoo` is a *variable*: the guest
 * loads the pointer out of it, so what a bind needs is the address of a
 * cell holding the object, not the object itself. The value is an NSString
 * of the symbol's own name — Apple's framework constants are opaque tokens
 * compared by pointer or by text, and the name is unique and printable. */
void *constant_string(const std::string &name) {
    mm_objc_class *nsstring = mm_objc_class_named("NSString");
    if (!nsstring) return nullptr;
    const std::uint64_t arg = reinterpret_cast<std::uintptr_t>(name.c_str());
    mm_id s = reinterpret_cast<mm_id>(mm_objc_send(reinterpret_cast<mm_id>(nsstring), mm_objc_sel("stringWithUTF8String:"), 1, &arg));
    if (!s) return nullptr;
    auto *cell = new mm_id(mm_objc_retain(s));
    return cell;
}

/* Function or constant? A lazy bind is a call site, so the symbol is a
 * function; any other bind reaches the guest through the GOT, which is how
 * data constants are read. Guessing from the name instead is unreliable —
 * constants are full of verbs ("NSApplicationDidBecomeActiveNotification",
 * "AVLinearPCMIsFloatKey") — and getting it wrong turns a string into code
 * or code into a string, so the bind record decides. A function imported by
 * address rather than called is the one case this misreads; it then behaves
 * as a constant and the guest fails where it calls it. */
bool looks_like_function(unsigned flags) { return (flags & MM_SYMBOL_LAZY) != 0; }

} // namespace

void *mm_synthesise_symbol(const char *raw, unsigned flags) {
    if (!raw || !*raw) return nullptr;
    std::lock_guard lock(g_mu);
    const std::string symbol(raw);
    if (auto it = g_cache.find(symbol); it != g_cache.end()) return it->second;

    std::string_view name(symbol);
    if (name.starts_with('_')) name.remove_prefix(1);

    void *value = nullptr;
    const char *kind = "function";
    if (name.starts_with("OBJC_CLASS_$_")) {
        value = stub_class(std::string(name.substr(13)), false);
        kind = "class";
    } else if (name.starts_with("OBJC_METACLASS_$_")) {
        value = stub_class(std::string(name.substr(17)), true);
        kind = "metaclass";
    } else if (name.starts_with("OBJC_EHTYPE_$_")) {
        /* An exception type object: a class works, since it is only ever
         * compared for identity by the personality routine. */
        value = stub_class(std::string(name.substr(14)), false);
        kind = "ehtype";
    } else if (looks_like_function(flags)) {
        value = make_function_stub(symbol);
    } else {
        value = constant_string(std::string(name));
        kind = "string constant";
        if (!value) return nullptr; /* Foundation not loaded yet: let the runtime trap it */
    }

    if (trace())
        std::fprintf(stderr, "stub: %s -> %s %p\n", symbol.c_str(), kind, value);
    g_cache.emplace(symbol, value);
    return value;
}
