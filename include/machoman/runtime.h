/* Layer 3: the runtime loader. Given a venv (layer 1) and a translated
 * executable image produced by layer 2, this is responsible for:
 *   - mapping the translated code + data segments into host memory
 *   - resolving the fixed Darwin dylib names the binary imports against
 *     the venv's usr/lib stubs (see venv.h)
 *   - servicing Darwin syscalls the translated code issues, by trapping
 *     them and dispatching to a table of host (Win32/Linux) equivalents
 *   - transferring control to the translated entry point
 *
 * Today only the last leg exists, and only for straight-line leaf code:
 * mm_runtime_exec allocates executable memory, enters the translated code
 * through a trampoline that sets up the guest register file, and returns
 * when the guest executes RET. */
#ifndef MACHOMAN_RUNTIME_H
#define MACHOMAN_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "machoman/macho.h"
#include "machoman/translator.h"
#include "machoman/venv.h"

enum class mm_runtime_status {
    ok = 0,                 /* guest returned to the host sentinel (x30 == 0) */
    bad_image,
    unresolved_dylib,
    exec_alloc_failed,
    map_failed,             /* could not map a segment at its vmaddr */
    unsupported_insn,       /* stopped at an untranslated instruction; see ctx.fault_va */
    bad_target,             /* branch left the translated text; see ctx.fault_va */
    syscall,                /* svc reached; ctx.x[16] is the Darwin syscall number */
    brk,
    memory_fault,           /* guest touched unmapped memory; ctx.fault_va / ctx.fault_addr */
    unresolved_import,      /* guest called an import with no host implementation */
    exited,                 /* guest called exit(); ctx.exit_code */
    host_abort,             /* a host function aborted the guest (see stderr) */
    unsupported_arch,       /* image has no arm64 code to translate */
};

/* Dynamic linking. An install name such as "/usr/lib/libc++.1.dylib" is
 * looked up under the venv root (then each entry of $MACHOMAN_DYLIB_PATH,
 * then <machoman>/dylibs) as "usr/lib/libc++.1.dylib.so" -- a host shared
 * object standing in for the Darwin dylib -- or, failing that, as
 * "usr/lib/libc++.1.dylib" -- a real arm64 Mach-O dylib that is loaded,
 * rebased, bound and translated like the executable. "@rpath/x" expands
 * through the referring image's LC_RPATH entries; @executable_path and
 * @loader_path resolve against the real directories of the executable and
 * the referring image, so an app bundle's Frameworks/ works as on macOS.
 *
 * Guest images resolve symbols through their export tries (following
 * re-exports and the dependency chain, since umbrella libraries re-export
 * their sub-libraries). Host libraries are looked up with dlsym twice:
 * first by the Darwin name verbatim (e.g. "_open", which is how a host
 * dylib exports an implementation with Darwin ABI/semantics), then with the
 * leading underscore dropped ("open"), which falls through the .so's
 * dependency chain to the host libc for everything that is ABI-compatible.
 * A "$DARWIN_EXTSN"-style suffix is dropped before both lookups.
 *
 * A host library may also export
 * `void *machoman_resolve_symbol(const char *name, unsigned flags)` to
 * synthesise symbols it has no static definition for (a framework stub
 * deriving a constant or a logging function stub from the name). It is
 * consulted only for binds that name that library, so a stub can never
 * shadow a symbol another library really defines. `flags` carries what the
 * bind record knows about the symbol's use. */
inline constexpr unsigned MM_SYMBOL_LAZY = 1u; /* a lazy bind: the slot is called, so the symbol is a function */

struct mm_callback_result {
    mm_runtime_status status = mm_runtime_status::ok;
    std::uint64_t value = 0, fault_va = 0, fault_addr = 0;
};
/* Passed to a host dylib's `machoman_dylib_init` (if it exports one) so it
 * can reach the guest image, process arguments and the runtime's thread
 * machinery without hardcoding anything about the executable. All
 * function pointers are valid for the process lifetime. */
