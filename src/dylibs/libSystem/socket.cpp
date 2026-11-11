/* Socket options.
 *
 * The BSD socket calls themselves are the same on both systems — same
 * argument order and same `SOCK_STREAM`. Addresses and resolver results
 * are translated below. The option *names* differ: Darwin's SOL_SOCKET is
 * 0xffff where Linux's is 1, and every SO_ constant differs, so a
 * setsockopt that is not translated fails with an error the caller reads
 * as "this system is broken" (nanomsg aborts on it).
 *
 * Options Darwin has and Linux does not are honoured where they have a
 * meaning here and accepted where they do not: refusing them would stop a
 * program that is only asking for a nicety. */
#include "internal.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace {

constexpr int darwin_sol_socket = 0xFFFF;

/* Darwin's SO_ values (sys/socket.h). */
enum : int {
    d_SO_DEBUG = 0x0001, d_SO_ACCEPTCONN = 0x0002, d_SO_REUSEADDR = 0x0004, d_SO_KEEPALIVE = 0x0008,
    d_SO_DONTROUTE = 0x0010, d_SO_BROADCAST = 0x0020, d_SO_LINGER = 0x0080, d_SO_OOBINLINE = 0x0100,
    d_SO_REUSEPORT = 0x0200, d_SO_TIMESTAMP = 0x0400, d_SO_SNDBUF = 0x1001, d_SO_RCVBUF = 0x1002,
    d_SO_SNDLOWAT = 0x1003, d_SO_RCVLOWAT = 0x1004, d_SO_SNDTIMEO = 0x1005, d_SO_RCVTIMEO = 0x1006,
    d_SO_ERROR = 0x1007, d_SO_TYPE = 0x1008, d_SO_NOSIGPIPE = 0x1022, d_SO_LINGER_SEC = 0x1080,
};

/* The host option a Darwin one means. Returns false for an option that has
 * no host equivalent; `handled` says it was dealt with here instead. */
bool translate_option(int level, int name, int &host_level, int &host_name, bool &handled) {
    handled = false;
    if (level != darwin_sol_socket) {
        /* IPPROTO_* levels and their options match between the two. */
        host_level = level;
        host_name = name;
        return true;
    }
    host_level = SOL_SOCKET;
    switch (name) {
    case d_SO_DEBUG: host_name = SO_DEBUG; return true;
    case d_SO_ACCEPTCONN: host_name = SO_ACCEPTCONN; return true;
    case d_SO_REUSEADDR: host_name = SO_REUSEADDR; return true;
    case d_SO_KEEPALIVE: host_name = SO_KEEPALIVE; return true;
    case d_SO_DONTROUTE: host_name = SO_DONTROUTE; return true;
    case d_SO_BROADCAST: host_name = SO_BROADCAST; return true;
    case d_SO_LINGER: case d_SO_LINGER_SEC: host_name = SO_LINGER; return true;
    case d_SO_OOBINLINE: host_name = SO_OOBINLINE; return true;
    case d_SO_REUSEPORT: host_name = SO_REUSEPORT; return true;
    case d_SO_TIMESTAMP: host_name = SO_TIMESTAMP; return true;
    case d_SO_SNDBUF: host_name = SO_SNDBUF; return true;
    case d_SO_RCVBUF: host_name = SO_RCVBUF; return true;
    case d_SO_SNDLOWAT: host_name = SO_SNDLOWAT; return true;
    case d_SO_RCVLOWAT: host_name = SO_RCVLOWAT; return true;
    case d_SO_SNDTIMEO: host_name = SO_SNDTIMEO; return true;
    case d_SO_RCVTIMEO: host_name = SO_RCVTIMEO; return true;
    case d_SO_ERROR: host_name = SO_ERROR; return true;
    case d_SO_TYPE: host_name = SO_TYPE; return true;
    case d_SO_NOSIGPIPE:
        /* Darwin turns off SIGPIPE per socket; Linux does it per send, with
         * MSG_NOSIGNAL. A program asking for it wants a failed write to
         * return EPIPE rather than kill the process, so ignoring SIGPIPE
         * for the process is the same thing from where it stands. */
        std::signal(SIGPIPE, SIG_IGN);
        handled = true;
        return false;
    default:
        handled = true; /* an option this system does not have: accepted */
        return false;
    }
}

