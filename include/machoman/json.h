#ifndef MACHOMAN_JSON_H
#define MACHOMAN_JSON_H
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/* Lossless JSON numbers preserve unknown manifest fields without conversion
 * through floating point. Objects reject duplicate keys when parsed. */
struct mm_json_number { std::string text; };
struct mm_json {
    using object = std::map<std::string, mm_json, std::less<>>;
    using array = std::vector<mm_json>;
    std::variant<std::nullptr_t, bool, mm_json_number, std::string, object, array> value = nullptr;
    mm_json() = default;
    template<class T> explicit mm_json(T data) : value(std::move(data)) {}
};
bool mm_json_parse(std::string_view source, mm_json &out, std::string &error);
std::string mm_json_serialize(const mm_json &value);
#endif
