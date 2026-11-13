#include "machoman/runtime.h"
#include "machoman/macho.h"
#include "machoman/translator.h"
#include "machoman/translation_cache.h"
#include "machoman/environment.h"

#include <algorithm>
#include <atomic>
#include <xmmintrin.h>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csetjmp>
#include <csignal>
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <unistd.h>
#include <pthread.h>
#include <ucontext.h>
#endif

extern char **environ;

extern "C" std::uint64_t mm_hostcall(mm_cpu_context *ctx, const void *fn);
extern "C" void mm_native_enter(mm_cpu_context *ctx, std::uint64_t pc);
extern "C" { thread_local std::uint64_t mm_native_entry_pc = 0; }
static_assert(offsetof(mm_cpu_context, x) == 0 && offsetof(mm_cpu_context, v) == 384 &&
              offsetof(mm_cpu_context, host_result_in_context) == 904,
              "hostcall_x86_64.S hardcodes these offsets");
static_assert(offsetof(mm_cpu_context, native_x) == 912 && offsetof(mm_cpu_context, native_flags) == 1040 &&
              offsetof(mm_cpu_context, native_mode) == 1048 && offsetof(mm_cpu_context, native_v) == 1056,
              "native_x86_64.S hardcodes these offsets");
static_assert(offsetof(mm_cpu_context, native_hi_valid) == 1312);
static_assert(offsetof(mm_cpu_context, native_fp) == 1328);

namespace {

/* Prepared callbacks retain an immutable image list on their owning
 * thread, including a fault lookup that needs no global code-map mutex. */
thread_local std::span<const mm_runtime_code *const> g_callback_fault_codes;

std::vector<std::uint8_t> read_whole_file(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool trace_enabled() {
    return std::getenv("MACHOMAN_TRACE") != nullptr;
}

bool host_callable(const void *address, bool lazy) {
#if defined(__linux__)
    Dl_info info{};
    void *symbol = nullptr;
    if (dladdr1(address, &info, &symbol, RTLD_DL_SYMENT) && symbol) {
        const auto type = ELF64_ST_TYPE(static_cast<const Elf64_Sym *>(symbol)->st_info);
        return type == STT_FUNC || type == STT_GNU_IFUNC;
    }
#endif
    return lazy;
}

bool diagnostics_enabled() {
    return std::getenv("MACHOMAN_DIAGNOSTICS") != nullptr;
}

/* dyld prints every image it loads under DYLD_PRINT_LIBRARIES; our own
 * trace says the same thing, so the two share one switch. */
bool dyld_trace_enabled() {
    return trace_enabled() || std::getenv("DYLD_PRINT_LIBRARIES") != nullptr;
}

/* A colon-separated list of directories from the environment, the form
 * every DYLD_* search path uses. */
std::vector<std::string> env_path_list(const char *name, mm_environment &environment) {
    std::vector<std::string> out;
    const char *value = environment.get(name);
    if (!value) return out;
    std::string_view sv(value);
    while (!sv.empty()) {
        const auto colon = sv.find(':');
        if (const std::string_view part = sv.substr(0, colon); !part.empty())
            out.emplace_back(part);
        if (colon == std::string_view::npos) break;
        sv.remove_prefix(colon + 1);
    }
    return out;
}

/* The part of an install name a framework search path is joined to:
 * "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit" is looked
 * up as "AppKit.framework/Versions/C/AppKit" under each DYLD_FRAMEWORK_PATH
 * entry. Empty when the name is not a framework. */
std::string framework_partial_path(std::string_view install_name) {
    const auto at = install_name.find(".framework/");
    if (at == std::string_view::npos) return {};
    const auto slash = install_name.rfind('/', at);
    return std::string(install_name.substr(slash == std::string_view::npos ? 0 : slash + 1));
}

/* Segments mapped at vmaddr + slide for the lifetime of the object. The
 * executable maps at its fixed addresses (slide 0); a dylib's segments are
 * laid out contiguously at whatever base the linker picked. */
class image_mapping {
public:
    bool map(const mm_macho_image &img, std::uint64_t slide) {
        for (const auto *seg : img.segments) {
            if (seg->vmsize == 0 || mm_macho_name(seg->segname) == "__PAGEZERO")
                continue;
            void *want = reinterpret_cast<void *>(seg->vmaddr + slide);
#if defined(_WIN32)
            void *got = VirtualAlloc(want, seg->vmsize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!got) return false;
#else
            const int executable = img.header->cputype == MM_CPU_TYPE_X86_64 && (seg->initprot & 4) ? PROT_EXEC : 0;
            void *got = mmap(want, seg->vmsize, PROT_READ | PROT_WRITE | executable,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (got == MAP_FAILED || got != want) {
                if (got != MAP_FAILED) munmap(got, seg->vmsize);
                return false;
            }
#endif
            regions_.push_back({got, seg->vmsize});
            const std::uint64_t n = std::min(seg->filesize, seg->vmsize);
            if (seg->fileoff + n > img.data.size())
                return false;
            std::memcpy(got, img.data.data() + seg->fileoff, n);
        }
        return true;
    }
    ~image_mapping() {
        for (auto &[p, size] : regions_) {
#if defined(_WIN32)
            VirtualFree(p, 0, MEM_RELEASE);
#else
            munmap(p, size);
#endif
        }
    }

private:
    std::vector<std::pair<void *, std::size_t>> regions_;
};

/* Process-wide code map walked by the dispatch stub (layout in
 * translator.h). L2 pages are allocated on first use and never freed;
 * entries are cleared when their mm_runtime_code goes away. */
class code_map {
public:
    static code_map &instance() {
        static code_map m;
        return m;
    }
    const std::uint64_t *l1() const { return l1_; }

    void set(std::uint64_t va, std::uintptr_t host) {
        if (!l1_ || va >> MM_CODE_MAP_VA_BITS) return;
        std::lock_guard lock(mu_);
        std::uint64_t &slot = l1_[va >> MM_CODE_MAP_PAGE_BITS];
        if (!slot) {
            if (!host) return;
            slot = reinterpret_cast<std::uintptr_t>(new std::uint64_t[MM_CODE_MAP_L2_ENTRIES]());
        }
        reinterpret_cast<std::uint64_t *>(slot)[(va & ((1u << MM_CODE_MAP_PAGE_BITS) - 1)) >> 2] = host;
    }
    std::uintptr_t lookup(std::uint64_t va) const {
        if (!l1_ || (va >> MM_CODE_MAP_VA_BITS) || (va & 3)) return 0;
        const auto *page = reinterpret_cast<const std::uint64_t *>(l1_[va >> MM_CODE_MAP_PAGE_BITS]);
        return page ? page[(va & ((1u << MM_CODE_MAP_PAGE_BITS) - 1)) >> 2] : 0;
    }

private:
    code_map() {
        const std::size_t bytes = MM_CODE_MAP_L1_ENTRIES * sizeof(std::uint64_t);
#if defined(_WIN32)
        l1_ = static_cast<std::uint64_t *>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
        void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        l1_ = p == MAP_FAILED ? nullptr : static_cast<std::uint64_t *>(p);
#endif
    }
    std::uint64_t *l1_ = nullptr;
    std::mutex mu_;
};

/* Live translations, for host rip -> guest PC mapping across images. */
std::mutex g_codes_mu;
std::vector<const mm_runtime_code *> g_codes;

/* One PROT_NONE page range whose addresses stand in for imports. A bind
 * slot filled with such an address makes any call or dereference through
 * it fault in a recognisable way, so the runtime can name the import. One
 * page per import, so that a field access into an unresolved object (an
 * objc class, a struct) still maps back to the right symbol. */
class import_traps {
public:
    bool reserve(std::size_t count) {
        count_ = count;
        size_ = ((count * stride + 4095) / 4096) * 4096;
        if (size_ == 0) return true;
#if defined(_WIN32)
        base_ = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, size_, MEM_RESERVE, PAGE_NOACCESS));
#else
        void *p = mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base_ = p == MAP_FAILED ? nullptr : static_cast<std::uint8_t *>(p);
#endif
        return base_ != nullptr;
    }
    ~import_traps() {
        if (!base_) return;
#if defined(_WIN32)
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        munmap(base_, size_);
#endif
    }
    std::uint64_t va(std::size_t index) const { return reinterpret_cast<std::uintptr_t>(base_) + index * stride; }
    /* Index of the import a VA belongs to, or -1. */
    std::ptrdiff_t index_of(std::uint64_t addr) const {
        const auto b = reinterpret_cast<std::uintptr_t>(base_);
        if (!base_ || addr < b || addr >= b + count_ * stride) return -1;
        return static_cast<std::ptrdiff_t>((addr - b) / stride);
    }

private:
    static constexpr std::size_t stride = 4096;
    std::uint8_t *base_ = nullptr;
    std::size_t size_ = 0, count_ = 0;
};

#if !defined(_WIN32)
/* Guest memory faults land here. The translated code never holds host
 * state that needs unwinding (no host stack frames, flags saved in the
 * context), so longjmp back into mm_runtime_code::run is safe. */
struct fault_guard {
    sigjmp_buf jump;
    std::uintptr_t rip = 0;
    std::uintptr_t addr = 0;
    int signum = SIGSEGV, code = SEGV_MAPERR;
    mm_cpu_context *native = nullptr;
};
thread_local fault_guard *g_guard = nullptr;
thread_local unsigned g_fault_scope_depth = 0;
std::mutex g_fault_handlers_mu;
unsigned g_fault_handler_users = 0;
constexpr int fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
struct sigaction g_previous_fault[std::size(fault_signals)]{};

void on_fault(int signum, siginfo_t *info, void *uctx) {
    fault_guard *g = g_guard;
    if (!g) {
        /* A host fault outside translated code belongs to the handler that
         * was installed before this runtime entered its execution scope. */
        std::size_t index = 0;
        while (fault_signals[index] != signum) ++index;
        const auto &previous = g_previous_fault[index];
        if (previous.sa_handler == SIG_IGN) return;
        if (previous.sa_handler == SIG_DFL) {
            sigaction(signum, &previous, nullptr);
            raise(signum);
        } else if (previous.sa_flags & SA_SIGINFO) previous.sa_sigaction(signum, info, uctx);
        else previous.sa_handler(signum);
        return;
    }
    auto *uc = static_cast<ucontext_t *>(uctx);
    g->rip = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    g->addr = reinterpret_cast<std::uintptr_t>(info->si_addr);
    g->signum = signum;
    g->code = info->si_code;
    if (g->native) {
        constexpr int registers[] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                     REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
        for (int i = 0; i < 16; ++i)
            g->native->native_x[i] = static_cast<std::uint64_t>(uc->uc_mcontext.gregs[registers[i]]);
        g->native->native_flags = static_cast<std::uint64_t>(uc->uc_mcontext.gregs[REG_EFL]);
        g->native->native_trapno = static_cast<std::uint32_t>(uc->uc_mcontext.gregs[REG_TRAPNO]);
        g->native->native_error = static_cast<std::uint32_t>(uc->uc_mcontext.gregs[REG_ERR]);
        if (uc->uc_mcontext.fpregs) {
            std::memcpy(g->native->native_fp, uc->uc_mcontext.fpregs, sizeof(g->native->native_fp));
            std::memcpy(g->native->native_v, uc->uc_mcontext.fpregs->_xmm, sizeof(g->native->native_v));
        }
    }
    siglongjmp(g->jump, 1);
}

/* A native guest may switch to a fiber's stack. Probe those stack words
 * under a fault guard; process_vm_readv is not available in every sandbox.
 * Only memcpy runs between setjmp and longjmp, with no host state to unwind. */
bool guarded_copy(void *destination, const void *source, std::size_t size) {
    fault_guard guard;
    auto *previous = g_guard;
    g_guard = &guard;
    const bool ok = sigsetjmp(guard.jump, 0) == 0;
    if (ok) std::memcpy(destination, source, size);
    g_guard = previous;
    return ok;
}

/* Signal dispositions are process-wide; the jump target is thread-local.
 * Keep the handlers installed across host calls and nested guest callbacks.
 * Only the first active thread installs them and the last restores them. */
class fault_handler_scope {
public:
    fault_handler_scope() {
        if (g_fault_scope_depth++ != 0) return;
        /* Native guests can use small fiber stacks. A Linux signal frame
         * plus on_fault's C++ frame must never grow into guest allocations.
         * Each host thread owns a persistent, prefaulted recovery stack;
         * nested runs share it and the outer scope restores the host setup. */
        static thread_local std::vector<std::uint8_t> recovery_stack(128 * 1024);
        if (sigaltstack(nullptr, &previous_stack_) != 0) std::abort();
        if (!(previous_stack_.ss_flags & SS_ONSTACK)) {
            stack_t stack{};
            stack.ss_sp = recovery_stack.data();
            stack.ss_size = recovery_stack.size();
            if (sigaltstack(&stack, nullptr) != 0) std::abort();
            installed_stack_ = true;
        }
        std::lock_guard lock(g_fault_handlers_mu);
        if (g_fault_handler_users++ != 0) return;
        struct sigaction sa{};
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (std::size_t i = 0; i < std::size(fault_signals); ++i)
            sigaction(fault_signals[i], &sa, &g_previous_fault[i]);
    }
    ~fault_handler_scope() {
        if (--g_fault_scope_depth != 0) return;
        if (installed_stack_ && sigaltstack(&previous_stack_, nullptr) != 0) std::abort();
        std::lock_guard lock(g_fault_handlers_mu);
        if (--g_fault_handler_users != 0) return;
        for (std::size_t i = 0; i < std::size(fault_signals); ++i)
            sigaction(fault_signals[i], &g_previous_fault[i], nullptr);
    }
    fault_handler_scope(const fault_handler_scope &) = delete;
    fault_handler_scope &operator=(const fault_handler_scope &) = delete;
private:
    stack_t previous_stack_{};
    bool installed_stack_ = false;
};
#endif

mm_runtime_status status_from_exit(mm_exit_reason r) {
    switch (r) {
    case mm_exit_reason::returned:     return mm_runtime_status::ok;
    case mm_exit_reason::unsupported:  return mm_runtime_status::unsupported_insn;
    case mm_exit_reason::bad_target:   return mm_runtime_status::bad_target;
    case mm_exit_reason::syscall:      return mm_runtime_status::syscall;
    case mm_exit_reason::brk:          return mm_runtime_status::brk;
    case mm_exit_reason::memory_fault: return mm_runtime_status::memory_fault;
    case mm_exit_reason::exited:       return mm_runtime_status::exited;
    case mm_exit_reason::host_abort:   return mm_runtime_status::host_abort;
    case mm_exit_reason::running:      break;
    }
    return mm_runtime_status::bad_image;
}

/* Host-side entry stub, placed in front of the translated code:
 *   push rbx/rbp/r12-r15      preserve everything the SysV/Win64 ABIs require
 *   mov  r15, rdi             guest context (first C argument)
 *   mov  rax, [r15+entry_va]
 *   call <dispatch stub>      jumps to the translation of rax; guest exits `ret` back here
 *   pop  ...; ret
 * Translated code only touches rax/rcx/rdx and r15, so this is the whole
 * ABI bridge. */
mm_x64_buf build_trampoline(std::size_t dispatch_stub_offset) {
    mm_x64_buf t;
    for (int r : {MM_RBX, MM_RBP, MM_R12, MM_R13, MM_R14, MM_R15})
        mm_x64_emit_push(t, r);
    mm_x64_emit_mov_reg(t, MM_R15, MM_RDI);
    mm_x64_emit_mov_load(t, MM_RAX, MM_R15, offsetof(mm_cpu_context, entry_va));

    const std::size_t call_at = t.size();
    mm_x64_emit_call_rel32(t, 0);
    const std::size_t call_end = t.size();

    for (int r : {MM_R15, MM_R14, MM_R13, MM_R12, MM_RBP, MM_RBX})
        mm_x64_emit_pop(t, r);
    mm_x64_emit_ret(t);

    const auto rel = static_cast<std::int32_t>(t.size() + dispatch_stub_offset - call_end);
    std::memcpy(t.bytes.data() + call_at + 1, &rel, 4);
    return t;
}

} // namespace

