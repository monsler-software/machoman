/* NSApplication and the process-level objects around it: NSScreen, NSMenu,
 * NSMenuItem, NSWorkspace, NSAlert, the panels, and AppKit's exported
 * constants.
 *
 * `-run` is the real event loop: it pulls platform events, turns them into
 * NSEvents, sends them to the window they belong to, and redraws windows
 * that asked for it. */
#include "appkit.h"
#include "native_menu.h"
#include "native_dialog.h"

#include <chrono>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <unistd.h>
#include <string>
#include <vector>

MM_FW_CLASS(NSApplication);
MM_FW_CLASS(NSScreen);
MM_FW_CLASS(NSMenu);
MM_FW_CLASS(NSMenuItem);
MM_FW_CLASS(NSWorkspace);
MM_FW_CLASS(NSWorkspaceOpenConfiguration);
MM_FW_CLASS(NSRunningApplication);
MM_FW_CLASS(NSAlert);

/* NSApp is a variable the guest reads directly, so the exported symbol is
 * the storage and everything here refers to it. */
extern "C" __attribute__((visibility("default"))) mm_id NSApp_storage __asm__("_NSApp");
mm_id NSApp_storage = nullptr;

namespace ak {

id &g_NSApp = NSApp_storage;
std::uint64_t g_pressed_buttons = 0, g_modifiers = 0;
point g_mouse;

/* CoreFoundation's run loop, which NSRunLoop is a wrapper around. */
extern "C" std::int32_t CFRunLoopRunInMode_(id mode, double seconds, int return_after_source) __asm__("_CFRunLoopRunInMode");

namespace {

Class g_application, g_screen, g_menu, g_menu_item;
void poll_alerts();

constexpr std::int64_t modal_stop = -1000, modal_abort = -1001, modal_continue = -1002;
struct modal_session { id window; std::int64_t response = modal_continue; };

struct app_object {
    mm_objc_object base;
    id next = nullptr;
    id delegate = nullptr, main_menu = nullptr, services_provider = nullptr;
    id current_event = nullptr;
    std::vector<id> windows;      /* not retained: windows outlive the list */
    std::vector<id> ordered_windows; /* visible windows, front to back; not retained */
    std::vector<modal_session *> modals; /* nested calls own their sessions */
    bool running = false, active = false, finished_launching = false;
    int termination_reply = 1;
    bool last_window_close_pending=false, user_close_pending=false;
};
app_object *app(id o) { return as<app_object>(o); }

std::uint64_t menu_generation = 1;
struct menu_object {
    mm_objc_object base; std::string title; std::vector<id> items;
    id delegate = nullptr;
    bool autoenables = true;
    ~menu_object();
};
struct menu_item_object {
    mm_objc_object base;
    std::string title, key_equivalent;
    id target = nullptr, submenu = nullptr, represented = nullptr, owner_menu = nullptr;
    SEL action = nullptr;
    std::uint64_t tag = 0, state = 0;
    std::uint64_t key_modifiers = 1ull << 20;
    bool enabled = true, hidden = false, separator = false;
    ~menu_item_object() { release(submenu); release(represented); }
};
menu_object::~menu_object() {
    for (id item : items) { as<menu_item_object>(item)->owner_menu = nullptr; release(item); }
}

double now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

/* ---- NSApplication -------------------------------------------------------- */

/* +sharedApplication instantiates the receiver, so an app with its own
 * NSApplication subclass gets an instance of that subclass. */
id shared_application(Class cls_) {
    if (!g_NSApp) {
        id instance = obj(send(reinterpret_cast<id>(cls_ ? cls_ : g_application), "alloc"));
        g_NSApp = obj(send(instance, "init"));
    }
    return g_NSApp;
}

std::uint64_t app_sharedApplication(id cls_, SEL) { return ret(shared_application(reinterpret_cast<Class>(cls_))); }
std::uint64_t app_init(id self, SEL) { g_NSApp = self; return ret(self); }
std::uint64_t app_delegate(id self, SEL) { return ret(app(self)->delegate); }
std::uint64_t app_setDelegate(id self, SEL, id d) {
    if (trace())
        std::fprintf(stderr, "AppKit: -[NSApplication setDelegate:] a %s\n",
                     d ? mm_objc_class_name(mm_objc_object_class(d)) : "(nil)");
    app(self)->delegate = d;
    return 0;
}
std::uint64_t app_setServicesProvider(id self, SEL, id provider) { app(self)->services_provider = provider; return 0; }
std::uint64_t app_mainMenu(id self, SEL) { return ret(app(self)->main_menu); }
std::uint64_t app_setMainMenu(id self, SEL, id m) {
    if (m == app(self)->main_menu) return 0;
    const auto previous = app(self)->main_menu;
    app(self)->main_menu = m ? retain(m) : nullptr;
    release(previous); ++menu_generation; return 0;
}
std::uint64_t app_isRunning(id self, SEL) { return app(self)->running; }
std::uint64_t app_isActive(id self, SEL) { return app(self)->active; }
std::uint64_t app_setActivationPolicy(id, SEL, std::uint64_t) { return 1; }
std::uint64_t app_activateIgnoringOtherApps(id self, SEL, std::uint64_t) {
    app(self)->active = true;
    return 0;
}
std::uint64_t app_windows(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id window : app(self)->windows) send(array, "addObject:", ret(window));
    return ret(array);
}
std::uint64_t app_orderedWindows(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id window : app(self)->ordered_windows) send(array, "addObject:", ret(window));
    return ret(array);
}
std::uint64_t app_keyWindow(id self, SEL) {
    for (id window : app(self)->windows)
        if (send(window, "isKeyWindow")) return ret(window);
    return 0;
}
std::uint64_t app_mainWindow(id self, SEL) { return app_keyWindow(self, nullptr); }
std::uint64_t app_windowWithWindowNumber(id, SEL, std::uint64_t) { return 0; }

void set_current_event(id self, id event) {
    if (app(self)->current_event == event) return;
    id previous = app(self)->current_event;
    app(self)->current_event = retain(event);
    release(previous);
}
void dispatch_event(id event) {
    set_current_event(g_NSApp, event);
    // Native dragging owns pointer tracking even if an application overrides
    // sendEvent: without calling super.
    if (dragging_session_event(event)) return;
    /* Applications can override sendEvent: to manage their own responder
     * state. In particular Live switches its text-input view here. */
    send(g_NSApp, "sendEvent:", ret(event));
}

void invalidate_view_tree(id view) {
    if (!view) return;
    auto *o = as<view_object>(view);
    o->needs_display = true;
    for (id child : o->subviews) invalidate_view_tree(child);
}

void invalidate_window(id window) {
    if (!window) return;
    auto *o = as<window_object>(window);
    o->needs_display = true;
    invalidate_view_tree(o->content_view);
}

