#pragma once
#include "stub_abi.h"
#include "machoman/translator.h"
#include <array>
#include <cstring>
#include <stdexcept>

namespace native_abi {
/* SysV classifies small aggregates in eightbytes. The host Cocoa methods
 * use flattened Darwin arm64 operands, so decode Intel values before calling
 * them and assemble the result according to the Intel return convention. */
inline void classify(const stub_abi::type &t, std::size_t offset, std::array<char, 2> &classes) {
    if (!t.members.empty()) {
        for (const auto &member : t.members) classify(member, offset + member.offset, classes);
        return;
    }
    const char kind = t.kind == 'f' || t.kind == 'd' ? 'S' : 'I';
    for (std::size_t i = offset / 8; t.size && i <= (offset + t.size - 1) / 8 && i < 2; ++i)
        if (classes[i] != 'I') classes[i] = kind;
}
inline std::array<char, 2> classes(const stub_abi::type &t) {
    std::array<char, 2> result{};
    classify(t, 0, result);
    return result;
}

/* Host framework sends supply flattened ARM operands. Pack them into the
 * Intel guest's register/stack convention before a nested callback. */
inline std::uint64_t call_guest(mm_cpu_context &ctx, const mm_host_api &api,
                                const void *imp, const std::vector<stub_abi::type> &sig,
                                const std::uint64_t *words, unsigned word_count) {
    if (sig.size() < 3) throw std::runtime_error("missing guest Objective-C signature");
    const auto &ret = sig[0];
    std::vector<std::uint8_t> result(stub_abi::align(ret.size, 8));
    std::vector<std::uint64_t> args(6);
    unsigned gp = 0, fp = 0, input_gp = 2, input_fp = 0;
    std::uint64_t vectors[8][2]{};
    if (ret.size > 16) args[gp++] = reinterpret_cast<std::uintptr_t>(result.data());
    args[gp++] = words[0]; args[gp++] = words[1];
    std::vector<std::uint8_t> stack;
    auto input_word = [&]() {
        if (input_gp >= word_count) throw std::runtime_error("missing host Objective-C operand");
        return words[input_gp++];
    };
    for (std::size_t i = 3; i < sig.size(); ++i) {
        const auto &t = sig[i];
        std::vector<std::uint8_t> bytes(stub_abi::align(t.size, 8));
        if (t.homogeneous) {
            const std::size_t size = t.homogeneous == 'f' ? 4 : 8;
            for (std::size_t j = 0; j < t.elements; ++j) {
                if (input_fp >= 8) throw std::runtime_error("host Objective-C FP operands exceed bridge capacity");
                std::memcpy(bytes.data() + j * size, &ctx.v[input_fp++][0], size);
            }
        } else if (t.size > 16) {
            std::memcpy(bytes.data(), reinterpret_cast<const void *>(input_word()), t.size);
        } else for (std::size_t j = 0; j < bytes.size(); j += 8) {
            const auto bits = input_word();
            std::memcpy(bytes.data() + j, &bits, 8);
        }
        const auto cls = classes(t);
        unsigned ints = 0, floats = 0;
        for (char c : cls) { ints += c == 'I'; floats += c == 'S'; }
        if (t.size > 16 || gp + ints > 6 || fp + floats > 8) {
            stack.resize(stub_abi::align(stack.size(), std::max(std::size_t{8}, t.alignment)));
            stack.insert(stack.end(), bytes.begin(), bytes.end());
        } else for (unsigned j = 0; j < 2 && j * 8 < t.size; ++j) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, bytes.data() + j * 8, std::min(std::size_t{8}, t.size - j * 8));
            if (cls[j] == 'S') vectors[fp++][0] = bits;
            else args[gp++] = bits;
        }
    }
    if (stack.size() > 26 * 8) throw std::runtime_error("guest Objective-C stack exceeds bridge capacity");
    args.resize(6 + stack.size() / 8);
    if (!stack.empty()) std::memcpy(args.data() + 6, stack.data(), stack.size());
    std::memcpy(ctx.v, vectors, sizeof vectors);
    const auto value = api.call_guest(reinterpret_cast<std::uintptr_t>(imp), args.data(), static_cast<int>(args.size()));
    if (ret.kind == '{' || ret.kind == '[') {
        if (ret.size <= 16) {
            const auto cls = classes(ret);
            unsigned ints = 0, floats = 0;
            for (unsigned i = 0; i < 2 && i * 8 < ret.size; ++i) {
                const auto bits = cls[i] == 'S' ? ctx.v[floats++][0] :
                    (ints++ == 0 ? value : ctx.native_x[MM_RDX]);
                std::memcpy(result.data() + i * 8, &bits, std::min(std::size_t{8}, ret.size - i * 8));
            }
        }
        if (ret.homogeneous) {
            const std::size_t size = ret.homogeneous == 'f' ? 4 : 8;
            for (std::size_t i = 0; i < ret.elements; ++i) {
                ctx.v[i][0] = ctx.v[i][1] = 0;
                std::memcpy(&ctx.v[i][0], result.data() + i * size, size);
            }
        } else if (ret.size <= 16) {
            if (ret.size > 8) std::memcpy(&ctx.x[1], result.data() + 8, ret.size - 8);
            std::uint64_t first = 0;
            std::memcpy(&first, result.data(), std::min(std::size_t{8}, ret.size));
            return first;
        } else throw std::runtime_error("host send cannot return an indirect aggregate");
    }
    return value;
}
struct arguments {
    const mm_cpu_context &ctx;
    unsigned gp, fp = 0;
    std::uint64_t stack;
    std::vector<std::uint8_t> read(const stub_abi::type &t) {
        std::vector<std::uint8_t> bytes(stub_abi::align(t.size, 8));
        const auto cls = classes(t);
        unsigned ints = 0, floats = 0;
        for (char c : cls) { ints += c == 'I'; floats += c == 'S'; }
        if (t.size > 16 || gp + ints > 6 || fp + floats > 8) {
            stack = stub_abi::align(stack, std::max(std::size_t{8}, t.alignment));
            std::memcpy(bytes.data(), reinterpret_cast<const void *>(stack), t.size);
            stack += bytes.size();
        } else {
            constexpr int regs[] = {MM_RDI, MM_RSI, MM_RDX, MM_RCX, MM_R8, MM_R9};
            for (unsigned i = 0; i < 2 && i * 8 < t.size; ++i) {
                const auto value = cls[i] == 'S' ? ctx.native_v[fp++][0] : ctx.native_x[regs[gp++]];
                std::memcpy(bytes.data() + i * 8, &value, std::min(std::size_t{8}, t.size - i * 8));
            }
        }
        return bytes;
    }
};

