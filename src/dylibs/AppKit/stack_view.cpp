/* Stack containers must own the full NSView payload: their children and
 * window membership participate in the same hierarchy as ordinary views. */
#include "appkit.h"
#include <algorithm>

MM_FW_CLASS(NSStackView);

namespace ak {
namespace {
struct stack_object {
    view_object view;
    std::vector<id> arranged; // retained by NSView's subviews
    std::uint64_t orientation = 0, alignment = 0, distribution = 0;
    double spacing = 8;
};
stack_object *stack(id self) { return as<stack_object>(self); }
std::uint64_t stack_views(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id view : stack(self)->arranged) send(array, "addObject:", ret(view));
    return ret(array);
}
std::uint64_t stack_insert(id self, SEL, id view, std::uint64_t index) {
    if (!view || view == self || !is_kind(view, cls("NSView"))) return 0;
    auto &views = stack(self)->arranged;
    if (std::find(views.begin(), views.end(), view) != views.end()) return 0;
    send(self, "addSubview:", ret(view));
    views.insert(views.begin() + static_cast<std::ptrdiff_t>(std::min(index, std::uint64_t(views.size()))), view);
    return 0;
}
std::uint64_t stack_add(id self, SEL, id view) { return stack_insert(self, nullptr, view, stack(self)->arranged.size()); }
std::uint64_t stack_remove(id self, SEL, id view) {
    auto &views = stack(self)->arranged;
    views.erase(std::remove(views.begin(), views.end(), view), views.end());
    if (view && obj(send(view, "superview")) == self) send(view, "removeFromSuperview");
    return 0;
}
std::uint64_t stack_factory(id klass, SEL, id views) {
    id self = obj(send(klass, "new"));
    const auto count = send(views, "count");
    for (std::uint64_t i = 0; i < count; ++i) stack_add(self, nullptr, obj(send(views, "objectAtIndex:", i)));
    return ret(autorelease(self));
}
}
void register_stack_view() {
    Class c = mm_objc_define_class("NSStackView", cls("NSView"), sizeof(stack_object),
        MM_FW_CLASS_REF(NSStackView), MM_FW_META_REF(NSStackView));
    register_lifecycle<stack_object>(c);
    MM_CLASS_METHODS(c, {"stackViewWithViews:", MM_IMP(stack_factory), "@24@0:8@16"});
    MM_METHODS(c,
        {"views", MM_IMP(stack_views), "@16@0:8"}, {"arrangedSubviews", MM_IMP(stack_views), "@16@0:8"},
        {"addArrangedSubview:", MM_IMP(stack_add), "v24@0:8@16"},
        {"insertArrangedSubview:atIndex:", MM_IMP(stack_insert), "v32@0:8@16Q24"},
        {"removeArrangedSubview:", MM_IMP(stack_remove), "v24@0:8@16"},
        {"addView:inGravity:", MM_IMP(+[](id s, SEL, id view, std::uint64_t) { return stack_add(s, nullptr, view); }), "v32@0:8@16q24"},
        {"removeView:", MM_IMP(stack_remove), "v24@0:8@16"},
        {"orientation", MM_IMP(+[](id s, SEL) { return stack(s)->orientation; }), "q16@0:8"},
        {"setOrientation:", MM_IMP(+[](id s, SEL, std::uint64_t v) -> std::uint64_t { stack(s)->orientation = v; return 0; }), "v24@0:8q16"},
        {"alignment", MM_IMP(+[](id s, SEL) { return stack(s)->alignment; }), "q16@0:8"},
        {"setAlignment:", MM_IMP(+[](id s, SEL, std::uint64_t v) -> std::uint64_t { stack(s)->alignment = v; return 0; }), "v24@0:8q16"},
        {"distribution", MM_IMP(+[](id s, SEL) { return stack(s)->distribution; }), "q16@0:8"},
        {"setDistribution:", MM_IMP(+[](id s, SEL, std::uint64_t v) -> std::uint64_t { stack(s)->distribution = v; return 0; }), "v24@0:8q16"},
        {"spacing", MM_IMP(+[](id s, SEL) { return stack(s)->spacing; }), "d16@0:8"},
        {"setSpacing:", MM_IMP(+[](id s, SEL, double v) -> std::uint64_t { stack(s)->spacing = v; return 0; }), "v24@0:8d16"});
}
}
