#include "machoman/ipc.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mm_ipc {
namespace {
constexpr std::uint32_t invalid_argument = 4, invalid_task = 16, invalid_name = 15,
    invalid_right = 17, name_exists = 13, right_exists = 21, urefs_overflow = 19;
struct object;
struct transferred_right {
    std::size_t offset = 0;
    std::uint32_t kind = 0, special_name = 0;
    std::shared_ptr<object> port;
};
struct queued_message {
    std::vector<std::uint8_t> bytes;
    std::vector<transferred_right> rights;
    std::uint64_t order = 0;
    std::uint32_t pid = 0, uid = 0;
};
struct notification_request {
    std::shared_ptr<object> destination;
    std::uint32_t id = 0, sync = 0;
    bool armed = false;
};
struct object {
    bool alive = true, is_set = false;
    bool guarded = false, strict_guard = false, immovable = false;
    task_id receiver = 0;
    std::uint64_t context = 0, guard = 0;
    std::uint32_t queue_limit = 5, sequence = 0, make_send_count = 0;
    std::deque<queued_message> queue;
    std::unordered_set<std::shared_ptr<object>> members;
    enum class kind { queue, task, thread, host, bootstrap, memory } kind_ = kind::queue;
    task_id target = 0;
    file_descriptor memory;
    std::uint64_t memory_size = 0;
    std::uint64_t memory_offset = 0;
    std::uint32_t protection = 3;
    struct watch { task_id owner; std::uint64_t id; file_descriptor event; bool ready = false; };
    std::vector<watch> watchers;
    struct exception { std::shared_ptr<object> port; std::uint32_t behavior = 0, flavor = 0; };
    std::array<exception, 32> exceptions{};
    notification_request no_senders, destroyed;
};
struct right {
    std::shared_ptr<object> port;
    std::array<std::uint32_t, 5> refs{};
    notification_request notification;
};
struct task {
    task_id id = 0;
    bool alive = true;
    std::uint32_t pid = 0, uid = 0;
    name next = 0x1003;
    std::unordered_map<name, right> names;
    std::unordered_map<std::uint64_t, std::shared_ptr<object>> threads;
};
bool used(const right &r) { return std::ranges::any_of(r.refs, [](auto n) { return n != 0; }); }
template <typename T> T load(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    T value; std::memcpy(&value, bytes.data() + offset, sizeof value); return value;
}
template <typename T> void store(std::vector<std::uint8_t> &bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}
std::uint32_t type_of(const right &r) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < r.refs.size(); ++i) if (r.refs[i]) result |= 1u << (16 + i);
    return result;
}
}

struct kernel::state {
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<task_id, std::shared_ptr<task>> tasks;
    std::uint64_t message_order = 0;
    std::uint64_t watch_id = 0;
    std::shared_ptr<object> host_port = std::make_shared<object>();
    std::shared_ptr<object> bootstrap_port = std::make_shared<object>();
    struct service { std::shared_ptr<object> port; task_id registrant = 0; bool checked_in = false; };
    std::unordered_map<std::string, service> services;
    void notify(const std::shared_ptr<object> &destination, std::uint32_t id,
                std::uint32_t value = 0, const std::shared_ptr<object> &receive = {}) {
        if (!destination || !destination->alive) {
            if (receive) destroy_receive(receive);
            return;
        }
        queued_message message;
        const unsigned size = receive ? 40 : id == 71 ? 24 : 36;
        message.bytes.resize(size);
        store(message.bytes, 0, (receive ? 0x80000000u : 0u) | (18u << 8));
        store(message.bytes, 4, size); store(message.bytes, 20, id);
        if (receive) {
            store(message.bytes, 24, 1u);
            message.bytes[38] = 16;
            message.rights.push_back({28, 16, 0, receive});
        } else if (size == 36) {
            /* NDR_record: version/encoding zero, little endian integer,
             * IEEE floating point and ASCII characters. */
            message.bytes[28] = 1;
            store(message.bytes, 32, value);
        }
        message.order = ++message_order;
        destination->queue.push_back(std::move(message));
        changed.notify_all();
    }
    void erase_name(task &t, name n, bool consumed_once = false) {
        const auto it = t.names.find(n);
        if (it == t.names.end()) return;
        auto value = std::move(it->second);
        t.names.erase(it);
        if (value.refs[2] && !consumed_once) notify(value.port, 71);
        if (value.notification.destination) notify(value.notification.destination, 65, n);
    }
    /* Count send rights held by tasks, messages in transit, bootstrap and
     * exception handlers. User references on a single name are one sender. */
    struct ownership {
        std::vector<std::shared_ptr<object>> objects;
        std::unordered_map<object *, std::uint64_t> send, once;
    };
    ownership owners() {
        ownership result;
        std::unordered_set<object *> seen;
        auto add = [&](const std::shared_ptr<object> &port) {
            if (port && seen.insert(port.get()).second) result.objects.push_back(port);
        };
        add(host_port); add(bootstrap_port);
        for (const auto &[id, t] : tasks) { (void)id;
            for (const auto &[n, r] : t->names) { (void)n;
                add(r.port); add(r.notification.destination);
                if (r.refs[0]) ++result.send[r.port.get()];
                result.once[r.port.get()] += r.refs[2];
                if (r.notification.destination) ++result.once[r.notification.destination.get()];
            }
            for (const auto &[id_, port] : t->threads) { (void)id_; add(port); }
        }
        for (const auto &[name_, service_] : services) { (void)name_;
            add(service_.port); ++result.send[service_.port.get()];
        }
        for (std::size_t i = 0; i < result.objects.size(); ++i) {
            const auto port = result.objects[i];
            for (const auto &member : port->members) add(member);
            for (const auto &notification : {port->no_senders, port->destroyed}) {
                add(notification.destination);
                if (notification.destination) ++result.once[notification.destination.get()];
            }
            for (const auto &handler : port->exceptions) {
                add(handler.port);
                if (handler.port) ++result.send[handler.port.get()];
            }
            for (const auto &message : port->queue) for (const auto &r : message.rights) {
                add(r.port);
                if (r.port && r.kind == 17) ++result.send[r.port.get()];
                if (r.port && r.kind == 18) ++result.once[r.port.get()];
            }
        }
        return result;
    }
    void refresh_notifications() {
        const auto held = owners();
        for (const auto &port : held.objects) {
            auto &request = port->no_senders;
            if (!port->alive || !request.destination) continue;
            const auto it = held.send.find(port.get());
            if (it != held.send.end() && it->second) request.armed = true;
            else if (request.armed || request.sync <= port->make_send_count) {
                const auto destination = std::move(request.destination);
                notify(destination, 70, port->make_send_count);
            }
        }
        for (const auto &[id, t] : tasks) { (void)id;
            for (auto &[n, r] : t->names) {
                auto &request = r.notification;
                if (request.id == 66 && request.armed && request.destination && r.port->alive &&
                    (r.refs[2] || r.port->queue.size() < r.port->queue_limit)) {
                    const auto destination = std::move(request.destination);
                    notify(destination, 66, n);
                }
            }
        }
    }
    static std::uint64_t pending(const std::shared_ptr<object> &port) {
        if (!port->is_set) return port->queue.size();
        std::uint64_t count = 0;
        for (const auto &member : port->members) count += member->queue.size();
        return count;
    }
    void refresh_one(const std::shared_ptr<object> &port) {
        const bool ready = !port->alive || pending(port) != 0;
        for (auto &watch : port->watchers) {
            if (ready && !watch.ready) { const std::uint64_t one = 1; (void)write(watch.event.get(), &one, sizeof one); }
            else if (!ready && watch.ready) { std::uint64_t value; while (read(watch.event.get(), &value, sizeof value) > 0) {} }
            watch.ready = ready;
        }
    }
    void refresh_watches() {
        std::unordered_set<std::shared_ptr<object>> seen;
        for (const auto &[id, t] : tasks) { (void)id;
            for (const auto &[n, r] : t->names) { (void)n; if (seen.insert(r.port).second) refresh_one(r.port); }
        }
    }

