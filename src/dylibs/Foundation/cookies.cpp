/* Cocoa's cookie bridge is also used by applications with their own HTTP
 * engine (Roblox uses guest libcurl). No network requests happen here. */
#include "foundation.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <unistd.h>
#include <mutex>
#include <unordered_map>

FND_CLASS(NSHTTPCookie);
FND_CLASS(NSHTTPCookieStorage);

#define COOKIE_KEY(name) extern "C" { EXPORT fnd::id name##_ __asm__("_" #name) = nullptr; }
COOKIE_KEY(NSHTTPCookieName)
COOKIE_KEY(NSHTTPCookieValue)
COOKIE_KEY(NSHTTPCookieDomain)
COOKIE_KEY(NSHTTPCookiePath)
COOKIE_KEY(NSHTTPCookieVersion)
COOKIE_KEY(NSHTTPCookieExpires)
COOKIE_KEY(NSHTTPCookieSecure)
COOKIE_KEY(NSHTTPCookieDiscard)
COOKIE_KEY(NSHTTPCookieMaximumAge)
COOKIE_KEY(NSHTTPCookieOriginURL)
COOKIE_KEY(NSHTTPCookiePort)
COOKIE_KEY(NSHTTPCookieComment)
COOKIE_KEY(NSHTTPCookieCommentURL)
COOKIE_KEY(NSHTTPCookieManagerCookiesChangedNotification)
COOKIE_KEY(NSHTTPCookieManagerAcceptPolicyChangedNotification)
#undef COOKIE_KEY

