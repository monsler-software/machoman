/* Helpers shared by the host frameworks (Foundation, AppKit, ...) that
 * define Objective-C classes on machoman's libobjc.
 *
 * A host class keeps its state in a C++ struct whose first member is the
 * objc header; `+alloc` placement-news it and `-dealloc` destroys it. Host
 * method implementations receive the guest's registers verbatim: integer
 * arguments in order, doubles in order, structs flattened (a method taking
 * an NSRect takes four doubles). See objc_host.h for the return rules. */
#ifndef MACHOMAN_OBJC_FRAMEWORK_H
#define MACHOMAN_OBJC_FRAMEWORK_H

#include "machoman/objc_host.h"

#include <cstddef>
#include <cstdint>
#include <new>

#define MM_FW_EXPORT __attribute__((visibility("default")))
/* An IMP from a plain function or a captureless lambda. */
#define MM_IMP(fn) reinterpret_cast<void *>(+(fn))
/* Class storage the guest binds to. Must be at namespace scope: inside an
 * anonymous namespace it would get internal linkage and the bind would
 * silently trap. */
#define MM_FW_CLASS(name) MM_OBJC_CLASS_STORAGE(name)
#define MM_FW_CLASS_REF(name) MM_OBJC_CLASS_REF(name)
#define MM_FW_META_REF(name) MM_OBJC_METACLASS_REF(name)

namespace mmfw {

using id = mm_id;
using Class = mm_objc_class *;
using SEL = mm_SEL;

/* ---- objects ------------------------------------------------------------ */

/* Constructs a payload in place and restores the isa the allocation set.
 * The objc header is the first thing in the payload, directly or through
 * a base payload (a control is a view is a responder), so it is reached
 * through the object pointer rather than by naming a member. */
template <typename T> T *construct(id o) {
    Class isa = o->isa;
    T *t = new (static_cast<void *>(o)) T;
    reinterpret_cast<mm_objc_object *>(t)->isa = isa;
    return t;
}
template <typename T> T *make(Class cls) { return construct<T>(mm_objc_alloc(cls, 0)); }
template <typename T> T *as(id o) { return reinterpret_cast<T *>(o); }

inline std::uint64_t ret(const void *p) { return reinterpret_cast<std::uintptr_t>(p); }
inline id obj(std::uint64_t v) { return reinterpret_cast<id>(v); }

template <typename T> std::uint64_t alloc_imp(id cls, SEL) { return ret(make<T>(reinterpret_cast<Class>(cls))); }
template <typename T> std::uint64_t dealloc_imp(id self, SEL) {
    reinterpret_cast<T *>(self)->~T();
    mm_objc_free_instance(self);
    return 0;
}

/* +alloc/-dealloc that construct and destroy the C++ payload. */
template <typename T> void register_lifecycle(Class cls) {
    mm_objc_add_class_method(cls, "alloc", MM_IMP(alloc_imp<T>), "@16@0:8");
    mm_objc_add_class_method(cls, "allocWithZone:", MM_IMP(alloc_imp<T>), "@24@0:8^v16");
    mm_objc_add_method(cls, "dealloc", MM_IMP(dealloc_imp<T>), "v16@0:8");
}

/* A class cluster: an abstract class (NSString, NSMutableArray) allocates
 * an instance of the concrete class that holds the payload. */
template <typename T> struct cluster {
    static inline Class concrete = nullptr;
    static std::uint64_t alloc(id, SEL) { return ret(make<T>(concrete)); }
    static std::uint64_t alloc_mutable(id, SEL) {
        T *o = make<T>(concrete);
        if constexpr (requires(T *t) { t->mutable_ = true; }) o->mutable_ = true;
        return ret(o);
    }
};
template <typename T> void register_cluster(Class abstract, Class concrete, bool mutable_) {
    cluster<T>::concrete = concrete;
    void *imp = mutable_ ? MM_IMP(cluster<T>::alloc_mutable) : MM_IMP(cluster<T>::alloc);
    mm_objc_add_class_method(abstract, "alloc", imp, "@16@0:8");
    mm_objc_add_class_method(abstract, "allocWithZone:", imp, "@24@0:8^v16");
}

/* ---- method tables ------------------------------------------------------- */

struct method { const char *sel; void *imp; const char *types; };
inline void add_methods(Class cls, const method *m, std::size_t n) {
    for (std::size_t i = 0; i < n; i++) mm_objc_add_method(cls, m[i].sel, m[i].imp, m[i].types);
}
inline void add_class_methods(Class cls, const method *m, std::size_t n) {
    for (std::size_t i = 0; i < n; i++) mm_objc_add_class_method(cls, m[i].sel, m[i].imp, m[i].types);
}
#define MM_METHODS(cls, ...)                                                                                           \
    do {                                                                                                               \
        static const mmfw::method mm_tbl[] = {__VA_ARGS__};                                                            \
        mmfw::add_methods(cls, mm_tbl, sizeof mm_tbl / sizeof mm_tbl[0]);                                              \
    } while (0)
#define MM_CLASS_METHODS(cls, ...)                                                                                     \
    do {                                                                                                               \
        static const mmfw::method mm_tbl[] = {__VA_ARGS__};                                                            \
        mmfw::add_class_methods(cls, mm_tbl, sizeof mm_tbl / sizeof mm_tbl[0]);                                        \
    } while (0)

/* ---- messaging ----------------------------------------------------------- */

inline std::uint64_t send(id r, const char *sel) { return mm_objc_send(r, mm_objc_sel(sel), 0, nullptr); }
inline std::uint64_t send(id r, const char *sel, std::uint64_t a) { return mm_objc_send(r, mm_objc_sel(sel), 1, &a); }
inline std::uint64_t send(id r, const char *sel, std::uint64_t a, std::uint64_t b) {
    const std::uint64_t v[2] = {a, b};
    return mm_objc_send(r, mm_objc_sel(sel), 2, v);
}
inline std::uint64_t send(id r, const char *sel, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    const std::uint64_t v[3] = {a, b, c};
    return mm_objc_send(r, mm_objc_sel(sel), 3, v);
}
inline bool responds(id r, const char *sel) { return r && send(r, "respondsToSelector:", ret(mm_objc_sel(sel))) != 0; }
inline id retain(id o) { return mm_objc_retain(o); }
inline void release(id o) { mm_objc_release(o); }
inline id autorelease(id o) { return mm_objc_autorelease(o); }
inline bool is_kind(id o, Class c) { return mm_objc_is_kind_of(o, c) != 0; }
inline Class cls(const char *name) { return mm_objc_class_named(name); }

} // namespace mmfw

#endif
