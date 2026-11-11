#ifndef MACHOMAN_TRANSLATION_CACHE_H
#define MACHOMAN_TRANSLATION_CACHE_H
#include "machoman/translator.h"
#include <filesystem>

enum class mm_cache_status { hit, stored, bypassed };
/* Cache lives in .machoman-cache beside the original Mach-O. A miss,
 * corrupt cache or unwritable directory falls back to fresh translation. */
mm_cache_status mm_translate_cached(const std::filesystem::path &binary,
    std::span<const std::uint8_t> image, std::span<const std::uint8_t> text,
    std::uint64_t base_va, mm_translation_result &result);
std::filesystem::path mm_translation_cache_path(const std::filesystem::path &binary);
#endif
