#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static void require(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "inet address failed: %s\n", what); exit(1); }
}
static void roundtrip(int family, const char *input, const unsigned char *expected, size_t size) {
    unsigned char address[32];
    memset(address, 0xa5, sizeof address);
    errno = EDOM;
    require(inet_pton(family, input, address) == 1, "parse IP literal");
    require(errno == EDOM, "successful parsing preserves errno");
    require(memcmp(address, expected, size) == 0 && address[size] == 0xa5, "network byte order and bounds");
    char text[INET6_ADDRSTRLEN];
    require(inet_ntop(family, address, text, sizeof text) == text, "format IP literal");
    require(strcmp(text, input) == 0, "IPv4/IPv6 roundtrip");
    require(inet_ntop(family, address, text, 1) == NULL && errno == ENOSPC, "small output buffer");
}
int main(void) {
    require(AF_INET6 == 30, "Darwin SDK IPv6 family");
    const unsigned char v4[] = {192, 0, 2, 1};
    const unsigned char v6[] = {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const unsigned char mapped[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1};
    roundtrip(AF_INET, "192.0.2.1", v4, sizeof v4);
    roundtrip(AF_INET6, "2001:db8::1", v6, sizeof v6);
    roundtrip(AF_INET6, "::ffff:192.0.2.1", mapped, sizeof mapped);
    for (int family_index = 0; family_index < 2; ++family_index) {
        const int family = family_index ? AF_INET6 : AF_INET;
        unsigned char address[16], unchanged[16];
        memset(address, 0xa5, sizeof address);
        memcpy(unchanged, address, sizeof address);
        errno = EDOM;
        require(inet_pton(family, "example.com", address) == 0, "DNS name is not an IP literal");
        require(errno == EDOM && memcmp(address, unchanged, sizeof address) == 0, "invalid text preserves buffer and errno");
        require(inet_pton(family, "not-an-address", address) == 0, "invalid literal");
    }
    unsigned char address[16] = {0};
    char text[64];
    require(inet_pton(AF_UNSPEC, "::1", address) == -1 && errno == EAFNOSUPPORT, "unsupported family errno");
    require(inet_pton(10, "::1", address) == -1 && errno == EAFNOSUPPORT, "Linux family cannot alias a Darwin family");
    require(inet_ntop(AF_UNSPEC, address, text, sizeof text) == NULL && errno == EAFNOSUPPORT, "unsupported format family");
    puts("SDK inet IPv4/IPv6 literals, DNS classification, network bytes and Darwin errno passed");
    return 0;
}
