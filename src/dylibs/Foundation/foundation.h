/* Internal header of the host CoreFoundation/Foundation implementation.
 *
 * Every CF type is an Objective-C object of a host class defined on
 * machoman's libobjc, exactly like Apple's toll-free bridging: CFStringRef
 * and NSString* are the same pointer, CFRetain is -retain, and a guest that
 * subclasses NSString gets a real class. Host classes keep their state in a
 * C++ struct whose first member is the objc header; +alloc placement-news
 * it and -dealloc destroys it (see register_lifecycle).
 *
 * Host method IMPs receive the guest's registers: integer arguments in
 * order, doubles in order, structs flattened (see objc_host.h). */
#ifndef MACHOMAN_FOUNDATION_H
#define MACHOMAN_FOUNDATION_H

#include "machoman/objc_host.h"
#include "machoman/runtime.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)
#define EXPORT __attribute__((visibility("default")))
#define IMP(fn) reinterpret_cast<void *>(+(fn))

namespace fnd {

using id = mm_id;
id date_from_epoch(double seconds);
using Class = mm_objc_class *;
using SEL = mm_SEL;
using CFIndex = std::int64_t;
using CFTypeID = std::uint64_t;
using UniChar = char16_t;

struct CFRange { CFIndex location, length; };
inline constexpr CFIndex kCFNotFound = -1;

/* ---- classes ------------------------------------------------------------ */

/* Storage for a host class: use FND_CLASS(Name) at namespace scope, then
 * define_class<Payload>("Name", super) in the registration function. */
#define FND_CLASS(name) MM_OBJC_CLASS_STORAGE(name)
#define FND_CLASS_REF(name) MM_OBJC_CLASS_REF(name)
#define FND_META_REF(name) MM_OBJC_METACLASS_REF(name)

template <typename T> T *construct(id o) {
    Class isa = o->isa;
    T *t = new (static_cast<void *>(o)) T;
    t->base.isa = isa;
    if (!mm_objc_construct_instance(o)) {
        t->~T();
        mm_objc_free_instance(o);
        return nullptr;
    }
    return t;
}

template <typename T> std::uint64_t alloc_imp(id cls, SEL) {
    return reinterpret_cast<std::uintptr_t>(construct<T>(mm_objc_alloc_raw(reinterpret_cast<Class>(cls), 0)));
}

template <typename T> std::uint64_t dealloc_imp(id self, SEL) {
    mm_objc_destruct_instance(self);
    reinterpret_cast<T *>(self)->~T();
    mm_objc_free_instance(self);
    return 0;
}

/* Registers +alloc/-dealloc that construct/destroy the C++ payload. */
template <typename T> void register_lifecycle(Class cls) {
    mm_objc_add_class_method(cls, "alloc", IMP(alloc_imp<T>), "@16@0:8");
    mm_objc_add_class_method(cls, "allocWithZone:", IMP(alloc_imp<T>), "@24@0:8^v16");
    mm_objc_add_method(cls, "dealloc", IMP(dealloc_imp<T>), "v16@0:8");
}

/* Allocates and constructs an instance of a host class from host code. */
template <typename T> T *make(Class cls) {
    id o = mm_objc_alloc_raw(cls, 0);
    return construct<T>(o);
}

/* A class cluster: the abstract classes (NSString, NSMutableArray, ...)
 * allocate an instance of the concrete class that holds the payload, and
 * the mutable ones mark it mutable. */
template <typename T> struct cluster {
    static inline Class concrete = nullptr;
    static std::uint64_t alloc(id, SEL) { return reinterpret_cast<std::uintptr_t>(make<T>(concrete)); }
    static std::uint64_t alloc_mutable(id, SEL) {
        T *o = make<T>(concrete);
        if constexpr (requires (T *t) { t->mutable_ = true; }) o->mutable_ = true;
        return reinterpret_cast<std::uintptr_t>(o);
    }
};

/* Points the abstract class `abstract` at the cluster's concrete class. */
template <typename T> void register_cluster(Class abstract, Class concrete, bool mutable_) {
    cluster<T>::concrete = concrete;
    void *imp = mutable_ ? IMP(cluster<T>::alloc_mutable) : IMP(cluster<T>::alloc);
    mm_objc_add_class_method(abstract, "alloc", imp, "@16@0:8");
    mm_objc_add_class_method(abstract, "allocWithZone:", imp, "@24@0:8^v16");
}

struct method { const char *sel; void *imp; const char *types; };
void add_methods(Class cls, const method *m, std::size_t n);
void add_class_methods(Class cls, const method *m, std::size_t n);
#define FND_METHODS(cls, ...) do { static const fnd::method tbl[] = {__VA_ARGS__}; fnd::add_methods(cls, tbl, sizeof tbl / sizeof tbl[0]); } while (0)
#define FND_CLASS_METHODS(cls, ...) do { static const fnd::method tbl[] = {__VA_ARGS__}; fnd::add_class_methods(cls, tbl, sizeof tbl / sizeof tbl[0]); } while (0)

template <typename T> T *as(id o) { return reinterpret_cast<T *>(o); }
inline std::uint64_t ret(const void *p) { return reinterpret_cast<std::uintptr_t>(p); }
inline id obj(std::uint64_t v) { return reinterpret_cast<id>(v); }

/* ---- messaging shortcuts ------------------------------------------------ */

inline std::uint64_t send(id r, const char *sel) { return mm_objc_send(r, mm_objc_sel(sel), 0, nullptr); }
inline std::uint64_t send(id r, const char *sel, std::uint64_t a) { return mm_objc_send(r, mm_objc_sel(sel), 1, &a); }
inline std::uint64_t send(id r, const char *sel, std::uint64_t a, std::uint64_t b) { const std::uint64_t v[2] = {a, b}; return mm_objc_send(r, mm_objc_sel(sel), 2, v); }
inline std::uint64_t send(id r, const char *sel, std::uint64_t a, std::uint64_t b, std::uint64_t c) { const std::uint64_t v[3] = {a, b, c}; return mm_objc_send(r, mm_objc_sel(sel), 3, v); }
inline id retain(id o) { return mm_objc_retain(o); }
inline void release(id o) { mm_objc_release(o); }
inline id autorelease(id o) { return mm_objc_autorelease(o); }
inline bool is_kind(id o, Class c) { return mm_objc_is_kind_of(o, c) != 0; }
Class cls(const char *name);

/* Equality/hash through the object protocol (works for guest objects). */
bool objects_equal(id a, id b);
std::uint64_t object_hash(id o);
/* -description as UTF-8. */
std::string describe(id o);

/* ---- strings ------------------------------------------------------------ */

/* Access to any NSString's characters (constant strings included). */
struct chars { const UniChar *data; CFIndex length; };
chars string_chars(id s);
std::string to_utf8(id s);
std::u16string to_utf16(std::string_view utf8);
std::string utf16_to_utf8(std::u16string_view s);
/* New immutable strings (retained, caller owns). */
id string_from_utf8(std::string_view s);
id string_from_utf16(std::u16string_view s);
id string_from_utf16(const UniChar *p, CFIndex n);
/* Shared constant strings for well-known keys. */
id constant_string(const char *utf8);
bool is_string(id o);
bool is_mutable_string(id o);

/* Encodings: kCFStringEncoding* (CF) and NSStringEncoding (NS) values. */
bool encode(std::u16string_view s, std::uint32_t cf_encoding, std::string &out, bool lossy);
bool decode(std::string_view bytes, std::uint32_t cf_encoding, std::u16string &out);
std::uint32_t ns_to_cf_encoding(std::uint64_t ns);

/* ---- collections -------------------------------------------------------- */

id array_create(const id *objects, CFIndex n, bool mutable_);
std::vector<id> &array_storage(id a); /* retained elements */
bool is_array(id o);
id dictionary_create(bool mutable_);
void dictionary_set(id d, id key, id value); /* key copied, value retained */
id dictionary_get(id d, id key);
void dictionary_remove(id d, id key);
CFIndex dictionary_count(id d);
void dictionary_keys(id d, std::vector<id> &keys);
bool is_dictionary(id o);
id data_create(const void *bytes, CFIndex n, bool mutable_);
std::string &data_storage(id d);
bool is_data(id o);
id number_from_int64(std::int64_t v);
id number_from_double(double v);
id number_bool(bool v);
bool is_number(id o);
double number_double(id n);
std::int64_t number_int64(id n);
bool number_is_float(id n);
id null_object();

/* A keyed archive's UID: an index into its $objects, and a type of its own
 * so it cannot be mistaken for a number. */
id uid_create(std::uint64_t value); /* retained */
bool is_uid(id o);
std::uint64_t uid_value(id o);

/* ---- property lists ----------------------------------------------------- */

id plist_parse(std::string_view bytes, std::string *error); /* XML or binary; retained result or null */
bool plist_write_xml(id plist, std::string &out);
bool plist_write_binary(id plist, std::string &out);

/* ---- misc --------------------------------------------------------------- */

const mm_host_api *api();
bool trace();
id error_create(id domain, std::int64_t code, id user_info);
std::string home_directory();
std::string main_bundle_path(); /* "" if the executable is not inside a bundle */
id url_from_path(std::string_view path, bool is_dir);
std::string url_path(id url);
bool is_url(id o);

/* Runs whatever is waiting on the main dispatch queue (dispatch.cpp).
 * The run loop calls this once per turn: the main queue is the run loop. */
void dispatch_drain_main_queue();

/* Registration entry points (one per translation unit). */
void register_strings();
void register_collections();
void register_plists();
void register_bundle();
void register_runloop();
void register_misc();
void register_threading();
void enqueue_selector(id loop, id target, SEL selector, id argument, double delay, bool wait);
void cancel_selectors(id target, SEL selector, id argument, bool match_argument);
void register_task();      /* NSTask, NSPipe, NSFileHandle */
void register_key_value(); /* Manual KVO and object-valued key access. */

} // namespace fnd

#endif