    name unused_name(task &t) {
        while (t.names.contains(t.next) || t.next < 0x1003 || t.next == ~0u) {
            t.next += 4;
            if (t.next < 0x1003) t.next = 0x1003;
        }
        const auto result = t.next; t.next += 4;
        return result;
    }
    void destroy_receive(const std::shared_ptr<object> &port) {
        if (!port->alive) return;
        if (port->destroyed.destination) {
            const auto destination = std::move(port->destroyed.destination);
            if (destination != port && destination->alive) {
                port->receiver = 0; port->sequence = port->make_send_count = 0; port->guard = 0;
                port->guarded = port->strict_guard = port->immovable = false;
                for (auto &[id, t] : tasks) { (void)id;
                    for (auto &[n, r] : t->names) { (void)n;
                        if (r.port == port) r.refs[1] = 0;
                        if (r.port->is_set) r.port->members.erase(port);
                    }
                }
                notify(destination, 69, 0, port);
                changed.notify_all();
                return;
            }
        }
        port->alive = false;
        port->receiver = 0;
        refresh_one(port);
        auto queued = std::move(port->queue);
        port->queue.clear();
        for (auto &message : queued) discard_message(message);
        for (auto &[id, t] : tasks) {
            (void)id;
            for (auto &[n, r] : t->names) {
                (void)n;
                if (r.port == port) {
                    r.refs[4] += r.refs[0] + r.refs[2];
                    r.refs[0] = r.refs[1] = r.refs[2] = 0;
                    if (r.notification.destination) {
                        if (r.refs[4] < 65535) ++r.refs[4];
                        const auto destination = std::move(r.notification.destination);
                        notify(destination, 72, n);
                    }
                }
                if (r.port->is_set) r.port->members.erase(port);
            }
        }
        if (port->no_senders.destination) {
            const auto destination = std::move(port->no_senders.destination);
            notify(destination, 71);
        }
        for (auto &handler : port->exceptions) handler.port.reset();
        for (auto it = services.begin(); it != services.end();) {
            if (it->second.port == port) it = services.erase(it);
            else ++it;
        }
        changed.notify_all();
        refresh_watches();
    }
    void discard_message(queued_message &message) {
        auto rights = std::move(message.rights);
        message.rights.clear();
        for (const auto &r : rights) if (r.port) {
            if (r.kind == 16) destroy_receive(r.port);
            else if (r.kind == 18) notify(r.port, 71);
        }
    }
    name import_send(task &t, const std::shared_ptr<object> &port) {
        for (auto &[n, r] : t.names) {
            if (r.port == port && (r.refs[0] || r.refs[1])) {
                if (r.refs[0] == 65535) return 0;
                ++r.refs[0]; return n;
            }
        }
        const auto n = unused_name(t);
        right r; r.port = port; r.refs[0] = 1;
        t.names.emplace(n, std::move(r));
        return n;
    }
    name import_right(task &t, task_id owner, const transferred_right &value) {
        if (!value.port) return value.special_name;
        if (!value.port->alive) return ~0u;
        if (value.kind == 17) return import_send(t, value.port);
        if (value.kind == 16) {
            for (auto &[n, r] : t.names) if (r.port == value.port && r.refs[0]) {
                r.refs[1] = 1; value.port->receiver = owner; return n;
            }
        }
        const auto n = unused_name(t);
        right r; r.port = value.port; r.refs[value.kind == 16 ? 1 : 2] = 1;
        if (value.kind == 16) value.port->receiver = owner;
        t.names.emplace(n, std::move(r));
        return n;
    }
};

