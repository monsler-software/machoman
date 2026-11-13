// A compositor fixture over socketpairs verifies real protocol and pipe
// transfers without touching the user's desktop clipboard.
#include "../src/dylibs/AppKit/clipboard_wayland.h"
#include <wayland-client.h>
#include <wayland-server.h>
#ifdef MM_APPKIT_DATA_CONTROL
#include "ext-data-control-v1-client-protocol.h"
#include "ext-data-control-v1-server-protocol.h"
#endif
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "clipboard: %s\n", message); std::exit(1); }
}
struct compositor {
    struct source {
        compositor *server;
        wl_resource *resource;
        std::vector<std::string> types;
        bool control;
    };
    struct device { compositor *server; wl_resource *resource; bool control; };
    struct offer { compositor *server; wl_resource *resource; source *from; bool control; };
    wl_display *display = wl_display_create();
    std::vector<std::unique_ptr<source>> sources;
    std::vector<std::unique_ptr<device>> devices;
    source *selection = nullptr;
    std::atomic<unsigned> writes{0};
    std::atomic<std::uint32_t> last_serial{0};
    std::atomic<bool> running{true};
    std::thread worker;

    static void destroy(wl_client *, wl_resource *resource) { wl_resource_destroy(resource); }
    static void seat_unused(wl_client *, wl_resource *, std::uint32_t) {}
    static constexpr struct wl_seat_interface seat_impl = {seat_unused, seat_unused, seat_unused, destroy};
    static void bind_seat(wl_client *client, void *, std::uint32_t version, std::uint32_t id) {
        auto *r = wl_resource_create(client, &wl_seat_interface, static_cast<int>(version), id);
        wl_resource_set_implementation(r, &seat_impl, nullptr, nullptr);
        wl_seat_send_capabilities(r, 0);
    }
    static void offered(wl_client *, wl_resource *r, const char *type) {
        static_cast<source *>(wl_resource_get_user_data(r))->types.emplace_back(type);
    }
    static void source_actions(wl_client *, wl_resource *, std::uint32_t) {}
    static constexpr struct wl_data_source_interface source_impl = {offered, destroy, source_actions};
    static void source_gone(wl_resource *r) {
        auto *s = static_cast<source *>(wl_resource_get_user_data(r));
        s->resource = nullptr;
        if (s->server->selection == s) s->server->selection = nullptr;
    }
    void create_source(wl_client *client, std::uint32_t id, int version, bool control) {
        const wl_interface *interface = &wl_data_source_interface;
#ifdef MM_APPKIT_DATA_CONTROL
        if (control) interface = &ext_data_control_source_v1_interface;
#endif
        auto *r = wl_resource_create(client, interface, version, id);
        auto s = std::make_unique<source>(source{this, r, {}, control});
        const void *implementation = &source_impl;
#ifdef MM_APPKIT_DATA_CONTROL
        if (control) implementation = &control_source_impl;
#endif
        wl_resource_set_implementation(r, implementation, s.get(), source_gone);
        sources.push_back(std::move(s));
    }
    static void manager_source(wl_client *client, wl_resource *r, std::uint32_t id) {
        static_cast<compositor *>(wl_resource_get_user_data(r))->create_source(client, id, wl_resource_get_version(r), false);
    }
    static void accept(wl_client *, wl_resource *, std::uint32_t, const char *) {}
    static void receive(wl_client *, wl_resource *r, const char *mime, std::int32_t fd) {
        auto *o = static_cast<offer *>(wl_resource_get_user_data(r));
        if (o->from && o->from->resource) {
#ifdef MM_APPKIT_DATA_CONTROL
            if (o->from->control) ext_data_control_source_v1_send_send(o->from->resource, mime, fd);
            else
#endif
                wl_data_source_send_send(o->from->resource, mime, fd);
        }
        close(fd);
    }
    static void finish(wl_client *, wl_resource *) {}
    static void offer_actions(wl_client *, wl_resource *, std::uint32_t, std::uint32_t) {}
    static constexpr struct wl_data_offer_interface offer_impl = {accept, receive, destroy, finish, offer_actions};
    static void offer_gone(wl_resource *r) { delete static_cast<offer *>(wl_resource_get_user_data(r)); }
    void advertise(device *d) {
        if (!d->resource) return;
        wl_resource *r = nullptr;
        if (selection) {
            const wl_interface *interface = &wl_data_offer_interface;
            const void *implementation = &offer_impl;
#ifdef MM_APPKIT_DATA_CONTROL
            if (d->control) { interface = &ext_data_control_offer_v1_interface; implementation = &control_offer_impl; }
#endif
            r = wl_resource_create(wl_resource_get_client(d->resource), interface, wl_resource_get_version(d->resource), 0);
            wl_resource_set_implementation(r, implementation, new offer{this, r, selection, d->control}, offer_gone);
#ifdef MM_APPKIT_DATA_CONTROL
            if (d->control) ext_data_control_device_v1_send_data_offer(d->resource, r);
            else
#endif
                wl_data_device_send_data_offer(d->resource, r);
            for (auto &mime : selection->types) {
#ifdef MM_APPKIT_DATA_CONTROL
                if (d->control) ext_data_control_offer_v1_send_offer(r, mime.c_str());
                else
#endif
                    wl_data_offer_send_offer(r, mime.c_str());
            }
        }
#ifdef MM_APPKIT_DATA_CONTROL
        if (d->control) ext_data_control_device_v1_send_selection(d->resource, r);
        else
#endif
            wl_data_device_send_selection(d->resource, r);
    }
    void select(wl_resource *r, std::uint32_t serial) {
        auto *next = r ? static_cast<source *>(wl_resource_get_user_data(r)) : nullptr;
        if (selection && selection != next && selection->resource) {
#ifdef MM_APPKIT_DATA_CONTROL
            if (selection->control) ext_data_control_source_v1_send_cancelled(selection->resource);
            else
#endif
                wl_data_source_send_cancelled(selection->resource);
        }
        selection = next; last_serial = serial; ++writes;
        for (auto &d : devices) advertise(d.get());
    }
    static void start_drag(wl_client *, wl_resource *, wl_resource *, wl_resource *, wl_resource *, std::uint32_t) {}
    static void set_selection(wl_client *, wl_resource *r, wl_resource *s, std::uint32_t serial) {
        static_cast<device *>(wl_resource_get_user_data(r))->server->select(s, serial);
    }
    static constexpr struct wl_data_device_interface device_impl = {start_drag, set_selection, destroy};
    static void device_gone(wl_resource *r) { static_cast<device *>(wl_resource_get_user_data(r))->resource = nullptr; }
    void create_device(wl_client *client, std::uint32_t id, int version, bool control) {
        const wl_interface *interface = &wl_data_device_interface;
        const void *implementation = &device_impl;
#ifdef MM_APPKIT_DATA_CONTROL
        if (control) { interface = &ext_data_control_device_v1_interface; implementation = &control_device_impl; }
#endif
        auto *r = wl_resource_create(client, interface, version, id);
        auto d = std::make_unique<device>(device{this, r, control});
        wl_resource_set_implementation(r, implementation, d.get(), device_gone);
        advertise(d.get()); devices.push_back(std::move(d));
    }
    static void manager_device(wl_client *c, wl_resource *r, std::uint32_t id, wl_resource *) {
        static_cast<compositor *>(wl_resource_get_user_data(r))->create_device(c, id, wl_resource_get_version(r), false);
    }
    static constexpr struct wl_data_device_manager_interface manager_impl = {manager_source, manager_device};
    static void bind_manager(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {
        auto *r = wl_resource_create(client, &wl_data_device_manager_interface, static_cast<int>(version), id);
        wl_resource_set_implementation(r, &manager_impl, data, nullptr);
    }
#ifdef MM_APPKIT_DATA_CONTROL
    static constexpr struct ext_data_control_source_v1_interface control_source_impl = {offered, destroy};
    static constexpr struct ext_data_control_offer_v1_interface control_offer_impl = {receive, destroy};
    static void control_select(wl_client *, wl_resource *r, wl_resource *s) {
        static_cast<device *>(wl_resource_get_user_data(r))->server->select(s, 0);
    }
    static void control_primary(wl_client *, wl_resource *, wl_resource *) {}
    static constexpr struct ext_data_control_device_v1_interface control_device_impl = {control_select, destroy, control_primary};
    static void control_manager_source(wl_client *c, wl_resource *r, std::uint32_t id) {
        static_cast<compositor *>(wl_resource_get_user_data(r))->create_source(c, id, 1, true);
    }
    static void control_manager_device(wl_client *c, wl_resource *r, std::uint32_t id, wl_resource *) {
        static_cast<compositor *>(wl_resource_get_user_data(r))->create_device(c, id, 1, true);
    }
    static constexpr struct ext_data_control_manager_v1_interface control_manager_impl = {control_manager_source, control_manager_device, destroy};
    static void bind_control(wl_client *c, void *data, std::uint32_t, std::uint32_t id) {
        auto *r = wl_resource_create(c, &ext_data_control_manager_v1_interface, 1, id);
        wl_resource_set_implementation(r, &control_manager_impl, data, nullptr);
    }
#endif
    explicit compositor(bool control) {
        check(display, "server display");
        wl_global_create(display, &wl_seat_interface, 5, this, bind_seat);
        wl_global_create(display, &wl_data_device_manager_interface, 3, this, bind_manager);
#ifdef MM_APPKIT_DATA_CONTROL
        if (control) wl_global_create(display, &ext_data_control_manager_v1_interface, 1, this, bind_control);
#else
        (void)control;
#endif
    }
    ~compositor() {
        running = false; if (worker.joinable()) worker.join();
        wl_display_destroy_clients(display); wl_display_destroy(display);
    }
    void start() {
        worker = std::thread([this] {
            while (running) { wl_event_loop_dispatch(wl_display_get_event_loop(display), 10); wl_display_flush_clients(display); }
        });
    }
};
struct connection {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_seat *seat = nullptr;
    ak::wayland_clipboard clipboard;
    static void global(void *p, wl_registry *r, std::uint32_t name, const char *interface, std::uint32_t version) {
        auto *c = static_cast<connection *>(p);
        c->clipboard.global(r, name, interface, version);
        if (!std::strcmp(interface, "wl_seat")) c->seat = static_cast<wl_seat *>(wl_registry_bind(r, name, &wl_seat_interface, 5));
    }
    static void removed(void *, wl_registry *, std::uint32_t) {}
    static constexpr wl_registry_listener listener = {global, removed};
    explicit connection(compositor &s) {
        int fds[2]; check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0, "socketpair");
        if (!wl_client_create(s.display, fds[0])) {
            std::fprintf(stderr, "clipboard: server client: %s\n", std::strerror(errno)); std::exit(1);
        }
        display = wl_display_connect_to_fd(fds[1]); check(display, "client display");
        registry = wl_display_get_registry(display); wl_registry_add_listener(registry, &listener, this);
    }
    void setup() {
        roundtrip(); clipboard.setup(display, seat); roundtrip();
    }
    void roundtrip() { check(wl_display_roundtrip(display) >= 0, "protocol roundtrip"); }
    // The clipboard's proxies must be destroyed before disconnecting display.
    void close() { wl_seat_destroy(seat); wl_registry_destroy(registry); }
};

