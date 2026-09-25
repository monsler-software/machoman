/* The rest of CoreFoundation/Foundation: the CFType base functions,
 * allocators, NSURL/CFURL, NSError/CFError, NSBundle/CFBundle,
 * CFPreferences/NSUserDefaults, NSProcessInfo, NSFileManager, NSLocale,
 * NSUUID, the run loop, geometry helpers and the well-known constant
 * strings the guest binds to by symbol. */
#include "foundation.h"
#include "synthesise.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
void *objc_autoreleasePoolPush_() __asm__("_objc_autoreleasePoolPush");
void objc_autoreleasePoolPop_(void *) __asm__("_objc_autoreleasePoolPop");
}

namespace fnd {

/* Standard directories for a NSSearchPathDirectory constant; defined with
 * the C API at the end of this file. */
id search_path_directories(std::uint64_t directory);

/* Class storage the guest binds to: external linkage, so not inside the
 * anonymous namespace below. */
FND_CLASS(NSURL); FND_CLASS(NSError); FND_CLASS(NSBundle); FND_CLASS(NSUserDefaults); FND_CLASS(NSProcessInfo);
FND_CLASS(NSFileManager); FND_CLASS(NSLocale); FND_CLASS(NSUUID); FND_CLASS(NSRunLoop); FND_CLASS(NSDate);
FND_CLASS(NSAppleEventManager);
FND_CLASS(NSNotificationCenter); FND_CLASS(NSDistributedNotificationCenter); FND_CLASS(NSNotification); FND_CLASS(NSException);
FND_CLASS(NSAutoreleasePool); FND_CLASS(NSThread); FND_CLASS(NSTimer); FND_CLASS(NSOperationQueue); FND_CLASS(NSCharacterSet);

namespace {

const mm_host_api *g_api = nullptr;
bool g_trace = false;


Class g_url, g_error, g_bundle, g_defaults, g_processinfo, g_filemanager, g_locale, g_uuid, g_runloop, g_date,
    g_apple_event_manager, g_notification_center, g_distributed_center, g_notification, g_exception, g_pool, g_thread, g_timer, g_opqueue, g_charset;

/* ---- allocators ---------------------------------------------------------- */

/* Allocators are opaque to us: every CF object is an objc object with the
 * runtime's reference counting, so all allocator arguments are ignored.
 * They must still be distinct, non-null pointers the guest can compare. */
struct allocator { mm_objc_object base; };
extern "C" {
extern EXPORT allocator kCFAllocatorSystemDefault_storage __asm__("___kCFAllocatorSystemDefault");
extern EXPORT allocator kCFAllocatorMalloc_storage __asm__("___kCFAllocatorMalloc");
extern EXPORT allocator kCFAllocatorNull_storage __asm__("___kCFAllocatorNull");
extern EXPORT void *kCFAllocatorDefault_ptr __asm__("_kCFAllocatorDefault");
extern EXPORT void *kCFAllocatorSystemDefault_ptr __asm__("_kCFAllocatorSystemDefault");
extern EXPORT void *kCFAllocatorMalloc_ptr __asm__("_kCFAllocatorMalloc");
extern EXPORT void *kCFAllocatorMallocZone_ptr __asm__("_kCFAllocatorMallocZone");
extern EXPORT void *kCFAllocatorNull_ptr __asm__("_kCFAllocatorNull");
extern EXPORT void *kCFAllocatorUseContext_ptr __asm__("_kCFAllocatorUseContext");
}
allocator kCFAllocatorSystemDefault_storage, kCFAllocatorMalloc_storage, kCFAllocatorNull_storage;
void *kCFAllocatorDefault_ptr, *kCFAllocatorSystemDefault_ptr, *kCFAllocatorMalloc_ptr, *kCFAllocatorMallocZone_ptr,
    *kCFAllocatorNull_ptr, *kCFAllocatorUseContext_ptr;

/* ---- well-known constant strings ------------------------------------------ */

/* Each is a CFString the guest dereferences directly, so they are real
 * objects created at registration time. */
struct named_string { const char *symbol_value; id *slot; };
extern "C" {
extern EXPORT id kCFRunLoopDefaultMode_ __asm__("_kCFRunLoopDefaultMode");
extern EXPORT id kCFRunLoopCommonModes_ __asm__("_kCFRunLoopCommonModes");
extern EXPORT id NSDefaultRunLoopMode_ __asm__("_NSDefaultRunLoopMode");
extern EXPORT id NSRunLoopCommonModes_ __asm__("_NSRunLoopCommonModes");
extern EXPORT id kCFErrorDomainPOSIX_ __asm__("_kCFErrorDomainPOSIX");
extern EXPORT id kCFErrorDomainOSStatus_ __asm__("_kCFErrorDomainOSStatus");
extern EXPORT id kCFErrorDomainMach_ __asm__("_kCFErrorDomainMach");
extern EXPORT id kCFErrorDomainCocoa_ __asm__("_kCFErrorDomainCocoa");
extern EXPORT id kCFErrorLocalizedDescriptionKey_ __asm__("_kCFErrorLocalizedDescriptionKey");
extern EXPORT id NSPOSIXErrorDomain_ __asm__("_NSPOSIXErrorDomain");
extern EXPORT id NSCocoaErrorDomain_ __asm__("_NSCocoaErrorDomain");
extern EXPORT id NSOSStatusErrorDomain_ __asm__("_NSOSStatusErrorDomain");
extern EXPORT id NSURLErrorDomain_ __asm__("_NSURLErrorDomain");
extern EXPORT id NSLocalizedDescriptionKey_ __asm__("_NSLocalizedDescriptionKey");
extern EXPORT id NSUnderlyingErrorKey_ __asm__("_NSUnderlyingErrorKey");
extern EXPORT id kCFPreferencesAnyApplication_ __asm__("_kCFPreferencesAnyApplication");
extern EXPORT id kCFPreferencesCurrentApplication_ __asm__("_kCFPreferencesCurrentApplication");
extern EXPORT id kCFPreferencesAnyHost_ __asm__("_kCFPreferencesAnyHost");
extern EXPORT id kCFPreferencesCurrentHost_ __asm__("_kCFPreferencesCurrentHost");
extern EXPORT id kCFPreferencesAnyUser_ __asm__("_kCFPreferencesAnyUser");
extern EXPORT id kCFPreferencesCurrentUser_ __asm__("_kCFPreferencesCurrentUser");
extern EXPORT id kCFURLIsHiddenKey_ __asm__("_kCFURLIsHiddenKey");
extern EXPORT id kCFURLIsPackageKey_ __asm__("_kCFURLIsPackageKey");
extern EXPORT id kCFURLIsDirectoryKey_ __asm__("_kCFURLIsDirectoryKey");
extern EXPORT id kCFURLVolumeIdentifierKey_ __asm__("_kCFURLVolumeIdentifierKey");
extern EXPORT id kCFURLNameKey_ __asm__("_kCFURLNameKey");
extern EXPORT id kCFURLPathKey_ __asm__("_kCFURLPathKey");
extern EXPORT id NSURLVolumeIsLocalKey_ __asm__("_NSURLVolumeIsLocalKey");
extern EXPORT id NSURLIsDirectoryKey_ __asm__("_NSURLIsDirectoryKey");
extern EXPORT id NSFileHFSCreatorCode_ __asm__("_NSFileHFSCreatorCode");
extern EXPORT id NSFileHFSTypeCode_ __asm__("_NSFileHFSTypeCode");
extern EXPORT id NSFileImmutable_ __asm__("_NSFileImmutable");
extern EXPORT id NSFileSize_ __asm__("_NSFileSize");
extern EXPORT id NSFileType_ __asm__("_NSFileType");
extern EXPORT id NSFileTypeDirectory_ __asm__("_NSFileTypeDirectory");
extern EXPORT id NSFileTypeRegular_ __asm__("_NSFileTypeRegular");
extern EXPORT id NSKeyValueChangeNewKey_ __asm__("_NSKeyValueChangeNewKey");
extern EXPORT id NSKeyValueChangeOldKey_ __asm__("_NSKeyValueChangeOldKey");
extern EXPORT id kCFNumberFormatterMinFractionDigits_ __asm__("_kCFNumberFormatterMinFractionDigits");
extern EXPORT id kCFNumberFormatterMaxFractionDigits_ __asm__("_kCFNumberFormatterMaxFractionDigits");
extern EXPORT id kCFLocaleIdentifier_ __asm__("_kCFLocaleIdentifier");
extern EXPORT id kCFLocaleDecimalSeparator_ __asm__("_kCFLocaleDecimalSeparator");
}
id kCFRunLoopDefaultMode_, kCFRunLoopCommonModes_, NSDefaultRunLoopMode_, NSRunLoopCommonModes_, kCFErrorDomainPOSIX_,
    kCFErrorDomainOSStatus_, kCFErrorDomainMach_, kCFErrorDomainCocoa_, kCFErrorLocalizedDescriptionKey_, NSPOSIXErrorDomain_,
    NSCocoaErrorDomain_, NSOSStatusErrorDomain_, NSURLErrorDomain_, NSLocalizedDescriptionKey_, NSUnderlyingErrorKey_,
    kCFPreferencesAnyApplication_, kCFPreferencesCurrentApplication_, kCFPreferencesAnyHost_, kCFPreferencesCurrentHost_,
    kCFPreferencesAnyUser_, kCFPreferencesCurrentUser_, kCFURLIsHiddenKey_, kCFURLIsPackageKey_, kCFURLIsDirectoryKey_,
    kCFURLVolumeIdentifierKey_, kCFURLNameKey_, kCFURLPathKey_, NSURLVolumeIsLocalKey_, NSURLIsDirectoryKey_,
    NSFileHFSCreatorCode_, NSFileHFSTypeCode_, NSFileImmutable_, NSFileSize_, NSFileType_, NSFileTypeDirectory_,
    NSFileTypeRegular_, NSKeyValueChangeNewKey_, NSKeyValueChangeOldKey_, kCFNumberFormatterMinFractionDigits_,
    kCFNumberFormatterMaxFractionDigits_, kCFLocaleIdentifier_, kCFLocaleDecimalSeparator_;

void init_constant_strings() {
    const named_string table[] = {
        {"kCFRunLoopDefaultMode", &kCFRunLoopDefaultMode_}, {"kCFRunLoopCommonModes", &kCFRunLoopCommonModes_},
        {"kCFRunLoopDefaultMode", &NSDefaultRunLoopMode_}, {"kCFRunLoopCommonModes", &NSRunLoopCommonModes_},
        {"NSPOSIXErrorDomain", &kCFErrorDomainPOSIX_}, {"NSOSStatusErrorDomain", &kCFErrorDomainOSStatus_},
        {"NSMachErrorDomain", &kCFErrorDomainMach_}, {"NSCocoaErrorDomain", &kCFErrorDomainCocoa_},
        {"NSLocalizedDescription", &kCFErrorLocalizedDescriptionKey_}, {"NSPOSIXErrorDomain", &NSPOSIXErrorDomain_},
        {"NSCocoaErrorDomain", &NSCocoaErrorDomain_}, {"NSOSStatusErrorDomain", &NSOSStatusErrorDomain_},
        {"NSURLErrorDomain", &NSURLErrorDomain_}, {"NSLocalizedDescription", &NSLocalizedDescriptionKey_},
        {"NSUnderlyingError", &NSUnderlyingErrorKey_}, {"kCFPreferencesAnyApplication", &kCFPreferencesAnyApplication_},
        {"kCFPreferencesCurrentApplication", &kCFPreferencesCurrentApplication_}, {"kCFPreferencesAnyHost", &kCFPreferencesAnyHost_},
        {"kCFPreferencesCurrentHost", &kCFPreferencesCurrentHost_}, {"kCFPreferencesAnyUser", &kCFPreferencesAnyUser_},
        {"kCFPreferencesCurrentUser", &kCFPreferencesCurrentUser_}, {"NSURLIsHiddenKey", &kCFURLIsHiddenKey_},
        {"NSURLIsPackageKey", &kCFURLIsPackageKey_}, {"NSURLIsDirectoryKey", &kCFURLIsDirectoryKey_},
        {"NSURLVolumeIdentifierKey", &kCFURLVolumeIdentifierKey_}, {"NSURLNameKey", &kCFURLNameKey_},
        {"NSURLPathKey", &kCFURLPathKey_}, {"NSURLVolumeIsLocalKey", &NSURLVolumeIsLocalKey_},
        {"NSURLIsDirectoryKey", &NSURLIsDirectoryKey_}, {"NSFileHFSCreatorCode", &NSFileHFSCreatorCode_},
        {"NSFileHFSTypeCode", &NSFileHFSTypeCode_}, {"NSFileImmutable", &NSFileImmutable_}, {"NSFileSize", &NSFileSize_},
        {"NSFileType", &NSFileType_}, {"NSFileTypeDirectory", &NSFileTypeDirectory_}, {"NSFileTypeRegular", &NSFileTypeRegular_},
        {"new", &NSKeyValueChangeNewKey_}, {"old", &NSKeyValueChangeOldKey_},
        {"kCFNumberFormatterMinFractionDigits", &kCFNumberFormatterMinFractionDigits_},
        {"kCFNumberFormatterMaxFractionDigits", &kCFNumberFormatterMaxFractionDigits_},
        {"kCFLocaleIdentifier", &kCFLocaleIdentifier_}, {"kCFLocaleDecimalSeparator", &kCFLocaleDecimalSeparator_},
    };
    for (const auto &e : table) *e.slot = constant_string(e.symbol_value);
}

/* ---- NSURL ---------------------------------------------------------------- */

/* Only file:// URLs matter to Live; the object keeps the absolute path and
 * whether it denotes a directory. */
struct url_object { mm_objc_object base; std::string path; std::string scheme{"file"}; bool is_dir = false; };

url_object *u(id o) { return as<url_object>(o); }

std::string percent_decode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}
std::string percent_encode(std::string_view s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || std::strchr("-_.~/", c)) out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::uint64_t url_path_m(id self, SEL) { return ret(autorelease(string_from_utf8(u(self)->path))); }
std::uint64_t url_absoluteString(id self, SEL) { return ret(autorelease(string_from_utf8(u(self)->scheme + "://" + percent_encode(u(self)->path)))); }
std::uint64_t url_isFileURL(id self, SEL) { return u(self)->scheme == "file"; }
std::uint64_t url_lastPathComponent(id self, SEL) {
    std::string_view p = u(self)->path;
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const auto slash = p.rfind('/');
    return ret(autorelease(string_from_utf8(slash == std::string_view::npos ? p : p.substr(slash + 1))));
}
std::uint64_t url_pathExtension(id self, SEL) {
    const std::string_view p = u(self)->path;
    const auto slash = p.rfind('/'), dot = p.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) || dot + 1 >= p.size()) return ret(constant_string(""));
    return ret(autorelease(string_from_utf8(p.substr(dot + 1))));
}
id url_new(std::string path, bool is_dir) {
    auto *o = make<url_object>(g_url);
    o->path = std::move(path);
    o->is_dir = is_dir;
    return reinterpret_cast<id>(o);
}
std::uint64_t url_URLByDeletingLastPathComponent(id self, SEL) {
    std::string p = u(self)->path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const auto slash = p.rfind('/');
    return ret(autorelease(url_new(slash == std::string::npos ? "" : slash == 0 ? "/" : p.substr(0, slash), true)));
}
std::uint64_t url_URLByAppendingPathComponent(id self, SEL, id comp) {
    std::string p = u(self)->path;
    if (!p.empty() && p.back() != '/') p += '/';
    p += to_utf8(comp);
    return ret(autorelease(url_new(std::move(p), false)));
}
std::uint64_t url_initFileURLWithPath(id self, SEL, id path) { u(self)->path = to_utf8(path); return ret(self); }
std::uint64_t url_initFileURLWithPathIsDirectory(id self, SEL, id path, std::uint64_t dir) { u(self)->path = to_utf8(path); u(self)->is_dir = dir != 0; return ret(self); }
std::uint64_t url_initWithString(id self, SEL, id str) {
    std::string s = to_utf8(str);
    const auto sep = s.find("://");
    if (sep != std::string::npos) { u(self)->scheme = s.substr(0, sep); s = s.substr(sep + 3); }
    u(self)->path = percent_decode(s);
    return ret(self);
}
std::uint64_t url_description(id self, SEL) { return url_absoluteString(self, nullptr); }
std::uint64_t url_isEqual(id self, SEL, id o) { return o && is_url(o) && u(self)->path == u(o)->path; }
std::uint64_t url_hash(id self, SEL) { std::uint64_t h = 1469598103934665603ull; for (unsigned char c : u(self)->path) { h ^= c; h *= 1099511628211ull; } return h; }
std::uint64_t url_copyWithZone(id self, SEL, void *) { return ret(retain(self)); }
std::uint64_t url_scheme(id self, SEL) { return ret(autorelease(string_from_utf8(u(self)->scheme))); }
std::uint64_t urlc_fileURLWithPath(id, SEL, id path) { return ret(autorelease(url_new(to_utf8(path), false))); }

