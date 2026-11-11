/* Malloc zones.
 *
 * Darwin's allocator is addressable: a program can ask for the default
 * zone, create one of its own, and allocate from a named zone so that
 * `leaks` and Instruments can attribute the memory. Everything a zone
 * does here goes to the host allocator — one heap, as on a system with a
 * single malloc — but the zone objects are real, because a program both
 * passes them back to `malloc_zone_*` and calls their function pointers
 * directly (the struct layout is part of the ABI, and a zone's `malloc`
 * slot is reached through it).
 *
 * The zone's own function pointers are host code: a guest calling one
 * goes through the same host-call path as a call to malloc itself. */
#include "internal.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

/* The zone's function pointers are handed to the guest, which calls them
 * directly. The runtime recognises a branch into host code by looking the
 * address up with dladdr, so these must be exported symbols rather than
 * hidden ones — a hidden function has no dynamic symbol and the branch
 * lands nowhere. */
#define ZONE_FN __attribute__((visibility("default")))

namespace {

struct malloc_zone_t;

/* malloc_zone_t as libmalloc lays it out (version 12). Only the fields a
 * program reads matter, but the offsets of all of them do. */
struct malloc_zone_t {
    void *reserved1 = nullptr;
    void *reserved2 = nullptr;
    std::size_t (*size)(malloc_zone_t *, const void *) = nullptr;
    void *(*malloc_fn)(malloc_zone_t *, std::size_t) = nullptr;
    void *(*calloc_fn)(malloc_zone_t *, std::size_t, std::size_t) = nullptr;
    void *(*valloc_fn)(malloc_zone_t *, std::size_t) = nullptr;
    void (*free_fn)(malloc_zone_t *, void *) = nullptr;
    void *(*realloc_fn)(malloc_zone_t *, void *, std::size_t) = nullptr;
    void (*destroy)(malloc_zone_t *) = nullptr;
    const char *zone_name = nullptr;
    unsigned (*batch_malloc)(malloc_zone_t *, std::size_t, void **, unsigned) = nullptr;
    void (*batch_free)(malloc_zone_t *, void **, unsigned) = nullptr;
    void *introspect = nullptr;
    unsigned version = 12;
    void *(*memalign)(malloc_zone_t *, std::size_t, std::size_t) = nullptr;
    void (*free_definite_size)(malloc_zone_t *, void *, std::size_t) = nullptr;
    std::size_t (*pressure_relief)(malloc_zone_t *, std::size_t) = nullptr;
    int (*claimed_address)(malloc_zone_t *, void *) = nullptr;
};

} // namespace

