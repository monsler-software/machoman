#include "mevk/shader.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

void check(bool yes,const char* message){if(!yes){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
std::vector<std::uint8_t> bitcode(const std::string& name) {
    llvm::LLVMContext context;llvm::SMDiagnostic diagnostic;
    auto module=llvm::parseAssemblyString("define <4 x float> @"+name+"(ptr %b,ptr %t,ptr %s){ret <4 x float> zeroinitializer}\n"
      "!air.fragment=!{!0}\n!0=!{ptr @"+name+",!1,!2}\n!1=!{}\n!2=!{!3,!4,!5}\n"
      "!3=!{i32 0,!\"air.buffer\",!\"air.location_index\",i32 2,!\"air.read\"}\n"
      "!4=!{i32 1,!\"air.texture\",!\"air.location_index\",i32 16,!\"air.arg_type_name\",!\"texture3d<float, sample>\"}\n"
      "!5=!{i32 2,!\"air.sampler\",!\"air.location_index\",i32 15}\n",diagnostic,context);
    check(bool(module),"assemble cache fixture");
    llvm::SmallVector<char,0> bytes;llvm::raw_svector_ostream stream(bytes);llvm::WriteBitcodeToFile(*module,stream);
    return {bytes.begin(),bytes.end()};
}
void equal(const mevk::ShaderFunction& a,const mevk::ShaderFunction& b) {
    check(a.name==b.name&&a.stage==b.stage&&a.spirv==b.spirv&&a.glsl==b.glsl,"cached shader source/SPIR-V/stage/name preserved");
    check(a.bindings.size()==b.bindings.size(),"cached reflection length preserved");
    for(unsigned i=0;i<a.bindings.size();++i) {
        auto x=a.bindings[i],y=b.bindings[i];
        check(x.metal_index==y.metal_index&&x.binding==y.binding&&x.texture==y.texture&&x.sampler==y.sampler&&
              x.texture_type==y.texture_type&&x.sample_type==y.sample_type&&x.writable==y.writable&&x.literal_sampler==y.literal_sampler,"cached reflection preserved");
    }
}
int main() {
    namespace fs=std::filesystem;
    const auto nonce=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto air=bitcode("cache"+nonce);mevk::ShaderFunction original,cached;std::string error;
    const auto before=mevk::shader_cache_stats();
    check(mevk::translate_air(air,original,error),error.c_str());
    auto cold=mevk::shader_cache_stats();
    check(cold.misses==before.misses+1&&cold.writes==before.writes+1,"cold translation persisted");
    check(mevk::translate_air(air,cached,error),error.c_str());equal(original,cached);
    check(mevk::shader_cache_stats().hits==before.hits+1,"warm translation reads disk cache");
    fs::path entry;
    for(const auto& file:fs::recursive_directory_iterator(mevk::shader_cache_directory())) {
        if(file.path().extension()!=".aircache")continue;
        std::ifstream input(file.path(),std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(input)),{});
        if(data.find(original.name)!=std::string::npos){entry=file.path();break;}
    }
    check(!entry.empty(),"find persisted fixture");
    {
        std::fstream output(entry,std::ios::binary|std::ios::in|std::ios::out);
        output.seekp(-1,std::ios::end);output.put('\xff');check(bool(output),"corrupt cached payload");
    }
    auto stats=mevk::shader_cache_stats();
    check(mevk::translate_air(air,cached,error),error.c_str());equal(original,cached);
    check(mevk::shader_cache_stats().rejected==stats.rejected+1&&mevk::shader_cache_stats().writes==stats.writes+1,"corrupt artifact rejected and regenerated");
    {
        std::ofstream output(entry,std::ios::binary|std::ios::trunc);output<<"MEVKSC02";
    }
    stats=mevk::shader_cache_stats();
    check(mevk::translate_air(air,cached,error),error.c_str());equal(original,cached);
    check(mevk::shader_cache_stats().rejected==stats.rejected+1,"truncated artifact rejected");
    auto concurrent=bitcode("parallel"+nonce);
    std::array<mevk::ShaderFunction,4> outputs;
    std::array<std::thread,4> workers;
    for(unsigned i=0;i<workers.size();++i)workers[i]=std::thread([&,i]{std::string diagnostic;check(mevk::translate_air(concurrent,outputs[i],diagnostic),diagnostic.c_str());});
    for(auto& worker:workers)worker.join();for(auto& shader:outputs)equal(outputs[0],shader);
    check(mevk::translate_air(concurrent,cached,error),error.c_str());equal(outputs[0],cached);
    for(const auto& file:fs::recursive_directory_iterator(mevk::shader_cache_directory()))check(file.path().extension()!=".tmp","atomic writers leave no partial files");
    std::puts("MEVK persistent shader cache, corruption recovery and concurrent writer tests passed");
}
