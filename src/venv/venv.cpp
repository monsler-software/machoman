#include "machoman/venv.h"
#include "machoman/json.h"
#include "machoman/environment.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <string>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

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
         "  \"dylibs\": [],\n"
         "  \"env\": {}\n"
         "}\n";
    return f ? mm_venv_status::ok : mm_venv_status::manifest_write_failed;
}

mm_venv_status read_manifest(mm_venv &venv, mm_json &document) {
    venv.env.clear(); venv.manifest_error.clear();
    const auto path=venv.path(MM_VENV_MANIFEST_FILE);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return mm_venv_status::not_found;
    const auto size=fs::file_size(path, ec);
    if (ec || size > 1024*1024) {
        venv.manifest_error="manifest exceeds 1 MiB or cannot be read";
        return mm_venv_status::invalid_manifest;
    }
    std::ifstream file(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    if (!file || !mm_json_parse(text, document, venv.manifest_error)) return mm_venv_status::invalid_manifest;
    const auto *object=std::get_if<mm_json::object>(&document.value);
    if (!object) { venv.manifest_error="manifest must be a JSON object"; return mm_venv_status::invalid_manifest; }
    if (const auto version=object->find("machoman_venv_version"); version!=object->end()) {
        const auto *number=std::get_if<mm_json_number>(&version->second.value);
        if (!number || number->text!="1") { venv.manifest_error="unsupported machoman_venv_version"; return mm_venv_status::invalid_manifest; }
    }
    const auto env=object->find("env");
    if (env==object->end()) return mm_venv_status::ok; // Existing v1 manifests.
    const auto *settings=std::get_if<mm_json::object>(&env->second.value);
    if (!settings) { venv.manifest_error="env must be a JSON object"; return mm_venv_status::invalid_environment; }
    for (const auto &[name, value] : *settings) {
        const auto *string=std::get_if<std::string>(&value.value);
        if (!mm_environment::valid_name(name) ||
            (string && string->find('\0')!=std::string::npos) ||
            (!string && !std::holds_alternative<std::nullptr_t>(value.value))) {
            venv.manifest_error="env names must be nonempty without '=' or NUL; values must be strings without NUL, or null";
            venv.env.clear(); return mm_venv_status::invalid_environment;
        }
        venv.env.emplace(name, string ? std::optional<std::string>(*string) : std::nullopt);
    }
    return mm_venv_status::ok;
}

struct configuration_lock {
#if defined(_WIN32)
    HANDLE file=INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    bool held=false;
    explicit configuration_lock(const fs::path &path) {
        file=CreateFileW(path.c_str(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        held=file!=INVALID_HANDLE_VALUE && LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped);
    }
    ~configuration_lock() { if (file!=INVALID_HANDLE_VALUE) { if (held) UnlockFileEx(file, 0, 1, 0, &overlapped); CloseHandle(file); } }
#else
    int file=-1;
    bool held=false;
    explicit configuration_lock(const fs::path &path) {
        file=::open(path.c_str(), O_CREAT|O_RDWR|O_CLOEXEC, 0600);
        held=file>=0 && ::flock(file, LOCK_EX)==0;
    }
    ~configuration_lock() { if (file>=0) { if (held) ::flock(file, LOCK_UN); ::close(file); } }
#endif
};

} // namespace

mm_venv_status mm_venv_create(const fs::path &root_path, mm_venv &out) {
    out.root = root_path;
    out.env.clear(); out.manifest_error.clear();

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
    mm_json document;
    return read_manifest(out, document);
}

mm_venv_status mm_venv_update_environment(mm_venv &venv, std::string_view name,
    std::optional<std::string_view> value, bool inherit) {
    if (!mm_environment::valid_name(name) || (value && value->find('\0')!=std::string_view::npos)) {
        venv.manifest_error="invalid variable name or NUL in value";
        return mm_venv_status::invalid_environment;
    }
    std::error_code ec;
    fs::create_directories(venv.path(".machoman"), ec);
    if (ec) return mm_venv_status::manifest_write_failed;
    configuration_lock lock(venv.path(".machoman/config.lock"));
    if (!lock.held) return mm_venv_status::manifest_write_failed;
    mm_json document;
    if (const auto status=read_manifest(venv, document); status!=mm_venv_status::ok) return status;
    auto &root=std::get<mm_json::object>(document.value);
    if (!root.contains("env")) root.emplace("env", mm_json(mm_json::object{}));
    auto &settings=std::get<mm_json::object>(root.at("env").value);
    if (inherit) settings.erase(std::string(name));
    else settings[std::string(name)]=value ? mm_json(std::string(*value)) : mm_json{};
    const auto serialized=mm_json_serialize(document);
    mm_json validated;
    if (!mm_json_parse(serialized, validated, venv.manifest_error)) return mm_venv_status::invalid_environment;
    const auto target=venv.path(MM_VENV_MANIFEST_FILE);
    static std::atomic<unsigned> sequence=0;
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary=venv.path(std::string(MM_VENV_MANIFEST_FILE)+".tmp-"+std::to_string(stamp)+"-"+std::to_string(sequence++));
    {
        std::ofstream file(temporary, std::ios::binary);
        file << serialized; file.flush(); file.close();
        if (!file) { fs::remove(temporary, ec); return mm_venv_status::manifest_write_failed; }
    }
    const auto permissions=fs::status(target, ec).permissions();
    if (!ec) fs::permissions(temporary, permissions, ec);
    if (ec) { fs::remove(temporary, ec); return mm_venv_status::manifest_write_failed; }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) ec=std::make_error_code(std::errc::io_error);
#else
    fs::rename(temporary, target, ec);
#endif
    if (ec) { fs::remove(temporary, ec); return mm_venv_status::manifest_write_failed; }
    return read_manifest(venv, document);
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
    case mm_venv_status::invalid_manifest:     return "invalid venv manifest";
    case mm_venv_status::invalid_environment:  return "invalid venv environment configuration";
    }
    return "unknown status";
}