/* Deliver state/input first; the loop draws after its bounded event burst. */
void deliver(const platform_event &pe) {
    id window = pe.window ? platform_window_owner(pe.window) : nullptr;
    switch (pe.kind) {
    case event_kind::menu_action:
        application_menu_activate(pe.menu_token);
        break;
    case event_kind::menu_open:
        application_menu_open(pe.menu_token);
        break;
    case event_kind::mouse_down:
    case event_kind::mouse_up:
    case event_kind::mouse_moved:
    case event_kind::mouse_dragged:
    case event_kind::scroll: {
        if (pe.kind == event_kind::mouse_moved || pe.kind == event_kind::mouse_dragged) application_cursor_moved();
        std::uint64_t type = ev_mouse_moved;
        if (pe.kind == event_kind::mouse_down) type = pe.button == 1 ? ev_right_mouse_down : pe.button == 0 ? ev_left_mouse_down : ev_other_mouse_down;
        else if (pe.kind == event_kind::mouse_up) type = pe.button == 1 ? ev_right_mouse_up : pe.button == 0 ? ev_left_mouse_up : ev_other_mouse_up;
        else if (pe.kind == event_kind::mouse_dragged) type = pe.button == 1 ? ev_right_mouse_dragged : pe.button == 0 ? ev_left_mouse_dragged : ev_other_mouse_dragged;
        else if (pe.kind == event_kind::scroll) type = ev_scroll_wheel;
        const rect frame = window ? as<window_object>(window)->frame : rect{};
        g_mouse = {pe.where.x + frame.x, pe.where.y + frame.y};
        if (trace() && (pe.kind == event_kind::mouse_down || pe.kind == event_kind::mouse_up))
            std::fprintf(stderr, "AppKit: pointer %s window=%p local={%g,%g} screen={%g,%g}\n",
                         pe.kind == event_kind::mouse_down ? "down" : "up", static_cast<void *>(window),
                         pe.where.x, pe.where.y, g_mouse.x, g_mouse.y);
        if (pe.kind == event_kind::mouse_down) g_pressed_buttons |= 1ull << pe.button;
        if (pe.kind == event_kind::mouse_up) g_pressed_buttons &= ~(1ull << pe.button);
        id event = make_event(type, window, pe.where, pe.modifiers);
        auto *e = as<event_object>(event);
        e->button = static_cast<std::uint64_t>(pe.button);
        e->click_count = pe.kind == event_kind::mouse_down ? 1 : 0;
        e->dx = pe.dx;
        e->dy = pe.dy;
        if (window) dispatch_event(event);
        break;
    }
    case event_kind::key_down:
    case event_kind::key_up: {
        id event = make_event(pe.kind == event_kind::key_down ? ev_key_down : ev_key_up, window, pe.where, pe.modifiers);
        auto *e = as<event_object>(event);
        e->keycode = pe.keycode;
        e->characters = pe.characters;
        e->characters_ignoring_modifiers = pe.characters_ignoring_modifiers;
        if (trace()) std::fprintf(stderr, "AppKit: key %s window=%p macCode=%u modifiers=0x%x text=%s\n",
            pe.kind == event_kind::key_down ? "down" : "up", static_cast<void *>(window), pe.keycode,
            pe.modifiers, pe.characters_ignoring_modifiers.c_str());
        if (window) dispatch_event(event);
        break;
    }
    case event_kind::flags_changed: {
        g_modifiers = pe.modifiers;
        id event = make_event(ev_flags_changed, window, pe.where, pe.modifiers);
        as<event_object>(event)->keycode = pe.keycode;
        if (window) dispatch_event(event);
        break;
    }
    case event_kind::resized:
        if (window) {
            const std::uint64_t args[1] = {0};
            (void)args;
            mm_cpu_context *ctx = api()->current_context();
            const rect previous = as<window_object>(window)->frame;
            const rect f{previous.x, previous.y, pe.new_size.w, pe.new_size.h};
            if (ctx) {
                std::memcpy(&ctx->v[0][0], &f.x, 8);
                std::memcpy(&ctx->v[1][0], &f.y, 8);
                std::memcpy(&ctx->v[2][0], &f.w, 8);
                std::memcpy(&ctx->v[3][0], &f.h, 8);
            }
            send(window, "setFrame:display:", 0);
            invalidate_window(window);
            id delegate = obj(send(window, "delegate"));
            if (responds(delegate, "windowDidResize:")) send(delegate, "windowDidResize:", ret(window));
        }
        break;
    case event_kind::close_requested:
        if (window) {
            id delegate = obj(send(window, "delegate"));
            bool close = true;
            if (responds(delegate, "windowShouldClose:")) close = send(delegate, "windowShouldClose:", ret(window)) != 0;
            if (close) {
                send(window,"close");
                if(g_NSApp)app(g_NSApp)->user_close_pending=true;
            }
        }
        break;
    case event_kind::exposed:
        invalidate_window(window);
        break;
    case event_kind::focus_in:
    case event_kind::focus_out:
        if (pe.kind == event_kind::focus_out && window) cancel_dragging_session(window);
        if (window) send(window, pe.kind == event_kind::focus_in ? "becomeKeyWindow" : "resignKeyWindow");
        break;
    case event_kind::none:
        break;
    }
}

void redraw_windows(id self) {
    layer_display_pass pass;
    static std::uint64_t last_menu_generation = 0;
    static std::vector<native_menu_item> menu_snapshot;
    if (last_menu_generation != menu_generation) {
        menu_snapshot = application_menu_snapshot();
        last_menu_generation = menu_generation;
    }
    /* Guest display callbacks can create or close a window. Keep a retained
     * snapshot so neither vector mutation nor deallocation breaks the pass. */
    const auto windows = app(self)->windows;
    for (id window : windows) retain(window);
    for (id window : windows) {
        if (send(window, "isVisible")) {
            const auto started = std::chrono::steady_clock::now();
            auto *wo = as<window_object>(window);
            if (wo->platform) platform::set_menu(wo->platform, menu_snapshot);
            if (wo->pending_configure_notification && wo->content_view) {
                wo->pending_configure_notification = false;
                auto *view = as<view_object>(wo->content_view);
                if (view->posts_frame_notifications) {
                    id center = obj(send(reinterpret_cast<id>(cls("NSNotificationCenter")), "defaultCenter"));
                    send(center, "postNotificationName:object:", ret(string("NSViewFrameDidChangeNotification")), ret(wo->content_view));
                }
            }
            send(window, "displayIfNeeded");
            if (wo->content_view) draw_layers(wo->content_view);
#ifdef MM_APPKIT_EVILCOCOA
            render_cocoa_window(window);
#endif
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (trace() && elapsed > std::chrono::milliseconds(200))
                std::fprintf(stderr, "AppKit: slow redraw window=%p size=%gx%g took %.1f ms\n",
                             static_cast<void *>(window), wo->frame.w, wo->frame.h,
                             std::chrono::duration<double, std::milli>(elapsed).count());
        }
    }
    for (id window : windows) release(window);
}

/* AppKit's notifications go through the one NSNotificationCenter, because
 * an app that is not the NSApplication delegate — as Ableton Live is not —
 * observes them there and would otherwise never learn it has launched.
 * The names are the plain constants: the guest's binds resolve to strings
 * of the symbol's own name, which is what these are. */
void post_app_notification(id self, const char *name) {
    id center = obj(send(reinterpret_cast<id>(cls("NSNotificationCenter")), "defaultCenter"));
    if (!center) return;
    id name_string = string(name);
    if (trace()) std::fprintf(stderr, "AppKit: posting %s\n", name);
    send(center, "postNotificationName:object:", ret(name_string), ret(self));
}

std::uint64_t app_finishLaunching(id self, SEL) {
    if (app(self)->finished_launching) return 0;
    app(self)->finished_launching = true;
    id delegate = app(self)->delegate;
    if (trace())
        std::fprintf(stderr, "AppKit: -[NSApplication finishLaunching], delegate is a %s\n",
                     delegate ? mm_objc_class_name(mm_objc_object_class(delegate)) : "(nil)");
    if (trace() && delegate) {
        /* Which of the delegate methods AppKit is supposed to send does
         * this delegate actually implement? An app that waits for one we
         * never send looks exactly like an app that has finished. */
        static const char *const delegate_selectors[] = {
            "applicationWillFinishLaunching:", "applicationDidFinishLaunching:",
            "applicationShouldOpenUntitledFile:", "applicationOpenUntitledFile:",
            "application:openFile:", "application:openFiles:", "applicationDidBecomeActive:",
            "applicationWillBecomeActive:", "applicationShouldTerminate:", "applicationWillTerminate:",
            "applicationShouldHandleReopen:hasVisibleWindows:", "applicationDidChangeScreenParameters:"};
        std::string answered;
        for (const char *sel : delegate_selectors)
            if (responds(delegate, sel)) { answered += ' '; answered += sel; }
        std::fprintf(stderr, "AppKit: delegate implements%s\n", answered.empty() ? " nothing we send" : answered.c_str());
    }
    id note = obj(send(reinterpret_cast<id>(cls("NSNotification")), "alloc"));
    if (responds(delegate, "applicationWillFinishLaunching:")) send(delegate, "applicationWillFinishLaunching:", ret(note));
    post_app_notification(self, "NSApplicationWillFinishLaunchingNotification");
    if (responds(delegate, "applicationDidFinishLaunching:")) send(delegate, "applicationDidFinishLaunching:", ret(note));
    post_app_notification(self, "NSApplicationDidFinishLaunchingNotification");
    post_app_notification(self, "NSApplicationWillBecomeActiveNotification");
    post_app_notification(self, "NSApplicationDidBecomeActiveNotification");

    /* A debugging aid: order every window the program built onto the
     * screen, whether or not it asked for that. It answers one question —
     * is the window missing because nothing draws it, or because the
     * program never asked to show it? */
    if (std::getenv("MACHOMAN_APPKIT_FORCE_SHOW")) {
        for (id window : app(self)->windows) {
            std::fprintf(stderr, "AppKit: forcing a window on screen\n");
            send(window, "makeKeyAndOrderFront:", 0);
        }
    }

    /* An app launched without a document is sent the open-application
     * Apple Event, which AppKit turns into
     * -applicationShouldOpenUntitledFile: and then
     * -applicationOpenUntitledFile:. That is where a document-based
     * program opens its first window — Ableton Live opens its default Set
     * there — so without it the app finishes launching and then sits with
     * a window nobody was ever told to show. */
    if (responds(delegate, "applicationOpenUntitledFile:")) {
        const bool ask = responds(delegate, "applicationShouldOpenUntitledFile:");
        const bool open = !ask || send(delegate, "applicationShouldOpenUntitledFile:", ret(self)) != 0;
        if (trace()) std::fprintf(stderr, "AppKit: applicationOpenUntitledFile: %s\n", open ? "yes" : "declined");
        if (open) send(delegate, "applicationOpenUntitledFile:", ret(self));
    }
    return 0;
}

