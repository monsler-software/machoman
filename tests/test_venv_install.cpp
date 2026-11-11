#include "machoman/install.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
static fs::path path_utf8(std::string_view s) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
static void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static std::string read(const fs::path &path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void write(const fs::path &path, const std::string &value) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary); f.write(value.data(), static_cast<std::streamsize>(value.size()));
    check(bool(f), "writing fixture failed");
}
static void macho(const fs::path &path, bool arm64 = true) {
    std::string header(32, '\0');
    auto field = [&](unsigned p, unsigned value) { for (unsigned i = 0; i < 4; ++i) header[p + i] = static_cast<char>(value >> (8 * i)); };
    field(0, 0xfeedfacf); field(4, arm64 ? 0x100000c : 0x1000007); field(12, 2);
    write(path, header);
}
static fs::path app(const fs::path &root, const std::string &name, const std::string &command, const std::string &xml_command = "") {
    const auto bundle = root / path_utf8(name);
    write(bundle / "Contents/Info.plist", "<?xml version=\"1.0\"?><plist><dict><!-- ignored <key>CFBundleExecutable</key><string>bad</string> -->"
        "<key>CFBundleExecutable</key><string>" + (xml_command.empty() ? command : xml_command) + "</string></dict></plist>");
    macho(bundle / "Contents/MacOS" / path_utf8(command));
    write(bundle / "Contents/Resources/picture", "resource bytes");
    return bundle;
}
static std::string binary_plist() {
    std::string data = "bplist00";
    std::array<unsigned, 3> offsets{};
    offsets[0] = static_cast<unsigned>(data.size());
    data += char(0x5f); data += char(0x10); data += char(18); data += "CFBundleExecutable";
    offsets[1] = static_cast<unsigned>(data.size());
    const std::u16string text = u"二 Main";
    data += static_cast<char>(0x60 | text.size());
    for (auto c : text) { data += static_cast<char>(c >> 8); data += static_cast<char>(c & 255); }
    offsets[2] = static_cast<unsigned>(data.size());
    data += char(0xd1); data += char(0); data += char(1);
    const auto table = data.size();
    for (auto offset : offsets) data += static_cast<char>(offset);
    std::string trailer(32, '\0'); trailer[6] = 1; trailer[7] = 1; trailer[15] = 3; trailer[23] = 2; trailer[31] = static_cast<char>(table);
    return data + trailer;
}
static mm_venv venv(const fs::path &root) { mm_venv v; check(mm_venv_create(root, v) == mm_venv_status::ok, "venv creation failed"); return v; }
static fs::path image(const fs::path &fixture, const fs::path &source, const std::string &name) {
    const auto path = fixture / path_utf8(name); write(path, source.string() + "\n"); return path;
}
static bool clean(const mm_venv &v) {
    for (const auto &entry : fs::directory_iterator(v.root)) if (entry.path().filename().string().starts_with(".machoman-install-")) return false;
    return true;
}
/* The same native test binary acts as an argv-sensitive extraction probe.
 * Actual DMG/HFS extraction is additionally checked with real 7-Zip. */
static int extraction_probe(int argc, char **argv) {
    fs::path output, input;
    bool separator = false, noninteractive = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (separator) { input = path_utf8(arg); break; }
        if (arg == "--") separator = true;
        else if (arg == "-p-") noninteractive = true;
        else if (arg.starts_with("-o")) output = path_utf8(arg.substr(2));
    }
    if (!separator || !noninteractive || output.empty() || input.empty()) return 7;
    std::ifstream f(input); std::string source; std::getline(f, source);
    if (source == "ERROR") return 2;
    fs::copy(path_utf8(source), output, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    return 0;
}
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "x") return extraction_probe(argc, argv);
        check(argc == 2, "fixture path required");
        const auto fixture = fs::absolute(argv[1]); fs::remove_all(fixture); fs::create_directories(fixture);
        mm_install_options options; options.seven_zip = fs::canonical(argv[0]);
        auto prefix = venv(fixture / "venv space ' ; $(literal)");
        const auto source = fixture / "volume space ' ; $(literal)";
        const auto bundle = app(source, "Sample & café.app", "Main & café", "Main &amp; café");
        app(bundle / "Contents/Resources", "Nested.app", "Nested");
#ifndef _WIN32
        fs::create_symlink("Resources/picture", bundle / "Contents/picture-link");
#endif
        mm_install_result result;
        const auto dmg = image(fixture, source, "image space ' ; $(literal).dmg");
        const bool installed = mm_install_dmg(prefix, dmg, result, options);
        check(installed, result.error.c_str());
        check(result.apps.size() == 1 && result.apps[0].executable == "Main & café", "outer bundle discovery/metadata failed");
        check(!fs::exists(prefix.root / "bin/Nested"), "nested helper was registered as an application");
        check(read(result.apps[0].bundle / "Contents/Resources/picture") == "resource bytes", "resources were lost");
