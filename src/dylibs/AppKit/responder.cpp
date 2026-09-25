/* NSResponder and the small value classes around it: NSEvent, NSColor,
 * NSCursor, NSAppearance, NSTrackingArea, NSPasteboard.
 *
 * NSResponder is where the responder chain lives: unhandled events walk
 * `-nextResponder` until something implements the action, which is how
 * AppKit apps expect key handling and menu actions to work. */
#include "appkit.h"

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

namespace ak {

namespace {

/* ---- NSResponder --------------------------------------------------------- */

std::uint64_t responder_init(id self, SEL) { return ret(self); }
std::uint64_t responder_nextResponder(id self, SEL) { return ret(as<responder_object>(self)->next); }
std::uint64_t responder_setNextResponder(id self, SEL, id next) { as<responder_object>(self)->next = next; return 0; }
std::uint64_t responder_acceptsFirstResponder(id, SEL) { return 0; }
std::uint64_t responder_becomeFirstResponder(id, SEL) { return 1; }
std::uint64_t responder_resignFirstResponder(id, SEL) { return 1; }

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

void register_responder() {
    Class nsobject = cls("NSObject");

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
        {"charactersIgnoringModifiers", MM_IMP(event_characters), "@16@0:8"},
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
        {"mouseLocation", MM_IMP(event_mouseLocation), "{CGPoint=dd}16@0:8"});

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
        {"NSCursor", MM_FW_CLASS_REF(NSCursor), MM_FW_META_REF(NSCursor)},
        {"NSAppearance", MM_FW_CLASS_REF(NSAppearance), MM_FW_META_REF(NSAppearance)},
        {"NSTrackingArea", MM_FW_CLASS_REF(NSTrackingArea), MM_FW_META_REF(NSTrackingArea)},
        {"NSPasteboard", MM_FW_CLASS_REF(NSPasteboard), MM_FW_META_REF(NSPasteboard)},
        {"NSPasteboardItem", MM_FW_CLASS_REF(NSPasteboardItem), MM_FW_META_REF(NSPasteboardItem)},
        {"NSTextInputContext", MM_FW_CLASS_REF(NSTextInputContext), MM_FW_META_REF(NSTextInputContext)},
        {"NSInputManager", MM_FW_CLASS_REF(NSInputManager), MM_FW_META_REF(NSInputManager)},
        {"NSAccessibilityElement", MM_FW_CLASS_REF(NSAccessibilityElement), MM_FW_META_REF(NSAccessibilityElement)},
        {"NSDraggingItem", MM_FW_CLASS_REF(NSDraggingItem), MM_FW_META_REF(NSDraggingItem)},
    };
    for (const auto &p : passthroughs)
        mm_objc_define_class(p.name, nsobject, sizeof(mm_objc_object), p.storage, p.meta);

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

    /* NSCursor: one shared instance per shape. Nothing changes the pointer
     * yet, but an app asks for these while building its UI. */
    Class cursor = cls("NSCursor");
    static auto cursor_shared = [](id cls_, SEL sel) -> std::uint64_t {
        static std::mutex mu;
        static std::unordered_map<std::string, id> shapes;
        std::lock_guard lock(mu);
        auto it = shapes.find(sel);
        if (it == shapes.end()) it = shapes.emplace(sel, mm_objc_alloc(reinterpret_cast<Class>(cls_), 0)).first;
        return ret(it->second);
    };
    static auto cursor_noop = [](id, SEL) -> std::uint64_t { return 0; };
    static auto cursor_init = [](id self, SEL, id, double, double) -> std::uint64_t { return ret(self); };
    static auto cursor_hotspot = [](id, SEL) -> mm_two_doubles { return {0, 0}; };
    for (const char *sel : {"arrowCursor", "IBeamCursor", "crosshairCursor", "closedHandCursor", "openHandCursor",
                            "pointingHandCursor", "resizeLeftRightCursor", "resizeUpDownCursor", "resizeLeftCursor",
                            "resizeRightCursor", "resizeUpCursor", "resizeDownCursor", "operationNotAllowedCursor",
                            "dragCopyCursor", "dragLinkCursor", "contextualMenuCursor", "disappearingItemCursor",
                            "currentCursor", "currentSystemCursor"})
        mm_objc_add_class_method(cursor, sel, MM_IMP(+cursor_shared), "@16@0:8");
    for (const char *sel : {"hide", "unhide", "pop", "setHiddenUntilMouseMoves:"})
        mm_objc_add_class_method(cursor, sel, MM_IMP(+cursor_noop), "v16@0:8");
    MM_METHODS(cursor,
        {"set", MM_IMP(+cursor_noop), "v16@0:8"},
        {"push", MM_IMP(+cursor_noop), "v16@0:8"},
        {"pop", MM_IMP(+cursor_noop), "v16@0:8"},
        {"initWithImage:hotSpot:", MM_IMP(+cursor_init), "@40@0:8@16{CGPoint=dd}24"},
        {"hotSpot", MM_IMP(+cursor_hotspot), "{CGPoint=dd}16@0:8"});

    /* NSImage: a named or file-backed image with a size, enough for an app
     * to lay out around it. Nothing decodes pixels yet. */
    Class image = mm_objc_define_class("NSImage", nsobject, sizeof(image_object), MM_FW_CLASS_REF(NSImage), MM_FW_META_REF(NSImage));
    register_lifecycle<image_object>(image);
    static auto image_named = [](id cls_, SEL, id name) -> std::uint64_t {
        auto *o = make<image_object>(reinterpret_cast<Class>(cls_));
        o->name = name ? to_utf8(name) : "";
        return ret(autorelease(reinterpret_cast<id>(o)));
    };
    static auto image_initWithContentsOfFile = [](id self, SEL, id path) -> std::uint64_t {
        as<image_object>(self)->name = path ? to_utf8(path) : "";
        return ret(self);
    };
    static auto image_initWithSize = [](id self, SEL, double w, double h) -> std::uint64_t {
        as<image_object>(self)->size = {w, h};
        return ret(self);
    };
    static auto image_size = [](id self, SEL) -> mm_two_doubles {
        auto *o = as<image_object>(self);
        return {o->size.w, o->size.h};
    };
    static auto image_setSize = [](id self, SEL, double w, double h) -> std::uint64_t {
        as<image_object>(self)->size = {w, h};
        return 0;
    };
    static auto image_name = [](id self, SEL) -> std::uint64_t { return ret(string(as<image_object>(self)->name)); };
    static auto image_noop = [](id, SEL) -> std::uint64_t { return 0; };
    MM_CLASS_METHODS(image, {"imageNamed:", MM_IMP(+image_named), "@24@0:8@16"});
    MM_METHODS(image,
        {"initWithContentsOfFile:", MM_IMP(+image_initWithContentsOfFile), "@24@0:8@16"},
        {"initWithContentsOfURL:", MM_IMP(+image_initWithContentsOfFile), "@24@0:8@16"},
        {"initWithSize:", MM_IMP(+image_initWithSize), "@32@0:8{CGSize=dd}16"},
        {"size", MM_IMP(+image_size), "{CGSize=dd}16@0:8"},
        {"setSize:", MM_IMP(+image_setSize), "v32@0:8{CGSize=dd}16"},
        {"name", MM_IMP(+image_name), "@16@0:8"},
        {"setTemplate:", MM_IMP(+image_noop), "v24@0:8B16"},
        {"isValid", MM_IMP(+image_noop), "B16@0:8"});

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
