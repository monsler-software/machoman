#pragma once
#include "mevk/shader.h"
namespace mevk::shader_cache {
bool read(std::span<const std::uint8_t>,ShaderFunction&);
void write(std::span<const std::uint8_t>,const ShaderFunction&);
}
