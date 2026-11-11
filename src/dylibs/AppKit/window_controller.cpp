#include "appkit.h"
#include <cstdio>

MM_FW_CLASS(NSWindowController);
MM_FW_CLASS(NSDocumentController);
namespace ak {
namespace {
struct document_controller_object {
    mm_objc_object base;
    std::vector<id> recent;
    std::uint64_t maximum = 10;
    ~document_controller_object() { for (id url : recent) release(url); }
};
id history_defaults() { return obj(send(reinterpret_cast<id>(cls("NSUserDefaults")), "standardUserDefaults")); }
id history_key() { return string("NSRecentDocumentURLs"); }
std::uint64_t document_controller_init(id self, SEL) {
    auto *c = as<document_controller_object>(self);
    for (id url : c->recent) release(url);
    c->recent.clear();
    id saved = obj(send(history_defaults(), "arrayForKey:", ret(history_key())));
    for (std::uint64_t i = 0; i < send(saved, "count") && c->recent.size() < c->maximum; ++i) {
        id text = obj(send(saved, "objectAtIndex:", i));
        if (!is_kind(text, cls("NSString"))) continue;
        if (id url = obj(send(reinterpret_cast<id>(cls("NSURL")), "URLWithString:", ret(text))))
            c->recent.push_back(retain(url));
    }
    return ret(self);
}
void save_history(document_controller_object *c) {
    id saved = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id url : c->recent) send(saved, "addObject:", send(url, "absoluteString"));
    id defaults = history_defaults();
    send(defaults, "setObject:forKey:", ret(saved), ret(history_key()));
    send(defaults, "synchronize");
}
std::uint64_t recent_urls(id self, SEL) {
    auto &recent = as<document_controller_object>(self)->recent;
    return send(reinterpret_cast<id>(cls("NSArray")), "arrayWithObjects:count:", ret(recent.data()), recent.size());
}
std::uint64_t note_recent_url(id self, SEL, id url) {
    if (!url || !is_kind(url, cls("NSURL"))) return 0;
    auto *c = as<document_controller_object>(self);
    const auto identity = to_utf8(obj(send(url, "absoluteString")));
    if (identity.empty()) return 0;
    id owned = retain(url);
    std::erase_if(c->recent, [&](id old) {
        if (to_utf8(obj(send(old, "absoluteString"))) != identity) return false;
        release(old); return true;
    });
    c->recent.insert(c->recent.begin(), owned);
    while (c->recent.size() > c->maximum) { release(c->recent.back()); c->recent.pop_back(); }
    save_history(c);
    return 0;
}
std::uint64_t clear_recent(id self, SEL, id) {
    auto *c = as<document_controller_object>(self);
    for (id url : c->recent) release(url);
    c->recent.clear(); save_history(c); return 0;
}
std::uint64_t maximum_recent(id self, SEL) { return as<document_controller_object>(self)->maximum; }
std::uint64_t set_maximum_recent(id self, SEL, std::uint64_t maximum) {
    auto *c = as<document_controller_object>(self);
    c->maximum = std::min(maximum, std::uint64_t(100));
    while (c->recent.size() > c->maximum) { release(c->recent.back()); c->recent.pop_back(); }
    save_history(c); return 0;
}
std::uint64_t shared_document_controller(id klass, SEL) {
    static id controller = obj(send(klass, "new"));
    return ret(controller);
}
struct controller_object {
    mm_objc_object base;
    id next = nullptr, window = nullptr, document = nullptr;
    std::string nib_name;
    std::vector<id> top_level;
    bool loaded = false, loading = false;
    ~controller_object() { release(window); release(document); for (id o : top_level) release(o); }
};
controller_object *controller(id self) { return as<controller_object>(self); }
std::uint64_t set_window(id self, SEL, id window) {
    auto *c = controller(self); id old = c->window; c->window = retain(window);
    c->loaded = window != nullptr; release(old); return 0;
}
std::uint64_t init_window(id self, SEL, id window) { set_window(self, nullptr, window); return ret(self); }
std::uint64_t init_nib(id self, SEL, id name) { controller(self)->nib_name = to_utf8(name); return ret(self); }
std::uint64_t nib_name(id self, SEL) { return ret(string(controller(self)->nib_name)); }
std::uint64_t load_window(id self, SEL) {
    auto *c = controller(self);
    if (c->loaded || c->loading) return 0;
    c->loading = true;
    const auto name = to_utf8(obj(send(self, "windowNibName")));
    if (!name.empty() && load_nib(name, self, &c->top_level)) {
        for (id o : c->top_level) retain(o);
        c->loaded = c->window != nullptr;
    }
    c->loading = false; return 0;
}
std::uint64_t window(id self, SEL) {
    auto *c = controller(self);
    if (!c->loaded && !c->loading) {
        send(self, "windowWillLoad"); send(self, "loadWindow");
        if (c->window) { c->loaded = true; send(self, "windowDidLoad"); }
    }
    return ret(c->window);
}
std::uint64_t show_window(id self, SEL, id sender) { id w = obj(send(self, "window")); send(w, "makeKeyAndOrderFront:", ret(sender)); return 0; }
std::uint64_t close(id self, SEL) { send(controller(self)->window, "close"); return 0; }
std::uint64_t loaded(id self, SEL) { return controller(self)->loaded; }
std::uint64_t document(id self, SEL) { return ret(controller(self)->document); }
std::uint64_t set_document(id self, SEL, id document) {
    auto *c = controller(self); id old = c->document; c->document = retain(document); release(old); return 0;
}
std::uint64_t noop(id, SEL) { return 0; }
}
void register_window_controller() {
    Class documents = mm_objc_define_class("NSDocumentController", cls("NSObject"), sizeof(document_controller_object),
                                           MM_FW_CLASS_REF(NSDocumentController), MM_FW_META_REF(NSDocumentController));
    register_lifecycle<document_controller_object>(documents);
    MM_CLASS_METHODS(documents, {"sharedDocumentController", MM_IMP(shared_document_controller), "@16@0:8"});
    MM_METHODS(documents, {"init", MM_IMP(document_controller_init), "@16@0:8"},
        {"recentDocumentURLs", MM_IMP(recent_urls), "@16@0:8"},
        {"noteNewRecentDocumentURL:", MM_IMP(note_recent_url), "v24@0:8@16"},
        {"clearRecentDocuments:", MM_IMP(clear_recent), "v24@0:8@16"},
        {"maximumRecentDocumentCount", MM_IMP(maximum_recent), "Q16@0:8"},
        {"setMaximumRecentDocumentCount:", MM_IMP(set_maximum_recent), "v24@0:8Q16"});
    Class c = mm_objc_define_class("NSWindowController", cls("NSResponder"), sizeof(controller_object),
        MM_FW_CLASS_REF(NSWindowController), MM_FW_META_REF(NSWindowController));
    register_lifecycle<controller_object>(c);
    MM_METHODS(c,
        {"initWithWindow:", MM_IMP(init_window), "@24@0:8@16"},
        {"initWithWindowNibName:", MM_IMP(init_nib), "@24@0:8@16"},
        {"windowNibName", MM_IMP(nib_name), "@16@0:8"},
        {"window", MM_IMP(window), "@16@0:8"}, {"setWindow:", MM_IMP(set_window), "v24@0:8@16"},
        {"loadWindow", MM_IMP(load_window), "v16@0:8"}, {"isWindowLoaded", MM_IMP(loaded), "B16@0:8"},
        {"showWindow:", MM_IMP(show_window), "v24@0:8@16"}, {"close", MM_IMP(close), "v16@0:8"},
        {"document", MM_IMP(document), "@16@0:8"}, {"setDocument:", MM_IMP(set_document), "v24@0:8@16"},
        {"windowWillLoad", MM_IMP(noop), "v16@0:8"}, {"windowDidLoad", MM_IMP(noop), "v16@0:8"});
}
}
