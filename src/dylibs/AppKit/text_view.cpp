/* Field editors used by guest views need real NSView/responder inheritance,
 * UTF-16 text and NSRange results even when the guest draws its own UI. */
#include "appkit.h"
#include <algorithm>

MM_FW_CLASS(NSTextView);

namespace ak {
namespace {
constexpr std::uint64_t not_found = 0x7fffffffffffffffULL;
struct text_view_object {
    view_object view;
    id text = nullptr, delegate = nullptr;
    std::uint64_t selection = 0, selection_length = 0;
    std::uint64_t marked = not_found, marked_length = 0;
    bool field_editor = false, editable = true, selectable = true;
    ~text_view_object() { release(text); }
};
text_view_object *tv(id self) { return as<text_view_object>(self); }
id contents(id self) {
    auto *o = tv(self);
    if (!o->text) o->text = obj(send(reinterpret_cast<id>(cls("NSMutableString")), "new"));
    return o->text;
}
id plain_text(id text) {
    return is_kind(text, cls("NSAttributedString")) ? obj(send(text, "string")) : text;
}
void text_notification(id self, const char *name, const char *selector) {
    id notification = obj(send(reinterpret_cast<id>(cls("NSNotification")),
        "notificationWithName:object:", ret(string(name)), ret(self)));
    id delegate = tv(self)->delegate;
    if (responds(delegate, selector)) send(delegate, selector, ret(notification));
    id center = obj(send(reinterpret_cast<id>(cls("NSNotificationCenter")), "defaultCenter"));
    send(center, "postNotification:", ret(notification));
}
void text_selection_changed(id self) {
    text_notification(self, "NSTextViewDidChangeSelectionNotification", "textViewDidChangeSelection:");
}
std::uint64_t text_string(id self, SEL) { return ret(contents(self)); }
std::uint64_t text_set_string(id self, SEL, id value) {
    send(contents(self), "setString:", ret(value ? plain_text(value) : string("")));
    auto *o = tv(self);
    const auto length = send(contents(self), "length");
    o->selection = std::min(o->selection, length);
    o->selection_length = std::min(o->selection_length, length - o->selection);
    o->marked = not_found; o->marked_length = 0;
    return 0;
}
std::uint64_t text_selected(id self, SEL) {
    mm_objc_return_words_hi(tv(self)->selection_length); return tv(self)->selection;
}
std::uint64_t text_set_selected(id self, SEL, std::uint64_t location, std::uint64_t length) {
    auto *o = tv(self); const auto count = send(contents(self), "length");
    o->selection = std::min(location, count);
    o->selection_length = std::min(length, count - o->selection);
    text_selection_changed(self);
    return 0;
}
std::uint64_t text_marked(id self, SEL) {
    mm_objc_return_words_hi(tv(self)->marked_length); return tv(self)->marked;
}
std::uint64_t text_unmark(id self, SEL) { tv(self)->marked = not_found; tv(self)->marked_length = 0; return 0; }
std::uint64_t text_replace(id self, id value, std::uint64_t location, std::uint64_t length) {
    if (!tv(self)->editable || !value) return 0;
    auto *o = tv(self); const auto count = send(contents(self), "length");
    if (location == not_found) {
        location = o->marked != not_found ? o->marked : o->selection;
        length = o->marked != not_found ? o->marked_length : o->selection_length;
    }
    location = std::min(location, count); length = std::min(length, count - location);
    id text = plain_text(value);
    const auto inserted = send(text, "length");
    send(contents(self), "replaceCharactersInRange:withString:", location, length, ret(text));
    o->selection = location + inserted; o->selection_length = 0;
    text_unmark(self, nullptr);
    return 0;
}
std::uint64_t text_insert(id self, SEL, id value, std::uint64_t location, std::uint64_t length) {
    if (!tv(self)->editable || !value) return 0;
    text_replace(self, value, location, length);
    /* Subclass delegates inspect the final string and marked/selected ranges
     * during this callback, before their keyDown: records its new baseline. */
    text_selection_changed(self);
    text_notification(self, "NSTextDidChangeNotification", "textDidChange:");
    return 0;
}
std::uint64_t text_insert_plain(id self, SEL, id text) { return text_insert(self, nullptr, text, not_found, 0); }
std::uint64_t text_replace_range(id self, SEL, std::uint64_t location, std::uint64_t length, id text) {
    return text_replace(self, text, location, length);
}
std::uint64_t text_set_marked(id self, SEL, id value, std::uint64_t selected, std::uint64_t selected_length,
                             std::uint64_t location, std::uint64_t length) {
    auto *o = tv(self);
    if (location == not_found) {
        location = o->marked != not_found ? o->marked : o->selection;
        length = o->marked != not_found ? o->marked_length : o->selection_length;
    }
    location = std::min(location, send(contents(self), "length"));
    if (!o->editable || !value) return 0;
    text_replace(self, value, location, length);
    o->marked = location; o->marked_length = send(plain_text(value), "length");
    selected = std::min(selected, o->marked_length);
    o->selection = location + selected;
    o->selection_length = std::min(selected_length, o->marked_length - selected);
    text_selection_changed(self);
    return 0;
}
std::uint64_t text_key_down(id self, SEL, id event) {
    if (!tv(self)->selectable) return 0;
    autorelease_pool pool;
    id events = obj(send(reinterpret_cast<id>(cls("NSArray")), "arrayWithObject:", ret(event)));
    return send(self, "interpretKeyEvents:", ret(events));
}
std::uint64_t text_copy(id self, SEL, id) {
    auto *o = tv(self);
    if (!o->selectable || !o->selection_length) return 0;
    id value = obj(send(contents(self), "substringWithRange:", o->selection, o->selection_length));
    id pasteboard = obj(send(reinterpret_cast<id>(cls("NSPasteboard")), "generalPasteboard"));
    send(pasteboard, "clearContents");
    return send(pasteboard, "setString:forType:", ret(value), ret(string("public.utf8-plain-text")));
}
std::uint64_t text_cut(id self, SEL, id sender) {
    if (!tv(self)->editable || !text_copy(self, nullptr, sender)) return 0;
    return send(self, "insertText:replacementRange:", ret(string("")), tv(self)->selection, tv(self)->selection_length);
}
std::uint64_t text_paste(id self, SEL, id) {
    if (!tv(self)->editable) return 0;
    id pasteboard = obj(send(reinterpret_cast<id>(cls("NSPasteboard")), "generalPasteboard"));
    id value = obj(send(pasteboard, "stringForType:", ret(string("public.utf8-plain-text"))));
    if (value) send(self, "insertText:replacementRange:", ret(value), not_found, 0);
    return 0;
}
std::uint64_t text_select_all(id self, SEL, id) {
    if (tv(self)->selectable) text_set_selected(self, nullptr, 0, send(contents(self), "length"));
    return 0;
}
std::uint64_t text_delete(id self, bool forward) {
    auto *o = tv(self); auto location = o->selection, length = o->selection_length;
    const auto count = send(contents(self), "length");
    if (!length) {
        if (forward) { if (location >= count) return 0; }
        else { if (!location) return 0; --location; }
        length = 1;
        const auto unit = send(contents(self), "characterAtIndex:", location);
        if (forward && unit >= 0xd800 && unit <= 0xdbff && location + 1 < count) {
            const auto low = send(contents(self), "characterAtIndex:", location + 1);
            if (low >= 0xdc00 && low <= 0xdfff) length = 2;
        } else if (!forward && unit >= 0xdc00 && unit <= 0xdfff && location) {
            const auto high = send(contents(self), "characterAtIndex:", location - 1);
            if (high >= 0xd800 && high <= 0xdbff) { --location; length = 2; }
        }
    }
    return text_insert(self, nullptr, string(""), location, length);
}
std::uint64_t text_mouse_down(id self, SEL, id) {
    if (tv(self)->view.window) send(tv(self)->view.window, "makeFirstResponder:", ret(self));
    return 0;
}
}

void register_text_view() {
    Class text = mm_objc_define_class("NSTextView", cls("NSView"), sizeof(text_view_object),
        MM_FW_CLASS_REF(NSTextView), MM_FW_META_REF(NSTextView));
    register_lifecycle<text_view_object>(text);
    MM_METHODS(text,
        {"string", MM_IMP(text_string), "@16@0:8"}, {"setString:", MM_IMP(text_set_string), "v24@0:8@16"},
        {"selectedRange", MM_IMP(text_selected), "{_NSRange=QQ}16@0:8"},
        {"setSelectedRange:", MM_IMP(text_set_selected), "v32@0:8{_NSRange=QQ}16"},
        {"markedRange", MM_IMP(text_marked), "{_NSRange=QQ}16@0:8"},
        {"hasMarkedText", MM_IMP(+[](id s, SEL) -> std::uint64_t { return tv(s)->marked != not_found; }), "B16@0:8"},
        {"unmarkText", MM_IMP(text_unmark), "v16@0:8"},
        {"insertText:", MM_IMP(text_insert_plain), "v24@0:8@16"},
        {"insertText:replacementRange:", MM_IMP(text_insert), "v40@0:8@16{_NSRange=QQ}24"},
        {"replaceCharactersInRange:withString:", MM_IMP(text_replace_range), "v40@0:8{_NSRange=QQ}16@32"},
        {"setMarkedText:selectedRange:replacementRange:", MM_IMP(text_set_marked), "v56@0:8@16{_NSRange=QQ}24{_NSRange=QQ}40"},
        {"keyDown:", MM_IMP(text_key_down), "v24@0:8@16"},
        {"copy:", MM_IMP(text_copy), "v24@0:8@16"}, {"cut:", MM_IMP(text_cut), "v24@0:8@16"},
        {"paste:", MM_IMP(text_paste), "v24@0:8@16"}, {"pasteAsPlainText:", MM_IMP(text_paste), "v24@0:8@16"},
        {"selectAll:", MM_IMP(text_select_all), "v24@0:8@16"},
        {"mouseDown:", MM_IMP(text_mouse_down), "v24@0:8@16"},
        {"deleteBackward:", MM_IMP(+[](id s, SEL, id) { return text_delete(s, false); }), "v24@0:8@16"},
        {"deleteForward:", MM_IMP(+[](id s, SEL, id) { return text_delete(s, true); }), "v24@0:8@16"},
        {"delegate", MM_IMP(+[](id s, SEL) { return ret(tv(s)->delegate); }), "@16@0:8"},
        {"setDelegate:", MM_IMP(+[](id s, SEL, id d) -> std::uint64_t { tv(s)->delegate = d; return 0; }), "v24@0:8@16"},
        {"isFieldEditor", MM_IMP(+[](id s, SEL) -> std::uint64_t { return tv(s)->field_editor; }), "B16@0:8"},
        {"setFieldEditor:", MM_IMP(+[](id s, SEL, std::uint64_t b) -> std::uint64_t { tv(s)->field_editor = (b & 0xff) != 0; return 0; }), "v24@0:8B16"},
        {"isEditable", MM_IMP(+[](id s, SEL) -> std::uint64_t { return tv(s)->editable; }), "B16@0:8"},
        {"setEditable:", MM_IMP(+[](id s, SEL, std::uint64_t b) -> std::uint64_t { tv(s)->editable = (b & 0xff) != 0; return 0; }), "v24@0:8B16"},
        {"isSelectable", MM_IMP(+[](id s, SEL) -> std::uint64_t { return tv(s)->selectable; }), "B16@0:8"},
        {"setSelectable:", MM_IMP(+[](id s, SEL, std::uint64_t b) -> std::uint64_t { tv(s)->selectable = (b & 0xff) != 0; return 0; }), "v24@0:8B16"});
}
}