kernel::kernel() : state_(std::make_unique<state>()) {
    state_->host_port->kind_ = object::kind::host;
    state_->bootstrap_port->kind_ = object::kind::bootstrap;
}
kernel::~kernel() = default;
void kernel::add_task(task_id id, std::uint32_t pid, std::uint32_t uid) {
    std::lock_guard lock(state_->mutex);
    if (state_->tasks.contains(id)) return;
    auto t = std::make_shared<task>(); t->id = id; t->pid = pid; t->uid = uid;
    auto self = std::make_shared<object>(); self->kind_ = object::kind::task; self->target = id;
    right self_right; self_right.port = self; self_right.refs[0] = 1; t->names.emplace(1, std::move(self_right));
    right host_right; host_right.port = state_->host_port; host_right.refs[0] = 1; t->names.emplace(2, std::move(host_right));
    right bootstrap_right; bootstrap_right.port = state_->bootstrap_port; bootstrap_right.refs[0] = 1; t->names.emplace(3, std::move(bootstrap_right));
    state_->tasks.emplace(id, std::move(t));
}
void kernel::remove_task(task_id id) {
    std::lock_guard lock(state_->mutex);
    auto it = state_->tasks.find(id); if (it == state_->tasks.end()) return;
    auto t = it->second;
    t->alive = false;
    std::vector<std::shared_ptr<object>> receive;
    for (const auto &[n, r] : t->names) { (void)n; if (r.refs[1]) receive.push_back(r.port); }
    for (const auto &port : receive) state_->destroy_receive(port);
    if (const auto self = t->names.find(1); self != t->names.end()) state_->destroy_receive(self->second.port);
    for (const auto &[thread, port] : t->threads) { (void)thread; state_->destroy_receive(port); }
    for (const auto &[other_id, other] : state_->tasks) { (void)other_id;
        for (auto &[n, r] : other->names) { (void)n;
            std::erase_if(r.port->watchers, [id](const auto &watch) { return watch.owner == id; });
        }
    }
    for (auto si = state_->services.begin(); si != state_->services.end();) {
        if (si->second.registrant == id) si = state_->services.erase(si); else ++si;
    }
    state_->tasks.erase(it);
    while (!t->names.empty()) state_->erase_name(*t, t->names.begin()->first);
    state_->refresh_notifications();
    state_->refresh_watches();
    state_->changed.notify_all();
}

