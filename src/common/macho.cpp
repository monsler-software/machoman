#include "machoman/macho.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace {

bool fits(std::size_t size, std::size_t offset, std::size_t len) {
    return len <= size && offset <= size - len;
}

template <typename T>
T read_le(const std::uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::uint32_t read_be32(const std::uint8_t *p) {
    return std::byteswap(read_le<std::uint32_t>(p));
}

std::uint64_t read_be64(const std::uint8_t *p) {
    return std::byteswap(read_le<std::uint64_t>(p));
}

/* fat_header { magic, nfat_arch } then nfat_arch of
 *   fat_arch    { cputype, cpusubtype, offset(u32), size(u32), align }       (20 bytes)
 *   fat_arch_64 { cputype, cpusubtype, offset(u64), size(u64), align, res }  (32 bytes)
 * all big-endian regardless of host. */
mm_macho_status select_arm64_slice(std::span<const std::uint8_t> file, bool is64,
                                   std::span<const std::uint8_t> &slice) {
    if (!fits(file.size(), 0, 8))
        return mm_macho_status::too_small;
    const std::uint32_t nfat = read_be32(file.data() + 4);
    const std::size_t entry_size = is64 ? 32 : 20;
    if (!fits(file.size(), 8, static_cast<std::size_t>(nfat) * entry_size))
        return mm_macho_status::truncated_cmds;

    for (std::uint32_t i = 0; i < nfat; i++) {
        const std::uint8_t *e = file.data() + 8 + i * entry_size;
        if (read_be32(e) != MM_CPU_TYPE_ARM64)
            continue;
        std::uint64_t off, len;
        if (is64) {
            off = read_be64(e + 8);
            len = read_be64(e + 16);
        } else {
            off = read_be32(e + 8);
            len = read_be32(e + 12);
        }
        if (!fits(file.size(), off, len))
            return mm_macho_status::truncated_cmds;
        slice = file.subspan(off, len);
        return mm_macho_status::ok;
    }
    return mm_macho_status::no_arm64_slice;
}

mm_macho_status parse_thin(std::span<const std::uint8_t> data, mm_macho_image &out) {
    out.data = data;

    const std::uint8_t *base = data.data();
    const std::size_t size = data.size();

    const auto *hdr = reinterpret_cast<const mm_mach_header_64 *>(base);
    out.header = hdr;

    if (!fits(size, sizeof(mm_mach_header_64), hdr->sizeofcmds))
        return mm_macho_status::truncated_cmds;

    const std::uint8_t *p = base + sizeof(mm_mach_header_64);
    const std::uint8_t *cmds_end = p + hdr->sizeofcmds;

    for (std::uint32_t i = 0; i < hdr->ncmds; i++) {
        if (static_cast<std::size_t>(cmds_end - p) < sizeof(mm_load_command))
            return mm_macho_status::truncated_cmds;
        const auto *lc = reinterpret_cast<const mm_load_command *>(p);
        /* Mach-O 64 requires cmdsize to be a multiple of 8; that is also what
         * keeps the struct views below aligned. */
        if (lc->cmdsize < sizeof(mm_load_command) || lc->cmdsize % 8 != 0 ||
            static_cast<std::size_t>(cmds_end - p) < lc->cmdsize)
            return mm_macho_status::truncated_cmds;

        switch (lc->cmd) {
        case MM_LC_SEGMENT_64: {
            if (lc->cmdsize < sizeof(mm_segment_command_64))
                return mm_macho_status::bad_load_command;
            const auto *seg = reinterpret_cast<const mm_segment_command_64 *>(p);
            const std::uint64_t need =
                sizeof(mm_segment_command_64) + static_cast<std::uint64_t>(seg->nsects) * sizeof(mm_section_64);
            if (need > lc->cmdsize)
                return mm_macho_status::bad_load_command;
            out.segments.push_back(seg);
            break;
        }
        case MM_LC_SYMTAB:
            if (lc->cmdsize < sizeof(mm_symtab_command))
                return mm_macho_status::bad_load_command;
            out.symtab = reinterpret_cast<const mm_symtab_command *>(p);
            break;
        case MM_LC_LOAD_DYLIB:
        case MM_LC_LOAD_WEAK_DYLIB:
        case MM_LC_REEXPORT_DYLIB:
        case MM_LC_LOAD_UPWARD_DYLIB: {
            /* dylib_command: {cmd, cmdsize}, struct dylib { name(lc_str u32 offset),
             * timestamp, current_version, compatibility_version } = 24 bytes min. */
            if (lc->cmdsize < 24)
                return mm_macho_status::bad_load_command;
            const std::uint32_t str_off = read_le<std::uint32_t>(p + 8);
            if (str_off >= lc->cmdsize)
                return mm_macho_status::bad_load_command;
            const char *s = reinterpret_cast<const char *>(p + str_off);
            const std::size_t max_len = lc->cmdsize - str_off;
            const void *nul = std::memchr(s, '\0', max_len);
            if (!nul)
                return mm_macho_status::bad_load_command;
            out.dylibs.push_back({std::string_view(s, static_cast<std::size_t>(static_cast<const char *>(nul) - s)), lc->cmd});
            break;
        }
        case MM_LC_DYLD_INFO:
        case MM_LC_DYLD_INFO_ONLY:
            if (lc->cmdsize < sizeof(mm_dyld_info_command))
                return mm_macho_status::bad_load_command;
            out.dyld_info = reinterpret_cast<const mm_dyld_info_command *>(p);
            break;
        case MM_LC_RPATH: {
            if (lc->cmdsize < 12)
                return mm_macho_status::bad_load_command;
            const std::uint32_t str_off = read_le<std::uint32_t>(p + 8);
            if (str_off < 12 || str_off >= lc->cmdsize)
                return mm_macho_status::bad_load_command;
            const char *s = reinterpret_cast<const char *>(p + str_off);
            const void *nul = std::memchr(s, '\0', lc->cmdsize - str_off);
            if (!nul)
                return mm_macho_status::bad_load_command;
            out.rpaths.emplace_back(s, static_cast<std::size_t>(static_cast<const char *>(nul) - s));
            break;
        }
        case MM_LC_MAIN: {
            /* entry_point_command: cmd, cmdsize, entryoff(u64), stacksize(u64) */
            if (lc->cmdsize < 24)
                return mm_macho_status::bad_load_command;
            out.entry_offset = read_le<std::uint64_t>(p + 8);
            out.has_entry = true;
            break;
        }
        default:
            break;
        }

        p += lc->cmdsize;
    }

    return mm_macho_status::ok;
}

} // namespace