/* Bookmarks: opaque data that names a file across moves and renames. What
 * makes them work on macOS is the file's inode, which a program never sees
 * — it only archives the data and resolves it later. Here the data is the
 * path, tagged so a foreign bookmark is recognised as one this build did
 * not write. Nothing else can be honoured without the volume's catalogue. */
constexpr std::string_view bookmark_tag = "machoman-bookmark\0";

id bookmark_data_for(const std::string &path) {
    std::string bytes(bookmark_tag);
    bytes += path;
    return data_create(bytes.data(), static_cast<CFIndex>(bytes.size()), false);
}
bool bookmark_path(id data, std::string &out) {
    if (!is_data(data)) return false;
    const std::string &bytes = data_storage(data);
    if (bytes.size() < bookmark_tag.size() || bytes.compare(0, bookmark_tag.size(), bookmark_tag) != 0) return false;
    out = bytes.substr(bookmark_tag.size());
    return true;
}
std::uint64_t url_bookmarkDataWithOptions(id self, SEL, std::uint64_t, id, id, id *error) {
    if (error) *error = nullptr;
    return ret(autorelease(bookmark_data_for(u(self)->path)));
}
std::uint64_t urlc_bookmarkDataWithContentsOfURL(id, SEL, id url, id *error) {
    if (error) *error = nullptr;
    if (!is_url(url)) return 0;
    return ret(autorelease(bookmark_data_for(u(url)->path)));
}
std::uint64_t urlc_URLByResolvingBookmarkData(id, SEL, id data, std::uint64_t, id, std::uint64_t *stale, id *error) {
    std::string path;
    if (stale) *stale = 0;
    if (!bookmark_path(data, path)) {
        if (error)
            *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 4 /* NSFileNoSuchFileError */, nullptr));
        return 0;
    }
    if (error) *error = nullptr;
    return ret(autorelease(url_new(path, false)));
}
std::uint64_t url_initByResolvingBookmarkData(id self, SEL, id data, std::uint64_t, id, std::uint64_t *stale, id *error) {
    std::string path;
    if (stale) *stale = 0;
    if (!bookmark_path(data, path)) {
        if (error)
            *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 4, nullptr));
        return 0;
    }
    if (error) *error = nullptr;
    u(self)->path = path;
    u(self)->scheme = "file";
    return ret(self);
}
std::uint64_t urlc_resourceValuesForKeys(id, SEL, id, id data) {
    /* The values a bookmark carries: only the path is recorded here. */
    std::string path;
    if (!bookmark_path(data, path)) return 0;
    id d = dictionary_create(false);
    return ret(autorelease(d));
}
std::uint64_t urlc_writeBookmarkData(id, SEL, id, id, std::uint64_t, id *error) {
    if (error) *error = nullptr;
    return 0; /* no alias files: nothing reads them back */
}
std::uint64_t urlc_fileURLWithPathIsDirectory(id, SEL, id path, std::uint64_t dir) { return ret(autorelease(url_new(to_utf8(path), dir != 0))); }
std::uint64_t urlc_URLWithString(id c, SEL, id s) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithString:", ret(s))))); }

/* ---- NSError --------------------------------------------------------------- */

struct error_object { mm_objc_object base; id domain = nullptr; std::int64_t code = 0; id user_info = nullptr; };
error_object *err(id o) { return as<error_object>(o); }
std::uint64_t err_domain(id self, SEL) { return ret(err(self)->domain); }
std::uint64_t err_code(id self, SEL) { return static_cast<std::uint64_t>(err(self)->code); }
std::uint64_t err_userInfo(id self, SEL) { return ret(err(self)->user_info ? err(self)->user_info : obj(send(reinterpret_cast<id>(cls("NSDictionary")), "dictionary"))); }
std::uint64_t err_localizedDescription(id self, SEL) {
    if (err(self)->user_info)
        if (id d = dictionary_get(err(self)->user_info, NSLocalizedDescriptionKey_)) return ret(d);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s error %lld.", describe(err(self)->domain).c_str(), static_cast<long long>(err(self)->code));
    return ret(autorelease(string_from_utf8(buf)));
}
std::uint64_t err_description(id self, SEL) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "Error Domain=%s Code=%lld", describe(err(self)->domain).c_str(), static_cast<long long>(err(self)->code));
    return ret(autorelease(string_from_utf8(buf)));
}
std::uint64_t err_initWithDomainCodeUserInfo(id self, SEL, id domain, std::uint64_t code, id info) {
    err(self)->domain = retain(domain);
    err(self)->code = static_cast<std::int64_t>(code);
    err(self)->user_info = info ? retain(info) : nullptr;
    return ret(self);
}
std::uint64_t errc_errorWithDomainCodeUserInfo(id c, SEL, id domain, std::uint64_t code, id info) {
    return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithDomain:code:userInfo:", ret(domain), code, ret(info)))));
}
struct error_dtor : error_object { ~error_dtor() { release(domain); release(user_info); } };

/* ---- NSBundle / CFBundle ---------------------------------------------------- */

struct bundle_object { mm_objc_object base; std::string path; id info = nullptr; };
bundle_object *bun(id o) { return as<bundle_object>(o); }

std::string executable_path() {
    const mm_host_api *a = api();
    return a && a->image_path ? a->image_path(0) : "";
}

/* "<...>/Foo.app/Contents/MacOS/Foo" -> "<...>/Foo.app". */
std::string bundle_path_of(std::string_view exe) {
    const std::string_view marker = "/Contents/MacOS/";
    const auto at = exe.rfind(marker);
    if (at == std::string_view::npos) return {};
    return std::string(exe.substr(0, at));
}

id load_info_plist(const std::string &bundle) {
    const std::string path = bundle + "/Contents/Info.plist";
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    std::string bytes;
    char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.append(buf, n);
    std::fclose(f);
    std::string error;
    id plist = plist_parse(bytes, &error);
    if (!plist && g_trace) std::fprintf(stderr, "Foundation: %s: %s\n", path.c_str(), error.c_str());
    if (plist && !is_dictionary(plist)) { release(plist); return nullptr; }
    return plist;
}

id make_bundle(const std::string &path) {
    auto *b = make<bundle_object>(g_bundle);
    b->path = path;
    b->info = load_info_plist(path);
    return reinterpret_cast<id>(b);
}

id main_bundle() {
    static id b = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string exe = executable_path();
        std::string p = bundle_path_of(exe);
        if (p.empty()) { const auto slash = exe.rfind('/'); p = slash == std::string::npos ? "." : exe.substr(0, slash); }
        b = make_bundle(p);
    });
    return b;
}

std::uint64_t bundle_bundlePath(id self, SEL) { return ret(autorelease(string_from_utf8(bun(self)->path))); }
std::uint64_t bundle_bundleURL(id self, SEL) { return ret(autorelease(url_new(bun(self)->path, true))); }
std::uint64_t bundle_resourcePath(id self, SEL) { return ret(autorelease(string_from_utf8(bun(self)->path + "/Contents/Resources"))); }
std::uint64_t bundle_resourceURL(id self, SEL) { return ret(autorelease(url_new(bun(self)->path + "/Contents/Resources", true))); }
std::uint64_t bundle_infoDictionary(id self, SEL) { return ret(bun(self)->info); }
std::uint64_t bundle_objectForInfoDictionaryKey(id self, SEL, id key) { return bun(self)->info ? ret(dictionary_get(bun(self)->info, key)) : 0; }
std::uint64_t bundle_bundleIdentifier(id self, SEL) { return bun(self)->info ? ret(dictionary_get(bun(self)->info, constant_string("CFBundleIdentifier"))) : 0; }
std::uint64_t bundle_executablePath(id, SEL) { return ret(autorelease(string_from_utf8(executable_path()))); }
std::uint64_t bundle_executableURL(id, SEL) { return ret(autorelease(url_new(executable_path(), false))); }
std::uint64_t bundle_pathForResourceOfType(id self, SEL, id name, id type) {
    std::string p = bun(self)->path + "/Contents/Resources/" + to_utf8(name);
    if (type && string_chars(type).length) { p += '.'; p += to_utf8(type); }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return 0;
    return ret(autorelease(string_from_utf8(p)));
}
std::uint64_t bundle_pathForResourceOfTypeInDirectory(id self, SEL, id name, id type, id dir) {
    std::string p = bun(self)->path + "/Contents/Resources/";
    if (dir && string_chars(dir).length) { p += to_utf8(dir); p += '/'; }
    p += to_utf8(name);
    if (type && string_chars(type).length) { p += '.'; p += to_utf8(type); }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return 0;
    return ret(autorelease(string_from_utf8(p)));
}
std::uint64_t bundle_URLForResourceWithExtension(id self, SEL, id name, id type) {
    const std::uint64_t p = bundle_pathForResourceOfType(self, nullptr, name, type);
    return p ? ret(autorelease(url_new(to_utf8(obj(p)), false))) : 0;
}
std::uint64_t bundle_localizedString(id self, SEL, id key, id value, id) { (void)self; return ret(value && string_chars(value).length ? value : key); }
std::uint64_t bundlec_mainBundle(id, SEL) { return ret(main_bundle()); }
std::uint64_t bundlec_bundleWithPath(id, SEL, id path) { return ret(autorelease(make_bundle(to_utf8(path)))); }
std::uint64_t bundlec_bundleForClass(id, SEL, id) { return ret(main_bundle()); }

