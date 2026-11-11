/* Integer IEEE-754 semantics. Using a wide significand avoids host FP
 * rounding, NaN and exception differences (notably fused sqrt steps). */
#ifndef MACHOMAN_SIMD_FP_BITS_H
#define MACHOMAN_SIMD_FP_BITS_H
#include "simd_lanes.h"
#include <algorithm>
namespace mm_simd {
inline unsigned width(uwide x) {
    const auto hi = static_cast<std::uint64_t>(x >> 64), lo = static_cast<std::uint64_t>(x);
    return hi ? 128u - static_cast<unsigned>(std::countl_zero(hi)) : lo ? 64u - static_cast<unsigned>(std::countl_zero(lo)) : 0;
}
inline uwide shift_jam(uwide x, int amount) {
    if (amount <= 0) return x << -amount;
    if (amount >= 128) return x != 0;
    return (x >> amount) | ((x & ((uwide{1} << amount) - 1)) != 0);
}
template<unsigned E> struct fp_bits {
    static constexpr unsigned fraction = E == 2 ? 10 : E == 4 ? 23 : 52;
    static constexpr int bias = E == 2 ? 15 : E == 4 ? 127 : 1023;
    static constexpr std::uint64_t sign = 1ull << (8 * E - 1), frac_mask = (1ull << fraction) - 1;
    static constexpr std::uint64_t inf = std::uint64_t{2 * bias + 1} << fraction, quiet = 1ull << (fraction - 1);
    std::uint64_t control;
    std::uint64_t &flags;
    unsigned rounding() const { return static_cast<unsigned>((control >> 22) & 3); }
    bool flush() const { return (control & (1ull << (E == 2 ? 19 : 24))) != 0; }
    bool nan(std::uint64_t x) const { return (x & ~sign) > inf; }
    bool snan(std::uint64_t x) const { return nan(x) && !(x & quiet); }
    std::uint64_t default_nan() const { return inf | quiet; }
    std::uint64_t silence(std::uint64_t x) {
        if (snan(x)) flags |= 1;
        return control & (1ull << 25) ? default_nan() : x | quiet;
    }
    std::uint64_t nan_pair(std::uint64_t a, std::uint64_t b) {
        if (snan(a)) return silence(a);
        if (snan(b)) return silence(b);
        return silence(nan(a) ? a : b);
    }
    std::uint64_t input(std::uint64_t x) {
        if (flush() && !(x & inf) && (x & frac_mask)) {
            if constexpr (E != 2) flags |= 0x80; /* FZ16 does not raise IDC */
            return x & sign;
        }
        return x;
    }
    struct finite { uwide sig; int exponent; bool negative; };
    finite unpack(std::uint64_t x) const {
        const auto exp = static_cast<int>((x & inf) >> fraction);
        return {uwide{x & frac_mask} | (exp ? uwide{1} << fraction : 0), (exp ? exp : 1) - bias - static_cast<int>(fraction), bool(x & sign)};
    }
    std::uint64_t pack(bool negative, uwide mag, int exponent) {
        const auto s = negative ? sign : 0;
        if (!mag) return s;
        const int top = static_cast<int>(width(mag)) - 1 + exponent;
        const bool tiny = top < 1 - bias;
        if (tiny && flush()) { flags |= 8; return s; }
        const int shift = std::max(static_cast<int>(width(mag)) - static_cast<int>(fraction) - 1,
                                   1 - bias - static_cast<int>(fraction) - exponent);
        uwide q = shift <= 0 ? mag << -shift : shift >= 128 ? 0 : mag >> shift;
        const uwide remainder = shift <= 0 ? 0 : shift >= 128 ? mag : mag & ((uwide{1} << shift) - 1);
        if (remainder) {
            flags |= 16;
            if (tiny) flags |= 8;
            bool increment = rounding() == 1 ? !negative : rounding() == 2 ? negative : false;
            if (rounding() == 0 && shift <= 128) {
                const uwide half = uwide{1} << (shift - 1);
                increment = remainder > half || (remainder == half && (q & 1));
            }
            if (increment) q++;
        }
        int exp = exponent + shift + static_cast<int>(fraction) + bias;
        if (q >= (uwide{1} << (fraction + 1))) { q >>= 1; exp++; }
        if (exp >= 2 * bias + 1) {
            flags |= 4 | 16;
            const bool to_inf = rounding() == 0 || (rounding() == 1 && !negative) || (rounding() == 2 && negative);
            return s | (to_inf ? inf : inf - 1);
        }
        if (q < (uwide{1} << fraction)) exp = 0;
        return s | (static_cast<std::uint64_t>(exp) << fraction) | (static_cast<std::uint64_t>(q) & frac_mask);
    }
    std::uint64_t sum(finite a, finite b, int scale = 0) {
        if (!a.sig && !b.sig) return (a.negative == b.negative ? a.negative : rounding() == 2) ? sign : 0;
        const int top_a = a.sig ? static_cast<int>(width(a.sig)) - 1 + a.exponent : -10000;
        const int top_b = b.sig ? static_cast<int>(width(b.sig)) - 1 + b.exponent : -10000;
        const int common = std::max(top_a, top_b) - 120;
        const uwide am = a.sig ? shift_jam(a.sig, common - a.exponent) : 0;
        const uwide bm = b.sig ? shift_jam(b.sig, common - b.exponent) : 0;
        if (a.negative == b.negative) return pack(a.negative, am + bm, common + scale);
        if (am == bm) return rounding() == 2 ? sign : 0;
        return am > bm ? pack(a.negative, am - bm, common + scale) : pack(b.negative, bm - am, common + scale);
    }
    std::uint64_t add(std::uint64_t a, std::uint64_t b) {
        a = input(a); b = input(b);
        if (nan(a) || nan(b)) return nan_pair(a, b);
        if ((a & ~sign) == inf || (b & ~sign) == inf) {
            if ((a & ~sign) == inf && (b & ~sign) == inf && ((a ^ b) & sign)) { flags |= 1; return default_nan(); }
            return (a & ~sign) == inf ? a : b;
        }
        return sum(unpack(a), unpack(b));
    }
    std::uint64_t step(std::uint64_t a, std::uint64_t b, bool sqrt) {
        a = input(a) ^ sign; b = input(b); /* ARM flips before NaN processing */
        if (nan(a) || nan(b)) return nan_pair(a, b);
        const bool ai = (a & ~sign) == inf, bi = (b & ~sign) == inf;
        if ((ai && !(b & ~sign)) || (bi && !(a & ~sign)))
            return sqrt ? (std::uint64_t{bias} << fraction) | (1ull << (fraction - 1)) : std::uint64_t{bias + 1} << fraction;
        if (ai || bi) return ((a ^ b) & sign) | inf;
        const auto av = unpack(a), bv = unpack(b);
        return sum({av.sig * bv.sig, av.exponent + bv.exponent, av.negative != bv.negative},
                   {sqrt ? uwide{3} : uwide{2}, 0, false}, sqrt ? -1 : 0);
    }
    std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
        a = input(a); b = input(b);
        if (nan(a) || nan(b)) return nan_pair(a, b);
        const auto s = (a ^ b) & sign;
        if ((a & ~sign) == inf || (b & ~sign) == inf) {
            if (!(a & ~sign) || !(b & ~sign)) { flags |= 1; return default_nan(); }
            return s | inf;
        }
        const auto av = unpack(a), bv = unpack(b);
        return pack(bool(s), av.sig * bv.sig, av.exponent + bv.exponent);
    }
    std::uint64_t subtract(std::uint64_t a, std::uint64_t b) {
        a = input(a); b = input(b);
        if (nan(a) || nan(b)) return nan_pair(a, b);
        return add(a, b ^ sign);
    }
    std::uint64_t fused(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
        a = input(a); b = input(b); c = input(c);
        const bool invalid_product = ((a & ~sign) == inf && !(b & ~sign)) || ((b & ~sign) == inf && !(a & ~sign));
        if (invalid_product && !snan(c)) { flags |= 1; return default_nan(); }
        if (nan(a) || nan(b) || nan(c)) {
            if (snan(c)) return silence(c);
            if (snan(a)) return silence(a);
            if (snan(b)) return silence(b);
            return silence(nan(c) ? c : nan(a) ? a : b);
        }
        const auto s = (a ^ b) & sign;
        const bool infinite = (a & ~sign) == inf || (b & ~sign) == inf;
        if (infinite && (!(a & ~sign) || !(b & ~sign) || ((c & ~sign) == inf && ((c ^ s) & sign))))
            { flags |= 1; return default_nan(); }
        if (infinite) return s | inf;
        if ((c & ~sign) == inf) return c;
        const auto av = unpack(a), bv = unpack(b);
        return sum({av.sig * bv.sig, av.exponent + bv.exponent, bool(s)}, unpack(c));
    }
    std::uint64_t divide(std::uint64_t a, std::uint64_t b) {
        a = input(a); b = input(b);
        if (nan(a) || nan(b)) return nan_pair(a, b);
        const auto s = (a ^ b) & sign, aa = a & ~sign, bb = b & ~sign;
        if ((!aa && !bb) || (aa == inf && bb == inf)) { flags |= 1; return default_nan(); }
        if (aa == inf) return s | inf;
        if (bb == inf || !aa) return s;
        if (!bb) { flags |= 2; return s | inf; }
        const auto av = unpack(a), bv = unpack(b);
        const uwide scaled = av.sig << 70;
        const uwide quotient = (scaled / bv.sig) | ((scaled % bv.sig) != 0);
        return pack(bool(s), quotient, av.exponent - bv.exponent - 70);
    }
    std::uint64_t to_integer(std::uint64_t x, unsigned bits, bool is_signed) {
        x = input(x);
        if (nan(x)) { flags |= 1; return 0; }
        const auto v = unpack(x);
        const uwide limit = is_signed ? (uwide{1} << (bits - 1)) - (v.negative ? 0 : 1) : (uwide{1} << bits) - 1;
        const bool too_large = (x & ~sign) == inf || (v.sig && static_cast<int>(width(v.sig)) + v.exponent > static_cast<int>(bits + 1));
        const uwide mag = too_large ? limit + 1 : v.exponent >= 0 ? v.sig << v.exponent : -v.exponent >= 128 ? 0 : v.sig >> -v.exponent;
        if (mag > limit || (!is_signed && v.negative && mag)) {
            flags |= 1;
            return v.negative ? (is_signed ? static_cast<std::uint64_t>(-static_cast<wide>(limit)) : 0)
                              : static_cast<std::uint64_t>(limit);
        }
        if (v.exponent < 0 && (v.sig != (mag << (-v.exponent >= 128 ? 0 : -v.exponent)))) flags |= 16;
        return v.negative ? static_cast<std::uint64_t>(-static_cast<wide>(mag)) : static_cast<std::uint64_t>(mag);
    }
    unsigned relation(std::uint64_t a, std::uint64_t b, bool signaling) {
        a = input(a); b = input(b);
        if (nan(a) || nan(b)) { if (signaling || snan(a) || snan(b)) flags |= 1; return 3; }
        if (a == b || (!(a & ~sign) && !(b & ~sign))) return 6;
        const auto ordered = [](std::uint64_t x) { return x & sign ? ~x & ((sign - 1) | sign) : x | sign; };
        return ordered(a) < ordered(b) ? 8 : 2;
    }
    std::uint64_t minmax(std::uint64_t a, std::uint64_t b, bool minimum) {
        a = input(a); b = input(b);
        if (nan(a) || nan(b)) return nan_pair(a, b);
        if (!(a & ~sign) && !(b & ~sign)) return minimum ? a | b : a & b;
        const auto ordered = [](std::uint64_t x) { return x & sign ? ~x & ((sign - 1) | sign) : x | sign; };
        return (ordered(a) < ordered(b)) == minimum ? a : b;
    }
    std::uint64_t compare_abs(std::uint64_t a, std::uint64_t b, bool greater) {
        a = input(a) & ~sign; b = input(b) & ~sign;
        if (nan(a) || nan(b)) { flags |= 1; return 0; }
        return (greater ? a > b : a >= b) ? static_cast<lane_type<E>>(-1) : 0;
    }
    std::uint64_t estimate(std::uint64_t x, bool sqrt) {
        x = input(x);
        const auto s = x & sign, abs = x & ~sign;
        if (nan(x)) return silence(x);
        if (!abs) { flags |= 2; return s | inf; }
        if (sqrt && s) { flags |= 1; return default_nan(); }
        if (abs == inf) return sqrt ? 0 : s;
        int exp = static_cast<int>((x & inf) >> fraction);
        auto frac = x & frac_mask;
        if (!sqrt && abs < (1ull << (fraction - 2))) {
            flags |= 4 | 16;
            const bool to_inf = rounding() == 0 || (rounding() == 1 && !s) || (rounding() == 2 && s);
            return s | (to_inf ? inf : inf - 1);
        }
        if (!sqrt && exp >= 2 * bias - 1 && flush()) { flags |= 8; return s; }
        unsigned scaled, r;
        if (sqrt) {
            if (!exp) {
                while (!(frac & (1ull << (fraction - 1)))) { frac <<= 1; exp--; }
                frac = (frac & ((1ull << (fraction - 1)) - 1)) << 1;
            }
            scaled = exp & 1 ? 128u | static_cast<unsigned>(frac >> (fraction - 7))
                             : 256u | static_cast<unsigned>(frac >> (fraction - 8));
            const unsigned a = scaled < 256 ? 2 * scaled + 1 : 2 * ((scaled & ~1u) + 1);
            unsigned lo = 513, hi = 1024;
            while (lo < hi) { const unsigned mid = (lo + hi) / 2; if (a * mid * mid < (1u << 28)) lo = mid + 1; else hi = mid; }
            r = lo / 2;
            exp = (3 * bias - 1 - exp) / 2;
        } else {
            if (!exp) { if (!(frac & (1ull << (fraction - 1)))) { exp = -1; frac <<= 2; } else frac <<= 1; }
            scaled = 256u | static_cast<unsigned>((frac >> (fraction - 8)) & 255);
            r = (((1u << 19) / (2 * scaled + 1)) + 1) / 2;
            exp = 2 * bias - 1 - exp;
        }
        frac = std::uint64_t{r & 255} << (fraction - 8);
        if (!sqrt && exp <= 0) { frac = (frac >> (1 - exp)) | (1ull << (static_cast<int>(fraction) - 1 + exp)); exp = 0; }
        return s | (static_cast<std::uint64_t>(exp) << fraction) | frac;
    }
    template<unsigned From> std::uint64_t convert(std::uint64_t x) {
        fp_bits<From> source{control, flags};
        x = source.input(x);
        if (source.nan(x)) {
            x = source.silence(x);
            auto payload = x & source.frac_mask;
            if constexpr (source.fraction > fraction) payload >>= source.fraction - fraction;
            else payload <<= fraction - source.fraction;
            return ((x & source.sign) ? sign : 0) | inf | quiet | payload;
        }
        if ((x & ~source.sign) == source.inf) return ((x & source.sign) ? sign : 0) | inf;
        const auto value = source.unpack(x);
        return pack(value.negative, value.sig, value.exponent);
    }
};
}
#endif