mm_runtime_code::mm_runtime_code(const mm_translation_result &result) : result_(result) {
    const mm_x64_buf trampoline = build_trampoline(result.dispatch_stub);
    mem_size_ = trampoline.size() + result.code.size();
#if defined(_WIN32)
    mem_ = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, mem_size_, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#else
    void *m = mmap(nullptr, mem_size_, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mem_ = m == MAP_FAILED ? nullptr : static_cast<std::uint8_t *>(m);
#endif
    if (!mem_)
        return;
    std::memcpy(mem_, trampoline.bytes.data(), trampoline.size());
    code_ = mem_ + trampoline.size();
    std::memcpy(code_, result.code.bytes.data(), result.code.size());

    code_map &map = code_map::instance();
    for (const auto &n : result.notes) {
        const auto entry = n.entry_offset ? n.entry_offset : n.output_offset;
        map.set(result.text_base + n.source_offset, reinterpret_cast<std::uintptr_t>(code_ + entry));
        if (entry != n.output_offset) entry_ranges_.emplace_back(entry, n.source_offset);
    }
    std::lock_guard lock(g_codes_mu);
    g_codes.push_back(this);
}

mm_runtime_code::~mm_runtime_code() {
    if (!mem_) return;
    {
        std::lock_guard lock(g_codes_mu);
        std::erase(g_codes, this);
    }
    code_map &map = code_map::instance();
    for (const auto &n : result_.notes)
        map.set(result_.text_base + n.source_offset, 0);
#if defined(_WIN32)
    VirtualFree(mem_, 0, MEM_RELEASE);
#else
    munmap(mem_, mem_size_);
#endif
}

std::uint64_t mm_runtime_code::guest_pc(std::uintptr_t host_rip) const {
    if (!code_ || host_rip < reinterpret_cast<std::uintptr_t>(code_) || host_rip >= reinterpret_cast<std::uintptr_t>(mem_) + mem_size_)
        return 0;
    const std::uint64_t off = host_rip - reinterpret_cast<std::uintptr_t>(code_);
    if (!entry_ranges_.empty() && off >= entry_ranges_.front().first) {
        auto it = std::upper_bound(entry_ranges_.begin(), entry_ranges_.end(), off,
            [](std::uint64_t offset, const auto &entry) { return offset < entry.first; });
        return result_.text_base + std::prev(it)->second;
    }
    const auto &notes = result_.notes;
    auto it = std::upper_bound(notes.begin(), notes.end(), off,
                               [](std::uint64_t o, const mm_translate_note &n) { return o < n.output_offset; });
    if (it == notes.begin())
        return 0; /* inside the shared stubs */
    --it;
    return result_.text_base + it->source_offset;
}

std::uint64_t mm_runtime_code::guest_pc_anywhere(std::uintptr_t host_rip) {
    std::lock_guard lock(g_codes_mu);
    for (const mm_runtime_code *c : g_codes)
        if (const std::uint64_t pc = c->guest_pc(host_rip))
            return pc;
    return 0;
}

std::uintptr_t mm_runtime_code::translation_of(std::uint64_t va) { return code_map::instance().lookup(va); }

mm_runtime_status mm_runtime_code::run(mm_cpu_context &ctx, std::uint64_t entry_va) {
    if (!mem_)
        return mm_runtime_status::exec_alloc_failed;
    /* A nested run (host code calling back into the guest) shares the outer
     * run's context, whose translated code still walks the same process-wide
     * table — save and restore the outer value rather than clearing it. */
    const std::uint64_t *const outer_branch_table = ctx.branch_table;
    ctx.branch_table = code_map::instance().l1();
    ctx.entry_va = entry_va;
    ctx.exit_reason = static_cast<std::uint64_t>(mm_exit_reason::running);

    using entry_fn = void (*)(mm_cpu_context *);
#if defined(_WIN32)
    reinterpret_cast<entry_fn>(mem_)(&ctx);
#else
    fault_handler_scope handlers;
    fault_guard guard;
    fault_guard *prev = g_guard;
    g_guard = &guard;
    /* on_fault uses SA_NODEFER and an empty mask, so fault delivery does
     * not change the thread's signal mask. Saving it here would add a
     * sigprocmask syscall at every guest/host transition. */
    if (sigsetjmp(guard.jump, 0) == 0) {
        reinterpret_cast<entry_fn>(mem_)(&ctx);
    } else {
#ifdef MM_DBG_DUMP
        std::fprintf(stderr, "FAULT rip=%llx\n", (unsigned long long)guard.rip);
#endif
        ctx.exit_reason = static_cast<std::uint64_t>(mm_exit_reason::memory_fault);
        if (g_callback_fault_codes.empty()) {
            ctx.fault_va = guest_pc_anywhere(guard.rip);
        } else {
            ctx.fault_va = 0;
            for (const auto *code : g_callback_fault_codes)
                if ((ctx.fault_va = code->guest_pc(guard.rip))) break;
        }
        ctx.fault_addr = guard.addr;
    }
    g_guard = prev;
#endif

    ctx.branch_table = outer_branch_table;
    return status_from_exit(static_cast<mm_exit_reason>(ctx.exit_reason));
}

mm_runtime_status mm_runtime_exec(const mm_translation_result &result, std::uint64_t entry_va, mm_cpu_context &ctx) {
    mm_runtime_code code(result);
    return code.run(ctx, entry_va);
}

/* ---- dynamic linking ---------------------------------------------------- */

namespace {

struct host_lib {
    std::string install_name;
    std::filesystem::path path;
    void *handle = nullptr;
};

struct guest_image;

/* What one LC_LOAD_DYLIB of an image resolved to: a guest Mach-O image, a
 * host shared object, or nothing (weak or missing). */
struct dependency {
    guest_image *guest = nullptr;
    host_lib *host = nullptr;
};

struct bound_import {
    mm_bind bind;
    guest_image *image = nullptr; /* the image whose slot this is */
    void *host = nullptr;         /* resolved host address, or null */
    std::uint64_t guest = 0;      /* resolved guest address (slid), or 0 */
    bool preserve_regs = false;   /* special ABI: restore x0-x18, v0-v7 after the call */
    bool returns_this = false;    /* C++ ctor/dtor: the ARM C++ ABI returns `this` in x0, x86-64 returns void */
    std::string name_storage;     /* NUL-terminated copy of the symbol for host API consumers */
    bool resolved() const { return host || guest; }
};

/* A loaded Mach-O image: the executable (slide 0) or a dylib. */
struct guest_image {
    std::string install_name;
    std::filesystem::path path;
    std::vector<std::uint8_t> file;
    // Expanded in the declaring image's context, nearest loader first.
    std::vector<std::filesystem::path> runpaths;
    mm_macho_image img;               /* aliases `file` */
    std::uint64_t slide = 0;
    image_mapping mapping;
    mm_macho_image::code_range code;  /* addr is slid */
    std::vector<dependency> deps;     /* index = dylib ordinal - 1 */
    std::unordered_map<std::string, std::uint64_t> exports; /* name -> slid address */
    std::vector<mm_export> reexports;
    mm_translation_result result;
    std::unique_ptr<mm_runtime_code> runtime;
    bool initialized = false;

    std::uint64_t header_va() const {
        for (const auto *seg : img.segments)
            if (mm_macho_name(seg->segname) == "__TEXT")
                return seg->vmaddr + slide;
        return 0;
    }
    bool contains(std::uint64_t va) const {
        for (const auto *seg : img.segments)
            if (seg->vmsize && mm_macho_name(seg->segname) != "__PAGEZERO" && va >= seg->vmaddr + slide &&
                va < seg->vmaddr + slide + seg->vmsize)
                return true;
        return false;
    }
};

/* The ARM Itanium C++ ABI variant makes constructors and destructors
 * return `this`, and Apple's compiler relies on it (x0 is reused as the
 * object pointer after the call). Host x86-64 ctors return void, so the run
 * loop restores x0 for symbols whose mangled name ends a nested-name with a
 * C1/C2/C3 or D0/D1/D2 token: "__ZN<...>C1E<params>" / "...C1I<targs>E...". */
/* Strips a Darwin symbol variant suffix ("_realpath$DARWIN_EXTSN",
 * "$UNIX2003", "$INODE64"). Objective-C metadata symbols also contain a
 * '$', as in "_OBJC_CLASS_$_NSView", and must be kept whole: a variant
 * suffix never starts with an underscore. */
std::string strip_variant(std::string_view symbol) {
    const auto at = symbol.find('$');
    if (at == std::string_view::npos || at + 1 >= symbol.size() || symbol[at + 1] == '_')
        return std::string(symbol);
    return std::string(symbol.substr(0, at));
}

bool is_ctor_or_dtor(std::string_view symbol) {
    if (!symbol.starts_with("__ZN"))
        return false;
    for (std::size_t i = 4; i + 2 < symbol.size(); i++) {
        const char k = symbol[i], n = symbol[i + 1], e = symbol[i + 2];
        const bool token = (k == 'C' && (n == '1' || n == '2' || n == '3')) || (k == 'D' && (n == '0' || n == '1' || n == '2'));
        if (token && (e == 'E' || e == 'I') && !std::isdigit(static_cast<unsigned char>(symbol[i - 1])))
            return true;
    }
    return false;
}

/* Loads images and resolves symbols; see the linking notes in runtime.h. */
class linker {
public:
    linker(const mm_venv &venv, std::filesystem::path exe_dir, mm_environment &environment)
        : venv_(venv), exe_dir_(std::move(exe_dir)), environment_(environment) {}
    ~linker() {
        /* Images first (their translations reference nothing in host libs), then host libs. */
        images_.clear();
#if !defined(_WIN32)
        for (auto &l : host_libs_)
            if (l->handle) dlclose(l->handle);
#endif
    }

    std::vector<std::unique_ptr<guest_image>> &images() { return images_; }
    std::vector<std::unique_ptr<host_lib>> &host_libs() { return host_libs_; }

    /* A host DSO can hide its dependency's lifecycle hook by defining the
     * same name. Include the already-loaded ELF dependency closure so, for
     * example, a C program using blocks initializes libobjc even without
     * an explicit Mach-O libobjc load command. NOLOAD never loads new code. */
    void include_host_dependencies() {
#if !defined(_WIN32)
        for (std::size_t index = 0; index < host_libs_.size(); ++index) {
            if (!host_libs_[index]->handle) continue;
            link_map *map = nullptr;
            if (dlinfo(host_libs_[index]->handle, RTLD_DI_LINKMAP, &map) != 0 || !map || !map->l_ld) continue;
            const char *strings = nullptr;
            for (auto *entry = map->l_ld; entry->d_tag != DT_NULL; ++entry)
                if (entry->d_tag == DT_STRTAB) strings = reinterpret_cast<const char *>(entry->d_un.d_ptr);
            if (!strings) continue;
            for (auto *entry = map->l_ld; entry->d_tag != DT_NULL; ++entry) {
                if (entry->d_tag != DT_NEEDED) continue;
                const char *name = strings + entry->d_un.d_val;
                void *handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
                if (!handle) continue;
                const bool known = std::any_of(host_libs_.begin(), host_libs_.end(),
                    [&](const auto &library) { return library->handle == handle; });
                if (known) { dlclose(handle); continue; }
                link_map *dependency_map = nullptr;
                if (dlinfo(handle, RTLD_DI_LINKMAP, &dependency_map) != 0 || !dependency_map) {
                    dlclose(handle); continue;
                }
                auto library = std::make_unique<host_lib>();
                library->install_name = name;
                library->path = dependency_map->l_name;
                library->handle = handle;
                host_libs_.push_back(std::move(library));
            }
        }
#endif
    }

    /* Maps the executable at its fixed addresses and loads its dependency
     * closure. Returns null on failure (details on stderr). */
    guest_image *load_executable(const std::filesystem::path &path, std::vector<std::uint8_t> file, const mm_macho_image &img) {
        auto image = std::make_unique<guest_image>();
        image->path = path;
        image->install_name = path.string();
        image->file = std::move(file);
        image->img = img;
        image->img.data = image->file; /* the parse aliased the caller's buffer; re-alias ours */
        if (mm_macho_parse(image->file, image->img) != mm_macho_status::ok ||
            (image->img.header->cputype != MM_CPU_TYPE_ARM64 && image->img.header->cputype != MM_CPU_TYPE_X86_64))
            return nullptr;
        image->slide = 0;
        if (!image->mapping.map(image->img, 0)) {
            std::fprintf(stderr, "dyld: cannot map %s at its vmaddr\n", path.c_str());
            return nullptr;
        }
        guest_image *raw = image.get();
        images_.push_back(std::move(image));
        finish_image(*raw);
        /* DYLD_INSERT_LIBRARIES: libraries loaded into the process although
         * nothing links against them, the way a profiler or a shim is
         * injected on macOS. They become dependencies of the executable, so
         * their initializers run before its own and their exports take part
         * in symbol lookup. */
        for (const auto &inserted : env_path_list("DYLD_INSERT_LIBRARIES", environment_)) {
            const dependency d = load_dependency(inserted, *raw);
            if (!d.guest && !d.host)
                std::fprintf(stderr, "dyld: could not insert %s\n", inserted.c_str());
            else
                raw->deps.push_back(d);
        }
        return raw;
    }

    /* Loads (or finds) the dylib with `install_name` as referenced by
     * `referrer`. Null if nothing provides it. */
    dependency load_dependency(std::string_view install_name, const guest_image &referrer) {
        // Token paths can name different libraries from different referrers.
        // Resolve them before reusing an image by its filesystem identity.
        if (!install_name.starts_with('@')) {
            for (auto &g : images_)
                if (g->install_name == install_name) return {g.get(), nullptr};
            for (auto &h : host_libs_)
                if (h->install_name == install_name) return {nullptr, h.get()};
        }

        const auto [host_path, guest_path] = locate(install_name, referrer);
        const auto &selected = host_path.empty() ? guest_path : host_path;
        if (!selected.empty()) {
            std::error_code ec;
            const auto identity = std::filesystem::weakly_canonical(selected, ec);
            for (auto &g : images_)
                if (g->path == identity) return {g.get(), nullptr};
            for (auto &h : host_libs_)
                if (h->path == identity) return {nullptr, h.get()};
        }
        if (!host_path.empty()) {
            auto lib = std::make_unique<host_lib>();
            lib->install_name = std::string(install_name);
            std::error_code ec;
            lib->path = std::filesystem::weakly_canonical(host_path, ec);
#if !defined(_WIN32)
            lib->handle = dlopen(host_path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!lib->handle)
                std::fprintf(stderr, "dyld: %s: %s\n", host_path.c_str(), dlerror());
#endif
            if (dyld_trace_enabled())
                std::fprintf(stderr, "dyld: %s -> %s (host)\n", std::string(install_name).c_str(), host_path.c_str());
            host_libs_.push_back(std::move(lib));
            return {nullptr, host_libs_.back().get()};
        }
        if (!guest_path.empty()) {
            if (guest_image *g = load_dylib(install_name, guest_path, referrer.runpaths))
                return {g, nullptr};
        }
        if (dyld_trace_enabled())
            std::fprintf(stderr, "dyld: %s -> not found\n", std::string(install_name).c_str());
        return {};
    }

    /* Address of `symbol` (a bind name, "$suffix" stripped) as seen from
     * `image` through dylib `ordinal`. Guest definitions win within the
     * named library; otherwise every image and host library is searched,
     * since umbrella frameworks re-export what other libraries define. */
    void lookup(guest_image &image, int ordinal, std::string_view symbol, bool lazy, std::uint64_t &guest, void *&host,
                bool *from_resolver = nullptr) {
        guest = 0;
        host = nullptr;
        if (from_resolver) *from_resolver = false;
        const std::string name = strip_variant(symbol);
        if (ordinal >= 1 && static_cast<std::size_t>(ordinal) <= image.deps.size()) {
            const dependency &d = image.deps[static_cast<std::size_t>(ordinal) - 1];
            std::vector<const guest_image *> seen;
            if (d.guest && (guest = guest_lookup(*d.guest, name, seen))) return;
            if (d.host) {
                void *h = host_lookup(*d.host, name);
                /* dlsym also searches the library's dependencies, so a
                 * symbol can come back from the host's own libgcc or
                 * libstdc++. When it does, a guest image that exports the
                 * same name is the real definition: on macOS that symbol
                 * would come from the Darwin library the guest loaded, and
                 * the C++ ABI in particular (`_Unwind_*`, `__cxa_*`) must
                 * stay inside the guest — an exception unwound by the
                 * host's unwinder walks a stack it cannot read and aborts. */
                if (h && !defined_in(*d.host, h)) {
                    for (auto &g : images_)
                        if (auto it = g->exports.find(name); it != g->exports.end()) {
                            guest = it->second;
                            return;
                        }
                }
                if (h) { host = h; return; }
            }
            /* A host library may synthesise symbols it does not define
             * statically (a framework stub deriving constants and stub
             * functions from the name). Only the library the bind actually
             * names is asked, so a stub never shadows another library. */
            if (d.host && (host = host_synthesise(*d.host, name, lazy))) {
                if (from_resolver) *from_resolver = true;
                return;
            }
        } else if (ordinal == 0) { /* BIND_SPECIAL_DYLIB_SELF */
            std::vector<const guest_image *> seen;
            if ((guest = guest_lookup(image, name, seen))) return;
        } else if (ordinal == -1 && !images_.empty()) { /* MAIN_EXECUTABLE */
            std::vector<const guest_image *> seen;
            if ((guest = guest_lookup(*images_[0], name, seen))) return;
        }
        for (auto &g : images_)
            if (auto it = g->exports.find(name); it != g->exports.end()) { guest = it->second; return; }
        for (auto &h : host_libs_)
            if ((host = host_lookup(*h, name))) return;
        /* Last: a host library may synthesise a symbol nothing defines (a
         * framework stub deriving a constant or a reporting function from
         * the name). Only the library the bind names is asked, and only
         * after every real definition has been ruled out — an umbrella
         * framework re-exports what its members define, so CoreServices
         * naming `_exp2` must reach libc rather than a stub. */
        if (ordinal >= 1 && static_cast<std::size_t>(ordinal) <= image.deps.size())
            if (const dependency &d = image.deps[static_cast<std::size_t>(ordinal) - 1]; d.host)
                if ((host = host_synthesise(*d.host, name, lazy)) && from_resolver) *from_resolver = true;
    }

    bool preserves_registers(std::string_view symbol) {
#if defined(_WIN32)
        (void)symbol;
        return false;
#else
        const std::string darwin = strip_variant(symbol);
        for (auto &h : host_libs_) {
            if (!h->handle) continue;
            const char *list = static_cast<const char *>(dlsym(h->handle, "machoman_preserve_regs"));
            for (const char *p = list; p && *p; p += std::strlen(p) + 1)
                if (darwin == p) return true;
        }
        return false;
#endif
    }

    guest_image *image_for(std::uint64_t va) {
        for (auto &g : images_)
            if (g->contains(va)) return g.get();
        return nullptr;
    }

private:
    static bool path_token(std::string_view path, std::string_view token) {
        return path == token || (path.starts_with(token) && path.size() > token.size() && path[token.size()] == '/');
    }
    std::vector<std::filesystem::path> expand_path(std::string_view p, const guest_image &referrer) const {
        namespace fs=std::filesystem;
        auto suffix = [&](std::string_view token) { return std::string(p.size()==token.size()?std::string_view{}:p.substr(token.size()+1)); };
        if (path_token(p,"@executable_path"))
            return {exe_dir_/suffix("@executable_path"),fs::path(std::string(MM_VENV_DIR_BIN))/suffix("@executable_path")};
        if (path_token(p,"@loader_path")) return {referrer.path.parent_path()/suffix("@loader_path")};
        if (p.starts_with('@')) return {};
        return {fs::path(std::string(p.starts_with('/')?p.substr(1):p))};
    }
    /* Search in candidate order; a later host shim must not override an
     * earlier guest dylib selected by the image's runpath stack. */
    std::pair<std::filesystem::path, std::filesystem::path> locate(std::string_view install_name, const guest_image &referrer) const {
        namespace fs = std::filesystem;
        std::vector<fs::path> candidates; /* absolute, or venv-relative */
        auto strip_root = [](std::string_view p) { return p.starts_with('/') ? p.substr(1) : p; };
        constexpr std::string_view rpath = "@rpath/";
        if (install_name.starts_with(rpath)) {
            const std::string rest(install_name.substr(rpath.size()));
            for (const auto &base : referrer.runpaths) candidates.push_back(base / rest);
            candidates.push_back(fs::path(std::string(MM_VENV_DIR_USR_LIB)) / rest); /* last resort */
        } else {
            for (const auto &c : expand_path(install_name,referrer))
                candidates.push_back(c);
        }

        /* DYLD_LIBRARY_PATH and DYLD_FRAMEWORK_PATH come before the path
         * the binary was linked against — on macOS they override even an
         * absolute install name, which is the whole point of them: a
         * library is substituted without touching the binary. The FALLBACK
         * lists are the other way round, consulted only once the install
         * name itself has failed. A directory from either is a guest path,
         * so it is tried on the host as given and under the venv roots. */
        const std::string leaf = fs::path(std::string(install_name)).filename().string();
        const std::string framework = framework_partial_path(install_name);
        auto env_candidates = [&](const char *variable, std::vector<fs::path> &into) {
            for (const auto &dir : env_path_list(variable, environment_)) {
                const bool frameworks = std::string_view(variable).find("FRAMEWORK") != std::string_view::npos;
                if (frameworks && framework.empty()) continue;
                const std::string rest = frameworks ? framework : leaf;
                if (rest.empty()) continue;
                into.push_back(fs::path(dir) / rest);                              /* host path */
                into.push_back(fs::path(std::string(strip_root(dir))) / rest);     /* under the venv */
            }
        };
        std::vector<fs::path> before, after;
        env_candidates("DYLD_FRAMEWORK_PATH", before);
        env_candidates("DYLD_LIBRARY_PATH", before);
        env_candidates("DYLD_FALLBACK_FRAMEWORK_PATH", after);
        env_candidates("DYLD_FALLBACK_LIBRARY_PATH", after);
        /* An absolute name is a *guest* path, so it is looked for under the
         * venv first (that is what the expansion above did); as a last
         * resort it is also taken literally, which is how a host path in
         * DYLD_INSERT_LIBRARIES finds its library. */
        if (install_name.starts_with('/')) after.push_back(fs::path(std::string(install_name)));
        before.insert(before.end(), candidates.begin(), candidates.end());
        before.insert(before.end(), after.begin(), after.end());
        candidates = std::move(before);

        std::vector<fs::path> roots;
        /* DYLD_ROOT_PATH is a whole alternate system root; it precedes the venv. */
        for (const auto &r : env_path_list("DYLD_ROOT_PATH", environment_)) roots.emplace_back(r);
        roots.push_back(venv_.root);
        for (const auto &r : env_path_list("MACHOMAN_DYLIB_PATH", environment_)) roots.emplace_back(r);
        std::error_code ec;
#if !defined(_WIN32)
        /* Development convenience: the dylibs built next to the machoman binary. */
        if (const auto self = fs::read_symlink("/proc/self/exe", ec); !ec) {
            if (referrer.img.header->cputype == MM_CPU_TYPE_X86_64)
                roots.push_back(self.parent_path() / "dylibs/x86_64");
            roots.push_back(self.parent_path() / "dylibs");
        }
#endif
        fs::path host, guest;
        auto consider = [&](fs::path p) -> bool {
            p = p.lexically_normal();
            fs::path so = p;
            so += ".so";
            if (host.empty() && fs::is_regular_file(so, ec)) host = so;
            if (guest.empty() && fs::is_regular_file(p, ec)) guest = p;
            return !host.empty() || !guest.empty();
        };
        for (const auto &c : candidates) {
            if (c.is_absolute()) { if(consider(c))break; continue; }
            for (const auto &root : roots)
                if(consider(root / c))break;
            if(!host.empty()||!guest.empty())break;
        }
        return {host, guest};
    }

    guest_image *load_dylib(std::string_view install_name, const std::filesystem::path &path,
                           const std::vector<std::filesystem::path> &inherited_runpaths) {
        auto image = std::make_unique<guest_image>();
        std::error_code ec;
        image->path = std::filesystem::weakly_canonical(path, ec);
        image->runpaths = inherited_runpaths;
        image->install_name = std::string(install_name);
        image->file = read_whole_file(path);
        if (image->file.empty() || mm_macho_parse(image->file, image->img) != mm_macho_status::ok ||
            image->img.header->cputype != images_.front()->img.header->cputype ||
            image->img.header->filetype != MM_MH_DYLIB) {
            std::fprintf(stderr, "dyld: %s is not a matching-architecture Mach-O dylib\n", path.c_str());
            return nullptr;
        }
        /* Contiguous region for all segments at the next free base. */
        std::uint64_t lo = ~0ull, hi = 0;
        for (const auto *seg : image->img.segments) {
            if (seg->vmsize == 0 || mm_macho_name(seg->segname) == "__PAGEZERO") continue;
            lo = std::min(lo, seg->vmaddr);
            hi = std::max(hi, seg->vmaddr + seg->vmsize);
        }
        if (hi <= lo) return nullptr;
        const std::uint64_t span = (hi - lo + 0x3fff) & ~std::uint64_t{0x3fff};
        for (int attempt = 0; attempt < 64; attempt++) {
            image->slide = next_base_ - lo;
            if (image->mapping.map(image->img, image->slide)) {
                next_base_ += span + 0x4000;
                break;
            }
            image->mapping = image_mapping();
            next_base_ += span + 0x4000;
            image->slide = 0;
        }
        if (!image->slide) {
            std::fprintf(stderr, "dyld: cannot map %s\n", path.c_str());
            return nullptr;
        }
        if (dyld_trace_enabled())
            std::fprintf(stderr, "dyld: %s -> %s (guest, slide 0x%llx)\n", std::string(install_name).c_str(), path.c_str(),
                         static_cast<unsigned long long>(image->slide));
        guest_image *raw = image.get();
        images_.push_back(std::move(image));
        finish_image(*raw);
        return raw;
    }

    /* Rebases, indexes exports and loads dependencies of a mapped image. */
    void finish_image(guest_image &g) {
        std::vector<std::filesystem::path> paths;
        for(auto rp:g.img.rpaths) for(auto base:expand_path(rp,g)) {
            base=base.lexically_normal();
            if(std::find(paths.begin(),paths.end(),base)==paths.end())paths.push_back(std::move(base));
        }
        for(auto &base:g.runpaths)
            if(std::find(paths.begin(),paths.end(),base)==paths.end())paths.push_back(base);
        g.runpaths=std::move(paths);
        for (const std::uint64_t slot : g.img.rebases()) {
            std::uint64_t v;
            std::memcpy(&v, reinterpret_cast<const void *>(slot + g.slide), 8);
            v += g.slide;
            std::memcpy(reinterpret_cast<void *>(slot + g.slide), &v, 8);
        }
        for (auto &e : g.img.exports()) {
            if (e.reexport_ordinal) g.reexports.push_back(std::move(e));
            else g.exports.emplace(std::move(e.name), e.address + g.slide);
        }
        const auto unslid = g.img.executable_code();
        g.code = {unslid.addr + g.slide, unslid.bytes};
        for (const auto &d : g.img.dylibs) {
            const dependency dep = load_dependency(d.name, g);
            if (!dep.guest && !dep.host && !d.weak())
                std::fprintf(stderr, "dyld: %s: missing dependency %s\n", g.path.c_str(), std::string(d.name).c_str());
            g.deps.push_back(dep);
        }
    }

    /* Export lookup following re-exports and, as a fallback, the image's
     * dependencies (an umbrella library re-exports its sub-libraries). */
    std::uint64_t guest_lookup(const guest_image &g, const std::string &name, std::vector<const guest_image *> &seen) {
        if (std::find(seen.begin(), seen.end(), &g) != seen.end()) return 0;
        seen.push_back(&g);
        if (auto it = g.exports.find(name); it != g.exports.end()) return it->second;
        for (const auto &re : g.reexports)
            if (re.name == name && re.reexport_ordinal >= 1 && static_cast<std::size_t>(re.reexport_ordinal) <= g.deps.size())
                if (const guest_image *target = g.deps[static_cast<std::size_t>(re.reexport_ordinal) - 1].guest)
                    if (const std::uint64_t a = guest_lookup(*target, re.reexport_name.empty() ? name : std::string(re.reexport_name), seen))
                        return a;
        for (const dependency &d : g.deps)
            if (d.guest)
                if (const std::uint64_t a = guest_lookup(*d.guest, name, seen)) return a;
        return 0;
    }

    static void *host_synthesise(const host_lib &h, const std::string &darwin, bool lazy) {
#if defined(_WIN32)
        (void)h; (void)darwin; (void)lazy;
        return nullptr;
#else
        if (!h.handle) return nullptr;
        auto resolve = reinterpret_cast<void *(*)(const char *, unsigned)>(dlsym(h.handle, "machoman_resolve_symbol"));
        return resolve ? resolve(darwin.c_str(), lazy ? MM_SYMBOL_LAZY : 0u) : nullptr;
#endif
    }

    /* Whether `address` came from `h` itself rather than from one of the
     * libraries it links against. */
    static bool defined_in(const host_lib &h, void *address) {
#if defined(_WIN32)
        (void)h; (void)address;
        return true;
#else
        Dl_info info{};
        if (!dladdr(address, &info) || !info.dli_fname) return true;
        std::error_code ec;
        return std::filesystem::equivalent(info.dli_fname, h.path, ec);
#endif
    }

    static void *host_lookup(const host_lib &h, const std::string &darwin) {
#if defined(_WIN32)
        (void)h; (void)darwin;
        return nullptr;
#else
        if (!h.handle) return nullptr;
        if (void *p = dlsym(h.handle, darwin.c_str())) return p;
        const std::string plain = darwin.starts_with('_') ? darwin.substr(1) : darwin;
        return dlsym(h.handle, plain.c_str());
#endif
    }

    const mm_venv &venv_;
    std::filesystem::path exe_dir_;
    mm_environment &environment_;
    std::vector<std::unique_ptr<guest_image>> images_;   /* load order; [0] is the executable */
    std::vector<std::unique_ptr<host_lib>> host_libs_;
    std::uint64_t next_base_ = 0x200000000ull;           /* dylibs are placed from here upwards */
};

const char *section_name_at(const guest_image &g, std::uint64_t va) {
    for (const auto *seg : g.img.segments)
        for (const auto &sect : g.img.sections(*seg))
            if (va >= sect.addr + g.slide && va < sect.addr + g.slide + sect.size)
                return sect.sectname;
    return nullptr;
}

/* Nearest defined symbol at or below `va` ("name+0x..") from LC_SYMTAB;
 * empty when the image is stripped or `va` lies before the first symbol. */
std::string nearest_symbol(const guest_image &g, std::uint64_t va) {
    static std::mutex mu;
    static std::unordered_map<const guest_image *, std::vector<mm_symbol>> cache;
    std::lock_guard lock(mu);
    auto &syms = cache[&g];
    if (syms.empty()) syms = g.img.symbols();
    const std::uint64_t unslid = va - g.slide;
    auto it = std::ranges::upper_bound(syms, unslid, {}, &mm_symbol::address);
    if (it == syms.begin())
        return {};
    --it;
    char buf[32];
    std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(unslid - it->address));
    return std::string(it->name) + (unslid == it->address ? "" : buf);
}

std::string describe_pc(linker &ld, std::uint64_t va) {
    char buf[256];
    const guest_image *g = ld.image_for(va);
    if (g && g->img.header->cputype == MM_CPU_TYPE_X86_64 && va >= g->code.addr && va - g->code.addr < g->code.bytes.size()) {
        std::snprintf(buf, sizeof buf, "0x%llx (x86_64) %s", static_cast<unsigned long long>(va), nearest_symbol(*g, va).c_str());
    } else if (g && va >= g->code.addr && va - g->code.addr + 4 <= g->code.bytes.size()) {
        std::uint32_t raw = 0;
        std::memcpy(&raw, g->code.bytes.data() + (va - g->code.addr), 4);
        const mm_a64_insn insn = mm_a64_decode(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(&raw), 4), va);
        std::snprintf(buf, sizeof buf, "0x%llx (%08x %s) %s", static_cast<unsigned long long>(va), raw, mm_a64_opcode_name(insn.op),
                      nearest_symbol(*g, va).c_str());
    } else if (g) {
        const char *sect = section_name_at(*g, va);
        std::snprintf(buf, sizeof buf, "0x%llx (%.16s in %s)", static_cast<unsigned long long>(va), sect ? sect : "?",
                      g->path.filename().c_str());
    } else {
        std::snprintf(buf, sizeof buf, "0x%llx (unmapped)", static_cast<unsigned long long>(va));
    }
    return buf;
}

