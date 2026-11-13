#ifndef MACHOMAN_CLIPBOARD_H
#define MACHOMAN_CLIPBOARD_H
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace ak {
// Payloads are byte strings: embedded NULs and empty data are valid.
using clipboard_data = std::map<std::string, std::string>;
struct clipboard_snapshot {
    std::uint64_t generation = 0;
    std::vector<std::string> types;
};
}
#endif
