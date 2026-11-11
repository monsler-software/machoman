#include "machoman/install.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
extern char **environ;
#endif

namespace fs = std::filesystem;
namespace {
fs::path path_utf8(std::string_view s) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
using bytes = std::vector<unsigned char>;
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
std::string utf8(const fs::path &p) { const auto s = p.generic_u8string(); return {reinterpret_cast<const char *>(s.data()), s.size()}; }
bool inside(const fs::path &root, const fs::path &path) {
    return std::mismatch(root.begin(), root.end(), path.begin(), path.end()).first == root.end();
}
bool occupied(const fs::path &p) {
    std::error_code ec;
    const auto status = fs::symlink_status(p, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) throw fs::filesystem_error("checking destination", p, ec);
    return status.type() != fs::file_type::not_found && status.type() != fs::file_type::none;
}
bool usable_tool(const fs::path &p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
#ifndef _WIN32
    return access(p.c_str(), X_OK) == 0;
#else
    return true;
#endif
}
fs::path seven_zip(const mm_install_options &options) {
    fs::path explicit_path = options.seven_zip;
    if (explicit_path.empty()) if (const char *env = std::getenv("MACHOMAN_7ZIP")) explicit_path = path_utf8(env);
    if (!explicit_path.empty()) {
        if (!usable_tool(explicit_path)) fail("7-Zip executable is unavailable: " + explicit_path.string());
        return fs::absolute(explicit_path);
    }
#ifdef _WIN32
    const std::array names{"7z.exe", "7zz.exe"}; constexpr char separator = ';';
#else
    const std::array names{"7zz", "7zzs", "7z"}; constexpr char separator = ':';
#endif
    if (const char *env = std::getenv("PATH")) {
        std::string_view path(env);
        for (std::size_t start = 0; start <= path.size();) {
            auto end = path.find(separator, start); if (end == std::string_view::npos) end = path.size();
            if (end != start) for (const char *name : names) {
                const auto candidate = path_utf8(path.substr(start, end - start)) / name;
                if (usable_tool(candidate)) return fs::absolute(candidate);
            }
            if (end == path.size()) break;
            start = end + 1;
        }
    }
    if (!options.executable_directory.empty()) for (const char *name : names) {
        const auto candidate = options.executable_directory / "tools" / name;
        if (usable_tool(candidate)) return fs::absolute(candidate);
    }
    fail("DMG extraction requires 7-Zip (7zz/7z); install 7-Zip or set MACHOMAN_7ZIP to its executable");
}
int extract(const fs::path &tool, const fs::path &dmg, const fs::path &directory) {
    /* argv, never a shell command. A supplied empty/noninteractive password
     * avoids a password prompt on an encrypted image. */
    std::vector<std::string> args{utf8(tool), "x", "-y", "-bd", "-bb0", "-bso0", "-bsp0", "-p-", "-o" + utf8(directory), "--", utf8(dmg)};
#ifdef _WIN32
    std::vector<std::wstring> wide;
    for (const auto &arg : args) wide.push_back(path_utf8(arg).wstring());
    std::vector<const wchar_t *> pointers;
    for (const auto &arg : wide) pointers.push_back(arg.c_str());
    pointers.push_back(nullptr);
    return static_cast<int>(_wspawnv(_P_WAIT, wide[0].c_str(), pointers.data()));
#else
    std::vector<char *> pointers;
    for (auto &arg : args) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) fail("could not prepare DMG extractor: " + std::string(std::strerror(error)));
    error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    pid_t child = 0;
    if (!error) error = posix_spawn(&child, tool.c_str(), &actions, nullptr, pointers.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) fail("could not start 7-Zip: " + std::string(std::strerror(error)));
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) fail("could not wait for 7-Zip");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
class install_lock {
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
public:
    explicit install_lock(const fs::path &root) {
        const auto path = root / ".machoman-install.lock";
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) fail("cannot acquire venv install lock (another installation may be running)");
#else
        fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (fd_ < 0) fail("cannot open venv install lock: " + std::string(std::strerror(errno)));
        struct stat info{};
        if (fstat(fd_, &info) || !S_ISREG(info.st_mode) || flock(fd_, LOCK_EX | LOCK_NB)) {
            close(fd_); fd_ = -1;
            fail("cannot acquire venv install lock (another installation may be running)");
        }
#endif
    }
    ~install_lock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) close(fd_);
