#include "internal.h"
#include <cerrno>
#include <climits>
#include <ctime>
#include <limits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
constexpr unsigned no_errno = 0x01000000, deadline = 0x02000000;
constexpr unsigned wake_all = 0x100, wake_thread = 0x200;
constexpr unsigned wait_flags = no_errno | deadline | 0x70000;
constexpr unsigned wake_flags = no_errno | wake_all | wake_thread | 0x400;

int finish(unsigned operation, int error, int saved_errno) {
    if (operation & no_errno) {
        errno = saved_errno;
        return error ? -mm_darwin_errno(error) : 0;
    }
    if (error) { errno = error; return -1; }
    errno = saved_errno;
    return 0;
}
bool supported(unsigned opcode) { return opcode >= 1 && opcode <= 3; }
bool aligned(void *address) { return (reinterpret_cast<std::uintptr_t>(address) & 3) == 0; }

int wait(unsigned operation, void *address, std::uint64_t value, std::uint64_t timeout) {
    const int saved_errno = errno;
    const unsigned opcode = operation & 0xff;
    if (!supported(opcode)) return finish(operation, opcode >= 4 && opcode <= 6 ? ENOTSUP : EINVAL, saved_errno);
    if (operation & ~(wait_flags | 0xff)) return finish(operation, EINVAL, saved_errno);
    if (!address) return finish(operation, EFAULT, saved_errno);
    if (!aligned(address)) return finish(operation, EINVAL, saved_errno);
    /* XNU compares the zero-extended 32-bit word with the full expected value. */
    if (value > std::numeric_limits<std::uint32_t>::max()) return finish(operation, 0, saved_errno);
    timespec duration{};
    if (timeout) {
        if (operation & deadline) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const auto ns = static_cast<std::uint64_t>(now.tv_sec) * 1000000000 + static_cast<std::uint64_t>(now.tv_nsec);
            timeout = timeout > ns ? timeout - ns : 1;
        }
        duration.tv_sec = static_cast<time_t>(timeout / 1000000000);
        duration.tv_nsec = static_cast<long>(timeout % 1000000000);
    }
    const int flags = opcode == 3 ? 0 : FUTEX_PRIVATE_FLAG;
    const auto result = syscall(SYS_futex, address, FUTEX_WAIT | flags,
                                static_cast<std::uint32_t>(value), timeout ? &duration : nullptr, nullptr, 0);
    /* Darwin returns success when the value has already changed. */
    const int error = result < 0 && errno != EAGAIN ? errno : 0;
    return finish(operation, error, saved_errno);
}
}

extern "C" {
int darwin_ulock_wait(unsigned, void *, std::uint64_t, std::uint32_t) DARWIN(__ulock_wait);
int darwin_ulock_wait(unsigned operation, void *address, std::uint64_t value, std::uint32_t timeout) {
    if (operation & deadline) return finish(operation, EINVAL, errno);
    return wait(operation, address, value, static_cast<std::uint64_t>(timeout) * 1000);
}
int darwin_ulock_wait2(unsigned, void *, std::uint64_t, std::uint64_t, std::uint64_t) DARWIN(__ulock_wait2);
int darwin_ulock_wait2(unsigned operation, void *address, std::uint64_t value, std::uint64_t timeout, std::uint64_t) {
    return wait(operation, address, value, timeout);
}
int darwin_ulock_wake(unsigned, void *, std::uint64_t) DARWIN(__ulock_wake);
int darwin_ulock_wake(unsigned operation, void *address, std::uint64_t) {
    const int saved_errno = errno;
    const unsigned opcode = operation & 0xff;
    if (!supported(opcode)) return finish(operation, opcode >= 4 && opcode <= 6 ? ENOTSUP : EINVAL, saved_errno);
    if (operation & ~(wake_flags | 0xff)) return finish(operation, EINVAL, saved_errno);
    if (operation & wake_thread) return finish(operation, ENOTSUP, saved_errno);
    if (!address) return finish(operation, EINVAL, saved_errno);
    if (!aligned(address)) return finish(operation, EINVAL, saved_errno);
    const int flags = opcode == 3 ? 0 : FUTEX_PRIVATE_FLAG;
    const auto result = syscall(SYS_futex, address, FUTEX_WAKE | flags, operation & wake_all ? INT_MAX : 1, nullptr, nullptr, 0);
    return finish(operation, result < 0 ? errno : result == 0 ? ENOENT : 0, saved_errno);
}
}
