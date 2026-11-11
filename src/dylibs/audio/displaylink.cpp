/* CVDisplayLink: the clock a program draws to.
 *
 * On macOS it is the display's own refresh signal, delivered on a
 * high-priority thread. Here it is a thread ticking at the refresh rate
 * the backend reports, because what a program needs from it is a
 * steady callback it can render on — without one it builds its window and
 * then waits forever for a frame that never comes, which is exactly where
 * Ableton Live stopped.
 *
 * The callback is guest code, so it goes through mm_call_callback rather
 * than being called directly. */
#include "machoman/objc_host.h"
#include "machoman/objc_framework.h"
#include "machoman/runtime.h"
#include "synthesise.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

namespace {

double refresh_hz() {
    if (const char *value = std::getenv("MACHOMAN_DISPLAY_HZ")) {
        char *end = nullptr; const double hz = std::strtod(value, &end);
        if (end != value && !*end && std::isfinite(hz) && hz >= 1 && hz <= 1000) return hz;
    }
    using Hook = double (*)();
    const auto hook = reinterpret_cast<Hook>(dlsym(RTLD_DEFAULT, "machoman_appkit_display_refresh_hz"));
    const double hz = hook ? hook() : 60.0;
    return std::isfinite(hz) && hz >= 1 && hz <= 1000 ? hz : 60.0;
}

/* CVTimeStamp, as CoreVideo lays it out; a callback reads the host time
 * and the video time from it. */
struct cv_time_stamp {
    std::uint32_t version;
    std::int32_t video_time_scale;
    std::int64_t video_time;
    std::uint64_t host_time;
    double rate_scalar;
    std::int64_t video_refresh_period;
    struct {
        std::int16_t subframes, subframe_divisor;
        std::uint32_t counter, type, flags;
        std::int16_t hours, minutes, seconds, frames;
    } smpte_time;
    std::uint64_t flags;
    std::uint64_t reserved;
};
static_assert(sizeof(cv_time_stamp) == 80);
static_assert(offsetof(cv_time_stamp, flags) == 64);

struct display_link {
    std::uint64_t callback = 0, user_info = 0;
    std::atomic<bool> running{false};
    std::thread thread;
    std::int64_t frame = 0;
    double hz = 60.0;
    ~display_link() {
        running = false;
        if (thread.joinable()) thread.join();
    }
};

std::mutex &links_mutex() { static std::mutex *m = new std::mutex; return *m; }
std::vector<std::unique_ptr<display_link>> &links() {
    static auto *v = new std::vector<std::unique_ptr<display_link>>;
    return *v;
}

const mm_host_api *api() { return mm_objc_host_api(); }

/* The callback is guest code: it has to go through the runtime rather
 * than be called as a host function. */
void call_callback(std::uint64_t fn, const std::uint64_t *args, int n) {
    const mm_host_api *a = api();
    if (!a || !fn) return;
    if (a->is_guest_code(fn)) a->call_guest(fn, args, n);
    else reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t)>(
             static_cast<std::uintptr_t>(fn))(args[0], args[1], args[2], args[3], args[4], args[5]);
}

std::uint64_t now_nanoseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void tick(display_link *link) {
    const auto period = std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / link->hz));
    auto next = std::chrono::steady_clock::now();
    while (link->running) {
        next += period;
        const auto clock_now = std::chrono::steady_clock::now();
        if (clock_now > next + period) {
            const auto missed = (clock_now - next) / period;
            next += (missed + 1) * period;
            link->frame += missed + 1;
        }
        std::this_thread::sleep_until(next);
        if (!link->running || !link->callback) continue;

        cv_time_stamp now{}, output{};
        now.version = 0;
        now.video_time_scale = static_cast<std::int32_t>(std::lround(link->hz * 1000));
        now.video_time = link->frame * 1000;
        now.host_time = now_nanoseconds();
        now.rate_scalar = 1.0;
        now.video_refresh_period = 1000;
        now.flags = 3 | 8 | 16; // Video/host times, refresh period and rate scalar.
        output = now;
        output.video_time += 1000;               /* the next frame */
        link->frame++;

        /* (link, inNow, inOutputTime, flagsIn, flagsOut, userInfo) */
        std::uint64_t flags_out = 0;
        const std::uint64_t args[6] = {reinterpret_cast<std::uintptr_t>(link),
                                       reinterpret_cast<std::uintptr_t>(&now),
                                       reinterpret_cast<std::uintptr_t>(&output),
                                       0,
                                       reinterpret_cast<std::uintptr_t>(&flags_out),
                                       link->user_info};
        mmfw::autorelease_pool pool;
        call_callback(link->callback, args, 6);
    }
}

} // namespace

extern "C" __attribute__((visibility("default"))) void machoman_dylib_quiesce() {
    std::vector<display_link *> snapshot;
    { std::lock_guard lock(links_mutex()); for (auto &link : links()) snapshot.push_back(link.get()); }
    for (auto *link : snapshot) link->running = false;
    for (auto *link : snapshot) if (link->thread.joinable()) link->thread.join();
}

