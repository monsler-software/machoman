/* Cocoa pasteboards own typed byte payloads. Only the general pasteboard is
 * connected to the host selection; named pasteboards remain process-local. */
#include "appkit.h"
#include <algorithm>
#include <mutex>
#include <optional>

MM_FW_CLASS(NSPasteboard);
MM_FW_CLASS(NSPasteboardItem);

#define PB_CONSTANT(name, value) extern "C" { MM_FW_EXPORT mm_id name##_ __asm__("_" #name) = nullptr; }
PB_CONSTANT(NSPasteboardNameGeneral, "NSGeneralPboard")
PB_CONSTANT(NSPasteboardNameFind, "NSFindPboard")
PB_CONSTANT(NSPasteboardNameDrag, "NSDragPboard")
PB_CONSTANT(NSPasteboardNameFont, "NSFontPboard")
PB_CONSTANT(NSPasteboardNameRuler, "NSRulerPboard")
PB_CONSTANT(NSPasteboardTypeString, "public.utf8-plain-text")
PB_CONSTANT(NSPasteboardTypeHTML, "public.html")
PB_CONSTANT(NSPasteboardTypeRTF, "public.rtf")
PB_CONSTANT(NSPasteboardTypeRTFD, "com.apple.flat-rtfd")
PB_CONSTANT(NSPasteboardTypeTIFF, "public.tiff")
PB_CONSTANT(NSPasteboardTypePNG, "public.png")
PB_CONSTANT(NSPasteboardTypePDF, "com.adobe.pdf")
PB_CONSTANT(NSPasteboardTypeURL, "public.url")
PB_CONSTANT(NSPasteboardTypeFileURL, "public.file-url")
PB_CONSTANT(NSPasteboardTypeTabularText, "public.utf8-tab-separated-values")
PB_CONSTANT(NSStringPboardType, "NSStringPboardType")
PB_CONSTANT(NSHTMLPboardType, "NSHTMLPboardType")
PB_CONSTANT(NSRTFPboardType, "NSRTFPboardType")
PB_CONSTANT(NSRTFDPboardType, "NSRTFDPboardType")
PB_CONSTANT(NSTIFFPboardType, "NSTIFFPboardType")
PB_CONSTANT(NSPDFPboardType, "NSPDFPboardType")
PB_CONSTANT(NSURLPboardType, "NSURLPboardType")
PB_CONSTANT(NSFilenamesPboardType, "NSFilenamesPboardType")
#undef PB_CONSTANT

