/* Loading a nib.
 *
 * A modern .nib is a directory holding `keyedobjects.nib`, which is a
 * binary property list written by NSKeyedArchiver: `$objects` is a flat
 * table, every reference into it is a UID, and each entry carries the name
 * of the class that archived it. Decoding one therefore needs no new file
 * format — Foundation already parses binary plists, and UIDs come back as
 * objects of their own class (see `uid_create` in plists.cpp).
 *
 * What is rebuilt here is what a nib means at runtime: the objects, the
 * menu tree, the windows, and the connections between them. An outlet
 * connection assigns the destination to the source's instance variable
 * (or its setter); an action connection sets the control's target and
 * action. This is how an app that never sets an application delegate in
 * code — Ableton Live is one — gets one, and with it the rest of its
 * startup.
 *
 * Objects whose class is not modelled become instances of that class if
 * the program defines it, and nothing otherwise; a nib is a description of
 * a UI, so a missing piece costs that piece and not the load. */
#include "appkit.h"
#include "nib_archive.h"

#include <algorithm>
#include <cstdio>
#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

/* libobjc's Darwin-named runtime, for the instance variable an outlet
 * assigns to when the class has no setter for it. */
extern "C" {
struct mm_ivar;
const mm_ivar *mm_nib_class_getInstanceVariable(mm_objc_class *cls, const char *name) __asm__("_class_getInstanceVariable");
void mm_nib_object_setIvar(mm_id obj, const mm_ivar *iv, mm_id value) __asm__("_object_setIvar");
}

namespace ak {
namespace {

namespace fs = std::filesystem;

/* ---- the archive --------------------------------------------------------- */

/* Objects are decoded on demand and remembered, because the table is a
 * graph: a menu's items point back at the menu. */
class unarchiver {
public:
    unarchiver(id objects, id owner, std::string table = {})
        : objects_(objects), owner_(owner), localization_table_(std::move(table)) {}

    id decode(id ref, int depth = 0);
    const std::vector<id> &top_level() const { return top_level_; }
    void note_top_level(id o) {
        if (o && o != owner_ && o != g_NSApp &&
            std::find(top_level_.begin(), top_level_.end(), o) == top_level_.end())
            top_level_.push_back(o);
    }

private:
    id entry(std::uint64_t index) {
        if (index >= count()) return nullptr;
        return obj(send(objects_, "objectAtIndex:", index));
    }
    std::uint64_t count() const { return send(objects_, "count"); }

    id build(id dict, std::uint64_t index, int depth);
    id build_custom_object(id dict, int depth);
    id build_menu(id dict, int depth);
    id build_menu_item(id dict, int depth);
    id build_window(id dict, int depth);
    id build_view(id dict, const std::string &klass, int depth, std::uint64_t index);
    void apply_connection(id dict, int depth);

    /* A dictionary value, already decoded. */
    id value(id dict, const char *key, int depth) {
        id v = obj(send(dict, "objectForKey:", ret(string(key))));
        return v ? decode(v, depth + 1) : nullptr;
    }
    std::string string_value(id dict, const char *key, int depth) {
        id v = value(dict, key, depth);
        return v ? to_utf8(v) : std::string();
    }
    std::int64_t int_value(id dict, const char *key, int depth, std::int64_t fallback = 0) {
        id v = value(dict, key, depth);
        return v ? static_cast<std::int64_t>(send(v, "longLongValue")) : fallback;
    }