#endif
    }
};
void directory_access(const fs::path &root) {
    std::error_code ec;
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ec);
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->symlink_status(ec).type() == fs::file_type::directory)
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
    }
}
struct transaction {
    fs::path staging;
    std::vector<fs::path> published;
    std::size_t committed = 0;
    bool complete = false;
    explicit transaction(const fs::path &root) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i) {
            staging = root / (".machoman-install-" + std::to_string(stamp) + "-" + std::to_string(i));
            if (fs::create_directory(staging)) return;
        }
        fail("cannot create installation staging directory");
    }
    ~transaction() {
        std::error_code ec;
        if (!complete) while (committed) fs::remove_all(published[--committed], ec);
        try { directory_access(staging); } catch (...) {}
        fs::remove_all(staging, ec);
    }
};
void publish(const fs::path &source, const fs::path &target) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), target.c_str(), 0)) fail("cannot publish application file: " + target.string());
#elif defined(__linux__)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), 1u) == 0) return;
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
        fail("cannot publish " + target.string() + ": " + std::strerror(errno));
    /* Some filesystems do not support RENAME_NOREPLACE. Reserve a fresh
     * directory before replacing it, or link/create a fresh launch file.
     * The venv lock serializes installers on either path. */
    if (fs::is_directory(source)) {
        if (!fs::create_directory(target)) fail("destination already exists: " + target.string());
        try { fs::rename(source, target); }
        catch (...) { std::error_code ec; fs::remove(target, ec); throw; }
    } else {
        std::error_code ec;
        fs::create_hard_link(source, target, ec);
        if (!ec) return;
        std::ifstream file(source, std::ios::binary);
        const std::string data{std::istreambuf_iterator<char>(file), {}};
        if (!file.eof() && file.fail()) fail("cannot read staged launch record");
        const int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) fail("cannot publish " + target.string() + ": " + std::strerror(errno));
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto n = write(fd, data.data() + offset, data.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); unlink(target.c_str()); fail("cannot write app launch record"); }
            offset += static_cast<std::size_t>(n);
        }
        if (close(fd)) { unlink(target.c_str()); fail("cannot close app launch record"); }
    }
#else
    if (occupied(target)) fail("destination already exists: " + target.string());
    fs::rename(source, target);
