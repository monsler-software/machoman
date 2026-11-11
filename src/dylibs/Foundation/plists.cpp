/* Property lists: the XML format (DTD "-//Apple//DTD PLIST 1.0//EN") and
 * the binary "bplist00" format, both directions. Info.plist files in app
 * bundles use either, so both readers are required; writing defaults to
 * XML, which is what NSDictionary -writeToFile: produces. */
#include "foundation.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace fnd {

/* Class storage the guest binds to: external linkage, so not inside the
 * anonymous namespace below. */
FND_CLASS(NSPropertyListSerialization);
FND_CLASS(__NSCFKeyedArchiverUID);

namespace {

Class g_uid = nullptr;

/* ---- XML ---------------------------------------------------------------- */

struct xml_parser {
    std::string_view s;
    std::size_t i = 0;
    std::string error;

    void skip_space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    /* Skips whitespace, comments, the XML declaration and the doctype. */
    void skip_junk() {
        for (;;) {
            skip_space();
            if (s.compare(i, 4, "<!--") == 0) { const auto e = s.find("-->", i); i = e == std::string_view::npos ? s.size() : e + 3; continue; }
            if (s.compare(i, 2, "<?") == 0) { const auto e = s.find("?>", i); i = e == std::string_view::npos ? s.size() : e + 2; continue; }
            if (s.compare(i, 9, "<!DOCTYPE") == 0) { const auto e = s.find('>', i); i = e == std::string_view::npos ? s.size() : e + 1; continue; }
            return;
        }
    }
    bool eat(std::string_view tag) {
        skip_junk();
        if (s.compare(i, tag.size(), tag) != 0) return false;
        i += tag.size();
        return true;
    }
    /* Reads the next tag name, e.g. "dict", "/dict", "true/". */
    std::string_view peek_tag() {
        skip_junk();
        if (i >= s.size() || s[i] != '<') return {};
        const auto end = s.find('>', i);
        if (end == std::string_view::npos) return {};
        return s.substr(i + 1, end - i - 1);
    }
    void consume_tag() { const auto end = s.find('>', i); i = end == std::string_view::npos ? s.size() : end + 1; }
    /* Text up to the matching close tag, with entities decoded. */
    std::string text_until(std::string_view close) {
        const auto end = s.find(close, i);
        std::string_view raw = s.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
        i = end == std::string_view::npos ? s.size() : end + close.size();
        std::string out;
        out.reserve(raw.size());
        for (std::size_t k = 0; k < raw.size();) {
            if (raw[k] != '&') { out += raw[k++]; continue; }
            const auto semi = raw.find(';', k);
            if (semi == std::string_view::npos) { out += raw[k++]; continue; }
            const std::string_view ent = raw.substr(k + 1, semi - k - 1);
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                const std::uint32_t cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                             ? static_cast<std::uint32_t>(std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16))
                                             : static_cast<std::uint32_t>(std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10));
                std::u16string u;
                if (cp >= 0x10000) { const std::uint32_t v = cp - 0x10000; u.push_back(static_cast<char16_t>(0xD800 + (v >> 10))); u.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF))); }
                else u.push_back(static_cast<char16_t>(cp));
                out += utf16_to_utf8(u);
            } else out.append(raw.substr(k, semi - k + 1));
            k = semi + 1;
        }
        return out;
    }

    /* Returns a retained object, or null on error. */
    id value() {
        const std::string_view tag = peek_tag();
        if (tag.empty()) { error = "unexpected end of plist"; return nullptr; }
        consume_tag();
        if (tag == "dict") {
            id d = dictionary_create(true);
            for (;;) {
                const std::string_view t = peek_tag();
                if (t == "/dict") { consume_tag(); return d; }
                if (t != "key") { error = "expected <key> in <dict>"; release(d); return nullptr; }
                consume_tag();
                id key = string_from_utf8(text_until("</key>"));
                id v = value();
                if (!v) { release(key); release(d); return nullptr; }
                dictionary_set(d, key, v);
                release(key);
                release(v);
            }
        }
        if (tag == "array") {
            std::vector<id> items;
            for (;;) {
                const std::string_view t = peek_tag();
                if (t == "/array") {
                    consume_tag();
                    id a = array_create(items.data(), static_cast<CFIndex>(items.size()), false);
                    for (id e : items) release(e);
                    return a;
                }
                id v = value();
                if (!v) { for (id e : items) release(e); return nullptr; }
                items.push_back(v);
            }
        }
        if (tag == "string") return string_from_utf8(text_until("</string>"));
        if (tag == "key") return string_from_utf8(text_until("</key>"));
        if (tag == "integer") return number_from_int64(std::strtoll(text_until("</integer>").c_str(), nullptr, 10));
        if (tag == "real") return number_from_double(std::strtod(text_until("</real>").c_str(), nullptr));
        if (tag == "true" || tag == "true/") { if (tag == "true") eat("</true>"); return retain(number_bool(true)); }
        if (tag == "false" || tag == "false/") { if (tag == "false") eat("</false>"); return retain(number_bool(false)); }
        if (tag == "data") {
            const std::string b64 = text_until("</data>");
            static const auto decode6 = [](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (c == '+') return 62;
                if (c == '/') return 63;
                return -1;
            };
            std::string bytes;
            int acc = 0, bits = 0;
            for (char c : b64) {
                const int v = decode6(c);
                if (v < 0) continue;
                acc = (acc << 6) | v;
                bits += 6;
                if (bits >= 8) { bits -= 8; bytes.push_back(static_cast<char>((acc >> bits) & 0xFF)); }
            }
            return data_create(bytes.data(), static_cast<CFIndex>(bytes.size()), false);
        }
        if (tag == "date") return string_from_utf8(text_until("</date>")); /* NSDate is not implemented; keep the text */
        if (tag == "array/") return array_create(nullptr, 0, false);
        if (tag == "dict/") return dictionary_create(false);
        error = "unknown plist element <" + std::string(tag) + ">";
        return nullptr;
    }
};

