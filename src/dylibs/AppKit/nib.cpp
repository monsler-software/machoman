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

#include <cstdio>
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
    unarchiver(id objects, id owner) : objects_(objects), owner_(owner) {}

    id decode(id ref, int depth = 0);
    const std::vector<id> &top_level() const { return top_level_; }
    void note_top_level(id o) { if (o) top_level_.push_back(o); }

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
    id build_view(id dict, const std::string &klass, int depth);
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

    id objects_;
    id owner_;
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

    if (klass == "NSMutableString" || klass == "NSString")
        return obj(send(dict, "objectForKey:", ret(string("NS.string"))));
    if (klass == "NSMutableData" || klass == "NSData")
        return obj(send(dict, "objectForKey:", ret(string("NS.data"))));
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
    if (klass == "NSCustomObject") return build_custom_object(dict, depth);
    if (klass == "NSMenu" || klass == "NSMenuTemplate") return build_menu(dict, depth);
    if (klass == "NSMenuItem") return build_menu_item(dict, depth);
    if (klass == "NSWindowTemplate") return build_window(dict, depth);
    if (klass == "NSCustomView" || klass == "NSView") return build_view(dict, klass, depth);
    if (klass == "NSNibOutletConnector" || klass == "NSNibControlConnector" || klass == "NSNibConnector") {
        apply_connection(dict, depth);
        return nullptr;
    }
    if (klass == "NSIBObjectData") {
        /* The root: the objects, then the connections between them (the
         * connectors apply themselves as they are decoded), then the
         * windows the nib says are visible. */
        id objects = value(dict, "NSObjectsValues", depth);
        if (objects) {
            const auto n = static_cast<std::uint64_t>(send(objects, "count"));
            for (std::uint64_t i = 0; i < n; i++) note_top_level(obj(send(objects, "objectAtIndex:", i)));
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
    if (id submenu = value(dict, "NSSubmenu", depth)) send(item, "setSubmenu:", ret(submenu));
    if (id target = value(dict, "NSTarget", depth)) send(item, "setTarget:", ret(target));
    if (int_value(dict, "NSIsDisabled", depth) != 0) send(item, "setEnabled:", 0);
    return item;
}

id unarchiver::build_window(id dict, int depth) {
    /* NSWindowRect is a string like "{{x, y}, {w, h}}". */
    double x = 0, y = 0, w = 480, h = 320;
    const std::string r = string_value(dict, "NSWindowRect", depth);
    if (!r.empty()) std::sscanf(r.c_str(), "{{%lf, %lf}, {%lf, %lf}}", &x, &y, &w, &h);
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
    const std::string title = string_value(dict, "NSWindowTitle", depth);
    if (!title.empty()) send(window, "setTitle:", ret(string(title)));
    if (id view = value(dict, "NSWindowView", depth)) send(window, "setContentView:", ret(view));
    return window;
}

id unarchiver::build_view(id dict, const std::string &klass, int depth) {
    Class c = cls("NSView");
    if (klass == "NSCustomView") {
        const std::string name = string_value(dict, "NSClassName", depth);
        if (Class custom = name.empty() ? nullptr : cls(name.c_str())) c = custom;
    }
    if (!c) return nullptr;
    /* NSFrame is "{{x, y}, {w, h}}"; a view with no frame is sized by its
     * window when it becomes the content view. */
    double x = 0, y = 0, w = 0, h = 0;
    const std::string r = string_value(dict, "NSFrame", depth);
    if (!r.empty()) std::sscanf(r.c_str(), "{{%lf, %lf}, {%lf, %lf}}", &x, &y, &w, &h);
    id view = obj(send(reinterpret_cast<id>(c), "alloc"));
    if (mm_cpu_context *ctx = api()->current_context()) {
        std::memcpy(&ctx->v[0][0], &x, 8);
        std::memcpy(&ctx->v[1][0], &y, 8);
        std::memcpy(&ctx->v[2][0], &w, 8);
        std::memcpy(&ctx->v[3][0], &h, 8);
    }
    view = obj(send(view, "initWithFrame:"));
    if (!view) return nullptr;
    if (id subviews = value(dict, "NSSubviews", depth)) {
        const auto n = static_cast<std::uint64_t>(send(subviews, "count"));
        for (std::uint64_t i = 0; i < n; i++)
            if (id sub = obj(send(subviews, "objectAtIndex:", i))) send(view, "addSubview:", ret(sub));
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
    for (const auto &entry : fs::directory_iterator(resources, ec))
        if (entry.is_directory(ec) && entry.path().extension() == ".lproj") dirs.push_back(entry.path());

    std::string base = name;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".nib") == 0) base.resize(base.size() - 4);
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

    id data = obj(send(reinterpret_cast<id>(cls("NSData")), "dataWithBytes:length:",
                       reinterpret_cast<std::uintptr_t>(bytes.data()), bytes.size()));
    const std::uint64_t args[4] = {ret(data), 0, 0, 0};
    id plist = obj(mm_objc_send(reinterpret_cast<id>(cls("NSPropertyListSerialization")),
                                mm_objc_sel("propertyListWithData:options:format:error:"), 4, args));
    if (!plist || !is_kind(plist, cls("NSDictionary"))) {
        std::fprintf(stderr, "AppKit: nib: %s is not a keyed archive\n", path.c_str());
        return false;
    }
    id objects = obj(send(plist, "objectForKey:", ret(string("$objects"))));
    id top = obj(send(plist, "objectForKey:", ret(string("$top"))));
    if (!objects || !top) {
        std::fprintf(stderr, "AppKit: nib: %s has no $objects\n", path.c_str());
        return false;
    }
    if (trace()) std::fprintf(stderr, "AppKit: nib: loading %s\n", path.c_str());

    unarchiver u(objects, owner);
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
    if (top_level) *top_level = u.top_level();
    return true;
}

} // namespace ak
