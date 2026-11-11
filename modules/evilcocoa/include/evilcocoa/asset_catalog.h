#ifndef EVILCOCOA_ASSET_CATALOG_H
#define EVILCOCOA_ASSET_CATALOG_H
#include "renderer.h"
#include <string_view>
namespace evilcocoa {
// Decode a named bitmap from a CoreUI BOM catalog. Unsupported renditions
// and malformed input return an empty bitmap.
bitmap read_asset_catalog(std::span<const std::uint8_t> bytes, std::string_view name, double scale = 2);
} // namespace evilcocoa
#endif