std::string_view mm_macho_name(const char (&field)[16]) {
    const void *nul = std::memchr(field, '\0', 16);
    return {field, nul ? static_cast<std::size_t>(static_cast<const char *>(nul) - field) : 16};
}

std::span<const mm_section_64> mm_macho_image::sections(const mm_segment_command_64 &seg) const {
    const auto *first = reinterpret_cast<const mm_section_64 *>(
        reinterpret_cast<const std::uint8_t *>(&seg) + sizeof(seg));
    return {first, seg.nsects};
}

const mm_section_64 *mm_macho_image::find_section(std::string_view segname, std::string_view sectname) const {
    for (const auto *seg : segments) {
        if (mm_macho_name(seg->segname) != segname)
            continue;
        for (const auto &sect : sections(*seg))
            if (mm_macho_name(sect.sectname) == sectname)
                return &sect;
    }
    return nullptr;
}

std::span<const std::uint8_t> mm_macho_image::section_bytes(const mm_section_64 &sect) const {
    if (!fits(data.size(), sect.offset, sect.size))
        return {};
    return data.subspan(sect.offset, sect.size);
}

mm_macho_image::code_range mm_macho_image::executable_code() const {
    for (const auto *seg : segments) {
        if (mm_macho_name(seg->segname) != "__TEXT")
            continue;
        std::uint64_t lo = ~0ull, hi = 0;
        for (const auto &sect : sections(*seg)) {
            const bool code = (sect.flags & MM_S_ATTR_PURE_INSTRUCTIONS) ||
                              (sect.flags & MM_S_TYPE_MASK) == MM_S_SYMBOL_STUBS;
            if (!code || sect.size == 0)
                continue;
            lo = std::min(lo, sect.addr);
            hi = std::max(hi, sect.addr + sect.size);
        }
        if (hi <= lo)
            return {};
        const std::uint64_t file_off = seg->fileoff + (lo - seg->vmaddr);
        if (!fits(data.size(), file_off, hi - lo))
            return {};
        return {lo, data.subspan(file_off, hi - lo)};
    }
    return {};
}

namespace {

struct opcode_stream {
    std::span<const std::uint8_t> s;
    std::size_t i = 0;
    bool ok = true;

