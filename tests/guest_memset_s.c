#define __STDC_WANT_LIB_EXT1__ 1
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "memset_s: %s\n", message); exit(1); }
}
int main(void) {
    unsigned char data[6] = {1, 2, 3, 4, 5, 6};
    errno = EDOM;
    check(memset_s(data + 1, 4, 0x123, 2) == 0, "success");
    check(data[0] == 1 && data[1] == 0x23 && data[2] == 0x23 && data[3] == 4 && data[5] == 6, "count and byte conversion");
    check(memset_s(data + 1, 4, 0, 5) == EOVERFLOW, "count exceeds capacity");
    check(data[0] == 1 && data[1] == 0 && data[4] == 0 && data[5] == 6, "error wipes bounded destination");
    check(memset_s(data + 1, 4, 7, RSIZE_MAX + 1) == E2BIG && data[1] == 7 && data[4] == 7, "huge count wipes capacity");
    check(memset_s(data, RSIZE_MAX + 1, 0, 1) == E2BIG && data[0] == 1, "invalid capacity does not write");
    check(memset_s(NULL, 0, 0, 0) == EINVAL, "null even for zero size");
    check(memset_s(data, 0, 0, 1) == EOVERFLOW && data[0] == 1, "zero capacity bounds");
    check(memset_s(data, sizeof data, 0, 0) == 0 && data[0] == 1, "empty write");
    check(errno == EDOM, "return errors preserve errno");
    puts("SDK memset_s bounds, error erasure and Darwin error codes passed");
    return 0;
}