/* ---- CFPreferences / NSUserDefaults ------------------------------------------ */

/* Preferences live in ~/.config/machoman/<app>.plist, loaded on first use
 * and written back on synchronize. */
struct prefs_store {
    std::mutex mu;
    id dict = nullptr;
    std::string path;
    bool dirty = false;

    void ensure() {
        if (dict) return;
        std::string app = "defaults";
        if (id b = main_bundle(); bun(b)->info)
            if (id v = dictionary_get(bun(b)->info, constant_string("CFBundleIdentifier"))) app = to_utf8(v);
        const char *home = std::getenv("HOME");
        const std::string dir = std::string(home ? home : ".") + "/.config/machoman";
        ::mkdir(std::string(home ? home : ".").append("/.config").c_str(), 0755);
        ::mkdir(dir.c_str(), 0755);
        path = dir + "/" + app + ".plist";
        FILE *f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::string bytes;
            char buf[65536];
            for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.append(buf, n);
            std::fclose(f);
            id parsed = plist_parse(bytes, nullptr);
            if (parsed && is_dictionary(parsed)) {
                dict = dictionary_create(true);
                std::vector<id> keys;
                dictionary_keys(parsed, keys);
                for (id k : keys) dictionary_set(dict, k, dictionary_get(parsed, k));
            }
            if (parsed) release(parsed);
        }
        if (!dict) dict = dictionary_create(true);
    }
    id get(id key) { std::lock_guard lock(mu); ensure(); return dictionary_get(dict, key); }
    void set(id key, id value) {
        std::lock_guard lock(mu);
        ensure();
        if (value) dictionary_set(dict, key, value); else dictionary_remove(dict, key);
        dirty = true;
    }
    bool sync() {
        std::lock_guard lock(mu);
        ensure();
        if (!dirty) return true;
        std::string out;
        if (!plist_write_xml(dict, out)) return false;
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        dirty = false;
        return true;
    }
};
prefs_store &prefs() { static prefs_store *p = new prefs_store; return *p; }

std::uint64_t defaults_objectForKey(id, SEL, id key) { return ret(prefs().get(key)); }
std::uint64_t defaults_setObjectForKey(id, SEL, id value, id key) { prefs().set(key, value); return 0; }
std::uint64_t defaults_removeObjectForKey(id, SEL, id key) { prefs().set(key, nullptr); return 0; }
std::uint64_t defaults_stringForKey(id, SEL, id key) { id v = prefs().get(key); return v && is_string(v) ? ret(v) : 0; }
std::uint64_t defaults_boolForKey(id, SEL, id key) { id v = prefs().get(key); return v && is_number(v) ? (number_int64(v) != 0) : 0; }
std::uint64_t defaults_integerForKey(id, SEL, id key) { id v = prefs().get(key); return v && is_number(v) ? static_cast<std::uint64_t>(number_int64(v)) : 0; }
double defaults_doubleForKey(id, SEL, id key) { id v = prefs().get(key); return v && is_number(v) ? number_double(v) : 0.0; }
std::uint64_t defaults_setBoolForKey(id, SEL, std::uint64_t b, id key) { prefs().set(key, number_bool((b & 0xFF) != 0)); return 0; }
std::uint64_t defaults_setIntegerForKey(id, SEL, std::uint64_t v, id key) { id n = number_from_int64(static_cast<std::int64_t>(v)); prefs().set(key, n); release(n); return 0; }
std::uint64_t defaults_setDoubleForKey(id, SEL, double v, id key) { id n = number_from_double(v); prefs().set(key, n); release(n); return 0; }
std::uint64_t defaults_synchronize(id, SEL) { return prefs().sync(); }
std::uint64_t defaults_registerDefaults(id, SEL, id d) {
    std::vector<id> keys;
    dictionary_keys(d, keys);
    for (id k : keys) if (!prefs().get(k)) prefs().set(k, dictionary_get(d, k));
    return 0;
}
std::uint64_t defaultsc_standardUserDefaults(id, SEL) {
    static id d = nullptr;
    if (!d) d = mm_objc_alloc(g_defaults, 0);
    return ret(d);
}

/* ---- NSProcessInfo ---------------------------------------------------------- */

std::uint64_t proc_processName(id, SEL) {
    const std::string exe = executable_path();
    const auto slash = exe.rfind('/');
    return ret(autorelease(string_from_utf8(slash == std::string::npos ? exe : exe.substr(slash + 1))));
}
std::uint64_t proc_processIdentifier(id, SEL) { return static_cast<std::uint64_t>(getpid()); }
std::uint64_t proc_arguments(id, SEL) {
    const mm_host_api *a = api();
    std::vector<id> v;
    for (int i = 0; a && i < a->argc; i++) v.push_back(string_from_utf8(a->argv[i]));
    id arr = array_create(v.data(), static_cast<CFIndex>(v.size()), false);
    for (id e : v) release(e);
    return ret(autorelease(arr));
}
std::uint64_t proc_environment(id, SEL) {
    id d = dictionary_create(false);
    const mm_host_api *a = api();
    for (char **e = a ? a->envp : nullptr; e && *e; e++) {
        const std::string_view kv(*e);
        const auto eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        id k = string_from_utf8(kv.substr(0, eq)), v = string_from_utf8(kv.substr(eq + 1));
        dictionary_set(d, k, v);
        release(k); release(v);
    }
    return ret(autorelease(d));
}
std::uint64_t proc_hostName(id, SEL) { char buf[256] = {}; gethostname(buf, sizeof buf - 1); return ret(autorelease(string_from_utf8(buf))); }
std::uint64_t proc_activeProcessorCount(id, SEL) { return static_cast<std::uint64_t>(sysconf(_SC_NPROCESSORS_ONLN)); }
std::uint64_t proc_physicalMemory(id, SEL) { return static_cast<std::uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)); }
std::uint64_t proc_operatingSystemVersionString(id, SEL) { return ret(constant_string("Version 12.0 (Build 21A559)")); }
std::uint64_t proc_operatingSystemVersion(id, SEL) { mm_objc_return_words_hi(0); mm_objc_return_doubles_hi(0, 0); return 12; } /* {major, minor}; patch in x2 */
std::uint64_t procc_processInfo(id, SEL) {
    static id p = nullptr;
    if (!p) p = mm_objc_alloc(g_processinfo, 0);
    return ret(p);
}

/* ---- NSFileManager ----------------------------------------------------------- */

std::uint64_t fm_fileExistsAtPath(id, SEL, id path) { struct stat st; return stat(to_utf8(path).c_str(), &st) == 0; }
std::uint64_t fm_fileExistsAtPathIsDirectory(id, SEL, id path, std::uint8_t *is_dir) {
    struct stat st;
    if (stat(to_utf8(path).c_str(), &st) != 0) return 0;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return 1;
}
std::uint64_t fm_isReadableFileAtPath(id, SEL, id path) { return access(to_utf8(path).c_str(), R_OK) == 0; }
std::uint64_t fm_isWritableFileAtPath(id, SEL, id path) { return access(to_utf8(path).c_str(), W_OK) == 0; }
std::uint64_t fm_isExecutableFileAtPath(id, SEL, id path) { return access(to_utf8(path).c_str(), X_OK) == 0; }
std::uint64_t fm_removeItemAtPath(id, SEL, id path, id *error) {
    const std::string p = to_utf8(path);
    struct stat st;
    if (stat(p.c_str(), &st) != 0) { if (error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr)); return 0; }
    const int r = S_ISDIR(st.st_mode) ? rmdir(p.c_str()) : unlink(p.c_str());
    if (r != 0 && error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr));
    return r == 0;
}
std::uint64_t fm_createDirectoryAtPath(id, SEL, id path, std::uint64_t intermediates, id, id *error) {
    std::string p = to_utf8(path);
    if (intermediates) {
        for (std::size_t i = 1; i < p.size(); i++)
            if (p[i] == '/') { p[i] = 0; ::mkdir(p.c_str(), 0755); p[i] = '/'; }
    }
    const int r = ::mkdir(p.c_str(), 0755);
    if (r != 0 && errno == EEXIST) return 1;
    if (r != 0 && error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr));
    return r == 0;
}
std::uint64_t fm_contentsOfDirectoryAtPath(id, SEL, id path, id *error) {
    DIR *d = opendir(to_utf8(path).c_str());
    if (!d) { if (error) *error = autorelease(error_create(NSCocoaErrorDomain_, 260, nullptr)); return 0; }
    std::vector<id> items;
    while (dirent *e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        items.push_back(string_from_utf8(e->d_name));
    }
    closedir(d);
    id arr = array_create(items.data(), static_cast<CFIndex>(items.size()), false);
    for (id e : items) release(e);
    return ret(autorelease(arr));
}
std::uint64_t fm_attributesOfItemAtPath(id, SEL, id path, id *error) {
    struct stat st;
    if (stat(to_utf8(path).c_str(), &st) != 0) { if (error) *error = autorelease(error_create(NSCocoaErrorDomain_, 260, nullptr)); return 0; }
    id d = dictionary_create(false);
    auto put_number = [&](id key, std::int64_t v) {
        id n = number_from_int64(v);
        dictionary_set(d, key, n);
        release(n);
    };
    put_number(NSFileSize_, st.st_size);
    dictionary_set(d, NSFileType_, S_ISDIR(st.st_mode) ? NSFileTypeDirectory_ : NSFileTypeRegular_);
    put_number(constant_string("NSFileSystemFileNumber"), static_cast<std::int64_t>(st.st_ino));
    put_number(constant_string("NSFileSystemNumber"), static_cast<std::int64_t>(st.st_dev));
    put_number(constant_string("NSFilePosixPermissions"), st.st_mode & 07777);
    put_number(constant_string("NSFileOwnerAccountID"), st.st_uid);
    put_number(constant_string("NSFileGroupOwnerAccountID"), st.st_gid);
    put_number(constant_string("NSFileReferenceCount"), static_cast<std::int64_t>(st.st_nlink));
    put_number(NSFileHFSCreatorCode_, 0);
    put_number(NSFileHFSTypeCode_, 0);
    dictionary_set(d, NSFileImmutable_, number_bool(false));
    return ret(autorelease(d));
}

/* Foundation's accessors for the attribute dictionaries above; they are a
 * category on NSDictionary, so they apply to any dictionary. */
std::uint64_t attrs_value(id self, const char *key) { return ret(dictionary_get(self, constant_string(key))); }
std::uint64_t attrs_fileSize(id self, SEL) { id v = obj(attrs_value(self, "NSFileSize")); return v ? static_cast<std::uint64_t>(number_int64(v)) : 0; }
std::uint64_t attrs_fileType(id self, SEL) { return attrs_value(self, "NSFileType"); }
std::uint64_t attrs_filePosixPermissions(id self, SEL) { id v = obj(attrs_value(self, "NSFilePosixPermissions")); return v ? static_cast<std::uint64_t>(number_int64(v)) : 0; }
std::uint64_t attrs_fileSystemFileNumber(id self, SEL) { id v = obj(attrs_value(self, "NSFileSystemFileNumber")); return v ? static_cast<std::uint64_t>(number_int64(v)) : 0; }
std::uint64_t attrs_fileSystemNumber(id self, SEL) { id v = obj(attrs_value(self, "NSFileSystemNumber")); return v ? static_cast<std::uint64_t>(number_int64(v)) : 0; }
std::uint64_t attrs_fileOwnerAccountID(id self, SEL) { return attrs_value(self, "NSFileOwnerAccountID"); }
std::uint64_t attrs_fileGroupOwnerAccountID(id self, SEL) { return attrs_value(self, "NSFileGroupOwnerAccountID"); }
std::uint64_t attrs_fileHFSCreatorCode(id self, SEL) { id v = obj(attrs_value(self, "NSFileHFSCreatorCode")); return v ? static_cast<std::uint64_t>(number_int64(v)) : 0; }
std::uint64_t attrs_fileHFSTypeCode(id self, SEL) { id v = obj(attrs_value(self, "NSFileHFSTypeCode")); return v ? static_cast<std::uint64_t>(number_int64(v)) : 0; }
std::uint64_t attrs_fileIsImmutable(id self, SEL) { id v = obj(attrs_value(self, "NSFileImmutable")); return v ? (number_int64(v) != 0) : 0; }
std::uint64_t attrs_fileModificationDate(id self, SEL) { return attrs_value(self, "NSFileModificationDate"); }
std::uint64_t fm_setAttributes(id, SEL, id attrs, id path, id *error) {
    const std::string p = to_utf8(path);
    bool ok = true;
    if (id mode = dictionary_get(attrs, constant_string("NSFilePosixPermissions")))
        ok = chmod(p.c_str(), static_cast<mode_t>(number_int64(mode))) == 0 && ok;
    /* Ownership and the HFS-only attributes are not applied: a translated
     * program is not running as root and has no HFS metadata to set. */
    if (!ok && error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr));
    return ok;
}
std::uint64_t fm_copyItemAtPath(id, SEL, id from, id to, id *error) {
    std::error_code ec;
    std::filesystem::copy(to_utf8(from), to_utf8(to), std::filesystem::copy_options::recursive, ec);
    if (ec && error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr));
    return !ec;
}
std::uint64_t fm_createSymbolicLink(id, SEL, id path, id target, id *error) {
    const int r = symlink(to_utf8(target).c_str(), to_utf8(path).c_str());
    if (r != 0 && error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr));
    return r == 0;
}
std::uint64_t fm_destinationOfSymbolicLink(id, SEL, id path, id *error) {
    char buf[4096];
    const ssize_t n = readlink(to_utf8(path).c_str(), buf, sizeof buf - 1);
    if (n < 0) { if (error) *error = autorelease(error_create(NSCocoaErrorDomain_, 260, nullptr)); return 0; }
    buf[n] = 0;
    return ret(autorelease(string_from_utf8(buf)));
}
std::uint64_t fm_subpathsOfDirectory(id, SEL, id path, id *error) {
    std::error_code ec;
    const std::string root = to_utf8(path);
    std::vector<id> items;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); ++it)
        items.push_back(string_from_utf8(std::filesystem::relative(it->path(), root, ec).string()));
    if (ec) { if (error) *error = autorelease(error_create(NSCocoaErrorDomain_, 260, nullptr)); for (id e : items) release(e); return 0; }
    id arr = array_create(items.data(), static_cast<CFIndex>(items.size()), false);
    for (id e : items) release(e);
    return ret(autorelease(arr));
}

