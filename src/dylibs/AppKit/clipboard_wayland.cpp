#include "clipboard_wayland.h"
#include <wayland-client.h>
#ifdef MM_APPKIT_DATA_CONTROL
#include "ext-data-control-v1-client-protocol.h"
#endif
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace ak {
bool dispatch_wayland(wl_display *display, int timeout_ms, int transfer_fd) {
    if (!display || wl_display_get_error(display)) return false;
    while (wl_display_prepare_read(display) != 0)
        if (wl_display_dispatch_pending(display) < 0) return false;
    const int flushed = wl_display_flush(display);
    if (flushed < 0 && errno != EAGAIN) { wl_display_cancel_read(display); return false; }
    pollfd fds[] = {{wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0},
                    {transfer_fd, POLLIN, 0}};
    int ready;
    do { ready = poll(fds, transfer_fd >= 0 ? 2 : 1, timeout_ms); } while (ready < 0 && errno == EINTR);
    if (ready > 0 && (fds[0].revents & POLLIN)) {
        if (wl_display_read_events(display) < 0) return false;
    } else wl_display_cancel_read(display);
    if (ready < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
    if (fds[0].revents & POLLOUT) wl_display_flush(display);
    return wl_display_dispatch_pending(display) >= 0;
}

namespace {
// Never block the display callback on a full pipe. A reader can be another
// application, a clipboard manager, or this same display connection.
struct transfer_job {
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::jthread worker;
};
std::unique_ptr<transfer_job> transfer(int fd, std::string bytes) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    try {
        auto job = std::make_unique<transfer_job>();
        job->worker = std::jthread([fd, bytes = std::move(bytes), done = job->done](std::stop_token stop) {
            sigset_t blocked;
            sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
            std::size_t offset = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (offset < bytes.size() && !stop.stop_requested()) {
                const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
                if (n > 0) { offset += static_cast<std::size_t>(n); continue; }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EAGAIN && std::chrono::steady_clock::now() < deadline) {
                    pollfd wait{fd, POLLOUT, 0};
                    if (poll(&wait, 1, 50) >= 0 && !(wait.revents & (POLLERR | POLLHUP))) continue;
                }
                break;
            }
            close(fd);
            *done = true;
        });
        return job;
    } catch (...) { close(fd); return nullptr; }
}
}

struct wayland_clipboard::impl {
    wl_display *display = nullptr;
    wl_data_device_manager *manager = nullptr;
    wl_data_device *device = nullptr;
    wl_data_source *source = nullptr;
    wl_data_offer *selection = nullptr, *drag = nullptr;
    std::unordered_map<wl_data_offer *, std::vector<std::string>> offers;
#ifdef MM_APPKIT_DATA_CONTROL
    ext_data_control_manager_v1 *control_manager = nullptr;
    ext_data_control_device_v1 *control_device = nullptr;
    ext_data_control_source_v1 *control_source = nullptr;
    ext_data_control_offer_v1 *control_selection = nullptr;
    std::unordered_map<ext_data_control_offer_v1 *, std::vector<std::string>> control_offers;
#endif
    clipboard_data data;
    std::vector<std::unique_ptr<transfer_job>> transfers;
    std::uint64_t generation = 0;
    std::uint32_t serial = 0;
    bool local = false, pending = false;

    void forget(wl_data_offer *offer) {
        if (offer) { offers.erase(offer); wl_data_offer_destroy(offer); }
    }
    void destroy_source() {
        if (source) { wl_data_source_destroy(source); source = nullptr; }
#ifdef MM_APPKIT_DATA_CONTROL
        if (control_source) { ext_data_control_source_v1_destroy(control_source); control_source = nullptr; }
#endif
    }
    void send(const char *mime, int fd) {
        std::erase_if(transfers, [](const auto &job) { return job->done->load(); });
        auto it = data.find(mime);
        if (it == data.end()) close(fd);
        else if (auto job = transfer(fd, it->second)) transfers.push_back(std::move(job));
    }
    void cancelled() { local = false; data.clear(); ++generation; }

