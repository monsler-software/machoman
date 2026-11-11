#include "machoman/objc_host.h"
#include "machoman/runtime.h"
#include "../src/dylibs/symbol-synthesis/synthesise.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" bool add_method(mm_objc_class *, mm_SEL, void *, const char *) __asm__("_class_addMethod");
extern "C" mm_id store_weak(mm_id *, mm_id) __asm__("_objc_storeWeak");
static void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static std::uint64_t real_value(mm_id, mm_SEL) { return 999; }
int main() {
    mm_objc_class storage{}, meta{};
    auto *cls = mm_objc_define_class("DynamicHostProbe", mm_objc_class_named("NSObject"), sizeof(mm_objc_object), &storage, &meta);
    mm_objc_enable_dynamic_stubs(cls);
    require(mm_objc_add_stub_method(cls, "counter", "Q16@0:8"), "typed getter registered");
    require(mm_objc_add_stub_method(cls, "setCounter:", "v24@0:8Q16"), "typed setter registered");
    require(mm_objc_add_stub_method(cls, "child", "@16@0:8"), "object getter registered");
    require(mm_objc_add_stub_method(cls, "setChild:", "v24@0:8@16"), "object setter registered");
    require(!mm_objc_add_stub_method(cls, "malformed", "{Broken=Q"), "malformed ABI rejected");
    require(!mm_objc_add_stub_method(cls, "huge", "[999999999Q]@:"), "oversized layout rejected");
    require(!mm_objc_add_stub_method(cls, "union", "(Union=Qd)@:"), "unsupported union rejected");
    auto object = mm_objc_alloc(cls, 0);
    require(mm_objc_send0(object, "counter") == 0, "zero scalar default");
    mm_objc_send1(object, "setCounter:", 123);
    require(mm_objc_send0(object, "counter") == 123, "scalar state preserved");
    auto child = reinterpret_cast<mm_id>(mm_objc_send0(object, "child"));
    require(child && child != object && mm_objc_send0(object, "child") == reinterpret_cast<std::uintptr_t>(child), "stable non-null child");
    mm_id weak = nullptr; store_weak(&weak, child);
    mm_objc_send1(object, "setChild:", 0);
    require(!weak && !mm_objc_send0(object, "child"), "replacement releases old child and preserves explicit nil");
    mm_id replacement = mm_objc_alloc(mm_objc_class_named("NSObject"), 0);
    store_weak(&weak, replacement);
    mm_objc_send1(object, "setChild:", reinterpret_cast<std::uintptr_t>(replacement));
    mm_objc_release(replacement);
    require(weak != nullptr, "property retains child");
    auto getter = mm_objc_sel("counter");
    auto start = std::chrono::steady_clock::now();
    std::uint64_t sum = 0;
    for (int i = 0; i < 100000; ++i) sum += mm_objc_send(object, getter, 0, nullptr);
    auto elapsed = std::chrono::steady_clock::now() - start;
    require(sum == 12300000, "repeated dispatch is stable");
    std::printf("cached dynamic getter: %.0f ns/call (100000 calls)\n", std::chrono::duration<double, std::nano>(elapsed).count() / 100000.);
    require(add_method(cls, getter, reinterpret_cast<void *>(&real_value), "Q@:"), "real implementation replaces cached stub");
    require(mm_objc_send0(object, "counter") == 999, "replacement wins");
    start = std::chrono::steady_clock::now(); sum = 0;
    for (int i = 0; i < 100000; ++i) sum += mm_objc_send(object, getter, 0, nullptr);
    elapsed = std::chrono::steady_clock::now() - start;
    require(sum == 99900000, "real dispatch baseline");
    std::printf("real constant getter: %.0f ns/call (100000 calls)\n", std::chrono::duration<double, std::nano>(elapsed).count() / 100000.);
    mm_objc_release(object); require(!weak, "owner destruction releases properties");
    void *stub = mm_synthesise_symbol("_MachomanMissingFunction", MM_SYMBOL_LAZY);
    require(stub && stub == mm_synthesise_symbol("_MachomanMissingFunction", MM_SYMBOL_LAZY), "function trampoline cached");
    for (int i = 0; i < 10000; ++i) require(reinterpret_cast<std::uint64_t (*)()>(stub)() == 0, "function stub returns zero");
    require(reinterpret_cast<double (*)()>(stub)() == 0., "function stub clears FP result");
    std::puts("dynamic stubs ownership, cache, ABI validation and replacement passed");
}