    double real_value(id dict, const char *key, int depth, double fallback = 0) {
        id v = value(dict,key,depth); if(!v) return fallback;
        send(v,"doubleValue"); double result=fallback;
        if(auto *ctx=api()->current_context())std::memcpy(&result,&ctx->v[0][0],8);
        return result;
    }
    id objects_;
    id owner_;
    std::string localization_table_;
    std::unordered_map<std::uint64_t, id> built_;
    std::vector<id> top_level_;
    std::vector<id> awake_;
public:
    const std::vector<id> &awake_list() const { return awake_; }
};

bool is_uid(id o) {
    static mm_objc_class *uid = mm_objc_class_named("__NSCFKeyedArchiverUID");
    return o && uid && mm_objc_object_class(o) == uid;
}

/* The archived class name: $class is a UID of a dictionary whose
 * $classname is it. */
std::string class_name_of(unarchiver &u, id dict, int depth);

id unarchiver::decode(id ref, int depth) {
    if (!ref || depth > 64) return nullptr;
    if (!is_uid(ref)) return ref; /* already a value: a string, a number */
    const auto index = static_cast<std::uint64_t>(send(ref, "value"));
    if (index == 0) return nullptr; /* $objects[0] is "$null" */
    if (auto it = built_.find(index); it != built_.end()) return it->second;
    id e = entry(index);
    if (!e) return nullptr;
    if (!is_kind(e, cls("NSDictionary"))) { built_[index] = e; return e; }
    id result = build(e, index, depth);
    built_[index] = result;
    return result;
}

std::string class_name_of(unarchiver &u, id dict, int depth) {
    id klass = obj(send(dict, "objectForKey:", ret(string("$class"))));
    if (!klass) return {};
    id d = u.decode(klass, depth + 1);
    if (!d || !is_kind(d, cls("NSDictionary"))) return {};
    id name = obj(send(d, "objectForKey:", ret(string("$classname"))));
    return name ? to_utf8(name) : std::string();
}

/* The elements of an archived NSArray/NSSet. */
std::vector<id> ns_objects(unarchiver &u, id dict, int depth) {
    std::vector<id> out;
    id array = obj(send(dict, "objectForKey:", ret(string("NS.objects"))));
    if (!array) return out;
    const auto n = static_cast<std::uint64_t>(send(array, "count"));
    for (std::uint64_t i = 0; i < n; i++)
        if (id v = u.decode(obj(send(array, "objectAtIndex:", i)), depth + 1)) out.push_back(v);
    return out;
}

id unarchiver::build(id dict, std::uint64_t index, int depth) {
    (void)index;
    const std::string klass = class_name_of(*this, dict, depth);
    if (trace()) std::fprintf(stderr, "AppKit: nib: decoding %s\n", klass.empty() ? "(plain dictionary)" : klass.c_str());
    if (klass.empty()) return dict; /* a plain dictionary */

    if (klass == "NSLocalizableString") {
        id fallback = obj(send(dict, "objectForKey:", ret(string("NS.string"))));
        id key = value(dict, "NSKey", depth);
        if (!key) return fallback;
        id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
        return obj(send(bundle, "localizedStringForKey:value:table:", ret(key), ret(fallback),
                        ret(string(localization_table_))));
    }
    if (klass == "NSMutableString" || klass == "NSString")
        return obj(send(dict, "objectForKey:", ret(string("NS.string"))));
    if (klass == "NSMutableData" || klass == "NSData")
        return obj(send(dict, "objectForKey:", ret(string("NS.data"))));
    // Archived table templates are embedded nib bytes, decoded afresh for
    // each makeViewWithIdentifier:owner: request.
    if (klass == "NSNib") return value(dict, "NSNibFileData", depth);
    if (klass == "NSArray" || klass == "NSMutableArray" || klass == "NSSet" || klass == "NSMutableSet") {
        std::vector<id> items = ns_objects(*this, dict, depth);
        id a = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
        for (id v : items) send(a, "addObject:", ret(v));
        return a;
    }
    if (klass == "NSDictionary" || klass == "NSMutableDictionary") {
        id keys = obj(send(dict, "objectForKey:", ret(string("NS.keys"))));
        id vals = obj(send(dict, "objectForKey:", ret(string("NS.objects"))));
        id d = obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary"));
        const auto n = keys ? static_cast<std::uint64_t>(send(keys, "count")) : 0;
        for (std::uint64_t i = 0; i < n; i++) {
            id k = decode(obj(send(keys, "objectAtIndex:", i)), depth + 1);
            id v = decode(obj(send(vals, "objectAtIndex:", i)), depth + 1);
            if (k && v) send(d, "setObject:forKey:", ret(v), ret(k));
        }
        return d;
    }
    if (klass == "NSLayoutConstraint") return dict;
    if (klass == "NSClassSwapper") return build_view(dict, klass, depth, index);
    if (klass == "NSFont") {
        const auto name=string_value(dict,"NSName",depth);double size=real_value(dict,"NSSize",depth,13);
        if(auto *ctx=api()->current_context())std::memcpy(&ctx->v[0][0],&size,8);
        return obj(send(reinterpret_cast<id>(cls("NSFont")),"fontWithName:size:",ret(string(name))));
    }
    if (klass == "NSTableColumn") {
        id o=obj(send(reinterpret_cast<id>(cls("NSTableColumn")),"alloc"));built_[index]=o;
        auto *column=as<table_column_object>(o);column->identifier=retain(value(dict,"NSIdentifier",depth));column->width=real_value(dict,"NSWidth",depth,120);
        column->minimum_width=real_value(dict,"NSMinWidth",depth,10);
        column->maximum_width=std::max(column->minimum_width,real_value(dict,"NSMaxWidth",depth,1000));
        column->resizing_mask=static_cast<std::uint64_t>(int_value(dict,"NSResizingMask",depth,1));
        return o;
    }
    if (klass == "NSCustomObject") return build_custom_object(dict, depth);
    if (klass == "NSMenu" || klass == "NSMenuTemplate") return build_menu(dict, depth);
    if (klass == "NSMenuItem") return build_menu_item(dict, depth);
    if (klass == "NSWindowTemplate") return build_window(dict, depth);
    if (klass == "NSCustomView" || klass == "NSView") return build_view(dict, klass, depth, index);
    if (klass == "NSNibOutletConnector" || klass == "NSNibControlConnector" || klass == "NSNibConnector") {
        apply_connection(dict, depth);
        return nullptr;
    }
    if (klass == "NSIBObjectData") {
        /* The root: the objects, then the connections between them (the
         * connectors apply themselves as they are decoded), then the
         * windows the nib says are visible. */
        id root = value(dict, "NSRoot", depth);
        id keys_ref = obj(send(dict, "objectForKey:", ret(string("NSObjectsKeys"))));
        id parents_ref = obj(send(dict, "objectForKey:", ret(string("NSObjectsValues"))));
        if (keys_ref && parents_ref) {
            /* These parallel arrays encode object -> parent, not a list
             * of owned objects. Releasing the values frees the same parent
             * repeatedly, including File's Owner and cached table cells.
             * Walk archived slots: decoding an NSArray drops nil objects
             * (First Responder), which would misalign the parent pairs. */
            id keys_dict = is_uid(keys_ref) ? entry(send(keys_ref, "value")) : keys_ref;
            id parents_dict = is_uid(parents_ref) ? entry(send(parents_ref, "value")) : parents_ref;
            id keys = obj(send(keys_dict, "objectForKey:", ret(string("NS.objects"))));
            id parents = obj(send(parents_dict, "objectForKey:", ret(string("NS.objects"))));
            const auto n = std::min(send(keys, "count"), send(parents, "count"));
            for (std::uint64_t i = 0; i < n; ++i) {
                id object = decode(obj(send(keys, "objectAtIndex:", i)), depth + 1);
                id parent = decode(obj(send(parents, "objectAtIndex:", i)), depth + 1);
                if (parent == root) note_top_level(object);
            }
        } else if (id objects = value(dict, "NSObjectsValues", depth)) {
            /* Older minimal archives can contain only an object list. */
            const auto n = send(objects, "count");
            for (std::uint64_t i = 0; i < n; ++i) note_top_level(obj(send(objects, "objectAtIndex:", i)));
        }
        value(dict, "NSConnections", depth);
        if (id visible = value(dict, "NSVisibleWindows", depth)) {
            const auto n = static_cast<std::uint64_t>(send(visible, "count"));
            for (std::uint64_t i = 0; i < n; i++)
                send(obj(send(visible, "objectAtIndex:", i)), "makeKeyAndOrderFront:", 0);
        }
        return nullptr;
    }
    if (klass == "NSCustomResource") {
        /* An image referenced by name; NSImage resolves it from the bundle. */
        const std::string name = string_value(dict, "NSResourceName", depth);
        Class image = cls("NSImage");
        return image && !name.empty() ? obj(send(reinterpret_cast<id>(image), "imageNamed:", ret(string(name)))) : nullptr;
    }

    if (Class c = cls(klass.c_str())) {
        for (Class base = c; base; base = base->superclass)
            if (base == cls("NSView")) return build_view(dict, klass, depth, index);
    }

    /* Anything else: the program's own class, if it has one. */
    if (Class c = cls(klass.c_str())) {
        id o = obj(send(reinterpret_cast<id>(c), "alloc"));
        if (o && responds(o, "init")) o = obj(send(o, "init"));
        if (o) awake_.push_back(o);
        return o;
    }
    if (trace()) std::fprintf(stderr, "AppKit: nib: no class %s\n", klass.c_str());
    return nullptr;
}

/* File's Owner and the other placeholders are NSCustomObjects; the owner
 * is the one whose archived class is the owner's own class (or a class it
 * inherits from), which is how Interface Builder records it. */
id unarchiver::build_custom_object(id dict, int depth) {
    const std::string name = string_value(dict, "NSClassName", depth);
    if (name.empty()) return nullptr;
    Class c = cls(name.c_str());
    if (owner_ && c && is_kind(owner_, c)) return owner_;
    if (name == "NSApplication" || name == "FirstResponder") return name == "NSApplication" ? g_NSApp : nullptr;
    if (!c) {
        if (trace()) std::fprintf(stderr, "AppKit: nib: no class %s for a custom object\n", name.c_str());
        return nullptr;
    }
    id o = obj(send(reinterpret_cast<id>(c), "alloc"));
    if (o && responds(o, "init")) o = obj(send(o, "init"));
    if (o) awake_.push_back(o);
    if (trace()) std::fprintf(stderr, "AppKit: nib: created a %s\n", name.c_str());
    return o;
}

id unarchiver::build_menu(id dict, int depth) {
    const std::string title = string_value(dict, "NSTitle", depth);
    id menu = obj(send(reinterpret_cast<id>(cls("NSMenu")), "alloc"));
    menu = obj(send(menu, "initWithTitle:", ret(string(title))));
    if (id items = value(dict, "NSMenuItems", depth)) {
        const auto n = static_cast<std::uint64_t>(send(items, "count"));
        for (std::uint64_t i = 0; i < n; i++)
            if (id item = obj(send(items, "objectAtIndex:", i))) send(menu, "addItem:", ret(item));
    }
    /* Interface Builder identifies application menus by their archived
     * role, rather than an outlet connection to NSApplication. */
    const auto role = string_value(dict, "NSName", depth);
    if (role == "_NSMainMenu") {
        id application = obj(send(reinterpret_cast<id>(cls("NSApplication")), "sharedApplication"));
        send(application, "setMainMenu:", ret(menu));
    }
    return menu;
}

id unarchiver::build_menu_item(id dict, int depth) {
    Class c = cls("NSMenuItem");
    if (!c) return nullptr;
    if (int_value(dict, "NSIsSeparator", depth) != 0)
        return obj(send(reinterpret_cast<id>(c), "separatorItem"));
    const std::string title = string_value(dict, "NSTitle", depth);
    const std::string key = string_value(dict, "NSKeyEquiv", depth);
    const std::string action = string_value(dict, "NSAction", depth);
    id item = obj(send(reinterpret_cast<id>(c), "alloc"));
    const std::uint64_t args[3] = {ret(string(title)), reinterpret_cast<std::uintptr_t>(action.empty() ? nullptr : mm_objc_sel(action.c_str())),
                                   ret(string(key))};
    item = obj(mm_objc_send(item, mm_objc_sel("initWithTitle:action:keyEquivalent:"), 3, args));
    if (!item) return nullptr;
    send(item, "setTag:", std::uint64_t(int_value(dict, "NSTag", depth)));
    send(item, "setState:", std::uint64_t(int_value(dict, "NSState", depth)));
    if (id submenu = value(dict, "NSSubmenu", depth)) send(item, "setSubmenu:", ret(submenu));
    if (id target = value(dict, "NSTarget", depth)) send(item, "setTarget:", ret(target));
    send(item, "setKeyEquivalentModifierMask:", static_cast<std::uint64_t>(int_value(dict, "NSKeyEquivModMask", depth, 1 << 20)));
    if (int_value(dict, "NSIsDisabled", depth) != 0) send(item, "setEnabled:", 0);
    return item;
}

id unarchiver::build_window(id dict, int depth) {
    /* NSWindowRect is a string like "{{x, y}, {w, h}}". */
    double x = 0, y = 0, w = 480, h = 320;
    const std::string r = string_value(dict, "NSWindowRect", depth);
    if (!r.empty()) {
        double values[4];
        if (parse_nib_geometry(r,values)) { x=values[0];y=values[1];w=values[2];h=values[3]; }
        else std::fprintf(stderr,"AppKit layout: invalid nib window rect=\"%s\"\n",r.c_str());
    }
    const auto style = static_cast<std::uint64_t>(int_value(dict, "NSWindowStyleMask", depth, 15));
    id window = obj(send(reinterpret_cast<id>(cls("NSWindow")), "alloc"));
    if (mm_cpu_context *ctx = api()->current_context()) {
        std::memcpy(&ctx->v[0][0], &x, 8);
        std::memcpy(&ctx->v[1][0], &y, 8);
        std::memcpy(&ctx->v[2][0], &w, 8);
        std::memcpy(&ctx->v[3][0], &h, 8);
    }
    const std::uint64_t args[3] = {style, 2 /* NSBackingStoreBuffered */, 0};
    window = obj(mm_objc_send(window, mm_objc_sel("initWithContentRect:styleMask:backing:defer:"), 3, args));
    if (!window) return nullptr;
    {
        auto f = as<window_object>(window)->frame;
        auto *ctx = api()->current_context();
        std::fprintf(stderr, "AppKit layout: nib-window=%p raw=\"%s\" parsed=%gx%g initialized=%gx%g context=%s arch=%s numeric-locale=%s\n",
                     static_cast<void *>(window),r.c_str(),w,h,f.w,f.h,ctx ? "yes" : "no",
                     ctx && ctx->native_mode ? "x86_64" : "arm64",std::setlocale(LC_NUMERIC,nullptr));
    }
    const std::string title = string_value(dict, "NSWindowTitle", depth);
    if (!title.empty()) send(window, "setTitle:", ret(string(title)));
    if (id view = value(dict, "NSWindowView", depth)) {
        // Auto Layout nibs can archive a content view larger than the old
        // window rect. Keep its designed minimum instead of compressing
        // nested groups until their controls overlap.
        const auto size = as<view_object>(view)->frame;
        if (!as<view_object>(view)->constraints.empty() && (size.w > w || size.h > h)) {
            const double width = std::max(w, size.w), height = std::max(h, size.h);
            if (auto *ctx = api()->current_context()) {
                std::memcpy(&ctx->v[0][0], &width, 8);
                std::memcpy(&ctx->v[1][0], &height, 8);
            }
            send(window, "setContentSize:");
        }
        send(window, "setContentView:", ret(view));
        const auto f = as<window_object>(window)->frame;
        const auto content = as<view_object>(view)->frame;
        std::fprintf(stderr, "AppKit layout: nib-content window=%p title=\"%s\" designed=%gx%g window=%gx%g content=%gx%g constraints=%zu\n",
                     static_cast<void *>(window),title.c_str(),size.w,size.h,f.w,f.h,content.w,content.h,
                     as<view_object>(view)->constraints.size());
    }
    return window;
}

id unarchiver::build_view(id dict, const std::string &klass, int depth, std::uint64_t index) {
    Class c = klass == "NSCustomView" ? cls("NSView") : cls(klass.c_str());
    if (klass == "NSCustomView" || klass == "NSClassSwapper") {
        const std::string name = string_value(dict, "NSClassName", depth);
        if (Class custom = name.empty() ? nullptr : cls(name.c_str())) c = custom;
        else if(klass=="NSClassSwapper")c=cls(string_value(dict,"NSOriginalClassName",depth).c_str());
    }
    if (!c) return nullptr;
    /* NSFrame is "{{x, y}, {w, h}}"; a view with no frame is sized by its
     * window when it becomes the content view. */
    double x = 0, y = 0, w = 0, h = 0;
    const std::string r = string_value(dict, "NSFrame", depth);
    if (!r.empty()) {
        double values[4];
        if (parse_nib_geometry(r,values)) { x=values[0];y=values[1];w=values[2];h=values[3]; }
        else std::fprintf(stderr,"AppKit layout: invalid nib view class=%s rect=\"%s\"\n",klass.c_str(),r.c_str());
    } else {
        const auto size = string_value(dict, "NSFrameSize", depth);
        double values[2];
        if (parse_nib_geometry(size,values)) { w=values[0];h=values[1]; }
    }
    if (trace()) std::fprintf(stderr, "AppKit: nib view %s NSFrame '%s' -> {%g, %g, %g, %g}\n",
                              klass.c_str(), r.c_str(), x, y, w, h);
    id view = obj(send(reinterpret_cast<id>(c), "alloc"));
    if (mm_cpu_context *ctx = api()->current_context()) {
        std::memcpy(&ctx->v[0][0], &x, 8);
        std::memcpy(&ctx->v[1][0], &y, 8);
        std::memcpy(&ctx->v[2][0], &w, 8);
        std::memcpy(&ctx->v[3][0], &h, 8);
    }
    view = obj(send(view, "initWithFrame:"));
    if (!view) return nullptr;
    built_[index]=view; // Publish before following parent/sibling constraint references.
    auto *vo=as<view_object>(view);
    const auto flags=static_cast<std::uint64_t>(int_value(dict,"NSvFlags",depth,256));
    vo->autoresizing_mask=flags&63;vo->autoresizes_subviews=(flags&256)!=0;vo->hidden=(flags&0x80000000)!=0;
    vo->translates_mask=!int_value(dict,"NSDoNotTranslateAutoresizingMask",depth);
    if (is_kind(view, cls("NSControl"))) {
        auto *control=as<control_object>(view);
        if (id identifier = value(dict, "NSReuseIdentifierKey", depth))
            send(view, "setIdentifier:", ret(identifier));
        control->enabled=int_value(dict,"NSEnabled",depth,1)!=0;
        control->alignment=int_value(dict,"NSControlTextAlignment",depth);

        id cell_ref = obj(send(dict, "objectForKey:", ret(string("NSCell"))));
        id cell = is_uid(cell_ref) ? entry(send(cell_ref, "value")) : cell_ref;
        if (cell) {
            if (id title = value(cell, "NSContents", depth)) {
                if(is_kind(title,cls("NSImage")))send(view,"setImage:",ret(title));else send(view,"setStringValue:",ret(title));
            }
            if(id font=value(cell,"NSSupport",depth))if(is_kind(font,cls("NSFont")))send(view,"setFont:",ret(font));
            const auto f1=static_cast<std::uint64_t>(int_value(cell,"NSCellFlags",depth));
            control->editable=(f1&0x20000000)!=0;control->bordered=(f1&0x800000)!=0;control->bezeled=(f1&0x1000000)!=0;
            control->selectable=(f1&0x40000000)!=0;control->state=(f1>>31)&1;
            control->bezel_style=int_value(cell,"NSBezelStyle",depth,1);
            control->button_type=int_value(cell,"NSButtonType",depth,int_value(cell,"NSAuxButtonType",depth,0));
            if(is_kind(view,cls("NSButton")))control->bordered=true;
            if(is_kind(view,cls("NSTextField")))control->wraps=int_value(dict,"NSControlLineBreakMode",depth)==0;
            control->image_scaling=int_value(cell,"NSScale",depth,0);
            if(id placeholder=value(cell,"NSPlaceholderString",depth))send(view,"setPlaceholderString:",ret(placeholder));

            const auto action = string_value(cell, "NSControlAction", depth);
            if (!action.empty()) send(view, "setAction:", ret(mm_objc_sel(action.c_str())));
            if (id target = value(cell, "NSControlTarget", depth)) send(view, "setTarget:", ret(target));
            if (id key = value(cell, "NSKeyEquivalent", depth)) send(view, "setKeyEquivalent:", ret(key));
            if (is_kind(view, cls("NSPopUpButton"))) {
                if (id menu = value(cell, "NSMenu", depth)) {
                    send(view, "setMenu:", ret(menu));
                    if (id selected = value(cell, "NSMenuItem", depth)) send(view, "selectItem:", ret(selected));
                }
            }
        }
    }
    if (id subviews = value(dict, "NSSubviews", depth)) {
        const auto n = static_cast<std::uint64_t>(send(subviews, "count"));
        for (std::uint64_t i = 0; i < n; i++)
            if (id sub = obj(send(subviews, "objectAtIndex:", i))) send(view, "addSubview:", ret(sub));
    }
    if (is_kind(view, cls("NSBox"))) {
        auto *box = as<control_object>(view);
        box->box_type = int_value(dict, "NSBoxType", depth);
        box->title_position = int_value(dict, "NSTitlePosition", depth, 2);
        box->bordered = int_value(dict, "NSBorderType", depth, 3) != 0;
        box->box_content = value(dict, "NSContentView", depth);
        if (box->box_content) {
            if (as<view_object>(box->box_content)->superview != view)
                send(view, "addSubview:", ret(box->box_content));
            box->box_title_height = std::max(0., h - as<view_object>(box->box_content)->frame.h);
        }
        id ref = obj(send(dict, "objectForKey:", ret(string("NSTitleCell"))));
        id cell = is_uid(ref) ? entry(send(ref, "value")) : ref;
        if (cell) {
            if (id title = value(cell, "NSContents", depth)) send(view, "setTitle:", ret(title));
            if (id font = value(cell, "NSSupport", depth)) send(view, "setFont:", ret(font));
        }
    }
    if(is_kind(view,cls("NSProgressIndicator"))||is_kind(view,cls("NSSlider"))) {
        auto *control=as<control_object>(view);
        control->minimum=real_value(dict,"NSMinValue",depth,0);control->maximum=real_value(dict,"NSMaxValue",depth,100);
        control->value=real_value(dict,"NSValue",depth,0);
        if(is_kind(view,cls("NSProgressIndicator")))control->indeterminate=(int_value(dict,"NSpiFlags",depth)&2)!=0;
    }
    if(is_kind(view,cls("NSScrollView"))) {
        if(id content=value(dict,"NSContentView",depth))send(view,"setContentView:",ret(content));
        if(id scroller=value(dict,"NSVScroller",depth))send(view,"setVerticalScroller:",ret(scroller));
        if(id scroller=value(dict,"NSHScroller",depth))send(view,"setHorizontalScroller:",ret(scroller));
        const auto flags=static_cast<std::uint64_t>(int_value(dict,"NSsFlags",depth));
        send(view,"setAutohidesScrollers:",(flags&512)!=0);
        send(view,"setHasVerticalScroller:",(flags&16)!=0);
        send(view,"setHasHorizontalScroller:",(flags&32)!=0);
        send(view,"setBorderType:",flags&3);
    }
    if(is_kind(view,cls("NSTableView"))) {
        auto *table=as<table_object>(view);table->row_height=real_value(dict,"NSRowHeight",depth,20);
        table->column_autoresizing_style=static_cast<std::uint64_t>(int_value(dict,"NSColumnAutoresizingStyle",depth,4));
        if(id columns=value(dict,"NSTableColumns",depth))for(std::uint64_t i=0;i<send(columns,"count");++i)
            send(view,"addTableColumn:",send(columns,"objectAtIndex:",i));
        if (id nibs = value(dict, "NSTableViewArchivedReusableViewsKey", depth)) {
            id keys = obj(send(nibs, "allKeys"));
            for (std::uint64_t i = 0; i < send(keys, "count"); ++i) {
                id key = obj(send(keys, "objectAtIndex:", i));
                id data = obj(send(nibs, "objectForKey:", ret(key)));
                if (!data || !is_kind(data, cls("NSData"))) continue;
                const char *bytes = reinterpret_cast<const char *>(send(data, "bytes"));
                const auto length = send(data, "length");
                if (bytes && length) table->reusable_nibs[to_utf8(key)] = std::string(bytes, length);
            }
        }
    }
    if(id list=value(dict,"NSViewConstraints",depth))for(std::uint64_t i=0;i<send(list,"count");++i) {
        id ref=obj(send(list,"objectAtIndex:",i));
        // Constraints are decoded as their dictionaries to preserve numeric attributes.
        id d=is_uid(ref)?entry(send(ref,"value")):ref;
        if(!d||!is_kind(d,cls("NSDictionary")))continue;
        view_object::layout_constraint constraint;
        constraint.first=value(d,"NSFirstItem",depth);constraint.second=value(d,"NSSecondItem",depth);
        constraint.attribute=int_value(d,"NSFirstAttributeV2",depth,int_value(d,"NSFirstAttribute",depth));
        constraint.second_attribute=int_value(d,"NSSecondAttributeV2",depth,int_value(d,"NSSecondAttribute",depth));
        constraint.relation=int_value(d,"NSRelation",depth);
        constraint.multiplier=real_value(d,"NSMultiplier",depth,1);
        constraint.constant=real_value(d,"NSConstantV2",depth,real_value(d,"NSConstant",depth));
        constraint.priority=real_value(d,"NSPriority",depth,1000);
        if(!string_value(d,"NSSymbolicConstant",depth).empty()) {
            const bool container=constraint.first==view||constraint.second==view;
            constraint.constant=container?20:8;
        }
        vo->constraints.push_back(constraint);
    }
    awake_.push_back(view);
    return view;
}

/* An outlet is an assignment: the setter if the class has one, the
 * instance variable otherwise (Interface Builder names it after the outlet
 * or with a leading underscore). An action connection is a target/action
 * pair on a control. */
void unarchiver::apply_connection(id dict, int depth) {
    id source = value(dict, "NSSource", depth);
    id destination = value(dict, "NSDestination", depth);
    const std::string label = string_value(dict, "NSLabel", depth);
    if (!source || label.empty()) return;

    /* A control connection's label is the action selector. */
    if (label.back() == ':') {
        send(source, "setTarget:", ret(destination));
        send(source, "setAction:", reinterpret_cast<std::uintptr_t>(mm_objc_sel(label.c_str())));
        if (trace()) std::fprintf(stderr, "AppKit: nib: action %s\n", label.c_str());
        return;
    }
    std::string setter = "set" + label + ":";
    setter[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(setter[3])));
    if (responds(source, setter.c_str())) {
        send(source, setter.c_str(), ret(destination));
        if (trace()) std::fprintf(stderr, "AppKit: nib: outlet %s -> %s\n", label.c_str(), setter.c_str());
        return;
    }
    mm_objc_class *c = mm_objc_object_class(source);
    const std::string underscored = "_" + label;
    for (const char *name : {label.c_str(), underscored.c_str()}) {
        if (const mm_ivar *iv = mm_nib_class_getInstanceVariable(c, name)) {
            mm_nib_object_setIvar(source, iv, mm_objc_retain(destination));
            if (trace()) std::fprintf(stderr, "AppKit: nib: outlet %s -> ivar %s\n", label.c_str(), name);
            return;
        }
    }
    if (trace())
        std::fprintf(stderr, "AppKit: nib: outlet %s on a %s has no setter or ivar\n", label.c_str(), mm_objc_class_name(c));
}

/* NIBArchive uses the same object graph and coder keys as NSKeyedArchiver,
 * but stores repeated array keys and primitive values in separate tables.
 * Normalize the tables into the keyed representation used above. */
id normalized_nib(const std::vector<nib_record> &records) {
    auto dictionary = [] { return obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary")); };
    auto array = [] { return obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array")); };
    auto put = [](id d, const char *key, id value) { if (value) send(d, "setObject:forKey:", ret(value), ret(string(key))); };
    auto uid = [](std::uint64_t index) { return obj(send(reinterpret_cast<id>(cls("__NSCFKeyedArchiverUID")), "uidWithValue:", index)); };
    id objects = array(); send(objects, "addObject:", ret(string("$null")));
    for (const auto &record : records) {
        id d = dictionary(), class_info = dictionary();
        put(class_info, "$classname", string(record.class_name)); put(d, "$class", class_info);
        id items = array();
        for (const auto &v : record.values) {
            id value = nullptr;
            switch (v.type) {
            case 0: case 1: case 2: case 3: case 4: case 5:
                value = obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithLongLong:", static_cast<std::uint64_t>(v.integer))); break;
            case 6: case 7: value = number(v.real); break;
            case 8: value = obj(send(reinterpret_cast<id>(cls("NSData")), "dataWithBytes:length:", ret(v.bytes.data()), v.bytes.size())); break;
            case 10: value = uid(static_cast<std::uint64_t>(v.integer) + 1); break;
            default: break;
            }
            if (v.key == "UINibEncoderEmptyKey") { if (value) send(items, "addObject:", ret(value)); }
            else put(d, v.key.c_str(), value);
            if (v.key == "NS.bytes" && (record.class_name == "NSString" || record.class_name == "NSLocalizableString"))
                put(d, "NS.string", string(v.bytes));
            if (v.key == "NS.bytes" && (record.class_name == "NSData" || record.class_name == "NSMutableData"))
                put(d, "NS.data", value);
        }
        if (record.class_name == "NSArray" || record.class_name == "NSMutableArray" || record.class_name == "NSSet" || record.class_name == "NSMutableSet")
            put(d, "NS.objects", items);
        if (record.class_name == "NSDictionary" || record.class_name == "NSMutableDictionary") {
            id keys = array(), values = array();
            for (std::uint64_t i = 0; i + 1 < send(items, "count"); i += 2) {
                send(keys, "addObject:", send(items, "objectAtIndex:", i));
                send(values, "addObject:", send(items, "objectAtIndex:", i + 1));
            }
            put(d, "NS.keys", keys); put(d, "NS.objects", values);
        }
        send(objects, "addObject:", ret(d));
    }
    id top = dictionary();
    if (!records.empty()) {
        id root = obj(send(objects, "objectAtIndex:", 1));
        for (const char *key : {"IB.objectdata", "NSRoot", "root"}) put(top, key, obj(send(root, "objectForKey:", ret(string(key)))));
    }
    id archive = dictionary(); put(archive, "$objects", objects); put(archive, "$top", top);
    return archive;
}

/* ---- finding the file ---------------------------------------------------- */

/* A nib is looked up the way NSBundle does: the localised resource
 * directories first, then the bundle's Resources. A ".nib" directory holds
 * the archive as `keyedobjects.nib` (plus a version-specific one on newer
 * systems, which is the same archive). */
fs::path locate_nib(const std::string &name) {
    id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
    const std::string root = bundle ? to_utf8(obj(send(bundle, "bundlePath"))) : std::string();
    if (root.empty()) return {};
    const fs::path resources = fs::path(root) / "Contents" / "Resources";
    std::vector<fs::path> dirs{resources};
    std::error_code ec;

    std::string base = name;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".nib") == 0) base.resize(base.size() - 4);
    if (id localized = obj(send(bundle, "pathForResource:ofType:", ret(string(base)), ret(string("nib")))))
        dirs.insert(dirs.begin(), fs::path(to_utf8(localized)).parent_path());
    for (const auto &dir : dirs) {
        const fs::path candidate = dir / (base + ".nib");
        if (fs::is_directory(candidate, ec)) {
            const fs::path keyed = candidate / "keyedobjects.nib";
            if (fs::is_regular_file(keyed, ec)) return keyed;
            for (const auto &entry : fs::directory_iterator(candidate, ec))
                if (entry.path().filename().string().starts_with("keyedobjects")) return entry.path();
        } else if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::string read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

/* Loads `name` from the main bundle. Returns false when there is no such
 * nib or it cannot be read; `top_level` collects the objects the nib owns,
 * as -loadNibNamed:owner:topLevelObjects: reports them. */
bool load_nib(const std::string &name, id owner, std::vector<id> *top_level) {
    const fs::path path = locate_nib(name);
    if (path.empty()) {
        if (trace()) std::fprintf(stderr, "AppKit: nib: %s is not in the bundle\n", name.c_str());
        return false;
    }
    const std::string bytes = read_file(path);
    if (bytes.empty()) return false;
    if (trace()) std::fprintf(stderr, "AppKit: nib: loading %s\n", path.c_str());
    return load_nib_data(bytes, owner, top_level, name);
}

bool load_nib_data(std::string_view bytes, id owner, std::vector<id> *top_level, const std::string &localization_table) {

    id plist = nullptr;
    if (bytes.starts_with("NIBArchive")) {
        std::vector<nib_record> records; std::string error;
        if (!parse_nib_archive(bytes, records, error)) {
            std::fprintf(stderr, "AppKit: nib: %s\n", error.c_str()); return false;
        }
        plist = normalized_nib(records);
    } else {
        id data = obj(send(reinterpret_cast<id>(cls("NSData")), "dataWithBytes:length:", ret(bytes.data()), bytes.size()));
        const std::uint64_t args[] = {ret(data), 0, 0, 0};
        plist = obj(mm_objc_send(reinterpret_cast<id>(cls("NSPropertyListSerialization")),
            mm_objc_sel("propertyListWithData:options:format:error:"), 4, args));
    }
    if (!plist || !is_kind(plist, cls("NSDictionary"))) {
        std::fprintf(stderr, "AppKit: nib: data is not a keyed archive\n");
        return false;
    }
    id objects = obj(send(plist, "objectForKey:", ret(string("$objects"))));
    id top = obj(send(plist, "objectForKey:", ret(string("$top"))));
    if (!objects || !top) {
        std::fprintf(stderr, "AppKit: nib: data has no $objects\n");
        return false;
    }

    unarchiver u(objects, owner, localization_table);
    /* $top names the root; Interface Builder calls it "IB.objectdata",
     * other archivers "root" or "NSRoot". Any single entry is the root. */
    id root = nullptr;
    for (const char *key : {"IB.objectdata", "NSRoot", "root"})
        if ((root = obj(send(top, "objectForKey:", ret(string(key)))))) break;
    if (!root) {
        id keys = obj(send(top, "allKeys"));
        if (keys && send(keys, "count") > 0)
            root = obj(send(top, "objectForKey:", send(keys, "objectAtIndex:", 0)));
    }
    if (trace())
        std::fprintf(stderr, "AppKit: nib: %llu objects\n", static_cast<unsigned long long>(send(objects, "count")));
    if (!root) root = obj(send(top, "objectForKey:", ret(string("root"))));
    u.decode(root);

    /* -awakeFromNib is sent once everything is connected. */
    for (id o : u.awake_list())
        if (responds(o, "awakeFromNib")) send(o, "awakeFromNib");
    if (owner && responds(owner, "awakeFromNib")) send(owner, "awakeFromNib");
    for(id o:u.awake_list())if(is_kind(o,cls("NSView"))&&!as<view_object>(o)->superview)layout_cocoa_view(o);
    if (top_level) *top_level = u.top_level();
    return true;
}

} // namespace ak
