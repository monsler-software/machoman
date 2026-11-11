/* NSResponder and the small value classes around it: NSEvent, NSColor,
 * NSCursor, NSAppearance, NSTrackingArea, NSPasteboard.
 *
 * NSResponder is where the responder chain lives: unhandled events walk
 * `-nextResponder` until something implements the action, which is how
 * AppKit apps expect key handling and menu actions to work. */
#include "appkit.h"
#include "app_icon.h"
#include <algorithm>
#include <cmath>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

/* Class storage the guest binds to. */
MM_FW_CLASS(NSResponder);
MM_FW_CLASS(NSEvent);
MM_FW_CLASS(NSColor);
MM_FW_CLASS(NSColorSpace);
MM_FW_CLASS(NSCursor);
MM_FW_CLASS(NSAppearance);
MM_FW_CLASS(NSTrackingArea);
MM_FW_CLASS(NSPasteboard);
MM_FW_CLASS(NSPasteboardItem);
MM_FW_CLASS(NSImage);
MM_FW_CLASS(NSTextInputContext);
MM_FW_CLASS(NSInputManager);
MM_FW_CLASS(NSAccessibilityElement);
MM_FW_CLASS(NSDraggingItem);
MM_FW_CLASS(NSDraggingSession);

namespace ak {

bool text_input_has_priority(id window, id event) {
    if (!window || !event || send(event, "type") != ev_key_down) return false;
    if (send(event, "modifierFlags") & ((1ull << 18) | (1ull << 20))) return false;
    id target = obj(send(window, "firstResponder"));
    if (!target) target = obj(send(window, "contentView"));
    if (!target || (!responds(target, "insertText:replacementRange:") && !responds(target, "insertText:"))) return false;
    id characters = obj(send(event, "characters"));
    if (!characters || !send(characters, "length")) return false;
    const auto first = send(characters, "characterAtIndex:", 0);
    /* Navigation and editing commands also go through the text client;
     * function keys may still be application key equivalents. */
    if (first >= 0xf700 && first <= 0xf8ff) return first <= 0xf703;
    return first >= 0x20 || first == '\t' || first == '\r' || first == '\n' || first == 0x1b || first == 0x08;
}

namespace {

struct dragging_session_object {
    mm_objc_object base;
    id items = nullptr, source = nullptr, window = nullptr;
    point location;
    std::uint64_t button = 0;
    bool animate_cancel = true;
    ~dragging_session_object() { release(items); release(source); release(window); }
};
id active_drag = nullptr;
Class dragging_session_class;

void drag_notify(id session, const char *selector, point location, bool ended) {
    auto *o = as<dragging_session_object>(session);
    if (!responds(o->source, selector)) return;
    if (auto *ctx = api()->current_context()) {
        std::memcpy(&ctx->v[0][0], &location.x, 8);
        std::memcpy(&ctx->v[1][0], &location.y, 8);
    }
    // Until a destination accepts a drop, report NSDragOperationNone.
    if (ended) send(o->source, selector, ret(session), 0);
    else send(o->source, selector, ret(session));
}

void finish_drag() {
    id session = active_drag;
    if (!session) return;
    active_drag = nullptr; // The callback may immediately start another session.
    auto *o = as<dragging_session_object>(session);
    if (o->window) {
        auto *window = as<window_object>(o->window);
        for (auto &target : window->mouse_targets) { release(target); target = nullptr; }
    }
    drag_notify(session, "draggingSession:endedAtPoint:operation:", o->location, true);
    release(session);
}

mm_two_doubles drag_location(id self, SEL) { auto p = as<dragging_session_object>(self)->location; return {p.x, p.y}; }
std::uint64_t drag_animation(id self, SEL) { return as<dragging_session_object>(self)->animate_cancel; }
std::uint64_t drag_set_animation(id self, SEL, std::uint64_t value) { as<dragging_session_object>(self)->animate_cancel = value & 0xff; return 0; }

struct cursor_object {
    mm_objc_object base;
    cursor_spec specification;
    id image = nullptr;
    point hotspot;
    ~cursor_object() { release(image); }
};
Class cursor_class = nullptr;
id current_cursor = nullptr;
std::vector<id> cursor_stack;
unsigned cursor_hide_count = 0;
bool cursor_hidden_until_motion = false;
void apply_cursor() {
    cursor_spec value;
    if (current_cursor) value = as<cursor_object>(current_cursor)->specification;
    value.hidden = cursor_hide_count || cursor_hidden_until_motion;
    platform::set_cursor(value);
}
std::uint64_t cursor_set(id self, SEL) {
    if (current_cursor != self) { id old = current_cursor; current_cursor = retain(self); release(old); }
    apply_cursor(); return 0;
}
std::uint64_t cursor_named(id cls_, SEL selector) {
    static std::unordered_map<std::string, id> shapes;
    if (std::strcmp(selector, "currentCursor") == 0 || std::strcmp(selector, "currentSystemCursor") == 0) {
        if (!current_cursor) cursor_set(obj(cursor_named(cls_, mm_objc_sel("arrowCursor"))), nullptr);
        return ret(current_cursor);
    }
    auto found = shapes.find(selector); if (found != shapes.end()) return ret(found->second);
    auto *o = make<cursor_object>(reinterpret_cast<Class>(cls_));
    const std::unordered_map<std::string, std::string> names = {
        {"arrowCursor","default"},{"IBeamCursor","text"},{"IBeamCursorForVerticalLayout","vertical-text"},
        {"crosshairCursor","crosshair"},{"closedHandCursor","grabbing"},{"openHandCursor","grab"},
        {"pointingHandCursor","pointer"},{"resizeLeftRightCursor","ew-resize"},{"resizeUpDownCursor","ns-resize"},
        {"resizeLeftCursor","w-resize"},{"resizeRightCursor","e-resize"},{"resizeUpCursor","n-resize"},{"resizeDownCursor","s-resize"},
        {"operationNotAllowedCursor","not-allowed"},{"dragCopyCursor","copy"},{"dragLinkCursor","alias"},
        {"contextualMenuCursor","context-menu"},{"disappearingItemCursor","not-allowed"}
    };
    if (auto it = names.find(selector); it != names.end()) o->specification.shape = it->second;
    shapes.emplace(selector, reinterpret_cast<id>(o)); return ret(o);
}
std::uint64_t cursor_push(id self, SEL) {
    id previous = obj(cursor_named(reinterpret_cast<id>(cursor_class), mm_objc_sel("currentCursor")));
    cursor_stack.push_back(retain(previous)); return cursor_set(self, nullptr);
}
std::uint64_t cursor_pop(id, SEL) {
    if (!cursor_stack.empty()) { id previous = cursor_stack.back(); cursor_stack.pop_back(); cursor_set(previous, nullptr); release(previous); } return 0;
}
std::uint64_t cursor_hide(id, SEL) { ++cursor_hide_count; apply_cursor(); return 0; }
std::uint64_t cursor_unhide(id, SEL) { if (cursor_hide_count) --cursor_hide_count; apply_cursor(); return 0; }
std::uint64_t cursor_hide_until(id, SEL, std::uint64_t value) { cursor_hidden_until_motion = value & 255; apply_cursor(); return 0; }
std::uint64_t cursor_init_image(id self, SEL, id image, double x, double y) {
    auto *o = as<cursor_object>(self); const auto previous = o->image; o->image = retain(image); release(previous);
    o->hotspot = {x, y}; o->specification = {};
    const auto name = image ? to_utf8(obj(send(image, "name"))) : std::string{};
    auto path = std::filesystem::path(name);
    if (!name.empty()) {
        if (!path.is_absolute()) {
            id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
            path = std::filesystem::path(to_utf8(obj(send(bundle, "resourcePath")))) / path;
        }
        auto pixels = read_application_icon(path);
        if (!pixels.argb.empty() && pixels.width <= 256 && pixels.height <= 256) {
            o->specification.pixels = std::move(pixels.argb); o->specification.width = pixels.width; o->specification.height = pixels.height;
            o->specification.hotspot_x = std::isfinite(x) ? std::clamp(int(x), 0, int(pixels.width) - 1) : 0;
            o->specification.hotspot_y = std::isfinite(y) ? std::clamp(int(y), 0, int(pixels.height) - 1) : 0;
        }
    }
    return ret(self);
}
mm_two_doubles cursor_hotspot(id self, SEL) { return {as<cursor_object>(self)->hotspot.x, as<cursor_object>(self)->hotspot.y}; }
std::uint64_t cursor_image(id self, SEL) { return ret(as<cursor_object>(self)->image); }

struct dragging_item_object {
    mm_objc_object base;
    id writer = nullptr, contents = nullptr;
    rect frame;
    ~dragging_item_object() { release(writer); release(contents); }
};
std::uint64_t dragging_item_init(id self, SEL, id writer) {
    auto *o = as<dragging_item_object>(self);
    id previous = o->writer;
    o->writer = retain(writer);
    release(previous);
    return ret(self);
}
std::uint64_t dragging_item_writer(id self, SEL) { return ret(as<dragging_item_object>(self)->writer); }
std::uint64_t dragging_item_set_frame(id self, SEL, double x, double y, double width, double height, id contents) {
    auto *o = as<dragging_item_object>(self);
    o->frame = {x, y, width, height};
    id previous = o->contents;
    o->contents = retain(contents);
    release(previous);
    return 0;
}
mm_two_doubles dragging_item_frame(id self, SEL) {
    const auto &frame = as<dragging_item_object>(self)->frame;
    mm_objc_return_doubles_hi(frame.w, frame.h);
    return {frame.x, frame.y};
}

/* ---- NSResponder --------------------------------------------------------- */

std::uint64_t responder_init(id self, SEL) { return ret(self); }
std::uint64_t responder_nextResponder(id self, SEL) { return ret(as<responder_object>(self)->next); }
std::uint64_t responder_setNextResponder(id self, SEL, id next) { as<responder_object>(self)->next = next; return 0; }
std::uint64_t responder_acceptsFirstResponder(id, SEL) { return 0; }
std::uint64_t responder_becomeFirstResponder(id, SEL) { return 1; }
std::uint64_t responder_resignFirstResponder(id, SEL) { return 1; }
std::uint64_t responder_performKeyEquivalent(id, SEL, id) { return 0; }

std::uint64_t responder_interpretKeyEvents(id self, SEL, id events) {
    const auto count = send(events, "count");
    for (std::uint64_t i = 0; i < count; ++i) {
        id event = obj(send(events, "objectAtIndex:", i));
        const auto code = send(event, "keyCode"), modifiers = send(event, "modifierFlags");
        const char *command = nullptr;
        switch (code) {
        case 36: case 76: command = "insertNewline:"; break;
        case 48: command = modifiers & (1ull << 17) ? "insertBacktab:" : "insertTab:"; break;
        case 51: command = "deleteBackward:"; break;
        case 117: command = "deleteForward:"; break;
        case 53: command = "cancelOperation:"; break;
        case 123: command = "moveLeft:"; break;
        case 124: command = "moveRight:"; break;
        case 125: command = "moveDown:"; break;
        case 126: command = "moveUp:"; break;
        case 115: command = "moveToBeginningOfDocument:"; break;
        case 119: command = "moveToEndOfDocument:"; break;
        case 116: command = "pageUp:"; break;
        case 121: command = "pageDown:"; break;
        }
        if (command) { send(self, "doCommandBySelector:", ret(mm_objc_sel(command))); continue; }
        if (modifiers & ((1ull << 18) | (1ull << 20))) continue;
        id text = obj(send(event, "characters"));
        const std::string utf8 = text ? to_utf8(text) : "";
        if (utf8.empty() || static_cast<unsigned char>(utf8.front()) < 0x20) continue;
        /* Function keys occupy Apple's private Unicode range; they are
         * commands, never text to insert into a document. */
        const auto first = send(text, "characterAtIndex:", 0);
        if (first >= 0xf700 && first <= 0xf8ff) continue;
        if (responds(self, "insertText:replacementRange:"))
            send(self, "insertText:replacementRange:", ret(text), 0x7fffffffffffffffULL, 0);
        else if (responds(self, "insertText:")) send(self, "insertText:", ret(text));
    }
    return 0;
}
std::uint64_t responder_doCommand(id self, SEL, SEL action) {
    if (!action) return 0;
    if (responds(self, action)) return mm_objc_send(self, action, 1, reinterpret_cast<const std::uint64_t *>(&self));
    id next = as<responder_object>(self)->next;
    if (next) return send(next, "doCommandBySelector:", ret(action));
    return 0;
}

/* Unhandled events and actions walk the chain; nothing at the end is not
 * an error, it is how AppKit reports "nobody wanted this". */
std::uint64_t responder_forward_event(id self, SEL sel, id event) {
    id next = as<responder_object>(self)->next;
    if (next) return mm_objc_send(next, mm_objc_sel_of(sel), 1, reinterpret_cast<const std::uint64_t *>(&event));
    return 0;
}
std::uint64_t responder_tryToPerform(id self, SEL, SEL action, id sender) {
    if (mm_objc_send(self, mm_objc_sel("respondsToSelector:"), 1, reinterpret_cast<const std::uint64_t *>(&action))) {
        const std::uint64_t arg = ret(sender);
        mm_objc_send(self, mm_objc_sel_of(action), 1, &arg);
        return 1;
    }
    id next = as<responder_object>(self)->next;
    if (!next) return 0;
    const std::uint64_t args[2] = {ret(action), ret(sender)};
    return mm_objc_send(next, mm_objc_sel("tryToPerform:with:"), 2, args);
}

/* ---- NSEvent ------------------------------------------------------------- */

std::uint64_t event_type(id self, SEL) { return as<event_object>(self)->type; }
std::uint64_t event_window(id self, SEL) { return ret(as<event_object>(self)->window); }
std::uint64_t event_modifierFlags(id self, SEL) { return as<event_object>(self)->modifiers; }
double event_timestamp(id self, SEL) { return as<event_object>(self)->timestamp; }
std::uint64_t event_clickCount(id self, SEL) { return as<event_object>(self)->click_count; }
std::uint64_t event_buttonNumber(id self, SEL) { return as<event_object>(self)->button; }
std::uint64_t event_keyCode(id self, SEL) { return as<event_object>(self)->keycode; }
std::uint64_t event_characters(id self, SEL) { return ret(string(as<event_object>(self)->characters)); }
std::uint64_t event_charactersIgnoringModifiers(id self, SEL) { return ret(string(as<event_object>(self)->characters_ignoring_modifiers)); }
std::uint64_t event_isARepeat(id self, SEL) { return as<event_object>(self)->repeat; }
mm_two_doubles event_locationInWindow(id self, SEL) { auto *e = as<event_object>(self); return {e->where.x, e->where.y}; }
double event_deltaX(id self, SEL) { return as<event_object>(self)->dx; }
double event_deltaY(id self, SEL) { return as<event_object>(self)->dy; }
double event_scrollingDeltaX(id self, SEL) { return as<event_object>(self)->dx; }
double event_scrollingDeltaY(id self, SEL) { return as<event_object>(self)->dy; }
std::uint64_t event_hasPreciseScrollingDeltas(id, SEL) { return 1; }
std::uint64_t event_pressedMouseButtons(id, SEL) { return g_pressed_buttons; }
std::uint64_t event_modifierFlagsClass(id, SEL) { return g_modifiers; }
mm_two_doubles event_mouseLocation(id, SEL) { return {g_mouse.x, g_mouse.y}; }
std::uint64_t event_addLocalMonitor(id, SEL, std::uint64_t, id handler) { return ret(handler); }
std::uint64_t event_removeMonitor(id, SEL, id) { return 0; }
/* ---- NSColor ------------------------------------------------------------- */

std::uint64_t color_make(id, SEL, double r, double g, double b, double a) {
    auto *c = make<color_object>(MM_FW_CLASS_REF(NSColor));
    c->r = r; c->g = g; c->b = b; c->a = a;
    return ret(autorelease(reinterpret_cast<id>(c)));
}
std::uint64_t color_white(id, SEL, double w, double a) { return color_make(nullptr, nullptr, w, w, w, a); }
double color_redComponent(id self, SEL) { return as<color_object>(self)->r; }
double color_greenComponent(id self, SEL) { return as<color_object>(self)->g; }
double color_blueComponent(id self, SEL) { return as<color_object>(self)->b; }
double color_alphaComponent(id self, SEL) { return as<color_object>(self)->a; }
std::uint64_t color_getRGBA(id self, SEL, double *r, double *g, double *b, double *a) {
    auto *c = as<color_object>(self);
    if (r) *r = c->r;
    if (g) *g = c->g;
    if (b) *b = c->b;
    if (a) *a = c->a;
    return 0;
}
std::uint64_t color_named(id cls, SEL) {
    /* +blackColor, +windowBackgroundColor, ...: one opaque colour each is
     * enough until something actually draws with them. */
    return color_make(cls, nullptr, 0.5, 0.5, 0.5, 1.0);
}
std::uint64_t color_clear(id cls, SEL) { return color_make(cls, nullptr, 0, 0, 0, 0); }
std::uint64_t color_set(id, SEL) { return 0; }

} // namespace

void application_cursor_moved() {
    if (cursor_hidden_until_motion) { cursor_hidden_until_motion = false; apply_cursor(); }
}

std::uint64_t begin_dragging_session(id view, SEL, id items, id event, id source) {
    if (!items || !source) return 0;
    finish_drag();
    id session = obj(send(reinterpret_cast<id>(dragging_session_class), "alloc"));
    auto *o = as<dragging_session_object>(session);
    o->items = retain(items); o->source = retain(source);
    o->window = retain(obj(send(view, "window")));
    o->location = g_mouse;
    o->button = send(event, "buttonNumber");
    active_drag = session;
    retain(session); // Keep the returned session alive across reentrant callbacks.
    drag_notify(session, "draggingSession:willBeginAtPoint:", o->location, false);
    return ret(autorelease(session));
}

bool dragging_session_event(id event) {
    if (!active_drag) return false;
    auto *o = as<dragging_session_object>(active_drag);
    const auto type = send(event, "type");
    if (type == ev_mouse_moved || type == ev_left_mouse_dragged || type == ev_right_mouse_dragged || type == ev_other_mouse_dragged) {
        o->location = g_mouse;
        id session = retain(active_drag);
        drag_notify(session, "draggingSession:movedToPoint:", o->location, false);
        release(session); return true;
    }
    if ((type == ev_left_mouse_up || type == ev_right_mouse_up || type == ev_other_mouse_up) && send(event, "buttonNumber") == o->button) {
        o->location = g_mouse; finish_drag(); return true;
    }
    if (type == ev_key_down && send(event, "keyCode") == 53) { finish_drag(); return true; }
    return false;
}
void cancel_dragging_session(id window) {
    if (active_drag && as<dragging_session_object>(active_drag)->window == window) finish_drag();
}

void register_responder() {
    Class nsobject = cls("NSObject");
    dragging_session_class = mm_objc_define_class("NSDraggingSession", nsobject, sizeof(dragging_session_object),
        MM_FW_CLASS_REF(NSDraggingSession), MM_FW_META_REF(NSDraggingSession));
    register_lifecycle<dragging_session_object>(dragging_session_class);
    MM_METHODS(dragging_session_class,
        {"draggingLocation", MM_IMP(drag_location), "{CGPoint=dd}16@0:8"},
        {"animatesToStartingPositionsOnCancelOrFail", MM_IMP(drag_animation), "B16@0:8"},
        {"setAnimatesToStartingPositionsOnCancelOrFail:", MM_IMP(drag_set_animation), "v24@0:8B16"});

    Class responder = mm_objc_define_class("NSResponder", nsobject, sizeof(responder_object),
                                           MM_FW_CLASS_REF(NSResponder), MM_FW_META_REF(NSResponder));
    register_lifecycle<responder_object>(responder);
    MM_METHODS(responder,
        {"init", MM_IMP(responder_init), "@16@0:8"},
        {"nextResponder", MM_IMP(responder_nextResponder), "@16@0:8"},
        {"setNextResponder:", MM_IMP(responder_setNextResponder), "v24@0:8@16"},
        {"acceptsFirstResponder", MM_IMP(responder_acceptsFirstResponder), "B16@0:8"},
        {"becomeFirstResponder", MM_IMP(responder_becomeFirstResponder), "B16@0:8"},
        {"resignFirstResponder", MM_IMP(responder_resignFirstResponder), "B16@0:8"},
        {"performKeyEquivalent:", MM_IMP(responder_performKeyEquivalent), "B24@0:8@16"},
        {"interpretKeyEvents:", MM_IMP(responder_interpretKeyEvents), "v24@0:8@16"},
        {"doCommandBySelector:", MM_IMP(responder_doCommand), "v24@0:8:16"},
        {"tryToPerform:with:", MM_IMP(responder_tryToPerform), "B32@0:8:16@24"});
    for (const char *sel : {"mouseDown:", "mouseUp:", "mouseDragged:", "mouseMoved:", "rightMouseDown:", "rightMouseUp:",
                            "rightMouseDragged:", "otherMouseDown:", "otherMouseUp:", "otherMouseDragged:", "scrollWheel:",
                            "keyDown:", "keyUp:", "flagsChanged:", "mouseEntered:", "mouseExited:", "cursorUpdate:"})
        mm_objc_add_method(responder, sel, MM_IMP(responder_forward_event), "v24@0:8@16");

    Class event = mm_objc_define_class("NSEvent", nsobject, sizeof(event_object), MM_FW_CLASS_REF(NSEvent), MM_FW_META_REF(NSEvent));
    register_lifecycle<event_object>(event);
    MM_METHODS(event,
        {"type", MM_IMP(event_type), "Q16@0:8"},
        {"window", MM_IMP(event_window), "@16@0:8"},
        {"modifierFlags", MM_IMP(event_modifierFlags), "Q16@0:8"},
        {"timestamp", MM_IMP(event_timestamp), "d16@0:8"},
        {"clickCount", MM_IMP(event_clickCount), "q16@0:8"},
        {"buttonNumber", MM_IMP(event_buttonNumber), "q16@0:8"},
        {"keyCode", MM_IMP(event_keyCode), "S16@0:8"},
        {"characters", MM_IMP(event_characters), "@16@0:8"},
        {"charactersIgnoringModifiers", MM_IMP(event_charactersIgnoringModifiers), "@16@0:8"},
        {"isARepeat", MM_IMP(event_isARepeat), "B16@0:8"},
        {"locationInWindow", MM_IMP(event_locationInWindow), "{CGPoint=dd}16@0:8"},
        {"deltaX", MM_IMP(event_deltaX), "d16@0:8"},
        {"deltaY", MM_IMP(event_deltaY), "d16@0:8"},
        {"scrollingDeltaX", MM_IMP(event_scrollingDeltaX), "d16@0:8"},
        {"scrollingDeltaY", MM_IMP(event_scrollingDeltaY), "d16@0:8"},
        {"hasPreciseScrollingDeltas", MM_IMP(event_hasPreciseScrollingDeltas), "B16@0:8"});
    MM_CLASS_METHODS(event,
        {"pressedMouseButtons", MM_IMP(event_pressedMouseButtons), "Q16@0:8"},
        {"modifierFlags", MM_IMP(event_modifierFlagsClass), "Q16@0:8"},
        {"mouseLocation", MM_IMP(event_mouseLocation), "{CGPoint=dd}16@0:8"},
        {"addLocalMonitorForEventsMatchingMask:handler:", MM_IMP(event_addLocalMonitor), "@32@0:8Q16@?24"},
        {"removeMonitor:", MM_IMP(event_removeMonitor), "v24@0:8@16"});

    Class color = mm_objc_define_class("NSColor", nsobject, sizeof(color_object), MM_FW_CLASS_REF(NSColor), MM_FW_META_REF(NSColor));
    register_lifecycle<color_object>(color);
    MM_METHODS(color,
        {"redComponent", MM_IMP(color_redComponent), "d16@0:8"},
        {"greenComponent", MM_IMP(color_greenComponent), "d16@0:8"},
        {"blueComponent", MM_IMP(color_blueComponent), "d16@0:8"},
        {"alphaComponent", MM_IMP(color_alphaComponent), "d16@0:8"},
        {"getRed:green:blue:alpha:", MM_IMP(color_getRGBA), "v48@0:8^d16^d24^d32^d40"},
        {"set", MM_IMP(color_set), "v16@0:8"},
        {"setFill", MM_IMP(color_set), "v16@0:8"},
        {"setStroke", MM_IMP(color_set), "v16@0:8"});
    MM_CLASS_METHODS(color,
        {"colorWithCalibratedRed:green:blue:alpha:", MM_IMP(color_make), "@48@0:8d16d24d32d40"},
        {"colorWithDeviceRed:green:blue:alpha:", MM_IMP(color_make), "@48@0:8d16d24d32d40"},
        {"colorWithSRGBRed:green:blue:alpha:", MM_IMP(color_make), "@48@0:8d16d24d32d40"},
        {"colorWithRed:green:blue:alpha:", MM_IMP(color_make), "@48@0:8d16d24d32d40"},
        {"colorWithCalibratedWhite:alpha:", MM_IMP(color_white), "@32@0:8d16d24"},
        {"colorWithDeviceWhite:alpha:", MM_IMP(color_white), "@32@0:8d16d24"},
        {"clearColor", MM_IMP(color_clear), "@16@0:8"});
    for (const char *sel : {"blackColor", "whiteColor", "grayColor", "redColor", "greenColor", "blueColor",
                            "windowBackgroundColor", "controlColor", "textColor", "highlightColor", "shadowColor",
                            "selectedTextBackgroundColor", "controlTextColor", "labelColor"})
        mm_objc_add_class_method(color, sel, MM_IMP(color_named), "@16@0:8");

    /* Classes the app only passes around so far; they exist so its
     * references resolve and `isKindOfClass:` works. */
    struct passthrough { const char *name; Class storage, meta; };
    const passthrough passthroughs[] = {
        {"NSColorSpace", MM_FW_CLASS_REF(NSColorSpace), MM_FW_META_REF(NSColorSpace)},
        {"NSAppearance", MM_FW_CLASS_REF(NSAppearance), MM_FW_META_REF(NSAppearance)},
        {"NSTrackingArea", MM_FW_CLASS_REF(NSTrackingArea), MM_FW_META_REF(NSTrackingArea)},
        {"NSPasteboard", MM_FW_CLASS_REF(NSPasteboard), MM_FW_META_REF(NSPasteboard)},
        {"NSPasteboardItem", MM_FW_CLASS_REF(NSPasteboardItem), MM_FW_META_REF(NSPasteboardItem)},
        {"NSTextInputContext", MM_FW_CLASS_REF(NSTextInputContext), MM_FW_META_REF(NSTextInputContext)},
        {"NSInputManager", MM_FW_CLASS_REF(NSInputManager), MM_FW_META_REF(NSInputManager)},
        {"NSAccessibilityElement", MM_FW_CLASS_REF(NSAccessibilityElement), MM_FW_META_REF(NSAccessibilityElement)},
    };
    for (const auto &p : passthroughs)
        mm_objc_define_class(p.name, nsobject, sizeof(mm_objc_object), p.storage, p.meta);

    Class dragging = mm_objc_define_class("NSDraggingItem", nsobject, sizeof(dragging_item_object),
                                         MM_FW_CLASS_REF(NSDraggingItem), MM_FW_META_REF(NSDraggingItem));
    register_lifecycle<dragging_item_object>(dragging);
    MM_METHODS(dragging,
        {"initWithPasteboardWriter:", MM_IMP(dragging_item_init), "@24@0:8@16"},
        {"item", MM_IMP(dragging_item_writer), "@16@0:8"},
        {"setDraggingFrame:contents:", MM_IMP(dragging_item_set_frame), "v56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48"},
        {"draggingFrame", MM_IMP(dragging_item_frame), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"});

    /* NSTrackingArea: a rectangle a view wants mouse events in. The
     * backend delivers moves to the whole window, so the area only has to
     * exist and remember what it was made with. */
    Class tracking = cls("NSTrackingArea");
    static auto tracking_init = [](id self, SEL, double, double, double, double,
                                   std::uint64_t, id, id) -> std::uint64_t { return ret(self); };
    static auto tracking_nil = [](id, SEL) -> std::uint64_t { return 0; };
    MM_METHODS(tracking,
        {"initWithRect:options:owner:userInfo:", MM_IMP(+tracking_init),
         "@72@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16Q48@56@64"},
        {"owner", MM_IMP(+tracking_nil), "@16@0:8"},
        {"userInfo", MM_IMP(+tracking_nil), "@16@0:8"});

    /* NSColorSpace: one shared instance per named space. Everything here
     * draws in sRGB, so the spaces differ only by identity — which is all
     * a program does with them (it asks for one and hands it to a window
     * or an image). */
    Class colorspace = cls("NSColorSpace");
    static auto colorspace_shared = [](id cls_, SEL sel) -> std::uint64_t {
        static std::mutex mu;
        static std::unordered_map<std::string, id> spaces;
        std::lock_guard lock(mu);
        auto it = spaces.find(sel);
        if (it == spaces.end()) it = spaces.emplace(sel, mm_objc_alloc(reinterpret_cast<Class>(cls_), 0)).first;
        return ret(it->second);
    };
    for (const char *sel : {"sRGBColorSpace", "genericRGBColorSpace", "deviceRGBColorSpace",
                            "genericGrayColorSpace", "deviceGrayColorSpace", "displayP3ColorSpace",
                            "adobeRGB1998ColorSpace", "extendedSRGBColorSpace"})
        mm_objc_add_class_method(colorspace, sel, MM_IMP(+colorspace_shared), "@16@0:8");
    static auto colorspace_name = [](id, SEL) -> std::uint64_t { return ret(string("sRGB IEC61966-2.1")); };
    static auto colorspace_model = [](id, SEL) -> std::uint64_t { return 1; }; /* NSColorSpaceModelRGB */
    MM_METHODS(colorspace,
        {"localizedName", MM_IMP(+colorspace_name), "@16@0:8"},
        {"colorSpaceModel", MM_IMP(+colorspace_model), "q16@0:8"},
        {"numberOfColorComponents", MM_IMP(+colorspace_model), "q16@0:8"});

    cursor_class = mm_objc_define_class("NSCursor", nsobject, sizeof(cursor_object), MM_FW_CLASS_REF(NSCursor), MM_FW_META_REF(NSCursor));
    register_lifecycle<cursor_object>(cursor_class);
    for (const char *selector : {"arrowCursor", "IBeamCursor", "IBeamCursorForVerticalLayout", "crosshairCursor", "closedHandCursor", "openHandCursor",
        "pointingHandCursor", "resizeLeftRightCursor", "resizeUpDownCursor", "resizeLeftCursor", "resizeRightCursor", "resizeUpCursor", "resizeDownCursor",
        "operationNotAllowedCursor", "dragCopyCursor", "dragLinkCursor", "contextualMenuCursor", "disappearingItemCursor", "currentCursor", "currentSystemCursor"})
        mm_objc_add_class_method(cursor_class, selector, MM_IMP(cursor_named), "@16@0:8");
    MM_CLASS_METHODS(cursor_class,
        {"hide", MM_IMP(cursor_hide), "v16@0:8"}, {"unhide", MM_IMP(cursor_unhide), "v16@0:8"},
        {"pop", MM_IMP(cursor_pop), "v16@0:8"}, {"setHiddenUntilMouseMoves:", MM_IMP(cursor_hide_until), "v24@0:8B16"});
    MM_METHODS(cursor_class,
        {"set", MM_IMP(cursor_set), "v16@0:8"}, {"push", MM_IMP(cursor_push), "v16@0:8"}, {"pop", MM_IMP(cursor_pop), "v16@0:8"},
        {"initWithImage:hotSpot:", MM_IMP(cursor_init_image), "@40@0:8@16{CGPoint=dd}24"},
        {"hotSpot", MM_IMP(cursor_hotspot), "{CGPoint=dd}16@0:8"}, {"image", MM_IMP(cursor_image), "@16@0:8"});

    /* NSImage pixels are decoded once, on demand, by the Cocoa renderer. */
    Class image = mm_objc_define_class("NSImage", nsobject, sizeof(image_object), MM_FW_CLASS_REF(NSImage), MM_FW_META_REF(NSImage));
    register_lifecycle<image_object>(image);
    static auto image_named = [](id cls_, SEL, id name) -> std::uint64_t {
        auto *o = make<image_object>(reinterpret_cast<Class>(cls_));
        o->name = name ? to_utf8(name) : "";
        return ret(autorelease(reinterpret_cast<id>(o)));
    };
    static auto image_initWithContentsOfFile = [](id self, SEL, id path) -> std::uint64_t {
        as<image_object>(self)->name = path ? to_utf8(is_kind(path,cls("NSURL")) ? obj(send(path,"path")) : path) : "";
#ifdef MM_APPKIT_EVILCOCOA
        as<image_object>(self)->decoded=false;as<image_object>(self)->pixels.reset();
#endif
        return ret(self);
    };
    static auto image_initWithSize = [](id self, SEL, double w, double h) -> std::uint64_t {
        as<image_object>(self)->size = {w, h};
        return ret(self);
    };
    static auto image_size = [](id self, SEL) -> mm_two_doubles {
#ifdef MM_APPKIT_EVILCOCOA
        decode_cocoa_image(self);
#endif
        auto *o = as<image_object>(self);
        return {o->size.w, o->size.h};
    };
    static auto image_setSize = [](id self, SEL, double w, double h) -> std::uint64_t {
        as<image_object>(self)->size = {w, h};
        return 0;
    };
    static auto image_name = [](id self, SEL) -> std::uint64_t { return ret(string(as<image_object>(self)->name)); };
    static auto image_noop = [](id, SEL) -> std::uint64_t { return 0; };
    static auto image_valid = [](id self, SEL) -> std::uint64_t {
#ifdef MM_APPKIT_EVILCOCOA
        decode_cocoa_image(self);return as<image_object>(self)->pixels!=nullptr;
#else
        return !as<image_object>(self)->name.empty();
#endif
    };
    MM_CLASS_METHODS(image, {"imageNamed:", MM_IMP(+image_named), "@24@0:8@16"});
    MM_METHODS(image,
        {"initWithContentsOfFile:", MM_IMP(+image_initWithContentsOfFile), "@24@0:8@16"},
        // Referenced images keep the filename and defer decoding until use.
        {"initByReferencingFile:", MM_IMP(+image_initWithContentsOfFile), "@24@0:8@16"},
        {"initWithContentsOfURL:", MM_IMP(+image_initWithContentsOfFile), "@24@0:8@16"},
        {"initWithSize:", MM_IMP(+image_initWithSize), "@32@0:8{CGSize=dd}16"},
        {"size", MM_IMP(+image_size), "{CGSize=dd}16@0:8"},
        {"setSize:", MM_IMP(+image_setSize), "v32@0:8{CGSize=dd}16"},
        {"name", MM_IMP(+image_name), "@16@0:8"},
        {"setTemplate:", MM_IMP(+image_noop), "v24@0:8B16"},
        {"isValid", MM_IMP(+image_valid), "B16@0:8"});

    /* NSAppearance: one appearance, named by whatever was asked for. */
    Class appearance = cls("NSAppearance");
    static auto appearance_named = [](id cls_, SEL, id) -> std::uint64_t {
        static id instance = nullptr;
        if (!instance) instance = mm_objc_alloc(reinterpret_cast<Class>(cls_), 0);
        return ret(instance);
    };
    static auto appearance_current = [](id cls_, SEL) -> std::uint64_t { return appearance_named(cls_, nullptr, nullptr); };
    static auto appearance_name = [](id, SEL) -> std::uint64_t { return ret(string("NSAppearanceNameAqua")); };
    MM_CLASS_METHODS(appearance,
        {"appearanceNamed:", MM_IMP(+appearance_named), "@24@0:8@16"},
        {"currentAppearance", MM_IMP(+appearance_current), "@16@0:8"},
        {"currentDrawingAppearance", MM_IMP(+appearance_current), "@16@0:8"});
    MM_METHODS(appearance, {"name", MM_IMP(+appearance_name), "@16@0:8"});
}

} // namespace ak
