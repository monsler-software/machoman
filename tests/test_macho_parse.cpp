/* The shared Mach-O parser must read a real arm64 macOS binary (the ripgrep
 * fixture) end to end, narrow a synthetic fat wrapper around it to the
 * arm64 slice, and reject truncated/malformed inputs without reading out
 * of bounds. */
#include "machoman/macho.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

void put_be32(std::vector<std::uint8_t> &v, std::uint32_t x) {
    x = std::byteswap(x);
    const auto *p = reinterpret_cast<const std::uint8_t *>(&x);
    v.insert(v.end(), p, p + 4);
}

/* fat_header + one x86_64 dummy arch + the real arm64 slice, page aligned. */
std::vector<std::uint8_t> wrap_in_fat(const std::vector<std::uint8_t> &thin) {
    const std::uint32_t slice_off = 4096;
    std::vector<std::uint8_t> fat;
    put_be32(fat, MM_FAT_MAGIC);
    put_be32(fat, 2);
    /* bogus x86_64 slice first, so selection has to skip it */
    put_be32(fat, MM_CPU_TYPE_X86_64); put_be32(fat, 3); put_be32(fat, 64); put_be32(fat, 16); put_be32(fat, 4);
    put_be32(fat, MM_CPU_TYPE_ARM64);  put_be32(fat, 0); put_be32(fat, slice_off);
    put_be32(fat, static_cast<std::uint32_t>(thin.size())); put_be32(fat, 12);
    fat.resize(slice_off, 0);
    fat.insert(fat.end(), thin.begin(), thin.end());
    return fat;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <macho-file>\n", argv[0]);
        return 1;
    }

    std::ifstream f(argv[1], std::ios::binary);
    std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (data.empty()) {
        std::fprintf(stderr, "FAIL: could not read %s\n", argv[1]);
        return 1;
    }

    mm_macho_image img;
    check(mm_macho_parse(data, img) == mm_macho_status::ok, "thin parse");
    check(img.header && img.header->cputype == MM_CPU_TYPE_ARM64, "cputype arm64");
    check(!img.segments.empty(), "at least one segment");
    check(!img.dylibs.empty(), "at least one dylib");
    check(img.has_entry, "LC_MAIN present");

    const mm_section_64 *text = img.find_section("__TEXT", "__text");
    check(text != nullptr, "__TEXT,__text found");
    if (text) {
        const auto bytes = img.section_bytes(*text);
        check(bytes.size() == text->size, "__text bytes in bounds");
        check(img.entry_offset >= text->offset && img.entry_offset < text->offset + text->size,
              "entry inside __text");
    }
    for (const auto &d : img.dylibs)
        check(!d.name.empty() && d.name.front() == '/', "dylib install names are absolute paths");

    /* Fat wrapper resolves to the same slice. */
    const auto fat = wrap_in_fat(data);
    mm_macho_image fat_img;
    check(mm_macho_parse(fat, fat_img) == mm_macho_status::ok, "fat parse");
    check(fat_img.data.size() == data.size(), "fat slice size");
    check(fat_img.segments.size() == img.segments.size(), "fat slice segments");
    check(fat_img.dylibs.size() == img.dylibs.size(), "fat slice dylibs");

    /* Fat with no arm64 slice. */
    {
        std::vector<std::uint8_t> only_x86;
        put_be32(only_x86, MM_FAT_MAGIC); put_be32(only_x86, 1);
        put_be32(only_x86, MM_CPU_TYPE_X86_64); put_be32(only_x86, 3);
        put_be32(only_x86, 64); put_be32(only_x86, 16); put_be32(only_x86, 4);
        only_x86.resize(128, 0);
        mm_macho_image tmp;
        check(mm_macho_parse(only_x86, tmp) == mm_macho_status::no_arm64_slice, "fat without arm64");
    }

    /* Truncation: cutting inside the load commands must be reported, not read past. */
    {
        const std::size_t cut = sizeof(mm_mach_header_64) + img.header->sizeofcmds / 2;
        mm_macho_image tmp;
        check(mm_macho_parse(std::span(data).first(cut), tmp) == mm_macho_status::truncated_cmds,
              "truncated load commands rejected");
    }

    /* A segment whose declared nsects overflows its cmdsize. */
    {
        std::vector<std::uint8_t> bad = data;
        auto *seg = reinterpret_cast<mm_segment_command_64 *>(
            bad.data() + (reinterpret_cast<const std::uint8_t *>(img.segments[0]) - data.data()));
        seg->nsects = 1000;
        mm_macho_image tmp;
        check(mm_macho_parse(bad, tmp) == mm_macho_status::bad_load_command, "oversized nsects rejected");
    }

    /* LC_RPATH strings are bounded by the command, never its header. */
    {
        mm_mach_header_64 header{MM_MH_MAGIC_64,MM_CPU_TYPE_ARM64,0,MM_MH_DYLIB,1,32,0,0};
        std::vector<std::uint8_t> bytes(sizeof(header)+32);
        std::memcpy(bytes.data(),&header,sizeof header);
        std::uint32_t command[]={MM_LC_RPATH,32,12};
        std::memcpy(bytes.data()+sizeof header,command,sizeof command);
        std::memcpy(bytes.data()+sizeof header+12,"@loader_path",13);
        mm_macho_image tmp;
        check(mm_macho_parse(bytes,tmp)==mm_macho_status::ok&&tmp.rpaths.size()==1&&tmp.rpaths[0]=="@loader_path","LC_RPATH exact token parsed");
        command[2]=8;std::memcpy(bytes.data()+sizeof header,command,sizeof command);
        check(mm_macho_parse(bytes,tmp)==mm_macho_status::bad_load_command,"LC_RPATH offset into header rejected");
        command[2]=32;std::memcpy(bytes.data()+sizeof header,command,sizeof command);
        check(mm_macho_parse(bytes,tmp)==mm_macho_status::bad_load_command,"LC_RPATH offset outside command rejected");
        command[2]=12;std::memcpy(bytes.data()+sizeof header,command,sizeof command);
        std::memset(bytes.data()+sizeof header+12,'x',20);
        check(mm_macho_parse(bytes,tmp)==mm_macho_status::bad_load_command,"LC_RPATH missing terminator rejected");
    }

    if (failures)
        return 1;
    std::printf("PASS: %s is arm64 Mach-O, %zu segments, %zu dylibs\n", argv[1], img.segments.size(),
                img.dylibs.size());
    return 0;
}