    bool done() const { return i >= s.size(); }
    std::uint8_t byte() { if (i >= s.size()) { ok = false; return 0; } return s[i++]; }
    std::uint64_t uleb() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint8_t b = byte();
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return v;
    }
    std::int64_t sleb() {
        std::int64_t v = 0;
        int shift = 0;
        std::uint8_t b;
        do {
            b = byte();
            v |= static_cast<std::int64_t>(b & 0x7F) << shift;
            shift += 7;
        } while ((b & 0x80) && shift < 64);
        if (shift < 64 && (b & 0x40))
            v |= -(std::int64_t{1} << shift);
        return v;
    }
    std::string_view cstr() {
        const std::size_t start = i;
        while (i < s.size() && s[i] != 0) i++;
        if (i >= s.size()) { ok = false; return {}; }
        return {reinterpret_cast<const char *>(s.data() + start), i++ - start};
    }
};

/* BIND_OPCODE_* from <mach-o/loader.h>. */
void decode_binds(const mm_macho_image &img, std::span<const std::uint8_t> stream, bool weak_table, bool lazy_table,
                  std::vector<mm_bind> &out) {
    opcode_stream in{stream};
    mm_bind cur{};
    cur.weak_table = weak_table;
    cur.lazy = lazy_table;
    std::uint8_t seg = 0;
    std::uint64_t seg_off = 0;

    auto slot = [&]() -> std::uint64_t {
        return seg < img.segments.size() ? img.segments[seg]->vmaddr + seg_off : 0;
    };
    auto emit = [&] {
        cur.address = slot();
        if (cur.address && !cur.symbol.empty())
            out.push_back(cur);
    };

    while (!in.done() && in.ok) {
        const std::uint8_t b = in.byte();
        const std::uint8_t op = b & 0xF0, imm = b & 0x0F;
        switch (op) {
        case 0x00: /* DONE: in the lazy table this just separates entries */
            if (!lazy_table) return;
            break;
        case 0x10: cur.dylib_ordinal = imm; break;                                   /* SET_DYLIB_ORDINAL_IMM */
        case 0x20: cur.dylib_ordinal = static_cast<int>(in.uleb()); break;           /* SET_DYLIB_ORDINAL_ULEB */
        case 0x30: cur.dylib_ordinal = imm ? static_cast<int>(imm) - 16 : 0; break;  /* SET_DYLIB_SPECIAL_IMM */
        case 0x40:                                                                    /* SET_SYMBOL_TRAILING_FLAGS_IMM */
            cur.weak = imm & 0x1;
            cur.symbol = in.cstr();
            break;
        case 0x50: break;                                                             /* SET_TYPE_IMM: always pointer here */
        case 0x60: cur.addend = in.sleb(); break;                                     /* SET_ADDEND_SLEB */
        case 0x70: seg = imm; seg_off = in.uleb(); break;                             /* SET_SEGMENT_AND_OFFSET_ULEB */
        case 0x80: seg_off += in.uleb(); break;                                       /* ADD_ADDR_ULEB */
        case 0x90: emit(); seg_off += 8; break;                                       /* DO_BIND */
        case 0xA0: emit(); seg_off += 8 + in.uleb(); break;                           /* DO_BIND_ADD_ADDR_ULEB */
        case 0xB0: emit(); seg_off += 8 + static_cast<std::uint64_t>(imm) * 8; break; /* DO_BIND_ADD_ADDR_IMM_SCALED */
        case 0xC0: {                                                                  /* DO_BIND_ULEB_TIMES_SKIPPING_ULEB */
            const std::uint64_t count = in.uleb(), skip = in.uleb();
            for (std::uint64_t k = 0; k < count && in.ok; k++) { emit(); seg_off += 8 + skip; }
            break;
        }
        default: /* THREADED and unknown opcodes: stop rather than misparse */
            return;
        }
    }
}

} // namespace

std::vector<mm_bind> mm_macho_image::binds() const {
    std::vector<mm_bind> out;
    if (!dyld_info)
        return out;
    auto table = [&](std::uint32_t off, std::uint32_t size) -> std::span<const std::uint8_t> {
        return fits(data.size(), off, size) ? data.subspan(off, size) : std::span<const std::uint8_t>{};
    };
    decode_binds(*this, table(dyld_info->bind_off, dyld_info->bind_size), false, false, out);
    decode_binds(*this, table(dyld_info->weak_bind_off, dyld_info->weak_bind_size), true, false, out);
    decode_binds(*this, table(dyld_info->lazy_bind_off, dyld_info->lazy_bind_size), false, true, out);
    return out;
}

