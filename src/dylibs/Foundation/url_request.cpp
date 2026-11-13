/* NSURLRequest preserves the URL and HTTP metadata handed to WebKit.
 * Mutable requests and their copies own independent header dictionaries. */
#include "foundation.h"
#include <algorithm>
#include <cctype>
#include <map>

namespace fnd {
FND_CLASS(NSURLRequest);
FND_CLASS(NSMutableURLRequest);
namespace {
struct request_object {
    mm_objc_object base;
    id url = nullptr, main_document = nullptr, body = nullptr, body_stream = nullptr;
    std::string method = "GET";
    std::map<std::string, std::pair<std::string, std::string>> headers;
    std::uint64_t cache_policy = 0;
    double timeout = 60;
    bool handle_cookies = true, pipelining = false;
    ~request_object() { release(url); release(main_document); release(body); release(body_stream); }
};
Class request_class, mutable_request_class;
void replace_copy(id &slot, id value) {
    id copy = value ? obj(send(value, "copy")) : nullptr;
    release(slot); slot = copy;
}
std::string header_key(id field) {
    auto key = to_utf8(field);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}
std::uint64_t init(id self, SEL, id url, std::uint64_t policy, double timeout) {
    auto *r = as<request_object>(self);
    replace_copy(r->url, url); r->cache_policy = policy; r->timeout = timeout;
    return ret(self);
}
std::uint64_t create(id c, SEL, id url, std::uint64_t policy, double timeout) {
    id self = &make<request_object>(reinterpret_cast<Class>(c))->base;
    init(self, nullptr, url, policy, timeout); return ret(autorelease(self));
}
std::uint64_t copy(id self, SEL sel, void *) {
    auto *r = as<request_object>(self);
    auto *out = make<request_object>(std::string_view(sel) == "mutableCopyWithZone:" ? mutable_request_class : request_class);
    out->url = retain(r->url); out->main_document = retain(r->main_document);
    out->body = retain(r->body); out->body_stream = retain(r->body_stream);
    out->method = r->method; out->headers = r->headers;
    out->cache_policy = r->cache_policy; out->timeout = r->timeout;
    out->handle_cookies = r->handle_cookies; out->pipelining = r->pipelining;
    return ret(out);
}
std::uint64_t set_header(id self, SEL sel, id value, id field) {
    if (!field) return 0;
    auto &headers = as<request_object>(self)->headers;
    auto key = header_key(field);
    if (!value) { headers.erase(key); return 0; }
    auto &entry = headers[key];
    const auto text = to_utf8(value);
    if (std::string_view(sel) == "addValue:forHTTPHeaderField:" && !entry.second.empty()) entry.second += "," + text;
    else entry.second = text;
    entry.first = to_utf8(field); return 0;
}
std::uint64_t all_headers(id self, SEL) {
    id dictionary = dictionary_create(false);
    for (const auto &[key, entry] : as<request_object>(self)->headers) {
        id name = string_from_utf8(entry.first), value = string_from_utf8(entry.second);
        dictionary_set(dictionary, name, value); release(name); release(value);
    }
    return ret(autorelease(dictionary));
}
} // namespace
void register_url_requests() {
    request_class = mm_objc_define_class("NSURLRequest", cls("NSObject"), sizeof(request_object),
        FND_CLASS_REF(NSURLRequest), FND_META_REF(NSURLRequest));
    mutable_request_class = mm_objc_define_class("NSMutableURLRequest", request_class, sizeof(request_object),
        FND_CLASS_REF(NSMutableURLRequest), FND_META_REF(NSMutableURLRequest));
    register_lifecycle<request_object>(request_class);
    register_lifecycle<request_object>(mutable_request_class);
    FND_CLASS_METHODS(request_class,
        {"requestWithURL:", IMP(+[](id c, SEL, id url) { return create(c, nullptr, url, 0, 60); }), "@24@0:8@16"},
        {"requestWithURL:cachePolicy:timeoutInterval:", IMP(create), "@40@0:8@16Q24d32"});
    FND_METHODS(request_class,
        {"initWithURL:", IMP(+[](id s, SEL, id url) { return init(s, nullptr, url, 0, 60); }), "@24@0:8@16"},
        {"initWithURL:cachePolicy:timeoutInterval:", IMP(init), "@40@0:8@16Q24d32"},
        {"URL", IMP(+[](id s, SEL) { return ret(as<request_object>(s)->url); }), "@16@0:8"},
        {"mainDocumentURL", IMP(+[](id s, SEL) { return ret(as<request_object>(s)->main_document); }), "@16@0:8"},
        {"HTTPBody", IMP(+[](id s, SEL) { return ret(as<request_object>(s)->body); }), "@16@0:8"},
        {"HTTPBodyStream", IMP(+[](id s, SEL) { return ret(as<request_object>(s)->body_stream); }), "@16@0:8"},
        {"HTTPMethod", IMP(+[](id s, SEL) { return ret(autorelease(string_from_utf8(as<request_object>(s)->method))); }), "@16@0:8"},
        {"cachePolicy", IMP(+[](id s, SEL) { return as<request_object>(s)->cache_policy; }), "Q16@0:8"},
        {"timeoutInterval", IMP(+[](id s, SEL) { return as<request_object>(s)->timeout; }), "d16@0:8"},
        {"HTTPShouldHandleCookies", IMP(+[](id s, SEL) -> std::uint64_t { return as<request_object>(s)->handle_cookies; }), "B16@0:8"},
        {"HTTPShouldUsePipelining", IMP(+[](id s, SEL) -> std::uint64_t { return as<request_object>(s)->pipelining; }), "B16@0:8"},
        {"allHTTPHeaderFields", IMP(all_headers), "@16@0:8"},
        {"valueForHTTPHeaderField:", IMP(+[](id s, SEL, id field) {
            const auto &headers = as<request_object>(s)->headers;
            auto it = headers.find(header_key(field));
            return it == headers.end() ? 0 : ret(autorelease(string_from_utf8(it->second.second)));
        }), "@24@0:8@16"},
        {"copyWithZone:", IMP(copy), "@24@0:8^v16"},
        {"mutableCopy", IMP(+[](id s, SEL) { return copy(s, "mutableCopyWithZone:", nullptr); }), "@16@0:8"},
        {"mutableCopyWithZone:", IMP(copy), "@24@0:8^v16"});
    FND_METHODS(mutable_request_class,
        {"setURL:", IMP(+[](id s, SEL, id value) -> std::uint64_t { replace_copy(as<request_object>(s)->url, value); return 0; }), "v24@0:8@16"},
        {"setMainDocumentURL:", IMP(+[](id s, SEL, id value) -> std::uint64_t { replace_copy(as<request_object>(s)->main_document, value); return 0; }), "v24@0:8@16"},
        {"setHTTPBody:", IMP(+[](id s, SEL, id value) -> std::uint64_t { replace_copy(as<request_object>(s)->body, value); return 0; }), "v24@0:8@16"},
        {"setHTTPBodyStream:", IMP(+[](id s, SEL, id value) -> std::uint64_t {
            auto *r = as<request_object>(s); retain(value); release(r->body_stream); r->body_stream = value; return 0;
        }), "v24@0:8@16"},
        {"setHTTPMethod:", IMP(+[](id s, SEL, id value) -> std::uint64_t { as<request_object>(s)->method = value ? to_utf8(value) : "GET"; return 0; }), "v24@0:8@16"},
        {"setCachePolicy:", IMP(+[](id s, SEL, std::uint64_t value) -> std::uint64_t { as<request_object>(s)->cache_policy = value; return 0; }), "v24@0:8Q16"},
        {"setTimeoutInterval:", IMP(+[](id s, SEL, double value) -> std::uint64_t { as<request_object>(s)->timeout = value; return 0; }), "v24@0:8d16"},
        {"setHTTPShouldHandleCookies:", IMP(+[](id s, SEL, std::uint64_t value) -> std::uint64_t { as<request_object>(s)->handle_cookies = value != 0; return 0; }), "v24@0:8B16"},
        {"setHTTPShouldUsePipelining:", IMP(+[](id s, SEL, std::uint64_t value) -> std::uint64_t { as<request_object>(s)->pipelining = value != 0; return 0; }), "v24@0:8B16"},
        {"setValue:forHTTPHeaderField:", IMP(set_header), "v32@0:8@16@24"},
        {"addValue:forHTTPHeaderField:", IMP(set_header), "v32@0:8@16@24"},
        {"setAllHTTPHeaderFields:", IMP(+[](id s, SEL, id value) -> std::uint64_t {
            as<request_object>(s)->headers.clear();
            if (is_dictionary(value)) {
                std::vector<id> keys; dictionary_keys(value, keys);
                for (id key : keys) set_header(s, "setValue:forHTTPHeaderField:", dictionary_get(value, key), key);
            }
            return 0;
        }), "v24@0:8@16"});
}
} // namespace fnd