std::uint64_t fm_contentsAtPath(id, SEL, id path) { return send(reinterpret_cast<id>(cls("NSData")), "dataWithContentsOfFile:", ret(path)); }
std::uint64_t fm_createFileAtPath(id, SEL, id path, id data, id) {
    FILE *f = std::fopen(to_utf8(path).c_str(), "wb");
    if (!f) return 0;
    if (data) { const std::string &b = data_storage(data); std::fwrite(b.data(), 1, b.size(), f); }
    std::fclose(f);
    return 1;
}
std::uint64_t fm_moveItemAtPath(id, SEL, id from, id to, id *error) {
    const int r = rename(to_utf8(from).c_str(), to_utf8(to).c_str());
    if (r != 0 && error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr));
    return r == 0;
}
/* NSSearchPathDirectory -> a directory under $HOME, the same mapping
 * NSSearchPathForDirectoriesInDomains uses. */
std::uint64_t fm_URLForDirectory(id, SEL, std::uint64_t directory, std::uint64_t, id, std::uint64_t create, id *error) {
    id paths = fnd::search_path_directories(directory);
    id first = obj(send(paths, "firstObject"));
    if (!first) { if (error) *error = autorelease(error_create(NSCocoaErrorDomain_, 4, nullptr)); return 0; }
    const std::string p = to_utf8(first);
    if (create & 0xFF) {
        std::string dir;
        for (std::size_t i = 1; i <= p.size(); i++)
            if (i == p.size() || p[i] == '/') { dir = p.substr(0, i); ::mkdir(dir.c_str(), 0755); }
    }
    return ret(autorelease(url_new(p, true)));
}

std::uint64_t fm_URLsForDirectory(id, SEL, std::uint64_t directory, std::uint64_t) {
    id paths = fnd::search_path_directories(directory);
    id urls = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id p : array_storage(paths)) send(urls, "addObject:", ret(autorelease(url_new(to_utf8(p), true))));
    return ret(urls);
}

std::uint64_t fm_currentDirectoryPath(id, SEL) { char buf[4096]; return ret(autorelease(string_from_utf8(getcwd(buf, sizeof buf) ? buf : "."))); }
std::uint64_t fmc_defaultManager(id, SEL) {
    static id m = nullptr;
    if (!m) m = mm_objc_alloc(g_filemanager, 0);
    return ret(m);
}

/* ---- NSLocale --------------------------------------------------------------- */

std::uint64_t locale_localeIdentifier(id, SEL) { return ret(constant_string("en_US")); }
std::uint64_t locale_objectForKey(id, SEL, id key) {
    const std::string k = describe(key);
    if (k == "kCFLocaleDecimalSeparator" || k == "NSLocaleDecimalSeparator") return ret(constant_string("."));
    if (k == "kCFLocaleIdentifier" || k == "NSLocaleIdentifier") return ret(constant_string("en_US"));
    if (k == "kCFLocaleGroupingSeparator" || k == "NSLocaleGroupingSeparator") return ret(constant_string(","));
    if (k == "kCFLocaleCountryCode" || k == "NSLocaleCountryCode") return ret(constant_string("US"));
    if (k == "kCFLocaleLanguageCode" || k == "NSLocaleLanguageCode") return ret(constant_string("en"));
    return 0;
}
id current_locale() {
    static id l = nullptr;
    if (!l) l = mm_objc_alloc(g_locale, 0);
    return l;
}
std::uint64_t localec_currentLocale(id, SEL) { return ret(current_locale()); }
std::uint64_t localec_preferredLanguages(id, SEL) {
    id s = constant_string("en-US");
    return ret(autorelease(array_create(&s, 1, false)));
}

/* ---- NSUUID / CFUUID ---------------------------------------------------------- */

struct uuid_object { mm_objc_object base; std::uint8_t bytes[16]{}; };
void random_uuid(std::uint8_t *out) {
    FILE *f = std::fopen("/dev/urandom", "rb");
    if (f) { if (std::fread(out, 1, 16, f) != 16) {} std::fclose(f); }
    else for (int i = 0; i < 16; i++) out[i] = static_cast<std::uint8_t>(std::rand());
    out[6] = static_cast<std::uint8_t>((out[6] & 0x0F) | 0x40);
    out[8] = static_cast<std::uint8_t>((out[8] & 0x3F) | 0x80);
}
std::string uuid_string(const std::uint8_t *b) {
    char buf[37];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}
std::uint64_t uuid_init(id self, SEL) { random_uuid(as<uuid_object>(self)->bytes); return ret(self); }
std::uint64_t uuid_UUIDString(id self, SEL) { return ret(autorelease(string_from_utf8(uuid_string(as<uuid_object>(self)->bytes)))); }
std::uint64_t uuid_description(id self, SEL) { return uuid_UUIDString(self, nullptr); }
std::uint64_t uuid_getUUIDBytes(id self, SEL, std::uint8_t *out) { std::memcpy(out, as<uuid_object>(self)->bytes, 16); return 0; }
std::uint64_t uuidc_UUID(id c, SEL) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "init")))); }

/* ---- run loop ------------------------------------------------------------------ */

/* One run loop per guest thread. Sources are blocks/functions queued with
 * CFRunLoopPerformBlock; CFRunLoopRun drains them and then waits until
 * something is queued or the loop is stopped. Timers and ports come with
 * the event loop AppKit will need, not before. */
struct run_loop {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<void *, bool>> blocks; /* block or function pointer, is_block */
    bool stopped = false;
    mm_objc_object base_storage{};
};
run_loop *loop_for_current_thread() {
    static std::mutex mu;
    static std::vector<std::pair<std::uint64_t, run_loop *>> loops;
    const std::uint64_t tid = g_api ? g_api->thread_self() : 0;
    std::lock_guard lock(mu);
    for (auto &[k, v] : loops) if (k == tid) return v;
    auto *l = new run_loop;
    l->base_storage.isa = g_runloop;
    loops.emplace_back(tid, l);
    return l;
}
run_loop *g_main_loop = nullptr;
run_loop *loop_of(id o) { return reinterpret_cast<run_loop *>(reinterpret_cast<char *>(o) - offsetof(run_loop, base_storage)); }
id loop_object(run_loop *l) { return reinterpret_cast<id>(&l->base_storage); }

void run_loop_drain(run_loop *l) {
    dispatch_drain_main_queue();
    for (;;) {
        std::pair<void *, bool> item;
        {
            std::lock_guard lock(l->mu);
            if (l->blocks.empty()) return;
            item = l->blocks.front();
            l->blocks.erase(l->blocks.begin());
        }
        if (item.second) mm_objc_block_invoke(item.first, 0, nullptr);
        else if (api()->is_guest_code(reinterpret_cast<std::uintptr_t>(item.first))) api()->call_guest(reinterpret_cast<std::uintptr_t>(item.first), nullptr, 0);
        else reinterpret_cast<void (*)()>(item.first)();
    }
}

std::uint64_t runloop_run(id self, SEL) {
    run_loop *l = loop_of(self);
    for (;;) {
        run_loop_drain(l);
        std::unique_lock lock(l->mu);
        if (l->stopped) { l->stopped = false; return 0; }
        if (l->blocks.empty()) l->cv.wait_for(lock, std::chrono::milliseconds(100));
    }
}
std::uint64_t runloop_stop(id self, SEL) { run_loop *l = loop_of(self); { std::lock_guard lock(l->mu); l->stopped = true; } l->cv.notify_all(); return 0; }
std::uint64_t runloopc_currentRunLoop(id, SEL) { return ret(loop_object(loop_for_current_thread())); }
std::uint64_t runloopc_mainRunLoop(id, SEL) { return ret(loop_object(g_main_loop ? g_main_loop : loop_for_current_thread())); }

/* ---- NSAutoreleasePool / NSThread / misc classes --------------------------------- */

struct pool_object { mm_objc_object base; void *token = nullptr; };
std::uint64_t pool_init(id self, SEL) { as<pool_object>(self)->token = objc_autoreleasePoolPush_(); return ret(self); }
std::uint64_t pool_drain(id self, SEL) { objc_autoreleasePoolPop_(as<pool_object>(self)->token); mm_objc_free_instance(self); return 0; }

std::uint64_t thread_isMainThread(id, SEL) { return api()->thread_self() == 1; }
std::uint64_t threadc_currentThread(id c, SEL) { static id t = nullptr; if (!t) t = mm_objc_alloc(reinterpret_cast<Class>(c), 0); return ret(t); }
std::uint64_t threadc_sleepForTimeInterval(id, SEL, double s) { struct timespec ts{static_cast<time_t>(s), static_cast<long>((s - static_cast<double>(static_cast<time_t>(s))) * 1e9)}; nanosleep(&ts, nullptr); return 0; }

/* ---- NSAppleEventManager ------------------------------------------------------ */

/* Apple events are how macOS delivers "open this document", "quit" and the
 * scripting commands to an application. Nothing sends them here, so the
 * manager keeps the handlers a program installs — which is all it asks of
 * it at startup — and there is no event to dispatch to them. */
struct apple_event_manager { mm_objc_object base; std::vector<std::pair<id, SEL>> handlers; };

std::uint64_t aem_shared(id c, SEL) {
    static id shared = nullptr;
    if (!shared) shared = &make<apple_event_manager>(reinterpret_cast<Class>(c))->base;
    return ret(shared);
}
std::uint64_t aem_setEventHandler(id self, SEL, id handler, SEL sel, std::uint32_t, std::uint32_t) {
    as<apple_event_manager>(self)->handlers.emplace_back(handler, sel);
    return 0;
}
std::uint64_t aem_removeEventHandler(id self, SEL, std::uint32_t, std::uint32_t) {
    as<apple_event_manager>(self)->handlers.clear();
    return 0;
}
std::uint64_t aem_nil(id, SEL) { return 0; }

/* ---- NSNotificationCenter ------------------------------------------------------ */

/* A notification is its name, the object that posted it and a user info
 * dictionary; an observer is a receiver plus the selector to send it,
 * optionally narrowed to one name and one posting object. Blocks registered
 * with -addObserverForName:object:queue:usingBlock: are called on the
 * posting thread, which is what a nil queue means on macOS. */
struct notification_object {
    mm_objc_object base;
    id name = nullptr, object = nullptr, user_info = nullptr;
    ~notification_object() { release(name); release(object); release(user_info); }
};

struct observer_entry {
    id observer = nullptr;   /* not retained, as on macOS */
    SEL selector = nullptr;
    id block = nullptr;      /* retained when set; mutually exclusive with selector */
    std::string name;        /* empty = every name */
    bool any_name = true;
    id object = nullptr;     /* null = every poster */
    id token = nullptr;      /* the object -addObserverForName: returned */
};

struct center_object {
    mm_objc_object base;
    std::vector<observer_entry> observers;
    std::recursive_mutex mu;
};

id notification_create(id name, id object, id user_info) {
    notification_object *n = make<notification_object>(g_notification);
    n->name = retain(name);
    n->object = retain(object);
    n->user_info = retain(user_info);
    return &n->base;
}

std::uint64_t notification_name(id self, SEL) { return ret(as<notification_object>(self)->name); }
std::uint64_t notification_object_(id self, SEL) { return ret(as<notification_object>(self)->object); }
std::uint64_t notification_userInfo(id self, SEL) { return ret(as<notification_object>(self)->user_info); }
std::uint64_t notificationc_notificationWithName(id, SEL, id name, id object) {
    return ret(autorelease(notification_create(name, object, nullptr)));
}
std::uint64_t notificationc_notificationWithNameUserInfo(id, SEL, id name, id object, id info) {
    return ret(autorelease(notification_create(name, object, info)));
}

center_object *center_of(id self) { return as<center_object>(self); }

void center_add(id self, id observer, SEL sel, id block, id name, id object, id token) {
    center_object *c = center_of(self);
    observer_entry e;
    e.observer = observer;
    e.selector = sel;
    e.block = block ? retain(block) : nullptr;
    e.any_name = name == nullptr;
    if (name) e.name = to_utf8(name);
    e.object = object;
    e.token = token;
    std::lock_guard lock(c->mu);
    c->observers.push_back(e);
}