mm_macho_status mm_macho_parse(std::span<const std::uint8_t> data, mm_macho_image &out) {
    out = {};
    out.data = data;

    if (!fits(data.size(), 0, sizeof(mm_mach_header_64)))
        return mm_macho_status::too_small;

    const std::uint32_t magic = read_le<std::uint32_t>(data.data());
    const std::uint32_t magic_be = std::byteswap(magic);

    if (magic_be == MM_FAT_MAGIC || magic_be == MM_FAT_MAGIC_64) {
        std::span<const std::uint8_t> slice;
        if (auto st = select_arm64_slice(data, magic_be == MM_FAT_MAGIC_64, slice); st != mm_macho_status::ok)
            return st;
        if (!fits(slice.size(), 0, sizeof(mm_mach_header_64)))
            return mm_macho_status::too_small;
        if (read_le<std::uint32_t>(slice.data()) != MM_MH_MAGIC_64)
            return mm_macho_status::bad_magic;
        return parse_thin(slice, out);
    }
    if (magic == MM_MH_CIGAM_64)
        return mm_macho_status::not_64bit_le;
    if (magic != MM_MH_MAGIC_64)
        return mm_macho_status::bad_magic;
    return parse_thin(data, out);
}

const char *mm_cpu_type_name(std::uint32_t cputype) {
    switch (cputype) {
    case MM_CPU_TYPE_ARM64:  return "arm64";
    case MM_CPU_TYPE_X86_64: return "x86_64";
    default:                 return "unknown";
    }
}

const char *mm_macho_status_str(mm_macho_status status) {
    switch (status) {
    case mm_macho_status::ok:               return "ok";
    case mm_macho_status::too_small:        return "file too small for Mach-O header";
    case mm_macho_status::bad_magic:        return "bad magic (not Mach-O 64 little-endian or universal)";
    case mm_macho_status::not_64bit_le:     return "big-endian-on-disk Mach-O is unsupported";
    case mm_macho_status::truncated_cmds:   return "load commands truncated or malformed";
    case mm_macho_status::bad_load_command: return "a load command is smaller than its declared layout";
    case mm_macho_status::no_arm64_slice:   return "universal binary has no arm64 slice";
    }
    return "unknown status";
}

namespace {
struct nlist_64 {
    std::uint32_t n_strx;
    std::uint8_t  n_type;
    std::uint8_t  n_sect;
    std::uint16_t n_desc;
    std::uint64_t n_value;
};
} // namespace

std::vector<mm_symbol> mm_macho_image::symbols() const {
    std::vector<mm_symbol> out;
    if (!symtab)
        return out;
    const std::uint64_t symend = std::uint64_t{symtab->symoff} + std::uint64_t{symtab->nsyms} * sizeof(nlist_64);
    const std::uint64_t strend = std::uint64_t{symtab->stroff} + symtab->strsize;
    if (symend > data.size() || strend > data.size())
        return out;
    const auto *syms = reinterpret_cast<const nlist_64 *>(data.data() + symtab->symoff);
    const char *strs = reinterpret_cast<const char *>(data.data() + symtab->stroff);
    for (std::uint32_t i = 0; i < symtab->nsyms; i++) {
        const nlist_64 &n = syms[i];
        if ((n.n_type & 0xe0) != 0 || (n.n_type & 0x0e) != 0x0e) /* stab or not N_SECT */
            continue;
        if (n.n_strx >= symtab->strsize)
            continue;
        const std::size_t maxlen = symtab->strsize - n.n_strx;
        const std::size_t len = ::strnlen(strs + n.n_strx, maxlen);
        out.push_back({std::string_view(strs + n.n_strx, len), n.n_value, (n.n_type & 0x01) != 0});
    }
    std::ranges::sort(out, {}, &mm_symbol::address);
    return out;
}

/* REBASE_OPCODE_* from <mach-o/loader.h>. Only pointer rebases exist on
 * arm64 (TEXT_ABSOLUTE32 would be a 32-bit slot). */