static void exercise(bool control) {
    compositor server(control);
    wl_display *ad, *bd;
    {
        connection a(server), b(server); ad = a.display; bd = b.display;
        server.start(); a.setup(); b.setup();
        const std::string text = "Привет 😀\nline two";
        const std::string binary("a\0b\xff", 4);
        std::string large(1024 * 1024, 'x'); large[65536] = '\0';
        ak::clipboard_data payload{{"text/plain;charset=utf-8", text}, {"application/test", binary},
                                    {"application/empty", ""}, {"application/large", large}};
        a.clipboard.write(payload); a.roundtrip();
        if (!control) {
            check(server.writes == 0, "copy waits for input serial");
            a.clipboard.input(101); a.roundtrip(); check(server.last_serial == 101, "input serial used for ownership");
        } else check(server.writes == 1, "data-control writes without input");
        b.roundtrip(); const auto before = b.clipboard.snapshot();
        check(before.types.size() == payload.size(), "advertised MIME types");
        std::atomic<bool> pumping{true};
        std::thread owner([&] { while (pumping) ak::dispatch_wayland(ad, 10); });
        for (const auto &[mime, bytes] : payload) {
            std::string received = "unchanged";
            check(b.clipboard.read(mime, received) && received == bytes, "cross-client pipe transfer, binary/empty/large data");
        }
        std::string absent = "unchanged";
        check(!b.clipboard.read("missing", absent) && absent == "unchanged", "missing MIME type");
        pumping = false; owner.join();
        b.clipboard.input(202); b.clipboard.write({{"text/plain", "external replacement"}}); b.roundtrip(); a.roundtrip();
        check(a.clipboard.snapshot().generation > before.generation, "foreign selection invalidates local cache");
        pumping = true;
        owner = std::thread([&] { while (pumping) ak::dispatch_wayland(bd, 10); });
        std::string received;
        check(a.clipboard.read("text/plain", received) && received == "external replacement", "host replacement read");
        pumping = false; owner.join();
        b.clipboard.unfocus(); b.clipboard.write({}); b.roundtrip();
        if (!control) { b.clipboard.input(303); b.roundtrip(); }
        a.roundtrip(); check(a.clipboard.snapshot().types.empty(), "clear host selection");
        check(!wl_display_get_error(ad) && !wl_display_get_error(bd), "no Wayland protocol errors");
        a.close(); b.close();
    }
    wl_display_disconnect(ad); wl_display_disconnect(bd);
}
int main() {
    exercise(false);
#ifdef MM_APPKIT_DATA_CONTROL
    exercise(true);
#endif
    std::puts("Wayland clipboard ownership, serials, typed transfers and replacement passed");
}