/* ---- binary ------------------------------------------------------------- */

struct bplist_parser {
    std::string_view s;
    std::uint8_t offset_size = 0, ref_size = 0;
    std::uint64_t num_objects = 0, top = 0, offset_table = 0;
    std::string error;

    std::uint64_t read_be(std::uint64_t at, std::uint8_t n) const {
        std::uint64_t v = 0;
        for (std::uint8_t k = 0; k < n; k++) v = (v << 8) | static_cast<unsigned char>(s[at + k]);
        return v;
    }
    bool valid(std::uint64_t at, std::uint64_t n) const { return at + n <= s.size(); }

    bool parse_trailer() {
        if (s.size() < 40 || s.compare(0, 6, "bplist") != 0) { error = "not a binary plist"; return false; }
        const std::uint64_t t = s.size() - 32;
        offset_size = static_cast<std::uint8_t>(s[t + 6]);
        ref_size = static_cast<std::uint8_t>(s[t + 7]);
        num_objects = read_be(t + 8, 8);
        top = read_be(t + 16, 8);
        offset_table = read_be(t + 24, 8);
        if (!offset_size || !ref_size || offset_size > 8 || ref_size > 8) { error = "bad binary plist trailer"; return false; }
        if (!valid(offset_table, num_objects * offset_size)) { error = "offset table out of bounds"; return false; }
        return true;
    }

    std::uint64_t object_offset(std::uint64_t index) const { return read_be(offset_table + index * offset_size, offset_size); }

    /* Marker byte + optional length; returns the payload start. */
    std::uint64_t length_at(std::uint64_t at, std::uint8_t low, std::uint64_t &len) {
        if (low != 0x0F) { len = low; return at + 1; }
        const std::uint8_t m = static_cast<std::uint8_t>(s[at + 1]);
        const std::uint64_t n = std::uint64_t{1} << (m & 0x0F);
        len = read_be(at + 2, static_cast<std::uint8_t>(n));
        return at + 2 + n;
    }