std::uint64_t app_run(id self, SEL) {
    app_object *o = app(self);
    if (trace()) std::fprintf(stderr, "AppKit: -[NSApplication run]\n");
    o->running = true;
    o->active = true;
    send(self, "finishLaunching");
    /* An app that is idle here has stopped asking for anything: the trace
     * says so once a second, with the window count, because silence from
     * the guest and silence from the loop look identical otherwise. */
    auto last_idle = std::chrono::steady_clock::now();
    while (o->running) {
        autorelease_pool pool;
        platform_event pe;
        if (platform::next_event(pe, 0.016)) {
            deliver(pe);
            /* Consume a bounded burst before drawing another expensive
             * guest frame, while still giving timers and redraw a turn. */
            for (int i = 1; i < 64 && platform::next_event(pe, 0); ++i) deliver(pe);
        }
        if (trace()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_idle > std::chrono::seconds(1)) {
                last_idle = now;
                std::size_t visible = 0;
                for (id window : o->windows) visible += send(window, "isVisible") ? 1 : 0;
                std::fprintf(stderr, "AppKit: event loop idle, %zu windows (%zu visible)\n", o->windows.size(), visible);
            }
        }
        /* Redraw what asked for it, then let CoreFoundation's run loop run
         * the blocks and sources the app queued. */
        dispatch_native_dialogs();
        poll_file_panels();
        poll_alerts();
        redraw_windows(self);
        CFRunLoopRunInMode_(nullptr, 0, 0); /* run whatever the app queued */
        if(o->last_window_close_pending) {
            o->last_window_close_pending=false;
            bool visible=false;for(id window:o->windows)if(as<window_object>(window)->visible){visible=true;break;}
            if(!visible) {
                const bool has_policy=responds(o->delegate,"applicationShouldTerminateAfterLastWindowClosed:");
                bool quit=has_policy?send(o->delegate,"applicationShouldTerminateAfterLastWindowClosed:",ret(self))!=0:o->user_close_pending;
                if(!has_policy&&std::getenv("MACHOMAN_APPKIT_KEEP_ALIVE"))quit=false;
                if(quit)send(self,"terminate:",ret(self));
            }
            o->user_close_pending=false;
        }
    }
    return 0;
}
std::uint64_t app_stop(id self, SEL, id) { app(self)->running = false; platform::wake(); return 0; }
std::uint64_t app_terminate(id self, SEL, id sender) {
    id delegate = app(self)->delegate;
    if (responds(delegate, "applicationShouldTerminate:")) {
        const std::uint64_t reply = send(delegate, "applicationShouldTerminate:", ret(self));
        if (reply == 0 || reply == 2) return 0; /* NSTerminateCancel */
    }
    if (responds(delegate, "applicationWillTerminate:")) send(delegate, "applicationWillTerminate:", ret(self));
    app(self)->running = false;
    std::exit(0);
}
std::uint64_t app_nextEventMatchingMask(id, SEL, std::uint64_t mask, id, id, std::uint64_t) {
    if (trace()) {
        static bool said = false;
        if (!said) { said = true; std::fprintf(stderr, "AppKit: -[NSApplication nextEventMatchingMask:0x%llx]\n", static_cast<unsigned long long>(mask)); }
    }
    platform_event pe;
    if (!platform::next_event(pe, 0)) return 0;
    deliver(pe);
    return 0;
}
std::uint64_t app_sendEvent(id self, SEL, id event) {
    set_current_event(self, event);
    if (dragging_session_event(event)) return 0;
    id window = obj(send(event, "window"));
    if (file_panel_blocks_window(window)) return 0;
    if (send(event, "type") == ev_key_down && !text_input_has_priority(window, event) && app(self)->modals.empty() && app(self)->main_menu &&
        send(app(self)->main_menu, "performKeyEquivalent:", ret(event))) return 0;
    if (window) send(window, "sendEvent:", ret(event));
    return 0;
}
std::uint64_t app_postEvent(id, SEL, id, std::uint64_t) { return 0; }
std::uint64_t app_noop(id, SEL, ...) { return 0; }
std::uint64_t app_currentEvent(id self, SEL) { return ret(app(self)->current_event); }
std::uint64_t app_modalWindow(id self, SEL) {
    const auto &sessions = app(self)->modals;
    return sessions.empty() ? 0 : ret(sessions.back()->window);
}
std::uint64_t app_stopModalWithCode(id self, SEL, std::int64_t code) {
    auto &sessions = app(self)->modals;
    if (!sessions.empty()) sessions.back()->response = code;
    platform::wake();
    return 0;
}
std::uint64_t app_stopModal(id self, SEL) { return app_stopModalWithCode(self, nullptr, modal_stop); }
std::uint64_t app_abortModal(id self, SEL) { return app_stopModalWithCode(self, nullptr, modal_abort); }
std::uint64_t app_runModalForWindow(id self, SEL, id window) {
    if (!window) return static_cast<std::uint64_t>(modal_abort);
    modal_session session{retain(window)};
    auto *o = app(self);
    o->modals.push_back(&session);
    if (trace()) std::fprintf(stderr, "AppKit: modal begin window=%p depth=%zu\n", static_cast<void *>(window), o->modals.size());
    send(window, "makeKeyAndOrderFront:", 0);
    id mode = string("NSModalPanelRunLoopMode");
    while (session.response == modal_continue) {
        autorelease_pool pool;
        platform_event pe;
        for (int i = 0; i < 64 && session.response == modal_continue &&
             platform::next_event(pe, i == 0 ? 0.016 : 0); ++i) {
            bool input = false;
            switch (pe.kind) {
            case event_kind::mouse_down: case event_kind::mouse_up:
            case event_kind::mouse_moved: case event_kind::mouse_dragged:
            case event_kind::scroll: case event_kind::key_down:
            case event_kind::key_up: case event_kind::flags_changed:
            case event_kind::menu_action: case event_kind::menu_open:
                input = true;
                break;
            default: break;
            }
            if (!input || (pe.window && platform_window_owner(pe.window) == window)) deliver(pe);
        }
        dispatch_native_dialogs();
        poll_file_panels();
        poll_alerts();
        redraw_windows(self);
        CFRunLoopRunInMode_(mode, 0, 0);
        if (session.response == modal_continue && !send(window, "isVisible")) session.response = modal_abort;
    }
    o->modals.pop_back();
    if (trace()) std::fprintf(stderr, "AppKit: modal end window=%p response=%lld depth=%zu\n", static_cast<void *>(window), static_cast<long long>(session.response), o->modals.size());
    release(session.window);
    return static_cast<std::uint64_t>(session.response);
}

/* ---- NSScreen -------------------------------------------------------------- */