extern "C" {

ZONE_FN std::size_t mm_zone_size(malloc_zone_t *, const void *ptr);
ZONE_FN void *mm_zone_malloc(malloc_zone_t *, std::size_t size);
ZONE_FN void *mm_zone_calloc(malloc_zone_t *, std::size_t count, std::size_t size);
ZONE_FN void *mm_zone_valloc(malloc_zone_t *, std::size_t size);
ZONE_FN void mm_zone_free(malloc_zone_t *, void *ptr);
ZONE_FN void *mm_zone_realloc(malloc_zone_t *, void *ptr, std::size_t size);
ZONE_FN void mm_zone_destroy(malloc_zone_t *);
ZONE_FN void *mm_zone_memalign(malloc_zone_t *, std::size_t alignment, std::size_t size);
ZONE_FN void mm_zone_free_definite_size(malloc_zone_t *, void *ptr, std::size_t);
ZONE_FN std::size_t mm_zone_pressure_relief(malloc_zone_t *, std::size_t);
ZONE_FN int mm_zone_claimed_address(malloc_zone_t *, void *);
ZONE_FN unsigned mm_zone_batch_malloc(malloc_zone_t *, std::size_t size, void **results, unsigned count);
ZONE_FN void mm_zone_batch_free(malloc_zone_t *, void **to_free, unsigned count);

std::size_t mm_zone_size(malloc_zone_t *, const void *ptr) {
    /* A zone reports 0 for a pointer it did not allocate; with one heap
     * every pointer belongs to it, and the host's usable size is what
     * malloc_size means. */
    return ptr ? malloc_usable_size(const_cast<void *>(ptr)) : 0;
}
void *mm_zone_malloc(malloc_zone_t *, std::size_t size) { return std::malloc(size); }
void *mm_zone_calloc(malloc_zone_t *, std::size_t count, std::size_t size) { return std::calloc(count, size); }
void *mm_zone_valloc(malloc_zone_t *, std::size_t size) {
    void *p = nullptr;
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return posix_memalign(&p, page, size) == 0 ? p : nullptr;
}
void mm_zone_free(malloc_zone_t *, void *ptr) { std::free(ptr); }
void *mm_zone_realloc(malloc_zone_t *, void *ptr, std::size_t size) { return std::realloc(ptr, size); }
void mm_zone_destroy(malloc_zone_t *) {}
void *mm_zone_memalign(malloc_zone_t *, std::size_t alignment, std::size_t size) {
    void *p = nullptr;
    return posix_memalign(&p, alignment < sizeof(void *) ? sizeof(void *) : alignment, size) == 0 ? p : nullptr;
}
void mm_zone_free_definite_size(malloc_zone_t *, void *ptr, std::size_t) { std::free(ptr); }
std::size_t mm_zone_pressure_relief(malloc_zone_t *, std::size_t) { return 0; }
int mm_zone_claimed_address(malloc_zone_t *, void *) { return 1; }
unsigned mm_zone_batch_malloc(malloc_zone_t *, std::size_t size, void **results, unsigned count) {
    unsigned made = 0;
    for (; made < count; made++) {
        results[made] = std::malloc(size);
        if (!results[made]) break;
    }
    return made;
}
void mm_zone_batch_free(malloc_zone_t *, void **to_free, unsigned count) {
    for (unsigned i = 0; i < count; i++) std::free(to_free[i]);
}

} // extern "C"

namespace {

void fill(malloc_zone_t &z, const char *name) {
    z.size = mm_zone_size;
    z.malloc_fn = mm_zone_malloc;
    z.calloc_fn = mm_zone_calloc;
    z.valloc_fn = mm_zone_valloc;
    z.free_fn = mm_zone_free;
    z.realloc_fn = mm_zone_realloc;
    z.destroy = mm_zone_destroy;
    z.zone_name = name;
    z.batch_malloc = mm_zone_batch_malloc;
    z.batch_free = mm_zone_batch_free;
    z.version = 12;
    z.memalign = mm_zone_memalign;
    z.free_definite_size = mm_zone_free_definite_size;
    z.pressure_relief = mm_zone_pressure_relief;
    z.claimed_address = mm_zone_claimed_address;
}

malloc_zone_t &default_zone() {
    static malloc_zone_t z = [] {
        malloc_zone_t zone;
        fill(zone, "DefaultMallocZone");
        return zone;
    }();
    return z;
}

/* Names a zone owns, because -setZoneName copies the string. */
std::vector<std::string> &zone_names() {
    static std::vector<std::string> *names = new std::vector<std::string>;
    return *names;
}

} // namespace

