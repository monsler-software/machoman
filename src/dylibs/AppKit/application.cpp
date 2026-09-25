/* NSApplication and the process-level objects around it: NSScreen, NSMenu,
 * NSMenuItem, NSWorkspace, NSAlert, the panels, and AppKit's exported
 * constants.
 *
 * `-run` is the real event loop: it pulls platform events, turns them into
 * NSEvents, sends them to the window they belong to, and redraws windows
 * that asked for it. */
#include "appkit.h"

#include <chrono>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
MM_FW_CLASS(NSSavePanel);
MM_FW_CLASS(NSOpenPanel);

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

struct app_object {
    mm_objc_object base;
    id next = nullptr;
    id delegate = nullptr, main_menu = nullptr;
    std::vector<id> windows;      /* not retained: windows outlive the list */
    bool running = false, active = false;
    int termination_reply = 1;
};
app_object *app(id o) { return as<app_object>(o); }

struct menu_object { mm_objc_object base; std::string title; std::vector<id> items; };
struct menu_item_object {
    mm_objc_object base;
    std::string title, key_equivalent;
    id target = nullptr, submenu = nullptr, represented = nullptr;
    SEL action = nullptr;
    std::uint64_t tag = 0, state = 0;
    bool enabled = true, hidden = false;
};

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
std::uint64_t app_mainMenu(id self, SEL) { return ret(app(self)->main_menu); }
std::uint64_t app_setMainMenu(id self, SEL, id m) { app(self)->main_menu = m ? retain(m) : nullptr; return 0; }
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
std::uint64_t app_keyWindow(id self, SEL) {
    for (id window : app(self)->windows)
        if (send(window, "isKeyWindow")) return ret(window);
    return 0;
}
std::uint64_t app_mainWindow(id self, SEL) { return app_keyWindow(self, nullptr); }
std::uint64_t app_windowWithWindowNumber(id, SEL, std::uint64_t) { return 0; }

/* The event loop. One iteration: wait for a platform event (or a timeout so
 * timers and redraws still happen), deliver it, then redraw. */
