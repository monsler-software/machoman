#include "../src/dylibs/AppKit/nib_archive.h"
#include "nib_archive_fixture.h"
#include <cstdio>
#include <cstdlib>
void require(bool ok, const char *message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
int main() {
    const std::string bytes(reinterpret_cast<const char *>(nib_archive_fixture), sizeof nib_archive_fixture);
    std::vector<ak::nib_record> records; std::string error;
    require(ak::parse_nib_archive(bytes, records, error), "valid NIBArchive");
    require(records.size() == 10 && records[0].values[0].integer == 1, "root reference");
    require(records[3].class_name == "NSMutableArray" && records[3].values[1].key == "UINibEncoderEmptyKey", "inline array");
    require(records[6].values[0].bytes == "Menu from NIBArchive", "string bytes");
    for (std::size_t length = 0; length < bytes.size(); ++length)
        require(!ak::parse_nib_archive(std::string_view(bytes).substr(0, length), records, error) && records.empty(), "truncation rejected atomically");
    auto bad = bytes; bad[50] = static_cast<char>(0xff);
    require(!ak::parse_nib_archive(bad, records, error), "invalid class index");
    bad = bytes; bad[26] = '\0'; bad[27] = '\0'; bad[28] = '\0'; bad[29] = '\0';
    require(!ak::parse_nib_archive(bad, records, error), "invalid table offset");
    std::puts("NIBArchive graph, inline arrays, strings and malformed inputs passed");
}
