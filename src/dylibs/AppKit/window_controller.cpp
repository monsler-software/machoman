#include "appkit.h"
#include <cstdio>

MM_FW_CLASS(NSWindowController);
namespace ak {
namespace {
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