std::uint64_t center_addObserver(id self, SEL, id observer, SEL sel, id name, id object) {
    center_add(self, observer, sel, nullptr, name, object, nullptr);
    return 0;
}
std::uint64_t center_addObserverForName(id self, SEL, id name, id object, id, id block) {
    /* The token is what the caller later passes to -removeObserver:. */
    id token = mm_objc_alloc(cls("NSObject"), 0);
    center_add(self, nullptr, nullptr, block, name, object, token);
    return ret(token);
}
std::uint64_t center_removeObserver(id self, SEL, id observer) {
    center_object *c = center_of(self);
    std::lock_guard lock(c->mu);
    std::erase_if(c->observers, [&](const observer_entry &e) {
        const bool gone = e.observer == observer || (e.token && e.token == observer);
        if (gone && e.block) release(e.block);
        return gone;
    });
    return 0;
}
std::uint64_t center_removeObserverName(id self, SEL, id observer, id name, id object) {
    center_object *c = center_of(self);
    const std::string wanted = name ? to_utf8(name) : std::string();
    std::lock_guard lock(c->mu);
    std::erase_if(c->observers, [&](const observer_entry &e) {
        if (e.observer != observer && !(e.token && e.token == observer)) return false;
        if (name && (e.any_name || e.name != wanted)) return false;
        if (object && e.object && e.object != object) return false;
        if (e.block) release(e.block);
        return true;
    });
    return 0;
}

std::uint64_t center_postNotification(id self, SEL, id note) {
    if (!note) return 0;
    id name = obj(send(note, "name"));
    id object = obj(send(note, "object"));
    const std::string posted = name ? to_utf8(name) : std::string();
    /* Deliver from a copy: an observer may add or remove observers. */
    std::vector<observer_entry> matches;
    {
        center_object *c = center_of(self);
        std::lock_guard lock(c->mu);
        for (const observer_entry &e : c->observers) {
            if (!e.any_name && e.name != posted) continue;
            if (e.object && object && e.object != object) continue;
            matches.push_back(e);
        }
    }
    if (trace() && !matches.empty())
        std::fprintf(stderr, "Foundation: posting %s to %zu observer(s)\n", posted.c_str(), matches.size());
    for (const observer_entry &e : matches) {
        if (e.block) mm_objc_block_invoke(e.block, 1, reinterpret_cast<const std::uint64_t *>(&note));
        else if (e.observer && e.selector) {
            const std::uint64_t arg = ret(note);
            mm_objc_send(e.observer, e.selector, 1, &arg);
        }
    }
    return 0;
}

std::uint64_t center_postNotificationName(id self, SEL, id name, id object) {
    id note = notification_create(name, object, nullptr);
    center_postNotification(self, nullptr, note);
    release(note);
    return 0;
}
std::uint64_t center_postNotificationNameUserInfo(id self, SEL, id name, id object, id info) {
    id note = notification_create(name, object, info);
    center_postNotification(self, nullptr, note);
    release(note);
    return 0;
}
/* The distributed center's extra arguments (deliver immediately, coalescing)
 * change nothing when there is only this process to notify. */
std::uint64_t center_postNotificationNameDeliver(id self, SEL, id name, id object, id info, std::uint64_t, std::uint64_t) {
    return center_postNotificationNameUserInfo(self, nullptr, name, object, info);
}
std::uint64_t centerc_defaultCenter(id c, SEL) {
    static id d = nullptr, dd = nullptr;
    id &slot = reinterpret_cast<Class>(c) == g_distributed_center ? dd : d;
    if (!slot) slot = &make<center_object>(reinterpret_cast<Class>(c))->base;
    return ret(slot);
}

std::uint64_t exception_raise(id self, SEL) {
    std::fprintf(stderr, "Foundation: uncaught NSException %s\n", describe(self).c_str());
    std::abort();
}
std::uint64_t exceptionc_raiseFormat(id, SEL, id name, id fmt) {
    std::fprintf(stderr, "Foundation: NSException %s: %s\n", describe(name).c_str(), describe(fmt).c_str());
    std::abort();
}

} // namespace

/* ---- shared helpers ---------------------------------------------------------- */

const mm_host_api *api() { return g_api; }
bool trace() { return g_trace; }
Class cls(const char *name) { return mm_objc_class_named(name); }

void add_methods(Class c, const method *m, std::size_t n) { for (std::size_t i = 0; i < n; i++) mm_objc_add_method(c, m[i].sel, m[i].imp, m[i].types); }
void add_class_methods(Class c, const method *m, std::size_t n) { for (std::size_t i = 0; i < n; i++) mm_objc_add_class_method(c, m[i].sel, m[i].imp, m[i].types); }

id error_create(id domain, std::int64_t code, id user_info) {
    auto *e = make<error_dtor>(g_error);
    e->domain = retain(domain);
    e->code = code;
    e->user_info = user_info ? retain(user_info) : nullptr;
    return reinterpret_cast<id>(e);
}

std::string home_directory() {
    if (const char *h = std::getenv("HOME")) return h;
    const passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/";
}
std::string main_bundle_path() { return bun(main_bundle())->path; }
id url_from_path(std::string_view path, bool is_dir) { return url_new(std::string(path), is_dir); }
std::string url_path(id url) { return is_url(url) ? u(url)->path : to_utf8(url); }
bool is_url(id o) { return o && is_kind(o, g_url); }