extern "C" {

malloc_zone_t *darwin_malloc_default_zone() DARWIN(malloc_default_zone);
malloc_zone_t *darwin_malloc_default_zone() { return &default_zone(); }

/* There is no separate purgeable heap; the default zone answers for it. */
malloc_zone_t *darwin_malloc_default_purgeable_zone() DARWIN(malloc_default_purgeable_zone);
malloc_zone_t *darwin_malloc_default_purgeable_zone() { return &default_zone(); }

malloc_zone_t *darwin_malloc_create_zone(std::size_t, unsigned) DARWIN(malloc_create_zone);
malloc_zone_t *darwin_malloc_create_zone(std::size_t, unsigned) {
    auto *z = new malloc_zone_t;
    fill(*z, "MallocZone");
    return z;
}

void darwin_malloc_destroy_zone(malloc_zone_t *zone) DARWIN(malloc_destroy_zone);
void darwin_malloc_destroy_zone(malloc_zone_t *zone) {
    /* Darwin frees everything the zone allocated; with one heap there is
     * nothing to sweep, so only the zone itself goes. */
    if (zone && zone != &default_zone()) delete zone;
}

void darwin_malloc_set_zone_name(malloc_zone_t *zone, const char *name) DARWIN(malloc_set_zone_name);
void darwin_malloc_set_zone_name(malloc_zone_t *zone, const char *name) {
    if (!zone || !name) return;
    zone_names().emplace_back(name);
    zone->zone_name = zone_names().back().c_str();
}

const char *darwin_malloc_get_zone_name(malloc_zone_t *zone) DARWIN(malloc_get_zone_name);
const char *darwin_malloc_get_zone_name(malloc_zone_t *zone) { return zone ? zone->zone_name : nullptr; }

void *darwin_malloc_zone_malloc(malloc_zone_t *, std::size_t size) DARWIN(malloc_zone_malloc);
void *darwin_malloc_zone_malloc(malloc_zone_t *, std::size_t size) { return std::malloc(size); }

void *darwin_malloc_zone_calloc(malloc_zone_t *, std::size_t count, std::size_t size) DARWIN(malloc_zone_calloc);
void *darwin_malloc_zone_calloc(malloc_zone_t *, std::size_t count, std::size_t size) { return std::calloc(count, size); }

void *darwin_malloc_zone_valloc(malloc_zone_t *zone, std::size_t size) DARWIN(malloc_zone_valloc);
void *darwin_malloc_zone_valloc(malloc_zone_t *zone, std::size_t size) { return mm_zone_valloc(zone, size); }

void *darwin_malloc_zone_realloc(malloc_zone_t *, void *ptr, std::size_t size) DARWIN(malloc_zone_realloc);
void *darwin_malloc_zone_realloc(malloc_zone_t *, void *ptr, std::size_t size) { return std::realloc(ptr, size); }

void *darwin_malloc_zone_memalign(malloc_zone_t *zone, std::size_t alignment, std::size_t size) DARWIN(malloc_zone_memalign);
void *darwin_malloc_zone_memalign(malloc_zone_t *zone, std::size_t alignment, std::size_t size) {
    return mm_zone_memalign(zone, alignment, size);
}

void darwin_malloc_zone_free(malloc_zone_t *, void *ptr) DARWIN(malloc_zone_free);
void darwin_malloc_zone_free(malloc_zone_t *, void *ptr) { std::free(ptr); }

/* Which zone a pointer came from: there is only one. */
malloc_zone_t *darwin_malloc_zone_from_ptr(const void *ptr) DARWIN(malloc_zone_from_ptr);
malloc_zone_t *darwin_malloc_zone_from_ptr(const void *ptr) { return ptr ? &default_zone() : nullptr; }

std::size_t darwin_malloc_size(const void *ptr) DARWIN(malloc_size);
std::size_t darwin_malloc_size(const void *ptr) { return ptr ? malloc_usable_size(const_cast<void *>(ptr)) : 0; }

std::size_t darwin_malloc_good_size(std::size_t size) DARWIN(malloc_good_size);
std::size_t darwin_malloc_good_size(std::size_t size) { return (size + 15) & ~std::size_t{15}; }

void darwin_malloc_zone_register(malloc_zone_t *) DARWIN(malloc_zone_register);
void darwin_malloc_zone_register(malloc_zone_t *) {}
void darwin_malloc_zone_unregister(malloc_zone_t *) DARWIN(malloc_zone_unregister);
void darwin_malloc_zone_unregister(malloc_zone_t *) {}

/* The zone enumeration `leaks` uses: this process has the one zone. */
int darwin_malloc_get_all_zones(unsigned, void *, std::uintptr_t **addresses, unsigned *count) DARWIN(malloc_get_all_zones);
int darwin_malloc_get_all_zones(unsigned, void *, std::uintptr_t **addresses, unsigned *count) {
    static std::uintptr_t one = reinterpret_cast<std::uintptr_t>(&default_zone());
    if (addresses) *addresses = &one;
    if (count) *count = 1;
    return 0;
}

} // extern "C"