namespace ak {
namespace {
std::recursive_mutex pasteboard_mu;
Class board_class = nullptr, item_class = nullptr;
std::map<std::string, id> boards;
std::uint64_t unique_name = 0;

std::string canonical(std::string type) {
    static const std::map<std::string, std::string> legacy = {
        {"NSStringPboardType", "public.utf8-plain-text"}, {"NSHTMLPboardType", "public.html"},
        {"NSRTFPboardType", "public.rtf"}, {"NSRTFDPboardType", "com.apple.flat-rtfd"},
        {"NSTIFFPboardType", "public.tiff"}, {"NSPDFPboardType", "com.adobe.pdf"}, {"NSURLPboardType", "public.url"}
    };
    auto it = legacy.find(type); return it == legacy.end() ? type : it->second;
}
std::string mime_type(const std::string &type) {
    static const std::map<std::string, std::string> mimes = {
        {"public.utf8-plain-text", "text/plain;charset=utf-8"}, {"public.html", "text/html"},
        {"public.rtf", "text/rtf"}, {"public.png", "image/png"}, {"public.tiff", "image/tiff"},
        {"com.adobe.pdf", "application/pdf"}, {"public.file-url", "text/uri-list"},
        {"public.utf8-tab-separated-values", "text/tab-separated-values"}
    };
    auto it = mimes.find(type); return it == mimes.end() ? type : it->second;
}
std::string cocoa_type(const std::string &mime) {
    if (mime == "text/plain" || mime == "UTF8_STRING" || mime == "TEXT" ||
        mime == "text/plain;charset=utf-8" || mime == "text/plain;charset=UTF-8") return "public.utf8-plain-text";
    for (const auto *type : {"public.html", "public.rtf", "public.png", "public.tiff", "com.adobe.pdf",
                              "public.file-url", "public.utf8-tab-separated-values"})
        if (mime_type(type) == mime) return type;
    // Mozilla's URL format is UTF-16 with a title; do not treat it as UTF-8.
    return mime;
}
id array(const std::vector<std::string> &values) {
    id a = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (const auto &value : values) send(a, "addObject:", ret(string(value)));
    return a;
}
id nsdata(const std::string &bytes) {
    return obj(send(reinterpret_cast<id>(cls("NSData")), "dataWithBytes:length:", ret(bytes.data()), bytes.size()));
}
std::string bytes_of(id data) {
    const auto size = send(data, "length");
    const auto *bytes = reinterpret_cast<const char *>(send(data, "bytes"));
    return bytes && size ? std::string(bytes, static_cast<std::size_t>(size)) : std::string{};
}
id utf8_string(const std::string &bytes) {
    id result = obj(send(reinterpret_cast<id>(cls("NSString")), "alloc"));
    return autorelease(obj(send(result, "initWithBytes:length:encoding:", ret(bytes.data()), bytes.size(), 4)));
}
std::optional<std::string> encode_property(id value, const std::string &type) {
    if (!value) return {};
    if (is_kind(value, cls("NSString")) && (type == "public.utf8-plain-text" || type == "public.html" ||
        type == "public.url" || type == "public.file-url" || type == "public.utf8-tab-separated-values")) {
        id data = obj(send(value, "dataUsingEncoding:", 4));
        if (data) return bytes_of(data);
    }
    if (is_kind(value, cls("NSData"))) return bytes_of(value);
    if (type == "public.file-url" && is_kind(value, cls("NSURL"))) return to_utf8(obj(send(value, "absoluteString"))) + "\r\n";
    const std::uint64_t args[] = {ret(value), 200, 0, 0}; // Binary property list.
    id data = obj(mm_objc_send(reinterpret_cast<id>(cls("NSPropertyListSerialization")),
        mm_objc_sel("dataWithPropertyList:format:options:error:"), 4, args));
    if (!data) return {};
    return bytes_of(data);
}
id decode_property(const std::string &bytes, const std::string &type) {
    if (type == "public.utf8-plain-text" || type == "public.html" || type == "public.url" || type == "public.file-url") return utf8_string(bytes);
    const std::uint64_t args[] = {ret(nsdata(bytes)), 0, 0, 0};
    return obj(mm_objc_send(reinterpret_cast<id>(cls("NSPropertyListSerialization")),
        mm_objc_sel("propertyListWithData:options:format:error:"), 4, args));
}
struct item_object {
    mm_objc_object base;
    std::vector<std::string> types;
    std::map<std::string, std::string> bytes;
};
struct board_object {
    mm_objc_object base;
    std::string name;
    std::vector<id> items;
    id owner = nullptr;
    std::uint64_t changes = 0, host_generation = 0;
    std::map<std::string, std::string> host_types;
    bool general = false;
    void clear() { for (id item : items) release(item); items.clear(); host_types.clear(); }
    ~board_object() { clear(); release(owner); }
};
item_object *item(id self) { return as<item_object>(self); }
board_object *board(id self) { return as<board_object>(self); }
void add_type(item_object *o, const std::string &type) {
    if (std::find(o->types.begin(), o->types.end(), type) == o->types.end()) o->types.push_back(type);
}
id new_item() { return reinterpret_cast<id>(make<item_object>(item_class)); }
void sync(id self) {
    auto *b = board(self);
    if (!b->general) return;
    const auto host = platform::clipboard_types();
    if (b->host_generation == host.generation) return;
    b->clear(); b->host_generation = host.generation; ++b->changes;
    if (!host.types.empty()) {
        id value = new_item(); auto *o = item(value);
        for (const auto &mime : host.types) {
            const auto type = cocoa_type(mime); add_type(o, type);
            // Prefer the explicit UTF-8 representation over text/plain.
            if (!b->host_types.contains(type) || mime == mime_type(type)) b->host_types[type] = mime;
        }
        b->items.push_back(value);
    }
    id previous = b->owner; b->owner = nullptr;
    if (responds(previous, "pasteboardChangedOwner:")) send(previous, "pasteboardChangedOwner:", ret(self));
    release(previous);
}
void publish(id self) {
    auto *b = board(self); if (!b->general) return;
    clipboard_data data;
    for (id value : b->items) for (const auto &[type, bytes] : item(value)->bytes) {
        data.try_emplace(type, bytes);
        if (type != "public.url") data.try_emplace(mime_type(type), bytes);
        if (type == "public.utf8-plain-text") { data.try_emplace("text/plain", bytes); data.try_emplace("UTF8_STRING", bytes); }
    }
    platform::clipboard_write(data);
    b->host_generation = platform::clipboard_types().generation;
}
std::optional<std::string> get_bytes(id self, const std::string &type, id pasteboard = nullptr) {
    struct keep_alive { id value; ~keep_alive() { release(value); } } guard{retain(self)};
    auto *o = item(self);
    if (auto it = o->bytes.find(type); it != o->bytes.end()) return it->second;
    if (pasteboard) {
        auto *b = board(pasteboard);
        if (responds(b->owner, "pasteboard:provideDataForType:"))
            send(b->owner, "pasteboard:provideDataForType:", ret(pasteboard), ret(string(type)));
        if (auto it = o->bytes.find(type); it != o->bytes.end()) return it->second;
        auto it = b->host_types.find(type);
        if (it != b->host_types.end()) {
            std::string result;
            if (platform::clipboard_read(it->second, result)) { o->bytes[type] = result; return result; }
        }
    }
    return {};
}
std::uint64_t item_types(id self, SEL) { std::lock_guard lock(pasteboard_mu); return ret(array(item(self)->types)); }
std::uint64_t item_set_data(id self, SEL, id data, id type) {
    std::lock_guard lock(pasteboard_mu); if (!data || !type) return 0;
    const auto key = canonical(to_utf8(type)); if (key.empty()) return 0;
    add_type(item(self), key); item(self)->bytes[key] = bytes_of(data); return 1;
}
std::uint64_t item_set_string(id self, SEL, id text, id type) {
    if (!text) return 0;
    id data = obj(send(text, "dataUsingEncoding:", 4));
    return data ? item_set_data(self, nullptr, data, type) : 0;
}
std::uint64_t item_set_property(id self, SEL, id value, id type) {
    std::lock_guard lock(pasteboard_mu); if (!type) return 0;
    const auto key = canonical(to_utf8(type)); auto bytes = encode_property(value, key);
    if (!bytes || key.empty()) return 0;
    add_type(item(self), key); item(self)->bytes[key] = std::move(*bytes); return 1;
}
std::uint64_t item_get_data(id self, SEL selector, id type) {
    std::lock_guard lock(pasteboard_mu); if (!type) return 0;
    const auto key = canonical(to_utf8(type)); auto bytes = get_bytes(self, key);
    if (!bytes) return 0;
    if (std::strcmp(selector, "stringForType:") == 0) return ret(utf8_string(*bytes));
    if (std::strcmp(selector, "pasteboardPropertyListForType:") == 0 && key != "public.utf8-plain-text") return ret(nsdata(*bytes));
    if (std::strcmp(selector, "propertyListForType:") == 0 || std::strcmp(selector, "pasteboardPropertyListForType:") == 0)
        return ret(decode_property(*bytes, key));
    return ret(nsdata(*bytes));
}
std::uint64_t available(id self, SEL, id types) {
    std::lock_guard lock(pasteboard_mu);
    const auto &supported = item(self)->types;
    for (std::uint64_t i = 0; i < send(types, "count"); ++i) {
        id value = obj(send(types, "objectAtIndex:", i));
        if (std::find(supported.begin(), supported.end(), canonical(to_utf8(value))) != supported.end()) return ret(value);
    }
    return 0;
}
std::uint64_t named(id, SEL, id name) {
    std::lock_guard lock(pasteboard_mu); if (!name) return 0;
    const auto key = to_utf8(name); auto &value = boards[key];
    if (!value) { value = reinterpret_cast<id>(make<board_object>(board_class)); board(value)->name = key; board(value)->general = key == "NSGeneralPboard"; }
    return ret(value);
}
std::uint64_t general(id c, SEL) { return named(c, nullptr, string("NSGeneralPboard")); }
std::uint64_t unique(id c, SEL) {
    std::lock_guard lock(pasteboard_mu); return named(c, nullptr, string("machoman.pasteboard." + std::to_string(++unique_name)));
}
std::uint64_t name(id self, SEL) { return ret(string(board(self)->name)); }
std::uint64_t change_count(id self, SEL) { std::lock_guard lock(pasteboard_mu); sync(self); return board(self)->changes; }
std::uint64_t clear(id self, SEL) {
    std::lock_guard lock(pasteboard_mu); sync(self);
    auto *b = board(self); b->clear(); ++b->changes;
    id previous = b->owner; b->owner = nullptr;
    if (responds(previous, "pasteboardChangedOwner:")) send(previous, "pasteboardChangedOwner:", ret(self));
    release(previous); publish(self); return b->changes;
}
std::uint64_t add_types(id self, SEL, id types, id owner) {
    std::lock_guard lock(pasteboard_mu); sync(self); auto *b = board(self);
    if (b->items.empty()) b->items.push_back(new_item());
    for (std::uint64_t i = 0; i < send(types, "count"); ++i) {
        const auto key = canonical(to_utf8(obj(send(types, "objectAtIndex:", i))));
        if (!key.empty()) add_type(item(b->items.front()), key);
    }
    id previous = b->owner; b->owner = retain(owner); release(previous); return b->changes;
}
std::uint64_t declare_types(id self, SEL, id types, id owner) {
    std::lock_guard lock(pasteboard_mu); clear(self, nullptr); return add_types(self, nullptr, types, owner);
}
std::uint64_t board_types(id self, SEL) {
    std::lock_guard lock(pasteboard_mu); sync(self); std::vector<std::string> types;
    for (id value : board(self)->items) for (const auto &type : item(value)->types)
        if (std::find(types.begin(), types.end(), type) == types.end()) types.push_back(type);
    return ret(array(types));
}
std::uint64_t board_available(id self, SEL, id types) {
    std::lock_guard lock(pasteboard_mu); sync(self);
    for (std::uint64_t i = 0; i < send(types, "count"); ++i) {
        id type = obj(send(types, "objectAtIndex:", i));
        for (id value : board(self)->items)
            if (std::find(item(value)->types.begin(), item(value)->types.end(), canonical(to_utf8(type))) != item(value)->types.end()) return ret(type);
    }
    return 0;
}
std::uint64_t board_set(id self, SEL selector, id value, id type) {
    std::lock_guard lock(pasteboard_mu); sync(self); if (!type || !value) return 0;
    auto *b = board(self); if (b->items.empty()) b->items.push_back(new_item());
    std::uint64_t success;
    if (std::strcmp(selector, "setString:forType:") == 0) success = item_set_string(b->items.front(), nullptr, value, type);
    else if (std::strcmp(selector, "setPropertyList:forType:") == 0) success = item_set_property(b->items.front(), nullptr, value, type);
    else success = item_set_data(b->items.front(), nullptr, value, type);
    if (success) publish(self);
    return success;
}
std::uint64_t board_get(id self, SEL selector, id type) {
    std::lock_guard lock(pasteboard_mu); sync(self); if (!type) return 0;
    const auto key = canonical(to_utf8(type));
    for (id value : board(self)->items) {
        auto bytes = get_bytes(value, key, self); if (!bytes) continue;
        if (std::strcmp(selector, "stringForType:") == 0) return ret(utf8_string(*bytes));
        if (std::strcmp(selector, "propertyListForType:") == 0) return ret(decode_property(*bytes, key));
        return ret(nsdata(*bytes));
    }
    return 0;
}
std::uint64_t board_items(id self, SEL) {
    std::lock_guard lock(pasteboard_mu); sync(self);
    id a = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id value : board(self)->items) {
        // Materialize host bytes before returning standalone items.
        for (const auto &type : item(value)->types) get_bytes(value, type, self);
        send(a, "addObject:", ret(value));
    }
    return ret(a);
}
std::uint64_t write_objects(id self, SEL, id objects) {
    std::lock_guard lock(pasteboard_mu); sync(self);
    std::vector<id> result;
    for (std::uint64_t i = 0; i < send(objects, "count"); ++i) {
        id writer = obj(send(objects, "objectAtIndex:", i)), value = new_item();
        auto *o = item(value);
        if (is_kind(writer, item_class)) { o->types = item(writer)->types; o->bytes = item(writer)->bytes; }
        else if (is_kind(writer, cls("NSString"))) {
            item_set_string(value, nullptr, writer, string("public.utf8-plain-text"));
        } else if (is_kind(writer, cls("NSURL"))) {
            const auto url = to_utf8(obj(send(writer, "absoluteString")));
            const std::string type = send(writer, "isFileURL") ? "public.file-url" : "public.url";
            add_type(o, type); o->bytes[type] = url + (type == "public.file-url" ? "\r\n" : "");
        } else if (responds(writer, "writableTypesForPasteboard:") && responds(writer, "pasteboardPropertyListForType:")) {
            id types = obj(send(writer, "writableTypesForPasteboard:", ret(self)));
            for (std::uint64_t j = 0; j < send(types, "count"); ++j) {
                id type = obj(send(types, "objectAtIndex:", j));
                item_set_property(value, nullptr, obj(send(writer, "pasteboardPropertyListForType:", ret(type))), type);
            }
        }
        if (o->bytes.empty()) {
            release(value); for (id previous : result) release(previous); return 0;
        }
        result.push_back(value);
    }
    auto *b = board(self); b->host_types.clear();
    b->items.insert(b->items.end(), result.begin(), result.end()); publish(self); return 1;
}
std::uint64_t read_objects(id self, SEL, id classes, id) {
    std::lock_guard lock(pasteboard_mu); sync(self);
    id a = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id value : board(self)->items) for (std::uint64_t j = 0; j < send(classes, "count"); ++j) {
        Class c = reinterpret_cast<Class>(send(classes, "objectAtIndex:", j)); id object = nullptr;
        if (c == item_class) {
            for (const auto &type : item(value)->types) get_bytes(value, type, self);
            object = value;
        } else if (c == cls("NSString")) {
            if (auto bytes = get_bytes(value, "public.utf8-plain-text", self)) object = utf8_string(*bytes);
        } else if (c == cls("NSURL")) {
            for (const auto *type : {"public.file-url", "public.url"}) if (auto bytes = get_bytes(value, type, self)) {
                auto end = bytes->find_first_of("\r\n");
                object = obj(send(reinterpret_cast<id>(c), "URLWithString:", ret(string(bytes->substr(0, end))))); break;
            }
        }
        if (object) { send(a, "addObject:", ret(object)); break; }
    }
    return ret(a);
}
}