    id object(std::uint64_t index, int depth) {
        if (index >= num_objects || depth > 64) { error = "bad object reference"; return nullptr; }
        const std::uint64_t at = object_offset(index);
        if (!valid(at, 1)) { error = "object out of bounds"; return nullptr; }
        const std::uint8_t marker = static_cast<std::uint8_t>(s[at]);
        const std::uint8_t high = marker >> 4, low = marker & 0x0F;
        switch (high) {
        case 0x0:
            if (low == 0) return retain(null_object());
            if (low == 8) return retain(number_bool(false));
            if (low == 9) return retain(number_bool(true));
            return retain(null_object());
        case 0x1: { /* int, 2^low bytes big endian */
            const std::uint64_t n = std::uint64_t{1} << low;
            if (!valid(at + 1, n)) { error = "truncated integer"; return nullptr; }
            std::int64_t v = static_cast<std::int64_t>(read_be(at + 1, static_cast<std::uint8_t>(n)));
            if (n == 1) v = static_cast<std::uint8_t>(v);
            else if (n == 2) v = static_cast<std::uint16_t>(v);
            else if (n == 4) v = static_cast<std::uint32_t>(v);
            return number_from_int64(v);
        }
        case 0x2: { /* real */
            const std::uint64_t n = std::uint64_t{1} << low;
            if (!valid(at + 1, n)) { error = "truncated real"; return nullptr; }
            if (n == 4) { const std::uint32_t bits = static_cast<std::uint32_t>(read_be(at + 1, 4)); float f; std::memcpy(&f, &bits, 4); return number_from_double(f); }
            const std::uint64_t bits = read_be(at + 1, 8); double d; std::memcpy(&d, &bits, 8); return number_from_double(d);
        }
        case 0x3: { /* date: seconds since 2001-01-01, kept as a number */
            if (!valid(at + 1, 8)) { error = "truncated date"; return nullptr; }
            const std::uint64_t bits = read_be(at + 1, 8); double d; std::memcpy(&d, &bits, 8); return number_from_double(d);
        }
        case 0x4: { /* data */
            std::uint64_t len = 0;
            const std::uint64_t p = length_at(at, low, len);
            if (!valid(p, len)) { error = "truncated data"; return nullptr; }
            return data_create(s.data() + p, static_cast<CFIndex>(len), false);
        }
        case 0x5: { /* ASCII string */
            std::uint64_t len = 0;
            const std::uint64_t p = length_at(at, low, len);
            if (!valid(p, len)) { error = "truncated string"; return nullptr; }
            std::u16string u;
            for (std::uint64_t k = 0; k < len; k++) u.push_back(static_cast<char16_t>(static_cast<unsigned char>(s[p + k])));
            return string_from_utf16(u);
        }
        case 0x6: { /* UTF-16BE string */
            std::uint64_t len = 0;
            const std::uint64_t p = length_at(at, low, len);
            if (!valid(p, len * 2)) { error = "truncated unicode string"; return nullptr; }
            std::u16string u;
            for (std::uint64_t k = 0; k < len; k++) u.push_back(static_cast<char16_t>(read_be(p + k * 2, 2)));
            return string_from_utf16(u);
        }
        case 0x8: { /* UID: a reference into a keyed archive's $objects */
            const std::uint64_t n = low + 1;
            return uid_create(read_be(at + 1, static_cast<std::uint8_t>(n)));
        }
        case 0xA: case 0xC: { /* array, set */
            std::uint64_t len = 0;
            const std::uint64_t p = length_at(at, low, len);
            if (!valid(p, len * ref_size)) { error = "truncated array"; return nullptr; }
            std::vector<id> items;
            for (std::uint64_t k = 0; k < len; k++) {
                id v = object(read_be(p + k * ref_size, ref_size), depth + 1);
                if (!v) { for (id e : items) release(e); return nullptr; }
                items.push_back(v);
            }
            id a = array_create(items.data(), static_cast<CFIndex>(items.size()), false);
            for (id e : items) release(e);
            return a;
        }
        case 0xD: { /* dict */
            std::uint64_t len = 0;
            const std::uint64_t p = length_at(at, low, len);
            if (!valid(p, len * ref_size * 2)) { error = "truncated dict"; return nullptr; }
            id d = dictionary_create(true);
            for (std::uint64_t k = 0; k < len; k++) {
                id key = object(read_be(p + k * ref_size, ref_size), depth + 1);
                id v = key ? object(read_be(p + (len + k) * ref_size, ref_size), depth + 1) : nullptr;
                if (!key || !v) { if (key) release(key); release(d); return nullptr; }
                dictionary_set(d, key, v);
                release(key);
                release(v);
            }
            return d;
        }
        default:
            error = "unknown binary plist marker";
            return nullptr;
        }
    }
};

/* ---- XML writer --------------------------------------------------------- */

void xml_escape(std::string_view in, std::string &out) {
    for (char c : in) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        default: out += c;
        }
    }
}

void base64(std::string_view in, std::string &out) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(in[i]);
        const unsigned b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        const unsigned c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        out += tbl[a >> 2];
        out += tbl[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < in.size() ? tbl[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < in.size() ? tbl[c & 63] : '=';
    }
}

