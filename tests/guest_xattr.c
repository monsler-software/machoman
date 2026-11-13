#include <sys/xattr.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "xattr: %s, errno %d\n", message, errno); abort(); }
}
int main(void) {
    char path[] = "/tmp/machoman-xattr-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "temporary file");
    const char *name = "org.example.compatibility";
    const char value[] = {'a', 0, 'b', 'c'};
    check(fgetxattr(fd, name, NULL, 0, 0, 0) == -1 && errno == ENOATTR, "missing Darwin attribute");
    check(setxattr(path, name, value, sizeof value, 0, XATTR_CREATE) == 0, "create attribute");
    check(fsetxattr(fd, name, value, sizeof value, 0, XATTR_CREATE) == -1 && errno == EEXIST, "create existing fails");
    check(getxattr(path, name, NULL, 0, 0, 0) == sizeof value, "query size");
    char bytes[80];
    check(fgetxattr(fd, name, bytes, sizeof bytes, 0, 0) == sizeof value && !memcmp(bytes, value, sizeof value), "binary value round trip");
    check(fsetxattr(fd, name, "new", 3, 0, XATTR_REPLACE) == 0, "replace flag");
    check(flistxattr(fd, NULL, 0, 0) == strlen(name) + 1, "list size");
    check(listxattr(path, bytes, sizeof bytes, 0) == strlen(name) + 1 && !strcmp(bytes, name), "Darwin names in list");
    check(listxattr(path, bytes, 1, 0) == -1 && errno == ERANGE, "small list buffer");
    check(removexattr(path, name, 0) == 0, "remove by path");
    check(fsetxattr(fd, name, value, sizeof value, 0, 0) == 0, "set by descriptor");
    check(fremovexattr(fd, name, 0) == 0, "remove by descriptor");
    close(fd); unlink(path);
    puts("Darwin xattr names, binary values, flags, listing and errno passed");
    return 0;
}
