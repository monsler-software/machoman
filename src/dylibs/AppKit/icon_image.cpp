#include "app_icon.h"
#include <cstring>
#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#ifdef MM_APPKIT_PNG
#include <png.h>
#endif

namespace ak {
namespace {
application_icon decode_png(std::span<const std::uint8_t> bytes) {
    application_icon out;
#ifdef MM_APPKIT_PNG
    png_image image{}; image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) return out;
    if (!image.width || image.width != image.height || image.width > 1024) { png_image_free(&image); return out; }
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> rgba(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)) { png_image_free(&image); return out; }
    out.width = image.width; out.height = image.height;
    png_image_free(&image);
    out.argb.resize(std::size_t{out.width} * out.height);
    for (std::size_t i = 0; i < out.argb.size(); ++i) {
        const auto *p = rgba.data() + i * 4;
        const unsigned a = p[3], r = (p[0] * a + 127) / 255, g = (p[1] * a + 127) / 255, b = (p[2] * a + 127) / 255;
        out.argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
#else
    (void)bytes;
#endif
    return out;
}
std::uint32_t be32(const std::uint8_t *p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
} // namespace

application_icon read_application_icon(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto size = f.tellg();
    if (size < 8 || size > 32 * 1024 * 1024) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.seekg(0); f.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!f) return {};
    constexpr std::array<std::uint8_t, 8> png_magic{137,80,78,71,13,10,26,10};
    if (std::equal(png_magic.begin(), png_magic.end(), bytes.begin())) return decode_png(bytes);
    if (std::memcmp(bytes.data(), "icns", 4) || be32(bytes.data() + 4) != bytes.size()) return {};
    application_icon best;
    for (std::size_t offset = 8; offset < bytes.size();) {
        if (bytes.size() - offset < 8) return {};
        const auto length = be32(bytes.data() + offset + 4);
        if (length < 8 || length > bytes.size() - offset) return {};
        const auto payload = std::span(bytes).subspan(offset + 8, length - 8);
        if (payload.size() >= 8 && std::equal(png_magic.begin(), png_magic.end(), payload.begin())) {
            auto icon = decode_png(payload);
            if (icon.width > best.width) best = std::move(icon);
        }
        offset += length;
    }
    return best;
}

bool install_application_icon(const application_identity &identity, const std::filesystem::path &data_home) {
#ifdef MM_APPKIT_PNG
    if (!data_home.is_absolute() || !identity.icon.width || identity.icon.width != identity.icon.height ||
        identity.icon.width > 1024 || identity.icon.argb.size() != std::size_t{identity.icon.width} * identity.icon.height ||
        !identity.app_id.starts_with("machoman.") ||
        !std::ranges::all_of(identity.app_id, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        })) return false;
    const auto icon_dir = data_home / "icons/hicolor/256x256/apps";
    const auto desktop_dir = data_home / "applications";
    std::error_code error;
    std::filesystem::create_directories(icon_dir, error);
    if (error) return false;
    std::filesystem::create_directories(desktop_dir, error);
    if (error) return false;
    std::vector<std::uint8_t> rgba(256 * 256 * 4);
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x) {
        const auto pixel = identity.icon.argb[(std::size_t{y} * identity.icon.height / 256) * identity.icon.width + x * identity.icon.width / 256];
        const unsigned a = pixel >> 24;
        const auto i = (y * 256 + x) * 4;
        for (unsigned c = 0; c < 3; ++c)
            rgba[i + c] = a ? static_cast<std::uint8_t>(std::min(255u, (((pixel >> (16 - c * 8)) & 255u) * 255u + a / 2) / a)) : 0;
        rgba[i + 3] = static_cast<std::uint8_t>(a);
    }
    png_image image{}; image.version = PNG_IMAGE_VERSION; image.width = image.height = 256; image.format = PNG_FORMAT_RGBA;
    const auto icon_path = icon_dir / (identity.app_id + ".png");
    if (!png_image_write_to_file(&image, icon_path.c_str(), 0, rgba.data(), 0, nullptr)) return false;
    const auto escape = [](std::string_view value) {
        std::string out;
        for (char c : value) {
            if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    };
    /* Hidden association entry: lets desktops resolve app_id -> bundle icon. */
    std::ofstream desktop(desktop_dir / (identity.app_id + ".desktop"));
    desktop << "[Desktop Entry]\nType=Application\nName=" << escape(identity.name)
            << "\nIcon=" << escape(icon_path.string()) << "\nExec=machoman\nNoDisplay=true\nStartupWMClass=" << identity.app_id << '\n';
    desktop.close();
    return !desktop.fail();
#else
    (void)identity; (void)data_home;
    return false;
#endif
}
} // namespace ak