extern "C" {

std::int32_t darwin_CVDisplayLinkCreateWithActiveCGDisplays(display_link **out) DARWIN(CVDisplayLinkCreateWithActiveCGDisplays);
std::int32_t darwin_CVDisplayLinkCreateWithActiveCGDisplays(display_link **out) {
    if (!out) return -1;
    auto link = std::make_unique<display_link>();
    *out = link.get();
    std::lock_guard lock(links_mutex());
    links().push_back(std::move(link));
    return 0;
}

std::int32_t darwin_CVDisplayLinkCreateWithCGDisplay(std::uint32_t, display_link **out) DARWIN(CVDisplayLinkCreateWithCGDisplay);
std::int32_t darwin_CVDisplayLinkCreateWithCGDisplay(std::uint32_t, display_link **out) {
    return darwin_CVDisplayLinkCreateWithActiveCGDisplays(out);
}

std::int32_t darwin_CVDisplayLinkSetOutputCallback(display_link *link, void *callback, void *user_info) DARWIN(CVDisplayLinkSetOutputCallback);
std::int32_t darwin_CVDisplayLinkSetOutputCallback(display_link *link, void *callback, void *user_info) {
    if (!link) return -1;
    link->callback = reinterpret_cast<std::uintptr_t>(callback);
    link->user_info = reinterpret_cast<std::uintptr_t>(user_info);
    return 0;
}

std::int32_t darwin_CVDisplayLinkSetCurrentCGDisplay(display_link *, std::uint32_t) DARWIN(CVDisplayLinkSetCurrentCGDisplay);
std::int32_t darwin_CVDisplayLinkSetCurrentCGDisplay(display_link *, std::uint32_t) { return 0; }

std::int32_t darwin_CVDisplayLinkStart(display_link *link) DARWIN(CVDisplayLinkStart);
std::int32_t darwin_CVDisplayLinkStart(display_link *link) {
    if (!link || link->running) return link ? 0 : -1;
    link->running = true;
    link->hz = refresh_hz();
    if (std::getenv("MACHOMAN_APPKIT_TRACE") || std::getenv("MACHOMAN_TRACE"))
        std::fprintf(stderr, "CoreVideo: display link started at %g Hz\n", link->hz);
    link->thread = std::thread([link] { tick(link); });
    return 0;
}

std::int32_t darwin_CVDisplayLinkStop(display_link *link) DARWIN(CVDisplayLinkStop);
std::int32_t darwin_CVDisplayLinkStop(display_link *link) {
    if (!link) return -1;
    link->running = false;
    if (link->thread.joinable()) link->thread.join();
    return 0;
}

int darwin_CVDisplayLinkIsRunning(display_link *link) DARWIN(CVDisplayLinkIsRunning);
int darwin_CVDisplayLinkIsRunning(display_link *link) { return link && link->running ? 1 : 0; }

display_link *darwin_CVDisplayLinkRetain(display_link *link) DARWIN(CVDisplayLinkRetain);
display_link *darwin_CVDisplayLinkRetain(display_link *link) { return link; }

void darwin_CVDisplayLinkRelease(display_link *link) DARWIN(CVDisplayLinkRelease);
void darwin_CVDisplayLinkRelease(display_link *link) {
    /* The link is owned by the list above; a release only stops it, since
     * a program may still hold the pointer. */
    if (link) darwin_CVDisplayLinkStop(link);
}

double darwin_CVDisplayLinkGetActualOutputVideoRefreshPeriod(display_link *) DARWIN(CVDisplayLinkGetActualOutputVideoRefreshPeriod);
double darwin_CVDisplayLinkGetActualOutputVideoRefreshPeriod(display_link *link) { return 1.0 / (link && link->running ? link->hz : refresh_hz()); }

std::uint64_t darwin_CVDisplayLinkGetNominalOutputVideoRefreshPeriod(display_link *) DARWIN(CVDisplayLinkGetNominalOutputVideoRefreshPeriod);
std::uint64_t darwin_CVDisplayLinkGetNominalOutputVideoRefreshPeriod(display_link *link) {
    // CVTime is returned in x0/x1 (int64 value, int32 scale, int32 flags).
    mm_objc_return_words_hi(static_cast<std::uint32_t>(std::lround((link && link->running ? link->hz : refresh_hz()) * 1000)));
    return 1000;
}

std::uint64_t darwin_CVGetCurrentHostTime() DARWIN(CVGetCurrentHostTime);
std::uint64_t darwin_CVGetCurrentHostTime() { return now_nanoseconds(); }

double darwin_CVGetHostClockFrequency() DARWIN(CVGetHostClockFrequency);
double darwin_CVGetHostClockFrequency() { return 1e9; }

} // extern "C"

/* CoreVideo's other symbols — pixel buffers and the like — are not
 * implemented; they answer through the shared synthesiser. */
extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}