#endif
}
void append_utf8(std::string &s, std::uint32_t c) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || c == 0) fail("invalid character in bundle metadata");
    if (c < 0x80) s.push_back(static_cast<char>(c));
    else if (c < 0x800) { s.push_back(static_cast<char>(0xc0 | (c >> 6))); s.push_back(static_cast<char>(0x80 | (c & 63))); }
    else if (c < 0x10000) { s.push_back(static_cast<char>(0xe0 | (c >> 12))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 63))); s.push_back(static_cast<char>(0x80 | (c & 63))); }
    else { s.push_back(static_cast<char>(0xf0 | (c >> 18))); s.push_back(static_cast<char>(0x80 | ((c >> 12) & 63))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 63))); s.push_back(static_cast<char>(0x80 | (c & 63))); }
}
std::string xml_text(std::string_view input) {
    std::string result;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') { result.push_back(input[i]); continue; }
        auto end = input.find(';', i + 1); if (end == std::string_view::npos) fail("invalid XML entity");
        const auto entity = input.substr(i + 1, end - i - 1);
        if (entity == "amp") result += '&'; else if (entity == "lt") result += '<'; else if (entity == "gt") result += '>';
        else if (entity == "quot") result += '"'; else if (entity == "apos") result += '\'';
        else if (entity.starts_with('#')) {
            const bool hex = entity.size() > 1 && entity[1] == 'x';
            std::uint32_t value = 0;
            if (entity.size() <= (hex ? 2u : 1u)) fail("invalid XML numeric entity");
            for (char c : entity.substr(hex ? 2 : 1)) {
                const unsigned digit = c >= '0' && c <= '9' ? unsigned(c - '0') : c >= 'a' && c <= 'f' ? unsigned(c - 'a') + 10 : c >= 'A' && c <= 'F' ? unsigned(c - 'A') + 10 : 99;
                const unsigned base = hex ? 16 : 10;
                if (digit >= base || value > (0x10ffff - digit) / base) fail("invalid XML numeric entity");
                value = value * base + digit;
            }
            append_utf8(result, value);
        } else fail("unsupported XML entity in Info.plist");
        i = end;
    }
    return result;
}
std::string xml_executable(std::string_view xml) {
    int depth = 0;
    for (std::size_t p = 0; (p = xml.find('<', p)) != std::string_view::npos;) {
        if (xml.substr(p).starts_with("<!--")) {
            auto end = xml.find("-->", p + 4); if (end == std::string_view::npos) fail("unterminated XML comment");
            p = end + 3; continue;
        }
        auto end = xml.find('>', p); if (end == std::string_view::npos) fail("invalid Info.plist XML");
        const auto tag = xml.substr(p, end - p + 1);
        p = end + 1;
        if (tag == "<dict>") ++depth; else if (tag == "</dict>") --depth;
        else if (tag == "<key>" && depth == 1) {
            auto close = xml.find("</key>", p); if (close == std::string_view::npos) fail("invalid plist key");
            const bool match = xml_text(xml.substr(p, close - p)) == "CFBundleExecutable";
            p = close + 6;
            if (match) {
                p = xml.find_first_not_of(" \t\r\n", p);
                if (p == std::string_view::npos || !xml.substr(p).starts_with("<string>")) fail("CFBundleExecutable must be a string");
                close = xml.find("</string>", p + 8); if (close == std::string_view::npos) fail("invalid plist string");
                return xml_text(xml.substr(p + 8, close - p - 8));
            }
        }
    }
    fail("Info.plist has no CFBundleExecutable");
}
std::uint64_t big_endian(const bytes &data, std::size_t p, unsigned size, std::size_t limit) {
    if (!size || size > 8 || p > limit || size > limit - p) fail("truncated binary plist");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | data[p + i];
    return value;
}
class binary_plist {
    const bytes &data_;
    std::size_t table_, count_, root_;
    unsigned offset_size_, ref_size_;
    std::size_t object(std::uint64_t index) const {
        if (index >= count_) fail("invalid binary plist reference");
        auto p = big_endian(data_, table_ + static_cast<std::size_t>(index) * offset_size_, offset_size_, data_.size() - 32);
        if (p < 8 || p >= table_) fail("invalid binary plist object offset");
        return static_cast<std::size_t>(p);
    }
    std::size_t length(std::size_t &p) const {
        if (p >= table_) fail("truncated binary plist object");
        std::uint64_t n = data_[p++] & 15;
        if (n == 15) {
            if (p >= table_ || (data_[p] >> 4) != 1 || (data_[p] & 15) > 3) fail("invalid binary plist length");
            const unsigned size = 1u << (data_[p++] & 15);
            n = big_endian(data_, p, size, table_); p += size;
        }
        if (n > table_) fail("oversized binary plist object");
        return static_cast<std::size_t>(n);
    }
    std::string text(std::uint64_t index) const {
        auto p = object(index);
        const unsigned kind = data_[p] >> 4;
        const auto n = length(p);
        if (kind == 5 || kind == 7) {
            if (n > table_ - p) fail("truncated binary plist string");
            return {reinterpret_cast<const char *>(data_.data() + p), n};
        }
        if (kind != 6 || n > (table_ - p) / 2) fail("invalid binary plist string");
        std::string result;
        for (std::size_t i = 0; i < n; ++i) {
            auto c = static_cast<std::uint32_t>(big_endian(data_, p + i * 2, 2, table_));
            if (c >= 0xd800 && c <= 0xdbff) {
                if (++i == n) fail("invalid UTF-16 plist string");
                const auto low = static_cast<std::uint32_t>(big_endian(data_, p + i * 2, 2, table_));
                if (low < 0xdc00 || low > 0xdfff) fail("invalid UTF-16 plist string");
                c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
            }
            append_utf8(result, c);
        }
        return result;
    }
public:
    explicit binary_plist(const bytes &data) : data_(data) {
        if (data.size() < 40) fail("truncated binary plist trailer");
        const auto p = data.size() - 32;
        offset_size_ = data[p + 6]; ref_size_ = data[p + 7];
        count_ = static_cast<std::size_t>(big_endian(data, p + 8, 8, data.size()));
        root_ = static_cast<std::size_t>(big_endian(data, p + 16, 8, data.size()));
        table_ = static_cast<std::size_t>(big_endian(data, p + 24, 8, data.size()));
        if (!offset_size_ || offset_size_ > 8 || !ref_size_ || ref_size_ > 8 || table_ < 8 || table_ > p ||
            !count_ || count_ > (p - table_) / offset_size_ || root_ >= count_) fail("invalid binary plist trailer");
    }
    std::string executable() const {
        auto p = object(root_);
        if ((data_[p] >> 4) != 13) fail("Info.plist root must be a dictionary");
        const auto n = length(p);
        if (n > (table_ - p) / (2 * ref_size_)) fail("truncated binary plist dictionary");
        for (std::size_t i = 0; i < n; ++i) {
            const auto key = big_endian(data_, p + i * ref_size_, ref_size_, table_);
            if (text(key) == "CFBundleExecutable") return text(big_endian(data_, p + (n + i) * ref_size_, ref_size_, table_));
        }
        fail("Info.plist has no CFBundleExecutable");
    }
};
std::string bundle_executable(const fs::path &bundle) {
    const auto plist = bundle / "Contents/Info.plist";
    const auto size = fs::file_size(plist);
    if (!size || size > 2 * 1024 * 1024) fail("Info.plist is empty or too large: " + plist.string());
    bytes data(static_cast<std::size_t>(size));
    std::ifstream file(plist, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()))) fail("cannot read " + plist.string());
    const bool binary = data.size() >= 8 && std::memcmp(data.data(), "bplist00", 8) == 0;
    std::string executable = binary ? binary_plist(data).executable() : xml_executable({reinterpret_cast<const char *>(data.data()), data.size()});
    if (executable.empty() || executable == "." || executable == ".." || executable.size() > 255 ||
        executable.find_first_of("/\\\r\n\0", 0, 5) != std::string::npos)
        fail("invalid CFBundleExecutable in " + bundle.string());
    return executable;
}
bool arm64_executable(const fs::path &file) {
    std::ifstream input(file, std::ios::binary);
    const auto size = fs::file_size(file);
    auto header = [&](std::uint64_t offset) {
        std::array<unsigned char, 16> data{};
        if (offset > size || data.size() > size - offset) return data;
        input.clear(); input.seekg(static_cast<std::streamoff>(offset));
        input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        return data;
    };
    auto h = header(0);
    auto little = [](const auto &d, unsigned p) { return std::uint32_t(d[p]) | (std::uint32_t(d[p + 1]) << 8) | (std::uint32_t(d[p + 2]) << 16) | (std::uint32_t(d[p + 3]) << 24); };
    auto thin = [&](const auto &d) { return little(d, 0) == 0xfeedfacf && little(d, 4) == 0x100000c && little(d, 12) == 2; };
    if (thin(h)) return size >= 32;
    const auto magic = little(h, 0);
    const bool fat64 = magic == 0xbfbafeca;
    if (magic != 0xbebafeca && !fat64) return false;
    bytes data(8);
    std::copy(h.begin(), h.begin() + 8, data.begin());
    const auto count = big_endian(data, 4, 4, 8);
    const unsigned stride = fat64 ? 32 : 20;
    if (!count || count > 128 || 8 + count * stride > size) return false;
    data.resize(8 + static_cast<std::size_t>(count) * stride);
    input.clear(); input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()))) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto p = 8 + i * stride;
        if (big_endian(data, p, 4, data.size()) != 0x100000c) continue;
        const auto offset = big_endian(data, p + 8, fat64 ? 8 : 4, data.size());
        const auto slice_size = big_endian(data, p + (fat64 ? 16 : 12), fat64 ? 8 : 4, data.size());
        if (offset <= size && slice_size >= 32 && slice_size <= size - offset && thin(header(offset))) return true;
    }
    return false;
}
void require_directory(const fs::path &path) {
    if (occupied(path)) {
        if (fs::symlink_status(path).type() != fs::file_type::directory) fail("venv directory is not a real directory: " + path.string());
    } else fs::create_directory(path);
}
} // namespace

