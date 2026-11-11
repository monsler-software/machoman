/* Host implementation of /usr/lib/libobjc.A.dylib: the Objective-C
 * runtime for guest images compiled against Apple's arm64 ABI.
 *
 * Guest metadata (__objc_classlist, method lists, categories, selector
 * references) is read in Apple's layouts and registered into this
 * runtime's own tables; host frameworks register their classes through
 * machoman/objc_host.h. objc_msgSend does not call implementations: it
 * resolves the IMP and tells the runtime to continue there (guest code via
 * resume_va, host code via host_tail_fn) with the argument registers
 * untouched, which keeps every calling convention detail out of here. */
#include "machoman/objc_host.h"
#include "machoman/macho.h"
#include "machoman/runtime.h"
#include "stub_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <array>

extern "C" std::uint64_t mm_hostcall(mm_cpu_context *ctx, const void *fn);

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)
#define EXPORT __attribute__((visibility("default")))

namespace {

using id = mm_id;
using SEL = mm_SEL;
using Class = mm_objc_class *;

const mm_host_api *g_api = nullptr;
std::recursive_mutex &lock_() { static std::recursive_mutex *m = new std::recursive_mutex; return *m; }
#define g_lock lock_()

bool trace() { static const bool on = std::getenv("MACHOMAN_OBJC_TRACE") != nullptr; return on; }

/* MACHOMAN_OBJC_BACKTRACE=setFrame:,frame prints the calling guest's stack
 * every time one of those selectors is sent. The trace says which message
 * was sent; only the backtrace says which of the guest's own functions
 * sent it, which is the question whenever a program stops doing anything. */
bool backtrace_selector(const char *sel) {
    static const char *const list = std::getenv("MACHOMAN_OBJC_BACKTRACE");
    if (!list || !sel) return false;
    const std::size_t n = std::strlen(sel);
    for (const char *p = list; *p;) {
        const char *comma = std::strchr(p, ',');
        const std::size_t len = comma ? static_cast<std::size_t>(comma - p) : std::strlen(p);
        if (len == n && std::strncmp(p, sel, n) == 0) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

/* ---- Apple metadata layouts -------------------------------------------- */

struct class_ro_t {
    std::uint32_t flags, instanceStart, instanceSize, reserved;
    const std::uint8_t *ivarLayout;
    const char *name;
    const void *baseMethods;
    const void *baseProtocols;
    const void *ivars;
    const std::uint8_t *weakIvarLayout;
    const void *baseProperties;
};

struct method_list_t { std::uint32_t entsize_flags, count; };
struct method_big { SEL name; const char *types; void *imp; };
struct method_rel { std::int32_t name, types, imp; };

struct ivar_t { std::int32_t *offset; const char *name; const char *type; std::uint32_t alignment_raw, size; };
struct ivar_list_t { std::uint32_t entsize, count; };

struct protocol_list_t { std::uintptr_t count; };
struct protocol_t {
    mm_objc_object base;
    const char *name;
    protocol_list_t *protocols;
    method_list_t *instanceMethods, *classMethods, *optionalInstanceMethods, *optionalClassMethods;
    void *instanceProperties;
    std::uint32_t size, flags;
};

struct category_t {
    const char *name;
    Class cls;
    method_list_t *instanceMethods, *classMethods;
    protocol_list_t *protocols;
    void *instanceProperties, *classProperties;
};

/* ---- runtime-side class data ------------------------------------------- */

struct dynamic_method;
struct method_entry { SEL sel; void *imp; const char *types; bool guest; dynamic_method *stub = nullptr; };
struct dynamic_method {
    method_entry entry{};
    std::string encoding;
    std::vector<stub_abi::type> signature;
    SEL property = nullptr;
    bool setter = false, initializer = false, inferred = false, placeholder = false;
};

struct class_rw {
    const class_ro_t *ro = nullptr;
    std::string name;
    std::vector<method_entry> methods;            /* later additions first */
    std::unordered_map<SEL, method_entry> cache;
    std::vector<protocol_t *> protocols;
    std::uint32_t instance_size = 0;
    bool realized = false, initializing = false, initialized = false, is_meta = false, host = false, guest_image = false;
    const char *missing_super = nullptr; /* unresolved superclass import: the class cannot be used */
    bool dynamic_stubs = false;
    std::unordered_map<SEL, std::unique_ptr<dynamic_method>> stubs;
};

class_rw *rw(Class c) { return reinterpret_cast<class_rw *>(c->bits & ~std::uintptr_t{7}); }
void set_rw(Class c, class_rw *r) { c->bits = reinterpret_cast<std::uintptr_t>(r); }

/* Function-local statics: the runtime's constructor runs before this
 * translation unit's dynamic initialisers would have run. */
struct refcount_entry { std::int64_t count; bool owned; };
struct assoc { id value; std::uintptr_t policy; };
struct dynamic_value { std::vector<std::uint8_t> bytes; id object = nullptr; };
struct tables {
    std::unordered_map<std::string_view, Class> classes;
    std::unordered_set<Class> registered_classes; /* pointers handed out as isa: guards against retaining garbage */
    std::unordered_map<std::string_view, SEL> selectors;
    std::unordered_map<std::string_view, protocol_t *> protocols;
    std::vector<std::string *> selector_storage; /* host-registered selector strings */
    std::unordered_map<id, refcount_entry> refcounts;
    std::unordered_set<id> deallocating;
    std::unordered_map<id, std::vector<method_entry>> cxx_destructors;
    std::unordered_map<id, std::vector<id *>> weak;
    std::unordered_map<id, std::unordered_map<const void *, assoc>> assoc_;
    std::unordered_map<id, std::recursive_mutex *> sync;
    std::unordered_map<id, std::unordered_map<SEL, dynamic_value>> stub_values;
};
tables &T() { static tables *t = new tables; return *t; }
#define g_classes (T().classes)
#define g_registered_classes (T().registered_classes)
#define g_selectors (T().selectors)
#define g_protocols (T().protocols)
#define g_selector_storage (T().selector_storage)
#define g_refcounts (T().refcounts)
#define g_deallocating (T().deallocating)
#define g_cxx_destructors (T().cxx_destructors)
#define g_weak (T().weak)
#define g_assoc (T().assoc_)
#define g_sync (T().sync)

bool have_dynamic_methods = false;
void invalidate_method_caches(class_rw *changed) {
    changed->cache.clear();
    if (!have_dynamic_methods) return;
    /* A subclass's cached gap must not hide a new superclass implementation.
     * Mutation is cold; existing descriptors remain alive for active calls. */
    for (auto &[name, c] : g_classes) { rw(c)->cache.clear(); rw(c->isa)->cache.clear(); }
}

SEL sel_intern(const char *name) {
    std::lock_guard lock(g_lock);
    auto it = g_selectors.find(name);
    if (it != g_selectors.end()) return it->second;
    /* First sight of this name: the given string becomes canonical (guest
     * __objc_methname strings and host literals live forever). */
    g_selectors.emplace(std::string_view(name), name);
    return name;
}

SEL sel_intern_copy(const char *name) {
    std::lock_guard lock(g_lock);
    auto it = g_selectors.find(name);
    if (it != g_selectors.end()) return it->second;
    auto *s = new std::string(name);
    g_selector_storage.push_back(s);
    g_selectors.emplace(std::string_view(*s), s->c_str());
    return s->c_str();
}

bool is_guest(const void *p) {
    return g_api && g_api->is_guest_code && g_api->is_guest_code(reinterpret_cast<std::uintptr_t>(p));
}

/* Appends the methods of an Apple method list (big or relative). */
void add_method_list(class_rw *r, const void *listp, bool guest_image) {
    if (!listp) return;
    const auto *list = static_cast<const method_list_t *>(listp);
    const bool relative = list->entsize_flags & 0x80000000u;
    const std::uint32_t entsize = list->entsize_flags & 0xFFFCu;
    const auto *base = reinterpret_cast<const std::uint8_t *>(list + 1);
    std::vector<method_entry> added;
    for (std::uint32_t i = 0; i < list->count; i++) {
        const std::uint8_t *e = base + i * entsize;
        method_entry m{};
        if (relative) {
            const auto *rm = reinterpret_cast<const method_rel *>(e);
            /* name field points at a selector reference slot (already uniqued) */
            const auto *selref = reinterpret_cast<const SEL *>(e + rm->name);
            m.sel = sel_intern(*selref);
            m.types = reinterpret_cast<const char *>(e + 4 + rm->types);
            m.imp = reinterpret_cast<void *>(const_cast<std::uint8_t *>(e) + 8 + rm->imp);
        } else {
            const auto *bm = reinterpret_cast<const method_big *>(e);
            m.sel = sel_intern(bm->name);
            m.types = bm->types;
            m.imp = bm->imp;
        }
        m.guest = guest_image || is_guest(m.imp);
        added.push_back(m);
    }
    /* Later lists take precedence: insert at the front, preserving list order within. */
    r->methods.insert(r->methods.begin(), added.begin(), added.end());
    invalidate_method_caches(r);
}

void add_protocol_list(class_rw *r, const void *listp) {
    if (!listp) return;
    const auto *list = static_cast<const protocol_list_t *>(listp);
    const auto *protos = reinterpret_cast<protocol_t *const *>(list + 1);
    for (std::uintptr_t i = 0; i < list->count; i++)
        r->protocols.push_back(protos[i]);
}

Class realize(Class c);

/* Guest classes: metadata from the image. Ivars are slid when the
 * superclass turned out bigger than the class was compiled against. */
const char *unresolved_name(const void *p) {
    return g_api && g_api->unresolved_import_name ? g_api->unresolved_import_name(reinterpret_cast<std::uintptr_t>(p)) : nullptr;
}

void realize_guest(Class c) {
    class_rw *r = rw(c);
    const class_ro_t *ro = r->ro;
    Class super = c->superclass;
    if (const char *missing = super ? unresolved_name(super) : nullptr) {
        r->missing_super = missing;
        r->realized = true;
        r->instance_size = ro->instanceSize;
        if (trace()) std::fprintf(stderr, "objc: class %s: superclass %s is not provided by any host framework\n", r->name.c_str(), missing);
        return;
    }
    if (super) {
        realize(super);
        if (rw(super)->missing_super) { r->missing_super = rw(super)->missing_super; r->realized = true; r->instance_size = ro->instanceSize; return; }
    }
    std::uint32_t start = ro->instanceStart, size = ro->instanceSize;
    if (super && !r->is_meta) {
        const std::uint32_t super_size = rw(super)->instance_size;
        if (super_size > start) {
            const std::uint32_t delta = super_size - start;
            if (ro->ivars) {
                const auto *il = static_cast<const ivar_list_t *>(ro->ivars);
                const auto *iv = reinterpret_cast<const ivar_t *>(il + 1);
                for (std::uint32_t i = 0; i < il->count; i++)
                    if (iv[i].offset) *iv[i].offset += static_cast<std::int32_t>(delta);
            }
            size += delta;
        }
    }
    r->instance_size = size;
    add_method_list(r, ro->baseMethods, true);
    add_protocol_list(r, ro->baseProtocols);
    r->realized = true;
}

Class realize(Class c) {
    if (!c) return nullptr;
    class_rw *r = rw(c);
    if (!r->realized) realize_guest(c);
    return c;
}

/* Registers one guest class (and its metaclass) from __objc_classlist. */
void register_guest_class(Class c) {
    auto *r = new class_rw;
    r->ro = reinterpret_cast<const class_ro_t *>(c->bits & ~std::uintptr_t{7});
    r->name = r->ro->name;
    r->guest_image = true;
    set_rw(c, r);
    Class meta = c->isa;
    auto *mr = new class_rw;
    mr->ro = reinterpret_cast<const class_ro_t *>(meta->bits & ~std::uintptr_t{7});
    mr->name = r->name;
    mr->is_meta = true;
    mr->guest_image = true;
    set_rw(meta, mr);
    g_classes[r->name] = c;
    g_registered_classes.insert(c);
    g_registered_classes.insert(meta);
    if (trace()) std::fprintf(stderr, "objc: class %s super=%p\n", r->name.c_str(), static_cast<void *>(c->superclass));
}

void attach_category(const category_t *cat) {
    Class c = cat->cls;
    if (!c) return;
    if (const char *missing = unresolved_name(c)) {
        if (trace()) std::fprintf(stderr, "objc: category %s on missing class %s skipped\n", cat->name, missing);
        return;
    }
    realize(c);
    add_method_list(rw(c), cat->instanceMethods, true);
    add_protocol_list(rw(c), cat->protocols);
    if (cat->classMethods) {
        realize(c->isa);
        add_method_list(rw(c->isa), cat->classMethods, true);
    }
    if (trace()) std::fprintf(stderr, "objc: category %s(%s)\n", rw(c)->name.c_str(), cat->name);
}

const method_entry *lookup_in(Class c, SEL sel) {
    for (Class k = c; k; k = k->superclass) {
        if (unresolved_name(k)) return nullptr;
        class_rw *r = rw(k);
        if (!r) return nullptr;
        if (!r->realized) realize(k);
        if (r->missing_super) return nullptr;
        auto it = r->cache.find(sel);
        if (it != r->cache.end()) return &it->second;
        for (const auto &m : r->methods)
            if (m.sel == sel) {
                r->cache[sel] = m; /* cache in the class the lookup started... keep it simple: cache where found */
                return &r->cache[sel];
            }
    }
    return nullptr;
}

const method_entry *resolve_stub(Class c, SEL sel);
std::uint64_t invoke_stub(mm_cpu_context &ctx, dynamic_method &method);

std::uint64_t send_host_or_guest(id receiver, const method_entry &m, int nargs, const std::uint64_t *args);

void ensure_initialized(Class c) {
    class_rw *r = rw(c);
    if (!r || r->initialized || r->initializing || r->missing_super) return;
    if (c->superclass && !unresolved_name(c->superclass)) ensure_initialized(c->superclass);
    r->initializing = true;
    SEL init_sel = sel_intern("initialize");
    Class meta = c->isa;
    if (const method_entry *m = lookup_in(meta, init_sel)) {
        /* Only run +initialize implemented by this class or inherited (like Apple: inherited ones run again per subclass). */
        if (trace()) std::fprintf(stderr, "objc: +[%s initialize]\n", r->name.c_str());
        send_host_or_guest(reinterpret_cast<id>(c), *m, 0, nullptr);
    }
    r->initialized = true;
    r->initializing = false;
}

/* Calls an implementation from host code (used by mm_objc_send and by
 * runtime-internal sends such as +initialize). */
std::uint64_t send_host_or_guest(id receiver, const method_entry &m, int nargs, const std::uint64_t *args) {
    std::uint64_t a[8] = {reinterpret_cast<std::uintptr_t>(receiver), reinterpret_cast<std::uintptr_t>(m.sel)};
    for (int i = 0; i < nargs && i < 6; i++) a[2 + i] = args[i];
    if (m.stub) {
        mm_cpu_context *ctx = g_api && g_api->current_context ? g_api->current_context() : nullptr;
        mm_cpu_context call{};
        if (ctx) call = *ctx;
        std::memcpy(call.x, a, sizeof a);
        const auto result = invoke_stub(call, *m.stub);
        if (ctx) {
            ctx->x[1] = call.x[1];
            std::memcpy(ctx->v, call.v, sizeof(call.v[0]) * 4);
        }
        return result;
    }
    if (m.guest)
        return g_api->call_guest(reinterpret_cast<std::uintptr_t>(m.imp), a, 2 + nargs);
    if (mm_cpu_context *ctx = g_api && g_api->current_context ? g_api->current_context() : nullptr) {
        /* Host framework sends carry geometry in the guest's v registers,
         * just like objc_msgSend. Use the ABI trampoline so host IMPs get
         * those arguments too, and propagate their FP aggregate returns. */
        mm_cpu_context call = *ctx;
        std::memcpy(call.x, a, sizeof a);
        mm_hostcall(&call, m.imp);
        if (m.types && (m.types[0] == 'd' || m.types[0] == 'f' || m.types[0] == '{')) {
            ctx->v[0][0] = call.v[0][0];
            ctx->v[1][0] = call.v[1][0];
        }
        return call.x[0];
    }
    using fn8 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    return reinterpret_cast<fn8>(m.imp)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

/* ---- reference counting, pools, weak refs, associations ---------------- */

thread_local std::vector<std::vector<id>> t_pools;

id do_retain(id obj);
void do_release(id obj);

} // namespace

/* Block isas: the guest's block literals point straight at these, so they
 * are the class objects themselves, exported under libsystem_blocks' names.
 * NSObject and Protocol are here for the same reason: the guest binds to
 * their storage, so it must have external linkage. */
MM_OBJC_CLASS_STORAGE(NSObject);
MM_OBJC_CLASS_STORAGE(Protocol);

extern "C" {
__attribute__((visibility("default"))) mm_objc_class mm_block_stack_class __asm__("__NSConcreteStackBlock");
__attribute__((visibility("default"))) mm_objc_class mm_block_malloc_class __asm__("__NSConcreteMallocBlock");
__attribute__((visibility("default"))) mm_objc_class mm_block_global_class __asm__("__NSConcreteGlobalBlock");
__attribute__((visibility("default"))) std::uintptr_t mm_objc_empty_cache[4] __asm__("__objc_empty_cache");
}

namespace {

mm_objc_class g_block_stack_meta, g_block_malloc_meta, g_block_global_meta;
void *block_copy(void *p);
void block_release(void *p);
bool is_block_object(id obj) {
    const auto isa = obj->isa;
    return isa == &mm_block_stack_class || isa == &mm_block_malloc_class || isa == &mm_block_global_class;
}

/* ---- NSObject ----------------------------------------------------------- */


} // namespace

/* ---- public host API ---------------------------------------------------- */

extern "C" {

EXPORT void machoman_dylib_init(const mm_host_api *api);

mm_objc_class *mm_objc_define_class(const char *name, mm_objc_class *super, size_t instance_size,
                                    mm_objc_class *storage, mm_objc_class *meta_storage) {
    std::lock_guard lock(g_lock);
    auto *r = new class_rw;
    r->name = name;
    r->host = true;
    r->realized = true;
    r->instance_size = static_cast<std::uint32_t>(instance_size);
    auto *mr = new class_rw;
    mr->name = name;
    mr->host = true;
    mr->realized = true;
    mr->is_meta = true;
    mr->instance_size = sizeof(mm_objc_class);
    *storage = mm_objc_class{};
    *meta_storage = mm_objc_class{};
    storage->isa = meta_storage;
    storage->superclass = super;
    set_rw(storage, r);
    if (super) {
        meta_storage->isa = super->isa->isa; /* root metaclass */
        meta_storage->superclass = super->isa;
    } else {
        meta_storage->isa = meta_storage;
        meta_storage->superclass = storage;
    }
    set_rw(meta_storage, mr);
    g_classes[r->name] = storage;
    g_registered_classes.insert(storage);
    g_registered_classes.insert(meta_storage);
    return storage;
}

mm_objc_class *mm_objc_class_named(const char *name) {
    std::lock_guard lock(g_lock);
    auto it = g_classes.find(name);
    return it == g_classes.end() ? nullptr : it->second;
}

void mm_objc_add_method(mm_objc_class *cls, const char *selector, void *imp, const char *types) {
    std::lock_guard lock(g_lock);
    class_rw *r = rw(cls);
    r->methods.insert(r->methods.begin(), method_entry{sel_intern_copy(selector), imp, types, is_guest(imp)});
    invalidate_method_caches(r);
}

void mm_objc_add_class_method(mm_objc_class *cls, const char *selector, void *imp, const char *types) {
    mm_objc_add_method(cls->isa, selector, imp, types);
}

mm_SEL mm_objc_sel(const char *name) { return sel_intern_copy(name); }

uint64_t mm_objc_send(mm_id receiver, mm_SEL sel, int nargs, const uint64_t *args) {
    if (!receiver) return 0;
    std::unique_lock lock(g_lock);
    Class c = receiver->isa;
    if (!g_registered_classes.contains(c)) {
        /* An object whose class the runtime never handed out — a corrupted
         * or foreign pointer (Live hit this with a stale ARC reference).
         * Dying here hides every later milestone, so answer nil instead. */
        std::fprintf(stderr, "objc: send %s to object %p with an unregistered class %p — returning nil\n",
                     static_cast<const char *>(sel), static_cast<const void *>(receiver), static_cast<const void *>(c));
        if (std::getenv("MACHOMAN_OBJC_INVALID_BACKTRACE") && g_api && g_api->print_guest_backtrace)
            g_api->print_guest_backtrace();
        return 0;
    }
    ensure_initialized(rw(c)->is_meta ? reinterpret_cast<Class>(receiver) : c);
    const method_entry *m = lookup_in(c, sel);
    if (!m) m = resolve_stub(c, sel);
    if (!m) {
        std::fprintf(stderr, "objc: host send: %c[%s %s] unrecognized\n", rw(c)->is_meta ? '+' : '-', rw(c)->name.c_str(), sel);
        std::abort();
    }
    const bool traced = backtrace_selector(sel);
    if (traced) {
        std::fprintf(stderr, "objc: host send [%s %s] from:\n", rw(c)->name.c_str(), sel);
        if (g_api && g_api->print_guest_backtrace) g_api->print_guest_backtrace();
    }
    /* The metadata lock protects lookup, not execution. An IMP can wait
     * for another thread that needs retain/release or its own message send.
     * Copy the entry because another thread may mutate the method cache. */
    const method_entry implementation = *m;
    lock.unlock();
    const std::uint64_t result = send_host_or_guest(receiver, implementation, nargs, args);
    if (traced)
        std::fprintf(stderr, "objc: host send %s returned 0x%llx\n", sel, static_cast<unsigned long long>(result));
    return result;
}

uint64_t mm_objc_send_super(mm_id receiver, mm_objc_class *cls, mm_SEL sel, int nargs, const uint64_t *args) {
    if (!receiver || !cls || !cls->superclass) return 0;
    method_entry implementation{};
    {
        std::lock_guard lock(g_lock);
        const method_entry *m = lookup_in(cls->superclass, sel_intern(sel));
        if (!m) m = resolve_stub(cls->superclass, sel_intern(sel));
        if (!m) {
            std::fprintf(stderr, "objc: host super send: [%s(super) %s] unrecognized\n", rw(cls)->name.c_str(), sel);
            std::abort();
        }
        implementation = *m;
    }
    return send_host_or_guest(receiver, implementation, nargs, args);
}

uint64_t mm_objc_send0(mm_id r, const char *sel) { return mm_objc_send(r, sel_intern_copy(sel), 0, nullptr); }
uint64_t mm_objc_send1(mm_id r, const char *sel, uint64_t a) { return mm_objc_send(r, sel_intern_copy(sel), 1, &a); }
uint64_t mm_objc_send2(mm_id r, const char *sel, uint64_t a, uint64_t b) { const uint64_t v[2] = {a, b}; return mm_objc_send(r, sel_intern_copy(sel), 2, v); }

mm_id mm_objc_alloc_raw(mm_objc_class *cls, size_t extra) {
    std::lock_guard lock(g_lock);
    realize(cls);
    const std::size_t size = (rw(cls)->instance_size + extra + 15) & ~std::size_t{15};
    auto *obj = static_cast<id>(std::calloc(1, size ? size : 16));
    obj->isa = cls;
    g_refcounts[obj] = {1, true};
    return obj;
}

int mm_objc_construct_instance(mm_id obj) {
    if (!obj) return 0;
    struct cxx_methods { method_entry ctor{}, dtor{}; };
    std::vector<cxx_methods> classes;
    {
        std::lock_guard lock(g_lock);
        for (Class c = realize(obj->isa); c; c = c->superclass) {
            cxx_methods methods;
            /* These methods describe only this class's ivars. An inherited
             * lookup would construct/destruct the same superclass twice. */
            for (const auto &m : rw(c)->methods) {
                if (!methods.ctor.imp && std::strcmp(m.sel, ".cxx_construct") == 0) methods.ctor = m;
                if (!methods.dtor.imp && std::strcmp(m.sel, ".cxx_destruct") == 0) methods.dtor = m;
            }
            if (methods.ctor.imp || methods.dtor.imp) classes.push_back(methods);
        }
    }
    std::vector<method_entry> destructors;
    /* Execute guest code outside the runtime lock, superclass first. */
    for (auto it = classes.rbegin(); it != classes.rend(); ++it) {
        if (it->ctor.imp && !send_host_or_guest(obj, it->ctor, 0, nullptr)) {
            for (auto d = destructors.rbegin(); d != destructors.rend(); ++d)
                send_host_or_guest(obj, *d, 0, nullptr);
            return 0;
        }
        if (it->dtor.imp) destructors.push_back(it->dtor);
    }
    if (!destructors.empty()) {
        std::lock_guard lock(g_lock);
        g_cxx_destructors.emplace(obj, std::move(destructors));
    }
    return 1;
}

void mm_objc_destruct_instance(mm_id obj) {
    std::vector<method_entry> destructors;
    {
        std::lock_guard lock(g_lock);
        auto it = g_cxx_destructors.find(obj);
        if (it == g_cxx_destructors.end()) return;
        destructors = std::move(it->second);
        g_cxx_destructors.erase(it);
    }
    for (auto it = destructors.rbegin(); it != destructors.rend(); ++it)
        send_host_or_guest(obj, *it, 0, nullptr);
}

mm_id mm_objc_alloc(mm_objc_class *cls, size_t extra) {
    id obj = mm_objc_alloc_raw(cls, extra);
    if (mm_objc_construct_instance(obj)) return obj;
    mm_objc_free_instance(obj);
    return nullptr;
}

const char *mm_objc_class_name(mm_objc_class *cls) { return cls && rw(cls) ? rw(cls)->name.c_str() : "?"; }
mm_objc_class *mm_objc_object_class(mm_id obj) { return obj ? obj->isa : nullptr; }

} // extern "C"

namespace {

id do_retain(id obj) {
    if (!obj) return obj;
    if (is_block_object(obj)) return static_cast<id>(block_copy(obj));
    std::lock_guard lock(g_lock);
    auto it = g_refcounts.find(obj);
    if (it == g_refcounts.end()) g_refcounts[obj] = {2, false}; /* not ours (constant/static): count but never free */
    else it->second.count++;
    return obj;
}

void do_dealloc(id obj) {
    /* -dealloc (may be overridden by the guest) then free our storage. */
    std::uint64_t none = 0;
    (void)none;
    mm_objc_send(obj, sel_intern("dealloc"), 0, nullptr);
}

void do_release(id obj) {
    if (!obj) return;
    if (is_block_object(obj)) { block_release(obj); return; }
    bool dealloc = false;
    {
        std::lock_guard lock(g_lock);
        auto it = g_refcounts.find(obj);
        if (it == g_refcounts.end()) return; /* never retained by us: static object */
        if (--it->second.count == 0) {
            if (!it->second.owned) { g_refcounts.erase(it); return; }
            if (g_deallocating.insert(obj).second) dealloc = true;
        }
    }
    if (dealloc) do_dealloc(obj);
}

void free_instance(id obj) {
    mm_objc_destruct_instance(obj);
    std::lock_guard lock(g_lock);
    if (auto it = T().stub_values.find(obj); it != T().stub_values.end()) {
        auto values = std::move(it->second);
        T().stub_values.erase(it);
        for (auto &[key, value] : values) do_release(value.object);
    }
    /* zero weak references */
    if (auto w = g_weak.find(obj); w != g_weak.end()) {
        for (id *slot : w->second) *slot = nullptr;
        g_weak.erase(w);
    }
    if (auto a = g_assoc.find(obj); a != g_assoc.end()) {
        auto copies = std::move(a->second);
        g_assoc.erase(a);
        for (auto &[k, v] : copies)
            if (v.policy & 1) do_release(v.value); /* retained policies have bit 0 set (1, 3) */
    }
    if (auto s = g_sync.find(obj); s != g_sync.end()) { delete s->second; g_sync.erase(s); }
    g_refcounts.erase(obj);
    g_deallocating.erase(obj);
    std::free(obj);
}

/* NSObject methods (host IMPs: (self, _cmd, ...)) */
std::uint64_t NSObject_alloc(id cls, SEL) { return reinterpret_cast<std::uintptr_t>(mm_objc_alloc(reinterpret_cast<Class>(cls), 0)); }
std::uint64_t NSObject_new(id cls, SEL) {
    id obj = reinterpret_cast<id>(mm_objc_send(cls, sel_intern("alloc"), 0, nullptr));
    return mm_objc_send(obj, sel_intern("init"), 0, nullptr);
}
std::uint64_t NSObject_init(id self, SEL) { return reinterpret_cast<std::uintptr_t>(self); }
std::uint64_t NSObject_dealloc(id self, SEL) { free_instance(self); return 0; }
std::uint64_t NSObject_retain(id self, SEL) { return reinterpret_cast<std::uintptr_t>(do_retain(self)); }
std::uint64_t NSObject_release(id self, SEL) { do_release(self); return 0; }
std::uint64_t NSObject_autorelease(id self, SEL) {
    if (t_pools.empty()) t_pools.emplace_back();
    t_pools.back().push_back(self);
    return reinterpret_cast<std::uintptr_t>(self);
}
std::uint64_t NSObject_retainCount(id self, SEL) {
    std::lock_guard lock(g_lock);
    auto it = g_refcounts.find(self);
    return it == g_refcounts.end() ? 1 : static_cast<std::uint64_t>(it->second.count);
}
std::uint64_t NSObject_class(id self, SEL) { return reinterpret_cast<std::uintptr_t>(self->isa); }
std::uint64_t NSObject_classClass(id cls, SEL) { return reinterpret_cast<std::uintptr_t>(cls); }
std::uint64_t NSObject_superclass(id self, SEL) { return reinterpret_cast<std::uintptr_t>(self->isa->superclass); }
std::uint64_t NSObject_classSuperclass(id cls, SEL) { return reinterpret_cast<std::uintptr_t>(reinterpret_cast<Class>(cls)->superclass); }
std::uint64_t NSObject_self(id self, SEL) { return reinterpret_cast<std::uintptr_t>(self); }
std::uint64_t NSObject_isEqual(id self, SEL, id other) { return self == other; }
std::uint64_t NSObject_hash(id self, SEL) { return reinterpret_cast<std::uintptr_t>(self) >> 4; }
std::uint64_t NSObject_isKindOfClass(id self, SEL, Class k) {
    for (Class c = self->isa; c; c = c->superclass) if (c == k) return 1;
    return 0;
}
std::uint64_t NSObject_isMemberOfClass(id self, SEL, Class k) { return self->isa == k; }
std::uint64_t NSObject_classIsSubclassOfClass(id cls, SEL, Class k) {
    for (Class c = reinterpret_cast<Class>(cls); c; c = c->superclass) if (c == k) return 1;
    return 0;
}
std::uint64_t NSObject_respondsToSelector(id self, SEL, SEL s) {
    std::lock_guard lock(g_lock);
    return lookup_in(self->isa, sel_intern(s)) != nullptr;
}
std::uint64_t NSObject_instancesRespondToSelector(id cls, SEL, SEL s) {
    std::lock_guard lock(g_lock);
    return lookup_in(reinterpret_cast<Class>(cls), sel_intern(s)) != nullptr;
}
std::uint64_t NSObject_conformsToProtocol(id self, SEL, protocol_t *p) {
    std::lock_guard lock(g_lock);
    Class start = rw(self->isa)->is_meta ? reinterpret_cast<Class>(self) : self->isa;
    for (Class c = start; c; c = c->superclass) {
        realize(c);
        for (protocol_t *q : rw(c)->protocols)
            if (q == p || (q && p && q->name && p->name && std::strcmp(q->name, p->name) == 0)) return 1;
    }
    return 0;
}
std::uint64_t NSObject_performSelector(id self, SEL, SEL s) { return mm_objc_send(self, sel_intern(s), 0, nullptr); }
std::uint64_t NSObject_performSelector1(id self, SEL, SEL s, std::uint64_t a) { return mm_objc_send(self, sel_intern(s), 1, &a); }
std::uint64_t NSObject_performSelector2(id self, SEL, SEL s, std::uint64_t a, std::uint64_t b) { const std::uint64_t v[2] = {a, b}; return mm_objc_send(self, sel_intern(s), 2, v); }
std::uint64_t NSObject_methodForSelector(id self, SEL, SEL s) {
    std::lock_guard lock(g_lock);
    const method_entry *m = lookup_in(self->isa, sel_intern(s));
    return m ? reinterpret_cast<std::uintptr_t>(m->imp) : 0;
}
std::uint64_t NSObject_doesNotRecognizeSelector(id self, SEL, SEL s) {
    std::fprintf(stderr, "objc: %c[%s %s]: unrecognized selector sent to %p\n", rw(self->isa)->is_meta ? '+' : '-',
                 rw(self->isa)->name.c_str(), s, static_cast<void *>(self));
    std::abort();
}
std::uint64_t NSObject_description(id self, SEL) {
    /* Returns a C string for now; Foundation overrides this with NSString. */
    static thread_local char buf[128];
    std::snprintf(buf, sizeof buf, "<%s: %p>", rw(self->isa)->name.c_str(), static_cast<void *>(self));
    return reinterpret_cast<std::uintptr_t>(buf);
}
std::uint64_t NSObject_copy(id self, SEL) { return mm_objc_send1(self, "copyWithZone:", 0); }
std::uint64_t NSObject_zone(id, SEL) { return 0; }
std::uint64_t NSObject_noop(id self, SEL) { return reinterpret_cast<std::uintptr_t>(self); }
std::uint64_t NSObject_forwardingTarget(id, SEL, SEL) { return 0; }
std::uint64_t NSObject_isProxy(id, SEL) { return 0; }
std::uint64_t NSObject_classDescription(id cls, SEL) { return reinterpret_cast<std::uintptr_t>(rw(reinterpret_cast<Class>(cls))->name.c_str()); }
std::uint64_t NSObject_initialize(id, SEL) { return 0; }
std::uint64_t NSObject_load(id, SEL) { return 0; }

/* ---- blocks (libsystem_blocks ABI) -------------------------------------- */

struct block_descriptor { std::uintptr_t reserved, size; void (*copy)(void *, const void *); void (*dispose)(const void *); const char *signature; };
struct block_layout { Class isa; std::int32_t flags; std::int32_t reserved; void (*invoke)(void *, ...); block_descriptor *descriptor; };
struct block_byref { Class isa; block_byref *forwarding; std::int32_t flags; std::int32_t size; void (*keep)(block_byref *, block_byref *); void (*destroy)(block_byref *); };
constexpr std::int32_t BLOCK_REFCOUNT_MASK = 0xFFFF, BLOCK_NEEDS_FREE = 1 << 24, BLOCK_HAS_COPY_DISPOSE = 1 << 25,
                       BLOCK_IS_GLOBAL = 1 << 28, BLOCK_BYREF_HAS_COPY_DISPOSE = 1 << 25;

void call_helper(void *fn, std::uint64_t a, std::uint64_t b) {
    if (!fn) return;
    if (is_guest(fn)) { const std::uint64_t args[2] = {a, b}; g_api->call_guest(reinterpret_cast<std::uintptr_t>(fn), args, 2); return; }
    reinterpret_cast<void (*)(std::uint64_t, std::uint64_t)>(fn)(a, b);
}

void *block_copy(void *p) {
    auto *b = static_cast<block_layout *>(p);
    if (!b) return nullptr;
    if (b->flags & BLOCK_IS_GLOBAL) return b;
    if (b->flags & BLOCK_NEEDS_FREE) { std::lock_guard lock(g_lock); b->flags++; return b; }
    auto *copy = static_cast<block_layout *>(std::malloc(b->descriptor->size));
    std::memcpy(copy, b, b->descriptor->size);
    copy->isa = &mm_block_malloc_class;
    copy->flags = (b->flags & ~BLOCK_REFCOUNT_MASK) | BLOCK_NEEDS_FREE | 1;
    if (b->flags & BLOCK_HAS_COPY_DISPOSE)
        call_helper(reinterpret_cast<void *>(b->descriptor->copy), reinterpret_cast<std::uintptr_t>(copy), reinterpret_cast<std::uintptr_t>(b));
    return copy;
}

void block_release(void *p) {
    auto *b = static_cast<block_layout *>(p);
    if (!b || (b->flags & BLOCK_IS_GLOBAL) || !(b->flags & BLOCK_NEEDS_FREE)) return;
    {
        std::lock_guard lock(g_lock);
        if ((--b->flags & BLOCK_REFCOUNT_MASK) != 0) return;
    }
    if (b->flags & BLOCK_HAS_COPY_DISPOSE)
        call_helper(reinterpret_cast<void *>(b->descriptor->dispose), reinterpret_cast<std::uintptr_t>(b), 0);
    std::free(b);
}

block_byref *byref_copy(block_byref *src) {
    src = src->forwarding;
    if (src->flags & BLOCK_NEEDS_FREE) { std::lock_guard lock(g_lock); src->flags++; return src; }
    auto *copy = static_cast<block_byref *>(std::malloc(static_cast<std::size_t>(src->size)));
    std::memcpy(copy, src, static_cast<std::size_t>(src->size));
    copy->forwarding = copy;
    src->forwarding = copy;
    copy->flags = (src->flags & ~BLOCK_REFCOUNT_MASK) | BLOCK_NEEDS_FREE | 2; /* the stack copy and the heap copy */
    if (src->flags & BLOCK_BYREF_HAS_COPY_DISPOSE)
        call_helper(reinterpret_cast<void *>(src->keep), reinterpret_cast<std::uintptr_t>(copy), reinterpret_cast<std::uintptr_t>(src));
    return copy;
}

void byref_release(block_byref *b) {
    b = b->forwarding;
    if (!(b->flags & BLOCK_NEEDS_FREE)) return;
    {
        std::lock_guard lock(g_lock);
        if ((--b->flags & BLOCK_REFCOUNT_MASK) != 0) return;
    }
    if (b->flags & BLOCK_BYREF_HAS_COPY_DISPOSE)
        call_helper(reinterpret_cast<void *>(b->destroy), reinterpret_cast<std::uintptr_t>(b), 0);
    std::free(b);
}

std::uint64_t Block_copyMethod(id self, SEL) { return reinterpret_cast<std::uintptr_t>(block_copy(self)); }
std::uint64_t Block_retain(id self, SEL) { return reinterpret_cast<std::uintptr_t>(block_copy(self)); }
std::uint64_t Block_release(id self, SEL) { block_release(self); return 0; }

/* ---- image registration -------------------------------------------------- */

struct image_sections { const mm_section_64 *selrefs = nullptr, *classlist = nullptr, *nlclslist = nullptr, *catlist = nullptr, *nlcatlist = nullptr, *protolist = nullptr; };

struct load_callback { Class receiver; void *imp; };
std::vector<load_callback> g_class_loads, g_category_loads;

void load_image(const void *header, std::uint64_t slide) {
    const auto *hdr = static_cast<const mm_mach_header_64 *>(header);
    std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t *>(header), sizeof(mm_mach_header_64) + hdr->sizeofcmds);
    mm_macho_image img;
    if (mm_macho_parse(bytes, img) != mm_macho_status::ok) return;
    /* Section vmaddres are unslid; the executable maps at slide 0 and the
     * dylibs carry their own, so every address below reads through it. */
    image_sections s;
    for (const auto *seg : img.segments)
        for (const auto &sect : img.sections(*seg)) {
            const auto name = mm_macho_name(sect.sectname);
            if (name == "__objc_selrefs") s.selrefs = &sect;
            else if (name == "__objc_classlist") s.classlist = &sect;
            else if (name == "__objc_nlclslist") s.nlclslist = &sect;
            else if (name == "__objc_catlist") s.catlist = &sect;
            else if (name == "__objc_nlcatlist") s.nlcatlist = &sect;
            else if (name == "__objc_protolist") s.protolist = &sect;
        }
    std::lock_guard lock(g_lock);
    if (s.selrefs) {
        auto *refs = reinterpret_cast<const char **>(s.selrefs->addr + slide);
        for (std::uint64_t i = 0; i < s.selrefs->size / 8; i++)
            refs[i] = sel_intern(refs[i]);
    }
    if (s.protolist) {
        auto *protos = reinterpret_cast<protocol_t **>(s.protolist->addr + slide);
        for (std::uint64_t i = 0; i < s.protolist->size / 8; i++) {
            protocol_t *p = protos[i];
            if (!p->base.isa) p->base.isa = MM_OBJC_CLASS_REF(Protocol);
            if (!g_protocols.contains(p->name)) g_protocols[p->name] = p;
        }
    }
    std::vector<Class> classes;
    if (s.classlist) {
        auto *list = reinterpret_cast<Class *>(s.classlist->addr + slide);
        for (std::uint64_t i = 0; i < s.classlist->size / 8; i++) {
            register_guest_class(list[i]);
            classes.push_back(list[i]);
        }
    }
    std::vector<const category_t *> cats;
    if (s.catlist) {
        auto *list = reinterpret_cast<const category_t **>(s.catlist->addr + slide);
        for (std::uint64_t i = 0; i < s.catlist->size / 8; i++) cats.push_back(list[i]);
    }
    for (Class c : classes) realize(c);
    /* Save the class's own +load before categories override method lookup.
     * Execute only after all images and translations are ready. */
    SEL load_sel = sel_intern("load");
    for (Class c : classes) {
        if (rw(c)->missing_super || unresolved_name(c->isa)) continue;
        realize(c->isa);
        for (const auto &m : rw(c->isa)->methods)
            if (m.sel == load_sel && m.guest) {
                if (trace()) std::fprintf(stderr, "objc: +[%s load]\n", rw(c)->name.c_str());
                g_class_loads.push_back({c, m.imp});
                break;
            }
    }
    for (const category_t *cat : cats) attach_category(cat);
    for (const category_t *cat : cats) {
        if (!cat->classMethods || unresolved_name(cat->cls) || rw(cat->cls)->missing_super) continue;
        const auto *list = cat->classMethods;
        const bool relative = list->entsize_flags & 0x80000000u;
        const std::uint32_t entsize = list->entsize_flags & 0xFFFCu;
        const auto *base = reinterpret_cast<const std::uint8_t *>(list + 1);
        for (std::uint32_t i = 0; i < list->count; i++) {
            const std::uint8_t *e = base + i * entsize;
            SEL sel; void *imp;
            if (relative) {
                const auto *rm = reinterpret_cast<const method_rel *>(e);
                sel = *reinterpret_cast<const SEL *>(e + rm->name);
                imp = reinterpret_cast<void *>(const_cast<std::uint8_t *>(e) + 8 + rm->imp);
            } else {
                const auto *bm = reinterpret_cast<const method_big *>(e);
                sel = bm->name; imp = bm->imp;
            }
            if (sel_intern(sel) == load_sel) {
                if (trace()) std::fprintf(stderr, "objc: +[%s(%s) load]\n", rw(cat->cls)->name.c_str(), cat->name);
                g_category_loads.push_back({cat->cls, imp});
            }
        }
    }
    if (trace()) std::fprintf(stderr, "objc: image registered: %zu classes, %zu categories\n", classes.size(), cats.size());
}

/* ---- message dispatch --------------------------------------------------- */

#include "dynamic_stubs.inc"

/* Resolves receiver/selector and hands the IMP to the runtime. Returns the
 * receiver so x0 is unchanged when the guest resumes at the IMP. */
std::uint64_t dispatch(mm_cpu_context *ctx, id receiver, SEL sel, Class start) {
    if (!receiver) {
        std::memset(ctx->v, 0, sizeof(ctx->v[0]) * 4);
        ctx->x[1] = 0;
        ctx->host_result_in_context = 1;
        return 0;
    }
    method_entry implementation{};
    {
        std::lock_guard lock(g_lock);
        Class c = start;
        class_rw *r = rw(c);
        if (!r) {
            std::fprintf(stderr, "objc: message %s to object %p with an unregistered class %p\n", sel, static_cast<void *>(receiver), static_cast<void *>(c));
            std::abort();
        }
        if (r->missing_super) {
            std::fprintf(stderr, "objc: %c[%s %s]: class depends on %s, which no host framework provides\n", r->is_meta ? '+' : '-',
                         r->name.c_str(), sel, r->missing_super);
            std::abort();
        }
        ensure_initialized(r->is_meta ? reinterpret_cast<Class>(receiver) : c);
        const method_entry *m = lookup_in(c, sel);
        if (!m) {
            SEL fwd = sel_intern("forwardingTargetForSelector:");
            if (const method_entry *f = lookup_in(c, fwd); f && f->imp != reinterpret_cast<void *>(&NSObject_forwardingTarget)) {
                const std::uint64_t a = reinterpret_cast<std::uintptr_t>(sel);
                const auto target = reinterpret_cast<id>(send_host_or_guest(receiver, *f, 1, &a));
                if (target && target != receiver) {
                    ctx->x[0] = reinterpret_cast<std::uintptr_t>(target);
                    return dispatch(ctx, target, sel, target->isa);
                }
            }
            m = resolve_stub(c, sel);
        }
        if (!m) {
            /* A class machoman implements is always partial: Apple's have
             * hundreds of methods and this has the ones programs were seen
             * to use. Stopping the process at the first one that is missing
             * makes every further gap invisible, so a host class reports it
             * and answers nil — the same shape an unimplemented framework
             * symbol has. A guest class is different: there the method
             * really should exist, so that stays fatal.
             * $MACHOMAN_STRICT_SELECTORS=1 makes both fatal. */
            static const bool strict = std::getenv("MACHOMAN_STRICT_SELECTORS") != nullptr;
            /* "A host class" means one machoman implements anywhere up the
             * chain: an app's own NSWindow subclass inherits the gaps of
             * the NSWindow here, and the missing method is this project's
             * to add, not the app's. */
            bool host_class = false;
            for (Class c = start; c && !host_class; c = c->superclass)
                host_class = rw(c) && rw(c)->host;
            std::fprintf(stderr, "objc: %c[%s %s]: unrecognized selector sent to %p (from 0x%llx)%s\n",
                         r->is_meta ? '+' : '-', r->name.c_str(), sel, static_cast<void *>(receiver),
                         static_cast<unsigned long long>(ctx->x[30]),
                         host_class && !strict ? "; answering nil" : "");
            if (!host_class || strict) std::abort();
            ctx->x[0] = 0;
            ctx->x[1] = 0;
            std::memset(ctx->v, 0, sizeof(ctx->v[0]) * 4);
            ctx->host_tail_fn = 0;
            ctx->resume_va = 0;
            ctx->host_result_in_context = 1;
            return 0;
        }
        implementation = *m;
    }
    const auto *m = &implementation;
    if (m->stub) {
        ctx->host_tail_fn = 0;
        ctx->resume_va = 0;
        auto result = invoke_stub(*ctx, *m->stub);
        ctx->host_result_in_context = 1;
        return result;
    }
    if (trace()) std::fprintf(stderr, "objc: %c[%s %s] -> %s %p\n", rw(start)->is_meta ? '+' : '-', rw(start)->name.c_str(), sel,
                              m->guest ? "guest" : "host", m->imp);
    if (backtrace_selector(sel)) {
        std::fprintf(stderr, "objc: %c[%s %s] sent from:\n", rw(start)->is_meta ? '+' : '-', rw(start)->name.c_str(), sel);
        if (g_api && g_api->print_guest_backtrace) g_api->print_guest_backtrace();
    }
    if (m->guest) ctx->resume_va = reinterpret_cast<std::uintptr_t>(m->imp);
    else ctx->host_tail_fn = reinterpret_cast<std::uintptr_t>(m->imp);
    return reinterpret_cast<std::uintptr_t>(receiver);
}

struct objc_super { id receiver; Class super_class; };

void define_root_classes() {
    Class nsobject = mm_objc_define_class("NSObject", nullptr, sizeof(mm_objc_object), MM_OBJC_CLASS_REF(NSObject), MM_OBJC_METACLASS_REF(NSObject));
    struct { const char *sel; void *imp; const char *types; bool cls; } table[] = {
        {"alloc", reinterpret_cast<void *>(&NSObject_alloc), "@16@0:8", true},
        {"allocWithZone:", reinterpret_cast<void *>(&NSObject_alloc), "@24@0:8^v16", true},
        {"new", reinterpret_cast<void *>(&NSObject_new), "@16@0:8", true},
        {"class", reinterpret_cast<void *>(&NSObject_classClass), "#16@0:8", true},
        {"superclass", reinterpret_cast<void *>(&NSObject_classSuperclass), "#16@0:8", true},
        {"isSubclassOfClass:", reinterpret_cast<void *>(&NSObject_classIsSubclassOfClass), "B24@0:8#16", true},
        {"instancesRespondToSelector:", reinterpret_cast<void *>(&NSObject_instancesRespondToSelector), "B24@0:8:16", true},
        {"conformsToProtocol:", reinterpret_cast<void *>(&NSObject_conformsToProtocol), "B24@0:8@16", true},
        {"description", reinterpret_cast<void *>(&NSObject_classDescription), "@16@0:8", true},
        {"initialize", reinterpret_cast<void *>(&NSObject_initialize), "v16@0:8", true},
        {"load", reinterpret_cast<void *>(&NSObject_load), "v16@0:8", true},
        {"self", reinterpret_cast<void *>(&NSObject_self), "@16@0:8", true},
        {"retain", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
        /* A class object is immortal, so copying one yields the class. */
        {"copy", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
        {"copyWithZone:", reinterpret_cast<void *>(&NSObject_noop), "@24@0:8^v16", true},
        {"mutableCopy", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
        {"mutableCopyWithZone:", reinterpret_cast<void *>(&NSObject_noop), "@24@0:8^v16", true},
        {"hash", reinterpret_cast<void *>(&NSObject_hash), "Q16@0:8", true},
        {"isEqual:", reinterpret_cast<void *>(&NSObject_isEqual), "B24@0:8@16", true},
        {"release", reinterpret_cast<void *>(&NSObject_zone), "v16@0:8", true},
        {"autorelease", reinterpret_cast<void *>(&NSObject_noop), "@16@0:8", true},
        {"respondsToSelector:", reinterpret_cast<void *>(&NSObject_respondsToSelector), "B24@0:8:16", true},
        {"isKindOfClass:", reinterpret_cast<void *>(&NSObject_isKindOfClass), "B24@0:8#16", true},
        {"init", reinterpret_cast<void *>(&NSObject_init), "@16@0:8", false},
        {"dealloc", reinterpret_cast<void *>(&NSObject_dealloc), "v16@0:8", false},
        {"retain", reinterpret_cast<void *>(&NSObject_retain), "@16@0:8", false},
        {"release", reinterpret_cast<void *>(&NSObject_release), "v16@0:8", false},
        {"autorelease", reinterpret_cast<void *>(&NSObject_autorelease), "@16@0:8", false},
        {"retainCount", reinterpret_cast<void *>(&NSObject_retainCount), "Q16@0:8", false},
        {"class", reinterpret_cast<void *>(&NSObject_class), "#16@0:8", false},
        {"superclass", reinterpret_cast<void *>(&NSObject_superclass), "#16@0:8", false},
        {"self", reinterpret_cast<void *>(&NSObject_self), "@16@0:8", false},
        {"isEqual:", reinterpret_cast<void *>(&NSObject_isEqual), "B24@0:8@16", false},
        {"hash", reinterpret_cast<void *>(&NSObject_hash), "Q16@0:8", false},
        {"isKindOfClass:", reinterpret_cast<void *>(&NSObject_isKindOfClass), "B24@0:8#16", false},
        {"isMemberOfClass:", reinterpret_cast<void *>(&NSObject_isMemberOfClass), "B24@0:8#16", false},
        {"respondsToSelector:", reinterpret_cast<void *>(&NSObject_respondsToSelector), "B24@0:8:16", false},
        {"conformsToProtocol:", reinterpret_cast<void *>(&NSObject_conformsToProtocol), "B24@0:8@16", false},
        {"performSelector:", reinterpret_cast<void *>(&NSObject_performSelector), "@24@0:8:16", false},
        {"performSelector:withObject:", reinterpret_cast<void *>(&NSObject_performSelector1), "@32@0:8:16@24", false},
        {"performSelector:withObject:withObject:", reinterpret_cast<void *>(&NSObject_performSelector2), "@40@0:8:16@24@32", false},
        {"methodForSelector:", reinterpret_cast<void *>(&NSObject_methodForSelector), "^?24@0:8:16", false},
        {"doesNotRecognizeSelector:", reinterpret_cast<void *>(&NSObject_doesNotRecognizeSelector), "v24@0:8:16", false},
        {"forwardingTargetForSelector:", reinterpret_cast<void *>(&NSObject_forwardingTarget), "@24@0:8:16", false},
        {"description", reinterpret_cast<void *>(&NSObject_description), "@16@0:8", false},
        {"copy", reinterpret_cast<void *>(&NSObject_copy), "@16@0:8", false},
        {"zone", reinterpret_cast<void *>(&NSObject_zone), "^v16@0:8", false},
        {"isProxy", reinterpret_cast<void *>(&NSObject_isProxy), "B16@0:8", false},
    };
    for (const auto &e : table)
        (e.cls ? mm_objc_add_class_method : mm_objc_add_method)(nsobject, e.sel, e.imp, e.types);

    mm_objc_define_class("Protocol", nsobject, sizeof(protocol_t), MM_OBJC_CLASS_REF(Protocol), MM_OBJC_METACLASS_REF(Protocol));
    struct { const char *name; Class cls; Class meta; } blocks[] = {
        {"__NSStackBlock__", &mm_block_stack_class, &g_block_stack_meta},
        {"__NSMallocBlock__", &mm_block_malloc_class, &g_block_malloc_meta},
        {"__NSGlobalBlock__", &mm_block_global_class, &g_block_global_meta},
    };
    for (const auto &b : blocks) {
        Class blk = b.cls;
        mm_objc_define_class(b.name, nsobject, sizeof(block_layout), blk, b.meta);
        mm_objc_add_method(blk, "copy", reinterpret_cast<void *>(&Block_copyMethod), "@16@0:8");
        mm_objc_add_method(blk, "copyWithZone:", reinterpret_cast<void *>(&Block_copyMethod), "@24@0:8^v16");
        mm_objc_add_method(blk, "retain", reinterpret_cast<void *>(&Block_retain), "@16@0:8");
        mm_objc_add_method(blk, "release", reinterpret_cast<void *>(&Block_release), "v16@0:8");
    }
}

} // namespace

/* ---- exported runtime entry points ------------------------------------- */

extern "C" {

EXPORT void machoman_dylib_init(const mm_host_api *api) {
    g_api = api;
    /* Register the objc metadata of every mapped guest image — the
     * executable first, then the dylibs, whose classes the guest allocates
     * and retains just like the executable's own. */
    if (api->image_count)
        for (std::size_t i = 0, n = api->image_count(); i < n; i++)
            if (const void *header = api->image_header(i)) load_image(header, api->image_slide(i));
    if (g_classes.empty() && api->main_image) load_image(api->main_image, 0);
}

EXPORT void machoman_dylib_start() {
    std::vector<load_callback> classes, categories;
    SEL load_sel;
    {
        std::lock_guard lock(g_lock);
        classes.swap(g_class_loads);
        categories.swap(g_category_loads);
        load_sel = sel_intern("load");
    }
    /* Call guest code outside the metadata lock, classes before categories. */
    for (const auto *callbacks : {&classes, &categories})
        for (const auto &callback : *callbacks) {
            const std::uint64_t args[2] = {reinterpret_cast<std::uintptr_t>(callback.receiver),
                                         reinterpret_cast<std::uintptr_t>(load_sel)};
            g_api->call_guest(reinterpret_cast<std::uintptr_t>(callback.imp), args, 2);
        }
}

static void objc_ctor() __attribute__((constructor));
static void objc_ctor() { define_root_classes(); }

std::uint64_t objc_msgSend(std::uint64_t self, std::uint64_t sel) DARWIN(objc_msgSend);
std::uint64_t objc_msgSend(std::uint64_t self, std::uint64_t sel) {
    mm_cpu_context *ctx = g_api->current_context();
    const auto receiver = reinterpret_cast<id>(self);
    return dispatch(ctx, receiver, reinterpret_cast<SEL>(sel), receiver ? receiver->isa : nullptr);
}

std::uint64_t objc_msgSendSuper2(objc_super *sup, std::uint64_t sel) DARWIN(objc_msgSendSuper2);
std::uint64_t objc_msgSendSuper2(objc_super *sup, std::uint64_t sel) {
    mm_cpu_context *ctx = g_api->current_context();
    ctx->x[0] = reinterpret_cast<std::uintptr_t>(sup->receiver);
    return dispatch(ctx, sup->receiver, reinterpret_cast<SEL>(sel), sup->super_class->superclass);
}

std::uint64_t objc_msgSendSuper(objc_super *sup, std::uint64_t sel) DARWIN(objc_msgSendSuper);
std::uint64_t objc_msgSendSuper(objc_super *sup, std::uint64_t sel) {
    mm_cpu_context *ctx = g_api->current_context();
    ctx->x[0] = reinterpret_cast<std::uintptr_t>(sup->receiver);
    return dispatch(ctx, sup->receiver, reinterpret_cast<SEL>(sel), sup->super_class);
}

/* Fast paths the compiler emits for common messages. */
std::uint64_t objc_alloc(Class cls) DARWIN(objc_alloc);
std::uint64_t objc_alloc(Class cls) { return cls ? mm_objc_send(reinterpret_cast<id>(cls), sel_intern("alloc"), 0, nullptr) : 0; }
std::uint64_t objc_alloc_init(Class cls) DARWIN(objc_alloc_init);
std::uint64_t objc_alloc_init(Class cls) {
    if (!cls) return 0;
    const auto obj = reinterpret_cast<id>(mm_objc_send(reinterpret_cast<id>(cls), sel_intern("alloc"), 0, nullptr));
    return mm_objc_send(obj, sel_intern("init"), 0, nullptr);
}
std::uint64_t objc_opt_new(Class cls) DARWIN(objc_opt_new);
std::uint64_t objc_opt_new(Class cls) { return cls ? mm_objc_send(reinterpret_cast<id>(cls), sel_intern("new"), 0, nullptr) : 0; }
std::uint64_t objc_opt_self(id obj) DARWIN(objc_opt_self);
std::uint64_t objc_opt_self(id obj) { return reinterpret_cast<std::uintptr_t>(obj); }
std::uint64_t objc_opt_class(id obj) DARWIN(objc_opt_class);
std::uint64_t objc_opt_class(id obj) { return obj ? mm_objc_send(obj, sel_intern("class"), 0, nullptr) : 0; }
std::uint64_t objc_opt_isKindOfClass(id obj, Class cls) DARWIN(objc_opt_isKindOfClass);
std::uint64_t objc_opt_isKindOfClass(id obj, Class cls) { return obj ? mm_objc_send1(obj, "isKindOfClass:", reinterpret_cast<std::uintptr_t>(cls)) : 0; }
std::uint64_t objc_opt_respondsToSelector(id obj, SEL sel) DARWIN(objc_opt_respondsToSelector);
std::uint64_t objc_opt_respondsToSelector(id obj, SEL sel) { return obj ? mm_objc_send1(obj, "respondsToSelector:", reinterpret_cast<std::uintptr_t>(sel)) : 0; }
std::uint64_t objc_opt_isKindOfClass_(id obj, Class cls) DARWIN(objc_opt_isKindOfClass_);
std::uint64_t objc_opt_isKindOfClass_(id obj, Class cls) { return objc_opt_isKindOfClass(obj, cls); }

/* ARC */
id objc_retain(id obj) DARWIN(objc_retain);
id objc_retain(id obj) {
    if (!obj) return obj;
    return reinterpret_cast<id>(mm_objc_send(obj, sel_intern("retain"), 0, nullptr));
}
void objc_release(id obj) DARWIN(objc_release);
void objc_release(id obj) { if (obj) mm_objc_send(obj, sel_intern("release"), 0, nullptr); }
id objc_autorelease(id obj) DARWIN(objc_autorelease);
id objc_autorelease(id obj) { return obj ? reinterpret_cast<id>(mm_objc_send(obj, sel_intern("autorelease"), 0, nullptr)) : obj; }
id objc_retainAutorelease(id obj) DARWIN(objc_retainAutorelease);
id objc_retainAutorelease(id obj) { return objc_autorelease(objc_retain(obj)); }
id objc_retainAutoreleaseReturnValue(id obj) DARWIN(objc_retainAutoreleaseReturnValue);
id objc_retainAutoreleaseReturnValue(id obj) { return objc_autorelease(objc_retain(obj)); }
id objc_autoreleaseReturnValue(id obj) DARWIN(objc_autoreleaseReturnValue);
id objc_autoreleaseReturnValue(id obj) { return objc_autorelease(obj); }
id objc_retainAutoreleasedReturnValue(id obj) DARWIN(objc_retainAutoreleasedReturnValue);
id objc_retainAutoreleasedReturnValue(id obj) { return objc_retain(obj); }
id objc_unsafeClaimAutoreleasedReturnValue(id obj) DARWIN(objc_unsafeClaimAutoreleasedReturnValue);
id objc_unsafeClaimAutoreleasedReturnValue(id obj) { return obj; }
id objc_claimAutoreleasedReturnValue(id obj) DARWIN(objc_claimAutoreleasedReturnValue);
id objc_claimAutoreleasedReturnValue(id obj) { return objc_retain(obj); }
void objc_storeStrong(id *location, id value) DARWIN(objc_storeStrong);
void objc_storeStrong(id *location, id value) {
    id old = *location;
    objc_retain(value);
    *location = value;
    objc_release(old);
}
id objc_retainBlock(id block) DARWIN(objc_retainBlock);
id objc_retainBlock(id block) { return static_cast<id>(block_copy(block)); }
void *objc_autoreleasePoolPush() DARWIN(objc_autoreleasePoolPush);
void *objc_autoreleasePoolPush() {
    t_pools.emplace_back();
    return reinterpret_cast<void *>(t_pools.size());
}
void objc_autoreleasePoolPop(void *token) DARWIN(objc_autoreleasePoolPop);
void objc_autoreleasePoolPop(void *token) {
    const auto depth = reinterpret_cast<std::uintptr_t>(token);
    while (t_pools.size() >= depth && !t_pools.empty()) {
        std::vector<id> pool = std::move(t_pools.back());
        t_pools.pop_back();
        for (auto it = pool.rbegin(); it != pool.rend(); ++it) objc_release(*it);
    }
}
void objc_destructInstance(id obj) DARWIN(objc_destructInstance);
void objc_destructInstance(id obj) {
    std::lock_guard lock(g_lock);
    if (auto w = g_weak.find(obj); w != g_weak.end()) { for (id *s : w->second) *s = nullptr; g_weak.erase(w); }
    g_assoc.erase(obj);
}

/* weak references */
id objc_initWeak(id *location, id value) DARWIN(objc_initWeak);
id objc_initWeak(id *location, id value) {
    std::lock_guard lock(g_lock);
    *location = value;
    if (value) g_weak[value].push_back(location);
    return value;
}
id objc_storeWeak(id *location, id value) DARWIN(objc_storeWeak);
id objc_storeWeak(id *location, id value) {
    std::lock_guard lock(g_lock);
    if (id old = *location) {
        auto &v = g_weak[old];
        std::erase(v, location);
    }
    *location = value;
    if (value) g_weak[value].push_back(location);
    return value;
}
void objc_destroyWeak(id *location) DARWIN(objc_destroyWeak);
void objc_destroyWeak(id *location) {
    std::lock_guard lock(g_lock);
    if (id old = *location) std::erase(g_weak[old], location);
}
id objc_loadWeakRetained(id *location) DARWIN(objc_loadWeakRetained);
id objc_loadWeakRetained(id *location) { std::lock_guard lock(g_lock); return objc_retain(*location); }
id objc_loadWeak(id *location) DARWIN(objc_loadWeak);
id objc_loadWeak(id *location) { return objc_autorelease(objc_loadWeakRetained(location)); }
void objc_copyWeak(id *dst, id *src) DARWIN(objc_copyWeak);
void objc_copyWeak(id *dst, id *src) { objc_initWeak(dst, objc_loadWeakRetained(src)); objc_release(*dst); }
void objc_moveWeak(id *dst, id *src) DARWIN(objc_moveWeak);
void objc_moveWeak(id *dst, id *src) { objc_copyWeak(dst, src); objc_destroyWeak(src); }

/* associated objects */
void objc_setAssociatedObject(id obj, const void *key, id value, std::uintptr_t policy) DARWIN(objc_setAssociatedObject);
void objc_setAssociatedObject(id obj, const void *key, id value, std::uintptr_t policy) {
    id old = nullptr; std::uintptr_t old_policy = 0;
    {
        std::lock_guard lock(g_lock);
        auto &m = g_assoc[obj];
        if (auto it = m.find(key); it != m.end()) { old = it->second.value; old_policy = it->second.policy; }
        if (policy & 1) objc_retain(value); /* OBJC_ASSOCIATION_RETAIN(_NONATOMIC) = 1/0x301, COPY = 3/0x303 */
        if (policy == 3 || policy == 0x303) value = reinterpret_cast<id>(mm_objc_send(value, sel_intern("copy"), 0, nullptr));
        m[key] = {value, policy};
    }
    if (old && (old_policy & 1)) objc_release(old);
}
id objc_getAssociatedObject(id obj, const void *key) DARWIN(objc_getAssociatedObject);
id objc_getAssociatedObject(id obj, const void *key) {
    std::lock_guard lock(g_lock);
    auto it = g_assoc.find(obj);
    if (it == g_assoc.end()) return nullptr;
    auto jt = it->second.find(key);
    return jt == it->second.end() ? nullptr : jt->second.value;
}
void objc_removeAssociatedObjects(id obj) DARWIN(objc_removeAssociatedObjects);
void objc_removeAssociatedObjects(id obj) { std::lock_guard lock(g_lock); g_assoc.erase(obj); }

/* @synchronized */
int objc_sync_enter(id obj) DARWIN(objc_sync_enter);
int objc_sync_enter(id obj) {
    if (!obj) return 0;
    std::recursive_mutex *m;
    { std::lock_guard lock(g_lock); auto &slot = g_sync[obj]; if (!slot) slot = new std::recursive_mutex; m = slot; }
    m->lock();
    return 0;
}
int objc_sync_exit(id obj) DARWIN(objc_sync_exit);
int objc_sync_exit(id obj) {
    if (!obj) return 0;
    std::recursive_mutex *m = nullptr;
    { std::lock_guard lock(g_lock); auto it = g_sync.find(obj); if (it != g_sync.end()) m = it->second; }
    if (m) m->unlock();
    return 0;
}

/* introspection */
Class objc_getClass(const char *name) DARWIN(objc_getClass);
Class objc_getClass(const char *name) { return mm_objc_class_named(name); }
Class objc_lookUpClass(const char *name) DARWIN(objc_lookUpClass);
Class objc_lookUpClass(const char *name) { return mm_objc_class_named(name); }
Class objc_getRequiredClass(const char *name) DARWIN(objc_getRequiredClass);
Class objc_getRequiredClass(const char *name) {
    Class c = mm_objc_class_named(name);
    if (!c) { std::fprintf(stderr, "objc: required class %s missing\n", name); std::abort(); }
    return c;
}
Class objc_getMetaClass(const char *name) DARWIN(objc_getMetaClass);
Class objc_getMetaClass(const char *name) { Class c = mm_objc_class_named(name); return c ? c->isa : nullptr; }
Class object_getClass(id obj) DARWIN(object_getClass);
Class object_getClass(id obj) { return obj ? obj->isa : nullptr; }
Class object_setClass(id obj, Class cls) DARWIN(object_setClass);
Class object_setClass(id obj, Class cls) { Class old = obj->isa; obj->isa = cls; return old; }
const char *object_getClassName(id obj) DARWIN(object_getClassName);
const char *object_getClassName(id obj) { return obj ? mm_objc_class_name(obj->isa) : "nil"; }
const char *class_getName(Class cls) DARWIN(class_getName);
const char *class_getName(Class cls) { return cls ? mm_objc_class_name(cls) : "nil"; }
Class class_getSuperclass(Class cls) DARWIN(class_getSuperclass);
Class class_getSuperclass(Class cls) { return cls ? realize(cls)->superclass : nullptr; }
bool class_isMetaClass(Class cls) DARWIN(class_isMetaClass);
bool class_isMetaClass(Class cls) { return cls && rw(cls) && rw(cls)->is_meta; }
std::size_t class_getInstanceSize(Class cls) DARWIN(class_getInstanceSize);
std::size_t class_getInstanceSize(Class cls) { return cls ? rw(realize(cls))->instance_size : 0; }
bool class_respondsToSelector(Class cls, SEL sel) DARWIN(class_respondsToSelector);
bool class_respondsToSelector(Class cls, SEL sel) { std::lock_guard lock(g_lock); return cls && lookup_in(cls, sel_intern(sel)); }
void *class_getMethodImplementation(Class cls, SEL sel) DARWIN(class_getMethodImplementation);
void *class_getMethodImplementation(Class cls, SEL sel) {
    std::lock_guard lock(g_lock);
    const method_entry *m = cls ? lookup_in(cls, sel_intern(sel)) : nullptr;
    if (!m && cls) m = resolve_stub(cls, sel_intern(sel));
    return m ? m->imp : nullptr;
}
const method_entry *class_getInstanceMethod(Class cls, SEL sel) DARWIN(class_getInstanceMethod);
const method_entry *class_getInstanceMethod(Class cls, SEL sel) {
    std::lock_guard lock(g_lock);
    if (!cls) return nullptr;
    SEL s = sel_intern(sel);
    const method_entry *found = lookup_in(cls, s);
    if (!found) {
        for (Class c = cls; c; c = c->superclass)
            if (auto it = rw(c)->stubs.find(s); it != rw(c)->stubs.end()) return &it->second->entry;
        return nullptr;
    }
    if (found->stub) return &found->stub->entry;
    for (Class c = cls; c; c = c->superclass)
        for (auto &method : rw(c)->methods) if (method.sel == s) return &method;
    return nullptr;
}
const method_entry *class_getClassMethod(Class cls, SEL sel) DARWIN(class_getClassMethod);
const method_entry *class_getClassMethod(Class cls, SEL sel) { return cls ? class_getInstanceMethod(cls->isa, sel) : nullptr; }
void *method_getImplementation(const method_entry *m) DARWIN(method_getImplementation);
void *method_getImplementation(const method_entry *m) { return m ? m->imp : nullptr; }
SEL method_getName(const method_entry *m) DARWIN(method_getName);
SEL method_getName(const method_entry *m) { return m ? m->sel : nullptr; }
const char *method_getTypeEncoding(const method_entry *m) DARWIN(method_getTypeEncoding);
const char *method_getTypeEncoding(const method_entry *m) { return m ? m->types : nullptr; }
void *method_setImplementation(method_entry *m, void *imp) DARWIN(method_setImplementation);
void *method_setImplementation(method_entry *m, void *imp) {
    std::lock_guard lock(g_lock);
    void *old = m->imp; m->imp = imp; m->guest = is_guest(imp); m->stub = nullptr;
    for (auto &[name, c] : g_classes) { rw(c)->cache.clear(); rw(c->isa)->cache.clear(); }
    return old;
}
void method_exchangeImplementations(method_entry *a, method_entry *b) DARWIN(method_exchangeImplementations);
void method_exchangeImplementations(method_entry *a, method_entry *b) {
    std::lock_guard lock(g_lock);
    std::swap(a->imp, b->imp); std::swap(a->guest, b->guest);
    std::swap(a->stub, b->stub);
    for (auto &[name, c] : g_classes) { rw(c)->cache.clear(); rw(c->isa)->cache.clear(); }
}
bool class_addMethod(Class cls, SEL sel, void *imp, const char *types) DARWIN(class_addMethod);
bool class_addMethod(Class cls, SEL sel, void *imp, const char *types) {
    std::lock_guard lock(g_lock);
    realize(cls);
    class_rw *r = rw(cls);
    SEL s = sel_intern(sel);
    for (const auto &m : r->methods) if (m.sel == s) return false;
    /* A real implementation replaces a lazily cached compatibility method. */
    r->methods.insert(r->methods.begin(), method_entry{s, imp, types, is_guest(imp)});
    invalidate_method_caches(r);
    return true;
}
void *class_replaceMethod(Class cls, SEL sel, void *imp, const char *types) DARWIN(class_replaceMethod);
void *class_replaceMethod(Class cls, SEL sel, void *imp, const char *types) {
    std::lock_guard lock(g_lock);
    realize(cls);
    class_rw *r = rw(cls);
    SEL s = sel_intern(sel);
    for (auto &m : r->methods) if (m.sel == s) { void *old = m.imp; m.imp = imp; m.guest = is_guest(imp); m.stub = nullptr; invalidate_method_caches(r); return old; }
    r->methods.insert(r->methods.begin(), method_entry{s, imp, types, is_guest(imp)});
    invalidate_method_caches(r);
    return nullptr;
}
SEL sel_registerName(const char *name) DARWIN(sel_registerName);
SEL sel_registerName(const char *name) { return sel_intern_copy(name); }
SEL sel_getUid(const char *name) DARWIN(sel_getUid);
SEL sel_getUid(const char *name) { return sel_intern_copy(name); }
const char *sel_getName(SEL sel) DARWIN(sel_getName);
const char *sel_getName(SEL sel) { return sel ? sel : "<null selector>"; }
bool sel_isEqual(SEL a, SEL b) DARWIN(sel_isEqual);
bool sel_isEqual(SEL a, SEL b) { return a == b || (a && b && std::strcmp(a, b) == 0); }
protocol_t *objc_getProtocol(const char *name) DARWIN(objc_getProtocol);
protocol_t *objc_getProtocol(const char *name) { std::lock_guard lock(g_lock); auto it = g_protocols.find(name); return it == g_protocols.end() ? nullptr : it->second; }
const char *protocol_getName(protocol_t *p) DARWIN(protocol_getName);
const char *protocol_getName(protocol_t *p) { return p ? p->name : "nil"; }
bool class_conformsToProtocol(Class cls, protocol_t *p) DARWIN(class_conformsToProtocol);
bool class_conformsToProtocol(Class cls, protocol_t *p) { return cls && NSObject_conformsToProtocol(reinterpret_cast<id>(cls), nullptr, p); }
bool class_addProtocol(Class cls, protocol_t *p) DARWIN(class_addProtocol);
bool class_addProtocol(Class cls, protocol_t *p) { std::lock_guard lock(g_lock); realize(cls); rw(cls)->protocols.push_back(p); return true; }
Class objc_allocateClassPair(Class super, const char *name, std::size_t extra) DARWIN(objc_allocateClassPair);
Class objc_allocateClassPair(Class super, const char *name, std::size_t extra) {
    std::lock_guard lock(g_lock);
    if (g_classes.contains(name)) return nullptr;
    auto *cls = new mm_objc_class;
    auto *meta = new mm_objc_class;
    auto *n = new std::string(name);
    mm_objc_define_class(n->c_str(), super, (super ? rw(realize(super))->instance_size : 0) + extra, cls, meta);
    rw(cls)->host = false;
    return cls;
}
void objc_registerClassPair(Class) DARWIN(objc_registerClassPair);
void objc_registerClassPair(Class) {}
void objc_disposeClassPair(Class cls) DARWIN(objc_disposeClassPair);
void objc_disposeClassPair(Class cls) { std::lock_guard lock(g_lock); g_classes.erase(rw(cls)->name); }
bool class_addIvar(Class cls, const char *, std::size_t size, std::uint8_t alignment, const char *) DARWIN(class_addIvar);
bool class_addIvar(Class cls, const char *, std::size_t size, std::uint8_t alignment, const char *) {
    std::lock_guard lock(g_lock);
    class_rw *r = rw(cls);
    const std::uint32_t align = 1u << alignment;
    r->instance_size = ((r->instance_size + align - 1) & ~(align - 1)) + static_cast<std::uint32_t>(size);
    return true;
}
const ivar_t *class_getInstanceVariable(Class cls, const char *name) DARWIN(class_getInstanceVariable);
const ivar_t *class_getInstanceVariable(Class cls, const char *name) {
    for (Class c = cls; c; c = c->superclass) {
        class_rw *r = rw(realize(c));
        if (!r->ro || !r->ro->ivars) continue;
        const auto *il = static_cast<const ivar_list_t *>(r->ro->ivars);
        const auto *iv = reinterpret_cast<const ivar_t *>(il + 1);
        for (std::uint32_t i = 0; i < il->count; i++) if (iv[i].name && std::strcmp(iv[i].name, name) == 0) return &iv[i];
    }
    return nullptr;
}
std::ptrdiff_t ivar_getOffset(const ivar_t *iv) DARWIN(ivar_getOffset);
std::ptrdiff_t ivar_getOffset(const ivar_t *iv) { return iv && iv->offset ? *iv->offset : 0; }
const char *ivar_getName(const ivar_t *iv) DARWIN(ivar_getName);
const char *ivar_getName(const ivar_t *iv) { return iv ? iv->name : nullptr; }
id object_getIvar(id obj, const ivar_t *iv) DARWIN(object_getIvar);
id object_getIvar(id obj, const ivar_t *iv) { return *reinterpret_cast<id *>(reinterpret_cast<std::uint8_t *>(obj) + ivar_getOffset(iv)); }
void object_setIvar(id obj, const ivar_t *iv, id value) DARWIN(object_setIvar);
void object_setIvar(id obj, const ivar_t *iv, id value) { *reinterpret_cast<id *>(reinterpret_cast<std::uint8_t *>(obj) + ivar_getOffset(iv)) = value; }
id class_createInstance(Class cls, std::size_t extra) DARWIN(class_createInstance);
id class_createInstance(Class cls, std::size_t extra) { return mm_objc_alloc(cls, extra); }
id object_dispose(id obj) DARWIN(object_dispose);
id object_dispose(id obj) { free_instance(obj); return nullptr; }
void objc_enumerationMutation(id obj) DARWIN(objc_enumerationMutation);
void objc_enumerationMutation(id obj) { std::fprintf(stderr, "objc: collection %p mutated during enumeration\n", static_cast<void *>(obj)); std::abort(); }
void objc_exception_throw(id exc) DARWIN(objc_exception_throw);
void objc_exception_throw(id exc) {
    std::fprintf(stderr, "objc: exception thrown: %s (%p); guest exception unwinding is not supported\n", object_getClassName(exc), static_cast<void *>(exc));
    std::abort();
}
void objc_terminate() DARWIN(objc_terminate);
void objc_terminate() { std::fprintf(stderr, "objc: terminate\n"); std::abort(); }
void objc_setProperty_nonatomic(id self, SEL, id value, std::ptrdiff_t offset) DARWIN(objc_setProperty_nonatomic);
void objc_setProperty_nonatomic(id self, SEL, id value, std::ptrdiff_t offset) {
    objc_storeStrong(reinterpret_cast<id *>(reinterpret_cast<std::uint8_t *>(self) + offset), value);
}
void objc_setProperty_nonatomic_copy(id self, SEL, id value, std::ptrdiff_t offset) DARWIN(objc_setProperty_nonatomic_copy);
void objc_setProperty_nonatomic_copy(id self, SEL, id value, std::ptrdiff_t offset) {
    id copy = value ? reinterpret_cast<id>(mm_objc_send1(value, "copyWithZone:", 0)) : nullptr;
    id *slot = reinterpret_cast<id *>(reinterpret_cast<std::uint8_t *>(self) + offset);
    id old = *slot; *slot = copy; objc_release(old);
}
void objc_setProperty_atomic(id self, SEL s, id value, std::ptrdiff_t offset) DARWIN(objc_setProperty_atomic);
void objc_setProperty_atomic(id self, SEL s, id value, std::ptrdiff_t offset) { objc_setProperty_nonatomic(self, s, value, offset); }
void objc_setProperty_atomic_copy(id self, SEL s, id value, std::ptrdiff_t offset) DARWIN(objc_setProperty_atomic_copy);
void objc_setProperty_atomic_copy(id self, SEL s, id value, std::ptrdiff_t offset) { objc_setProperty_nonatomic_copy(self, s, value, offset); }
id objc_getProperty(id self, SEL, std::ptrdiff_t offset, bool) DARWIN(objc_getProperty);
id objc_getProperty(id self, SEL, std::ptrdiff_t offset, bool) { return objc_retainAutorelease(*reinterpret_cast<id *>(reinterpret_cast<std::uint8_t *>(self) + offset)); }
void objc_copyStruct(void *dst, const void *src, std::ptrdiff_t size, bool, bool) DARWIN(objc_copyStruct);
void objc_copyStruct(void *dst, const void *src, std::ptrdiff_t size, bool, bool) { std::memcpy(dst, src, static_cast<std::size_t>(size)); }

/* blocks (libsystem_blocks) */
void *Block_copy_(const void *b) DARWIN(_Block_copy);
void *Block_copy_(const void *b) { return block_copy(const_cast<void *>(b)); }
void Block_release_(const void *b) DARWIN(_Block_release);
void Block_release_(const void *b) { block_release(const_cast<void *>(b)); }
void Block_object_assign_(void *dst, const void *src, int flags) DARWIN(_Block_object_assign);
void Block_object_assign_(void *dst, const void *src, int flags) {
    switch (flags & 0xFF) {
    case 3:  *static_cast<id *>(dst) = objc_retain(static_cast<id>(const_cast<void *>(src))); break;                      /* BLOCK_FIELD_IS_OBJECT */
    case 7:  *static_cast<void **>(dst) = block_copy(const_cast<void *>(src)); break;                                     /* BLOCK_FIELD_IS_BLOCK */
    case 8: case 24: *static_cast<block_byref **>(dst) = byref_copy(static_cast<block_byref *>(const_cast<void *>(src))); break; /* BYREF (+ weak) */
    case 128 | 3: case 128 | 7: case 128 | 8: *static_cast<const void **>(dst) = src; break;                              /* BLOCK_BYREF_CALLER: plain copy */
    default: *static_cast<const void **>(dst) = src; break;
    }
}
void Block_object_dispose_(const void *obj, int flags) DARWIN(_Block_object_dispose);
void Block_object_dispose_(const void *obj, int flags) {
    switch (flags & 0xFF) {
    case 3:  objc_release(static_cast<id>(const_cast<void *>(obj))); break;
    case 7:  block_release(const_cast<void *>(obj)); break;
    case 8: case 24: byref_release(static_cast<block_byref *>(const_cast<void *>(obj))); break;
    default: break;
    }
}
const char *Block_signature_(const void *b) DARWIN(_Block_signature);
const char *Block_signature_(const void *b) {
    const auto *blk = static_cast<const block_layout *>(b);
    return (blk->flags & (1 << 30)) ? blk->descriptor->signature : nullptr;
}

} // extern "C"

/* ---- host API needing the definitions above --------------------------- */

extern "C" {

void mm_objc_free_instance(mm_id obj) { free_instance(obj); }
int mm_objc_is_kind_of(mm_id obj, mm_objc_class *cls) {
    if (!obj) return 0;
    for (Class c = obj->isa; c; c = c->superclass) if (c == cls) return 1;
    return 0;
}
mm_id mm_objc_retain(mm_id obj) { return do_retain(obj); }
void mm_objc_release(mm_id obj) { do_release(obj); }
mm_id mm_objc_autorelease(mm_id obj) { return obj ? reinterpret_cast<mm_id>(NSObject_autorelease(obj, nullptr)) : nullptr; }
mm_SEL mm_objc_sel_of(mm_SEL sel) { return sel_intern(sel); }
const mm_host_api *mm_objc_host_api() { return g_api; }
void mm_objc_return_words_hi(uint64_t x1) {
    if (mm_cpu_context *ctx = g_api ? g_api->current_context() : nullptr) ctx->x[1] = x1;
}
void mm_objc_return_doubles_hi(double v2, double v3) {
    mm_cpu_context *ctx = g_api ? g_api->current_context() : nullptr;
    if (!ctx) return;
    std::memcpy(&ctx->v[2][0], &v2, 8); ctx->v[2][1] = 0;
    std::memcpy(&ctx->v[3][0], &v3, 8); ctx->v[3][1] = 0;
}
uint64_t mm_objc_block_invoke(void *block, int nargs, const uint64_t *args) {
    auto *b = static_cast<block_layout *>(block);
    std::uint64_t a[8] = {reinterpret_cast<std::uintptr_t>(block)};
    for (int i = 0; i < nargs && i < 7; i++) a[1 + i] = args[i];
    if (is_guest(reinterpret_cast<const void *>(b->invoke)))
        return g_api->call_guest(reinterpret_cast<std::uintptr_t>(b->invoke), a, 1 + nargs);
    using fn8 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    return reinterpret_cast<fn8>(reinterpret_cast<void *>(b->invoke))(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

} // extern "C"
