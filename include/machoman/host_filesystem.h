#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

/* Host mount names for Darwin volume APIs. No separate guest disk is needed:
 * the native chooser and POSIX file calls use the same host paths. */
namespace mmfs {
struct mount_info { std::string path = "/", source, type = "linux"; bool local = true; };
inline std::string unescape_mount(std::string text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() && text[i+1] >= '0' && text[i+1] <= '7' &&
            text[i+2] >= '0' && text[i+2] <= '7' && text[i+3] >= '0' && text[i+3] <= '7') {
            out += char((text[i+1]-'0')*64+(text[i+2]-'0')*8+text[i+3]-'0'); i += 3;
        } else out += text[i];
    }
    return out;
}
inline mount_info mount_for(const std::filesystem::path &requested) {
    std::error_code error;
    auto canonical = std::filesystem::canonical(requested, error);
    if (error) canonical = std::filesystem::absolute(requested, error).lexically_normal();
    const auto path = canonical.string();
    mount_info best;
    std::ifstream mounts("/proc/self/mountinfo");
    for (std::string line; std::getline(mounts, line);) {
        std::istringstream fields(line);
        std::string id, parent, device, root, point, flags, field, type, source;
        if (!(fields >> id >> parent >> device >> root >> point >> flags)) continue;
        while (fields >> field && field != "-") {}
        if (!(fields >> type >> source)) continue;
        point = unescape_mount(point);
        if (point.size() < best.path.size() ||
            !(point == "/" || path == point || (path.starts_with(point) && path.size() > point.size() && path[point.size()] == '/'))) continue;
        best = {point, unescape_mount(source), type, type != "nfs" && type != "nfs4" && type != "cifs" && type != "smb3" && type != "fuse.sshfs"};
    }
    return best;
}
}
