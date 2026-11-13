/* NSJSONSerialization bridges JSON values to real Cocoa objects. In
 * particular a failed parse must supply NSError rather than silently
 * dropping the parameters of a native message-bus command. */
#include "foundation.h"
#include "machoman/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace fnd {
FND_CLASS(NSJSONSerialization);

namespace {
using namespace std::literals;
constexpr std::uint64_t mutable_containers = 1, mutable_leaves = 2, read_fragments = 4;
constexpr std::uint64_t pretty = 1, sorted_keys = 2, write_fragments = 4, unescaped_slashes = 8;

/* Own temporary objects while recursively building a container. */
struct owned {
    id value;
    explicit owned(id o) : value(o) {}
    ~owned() { release(value); }
    owned(const owned &) = delete;
    owned &operator=(const owned &) = delete;
};

std::uint64_t failure(id *error, std::string_view message) {
    if (error) {
        owned info(dictionary_create(false)), text(string_from_utf8(message));
        dictionary_set(info.value, constant_string("NSLocalizedDescription"), text.value);
        dictionary_set(info.value, constant_string("NSDebugDescription"), text.value);
        *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 3840, info.value));
    }
    return 0;
}

id cocoa_value(const mm_json &json, std::uint64_t options, std::string &error) {
    if (std::holds_alternative<std::nullptr_t>(json.value)) return retain(null_object());
    if (auto *v = std::get_if<bool>(&json.value)) return retain(number_bool(*v));
    if (auto *v = std::get_if<std::string>(&json.value)) {
        id s = string_from_utf8(*v);
        if (options & mutable_leaves) { id m = obj(send(s, "mutableCopy")); release(s); return m; }
        return s;
    }
    if (auto *v = std::get_if<mm_json_number>(&json.value)) {
        const auto &s = v->text;
        if (s.find_first_of(".eE") == s.npos) {
            std::int64_t integer;
            auto result = std::from_chars(s.data(), s.data() + s.size(), integer);
            if (result.ec == std::errc{}) return number_from_int64(integer);
            std::uint64_t unsigned_integer;
            result = std::from_chars(s.data(), s.data() + s.size(), unsigned_integer);
            if (result.ec == std::errc{}) {
                id n = obj(send(reinterpret_cast<id>(cls("NSNumber")), "alloc"));
                return obj(send(n, "initWithUnsignedLongLong:", unsigned_integer));
            }
        }
        double number;
        auto result = std::from_chars(s.data(), s.data() + s.size(), number);
        if (result.ec == std::errc{} && std::isfinite(number)) return number_from_double(number);
        error = "JSON number is outside the supported finite range"; return nullptr;
    }
    if (auto *v = std::get_if<mm_json::object>(&json.value)) {
        id d = dictionary_create((options & mutable_containers) != 0);
        for (const auto &[key, entry] : *v) {
            owned k(string_from_utf8(key)), value(cocoa_value(entry, options, error));
            if (!value.value) { release(d); return nullptr; }
            dictionary_set(d, k.value, value.value);
        }
        return d;
    }
    id a = array_create(nullptr, 0, (options & mutable_containers) != 0);
    for (const auto &entry : std::get<mm_json::array>(json.value)) {
        id value = cocoa_value(entry, options, error);
        if (!value) { release(a); return nullptr; }
        array_storage(a).push_back(value); // transfer the retained element
    }
    return a;
}

/* Detect the five JSON Unicode encodings, including BOM-less UTF-16/32.
 * Validate code points here because NSString decoding is deliberately
 * permissive, whereas JSON must reject invalid Unicode. */
bool json_utf8(std::string_view input, std::string &out) {
    auto byte = [&](std::size_t i) { return static_cast<unsigned char>(input[i]); };
    unsigned width = 1; bool be = false; std::size_t start = 0;
    if (input.starts_with("\x00\x00\xfe\xff"sv)) { width = 4; be = true; start = 4; }
    else if (input.starts_with("\xff\xfe\x00\x00"sv)) { width = 4; start = 4; }
    else if (input.starts_with("\xfe\xff"sv)) { width = 2; be = true; start = 2; }
    else if (input.starts_with("\xff\xfe"sv)) { width = 2; start = 2; }
    else if (input.starts_with("\xef\xbb\xbf"sv)) start = 3;
    else if (input.size() >= 4) {
        if (!byte(0) && !byte(1) && !byte(2)) { width = 4; be = true; }
        else if (!byte(1) && !byte(2) && !byte(3)) width = 4;
        else if (!byte(0) && !byte(2)) { width = 2; be = true; }
        else if (!byte(1) && !byte(3)) width = 2;
    }
    if (width == 1) { out.assign(input.substr(start)); return true; }
    if ((input.size() - start) % width) return false;
    std::u16string text;
    auto unit = [&](std::size_t i) {
        std::uint32_t cp = 0;
        for (unsigned j = 0; j < width; ++j) cp |= static_cast<std::uint32_t>(byte(i + j)) << ((be ? width - 1 - j : j) * 8);
        return cp;
    };
    for (std::size_t i = start; i < input.size(); i += width) {
        auto cp = unit(i);
        if (width == 2 && cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 2 >= input.size()) return false;
            const auto low = unit(i + 2);
            if (low < 0xdc00 || low > 0xdfff) return false;
            text.push_back(static_cast<char16_t>(cp)); text.push_back(static_cast<char16_t>(low)); i += 2; continue;
        }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp >= 0x10000) { cp -= 0x10000; text.push_back(static_cast<char16_t>(0xd800 + (cp >> 10))); text.push_back(static_cast<char16_t>(0xdc00 + (cp & 1023))); }
        else text.push_back(static_cast<char16_t>(cp));
    }
    out = utf16_to_utf8(text); return true;
}

