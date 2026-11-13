#ifndef MACHOMAN_GUEST_ARGS_H
#define MACHOMAN_GUEST_ARGS_H

#include "machoman/translator.h"
#include <cstring>

/* Named stack arguments on Darwin arm64 use the type's size/alignment,
 * unlike anonymous arguments, which occupy eight-byte slots. A typed host
 * adapter uses this reader for parameters after x7. */
class mm_darwin_arm64_stack_args {
public:
    explicit mm_darwin_arm64_stack_args(const mm_cpu_context *ctx) : p_(ctx->x[31]) {}
    template <typename T> T next() {
        p_ = (p_ + alignof(T) - 1) & ~std::uint64_t{alignof(T) - 1};
        T value;
        std::memcpy(&value, reinterpret_cast<const void *>(p_), sizeof value);
        p_ += sizeof value;
        return value;
    }
private:
    std::uint64_t p_;
};

/* Darwin anonymous arguments: arm64 uses stack slots and a pointer va_list;
 * x86_64 uses the SysV register save area and overflow stack. Each reader
 * owns a snapshot, so nested callbacks and positional formatting can copy it. */
class mm_darwin_va_list {
public:
    mm_darwin_va_list(std::uint64_t base, const mm_cpu_context *ctx) : p_(base) {
        if (!ctx || !ctx->native_mode || !base) return;
        struct sysv_va_list { unsigned gp_offset, fp_offset; std::uint64_t overflow, save; } ap;
        std::memcpy(&ap, reinterpret_cast<const void *>(base), sizeof ap);
        native_ = true;
        p_ = ap.overflow;
        gp_ = ap.gp_offset / 8;
        fp_ = ap.fp_offset >= 48 ? (ap.fp_offset - 48) / 16 : 8;
        for (unsigned i = gp_; i < 6; ++i)
            std::memcpy(&gpr_[i], reinterpret_cast<const void *>(ap.save + i * 8), 8);
        for (unsigned i = fp_; i < 8; ++i)
            std::memcpy(&fpr_[i], reinterpret_cast<const void *>(ap.save + 48 + i * 16), 8);
    }

    static mm_darwin_va_list at_call_site(const mm_cpu_context *ctx,
                                         unsigned named_integer_args,
                                         unsigned named_float_args = 0) {
        mm_darwin_va_list result;
        if (!ctx) return result;
        result.p_ = ctx->x[31];
        if (!ctx->native_mode) return result;
        result.native_ = true;
        result.p_ = ctx->native_x[MM_RSP] + 8;
        result.gp_ = named_integer_args;
        result.fp_ = named_float_args;
        constexpr int args[] = {MM_RDI, MM_RSI, MM_RDX, MM_RCX, MM_R8, MM_R9};
        for (unsigned i = 0; i < 6; ++i) result.gpr_[i] = ctx->native_x[args[i]];
        for (unsigned i = 0; i < 8; ++i) result.fpr_[i] = ctx->native_v[i][0];
        return result;
    }

    std::uint64_t next_u64() {
        if (native_ && gp_ < 6) return gpr_[gp_++];
        const auto v = read(); p_ += 8; return v;
    }
    std::int64_t next_i64() { return static_cast<std::int64_t>(next_u64()); }
    std::int32_t next_i32() { return static_cast<std::int32_t>(next_u64()); }
    double next_double() {
        std::uint64_t bits;
        if (native_ && fp_ < 8) bits = fpr_[fp_++];
        else { bits = read(); p_ += 8; }
        double value; std::memcpy(&value, &bits, 8); return value;
    }
    long double next_long_double() {
        /* Darwin arm64 long double is double; Intel uses stack-passed x87
         * extended precision in a 16-byte aligned slot. */
        if (!native_) return static_cast<long double>(next_double());
        p_ = (p_ + 15) & ~std::uint64_t{15};
        long double value;
        std::memcpy(&value, reinterpret_cast<const void *>(p_), sizeof value);
        p_ += 16;
        return value;
    }
    template <typename T> T *next_ptr() { return reinterpret_cast<T *>(next_u64()); }
    std::uint64_t position() const { return p_; }

private:
    mm_darwin_va_list() = default;
    std::uint64_t read() const {
        std::uint64_t value;
        std::memcpy(&value, reinterpret_cast<const void *>(p_), 8);
        return value;
    }
    std::uint64_t p_ = 0, gpr_[6] = {}, fpr_[8] = {};
    unsigned gp_ = 6, fp_ = 8;
    bool native_ = false;
};

#endif