id main_screen() {
    static id screen = nullptr;
    if (!screen) screen = mm_objc_alloc(g_screen, 0);
    return screen;
}
std::uint64_t screen_mainScreen(id, SEL) { return ret(main_screen()); }
std::uint64_t screen_screens(id, SEL) {
    id one = main_screen();
    id array = obj(send(reinterpret_cast<id>(cls("NSArray")), "arrayWithObject:", ret(one)));
    return ret(array);
}
mm_two_doubles screen_frame(id, SEL) {
    const size s = platform::screen_size();
    mm_objc_return_doubles_hi(s.w, s.h);
    return {0, 0};
}
double screen_backingScaleFactor(id, SEL) { return platform::screen_scale(); }
std::uint64_t screen_deviceDescription(id, SEL) {
    id d = obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary"));
    send(d, "setObject:forKey:", ret(number(1)), ret(string("NSScreenNumber")));
    return ret(d);
}

/* ---- NSMenu / NSMenuItem ---------------------------------------------------- */

std::uint64_t menu_initWithTitle(id self, SEL, id title) { as<menu_object>(self)->title = to_utf8(title); ++menu_generation; return ret(self); }
std::uint64_t menu_init(id self, SEL) { return ret(self); }
std::uint64_t menu_addItem(id self, SEL, id item) {
    if (item) { as<menu_object>(self)->items.push_back(retain(item)); as<menu_item_object>(item)->owner_menu = self; }
    ++menu_generation; return 0;
}
std::uint64_t menu_numberOfItems(id self, SEL) { return as<menu_object>(self)->items.size(); }
std::uint64_t menu_itemAtIndex(id self, SEL, std::uint64_t i) {
    auto &items = as<menu_object>(self)->items; return i < items.size() ? ret(items[i]) : 0;
}
std::uint64_t menu_title(id self, SEL) { return ret(string(as<menu_object>(self)->title)); }
std::uint64_t menu_setTitle(id self, SEL, id t) { as<menu_object>(self)->title = to_utf8(t); ++menu_generation; return 0; }
std::uint64_t menu_addItemWithTitle(id self, SEL, id title, SEL action, id key) {
    id item = obj(send(reinterpret_cast<id>(g_menu_item), "alloc"));
    const std::uint64_t args[3] = {ret(title), ret(action), ret(key)};
    item = obj(mm_objc_send(item, mm_objc_sel("initWithTitle:action:keyEquivalent:"), 3, args));
    menu_addItem(self, nullptr, item); return ret(autorelease(item));
}
std::uint64_t menu_removeAllItems(id self, SEL) {
    auto &items = as<menu_object>(self)->items; const auto old = std::move(items); items.clear();
    ++menu_generation; for (id i : old) { as<menu_item_object>(i)->owner_menu = nullptr; release(i); } return 0;
}
std::uint64_t menu_insertItem(id self, SEL, id item, std::uint64_t index) {
    auto &items = as<menu_object>(self)->items;
    if (item && index <= items.size()) { items.insert(items.begin() + index, retain(item)); as<menu_item_object>(item)->owner_menu = self; }
    ++menu_generation; return 0;
}
std::uint64_t menu_removeItem(id self, SEL, id item) {
    auto &items = as<menu_object>(self)->items; const auto it = std::find(items.begin(), items.end(), item);
    if (it != items.end()) { items.erase(it); as<menu_item_object>(item)->owner_menu = nullptr; ++menu_generation; release(item); } return 0;
}
std::uint64_t menu_removeItemAtIndex(id self, SEL, std::uint64_t index) {
    auto &items = as<menu_object>(self)->items; return index < items.size() ? menu_removeItem(self, nullptr, items[index]) : 0;
}
std::uint64_t menu_itemWithTag(id self, SEL, std::uint64_t tag) {
    for (id item : as<menu_object>(self)->items) if (send(item, "tag") == tag) return ret(item); return 0;
}
std::uint64_t menu_setDelegate(id self, SEL, id delegate) { as<menu_object>(self)->delegate = delegate; return 0; }
std::uint64_t menu_delegate(id self, SEL) { return ret(as<menu_object>(self)->delegate); }
std::uint64_t menu_autoenables(id self, SEL) { return as<menu_object>(self)->autoenables; }
std::uint64_t menu_setAutoenables(id self, SEL, std::uint64_t value) { as<menu_object>(self)->autoenables = value & 255; ++menu_generation; return 0; }

