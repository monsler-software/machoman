#define _GNU_SOURCE 1
/* Functions that take a callback. The pointer the guest passes is guest
 * code, which the host cannot call directly, so each of these runs the
 * comparator or handler through the runtime (mm_call_callback). Without
 * this, qsort would jump straight into translated arm64 bytes. */
#include "internal.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <semaphore.h>
#include <fcntl.h>
#include <cerrno>
#include <mutex>
#include <vector>

namespace {

int compare_through(const void *fn, const void *a, const void *b) {
    const std::uint64_t args[2] = {reinterpret_cast<std::uintptr_t>(a), reinterpret_cast<std::uintptr_t>(b)};
    return static_cast<std::int32_t>(mm_call_callback(fn, args, 2));
}
int compare_through_r(const void *fn, void *ctx, const void *a, const void *b) {
    /* Darwin's qsort_r passes the context first. */
    const std::uint64_t args[3] = {reinterpret_cast<std::uintptr_t>(ctx), reinterpret_cast<std::uintptr_t>(a),
                                   reinterpret_cast<std::uintptr_t>(b)};
    return static_cast<std::int32_t>(mm_call_callback(fn, args, 3));
}

/* Sorts indices, then permutes, so each element moves once and the
 * comparator never sees a half-written element. */
template <typename Compare>
void sort_elements(void *base, std::size_t count, std::size_t size, Compare less_than) {
    if (count < 2 || size == 0) return;
    auto *bytes = static_cast<std::uint8_t *>(base);
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        return less_than(bytes + x * size, bytes + y * size);
    });
    std::vector<std::uint8_t> sorted(count * size);
    for (std::size_t i = 0; i < count; i++) std::memcpy(sorted.data() + i * size, bytes + order[i] * size, size);
    std::memcpy(bytes, sorted.data(), sorted.size());
}

} // namespace

extern "C" {

void darwin_qsort(void *base, std::size_t count, std::size_t size, const void *cmp) DARWIN(qsort);
void darwin_qsort(void *base, std::size_t count, std::size_t size, const void *cmp) {
    sort_elements(base, count, size, [&](const void *a, const void *b) { return compare_through(cmp, a, b) < 0; });
}

void darwin_qsort_r(void *base, std::size_t count, std::size_t size, void *ctx, const void *cmp) DARWIN(qsort_r);
void darwin_qsort_r(void *base, std::size_t count, std::size_t size, void *ctx, const void *cmp) {
    sort_elements(base, count, size, [&](const void *a, const void *b) { return compare_through_r(cmp, ctx, a, b) < 0; });
}

/* Darwin's BSD sorts have the same contract as qsort here. */
int darwin_mergesort(void *base, std::size_t count, std::size_t size, const void *cmp) DARWIN(mergesort);
int darwin_mergesort(void *base, std::size_t count, std::size_t size, const void *cmp) {
    darwin_qsort(base, count, size, cmp);
    return 0;
}
int darwin_heapsort(void *base, std::size_t count, std::size_t size, const void *cmp) DARWIN(heapsort);
int darwin_heapsort(void *base, std::size_t count, std::size_t size, const void *cmp) {
    darwin_qsort(base, count, size, cmp);
    return 0;
}
void darwin_psort(void *base, std::size_t count, std::size_t size, const void *cmp) DARWIN(psort);
void darwin_psort(void *base, std::size_t count, std::size_t size, const void *cmp) { darwin_qsort(base, count, size, cmp); }

void *darwin_bsearch(const void *key, const void *base, std::size_t count, std::size_t size, const void *cmp) DARWIN(bsearch);
void *darwin_bsearch(const void *key, const void *base, std::size_t count, std::size_t size, const void *cmp) {
    const auto *bytes = static_cast<const std::uint8_t *>(base);
    std::size_t lo = 0, hi = count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int r = compare_through(cmp, key, bytes + mid * size);
        if (r == 0) return const_cast<std::uint8_t *>(bytes + mid * size);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return nullptr;
}

/* pthread_once's initializer is guest code too. Darwin's pthread_once_t is
 * {long sig; char opaque[8]}; only the signature word matters here. */
struct darwin_once { long sig; char opaque[8]; };
int darwin_pthread_once(darwin_once *once, const void *init) DARWIN(pthread_once);
int darwin_pthread_once(darwin_once *once, const void *init) {
    static std::mutex mu;
    std::lock_guard lock(mu);
    if (!once) return 22; /* EINVAL */
    if (once->opaque[0] == 1) return 0;
    once->opaque[0] = 1;
    mm_call_callback(init, nullptr, 0);
    return 0;
}

} // extern "C"