/* ---- addresses ----------------------------------------------------------- */

/* A Darwin sockaddr starts with its own length: `{ uint8 sa_len; uint8
 * sa_family; ... }` where Linux has a 16-bit family and no length. The
 * address families differ too (AF_INET6 is 30 on Darwin, 10 here), so
 * every address a guest passes has to be rebuilt before the host sees it,
 * and every address the host returns has to be rebuilt the other way.
 * Without this a bind() of a Unix socket fails, which is what stopped
 * Ableton Live from talking to its extension host. */
enum : int { d_AF_UNIX = 1, d_AF_INET = 2, d_AF_INET6 = 30 };

int family_to_host(int family) {
    switch (family) {
    case d_AF_UNIX: return AF_UNIX;
    case d_AF_INET: return AF_INET;
    case d_AF_INET6: return AF_INET6;
    default: return family;
    }
}
int family_to_darwin(int family) {
    switch (family) {
    case AF_UNIX: return d_AF_UNIX;
    case AF_INET: return d_AF_INET;
    case AF_INET6: return d_AF_INET6;
    default: return family;
    }
}

/* Builds the host form of a guest address. Returns the length to pass on,
 * or 0 when the address is not one this can translate. */
socklen_t address_to_host(const void *from, socklen_t length, sockaddr_storage &out) {
    if (!from || length < 2) return 0;
    const auto *bytes = static_cast<const std::uint8_t *>(from);
    const int family = family_to_host(bytes[1]);
    std::memset(&out, 0, sizeof out);
    out.ss_family = static_cast<sa_family_t>(family);
    switch (family) {
    case AF_UNIX: {
        auto &un = reinterpret_cast<sockaddr_un &>(out);
        const char *path = reinterpret_cast<const char *>(bytes + 2);
        const std::size_t max = sizeof un.sun_path - 1;
        std::size_t n = 0;
        while (n < max && n + 2 < static_cast<std::size_t>(length) && path[n]) n++;
        std::memcpy(un.sun_path, path, n);
        un.sun_path[n] = '\0';
        return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + n + 1);
    }
    case AF_INET: {
        auto &in4 = reinterpret_cast<sockaddr_in &>(out);
        std::memcpy(&in4.sin_port, bytes + 2, 2);      /* both are network order */
        std::memcpy(&in4.sin_addr, bytes + 4, 4);
        return sizeof(sockaddr_in);
    }
    case AF_INET6: {
        auto &in6 = reinterpret_cast<sockaddr_in6 &>(out);
        std::memcpy(&in6.sin6_port, bytes + 2, 2);
        std::memcpy(&in6.sin6_flowinfo, bytes + 4, 4);
        std::memcpy(&in6.sin6_addr, bytes + 8, 16);
        std::memcpy(&in6.sin6_scope_id, bytes + 24, 4);
        return sizeof(sockaddr_in6);
    }
    default:
        return 0;
    }
}

/* Writes a host address back in Darwin's form, updating *length the way
 * the BSD calls do. */
