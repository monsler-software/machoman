#include "mevk/shader.h"
#include <cstring>
#include <limits>

namespace mevk {
namespace {
std::uint64_t read(std::span<const std::uint8_t> b, std::size_t o, unsigned n) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i)
    v |= std::uint64_t{b[o + i]} << (8 * i);
  return v;
}
} // namespace
bool load_metallib(std::span<const std::uint8_t> bytes,
                   std::vector<ShaderFunction> &output, std::string &error) {
  error.clear();
  output.clear();
  if (bytes.size() < 88 || std::memcmp(bytes.data(), "MTLB", 4) != 0 ||
      read(bytes, 16, 8) != bytes.size()) {
    error = "MEVK: invalid or truncated metallib header";
    return false;
  }
  auto start = read(bytes, 72, 8), length = read(bytes, 80, 8);
  if (start > bytes.size() || length > bytes.size() - start) {
    error = "MEVK: metallib bitcode section outside file";
    return false;
  }
  auto end = static_cast<std::size_t>(start + length);
  for (auto offset = static_cast<std::size_t>(start); offset + 20 <= end;
       ++offset) {
    if (read(bytes, offset, 4) != 0x0b17c0deu)
      continue;
    auto relative = read(bytes, offset + 8, 4),
         size = read(bytes, offset + 12, 4);
    if (relative < 20 || relative > end - offset ||
        size > end - offset - relative) {
      error = "MEVK: truncated AIR bitcode wrapper";
      output.clear();
      return false;
    }
    ShaderFunction shader;
    if (!translate_air(
            bytes.subspan(offset + static_cast<std::size_t>(relative),
                          static_cast<std::size_t>(size)),
            shader, error)) {
      output.clear();
      return false;
    }
    for (const auto &existing : output)
      if (existing.name == shader.name) {
        error = "MEVK: duplicate metallib function";
        output.clear();
        return false;
      }
    output.push_back(std::move(shader));
    offset += static_cast<std::size_t>(relative + size) - 1;
  }
  if (output.empty()) {
    error = "MEVK: metallib contains no supported AIR functions";
    return false;
  }
  return true;
}
} // namespace mevk