namespace fnd {
namespace {
Class cookie_class, storage_class;
struct cookie_object {
    mm_objc_object base;
    id properties = nullptr;
    std::string name, value, domain, path;
    double expires = 0;
    std::uint64_t version = 0;
    bool secure = false, http_only = false, session = true, host_only = true, has_expiry = false;
    ~cookie_object() { release(properties); }
};
struct storage_object {
    mm_objc_object base;
    std::mutex mutex;
    std::vector<id> cookies;
    std::uint64_t policy = 0; // NSHTTPCookieAcceptPolicyAlways
    std::string path;
    ~storage_object() { for (id cookie : cookies) release(cookie); }
};
std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return out;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}
double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
id property(id d, const char *key) { return dictionary_get(d, constant_string(key)); }
void put(id d, const char *key, std::string_view value) {
    id text = string_from_utf8(value); dictionary_set(d, constant_string(key), text); release(text);
}
bool truth(id value) {
    if (is_number(value)) return number_int64(value) != 0;
    auto text = lower(to_utf8(value));
    return text == "true" || text == "yes" || text == "1";
}
bool integer(std::string_view text, std::int64_t &result) {
    text = trim(text);
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool domain_matches(std::string_view host, std::string_view domain, bool host_only) {
    while (!domain.empty() && domain.front() == '.') domain.remove_prefix(1);
    return host == domain || (!host_only && host.size() > domain.size() && host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.');
}
struct cookie_url { std::string host, path; bool secure = false; };
cookie_url parse_url(id url) {
    cookie_url result;
    if (!is_url(url)) return result;
    std::string text = to_utf8(obj(send(url, "absoluteString")));
    auto sep = text.find("://");
    if (sep == std::string::npos) return result;
    auto scheme = lower(std::string_view(text).substr(0, sep));
    if (scheme != "http" && scheme != "https") return result;
    result.secure = scheme == "https";
    std::string_view rest(text); rest.remove_prefix(sep + 3);
    auto end = rest.find_first_of("/?#");
    auto authority = rest.substr(0, end);
    if (auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (authority.starts_with('[')) {
        auto close = authority.find(']');
        if (close == std::string_view::npos) return {};
        result.host = lower(authority.substr(0, close + 1));
    } else result.host = lower(authority.substr(0, authority.find(':')));
    result.path = end != std::string_view::npos && rest[end] == '/' ? std::string(rest.substr(end, rest.find_first_of("?#", end) - end)) : "/";
    return result;
}
bool cookie_date(std::string_view text, double &result) {
    // RFC 6265 date tokens also cover RFC 850 and asctime cookie dates.
    int day = -1, month = -1, year = -1, hour = -1, minute = -1, second = -1;
    constexpr std::string_view months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string s = lower(text);
    std::size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && !(std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == ':')) ++pos;
        auto start = pos;
        while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == ':')) ++pos;
        if (start == pos) break;
        auto token = std::string_view(s).substr(start, pos - start);
        int h, m, sec;
        std::string token_string(token);
        if (hour < 0 && std::sscanf(token_string.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) { hour = h; minute = m; second = sec; continue; }
        if (month < 0 && token.size() >= 3) {
            for (int i = 0; i < 12; ++i) if (token.substr(0, 3) == months[i]) { month = i; break; }
            if (month >= 0) continue;
        }
        std::int64_t number;
        if (!integer(token, number)) continue;
        if (day < 0 && token.size() <= 2 && number >= 1 && number <= 31) day = static_cast<int>(number);
        else if (year < 0 && token.size() >= 2 && token.size() <= 4) year = static_cast<int>(number);
    }
    if (year >= 0 && year <= 69) year += 2000;
    else if (year >= 70 && year <= 99) year += 1900;
    if (year < 1601 || month < 0 || day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    std::tm tm{}; tm.tm_year = year - 1900; tm.tm_mon = month; tm.tm_mday = day; tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = second;
    result = static_cast<double>(timegm(&tm)); return true;
}
bool initialize_cookie(cookie_object *cookie, id properties) {
    if (!is_dictionary(properties)) return false;
    id name = property(properties, "Name"), value = property(properties, "Value");
    if (!is_string(name) || !is_string(value)) return false;
    cookie->name = to_utf8(name); cookie->value = to_utf8(value);
    cookie->domain = lower(to_utf8(property(properties, "Domain")));
    if (cookie->domain.empty()) cookie->domain = parse_url(property(properties, "OriginURL")).host;
    cookie->path = to_utf8(property(properties, "Path"));
    if (cookie->name.empty() || cookie->name.find_first_of("()<>@,;:\\\"/[]?={} \t\r\n") != std::string::npos || cookie->value.find_first_of("\r\n;") != std::string::npos || cookie->domain.empty() || cookie->path.empty() || cookie->path[0] != '/') return false;
    cookie->host_only = cookie->domain.front() != '.';
    cookie->secure = truth(property(properties, "Secure")); cookie->http_only = truth(property(properties, "HTTPOnly"));
    id version = property(properties, "Version");
    std::int64_t number = 0;
    if (is_number(version)) number = number_int64(version); else integer(to_utf8(version), number);
    cookie->version = number > 0 ? static_cast<std::uint64_t>(number) : 0;
    id expiry = property(properties, "Expires");
    bool has_expiry = expiry && is_kind(expiry, cls("NSDate"));
    if (has_expiry) {
        cookie->expires = date_to_epoch(expiry);
    } else if (is_string(expiry)) has_expiry = cookie_date(to_utf8(expiry), cookie->expires);
    id max_age = property(properties, "Max-Age");
    if (max_age && (is_number(max_age) || integer(to_utf8(max_age), number))) {
        if (is_number(max_age)) number = number_int64(max_age);
        cookie->expires = number <= 0 ? 1 : now() + static_cast<double>(number); has_expiry = true;
    }
    cookie->session = !has_expiry || truth(property(properties, "Discard"));
    cookie->has_expiry = has_expiry;
    release(cookie->properties);
    cookie->properties = obj(send(properties, "copy"));
    return true;
}
id new_cookie(id properties) {
    auto *cookie = make<cookie_object>(cookie_class);
    if (!initialize_cookie(cookie, properties)) { release(reinterpret_cast<id>(cookie)); return nullptr; }
    return reinterpret_cast<id>(cookie);
}
std::uint64_t cookie_init(id self, SEL, id properties) {
    if (!initialize_cookie(as<cookie_object>(self), properties)) { release(self); return 0; }
    return ret(self);
}
std::uint64_t cookie_create(id, SEL, id properties) { return ret(autorelease(new_cookie(properties))); }

// Split folded Set-Cookie fields without splitting the comma inside Expires.
std::vector<std::string_view> split_cookies(std::string_view header) {
    std::vector<std::string_view> result;
    std::size_t start = 0; bool quoted = false;
    for (std::size_t i = 0; i < header.size(); ++i) {
        if (header[i] == '"') quoted = !quoted;
        if (quoted || (header[i] != ',' && header[i] != '\n')) continue;
        auto rest = trim(header.substr(i + 1));
        auto equal = rest.find('='); auto delimiter = rest.find_first_of(";,\r\n");
        if (header[i] == '\n' || (equal != std::string_view::npos && (delimiter == std::string_view::npos || equal < delimiter))) {
            result.push_back(trim(header.substr(start, i - start))); start = i + 1;
        }
    }
    result.push_back(trim(header.substr(start))); return result;
}
std::uint64_t cookies_response(id, SEL, id headers, id url) {
    std::vector<id> cookies;
    auto origin = parse_url(url);
    if (is_dictionary(headers) && !origin.host.empty()) {
        std::vector<id> keys; dictionary_keys(headers, keys);
        for (id key : keys) {
            if (lower(to_utf8(key)) != "set-cookie") continue;
            id values = dictionary_get(headers, key);
            std::vector<id> fields = is_array(values) ? array_storage(values) : std::vector<id>{values};
            for (id field : fields) {
                std::string text = to_utf8(field);
                for (auto line : split_cookies(text)) {
                    auto semicolon = line.find(';'); auto pair = trim(line.substr(0, semicolon)); auto equal = pair.find('=');
                    if (equal == std::string_view::npos) continue;
                    id properties = dictionary_create(true);
                    put(properties, "Name", trim(pair.substr(0, equal))); put(properties, "Value", trim(pair.substr(equal + 1)));
                    put(properties, "Domain", origin.host);
                    auto slash = origin.path.rfind('/'); put(properties, "Path", slash == 0 || slash == std::string::npos ? "/" : std::string_view(origin.path).substr(0, slash));
                    bool valid = true;
                    while (semicolon != std::string_view::npos) {
                        line.remove_prefix(semicolon + 1); semicolon = line.find(';');
                        auto attr = trim(line.substr(0, semicolon)); auto eq = attr.find('=');
                        auto name = lower(trim(attr.substr(0, eq)));
                        auto value = eq == std::string_view::npos ? std::string_view{} : trim(attr.substr(eq + 1));
                        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
                        if (name == "domain") {
                            auto domain = lower(value);
                            while (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
                            if (domain.empty() || domain.back() == '.' || !domain_matches(origin.host, domain, false)) valid = false;
                            else put(properties, "Domain", "." + domain);
                        } else if (name == "path" && value.starts_with('/')) put(properties, "Path", value);
                        else if (name == "secure") put(properties, "Secure", "TRUE");
                        else if (name == "httponly") put(properties, "HTTPOnly", "TRUE");
                        else if (name == "expires") {
                            double expiry;
                            if (cookie_date(value, expiry)) { id date = date_from_epoch(expiry); dictionary_set(properties, constant_string("Expires"), date); release(date); }
                        } else if (name == "max-age") { std::int64_t age; if (integer(value, age)) put(properties, "Max-Age", value); }
                    }
                    if (valid) if (id cookie = new_cookie(properties)) cookies.push_back(cookie);
                    release(properties);
                }
            }
        }
    }
    id result = array_create(cookies.data(), static_cast<CFIndex>(cookies.size()), false);
    for (id cookie : cookies) release(cookie);
    return ret(autorelease(result));
}
std::uint64_t cookies_request(id, SEL, id cookies) {
    id headers = dictionary_create(true); std::string value;
    if (is_array(cookies)) for (id object : array_storage(cookies)) {
        if (!is_kind(object, cookie_class)) continue;
        auto *cookie = as<cookie_object>(object);
        if (!value.empty()) value += "; "; value += cookie->name + "=" + cookie->value;
    }
    if (!value.empty()) put(headers, "Cookie", value);
    return ret(autorelease(headers));
}
void notify(id storage, id name) {
    id center = obj(send(reinterpret_cast<id>(cls("NSNotificationCenter")), "defaultCenter"));
    send(center, "postNotificationName:object:", ret(name), ret(storage));
}
bool expired(const cookie_object *cookie);
bool store_cookie(storage_object *storage, id cookie);
std::string storage_name(std::string_view name) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') out += static_cast<char>(c);
        else { out += '_'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
id persistent_storage(std::string_view name) {
    auto *storage = make<storage_object>(storage_class);
    std::filesystem::path directory;
    if (const char *override = mm_host_getenv(api(), "MACHOMAN_COOKIE_STORAGE_DIR"); override && override[0]) directory = override;
    else if (const char *venv = mm_host_venv(api()); venv && venv[0]) {
        // Removable/FUSE venv filesystems may ignore Unix permissions. Keep
        // authentication cookies in the user's private data directory, with
        // a separate profile for each canonical venv.
        const char *home = mm_host_getenv(api(), "HOME");
        if (home && home[0]) {
            std::error_code error;
            auto root = std::filesystem::weakly_canonical(venv, error);
            const std::string identity = error ? std::string(venv) : root.string();
            std::uint64_t hash = 14695981039346656037ull;
            for (unsigned char c : identity) { hash ^= c; hash *= 1099511628211ull; }
            directory = std::filesystem::path(home) / ".local/share/machoman/cookies" / std::to_string(hash);
        }
    }
    if (directory.empty()) return reinterpret_cast<id>(storage);
    storage->path = (directory / (storage_name(name) + ".plist")).string();
    if (FILE *file = std::fopen(storage->path.c_str(), "rb")) {
        std::string bytes; char buffer[4096];
        for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, file));) {
            bytes.append(buffer, n);
            if (bytes.size() > 16 * 1024 * 1024) { bytes.clear(); break; }
        }
        std::fclose(file);
        id parsed = plist_parse(bytes, nullptr);
        if (is_array(parsed)) for (id saved : array_storage(parsed)) {
            if (!is_dictionary(saved)) continue;
            id expiry = property(saved, "Expires");
            if (!is_number(expiry) || !std::isfinite(number_double(expiry))) continue;
            id properties = obj(send(saved, "mutableCopy"));
            id date = date_from_epoch(number_double(expiry));
            dictionary_set(properties, constant_string("Expires"), date); release(date);
            if (id cookie = new_cookie(properties)) {
                if (!as<cookie_object>(cookie)->session) store_cookie(storage, cookie);
                release(cookie);
            }
            release(properties);
        }
        release(parsed);
    }
    return reinterpret_cast<id>(storage);
}
void persist(storage_object *storage) {
    if (storage->path.empty()) return;
    std::vector<id> entries;
    for (id cookie : storage->cookies) {
        const auto *c = as<cookie_object>(cookie);
        if (c->session || expired(c) || !std::isfinite(c->expires)) continue;
        id properties = dictionary_create(true);
        std::vector<id> keys; dictionary_keys(c->properties, keys);
        for (id key : keys) {
            id value = dictionary_get(c->properties, key);
            if (is_string(value) || is_number(value)) dictionary_set(properties, key, value);
        }
        put(properties,"Name",c->name); put(properties,"Value",c->value);
        put(properties,"Domain",c->domain); put(properties,"Path",c->path);
        // Max-Age is relative to receipt. Save its absolute expiry so loading
        // the jar never extends a login's lifetime.
        dictionary_remove(properties, constant_string("Max-Age"));
        // NSDate is not a supported plist value in this layer yet. Keep the
        // absolute epoch as a number on disk and restore NSDate when loading.
        id epoch = number_from_double(c->expires);
        dictionary_set(properties, constant_string("Expires"), epoch); release(epoch);
        entries.push_back(properties);
    }
    id array = array_create(entries.data(), static_cast<CFIndex>(entries.size()), false);
    for (id entry : entries) release(entry);
    std::string bytes; bool written = plist_write_xml(array, bytes); release(array);
    if (!written) return;
    std::error_code error;
    const auto directory = std::filesystem::path(storage->path).parent_path();
    std::filesystem::create_directories(directory, error);
    if (!error) std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
    if (error) { std::fprintf(stderr, "cookies: cannot create persistent storage: %s\n", error.message().c_str()); return; }
    std::string temporary = storage->path + ".tmp-XXXXXX";
    int fd = ::mkstemp(temporary.data());
    if (fd < 0) { std::fprintf(stderr, "cookies: cannot create persistent cookie file\n"); return; }
    FILE *file = ::fdopen(fd, "wb");
    if (file) {
        written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fflush(file) == 0 && ::fsync(fd) == 0;
        if (std::fclose(file) != 0) written = false;
    } else { ::close(fd); written = false; }
    if (!written || std::rename(temporary.c_str(), storage->path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        std::fprintf(stderr, "cookies: cannot save persistent cookie file\n");
    }
}
std::uint64_t shared_storage(id, SEL) {
    static id storage = [] {
        id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
        std::string name = to_utf8(obj(send(bundle, "bundleIdentifier")));
        return persistent_storage("app." + (name.empty() ? std::string("default") : name));
    }();
    return ret(storage);
}
std::uint64_t group_storage(id, SEL, id group) {
    static std::mutex mutex; static std::unordered_map<std::string, id> stores;
    std::lock_guard lock(mutex);
    auto &storage = stores[to_utf8(group)];
    if (!storage) storage = persistent_storage("group." + to_utf8(group));
    return ret(storage);
}
bool expired(const cookie_object *cookie) { return cookie->has_expiry && cookie->expires <= now(); }
void prune(storage_object *storage) {
    std::erase_if(storage->cookies, [](id cookie) { if (!expired(as<cookie_object>(cookie))) return false; release(cookie); return true; });
}
std::uint64_t storage_cookies(id self, SEL) {
    auto *storage = as<storage_object>(self); std::lock_guard lock(storage->mutex); prune(storage);
    return ret(autorelease(array_create(storage->cookies.data(), static_cast<CFIndex>(storage->cookies.size()), false)));
}
std::uint64_t storage_for_url(id self, SEL, id url) {
    auto origin = parse_url(url); auto *storage = as<storage_object>(self);
    std::lock_guard lock(storage->mutex); prune(storage);
    std::vector<id> result;
    for (id cookie : storage->cookies) {
        auto *c = as<cookie_object>(cookie);
        if (!domain_matches(origin.host, c->domain, c->host_only) || (c->secure && !origin.secure)) continue;
        if (origin.path != c->path && !(origin.path.starts_with(c->path) && (c->path.back() == '/' || origin.path[c->path.size()] == '/'))) continue;
        result.push_back(cookie);
    }
    std::stable_sort(result.begin(), result.end(), [](id a, id b) { return as<cookie_object>(a)->path.size() > as<cookie_object>(b)->path.size(); });
    return ret(autorelease(array_create(result.data(), static_cast<CFIndex>(result.size()), false)));
}
bool store_cookie(storage_object *storage, id cookie) {
    if (!is_kind(cookie, cookie_class)) return false;
    retain(cookie);
    auto *c = as<cookie_object>(cookie);
    std::erase_if(storage->cookies, [&](id old) {
        auto *o = as<cookie_object>(old);
        if (o->name != c->name || o->domain != c->domain || o->path != c->path) return false;
        release(old); return true;
    });
    if (!expired(c)) storage->cookies.push_back(retain(cookie));
    if (std::getenv("MACHOMAN_COOKIE_TRACE")) std::fprintf(stderr, "cookies: %s name=%s domain=%s path=%s secure=%d session=%d\n", expired(c) ? "expired" : "stored", c->name.c_str(), c->domain.c_str(), c->path.c_str(), c->secure, c->session);
    release(cookie);
    return true;
}
std::uint64_t storage_set(id self, SEL, id cookie) {
    bool changed; auto *storage = as<storage_object>(self);
    { std::lock_guard lock(storage->mutex); changed = storage->policy != 1 && store_cookie(storage, cookie); if (changed) persist(storage); }
    if (changed) notify(self, NSHTTPCookieManagerCookiesChangedNotification_);
    return 0;
}
std::uint64_t storage_set_many(id self, SEL, id cookies, id url, id main_url) {
    auto origin = parse_url(url), main = parse_url(main_url); bool changed = false;
    auto *storage = as<storage_object>(self);
    {
        std::lock_guard lock(storage->mutex);
        if (storage->policy == 1 || !is_array(cookies)) return 0;
        for (id cookie : array_storage(cookies)) {
            if (!is_kind(cookie, cookie_class)) continue;
            auto *c = as<cookie_object>(cookie);
            if (!domain_matches(origin.host, c->domain, c->host_only) || (storage->policy == 2 && !domain_matches(main.host, c->domain, false))) continue;
            changed |= store_cookie(storage, cookie);
        }
        if (changed) persist(storage);
    }
    if (changed) notify(self, NSHTTPCookieManagerCookiesChangedNotification_);
    return 0;
}
std::uint64_t storage_delete(id self, SEL, id cookie) {
    if (!is_kind(cookie, cookie_class)) return 0;
    auto *c = as<cookie_object>(cookie);
    const auto name = c->name, domain = c->domain, path = c->path;
    auto *storage = as<storage_object>(self); bool changed;
    {
        std::lock_guard lock(storage->mutex);
        changed = std::erase_if(storage->cookies, [&](id old) {
            auto *o = as<cookie_object>(old);
            if (o->name != name || o->domain != domain || o->path != path) return false;
            release(old); return true;
        }) != 0;
        if (changed) persist(storage);
    }
    if (changed) notify(self, NSHTTPCookieManagerCookiesChangedNotification_);
    return 0;
}
std::uint64_t storage_policy(id self, SEL) { auto *s = as<storage_object>(self); std::lock_guard lock(s->mutex); return s->policy; }
std::uint64_t storage_set_policy(id self, SEL, std::uint64_t policy) {
    if (policy > 2) return 0; bool changed; auto *s = as<storage_object>(self);
    { std::lock_guard lock(s->mutex); changed = s->policy != policy; s->policy = policy; }
    if (changed) notify(self, NSHTTPCookieManagerAcceptPolicyChangedNotification_); return 0;
}
} // namespace

void register_cookies() {
#define KEY(name, value) name##_ = constant_string(value)
    KEY(NSHTTPCookieName, "Name"); KEY(NSHTTPCookieValue, "Value"); KEY(NSHTTPCookieDomain, "Domain"); KEY(NSHTTPCookiePath, "Path");
    KEY(NSHTTPCookieVersion, "Version"); KEY(NSHTTPCookieExpires, "Expires"); KEY(NSHTTPCookieSecure, "Secure"); KEY(NSHTTPCookieDiscard, "Discard");
    KEY(NSHTTPCookieMaximumAge, "Max-Age"); KEY(NSHTTPCookieOriginURL, "OriginURL"); KEY(NSHTTPCookiePort, "Port");
    KEY(NSHTTPCookieComment, "Comment"); KEY(NSHTTPCookieCommentURL, "CommentURL");
    KEY(NSHTTPCookieManagerCookiesChangedNotification, "NSHTTPCookieManagerCookiesChangedNotification");
    KEY(NSHTTPCookieManagerAcceptPolicyChangedNotification, "NSHTTPCookieManagerAcceptPolicyChangedNotification");
#undef KEY
    cookie_class = mm_objc_define_class("NSHTTPCookie", cls("NSObject"), sizeof(cookie_object), FND_CLASS_REF(NSHTTPCookie), FND_META_REF(NSHTTPCookie));
    register_lifecycle<cookie_object>(cookie_class);
    FND_CLASS_METHODS(cookie_class,
        {"cookieWithProperties:", IMP(cookie_create), "@@:@"},
        {"cookiesWithResponseHeaderFields:forURL:", IMP(cookies_response), "@@:@@"},
        {"requestHeaderFieldsWithCookies:", IMP(cookies_request), "@@:@"});
    FND_METHODS(cookie_class,
        {"initWithProperties:", IMP(cookie_init), "@@:@"},
        {"properties", IMP([](id s, SEL) { return ret(as<cookie_object>(s)->properties); }), "@@:"},
        {"name", IMP([](id s, SEL) { return ret(autorelease(string_from_utf8(as<cookie_object>(s)->name))); }), "@@:"},
        {"value", IMP([](id s, SEL) { return ret(autorelease(string_from_utf8(as<cookie_object>(s)->value))); }), "@@:"},
        {"domain", IMP([](id s, SEL) { return ret(autorelease(string_from_utf8(as<cookie_object>(s)->domain))); }), "@@:"},
        {"path", IMP([](id s, SEL) { return ret(autorelease(string_from_utf8(as<cookie_object>(s)->path))); }), "@@:"},
        {"version", IMP([](id s, SEL) { return as<cookie_object>(s)->version; }), "Q@:"},
        {"isSecure", IMP([](id s, SEL) -> std::uint64_t { return as<cookie_object>(s)->secure; }), "B@:"},
        {"isHTTPOnly", IMP([](id s, SEL) -> std::uint64_t { return as<cookie_object>(s)->http_only; }), "B@:"},
        {"isSessionOnly", IMP([](id s, SEL) -> std::uint64_t { return as<cookie_object>(s)->session; }), "B@:"},
        {"expiresDate", IMP([](id s, SEL) { auto *c = as<cookie_object>(s); return c->has_expiry ? ret(autorelease(date_from_epoch(c->expires))) : 0; }), "@@:"},
        {"copyWithZone:", IMP([](id s, SEL, void *) { return ret(retain(s)); }), "@@:^v"});
    storage_class = mm_objc_define_class("NSHTTPCookieStorage", cls("NSObject"), sizeof(storage_object), FND_CLASS_REF(NSHTTPCookieStorage), FND_META_REF(NSHTTPCookieStorage));
    register_lifecycle<storage_object>(storage_class);
    FND_CLASS_METHODS(storage_class,
        {"sharedHTTPCookieStorage", IMP(shared_storage), "@@:"},
        {"sharedCookieStorageForGroupContainerIdentifier:", IMP(group_storage), "@@:@"});
    FND_METHODS(storage_class,
        {"cookies", IMP(storage_cookies), "@@:"}, {"cookiesForURL:", IMP(storage_for_url), "@@:@"},
        {"setCookie:", IMP(storage_set), "v@:@"}, {"deleteCookie:", IMP(storage_delete), "v@:@"},
        {"setCookies:forURL:mainDocumentURL:", IMP(storage_set_many), "v@:@@@"},
        {"cookieAcceptPolicy", IMP(storage_policy), "Q@:"}, {"setCookieAcceptPolicy:", IMP(storage_set_policy), "v@:Q"});
}
} // namespace fnd