struct mm_host_api {
    std::uint32_t version;         /* 6; new fields are appended */
    const void   *main_image;      /* mapped Mach-O header of the executable */
    int           argc;
    char        **argv;
    char        **envp;

    /* Runs guest function `entry_va(arg)` on a new host thread with its own
     * guest register file and stack. Returns 0 and the runtime's thread id,
     * or a Darwin errno. */
    int (*thread_create)(std::uint64_t entry_va, std::uint64_t arg, std::size_t stack_size, std::uint64_t *thread_id);
    /* Waits for a guest thread; *result receives its x0 at return. */
    int (*thread_join)(std::uint64_t thread_id, std::uint64_t *result);
    /* Id of the guest thread calling into the host right now. */
    std::uint64_t (*thread_self)();
    /* Guest stack of the calling thread, for pthread_get_stack*_np. */
    void (*thread_stack)(std::uint64_t *base, std::uint64_t *size);
    /* Guest FPCR of the calling thread (fenv.h lives in host code but must
     * affect guest arithmetic; the runtime keeps MXCSR in sync). */
    std::uint64_t (*fpcr_get)();
    void (*fpcr_set)(std::uint64_t fpcr);
    /* Register file of the calling guest thread, for host functions whose
     * semantics are defined on guest registers (setjmp/longjmp, signal
     * contexts). When a host call returns, execution resumes at x[30] with
     * x[0] = the return value, so a host function may redirect the guest
     * by rewriting x[30] and x[31]. */
    mm_cpu_context *(*current_context)();
    /* Guest signal handlers (Darwin numbering). handler_va 0 = SIG_DFL, 1 =
     * SIG_IGN. Returns the previous handler. The runtime delivers SIGILL
     * (untranslated instruction), SIGSEGV/SIGBUS (memory fault) and SIGTRAP
     * (brk) to these with Darwin siginfo/ucontext arguments. */
    std::uint64_t (*signal_install)(int signum, std::uint64_t handler_va, std::uint32_t flags, std::uint32_t mask);
    /* Calls guest function `fn_va` with up to 32 integer words and
     * returns its x0. A guest caller uses a nested run on its own stack;
     * a native framework worker gets a persistent guest thread and stack
     * on its first callback. Words beyond the ISA's argument registers are
     * copied to the guest stack. FP arguments come from the current context;
     * x0, native rdx and v0/v1 carry results, other registers are preserved.
     * Swift-context callbacks inherit swiftself in arm64 x20 or native r13
     * from current_context; adapters must restore it after the call. */
    std::uint64_t (*call_guest)(std::uint64_t fn_va, const std::uint64_t *args, int nargs);
    /* True if `va` is inside the translated code of the executable, i.e.
     * a function pointer that must be called with call_guest rather than
     * as a host function. */
    bool (*is_guest_code)(std::uint64_t va);
    /* If `va` is the trap address of an import no host dylib provides,
     * returns that import's symbol name; otherwise null. */
    const char *(*unresolved_import_name)(std::uint64_t va);
    /* Guest atexit handlers: registered functions are guest code called
     * with `arg` when the guest exits (main returns or exit() is called). */
    void (*atexit_register)(std::uint64_t fn_va, std::uint64_t arg);
    /* Runs the registered handlers (last first) on the calling guest thread. */
    void (*atexit_run)();

    /* Loaded guest Mach-O images (the executable is index 0), for the
     * _dyld_* introspection API. Headers are the mapped, slid images. */
    std::size_t (*image_count)();
    const void *(*image_header)(std::size_t index);
    std::int64_t (*image_slide)(std::size_t index);
    const char *(*image_path)(std::size_t index);
    /* Index of the image whose mapped segments contain `va`, or -1. */
    std::ptrdiff_t (*image_for_address)(std::uint64_t va);
    /* Prints the calling guest thread's symbolised backtrace to stderr.
     * For a host function that is about to report something the guest did
     * (an alert, an unimplemented call), so the log says where it came from. */
    void (*print_guest_backtrace)();