std::string symbol_name(const bound_import &i) { return std::string(i.bind.symbol); }

/* Reads 8 bytes of guest memory without faulting on unmapped addresses. */
bool safe_read(std::uint64_t va, std::uint64_t *out) {
#if defined(_WIN32)
    (void)va; (void)out;
    return false;
#else
    iovec local{out, 8};
    iovec remote{reinterpret_cast<void *>(va), 8};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 8;
#endif
}

/* Walks the frame-pointer chain (x29 -> {prev fp, lr}) of a stopped guest
 * thread. Reads are guarded because a corrupt chain is exactly what we are
 * likely to be diagnosing. */
void print_backtrace(linker &ld, const mm_cpu_context &ctx) {
    std::uint64_t pc = ctx.fault_va, lr = ctx.x[30], fp = ctx.x[29];
    for (int depth = 0; depth < 24; depth++) {
        const guest_image *g = ld.image_for(pc);
        std::fprintf(stderr, "  #%-2d 0x%llx %s%s%s\n", depth, static_cast<unsigned long long>(pc),
                     g ? nearest_symbol(*g, pc).c_str() : "", g ? " in " : "", g ? g->path.filename().c_str() : "");
        if (depth == 0)
            pc = lr;
        else {
            std::uint64_t next_fp = 0, next_lr = 0;
            const bool ok = safe_read(fp, &next_fp) && safe_read(fp + 8, &next_lr);
            if (!ok || next_fp <= fp)
                break;
            fp = next_fp;
            pc = next_lr;
        }
        if (pc == 0)
            break;
    }
}

