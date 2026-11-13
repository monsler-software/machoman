#include "mevk/shader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <llvm/AsmParser/Parser.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

void check(bool yes, const char *message) {
  if (!yes) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
std::vector<std::uint8_t> assembly_bitcode(const std::string &assembly) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(assembly, diagnostic, context);
  check(bool(module), "assemble AIR test input");
  llvm::SmallVector<char, 0> data;
  llvm::raw_svector_ostream stream(data);
  llvm::WriteBitcodeToFile(*module, stream);
  return {data.begin(), data.end()};
}
std::vector<std::uint8_t> bitcode(const std::string &instructions) {
  return assembly_bitcode("define <4 x float> @test() {\n" + instructions +
          "\nret <4 x float> zeroinitializer\n}\n!air.fragment = !{!0}\n"
          "!0 = !{ptr @test, !1, !1}\n!1 = !{}\n");
}
void put(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint64_t n,
         unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    bytes[offset + i] = static_cast<std::uint8_t>(n >> (i * 8));
}
int main() {
  mevk::ShaderFunction shader;
  std::string error;
  auto packed = assembly_bitcode(
      "declare float @air.fast_exp2.f32(float)\n"
      "define <{ <4 x float> }> @test(<4 x float> %color) {\n"
      "%bits = bitcast <4 x float> %color to <4 x i32>\n"
      "%floats = bitcast <4 x i32> %bits to <4 x float>\n"
      "%e = call float @air.fast_exp2.f32(float 0.0)\n"
      "%v = insertelement <4 x float> %floats, float %e, i32 3\n"
      "%r = insertvalue <{ <4 x float> }> undef, <4 x float> %v, 0\n"
      "ret <{ <4 x float> }> %r\n}\n"
      "!air.fragment = !{!0}\n!0 = !{ptr @test, !1, !3}\n"
      "!1 = !{!2}\n!2 = !{!\"air.render_target\", i32 0}\n"
      "!3 = !{!4}\n!4 = !{i32 0, !\"air.fragment_input\", !\"user(locn4)\"}\n");
  check(mevk::translate_air(packed, shader, error) && !shader.spirv.empty(),
        "packed fragment color, vector bitcasts and exp2 compile");
  check(shader.glsl.find("layout(location=4) in vec4") != std::string::npos,
        "fragment varying preserves AIR location instead of argument order");
  auto invalid_target = assembly_bitcode(
      "define <{ <4 x float> }> @test() { ret <{ <4 x float> }> zeroinitializer }\n"
      "!air.fragment = !{!0}\n!0 = !{ptr @test, !1, !3}\n"
      "!1 = !{!2}\n!2 = !{!\"air.render_target\", i32 8}\n!3 = !{}\n");
  check(!mevk::translate_air(invalid_target, shader, error) && !error.empty(),
        "unsupported fragment render target rejected");
  auto good = bitcode("%i = zext i32 3 to i64\n%p = getelementptr float, ptr "
                      "null, i64 %i\n%v = load float, ptr %p");
  if (!mevk::translate_air(good, shader, error)) {
    std::fprintf(stderr, "%s\n", shader.glsl.c_str());
    check(false, error.c_str());
  }
  check(!shader.spirv.empty(), "accepted i64 buffer index produces SPIR-V");
  for (const auto &input : {
           "%i = add i64 4294967296, 1",
           "%i = zext i32 3 to i64\n%f = uitofp i64 %i to float",
           "%v = insertelement <2 x i64> zeroinitializer, i64 4, i32 0",
           "%i = phi i64 [ 4294967296, %0 ]" // verifier rejects invalid CFG
       })
    check(!mevk::translate_air(bitcode(input), shader, error) && !error.empty(),
          "unsupported or invalid i64 input rejected");

  std::vector<std::uint8_t> lib(108 + good.size());
  std::memcpy(lib.data(), "MTLB", 4);
  put(lib, 16, lib.size(), 8);
  put(lib, 72, 88, 8);
  put(lib, 80, lib.size() - 88, 8);
  put(lib, 88, 0x0b17c0de, 4);
  put(lib, 96, 20, 4);
  put(lib, 100, good.size(), 4);
  std::memcpy(lib.data() + 108, good.data(), good.size());
  std::vector<mevk::ShaderFunction> functions;
  check(mevk::load_metallib(lib, functions, error) && functions.size() == 1,
        "synthetic metallib translated");
  auto bad = lib;
  put(bad, 72, UINT64_MAX, 8);
  check(!mevk::load_metallib(bad, functions, error) && functions.empty(),
        "overflowed section offset rejected");
  bad = lib;
  put(bad, 80, UINT64_MAX, 8);
  check(!mevk::load_metallib(bad, functions, error) && functions.empty(),
        "overflowed section length rejected");
  bad = lib;
  put(bad, 96, 19, 4);
  check(!mevk::load_metallib(bad, functions, error) && functions.empty(),
        "wrapper overlapping header rejected");
  bad = lib;
  put(bad, 100, UINT32_MAX, 4);
  check(!mevk::load_metallib(bad, functions, error) && functions.empty(),
        "out of range bitcode length rejected");
  bad = lib;
  bad.resize(88);
  put(bad, 16, 88, 8);
  put(bad, 80, 0, 8);
  check(!mevk::load_metallib(bad, functions, error) && functions.empty(),
        "empty library rejected");
  bad = lib;
  bad.insert(bad.end(), lib.begin() + 88, lib.end());
  put(bad, 16, bad.size(), 8);
  put(bad, 80, bad.size() - 88, 8);
  check(!mevk::load_metallib(bad, functions, error) && functions.empty(),
        "duplicate function rejected atomically");
  std::puts("MEVK shader input and metallib bounds tests passed");
}
