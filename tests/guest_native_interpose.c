#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

extern int native_original_pid(void);
int main(void) {
    const int original = native_original_pid();
    if (getpid() != original + 12345) return 1;
    int (*dynamic_pid)(void) = dlsym(RTLD_DEFAULT, "getpid");
    if (!dynamic_pid || dynamic_pid() != original + 12345) return 2;
    puts("native interposition and original-function fallback passed");
    return 0;
}