void register_pasteboard() {
#define PB_CONSTANT(name, value) name##_ = retain(string(value));
    PB_CONSTANT(NSPasteboardNameGeneral, "NSGeneralPboard") PB_CONSTANT(NSPasteboardNameFind, "NSFindPboard")
    PB_CONSTANT(NSPasteboardNameDrag, "NSDragPboard") PB_CONSTANT(NSPasteboardNameFont, "NSFontPboard") PB_CONSTANT(NSPasteboardNameRuler, "NSRulerPboard")
    PB_CONSTANT(NSPasteboardTypeString, "public.utf8-plain-text") PB_CONSTANT(NSPasteboardTypeHTML, "public.html")
    PB_CONSTANT(NSPasteboardTypeRTF, "public.rtf") PB_CONSTANT(NSPasteboardTypeRTFD, "com.apple.flat-rtfd")
    PB_CONSTANT(NSPasteboardTypeTIFF, "public.tiff") PB_CONSTANT(NSPasteboardTypePNG, "public.png")
    PB_CONSTANT(NSPasteboardTypePDF, "com.adobe.pdf") PB_CONSTANT(NSPasteboardTypeURL, "public.url")
    PB_CONSTANT(NSPasteboardTypeFileURL, "public.file-url") PB_CONSTANT(NSPasteboardTypeTabularText, "public.utf8-tab-separated-values")
    PB_CONSTANT(NSStringPboardType, "NSStringPboardType") PB_CONSTANT(NSHTMLPboardType, "NSHTMLPboardType")
    PB_CONSTANT(NSRTFPboardType, "NSRTFPboardType") PB_CONSTANT(NSRTFDPboardType, "NSRTFDPboardType")
    PB_CONSTANT(NSTIFFPboardType, "NSTIFFPboardType") PB_CONSTANT(NSPDFPboardType, "NSPDFPboardType")
    PB_CONSTANT(NSURLPboardType, "NSURLPboardType") PB_CONSTANT(NSFilenamesPboardType, "NSFilenamesPboardType")
#undef PB_CONSTANT
    item_class = mm_objc_define_class("NSPasteboardItem", cls("NSObject"), sizeof(item_object), MM_FW_CLASS_REF(NSPasteboardItem), MM_FW_META_REF(NSPasteboardItem));
    register_lifecycle<item_object>(item_class);
    MM_METHODS(item_class,
        {"types", MM_IMP(item_types), "@16@0:8"},
        {"writableTypesForPasteboard:", MM_IMP(item_types), "@24@0:8@16"},
        {"availableTypeFromArray:", MM_IMP(available), "@24@0:8@16"},
        {"setString:forType:", MM_IMP(item_set_string), "B32@0:8@16@24"},
        {"setData:forType:", MM_IMP(item_set_data), "B32@0:8@16@24"},
        {"setPropertyList:forType:", MM_IMP(item_set_property), "B32@0:8@16@24"},
        {"stringForType:", MM_IMP(item_get_data), "@24@0:8@16"},
        {"dataForType:", MM_IMP(item_get_data), "@24@0:8@16"},
        {"propertyListForType:", MM_IMP(item_get_data), "@24@0:8@16"},
        {"pasteboardPropertyListForType:", MM_IMP(item_get_data), "@24@0:8@16"});
    board_class = mm_objc_define_class("NSPasteboard", cls("NSObject"), sizeof(board_object), MM_FW_CLASS_REF(NSPasteboard), MM_FW_META_REF(NSPasteboard));
    register_lifecycle<board_object>(board_class);
    MM_CLASS_METHODS(board_class,
        {"generalPasteboard", MM_IMP(general), "@16@0:8"},
        {"pasteboardWithName:", MM_IMP(named), "@24@0:8@16"},
        {"pasteboardWithUniqueName", MM_IMP(unique), "@16@0:8"});
    MM_METHODS(board_class,
        {"name", MM_IMP(name), "@16@0:8"}, {"changeCount", MM_IMP(change_count), "q16@0:8"},
        {"clearContents", MM_IMP(clear), "q16@0:8"}, {"prepareForNewContentsWithOptions:", MM_IMP(clear), "q24@0:8Q16"},
        {"declareTypes:owner:", MM_IMP(declare_types), "q32@0:8@16@24"},
        {"addTypes:owner:", MM_IMP(add_types), "q32@0:8@16@24"},
        {"types", MM_IMP(board_types), "@16@0:8"}, {"availableTypeFromArray:", MM_IMP(board_available), "@24@0:8@16"},
        {"setString:forType:", MM_IMP(board_set), "B32@0:8@16@24"},
        {"setData:forType:", MM_IMP(board_set), "B32@0:8@16@24"},
        {"setPropertyList:forType:", MM_IMP(board_set), "B32@0:8@16@24"},
        {"stringForType:", MM_IMP(board_get), "@24@0:8@16"},
        {"dataForType:", MM_IMP(board_get), "@24@0:8@16"},
        {"propertyListForType:", MM_IMP(board_get), "@24@0:8@16"},
        {"pasteboardItems", MM_IMP(board_items), "@16@0:8"},
        {"writeObjects:", MM_IMP(write_objects), "B24@0:8@16"},
        {"readObjectsForClasses:options:", MM_IMP(read_objects), "@32@0:8@16@24"},
        {"canReadObjectForClasses:options:", MM_IMP(+[](id s, SEL, id c, id options) -> std::uint64_t { return send(obj(read_objects(s, nullptr, c, options)), "count") != 0; }), "B32@0:8@16@24"});
}
}
