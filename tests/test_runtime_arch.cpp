#include "machoman/runtime.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
void put_be32(std::vector<std::uint8_t> &data, std::uint32_t value) {
    value = std::byteswap(value);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(value));
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    mm_macho_image image;
    if (mm_macho_parse(bytes, image) != mm_macho_status::ok || !image.has_entry)
        return 1;

    mm_venv venv{std::filesystem::absolute(argv[2])};
    std::filesystem::create_directories(venv.path("bin"));
    auto reject = [&](const char *name, const std::vector<std::uint8_t> &data) {
        std::ofstream out(venv.path(std::string("bin/") + name), std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        check(bool(out), "fixture written");
        const mm_launch_request request{.venv = &venv, .executable_path = name, .argv = {}, .env = {}};
        check(mm_runtime_launch(request) == mm_runtime_status::unsupported_arch, name);
    };

    const std::uint32_t intel = MM_CPU_TYPE_X86_64;
    std::memcpy(bytes.data() + offsetof(mm_mach_header_64, cputype), &intel, sizeof(intel));
    check(mm_macho_parse(bytes, image) == mm_macho_status::ok && image.header->cputype == intel,
          "inspection still accepts thin Intel Mach-O");
    

    std::vector<std::uint8_t> fat;
    put_be32(fat, MM_FAT_MAGIC); put_be32(fat, 1);
    put_be32(fat, intel); put_be32(fat, 3);
    put_be32(fat, 4096); put_be32(fat, static_cast<std::uint32_t>(bytes.size())); put_be32(fat, 12);
    fat.resize(4096);
    fat.insert(fat.end(), bytes.begin(), bytes.end());
    reject("fat-x86", fat);


    const std::uint32_t unknown = 0x010000ff;
    std::memcpy(bytes.data() + offsetof(mm_mach_header_64, cputype), &unknown, sizeof(unknown));
    reject("thin-unknown", bytes);
    return failures ? 1 : 0;
}
