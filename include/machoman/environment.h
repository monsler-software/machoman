#ifndef MACHOMAN_ENVIRONMENT_H
#define MACHOMAN_ENVIRONMENT_H

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/* One mutable Darwin process environment. The pointer slot is authoritative:
 * callers may replace environ or edit putenv's borrowed buffer. Reads therefore
 * scan that array instead of maintaining a second, potentially stale map.
 * Owned strings/arrays survive replacement until process teardown. As with
 * libc, callers must serialize direct writes to environ/borrowed buffers. */
class mm_environment {
public:
    explicit mm_environment(char *const *initial = nullptr);
    char ***slot() { return slot_; }
    char **data() const { return *slot_; }
    void bind(char ***storage);
    char *get(const char *name);
    int set(const char *name, const char *value, int overwrite);
    int unset(const char *name);
    int put(char *entry);
    std::vector<std::string> snapshot();
    void prepare_fork() { mutex_.lock(); }
    void finish_fork() { mutex_.unlock(); }
    static bool valid_name(std::string_view name);
private:
    void publish(std::span<char *const> entries);
    char *own(std::string_view text);
    char **current_ = nullptr;
    char ***slot_ = &current_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<char[]>> strings_;
    std::vector<std::unique_ptr<char *[]>> arrays_;
};

#endif
