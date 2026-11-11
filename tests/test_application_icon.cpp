#include "../src/dylibs/AppKit/app_icon.h"
#include <png.h>
#include <array>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <iterator>
static void require(bool ok) { if (!ok) std::abort(); }
static void be32(std::ofstream &f, std::uint32_t value) {
    char bytes[4]; for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<char>(value >> ((3 - i) * 8));
    f.write(bytes, 4);
}
int main(int argc, char **argv) {
    require(argc == 2);
    const std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    const auto png_path = root / "fixture.png", icns_path = root / "fixture.icns";
    const std::array<unsigned char, 16> pixels{200,100,50,128, 1,2,3,255, 0,0,0,0, 255,255,255,255};
    png_image image{}; image.version = PNG_IMAGE_VERSION; image.width = image.height = 2; image.format = PNG_FORMAT_RGBA;
    require(png_image_write_to_file(&image, png_path.c_str(), 0, pixels.data(), 0, nullptr));
    std::ifstream source(png_path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(source), {}};
    {
        std::ofstream f(icns_path, std::ios::binary);
        f.write("icns", 4); be32(f, static_cast<std::uint32_t>(16 + bytes.size()));
        f.write("icp4", 4); be32(f, static_cast<std::uint32_t>(8 + bytes.size())); f.write(bytes.data(), bytes.size());
    }
    auto png = ak::read_application_icon(png_path), icns = ak::read_application_icon(icns_path);
    require(png.width == 2 && png.height == 2 && png.argb == icns.argb && png.argb[0] == 0x80643219 && png.argb[1] == 0xff010203);
    { std::fstream f(icns_path, std::ios::in | std::ios::out | std::ios::binary); f.seekp(12); char bad[4]{}; f.write(bad, 4); }
    require(ak::read_application_icon(icns_path).argb.empty());
    ak::application_identity identity; identity.app_id = "machoman.org.test.icon"; identity.name = "Generic\nApp"; identity.icon = png;
    require(ak::install_application_icon(identity, root));
    const auto installed = ak::read_application_icon(root / "icons/hicolor/256x256/apps/machoman.org.test.icon.png");
    require(installed.width == 256 && installed.height == 256 && installed.argb.front() == png.argb.front());
    std::ifstream desktop(root / "applications/machoman.org.test.icon.desktop");
    const std::string contents{std::istreambuf_iterator<char>(desktop), {}};
    require(contents.find("Name=Generic\\nApp\n") != std::string::npos && contents.find("StartupWMClass=machoman.org.test.icon") != std::string::npos);
    identity.app_id = "../escape";
    require(!ak::install_application_icon(identity, root));
    require(!ak::install_application_icon(identity, "relative-path"));
    puts("PNG/ICNS icons, premultiplied alpha, desktop association and malformed input passed");
}
