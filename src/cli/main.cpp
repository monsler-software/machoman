/* machoman CLI — ties the three layers together:
 *   machoman venv-init  <dir>                 (layer 1)
 *   machoman inspect    <arm64-macho-binary>  (parsing, used by layers 2 & 3)
 *   machoman translate  <arm64-macho-binary>  (layer 2)
 *   machoman run        <venv-dir> <binary>   (layer 3, straight-line leaf code only)
 */
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <string_view>
#include <vector>

#include "machoman/arch/arm64_decoder.h"
#include "machoman/macho.h"
#include "machoman/runtime.h"
#include "machoman/translator.h"
#include "machoman/venv.h"

namespace {

std::vector<std::uint8_t> read_whole_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool load_image(const char *path, std::vector<std::uint8_t> &data, mm_macho_image &img) {
    data = read_whole_file(path);
    if (data.empty()) {
        std::println(stderr, "error: could not read '{}'", path);
        return false;
    }
    if (auto st = mm_macho_parse(data, img); st != mm_macho_status::ok) {
        std::println(stderr, "error: {}", mm_macho_status_str(st));
        return false;
    }
    return true;
}

int cmd_inspect(const char *path) {
    std::vector<std::uint8_t> data;
    mm_macho_image img;
    if (!load_image(path, data, img))
        return 1;

    std::println("file:       {}", path);
    std::println("cpu type:   {}", mm_cpu_type_name(img.header->cputype));
    std::println("filetype:   {:#x}", img.header->filetype);
    std::println("ncmds:      {}", img.header->ncmds);
    std::println("segments:   {}", img.segments.size());
    for (const auto *seg : img.segments)
        std::println("  {:<16} vmaddr={:<#12x} vmsize={:<#12x} nsects={}",
                     mm_macho_name(seg->segname), seg->vmaddr, seg->vmsize, seg->nsects);
    std::println("dylibs:     {}", img.dylibs.size());
    for (const auto &d : img.dylibs)
        std::println("  {}{}", d.name, d.weak() ? " (weak)" : "");
    for (auto rp : img.rpaths)
        std::println("rpath:      {}", rp);
    if (img.has_entry)
        std::println("entry off:  {:#x}", img.entry_offset);
    const auto binds = img.binds();
    std::println("binds:      {}", binds.size());
    for (const auto &b : binds)
        std::println("  {:#x} {}{}{}{} <- {}", b.address, b.symbol, b.lazy ? " (lazy)" : "", b.weak ? " (weak)" : "", b.weak_table ? " (coalesced)" : "",
                     b.dylib_ordinal > 0 && static_cast<std::size_t>(b.dylib_ordinal) <= img.dylibs.size()
                         ? img.dylibs[static_cast<std::size_t>(b.dylib_ordinal) - 1].name : "?");
    std::println("rebases:    {}", img.rebases().size());
    const auto exports = img.exports();
    std::println("exports:    {}", exports.size());
    for (const auto &e : exports) {
        if (e.reexport_ordinal)
            std::println("  {} -> reexport from #{}{}{}", e.name, e.reexport_ordinal, e.reexport_name.empty() ? "" : " as ",
                         e.reexport_name);
        else
            std::println("  {:#x} {}{}", e.address, e.name, (e.flags & 0x4) ? " (weak)" : "");
    }
    return 0;
}

int cmd_venv_init(const char *dir) {
    mm_venv venv;
    if (auto st = mm_venv_create(dir, venv); st != mm_venv_status::ok) {
        std::println(stderr, "error: {}", mm_venv_status_str(st));
        return 1;
    }
    std::println("created venv at {}", venv.root.string());
    for (auto d : {MM_VENV_DIR_BIN, MM_VENV_DIR_ORIGINAL, MM_VENV_DIR_USR_LIB, MM_VENV_DIR_FRAMEWORKS})
        std::println("  {}/", d);
    return 0;
}

void print_unsupported_stats(std::span<const std::uint8_t> text, const mm_translation_result &result) {
    std::map<std::string_view, std::size_t> by_class;
    for (const auto &n : result.notes) {
        if (n.supported)
            continue;
        std::uint32_t raw;
        std::memcpy(&raw, text.data() + n.source_offset, 4);
        by_class[mm_a64_class_name(raw)]++;
    }
    std::vector<std::pair<std::string_view, std::size_t>> rows(by_class.begin(), by_class.end());
    std::ranges::sort(rows, std::ranges::greater{}, &std::pair<std::string_view, std::size_t>::second);

    std::println("\nunsupported by encoding class (share of all instructions):");
    const double total = static_cast<double>(result.notes.size());
    for (const auto &[name, count] : rows)
        std::println("  {:<28} {:>10}  {:5.1f}%", name, count, 100.0 * static_cast<double>(count) / total);
}

int cmd_translate(const char *path, bool stats) {
    std::vector<std::uint8_t> data;
    mm_macho_image img;
    if (!load_image(path, data, img))
        return 1;

    const mm_section_64 *text = img.find_section("__TEXT", "__text");
    if (!text) {
        std::println(stderr, "error: no __TEXT,__text section found");
        return 1;
    }
    const auto text_bytes = img.section_bytes(*text);
    if (text_bytes.empty()) {
        std::println(stderr, "error: __TEXT,__text lies outside the file");
        return 1;
    }

    mm_translation_result result;
    const std::size_t n = mm_translate_buffer(text_bytes, text->addr, result);

    std::println("translated {} instructions from __text ({} bytes)", n, text_bytes.size());
    std::println("emitted {} bytes of x86_64 code", result.code.size());
    std::println("unsupported: {} / {} instructions ({:.1f}%)", result.unsupported_count, n,
                 n ? 100.0 * static_cast<double>(result.unsupported_count) / static_cast<double>(n) : 0.0);
    if (stats)
        print_unsupported_stats(text_bytes, result);
    return 0;
}

/* Symbol lookup for annotations: the nearest defined symbol at or below
 * `va`, and for a __stubs entry the import it jumps through. */
struct disasm_symbols {
    std::vector<mm_symbol> syms;
    std::map<std::uint64_t, std::string_view> slots; /* bind slot VA -> import name */
    const mm_section_64 *stubs = nullptr;
    const mm_macho_image *img = nullptr;

