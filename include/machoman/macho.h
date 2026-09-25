/* Mach-O 64-bit parsing: shared by the venv builder, the AOT translator,
 * and the runtime loader. Read-only view over a caller-owned file buffer;
 * this module does not own the memory it points into. Universal (fat)
 * files are transparently narrowed to their arm64 slice. */
#ifndef MACHOMAN_MACHO_H
#define MACHOMAN_MACHO_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

inline constexpr std::uint32_t MM_MH_MAGIC_64  = 0xFEEDFACFu;
inline constexpr std::uint32_t MM_MH_CIGAM_64  = 0xCFFAEDFEu; /* big-endian on disk */
inline constexpr std::uint32_t MM_FAT_MAGIC    = 0xCAFEBABEu; /* fat header is big-endian */
inline constexpr std::uint32_t MM_FAT_MAGIC_64 = 0xCAFEBABFu;

inline constexpr std::uint32_t MM_CPU_TYPE_ARM64  = 0x0100000Cu;
inline constexpr std::uint32_t MM_CPU_TYPE_X86_64 = 0x01000007u;

inline constexpr std::uint32_t MM_MH_EXECUTE = 0x2u;
inline constexpr std::uint32_t MM_MH_DYLIB   = 0x6u;
inline constexpr std::uint32_t MM_MH_BUNDLE  = 0x8u;

inline constexpr std::uint32_t MM_LC_SEGMENT_64        = 0x19u;
inline constexpr std::uint32_t MM_LC_SYMTAB            = 0x02u;
inline constexpr std::uint32_t MM_LC_DYSYMTAB          = 0x0Bu;
inline constexpr std::uint32_t MM_LC_LOAD_DYLIB        = 0x0Cu;
inline constexpr std::uint32_t MM_LC_LOAD_WEAK_DYLIB   = 0x80000018u;
inline constexpr std::uint32_t MM_LC_REEXPORT_DYLIB    = 0x8000001Fu;
inline constexpr std::uint32_t MM_LC_LOAD_UPWARD_DYLIB = 0x80000023u;
inline constexpr std::uint32_t MM_LC_MAIN              = 0x80000028u;
inline constexpr std::uint32_t MM_LC_DYLD_INFO         = 0x22u;
inline constexpr std::uint32_t MM_LC_DYLD_INFO_ONLY    = 0x80000022u;
inline constexpr std::uint32_t MM_LC_RPATH             = 0x8000001Cu;

/* section flags */
inline constexpr std::uint32_t MM_S_ATTR_PURE_INSTRUCTIONS = 0x80000000u;
inline constexpr std::uint32_t MM_S_TYPE_MASK              = 0xFFu;
inline constexpr std::uint32_t MM_S_SYMBOL_STUBS           = 0x8u;
inline constexpr std::uint32_t MM_LC_UUID              = 0x1Bu;
inline constexpr std::uint32_t MM_LC_BUILD_VERSION     = 0x32u;

struct mm_mach_header_64 {
    std::uint32_t magic;
    std::uint32_t cputype;
    std::uint32_t cpusubtype;
    std::uint32_t filetype;
    std::uint32_t ncmds;
    std::uint32_t sizeofcmds;
    std::uint32_t flags;
    std::uint32_t reserved;
};

struct mm_load_command {
    std::uint32_t cmd;
    std::uint32_t cmdsize;
};

struct mm_segment_command_64 {
    std::uint32_t cmd;
    std::uint32_t cmdsize;
    char          segname[16];
    std::uint64_t vmaddr;
    std::uint64_t vmsize;
    std::uint64_t fileoff;
    std::uint64_t filesize;
    std::uint32_t maxprot;
    std::uint32_t initprot;
    std::uint32_t nsects;
    std::uint32_t flags;
};

struct mm_section_64 {
    char          sectname[16];
    char          segname[16];
    std::uint64_t addr;
    std::uint64_t size;
    std::uint32_t offset;
    std::uint32_t align;
    std::uint32_t reloff;
    std::uint32_t nreloc;
    std::uint32_t flags;
    std::uint32_t reserved1;
    std::uint32_t reserved2;
    std::uint32_t reserved3;
};

struct mm_symtab_command {
    std::uint32_t cmd;
    std::uint32_t cmdsize;
    std::uint32_t symoff;
    std::uint32_t nsyms;
    std::uint32_t stroff;
    std::uint32_t strsize;
};

struct mm_dyld_info_command {
    std::uint32_t cmd, cmdsize;
    std::uint32_t rebase_off, rebase_size;
    std::uint32_t bind_off, bind_size;
    std::uint32_t weak_bind_off, weak_bind_size;
    std::uint32_t lazy_bind_off, lazy_bind_size;
    std::uint32_t export_off, export_size;
};

/* One pointer slot the dynamic linker would fill in. `address` is the
 * slot's guest VA; the slot must receive `symbol`'s address + addend. */