void address_to_darwin(const sockaddr_storage &from, socklen_t from_length, void *out, socklen_t *length) {
    if (!out || !length) return;
    std::uint8_t buffer[128] = {};
    socklen_t size = 0;
    switch (from.ss_family) {
    case AF_UNIX: {
        const auto &un = reinterpret_cast<const sockaddr_un &>(from);
        const std::size_t path = from_length > offsetof(sockaddr_un, sun_path)
                                     ? strnlen(un.sun_path, sizeof un.sun_path) : 0;
        size = static_cast<socklen_t>(2 + path + 1);
        std::memcpy(buffer + 2, un.sun_path, path);
        break;
    }
    case AF_INET: {
        const auto &in4 = reinterpret_cast<const sockaddr_in &>(from);
        size = 16;
        std::memcpy(buffer + 2, &in4.sin_port, 2);
        std::memcpy(buffer + 4, &in4.sin_addr, 4);
        break;
    }
    case AF_INET6: {
        const auto &in6 = reinterpret_cast<const sockaddr_in6 &>(from);
        size = 28;
        std::memcpy(buffer + 2, &in6.sin6_port, 2);
        std::memcpy(buffer + 4, &in6.sin6_flowinfo, 4);
        std::memcpy(buffer + 8, &in6.sin6_addr, 16);
        std::memcpy(buffer + 24, &in6.sin6_scope_id, 4);
        break;
    }
    default:
        size = static_cast<socklen_t>(std::min<socklen_t>(from_length, sizeof buffer));
        break;
    }
    buffer[0] = static_cast<std::uint8_t>(size);
    buffer[1] = static_cast<std::uint8_t>(family_to_darwin(from.ss_family));
    const socklen_t copied = std::min(size, *length);
    std::memcpy(out, buffer, copied);
    *length = size;
}

/* Darwin places ai_canonname before ai_addr; Linux reverses them. Returning
 * libc's list directly makes the guest dereference a NULL canonical name
 * as its sockaddr, as Python's socket.getaddrinfo did during Live startup. */
struct darwin_addrinfo {
    int flags, family, socktype, protocol;
    socklen_t addrlen;
    char *canonname;
    void *addr;
    darwin_addrinfo *next;
};
static_assert(sizeof(darwin_addrinfo) == 48 && offsetof(darwin_addrinfo, addr) == 32);

struct resolver_error { int host, darwin; };
constexpr resolver_error resolver_errors[] = {
    {EAI_AGAIN, 2}, {EAI_BADFLAGS, 3}, {EAI_FAIL, 4}, {EAI_FAMILY, 5},
    {EAI_MEMORY, 6}, {EAI_NONAME, 8}, {EAI_SERVICE, 9}, {EAI_SOCKTYPE, 10},
    {EAI_SYSTEM, 11}, {EAI_OVERFLOW, 14},
#ifdef EAI_ADDRFAMILY
    {EAI_ADDRFAMILY, 1},
#endif
#ifdef EAI_NODATA
    {EAI_NODATA, 7},
#endif
};
int resolver_error_to_darwin(int code) {
    if (!code) return 0;
    for (auto error : resolver_errors) if (error.host == code) return error.darwin;
    return 4;
}
int resolver_flags_to_host(int flags) {
    int out = flags & 7; /* AI_PASSIVE, AI_CANONNAME, AI_NUMERICHOST match */
    if (flags & 0x100) out |= AI_ALL;
    if (flags & (0x200 | 0x800)) out |= AI_V4MAPPED;
    if (flags & 0x400) out |= AI_ADDRCONFIG;
    if (flags & 0x1000) out |= AI_NUMERICSERV;
    return out;
}
void free_darwin_addrinfo(darwin_addrinfo *list) {
    while (list) {
        auto *next = list->next;
        std::free(list->addr); std::free(list->canonname); std::free(list);
        list = next;
    }
}

} // namespace