response kernel::transact(task_id caller, const request &req) {
    auto &s = *state_;
    std::unique_lock lock(s.mutex);
    response out;
    const auto ti = s.tasks.find(caller);
    if (ti == s.tasks.end() || !ti->second->alive) { out.status = invalid_task; return out; }
    auto t = ti->second;
    auto fail = [&](std::uint32_t status) {
        out.status = status; s.refresh_notifications(); s.refresh_watches(); return out;
    };
    const name n = static_cast<name>(req.args[1]);
    auto get = [&](name value) -> right * {
        const auto it = t->names.find(value); return it == t->names.end() ? nullptr : &it->second;
    };
    /* Task-control operations currently address the caller's namespace.
     * Remote task rights will resolve here through task_for_pid. */
    if ((req.op <= operation::request_notification ||
         (req.op >= operation::construct && req.op <= operation::set_seqno)) && req.args[0] != 1) {
        auto *target = get(static_cast<name>(req.args[0]));
        if (!target || !target->refs[0] || target->port->kind_ != object::kind::task || !target->port->alive) return fail(invalid_task);
        const auto target_task = s.tasks.find(target->port->target);
        if (target_task == s.tasks.end() || target_task->second->uid != t->uid) return fail(invalid_task);
        t = target_task->second;
    }
    right *r = get(n);
    switch (req.op) {
    case operation::construct: {
        const auto flags = req.args[1];
        if (flags & ~(1u | 2u | 0x10u | 0x20u | 0x80u)) return fail(46);
        if ((flags & (0x20u | 0x80u)) && !(flags & 1u)) return fail(invalid_argument);
        if ((flags & 2u) && req.args[2] > 1024) return fail(invalid_argument);
        auto port = std::make_shared<object>(); port->receiver = t->id;
        port->context = req.args[3];
        port->guarded = flags & 1u; port->guard = port->guarded ? req.args[3] : 0;
        port->strict_guard = flags & 0x20u; port->immovable = flags & 0x80u;
        if (flags & 2u) port->queue_limit = static_cast<unsigned>(req.args[2]);
        right fresh; fresh.port = port; fresh.refs[1] = 1;
        if (flags & 0x10u) fresh.refs[0] = port->make_send_count = 1;
        const auto assigned = s.unused_name(*t); t->names.emplace(assigned, std::move(fresh)); out.args[0] = assigned;
        break;
    }
    case operation::destruct: {
        if (!r) return fail(invalid_name);
        if (!r->refs[1]) return fail(invalid_right);
        const auto delta = static_cast<std::int64_t>(req.args[2]);
        if (delta > 0 || delta < -static_cast<std::int64_t>(r->refs[0])) return fail(invalid_argument);
        if (r->port->guarded && r->port->guard != req.args[3]) return fail(invalid_argument);
        r->refs[0] = static_cast<unsigned>(static_cast<std::int64_t>(r->refs[0]) + delta);
        s.destroy_receive(r->port);
        if (!used(*r)) s.erase_name(*t, n);
        break;
    }
    case operation::set_mscount: case operation::set_seqno:
        if (!r) return fail(invalid_name);
        if (!r->refs[1]) return fail(invalid_right);
        if (req.op == operation::set_mscount) r->port->make_send_count = static_cast<unsigned>(req.args[2]);
        else r->port->sequence = static_cast<unsigned>(req.args[2]);
        break;
    case operation::allocate: {
        const auto kind = req.args[1];
        if (kind != 1 && kind != 3 && kind != 4) return fail(invalid_argument);
        const name requested = static_cast<name>(req.args[2]);
        if (requested && (requested < 4 || requested == ~0u || t->names.contains(requested))) return fail(name_exists);
        const auto assigned = requested ? requested : s.unused_name(*t);
        right fresh; fresh.port = std::make_shared<object>(); fresh.refs[kind] = 1;
        fresh.port->is_set = kind == 3;
        fresh.port->receiver = kind == 1 ? t->id : 0;
        t->names.emplace(assigned, std::move(fresh)); out.args[0] = assigned;
        break;
    }
    case operation::get_refs:
        if (!r) return fail(invalid_name);
        if (req.args[2] >= 5) return fail(invalid_argument);
        out.args[0] = r->refs[req.args[2]];
        break;
    case operation::type:
        if (!r) return fail(invalid_name);
        out.args[0] = type_of(*r);
        break;
    case operation::mod_refs: {
        if (!r) return fail(invalid_name);
        const auto kind = req.args[2]; if (kind >= 5) return fail(invalid_argument);
        if (!r->refs[kind]) return fail(invalid_right);
        const auto count = static_cast<std::int64_t>(r->refs[kind]) + static_cast<std::int64_t>(req.args[3]);
        if (count < 0 || ((kind == 1 || kind == 2 || kind == 3) && count > 1)) return fail(invalid_argument);
        if (count > 65535) return fail(urefs_overflow);
        if (kind == 1 && count == 0) {
            if (r->port->guarded) return fail(invalid_argument);
            s.destroy_receive(r->port);
        }
        else if (kind == 3 && count == 0) { r->port->alive = false; r->refs[3] = 0; s.changed.notify_all(); }
        else r->refs[kind] = static_cast<std::uint32_t>(count);
        if (kind == 2 && count == 0) s.notify(r->port, 71);
        if (!used(*r)) s.erase_name(*t, n);
        break;
    }
    case operation::destroy:
        if (!r) return fail(invalid_name);
        if (r->port->guarded) return fail(invalid_argument);
        if (r->refs[1]) s.destroy_receive(r->port);
        if (r->refs[3]) { r->port->alive = false; s.changed.notify_all(); }
        s.erase_name(*t, n);
        break;
    case operation::deallocate:
        if (n == 0 || n == ~0u || n <= 3) break;
        if (!r) return fail(invalid_name);
        if (r->refs[0]) --r->refs[0];
        else if (r->refs[2]) { --r->refs[2]; s.notify(r->port, 71); }
        else if (r->refs[4]) --r->refs[4];
        else return fail(invalid_right);
        if (!used(*r)) s.erase_name(*t, n);
        break;
    case operation::extract: {
        if (!r) return fail(invalid_name);
        const auto disposition = req.args[2];
        const auto port = r->port;
        if (!port->alive) return fail(invalid_right);
        if (disposition == 20 || disposition == 21) {
            if (!r->refs[1]) return fail(invalid_right);
            if (disposition == 20) {
                if (r->refs[0] == 65535) return fail(urefs_overflow);
                ++r->refs[0]; ++port->make_send_count; out.args[0] = n; out.args[1] = 17;
            } else {
                const auto assigned = s.unused_name(*t);
                right once; once.port = port; once.refs[2] = 1;
                t->names.emplace(assigned, std::move(once)); out.args[0] = assigned; out.args[1] = 18;
            }
        } else if (disposition == 17 || disposition == 19) {
            if (!r->refs[0]) return fail(invalid_right);
            if (disposition == 19) {
                if (r->refs[0] == 65535) return fail(urefs_overflow);
                ++r->refs[0];
            }
            out.args[0] = n; out.args[1] = 17;
        } else if (disposition == 18) {
            if (!r->refs[2]) return fail(invalid_right);
            out.args[0] = n; out.args[1] = 18;
        } else return fail(invalid_argument);
        break;
    }
    case operation::insert: {
        if (n < 4 || n == ~0u) return fail(invalid_argument);
        const name source_name = static_cast<name>(req.args[2]);
        auto *source = get(source_name); if (!source) return fail(invalid_name);
        const auto disposition = req.args[3];
        const auto port = source->port;
        if (r && r->port != port) return fail(name_exists);
        const unsigned kind = disposition == 16 ? 1u : disposition == 18 || disposition == 21 ? 2u : 0u;
        const unsigned needed = disposition == 16 || disposition == 20 || disposition == 21 ? 1u : disposition == 18 ? 2u : 0u;
        if (disposition < 16 || disposition > 21 || !source->refs[needed] || !port->alive) return fail(invalid_right);
        if (r && (kind == 1 || kind == 2) && r->refs[kind]) return fail(right_exists);
        if (r && kind == 0 && r->refs[0] == 65535) return fail(urefs_overflow);
        if (disposition == 16 || disposition == 17 || disposition == 18) --source->refs[needed];
        if (disposition == 20) ++port->make_send_count;
        if (!r) { right fresh; fresh.port = port; r = &t->names.emplace(n, std::move(fresh)).first->second; }
        ++r->refs[kind];
        /* Rehash above can invalidate source; look it up again. */
        source = get(source_name);
        if (source && !used(*source)) s.erase_name(*t, source_name, true);
        break;
    }
    case operation::rename: {
        if (!r) return fail(invalid_name);
        const auto new_name = static_cast<name>(req.args[2]);
        if (new_name < 4 || new_name == ~0u) return fail(invalid_argument);
        if (t->names.contains(new_name)) return fail(name_exists);
        auto node = t->names.extract(n); node.key() = new_name; t->names.insert(std::move(node));
        break;
    }
    case operation::names: {
        std::vector<std::pair<name, std::uint32_t>> values;
        for (const auto &[port_name, value] : t->names) values.emplace_back(port_name, type_of(value));
        std::ranges::sort(values);
        out.args[0] = values.size(); out.data.resize(values.size() * 8);
        for (std::size_t i = 0; i < values.size(); ++i) {
            std::memcpy(out.data.data() + i * 8, &values[i].first, 4);
            std::memcpy(out.data.data() + i * 8 + 4, &values[i].second, 4);
        }
        break;
    }
    case operation::move_member: case operation::insert_member: case operation::extract_member: {
        if (!r) return fail(invalid_name);
        if (!r->refs[1]) return fail(invalid_right);
        auto *set = get(static_cast<name>(req.args[2]));
        if (req.args[2] && !set) return fail(invalid_name);
        if (set && !set->refs[3]) return fail(invalid_right);
        if (req.op == operation::move_member) {
            for (auto &[key, value] : t->names) { (void)key; if (value.refs[3]) value.port->members.erase(r->port); }
        }
        if (set) {
            if (req.op == operation::extract_member) {
                if (!set->port->members.erase(r->port)) return fail(12); /* KERN_NOT_IN_SET */
            } else if (!set->port->members.insert(r->port).second && req.op == operation::insert_member) return fail(11);
        }
        s.changed.notify_all(); break;
    }
    case operation::members:
        if (!r) return fail(invalid_name);
        if (!r->refs[3]) return fail(invalid_right);
        for (const auto &[port_name, value] : t->names) if (value.refs[1] && r->port->members.contains(value.port)) {
            const auto offset = out.data.size(); out.data.resize(offset + 4);
            std::memcpy(out.data.data() + offset, &port_name, 4);
        }
        out.args[0] = out.data.size() / 4; break;
    case operation::get_attributes: case operation::set_attributes: {
        if (!r) return fail(invalid_name);
        if (!r->refs[1]) return fail(invalid_right);
        const auto flavor = req.args[2];
        if (req.op == operation::set_attributes) {
            if (flavor != 1 || req.data.size() != 4) return fail(invalid_argument);
            const auto limit = load<std::uint32_t>(req.data, 0);
            if (limit > 1024) return fail(invalid_argument);
            r->port->queue_limit = limit; s.changed.notify_all();
        } else {
            std::vector<std::uint32_t> words;
            if (flavor == 1) words = {r->port->queue_limit};
            else if (flavor == 2) {
                bool member = false;
                for (const auto &[key, value] : t->names) { (void)key; if (value.refs[3] && value.port->members.contains(r->port)) member = true; }
                auto ownership = s.owners();
                const auto send_refs = ownership.send[r->port.get()], once_refs = ownership.once[r->port.get()];
                words = {static_cast<std::uint32_t>(member), r->port->sequence, r->port->make_send_count,
                    r->port->queue_limit, static_cast<std::uint32_t>(r->port->queue.size()), static_cast<unsigned>(once_refs),
                    static_cast<std::uint32_t>(send_refs != 0), static_cast<unsigned>(!!r->port->destroyed.destination),
                    static_cast<unsigned>(!!r->port->no_senders.destination),
                    (r->port->guarded ? 2u : 0u) | (r->port->strict_guard ? 4u : 0u) | (r->port->immovable ? 0x40u : 0u)};
            } else return fail(invalid_argument);
            if (req.args[3] < words.size()) return fail(invalid_argument);
            out.args[0] = words.size(); out.data.resize(words.size() * 4);
            std::memcpy(out.data.data(), words.data(), out.data.size());
        }
        break;
    }
    case operation::get_context: case operation::set_context:
        if (!r) return fail(invalid_name);
        if (!r->refs[1]) return fail(invalid_right);
        if (req.op == operation::get_context) out.args[0] = r->port->context;
        else {
            if (r->port->strict_guard) return fail(invalid_argument);
            r->port->context = req.args[2];
            if (r->port->guarded) r->port->guard = req.args[2];
        }
        break;
    case operation::guard: case operation::unguard:
        if (!r) return fail(invalid_name);
        if (!r->refs[1]) return fail(invalid_right);
        if (req.op == operation::guard) {
            if (r->port->guarded) return fail(invalid_argument);
            r->port->guarded = true; r->port->strict_guard = req.args[3] != 0;
            r->port->context = r->port->guard = req.args[2];
        } else {
            if (!r->port->guarded || r->port->guard != req.args[2]) return fail(invalid_argument);
            r->port->guard = 0; r->port->guarded = r->port->strict_guard = r->port->immovable = false;
        }
        break;
    case operation::request_notification: {
        if (!r) return fail(invalid_name);
        const auto id = static_cast<unsigned>(req.args[2]), sync = static_cast<unsigned>(req.args[3]);
        const auto destination_name = static_cast<name>(req.args[4]);
        const auto disposition = req.args[5];
        if (id != 69 && id != 70 && id != 72 && id != 66) return fail(18);
        if ((id == 69 || id == 70) && !r->refs[1]) return fail(invalid_right);
        if (id == 69 && sync) return fail(18);
        if ((id == 72 || id == 66) && !(r->refs[0] || r->refs[1] || r->refs[2] || r->refs[4])) return fail(invalid_right);
        if (r->refs[4] && (!sync || !destination_name)) return fail(invalid_argument);
        if (r->refs[4] && r->refs[4] == 65535) return fail(urefs_overflow);
        auto *destination = get(destination_name);
        if (destination_name && (!destination || !destination->port->alive)) return fail(20);
        if (destination_name && (disposition != 18 && disposition != 21)) return fail(invalid_argument);
        if (destination_name && !destination->refs[disposition == 18 ? 2 : 1]) return fail(invalid_right);
        auto &slot = id == 69 ? r->port->destroyed : id == 70 ? r->port->no_senders : r->notification;
        /* Current XNU does not allow replacing a port-destroyed request. */
        if (id == 69 && slot.destination && destination_name) return fail(5);
        const auto fresh = destination_name ? destination->port : nullptr;
        const auto previous = std::move(slot.destination);
        slot = {fresh, id, sync, id == 66 && sync != 0};
        if (fresh && disposition == 18) {
            --destination->refs[2];
            if (!used(*destination)) s.erase_name(*t, destination_name, true);
        }
        if (previous) out.args[0] = s.import_right(*t, t->id, {0, 18, 0, previous});
        /* Reacquire after importing a previous request can rehash names. */
        r = get(n);
        if (r && r->refs[4] && fresh) {
            ++r->refs[4]; r->notification.destination.reset(); s.notify(fresh, 72, n);
        }
        break;
    }
    case operation::watch: case operation::watch_status: case operation::unwatch: {
        r = get(static_cast<name>(req.args[0]));
        if (!r || (!r->refs[1] && !r->refs[3])) return fail(invalid_right);
        if (req.op == operation::watch) {
            file_descriptor fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
            if (fd.get() < 0) return fail(6);
            const auto id = ++s.watch_id;
            r->port->watchers.push_back({caller, id, fd});
            s.refresh_one(r->port); out.args[0] = id; out.descriptors.push_back(fd);
        } else if (req.op == operation::unwatch) {
            std::erase_if(r->port->watchers, [&](const auto &watch) { return watch.owner == caller && watch.id == req.args[1]; });
        } else { out.args[0] = state::pending(r->port); out.args[1] = r->port->alive; }
        break;
    }
    case operation::memory_create: {
        if (req.args[0] != 1 || req.descriptors.size() != 1 || req.descriptors[0].get() < 0) return fail(invalid_argument);
        struct stat info{};
        if (fstat(req.descriptors[0].get(), &info) < 0 || !S_ISREG(info.st_mode) ||
            !req.args[1] || req.args[3] > static_cast<std::uint64_t>(info.st_size) ||
            req.args[1] > static_cast<std::uint64_t>(info.st_size) - req.args[3] || (req.args[2] & ~7u)) return fail(invalid_argument);
        auto port = std::make_shared<object>(); port->kind_ = object::kind::memory;
        port->memory = req.descriptors[0]; port->memory_size = req.args[1]; port->protection = static_cast<unsigned>(req.args[2]);
        port->memory_offset = req.args[3];
        out.args[0] = s.import_send(*t, port);
        break;
    }
    case operation::memory_get: {
        r = get(static_cast<name>(req.args[0]));
        if (!r || !r->refs[0] || r->port->kind_ != object::kind::memory) return fail(invalid_right);
        out.args[0] = r->port->memory_size; out.args[1] = r->port->protection; out.args[2] = r->port->memory_offset;
        out.descriptors.push_back(r->port->memory); break;
    }
    case operation::task_for_pid: {
        std::shared_ptr<task> target;
        for (const auto &[id, other] : s.tasks) { (void)id;
            if (other->pid == req.args[0] && other->uid == t->uid && other->alive) { target = other; break; }
        }
        if (!target) return fail(invalid_task);
        const auto self = target->names.find(1);
        if (self == target->names.end()) return fail(invalid_task);
        out.args[0] = s.import_send(*t, self->second.port); break;
    }
    case operation::thread_self: {
        auto &port = t->threads[req.args[0]];
        if (!port) { port = std::make_shared<object>(); port->kind_ = object::kind::thread; port->target = caller; }
        out.args[0] = s.import_send(*t, port); break;
    }
    case operation::exception_get: case operation::exception_set: case operation::exception_swap: {
        r = get(static_cast<name>(req.args[0]));
        if (!r || !r->refs[0] || (r->port->kind_ != object::kind::task && r->port->kind_ != object::kind::thread)) return fail(invalid_right);
        const auto target = r->port;
        auto &handlers = target->exceptions;
        const auto mask = req.args[1];
        if (mask & ~0x3ffeu) return fail(invalid_argument);
        std::shared_ptr<object> replacement;
        if (req.op != operation::exception_get) {
            auto *handler = get(static_cast<name>(req.args[2]));
            if (req.args[2] && (!handler || !handler->refs[0] || !handler->port->alive)) return fail(invalid_right);
            if (handler) replacement = handler->port;
            const auto behavior = req.args[3] & ~0x80000000ull;
            if (replacement && (behavior < 1 || behavior > 3)) return fail(invalid_argument);
        }
        if (req.op != operation::exception_set) {
            struct row { unsigned mask; object::exception handler; };
            std::vector<row> rows;
            for (unsigned i = 1; i < 14; ++i) if (mask & (1ull << i)) {
                auto handler = handlers[i];
                if (handler.port && !handler.port->alive) handler.port.reset();
                auto it = std::ranges::find_if(rows, [&](const auto &r_) {
                    return r_.handler.port == handler.port && r_.handler.behavior == handler.behavior && r_.handler.flavor == handler.flavor;
                });
                if (it != rows.end()) it->mask |= 1u << i;
                else rows.push_back({1u << i, handler});
            }
            out.args[0] = rows.size();
            if (req.args[5] < rows.size()) return fail(3);
            for (const auto &row : rows) {
                const auto imported = row.handler.port ? s.import_send(*t, row.handler.port) : 0;
                if (row.handler.port && !imported) return fail(urefs_overflow);
                const unsigned values[] = {row.mask, imported, row.handler.behavior, row.handler.flavor};
                const auto offset = out.data.size(); out.data.resize(offset + sizeof values);
                std::memcpy(out.data.data() + offset, values, sizeof values);
            }
        }
        if (req.op != operation::exception_get) {
            for (unsigned i = 1; i < 14; ++i) if (mask & (1ull << i))
                handlers[i] = {replacement, static_cast<unsigned>(req.args[3]), static_cast<unsigned>(req.args[4])};
        }
        break;
    }
    case operation::send: {
        if (req.data.size() < 24) return fail(0x10000008);
        const auto bits = load<std::uint32_t>(req.data, 0);
        const auto inline_size = load<std::uint32_t>(req.data, 4);
        const auto destination = load<name>(req.data, 8);
        if (inline_size < 24 || inline_size > req.data.size() || (bits & ~0x801f1f1fu)) return fail(0x10000010);
        if (req.data.size() > 64 * 1024 * 1024) return fail(0x1000000e);
        auto *destination_right = get(destination);
        if (!destination_right || !destination_right->port->alive || destination_right->port->is_set ||
            destination_right->port->kind_ != object::kind::queue) return fail(0x10000003);
        const auto port = destination_right->port;
        const auto remote_kind = bits & 31u;
        if (remote_kind != 17 && remote_kind != 18 && remote_kind != 19) return fail(0x1000000a);
        if (!destination_right->refs[remote_kind == 18 ? 2 : 0]) return fail(0x1000000a);
        const auto ready = [&] { return !t->alive || !port->alive || remote_kind == 18 || port->queue.size() < port->queue_limit; };
        if (!ready()) {
            if (req.args[0] & 0x10) {
                if (!s.changed.wait_for(lock, std::chrono::milliseconds(req.args[1]), ready)) {
                    if ((req.args[0] & 0x20) && (destination_right = get(destination)) && destination_right->notification.id == 66)
                        destination_right->notification.armed = true;
                    return fail(0x10000004);
                }
            } else s.changed.wait(lock, ready);
        }
        if (!t->alive || !port->alive) return fail(0x10000003);
        struct pending { name n; unsigned disposition; transferred_right right; };
        std::vector<pending> transfer;
        std::unordered_map<name, std::array<std::uint32_t, 5>> remaining;
        auto reserve = [&](name from, unsigned disposition, std::size_t offset) -> bool {
            transferred_right value; value.offset = offset;
            value.kind = disposition == 16 ? 16 : disposition == 18 || disposition == 21 ? 18 : 17;
            if (from == 0 || from == ~0u) {
                value.special_name = from; transfer.push_back({from, disposition, value}); return true;
            }
            auto *source = get(from);
            if (!source || !source->port->alive || disposition < 16 || disposition > 21 || source->port->is_set) return false;
            if (disposition == 16 && source->port->immovable) return false;
            const auto needed = disposition == 16 || disposition == 20 || disposition == 21 ? 1u : disposition == 18 ? 2u : 0u;
            auto [it, inserted] = remaining.try_emplace(from, source->refs); (void)inserted;
            if (!it->second[needed]) return false;
            if (disposition == 16 || disposition == 17 || disposition == 18) --it->second[needed];
            if (disposition == 16 && source->port == port) return false; /* Circular receive transfer. */
            value.port = source->port; transfer.push_back({from, disposition, std::move(value)});
            return true;
        };
        /* Destination consumption participates in the same transaction as
         * reply and body rights, including repeated MOVE_SEND descriptors. */
        destination_right = get(destination);
        if (!destination_right) return fail(0x10000003);
        remaining.emplace(destination, destination_right->refs);
        if (!remaining[destination][remote_kind == 18 ? 2 : 0]) return fail(0x1000000a);
        if (remote_kind == 17 || remote_kind == 18) --remaining[destination][remote_kind == 18 ? 2 : 0];
        const auto reply = load<name>(req.data, 12);
        const auto reply_kind = (bits >> 8) & 31u;
        if (reply && (reply_kind < 17 || reply_kind > 21)) return fail(0x10000009);
        if (reply && !reserve(reply, reply_kind, 8)) return fail(0x10000009);
        if (load<name>(req.data, 16)) return fail(0x10000005); /* Vouchers require voucher objects. */
        queued_message message; message.bytes = req.data; message.pid = t->pid; message.uid = t->uid;
        if (bits & 0x80000000u) {
            if (inline_size < 28) return fail(0x1000000f);
            const auto count = load<std::uint32_t>(req.data, 24);
            std::size_t offset = 28;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (offset + 12 > inline_size) return fail(0x1000000f);
                const auto type = req.data[offset + 11];
                if (type == 0) {
                    const auto disposition = req.data[offset + 10];
                    if (!reserve(load<name>(req.data, offset), disposition, offset)) return fail(0x1000000a);
                    message.bytes[offset + 10] = static_cast<std::uint8_t>(disposition == 16 ? 16 : disposition == 18 || disposition == 21 ? 18 : 17);
                    offset += 12;
                } else if (type == 1 || type == 2 || type == 3) {
                    if (offset + 16 > inline_size) return fail(0x1000000f);
                    const auto address = load<std::uint64_t>(req.data, offset);
                    const auto length = load<std::uint32_t>(req.data, offset + 12);
                    const auto size = static_cast<std::uint64_t>(length) * (type == 2 ? 4 : 1);
                    if ((size && address < inline_size) || address > req.data.size() || size > req.data.size() - address) return fail(0x1000000c);
                    if (req.data[offset + 9] > 1) return fail(0x1000000f);
                    if (type == 2) {
                        const auto disposition = req.data[offset + 10];
                        for (std::uint32_t j = 0; j < length; ++j)
                            if (!reserve(load<name>(req.data, static_cast<std::size_t>(address) + j * 4), disposition,
                                         static_cast<std::size_t>(address) + j * 4)) return fail(0x1000000a);
                        message.bytes[offset + 10] = static_cast<std::uint8_t>(disposition == 16 ? 16 : disposition == 18 || disposition == 21 ? 18 : 17);
                    }
                    /* Received OOL mappings belong to the receiver and
                     * mach_msg_destroy must deallocate them. */
                    message.bytes[offset + 8] = 1;
                    message.bytes[offset + 9] = 1;
                    offset += 16;
                } else if (type == 4) {
                    if (offset + 16 > inline_size) return fail(0x1000000f);
                    const auto flags = load<std::uint16_t>(req.data, offset + 8);
                    const auto context = load<std::uint64_t>(req.data, offset);
                    const auto name = load<mm_ipc::name>(req.data, offset + 12);
                    const auto *source = get(name);
                    if ((flags & ~3u) || req.data[offset + 10] != 16) return fail(0x1000000f);
                    if (!source || !source->refs[1]) return fail(0x1000000a);
                    if (!((source->port->guarded && source->port->guard == context) ||
                          ((flags & 2u) && !source->port->guarded && !context))) return fail(0x10000012);
                    if (!reserve(name, 16, offset + 12)) return fail(0x1000000a);
                    offset += 16;
                } else return fail(0x1000000f);
            }
        }
        /* No namespace mutation happens before all descriptors validate. */
        for (const auto &[key, refs] : remaining) t->names.at(key).refs = refs;
        for (const auto &item : transfer) {
            if (item.right.port && item.disposition == 16) {
                item.right.port->receiver = 0;
                item.right.port->sequence = item.right.port->make_send_count = 0;
                item.right.port->guard = item.right.port->context = 0;
                item.right.port->guarded = item.right.port->strict_guard = item.right.port->immovable = false;
                for (auto &[key, value] : t->names) { (void)key; if (value.refs[3]) value.port->members.erase(item.right.port); }
            }
            if (item.right.port && item.disposition == 20) ++item.right.port->make_send_count;
            message.rights.push_back(item.right);
        }
        for (auto it = t->names.begin(); it != t->names.end();) {
            if (!used(it->second)) { const auto deleted = it++->first; s.erase_name(*t, deleted, true); }
            else ++it;
        }
        store(message.bytes, 0, (bits & 0x80000000u) | (reply ? (reply_kind == 18 || reply_kind == 21 ? 18u : 17u) : 0u) |
            ((remote_kind == 18 ? 18u : 17u) << 8));
        store<name>(message.bytes, 8, 0); store<name>(message.bytes, 12, 0);
        message.order = ++s.message_order;
        port->queue.push_back(std::move(message)); s.changed.notify_all();
        break;
    }
    case operation::receive: {
        const auto receive_name = static_cast<name>(req.args[0]);
        r = get(receive_name);
        if (!r || (!r->refs[1] && !r->refs[3])) return fail(0x10004002);
        const auto receive_object = r->port;
        if (!receive_object->is_set) {
            for (const auto &[key, value] : t->names) { (void)key;
                if (value.refs[3] && value.port->members.contains(receive_object)) return fail(0x1000400a);
            }
        }
        auto next_port = [&]() -> std::shared_ptr<object> {
            if (!receive_object->is_set) return receive_object->queue.empty() ? nullptr : receive_object;
            std::shared_ptr<object> selected;
            for (const auto &member : receive_object->members)
                if (member->alive && !member->queue.empty() && (!selected || member->queue.front().order < selected->queue.front().order)) selected = member;
            return selected;
        };
        const auto ready = [&] { return !t->alive || !receive_object->alive || next_port() != nullptr; };
        if (req.args[1] & 0x100) {
            if (!s.changed.wait_for(lock, std::chrono::milliseconds(req.args[2]), ready)) return fail(0x10004003);
        } else s.changed.wait(lock, ready);
        if (!t->alive || !receive_object->alive) return fail(0x10004009);
        auto source = next_port();
        if (!source) return fail(0x10004006);
        name actual = receive_name;
        if (receive_object->is_set) {
            for (const auto &[key, value] : t->names) if (value.refs[1] && value.port == source) { actual = key; break; }
        }
        auto &queued = source->queue.front();
        const auto inline_size = load<std::uint32_t>(queued.bytes, 4);
        out.args[0] = inline_size; out.args[1] = actual;
        if (req.args[3] < inline_size) {
            if (!(req.args[1] & 4)) { s.discard_message(queued); source->queue.pop_front(); s.changed.notify_all(); }
            return fail(0x10004004);
        }
        out.data = queued.bytes;
        store<name>(out.data, 12, actual);
        for (const auto &value : queued.rights) {
            const auto imported = s.import_right(*t, caller, value);
            store<name>(out.data, value.offset, imported);
        }
        if (load<std::uint32_t>(out.data, 0) & 0x80000000u) {
            std::size_t offset = 28;
            for (unsigned i = 0; i < load<unsigned>(out.data, 24); ++i) {
                const auto type = out.data[offset + 11];
                if (type == 4) {
                    auto flags = load<std::uint16_t>(out.data, offset + 8);
                    const auto name = load<mm_ipc::name>(out.data, offset + 12);
                    const auto *right = get(name);
                    std::uint64_t context = 0;
                    if ((flags & 1u) && right) {
                        context = req.args[4];
                        right->port->guarded = right->port->immovable = true;
                        right->port->context = right->port->guard = context;
                        flags &= ~2u;
                    } else flags |= 2u;
                    store(out.data, offset, context); store(out.data, offset + 8, flags);
                }
                offset += type == 0 ? 12 : 16;
            }
        }
        out.args[2] = source->sequence++; out.args[3] = source->context;
        out.args[4] = queued.pid; out.args[5] = queued.uid;
        /* Escrow rights now belong to the receiving task. */
        queued.rights.clear(); source->queue.pop_front(); s.changed.notify_all();
        break;
    }
    case operation::bootstrap_register: case operation::bootstrap_lookup:
    case operation::bootstrap_create_service: case operation::bootstrap_check_in: case operation::bootstrap_status: {
        if (req.args[0] != 3) return fail(1100);
        if (req.data.empty() || req.data.size() > 127 || std::ranges::find(req.data, 0) != req.data.end()) return fail(1104);
        const std::string service_name(req.data.begin(), req.data.end());
        auto service = s.services.find(service_name);
        /* launchd permits an unmanaged process to check in a new dynamic
         * service without bootstrap_create_service. Lookup remains unknown
         * until this first check-in. See launchd src/core.c
         * job_mig_check_in2 and Crashpad's upstream bootstrap test. */
        if (req.op == operation::bootstrap_check_in && service == s.services.end()) {
            auto port = std::make_shared<object>();
            service = s.services.emplace(service_name, state::service{port, caller, false}).first;
        }
        if (req.op == operation::bootstrap_create_service) {
            if (service != s.services.end()) return fail(1101);
            auto port = std::make_shared<object>();
            s.services.emplace(service_name, state::service{port, caller, false});
            out.args[0] = s.import_send(*t, port);
        } else if (req.op == operation::bootstrap_register) {
            if (!n) {
                if (service == s.services.end()) return fail(1102);
                if (service->second.registrant != caller) return fail(1100);
                s.services.erase(service); break;
            }
            if (!r || !r->refs[0] || !r->port->alive) return fail(invalid_right);
            if (service != s.services.end()) return fail(1101);
            s.services.emplace(service_name, state::service{r->port, caller, true});
        } else {
            if (service == s.services.end() || !service->second.port->alive) return fail(1102);
            auto &record = service->second;
            if (req.op == operation::bootstrap_status) { out.args[0] = record.checked_in ? 1 : 0; break; }
            if (req.op == operation::bootstrap_check_in) {
                if (record.checked_in) return fail(1103);
                if (record.registrant != caller) return fail(1100);
                const auto assigned = s.unused_name(*t);
                right receive; receive.port = record.port; receive.refs[1] = 1;
                t->names.emplace(assigned, std::move(receive));
                record.port->receiver = caller; record.checked_in = true; out.args[0] = assigned;
            } else {
                out.args[0] = s.import_send(*t, record.port);
                if (!out.args[0]) return fail(urefs_overflow);
            }
        }
        break;
    }
    default: return fail(46); /* Unimplemented operations are not success. */
    }
    s.refresh_notifications();
    s.refresh_watches();
    return out;
}
}
