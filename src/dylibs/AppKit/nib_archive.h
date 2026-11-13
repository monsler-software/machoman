#ifndef MACHOMAN_NIB_ARCHIVE_H
#define MACHOMAN_NIB_ARCHIVE_H
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace ak {
struct nib_value {
    std::string key;
    unsigned type = 9;
    std::int64_t integer = 0;
    double real = 0;
    std::string bytes;
};
struct nib_record { std::string class_name; std::vector<nib_value> values; };
bool parse_nib_archive(std::string_view bytes, std::vector<nib_record> &objects, std::string &error);
bool parse_nib_geometry(std::string_view text, std::span<double> values);
}
#endif