/* Darwin signal frames, from <sys/signal.h> and <mach/{arm,i386}/_structs.h>. */
struct darwin_siginfo {
    std::int32_t signo, errno_, code, pid;
    std::uint32_t uid;
    std::int32_t status;
    std::uint64_t addr, value, band;
    std::uint64_t pad[7];
};
struct darwin_mcontext {
    std::uint64_t far;             /* exception state */
    std::uint32_t esr, exception;
    std::uint64_t x[29], fp, lr, sp, pc; /* thread state */
    std::uint32_t cpsr, pad;
    std::uint64_t v[32][2];        /* neon state: v0-v31 */
    std::uint32_t fpsr, fpcr;
};
struct darwin_x86_mcontext {
    struct { std::uint16_t trapno, cpu; std::uint32_t err; std::uint64_t faultvaddr; } es;
    struct { std::uint64_t registers[16], rip, rflags, cs, fs, gs; } ss;
    struct { std::uint32_t reserved[2]; std::uint8_t fx[512]; std::uint32_t reserved1; } fs;
};
static_assert(sizeof(darwin_x86_mcontext) == 712);
static_assert(offsetof(darwin_x86_mcontext, ss) == 16 && offsetof(darwin_x86_mcontext, fs) == 184);
constexpr int darwin_x86_registers[] = {MM_RAX, MM_RBX, MM_RCX, MM_RDX, MM_RDI, MM_RSI, MM_RBP, MM_RSP,
                                      MM_R8, MM_R9, MM_R10, MM_R11, MM_R12, MM_R13, MM_R14, MM_R15};
struct darwin_ucontext {
    std::int32_t uc_onstack;
    std::uint32_t uc_sigmask;
    struct { std::uint64_t ss_sp, ss_size; std::int32_t ss_flags, pad; } uc_stack;
    std::uint64_t uc_link;
    std::uint64_t uc_mcsize;
    std::uint64_t uc_mcontext;      /* -> darwin_mcontext */
};

struct guest_signal { std::uint64_t handler = 0; std::uint32_t flags = 0, mask = 0; };

/* Diagnostic frames are local to the interrupted thread's host stack.
 * Read them only while that same thread is stopped in the signal handler. */
struct host_call_frame {
    const void *function;
    std::uint64_t caller;
    std::uint64_t args[8];
    const host_call_frame *outer;
};

/* One guest thread: its register file, stack and lifecycle. */
struct guest_thread {
    std::uint64_t id = 0;
    mm_cpu_context ctx;
    std::uint64_t native_tls[512] = {};
    std::vector<std::uint64_t> stack;
    std::thread host;
    mm_runtime_status status = mm_runtime_status::ok;
    std::uint64_t result = 0;
    std::atomic<bool> finished{false};
    bool prepared_execution = false;
    mm_callback_result callback_failure;
    const host_call_frame *host_calls = nullptr;
    /* Frames of signal handlers currently running on this thread. */
    struct signal_frame {
        mm_cpu_context saved;
        std::unique_ptr<darwin_siginfo> info;
        std::unique_ptr<darwin_ucontext> uc;
        std::unique_ptr<darwin_mcontext> mc;
        std::unique_ptr<darwin_x86_mcontext> native_mc;
    };
    std::vector<signal_frame> signal_frames;
};

/* Everything a running guest executable consists of. There is one per
 * process; host dylibs reach it through the mm_host_api callbacks. */
class guest_process {
public:
    guest_process(const mm_venv &venv, std::span<char *const> args, const std::filesystem::path &exe_path,
                  mm_environment &environment, std::string launcher)
        : exe_path_(exe_path), environment_(environment), venv_root_(std::filesystem::absolute(venv.root).string()),
          launcher_(std::move(launcher)), executable_path_(exe_path.string()),
          linker_(venv, std::filesystem::weakly_canonical(exe_path).parent_path(), environment) {
        if (args.empty())
            arg_storage_.push_back(exe_path.string());
        for (char *a : args)
            arg_storage_.emplace_back(a);
        for (auto &a : arg_storage_) argv_.push_back(a.data());
        argv_.push_back(nullptr);
        /* dyld's "apple" array; its first entry is the executable path. */
        apple_storage_.push_back("executable_path=" + exe_path.string());
        for (auto &a : apple_storage_) apple_.push_back(a.data());
        apple_.push_back(nullptr);
    }

    mm_runtime_status load(std::vector<std::uint8_t> file, const mm_macho_image &img);
    mm_runtime_status run_main();
    ~guest_process() {
#if !defined(_WIN32)
        // Stop callback producers, then drain consumers while guest images,
        // thread contexts and host dylibs are still available.
        for (const char *name : {"machoman_dylib_quiesce", "machoman_dylib_stop"}) {
            std::vector<void (*)()> called;
            for (auto &library : linker_.host_libs()) if (library->handle) {
                auto stop = reinterpret_cast<void (*)()>(dlsym(library->handle, name));
                if (stop && std::find(called.begin(), called.end(), stop) == called.end()) {
                    called.push_back(stop); stop();
                }
            }
        }
#endif
        environment_.bind(nullptr);
        if (g_process == this) g_process = nullptr;
    }
    /* dyld runs these before main, dependencies first. */
    std::vector<std::uint64_t> initializers(const guest_image &g) const;

    /* mm_host_api callbacks (static: the api struct holds plain pointers). */
    static int api_thread_create(std::uint64_t entry_va, std::uint64_t arg, std::size_t stack_size, std::uint64_t *id);
    static int api_thread_join(std::uint64_t id, std::uint64_t *result);
    static std::uint64_t api_thread_self();
    static void api_thread_stack(std::uint64_t *base, std::uint64_t *size);
    static mm_cpu_context *api_current_context() { return g_current ? &g_current->ctx : nullptr; }
    static std::uint64_t api_fpcr_get();
    static void api_fpcr_set(std::uint64_t fpcr);
    static std::uint64_t api_signal_install(int signum, std::uint64_t handler_va, std::uint32_t flags, std::uint32_t mask);
    static std::uint64_t api_call_guest(std::uint64_t fn_va, const std::uint64_t *args, int nargs);
    static int api_prepare_guest_callback(std::uint64_t fn_va, std::size_t stack_size, void **handle);
    static int api_invoke_guest_callback(void *handle, const std::uint64_t *args, int nargs, mm_callback_result *result);
    static int api_release_guest_callback(void *handle);
    static std::uint64_t api_lookup_symbol(const char *name);
    static int api_copy_guest_memory(void *destination, const void *source, std::size_t size);
    static char ***api_environment() { return g_process->environment_.slot(); }
    static char *api_environment_get(const char *name) { return g_process->environment_.get(name); }
    static int api_environment_set(const char *name, const char *value, int overwrite) {
        const int error=g_process->environment_.set(name, value, overwrite);
        g_process->api_.envp=g_process->environment_.data(); return error;
    }
    static int api_environment_unset(const char *name) {
        const int error=g_process->environment_.unset(name);
        g_process->api_.envp=g_process->environment_.data(); return error;
    }
    static int api_environment_put(char *entry) {
        const int error=g_process->environment_.put(entry);
        g_process->api_.envp=g_process->environment_.data(); return error;
    }
    static void api_bind_environment(char ***storage) { g_process->environment_.bind(storage); }
    static bool api_is_guest_code(std::uint64_t va);
    static const char *api_unresolved_import_name(std::uint64_t va);
    static void api_atexit_register(std::uint64_t fn_va, std::uint64_t arg);
    static void api_atexit_run();
    static std::size_t api_image_count();
    static const void *api_image_header(std::size_t i);
    static std::int64_t api_image_slide(std::size_t i);
    static const char *api_image_path(std::size_t i);
    static std::ptrdiff_t api_image_for_address(std::uint64_t va);
    static void api_print_guest_backtrace();
    /* Prints every guest thread's stack (the SIGQUIT handler). */
    void dump_threads(std::uintptr_t interrupted_rip = 0);

private:
    mm_runtime_status run_thread(guest_thread &t, std::uint64_t pc);
    void report(const guest_thread &t, mm_runtime_status st);
    /* Sets up a Darwin signal frame for `signum` and returns the handler
     * VA to continue at, or 0 if the guest has no handler for it. */
    std::uint64_t deliver_signal(guest_thread &t, int signum, int code, std::uint64_t fault_addr);
    bool sigreturn(guest_thread &t);

    /* Binds an image's slots; imports of every image share traps_. */
    void bind_image(guest_image &g);
    /* The host function a guest branch to `target` means, if any: an
     * import's address, or (vtable slots of host objects, function pointers
     * handed out by host code) any exported symbol of a loaded host library. */
    const bound_import *host_target(const void *target);

    std::filesystem::path exe_path_;
    mm_environment &environment_;
    std::string venv_root_, launcher_, executable_path_;
    linker linker_;
    guest_image *main_ = nullptr;
    std::vector<bound_import> imports_;
    struct interposition { std::uint64_t original, replacement; const guest_image *owner; };
    std::vector<interposition> interpositions_;
    std::unordered_map<const void *, std::size_t> import_by_host_;
    std::mutex dladdr_mu_;
    std::vector<std::unique_ptr<bound_import>> dladdr_imports_; /* host targets discovered at run time */
    import_traps traps_;
    std::vector<std::string> arg_storage_, apple_storage_;
    std::vector<char *> argv_, apple_;
    mm_host_api api_{};
    std::vector<std::pair<mm_dylib_boundary_fn, mm_dylib_boundary_fn>> boundary_hooks_;
    std::vector<mm_dylib_thread_exit_fn> thread_exit_hooks_;
    void enter_guest_boundary() { for (const auto &[enter, leave] : boundary_hooks_) { (void)leave; enter(); } }
    void leave_guest_boundary() { for (const auto &[enter, leave] : boundary_hooks_) { (void)enter; leave(); } }

    std::mutex atexit_mu_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> atexit_;

    std::mutex signals_mu_;
    guest_signal signals_[33];
    std::uint64_t sigreturn_va_ = 0; /* trap address a signal handler returns to */
    std::uint64_t native_return_va_ = 0;

    std::mutex threads_mu_;
    std::uint64_t next_thread_id_ = 1;
    std::vector<std::unique_ptr<guest_thread>> threads_;

    static inline guest_process *g_process = nullptr;
    static inline thread_local guest_thread *g_current = nullptr;
    static inline thread_local mm_environment *fork_environment_ = nullptr;
    static void prepare_environment_fork() {
        fork_environment_=g_process ? &g_process->environment_ : nullptr;
        if (fork_environment_) fork_environment_->prepare_fork();
    }
    static void finish_environment_fork() {
        if (fork_environment_) fork_environment_->finish_fork();
        fork_environment_=nullptr;
    }

    struct callback_binding {
        guest_process *process;
        guest_thread *thread;
        std::thread::id owner = std::this_thread::get_id();
        std::size_t handles = 0;
        bool active = false;
        std::vector<const mm_runtime_code *> codes;
#if !defined(_WIN32)
        fault_handler_scope handlers;
#endif
    };
    struct prepared_callback { callback_binding *binding; std::uint64_t function; };
    static inline thread_local callback_binding *g_callback_binding = nullptr;
};