bool write_value(id v, std::string &out, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), '\t');
    if (!v) return false;
    if (is_string(v)) { out += pad + "<string>"; xml_escape(to_utf8(v), out); out += "</string>\n"; return true; }
    if (is_number(v)) {
        if (v == number_bool(true)) { out += pad + "<true/>\n"; return true; }
        if (v == number_bool(false)) { out += pad + "<false/>\n"; return true; }
        char buf[64];
        if (number_is_float(v)) { std::snprintf(buf, sizeof buf, "%.17g", number_double(v)); out += pad + "<real>" + buf + "</real>\n"; }
        else { std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(number_int64(v))); out += pad + "<integer>" + buf + "</integer>\n"; }
        return true;
    }
    if (is_data(v)) { out += pad + "<data>\n" + pad; base64(data_storage(v), out); out += "\n" + pad + "</data>\n"; return true; }
    if (is_array(v)) {
        auto &items = array_storage(v);
        if (items.empty()) { out += pad + "<array/>\n"; return true; }
        out += pad + "<array>\n";
        for (id e : items) if (!write_value(e, out, indent + 1)) return false;
        out += pad + "</array>\n";
        return true;
    }
    if (is_dictionary(v)) {
        std::vector<id> keys;
        dictionary_keys(v, keys);
        if (keys.empty()) { out += pad + "<dict/>\n"; return true; }
        /* Apple writes keys sorted; match that so files compare equal. */
        std::sort(keys.begin(), keys.end(), [](id a, id b) { return to_utf8(a) < to_utf8(b); });
        out += pad + "<dict>\n";
        for (id k : keys) {
            out += pad + "\t<key>";
            xml_escape(to_utf8(k), out);
            out += "</key>\n";
            if (!write_value(dictionary_get(v, k), out, indent + 1)) return false;
        }
        out += pad + "</dict>\n";
        return true;
    }
    if (v == null_object()) { out += pad + "<string></string>\n"; return true; }
    return false;
}

/* ---- binary writer ------------------------------------------------------- */

void append_be(std::string &out, std::uint64_t v, int n) { for (int k = n - 1; k >= 0; k--) out += static_cast<char>((v >> (k * 8)) & 0xFF); }

struct bplist_writer {
    std::string body{"bplist00"};
    std::vector<std::uint64_t> offsets;
    std::uint8_t ref_size = 1;

    void marker(std::uint8_t high, std::uint64_t len) {
        if (len < 15) { body += static_cast<char>((high << 4) | len); return; }
        body += static_cast<char>((high << 4) | 0x0F);
        if (len < 0x100) { body += static_cast<char>(0x10); append_be(body, len, 1); }
        else if (len < 0x10000) { body += static_cast<char>(0x11); append_be(body, len, 2); }
        else if (len < 0x100000000ull) { body += static_cast<char>(0x12); append_be(body, len, 4); }
        else { body += static_cast<char>(0x13); append_be(body, len, 8); }
    }

    /* Flattens the object graph; objects are written in the order visited. */
    void collect(id v, std::vector<id> &objects) {
        objects.push_back(v);
        if (is_array(v)) { for (id e : array_storage(v)) collect(e, objects); return; }
        if (is_dictionary(v)) {
            std::vector<id> keys;
            dictionary_keys(v, keys);
            std::sort(keys.begin(), keys.end(), [](id a, id b) { return to_utf8(a) < to_utf8(b); });
            for (id k : keys) collect(k, objects);
            for (id k : keys) collect(dictionary_get(v, k), objects);
        }
    }