    explicit disasm_symbols(const mm_macho_image &image) : syms(image.symbols()), img(&image) {
        for (const auto &b : image.binds())
            slots.emplace(b.address, b.symbol);
        stubs = image.find_section("__TEXT", "__stubs");
    }

    std::string describe(std::uint64_t va) const {
        if (stubs && va >= stubs->addr && va < stubs->addr + stubs->size) {
            /* adrp x16, page; ldr x16, [x16, #off]; br x16 */
            const auto bytes = img->section_bytes(*stubs);
            const std::size_t off = va - stubs->addr;
            if (off + 8 <= bytes.size()) {
                const mm_a64_insn adrp = mm_a64_decode(bytes.subspan(off, 4), va);
                const mm_a64_insn ldr = mm_a64_decode(bytes.subspan(off + 4, 4), va + 4);
                if (adrp.op == mm_a64_opcode::adrp && ldr.op == mm_a64_opcode::ldr) {
                    const std::uint64_t slot = static_cast<std::uint64_t>(adrp.imm) + static_cast<std::uint64_t>(ldr.imm);
                    if (auto it = slots.find(slot); it != slots.end())
                        return std::string(it->second);
                }
            }
            return std::format("stub@{:#x}", va);
        }
        auto it = std::ranges::upper_bound(syms, va, {}, &mm_symbol::address);
        if (it == syms.begin())
            return {};
        --it;
        if (va == it->address)
            return std::string(it->name);
        return std::format("{}+{:#x}", it->name, va - it->address);
    }
};

int cmd_disasm(const char *path, const char *va_str, const char *count_str) {
    std::vector<std::uint8_t> data;
    mm_macho_image img;
    if (!load_image(path, data, img))
        return 1;
    const auto code = img.executable_code();
    if (code.bytes.empty()) {
        std::println(stderr, "error: no executable sections found");
        return 1;
    }
    const std::uint64_t va = std::strtoull(va_str, nullptr, 0);
    const std::size_t count = count_str ? std::strtoul(count_str, nullptr, 0) : 16;
    if (va < code.addr || va >= code.addr + code.bytes.size()) {
        std::println(stderr, "error: {:#x} is not inside __TEXT code [{:#x}, {:#x})", va, code.addr, code.addr + code.bytes.size());
        return 1;
    }
    const disasm_symbols symbols(img);
    std::println("; {}", symbols.describe(va));
    for (std::size_t i = 0; i < count; i++) {
        const std::uint64_t pc = va + i * 4;
        const std::uint64_t off = pc - code.addr;
        if (off + 4 > code.bytes.size())
            break;
        const mm_a64_insn insn = mm_a64_decode(code.bytes.subspan(off, 4), pc);
        std::string note;
        if (insn.op == mm_a64_opcode::unknown)
            note = std::string("  [") + mm_a64_class_name(insn.raw) + "]";
        else if (insn.op == mm_a64_opcode::bl || insn.op == mm_a64_opcode::b || insn.op == mm_a64_opcode::adrp)
            note = "  ; " + symbols.describe(static_cast<std::uint64_t>(insn.imm));
        std::println("{:#x}  {:08x}  {:<8} rd={:<2} rn={:<2} rm={:<2} imm={:#x}{}", pc, insn.raw,
                     mm_a64_opcode_name(insn.op), insn.rd, insn.rn, insn.rm,
                     static_cast<std::uint64_t>(insn.imm), note);
    }
    return 0;
}

int cmd_run(const char *venv_dir, const char *binary, std::span<char *const> args, std::span<char *const> env) {
    mm_venv venv;
    if (auto st = mm_venv_open(venv_dir, venv); st != mm_venv_status::ok) {
        std::println(stderr, "error: {}", mm_venv_status_str(st));
        return 1;
    }

    const mm_launch_request req{.venv = &venv, .executable_path = binary, .argv = args, .env = env};
    const mm_runtime_status st = mm_runtime_launch(req);
    std::println(stderr, "run: {}", mm_runtime_status_str(st));
    return st == mm_runtime_status::ok ? 0 : 1;
}

void usage(const char *argv0) {
    std::println(stderr,
                 "machoman - a wine-like compatibility layer for running macOS arm64 CLI binaries\n"
                 "on Windows/Linux x86_64\n\n"
                 "usage:\n"
                 "  {0} inspect <arm64-macho-binary>\n"
                 "  {0} venv-init <venv-dir>\n"
                 "  {0} translate [--stats] <arm64-macho-binary>\n"
                 "  {0} run [-e KEY=VALUE]... <venv-dir> <binary-within-venv> [args...]\n"
                 "  {0} disasm <arm64-macho-binary> <va> [count]\n\n"
                 "environment:\n"
                 "  -e KEY=VALUE   set a variable for the guest (-e KEY removes one)\n"
                 "  DYLD_LIBRARY_PATH, DYLD_FRAMEWORK_PATH and their DYLD_FALLBACK_*\n"
                 "  counterparts, DYLD_ROOT_PATH, DYLD_INSERT_LIBRARIES and\n"
                 "  DYLD_PRINT_LIBRARIES work as they do under dyld; the first two\n"
                 "  override even an absolute install name inside the binary.",
                 argv0);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    const std::string_view cmd = argv[1];

    if (cmd == "inspect" && argc == 3)
        return cmd_inspect(argv[2]);
    if (cmd == "venv-init" && argc == 3)
        return cmd_venv_init(argv[2]);
    if (cmd == "translate" && argc == 3)
        return cmd_translate(argv[2], false);
    if (cmd == "translate" && argc == 4 && std::string_view(argv[2]) == "--stats")
        return cmd_translate(argv[3], true);
    if (cmd == "run") {
        /* `-e KEY=VALUE` assignments precede the venv; everything after the
         * binary belongs to the guest, options included. */
        std::vector<char *> env;
        int i = 2;
        for (; i + 1 < argc && std::string_view(argv[i]) == "-e"; i += 2)
            env.push_back(argv[i + 1]);
        if (argc - i >= 2)
            return cmd_run(argv[i], argv[i + 1], std::span<char *const>(argv + i + 1, static_cast<std::size_t>(argc - i - 1)),
                           std::span<char *const>(env));
    }
    if (cmd == "disasm" && (argc == 4 || argc == 5))
        return cmd_disasm(argv[2], argv[3], argc == 5 ? argv[4] : nullptr);

    usage(argv[0]);
    return 1;
}