std::uint64_t read_data(id, SEL, id data, std::uint64_t options, id *error) {
    if (error) *error = nullptr;
    if (!is_data(data)) return failure(error, "JSON input must be NSData");
    std::string input, message;
    if (!json_utf8(data_storage(data), input)) return failure(error, "Invalid Unicode encoding in JSON input");
    mm_json value;
    if (!mm_json_parse(input, value, message, true)) return failure(error, message);
    if (!(options & read_fragments) && !std::holds_alternative<mm_json::object>(value.value) && !std::holds_alternative<mm_json::array>(value.value))
        return failure(error, "JSON document must contain an object or array");
    id result = cocoa_value(value, options, message);
    return result ? ret(autorelease(result)) : failure(error, message);
}

bool quote(id string, std::string &out, std::uint64_t options) {
    const auto c = string_chars(string);
    for (CFIndex i = 0; i < c.length; ++i) {
        const auto u = c.data[i];
        if (u >= 0xd800 && u <= 0xdbff) {
            if (++i == c.length || c.data[i] < 0xdc00 || c.data[i] > 0xdfff) return false;
        } else if (u >= 0xdc00 && u <= 0xdfff) return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (char raw : to_utf8(string)) {
        const auto b = static_cast<unsigned char>(raw);
        if (b == '"' || b == '\\' || (b == '/' && !(options & unescaped_slashes))) { out += '\\'; out += raw; }
        else if (b < 32) { out += "\\u00"; out += hex[b >> 4]; out += hex[b & 15]; }
        else out += raw;
    }
    out += '"'; return true;
}

struct writer {
    std::uint64_t options;
    std::string out;
    std::unordered_set<id> active;
    bool value(id o, unsigned depth = 0) {
        if (!o || depth > 128) return false;
        if (o == null_object()) { out += "null"; return true; }
        if (is_string(o)) return quote(o, out, options);
        if (is_number(o)) {
            if (o == number_bool(true) || o == number_bool(false)) { out += o == number_bool(true) ? "true" : "false"; return true; }
            if (number_is_float(o)) {
                const double n = number_double(o);
                if (!std::isfinite(n)) return false;
                char bytes[64]; auto r = std::to_chars(bytes, bytes + sizeof bytes, n);
                if (r.ec != std::errc{}) return false;
                out.append(bytes, r.ptr); return true;
            }
            const char *type = reinterpret_cast<const char *>(send(o, "objCType"));
            if (type && *type == 'B') { out += send(o, "boolValue") ? "true" : "false"; return true; }
            out += to_utf8(obj(send(o, "stringValue"))); return true;
        }
        const bool dict = is_dictionary(o);
        if (!dict && !is_array(o)) return false;
        if (!active.insert(o).second) return false;
        std::vector<id> items;
        if (dict) {
            dictionary_keys(o, items);
            for (id key : items) if (!is_string(key)) { active.erase(o); return false; }
            if (options & sorted_keys) std::sort(items.begin(), items.end(), [](id a, id b) { return to_utf8(a) < to_utf8(b); });
        } else items = array_storage(o);
        out += dict ? '{' : '[';
        bool first = true;
        for (id item : items) {
            if (!first) out += ',';
            first = false;
            if (options & pretty) { out += '\n'; out.append((depth + 1) * 2, ' '); }
            if (dict) {
                if (!quote(item, out, options)) { active.erase(o); return false; }
                out += (options & pretty) ? ": " : ":";
            }
            if (!value(dict ? dictionary_get(o, item) : item, depth + 1)) { active.erase(o); return false; }
        }
        if (!items.empty() && (options & pretty)) { out += '\n'; out.append(depth * 2, ' '); }
        out += dict ? '}' : ']'; active.erase(o); return true;
    }
    bool document(id o) {
        return ((options & write_fragments) || is_array(o) || is_dictionary(o)) && value(o);
    }
};

std::uint64_t write_data(id, SEL, id object, std::uint64_t options, id *error) {
    if (error) *error = nullptr;
    writer w{options, {}, {}};
    if (!w.document(object)) return failure(error, "Object cannot be represented as valid JSON");
    return ret(autorelease(data_create(w.out.data(), static_cast<CFIndex>(w.out.size()), false)));
}
std::uint64_t valid_object(id, SEL, id object) { writer w{0, {}, {}}; return w.document(object); }
} // namespace

void register_json() {
    Class c = mm_objc_define_class("NSJSONSerialization", cls("NSObject"), sizeof(mm_objc_object), FND_CLASS_REF(NSJSONSerialization), FND_META_REF(NSJSONSerialization));
    FND_CLASS_METHODS(c,
        {"JSONObjectWithData:options:error:", IMP(read_data), "@40@0:8@16Q24^@32"},
        {"dataWithJSONObject:options:error:", IMP(write_data), "@40@0:8@16Q24^@32"},
        {"isValidJSONObject:", IMP(valid_object), "B24@0:8@16"});
}
} // namespace fnd