void register_misc() {
    Class nsobject = cls("NSObject");
    init_constant_strings();
    kCFAllocatorSystemDefault_storage.base.isa = nsobject;
    kCFAllocatorMalloc_storage.base.isa = nsobject;
    kCFAllocatorNull_storage.base.isa = nsobject;
    kCFAllocatorSystemDefault_ptr = &kCFAllocatorSystemDefault_storage;
    kCFAllocatorDefault_ptr = nullptr; /* Apple's kCFAllocatorDefault is NULL */
    kCFAllocatorMalloc_ptr = &kCFAllocatorMalloc_storage;
    kCFAllocatorMallocZone_ptr = &kCFAllocatorMalloc_storage;
    kCFAllocatorNull_ptr = &kCFAllocatorNull_storage;
    kCFAllocatorUseContext_ptr = &kCFAllocatorSystemDefault_storage;

    g_url = mm_objc_define_class("NSURL", nsobject, sizeof(url_object), FND_CLASS_REF(NSURL), FND_META_REF(NSURL));
    register_lifecycle<url_object>(g_url);
    FND_METHODS(g_url,
        {"path", IMP(url_path_m), "@16@0:8"},
        {"absoluteString", IMP(url_absoluteString), "@16@0:8"},
        {"relativePath", IMP(url_path_m), "@16@0:8"},
        {"scheme", IMP(url_scheme), "@16@0:8"},
        {"isFileURL", IMP(url_isFileURL), "B16@0:8"},
        {"lastPathComponent", IMP(url_lastPathComponent), "@16@0:8"},
        {"pathExtension", IMP(url_pathExtension), "@16@0:8"},
        {"URLByDeletingLastPathComponent", IMP(url_URLByDeletingLastPathComponent), "@16@0:8"},
        {"URLByAppendingPathComponent:", IMP(url_URLByAppendingPathComponent), "@24@0:8@16"},
        {"URLByStandardizingPath", IMP(url_copyWithZone), "@16@0:8"},
        {"URLByResolvingSymlinksInPath", IMP(url_copyWithZone), "@16@0:8"},
        {"initFileURLWithPath:", IMP(url_initFileURLWithPath), "@24@0:8@16"},
        {"initFileURLWithPath:isDirectory:", IMP(url_initFileURLWithPathIsDirectory), "@32@0:8@16B24"},
        {"initWithString:", IMP(url_initWithString), "@24@0:8@16"},
        {"description", IMP(url_description), "@16@0:8"},
        {"isEqual:", IMP(url_isEqual), "B24@0:8@16"},
        {"hash", IMP(url_hash), "Q16@0:8"},
        {"copyWithZone:", IMP(url_copyWithZone), "@24@0:8^v16"});
    FND_METHODS(g_url,
        {"bookmarkDataWithOptions:includingResourceValuesForKeys:relativeToURL:error:",
         IMP(url_bookmarkDataWithOptions), "@48@0:8Q16@24@32^@40"},
        {"initByResolvingBookmarkData:options:relativeToURL:bookmarkDataIsStale:error:",
         IMP(url_initByResolvingBookmarkData), "@56@0:8@16Q24@32^B40^@48"});
    FND_CLASS_METHODS(g_url,
        {"bookmarkDataWithContentsOfURL:error:", IMP(urlc_bookmarkDataWithContentsOfURL), "@32@0:8@16^@24"},
        {"URLByResolvingBookmarkData:options:relativeToURL:bookmarkDataIsStale:error:",
         IMP(urlc_URLByResolvingBookmarkData), "@56@0:8@16Q24@32^B40^@48"},
        {"resourceValuesForKeys:fromBookmarkData:", IMP(urlc_resourceValuesForKeys), "@32@0:8@16@24"},
        {"writeBookmarkData:toURL:options:error:", IMP(urlc_writeBookmarkData), "B48@0:8@16@24Q32^@40"},
        {"fileURLWithPath:", IMP(urlc_fileURLWithPath), "@24@0:8@16"},
        {"fileURLWithPath:isDirectory:", IMP(urlc_fileURLWithPathIsDirectory), "@32@0:8@16B24"},
        {"URLWithString:", IMP(urlc_URLWithString), "@24@0:8@16"});

    g_error = mm_objc_define_class("NSError", nsobject, sizeof(error_object), FND_CLASS_REF(NSError), FND_META_REF(NSError));
    register_lifecycle<error_dtor>(g_error);
    FND_METHODS(g_error,
        {"domain", IMP(err_domain), "@16@0:8"},
        {"code", IMP(err_code), "q16@0:8"},
        {"userInfo", IMP(err_userInfo), "@16@0:8"},
        {"localizedDescription", IMP(err_localizedDescription), "@16@0:8"},
        {"localizedFailureReason", IMP(err_localizedDescription), "@16@0:8"},
        {"description", IMP(err_description), "@16@0:8"},
        {"initWithDomain:code:userInfo:", IMP(err_initWithDomainCodeUserInfo), "@40@0:8@16q24@32"});
    FND_CLASS_METHODS(g_error, {"errorWithDomain:code:userInfo:", IMP(errc_errorWithDomainCodeUserInfo), "@40@0:8@16q24@32"});

    g_bundle = mm_objc_define_class("NSBundle", nsobject, sizeof(bundle_object), FND_CLASS_REF(NSBundle), FND_META_REF(NSBundle));
    register_lifecycle<bundle_object>(g_bundle);
    FND_METHODS(g_bundle,
        {"bundlePath", IMP(bundle_bundlePath), "@16@0:8"},
        {"bundleURL", IMP(bundle_bundleURL), "@16@0:8"},
        {"resourcePath", IMP(bundle_resourcePath), "@16@0:8"},
        {"resourceURL", IMP(bundle_resourceURL), "@16@0:8"},
        {"infoDictionary", IMP(bundle_infoDictionary), "@16@0:8"},
        {"localizedInfoDictionary", IMP(bundle_infoDictionary), "@16@0:8"},
        {"objectForInfoDictionaryKey:", IMP(bundle_objectForInfoDictionaryKey), "@24@0:8@16"},
        {"bundleIdentifier", IMP(bundle_bundleIdentifier), "@16@0:8"},
        {"executablePath", IMP(bundle_executablePath), "@16@0:8"},
        {"executableURL", IMP(bundle_executableURL), "@16@0:8"},
        {"pathForResource:ofType:", IMP(bundle_pathForResourceOfType), "@32@0:8@16@24"},
        {"pathForResource:ofType:inDirectory:", IMP(bundle_pathForResourceOfTypeInDirectory), "@40@0:8@16@24@32"},
        {"URLForResource:withExtension:", IMP(bundle_URLForResourceWithExtension), "@32@0:8@16@24"},
        {"localizedStringForKey:value:table:", IMP(bundle_localizedString), "@40@0:8@16@24@32"});
    FND_CLASS_METHODS(g_bundle,
        {"mainBundle", IMP(bundlec_mainBundle), "@16@0:8"},
        {"bundleWithPath:", IMP(bundlec_bundleWithPath), "@24@0:8@16"},
        {"bundleForClass:", IMP(bundlec_bundleForClass), "@24@0:8#16"});

    g_defaults = mm_objc_define_class("NSUserDefaults", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSUserDefaults), FND_META_REF(NSUserDefaults));
    FND_METHODS(g_defaults,
        {"objectForKey:", IMP(defaults_objectForKey), "@24@0:8@16"},
        {"setObject:forKey:", IMP(defaults_setObjectForKey), "v32@0:8@16@24"},
        {"removeObjectForKey:", IMP(defaults_removeObjectForKey), "v24@0:8@16"},
        {"stringForKey:", IMP(defaults_stringForKey), "@24@0:8@16"},
        {"boolForKey:", IMP(defaults_boolForKey), "B24@0:8@16"},
        {"integerForKey:", IMP(defaults_integerForKey), "q24@0:8@16"},
        {"doubleForKey:", IMP(defaults_doubleForKey), "d24@0:8@16"},
        {"setBool:forKey:", IMP(defaults_setBoolForKey), "v32@0:8B16@24"},
        {"setInteger:forKey:", IMP(defaults_setIntegerForKey), "v32@0:8q16@24"},
        {"setDouble:forKey:", IMP(defaults_setDoubleForKey), "v32@0:8d16@24"},
        {"synchronize", IMP(defaults_synchronize), "B16@0:8"},
        {"registerDefaults:", IMP(defaults_registerDefaults), "v24@0:8@16"});
    FND_CLASS_METHODS(g_defaults, {"standardUserDefaults", IMP(defaultsc_standardUserDefaults), "@16@0:8"});

    g_processinfo = mm_objc_define_class("NSProcessInfo", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSProcessInfo), FND_META_REF(NSProcessInfo));
    FND_METHODS(g_processinfo,
        {"processName", IMP(proc_processName), "@16@0:8"},
        {"processIdentifier", IMP(proc_processIdentifier), "i16@0:8"},
        {"arguments", IMP(proc_arguments), "@16@0:8"},
        {"environment", IMP(proc_environment), "@16@0:8"},
        {"hostName", IMP(proc_hostName), "@16@0:8"},
        {"activeProcessorCount", IMP(proc_activeProcessorCount), "Q16@0:8"},
        {"processorCount", IMP(proc_activeProcessorCount), "Q16@0:8"},
        {"physicalMemory", IMP(proc_physicalMemory), "Q16@0:8"},
        {"operatingSystemVersionString", IMP(proc_operatingSystemVersionString), "@16@0:8"},
        {"operatingSystemVersion", IMP(proc_operatingSystemVersion), "{?=qqq}16@0:8"});
    FND_CLASS_METHODS(g_processinfo, {"processInfo", IMP(procc_processInfo), "@16@0:8"});

    g_filemanager = mm_objc_define_class("NSFileManager", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSFileManager), FND_META_REF(NSFileManager));
    FND_METHODS(g_filemanager,
        {"fileExistsAtPath:", IMP(fm_fileExistsAtPath), "B24@0:8@16"},
        {"fileExistsAtPath:isDirectory:", IMP(fm_fileExistsAtPathIsDirectory), "B32@0:8@16^B24"},
        {"isReadableFileAtPath:", IMP(fm_isReadableFileAtPath), "B24@0:8@16"},
        {"isWritableFileAtPath:", IMP(fm_isWritableFileAtPath), "B24@0:8@16"},
        {"isExecutableFileAtPath:", IMP(fm_isExecutableFileAtPath), "B24@0:8@16"},
        {"removeItemAtPath:error:", IMP(fm_removeItemAtPath), "B32@0:8@16^@24"},
        {"createDirectoryAtPath:withIntermediateDirectories:attributes:error:", IMP(fm_createDirectoryAtPath), "B48@0:8@16B24@32^@40"},
        {"contentsOfDirectoryAtPath:error:", IMP(fm_contentsOfDirectoryAtPath), "@32@0:8@16^@24"},
        {"attributesOfItemAtPath:error:", IMP(fm_attributesOfItemAtPath), "@32@0:8@16^@24"},
        {"contentsAtPath:", IMP(fm_contentsAtPath), "@24@0:8@16"},
        {"createFileAtPath:contents:attributes:", IMP(fm_createFileAtPath), "B40@0:8@16@24@32"},
        {"moveItemAtPath:toPath:error:", IMP(fm_moveItemAtPath), "B40@0:8@16@24^@32"},
        {"currentDirectoryPath", IMP(fm_currentDirectoryPath), "@16@0:8"},
        {"URLForDirectory:inDomain:appropriateForURL:create:error:", IMP(fm_URLForDirectory), "@56@0:8Q16Q24@32B40^@48"},
        {"URLsForDirectory:inDomains:", IMP(fm_URLsForDirectory), "@32@0:8Q16Q24"},
        {"setAttributes:ofItemAtPath:error:", IMP(fm_setAttributes), "B40@0:8@16@24^@32"},
        {"copyItemAtPath:toPath:error:", IMP(fm_copyItemAtPath), "B40@0:8@16@24^@32"},
        {"createSymbolicLinkAtPath:withDestinationPath:error:", IMP(fm_createSymbolicLink), "B40@0:8@16@24^@32"},
        {"destinationOfSymbolicLinkAtPath:error:", IMP(fm_destinationOfSymbolicLink), "@32@0:8@16^@24"},
        {"subpathsOfDirectoryAtPath:error:", IMP(fm_subpathsOfDirectory), "@32@0:8@16^@24"});
    FND_CLASS_METHODS(g_filemanager, {"defaultManager", IMP(fmc_defaultManager), "@16@0:8"});
    if (Class dict_class = cls("NSDictionary"))
        FND_METHODS(dict_class,
            {"fileSize", IMP(attrs_fileSize), "Q16@0:8"},
            {"fileType", IMP(attrs_fileType), "@16@0:8"},
            {"filePosixPermissions", IMP(attrs_filePosixPermissions), "Q16@0:8"},
            {"fileSystemFileNumber", IMP(attrs_fileSystemFileNumber), "Q16@0:8"},
            {"fileSystemNumber", IMP(attrs_fileSystemNumber), "q16@0:8"},
            {"fileOwnerAccountID", IMP(attrs_fileOwnerAccountID), "@16@0:8"},
            {"fileGroupOwnerAccountID", IMP(attrs_fileGroupOwnerAccountID), "@16@0:8"},
            {"fileHFSCreatorCode", IMP(attrs_fileHFSCreatorCode), "I16@0:8"},
            {"fileHFSTypeCode", IMP(attrs_fileHFSTypeCode), "I16@0:8"},
            {"fileIsImmutable", IMP(attrs_fileIsImmutable), "B16@0:8"},
            {"fileModificationDate", IMP(attrs_fileModificationDate), "@16@0:8"});

    g_locale = mm_objc_define_class("NSLocale", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSLocale), FND_META_REF(NSLocale));
    FND_METHODS(g_locale,
        {"localeIdentifier", IMP(locale_localeIdentifier), "@16@0:8"},
        {"objectForKey:", IMP(locale_objectForKey), "@24@0:8@16"});
    FND_CLASS_METHODS(g_locale,
        {"currentLocale", IMP(localec_currentLocale), "@16@0:8"},
        {"systemLocale", IMP(localec_currentLocale), "@16@0:8"},
        {"autoupdatingCurrentLocale", IMP(localec_currentLocale), "@16@0:8"},
        {"preferredLanguages", IMP(localec_preferredLanguages), "@16@0:8"});

    g_uuid = mm_objc_define_class("NSUUID", nsobject, sizeof(uuid_object), FND_CLASS_REF(NSUUID), FND_META_REF(NSUUID));
    register_lifecycle<uuid_object>(g_uuid);
    FND_METHODS(g_uuid,
        {"init", IMP(uuid_init), "@16@0:8"},
        {"UUIDString", IMP(uuid_UUIDString), "@16@0:8"},
        {"description", IMP(uuid_description), "@16@0:8"},
        {"getUUIDBytes:", IMP(uuid_getUUIDBytes), "v24@0:8^v16"});
    FND_CLASS_METHODS(g_uuid, {"UUID", IMP(uuidc_UUID), "@16@0:8"});

    g_runloop = mm_objc_define_class("NSRunLoop", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSRunLoop), FND_META_REF(NSRunLoop));
    FND_METHODS(g_runloop, {"run", IMP(runloop_run), "v16@0:8"}, {"stop", IMP(runloop_stop), "v16@0:8"});
    FND_CLASS_METHODS(g_runloop,
        {"currentRunLoop", IMP(runloopc_currentRunLoop), "@16@0:8"},
        {"mainRunLoop", IMP(runloopc_mainRunLoop), "@16@0:8"});

    g_pool = mm_objc_define_class("NSAutoreleasePool", nsobject, sizeof(pool_object), FND_CLASS_REF(NSAutoreleasePool), FND_META_REF(NSAutoreleasePool));
    FND_METHODS(g_pool, {"init", IMP(pool_init), "@16@0:8"}, {"drain", IMP(pool_drain), "v16@0:8"}, {"release", IMP(pool_drain), "v16@0:8"});

    g_thread = mm_objc_define_class("NSThread", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSThread), FND_META_REF(NSThread));
    FND_METHODS(g_thread, {"isMainThread", IMP(thread_isMainThread), "B16@0:8"});
    FND_CLASS_METHODS(g_thread,
        {"currentThread", IMP(threadc_currentThread), "@16@0:8"},
        {"isMainThread", IMP(thread_isMainThread), "B16@0:8"},
        {"sleepForTimeInterval:", IMP(threadc_sleepForTimeInterval), "v24@0:8d16"});

    g_apple_event_manager = mm_objc_define_class("NSAppleEventManager", nsobject, sizeof(apple_event_manager),
                                                FND_CLASS_REF(NSAppleEventManager), FND_META_REF(NSAppleEventManager));
    register_lifecycle<apple_event_manager>(g_apple_event_manager);
    FND_CLASS_METHODS(g_apple_event_manager, {"sharedAppleEventManager", IMP(aem_shared), "@16@0:8"});
    FND_METHODS(g_apple_event_manager,
        {"setEventHandler:andSelector:forEventClass:andEventID:", IMP(aem_setEventHandler), "v48@0:8@16:24I32I40"},
        {"removeEventHandler:forEventClass:andEventID:", IMP(aem_removeEventHandler), "v32@0:8I16I24"},
        {"removeEventHandlerForEventClass:andEventID:", IMP(aem_removeEventHandler), "v32@0:8I16I24"},
        {"currentAppleEvent", IMP(aem_nil), "@16@0:8"},
        {"currentReplyAppleEvent", IMP(aem_nil), "@16@0:8"},
        {"suspendCurrentAppleEvent", IMP(aem_nil), "^v16@0:8"});

    g_notification_center = mm_objc_define_class("NSNotificationCenter", nsobject, sizeof(center_object), FND_CLASS_REF(NSNotificationCenter), FND_META_REF(NSNotificationCenter));
    g_distributed_center = mm_objc_define_class("NSDistributedNotificationCenter", g_notification_center, sizeof(center_object),
                                                FND_CLASS_REF(NSDistributedNotificationCenter), FND_META_REF(NSDistributedNotificationCenter));
    for (Class c : {g_notification_center, g_distributed_center}) {
        register_lifecycle<center_object>(c);
        FND_METHODS(c,
            {"addObserver:selector:name:object:", IMP(center_addObserver), "v48@0:8@16:24@32@40"},
            {"addObserverForName:object:queue:usingBlock:", IMP(center_addObserverForName), "@48@0:8@16@24@32@40"},
            {"removeObserver:", IMP(center_removeObserver), "v24@0:8@16"},
            {"removeObserver:name:object:", IMP(center_removeObserverName), "v40@0:8@16@24@32"},
            {"postNotification:", IMP(center_postNotification), "v24@0:8@16"},
            {"postNotificationName:object:", IMP(center_postNotificationName), "v32@0:8@16@24"},
            {"postNotificationName:object:userInfo:", IMP(center_postNotificationNameUserInfo), "v40@0:8@16@24@32"},
            {"postNotificationName:object:userInfo:deliverImmediately:", IMP(center_postNotificationNameDeliver), "v48@0:8@16@24@32B40"},
            {"postNotificationName:object:userInfo:options:", IMP(center_postNotificationNameDeliver), "v48@0:8@16@24@32Q40"});
        FND_CLASS_METHODS(c, {"defaultCenter", IMP(centerc_defaultCenter), "@16@0:8"});
    }
    g_notification = mm_objc_define_class("NSNotification", nsobject, sizeof(notification_object), FND_CLASS_REF(NSNotification), FND_META_REF(NSNotification));
    register_lifecycle<notification_object>(g_notification);
    FND_METHODS(g_notification,
        {"name", IMP(notification_name), "@16@0:8"},
        {"object", IMP(notification_object_), "@16@0:8"},
        {"userInfo", IMP(notification_userInfo), "@16@0:8"});
    FND_CLASS_METHODS(g_notification,
        {"notificationWithName:object:", IMP(notificationc_notificationWithName), "@32@0:8@16@24"},
        {"notificationWithName:object:userInfo:", IMP(notificationc_notificationWithNameUserInfo), "@40@0:8@16@24@32"});

    g_exception = mm_objc_define_class("NSException", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSException), FND_META_REF(NSException));
    FND_METHODS(g_exception, {"raise", IMP(exception_raise), "v16@0:8"});
    FND_CLASS_METHODS(g_exception, {"raise:format:", IMP(exceptionc_raiseFormat), "v32@0:8@16@24"});

    /* Declared so the guest's class references resolve; behaviour comes later. */
    g_date = mm_objc_define_class("NSDate", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSDate), FND_META_REF(NSDate));
    g_timer = mm_objc_define_class("NSTimer", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSTimer), FND_META_REF(NSTimer));
    g_opqueue = mm_objc_define_class("NSOperationQueue", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSOperationQueue), FND_META_REF(NSOperationQueue));
    g_charset = mm_objc_define_class("NSCharacterSet", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSCharacterSet), FND_META_REF(NSCharacterSet));
}

} // namespace fnd

