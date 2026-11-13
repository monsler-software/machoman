#include "cache.h"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace mevk {
namespace {
namespace fs=std::filesystem;
std::atomic<std::uint64_t> hits{0},misses{0},writes{0},rejected{0},serial{0};
constexpr std::uint32_t version=2,max_entry_bytes=64*1024*1024;
constexpr std::array<std::uint8_t,8> magic={'M','E','V','K','S','C','0','2'};
auto digest(std::span<const std::uint8_t> bytes) {
    return llvm::SHA256::hash(llvm::ArrayRef(bytes.data(),bytes.size()));
}
std::string hex(const std::array<std::uint8_t,32>& bytes) {
    constexpr char digits[]="0123456789abcdef";std::string result;
    for(auto b:bytes){result+=digits[b>>4];result+=digits[b&15];}return result;
}
fs::path path(std::span<const std::uint8_t> air) {
    auto directory=shader_cache_directory();
    if(directory.empty())return {};
    return fs::path(directory)/MEVK_SHADER_CACHE_REVISION/(hex(digest(air))+".aircache");
}
void word(std::vector<std::uint8_t>& out,std::uint32_t x) {
    for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(x>>(8*i)));
}
void bytes(std::vector<std::uint8_t>& out,std::span<const std::uint8_t> x) {
    if(x.size()>max_entry_bytes)throw std::runtime_error("cache size");
    word(out,static_cast<std::uint32_t>(x.size()));out.insert(out.end(),x.begin(),x.end());
}
void string(std::vector<std::uint8_t>& out,const std::string& x) {
    bytes(out,std::span(reinterpret_cast<const std::uint8_t*>(x.data()),x.size()));
}
struct Reader {
    std::span<const std::uint8_t> remaining;
    std::span<const std::uint8_t> take(std::size_t size) {
        if(size>remaining.size())throw std::runtime_error("cache truncated");
        auto x=remaining.first(size);remaining=remaining.subspan(size);return x;
    }
    std::uint32_t word() {
        auto x=take(4);std::uint32_t n=0;
        for(unsigned i=0;i<4;++i)n|=std::uint32_t{x[i]}<<(i*8);return n;
    }
    std::string string(std::size_t limit) {
        auto size=word();if(size>limit)throw std::runtime_error("cache string size");
        auto x=take(size);return {reinterpret_cast<const char*>(x.data()),x.size()};
    }
    bool boolean(){auto x=word();if(x>1)throw std::runtime_error("cache boolean");return x!=0;}
};
bool binding_valid(const ShaderBinding& b,ShaderStage stage) {
    if(b.sampler&&b.texture)return false;
    if(b.sampler&&b.metal_index==UINT32_MAX)return b.binding==128&&b.literal_sampler==0x807bff0000080a49ull;
    if(b.metal_index>=(b.texture?32u:16u)||b.sample_type>2||b.literal_sampler)return false;
    const auto expected=b.metal_index+(stage==ShaderStage::fragment?(b.texture?32u:16u):0u)+(b.texture?32u:b.sampler?96u:0u);
    return b.binding==expected;
}
} // namespace
ShaderCacheStats shader_cache_stats(){return {hits.load(),misses.load(),writes.load(),rejected.load()};}
std::string shader_cache_directory() {
    if(const char* value=std::getenv("MEVK_SHADER_CACHE"))return std::string(value)=="0"?"":value;
#ifdef _WIN32
    if(const char* value=std::getenv("LOCALAPPDATA"))return (fs::path(value)/"machoman"/"mevk"/"shaders").string();
#endif
    if(const char* value=std::getenv("XDG_CACHE_HOME");value&&*value)return (fs::path(value)/"machoman"/"mevk"/"shaders").string();
    if(const char* value=std::getenv("HOME");value&&*value)return (fs::path(value)/".cache"/"machoman"/"mevk"/"shaders").string();
    return {};
}
namespace shader_cache {
bool read(std::span<const std::uint8_t> air,ShaderFunction& output) {
    try {
        const auto file=path(air);if(file.empty())return false;
        std::ifstream input(file,std::ios::binary|std::ios::ate);
        if(!input){++misses;return false;}
        const auto size=input.tellg();
        if(size<48||size>max_entry_bytes)throw std::runtime_error("cache file size");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));input.seekg(0);
        input.read(reinterpret_cast<char*>(data.data()),size);if(!input)throw std::runtime_error("cache read");
        Reader file_reader{data};auto signature=file_reader.take(magic.size());
        if(!std::equal(signature.begin(),signature.end(),magic.begin())||file_reader.word()!=version)throw std::runtime_error("cache version");
        auto payload_size=file_reader.word();auto checksum=file_reader.take(32);auto payload=file_reader.take(payload_size);
        if(!file_reader.remaining.empty())throw std::runtime_error("cache trailing bytes");
        auto actual=digest(payload);if(!std::equal(actual.begin(),actual.end(),checksum.begin()))throw std::runtime_error("cache checksum");
        Reader reader{payload};auto original=reader.take(reader.word());
        if(original.size()!=air.size()||!std::equal(original.begin(),original.end(),air.begin()))throw std::runtime_error("cache AIR mismatch");
        ShaderFunction shader;auto stage=reader.word();if(stage>2)throw std::runtime_error("cache stage");
        shader.stage=static_cast<ShaderStage>(stage);shader.name=reader.string(4096);
        if(shader.name.empty())throw std::runtime_error("cache name");
        auto count=reader.word();if(count>128)throw std::runtime_error("cache reflection count");
        for(unsigned i=0;i<count;++i) {
            ShaderBinding b;b.metal_index=reader.word();b.binding=reader.word();b.texture=reader.boolean();b.sampler=reader.boolean();
            b.texture_type=reader.word();b.sample_type=reader.word();b.writable=reader.boolean();
            b.literal_sampler=reader.word();b.literal_sampler|=std::uint64_t{reader.word()}<<32;
            if(!binding_valid(b,shader.stage))throw std::runtime_error("cache reflection");
            for(auto previous:shader.bindings)if(previous.binding==b.binding)throw std::runtime_error("cache duplicate binding");
            shader.bindings.push_back(b);
        }
        count=reader.word();if(count<5||count>4*1024*1024||count>reader.remaining.size()/4)throw std::runtime_error("cache SPIR-V length");
        shader.spirv.reserve(count);for(unsigned i=0;i<count;++i)shader.spirv.push_back(reader.word());
        if(shader.spirv[0]!=0x07230203u)throw std::runtime_error("cache SPIR-V magic");
        shader.glsl=reader.string(32*1024*1024);if(!reader.remaining.empty())throw std::runtime_error("cache trailing payload");
        output=std::move(shader);++hits;return true;
    } catch(const std::exception&) {++rejected;++misses;return false;}
}
void write(std::span<const std::uint8_t> air,const ShaderFunction& shader) {
    fs::path temporary;
    try {
        const auto file=path(air);if(file.empty())return;
        std::vector<std::uint8_t> payload;bytes(payload,air);word(payload,static_cast<std::uint32_t>(shader.stage));string(payload,shader.name);
        word(payload,static_cast<std::uint32_t>(shader.bindings.size()));
        for(auto b:shader.bindings) {
            word(payload,b.metal_index);word(payload,b.binding);word(payload,b.texture);word(payload,b.sampler);
            word(payload,b.texture_type);word(payload,b.sample_type);word(payload,b.writable);
            word(payload,static_cast<std::uint32_t>(b.literal_sampler));word(payload,static_cast<std::uint32_t>(b.literal_sampler>>32));
        }
        word(payload,static_cast<std::uint32_t>(shader.spirv.size()));for(auto x:shader.spirv)word(payload,x);string(payload,shader.glsl);
        if(payload.size()>max_entry_bytes-48)return;
        std::vector<std::uint8_t> data(magic.begin(),magic.end());word(data,version);word(data,static_cast<std::uint32_t>(payload.size()));
        auto checksum=digest(payload);data.insert(data.end(),checksum.begin(),checksum.end());data.insert(data.end(),payload.begin(),payload.end());
        std::error_code error;fs::create_directories(file.parent_path(),error);if(error)return;
        static const auto nonce=std::random_device{}();
        temporary=file.string()+"."+std::to_string(nonce)+"."+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"."+std::to_string(serial++)+".tmp";
        std::ofstream output(temporary,std::ios::binary|std::ios::noreplace);
        if(!output)return;
        output.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));output.close();
        if(output){fs::rename(temporary,file,error);if(!error)++writes;}
        fs::remove(temporary,error);
    } catch(const std::exception&) {if(!temporary.empty()){std::error_code error;fs::remove(temporary,error);}}
}
} // namespace shader_cache
} // namespace mevk
