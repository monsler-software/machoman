#include "appkit.h"
#ifdef MM_APPKIT_PANGO
#include <pango/pangocairo.h>
#endif
#include "cocoa_layout.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

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
                 choice_menu, choice_selection, attributed_title})
        release(o);
    for (id o : choices)
        release(o);
}
table_object::~table_object() {
    for (id o : columns)
        release(o);
    for (auto &[key, view] : cell_views)
        release(view);
}
id table_cell_view(id table, id column, std::int64_t row) {
    auto *t = as<table_object>(table);
    const auto key = std::make_pair(row, column);
    if (auto it = t->cell_views.find(key); it != t->cell_views.end())
        return it->second;
    id view = nullptr;
    if (t->delegate && responds(t->delegate, "tableView:viewForTableColumn:row:"))
        view = obj(send(t->delegate, "tableView:viewForTableColumn:row:", ret(table), ret(column), row));
    if (view && is_kind(view, cls("NSView"))) {
        t->cell_views.emplace(key, retain(view));
        return view;
    }
    return nullptr;
}
id table_data_cell(id column) {
    auto *c = as<table_column_object>(column);
    // Host text controls also implement the NSCell text API.
    if (!c->data_cell) c->data_cell = obj(send(reinterpret_cast<id>(cls("NSTextField")), "new"));
    return c->data_cell;
}
id prepare_table_data_cell(id table, id column, std::int64_t row) {
    auto *t = as<table_object>(table);
    id cell = table_data_cell(column);
    id value = nullptr;
    if (t->data_source && responds(t->data_source,"tableView:objectValueForTableColumn:row:"))
        value = obj(send(t->data_source,"tableView:objectValueForTableColumn:row:",ret(table),ret(column),row));
    if (value && is_kind(value,cls("NSAttributedString"))) send(cell,"setAttributedStringValue:",ret(value));
    else send(cell,"setStringValue:",value ? send(value,"description") : ret(string("")));
    if (t->delegate && responds(t->delegate,"tableView:willDisplayCell:forTableColumn:row:")) {
        const std::uint64_t args[]{ret(table),ret(cell),ret(column),static_cast<std::uint64_t>(row)};
        mm_objc_send(t->delegate,mm_objc_sel("tableView:willDisplayCell:forTableColumn:row:"),4,args);
    }
    return cell;
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
    id content = nullptr, horizontal_scroller = nullptr, vertical_scroller = nullptr; // Retained by subviews.
    bool horizontal = false, vertical = false, autohides = false;
    std::int64_t border_type = 2;
};
void tile_scroll(id o);
void fit_table_columns(table_object &table, double width) {
    if (!table.column_autoresizing_style) return;
    std::vector<table_column_object *> columns;
    double total = 0;
    for (id column : table.columns) {
        auto *c = as<table_column_object>(column); total += c->width;
        if (c->resizing_mask&1) columns.push_back(c);
    }
    const auto style = table.column_autoresizing_style;
    if (style==2 || style==4) std::reverse(columns.begin(),columns.end());
    double remaining = width-total;
    if (style==1) {
        while (!columns.empty() && std::abs(remaining)>.01) {
            const double share = remaining/columns.size();
            double applied = 0;
            for (auto *c : columns) {
                const double next = std::clamp(c->width+share,c->minimum_width,c->maximum_width);
                applied += next-c->width; c->width = next;
            }
            remaining -= applied;
            std::erase_if(columns,[&](auto *c) { return remaining>0 ? c->width>=c->maximum_width : c->width<=c->minimum_width; });
            if (std::abs(applied)<.01) break;
        }
    } else for (auto *c : columns) {
        const double next = std::clamp(c->width+remaining,c->minimum_width,c->maximum_width);
        remaining -= next-c->width; c->width = next;
        if (style==4 || style==5 || std::abs(remaining)<.01) break;
    }
}
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
    const auto key = send(e,"keyCode");
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
std::uint64_t choice_list_mouse(id o, SEL, id event) {
    auto *list = as<choice_list_object>(o);
    const auto point = local_point(o,event);
    auto index = std::int64_t(std::floor((point.y-2)/26));
    auto &rows = list->view.subviews;
    if (point.x < 2 || point.x >= list->view.bounds.w-2 || index < 0 || index >= std::int64_t(rows.size()) ||
        !c(rows[index])->enabled) index = -1;
    if (list->highlighted == index) return 0;
    list->highlighted = index;
    for (std::size_t i = 0; i < rows.size(); ++i) c(rows[i])->highlighted = std::int64_t(i) == index;
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
    id doc = obj(document(o,nullptr));
    const auto f = doc ? as<view_object>(doc)->frame : rect{};
    v->bounds.x = std::clamp(x,0.,std::max(0.,f.w-v->bounds.w));
    v->bounds.y = std::clamp(y,0.,std::max(0.,f.h-v->bounds.h));
    if (v->superview && is_kind(v->superview,cls("NSScrollView"))) tile_scroll(v->superview);
    dirty(o);
    return 0;
}
void tile_scroll(id o) {
    auto *s = as<scroll_object>(o);
    if (!s->content) return;
    const double border = s->border_type ? 1. : 0.;
    const double width = std::max(0.,s->view.bounds.w-2*border), height = std::max(0.,s->view.bounds.h-2*border);
    id doc = obj(document(s->content,nullptr));
    rect extent = doc ? as<view_object>(doc)->frame : rect{};
    auto *table = doc && is_kind(doc,cls("NSTableView")) ? as<table_object>(doc) : nullptr;
    if (table) {
        extent.w = 0;
        for (id column : table->columns) extent.w += as<table_column_object>(column)->width;
        extent.h = table->rows*table->row_height;
    }
    bool vertical = s->vertical && !s->autohides, horizontal = s->horizontal && !s->autohides;
    constexpr double bar = 17;
    for (int pass=0; pass<3; ++pass) {
        vertical = vertical || (s->vertical && extent.h>height-(horizontal?bar:0)+.01);
        if (table) {
            fit_table_columns(*table,width-(vertical?bar:0));
            extent.w = 0;
            for (id column : table->columns) extent.w += as<table_column_object>(column)->width;
        }
        horizontal = horizontal || (s->horizontal && extent.w>width-(vertical?bar:0)+.01);
    }
    const double clip_w = std::max(0.,width-(vertical?bar:0)), clip_h = std::max(0.,height-(horizontal?bar:0));
    // Scroll views use a bottom-left origin; the horizontal bar sits below the clip view.
    set_frame(s->content,{border,border+(horizontal?bar:0),clip_w,clip_h});
    if (table) set_frame(doc,{0,0,std::max(clip_w,extent.w),std::max(clip_h,extent.h)});
    auto *clip = as<view_object>(s->content);
    const auto frame = doc ? as<view_object>(doc)->frame : rect{};
    clip->bounds.x = std::clamp(clip->bounds.x,0.,std::max(0.,frame.w-clip_w));
    clip->bounds.y = std::clamp(clip->bounds.y,0.,std::max(0.,frame.h-clip_h));
    auto configure = [&](id scroller, bool visible, bool is_vertical) {
        if (!scroller) return;
        auto *control = c(scroller);
        control->view.hidden = !visible;
        if (!visible) return;
        set_frame(scroller,is_vertical ? rect{border+clip_w,border+(horizontal?bar:0),bar,clip_h}
                                      : rect{border,border,clip_w,bar});
        const double size = is_vertical ? frame.h : frame.w, viewport = is_vertical ? clip_h : clip_w;
        const double offset = is_vertical ? clip->bounds.y : clip->bounds.x;
        control->knob = size>0 ? std::min(1.,viewport/size) : 1.;
        control->enabled = size>viewport;
        control->minimum = 0; control->maximum = 1;
        control->value = size>viewport ? offset/(size-viewport) : 0;
        if (is_vertical && !send(s->content,"isFlipped")) control->value = 1-control->value;
        control->vertical = is_vertical;
    };
    configure(s->vertical_scroller,vertical,true);
    configure(s->horizontal_scroller,horizontal,false);
}
std::uint64_t scroll_tile(id o, SEL) { tile_scroll(o); dirty(o); return 0; }
std::uint64_t scroll_reflect(id o, SEL, id) { return scroll_tile(o,nullptr); }
std::uint64_t scroll_scroller(id o, SEL, id scroller) {
    auto *s = as<scroll_object>(o);
    id doc = obj(document(s->content,nullptr));
    if (!doc || !scroller) return 0;
    auto *clip = as<view_object>(s->content);
    const auto f = as<view_object>(doc)->frame;
    double fraction = std::clamp(c(scroller)->value,0.,1.);
    if (scroller==s->vertical_scroller && !send(s->content,"isFlipped")) fraction = 1-fraction;
    return scroll_to(s->content,nullptr,
                     scroller==s->horizontal_scroller ? fraction*std::max(0.,f.w-clip->bounds.w) : clip->bounds.x,
                     scroller==s->vertical_scroller ? fraction*std::max(0.,f.h-clip->bounds.h) : clip->bounds.y);
}
std::uint64_t get_vertical_scroller(id o, SEL) { return ret(as<scroll_object>(o)->vertical_scroller); }
std::uint64_t get_horizontal_scroller(id o, SEL) { return ret(as<scroll_object>(o)->horizontal_scroller); }
void set_scroller(id o, id value, bool vertical) {
    auto *s = as<scroll_object>(o);
    id &slot = vertical ? s->vertical_scroller : s->horizontal_scroller;
    if (slot==value) return;
    retain(value);
    if (slot) send(slot,"removeFromSuperview");
    slot = value;
    if (value) {
        send(o,"addSubview:",ret(value));
        c(value)->target = o; c(value)->action = mm_objc_sel("_machomanScroll:");
    }
    release(value); dirty(o);
}
std::uint64_t set_vertical_scroller(id o, SEL, id value) { set_scroller(o,value,true); return 0; }
std::uint64_t set_horizontal_scroller(id o, SEL, id value) { set_scroller(o,value,false); return 0; }
std::uint64_t get_has_vertical(id o, SEL) { return as<scroll_object>(o)->vertical; }
std::uint64_t get_has_horizontal(id o, SEL) { return as<scroll_object>(o)->horizontal; }
std::uint64_t set_has_scroller(id o, bool vertical, std::uint64_t value) {
    auto *s = as<scroll_object>(o);
    (vertical ? s->vertical : s->horizontal) = (value&255)!=0;
    if (value&255) if (!(vertical ? s->vertical_scroller : s->horizontal_scroller)) {
        id scroller = obj(send(reinterpret_cast<id>(cls("NSScroller")),"new"));
        set_scroller(o,scroller,vertical); release(scroller);
    }
    return scroll_tile(o,nullptr);
}
std::uint64_t set_has_vertical(id o, SEL, std::uint64_t value) { return set_has_scroller(o,true,value); }
std::uint64_t set_has_horizontal(id o, SEL, std::uint64_t value) { return set_has_scroller(o,false,value); }
std::uint64_t get_autohides(id o, SEL) { return as<scroll_object>(o)->autohides; }
std::uint64_t set_autohides(id o, SEL, std::uint64_t value) { as<scroll_object>(o)->autohides=(value&255)!=0; return scroll_tile(o,nullptr); }
std::uint64_t get_scroll_border(id o, SEL) { return as<scroll_object>(o)->border_type; }
std::uint64_t set_scroll_border(id o, SEL, std::int64_t value) { as<scroll_object>(o)->border_type=value; return scroll_tile(o,nullptr); }
std::uint64_t scroller_mouse(id o, SEL, id event) {
    auto *control = c(o);
    if (!control->enabled) return 0;
    const auto p = local_point(o,event);
    const auto bounds = control->view.bounds;
    const double length = control->vertical ? bounds.h : bounds.w;
    const double position = control->vertical ? p.y-bounds.y : p.x-bounds.x;
    control->value = std::clamp((position-length*control->knob/2)/std::max(1.,length*(1-control->knob)),0.,1.);
    action(o); dirty(o); return 0;
}
std::uint64_t clip_flipped(id o, SEL) { return send(obj(document(o,nullptr)),"isFlipped"); }
std::uint64_t wheel(id o, SEL, id e) {
    id clip = is_kind(o,cls("NSScrollView")) ? obj(scroll_content(o,nullptr)) : o;
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
    return scroll_to(clip,nullptr,v->bounds.x,v->bounds.y);
}
std::uint64_t table_reload(id o, SEL) {
    auto *t = as<table_object>(o);
    auto previous = std::move(t->cell_views);
    t->cell_views.clear();
    for (auto &[key, view] : previous)
        release(view);
    t->rows = t->data_source ? std::clamp(std::int64_t(send(t->data_source, "numberOfRowsInTableView:", ret(o))),
                                          std::int64_t(0), std::int64_t(1000000))
                             : 0;
    std::erase_if(t->selected_rows,[&](auto row){ return row >= t->rows; });
    t->selected_row = t->selected_rows.empty() ? -1 : *t->selected_rows.begin();
    dirty(o);
    return 0;
}
std::uint64_t table_rows(id o, SEL) { return as<table_object>(o)->rows; }
std::uint64_t table_selected(id o, SEL) { return std::uint64_t(as<table_object>(o)->selected_row); }
void table_selection_changed(id o) {
    auto *t = as<table_object>(o);
    t->selected_row = t->selected_rows.empty() ? -1 : *t->selected_rows.begin();
    dirty(o);
    id notification = obj(send(reinterpret_cast<id>(cls("NSNotification")),"notificationWithName:object:",
                               ret(string("NSTableViewSelectionDidChangeNotification")),ret(o)));
    if (t->delegate && responds(t->delegate,"tableViewSelectionDidChange:"))
        send(t->delegate,"tableViewSelectionDidChange:",ret(notification));
    id center = obj(send(reinterpret_cast<id>(cls("NSNotificationCenter")),"defaultCenter"));
    send(center,"postNotification:",ret(notification));
}
std::uint64_t table_select_indexes(id o, SEL, id indexes, std::uint64_t extend) {
    auto *t = as<table_object>(o);
    auto selected = (extend & 255) ? t->selected_rows : std::set<std::int64_t>{};
    constexpr std::uint64_t not_found = 0x7fffffffffffffffull;
    for (auto row = indexes ? send(indexes,"firstIndex") : not_found;
         row < static_cast<std::uint64_t>(t->rows); row = send(indexes,"indexGreaterThanIndex:",row)) {
        if (!t->allows_multiple_selection) selected.clear();
        selected.insert(static_cast<std::int64_t>(row));
        if (!t->allows_multiple_selection) break;
    }
    if (selected.empty() && !t->allows_empty_selection) return 0;
    if (selected != t->selected_rows) { t->selected_rows=std::move(selected); table_selection_changed(o); }
    return 0;
}
std::uint64_t table_selected_indexes(id o, SEL) {
    id result = obj(send(reinterpret_cast<id>(cls("NSMutableIndexSet")),"indexSet"));
    for (auto row : as<table_object>(o)->selected_rows) send(result,"addIndex:",static_cast<std::uint64_t>(row));
    return ret(autorelease(obj(send(result,"copy"))));
}
std::uint64_t table_selected_count(id o, SEL) { return as<table_object>(o)->selected_rows.size(); }
std::uint64_t table_row_selected(id o, SEL, std::int64_t row) { return as<table_object>(o)->selected_rows.contains(row); }
std::uint64_t table_deselect_all(id o, SEL, id) {
    auto *t = as<table_object>(o);
    if (t->allows_empty_selection && !t->selected_rows.empty()) { t->selected_rows.clear(); table_selection_changed(o); }
    return 0;
}
std::uint64_t table_scroll_row(id o, SEL, std::int64_t row) {
    auto *t = as<table_object>(o);
    if (row<0 || row>=t->rows) return 0;
    id clip = t->control.view.superview;
    while (clip && !is_kind(clip,cls("NSClipView"))) clip = as<view_object>(clip)->superview;
    if (!clip) return 0;
    auto *v = as<view_object>(clip);
    const double top = row*t->row_height, bottom = top+t->row_height;
    const double current = v->bounds.y;
    const double next = top<current ? top : bottom>current+v->bounds.h ? bottom-v->bounds.h : current;
    return scroll_to(clip,nullptr,v->bounds.x,std::max(0.,next));
}
std::uint64_t table_multiple(id o, SEL) { return as<table_object>(o)->allows_multiple_selection; }
std::uint64_t table_set_multiple(id o, SEL, std::uint64_t value) {
    auto *t=as<table_object>(o); t->allows_multiple_selection=(value&255)!=0;
    if (!t->allows_multiple_selection && t->selected_rows.size()>1) {
        const auto first=*t->selected_rows.begin(); t->selected_rows={first}; table_selection_changed(o);
    }
    return 0;
}
std::uint64_t table_empty(id o, SEL) { return as<table_object>(o)->allows_empty_selection; }
std::uint64_t table_column_style(id o, SEL) { return as<table_object>(o)->column_autoresizing_style; }
std::uint64_t table_set_column_style(id o, SEL, std::uint64_t style) {
    if (style<=5) as<table_object>(o)->column_autoresizing_style=style;
    dirty(o); return 0;
}
std::uint64_t table_set_empty(id o, SEL, std::uint64_t value) { as<table_object>(o)->allows_empty_selection=(value&255)!=0; return 0; }
std::uint64_t table_prepared_cell(id o, SEL, std::int64_t column, std::int64_t row) {
    auto *t=as<table_object>(o);
    if(column<0 || column>=static_cast<std::int64_t>(t->columns.size()) || row<0 || row>=t->rows) return 0;
    return ret(prepare_table_data_cell(o,t->columns[column],row));
}
std::uint64_t table_source(id o, SEL) { return ret(as<table_object>(o)->data_source); }
std::uint64_t set_table_source(id o, SEL, id source) {
    as<table_object>(o)->data_source = source;
    return table_reload(o, nullptr);
}
std::uint64_t table_delegate(id o, SEL) { return ret(as<table_object>(o)->delegate); }
std::uint64_t set_table_delegate(id o, SEL, id delegate) {
    as<table_object>(o)->delegate = delegate;
    return table_reload(o, nullptr);
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
    id indexes = obj(send(reinterpret_cast<id>(cls("NSIndexSet")),"indexSetWithIndex:",
                          row>=0 && row<t->rows ? static_cast<std::uint64_t>(row) : 0x7fffffffffffffffull));
    table_select_indexes(o,nullptr,indexes,0);
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
std::uint64_t make_table_view(id o, SEL, id identifier, id owner) {
    auto *t = as<table_object>(o);
    const auto name = to_utf8(identifier);
    auto it = t->reusable_nibs.find(name);
    if (it == t->reusable_nibs.end()) return 0;
    std::vector<id> objects;
    if (!load_nib_data(it->second, owner, &objects)) return 0;
    id result = nullptr;
    for (id view : objects)
        if (is_kind(view, cls("NSView")) &&
            to_utf8(obj(send(view, "identifier"))) == name) {
            result = view;
            break;
        }
    // Each instantiation has its own outlets and subviews; visible rows must
    // never overwrite the contents of another row's template instance.
    for (id object : objects) autorelease(object);
    if (result) return ret(result);
    return 0;
}
std::uint64_t column_init(id o, SEL, id identifier) {
    assign(as<table_column_object>(o)->identifier, identifier);
    return ret(o);
}
std::uint64_t column_identifier(id o, SEL) { return ret(as<table_column_object>(o)->identifier); }
std::uint64_t column_cell(id o, SEL) { return ret(table_data_cell(o)); }
std::uint64_t column_set_cell(id o, SEL, id cell) { assign(as<table_column_object>(o)->data_cell,cell); return 0; }
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
std::uint64_t box_content(id o, SEL) { return ret(c(o)->box_content); }
std::uint64_t set_box_content(id o, SEL, id content) {
    if (c(o)->box_content == content) return 0;
    id next = retain(content), old = c(o)->box_content;
    c(o)->box_content = next;
    if (old) send(old,"removeFromSuperview");
    if (next) { send(o,"addSubview:",ret(next)); release(next); }
    layout_cocoa_view(o);
    dirty(o);
    return 0;
}
INT_PROPERTY(box_type, box_type)
INT_PROPERTY(title_position, title_position)
void wrapping_text_size(id view, layout::frame &frame) {
#ifdef MM_APPKIT_PANGO
    if (!is_kind(view,cls("NSTextField"))) return;
    const auto *control = c(view);
    if (control->view.translates_mask || !control->wraps || control->editable || control->bordered || !control->title) return;
    const auto contents = to_utf8(control->title);
    const auto key = contents+'\0'+control->font_family+':'+std::to_string(control->font_size)+':'+
                     std::to_string(control->bold)+':'+std::to_string(std::floor(frame.w-4));
    static thread_local std::unordered_map<std::string,int> measured;
    if (auto found=measured.find(key); found!=measured.end()) { frame.h=std::max(frame.h,double(found->second)); return; }
    auto *context = pango_font_map_create_context(pango_cairo_font_map_get_default());
    auto *text = pango_layout_new(context);
    auto *font = pango_font_description_new();
    pango_font_description_set_family(font,control->font_family.c_str());
    pango_font_description_set_absolute_size(font,control->font_size*PANGO_SCALE);
    pango_font_description_set_weight(font,control->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(text,font);
    pango_layout_set_text(text,contents.c_str(),static_cast<int>(contents.size()));
    pango_layout_set_width(text,std::max(1,static_cast<int>(std::floor(frame.w-4)))*PANGO_SCALE);
    pango_layout_set_wrap(text,PANGO_WRAP_WORD_CHAR);
    int height = 0;
    pango_layout_get_pixel_size(text,nullptr,&height);
    frame.h = std::max(frame.h,static_cast<double>(height));
    if (measured.size()>=256) measured.clear();
    measured.emplace(key,height);
    pango_font_description_free(font); g_object_unref(text); g_object_unref(context);
#else
    (void)view; (void)frame;
#endif
}
// A constraint installed on a common ancestor may connect controls in
// different boxes. Solve those descendants in the ancestor's coordinates;
// treating them as missing siblings silently drops width/alignment links.
bool layout_descendants(id root) {
    auto *container = as<view_object>(root);
    auto direct = [&](id o) {
        return !o || o == root || std::find(container->subviews.begin(), container->subviews.end(), o) != container->subviews.end();
    };
    bool cross = false;
    for (auto &c : container->constraints) cross |= !direct(c.first) || !direct(c.second);
    if (!cross) return false;
    std::vector<id> nodes;
    std::vector<int> parents;
    std::vector<layout::frame> frames;
    auto gather = [&](auto &&self, id parent, int index, double x, double y, int depth) -> void {
        if (depth > 128) return;
        auto *p = as<view_object>(parent);
        const bool flipped = send(parent, "isFlipped") != 0;
        for (id child : p->subviews) {
            auto f = as<view_object>(child)->frame;
            layout::frame g{x + f.x - p->bounds.x,
                            y + (flipped ? f.y - p->bounds.y : p->bounds.h - (f.y - p->bounds.y) - f.h), f.w, f.h};
            wrapping_text_size(child,g);
            int next = int(nodes.size());
            nodes.push_back(child); parents.push_back(index); frames.push_back(g);
            // A scroll/document view has its own coordinate and layout scope.
            if (!is_kind(child, cls("NSScrollView")) && !is_kind(child, cls("NSTableView")))
                self(self, child, next, g.x, g.y, depth + 1);
        }
    };
    gather(gather, root, -1, 0, 0, 0);
    auto index = [&](id o) {
        if (!o || o == root) return -1;
        auto it = std::find(nodes.begin(), nodes.end(), o);
        return it == nodes.end() ? -2 : int(it - nodes.begin());
    };
    std::vector<layout::constraint> constraints;
    std::size_t unresolved = 0;
    auto add = [&](id owner) {
        for (auto &c : as<view_object>(owner)->constraints) {
            int a = index(c.first), b = index(c.second);
            if (a >= -1 && b >= -1)
                constraints.push_back({a,c.attribute,b,c.second_attribute,c.relation,c.multiplier,c.constant,c.priority});
            else ++unresolved;
        }
    };
    add(root);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        add(nodes[i]);
        const int parent = parents[i];
        id p = parent < 0 ? root : nodes[parent];
        auto f = frames[i];
        auto pf = parent < 0 ? layout::frame{0,0,container->bounds.w,container->bounds.h} : frames[parent];
        if (is_kind(p, cls("NSBox")) && as<control_object>(p)->box_content == nodes[i]) {
            const double title = as<control_object>(p)->box_title_height;
            constraints.push_back({int(i),5,parent,5});
            constraints.push_back({int(i),3,parent,3,0,1,title});
            constraints.push_back({int(i),7,parent,7});
            constraints.push_back({int(i),8,parent,8,0,1,-title});
        } else if (as<view_object>(nodes[i])->translates_mask) {
            constraints.push_back({int(i),5,parent,5,0,1,f.x-pf.x});
            constraints.push_back({int(i),3,parent,3,0,1,f.y-pf.y});
            constraints.push_back({int(i),7,-1,0,0,1,f.w});
            constraints.push_back({int(i),8,-1,0,0,1,f.h});
        }
    }
    layout::solve(frames,container->bounds.w,container->bounds.h,constraints);
    double popup_height = 0;
    std::size_t popups = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (is_kind(nodes[i],cls("NSPopUpButton"))) {
            popup_height = popups ? std::min(popup_height,frames[i].h) : frames[i].h;
            ++popups;
        }
    if (popups) {
        using summary = std::array<double,5>;
        static thread_local std::unordered_map<id,summary> logged;
        const summary state{container->bounds.w,container->bounds.h,std::round(popup_height*100)/100,double(constraints.size()),double(unresolved)};
        if (logged.size() > 1024) logged.clear();
        if (!logged.contains(root) || logged[root] != state) {
            logged[root] = state;
            std::fprintf(stderr,"AppKit layout: solve root=%p bounds=%gx%g nodes=%zu constraints=%zu unresolved=%zu popups=%zu min-popup-height=%g\n",
                         static_cast<void *>(root),container->bounds.w,container->bounds.h,nodes.size(),constraints.size(),unresolved,popups,popup_height);
        }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const int parent = parents[i];
        id p = parent < 0 ? root : nodes[parent];
        auto *pv = as<view_object>(p);
        auto f = layout::align_edges(frames[i]);
        auto pf = parent < 0 ? layout::frame{0,0,container->bounds.w,container->bounds.h} : layout::align_edges(frames[parent]);
        const double x = f.x-pf.x, y = f.y-pf.y;
        rect next{x+pv->bounds.x, send(p,"isFlipped") ? y+pv->bounds.y : pv->bounds.h-y-f.h+pv->bounds.y, f.w,f.h};
        auto old = as<view_object>(nodes[i])->frame;
        if (std::abs(old.x-next.x)>.01 || std::abs(old.y-next.y)>.01 || std::abs(old.w-next.w)>.01 || std::abs(old.h-next.h)>.01)
            set_frame(nodes[i],next);
    }
    return true;
}
} // namespace
static void layout_cocoa_view_tree(id view, bool inherited) {
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
        wrapping_text_size(o,frames.back());
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
    const bool descendants = inherited || layout_descendants(view);
    if (!descendants) layout::solve(frames, v->bounds.w, v->bounds.h, constraints);
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (descendants) break;
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
    if (is_kind(view, cls("NSBox"))) {
        auto *box = as<control_object>(view);
        if (box->box_content)
            set_frame(box->box_content, {0,0,v->bounds.w,std::max(0.,v->bounds.h-box->box_title_height)});
    }
    if (is_kind(view, cls("NSScrollView"))) {
        tile_scroll(view);
    }
    const bool solved_children = descendants && !is_kind(view, cls("NSScrollView")) && !is_kind(view, cls("NSTableView"));
    for (id o : children)
        layout_cocoa_view_tree(o, solved_children);
}
void layout_cocoa_view(id view) { layout_cocoa_view_tree(view, false); }
bool dispatch_cocoa_popup_event(id event) {
    if (!event || !previous_choice_popup) return false;
    // Wayland compositors may retain keyboard focus on the parent surface.
    // An open choice menu owns keyboard input until selection or dismissal.
    id popup = retain(previous_choice_popup);
    const auto type = send(event,"type");
    const auto window = obj(send(event,"window"));
    bool handled = false;
    if (type == ev_key_down) {
        send(popup,"sendEvent:",ret(event));
        handled = true;
    } else if (window != popup &&
               (type == ev_left_mouse_down || type == ev_right_mouse_down || type == ev_other_mouse_down)) {
        send(popup,"close");
        handled = true;
    }
    release(popup);
    return handled;
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
    Class box = define("NSBox", control, MM_FW_CLASS_REF(NSBox), MM_FW_META_REF(NSBox));
    MM_METHODS(box, {"contentView",MM_IMP(box_content),"@16@0:8"},
        {"setContentView:",MM_IMP(set_box_content),"v24@0:8@16"},
        {"boxType",MM_IMP(get_box_type),"q16@0:8"}, {"setBoxType:",MM_IMP(set_box_type),"v24@0:8q16"},
        {"titlePosition",MM_IMP(get_title_position),"q16@0:8"}, {"setTitlePosition:",MM_IMP(set_title_position),"v24@0:8q16"});
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
        {"mouseMoved:", MM_IMP(choice_list_mouse), "v24@0:8@16"},
        {"mouseDragged:", MM_IMP(choice_list_mouse), "v24@0:8@16"},
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
        {"isFlipped", MM_IMP(clip_flipped), "B16@0:8"},
        {"scrollToPoint:", MM_IMP(scroll_to), "v32@0:8{CGPoint=dd}16"}, {"scrollWheel:", MM_IMP(wheel), "v24@0:8@16"});
    Class scroll = mm_objc_define_class("NSScrollView", view, sizeof(scroll_object), MM_FW_CLASS_REF(NSScrollView),
                                        MM_FW_META_REF(NSScrollView));
    register_lifecycle<scroll_object>(scroll);
    MM_METHODS(scroll, {"initWithFrame:", MM_IMP(scroll_init), "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
               {"contentView", MM_IMP(scroll_content), "@16@0:8"},
               {"setContentView:", MM_IMP(set_scroll_content), "v24@0:8@16"},
               {"documentView", MM_IMP(scroll_document), "@16@0:8"},
               {"setDocumentView:", MM_IMP(set_scroll_document), "v24@0:8@16"},
               {"verticalScroller",MM_IMP(get_vertical_scroller),"@16@0:8"}, {"horizontalScroller",MM_IMP(get_horizontal_scroller),"@16@0:8"},
               {"setVerticalScroller:",MM_IMP(set_vertical_scroller),"v24@0:8@16"}, {"setHorizontalScroller:",MM_IMP(set_horizontal_scroller),"v24@0:8@16"},
               {"hasVerticalScroller",MM_IMP(get_has_vertical),"B16@0:8"}, {"hasHorizontalScroller",MM_IMP(get_has_horizontal),"B16@0:8"},
               {"setHasVerticalScroller:",MM_IMP(set_has_vertical),"v24@0:8B16"}, {"setHasHorizontalScroller:",MM_IMP(set_has_horizontal),"v24@0:8B16"},
               {"autohidesScrollers",MM_IMP(get_autohides),"B16@0:8"}, {"setAutohidesScrollers:",MM_IMP(set_autohides),"v24@0:8B16"},
               {"borderType",MM_IMP(get_scroll_border),"q16@0:8"}, {"setBorderType:",MM_IMP(set_scroll_border),"v24@0:8q16"},
               {"tile",MM_IMP(scroll_tile),"v16@0:8"}, {"reflectScrolledClipView:",MM_IMP(scroll_reflect),"v24@0:8@16"},
               {"_machomanScroll:",MM_IMP(scroll_scroller),"v24@0:8@16"});
    MM_METHODS(cls("NSScroller"), {"mouseDown:",MM_IMP(scroller_mouse),"v24@0:8@16"},
               {"mouseDragged:",MM_IMP(scroller_mouse),"v24@0:8@16"}, {"isFlipped",MM_IMP(+[](id,SEL)->std::uint64_t{return 1;}),"B16@0:8"});
    Class table = mm_objc_define_class("NSTableView", control, sizeof(table_object), MM_FW_CLASS_REF(NSTableView),
                                       MM_FW_META_REF(NSTableView));
    register_lifecycle<table_object>(table);
    mm_objc_define_class("NSOutlineView", table, sizeof(table_object), MM_FW_CLASS_REF(NSOutlineView),
                         MM_FW_META_REF(NSOutlineView));
    MM_METHODS(
        table, {"isFlipped", MM_IMP(flipped), "B16@0:8"}, {"reloadData", MM_IMP(table_reload), "v16@0:8"},
        {"numberOfRows", MM_IMP(table_rows), "q16@0:8"}, {"selectedRow", MM_IMP(table_selected), "q16@0:8"},
        {"selectRowIndexes:byExtendingSelection:", MM_IMP(table_select_indexes), "v32@0:8@16B24"},
        {"selectedRowIndexes", MM_IMP(table_selected_indexes), "@16@0:8"},
        {"numberOfSelectedRows", MM_IMP(table_selected_count), "q16@0:8"},
        {"isRowSelected:", MM_IMP(table_row_selected), "B24@0:8q16"},
        {"deselectAll:", MM_IMP(table_deselect_all), "v24@0:8@16"},
        {"scrollRowToVisible:", MM_IMP(table_scroll_row), "v24@0:8q16"},
        {"allowsMultipleSelection", MM_IMP(table_multiple), "B16@0:8"},
        {"setAllowsMultipleSelection:", MM_IMP(table_set_multiple), "v24@0:8B16"},
        {"allowsEmptySelection", MM_IMP(table_empty), "B16@0:8"},
        {"columnAutoresizingStyle",MM_IMP(table_column_style),"Q16@0:8"},
        {"setColumnAutoresizingStyle:",MM_IMP(table_set_column_style),"v24@0:8Q16"},
        {"setAllowsEmptySelection:", MM_IMP(table_set_empty), "v24@0:8B16"},
        {"preparedCellAtColumn:row:", MM_IMP(table_prepared_cell), "@32@0:8q16q24"},
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
               {"dataCell", MM_IMP(column_cell), "@16@0:8"}, {"setDataCell:", MM_IMP(column_set_cell), "v24@0:8@16"},
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
