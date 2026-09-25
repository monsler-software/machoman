/* API for host frameworks that implement Objective-C classes on top of
 * machoman's libobjc (libobjc.A.dylib.so). Classes defined here are real
 * objc classes visible to guest code: the guest can subclass them, send
 * them messages and read their metadata through the runtime functions.
 *
 * A host framework exports its classes under the Darwin symbol names so
 * the guest's bind entries resolve to them:
 *
 *   MM_OBJC_CLASS_STORAGE(NSString)                      // defines _OBJC_CLASS_$_NSString / _OBJC_METACLASS_$_NSString
 *   static void init() __attribute__((constructor));
 *   static void init() {
 *       mm_objc_class *c = mm_objc_define_class("NSString", mm_objc_class_named("NSObject"),
 *                                               sizeof(NSStringIvars), MM_OBJC_CLASS_REF(NSString), MM_OBJC_METACLASS_REF(NSString));
 *       mm_objc_add_method(c, "length", (void *)NSString_length, "Q16@0:8");
 *       mm_objc_add_class_method(c, "string", (void *)NSString_string, "@16@0:8");
 *   }
 *
 * Host method implementations have the usual IMP signature
 * `ret (*)(id self, SEL _cmd, args...)` and are called with the guest's
 * argument registers (integer and double arguments only for now). */
#ifndef MACHOMAN_OBJC_HOST_H
#define MACHOMAN_OBJC_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The host dylibs are built with -fvisibility=hidden, so every entry point
 * of this API is marked explicitly. */
#define MM_OBJC_API __attribute__((visibility("default")))

typedef struct mm_objc_object { struct mm_objc_class *isa; } mm_objc_object;
typedef mm_objc_object *mm_id;
typedef const char *mm_SEL;
typedef void (*mm_IMP)(void);

/* Apple's objc_class layout; host classes must use exactly this. */
typedef struct mm_objc_class {
    struct mm_objc_class *isa;
    struct mm_objc_class *superclass;
    void *cache_buckets;
    uintptr_t cache_mask;
    uintptr_t bits;
} mm_objc_class;

#define MM_OBJC_CLASS_STORAGE(name)                                                                     \
    __attribute__((visibility("default"))) mm_objc_class mm_objc_class_storage_##name __asm__("_OBJC_CLASS_$_" #name); \
    __attribute__((visibility("default"))) mm_objc_class mm_objc_metaclass_storage_##name __asm__("_OBJC_METACLASS_$_" #name)
#define MM_OBJC_CLASS_REF(name) (&mm_objc_class_storage_##name)
#define MM_OBJC_METACLASS_REF(name) (&mm_objc_metaclass_storage_##name)

/* Defines a class backed by the given static storage (see the macros).
 * `super` may be NULL for a new root class. Returns `storage`. */
MM_OBJC_API mm_objc_class *mm_objc_define_class(const char *name, mm_objc_class *super, size_t instance_size,
                                                mm_objc_class *storage, mm_objc_class *metaclass_storage);
MM_OBJC_API mm_objc_class *mm_objc_class_named(const char *name);
MM_OBJC_API void mm_objc_add_method(mm_objc_class *cls, const char *selector, void *imp, const char *types);
MM_OBJC_API void mm_objc_add_class_method(mm_objc_class *cls, const char *selector, void *imp, const char *types);
MM_OBJC_API mm_SEL mm_objc_sel(const char *name);

/* Sends a message from host code with up to six integer arguments after
 * _cmd. Dispatches to host or guest implementations as needed. */
MM_OBJC_API uint64_t mm_objc_send(mm_id receiver, mm_SEL sel, int nargs, const uint64_t *args);

/* Convenience wrappers. */
MM_OBJC_API uint64_t mm_objc_send0(mm_id receiver, const char *sel);
MM_OBJC_API uint64_t mm_objc_send1(mm_id receiver, const char *sel, uint64_t a);
MM_OBJC_API uint64_t mm_objc_send2(mm_id receiver, const char *sel, uint64_t a, uint64_t b);

/* Like mm_objc_send but starts the method search at the superclass of
 * `cls`, i.e. what `[super sel]` does inside cls's own implementation. */
MM_OBJC_API uint64_t mm_objc_send_super(mm_id receiver, mm_objc_class *cls, mm_SEL sel, int nargs, const uint64_t *args);

/* Allocation with the runtime's reference counting. */
MM_OBJC_API mm_id mm_objc_alloc(mm_objc_class *cls, size_t extra_bytes);
/* Frees an instance's storage and clears its weak references and
 * associations: what NSObject's -dealloc does. A host class whose
 * -dealloc destroys C++ members calls this last instead of [super dealloc]. */
MM_OBJC_API void mm_objc_free_instance(mm_id obj);
MM_OBJC_API const char *mm_objc_class_name(mm_objc_class *cls);
MM_OBJC_API mm_objc_class *mm_objc_object_class(mm_id obj);
MM_OBJC_API int mm_objc_is_kind_of(mm_id obj, mm_objc_class *cls);
MM_OBJC_API mm_id mm_objc_retain(mm_id obj);
MM_OBJC_API void mm_objc_release(mm_id obj);
MM_OBJC_API mm_id mm_objc_autorelease(mm_id obj);
MM_OBJC_API mm_SEL mm_objc_sel_of(mm_SEL sel); /* interned SEL for a selector string (selectors compare by pointer) */

/* The runtime's host API (thread context, call_guest, ...), for frameworks
 * that need it before their own machoman_dylib_init ran. */
MM_OBJC_API const struct mm_host_api *mm_objc_host_api();

/* Calls a block (guest or host) with up to six integer arguments after the
 * block pointer itself. */
MM_OBJC_API uint64_t mm_objc_block_invoke(void *block, int nargs, const uint64_t *args);

/* Host method implementations receive the guest's registers directly:
 * x0-x7 as the first eight integer parameters and v0-v7 (low halves) as
 * the first eight double parameters, whatever the C signature says. So a
 * method taking an NSRect is written as `(id self, SEL, double x, double y,
 * double w, double h)` and one taking an NSRange as `(id, SEL, uint64_t
 * location, uint64_t length)`. Returns: x0 <- rax, x1 <- rdx, v0 <- xmm0,
 * v1 <- xmm1; an NSRange is returned as a two-word struct, a CGPoint/CGSize
 * as `mm_two_doubles`, and an NSRect as `mm_two_doubles` for x/y after
 * storing w/h with mm_objc_return_doubles_hi. */
typedef struct { double a, b; } mm_two_doubles;
MM_OBJC_API void mm_objc_return_doubles_hi(double v2, double v3);
/* A two-word integer return (NSRange): the method returns the first word
 * and stores the second here. The runtime does not copy the host's rdx
 * into x1, because after most calls that register holds nothing
 * meaningful and the resumed guest code may still need x1. */
MM_OBJC_API void mm_objc_return_words_hi(uint64_t x1);

#ifdef __cplusplus
}
#endif

#endif /* MACHOMAN_OBJC_HOST_H */
