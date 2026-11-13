#include "mevk/shader.h"
#include "cache.h"
#include <llvm/ADT/MapVector.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <charconv>
#include <map>
#include <shaderc/shaderc.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace mevk {
namespace {
using namespace llvm;
std::string mdtext(const Metadata *m) {
  auto *s = dyn_cast_or_null<MDString>(m);
  return s ? s->getString().str() : "";
}
bool tag(const MDNode *n, const char *s) {
  for (const auto &x : n->operands())
    if (mdtext(x.get()) == s)
      return true;
  return false;
}
std::string text_after(const MDNode *n, const char *s) {
  for(unsigned i=0;i+1<n->getNumOperands();++i)
    if(mdtext(n->getOperand(i))==s)return mdtext(n->getOperand(i+1));
  return {};
}
std::uint32_t number(const Metadata *m) {
  auto *c = dyn_cast_or_null<ConstantAsMetadata>(m);
  auto *i = c ? dyn_cast<ConstantInt>(c->getValue()) : nullptr;
  if (!i || i->getValue().getActiveBits() > 32)
    throw std::runtime_error("AIR metadata requires a 32-bit unsigned integer");
  return static_cast<std::uint32_t>(i->getZExtValue());
}
std::uint32_t location(const MDNode *n) {
  for (unsigned i = 0; i + 1 < n->getNumOperands(); ++i)
    if (mdtext(n->getOperand(i)) == "air.location_index")
      return number(n->getOperand(i + 1));
  throw std::runtime_error("AIR resource has no location_index");
}
unsigned varying_location(const MDNode *n, unsigned fallback) {
  for (const auto &operand : n->operands()) {
    auto name = mdtext(operand.get());
    if (!name.starts_with("user(locn")) continue;
    if (name.size() <= 10)
      throw std::runtime_error("AIR varying location unsupported");
    unsigned result = 0;
    const char *begin = name.data() + 9;
    const char *end = name.data() + name.size() - 1;
    auto parsed = std::from_chars(begin, end, result);
    if (name.back() != ')' || parsed.ec != std::errc{} || parsed.ptr != end ||
        result >= 32)
      throw std::runtime_error("AIR varying location unsupported");
    return result;
  }
  return fallback;
}

struct Emitter {
  Module &module;
  ShaderFunction &output;
  Function *entry = nullptr;
  const MDNode *outputs = nullptr;
  bool fragment_aggregate = false;
  std::vector<std::string> output_targets;
  std::vector<std::string> arguments;
  std::string interfaces, buffers, textures, structures;
  std::map<Type *, std::string> types;
  std::unordered_map<const Value *, std::string> values;
  std::unordered_map<const Function *, std::string> functions;
  std::vector<ShaderBinding> buffer_bindings, texture_bindings;
  std::vector<ShaderBinding> sampler_bindings;
  std::map<std::string,std::string> texture_helpers;
  std::string sampling;
  struct Memory { unsigned id; std::string name; bool writable; };
  std::unordered_map<const Value*,Memory> memories;
  std::string memory_declarations;
  unsigned serial = 0;
  Emitter(Module &m, ShaderFunction &o) : module(m), output(o) {}

