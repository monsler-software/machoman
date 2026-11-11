#include "appkit.h"
#include "cocoa_layout.h"
#include <algorithm>
#include <cmath>

MM_FW_CLASS(NSImageView);
MM_FW_CLASS(NSScrollView);
MM_FW_CLASS(NSClipView);
MM_FW_CLASS(NSTableView);
MM_FW_CLASS(NSOutlineView);
MM_FW_CLASS(NSTableColumn);
MM_FW_CLASS(NSTableCellView);
MM_FW_CLASS(NSTableRowView);
MM_FW_CLASS(NSBox);
MM_FW_CLASS(NSSlider);
MM_FW_CLASS(NSProgressIndicator);
MM_FW_CLASS(NSComboBox);
MM_FW_CLASS(NSSegmentedControl);
MM_FW_CLASS(NSSecureTextField);
MM_FW_CLASS(NSSearchField);
MM_FW_CLASS(NSFont);

namespace ak {
control_object::~control_object() {
    for (id o : {title, key_equivalent, image, font, placeholder, identifier, text_field, image_view,
                 choice_menu, choice_selection})
        release(o);
    for (id o : choices)
        release(o);
}
table_object::~table_object() {
    for (id o : columns)
        release(o);
    for (id o : reusable_views)
        release(o);
}
namespace {
void dirty(id o) { send(o, "setNeedsDisplay:", 1); }
void assign(id &field, id value) {
    id next = retain(value);
    release(field);
    field = next;
}
void set_frame(id o, rect f) {
    if (auto *ctx = api()->current_context()) {
        const double a[]{f.x, f.y, f.w, f.h};
        for (int i = 0; i < 4; ++i)
            std::memcpy(&ctx->v[i][0], &a[i], 8);
    }
    send(o, "setFrame:");
}
struct font_object {
    mm_objc_object base;
    double size = 13;
    std::string family="Sans";
    bool bold = false;
};
struct scroll_object {
    view_object view;
    id content = nullptr;
    bool horizontal = false, vertical = true, border = true;
};
control_object *c(id o) { return as<control_object>(o); }
#define BOOL_PROPERTY(name, field)                                                                                     \
    std::uint64_t get_##name(id o, SEL) { return c(o)->field; }                                                        \
    std::uint64_t set_##name(id o, SEL, std::uint64_t v) {                                                             \
        c(o)->field = (v & 255) != 0;                                                                                  \
        dirty(o);                                                                                                      \
        return 0;                                                                                                      \
    }
#define INT_PROPERTY(name, field)                                                                                      \
    std::uint64_t get_##name(id o, SEL) { return static_cast<std::uint64_t>(c(o)->field); }                            \
    std::uint64_t set_##name(id o, SEL, std::int64_t v) {                                                              \
        c(o)->field = v;                                                                                               \
        dirty(o);                                                                                                      \
        return 0;                                                                                                      \
    }
#define DOUBLE_PROPERTY(name, field)                                                                                   \
    double get_##name(id o, SEL) { return c(o)->field; }                                                               \
    std::uint64_t set_##name(id o, SEL, double v) {                                                                    \
        if (std::isfinite(v))                                                                                          \
            c(o)->field = v;                                                                                           \
        dirty(o);                                                                                                      \
        return 0;                                                                                                      \
    }
#define OBJECT_PROPERTY(name, field)                                                                                   \
    std::uint64_t get_##name(id o, SEL) { return ret(c(o)->field); }                                                   \
    std::uint64_t set_##name(id o, SEL, id v) {                                                                        \
        assign(c(o)->field, v);                                                                                        \
        dirty(o);                                                                                                      \
        return 0;                                                                                                      \
    }