/* ---- POSIX semaphores ------------------------------------------------------ */

/* Darwin's open flags differ from Linux's, and Darwin has no working
 * sem_init (unnamed semaphores are unimplemented there), so a program only
 * ever gets here through sem_open. The pointer checks matter: on Darwin a
 * sem_t* is a descriptor-like value and passing SEM_FAILED to sem_wait
 * fails with EINVAL, where the host libc would dereference it. */
extern "C" {

sem_t *darwin_sem_open(const char *name, int oflag, ...) DARWIN(sem_open);
sem_t *darwin_sem_open(const char *name, int oflag, ...) {
    int flags = 0;
    if (oflag & 0x0200) flags |= O_CREAT;  /* Darwin O_CREAT */
    if (oflag & 0x0800) flags |= O_EXCL;   /* Darwin O_EXCL */
    mm_guest_va_list args = mm_guest_va_list::at_call_site();
    const unsigned mode = (flags & O_CREAT) ? static_cast<unsigned>(args.next_u64()) : 0;
    const unsigned value = (flags & O_CREAT) ? static_cast<unsigned>(args.next_u64()) : 0;
    /* POSIX wants a name starting with a slash and no others in it. */
    std::string path = name ? name : "";
    if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
    for (std::size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') path[i] = '_';
    return (flags & O_CREAT) ? sem_open(path.c_str(), flags, mode, value) : sem_open(path.c_str(), flags);
}

int darwin_sem_close(sem_t *s) DARWIN(sem_close);
int darwin_sem_close(sem_t *s) {
    if (!s || s == SEM_FAILED) { errno = EINVAL; return -1; }
    return sem_close(s);
}
int darwin_sem_unlink(const char *name) DARWIN(sem_unlink);
int darwin_sem_unlink(const char *name) {
    std::string path = name ? name : "";
    if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
    for (std::size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') path[i] = '_';
    return sem_unlink(path.c_str());
}
int darwin_sem_wait(sem_t *s) DARWIN(sem_wait);
int darwin_sem_wait(sem_t *s) {
    if (!s || s == SEM_FAILED) { errno = EINVAL; return -1; }
    return sem_wait(s);
}
int darwin_sem_trywait(sem_t *s) DARWIN(sem_trywait);
int darwin_sem_trywait(sem_t *s) {
    if (!s || s == SEM_FAILED) { errno = EINVAL; return -1; }
    return sem_trywait(s);
}
int darwin_sem_post(sem_t *s) DARWIN(sem_post);
int darwin_sem_post(sem_t *s) {
    if (!s || s == SEM_FAILED) { errno = EINVAL; return -1; }
    return sem_post(s);
}
/* Unnamed semaphores are unimplemented on Darwin, and a program that
 * checks the return value expects that. */
int darwin_sem_init(sem_t *, int, unsigned) DARWIN(sem_init);
int darwin_sem_init(sem_t *, int, unsigned) { errno = ENOSYS; return -1; }
int darwin_sem_destroy(sem_t *) DARWIN(sem_destroy);
int darwin_sem_destroy(sem_t *) { errno = ENOSYS; return -1; }

} // extern "C"

/* ---- math functions Darwin has and glibc does not --------------------------- */

extern "C" {

/* __exp10/__exp10f are Darwin spellings of 10^x. */
double darwin_exp10(double x) DARWIN(__exp10);
double darwin_exp10(double x) { return exp10(x); }
float darwin_exp10f(float x) DARWIN(__exp10f);
float darwin_exp10f(float x) { return exp10f(x); }

/* The __sincos family returns both results through pointers. */
void darwin_sincos(double x, double *s, double *c) DARWIN(__sincos);
void darwin_sincos(double x, double *s, double *c) { sincos(x, s, c); }
void darwin_sincosf(float x, float *s, float *c) DARWIN(__sincosf);
void darwin_sincosf(float x, float *s, float *c) { sincosf(x, s, c); }

/* ... and the ones that return a pair in registers. */
struct double2 { double sinval, cosval; };
struct float2 { float sinval, cosval; };
double2 darwin_sincos_stret(double x) DARWIN(__sincos_stret);
double2 darwin_sincos_stret(double x) {
    double2 r;
    sincos(x, &r.sinval, &r.cosval);
    return r;
}
float2 darwin_sincosf_stret(float x) DARWIN(__sincosf_stret);
float2 darwin_sincosf_stret(float x) {
    float2 r;
    sincosf(x, &r.sinval, &r.cosval);
    return r;
}

} // extern "C"