mm_runtime_status guest_process::load(std::vector<std::uint8_t> file, const mm_macho_image &img) {
    main_ = linker_.load_executable(exe_path_, std::move(file), img);
    if (!main_)
        return mm_runtime_status::map_failed;
    if (main_->code.bytes.empty())
        return mm_runtime_status::bad_image;

    /* Dynamic linking: every bind slot of every image gets the definition's
     * address or a unique trap address that identifies the import when hit. */
    for (auto &g : linker_.images())
        for (const mm_bind &b : g->img.binds()) {
            bound_import imp{};
            imp.bind = b;
            imp.image = g.get();
            linker_.lookup(*g, b.dylib_ordinal, b.symbol, b.lazy, imp.guest, imp.host);
            imp.preserve_regs = imp.host && linker_.preserves_registers(b.symbol);
            imp.returns_this = imp.host && is_ctor_or_dtor(b.symbol);
            imports_.push_back(std::move(imp));
        }
    const auto trap_count = imports_.size() + 1026;
    imports_.reserve(trap_count - 2);
    if (!traps_.reserve(trap_count))
        return mm_runtime_status::map_failed;
    sigreturn_va_ = traps_.va(trap_count - 2);
    native_return_va_ = traps_.va(trap_count - 1);
    std::size_t unresolved = 0;
    for (std::size_t i = 0; i < imports_.size(); i++) {
        const auto &imp = imports_[i];
        std::uint64_t value;
        if (imp.bind.weak_table) {
            continue; /* coalescable symbol: keep the image's own (rebased) definition */
        } else if (imp.guest) {
            value = imp.guest + static_cast<std::uint64_t>(imp.bind.addend);
        } else if (imp.host) {
            /* Even ABI-compatible libc functions cross the boundary: guest
             * errno writes must reach libc before it executes. */
            value = main_->img.header->cputype == MM_CPU_TYPE_X86_64 && host_callable(imp.host, imp.bind.lazy)
                ? traps_.va(i) : reinterpret_cast<std::uintptr_t>(imp.host);
            value += static_cast<std::uint64_t>(imp.bind.addend);
            import_by_host_.emplace(imp.host, i);
        } else if (imp.bind.weak) {
            value = 0; /* weak import that nobody provides */
        } else {
            value = traps_.va(i);
            unresolved++;
        }
        std::memcpy(reinterpret_cast<void *>(imp.bind.address + imp.image->slide), &value, 8);
        if (trace_enabled())
            std::fprintf(stderr, "bind: %s: %s -> %s [slot %llx]\n", imp.image->path.filename().c_str(), symbol_name(imp).c_str(),
                         imp.guest ? "guest" : imp.host ? "host" : imp.bind.weak_table ? "own" : imp.bind.weak ? "null" : "trap",
                         static_cast<unsigned long long>(imp.bind.address));
    }
    if (dyld_trace_enabled())
        std::fprintf(stderr, "dyld: %zu images, %zu imports, %zu unresolved\n", linker_.images().size(), imports_.size(), unresolved);

    /* DYLD_INTERPOSE pairs contain {replacement, original}. The original
     * is already bound above; native host traps identify the same original
     * function even when different import slots have different trap VAs.
     * The interposing image retains its original imports for fallbacks. */
    for (const auto &image : linker_.images())
        for (const auto *segment : image->img.segments)
            for (const auto &section : image->img.sections(*segment)) {
                if (mm_macho_name(section.sectname) != "__interpose") continue;
                for (std::uint64_t offset = 0; offset + 16 <= section.size; offset += 16) {
                    std::uint64_t pair[2];
                    std::memcpy(pair, reinterpret_cast<const void *>(section.addr + image->slide + offset), sizeof pair);
                    const auto trap = traps_.index_of(pair[1]);
                    if (trap >= 0 && static_cast<std::size_t>(trap) < imports_.size()) {
                        const auto &imported = imports_[static_cast<std::size_t>(trap)];
                        pair[1] = imported.guest ? imported.guest : reinterpret_cast<std::uintptr_t>(imported.host);
                    }
                    if (pair[0] && pair[1]) interpositions_.push_back({pair[1], pair[0], image.get()});
                }
            }
    std::size_t replaced = 0;
    for (const auto &imported : imports_) {
        if (imported.bind.weak_table) continue;
        const auto original = imported.guest ? imported.guest : reinterpret_cast<std::uintptr_t>(imported.host);
        for (const auto &interpose : interpositions_) {
            if (interpose.owner == imported.image || interpose.original != original) continue;
            const auto replacement = interpose.replacement + static_cast<std::uint64_t>(imported.bind.addend);
            std::memcpy(reinterpret_cast<void *>(imported.bind.address + imported.image->slide), &replacement, 8);
            ++replaced;
            break;
        }
    }
    if (dyld_trace_enabled() && replaced)
        std::fprintf(stderr, "dyld: applied %zu interposed bindings from %zu replacement pairs\n", replaced, interpositions_.size());

    api_ = mm_host_api{6, reinterpret_cast<const void *>(main_->header_va()),
                       static_cast<int>(arg_storage_.size()), argv_.data(), environment_.data(),
                       &api_thread_create, &api_thread_join, &api_thread_self, &api_thread_stack,
                       &api_fpcr_get, &api_fpcr_set, &api_current_context, &api_signal_install,
                       &api_call_guest, &api_is_guest_code, &api_unresolved_import_name,
                       &api_atexit_register, &api_atexit_run,
                       &api_image_count, &api_image_header, &api_image_slide, &api_image_path, &api_image_for_address,
                       &api_print_guest_backtrace,
                       &api_prepare_guest_callback, &api_invoke_guest_callback, &api_release_guest_callback,
                       &api_lookup_symbol, &api_copy_guest_memory,
                       &api_environment, &api_environment_get, &api_environment_set, &api_environment_unset,
                       &api_environment_put, &api_bind_environment, venv_root_.c_str(), launcher_.c_str(), executable_path_.c_str()};
    g_process = this;
#if !defined(_WIN32)
    /* Register before guest atfork handlers. Prepare runs last and unlock
     * runs first, so guest callbacks can themselves change the environment. */
    static const int environment_fork_registration=::pthread_atfork(
        prepare_environment_fork, finish_environment_fork, finish_environment_fork);
    if (environment_fork_registration) return mm_runtime_status::map_failed;
    /* SIGQUIT (^\ or `kill -QUIT`) dumps every guest thread's stack, which
     * is the only way to see where a hung guest is standing: translated
     * code holds no host state, so a host debugger's backtrace stops at
     * the trampoline. */
    {
        struct sigaction quit{};
        quit.sa_sigaction = [](int, siginfo_t *, void *context) {
            const auto *uc = static_cast<const ucontext_t *>(context);
            if (g_process) g_process->dump_threads(static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]));
        };
        quit.sa_flags = SA_SIGINFO;
        sigemptyset(&quit.sa_mask);
        sigaction(SIGQUIT, &quit, nullptr);
    }
#endif
#if !defined(_WIN32)
    /* dlsym searches a library's dependencies too, so several libraries can
     * report the same initializer (every stub framework sees libobjc's);
     * call each distinct one once. */
    linker_.include_host_dependencies();
    std::vector<mm_dylib_init_fn> initialized;
    for (auto &h : linker_.host_libs()) {
        if (!h->handle) continue;
        auto init = reinterpret_cast<mm_dylib_init_fn>(dlsym(h->handle, "machoman_dylib_init"));
        if (!init || std::find(initialized.begin(), initialized.end(), init) != initialized.end()) continue;
        initialized.push_back(init);
        init(&api_);
    }
    for (auto &h : linker_.host_libs()) {
        if (!h->handle) continue;
        const auto enter = reinterpret_cast<mm_dylib_boundary_fn>(dlsym(h->handle, "machoman_dylib_guest_enter"));
        const auto leave = reinterpret_cast<mm_dylib_boundary_fn>(dlsym(h->handle, "machoman_dylib_guest_leave"));
        const auto hooks = std::pair{enter, leave};
        if (enter && leave && std::find(boundary_hooks_.begin(), boundary_hooks_.end(), hooks) == boundary_hooks_.end())
            boundary_hooks_.push_back(hooks);
        const auto thread_exit = reinterpret_cast<mm_dylib_thread_exit_fn>(dlsym(h->handle, "machoman_dylib_thread_exit"));
        if (thread_exit && std::find(thread_exit_hooks_.begin(), thread_exit_hooks_.end(), thread_exit) == thread_exit_hooks_.end())
            thread_exit_hooks_.push_back(thread_exit);
    }
#endif

    for (auto &g : linker_.images()) {
        if (g->img.header->cputype == MM_CPU_TYPE_X86_64) {
            if (dyld_trace_enabled()) std::fprintf(stderr, "native: %s (x86_64, no translation)\n", g->path.c_str());
            continue;
        }
        const auto cached = mm_translate_cached(g->path, g->file, g->code.bytes, g->code.addr, g->result);
        if (trace_enabled() || std::getenv("MACHOMAN_CACHE_TRACE"))
            std::fprintf(stderr, "translation cache: %s: %s\n", g->path.c_str(),
                         cached == mm_cache_status::hit ? "hit" : cached == mm_cache_status::stored ? "stored" : "bypassed");
        g->runtime = std::make_unique<mm_runtime_code>(g->result);
        if (!g->runtime->ok())
            return mm_runtime_status::exec_alloc_failed;
        if (trace_enabled())
            std::fprintf(stderr, "translate: %s: %zu instructions, %zu unsupported\n", g->path.filename().c_str(),
                         g->result.notes.size(), g->result.unsupported_count);
    }
    return mm_runtime_status::ok;
}

const bound_import *guest_process::host_target(const void *target) {
    if (auto it = import_by_host_.find(target); it != import_by_host_.end())
        return &imports_[it->second];
#if !defined(_WIN32)
    std::lock_guard lock(dladdr_mu_);
    for (const auto &d : dladdr_imports_)
        if (d->host == target) return d.get();
    Dl_info info{};
    if (!dladdr(target, &info) || !info.dli_sname || info.dli_saddr != target)
        return nullptr;
    auto imp = std::make_unique<bound_import>();
    imp->name_storage = std::string("_") + info.dli_sname;
    imp->bind.symbol = imp->name_storage;
    imp->host = const_cast<void *>(target);
    imp->returns_this = is_ctor_or_dtor(imp->name_storage);
    imp->preserve_regs = linker_.preserves_registers(imp->name_storage);
    dladdr_imports_.push_back(std::move(imp));
    return dladdr_imports_.back().get();
#else
    return nullptr;
#endif
}

/* Runs guest code on the calling host thread until the guest exits,
 * servicing calls into host dylibs on the way. */
mm_runtime_status guest_process::run_thread(guest_thread &t, std::uint64_t pc) {
#if !defined(_WIN32)
    fault_handler_scope handlers;
#endif
    /* Nested runs happen whenever host code calls back into the guest
     * (call_guest, +load, a block), so the previous current thread has to
     * come back afterwards rather than being cleared. */
    guest_thread *const outer = g_current;
    g_current = &t;
    const bool native = main_->img.header->cputype == MM_CPU_TYPE_X86_64;
    if (native) {
        t.ctx.native_mode = 1;
        // Swift-context callbacks pass swiftself in r13, like arm64 x20.
        // Initial thread contexts are zero; nested calls may supply a self.
        const auto swift_self = t.ctx.native_x[MM_R13];
        std::memset(t.ctx.native_x, 0, sizeof t.ctx.native_x);
        t.ctx.native_x[MM_R13] = swift_self;
        constexpr int arguments[] = {MM_RDI, MM_RSI, MM_RDX, MM_RCX, MM_R8, MM_R9};
        for (int i = 0; i < 6; ++i) t.ctx.native_x[arguments[i]] = t.ctx.x[i];
        t.ctx.native_x[MM_RSP] = t.ctx.x[31] - 8;
        const std::uint64_t initial_stack[] = {native_return_va_, t.ctx.x[6], t.ctx.x[7]};
        std::memcpy(reinterpret_cast<void *>(t.ctx.native_x[MM_RSP]), initial_stack, sizeof initial_stack);
        std::memcpy(t.ctx.native_v, t.ctx.v, sizeof(t.ctx.v[0]) * 16);
#if !defined(_WIN32)
        asm volatile("fxsave %0" : "=m" (t.ctx.native_fp));
#endif
        t.ctx.native_flags = 0x202;
        t.native_tls[0] = reinterpret_cast<std::uintptr_t>(t.native_tls);
    }
    mm_runtime_status st;
    for (;;) {
        int native_signal = 11, native_signal_code = 1;
        if (native) {
#if !defined(_WIN32)
            fault_guard guard;
            guard.native = &t.ctx;
            auto *previous = g_guard;
            g_guard = &guard;
            unsigned long previous_gs = 0;
            syscall(SYS_arch_prctl, ARCH_GET_GS, &previous_gs);
            syscall(SYS_arch_prctl, ARCH_SET_GS, t.native_tls);
            alignas(16) std::uint8_t host_fp[512];
            asm volatile("fxsave %0" : "=m" (host_fp));
            enter_guest_boundary();
            if (sigsetjmp(guard.jump, 0) == 0) mm_native_enter(&t.ctx, pc);
            leave_guest_boundary();
            asm volatile("fxrstor %0" : : "m" (host_fp));
            syscall(SYS_arch_prctl, ARCH_SET_GS, previous_gs);
            g_guard = previous;
            t.ctx.fault_va = guard.rip;
            t.ctx.fault_addr = guard.addr;
            t.ctx.x[29] = t.ctx.native_x[MM_RBP];
            if (guard.rip == native_return_va_) {
                t.ctx.x[0] = t.ctx.native_x[MM_RAX];
                t.ctx.x[31] = t.ctx.native_x[MM_RSP];
                st = mm_runtime_status::ok;
                break;
            }
            constexpr int arguments[] = {MM_RDI, MM_RSI, MM_RDX, MM_RCX, MM_R8, MM_R9};
            for (int i = 0; i < 6; ++i) t.ctx.x[i] = t.ctx.native_x[arguments[i]];
            const auto sp = t.ctx.native_x[MM_RSP];
            const auto stack_begin = reinterpret_cast<std::uintptr_t>(t.stack.data());
            const auto stack_end = stack_begin + t.stack.size() * sizeof(std::uint64_t);
            std::uint64_t words[3];
            if (sp >= stack_begin && sp <= stack_end - sizeof words)
                std::memcpy(words, reinterpret_cast<const void *>(sp), sizeof words);
            else if (sp > UINT64_MAX - sizeof words ||
                     !guarded_copy(words, reinterpret_cast<const void *>(sp), sizeof words)) {
                st = mm_runtime_status::memory_fault; break;
            }
            t.ctx.x[30] = words[0];
            t.ctx.x[6] = words[1];
            t.ctx.x[7] = words[2];
            t.ctx.x[31] = sp + 24;
            std::memcpy(t.ctx.v, t.ctx.native_v, sizeof t.ctx.native_v);
            st = guard.signum == SIGSEGV && traps_.index_of(guard.rip) >= 0
                ? mm_runtime_status::bad_target : mm_runtime_status::memory_fault;
            native_signal = guard.signum == SIGBUS ? 10 : guard.signum;
            native_signal_code = guard.code > 0 && guard.code < 128 ? guard.code : 1;
#else
            st = mm_runtime_status::unsupported_arch;
#endif
        } else {
            enter_guest_boundary();
            st = main_->runtime->run(t.ctx, pc);
            leave_guest_boundary();
        }
        if (st == mm_runtime_status::unsupported_insn || st == mm_runtime_status::memory_fault ||
            st == mm_runtime_status::brk) {
            if (t.prepared_execution) break;
            int signum = 4, code = 1; /* SIGILL, ILL_ILLOPC */
            std::uint64_t addr = t.ctx.fault_va;
            if (st == mm_runtime_status::memory_fault) { signum = 11; code = 1; addr = t.ctx.fault_addr; } /* SIGSEGV, SEGV_MAPERR */
            if (st == mm_runtime_status::brk) { signum = 5; code = 1; }                                       /* SIGTRAP */
            if (native) { signum = native_signal; code = native_signal_code; }
            if (const std::uint64_t handler = deliver_signal(t, signum, code, addr)) {
                if (trace_enabled() || diagnostics_enabled())
                    std::fprintf(stderr, "signal: %d code %d at 0x%llx accessing 0x%llx -> handler 0x%llx\n", signum,
                                 code, static_cast<unsigned long long>(t.ctx.fault_va),
                                 static_cast<unsigned long long>(addr), static_cast<unsigned long long>(handler));
                pc = handler;
                continue;
            }
            break;
        }
        if (st != mm_runtime_status::bad_target)
            break;
        if (t.ctx.fault_va == sigreturn_va_) {
            if (!sigreturn(t)) break;
            pc = t.ctx.entry_va; /* set by sigreturn: resume address */
            continue;
        }
        const std::ptrdiff_t trap = traps_.index_of(t.ctx.fault_va);
        const bound_import *native_import = nullptr;
        if (trap >= 0 && static_cast<std::size_t>(trap) < imports_.size() && native)
            native_import = &imports_[static_cast<std::size_t>(trap)];
        if (trap >= 0 && (!native_import || !native_import->host)) {
            if (t.prepared_execution) { st = mm_runtime_status::unresolved_import; break; }
            const auto &imp = imports_[static_cast<std::size_t>(trap)];
            const auto &dylibs = imp.image->img.dylibs;
            std::fprintf(stderr, "run: unresolved import %s (from %s), called from %s\n", symbol_name(imp).c_str(),
                         imp.bind.dylib_ordinal >= 1 && static_cast<std::size_t>(imp.bind.dylib_ordinal) <= dylibs.size()
                             ? std::string(dylibs[static_cast<std::size_t>(imp.bind.dylib_ordinal) - 1].name).c_str() : "?",
                         describe_pc(linker_, t.ctx.x[30]).c_str());
            st = mm_runtime_status::unresolved_import;
            break;
        }
        const void *target = native_import ? native_import->host : reinterpret_cast<const void *>(t.ctx.fault_va);
        const bound_import *it = nullptr;
        if (t.prepared_execution) {
            if (const auto found = import_by_host_.find(target); found != import_by_host_.end())
                it = &imports_[found->second];
        } else {
            it = host_target(target);
        }
        if (!it) {
            if (t.prepared_execution) break;
            std::fprintf(stderr, "run: branch to %s, x30=0x%llx\n", describe_pc(linker_, t.ctx.fault_va).c_str(),
                         static_cast<unsigned long long>(t.ctx.x[30]));
            for (int i = 0; i < 32; i += 4)
                std::fprintf(stderr, "     x%-2d=0x%016llx x%-2d=0x%016llx x%-2d=0x%016llx x%-2d=0x%016llx\n", i,
                             static_cast<unsigned long long>(t.ctx.x[i]), i + 1, static_cast<unsigned long long>(t.ctx.x[i + 1]),
                             i + 2, static_cast<unsigned long long>(t.ctx.x[i + 2]), i + 3, static_cast<unsigned long long>(t.ctx.x[i + 3]));
            print_backtrace(linker_, t.ctx);
            break;
        }
        if (!t.prepared_execution && trace_enabled())
            std::fprintf(stderr, "call: %s(0x%llx, 0x%llx, 0x%llx)\n", symbol_name(*it).c_str(),
                         static_cast<unsigned long long>(t.ctx.x[0]), static_cast<unsigned long long>(t.ctx.x[1]),
                         static_cast<unsigned long long>(t.ctx.x[2]));
        /* mm_hostcall forwards x0-x7, v0-v7 and six stack words to SysV.
         * Adapters read packed narrow Darwin arm64 stack arguments by type.
         * Integer/FP results return in x0 and v0/v1; x1 needs an explicit
         * two-word return override. */
        t.ctx.resume_va = 0;
        t.ctx.host_tail_fn = 0;
        auto call_host = [&](const void *fn) {
            std::uint64_t argument_vectors[8][2];
            std::memcpy(argument_vectors, t.ctx.v, sizeof argument_vectors);
            host_call_frame frame;
            if (!t.prepared_execution && diagnostics_enabled()) {
                frame.function = fn;
                frame.caller = t.ctx.x[30];
                frame.outer = t.host_calls;
                std::memcpy(frame.args, t.ctx.x, sizeof frame.args);
                t.host_calls = &frame;
            }
            enter_guest_boundary();
            mm_hostcall(&t.ctx, fn);
            leave_guest_boundary();
            if (!t.prepared_execution && diagnostics_enabled()) t.host_calls = frame.outer;
            /* A dispatcher has selected an implementation; it has not
             * returned to the guest yet. Its C return registers are
             * scratch, so forward the original FP/SIMD arguments. */
            if (t.ctx.resume_va || t.ctx.host_tail_fn)
                std::memcpy(t.ctx.v, argument_vectors, sizeof argument_vectors);
        };
        if (native && symbol_name(*it) == "__tlv_bootstrap") {
            const auto saved_rdx = t.ctx.native_x[MM_RDX];
            std::uint64_t saved_v[16][2];
            std::memcpy(saved_v, t.ctx.v, sizeof saved_v);
            call_host(target);
            t.ctx.native_x[MM_RDX] = saved_rdx;
            std::memcpy(t.ctx.v, saved_v, sizeof saved_v);
        } else if (it->preserve_regs) {
            std::uint64_t saved_x[19], saved_v[8][2];
            std::memcpy(saved_x, t.ctx.x, sizeof saved_x);
            std::memcpy(saved_v, t.ctx.v, sizeof saved_v);
            call_host(target);
            std::memcpy(t.ctx.x, saved_x, sizeof saved_x);
            std::memcpy(t.ctx.v, saved_v, sizeof saved_v);
            if (native) t.ctx.x[0] = t.ctx.native_x[MM_RAX];
        } else if (it->returns_this) {
            const std::uint64_t self = t.ctx.x[0];
            call_host(target);
            t.ctx.x[0] = self;
        } else {
            call_host(target);
        }
        /* A host function may chain to another host function with the
         * guest's original argument registers (objc_msgSend -> host IMP). */
        while (t.ctx.host_tail_fn) {
            if (t.callback_failure.status != mm_runtime_status::ok) break;
            const auto fn = reinterpret_cast<const void *>(t.ctx.host_tail_fn);
            t.ctx.host_tail_fn = 0;
            call_host(fn);
        }
        if (native && trace_enabled())
            std::fprintf(stderr, "return: %s -> 0x%llx, caller 0x%llx, resume 0x%llx\n", symbol_name(*it).c_str(),
                         static_cast<unsigned long long>(t.ctx.x[0]), static_cast<unsigned long long>(t.ctx.x[30]),
                         static_cast<unsigned long long>(t.ctx.resume_va));
        if (t.callback_failure.status != mm_runtime_status::ok) {
            st = t.callback_failure.status;
            t.ctx.fault_va = t.callback_failure.fault_va;
            t.ctx.fault_addr = t.callback_failure.fault_addr;
            break;
        }
        pc = t.ctx.resume_va ? t.ctx.resume_va : t.ctx.x[30];
        if (native) {
            if (t.ctx.resume_va) {
                constexpr int arguments[] = {MM_RDI, MM_RSI, MM_RDX, MM_RCX, MM_R8, MM_R9};
                for (int i = 0; i < 6; ++i) t.ctx.native_x[arguments[i]] = t.ctx.x[i];
            } else {
                t.ctx.native_x[MM_RSP] += 8;
                t.ctx.native_x[MM_RAX] = t.ctx.x[0];
            }
            std::memcpy(t.ctx.native_v, t.ctx.v, sizeof t.ctx.native_v);
        }
        t.ctx.resume_va = 0;
        /* A top-level guest function can tail-call a host implementation;
         * its host return then consumes the same zero LR as guest RET. */
        if (!pc) { st = mm_runtime_status::ok; break; }
    }
    g_current = outer;
    return st;
}