BOOL_PROPERTY(bordered, bordered)
BOOL_PROPERTY(bezeled, bezeled) BOOL_PROPERTY(editable, editable) BOOL_PROPERTY(selectable, selectable)
    BOOL_PROPERTY(background, draws_background) BOOL_PROPERTY(indeterminate, indeterminate)
        INT_PROPERTY(button_type, button_type) INT_PROPERTY(bezel, bezel_style) INT_PROPERTY(alignment, alignment)
            INT_PROPERTY(scaling, image_scaling) DOUBLE_PROPERTY(value, value) DOUBLE_PROPERTY(minimum, minimum)
                DOUBLE_PROPERTY(maximum, maximum) DOUBLE_PROPERTY(knob, knob) OBJECT_PROPERTY(image, image)
                    OBJECT_PROPERTY(placeholder, placeholder) OBJECT_PROPERTY(identifier, identifier)
                        OBJECT_PROPERTY(text_field, text_field) OBJECT_PROPERTY(image_view, image_view) std::uint64_t
    cell(id o, SEL) {
    return ret(o);
}
std::uint64_t font(id o, SEL) { return ret(c(o)->font); }
std::uint64_t set_font(id o, SEL, id f) {
    assign(c(o)->font, f);
    if (f && is_kind(f, cls("NSFont"))) {
        c(o)->font_size = as<font_object>(f)->size;
        c(o)->font_family=as<font_object>(f)->family;
        c(o)->bold = as<font_object>(f)->bold;
    }
    dirty(o);
    return 0;
}
std::uint64_t mask(id o, SEL) { return as<view_object>(o)->autoresizing_mask; }
std::uint64_t set_mask(id o, SEL, std::uint64_t m) {
    as<view_object>(o)->autoresizing_mask = m & 63;
    return 0;
}
std::uint64_t autoresizes(id o, SEL) { return as<view_object>(o)->autoresizes_subviews; }
std::uint64_t set_autoresizes(id o, SEL, std::uint64_t v) {
    as<view_object>(o)->autoresizes_subviews = v & 255;
    return 0;
}
std::uint64_t translates(id o, SEL) { return as<view_object>(o)->translates_mask; }
std::uint64_t set_translates(id o, SEL, std::uint64_t v) {
    as<view_object>(o)->translates_mask = v & 255;
    return 0;
}
std::uint64_t layout_view(id o, SEL) {
    layout_cocoa_view(o);
    return 0;
}
std::uint64_t flipped(id, SEL) { return 1; }
std::uint64_t start(id o, SEL, id) {
    c(o)->animating = true;
    dirty(o);
    return 0;
}
std::uint64_t stop(id o, SEL, id) {
    c(o)->animating = false;
    dirty(o);
    return 0;
}
std::uint64_t increment(id o, SEL, double v) { return set_value(o, nullptr, c(o)->value + v); }
point local_point(id o, id event) {
    auto p = as<event_object>(event)->where;
    std::vector<id> parents;
    for (id v = o; v; v = as<view_object>(v)->superview)
        parents.push_back(v);
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
        auto *v = as<view_object>(*it);
        auto f = v->frame, b = v->bounds;
        const bool flip = (send(*it, "isFlipped") != 0) != (v->superview && send(v->superview, "isFlipped") != 0);
        p = {b.x + (p.x - f.x) * (f.w ? b.w / f.w : 1),
             b.y + (flip ? f.y + f.h - p.y : p.y - f.y) * (f.h ? b.h / f.h : 1)};
    }
    return p;
}
void action(id o) {
    auto *v = c(o);
    if (v->enabled && v->action)
        send(g_NSApp, "sendAction:to:from:", ret(v->action), ret(v->target), ret(o));
}
std::uint64_t slider_mouse(id o, SEL, id event) {
    auto *v = c(o);
    if (!v->enabled)
        return 0;
    auto p = local_point(o, event);
    auto b = v->view.bounds;
    double fraction = b.h > b.w ? (p.y - b.y - 7) / std::max(1., b.h - 14) : (p.x - b.x - 7) / std::max(1., b.w - 14);
    v->value = v->minimum + std::clamp(fraction, 0., 1.) * (v->maximum - v->minimum);
    dirty(o);
    action(o);
    return 0;
}
std::uint64_t text_mouse(id o, SEL, id) {
    if (c(o)->enabled && (c(o)->editable || c(o)->selectable))
        send(c(o)->view.window, "makeFirstResponder:", ret(o));
    dirty(o);
    return 0;
}
std::uint64_t text_key(id o, SEL, id e) {
    auto *v = c(o);
    if (!v->enabled || !v->editable)
        return 0;
    auto *event = as<event_object>(e);
    if (event->keycode == 36 || event->keycode == 76) {
        action(o);
        return 0;
    }
    std::string text = v->title ? to_utf8(v->title) : "";
    if (event->keycode == 51) {
        if (!text.empty()) {
            auto pos = text.size() - 1;
            while (pos && (static_cast<unsigned char>(text[pos]) & 0xc0) == 0x80)
                --pos;
            text.erase(pos);
        }
    } else if(event->keycode>=123&&event->keycode<=126)return 0;
    else if (!(event->modifiers & ((1ull << 18) | (1ull << 20))))
        text += event->characters;
    send(o, "setStringValue:", ret(string(text)));
    return 0;
}
std::uint64_t accepts_text(id o, SEL) { return c(o)->enabled && (c(o)->editable || c(o)->selectable); }
id popup_menu(id o) {
    auto *v = c(o);
    if (!v->choice_menu)
        v->choice_menu = obj(send(reinterpret_cast<id>(cls("NSMenu")), "new"));
    return v->choice_menu;
}
std::uint64_t choice_count(id o, SEL) {
    return c(o)->choice_menu ? send(c(o)->choice_menu, "numberOfItems") : c(o)->choices.size();
}
id choice_item(id o, std::int64_t index) {
    return c(o)->choice_menu ? obj(send(c(o)->choice_menu, "itemAtIndex:", std::uint64_t(index))) : nullptr;
}
id title_at(id o, std::int64_t index) {
    if (auto item = choice_item(o, index)) return obj(send(item, "title"));
    auto &choices = c(o)->choices;
    return index >= 0 && index < std::int64_t(choices.size()) ? choices[index] : nullptr;
}
std::uint64_t selected_choice(id o, SEL) {
    auto *v = c(o);
    return v->choice_menu ? (v->choice_selection ? send(v->choice_menu, "indexOfItem:", ret(v->choice_selection)) : std::uint64_t(-1))
                          : std::uint64_t(v->selected);
}
std::uint64_t select_choice(id o, SEL, std::int64_t index) {
    auto *v = c(o);
    if (index < -1 || index >= std::int64_t(choice_count(o, nullptr))) return 0;
    v->selected = index;
    if (v->choice_menu) {
        if (v->choice_selection) send(v->choice_selection, "setState:", 0);
        assign(v->choice_selection, choice_item(o, index));
        if (v->choice_selection) send(v->choice_selection, "setState:", 1);
    }
    send(o, "setTitle:", ret(title_at(o, index)));
    dirty(o);
    return 0;
}
std::uint64_t insert_choice(id o, SEL, id title, std::int64_t index) {
    id menu = popup_menu(o);
    if (index < 0 || index > std::int64_t(send(menu, "numberOfItems"))) return 0;
    id item = obj(send(reinterpret_cast<id>(cls("NSMenuItem")), "alloc"));
    const std::uint64_t args[]{ret(title), 0, ret(string(""))};
    item = obj(mm_objc_send(item, mm_objc_sel("initWithTitle:action:keyEquivalent:"), 3, args));
    send(menu, "insertItem:atIndex:", ret(item), std::uint64_t(index));
    release(item);
    if (!c(o)->choice_selection) select_choice(o, nullptr, 0);
    dirty(o);
    return 0;
}
std::uint64_t add_choice(id o, SEL, id title) {
    if (is_kind(o, cls("NSPopUpButton"))) return insert_choice(o, nullptr, title, choice_count(o, nullptr));
    auto *v = c(o);
    v->choices.push_back(retain(title));
    if (v->selected < 0) {
        v->selected = 0;
        send(o, "setTitle:", ret(title));
    }
    dirty(o);
    return 0;
}
std::uint64_t remove_choices(id o, SEL) {
    auto *v = c(o);
    if (v->choice_menu) send(v->choice_menu, "removeAllItems");
    assign(v->choice_selection, nullptr);
    for (id i : v->choices)
        release(i);
    v->choices.clear();
    v->selected = -1;
    send(o, "setTitle:", ret(string("")));
    return 0;
}
std::uint64_t choice_title(id o, SEL) {
    return ret(title_at(o, std::int64_t(selected_choice(o, nullptr))));
}
std::uint64_t select_title(id o, SEL, id title) {
    for (std::uint64_t i = 0; i < choice_count(o, nullptr); ++i)
        if (to_utf8(title_at(o, i)) == to_utf8(title))
            return select_choice(o, nullptr, i);
    return 0;
}
std::uint64_t get_choice_menu(id o, SEL) { return ret(popup_menu(o)); }
std::uint64_t set_choice_menu(id o, SEL, id menu) {
    if (menu && !is_kind(menu, cls("NSMenu"))) return 0;
    assign(c(o)->choice_menu, menu);
    assign(c(o)->choice_selection, nullptr);
    select_choice(o, nullptr, choice_count(o, nullptr) ? 0 : -1);
    return 0;
}
std::uint64_t get_choice_item(id o, SEL, std::int64_t index) { return ret(choice_item(o, index)); }
std::uint64_t get_selected_item(id o, SEL) { return ret(choice_item(o, std::int64_t(selected_choice(o, nullptr)))); }
std::uint64_t index_of_tag(id o, SEL, std::int64_t tag) {
    for (std::uint64_t i = 0; i < choice_count(o, nullptr); ++i)
        if (std::int64_t(send(choice_item(o, i), "tag")) == tag) return i;
    return std::uint64_t(-1);
}
std::uint64_t select_tag(id o, SEL, std::int64_t tag) {
    const auto index = std::int64_t(index_of_tag(o, nullptr, tag));
    if (index < 0) return 0;
    select_choice(o, nullptr, index);
    return 1;
}
std::uint64_t select_item(id o, SEL, id item) {
    return select_choice(o, nullptr, item ? std::int64_t(send(popup_menu(o), "indexOfItem:", ret(item))) : -1);
}
std::uint64_t choice_array(id o, SEL) { return send(popup_menu(o), "itemArray"); }
std::uint64_t add_titles(id o, SEL, id titles) {
    for (std::uint64_t i = 0; i < send(titles, "count"); ++i) add_choice(o, nullptr, obj(send(titles, "objectAtIndex:", i)));
    return 0;
}
std::uint64_t remove_choice(id o, SEL, std::int64_t index) {
    if (index < 0 || index >= std::int64_t(choice_count(o, nullptr))) return 0;
    bool selected = choice_item(o, index) == c(o)->choice_selection;
    send(popup_menu(o), "removeItemAtIndex:", std::uint64_t(index));
    if (selected) select_choice(o, nullptr, choice_count(o, nullptr) ? 0 : -1);
    dirty(o);
    return 0;
}
std::uint64_t choice_key(id o, SEL, id e) {
    auto key = as<event_object>(e)->keycode;
    auto *v = c(o);
    if (!v->enabled)
        return 0;
    if (key == 125 || key == 124)
        select_choice(o, nullptr, std::min(std::int64_t(selected_choice(o, nullptr)) + 1, std::int64_t(choice_count(o, nullptr)) - 1));
    else if (key == 126 || key == 123)
        select_choice(o, nullptr, std::max(std::int64_t(0), std::int64_t(selected_choice(o, nullptr)) - 1));
    else
        return 0;
    action(o);
    return 0;
}
struct choice_button_object {
    control_object control;
    id owner = nullptr;
    std::int64_t index = 0;
    ~choice_button_object() { release(owner); }
};
Class choice_button_class = nullptr;
Class choice_list_class = nullptr;
id previous_choice_popup = nullptr;
struct choice_list_object {
    view_object view;
    id owner = nullptr;
    std::int64_t highlighted = 0;
    ~choice_list_object() { release(owner); }
};
std::uint64_t choice_popup_closed(id, SEL, id window) {
    if (id parent = as<window_object>(window)->parent_window) send(parent, "removeChildWindow:", ret(window));
    if (previous_choice_popup == window) {
        previous_choice_popup = nullptr;
        release(window);
    }
    return 0;
}
std::uint64_t choice_list_key(id o, SEL, id e) {
    auto *list = as<choice_list_object>(o);
    const auto key = as<event_object>(e)->keycode;
    if (key == 53) { send(list->view.window, "close"); return 0; }
    auto &rows = list->view.subviews;
    if (rows.empty()) return 0;
    if (key == 36 || key == 76) {
        if (list->highlighted >= 0 && list->highlighted < std::int64_t(rows.size()))
            send(rows[list->highlighted], "performClick:", 0);
        return 0;
    }
    if (key != 125 && key != 126) return 0;
    auto index = list->highlighted;
    for (std::size_t n = 0; n < rows.size(); ++n) {
        index = (index + std::int64_t(rows.size()) + (key == 125 ? 1 : -1)) % std::int64_t(rows.size());
        if (c(rows[index])->enabled) break;
    }
    list->highlighted = index;
    for (std::size_t i = 0; i < rows.size(); ++i) c(rows[i])->highlighted = std::int64_t(i) == index;
    auto *clip = list->view.superview ? as<view_object>(list->view.superview) : nullptr;
    if (clip) clip->bounds.y = std::clamp(index * 26. - clip->bounds.h / 2, 0., std::max(0., list->view.frame.h - clip->bounds.h));
    dirty(o);
    return 0;
}
std::uint64_t choice_click(id o, SEL, id) {
    auto *b = as<choice_button_object>(o);
    if (!b->control.enabled) return 0;
    id owner = retain(b->owner);
    auto index = b->index;
    id window = retain(b->control.view.window);
    select_choice(owner, nullptr, index);
    send(window, "close");
    release(window);
    action(owner);
    release(owner);
    return 0;
}
std::uint64_t popup_mouse(id o, SEL, id event) {
    auto *v = c(o);
    const auto count = choice_count(o, nullptr);
    if (!v->enabled || !count || !v->view.window)
        return 0;
    send(v->view.window, "makeFirstResponder:", ret(o));
    if (previous_choice_popup) send(previous_choice_popup, "close");
    auto w = as<window_object>(v->view.window);
    rect anchor = v->view.bounds;
    if (auto *ctx = api()->current_context()) {
        const double values[]{anchor.x, anchor.y, anchor.w, anchor.h};
        for (int i = 0; i < 4; ++i) std::memcpy(&ctx->v[i][0], &values[i], 8);
        send(o, "convertRect:toView:", 0);
        double *fields[]{&anchor.x, &anchor.y, &anchor.w, &anchor.h};
        for (int i = 0; i < 4; ++i) std::memcpy(fields[i], &ctx->v[i][0], 8);
    }
    anchor.y = w->frame.h - anchor.y - anchor.h;
    double width = std::max(140., v->view.frame.w), height = std::min(std::uint64_t(20), count) * 26. + 4;
    id popup = make_window({0, 0, width, height}, 0);
    as<window_object>(popup)->popup = true;
    as<window_object>(popup)->popup_anchor = anchor;
    as<window_object>(popup)->popup_serial = event ? as<event_object>(event)->input_serial : 0;
    previous_choice_popup = retain(popup);
    auto *list = make<choice_list_object>(choice_list_class);
    id document = reinterpret_cast<id>(list);
    list->owner = retain(o);
    list->highlighted = std::max(std::int64_t(0), std::int64_t(selected_choice(o, nullptr)));
    list->view.frame = list->view.bounds = {0, 0, width, count * 26. + 4};
    id scroll = obj(send(reinterpret_cast<id>(cls("NSScrollView")), "alloc"));
    as<view_object>(scroll)->frame = as<view_object>(scroll)->bounds = {0, 0, width, height};
    if (auto *ctx = api()->current_context()) {
        const double values[]{0, 0, width, height};
        for (int i = 0; i < 4; ++i) std::memcpy(&ctx->v[i][0], &values[i], 8);
    }
    scroll = obj(send(scroll, "initWithFrame:"));
    send(scroll, "setDocumentView:", ret(document));
    send(popup, "setContentView:", ret(scroll));
    send(popup, "setDelegate:", ret(document));
    for (std::size_t n = 0; n < count; ++n) {
        auto *b = make<choice_button_object>(choice_button_class);
        id button = reinterpret_cast<id>(b);
        b->owner = retain(o);
        b->index = n;
        b->control.title = retain(title_at(o, n));
        if (auto item = choice_item(o, n)) b->control.enabled = send(item, "isEnabled") && !send(item, "isSeparatorItem");
        b->control.bordered = false;
        b->control.alignment = 0;
        b->control.menu_row = true;
        b->control.state = std::int64_t(n) == std::int64_t(selected_choice(o, nullptr));
        b->control.highlighted = std::int64_t(n) == list->highlighted;
        b->control.view.frame = {2, 2 + n * 26., width - 4, 26};
        b->control.view.bounds = {0, 0, width - 4, 26};
        send(document, "addSubview:", ret(button));
        release(button);
    }
    send(w->content_view, "setNeedsDisplay:", 1);
    send(v->view.window, "addChildWindow:ordered:", ret(popup), 1);
    send(popup, "makeKeyAndOrderFront:", 0);
    send(popup, "makeFirstResponder:", ret(document));
    release(document);
    release(scroll);
    release(popup);
    return 0;
}
std::uint64_t segment_count(id o, SEL, std::int64_t n) {
    auto *v = c(o);
    if (n < 0 || n > 1024)
        return 0;
    for (id i : v->choices)
        release(i);
    v->choices.assign(n, nullptr);
    dirty(o);
    return 0;
}
std::uint64_t segment_label(id o, SEL, id title, std::int64_t n) {
    auto *v = c(o);
    if (n >= 0 && n < std::int64_t(v->choices.size()))
        assign(v->choices[n], title);
    dirty(o);
    return 0;
}
std::uint64_t segment_mouse(id o, SEL, id event) {
    auto *v = c(o);
    if (!v->enabled || v->choices.empty())
        return 0;
    auto p = local_point(o, event);
    auto b = v->view.bounds;
    v->selected = std::clamp(std::int64_t((p.x - b.x) * v->choices.size() / std::max(1., b.w)), std::int64_t(0),
                             std::int64_t(v->choices.size()) - 1);
    dirty(o);
    action(o);
    return 0;
}
std::uint64_t scroll_content(id o, SEL) { return ret(as<scroll_object>(o)->content); }
std::uint64_t set_scroll_content(id o, SEL, id content) {
    auto *s = as<scroll_object>(o);
    if (s->content == content)
        return 0;
    if (s->content)
        send(s->content, "removeFromSuperview");
    s->content = content;
    if (content)
        send(o, "addSubview:", ret(content));
    dirty(o);
    return 0;
}
std::uint64_t scroll_init(id o, SEL, double x, double y, double w, double h) {
    auto *s = as<scroll_object>(o);
    s->view.frame = {x, y, w, h};
    s->view.bounds = {0, 0, w, h};
    id content = obj(send(reinterpret_cast<id>(cls("NSClipView")), "alloc"));
    as<view_object>(content)->frame = {1, 1, std::max(0., w - 2), std::max(0., h - 2)};
    as<view_object>(content)->bounds = {0, 0, std::max(0., w - 2), std::max(0., h - 2)};
    send(o, "setContentView:", ret(content));
    release(content);
    return ret(o);
}
std::uint64_t document(id o, SEL) {
    auto *v = as<view_object>(o);
    return v->subviews.empty() ? 0 : ret(v->subviews.front());
}
std::uint64_t set_document(id o, SEL, id doc) {
    auto *v = as<view_object>(o);
    auto subs = v->subviews;
    for (id i : subs)
        send(i, "removeFromSuperview");
    if (doc)
        send(o, "addSubview:", ret(doc));
    dirty(o);
    return 0;
}
std::uint64_t scroll_document(id o, SEL) { return send(as<scroll_object>(o)->content, "documentView"); }
std::uint64_t set_scroll_document(id o, SEL, id doc) {
    return send(as<scroll_object>(o)->content, "setDocumentView:", ret(doc));
}
std::uint64_t scroll_to(id o, SEL, double x, double y) {
    auto *v = as<view_object>(o);
    v->bounds.x = x;
    v->bounds.y = y;
    dirty(o);
    return 0;
}
std::uint64_t wheel(id o, SEL, id e) {
    id clip = o;
    while (clip && !is_kind(clip, cls("NSClipView")))
        clip = as<view_object>(clip)->superview;
    if (!clip)
        return 0;
    auto *v = as<view_object>(clip);
    id doc = obj(send(clip, "documentView"));
    if (!doc)
        return 0;
    auto f = as<view_object>(doc)->frame;
    auto *event = as<event_object>(e);
    v->bounds.x = std::clamp(v->bounds.x - event->dx * 12, 0., std::max(0., f.w - v->bounds.w));
    double delta = event->dy * 12;
    if (send(clip, "isFlipped"))
        delta = -delta;
    v->bounds.y = std::clamp(v->bounds.y + delta, 0., std::max(0., f.h - v->bounds.h));
    dirty(clip);
    return 0;
}
std::uint64_t table_reload(id o, SEL) {
    auto *t = as<table_object>(o);
    t->rows = t->data_source ? std::clamp(std::int64_t(send(t->data_source, "numberOfRowsInTableView:", ret(o))),
                                          std::int64_t(0), std::int64_t(1000000))
                             : 0;
    if (t->selected_row >= t->rows)
        t->selected_row = -1;
    dirty(o);
    return 0;
}
std::uint64_t table_rows(id o, SEL) { return as<table_object>(o)->rows; }
std::uint64_t table_selected(id o, SEL) { return std::uint64_t(as<table_object>(o)->selected_row); }
std::uint64_t table_source(id o, SEL) { return ret(as<table_object>(o)->data_source); }
std::uint64_t set_table_source(id o, SEL, id source) {
    as<table_object>(o)->data_source = source;
    return table_reload(o, nullptr);
}
std::uint64_t table_delegate(id o, SEL) { return ret(as<table_object>(o)->delegate); }
std::uint64_t set_table_delegate(id o, SEL, id delegate) {
    as<table_object>(o)->delegate = delegate;
    dirty(o);
    return 0;
}
std::uint64_t table_columns(id o, SEL) {
    id a = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id i : as<table_object>(o)->columns)
        send(a, "addObject:", ret(i));
    return ret(a);
}
std::uint64_t add_column(id o, SEL, id column) {
    if (column)
        as<table_object>(o)->columns.push_back(retain(column));
    dirty(o);
    return 0;
}
double row_height(id o, SEL) { return as<table_object>(o)->row_height; }
std::uint64_t set_row_height(id o, SEL, double h) {
    if (std::isfinite(h) && h > 0)
        as<table_object>(o)->row_height = h;
    dirty(o);
    return 0;
}
std::uint64_t alternating(id o, SEL, std::uint64_t v) {
    as<table_object>(o)->alternating = v & 255;
    dirty(o);
    return 0;
}
std::uint64_t table_mouse(id o, SEL, id e) {
    auto *t = as<table_object>(o);
    auto p = local_point(o, e);
    auto b = t->control.view.bounds;
    auto row = std::int64_t((p.y - b.y) / t->row_height);
    t->selected_row = row >= 0 && row < t->rows ? row : -1;
    if (t->delegate && responds(t->delegate, "tableViewSelectionDidChange:")) {
        id n =
            obj(send(reinterpret_cast<id>(cls("NSNotification")),
                     "notificationWithName:object:", ret(string("NSTableViewSelectionDidChangeNotification")), ret(o)));
        send(t->delegate, "tableViewSelectionDidChange:", ret(n));
    }
    dirty(o);
    if (as<event_object>(e)->click_count > 1 && t->control.double_action) {
        auto saved = t->control.action;
        t->control.action = t->control.double_action;
        action(o);
        t->control.action = saved;
    } else
        action(o);
    return 0;
}
std::uint64_t double_action(id o, SEL) { return ret(c(o)->double_action); }
std::uint64_t set_double_action(id o, SEL, SEL a) {
    c(o)->double_action = a;
    return 0;
}
std::uint64_t last_column(id o, SEL) {
    auto *t = as<table_object>(o);
    double used = 0;
    for (std::size_t i = 0; i + 1 < t->columns.size(); ++i)
        used += as<table_column_object>(t->columns[i])->width;
    if (!t->columns.empty())
        as<table_column_object>(t->columns.back())->width = std::max(1., t->control.view.bounds.w - used);
    dirty(o);
    return 0;
}
std::uint64_t make_table_view(id o, SEL, id identifier, id) {
    for (id v : as<table_object>(o)->reusable_views)
        if (to_utf8(obj(send(v, "identifier"))) == to_utf8(identifier))
            return ret(v);
    return 0;
}
std::uint64_t column_init(id o, SEL, id identifier) {
    assign(as<table_column_object>(o)->identifier, identifier);
    return ret(o);
}
std::uint64_t column_identifier(id o, SEL) { return ret(as<table_column_object>(o)->identifier); }
std::uint64_t set_column_identifier(id o, SEL, id identifier) {
    assign(as<table_column_object>(o)->identifier, identifier);
    return 0;
}
double column_width(id o, SEL) { return as<table_column_object>(o)->width; }
std::uint64_t set_column_width(id o, SEL, double w) {
    if (std::isfinite(w) && w > 0)
        as<table_column_object>(o)->width = w;
    return 0;
}
std::uint64_t system_font(id klass, SEL, double size) {
    auto *f = make<font_object>(reinterpret_cast<Class>(klass));
    f->size = size > 0 ? size : 13;
    return ret(autorelease(reinterpret_cast<id>(f)));
}
std::uint64_t bold_font(id klass, SEL, double size) {
    id f = obj(system_font(klass, nullptr, size));
    as<font_object>(f)->bold = true;
    return ret(f);
}
std::uint64_t named_font(id klass, SEL, id name, double size) {
    id f = obj(system_font(klass, nullptr, size));
    as<font_object>(f)->bold = to_utf8(name).find("Bold") != std::string::npos;
    return ret(f);
}
std::uint64_t mono_font(id klass,SEL,double size,double weight) {
    id f=obj(system_font(klass,nullptr,size));auto *font=as<font_object>(f);font->family="Monospace";font->bold=weight>0;return ret(f);
}
std::uint64_t tooltip_add(id o,SEL,double x,double y,double w,double h,id owner,void *data) {
    auto *view=as<view_object>(o);auto tag=view->next_tooltip_tag++;view->tooltips.push_back({{x,y,w,h},owner,data,tag});return std::uint64_t(tag);
}
std::uint64_t tooltip_remove(id o,SEL,std::int64_t tag){std::erase_if(as<view_object>(o)->tooltips,[&](const auto &tooltip){return tooltip.tag==tag;});return 0;}
double point_size(id o, SEL) { return as<font_object>(o)->size; }
} // namespace
void layout_cocoa_view(id view) {
    if (!view || !is_kind(view, cls("NSView")))
        return;
    auto *v = as<view_object>(view);
    const bool flipped = send(view, "isFlipped") != 0;
    std::vector<id> children = v->subviews;
    std::vector<layout::frame> frames;
    for (id o : children) {
        auto f = as<view_object>(o)->frame;
        frames.push_back(
            {f.x - v->bounds.x, flipped ? f.y - v->bounds.y : v->bounds.h - (f.y - v->bounds.y) - f.h, f.w, f.h});
        if (is_kind(o, cls("NSScrollView")) || is_kind(o, cls("NSTableView"))) {
            frames.back().width_stay = .0001;
            frames.back().height_stay = .0001;
        }
    }
    std::vector<layout::constraint> constraints;
    auto index = [&](id o) {
        if (!o || o == view)
            return -1;
        auto it = std::find(children.begin(), children.end(), o);
        return it == children.end() ? -2 : int(it - children.begin());
    };
    auto add = [&](const view_object::layout_constraint &c) {
        int a = index(c.first), b = index(c.second);
        if (a < -1 || b < -1)
            return;
        constraints.push_back(
            {a, c.attribute, b, c.second_attribute, c.relation, c.multiplier, c.constant, c.priority});
    };
    for (auto &c : v->constraints)
        add(c);
    for (id o : children)
        for (auto &c : as<view_object>(o)->constraints)
            add(c);
    layout::solve(frames, v->bounds.w, v->bounds.h, constraints);
    for (std::size_t i = 0; i < children.size(); ++i) {
        auto f = frames[i];
        auto old = as<view_object>(children[i])->frame;
        rect next{f.x + v->bounds.x, flipped ? f.y + v->bounds.y : v->bounds.h - f.y - f.h + v->bounds.y, f.w, f.h};
        if (!constraints.empty()) {
            auto aligned = layout::align_edges({next.x, next.y, next.w, next.h});
            next = {aligned.x, aligned.y, aligned.w, aligned.h};
        }
        if (std::abs(old.x - next.x) > .01 || std::abs(old.y - next.y) > .01 || std::abs(old.w - next.w) > .01 ||
            std::abs(old.h - next.h) > .01)
            set_frame(children[i], next);
    }
    if (is_kind(view, cls("NSScrollView"))) {
        auto *s = as<scroll_object>(view);
        if (s->content) {
            set_frame(s->content, {1, 1, std::max(0., v->bounds.w - 2), std::max(0., v->bounds.h - 2)});
            id doc = obj(send(s->content, "documentView"));
            if (doc && is_kind(doc, cls("NSTableView"))) {
                auto *t = as<table_object>(doc);
                auto f = t->control.view.frame;
                set_frame(doc, {f.x, f.y, std::max(f.w, v->bounds.w - 2),
                                std::max(v->bounds.h - 2, t->rows * t->row_height)});
            }
        }
    }
    for (id o : children)
        layout_cocoa_view(o);
}
void register_cocoa_controls(Class view, Class control) {
#define PROPERTY(klass, name, setter, fn, encoding)                                                                    \
    mm_objc_add_method(klass, name, MM_IMP(get_##fn), encoding "16@0:8");                                              \
    mm_objc_add_method(klass, setter, MM_IMP(set_##fn), "v24@0:8" encoding "16")
    PROPERTY(control, "isBordered", "setBordered:", bordered, "B");
    PROPERTY(control, "isBezeled", "setBezeled:", bezeled, "B");
    PROPERTY(control, "isEditable", "setEditable:", editable, "B");
    PROPERTY(control, "isSelectable", "setSelectable:", selectable, "B");
    PROPERTY(control, "drawsBackground", "setDrawsBackground:", background, "B");
    PROPERTY(control, "isIndeterminate", "setIndeterminate:", indeterminate, "B");
    PROPERTY(control, "buttonType", "setButtonType:", button_type, "q");
    PROPERTY(control, "bezelStyle", "setBezelStyle:", bezel, "q");
    PROPERTY(control, "alignment", "setAlignment:", alignment, "q");
    PROPERTY(control, "imageScaling", "setImageScaling:", scaling, "q");
    PROPERTY(control, "doubleValue", "setDoubleValue:", value, "d");
    PROPERTY(control, "minValue", "setMinValue:", minimum, "d");
    PROPERTY(control, "maxValue", "setMaxValue:", maximum, "d");
    PROPERTY(control, "knobProportion", "setKnobProportion:", knob, "d");
    PROPERTY(control, "identifier", "setIdentifier:", identifier, "@");
    PROPERTY(control, "textField", "setTextField:", text_field, "@");
    PROPERTY(control, "imageView", "setImageView:", image_view, "@");
    PROPERTY(control, "image", "setImage:", image, "@");
    PROPERTY(control, "placeholderString", "setPlaceholderString:", placeholder, "@");
    MM_METHODS(control, {"cell", MM_IMP(cell), "@16@0:8"}, {"font", MM_IMP(font), "@16@0:8"},
               {"setFont:", MM_IMP(set_font), "v24@0:8@16"});
    MM_METHODS(view, {"autoresizingMask", MM_IMP(mask), "Q16@0:8"},
               {"setAutoresizingMask:", MM_IMP(set_mask), "v24@0:8Q16"},
               {"autoresizesSubviews", MM_IMP(autoresizes), "B16@0:8"},
               {"setAutoresizesSubviews:", MM_IMP(set_autoresizes), "v24@0:8B16"},
               {"translatesAutoresizingMaskIntoConstraints", MM_IMP(translates), "B16@0:8"},
               {"setTranslatesAutoresizingMaskIntoConstraints:", MM_IMP(set_translates), "v24@0:8B16"},
               {"layout", MM_IMP(layout_view), "v16@0:8"}, {"layoutSubtreeIfNeeded", MM_IMP(layout_view), "v16@0:8"},
        {"addToolTipRect:owner:userData:",MM_IMP(tooltip_add),"q64@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48^v56"},
        {"removeToolTip:",MM_IMP(tooltip_remove),"v24@0:8q16"});
    auto define = [&](const char *name, Class base, mm_objc_class *storage, mm_objc_class *meta) {
        return mm_objc_define_class(name, base, sizeof(control_object), storage, meta);
    };
    define("NSImageView", control, MM_FW_CLASS_REF(NSImageView), MM_FW_META_REF(NSImageView));
    define("NSBox", control, MM_FW_CLASS_REF(NSBox), MM_FW_META_REF(NSBox));
    Class slider = define("NSSlider", control, MM_FW_CLASS_REF(NSSlider), MM_FW_META_REF(NSSlider));
    MM_METHODS(slider, {"mouseDown:", MM_IMP(slider_mouse), "v24@0:8@16"},
               {"mouseDragged:", MM_IMP(slider_mouse), "v24@0:8@16"});
    Class progress = define("NSProgressIndicator", control, MM_FW_CLASS_REF(NSProgressIndicator),
                            MM_FW_META_REF(NSProgressIndicator));
    MM_METHODS(progress, {"startAnimation:", MM_IMP(start), "v24@0:8@16"},
               {"stopAnimation:", MM_IMP(stop), "v24@0:8@16"}, {"incrementBy:", MM_IMP(increment), "v24@0:8d16"});
    static mm_objc_class choice_storage{}, choice_meta{};
    choice_button_class = mm_objc_define_class("__MMCocoaChoiceButton", cls("NSButton"), sizeof(choice_button_object),
                                               &choice_storage, &choice_meta);
    register_lifecycle<choice_button_object>(choice_button_class);
    MM_METHODS(choice_button_class, {"performClick:", MM_IMP(choice_click), "v24@0:8@16"});
    static mm_objc_class list_storage{}, list_meta{};
    choice_list_class = mm_objc_define_class("__MMCocoaChoiceList", view, sizeof(choice_list_object), &list_storage, &list_meta);
    register_lifecycle<choice_list_object>(choice_list_class);
    MM_METHODS(choice_list_class,
        {"isFlipped", MM_IMP(flipped), "B16@0:8"}, {"acceptsFirstResponder", MM_IMP(flipped), "B16@0:8"},
        {"keyDown:", MM_IMP(choice_list_key), "v24@0:8@16"},
        {"windowWillClose:", MM_IMP(choice_popup_closed), "v24@0:8@16"},
        {"scrollWheel:", MM_IMP(wheel), "v24@0:8@16"});
    Class combo = define("NSComboBox", cls("NSTextField"), MM_FW_CLASS_REF(NSComboBox), MM_FW_META_REF(NSComboBox));
    define("NSSecureTextField", cls("NSTextField"), MM_FW_CLASS_REF(NSSecureTextField),
           MM_FW_META_REF(NSSecureTextField));
    define("NSSearchField", cls("NSTextField"), MM_FW_CLASS_REF(NSSearchField), MM_FW_META_REF(NSSearchField));
    MM_METHODS(cls("NSTextField"), {"mouseDown:", MM_IMP(text_mouse), "v24@0:8@16"},
               {"keyDown:", MM_IMP(text_key), "v24@0:8@16"},
               {"acceptsFirstResponder", MM_IMP(accepts_text), "B16@0:8"});
    for (Class klass : {combo, cls("NSPopUpButton")})
        MM_METHODS(klass, {"addItemWithTitle:", MM_IMP(add_choice), "v24@0:8@16"},
                   {"addItemWithObjectValue:", MM_IMP(add_choice), "v24@0:8@16"},
                   {"removeAllItems", MM_IMP(remove_choices), "v16@0:8"},
                   {"selectItemAtIndex:", MM_IMP(select_choice), "v24@0:8q16"},
                   {"selectItemWithTitle:", MM_IMP(select_title), "v24@0:8@16"},
                   {"indexOfSelectedItem", MM_IMP(selected_choice), "q16@0:8"},
                   {"numberOfItems", MM_IMP(choice_count), "q16@0:8"},
                   {"titleOfSelectedItem", MM_IMP(choice_title), "@16@0:8"},
                   {"keyDown:", MM_IMP(choice_key), "v24@0:8@16"}, {"mouseDown:", MM_IMP(popup_mouse), "v24@0:8@16"});
    MM_METHODS(cls("NSPopUpButton"),
        {"menu", MM_IMP(get_choice_menu), "@16@0:8"}, {"setMenu:", MM_IMP(set_choice_menu), "v24@0:8@16"},
        {"insertItemWithTitle:atIndex:", MM_IMP(insert_choice), "v32@0:8@16q24"},
        {"itemAtIndex:", MM_IMP(get_choice_item), "@24@0:8q16"},
        {"selectedItem", MM_IMP(get_selected_item), "@16@0:8"},
        {"indexOfItemWithTag:", MM_IMP(index_of_tag), "q24@0:8q16"},
        {"selectItemWithTag:", MM_IMP(select_tag), "B24@0:8q16"},
        {"selectItem:", MM_IMP(select_item), "v24@0:8@16"},
        {"itemArray", MM_IMP(choice_array), "@16@0:8"},
        {"addItemsWithTitles:", MM_IMP(add_titles), "v24@0:8@16"},
        {"removeItemAtIndex:", MM_IMP(remove_choice), "v24@0:8q16"});
    Class segments =
        define("NSSegmentedControl", control, MM_FW_CLASS_REF(NSSegmentedControl), MM_FW_META_REF(NSSegmentedControl));
    MM_METHODS(segments, {"setSegmentCount:", MM_IMP(segment_count), "v24@0:8q16"},
               {"segmentCount", MM_IMP(choice_count), "q16@0:8"},
               {"setLabel:forSegment:", MM_IMP(segment_label), "v32@0:8@16q24"},
               {"selectedSegment", MM_IMP(selected_choice), "q16@0:8"},
               {"setSelectedSegment:", MM_IMP(select_choice), "v24@0:8q16"},
               {"mouseDown:", MM_IMP(segment_mouse), "v24@0:8@16"});
    Class clip = mm_objc_define_class("NSClipView", view, sizeof(view_object), MM_FW_CLASS_REF(NSClipView),
                                      MM_FW_META_REF(NSClipView));
    MM_METHODS(
        clip, {"documentView", MM_IMP(document), "@16@0:8"}, {"setDocumentView:", MM_IMP(set_document), "v24@0:8@16"},
        {"scrollToPoint:", MM_IMP(scroll_to), "v32@0:8{CGPoint=dd}16"}, {"scrollWheel:", MM_IMP(wheel), "v24@0:8@16"});
    Class scroll = mm_objc_define_class("NSScrollView", view, sizeof(scroll_object), MM_FW_CLASS_REF(NSScrollView),
                                        MM_FW_META_REF(NSScrollView));
    register_lifecycle<scroll_object>(scroll);
    MM_METHODS(scroll, {"initWithFrame:", MM_IMP(scroll_init), "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
               {"contentView", MM_IMP(scroll_content), "@16@0:8"},
               {"setContentView:", MM_IMP(set_scroll_content), "v24@0:8@16"},
               {"documentView", MM_IMP(scroll_document), "@16@0:8"},
               {"setDocumentView:", MM_IMP(set_scroll_document), "v24@0:8@16"});
    Class table = mm_objc_define_class("NSTableView", control, sizeof(table_object), MM_FW_CLASS_REF(NSTableView),
                                       MM_FW_META_REF(NSTableView));
    register_lifecycle<table_object>(table);
    mm_objc_define_class("NSOutlineView", table, sizeof(table_object), MM_FW_CLASS_REF(NSOutlineView),
                         MM_FW_META_REF(NSOutlineView));
    MM_METHODS(
        table, {"isFlipped", MM_IMP(flipped), "B16@0:8"}, {"reloadData", MM_IMP(table_reload), "v16@0:8"},
        {"numberOfRows", MM_IMP(table_rows), "q16@0:8"}, {"selectedRow", MM_IMP(table_selected), "q16@0:8"},
        {"dataSource", MM_IMP(table_source), "@16@0:8"}, {"setDataSource:", MM_IMP(set_table_source), "v24@0:8@16"},
        {"delegate", MM_IMP(table_delegate), "@16@0:8"}, {"setDelegate:", MM_IMP(set_table_delegate), "v24@0:8@16"},
        {"doubleAction", MM_IMP(double_action), ":16@0:8"},
        {"setDoubleAction:", MM_IMP(set_double_action), "v24@0:8:16"},
        {"sizeLastColumnToFit", MM_IMP(last_column), "v16@0:8"}, {"tableColumns", MM_IMP(table_columns), "@16@0:8"},
        {"addTableColumn:", MM_IMP(add_column), "v24@0:8@16"}, {"rowHeight", MM_IMP(row_height), "d16@0:8"},
        {"setRowHeight:", MM_IMP(set_row_height), "v24@0:8d16"},
        {"setUsesAlternatingRowBackgroundColors:", MM_IMP(alternating), "v24@0:8B16"},
        {"mouseDown:", MM_IMP(table_mouse), "v24@0:8@16"}, {"scrollWheel:", MM_IMP(wheel), "v24@0:8@16"},
        {"makeViewWithIdentifier:owner:", MM_IMP(make_table_view), "@32@0:8@16@24"});
    Class column = mm_objc_define_class("NSTableColumn", cls("NSObject"), sizeof(table_column_object),
                                        MM_FW_CLASS_REF(NSTableColumn), MM_FW_META_REF(NSTableColumn));
    register_lifecycle<table_column_object>(column);
    MM_METHODS(column, {"initWithIdentifier:", MM_IMP(column_init), "@24@0:8@16"},
               {"identifier", MM_IMP(column_identifier), "@16@0:8"},
               {"setIdentifier:", MM_IMP(set_column_identifier), "v24@0:8@16"},
               {"width", MM_IMP(column_width), "d16@0:8"}, {"setWidth:", MM_IMP(set_column_width), "v24@0:8d16"});
    define("NSTableCellView", control, MM_FW_CLASS_REF(NSTableCellView), MM_FW_META_REF(NSTableCellView));
    define("NSTableRowView", control, MM_FW_CLASS_REF(NSTableRowView), MM_FW_META_REF(NSTableRowView));
    Class font_class = mm_objc_define_class("NSFont", cls("NSObject"), sizeof(font_object), MM_FW_CLASS_REF(NSFont),
                                            MM_FW_META_REF(NSFont));
    register_lifecycle<font_object>(font_class);
    MM_CLASS_METHODS(font_class, {"systemFontOfSize:", MM_IMP(system_font), "@24@0:8d16"},
                     {"boldSystemFontOfSize:", MM_IMP(bold_font), "@24@0:8d16"},
                     {"fontWithName:size:", MM_IMP(named_font), "@32@0:8@16d24"},
        {"monospacedDigitSystemFontOfSize:weight:",MM_IMP(mono_font),"@32@0:8d16d24"},
        {"monospacedSystemFontOfSize:weight:",MM_IMP(mono_font),"@32@0:8d16d24"});
    MM_METHODS(font_class, {"pointSize", MM_IMP(point_size), "d16@0:8"});
}
} // namespace ak