    bool write(id v, std::string &out) {
        std::vector<id> objects;
        collect(v, objects);
        std::map<id, std::uint64_t> index;
        for (std::uint64_t i = 0; i < objects.size(); i++) index.emplace(objects[i], i);
        ref_size = objects.size() < 0x100 ? 1 : objects.size() < 0x10000 ? 2 : 4;
        offsets.resize(objects.size());
        for (std::uint64_t i = 0; i < objects.size(); i++) {
            offsets[i] = body.size();
            id o = objects[i];
            if (o == number_bool(true)) { body += static_cast<char>(0x09); continue; }
            if (o == number_bool(false)) { body += static_cast<char>(0x08); continue; }
            if (o == null_object()) { body += static_cast<char>(0x00); continue; }
            if (is_string(o)) {
                const chars c = string_chars(o);
                bool ascii = true;
                for (CFIndex k = 0; k < c.length; k++) if (c.data[k] >= 0x80) { ascii = false; break; }
                marker(ascii ? 0x5 : 0x6, static_cast<std::uint64_t>(c.length));
                for (CFIndex k = 0; k < c.length; k++) { if (ascii) body += static_cast<char>(c.data[k]); else append_be(body, c.data[k], 2); }
                continue;
            }
            if (is_number(o)) {
                if (number_is_float(o)) { const double d = number_double(o); std::uint64_t bits; std::memcpy(&bits, &d, 8); body += static_cast<char>(0x23); append_be(body, bits, 8); }
                else { body += static_cast<char>(0x13); append_be(body, static_cast<std::uint64_t>(number_int64(o)), 8); }
                continue;
            }
            if (is_data(o)) { const std::string &b = data_storage(o); marker(0x4, b.size()); body += b; continue; }
            if (is_array(o)) {
                auto &items = array_storage(o);
                marker(0xA, items.size());
                for (id e : items) append_be(body, index[e], ref_size);
                continue;
            }
            if (is_dictionary(o)) {
                std::vector<id> keys;
                dictionary_keys(o, keys);
                std::sort(keys.begin(), keys.end(), [](id a, id b) { return to_utf8(a) < to_utf8(b); });
                marker(0xD, keys.size());
                for (id k : keys) append_be(body, index[k], ref_size);
                for (id k : keys) append_be(body, index[dictionary_get(o, k)], ref_size);
                continue;
            }
            return false;
        }
        const std::uint64_t table = body.size();
        const std::uint8_t offset_size = table < 0x100 ? 1 : table < 0x10000 ? 2 : 4;
        for (std::uint64_t off : offsets) append_be(body, off, offset_size);
        body.append(6, '\0');
        body += static_cast<char>(offset_size);
        body += static_cast<char>(ref_size);
        append_be(body, objects.size(), 8);
        append_be(body, 0, 8); /* top object */
        append_be(body, table, 8);
        out = std::move(body);
        return true;
    }
};

} // namespace

id plist_parse(std::string_view bytes, std::string *error) {
    if (bytes.size() >= 8 && bytes.compare(0, 6, "bplist") == 0) {
        bplist_parser p{bytes, 0, 0, 0, 0, 0, {}};
        if (!p.parse_trailer()) { if (error) *error = p.error; return nullptr; }
        id v = p.object(p.top, 0);
        if (!v && error) *error = p.error;
        return v;
    }
    xml_parser p{bytes, 0, {}};
    if (!p.eat("<plist")) { if (error) *error = "missing <plist>"; return nullptr; }
    p.consume_tag();
    id v = p.value();
    if (!v && error) *error = p.error;
    return v;
}

bool plist_write_xml(id plist, std::string &out) {
    out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          "<plist version=\"1.0\">\n";
    if (!write_value(plist, out, 0)) return false;
    out += "</plist>\n";
    return true;
}

bool plist_write_binary(id plist, std::string &out) { bplist_writer w; return w.write(plist, out); }

struct uid_object { mm_objc_object base; std::uint64_t value = 0; };
std::uint64_t uid_value_imp(id self, SEL) { return as<uid_object>(self)->value; }

/* UIDs are created by the binary reader and read back by a nib loader. */
id uid_create(std::uint64_t value) {
    uid_object *o = make<uid_object>(g_uid);
    o->value = value;
    return &o->base;
}
bool is_uid(id o) { return o && mm_objc_object_class(o) == g_uid; }
std::uint64_t uid_value(id o) { return is_uid(o) ? as<uid_object>(o)->value : 0; }

/* ---- keyed-archive UIDs ------------------------------------------------- */

/* A UID is a plist type of its own: inside a keyed archive it is an index
 * into $objects, and reading it as a number would make it indistinguishable
 * from one. Apple's class for it has the same name. */
void register_uid() {
    Class c = mm_objc_define_class("__NSCFKeyedArchiverUID", cls("NSObject"), sizeof(uid_object),
                                   FND_CLASS_REF(__NSCFKeyedArchiverUID), FND_META_REF(__NSCFKeyedArchiverUID));
    register_lifecycle<uid_object>(c);
    static auto create = [](id, SEL, std::uint64_t value) -> std::uint64_t { return ret(autorelease(uid_create(value))); };
    mm_objc_add_class_method(c, "uidWithValue:", IMP(+create), "@24@0:8Q16");
    FND_METHODS(c,
        {"value", IMP(uid_value_imp), "Q16@0:8"},
        {"unsignedIntegerValue", IMP(uid_value_imp), "Q16@0:8"},
        {"integerValue", IMP(uid_value_imp), "q16@0:8"});
    g_uid = c;
}

