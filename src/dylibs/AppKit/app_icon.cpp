#include "app_icon.h"
#include "appkit.h"
#include <algorithm>

namespace ak {
const application_identity &main_application_identity() {
    static const application_identity identity = [] {
        application_identity out;
        id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
        const auto info = [&](const char *key) {
            id value = obj(send(bundle, "objectForInfoDictionaryKey:", ret(string(key))));
            return value ? to_utf8(value) : std::string{};
        };
        auto identifier = info("CFBundleIdentifier");
        if (identifier.empty()) {
            const auto path = to_utf8(obj(send(bundle, "bundlePath")));
            std::uint64_t hash = 14695981039346656037ull;
            for (unsigned char c : path) { hash ^= c; hash *= 1099511628211ull; }
            identifier = std::filesystem::path(path).stem().string() + "-" + std::to_string(hash);
        }
        if (!identifier.empty()) {
            out.app_id = "machoman.";
            for (unsigned char c : identifier)
                out.app_id += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') ? static_cast<char>(c) : '_';
        }
        auto name = info("CFBundleDisplayName");
        if (name.empty()) name = info("CFBundleName");
        if (!name.empty()) out.name = name;
        auto icon_name = info("CFBundleIconFile");
        if (!icon_name.empty()) {
            const auto resources = to_utf8(obj(send(bundle, "resourcePath")));
            auto relative = std::filesystem::path(icon_name);
            if (!relative.has_extension()) relative += ".icns";
            if (!relative.is_absolute() && std::ranges::none_of(relative, [](const auto &part) { return part == ".."; }))
                out.icon = read_application_icon(std::filesystem::path(resources) / relative);
        }
        return out;
    }();
    return identity;
}
} // namespace ak