    /* v3: prepare/release on the native processing thread before/after
     * activation. Handles on one native thread share a prefaulted guest
     * stack and identity. Invoke rejects concurrent/reentrant use, returns
     * faults as data, and avoids thread-registration and handler-lifecycle
     * locks. The caller owns handle lifetime and must quiesce before release.
     * Unbound host branch targets are rejected; no dynamic host target
     * discovery occurs here. Guest DSP, dispatchers and host callees require
     * a realtime audit; this API imposes no execution time limit. */
    int (*prepare_guest_callback)(std::uint64_t fn_va, std::size_t stack_size, void **handle);
    int (*invoke_guest_callback)(void *handle, const std::uint64_t *args, int nargs, mm_callback_result *result);
    int (*release_guest_callback)(void *handle);
    /* v4: Darwin dlsym name (without Mach-O's extra underscore). Returns
     * guest code or a callable host bridge for the current architecture. */
    std::uint64_t (*lookup_symbol)(const char *name);
    /* v5: bounded guest-memory copying with fault recovery, for APIs that
     * marshal message buffers or out-of-line memory. Returns 0 or -1. */
    int (*copy_guest_memory)(void *destination, const void *source, std::size_t size);
    /* v6: live process environment. Mutators return 0 or a host errno;
     * putenv borrows the caller's buffer. bind_environment connects exported
     * Darwin environ storage to the same authoritative pointer slot. */
    char ***(*environment)();
    char *(*environment_get)(const char *name);
    int (*environment_set)(const char *name, const char *value, int overwrite);
    int (*environment_unset)(const char *name);
    int (*environment_put)(char *entry);
    void (*bind_environment)(char ***storage);
    /* Launch context is independent of guest-modifiable environment vars. */
    const char *venv_root;
    const char *launcher_path;
    const char *executable_path;
};
using mm_dylib_init_fn = void (*)(const mm_host_api *);
/* Optional machoman_dylib_start hook: all host APIs and guest translations
 * are ready, and callbacks run on the guest main thread before its static
 * initializers. init hooks register metadata without executing guest code. */
using mm_dylib_start_fn = void (*)();
/* Optional machoman_dylib_guest_enter / machoman_dylib_guest_leave hooks
 * publish guest thread state before executing guest code or a host API,
 * and capture it immediately afterwards. They must not call guest code. */
using mm_dylib_boundary_fn = void (*)();
/* Optional machoman_dylib_thread_exit hook: a guest pthread has returned,
 * but still owns its guest identity, stack and TLS. The hook may use
 * call_guest to run thread destructors before host thread teardown. */
using mm_dylib_thread_exit_fn = void (*)();

/* Translated code mapped into executable memory, re-enterable any number
 * of times (each host call-out returns here and resumes via run()). Its
 * instructions are registered in the process-wide code map (see
 * MM_CODE_MAP_* in translator.h) for as long as the object lives, so
 * indirect branches from any other translated image can reach them. */
class mm_runtime_code {
public:
    explicit mm_runtime_code(const mm_translation_result &result);
    ~mm_runtime_code();
    mm_runtime_code(const mm_runtime_code &) = delete;
    mm_runtime_code &operator=(const mm_runtime_code &) = delete;

    bool ok() const { return code_ != nullptr; }

    /* Runs from guest VA `entry_va` until the guest exits; returns the
     * status mirroring ctx.exit_reason. ctx.x[30] == 0 makes a top-level
     * `ret` return to the host. */
    mm_runtime_status run(mm_cpu_context &ctx, std::uint64_t entry_va);

    /* Host rip -> guest VA for fault reporting; 0 if not in this code. */
    std::uint64_t guest_pc(std::uintptr_t host_rip) const;

