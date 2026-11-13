#include <unistd.h>

int native_original_pid(void) { return getpid(); }
static int replacement_pid(void) { return native_original_pid() + 12345; }
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } interpose = {
    (const void *)replacement_pid, (const void *)getpid
};