/* ---- C API ------------------------------------------------------------------- */

using namespace fnd;

extern "C" {

/* CFType base: every CF object is an objc object. */
id CFRetain_(id o) DARWIN(CFRetain);
id CFRetain_(id o) { return retain(o); }
void CFRelease_(id o) DARWIN(CFRelease);
void CFRelease_(id o) { release(o); }
id CFAutorelease_(id o) DARWIN(CFAutorelease);
id CFAutorelease_(id o) { return autorelease(o); }
CFIndex CFGetRetainCount_(id o) DARWIN(CFGetRetainCount);
CFIndex CFGetRetainCount_(id o) { return static_cast<CFIndex>(send(o, "retainCount")); }
CFTypeID CFGetTypeID_(id o) DARWIN(CFGetTypeID);
CFTypeID CFGetTypeID_(id o) { return o ? ret(mm_objc_object_class(o)) : 0; }
int CFEqual_(id a, id b) DARWIN(CFEqual);
int CFEqual_(id a, id b) { return objects_equal(a, b); }
std::uint64_t CFHash_(id o) DARWIN(CFHash);
std::uint64_t CFHash_(id o) { return object_hash(o); }
id CFCopyDescription_(id o) DARWIN(CFCopyDescription);
id CFCopyDescription_(id o) { return string_from_utf8(describe(o)); }
id CFCopyTypeIDDescription_(CFTypeID t) DARWIN(CFCopyTypeIDDescription);
id CFCopyTypeIDDescription_(CFTypeID t) { return string_from_utf8(mm_objc_class_name(reinterpret_cast<Class>(t))); }
void *CFAllocatorAllocate_(void *, CFIndex size, std::uint64_t) DARWIN(CFAllocatorAllocate);
void *CFAllocatorAllocate_(void *, CFIndex size, std::uint64_t) { return std::malloc(static_cast<std::size_t>(size)); }
void CFAllocatorDeallocate_(void *, void *p) DARWIN(CFAllocatorDeallocate);
void CFAllocatorDeallocate_(void *, void *p) { std::free(p); }
void *CFGetAllocator_(id) DARWIN(CFGetAllocator);
void *CFGetAllocator_(id) { return kCFAllocatorSystemDefault_ptr; }

/* CFBundle: the same objects NSBundle uses. */
id CFBundleGetMainBundle_() DARWIN(CFBundleGetMainBundle);
id CFBundleGetMainBundle_() { return main_bundle(); }
id CFBundleGetInfoDictionary_(id b) DARWIN(CFBundleGetInfoDictionary);
id CFBundleGetInfoDictionary_(id b) { return obj(bundle_infoDictionary(b, nullptr)); }
id CFBundleGetValueForInfoDictionaryKey_(id b, id key) DARWIN(CFBundleGetValueForInfoDictionaryKey);
id CFBundleGetValueForInfoDictionaryKey_(id b, id key) { return obj(bundle_objectForInfoDictionaryKey(b, nullptr, key)); }
id CFBundleCopyBundleURL_(id b) DARWIN(CFBundleCopyBundleURL);
id CFBundleCopyBundleURL_(id b) { return url_from_path(bun(b)->path, true); }
id CFBundleCopyExecutableURL_(id) DARWIN(CFBundleCopyExecutableURL);
id CFBundleCopyExecutableURL_(id) { return url_from_path(executable_path(), false); }
id CFBundleCopyResourcesDirectoryURL_(id b) DARWIN(CFBundleCopyResourcesDirectoryURL);
id CFBundleCopyResourcesDirectoryURL_(id b) { return url_from_path(bun(b)->path + "/Contents/Resources", true); }
id CFBundleCreate_(void *, id url) DARWIN(CFBundleCreate);
id CFBundleCreate_(void *, id url) { return make_bundle(url_path(url)); }
id CFBundleGetBundleWithIdentifier_(id ident) DARWIN(CFBundleGetBundleWithIdentifier);
id CFBundleGetBundleWithIdentifier_(id ident) {
    id b = main_bundle();
    id own = obj(bundle_bundleIdentifier(b, nullptr));
    return own && objects_equal(own, ident) ? b : nullptr;
}
id CFBundleCopyInfoDictionaryInDirectory_(id url) DARWIN(CFBundleCopyInfoDictionaryInDirectory);
id CFBundleCopyInfoDictionaryInDirectory_(id url) { return load_info_plist(url_path(url)); }
id CFBundleCopyResourceURL_(id b, id name, id type, id dir) DARWIN(CFBundleCopyResourceURL);
id CFBundleCopyResourceURL_(id b, id name, id type, id dir) {
    const std::uint64_t p = bundle_pathForResourceOfTypeInDirectory(b, nullptr, name, type, dir);
    return p ? url_from_path(to_utf8(obj(p)), false) : nullptr;
}
int CFBundleLoadExecutable_(id) DARWIN(CFBundleLoadExecutable);
int CFBundleLoadExecutable_(id) { return 1; }
int CFBundleLoadExecutableAndReturnError_(id, id *) DARWIN(CFBundleLoadExecutableAndReturnError);
int CFBundleLoadExecutableAndReturnError_(id, id *) { return 1; }
int CFBundleIsExecutableLoaded_(id) DARWIN(CFBundleIsExecutableLoaded);
int CFBundleIsExecutableLoaded_(id) { return 1; }
void *CFBundleGetFunctionPointerForName_(id, id name) DARWIN(CFBundleGetFunctionPointerForName);
void *CFBundleGetFunctionPointerForName_(id, id name) {
    if (g_trace) std::fprintf(stderr, "Foundation: CFBundleGetFunctionPointerForName(%s) -> null\n", describe(name).c_str());
    return nullptr;
}
CFIndex CFBundleOpenBundleResourceMap_(id) DARWIN(CFBundleOpenBundleResourceMap);
CFIndex CFBundleOpenBundleResourceMap_(id) { return -1; }
void CFBundleCloseBundleResourceMap_(id, CFIndex) DARWIN(CFBundleCloseBundleResourceMap);
void CFBundleCloseBundleResourceMap_(id, CFIndex) {}

/* CFURL */
id CFURLCreateWithFileSystemPath_(void *, id path, std::uint64_t, int is_dir) DARWIN(CFURLCreateWithFileSystemPath);
id CFURLCreateWithFileSystemPath_(void *, id path, std::uint64_t, int is_dir) { return url_from_path(to_utf8(path), is_dir != 0); }
id CFURLCreateFromFileSystemRepresentation_(void *, const std::uint8_t *p, CFIndex n, int is_dir) DARWIN(CFURLCreateFromFileSystemRepresentation);
id CFURLCreateFromFileSystemRepresentation_(void *, const std::uint8_t *p, CFIndex n, int is_dir) {
    return url_from_path(std::string(reinterpret_cast<const char *>(p), static_cast<std::size_t>(n)), is_dir != 0);
}
id CFURLCreateWithString_(void *, id s, id base) DARWIN(CFURLCreateWithString);
id CFURLCreateWithString_(void *, id s, id base) {
    (void)base;
    id u2 = mm_objc_alloc(reinterpret_cast<Class>(cls("NSURL")), 0);
    return obj(url_initWithString(u2, nullptr, s));
}
id CFURLCopyFileSystemPath_(id url, std::uint64_t) DARWIN(CFURLCopyFileSystemPath);
id CFURLCopyFileSystemPath_(id url, std::uint64_t) { return string_from_utf8(url_path(url)); }
id CFURLCopyPath_(id url) DARWIN(CFURLCopyPath);
id CFURLCopyPath_(id url) { return string_from_utf8(url_path(url)); }
id CFURLGetString_(id url) DARWIN(CFURLGetString);
id CFURLGetString_(id url) { return obj(url_absoluteString(url, nullptr)); }
int CFURLGetFileSystemRepresentation_(id url, int, std::uint8_t *buf, CFIndex max) DARWIN(CFURLGetFileSystemRepresentation);
int CFURLGetFileSystemRepresentation_(id url, int, std::uint8_t *buf, CFIndex max) {
    const std::string p = url_path(url);
    if (static_cast<CFIndex>(p.size()) + 1 > max) return 0;
    std::memcpy(buf, p.c_str(), p.size() + 1);
    return 1;
}
int CFURLGetFSRef_(id, void *) DARWIN(CFURLGetFSRef);
int CFURLGetFSRef_(id, void *) { return 0; } /* Carbon FSRef: unsupported, callers fall back to paths */
id CFURLCreateCopyAppendingPathComponent_(void *, id url, id comp, int is_dir) DARWIN(CFURLCreateCopyAppendingPathComponent);
id CFURLCreateCopyAppendingPathComponent_(void *, id url, id comp, int is_dir) {
    std::string p = url_path(url);
    if (!p.empty() && p.back() != '/') p += '/';
    p += to_utf8(comp);
    return url_from_path(p, is_dir != 0);
}
id CFURLCreateCopyDeletingLastPathComponent_(void *, id url) DARWIN(CFURLCreateCopyDeletingLastPathComponent);
id CFURLCreateCopyDeletingLastPathComponent_(void *, id url) { return retain(obj(url_URLByDeletingLastPathComponent(url, nullptr))); }
int CFURLCopyResourcePropertyForKey_(id url, id key, void *out, id *) DARWIN(CFURLCopyResourcePropertyForKey);
int CFURLCopyResourcePropertyForKey_(id url, id key, void *out, id *) {
    const std::string k = describe(key);
    struct stat st;
    if (stat(url_path(url).c_str(), &st) != 0) return 0;
    id value = nullptr;
    if (k == "NSURLIsDirectoryKey") value = retain(number_bool(S_ISDIR(st.st_mode)));
    else if (k == "NSURLIsHiddenKey") { const std::string p = url_path(url); const auto slash = p.rfind('/'); value = retain(number_bool(slash + 1 < p.size() && p[slash + 1] == '.')); }
    else if (k == "NSURLIsPackageKey") value = retain(number_bool(url_path(url).ends_with(".app") || url_path(url).ends_with(".bundle")));
    else if (k == "NSURLNameKey") value = string_from_utf8(url_path(url));
    else if (k == "NSURLVolumeIdentifierKey" || k == "NSURLVolumeIsLocalKey") value = number_from_int64(static_cast<std::int64_t>(st.st_dev));
    else return 0;
    if (out) std::memcpy(out, &value, 8);
    return 1;
}
int CFURLSetResourcePropertyForKey_(id, id, id, id *) DARWIN(CFURLSetResourcePropertyForKey);
int CFURLSetResourcePropertyForKey_(id, id, id, id *) { return 1; }
int CFURLCreateDataAndPropertiesFromResource_(void *, id url, id *data, id *props, id, std::int32_t *code) DARWIN(CFURLCreateDataAndPropertiesFromResource);
int CFURLCreateDataAndPropertiesFromResource_(void *, id url, id *data, id *props, id, std::int32_t *code) {
    FILE *f = std::fopen(url_path(url).c_str(), "rb");
    if (!f) { if (code) *code = 260; return 0; }
    std::string bytes;
    char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.append(buf, n);
    std::fclose(f);
    if (data) *data = data_create(bytes.data(), static_cast<CFIndex>(bytes.size()), false);
    if (props) *props = dictionary_create(false);
    if (code) *code = 0;
    return 1;
}

/* CFError */
CFTypeID CFErrorGetTypeID_() DARWIN(CFErrorGetTypeID);
CFTypeID CFErrorGetTypeID_() { return ret(cls("NSError")); }
id CFErrorCreate_(void *, id domain, std::int64_t code, id info) DARWIN(CFErrorCreate);
id CFErrorCreate_(void *, id domain, std::int64_t code, id info) { return error_create(domain, code, info); }
id CFErrorGetDomain_(id e) DARWIN(CFErrorGetDomain);
id CFErrorGetDomain_(id e) { return obj(send(e, "domain")); }
std::int64_t CFErrorGetCode_(id e) DARWIN(CFErrorGetCode);
std::int64_t CFErrorGetCode_(id e) { return static_cast<std::int64_t>(send(e, "code")); }
id CFErrorCopyDescription_(id e) DARWIN(CFErrorCopyDescription);
id CFErrorCopyDescription_(id e) { return retain(obj(send(e, "localizedDescription"))); }
id CFErrorCopyUserInfo_(id e) DARWIN(CFErrorCopyUserInfo);
id CFErrorCopyUserInfo_(id e) { return retain(obj(send(e, "userInfo"))); }

/* CFPreferences */
id CFPreferencesCopyAppValue_(id key, id) DARWIN(CFPreferencesCopyAppValue);
id CFPreferencesCopyAppValue_(id key, id) { id v = prefs().get(key); return v ? retain(v) : nullptr; }
id CFPreferencesCopyValue_(id key, id, id, id) DARWIN(CFPreferencesCopyValue);
id CFPreferencesCopyValue_(id key, id, id, id) { id v = prefs().get(key); return v ? retain(v) : nullptr; }
void CFPreferencesSetAppValue_(id key, id value, id) DARWIN(CFPreferencesSetAppValue);
void CFPreferencesSetAppValue_(id key, id value, id) { prefs().set(key, value); }
void CFPreferencesSetValue_(id key, id value, id, id, id) DARWIN(CFPreferencesSetValue);
void CFPreferencesSetValue_(id key, id value, id, id, id) { prefs().set(key, value); }
int CFPreferencesAppSynchronize_(id) DARWIN(CFPreferencesAppSynchronize);
int CFPreferencesAppSynchronize_(id) { return prefs().sync(); }
int CFPreferencesSynchronize_(id, id, id) DARWIN(CFPreferencesSynchronize);
int CFPreferencesSynchronize_(id, id, id) { return prefs().sync(); }
int CFPreferencesGetAppBooleanValue_(id key, id, int *exists) DARWIN(CFPreferencesGetAppBooleanValue);
int CFPreferencesGetAppBooleanValue_(id key, id, int *exists) {
    id v = prefs().get(key);
    if (exists) *exists = v != nullptr;
    return v && is_number(v) ? number_int64(v) != 0 : 0;
}
CFIndex CFPreferencesGetAppIntegerValue_(id key, id, int *exists) DARWIN(CFPreferencesGetAppIntegerValue);
CFIndex CFPreferencesGetAppIntegerValue_(id key, id, int *exists) {
    id v = prefs().get(key);
    if (exists) *exists = v != nullptr;
    return v && is_number(v) ? static_cast<CFIndex>(number_int64(v)) : 0;
}

/* CFLocale */
CFTypeID CFLocaleGetTypeID_() DARWIN(CFLocaleGetTypeID);
CFTypeID CFLocaleGetTypeID_() { return ret(cls("NSLocale")); }
id CFLocaleCopyCurrent_() DARWIN(CFLocaleCopyCurrent);
id CFLocaleCopyCurrent_() { return retain(current_locale()); }
id CFLocaleGetSystem_() DARWIN(CFLocaleGetSystem);
id CFLocaleGetSystem_() { return current_locale(); }
id CFLocaleGetValue_(id, id key) DARWIN(CFLocaleGetValue);
id CFLocaleGetValue_(id, id key) { return obj(locale_objectForKey(nullptr, nullptr, key)); }
id CFLocaleGetIdentifier_(id) DARWIN(CFLocaleGetIdentifier);
id CFLocaleGetIdentifier_(id) { return constant_string("en_US"); }

/* CFUUID */
struct cfuuid_bytes { std::uint8_t b[16]; };
id CFUUIDCreate_(void *) DARWIN(CFUUIDCreate);
id CFUUIDCreate_(void *) {
    id u2 = mm_objc_alloc(reinterpret_cast<Class>(cls("NSUUID")), 0);
    return obj(uuid_init(u2, nullptr));
}
id CFUUIDCreateString_(void *, id u2) DARWIN(CFUUIDCreateString);
id CFUUIDCreateString_(void *, id u2) { return retain(obj(send(u2, "UUIDString"))); }
cfuuid_bytes CFUUIDGetUUIDBytes_(id u2) DARWIN(CFUUIDGetUUIDBytes);
cfuuid_bytes CFUUIDGetUUIDBytes_(id u2) { cfuuid_bytes out{}; send(u2, "getUUIDBytes:", ret(out.b)); return out; }

/* CFRunLoop: the same loops NSRunLoop uses. */
id CFRunLoopGetCurrent_() DARWIN(CFRunLoopGetCurrent);
id CFRunLoopGetCurrent_() { return loop_object(loop_for_current_thread()); }
id CFRunLoopGetMain_() DARWIN(CFRunLoopGetMain);
id CFRunLoopGetMain_() { return loop_object(g_main_loop ? g_main_loop : loop_for_current_thread()); }
void CFRunLoopRun_() DARWIN(CFRunLoopRun);
void CFRunLoopRun_() { runloop_run(loop_object(loop_for_current_thread()), nullptr); }
std::int32_t CFRunLoopRunInMode_(id, double seconds, int) DARWIN(CFRunLoopRunInMode);
std::int32_t CFRunLoopRunInMode_(id, double seconds, int) {
    /* Blocks sent to the main dispatch queue run here, on the thread that
     * owns the loop, as they do on macOS. */
    dispatch_drain_main_queue();
    run_loop *l = loop_for_current_thread();
    run_loop_drain(l);
    if (seconds > 0) {
        std::unique_lock lock(l->mu);
        if (l->blocks.empty() && !l->stopped) l->cv.wait_for(lock, std::chrono::duration<double>(seconds));
    }
    run_loop_drain(l);
    std::lock_guard lock(l->mu);
    if (l->stopped) { l->stopped = false; return 2; } /* kCFRunLoopRunStopped */
    return 3;                                          /* kCFRunLoopRunTimedOut */
}
void CFRunLoopStop_(id loop) DARWIN(CFRunLoopStop);
void CFRunLoopStop_(id loop) { runloop_stop(loop, nullptr); }
void CFRunLoopWakeUp_(id loop) DARWIN(CFRunLoopWakeUp);
void CFRunLoopWakeUp_(id loop) { loop_of(loop)->cv.notify_all(); }
void CFRunLoopPerformBlock_(id loop, id, void *block) DARWIN(CFRunLoopPerformBlock);
void CFRunLoopPerformBlock_(id loop, id, void *block) {
    run_loop *l = loop_of(loop);
    { std::lock_guard lock(l->mu); l->blocks.emplace_back(block, true); }
    l->cv.notify_all();
}
int CFRunLoopIsWaiting_(id) DARWIN(CFRunLoopIsWaiting);
int CFRunLoopIsWaiting_(id) { return 0; }

/* NSLog and the Foundation free functions. */
void NSLog_(id fmt, ...) DARWIN(NSLog);
void NSLog_(id fmt, ...) {
    id s = obj(send(reinterpret_cast<id>(cls("NSString")), "stringWithFormat:", ret(fmt)));
    std::fprintf(stderr, "%s\n", describe(s).c_str());
}
void NSLogv_(id fmt, std::uint64_t va) DARWIN(NSLogv);
void NSLogv_(id fmt, std::uint64_t va) {
    id s = obj(send(reinterpret_cast<id>(cls("NSString")), "alloc"));
    s = obj(send(s, "initWithFormat:arguments:", ret(fmt), va));
    std::fprintf(stderr, "%s\n", describe(s).c_str());
    release(s);
}
id NSStringFromClass_(Class c) DARWIN(NSStringFromClass);
id NSStringFromClass_(Class c) { return c ? autorelease(string_from_utf8(mm_objc_class_name(c))) : nullptr; }
Class NSClassFromString_(id name) DARWIN(NSClassFromString);
Class NSClassFromString_(id name) { return mm_objc_class_named(to_utf8(name).c_str()); }
id NSStringFromSelector_(SEL s) DARWIN(NSStringFromSelector);
id NSStringFromSelector_(SEL s) { return s ? autorelease(string_from_utf8(s)) : nullptr; }
SEL NSSelectorFromString_(id name) DARWIN(NSSelectorFromString);
SEL NSSelectorFromString_(id name) { return mm_objc_sel(to_utf8(name).c_str()); }
id NSHomeDirectory_() DARWIN(NSHomeDirectory);
id NSHomeDirectory_() { return autorelease(string_from_utf8(home_directory())); }
id NSTemporaryDirectory_() DARWIN(NSTemporaryDirectory);
id NSTemporaryDirectory_() { const char *t = std::getenv("TMPDIR"); return autorelease(string_from_utf8(t ? t : "/tmp")); }
id NSUserName_() DARWIN(NSUserName);
id NSUserName_() { const passwd *pw = getpwuid(getuid()); return autorelease(string_from_utf8(pw && pw->pw_name ? pw->pw_name : "user")); }
id NSFullUserName_() DARWIN(NSFullUserName);
id NSFullUserName_() { return NSUserName_(); }
id NSSearchPathForDirectoriesInDomains_(std::uint64_t directory, std::uint64_t, int expand) DARWIN(NSSearchPathForDirectoriesInDomains);
id NSSearchPathForDirectoriesInDomains_(std::uint64_t directory, std::uint64_t, int expand) {
    (void)expand;
    return fnd::search_path_directories(directory);
}

} // extern "C"

