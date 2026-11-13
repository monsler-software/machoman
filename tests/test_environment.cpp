#include "machoman/environment.h"
#include "machoman/json.h"
#include "machoman/venv.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace fs=std::filesystem;
static void check(bool value, const char *what) { if (!value) throw std::runtime_error(what); }
static void write(const fs::path &path, const std::string &text) { std::ofstream file(path, std::ios::binary); file << text; check(bool(file), "fixture write"); }
static std::string read(const fs::path &path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
int main(int argc, char **argv) {
    try {
        char original[]="ORIGINAL=one";
        char *initial[]={original, nullptr};
        mm_environment env(initial), other(initial);
        original[9]='x';
        check(std::strcmp(env.get("ORIGINAL"), "one")==0, "initial environment copied");
        check(env.set("ORIGINAL", "two", 0)==0 && std::strcmp(env.get("ORIGINAL"), "one")==0, "overwrite false");
        check(env.set("ORIGINAL", "two", 1)==0 && std::strcmp(other.get("ORIGINAL"), "one")==0, "process isolation");
        char borrowed[]="BORROW=first";
        check(env.put(borrowed)==0, "putenv");
        std::memcpy(borrowed+7, "other", 5);
        check(std::strcmp(env.get("BORROW"), "other")==0, "borrowed buffer remains live");
        char direct[]="DIRECT=chosen";
        char *replacement[]={direct, nullptr};
        char **exported=nullptr;
        env.bind(&exported);
        exported=replacement;
        check(std::strcmp(env.get("DIRECT"), "chosen")==0 && !env.get("ORIGINAL"), "direct environ replacement");
        check(env.set("APPEND", "", 1)==0 && exported==env.data() && env.get("APPEND")[0]==0, "mutate replaced array");
        check(env.unset("DIRECT")==0 && !env.get("DIRECT"), "unset");
        check(env.set("", "x", 1)==EINVAL && env.set("A=B", "x", 1)==EINVAL && env.put(direct+7)==EINVAL, "invalid names");
        env.bind(nullptr);
        std::thread first([&] { for (int i=0; i<100; ++i) check(env.set("THREAD_A", "a", 1)==0, "thread A"); });
        std::thread second([&] { for (int i=0; i<100; ++i) check(env.set("THREAD_B", "b", 1)==0, "thread B"); });
        first.join(); second.join();
        check(env.get("THREAD_A") && env.get("THREAD_B"), "concurrent setters");
        mm_json document; std::string error;
        check(mm_json_parse("{\"future\":[true,null,123456789012345678901234567890],\"unicode\":\"\\ud83d\\ude00\"}", document, error), "JSON unicode/large number");
        const auto serialized=mm_json_serialize(document);
        check(serialized.find("123456789012345678901234567890")!=std::string::npos && mm_json_parse(serialized, document, error), "lossless JSON roundtrip");
        for (const auto text : {"{\"x\":1,\"x\":2}", "[1,]", "01", "\"\\ud800\"", "{\"x\":true}garbage"})
            check(!mm_json_parse(text, document, error), "malformed JSON rejected");
        check(argc==2, "fixture path required");
        const auto root=fs::path(argv[1]);
        fs::remove_all(root);
        mm_venv venv;
        check(mm_venv_create(root, venv)==mm_venv_status::ok, "venv creation");
        const auto manifest=venv.path(MM_VENV_MANIFEST_FILE);
        write(manifest, "{\"machoman_venv_version\":1,\"future\":{\"n\":12345678901234567890},\"env\":{\"MASK\":null,\"EMPTY\":\"\"}}");
        check(mm_venv_open(root, venv)==mm_venv_status::ok && !venv.env.at("MASK") && venv.env.at("EMPTY")=="", "null versus empty");
        check(mm_venv_update_environment(venv, "NAME", "café\n\"quote\"")==mm_venv_status::ok, "persistent UTF-8 env");
        check(read(manifest).find("12345678901234567890")!=std::string::npos, "unknown fields preserved");
        const auto before=read(manifest);
        const std::string bad_utf8(1, static_cast<char>(0xff));
        check(mm_venv_update_environment(venv, "BAD", bad_utf8)==mm_venv_status::invalid_environment && read(manifest)==before, "invalid UTF-8 does not corrupt manifest");
        check(mm_venv_update_environment(venv, "NAME", std::nullopt)==mm_venv_status::ok && !venv.env.at("NAME"), "persistent mask");
        check(mm_venv_update_environment(venv, "NAME", std::nullopt, true)==mm_venv_status::ok && !venv.env.contains("NAME"), "restore inheritance");
#ifndef _WIN32
        for (int i=0; i<4; ++i) {
            const auto child=::fork(); check(child>=0, "fork config writer");
            if (child==0) {
                mm_venv local; if (mm_venv_open(root, local)!=mm_venv_status::ok) ::_exit(1);
                const auto status=mm_venv_update_environment(local, "WRITER_"+std::to_string(i), "ok");
                ::_exit(status==mm_venv_status::ok ? 0 : 1);
            }
        }
        for (int i=0; i<4; ++i) { int status; check(::wait(&status)>0 && WIFEXITED(status) && WEXITSTATUS(status)==0, "concurrent config writer"); }
        check(mm_venv_open(root, venv)==mm_venv_status::ok, "reload concurrent config");
        for (int i=0; i<4; ++i) check(venv.env.contains("WRITER_"+std::to_string(i)), "no lost config update");
#endif
        write(manifest, "{\"env\":{\"A\":42}}");
        check(mm_venv_open(root, venv)==mm_venv_status::invalid_environment, "bad env schema rejected");
        std::puts("Environment ownership, live buffers, JSON and atomic manifest configuration passed");
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