struct mm_bind {
    std::uint64_t    address;
    std::string_view symbol;
    int              dylib_ordinal; /* 1-based into mm_macho_image::dylibs; <= 0 for self/main/flat */
    std::int64_t     addend;
    bool             weak;          /* BIND_SYMBOL_FLAGS_WEAK_IMPORT: may be missing, then binds to NULL */
    bool             weak_table;    /* from the weak-bind table: coalescable definition the image itself may provide */
    bool             lazy;          /* from the lazy-bind table (a __la_symbol_ptr slot) */
};

/* One symbol from the LC_DYLD_INFO export trie. `address` is unslid
 * (segment vmaddr based); for a re-export it is 0 and `reexport_ordinal`
 * names the dylib that really defines it (under `reexport_name` if that
 * differs from `name`). */
struct mm_export {
    std::string      name; /* assembled from trie edge labels, hence owned */
    std::uint64_t    address;
    std::uint64_t    flags;
    int              reexport_ordinal; /* 0 unless EXPORT_SYMBOL_FLAGS_REEXPORT */
    std::string_view reexport_name;
};

/* One defined symbol from LC_SYMTAB (undefined/stab entries are skipped). */
struct mm_symbol {
    std::string_view name;
    std::uint64_t    address;
    bool             external;
};

struct mm_dylib_ref {
    std::string_view name; /* as written in the load command, e.g. "/usr/lib/libSystem.B.dylib" */
    std::uint32_t    cmd;  /* MM_LC_LOAD_DYLIB / _WEAK_ / _REEXPORT_ / _UPWARD_ */
    bool weak() const { return cmd == MM_LC_LOAD_WEAK_DYLIB; }
};

/* Segment/section names are 16-byte fields that are NOT NUL-terminated
 * when the name is exactly 16 chars long. */
std::string_view mm_macho_name(const char (&field)[16]);

/* A parsed handle over an in-memory Mach-O 64 image. Every pointer field
 * aliases into the caller-owned buffer; nothing here is copied. For a fat
 * file, `data`/`size` describe the arm64 slice, not the whole file. */
struct mm_macho_image {
    std::span<const std::uint8_t> data;

    const mm_mach_header_64 *header = nullptr;

    std::vector<const mm_segment_command_64 *> segments;
    std::vector<mm_dylib_ref>                   dylibs;

    const mm_symtab_command *symtab = nullptr;      /* may be null */
    const mm_dyld_info_command *dyld_info = nullptr; /* may be null (chained fixups not supported yet) */
    std::vector<std::string_view> rpaths;

    std::uint64_t entry_offset = 0; /* file offset of the entry point, from LC_MAIN */
    bool          has_entry    = false;

    /* Sections of a segment, laid out right after its command. Bounds are
     * validated by mm_macho_parse, so this cannot walk off the buffer. */
    std::span<const mm_section_64> sections(const mm_segment_command_64 &seg) const;

    /* Finds e.g. ("__TEXT", "__text"); null if absent. */
    const mm_section_64 *find_section(std::string_view segname, std::string_view sectname) const;

    /* The file bytes a section covers, bounds-checked against the image.
     * Empty span if the section lies outside the buffer (S_ZEROFILL etc.). */
    std::span<const std::uint8_t> section_bytes(const mm_section_64 &sect) const;

    /* The contiguous VA range of __TEXT covering every pure-instruction
     * section (__text, __stubs, __stub_helper, ...), so that stubs can be
     * translated together with the code that calls them. */
    struct code_range { std::uint64_t addr = 0; std::span<const std::uint8_t> bytes; };
    code_range executable_code() const;

    /* Decodes the bind, weak-bind and lazy-bind opcode streams of
     * LC_DYLD_INFO. Empty if the image has none. Slot VAs are absolute
     * (segment vmaddr + offset), valid when segments are mapped unslid. */
    std::vector<mm_bind> binds() const;

    /* Defined symbols from LC_SYMTAB, sorted by address. Empty if stripped. */
    std::vector<mm_symbol> symbols() const;

    /* Pointer slots (unslid VAs) the rebase opcode stream says hold
     * absolute addresses; each must have the load slide added. */
    std::vector<std::uint64_t> rebases() const;

    /* Every symbol in the export trie. */
    std::vector<mm_export> exports() const;
};

enum class mm_macho_status {
    ok = 0,
    too_small,
    bad_magic,
    not_64bit_le,
    truncated_cmds,
    bad_load_command,
    no_arm64_slice,
};

/* Parses `data` as a Mach-O 64-bit image (little-endian on disk) or as a
 * universal binary, in which case the arm64 slice is selected. The
 * byte-swapped thin form (MM_MH_CIGAM_64) is rejected since Apple has not
 * shipped one since PowerPC. */
mm_macho_status mm_macho_parse(std::span<const std::uint8_t> data, mm_macho_image &out);

/* Human-readable cpu type, e.g. "arm64", "x86_64", "unknown". */
const char *mm_cpu_type_name(std::uint32_t cputype);

const char *mm_macho_status_str(mm_macho_status status);

#endif /* MACHOMAN_MACHO_H */
