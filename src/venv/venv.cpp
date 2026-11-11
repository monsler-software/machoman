#include "machoman/venv.h"

#include <fstream>
#include <algorithm>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {
fs::path path_utf8(std::string_view s) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

bool mkdirs(const fs::path &dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec && fs::is_directory(dir, ec);
}

mm_venv_status write_initial_manifest(const mm_venv &venv) {
    std::ofstream f(venv.path(MM_VENV_MANIFEST_FILE), std::ios::binary);
    if (!f)
        return mm_venv_status::manifest_write_failed;
    f << "{\n"
         "  \"machoman_venv_version\": 1,\n"
         "  \"target_arch\": \"x86_64\",\n"
         "  \"source_arch\": \"arm64\",\n"
         "  \"dylibs\": []\n"
         "}\n";
    return f ? mm_venv_status::ok : mm_venv_status::manifest_write_failed;
}

} // namespace

mm_venv_status mm_venv_create(const fs::path &root_path, mm_venv &out) {
    out.root = root_path;

    std::error_code ec;
    if (fs::exists(out.path(MM_VENV_MANIFEST_FILE), ec))
        return mm_venv_status::exists;

    for (auto dir : {MM_VENV_DIR_BIN, MM_VENV_DIR_ORIGINAL, MM_VENV_DIR_USR_LIB, MM_VENV_DIR_FRAMEWORKS})
        if (!mkdirs(out.path(dir)))
            return mm_venv_status::mkdir_failed;

    return write_initial_manifest(out);
}

mm_venv_status mm_venv_open(const fs::path &root_path, mm_venv &out) {
    out.root = root_path;
    std::error_code ec;
    return fs::is_regular_file(out.path(MM_VENV_MANIFEST_FILE), ec) ? mm_venv_status::ok
                                                                      : mm_venv_status::not_found;
}

mm_venv_status mm_venv_register_dylib_stub(const mm_venv &venv, std::string_view dylib_name) {
    /* Install names are absolute Darwin paths; the venv keys stubs by leaf
     * name only, which also rules out any path traversal in the input. */
    const auto slash = dylib_name.find_last_of("/\\");
    const std::string_view base = slash == std::string_view::npos ? dylib_name : dylib_name.substr(slash + 1);
    if (base.empty() || base == "." || base == "..")
        return mm_venv_status::bad_dylib_name;

    const fs::path stub_path = venv.path(MM_VENV_DIR_USR_LIB) / (std::string(base) + ".stub");
    std::ofstream f(stub_path, std::ios::binary);
    if (!f)
        return mm_venv_status::manifest_write_failed;
    f << "# machoman placeholder for " << dylib_name << " — not yet backed by translated code\n";
    return f ? mm_venv_status::ok : mm_venv_status::manifest_write_failed;
}

fs::path mm_venv_resolve_executable(const mm_venv &venv, std::string_view binary, std::string &error) {
    error.clear();
    std::error_code ec;
    const fs::path requested = venv.path(MM_VENV_DIR_BIN) / path_utf8(binary);
    fs::path executable = fs::weakly_canonical(requested, ec);
    if (ec) executable = requested;
    std::ifstream file(executable, std::ios::binary);
    /* Read only the marker from ordinary, potentially very large Mach-O files. */
    std::string marker(MM_VENV_LAUNCHER_MAGIC.size() + 1, '\0');
    if (!file.read(marker.data(), static_cast<std::streamsize>(marker.size())) || marker != std::string(MM_VENV_LAUNCHER_MAGIC) + "\n")
        return executable;
    std::string relative;
    char c;
    while (file.get(c) && c != '\n' && relative.size() <= 4096) relative.push_back(c);
    const fs::path target = path_utf8(relative);
    if (relative.empty() || relative.size() > 4096 || relative.find_first_of("\r\0", 0, 2) != std::string::npos ||
        target.is_absolute() || target.has_root_name() || file.peek() != std::char_traits<char>::eof()) {
        error = "invalid app launch record: " + executable.string();
        return {};
    }
    const fs::path root = fs::canonical(venv.root, ec);
    if (!ec) executable = fs::canonical(executable.parent_path() / target, ec);
    if (ec || !fs::is_regular_file(executable, ec)) {
        error = "app launch record refers to a missing executable";
        return {};
    }
    const auto mismatch = std::mismatch(root.begin(), root.end(), executable.begin(), executable.end());
    if (mismatch.first != root.end()) {
        error = "app launch record points outside the venv";
        return {};
    }
    return executable;
}

const char *mm_venv_status_str(mm_venv_status status) {
    switch (status) {
    case mm_venv_status::ok:                    return "ok";
    case mm_venv_status::exists:                return "venv already exists at this path";
    case mm_venv_status::mkdir_failed:          return "failed to create venv directory";
    case mm_venv_status::manifest_write_failed: return "failed to write venv manifest";
    case mm_venv_status::not_found:             return "no venv manifest found at this path";
    case mm_venv_status::bad_dylib_name:        return "dylib install name has no usable basename";
    }
    return "unknown status";
}