std::vector<std::uint64_t> mm_macho_image::rebases() const {
    std::vector<std::uint64_t> out;
    if (!dyld_info || !fits(data.size(), dyld_info->rebase_off, dyld_info->rebase_size))
        return out;
    opcode_stream in{data.subspan(dyld_info->rebase_off, dyld_info->rebase_size)};
    std::uint8_t seg = 0;
    std::uint64_t seg_off = 0;
    auto emit = [&] {
        if (seg < segments.size())
            out.push_back(segments[seg]->vmaddr + seg_off);
    };
    while (!in.done() && in.ok) {
        const std::uint8_t b = in.byte();
        const std::uint8_t op = b & 0xF0, imm = b & 0x0F;
        switch (op) {
        case 0x00: return out;                                                              /* DONE */
        case 0x10: break;                                                               /* SET_TYPE_IMM */
        case 0x20: seg = imm; seg_off = in.uleb(); break;                               /* SET_SEGMENT_AND_OFFSET_ULEB */
        case 0x30: seg_off += in.uleb(); break;                                         /* ADD_ADDR_ULEB */
        case 0x40: seg_off += static_cast<std::uint64_t>(imm) * 8; break;               /* ADD_ADDR_IMM_SCALED */
        case 0x50: for (int k = 0; k < imm; k++) { emit(); seg_off += 8; } break;       /* DO_REBASE_IMM_TIMES */
        case 0x60: {                                                                    /* DO_REBASE_ULEB_TIMES */
            const std::uint64_t count = in.uleb();
            for (std::uint64_t k = 0; k < count && in.ok; k++) { emit(); seg_off += 8; }
            break;
        }
        case 0x70: emit(); seg_off += 8 + in.uleb(); break;                             /* DO_REBASE_ADD_ADDR_ULEB */
        case 0x80: {                                                                    /* DO_REBASE_ULEB_TIMES_SKIPPING_ULEB */
            const std::uint64_t count = in.uleb(), skip = in.uleb();
            for (std::uint64_t k = 0; k < count && in.ok; k++) { emit(); seg_off += 8 + skip; }
            break;
        }
        default: return out;
        }
    }
    return out;
}

namespace {

/* Export trie walk (see dyld's ExportInfoTrie): each node is a uleb
 * terminal-size, the terminal payload, a child count and edges of
 * (label cstring, uleb child offset). */
void walk_exports(const mm_macho_image &img, std::span<const std::uint8_t> trie, std::size_t node, std::string &prefix,
                  int depth, std::vector<mm_export> &out) {
    if (node >= trie.size() || depth > 256)
        return;
    opcode_stream in{trie.subspan(node)};
    const std::uint64_t terminal_size = in.uleb();
    if (!in.ok) return;
    const std::size_t after_terminal = node + in.i + terminal_size;
    if (terminal_size) {
        mm_export e{};
        e.flags = in.uleb();
        /* Offsets are from the mach header, i.e. the __TEXT segment. */
        std::uint64_t base = 0;
        for (const auto *seg : img.segments)
            if (mm_macho_name(seg->segname) == "__TEXT") { base = seg->vmaddr; break; }
        if (e.flags & 0x08) { /* EXPORT_SYMBOL_FLAGS_REEXPORT */
            e.reexport_ordinal = static_cast<int>(in.uleb());
            e.reexport_name = in.cstr();
        } else {
            e.address = base + in.uleb();
            if (e.flags & 0x10) /* STUB_AND_RESOLVER: skip the resolver offset */
                in.uleb();
        }
        if (!in.ok) return;
        e.name = prefix;
        out.push_back(std::move(e));
    }
    if (after_terminal >= trie.size()) return;
    opcode_stream edges{trie.subspan(after_terminal)};
    const std::uint8_t nchildren = edges.byte();
    for (std::uint8_t c = 0; c < nchildren && edges.ok; c++) {
        const std::string_view label = edges.cstr();
        const std::uint64_t child = edges.uleb();
        if (!edges.ok) break;
        const std::size_t len = prefix.size();
        prefix.append(label);
        walk_exports(img, trie, static_cast<std::size_t>(child), prefix, depth + 1, out);
        prefix.resize(len);
    }
}

} // namespace

std::vector<mm_export> mm_macho_image::exports() const {
    std::vector<mm_export> out;
    if (!dyld_info || !fits(data.size(), dyld_info->export_off, dyld_info->export_size) || dyld_info->export_size == 0)
        return out;
    std::string prefix;
    walk_exports(*this, data.subspan(dyld_info->export_off, dyld_info->export_size), 0, prefix, 0, out);
    return out;
}
