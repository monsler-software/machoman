#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <evilcocoa/asset_catalog.h>
using bytes = std::vector<std::uint8_t>;
void require(bool ok, const char *s) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", s);
        std::exit(1);
    }
}
void le(bytes &b, unsigned x) {
    for (int n = 0; n < 4; ++n)
        b.push_back(x >> (n * 8));
}
void be(bytes &b, unsigned x) {
    for (int n = 3; n >= 0; --n)
        b.push_back(x >> (n * 8));
}
void u16(bytes &b, unsigned x) {
    b.push_back(x);
    b.push_back(x >> 8);
}
void tag(bytes &b, const char *s) { b.insert(b.end(), s, s + 4); }
void put(bytes &b, std::size_t i, unsigned x, bool big = false) {
    for (int n = 0; n < 4; ++n)
        b[i + n] = x >> (8 * (big ? 3 - n : n));
}
bytes lzfse(bytes raw) {
    bytes b;
    tag(b, "bvx-");
    le(b, raw.size());
    b.insert(b.end(), raw.begin(), raw.end());
    tag(b, "bvx$");
    return b;
}
bytes fixture(unsigned compression) {
    std::vector<bytes> blocks(1);
    auto add = [&](bytes b) {
        blocks.push_back(std::move(b));
        return unsigned(blocks.size() - 1);
    };
    bytes fmt;
    tag(fmt, "tmfk");
    le(fmt, 0);
    le(fmt, 2);
    le(fmt, 17);
    le(fmt, 12);
    auto format = add(fmt);
    auto name = add({'l', 'o', 'g', 'o'});
    bytes token(4);
    u16(token, 1);
    u16(token, 17);
    u16(token, 42);
    auto value = add(token);
    bytes leaf{0, 1, 0, 1};
    be(leaf, 0);
    be(leaf, 0);
    be(leaf, value);
    be(leaf, name);
    auto node = add(leaf);
    auto tree = [&](unsigned node) {
        bytes b;
        tag(b, "tree");
        be(b, 1);
        be(b, node);
        be(b, 4096);
        be(b, 1);
        return b;
    };
    auto facets = add(tree(node));
    bytes key;
    u16(key, 42);
    u16(key, 2);
    auto keyid = add(key);
    bytes rendition(184);
    std::memcpy(rendition.data(), "ISTC", 4);
    put(rendition, 4, 1);
    put(rendition, 12, 1);
    put(rendition, 16, 1);
    std::memcpy(rendition.data() + 24, "BGRA", 4);
    bytes payload;
    tag(payload, "MLEC");
    le(payload, 0);
    le(payload, compression);
    le(payload, 0);
    if (compression == 0)
        le(payload, 0x80604020);
    if (compression == 4) {
        auto stream = lzfse({0x20, 0x40, 0x60, 0x80});
        tag(payload, "KCBC");
        le(payload, 0);
        le(payload, 0);
        le(payload, 1);
        le(payload, stream.size());
        payload.insert(payload.end(), stream.begin(), stream.end());
    }
    if (compression == 11) {
        payload.resize(32);
        tag(payload, "dmp2");
        payload.insert(payload.end(), {2, 1, 10, 4});
        u16(payload, 1);
        u16(payload, 1);
        auto stream = lzfse({128, 0, 0, 0, 0, 128, 64, 0});
        le(payload, stream.size());
        payload.insert(payload.end(), stream.begin(), stream.end());
    }
    put(rendition, 180, payload.size());
    rendition.insert(rendition.end(), payload.begin(), payload.end());
    auto image = add(rendition);
    leaf = {0, 1, 0, 1};
    be(leaf, 0);
    be(leaf, 0);
    be(leaf, image);
    be(leaf, keyid);
    node = add(leaf);
    auto renditions = add(tree(node));
    bytes file(64);
    std::memcpy(file.data(), "BOMStore", 8);
    put(file, 8, 1, true);
    std::vector<std::pair<unsigned, unsigned>> ranges;
    for (auto &b : blocks) {
        ranges.push_back({file.size(), b.size()});
        file.insert(file.end(), b.begin(), b.end());
    }
    put(file, 24, file.size(), true);
    be(file, 3);
    auto named = [&](unsigned id, const char *name) {
        be(file, id);
        auto size = std::strlen(name);
        file.push_back(size);
        file.insert(file.end(), name, name + size);
    };
    named(format, "KEYFORMAT");
    named(facets, "FACETKEYS");
    named(renditions, "RENDITIONS");
    put(file, 16, file.size(), true);
    be(file, ranges.size());
    for (auto [offset, size] : ranges) {
        be(file, offset);
        be(file, size);
    }
    return file;
}
int main() {
    for (auto compression : {0, 4, 11}) {
        auto b = fixture(compression);
        auto p = evilcocoa::read_asset_catalog(b, "logo");
        require(p.width == 1 && p.height == 1 && p.argb.size() == 1, "named rendition dimensions");
        require(p.argb[0] == (compression == 11 ? 0x80204060u : 0x80604020u), "premultiplied color and alpha decoded");
        require(evilcocoa::read_asset_catalog(b, "missing").argb.empty(), "unknown asset stays empty");
        for (std::size_t i = 0; i < b.size(); ++i)
            require(evilcocoa::read_asset_catalog(std::span(b).first(i), "logo").argb.empty(),
                    "truncated catalogs rejected");
    }
    puts("CoreUI named assets, LZFSE, Deepmap2 and malformed catalogs passed");
}