extern "C" {

int darwin_getaddrinfo(const char *name, const char *service, const darwin_addrinfo *hints,
                       darwin_addrinfo **result) DARWIN(getaddrinfo);
int darwin_getaddrinfo(const char *name, const char *service, const darwin_addrinfo *hints,
                       darwin_addrinfo **result) {
    if (!result) return 4;
    *result = nullptr;
    addrinfo host_hints{};
    if (hints) {
        if (hints->flags & ~0x10001f07) return 3;
        host_hints.ai_flags = resolver_flags_to_host(hints->flags);
        host_hints.ai_family = family_to_host(hints->family);
        host_hints.ai_socktype = hints->socktype;
        host_hints.ai_protocol = hints->protocol;
    }
    addrinfo *host = nullptr;
    const int rc = ::getaddrinfo(name, service, hints ? &host_hints : nullptr, &host);
    if (rc) return resolver_error_to_darwin(rc);
    darwin_addrinfo *head = nullptr, **tail = &head;
    int error = 0;
    for (auto *entry = host; entry; entry = entry->ai_next) {
        auto *node = static_cast<darwin_addrinfo *>(std::calloc(1, sizeof(darwin_addrinfo)));
        if (!node) { error = 6; break; }
        *tail = node; tail = &node->next;
        node->flags = hints ? hints->flags : 0;
        node->family = family_to_darwin(entry->ai_family);
        node->socktype = entry->ai_socktype; node->protocol = entry->ai_protocol;
        if (entry->ai_canonname) {
            node->canonname = ::strdup(entry->ai_canonname);
            if (!node->canonname) { error = 6; break; }
        }
        if (entry->ai_addr) {
            if (entry->ai_addrlen > sizeof(sockaddr_storage)) { error = 5; break; }
            sockaddr_storage address{};
            std::memcpy(&address, entry->ai_addr, entry->ai_addrlen);
            node->addr = std::calloc(1, sizeof(sockaddr_storage));
            if (!node->addr) { error = 6; break; }
            node->addrlen = sizeof(sockaddr_storage);
            address_to_darwin(address, entry->ai_addrlen, node->addr, &node->addrlen);
        }
    }
    ::freeaddrinfo(host);
    if (error) { free_darwin_addrinfo(head); return error; }
    *result = head;
    return 0;
}
void darwin_freeaddrinfo(darwin_addrinfo *list) DARWIN(freeaddrinfo);
void darwin_freeaddrinfo(darwin_addrinfo *list) { free_darwin_addrinfo(list); }
const char *darwin_gai_strerror(int code) DARWIN(gai_strerror);
const char *darwin_gai_strerror(int code) {
    for (auto error : resolver_errors) if (error.darwin == code) return ::gai_strerror(error.host);
    return code ? "Unknown address resolution error" : "Success";
}
int darwin_getnameinfo(const void *address, socklen_t length, char *name, socklen_t name_size,
                      char *service, socklen_t service_size, int flags) DARWIN(getnameinfo);
int darwin_getnameinfo(const void *address, socklen_t length, char *name, socklen_t name_size,
                      char *service, socklen_t service_size, int flags) {
    if (flags & ~0x13f) return 3;
    int host_flags = 0;
    if (flags & 1) host_flags |= NI_NOFQDN;
    if (flags & 2) host_flags |= NI_NUMERICHOST;
    if (flags & 4) host_flags |= NI_NAMEREQD;
    if (flags & 8) host_flags |= NI_NUMERICSERV;
    if (flags & 16) host_flags |= NI_DGRAM;
    sockaddr_storage host{};
    const socklen_t host_length = address_to_host(address, length, host);
    if (!host_length) return 5;
    return resolver_error_to_darwin(::getnameinfo(reinterpret_cast<const sockaddr *>(&host), host_length,
        name, name_size, service, service_size, host_flags));
}

int darwin_socket(int domain, int type, int protocol) DARWIN(socket);
int darwin_socket(int domain, int type, int protocol) { return ::socket(family_to_host(domain), type, protocol); }

int darwin_bind(int fd, const void *address, socklen_t length) DARWIN(bind);
int darwin_bind(int fd, const void *address, socklen_t length) {
    sockaddr_storage host{};
    const socklen_t host_length = address_to_host(address, length, host);
    if (!host_length) { errno = EAFNOSUPPORT; return -1; }
    return ::bind(fd, reinterpret_cast<const sockaddr *>(&host), host_length);
}

int darwin_connect(int fd, const void *address, socklen_t length) DARWIN(connect);
int darwin_connect(int fd, const void *address, socklen_t length) {
    sockaddr_storage host{};
    const socklen_t host_length = address_to_host(address, length, host);
    if (!host_length) { errno = EAFNOSUPPORT; return -1; }
    return ::connect(fd, reinterpret_cast<const sockaddr *>(&host), host_length);
}

int darwin_accept(int fd, void *address, socklen_t *length) DARWIN(accept);
int darwin_accept(int fd, void *address, socklen_t *length) {
    sockaddr_storage host{};
    socklen_t host_length = sizeof host;
    const int accepted = ::accept(fd, address ? reinterpret_cast<sockaddr *>(&host) : nullptr,
                                  address ? &host_length : nullptr);
    if (accepted >= 0 && address) address_to_darwin(host, host_length, address, length);
    return accepted;
}

int darwin_getsockname(int fd, void *address, socklen_t *length) DARWIN(getsockname);
int darwin_getsockname(int fd, void *address, socklen_t *length) {
    sockaddr_storage host{};
    socklen_t host_length = sizeof host;
    const int rc = ::getsockname(fd, reinterpret_cast<sockaddr *>(&host), &host_length);
    if (rc == 0) address_to_darwin(host, host_length, address, length);
    return rc;
}

int darwin_getpeername(int fd, void *address, socklen_t *length) DARWIN(getpeername);
int darwin_getpeername(int fd, void *address, socklen_t *length) {
    sockaddr_storage host{};
    socklen_t host_length = sizeof host;
    const int rc = ::getpeername(fd, reinterpret_cast<sockaddr *>(&host), &host_length);
    if (rc == 0) address_to_darwin(host, host_length, address, length);
    return rc;
}

ssize_t darwin_sendto(int fd, const void *buffer, std::size_t n, int flags, const void *address, socklen_t length) DARWIN(sendto);
ssize_t darwin_sendto(int fd, const void *buffer, std::size_t n, int flags, const void *address, socklen_t length) {
    if (!address) return ::send(fd, buffer, n, flags);
    sockaddr_storage host{};
    const socklen_t host_length = address_to_host(address, length, host);
    if (!host_length) { errno = EAFNOSUPPORT; return -1; }
    return ::sendto(fd, buffer, n, flags, reinterpret_cast<const sockaddr *>(&host), host_length);
}

ssize_t darwin_recvfrom(int fd, void *buffer, std::size_t n, int flags, void *address, socklen_t *length) DARWIN(recvfrom);
ssize_t darwin_recvfrom(int fd, void *buffer, std::size_t n, int flags, void *address, socklen_t *length) {
    if (!address) return ::recv(fd, buffer, n, flags);
    sockaddr_storage host{};
    socklen_t host_length = sizeof host;
    const ssize_t received = ::recvfrom(fd, buffer, n, flags, reinterpret_cast<sockaddr *>(&host), &host_length);
    if (received >= 0) address_to_darwin(host, host_length, address, length);
    return received;
}

int darwin_setsockopt(int fd, int level, int name, const void *value, socklen_t length) DARWIN(setsockopt);
int darwin_setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
    int host_level = 0, host_name = 0;
    bool handled = false;
    if (!translate_option(level, name, host_level, host_name, handled)) {
        if (handled) return 0;
        errno = ENOPROTOOPT;
        return -1;
    }
    return ::setsockopt(fd, host_level, host_name, value, length);
}

int darwin_getsockopt(int fd, int level, int name, void *value, socklen_t *length) DARWIN(getsockopt);
int darwin_getsockopt(int fd, int level, int name, void *value, socklen_t *length) {
    int host_level = 0, host_name = 0;
    bool handled = false;
    if (!translate_option(level, name, host_level, host_name, handled)) {
        if (handled) {
            /* Report it as off, which is what an option nobody set reads as. */
            if (value && length && *length >= sizeof(int)) {
                const int off = 0;
                std::memcpy(value, &off, sizeof off);
                *length = sizeof off;
            }
            return 0;
        }
        errno = ENOPROTOOPT;
        return -1;
    }
    return ::getsockopt(fd, host_level, host_name, value, length);
}

} // extern "C"
