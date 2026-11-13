#include "machoman/json.h"
#include <cstdint>
#include <stdexcept>

namespace {
struct parser {
    std::string_view input;
    std::size_t at = 0;
    [[noreturn]] void fail(const char *message) const {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(at));
    }
    void space() { while (at < input.size() && (input[at]==' ' || input[at]=='\t' || input[at]=='\n' || input[at]=='\r')) ++at; }
    bool take(char c) { space(); if (at < input.size() && input[at] == c) { ++at; return true; } return false; }
    void expect(char c) { if (!take(c)) fail("unexpected token"); }
    std::uint32_t hex4() {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (at == input.size()) fail("truncated unicode escape");
            const char c = input[at++];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
            else fail("invalid unicode escape");
            result = result * 16 + digit;
        }
        return result;
    }
    static void utf8(std::string &out, std::uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xe0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
        else { out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
    }
    std::string string() {
        expect('"'); std::string out;
        while (at < input.size()) {
            const auto c = static_cast<unsigned char>(input[at++]);
            if (c == '"') return out;
            if (c < 32) fail("control character in string");
            if (c == '\\') {
                if (at == input.size()) fail("truncated escape");
                switch (input[at++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    auto cp = hex4();
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (input.substr(at, 2) != "\\u") fail("unpaired surrogate");
                        at += 2; const auto low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail("unpaired surrogate");
                        cp = 0x10000 + (cp - 0xd800) * 1024 + low - 0xdc00;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) fail("unpaired surrogate");
                    utf8(out, cp); break;
                }
                default: fail("invalid escape");
                }
            } else if (c < 128) out += static_cast<char>(c);
            else {
                unsigned count; std::uint32_t cp, minimum;
                if (c >= 0xc2 && c <= 0xdf) { count=1; cp=c & 31; minimum=0x80; }
                else if (c >= 0xe0 && c <= 0xef) { count=2; cp=c & 15; minimum=0x800; }
                else if (c >= 0xf0 && c <= 0xf4) { count=3; cp=c & 7; minimum=0x10000; }
                else fail("invalid UTF-8");
                for (unsigned i=0; i<count; ++i) {
                    if (at == input.size()) fail("truncated UTF-8");
                    const auto next=static_cast<unsigned char>(input[at++]);
                    if ((next & 0xc0) != 0x80) fail("invalid UTF-8 continuation");
                    cp=(cp << 6) | (next & 63);
                }
                if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) fail("invalid UTF-8 codepoint");
                utf8(out, cp);
            }
        }
        fail("unterminated string");
    }
    mm_json parse(unsigned depth=0) {
        if (depth > 128) fail("JSON nesting limit exceeded");
        space(); if (at == input.size()) fail("missing value");
        if (input[at]=='"') return mm_json(string());
        if (take('{')) {
            mm_json::object out;
            if (!take('}')) do {
                space(); if (at == input.size() || input[at]!='"') fail("object key must be a string");
                auto key=string(); expect(':'); auto val=parse(depth+1);
                if (!out.emplace(std::move(key), std::move(val)).second) fail("duplicate object key");
                if (take('}')) return mm_json(std::move(out));
                expect(',');
            } while (true);
            return mm_json(std::move(out));
        }
        if (take('[')) {
            mm_json::array out;
            if (!take(']')) do {
                out.push_back(parse(depth+1));
                if (take(']')) return mm_json(std::move(out));
                expect(',');
            } while (true);
            return mm_json(std::move(out));
        }
        for (const auto literal : {std::string_view("null"), std::string_view("true"), std::string_view("false")})
            if (input.substr(at, literal.size())==literal) {
                at += literal.size();
                if (literal=="null") return {};
                return mm_json(literal=="true");
            }
        const auto start=at;
        if (input[at]=='-') ++at;
        if (at == input.size()) fail("truncated number");
        if (input[at]=='0') ++at;
        else { if (input[at]<'1' || input[at]>'9') fail("invalid number"); while (at<input.size() && input[at]>='0' && input[at]<='9') ++at; }
        auto digits=[&] {
            const auto first=at;
            while (at<input.size() && input[at]>='0' && input[at]<='9') ++at;
            if (at==first) fail("missing number digits");
        };
        if (at<input.size() && input[at]=='.') { ++at; digits(); }
        if (at<input.size() && (input[at]=='e' || input[at]=='E')) {
            ++at; if (at<input.size() && (input[at]=='+' || input[at]=='-')) ++at; digits();
        }
        return mm_json(mm_json_number{std::string(input.substr(start, at-start))});
    }
};
std::string quote(std::string_view text) {
    constexpr char hex[]="0123456789abcdef";
    std::string out="\"";
    for (char raw : text) {
        const auto c=static_cast<unsigned char>(raw);
        if (c=='"' || c=='\\') { out+='\\'; out+=raw; }
        else if (c<32) { out+="\\u00"; out+=hex[c >> 4]; out+=hex[c & 15]; }
        else out+=raw;
    }
    return out+'"';
}
std::string serialize(const mm_json &value, unsigned depth) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return "null";
    if (const auto *v=std::get_if<bool>(&value.value)) return *v ? "true" : "false";
    if (const auto *v=std::get_if<mm_json_number>(&value.value)) return v->text;
    if (const auto *v=std::get_if<std::string>(&value.value)) return quote(*v);
    std::string out;
    const std::string indent((depth+1)*2, ' '), end_indent(depth*2, ' ');
    if (const auto *v=std::get_if<mm_json::object>(&value.value)) {
        out="{";
        for (const auto &[key, entry] : *v) { if (out.size()>1) out+=','; out+='\n'+indent+quote(key)+": "+serialize(entry, depth+1); }
        if (!v->empty()) out+='\n'+end_indent;
        return out+'}';
    }
    out="[";
    const auto &v=std::get<mm_json::array>(value.value);
    for (const auto &entry : v) { if (out.size()>1) out+=','; out+='\n'+indent+serialize(entry, depth+1); }
    if (!v.empty()) out+='\n'+end_indent;
    return out+']';
}
}
bool mm_json_parse(std::string_view source, mm_json &out, std::string &error) {
    try {
        parser p{source}; auto value=p.parse(); p.space();
        if (p.at != source.size()) p.fail("trailing JSON data");
        out=std::move(value); error.clear(); return true;
    } catch (const std::runtime_error &e) { error=e.what(); return false; }
}
std::string mm_json_serialize(const mm_json &value) { return serialize(value, 0)+'\n'; }