namespace fnd {
id search_path_directories(std::uint64_t directory) {
    const std::string home = home_directory();
    std::string p;
    switch (directory) {
    case 5:  p = home + "/Library/Application Support"; break; /* NSApplicationSupportDirectory is 14 on macOS; 5 = NSLibraryDirectory */
    case 9:  p = home + "/Documents"; break;
    case 10: p = home + "/Desktop"; break;
    case 13: p = home + "/Library/Caches"; break;
    case 14: p = home + "/Library/Application Support"; break;
    case 15: p = home + "/Downloads"; break;
    case 99: p = home; break;
    default: p = home + "/Library"; break;
    }
    id s = string_from_utf8(p);
    id a = array_create(&s, 1, false);
    release(s);
    return autorelease(a);
}
} // namespace fnd

extern "C" {

/* Geometry: NSRect/NSPoint/NSSize are four/two doubles in v0-v3. */
mm_two_doubles NSMakePoint_(double x, double y) DARWIN(NSMakePoint);
mm_two_doubles NSMakePoint_(double x, double y) { return {x, y}; }
int NSEqualPoints_(double ax, double ay, double bx, double by) DARWIN(NSEqualPoints);
int NSEqualPoints_(double ax, double ay, double bx, double by) { return ax == bx && ay == by; }
int NSEqualSizes_(double aw, double ah, double bw, double bh) DARWIN(NSEqualSizes);
int NSEqualSizes_(double aw, double ah, double bw, double bh) { return aw == bw && ah == bh; }
int NSEqualRects_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) DARWIN(NSEqualRects);
int NSEqualRects_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) {
    return ax == bx && ay == by && aw == bw && ah == bh;
}
int NSPointInRect_(double px, double py, double x, double y, double w, double h) DARWIN(NSPointInRect);
int NSPointInRect_(double px, double py, double x, double y, double w, double h) { return px >= x && py >= y && px < x + w && py < y + h; }
int NSContainsRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) DARWIN(NSContainsRect);
int NSContainsRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) {
    return bx >= ax && by >= ay && bx + bw <= ax + aw && by + bh <= ay + ah;
}
int NSIntersectsRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) DARWIN(NSIntersectsRect);
int NSIntersectsRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}
mm_two_doubles NSIntersectionRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) DARWIN(NSIntersectionRect);
mm_two_doubles NSIntersectionRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) {
    const double x = std::max(ax, bx), y = std::max(ay, by);
    const double r = std::min(ax + aw, bx + bw), t = std::min(ay + ah, by + bh);
    if (r <= x || t <= y) { mm_objc_return_doubles_hi(0, 0); return {0, 0}; }
    mm_objc_return_doubles_hi(r - x, t - y);
    return {x, y};
}
mm_two_doubles NSUnionRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) DARWIN(NSUnionRect);
mm_two_doubles NSUnionRect_(double ax, double ay, double aw, double ah, double bx, double by, double bw, double bh) {
    if (aw <= 0 || ah <= 0) { mm_objc_return_doubles_hi(bw, bh); return {bx, by}; }
    if (bw <= 0 || bh <= 0) { mm_objc_return_doubles_hi(aw, ah); return {ax, ay}; }
    const double x = std::min(ax, bx), y = std::min(ay, by);
    mm_objc_return_doubles_hi(std::max(ax + aw, bx + bw) - x, std::max(ay + ah, by + bh) - y);
    return {x, y};
}
/* NSZeroPoint/NSZeroRect/NSZeroSize are data symbols. */
EXPORT double NSZeroPoint_[2] __asm__("_NSZeroPoint") = {0, 0};
EXPORT double NSZeroSize_[2] __asm__("_NSZeroSize") = {0, 0};
EXPORT double NSZeroRect_[4] __asm__("_NSZeroRect") = {0, 0, 0, 0};

/* CFUserNotification: the GUI is not up yet, so alerts go to stderr. */
std::int32_t CFUserNotificationDisplayAlert_(double, std::uint32_t, id, id, id, id header, id message, id, id, id, std::uint64_t *response) DARWIN(CFUserNotificationDisplayAlert);
std::int32_t CFUserNotificationDisplayAlert_(double, std::uint32_t, id, id, id, id header, id message, id, id, id, std::uint64_t *response) {
    std::fprintf(stderr, "alert: %s: %s\n", describe(header).c_str(), describe(message).c_str());
    if (g_trace && g_api && g_api->print_guest_backtrace) g_api->print_guest_backtrace();
    if (response) *response = 0;
    return 0;
}

EXPORT void machoman_dylib_init(const mm_host_api *api_) {
    g_api = api_;
    g_trace = std::getenv("MACHOMAN_TRACE") != nullptr;
    /* The thread that links the process is the guest's main thread. */
    g_main_loop = loop_for_current_thread();
}

static void foundation_ctor() __attribute__((constructor));
static void foundation_ctor() {
    g_trace = std::getenv("MACHOMAN_TRACE") != nullptr;
    g_api = mm_objc_host_api(); /* null until the runtime calls machoman_dylib_init */
    /* Order matters: strings first (everything else makes constant strings). */
    register_strings();
    register_collections();
    register_misc();
    register_plists();
    register_task();
}

} // extern "C"

/* Foundation answers for the constants and classes it has not implemented
 * yet, the same way the stub framework does for a framework as a whole. */
extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}
