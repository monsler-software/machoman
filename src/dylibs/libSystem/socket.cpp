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

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <new>
#include <vector>

namespace {

void trace_message(const char *operation, int fd, const msghdr &message, int flags, ssize_t result) {
    const int saved_errno = errno;
    const char *enabled = std::getenv("MACHOMAN_SOCKET_TRACE");
    if (enabled && enabled[0] && std::strcmp(enabled, "0")) {
        char ip[INET6_ADDRSTRLEN] = "connected";
        unsigned port = 0;
        if (message.msg_name && message.msg_namelen >= sizeof(sa_family_t)) {
            const auto *address = static_cast<const sockaddr *>(message.msg_name);
            if (address->sa_family == AF_INET && message.msg_namelen >= sizeof(sockaddr_in)) {
                const auto *a = reinterpret_cast<const sockaddr_in *>(address);
                ::inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip); port = ntohs(a->sin_port);
            } else if (address->sa_family == AF_INET6 && message.msg_namelen >= sizeof(sockaddr_in6)) {
                const auto *a = reinterpret_cast<const sockaddr_in6 *>(address);
                ::inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip); port = ntohs(a->sin6_port);
            }
        }
        std::fprintf(stderr, "[socket] %s fd=%d address=%s port=%u iov=%zu control=%zu flags=0x%x result=%zd host_errno=%d\n",
            operation, fd, ip, port, message.msg_iovlen, message.msg_controllen, flags, result, result < 0 ? saved_errno : 0);
    }
    errno = saved_errno;
}

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
    host_level = level;
    if (level == IPPROTO_IP) {
        switch (name) {
        case 1: host_name = IP_OPTIONS; return true;
        case 2: host_name = IP_HDRINCL; return true;
        case 3: host_name = IP_TOS; return true;
        case 4: host_name = IP_TTL; return true;
        case 5: host_name = IP_RECVOPTS; return true;
        case 9: host_name = IP_MULTICAST_IF; return true;
        case 10: host_name = IP_MULTICAST_TTL; return true;
        case 11: host_name = IP_MULTICAST_LOOP; return true;
        case 12: host_name = IP_ADD_MEMBERSHIP; return true;
        case 13: host_name = IP_DROP_MEMBERSHIP; return true;
        case 24: host_name = IP_RECVTTL; return true;
        case 26: host_name = IP_PKTINFO; return true;
        case 27: host_name = IP_RECVTOS; return true;
        default: return false;
        }
    }
    if (level == IPPROTO_IPV6) {
        switch (name) {
        case 4: host_name = IPV6_UNICAST_HOPS; return true;
        case 9: host_name = IPV6_MULTICAST_IF; return true;
        case 10: host_name = IPV6_MULTICAST_HOPS; return true;
        case 11: host_name = IPV6_MULTICAST_LOOP; return true;
        case 12: host_name = IPV6_JOIN_GROUP; return true;
        case 13: host_name = IPV6_LEAVE_GROUP; return true;
        case 27: host_name = IPV6_V6ONLY; return true;
        case 35: host_name = IPV6_RECVTCLASS; return true;
        case 36: host_name = IPV6_TCLASS; return true;
        case 37: host_name = IPV6_RECVHOPLIMIT; return true;
        case 61: host_name = IPV6_RECVPKTINFO; return true;
        default: return false;
        }
    }
    if (level != darwin_sol_socket) {
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
        if (length < 16) return 0;
        auto &in4 = reinterpret_cast<sockaddr_in &>(out);
        std::memcpy(&in4.sin_port, bytes + 2, 2);      /* both are network order */
        std::memcpy(&in4.sin_addr, bytes + 4, 4);
        return sizeof(sockaddr_in);
    }
    case AF_INET6: {
        if (length < 28) return 0;
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

/* ---- message I/O -------------------------------------------------------- */

/* Darwin uses int for iovlen, socklen_t for controllen, and four-byte
 * alignment for control messages. Linux uses size_t and eight-byte
 * alignment. Only the iovec itself has the same layout on both ABIs. */
struct darwin_msghdr {
    void *name;
    socklen_t namelen;
    iovec *iov;
    int iovlen;
    void *control;
    socklen_t controllen;
    int flags;
};
struct darwin_cmsghdr { std::uint32_t len; int level, type; };
static_assert(sizeof(darwin_msghdr) == 48 && offsetof(darwin_msghdr, flags) == 44);
static_assert(sizeof(darwin_cmsghdr) == 12);
static_assert(sizeof(iovec) == 16 && offsetof(iovec, iov_len) == 8);
constexpr std::size_t align_control(std::size_t n) { return (n + 3) & ~std::size_t(3); }

struct message_flag { int darwin, host; };
constexpr message_flag message_flags[] = {
    {0x1, MSG_OOB}, {0x2, MSG_PEEK}, {0x4, MSG_DONTROUTE},
    {0x8, MSG_EOR}, {0x10, MSG_TRUNC}, {0x20, MSG_CTRUNC},
    {0x40, MSG_WAITALL}, {0x80, MSG_DONTWAIT}, {0x80000, MSG_NOSIGNAL},
};
bool flags_to_host(int flags, int &out) {
    out = 0;
    for (auto f : message_flags) { if (flags & f.darwin) out |= f.host; flags &= ~f.darwin; }
    if (flags) { errno = ENOTSUP; return false; }
    return true;
}
int flags_to_darwin(int flags) {
    int out = 0;
    for (auto f : message_flags) if (flags & f.host) out |= f.darwin;
    return out;
}
bool message_to_host(const darwin_msghdr *guest, msghdr &host, sockaddr_storage &address) {
    if (!guest) { errno = EFAULT; return false; }
    if (guest->iovlen < 0) { errno = EINVAL; return false; }
    if (guest->iovlen > 1024) { errno = EMSGSIZE; return false; }
    if ((guest->iovlen && !guest->iov) || (guest->controllen && !guest->control)) { errno = EFAULT; return false; }
    host.msg_iov = guest->iov;
    host.msg_iovlen = static_cast<std::size_t>(guest->iovlen);
    if (guest->name) { host.msg_name = &address; host.msg_namelen = sizeof address; }
    return true;
}

bool control_type_to_host(int level, int type, int &host_level, int &host_type) {
    host_level = level;
    if (level == darwin_sol_socket) {
        host_level = SOL_SOCKET;
        if (type == 1) { host_type = SCM_RIGHTS; return true; }
        if (type == 2) { host_type = SCM_TIMESTAMP; return true; }
    } else if (level == IPPROTO_IP) {
        if (type == 3) { host_type = IP_TOS; return true; }
        if (type == 4) { host_type = IP_TTL; return true; }
        if (type == 26) { host_type = IP_PKTINFO; return true; }
    } else if (level == IPPROTO_IPV6) {
        if (type == 36) { host_type = IPV6_TCLASS; return true; }
        if (type == 46) { host_type = IPV6_PKTINFO; return true; }
        if (type == 47) { host_type = IPV6_HOPLIMIT; return true; }
    }
    errno = ENOPROTOOPT; return false;
}
bool control_type_to_darwin(int level, int type, int &guest_level, int &guest_type) {
    guest_level = level;
    if (level == SOL_SOCKET) {
        guest_level = darwin_sol_socket;
        if (type == SCM_RIGHTS) { guest_type = 1; return true; }
        if (type == SCM_TIMESTAMP) { guest_type = 2; return true; }
    } else if (level == IPPROTO_IP) {
        if (type == IP_TOS) { guest_type = 27; return true; }
        if (type == IP_TTL) { guest_type = 24; return true; }
        if (type == IP_PKTINFO) { guest_type = 26; return true; }
    } else if (level == IPPROTO_IPV6) {
        if (type == IPV6_TCLASS) { guest_type = 36; return true; }
        if (type == IPV6_PKTINFO) { guest_type = 46; return true; }
        if (type == IPV6_HOPLIMIT) { guest_type = 47; return true; }
    }
    return false;
}

bool control_to_host(const darwin_msghdr &guest, std::vector<std::uint8_t> &out) {
    const auto *data = static_cast<const std::uint8_t *>(guest.control);
    for (std::size_t at = 0; at < guest.controllen;) {
        const std::size_t remaining = guest.controllen - at;
        if (remaining < sizeof(darwin_cmsghdr)) { errno = EINVAL; return false; }
        darwin_cmsghdr header;
        std::memcpy(&header, data + at, sizeof header);
        if (header.len < sizeof header || header.len > remaining) { errno = EINVAL; return false; }
        int level, type;
        if (!control_type_to_host(header.level, header.type, level, type)) return false;
        const std::size_t size = header.len - sizeof header;
        const auto start = out.size(); out.resize(start + CMSG_SPACE(size));
        cmsghdr host{}; host.cmsg_len = CMSG_LEN(size); host.cmsg_level = level; host.cmsg_type = type;
        std::memcpy(out.data() + start, &host, sizeof host);
        std::memcpy(out.data() + start + CMSG_LEN(0), data + at + sizeof header, size);
        if (align_control(header.len) >= remaining) break;
        at += align_control(header.len);
    }
    return true;
}

void close_rights(const cmsghdr &header, const std::uint8_t *payload, std::size_t size, std::size_t retained) {
    if (header.cmsg_level != SOL_SOCKET || header.cmsg_type != SCM_RIGHTS) return;
    for (std::size_t at = retained; at + sizeof(int) <= size; at += sizeof(int)) {
        int fd; std::memcpy(&fd, payload + at, sizeof fd); ::close(fd);
    }
}
void control_to_darwin(const msghdr &host, darwin_msghdr &guest) {
    const auto *data = static_cast<const std::uint8_t *>(host.msg_control);
    auto *out = static_cast<std::uint8_t *>(guest.control);
    const std::size_t capacity = guest.controllen;
    std::size_t used = 0;
    for (std::size_t at = 0; at + sizeof(cmsghdr) <= host.msg_controllen;) {
        cmsghdr header; std::memcpy(&header, data + at, sizeof header);
        if (header.cmsg_len < CMSG_LEN(0) || header.cmsg_len > host.msg_controllen - at) { guest.flags |= 0x20; break; }
        const auto *payload = data + at + CMSG_LEN(0);
        const std::size_t original_size = header.cmsg_len - CMSG_LEN(0);
        std::size_t size = original_size;
        int level, type;
        bool supported = control_type_to_darwin(header.cmsg_level, header.cmsg_type, level, type);
        std::uint8_t ttl = 0;
        if (header.cmsg_level == IPPROTO_IP && header.cmsg_type == IP_TTL && size == sizeof(int)) {
            int value; std::memcpy(&value, payload, sizeof value); ttl = static_cast<std::uint8_t>(value); payload = &ttl; size = 1;
        }
        const bool rights = header.cmsg_level == SOL_SOCKET && header.cmsg_type == SCM_RIGHTS;
        const std::size_t remaining = capacity - used;
        if (rights && remaining >= sizeof(darwin_cmsghdr)) {
            size = std::min(size, ((remaining - sizeof(darwin_cmsghdr)) / sizeof(int)) * sizeof(int));
            if (!size) supported = false;
        }
        if (supported && sizeof(darwin_cmsghdr) + size <= remaining) {
            darwin_cmsghdr guest_header{static_cast<std::uint32_t>(sizeof(darwin_cmsghdr) + size), level, type};
            std::memcpy(out + used, &guest_header, sizeof guest_header);
            std::memcpy(out + used + sizeof guest_header, payload, size);
            const auto written = std::min(align_control(guest_header.len), remaining);
            std::memset(out + used + guest_header.len, 0, written - guest_header.len);
            used += written;
            if (rights && size < original_size) { guest.flags |= 0x20; close_rights(header, payload, original_size, size); }
        } else {
            guest.flags |= 0x20;
            close_rights(header, payload, original_size, 0);
        }
        at += CMSG_ALIGN(header.cmsg_len);
    }
    guest.controllen = static_cast<socklen_t>(used);
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

/* The address bytes match, but AF_INET6 does not. Passing Darwin's 30 to
 * libc yields -1/EAFNOSUPPORT even for a DNS name; callers that use a
 * nonzero result to detect IP literals then misclassify it as IPv6. */
int darwin_inet_pton(int, const char *, void *) DARWIN(inet_pton);
int darwin_inet_pton(int family, const char *text, void *address) {
    if (family != d_AF_INET && family != d_AF_INET6) { errno = EAFNOSUPPORT; return -1; }
    return ::inet_pton(family_to_host(family), text, address);
}
const char *darwin_inet_ntop(int, const void *, char *, socklen_t) DARWIN(inet_ntop);
const char *darwin_inet_ntop(int family, const void *address, char *text, socklen_t size) {
    if (family != d_AF_INET && family != d_AF_INET6) { errno = EAFNOSUPPORT; return nullptr; }
    return ::inet_ntop(family_to_host(family), address, text, size);
}

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
    int host_flags;
    if (!flags_to_host(flags, host_flags)) return -1;
    if (!address) return ::send(fd, buffer, n, host_flags);
    sockaddr_storage host{};
    const socklen_t host_length = address_to_host(address, length, host);
    if (!host_length) { errno = EAFNOSUPPORT; return -1; }
    return ::sendto(fd, buffer, n, host_flags, reinterpret_cast<const sockaddr *>(&host), host_length);
}

ssize_t darwin_recvfrom(int fd, void *buffer, std::size_t n, int flags, void *address, socklen_t *length) DARWIN(recvfrom);
ssize_t darwin_recvfrom(int fd, void *buffer, std::size_t n, int flags, void *address, socklen_t *length) {
    int host_flags;
    if (!flags_to_host(flags, host_flags)) return -1;
    if (!address) return ::recv(fd, buffer, n, host_flags);
    if (!length) { errno = EFAULT; return -1; }
    sockaddr_storage host{};
    socklen_t host_length = sizeof host;
    const ssize_t received = ::recvfrom(fd, buffer, n, host_flags, reinterpret_cast<sockaddr *>(&host), &host_length);
    if (received >= 0) address_to_darwin(host, host_length, address, length);
    return received;
}

ssize_t darwin_send(int fd, const void *buffer, std::size_t n, int flags) DARWIN(send);
ssize_t darwin_send(int fd, const void *buffer, std::size_t n, int flags) { return darwin_sendto(fd, buffer, n, flags, nullptr, 0); }
ssize_t darwin_recv(int fd, void *buffer, std::size_t n, int flags) DARWIN(recv);
ssize_t darwin_recv(int fd, void *buffer, std::size_t n, int flags) { return darwin_recvfrom(fd, buffer, n, flags, nullptr, nullptr); }

ssize_t darwin_sendmsg(int fd, const darwin_msghdr *guest, int flags) DARWIN(sendmsg);
ssize_t darwin_sendmsg(int fd, const darwin_msghdr *guest, int flags) {
    int host_flags; msghdr host{}; sockaddr_storage address{};
    if (!flags_to_host(flags, host_flags) || !message_to_host(guest, host, address)) return -1;
    if (guest->name) {
        host.msg_namelen = address_to_host(guest->name, guest->namelen, address);
        if (!host.msg_namelen) { errno = EAFNOSUPPORT; return -1; }
    }
    try {
        std::vector<std::uint8_t> control;
        if (!control_to_host(*guest, control)) return -1;
        if (!control.empty()) { host.msg_control = control.data(); host.msg_controllen = control.size(); }
        const ssize_t result = ::sendmsg(fd, &host, host_flags);
        trace_message("sendmsg", fd, host, host_flags, result);
        return result;
    } catch (const std::bad_alloc &) { errno = ENOMEM; return -1; }
}
ssize_t darwin_recvmsg(int fd, darwin_msghdr *guest, int flags) DARWIN(recvmsg);
ssize_t darwin_recvmsg(int fd, darwin_msghdr *guest, int flags) {
    int host_flags; msghdr host{}; sockaddr_storage address{};
    if (!flags_to_host(flags, host_flags) || !message_to_host(guest, host, address)) return -1;
    try {
        /* Enough space for the larger Linux headers/alignment. Any rights
         * that cannot fit the guest buffer are closed during conversion. */
        std::vector<std::uint8_t> control;
        if (guest->controllen) {
            control.resize(static_cast<std::size_t>(guest->controllen) * 2 + sizeof(cmsghdr));
            host.msg_control = control.data(); host.msg_controllen = control.size();
        }
        const ssize_t result = ::recvmsg(fd, &host, host_flags);
        trace_message("recvmsg", fd, host, host_flags, result);
        if (result < 0) return result;
        guest->flags = flags_to_darwin(host.msg_flags);
        if (guest->name) address_to_darwin(address, host.msg_namelen, guest->name, &guest->namelen);
        else guest->namelen = 0;
        control_to_darwin(host, *guest);
        return result;
    } catch (const std::bad_alloc &) { errno = ENOMEM; return -1; }
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