bool mm_install_dmg(const mm_venv &venv, const fs::path &image, mm_install_result &out, const mm_install_options &options) {
    out = {};
    try {
        mm_venv checked;
        if (mm_venv_open(venv.root, checked) != mm_venv_status::ok) fail("no venv manifest found at this path");
        const auto root = fs::canonical(venv.root);
        if (!fs::is_regular_file(image) || !fs::file_size(image)) fail("DMG is not a readable, nonempty file: " + image.string());
        const auto tool = seven_zip(options);
        install_lock lock(root);
        transaction tx(root);
        const auto unpacked = tx.staging / "unpacked";
        fs::create_directory(unpacked);
        const int status = extract(tool, fs::canonical(image), unpacked);
        if (status != 0) fail("7-Zip could not extract the DMG (exit " + std::to_string(status) + "); damaged, encrypted or unsupported images cannot be installed");
        directory_access(unpacked);
        std::vector<fs::path> bundles;
        for (auto it = fs::recursive_directory_iterator(unpacked, fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it) {
            if (it->symlink_status().type() == fs::file_type::directory && it->path().extension() == ".app") {
                bundles.push_back(it->path()); it.disable_recursion_pending(); // nested helper apps belong to their parent
            }
        }
        if (bundles.empty()) fail("DMG contains no .app bundles; .pkg installers are not supported");
        std::sort(bundles.begin(), bundles.end());
        const auto applications = root / "Applications", bin = root / MM_VENV_DIR_BIN;
        require_directory(applications); require_directory(bin);
        std::set<fs::path> bundle_names;
        std::set<std::string> commands;
        std::vector<mm_installed_app> apps;
        for (const auto &bundle : bundles) {
            const auto name = bundle.filename();
            const auto command = bundle_executable(bundle);
            const auto executable = fs::canonical(bundle / "Contents/MacOS" / path_utf8(command));
            if (!inside(fs::canonical(bundle), executable) || !fs::is_regular_file(executable) || !arm64_executable(executable))
                fail("bundle has no usable ARM64 Mach-O executable: " + bundle.string());
            if (!bundle_names.insert(name).second || !commands.insert(command).second || occupied(applications / name) || occupied(bin / path_utf8(command)))
                fail("application or command already exists: " + utf8(name) + " / " + command);
            if (utf8(name).find_first_of("\r\n\0", 0, 3) != std::string::npos) fail("invalid app bundle name");
            apps.push_back({applications / name, command});
        }
        const auto staged_apps = tx.staging / "apps", launchers = tx.staging / "launchers";
        fs::create_directory(staged_apps); fs::create_directory(launchers);
        tx.published.reserve(apps.size() * 2);
        for (std::size_t i = 0; i < apps.size(); ++i) {
            fs::rename(bundles[i], staged_apps / apps[i].bundle.filename());
            const auto relative = fs::path("..") / "Applications" / apps[i].bundle.filename() / "Contents/MacOS" / path_utf8(apps[i].executable);
            std::ofstream record(launchers / path_utf8(apps[i].executable), std::ios::binary);
            record << MM_VENV_LAUNCHER_MAGIC << '\n' << utf8(relative) << '\n';
            record.close(); if (!record) fail("cannot write app launch record");
            tx.published.push_back(apps[i].bundle);
            tx.published.push_back(bin / path_utf8(apps[i].executable));
        }
        for (const auto &app : apps) {
            publish(staged_apps / app.bundle.filename(), app.bundle); ++tx.committed;
            const auto command = bin / path_utf8(app.executable);
            publish(launchers / path_utf8(app.executable), command); ++tx.committed;
        }
        out.apps = std::move(apps); tx.complete = true;
        return true;
    } catch (const std::exception &e) { out.error = e.what(); return false; }
}