#ifndef _WIN32
        check(fs::is_symlink(result.apps[0].bundle / "Contents/picture-link"), "bundle symlink was lost");
#endif
        std::string error;
        check(mm_venv_resolve_executable(prefix, "Main & café", error) == fs::canonical(result.apps[0].bundle / "Contents/MacOS/Main & café") && error.empty(), "launcher did not resolve into bundle");
        check(clean(prefix), "staging not cleaned after success");
        const auto moved = fixture / "relocated venv"; fs::rename(prefix.root, moved); prefix.root = moved;
        check(mm_venv_resolve_executable(prefix, "Main & café", error) == fs::canonical(moved / "Applications/Sample & café.app/Contents/MacOS/Main & café"), "relocated launcher failed");
        const auto record = read(prefix.root / "bin/Main & café");
        check(!mm_install_dmg(prefix, dmg, result, options) && result.apps.empty(), "existing install was overwritten");
        check(read(prefix.root / "bin/Main & café") == record && clean(prefix), "conflict changed original install");

        const auto multiple = fixture / "multiple";
        app(multiple, "First.app", "First");
        const auto second = app(multiple, "Second.app", "二 Main"); write(second / "Contents/Info.plist", binary_plist());
        auto multi_venv = venv(fixture / "multiple-venv");
        check(mm_install_dmg(multi_venv, image(fixture, multiple, "multi.dmg"), result, options) && result.apps.size() == 2, "multiple apps/binary UTF-16 plist failed");
        check(mm_venv_resolve_executable(multi_venv, "二 Main", error) == fs::canonical(multi_venv.root / "Applications/Second.app/Contents/MacOS/二 Main"), "binary plist executable was misread");

        const auto duplicate = fixture / "duplicate"; app(duplicate, "One.app", "Shared"); app(duplicate, "Two.app", "Shared");
        auto failed = venv(fixture / "failed-venv");
        check(!mm_install_dmg(failed, image(fixture, duplicate, "duplicate.dmg"), result, options), "duplicate command accepted");
        check(fs::is_empty(failed.root / "bin") && fs::is_empty(failed.root / "Applications") && clean(failed), "failed transaction installed a partial app");

        const auto invalid = fixture / "invalid"; auto bad = app(invalid, "Bad.app", "Bad");
        macho(bad / "Contents/MacOS/Bad", false);
        check(!mm_install_dmg(failed, image(fixture, invalid, "bad-arch.dmg"), result, options), "x86-only app accepted");
        macho(bad / "Contents/MacOS/Bad"); write(bad / "Contents/Info.plist", "bplist00broken");
        check(!mm_install_dmg(failed, image(fixture, invalid, "bad-plist.dmg"), result, options), "truncated binary plist accepted");
        write(fixture / "broken.dmg", "ERROR\n");
        check(!mm_install_dmg(failed, fixture / "broken.dmg", result, options) && clean(failed), "extractor failure left staging");
        const auto empty = fixture / "empty"; write(empty / "Installer.pkg", "package");
        check(!mm_install_dmg(failed, image(fixture, empty, "package.dmg"), result, options) && result.error.find(".pkg") != std::string::npos, "package-only image was accepted");

#ifndef _WIN32
        const auto escape = fixture / "escape"; auto escaping = app(escape, "Escape.app", "Escape");
        macho(escape / "outside"); fs::remove(escaping / "Contents/MacOS/Escape"); fs::create_symlink("../../../outside", escaping / "Contents/MacOS/Escape");
        check(!mm_install_dmg(failed, image(fixture, escape, "escape.dmg"), result, options), "executable symlink outside bundle accepted");
        const auto collision = fixture / "collision"; app(collision, "Collision.app", "Dangling");
        fs::create_symlink("missing", failed.root / "bin/Dangling");
        check(!mm_install_dmg(failed, image(fixture, collision, "collision.dmg"), result, options), "dangling destination symlink overwritten");
#endif
        macho(fixture / "outside-main");
        write(failed.root / "bin/escape", std::string(MM_VENV_LAUNCHER_MAGIC) + "\n../../outside-main\n");
        check(mm_venv_resolve_executable(failed, "escape", error).empty() && !error.empty(), "launcher escape accepted");
        write(failed.root / "bin/malformed", std::string(MM_VENV_LAUNCHER_MAGIC) + "\n../missing\nextra\n");
        check(mm_venv_resolve_executable(failed, "malformed", error).empty() && !error.empty(), "malformed launcher accepted");
        check(clean(failed), "staging remains after failed installations");
        std::puts("install: argv paths, bundles/resources/symlinks, binary/XML plists, relocation, conflicts and failed transactions passed");
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "install test: %s\n", e.what()); return 1; }
}
