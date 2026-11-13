#define __APPLE_USE_RFC_3542 1
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "socket messages: %s (errno=%d %s)\n", message, errno, strerror(errno)); exit(1); }
}
static void await_packet(int fd) {
    struct pollfd p = {fd,POLLIN,0};
    check(poll(&p,1,1000) == 1 && (p.revents & POLLIN), "datagram becomes readable");
}
static void test_udp(int family) {
    int receiver = socket(family, SOCK_DGRAM, 0), sender = socket(family, SOCK_DGRAM, 0);
    check(receiver >= 0 && sender >= 0, "create UDP sockets");
    struct sockaddr_storage destination = {0};
    socklen_t length;
    if (family == AF_INET) {
        struct sockaddr_in *a = (void *)&destination;
        a->sin_len = sizeof(*a); a->sin_family = family; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); length = sizeof(*a);
    } else {
        struct sockaddr_in6 *a = (void *)&destination;
        a->sin6_len = sizeof(*a); a->sin6_family = family; a->sin6_addr = in6addr_loopback; length = sizeof(*a);
    }
    check(bind(receiver, (void *)&destination, length) == 0, "bind loopback");
    check(getsockname(receiver, (void *)&destination, &length) == 0, "discover UDP port");
    int enabled = 1;
    check(setsockopt(receiver, SOL_SOCKET, SO_TIMESTAMP, &enabled, sizeof(enabled)) == 0, "receive timestamps");
    check(setsockopt(receiver, family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
        family == AF_INET ? IP_PKTINFO : IPV6_RECVPKTINFO, &enabled, sizeof(enabled)) == 0, "receive packet information");
    struct iovec send_vectors[] = {{"packet",6},{"-",1},{"payload",7}};
    struct msghdr outgoing;
    memset(&outgoing, 0xa5, sizeof outgoing); // poison padding beside the 32-bit iovlen
    outgoing.msg_name = &destination; outgoing.msg_namelen = length;
    outgoing.msg_iov = send_vectors; outgoing.msg_iovlen = 3;
    outgoing.msg_control = NULL; outgoing.msg_controllen = 0; outgoing.msg_flags = 0;
    struct msghdr original = outgoing;
    check(sendmsg(sender, &outgoing, MSG_NOSIGNAL) == 14, "scatter-gather UDP sendmsg");
    await_packet(receiver);
    check(memcmp(&outgoing, &original, sizeof outgoing) == 0, "sendmsg leaves header unchanged");
    char first[6], second[8];
    struct iovec receive_vectors[] = {{first,sizeof first},{second,sizeof second}};
    struct sockaddr_storage source;
    union { struct cmsghdr align; char bytes[256]; } control;
    struct { struct msghdr header; unsigned long long guard; } received;
    memset(&received, 0xa5, sizeof received);
    received.guard = 0x1122334455667788ULL;
    received.header.msg_name = &source; received.header.msg_namelen = sizeof source;
    received.header.msg_iov = receive_vectors; received.header.msg_iovlen = 2;
    received.header.msg_control = control.bytes; received.header.msg_controllen = sizeof control.bytes;
    check(recvmsg(receiver, &received.header, MSG_DONTWAIT) == 14, "scatter-gather UDP recvmsg");
    check(received.guard == 0x1122334455667788ULL && received.header.msg_iovlen == 2, "recvmsg respects Darwin header size");
    check(memcmp(first,"packet",6) == 0 && memcmp(second,"-payload",8) == 0, "received all payload bytes");
    check(source.ss_family == family && received.header.msg_namelen == length, "sender address has Darwin family and length");
    check(received.header.msg_flags == 0, "complete receive flags");
    int timestamp = 0, packet_info = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&received.header); c; c = CMSG_NXTHDR(&received.header,c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP) {
            struct timeval t; check(c->cmsg_len == CMSG_LEN(sizeof t), "timestamp control size");
            memcpy(&t,CMSG_DATA(c),sizeof t); check(t.tv_sec > 0 && t.tv_usec >= 0 && t.tv_usec < 1000000, "timestamp payload"); timestamp = 1;
        }
        if (family == AF_INET && c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo info; check(c->cmsg_len == CMSG_LEN(sizeof info), "IPv4 packet control size");
            memcpy(&info,CMSG_DATA(c),sizeof info);
            check(info.ipi_ifindex != 0 && info.ipi_addr.s_addr == htonl(INADDR_LOOPBACK), "IPv4 destination metadata"); packet_info = 1;
        }
        if (family == AF_INET6 && c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo info; check(c->cmsg_len == CMSG_LEN(sizeof info), "IPv6 packet control size");
            memcpy(&info,CMSG_DATA(c),sizeof info);
            check(info.ipi6_ifindex != 0 && IN6_IS_ADDR_LOOPBACK(&info.ipi6_addr), "IPv6 destination metadata"); packet_info = 1;
        }
    }
    check(timestamp && packet_info, "multiple Darwin-aligned control headers");
    check(recvmsg(receiver, &received.header, MSG_DONTWAIT) == -1 && errno == EAGAIN, "nonblocking empty receive");
    check(setsockopt(receiver, family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
        family == AF_INET ? IP_RECVTTL : IPV6_RECVHOPLIMIT, &enabled, sizeof(enabled)) == 0, "receive hop limit");
    check(setsockopt(receiver, family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
        family == AF_INET ? IP_RECVTOS : IPV6_RECVTCLASS, &enabled, sizeof(enabled)) == 0, "receive traffic class");
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))*2]; } send_control;
    memset(&send_control,0,sizeof send_control);
    outgoing.msg_control = send_control.bytes; outgoing.msg_controllen = sizeof send_control.bytes;
    struct cmsghdr *hop = CMSG_FIRSTHDR(&outgoing);
    hop->cmsg_len = CMSG_LEN(sizeof(int)); hop->cmsg_level = family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
    hop->cmsg_type = family == AF_INET ? IP_TTL : IPV6_HOPLIMIT;
    int hop_value = 42; memcpy(CMSG_DATA(hop),&hop_value,sizeof hop_value);
    struct cmsghdr *traffic = CMSG_NXTHDR(&outgoing,hop);
    check(traffic != NULL,"second outgoing control header");
    traffic->cmsg_len = CMSG_LEN(sizeof(int)); traffic->cmsg_level = hop->cmsg_level;
    traffic->cmsg_type = family == AF_INET ? IP_TOS : IPV6_TCLASS;
    int traffic_value = 0x28; memcpy(CMSG_DATA(traffic),&traffic_value,sizeof traffic_value);
    check(sendmsg(sender,&outgoing,0) == 14,"send multiple ancillary headers");
    await_packet(receiver);
    received.header.msg_namelen = sizeof source; received.header.msg_controllen = sizeof control.bytes;
    check(recvmsg(receiver,&received.header,MSG_DONTWAIT) == 14 && !received.header.msg_flags,"receive hop limit and traffic class");
    int saw_hop = 0, saw_traffic = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&received.header); c; c = CMSG_NXTHDR(&received.header,c)) {
        if (c->cmsg_level != hop->cmsg_level) continue;
        if (c->cmsg_type == (family == AF_INET ? IP_RECVTTL : IPV6_HOPLIMIT)) {
            int value = 0;
            if (family == AF_INET) { check(c->cmsg_len == CMSG_LEN(1),"Darwin IPv4 TTL is a byte"); value = *CMSG_DATA(c); }
            else { check(c->cmsg_len == CMSG_LEN(sizeof(int)),"IPv6 hop limit size"); memcpy(&value,CMSG_DATA(c),sizeof value); }
            check(value == hop_value,"outgoing hop limit applied"); saw_hop = 1;
        }
        if (c->cmsg_type == (family == AF_INET ? IP_RECVTOS : IPV6_TCLASS)) {
            int value = 0;
            if (family == AF_INET) { check(c->cmsg_len == CMSG_LEN(1),"Darwin IPv4 TOS is a byte"); value = *CMSG_DATA(c); }
            else { check(c->cmsg_len == CMSG_LEN(sizeof(int)),"IPv6 traffic class size"); memcpy(&value,CMSG_DATA(c),sizeof value); }
            check(value == traffic_value,"outgoing traffic class applied"); saw_traffic = 1;
        }
    }
    check(saw_hop && saw_traffic,"translated hop limit and traffic class metadata");
    outgoing.msg_control = NULL; outgoing.msg_controllen = 0;
    check(connect(sender,(void *)&destination,length) == 0, "connected UDP socket");
    outgoing.msg_name = NULL; outgoing.msg_namelen = 0;
    check(sendmsg(sender,&outgoing,0) == 14, "connected sendmsg without address");
    await_packet(receiver);
    char small[3]; struct iovec short_vector = {small,sizeof small};
    struct msghdr short_message = {0}; short_message.msg_iov = &short_vector; short_message.msg_iovlen = 1;
    check(recvmsg(receiver,&short_message,MSG_PEEK | MSG_DONTWAIT) == 3 && (short_message.msg_flags & MSG_TRUNC), "peek preserves truncated datagram");
    check(recvmsg(receiver,&short_message,MSG_TRUNC | MSG_DONTWAIT) == 14 && (short_message.msg_flags & MSG_TRUNC), "MSG_TRUNC returns original datagram length");
    check(recv(receiver,small,sizeof small,MSG_DONTWAIT) == -1 && errno == EAGAIN, "recv flag translation");
    check(send(sender,"ok",2,MSG_NOSIGNAL) == 2, "send flag translation");
    await_packet(receiver);
    check(recvfrom(receiver,small,sizeof small,MSG_DONTWAIT,NULL,NULL) == 2 && memcmp(small,"ok",2) == 0, "recvfrom without address");
    check(sendto(sender,"ok",2,MSG_NOSIGNAL,(void *)&destination,length) == 2, "sendto flag translation");
    await_packet(receiver);
    struct { unsigned char bytes[4]; unsigned guard; } short_address = {{0},0xabcdef01};
    short_message.msg_name = short_address.bytes; short_message.msg_namelen = sizeof short_address.bytes;
    check(recvmsg(receiver,&short_message,MSG_DONTWAIT) == 2 && short_message.msg_namelen == length && short_address.guard == 0xabcdef01, "truncated sender address reports full length without overflow");
    check(sendmsg(sender,&outgoing,0x40000000) == -1 && errno == ENOTSUP, "unknown flags fail explicitly");
    outgoing.msg_iovlen = -1;
    check(sendmsg(sender,&outgoing,0) == -1 && errno == EINVAL, "negative iov count");
    outgoing.msg_iovlen = 1025;
    check(sendmsg(sender,&outgoing,0) == -1 && errno == EMSGSIZE, "excessive iov count and Darwin EMSGSIZE");
    close(sender); close(receiver);
}
static void test_rights(void) {
    int sockets[2], pipes[2];
    check(socketpair(AF_UNIX,SOCK_DGRAM,0,sockets) == 0 && pipe(pipes) == 0,"create descriptor fixture");
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int)*2)]; } control;
    memset(&control,0,sizeof control);
    struct iovec vector = {"x",1};
    struct msghdr outgoing = {0}; outgoing.msg_iov = &vector; outgoing.msg_iovlen = 1;
    outgoing.msg_control = control.bytes; outgoing.msg_controllen = sizeof control.bytes;
    struct cmsghdr *c = CMSG_FIRSTHDR(&outgoing);
    c->cmsg_len = CMSG_LEN(sizeof(int)*2); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(c),pipes,sizeof pipes);
    check(sendmsg(sockets[0],&outgoing,0) == 1,"send descriptor rights");
    await_packet(sockets[1]);
    char byte; struct iovec incoming_vector = {&byte,1};
    struct msghdr incoming = {0}; incoming.msg_iov = &incoming_vector; incoming.msg_iovlen = 1;
    incoming.msg_control = control.bytes; incoming.msg_controllen = sizeof control.bytes;
    check(recvmsg(sockets[1],&incoming,MSG_DONTWAIT) == 1,"receive descriptor rights");
    c = CMSG_FIRSTHDR(&incoming);
    check(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(int)*2) && !incoming.msg_flags,"Darwin SCM_RIGHTS header");
    int received[2]; memcpy(received,CMSG_DATA(c),sizeof received);
    check(write(received[1],"q",1) == 1 && read(received[0],&byte,1) == 1 && byte == 'q',"received descriptors are usable");
    close(received[0]); close(received[1]);
    c->cmsg_len = CMSG_LEN(sizeof(int)*2); memcpy(CMSG_DATA(c),pipes,sizeof pipes);
    check(sendmsg(sockets[0],&outgoing,0) == 1,"send rights before control truncation");
    await_packet(sockets[1]);
    incoming.msg_controllen = CMSG_SPACE(sizeof(int));
    check(recvmsg(sockets[1],&incoming,MSG_DONTWAIT) == 1 && (incoming.msg_flags & MSG_CTRUNC),"small control buffer reports truncation");
    c = CMSG_FIRSTHDR(&incoming);
    check(c && c->cmsg_len == CMSG_LEN(sizeof(int)),"complete descriptor retained in truncated control buffer");
    memcpy(&received[0],CMSG_DATA(c),sizeof(int)); close(received[0]);
    c->cmsg_len = sizeof(*c)-1;
    check(sendmsg(sockets[0],&outgoing,0) == -1 && errno == EINVAL,"malformed control header rejected");
    c->cmsg_len = CMSG_LEN(sizeof(int)); c->cmsg_type = 999;
    check(sendmsg(sockets[0],&outgoing,0) == -1 && errno == ENOPROTOOPT,"unknown ancillary contract rejected");
    close(pipes[0]); close(pipes[1]); close(sockets[0]); close(sockets[1]);
}
int main(void) {
    _Static_assert(sizeof(struct msghdr) == 48,"Darwin message header");
    _Static_assert(sizeof(struct cmsghdr) == 12,"Darwin ancillary header");
    test_udp(AF_INET); test_udp(AF_INET6); test_rights();
    check(strstr(strerror(EAFNOSUPPORT),"Address family") != NULL,"Darwin strerror for address family error");
    check(strstr(strerror(EMSGSIZE),"Message too long") != NULL,"Darwin strerror for datagram size error");
    puts("Darwin socket messages, flags, ancillary data and errors passed");
    return 0;
}