void guest_process::report(const guest_thread &t, mm_runtime_status st) {
    const mm_cpu_context &ctx = t.ctx;
    switch (st) {
    case mm_runtime_status::ok:
        std::fprintf(stderr, "run: entry returned x0=%llu\n", static_cast<unsigned long long>(ctx.x[0]));
        break;
    case mm_runtime_status::unsupported_insn: {
        std::uint32_t raw = 0;
        if (const guest_image *g = linker_.image_for(ctx.fault_va);
            g && ctx.fault_va >= g->code.addr && ctx.fault_va - g->code.addr + 4 <= g->code.bytes.size())
            std::memcpy(&raw, g->code.bytes.data() + (ctx.fault_va - g->code.addr), 4);
        std::fprintf(stderr, "run: unsupported instruction at %s: %08x (%s)\n", describe_pc(linker_, ctx.fault_va).c_str(), raw,
                     mm_a64_class_name(raw));
        print_backtrace(linker_, ctx);
        break;
    }
    case mm_runtime_status::syscall:
        std::fprintf(stderr, "run: syscall x16=%llu at 0x%llx (not dispatched yet)\n",
                     static_cast<unsigned long long>(ctx.x[16]), static_cast<unsigned long long>(ctx.fault_va));
        break;
    case mm_runtime_status::brk:
        std::fprintf(stderr, "run: brk at 0x%llx\n", static_cast<unsigned long long>(ctx.fault_va));
        break;
    case mm_runtime_status::memory_fault: {
        const std::ptrdiff_t trap = traps_.index_of(ctx.fault_addr);
        if (trap >= 0 && static_cast<std::size_t>(trap) < imports_.size() &&
            !imports_[static_cast<std::size_t>(trap)].host && !imports_[static_cast<std::size_t>(trap)].guest) {
            std::fprintf(stderr, "run: unresolved data import %s dereferenced at %s\n",
                         symbol_name(imports_[static_cast<std::size_t>(trap)]).c_str(),
                         describe_pc(linker_, ctx.fault_va).c_str());
            print_backtrace(linker_, ctx);
        } else {
            std::fprintf(stderr, "run: memory fault at guest %s accessing 0x%llx; sp=0x%llx x30=0x%llx\n",
                         describe_pc(linker_, ctx.fault_va).c_str(), static_cast<unsigned long long>(ctx.fault_addr),
                         static_cast<unsigned long long>(ctx.x[31]), static_cast<unsigned long long>(ctx.x[30]));
            if (std::getenv("MACHOMAN_TRACE_FAULT_REGISTERS")) {
                for (int i=0;i<31;++i)std::fprintf(stderr," x%d=0x%llx",i,static_cast<unsigned long long>(ctx.x[i]));
                std::fputc('\n',stderr);
            }
            print_backtrace(linker_, ctx);
        }
        break;
    }
    default:
        break;
    }
}

/* `__mod_init_func` holds absolute (rebased) function pointers,
 * `__init_offsets` 32-bit offsets from the image's mach header. */
std::vector<std::uint64_t> guest_process::initializers(const guest_image &g) const {
    std::vector<std::uint64_t> out;
    const std::uint64_t base = g.header_va();
    for (const auto *seg : g.img.segments)
        for (const auto &sect : g.img.sections(*seg)) {
            const std::uint32_t type = sect.flags & MM_S_TYPE_MASK;
            if (type == 0x9) {
                for (std::uint64_t off = 0; off + 8 <= sect.size; off += 8) {
                    std::uint64_t fn;
                    std::memcpy(&fn, reinterpret_cast<const void *>(sect.addr + g.slide + off), 8);
                    if (fn) out.push_back(fn);
                }
            } else if (type == 0x16) {
                for (std::uint64_t off = 0; off + 4 <= sect.size; off += 4) {
                    std::uint32_t rel;
                    std::memcpy(&rel, reinterpret_cast<const void *>(sect.addr + g.slide + off), 4);
                    out.push_back(base + rel);
                }
            }
        }
    return out;
}

mm_runtime_status guest_process::run_main() {
    const mm_section_64 *text = main_->img.find_section("__TEXT", "__text");
    if (!text)
        return mm_runtime_status::bad_image;

    auto main_thread = std::make_unique<guest_thread>();
    main_thread->id = next_thread_id_++;
    main_thread->stack.resize(8 * 1024 * 1024 / 8);
    mm_cpu_context &ctx = main_thread->ctx;
    ctx.x[31] = reinterpret_cast<std::uintptr_t>(main_thread->stack.data() + main_thread->stack.size() - 64) & ~std::uintptr_t{15};
    ctx.x[30] = 0; /* return-to-host sentinel */
    ctx.x[0] = static_cast<std::uint64_t>(api_.argc);
    ctx.x[1] = reinterpret_cast<std::uintptr_t>(argv_.data());
    ctx.x[2] = reinterpret_cast<std::uintptr_t>(environment_.data());
    ctx.x[3] = reinterpret_cast<std::uintptr_t>(apple_.data());
    guest_thread &t = *main_thread;
    {
        std::lock_guard lock(threads_mu_);
        threads_.push_back(std::move(main_thread));
    }

#if !defined(_WIN32)
    /* +load and other startup callbacks need executable translations,
     * initialized frameworks, and the same guest identity as main. */
    guest_thread *const outer = g_current;
    g_current = &t;
    std::vector<mm_dylib_start_fn> started;
    for (auto &h : linker_.host_libs()) {
        if (!h->handle) continue;
        auto start = reinterpret_cast<mm_dylib_start_fn>(dlsym(h->handle, "machoman_dylib_start"));
        if (!start || std::find(started.begin(), started.end(), start) != started.end()) continue;
        started.push_back(start);
        start();
    }
    g_current = outer;
#endif

    /* Static initializers, dependencies before dependents like dyld, with
     * the arguments dyld passes (argc, argv, envp, apple, ProgramVars). */
    std::vector<std::uint64_t> inits;
    std::vector<const guest_image *> visited;
    auto visit = [&](auto &self, const guest_image &g) -> void {
        if (std::find(visited.begin(), visited.end(), &g) != visited.end()) return;
        visited.push_back(&g);
        for (const dependency &d : g.deps)
            if (d.guest) self(self, *d.guest);
        for (const std::uint64_t init : initializers(g)) inits.push_back(init);
    };
    visit(visit, *main_);
    for (const std::uint64_t init : inits) {
        if (trace_enabled())
            std::fprintf(stderr, "init: %s\n", describe_pc(linker_, init).c_str());
        t.ctx.x[0] = static_cast<std::uint64_t>(api_.argc);
        t.ctx.x[1] = reinterpret_cast<std::uintptr_t>(argv_.data());
        t.ctx.x[2] = reinterpret_cast<std::uintptr_t>(environment_.data());
        t.ctx.x[3] = reinterpret_cast<std::uintptr_t>(apple_.data());
        t.ctx.x[4] = 0;
        t.ctx.x[30] = 0;
        const mm_runtime_status ist = run_thread(t, init);
        if (ist != mm_runtime_status::ok) {
            std::fprintf(stderr, "run: static initializer 0x%llx failed\n", static_cast<unsigned long long>(init));
            report(t, ist);
            return ist;
        }
    }
    t.ctx.x[0] = static_cast<std::uint64_t>(api_.argc);
    t.ctx.x[1] = reinterpret_cast<std::uintptr_t>(argv_.data());
    t.ctx.x[2] = reinterpret_cast<std::uintptr_t>(environment_.data());
    t.ctx.x[3] = reinterpret_cast<std::uintptr_t>(apple_.data());
    t.ctx.x[30] = 0;

    const std::uint64_t pc = text->addr + (main_->img.entry_offset - text->offset);
    mm_runtime_status st = run_thread(t, pc);
    if (st == mm_runtime_status::ok) {
        g_current = &t;
        api_atexit_run();
        g_current = nullptr;
    }
    if (st == mm_runtime_status::memory_fault && traps_.index_of(t.ctx.fault_addr) >= 0) {
        report(t, st);
        return mm_runtime_status::unresolved_import;
    }
    report(t, st);
    return st;
}

int guest_process::api_thread_create(std::uint64_t entry_va, std::uint64_t arg, std::size_t stack_size, std::uint64_t *id) {
    guest_process *p = g_process;
    if (!p) return 78; /* ENOSYS */
    auto t = std::make_unique<guest_thread>();
    t->stack.resize(std::max<std::size_t>(stack_size, 512 * 1024) / 8);
    t->ctx.x[31] = reinterpret_cast<std::uintptr_t>(t->stack.data() + t->stack.size() - 64) & ~std::uintptr_t{15};
    t->ctx.x[30] = 0;
    t->ctx.x[0] = arg;
    guest_thread *raw = t.get();
    {
        std::lock_guard lock(p->threads_mu_);
        raw->id = p->next_thread_id_++;
        p->threads_.push_back(std::move(t));
    }
    *id = raw->id;
    raw->host = std::thread([p, raw, entry_va] {
        const mm_runtime_status st = p->run_thread(*raw, entry_va);
        const auto result = raw->ctx.x[0];
        if (st != mm_runtime_status::ok)
            p->report(*raw, st);
        auto *outer = g_current;
        g_current = raw;
        for (auto thread_exit : p->thread_exit_hooks_) thread_exit();
        g_current = outer;
        std::lock_guard lock(p->threads_mu_);
        raw->status = st;
        raw->result = result;
        raw->finished = true;
    });
    return 0;
}

int guest_process::api_thread_join(std::uint64_t id, std::uint64_t *result) {
    guest_process *p = g_process;
    if (!p) return 78;
    guest_thread *t = nullptr;
    {
        std::lock_guard lock(p->threads_mu_);
        for (auto &th : p->threads_)
            if (th->id == id) t = th.get();
    }
    if (!t || !t->host.joinable()) return 3; /* ESRCH */
    t->host.join();
    if (result) *result = t->result;
    return 0;
}

std::uint64_t guest_process::api_signal_install(int signum, std::uint64_t handler_va, std::uint32_t flags, std::uint32_t mask) {
    guest_process *p = g_process;
    if (!p || signum < 1 || signum > 32) return 0;
    std::lock_guard lock(p->signals_mu_);
    const std::uint64_t old = p->signals_[signum].handler;
    p->signals_[signum] = {handler_va, flags, mask};
    return old;
}

