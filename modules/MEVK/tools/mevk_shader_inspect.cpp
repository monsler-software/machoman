#include "mevk/shader.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc<2||argc>4){std::fprintf(stderr,"usage: mevk-shader-inspect library.metallib [--pack] [dump-directory]\n");return 2;}
    std::ifstream file(argv[1],std::ios::binary);
    if(!file){std::fprintf(stderr,"cannot open metallib\n");return 1;}
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    std::vector<mevk::ShaderFunction> functions;std::string error;
    if(argc>=3&&std::strcmp(argv[2],"--air")==0) {
        mevk::ShaderFunction shader;
        bool ok=mevk::translate_air(bytes,shader,error);
        if(argc==4)std::ofstream(argv[3])<<shader.glsl;
        std::printf("%s\n",ok?"OK":error.c_str());return ok?0:1;
    }
    if(argc>=3&&std::strcmp(argv[2],"--pack")==0) {
        struct Entry {std::size_t offset,size;bool ok=false;std::string error;};
        std::vector<Entry> entries;
        for(std::size_t i=0;i+88<=bytes.size();++i) {
            if(std::memcmp(bytes.data()+i,"MTLB",4)!=0)continue;
            std::uint64_t size=0;
            for(unsigned n=0;n<8;++n)size|=std::uint64_t{bytes[i+16+n]}<<(8*n);
            if(size<88||size>bytes.size()-i)continue;
            entries.push_back({i,static_cast<std::size_t>(size),false,{}});
            i+=static_cast<std::size_t>(size)-1;
        }
        if(argc==4)std::filesystem::create_directories(argv[3]);
        const auto start=std::chrono::steady_clock::now();
        std::atomic<std::size_t> next{0};std::vector<std::thread> workers;
        const auto count=std::min<std::size_t>(entries.size(),std::clamp(std::thread::hardware_concurrency(),1u,8u));
        for(std::size_t worker=0;worker<count;++worker)workers.emplace_back([&]{
            for(;;) {
                const auto index=next.fetch_add(1);if(index>=entries.size())break;
                auto& entry=entries[index];std::vector<mevk::ShaderFunction> translated;
                entry.ok=mevk::load_metallib(std::span(bytes).subspan(entry.offset,entry.size),translated,entry.error);
                if(argc==4) {
                    const auto prefix=std::filesystem::path(argv[3])/std::to_string(index);
                    std::ofstream lib(prefix.string()+".metallib",std::ios::binary);
                    lib.write(reinterpret_cast<const char*>(bytes.data()+entry.offset),static_cast<std::streamsize>(entry.size));
                    for(std::size_t function=0;function<translated.size();++function)
                        std::ofstream(prefix.string()+"."+std::to_string(function)+".glsl")<<translated[function].glsl;
                }
            }
        });
        for(auto& worker:workers)worker.join();
        std::size_t passed=0;
        for(std::size_t index=0;index<entries.size();++index) {
            const auto& entry=entries[index];if(entry.ok)++passed;
            std::printf("%zu %s %s\n",index,entry.ok?"OK":"FAIL",entry.ok?"":entry.error.c_str());
        }
        std::printf("Translated %zu/%zu embedded metallibs in %.3f seconds\n",passed,entries.size(),
            std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
        auto stats=mevk::shader_cache_stats();
        std::printf("Shader cache: %llu hits, %llu misses, %llu writes, %llu rejected\n",
            static_cast<unsigned long long>(stats.hits),static_cast<unsigned long long>(stats.misses),static_cast<unsigned long long>(stats.writes),static_cast<unsigned long long>(stats.rejected));
        return !entries.empty()&&passed==entries.size()?0:1;
    }
    if(!mevk::load_metallib(bytes,functions,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
    for(const auto& f:functions)std::printf("%s %s: %zu SPIR-V words, %zu bindings\n",f.stage==mevk::ShaderStage::vertex?"vertex":"fragment",f.name.c_str(),f.spirv.size(),f.bindings.size());
    return 0;
}
