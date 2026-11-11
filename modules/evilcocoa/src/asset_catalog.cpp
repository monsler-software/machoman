#include "evilcocoa/asset_catalog.h"
#include "lzfse.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace evilcocoa {
namespace {
using bytes = std::span<const std::uint8_t>;
struct input {
    bytes data;
    bytes slice(std::size_t offset, std::size_t length) const {
        if (offset > data.size() || length > data.size() - offset)
            throw std::runtime_error("catalog range");
        return data.subspan(offset, length);
    }
    unsigned u16(std::size_t i) const {
        auto b = slice(i, 2);
        return b[0] | (unsigned(b[1]) << 8);
    }
    unsigned le(std::size_t i) const {
        auto b = slice(i, 4);
        return b[0] | (unsigned(b[1]) << 8) | (unsigned(b[2]) << 16) | (unsigned(b[3]) << 24);
    }
    unsigned be(std::size_t i) const {
        auto b = slice(i, 4);
        return (unsigned(b[0]) << 24) | (unsigned(b[1]) << 16) | (unsigned(b[2]) << 8) | b[3];
    }
    unsigned be16(std::size_t i) const {
        auto b = slice(i, 2);
        return (unsigned(b[0]) << 8) | b[1];
    }
    bool magic(std::size_t i, std::string_view magic) const {
        auto b = slice(i, magic.size());
        return !std::memcmp(b.data(), magic.data(), magic.size());
    }
};
std::vector<std::uint8_t> decompress(bytes compressed, std::size_t capacity) {
    if (!capacity || capacity > 64 * 1024 * 1024)
        return {};
    std::vector<std::uint8_t> decoded(capacity + 1);
    auto n = lzfse_decode_buffer(decoded.data(), decoded.size(), compressed.data(), compressed.size(), nullptr);
    if (!n || n > capacity)
        return {};
    decoded.resize(n);
    return decoded;
}
// Deepmap2 prediction and YCoCg reconstruction follow the documented
// assets-parser codec (MIT; attribution in third_party/assets-parser-LICENSE).
bool deepmap(input d, bitmap &out) {
    if (!d.magic(0, "dmp2") || d.data.size() < 16 || d.data[7] != 4 || d.u16(8) != unsigned(out.width) ||
        d.u16(10) != unsigned(out.height))
        return false;
    const int type = d.data[4], scale = d.data[5] ? 2 : 1;
    const std::size_t w = out.width, h = out.height, count = w * h;
    std::size_t offset = 12, row_base = 0;
    while (offset < d.data.size() && row_base < h) {
        const auto size = d.le(offset);
        offset += 4;
        auto raw = decompress(d.slice(offset, size), count * 7 + h);
        offset += size;
        if (raw.empty())
            return false;
        if (type == 3) {
            if (raw.size() != count * 4)
                return false;
            input pixels{raw};
            for (std::size_t i = 0; i < count; ++i)
                out.argb[i] = pixels.le(i * 4);
            return true;
        }
        if (type != 2)
            return false;
        const std::size_t rows = raw.size() / (w * 7 + 1);
        if (!rows || raw.size() != rows * (w * 7 + 1))
            return false;
        const std::size_t alpha = rows * w, high = alpha + rows, low = high + alpha * 3;
        std::vector<std::int16_t> previous(w * 3), current(w * 3);
        for (std::size_t y = 0; y < rows && row_base < h; ++y, ++row_base) {
            const int predictor = raw[alpha + y];
            if (predictor > 4)
                return false;
            for (std::size_t x = 0; x < w; ++x) {
                bool use_left = false;
                if (x && predictor == 1)
                    use_left = std::abs(int(previous[x * 3]) - previous[(x - 1) * 3]) <=
                               std::abs(int(current[(x - 1) * 3]) - previous[(x - 1) * 3]);
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto k = y * w * 3 + x * 3 + c;
                    unsigned value = raw[low + k] | (unsigned(raw[high + k]) << 8);
                    int residual = (value & 1) ? -int(value >> 1) : int(value >> 1),
                        left = x ? current[(x - 1) * 3 + c] : 0, up = previous[x * 3 + c], base = 0;
                    if (predictor == 1)
                        base = x ? (use_left ? left : up) : up;
                    else if (predictor == 2)
                        base = left;
                    else if (predictor == 3)
                        base = up;
                    else if (predictor == 4)
                        base = x ? (left + up + 1) / 2 : up;
                    current[x * 3 + c] = std::int16_t(residual + base);
                }
                int yy = current[x * 3], co = current[x * 3 + 1] * scale, cg = current[x * 3 + 2] * scale,
                    temp = yy - cg / 2;
                unsigned b = std::clamp(temp + co - co / 2, 0, 255), g = std::clamp(temp + cg, 0, 255),
                         r = std::clamp(temp - co / 2, 0, 255), a = raw[y * w + x];
                out.argb[row_base * w + x] =
                    (a << 24) | (std::min(r, a) << 16) | (std::min(g, a) << 8) | std::min(b, a);
            }
            previous.swap(current);
        }
    }
    return row_base == h;
}
bitmap rendition(bytes data) {
    input d{data};
    if (data.size() < 184 || !d.magic(0, "ISTC") || !d.magic(24, "BGRA"))
        return {};
    const unsigned w = d.le(12), h = d.le(16);
    if (!w || !h || w > 4096 || h > 4096)
        return {};
    const std::size_t pixels = std::size_t(w) * h;
    bitmap out;
    out.width = w;
    out.height = h;
    out.argb.resize(pixels);
    const auto offset = 184 + std::size_t(d.le(168));
    input payload{d.slice(offset, d.le(180))};
    if (!payload.magic(0, "MLEC"))
        return {};
    const auto compression = payload.le(8);
    auto raw = payload.slice(16, payload.data.size() - 16);
    if (compression == 0) {
        if (raw.size() < pixels * 4)
            return {};
        input p{raw};
        for (std::size_t i = 0; i < pixels; ++i)
            out.argb[i] = p.le(i * 4);
        return out;
    }
    if (compression == 4 || compression == 3) {
        std::vector<std::uint8_t> decoded;
        if (raw.size() >= 20 && !std::memcmp(raw.data(), "KCBC", 4)) {
            input p{raw};
            std::size_t at = 0;
            while (at < p.data.size()) {
                if (!p.magic(at, "KCBC"))
                    return {};
                auto size = p.le(at + 16);
                auto block = decompress(p.slice(at + 20, size), pixels * 4 - decoded.size());
                if (block.empty())
                    return {};
                decoded.insert(decoded.end(), block.begin(), block.end());
                at += 20 + size;
            }
        } else
            decoded = decompress(raw, pixels * 4);
        if (decoded.size() != pixels * 4)
            return {};
        input p{decoded};
        for (std::size_t i = 0; i < pixels; ++i)
            out.argb[i] = p.le(i * 4);
        return out;
    }
    if (compression == 11 || compression == 9) {
        // Version-zero CELM carries an additional 16-byte chunk wrapper.
        std::size_t at = 0;
        if (raw.size() >= 20 && !std::memcmp(raw.data() + 16, "dmp2", 4))
            at = 16;
        if (raw.size() > at + 4 && !std::memcmp(raw.data() + at, "dmp2", 4) && deepmap({raw.subspan(at)}, out))
            return out;
    }
    return {};
}
struct bom {
    input file;
    std::vector<bytes> blocks;
    bom(bytes data) : file{data} {
        if (data.size() < 32 || !file.magic(0, "BOMStore"))
            throw std::runtime_error("catalog header");
        auto index = file.be(16), count = file.be(index);
        if (count > data.size() / 8)
            throw std::runtime_error("catalog blocks");
        for (unsigned i = 0; i < count; ++i)
            blocks.push_back(file.slice(file.be(index + 4 + i * 8), file.be(index + 8 + i * 8)));
    }
    input block(unsigned id) const {
        if (id >= blocks.size())
            throw std::runtime_error("catalog block");
        return {blocks[id]};
    }
    unsigned named(std::string_view name) const {
        std::size_t at = file.be(24);
        auto count = file.be(at);
        at += 4;
        for (unsigned i = 0; i < count; ++i) {
            unsigned id = file.be(at), size = file.slice(at + 4, 1)[0];
            auto text = file.slice(at + 5, size);
            if (std::string_view(reinterpret_cast<const char *>(text.data()), text.size()) == name)
                return id;
            at += 5 + size;
        }
        return 0;
    }
    std::vector<std::pair<bytes, bytes>> tree(std::string_view name) const {
        const auto id = named(name);
        if (!id)
            return {};
        auto t = block(id);
        if (!t.magic(0, "tree"))
            return {};
        std::vector<std::pair<bytes, bytes>> values;
        std::unordered_set<unsigned> visited;
        std::function<void(unsigned, int)> walk = [&](unsigned index, int depth) {
            if (depth > 64 || !visited.insert(index).second)
                throw std::runtime_error("catalog tree cycle");
            auto n = block(index);
            auto leaf = n.be16(0), count = n.be16(2);
            for (unsigned i = 0; i < count; ++i) {
                auto value = n.be(12 + i * 8), key = n.be(16 + i * 8);
                if (leaf)
                    values.push_back({block(key).data, block(value).data});
                else
                    walk(value, depth + 1);
            }
        };
        walk(t.be(8), 0);
        return values;
    }
};
} // namespace
bitmap read_asset_catalog(std::span<const std::uint8_t> bytes, std::string_view name, double scale) {
    try {
        bom catalog(bytes);
        auto format = catalog.block(catalog.named("KEYFORMAT"));
        const unsigned count = format.le(8);
        if (count > 64)
            return {};
        std::vector<unsigned> attributes;
        for (unsigned i = 0; i < count; ++i)
            attributes.push_back(format.le(12 + i * 4));
        unsigned identifier = 0;
        bool found = false;
        for (auto &[key, value] : catalog.tree("FACETKEYS"))
            if (std::string_view(reinterpret_cast<const char *>(key.data()), key.size()) == name) {
                input token{value};
                auto n = token.u16(4);
                for (unsigned i = 0; i < n; ++i)
                    if (token.u16(6 + i * 4) == 17) {
                        identifier = token.u16(8 + i * 4);
                        found = true;
                    }
            }
        if (!found)
            return {};
        bitmap best;
        double score = -1e30;
        for (auto &[key, value] : catalog.tree("RENDITIONS")) {
            input token{key};
            if (key.size() != count * 2)
                continue;
            unsigned identity = 0, appearance = 0;
            double rendition_scale = 1;
            for (unsigned i = 0; i < count; ++i) {
                auto v = token.u16(i * 2);
                if (attributes[i] == 17)
                    identity = v;
                if (attributes[i] == 12)
                    rendition_scale = v ? v : 1;
                if (attributes[i] == 7)
                    appearance = v;
            }
            if (identity != identifier)
                continue;
            auto image = rendition(value);
            if (!image.width)
                continue;
            double quality = -std::abs(rendition_scale - scale) * 10000 + image.width - (appearance ? 100000 : 0);
            if (quality > score) {
                best = std::move(image);
                score = quality;
            }
        }
        return best;
    } catch (const std::exception &) {
        return {};
    }
}
} // namespace evilcocoa
