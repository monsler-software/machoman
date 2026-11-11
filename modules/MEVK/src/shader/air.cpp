#include "mevk/shader.h"
#include <llvm/ADT/MapVector.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
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

struct Emitter {
  Module &module;
  ShaderFunction &output;
  Function *entry = nullptr;
  const MDNode *outputs = nullptr;
  std::vector<std::string> arguments;
  std::string interfaces, buffers, textures, structures;
  std::map<Type *, std::string> types;
  std::unordered_map<const Value *, std::string> values;
  std::unordered_map<const Function *, std::string> functions;
  std::vector<ShaderBinding> buffer_bindings, texture_bindings;
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
    if (isa<UndefValue>(v) || isa<PoisonValue>(v) ||
        isa<ConstantAggregateZero>(v) || isa<ConstantPointerNull>(v))
      return zero(v->getType());
    // LLVM 23 also uses ConstantFP/ConstantInt for vector splats. Preserve
    // their vector type before handling scalar constants.
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
        g && g->getName() == "__air_sampler_state")
      return "uvec2(0u)";
    throw std::runtime_error("AIR operand unsupported");
  }
  std::string signed_value(const Value *v) {
    auto *t = dyn_cast<IntegerType>(v->getType());
    if (!t)
      throw std::runtime_error("AIR signed vector operation unsupported");
    auto bits = t->getBitWidth();
    if (bits > 32)
      throw std::runtime_error("AIR signed 64-bit arithmetic unsupported");
    if (bits == 32)
      return "int(" + value(v) + ")";
    return "(int(" + value(v) + " << " + std::to_string(32 - bits) + ") >> " +
           std::to_string(32 - bits) + ")";
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
  void metadata() {
    NamedMDNode *node = module.getNamedMetadata("air.vertex");
    output.stage = ShaderStage::vertex;
    if (!node) {
      node = module.getNamedMetadata("air.fragment");
      output.stage = ShaderStage::fragment;
    }
    if (!node || node->getNumOperands() != 1)
      throw std::runtime_error(
          "AIR requires one vertex or fragment entry point");
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
        if (!tag(o, "air.vertex_output"))
          throw std::runtime_error("AIR vertex output unsupported");
        auto *t = cast<StructType>(entry->getReturnType())->getElementType(i);
        bool flat = t->isIntegerTy();
        interfaces += "layout(location=" + std::to_string(varying) + ") " +
                      (flat ? "flat " : "") + "out " + type(t) + " varying" +
                      std::to_string(varying) + ";\n";
        ++varying;
      }
      if (!position)
        throw std::runtime_error("AIR vertex position missing");
    } else {
      auto *returned = dyn_cast<FixedVectorType>(entry->getReturnType());
      if (!returned || returned->getNumElements() != 4 ||
          (!returned->getElementType()->isFloatTy() &&
           !returned->getElementType()->isHalfTy()))
        throw std::runtime_error("AIR fragment output shape unsupported");
      interfaces += "layout(location=0) out vec4 color;\n";
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
        if (metal >= 16)
          throw std::runtime_error(
              "AIR resource index exceeds initial 16-slot stage limit");
        auto binding = metal +
                       (output.stage == ShaderStage::fragment ? 16u : 0u) +
                       (texture ? 32u : 0u);
        ShaderBinding resource{metal, binding, texture};
        output.bindings.push_back(resource);
        if (texture) {
          texture_bindings.push_back(resource);
          textures += "layout(set=0,binding=" + std::to_string(binding) +
                      ") uniform sampler2D tex" + std::to_string(binding) +
                      ";\n";
          arguments[index] = "uvec2(" + std::to_string(binding) + "u,0u)";
        } else {
          buffer_bindings.push_back(resource);
          buffers += "layout(std430,set=0,binding=" + std::to_string(binding) +
                     ") readonly buffer B" + std::to_string(binding) +
                     "{uint words[];} b" + std::to_string(binding) + ";\n";
          arguments[index] = "uvec2(" + std::to_string(binding) +
                             "u,push.offsets[" + std::to_string(binding) + "])";
        }
      } else if (tag(n, "air.vertex_id"))
        arguments[index] = "uint(gl_VertexIndex)";
      else if (tag(n, "air.instance_id"))
        arguments[index] = "uint(gl_InstanceIndex)";
      else if (tag(n, "air.position"))
        arguments[index] = "gl_FragCoord";
      else if (tag(n, "air.fragment_input")) {
        bool flat = t->isIntegerTy();
        interfaces += "layout(location=" + std::to_string(varying) + ") " +
                      (flat ? "flat " : "") + "in " + type(t) + " varying" +
                      std::to_string(varying) + ";\n";
        arguments[index] = "varying" + std::to_string(varying++);
      } else
        throw std::runtime_error("AIR shader input unsupported");
    }
    for (const auto &a : arguments)
      if (a.empty())
        throw std::runtime_error("AIR argument metadata missing");
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
    if (name.starts_with("air.dot."))
      return join("dot", 2);
    if (name.starts_with("air.fast_fmin."))
      return join("min", 2);
    if (name.starts_with("air.fast_fmax."))
      return join("max", 2);
    if (name.starts_with("air.fast_sqrt."))
      return join("sqrt", 1);
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
    if (name == "air.convert.f.f32.s.i32")
      return "float(int(" + args.at(0) + "))";
    if (name == "air.convert.u.i32.f.f32")
      return "uint(" + args.at(0) + ")";
    if (name == "air.convert.f.v4f32.f.v4f16")
      return args.at(0);
    if (name.starts_with("air.sample_texture_2d.")) {
      auto constant_zero = [&](unsigned i) {
        auto *c = dyn_cast<Constant>(call.getArgOperand(i));
        return c && c->isNullValue();
      };
      auto *normalized = args.size() == 9
                             ? dyn_cast<ConstantInt>(call.getArgOperand(3))
                             : nullptr;
      if (args.size() != 9 || !normalized || !normalized->isOne() ||
          !constant_zero(5) || !constant_zero(4) || !constant_zero(6) ||
          !constant_zero(7) || !constant_zero(8))
        throw std::runtime_error("AIR texture sampling mode unsupported");
      auto *sampler = dyn_cast<GlobalVariable>(call.getArgOperand(1));
      auto *encoding = sampler && sampler->hasInitializer()
                           ? dyn_cast<ConstantInt>(sampler->getInitializer())
                           : nullptr;
      if (!encoding || encoding->getZExtValue() != 0x807bff0000080a49ull)
        throw std::runtime_error("AIR literal sampler encoding unsupported");
      auto sample = "sample2d(" + args[0] + ".x," + args[2] + ")";
      if (name.ends_with("v4f16"))
        sample = "half4(" + sample + ")";
      return type(call.getType()) + "(" + sample + ",0u)";
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
    case Instruction::URem:
      return binary("%");
    case Instruction::And:
      return binary(i.getType()->isIntegerTy(1) ? "&&" : "&");
    case Instruction::Or:
      return binary(i.getType()->isIntegerTy(1) ? "||" : "|");
    case Instruction::Xor:
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
      if (i.getType()->isFloatTy())
        return "uintBitsToFloat(" + operand(0) + ")";
      if (i.getOperand(0)->getType()->isFloatTy())
        return "floatBitsToUint(" + operand(0) + ")";
      throw std::runtime_error("AIR bitcast type unsupported");
    case Instruction::Select:
      return "(" + operand(0) + "?" + operand(1) + ":" + operand(2) + ")";
    case Instruction::ExtractElement:
      return "(" + operand(0) + ")[" + operand(1) + "]";
    case Instruction::Load:
      return load(i.getType(), operand(0));
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
      return "(" + a + op + b + ")";
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
      auto nan = "(isnan(" + a + ")||isnan(" + b + "))";
      return "(" +
             (CmpInst::isUnordered(c.getPredicate()) ? nan + "||"
                                                     : "!" + nan + "&&") +
             "(" + a + op + b + "))";
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
              (i.getOpcode() == Instruction::ZExt &&
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
        if (isa<PHINode>(i))
          continue;
        if (auto *r = dyn_cast<ReturnInst>(&i))
          body += "return " + value(r->getReturnValue()) + ";\n";
        else if (auto *branch = dyn_cast<CondBrInst>(&i))
          body += "if(" + value(branch->getCondition()) + "){" +
                  edge(block, *branch->getSuccessor(0)) + "}else{" +
                  edge(block, *branch->getSuccessor(1)) + "}\n";
        else if (auto *branch = dyn_cast<UncondBrInst>(&i))
          body += edge(block, *branch->getSuccessor(0));
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
          body += value(&i) + "=" + expr + ";\n";
        }
      }
      body += "}\n";
    }
    body += "default:return " + zero(f.getReturnType()) + ";}}}\n";
    return body;
  }
  std::string run() {
    metadata();
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
    std::string helpers = "uint word(uvec2 p){uint x=0u;switch(p.x){";
    for (auto b : buffer_bindings)
      helpers += "case " + std::to_string(b.binding) + "u:x=b" +
                 std::to_string(b.binding) + ".words[p.y>>2];break;";
    helpers += "}return x>>((p.y&3u)*8u);}\nvec4 sample2d(uint index,vec2 "
               "uv){switch(index){";
    for (auto t : texture_bindings)
      helpers += "case " + std::to_string(t.binding) + "u:return texture(tex" +
                 std::to_string(t.binding) + ",uv);";
    helpers += "}return vec4(0);}\nvec4 half4(vec4 x){return "
               "vec4(unpackHalf2x16(packHalf2x16(x.xy)),unpackHalf2x16("
               "packHalf2x16(x.zw)));}\n";
    std::string main = "void main(){" + type(entry->getReturnType()) +
                       " result=" + functions.at(entry) + "(";
    for (unsigned i = 0; i < arguments.size(); ++i)
      main += (i ? "," : "") + arguments[i];
    main += ");";
    if (output.stage == ShaderStage::fragment)
      main += "color=result;";
    else {
      unsigned varying = 0;
      for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
        auto *o = cast<MDNode>(outputs->getOperand(i));
        if (tag(o, "air.position"))
          main += "gl_Position=result.f" + std::to_string(i) +
                  ";gl_Position.y=-gl_Position.y;";
        else
          main += "varying" + std::to_string(varying++) + "=result.f" +
                  std::to_string(i) + ";";
      }
    }
    return "#version 450\nlayout(push_constant) uniform Offsets{uint "
           "offsets[32];}push;\n" +
           structures + buffers + textures + interfaces + helpers + prototypes +
           body + main + "}\n";
  }
};
} // namespace

bool translate_air(std::span<const std::uint8_t> bitcode,
                   ShaderFunction &output, std::string &error) {
  error.clear();
  output = {};
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
                                                : shaderc_fragment_shader,
                                            output.name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
      error = output.name + ": " + result.GetErrorMessage();
      return false;
    }
    output.spirv.assign(result.cbegin(), result.cend());
    return true;
  } catch (const std::exception &e) {
    error = output.name + ": " + e.what();
    return false;
  }
}
} // namespace mevk
