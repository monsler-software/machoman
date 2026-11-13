#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace stub_abi {
/* Bounded Objective-C encoding parser. Layout follows Darwin arm64, not
 * sizeof(host type). Unions, bitfields and vectors require a real bridge. */
struct type {
    char kind = 0, homogeneous = 0;
    std::size_t size = 0, alignment = 1, elements = 0;
    std::string object_class;
    std::vector<type> members;
    std::size_t offset = 0;
    bool valid = false;
};
inline std::size_t align(std::size_t n, std::size_t a) { return (n + a - 1) & ~(a - 1); }
inline type parse(std::string_view &s, unsigned depth = 0) {
    type t;
    if (depth > 32 || s.empty()) return t;
    while (!s.empty() && std::string_view("rnNoORV").find(s.front()) != std::string_view::npos) s.remove_prefix(1);
    if (s.empty()) return t;
    t.kind = s.front(); s.remove_prefix(1);
    switch (t.kind) {
    case 'v': break;
    case 'c': case 'C': case 'B': t.size = t.alignment = 1; break;
    case 's': case 'S': t.size = t.alignment = 2; break;
    case 'i': case 'I': t.size = t.alignment = 4; break;
    case 'l': case 'L': case 'q': case 'Q': case '#': case ':': case '*': t.size = t.alignment = 8; break;
    case 'f': t.size = t.alignment = 4; t.homogeneous = 'f'; t.elements = 1; break;
    case 'd': t.size = t.alignment = 8; t.homogeneous = 'd'; t.elements = 1; break;
    case '@':
        t.size = t.alignment = 8;
        if (!s.empty() && s.front() == '?') { t.kind = '^'; s.remove_prefix(1); }
        else if (!s.empty() && s.front() == '"') {
            s.remove_prefix(1); const auto end = s.find('"');
            if (end == std::string_view::npos) return {};
            t.object_class = s.substr(0, end); s.remove_prefix(end + 1);
        }
        break;
    case '^':
        t.size = t.alignment = 8;
        /* Pointee layout need not be known, but the encoding must be complete. */
        if (!s.empty() && s.front() == '?') s.remove_prefix(1);
        else if (!parse(s, depth + 1).valid) return {};
        break;
    case '[': {
        std::size_t count = 0;
        if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.front()))) return {};
        while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.front()))) {
            count = count * 10 + static_cast<unsigned>(s.front() - '0'); s.remove_prefix(1);
            if (count > 4096) return {};
        }
        const type item = parse(s, depth + 1);
        if (!item.valid || item.size == 0 || s.empty() || s.front() != ']') return {};
        s.remove_prefix(1);
        t.size = item.size * count; t.alignment = item.alignment;
        t.homogeneous = item.homogeneous; t.elements = item.elements * count;
        for (std::size_t i = 0; i < count; ++i) {
            t.members.push_back(item);
            t.members.back().offset = i * item.size;
        }
        break;
    }
    case '{': {
        const auto end = s.find_first_of("=}");
        if (end == std::string_view::npos || s[end] != '=') return {};
        s.remove_prefix(end + 1);
        bool first = true, homogeneous = true;
        while (!s.empty() && s.front() != '}') {
            if (s.front() == '"') {
                s.remove_prefix(1); const auto quote = s.find('"');
                if (quote == std::string_view::npos) return {};
                s.remove_prefix(quote + 1);
            }
            auto item = parse(s, depth + 1);
            if (!item.valid || item.size == 0) return {};
            item.offset = align(t.size, item.alignment);
            t.size = item.offset + item.size;
            t.alignment = std::max(t.alignment, item.alignment);
            if (first) t.homogeneous = item.homogeneous;
            if (!item.homogeneous || item.homogeneous != t.homogeneous) homogeneous = false;
            t.elements += item.elements; first = false;
            t.members.push_back(std::move(item));
            if (t.size > 4096) return {};
        }
        if (s.empty() || first) return {};
        s.remove_prefix(1); t.size = align(t.size, t.alignment);
        if (!homogeneous) t.homogeneous = 0;
        break;
    }
    default: return {};
    }
    if (t.size > 4096) return {};
    if (t.elements > 4) t.homogeneous = 0;
    t.valid = true;
    return t;
}
inline std::vector<type> signature(std::string_view s) {
    std::vector<type> result;
    while (!s.empty()) {
        auto t = parse(s);
        if (!t.valid || result.size() >= 64) return {};
        result.push_back(std::move(t));
        while (!s.empty() && (std::isdigit(static_cast<unsigned char>(s.front())) || s.front() == '+' || s.front() == '-')) s.remove_prefix(1);
    }
    if (result.size() < 3 || result[1].kind != '@' || result[2].kind != ':') return {};
    return result;
}
}
