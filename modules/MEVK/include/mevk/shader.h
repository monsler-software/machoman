#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mevk {
enum class ShaderStage { vertex, fragment, compute };
struct ShaderBinding {
    std::uint32_t metal_index = 0;
    std::uint32_t binding = 0;
    bool texture = false;
    bool sampler = false;
    // Metal texture type: 2D=2, 2D array=3, MSAA=4, cube=5, 3D=7.
    std::uint32_t texture_type = 2;
    // 0=float, 1=signed integer, 2=unsigned integer.
    std::uint32_t sample_type = 0;
    bool writable = false;
    // A literal sampler uses metal_index=UINT32_MAX.
    std::uint64_t literal_sampler = 0;
};
struct ShaderFunction {
    std::string name;
    ShaderStage stage = ShaderStage::vertex;
    std::vector<ShaderBinding> bindings;
    std::vector<std::uint32_t> spirv;
    std::string glsl;
};
struct ShaderCacheStats {
    std::uint64_t hits=0,misses=0,writes=0,rejected=0;
};
ShaderCacheStats shader_cache_stats();
// MEVK_SHADER_CACHE selects a directory; "0" disables disk caching.
// Otherwise use the platform's user cache directory.
std::string shader_cache_directory();
// Apple AIR bitcode is translated from its actual LLVM instructions and
// metadata. Unsupported instructions/intrinsics fail with a diagnostic.
bool translate_air(std::span<const std::uint8_t> bitcode, ShaderFunction& output, std::string& error);
bool load_metallib(std::span<const std::uint8_t> bytes, std::vector<ShaderFunction>& output, std::string& error);
} // namespace mevk
