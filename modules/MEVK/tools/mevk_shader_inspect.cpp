#include "mevk/shader.h"
#include <cstdio>
#include <fstream>
#include <iterator>
int main(int argc,char** argv) {
    if(argc!=2){std::fprintf(stderr,"usage: mevk-shader-inspect library.metallib\n");return 2;}
    std::ifstream file(argv[1],std::ios::binary);
    if(!file){std::fprintf(stderr,"cannot open metallib\n");return 1;}
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    std::vector<mevk::ShaderFunction> functions;std::string error;
    if(!mevk::load_metallib(bytes,functions,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
    for(const auto& f:functions)std::printf("%s %s: %zu SPIR-V words, %zu bindings\n",f.stage==mevk::ShaderStage::vertex?"vertex":"fragment",f.name.c_str(),f.spirv.size(),f.bindings.size());
    return 0;
}