  std::string type(Type *t) {
    if (t->isVoidTy())
      return "void";
    if (t->isPointerTy())
      return "uvec2";
    if (t->isIntegerTy(1))
      return "bool";
    if (t->isIntegerTy()) {
      if (t->getIntegerBitWidth() > 32 && !t->isIntegerTy(64))
        throw std::runtime_error("AIR integer width unsupported");
      return "uint";
    }
    if (t->isFloatTy() || t->isHalfTy())
      return "float";
    if (auto *v = dyn_cast<FixedVectorType>(t)) {
      auto count = v->getNumElements();
      if (count < 2 || count > 4)
        throw std::runtime_error("AIR vector width unsupported");
      if (!v->getElementType()->isFloatTy() &&
          !v->getElementType()->isHalfTy() &&
          !(v->getElementType()->isIntegerTy() &&
            v->getElementType()->getIntegerBitWidth() <= 32))
        throw std::runtime_error("AIR vector element type unsupported");
      return (v->getElementType()->isFloatingPointTy() ? "vec"
              : v->getElementType()->isIntegerTy(1)    ? "bvec"
                                                       : "uvec") +
             std::to_string(count);
    }
    if (auto *s = dyn_cast<StructType>(t)) {
      if (auto it = types.find(t); it != types.end())
        return it->second;
      std::string fields;
      for (unsigned i = 0; i < s->getNumElements(); ++i) {
        if (s->getElementType(i)->isIntegerTy(64))
          throw std::runtime_error("AIR i64 aggregate field unsupported");
        fields += type(s->getElementType(i)) + " f" + std::to_string(i) + ";";
      }
      std::string name = "T" + std::to_string(types.size());
      types[t] = name;
      structures += "struct " + name + "{" + fields + "};\n";
      return name;
    }
    throw std::runtime_error("AIR value type unsupported");
  }
  std::string zero(Type *t) {
    if (auto *s = dyn_cast<StructType>(t)) {
      std::string r = type(t) + "(";
      for (unsigned i = 0; i < s->getNumElements(); ++i)
        r += (i ? "," : "") + zero(s->getElementType(i));
      return r + ")";
    }
    if (t->isPointerTy())
      return "uvec2(0u)";
    if (t->isIntegerTy(1))
      return "false";
    return type(t) + "(0)";
  }
  std::string value(const Value *v) {
    if (auto it = values.find(v); it != values.end())
      return it->second;
    if(auto it=memories.find(v);it!=memories.end())return "uvec2("+std::to_string(it->second.id)+"u,0u)";
    if(auto *c=dyn_cast<ConstantExpr>(v)) {
      if(c->getOpcode()==Instruction::BitCast||c->getOpcode()==Instruction::AddrSpaceCast)return value(c->getOperand(0));
      if(c->getOpcode()==Instruction::GetElementPtr) {
        APInt offset(64,0);
        if(!cast<GEPOperator>(c)->accumulateConstantOffset(module.getDataLayout(),offset))
          throw std::runtime_error("AIR constant GEP unsupported");
        auto p=value(c->getOperand(0));
        return "uvec2(("+p+").x,("+p+").y+"+std::to_string(static_cast<std::uint32_t>(offset.getZExtValue()))+"u)";
      }
    }
    if (isa<UndefValue>(v) || isa<PoisonValue>(v) ||
        isa<ConstantAggregateZero>(v) || isa<ConstantPointerNull>(v))
      return zero(v->getType());
    // LLVM 23 also uses ConstantFP/ConstantInt for vector splats. Preserve
    // their vector type before handling scalar constants.
    if (auto *c = dyn_cast<Constant>(v))
      if (auto *s = dyn_cast<StructType>(v->getType())) {
        std::string r=type(s)+"(";
        for(unsigned i=0;i<s->getNumElements();++i)r+=(i?",":"")+value(c->getAggregateElement(i));
        return r+")";
      }
    if (auto *c = dyn_cast<Constant>(v))
      if (auto *vec = dyn_cast<FixedVectorType>(v->getType())) {
        std::string r = type(vec) + "(";
        for (unsigned i = 0; i < vec->getNumElements(); ++i)
          r += (i ? "," : "") + value(c->getAggregateElement(i));
        return r + ")";
      }
    if (auto *i = dyn_cast<ConstantInt>(v)) {
      if (i->getBitWidth() > 32 && i->getValue().getActiveBits() > 32)
        throw std::runtime_error("AIR wide integer constant unsupported");
      if (i->getBitWidth() == 1)
        return i->isZero() ? "false" : "true";
      return std::to_string(static_cast<std::uint32_t>(i->getZExtValue())) +
             "u";
    }
    if (auto *f = dyn_cast<ConstantFP>(v)) {
      APFloat n = f->getValueAPF();
      bool loses = false;
      n.convert(APFloat::IEEEsingle(), APFloat::rmNearestTiesToEven, &loses);
      return "uintBitsToFloat(" +
             std::to_string(n.bitcastToAPInt().getZExtValue()) + "u)";
    }
    if (auto *g = dyn_cast<GlobalVariable>(v);
        g && g->getName().starts_with("__air_sampler_state")) {
      auto *encoding=g->hasInitializer()?dyn_cast<ConstantInt>(g->getInitializer()):nullptr;
      if(!encoding||encoding->getZExtValue()!=0x807bff0000080a49ull)
        throw std::runtime_error("AIR literal sampler encoding unsupported");
      if(sampler_bindings.empty()||sampler_bindings.back().metal_index!=UINT32_MAX) {
        ShaderBinding b{UINT32_MAX,128,false};b.sampler=true;b.literal_sampler=encoding->getZExtValue();
        sampler_bindings.push_back(b);output.bindings.push_back(b);
        textures+="layout(set=0,binding=128) uniform sampler samp128;\n";
      }
      return "uvec2(128u,0u)";
    }
    throw std::runtime_error("AIR operand unsupported");
  }
  std::string signed_value(const Value *v) {
    auto *t = dyn_cast<IntegerType>(v->getType()->getScalarType());
    if (!t)
      throw std::runtime_error("AIR signed vector operation unsupported");
    auto bits = t->getBitWidth();
    if (bits > 32)
      throw std::runtime_error("AIR signed 64-bit arithmetic unsupported");
    auto cast_type = isa<FixedVectorType>(v->getType())
                         ? "ivec" + std::to_string(cast<FixedVectorType>(v->getType())->getNumElements()) : "int";
    if(bits==1) {
      if(isa<FixedVectorType>(v->getType()))return "mix("+cast_type+"(0),"+cast_type+"(-1),"+value(v)+")";
      return "("+value(v)+"?-1:0)";
    }
    if (bits == 32)
      return cast_type + "(" + value(v) + ")";
    return "(" + cast_type + "(" + value(v) + " << " + std::to_string(32 - bits) + ") >> " +
           std::to_string(32 - bits) + ")";
  }
  std::string compare(Type *t, const std::string &a, const std::string &b,
                      const std::string &op, int nan_mode = 0) {
    auto scalar = [&](std::string x, std::string y) {
      auto cmp = "("+x+op+y+")";
      if (nan_mode) {
        auto nan="(isnan("+x+")||isnan("+y+"))";
        cmp="("+(nan_mode>0?nan+"||":"!"+nan+"&&")+cmp+")";
      }
      return cmp;
    };
    if(auto *v=dyn_cast<FixedVectorType>(t)) {
      std::string result=type(t)+"(";
      for(unsigned n=0;n<v->getNumElements();++n)
        result+=(n?",":"")+scalar("("+a+")["+std::to_string(n)+"]","("+b+")["+std::to_string(n)+"]");
      return result+")";
    }
    return scalar(a,b);
  }
  std::string load(Type *t, const std::string &pointer) {
    if (auto *v = dyn_cast<FixedVectorType>(t)) {
      std::string result = type(t) + "(";
      auto stride = module.getDataLayout()
                        .getTypeStoreSize(v->getElementType())
                        .getFixedValue();
      for (unsigned i = 0; i < v->getNumElements(); ++i)
        result += (i ? "," : "") +
                  load(v->getElementType(),
                       "uvec2((" + pointer + ").x,(" + pointer + ").y+" +
                           std::to_string(i * stride) + "u)");
      return result + ")";
    }
    auto raw = "word(" + pointer + ")";
    if (t->isFloatTy())
      return "uintBitsToFloat(" + raw + ")";
    if (t->isHalfTy())
      return "unpackHalf2x16(" + raw + ").x";
    if (t->isIntegerTy(1))
      return "((" + raw + "&1u)!=0u)";
    if (t->isIntegerTy(8))
      return "(" + raw + "&255u)";
    if (t->isIntegerTy(16))
      return "(" + raw + "&65535u)";
    if (t->isIntegerTy(32))
      return raw;
    throw std::runtime_error("AIR load type unsupported");
  }
  void constant_bytes(Constant *c, std::vector<std::uint8_t> &bytes,std::size_t offset) {
    if(c->isNullValue()||isa<UndefValue>(c)||isa<PoisonValue>(c))return;
    auto *t=c->getType();
    if(t->isIntegerTy()||t->isFloatingPointTy()) {
      auto bits=isa<ConstantInt>(c)?cast<ConstantInt>(c)->getValue():cast<ConstantFP>(c)->getValueAPF().bitcastToAPInt();
      if(bits.getBitWidth()>64)throw std::runtime_error("AIR constant width unsupported");
      auto n=module.getDataLayout().getTypeStoreSize(t).getFixedValue();
      for(unsigned i=0;i<n;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(bits.getZExtValue()>>(8*i));
      return;
    }
    if(auto *s=dyn_cast<StructType>(t)) {
      auto *layout=module.getDataLayout().getStructLayout(s);
      for(unsigned i=0;i<s->getNumElements();++i)constant_bytes(c->getAggregateElement(i),bytes,offset+layout->getElementOffset(i));
      return;
    }
    auto *a=dyn_cast<ArrayType>(t);auto *v=dyn_cast<FixedVectorType>(t);
    if(!a&&!v)throw std::runtime_error("AIR constant aggregate unsupported");
    auto count=a?a->getNumElements():v->getNumElements();
    auto element=a?a->getElementType():v->getElementType();
    auto stride=module.getDataLayout().getTypeAllocSize(element).getFixedValue();
    for(unsigned i=0;i<count;++i)constant_bytes(c->getAggregateElement(i),bytes,offset+i*stride);
  }
  void memory(const Value *v,Type *t,Constant *initializer=nullptr,unsigned count=1) {
    auto size=module.getDataLayout().getTypeAllocSize(t).getFixedValue()*count;
    if(!size||size>1024*1024)throw std::runtime_error("AIR local/constant memory size unsupported");
    auto id=256u+static_cast<unsigned>(memories.size());auto name="memory"+std::to_string(id);
    memories[v]={id,name,initializer==nullptr};
    auto words=(size+3)/4;
    memory_declarations+=(initializer?"const ":"")+std::string("uint ")+name+"["+std::to_string(words)+"]";
    if(initializer) {
      std::vector<std::uint8_t> bytes(words*4,0);constant_bytes(initializer,bytes,0);
      memory_declarations+="=uint["+std::to_string(words)+"](";
      for(unsigned i=0;i<words;++i) {
        std::uint32_t word=0;for(unsigned j=0;j<4;++j)word|=std::uint32_t{bytes[i*4+j]}<<(j*8);
        memory_declarations+=(i?",":"")+std::to_string(word)+"u";
      }
      memory_declarations+=")";
    }
    memory_declarations+=";\n";
  }
  std::string store(Type *t,const std::string &pointer,const std::string &x) {
    if(auto *v=dyn_cast<FixedVectorType>(t)) {
      std::string code;auto stride=module.getDataLayout().getTypeStoreSize(v->getElementType()).getFixedValue();
      for(unsigned i=0;i<v->getNumElements();++i)code+=store(v->getElementType(),"uvec2(("+pointer+").x,("+pointer+").y+"+std::to_string(stride*i)+"u)","("+x+")["+std::to_string(i)+"]");
      return code;
    }
    std::string raw=t->isFloatTy()?"floatBitsToUint("+x+")":t->isHalfTy()?"packHalf2x16(vec2("+x+",0.0))":"uint("+x+")";
    auto bytes=module.getDataLayout().getTypeStoreSize(t).getFixedValue();
    if(bytes>4)throw std::runtime_error("AIR store type unsupported");
    return "storeword("+pointer+","+raw+","+std::to_string(bytes)+"u);";
  }
  void metadata() {
    NamedMDNode *node = module.getNamedMetadata("air.vertex");
    output.stage = ShaderStage::vertex;
    if (!node) {
      node = module.getNamedMetadata("air.fragment");
      output.stage = ShaderStage::fragment;
    }
    if(!node){node=module.getNamedMetadata("air.kernel");output.stage=ShaderStage::compute;}
    if (!node || node->getNumOperands() != 1)
      throw std::runtime_error(
          "AIR requires one vertex, fragment or compute entry point");
    auto *description = node->getOperand(0);
    if (description->getNumOperands() != 3)
      throw std::runtime_error("AIR entry metadata shape unsupported");
    auto *function =
        dyn_cast_or_null<ValueAsMetadata>(description->getOperand(0));
    entry = function ? dyn_cast<Function>(function->getValue()) : nullptr;
    outputs = dyn_cast_or_null<MDNode>(description->getOperand(1));
    auto *inputs = dyn_cast_or_null<MDNode>(description->getOperand(2));
    if (!entry || entry->isDeclaration() || !outputs || !inputs)
      throw std::runtime_error("AIR entry metadata is incomplete");
    output.name = entry->getName().str();
    unsigned varying = 0;
    if (output.stage == ShaderStage::vertex) {
      auto *returned = dyn_cast<StructType>(entry->getReturnType());
      if (!returned || returned->getNumElements() != outputs->getNumOperands())
        throw std::runtime_error("AIR vertex output shape unsupported");
      bool position = false;
      for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
        auto *o = dyn_cast<MDNode>(outputs->getOperand(i));
        if (!o)
          throw std::runtime_error("AIR output metadata invalid");
        if (tag(o, "air.position")) {
          if (position || returned->getElementType(i) !=
                              FixedVectorType::get(
                                  Type::getFloatTy(module.getContext()), 4))
            throw std::runtime_error("AIR vertex position shape unsupported");
          position = true;
          continue;
        }
        if (tag(o, "air.point_size")) continue;
        if (!tag(o, "air.vertex_output"))
          throw std::runtime_error("AIR vertex output unsupported");
        auto *t = cast<StructType>(entry->getReturnType())->getElementType(i);
        bool flat = t->getScalarType()->isIntegerTy();
        interfaces += "layout(location=" + std::to_string(varying_location(o, varying)) + ") " +
                      (flat ? "flat " : "") + "out " + type(t) + " varying" +
                      std::to_string(varying) + ";\n";
        ++varying;
      }
      if (!position)
        throw std::runtime_error("AIR vertex position missing");
    } else if(output.stage==ShaderStage::fragment) {
      auto *s = dyn_cast<StructType>(entry->getReturnType());
      fragment_aggregate = s != nullptr;
      if (s && s->getNumElements() != outputs->getNumOperands())
        throw std::runtime_error("AIR fragment output shape unsupported");
      unsigned count = s ? s->getNumElements() : entry->getReturnType()->isVoidTy() ? 0u : 1u;
      for (unsigned i=0;i<count;++i) {
        Type *t = s ? s->getElementType(i) : entry->getReturnType();
        auto *o = outputs->getNumOperands()>i ? dyn_cast<MDNode>(outputs->getOperand(i)) : nullptr;
        if (o && tag(o,"air.depth")) {
          if (!t->isFloatTy()) throw std::runtime_error("AIR fragment depth shape unsupported");
          output_targets.push_back("gl_FragDepth");
          continue;
        }
        unsigned loc=0;
        if (o) {
          if(o->getNumOperands()<2||mdtext(o->getOperand(0))!="air.render_target")
            throw std::runtime_error("AIR fragment output metadata unsupported");
          loc=number(o->getOperand(1));
        }
        if(loc>=8)throw std::runtime_error("AIR fragment render target unsupported");
        auto name="color"+std::to_string(i);
        interfaces += "layout(location="+std::to_string(loc)+") out "+type(t)+" "+name+";\n";
        output_targets.push_back(name);
      }
    } else {
      if(!entry->getReturnType()->isVoidTy()||outputs->getNumOperands())throw std::runtime_error("AIR compute output unsupported");
      interfaces+="layout(local_size_x_id=0,local_size_y_id=1,local_size_z_id=2) in;\n";
    }
    varying = 0;
    arguments.resize(entry->arg_size());
    for (const auto &input : inputs->operands()) {
      auto *n = dyn_cast_or_null<MDNode>(input.get());
      if (!n || n->getNumOperands() < 2)
        throw std::runtime_error("AIR input metadata invalid");
      auto index = number(n->getOperand(0));
      if (index >= arguments.size())
        throw std::runtime_error(
            "AIR input index is outside function arguments");
      Type *t = entry->getArg(index)->getType();
      if (tag(n, "air.buffer") || tag(n, "air.texture")) {
        auto metal = location(n);
        bool texture = tag(n, "air.texture");
        if (metal >= (texture?32u:16u))
          throw std::runtime_error(
              "AIR resource index exceeds initial 16-slot stage limit");
        auto binding = metal +
                       (output.stage == ShaderStage::fragment ? (texture?32u:16u) : 0u) +
                       (texture ? 32u : 0u);
        ShaderBinding resource{metal, binding, texture};
        if (texture) {
          auto name=text_after(n,"air.arg_type_name");
          resource.texture_type=name.starts_with("texturecube")?5u:name.starts_with("texture3d")?7u:
            name.starts_with("texture2d_array")?3u:name.starts_with("texture2d_ms")?4u:2u;
          resource.sample_type=name.find("<int,")!=std::string::npos?1u:name.find("<uint,")!=std::string::npos?2u:0u;
          resource.writable=tag(n,"air.write");
          if(resource.writable&&output.stage!=ShaderStage::compute)throw std::runtime_error("AIR writable graphics texture unsupported");
          texture_bindings.push_back(resource);
          textures += "layout(set=0,binding=" + std::to_string(binding) +
                      ") uniform "+(resource.writable?"writeonly ":"")+texture_type(resource,false)+" tex" + std::to_string(binding) +
                      ";\n";
          arguments[index] = "uvec2(" + std::to_string(binding) + "u,0u)";
        } else {
          resource.writable=tag(n,"air.read_write")||tag(n,"air.write");
          buffer_bindings.push_back(resource);
          buffers += "layout(std430,set=0,binding=" + std::to_string(binding) +
                     ") "+(resource.writable?"":"readonly ")+"buffer B" + std::to_string(binding) +
                     "{uint words[];} b" + std::to_string(binding) + ";\n";
          arguments[index] = "uvec2(" + std::to_string(binding) +
                             "u,push.offsets[" + std::to_string(binding) + "])";
        }
        output.bindings.push_back(resource);
      } else if (tag(n, "air.vertex_id"))
        arguments[index] = "uint(gl_VertexIndex)";
      else if (tag(n, "air.instance_id"))
        arguments[index] = "uint(gl_InstanceIndex)";
      else if (tag(n, "air.position"))
        arguments[index] = "gl_FragCoord";
      else if (tag(n, "air.front_facing"))
        arguments[index] = t->isIntegerTy(1) ? "gl_FrontFacing" : "uint(gl_FrontFacing)";
      else if(tag(n,"air.thread_position_in_grid"))arguments[index]="gl_GlobalInvocationID";
      else if(tag(n,"air.thread_position_in_threadgroup"))arguments[index]="gl_LocalInvocationID";
      else if(tag(n,"air.threadgroup_position_in_grid"))arguments[index]="gl_WorkGroupID";
      else if(tag(n,"air.thread_index_in_threadgroup"))arguments[index]="gl_LocalInvocationIndex";
      else if(tag(n,"air.threads_per_threadgroup"))arguments[index]="gl_WorkGroupSize";
      else if (tag(n, "air.vertex_input")) {
        auto name = "attribute"+std::to_string(index);
        interfaces += "layout(location="+std::to_string(location(n))+") in "+type(t)+" "+name+";\n";
        arguments[index] = name;
      } else if (tag(n, "air.sampler")) {
        auto metal=location(n);
        if(metal>=16)throw std::runtime_error("AIR sampler index unsupported");
        auto binding=96u+metal+(output.stage==ShaderStage::fragment?16u:0u);
        ShaderBinding resource{metal,binding,false};resource.sampler=true;
        sampler_bindings.push_back(resource);output.bindings.push_back(resource);
        textures+="layout(set=0,binding="+std::to_string(binding)+") uniform sampler samp"+std::to_string(binding)+";\n";
        arguments[index]="uvec2("+std::to_string(binding)+"u,0u)";
      }
      else if (tag(n, "air.fragment_input")) {
        bool flat = t->getScalarType()->isIntegerTy() || tag(n,"air.flat");
        interfaces += "layout(location=" + std::to_string(varying_location(n, varying)) + ") " +
                      (flat ? "flat " : "") + "in " + type(t) + " varying" +
                      std::to_string(varying) + ";\n";
        arguments[index] = "varying" + std::to_string(varying++);
      } else
        throw std::runtime_error("AIR shader input unsupported: argument "+std::to_string(index)+" "+mdtext(n->getOperand(1)));
    }
    for (const auto &a : arguments)
      if (a.empty())
        throw std::runtime_error("AIR argument metadata missing");
  }
  std::string texture_type(const ShaderBinding &b,bool combined) {
    auto dim=b.texture_type==5?"Cube":b.texture_type==7?"3D":b.texture_type==3?"2DArray":b.texture_type==4?"2DMS":"2D";
    return std::string(b.sample_type==1?"i":b.sample_type==2?"u":"")+(b.writable?"image":combined?"sampler":"texture")+dim;
  }
  std::string texture_call(CallInst &call,const std::string &name,const std::vector<std::string> &args) {
    if(name.starts_with("air.write_")) {
      auto *level=dyn_cast<ConstantInt>(call.getArgOperand(static_cast<unsigned>(args.size()-1)));
      if(!level||!level->isZero())throw std::runtime_error("AIR texture write mip unsupported");
    }
    if(name.starts_with("air.gather_")) {
      auto *normalized=args.size()==6?dyn_cast<ConstantInt>(call.getArgOperand(3)):nullptr;
      auto *offset=args.size()==6?dyn_cast<Constant>(call.getArgOperand(4)):nullptr;
      if(!normalized||!normalized->isOne()||!offset||!offset->isNullValue())throw std::runtime_error("AIR texture gather mode unsupported");
    }
    if(name.starts_with("air.sample_")&&name.find("_grad")==std::string::npos&&name.find("_cube")==std::string::npos) {
      unsigned normalized=name.find("_array")!=std::string::npos?4u:3u;
      auto *c=dyn_cast<ConstantInt>(call.getArgOperand(normalized));
      if(!c||!c->isOne())throw std::runtime_error("AIR unnormalized sampling unsupported");
      auto *offset=dyn_cast<Constant>(call.getArgOperand(normalized+1));
      if(!offset||!offset->isNullValue())throw std::runtime_error("AIR texture sample offset unsupported");
      if(args.size()==9)for(unsigned i : {7u,8u}) {
        auto *c=dyn_cast<Constant>(call.getArgOperand(i));
        if(!c||!c->isNullValue())throw std::runtime_error("AIR legacy sampling clamp/offset unsupported");
      }
    }
    auto found=texture_helpers.find(name);
    if(found==texture_helpers.end()) {
      std::string fn="texturefn"+std::to_string(texture_helpers.size());
      texture_helpers[name]=fn;
      std::string body=type(call.getType())+" "+fn+"(";
      for(unsigned i=0;i<args.size();++i)body+=(i?",":"")+type(call.getArgOperand(i)->getType())+" p"+std::to_string(i);
      body+="){\n";
      bool sample=name.starts_with("air.sample_")||name.starts_with("air.gather_");
      bool write=name.starts_with("air.write_");
      auto kind=name.find("_cube")!=std::string::npos?5u:name.find("_3d")!=std::string::npos?7u:
        name.find("_2d_array")!=std::string::npos?3u:name.find("_2d_ms")!=std::string::npos?4u:2u;
      auto sample_type=name.find(".s.")!=std::string::npos?1u:name.find(".u.")!=std::string::npos?2u:0u;
      for(auto t:texture_bindings) {
        if(t.texture_type!=kind||t.sample_type!=sample_type)continue;
        if(t.writable!=write)continue;
        auto tex="tex"+std::to_string(t.binding);
        body+="if(p0.x=="+std::to_string(t.binding)+"u){";
        if(write) {
          auto coord=kind==3?"ivec3(p1,int(p2))":std::string(kind==7?"ivec3":"ivec2")+"(p1)";
          auto target_type=t.sample_type==1?"ivec4":t.sample_type==2?"uvec4":"vec4";
          body+="imageStore("+tex+","+coord+","+target_type+"(p"+std::to_string(kind==3?3u:2u)+"));return;";
        } else if(sample) {
          for(auto s:sampler_bindings) {
            auto combined=texture_type(t,true)+"("+tex+",samp"+std::to_string(s.binding)+")";
            std::string implicit=output.stage==ShaderStage::fragment?"texture":"textureLod";
            std::string expr;
            if(name.starts_with("air.gather_")) {
              // GLSL requires a constant component selector.
              expr="(p5==0u?textureGather("+combined+",p2,0):p5==1u?textureGather("+combined+",p2,1):p5==2u?textureGather("+combined+",p2,2):textureGather("+combined+",p2,3))";
            } else if(name.find("_grad")!=std::string::npos) {
              if(kind==2)expr="textureGrad("+combined+",p2,p3,p4)";
              else expr="textureGrad("+combined+",vec3(p2,float(p3)),p4,p5)";
            } else if(kind==5) {
              expr="(p3?textureLod("+combined+",p2,p4):"+implicit+"("+combined+",p2,p4))";
            } else if(kind==3)expr="(p6?textureLod("+combined+",vec3(p2,float(p3)),p7):"+implicit+"("+combined+",vec3(p2,float(p3)),p7))";
            else expr="(p5?textureLod("+combined+",p2,p6):"+implicit+"("+combined+",p2,p6))";
            if(name.ends_with("v4f16"))expr="half4("+expr+")";
            if(isa<StructType>(call.getType()))expr=type(call.getType())+"("+expr+",0u)";
            body+="if(p1.x=="+std::to_string(s.binding)+"u)return "+expr+";";
          }
        } else {
          std::string expr;
          if(name.starts_with("air.get_")) {
            auto size="textureSize("+tex+",int(p1))";
            expr="uint("+size+(name.starts_with("air.get_width")?".x":name.starts_with("air.get_height")?".y":".z")+")";
          } else {
            auto coordinate=kind==3?"ivec3(p1,int(p2))":std::string(kind==7?"ivec3":"ivec2")+"(p1)";
            expr="texelFetch("+tex+","+coordinate+",int(p"+std::to_string(kind==3?3u:2u)+"))";
            if(t.sample_type)expr=type(call.getType())+"("+expr+")";
          }
          body+="return "+expr+";";
        }
        body+="}\n";
      }
      body+="return"+(call.getType()->isVoidTy()?"":" "+zero(call.getType()))+";}\n";
      sampling+=body;
      found=texture_helpers.find(name);
    }
    std::string result=found->second+"(";
    for(unsigned i=0;i<args.size();++i)result+=(i?",":"")+args[i];
    return result+")";
  }
  std::string call(CallInst &call) {
    auto *f = call.getCalledFunction();
    if (!f)
      throw std::runtime_error("AIR indirect function call unsupported");
    std::string name = f->getName().str();
    std::vector<std::string> args;
    for (const auto &a : call.args())
      args.push_back(value(a));
    auto join = [&](const std::string &fn, unsigned count) {
      std::string s = fn + "(";
      for (unsigned i = 0; i < count; ++i)
        s += (i ? "," : "") + args.at(i);
      return s + ")";
    };
    if (!f->isDeclaration())
      return join(functions.at(f), static_cast<unsigned>(args.size()));
    const std::pair<const char*,const char*> unary[] = {
      {"fast_floor","floor"},{"fast_ceil","ceil"},{"fast_exp","exp"},
      {"fast_log2","log2"},{"fast_fabs","abs"},{"fast_fract","fract"},
      {"fast_sin","sin"},{"fast_cos","cos"},{"fast_atan","atan"},
      {"fast_round","round"},{"fast_rint","roundEven"},{"sign","sign"},
      {"dfdx","dFdx"},{"dfdy","dFdy"},{"fwidth","fwidth"},{"any","any"},{"all","all"}};
    for (const auto &[air,glsl] : unary)
      if(name.starts_with(std::string("air.")+air+"."))return join(glsl,1);
    if(name.starts_with("air.fast_pow."))return join("pow",2);
    if(name.starts_with("air.fma."))return join("fma",3);
    if(name=="air.discard_fragment")return "discard";
    if(name=="air.wg.barrier")return "memoryBarrierShared();barrier()";
    if(name=="air.atomic.global.add.u.i32") {
      if(args.size()!=4)throw std::runtime_error("AIR atomic signature unsupported");
      auto *order=dyn_cast<ConstantInt>(call.getArgOperand(2));
      auto *scope=dyn_cast<ConstantInt>(call.getArgOperand(3));
      if(!order||!order->isZero()||!scope||scope->getZExtValue()!=2)throw std::runtime_error("AIR atomic ordering unsupported");
      return "atomicadd("+args[0]+","+args[1]+")";
    }
    if(name.starts_with("llvm.memset.")) {
      if(args.size()!=4)throw std::runtime_error("AIR memset signature unsupported");
      return "for(uint i=0u;i<uint("+args[2]+");++i){storeword(uvec2(("+args[0]+").x,("+args[0]+").y+i),uint("+args[1]+"),1u);}";
    }
    if(name.starts_with("llvm.memcpy.")) {
      if(args.size()!=4)throw std::runtime_error("AIR memcpy signature unsupported");
      return "for(uint i=0u;i<uint("+args[2]+");++i){storeword(uvec2(("+args[0]+").x,("+args[0]+").y+i),word(uvec2(("+args[1]+").x,("+args[1]+").y+i))&255u,1u);}";
    }
    if(name.starts_with("llvm.lifetime.start")||name.starts_with("llvm.lifetime.end"))return "";
    if(name.starts_with("air.abs.s."))return type(call.getType())+"(abs("+signed_value(call.getArgOperand(0))+"))";
    for (auto operation : {"min", "max", "clamp"}) {
      if(!name.starts_with(std::string("air.")+operation+"."))continue;
      if(name.starts_with(std::string("air.")+operation+".s."))
        for(unsigned i=0;i<args.size();++i)args[i]=signed_value(call.getArgOperand(i));
      return type(call.getType())+"("+join(operation,static_cast<unsigned>(args.size()))+")";
    }
    if(name=="air.clz.i32")return "uint(31-findMSB("+args[0]+"))";
    if(name=="air.ctz.i32")return "("+args[0]+"==0u?32u:uint(findLSB("+args[0]+")))";
    if(name=="air.popcount.i32")return "uint(bitCount("+args[0]+"))";
    if(name.starts_with("air.convert.")) {
      if(args.size()!=1)throw std::runtime_error("AIR conversion arity unsupported");
      auto source=f->getFunctionType()->getParamType(0);
      auto target=call.getType();
      auto x=args[0];
      if(name.find(".s.i")!=std::string::npos && source->getScalarType()->isIntegerTy())x=signed_value(call.getArgOperand(0));
      if(source->isIntegerTy(1))x="("+x+"?1u:0u)";
      if(target->getScalarType()->isIntegerTy()&&name.starts_with("air.convert.s.")) {
        auto c=isa<FixedVectorType>(target)?"ivec"+std::to_string(cast<FixedVectorType>(target)->getNumElements()):"int";
        x=c+"("+x+")";
      }
      return type(target)+"("+x+")";
    }
    if (name.starts_with("air.dot."))
      return join("dot", 2);
    if (name.starts_with("air.fast_fmin."))
      return join("min", 2);
    if (name.starts_with("air.fast_fmax."))
      return join("max", 2);
    if (name.starts_with("air.fast_sqrt."))
      return join("sqrt", 1);
    if (name.starts_with("air.fast_exp2."))
      return join("exp2", 1);
    if (name.starts_with("air.fast_rsqrt."))
      return join("inversesqrt", 1);
    if (name.starts_with("air.fast_clamp."))
      return join("clamp", 3);
    if (name.starts_with("air.fast_saturate."))
      return "clamp(" + args.at(0) + ",0.0,1.0)";
    if (name.starts_with("air.fast_atan2."))
      return join("atan", 2);
    if (name.starts_with("air.fast_fmod."))
      return "(" + args.at(0) + "-" + args.at(1) + "*trunc(" + args.at(0) +
             "/" + args.at(1) + "))";
    if (name.starts_with("air.mix."))
      return join("mix", 3);
    if(name.starts_with("air.sample_texture_")||name.starts_with("air.read_texture_")||name.starts_with("air.write_texture_")||name.starts_with("air.get_")||name.starts_with("air.gather_texture_")) {
      return texture_call(call,name,args);
    }
    throw std::runtime_error("AIR intrinsic unsupported: " + name);
  }
  std::string expression(Instruction &i) {
    auto operand = [&](unsigned n) { return value(i.getOperand(n)); };
    auto binary = [&](const char *op) {
      return "(" + operand(0) + op + operand(1) + ")";
    };
    switch (i.getOpcode()) {
    case Instruction::FAdd:
    case Instruction::Add:
      return binary("+");
    case Instruction::FSub:
    case Instruction::Sub:
      return binary("-");
    case Instruction::FMul:
    case Instruction::Mul:
      return binary("*");
    case Instruction::FDiv:
    case Instruction::UDiv:
      return binary("/");
    case Instruction::FNeg:
      return "(-"+operand(0)+")";
    case Instruction::FPToSI:
      return type(i.getType())+"(int("+operand(0)+"))";
    case Instruction::SExt:
      return type(i.getType())+"("+signed_value(i.getOperand(0))+")";
    case Instruction::AShr:
      return type(i.getType())+"("+signed_value(i.getOperand(0))+">>"+operand(1)+")";
    case Instruction::FPExt:
    case Instruction::FPTrunc:
      return type(i.getType())+"("+operand(0)+")";
    case Instruction::URem:
      return binary("%");
    case Instruction::SDiv:
    case Instruction::SRem:
      return type(i.getType())+"("+signed_value(i.getOperand(0))+(i.getOpcode()==Instruction::SDiv?"/":"%")+signed_value(i.getOperand(1))+")";
    case Instruction::And:
      if(i.getType()->getScalarType()->isIntegerTy(1)&&isa<FixedVectorType>(i.getType()))return compare(i.getType(),operand(0),operand(1),"&&");
      return binary(i.getType()->isIntegerTy(1) ? "&&" : "&");
    case Instruction::Or:
      if(i.getType()->getScalarType()->isIntegerTy(1)&&isa<FixedVectorType>(i.getType()))return compare(i.getType(),operand(0),operand(1),"||");
      return binary(i.getType()->isIntegerTy(1) ? "||" : "|");
    case Instruction::Xor:
      if(i.getType()->getScalarType()->isIntegerTy(1)&&isa<FixedVectorType>(i.getType()))return compare(i.getType(),operand(0),operand(1),"!=");
      return binary(i.getType()->isIntegerTy(1) ? "!=" : "^");
    case Instruction::Shl:
      return binary("<<");
    case Instruction::LShr:
      return binary(">>");
    case Instruction::ZExt:
    case Instruction::Trunc:
    case Instruction::UIToFP:
    case Instruction::FPToUI:
      if (i.getType()->isIntegerTy(1))
        return "((" + operand(0) + "&1u)!=0u)";
      if (i.getOperand(0)->getType()->isIntegerTy(1))
        return "(" + operand(0) + "?1u:0u)";
      return type(i.getType()) + "(" + operand(0) + ")";
    case Instruction::SIToFP:
      return type(i.getType()) + "(" + signed_value(i.getOperand(0)) + ")";
    case Instruction::BitCast:
      if(i.getType()==i.getOperand(0)->getType()||
         (i.getType()->isPointerTy()&&i.getOperand(0)->getType()->isPointerTy()))return operand(0);
      if(i.getType()->isHalfTy()&&i.getOperand(0)->getType()->isIntegerTy(16))return "unpackHalf2x16("+operand(0)+").x";
      if(i.getType()->isIntegerTy(16)&&i.getOperand(0)->getType()->isHalfTy())return "(packHalf2x16(vec2("+operand(0)+",0.0))&65535u)";
      if(auto *v=dyn_cast<FixedVectorType>(i.getType());v&&v->getNumElements()==2&&v->getElementType()->isHalfTy()&&i.getOperand(0)->getType()->isIntegerTy(32))return "unpackHalf2x16("+operand(0)+")";
      if(auto *v=dyn_cast<FixedVectorType>(i.getOperand(0)->getType());v&&v->getNumElements()==2&&v->getElementType()->isHalfTy()&&i.getType()->isIntegerTy(32))return "packHalf2x16("+operand(0)+")";
      if (i.getType()->isFloatTy() ||
          (isa<FixedVectorType>(i.getType()) &&
           cast<FixedVectorType>(i.getType())->getElementType()->isFloatTy() &&
           isa<FixedVectorType>(i.getOperand(0)->getType()) &&
           cast<FixedVectorType>(i.getOperand(0)->getType())->getElementType()->isIntegerTy(32)))
        return "uintBitsToFloat(" + operand(0) + ")";
      if (i.getOperand(0)->getType()->isFloatTy() ||
          (isa<FixedVectorType>(i.getOperand(0)->getType()) &&
           cast<FixedVectorType>(i.getOperand(0)->getType())->getElementType()->isFloatTy() &&
           isa<FixedVectorType>(i.getType()) &&
           cast<FixedVectorType>(i.getType())->getElementType()->isIntegerTy(32)))
        return "floatBitsToUint(" + operand(0) + ")";
      throw std::runtime_error("AIR bitcast type unsupported");
    case Instruction::Select:
      if(isa<FixedVectorType>(i.getOperand(0)->getType()))return "mix("+operand(2)+","+operand(1)+","+operand(0)+")";
      return "(" + operand(0) + "?" + operand(1) + ":" + operand(2) + ")";
    case Instruction::ExtractElement:
      return "(" + operand(0) + ")[" + operand(1) + "]";
    case Instruction::Load:
      return load(i.getType(), operand(0));
    case Instruction::Alloca:
      return value(&i);
    case Instruction::Call:
      return call(cast<CallInst>(i));
    case Instruction::GetElementPtr: {
      auto &g = cast<GetElementPtrInst>(i);
      SmallMapVector<Value *, APInt, 4> variables;
      APInt constant(64, 0);
      if (!g.collectOffset(module.getDataLayout(), 64, variables, constant))
        throw std::runtime_error("AIR GEP offset unsupported");
      auto offset =
          std::to_string(static_cast<std::uint32_t>(constant.getZExtValue())) +
          "u";
      for (const auto &[v, k] : variables)
        offset += "+uint(" + value(v) + ")*" +
                  std::to_string(static_cast<std::uint32_t>(k.getZExtValue())) +
                  "u";
      return "uvec2((" + operand(0) + ").x,(" + operand(0) + ").y+" + offset +
             ")";
    }
    case Instruction::ShuffleVector: {
      auto &s = cast<ShuffleVectorInst>(i);
      std::string result = type(i.getType()) + "(";
      auto count =
          cast<FixedVectorType>(i.getOperand(0)->getType())->getNumElements();
      for (unsigned n = 0; n < s.getShuffleMask().size(); ++n) {
        if (n)
          result += ",";
        int index = s.getMaskValue(n);
        result +=
            index < 0
                ? zero(cast<FixedVectorType>(i.getType())->getElementType())
                : "(" + operand(static_cast<unsigned>(index) >= count ? 1 : 0) +
                      ")[" +
                      std::to_string(static_cast<unsigned>(index) % count) +
                      "]";
      }
      return result + ")";
    }
    case Instruction::ExtractValue: {
      auto &e = cast<ExtractValueInst>(i);
      auto r = operand(0);
      for (auto n : e.indices())
        r += ".f" + std::to_string(n);
      return r;
    }
    case Instruction::ICmp: {
      auto &c = cast<ICmpInst>(i);
      std::string a = operand(0), b = operand(1), op;
      if (c.isSigned()) {
        a = signed_value(i.getOperand(0));
        b = signed_value(i.getOperand(1));
      }
      switch (c.getPredicate()) {
      case CmpInst::ICMP_EQ:
        op = "==";
        break;
      case CmpInst::ICMP_NE:
        op = "!=";
        break;
      case CmpInst::ICMP_UGT:
      case CmpInst::ICMP_SGT:
        op = ">";
        break;
      case CmpInst::ICMP_UGE:
      case CmpInst::ICMP_SGE:
        op = ">=";
        break;
      case CmpInst::ICMP_ULT:
      case CmpInst::ICMP_SLT:
        op = "<";
        break;
      case CmpInst::ICMP_ULE:
      case CmpInst::ICMP_SLE:
        op = "<=";
        break;
      default:
        throw std::runtime_error("AIR integer predicate unsupported");
      }
      return compare(i.getType(),a,b,op);
    }
    case Instruction::FCmp: {
      auto &c = cast<FCmpInst>(i);
      std::string a = operand(0), b = operand(1), op;
      switch (c.getPredicate()) {
      case CmpInst::FCMP_OEQ:
      case CmpInst::FCMP_UEQ:
        op = "==";
        break;
      case CmpInst::FCMP_ONE:
      case CmpInst::FCMP_UNE:
        op = "!=";
        break;
      case CmpInst::FCMP_OGT:
      case CmpInst::FCMP_UGT:
        op = ">";
        break;
      case CmpInst::FCMP_OGE:
      case CmpInst::FCMP_UGE:
        op = ">=";
        break;
      case CmpInst::FCMP_OLT:
      case CmpInst::FCMP_ULT:
        op = "<";
        break;
      case CmpInst::FCMP_OLE:
      case CmpInst::FCMP_ULE:
        op = "<=";
        break;
      default:
        throw std::runtime_error("AIR floating predicate unsupported");
      }
      return compare(i.getType(),a,b,op,CmpInst::isUnordered(c.getPredicate())?1:-1);
    }
    default:
      throw std::runtime_error(std::string("AIR instruction unsupported: ") +
                               i.getOpcodeName());
    }
  }
  std::string signature(Function &f) {
    std::string result = type(f.getReturnType()) + " " + functions.at(&f) + "(";
    unsigned n = 0;
    for (auto &a : f.args()) {
      result += (n ? "," : "") + type(a.getType()) + " a" + std::to_string(n);
      ++n;
    }
    return result + ")";
  }
  std::string function(Function &f) {
    // AIR extends 32-bit indices to i64 for pointer math. Represent these
    // indices in uint, but reject general i64 computations instead of
    // truncating.
    if (f.getReturnType()->isIntegerTy(64))
      throw std::runtime_error("AIR i64 return unsupported");
    for (auto &a : f.args())
      if (a.getType()->isIntegerTy(64))
        throw std::runtime_error("AIR i64 parameter unsupported");
    for (auto &block : f)
      for (auto &i : block) {
        if (i.getType()->isIntegerTy(64)) {
          bool index =
              ((i.getOpcode() == Instruction::ZExt || i.getOpcode() == Instruction::SExt) &&
               i.getOperand(0)->getType()->getIntegerBitWidth() <= 32) ||
              isa<PHINode>(i) || isa<SelectInst>(i);
          if (!index)
            throw std::runtime_error("AIR i64 arithmetic unsupported");
          for (auto *user : i.users())
            if (!isa<GetElementPtrInst>(user) && !isa<PHINode>(user) &&
                !isa<SelectInst>(user))
              throw std::runtime_error(
                  "AIR i64 value is only supported as a buffer index");
        }
      }
    values.clear();
    unsigned n = 0;
    for (auto &a : f.args())
      values[&a] = "a" + std::to_string(n++);
    std::map<const BasicBlock *, unsigned> blocks;
    unsigned b = 0;
    for (auto &block : f)
      blocks[&block] = b++;
    std::string locals;
    for (auto &block : f)
      for (auto &i : block)
        if (!i.getType()->isVoidTy()) {
          if(auto *a=dyn_cast<AllocaInst>(&i)) {
            auto *count=dyn_cast<ConstantInt>(a->getArraySize());
            if(!count||count->getZExtValue()>65536)throw std::runtime_error("AIR dynamic alloca unsupported");
            memory(a,a->getAllocatedType(),nullptr,static_cast<unsigned>(count->getZExtValue()));
            continue;
          }
          auto name = "v" + std::to_string(serial++);
          values[&i] = name;
          locals +=
              type(i.getType()) + " " + name + "=" + zero(i.getType()) + ";\n";
        }
    std::string body = signature(f) + "{\n" + locals +
                       "int block=0;while(true){switch(block){\n";
    auto edge = [&](const BasicBlock &from, const BasicBlock &to) {
      std::string code, assign;
      for (const auto &p : to.phis()) {
        auto temp = "edge" + std::to_string(serial++);
        code += type(p.getType()) + " " + temp + "=" +
                value(p.getIncomingValueForBlock(&from)) + ";";
        assign += value(&p) + "=" + temp + ";";
      }
      return code + assign + "block=" + std::to_string(blocks.at(&to)) +
             ";continue;";
    };
    for (auto &block : f) {
      body += "case " + std::to_string(blocks[&block]) + ":{\n";
      for (auto &i : block) {
        if (isa<PHINode>(i)||isa<AllocaInst>(i))
          continue;
        if (auto *r = dyn_cast<ReturnInst>(&i))
          body += "return" + (r->getReturnValue()?" "+value(r->getReturnValue()):"") + ";\n";
        else if (auto *branch = dyn_cast<CondBrInst>(&i))
          body += "if(" + value(branch->getCondition()) + "){" +
                  edge(block, *branch->getSuccessor(0)) + "}else{" +
                  edge(block, *branch->getSuccessor(1)) + "}\n";
        else if (auto *branch = dyn_cast<UncondBrInst>(&i))
          body += edge(block, *branch->getSuccessor(0));
        else if (auto *s = dyn_cast<SwitchInst>(&i)) {
          for(auto c:s->cases())body+="if("+value(s->getCondition())+"=="+value(c.getCaseValue())+"){"+edge(block,*c.getCaseSuccessor())+"}";
          body+=edge(block,*s->getDefaultDest());
        } else if(auto *s=dyn_cast<StoreInst>(&i)) {
          body+=store(s->getValueOperand()->getType(),value(s->getPointerOperand()),value(s->getValueOperand()));
        }
        else if (auto *insert = dyn_cast<InsertElementInst>(&i)) {
          body += value(&i) + "=" + value(insert->getOperand(0)) + ";" +
                  value(&i) + "[" + value(insert->getOperand(2)) +
                  "]=" + value(insert->getOperand(1)) + ";\n";
        } else if (auto *insert = dyn_cast<InsertValueInst>(&i)) {
          std::string target = value(&i);
          for (auto index : insert->indices())
            target += ".f" + std::to_string(index);
          body += value(&i) + "=" + value(insert->getOperand(0)) + ";" +
                  target + "=" + value(insert->getOperand(1)) + ";\n";
        } else {
          auto expr = expression(i);
          if (auto *t = dyn_cast<IntegerType>(i.getType());
              t && t->getBitWidth() > 1 && t->getBitWidth() < 32)
            expr = "(" + expr + "&" +
                   std::to_string((1u << t->getBitWidth()) - 1u) + "u)";
          body += (i.getType()->isVoidTy()?"":value(&i)+"=") + expr + ";\n";
        }
      }
      body += "}\n";
    }
    body += "default:return" + (f.getReturnType()->isVoidTy()?"":" "+zero(f.getReturnType())) + ";}}}\n";
    return body;
  }
  std::string run() {
    metadata();
    for(auto &g:module.globals()) {
      if(g.getName().starts_with("__air_sampler_state"))continue;
      if(g.getAddressSpace()==3&&output.stage==ShaderStage::compute) {
        auto previous=memory_declarations.size();memory(&g,g.getValueType());memory_declarations.insert(previous,"shared ");
      } else {
        if(!g.isConstant()||!g.hasInitializer())throw std::runtime_error("AIR mutable global unsupported");
        memory(&g,g.getValueType(),g.getInitializer());
      }
    }
    unsigned n = 0;
    for (auto &f : module)
      if (!f.isDeclaration())
        functions[&f] = "airfn" + std::to_string(n++);
    std::string prototypes, body;
    for (auto &f : module)
      if (!f.isDeclaration()) {
        prototypes += signature(f) + ";\n";
        body += function(f);
      }
    std::string helpers = "uint rawword(uvec2 p){uint x=0u;switch(p.x){";
    for (auto b : buffer_bindings)
      helpers += "case " + std::to_string(b.binding) + "u:x=b" +
                 std::to_string(b.binding) + ".words[p.y>>2];break;";
    for(const auto &[v,m]:memories) {
      (void)v;helpers+="case "+std::to_string(m.id)+"u:x="+m.name+"[p.y>>2];break;";
    }
    helpers += "}return x;}\nuint word(uvec2 p){uint s=(p.y&3u)*8u;uint x=rawword(p);if(s!=0u)x=(x>>s)|(rawword(uvec2(p.x,(p.y&~3u)+4u))<<(32u-s));return x;}\n";
    helpers += "void maskedstore(uvec2 p,uint x,uint mask){switch(p.x){";
    for(auto b:buffer_bindings)if(b.writable) {
      auto dst="b"+std::to_string(b.binding)+".words[p.y>>2]";
      helpers+="case "+std::to_string(b.binding)+"u:"+dst+"=("+dst+"&~mask)|(x&mask);break;";
    }
    for(const auto &[v,m]:memories)if(m.writable) {
      (void)v;auto dst=m.name+"[p.y>>2]";
      helpers+="case "+std::to_string(m.id)+"u:"+dst+"=("+dst+"&~mask)|(x&mask);break;";
    }
    helpers+="}}\nvoid storeword(uvec2 p,uint x,uint bytes){uint s=(p.y&3u)*8u;uint mask=bytes==4u?0xffffffffu:(1u<<(bytes*8u))-1u;maskedstore(p,x<<s,mask<<s);if(s!=0u&&s+bytes*8u>32u)maskedstore(uvec2(p.x,(p.y&~3u)+4u),x>>(32u-s),mask>>(32u-s));}\n";
    helpers+="uint atomicadd(uvec2 p,uint x){switch(p.x){";
    for(auto b:buffer_bindings)if(b.writable)helpers+="case "+std::to_string(b.binding)+"u:return atomicAdd(b"+std::to_string(b.binding)+".words[p.y>>2],x);";
    helpers+="}return 0u;}\n";
    helpers += "vec4 half4(vec4 x){return "
               "vec4(unpackHalf2x16(packHalf2x16(x.xy)),unpackHalf2x16("
               "packHalf2x16(x.zw)));}\n";
    std::string main = "void main(){" + (entry->getReturnType()->isVoidTy()?"":type(entry->getReturnType())+" result=") + functions.at(entry) + "(";
    for (unsigned i = 0; i < arguments.size(); ++i)
      main += (i ? "," : "") + arguments[i];
    main += ");";
    if (output.stage == ShaderStage::fragment) {
      for(unsigned i=0;i<output_targets.size();++i)
        main+=output_targets[i]+"=result"+(fragment_aggregate?".f"+std::to_string(i):"")+";";
    }
    else if(output.stage==ShaderStage::vertex) {
      unsigned varying = 0;
      for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
        auto *o = cast<MDNode>(outputs->getOperand(i));
        if (tag(o, "air.position"))
          main += "gl_Position=result.f" + std::to_string(i) +
                  ";gl_Position.y=-gl_Position.y;";
        else if (tag(o,"air.point_size"))
          main += "gl_PointSize=result.f"+std::to_string(i)+";";
        else
          main += "varying" + std::to_string(varying++) + "=result.f" +
                  std::to_string(i) + ";";
      }
    }
    return "#version 450\n#extension GL_EXT_samplerless_texture_functions : require\nlayout(push_constant) uniform Offsets{uint "
           "offsets[32];}push;\n" +
           structures + buffers + textures + interfaces + memory_declarations + helpers + sampling + prototypes +
           body + main + "}\n";
  }
};
} // namespace