id action_target(SEL action, id target) {
    if (target) return target;
    if (!g_NSApp || !action) return nullptr;
    id window = obj(app_keyWindow(g_NSApp, nullptr));
    id responder = window ? obj(send(window, "firstResponder")) : nullptr;
    for (int n = 0; responder && n < 128; ++n) {
        if (send(responder, "respondsToSelector:", ret(action))) return responder;
        responder = obj(send(responder, "nextResponder"));
    }
    for (id candidate : {window, window ? obj(send(window, "delegate")) : nullptr, g_NSApp, app(g_NSApp)->delegate})
        if (candidate && send(candidate, "respondsToSelector:", ret(action))) return candidate;
    return nullptr;
}
std::uint64_t app_sendAction(id, SEL, SEL action, id target, id sender) {
    target = action_target(action, target); if (!target || !action) return 0;
    const auto argument = ret(sender); mm_objc_send(target, action, 1, &argument); return 1;
}
void update_menu(id menu, unsigned depth = 0) {
    if (!menu || depth >= 32) return;
    auto *m = as<menu_object>(menu);
    if (responds(m->delegate, "menuNeedsUpdate:")) send(m->delegate, "menuNeedsUpdate:", ret(menu));
    auto items = m->items; for (id item : items) retain(item);
    for (id item : items) {
        auto *i = as<menu_item_object>(item);
        if (m->autoenables && i->action && !i->submenu) {
            id target = action_target(i->action, i->target);
            bool enabled = target != nullptr;
            if (enabled && responds(target, "validateMenuItem:")) enabled = send(target, "validateMenuItem:", ret(item)) & 255;
            else if (enabled && responds(target, "validateUserInterfaceItem:")) enabled = send(target, "validateUserInterfaceItem:", ret(item)) & 255;
            if (i->enabled != enabled) { i->enabled = enabled; ++menu_generation; }
        }
        release(item);
    }
}
std::uint64_t menu_update(id self, SEL) { update_menu(self); return 0; }
std::uint64_t item_initWithTitle(id self, SEL, id title, SEL action, id key) {
    auto *o = as<menu_item_object>(self); o->title = to_utf8(title); o->action = action; o->key_equivalent = key ? to_utf8(key) : ""; ++menu_generation; return ret(self);
}
std::uint64_t item_init(id self, SEL) { return ret(self); }
std::uint64_t item_title(id self, SEL) { return ret(string(as<menu_item_object>(self)->title)); }
std::uint64_t item_setTitle(id self, SEL, id t) { as<menu_item_object>(self)->title = to_utf8(t); ++menu_generation; return 0; }
std::uint64_t item_setTarget(id self, SEL, id t) { as<menu_item_object>(self)->target = t; ++menu_generation; return 0; }
std::uint64_t item_target(id self, SEL) { return ret(as<menu_item_object>(self)->target); }
std::uint64_t item_menu(id self, SEL) { return ret(as<menu_item_object>(self)->owner_menu); }
std::uint64_t item_setAction(id self, SEL, SEL a) { as<menu_item_object>(self)->action = a; ++menu_generation; return 0; }
std::uint64_t item_action(id self, SEL) { return ret(as<menu_item_object>(self)->action); }
std::uint64_t item_setSubmenu(id self, SEL, id m) {
    auto *o = as<menu_item_object>(self); if (o->submenu == m) return 0;
    const auto old = o->submenu; o->submenu = retain(m); ++menu_generation; release(old); return 0;
}
std::uint64_t menu_setSubmenu(id, SEL, id submenu, id item) { return item ? item_setSubmenu(item, nullptr, submenu) : 0; }
std::uint64_t item_submenu(id self, SEL) { return ret(as<menu_item_object>(self)->submenu); }
std::uint64_t item_setEnabled(id self, SEL, std::uint64_t e) { as<menu_item_object>(self)->enabled = (e & 255) != 0; ++menu_generation; return 0; }
std::uint64_t item_isEnabled(id self, SEL) { return as<menu_item_object>(self)->enabled; }
std::uint64_t item_setHidden(id self, SEL, std::uint64_t e) { as<menu_item_object>(self)->hidden = (e & 255) != 0; ++menu_generation; return 0; }
std::uint64_t item_isHidden(id self, SEL) { return as<menu_item_object>(self)->hidden; }
std::uint64_t item_isSeparator(id self, SEL) { return as<menu_item_object>(self)->separator; }
std::uint64_t item_setState(id self, SEL, std::uint64_t s) { as<menu_item_object>(self)->state = s; ++menu_generation; return 0; }
std::uint64_t item_state(id self, SEL) { return as<menu_item_object>(self)->state; }
std::uint64_t item_setTag(id self, SEL, std::uint64_t t) { as<menu_item_object>(self)->tag = t; ++menu_generation; return 0; }
std::uint64_t item_tag(id self, SEL) { return as<menu_item_object>(self)->tag; }
std::uint64_t item_setRepresentedObject(id self, SEL, id value) {
    auto *o = as<menu_item_object>(self); const auto old = o->represented; o->represented = retain(value); release(old); return 0;
}
std::uint64_t item_representedObject(id self, SEL) { return ret(as<menu_item_object>(self)->represented); }
std::uint64_t item_keyEquivalent(id self, SEL) { return ret(string(as<menu_item_object>(self)->key_equivalent)); }
std::uint64_t item_setKeyEquivalent(id self, SEL, id key) { as<menu_item_object>(self)->key_equivalent = to_utf8(key); ++menu_generation; return 0; }
std::uint64_t item_keyModifiers(id self, SEL) { return as<menu_item_object>(self)->key_modifiers; }
std::uint64_t item_setKeyModifiers(id self, SEL, std::uint64_t mask) { as<menu_item_object>(self)->key_modifiers = mask; ++menu_generation; return 0; }
std::uint64_t item_separatorItem(id, SEL) {
    id item = obj(send(reinterpret_cast<id>(g_menu_item), "alloc")); item = obj(send(item, "init"));
    as<menu_item_object>(item)->separator = true; as<menu_item_object>(item)->enabled = false; return ret(autorelease(item));
}
id find_menu_item(id menu, std::uintptr_t token, unsigned depth = 0) {
    if (!menu || depth >= 32) return nullptr;
    for (id item : as<menu_object>(menu)->items) {
        if (ret(item) == token) return item;
        if (id found = find_menu_item(as<menu_item_object>(item)->submenu, token, depth + 1)) return found;
    }
    return nullptr;
}
std::vector<native_menu_item> snapshot_menu(id menu, unsigned depth = 0) {
    std::vector<native_menu_item> out; if (!menu || depth >= 32) return out;
    for (id item : as<menu_object>(menu)->items) {
        const auto *i = as<menu_item_object>(item); if (i->hidden) continue;
        native_menu_item entry; entry.token = ret(item); entry.title = i->title; entry.enabled = i->enabled;
        entry.separator = i->separator; entry.state = static_cast<std::int64_t>(i->state);
        if (!i->key_equivalent.empty()) {
            if (i->key_modifiers & (1ull << 20)) entry.shortcut += "Ctrl+";
            else if (i->key_modifiers & (1ull << 18)) entry.shortcut += "Ctrl+";
            if (i->key_modifiers & (1ull << 19)) entry.shortcut += "Alt+";
            if (i->key_modifiers & (1ull << 17)) entry.shortcut += "Shift+";
            entry.shortcut += i->key_equivalent;
        }
        entry.children = snapshot_menu(i->submenu, depth + 1);
        if (entry.title.empty() && i->submenu) entry.title = as<menu_object>(i->submenu)->title;
        out.push_back(std::move(entry));
    }
    return out;
}
id key_item(id menu, const std::string &key, std::uint64_t modifiers, unsigned depth = 0) {
    if (!menu || depth >= 32) return nullptr;
    for (id item : as<menu_object>(menu)->items) {
        auto *i = as<menu_item_object>(item); if (i->hidden || !i->enabled) continue;
        if (id found = key_item(i->submenu, key, modifiers, depth + 1)) return found;
        if (i->action && !i->key_equivalent.empty() && i->key_equivalent == key && (i->key_modifiers & 0x1e0000) == modifiers) return item;
    }
    return nullptr;
}
std::uint64_t menu_performKeyEquivalent(id self, SEL, id event) {
    if (send(event, "type") != ev_key_down) return 0;
    const auto key = to_utf8(obj(send(event, "charactersIgnoringModifiers")));
    auto modifiers = send(event, "modifierFlags") & 0x1e0000;
    id item = key_item(self, key, modifiers);
    const char *native = std::getenv("MACHOMAN_NATIVE_SHORTCUTS");
    if (!item && native && std::strcmp(native, "0") == 0 && (modifiers & (1ull << 18)) && !(modifiers & (1ull << 20)))
        item = key_item(self, key, (modifiers & ~(1ull << 18)) | (1ull << 20));
    if (!item) return 0;
    retain(item); auto *i = as<menu_item_object>(item); const auto result = app_sendAction(g_NSApp, nullptr, i->action, i->target, item); release(item); return result;
}

/* ---- NSWorkspace and friends ------------------------------------------------ */

std::uint64_t workspace_shared(id cls_, SEL) {
    static id instance = nullptr;
    if (!instance) instance = mm_objc_alloc(reinterpret_cast<Class>(cls_), 0);
    return ret(instance);
}
std::uint64_t workspace_openURL(id, SEL, id url) {
    const auto uri = to_utf8(obj(send(url, "absoluteString")));
    if (trace()) std::fprintf(stderr, "AppKit: -[NSWorkspace openURL:] %s\n", uri.c_str());
    return !uri.empty() && open_external_uri(uri);
}
std::uint64_t app_showHelp(id, SEL, id) {
    id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
    id folder = obj(send(bundle, "objectForInfoDictionaryKey:", ret(string("CFBundleHelpBookFolder"))));
    if (!folder) return 0;
    id directory = obj(send(bundle, "pathForResource:ofType:", ret(folder), 0));
    if (!directory) return 0;
    namespace fs = std::filesystem;
    const fs::path root(to_utf8(directory));
    std::error_code ec;
    fs::path index;
    for (auto name : {root.filename().string() + ".html", std::string("index.html")})
        if (fs::is_regular_file(root / name, ec)) { index = root / name; break; }
    if (index.empty()) {
        for (auto &entry : fs::directory_iterator(root, ec)) {
            if (entry.path().extension() != ".html") continue;
            id html = obj(send(reinterpret_cast<id>(cls("NSString")), "stringWithContentsOfFile:encoding:error:", ret(string(entry.path().string())), 4, 0));
            if (to_utf8(html).find("AppleTitle") != std::string::npos) { index = entry.path(); break; }
        }
    }
    if (!index.empty()) {
        id url = obj(send(reinterpret_cast<id>(cls("NSURL")), "fileURLWithPath:", ret(string(index.string()))));
        workspace_openURL(nullptr, nullptr, url);
    }
    return 0;
}
std::uint64_t workspace_setIcon(id, SEL, id, id, std::uint64_t) {
    // The Linux backend has no Finder per-file custom icons. Report failure
    // through the public BOOL result rather than aborting an optional action.
    return 0;
}
/* NSAlert is a window with a message in it, so it keeps the same state a
 * window does: an app configures the alert, may ask for its -window to
 * position or theme it, and then runs it. Nothing here can wait for a
 * person to click uses the native Linux message dialog backend. */
struct alert_object {
    mm_objc_object base;
    id message = nullptr, informative = nullptr, window = nullptr;
    std::vector<id> buttons;
    std::uint64_t style = 1; /* NSAlertStyleWarning */
    std::unique_ptr<native_dialog> request;
    id handler = nullptr;
    ~alert_object() {
        release(message); release(informative); release(window); release(handler);
        for (id button : buttons) release(button);
    }
};
alert_object *alert(id o) { return as<alert_object>(o); }

std::uint64_t alert_messageText(id self, SEL) { return ret(alert(self)->message); }
std::uint64_t alert_setMessageText(id self, SEL, id text) {
    alert_object *a = alert(self);
    if (a->message) release(a->message);
    a->message = text ? retain(text) : nullptr;
    if (a->window && text) send(a->window, "setTitle:", ret(text));
    return 0;
}
std::uint64_t alert_informativeText(id self, SEL) { return ret(alert(self)->informative); }
std::uint64_t alert_setInformativeText(id self, SEL, id text) {
    alert_object *a = alert(self);
    if (a->informative) release(a->informative);
    a->informative = text ? retain(text) : nullptr;
    return 0;
}
std::uint64_t alert_setAlertStyle(id self, SEL, std::uint64_t style) { alert(self)->style = style; return 0; }
std::uint64_t alert_alertStyle(id self, SEL) { return alert(self)->style; }

