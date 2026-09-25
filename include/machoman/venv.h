/* Layer 1: macOS-like "venv" — a directory tree on the host filesystem that
 * stands in for a macOS root, similar in spirit to a WINEPREFIX. It holds:
 *   - a fake /usr/lib + /System/Library/Frameworks with stub or real dylibs
 *   - a per-venv manifest describing which frameworks/dylibs are provided
 *   - staged copies (or shims) of translated executables under bin/
 * The translator (layer 2) and runtime (layer 3) both read this tree; venv
 * only knows how to create/validate it, not how to run anything. */
#ifndef MACHOMAN_VENV_H
#define MACHOMAN_VENV_H

#include <filesystem>
#include <string_view>

enum class mm_venv_status {
    ok = 0,
    exists,
    mkdir_failed,
    manifest_write_failed,
    not_found,
    bad_dylib_name,
};

/* Fixed relative layout created under a venv root. */
inline constexpr std::string_view MM_VENV_DIR_BIN        = "bin";                       /* translated, runnable executables */
inline constexpr std::string_view MM_VENV_DIR_ORIGINAL   = "original";                  /* untouched arm64 Mach-O inputs */
inline constexpr std::string_view MM_VENV_DIR_USR_LIB    = "usr/lib";                   /* flat-namespace dylib stubs */
inline constexpr std::string_view MM_VENV_DIR_FRAMEWORKS = "System/Library/Frameworks"; /* framework bundle stubs */
inline constexpr std::string_view MM_VENV_MANIFEST_FILE  = "machoman-venv.json";

struct mm_venv {
    std::filesystem::path root;

    std::filesystem::path path(std::string_view relative) const { return root / relative; }
};

/* Creates a new venv rooted at `root_path`, laying out the directories
 * above and writing an initial manifest. Fails with `exists` if the
 * manifest file is already present. */
mm_venv_status mm_venv_create(const std::filesystem::path &root_path, mm_venv &out);

/* Opens an existing venv, validating the manifest is present. Does not
 * validate every framework stub exists; that is left to whichever layer
 * actually needs a given dependency. */
mm_venv_status mm_venv_open(const std::filesystem::path &root_path, mm_venv &out);

/* Registers that `dylib_name` should resolve within this venv, creating an
 * empty placeholder stub under usr/lib if none exists yet. Accepts the
 * install name as found in LC_LOAD_DYLIB (e.g. "/usr/lib/libSystem.B.dylib")
 * and stores the stub by its basename. */
mm_venv_status mm_venv_register_dylib_stub(const mm_venv &venv, std::string_view dylib_name);

const char *mm_venv_status_str(mm_venv_status status);

#endif /* MACHOMAN_VENV_H */