    /* Host rip -> guest VA across every live mm_runtime_code; 0 if none. */
    static std::uint64_t guest_pc_anywhere(std::uintptr_t host_rip);
    /* Host address of the translation of guest VA `va`, 0 if untranslated. */
    static std::uintptr_t translation_of(std::uint64_t va);

private:
    const mm_translation_result &result_;
    std::uint8_t *mem_ = nullptr;
    std::size_t mem_size_ = 0;
    std::uint8_t *code_ = nullptr;             /* translated instructions start */
    std::vector<std::pair<std::uint64_t, std::uint64_t>> entry_ranges_; /* cold offset -> source offset */
};

/* One entry in the Darwin -> host syscall translation table. `darwin_nr`
 * is the BSD/Darwin syscall number the translated code will trap on
 * (arm64 Darwin syscalls use `svc #0x80` with the number in x16). */
using mm_syscall_handler = long (*)(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                    std::uint64_t a4, std::uint64_t a5, std::uint64_t a6);

struct mm_syscall_entry {
    std::int32_t       darwin_nr;
    const char        *name;
    mm_syscall_handler handler; /* nullptr until implemented -> traps to "unimplemented" */
};

/* Populated incrementally as syscalls are implemented; unimplemented
 * entries keep handler == nullptr deliberately so dispatch can report
 * exactly which syscall blocked a given program from running. */
std::span<const mm_syscall_entry> mm_syscall_table();

/* One-shot convenience over mm_runtime_code for tests: runs from guest VA
 * `entry_va` with the given register file. */
mm_runtime_status mm_runtime_exec(const mm_translation_result &result, std::uint64_t entry_va,
                                  mm_cpu_context &ctx);

struct mm_launch_request {
    const mm_venv   *venv;
    std::string_view executable_path; /* path under venv's bin/ to launch */
    std::span<char *const> argv;      /* argv[0] defaults to the executable path */
    /* Host inheritance -> venv env defaults -> these "KEY=VALUE" overrides.
     * "KEY" alone removes one. The guest owns its live environment; ordinary
     * guest variables do not mutate host libc's environment. */
    std::span<char *const> env;
    /* Nested exec/spawn already supplies the complete environment: do not
     * reapply venv defaults after unsetenv or an explicit empty envp. */
    bool apply_venv_env = true;
    std::optional<std::uint32_t> initial_signal_mask;
};

/* Loads the arm64 image from the venv, maps its segments at their link
 * time vmaddr (the image is PIE but we honour its preferred base so that
 * adrp-derived addresses are valid without relocation), translates
 * __TEXT,__text and runs the entry point until the guest exits. Reports
 * where and why it stopped on stderr. Not a general loader yet: no dylib
 * symbol resolution, no syscall dispatch, no argv/envp ABI setup. */
mm_runtime_status mm_runtime_launch(const mm_launch_request &request);

enum class mm_import_resolution { guest, host, resolver, own, weak_missing, missing };
struct mm_import_audit_entry {
    std::string image, dependency, symbol, provider;
    mm_import_resolution resolution;
    bool lazy = false, weak = false;
};
struct mm_dependency_audit_entry {
    std::string image, dependency;
    bool weak = false;
};
struct mm_import_audit {
    std::size_t guest_images = 0, host_libraries = 0;
    std::vector<mm_import_audit_entry> imports;
    std::vector<mm_dependency_audit_entry> missing_dependencies;
};
/* Loads the dependency closure using the ordinary dyld search/lookup rules.
 * Does not bind slots, translate code, call framework lifecycle hooks, or
 * execute guest initializers/main. Host dlopen constructors still run.
 * Resolver means the named library's dynamic resolver supplied an address;
 * it may be a stub or generated metadata and does not prove API support.
 * A successful audit returns ok even when the report contains missing APIs. */
mm_runtime_status mm_runtime_audit(const mm_launch_request &request, mm_import_audit &report);

const char *mm_runtime_status_str(mm_runtime_status status);

#endif /* MACHOMAN_RUNTIME_H */