    static void offer(void *p, wl_data_offer *o, const char *mime) { static_cast<impl *>(p)->offers[o].emplace_back(mime); }
    static void offer_action(void *, wl_data_offer *, std::uint32_t) {}
    static constexpr wl_data_offer_listener offer_listener = {offer, offer_action, offer_action};
    static void new_offer(void *p, wl_data_device *, wl_data_offer *o) {
        auto *s = static_cast<impl *>(p); s->offers.emplace(o, std::vector<std::string>{});
        wl_data_offer_add_listener(o, &offer_listener, p);
    }
    static void enter(void *p, wl_data_device *, std::uint32_t serial, wl_surface *, wl_fixed_t, wl_fixed_t, wl_data_offer *o) {
        auto *s = static_cast<impl *>(p); s->forget(s->drag); s->drag = o;
        if (o) wl_data_offer_accept(o, serial, nullptr); // No drop destination yet.
    }
    static void leave(void *p, wl_data_device *) { auto *s = static_cast<impl *>(p); s->forget(s->drag); s->drag = nullptr; }
    static void motion(void *, wl_data_device *, std::uint32_t, wl_fixed_t, wl_fixed_t) {}
    static void drop(void *p, wl_data_device *d) { leave(p, d); }
    static void selected(void *p, wl_data_device *, wl_data_offer *o) {
        auto *s = static_cast<impl *>(p); s->forget(s->selection); s->selection = o;
#ifdef MM_APPKIT_DATA_CONTROL
        if (s->control_device) return;
#endif
        if (!s->source && !s->pending) { s->local = false; s->data.clear(); ++s->generation; }
    }
    static constexpr wl_data_device_listener device_listener = {new_offer, enter, leave, motion, drop, selected};
    static void target(void *, wl_data_source *, const char *) {}
    static void source_send(void *p, wl_data_source *, const char *mime, int fd) { static_cast<impl *>(p)->send(mime, fd); }
    static void source_cancelled(void *p, wl_data_source *source) {
        auto *s = static_cast<impl *>(p); wl_data_source_destroy(source);
        if (s->source == source) { s->source = nullptr; s->cancelled(); }
    }
    static void source_done(void *, wl_data_source *) {}
    static void source_action(void *, wl_data_source *, std::uint32_t) {}
    static constexpr wl_data_source_listener source_listener = {target, source_send, source_cancelled, source_done, source_done, source_action};

#ifdef MM_APPKIT_DATA_CONTROL
    void forget(ext_data_control_offer_v1 *offer) {
        if (offer) { control_offers.erase(offer); ext_data_control_offer_v1_destroy(offer); }
    }
    static void control_offer(void *p, ext_data_control_offer_v1 *o, const char *mime) {
        static_cast<impl *>(p)->control_offers[o].emplace_back(mime);
    }
    static constexpr ext_data_control_offer_v1_listener control_offer_listener = {control_offer};
    static void control_new_offer(void *p, ext_data_control_device_v1 *, ext_data_control_offer_v1 *o) {
        auto *s = static_cast<impl *>(p); s->control_offers.emplace(o, std::vector<std::string>{});
        ext_data_control_offer_v1_add_listener(o, &control_offer_listener, p);
    }
    static void control_selected(void *p, ext_data_control_device_v1 *, ext_data_control_offer_v1 *o) {
        auto *s = static_cast<impl *>(p); s->forget(s->control_selection); s->control_selection = o;
        if (!s->control_source) { s->local = false; s->data.clear(); ++s->generation; }
    }
    static void control_finished(void *p, ext_data_control_device_v1 *device) {
        auto *s = static_cast<impl *>(p); ext_data_control_device_v1_destroy(device); s->control_device = nullptr;
        s->forget(s->control_selection); s->control_selection = nullptr;
        s->destroy_source(); s->cancelled();
    }
    static void primary(void *p, ext_data_control_device_v1 *, ext_data_control_offer_v1 *o) { static_cast<impl *>(p)->forget(o); }
    static constexpr ext_data_control_device_v1_listener control_device_listener = {control_new_offer, control_selected, control_finished, primary};
    static void control_send(void *p, ext_data_control_source_v1 *, const char *mime, int fd) { static_cast<impl *>(p)->send(mime, fd); }
    static void control_cancelled(void *p, ext_data_control_source_v1 *source) {
        auto *s = static_cast<impl *>(p); ext_data_control_source_v1_destroy(source);
        if (s->control_source == source) { s->control_source = nullptr; s->cancelled(); }
    }
    static constexpr ext_data_control_source_v1_listener control_source_listener = {control_send, control_cancelled};
#endif

