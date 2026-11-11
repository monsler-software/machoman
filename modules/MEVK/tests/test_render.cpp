#include "mevk/buffer.h"
#include "mevk/command_buffer.h"
#include "mevk/context.h"
#include "mevk/render.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {
void check(bool yes,const std::string& message){if(!yes){std::fprintf(stderr,"FAIL: %s\n",message.c_str());std::exit(1);}}
std::shared_ptr<mevk::Buffer> bytes(mevk::Context& context,const void* source,std::uint64_t size) {
    std::string error;auto b=context.create_buffer(size,error);check(b!=nullptr,error);
    std::memcpy(b->contents(),source,static_cast<std::size_t>(size));return b;
}
const mevk::ShaderFunction& shader(const std::vector<mevk::ShaderFunction>& shaders,const char* name) {
    for(auto& s:shaders)if(s.name==name)return s;
    check(false,std::string("missing shader ")+name);std::abort();
}
std::shared_ptr<mevk::RenderPipeline> pipeline(mevk::Context& c,const std::vector<mevk::ShaderFunction>& shaders,
    const char* vertex,const char* fragment,bool blend=false) {
    mevk::BlendState state;state.enabled=blend;state.destination_rgb=blend?5u:0u;state.destination_alpha=blend?5u:0u;
    std::string error;auto p=c.create_render_pipeline(shader(shaders,vertex),shader(shaders,fragment),80,state,error);
    check(p!=nullptr,error);return p;
}
void render(mevk::Context& c,const std::shared_ptr<mevk::Texture>& target,const std::shared_ptr<mevk::RenderPipeline>& p,
    const mevk::RenderBindings& bindings,const mevk::Scissor& clip={0,0,64,64},const std::shared_ptr<mevk::Buffer>& indices={}) {
    std::string error;auto cmd=c.create_command_buffer(error);check(cmd!=nullptr,error);
    check(cmd->begin_render(target,true,{0,0,1,1},error),error);
    check(cmd->draw(p,bindings,{0,0,64,64,0,1},clip,4,0,indices?6:4,1,0,indices,indices?2:0,0,0,error),error);
    check(cmd->end_render(error),error);check(cmd->commit(error),error);check(cmd->wait_until_completed(error),error);
}
std::vector<std::uint8_t> read(const std::shared_ptr<mevk::Texture>& target) {
    std::vector<std::uint8_t> result;std::string error;check(target->read_pixels(result,error),error);return result;
}
void rectangle(const std::vector<std::uint8_t>& data,unsigned x0,unsigned y0,unsigned x1,unsigned y1,
               const std::array<int,4>& inside={0,0,255,255}) {
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        auto expected=x>=x0&&x<x1&&y>=y0&&y<y1?inside:std::array<int,4>{255,0,0,255};
        for(unsigned n=0;n<4;++n)if(std::abs(int(data[(y*64+x)*4+n])-expected[n])>1)
            check(false,"pixel mismatch at "+std::to_string(x)+","+std::to_string(y)+" component "+std::to_string(n));
    }
}
}
int main(int argc,char** argv) {
    check(argc>=2,"usage: test_mevk_render library.metallib [image.ppm]");
    std::ifstream input(argv[1],std::ios::binary);check(bool(input),"open fixture metallib");
    std::vector<std::uint8_t> library((std::istreambuf_iterator<char>(input)),{});
    std::vector<mevk::ShaderFunction> shaders;std::string error;
    check(mevk::load_metallib(library,shaders,error),error);check(shaders.size()==16,"all 16 AIR functions translated");
    auto context=mevk::Context::create(error);check(context!=nullptr,error);
    auto target=context->create_texture(64,64,80,error);check(target!=nullptr,error);
    float rect[]={8,8,32,32};float viewport[]={64,64};float color[]={1,0,0,1};
    mevk::RenderBindings bindings;
    bindings.buffers[0]=bytes(*context,rect,sizeof rect);
    // Exercise unaligned Metal offsets without violating Vulkan descriptor alignment.
    float offset_uniforms[]={-1,64,64};bindings.buffers[1]=bytes(*context,offset_uniforms,sizeof offset_uniforms);bindings.offsets[1]=4;
    bindings.buffers[16]=bytes(*context,color,sizeof color);
    auto rect_pipeline=pipeline(*context,shaders,"RectsVertexShader","IndexedColorFragmentShader");
    render(*context,target,rect_pipeline,bindings);rectangle(read(target),8,8,32,32);
    std::puts("actual AIR rectangle shader and nonaligned buffer offset passed");

    float half_red[]={0.5f,0,0,0.5f};bindings.buffers[16]=bytes(*context,half_red,sizeof half_red);
    auto blended=pipeline(*context,shaders,"RectsVertexShader","IndexedColorFragmentShader",true);
    std::uint16_t index_data[]={0xffff,0,1,2,2,1,3};auto indices=bytes(*context,index_data,sizeof index_data);
    // Triangle list for indexed drawing, while nonindexed paths use a strip.
    auto cmd=context->create_command_buffer(error);check(cmd!=nullptr,error);
    check(cmd->begin_render(target,true,{0,0,1,1},error),error);
    check(cmd->draw(blended,bindings,{0,0,64,64,0,1},{10,10,16,16},3,0,6,1,0,indices,2,0,0,error),error);
    check(cmd->end_render(error)&&cmd->commit(error)&&cmd->wait_until_completed(error),error);
    rectangle(read(target),10,10,26,26,{128,0,128,255});
    std::puts("indexed draw, scissor and premultiplied blending passed");

    float vertices[]={8,8,0,0,32,8,1,0,8,32,0,1,32,32,1,1};
    auto sampled=context->create_texture(2,2,70,error);check(sampled!=nullptr,error);
    unsigned char image[]={255,0,0,255,0,255,0,255,0xaa,0xaa,0xaa,0xaa,
                           0,0,255,255,255,255,255,255,0xbb,0xbb,0xbb,0xbb};
    check(sampled->replace(0,0,2,2,image,12,error),error);
    auto uploaded=read(sampled);check(uploaded.size()==16&&uploaded[0]==255&&uploaded[10]==255,"strided texture upload/readback");
    mevk::RenderBindings textured;textured.buffers[0]=bytes(*context,vertices,sizeof vertices);
    textured.buffers[1]=bytes(*context,viewport,sizeof viewport);textured.textures[16]=sampled;
    auto texture_pipeline=pipeline(*context,shaders,"TextureVertexShader","TextureFragmentShader");
    render(*context,target,texture_pipeline,textured);auto pixels=read(target);
    for(unsigned y=8;y<32;++y)for(unsigned x=8;x<32;++x) {
        double u=std::clamp((double(x)+0.5-8)/24*2-0.5,0.0,1.0);
        double v=std::clamp((double(y)+0.5-8)/24*2-0.5,0.0,1.0);
        int expected[]={int(std::lround(v*255)),int(std::lround(u*255)),int(std::lround(((1-u)*(1-v)+u*v)*255)),255};
        for(unsigned n=0;n<4;++n)check(std::abs(int(pixels[(y*64+x)*4+n])-expected[n])<=2,"textured AIR shader pixel interpolation");
    }
    std::puts("AIR texture shader, half conversion and bilinear sampling passed");

    float positions[]={0,0,64,0,0,64,64,64};std::uint16_t segment_count=4;
    float segments[]={8,8,32,8,32,8,32,32,32,32,8,32,8,32,8,8};
    mevk::RenderBindings polygon;polygon.buffers[0]=bytes(*context,positions,sizeof positions);polygon.buffers[1]=bytes(*context,viewport,sizeof viewport);
    polygon.buffers[16]=bytes(*context,color,sizeof color);polygon.buffers[17]=bytes(*context,&segment_count,sizeof segment_count);
    polygon.buffers[18]=bytes(*context,segments,sizeof segments);
    auto poly=pipeline(*context,shaders,"PosVertexShader","PolyLineFragmentShader",true);
    render(*context,target,poly,polygon);pixels=read(target);rectangle(pixels,8,8,32,32);
    std::puts("AIR control-flow loops, simultaneous phi transfers and ushort parameters passed");

    // A rejected draw must not submit the earlier recorded clear.
    auto invalid=context->create_command_buffer(error);check(invalid!=nullptr,error);
    check(invalid->begin_render(target,true,{0,1,0,1},error),error);
    check(!invalid->draw(rect_pipeline,{}, {0,0,64,64,0,1},{0,0,64,64},4,0,4,1,0,{},0,0,0,error),"missing bindings rejected");
    check(!invalid->commit(error),"invalid render batch not submitted");rectangle(read(target),8,8,32,32);
    check(!context->create_texture(1,1,999,error),"unsupported format rejected");

    auto truncated=library;truncated.resize(30);std::vector<mevk::ShaderFunction> rejected;
    check(!mevk::load_metallib(truncated,rejected,error)&&rejected.empty(),"truncated library rejected");
    if(argc>2){std::ofstream image_file(argv[2],std::ios::binary);image_file<<"P6\n64 64\n255\n";
        for(std::size_t i=0;i<pixels.size();i+=4){char rgb[]={static_cast<char>(pixels[i+2]),static_cast<char>(pixels[i+1]),static_cast<char>(pixels[i])};image_file.write(rgb,3);}}
    std::puts("MEVK AIR renderer tests passed");return 0;
}
