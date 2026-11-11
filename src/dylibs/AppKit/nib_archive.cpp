#include "nib_archive.h"
#include <bit>
#include <limits>
#include <stdexcept>

namespace ak {
namespace {
struct reader {
    std::string_view bytes;
    std::size_t at = 0;
    std::uint64_t integer(unsigned n) {
        if (n > 8 || at > bytes.size() || n > bytes.size() - at) throw std::runtime_error("Truncated NIBArchive");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < n; ++i) value |= std::uint64_t(static_cast<unsigned char>(bytes[at++])) << (8 * i);
        return value;
    }
    std::uint32_t varint() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7) {
            const auto byte = static_cast<unsigned>(integer(1));
            if (shift == 28 && (byte & 0x70)) throw std::runtime_error("NIBArchive integer overflow");
            value |= (byte & 0x7f) << shift;
            // NIBArchive marks the last byte with the high bit.
            if (byte & 0x80) return value;
        }
        throw std::runtime_error("Invalid NIBArchive integer");
    }
    std::string data(std::size_t length) {
        if (at > bytes.size() || length > bytes.size() - at) throw std::runtime_error("Truncated NIBArchive data");
        auto out = std::string(bytes.substr(at, length)); at += length; return out;
    }
};
struct object_entry { std::uint32_t class_id, start, count; };
}
bool parse_nib_archive(std::string_view bytes, std::vector<nib_record> &objects, std::string &error) {
    objects.clear(); error.clear();
    try {
        if (!bytes.starts_with("NIBArchive") || bytes.size() < 50) throw std::runtime_error("Invalid NIBArchive header");
        reader header{bytes, 10};
        if (header.integer(4) != 1) throw std::runtime_error("Unsupported NIBArchive version");
        header.integer(4); // Encoder version.
        std::uint32_t counts[4], offsets[4];
        for (unsigned i = 0; i < 4; ++i) {
            counts[i] = static_cast<std::uint32_t>(header.integer(4));
            offsets[i] = static_cast<std::uint32_t>(header.integer(4));
            if (offsets[i] < 50 || offsets[i] > bytes.size() || counts[i] > bytes.size())
                throw std::runtime_error("Invalid NIBArchive table");
            if (i && offsets[i] < offsets[i-1]) throw std::runtime_error("Overlapping NIBArchive tables");
        }
        auto table = [&](unsigned i) { return reader{bytes.substr(offsets[i], (i == 3 ? bytes.size() : offsets[i+1]) - offsets[i])}; };
        reader r = table(0);
        std::vector<object_entry> entries;
        for (std::uint32_t i = 0; i < counts[0]; ++i) {
            object_entry e{r.varint(), r.varint(), r.varint()};
            if (e.class_id >= counts[3] || e.start > counts[2] || e.count > counts[2] - e.start)
                throw std::runtime_error("Invalid NIBArchive object range");
            entries.push_back(e);
        }
        r = table(1); std::vector<std::string> keys;
        for (std::uint32_t i = 0; i < counts[1]; ++i) keys.push_back(r.data(r.varint()));
        r = table(3); std::vector<std::string> classes;
        for (std::uint32_t i = 0; i < counts[3]; ++i) {
            const auto length = r.varint(), extras = r.varint();
            if (extras > r.bytes.size() / 4) throw std::runtime_error("Invalid NIBArchive class metadata");
            r.data(std::size_t(extras) * 4);
            auto name = r.data(length);
            if (name.empty() || name.back() != '\0') throw std::runtime_error("Invalid NIBArchive class name");
            name.pop_back(); classes.push_back(std::move(name));
        }
        r = table(2); std::vector<nib_value> values;
        for (std::uint32_t i = 0; i < counts[2]; ++i) {
            const auto key = r.varint();
            if (key >= keys.size()) throw std::runtime_error("Invalid NIBArchive key index");
            nib_value v; v.key = keys[key]; v.type = static_cast<unsigned>(r.integer(1));
            switch (v.type) {
            case 0: case 1: case 2: case 3: {
                const unsigned bits = 8u << v.type;
                auto raw = r.integer(bits / 8);
                if (bits < 64 && (raw & (std::uint64_t{1} << (bits-1)))) raw |= ~std::uint64_t{0} << bits;
                v.integer = std::bit_cast<std::int64_t>(raw); break;
            }
            case 4: v.integer = 0; break;
            case 5: v.integer = 1; break;
            case 6: v.real = std::bit_cast<float>(static_cast<std::uint32_t>(r.integer(4))); break;
            case 7: v.real = std::bit_cast<double>(r.integer(8)); break;
            case 8: v.bytes = r.data(r.varint()); break;
            case 9: break;
            case 10:
                v.integer = static_cast<std::int64_t>(r.integer(4));
                if (static_cast<std::uint64_t>(v.integer) >= counts[0]) throw std::runtime_error("Invalid NIBArchive object reference");
                break;
            default: throw std::runtime_error("Unsupported NIBArchive value type");
            }
            values.push_back(std::move(v));
        }
        std::vector<nib_record> parsed;
        for (auto e : entries) parsed.push_back({classes[e.class_id], {values.begin()+e.start, values.begin()+e.start+e.count}});
        objects = std::move(parsed); return true;
    } catch (const std::exception &e) { error = e.what(); return false; }
}
}
