/* WKWebView is a real NSView. Chromium lives in an isolated host helper;
 * only the main Cocoa queue sends messages back into guest delegates. */
#include "appkit.h"
#include "machoman/json.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
MM_FW_CLASS(WKWebView); MM_FW_CLASS(WKWebViewConfiguration);
MM_FW_CLASS(WKWebsiteDataStore); MM_FW_CLASS(WKHTTPCookieStore);
MM_FW_CLASS(WKUserContentController); MM_FW_CLASS(WKUserScript);
MM_FW_CLASS(WKScriptMessage); MM_FW_CLASS(WKNavigation); MM_FW_CLASS(WKPreferences);

namespace ak {
namespace {
using json_object = mm_json::object;
using json_array = mm_json::array;
mm_json num(std::uint64_t n) { return mm_json(mm_json_number{std::to_string(n)}); }
const mm_json &field(const mm_json &o, const char *key) {
    static const mm_json missing;
    const auto *p = std::get_if<json_object>(&o.value);
    if (!p) return missing;
    auto it = p->find(key); return it == p->end() ? missing : it->second;
}
std::string text(const mm_json &j) {
    const auto *p = std::get_if<std::string>(&j.value); return p ? *p : std::string();
}
std::uint64_t integer(const mm_json &j) {
    const auto *n = std::get_if<mm_json_number>(&j.value);
    return n ? std::strtoull(n->text.c_str(), nullptr, 10) : 0;
}
bool truth(const mm_json &j) { const auto *b = std::get_if<bool>(&j.value); return b && *b; }
id array() { return obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array")); }
id dictionary() { return obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary")); }
void dictionary_put(id d, const char *key, id value) {
    if (value) send(d, "setObject:forKey:", ret(value), ret(string(key)));
}
id from_json(const mm_json &j) {
    if (const auto *s = std::get_if<std::string>(&j.value)) return string(*s);
    if (const auto *b = std::get_if<bool>(&j.value))
        return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", *b));
    if (const auto *n = std::get_if<mm_json_number>(&j.value)) {
        if (n->text.find_first_of(".eE") != std::string::npos) return number(std::strtod(n->text.c_str(), nullptr));
        return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithLongLong:", std::strtoull(n->text.c_str(), nullptr, 10)));
    }
    if (const auto *a = std::get_if<json_array>(&j.value)) {
        id out = array(); for (const auto &v : *a) send(out, "addObject:", ret(from_json(v))); return out;
    }
    if (const auto *o = std::get_if<json_object>(&j.value)) {
        id out = dictionary();
        for (const auto &[key, v] : *o) send(out, "setObject:forKey:", ret(from_json(v)), ret(string(key)));
        return out;
    }
    return obj(send(reinterpret_cast<id>(cls("NSNull")), "null"));
}
id error(std::int64_t code, const std::string &description) {
    id info = dictionary(); dictionary_put(info, "NSLocalizedDescription", string(description));
    return obj(send(reinterpret_cast<id>(cls("NSError")), "errorWithDomain:code:userInfo:",
        ret(string("WKErrorDomain")), static_cast<std::uint64_t>(code), ret(info)));
}
struct object {
    mm_objc_object base;
    std::map<std::string, id> values;
    ~object() { for (const auto &[key, value] : values) release(value); }
};
id get(id self, const char *key) {
    auto &values = as<object>(self)->values;
    const auto it = values.find(key); return it == values.end() ? nullptr : it->second;
}
void put(id self, const char *key, id value) {
    auto &slot = as<object>(self)->values[key];
    retain(value); release(slot); slot = value;
}
id make_object(const char *name) { return obj(send(reinterpret_cast<id>(cls(name)), "new")); }
std::uint64_t copy_object(id self, SEL, void *) {
    id copy = make_object(mm_objc_class_name(self->isa));
    for (const auto &[key, value] : as<object>(self)->values) put(copy, key.c_str(), value);
    return ret(copy);
}
void property(Class c, const char *key, const char *setter) {
    // Accessors use their actual selector as the key, and derive the setter key.
    mm_objc_add_method(c, key, MM_IMP(+[](id s, SEL sel) { return ret(get(s, sel)); }), "@16@0:8");
    mm_objc_add_method(c, setter, MM_IMP(+[](id s, SEL sel, id value) -> std::uint64_t {
        std::string key(sel + 3); key.pop_back();
        key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
        put(s, key.c_str(), value); return 0;
    }), "v24@0:8@16");
}
Class plain(const char *name, mm_objc_class *storage, mm_objc_class *meta) {
    Class c = mm_objc_define_class(name, cls("NSObject"), sizeof(object), storage, meta);
    register_lifecycle<object>(c);
    MM_METHODS(c, {"copyWithZone:", MM_IMP(copy_object), "@24@0:8^v16"});
    return c;
}

using dispatch_fn = void (*)(void *, void *, void *);
using main_queue_fn = void *(*)();
dispatch_fn dispatch = nullptr;
main_queue_fn main_queue = nullptr;
struct task { void (*run)(task *); virtual ~task() = default; };
void run_task(std::uint64_t pointer) {
    std::unique_ptr<task> item(reinterpret_cast<task *>(pointer));
    autorelease_pool pool; item->run(item.get());
}
void post(task *item) {
    if (dispatch && main_queue) dispatch(main_queue(), item, reinterpret_cast<void *>(run_task));
    else { std::fprintf(stderr, "WebKit: Cocoa main queue is unavailable\n"); delete item; }
}
struct block_task : task {
    id block; std::vector<id> objects;
    block_task(id b, std::initializer_list<id> args) : block(retain(b)), objects(args) {
        for (id value : objects) retain(value);
        run = +[](task *t) {
            auto *self = static_cast<block_task *>(t);
            std::vector<std::uint64_t> words;
            for (id value : self->objects) words.push_back(ret(value));
            mm_objc_block_invoke(self->block, static_cast<int>(words.size()), words.data());
        };
    }
    ~block_task() { release(block); for (id value : objects) release(value); }
};
void complete(id block, std::initializer_list<id> args = {}) { if (block) post(new block_task(block, args)); }

struct cookie_store {
    mm_objc_object base;
    std::vector<id> cookies;
    bool persistent = false;
    ~cookie_store() { for (id c : cookies) release(c); }
};
id cocoa_cookies() { return obj(send(reinterpret_cast<id>(cls("NSHTTPCookieStorage")), "sharedHTTPCookieStorage")); }
bool same_cookie(id a, id b) {
    for (const char *key : {"name", "domain", "path"})
        if (to_utf8(obj(send(a, key))) != to_utf8(obj(send(b, key)))) return false;
    return true;
}
void save_cookie(id store, id cookie, bool remove) {
    if (!cookie) return;
    auto *s = as<cookie_store>(store);
    auto it = std::find_if(s->cookies.begin(), s->cookies.end(), [&](id c) { return same_cookie(c, cookie); });
    if (it != s->cookies.end()) { release(*it); s->cookies.erase(it); }
    if (!remove) s->cookies.push_back(retain(cookie));
    if (s->persistent) send(cocoa_cookies(), remove ? "deleteCookie:" : "setCookie:", ret(cookie));
}
mm_json encode_cookie(id cookie) {
    json_object out;
    for (const auto &[property, key] : {std::pair{"name", "Name"}, {"value", "Value"},
        {"domain", "Domain"}, {"path", "Path"}})
        out[key] = mm_json(to_utf8(obj(send(cookie, property))));
    out["Secure"] = mm_json(send(cookie, "isSecure") != 0);
    out["HTTPOnly"] = mm_json(send(cookie, "isHTTPOnly") != 0);
    if (id date = obj(send(cookie, "expiresDate"))) {
        // NSDate's interval is a floating return; read it from the runtime context.
        send(date, "timeIntervalSince1970");
        if (auto *ctx = api()->current_context()) {
            double time; std::memcpy(&time, &ctx->v[0][0], 8);
            out["Expires"] = mm_json(mm_json_number{std::to_string(time)});
        }
    }
    return mm_json(std::move(out));
}
id decode_cookie(const mm_json &value) {
    id props = dictionary();
    for (const char *key : {"Name", "Value", "Domain", "Path", "Secure", "HTTPOnly"})
        dictionary_put(props, key, from_json(field(value, key)));
    if (std::get_if<mm_json_number>(&field(value, "Expires").value)) {
        double time = std::strtod(std::get<mm_json_number>(field(value, "Expires").value).text.c_str(), nullptr);
        if (auto *ctx = api()->current_context()) std::memcpy(&ctx->v[0][0], &time, 8);
        dictionary_put(props, "Expires", obj(send(reinterpret_cast<id>(cls("NSDate")), "dateWithTimeIntervalSince1970:")));
    }
    return obj(send(reinterpret_cast<id>(cls("NSHTTPCookie")), "cookieWithProperties:", ret(props)));
}
id default_store = nullptr;
id data_store(bool persistent) {
    if (persistent && default_store) return default_store;
    id store = make_object("WKWebsiteDataStore");
    id cookies = make_object("WKHTTPCookieStore");
    as<cookie_store>(cookies)->persistent = persistent;
    if (persistent) {
        id all = obj(send(cocoa_cookies(), "cookies"));
        for (std::uint64_t i = 0, n = send(all, "count"); i < n; ++i)
            as<cookie_store>(cookies)->cookies.push_back(retain(obj(send(all, "objectAtIndex:", i))));
    }
    put(store, "httpCookieStore", cookies); release(cookies);
    put(store, "persistent", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", persistent)));
    if (persistent) default_store = store;
    else autorelease(store);
    return store;
}
id config_child(id config, const char *key, const char *type) {
    id child = get(config, key);
    if (!child) {
        child = make_object(type); put(config, key, child); release(child);
    }
    return child;
}
id config_store(id config) {
    id store = get(config, "websiteDataStore");
    if (!store) { store = data_store(true); put(config, "websiteDataStore", store); }
    return store;
}

struct browser;
struct webview {
    view_object view;
    id configuration = nullptr, navigation_delegate = nullptr, ui_delegate = nullptr;
    id navigation = nullptr, url = nullptr, title = nullptr;
    bool loading = false;
    std::shared_ptr<browser> process;
    std::map<std::uint64_t, id> completions;
    std::uint64_t serial = 0;
    ~webview();
};
struct event_task;
struct browser : std::enable_shared_from_this<browser> {
    id owner = nullptr; // main queue only, cleared before the view is destroyed
    pid_t pid = -1;
    int socket = -1;
    std::thread reader;
    std::mutex write_mutex;
    std::atomic<bool> stopping = false;
    bool send_json(const mm_json &command) {
        if (std::getenv("MACHOMAN_WEBKIT_TRACE"))
            std::fprintf(stderr, "WebKit: sending %s\n", text(field(command, "op")).c_str());
        // The manifest serializer pretty-prints JSON and escapes all control
        // characters in strings. Strip its formatting newlines for this wire
        // protocol, which uses one complete JSON object per line.
        std::string line = mm_json_serialize(command);
        std::erase(line, '\n'); line += '\n';
        std::lock_guard lock(write_mutex);
        std::size_t written = 0;
        while (socket >= 0 && written < line.size()) {
            const auto n = ::send(socket, line.data() + written, line.size() - written, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            written += static_cast<std::size_t>(n);
        }
        return written == line.size();
    }
    void stop() {
        if (stopping.exchange(true)) return;
        owner = nullptr;
        send_json(mm_json(json_object{{"op", mm_json(std::string("quit"))}}));
        {
            std::lock_guard lock(write_mutex);
            if (socket >= 0) ::shutdown(socket, SHUT_RDWR);
        }
        if (reader.joinable()) reader.join();
        if (socket >= 0) { ::close(socket); socket = -1; }
        if (pid > 0) {
            int status;
            for (int i = 0; i < 20; ++i) {
                if (::waitpid(pid, &status, WNOHANG) != 0) { pid = -1; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, &status, 0); pid = -1; }
        }
    }
    ~browser() { stop(); }
    void read_events();
};
std::vector<std::weak_ptr<browser>> browsers;
webview::~webview() {
    if (process) process->stop();
    release(configuration); release(navigation); release(url); release(title);
    for (auto &[key, block] : completions) release(block);
}
id view_cookie_store(webview *v) { return get(config_store(v->configuration), "httpCookieStore"); }
void notify(webview *v, id self, const char *selector, id arg) {
    if (responds(v->navigation_delegate, selector))
        send(v->navigation_delegate, selector, ret(self), ret(arg));
}
void navigation_error(id self, id e) {
    auto *v = as<webview>(self); v->loading = false;
    if (responds(v->navigation_delegate, "webView:didFailProvisionalNavigation:withError:"))
        send(v->navigation_delegate, "webView:didFailProvisionalNavigation:withError:", ret(self), ret(v->navigation), ret(e));
    std::fprintf(stderr, "WebKit: navigation failed: %s\n", to_utf8(obj(send(e, "localizedDescription"))).c_str());
}
void handle_event(const std::shared_ptr<browser> &process, const mm_json &event) {
    id self = retain(process->owner);
    if (!self) return;
    auto *v = as<webview>(self);
    const std::string name = text(field(event, "event"));
    if (std::getenv("MACHOMAN_WEBKIT_TRACE")) std::fprintf(stderr, "WebKit: main queue event %s\n", name.c_str());
    if (name == "started") { v->loading = true; notify(v, self, "webView:didStartProvisionalNavigation:", v->navigation); }
    else if (name == "finished") {
        v->loading = false;
        if (truth(field(event, "ok"))) {
            notify(v, self, "webView:didCommitNavigation:", v->navigation);
            notify(v, self, "webView:didFinishNavigation:", v->navigation);
        }
    } else if (name == "url") {
        id value = obj(send(reinterpret_cast<id>(cls("NSURL")), "URLWithString:", ret(string(text(field(event, "url"))))));
        retain(value); release(v->url); v->url = value;
    } else if (name == "title") { id value = retain(string(text(field(event, "title")))); release(v->title); v->title = value; }
    else if (name == "error" || name == "closed")
        navigation_error(self, error(1, name == "closed" ? "Browser helper exited" : text(field(event, "message"))));
    else if (name == "js") {
        auto it = v->completions.find(integer(field(event, "id")));
        if (it != v->completions.end()) {
            id block = it->second; v->completions.erase(it);
            const std::uint64_t args[] = {ret(from_json(field(event, "result"))), 0};
            mm_objc_block_invoke(block, 2, args); release(block);
        }
    } else if (name == "message") {
        id controller = config_child(v->configuration, "userContentController", "WKUserContentController");
        const std::string handler_name = text(field(event, "name"));
        id handler = get(controller, ("handler:" + handler_name).c_str());
        if (responds(handler, "userContentController:didReceiveScriptMessage:")) {
            id message = autorelease(make_object("WKScriptMessage"));
            put(message, "name", string(handler_name)); put(message, "body", from_json(field(event, "body")));
            put(message, "webView", self);
            send(handler, "userContentController:didReceiveScriptMessage:", ret(controller), ret(message));
        }
    } else if (name == "cookie" || name == "cookieRemoved")
        save_cookie(view_cookie_store(v), decode_cookie(field(event, "cookie")), name == "cookieRemoved");
    release(self);
}
struct event_task : task {
    std::shared_ptr<browser> process; mm_json event;
    event_task(std::shared_ptr<browser> p, mm_json e) : process(std::move(p)), event(std::move(e)) {
        run = +[](task *t) { auto *e = static_cast<event_task *>(t); handle_event(e->process, e->event); };
    }
};
void browser::read_events() {
    char bytes[8192]; std::string pending;
    while (!stopping) {
        const auto n = ::recv(socket, bytes, sizeof bytes, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        pending.append(bytes, static_cast<std::size_t>(n));
        if (pending.size() > 8 * 1024 * 1024) break;
        std::size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            mm_json event; std::string diagnostic;
            if (mm_json_parse(std::string_view(pending).substr(0, end), event, diagnostic))
                post(new event_task(shared_from_this(), std::move(event)));
            else if (std::getenv("MACHOMAN_WEBKIT_TRACE")) std::fprintf(stderr, "WebKit: malformed browser event\n");
            pending.erase(0, end + 1);
        }
    }
    if (!stopping) post(new event_task(shared_from_this(), mm_json(json_object{{"event", mm_json(std::string("closed"))}})));
}
json_object browser_configuration(webview *v) {
    json_object out{{"op", mm_json(std::string("configure"))}};
    out["applicationName"] = mm_json(to_utf8(get(v->configuration, "applicationNameForUserAgent")));
    out["userAgent"] = mm_json(to_utf8(get(v->configuration, "customUserAgent")));
    json_array cookies;
    for (id cookie : as<cookie_store>(view_cookie_store(v))->cookies) cookies.push_back(encode_cookie(cookie));
    out["cookies"] = mm_json(std::move(cookies));
    id controller = config_child(v->configuration, "userContentController", "WKUserContentController");
    json_array handlers, scripts;
    for (const auto &[key, value] : as<object>(controller)->values)
        if (key.starts_with("handler:")) handlers.emplace_back(key.substr(8));
    if (id array = get(controller, "userScripts"))
        for (std::uint64_t i = 0, n = send(array, "count"); i < n; ++i) {
            id s = obj(send(array, "objectAtIndex:", i));
            scripts.emplace_back(json_object{{"source", mm_json(to_utf8(get(s, "source")))},
                {"time", num(send(get(s, "injectionTime"), "unsignedLongLongValue"))},
                {"mainOnly", mm_json(send(get(s, "forMainFrameOnly"), "boolValue") != 0)}});
        }
    out["handlers"] = mm_json(std::move(handlers)); out["scripts"] = mm_json(std::move(scripts));
    return out;
}
bool ensure_browser(id self) {
    auto *v = as<webview>(self);
    if (v->process) return !v->process->stopping;
    std::filesystem::path helper;
    if (const char *override = std::getenv("MACHOMAN_WEBVIEW_HELPER")) helper = override;
#ifdef MM_WEBVIEW_HELPER
    else helper = MM_WEBVIEW_HELPER;
#endif
    if (helper.empty() || ::access(helper.c_str(), X_OK) != 0) {
        navigation_error(self, error(1, "Qt WebEngine browser helper is unavailable")); return false;
    }
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        navigation_error(self, error(1, "Cannot create browser connection")); return false;
    }
    auto process = std::make_shared<browser>(); process->owner = self; process->socket = sockets[0];
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, sockets[0]);
    ::posix_spawn_file_actions_addclose(&actions, sockets[1]);
    std::string path = helper.string(); char *argv[] = {path.data(), nullptr};
    const int result = ::posix_spawn(&process->pid, path.c_str(), &actions, nullptr, argv, environ);
    ::posix_spawn_file_actions_destroy(&actions); ::close(sockets[1]);
    if (result != 0) { navigation_error(self, error(1, "Cannot launch browser helper")); return false; }
    v->process = process; browsers.push_back(process);
    process->reader = std::thread([process] { process->read_events(); });
    process->send_json(mm_json(browser_configuration(v)));
    process->send_json(mm_json(json_object{{"op", mm_json(std::string("resize"))},
        {"width", num(static_cast<std::uint64_t>(std::max(100., v->view.frame.w)))},
        {"height", num(static_cast<std::uint64_t>(std::max(100., v->view.frame.h)))}}));
    std::fprintf(stderr, "WebKit: browser helper started (PID %d)\n", process->pid);
    return true;
}
std::uint64_t view_init(id self, SEL, id configuration, double x, double y, double width, double height) {
    auto *v = as<webview>(self);
    v->view.frame = {x, y, width, height}; v->view.bounds = {0, 0, width, height};
    release(v->configuration);
    v->configuration = configuration ? obj(send(configuration, "copy")) : make_object("WKWebViewConfiguration");
    return ret(self);
}
void new_navigation(webview *v) {
    release(v->navigation); v->navigation = make_object("WKNavigation"); v->loading = true;
}
std::uint64_t load_request(id self, SEL, id request) {
    auto *v = as<webview>(self); new_navigation(v);
    id url = obj(send(request, "URL"));
    const auto address = to_utf8(obj(send(url, "absoluteString")));
    if (address.empty()) {
        navigation_error(self, error(1, "URL request has no address")); return 0;
    }
    if (std::getenv("MACHOMAN_WEBKIT_TRACE"))
        std::fprintf(stderr, "WebKit: request scheme=%s host=%s\n", to_utf8(obj(send(url, "scheme"))).c_str(), to_utf8(obj(send(url, "host"))).c_str());
    retain(url); release(v->url); v->url = url;
    if (!ensure_browser(self)) return 0;
    json_object command{{"op", mm_json(std::string("open"))},
        {"url", mm_json(address)},
        {"method", mm_json(to_utf8(obj(send(request, "HTTPMethod"))))}};
    id body = obj(send(request, "HTTPBody"));
    if (body) command["body"] = mm_json(to_utf8(obj(send(body, "base64EncodedStringWithOptions:", 0))));
    json_object headers;
    id source = obj(send(request, "allHTTPHeaderFields")), keys = obj(send(source, "allKeys"));
    for (std::uint64_t i = 0, n = send(keys, "count"); i < n; ++i) {
        id key = obj(send(keys, "objectAtIndex:", i));
        headers[to_utf8(key)] = mm_json(to_utf8(obj(send(source, "objectForKey:", ret(key)))));
    }
    command["headers"] = mm_json(std::move(headers));
    if (!v->process->send_json(mm_json(std::move(command)))) navigation_error(self, error(1, "Browser connection closed"));
    return ret(autorelease(retain(v->navigation)));
}
std::uint64_t load_html(id self, SEL, id html, id base) {
    auto *v = as<webview>(self); new_navigation(v);
    if (!ensure_browser(self)) return 0;
    v->process->send_json(mm_json(json_object{{"op", mm_json(std::string("html"))}, {"html", mm_json(to_utf8(html))},
        {"base", mm_json(to_utf8(obj(send(base, "absoluteString"))))}}));
    return ret(autorelease(retain(v->navigation)));
}
std::uint64_t evaluate(id self, SEL, id source, id block) {
    auto *v = as<webview>(self);
    if (!ensure_browser(self)) { complete(block, {nullptr, error(1, "Browser unavailable")}); return 0; }
    const auto id = ++v->serial;
    if (block) v->completions[id] = retain(block);
    if (!v->process->send_json(mm_json(json_object{{"op", mm_json(std::string("js"))},
        {"id", num(id)}, {"source", mm_json(to_utf8(source))}}))) {
        if (block) { release(v->completions[id]); v->completions.erase(id); }
        complete(block, {nullptr, error(1, "Browser connection closed")});
    }
    return 0;
}
std::uint64_t browser_action(id self, SEL sel) {
    auto *v = as<webview>(self);
    std::string op = std::strcmp(sel, "stopLoading") == 0 ? "stop" :
        std::strcmp(sel, "goBack") == 0 ? "back" : std::strcmp(sel, "goForward") == 0 ? "forward" : "reload";
    if (v->process) v->process->send_json(mm_json(json_object{{"op", mm_json(op)}}));
    return op == "stop" ? 0 : ret(v->navigation);
}
std::uint64_t cookie_change(id self, SEL sel, id cookie, id handler) {
    const bool remove = std::strcmp(sel, "deleteCookie:completionHandler:") == 0;
    save_cookie(self, cookie, remove);
    for (const auto &weak : browsers) if (auto browser = weak.lock()) {
        if (browser->owner && view_cookie_store(as<webview>(browser->owner)) == self)
            browser->send_json(mm_json(json_object{{"op", mm_json(std::string(remove ? "deleteCookie" : "cookie"))},
                {"cookie", encode_cookie(cookie)}}));
    }
    complete(handler); return 0;
}
} // namespace

void register_webview() {
    dispatch = reinterpret_cast<dispatch_fn>(dlsym(RTLD_DEFAULT, "_dispatch_async_f"));
    main_queue = reinterpret_cast<main_queue_fn>(dlsym(RTLD_DEFAULT, "_dispatch_get_main_queue"));
    Class config = plain("WKWebViewConfiguration", MM_FW_CLASS_REF(WKWebViewConfiguration), MM_FW_META_REF(WKWebViewConfiguration));
    Class store = plain("WKWebsiteDataStore", MM_FW_CLASS_REF(WKWebsiteDataStore), MM_FW_META_REF(WKWebsiteDataStore));
    Class controller = plain("WKUserContentController", MM_FW_CLASS_REF(WKUserContentController), MM_FW_META_REF(WKUserContentController));
    Class script = plain("WKUserScript", MM_FW_CLASS_REF(WKUserScript), MM_FW_META_REF(WKUserScript));
    Class message = plain("WKScriptMessage", MM_FW_CLASS_REF(WKScriptMessage), MM_FW_META_REF(WKScriptMessage));
    plain("WKNavigation", MM_FW_CLASS_REF(WKNavigation), MM_FW_META_REF(WKNavigation));
    Class preferences = plain("WKPreferences", MM_FW_CLASS_REF(WKPreferences), MM_FW_META_REF(WKPreferences));
    Class cookies = mm_objc_define_class("WKHTTPCookieStore", cls("NSObject"), sizeof(cookie_store),
        MM_FW_CLASS_REF(WKHTTPCookieStore), MM_FW_META_REF(WKHTTPCookieStore));
    register_lifecycle<cookie_store>(cookies);
    Class view = mm_objc_define_class("WKWebView", cls("NSView"), sizeof(webview), MM_FW_CLASS_REF(WKWebView), MM_FW_META_REF(WKWebView));
    register_lifecycle<webview>(view);
    property(config, "applicationNameForUserAgent", "setApplicationNameForUserAgent:");
    property(config, "websiteDataStore", "setWebsiteDataStore:");
    property(config, "userContentController", "setUserContentController:");
    property(config, "preferences", "setPreferences:");
    MM_METHODS(config,
        {"websiteDataStore", MM_IMP(+[](id s, SEL) { return ret(config_store(s)); }), "@16@0:8"},
        {"userContentController", MM_IMP(+[](id s, SEL) { return ret(config_child(s, "userContentController", "WKUserContentController")); }), "@16@0:8"},
        {"preferences", MM_IMP(+[](id s, SEL) { return ret(config_child(s, "preferences", "WKPreferences")); }), "@16@0:8"});
    MM_CLASS_METHODS(store,
        {"defaultDataStore", MM_IMP(+[](id, SEL) { return ret(data_store(true)); }), "@16@0:8"},
        {"nonPersistentDataStore", MM_IMP(+[](id, SEL) { return ret(data_store(false)); }), "@16@0:8"});
    MM_METHODS(store,
        {"httpCookieStore", MM_IMP(+[](id s, SEL) { return ret(get(s, "httpCookieStore")); }), "@16@0:8"},
        {"isPersistent", MM_IMP(+[](id s, SEL) { return send(get(s, "persistent"), "boolValue"); }), "B16@0:8"});
    MM_METHODS(cookies,
        {"setCookie:completionHandler:", MM_IMP(cookie_change), "v32@0:8@16@?24"},
        {"deleteCookie:completionHandler:", MM_IMP(cookie_change), "v32@0:8@16@?24"},
        {"getAllCookies:", MM_IMP(+[](id s, SEL, id block) -> std::uint64_t {
            id all = array(); for (id c : as<cookie_store>(s)->cookies) send(all, "addObject:", ret(c));
            complete(block, {all}); return 0;
        }), "v24@0:8@?16"});
    MM_METHODS(controller,
        {"addScriptMessageHandler:name:", MM_IMP(+[](id s, SEL, id handler, id name) -> std::uint64_t {
            put(s, ("handler:" + to_utf8(name)).c_str(), handler); return 0;
        }), "v32@0:8@16@24"},
        {"removeScriptMessageHandlerForName:", MM_IMP(+[](id s, SEL, id name) -> std::uint64_t {
            put(s, ("handler:" + to_utf8(name)).c_str(), nullptr); return 0;
        }), "v24@0:8@16"},
        {"addUserScript:", MM_IMP(+[](id s, SEL, id value) -> std::uint64_t {
            id scripts = get(s, "userScripts"); if (!scripts) { scripts = array(); put(s, "userScripts", scripts); }
            send(scripts, "addObject:", ret(value)); return 0;
        }), "v24@0:8@16"},
        {"userScripts", MM_IMP(+[](id s, SEL) { return ret(get(s, "userScripts") ? get(s, "userScripts") : array()); }), "@16@0:8"},
        {"removeAllUserScripts", MM_IMP(+[](id s, SEL) -> std::uint64_t { put(s, "userScripts", nullptr); return 0; }), "v16@0:8"});
    MM_METHODS(script,
        {"initWithSource:injectionTime:forMainFrameOnly:", MM_IMP(+[](id s, SEL, id source, std::uint64_t time, std::uint64_t only) {
            put(s, "source", source);
            put(s, "injectionTime", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithUnsignedLongLong:", time)));
            put(s, "forMainFrameOnly", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", only)));
            return ret(s);
        }), "@40@0:8@16q24B32"},
        {"source", MM_IMP(+[](id s, SEL) { return ret(get(s, "source")); }), "@16@0:8"},
        {"injectionTime", MM_IMP(+[](id s, SEL) { return send(get(s, "injectionTime"), "unsignedLongLongValue"); }), "q16@0:8"},
        {"isForMainFrameOnly", MM_IMP(+[](id s, SEL) { return send(get(s, "forMainFrameOnly"), "boolValue"); }), "B16@0:8"});
    for (const char *key : {"name", "body", "webView"})
        mm_objc_add_method(message, key, MM_IMP(+[](id s, SEL sel) { return ret(get(s, sel)); }), "@16@0:8");
    MM_METHODS(preferences,
        {"setJavaScriptEnabled:", MM_IMP(+[](id s, SEL, std::uint64_t enabled) -> std::uint64_t {
            put(s, "javaScriptEnabled", obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithBool:", enabled))); return 0;
        }), "v24@0:8B16"},
        {"javaScriptEnabled", MM_IMP(+[](id s, SEL) -> std::uint64_t {
            id value = get(s, "javaScriptEnabled"); return !value || send(value, "boolValue");
        }), "B16@0:8"});
    MM_METHODS(view,
        {"initWithFrame:configuration:", MM_IMP(view_init), "@56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48"},
        {"init", MM_IMP(+[](id s, SEL) { return view_init(s, nullptr, nullptr, 0, 0, 640, 600); }), "@16@0:8"},
        {"initWithFrame:", MM_IMP(+[](id s, SEL, double x, double y, double w, double h) {
            return view_init(s, nullptr, nullptr, x, y, w, h);
        }), "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"configuration", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->configuration); }), "@16@0:8"},
        {"setNavigationDelegate:", MM_IMP(+[](id s, SEL, id d) -> std::uint64_t { as<webview>(s)->navigation_delegate = d; return 0; }), "v24@0:8@16"},
        {"navigationDelegate", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->navigation_delegate); }), "@16@0:8"},
        {"setUIDelegate:", MM_IMP(+[](id s, SEL, id d) -> std::uint64_t { as<webview>(s)->ui_delegate = d; return 0; }), "v24@0:8@16"},
        {"UIDelegate", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->ui_delegate); }), "@16@0:8"},
        {"loadRequest:", MM_IMP(load_request), "@24@0:8@16"},
        {"loadHTMLString:baseURL:", MM_IMP(load_html), "@32@0:8@16@24"},
        {"evaluateJavaScript:completionHandler:", MM_IMP(evaluate), "v32@0:8@16@?24"},
        {"stopLoading", MM_IMP(browser_action), "v16@0:8"},
        {"reload", MM_IMP(browser_action), "@16@0:8"},
        {"goBack", MM_IMP(browser_action), "@16@0:8"},
        {"goForward", MM_IMP(browser_action), "@16@0:8"},
        {"URL", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->url); }), "@16@0:8"},
        {"title", MM_IMP(+[](id s, SEL) { return ret(as<webview>(s)->title); }), "@16@0:8"},
        {"isLoading", MM_IMP(+[](id s, SEL) -> std::uint64_t { return as<webview>(s)->loading; }), "B16@0:8"},
        {"setCustomUserAgent:", MM_IMP(+[](id s, SEL, id value) -> std::uint64_t {
            put(as<webview>(s)->configuration, "customUserAgent", value); return 0;
        }), "v24@0:8@16"},
        {"customUserAgent", MM_IMP(+[](id s, SEL) { return ret(get(as<webview>(s)->configuration, "customUserAgent")); }), "@16@0:8"},
        {"viewDidMoveToWindow", MM_IMP(+[](id s, SEL) -> std::uint64_t {
            auto *v = as<webview>(s);
            if (v->process && !v->view.window) v->process->send_json(mm_json(json_object{{"op", mm_json(std::string("hide"))}}));
            return 0;
        }), "v16@0:8"});
}
} // namespace ak