void register_plists() {
    register_uid();
    /* NSPropertyListSerialization is a class of class methods only. */
    Class c = mm_objc_define_class("NSPropertyListSerialization", cls("NSObject"), sizeof(mm_objc_object),
                                   FND_CLASS_REF(NSPropertyListSerialization), FND_META_REF(NSPropertyListSerialization));
    static auto from_data = [](id, SEL, id data, std::uint64_t, std::uint64_t *format, id *error) -> std::uint64_t {
        std::string err;
        id v = plist_parse(data_storage(data), &err);
        if (!v) { if (error) *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 3840, nullptr)); return 0; }
        if (format) *format = data_storage(data).compare(0, 6, "bplist") == 0 ? 200 : 100;
        return ret(autorelease(v));
    };
    static auto to_data = [](id, SEL, id plist, std::uint64_t format, std::uint64_t, id *error) -> std::uint64_t {
        std::string out;
        const bool ok = format == 200 ? plist_write_binary(plist, out) : plist_write_xml(plist, out);
        if (!ok) { if (error) *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 3851, nullptr)); return 0; }
        return ret(autorelease(data_create(out.data(), static_cast<CFIndex>(out.size()), false)));
    };
    FND_CLASS_METHODS(c,
        {"propertyListWithData:options:format:error:", IMP(from_data), "@48@0:8@16Q24^Q32^@40"},
        {"dataWithPropertyList:format:options:error:", IMP(to_data), "@48@0:8@16q24Q32^@40"});
}

extern "C" {

id CFPropertyListCreateWithData_(void *, id data, std::uint64_t, std::uint64_t *format, id *error) DARWIN(CFPropertyListCreateWithData);
id CFPropertyListCreateWithData_(void *, id data, std::uint64_t, std::uint64_t *format, id *error) {
    std::string err;
    id v = plist_parse(data_storage(data), &err);
    if (!v) { if (error) *error = error_create(constant_string("NSCocoaErrorDomain"), 3840, nullptr); return nullptr; }
    if (format) *format = data_storage(data).compare(0, 6, "bplist") == 0 ? 200 : 100;
    return v;
}
id CFPropertyListCreateFromXMLData_(void *a, id data, std::uint64_t, id *error) DARWIN(CFPropertyListCreateFromXMLData);
id CFPropertyListCreateFromXMLData_(void *a, id data, std::uint64_t, id *error) { return CFPropertyListCreateWithData_(a, data, 0, nullptr, error); }
id CFPropertyListCreateData_(void *, id plist, std::uint64_t format, std::uint64_t, id *error) DARWIN(CFPropertyListCreateData);
id CFPropertyListCreateData_(void *, id plist, std::uint64_t format, std::uint64_t, id *error) {
    std::string out;
    const bool ok = format == 200 ? plist_write_binary(plist, out) : plist_write_xml(plist, out);
    if (!ok) { if (error) *error = error_create(constant_string("NSCocoaErrorDomain"), 3851, nullptr); return nullptr; }
    return data_create(out.data(), static_cast<CFIndex>(out.size()), false);
}
id CFPropertyListCreateXMLData_(void *a, id plist) DARWIN(CFPropertyListCreateXMLData);
id CFPropertyListCreateXMLData_(void *a, id plist) { return CFPropertyListCreateData_(a, plist, 100, 0, nullptr); }
id CFPropertyListCreateDeepCopy_(void *, id plist, std::uint64_t) DARWIN(CFPropertyListCreateDeepCopy);
id CFPropertyListCreateDeepCopy_(void *, id plist, std::uint64_t) {
    std::string out;
    if (!plist_write_binary(plist, out)) return nullptr;
    return plist_parse(out, nullptr);
}
/* Streams are only used for Info.plist reading; read the whole file. */
id CFPropertyListCreateWithStream_(void *, id stream, CFIndex, std::uint64_t, std::uint64_t *format, id *error) DARWIN(CFPropertyListCreateWithStream);
id CFPropertyListCreateWithStream_(void *, id stream, CFIndex, std::uint64_t, std::uint64_t *format, id *error) {
    id data = obj(send(stream, "machomanReadAllData"));
    if (!data) { if (error) *error = error_create(constant_string("NSCocoaErrorDomain"), 260, nullptr); return nullptr; }
    return CFPropertyListCreateWithData_(nullptr, data, 0, format, error);
}

} // extern "C"

} // namespace fnd
