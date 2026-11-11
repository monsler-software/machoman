#include "mevk/context.h"

#include <cstdio>
#include <string>

int main() {
    std::string error;
    auto context = mevk::Context::create(error);
    if (!context) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const auto& info = context->device_info();
    std::printf("MEVK Vulkan backend\nDevice: %s\nVulkan: %u.%u.%u\nQueue family: %u\n",
                info.name.c_str(), (info.api_version >> 22) & 0x7fu,
                (info.api_version >> 12) & 0x3ffu, info.api_version & 0xfffu,
                info.queue_family);
    if (!context->wait_idle(error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::puts("Vulkan instance, device and command pool initialized successfully.");
    return 0;
}
