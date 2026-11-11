#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mevk {
enum class ShaderStage { vertex, fragment };
struct ShaderBinding {
    std::uint32_t metal_index = 0;
    std::uint32_t binding = 0;
    bool texture = false;
};
struct ShaderFunction {
    std::string name;
    ShaderStage stage = ShaderStage::vertex;
    std::vector<ShaderBinding> bindings;
    std::vector<std::uint32_t> spirv;
    std::string glsl;
};
// Apple AIR bitcode is translated from its actual LLVM instructions and
// metadata. Unsupported instructions/intrinsics fail with a diagnostic.
bool translate_air(std::span<const std::uint8_t> bitcode, ShaderFunction& output, std::string& error);
bool load_metallib(std::span<const std::uint8_t> bytes, std::vector<ShaderFunction>& output, std::string& error);
} // namespace mevk
