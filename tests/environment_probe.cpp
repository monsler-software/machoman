#include <cstdlib>
#include <cstring>
extern "C" int host_environment_unchanged() __asm__("_mm_test_host_environment");
extern "C" int host_environment_unchanged() {
    const char *value=std::getenv("MM_HOST_CONTROL");
    return value && std::strcmp(value, "host")==0;
}