std::uint64_t guest_process::deliver_signal(guest_thread &t, int signum, int code, std::uint64_t fault_addr) {
    guest_signal sig;
    {
        std::lock_guard lock(signals_mu_);
        sig = signals_[signum];
    }
    if (sig.handler <= 1)
        return 0; /* SIG_DFL / SIG_IGN: fatal for these synchronous signals */

    guest_thread::signal_frame frame;
    frame.saved = t.ctx;
    frame.info = std::make_unique<darwin_siginfo>();
    frame.uc = std::make_unique<darwin_ucontext>();
    *frame.info = {};
    frame.info->signo = signum;
    frame.info->code = code;
    frame.info->addr = fault_addr;
    *frame.uc = {};
    frame.uc->uc_stack = {reinterpret_cast<std::uintptr_t>(t.stack.data()), t.stack.size() * 8, 0, 0};
    if (t.ctx.native_mode) {
        frame.native_mc = std::make_unique<darwin_x86_mcontext>();
        auto &mc = *frame.native_mc;
        mc.es = {static_cast<std::uint16_t>(t.ctx.native_trapno), 0, t.ctx.native_error, fault_addr};
        for (int i = 0; i < 16; ++i) mc.ss.registers[i] = t.ctx.native_x[darwin_x86_registers[i]];
        mc.ss.rip = t.ctx.fault_va;
        mc.ss.rflags = t.ctx.native_flags;
        mc.ss.cs = 0x33;
        std::memcpy(mc.fs.fx, t.ctx.native_fp, sizeof mc.fs.fx);
        /* Linux saves 64-bit x87 instruction/data pointers; Darwin uses
         * 32-bit offsets followed by selectors, as does legacy FXSAVE. */
        std::memset(mc.fs.fx + 12, 0, 4);
        std::memset(mc.fs.fx + 20, 0, 4);
        frame.uc->uc_mcsize = sizeof mc;
        frame.uc->uc_mcontext = reinterpret_cast<std::uintptr_t>(&mc);
    } else {
        frame.mc = std::make_unique<darwin_mcontext>();
        frame.mc->far = fault_addr;
        for (int i = 0; i < 29; i++) frame.mc->x[i] = t.ctx.x[i];
        frame.mc->fp = t.ctx.x[29];
        frame.mc->lr = t.ctx.x[30];
        frame.mc->sp = t.ctx.x[31];
        frame.mc->pc = t.ctx.fault_va;
        std::memcpy(frame.mc->v, t.ctx.v, sizeof frame.mc->v);
        frame.mc->fpcr = static_cast<std::uint32_t>(t.ctx.fpcr);
        frame.uc->uc_mcsize = sizeof(darwin_mcontext);
        frame.uc->uc_mcontext = reinterpret_cast<std::uintptr_t>(frame.mc.get());
    }

    /* Handler runs on the same guest stack below a red zone; returning
     * from it lands on the sigreturn trap. */
    t.ctx.x[31] = (t.ctx.x[31] - 1024) & ~std::uint64_t{15};
    t.ctx.x[0] = static_cast<std::uint64_t>(signum);
    t.ctx.x[1] = reinterpret_cast<std::uintptr_t>(frame.info.get());
    t.ctx.x[2] = reinterpret_cast<std::uintptr_t>(frame.uc.get());
    t.ctx.x[30] = sigreturn_va_;
    if (t.ctx.native_mode) {
        const auto sp = ((t.ctx.native_x[MM_RSP] - 1024) & ~std::uint64_t{15}) - 8;
        const auto begin = reinterpret_cast<std::uintptr_t>(t.stack.data());
        if (sp >= begin && sp < begin + t.stack.size() * 8)
            std::memcpy(reinterpret_cast<void *>(sp), &sigreturn_va_, 8);
#if !defined(_WIN32)
        else if (!guarded_copy(reinterpret_cast<void *>(sp), &sigreturn_va_, 8)) { t.ctx = frame.saved; return 0; }
#endif
        t.ctx.native_x[MM_RSP] = sp;
        t.ctx.native_x[MM_RDI] = t.ctx.x[0];
        t.ctx.native_x[MM_RSI] = t.ctx.x[1];
        t.ctx.native_x[MM_RDX] = t.ctx.x[2];
    }
    t.signal_frames.push_back(std::move(frame));
    return sig.handler;
}

bool guest_process::sigreturn(guest_thread &t) {
    if (t.signal_frames.empty())
        return false;
    guest_thread::signal_frame frame = std::move(t.signal_frames.back());
    t.signal_frames.pop_back();
    /* A handler that returns normally resumes at the PC it may have
     * edited in the mcontext (the fault instruction by default). */
    mm_cpu_context resumed = frame.saved;
    std::uint64_t resume_pc;
    if (frame.native_mc) {
        const auto &mc = *frame.native_mc;
        for (int i = 0; i < 16; ++i) resumed.native_x[darwin_x86_registers[i]] = mc.ss.registers[i];
        resumed.native_flags = mc.ss.rflags;
        std::memcpy(resumed.native_fp, mc.fs.fx, sizeof resumed.native_fp);
        std::memcpy(resumed.native_v, mc.fs.fx + 160, sizeof resumed.native_v);
        resume_pc = mc.ss.rip;
    } else {
        for (int i = 0; i < 29; i++) resumed.x[i] = frame.mc->x[i];
        resumed.x[29] = frame.mc->fp;
        resumed.x[30] = frame.mc->lr;
        resumed.x[31] = frame.mc->sp;
        std::memcpy(resumed.v, frame.mc->v, sizeof resumed.v);
        resumed.fpcr = frame.mc->fpcr;
        resume_pc = frame.mc->pc;
    }
    /* A handler can repair memory or a register without changing the PC. */
    resumed.branch_table = t.ctx.branch_table;
    t.ctx = resumed;
    t.ctx.entry_va = resume_pc;
    return true;
}

std::uint64_t guest_process::api_call_guest(std::uint64_t fn_va, const std::uint64_t *args, int nargs) {
    guest_process *p = g_process;
    guest_thread *t = g_current;
    if (!p || nargs < 0 || nargs > 32 || (nargs && !args)) return 0;
    p->leave_guest_boundary(); /* Capture host API changes before its guest callback. */
    const bool host_callback = !t;
    if (host_callback) {
        /* Framework workers (dispatch queues, display links, etc.) are
         * native threads. Give each a persistent guest identity and stack
         * before its first callback, just as pthread_create does. A nested
         * callback reuses g_current; a later work item on this native worker
         * reuses this binding, preserving thread identity and native TLS. */
        static thread_local guest_process *callback_owner = nullptr;
        static thread_local guest_thread *callback_thread = nullptr;
        if (callback_owner != p) {
            auto created = std::make_unique<guest_thread>();
            created->stack.resize(512 * 1024 / 8);
            created->ctx.x[31] = reinterpret_cast<std::uintptr_t>(created->stack.data() + created->stack.size() - 64)
                & ~std::uintptr_t{15};
            std::lock_guard lock(p->threads_mu_);
            created->id = p->next_thread_id_++;
            callback_thread = created.get();
            p->threads_.push_back(std::move(created));
            callback_owner = p;
        }
        t = callback_thread;
        std::lock_guard lock(p->threads_mu_);
        t->finished = false;
    }
    const mm_cpu_context saved = t->ctx;
    for (int i = 0; i < 8; i++) t->ctx.x[i] = i < nargs ? args[i] : 0;
    /* An Intel aggregate adapter temporarily uses x[31] for flattened
     * host arguments, which may live in a small heap vector. Nested guest
     * callbacks must keep using the real Intel stack, below its red zone. */
    const auto callback_sp = saved.native_mode ? saved.native_x[MM_RSP] : saved.x[31];
    t->ctx.x[31] = (callback_sp - 256) & ~std::uint64_t{15};
    if (nargs > 8) {
        const bool native = p->main_->img.header->cputype == MM_CPU_TYPE_X86_64;
        // Native run_thread installs words 6 and 7 at SP and SP+8.
        // ARM's first stacked argument is word 8 at SP itself.
        std::memcpy(reinterpret_cast<void *>(t->ctx.x[31] + (native ? 16 : 0)),
                    args + 8, static_cast<std::size_t>(nargs - 8) * 8);
    }
    t->ctx.x[30] = 0;
    t->ctx.resume_va = 0;
    t->ctx.host_tail_fn = 0;
    const mm_runtime_status st = p->run_thread(*t, fn_va);
    const std::uint64_t result = t->ctx.x[0];
    if (st != mm_runtime_status::ok && t->prepared_execution) {
        t->callback_failure = {st, 0, t->ctx.fault_va, t->ctx.fault_addr};
    } else if (st != mm_runtime_status::ok) {
        p->report(*t, st);
        std::fprintf(stderr, "run: guest callback 0x%llx failed inside a host function\n", static_cast<unsigned long long>(fn_va));
        std::abort();
    }
    /* Memory effects stay; registers (callee-saved and otherwise) are restored for the host caller's guest frame. */
    std::uint64_t vectors[2][2];
    std::memcpy(vectors, t->ctx.v, sizeof vectors);
    const auto native_rdx = t->ctx.native_x[MM_RDX];
    t->ctx = saved;
    t->ctx.x[0] = result;
    std::memcpy(t->ctx.v, vectors, sizeof vectors);
    if (t->ctx.native_mode) t->ctx.native_x[MM_RDX] = native_rdx;
    if (host_callback) {
        std::lock_guard lock(p->threads_mu_);
        t->status = st;
        t->result = result;
        t->finished = true;
    }
    return result;
}

int guest_process::api_prepare_guest_callback(std::uint64_t fn_va, std::size_t stack_size, void **out) {
    if (!out) return 22;
    *out = nullptr;
    guest_process *p = g_process;
    if (!p) return 78;
    if (g_current || (g_callback_binding && g_callback_binding->active)) return 16;
    if (!api_is_guest_code(fn_va) || !mm_runtime_code::translation_of(fn_va) || stack_size > 8 * 1024 * 1024) return 22;
    stack_size = std::max<std::size_t>(stack_size, 512 * 1024);
    if (g_callback_binding && (g_callback_binding->process != p || g_callback_binding->thread->stack.size() * 8 < stack_size)) return 22;
    if (!g_callback_binding) {
        auto thread = std::make_unique<guest_thread>();
        thread->stack.resize((stack_size + 7) / 8); // zero initialization touches stack pages before activation
        thread->ctx.x[31] = reinterpret_cast<std::uintptr_t>(thread->stack.data() + thread->stack.size() - 64) & ~std::uintptr_t{15};
        thread->prepared_execution = true;
        thread->finished = true;
        auto binding = std::make_unique<callback_binding>();
        binding->process = p; binding->thread = thread.get();
        for (const auto &image : p->linker_.images()) binding->codes.push_back(image->runtime.get());
        {
            std::lock_guard lock(p->threads_mu_);
            thread->id = p->next_thread_id_++;
            p->threads_.push_back(std::move(thread));
        }
        g_callback_binding = binding.release();
    }
    auto handle = std::make_unique<prepared_callback>();
    handle->binding = g_callback_binding; handle->function = fn_va;
    ++g_callback_binding->handles;
    *out = handle.release();
    return 0;
}

int guest_process::api_invoke_guest_callback(void *opaque, const std::uint64_t *args, int nargs, mm_callback_result *out) {
    if (!out) return 22;
    *out = {};
    if (!opaque || nargs < 0 || nargs > 8 || (nargs && !args)) return 22;
    auto *handle = static_cast<prepared_callback *>(opaque);
    auto *binding = handle->binding;
    if (binding->owner != std::this_thread::get_id()) return 1;
    if (binding != g_callback_binding || binding->process != g_process) return 22;
    if (binding->active || g_current) return 16;
    binding->active = true;
    auto &t = *binding->thread;
    const auto saved = t.ctx;
    const auto native_mxcsr = _mm_getcsr();
    const auto old_codes = g_callback_fault_codes;
    g_callback_fault_codes = binding->codes;
    t.callback_failure = {};
    t.finished.store(false, std::memory_order_release);
    for (int i = 0; i < 8; ++i) t.ctx.x[i] = i < nargs ? args[i] : 0;
    t.ctx.x[30] = t.ctx.resume_va = t.ctx.host_tail_fn = t.ctx.fault_va = t.ctx.fault_addr = 0;
    t.ctx.x[31] = (saved.x[31] - 256) & ~std::uint64_t{15};
    const auto status = binding->process->run_thread(t, handle->function);
    *out = {status, status == mm_runtime_status::ok ? t.ctx.x[0] : 0, t.ctx.fault_va, t.ctx.fault_addr};
    t.ctx = saved;
    t.finished.store(true, std::memory_order_release);
    g_callback_fault_codes = old_codes;
    _mm_setcsr(native_mxcsr);
    binding->active = false;
    return 0;
}

int guest_process::api_release_guest_callback(void *opaque) {
    if (!opaque) return 22;
    auto *handle = static_cast<prepared_callback *>(opaque);
    auto *binding = handle->binding;
    if (binding->owner != std::this_thread::get_id()) return 1;
    if (binding != g_callback_binding || binding->process != g_process) return 22;
    if (binding->active || g_current) return 16;
    delete handle;
    if (--binding->handles == 0) {
        {
            std::lock_guard lock(binding->process->threads_mu_);
            auto &threads = binding->process->threads_;
            threads.erase(std::remove_if(threads.begin(), threads.end(), [&](const auto &t) { return t.get() == binding->thread; }), threads.end());
        }
        g_callback_binding = nullptr;
        delete binding; // handler lifecycle and stack teardown are outside invocation
    }
    return 0;
}

const char *guest_process::api_unresolved_import_name(std::uint64_t va) {
    guest_process *p = g_process;
    if (!p) return nullptr;
    const std::ptrdiff_t i = p->traps_.index_of(va);
    if (i < 0 || static_cast<std::size_t>(i) >= p->imports_.size()) return nullptr;
    auto &imp = p->imports_[static_cast<std::size_t>(i)];
    if (imp.name_storage.empty()) imp.name_storage = std::string(imp.bind.symbol);
    return imp.name_storage.c_str();
}

void guest_process::api_atexit_register(std::uint64_t fn_va, std::uint64_t arg) {
    if (guest_process *p = g_process) { std::lock_guard lock(p->atexit_mu_); p->atexit_.push_back({fn_va, arg}); }
}

void guest_process::api_atexit_run() {
    guest_process *p = g_process;
    if (!p || !g_current) return;
    for (;;) {
        std::pair<std::uint64_t, std::uint64_t> h;
        {
            std::lock_guard lock(p->atexit_mu_);
            if (p->atexit_.empty()) return;
            h = p->atexit_.back();
            p->atexit_.pop_back();
        }
        if (trace_enabled()) std::fprintf(stderr, "atexit: 0x%llx\n", static_cast<unsigned long long>(h.first));
        api_call_guest(h.first, &h.second, 1);
    }
}

bool guest_process::api_is_guest_code(std::uint64_t va) {
    if (g_process && g_process->main_->img.header->cputype == MM_CPU_TYPE_X86_64) {
        const auto *image = g_process->linker_.image_for(va);
        return image && va >= image->code.addr && va - image->code.addr < image->code.bytes.size();
    }
    return mm_runtime_code::translation_of(va) != 0;
}

int guest_process::api_copy_guest_memory(void *destination, const void *source, std::size_t size) {
#if !defined(_WIN32)
    fault_handler_scope handlers;
    return guarded_copy(destination, source, size) ? 0 : -1;
#else
    (void)destination; (void)source; (void)size;
    return -1;
#endif
}

std::uint64_t guest_process::api_lookup_symbol(const char *name) {
    auto *process = g_process;
    if (!process || !name) return 0;
    const std::string darwin = std::string("_") + name;
    std::uint64_t guest = 0;
    void *host = nullptr;
    process->linker_.lookup(*process->main_, -2, darwin, false, guest, host);
    const auto original = guest ? guest : reinterpret_cast<std::uintptr_t>(host);
    for (const auto &interpose : process->interpositions_)
        if (interpose.original == original) return interpose.replacement;
    if (guest) return guest;
    if (!host) return 0;
    if (process->main_->img.header->cputype != MM_CPU_TYPE_X86_64 || !host_callable(host, false))
        return reinterpret_cast<std::uintptr_t>(host);
    if (const auto found = process->import_by_host_.find(host); found != process->import_by_host_.end())
        return process->traps_.va(found->second);
    if (process->imports_.size() == process->imports_.capacity()) return 0;
    bound_import imported{};
    imported.image = process->main_;
    imported.host = host;
    imported.name_storage = darwin;
    process->imports_.push_back(std::move(imported));
    const auto index = process->imports_.size() - 1;
    process->imports_.back().bind.symbol = process->imports_.back().name_storage;
    process->import_by_host_.emplace(host, index);
    return process->traps_.va(index);
}