std::uint64_t alert_window(id self, SEL) {
    alert_object *a = alert(self);
    if (!a->window) {
        id w = make_window({0, 0, 420, 160}, 1 /* NSWindowStyleMaskTitled */);
        a->window = w ? retain(w) : nullptr;
        if (a->window && a->message) send(a->window, "setTitle:", ret(a->message));
    }
    return ret(a->window);
}

std::uint64_t alert_addButtonWithTitle(id self, SEL, id title) {
    /* The buttons are NSViews carrying their title; an alert's caller only
     * ever keeps them to set a key equivalent or read them back. */
    id button = obj(send(reinterpret_cast<id>(cls("NSButton")), "alloc"));
    button = obj(send(button, "init"));
    if (button && title) send(button, "setTitle:", ret(title));
    if (button) alert(self)->buttons.push_back(button);
    return ret(button);
}
std::uint64_t alert_buttons(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id b : alert(self)->buttons) send(array, "addObject:", ret(b));
    return ret(array);
}

std::unique_ptr<native_dialog> alert_request(id self) {
    auto *a = alert(self);
    std::vector<std::string> buttons;
    for (id button : a->buttons) buttons.push_back(to_utf8(obj(send(button, "title"))));
    return open_message_dialog(to_utf8(a->message), to_utf8(a->informative), buttons, static_cast<unsigned>(a->style));
}
std::uint64_t alert_response(const dialog_result &result) {
    if (!result.error.empty()) std::fprintf(stderr, "AppKit: alert: %s\n", result.error.c_str());
    return result.accepted && !result.paths.empty() ? 1000 + std::stoull(result.paths[0]) : 0;
}
std::vector<id> active_alerts;
std::uint64_t alert_runModal(id self, SEL) {
    auto request = alert_request(self);
    dialog_result result;
    while (!request->poll(result)) application_dialog_tick(nullptr);
    return alert_response(result);
}
std::uint64_t alert_beginSheetModal(id self, SEL, id, id handler) {
    if (alert(self)->request) return 0;
    alert(self)->request = alert_request(self);
    alert(self)->handler = handler ? obj(send(handler, "copy")) : nullptr;
    active_alerts.push_back(retain(self));
    return 0;
}
void poll_alerts() {
    const auto snapshot = active_alerts;
    for (id self : snapshot) retain(self);
    for (id self : snapshot) {
        auto it = std::find(active_alerts.begin(), active_alerts.end(), self);
        if (it == active_alerts.end()) { release(self); continue; }
        dialog_result result;
        if (alert(self)->request->poll(result)) {
            active_alerts.erase(it);
            auto handler = alert(self)->handler;
            alert(self)->handler = nullptr; alert(self)->request.reset();
            const auto response = alert_response(result);
            if (handler) { mm_objc_block_invoke(handler, 1, &response); release(handler); }
            release(self);
        }
        release(self);
    }
}

} // namespace

/* Modal native dialogs keep timers, queued guest work and display events
 * moving while input to the owning window is blocked. */
void application_dialog_tick(id owner) {
    autorelease_pool pool;
    platform_event pe;
    for (int i = 0; i < 64 && platform::next_event(pe, i == 0 ? 0.016 : 0); ++i) {
        const bool input = pe.kind == event_kind::menu_action || pe.kind == event_kind::menu_open ||
            pe.kind == event_kind::mouse_down || pe.kind == event_kind::mouse_up ||
            pe.kind == event_kind::mouse_moved || pe.kind == event_kind::mouse_dragged ||
            pe.kind == event_kind::key_down || pe.kind == event_kind::key_up ||
            pe.kind == event_kind::scroll || pe.kind == event_kind::flags_changed;
        if (!input || (owner && pe.window && platform_window_owner(pe.window) != owner)) deliver(pe);
    }
    dispatch_native_dialogs();
    poll_file_panels();
    poll_alerts();
    if (g_NSApp) redraw_windows(g_NSApp);
    CFRunLoopRunInMode_(string("NSModalPanelRunLoopMode"), 0, 0);
}

std::vector<native_menu_item> application_menu_snapshot() { return g_NSApp ? snapshot_menu(app(g_NSApp)->main_menu) : std::vector<native_menu_item>{}; }
void application_menu_activate(std::uintptr_t token) {
    if (!g_NSApp || !app(g_NSApp)->modals.empty() || file_panel_blocks_window(obj(send(g_NSApp, "keyWindow")))) return;
    id item = find_menu_item(app(g_NSApp)->main_menu, token); if (!item) return;
    retain(item); auto *i = as<menu_item_object>(item);
    if (i->enabled && !i->separator && !i->hidden && !i->submenu) send(g_NSApp, "sendAction:to:from:", ret(i->action), ret(i->target), ret(item));
    release(item);
}
void application_menu_open(std::uintptr_t token) {
    if (!g_NSApp) return;
    id item = find_menu_item(app(g_NSApp)->main_menu, token); if (!item) return;
    id menu = as<menu_item_object>(item)->submenu; if (!menu) return;
    retain(menu); auto *m = as<menu_object>(menu);
    if (responds(m->delegate, "menuWillOpen:")) send(m->delegate, "menuWillOpen:", ret(menu));
    update_menu(menu); release(menu);
}

id make_event(std::uint64_t type, id window, point where, std::uint64_t modifiers) {
    id event = obj(send(reinterpret_cast<id>(cls("NSEvent")), "alloc"));
    auto *e = as<event_object>(event);
    e->type = type;
    e->window = window;
    e->where = where;
    e->modifiers = modifiers;
    e->timestamp = now_seconds();
    return autorelease(event);
}

/* Windows register themselves so -[NSApplication windows] and the redraw
 * pass can find them. */
void application_add_window(id window) {
    if (!g_NSApp) return;
    auto &v = app(g_NSApp)->windows;
    if (std::find(v.begin(), v.end(), window) == v.end()) v.push_back(window);
}
void application_window_closed() {if(g_NSApp)app(g_NSApp)->last_window_close_pending=true;}
void application_remove_window(id window) {
    if (!g_NSApp) return;
    auto &v = app(g_NSApp)->windows;
    v.erase(std::remove(v.begin(), v.end(), window), v.end());
    application_order_out(window);
}
void application_order_front(id window) {
    if (!g_NSApp) return;
    application_add_window(window);
    application_order_out(window);
    auto &ordered = app(g_NSApp)->ordered_windows;
    ordered.insert(ordered.begin(), window);
}
void application_order_out(id window) {
    if (!g_NSApp) return;
    auto &ordered = app(g_NSApp)->ordered_windows;
    ordered.erase(std::remove(ordered.begin(), ordered.end(), window), ordered.end());
}

