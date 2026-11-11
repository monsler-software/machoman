#ifndef MACHOMAN_SIMD_LANES_H
#define MACHOMAN_SIMD_LANES_H
#include "machoman/translator.h"
#include <array>
#include <bit>
#include <cstring>
#include <type_traits>
namespace mm_simd {
using vector = std::array<std::uint8_t, 16>;
using wide = __int128_t;
using uwide = __uint128_t;
template<unsigned Bytes> using lane_type = std::conditional_t<Bytes == 1, std::uint8_t,
    std::conditional_t<Bytes == 2, std::uint16_t, std::conditional_t<Bytes == 4, std::uint32_t, std::uint64_t>>>;
template<unsigned Bytes> lane_type<Bytes> get(const vector &v, unsigned lane) {
    lane_type<Bytes> value;
    std::memcpy(&value, v.data() + Bytes * lane, Bytes);
    return value;
}
template<unsigned Bytes> void put(vector &v, unsigned lane, wide value) {
    const auto narrowed = static_cast<lane_type<Bytes>>(value);
    std::memcpy(v.data() + Bytes * lane, &narrowed, Bytes);
}
template<unsigned Bytes> wide signed_lane(const vector &v, unsigned lane) {
    return std::bit_cast<std::make_signed_t<lane_type<Bytes>>>(get<Bytes>(v, lane));
}
template<unsigned Bytes> wide saturate(wide value, bool is_signed, bool &qc) {
    const wide min = is_signed ? -(wide{1} << (8 * Bytes - 1)) : 0;
    const wide max = (wide{1} << (8 * Bytes - (is_signed ? 1 : 0))) - 1;
    if (value < min) { qc = true; return min; }
    if (value > max) { qc = true; return max; }
    return value;
}
template<typename F> void visit_width(unsigned size, F f) {
    switch (size) {
    case 0: f.template operator()<1>(); break;
    case 1: f.template operator()<2>(); break;
    case 2: f.template operator()<4>(); break;
    case 3: f.template operator()<8>(); break;
    }
}
inline vector read(const mm_cpu_context &ctx, unsigned reg) {
    vector value;
    std::memcpy(value.data(), ctx.v[reg], 16);
    return value;
}

}
#endif
