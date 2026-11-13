#include "mevk/shader.h"
#include "mevk/context.h"
#include "mevk/render.h"
#include "mevk/command_buffer.h"
#include "mevk/buffer.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void check(bool yes,const std::string& message){if(!yes){std::fprintf(stderr,"FAIL: %s\n",message.c_str());std::exit(1);}}
mevk::ShaderFunction shader(const std::string& ir) {
    llvm::LLVMContext context;llvm::SMDiagnostic diagnostic;
    auto module=llvm::parseAssemblyString(ir,diagnostic,context);
    if(!module)diagnostic.print("AIR test",llvm::errs());
    check(bool(module),"assemble AIR resource fixture");
    llvm::SmallVector<char,0> bytes;llvm::raw_svector_ostream stream(bytes);llvm::WriteBitcodeToFile(*module,stream);
    mevk::ShaderFunction result;std::string error;
    check(mevk::translate_air(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()),result,error),error);
    return result;
}
int main() {
    auto vertex=shader(R"(
define <{<4 x float>}> @v(i32 %id) {
 %x = icmp eq i32 %id, 1
 %y = icmp eq i32 %id, 2
 %px = select i1 %x, float 3.0, float -1.0
 %py = select i1 %y, float 3.0, float -1.0
 %a = insertelement <4 x float> <float 0.0,float 0.0,float 0.0,float 1.0>, float %px, i32 0
 %b = insertelement <4 x float> %a, float %py, i32 1
 %r = insertvalue <{<4 x float>}> undef, <4 x float> %b, 0
 ret <{<4 x float>}> %r
}
!air.vertex = !{!0}
!0 = !{ptr @v,!1,!3}
!1 = !{!2}
!2 = !{!"air.position"}
!3 = !{!4}
!4 = !{i32 0,!"air.vertex_id"}
)");
    std::string error;auto context=mevk::Context::create(error);check(bool(context),error);
    mevk::TextureDescription depth_description;depth_description.width=3;depth_description.height=3;depth_description.pixel_format=252;
    auto depth_texture=context->create_texture(depth_description,error);check(bool(depth_texture),error);
    const float depth_values[]={0.0f,0.125f,0.25f,0.375f,0.5f,0.625f,0.75f,0.875f,1.0f};
    check(depth_texture->replace(0,0,3,3,depth_values,12,error),error);
    std::vector<std::uint8_t> depth_pixels;check(depth_texture->read_pixels(depth_pixels,error),error);
    check(depth_pixels.size()==sizeof depth_values&&!std::memcmp(depth_pixels.data(),depth_values,sizeof depth_values),
          "Depth32Float upload/readback uses the depth image aspect and four-byte pixels");
    auto target=context->create_texture(8,8,80,error);check(bool(target),error);
    auto mesh_vertex=shader(R"AIR(
define <{<4 x float>,<4 x float>}> @mesh(<4 x float> %p,<4 x float> %c) {
 %a=insertvalue <{<4 x float>,<4 x float>}> undef,<4 x float> %p,0
 %b=insertvalue <{<4 x float>,<4 x float>}> %a,<4 x float> %c,1
 ret <{<4 x float>,<4 x float>}> %b
}
!air.vertex=!{!0}
!0=!{ptr @mesh,!1,!4}
!1=!{!2,!3}
!2=!{!"air.position"}
!3=!{!"air.vertex_output",!"user(locn0)"}
!4=!{!5,!6}
!5=!{i32 0,!"air.vertex_input",!"air.location_index",i32 0}
!6=!{i32 1,!"air.vertex_input",!"air.location_index",i32 1}
)AIR");
    auto mesh_fragment=shader("define <4 x float> @meshcolor(<4 x float> %c){ret <4 x float> %c}\n!air.fragment=!{!0}\n!0=!{ptr @meshcolor,!1,!2}\n!1=!{}\n!2=!{!3}\n!3=!{i32 0,!\"air.fragment_input\",!\"user(locn0)\"}\n");
    mevk::VertexInput mesh_input;mesh_input.attributes={{0,30,31,0},{1,30,9,16}};mesh_input.buffers={{30,20,1,1}};
    auto mesh_pipeline=context->create_render_pipeline(mesh_vertex,mesh_fragment,80,{},error,&mesh_input,252);check(bool(mesh_pipeline),error);
    struct MeshVertex {float p[4];std::uint32_t color;};static_assert(sizeof(MeshVertex)==20);
    const MeshVertex mesh_vertices[]={{{-1,-1,0.25f,1},0xff0000ffu},{{3,-1,0.25f,1},0xff0000ffu},{{-1,3,0.25f,1},0xff0000ffu},
      {{-1,-1,0.75f,1},0xffff0000u},{{3,-1,0.75f,1},0xffff0000u},{{-1,3,0.75f,1},0xffff0000u}};
    auto mesh_buffer=context->create_buffer(sizeof mesh_vertices+4,error);check(bool(mesh_buffer),error);
    std::memcpy(static_cast<std::uint8_t*>(mesh_buffer->contents())+4,mesh_vertices,sizeof mesh_vertices);
    mevk::RenderBindings mesh_bindings;mesh_bindings.vertex_buffers[30]=mesh_buffer;mesh_bindings.vertex_offsets[30]=4;
    depth_description.width=8;depth_description.height=8;
    auto render_depth=context->create_texture(depth_description,error);check(bool(render_depth),error);
    mevk::DepthAttachment depth_attachment{render_depth,0,0,true,1};mevk::DepthState less{1,true};
    auto depth_commands=context->create_command_buffer(error);check(bool(depth_commands),error);
    check(depth_commands->begin_render(target,true,{0,0,0,1},error,&depth_attachment),error);
    check(depth_commands->draw(mesh_pipeline,mesh_bindings,{0,0,8,8,0,1},{0,0,8,8},3,0,3,1,0,{},0,0,0,error,&less),error);
    check(depth_commands->draw(mesh_pipeline,mesh_bindings,{0,0,8,8,0,1},{0,0,8,8},3,3,3,1,0,{},0,0,0,error,&less),error);
    check(depth_commands->end_render(error)&&depth_commands->commit(error)&&depth_commands->wait_until_completed(error),error);
    std::vector<std::uint8_t> state_pixels;check(target->read_pixels(state_pixels,error),error);
    for(std::size_t i=0;i<state_pixels.size();i+=4)check(state_pixels[i]==0&&state_pixels[i+1]==0&&state_pixels[i+2]==255&&state_pixels[i+3]==255,"vertex layout high binding, normalized color and near-over-far depth test");
    check(render_depth->read_pixels(depth_pixels,error),error);
    for(std::size_t i=0;i<depth_pixels.size();i+=4){float value;std::memcpy(&value,depth_pixels.data()+i,4);check(value==0.25f,"render pass depth writes retain the nearest fragment");}
    depth_attachment.clear=false;mevk::DepthState always{7,false};mevk::RasterState raster;
    auto always_commands=context->create_command_buffer(error);check(bool(always_commands),error);
    check(always_commands->begin_render(target,true,{0,0,0,1},error,&depth_attachment),error);
    check(always_commands->draw(mesh_pipeline,mesh_bindings,{0,0,8,8,0,1},{0,0,8,8},3,3,3,1,0,{},0,0,0,error,&always,&raster),error);
    check(always_commands->end_render(error)&&always_commands->commit(error)&&always_commands->wait_until_completed(error),error);
    check(target->read_pixels(state_pixels,error),error);
    for(std::size_t i=0;i<state_pixels.size();i+=4)check(state_pixels[i]==255&&state_pixels[i+2]==0,"depth compare state changes pipeline variant without writing depth");
    check(render_depth->read_pixels(depth_pixels,error),error);
    for(std::size_t i=0;i<depth_pixels.size();i+=4){float value;std::memcpy(&value,depth_pixels.data()+i,4);check(value==0.25f,"disabled depth writes preserve loaded depth values");}
    for(unsigned cull:{1u,2u}) {
      raster.cull=cull;raster.winding=1;
      auto cull_commands=context->create_command_buffer(error);check(bool(cull_commands),error);
      check(cull_commands->begin_render(target,true,{0,0,0,1},error,&depth_attachment),error);
      check(cull_commands->draw(mesh_pipeline,mesh_bindings,{0,0,8,8,0,1},{0,0,8,8},3,0,3,1,0,{},0,0,0,error,&always,&raster),error);
      check(cull_commands->end_render(error)&&cull_commands->commit(error)&&cull_commands->wait_until_completed(error),error);
      check(target->read_pixels(state_pixels,error),error);
      for(std::size_t i=0;i<state_pixels.size();i+=4)check(state_pixels[i+2]==(cull==1?0:255),"front/back culling respects counterclockwise Metal winding");
    }
    auto missing_vertex=context->create_command_buffer(error);check(bool(missing_vertex),error);
    check(missing_vertex->begin_render(target,false,{},error,&depth_attachment),error);
    check(!missing_vertex->draw(mesh_pipeline,{}, {0,0,8,8,0,1},{0,0,8,8},3,0,3,1,0,{},0,0,0,error,&less)&&!missing_vertex->commit(error),"missing vertex input poisons the render batch");
    auto packed_target=context->create_texture(8,8,90,error);check(bool(packed_target),error);
    auto packed_fragment=shader("define <4 x float> @packed(){ret <4 x float> <float 1.0,float 0.0,float 0.0,float 1.0>}\n!air.fragment=!{!0}\n!0=!{ptr @packed,!1,!1}\n!1=!{}\n");
    auto packed_pipeline=context->create_render_pipeline(vertex,packed_fragment,90,{},error);check(bool(packed_pipeline),error);
    mevk::RenderBindings packed_bindings;
    auto packed_commands=context->create_command_buffer(error);check(bool(packed_commands),error);
    check(packed_commands->begin_render(packed_target,true,{0,0,0,1},error),error);
    check(packed_commands->draw(packed_pipeline,packed_bindings,{0,0,8,8,0,1},{0,0,8,8},3,0,3,1,0,{},0,0,0,error),error);
    check(packed_commands->end_render(error)&&packed_commands->commit(error)&&packed_commands->wait_until_completed(error),error);
    std::vector<std::uint8_t> packed_pixels;check(packed_target->read_pixels(packed_pixels,error),error);
    for(std::size_t i=0;i<packed_pixels.size();i+=4) {
      std::uint32_t pixel;std::memcpy(&pixel,packed_pixels.data()+i,4);
      check(pixel==0xc00003ffu,"RGB10A2 render target packs red in bits 0..9 and alpha in bits 30..31");
    }
    mevk::SamplerDescription sampler_desc;sampler_desc.mip_filter=1;
    auto sampler=context->create_sampler(sampler_desc,error);check(bool(sampler),error);
    struct Case {unsigned type;const char* name;const char* signature;const char* args;const char* metadata;unsigned slice,z,mip;};
    const Case cases[]={
      {2,"2d","<2 x float>,i1,<2 x i32>,i1,float","<2 x float> <float 0.5,float 0.5>,i1 true,<2 x i32> zeroinitializer,i1 true,float 1.0","texture2d<float, sample>",0,0,1},
      {3,"2d_array","<2 x float>,i32,i1,<2 x i32>,i1,float","<2 x float> <float 0.5,float 0.5>,i32 1,i1 true,<2 x i32> zeroinitializer,i1 true,float 0.0","texture2d_array<float, sample>",1,0,0},
      {5,"cube","<3 x float>,i1,float","<3 x float> <float 1.0,float 0.0,float 0.0>,i1 true,float 0.0","texturecube<float, sample>",0,0,0},
      {7,"3d","<3 x float>,i1,<3 x i32>,i1,float","<3 x float> <float 0.5,float 0.5,float 0.75>,i1 true,<3 x i32> zeroinitializer,i1 true,float 0.0","texture3d<float, sample>",0,1,0}
    };
    for(auto test:cases) {
      std::string name=std::string("air.sample_texture_")+test.name+".v4f32";
      auto fragment=shader("declare <4 x float> @"+name+"(ptr,ptr,"+test.signature+")\n"
        "define <4 x float> @f(ptr %t,ptr %s){%c=call <4 x float> @"+name+"(ptr %t,ptr %s,"+test.args+")\nret <4 x float> %c}\n"
        "!air.fragment=!{!0}\n!0=!{ptr @f,!1,!2}\n!1=!{}\n!2=!{!3,!4}\n"
        "!3=!{i32 0,!\"air.texture\",!\"air.location_index\",i32 16,!\"air.arg_type_name\",!\""+test.metadata+"\"}\n"
        "!4=!{i32 1,!\"air.sampler\",!\"air.location_index\",i32 15}\n");
      mevk::TextureDescription description;description.type=test.type;
      if(test.type==2){description.width=2;description.height=2;description.mips=2;}
      if(test.type==3)description.array_length=2;if(test.type==7)description.depth=2;
      auto texture=context->create_texture(description,error);check(bool(texture),error);
      const unsigned char red[]={255,0,0,255};
      check(texture->replace(0,0,test.z,1,1,1,test.mip,test.slice,red,4,4,error),error);
      auto pipeline=context->create_render_pipeline(vertex,fragment,80,{},error);check(bool(pipeline),error);
      if(test.type==2) {
        auto shared_vertex=vertex,shared_fragment=fragment;
        mevk::ShaderBinding literal{UINT32_MAX,128,false};
        literal.sampler=true;literal.literal_sampler=0x807bff0000080a49ull;
        shared_vertex.bindings.push_back(literal);shared_fragment.bindings.push_back(literal);
        pipeline=context->create_render_pipeline(shared_vertex,shared_fragment,80,{},error);
        check(bool(pipeline),"literal sampler shared by vertex and fragment stages: "+error);
      }
      mevk::RenderBindings bindings;bindings.textures[32+16]=texture;bindings.samplers[16+15]=sampler;
      auto commands=context->create_command_buffer(error);check(bool(commands),error);
      check(commands->begin_render(target,true,{0,0,0,1},error),error);
      check(commands->draw(pipeline,bindings,{0,0,8,8,0,1},{0,0,8,8},3,0,3,1,0,{},0,0,0,error),error);
      check(commands->end_render(error)&&commands->commit(error)&&commands->wait_until_completed(error),error);
      std::vector<std::uint8_t> pixels;check(target->read_pixels(pixels,error),error);
      for(std::size_t i=0;i<pixels.size();i+=4)check(pixels[i]==0&&pixels[i+1]==0&&pixels[i+2]==255&&pixels[i+3]==255,std::string("AIR ")+test.name+" sampling pixel");
      if(test.type==2||test.type==5) {
        auto attachment_commands=context->create_command_buffer(error);check(bool(attachment_commands),error);
        check(attachment_commands->begin_render(texture,true,{0,1,0,1},error,nullptr,test.mip,test.slice),error);
        check(attachment_commands->end_render(error)&&attachment_commands->commit(error)&&attachment_commands->wait_until_completed(error),error);
        auto sampled_attachment=context->create_command_buffer(error);check(bool(sampled_attachment),error);
        check(sampled_attachment->begin_render(target,true,{0,0,0,1},error),error);
        check(sampled_attachment->draw(pipeline,bindings,{0,0,8,8,0,1},{0,0,8,8},3,0,3,1,0,{},0,0,0,error),error);
        check(sampled_attachment->end_render(error)&&sampled_attachment->commit(error)&&sampled_attachment->wait_until_completed(error),error);
        check(target->read_pixels(pixels,error),error);
        for(std::size_t i=0;i<pixels.size();i+=4)check(pixels[i]==0&&pixels[i+1]==255&&pixels[i+2]==0&&pixels[i+3]==255,"render pass writes the selected mip or cube face");
      }
      if(test.type==2) {
        mevk::TextureDescription bc_description;bc_description.width=8;bc_description.height=8;bc_description.mips=4;bc_description.pixel_format=130;bc_description.usage=1;
        auto bc=context->create_texture(bc_description,error);check(bool(bc),error);
        const std::uint8_t red_block[]={0x00,0xf8,0xe0,0x07,0,0,0,0};
        std::array<std::uint8_t,32> blocks{};
        for(unsigned i=0;i<4;++i)std::memcpy(blocks.data()+i*8,red_block,8);
        check(bc->replace(0,0,8,8,blocks.data(),16,error),error);
        std::vector<std::uint8_t> compressed_pixels;check(bc->read_pixels(compressed_pixels,error),error);
        check(compressed_pixels.size()==blocks.size()&&!std::memcmp(compressed_pixels.data(),blocks.data(),blocks.size()),"BC1 block upload and raw readback");
        auto bc_upload=context->create_buffer(24,error);check(bool(bc_upload),error);
        std::memcpy(static_cast<std::uint8_t*>(bc_upload->contents())+8,red_block,8);
        auto bc_commands=context->create_command_buffer(error);check(bool(bc_commands),error);
        check(bc_commands->copy_buffer_to_texture(bc_upload,8,16,0,{4,4,1},bc,0,1,{0,0,0},error),error);
        check(bc_commands->copy_buffer_to_texture(bc_upload,8,16,0,{1,1,1},bc,0,3,{0,0,0},error),error);
        check(bc_commands->commit(error)&&bc_commands->wait_until_completed(error),error);
        bindings.textures[48]=bc;
        auto sampled=context->create_command_buffer(error);check(bool(sampled),error);
        check(sampled->begin_render(target,true,{0,0,0,1},error),error);
        check(sampled->draw(pipeline,bindings,{0,0,8,8,0,1},{0,0,8,8},3,0,3,1,0,{},0,0,0,error),error);
        check(sampled->end_render(error)&&sampled->commit(error)&&sampled->wait_until_completed(error),error);
        check(target->read_pixels(pixels,error),error);
        for(std::size_t i=0;i<pixels.size();i+=4)check(pixels[i]==0&&pixels[i+1]==0&&pixels[i+2]==255&&pixels[i+3]==255,"BC1 explicit-LOD GPU sampling decodes the compressed red block");
      }
    }
    auto compute=shader(R"(
declare i32 @air.atomic.global.add.u.i32(ptr,i32,i32,i32)
define void @k(ptr %out,ptr %counter,<3 x i32> %gid) {
 %x=extractelement <3 x i32> %gid,i32 0
 %index=zext i32 %x to i64
 %p=getelementptr i32,ptr %out,i64 %index
 %v=add i32 %x,100
 store i32 %v,ptr %p
 %old=call i32 @air.atomic.global.add.u.i32(ptr %counter,i32 1,i32 0,i32 2)
 ret void
}
!air.kernel=!{!0}
!0=!{ptr @k,!1,!2}
!1=!{}
!2=!{!3,!4,!5}
!3=!{i32 0,!"air.buffer",!"air.location_index",i32 0,!"air.read_write"}
!4=!{i32 1,!"air.buffer",!"air.location_index",i32 1,!"air.read_write"}
!5=!{i32 2,!"air.thread_position_in_grid"}
)");
    auto pipeline=context->create_compute_pipeline(compute,error);check(bool(pipeline),error);
    auto output=context->create_buffer(18*4,error);check(bool(output),error);
    auto counter=context->create_buffer(4,error);check(bool(counter),error);
    std::memset(output->contents(),0xa5,72);std::memset(counter->contents(),0,4);
    mevk::RenderBindings bindings;bindings.buffers[0]=output;bindings.offsets[0]=4;bindings.buffers[1]=counter;
    auto commands=context->create_command_buffer(error);check(bool(commands),error);
    check(commands->dispatch(pipeline,bindings,{2,1,1},{8,1,1},error),error);
    check(commands->commit(error)&&commands->wait_until_completed(error),error);
    auto *words=static_cast<std::uint32_t*>(output->contents());
    check(words[0]==0xa5a5a5a5u&&words[17]==0xa5a5a5a5u,"compute offset and guard words");
    for(unsigned i=0;i<16;++i)check(words[i+1]==i+100,"compute specialization and global invocation IDs");
    check(*static_cast<std::uint32_t*>(counter->contents())==16,"compute atomic add");
    auto write3d=shader(R"(
declare void @air.write_texture_3d.v4f32(ptr,<3 x i32>,<4 x float>,i32)
define void @volume(ptr %out,<3 x i32> %gid) {
 call void @air.write_texture_3d.v4f32(ptr %out,<3 x i32> %gid,<4 x float> <float 1.0,float 0.0,float 0.0,float 1.0>,i32 0)
 ret void
}
!air.kernel=!{!0}
!0=!{ptr @volume,!1,!2}
!1=!{}
!2=!{!3,!4}
!3=!{i32 0,!"air.texture",!"air.location_index",i32 0,!"air.write",!"air.arg_type_name",!"texture3d<float, write>"}
!4=!{i32 1,!"air.thread_position_in_grid"}
)");
    auto volume_pipeline=context->create_compute_pipeline(write3d,error);check(bool(volume_pipeline),error);
    mevk::TextureDescription volume_desc;volume_desc.width=4;volume_desc.height=4;volume_desc.depth=2;volume_desc.type=7;volume_desc.usage=3;
    auto volume=context->create_texture(volume_desc,error);check(bool(volume)&&volume->supports_shader_write(),error);
    mevk::RenderBindings volume_bindings;volume_bindings.textures[0]=volume;
    auto volume_commands=context->create_command_buffer(error);check(bool(volume_commands),error);
    check(volume_commands->dispatch(volume_pipeline,volume_bindings,{2,2,2},{2,2,1},error),error);
    check(volume_commands->commit(error)&&volume_commands->wait_until_completed(error),error);
    std::vector<std::uint8_t> voxels;check(volume->read_pixels(voxels,error)&&voxels.size()==4*4*2*4,error);
    for(std::size_t i=0;i<voxels.size();i+=4)check(voxels[i]==255&&voxels[i+1]==0&&voxels[i+2]==0&&voxels[i+3]==255,"compute write/readback of every 3D voxel");
    auto upload=context->create_buffer(80,error);check(bool(upload),error);
    std::memset(upload->contents(),0xa5,80);
    for(unsigned z=0;z<2;++z)for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x) {
      auto* p=static_cast<std::uint8_t*>(upload->contents())+4+z*48+y*16+x*4;
      p[0]=static_cast<std::uint8_t>(32+x);p[1]=static_cast<std::uint8_t>(64+y);p[2]=static_cast<std::uint8_t>(96+z);p[3]=255;
    }
    volume_desc.depth=3;
    auto copied_volume=context->create_texture(volume_desc,error);check(bool(copied_volume),error);
    std::vector<std::uint8_t> empty(4*4*3*4,0);
    check(copied_volume->replace(0,0,0,4,4,3,0,0,empty.data(),16,64,error),error);
    auto copy_commands=context->create_command_buffer(error);check(bool(copy_commands),error);
    check(copy_commands->copy_buffer_to_texture(upload,4,16,48,{2,2,2},copied_volume,0,0,{1,1,1},error),error);
    upload.reset();
    check(copy_commands->commit(error)&&copy_commands->wait_until_completed(error),error);
    check(copied_volume->read_pixels(voxels,error),error);
    for(unsigned z=0;z<3;++z)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
      auto* p=voxels.data()+((z*4+y)*4+x)*4;bool inside=x>=1&&x<3&&y>=1&&y<3&&z>=1;
      check(p[0]==(inside?31+x:0)&&p[1]==(inside?63+y:0)&&p[2]==(inside?95+z:0)&&p[3]==(inside?255:0),
            "buffer-to-3D-texture copy preserves row/image padding and destination origin");
    }
    auto short_source=context->create_buffer(4,error);check(bool(short_source),error);
    auto invalid_copy=context->create_command_buffer(error);check(bool(invalid_copy),error);
    check(!invalid_copy->copy_buffer_to_texture(short_source,0,16,48,{2,2,2},copied_volume,0,0,{1,1,1},error)&&!invalid_copy->commit(error),
          "buffer-to-texture source bounds poison the batch");
    auto invalid=context->create_command_buffer(error);check(bool(invalid),error);
    check(!invalid->dispatch(pipeline,bindings,{1,1,1},{0,1,1},error)&&!invalid->commit(error),"invalid compute dimensions poison batch");
    std::puts("MEVK AIR texture dimensions, high slots, explicit LOD and compute GPU tests passed");
}