void deliver(const platform_event &pe) {
    id window = pe.window ? platform_window_owner(pe.window) : nullptr;
    switch (pe.kind) {
    case event_kind::mouse_down:
    case event_kind::mouse_up:
    case event_kind::mouse_moved:
    case event_kind::mouse_dragged:
    case event_kind::scroll: {
        std::uint64_t type = ev_mouse_moved;
        if (pe.kind == event_kind::mouse_down) type = pe.button == 1 ? ev_right_mouse_down : pe.button == 0 ? ev_left_mouse_down : ev_other_mouse_down;
        else if (pe.kind == event_kind::mouse_up) type = pe.button == 1 ? ev_right_mouse_up : pe.button == 0 ? ev_left_mouse_up : ev_other_mouse_up;
        else if (pe.kind == event_kind::mouse_dragged) type = pe.button == 1 ? ev_right_mouse_dragged : pe.button == 0 ? ev_left_mouse_dragged : ev_other_mouse_dragged;
        else if (pe.kind == event_kind::scroll) type = ev_scroll_wheel;
        g_mouse = pe.where;
        if (pe.kind == event_kind::mouse_down) g_pressed_buttons |= 1ull << pe.button;
        if (pe.kind == event_kind::mouse_up) g_pressed_buttons &= ~(1ull << pe.button);
        id event = make_event(type, window, pe.where, pe.modifiers);
        auto *e = as<event_object>(event);
        e->button = static_cast<std::uint64_t>(pe.button);
        e->click_count = pe.kind == event_kind::mouse_down ? 1 : 0;
        e->dx = pe.dx;
        e->dy = pe.dy;
        if (window) send(window, "sendEvent:", ret(event));
        break;
    }
    case event_kind::key_down:
    case event_kind::key_up: {
        id event = make_event(pe.kind == event_kind::key_down ? ev_key_down : ev_key_up, window, pe.where, pe.modifiers);
        auto *e = as<event_object>(event);
        e->keycode = pe.keycode;
        e->characters = pe.characters;
        if (window) send(window, "sendEvent:", ret(event));
        break;
    }
    case event_kind::flags_changed: {
        g_modifiers = pe.modifiers;
        id event = make_event(ev_flags_changed, window, pe.where, pe.modifiers);
        if (window) send(window, "sendEvent:", ret(event));
        break;
    }
    case event_kind::resized:
        if (window) {
            const std::uint64_t args[1] = {0};
            (void)args;
            mm_cpu_context *ctx = api()->current_context();
            const rect f{0, 0, pe.new_size.w, pe.new_size.h};
            if (ctx) {
                std::memcpy(&ctx->v[0][0], &f.x, 8);
                std::memcpy(&ctx->v[1][0], &f.y, 8);
                std::memcpy(&ctx->v[2][0], &f.w, 8);
                std::memcpy(&ctx->v[3][0], &f.h, 8);
            }
            send(window, "setFrame:display:", 1);
            id delegate = obj(send(window, "delegate"));
            if (responds(delegate, "windowDidResize:")) send(delegate, "windowDidResize:", ret(window));
        }
        break;
    case event_kind::close_requested:
        if (window) {
            id delegate = obj(send(window, "delegate"));
            bool close = true;
            if (responds(delegate, "windowShouldClose:")) close = send(delegate, "windowShouldClose:", ret(window)) != 0;
            if (close) send(window, "close");
        }
        break;
    case event_kind::exposed:
        if (window) send(window, "display");
        break;
    case event_kind::focus_in:
    case event_kind::focus_out:
        if (window) send(window, pe.kind == event_kind::focus_in ? "becomeKeyWindow" : "resignKeyWindow");
        break;
    case event_kind::none:
        break;
    }
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
        platform_event pe;
        if (platform::next_event(pe, 0.016)) deliver(pe);
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
        for (id window : o->windows)
            if (send(window, "isVisible")) send(window, "displayIfNeeded");
        CFRunLoopRunInMode_(nullptr, 0, 0); /* run whatever the app queued */
    }
    return 0;
}
std::uint64_t app_stop(id self, SEL, id) { app(self)->running = false; platform::wake(); return 0; }
std::uint64_t app_terminate(id self, SEL, id sender) {
    id delegate = app(self)->delegate;
    if (responds(delegate, "applicationShouldTerminate:")) {
        const std::uint64_t reply = send(delegate, "applicationShouldTerminate:", ret(sender));
        if (reply == 0) return 0; /* NSTerminateCancel */
    }
    if (responds(delegate, "applicationWillTerminate:")) send(delegate, "applicationWillTerminate:", 0);
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
std::uint64_t app_sendEvent(id, SEL, id event) {
    id window = obj(send(event, "window"));
    if (window) send(window, "sendEvent:", ret(event));
    return 0;
}
std::uint64_t app_postEvent(id, SEL, id, std::uint64_t) { return 0; }
std::uint64_t app_noop(id, SEL, ...) { return 0; }
std::uint64_t app_currentEvent(id, SEL) { return 0; }
std::uint64_t app_runModalForWindow(id, SEL, id window) {
    /* A modal session with no nesting: show the window and let the main
     * loop keep running until it goes away. */
    send(window, "makeKeyAndOrderFront:", 0);
    return 0;
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

std::uint64_t menu_initWithTitle(id self, SEL, id title) { as<menu_object>(self)->title = to_utf8(title); return ret(self); }
std::uint64_t menu_init(id self, SEL) { return ret(self); }
std::uint64_t menu_addItem(id self, SEL, id item) { as<menu_object>(self)->items.push_back(retain(item)); return 0; }
std::uint64_t menu_numberOfItems(id self, SEL) { return as<menu_object>(self)->items.size(); }
std::uint64_t menu_itemAtIndex(id self, SEL, std::uint64_t i) {
    auto &items = as<menu_object>(self)->items;
    return i < items.size() ? ret(items[i]) : 0;
}
std::uint64_t menu_title(id self, SEL) { return ret(string(as<menu_object>(self)->title)); }
std::uint64_t menu_setTitle(id self, SEL, id t) { as<menu_object>(self)->title = to_utf8(t); return 0; }
std::uint64_t menu_addItemWithTitle(id self, SEL, id title, SEL action, id key) {
    id item = obj(send(reinterpret_cast<id>(g_menu_item), "alloc"));
    const std::uint64_t args[3] = {ret(title), ret(action), ret(key)};
    item = obj(mm_objc_send(item, mm_objc_sel("initWithTitle:action:keyEquivalent:"), 3, args));
    menu_addItem(self, nullptr, item);
    return ret(item);
}
std::uint64_t menu_removeAllItems(id self, SEL) {
    auto &items = as<menu_object>(self)->items;
    for (id i : items) release(i);
    items.clear();
    return 0;
}

std::uint64_t item_initWithTitle(id self, SEL, id title, SEL action, id key) {
    auto *o = as<menu_item_object>(self);
    o->title = to_utf8(title);
    o->action = action;
    o->key_equivalent = key ? to_utf8(key) : "";
    return ret(self);
}
std::uint64_t item_init(id self, SEL) { return ret(self); }
std::uint64_t item_title(id self, SEL) { return ret(string(as<menu_item_object>(self)->title)); }
std::uint64_t item_setTitle(id self, SEL, id t) { as<menu_item_object>(self)->title = to_utf8(t); return 0; }
std::uint64_t item_setTarget(id self, SEL, id t) { as<menu_item_object>(self)->target = t; return 0; }
std::uint64_t item_target(id self, SEL) { return ret(as<menu_item_object>(self)->target); }
std::uint64_t item_setAction(id self, SEL, SEL a) { as<menu_item_object>(self)->action = a; return 0; }
std::uint64_t item_action(id self, SEL) { return ret(as<menu_item_object>(self)->action); }
std::uint64_t item_setSubmenu(id self, SEL, id m) { as<menu_item_object>(self)->submenu = m ? retain(m) : nullptr; return 0; }
std::uint64_t item_submenu(id self, SEL) { return ret(as<menu_item_object>(self)->submenu); }
std::uint64_t item_setEnabled(id self, SEL, std::uint64_t e) { as<menu_item_object>(self)->enabled = (e & 0xFF) != 0; return 0; }
std::uint64_t item_isEnabled(id self, SEL) { return as<menu_item_object>(self)->enabled; }
std::uint64_t item_setState(id self, SEL, std::uint64_t s) { as<menu_item_object>(self)->state = s; return 0; }
std::uint64_t item_state(id self, SEL) { return as<menu_item_object>(self)->state; }
std::uint64_t item_setTag(id self, SEL, std::uint64_t t) { as<menu_item_object>(self)->tag = t; return 0; }
std::uint64_t item_tag(id self, SEL) { return as<menu_item_object>(self)->tag; }
std::uint64_t item_setRepresentedObject(id self, SEL, id o) { as<menu_item_object>(self)->represented = o; return 0; }
std::uint64_t item_representedObject(id self, SEL) { return ret(as<menu_item_object>(self)->represented); }
std::uint64_t item_separatorItem(id, SEL) {
    id item = obj(send(reinterpret_cast<id>(g_menu_item), "alloc"));
    return ret(autorelease(obj(send(item, "init"))));
}

/* ---- NSWorkspace and friends ------------------------------------------------ */

std::uint64_t workspace_shared(id cls_, SEL) {
    static id instance = nullptr;
    if (!instance) instance = mm_objc_alloc(reinterpret_cast<Class>(cls_), 0);
    return ret(instance);
}
std::uint64_t workspace_openURL(id, SEL, id url) {
    if (trace()) std::fprintf(stderr, "AppKit: -[NSWorkspace openURL:] %s\n", to_utf8(obj(send(url, "absoluteString"))).c_str());
    return 0;
}
/* NSAlert is a window with a message in it, so it keeps the same state a
 * window does: an app configures the alert, may ask for its -window to
 * position or theme it, and then runs it. Nothing here can wait for a
 * person to click, so -runModal reports the first button — the one a
 * default alert makes "OK" — and says on stderr what was asked. */
struct alert_object {
    mm_objc_object base;
    id message = nullptr, informative = nullptr, window = nullptr;
    std::vector<id> buttons;
    std::uint64_t style = 1; /* NSAlertStyleWarning */
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

std::uint64_t alert_runModal(id self, SEL) {
    alert_object *a = alert(self);
    std::fprintf(stderr, "AppKit: NSAlert: %s%s%s\n", to_utf8(a->message).c_str(),
                 a->informative ? " — " : "", a->informative ? to_utf8(a->informative).c_str() : "");
    return 1000; /* NSAlertFirstButtonReturn */
}
std::uint64_t alert_beginSheetModal(id self, SEL, id, id handler) {
    /* The completion handler runs with the same answer -runModal gives. */
    alert_runModal(self, nullptr);
    if (handler) {
        const std::uint64_t response = 1000;
        mm_objc_block_invoke(handler, 1, &response);
    }
    return 0;
}
std::uint64_t panel_runModal(id, SEL) { return 0; /* NSModalResponseCancel */ }

} // namespace

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
void application_remove_window(id window) {
    if (!g_NSApp) return;
    auto &v = app(g_NSApp)->windows;
    v.erase(std::remove(v.begin(), v.end(), window), v.end());
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
        {"mainMenu", MM_IMP(app_mainMenu), "@16@0:8"},
        {"setMainMenu:", MM_IMP(app_setMainMenu), "v24@0:8@16"},
        {"isRunning", MM_IMP(app_isRunning), "B16@0:8"},
        {"isActive", MM_IMP(app_isActive), "B16@0:8"},
        {"setActivationPolicy:", MM_IMP(app_setActivationPolicy), "B24@0:8q16"},
        {"activateIgnoringOtherApps:", MM_IMP(app_activateIgnoringOtherApps), "v24@0:8B16"},
        {"activate", MM_IMP(app_activateIgnoringOtherApps), "v16@0:8"},
        {"windows", MM_IMP(app_windows), "@16@0:8"},
        {"keyWindow", MM_IMP(app_keyWindow), "@16@0:8"},
        {"mainWindow", MM_IMP(app_mainWindow), "@16@0:8"},
        {"windowWithWindowNumber:", MM_IMP(app_windowWithWindowNumber), "@24@0:8q16"},
        {"finishLaunching", MM_IMP(app_finishLaunching), "v16@0:8"},
        {"run", MM_IMP(app_run), "v16@0:8"},
        {"stop:", MM_IMP(app_stop), "v24@0:8@16"},
        {"terminate:", MM_IMP(app_terminate), "v24@0:8@16"},
        {"nextEventMatchingMask:untilDate:inMode:dequeue:", MM_IMP(app_nextEventMatchingMask), "@48@0:8Q16@24@32B40"},
        {"sendEvent:", MM_IMP(app_sendEvent), "v24@0:8@16"},
        {"postEvent:atStart:", MM_IMP(app_postEvent), "v32@0:8@16B24"},
        {"currentEvent", MM_IMP(app_currentEvent), "@16@0:8"},
        {"runModalForWindow:", MM_IMP(app_runModalForWindow), "q24@0:8@16"});
    for (const char *sel : {"setApplicationIconImage:", "hide:", "unhide:", "unhideAllApplications:", "arrangeInFront:",
                            "updateWindows", "registerForRemoteNotifications", "setServicesMenu:", "setWindowsMenu:",
                            "setHelpMenu:", "orderFrontStandardAboutPanel:", "abortModal", "stopModal", "requestUserAttention:",
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
        {"removeAllItems", MM_IMP(menu_removeAllItems), "v16@0:8"});
    for (const char *sel : {"setAutoenablesItems:", "setDelegate:", "update", "cancelTracking", "removeItem:",
                            "insertItem:atIndex:", "setSubmenu:forItem:", "popUpMenuPositioningItem:atLocation:inView:"})
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
        {"setRepresentedObject:", MM_IMP(item_setRepresentedObject), "v24@0:8@16"});
    for (const char *sel : {"setKeyEquivalent:", "setKeyEquivalentModifierMask:", "setImage:", "setHidden:",
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

    Class save = mm_objc_define_class("NSSavePanel", cls("NSPanel"), sizeof(window_object),
                                      MM_FW_CLASS_REF(NSSavePanel), MM_FW_META_REF(NSSavePanel));
    Class open = mm_objc_define_class("NSOpenPanel", save, sizeof(window_object), MM_FW_CLASS_REF(NSOpenPanel), MM_FW_META_REF(NSOpenPanel));
    for (Class c : {save, open}) MM_METHODS(c, {"runModal", MM_IMP(panel_runModal), "q16@0:8"});
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