std::size_t guest_process::api_image_count() { return g_process ? g_process->linker_.images().size() : 0; }

const void *guest_process::api_image_header(std::size_t i) {
    guest_process *p = g_process;
    if (!p || i >= p->linker_.images().size()) return nullptr;
    return reinterpret_cast<const void *>(p->linker_.images()[i]->header_va());
}

std::int64_t guest_process::api_image_slide(std::size_t i) {
    guest_process *p = g_process;
    if (!p || i >= p->linker_.images().size()) return 0;
    return static_cast<std::int64_t>(p->linker_.images()[i]->slide);
}

const char *guest_process::api_image_path(std::size_t i) {
    guest_process *p = g_process;
    if (!p || i >= p->linker_.images().size()) return nullptr;
    return p->linker_.images()[i]->path.c_str();
}

void guest_process::dump_threads(std::uintptr_t interrupted_rip) {
    std::fprintf(stderr, "\n--- guest threads ---\n");
#if !defined(_WIN32)
    if (diagnostics_enabled()) {
        void *frames[48];
        const int count = backtrace(frames, 48);
        std::fprintf(stderr, "--- interrupted native stack ---\n");
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        std::fprintf(stderr, "--- end native stack ---\n");
    }
#endif
    std::lock_guard lock(threads_mu_);
    for (const auto &t : threads_) {
        std::fprintf(stderr, "thread %llu%s:\n", static_cast<unsigned long long>(t->id),
                     t->finished ? " (finished)" : "");
        if (t->finished) continue;
        /* A running thread is either inside translated code or inside a
         * host call; in both cases x30 is the guest address to resume at,
         * which is close enough to name where it is. */
        mm_cpu_context ctx = t->ctx;
        ctx.fault_va = ctx.x[30];
#if !defined(_WIN32)
        if (interrupted_rip && t.get() == g_current) {
            const auto pc = mm_runtime_code::guest_pc_anywhere(interrupted_rip);
            if (pc) ctx.fault_va = pc;
            Dl_info info{};
            dladdr(reinterpret_cast<const void *>(interrupted_rip), &info);
            std::fprintf(stderr, "  interrupted host PC=0x%llx %s+0x%llx; guest PC=0x%llx; guest LR=0x%llx\n",
                static_cast<unsigned long long>(interrupted_rip), info.dli_sname ? info.dli_sname : "(translated or unknown)",
                static_cast<unsigned long long>(info.dli_saddr ? interrupted_rip - reinterpret_cast<std::uintptr_t>(info.dli_saddr) : 0),
                static_cast<unsigned long long>(pc), static_cast<unsigned long long>(ctx.x[30]));
            if (diagnostics_enabled()) {
                for (int i = 0; i < 32; i += 4)
                    std::fprintf(stderr, "  x%d=%llx x%d=%llx x%d=%llx x%d=%llx\n", i,
                        static_cast<unsigned long long>(ctx.x[i]), i + 1, static_cast<unsigned long long>(ctx.x[i + 1]),
                        i + 2, static_cast<unsigned long long>(ctx.x[i + 2]), i + 3, static_cast<unsigned long long>(ctx.x[i + 3]));
                for (auto *frame = t->host_calls; frame; frame = frame->outer) {
                    Dl_info function{};
                    dladdr(frame->function, &function);
                    std::fprintf(stderr, "  active host call %s (%p) from guest 0x%llx args={%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx}\n",
                        function.dli_sname ? function.dli_sname : "(unknown IMP)", frame->function,
                        static_cast<unsigned long long>(frame->caller),
                        static_cast<unsigned long long>(frame->args[0]), static_cast<unsigned long long>(frame->args[1]),
                        static_cast<unsigned long long>(frame->args[2]), static_cast<unsigned long long>(frame->args[3]),
                        static_cast<unsigned long long>(frame->args[4]), static_cast<unsigned long long>(frame->args[5]),
                        static_cast<unsigned long long>(frame->args[6]), static_cast<unsigned long long>(frame->args[7]));
                }
            }
        }
#else
        (void)interrupted_rip;
#endif
        print_backtrace(linker_, ctx);
    }
    std::fprintf(stderr, "--- end of guest threads ---\n");
}

void guest_process::api_print_guest_backtrace() {
    guest_process *p = g_process;
    guest_thread *t = g_current;
    if (!p || !t) return;
    /* The guest is inside a host call, so its PC is the return address. */
    mm_cpu_context ctx = t->ctx;
    ctx.fault_va = ctx.x[30];
    print_backtrace(p->linker_, ctx);
}

std::ptrdiff_t guest_process::api_image_for_address(std::uint64_t va) {
    guest_process *p = g_process;
    if (!p) return -1;
    const auto &images = p->linker_.images();
    for (std::size_t i = 0; i < images.size(); i++)
        if (images[i]->contains(va)) return static_cast<std::ptrdiff_t>(i);
    return -1;
}

std::uint64_t guest_process::api_thread_self() { return g_current ? g_current->id : 0; }

std::uint64_t guest_process::api_fpcr_get() { return g_current ? g_current->ctx.fpcr : 0; }

void guest_process::api_fpcr_set(std::uint64_t fpcr) {
    if (!g_current) return;
    mm_cpu_context &ctx = g_current->ctx;
    ctx.fpcr = fpcr;
    /* Same mapping as the translator's `msr fpcr`: RMode -> RC, FZ -> FTZ|DAZ. */
    const unsigned rmode = static_cast<unsigned>((fpcr >> 22) & 3);
    const unsigned rc = ((rmode & 1) << 1) | (rmode >> 1);
    std::uint32_t mxcsr = 0x1F80 | (rc << 13);
    if (fpcr & (1u << 24)) mxcsr |= (1u << 15) | (1u << 6);
    ctx.mxcsr = mxcsr;
    _mm_setcsr(mxcsr);
}

void guest_process::api_thread_stack(std::uint64_t *base, std::uint64_t *size) {
    if (!g_current) { *base = 0; *size = 0; return; }
    *size = g_current->stack.size() * 8;
    *base = reinterpret_cast<std::uintptr_t>(g_current->stack.data()) + *size;
}

/* Configure the initial guest environment without changing host libc. */
bool prepare_environment(const mm_launch_request &request, mm_environment &environment) {
    if (request.apply_venv_env) for (const auto &[name, value] : request.venv->env) {
        const int error=value ? environment.set(name.c_str(), value->c_str(), 1) : environment.unset(name.c_str());
        if (error) return false;
    }
    for (char *entry : request.env) {
        if (!entry) continue;
        const std::string_view text(entry);
        const auto separator=text.find('=');
        const std::string name(text.substr(0, separator));
        const int error=separator==std::string_view::npos ? environment.unset(name.c_str())
            : environment.set(name.c_str(), entry+separator+1, 1);
        if (error) return false;
    }
    return true;
}

/* Backend options intentionally stay in the host environment, where Wayland,
 * libc and framework workers read them. Only runtime configuration is overlaid;
 * guest HOME/PATH/etc remain isolated. Restore overlays on every return path. */
struct host_configuration_scope {
    std::vector<std::pair<std::string, std::optional<std::string>>> saved;
    void set(const std::string &name, const char *value) {
#if !defined(_WIN32)
        if (std::none_of(saved.begin(), saved.end(), [&](const auto &entry) { return entry.first==name; })) {
            const char *previous=std::getenv(name.c_str());
            saved.emplace_back(name, previous ? std::optional<std::string>(previous) : std::nullopt);
        }
        if (value) ::setenv(name.c_str(), value, 1); else ::unsetenv(name.c_str());
#else
        (void)name; (void)value;
#endif
    }
    host_configuration_scope(const mm_launch_request &request, mm_environment &environment) {
        auto apply=[&](std::string_view name) {
            if (name.starts_with("MACHOMAN_") || name=="DYLD_PRINT_LIBRARIES") {
                const std::string key(name); set(key, environment.get(key.c_str()));
            }
        };
        if (request.apply_venv_env) for (const auto &[name, value] : request.venv->env) { (void)value; apply(name); }
        for (const char *entry : request.env) if (entry) {
            const std::string_view text(entry); apply(text.substr(0, text.find('=')));
        }
    }
    ~host_configuration_scope() {
#if !defined(_WIN32)
        for (auto it=saved.rbegin(); it!=saved.rend(); ++it) {
            if (it->second) ::setenv(it->first.c_str(), it->second->c_str(), 1);
            else ::unsetenv(it->first.c_str());
        }
#endif
    }
};
} // namespace

mm_runtime_status mm_runtime_audit(const mm_launch_request &request, mm_import_audit &report) {
    report = {};
    if (!request.venv) return mm_runtime_status::bad_image;
    mm_environment environment(environ);
    if (!prepare_environment(request, environment)) return mm_runtime_status::bad_image;
    host_configuration_scope configuration(request, environment);
    std::string error;
    const auto path = mm_venv_resolve_executable(*request.venv, request.executable_path, error);
    if (!error.empty()) {
        std::fprintf(stderr, "audit: %s\n", error.c_str());
        return mm_runtime_status::bad_image;
    }
    auto data = read_whole_file(path);
    mm_macho_image image;
    if (data.empty() || mm_macho_parse(data, image) != mm_macho_status::ok) return mm_runtime_status::bad_image;
    if (image.header->cputype != MM_CPU_TYPE_ARM64 && image.header->cputype != MM_CPU_TYPE_X86_64)
        return mm_runtime_status::unsupported_arch;
    linker ld(*request.venv, path.parent_path(), environment);
    if (!ld.load_executable(path, std::move(data), image)) return mm_runtime_status::map_failed;
    report.guest_images = ld.images().size();
    report.host_libraries = ld.host_libs().size();
    for (auto &loaded : ld.images()) {
        for (std::size_t i = 0; i < loaded->img.dylibs.size(); ++i) {
            const auto &dep = loaded->deps[i];
            if (!dep.guest && (!dep.host || !dep.host->handle))
                report.missing_dependencies.push_back({loaded->path.string(), std::string(loaded->img.dylibs[i].name), loaded->img.dylibs[i].weak()});
        }
        for (const auto &bind : loaded->img.binds()) {
            mm_import_audit_entry entry{.image = loaded->path.string(), .symbol = std::string(bind.symbol),
                                        .resolution = mm_import_resolution::missing, .lazy = bind.lazy, .weak = bind.weak};
            if (bind.dylib_ordinal >= 1 && static_cast<std::size_t>(bind.dylib_ordinal) <= loaded->img.dylibs.size())
                entry.dependency = loaded->img.dylibs[static_cast<std::size_t>(bind.dylib_ordinal) - 1].name;
            else entry.dependency = "ordinal:" + std::to_string(bind.dylib_ordinal);
            std::uint64_t guest;
            void *host;
            bool resolver;
            if (bind.weak_table) {
                entry.resolution = mm_import_resolution::own;
                entry.provider = entry.image;
            } else {
                ld.lookup(*loaded, bind.dylib_ordinal, bind.symbol, bind.lazy, guest, host, &resolver);
                if (guest) {
                    entry.resolution = mm_import_resolution::guest;
                    if (const auto *provider = ld.image_for(guest)) entry.provider = provider->path.string();
                } else if (host) {
                    entry.resolution = resolver ? mm_import_resolution::resolver : mm_import_resolution::host;
#if !defined(_WIN32)
                    Dl_info info{};
                    if (dladdr(host, &info) && info.dli_fname) entry.provider = info.dli_fname;
#endif
                    if (entry.provider.empty() && resolver) entry.provider = entry.dependency;
                } else if (bind.weak) entry.resolution = mm_import_resolution::weak_missing;
            }
            report.imports.push_back(std::move(entry));
        }
    }
    return mm_runtime_status::ok;
}

mm_runtime_status mm_runtime_launch(const mm_launch_request &request) {
    if (!request.venv) return mm_runtime_status::bad_image;
    mm_environment environment(environ);
    if (!prepare_environment(request, environment)) return mm_runtime_status::bad_image;
    host_configuration_scope configuration(request, environment);
    std::string launcher;
#if !defined(_WIN32)
    /* Host backends retain their scoped launch context; guest-facing launch
     * APIs receive immutable paths in mm_host_api, independently of envp. */
    {
        std::error_code ec;
        const auto venv_root = std::filesystem::absolute(request.venv->root);
        configuration.set("MACHOMAN_VENV", venv_root.c_str());
        const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) { launcher=self.string(); configuration.set("MACHOMAN_EXE", self.c_str()); }
        if (request.initial_signal_mask) {
            const auto mask=std::to_string(*request.initial_signal_mask);
            configuration.set("MACHOMAN_INITIAL_SIGNAL_MASK", mask.c_str());
        }
    }
#endif
    /* Resolved, because a binary in the venv's bin/ is often a symlink into
     * an app bundle and everything the guest computes from its own path
     * (its bundle, @executable_path, an embedded interpreter's home) has to
     * land inside that bundle, as it would on macOS. */
    std::string launcher_error;
    std::filesystem::path exe_path = mm_venv_resolve_executable(*request.venv, request.executable_path, launcher_error);
    if (!launcher_error.empty()) {
        std::fprintf(stderr, "run: %s\n", launcher_error.c_str());
        return mm_runtime_status::bad_image;
    }
    std::vector<std::uint8_t> data = read_whole_file(exe_path);
    if (data.empty())
        return mm_runtime_status::bad_image;

    mm_macho_image img;
    const auto parsed = mm_macho_parse(data, img);
    if (parsed == mm_macho_status::no_arm64_slice) {
        std::fprintf(stderr, "run: %s has no arm64 slice; machoman requires an arm64 macOS executable\n", exe_path.c_str());
        return mm_runtime_status::unsupported_arch;
    }
    if (parsed != mm_macho_status::ok)
        return mm_runtime_status::bad_image;
    if (img.header->cputype != MM_CPU_TYPE_ARM64 && img.header->cputype != MM_CPU_TYPE_X86_64) {
        std::fprintf(stderr, "run: %s is %s; machoman requires an arm64 macOS executable\n",
                     exe_path.c_str(), mm_cpu_type_name(img.header->cputype));
        return mm_runtime_status::unsupported_arch;
    }
    if (!img.has_entry)
        return mm_runtime_status::bad_image;

    guest_process process(*request.venv, request.argv, exe_path, environment, std::move(launcher));
    if (const auto st = process.load(std::move(data), img); st != mm_runtime_status::ok)
        return st;
    return process.run_main();
}

const char *mm_runtime_status_str(mm_runtime_status status) {
    switch (status) {
    case mm_runtime_status::ok:                return "ok";
    case mm_runtime_status::bad_image:         return "image is malformed or has no usable entry point";
    case mm_runtime_status::unresolved_dylib:  return "a required dylib has no host-side implementation";
    case mm_runtime_status::exec_alloc_failed: return "failed to allocate executable memory";
    case mm_runtime_status::map_failed:        return "could not map a segment at its preferred vmaddr";
    case mm_runtime_status::unsupported_insn:  return "stopped at an instruction with no translation";
    case mm_runtime_status::bad_target:        return "control left the translated code";
    case mm_runtime_status::syscall:           return "stopped at a Darwin syscall (dispatch not implemented)";
    case mm_runtime_status::brk:               return "guest executed brk";
    case mm_runtime_status::memory_fault:      return "guest accessed unmapped memory";
    case mm_runtime_status::unresolved_import: return "guest used an import no host dylib provides";
    case mm_runtime_status::exited:            return "guest called exit()";
    case mm_runtime_status::host_abort:        return "a host dylib aborted the guest";
    case mm_runtime_status::unsupported_arch:  return "unsupported guest architecture (arm64 required)";
    }
    return "unknown status";
}
