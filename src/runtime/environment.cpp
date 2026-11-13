#include "machoman/environment.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

namespace {
bool matches(const char *entry, std::string_view name) {
    const std::string_view text(entry);
    return text.size() > name.size() && text.starts_with(name) && text[name.size()] == '=';
}
}
bool mm_environment::valid_name(std::string_view name) {
    return !name.empty() && name.find_first_of("=\0", 0, 2) == std::string_view::npos;
}
char *mm_environment::own(std::string_view text) {
    auto storage = std::make_unique<char[]>(text.size() + 1);
    std::memcpy(storage.get(), text.data(), text.size());
    storage[text.size()] = '\0';
    char *result = storage.get();
    strings_.push_back(std::move(storage));
    return result;
}
void mm_environment::publish(std::span<char *const> entries) {
    auto array = std::make_unique<char *[]>(entries.size() + 1);
    std::copy(entries.begin(), entries.end(), array.get());
    array[entries.size()] = nullptr;
    auto *pointer = array.get();
    arrays_.push_back(std::move(array));
    *slot_ = pointer;
}
mm_environment::mm_environment(char *const *initial) {
    std::vector<char *> entries;
    for (auto p = initial; p && *p; ++p) entries.push_back(own(*p));
    publish(entries);
}
void mm_environment::bind(char ***storage) {
    std::lock_guard lock(mutex_);
    current_ = *slot_;
    slot_ = storage ? storage : &current_;
    *slot_ = current_;
}
char *mm_environment::get(const char *name) {
    if (!name || !valid_name(name)) return nullptr;
    std::lock_guard lock(mutex_);
    for (auto p = data(); p && *p; ++p)
        if (matches(*p, name)) return *p + std::strlen(name) + 1;
    return nullptr;
}
int mm_environment::set(const char *name, const char *value, int overwrite) {
    if (!name || !value || !valid_name(name)) return EINVAL;
    std::lock_guard lock(mutex_);
    try {
        std::vector<char *> entries;
        bool found = false;
        char *replacement = nullptr;
        for (auto p = data(); p && *p; ++p) {
            if (matches(*p, name)) {
                if (!overwrite) return 0;
                if (!found) {
                    replacement = own(std::string(name) + '=' + value);
                    entries.push_back(replacement);
                }
                found = true;
            } else entries.push_back(*p);
        }
        if (!found) entries.push_back(own(std::string(name) + '=' + value));
        publish(entries);
        return 0;
    } catch (const std::bad_alloc &) { return ENOMEM; }
}
int mm_environment::unset(const char *name) {
    if (!name || !valid_name(name)) return EINVAL;
    std::lock_guard lock(mutex_);
    try {
        std::vector<char *> entries;
        bool found = false;
        for (auto p = data(); p && *p; ++p) {
            if (matches(*p, name)) found = true;
            else entries.push_back(*p);
        }
        if (found) publish(entries);
        return 0;
    } catch (const std::bad_alloc &) { return ENOMEM; }
}
int mm_environment::put(char *entry) {
    if (!entry) return EINVAL;
    const std::string_view text(entry);
    const auto separator = text.find('=');
    if (separator == std::string_view::npos || !valid_name(text.substr(0, separator))) return EINVAL;
    std::lock_guard lock(mutex_);
    try {
        std::vector<char *> entries;
        bool found = false;
        for (auto p = data(); p && *p; ++p) {
            if (matches(*p, text.substr(0, separator))) {
                if (!found) entries.push_back(entry);
                found = true;
            } else entries.push_back(*p);
        }
        if (!found) entries.push_back(entry);
        publish(entries);
        return 0;
    } catch (const std::bad_alloc &) { return ENOMEM; }
}
std::vector<std::string> mm_environment::snapshot() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    for (auto p = data(); p && *p; ++p) result.emplace_back(*p);
    return result;
}