inline std::uint64_t call_host(mm_cpu_context &ctx, const void *imp,
                               const std::vector<stub_abi::type> &sig,
                               std::uint64_t receiver, std::uint64_t selector,
                               std::uint64_t result_buffer) {
    arguments input{ctx, result_buffer ? 3u : 2u, 0, ctx.native_x[MM_RSP] + 8};
    /* Keep aggregate storage alive for methods taking indirect ARM operands. */
    std::vector<std::vector<std::uint8_t>> values;
    for (std::size_t i = 3; i < sig.size(); ++i) values.push_back(input.read(sig[i]));
    std::uint64_t words[6] = {receiver, selector};
    std::uint64_t vectors[8][2]{};
    std::vector<std::uint64_t> stack;
    unsigned gp = 2, fp = 0;
    auto integer = [&](std::uint64_t bits) {
        if (gp < 6) words[gp++] = bits;
        else stack.push_back(bits);
    };
    auto floating = [&](std::uint64_t bits) {
        if (fp < 8) vectors[fp++][0] = bits;
        else stack.push_back(bits);
    };
    for (std::size_t i = 3; i < sig.size(); ++i) {
        const auto &t = sig[i];
        const auto &bytes = values[i - 3];
        if (t.homogeneous) {
            const std::size_t size = t.homogeneous == 'f' ? 4 : 8;
            for (std::size_t j = 0; j < t.elements; ++j) {
                std::uint64_t bits = 0;
                std::memcpy(&bits, bytes.data() + j * size, size);
                floating(bits);
            }
        } else if (t.size > 16) integer(reinterpret_cast<std::uintptr_t>(bytes.data()));
        else for (std::size_t j = 0; j < bytes.size(); j += 8) {
            std::uint64_t bits;
            std::memcpy(&bits, bytes.data() + j, 8);
            integer(bits);
        }
    }
    if (stack.size() > 8) throw std::runtime_error("host Objective-C argument stack exceeds bridge capacity");
    stack.resize(8);
    const auto original_sp = ctx.x[31];
    const auto original_indirect = ctx.x[8];
    const auto &ret = sig[0];
    if (ret.size > 16 && !ret.homogeneous) {
        if (!result_buffer) throw std::runtime_error("Intel aggregate return requires objc_msgSend_stret");
        ctx.x[8] = result_buffer;
    }
    std::memcpy(ctx.x, words, sizeof words);
    ctx.x[6] = stack[0]; ctx.x[7] = stack[1];
    ctx.x[31] = reinterpret_cast<std::uintptr_t>(stack.data() + 2);
    std::memcpy(ctx.v, vectors, sizeof vectors);
    mm_hostcall(&ctx, imp);
    ctx.x[31] = original_sp;
    ctx.x[8] = original_indirect;
    if (ret.kind == '{' || ret.kind == '[') {
        std::vector<std::uint8_t> bytes(stub_abi::align(ret.size, 8));
        if (ret.homogeneous) {
            const std::size_t size = ret.homogeneous == 'f' ? 4 : 8;
            for (std::size_t i = 0; i < ret.elements; ++i)
                std::memcpy(bytes.data() + i * size, &ctx.v[i][0], size);
        } else if (ret.size <= 16) {
            std::memcpy(bytes.data(), &ctx.x[0], 8);
            if (ret.size > 8) std::memcpy(bytes.data() + 8, &ctx.x[1], ret.size - 8);
        } else std::memcpy(bytes.data(), reinterpret_cast<const void *>(result_buffer), ret.size);
        if (ret.size > 16) {
            if (!result_buffer) throw std::runtime_error("Intel aggregate return requires objc_msgSend_stret");
            std::memcpy(reinterpret_cast<void *>(result_buffer), bytes.data(), ret.size);
            ctx.x[0] = result_buffer;
        } else {
            const auto cls = classes(ret);
            unsigned ints = 0, floats = 0;
            std::memset(ctx.v, 0, sizeof(ctx.v[0]) * 4);
            for (unsigned i = 0; i < 2 && i * 8 < ret.size; ++i) {
                std::uint64_t bits;
                std::memcpy(&bits, bytes.data() + i * 8, 8);
                if (cls[i] == 'S') ctx.v[floats++][0] = bits;
                else if (ints++ == 0) ctx.x[0] = bits;
                else ctx.native_x[MM_RDX] = bits;
            }
        }
    }
    ctx.native_hi_valid = 1;
    ctx.host_result_in_context = 1;
    return ctx.x[0];
}
}
