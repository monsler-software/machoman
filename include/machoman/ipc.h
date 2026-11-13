#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mm_ipc {
using task_id = std::uint64_t;
using name = std::uint32_t;
class file_descriptor {
public:
    file_descriptor() = default;
    explicit file_descriptor(int fd);
    int get() const { return fd_ ? *fd_ : -1; }
private:
    std::shared_ptr<int> fd_;
};
enum class operation : std::uint32_t {
    allocate, get_refs, mod_refs, type, destroy, deallocate, extract, insert,
    rename, names, move_member, insert_member, extract_member, members,
    get_attributes, set_attributes, get_context, set_context, guard, unguard,
    request_notification, send, receive, bootstrap_register, bootstrap_lookup,
    bootstrap_create_service, bootstrap_check_in, bootstrap_status,
    memory_create, memory_get, task_for_pid, exception_get, exception_set, watch,
    watch_status, unwatch, thread_self, construct, destruct, set_mscount,
    set_seqno, exception_swap, _count,
};
struct request {
    operation op{};
    std::array<std::uint64_t, 12> args{};
    std::vector<std::uint8_t> data;
    std::vector<file_descriptor> descriptors;
};
struct response {
    std::uint32_t status = 0;
    std::array<std::uint64_t, 12> args{};
    std::vector<std::uint8_t> data;
    std::vector<file_descriptor> descriptors;
};
/* The broker owns objects; task-local names never serve as global IDs.
 * Calls from different task threads can block independently. */
class kernel {
public:
    kernel();
    ~kernel();
    kernel(const kernel &) = delete;
    kernel &operator=(const kernel &) = delete;
    void add_task(task_id id, std::uint32_t pid, std::uint32_t uid);
    void remove_task(task_id id);
    response transact(task_id caller, const request &message);
private:
    struct state;
    std::unique_ptr<state> state_;
};

/* Linux broker transport; client identity comes from SO_PEERCRED. */
int serve(const char *venv_root);
response exchange(const request &message);
/* Runtime callers pass immutable launch context separately from the guest
 * environment. The one-argument form is for standalone transport clients. */
response exchange(const request &message, const char *venv_root, const char *launcher_path);
}