void register_application() {
    Class responder = cls("NSResponder");
    Class nsobject = cls("NSObject");

    g_application = mm_objc_define_class("NSApplication", responder, sizeof(app_object),
                                         MM_FW_CLASS_REF(NSApplication), MM_FW_META_REF(NSApplication));
    register_lifecycle<app_object>(g_application);
    MM_METHODS(g_application,
        {"init", MM_IMP(app_init), "@16@0:8"},
        {"delegate", MM_IMP(app_delegate), "@16@0:8"},
        {"setDelegate:", MM_IMP(app_setDelegate), "v24@0:8@16"},
        {"setServicesProvider:", MM_IMP(app_setServicesProvider), "v24@0:8@16"},
        {"mainMenu", MM_IMP(app_mainMenu), "@16@0:8"},
        {"setMainMenu:", MM_IMP(app_setMainMenu), "v24@0:8@16"},
        {"isRunning", MM_IMP(app_isRunning), "B16@0:8"},
        {"isActive", MM_IMP(app_isActive), "B16@0:8"},
        {"setActivationPolicy:", MM_IMP(app_setActivationPolicy), "B24@0:8q16"},
        {"activateIgnoringOtherApps:", MM_IMP(app_activateIgnoringOtherApps), "v24@0:8B16"},
        {"activate", MM_IMP(app_activateIgnoringOtherApps), "v16@0:8"},
        {"windows", MM_IMP(app_windows), "@16@0:8"},
        {"orderedWindows", MM_IMP(app_orderedWindows), "@16@0:8"},
        {"keyWindow", MM_IMP(app_keyWindow), "@16@0:8"},
        {"mainWindow", MM_IMP(app_mainWindow), "@16@0:8"},
        {"windowWithWindowNumber:", MM_IMP(app_windowWithWindowNumber), "@24@0:8q16"},
        {"finishLaunching", MM_IMP(app_finishLaunching), "v16@0:8"},
        {"run", MM_IMP(app_run), "v16@0:8"},
        {"stop:", MM_IMP(app_stop), "v24@0:8@16"},
        {"terminate:", MM_IMP(app_terminate), "v24@0:8@16"},
        {"showHelp:", MM_IMP(app_showHelp), "v24@0:8@16"},
        {"nextEventMatchingMask:untilDate:inMode:dequeue:", MM_IMP(app_nextEventMatchingMask), "@48@0:8Q16@24@32B40"},
        {"sendEvent:", MM_IMP(app_sendEvent), "v24@0:8@16"},
        {"sendAction:to:from:", MM_IMP(app_sendAction), "B40@0:8:16@24@32"},
        {"postEvent:atStart:", MM_IMP(app_postEvent), "v32@0:8@16B24"},
        {"currentEvent", MM_IMP(app_currentEvent), "@16@0:8"},
        {"runModalForWindow:", MM_IMP(app_runModalForWindow), "q24@0:8@16"},
        {"modalWindow", MM_IMP(app_modalWindow), "@16@0:8"},
        {"stopModal", MM_IMP(app_stopModal), "v16@0:8"},
        {"stopModalWithCode:", MM_IMP(app_stopModalWithCode), "v24@0:8q16"},
        {"abortModal", MM_IMP(app_abortModal), "v16@0:8"});
    for (const char *sel : {"setApplicationIconImage:", "hide:", "unhide:", "unhideAllApplications:", "arrangeInFront:",
                            "updateWindows", "registerForRemoteNotifications", "setServicesMenu:", "setWindowsMenu:",
                            "setHelpMenu:", "orderFrontStandardAboutPanel:", "requestUserAttention:",
                            "setPresentationOptions:", "disableRelaunchOnLogin", "replyToApplicationShouldTerminate:"})
        mm_objc_add_method(g_application, sel, MM_IMP(app_noop), "v24@0:8@16");
    MM_CLASS_METHODS(g_application, {"sharedApplication", MM_IMP(app_sharedApplication), "@16@0:8"});

    g_screen = mm_objc_define_class("NSScreen", nsobject, sizeof(mm_objc_object), MM_FW_CLASS_REF(NSScreen), MM_FW_META_REF(NSScreen));
    MM_METHODS(g_screen,
        {"frame", MM_IMP(screen_frame), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"visibleFrame", MM_IMP(screen_frame), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"backingScaleFactor", MM_IMP(screen_backingScaleFactor), "d16@0:8"},
        {"deviceDescription", MM_IMP(screen_deviceDescription), "@16@0:8"});
    MM_CLASS_METHODS(g_screen,
        {"mainScreen", MM_IMP(screen_mainScreen), "@16@0:8"},
        {"screens", MM_IMP(screen_screens), "@16@0:8"});

    g_menu = mm_objc_define_class("NSMenu", nsobject, sizeof(menu_object), MM_FW_CLASS_REF(NSMenu), MM_FW_META_REF(NSMenu));
    register_lifecycle<menu_object>(g_menu);
    MM_METHODS(g_menu,
        {"init", MM_IMP(menu_init), "@16@0:8"},
        {"initWithTitle:", MM_IMP(menu_initWithTitle), "@24@0:8@16"},
        {"addItem:", MM_IMP(menu_addItem), "v24@0:8@16"},
        {"addItemWithTitle:action:keyEquivalent:", MM_IMP(menu_addItemWithTitle), "@40@0:8@16:24@32"},
        {"numberOfItems", MM_IMP(menu_numberOfItems), "q16@0:8"},
        {"itemAtIndex:", MM_IMP(menu_itemAtIndex), "@24@0:8q16"},
        {"title", MM_IMP(menu_title), "@16@0:8"},
        {"setTitle:", MM_IMP(menu_setTitle), "v24@0:8@16"},
        {"removeAllItems", MM_IMP(menu_removeAllItems), "v16@0:8"},
        {"insertItem:atIndex:", MM_IMP(menu_insertItem), "v32@0:8@16q24"},
        {"removeItem:", MM_IMP(menu_removeItem), "v24@0:8@16"},
        {"removeItemAtIndex:", MM_IMP(menu_removeItemAtIndex), "v24@0:8q16"},
        {"itemWithTag:", MM_IMP(menu_itemWithTag), "@24@0:8q16"},
        {"setSubmenu:forItem:", MM_IMP(menu_setSubmenu), "v32@0:8@16@24"},
        {"setDelegate:", MM_IMP(menu_setDelegate), "v24@0:8@16"},
        {"delegate", MM_IMP(menu_delegate), "@16@0:8"},
        {"setAutoenablesItems:", MM_IMP(menu_setAutoenables), "v24@0:8B16"},
        {"autoenablesItems", MM_IMP(menu_autoenables), "B16@0:8"},
        {"update", MM_IMP(menu_update), "v16@0:8"},
        {"performKeyEquivalent:", MM_IMP(menu_performKeyEquivalent), "B24@0:8@16"});
    for (const char *sel : {"cancelTracking", "popUpMenuPositioningItem:atLocation:inView:"})
        mm_objc_add_method(g_menu, sel, MM_IMP(app_noop), "v24@0:8@16");

    g_menu_item = mm_objc_define_class("NSMenuItem", nsobject, sizeof(menu_item_object),
                                       MM_FW_CLASS_REF(NSMenuItem), MM_FW_META_REF(NSMenuItem));
    register_lifecycle<menu_item_object>(g_menu_item);
    MM_METHODS(g_menu_item,
        {"init", MM_IMP(item_init), "@16@0:8"},
        {"initWithTitle:action:keyEquivalent:", MM_IMP(item_initWithTitle), "@40@0:8@16:24@32"},
        {"title", MM_IMP(item_title), "@16@0:8"},
        {"setTitle:", MM_IMP(item_setTitle), "v24@0:8@16"},
        {"target", MM_IMP(item_target), "@16@0:8"},
        {"menu", MM_IMP(item_menu), "@16@0:8"},
        {"setTarget:", MM_IMP(item_setTarget), "v24@0:8@16"},
        {"action", MM_IMP(item_action), ":16@0:8"},
        {"setAction:", MM_IMP(item_setAction), "v24@0:8:16"},
        {"submenu", MM_IMP(item_submenu), "@16@0:8"},
        {"setSubmenu:", MM_IMP(item_setSubmenu), "v24@0:8@16"},
        {"isEnabled", MM_IMP(item_isEnabled), "B16@0:8"},
        {"setEnabled:", MM_IMP(item_setEnabled), "v24@0:8B16"},
        {"state", MM_IMP(item_state), "q16@0:8"},
        {"setState:", MM_IMP(item_setState), "v24@0:8q16"},
        {"tag", MM_IMP(item_tag), "q16@0:8"},
        {"setTag:", MM_IMP(item_setTag), "v24@0:8q16"},
        {"representedObject", MM_IMP(item_representedObject), "@16@0:8"},
        {"setRepresentedObject:", MM_IMP(item_setRepresentedObject), "v24@0:8@16"},
        {"keyEquivalent", MM_IMP(item_keyEquivalent), "@16@0:8"},
        {"setKeyEquivalent:", MM_IMP(item_setKeyEquivalent), "v24@0:8@16"},
        {"keyEquivalentModifierMask", MM_IMP(item_keyModifiers), "Q16@0:8"},
        {"setKeyEquivalentModifierMask:", MM_IMP(item_setKeyModifiers), "v24@0:8Q16"},
        {"isHidden", MM_IMP(item_isHidden), "B16@0:8"},
        {"setHidden:", MM_IMP(item_setHidden), "v24@0:8B16"},
        {"isSeparatorItem", MM_IMP(item_isSeparator), "B16@0:8"});
    for (const char *sel : {"setImage:",
                            "setToolTip:", "setAttributedTitle:", "setIndentationLevel:", "setOnStateImage:"})
        mm_objc_add_method(g_menu_item, sel, MM_IMP(app_noop), "v24@0:8@16");
    MM_CLASS_METHODS(g_menu_item, {"separatorItem", MM_IMP(item_separatorItem), "@16@0:8"});

    static auto running_current_of_workspace = [](id, SEL) -> std::uint64_t {
        return send(reinterpret_cast<id>(cls("NSRunningApplication")), "currentApplication");
    };
    Class workspace = mm_objc_define_class("NSWorkspace", nsobject, sizeof(mm_objc_object),
                                           MM_FW_CLASS_REF(NSWorkspace), MM_FW_META_REF(NSWorkspace));
    static auto workspace_running_apps = [](id, SEL) -> std::uint64_t {
        /* Only this process: enumerating other processes has no meaning
         * here, and a program looking for itself finds itself. */
        id one = obj(send(reinterpret_cast<id>(cls("NSRunningApplication")), "currentApplication"));
        return send(reinterpret_cast<id>(cls("NSArray")), "arrayWithObject:", ret(one));
    };
    MM_METHODS(workspace,
        {"openURL:", MM_IMP(workspace_openURL), "B24@0:8@16"},
        {"setIcon:forFile:options:", MM_IMP(workspace_setIcon), "B40@0:8@16@24Q32"},
        {"runningApplications", MM_IMP(+workspace_running_apps), "@16@0:8"},
        {"frontmostApplication", MM_IMP(+running_current_of_workspace), "@16@0:8"});
    static auto workspace_center = [](id, SEL) -> std::uint64_t {
        /* macOS gives the workspace a centre of its own; one centre
         * delivers everything here, and a program that observes workspace
         * notifications still gets what AppKit posts. */
        return send(reinterpret_cast<id>(cls("NSNotificationCenter")), "defaultCenter");
    };
    MM_METHODS(workspace, {"notificationCenter", MM_IMP(+workspace_center), "@16@0:8"});
    MM_CLASS_METHODS(workspace, {"sharedWorkspace", MM_IMP(workspace_shared), "@16@0:8"});
    if (Class screen = cls("NSScreen")) {
        static auto screens_separate = [](id, SEL) -> std::uint64_t { return 0; };
        mm_objc_add_class_method(screen, "screensHaveSeparateSpaces", MM_IMP(+screens_separate), "B16@0:8");
    }
    mm_objc_define_class("NSWorkspaceOpenConfiguration", nsobject, sizeof(mm_objc_object),
                         MM_FW_CLASS_REF(NSWorkspaceOpenConfiguration), MM_FW_META_REF(NSWorkspaceOpenConfiguration));
    Class running = mm_objc_define_class("NSRunningApplication", nsobject, sizeof(mm_objc_object),
                                         MM_FW_CLASS_REF(NSRunningApplication), MM_FW_META_REF(NSRunningApplication));
    static auto running_current = [](id cls_, SEL) -> std::uint64_t {
        static id instance = nullptr;
        if (!instance) instance = mm_objc_alloc(reinterpret_cast<Class>(cls_), 0);
        return ret(instance);
    };
    static auto running_pid = [](id, SEL) -> std::uint64_t { return static_cast<std::uint64_t>(getpid()); };
    static auto running_bundle_identifier = [](id, SEL) -> std::uint64_t {
        return send(obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle")), "bundleIdentifier");
    };
    static auto running_localized_name = [](id, SEL) -> std::uint64_t {
        return send(obj(send(reinterpret_cast<id>(cls("NSProcessInfo")), "processInfo")), "processName");
    };
    static auto running_bundle_url = [](id, SEL) -> std::uint64_t {
        return send(obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle")), "bundleURL");
    };
    static auto running_yes = [](id, SEL) -> std::uint64_t { return 1; };
    static auto running_no = [](id, SEL) -> std::uint64_t { return 0; };
    MM_CLASS_METHODS(running, {"currentApplication", MM_IMP(+running_current), "@16@0:8"});
    MM_METHODS(running,
        {"processIdentifier", MM_IMP(+running_pid), "i16@0:8"},
        {"bundleIdentifier", MM_IMP(+running_bundle_identifier), "@16@0:8"},
        {"localizedName", MM_IMP(+running_localized_name), "@16@0:8"},
        {"bundleURL", MM_IMP(+running_bundle_url), "@16@0:8"},
        {"isActive", MM_IMP(+running_yes), "B16@0:8"},
        {"isFinishedLaunching", MM_IMP(+running_yes), "B16@0:8"},
        {"isHidden", MM_IMP(+running_no), "B16@0:8"},
        {"isTerminated", MM_IMP(+running_no), "B16@0:8"});

    Class alert = mm_objc_define_class("NSAlert", nsobject, sizeof(alert_object), MM_FW_CLASS_REF(NSAlert), MM_FW_META_REF(NSAlert));
    mmfw::register_lifecycle<alert_object>(alert);
    MM_METHODS(alert,
        {"runModal", MM_IMP(alert_runModal), "q16@0:8"},
        {"window", MM_IMP(alert_window), "@16@0:8"},
        {"messageText", MM_IMP(alert_messageText), "@16@0:8"},
        {"setMessageText:", MM_IMP(alert_setMessageText), "v24@0:8@16"},
        {"informativeText", MM_IMP(alert_informativeText), "@16@0:8"},
        {"setInformativeText:", MM_IMP(alert_setInformativeText), "v24@0:8@16"},
        {"alertStyle", MM_IMP(alert_alertStyle), "Q16@0:8"},
        {"setAlertStyle:", MM_IMP(alert_setAlertStyle), "v24@0:8Q16"},
        {"addButtonWithTitle:", MM_IMP(alert_addButtonWithTitle), "@24@0:8@16"},
        {"buttons", MM_IMP(alert_buttons), "@16@0:8"},
        {"beginSheetModalForWindow:completionHandler:", MM_IMP(alert_beginSheetModal), "v32@0:8@16@?24"});
    for (const char *sel : {"setIcon:", "setShowsHelp:", "setShowsSuppressionButton:", "setAccessoryView:", "layout"})
        mm_objc_add_method(alert, sel, MM_IMP(app_noop), "v24@0:8@16");

    /* AppKit's categories on Foundation classes: nib loading (nib.cpp). */
    if (Class bundle = cls("NSBundle")) {
        static auto nib_load = [](id, SEL, id name, id owner) -> std::uint64_t {
            return load_nib(to_utf8(name), owner, nullptr) ? 1 : 0;
        };
        static auto nib_load_top = [](id, SEL, id name, id owner, id *top) -> std::uint64_t {
            std::vector<id> objects;
            if (!load_nib(to_utf8(name), owner, &objects)) return 0;
            if (top) {
                id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
                for (id o : objects) send(array, "addObject:", ret(o));
                *top = array;
            }
            return 1;
        };
        mm_objc_add_class_method(bundle, "loadNibNamed:owner:", MM_IMP(+nib_load), "B32@0:8@16@24");
        mm_objc_add_method(bundle, "loadNibNamed:owner:topLevelObjects:", MM_IMP(+nib_load_top), "B40@0:8@16@24^@32");
    }


}

} // namespace ak

/* ---- AppKit's exported symbols ---------------------------------------------- */

using namespace ak;

extern "C" {

extern __attribute__((visibility("default"))) double NSAppKitVersionNumber_symbol __asm__("_NSAppKitVersionNumber");
double NSAppKitVersionNumber_symbol = 2299.0; /* macOS 13 */

void NSBeep_() DARWIN(NSBeep);
void NSBeep_() { std::fputc('\a', stderr); }

std::uint64_t NSAccessibilityPostNotification_(id, id) DARWIN(NSAccessibilityPostNotification);
std::uint64_t NSAccessibilityPostNotification_(id, id) { return 0; }
std::uint64_t NSAccessibilityPostNotificationWithUserInfo_(id, id, id) DARWIN(NSAccessibilityPostNotificationWithUserInfo);
std::uint64_t NSAccessibilityPostNotificationWithUserInfo_(id, id, id) { return 0; }

} // extern "C"