    void publish() {
#ifdef MM_APPKIT_DATA_CONTROL
        if (control_device) {
            destroy_source();
            if (!data.empty()) {
                control_source = ext_data_control_manager_v1_create_data_source(control_manager);
                ext_data_control_source_v1_add_listener(control_source, &control_source_listener, this);
                for (const auto &[mime, bytes] : data) ext_data_control_source_v1_offer(control_source, mime.c_str());
            }
            ext_data_control_device_v1_set_selection(control_device, control_source);
            pending = false; wl_display_flush(display); return;
        }
#endif
        if (!device || !serial) return; // Publish on the next focused input serial.
        destroy_source();
        if (!data.empty()) {
            source = wl_data_device_manager_create_data_source(manager);
            wl_data_source_add_listener(source, &source_listener, this);
            for (const auto &[mime, bytes] : data) wl_data_source_offer(source, mime.c_str());
        }
        wl_data_device_set_selection(device, source, serial);
        pending = false; wl_display_flush(display);
    }
    ~impl() {
        destroy_source();
        for (auto &[o, types] : offers) wl_data_offer_destroy(o);
        if (device) {
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(device)) >= 2) wl_data_device_release(device);
            else wl_data_device_destroy(device);
        }
        if (manager) wl_data_device_manager_destroy(manager);
#ifdef MM_APPKIT_DATA_CONTROL
        for (auto &[o, types] : control_offers) ext_data_control_offer_v1_destroy(o);
        if (control_device) ext_data_control_device_v1_destroy(control_device);
        if (control_manager) ext_data_control_manager_v1_destroy(control_manager);
#endif
    }
};

wayland_clipboard::wayland_clipboard() : p(std::make_unique<impl>()) {}
wayland_clipboard::~wayland_clipboard() = default;
void wayland_clipboard::global(wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version) {
    if (!p->manager && std::strcmp(interface, wl_data_device_manager_interface.name) == 0)
        p->manager = static_cast<wl_data_device_manager *>(wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3u)));
#ifdef MM_APPKIT_DATA_CONTROL
    if (!p->control_manager && std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
        p->control_manager = static_cast<ext_data_control_manager_v1 *>(wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
#endif
}
void wayland_clipboard::setup(wl_display *display, wl_seat *seat) {
    p->display = display;
    if (p->manager && seat && !p->device) {
        p->device = wl_data_device_manager_get_data_device(p->manager, seat);
        wl_data_device_add_listener(p->device, &impl::device_listener, p.get());
    }
#ifdef MM_APPKIT_DATA_CONTROL
    if (p->control_manager && seat && !p->control_device) {
        p->control_device = ext_data_control_manager_v1_get_data_device(p->control_manager, seat);
        ext_data_control_device_v1_add_listener(p->control_device, &impl::control_device_listener, p.get());
    }
#endif
}
void wayland_clipboard::input(std::uint32_t serial) { p->serial = serial; if (p->pending) p->publish(); }
void wayland_clipboard::unfocus() { p->serial = 0; }
void wayland_clipboard::write(const clipboard_data &data) {
    p->destroy_source(); p->data = data; p->local = true; p->pending = true; ++p->generation;
    p->publish();
}
clipboard_snapshot wayland_clipboard::snapshot() const {
    clipboard_snapshot result{p->generation, {}};
    if (p->local) { for (auto &[mime, bytes] : p->data) result.types.push_back(mime); return result; }
#ifdef MM_APPKIT_DATA_CONTROL
    if (p->control_device) {
        auto it = p->control_offers.find(p->control_selection);
        if (it != p->control_offers.end()) result.types = it->second;
        return result;
    }
#endif
    auto it = p->offers.find(p->selection);
    if (it != p->offers.end()) result.types = it->second;
    return result;
}
bool wayland_clipboard::read(const std::string &type, std::string &bytes) {
    if (p->local) {
        auto it = p->data.find(type); if (it == p->data.end()) return false;
        bytes = it->second; return true;
    }
    const auto info = snapshot();
    if (std::find(info.types.begin(), info.types.end(), type) == info.types.end()) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) return false;
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
#ifdef MM_APPKIT_DATA_CONTROL
    if (p->control_device) ext_data_control_offer_v1_receive(p->control_selection, type.c_str(), fds[1]);
    else
#endif
        wl_data_offer_receive(p->selection, type.c_str(), fds[1]);
    close(fds[1]);
    std::string result;
    bool success = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (p->generation == info.generation) {
        char buffer[16384];
        auto n = ::read(fds[0], buffer, sizeof buffer);
        if (n == 0) { success = true; break; }
        if (n > 0) {
            if (result.size() + static_cast<std::size_t>(n) > 64 * 1024 * 1024) break;
            result.append(buffer, static_cast<std::size_t>(n)); continue;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN || std::chrono::steady_clock::now() >= deadline) break;
        if (!dispatch_wayland(p->display, 50, fds[0])) break;
    }
    close(fds[0]);
    if (success) bytes = std::move(result);
    return success;
}
}