bool translate_air(std::span<const std::uint8_t> bitcode,
                   ShaderFunction &output, std::string &error) {
  error.clear();
  output = {};
  if(shader_cache::read(bitcode,output))return true;
  try {
    LLVMContext context;
    auto parsed = parseBitcodeFile(
        MemoryBufferRef(
            StringRef(reinterpret_cast<const char *>(bitcode.data()),
                      bitcode.size()),
            "MEVK AIR"),
        context);
    if (!parsed) {
      error = toString(parsed.takeError());
      return false;
    }
    std::string verification;
    raw_string_ostream diagnostic(verification);
    if (verifyModule(**parsed, &diagnostic)) {
      error = "invalid AIR module: " + verification;
      return false;
    }
    Emitter emitter{**parsed, output};
    output.glsl = emitter.run();
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                 shaderc_env_version_vulkan_1_0);
    auto result = compiler.CompileGlslToSpv(output.glsl,
                                            output.stage == ShaderStage::vertex
                                                ? shaderc_vertex_shader
                                                : output.stage==ShaderStage::fragment?shaderc_fragment_shader:shaderc_compute_shader,
                                            output.name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
      error = output.name + ": " + result.GetErrorMessage();
      return false;
    }
    output.spirv.assign(result.cbegin(), result.cend());
    shader_cache::write(bitcode,output);
    return true;
  } catch (const std::exception &e) {
    error = output.name + ": " + e.what();
    return false;
  }
}
} // namespace mevk
